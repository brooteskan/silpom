// SPDX-License-Identifier: MIT
#pragma once
#include <SilPOM/MeshSurfaceAsset.h>
#include <AzCore/Component/TickBus.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Math/Vector4.h>
#include <AzFramework/Components/ComponentAdapter.h>
#include <Atom/RPI.Public/SceneBus.h>
#include <Atom/RPI.Reflect/Material/MaterialAsset.h>
#include <Atom/RPI.Reflect/Image/StreamingImageAsset.h>
#include <Atom/Feature/Mesh/MeshFeatureProcessorInterface.h>

namespace SilPOM
{
enum class SpiralLightMode : AZ::u32
{
    Off = 0,
    SelectedMaterials = 1,
    AllSilPomSurfaces = 2
};
AZ_TYPE_INFO_SPECIALIZE(SpiralLightMode, "{375431D9-B6A5-49DD-8663-268A00CF6709}");
// Preserve serialized value 2 as the historical hybrid comparison.
enum class LocalShadowMethod : AZ::u32 { Baseline=0, Gather=1, Neural=2, NeuralOnly=3 };
AZ_TYPE_INFO_SPECIALIZE(LocalShadowMethod, "{8C21E33E-53B1-45A9-9554-57656F558F10}");
// Lighting only. Zero preserves the approved near/shared/proxy routing.
enum class LightingEstimator : AZ::u32 { CurrentRouting=0, PhysicalRayNeural=1, DirectionalField32=2, AngularOccluders4=3, AngularOccluders8=4, AngularEdges4=5 };
AZ_TYPE_INFO_SPECIALIZE(LightingEstimator, "{4BDDBA90-2C99-4DD0-99A2-40F22F5D8B82}");
struct MeshMaterialBinding
{
    AZ_TYPE_INFO(MeshMaterialBinding, "{B592E91C-172C-4A55-A887-B4870B8F19F5}");
    AZStd::string m_id;
    AZ::Data::Asset<AZ::RPI::MaterialAsset> m_material{AZ::Data::AssetLoadBehavior::NoLoad};
    AZ::u32 m_lightingChannelMask = 1;
    bool m_disableEnvironmentLighting = false;
    bool m_receiveSpiralLight = false;
};
struct MeshProfileBinding
{
    AZ_TYPE_INFO(MeshProfileBinding, "{9B32A1E0-E4E5-43BC-B321-45B407B75F58}");
    bool IsTrained() const { return !m_useBaseMesh && m_neuralModel != 0; }
    AZ::u32 m_id = 1;
    AZ::Data::Asset<AZ::RPI::StreamingImageAsset> m_height{AZ::Data::AssetLoadBehavior::NoLoad};
    AZ::Data::Asset<AZ::RPI::StreamingImageAsset> m_neuralCache{AZ::Data::AssetLoadBehavior::NoLoad};
    float m_scale = .1f, m_reference = .5f;
    AZ::Vector2 m_tiling = AZ::Vector2(1), m_offset = AZ::Vector2(0);
    AZ::u32 m_addressMode = 0;
    AZ::u32 m_neuralModel = 0;
    bool m_useBaseMesh = false;
};
class MeshConfig final : public AZ::ComponentConfig
{
public:
    AZ_RTTI(MeshConfig, "{FBA6857B-58C5-48F3-8E48-C7A71C10D850}", AZ::ComponentConfig);
    AZ_CLASS_ALLOCATOR(MeshConfig, AZ::SystemAllocator);
    static void Reflect(AZ::ReflectContext* context);
    static constexpr float MaxReliefDepthMultiplier = 2.0f;
    static constexpr size_t MaxAdditionalSpiralWindows = 8;
    bool UsesTrainedProfiles() const;
    AZ::Crc32 GetReliefDepthVisibility() const;
    AZ::Data::Asset<MeshSurfaceAsset> m_surface{AZ::Data::AssetLoadBehavior::NoLoad};
    AZStd::vector<MeshMaterialBinding> m_materials;
    AZStd::vector<MeshProfileBinding> m_profiles;
    float m_reliefDepthMultiplier = 1.0f;
    AZ::u32 m_maxCells = 4096;
    AZ::u32 m_reliefShadowSteps = 16;
    AZ::u32 m_validationIterations = 6;
    AZ::u32 m_validationRetryCap = 4;
    float m_validationToleranceMultiplier = 1.0f;
    bool m_useR16HeightTextures = false;
    bool m_useValidationGather = false;
    bool m_useLevelTwoPredictor = false;
    bool m_useLevelFourWallMaterial = false;
    bool m_useLevelFourShared = false;
    bool m_useLevelFiveProxy = false;
    bool m_useLevelFiveDepth = false;
    float m_levelFivePixelBudget = .75f;
    float m_reliefLodPixelBudget = 0.00f;
    float m_maxReliefDistance = 0.00f;
    float m_validationDistanceStart = 0.00f;
    float m_validationDistanceEnd = 0.00f;
    AZ::u32 m_distantValidationIterations = 4;
    AZ::u32 m_distantValidationRetryCap = 1;
    float m_distantValidationToleranceMultiplier = 1.00f;
    AZ::u32 m_spiralLightShadowSteps = 18;
    LocalShadowMethod m_localShadowMethod = LocalShadowMethod::Baseline;
    LightingEstimator m_lightingEstimator = LightingEstimator::CurrentRouting;
    bool m_diffuseAccessibility = false;
    bool m_neuralDirectionalVisibility = false;
    bool m_proxyDirectionalResponse = false;
    float m_neuralShadowConfidence = .995f;
    bool m_useHeightDerivedNormals = true;
    SpiralLightMode m_spiralLightMode = SpiralLightMode::SelectedMaterials;
    float m_spiralLightBrightness = 1.0f;
    // Local window XY, centre height of its stair corridor, and signed rise per turn.
    AZ::Vector4 m_spiralLightPath = AZ::Vector4::CreateZero();
    float m_spiralLightPeakLux = 0.0f;
    // Local end height and fade distance below it. A zero distance leaves the path unbounded.
    AZ::Vector2 m_spiralLightEnd = AZ::Vector2::CreateZero();
    // Signed turns from the path origin and peak lux for each additional window.
    AZStd::vector<AZ::Vector2> m_spiralLightWindows;
    bool m_useDepthHeightBounds = false;
    bool m_useNeuralSeed = false;
};
class MeshRequests : public AZ::EBusTraits
{
public:
    static constexpr AZ::EBusAddressPolicy AddressPolicy = AZ::EBusAddressPolicy::ById;
    static constexpr AZ::EBusHandlerPolicy HandlerPolicy = AZ::EBusHandlerPolicy::Single;
    using BusIdType = AZ::EntityId;
    virtual AZStd::string GetStatus() const = 0;
    virtual bool IsReady() const = 0;
    virtual AZ::Vector3 GetProxyLodState() const = 0; // far faces, candidate faces, largest selected pixel bound
    virtual AZStd::vector<AZ::u32> GetProxyFaceOwners() const = 0;
};
using MeshRequestBus = AZ::EBus<MeshRequests>;

class MeshController final : private AZ::TickBus::Handler, private AZ::TransformNotificationBus::Handler,
    private AZ::Data::AssetBus::MultiHandler, private MeshRequestBus::Handler, private AZ::RPI::SceneNotificationBus::Handler
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
    AZ::Vector3 GetProxyLodState() const override;
    AZStd::vector<AZ::u32> GetProxyFaceOwners() const override;
    MeshConfig m_configuration;
private:
    struct Generation;
    void OnTick(float, AZ::ScriptTimePoint) override;
    void OnTransformChanged(const AZ::Transform&, const AZ::Transform&) override;
    void OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData>) override;
    void OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData>) override;
    void OnAssetError(AZ::Data::Asset<AZ::Data::AssetData>) override;
    void OnRenderPipelineChanged(AZ::RPI::RenderPipeline*, AZ::RPI::SceneNotification::RenderPipelineChangeType) override;
    void OnBeginPrepareRender() override;
    void UpdateProxyLod();
    bool Prepare();
    void ConnectAssets();
    void Release(AZStd::unique_ptr<Generation>&);
    AZ::EntityId m_entity;
    AZ::Transform m_world = AZ::Transform::CreateIdentity();
    AZ::Render::MeshFeatureProcessorInterface* m_processor = nullptr;
    AZ::Data::Asset<AZ::RPI::MaterialAsset> m_template{AZ::Data::AssetLoadBehavior::NoLoad};
    AZ::Data::Asset<LearnedMaterialAsset> m_learnedMaterial{AZ::Data::AssetLoadBehavior::NoLoad};
    AZ::Data::Asset<LearnedMaterialAsset> m_appearance{AZ::Data::AssetLoadBehavior::NoLoad};
    AZStd::unique_ptr<Generation> m_active, m_pending;
    AZStd::string m_status = "Inactive";
    bool m_dirty = true;
    bool m_checkHitPasses = false;
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
