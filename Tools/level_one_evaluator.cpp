// SPDX-License-Identifier: MIT
#include "LevelOneCpu.h"
#include <cstring>
#define SILPOM_EXPORT extern "C" __declspec(dllexport)
using namespace SilPOM::Cpu;

// Same 27-float face ABI as the existing teacher. Outputs: count*5 floats and
// count*16 uints. Optional weights: 226 floats in shader order; null uses the
// shipping constants. CPU diagnostics are not a GPU performance simulation.
static bool EvaluatePredictor(const float* source,uint width,uint height,float scale,float reference,uint address,
    const float* packed,uint faceCount,const float* rays,const uint* ids,uint count,uint model,
    uint iterations,uint retries,float tolerance,const float* weights,float* hits,uint* diagnostics,uint level,float* features=nullptr,uint* valid=nullptr,int probeHead=-1)
{
    if(!source||!packed||!rays||!ids||(!features&&(!hits||!diagnostics))||!width||!height||!faceCount
        ||model<1||model>7||iterations<1||iterations>16||retries>4||!std::isfinite(tolerance)||tolerance<=0
        ||!std::isfinite(scale)||!std::isfinite(reference)||address>1) return false;
    if(level<1||level>2||(level==2&&model<4)||((features==nullptr)!=(valid==nullptr))) return false;
    if(weights) for(uint i=0;i<(level==2?605u:226u);++i) if(!std::isfinite(weights[i])) return false;
    for(uint i=0;i<count;++i) if(ids[i]>=faceCount) return false;
    const auto vector=[](const float* p) { return float3(p[0],p[1],p[2]); };
    struct Surface { SilPomMeshFace f;SilPomPatch p;float2 low; };
    std::vector<Surface> surfaces(faceCount);
    for(uint i=0;i<faceCount;++i)
    {
        const float* p=packed+i*27;
        auto& s=surfaces[i];auto& f=s.f;
        f.origin=vector(p);f.du=vector(p+3);f.dv=vector(p+6);
        f.uv0={p[9],p[10]};f.uv1={p[11],p[12]};f.uv2={p[13],p[14]};
        f.d0=vector(p+15);f.d1=vector(p+18);f.d2=vector(p+21);f.normal=vector(p+24);
        s.low={min(f.uv0.x,min(f.uv1.x,f.uv2.x)),min(f.uv0.y,min(f.uv1.y,f.uv2.y))};
        const float w=max(f.uv0.x,max(f.uv1.x,f.uv2.x))-s.low.x;
        const float h=max(f.uv0.y,max(f.uv1.y,f.uv2.y))-s.low.y;
        if(w<=0||h<=0||abs(SilPomUvArea(f))<1e-10f) return false;
        s.p={w,h,scale,reference,w,h,s.low.x,s.low.y,width,height,address,SP_TOWER_NO_MARCH};
    }
    const Texture texture{width,height,std::vector<float>(source,source+size_t(width)*height)};
    levelOneOverride=level==1?weights:nullptr;
    levelTwoOverride=level==2?weights:nullptr;
    levelTwoEnabled=level==2;
    levelCaptureOnly=features!=nullptr;
    levelProbeHead=probeHead;
    for(uint i=0;i<count;++i)
    {
        const auto& s=surfaces[ids[i]];
        const float* p=rays+i*6;
        const SilPomRay ray{vector(p),vector(p+3),0,150};
        levelOneStats={};levelCaptured=false;levelFeatures={};levelStart=0;levelSpan=0;
        const auto hit=SilPomIntersectFace(texture,s.p,s.f,ray,s.low,model,iterations,retries,tolerance);
        if(features)
        {
            std::memcpy(features+i*21,levelFeatures.data(),19*sizeof(float));
            features[i*21+19]=levelStart;features[i*21+20]=levelSpan;
            valid[i]=levelCaptured?1u:0u;
            continue;
        }
        float residual=0;
        if(hit.status==SP_HIT)
        {
            const float h=SilPomFaceHeight(texture,s.p,hit.faceUv,s.low).x;
            const auto actual=s.f.origin+s.f.du*(hit.faceUv.x-s.f.uv0.x)+s.f.dv*(hit.faceUv.y-s.f.uv0.y)
                +SilPomFaceDirection(s.f,hit.faceUv)*h;
            residual=length(actual-(ray.origin+ray.direction*hit.t));
        }
        const float result[]={float(hit.status),hit.t,hit.faceUv.x,hit.faceUv.y,residual};
        std::memcpy(hits+i*5,result,sizeof(result));
        std::memcpy(diagnostics+i*16,levelOneStats.data(),16*sizeof(uint));
    }
    levelOneOverride=nullptr;levelTwoOverride=nullptr;levelTwoEnabled=false;levelCaptureOnly=false;levelProbeHead=-1;
    return true;
}

#define SILPOM_EVAL_ARGS const float* source,uint width,uint height,float scale,float reference,uint address, \
    const float* packed,uint faceCount,const float* rays,const uint* ids,uint count,uint model
#define SILPOM_EVAL_VALUES source,width,height,scale,reference,address,packed,faceCount,rays,ids,count,model
SILPOM_EXPORT bool EvaluateLevelOne(SILPOM_EVAL_ARGS,uint iterations,uint retries,float tolerance,
    const float* weights,float* hits,uint* diagnostics)
{ return EvaluatePredictor(SILPOM_EVAL_VALUES,iterations,retries,tolerance,weights,hits,diagnostics,1); }
SILPOM_EXPORT bool EvaluateLevelTwo(SILPOM_EVAL_ARGS,uint iterations,uint retries,float tolerance,
    const float* weights,float* hits,uint* diagnostics)
{ return EvaluatePredictor(SILPOM_EVAL_VALUES,iterations,retries,tolerance,weights,hits,diagnostics,2); }
SILPOM_EXPORT bool DescribeRays(SILPOM_EVAL_ARGS,uint level,float* features,uint* valid)
{ return EvaluatePredictor(SILPOM_EVAL_VALUES,6,4,1,nullptr,nullptr,nullptr,level,features,valid); }
SILPOM_EXPORT bool ProbeLevelTwo(SILPOM_EVAL_ARGS,uint head,const float* weights,float* hits,uint* diagnostics)
{
    if(head>1||!weights) return false;
    return EvaluatePredictor(SILPOM_EVAL_VALUES,2,0,1,weights,hits,diagnostics,2,nullptr,nullptr,int(head));
}
