// SPDX-License-Identifier: MIT
#include <SilPOM/Mesh.h>
#include "MeshRender.h"
#include <SilPOM/Curved/CurvedPacking.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <Atom/RPI.Reflect/Material/MaterialPropertiesLayout.h>
#include <Atom/RPI.Reflect/Material/MaterialAssetCreator.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzFramework/Asset/AssetCatalogBus.h>
#include <AzCore/std/containers/map.h>
#include <AzCore/std/containers/set.h>
#include <AzCore/std/parallel/atomic.h>

namespace SilPOM
{
namespace
{
using Material = AZ::Data::Instance<AZ::RPI::Material>;
using Handle = AZ::Render::MeshFeatureProcessorInterface::MeshHandle;
struct Vertex { AZ::Vector3 position, normal; AZ::Vector2 uv; };
AZ::Data::Asset<AZ::RPI::BufferAsset> Buffer(const void* data, size_t count, AZ::u32 stride, bool raw = false)
{
    AZ::RPI::BufferAssetCreator creator; creator.Begin(AZ::Uuid::CreateRandom());
    AZ::RHI::BufferDescriptor descriptor;
    descriptor.m_bindFlags = AZ::RHI::BufferBindFlags::InputAssembly | AZ::RHI::BufferBindFlags::ShaderRead;
    descriptor.m_byteCount = count * stride;
    creator.SetBuffer(data, descriptor.m_byteCount, descriptor);
    creator.SetBufferViewDescriptor(raw ? AZ::RHI::BufferViewDescriptor::CreateRaw(0, AZ::u32(descriptor.m_byteCount))
        : AZ::RHI::BufferViewDescriptor::CreateStructured(0, AZ::u32(count), stride));
    creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
    AZ::Data::Asset<AZ::RPI::BufferAsset> result; creator.End(result); return result;
}
AZ::Data::Asset<AZ::RPI::ModelAsset> Model(const AZStd::vector<Vertex>& vertices, const AZStd::vector<AZ::u32>& indices, const AZ::Aabb& bounds)
{
    auto indexBuffer = Buffer(indices.data(), indices.size(), 4);
    AZ::RPI::ModelLodAssetCreator lod; lod.Begin(AZ::Uuid::CreateRandom()); lod.SetLodIndexBuffer(indexBuffer); lod.BeginMesh();
    lod.SetMeshAabb(bounds); lod.SetMeshMaterialSlot(0);
    lod.SetMeshIndexBuffer({indexBuffer, AZ::RHI::BufferViewDescriptor::CreateTyped(0, AZ::u32(indices.size()), AZ::RHI::Format::R32_UINT)});
    AZStd::vector<float> positions, normals, uvs, tangents;
    for (const auto& v : vertices)
    {
        positions.insert(positions.end(), {v.position.GetX(), v.position.GetY(), v.position.GetZ()});
        normals.insert(normals.end(), {v.normal.GetX(), v.normal.GetY(), v.normal.GetZ()});
        uvs.insert(uvs.end(), {v.uv.GetX(), v.uv.GetY()});
        auto tangent = v.normal.GetOrthogonalVector().GetNormalizedSafe();
        tangents.insert(tangents.end(), {tangent.GetX(), tangent.GetY(), tangent.GetZ(), 1});
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const auto& a = vertices[indices[i]], &b = vertices[indices[i + 1]], &c = vertices[indices[i + 2]];
        const auto duv1 = b.uv - a.uv, duv2 = c.uv - a.uv;
        const float det = duv1.GetX() * duv2.GetY() - duv1.GetY() * duv2.GetX();
        if (std::abs(det) < 1e-10f) continue;
        const auto raw = ((b.position - a.position) * duv2.GetY() - (c.position - a.position) * duv1.GetY()) / det;
        const auto bitangent = ((c.position - a.position) * duv1.GetX() - (b.position - a.position) * duv2.GetX()) / det;
        for (size_t k = 0; k != 3; ++k)
        {
            const size_t index = indices[i + k]; const auto& n = vertices[index].normal;
            const auto t = (raw - n * raw.Dot(n)).GetNormalizedSafe();
            tangents[index * 4] = t.GetX(); tangents[index * 4 + 1] = t.GetY(); tangents[index * 4 + 2] = t.GetZ();
            tangents[index * 4 + 3] = n.Cross(t).Dot(bitangent) < 0 ? -1.f : 1.f;
        }
    }
    auto stream = [&](const char* name, AZ::u32 semantic, const AZStd::vector<float>& values, AZ::u32 components)
    {
        auto buffer = Buffer(values.data(), vertices.size(), components * 4);
        if (!buffer) return false;
        lod.AddLodStreamBuffer(buffer);
        const auto format = components == 2 ? AZ::RHI::Format::R32G32_FLOAT : components == 3 ? AZ::RHI::Format::R32G32B32_FLOAT : AZ::RHI::Format::R32G32B32A32_FLOAT;
        return lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(AZ::Name(name), semantic), AZ::Name(),
            {buffer, AZ::RHI::BufferViewDescriptor::CreateTyped(0, AZ::u32(vertices.size()), format)});
    };
    if (!stream("POSITION", 0, positions, 3) || !stream("NORMAL", 0, normals, 3)
        || !stream("TANGENT", 0, tangents, 4) || !stream("UV", 0, uvs, 2) || !stream("UV", 1, uvs, 2)) return {};
    lod.EndMesh(); AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset; if (!lod.End(lodAsset)) return {};
    AZ::RPI::ModelAssetCreator model; model.Begin(AZ::Uuid::CreateRandom()); model.SetName("SilPOM mesh generation");
    AZ::RPI::ModelMaterialSlot slot; slot.m_stableId = 0; slot.m_displayName = AZ::Name("Authored material");
    model.AddMaterialSlot(slot); model.AddLodAsset(AZStd::move(lodAsset));
    AZ::Data::Asset<AZ::RPI::ModelAsset> result; model.End(result); return result;
}
template<class T> bool Set(const Material& material, const char* name, const T& value)
{
    auto index = material->FindPropertyIndex(AZ::Name(name));
    if (!index.IsValid() || !material->GetPropertyValue(index).Is<T>()) return false;
    material->SetPropertyValue(index, value); return true;
}
Curved::Vec3 Vector(const AZ::Vector3& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
Material Snapshot(const AZ::Data::Asset<AZ::RPI::MaterialAsset>& source)
{
    AZ::RPI::MaterialAssetCreator creator;
    const auto& type = source->GetMaterialTypeAsset();
    creator.Begin(AZ::Uuid::CreateRandom(), type);
    creator.SetMaterialTypeVersion(type->GetVersion());
    const auto layout = type->GetMaterialPropertiesLayout();
    const auto& values = source->GetPropertyValues();
    for (size_t i = 0; i < values.size(); ++i)
        creator.SetPropertyValue(layout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(i))->GetName(), values[i]);
    AZ::Data::Asset<AZ::RPI::MaterialAsset> asset;
    return creator.End(asset) ? AZ::RPI::Material::Create(asset) : Material{};
}
}
struct MeshController::Generation
{
    using PacketHandler = AZ::Render::ModelDataInstanceInterface::MeshDrawPacketUpdatedEvent::Handler;
    AZStd::vector<Handle> handles;
    AZStd::vector<AZStd::unique_ptr<PacketHandler>> packetHandlers;
    AZStd::vector<Material> materials;
    AZStd::vector<AZ::Data::Asset<AZ::RPI::ModelAsset>> models;
    AZStd::vector<AZ::Data::Instance<AZ::RPI::Buffer>> buffers;
    AZStd::vector<AZ::Data::Instance<AZ::RPI::Image>> heights;
    AZStd::vector<AZStd::shared_ptr<MeshRenderBatch>> computeBatches;
    AZStd::string id;
    size_t fragments = 0, bytes = 0;
    AZStd::atomic_bool published{false};
};
MeshController::MeshController() = default;
MeshController::MeshController(const MeshConfig& config) : m_configuration(config) {}
MeshController::~MeshController() = default;
void MeshConfig::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
    {
        sc->Class<MeshMaterialBinding>()->Version(1)->Field("MaterialIdentity", &MeshMaterialBinding::m_id)->Field("Material", &MeshMaterialBinding::m_material);
        sc->Class<MeshProfileBinding>()->Version(1)->Field("ProfileId", &MeshProfileBinding::m_id)->Field("Height", &MeshProfileBinding::m_height)
            ->Field("ScaleMetres", &MeshProfileBinding::m_scale)->Field("Reference", &MeshProfileBinding::m_reference)
            ->Field("Tiling", &MeshProfileBinding::m_tiling)->Field("Offset", &MeshProfileBinding::m_offset)->Field("AddressMode", &MeshProfileBinding::m_addressMode);
        sc->Class<MeshConfig, AZ::ComponentConfig>()->Version(1)->Field("Surface", &MeshConfig::m_surface)
            ->Field("Materials", &MeshConfig::m_materials)->Field("Profiles", &MeshConfig::m_profiles)
            ->Field("MaxNodes", &MeshConfig::m_maxNodes)->Field("MaxFragments", &MeshConfig::m_maxFragments)->Field("DepthAccuracy", &MeshConfig::m_depthAccuracy)
            ->Field("EnableExperimentalRaster", &MeshConfig::m_enableExperimentalRaster);
    }
}
void MeshController::Reflect(AZ::ReflectContext* context)
{
    MeshConfig::Reflect(context);
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<MeshController>()->Version(1)->Field("Configuration", &MeshController::m_configuration);
    if (auto* bc = azrtti_cast<AZ::BehaviorContext*>(context))
        bc->EBus<MeshRequestBus>("SilPomMeshRequestBus")->Attribute(AZ::Script::Attributes::Module, "silpom")
            ->Attribute(AZ::Script::Attributes::Scope, AZ::Script::Attributes::ScopeFlags::Common)
            ->Event("GetStatus", &MeshRequests::GetStatus)->Event("IsReady", &MeshRequests::IsReady)
            ->Event("GetHitOffsets", &MeshRequests::GetHitOffsets);
}
void MeshComponent::Reflect(AZ::ReflectContext* context)
{
    BaseClass::Reflect(context);
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<MeshComponent, BaseClass>()->Version(1);
}
void MeshController::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& s) { s.push_back(AZ_CRC_CE("MeshService")); s.push_back(AZ_CRC_CE("SilPomMeshService")); }
void MeshController::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& s) { s.push_back(AZ_CRC_CE("MeshService")); s.push_back(AZ_CRC_CE("NonUniformScaleService")); }
void MeshController::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& s) { s.push_back(AZ_CRC_CE("TransformService")); }
bool MeshController::IsReady() const
{
    // A retained last-known-good generation is visible, but is not readiness
    // for a newly requested configuration which failed validation.
    return m_active && !m_dirty && !m_pending && m_status.starts_with("Ready:");
}
AZStd::vector<AZ::u32> MeshController::GetHitOffsets() const
{
    AZStd::vector<AZ::u32> offsets;
    if (m_active) for (const auto& batch : m_active->computeBatches) offsets.push_back(batch->slot*MeshRenderFeatureProcessor::BatchBytes);
    return offsets;
}
void MeshController::ConnectAssets()
{
    AZ::Data::AssetBus::MultiHandler::BusDisconnect();
    AZStd::set<AZ::Data::AssetId> connected;
    auto queue = [&](auto& asset)
    {
        if (asset.GetId().IsValid() && connected.insert(asset.GetId()).second)
        { AZ::Data::AssetBus::MultiHandler::BusConnect(asset.GetId()); asset.QueueLoad(); }
    };
    queue(m_configuration.m_surface); queue(m_template);
    for (auto& binding : m_configuration.m_materials) queue(binding.m_material);
    for (auto& profile : m_configuration.m_profiles) queue(profile.m_height);
}
void MeshController::Activate(AZ::EntityId entity)
{
    m_entity = entity; m_dirty = true;
    AZ::TransformBus::EventResult(m_world, entity, &AZ::TransformBus::Events::GetWorldTM);
    AZ::Data::AssetId materialId;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(materialId, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
        "materials/silpommesh.template.azmaterial", azrtti_typeid<AZ::RPI::MaterialAsset>(), false);
    if (materialId.IsValid()) m_template = AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(materialId, AZ::Data::AssetLoadBehavior::NoLoad);
    ConnectAssets();
    AZ::TransformNotificationBus::Handler::BusConnect(entity); MeshRequestBus::Handler::BusConnect(entity); AZ::TickBus::Handler::BusConnect();
}
void MeshController::Release(AZStd::unique_ptr<Generation>& generation)
{
    if (generation) for (const auto& batch : generation->computeBatches) batch->registered = false;
    if (generation && m_processor) for (auto& handle : generation->handles) if (handle.IsValid()) m_processor->ReleaseMesh(handle);
    generation.reset();
}
void MeshController::Deactivate()
{
    AZ::TickBus::Handler::BusDisconnect(); MeshRequestBus::Handler::BusDisconnect();
    AZ::TransformNotificationBus::Handler::BusDisconnect(); AZ::Data::AssetBus::MultiHandler::BusDisconnect();
    Release(m_pending); Release(m_active); m_processor = nullptr; m_entity.SetInvalid(); m_status = "Inactive";
}
void MeshController::SetConfiguration(const MeshConfig& config)
{
    m_configuration = config; m_dirty = true;
    if (m_entity.IsValid())
    {
        Release(m_pending);
        if (!config.m_enableExperimentalRaster) Release(m_active);
        ConnectAssets();
    }
}
void MeshController::OnTransformChanged(const AZ::Transform&, const AZ::Transform& world)
{
    const bool scaleChanged = m_world.GetUniformScale() != world.GetUniformScale();
    m_world = world;
    if (scaleChanged) { m_dirty = true; return; }
    // Translation/rotation do not change the model-space surface. Scale does,
    // because displacement amplitude is measured in world metres.
    auto rigid = world; rigid.SetUniformScale(1);
    if (m_processor)
        for (auto* generation : {m_active.get(), m_pending.get()})
            if (generation)
            {
                for (const auto& handle : generation->handles) m_processor->SetTransform(handle, rigid);
                for (const auto& batch : generation->computeBatches) batch->rigid = rigid;
            }
}
void MeshController::OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData> asset) { OnAssetReloaded(asset); }
void MeshController::OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData> asset)
{
    if (asset.GetId() == m_configuration.m_surface.GetId()) m_configuration.m_surface = asset;
    if (asset.GetId() == m_template.GetId()) m_template = asset;
    for (auto& binding : m_configuration.m_materials) if (asset.GetId() == binding.m_material.GetId()) binding.m_material = asset;
    for (auto& profile : m_configuration.m_profiles) if (asset.GetId() == profile.m_height.GetId()) profile.m_height = asset;
    m_dirty = true;
}
void MeshController::OnAssetError(AZ::Data::Asset<AZ::Data::AssetData>)
{ Release(m_pending); m_dirty = false; m_status = "Asset load failed; previous complete generation retained. Check Asset Processor."; }

