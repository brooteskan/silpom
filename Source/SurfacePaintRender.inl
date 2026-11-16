// SPDX-License-Identifier: MIT
// Included inside SilPOM by Mesh.cpp, after Generation and ReceiverUvPaint.inl.
struct MeshController::SurfacePaintRuntime
{
    struct Snapshot
    {
        struct Batch {AZ::u32 index=0;Material original,material;};
        AZStd::vector<Image> images;
        AZStd::vector<Batch> batches;
        AZ::Data::Instance<AZ::RPI::Buffer> table;
        size_t bytes=0,charts=0;
    };
    Generation* generation=nullptr;
    AZ::Data::Asset<AZ::RPI::MaterialAsset> templateAsset;
    AZStd::shared_ptr<Snapshot> published,pending;
    AZStd::vector<AZStd::pair<AZ::u64,AZStd::shared_ptr<Snapshot>>> retired;
    AZ::u64 fingerprint=0,publishedFingerprint=0,frame=0,retryFrame=0;
    bool requested=false;
    AZStd::string status="Waiting for surface texture material";
};

void MeshController::ReleaseSurfacePaintTextures()
{
    if(m_surfacePaintRuntime&&m_processor&&m_active&&m_surfacePaintRuntime->generation==m_active.get())
        if(const auto& snapshot=m_surfacePaintRuntime->published)
            for(const auto& batch:snapshot->batches)
                if(batch.index<m_active->handles.size())m_processor->SetCustomMaterials(m_active->handles[batch.index],batch.original);
    m_surfacePaintRuntime.reset();
}

AZStd::string MeshController::GetSurfacePaintStatus() const
{
    if(!m_surfacePaintRuntime)return "{\"state\":\"unbound\"}";
    const auto& runtime=*m_surfacePaintRuntime;
    const auto& snapshot=runtime.published;
    return AZStd::string::format("{\"state\":\"%s\",\"mode\":3,\"ready\":%s,\"charts\":%zu,\"bytes\":%zu,\"pending\":%s}",
        runtime.status.c_str(),snapshot&&runtime.publishedFingerprint==runtime.fingerprint&&!runtime.pending?"true":"false",
        snapshot?snapshot->charts:0,snapshot?snapshot->bytes:0,runtime.pending?"true":"false");
}

void MeshController::SyncSurfacePaintMaterials()
{
    if(!m_surfacePaintRuntime||m_surfacePaintRuntime->generation!=m_active.get())return;
    for(const auto& snapshot:{m_surfacePaintRuntime->published,m_surfacePaintRuntime->pending})
        if(snapshot)for(auto& batch:snapshot->batches)
        {
            const auto name=AZ::Name("surface.proxyLodBuffer");
            const auto from=batch.original->FindPropertyIndex(name),to=batch.material->FindPropertyIndex(name);
            if(!from.IsValid()||!to.IsValid())continue;
            const auto value=batch.original->GetPropertyValue(from);
            if(value.Is<AZ::u32>()&&batch.material->GetPropertyValue(to)!=value)
            {
                batch.material->SetPropertyValue(to,value);batch.material->Compile();
                if(auto srg=batch.material->GetShaderResourceGroup())srg->Compile();
            }
        }
}

