// SPDX-License-Identifier: MIT
#pragma once

#include <AzCore/Asset/AssetCommon.h>
#include <AzCore/Component/Component.h>
#include <AzCore/Math/Vector2.h>
#include <AzCore/Math/Vector3.h>
#include <AzCore/std/containers/vector.h>
#include <AzFramework/Asset/GenericAssetHandler.h>

namespace SilPOM
{
// Canonical, metre-space authoring data. GPU traversal layouts are deliberately
// not serialized here: height/UV/scale edits must not reinterpret stale bounds.
struct MeshCorner
{
    AZ_TYPE_INFO(MeshCorner, "{9AA9F112-175C-44FB-920A-21B71959DD1C}");
    AZ::Vector3 m_position = AZ::Vector3::CreateZero();
    AZ::Vector3 m_normal = AZ::Vector3::CreateAxisZ();
    AZ::Vector3 m_direction = AZ::Vector3::CreateAxisZ();
    AZ::Vector2 m_uv = AZ::Vector2::CreateZero();
    AZStd::string m_vertexId;
};
struct MeshFace
{
    AZ_TYPE_INFO(MeshFace, "{532A40C2-CA64-495D-B0CB-74DDA65B7FA7}");
    AZStd::string m_id, m_objectId, m_materialId;
    AZ::u32 m_region = 0, m_profile = 0;
    AZStd::vector<MeshCorner> m_corners;
    // Face indices in this canonical asset; -1 is an exterior boundary.
    AZStd::vector<AZ::s32> m_neighbors;
};
struct MeshMaterial
{
    AZ_TYPE_INFO(MeshMaterial, "{8D14A8B7-1233-4336-A5B1-91F059B6A2B7}");
    AZStd::string m_id, m_name;
};
class MeshSurfaceAsset final : public AZ::Data::AssetData
{
public:
    AZ_RTTI(MeshSurfaceAsset, "{58B07B13-4924-4BC5-94F1-AB4911658F23}", AZ::Data::AssetData);
    AZ_CLASS_ALLOCATOR(MeshSurfaceAsset, AZ::SystemAllocator);
    static void Reflect(AZ::ReflectContext* context);
    static constexpr AZ::u32 Version = 2; // generated tagged-fan displacement directions
    AZ::u32 m_surfaceVersion = Version;
    AZStd::string m_generation;
    AZStd::vector<MeshFace> m_faces;
    AZStd::vector<MeshMaterial> m_materials;
};

class MeshAssetSystem final : public AZ::Component
{
public:
    AZ_COMPONENT(MeshAssetSystem, "{1686228D-D21F-45CE-A77E-8A331865B0D0}");
    static void Reflect(AZ::ReflectContext* context);
    void Activate() override;
    void Deactivate() override;
private:
    AZStd::unique_ptr<AzFramework::GenericAssetHandler<MeshSurfaceAsset>> m_handler;
};
}
