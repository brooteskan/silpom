// SPDX-License-Identifier: MIT
#include "PlanarBenchmark.h"
#include <SilPOM/Patch.h>
#include <AzCore/Asset/AssetManager.h>
#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzFramework/Asset/AssetCatalogBus.h>
#include <AzFramework/Font/FontInterface.h>
#include <AzCore/Interface/Interface.h>
#include <Atom/RHI/RHISystemInterface.h>
#include <Atom/RHI/Device.h>
#include <Atom/RHI/PhysicalDevice.h>
#include <Atom/RHI/Scope.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/ViewportContext.h>
#include <Atom/RPI.Public/ViewportContextBus.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Pass/ParentPass.h>
#include <Atom/RPI.Public/Pass/RenderPass.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelAssetCreator.h>
#include <Atom/RPI.Reflect/Model/ModelLodAssetCreator.h>
#include <AzCore/JSON/document.h>
#include <AzCore/JSON/stringbuffer.h>
#include <AzCore/JSON/writer.h>
#include <cmath>

namespace SilPOM
{
namespace
{
AZ::Data::Asset<AZ::RPI::ModelAsset> CreateGrid(AZ::u32 cells,float width,float height)
{
    const AZ::u32 count=(cells+1)*(cells+1);
    AZStd::vector<float> positions,normals,tangents,uvs;
    AZStd::vector<AZ::u32> indices;
    positions.reserve(count*3); normals.reserve(count*3); tangents.reserve(count*4); uvs.reserve(count*2);
    indices.reserve(cells*cells*6);
    for(AZ::u32 y=0;y<=cells;++y) for(AZ::u32 x=0;x<=cells;++x)
    {
        const float u=float(x)/cells,v=float(y)/cells;
        positions.insert(positions.end(),{(u-.5f)*width,(v-.5f)*height,0});
        normals.insert(normals.end(),{0,0,1}); tangents.insert(tangents.end(),{1,0,0,1});
        uvs.insert(uvs.end(),{u,v});
        if(x<cells && y<cells)
        {
            const AZ::u32 a=y*(cells+1)+x,b=a+1,c=a+cells+1,d=c+1;
            indices.insert(indices.end(),{a,b,c,c,b,d});
        }
    }
    auto buffer=[](const void* data,AZ::u32 size,AZ::u32 stride)
    {
        AZ::RPI::BufferAssetCreator creator; creator.Begin(AZ::Uuid::CreateRandom());
        AZ::RHI::BufferDescriptor descriptor;
        descriptor.m_bindFlags=AZ::RHI::BufferBindFlags::InputAssembly|AZ::RHI::BufferBindFlags::ShaderRead;
        descriptor.m_byteCount=AZ::u64(size)*stride;
        creator.SetBuffer(data,descriptor.m_byteCount,descriptor);
        creator.SetBufferViewDescriptor(AZ::RHI::BufferViewDescriptor::CreateStructured(0,size,stride));
        creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
        AZ::Data::Asset<AZ::RPI::BufferAsset> result; creator.End(result); return result;
    };
    auto index=buffer(indices.data(),aznumeric_cast<AZ::u32>(indices.size()),4);
    if(!index) return {};
    AZ::RPI::ModelLodAssetCreator lod; lod.Begin(AZ::Uuid::CreateRandom());
    lod.SetLodIndexBuffer(index); lod.BeginMesh();
    lod.SetMeshAabb(AZ::Aabb::CreateFromMinMax(AZ::Vector3(-width*.5f,-height*.5f,-1),AZ::Vector3(width*.5f,height*.5f,1)));
    lod.SetMeshMaterialSlot(0);
    lod.SetMeshIndexBuffer({index,AZ::RHI::BufferViewDescriptor::CreateTyped(0,aznumeric_cast<AZ::u32>(indices.size()),AZ::RHI::Format::R32_UINT)});
    auto stream=[&](const char* name,AZ::u32 semantic,const AZStd::vector<float>& values,AZ::u32 stride,AZ::RHI::Format format)
    {
        auto asset=buffer(values.data(),count,stride); if(!asset) return false;
        lod.AddLodStreamBuffer(asset);
        return lod.AddMeshStreamBuffer(AZ::RHI::ShaderSemantic(AZ::Name(name),semantic),AZ::Name(),
            {asset,AZ::RHI::BufferViewDescriptor::CreateTyped(0,count,format)});
    };
    if(!stream("POSITION",0,positions,12,AZ::RHI::Format::R32G32B32_FLOAT)
        || !stream("NORMAL",0,normals,12,AZ::RHI::Format::R32G32B32_FLOAT)
        || !stream("TANGENT",0,tangents,16,AZ::RHI::Format::R32G32B32A32_FLOAT)
        || !stream("UV",0,uvs,8,AZ::RHI::Format::R32G32_FLOAT)
        || !stream("UV",1,uvs,8,AZ::RHI::Format::R32G32_FLOAT)) return {};
    lod.EndMesh(); AZ::Data::Asset<AZ::RPI::ModelLodAsset> lodAsset; if(!lod.End(lodAsset)) return {};
    AZ::RPI::ModelAssetCreator model; model.Begin(AZ::Uuid::CreateRandom()); model.SetName("SilPOM A/B reference grid");
    AZ::RPI::ModelMaterialSlot slot; slot.m_stableId=0; slot.m_displayName=AZ::Name("Reference");
    model.AddMaterialSlot(slot); model.AddLodAsset(AZStd::move(lodAsset));
    AZ::Data::Asset<AZ::RPI::ModelAsset> result; model.End(result); return result;
}
}

void PlanarBenchmark::Reflect(AZ::ReflectContext* context)
{
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context)) sc->Class<PlanarBenchmark,AZ::Component>()->Version(1);
    if(auto* bc=azrtti_cast<AZ::BehaviorContext*>(context))
        bc->EBus<PlanarBenchmarkRequestBus>("SilPomPlanarBenchmarkBus")
            ->Attribute(AZ::Script::Attributes::Module,"silpom")
            ->Attribute(AZ::Script::Attributes::Scope,AZ::Script::Attributes::ScopeFlags::Common)
            ->Event("CreateReference",&PlanarBenchmarkRequests::CreateReference)
            ->Event("IsReady",&PlanarBenchmarkRequests::IsReady)
            ->Event("SetReferenceVisible",&PlanarBenchmarkRequests::SetReferenceVisible)
            ->Event("SetOverlay",&PlanarBenchmarkRequests::SetOverlay)
            ->Event("BeginCapture",&PlanarBenchmarkRequests::BeginCapture)
            ->Event("IsCaptureDone",&PlanarBenchmarkRequests::IsCaptureDone)
            ->Event("EndCapture",&PlanarBenchmarkRequests::EndCapture)
            ->Event("Release",&PlanarBenchmarkRequests::Release);
}
void PlanarBenchmark::Activate()
{PlanarBenchmarkRequestBus::Handler::BusConnect(); AZ::TickBus::Handler::BusConnect();}
void PlanarBenchmark::Deactivate()
{Release(); AZ::TickBus::Handler::BusDisconnect(); PlanarBenchmarkRequestBus::Handler::BusDisconnect();}
bool PlanarBenchmark::CreateReference(AZ::EntityId patch,const AZStd::string& path,AZ::u32 cells,float width,float height)
{
    Release();
    if(!patch.IsValid() || cells<1 || cells>1024 || !std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0) return false;
    auto* scene=AZ::RPI::Scene::GetSceneForEntityId(patch); if(!scene) return false;
    m_processor=scene->GetFeatureProcessor<AZ::Render::MeshFeatureProcessorInterface>(); if(!m_processor) return false;
    AZ::Data::AssetId id;
    AZ::Data::AssetCatalogRequestBus::BroadcastResult(id,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
        path.c_str(),azrtti_typeid<AZ::RPI::MaterialAsset>(),false);
    if(!id.IsValid()) return false;
    auto asset=AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(id,AZ::Data::AssetLoadBehavior::PreLoad);
    asset.BlockUntilLoadComplete(); if(!asset.IsReady()) return false;
    m_material=AZ::RPI::Material::Create(asset); if(!m_material) return false;
    m_material->SetPropertyValue(m_material->FindPropertyIndex(AZ::Name("general.castShadows")),false); m_material->Compile();
    m_model=CreateGrid(cells,width,height); if(!m_model) return false;
    AZ::Render::MeshHandleDescriptor descriptor(m_model,m_material);
    descriptor.m_entityId=patch; descriptor.m_isRayTracingEnabled=false; descriptor.m_excludeFromReflectionCubeMaps=true;
    m_mesh=m_processor->AcquireMesh(descriptor); if(!m_mesh.IsValid()) return false;
    m_patch=patch;
    AZ::Transform world=AZ::Transform::CreateIdentity();
    AZ::TransformBus::EventResult(world,patch,&AZ::TransformBus::Events::GetWorldTM);
    m_processor->SetTransform(m_mesh,world); m_processor->SetVisible(m_mesh,false);
    return true;
}
bool PlanarBenchmark::IsReady() const
{return m_processor && m_mesh.IsValid() && bool(m_processor->GetModel(m_mesh));}
void PlanarBenchmark::SetReferenceVisible(bool visible)
{
    if(m_processor && m_mesh.IsValid()) m_processor->SetVisible(m_mesh,visible);
    if(m_patch.IsValid()) PatchRequestBus::Event(m_patch,&PatchRequests::SetCameraVisible,!visible);
}
bool PlanarBenchmark::BeginCapture(AZ::u32 samples)
{
    EndCapture(); if(!IsReady() || samples<1 || samples>10000) return false;
    auto* scene=AZ::RPI::Scene::GetSceneForEntityId(m_patch); if(!scene) return false;
    const auto pipeline=scene->GetDefaultRenderPipeline(); if(!pipeline) return false;
    AZStd::function<void(AZ::RPI::Pass*)> visit=[&](AZ::RPI::Pass* pass)
    {
        m_queries.push_back({pass,pass->IsTimestampQueryEnabled(),0});
        if(auto* parent=azrtti_cast<AZ::RPI::ParentPass*>(pass))
            for(const auto& child:parent->GetChildren()) visit(child.get());
    };
    visit(pipeline->GetRootPass().get());
    // ParentPass has no native timestamp of its own. Use the camera depth leaf
    // as the readback cadence, retaining all other leaf timings independently.
    m_anchor=aznumeric_cast<AZ::u32>(m_queries.size());
    for(AZ::u32 i=0;i<m_queries.size();++i)
        if(m_queries[i].pass->GetName()==AZ::Name("DepthPass")) {m_anchor=i;break;}
    if(m_anchor==m_queries.size()) {m_queries.clear();return false;}
    const auto* binding=m_queries[m_anchor].pass->FindAttachmentBinding(AZ::Name("Output"));
    if(!binding || !binding->GetAttachment()) {m_queries.clear();return false;}
    const auto& image=binding->GetAttachment()->m_descriptor.m_image;
    m_width=image.m_size.m_width; m_height=image.m_size.m_height; m_msaa=image.m_multisampleState.m_samples;
    m_hardware=AZ::RHI::RHISystemInterface::Get()->GetDevice()->GetPhysicalDevice().GetDescriptor().m_description;
    for(auto& query:m_queries) query.pass->SetTimestampQueryEnabled(true);
    m_requested=samples; m_frames=0; m_warmup=16;
    m_samples.reserve(size_t(samples)*m_queries.size());
    return true;
}
AZStd::string PlanarBenchmark::EndCapture()
{
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    out.StartObject(); out.Key("requested"); out.Uint(m_requested); out.Key("frames"); out.Uint(m_frames);
    out.Key("hardware"); out.String(m_hardware.c_str());
    out.Key("depth_attachment"); out.StartObject();
    out.Key("width"); out.Uint(m_width); out.Key("height"); out.Uint(m_height); out.Key("msaa"); out.Uint(m_msaa); out.EndObject();
    out.Key("complete"); out.Bool(IsCaptureDone()); out.Key("passes"); out.StartArray();
    for(auto& query:m_queries)
    {
        out.StartObject(); out.Key("path"); out.String(query.pass->GetPathName().GetCStr());
        out.Key("parent"); out.Bool(azrtti_cast<AZ::RPI::ParentPass*>(query.pass.get())!=nullptr);
        const auto* render=azrtti_cast<const AZ::RPI::RenderPass*>(query.pass.get());
        out.Key("queue"); out.Int(render && render->GetScope()?int(render->GetScope()->GetHardwareQueueClass()):-1); out.EndObject();
    }
    out.EndArray(); out.Key("samples"); out.StartArray();
    for(const auto& sample:m_samples)
    {
        out.StartArray(); out.Uint(sample.frame); out.Uint(sample.pass); out.Uint64(sample.begin);
        out.Uint64(sample.durationTicks); out.Uint64(sample.nanoseconds); out.EndArray();
    }
    out.EndArray(); out.Key("graphics_span_ns"); out.StartArray();
    for(const auto ns:m_graphicsSpans) out.Uint64(ns);
    out.EndArray(); out.EndObject();
    // Parent setters recurse. Restore parents first, then each child's own state.
    for(auto& query:m_queries) query.pass->SetTimestampQueryEnabled(query.previouslyEnabled);
    m_queries.clear(); m_samples.clear(); m_graphicsSpans.clear(); m_requested=0; m_frames=0;
    return {buffer.GetString(),buffer.GetSize()};
}
void PlanarBenchmark::Release()
{
    EndCapture(); SetReferenceVisible(false);
    if(m_processor && m_mesh.IsValid()) m_processor->ReleaseMesh(m_mesh);
    m_processor=nullptr; m_model={}; m_material={}; m_patch.SetInvalid(); m_overlay.clear();
}
void PlanarBenchmark::OnTick(float,AZ::ScriptTimePoint)
{
    // Paint-event overlays do not update in a hidden automated Editor session.
    // Submit text on render ticks so the diagnostic label is captured there too.
    if(!m_overlay.empty())
    {
        auto* fonts=AZ::Interface<AzFramework::FontQueryInterface>::Get();
        auto* viewports=AZ::Interface<AZ::RPI::ViewportContextRequestsInterface>::Get();
        if(fonts && viewports)
            if(auto font=fonts->GetDefaultFontDrawInterface())
                if(auto viewport=viewports->GetDefaultViewportContext())
                {
                    AzFramework::TextDrawParameters params;
                    params.m_drawViewportId=viewport->GetId(); params.m_position=AZ::Vector3(12,16,0);
                    params.m_textSizeFactor=16; font->DrawScreenAlignedText2d(params,m_overlay);
                }
    }
    if(IsReady())
    {
        auto* registry=AZ::RHI::RHISystemInterface::Get()->GetDrawListTagRegistry();
        for(const char* name:{"shadow","motion"})
        {
            const auto tag=registry->FindTag(AZ::Name(name));
            if(tag.IsValid()) m_processor->SetDrawItemEnabled(m_mesh,tag,false);
        }
        m_material->Compile();
    }
    if(m_queries.empty() || IsCaptureDone()) return;
    if(m_warmup) {--m_warmup; return;}
    const auto anchor=m_queries[m_anchor].pass->GetLatestTimestampResult();
    if(anchor.GetTimestampBeginInTicks()==0 || anchor.GetTimestampBeginInTicks()<=m_queries[m_anchor].lastBegin) return;
    AZ::RPI::TimestampResult graphicsSpan; bool hasGraphics=false;
    for(AZ::u32 i=0;i<m_queries.size();++i)
    {
        auto& query=m_queries[i]; const auto result=query.pass->GetLatestTimestampResult();
        const auto begin=result.GetTimestampBeginInTicks();
        if(begin==0 || begin<=query.lastBegin) continue; // never count a stale readback twice
        query.lastBegin=begin;
        m_samples.push_back({m_frames,i,begin,result.GetDurationInTicks(),result.GetDurationInNanoseconds()});
        const auto* render=azrtti_cast<const AZ::RPI::RenderPass*>(query.pass.get());
        if(render && render->GetScope() && render->GetScope()->GetHardwareQueueClass()==AZ::RHI::HardwareQueueClass::Graphics)
        {
            if(hasGraphics) graphicsSpan.Add(result); else {graphicsSpan=result;hasGraphics=true;}
        }
    }
    m_graphicsSpans.push_back(hasGraphics?graphicsSpan.GetDurationInNanoseconds():0);
    ++m_frames;
}
}
