// SPDX-License-Identifier: MIT
#include "Module.h"
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
                    ->DataElement(AZ::Edit::UIHandlers::Default,&PatchConfig::m_debug,"Debug","0 shaded; 1 normal");
            }
        }
    }
};
class EditorModule final : public Module
{
public:
    AZ_RTTI(EditorModule,"{4C784C12-E407-421E-A060-D9C669FCDB54}",Module);
    AZ_CLASS_ALLOCATOR(EditorModule,AZ::SystemAllocator);
    EditorModule() {m_descriptors.push_back(EditorPatchComponent::CreateDescriptor());}
};
}
AZ_DECLARE_MODULE_CLASS(Gem_SilPOM_Editor,SilPOM::EditorModule)
