// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/Component.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Entity/EditorEntityContextBus.h>
#include <AzCore/Component/TickBus.h>
namespace SilPOM
{
AZ::ComponentDescriptor* CreateEditorPaintDescriptor();
class DecalPainterSystem final : public AZ::Component,private AzToolsFramework::EditorEvents::Bus::Handler,
    private AzToolsFramework::EditorEntityContextNotificationBus::Handler,private AZ::TickBus::Handler
{
public:
    AZ_COMPONENT(DecalPainterSystem,"{187AB0BF-47BD-40D5-93EA-3A7E281B5421}");
    static void Reflect(AZ::ReflectContext*);
    void Activate() override;
    void Deactivate() override;
private:
    void NotifyRegisterViews() override;
    void OnPrepareForContextReset() override;
    void OnTick(float,AZ::ScriptTimePoint) override;
};
}
