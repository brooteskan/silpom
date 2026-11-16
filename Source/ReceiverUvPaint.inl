// SPDX-License-Identifier: MIT
// Included by Mesh.cpp, inside SilPOM, after Generation is defined.
namespace
{
struct ReceiverFaceGeometry
{
    AZ::Vector3 origin,du,dv,normal,d[3];
    AZ::Vector2 uv[3];
    const MeshProfileBinding* profile=nullptr;
    float multiplier=1;
    bool Initialize(const MeshFace& f,const MeshConfig& config)
    {
        if(f.m_corners.size()!=3)return false;
        for(const auto& p:config.m_profiles)if(p.m_id==f.m_profile){profile=&p;break;}
        if(!profile||!profile->m_height.IsReady())return false;
        for(unsigned i=0;i<3;++i){uv[i]=f.m_corners[i].m_uv;d[i]=f.m_corners[i].m_direction;}
        origin=f.m_corners[0].m_position;
        const auto e1=f.m_corners[1].m_position-origin,e2=f.m_corners[2].m_position-origin;
        const auto a=uv[1]-uv[0],b=uv[2]-uv[0];const float det=a.GetX()*b.GetY()-a.GetY()*b.GetX();
        if(std::abs(det)<1e-10f)return false;
        du=(e1*b.GetY()-e2*a.GetY())/det;dv=(e2*a.GetX()-e1*b.GetX())/det;
        normal=e1.Cross(e2).GetNormalizedSafe();multiplier=config.m_reliefDepthMultiplier;return true;
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
            if(desc.m_format==AZ::RHI::Format::R32_FLOAT){float value;memcpy(&value,pixels.data()+index*4,4);return value;}
            AZ::u16 value;memcpy(&value,pixels.data()+index*2,2);return float(value)/65535.f;
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
        const float det=du.Dot(dv.Cross(normal));
        const auto base=(normal-dv.Cross(normal)*(height.GetY()/det)-normal.Cross(du)*(height.GetZ()/det)).GetNormalizedSafe();
        const auto target=direction.GetNormalizedSafe(),axis=normal.Cross(target);
        n=(base+axis.Cross(base)+axis.Cross(axis.Cross(base))/(1+normal.Dot(target))).GetNormalizedSafe();
    }
};
}
bool MeshController::ResolvePaintSurface(const PaintSurfacePoint& address,PaintSurfaceSample& sample) const
{
    if(address.receiver!=m_entity||!address.IsValid()||!m_active||!m_configuration.m_surface.IsReady())return false;
    for(const auto& record:m_active->paintFaces)if(record.id==address.face)
    {
        ReceiverFaceGeometry geometry;if(!geometry.Initialize(m_configuration.m_surface->m_faces[record.source],m_configuration))return false;
        const auto weights=geometry.Weights(address.uv);if(weights.GetMinElement()<-.0001f)return false;
        geometry.Evaluate(address.uv,sample.position,sample.normal);
        sample.position=m_world.TransformPoint(sample.position);sample.normal=m_world.TransformVector(sample.normal).GetNormalizedSafe();
        if(!address.front)sample.normal=-sample.normal;sample.hitFace=record.hit;return sample.position.IsFinite()&&sample.normal.IsFinite();
    }
    return false;
}
bool MeshController::FindPaintSurfacePoint(const AZ::Vector3& worldPosition,const AZ::Vector3& worldNormal,PaintSurfacePoint& address,float& distance) const
{
    if(!m_active||!m_configuration.m_surface.IsReady())return false;
    const auto inverse=m_world.GetInverse();const auto position=inverse.TransformPoint(worldPosition);bool found=false;
    for(const auto& record:m_active->paintFaces)
    {
        ReceiverFaceGeometry g;if(!g.Initialize(m_configuration.m_surface->m_faces[record.source],m_configuration))continue;
        const auto delta=position-g.origin;const float aa=g.du.Dot(g.du),ab=g.du.Dot(g.dv),bb=g.dv.Dot(g.dv),det=aa*bb-ab*ab;
        AZ::Vector2 q=g.uv[0]+AZ::Vector2((delta.Dot(g.du)*bb-delta.Dot(g.dv)*ab)/det,(delta.Dot(g.dv)*aa-delta.Dot(g.du)*ab)/det);
        if(g.Weights(q).GetMinElement()<-.1f)continue;
        AZ::Vector3 p,n;
        for(unsigned i=0;i<10;++i)
        {
            g.Evaluate(q,p,n);AZ::Vector3 px,py,ignored;
            constexpr float step=1e-5f;g.Evaluate(q+AZ::Vector2(step,0),px,ignored);g.Evaluate(q+AZ::Vector2(0,step),py,ignored);
            const auto dx=(px-p)/step,dy=(py-p)/step,error=position-p;
            const float a=dx.Dot(dx),b=dx.Dot(dy),c=dy.Dot(dy),deter=a*c-b*b;if(deter<1e-12f)break;
            const AZ::Vector2 correction((error.Dot(dx)*c-error.Dot(dy)*b)/deter,(error.Dot(dy)*a-error.Dot(dx)*b)/deter);
            q+=correction;if(correction.GetLengthSq()<1e-14f)break;
        }
        g.Evaluate(q,p,n);const float error=(m_world.TransformPoint(p)-worldPosition).GetLength();
        if(error<distance&&error<.025f&&g.Weights(q).GetMinElement()>=-.0001f)
        {distance=error;address.receiver=m_entity;address.face=record.id;address.uv=q;address.front=m_world.TransformVector(n).Dot(worldNormal)>=0;found=true;}
    }
    return found;
}
bool FindPaintSurface(const AZ::Vector3& position,const AZ::Vector3& normal,PaintSurfacePoint& address)
{
    float distance=.025f;bool found=false;
    MeshRequestBus::EnumerateHandlers([&](MeshRequests* receiver)
    {found=receiver->FindPaintSurfacePoint(position,normal,address,distance)||found;return true;});
    return found;
}
AZStd::string MeshController::GetPaintFacesJson() const
{
    rapidjson::StringBuffer json;rapidjson::Writer<rapidjson::StringBuffer> w(json);w.StartArray();
    if(m_active&&m_configuration.m_surface.IsReady())for(const auto& record:m_active->paintFaces)
    {
        const auto& face=m_configuration.m_surface->m_faces[record.source];w.StartObject();w.Key("face");w.String(record.id.c_str());
        w.Key("hit_face");w.Uint(record.hit);w.Key("batch");w.Uint(record.batch);w.Key("profile");w.Uint(face.m_profile);
        w.Key("corners");w.StartArray();for(const auto& c:face.m_corners)
        {w.StartObject();w.Key("uv");w.StartArray();w.Double(c.m_uv.GetX());w.Double(c.m_uv.GetY());w.EndArray();w.Key("position");w.StartArray();const auto p=m_world.TransformPoint(c.m_position);w.Double(p.GetX());w.Double(p.GetY());w.Double(p.GetZ());w.EndArray();w.EndObject();}w.EndArray();w.EndObject();
    }
    w.EndArray();return {json.GetString(),json.GetSize()};
}

