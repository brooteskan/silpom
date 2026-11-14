// SPDX-License-Identifier: MIT
#include <SilPOM/DecalPaint.h>
#include <SilPOM/DecalPaintMath.h>
#include <SilPOM/DecalCompositeMath.h>
#include <AzCore/Asset/AssetSerializer.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Serialization/Json/JsonUtils.h>
#include <AzCore/IO/ByteContainerStream.h>
#include <AzCore/std/parallel/atomic.h>
#include <AzCore/Console/IConsole.h>
#include <AzCore/RTTI/BehaviorContext.h>
#include <AzCore/Script/ScriptContextAttributes.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Reflect/Buffer/BufferAssetCreator.h>
#include <AzCore/std/containers/unordered_set.h>
#include <algorithm>
#include <cmath>

namespace SilPOM
{
namespace
{
AZStd::atomic_uint64_t PaintRevision{0};
AZ_CVAR(bool,r_silpomPaintCompositeReference,false,nullptr,AZ::ConsoleFunctorFlags::Null,
    "Diagnostic: evaluate the complete paint stream without spatial skipping or opacity early-out.");
// Native entity undo reconstructs controllers. Retain the irreversible frontier
// separately so an old entity snapshot cannot resurrect composited source paths.
AZStd::unordered_map<AZ::EntityId,PaintConfig> CompositeHistory;
bool KeepCompositeHistory=false;
PaintDab ResolveDab(const PaintStroke& s,const PaintPoint& p)
{
    PaintDab d;d.m_point=p;d.m_material=s.m_material;d.m_color=s.m_color;
    d.m_diameter=PaintMath::Diameter(s.m_diameter,p.m_pressure,s.m_pressureSize,s.m_minimumSize);
    d.m_depth=s.m_depth;d.m_rotation=s.m_rotation;
    d.m_opacity=s.m_opacity*(s.m_pressureOpacity?PaintMath::Pressure(p.m_pressure):1.f);
    d.m_normalOpacity=s.m_normalOpacity;d.m_blendMode=s.m_blendMode;d.m_visible=s.m_visible;d.m_strokeId=s.m_id;
    d.m_erase=s.m_erase;d.m_heightMask=s.m_heightMask&7u;
    return d;
}
}
void ResetPaintCompositeHistory(){CompositeHistory.clear();}
void EnablePaintCompositeHistory(bool enabled){KeepCompositeHistory=enabled;if(!enabled)ResetPaintCompositeHistory();}
void CompactPaint(PaintConfig& c)
{
    AZStd::vector<AZ::u64> ages;
    for(auto& layer:c.m_layers)
    {
        if(!layer.m_id)layer.m_id=c.m_nextId++;
        for(auto& s:layer.m_strokes)
        {
            if(!s.m_id)s.m_id=c.m_nextId++;
            if(AZStd::find(layer.m_order.begin(),layer.m_order.end(),s.m_id)==layer.m_order.end())layer.m_order.push_back(s.m_id);
            if(!s.m_explicitDabs)
            {
                AZStd::vector<PaintPoint> points;
                PaintMath::Sample(s.m_points.size(),[&](size_t i){const auto& p=s.m_points[i];return PaintMath::Point{
                    {p.m_position.GetX(),p.m_position.GetY(),p.m_position.GetZ()},
                    {p.m_normal.GetX(),p.m_normal.GetY(),p.m_normal.GetZ()},p.m_pressure,p.m_start};},
                    [&](const PaintMath::Point& p){PaintPoint point;point.m_position={p.position.x,p.position.y,p.position.z};
                        point.m_normal={p.normal.x,p.normal.y,p.normal.z};point.m_pressure=p.pressure;point.m_start=p.start;
                        points.push_back(point);return true;},s.m_diameter,s.m_spacing,s.m_pressureSize,s.m_minimumSize,1024*1024);
                s.m_points=AZStd::move(points);s.m_explicitDabs=true;
            }
            for(auto& p:s.m_points)
            {
                if(!p.m_sequence)p.m_sequence=c.m_nextId++;
                c.m_nextId=AZStd::max(c.m_nextId,p.m_sequence+1);
                if(p.m_sequence>c.m_frozenThrough)ages.push_back(p.m_sequence);
            }
        }
        // Reorder only the editable slots; fixed runs retain their stack position.
        // Prefab reconstruction can merge a saved order with the irreversible
        // frontier. A run must occupy exactly one slot in that merged order.
        AZStd::unordered_set<AZ::u64> orderIds;
        layer.m_order.erase(AZStd::remove_if(layer.m_order.begin(),layer.m_order.end(),
            [&](AZ::u64 id){return !orderIds.insert(id).second;}),layer.m_order.end());
        AZStd::vector<AZ::u64> liveIds;for(const auto& s:layer.m_strokes)liveIds.push_back(s.m_id);
        size_t next=0;
        for(auto& id:layer.m_order)
            if(AZStd::find(liveIds.begin(),liveIds.end(),id)!=liveIds.end())id=liveIds[next++];
    }
    AZ::u64 cutoff=c.m_frozenThrough;
    if(ages.size()>PaintConfig::LiveDabs)
    {
        std::sort(ages.begin(),ages.end());cutoff=ages[ages.size()-PaintConfig::LiveDabs-1];
    }
    for(auto& layer:c.m_layers)
    {
        for(auto& s:layer.m_strokes)
        {
            auto keep=s.m_points.begin();
            for(auto it=s.m_points.begin();it!=s.m_points.end();++it)
            {
                if(it->m_sequence<=cutoff)
                {
                    if(it->m_sequence>c.m_frozenThrough)layer.m_composite.push_back(ResolveDab(s,*it));
                }
                else *keep++=*it;
            }
            s.m_points.erase(keep,s.m_points.end());
        }
        layer.m_strokes.erase(AZStd::remove_if(layer.m_strokes.begin(),layer.m_strokes.end(),
            [](const PaintStroke& s){return s.m_points.empty();}),layer.m_strokes.end());
    }
    c.m_frozenThrough=cutoff;
}
void PaintConfig::Reflect(AZ::ReflectContext* context)
{
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
    {
        sc->Class<PaintPoint>()->Version(1)->Field("Position",&PaintPoint::m_position)->Field("Normal",&PaintPoint::m_normal)
            ->Field("Pressure",&PaintPoint::m_pressure)->Field("Start",&PaintPoint::m_start)->Field("Sequence",&PaintPoint::m_sequence);
        sc->Class<PaintStroke>()->Version(1)->Field("Name",&PaintStroke::m_name)->Field("Material",&PaintStroke::m_material)
            ->Field("Points",&PaintStroke::m_points)->Field("Color",&PaintStroke::m_color)->Field("Diameter",&PaintStroke::m_diameter)
            ->Field("Spacing",&PaintStroke::m_spacing)->Field("Opacity",&PaintStroke::m_opacity)->Field("Rotation",&PaintStroke::m_rotation)
            ->Field("Depth",&PaintStroke::m_depth)->Field("MinimumSize",&PaintStroke::m_minimumSize)->Field("BlendMode",&PaintStroke::m_blendMode)
            ->Field("NormalOpacity",&PaintStroke::m_normalOpacity)
            ->Field("PressureSize",&PaintStroke::m_pressureSize)->Field("PressureOpacity",&PaintStroke::m_pressureOpacity)->Field("Visible",&PaintStroke::m_visible)
            ->Field("Id",&PaintStroke::m_id)->Field("ExplicitDabs",&PaintStroke::m_explicitDabs)
            ->Field("Erase",&PaintStroke::m_erase)->Field("HeightMask",&PaintStroke::m_heightMask);
        sc->Class<PaintDab>()->Version(1)->Field("Point",&PaintDab::m_point)->Field("Material",&PaintDab::m_material)
            ->Field("Color",&PaintDab::m_color)->Field("Diameter",&PaintDab::m_diameter)->Field("Depth",&PaintDab::m_depth)
            ->Field("Rotation",&PaintDab::m_rotation)->Field("Opacity",&PaintDab::m_opacity)->Field("NormalOpacity",&PaintDab::m_normalOpacity)
            ->Field("BlendMode",&PaintDab::m_blendMode)->Field("StrokeId",&PaintDab::m_strokeId)->Field("Visible",&PaintDab::m_visible)
            ->Field("Erase",&PaintDab::m_erase)->Field("HeightMask",&PaintDab::m_heightMask);
        sc->Class<PaintLayer>()->Version(1)->Field("Name",&PaintLayer::m_name)->Field("Visible",&PaintLayer::m_visible)
            ->Field("Locked",&PaintLayer::m_locked)->Field("Opacity",&PaintLayer::m_opacity)->Field("Strokes",&PaintLayer::m_strokes)
            ->Field("Id",&PaintLayer::m_id)->Field("Composite",&PaintLayer::m_composite)->Field("Order",&PaintLayer::m_order);
        sc->Class<PaintConfig,AZ::ComponentConfig>()->Version(1)->Field("Layers",&PaintConfig::m_layers)
            ->Field("NextId",&PaintConfig::m_nextId)->Field("FrozenThrough",&PaintConfig::m_frozenThrough);
    }
}
void PaintController::Reflect(AZ::ReflectContext* context)
{
    PaintConfig::Reflect(context);
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
        sc->Class<PaintController>()->Version(1)->Field("Configuration",&PaintController::m_configuration);
    if(auto* bc=azrtti_cast<AZ::BehaviorContext*>(context))
        bc->EBus<PaintRequestBus>("DecalPaintRequestBus")->Attribute(AZ::Script::Attributes::Module,"silpom")
            ->Attribute(AZ::Script::Attributes::Scope,AZ::Script::Attributes::ScopeFlags::Common)
            ->Event("GetPaintStatus",&PaintRequests::GetPaintStatus)->Event("GetDabCount",&PaintRequests::GetDabCount)
            ->Event("GetDocumentJson",&PaintRequests::GetDocumentJson)
            ->Event("GetCompositeDabCount",&PaintRequests::GetCompositeDabCount)->Event("GetRenderHandleCount",&PaintRequests::GetRenderHandleCount)
            ->Event("SetDocumentJson",&PaintRequests::SetDocumentJson);
}
void PaintComponent::Reflect(AZ::ReflectContext* c)
{ BaseClass::Reflect(c); if(auto* sc=azrtti_cast<AZ::SerializeContext*>(c)) sc->Class<PaintComponent,BaseClass>()->Version(1); }
void PaintController::GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& a) { a.push_back(AZ_CRC_CE("DecalPaintService")); }
void PaintController::GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& a) { a.push_back(AZ_CRC_CE("DecalPaintService")); }
void PaintController::GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& a) { a.push_back(AZ_CRC_CE("TransformService")); }
void PaintController::Activate(AZ::EntityId entity)
{
    m_entity=entity;
    SetDocument(m_configuration);
    AZ::TransformBus::EventResult(m_world,entity,&AZ::TransformBus::Events::GetWorldTM);
    PaintRequestBus::Handler::BusConnect(entity);
    AZ::TransformNotificationBus::Handler::BusConnect(entity);
    AZ::TickBus::Handler::BusConnect();
    m_dirty=true;
}
void PaintController::Release()
{
    if(m_processor)
    {
        if(m_batch.IsValid())m_processor->ReleaseDecal(m_batch);
        for(auto [id,h]:m_materialHandles){(void)id;m_processor->ReleaseDecal(h);}
    }
    m_batch={};m_materialHandles.clear();m_buffer={};
}
void PaintController::Deactivate()
{
    AZ::TickBus::Handler::BusDisconnect();
    PaintRequestBus::Handler::BusDisconnect();
    AZ::TransformNotificationBus::Handler::BusDisconnect();
    Release();m_processor=nullptr;m_entity.SetInvalid();
}
void PaintController::SetDocument(const PaintConfig& c)
{
    auto next=c;
    const auto history=CompositeHistory.find(m_entity);
    if(KeepCompositeHistory&&m_entity.IsValid()&&history!=CompositeHistory.end())
    {
        const auto& fixed=history->second;
        next.m_nextId=AZStd::max(next.m_nextId,fixed.m_nextId);
        if(next.m_frozenThrough<fixed.m_frozenThrough)
        {
            m_repairedComposite=true;
            for(size_t i=0;i<fixed.m_layers.size();++i)
            {
                const auto& old=fixed.m_layers[i];if(old.m_composite.empty())continue;
                auto it=AZStd::find_if(next.m_layers.begin(),next.m_layers.end(),[&](const PaintLayer& l){return l.m_id==old.m_id;});
                if(it==next.m_layers.end())
                {auto restored=old;restored.m_strokes.clear();next.m_layers.insert(next.m_layers.begin()+AZStd::min(i,next.m_layers.size()),AZStd::move(restored));}
                else
                {
                    it->m_composite=old.m_composite;
                    auto order=old.m_order;
                    for(auto id:it->m_order)if(AZStd::find(order.begin(),order.end(),id)==order.end())order.push_back(id);
                    it->m_order=AZStd::move(order);
                }
            }
            next.m_frozenThrough=fixed.m_frozenThrough;
        }
    }
    CompactPaint(next);
    m_configuration=AZStd::move(next);
    if(KeepCompositeHistory&&m_entity.IsValid())
    {
        // Keep fixed runs for deleted layers as well: an undo snapshot stores
        // only editable data and can restore a layer without copying its paint.
        auto& archive=CompositeHistory[m_entity];
        archive.m_nextId=AZStd::max(archive.m_nextId,m_configuration.m_nextId);
        archive.m_frozenThrough=AZStd::max(archive.m_frozenThrough,m_configuration.m_frozenThrough);
        for(const auto& layer:m_configuration.m_layers)
        {
            auto it=AZStd::find_if(archive.m_layers.begin(),archive.m_layers.end(),[&](const PaintLayer& l){return l.m_id==layer.m_id;});
            if(it==archive.m_layers.end())archive.m_layers.push_back(layer);else *it=layer;
        }
    }
    m_revision=++PaintRevision;m_dirty=true;
}
void PaintController::RestoreUndoDocument(const PaintConfig& snapshot)
{
    auto next=snapshot;
    if(const auto history=CompositeHistory.find(m_entity);history!=CompositeHistory.end())
    {
        const auto& archive=history->second;
        next.m_nextId=AZStd::max(next.m_nextId,archive.m_nextId);
        next.m_frozenThrough=AZStd::max(next.m_frozenThrough,archive.m_frozenThrough);
        for(auto& layer:next.m_layers)
        {
            const auto fixed=AZStd::find_if(archive.m_layers.begin(),archive.m_layers.end(),[&](const PaintLayer& l){return l.m_id==layer.m_id;});
            if(fixed==archive.m_layers.end())continue;
            layer.m_composite=fixed->m_composite;
            auto order=fixed->m_order;
            for(auto id:layer.m_order)if(AZStd::find(order.begin(),order.end(),id)==order.end())order.push_back(id);
            layer.m_order=AZStd::move(order);
        }
    }
    SetDocument(next);
}
AZStd::string PaintController::GetDocumentJson() const
{
    AZStd::string result;AZ::IO::ByteContainerStream<AZStd::string> stream(&result);
    return AZ::JsonSerializationUtils::SaveObjectToStream(&m_configuration,stream).IsSuccess()?result:AZStd::string{};
}
AZStd::string PaintController::SetDocumentJson(const AZStd::string& json)
{
    PaintConfig c;const auto loaded=AZ::JsonSerializationUtils::LoadObjectFromString(c,json);
    if(!loaded.IsSuccess())return loaded.GetError();
    SetDocument(c);return {};
}
void PaintController::OnTick(float,AZ::ScriptTimePoint)
{
    if(!m_processor) m_processor=AZ::RPI::Scene::GetFeatureProcessorForEntity<AZ::Render::DecalFeatureProcessorInterface>(m_entity);
    if(m_processor)
    {
        AZ::u64 fingerprint=r_silpomPaintCompositeReference?0x7144878129ULL:0;
        for(const auto& [id,h]:m_materialHandles)
        {
            (void)id;const auto d=m_processor->GetDecalData(h);
            fingerprint^=(AZ::u64(d.m_textureArrayIndex)<<32|d.m_textureIndex)+0x9e3779b97f4a7c15ULL+AZ::u64(h.GetIndex());
        }
        if(fingerprint!=m_materialFingerprint){m_materialFingerprint=fingerprint;m_dirty=true;}
    }
    if(m_dirty && m_processor) {Rebuild();m_dirty=false;}
}
#include "DecalComposite.inl"
}
