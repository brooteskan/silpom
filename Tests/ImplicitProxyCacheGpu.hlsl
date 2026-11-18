// SPDX-License-Identifier: MIT
ByteAddressBuffer Cases:register(t0);
namespace PassSrg
{
    ByteAddressBuffer m_silpomFaceCache:register(t1);
    ByteAddressBuffer m_silpomFaceCacheTable:register(t2);
#if defined(SILPOM_TEST_EMPTY_TABLE)
    static const uint m_silpomFaceCacheBatchCount=0;
#else
    static const uint m_silpomFaceCacheBatchCount=2;
#endif
}
namespace Bindless
{
    ByteAddressBuffer CoefficientStub:register(t3);
    ByteAddressBuffer GetByteAddressBuffer(uint index){return CoefficientStub;}
}
#include "../Assets/Shaders/SilPOM/HitReuse/SilPOMFaceCacheRead.azsli"
#include "Reference/ImplicitProxyCoverageLegacy.hlsl"
#define Cache PassSrg::m_silpomFaceCache
RWByteAddressBuffer CacheOutput:register(u0);
RWByteAddressBuffer Output:register(u1);
cbuffer Constants:register(b0){uint CaseCount;}
struct TestCase
{
    SilPomMeshFace face;
    float4x4 model,vp;
    SilPomImplicitProfile profile;
    SilPomLegacyImplicitProfile legacyProfile;
    float projectionY;
    bool selected;
};
TestCase LoadCase(uint index)
{
    TestCase input;const uint offset=index*320u;
    input.face.origin=asfloat(Cases.Load4(offset)).xyz;
    input.face.du=asfloat(Cases.Load4(offset+16u)).xyz;
    input.face.dv=asfloat(Cases.Load4(offset+32u)).xyz;
    input.face.normal=asfloat(Cases.Load4(offset+48u)).xyz;
    const float4 uv=asfloat(Cases.Load4(offset+64u));
    input.face.uv0=uv.xy;input.face.uv1=uv.zw;
    input.face.uv2=asfloat(Cases.Load4(offset+80u)).xy;
    input.face.d0=asfloat(Cases.Load4(offset+96u)).xyz;
    input.face.d1=asfloat(Cases.Load4(offset+112u)).xyz;
    input.face.d2=asfloat(Cases.Load4(offset+128u)).xyz;
    [unroll] for(uint row=0;row<4;++row)
    {
        input.model[row]=asfloat(Cases.Load4(offset+144u+row*16u));
        input.vp[row]=asfloat(Cases.Load4(offset+208u+row*16u));
    }
    const float4 profile=asfloat(Cases.Load4(offset+272u));
    const uint4 policy=Cases.Load4(offset+288u);
    const float4 extra=asfloat(Cases.Load4(offset+304u));
    input.profile.scale=profile.x;input.profile.reference=profile.y;
    input.profile.pixelBudget=profile.z;input.profile.maxDistance=profile.w;
    input.profile.reliefMode=asuint(extra.y);input.profile.neuralModel=policy.y;
    input.profile.proxyLodEnabled=policy.z!=0u;
    input.legacyProfile.scale=profile.x;input.legacyProfile.reference=profile.y;
    input.legacyProfile.pixelBudget=profile.z;input.legacyProfile.maxDistance=profile.w;
    input.legacyProfile.maxCells=policy.x;input.legacyProfile.neuralModel=policy.y;
    input.legacyProfile.proxyLodEnabled=policy.z!=0u;
    input.selected=policy.w!=0u;input.projectionY=extra.x;
    return input;
}
[numthreads(64,1,1)]
void PrepareCS(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=CaseCount)return;
    const TestCase input=LoadCase(id.x);
    SilPomImplicitCoverage value=SilPomBuildImplicitCoverage(input.face,input.profile,input.model,input.vp,
        input.projectionY,true,true,input.selected);
    value.depth=SilPomImplicitNearestDepth(input.face,input.profile.scale,input.profile.reference,input.model,input.vp);
    const uint offset=id.x*48u;
    CacheOutput.Store4(offset,asuint(float4(value.ndc[0],value.ndc[1])));
    CacheOutput.Store4(offset+16u,asuint(float4(value.ndc[2],value.ndc[3])));
    CacheOutput.Store4(offset+32u,uint4(asuint(value.depth),value.status,0u,0u));
    if(id.x==0u)
    {
        CacheOutput.Store4(CaseCount*48u+32u,uint4(0u,0u,0u,0u));
        CacheOutput.Store4((CaseCount+1u)*48u+32u,uint4(0u,3u,0u,0u));
    }
}
[numthreads(64,1,1)]
void CompareCS(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=CaseCount)return;
    const TestCase input=LoadCase(id.x);
    const uint cacheOffset=id.x*48u,offset=id.x*96u;
    const uint4 cached0=Cache.Load4(cacheOffset),cached1=Cache.Load4(cacheOffset+16u);
    const uint4 cachedDepth=Cache.Load4(cacheOffset+32u);
    const float2 corners[4]={float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
    uint4 old0=0,old1=0;uint differing=0,oldStatus=0;
    [unroll] for(uint i=0;i<4;++i)
    {
        const SilPomImplicitCoverage legacy=SilPomLegacyImplicitCoverage(input.face,input.legacyProfile,input.model,input.vp,
            input.projectionY,input.selected,corners[i]);
        const uint2 bits=asuint(legacy.ndc[0]);oldStatus=legacy.status;
        const SilPomImplicitCoverage fallback=SilPomBuildImplicitCoverage(input.face,input.profile,input.model,input.vp,
            input.projectionY,true,true,input.selected,true,corners[i]);
        if(i==0)old0.xy=bits;if(i==1)old0.zw=bits;
        if(i==2)old1.xy=bits;if(i==3)old1.zw=bits;
        const uint2 cached=i<2?(i==0?cached0.xy:cached0.zw):(i==2?cached1.xy:cached1.zw);
        if(any(bits!=cached)||legacy.status!=cachedDepth.y
            ||any(bits!=asuint(fallback.ndc[0]))||legacy.status!=fallback.status)++differing;
    }
    const uint oldDepth=asuint(SilPomLegacyNearestDepth(input.face,input.legacyProfile,input.model,input.vp));
    uint lookupErrors=0;
    SilPomImplicitCoverage readValue;
    const bool available=SilPomReadImplicitFaceCache(54321u,12345u,id.x,readValue);
#if defined(SILPOM_TEST_EMPTY_TABLE)
    if(available)++lookupErrors;
    // Both resources are intentionally unbound for this path. A zero-count
    // table must reject before touching the table or coefficient descriptor.
    const SilPomFaceCoefficients coefficients=SilPomReadImmutableFaceCoefficients(0u,id.x);
#else
    if(!available||readValue.status!=cachedDepth.y||asuint(readValue.depth)!=cachedDepth.x)++lookupErrors;
    if(available&&(any(asuint(float4(readValue.ndc[0],readValue.ndc[1]))!=cached0)
        ||any(asuint(float4(readValue.ndc[2],readValue.ndc[3]))!=cached1)))++lookupErrors;
    const SilPomFaceCoefficients coefficients=SilPomReadImmutableFaceCoefficients(0xffffffffu,id.x);
#endif
    if(any(coefficients.metadata!=0u)||any(asuint(coefficients.duArea)!=0u)||any(asuint(coefficients.dvTolerance)!=0u))++lookupErrors;
    if(SilPomReadImplicitFaceCache(54322u,12345u,id.x,readValue))++lookupErrors;
    if(SilPomReadImplicitFaceCache(54321u,12344u,id.x,readValue))++lookupErrors;
    if(SilPomReadImplicitFaceCache(54321u,12346u,id.x,readValue))++lookupErrors; // unavailable header
    if(SilPomReadImplicitFaceCache(54321u,12345u,CaseCount+2u,readValue))++lookupErrors; // outside owner range
    if(SilPomReadImplicitFaceCache(54321u,12345u,CaseCount,readValue))++lookupErrors; // missing record
    if(SilPomReadImplicitFaceCache(54321u,12345u,CaseCount+1u,readValue))++lookupErrors; // unknown status
    Output.Store4(offset,uint4(differing,oldDepth!=cachedDepth.x?1u:0u,oldStatus,cachedDepth.y));
    Output.Store4(offset+16u,old0);Output.Store4(offset+32u,old1);
    Output.Store4(offset+48u,cached0);Output.Store4(offset+64u,cached1);
    Output.Store4(offset+80u,uint4(oldDepth,cachedDepth.x,lookupErrors,0u));
}
