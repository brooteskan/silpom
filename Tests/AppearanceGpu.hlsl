// SPDX-License-Identifier: MIT
struct Query {float x[23];};
StructuredBuffer<Query> queries:register(t1);
StructuredBuffer<float> decoder:register(t2);
#define SP_OUT out
#define SP_UNROLL [unroll]
#define SILPOM_APPEARANCE_WEIGHT(i) decoder[i]
#define SILPOM_EXPERIMENT_WEIGHT(i) decoder[i]
#include "../Assets/Shaders/SilPOM/AppearanceDecode.azsli"
#include "../Assets/Shaders/SilPOM/ExperimentalDecode.azsli"
struct Result {float value[12];};
RWStructuredBuffer<Result> results:register(u0);
cbuffer Params:register(b0){uint count;}
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=count)return;
    Query q=queries[id.x];float surface[18];[unroll]for(uint i=0;i<18;++i)surface[i]=q.x[i];
    float sharedInput[20],light[22];[unroll]for(uint i=0;i<22;++i){light[i]=q.x[i];if(i<20)sharedInput[i]=q.x[i];}
    float s[3],l[2];SilPomDecodeAppearanceSurface(surface,0,s);SilPomDecodeAppearanceLight(light,0,l);
    Result result;[unroll]for(uint i=0;i<3;++i)result.value[i]=s[i];result.value[3]=l[0];result.value[4]=l[1];
    float4 derivative=SilPomProxyUvDerivatives(float4(2,1,0,.2),float4(-.3,3,0,.1),float2(q.x[0],q.x[1]),.2f,float2(640,400));
    [unroll]for(uint i=0;i<4;++i)result.value[5+i]=derivative[i];
    result.value[9]=SilPomDecodeSharedVisibility(sharedInput,0);result.value[10]=SilPomDecodeProxyDepth(q.x,0);
    result.value[11]=SilPomProxyRayOffset(q.x[0],.05f,.05f,.8f,.35f+.65f*abs(q.x[1]));results[id.x]=result;
}
