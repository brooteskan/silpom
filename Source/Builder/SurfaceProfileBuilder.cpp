// SPDX-License-Identifier: MIT
#include "SurfaceProfileBuilder.h"
#include <SilPOM/SurfaceProfileAsset.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/IO/SystemFile.h>
#include <AzCore/Serialization/EditContextConstants.inl>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/Utils/Utils.h>
#include <AzCore/std/functional.h>
#include <AzCore/std/string/conversions.h>
#include <AzFramework/Process/ProcessCommunicator.h>
#include <AzFramework/Process/ProcessWatcher.h>
#include <openssl/sha.h>

namespace SilPOM
{
namespace
{
constexpr AZ::u32 ProfileVersion = 1, WeightsVersion = 1;
constexpr const char* JobKey = "SilPOM surface profile";

struct ProfileSource
{
    AZStd::string height, weights;  // absolute, normalized
    AZ::u32 width = 0, heightPixels = 0, addressMode = 0, shaderModel = 0;
    float scale = 0, reference = 0;
    SurfaceRole role = SurfaceRole::Surface;
};

bool ReadProfile(const AZStd::string& path, ProfileSource& out, AZStd::string& error)
{
    auto document = AZ::JsonSerializationUtils::ReadJsonFile(path);
    if (!document.IsSuccess()) { error = document.GetError(); return false; }
    const auto& root = document.GetValue();
    const auto member = [&](const char* name) -> const rapidjson::Value*
    { const auto it = root.IsObject() ? root.FindMember(name) : root.MemberEnd(); return root.IsObject() && it != root.MemberEnd() ? &it->value : nullptr; };
    const auto* version = member("Version");
    const auto* height = member("Height");
    const auto* width = member("Width");
    const auto* pixels = member("HeightPixels");
    const auto* scale = member("Scale");
    const auto* reference = member("Reference");
    const auto* address = member("AddressMode");
    const auto* role = member("Role");
    const auto* model = member("ShaderModel");
    const auto* weights = member("Weights");
    if (!version || !version->IsUint() || version->GetUint() != ProfileVersion || !height || !height->IsString()
        || !width || !width->IsUint() || !pixels || !pixels->IsUint() || !scale || !scale->IsNumber()
        || !reference || !reference->IsNumber() || !address || !address->IsUint() || !role || !role->IsString()
        || !model || !model->IsUint() || (weights && !weights->IsString()))
    { error = "needs Version 1, Height, Width, HeightPixels, Scale, Reference, AddressMode, Role, ShaderModel"; return false; }
    const AZStd::string roleName = role->GetString();
    if (roleName == "Wall") out.role = SurfaceRole::Wall;
    else if (roleName == "Opening") out.role = SurfaceRole::Opening;
    else if (roleName == "Surface") out.role = SurfaceRole::Surface;
    else { error = "Role must be Surface, Wall or Opening"; return false; }
    const AZ::IO::Path folder(AZ::IO::Path(path).ParentPath());
    if (AZ::IO::Path(height->GetString()).IsAbsolute() || (weights && AZ::IO::Path(weights->GetString()).IsAbsolute()))
    { error = "paths must be relative to the profile"; return false; }
    out.height = (folder / height->GetString()).LexicallyNormal().String();
    AZStd::string weightsName;
    if (weights) weightsName = weights->GetString();
    else
    {
        weightsName = AZ::IO::Path(path).Filename().String();
        weightsName = weightsName.substr(0, weightsName.size() - AZStd::string_view(".silpomprofile").size()) + ".silpomweights.json";
    }
    out.weights = (folder / weightsName).LexicallyNormal().String();
    out.width = width->GetUint(); out.heightPixels = pixels->GetUint(); out.addressMode = address->GetUint();
    out.shaderModel = model->GetUint(); out.scale = float(scale->GetDouble()); out.reference = float(reference->GetDouble());
    return true;
}

// Each array is a flattened, declaration-order copy of the shader array.
bool ReadArray(const rapidjson::Value& section, const char* name, size_t count, AZStd::vector<float>& out, AZStd::string& error)
{
    const auto it = section.FindMember(name);
    if (it == section.MemberEnd() || !it->value.IsArray() || it->value.Size() != count)
    { error = AZStd::string::format("%s must have %zu values", name, count); return false; }
    for (const auto& value : it->value.GetArray())
    {
        if (!value.IsNumber()) { error = AZStd::string::format("%s has a non-number", name); return false; }
        // Values are shortest float32 round-trip text, so double->float is exact.
        out.push_back(float(value.GetDouble()));
    }
    return true;
}

bool ReadSection(const rapidjson::Value& root, const char* name, const AZStd::vector<AZStd::pair<const char*, size_t>>& arrays,
    AZStd::vector<float>& out, AZStd::string& error)
{
    const auto it = root.FindMember(name);
    if (it == root.MemberEnd() || !it->value.IsObject()) { error = AZStd::string::format("missing %s", name); return false; }
    for (const auto& [array, count] : arrays)
        if (!ReadArray(it->value, array, count, out, error)) { error = AZStd::string(name) + "." + error; return false; }
    return true;
}

bool ReadWeights(const AZStd::string& path, SurfaceProfileAsset& asset, AZStd::string& error)
{
    auto document = AZ::JsonSerializationUtils::ReadJsonFile(path);
    if (!document.IsSuccess()) { error = document.GetError(); return false; }
    const auto& root = document.GetValue();
    if (!root.IsObject()) { error = "weights must be a JSON object"; return false; }
    const auto version = root.FindMember("Version");
    if (version == root.MemberEnd() || !version->value.IsUint() || version->value.GetUint() != WeightsVersion)
    { error = "weights need Version 1"; return false; }
    const auto model = root.FindMember("ShaderModel");
    if (model != root.MemberEnd() && (!model->value.IsUint() || model->value.GetUint() != asset.m_shaderModel))
    { error = "weights were made for a different ShaderModel"; return false; }
    if (!ReadSection(root, "HitModel", {{"W0", 16 * 11}, {"B0", 16}, {"W1", 16 * 2}, {"B1", 2}}, asset.m_hitModel, error))
        return false;
    if (asset.m_shaderModel < 4) return true;
    const auto threshold = root.FindMember("PhysicalVisibilityThreshold");
    if (threshold == root.MemberEnd() || !threshold->value.IsNumber()) { error = "missing PhysicalVisibilityThreshold"; return false; }
    asset.m_physicalVisibilityThreshold = float(threshold->value.GetDouble());
    const AZStd::vector<AZStd::pair<const char*, size_t>> visibility{{"W0", 12 * 10}, {"B0", 12}, {"W1", 12}, {"B1", 1}};
    return ReadSection(root, "LevelTwo", {{"W0", 24 * 19}, {"W1", 24 * 5}, {"B0", 24}, {"B1", 5}}, asset.m_levelTwo, error)
        && ReadSection(root, "Visibility", visibility, asset.m_visibility, error)
        && ReadSection(root, "PhysicalVisibility", visibility, asset.m_physicalVisibility, error);
}

AZStd::string GemPath()
{
    AZStd::string found;
    if (auto* registry = AZ::SettingsRegistry::Get())
        AZ::SettingsRegistryMergeUtils::VisitActiveGems(*registry, [&found](AZStd::string_view name, AZStd::string_view path)
        { if (name == "SilPOM") found = path; });
    return found;
}

// Writes the missing weights file next to the profile with SilPOM's tool.
bool RunProfileTool(const AZStd::string& profile, AZStd::string& error)
{
    const auto gem = GemPath();
    if (gem.empty()) { error = "cannot locate the SilPOM gem to run Tools/silpom_profile.py"; return false; }
    AZStd::string python;
    char buffer[AZ_MAX_PATH_LEN];
    if (const auto overridePython = AZ::Utils::GetEnv(buffer, "SILPOM_PYTHON"); overridePython && !overridePython.GetValue().empty())
        python = overridePython.GetValue();
    else python = (AZ::IO::Path(AZ::Utils::GetEnginePath()) / "python/python.cmd").String();
    AzFramework::ProcessLauncher::ProcessLaunchInfo launch;
    launch.m_commandlineParameters = AZStd::vector<AZStd::string>{
        python, (AZ::IO::Path(gem) / "Tools/silpom_profile.py").String(), "weights", profile};
    launch.m_showWindow = false;
    AzFramework::ProcessOutput output;
    const bool ran = AzFramework::ProcessWatcher::LaunchProcessAndRetrieveOutput(
        launch, AzFramework::ProcessCommunicationType::COMMUNICATOR_TYPE_STDINOUT, output);
    AZ_TracePrintf("SilPOM", "silpom_profile.py: %s%s\n", output.outputResult.c_str(), output.errorResult.c_str());
    if (!ran) { error = "Tools/silpom_profile.py failed: " + output.errorResult; return false; }
    return true;
}

bool PngSize(const AZStd::string& path, AZ::u32& width, AZ::u32& height)
{
    auto bytes = AZ::Utils::ReadFile<AZStd::vector<AZ::u8>>(path, 32);
    if (!bytes.IsSuccess() || bytes.GetValue().size() < 24) return false;
    const auto& b = bytes.GetValue();
    if (memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) || memcmp(b.data() + 12, "IHDR", 4)) return false;
    width = (AZ::u32(b[16]) << 24) | (AZ::u32(b[17]) << 16) | (AZ::u32(b[18]) << 8) | b[19];
    height = (AZ::u32(b[20]) << 24) | (AZ::u32(b[21]) << 16) | (AZ::u32(b[22]) << 8) | b[23];
    return true;
}
} // namespace

