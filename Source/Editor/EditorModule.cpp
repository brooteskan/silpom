// SPDX-License-Identifier: MIT
#include "Module.h"
#include "PlanarBenchmark.h"
#include "DecalPainter.h"
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <Atom/Feature/Utils/EditorRenderComponentAdapter.h>
#include <AzCore/Serialization/EditContext.h>
namespace SilPOM
{
class EditorPatchComponent final : public AZ::Render::EditorRenderComponentAdapter<PatchController,PatchComponent,PatchConfig>
{
public:
    using BaseClass=AZ::Render::EditorRenderComponentAdapter<PatchController,PatchComponent,PatchConfig>;
    AZ_EDITOR_COMPONENT(EditorPatchComponent,"{0BE4015F-2C34-4FAA-999A-A758C69ED30B}",BaseClass);
    static void Reflect(AZ::ReflectContext* context)
    {
        BaseClass::Reflect(context);
        if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
        {
            sc->Class<EditorPatchComponent,BaseClass>()->Version(1);
            if(auto* edit=sc->GetEditContext())
            {
                edit->Class<EditorPatchComponent>("SilPOM Patch","Bounded planar heightfield. Local XY is the wall; +Z is outward.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData,"")
                    ->Attribute(AZ::Edit::Attributes::Category,"Graphics")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu,AZ_CRC_CE("Game"))
                    ->Attribute(AZ::Edit::Attributes::AutoExpand,true);
                edit->Class<PatchController>("Patch controller","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchController::m_configuration,"Configuration","")
                    ->Attribute(AZ::Edit::Attributes::Visibility,AZ::Edit::PropertyVisibility::ShowChildrenOnly);
                edit->Class<PatchConfig>("Patch configuration","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_material,"Material","SilPOM material with a linear single-mip height image")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_width,"Width","Local metres, follows entity scale")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_height,"Height","Local metres, follows entity scale")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_heightScale,"Height scale (world metres)","Unaffected by entity scale")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_overrideReliefFade,"Override relief fade",
                        "Use this patch's fade distances on materials that support relief fading")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_reliefFadeStart,"Relief fade start (m)",
                        "Distance from the panel up to which full relief is used")
                    ->Attribute(AZ::Edit::Attributes::Min,0.0f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_reliefFadeEnd,"Relief fade end (m)",
                        "Distance at which relief is flat. Set at or below the start to disable fading.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.0f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_reference,"Reference height","Normalized source value at the base plane")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_tileU,"Tile U","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_tileV,"Tile V","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_offsetU,"Offset U","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_offsetV,"Offset V","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_maxCells,"Maximum cells","Exhaustion is displayed in magenta")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_addressMode,"Address mode","0 repeat; 1 clamp")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_reuseTexels,"Reuse adjacent height texels","Exact optimized traversal; disable only for frozen A/B baseline")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_debug,"Debug","0 shaded; 1 normal; 2 cell budget; 3 UV; 4 depth; 5 hit mask. Magenta exhausted; yellow invalid.");
            }
        }
    }
};
class EditorMeshComponent final : public AZ::Render::EditorRenderComponentAdapter<MeshController,MeshComponent,MeshConfig>,
    private AZ::TickBus::Handler
{
public:
    using BaseClass=AZ::Render::EditorRenderComponentAdapter<MeshController,MeshComponent,MeshConfig>;
    AZ_EDITOR_COMPONENT(EditorMeshComponent,"{D88524B3-CC98-4A85-8E3B-470FE888B61B}",BaseClass);
    void Activate() override { BaseClass::Activate(); AZ::TickBus::Handler::BusConnect(); }
    void Deactivate() override { AZ::TickBus::Handler::BusDisconnect(); BaseClass::Deactivate(); }
    AZStd::string Status() const { return m_controller.GetStatus(); }
    static void Reflect(AZ::ReflectContext* context)
    {
        BaseClass::Reflect(context);
        if(auto* sc=azrtti_cast<AZ::SerializeContext*>(context))
        {
            sc->Class<EditorMeshComponent,BaseClass>()->Version(1);
            if(auto* edit=sc->GetEditContext())
            {
                edit->Class<EditorMeshComponent>("SilPOM Mesh","Imported static mesh: ordinary faces and planar displaced faces.")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData,"")
                    ->Attribute(AZ::Edit::Attributes::Category,"Graphics")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu,AZ_CRC_CE("Game"))
                    ->Attribute(AZ::Edit::Attributes::AutoExpand,true)
                    ->UIElement(AZ::Edit::UIHandlers::Label,"Status","")
                    ->Attribute(AZ::Edit::Attributes::ValueText,&EditorMeshComponent::Status);
                edit->Class<MeshController>("Mesh controller","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshController::m_configuration,"Configuration","")
                    ->Attribute(AZ::Edit::Attributes::Visibility,AZ::Edit::PropertyVisibility::ShowChildrenOnly);
                edit->Class<MeshConfig>("Mesh settings","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_surface,"Mesh surface","Export .fbx and .silpom.json together; select the compiled .silpommesh")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_materials,"Materials","One assignment per authored material identity")
                    ->DataElement(AZ::Edit::UIHandlers::Slider,&MeshConfig::m_reliefDepthMultiplier,"Relief depth multiplier",
                        "Scales the displacement of all trained profiles together. 0 is flat and 1 uses the authored depths.")
                    ->Attribute(AZ::Edit::Attributes::Visibility,&MeshConfig::GetReliefDepthVisibility)
                    ->Attribute(AZ::Edit::Attributes::Min,0.0f)
                    ->Attribute(AZ::Edit::Attributes::Max,MeshConfig::MaxReliefDepthMultiplier)
                    ->Attribute(AZ::Edit::Attributes::Step,0.01f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_profiles,"Displacement profiles","Profile IDs come from Blender face tags")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_maxCells,"Maximum cells","Planar traversal budget per ray; exhaustion is magenta")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_useDepthHeightBounds,"Depth height bounds",
                        "Experimental block skipping with depth-hit reuse, clamped R32F or square power-of-two repeating R32F heights, and a budget of at least 4096 cells")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_useNeuralSeed,"Neural depth seed",
                        "Use the Irish Fieldstone neural seed on the DX12 depth-hit path for clamped profiles at 0.02 m and reference 0.05")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_useLevelTwoPredictor,"Level 2 predictor (experimental)","Eight height probes and two learned candidate seeds for the open-shaft profiles. More GPU work is possible. The nearer validated hit wins; the second seed uses one retry. Off preserves the Level 1 baseline.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_useLevelFourWallMaterial,"Level 4A wall material (experimental)","Decode supported wall color, detail normal and roughness from a trained mip atlas. Requires neural-only visibility. Performance is unmeasured; off preserves ordinary textures.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_useR16HeightTextures,"R16 height textures (less GPU memory)","Experimental: use shared runtime R16_UNORM copies of R32 height maps. Halves their height payload; can reduce texture bandwidth. Off preserves source formats. Source assets are unchanged.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_useValidationGather,"Gather validation (may be cheaper)","Experimental: fetch each validation height quad with GatherRed. Exact loads at addressing borders. Does not change the neural estimator, iterations, retries or tolerance. GPU benefit depends on hardware.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_validationIterations,"Validation iterations (lower = cheaper)","Maximum iterations per no-march solve. Baseline 6; try 4. Lower values can increase fallback coverage.")
                    ->Attribute(AZ::Edit::Attributes::Min,1)->Attribute(AZ::Edit::Attributes::Max,16)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_validationRetryCap,"Validation retry cap (lower = cheaper)","Retries after the first no-march solve. Baseline 4; walls retain their one-retry limit. Try 1; 0 disables retries.")
                    ->Attribute(AZ::Edit::Attributes::Min,0)->Attribute(AZ::Edit::Attributes::Max,4)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_validationToleranceMultiplier,"Validation tolerance (higher may be cheaper)","No-march convergence tolerance multiplier. Baseline 1. Higher values may finish sooner but reduce hit accuracy.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.01f)->Attribute(AZ::Edit::Attributes::Max,100.00f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_reliefLodPixelBudget,"Relief LOD pixel budget (higher = cheaper)","0 preserves the current mode (4 pixels for the tower). Higher values switch to ordinary geometry sooner. Uses the existing 656-line reference.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.00f)->Attribute(AZ::Edit::Attributes::Max,64.00f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_maxReliefDistance,"Maximum relief distance (closer = cheaper)","0 uses automatic LOD only. Positive values cap the near relief draw at this view depth in metres, including the transition overlap. LOD modes only.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.00f)->Attribute(AZ::Edit::Attributes::Max,100000.00f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_validationDistanceStart,"Validation distance fade start","Camera distance to the face origin in metres where validation begins transitioning toward distant settings.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.00f)->Attribute(AZ::Edit::Attributes::Max,100000.00f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_validationDistanceEnd,"Validation distance fade end (0 = off)","0 disables distance-based validation. Otherwise must exceed fade start; distant settings apply fully beyond this camera distance.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.00f)->Attribute(AZ::Edit::Attributes::Max,100000.00f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_distantValidationIterations,"Distant validation iterations (lower = cheaper)","Maximum solve iterations beyond fade end. Interpolated from the normal validation limit between start and end.")
                    ->Attribute(AZ::Edit::Attributes::Min,1)->Attribute(AZ::Edit::Attributes::Max,16)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_distantValidationRetryCap,"Distant validation retry cap (lower = cheaper)","Retry cap beyond fade end. 0 disables retries; existing wall and confidence limits still apply.")
                    ->Attribute(AZ::Edit::Attributes::Min,0)->Attribute(AZ::Edit::Attributes::Max,4)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_distantValidationToleranceMultiplier,"Distant validation tolerance (higher may be cheaper)","Convergence tolerance multiplier beyond fade end. Higher values can finish sooner but accept less accurate hits.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.01f)->Attribute(AZ::Edit::Attributes::Max,100.00f)
                    ->DataElement(AZ::Edit::UIHandlers::ComboBox,&MeshConfig::m_localShadowMethod,"Local shadow method",
                        "Affects local relief visibility for spiral and directional lights. External shadows and camera intersections are unchanged.")
                    ->EnumAttribute(LocalShadowMethod::Baseline,"March (baseline)")
                    ->EnumAttribute(LocalShadowMethod::Gather,"March with Gather (may be cheaper)")
                    ->EnumAttribute(LocalShadowMethod::Neural,"Hybrid neural + march (reference)")
                    ->EnumAttribute(LocalShadowMethod::NeuralOnly,"Level 3 neural only (fixed probes; experimental)")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_neuralShadowConfidence,"Hybrid shadow confidence (higher = safer)",
                        "Historical hybrid only. Higher values use more fallback. Neural only uses calibrated binary thresholds and has no march or uncertainty fallback.")
                    ->Attribute(AZ::Edit::Attributes::Min,.9f)->Attribute(AZ::Edit::Attributes::Max,.99999f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_spiralLightShadowSteps,"Spiral-light shadow samples (lower = cheaper)","Samples per spiral-light shadow ray. 0 disables these local shadows while retaining the light. Only affects materials receiving spiral light.")
                    ->Attribute(AZ::Edit::Attributes::Min,0)->Attribute(AZ::Edit::Attributes::Max,64)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_useHeightDerivedNormals,"Height-derived normals (off = cheaper)","Calculate lighting normals from the displacement height gradient. Off uses the face normal and retains the material normal map. Does not disable intersection validation.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_reliefShadowSteps,"Relief shadow steps","Local directional-light relief shadows: 0 disables, 1-64 samples. External shadows always use the base mesh.")
                    ->Attribute(AZ::Edit::Attributes::Min,0)->Attribute(AZ::Edit::Attributes::Max,64)
                    ->DataElement(AZ::Edit::UIHandlers::ComboBox,&MeshConfig::m_spiralLightMode,"Spiral light",
                        "Off disables the first and all additional spiral windows. Selected materials respects Receive spiral light. All SILPOM surfaces illuminates every displaced batch; ordinary geometry, including the stairs, is unchanged.")
                    ->EnumAttribute(SpiralLightMode::Off,"Off (cheaper)")
                    ->EnumAttribute(SpiralLightMode::SelectedMaterials,"Selected materials")
                    ->EnumAttribute(SpiralLightMode::AllSilPomSurfaces,"All SILPOM surfaces")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_spiralLightBrightness,"Spiral light master brightness",
                        "Multiplier for the first window and every additional spiral window together. 1 preserves their lux values, 2 doubles them, 0 mutes all spiral light and its local shadow work. Ordinary stairs are unchanged.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.0f)->Attribute(AZ::Edit::Attributes::Max,100.0f)
                    ->Attribute(AZ::Edit::Attributes::Step,0.1f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_spiralLightEnd,"Spiral light top fade",
                        "X: local end height in metres, where spiral light reaches zero. Y: fade distance below that height, using a smooth curve. Increase Y for a softer cutoff; Y=0 disables the height limit. Applies to every spiral window.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_spiralLightPath,"Spiral light path",
                        "Window XY, stair corridor centre height, and signed rise per turn in mesh space.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_spiralLightPeakLux,"Spiral light peak lux",
                        "Brightness of the first spiral window only. Additional windows have independent peak lux; use Spiral light = Off to disable all of them.")
                    ->Attribute(AZ::Edit::Attributes::Min,0.0f)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_spiralLightWindows,"Additional spiral windows",
                        "For each window, X is signed turns from the first window and Y is peak lux.");
                edit->Class<MeshMaterialBinding>("Material slot","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshMaterialBinding::m_id,"Authored material ID","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshMaterialBinding::m_material,"Material","StandardPBR material; planar face variant is managed automatically")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshMaterialBinding::m_lightingChannelMask,
                        "Lighting channel mask","Direct lights affect this material's faces when they share a channel. The default mask is 1.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshMaterialBinding::m_disableEnvironmentLighting,
                        "Disable environment lighting","Turn off image-based and ambient lighting for this material binding.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshMaterialBinding::m_receiveSpiralLight,
                        "Receive spiral light","Applies to this binding's relief faces in Selected materials mode. All SILPOM surfaces overrides this flag; ordinary geometry never receives this effect.");
                edit->Class<MeshProfileBinding>("Displacement profile","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_id,"Profile ID","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_useBaseMesh,"Use base mesh",
                        "Shade these faces using the authored material on the original mesh, including StandardPBR parallax.")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_height,"Height image","Normalized linear single-mip scalar image")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_neuralCache,"Neural hit cache",
                        "Optional baked Irish Fieldstone tower predictions for this height atlas")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_scale,"Displacement (world metres)",
                        "Independent of entity scale. For a trained profile, adjust the relief depth multiplier instead.")
                    ->Attribute(AZ::Edit::Attributes::ReadOnly,&MeshProfileBinding::IsTrained)
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_reference,"Reference height","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_tiling,"UV tiling","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_offset,"UV offset","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_addressMode,"Address mode","0 repeat; 1 clamp")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_neuralModel,"Neural model",
                        "0 legacy or disabled. 1–3 welded tower. 4–7 open-shaft walls, stairs, trim and reveal.");
            }
        }
    }
