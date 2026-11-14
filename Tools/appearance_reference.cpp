// SPDX-License-Identifier: MIT
// Offline detailed-relief renderer. No runtime shader depends on this tracer.
#define SILPOM_HIT_DEPTH 1
#include "../Tests/BlendedFaceCpu.h"
#include <array>
#include <thread>
#include <atomic>
using namespace SilPOM::Cpu;
#include "../Assets/Shaders/SilPOM/PhysicalShadowReach.azsli"
#include "visibility_reference.h"
namespace
{
float3 Vec(const float* p){return {p[0],p[1],p[2]};}
float Clamp(float x,float a,float b){return std::max(a,std::min(b,x));}
float Sample(const float* map,uint w,uint h,float2 uv,uint channel)
{
    const float x=uv.x*w-.5f,y=uv.y*h-.5f;const int ix=int(floor(x)),iy=int(floor(y));
    const float fx=x-floor(x),fy=y-floor(y);
    auto tap=[&](int a,int b){a=std::max(0,std::min(int(w)-1,a));b=std::max(0,std::min(int(h)-1,b));return map[(size_t(b)*w+a)*6+channel];};
    return (tap(ix,iy)*(1-fx)+tap(ix+1,iy)*fx)*(1-fy)+(tap(ix,iy+1)*(1-fx)+tap(ix+1,iy+1)*fx)*fy;
}
struct Surface{SilPomMeshFace face;SilPomPatch patch;float2 low;float3 T,B;};
Surface Unpack(const float* p,uint w,uint h,float scale,float reference)
{
    Surface s;auto& f=s.face;f.origin=Vec(p);f.du=Vec(p+3);f.dv=Vec(p+6);
    f.uv0={p[9],p[10]};f.uv1={p[11],p[12]};f.uv2={p[13],p[14]};
    f.d0=Vec(p+15);f.d1=Vec(p+18);f.d2=Vec(p+21);f.normal=Vec(p+24);
    s.low={min(f.uv0.x,min(f.uv1.x,f.uv2.x)),min(f.uv0.y,min(f.uv1.y,f.uv2.y))};
    const float u=max(f.uv0.x,max(f.uv1.x,f.uv2.x))-s.low.x,v=max(f.uv0.y,max(f.uv1.y,f.uv2.y))-s.low.y;
    s.patch={u,v,scale,reference,u,v,s.low.x,s.low.y,w,h,1,65536};
    s.T=normalize(f.du);s.B=normalize(cross(f.normal,s.T));return s;
}
float Visibility(const Texture& texture,const Surface& s,float2 uv,float3 L,float reach,bool& capped)
{
    const auto& f=s.face;const auto& p=s.patch;const auto D=SilPomFaceDirection(f,uv);
    const float det=dot(f.du,cross(f.dv,D));if(abs(det)<1e-10f)return 1;
    const float ur=dot(L,cross(f.dv,D))/det,vr=dot(f.du,cross(L,D))/det,hr=dot(f.du,cross(f.dv,L))/det;
    const float start=SilPomFaceHeight(texture,p,uv,s.low).x,top=p.scale*(1-p.reference),bias=max(1e-5f,abs(p.scale)*.002f);
    if(hr<=0)return 0;
    const float end=SilPomPhysicalShadowReach(start,top,bias,hr,reach);
    const float rate=max(abs(ur)*p.textureWidth,abs(vr)*p.textureHeight);
    const uint requested=uint(std::ceil(end*rate*2));capped=capped||requested>4096;
    const uint steps=std::min(4096u,std::max(1u,requested));
    for(uint i=1;i<=steps;++i){const float t=end*float(i)/steps;
        if(SilPomFaceHeight(texture,p,{uv.x+ur*t,uv.y+vr*t},s.low).x>start+hr*t+bias)return 0;}
    return 1;
}
float Specular(float3 N,float3 V,float3 L,float rough)
{
    const float nv=max(1e-4f,dot(N,V)),nl=max(0.f,dot(N,L));if(nl<=0)return 0;
    const auto H=normalize(V+L);const float nh=max(0.f,dot(N,H)),vh=max(0.f,dot(V,H));
    const float a=std::pow(max(.08f,rough),4.f),d=a/(3.14159265f*std::pow(nh*nh*(a-1)+1,2.f));
    const float k=(rough+1)*(rough+1)/8,g=nv/(nv*(1-k)+k)*nl/(nl*(1-k)+k);
    return d*g*(.04f+.96f*std::pow(1-vh,5.f))/(4*nv);
}
}
// Each query: UV2, V3, L3 (base tangent frame), reach, major/minor, axis2.
// Output: visible normal3, roughness, filtered diffuse RGB, specular,
//         proxy basecolor RGB, valid coverage fraction, teacher-cap fraction.
extern "C" __declspec(dllexport) bool RenderAppearance(const float* heights,uint width,uint height,
    float scale,float reference,const float* faces,uint faceCount,const float* maps,
    const float* queries,const uint* ids,uint count,uint samples,float* output)
{
    if(!width||!height||!faceCount||!samples||samples>256)return false;
    std::vector<Surface> surfaces;for(uint i=0;i<faceCount;++i)surfaces.push_back(Unpack(faces+i*27,width,height,scale,reference));
    std::atomic<uint> next{0};std::vector<std::thread> workers;
    // Texture's load counter is thread-local by ownership; immutable pixels
    // are copied per worker because the CPU shader's Texture owns its data.
    for(uint worker=0;worker<8;++worker)workers.emplace_back([&]{
        const Texture texture{width,height,std::vector<float>(heights,heights+size_t(width)*height)};
        for(uint i=next.fetch_add(1);i<count;i=next.fetch_add(1))
        {
            auto* out=output+i*13;std::fill(out,out+13,0.f);if(ids[i]>=faceCount)continue;
            const auto& s=surfaces[ids[i]];const auto& f=s.face;const auto& p=s.patch;const auto* q=queries+i*13;
            const float3 V=s.T*q[2]+s.B*q[3]+f.normal*q[4],L=s.T*q[5]+s.B*q[6]+f.normal*q[7];
            const uint side=uint(std::sqrt(float(samples)));if(side*side!=samples)continue;
            uint valid=0,cappedCount=0;
            for(uint j=0;j<samples;++j)
            {
                const float a=(float(j%side)+.5f)/side-.5f,b=(float(j/side)+.5f)/side-.5f;
                const float2 uv{q[0]+(q[11]*a*q[9]-q[12]*b*q[10])/width,q[1]+(q[12]*a*q[9]+q[11]*b*q[10])/height};
                for(uint c=0;c<3;++c)out[8+c]+=Sample(maps,width,height,uv,c)/samples;
                const auto base=f.origin+f.du*(uv.x-f.uv0.x)+f.dv*(uv.y-f.uv0.y);
                const float distance=max(.2f,abs(scale)*4/max(.1f,q[4]));
                const SilPomRay ray{base+V*distance,V*(-1.f),0,2*distance+abs(scale)*4};
                const auto hit=SilPomIntersectFace(texture,p,f,ray,s.low);if(hit.status!=SP_HIT)continue;
                const auto hu=hit.faceUv;const auto Ngeo=SilPomFaceNormal(texture,p,f,hu,s.low);
                auto T=normalize(f.du-Ngeo*dot(f.du,Ngeo)),B=normalize(cross(Ngeo,T));
                if(dot(B,f.dv)<0)B=B*(-1.f);
                const float nx=-(Sample(maps,width,height,hu,3)*2-1),ny=Sample(maps,width,height,hu,4)*2-1;
                const float3 N=normalize(T*nx+B*ny+Ngeo*sqrt(max(0.f,1-nx*nx-ny*ny)));
                const float rough=Sample(maps,width,height,hu,5);bool capped=false;
                const float visibility=Visibility(texture,s,hu,L,q[8],capped),nl=max(0.f,dot(N,L));
                out[0]+=dot(N,s.T);out[1]+=dot(N,s.B);out[2]+=dot(N,f.normal);out[3]+=rough;
                for(uint c=0;c<3;++c)out[4+c]+=visibility*Sample(maps,width,height,hu,c)*nl/3.14159265f;
                out[7]+=visibility*Specular(N,V,L,rough);++valid;cappedCount+=capped;
            }
            if(valid)for(uint c=0;c<8;++c)out[c]/=valid;
            out[11]=float(valid)/samples;out[12]=float(cappedCount)/samples;
        }
    });
    for(auto& w:workers)w.join();return true;
}

