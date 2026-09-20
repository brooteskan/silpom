// SPDX-License-Identifier: MIT
// Native compute adapter for the same kernel used by Atom mesh materials.
Texture2D<float> heightImage : register(t0);
StructuredBuffer<float4> fragmentData : register(t2);
cbuffer Parameters : register(b0)
{
    float baseScale; float amplitude; float reference; uint addressMode;
    uint textureWidth; uint textureHeight; uint fragmentCount; uint maxNodes;
    uint maxDepth; uint rayCount; uint firstRay; float requiredDepthAccuracy;
    uint packedTriangleCount;
};
#define SP_CURVED_DATA(index) fragmentData[index]
#define SP_CURVED_HEIGHT(coord) heightImage.Load(coord)
#include "../../Assets/Shaders/SilPOM/Curved/Intersection.azsli"
StructuredBuffer<SilPomCurved_Ray> rays : register(t1);
RWStructuredBuffer<SilPomCurved_Result> results : register(u0);
[numthreads(64,1,1)]
void Main(uint3 id : SV_DispatchThreadID)
{
    uint index=id.x+firstRay;
    if(index<rayCount) results[index]=SilPomCurved_Intersect(rays[index]);
}
[numthreads(64,1,1)]
void RationalFallback(uint3 id : SV_DispatchThreadID)
{
    uint index=id.x+firstRay;
    if(index<rayCount) results[index]=SilPomCurved_Resolve(rays[index],results[index]);
}
