// SPDX-License-Identifier: MIT
// Run through test_heightfield_fetches.py; its temporary include is extracted
// from the actual production traversal, with no copied optimized algorithm.
#include "CpuShader.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace SilPOM::Cpu
{
struct FetchBoundsTexture
{
    uint width,height;
    std::vector<float2> pixels;
    mutable std::uint64_t loads=0;
    float2 Load(int x,int y) const {++loads;return pixels[y*width+x];}
};
inline int clamp(int value,int low,int high) {return min(max(value,low),high);}
#define SILPOM_MESH_DEPTH_BOUNDS 1
#define SP_TEX const Texture&
#define SP_BOUND_TEX const FetchBoundsTexture&
#define SP_FETCH_BOUND(texture,x,y) texture.Load(x,y)
#define SP_ZERO(T) T{}
#define SilPomIntersectMode SilPomIntersectModeCurrentBounds
#include "HeightfieldFetchCurrent.azsli"
#undef SilPomIntersectMode
#define SilPomIntersectMode SilPomIntersectModeEagerReference
#include "Reference/HeightfieldTraversalEager.azsli"
#undef SilPomIntersectMode
#undef SP_ZERO
#undef SP_FETCH_BOUND
#undef SP_BOUND_TEX
#undef SP_TEX
#undef SILPOM_MESH_DEPTH_BOUNDS

// Compare default, constant-divisor, generic-remainder and old double-modulo
// policies without changing coordinate construction or neighbor-tap selection.
#undef SILPOM_FAST_ADDRESSING
#define SILPOM_FAST_ADDRESSING 0
#define SilPomAddress SilPomAddressDoubleModulo
#include "HeightfieldAddressCurrent.azsli"
#undef SilPomAddress
#undef SILPOM_FAST_ADDRESSING
#define SILPOM_FAST_ADDRESSING 1
#undef SILPOM_GENERIC_REMAINDER
#define SILPOM_GENERIC_REMAINDER 1
#define SilPomAddress SilPomAddressGenericRemainder
#include "HeightfieldAddressCurrent.azsli"
#undef SilPomAddress
#undef SILPOM_GENERIC_REMAINDER
#undef SILPOM_FAST_ADDRESSING
}

