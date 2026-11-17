// SPDX-License-Identifier: MIT
#if SILPOM_TEST_DISABLE_CACHE
#define SILPOM_FACE_COEFFICIENT_CACHE 0
#else
#define SILPOM_FACE_COEFFICIENT_CACHE 1
#endif
#define SILPOM_HIT_DEPTH 1
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
#include "../Assets/Shaders/SilPOM/FaceCoefficients.azsli"
#if SILPOM_TEST_FORCE_NEWTON_FALLBACK
#include "../Assets/Shaders/SilPOM/FaceCoefficients.azsli"
#define SilPomTryFaceNewtonCoefficients(face,du,dv,tolerance) false
#endif
#if SILPOM_TEST_FORCE_AREA_FALLBACK
#define SilPomTryFaceCoefficientArea(face,area) false
#endif
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
#include "../Assets/Shaders/SilPOM/GradientContext.azsli"
#include "FaceCoefficientLegacy.azsli"
ByteAddressBuffer PackedFaces:register(t0);
StructuredBuffer<SilPomFaceCoefficients> Prepared:register(t1);
Texture2D HeightImage:register(t2);
RWStructuredBuffer<SilPomFaceCoefficients> PrepareOutput:register(u0);
struct CoefficientTestOutput
{
    uint4 flags;
    float4 oldDuArea,newDuArea,oldDvTolerance,newDvTolerance;
    float4 oldNewton,newNewton;
};
RWStructuredBuffer<CoefficientTestOutput> Results:register(u1);
cbuffer Parameters:register(b0){uint FaceCount;}
SilPomMeshFace ReadFace(uint index)
{
    uint o=index*144u;SilPomMeshFace f=(SilPomMeshFace)0;
    f.origin=asfloat(PackedFaces.Load4(o)).xyz;f.du=asfloat(PackedFaces.Load4(o+16u)).xyz;
    f.dv=asfloat(PackedFaces.Load4(o+32u)).xyz;f.normal=asfloat(PackedFaces.Load4(o+48u)).xyz;
    float4 uv=asfloat(PackedFaces.Load4(o+64u));f.uv0=uv.xy;f.uv1=uv.zw;
    f.uv2=asfloat(PackedFaces.Load4(o+80u)).xy;f.d0=asfloat(PackedFaces.Load4(o+96u)).xyz;
    f.d1=asfloat(PackedFaces.Load4(o+112u)).xyz;f.d2=asfloat(PackedFaces.Load4(o+128u)).xyz;return f;
}
bool Equal1(float a,float b){return asuint(a)==asuint(b)||(isnan(a)&&isnan(b));}
bool Equal3(float3 a,float3 b){return Equal1(a.x,b.x)&&Equal1(a.y,b.y)&&Equal1(a.z,b.z);}
bool Equal4(float4 a,float4 b){return Equal3(a.xyz,b.xyz)&&Equal1(a.w,b.w);}
[numthreads(64,1,1)]
void Prepare(uint3 id:SV_DispatchThreadID)
{if(id.x<FaceCount)PrepareOutput[id.x]=SilPomPrepareFaceCoefficients(ReadFace(id.x));}
[numthreads(64,1,1)]
void Compare(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=FaceCount*4)return;
    uint faceIndex=id.x/4,variation=id.x%4;SilPomMeshFace f=ReadFace(faceIndex);
    const SilPomFaceCoefficients stored=Prepared[faceIndex];
    const SilPomFaceCoefficients flagsReference=SilPomPrepareFaceCoefficients(f);
    CoefficientTestOutput output=(CoefficientTestOutput)0;
    const float area=LegacyFaceUvArea(f);
    const float3 oldDu=((f.d1-f.d0)*(f.uv2.y-f.uv0.y)-(f.d2-f.d0)*(f.uv1.y-f.uv0.y))*(1/area);
    const float3 oldDv=((f.d2-f.d0)*(f.uv1.x-f.uv0.x)-(f.d1-f.d0)*(f.uv2.x-f.uv0.x))*(1/area);
    const float base=2e-6f*max(1.0f,max(length(f.du),length(f.dv)));
    output.oldDuArea=float4(oldDu,area);output.newDuArea=stored.duArea;
    output.oldDvTolerance=float4(oldDv,base);output.newDvTolerance=stored.dvTolerance;
    uint failures=0;
    if(!Equal4(output.oldDuArea,stored.duArea))failures|=1u;
    if(!Equal4(output.oldDvTolerance,stored.dvTolerance))failures|=2u;
    if(any(stored.metadata!=flagsReference.metadata))failures|=4u;
    SilPomFaceCoefficients selected=stored;
    if(variation==3)selected.metadata.y=999u; // Unsupported-version fallback.
