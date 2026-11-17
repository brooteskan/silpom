// SPDX-License-Identifier: MIT
#include "../Assets/Shaders/SilPOM/GradientContext.azsli"
struct GradientInput
{
    float4 clip,clipDu,clipDv,viewportAndSpan;
    float4 positionH,derivativeHX,derivativeHY;
    float4 donorUvA,donorDepthA,donorUvB,donorDepthB;
    float4 centerA,centerB,weightAndAnchor,parameters;
};
struct GradientOutput
{
    float4 uv,depth,worldDx,worldDy,repairUv,repairDepth,morphUv,height;
};
StructuredBuffer<GradientInput> Inputs:register(t0);
RWStructuredBuffer<GradientOutput> Results:register(u0);
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    GradientInput a=Inputs[id.x];GradientOutput r;
    SilPomGradientContext c=SilPomGradientFromTangents(a.clip,a.clipDu,a.clipDv,
        a.viewportAndSpan.xy,a.viewportAndSpan.zw,SP_GRADIENT_RELIEF);
    r.uv=c.uv;r.depth=SilPomGradientPackDepth(c);
    float3 dx,dy;bool valid=SilPomGradientWorldPosition(a.positionH,a.derivativeHX,a.derivativeHY,dx,dy);
    r.worldDx=float4(dx,float(valid));r.worldDy=float4(dy,float(valid));
    SilPomGradientContext ca=SilPomGradientUnpack(a.donorUvA,a.donorDepthA);
    SilPomGradientContext cb=SilPomGradientUnpack(a.donorUvB,a.donorDepthB);
    SilPomGradientContext repaired=SilPomGradientBlendAnchored(a.centerA.xy,a.centerA.z,ca,
        a.centerB.xy,a.centerB.z,cb,a.centerA.w,a.weightAndAnchor.xy,a.centerB.w!=0,
        uint(a.parameters.y),a.weightAndAnchor.zw,a.parameters.x);
    r.repairUv=repaired.uv;r.repairDepth=SilPomGradientPackDepth(repaired);
    r.morphUv=SilPomGradientMorphUv(a.centerA.xy,a.centerB.xy,a.donorUvA,a.donorUvB,a.centerA.w,a.weightAndAnchor.xy);
    r.height=float4(SilPomGradientBilinear(a.donorUvA,float2(a.centerA.w,a.centerB.w),a.parameters.zw,.7f,.4f),0);
    Results[id.x]=r;
}
