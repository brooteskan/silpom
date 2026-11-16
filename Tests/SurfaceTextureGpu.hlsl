// SPDX-License-Identifier: MIT
// Compile the production texture-table traversal with ordinary HLSL bindings.
// The application integration supplies equivalent bindless resources and frame.
ByteAddressBuffer chartTable:register(t0);
Texture2D<float4> paintImages[64]:register(t1);
namespace Bindless
{
    ByteAddressBuffer GetByteAddressBuffer(uint index){return chartTable;}
    Texture2D<float4> GetTexture2D(uint index){return paintImages[index];}
}
namespace PassSrg {SamplerState LinearSampler:register(s0);}
cbuffer NullTexture:register(b0) {int nullTextureIndex;}
// Match SceneMaterialSrg.azsli exactly: this fallback can discard an upstream
// NonUniformResourceIndex marker, so the production paint path must bypass it.
Texture2D<float4> GetMaterialTexture(const int readIndex)
{
    int localReadIndex=readIndex;
    if(localReadIndex<0)localReadIndex=nullTextureIndex;
    return Bindless::GetTexture2D(localReadIndex);
}
struct MaterialParameters {uint m_receiverPaintAtlas,m_silpomHitFaceBase;};
struct Surface {float3 albedo,normal;};
struct Face {float3 normal,du,dv;};
static Face silpomLitFace;
static float2 silpomLitUv;
static float3x3 silpomLitRotation;
#include "../Assets/Shaders/SilPOM/ReceiverUv/SurfaceTexture.azsli"
struct Input {float4 position:SV_Position;float2 uv:TEXCOORD0;};
struct Output {float4 color:SV_Target0;float4 normal:SV_Target1;};
Output MainPS(Input input,bool front:SV_IsFrontFace)
{
    silpomLitFace.normal=float3(0,0,1);silpomLitFace.du=float3(1,0,0);silpomLitFace.dv=float3(0,1,0);
    silpomLitRotation=float3x3(1,0,0,0,1,0,0,0,1);silpomLitUv=input.uv;
    MaterialParameters params;params.m_receiverPaintAtlas=0;params.m_silpomHitFaceBase=1;
    Surface surface;surface.albedo=float3(.2,.3,.4);surface.normal=normalize(float3(.1,.2,front?1:-1));
    SilPomApplySurfaceTextures(surface,params,0,front);
    Output output;output.color=float4(surface.albedo,1);output.normal=float4(surface.normal,1);return output;
}
