// SPDX-License-Identifier: MIT
#pragma once
#include <SilPOM/MeshSurfaceAsset.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzFramework/Components/ComponentAdapter.h>
#include <Atom/RPI.Reflect/Material/MaterialAsset.h>
#include <Atom/RPI.Reflect/Image/StreamingImageAsset.h>
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>

namespace SilPOM
{
struct MeshMaterialBinding
{
    AZ_TYPE_INFO(MeshMaterialBinding, "{B592E91C-172C-4A55-A887-B4870B8F19F5}");
    AZStd::string m_id;
    AZ::Data::Asset<AZ::RPI::MaterialAsset> m_material{AZ::Data::AssetLoadBehavior::NoLoad};
};
struct MeshProfileBinding
{
    AZ_TYPE_INFO(MeshProfileBinding, "{9B32A1E0-E4E5-43BC-B321-45B407B75F58}");
    AZ::u32 m_id = 1;
    AZ::Data::Asset<AZ::RPI::StreamingImageAsset> m_height{AZ::Data::AssetLoadBehavior::NoLoad};
    float m_scale = .1f, m_reference = .5f;
    AZ::Vector2 m_tiling = AZ::Vector2(1), m_offset = AZ::Vector2(0);
    AZ::u32 m_addressMode = 0;
};
class MeshConfig final : public AZ::ComponentConfig
{
public:
    AZ_RTTI(MeshConfig, "{FBA6857B-58C5-48F3-8E48-C7A71C10D850}", AZ::ComponentConfig);
    AZ_CLASS_ALLOCATOR(MeshConfig, AZ::SystemAllocator);
    static void Reflect(AZ::ReflectContext* context);
    AZ::Data::Asset<MeshSurfaceAsset> m_surface{AZ::Data::AssetLoadBehavior::NoLoad};
    AZStd::vector<MeshMaterialBinding> m_materials;
    AZStd::vector<MeshProfileBinding> m_profiles;
    AZ::u32 m_maxNodes = 2048;
    AZ::u32 m_maxFragments = 8192;
    float m_depthAccuracy = .0005f;
    // Opt-in until the integrated shader-complexity/performance gate passes.
    bool m_enableExperimentalRaster = false;
};
class MeshRequests : public AZ::EBusTraits
{
public:
    static constexpr AZ::EBusAddressPolicy AddressPolicy = AZ::EBusAddressPolicy::ById;
    static constexpr AZ::EBusHandlerPolicy HandlerPolicy = AZ::EBusHandlerPolicy::Single;
    using BusIdType = AZ::EntityId;
    virtual AZStd::string GetStatus() const = 0;
    virtual bool IsReady() const = 0;
    virtual AZStd::vector<AZ::u32> GetHitOffsets() const = 0;
};
using MeshRequestBus = AZ::EBus<MeshRequests>;

class MeshController final : private AZ::TickBus::Handler, private AZ::TransformNotificationBus::Handler,
    private AZ::Data::AssetBus::MultiHandler, private MeshRequestBus::Handler
{
public:
    AZ_TYPE_INFO(MeshController, "{5B217221-61FB-4F6D-9817-E08DA2CEB6F8}");
    MeshController();
    explicit MeshController(const MeshConfig& config);
    ~MeshController();
    static void Reflect(AZ::ReflectContext* context);
    static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType&);
    static void GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType&);
    static void GetRequiredServices(AZ::ComponentDescriptor::DependencyArrayType&);
    void Activate(AZ::EntityId);
    void Deactivate();
    void SetConfiguration(const MeshConfig&);
    const MeshConfig& GetConfiguration() const { return m_configuration; }
    AZStd::string GetStatus() const override { return m_status; }
    bool IsReady() const override;
    AZStd::vector<AZ::u32> GetHitOffsets() const override;
    MeshConfig m_configuration;
private:
    struct Generation;
    void OnTick(float, AZ::ScriptTimePoint) override;
    void OnTransformChanged(const AZ::Transform&, const AZ::Transform&) override;
    void OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData>) override;
    void OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData>) override;
    void OnAssetError(AZ::Data::Asset<AZ::Data::AssetData>) override;
    bool Prepare();
    void ConnectAssets();
    void Release(AZStd::unique_ptr<Generation>&);
    AZ::EntityId m_entity;
    AZ::Transform m_world = AZ::Transform::CreateIdentity();
    AZ::Render::MeshFeatureProcessorInterface* m_processor = nullptr;
    AZ::Data::Asset<AZ::RPI::MaterialAsset> m_template{AZ::Data::AssetLoadBehavior::NoLoad};
    AZStd::unique_ptr<Generation> m_active, m_pending;
    AZStd::string m_status = "Inactive";
    bool m_dirty = true;
};
class MeshComponent final : public AzFramework::Components::ComponentAdapter<MeshController, MeshConfig>
{
public:
    using BaseClass = AzFramework::Components::ComponentAdapter<MeshController, MeshConfig>;
    AZ_COMPONENT(MeshComponent, "{7F58067E-F9E7-4B76-A3D4-802B26638C05}", BaseClass);
    MeshComponent() = default;
    explicit MeshComponent(const MeshConfig& config) : BaseClass(config) {}
    static void Reflect(AZ::ReflectContext* context);
};
}
