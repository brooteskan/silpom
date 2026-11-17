// SPDX-License-Identifier: MIT
#define Main GradientContextOriginalMain
#include "GradientContextGpu.hlsl"
#undef Main
#include "../Assets/Shaders/SilPOM/GradientConsumers.azsli"
#include "../Assets/Shaders/SilPOM/NormalQueryPolicy.azsli"

[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    GradientContextOriginalMain(id);
    GradientOutput output=Results[id.x];
    const float4 du=float4(1,0,0,1920),dv=float4(0,-768.0f/54,0,0);
    SilPomGradientContext center=SilPomGradientFromTangents(float4(0,0,.1f,768),du,dv,float2(1920,1080),float2(1,1),SP_GRADIENT_PROXY);
    SilPomGradientContext probe=SilPomGradientFromTangents(float4(.4f,0,.1f,1536),du,dv,float2(1920,1080),float2(1,1),SP_GRADIENT_PROXY);
    const float3 zero=float3(0,0,0);
    bool correct=center.validity==SP_GRADIENT_VALID&&probe.validity==SP_GRADIENT_COARSE;
    correct=correct&&SilPomNormalQueryShouldCoarse(center.validity,true,false);
    correct=correct&&abs(SilPomNormalQueryRoughness(.2f,true,center.validity,true,false,zero,zero)-.38f)<2e-7f;
    const uint validity=uint(output.depth.w);
    const bool anyProbeCoarse=(id.x&1u)!=0,appearanceCoarse=(id.x&2u)!=0;
    const float base=.05f+abs(Inputs[id.x].centerA.x)*.8f;
    correct=correct&&!SilPomNormalQueryNeedsProbes(false,true,validity);
    correct=correct&&SilPomNormalQueryRoughness(base,false,validity,anyProbeCoarse,appearanceCoarse,float3(9,9,9),float3(9,9,9))==base;
    correct=correct&&SilPomNormalQueryNeedsProbes(true,true,SP_GRADIENT_VALID);
    correct=correct&&!SilPomNormalQueryNeedsProbes(true,false,SP_GRADIENT_VALID);
    float coarse=SilPomNormalQueryRoughness(base,true,SP_GRADIENT_VALID,true,false,zero,zero);
    correct=correct&&abs(coarse-saturate(base+.18f))<2e-7f;
    const float3 dx=output.worldDx.xyz,dy=output.worldDy.xyz;
    correct=correct&&SilPomNormalQueryRoughness(base,true,SP_GRADIENT_VALID,false,false,dx,dy)
        ==SilPomSpecularAAFromDerivatives(base,dx,dy);
    if(!correct)output.height.w=1;
    Results[id.x]=output;
}
