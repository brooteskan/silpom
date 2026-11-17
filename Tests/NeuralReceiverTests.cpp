// SPDX-License-Identifier: MIT
#include "../Tools/VisibilityCpu.h"
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace SilPOM::Cpu;
namespace
{
uint checks=0;
void Require(bool condition,const char* message)
{
    ++checks;
    if(!condition)throw std::runtime_error(message);
}
bool Same(float a,float b)
{
    uint x,y;std::memcpy(&x,&a,sizeof(x));std::memcpy(&y,&b,sizeof(y));return x==y;
}
void SameRay(const SilPomLocalShadowRay& a,const SilPomLocalShadowRay& b)
{
    Require(Same(a.uRate,b.uRate)&&Same(a.vRate,b.vRate)&&Same(a.hRate,b.hRate)
        &&Same(a.start,b.start)&&Same(a.top,b.top)&&Same(a.bias,b.bias)
        &&Same(a.step,b.step)&&Same(a.extra,b.extra)&&Same(a.reach,b.reach)&&a.count==b.count,
        "Receiver sharing changed the original per-light ray arithmetic");
}
SilPomMeshFace Face()
{
    SilPomMeshFace face{};face.du={1,0,0};face.dv={0,1,0};
    face.normal=face.d0=face.d1=face.d2={0,0,1};face.uv1={1,0};face.uv2={0,1};return face;
}
SilPomPatch Patch()
{
    return {1,1,.2f,.5f,1,1,0,0,32,16,0,SP_TOWER_NO_MARCH};
}
void EarlyExits()
{
    Texture image{32,16,std::vector<float>(512,.5f)};
    auto patch=Patch();auto face=Face();SilPomNeuralPhysicalReceiver receiver;
    SilPomInitNeuralPhysicalReceiver(receiver);
    SilPomLocalShadowRay ray{};float immediate=0;
    visibilityStats={};
    Require(!SilPomPrepareNeuralLocalShadow(image,patch,face,{.2f,.3f},{0,0},{1,0,.01f},0,.3f,receiver,ray,immediate)
        &&immediate==1&&!receiver.framePrepared&&!receiver.heightPrepared&&visibilityStats[0]==0,
        "Disabled queries acquired receiver work");
    patch.scale=1e-9f;
    Require(!SilPomPrepareNeuralLocalShadow(image,patch,face,{.2f,.3f},{0,0},{1,0,.01f},9,.3f,receiver,ray,immediate)
        &&immediate==1&&!receiver.framePrepared&&!receiver.heightPrepared&&visibilityStats[0]==0,
        "Negligible relief acquired receiver work");
    patch=Patch();face.du={0,0,0};
    Require(!SilPomPrepareNeuralLocalShadow(image,patch,face,{.2f,.3f},{0,0},{1,0,.01f},9,.3f,receiver,ray,immediate)
        &&immediate==1&&receiver.framePrepared&&!receiver.frameValid&&!receiver.heightPrepared&&visibilityStats[0]==0,
        "A degenerate receiver was not retained as a height-free analytic exit");
    face=Face();SilPomInitNeuralPhysicalReceiver(receiver);
    Require(!SilPomPrepareNeuralLocalShadow(image,patch,face,{.2f,.3f},{0,0},{1,0,-.01f},9,.3f,receiver,ray,immediate)
        &&immediate==0&&receiver.framePrepared&&receiver.frameValid&&!receiver.heightPrepared&&visibilityStats[0]==0,
        "Downward light acquired the center height before its exit");
    Require(!SilPomPrepareNeuralLocalShadow(image,patch,face,{.2f,.3f},{0,0},{0,0,1},9,.3f,receiver,ray,immediate)
        &&immediate==1&&!receiver.heightPrepared&&visibilityStats[0]==0,
        "Zero lateral rate acquired a center height");
    Require(SilPomPrepareNeuralLocalShadow(image,patch,face,{.2f,.3f},{0,0},{1,0,.01f},9,.3f,receiver,ray,immediate)
        &&receiver.heightPrepared&&visibilityStats[0]==1,
        "The first qualifying light did not acquire exactly one center height");
    Require(SilPomPrepareNeuralLocalShadow(image,patch,face,{.2f,.3f},{0,0},{0,1,.01f},32,.1f,receiver,ray,immediate)
        &&visibilityStats[0]==1,"Subsequent lights repeated the center height acquisition");
}
void LightSpecificRays()
{
    Texture image{32,16,std::vector<float>(512,.5f)};
    std::mt19937 random(73409);std::uniform_real_distribution<float> unit(0,1),coord(-.5f,1.5f);
    for(auto& value:image.pixels)value=unit(random);
    const uint budgets[]={5,9,16,32,64};
    for(uint center=0;center<400;++center)
    {
        auto patch=Patch();patch.scale=.01f+unit(random)*.3f;patch.reference=unit(random);
        patch.addressMode=center&1;patch.tileU=.5f+unit(random)*2;patch.tileV=.5f+unit(random)*2;
        auto face=Face();face.du={.5f+unit(random)*2,.2f*unit(random),.1f*unit(random)};
        face.dv={.2f*unit(random),.5f+unit(random)*2,.1f*unit(random)};
        face.d1={.2f*unit(random),0,1};face.d2={0,.2f*unit(random),1};
        const float2 uv{coord(random),coord(random)};
        SilPomNeuralPhysicalReceiver receiver;SilPomInitNeuralPhysicalReceiver(receiver);
        for(uint light=0;light<12;++light)
        {
            const float3 direction=normalize({coord(random),coord(random),coord(random)});
            const uint steps=budgets[(light+center)%5];const float reach=(light&1)?.3f:0;
            SilPomLocalShadowRay actual{},reference{};float result=0,expected=0;
            const bool wasHeightPrepared=receiver.heightPrepared;
            visibilityStats={};
            const bool active=SilPomPrepareNeuralLocalShadow(image,patch,face,uv,{0,0},direction,steps,reach,receiver,actual,result);
            const auto reads=visibilityStats[0];
            const bool expectedActive=SilPomPrepareLocalShadow(image,patch,face,uv,{0,0},direction,steps,reach,true,reference,expected);
            Require(active==expectedActive&&Same(result,expected),"Sharing changed a direction-dependent analytic exit");
            if(active)SameRay(actual,reference);
            Require(reads==(wasHeightPrepared?0u:receiver.heightPrepared?1u:0u),
                "A center cached a light-specific probe or repeated height acquisition");
        }
    }
}
void SameEstimatorAndBoundedProbes()
{
    Texture image{32,16,std::vector<float>(512,.5f)};
    auto patch=Patch();const auto face=Face();const float2 uv{.2f,.3f};
    std::array<float,145> weights{};visibilityOverride=weights.data();
    const float3 lights[]={{1,0,.01f},{0,1,.01f},{-.5f,1,.01f}};
    for(float logit:{-20.f,0.f,20.f})
    {
        weights[144]=logit;
        SilPomNeuralPhysicalReceiver cached,uncached;
        SilPomInitNeuralPhysicalReceiver(cached);SilPomInitNeuralPhysicalReceiver(uncached);
        uncached.cacheAvailable=false;
        for(uint light=0;light<3;++light)
        {
            visibilityStats={};
            const float actual=SilPomNeuralOnlyVisibilityPrepared(image,patch,face,uv,{0,0},lights[light],9,.3f,4,.6f,cached);
            const auto cachedReads=visibilityStats[0];
            Require(visibilityStats[5]==0&&visibilityStats[6]==0,"A cached strict neural query invoked marcher recovery");
            visibilityStats={};
            const float withoutCache=SilPomNeuralOnlyVisibilityPrepared(image,patch,face,uv,{0,0},lights[light],9,.3f,4,.6f,uncached);
            Require(visibilityStats[0]==5&&visibilityStats[5]==0&&visibilityStats[6]==0,
                "Unavailable cache changed the fixed descriptor acquisition or selected a marcher");
            visibilityStats={};
            const float expected=SilPomNeuralOnlyVisibility(image,patch,face,uv,{0,0},lights[light],9,.3f,4,.6f);
            Require(Same(actual,expected)&&Same(withoutCache,expected),"Cache availability changed the selected neural estimator");
            Require(cachedReads==(light==0?5u:4u),"Fixed probes were cached across lights or center work was repeated");
        }
        Require(!uncached.framePrepared&&!uncached.heightPrepared,"Uncached evaluation published center-cache state");
    }
    // Every sample invocation owns a fresh receiver, even at the same UV.
    SilPomNeuralPhysicalReceiver anotherSample;SilPomInitNeuralPhysicalReceiver(anotherSample);visibilityStats={};
    SilPomNeuralOnlyVisibilityPrepared(image,patch,face,uv,{0,0},lights[0],9,.3f,4,.6f,anotherSample);
    Require(visibilityStats[0]==5,"A different sample inherited another invocation's center cache");
    visibilityOverride=nullptr;
}
}
int main()
{
    try
    {
        EarlyExits();LightSpecificRays();SameEstimatorAndBoundedProbes();
        std::cout<<checks<<" neural receiver checks passed: lazy acquisition, exact per-light rays, fixed probes and uncached strict decisions\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