// Local-plane reference for 4B visibility and 5B bounded depth. Queries use
// the same layout as RenderAppearance. Coverage is returned as a teacher
// diagnostic only: runtime coverage remains the authored raster triangle.
extern "C" __declspec(dllexport) bool RenderSharedDepth(const float* heights,uint width,uint height,
    float scale,float reference,const float* faces,uint faceCount,const float* queries,
    const uint* ids,uint count,uint samples,bool visibility,float* output)
{
    const uint side=uint(std::sqrt(float(samples)));
    if(!width||!height||!faceCount||!samples||samples>256||side*side!=samples)return false;
    std::vector<Surface> surfaces;for(uint i=0;i<faceCount;++i)surfaces.push_back(Unpack(faces+i*27,width,height,scale,reference));
    std::atomic<uint> next{0};std::vector<std::thread> workers;
    for(uint worker=0;worker<8;++worker)workers.emplace_back([&]{
        const Texture texture{width,height,std::vector<float>(heights,heights+size_t(width)*height)};
        for(uint i=next.fetch_add(1);i<count;i=next.fetch_add(1))
        {
            float* out=output+i*3;out[0]=out[1]=out[2]=0;if(ids[i]>=faceCount)continue;
            Surface s=surfaces[ids[i]];auto& f=s.face;const auto* q=queries+i*13;
            const float3 V=s.T*q[2]+s.B*q[3]+f.normal*q[4],L=s.T*q[5]+s.B*q[6]+f.normal*q[7];
            const auto D=SilPomFaceDirection(f,{q[0],q[1]});
            // Continue the local UV metric without treating incidental source
            // triangulation edges as holes in coarse pixel footprints.
            f.origin=f.origin+f.du*(-16-f.uv0.x)+f.dv*(-16-f.uv0.y);
            f.uv0={-16,-16};f.uv1={32,-16};f.uv2={-16,32};f.d0=f.d1=f.d2=D;
            s.low={-16,-16};s.patch={48,48,scale,reference,48,48,-16,-16,width,height,1,65536};
            uint valid=0,cappedCount=0;
            for(uint j=0;j<samples;++j)
            {
                const float a=(float(j%side)+.5f)/side-.5f,b=(float(j/side)+.5f)/side-.5f;
                const float2 uv{q[0]+(q[11]*a*q[9]-q[12]*b*q[10])/width,q[1]+(q[12]*a*q[9]+q[11]*b*q[10])/height};
                if(visibility){bool capped=false;out[0]+=Visibility(texture,s,uv,L,q[8],capped);cappedCount+=capped;++valid;}
                else
                {
                    const auto base=f.origin+f.du*(uv.x-f.uv0.x)+f.dv*(uv.y-f.uv0.y);
                    const float distance=max(.2f,abs(scale)*4/max(.1f,q[4]));
                    const SilPomRay ray{base+V*distance,V*(-1.f),0,2*distance+abs(scale)*4};
                    const auto hit=SilPomIntersectFace(texture,s.patch,f,ray,s.low);
                    if(hit.status!=SP_HIT)continue;
                    out[0]+=SilPomFaceHeight(texture,s.patch,hit.faceUv,s.low).x/scale+reference;++valid;
                }
            }
            if(valid)out[0]/=valid;out[1]=float(valid)/samples;out[2]=float(cappedCount)/samples;
        }
    });
    for(auto& worker:workers)worker.join();return true;
}

