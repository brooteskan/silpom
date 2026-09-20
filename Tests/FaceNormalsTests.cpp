// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/FaceNormals.h"
#include "CpuShader.h"
#include <iostream>
#include <cstdlib>
namespace SilPOM::Cpu {
#define SP_REF(T) T&
#define SP_TEX const Texture&
#define SP_ZERO(T) T{}
#include "../Assets/Shaders/SilPOM/PlanarFace.azsli"
#include "../Assets/Shaders/SilPOM/BlendedFace.azsli"
#undef SP_REF
#undef SP_TEX
#undef SP_ZERO
}
using namespace SilPOM::FaceNormals;
void Require(bool b,const char* m) { if(!b) {std::cerr<<m<<'\n';std::exit(1);} }
int main()
{
    // Two quads with deliberately different diagonals, a material/UV seam
    // represented by duplicate corners sharing only logical vertex identities.
    std::vector<Face> faces{
        {{{0,1,2}},{{{0,0,0},{1,0,0},{1,1,0}}},true},
        {{{0,2,3}},{{{0,0,0},{1,1,0},{0,1,0}}},true},
        {{{1,4,2}},{{{1,0,0},{2,0,.6},{1,1,0}}},true},
        {{{4,5,2}},{{{2,0,.6},{2,1,.6},{1,1,0}}},true}};
    auto smooth=Build(faces);Require(smooth.error.empty(),smooth.error.c_str());
    auto same=[](Vector a,Vector b){return (a-b).Length()<1e-10;};
    Require(same(smooth.directions[0][1],smooth.directions[2][0]),"UV/material split broke shared normal");
    Require(same(smooth.directions[0][2],smooth.directions[3][2]),"Different quad diagonals changed endpoint normal");
    Vector expected=(Vector{0,0,1}+Vector{-.6,0,1}.Unit()).Unit();
    Require(same(smooth.directions[0][1],expected),"Angle weighting depends on quad triangulation");
    // A tagged/ordinary boundary must not contribute ordinary normals.
    faces[2].tagged=faces[3].tagged=false;
    auto hard=Build(faces);Require(hard.error.empty(),hard.error.c_str());
    Require(same(hard.directions[0][1],{0,0,1}),"Ordinary face leaked into tagged fan");
    // Two tagged fans sharing only a vertex remain separate.
    faces.push_back({{{1,6,7}},{{{1,0,0},{1,0,1},{1,-1,0}}},true});
    auto split=Build(faces);Require(split.error.empty(),split.error.c_str());
    Require(same(split.directions[0][1],{0,0,1}),"Vertex-only contact blended unrelated fans");
    // A third face sharing an edge must fail instead of guessing adjacency.
    faces.push_back(faces[0]);Require(!Build(faces).error.empty(),"Nonmanifold edge accepted");

    using namespace SilPOM::Cpu;
    auto vec=[](Vector v){return float3(float(v.x),float(v.y),float(v.z));};
    SilPomMeshFace a{};a.origin={0,0,0};a.du={1,0,0};a.dv={0,1,0};a.normal={0,0,1};
    a.uv0={0,0};a.uv1={1,0};a.uv2={1,1};
    a.d0=vec(smooth.directions[0][0]);a.d1=vec(smooth.directions[0][1]);a.d2=vec(smooth.directions[0][2]);
    SilPomMeshFace b{};b.origin={1,0,0};b.du={1,0,.6f};b.dv={0,1,0};b.normal=normalize(float3(-.6f,0,1));
    b.uv0={1,0};b.uv1={2,0};b.uv2={1,1};
    b.d0=vec(smooth.directions[2][0]);b.d1=vec(smooth.directions[2][1]);b.d2=vec(smooth.directions[2][2]);
    for(int i=0;i<=100;++i)
    {
        float2 uv{1,i/100.f};float height=.12f*std::sin(i*.21f);
        float3 left=a.origin+a.du*uv.x+a.dv*uv.y+SilPomFaceDirection(a,uv)*height;
        float3 right=b.origin+b.dv*uv.y+SilPomFaceDirection(b,uv)*height;
        Require(length(left-right)<1e-6f,"Blended displacement opened a shared edge");
    }
    // Constant height makes the displaced surface an exact plane, even with
    // varying directions. Compare with an independent displaced-triangle hit.
    Texture texture{2,2,{.75f,.75f,.75f,.75f}};
    SilPomPatch p{1,1,.4f,.5f,1,1,0,0,2,2,0,4096};
    for(int i=1;i<30;++i)
    {
        float2 uv{1,i/30.f};
        float3 point=a.du*uv.x+a.dv*uv.y+SilPomFaceDirection(a,uv)*.1f;
        SilPomRay ray{point+float3(0,0,2),{0,0,-1},0,4};
        auto left=SilPomIntersectFace(texture,p,a,ray,{0,0});
        auto right=SilPomIntersectFace(texture,p,b,ray,{1,0});
        Require(left.status==SP_HIT && right.status==SP_HIT,"Ray intersection opened a shared fold edge");
        Require(std::abs(left.t-right.t)<1e-5f,"Shared fold hits have different depths");
        Require(length(left.normal-right.normal)<1e-5f,"Flat-height lighting normal jumps across tagged fold");
    }
    for(int y=1;y<30;++y) for(int x=y;x<30;++x)
    {
        float2 uv{x/30.f,y/30.f};
        float3 point=a.du*uv.x+a.dv*uv.y+SilPomFaceDirection(a,uv)*.1f;
        for(float slope:{0.f,1.f,5.f})
        {
            float3 direction=normalize(float3(slope,.2f,-1));
            SilPomRay ray{point-direction*2,direction,0,4};
            auto hit=SilPomIntersectFace(texture,p,a,ray,{0,0});
            Require(hit.status==SP_HIT,"Missed analytically known blended-face hit");
            Require(std::abs(hit.t-2)<2e-5f,"Blended-face depth differs from analytic plane");
        }
    }
    // Varying height: construct rays through known surface points. Vertical
    // rays on this shallow fixture have one intersection (no root ambiguity).
    Texture waves{16,16,{}};
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) waves.pixels.push_back(.5f+.2f*std::sin(x*.6f)*std::cos(y*.6f));
    p.textureWidth=p.textureHeight=16;
    for(int y=1;y<25;++y) for(int x=y+1;x<30;++x)
    {
        float2 uv{x/31.f,y/31.f};float height=SilPomFaceHeight(waves,p,uv,{0,0}).x;
        float3 point=a.du*uv.x+a.dv*uv.y+SilPomFaceDirection(a,uv)*height;
        for(float slope:{0.f,1.f,5.f})
        {
            float3 direction=normalize(float3(slope,.1f,-1));
            SilPomRay ray{point-direction*2,direction,0,4};
            auto hit=SilPomIntersectFace(waves,p,a,ray,{0,0});
            Require(hit.status==SP_HIT && hit.t<=2+2e-5f,"Missed a known heightfield hit or returned a later root");
            if(slope==0) Require(std::abs(hit.t-2)<2e-5f,"Blended heightfield ray residual failed");
        }
    }
    std::cout<<"Tagged fans, hard boundaries, UV splits, weighting, shared displaced edges and analytic rays passed\n";
}
