// SPDX-License-Identifier: MIT
struct Query {float x[22];};
StructuredBuffer<Query> queries:register(t1);
StructuredBuffer<float> decoder:register(t2);
#define SP_OUT out
#define SP_UNROLL [unroll]
#define SILPOM_APPEARANCE_WEIGHT(i) decoder[i]
#include "../Assets/Shaders/SilPOM/AppearanceDecode.azsli"
struct Result {float value[9];};
RWStructuredBuffer<Result> results:register(u0);
cbuffer Params:register(b0){uint count;}
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=count)return;
    Query q=queries[id.x];float surface[18];[unroll]for(uint i=0;i<18;++i)surface[i]=q.x[i];
    float s[3],l[2];SilPomDecodeAppearanceSurface(surface,0,s);SilPomDecodeAppearanceLight(q.x,0,l);
    Result result;[unroll]for(uint i=0;i<3;++i)result.value[i]=s[i];result.value[3]=l[0];result.value[4]=l[1];
    float4 derivative=SilPomProxyUvDerivatives(float4(2,1,0,.2),float4(-.3,3,0,.1),float2(q.x[0],q.x[1]),.2f,float2(640,400));
    [unroll]for(uint i=0;i<4;++i)result.value[5+i]=derivative[i];results[id.x]=result;
}
