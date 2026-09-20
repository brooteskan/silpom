// SPDX-License-Identifier: MIT
#include <SilPOM/MeshSurfaceAsset.h>
#include <AzCore/Serialization/SerializeContext.h>

namespace SilPOM
{
void MeshSurfaceAsset::Reflect(AZ::ReflectContext* context)
{
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
    {
        sc->Class<MeshCorner>()->Version(1)
            ->Field("Position", &MeshCorner::m_position)->Field("Normal", &MeshCorner::m_normal)
            ->Field("Direction", &MeshCorner::m_direction)->Field("UV", &MeshCorner::m_uv)
            ->Field("VertexId", &MeshCorner::m_vertexId);
        sc->Class<MeshFace>()->Version(1)->Field("Id", &MeshFace::m_id)
            ->Field("ObjectId", &MeshFace::m_objectId)->Field("MaterialId", &MeshFace::m_materialId)
            ->Field("Region", &MeshFace::m_region)->Field("Profile", &MeshFace::m_profile)
            ->Field("Corners", &MeshFace::m_corners)->Field("Neighbors", &MeshFace::m_neighbors);
        sc->Class<MeshMaterial>()->Version(1)->Field("Id", &MeshMaterial::m_id)->Field("Name", &MeshMaterial::m_name);
        sc->Class<MeshSurfaceAsset, AZ::Data::AssetData>()->Version(1)
            ->Field("SurfaceVersion", &MeshSurfaceAsset::m_surfaceVersion)
            ->Field("Generation", &MeshSurfaceAsset::m_generation)
            ->Field("Faces", &MeshSurfaceAsset::m_faces)->Field("Materials", &MeshSurfaceAsset::m_materials);
    }
}
void MeshAssetSystem::Reflect(AZ::ReflectContext* context)
{
    MeshSurfaceAsset::Reflect(context);
    if (auto* sc = azrtti_cast<AZ::SerializeContext*>(context))
        sc->Class<MeshAssetSystem, AZ::Component>()->Version(1);
}
void MeshAssetSystem::Activate()
{
    m_handler = AZStd::make_unique<AzFramework::GenericAssetHandler<MeshSurfaceAsset>>(
        "SilPOM Mesh Surface", "Graphics", "silpommesh");
    m_handler->Register();
}
void MeshAssetSystem::Deactivate()
{
    m_handler.reset();
}
}
