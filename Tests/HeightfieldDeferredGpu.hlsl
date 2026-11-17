// SPDX-License-Identifier: MIT
// Production and frozen eager traversals run in the same invocation. This is
// numerical/fetch-count conformance, not a timing or production bounds builder.
static uint heightLoads;
float FetchHeight(Texture2D image,int x,int y)
{++heightLoads;return image.Load(int3(x,y,0)).r;}
float2 FetchBounds(Texture2D image,int blockX,int blockY);
#define SILPOM_CPU 1
#define SILPOM_MESH_DEPTH_BOUNDS 1
#define SP_REF(T) inout T
#define SP_TEX Texture2D
#define SP_FETCH(texture,x,y) FetchHeight(texture,x,y)
#define SP_BOUND_TEX Texture2D
#define SP_FETCH_BOUND(texture,x,y) FetchBounds(texture,x,y)
#define SP_ZERO(T) (T)0
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
#define SilPomIntersectMode SilPomIntersectModeEagerReference
#include "Reference/HeightfieldTraversalEager.azsli"
#undef SilPomIntersectMode
Texture2D heightImage:register(t0);
StructuredBuffer<SilPomRay> rays:register(t1);
RWStructuredBuffer<SilPomHit> hits:register(u0);
cbuffer Parameters:register(b0){SilPomPatch patch;uint rayCount;}
float2 FetchBounds(Texture2D image,int blockX,int blockY)
{
    float low=1,high=0;
    [unroll] for(int y=0;y<=4;++y)[unroll] for(int x=0;x<=4;++x)
    {
        const int ix=SilPomAddress(blockX*4+x,int(patch.textureWidth),patch.addressMode);
        const int iy=SilPomAddress(blockY*4+y,int(patch.textureHeight),patch.addressMode);
        // Bounds acquisition is separate from the raw leaf-load counter.
        const float height=image.Load(int3(ix,iy,0)).r;
        low=min(low,height);high=max(high,height);
    }
    return float2(low,high);
}
bool Same(SilPomHit a,SilPomHit b)
{
    return a.status==b.status&&a.cells==b.cells&&asuint(a.t)==asuint(b.t)
        &&all(asuint(a.position)==asuint(b.position))&&all(asuint(a.uv)==asuint(b.uv))
        &&all(asuint(a.normal)==asuint(b.normal));
}
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=rayCount)return;
    heightLoads=0;
    SilPomHit expected=SilPomIntersectModeEagerReference(heightImage,patch,rays[id.x],true,heightImage,true);
    uint expectedLoads=heightLoads;
    heightLoads=0;
    SilPomHit actual=SilPomIntersectMode(heightImage,patch,rays[id.x],true,heightImage,true);
    if(!Same(actual,expected)||heightLoads>expectedLoads)actual.status=SP_INVALID;
    hits[id.x]=actual;
}
