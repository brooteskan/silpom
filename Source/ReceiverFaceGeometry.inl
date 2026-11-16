// SPDX-License-Identifier: MIT
// Shared with portable receiver-surface regression tests. Included inside SilPOM.
struct ReceiverFaceGeometry
{
    AZ::Vector3 origin,du,dv,normal,d[3];
    AZ::Vector2 uv[3];
    const MeshProfileBinding* profile=nullptr;
    float multiplier=1;
    HeightPixelFormat format=HeightPixelFormat::Unsupported;
    bool quantizeFloat32=false,useHeightNormals=true;
    bool Initialize(const MeshFace& f,const MeshConfig& config,float uniformScale)
    {
        if(f.m_corners.size()!=3||!std::isfinite(uniformScale)||uniformScale<=0)return false;
        for(const auto& p:config.m_profiles)if(p.m_id==f.m_profile){profile=&p;break;}
        if(!profile||!profile->m_height.IsReady())return false;
        const auto& desc=profile->m_height->GetImageDescriptor();
        switch(desc.m_format)
        {
        case AZ::RHI::Format::R8_UNORM: format=HeightPixelFormat::Unorm8;break;
        case AZ::RHI::Format::R16_UNORM: format=HeightPixelFormat::Unorm16;break;
        case AZ::RHI::Format::R32_FLOAT: format=HeightPixelFormat::Float32;break;
        default: return false;
        }
        const auto pixels=profile->m_height->GetSubImageData(0,0);
        const size_t stride=HeightPixelStride(format),w=desc.m_size.m_width,h=desc.m_size.m_height;
        if(!w||!h||w>pixels.size()/stride||h>pixels.size()/(w*stride)
            ||pixels.size()!=w*h*stride)return false;
        quantizeFloat32=config.m_useR16HeightTextures;
        useHeightNormals=config.m_useHeightDerivedNormals;
        for(unsigned i=0;i<3;++i){uv[i]=f.m_corners[i].m_uv;d[i]=f.m_corners[i].m_direction;}
        // The renderer bakes uniform scale into the base geometry, then uses a
        // rigid object transform. Relief height and its UV derivatives remain
        // in world metres; applying the full transform after relief scales it twice.
        origin=f.m_corners[0].m_position*uniformScale;
        const auto e1=f.m_corners[1].m_position*uniformScale-origin,e2=f.m_corners[2].m_position*uniformScale-origin;
        const auto a=uv[1]-uv[0],b=uv[2]-uv[0];const float det=a.GetX()*b.GetY()-a.GetY()*b.GetX();
        if(std::abs(det)<1e-10f)return false;
        du=(e1*b.GetY()-e2*a.GetY())/det;dv=(e2*a.GetX()-e1*b.GetX())/det;
        normal=e1.Cross(e2).GetNormalizedSafe();
        multiplier=config.UsesTrainedProfiles()?config.m_reliefDepthMultiplier:1.f;
        return normal.GetLengthSq()>0;
    }
    AZ::Vector3 Weights(AZ::Vector2 q) const
    {
        const auto a=uv[1]-uv[0],b=uv[2]-uv[0],c=q-uv[0];const float det=a.GetX()*b.GetY()-a.GetY()*b.GetX();
        const float y=(c.GetX()*b.GetY()-c.GetY()*b.GetX())/det,z=(a.GetX()*c.GetY()-a.GetY()*c.GetX())/det;
        return {1-y-z,y,z};
    }
    AZ::Vector3 Height(AZ::Vector2 q) const
    {
        const auto& desc=profile->m_height->GetImageDescriptor();const auto pixels=profile->m_height->GetSubImageData(0,0);
        const int w=desc.m_size.m_width,h=desc.m_size.m_height;
        if(w<=0||h<=0||pixels.empty())return AZ::Vector3::CreateZero();
        auto address=[&](int i,int n){return profile->m_addressMode?AZStd::clamp(i,0,n-1):(i%n+n)%n;};
        auto texel=[&](int x,int y)
        {
            const size_t index=size_t(address(y,h))*w+address(x,w);
            float value=0;
            if(!DecodeHeightPixel(pixels.data(),pixels.size(),index,format,value,quantizeFloat32))
                return std::numeric_limits<float>::quiet_NaN();
            return value;
        };
        const auto tiled=q*profile->m_tiling+profile->m_offset;
        const float x=tiled.GetX()*w-.5f,y=tiled.GetY()*h-.5f,fx=x-std::floor(x),fy=y-std::floor(y);
        const int ix=int(std::floor(x)),iy=int(std::floor(y));const float a=texel(ix,iy),b=texel(ix+1,iy)-a,c=texel(ix,iy+1)-a,e=texel(ix+1,iy+1)-a-b-c;
        const float scale=profile->m_scale*multiplier;
        return {scale*(a+b*fx+c*fy+e*fx*fy-profile->m_reference),scale*(b+e*fy)*w*profile->m_tiling.GetX(),scale*(c+e*fx)*h*profile->m_tiling.GetY()};
    }
    void Evaluate(AZ::Vector2 q,AZ::Vector3& position,AZ::Vector3& n) const
    {
        const auto weights=Weights(q),height=Height(q);const auto direction=d[0]*weights.GetX()+d[1]*weights.GetY()+d[2]*weights.GetZ();
        position=origin+du*(q.GetX()-uv[0].GetX())+dv*(q.GetY()-uv[0].GetY())+direction*height.GetX();
        if(!useHeightNormals){n=normal;return;}
        const float det=du.Dot(dv.Cross(normal));
        const auto base=(normal-dv.Cross(normal)*(height.GetY()/det)-normal.Cross(du)*(height.GetZ()/det)).GetNormalizedSafe();
        const auto target=direction.GetNormalizedSafe(),axis=normal.Cross(target);
        n=(base+axis.Cross(base)+axis.Cross(axis.Cross(base))/(1+normal.Dot(target))).GetNormalizedSafe();
    }
};
