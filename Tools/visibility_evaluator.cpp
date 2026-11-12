// SPDX-License-Identifier: MIT
#include "VisibilityCpu.h"
#include "visibility_reference.h"
#include <cstring>
using namespace SilPOM::Cpu;

// Requests: UV(2), unit direction(3), reach, sample count. Same 27-float face
// packing as the intersection evaluator. Outputs: baseline, method, exact,
// probability; diagnostics 12 uints; descriptor 10 floats per request.
extern "C" __declspec(dllexport) bool EvaluateVisibility(const float* source,uint width,uint height,
    float scale,float reference,uint address,const float* packed,uint faceCount,const float* requests,
    const uint* ids,uint count,uint model,uint mode,float confidence,const float* weights,
    float* output,uint* diagnostics,float* features)
{
    if(!source||!packed||!requests||!ids||!output||!diagnostics||!features||!width||!height
        ||mode>2||model>7||!std::isfinite(confidence)||confidence<.5f||confidence>=1) return false;
    for(uint i=0;i<count;++i) if(ids[i]>=faceCount) return false;
    if(weights) for(uint i=0;i<145;++i) if(!std::isfinite(weights[i])) return false;
    const auto vector=[](const float* p) {return float3(p[0],p[1],p[2]);};
    const Texture texture{width,height,std::vector<float>(source,source+size_t(width)*height)};
    visibilityOverride=weights;
    for(uint i=0;i<count;++i)
    {
        const auto* p=packed+ids[i]*27;const auto* q=requests+i*7;
        SilPomMeshFace face{};
        face.origin=vector(p);face.du=vector(p+3);face.dv=vector(p+6);
        face.uv0={p[9],p[10]};face.uv1={p[11],p[12]};face.uv2={p[13],p[14]};
        face.d0=vector(p+15);face.d1=vector(p+18);face.d2=vector(p+21);face.normal=vector(p+24);
        const float2 low{min(face.uv0.x,min(face.uv1.x,face.uv2.x)),min(face.uv0.y,min(face.uv1.y,face.uv2.y))};
        const float w=max(face.uv0.x,max(face.uv1.x,face.uv2.x))-low.x;
        const float h=max(face.uv0.y,max(face.uv1.y,face.uv2.y))-low.y;
        if(w<=0||h<=0||q[6]<0||q[6]>64||q[5]<0) return false;
        const SilPomPatch patch{w,h,scale,reference,w,h,low.x,low.y,width,height,address,4096};
        const float2 uv{q[0],q[1]};const auto light=vector(q+2);const uint steps=uint(q[6]);
        uint64_t before=texture.loads;
        float baseline=SilPomReliefVisibility(texture,patch,face,uv,low,light,steps,q[5]);
        const uint baselineQuads=uint((texture.loads-before)/4);
        SilPomLocalShadowRay ray{};float immediate=1;uint cells=0;
        double exact=immediate;
        if(SilPomPrepareLocalShadow(texture,patch,face,uv,low,light,steps,q[5],ray,immediate))
            exact=SilPOM::VisibilityReference::Trace(source,int(width),int(height),
                double(uv.x)*width-.5,double(uv.y)*height-.5,double(ray.uRate)*width,double(ray.vRate)*height,
                ray.start,ray.hRate,ray.bias,scale,reference,ray.reach,address!=0,cells);
        else exact=immediate;
        visibilityStats={};visibilityCaptured=false;visibilityFeatures={};visibilityProbability=.5f;
        const float result=mode==0?baseline:SilPomLearnedReliefVisibility(texture,patch,face,uv,low,light,steps,q[5],
            model,mode==2,confidence);
        visibilityStats[8]=baselineQuads;visibilityStats[9]=cells;visibilityStats[10]=visibilityCaptured?1:0;
        const float values[]={baseline,result,float(exact),visibilityProbability};
        std::memcpy(output+i*4,values,sizeof(values));
        std::memcpy(diagnostics+i*12,visibilityStats.data(),12*sizeof(uint));
        std::memcpy(features+i*10,visibilityFeatures.data(),10*sizeof(float));
    }
    visibilityOverride=nullptr;
    return true;
}
