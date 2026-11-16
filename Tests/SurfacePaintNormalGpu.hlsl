// SPDX-License-Identifier: MIT
#include "../Assets/Shaders/SilPOM/ReceiverUv/SurfaceNormal.azsli"
Texture2D<float4> normalTexture:register(t0);
// Production mode 3 stores each mip in a separate image descriptor. A mip
// chosen from a per-pixel footprint must remain nonuniform within a wave.
Texture2D<float4> normalMipTextures[3]:register(t2);
SamplerState linearClamp:register(s0);
struct Query {float4 baseMode;float4 paint;float4 uvLod;};
struct Result {float4 normal;float4 paint;};
StructuredBuffer<Query> queries:register(t1);
RWStructuredBuffer<Result> results:register(u0);
cbuffer Params:register(b0){uint count;uint materialNullTextureIndex;}
namespace Bindless
{
    Texture2D<float4> GetTexture2D(uint index){return normalMipTextures[index];}
}
// Match the engine helper's signed null-resource fallback. DXC 1.8 loses an
// upstream nonuniform annotation across this select, despite the call-site
// marker. The production fix accesses validated descriptors directly instead.
Texture2D GetMaterialTexture(const int readIndex)
{
    int localReadIndex=readIndex;
    if(localReadIndex<0)localReadIndex=materialNullTextureIndex;
    return Bindless::GetTexture2D(localReadIndex);
}
#if defined(SILPOM_TEST_NULLABLE_MATERIAL_HELPER)
#define SP_MIP_TEXTURE(index) GetMaterialTexture(NonUniformResourceIndex(index))
#elif defined(SILPOM_TEST_OMIT_NONUNIFORM)
#define SP_MIP_TEXTURE(index) Bindless::GetTexture2D(index)
#else
#define SP_MIP_TEXTURE(index) Bindless::GetTexture2D(NonUniformResourceIndex(index))
#endif
[numthreads(64,1,1)]
void Main(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=count)return;
    const Query q=queries[id.x];
    float4 paint=q.paint;
    if(q.baseMode.w==1)
        paint=normalTexture.SampleLevel(linearClamp,q.uvLod.xy,q.uvLod.z);
    else if(q.baseMode.w==2)
    {
        const float lod=clamp(q.uvLod.z,0.0f,2.0f);
        const uint low=uint(floor(lod)),high=min(low+1u,2u);
        const float2 lowHalf=.5f/float(4u>>low),highHalf=.5f/float(4u>>high);
        const float4 first=SP_MIP_TEXTURE(low).SampleLevel(linearClamp,clamp(q.uvLod.xy,lowHalf,1-lowHalf),0);
        const float4 second=SP_MIP_TEXTURE(high).SampleLevel(linearClamp,clamp(q.uvLod.xy,highHalf,1-highHalf),0);
        paint=lerp(first,second,frac(lod));
    }
    Result result;result.normal=float4(SilPomComposeSurfaceNormal(q.baseMode.xyz,paint),1);
    result.paint=paint;results[id.x]=result;
}
