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
#define SP_XY(value) XY(value)
#define SP_SAMPLE_HEIGHT(texture,uv) SampleHeight(texture,uv)
#define SP_BRANCH
#define SP_LOOP
#include "Reference/NearestHit.azsli"
#undef SP_LOOP
#undef SP_BRANCH
#undef SP_SAMPLE_HEIGHT
#undef SP_XY
#undef SP_TEX
}
using namespace SilPOM::Cpu;
bool BitsEqual(float a,float b) {return std::memcmp(&a,&b,sizeof(float))==0;}
int main()
{
    try
    {
        std::mt19937 rng(381923);
        std::uniform_real_distribution<float> unit(0,1),signedUnit(-1,1);
        for(uint query=0;query<6000u;++query)
        {
            const uint widths[]={1,7,32,65},heights[]={1,5,31};
            const uint width=widths[query%4],height=heights[(query/4)%3];
            Texture texture{width,height,std::vector<float>(width*height)};
            for(auto& value:texture.pixels) value=unit(rng);
            SilPomPatch p{1,1,.2f,.5f,1,1,0,0,width,height,(query/12)%2,SP_TOWER_NO_MARCH};
            p.tileU=(query%2?1.0f:-1.0f)*(.5f+unit(rng));p.tileV=.5f+unit(rng);
            p.offsetU=signedUnit(rng)*2;p.offsetV=signedUnit(rng)*2;
            SilPomMeshFace face;
            face.origin={0,0,0};face.du={1,0,0};face.dv={0,1,0};face.normal={0,0,1};
            face.uv0={0,0};face.uv1={1,0};face.uv2={0,1};
            face.d0={0,0,1};face.d1={.2f,0,1};face.d2={0,.1f,1};
            SilPomRay ray{{signedUnit(rng)*.8f+.5f,unit(rng),signedUnit(rng)*.1f},
                normalize({signedUnit(rng),signedUnit(rng),-.01f-unit(rng)*.28f}),0,4};
            SilPomHit predicted{};
            if(query%3u==0u)
            {
                SilPomRay projected=ray;
                if(SilPomMakeSeedRay(face,(face.d0+face.d1+face.d2)/3.0f,ray,projected))
                    predicted=SilPomNoMarchCandidate(texture,p,face,ray,projected,{0,0},unit(rng),4u,5.0f);
            }
            texture.loads=0;levelOneStats={};
            const auto reference=SilPomRecoverNearestHitReference(texture,p,face,ray,ray,{0,0},predicted,4);
            const auto loads=texture.loads;const auto stats=levelOneStats;
            texture.loads=0;levelOneStats={};
            const auto actual=SilPomRecoverNearestHit(texture,p,face,ray,ray,{0,0},predicted,4);
            if(actual.status!=reference.status || actual.cells!=reference.cells
                || !BitsEqual(actual.t,reference.t)
                || !BitsEqual(actual.faceUv.x,reference.faceUv.x) || !BitsEqual(actual.faceUv.y,reference.faceUv.y)
                || !BitsEqual(actual.uv.x,reference.uv.x) || !BitsEqual(actual.uv.y,reference.uv.y)
                || !BitsEqual(actual.position.x,reference.position.x)
                || !BitsEqual(actual.position.y,reference.position.y)
                || !BitsEqual(actual.position.z,reference.position.z) || stats!=levelOneStats)
                throw std::runtime_error("Approved reference changed at query "+std::to_string(query));
            // At most three prefetched probes remain after an early return.
            if(texture.loads>loads+12u) throw std::runtime_error("Unbounded speculative acquisition");
        }
        std::cout<<"6000 recovery queries match the approved reference bit-for-bit; prefetch is bounded.\n";
        return 0;
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
