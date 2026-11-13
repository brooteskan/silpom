// SPDX-License-Identifier: MIT
#include "../Tools/LevelOneCpu.h"
#include <iostream>
#include <stdexcept>
using namespace SilPOM::Cpu;
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main()
{
    try
    {
        Texture texture{64,4,std::vector<float>(256)};
        for(uint y=0;y<4;++y) for(uint x=0;x<64;++x)
            texture.pixels[y*64+x]=(x%16u<8u)?.9f:.1f;
        SilPomPatch p{1,1,.2f,.5f,1,1,0,0,64,4,1,2048};
        SilPomMeshFace f;
        f.origin={0,0,0};f.du={1,0,0};f.dv={0,1,0};f.normal={0,0,1};
        f.uv0={0,0};f.uv1={1,0};f.uv2={0,1};
        f.d0=f.d1=f.d2={0,0,1};
        SilPomRay world{{-.05f,.1f,.06f},{1,0,-.05f},0,.9f};
        SilPomRay planar=world;planar.origin.x-=.5f;planar.origin.y-=.5f;
        SilPomHit nearest=SilPomIntersect(texture,p,planar);
        Require(nearest.status==SP_HIT,"Reference must cross the first ridge");
        planar.tMin=nearest.t+.02f;
        SilPomHit later=SilPomIntersect(texture,p,planar);
        Require(later.status==SP_HIT && later.t>nearest.t+.02f,"Fixture needs distinct roots");
        later.faceUv=XY(world.origin+world.direction*later.t);
        const auto recovered=SilPomRecoverNearestHit(texture,p,f,world,world,{0,0},later,4);
        Require(recovered.status==SP_HIT && abs(recovered.t-nearest.t)<2e-5f,
            "A validated later root must not hide the nearest ridge");
        const auto missing=SilPomRecoverNearestHit(texture,p,f,world,world,{0,0},{},4);
        Require(missing.status==SP_HIT && abs(missing.t-nearest.t)<2e-5f,
            "A rejected neural prediction must recover the first ridge");
        for(uint address:{0u,1u}) for(float tile:{-1.0f,1.0f}) for(int motion=-8;motion<=8;++motion)
        {
            SilPomPatch movedPatch=p;movedPatch.addressMode=address;movedPatch.tileU=tile;movedPatch.offsetU=.31f;
            SilPomRay moved=world;moved.origin.x+=float(motion)*.0001f;
            SilPomRay reference=moved;reference.origin.x-=.5f;reference.origin.y-=.5f;
            const auto exact=SilPomIntersect(texture,movedPatch,reference);
            const auto recoveredMotion=SilPomRecoverNearestHit(texture,movedPatch,f,moved,moved,{0,0},{},4);
            Require(recoveredMotion.status==exact.status && (exact.status!=SP_HIT
                || abs(recoveredMotion.t-exact.t)<2e-5f),
                "Small camera motion, signed tiling and texture addressing must retain the near ridge");
        }
        for(uint model:{1u,5u,6u,7u})
        {
            const auto loads=texture.loads;
            const auto unchanged=SilPomRecoverNearestHit(texture,p,f,world,world,{0,0},later,model);
            Require(unchanged.t==later.t && texture.loads==loads,"Other profiles must have no recovery cost");
        }
        SilPomRay frontal=world;frontal.direction={0,0,-1};
        auto loads=texture.loads;
        Require(SilPomRecoverNearestHit(texture,p,f,frontal,frontal,{0,0},later,4).t==later.t
            && texture.loads==loads,"Face-on rays must skip all recovery texture reads");
        SilPomRay outside=world;outside.origin.y=2;
        Require(SilPomRecoverNearestHit(texture,p,f,outside,outside,{0,0},{},4).status==SP_MISS,
            "Recovery must never publish outside-triangle seed geometry");
        // The affine-direction validator must transport the actual recovered
        // surface, rather than publish the depth of its planar approximation.
        f.d1={.2f,0,1};f.d2={0,.1f,1};
        const auto affine=SilPomRecoverNearestHit(texture,p,f,world,world,{0,0},{},4);
        Require(affine.status==SP_HIT,"Affine displacement must retain recovery");
        const float h=SilPomFaceHeight(texture,p,affine.faceUv,{0,0}).x;
        const auto actual=f.origin+f.du*affine.faceUv.x+f.dv*affine.faceUv.y
            +SilPomFaceDirection(f,affine.faceUv)*h;
        Require(length(actual-(world.origin+world.direction*affine.t))<3e-6f,
            "Recovered depth must satisfy the actual affine-direction surface");
        Texture wide{4096,4,std::vector<float>(4096*4,.5f)};
        SilPomPatch widePatch=p;widePatch.textureWidth=4096;
        SilPomRecoverNearestHit(wide,widePatch,f,world,world,{0,0},{},4);
        Require(wide.loads<=(129u+4u*8u)*4u,
            "Long grazing queries must respect the probe and validation read budgets");
        std::cout<<"Nearest ridge, missed prediction, affine validation and zero-work gates passed.\n";
        return 0;
    }
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
