// SPDX-License-Identifier: MIT
#include "MeshSurfaceBuilder.h"
#include <SilPOM/MeshSurfaceAsset.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Serialization/EditContextConstants.inl>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/Utils/Utils.h>
#include <AzCore/std/containers/map.h>
#include <AzCore/std/containers/set.h>
#include <AzCore/std/functional.h>
#include <AzCore/std/sort.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <SceneAPI/SceneCore/Containers/Scene.h>
#include <SceneAPI/SceneCore/Events/SceneSerializationBus.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshVertexUVData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMaterialData.h>
#include <openssl/sha.h>
#include <cmath>

namespace SilPOM
{
namespace
{
using Json = rapidjson::Value;
AZStd::string FbxPath(AZStd::string sidecar)
{
    constexpr size_t suffix = sizeof(".silpom.json") - 1;
    sidecar.resize(sidecar.size() - suffix);
    return sidecar + ".fbx";
}
AZStd::string Hash(const AZStd::string& bytes)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    AZStd::string result;
    for (unsigned char value : digest) result += AZStd::string::format("%02x", value);
    return result;
}
const Json* Member(const Json& object, const char* name)
{
    if (!object.IsObject()) return nullptr;
    auto found = object.FindMember(name);
    return found == object.MemberEnd() ? nullptr : &found->value;
}
AZStd::string String(const Json& object, const char* name)
{
    auto* value = Member(object, name);
    return value && value->IsString() ? AZStd::string(value->GetString(), value->GetStringLength()) : AZStd::string();
}
bool Vector(const Json& value, AZ::Vector3& out, unsigned count = 3)
{
    if (!value.IsArray() || value.Size() != count) return false;
    out = AZ::Vector3::CreateZero();
    for (unsigned i = 0; i != count; ++i)
    {
        if (!value[i].IsNumber() || !std::isfinite(value[i].GetDouble())) return false;
        out.SetElement(i, float(value[i].GetDouble()));
    }
    return out.IsFinite();
}
bool Id(float value, AZ::u32& id)
{
    if (!std::isfinite(value) || value <= 0 || value >= float(1 << 24) || std::abs(value - std::round(value)) > 1e-5f)
        return false;
    id = AZ::u32(std::round(value));
    return true;
}

// Reconstruct logical identity from transport channels, never from post-import
// vertex indices or coincident positions. The sidecar is checked against the
// real SceneAPI graph before any renderer consumes it.
bool Compile(const Json& root, const AZ::SceneAPI::Containers::Scene& scene, MeshSurfaceAsset& output, AZStd::string& error)
{
    auto fail = [&](const AZStd::string& message) { error = message; return false; };
    const auto* version = Member(root, "version");
    const auto* faces = Member(root, "faces");
    const auto* vertices = Member(root, "vertices");
    const auto* logical = Member(root, "logical_vertex_ids");
    const auto* materials = Member(root, "materials");
    const auto* objects = Member(root, "objects");
    if (!version || !version->IsUint() || version->GetUint() != 2 || !faces || !faces->IsArray()
        || !vertices || !vertices->IsObject() || !logical || !logical->IsObject()
        || !materials || !materials->IsArray() || !objects || !objects->IsArray())
        return fail("Expected transport version 2: re-export with the SilPOM Face add-on.");
    AZStd::map<AZ::u32, const Json*> expected;
    AZStd::set<AZStd::string> faceIds, objectIds, vertexIds;
    AZStd::map<AZStd::string, AZStd::string> materialTokens;
    for (const auto& object : objects->GetArray())
    {
        auto id = String(object, "id");
        if (id.empty() || !objectIds.insert(id).second) return fail("Duplicate or empty object identity.");
    }
    for (auto it = logical->MemberBegin(); it != logical->MemberEnd(); ++it)
        if (!it->value.IsString() || !vertexIds.insert(it->value.GetString()).second)
            return fail("Ambiguous logical vertex identity.");
    for (const auto& material : materials->GetArray())
    {
        auto id = String(material, "id"), token = String(material, "export_name");
        if (id.empty() || token.empty() || !materialTokens.emplace(id, token).second)
            return fail("Duplicate or missing material identity.");
        output.m_materials.push_back({id, String(material, "name")});
    }
    for (const auto& face : faces->GetArray())
    {
        const auto* id = Member(face, "id");
        if (!id || !id->IsUint() || id->GetUint() == 0 || id->GetUint() >= (1u << 24)
            || !expected.emplace(id->GetUint(), &face).second || String(face, "stable_id").empty()
            || !faceIds.insert(String(face, "stable_id")).second)
            return fail("Duplicate, missing or unrepresentable face identity.");
    }
    const auto& graph = scene.GetGraph();
    AZStd::vector<AZ::SceneAPI::Containers::SceneGraph::NodeIndex> pending{graph.GetRoot()};
    AZStd::set<AZ::u32> seen;
    while (!pending.empty())
    {
        auto node = pending.back(); pending.pop_back();
        if (graph.HasNodeSibling(node)) pending.push_back(graph.GetNodeSibling(node));
        if (graph.HasNodeChild(node)) pending.push_back(graph.GetNodeChild(node));
        auto content = graph.GetNodeContent(node);
        auto* mesh = azrtti_cast<const AZ::SceneAPI::DataTypes::IMeshData*>(content.get());
        if (!mesh) continue;
        const AZ::SceneAPI::DataTypes::IMeshVertexUVData* uv[4]{};
        const char* names[] = {"UVMap", "SP_Identity", "SP_DirectionXY", "SP_DirectionZ"};
        AZStd::vector<AZStd::string> tokens;
        for (auto child = graph.GetNodeChild(node); child.IsValid(); child = graph.GetNodeSibling(child))
        {
            auto data = graph.GetNodeContent(child);
            if (auto* channel = azrtti_cast<const AZ::SceneAPI::DataTypes::IMeshVertexUVData*>(data.get()))
                for (unsigned i = 0; i != 4; ++i)
                    if (AZStd::string_view(graph.GetNodeName(child).GetName()) == names[i]
                        || AZStd::string_view(graph.GetNodeName(child).GetName()) == AZStd::string::format("UV%u", i)) uv[i] = channel;
            if (azrtti_cast<const AZ::SceneAPI::DataTypes::IMaterialData*>(data.get())) tokens.push_back(graph.GetNodeName(child).GetName());
        }
        for (const auto* channel : uv)
            if (!channel || channel->GetCount() != mesh->GetVertexCount()) return fail("Missing or damaged SilPOM transport UV channels.");
        if (!mesh->HasNormalData()) return fail("Missing authored corner normals.");
        for (unsigned fi = 0; fi < mesh->GetFaceCount(); ++fi)
        {
            const auto& indices = mesh->GetFaceInfo(fi);
            AZ::u32 fid = 0;
            if (!Id(uv[1]->GetUV(indices.vertexIndex[0]).GetX(), fid) || !expected.contains(fid) || !seen.insert(fid).second)
                return fail("Unknown or duplicated imported face identity.");
            const Json& record = *expected[fid];
            MeshFace face;
            face.m_id = String(record, "stable_id"); face.m_objectId = String(record, "object_id");
            face.m_materialId = String(record, "material_id");
            const auto* region = Member(record, "region"), *profile = Member(record, "profile");
            const auto* vids = Member(record, "vertices"), *uvs = Member(record, "uv");
            const auto* normals = Member(record, "normals"), *directions = Member(record, "directions");
            if (!region || !region->IsUint() || !profile || !profile->IsUint() || (region->GetUint() && !profile->GetUint())
                || !vids || !vids->IsArray() || vids->Size() != 3 || !uvs || !uvs->IsArray() || uvs->Size() != 3
                || !normals || !normals->IsArray() || normals->Size() != 3 || !directions || !directions->IsArray() || directions->Size() != 3
                || !objectIds.contains(face.m_objectId) || !face.m_id.starts_with(face.m_objectId + ":")
                || !materialTokens.contains(face.m_materialId)) return fail("Invalid face/profile/corner binding: " + face.m_id);
            face.m_region = region->GetUint(); face.m_profile = profile->GetUint();
            if ((AZStd::string_view(graph.GetNodeName(node).GetPath()).find("SilPOM_Selected") != AZStd::string_view::npos) != bool(face.m_region))
                return fail("Selection partition disagrees with metadata: " + face.m_id);
            const auto materialIndex = mesh->GetFaceMaterialId(fi);
            if (materialIndex >= tokens.size() || tokens[materialIndex] != materialTokens[face.m_materialId])
                return fail("Imported material identity disagrees with metadata: " + face.m_id);
            unsigned rotation = 3;
            for (unsigned ci = 0; ci != 3; ++ci)
            {
                unsigned index = indices.vertexIndex[ci]; AZ::u32 cornerFid = 0, vid = 0;
                if (!Id(uv[1]->GetUV(index).GetX(), cornerFid) || cornerFid != fid || !Id(1 - uv[1]->GetUV(index).GetY(), vid))
                    return fail("Mixed face or logical vertex transport: " + face.m_id);
                unsigned authored = 3;
                for (unsigned k = 0; k != 3; ++k) if ((*vids)[k].IsUint() && (*vids)[k].GetUint() == vid) authored = k;
                if (authored == 3) return fail("Logical topology changed: " + face.m_id);
                if (ci == 0) rotation = authored;
                else if (authored != (rotation + ci) % 3) return fail("Triangle winding changed: " + face.m_id);
                auto key = AZStd::string::format("%u", vid);
                const auto* position = Member(*vertices, key.c_str()), *identity = Member(*logical, key.c_str());
                MeshCorner corner; AZ::Vector3 expectedUv;
                if (!position || !identity || !identity->IsString() || !Vector(*position, corner.m_position)
                    || !Vector((*uvs)[authored], expectedUv, 2) || !Vector((*normals)[authored], corner.m_normal)
                    || !Vector((*directions)[authored], corner.m_direction)) return fail("Invalid corner data: " + face.m_id);
                corner.m_vertexId = identity->GetString(); corner.m_uv = AZ::Vector2(expectedUv.GetX(), expectedUv.GetY());
                const auto& importedUv = uv[0]->GetUV(index);
                AZ::Vector3 direction(uv[2]->GetUV(index).GetX(), 1 - uv[2]->GetUV(index).GetY(), uv[3]->GetUV(index).GetX());
                if (!corner.m_vertexId.starts_with(face.m_objectId + ":") || !corner.m_position.IsClose(mesh->GetPosition(index), 2e-5f)
                    || !corner.m_normal.IsClose(mesh->GetNormal(index), 2e-4f) || !corner.m_direction.IsClose(direction, 2e-5f)
                    || std::abs(corner.m_direction.GetLengthSq() - 1) > 2e-5f
                    || !corner.m_uv.IsClose(AZ::Vector2(importedUv.GetX(), 1 - importedUv.GetY()), 2e-5f)
                    || std::abs(1 - uv[3]->GetUV(index).GetY() - 17) > 1e-5f)
                    return fail("Position/units/UV/normal/direction transport mismatch: " + face.m_id);
                face.m_corners.push_back(AZStd::move(corner));
            }
            const auto e0 = face.m_corners[1].m_position - face.m_corners[0].m_position;
            const auto e1 = face.m_corners[2].m_position - face.m_corners[0].m_position;
            if (e0.Cross(e1).GetLengthSq() < 1e-16f) return fail("Degenerate base triangle: " + face.m_id);
            face.m_neighbors.assign(3, -1);
            output.m_faces.push_back(AZStd::move(face));
        }
    }
    if (seen.size() != expected.size() || seen.empty()) return fail("Missing imported faces or empty surface.");
    AZStd::sort(output.m_faces.begin(), output.m_faces.end(), [](const auto& a, const auto& b) { return a.m_id < b.m_id; });
    AZStd::sort(output.m_materials.begin(), output.m_materials.end(), [](const auto& a, const auto& b) { return a.m_id < b.m_id; });
    struct Edge { AZ::u32 face, corner; bool reversed; };
    AZStd::map<AZStd::pair<AZStd::string, AZStd::string>, AZStd::vector<Edge>> edges;
    for (AZ::u32 fi = 0; fi < output.m_faces.size(); ++fi)
        for (AZ::u32 ci = 0; ci != 3; ++ci)
        {
            const auto& corners = output.m_faces[fi].m_corners;
            auto a = corners[ci].m_vertexId, b = corners[(ci + 1) % 3].m_vertexId;
            bool reversed = a > b; if (reversed) AZStd::swap(a, b);
            edges[{a,b}].push_back({fi,ci,reversed});
        }
    for (const auto& [key, edge] : edges)
    {
        if (edge.size() > 2 || (edge.size() == 2 && edge[0].reversed == edge[1].reversed))
            return fail("Nonmanifold or inconsistently oriented logical edge: " + key.first + " / " + key.second);
        if (edge.size() == 2)
        {
            output.m_faces[edge[0].face].m_neighbors[edge[0].corner] = edge[1].face;
            output.m_faces[edge[1].face].m_neighbors[edge[1].corner] = edge[0].face;
        }
    }
    return true;
}
}
void MeshSurfaceBuilder::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
        sc->Class<MeshSurfaceBuilder, AZ::Component>()->Version(1)->Attribute(AZ::Edit::Attributes::SystemComponentTags,
            AZStd::vector<AZ::Crc32>{AssetBuilderSDK::ComponentTags::AssetBuilder});
}
void MeshSurfaceBuilder::Activate()
{
    m_stopping = false;
    AssetBuilderSDK::AssetBuilderDesc desc;
    desc.m_name = "SilPOM Mesh Surface"; desc.m_version = 1;
    desc.m_analysisFingerprint = "mesh-surface-v1-transport-v2";
    desc.m_patterns.emplace_back("*.silpom.json", AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    desc.m_busId = azrtti_typeid<MeshSurfaceBuilder>();
    desc.m_createJobFunction = AZStd::bind(&MeshSurfaceBuilder::CreateJobs, this, AZStd::placeholders::_1, AZStd::placeholders::_2);
    desc.m_processJobFunction = AZStd::bind(&MeshSurfaceBuilder::ProcessJob, this, AZStd::placeholders::_1, AZStd::placeholders::_2);
    BusConnect(desc.m_busId);
    AssetBuilderSDK::AssetBuilderBus::Broadcast(&AssetBuilderSDK::AssetBuilderBusTraits::RegisterBuilderInformation, desc);
}
void MeshSurfaceBuilder::Deactivate() { m_stopping = true; BusDisconnect(); }
void MeshSurfaceBuilder::CreateJobs(const AssetBuilderSDK::CreateJobsRequest& request, AssetBuilderSDK::CreateJobsResponse& response)
{
    if (m_stopping) { response.m_result = AssetBuilderSDK::CreateJobsResultCode::ShuttingDown; return; }
    auto fbx = FbxPath((AZ::IO::Path(request.m_watchFolder) / request.m_sourceFile).String());
    // Include missing dependencies so restoring an FBX or manifest retries a job.
    response.m_sourceFileDependencyList.emplace_back(fbx, AZ::Uuid::CreateNull());
    response.m_sourceFileDependencyList.emplace_back(fbx + ".assetinfo", AZ::Uuid::CreateNull());
    for (const auto& platform : request.m_enabledPlatforms)
    {
        AssetBuilderSDK::JobDescriptor job; job.m_jobKey = "SilPOM Mesh Surface";
        job.SetPlatformIdentifier(platform.m_identifier.c_str()); job.m_failOnError = true;
        response.m_createJobOutputs.push_back(AZStd::move(job));
    }
    response.m_result = AssetBuilderSDK::CreateJobsResultCode::Success;
}
void MeshSurfaceBuilder::ProcessJob(const AssetBuilderSDK::ProcessJobRequest& request, AssetBuilderSDK::ProcessJobResponse& response)
{
    response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Failed;
    AssetBuilderSDK::JobCancelListener cancel(request.m_jobId);
    if (m_stopping || cancel.IsCancelled()) { response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Cancelled; return; }
    auto json = AZ::JsonSerializationUtils::ReadJsonFile(request.m_fullPath);
    auto fbx = FbxPath(request.m_fullPath);
    auto bytes = AZ::Utils::ReadFile(fbx);
    if (!json.IsSuccess() || !bytes.IsSuccess())
    { AZ_Error("SilPOM", false, "%s: missing/unreadable FBX or metadata; re-export the paired bundle.", request.m_fullPath.c_str()); return; }
    auto& root = json.GetValue();
    auto fbxHash = Hash(bytes.GetValue());
    if (String(root, "fbx_sha256") != fbxHash)
    { AZ_Error("SilPOM", false, "%s: stale FBX/sidecar pair; re-export both files.", request.m_fullPath.c_str()); return; }
    AZStd::shared_ptr<AZ::SceneAPI::Containers::Scene> scene;
    AZ::Data::AssetInfo sourceInfo; AZStd::string sourceWatchFolder; bool foundSource = false;
    AzToolsFramework::AssetSystemRequestBus::BroadcastResult(foundSource,
        &AzToolsFramework::AssetSystem::AssetSystemRequest::GetSourceInfoBySourcePath,
        fbx.c_str(), sourceInfo, sourceWatchFolder);
    if (!foundSource || sourceInfo.m_assetId.m_guid.IsNull())
    { AZ_Error("SilPOM", false, "%s: FBX source identity is not registered with Asset Processor.", fbx.c_str()); return; }
    AZ::SceneAPI::Events::SceneSerializationBus::BroadcastResult(scene,
        &AZ::SceneAPI::Events::SceneSerialization::LoadScene, fbx, sourceInfo.m_assetId.m_guid, sourceWatchFolder);
    if (!scene) { AZ_Error("SilPOM", false, "%s: O3DE SceneAPI import failed.", fbx.c_str()); return; }
    MeshSurfaceAsset asset; AZStd::string error;
    if (!Compile(root, *scene, asset, error)) { AZ_Error("SilPOM", false, "%s: %s", fbx.c_str(), error.c_str()); return; }
    auto source = AZ::Utils::ReadFile(request.m_fullPath);
    if (!source.IsSuccess()) return;
    asset.m_generation = Hash(source.GetValue() + fbxHash);
    auto output = (AZ::IO::Path(request.m_tempDirPath) / (AZ::IO::Path(fbx).Filename().String() + ".silpommesh")).String();
    if (m_stopping || cancel.IsCancelled()) { response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Cancelled; return; }
    if (!AZ::Utils::SaveObjectToFile(output, AZ::DataStream::ST_BINARY, &asset))
    { AZ_Error("SilPOM", false, "Cannot save mesh surface product: %s", output.c_str()); return; }
    AssetBuilderSDK::JobProduct product(output, azrtti_typeid<MeshSurfaceAsset>(), 1);
    product.m_dependenciesHandled = true;
    response.m_outputProducts.push_back(AZStd::move(product));
    response.m_resultCode = AssetBuilderSDK::ProcessJobResult_Success;
}
}
