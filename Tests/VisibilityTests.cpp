// SPDX-License-Identifier: MIT
#include "../Tools/VisibilityCpu.h"
#include "../Tools/visibility_reference.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace SilPOM::Cpu;
static void Check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main()
{
    try
    {
        uint cells=0;
        const float saddle[]={0,1,1,0};
        using SilPOM::VisibilityReference::Trace;
        Check(Trace(saddle,2,2,0,0,1,1,0,0,.4,1,0,1,true,cells)==0,
            "Continuous reference must catch an interior quadratic maximum");
        Check(Trace(saddle,2,2,0,0,1,1,0,0,.6,1,0,1,true,cells)==1,
            "A ray above the quadratic maximum must be lit");
        Check(Trace(saddle,2,2,0,0,1,1,0,0,.5,1,0,1,true,cells)==-1,
            "A tangent must be marked uncertain");
        const float inverse[]={1,0,0,1};
        Check(Trace(inverse,2,2,0,0,1,1,0,0,.4,-1,1,1,true,cells)==0,
            "Reference must support equivalent signed displacement");
        Texture image{32,16,std::vector<float>(512,.5f)};
        SilPomPatch p{1,1,.2f,.5f,1,1,0,0,32,16,0,SP_TOWER_NO_MARCH};
        SilPomMeshFace f{};f.du={1,0,0};f.dv={0,1,0};f.normal=f.d0=f.d1=f.d2={0,0,1};
        f.uv1={1,0};f.uv2={0,1};
        auto evaluate=[&](float2 uv,float3 light,uint steps,bool neural)
        {
            visibilityStats={};image.loads=0;
            return SilPomLearnedReliefVisibility(image,p,f,uv,{0,0},light,steps,.3f,4,neural,.995f);
        };
        Check(evaluate({.2f,.2f},{1,0,.01f},0,true)==1 && image.loads==0,"Disabled shadows must not read heights");
        p.scale=0;
        Check(evaluate({.2f,.2f},{1,0,.01f},64,true)==1 && image.loads==0,"Flat relief must not read heights");
        p.scale=.2f;
        std::array<float,145> forced{};visibilityOverride=forced.data();
        Check(evaluate({.2f,.2f},{1,0,.01f},64,true)==1 && visibilityStats[5]==1
            && visibilityStats[0]==65,"Uncertain fallback must skip all four already-tested positions");
        forced[144]=20;
        Check(evaluate({.2f,.2f},{1,0,.01f},64,true)==1 && visibilityStats[3]==1
            && visibilityStats[0]==5,"Accepted prediction must use only the start and four probes");
        forced[144]=-20;
        Check(evaluate({.2f,.2f},{1,0,.01f},64,true)==0 && visibilityStats[4]==1,"Shadow prediction branch must execute");
        forced[144]=0;
        std::mt19937 rng(51619);std::uniform_real_distribution<float> coord(-.5f,1.5f),unit(0,1);
        for(auto& value:image.pixels)value=unit(rng);
        uint comparisons=0;
        for(uint address:{0u,1u}) for(float scale:{-.2f,.2f})
        for(uint i=0;i<5000;++i)
        {
            p.addressMode=address;p.scale=scale;
            float2 uv{coord(rng),coord(rng)};float3 light=normalize({coord(rng),coord(rng),unit(rng)});
            uint steps=i%65;float reach=i%2?.3f:0;
            float baseline=SilPomReliefVisibility(image,p,f,uv,{0,0},light,steps,reach);
            visibilityStats={};
            float gather=SilPomLearnedReliefVisibility(image,p,f,uv,{0,0},light,steps,reach,4,false,.995f);
            Check(gather==baseline,"Gather marcher must preserve baseline visibility");
            visibilityStats={};
            float fallback=SilPomLearnedReliefVisibility(image,p,f,uv,{0,0},light,steps,reach,4,true,.995f);
            Check(fallback==baseline,"Uncertain neural fallback must preserve baseline visibility");
            Check(visibilityStats[0]<=steps+1,"Fallback must not duplicate tested positions");
            ++comparisons;
        }
        visibilityOverride=nullptr;
        std::cout<<comparisons<<" signed/addressing/budget comparisons and continuous-reference checks passed.\n";
        return 0;
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
