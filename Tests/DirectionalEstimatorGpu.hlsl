// SPDX-License-Identifier: MIT
#define SILPOM_DIRECTIONAL_STANDALONE 1
namespace SilPomLightingEstimator {
static const uint CurrentRouting=0,PhysicalRayNeural=1,DirectionalField32=2,AngularOccluders4=3,AngularOccluders8=4;
}
static uint o_silpomLightingEstimator;
struct Face {float3 du,dv,normal;};
static Face silpomLitFace;
#include "../Assets/Shaders/SilPOM/DirectionalEstimator.azsli"
struct Query {float x[20];float3 light;float reach;float steps;float mode;};
StructuredBuffer<Query> requests : register(t0);
RWStructuredBuffer<float> results : register(u0);
cbuffer Constants : register(b0) {uint count;}
[numthreads(64,1,1)]
void Main(uint3 tid:SV_DispatchThreadID)
{
    if(tid.x>=count)return;
    Query q=requests[tid.x];o_silpomLightingEstimator=uint(q.mode);
    silpomLitFace.du=float3(1,0,0);silpomLitFace.dv=float3(0,1,0);silpomLitFace.normal=float3(0,0,1);
    SilPomDecodeDirectionalReceiver(q.x);
    results[tid.x]=SilPomDirectionalVisibility(q.light,uint(q.steps),q.reach);
}
