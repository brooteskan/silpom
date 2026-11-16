// SPDX-License-Identifier: MIT
// Included by Mesh.cpp after the active-generation surface evaluator.
namespace
{
float SurfacePaintLinear(float value)
{return value<=.04045f?value/12.92f:std::pow((value+.055f)/1.055f,2.4f);}
AZ::Vector4 SurfaceBrushSample(const std::vector<uint8_t>& rgba,unsigned w,unsigned h,float u,float v,bool color=false)
{
    if(!w||!h||rgba.size()!=size_t(w)*h*4)return AZ::Vector4(1,1,1,1);
    const float x=AZStd::clamp(u*float(w)-.5f,0.f,float(w-1)),y=AZStd::clamp(v*float(h)-.5f,0.f,float(h-1));
    const unsigned ix=unsigned(x),iy=unsigned(y),jx=AZStd::min(ix+1,w-1),jy=AZStd::min(iy+1,h-1);
    auto at=[&](unsigned a,unsigned b){const auto* p=rgba.data()+(size_t(b)*w+a)*4;return color?AZ::Vector4(SurfacePaintLinear(p[0]/255.f),SurfacePaintLinear(p[1]/255.f),SurfacePaintLinear(p[2]/255.f),p[3]/255.f):AZ::Vector4(p[0]/255.f,p[1]/255.f,p[2]/255.f,p[3]/255.f);};
    return at(ix,iy)*(1-(x-ix))*(1-(y-iy))+at(jx,iy)*(x-ix)*(1-(y-iy))+at(ix,jy)*(1-(x-ix))*(y-iy)+at(jx,jy)*(x-ix)*(y-iy);
}
}
bool MeshController::ApplySurfaceTextureBrush(const PaintSurfacePoint& anchor,const SurfacePaintBrush& brush,
    const SurfaceBrushPixels& source,AZ::u64 layer,AZStd::vector<SurfacePaintChart>& charts,AZStd::string& error)
{
    error.clear();
    if(anchor.receiver!=m_entity||!anchor.IsValid()||!m_active||!m_active->paintConfig.m_surface.IsReady()
        ||!brush.position.IsFinite()||!brush.normal.IsFinite()||!brush.tangent.IsFinite()
        ||!std::isfinite(brush.diameter)||brush.diameter<=0||!std::isfinite(brush.depth)||brush.depth<=0
        ||!source.width||!source.height||source.color.size()!=size_t(source.width)*source.height*4
        ||(!source.normal.empty()&&(!source.normalWidth||!source.normalHeight||source.normal.size()!=size_t(source.normalWidth)*source.normalHeight*4)))
    {error="Brush input or receiving surface is unavailable";return false;}
    const auto& config=m_active->paintConfig;const auto& rigid=m_active->paintWorld;
    const auto centerFace=AZStd::find_if(m_active->paintFaces.begin(),m_active->paintFaces.end(),[&](const auto& f){return f.id==anchor.face;});
    ReceiverFaceGeometry centerGeometry;
    if(centerFace==m_active->paintFaces.end()||centerFace->source>=config.m_surface->m_faces.size()
        ||!centerGeometry.Initialize(config.m_surface->m_faces[centerFace->source],config,m_active->paintScale))
    {error="The anchored receiving face is unavailable";return false;}
    const auto facing=rigid.TransformVector(anchor.front?centerGeometry.normal:-centerGeometry.normal).GetNormalizedSafe();
    const auto axisX=brush.tangent.GetNormalizedSafe(),axisZ=brush.normal.GetNormalizedSafe();
    const auto axisY=axisZ.Cross(axisX).GetNormalizedSafe();
    if(axisX.GetLengthSq()<.5f||axisY.GetLengthSq()<.5f||axisZ.GetLengthSq()<.5f)
    {error="Invalid brush frame";return false;}
    const float half=brush.diameter*.5f,halfDepth=AZStd::max(.001f,brush.depth*.5f);
    const auto radius=axisX.GetAbs()*half+axisY.GetAbs()*half+axisZ.GetAbs()*halfDepth;
    const auto brushBounds=AZ::Aabb::CreateFromMinMax(brush.position-radius,brush.position+radius);
    auto result=charts;bool any=false;
    for(const auto& record:m_active->paintFaces)
    {
        if(record.source>=config.m_surface->m_faces.size())continue;
        const auto& face=config.m_surface->m_faces[record.source];ReceiverFaceGeometry g;
        if(!g.Initialize(face,config,m_active->paintScale))continue;
        auto bounds=AZ::Aabb::CreateNull();
        const float amplitude=g.profile->m_scale*g.multiplier;
        for(const auto& corner:face.m_corners)for(float value:{-g.profile->m_reference,1-g.profile->m_reference})
            bounds.AddPoint(rigid.TransformPoint(corner.m_position*m_active->paintScale+corner.m_direction*(value*amplitude)));
        if(!bounds.Overlaps(brushBounds))continue;
        const auto planeNormal=rigid.TransformVector(g.normal).GetNormalizedSafe();
        const bool front=record.id==anchor.face?anchor.front:planeNormal.Dot(facing)>=0;
        const auto n=front?planeNormal:-planeNormal,t=rigid.TransformVector(g.du).GetNormalizedSafe();
        const float handed=g.normal.Cross(g.du.GetNormalizedSafe()).Dot(g.dv)<0?-1.f:1.f;
        const auto b=n.Cross(t).GetNormalizedSafe()*handed;
        float ox=axisX.Dot(t),oy=axisX.Dot(b);const float length=std::hypot(ox,oy);
        if(length<1e-6f)continue;ox/=length;oy/=length;
        const float orientation=(axisX.Dot(t)*axisY.Dot(b)-axisX.Dot(b)*axisY.Dot(t))<0?-1.f:1.f;
        auto found=AZStd::find_if(result.begin(),result.end(),[&](const auto& c)
            {return c.receiver==m_entity&&c.face==record.id&&c.layer==layer&&c.front==front;});
        SurfacePaintChart chart;SurfacePaintImage::Image image;
        if(found==result.end()&&brush.erase)continue;
        if(found!=result.end())
        {
            chart=*found;
            if(!SurfacePaintImage::Decode(std::string_view(chart.texels.data(),chart.texels.size()),image))
            {error="Saved surface texture is invalid; stroke was not applied";return false;}
        }
        else
        {
            float lowU=g.uv[0].GetX(),lowV=g.uv[0].GetY(),highU=lowU,highV=lowV;
            for(const auto& uv:g.uv){lowU=AZStd::min(lowU,uv.GetX());lowV=AZStd::min(lowV,uv.GetY());highU=AZStd::max(highU,uv.GetX());highV=AZStd::max(highV,uv.GetY());}
            if(highU<=lowU||highV<=lowV)continue;
            chart.receiver=m_entity;chart.face=record.id;chart.layer=layer;chart.front=front;chart.rectangle=AZ::Vector4(lowU,lowV,highU,highV);
            image.width=image.height=chart.resolution;image.pixels.resize(size_t(image.width)*image.height);
        }
        const auto& rect=chart.rectangle;bool changed=false;
        const float stepU=(rect.GetZ()-rect.GetX())/image.width,stepV=(rect.GetW()-rect.GetY())/image.height;
        const auto w0=g.Weights(AZ::Vector2(0,0));
        const auto border=(g.Weights(AZ::Vector2(stepU,0))-w0).GetAbs()*2+(g.Weights(AZ::Vector2(0,stepV))-w0).GetAbs()*2;
        for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x)
        {
            const AZ::Vector2 uv(rect.GetX()+(float(x)+.5f)/image.width*(rect.GetZ()-rect.GetX()),rect.GetY()+(float(y)+.5f)/image.height*(rect.GetW()-rect.GetY()));
            const auto weights=g.Weights(uv);
            if(weights.GetX() < -border.GetX()||weights.GetY() < -border.GetY()||weights.GetZ() < -border.GetZ())continue;
            const bool receiving=weights.GetMinElement()>=0;
            AZ::Vector3 p,surfaceNormal;g.Evaluate(uv,p,surfaceNormal);p=rigid.TransformPoint(p);
            surfaceNormal=rigid.TransformVector(front?surfaceNormal:-surfaceNormal).GetNormalizedSafe();
            const auto delta=p-brush.position;const float px=delta.Dot(axisX)/half,py=delta.Dot(axisY)/half,pz=delta.Dot(axisZ)/halfDepth;
            if(std::abs(px)>1||std::abs(py)>1||std::abs(pz)>1)continue;
            const float angle=AZStd::clamp(surfaceNormal.Dot(axisZ)*4.f,0.f,1.f);if(angle<=0)continue;
            if((brush.heightMask&7u)!=7u)
            {
                const float h=g.Height(uv,true).GetX();
                const unsigned band=h<1.f/3?0:(h<2.f/3?1:2);if(!(brush.heightMask&(1u<<band)))continue;
            }
            const float u=px*.5f+.5f,v=.5f-py*.5f;
            const auto rgba=SurfaceBrushSample(source.color,source.width,source.height,u,v,true);
            const float alpha=rgba.GetW()*angle;auto& pixel=image.pixels[size_t(y)*image.width+x];const auto before=pixel;
            if(alpha<=0)continue;
            if(brush.erase)
            {SurfacePaintMath::EraseColor(pixel.color,alpha*brush.opacity);SurfacePaintMath::EraseNormal(pixel.normal,alpha*brush.opacity);}
            else
            {
                SurfacePaintMath::PaintColor(pixel.color,{rgba.GetX()*brush.color.GetR(),rgba.GetY()*brush.color.GetG(),rgba.GetZ()*brush.color.GetB()},alpha*brush.opacity,brush.blendMode);
                if(brush.normalOpacity>0)
                {
                    const auto value=source.normal.empty()?AZ::Vector4(.5f,.5f,1,1):SurfaceBrushSample(source.normal,source.normalWidth,source.normalHeight,u,v);
                    auto decode=[](float v){const float n=v*2-1;return std::abs(n)<=1.f/255+1e-6f?0.f:n;};
                    const float nx=decode(value.GetX()),ny=decode(value.GetY()),nz=std::sqrt(AZStd::max(0.f,1-nx*nx-ny*ny));
                    SurfacePaintMath::PaintNormal(pixel.normal,{ox*nx-orientation*oy*ny,oy*nx+orientation*ox*ny,nz},alpha*brush.normalOpacity);
                }
            }
            if(receiving&&memcmp(&before,&pixel,sizeof(pixel))!=0)changed=true;
        }
        if(!changed)continue;
        const auto encoded=SurfacePaintImage::Encode(image);if(encoded.empty()){error="Surface texture encoding failed; stroke was not applied";return false;}
        chart.texels.assign(encoded.data(),encoded.size());
        if(found==result.end())result.push_back(AZStd::move(chart));else *found=AZStd::move(chart);
        if(AZStd::count_if(result.begin(),result.end(),[&](const auto& c){return c.receiver==m_entity;})>128)
        {error="Surface paint supports at most 128 face/layer/side charts per receiver";return false;}
        size_t bytes=0;for(const auto& c:result)if(c.receiver==m_entity)
            for(unsigned size=c.resolution;size;size/=2)bytes+=size_t(size)*size*32;
        if(bytes>256u*1024u*1024u){error="Surface paint exceeds the 256 MiB receiver texture budget";return false;}
        any=true;
    }
    if(any)charts=AZStd::move(result);
    return any;
}
