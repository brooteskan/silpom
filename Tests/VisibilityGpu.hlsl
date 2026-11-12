// SPDX-License-Identifier: MIT
#define SILPOM_HIT_DEPTH 1
#define SILPOM_HIT_NEURAL 1
#define SILPOM_NEURAL_NO_MARCH 1
#define SILPOM_LEVEL2 false
#define SILPOM_VALIDATION_GATHER false
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
SamplerState testSampler : register(s0);
#define GetMaterialTextureSampler() testSampler
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
#include "../Assets/Shaders/SilPOM/ReliefShadow.azsli"
static uint counters[8];
static float probability;
#define SILPOM_SHADOW_STAT(event) ++counters[event]
#define SILPOM_VISIBILITY_PROBABILITY(value) probability=value
#include "../Assets/Shaders/SilPOM/LevelThree.azsli"
Texture2D heightImage : register(t0);
struct Request {float2 uv;float3 light;float reach;uint steps;};
StructuredBuffer<Request> requests : register(t1);
struct Result {float4 values;uint counts[8];};
RWStructuredBuffer<Result> results : register(u0);
cbuffer Parameters : register(b0) {SilPomPatch patch;uint count;uint model;uint mode;float confidence;}
[numthreads(64,1,1)]
void Main(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=count) return;
    [unroll] for(uint j=0;j<8;++j) counters[j]=0;
    probability=.5f;
    SilPomMeshFace face;
    face.origin=float3(0,0,0);face.du=float3(1,0,0);face.dv=float3(0,1,0);
    face.normal=float3(0,0,1);face.uv0=float2(0,0);face.uv1=float2(1,0);face.uv2=float2(0,1);
    face.d0=float3(0,0,1);face.d1=float3(.2f,0,1);face.d2=float3(0,.1f,1);
    Request q=requests[id.x];
    float visibility=SilPomLearnedReliefVisibility(heightImage,patch,face,q.uv,float2(0,0),q.light,q.steps,q.reach,
        model,mode==2u,confidence);
    Result result;
    result.values=float4(visibility,probability,0,0);
    [unroll] for(uint k=0;k<8;++k) result.counts[k]=counters[k];
    results[id.x]=result;
}
