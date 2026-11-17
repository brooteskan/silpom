// SPDX-License-Identifier: MIT
struct Query {float x[23];};
StructuredBuffer<Query> queries:register(t1);
StructuredBuffer<float> decoder:register(t2);
#define SP_OUT out
#define SP_UNROLL [unroll]
#define SILPOM_APPEARANCE_WEIGHT(i) decoder[i]
#define SILPOM_MATERIAL_WEIGHT(i) decoder[i]
#define SILPOM_EXPERIMENT_WEIGHT(i) decoder[i]
#include "../Assets/Shaders/SilPOM/MaterialDecode.azsli"
#include "../Assets/Shaders/SilPOM/AppearanceDecode.azsli"
#include "../Assets/Shaders/SilPOM/MaterialNormalDecode.azsli"
#include "../Assets/Shaders/SilPOM/ExperimentalDecode.azsli"
// Minimal receiver fixture around the production shared-visibility helper.
// Only frame/material acquisition is supplied here; preparation and queries
// themselves come from the same include used by forward lighting.
struct ReceiverTestFace {float3 du,dv,normal;};
struct MaterialParameters {uint m_silpomMaterialDecoderBuffer;};
static ReceiverTestFace silpomLitFace;
static float2 silpomLitUv;
static uint receiverDirectionAcquisitions;
float3 SilPomFaceDirection(ReceiverTestFace face,float2 uv)
{
    receiverDirectionAcquisitions++;
    return normalize(face.normal+.07f*uv.x*face.du-.05f*uv.y*face.dv);
}
#include "../Assets/Shaders/SilPOM/SharedVisibility.azsli"
struct Result {float value[42];};
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
    result.value[21]=1;result.value[22]=l[0];
    float materialInput[12];[unroll]for(uint i=0;i<12;++i)materialInput[i]=q.x[i];
    float materialFull[6],materialNormal[2],appearanceNormal[2];
    SilPomDecodeMaterial(materialInput,0,materialFull);
    SilPomDecodeMaterialNormal(materialInput,0,materialNormal);
    SilPomDecodeAppearanceNormal(surface,0,appearanceNormal);
    [unroll]for(uint i=0;i<2;++i)
    {
        result.value[23+i]=asuint(materialFull[3+i])==asuint(materialNormal[i])?0:1;
        result.value[25+i]=asuint(s[i])==asuint(appearanceNormal[i])?0:1;
    }
    SilPomSharedVisibilityPrefix sharedPrefix=SilPomPrepareSharedVisibilityPrefix(sharedInput,0);
    SilPomAppearanceLightPrefix appearancePrefix=SilPomPrepareAppearanceLightPrefix(light,0);
    float cachedShared=SilPomDecodeSharedVisibilityWithPrefix(sharedInput,0,sharedPrefix),cachedLight[2];
    SilPomDecodeAppearanceLightWithPrefix(light,0,appearancePrefix,cachedLight);
    result.value[27]=abs(cachedShared-result.value[9]);
    [unroll]for(uint i=0;i<2;++i)result.value[28+i]=abs(cachedLight[i]-l[i]);
    // Reuse the center's prefix while light/suffix inputs change. The receiver
    // prefix must be a preactivation: suffix terms may cross the ReLU boundary.
    [unroll]for(uint i=8;i<20;++i)sharedInput[i]=-.75f*sharedInput[i]+.1f;
    [unroll]for(uint i=11;i<22;++i)light[i]=-.5f*light[i]-.2f;
    cachedShared=SilPomDecodeSharedVisibilityWithPrefix(sharedInput,0,sharedPrefix);
    SilPomDecodeAppearanceLightWithPrefix(light,0,appearancePrefix,cachedLight);
    result.value[30]=abs(cachedShared-SilPomDecodeSharedVisibility(sharedInput,0));
    SilPomDecodeAppearanceLight(light,0,l);
    [unroll]for(uint i=0;i<2;++i)result.value[31+i]=abs(cachedLight[i]-l[i]);
    // A different center prepares its own prefix rather than reusing state from
    // an earlier center/AA probe. Continue to compare against untouched decoders.
    [unroll]for(uint i=0;i<8;++i)sharedInput[i]=.5f*sharedInput[i]-.3f;
    [unroll]for(uint i=0;i<11;++i)light[i]=.75f*light[i]+.2f;
    sharedPrefix=SilPomPrepareSharedVisibilityPrefix(sharedInput,0);
    appearancePrefix=SilPomPrepareAppearanceLightPrefix(light,0);
    cachedShared=SilPomDecodeSharedVisibilityWithPrefix(sharedInput,0,sharedPrefix);
    SilPomDecodeAppearanceLightWithPrefix(light,0,appearancePrefix,cachedLight);
    result.value[33]=abs(cachedShared-SilPomDecodeSharedVisibility(sharedInput,0));
    SilPomDecodeAppearanceLight(light,0,l);
    [unroll]for(uint i=0;i<2;++i)result.value[34+i]=abs(cachedLight[i]-l[i]);
    result.value[36]=0;
    [unroll]for(uint i=0;i<12;++i)if(sharedPrefix.hidden[i]<0)result.value[36]+=1;
    [unroll]for(uint i=0;i<16;++i)if(appearancePrefix.hidden[i]<0)result.value[36]+=1;
    silpomLitFace.du=float3(2,.2f,0);silpomLitFace.dv=float3(.3f,1.7f,.2f);
    silpomLitFace.normal=normalize(cross(silpomLitFace.du,silpomLitFace.dv));
    silpomLitUv=float2(q.x[0],q.x[1]);
    [unroll]for(uint i=0;i<8;++i)silpomSharedState[i]=q.x[i];
    receiverDirectionAcquisitions=0;
    MaterialParameters params;params.m_silpomMaterialDecoderBuffer=0;
    SilPomSharedVisibilityReceiver receiver;
    SilPomInitSharedVisibilityReceiver(receiver);
    result.value[40]=receiver.framePrepared||receiver.prepared?1:0;
    const float3 firstLight=normalize(silpomLitFace.normal+.2f*silpomLitFace.du);
    const float3 secondLight=normalize(silpomLitFace.normal-.3f*silpomLitFace.dv);
    float disabled=SilPomSharedVisibilityPrepared(firstLight,0,.3f,params,receiver);
    if(disabled!=1||receiver.framePrepared||receiver.prepared||receiverDirectionAcquisitions!=0)result.value[40]+=1;
    float backside=SilPomSharedVisibilityPrepared(-silpomLitFace.normal,16,.3f,params,receiver);
    if(backside!=0||!receiver.framePrepared||receiver.prepared||receiverDirectionAcquisitions!=0)result.value[40]+=1;
    float firstVisibility=SilPomSharedVisibilityPrepared(firstLight,16,.3f,params,receiver);
    if(!receiver.prepared||receiverDirectionAcquisitions!=1)result.value[40]+=1;
    float secondVisibility=SilPomSharedVisibilityPrepared(secondLight,16,0,params,receiver);
    if(receiverDirectionAcquisitions!=1)result.value[40]+=1;
    result.value[37]=abs(firstVisibility-SilPomSharedVisibility(firstLight,16,.3f,params));
    result.value[38]=abs(secondVisibility-SilPomSharedVisibility(secondLight,16,0,params));
    SilPomSharedVisibilityReceiver unavailable;
    SilPomInitSharedVisibilityReceiver(unavailable);unavailable.cacheAvailable=false;
    float uncached=SilPomSharedVisibilityPrepared(secondLight,16,.1f,params,unavailable);
    result.value[39]=abs(uncached-SilPomSharedVisibility(secondLight,16,.1f,params));
    if(unavailable.framePrepared||unavailable.prepared)result.value[40]+=1;
    // Another sample/receiver begins after its own center publication.
    [unroll]for(uint i=0;i<8;++i)silpomSharedState[i]=.6f*q.x[i]-.15f;
    silpomLitUv=.75f*silpomLitUv+.1f;
    SilPomSharedVisibilityReceiver nextReceiver;SilPomInitSharedVisibilityReceiver(nextReceiver);
    result.value[41]=abs(SilPomSharedVisibilityPrepared(firstLight,16,.2f,params,nextReceiver)
        -SilPomSharedVisibility(firstLight,16,.2f,params));
    results[id.x]=result;
}
