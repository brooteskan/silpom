// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/Component.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Asset/AssetCommon.h>
#include <AzFramework/Components/ComponentAdapter.h>
#include <Atom/RPI.Public/Material/Material.h>
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>

namespace SilPOM
{
class PatchConfig final : public AZ::ComponentConfig
{
public:
    AZ_RTTI(PatchConfig,"{3248B640-B61C-4320-B96C-FDC72142EB30}",AZ::ComponentConfig);
    AZ_CLASS_ALLOCATOR(PatchConfig,AZ::SystemAllocator);
    static void Reflect(AZ::ReflectContext* context);
    AZ::Data::Asset<AZ::RPI::MaterialAsset> m_material{AZ::Data::AssetLoadBehavior::NoLoad};
    float m_width=2, m_height=2, m_heightScale=.1f, m_reference=.5f;
    float m_tileU=1, m_tileV=1, m_offsetU=0, m_offsetV=0;
    AZ::u32 m_maxCells=4096, m_addressMode=0, m_debug=0;
};
class PatchRequests : public AZ::EBusTraits
{
public:
    static constexpr AZ::EBusAddressPolicy AddressPolicy=AZ::EBusAddressPolicy::ById;
    static constexpr AZ::EBusHandlerPolicy HandlerPolicy=AZ::EBusHandlerPolicy::Single;
    using BusIdType=AZ::EntityId;
    virtual AZStd::string GetStatus() const=0;
    virtual bool IsReady() const=0;
};
using PatchRequestBus=AZ::EBus<PatchRequests>;

class PatchController final : private AZ::TickBus::Handler,
    private AZ::TransformNotificationBus::Handler, private AZ::Data::AssetBus::Handler,
    private PatchRequestBus::Handler
{
public:
    AZ_TYPE_INFO(PatchController,"{CBDBB427-7A62-4DCB-8783-F987F5841884}");
    PatchController()=default;
    explicit PatchController(const PatchConfig& config):m_configuration(config) {}
    static void Reflect(AZ::ReflectContext* context);
    static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& services);
    static void GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& services);
    static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType& services);
    void Activate(AZ::EntityId id);
    void Deactivate();
    void SetConfiguration(const PatchConfig& config);
    const PatchConfig& GetConfiguration() const {return m_configuration;}
    AZStd::string GetStatus() const override
    {return m_mesh.IsValid() && !IsReady()?AZStd::string("Waiting for mesh GPU resources"):m_status;}
    bool IsReady() const override
    {return m_meshProcessor && m_mesh.IsValid() && bool(m_meshProcessor->GetModel(m_mesh));}
    PatchConfig m_configuration;
private:
    void OnTick(float,AZ::ScriptTimePoint) override;
    void OnTransformChanged(const AZ::Transform&,const AZ::Transform& world) override;
    void OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData>) override {m_dirty=true;}
    void OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData> asset) override;
    void OnAssetError(AZ::Data::Asset<AZ::Data::AssetData>) override;
    void ReleaseMesh();
    bool Prepare();
    AZ::EntityId m_entity;
    AZ::Transform m_world=AZ::Transform::CreateIdentity();
    AZ::Render::MeshFeatureProcessorInterface* m_meshProcessor=nullptr;
    AZ::Render::MeshFeatureProcessorInterface::MeshHandle m_mesh;
    AZ::Data::Asset<AZ::RPI::ModelAsset> m_model;
    AZ::Data::Instance<AZ::RPI::Material> m_material;
    AZStd::string m_status="Inactive";
    bool m_dirty=true;
};
class PatchComponent final : public AzFramework::Components::ComponentAdapter<PatchController,PatchConfig>
{
public:
    using BaseClass=AzFramework::Components::ComponentAdapter<PatchController,PatchConfig>;
    AZ_COMPONENT(PatchComponent,"{C8F626EF-C5DB-4F84-975B-C420606C9605}",BaseClass);
    PatchComponent()=default;
    explicit PatchComponent(const PatchConfig& config):BaseClass(config) {}
    static void Reflect(AZ::ReflectContext* context);
};
}
