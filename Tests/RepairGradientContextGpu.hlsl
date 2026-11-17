// SPDX-License-Identifier: MIT
// Reuse the regular gradient ABI/oracle while checking the conditioning policy
// on hardware with two independently constructed near-far counterexamples.
#define Main GradientContextOriginalMain
#include "GradientContextGpu.hlsl"
#undef Main
#include "../Assets/Shaders/SilPOM/RepairGradientContext.azsli"

bool RepairPolicyCounterexample(float depth)
{
    SilPomGradientContext a=SilPomGradientFromTangents(float4(-2.0f/1920,0,depth,1),
        float4(2.0f/(1920*.05f),0,0,0),float4(0,-2.0f/(1080*.01f),1e-4f,0),float2(1920,1080),float2(1,1),SP_GRADIENT_RELIEF);
    SilPomGradientContext b=SilPomGradientFromTangents(float4(2.0f/1920,0,depth,1),
        float4(2.0f/(1920*.05f),0,0,0),float4(0,-2.0f/(1080*.01f),-1e-4f,0),float2(1920,1080),float2(1,1),SP_GRADIENT_RELIEF);
    SilPomGradientContext raw=SilPomGradientBlend(float2(.1f,.2f),depth,a,float2(.2f,.3f),depth,b,
        .5f,float2(.5f,0),true,SP_GRADIENT_REPAIR_DEPTH_WEIGHTED);
    SilPomGradientContext conditioned=SilPomGradientConditionRepair(raw);
    return a.validity==SP_GRADIENT_VALID&&b.validity==SP_GRADIENT_VALID&&raw.validity==SP_GRADIENT_VALID
        &&abs(raw.uv.z)>1&&conditioned.validity==SP_GRADIENT_COARSE
        &&all(conditioned.uv==float4(1,0,0,1))&&all(conditioned.depth==0)
        &&conditioned.representation==raw.representation;
}
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    GradientContextOriginalMain(id);
    GradientOutput output=Results[id.x];
    SilPomGradientContext regular=SilPomGradientUnpack(output.repairUv,output.repairDepth);
    SilPomGradientContext conditioned=SilPomGradientConditionRepair(regular);
    bool within=all(abs(regular.uv)<=1)&&all(abs(regular.depth)<=1);
    bool preserves=regular.validity==SP_GRADIENT_COARSE||within;
    bool correct=preserves?(all(conditioned.uv==regular.uv)&&all(conditioned.depth==regular.depth)
        &&conditioned.representation==regular.representation&&conditioned.validity==regular.validity):
        (conditioned.validity==SP_GRADIENT_COARSE&&all(conditioned.uv==float4(1,0,0,1))
        &&all(conditioned.depth==0)&&conditioned.representation==regular.representation);
    SilPomGradientContext empty=(SilPomGradientContext)0;
    empty=SilPomGradientConditionRepair(empty);
    correct=correct&&empty.validity==0&&all(empty.uv==0)&&all(empty.depth==0);
    // The existing CPU ABI expects height.w=0. A failed hardware policy check
    // makes this component nonzero and the unchanged runner rejects the case.
    if(!correct||!RepairPolicyCounterexample(1e-8f)||!RepairPolicyCounterexample(1e-12f))output.height.w=1;
    Results[id.x]=output;
}
