// SPDX-License-Identifier: MIT
#include "Module.h"
#include "PlanarBenchmark.h"
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
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_profiles,"Displacement profiles","Profile IDs come from Blender face tags")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshConfig::m_maxCells,"Maximum cells","Planar traversal budget per ray; exhaustion is magenta");
                edit->Class<MeshMaterialBinding>("Material slot","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshMaterialBinding::m_id,"Authored material ID","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshMaterialBinding::m_material,"Material","StandardPBR material; planar face variant is managed automatically");
                edit->Class<MeshProfileBinding>("Displacement profile","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_id,"Profile ID","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_height,"Height image","Normalized linear single-mip scalar image")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_scale,"Displacement (world metres)","Independent of entity scale")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_reference,"Reference height","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_tiling,"UV tiling","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_offset,"UV offset","")
                    ->DataElement(AZ::Edit::UIHandlers::Default,&MeshProfileBinding::m_addressMode,"Address mode","0 repeat; 1 clamp");
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
    }
    AZ::ComponentTypeList GetRequiredSystemComponents() const override
    {return {azrtti_typeid<PlanarBenchmark>(),azrtti_typeid<MeshAssetSystem>()};}
};
}
AZ_DECLARE_MODULE_CLASS(Gem_SilPOM_Editor,SilPOM::EditorModule)
