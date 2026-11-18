// SPDX-License-Identifier: MIT
#include "LearnedMaterialBuilder.h"
#include <SilPOM/LearnedMaterialAsset.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Serialization/EditContextConstants.inl>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/Utils/Utils.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/std/functional.h>
#include <openssl/sha.h>
namespace SilPOM
{
namespace
{
// A learned bundle names the files it was trained from in a sidecar next to
// it: <bundle>.sources, e.g. wall.l4mat.sources. Paths are relative to the
// bundle's folder, so the bundle and its sources can live in any project.
//
// {
//     "Material": ["basecolor.png", "normal.png", "roughness.png"],
//     "Training": ["height.png", "surface.silpom.json"]
// }
//
// "Material" is required and hashed for every bundle. "Training" is hashed
// after it for bundles that are not SILPMAT4 (Level 5 bundles). The order in
// each list is the order the trainer hashed the files in.
struct BundleSources
{
    AZStd::vector<AZStd::string> material, training;
};

AZStd::string SidecarPath(const AZStd::string& bundlePath)
{
    return bundlePath + ".sources";
}

bool ReadPathList(const rapidjson::Value& root, const char* key, const AZ::IO::Path& folder, bool required,
    AZStd::vector<AZStd::string>& out, AZStd::string& error)
{
    const auto member = root.FindMember(key);
    if (member == root.MemberEnd())
    {
        if (required) error = AZStd::string::format("missing \"%s\" list", key);
        return !required;
    }
    if (!member->value.IsArray() || member->value.Empty())
    {
        error = AZStd::string::format("\"%s\" must be a non-empty list of paths", key);
        return false;
    }
    for (const auto& entry : member->value.GetArray())
    {
        if (!entry.IsString())
        {
            error = AZStd::string::format("\"%s\" entries must be strings", key);
            return false;
        }
        const AZ::IO::Path relative(entry.GetString());
        if (relative.IsAbsolute())
        {
            error = AZStd::string::format("\"%s\" paths must be relative to the bundle folder", key);
            return false;
        }
        out.push_back((folder / relative).LexicallyNormal().String());
    }
    return true;
}

bool ReadBundleSources(const AZStd::string& bundlePath, BundleSources& sources, AZStd::string& error)
{
    const auto sidecar = SidecarPath(bundlePath);
    auto document = AZ::JsonSerializationUtils::ReadJsonFile(sidecar);
    if (!document.IsSuccess())
    {
        error = AZStd::string::format("cannot read %s: %s", sidecar.c_str(), document.GetError().c_str());
        return false;
    }
    const auto& root = document.GetValue();
    if (!root.IsObject())
    {
        error = AZStd::string::format("%s must contain a JSON object", sidecar.c_str());
        return false;
    }
    // Own the folder: ParentPath() of a temporary is a view into freed storage.
    const AZ::IO::Path folder(AZ::IO::Path(bundlePath).ParentPath());
    return ReadPathList(root, "Material", folder, true, sources.material, error)
        && ReadPathList(root, "Training", folder, false, sources.training, error);
}
}
void LearnedMaterialBuilder::Reflect(AZ::ReflectContext* c)
{
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(c))sc->Class<LearnedMaterialBuilder,AZ::Component>()->Version(1)
        ->Attribute(AZ::Edit::Attributes::SystemComponentTags,AZStd::vector<AZ::Crc32>{AssetBuilderSDK::ComponentTags::AssetBuilder});
}
void LearnedMaterialBuilder::Activate()
{
    AssetBuilderSDK::AssetBuilderDesc d;d.m_name="SilPOM learned material";d.m_version=6;
    d.m_analysisFingerprint="material-v5-sidecar-sources";d.m_busId=azrtti_typeid<LearnedMaterialBuilder>();
    d.m_patterns.emplace_back("*.l4mat",AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    d.m_patterns.emplace_back("*.l5app",AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    d.m_patterns.emplace_back("*.l4shared",AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    d.m_patterns.emplace_back("*.l5depth",AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    d.m_createJobFunction=AZStd::bind(&LearnedMaterialBuilder::CreateJobs,this,AZStd::placeholders::_1,AZStd::placeholders::_2);
    d.m_processJobFunction=AZStd::bind(&LearnedMaterialBuilder::ProcessJob,this,AZStd::placeholders::_1,AZStd::placeholders::_2);
    BusConnect(d.m_busId);AssetBuilderSDK::AssetBuilderBus::Broadcast(&AssetBuilderSDK::AssetBuilderBusTraits::RegisterBuilderInformation,d);
}
void LearnedMaterialBuilder::Deactivate(){BusDisconnect();}
void LearnedMaterialBuilder::CreateJobs(const AssetBuilderSDK::CreateJobsRequest& q,AssetBuilderSDK::CreateJobsResponse& r)
{
    const auto bundle=(AZ::IO::Path(q.m_watchFolder)/q.m_sourceFile).LexicallyNormal().String();
    // Reprocess when the sidecar changes, and when any listed source changes.
    r.m_sourceFileDependencyList.emplace_back(SidecarPath(bundle),AZ::Uuid::CreateNull());
    BundleSources sources;AZStd::string error;
    if(!ReadBundleSources(bundle,sources,error))
    {
        AZ_Error("SilPOM",false,"Learned bundle %s: %s",q.m_sourceFile.c_str(),error.c_str());
        r.m_result=AssetBuilderSDK::CreateJobsResultCode::Failed;return;
    }
    for(const auto& file:sources.material)r.m_sourceFileDependencyList.emplace_back(file,AZ::Uuid::CreateNull());
    for(const auto& file:sources.training)r.m_sourceFileDependencyList.emplace_back(file,AZ::Uuid::CreateNull());
    for(const auto& p:q.m_enabledPlatforms){AssetBuilderSDK::JobDescriptor j;j.m_jobKey="SilPOM learned material";j.SetPlatformIdentifier(p.m_identifier.c_str());j.m_failOnError=true;r.m_createJobOutputs.push_back(j);}
    r.m_result=AssetBuilderSDK::CreateJobsResultCode::Success;
}
void LearnedMaterialBuilder::ProcessJob(const AssetBuilderSDK::ProcessJobRequest& q,AssetBuilderSDK::ProcessJobResponse& r)
{
    r.m_resultCode=AssetBuilderSDK::ProcessJobResult_Failed;
    auto bytes=AZ::Utils::ReadFile(q.m_fullPath);if(!bytes.IsSuccess())return;
    LearnedMaterialAsset asset;asset.m_bytes.assign(bytes.GetValue().begin(),bytes.GetValue().end());
    if(!asset.Validate()){AZ_Error("SilPOM",false,"Invalid learned material bundle");return;}
    BundleSources sources;AZStd::string error;
    if(!ReadBundleSources(q.m_fullPath,sources,error)){AZ_Error("SilPOM",false,"Learned bundle %s: %s",q.m_sourceFile.c_str(),error.c_str());return;}
    AZStd::string original;
    auto append=[&original](const AZStd::string& file)
    {
        auto data=AZ::Utils::ReadFile(file);
        if(!data.IsSuccess()){AZ_Error("SilPOM",false,"Learned bundle source is unavailable: %s",file.c_str());return false;}
        original+=data.GetValue();return true;
    };
    for(const auto& file:sources.material)if(!append(file))return;
    if(memcmp(asset.Header().magic,"SILPMAT4",8))
    {
        if(sources.training.empty()){AZ_Error("SilPOM",false,"Level 5 bundle %s needs a \"Training\" list in its .sources sidecar",q.m_sourceFile.c_str());return;}
        for(const auto& file:sources.training)if(!append(file))return;
    }
    unsigned char sourceHash[32];SHA256(reinterpret_cast<const unsigned char*>(original.data()),original.size(),sourceHash);
    if(memcmp(sourceHash,asset.Header().sourceHash,32)){AZ_Error("SilPOM",false,"Training sources changed; retrain the learned bundle");return;}
    unsigned char hash[32];SHA256(asset.m_bytes.data(),asset.m_bytes.size(),hash);
    for(auto b:hash)asset.m_generation+=AZStd::string::format("%02x",b);
    const auto output=(AZ::IO::Path(q.m_tempDirPath)/(AZ::IO::Path(q.m_sourceFile).Stem().String()+".silpommaterial")).String();
    if(!AZ::Utils::SaveObjectToFile(output,AZ::DataStream::ST_BINARY,&asset))return;
    AssetBuilderSDK::JobProduct p(output,azrtti_typeid<LearnedMaterialAsset>(),1);p.m_dependenciesHandled=true;
    r.m_outputProducts.push_back(p);r.m_resultCode=AssetBuilderSDK::ProcessJobResult_Success;
}
}