void MeshController::TickSurfacePaintTextures()
{
    if(!m_processor||!m_active){ReleaseSurfacePaintTextures();return;}
    struct Document {AZ::EntityId entity;AZ::u64 revision;const PaintConfig* config;};
    AZStd::vector<Document> documents;
    PaintRequestBus::EnumerateHandlers([&](PaintRequests* canvas)
    {
        const auto& config=canvas->GetDocument();
        if(config.m_textureAuthoritative&&AZStd::any_of(config.m_surfacePaint.begin(),config.m_surfacePaint.end(),
            [&](const auto& chart){return chart.receiver==m_entity;}))
            documents.push_back({canvas->GetPaintEntity(),canvas->GetRevision(),&config});
        return true;
    });
    if(documents.empty()){ReleaseSurfacePaintTextures();return;}
    // Source ownership is surface data. Camera changes never choose or rebuild
    // these images; only document revisions and receiver generation do.
    AZStd::sort(documents.begin(),documents.end(),[](const auto& a,const auto& b){return AZ::u64(a.entity)<AZ::u64(b.entity);});
    AZ::u64 fingerprint=1469598103934665603ULL;
    for(const auto& document:documents)
    {fingerprint=(fingerprint^AZ::u64(document.entity))*1099511628211ULL;fingerprint=(fingerprint^document.revision)*1099511628211ULL;}
    if(m_surfacePaintRuntime&&m_surfacePaintRuntime->generation!=m_active.get())ReleaseSurfacePaintTextures();
    if(!m_surfacePaintRuntime)
    {
        m_surfacePaintRuntime=AZStd::make_unique<SurfacePaintRuntime>();auto& runtime=*m_surfacePaintRuntime;
        runtime.generation=m_active.get();
        AZ::Data::AssetId id;AZ::Data::AssetCatalogRequestBus::BroadcastResult(id,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,
            "materials/silpommeshuv.template.azmaterial",azrtti_typeid<AZ::RPI::MaterialAsset>(),false);
        if(id.IsValid())runtime.templateAsset=AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(id,AZ::Data::AssetLoadBehavior::QueueLoad);
    }
    auto& runtime=*m_surfacePaintRuntime;++runtime.frame;
    runtime.retired.erase(AZStd::remove_if(runtime.retired.begin(),runtime.retired.end(),
        [&](const auto& retired){return retired.first<=runtime.frame;}),runtime.retired.end());
    if(!runtime.templateAsset.IsReady())return;
    // An authoritative texture document supersedes the disposable prefix cache.
    // Legacy native projections are independent and are not removed here.
    if(m_uvPaint)ReleaseReceiverUvPaint();
    if(!runtime.requested||runtime.fingerprint!=fingerprint||(runtime.retryFrame&&runtime.frame>=runtime.retryFrame))
    {
        if(runtime.pending)runtime.retired.push_back({runtime.frame+8,runtime.pending});
        runtime.pending={};runtime.fingerprint=fingerprint;runtime.requested=true;runtime.retryFrame=0;
        auto next=AZStd::make_shared<SurfacePaintRuntime::Snapshot>();
        struct Entry {const SurfacePaintChart* chart;AZ::u32 hitFace,batch,source;float opacity;};
        AZStd::vector<Entry> entries;
        bool valid=ValidateSurfacePaintReceiver(m_entity,AZ::EntityId(),nullptr,runtime.status),retry=false;
        for(const auto& document:documents)
            for(const auto& layer:document.config->m_layers)
            {
                if(!std::isfinite(layer.m_opacity)){valid=false;runtime.status="Invalid surface paint layer opacity";break;}
                if(!layer.m_visible||layer.m_opacity<=0)continue;
                for(const auto& chart:document.config->m_surfacePaint)
                {
                    if(chart.receiver!=m_entity||chart.layer!=layer.m_id)continue;
                    const auto face=AZStd::find_if(m_active->paintFaces.begin(),m_active->paintFaces.end(),[&](const auto& f){return f.id==chart.face;});
                    if(face==m_active->paintFaces.end())continue; // Orphaned chart stays saved but cannot paint another face.
                    if(!chart.rectangle.IsFinite()||chart.rectangle.GetZ()<=chart.rectangle.GetX()||chart.rectangle.GetW()<=chart.rectangle.GetY())
                    {valid=false;runtime.status="Invalid surface texture rectangle";break;}
                    entries.push_back({&chart,face->hit,face->batch,face->source,AZStd::clamp(layer.m_opacity,0.f,1.f)});
                }
            }
        const auto pool=AZ::RPI::ImageSystemInterface::Get()->GetSystemStreamingPool();
        if(!pool){retry=valid;valid=false;runtime.status="Surface paint streaming pool is unavailable";}
        const size_t recordsEnd=16+entries.size()*48;
        AZStd::vector<AZ::u8> bytes(recordsEnd,0);
        const AZ::u32 header[4]={0x53505431u,aznumeric_cast<AZ::u32>(entries.size()),0,0};memcpy(bytes.data(),header,16);
        for(size_t index=0;valid&&index<entries.size();++index)
        {
            const auto& entry=entries[index];const auto& chart=*entry.chart;
            if(chart.resolution<SurfacePaintImage::MinResolution||chart.resolution>SurfacePaintImage::MaxResolution
                ||(chart.resolution&(chart.resolution-1u))!=0)
            {valid=false;runtime.status="Invalid surface texture resolution";break;}
            SurfacePaintImage::Image image;
            if(!SurfacePaintImage::Decode(std::string_view(chart.texels.data(),chart.texels.size()),image)
                ||image.width!=chart.resolution||image.height!=chart.resolution)
            {valid=false;runtime.status="Invalid authoritative surface texture payload";break;}
            if(entry.source>=m_active->paintConfig.m_surface->m_faces.size()
                ||m_active->paintConfig.m_surface->m_faces[entry.source].m_corners.size()!=3)
            {valid=false;runtime.status="Surface texture receiving triangle is unavailable";break;}
            const auto& corners=m_active->paintConfig.m_surface->m_faces[entry.source].m_corners;
            SurfacePaintImage::Triangle domain;
            const float inverseU=1/(chart.rectangle.GetZ()-chart.rectangle.GetX()),inverseV=1/(chart.rectangle.GetW()-chart.rectangle.GetY());
            for(unsigned i=0;i<3;++i)domain.corners[i]={
                (corners[i].m_uv.GetX()-chart.rectangle.GetX())*inverseU,
                (corners[i].m_uv.GetY()-chart.rectangle.GetY())*inverseV};
            const auto mips=SurfacePaintImage::GenerateMips(image,domain);
            if(mips.empty()){valid=false;runtime.status="Surface texture receiving domain has no texel samples";break;}
            const AZ::u32 record[4]={entry.hitFace,chart.front?1u:0u,aznumeric_cast<AZ::u32>(mips.size()),aznumeric_cast<AZ::u32>(bytes.size())};
            const AZ::Vector4 rectangle(chart.rectangle.GetX(),chart.rectangle.GetY(),1/(chart.rectangle.GetZ()-chart.rectangle.GetX()),1/(chart.rectangle.GetW()-chart.rectangle.GetY()));
            const float opacity[4]={entry.opacity,0,0,0};
            memcpy(bytes.data()+16+index*48,record,16);memcpy(bytes.data()+32+index*48,&rectangle,16);memcpy(bytes.data()+48+index*48,opacity,16);
            for(const auto& mip:mips)
            {
                std::vector<float> color(mip.pixels.size()*4),normal(color.size());
                for(size_t p=0;p<mip.pixels.size();++p)
                {
                    const auto& c=mip.pixels[p].color;const auto& n=mip.pixels[p].normal;
                    color[p*4]=c.premultiplied.x;color[p*4+1]=c.premultiplied.y;color[p*4+2]=c.premultiplied.z;color[p*4+3]=c.coverage;
                    normal[p*4]=n.premultiplied.x;normal[p*4+1]=n.premultiplied.y;normal[p*4+2]=n.premultiplied.z;normal[p*4+3]=n.coverage;
                }
                Image images[2]={AZ::RPI::StreamingImage::CreateFromCpuData(*pool,AZ::RHI::ImageDimension::Image2D,AZ::RHI::Size(mip.width,mip.height,1),
                    AZ::RHI::Format::R32G32B32A32_FLOAT,color.data(),color.size()*sizeof(float)),
                    AZ::RPI::StreamingImage::CreateFromCpuData(*pool,AZ::RHI::ImageDimension::Image2D,AZ::RHI::Size(mip.width,mip.height,1),
                    AZ::RHI::Format::R32G32B32A32_FLOAT,normal.data(),normal.size()*sizeof(float))};
                AZ::u32 mipRecord[4]={0,0,mip.width,mip.height};
                for(unsigned channel=0;channel<2;++channel)
                {
                    if(!images[channel]){valid=false;retry=true;runtime.status="Surface texture image allocation failed";break;}
                    const auto bindless=images[channel]->GetImageView()->GetBindlessReadIndex();
                    if(bindless.size()!=1||bindless.begin()->second==AZ::u32(-1)){valid=false;retry=true;runtime.status="Surface texture bindless image unavailable";break;}
                    mipRecord[channel]=bindless.begin()->second;next->images.push_back(images[channel]);
                }
                if(!valid)break;
                const auto end=bytes.size();bytes.resize(end+16);memcpy(bytes.data()+end,mipRecord,16);
                next->bytes+=color.size()*sizeof(float)+normal.size()*sizeof(float);
            }
            if(!valid)break;
            if(AZStd::none_of(next->batches.begin(),next->batches.end(),[&](const auto& b){return b.index==entry.batch;}))
            {
                auto original=m_active->materials[entry.batch],material=Snapshot(runtime.templateAsset);
                if(!material){valid=false;retry=true;runtime.status="Surface texture material allocation failed";break;}
                const auto layout=original->GetMaterialPropertiesLayout();const auto& values=original->GetPropertyValues();
                for(size_t property=0;property<values.size();++property)
                {
                    const auto to=material->FindPropertyIndex(layout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(property))->GetName());
                    if(to.IsValid())material->SetPropertyValue(to,values[property]);
                }
                next->batches.push_back({entry.batch,original,material});
            }
        }
        if(valid)
        {
            next->table=AZ::RPI::Buffer::FindOrCreate(Buffer(bytes.data(),bytes.size()/4,4,true));
            if(next->table)next->table->WaitForUpload();
            const auto bindless=next->table?next->table->GetBufferView()->GetBindlessReadIndex():AZStd::unordered_map<int,AZ::u32>{};
            if(bindless.size()!=1||bindless.begin()->second==AZ::u32(-1)){valid=false;retry=true;runtime.status="Surface texture table upload failed";}
            else for(auto& batch:next->batches)
            {
                if(!Set(batch.material,"uvPaint.mode",3u)||!Set(batch.material,"uvPaint.owner",0u)
                    ||!Set(batch.material,"uvPaint.atlasBuffer",bindless.begin()->second))
                {valid=false;runtime.status="Surface texture material contract mismatch";break;}
                batch.material->Compile();
            }
        }
        if(valid){next->charts=entries.size();runtime.pending=AZStd::move(next);runtime.status="Uploading authoritative surface textures";}
        // Resource pressure may clear without a document edit. Keep the last
        // published result and retry transient failures at most once per 60 ticks.
        else if(retry)runtime.retryFrame=runtime.frame+60;
    }
    SyncSurfacePaintMaterials();
    if(runtime.pending&&AZStd::none_of(runtime.pending->batches.begin(),runtime.pending->batches.end(),[](const auto& batch){return batch.material->NeedsCompile();}))
    {
        if(runtime.published)
        {
            for(const auto& old:runtime.published->batches)
                if(AZStd::none_of(runtime.pending->batches.begin(),runtime.pending->batches.end(),[&](const auto& batch){return batch.index==old.index;}))
                    m_processor->SetCustomMaterials(m_active->handles[old.index],old.original);
            runtime.retired.push_back({runtime.frame+8,runtime.published});
        }
        runtime.published=AZStd::move(runtime.pending);
        runtime.publishedFingerprint=runtime.fingerprint;
        for(const auto& batch:runtime.published->batches)m_processor->SetCustomMaterials(m_active->handles[batch.index],batch.material);
        runtime.status="Authoritative surface textures active";
    }
}
