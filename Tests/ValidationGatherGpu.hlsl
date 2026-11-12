// SPDX-License-Identifier: MIT
SamplerState testSampler : register(s0);
#define GetMaterialTextureSampler() testSampler
#define SILPOM_VALIDATION_GATHER true
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
Texture2D heightImage : register(t0);
StructuredBuffer<float2> coordinates : register(t1);
struct Result { float3 loads; float3 gather; };
RWStructuredBuffer<Result> results : register(u0);
cbuffer Parameters : register(b0) { SilPomPatch patch; uint count; }
[numthreads(64,1,1)]
void Main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= count) return;
    Result result;
    result.loads = SilPomFaceHeight(heightImage, patch, coordinates[id.x], float2(0,0));
    result.gather = SilPomGatherFaceHeight(heightImage, patch, coordinates[id.x], float2(0,0));
    results[id.x] = result;
}