#if SILPOM_TEST_FORCE_MISSING_CACHE
    selected=SilPomEmptyFaceCoefficients();
#endif
#if SILPOM_FACE_COEFFICIENT_CACHE
    SilPomSetFaceCoefficients(f,selected);
#endif
    if(!Equal1(SilPomUvArea(f),area))failures|=8u;
    float3 gu,gv,oldGu,oldGv;
    const bool gradient=SilPomFaceGradientDirectionPartials(f,gu,gv);
    const bool oldGradient=SilPomGradientDirectionPartials(f.uv0,f.uv1,f.uv2,f.d0,f.d1,f.d2,oldGu,oldGv);
    if(gradient!=oldGradient||(gradient&&(!Equal3(gu,oldGu)||!Equal3(gv,oldGv))))failures|=16u;
    float3 cachedU,cachedV;float cachedTolerance;
    const bool cachedNewton=SilPomTryFaceNewtonCoefficients(f,cachedU,cachedV,cachedTolerance);
    if(variation==3&&cachedNewton)failures|=32u;
    if(abs(area)>1e-9f)
    {
        const float2 uv=(f.uv0+f.uv1+f.uv2)/3;
        if(!Equal3(SilPomFaceWeights(f,uv),LegacyFaceWeights(f,uv))
            ||!Equal3(SilPomFaceDirection(f,uv),LegacyFaceDirection(f,uv)))failures|=64u;
        const float2 uvChecks[3]={uv+float2(.001f,-.001f),uv+float2(.019f,.003f),uv+float2(-.013f,.027f)};
        for(uint q=0;q<3;++q)
            if(!Equal3(SilPomFaceWeights(f,uvChecks[q]),LegacyFaceWeights(f,uvChecks[q]))
                ||!Equal3(SilPomFaceDirection(f,uvChecks[q]),LegacyFaceDirection(f,uvChecks[q])))failures|=64u;
        const float2 low=min(f.uv0,min(f.uv1,f.uv2)),high=max(f.uv0,max(f.uv1,f.uv2));
        SilPomPatch p;
        p.width=high.x-low.x;p.height=high.y-low.y;p.scale=.08f;p.reference=.5f;
        p.tileU=p.width;p.tileV=p.height;p.offsetU=low.x;p.offsetV=low.y;
        p.textureWidth=4;p.textureHeight=4;p.addressMode=0;p.maxCells=4096;
        const float3 height=SilPomFaceHeight(HeightImage,p,uv,low);
        const float3 surface=f.origin+f.du*(uv.x-f.uv0.x)+f.dv*(uv.y-f.uv0.y)+LegacyFaceDirection(f,uv)*height.x;
        SilPomRay ray;ray.origin=surface+f.normal*.2f;ray.direction=-f.normal;ray.tMin=0;ray.tMax=1;
        const float multipliers[4]={.25f,1,5,30};const float multiplier=multipliers[variation];
        if(cachedNewton&&!Equal1(cachedTolerance*multiplier,2e-6f*max(1.0f,max(length(f.du),length(f.dv)))*multiplier))failures|=128u;
        float2 oldUv=uv+float2(.001f,-.001f),newUv=oldUv;float oldT=.18f,newT=oldT;
        const uint oldStatus=LegacyRefineFace(HeightImage,p,f,ray,low,oldUv,oldT,8u,multiplier);
        const uint newStatus=SilPomRefineFace(HeightImage,p,f,ray,low,newUv,newT,8u,multiplier);
        output.oldNewton=float4(float(oldStatus),oldT,oldUv);output.newNewton=float4(float(newStatus),newT,newUv);
        if(!Equal4(output.oldNewton,output.newNewton))failures|=256u;
        output.flags.y=1;output.flags.w=uint(oldStatus==SP_HIT);
    }
    output.flags.x=failures;output.flags.z=uint(gradient);
    Results[id.x]=output;
}
