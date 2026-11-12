// SPDX-License-Identifier: MIT
#pragma once
#define SILPOM_HIT_DEPTH 1
#include "../Tests/CpuShader.h"
#include <array>

namespace SilPOM::Cpu
{
inline float2 operator+(float2 a,float2 b) { return {a.x+b.x,a.y+b.y}; }
inline float2 operator-(float2 a,float2 b) { return {a.x-b.x,a.y-b.y}; }
inline float2 operator*(float2 a,float2 b) { return {a.x*b.x,a.y*b.y}; }
inline float2 operator*(float2 a,float b) { return {a.x*b,a.y*b}; }
inline float2 operator*(float a,float2 b) { return b*a; }
inline float2 operator/(float2 a,float b) { return {a.x/b,a.y/b}; }
inline float2 operator/(float a,float2 b) { return {a/b.x,a/b.y}; }
inline float2 operator-(float a,float2 b) { return {a-b.x,a-b.y}; }
inline float2& operator+=(float2& a,float2 b) { a=a+b;return a; }
inline float3 operator/(float3 a,float b) { return {a.x/b,a.y/b,a.z/b}; }
inline float3 operator*(float a,float3 b) { return b*a; }
inline float3 operator*(float3 a,float3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
inline float3 max(float3 a,float b) { return max(a,float3(b,b,b)); }
inline float clamp(float a,float lo,float hi) { return min(max(a,lo),hi); }
inline float2 clamp(float2 a,float lo,float hi) { return {clamp(a.x,lo,hi),clamp(a.y,lo,hi)}; }
inline float2 clamp(float2 a,float2 lo,float2 hi) { return {clamp(a.x,lo.x,hi.x),clamp(a.y,lo.y,hi.y)}; }
inline float2 frac(float2 a) { return {a.x-floor(a.x),a.y-floor(a.y)}; }
inline float rcp(float a) { return 1.f/a; }
using std::exp;
struct float4
{
    float x,y,z,w;
    float4(float a,float b,float c,float d):x(a),y(b),z(c),w(d) {}
    float& operator[](uint i) { return i==0?x:i==1?y:i==2?z:w; }
};
inline float4 abs(float4 a) { return {abs(a.x),abs(a.y),abs(a.z),abs(a.w)}; }
inline float2 XY(float3 a) { return {a.x,a.y}; }
inline float SampleHeight(const Texture& image,float2 uv)
{
    const float x=uv.x*image.width-.5f,y=uv.y*image.height-.5f;
    const int ix=int(floor(x)),iy=int(floor(y));
    const float fx=x-floor(x),fy=y-floor(y);
    const auto tap=[&](int u,int v)
    {
        u=(u%int(image.width)+int(image.width))%int(image.width);
        v=(v%int(image.height)+int(image.height))%int(image.height);
        return image.Load(u,v);
    };
    const float a=tap(ix,iy),b=tap(ix+1,iy),c=tap(ix,iy+1),d=tap(ix+1,iy+1);
    return (a+(b-a)*fx)+((c+(d-c)*fx)-(a+(b-a)*fx))*fy;
}
// Slots: iterations, candidates, primary successes, retry successes, retries,
// confidence rejects, fill attempts, fills, containment rejects, singular
// Jacobians, escaped iterates, exhausted budgets. Remaining slots are reserved.
inline thread_local std::array<uint,16> levelOneStats{};
inline thread_local const float* levelOneOverride=nullptr;
inline thread_local const float* levelTwoOverride=nullptr;
inline thread_local bool levelTwoEnabled=false,levelCaptureOnly=false,levelCaptured=false;
inline thread_local std::array<float,19> levelFeatures{};
inline thread_local float levelStart=0,levelSpan=0;
inline thread_local int levelProbeHead=-1;
inline bool CaptureFeatures(float* feature,uint size)
{
    if(!levelCaptureOnly) return false;
    levelFeatures={};
    std::copy(feature,feature+size,levelFeatures.begin());
    levelCaptured=true;
    return true;
}
inline void Diagnostic(uint event,uint value)
{
    auto& s=levelOneStats;
    switch(event)
    {
    case 1: ++s[0];break;
    case 2: if(value!=SP_HIT) ++s[8];break;
    case 3: ++s[9];break;
    case 4: ++s[10];break;
    case 5: ++s[11];break;
    case 6: ++s[1];break;
    case 7: ++s[s[1]==1?2:3];break;
    case 8: ++s[5];break;
    case 9: ++s[4];break;
    case 10: ++s[6];break;
    case 11: if(value==SP_HIT) ++s[7];break;
    case 12: ++s[12];break;
    case 13: ++s[13];break;
    case 14: ++s[14];break;
    case 15: ++s[15];break;
    }
}
inline bool OverrideLevelTwo(float* feature,uint,float* output)
{
    if(!levelTwoOverride) return false;
    const float* w=levelTwoOverride;
    for(uint k=0;k<5;++k) output[k]=w[600+k];
    for(uint j=0;j<24;++j)
    {
        float unit=w[456+j];
        for(uint k=0;k<19;++k) unit+=feature[k]*w[j*19+k];
        unit=max(unit,0.f);unit*=unit;
        for(uint k=0;k<5;++k) output[k]+=unit*w[480+j*5+k];
    }
    if(levelProbeHead>=0)
    {
        output[0]=output[levelProbeHead];
        output[2]=100.f;output[3]=100.f;output[4]=-100.f;
    }
    return true;
}
inline bool OverrideModel(float feature[11],uint,float2& logits)
{
    if(!levelOneOverride) return false;
    const float* w=levelOneOverride;
    logits={w[224],w[225]};
    for(uint j=0;j<16;++j)
    {
        float unit=w[176+j];
        for(uint k=0;k<11;++k) unit+=feature[k]*w[j*11+k];
        unit=max(unit,0.f);
        logits+=float2(w[192+j*2],w[193+j*2])*(unit*unit);
    }
    return true;
}
#define SILPOM_CPU 1
#define SILPOM_HIT_NEURAL 1
#define SILPOM_NEURAL_NO_MARCH 1
#define SILPOM_EVALUATION 1
#define Texture2D const Texture&
#define SP_REF(T) T&
#define SP_TEX const Texture&
#define SP_ZERO(T) T{}
#define SP_XY(value) XY(value)
#define SP_UNROLL
#define SP_BRANCH
#define SP_LOOP
#define SP_SAMPLE_HEIGHT(texture,uv) SampleHeight(texture,uv)
#define SILPOM_DIAGNOSTIC(event,value) Diagnostic(event,value)
#define SILPOM_OVERRIDE_MODEL(feature,model,logits) OverrideModel(feature,model,logits)
#define SILPOM_OVERRIDE_LEVEL2(feature,model,output) OverrideLevelTwo(feature,model,output)
#define SILPOM_CAPTURE_FEATURES(feature,size) CaptureFeatures(feature,size)
#define SILPOM_CAPTURE_INTERVAL(start,span) levelStart=start;levelSpan=span
#define SILPOM_LEVEL2 levelTwoEnabled
#define SP_OUT
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
#include "../Assets/Shaders/SilPOM/MeshFace.azsli"
// Models 1-7 are supported; model 0 is the separate legacy path.
inline SilPomHit SilPomTryTowerNeural(const Texture&,SilPomPatch,SilPomMeshFace,SilPomRay,SilPomRay,float2) { return {}; }
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
#undef SP_OUT
#undef SILPOM_LEVEL2
#undef SILPOM_CAPTURE_FEATURES
#undef SILPOM_CAPTURE_INTERVAL
#undef SILPOM_OVERRIDE_LEVEL2
#undef SILPOM_OVERRIDE_MODEL
#undef SILPOM_DIAGNOSTIC
#undef SP_SAMPLE_HEIGHT
#undef SP_LOOP
#undef SP_BRANCH
#undef SP_UNROLL
#undef SP_XY
#undef SP_ZERO
#undef SP_TEX
#undef SP_REF
#undef Texture2D
#undef SILPOM_EVALUATION
#undef SILPOM_NEURAL_NO_MARCH
#undef SILPOM_HIT_NEURAL
#undef SILPOM_CPU
#undef SILPOM_HIT_DEPTH
}
