// SPDX-License-Identifier: MIT
#include "../Tools/LevelOneCpu.h"
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
namespace SilPOM::Cpu
{
#define SP_TEX const Texture&
#define SP_REF(T) T&
#define SILPOM_DIAGNOSTIC(event,value) Diagnostic(event,value)
#include "Reference/RefineFaceUncached.azsli"
#undef SILPOM_DIAGNOSTIC
#undef SP_REF
#undef SP_TEX
}
using namespace SilPOM::Cpu;
namespace
{
unsigned checks=0;
void Require(bool condition,const std::string& message)
{++checks;if(!condition)throw std::runtime_error(message);}
bool Same(float a,float b){return std::memcmp(&a,&b,sizeof(float))==0;}
bool Same(float3 a,float3 b){return Same(a.x,b.x)&&Same(a.y,b.y)&&Same(a.z,b.z);}
SilPomMeshFace Face()
{
    SilPomMeshFace f{};f.du={1,0,0};f.dv={0,1,0};f.normal=f.d0=f.d1=f.d2={0,0,1};
    f.uv1={1,0};f.uv2={0,1};return f;
}
void CellAcquisition()
{
    Texture texture{7,5,std::vector<float>(35)};
    for(unsigned i=0;i<35;++i)texture.pixels[i]=float(i)/35;
    SilPomPatch p{1,1,.2f,.5f,1,1,0,0,7,5,0,4096};
    for(unsigned mode:{0u,1u})
    {
        p.addressMode=mode;
        for(const float2 origin:{float2{.3f,.4f},float2{-.01f,.99f},float2{1.01f,-1.2f}})
        {
            SilPomRefineHeightCell cell{};
            const float2 points[]={origin,origin,origin+float2{1.f/7,0},origin,
                origin+float2{1,0},origin+float2{1,0}};
            std::uint64_t expected=0;
            int oldX=0,oldY=0;bool valid=false;
            for(auto uv:points)
            {
                const int x=int(floor(uv.x*7-.5f)),y=int(floor(uv.y*5-.5f));
                if(!valid||x!=oldX||y!=oldY)expected+=4;
                texture.loads=0;const auto reference=SilPomFaceHeight(texture,p,uv,{0,0});
                const auto referenceLoads=texture.loads;
                texture.loads=0;const auto actual=SilPomRefineFaceHeight(texture,p,uv,{0,0},cell);
                Require(Same(reference,actual),"A cached raw quad changed current height/derivatives");
                Require(referenceLoads==4,"The uncached height reference did not read a complete quad");
                Require(texture.loads==(!valid||x!=oldX||y!=oldY?4u:0u),"Raw quad acquisition did not follow the unaddressed cell");
                oldX=x;oldY=y;valid=true;
            }
            Require(expected==16,"Cell sequence did not exercise reload, return and full repeat invalidation");
        }
    }
    // One-texel images still use the unaddressed key. Distinct wrapped cells
    // must not be mistaken for one cached interpolation footprint.
    Texture one{1,1,{.37f}};p.textureWidth=p.textureHeight=1;
    SilPomRefineHeightCell cell{};
    SilPomRefineFaceHeight(one,p,{.1f,.1f},{0,0},cell);
    SilPomRefineFaceHeight(one,p,{1.1f,.1f},{0,0},cell);
    Require(one.loads==8,"One-texel wrapping incorrectly shared an addressed-cell cache");
}
void PerSolveOwnership()
{
    Texture first{4,4,std::vector<float>(16,.5f)},second{4,4,std::vector<float>(16,.7f)};
    SilPomPatch p{1,1,.1f,.5f,1,1,0,0,4,4,0,4096};auto face=Face();
    const SilPomRay ray{{.25f,.25f,1},{0,0,-1},0,2};
    for(unsigned sample=0;sample<4;++sample)
    {
        auto& texture=sample&1?second:first;texture.loads=0;
        float2 uv{.25f,.25f};float t=1.00001f;
        const auto status=SilPomRefineFace(texture,p,face,ray,{0,0},uv,t,4,1);
        Require(status==SP_HIT&&texture.loads==4,"A new candidate/sample reused another solve's height cache");
        Require(Same(t,sample&1?.98f:1.f),"A separate sample used another sample's height texels");
    }
}
void RepairedRecordedOwners()
{
    Texture textures[2]={{7,5,std::vector<float>(35)},{7,5,std::vector<float>(35)}};
    for(unsigned image=0;image<2;++image)
        for(unsigned i=0;i<35;++i)textures[image].pixels[i]=.2f+float((i*11+image*7)%35)/70;
    // Two final repair records lie at the repeat seam of different recorded
    // owners. Alternate their MSAA invocations and retry each solve. A cache
    // attached to a previous face/sample would reuse incompatible raw texels.
    for(unsigned sample=0;sample<8;++sample)
    {
        const unsigned owner=sample&1;auto& texture=textures[owner];
        SilPomPatch p{1,1,.15f,.4f,1,1,owner?.75f:.25f,.5f,7,5,0,4096};
        auto face=Face();face.origin={float(owner)*3,0,float(owner)*.1f};
        face.uv0={.5f,-.5f};face.uv1={1.5f,-.5f};face.uv2={.5f,.5f};
        face.d1={.08f*float(owner),0,1};face.d2={0,-.05f*float(owner),1};
        const float2 low{.5f,-.5f};
        SilPomHit recorded{};recorded.status=SP_HIT;
        recorded.faceUv={owner?.74999f:1.24999f,owner?-.00001f:-.25001f};
        const auto height=SilPomFaceHeight(texture,p,recorded.faceUv,low);
        const auto position=face.origin+face.du*(recorded.faceUv.x-face.uv0.x)
            +face.dv*(recorded.faceUv.y-face.uv0.y)+SilPomFaceDirection(face,recorded.faceUv)*height.x;
        const SilPomRay ray{position+float3{0,0,.2f},{0,0,-1},0,1};
        for(unsigned retry=0;retry<2;++retry)
        {
            auto oldUv=recorded.faceUv,newUv=oldUv;float oldT=.19999f,newT=oldT;
            texture.loads=0;levelOneStats={};
            const auto reference=SilPomRefineFaceUncachedReference(texture,p,face,ray,low,oldUv,oldT,8,1);
            const auto referenceLoads=texture.loads;const auto diagnostics=levelOneStats;
            texture.loads=0;levelOneStats={};
            const auto actual=SilPomRefineFace(texture,p,face,ray,low,newUv,newT,8,1);
            Require(actual==SP_HIT&&actual==reference&&Same(oldUv.x,newUv.x)&&Same(oldUv.y,newUv.y)&&Same(oldT,newT)
                &&diagnostics==levelOneStats,"Final repaired owner/UV changed refinement or acceptance");
            Require(texture.loads>=4&&texture.loads<=referenceLoads,
                "A repaired owner/sample/retry did not acquire its own initial raw quad");
        }
    }
}
void SolverEquivalence()
{
    std::mt19937 random(91273);std::uniform_real_distribution<float> unit(0,1),signedUnit(-1,1);
    std::uint64_t oldLoads=0,newLoads=0;unsigned hits=0,reloads=0;
    const unsigned widths[]={1,7,16,33},heights[]={1,5,17};
    const unsigned budgets[]={0,1,2,4,8,16};const float tolerances[]={.25f,1,5,30};
    for(unsigned query=0;query<16000;++query)
    {
        const auto width=widths[query%4],height=heights[(query/4)%3];
        Texture texture{width,height,std::vector<float>(width*height)};
        for(auto& value:texture.pixels)value=unit(random);
        SilPomPatch p{1,1,.01f+.3f*unit(random),unit(random),1,1,0,0,width,height,(query/12)%2,4096};
        p.tileU=(query&1?-1.f:1.f)*(.5f+unit(random));p.tileV=(query&2?-1.f:1.f)*(.5f+unit(random));
        p.offsetU=signedUnit(random)*3;p.offsetV=signedUnit(random)*3;
        auto face=Face();face.du={.5f+unit(random)*2,.1f*signedUnit(random),.1f*signedUnit(random)};
        face.dv={.1f*signedUnit(random),.5f+unit(random)*2,.1f*signedUnit(random)};
        face.normal=normalize(cross(face.du,face.dv));face.d0=face.normal;
        face.d1=face.normal+float3{.2f*signedUnit(random),0,.1f*signedUnit(random)};
        face.d2=face.normal+float3{0,.2f*signedUnit(random),.1f*signedUnit(random)};
        // Final recorded UVs include repaired/neighbor-owned coordinates and
        // edge/outside cases; no lighting/normal cache participates.
        const float2 target{-.15f+unit(random)*1.3f,-.15f+unit(random)*1.3f};
        const auto h=SilPomFaceHeight(texture,p,target,{0,0});
        const auto position=face.du*target.x+face.dv*target.y+SilPomFaceDirection(face,target)*h.x;
        const auto direction=normalize(float3{.2f*signedUnit(random),.2f*signedUnit(random),-1});
        SilPomRay ray{position-direction*.2f,direction,0,query%19==0?.15f:1.f};
        float2 oldUv=target+float2{signedUnit(random)*.03f,signedUnit(random)*.03f},newUv=oldUv;
        float oldT=.18f,newT=oldT;const auto budget=budgets[query%6];const auto tolerance=tolerances[(query/6)%4];
        texture.loads=0;levelOneStats={};
        const auto oldStatus=SilPomRefineFaceUncachedReference(texture,p,face,ray,{0,0},oldUv,oldT,budget,tolerance);
        const auto originalLoads=texture.loads;const auto stats=levelOneStats;
        texture.loads=0;levelOneStats={};
        const auto newStatus=SilPomRefineFace(texture,p,face,ray,{0,0},newUv,newT,budget,tolerance);
        const auto prefix="Newton query "+std::to_string(query)+": ";
        Require(oldStatus==newStatus&&Same(oldUv.x,newUv.x)&&Same(oldUv.y,newUv.y)&&Same(oldT,newT),prefix+"status/UV/t changed");
        Require(stats==levelOneStats,prefix+"solver diagnostics/acceptance changed");
        Require(texture.loads<=originalLoads,prefix+"cache increased quad fetches");
        Require(originalLoads==stats[0]*4,prefix+"uncached oracle no longer evaluates a quad per iteration");
        oldLoads+=originalLoads;newLoads+=texture.loads;hits+=newStatus==SP_HIT;reloads+=texture.loads>4;
    }
    Require(oldLoads>newLoads&&reloads>0&&hits>100,"Equivalence fixtures did not exercise reuse, cell changes and accepted hits");
    std::cout<<"16000 exact Newton comparisons; "<<hits<<" hits; height loads "<<oldLoads<<" -> "<<newLoads<<"\n";
}
}
int main()
{
    try{CellAcquisition();PerSolveOwnership();RepairedRecordedOwners();SolverEquivalence();std::cout<<checks<<" refinement height-cache checks passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
