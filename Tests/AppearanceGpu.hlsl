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
struct Result {float value[23];};
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
    result.value[11]=SilPomProxyRayOffset(q.x[0],.05f,.05f,.8f,.35f+.65f*abs(q.x[1]));
    float3 origin=float3(.3,-.2,.5),du=float3(2,0,0),dv=float3(.4,1.7,0),N=float3(0,0,1),D=float3(.2,.1,.97);
    float3 V=normalize(float3(q.x[3],q.x[4],.35f+.65f*abs(q.x[5])));
    float2 uv=float2(q.x[0],q.x[1]);float objectScale=.5f+abs(q.x[6]);
    SilPomProxyPlaneHit hit=SilPomResolveProxyPlane(uv,origin,float2(0,0),du,dv,N,D,V,result.value[10],.05f,.05f,objectScale);
    SilPomProxyQuery restored=SilPomRestoreProxyQuery(hit.position,V,origin,du,dv,N,float2(0,0));
    result.value[12]=restored.uv.x;result.value[13]=restored.uv.y;
    [unroll]for(uint i=0;i<3;++i)result.value[14+i]=hit.position[i];
    result.value[17]=sin(restored.uv.x*17)+cos(restored.uv.y*13);
    result.value[18]=hit.uv.x;result.value[19]=hit.uv.y;result.value[20]=hit.position.z/(hit.position.z+2);
    // Independent controls must permit a unit-gain comparison without changing
    // coefficients, descriptors, features, geometry or lighting configuration.
    result.value[21]=1;result.value[22]=l[0];results[id.x]=result;
}