bool MeshController::Prepare()
{
    auto& c = m_configuration;
    if (!c.m_surface.GetId().IsValid()) { m_status = "Assign an imported .silpommesh surface"; return false; }
    if (!c.m_surface.IsReady()) { m_status = "Loading mesh surface"; return true; }
    if (!m_template.GetId().IsValid()) { m_status = "SilPOM mesh shader template is missing; process Gem assets"; return false; }
    if (!m_template.IsReady()) { m_status = "Loading mesh material template"; return true; }
    const auto& asset = *c.m_surface;
    if (asset.m_surfaceVersion != MeshSurfaceAsset::Version || asset.m_faces.empty()) { m_status = "Unsupported/empty mesh surface"; return false; }
    bool initialized = false;
    for (const auto& material : asset.m_materials)
        if (AZStd::none_of(c.m_materials.begin(), c.m_materials.end(), [&](const auto& b) { return b.m_id == material.m_id; }))
        { MeshMaterialBinding binding; binding.m_id = material.m_id; c.m_materials.push_back(binding); initialized = true; }
    for (const auto& face : asset.m_faces)
        if (face.m_region && AZStd::none_of(c.m_profiles.begin(), c.m_profiles.end(), [&](const auto& p) { return p.m_id == face.m_profile; }))
        { MeshProfileBinding profile; profile.m_id = face.m_profile; c.m_profiles.push_back(profile); initialized = true; }
    if (initialized) { m_status = "Material slots and profiles discovered; assign their materials and height images"; return false; }
    AZStd::map<AZStd::string, AZ::Data::Asset<AZ::RPI::MaterialAsset>> materialAssets;
    AZStd::map<AZ::u32, MeshProfileBinding> profiles;
    for (const auto& b : c.m_materials)
    {
        if (!materialAssets.emplace(b.m_id, b.m_material).second) { m_status = "Duplicate material binding"; return false; }
        if (!b.m_material.GetId().IsValid()) { m_status = "Assign material: " + b.m_id; return false; }
        if (!b.m_material.IsReady()) { m_status = "Loading materials"; return true; }
    }
    for (const auto& p : c.m_profiles)
    {
        if (!profiles.emplace(p.m_id, p).second || p.m_id == 0) { m_status = "Duplicate/zero profile ID"; return false; }
        if (!p.m_height.GetId().IsValid()) { m_status = AZStd::string::format("Assign height texture for profile %u", p.m_id); return false; }
        if (!p.m_height.IsReady()) { m_status = "Loading height textures"; return true; }
    }
    const float scale = m_world.GetUniformScale();
    if (!c.m_enableExperimentalRaster)
    { m_status = "Experimental raster disabled: staged compute renderer remains an opt-in integration; see Docs/ImportedMeshIntegration.md"; return false; }
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(c.m_depthAccuracy) || c.m_depthAccuracy <= 0
        || c.m_maxNodes < 1 || c.m_maxNodes > 65536 || c.m_maxFragments < 1 || c.m_maxFragments > 1048576)
    { m_status = "Invalid scale, depth accuracy or traversal budget"; return false; }
    auto* scene = AZ::RPI::Scene::GetSceneForEntityId(m_entity);
    if (!scene) { m_status = "Waiting for render scene"; return true; }
    m_processor = scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>();
    if (!m_processor) { m_status = "Waiting for MeshFeatureProcessor"; return true; }
    auto* compute = scene->GetFeatureProcessor<MeshRenderFeatureProcessor>();
    if (!compute) compute = scene->EnableFeatureProcessor<MeshRenderFeatureProcessor>();
    if (!compute) { m_status = "Staged mesh feature processor unavailable"; return false; }
    if (AZ::Render::r_meshInstancingEnabled)
    { m_status = "Experimental mesh renderer requires r_meshInstancingEnabled=false"; return false; }
    auto candidate = AZStd::make_unique<Generation>(); candidate->id = asset.m_generation;
    auto fail = [&](const AZStd::string& message) { Release(candidate); m_status = message; return false; };
    AZ::Transform rigid = m_world; rigid.SetUniformScale(1);
    using Key = AZStd::pair<AZStd::string, AZ::u32>;
    AZStd::map<Key, AZStd::vector<AZ::u32>> groups;
    for (AZ::u32 i = 0; i < asset.m_faces.size(); ++i)
    {
        const auto& f = asset.m_faces[i];
        if (f.m_corners.size() != 3 || !materialAssets.contains(f.m_materialId) || (f.m_region && !profiles.contains(f.m_profile)))
            return fail("Invalid surface/material/profile binding; reimport the mesh");
        groups[{f.m_materialId, f.m_region ? f.m_profile : 0}].push_back(i);
    }
    try
    {
        for (const auto& [key, faces] : groups)
        {
            auto source = Snapshot(materialAssets[key.first]);
            if (!source) return fail("Cannot create authored material");
            Material material = source;
            AZStd::vector<Vertex> vertices; AZStd::vector<AZ::u32> indices;
            AZ::Aabb bounds = AZ::Aabb::CreateNull();
            if (key.second == 0)
            {
                for (auto index : faces) for (const auto& corner : asset.m_faces[index].m_corners)
                {
                    indices.push_back(AZ::u32(vertices.size()));
                    vertices.push_back({corner.m_position * scale, corner.m_normal, corner.m_uv});
                    bounds.AddPoint(corner.m_position * scale);
                }
            }
            else
            {
                AZ::Data::AssetId standardType;
                AZ::Data::AssetCatalogRequestBus::BroadcastResult(standardType, &AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
                    "materials/types/standardpbr_generated.azmaterialtype", azrtti_typeid<AZ::RPI::MaterialTypeAsset>(), false);
                const auto opacity = source->FindPropertyIndex(AZ::Name("opacity.mode"));
                if (!standardType.IsValid() || materialAssets[key.first]->GetMaterialTypeAsset().GetId() != standardType
                    || !opacity.IsValid() || !source->GetPropertyValue(opacity).Is<AZ::u32>()
                    || source->GetPropertyValue(opacity).GetValue<AZ::u32>() != 0)
                    return fail("Displaced batches currently require opaque StandardPBR materials");
                const auto& p = profiles[key.second];
                if (!std::isfinite(p.m_scale) || !std::isfinite(p.m_reference) || p.m_reference < 0 || p.m_reference > 1
                    || !p.m_tiling.IsFinite() || !p.m_offset.IsFinite() || p.m_addressMode > 1
                    || std::abs(p.m_tiling.GetX()) < 1e-8f || std::abs(p.m_tiling.GetY()) < 1e-8f)
                    return fail("Invalid displacement profile settings");
                const auto& desc = p.m_height->GetImageDescriptor();
                if (desc.m_mipLevels != 1 || desc.m_arraySize != 1 || desc.m_size.m_depth != 1
                    || (desc.m_format != AZ::RHI::Format::R32_FLOAT && desc.m_format != AZ::RHI::Format::R16_UNORM && desc.m_format != AZ::RHI::Format::R8_UNORM))
                    return fail("Height must be normalized linear, single-mip R32_FLOAT/R16_UNORM/R8_UNORM");
                Curved::Texture texture; texture.width = desc.m_size.m_width; texture.height = desc.m_size.m_height;
                auto pixels = p.m_height->GetSubImageData(0, 0);
                const size_t pixelCount = size_t(texture.width) * texture.height;
                const size_t stride = desc.m_format == AZ::RHI::Format::R32_FLOAT ? 4 : desc.m_format == AZ::RHI::Format::R16_UNORM ? 2 : 1;
                if (pixelCount == 0 || pixels.size() != pixelCount * stride) return fail("Height pixel storage is unavailable or unsupported");
                texture.pixels.resize(pixelCount);
                for (size_t i = 0; i < pixelCount; ++i)
                {
                    if (stride == 4) { float value; memcpy(&value, pixels.data() + i * 4, 4); texture.pixels[i] = value; }
                    else if (stride == 2) { uint16_t value; memcpy(&value, pixels.data() + i * 2, 2); texture.pixels[i] = double(value) / 65535; }
                    else texture.pixels[i] = double(pixels[i]) / 255;
                }
                std::vector<Curved::Triangle> triangles;
                size_t estimatedFragments = 0;
                for (auto index : faces)
                {
                    Curved::Triangle triangle; triangle.primitiveId = index;
                    for (unsigned k = 0; k != 3; ++k)
                    {
                        const auto& corner = asset.m_faces[index].m_corners[k];
                        triangle.position[k] = Vector(corner.m_position); triangle.direction[k] = Vector(corner.m_direction);
                        auto uv = corner.m_uv * p.m_tiling + p.m_offset; triangle.uv[k] = {uv.GetX(), uv.GetY()};
                        bounds.AddPoint(corner.m_position * scale + corner.m_direction * (-p.m_scale * p.m_reference));
                        bounds.AddPoint(corner.m_position * scale + corner.m_direction * (p.m_scale * (1 - p.m_reference)));
                    }
                    double lowU = 1e300, lowV = 1e300, highU = -1e300, highV = -1e300;
                    for (const auto& uv : triangle.uv)
                    {
                        lowU = std::min(lowU, std::floor(uv.x * texture.width - .5)); highU = std::max(highU, std::floor(uv.x * texture.width - .5));
                        lowV = std::min(lowV, std::floor(uv.y * texture.height - .5)); highV = std::max(highV, std::floor(uv.y * texture.height - .5));
                    }
                    const double estimate = 5 * (highU - lowU + 1) * (highV - lowV + 1);
                    if (!std::isfinite(estimate) || estimate > c.m_maxFragments || estimatedFragments + estimate > c.m_maxFragments)
                        return fail("Eager traversal preprocessing exceeds budget; reduce height resolution/tiling (hierarchical traversal required for larger inputs)");
                    estimatedFragments += size_t(estimate);
                    triangles.push_back(triangle);
                }
                Curved::Surface surface; surface.baseScale = scale; surface.amplitude = p.m_scale; surface.reference = p.m_reference; surface.addressMode = p.m_addressMode;
                auto packed = Curved::PackMesh(triangles, surface, texture, c.m_maxFragments, true);
                if (packed.fragmentCount + candidate->fragments > c.m_maxFragments) return fail("Mesh exceeds fragment budget; reduce height resolution or UV tiling");
                candidate->fragments += packed.fragmentCount; candidate->bytes += packed.data.size() * sizeof(Curved::PackedFloat4);
                auto data = Buffer(packed.data.data(), packed.data.size(), sizeof(Curved::PackedFloat4), true);
                auto buffer = AZ::RPI::Buffer::FindOrCreate(data); if (!buffer) return fail("Cannot allocate traversal buffer");
                buffer->WaitForUpload();
                auto bindless = buffer->GetBufferView()->GetBindlessReadIndex();
                if (bindless.size() != 1 || bindless.begin()->second == uint32_t(-1)) return fail("Mesh integration currently requires one bindless-capable GPU");
                material = Snapshot(m_template); if (!material) return fail("Cannot create curved material variant");
                // Authoring properties retain their names across StandardPBR and
                // the internal curved variant. Geometry settings are owned here.
                const auto layout = source->GetMaterialPropertiesLayout();
                for (size_t propertyIndex = 0; propertyIndex < layout->GetPropertyCount(); ++propertyIndex)
                {
                    const auto* property = layout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(propertyIndex));
                    auto from = source->FindPropertyIndex(property->GetName()), to = material->FindPropertyIndex(property->GetName());
                    if (to.IsValid()) material->SetPropertyValue(to, source->GetPropertyValue(from));
                }
                bool valid = Set(material, "mesh.bufferIndex", bindless.begin()->second)
                    && Set(material, "mesh.triangleCount", packed.ShaderHeader()) && Set(material, "mesh.fragmentCount", packed.fragmentCount)
                    && Set(material, "mesh.baseScale", scale) && Set(material, "mesh.scale", p.m_scale) && Set(material, "mesh.reference", p.m_reference)
                    && Set(material, "mesh.textureWidth", texture.width) && Set(material, "mesh.textureHeight", texture.height)
                    && Set(material, "mesh.addressMode", p.m_addressMode) && Set(material, "mesh.maxNodes", c.m_maxNodes)
                    && Set(material, "mesh.depthAccuracy", c.m_depthAccuracy) && Set(material, "mesh.boundsMin", bounds.GetMin())
                    && Set(material, "mesh.boundsMax", bounds.GetMax()) && Set(material, "general.doubleSided", true);
                auto image = AZ::RPI::StreamingImage::FindOrCreate(p.m_height);
                AZ::Data::Instance<AZ::RPI::Image> height = image;
                valid = valid && Set(material, "mesh.heightMap", height);
                auto batch = AZStd::make_shared<MeshRenderBatch>();
                batch->geometry = buffer; batch->height = height; batch->rigid = rigid; batch->bounds = bounds;
                batch->surface = AZ::Vector4(scale,p.m_scale,p.m_reference,c.m_depthAccuracy);
                batch->uvTransform = AZ::Vector4(p.m_tiling.GetX(),p.m_tiling.GetY(),p.m_offset.GetX(),p.m_offset.GetY());
                batch->counts[0]=texture.width; batch->counts[1]=texture.height;
                batch->counts[2]=packed.fragmentCount; batch->counts[3]=packed.ShaderHeader();
                batch->maxNodes = c.m_maxNodes; batch->addressMode = p.m_addressMode;
                if (!compute->Acquire(batch)) return fail("Staged renderer allocation failed or 8-batch generation budget exceeded");
                valid = valid && Set(material,"mesh.hitBufferIndex",compute->GetBindlessIndex())
                    && Set(material,"mesh.hitOffset",batch->slot*MeshRenderFeatureProcessor::BatchBytes);
                candidate->computeBatches.push_back(AZStd::move(batch));
                if (!valid) return fail("Curved material shader contract mismatch");
                candidate->buffers.push_back(buffer); candidate->heights.push_back(height);
                vertices = {{AZ::Vector3(-1,-1,0),AZ::Vector3::CreateAxisZ(),AZ::Vector2(0,0)},
                    {AZ::Vector3(1,-1,0),AZ::Vector3::CreateAxisZ(),AZ::Vector2(1,0)},
                    {AZ::Vector3(-1,1,0),AZ::Vector3::CreateAxisZ(),AZ::Vector2(0,1)},
                    {AZ::Vector3(1,1,0),AZ::Vector3::CreateAxisZ(),AZ::Vector2(1,1)}};
                indices = {0,1,2,2,1,3}; bounds.Expand(AZ::Vector3(1e-5f));
            }
            material->Compile();
            auto model = Model(vertices, indices, bounds); if (!model) return fail("Cannot create generation geometry");
            AZ::Render::MeshHandleDescriptor descriptor(model, material);
            descriptor.m_entityId = m_entity; descriptor.m_isRayTracingEnabled = false; descriptor.m_excludeFromReflectionCubeMaps = true;
            auto handle = m_processor->AcquireMesh(descriptor); if (!handle.IsValid()) return fail("Mesh acquisition failed");
            // Atom does not initialize invisible meshes. Allow initialization,
            // but disable every new draw item in its update notification before
            // the subsequent culling/submission phase. Never draw a partial
            // generation while waiting for the other batches to become ready.
            m_processor->SetTransform(handle, rigid); m_processor->SetLocalAabb(handle, bounds);
            const size_t batchIndex = candidate->handles.size();
            candidate->models.push_back(model); candidate->materials.push_back(material); candidate->handles.push_back(AZStd::move(handle));
            const auto motionTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
            auto handler = AZStd::make_unique<Generation::PacketHandler>(
                [generation = candidate.get(), batchIndex, processor = m_processor, motionTag](
                    const AZ::Render::ModelDataInstanceInterface&, AZ::u32, AZ::u32, const AZ::RPI::MeshDrawPacket& packet)
                {
                    if (const auto* draw = packet.GetRHIDrawPacket())
                        for (size_t i = 0; i < draw->GetDrawItemCount(); ++i)
                        {
                            const auto tag = draw->GetDrawListTag(i);
                            processor->SetDrawItemEnabled(generation->handles[batchIndex], tag,
                                generation->published.load() && tag != motionTag);
                        }
                });
            candidate->handles.back()->ConnectMeshDrawPacketUpdatedHandler(*handler);
            candidate->packetHandlers.push_back(AZStd::move(handler));
        }
    }
    catch (const std::exception& error) { return fail(AZStd::string("Surface preparation rejected: ") + error.what()); }
    m_pending = AZStd::move(candidate); m_status = "Preparing complete mesh generation"; return false;
}
void MeshController::OnTick(float, AZ::ScriptTimePoint)
{
    if (m_dirty) { Release(m_pending); m_dirty = Prepare(); }
    if (m_pending)
    {
        bool ready = true;
        for (const auto& batch : m_pending->computeBatches)
        {
            ready = ready && batch->prepared && batch->error.empty();
            if (!batch->error.empty()) m_status = "Staged renderer: " + batch->error;
        }
        for (const auto& material : m_pending->materials)
        {
            if (material->NeedsCompile()) material->Compile();
            ready = ready && !material->NeedsCompile();
        }
        for (const auto& handle : m_pending->handles)
        {
            ready = ready && bool(m_processor->GetModel(handle));
            const auto& lods = m_processor->GetDrawPackets(handle);
            ready = ready && !lods.empty() && !lods.front().empty();
            for (const auto& lod : lods) for (const auto& packet : lod)
                ready = ready && packet.GetRHIDrawPacket() && packet.GetRHIDrawPacket()->GetDrawItemCount() != 0;
        }
        if (ready)
        {
            // All handles reference private immutable model/buffer assets. Stock
            // source-model reloads cannot replace just the ordinary half.
            if (m_active) for (const auto& handle : m_active->handles) m_processor->SetVisible(handle, false);
            m_pending->published.store(true);
            const auto motionTag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
            for (const auto& handle : m_pending->handles)
                for (const auto& lod : m_processor->GetDrawPackets(handle)) for (const auto& packet : lod)
                    for (size_t i = 0; i < packet.GetRHIDrawPacket()->GetDrawItemCount(); ++i)
                    {
                        const auto tag = packet.GetRHIDrawPacket()->GetDrawListTag(i);
                        m_processor->SetDrawItemEnabled(handle, tag, tag != motionTag);
                    }
            Release(m_active); m_active = AZStd::move(m_pending);
            m_status = AZStd::string::format("Ready: %zu batches, %zu fragments, %zu traversal bytes; staged compute curved raster", m_active->handles.size(), m_active->fragments, m_active->bytes);
        }
    }
    if (m_active)
    {
        bool valid = true;
        for (const auto& batch : m_active->computeBatches) if (!batch->error.empty())
        { valid = false; m_status = "Staged renderer: " + batch->error; }
        if (valid && !m_dirty && !m_pending)
        {
            AZ::u32 budget = 0, stride = 1, rays = 0;
            for (const auto& batch : m_active->computeBatches)
            {
                budget = AZStd::max(budget, batch->previewBudget);
                stride = AZStd::max(stride, batch->previewStride); rays += batch->previewRays;
            }
            if (budget)
                m_status = AZStd::string::format("Ready: APPROXIMATE preview; up to %ux%u pixel blocks, %u sampled rays, %u budget/batch", stride, stride, rays, budget);
            else if (m_status.starts_with("Staged renderer:") || m_status.starts_with("Ready: APPROXIMATE"))
                m_status = "Ready: staged compute curved raster";
        }
        const auto tag = AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
        for (const auto& handle : m_active->handles) if (tag.IsValid()) m_processor->SetDrawItemEnabled(handle, tag, false);
    }
}
}
