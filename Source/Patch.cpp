// SPDX-License-Identifier: MIT
#include <SilPOM/Patch.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
#include <Atom/RPI.Reflect/Material/MaterialAssetCreator.h>
#include <Atom/RPI.Reflect/Material/MaterialPropertiesLayout.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Asset/AssetSerializer.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <cmath>

namespace SilPOM
{
namespace
{
AZ::Data::Asset<AZ::RPI::ModelAsset> CreateQuad()
{
    auto buffer=[](const void* data,AZ::u32 count,AZ::u32 stride)
    {
        AZ::RPI::BufferAssetCreator creator;
        creator.Begin(AZ::Uuid::CreateRandom());
        AZ::RHI::BufferDescriptor descriptor;
        descriptor.m_bindFlags=AZ::RHI::BufferBindFlags::InputAssembly | AZ::RHI::BufferBindFlags::ShaderRead;
        descriptor.m_byteCount=size_t(count)*stride;
        creator.SetBuffer(data,descriptor.m_byteCount,descriptor);
        creator.SetBufferViewDescriptor(AZ::RHI::BufferViewDescriptor::CreateStructured(0,count,stride));
        creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
        AZ::Data::Asset<AZ::RPI::BufferAsset> result;
        creator.End(result); return result;
    };
    const float positions[]={-1,-1,0,1,-1,0,-1,1,0,1,1,0};
    const float normals[]={0,0,1,0,0,1,0,0,1,0,0,1};
    const float tangents[]={1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1};
    const float uvs[]={0,0,1,0,0,1,1,1};
    const AZ::u32 indices[]={0,1,2,2,1,3};
    const auto index=buffer(indices,6,4);
    if(!index) return {};
    AZ::RPI::ModelLodAssetCreator lod;
    lod.Begin(AZ::Uuid::CreateRandom()); lod.SetLodIndexBuffer(index); lod.BeginMesh();
    lod.SetMeshAabb(AZ::Aabb::CreateFromMinMax(AZ::Vector3(-1,-1,-.1f),AZ::Vector3(1,1,.1f)));
    lod.SetMeshMaterialSlot(0);
    lod.SetMeshIndexBuffer({index,AZ::RHI::BufferViewDescriptor::CreateTyped(0,6,AZ::RHI::Format::R32_UINT)});
    auto stream=[&](const char* name,AZ::u32 semanticIndex,const float* values,AZ::u32 stride)
    {
        auto asset=buffer(values,4,stride);
        if(!asset) return false;
        lod.AddLodStreamBuffer(asset);
        const auto format=stride==8?AZ::RHI::Format::R32G32_FLOAT:
            stride==12?AZ::RHI::Format::R32G32B32_FLOAT:AZ::RHI::Format::R32G32B32A32_FLOAT;
        return lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(AZ::Name(name),semanticIndex),AZ::Name(),
            {asset,AZ::RHI::BufferViewDescriptor::CreateTyped(0,4,format)});
    };
    if(!stream("POSITION",0,positions,12) || !stream("NORMAL",0,normals,12)
        || !stream("TANGENT",0,tangents,16) || !stream("UV",0,uvs,8) || !stream("UV",1,uvs,8)) return {};
    lod.EndMesh();
    AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset;
    if(!lod.End(lodAsset)) return {};
    AZ::RPI::ModelAssetCreator model;
    model.Begin(AZ::Uuid::CreateRandom()); model.SetName("SilPOM conservative coverage");
    AZ::RPI::ModelMaterialSlot slot; slot.m_stableId=0; slot.m_displayName=AZ::Name("SilPOM");
    model.AddMaterialSlot(slot); model.AddLodAsset(AZStd::move(lodAsset));
    AZ::Data::Asset<AZ::RPI::ModelAsset> result; model.End(result); return result;
}
}
void PatchConfig::Reflect(AZ::ReflectContext* context)
{
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
        sc->Class<PatchConfig,AZ::ComponentConfig>()->Version(2)
            ->Field("Material",&PatchConfig::m_material)->Field("Width",&PatchConfig::m_width)
            ->Field("Height",&PatchConfig::m_height)->Field("HeightScaleMetres",&PatchConfig::m_heightScale)
            ->Field("ReferenceHeight",&PatchConfig::m_reference)->Field("TileU",&PatchConfig::m_tileU)
            ->Field("TileV",&PatchConfig::m_tileV)->Field("OffsetU",&PatchConfig::m_offsetU)->Field("OffsetV",&PatchConfig::m_offsetV)
            ->Field("MaxCells",&PatchConfig::m_maxCells)->Field("AddressMode",&PatchConfig::m_addressMode)->Field("Debug",&PatchConfig::m_debug)
            ->Field("ReuseTexels",&PatchConfig::m_reuseTexels);
}
void PatchController::Reflect(AZ::ReflectContext* context)
{
    PatchConfig::Reflect(context);
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
        sc->Class<PatchController>()->Version(1)->Field("Configuration",&PatchController::m_configuration);
    if(auto* bc=azrtti_cast<AZ::BehaviorContext*>(context))
        bc->EBus<PatchRequestBus>("SilPomPatchRequestBus")
            ->Attribute(AZ::Script::Attributes::Module,"silpom")
            ->Attribute(AZ::Script::Attributes::Scope,AZ::Script::Attributes::ScopeFlags::Common)
            ->Event("GetStatus",&PatchRequests::GetStatus)->Event("IsReady",&PatchRequests::IsReady)
            ->Event("SetCameraVisible",&PatchRequests::SetCameraVisible)->Event("SetDebug",&PatchRequests::SetDebug);
}
void PatchComponent::Reflect(AZ::ReflectContext* context)
{
    BaseClass::Reflect(context);
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<PatchComponent,BaseClass>()->Version(1);
}
void PatchController::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& s) {s.push_back(AZ_CRC_CE("SilPomPatchService"));}
void PatchController::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& s)
{s.push_back(AZ_CRC_CE("SilPomPatchService"));s.push_back(AZ_CRC_CE("NonUniformScaleService"));}
void PatchController::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& s) {s.push_back(AZ_CRC_CE("TransformService"));}
void PatchController::Activate(AZ::EntityId id)
{
    m_entity=id; m_dirty=true;
    AZ::TransformBus::EventResult(m_world,id,&AZ::TransformBus::Events::GetWorldTM);
    AZ::TransformNotificationBus::Handler::BusConnect(id); PatchRequestBus::Handler::BusConnect(id);
    if(m_configuration.m_material.GetId().IsValid())
    {
        AZ::Data::AssetBus::Handler::BusConnect(m_configuration.m_material.GetId());
        m_configuration.m_material.QueueLoad();
    }
    AZ::TickBus::Handler::BusConnect();
}
void PatchController::ReleaseMesh()
{
    if(m_meshProcessor && m_mesh.IsValid()) m_meshProcessor->ReleaseMesh(m_mesh);
    m_material={}; m_model={}; m_meshProcessor=nullptr;
}
void PatchController::Deactivate()
{
    AZ::TickBus::Handler::BusDisconnect(); AZ::Data::AssetBus::Handler::BusDisconnect();
    AZ::TransformNotificationBus::Handler::BusDisconnect(); PatchRequestBus::Handler::BusDisconnect();
    ReleaseMesh(); m_entity.SetInvalid(); m_status="Inactive";
}
void PatchController::SetConfiguration(const PatchConfig& config)
{
    const auto id=m_entity; if(id.IsValid()) Deactivate();
    m_configuration=config; if(id.IsValid()) Activate(id);
}
void PatchController::OnTransformChanged(const AZ::Transform&,const AZ::Transform& world) {m_world=world;m_dirty=true;}
void PatchController::OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData> asset) {m_configuration.m_material=asset;m_dirty=true;}
void PatchController::OnAssetError(AZ::Data::Asset<AZ::Data::AssetData>) {ReleaseMesh();m_status="Material load failed";m_dirty=false;}