void SurfaceProfileBuilder::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
        sc->Class<SurfaceProfileBuilder, AZ::Component>()->Version(1)->Attribute(AZ::Edit::Attributes::SystemComponentTags,
            AZStd::vector<AZ::Crc32>{AssetBuilderSDK::ComponentTags::AssetBuilder});
}

void SurfaceProfileBuilder::Activate()
{
    AssetBuilderSDK::AssetBuilderDesc desc;
    desc.m_name = JobKey;
    desc.m_version = 1;
    desc.m_analysisFingerprint = "profile-v1";
    desc.m_busId = azrtti_typeid<SurfaceProfileBuilder>();
    desc.m_patterns.emplace_back("*.silpomprofile", AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    desc.m_createJobFunction = AZStd::bind(&SurfaceProfileBuilder::CreateJobs, this, AZStd::placeholders::_1, AZStd::placeholders::_2);
    desc.m_processJobFunction = AZStd::bind(&SurfaceProfileBuilder::ProcessJob, this, AZStd::placeholders::_1, AZStd::placeholders::_2);
    BusConnect(desc.m_busId);
    AssetBuilderSDK::AssetBuilderBus::Broadcast(&AssetBuilderSDK::AssetBuilderBusTraits::RegisterBuilderInformation, desc);
}

void SurfaceProfileBuilder::Deactivate() { BusDisconnect(); }

void SurfaceProfileBuilder::CreateJobs(const AssetBuilderSDK::CreateJobsRequest& request, AssetBuilderSDK::CreateJobsResponse& response)
{
    const auto path = (AZ::IO::Path(request.m_watchFolder) / request.m_sourceFile).LexicallyNormal().String();
    ProfileSource source;
    AZStd::string error;
    if (!ReadProfile(path, source, error))
    {
        AZ_Error("SilPOM", false, "Profile %s: %s", request.m_sourceFile.c_str(), error.c_str());
        response.m_result = AssetBuilderSDK::CreateJobsResultCode::Failed;
        return;
    }
    // Rebuild when the weights (re)appear or the training height image changes.
    response.m_sourceFileDependencyList.emplace_back(source.weights, AZ::Uuid::CreateNull());
    response.m_sourceFileDependencyList.emplace_back(source.height, AZ::Uuid::CreateNull());
    for (const auto& platform : request.m_enabledPlatforms)
    {
        AssetBuilderSDK::JobDescriptor job;
        job.m_jobKey = JobKey;
        job.SetPlatformIdentifier(platform.m_identifier.c_str());
        job.m_failOnError = true;
        response.m_createJobOutputs.push_back(job);
    }
    response.m_result = AssetBuilderSDK::CreateJobsResultCode::Success;
}

void SurfaceProfileBuilder::ProcessJob(const AssetBuilderSDK::ProcessJobRequest& request, AssetBuilderSDK::ProcessJobResponse& response)
{
    response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
    const auto fail = [&](const AZStd::string& message)
    { AZ_Error("SilPOM", false, "Profile %s: %s", request.m_sourceFile.c_str(), message.c_str()); };
    ProfileSource source;
    AZStd::string error;
    if (!ReadProfile(request.m_fullPath, source, error)) return fail(error);
    if (!AZ::IO::SystemFile::Exists(source.weights.c_str()))
    {
        if (!RunProfileTool(request.m_fullPath, error)) return fail(error);
        if (!AZ::IO::SystemFile::Exists(source.weights.c_str())) return fail("the profile tool did not write " + source.weights);
    }
    if (!AZ::IO::SystemFile::Exists(source.height.c_str())) return fail("height image not found: " + source.height);
    AZ::u32 width = 0, height = 0;
    if (PngSize(source.height, width, height) && (width != source.width || height != source.heightPixels))
        return fail(AZStd::string::format("height image is %u x %u, the profile was trained on %u x %u",
            width, height, source.width, source.heightPixels));

    SurfaceProfileAsset asset;
    asset.m_width = source.width; asset.m_heightPixels = source.heightPixels;
    asset.m_scale = source.scale; asset.m_reference = source.reference;
    asset.m_addressMode = source.addressMode; asset.m_role = source.role; asset.m_shaderModel = source.shaderModel;
    // Product path of the image: its path below the scan folder, lower case.
    AZ::IO::Path relative = AZ::IO::Path(source.height).LexicallyRelative(AZ::IO::Path(request.m_watchFolder).LexicallyNormal());
    if (relative.empty() || relative.Native().starts_with("..")) return fail("height image must be in the same scan folder as the profile");
    asset.m_heightProduct = relative.AsPosix() + ".streamingimage";
    AZStd::to_lower(asset.m_heightProduct.begin(), asset.m_heightProduct.end());
    if (!ReadWeights(source.weights, asset, error)) return fail(source.weights + ": " + error);
    if (const auto invalid = asset.Validate(); !invalid.empty()) return fail(invalid);

    SHA256_CTX hash;
    SHA256_Init(&hash);
    for (const auto& file : {AZStd::string(request.m_fullPath), source.weights})
    {
        auto bytes = AZ::Utils::ReadFile(file);
        if (!bytes.IsSuccess()) return fail("cannot read " + file);
        SHA256_Update(&hash, bytes.GetValue().data(), bytes.GetValue().size());
    }
    unsigned char digest[32];
    SHA256_Final(digest, &hash);
    for (auto b : digest) asset.m_generation += AZStd::string::format("%02x", b);

    const auto output = (AZ::IO::Path(request.m_tempDirPath) / (AZ::IO::Path(request.m_sourceFile).Stem().String() + ".silpomprofileasset")).String();
    if (!AZ::Utils::SaveObjectToFile(output, AZ::DataStream::ST_BINARY, &asset)) return fail("cannot write the product");
    AssetBuilderSDK::JobProduct product(output, azrtti_typeid<SurfaceProfileAsset>(), 0);
    product.m_dependenciesHandled = true;
    response.m_outputProducts.push_back(product);
    response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Success;
}
} // namespace SilPOM