private:
    AZ::u32 OnConfigurationChanged() override
    {
        // The generic adapter deactivates the controller for every property
        // edit, which would destroy its last-known-good generation.
        const auto config = m_controller.GetConfiguration();
        m_controller.SetConfiguration(config);
        return AZ::Edit::PropertyRefreshLevels::AttributesAndValues;
    }
    void OnTick(float,AZ::ScriptTimePoint) override
    {
        const auto& config=m_controller.GetConfiguration();
        const size_t bindingCount=config.m_materials.size()+config.m_profiles.size();
        const auto status=Status();
        if(status!=m_lastStatus||bindingCount!=m_lastSlots)
        {
            const bool shapeChanged=bindingCount!=m_lastSlots;
            m_lastSlots=bindingCount;m_lastStatus=status;
            if(shapeChanged) SetDirty();
            AzToolsFramework::ToolsApplicationNotificationBus::Broadcast(
                &AzToolsFramework::ToolsApplicationEvents::InvalidatePropertyDisplayForComponent,
                AZ::EntityComponentIdPair(GetEntityId(),GetId()),
                shapeChanged?AzToolsFramework::Refresh_EntireTree:AzToolsFramework::Refresh_AttributesAndValues);
        }
    }
    AZStd::string m_lastStatus;
    size_t m_lastSlots=0;
};
class EditorModule final : public Module
{
public:
    AZ_RTTI(EditorModule,"{4C784C12-E407-421E-A060-D9C669FCDB54}",Module);
    AZ_CLASS_ALLOCATOR(EditorModule,AZ::SystemAllocator);
    EditorModule()
    {
        m_descriptors.push_back(EditorPatchComponent::CreateDescriptor());
        m_descriptors.push_back(PlanarBenchmark::CreateDescriptor());
        m_descriptors.push_back(EditorMeshComponent::CreateDescriptor());
        m_descriptors.push_back(CreateEditorPaintDescriptor());
        m_descriptors.push_back(DecalPainterSystem::CreateDescriptor());
    }
    AZ::ComponentTypeList GetRequiredSystemComponents() const override
    {return {azrtti_typeid<PlanarBenchmark>(),azrtti_typeid<MeshAssetSystem>(),azrtti_typeid<DecalPainterSystem>()};}
};
}
AZ_DECLARE_MODULE_CLASS(Gem_SilPOM_Editor,SilPOM::EditorModule)
