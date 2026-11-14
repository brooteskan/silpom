// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/Component.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Math/Color.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/containers/unordered_map.h>
#include <Atom/RPI.Public/Buffer/Buffer.h>
#include <AzFramework/Components/ComponentAdapter.h>
#include <Atom/Feature/Decals/DecalFeatureProcessorInterface.h>
#include <Atom/RPI.Reflect/Material/MaterialAsset.h>

namespace SilPOM
{
struct PaintPoint
{
    AZ_TYPE_INFO(PaintPoint,"{26BE20E4-1543-465A-B623-5BCD3D887E62}");
    AZ::Vector3 m_position=AZ::Vector3::CreateZero(),m_normal=AZ::Vector3::CreateAxisZ();
    float m_pressure=1;
    bool m_start=false;
    AZ::u64 m_sequence=0;
};
struct PaintStroke
{
    AZ_TYPE_INFO(PaintStroke,"{969A1930-609C-427B-BC09-CDE5D0F6AD39}");
    AZStd::string m_name="Stroke";
    AZ::Data::Asset<AZ::RPI::MaterialAsset> m_material{AZ::Data::AssetLoadBehavior::QueueLoad};
    AZStd::vector<PaintPoint> m_points;
    AZ::Color m_color=AZ::Color(1.f,1.f,1.f,1.f);
    float m_diameter=.3f,m_spacing=.25f,m_opacity=.6f,m_rotation=0,m_depth=.12f,m_minimumSize=.15f;
    float m_normalOpacity=0;
    AZ::u32 m_blendMode=0;
    bool m_pressureSize=true,m_pressureOpacity=false,m_visible=true;
    AZ::u64 m_id=0;
    bool m_explicitDabs=false;
    bool m_erase=false;
    AZ::u32 m_heightMask=7; // Low, medium, high; all selected preserves ordinary materials.
};
// A compiled projection has no editable path or pressure history. Its resolved
// footprint and opacities are independent of subsequent brush changes.
struct PaintDab
{
    AZ_TYPE_INFO(PaintDab,"{C5E66BF8-6692-455B-9DC0-A18622E87360}");
    PaintPoint m_point;
    AZ::Data::Asset<AZ::RPI::MaterialAsset> m_material{AZ::Data::AssetLoadBehavior::QueueLoad};
    AZ::Color m_color=AZ::Color::CreateOne();
    float m_diameter=.3f,m_depth=.12f,m_rotation=0,m_opacity=1,m_normalOpacity=0;
    AZ::u32 m_blendMode=0;
    AZ::u64 m_strokeId=0;
    bool m_visible=true;
    bool m_erase=false;
    AZ::u32 m_heightMask=7;
};
struct PaintLayer
{
    AZ_TYPE_INFO(PaintLayer,"{E6C1B1E0-D6CF-4B42-9C97-B2422F6F1E16}");
    AZStd::string m_name="Layer 1";
    bool m_visible=true,m_locked=false;
    float m_opacity=1;
    AZStd::vector<PaintStroke> m_strokes;
    AZ::u64 m_id=0;
    AZStd::vector<PaintDab> m_composite;
    AZStd::vector<AZ::u64> m_order;
};
class PaintConfig final : public AZ::ComponentConfig
{
public:
    AZ_RTTI(PaintConfig,"{07902F9C-78FD-4B78-B199-9722CD0D43E7}",AZ::ComponentConfig);
    AZ_CLASS_ALLOCATOR(PaintConfig,AZ::SystemAllocator);
    static void Reflect(AZ::ReflectContext*);
    static constexpr size_t MaxLayers=64,MaxPoints=32768;
    static constexpr size_t LiveDabs=100;
    // Serialized containers must start empty: O3DE loads their elements by
    // insertion. The painter explicitly adds its first layer on creation.
    AZStd::vector<PaintLayer> m_layers;
    AZ::u64 m_nextId=1,m_frozenThrough=0;
};
// Used by both the editor and runtime, including migration of old prefabs.
void CompactPaint(PaintConfig&);
void ResetPaintCompositeHistory();
void EnablePaintCompositeHistory(bool);
class PaintRequests : public AZ::EBusTraits
{
public:
    static constexpr AZ::EBusAddressPolicy AddressPolicy=AZ::EBusAddressPolicy::ById;
    static constexpr AZ::EBusHandlerPolicy HandlerPolicy=AZ::EBusHandlerPolicy::Single;
    using BusIdType=AZ::EntityId;
    virtual const PaintConfig& GetDocument() const=0;
    virtual void SetDocument(const PaintConfig&)=0;
    virtual AZ::EntityId GetPaintEntity() const=0;
    virtual AZStd::string GetPaintStatus() const=0;
    virtual AZStd::string GetDocumentJson() const=0;
    virtual AZ::u32 GetDabCount() const=0;
    virtual AZ::u64 GetRevision() const=0;
    virtual AZ::u32 GetCompositeDabCount() const=0;
    virtual AZ::u32 GetRenderHandleCount() const=0;
    virtual AZStd::string SetDocumentJson(const AZStd::string&)=0;
    virtual bool ConsumeCompositeRepair()=0;
    virtual void RestoreUndoDocument(const PaintConfig&)=0;
};
using PaintRequestBus=AZ::EBus<PaintRequests>;
class PaintController final : private PaintRequestBus::Handler,private AZ::TransformNotificationBus::Handler,private AZ::TickBus::Handler
{
public:
    AZ_TYPE_INFO(PaintController,"{8CA1B393-B3A0-4610-B98C-450A019B5565}");
    PaintController()=default;
    explicit PaintController(const PaintConfig& c):m_configuration(c){}
    static void Reflect(AZ::ReflectContext*);
    static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType&);
    static void GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType&);
    static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType&);
    void Activate(AZ::EntityId);
    void Deactivate();
    void SetConfiguration(const PaintConfig& c) { SetDocument(c); }
    const PaintConfig& GetConfiguration() const { return m_configuration; }
    const PaintConfig& GetDocument() const override { return m_configuration; }
    void SetDocument(const PaintConfig&) override;
    AZ::EntityId GetPaintEntity() const override { return m_entity; }
    AZStd::string GetPaintStatus() const override { return m_status; }
    AZStd::string GetDocumentJson() const override;
    AZ::u32 GetDabCount() const override { return m_liveCount; }
    AZ::u32 GetCompositeDabCount() const override { return m_compositeCount; }
    AZ::u32 GetRenderHandleCount() const override { return m_batch.IsValid()?1:0; }
    AZStd::string SetDocumentJson(const AZStd::string&) override;
    bool ConsumeCompositeRepair() override {const bool repaired=m_repairedComposite;m_repairedComposite=false;return repaired;}
    void RestoreUndoDocument(const PaintConfig&) override;
    AZ::u64 GetRevision() const override { return m_revision; }
    PaintConfig m_configuration;
