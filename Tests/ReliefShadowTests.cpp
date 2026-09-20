// SPDX-License-Identifier: MIT
#include "BlendedFaceCpu.h"
#include <cstdlib>
#include <iostream>
using namespace SilPOM::Cpu;
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
    Check(std::abs(SilPomFlatReceiverAdvance(-.1f,.5f,.001f)-.202f)<1e-6f,"Recessed receiver must advance to base plane along light");
    Check(SilPomFlatReceiverAdvance(.1f,.5f,.001f)==0,"Raised receiver must not move");
    Check(SilPomFlatReceiverAdvance(-.1f,0,.001f)==0,"Parallel light must remain finite");
    std::cout<<"Relief shadow occlusion, addressing, signed height, budgets and receiver checks passed\n";
}