bool PatchController::Prepare()
{
    const auto& c=m_configuration;
    const float scale=m_world.GetUniformScale();
    const float values[]={c.m_width,c.m_height,c.m_heightScale,c.m_reference,c.m_tileU,c.m_tileV,c.m_offsetU,c.m_offsetV,scale};
    for(float value:values) if(!std::isfinite(value)) {m_status="Non-finite patch configuration";return false;}
    if(c.m_width<=0 || c.m_height<=0 || scale<=0 || c.m_reference<0 || c.m_reference>1
        || c.m_maxCells<1 || c.m_maxCells>65536 || c.m_addressMode>1
        || std::abs(c.m_tileU)>256 || std::abs(c.m_tileV)>256 || std::abs(c.m_offsetU)>4096 || std::abs(c.m_offsetV)>4096)
    {m_status="Invalid extent, scale, reference, UV range, or traversal budget";return false;}
    auto* scene=AZ::RPI::Scene::GetSceneForEntityId(m_entity);
    if(!scene) {m_status="Waiting for render scene";return false;}
    m_meshProcessor=scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>();
    if(!m_meshProcessor) {m_status="Mesh feature processor unavailable";return false;}
    const auto& type=c.m_material->GetMaterialTypeAsset();
    const auto layout=type->GetMaterialPropertiesLayout();
    const auto& properties=c.m_material->GetPropertyValues();
    const AZ::Name doubleSided("general.doubleSided");
    const auto doubleSidedIndex=layout->FindPropertyIndex(doubleSided);
    if(!doubleSidedIndex.IsValid() || !properties[doubleSidedIndex.GetIndex()].Is<bool>())
    {m_status="SilPOM material contract mismatch";return false;}
    // Initialize the final rasterizer state with the asset. Changing it on the
    // live instance would make the double-sided functor request a runtime PSO change.
    AZ::RPI::MaterialAssetCreator creator;
    creator.Begin(AZ::Uuid::CreateRandom(),type);
    creator.SetMaterialTypeVersion(type->GetVersion());
    for(size_t i=0;i<properties.size();++i)
        creator.SetPropertyValue(layout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(i))->GetName(),properties[i]);
    creator.SetPropertyValue(doubleSided,true);
    AZ::Data::Asset<AZ::RPI::MaterialAsset> materialAsset;
    if(!creator.End(materialAsset)) {m_status="Material asset creation failed";return false;}
    m_material=AZ::RPI::Material::Create(materialAsset);
    if(!m_material) {m_status="Material instance unavailable";return false;}
    auto heightIndex=m_material->FindPropertyIndex(AZ::Name("surface.heightMap"));
    if(!heightIndex.IsValid()) {m_status="Material is not a SilPOM surface";return false;}
    const auto& height=m_material->GetPropertyValue<AZ::Data::Instance<AZ::RPI::Image>>(heightIndex);
    if(!height) {m_status="Assign a linear single-mip height image";return false;}
    const auto& desc=height->GetRHIImage()->GetDescriptor();
    if(desc.m_mipLevels!=1 || (desc.m_format!=AZ::RHI::Format::R32_FLOAT && desc.m_format!=AZ::RHI::Format::R16_UNORM
        && desc.m_format!=AZ::RHI::Format::R8_UNORM))
    {m_status="Height image must be single-mip R32_FLOAT, R16_UNORM, or R8_UNORM (LUT preset)";return false;}
    const auto boundsIndex=m_material->FindPropertyIndex(AZ::Name("surface.heightBounds"));
    const auto& bounds=boundsIndex.IsValid()
        ? m_material->GetPropertyValue<AZ::Data::Instance<AZ::RPI::Image>>(boundsIndex)
        : AZ::Data::Instance<AZ::RPI::Image>();
    if(c.m_reuseTexels && !bounds)
    {m_status="Assign the conservative RG32F height-bounds atlas for accelerated traversal";return false;}
    bool propertiesValid=true;
    auto set=[&](const char* name,auto value)
    {
        const auto index=m_material->FindPropertyIndex(AZ::Name(name));
        if(!index.IsValid() || !m_material->GetPropertyValue(index).Is<decltype(value)>())
        {propertiesValid=false;return;}
        // SetPropertyValue returns whether the value changed, not whether it is valid.
        m_material->SetPropertyValue(index,value);
    };
    set("surface.width",c.m_width*scale);set("surface.height",c.m_height*scale);
    set("surface.scale",c.m_heightScale);set("surface.reference",c.m_reference);
    set("surface.tileU",c.m_tileU);set("surface.tileV",c.m_tileV);
    set("surface.offsetU",c.m_offsetU);set("surface.offsetV",c.m_offsetV);
    set("surface.maxCells",c.m_maxCells);set("surface.addressMode",c.m_addressMode);set("surface.debug",c.m_debug);
    set("surface.reuseTexels",c.m_reuseTexels);
    set("surface.useHierarchy",c.m_reuseTexels);
    if(!propertiesValid) {m_status="SilPOM material contract mismatch";return false;}
    m_material->Compile();
    m_model=CreateQuad();
    if(!m_model) {m_status="Coverage model creation failed";return false;}
    AZ::Render::MeshHandleDescriptor mesh(m_model,m_material);
    mesh.m_entityId=m_entity;mesh.m_isRayTracingEnabled=false;mesh.m_excludeFromReflectionCubeMaps=true;
    m_mesh=m_meshProcessor->AcquireMesh(mesh);
    if(!m_mesh.IsValid()) {m_status="Mesh acquisition failed";return false;}
    AZ::Transform rigid=m_world;rigid.SetUniformScale(1);
    m_meshProcessor->SetTransform(m_mesh,rigid);
    const float a=-c.m_heightScale*c.m_reference,b=c.m_heightScale*(1-c.m_reference);
    m_meshProcessor->SetLocalAabb(m_mesh,AZ::Aabb::CreateFromMinMax(
        AZ::Vector3(-c.m_width*scale*.5f,-c.m_height*scale*.5f,AZStd::min(a,b)-1e-5f),
        AZ::Vector3(c.m_width*scale*.5f,c.m_height*scale*.5f,AZStd::max(a,b)+1e-5f)));
    m_status=c.m_reuseTexels
        ? "Ready: conservative bounds + exact leaf traversal; full-resolution hit depth; flat quad shadows; RT disabled"
        : "Ready: frozen exact traversal baseline; full-resolution hit depth; flat quad shadows; RT disabled";
    return true;
}
void PatchController::SetDebug(AZ::u32 mode)
{
    m_configuration.m_debug=AZStd::min(mode,5u);
    if(m_material)
    {
        m_material->SetPropertyValue(m_material->FindPropertyIndex(AZ::Name("surface.debug")),m_configuration.m_debug);
        m_material->Compile();
    }
}
void PatchController::OnTick(float,AZ::ScriptTimePoint)
{
    if(m_dirty)
    {
        if(!m_configuration.m_material.GetId().IsValid()) {ReleaseMesh();m_status="Assign a SilPOM material";m_dirty=false;return;}
        if(!m_configuration.m_material.IsReady()) {m_status="Loading material";return;}
        ReleaseMesh();Prepare();
        m_dirty=m_status=="Waiting for render scene" || m_status=="Mesh feature processor unavailable";
    }
    if(m_mesh.IsValid())
    {
        // Reapply after asynchronous draw-packet creation/rebuilds. Static camera motion uses hit depth.
        auto* rhi=AZ::RHI::RHISystemInterface::Get();
        const auto tag=rhi->GetDrawListTagRegistry()->FindTag(AZ::Name("motion"));
        if(tag.IsValid()) m_meshProcessor->SetDrawItemEnabled(m_mesh,tag,false);
        for(const char* name:{"depth","forward"})
        {
            const auto cameraTag=rhi->GetDrawListTagRegistry()->FindTag(AZ::Name(name));
            if(cameraTag.IsValid()) m_meshProcessor->SetDrawItemEnabled(m_mesh,cameraTag,m_cameraVisible);
        }
        m_material->Compile();
    }
}
}