private:
    void OnTransformChanged(const AZ::Transform&,const AZ::Transform& world) override { m_world=world;m_dirty=true; }
    void OnTick(float,AZ::ScriptTimePoint) override;
    void Release();
    void Rebuild();
    AZ::EntityId m_entity;
    AZ::Transform m_world=AZ::Transform::CreateIdentity();
    AZ::Render::DecalFeatureProcessorInterface* m_processor=nullptr;
    AZ::Render::DecalFeatureProcessorInterface::DecalHandle m_batch;
    AZStd::unordered_map<AZ::Data::AssetId,AZ::Render::DecalFeatureProcessorInterface::DecalHandle> m_materialHandles;
    AZ::Data::Instance<AZ::RPI::Buffer> m_buffer;
    AZ::u32 m_liveCount=0,m_compositeCount=0;
    AZ::u64 m_materialFingerprint=0;
    AZStd::string m_status="Inactive";
    AZ::u64 m_revision=0;
    bool m_dirty=true;
    bool m_repairedComposite=false;
};
class PaintComponent final : public AzFramework::Components::ComponentAdapter<PaintController,PaintConfig>
{
public:
    using BaseClass=AzFramework::Components::ComponentAdapter<PaintController,PaintConfig>;
    AZ_COMPONENT(PaintComponent,"{80EF36B7-E6D9-4CC4-AC3E-7733A571B55B}",BaseClass);
    PaintComponent()=default;
    explicit PaintComponent(const PaintConfig& c):BaseClass(c){}
    static void Reflect(AZ::ReflectContext*);
};
}