// Dedicated continuous-cell visibility labels for whole-hemisphere training.
// Existing appearance/shared-depth reference paths above remain frozen.
extern "C" __declspec(dllexport) bool RenderDirectionalVisibility(const float* heights,uint width,uint height,
    float scale,float reference,uint address,const float* faces,uint faceCount,const float* queries,
    const uint* ids,uint count,uint samples,float* output)
{
    const uint side=uint(std::sqrt(float(samples)));
    if(!width||!height||!faceCount||!samples||samples>256||side*side!=samples)return false;
    std::vector<Surface> surfaces;for(uint i=0;i<faceCount;++i)surfaces.push_back(Unpack(faces+i*27,width,height,scale,reference));
    std::atomic<uint> next{0};std::vector<std::thread> workers;
    for(uint worker=0;worker<8;++worker)workers.emplace_back([&]{
        const Texture texture{width,height,std::vector<float>(heights,heights+size_t(width)*height)};
        for(uint i=next.fetch_add(1);i<count;i=next.fetch_add(1))
        {
            float* out=output+i*3;out[0]=out[1]=out[2]=0;if(ids[i]>=faceCount)continue;
            Surface s=surfaces[ids[i]];auto& f=s.face;const auto* q=queries+i*13;
            const float3 L=s.T*q[5]+s.B*q[6]+f.normal*q[7],D=SilPomFaceDirection(f,{q[0],q[1]});
            f.origin=f.origin+f.du*(-16-f.uv0.x)+f.dv*(-16-f.uv0.y);
            f.uv0={-16,-16};f.uv1={32,-16};f.uv2={-16,32};f.d0=f.d1=f.d2=D;
            s.low={-16,-16};s.patch={48,48,scale,reference,48,48,-16,-16,width,height,address,65536};
            uint valid=0;
            const float det=dot(f.du,cross(f.dv,D));
            if(abs(det)<1e-10f){out[2]=1;continue;}
            const float ur=dot(L,cross(f.dv,D))/det,vr=dot(f.du,cross(L,D))/det,hr=dot(f.du,cross(f.dv,L))/det;
            for(uint j=0;j<samples;++j)
            {
                const float a=(float(j%side)+.5f)/side-.5f,b=(float(j/side)+.5f)/side-.5f;
                const float2 uv{q[0]+(q[11]*a*q[9]-q[12]*b*q[10])/width,q[1]+(q[12]*a*q[9]+q[11]*b*q[10])/height};
                if(hr<=0){++valid;continue;}
                const float start=SilPomFaceHeight(texture,s.patch,uv,s.low).x;
                const float bias=max(1e-5f,abs(scale)*.002f);
                const float end=SilPomPhysicalShadowReach(start,scale*(1-reference),bias,hr,q[8]);
                uint cells=0;const double v=SilPOM::VisibilityReference::Trace(heights,width,height,
                    double(uv.x)*width-.5,double(uv.y)*height-.5,double(ur)*width,double(vr)*height,
                    start,hr,bias,scale,reference,end,address!=0,cells);
                if(v>=0){out[0]+=float(v);++valid;}else out[2]+=1.0f/samples;
            }
            if(valid)out[0]/=valid;out[1]=float(valid)/samples;
        }
    });
    for(auto& worker:workers)worker.join();return true;
}
