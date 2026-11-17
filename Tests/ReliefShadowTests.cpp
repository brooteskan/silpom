// SPDX-License-Identifier: MIT
#include "BlendedFaceCpu.h"
#include <cstdlib>
#include <iostream>
using namespace SilPOM::Cpu;
namespace SilPOM::Cpu
{
inline float saturate(float value){return std::clamp(value,0.0f,1.0f);}
#define SILPOM_GRADIENT_CONSUMERS_CPU 1
#include "../Assets/Shaders/SilPOM/GradientConsumers.azsli"
#undef SILPOM_GRADIENT_CONSUMERS_CPU
}
void Check(bool value,const char* message)
{
    if(!value) { std::cerr<<message<<'\n'; std::exit(1); }
}
int main()
{
    Texture texture{16,16,std::vector<float>(256,0)};
    for(int y=0;y<16;++y) texture.pixels[y*16+8]=1;
    SilPomPatch p{};
    p.width=p.height=p.tileU=p.tileV=1;
    p.scale=.25f; p.reference=.5f; p.textureWidth=p.textureHeight=16;
    SilPomMeshFace f{};
    f.du={1,0,0}; f.dv={0,1,0}; f.normal=f.d0=f.d1=f.d2={0,0,1};
    f.uv1={1,0}; f.uv2={0,1};
    const float2 uv{.25f,.25f}, low{0,0};
    const auto light=normalize(float3{1,0,.2f});
    Check(SilPomReliefVisibility(texture,p,f,uv,low,light,16)==0,"Ridge must block grazing light");
    Check(SilPomReliefVisibility(texture,p,f,uv,low,{0,0,1},16)==1,"Overhead light must reach valley");
    texture.loads=0;
    Check(SilPomReliefVisibility(texture,p,f,uv,low,light,0)==1 && texture.loads==0,"Disabled shadows must not read texture");
    p.scale=0;
    Check(SilPomReliefVisibility(texture,p,f,uv,low,light,16)==1 && texture.loads==0,"Flat relief must not read texture");
    p.scale=.25f;
    Check(SilPomReliefVisibility(texture,p,f,uv,low,light,2)==1,"Local budget must bound shadow reach");
    Check(SilPomReliefVisibility(texture,p,f,{.95f,.25f},low,light,16)==0,"Repeat addressing must find wrapped ridge");
    p.addressMode=1;
    Check(SilPomReliefVisibility(texture,p,f,{.95f,.25f},low,light,16)==1,"Clamp must not wrap ridge");
    for(auto& height:texture.pixels) height=1-height;
    p.scale=-.25f;
    Check(SilPomReliefVisibility(texture,p,f,uv,low,light,16)==0,"Signed displacement must preserve equivalent ridge");
    texture.pixels.assign(256,.4f);
    Check(SilPomReliefVisibility(texture,p,f,uv,low,light,16)==1,"Constant height must not self-shadow");

    Texture stone{256,16,std::vector<float>(256*16,0)};
    for(int y=0;y<16;++y)
        for(int x=100;x<115;++x)
            stone.pixels[y*256+x]=1;
    p.textureWidth=256;
    p.scale=.03f;
    const auto grazing=normalize(float3{1,0,.12f});
    Check(SilPomReliefVisibility(stone,p,f,uv,low,grazing,18)==1,"Texel-limited trace cannot reach the stone");
    Check(SilPomReliefVisibility(stone,p,f,uv,low,grazing,18,.24f)==0,"Physical reach must catch the stone");
    Check(SilPomReliefVisibility(stone,p,f,uv,low,grazing,18,.10f)==1,"Physical reach must remain bounded");
    Check(SilPomReliefVisibility(stone,p,f,uv,low,{-grazing.x,0,grazing.z},18,.24f)==1,
        "The same receiver must be lit from the other direction");

    Check(std::abs(SilPomFlatReceiverAdvance(-.1f,.5f,.001f)-.202f)<1e-6f,"Recessed receiver must advance to base plane along light");
    Check(SilPomFlatReceiverAdvance(.1f,.5f,.001f)==0,"Raised receiver must not move");
    Check(SilPomFlatReceiverAdvance(-.1f,0,.001f)==0,"Parallel light must remain finite");
    const float3 receiverNormal=normalize(float3{.2f,-.3f,1.0f});
    const float3 receiverLight=normalize(float3{.7f,.4f,1.0f});
    const float3 receiverRate{.07f,-.11f,.03f};
    const float lightNormalDot=dot(receiverLight,receiverNormal);
    for(float receiverHeight:{-.2f,-.01f,.02f,.3f})
    {
        const float3 center=receiverNormal*receiverHeight;
        const auto adjusted=[&](float step)
        {
            const float3 position=center+receiverRate*step;
            return position+receiverLight*SilPomFlatReceiverAdvance(dot(position,receiverNormal),lightNormalDot,.001f);
        };
        const float3 numeric=(adjusted(.001f)-adjusted(-.001f))*500.0f;
        const float3 analytic=SilPomFlatReceiverDerivative(receiverRate,receiverNormal,receiverLight,
            receiverHeight,lightNormalDot,.001f);
        Check(length(numeric-analytic)<3e-5f,"Adjusted receiver gradient must differentiate the light-ray advance");
    }
    Check(length(SilPomFlatReceiverDerivative(receiverRate,receiverNormal,receiverLight,-.2f,0,.001f)-receiverRate)<1e-8f,
        "Parallel-light receiver continuation must remain finite");
    Check(SilPomSpecularAAFromDerivatives(.2f,{0,0,0},{0,0,0})==.2f,"Flat material normal must preserve roughness");
    Check(std::abs(SilPomSpecularAAFromDerivatives(.2f,{1,0,0},{0,1,0})-.38f)<1e-6f,
        "Owner-local normal variance must retain Atom's roughness kernel clamp");
    std::cout<<"Relief shadow occlusion, addressing, receiver differential and normal variance checks passed\n";
}
