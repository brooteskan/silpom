// SPDX-License-Identifier: MIT
// Included by DecalPaint.cpp inside namespace SilPOM.
class PaintBakeRasterPass final : public AZ::RPI::FullscreenTrianglePass
{
public:
    AZ_RTTI(PaintBakeRasterPass,"{81C3C4C7-1517-476F-BB83-FF81912C203A}",AZ::RPI::FullscreenTrianglePass);
    AZ_CLASS_ALLOCATOR(PaintBakeRasterPass,AZ::SystemAllocator);
    explicit PaintBakeRasterPass(const AZ::RPI::PassDescriptor& descriptor):FullscreenTrianglePass(descriptor){}
    AZ::Data::Instance<AZ::RPI::AttachmentImage> m_images[5];
    AZStd::atomic_uint32_t m_draws{0};
protected:
    void BuildCommandListInternal(const AZ::RHI::FrameGraphExecuteContext& context) override
    {
        FullscreenTrianglePass::BuildCommandListInternal(context);++m_draws;
    }
    void BuildInternal() override
    {
        const char* slots[]={"Scale","Bias","Normal","Receiver","Status"};
        for(unsigned i=0;i<5;++i)if(m_images[i])AttachImageToSlot(AZ::Name(slots[i]),m_images[i]);
        FullscreenTrianglePass::BuildInternal();
    }
};
void PaintBakeCache::Entry::Remove()
{
    if(pass)pass->QueueForRemoval();if(finalize)finalize->QueueForRemoval();
    pass={};finalize={};source={};configured={};status={};for(auto& image:images)image={};
}
PaintBakeCache::~PaintBakeCache(){for(auto& [key,entry]:m_entries){(void)key;entry.Remove();}}
void PaintBakeCache::Begin(){m_used.clear();}
void PaintBakeCache::End()
{
    for(auto it=m_entries.begin();it!=m_entries.end();)
        if(!m_used.contains(it->first)){it->second.Remove();it=m_entries.erase(it);}else ++it;
    if(m_entries.empty())m_wallMilliseconds=0;
}
size_t PaintBakeCache::Count(unsigned stage) const
{return std::count_if(m_entries.begin(),m_entries.end(),[&](const auto& p){return p.second.stage==stage;});}
size_t PaintBakeCache::Bytes() const
{size_t bytes=0;for(const auto& [key,e]:m_entries){(void)key;bytes+=e.grid.Bytes()+8;}return bytes;}
std::vector<PaintBakeCache::Entry*> PaintBakeCache::Prepare(const std::vector<PaintProjection>& projections,
    const std::vector<PaintMath::Bounds>& boxes,const PaintMath::Bounds& clip,size_t budget,float density,bool enabled,bool createEntry)
{
    std::vector<Entry*> result;if(!enabled)return result;
    for(size_t first=0;first<projections.size();)
    {
        size_t layerEnd=first+1;while(layerEnd<projections.size()&&projections[layerEnd].m_layer==projections[first].m_layer)++layerEnd;
        size_t end=first;
        while(end<layerEnd&&projections[end].m_reserved==1&&
            ((projections[end].m_flags&1u)||PaintMath::BakeBlendSupported(unsigned(projections[end].m_data.m_blendMode))))++end;
        if(end-first>=32)
        {
            AZ::u64 key=1469598103934665603ULL;
            auto hash=[&](const void* p,size_t bytes){auto* b=static_cast<const AZ::u8*>(p);while(bytes--){key^=*b++;key*=1099511628211ULL;}};
            hash(&clip,sizeof(clip));hash(&density,sizeof(density));hash(&budget,sizeof(budget));
            // Hash semantic fields, never C++ padding or threaded-node links.
            // Padding can differ after a feature-processor vector relocation.
            for(size_t i=first;i<end;++i)
            {
                const auto& p=projections[i];const auto& d=p.m_data;
                hash(&d.m_position,sizeof(d.m_position));hash(&d.m_opacity,sizeof(d.m_opacity));
                hash(&d.m_quaternion,sizeof(d.m_quaternion));hash(&d.m_halfSize,sizeof(d.m_halfSize));
                hash(&d.m_angleAttenuation,sizeof(d.m_angleAttenuation));hash(&d.m_normalMapOpacity,sizeof(d.m_normalMapOpacity));
                hash(&d.m_sortKey,sizeof(d.m_sortKey));hash(&d.m_blendMode,sizeof(d.m_blendMode));
                hash(&d.m_textureArrayIndex,sizeof(d.m_textureArrayIndex));hash(&d.m_textureIndex,sizeof(d.m_textureIndex));
                hash(&d.m_decalColor,sizeof(d.m_decalColor));hash(&d.m_decalColorFactor,sizeof(d.m_decalColorFactor));
                hash(&p.m_flags,sizeof(p.m_flags));hash(&p.m_layer,sizeof(p.m_layer));
            }
            auto it=m_entries.find(key);
            if(it==m_entries.end()&&createEntry&&m_entries.size()>=128)
            {
                // A transformed/replaced document may leave a full set of old
                // entries until End(). Do not let those block all new bakes.
                auto unused=std::find_if(m_entries.begin(),m_entries.end(),[&](const auto& old){return !m_used.contains(old.first);});
                if(unused!=m_entries.end()){unused->second.Remove();m_entries.erase(unused);}
            }
            if(it==m_entries.end()&&createEntry&&m_entries.size()<128)
            {
                if(Count(0)+Count(1)+Count(2)==0){m_started=std::chrono::steady_clock::now();m_wallMilliseconds=0;}
                Entry entry;entry.key=key;entry.first=aznumeric_cast<AZ::u32>(first);entry.end=aznumeric_cast<AZ::u32>(end);
                for(size_t i=first;i<end;++i)entry.bounds.Add(PaintMath::Intersection(boxes[i],clip));
                const bool filtered=std::any_of(projections.begin()+first,projections.begin()+end,[](const auto& p){return ((p.m_flags>>8)&7u)!=7u;});
                entry.grid=PaintMath::MakeBakeGrid(entry.bounds,density,budget>8?budget-8:budget,filtered?4:1);
                const auto n=aznumeric_cast<AZ::u32>(end-first);
                std::vector<PaintMath::CompositeNode> nodes;nodes.reserve(n*2);
                PaintMath::BuildCompositeNodes(nodes,0,n,[&](AZ::u32 i){return PaintMath::Intersection(boxes[first+i],clip);});
                const AZ::u32 header[4]={aznumeric_cast<AZ::u32>(nodes.size()),aznumeric_cast<AZ::u32>(16+nodes.size()*32),0,n};
                AZStd::vector<AZ::u8> bytes(header[1]+n*96);
                memcpy(bytes.data(),header,16);memcpy(bytes.data()+16,nodes.data(),nodes.size()*32);
                memcpy(bytes.data()+header[1],projections.data()+first,n*96);
                AZ::RPI::BufferAssetCreator creator;creator.Begin(AZ::Uuid::CreateRandom());
                AZ::RHI::BufferDescriptor descriptor;descriptor.m_bindFlags=AZ::RHI::BufferBindFlags::ShaderRead|AZ::RHI::BufferBindFlags::InputAssembly;
                descriptor.m_byteCount=bytes.size();creator.SetBuffer(bytes.data(),bytes.size(),descriptor);
                creator.SetBufferViewDescriptor(AZ::RHI::BufferViewDescriptor::CreateRaw(0,aznumeric_cast<AZ::u32>(bytes.size())));
                creator.SetUseCommonPool(AZ::RPI::CommonBufferPoolType::StaticInputAssembly);
                AZ::Data::Asset<AZ::RPI::BufferAsset> asset;if(creator.End(asset))entry.source=AZ::RPI::Buffer::FindOrCreate(asset);
                if(entry.source){entry.source->WaitForUpload();it=m_entries.emplace(key,AZStd::move(entry)).first;}
            }
            if(it!=m_entries.end())
            {
                m_used.insert(key);it->second.first=aznumeric_cast<AZ::u32>(first);it->second.end=aznumeric_cast<AZ::u32>(end);
                if(it->second.stage==3)result.push_back(&it->second);
            }
        }
        first=layerEnd;
    }
    return result;
}
bool PaintBakeCache::Tick(AZ::RPI::Scene* scene,bool idle)
{
    if(!scene)return false;
    if(!idle)
    {
        for(auto& [key,e]:m_entries)
        {
            (void)key;if(e.pass)e.pass->SetEnabled(false);if(e.finalize)e.finalize->SetEnabled(false);
        }
        return false;
    }
    for(auto& [key,e]:m_entries)if(e.stage==1||e.stage==2)
    {
        (void)key;
        if(e.stage==2&&!e.pass->GetRenderPipeline())
        {
            auto source=e.source;e.Remove();e.source=AZStd::move(source);e.stage=0;return true;
        }
        auto* pass=static_cast<PaintBakeRasterPass*>(e.pass.get());
        if(e.stage==2&&e.configured!=pass->GetShaderResourceGroup())
        {e.pass->SetEnabled(false);e.finalize->SetEnabled(false);e.stage=1;return false;}
        if(e.stage==1)
        {
            auto srg=e.pass->GetShaderResourceGroup();if(!srg||!e.finalize->GetShaderResourceGroup())return false;
            auto* output=e.pass->FindAttachmentBinding(AZ::Name("Scale"));
            if(!output||!output->GetAttachment())return false;
            const auto span=e.bounds.high-e.bounds.low;const auto& g=e.grid;
            const AZ::Vector4 low(e.bounds.low.x,e.bounds.low.y,e.bounds.low.z,float(g.bands)),size(span.x,span.y,span.z,0);
            const AZStd::array<AZ::u32,4> grid{g.x,g.y,g.z,g.columns};
            e.rows=AZStd::max(1u,AZStd::clamp(8u*1024*1024/AZStd::max(1u,e.end-e.first),512u,65536u)/g.width);
            e.row=AZStd::min(g.height,e.rows);
            const AZStd::array<AZ::u32,2> rows{0,e.row};
            const bool ok=srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_low")),low)
                &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_span")),size)
                &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_grid")),grid)
                &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_rows")),rows)
                &&srg->SetBufferView(srg->FindShaderInputBufferIndex(AZ::Name("m_source")),e.source->GetBufferView());
            if(!ok){e.stage=4;e.pass->SetEnabled(false);e.finalize->SetEnabled(false);return true;}
            e.pass->SetEnabled(true);e.finalize->SetEnabled(true);
            e.configured=srg;e.stage=2;e.age=pass->m_draws.load();
        }
        else if(pass->m_draws.load()!=e.age)
        {
            e.age=pass->m_draws.load();
            if(e.row<e.grid.height)
            {
                const AZStd::array<AZ::u32,2> rows{e.row,AZStd::min(e.grid.height,e.row+e.rows)};
                e.row=rows[1];auto srg=pass->GetShaderResourceGroup();
                srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_rows")),rows);
                e.pass->SetEnabled(true);e.finalize->SetEnabled(true);return false;
            }
            e.pass->QueueForRemoval();e.finalize->QueueForRemoval();e.pass={};e.finalize={};e.source={};e.configured={};e.status={};e.stage=3;
            m_wallMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-m_started).count();return true;
        }
        e.pass->SetEnabled(true);e.finalize->SetEnabled(true);return false;
    }
    auto* system=AZ::RPI::PassSystemInterface::Get();
    const AZ::Name bakeTemplate("SilPOMPaintBakeTemplate"),finalTemplate("SilPOMPaintBakeFinalizeTemplate");
    if(!system->GetPassTemplate(bakeTemplate)&&!system->LoadPassTemplateMappings("passes/silpompaintbaketemplates.azasset"))return false;
    for(auto& [key,e]:m_entries)if(e.stage==0)
    {
        (void)key;
        for(const auto& pipeline:scene->GetRenderPipelines())
        {
            if(pipeline!=scene->GetDefaultRenderPipeline())continue;
            if(pipeline->GetDescriptor().m_materialPipelineTag!="MainPipeline")continue;
            auto reference=pipeline->FindFirstPass(AZ::Name("Forward"));if(!reference)continue;
            static AZStd::atomic_uint64_t next{1};const auto serial=next++;
            auto create=[&](const AZ::Name& templ,const char* label)->AZ::RPI::Ptr<AZ::RPI::FullscreenTrianglePass>
            {
                auto t=system->GetPassTemplate(templ);if(!t)return AZ::RPI::Ptr<AZ::RPI::FullscreenTrianglePass>{};
                auto* templateData=azrtti_cast<const AZ::RPI::FullscreenTrianglePassData*>(t->m_passData.get());
                if(!templateData)return AZ::RPI::Ptr<AZ::RPI::FullscreenTrianglePass>{};
                auto data=AZStd::make_shared<AZ::RPI::FullscreenTrianglePassData>(*templateData);
                data->m_pipelineViewTag=reference->GetPipelineViewTag();
                AZ::RPI::PassRequest request;request.m_templateName=templ;request.m_passName=AZ::Name(AZStd::string::format("%s_%llu",label,static_cast<unsigned long long>(serial)));
                request.m_passData=data;
                AZ::RPI::PassDescriptor descriptor;descriptor.m_passName=request.m_passName;descriptor.m_passTemplate=t;
                descriptor.m_passRequest=AZStd::make_shared<AZ::RPI::PassRequest>(request);
                return aznew PaintBakeRasterPass(descriptor);
            };
            e.pass=create(bakeTemplate,"PaintBake");e.finalize=create(finalTemplate,"PaintBakeFinalize");
            if(!e.pass||!e.finalize){e.Remove();e.stage=4;return true;}
            e.pass->SetEnabled(false);e.finalize->SetEnabled(false);
            const auto span=e.bounds.high-e.bounds.low;auto& r=e.record;const auto& g=e.grid;
            r.low=e.bounds.low;r.inverseSpan={1/std::max(span.x,1e-6f),1/std::max(span.y,1e-6f),1/std::max(span.z,1e-6f)};
            r.x=g.x;r.y=g.y;r.z=g.z;r.columns=g.columns;
            r.flags=g.bands==1?2u:0u;
            r.step={span.x/(g.x-1),span.y/(g.y-1),span.z/(g.z-1)};r.inverseWidth=1.f/g.width;r.inverseHeight=1.f/g.height;
            // This engine's request overload dereferences m_imagePool even
            // though its header documents nullptr as the default system pool.
            auto imagePool=AZ::RPI::ImageSystemInterface::Get()->GetSystemAttachmentPool();
            if(!imagePool){e.Remove();e.stage=4;return true;}
            AZ::u32* indices[]={&r.scale,&r.bias,&r.normal,&r.receiver};bool valid=true;
            for(unsigned i=0;i<4;++i)
            {
                AZ::RPI::CreateAttachmentImageRequest image;image.m_imageName=AZ::Name(AZStd::string::format("PaintBake_%llu_%u",static_cast<unsigned long long>(serial),i));
                image.m_imageDescriptor=AZ::RHI::ImageDescriptor::Create2D(AZ::RHI::ImageBindFlags::Color|AZ::RHI::ImageBindFlags::ShaderRead,g.width,g.height,AZ::RHI::Format::R16G16B16A16_FLOAT);
                image.m_imagePool=imagePool.get();
                e.images[i]=AZ::RPI::AttachmentImage::Create(image);if(!e.images[i]){valid=false;break;}
                const auto indicesByDevice=e.images[i]->GetImageView()->GetBindlessReadIndex();
                if(indicesByDevice.size()!=1||indicesByDevice.begin()->second==AZ::u32(-1)){valid=false;break;}
                *indices[i]=indicesByDevice.begin()->second;
                static_cast<PaintBakeRasterPass*>(e.pass.get())->m_images[i]=e.images[i];
                static_cast<PaintBakeRasterPass*>(e.finalize.get())->m_images[i]=e.images[i];
            }
            AZ::RPI::CreateAttachmentImageRequest status;status.m_imageName=AZ::Name(AZStd::string::format("PaintBakeStatus_%llu",static_cast<unsigned long long>(serial)));
            status.m_imageDescriptor=AZ::RHI::ImageDescriptor::Create2D(AZ::RHI::ImageBindFlags::Color|AZ::RHI::ImageBindFlags::ShaderRead,1,1,AZ::RHI::Format::R16G16B16A16_FLOAT);
            status.m_imagePool=imagePool.get();
            e.status=AZ::RPI::AttachmentImage::Create(status);if(e.status)static_cast<PaintBakeRasterPass*>(e.finalize.get())->m_images[4]=e.status;else valid=false;
            if(!valid||!pipeline->AddPassBefore(e.pass,AZ::Name("Forward"))||!pipeline->AddPassBefore(e.finalize,AZ::Name("Forward")))
            {e.Remove();e.stage=4;return true;}
            e.stage=1;e.row=0;return false;
        }
    }
    return false;
}