// GPU cache implementation follows. Ownership above remains authoritative
// whether this disposable acceleration is enabled, invalid, or absent.
class ReceiverUvBakePass final : public AZ::RPI::FullscreenTrianglePass
{
public:
    AZ_RTTI(ReceiverUvBakePass,"{5713B531-4F68-454B-A313-B09109FB77E0}",AZ::RPI::FullscreenTrianglePass);
    AZ_CLASS_ALLOCATOR(ReceiverUvBakePass,AZ::SystemAllocator);
    explicit ReceiverUvBakePass(const AZ::RPI::PassDescriptor& d):FullscreenTrianglePass(d){}
    Material material;
    AZ::Data::Instance<AZ::RPI::ShaderResourceGroup> objectSrg;
    AZ::u32 instanceOffset=0;
    AZ::Data::Instance<AZ::RPI::AttachmentImage> images[4];
    AZStd::atomic_uint32_t draws{0};
protected:
    void BuildInternal() override
    {
        const char* slots[]={"Color","NormalFront","NormalBack","Status"};
        for(unsigned i=0;i<4;++i)if(images[i])AttachImageToSlot(AZ::Name(slots[i]),images[i]);
        FullscreenTrianglePass::BuildInternal();
    }
    void CompileResources(const AZ::RHI::FrameGraphCompileContext& context) override
    {
        FullscreenTrianglePass::CompileResources(context);
        if(material)BindSrg(material->GetRHIShaderResourceGroup());
        if(objectSrg)BindSrg(objectSrg->GetRHIShaderResourceGroup());
    }
    void BuildCommandListInternal(const AZ::RHI::FrameGraphExecuteContext& context) override
    {
        if(material)m_item.SetRootConstants(reinterpret_cast<const AZ::u8*>(&instanceOffset),sizeof(instanceOffset));
        FullscreenTrianglePass::BuildCommandListInternal(context);++draws;
    }
};
struct MeshController::UvPaint
{
    AZ::EntityId canvas;
    AZStd::string face,status="Waiting for receiver UV assets";
    AZ::Vector4 rectangle;
    AZ::u32 resolution=0,batch=0,localFace=0,hitFace=0,mode=0;
    AZ::u64 revision=0;
    Generation* generation=nullptr;
    AZ::Data::Asset<AZ::RPI::MaterialAsset> templateAsset;
    Material original,material;
    AZ::Data::Instance<AZ::RPI::ShaderResourceGroup> objectSrg;
    struct Mip
    {
        ReceiverUvMip record;
        AZ::Data::Instance<AZ::RPI::AttachmentImage> images[3],statusImage;
        AZ::RPI::Ptr<ReceiverUvBakePass> pass,finalize;
        AZ::u32 row=0,draws=0,stage=0;
        AZ::RPI::ShaderVariantId variant;
        void Remove(){if(pass)pass->QueueForRemoval();if(finalize)finalize->QueueForRemoval();pass={};finalize={};}
    };
    AZStd::vector<Mip> mips;
    AZStd::shared_ptr<const ReceiverPaintInput> input;
    AZ::Data::Instance<AZ::RPI::Buffer> table;
    AZ::Transform world=AZ::Transform::CreateIdentity();
    bool swapped=false;
    double bakeMs=0;
    std::chrono::steady_clock::time_point started;
    ~UvPaint(){for(auto& mip:mips)mip.Remove();}
    bool Bake(AZ::RPI::Scene*,const AZ::Transform&);
};
bool MeshController::UvPaint::Bake(AZ::RPI::Scene* scene,const AZ::Transform& transform)
{
    if(table)return true;if(!scene||!input||!objectSrg)return false;
    if(!material->UsesSceneMaterialSrg()||material->GetMaterialTypeId()<0||material->GetMaterialInstanceId()<0)
    {status="Waiting for scene material registration";return false;}
    auto* system=AZ::RPI::PassSystemInterface::Get();
    const AZ::Name bakeTemplate("SilPOMReceiverUvBakeTemplate"),finalTemplate("SilPOMReceiverUvFinalizeTemplate");
    if(!system->GetPassTemplate(bakeTemplate)&&!system->LoadPassTemplateMappings("passes/silpomreceiveruvtemplates.azasset"))return false;
    if(mips.empty())
    {
        for(AZ::u32 size=resolution;;size=AZStd::max(1u,size/2))
        {Mip mip;mip.record.size=size;mips.push_back(AZStd::move(mip));if(size==1)break;}
        started=std::chrono::steady_clock::now();
    }
    for(auto& mip:mips)
    {
        if(mip.stage==3)continue;
        if(mip.stage==0)
        {
            auto pipeline=scene->GetDefaultRenderPipeline();if(!pipeline||pipeline->GetDescriptor().m_materialPipelineTag!="MainPipeline")return false;
            auto reference=pipeline->FindFirstPass(AZ::Name("Forward"));if(!reference)return false;
            auto enabled=[&](const char* name){auto i=material->FindPropertyIndex(AZ::Name(name));return i.IsValid()&&material->GetPropertyValue(i).GetValue<bool>();};
            const AZ::Name tag(enabled("surface.learnedMaterial")?(enabled("surface.sharedVisibility")?"silpom_uv_bake_shared":"silpom_uv_bake_learned"):"silpom_uv_bake");
            const auto& collection=material->GetShaderCollection(AZ::Name("MainPipeline"));
            if(!collection.HasShaderTag(tag)){status="Bake shader tag unavailable";return false;}
            const auto& shader=collection[tag];mip.variant=shader.GetShaderVariantId();
            static AZStd::atomic_uint64_t serial{1};const auto id=serial++;
            auto create=[&](const AZ::Name& templateName,const char* label,bool bake)
            {
                auto t=system->GetPassTemplate(templateName);auto data=AZStd::make_shared<AZ::RPI::FullscreenTrianglePassData>(*azrtti_cast<const AZ::RPI::FullscreenTrianglePassData*>(t->m_passData.get()));
                data->m_pipelineViewTag=reference->GetPipelineViewTag();data->m_bindViewSrg=bake;
                if(bake)data->m_shaderAsset.m_assetId=shader.GetShaderAssetId();
                AZ::RPI::PassRequest request;request.m_templateName=templateName;request.m_passName=AZ::Name(AZStd::string::format("%s_%llu",label,static_cast<unsigned long long>(id)));request.m_passData=data;
                AZ::RPI::PassDescriptor descriptor(request.m_passName,t,&request);
                AZ::RPI::Ptr<ReceiverUvBakePass> pass=aznew ReceiverUvBakePass(descriptor);
                if(bake){pass->material=material;pass->objectSrg=objectSrg;}
                pass->SetEnabled(false);return pass;
            };
            mip.pass=create(bakeTemplate,"ReceiverUvBake",true);mip.finalize=create(finalTemplate,"ReceiverUvFinalize",false);
            auto pool=AZ::RPI::ImageSystemInterface::Get()->GetSystemAttachmentPool();if(!pool){status="Attachment pool unavailable";return false;}
            AZ::u32* indices[]={&mip.record.color,&mip.record.normalFront,&mip.record.normalBack};
            for(unsigned i=0;i<4;++i)
            {
                AZ::RPI::CreateAttachmentImageRequest request;request.m_imageName=AZ::Name(AZStd::string::format("ReceiverUv_%llu_%u",static_cast<unsigned long long>(id),i));
                const auto size=i==3?1:mip.record.size+4;request.m_imageDescriptor=AZ::RHI::ImageDescriptor::Create2D(AZ::RHI::ImageBindFlags::Color|AZ::RHI::ImageBindFlags::ShaderRead,size,size,AZ::RHI::Format::R16G16B16A16_FLOAT);request.m_imagePool=pool.get();
                auto image=AZ::RPI::AttachmentImage::Create(request);if(!image){status="Chart image allocation failed";return false;}
                if(i<3)
                {
                    mip.images[i]=image;mip.pass->images[i]=image;mip.finalize->images[i]=image;
                    const auto bindless=image->GetImageView()->GetBindlessReadIndex();if(bindless.size()!=1)return false;*indices[i]=bindless.begin()->second;
                }
                else {mip.statusImage=image;mip.finalize->images[3]=image;}
            }
            if(!pipeline->AddPassBefore(mip.pass,AZ::Name("Forward"))||!pipeline->AddPassBefore(mip.finalize,AZ::Name("Forward")))
            {status="Chart pass insertion failed";mip.Remove();return false;}
            mip.stage=1;return false;
        }
        auto srg=mip.pass->GetShaderResourceGroup();if(!srg||!mip.finalize->GetShaderResourceGroup())return false;
        if(mip.stage==2&&mip.pass->draws.load()==mip.draws)return false;
        if(mip.stage==2&&mip.row>=mip.record.size+4)
        {mip.Remove();mip.stage=3;return false;}
        const AZ::u32 rows=AZStd::max(1u,4u*1024*1024/AZStd::max(1u,input->fixed)/(mip.record.size+4));
        const AZStd::array<AZ::u32,2> range{mip.row,AZStd::min(mip.record.size+4,mip.row+rows)};
        AZ::Transform rigid=transform;rigid.SetUniformScale(1);
        const AZ::Vector4 rect(rectangle.GetX(),rectangle.GetY(),rectangle.GetZ()-rectangle.GetX(),rectangle.GetW()-rectangle.GetY());
        const bool ok=srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_receiverModel")),AZ::Matrix4x4::CreateFromTransform(rigid))
            &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_receiverRectangle")),rect)
            &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_receiverFace")),localFace)
            &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_receiverResolution")),mip.record.size)
            &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_receiverMaterialType")),material->GetMaterialTypeId())
            &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_receiverMaterialInstance")),material->GetMaterialInstanceId())
            &&srg->SetConstant(srg->FindShaderInputConstantIndex(AZ::Name("m_receiverRows")),range);
        if(!ok){status="Chart shader constants unavailable";return false;}
        mip.pass->UpdateShaderOptions(mip.variant);mip.row=range[1];mip.draws=mip.pass->draws.load();mip.stage=2;
        mip.pass->SetEnabled(true);mip.finalize->SetEnabled(true);return false;
    }
    const AZ::u32 header[4]={hitFace,input->fixed,aznumeric_cast<AZ::u32>(mips.size()),1};
    const AZ::Vector4 rect(rectangle.GetX(),rectangle.GetY(),1/(rectangle.GetZ()-rectangle.GetX()),1/(rectangle.GetW()-rectangle.GetY()));
    AZStd::vector<AZ::u8> bytes(32+mips.size()*16);memcpy(bytes.data(),header,16);memcpy(bytes.data()+16,&rect,16);
    for(size_t i=0;i<mips.size();++i)memcpy(bytes.data()+32+i*16,&mips[i].record,16);
    table=AZ::RPI::Buffer::FindOrCreate(Buffer(bytes.data(),bytes.size()/4,4,true));if(!table)return false;table->WaitForUpload();
    bakeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();return true;
}
bool MeshController::BeginReceiverUvPaint(AZ::EntityId canvas,const AZStd::string& face,const AZ::Vector4& rectangle,AZ::u32 resolution)
{
    if(!canvas.IsValid()||!m_active||!rectangle.IsFinite()||rectangle.GetZ()<=rectangle.GetX()||rectangle.GetW()<=rectangle.GetY()||resolution<32||resolution>2048)return false;
    if(m_uvPaint&&m_uvPaint->generation==m_active.get()&&m_uvPaint->canvas==canvas&&m_uvPaint->face==face&&m_uvPaint->rectangle==rectangle&&m_uvPaint->resolution==resolution)return true;
    auto it=AZStd::find_if(m_active->paintFaces.begin(),m_active->paintFaces.end(),[&](const auto& f){return f.id==face;});if(it==m_active->paintFaces.end())return false;
    ReleaseReceiverUvPaint();m_uvPaint=AZStd::make_unique<UvPaint>();auto& p=*m_uvPaint;
    p.canvas=canvas;p.face=face;p.rectangle=rectangle;p.resolution=resolution;p.batch=it->batch;p.localFace=it->local;p.hitFace=it->hit;p.generation=m_active.get();p.original=m_active->materials[p.batch];
    AZ::Data::AssetId id;AZ::Data::AssetCatalogRequestBus::BroadcastResult(id,&AZ::Data::AssetCatalogRequests::GetAssetIdByPath,"materials/silpommeshuv.template.azmaterial",azrtti_typeid<AZ::RPI::MaterialAsset>(),false);
    if(!id.IsValid()){p.status="Receiver UV template asset is unavailable";return true;}
    p.templateAsset=AZ::Data::AssetManager::Instance().GetAsset<AZ::RPI::MaterialAsset>(id,AZ::Data::AssetLoadBehavior::QueueLoad);return true;
}
bool MeshController::SetReceiverUvPaintMode(AZ::u32 mode)
{
    if(!m_uvPaint||mode>2)return false;m_uvPaint->mode=mode;
    for(auto& mip:m_uvPaint->mips)if(mip.stage==2)
    {if(mip.pass)mip.pass->SetEnabled(mode==2);if(mip.finalize)mip.finalize->SetEnabled(mode==2);}
    return true;
}
AZStd::string MeshController::GetReceiverUvPaintStatus() const
{
    if(!m_uvPaint)return "{\"state\":\"unbound\"}";const auto& p=*m_uvPaint;
    size_t bytes=0;for(const auto& mip:p.mips)bytes+=size_t(mip.record.size+4)*(mip.record.size+4)*24;
    return AZStd::string::format("{\"state\":\"%s\",\"mode\":%u,\"hit_face\":%u,\"resolution\":%u,\"ready\":%s,\"mips\":%zu,\"bytes\":%zu,\"bake_ms\":%.3f}",p.status.c_str(),p.mode,p.hitFace,p.resolution,p.table?"true":"false",p.mips.size(),bytes,p.bakeMs);
}
void MeshController::ReleaseReceiverUvPaint()
{
    if(m_uvPaint&&m_processor&&m_active&&m_uvPaint->generation==m_active.get())m_processor->SetCustomMaterials(m_active->handles[m_uvPaint->batch],m_uvPaint->original);
    m_uvPaint.reset();
}
void MeshController::TickReceiverUvPaint()
{
    if(!m_uvPaint)return;auto& p=*m_uvPaint;
    if(p.generation!=m_active.get()){ReleaseReceiverUvPaint();return;}
    const auto& objectSrgs=m_processor->GetObjectSrgs(m_active->handles[p.batch]);
    if(!objectSrgs.empty())p.objectSrg=objectSrgs.front();
    if(!p.templateAsset.IsReady())return;
    if(!p.material)
    {
        p.material=Snapshot(p.templateAsset);if(!p.material){p.status="UV material creation failed";return;}
        const auto layout=p.original->GetMaterialPropertiesLayout();const auto& values=p.original->GetPropertyValues();
        for(size_t i=0;i<values.size();++i)
        {const auto index=p.material->FindPropertyIndex(layout->GetPropertyDescriptor(AZ::RPI::MaterialPropertyIndex(i))->GetName());if(index.IsValid())p.material->SetPropertyValue(index,values[i]);}
    }
    AZStd::shared_ptr<const ReceiverPaintInput> input;PaintRequestBus::EventResult(input,p.canvas,&PaintRequests::GetReceiverPaintInput);
    if(!input||!input->buffer||input->hitFace!=p.hitFace)
    {
        if(p.swapped){m_processor->SetCustomMaterials(m_active->handles[p.batch],p.original);p.swapped=false;}
        for(auto& mip:p.mips)mip.Remove();p.mips.clear();p.table={};p.input={};p.revision=0;
        p.status="Requires stamps anchored to this receiver face";return;
    }
    const auto bindless=input->buffer->GetBufferView()->GetBindlessReadIndex();if(bindless.size()!=1)return;
    if(p.revision!=input->revision||!p.world.IsClose(m_world))
    {
        for(auto& mip:p.mips)mip.Remove();p.mips.clear();p.table={};p.revision=input->revision;p.world=m_world;p.input=input;
    }
    Set(p.material,"uvPaint.owner",input->token);Set(p.material,"uvPaint.prefixBuffer",bindless.begin()->second);Set(p.material,"uvPaint.face",p.hitFace);
    p.material->Compile();
    bool ready=false;if(p.mode==2&&!p.material->NeedsCompile())ready=p.Bake(AZ::RPI::Scene::GetSceneForEntityId(m_entity),m_world);
    if(ready)
    {
        const auto index=p.table->GetBufferView()->GetBindlessReadIndex();if(index.size()!=1)return;
        Set(p.material,"uvPaint.atlasBuffer",index.begin()->second);
    }
    Set(p.material,"uvPaint.mode",p.mode?(ready?2u:1u):0u);if(p.material->NeedsCompile())p.material->Compile();
    if(p.swapped!=(p.mode!=0))
    {m_processor->SetCustomMaterials(m_active->handles[p.batch],p.mode?p.material:p.original);p.swapped=p.mode!=0;}
    p.status=p.mode==0?"Original renderer":(ready?"Receiver UV atlas active":(p.mode==1?"UV path with prefix replay":"Baking receiver UV atlas"));
}
