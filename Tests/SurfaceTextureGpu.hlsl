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
// Match the pinned engine's BaseColorInput working-space conversion. This
// adapter supplies the engine function used by the real production include.
float3 LinearSrgb_To_AcesCg(float3 color)
{
    const float3x3 xyzToAp1=float3x3(1.64102352,-.32480335,-.23642471,
        -.66366309,1.61533189,.01675635,.01172191,-.00828444,.98839492);
    const float3x3 d65ToD60=float3x3(1.01303,.00610531,-.014971,
        .00769823,.998165,-.00503203,-.00284131,.00468516,.924507);
    const float3x3 srgbToXyz=float3x3(.41239089,.35758430,.18048084,
        .21263906,.71516860,.07219233,.01933082,.11919472,.95053232);
    return mul(mul(xyzToAp1,mul(d65ToD60,srgbToXyz)),color);
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
