// SPDX-License-Identifier: MIT
#include "CpuShader.h"
#include <iostream>
#include <random>
#include <cstdlib>
namespace SilPOM::Cpu {
#define SP_REF(T) T&
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
#undef SP_REF
}
using namespace SilPOM::Cpu;
void Require(bool value,const char* message) { if(!value) { std::cerr<<message<<'\n'; std::exit(1); } }
int main()
{
    Texture texture{2,2,{.75f,.75f,.75f,.75f}};
    SilPomPatch p{1,1,1,.5f,1,1,0,0,2,2,0,4096};
    std::mt19937 random(5); std::uniform_real_distribution<float> coord(-1,2);
    for(int i=0;i<20000;++i)
    {
        SilPomRay ray{{coord(random),coord(random),1},{coord(random),coord(random),-1},0,2};
        const float x=ray.origin.x+.75f*ray.direction.x,y=ray.origin.y+.75f*ray.direction.y;
        if(std::min({std::abs(x),std::abs(y),std::abs(x+y-1)})<1e-4f) continue;
        const bool expected=x>=0&&y>=0&&x+y<=1;
        float begin,end;
        const float2 a{0,0},b=i%2?float2{1,0}:float2{0,1},c=i%2?float2{0,1}:float2{1,0};
        SilPomHit hit{};
        if(SilPomFaceInterval(ray,a,b,c,begin,end))
        {
            ray.tMin=begin;ray.tMax=end;ray.origin.x-=.5f;ray.origin.y-=.5f;
            hit=SilPomIntersect(texture,p,ray);
        }
        Require((hit.status==SP_HIT)==expected,"Triangle-clipped planar hit disagrees with independent plane/barycentric oracle");
        if(expected) Require(std::abs(hit.t-.75f)<1e-5f,"Wrong finite triangle hit depth");
        Require(hit.status!=SP_EXHAUSTED&&hit.status!=SP_INVALID,"Unexpected traversal failure");
    }
    float begin,end;
    SilPomRay outside{{.8f,.8f,1},{0,0,-1},0,2};
    Require(!SilPomFaceInterval(outside,{0,0},{1,0},{0,1},begin,end),"Bounding rectangle must not fill outside-triangle area");
    SilPomRay edge{{.5f,.5f,1},{0,0,-1},0,2};
    Require(SilPomFaceInterval(edge,{0,0},{1,0},{0,1},begin,end),"Shared diagonal must retain coverage");
    Require(!SilPomFaceInterval(edge,{0,0},{1,0},{2,0},begin,end),"Degenerate UVs must be rejected");
    std::cout<<"PlanarFace: 20000 analytical rays, both UV windings, exterior/shared edges passed\n";
}
