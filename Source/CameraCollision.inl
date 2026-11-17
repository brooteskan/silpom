// SPDX-License-Identifier: MIT
// Included by Mesh.cpp after Generation is defined, inside SilPOM.
bool MeshController::SweepCameraSphere(const AZ::Vector3& start,const AZ::Vector3& end,float radius,
    float& fraction,AZ::Vector3& normal) const
{
    if(!m_active||!m_active->paintConfig.m_surface.IsReady()||!start.IsFinite()||!end.IsFinite()
        ||!std::isfinite(radius)||radius<=0)return false;
    namespace Math=CameraCollisionMath;
    auto vector=[](const AZ::Vector3& p){return Math::Vec{p.GetX(),p.GetY(),p.GetZ()};};
    Math::Hit hit;hit.fraction=std::clamp(double(fraction),0.,1.);bool found=false;
    const auto& config=m_active->paintConfig;
    const float multiplier=config.UsesTrainedProfiles()?config.m_reliefDepthMultiplier:1.f;
    for(const auto& face:config.m_surface->m_faces)
    {
        if(face.m_corners.size()<3)continue;
        float relief=0;
        for(const auto& profile:config.m_profiles)if(profile.m_id==face.m_profile&&!profile.m_useBaseMesh)
        {
            float direction=0;for(const auto& corner:face.m_corners)direction=std::max(direction,corner.m_direction.GetLength());
            // Relief remains in world metres, independently of base-mesh scale.
            // Use its full possible displacement envelope instead of sampling
            // height texels each frame; openings retain their original topology.
            relief=std::abs(profile.m_scale*multiplier)*std::max(std::abs(profile.m_reference),std::abs(1-profile.m_reference))*direction;
            break;
        }
        const float clearance=radius+relief;
        auto movementBounds=AZ::Aabb::CreateFromPoint(start);movementBounds.AddPoint(end);movementBounds.Expand(AZ::Vector3(clearance));
        auto bounds=AZ::Aabb::CreateNull();
        for(const auto& corner:face.m_corners)
            bounds.AddPoint(m_active->paintWorld.TransformPoint(corner.m_position*m_active->paintScale));
        if(!bounds.Overlaps(movementBounds))continue;
        const auto a=vector(m_active->paintWorld.TransformPoint(face.m_corners[0].m_position*m_active->paintScale));
        for(size_t i=1;i+1<face.m_corners.size();++i)
        {
            const auto b=vector(m_active->paintWorld.TransformPoint(face.m_corners[i].m_position*m_active->paintScale));
            const auto c=vector(m_active->paintWorld.TransformPoint(face.m_corners[i+1].m_position*m_active->paintScale));
            found=Math::SweepTriangle(vector(start),vector(end),clearance,a,b,c,hit)||found;
        }
    }
    if(found){fraction=float(hit.fraction);normal=AZ::Vector3(float(hit.normal.x),float(hit.normal.y),float(hit.normal.z));}
    return found;
}
