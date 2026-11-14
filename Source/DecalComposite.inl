// SPDX-License-Identifier: MIT
// Included in DecalPaint.cpp, inside namespace SilPOM.
void PaintController::Rebuild()
{
    using Data=AZ::Render::DecalData;
    struct Projection {Data m_data;AZ::u32 m_flags=0,m_layer=0,m_layerEndNode=0,m_reserved=0;};
    static_assert(sizeof(Projection)==96);
    std::vector<Projection> decals;
    AZStd::unordered_set<AZ::Data::AssetId> needed;
    m_liveCount=0;m_compositeCount=0;
    bool waiting=false;
    auto append=[&](const PaintDab& dab,float opacity,bool fixed,AZ::u32 layer)
    {
        if(fixed)++m_compositeCount;else ++m_liveCount;
        if((dab.m_heightMask&7u)==0)return;
        if(!dab.m_material.GetId().IsValid())return;
        needed.insert(dab.m_material.GetId());
        auto it=m_materialHandles.find(dab.m_material.GetId());
        if(it==m_materialHandles.end())
        {
            const auto h=m_processor->AcquireDecal();if(!h.IsValid()){waiting=true;return;}
            Data anchor;anchor.m_opacity=0;anchor.m_normalMapOpacity=0;anchor.m_halfSize={0,0,0};
            m_processor->SetDecalData(h,anchor);m_processor->SetDecalPosition(h,AZ::Vector3::CreateZero());
            m_processor->SetDecalMaterial(h,dab.m_material.GetId());
            it=m_materialHandles.emplace(dab.m_material.GetId(),h).first;
        }
        Data data=m_processor->GetDecalData(it->second);
        if(data.m_textureArrayIndex==Data::UnusedIndex){waiting=true;return;}
        const auto& p=dab.m_point;
        if(!p.m_position.IsFinite()||!p.m_normal.IsFinite()||!std::isfinite(dab.m_diameter)||dab.m_diameter<=0
            ||!std::isfinite(dab.m_depth)||!std::isfinite(dab.m_rotation)||!std::isfinite(dab.m_opacity))return;
        const auto position=m_world.TransformPoint(p.m_position);
        auto normal=p.m_normal.GetNormalizedSafe();if(normal.GetLengthSq()<.5f)normal=AZ::Vector3::CreateAxisZ();
        const auto q=m_world.GetRotation()*AZ::Quaternion::CreateShortestArc(AZ::Vector3::CreateAxisZ(),normal)
            *AZ::Quaternion::CreateRotationZ(AZ::DegToRad(dab.m_rotation));
        const float half=.5f*dab.m_diameter*m_world.GetUniformScale();
        data.m_position={position.GetX(),position.GetY(),position.GetZ()};data.m_quaternion={q.GetX(),q.GetY(),q.GetZ(),q.GetW()};
        data.m_halfSize={half,half,AZStd::max(.001f,.5f*dab.m_depth*m_world.GetUniformScale())};
        data.m_opacity=AZStd::clamp(opacity*dab.m_opacity,0.f,1.f);
        data.m_normalMapOpacity=dab.m_erase?0:AZStd::clamp(opacity*dab.m_normalOpacity,0.f,1.f);
        data.m_decalColor={dab.m_color.GetR(),dab.m_color.GetG(),dab.m_color.GetB()};data.m_decalColorFactor=1;
        data.m_blendMode=static_cast<AZ::Render::DecalBlendMode>(AZStd::min(dab.m_blendMode,4u));data.m_sortKey=0;
        decals.push_back({data,PaintMath::BrushFlags(dab.m_erase,dab.m_heightMask),layer,0,0});
    };
    AZ::u32 layerIndex=0;
    for(const auto& layer:m_configuration.m_layers)
    {
        const AZ::u32 layerId=layerIndex++;
        if(!layer.m_visible||layer.m_opacity<=0)continue;
        AZStd::unordered_map<AZ::u64,AZStd::vector<const PaintDab*>> fixedRuns;
        AZStd::unordered_map<AZ::u64,const PaintStroke*> liveRuns;
        for(const auto& dab:layer.m_composite)if(dab.m_visible)fixedRuns[dab.m_strokeId].push_back(&dab);
        for(const auto& stroke:layer.m_strokes)if(stroke.m_visible)liveRuns[stroke.m_id]=&stroke;
        // Preserve the complete layer/stroke order, including fixed runs.
        for(const auto id:layer.m_order)
        {
            if(const auto fixed=fixedRuns.find(id);fixed!=fixedRuns.end())
                for(const auto* dab:fixed->second)append(*dab,layer.m_opacity,true,layerId);
            if(const auto live=liveRuns.find(id);live!=liveRuns.end())
                for(const auto& point:live->second->m_points)append(ResolveDab(*live->second,point),layer.m_opacity,false,layerId);
        }
    }
    for(auto it=m_materialHandles.begin();it!=m_materialHandles.end();)
    {
        if(!needed.contains(it->first)){m_processor->ReleaseDecal(it->second);it=m_materialHandles.erase(it);}
        else ++it;
    }
    if(decals.empty())
    {
        if(m_batch.IsValid()){m_processor->ReleaseDecal(m_batch);m_batch={};}m_buffer={};
    }
    else
    {
        const bool alphaOnly=!r_silpomPaintCompositeReference&&std::all_of(decals.begin(),decals.end(),[](const Projection& p)
            {const auto& d=p.m_data;return (p.m_flags&1u)||(d.m_blendMode==AZ::Render::DecalBlendMode::Alpha&&d.m_normalMapOpacity==0);});
        const bool filtered=std::any_of(decals.begin(),decals.end(),[](const Projection& p){return ((p.m_flags>>8)&7u)!=7u;});
        // Front-to-back alpha can stop after opacity saturates. Other blends
        // and normal paint retain their original forward evaluation order.
        if(alphaOnly)std::reverse(decals.begin(),decals.end());
        std::vector<PaintMath::CompositeNode> nodes;nodes.reserve(decals.size()*2);
        PaintMath::BuildCompositeNodes(nodes,0,aznumeric_cast<AZ::u32>(decals.size()),[&](AZ::u32 i)
        {
            const auto& d=decals[i].m_data;const AZ::Vector3 position=AZ::Vector3::CreateFromFloat3(d.m_position.data());
            const auto q=AZ::Quaternion::CreateFromFloat4(d.m_quaternion.data());
            PaintMath::Bounds b;
            for(int z=-1;z<=1;z+=2)for(int y=-1;y<=1;y+=2)for(int x=-1;x<=1;x+=2)
            {
                const auto v=position+q.TransformVector(AZ::Vector3(x*d.m_halfSize[0],y*d.m_halfSize[1],z*d.m_halfSize[2]));
                b.Add(PaintMath::Vec{v.GetX(),v.GetY(),v.GetZ()});
            }
            // Conservative against the different rounding of quaternion math
            // in CPU bounds and GPU projection tests at the volume boundary.
            const PaintMath::Vec epsilon{.0001f,.0001f,.0001f};b.low=b.low-epsilon;b.high=b.high+epsilon;
            return b;
        });
        static_assert(sizeof(Data)==80);
        std::vector<AZ::u32> leafNodes(decals.size());
        for(AZ::u32 i=0;i<nodes.size();++i)if(nodes[i].leaf<decals.size())leafNodes[nodes[i].leaf]=i;
        for(size_t first=0;first<decals.size();)
        {
            size_t end=first+1;while(end<decals.size()&&decals[end].m_layer==decals[first].m_layer)++end;
            const AZ::u32 endNode=end==decals.size()?aznumeric_cast<AZ::u32>(nodes.size()):leafNodes[end];
            for(size_t i=first;i<end;++i)decals[i].m_layerEndNode=endNode;
            first=end;
        }
        AZStd::vector<AZ::u8> bytes(16+nodes.size()*32+decals.size()*96);
        const AZ::u32 header[4]={aznumeric_cast<AZ::u32>(nodes.size()),aznumeric_cast<AZ::u32>(16+nodes.size()*32),(r_silpomPaintCompositeReference?2u:(alphaOnly?1u:0u))|(filtered?4u:0u),aznumeric_cast<AZ::u32>(decals.size())};
        memcpy(bytes.data(),header,16);memcpy(bytes.data()+16,nodes.data(),nodes.size()*32);
        memcpy(bytes.data()+header[1],decals.data(),decals.size()*96);
        AZ::RPI::BufferAssetCreator creator;creator.Begin(AZ::Uuid::CreateRandom());
        AZ::RHI::BufferDescriptor descriptor;descriptor.m_bindFlags=AZ::RHI::BufferBindFlags::ShaderRead|AZ::RHI::BufferBindFlags::InputAssembly;
        descriptor.m_byteCount=bytes.size();creator.SetBuffer(bytes.data(),bytes.size(),descriptor);
        creator.SetBufferViewDescriptor(AZ::RHI::BufferViewDescriptor::CreateRaw(0,aznumeric_cast<AZ::u32>(bytes.size())));
        creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
        AZ::Data::Asset<AZ::RPI::BufferAsset> asset;creator.End(asset);
        auto buffer=AZ::RPI::Buffer::FindOrCreate(asset);
        if(buffer)
        {
            buffer->WaitForUpload();const auto indices=buffer->GetBufferView()->GetBindlessReadIndex();
            if(indices.size()==1&&indices.begin()->second!=AZ::u32(-1))
            {
                if(!m_batch.IsValid())m_batch=m_processor->AcquireDecal();
                if(m_batch.IsValid())
                {
                    Data batch;const auto& b=nodes.front();
                    batch.m_position={(b.low.x+b.high.x)*.5f,(b.low.y+b.high.y)*.5f,(b.low.z+b.high.z)*.5f};
                    batch.m_halfSize={(b.high.x-b.low.x)*.5f,(b.high.y-b.low.y)*.5f,(b.high.z-b.low.z)*.5f};
                    batch.m_blendMode=AZ::Render::DecalBlendMode::Composite;batch.m_normalMapOpacity=0;
                    batch.m_textureArrayIndex=indices.begin()->second;batch.m_textureIndex=0;
                    m_processor->SetDecalData(m_batch,batch);
                    m_processor->SetDecalPosition(m_batch,AZ::Vector3::CreateFromFloat3(batch.m_position.data()));
                    m_buffer=AZStd::move(buffer);
                }
                else waiting=true;
            }
            else waiting=true;
        }
        else waiting=true;
    }
    m_status=AZStd::string::format("%u live dabs / 100; %u in Composite; %u render batch%s",m_liveCount,m_compositeCount,
        GetRenderHandleCount(),waiting?"; waiting for brush material / GPU buffer":"");
}
