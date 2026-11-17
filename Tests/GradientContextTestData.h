// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace SilPOM::GradientCpu
{
using uint=std::uint32_t;
using std::abs;using std::min;using std::max;
struct float2 {float x=0,y=0;float2()=default;float2(float a,float b):x(a),y(b){}};
struct float3 {float x=0,y=0,z=0;float3()=default;float3(float a,float b,float c):x(a),y(b),z(c){}};
struct float4 {float x=0,y=0,z=0,w=0;float4()=default;float4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){}};
inline float3 operator+(float3 a,float3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline float3 operator-(float3 a,float3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline float3 operator*(float3 a,float b){return {a.x*b,a.y*b,a.z*b};}
#define SILPOM_GRADIENT_CPU 1
#include "../Assets/Shaders/SilPOM/GradientContext.azsli"
#undef SILPOM_GRADIENT_CPU

// Float4-only layout is shared verbatim with GradientContextGpu.hlsl.
struct GradientInput
{
    float4 clip,clipDu,clipDv,viewportAndSpan;
    float4 positionH,derivativeHX,derivativeHY;
    float4 donorUvA,donorDepthA,donorUvB,donorDepthB;
    float4 centerA,centerB,weightAndAnchor,parameters;
};
struct GradientOutput
{
    float4 uv,depth,worldDx,worldDy,repairUv,repairDepth,morphUv,height;
};
static_assert(sizeof(GradientInput)==15*16);
static_assert(sizeof(GradientOutput)==8*16);
inline GradientOutput EvaluateGradientCase(const GradientInput& a)
{
    GradientOutput r{};
    auto c=SilPomGradientFromTangents(a.clip,a.clipDu,a.clipDv,
        {a.viewportAndSpan.x,a.viewportAndSpan.y},{a.viewportAndSpan.z,a.viewportAndSpan.w},SP_GRADIENT_RELIEF);
    r.uv=c.uv;r.depth=SilPomGradientPackDepth(c);
    float3 dx,dy;const bool valid=SilPomGradientWorldPosition(a.positionH,a.derivativeHX,a.derivativeHY,dx,dy);
    r.worldDx={dx.x,dx.y,dx.z,float(valid)};r.worldDy={dy.x,dy.y,dy.z,float(valid)};
    auto ca=SilPomGradientUnpack(a.donorUvA,a.donorDepthA),cb=SilPomGradientUnpack(a.donorUvB,a.donorDepthB);
    auto repaired=SilPomGradientBlendAnchored({a.centerA.x,a.centerA.y},a.centerA.z,ca,
        {a.centerB.x,a.centerB.y},a.centerB.z,cb,a.centerA.w,{a.weightAndAnchor.x,a.weightAndAnchor.y},
        a.centerB.w!=0,uint(a.parameters.y),{a.weightAndAnchor.z,a.weightAndAnchor.w},a.parameters.x);
    r.repairUv=repaired.uv;r.repairDepth=SilPomGradientPackDepth(repaired);
    r.morphUv=SilPomGradientMorphUv({a.centerA.x,a.centerA.y},{a.centerB.x,a.centerB.y},
        a.donorUvA,a.donorUvB,a.centerA.w,{a.weightAndAnchor.x,a.weightAndAnchor.y});
    const auto h=SilPomGradientBilinear(a.donorUvA,{a.centerA.w,a.centerB.w},{a.parameters.z,a.parameters.w},.7f,.4f);
    r.height={h.x,h.y,h.z,0};return r;
}
inline std::vector<GradientInput> MakeGradientCases(std::size_t count)
{
    std::vector<GradientInput> result(count);
    std::mt19937 rng(28517);std::uniform_real_distribution<float> signedUnit(-1,1),unit(0,1);
    for(std::size_t i=0;i<count;++i)
    {
        auto& a=result[i];const float z=2+6*unit(rng);
        a.clip={signedUnit(rng)*z,signedUnit(rng)*z,.1f+.2f*unit(rng),z};
        a.clipDu={1+.3f*signedUnit(rng),.1f*signedUnit(rng),.03f*signedUnit(rng),.5f*signedUnit(rng)};
        a.clipDv={.1f*signedUnit(rng),1+.3f*signedUnit(rng),.03f*signedUnit(rng),.5f*signedUnit(rng)};
        if(i%19==0)a.clipDv=a.clipDu; // Defined singular fallback.
        if(i%41==0)a.clip.w=-1;
        a.viewportAndSpan={1920,1080,1.5f,2};
        a.positionH={signedUnit(rng),signedUnit(rng),1,.1f+.9f*unit(rng)};
        a.derivativeHX={.001f,.0001f*signedUnit(rng),0,.001f*signedUnit(rng)};
        a.derivativeHY={.0001f*signedUnit(rng),-.002f,0,.001f*signedUnit(rng)};
        a.donorUvA={.01f*signedUnit(rng),.01f*signedUnit(rng),.01f*signedUnit(rng),.01f*signedUnit(rng)};
        a.donorUvB={.01f*signedUnit(rng),.01f*signedUnit(rng),.01f*signedUnit(rng),.01f*signedUnit(rng)};
        a.donorDepthA={.002f*signedUnit(rng),.002f*signedUnit(rng),float(SP_GRADIENT_SHELL),1};
        a.donorDepthB={.002f*signedUnit(rng),.002f*signedUnit(rng),float(SP_GRADIENT_RELIEF),float(i%31==0?2:1)};
        a.centerA={signedUnit(rng),signedUnit(rng),.1f+.8f*unit(rng),unit(rng)};
        a.centerB={signedUnit(rng),signedUnit(rng),.1f+.8f*unit(rng),float(i%2)};
        a.weightAndAnchor={.1f+.4f*unit(rng),.1f*signedUnit(rng),0,0};
        const float w=a.centerA.w,zCenter=a.centerA.z*(1-w)+a.centerB.z*w;
        a.weightAndAnchor.z=a.centerA.x*(1-w)+a.centerB.x*w;
        a.weightAndAnchor.w=a.centerA.y*(1-w)+a.centerB.y*w;
        if(a.centerB.w!=0)
        {
            a.weightAndAnchor.z=(a.centerA.x*a.centerA.z*(1-w)+a.centerB.x*a.centerB.z*w)/zCenter;
            a.weightAndAnchor.w=(a.centerA.y*a.centerA.z*(1-w)+a.centerB.y*a.centerB.z*w)/zCenter;
        }
        a.parameters={zCenter,float(i%2?SP_GRADIENT_REPAIR_DEPTH_WEIGHTED:SP_GRADIENT_REPAIR_ARITHMETIC),7,11};
    }
    return result;
}
}
