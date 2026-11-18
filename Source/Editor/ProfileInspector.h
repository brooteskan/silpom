// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/Component/Component.h>
#include <AzCore/Component/EntityId.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>

namespace SilPOM
{
// Tools > SilPOM Profiles: what each profile ID of the selected SilPOM Mesh
// resolved to, and creating/assigning .silpomprofile files.
class ProfileInspectorSystem final : public AZ::Component, private AzToolsFramework::EditorEvents::Bus::Handler
{
public:
    AZ_COMPONENT(ProfileInspectorSystem, "{8F1E6C2B-3A47-4D9E-B6A1-5C0D2E7F4B93}");
    static void Reflect(AZ::ReflectContext*);
    void Activate() override;
    void Deactivate() override;
private:
    void NotifyRegisterViews() override;
};

// Implemented with the SilPOM Mesh editor component (EditorModule.cpp).
// The surface asset of an entity's SilPOM Mesh, if it has one.
bool GetMeshSurface(AZ::EntityId entity, AZ::Data::AssetId& surface);
// Sets the binding's "Trained profile override" for a profile ID (adding the
// binding if needed), as one undoable edit.
bool AssignProfileOverride(AZ::EntityId entity, AZ::u32 profileId, const AZ::Data::AssetId& profile, AZStd::string& error);
} // namespace SilPOM