using namespace SilPOM::Cpu;
namespace
{
std::uint64_t checks=0,queries=0,oldLoads=0,newLoads=0,strictReductions=0;
void Require(bool condition,const char* message)
{++checks;if(!condition)throw std::runtime_error(message);}
bool Same(float a,float b) {return std::memcmp(&a,&b,sizeof(float))==0;}
bool Same(const SilPomHit& a,const SilPomHit& b)
{
    return a.status==b.status&&a.cells==b.cells&&Same(a.t,b.t)
        &&Same(a.position.x,b.position.x)&&Same(a.position.y,b.position.y)&&Same(a.position.z,b.position.z)
        &&Same(a.uv.x,b.uv.x)&&Same(a.uv.y,b.uv.y)
        &&Same(a.normal.x,b.normal.x)&&Same(a.normal.y,b.normal.y)&&Same(a.normal.z,b.normal.z);
}
FetchBoundsTexture Bounds(const Texture& texture,uint mode)
{
    const uint width=(texture.width+3)/4,height=(texture.height+3)/4;
    FetchBoundsTexture result{width,height,std::vector<float2>(width*height)};
    for(uint y=0;y<height;++y)for(uint x=0;x<width;++x)
    {
        float low=1,high=0;
        // A 4x4-cell block contains five rows/columns of corner texels.
        for(uint v=0;v<=4;++v)for(uint u=0;u<=4;++u)
        {
            const auto ix=SilPomAddress(int(x*4+u),int(texture.width),mode);
            const auto iy=SilPomAddress(int(y*4+v),int(texture.height),mode);
            const float value=texture.pixels[iy*texture.width+ix];
            low=min(low,value);high=max(high,value);
        }
        result.pixels[y*width+x]={low,high};
    }
    return result;
}
void Compare(const Texture& texture,const FetchBoundsTexture& bounds,SilPomPatch p,SilPomRay ray,bool reuse,bool blocks)
{
    texture.loads=0;bounds.loads=0;
    const auto expected=SilPomIntersectModeEagerReference(texture,p,ray,reuse,bounds,blocks);
    const auto oldHeight=texture.loads,oldBounds=bounds.loads;
    texture.loads=0;bounds.loads=0;
    const auto actual=SilPomIntersectModeCurrentBounds(texture,p,ray,reuse,bounds,blocks);
    if(!Same(actual,expected))
        throw std::runtime_error("Legacy traversal fields changed at query "+std::to_string(queries)
            +" (blocks="+std::to_string(blocks)+", reuse="+std::to_string(reuse)+")");
    Require(bounds.loads==oldBounds,"Deferred leaves changed block-bound queries");
    Require(texture.loads<=oldHeight,"Deferred traversal increased leaf fetches");
    oldLoads+=oldHeight;newLoads+=texture.loads;strictReductions+=texture.loads<oldHeight;++queries;
}
void Addresses()
{
#if defined(SILPOM_MSAA_FAST_ADDRESSING)
#error This proof must exercise the default policy without the old MSAA define.
#endif
    std::mt19937 random(61837);
    const int sizes[]={1,2,3,7,511,512,513,6144,6656,8192,16384,65535};
    const int extremes[]={std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),-1,0,1};
    for(int size:sizes)for(uint query=0;query<200000;++query)
    {
        int coordinate=query<5?extremes[query]:static_cast<int>(random());
        if(query>=5&&query<152)
            coordinate=(int(query/7)-10)*size+int(query%7)-3;
        const std::int64_t dividend=coordinate,divisor=size;
        std::int64_t quotient=dividend/divisor;
        if(dividend<0&&dividend%divisor)--quotient;
        const int expected=int(dividend-quotient*divisor);
        const int expectedClamp=std::clamp(coordinate,0,size-1);
        Require(SilPomAddress(coordinate,size,0)==expected,"Default NPOT repeat differs from floor division");
        Require(SilPomAddressDoubleModulo(coordinate,size,0)==expected,"Reference repeat differs from floor division");
        Require(SilPomAddressGenericRemainder(coordinate,size,0)==expected,"Generic remainder differs from floor division");
        Require(SilPomAddress(coordinate,size,1)==expectedClamp
            &&SilPomAddressDoubleModulo(coordinate,size,1)==expectedClamp
            &&SilPomAddressGenericRemainder(coordinate,size,1)==expectedClamp,"Clamp addressing changed");
    }
}
void RejectedBlocks()
{
    Texture texture{16,16,std::vector<float>(256,.5f)};
    auto bounds=Bounds(texture,1);
    SilPomPatch patch{2,2,.2f,.5f,1,1,0,0,16,16,1,4096};
    for(float direction:{-1.f,1.f})for(float start:{-.99f,0.f,.99f})
    {
        const SilPomRay ray{{start,0,.09f},{direction,0,0},0,10};
        Compare(texture,bounds,patch,ray,true,true);
        Require(texture.loads==0,"A fully rejected bound interval acquired a leaf quad");
    }
    patch.maxCells=1;
    Compare(texture,bounds,patch,{{-.99f,0,.09f},{1,0,0},0,10},true,true);
    Require(texture.loads==0,"An exhausted block skip acquired an unused destination quad");
}
void Rays()
{
    std::mt19937 random(179031);
    std::uniform_real_distribution<float> unit(0,1),signedUnit(-1,1);
    const uint widths[]={1,7,8,13,16,32},heights[]={1,5,8,17,16,32};
    for(uint fixture=0;fixture<24;++fixture)
    {
        const auto width=widths[fixture%6],height=heights[(fixture/6+fixture)%6];
        Texture texture{width,height,std::vector<float>(width*height)};
        for(uint y=0;y<height;++y)for(uint x=0;x<width;++x)
            texture.pixels[y*width+x]=fixture%3==0?(.1f+.2f*float(((x/4)+(y/4))%4)):
                fixture%3==1?unit(random):(.35f+.24f*std::sin(float(x)*.11f)*std::cos(float(y)*.13f));
        for(uint mode=0;mode<2;++mode)
        {
            const auto bounds=Bounds(texture,mode);
            const bool blockSupported=mode!=0||((width&(width-1))==0&&(height&(height-1))==0);
            for(uint query=0;query<900;++query)
            {
                SilPomPatch patch{2,2,query%4==0?-.2f:.2f,.5f,
                    (query%2?1.f:-1.f)*(.5f+unit(random)*2),
                    (query%3?1.f:-1.f)*(.5f+unit(random)*2),signedUnit(random)*2,signedUnit(random)*2,
                    width,height,mode,query%5==0?1u+query%64:4096u};
                SilPomRay ray{{signedUnit(random)*1.4f,signedUnit(random)*1.4f,signedUnit(random)*.25f},
                    normalize({signedUnit(random),signedUnit(random),signedUnit(random)*.5f}),query%7==0?.2f:0.f,10};
                if(query%17==0)ray.direction={query%2?-1.f:1.f,0,0};
                if(query%19==0)ray.direction={0,query%2?-1.f:1.f,0};
                if(query%23==0)ray.direction={0,0,query%2?-1.f:1.f};
                for(bool reuse:{false,true})
                {
                    Compare(texture,bounds,patch,ray,reuse,false);
                    if(blockSupported)Compare(texture,bounds,patch,ray,reuse,true);
                }
            }
        }
    }
    Require(strictReductions>100,"Fixtures did not exercise deferred leaf rejection/unused final leaves");
    Require(newLoads<oldLoads,"Deferred leaves did not reduce fetches");
}
}
int main()
{
    try
    {
        Addresses();RejectedBlocks();Rays();
        std::cout<<checks<<" checks passed; "<<queries<<" frozen/current legacy rays match all fields bit-for-bit; "
            <<oldLoads<<" -> "<<newLoads<<" raw leaf fetches ("<<strictReductions<<" lower-fetch queries).\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
