// SPDX-License-Identifier: MIT
#define SILPOM_HIT_DEPTH 1
#define SILPOM_HIT_NEURAL 1
#define SILPOM_NEURAL_NO_MARCH 1
#if SILPOM_LEVEL_TWO_TEST
#define SILPOM_LEVEL2 true
#else
#define SILPOM_LEVEL2 false
#endif
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
SamplerState testSampler : register(s0);
#define GetMaterialTextureSampler() testSampler
static uint counters[16];
void Diagnostic(uint event,uint value)
{
    switch(event)
    {
    case 1: ++counters[0];break;
    case 2: if(value!=SP_HIT) ++counters[8];break;
    case 3: ++counters[9];break;
    case 4: ++counters[10];break;
    case 5: ++counters[11];break;
    case 6: ++counters[1];break;
    case 7: ++counters[counters[1]==1?2:3];break;
    case 8: ++counters[5];break;
    case 9: ++counters[4];break;
    case 10: ++counters[6];break;
    case 11: if(value==SP_HIT) ++counters[7];break;
    case 12: ++counters[12];break;
    case 13: ++counters[13];break;
    case 14: ++counters[14];break;
    case 15: ++counters[15];break;
    }
}
#define SILPOM_DIAGNOSTIC(event,value) Diagnostic(event,value)
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
Texture2D heightImage : register(t0);
StructuredBuffer<SilPomRay> rays : register(t1);
struct Result { float4 hit; uint counts[16]; };
RWStructuredBuffer<Result> results : register(u0);
cbuffer Parameters : register(b0) { SilPomPatch patch; uint count; uint model; uint iterations; uint recoveryFixture; }
[numthreads(64,1,1)]
void Main(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=count) return;
    [unroll] for(uint i=0;i<16;++i) counters[i]=0;
    SilPomMeshFace face;
    face.origin=float3(0,0,0);face.du=float3(1,0,0);face.dv=float3(0,1,0);
    face.normal=float3(0,0,1);face.uv0=float2(0,0);face.uv1=float2(1,0);face.uv2=float2(0,1);
    face.d0=float3(0,0,1);face.d1=float3(.2f,0,1);face.d2=float3(0,.1f,1);
    SilPomHit hit;
    if(recoveryFixture!=0u)
        hit=SilPomRecoverNearestHit(heightImage,patch,face,rays[id.x],rays[id.x],float2(0,0),(SilPomHit)0,model);
    else hit=SilPomIntersectFace(heightImage,patch,face,rays[id.x],float2(0,0),model,iterations,1,5);
    Result result;
    result.hit=float4(float(hit.status),hit.t,hit.faceUv);
    [unroll] for(uint j=0;j<16;++j) result.counts[j]=counters[j];
    results[id.x]=result;
}
