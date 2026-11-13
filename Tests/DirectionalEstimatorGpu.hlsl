// SPDX-License-Identifier: MIT
#define SILPOM_DIRECTIONAL_STANDALONE 1
namespace SilPomLightingEstimator {
static const uint CurrentRouting=0,PhysicalRayNeural=1,DirectionalField32=2,AngularOccluders4=3,AngularOccluders8=4,AngularEdges4=5;
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
    Query q=requests[tid.x];o_silpomLightingEstimator=q.mode<0?5u:uint(q.mode);
    silpomLitFace.du=float3(1,0,0);silpomLitFace.dv=float3(0,1,0);silpomLitFace.normal=float3(0,0,1);
    if(q.mode<0)
    {
        for(uint k=0;k<4u;++k)
        {
            silpomDirectionalAxes[k]=normalize(float3(q.x[k*3u],q.x[k*3u+1u],q.x[k*3u+2u]));
            silpomDirectionalCosines[k]=float4(q.x[12u+k],q.x[12u+k],q.x[12u+k],q.x[12u+k]);
            silpomAngularInverseWidth[k]=rcp(q.x[16u+k]);
        }
    }
    else SilPomDecodeDirectionalReceiver(q.x);
    results[tid.x]=q.steps<0?SilPomDirectionalAmbientAccessibility():SilPomDirectionalVisibility(q.light,uint(q.steps),q.reach);
}
