// SPDX-License-Identifier: MIT
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
Texture2D heightImage : register(t0);
StructuredBuffer<SilPomRay> rays : register(t1);
RWStructuredBuffer<SilPomHit> hits : register(u0);
cbuffer Parameters : register(b0) { SilPomPatch patch; uint rayCount; }
#ifndef SILPOM_RT
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    if(id.x<rayCount) hits[id.x]=SilPomIntersect(heightImage,patch,rays[id.x]);
}
#else
RaytracingAccelerationStructure scene : register(t2);
struct Attributes { float3 position; float2 uv; float3 normal; };
struct Payload { SilPomHit hit; };
[shader("raygeneration")]
void RayGeneration()
{
    uint index=DispatchRaysIndex().x;
    SilPomRay input=rays[index];
    RayDesc ray;ray.Origin=input.origin;ray.Direction=input.direction;ray.TMin=input.tMin;ray.TMax=input.tMax;
    Payload payload=(Payload)0;
    TraceRay(scene,RAY_FLAG_NONE,255,0,1,0,ray,payload);
    hits[index]=payload.hit;
}
[shader("miss")]
void Miss(inout Payload payload) {payload.hit.status=SP_MISS;}
[shader("intersection")]
void Intersection()
{
    SilPomRay ray;ray.origin=ObjectRayOrigin();ray.direction=ObjectRayDirection();ray.tMin=RayTMin();ray.tMax=RayTCurrent();
    SilPomHit hit=SilPomIntersect(heightImage,patch,ray);
    if(hit.status==SP_HIT)
    {
        Attributes attributes;attributes.position=hit.position;attributes.uv=hit.uv;attributes.normal=hit.normal;
        ReportHit(hit.t,0,attributes);
    }
}
[shader("closesthit")]
void ClosestHit(inout Payload payload,Attributes attributes)
{
    payload.hit.status=SP_HIT;payload.hit.t=RayTCurrent();
    payload.hit.position=attributes.position;payload.hit.uv=attributes.uv;payload.hit.normal=attributes.normal;
}
#endif
