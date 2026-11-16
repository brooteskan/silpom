// SPDX-License-Identifier: MIT
// Included in DecalPaint.cpp, inside namespace SilPOM.
void PaintController::Rebuild()
{
    const auto started=std::chrono::steady_clock::now();
    using Data=AZ::Render::DecalData;
    using Projection=PaintProjection;
    static_assert(sizeof(Projection)==96);
    std::vector<Projection> decals;
    if(!m_bakeCache)m_bakeCache=AZStd::make_shared<PaintBakeCache>();
    m_bakeCache->Begin();
    AZStd::unordered_set<AZ::Data::AssetId> needed;
    AZStd::unordered_set<AZ::Data::AssetId> assets;
    m_liveCount=0;m_compositeCount=0;
    bool waiting=false;size_t references=0,bufferBytes=0;
    auto append=[&](const PaintDab& dab,float opacity,bool fixed,AZ::u32 layer)
    {
        if(fixed)++m_compositeCount;else ++m_liveCount;
        if((dab.m_heightMask&7u)==0)return;
        if(!dab.m_material.GetId().IsValid())return;
        if(needed.insert(dab.m_material.GetId()).second)
        {
            assets.insert(dab.m_material.GetId());
            if(dab.m_material.IsReady())for(const auto& value:dab.m_material->GetPropertyValues())
                if(value.Is<AZ::Data::Asset<AZ::RPI::ImageAsset>>())
                {
                    const auto id=value.GetValue<AZ::Data::Asset<AZ::RPI::ImageAsset>>().GetId();
                    if(id.IsValid())assets.insert(id);
                }
        }
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
        decals.push_back({data,PaintMath::BrushFlags(dab.m_erase,dab.m_heightMask),layer,0,fixed?1u:0u});
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
    for(auto id:m_bakeAssets)if(!assets.contains(id))AZ::Data::AssetBus::MultiHandler::BusDisconnect(id);
    for(auto id:assets)if(!m_bakeAssets.contains(id))AZ::Data::AssetBus::MultiHandler::BusConnect(id);
    m_bakeAssets=AZStd::move(assets);
    for(auto it=m_materialHandles.begin();it!=m_materialHandles.end();)
    {
        if(!needed.contains(it->first)){m_processor->ReleaseDecal(it->second);it=m_materialHandles.erase(it);}
        else ++it;
    }
    if(decals.empty())
    {
        for(auto& batch:m_batches)m_processor->ReleaseDecal(batch);m_batches.clear();m_buffer={};
    }
    else
    {
        const bool alphaOnly=!r_silpomPaintCompositeReference&&std::all_of(decals.begin(),decals.end(),[](const Projection& p)
            {const auto& d=p.m_data;return (p.m_flags&1u)||(d.m_blendMode==AZ::Render::DecalBlendMode::Alpha&&d.m_normalMapOpacity==0);});
        const bool filtered=std::any_of(decals.begin(),decals.end(),[](const Projection& p){return ((p.m_flags>>8)&7u)!=7u;});
        // Front-to-back alpha can stop after opacity saturates. Other blends
        // and normal paint retain their original forward evaluation order.

        std::vector<PaintMath::Bounds> bounds;bounds.reserve(decals.size());
        for(const auto& projection:decals)
        {
            const auto& d=projection.m_data;const AZ::Vector3 position=AZ::Vector3::CreateFromFloat3(d.m_position.data());
            const auto q=AZ::Quaternion::CreateFromFloat4(d.m_quaternion.data());
            PaintMath::Bounds b;
            for(int z=-1;z<=1;z+=2)for(int y=-1;y<=1;y+=2)for(int x=-1;x<=1;x+=2)
            {
                const auto v=position+q.TransformVector(AZ::Vector3(x*d.m_halfSize[0],y*d.m_halfSize[1],z*d.m_halfSize[2]));
                b.Add(PaintMath::Vec{v.GetX(),v.GetY(),v.GetZ()});
            }
            const PaintMath::Vec epsilon{.0001f,.0001f,.0001f};b.low=b.low-epsilon;b.high=b.high+epsilon;
            bounds.push_back(b);
        }
        const auto groups=PaintMath::BuildSpatialGroups(bounds,r_silpomPaintSpatialGrouping&&!r_silpomPaintCompositeReference);
        const bool clipped=groups.size()>1;
        AZStd::vector<AZ::u8> bytes;
        std::vector<AZ::u32> headers;headers.reserve(groups.size());
        static_assert(sizeof(Data)==80);
        for(const auto& group:groups)
        {
            const auto base=aznumeric_cast<AZ::u32>(bytes.size());headers.push_back(base);
            std::vector<Projection> local;local.reserve(group.projections.size());
            for(auto i:group.projections)local.push_back(decals[i]);
            std::vector<PaintMath::Bounds> localBounds;localBounds.reserve(local.size());
            for(auto i:group.projections)localBounds.push_back(PaintMath::Intersection(bounds[i],group.clip));
            const size_t perBakeBudget=size_t(AZStd::clamp(AZ::u32(r_silpomPaintBakeMiB),1u,1024u))*1024*1024/
                (groups.size()*AZStd::max(size_t(1),m_configuration.m_layers.size()));
            const auto bakes=m_bakeCache->Prepare(local,localBounds,group.clip,perBakeBudget,
                AZStd::clamp(float(r_silpomPaintBakeDensity),1.f,1024.f),r_silpomPaintBake&&!r_silpomPaintCompositeReference,m_bakeRequested);
            const bool reverse=alphaOnly;
            if(reverse){std::reverse(local.begin(),local.end());std::reverse(localBounds.begin(),localBounds.end());}
            for(auto& p:local)p.m_reserved=0;
            references+=local.size();
            std::vector<PaintMath::CompositeNode> nodes;nodes.reserve(local.size()*2);
            PaintMath::BuildCompositeNodes(nodes,0,aznumeric_cast<AZ::u32>(local.size()),
                [&](AZ::u32 i){return localBounds[i];});
            std::vector<AZ::u32> leafNodes(local.size());
            for(AZ::u32 i=0;i<nodes.size();++i)if(nodes[i].leaf<local.size())leafNodes[nodes[i].leaf]=i;
            for(size_t first=0;first<local.size();)
            {
                size_t end=first+1;while(end<local.size()&&local[end].m_layer==local[first].m_layer)++end;
                const AZ::u32 endNode=end==local.size()?aznumeric_cast<AZ::u32>(nodes.size()):leafNodes[end];
                for(size_t i=first;i<end;++i)local[i].m_layerEndNode=endNode;
                first=end;
            }
            if(!bakes.empty())
            {
                AZ::u32 firstLeaf=0;
                for(size_t i=nodes.size();i-->0;)
                    if(nodes[i].leaf<local.size())firstLeaf=nodes[i].leaf;
                    else nodes[i].leaf=0x80000000u|firstLeaf;
            }
            const AZ::u32 header[4]={aznumeric_cast<AZ::u32>(nodes.size()),aznumeric_cast<AZ::u32>(base+16+nodes.size()*32),
                (r_silpomPaintCompositeReference?2u:(reverse?1u:0u))|(filtered?4u:0u)|(clipped?8u:0u)|(bakes.empty()?0u:16u),aznumeric_cast<AZ::u32>(local.size())};
            const size_t end=header[1]+local.size()*96;
            const size_t bakeInfo=end+(clipped?32:0);
            bytes.resize(bakeInfo+(bakes.empty()?0:16+bakes.size()*sizeof(PaintBakeRecord)));
            memcpy(bytes.data()+base,header,16);memcpy(bytes.data()+base+16,nodes.data(),nodes.size()*32);
            memcpy(bytes.data()+header[1],local.data(),local.size()*96);
            if(clipped)
            {
                const PaintMath::CompositeNode clip{group.clip.low,group.inclusiveHigh,group.clip.high,0};
                memcpy(bytes.data()+end,&clip,32);
            }
            if(!bakes.empty())
            {
                const AZ::u32 info[4]={aznumeric_cast<AZ::u32>(bakes.size()),aznumeric_cast<AZ::u32>(bakeInfo+16),0,0};
                memcpy(bytes.data()+bakeInfo,info,16);
                for(size_t i=0;i<bakes.size();++i)
                {
                    auto record=bakes[reverse?bakes.size()-1-i:i]->record;
                    auto* entry=bakes[reverse?bakes.size()-1-i:i];
                    const auto first=reverse?local.size()-entry->end:entry->first,endLeaf=reverse?local.size()-entry->first:entry->end;
                    record.reserved=aznumeric_cast<AZ::u32>(first);record.reserved2=aznumeric_cast<AZ::u32>(endLeaf);
                    record.layer=local[first].m_layer;
                    record.flags=(record.flags&2u)|(r_silpomPaintBakeDebug?1u:0u)|(AZ::u32(r_silpomPaintBakeDebug)>1u?4u:0u);
                    record.endNode=endLeaf==local.size()?aznumeric_cast<AZ::u32>(nodes.size()):leafNodes[endLeaf];
                    memcpy(bytes.data()+bakeInfo+16+i*sizeof(record),&record,sizeof(record));
                }
            }
        }
        bufferBytes=bytes.size();
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
                // Acquire the complete set before publishing any handles. A
                // transient allocation failure retains the previous valid image.
                AZStd::vector<AZ::Render::DecalFeatureProcessorInterface::DecalHandle> acquired;
                bool allocationFailed=false;
                while(m_batches.size()+acquired.size()<groups.size())
                {
                    const auto h=m_processor->AcquireDecal();
                    if(!h.IsValid()){waiting=true;allocationFailed=true;break;}acquired.push_back(h);
                }
                if(allocationFailed){for(auto& h:acquired)m_processor->ReleaseDecal(h);}
                else
                {
                    m_batches.insert(m_batches.end(),acquired.begin(),acquired.end());
                    while(m_batches.size()>groups.size()){m_processor->ReleaseDecal(m_batches.back());m_batches.pop_back();}
                    for(size_t i=0;i<groups.size();++i)
                    {
                        Data batch;const auto& b=groups[i].bounds;
                        batch.m_position={(b.low.x+b.high.x)*.5f,(b.low.y+b.high.y)*.5f,(b.low.z+b.high.z)*.5f};
                        // Pad native culling bounds against center/extent rounding;
                        // the shader uses the exact, shared split-plane values.
                        batch.m_halfSize={(b.high.x-b.low.x)*.5f+.0001f,(b.high.y-b.low.y)*.5f+.0001f,(b.high.z-b.low.z)*.5f+.0001f};
                        batch.m_blendMode=AZ::Render::DecalBlendMode::Composite;batch.m_normalMapOpacity=0;
                        batch.m_textureArrayIndex=indices.begin()->second;batch.m_textureIndex=headers[i];
                        m_processor->SetDecalData(m_batches[i],batch);
                        m_processor->SetDecalPosition(m_batches[i],AZ::Vector3::CreateFromFloat3(batch.m_position.data()));
                    }
                    m_buffer=AZStd::move(buffer);
                }
            }
            else waiting=true;
        }
        else waiting=true;
    }
    m_bakeCache->End();
    const double milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    m_renderStats=AZStd::string::format("{\"batches\":%u,\"materials\":%zu,\"unique_projections\":%zu,\"projection_references\":%zu,\"gpu_buffers\":%u,\"gpu_bytes\":%zu,\"rebuild_ms\":%.6f,\"baked_prefixes\":%zu,\"bake_pending\":%zu,\"bake_failed\":%zu,\"bake_bytes\":%zu,\"bake_wall_ms\":%.3f}",
        GetRenderHandleCount(),m_materialHandles.size(),decals.size(),references,m_buffer?1u:0u,bufferBytes,milliseconds,m_bakeCache->Count(3),m_bakeCache->Count(0)+m_bakeCache->Count(1)+m_bakeCache->Count(2),m_bakeCache->Count(4),m_bakeCache->Bytes(),m_bakeCache->WallMilliseconds());
    m_status=AZStd::string::format("%u live dabs / 100; %u in Composite; %u render batches%s",
        m_liveCount,m_compositeCount,GetRenderHandleCount(),
        waiting?"; waiting for brush material / GPU buffer":"");
}
