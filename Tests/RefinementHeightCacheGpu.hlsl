// SPDX-License-Identifier: MIT
SamplerState testSampler:register(s0);
#define GetMaterialTextureSampler() testSampler
#ifndef SILPOM_TEST_GATHER
#define SILPOM_TEST_GATHER 0
#endif
#define SILPOM_VALIDATION_GATHER SILPOM_TEST_GATHER
#define SILPOM_HIT_DEPTH 1
static uint diagnosticIterations=0,diagnosticHash=2166136261u;
void RefinementDiagnostic(uint event,uint value)
{
    if(event==1)++diagnosticIterations;
    diagnosticHash=(diagnosticHash^event)*16777619u;
    diagnosticHash=(diagnosticHash^value)*16777619u;
}
#define SILPOM_DIAGNOSTIC(event,value) RefinementDiagnostic(event,value)
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
#include "Reference/RefineFaceUncached.azsli"
Texture2D heightImage:register(t0);
StructuredBuffer<float2> coordinates:register(t1);
struct Result {float4 oldNewton,newNewton;uint4 diagnostics,directChecks;};
RWStructuredBuffer<Result> results:register(u0);
cbuffer Parameters:register(b0){SilPomPatch patch;uint count;}
float3 UncachedHeight(float2 uv)
{
#if SILPOM_TEST_GATHER
    return SilPomGatherFaceHeight(heightImage,patch,uv,float2(0,0));
#else
    return SilPomFaceHeight(heightImage,patch,uv,float2(0,0));
#endif
}
bool Equal3(float3 a,float3 b){return all(asuint(a)==asuint(b));}
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=count)return;
    SilPomMeshFace face=(SilPomMeshFace)0;
    face.du=float3(1,.05f,.02f);face.dv=float3(.03f,1,-.01f);
    face.normal=normalize(cross(face.du,face.dv));face.uv1=float2(1,0);face.uv2=float2(0,1);
    face.d0=face.normal;face.d1=face.normal+float3(.12f,0,.03f);face.d2=face.normal+float3(0,.09f,-.02f);
#if SILPOM_FACE_COEFFICIENT_CACHE
    SilPomFaceCoefficients coefficients=SilPomPrepareFaceCoefficients(face);
    if(id.x%4==1)coefficients=SilPomEmptyFaceCoefficients();
    else if(id.x%4==2)coefficients.metadata.y=999u;
    else if(id.x%4==3)coefficients.metadata.x&=~SP_FACE_COEFFICIENT_NEWTON;
    SilPomSetFaceCoefficients(face,coefficients);
#endif
    const float2 target=coordinates[id.x];
    const float3 h=UncachedHeight(target);
    const float3 surface=face.du*target.x+face.dv*target.y+SilPomFaceDirection(face,target)*h.x;
    const float3 direction=normalize(float3(.15f*sin(float(id.x)),.1f*cos(float(id.x)),-1));
    SilPomRay ray;ray.origin=surface-direction*.2f;ray.direction=direction;
    ray.tMin=0;ray.tMax=id.x%19==0?.15f:1.f;
    const uint budgets[6]={0,1,2,4,8,16};const float multipliers[4]={.25f,1,5,30};
    const uint budget=budgets[id.x%6];const float tolerance=multipliers[(id.x/6)%4];
    const float2 seed=target+float2(.007f*sin(float(id.x)*.7f),.011f*cos(float(id.x)*.3f));
    float2 oldUv=seed,newUv=seed;float oldT=.18f,newT=oldT;
    diagnosticIterations=0;diagnosticHash=2166136261u;
    const uint oldStatus=SilPomRefineFaceUncachedReference(heightImage,patch,face,ray,float2(0,0),oldUv,oldT,budget,tolerance);
    const uint oldIterations=diagnosticIterations,oldHash=diagnosticHash;
    diagnosticIterations=0;diagnosticHash=2166136261u;
    const uint newStatus=SilPomRefineFace(heightImage,patch,face,ray,float2(0,0),newUv,newT,budget,tolerance);
    Result result;
    result.oldNewton=float4(oldUv,oldT,float(oldStatus));result.newNewton=float4(newUv,newT,float(newStatus));
    result.diagnostics=uint4(oldIterations,diagnosticIterations,oldHash,diagnosticHash);
    // Check the production height helper directly with changing fractions,
    // identical cells, adjacent cells, returns, and whole-image wraps.
    SilPomRefineHeightCell cell=(SilPomRefineHeightCell)0;
    const float2 texel=float2(patch.width/(patch.tileU*patch.textureWidth),patch.height/(patch.tileV*patch.textureHeight));
    const float2 offsets[8]={float2(0,0),texel*.01f,texel*.02f,texel,texel*.01f,
        float2(patch.width/patch.tileU,0),float2(0,0),float2(0,0)};
    uint failures=0;
    [loop] for(uint i=0;i<8;++i)
    {
        const float2 uv=target+offsets[i];
        const float3 actual=SilPomRefineFaceHeight(heightImage,patch,uv,float2(0,0),cell);
        if(!Equal3(UncachedHeight(uv),actual))failures|=1u<<i;
    }
    // A second independent candidate/sample must start cold, even when its
    // seed happens to belong to the same integer cell as the previous solve.
    float2 nextOld=target+texel*.03f,nextNew=nextOld;float nextOldT=.19f,nextNewT=nextOldT;
    diagnosticIterations=0;diagnosticHash=2166136261u;
    const uint nextOldStatus=SilPomRefineFaceUncachedReference(heightImage,patch,face,ray,float2(0,0),nextOld,nextOldT,8,tolerance);
    const uint nextOldHash=diagnosticHash;
    diagnosticIterations=0;diagnosticHash=2166136261u;
    const uint nextNewStatus=SilPomRefineFace(heightImage,patch,face,ray,float2(0,0),nextNew,nextNewT,8,tolerance);
    if(nextOldStatus!=nextNewStatus||any(asuint(nextOld)!=asuint(nextNew))||asuint(nextOldT)!=asuint(nextNewT)
        ||nextOldHash!=diagnosticHash)failures|=256u;
    result.directChecks=uint4(failures,oldStatus==1,newStatus==1,nextOldStatus==1);
    results[id.x]=result;
}
