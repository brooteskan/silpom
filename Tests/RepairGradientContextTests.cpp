// SPDX-License-Identifier: MIT
#include "GradientContextTestData.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace SilPOM::GradientCpu
{
#include "../Assets/Shaders/SilPOM/RepairGradientContext.azsli"
}
using namespace SilPOM::GradientCpu;
namespace
{
std::size_t checks=0;
void Check(bool valid,const char* message){++checks;if(!valid)throw std::runtime_error(message);}
void Unchanged(SilPomGradientContext a,SilPomGradientContext b,const char* message)
{Check(std::memcmp(&a,&b,sizeof(a))==0,message);}
void Coarse(SilPomGradientContext a,uint representation)
{
    Check(a.validity==SP_GRADIENT_COARSE,"Amplified repair must be coarse");
    Check(a.uv.x==1&&a.uv.y==0&&a.uv.z==0&&a.uv.w==1,"Coarse repair has the prescribed unit source footprint");
    Check(a.depth.x==0&&a.depth.y==0,"Coarse repair has constant recorded-depth continuation");
    Check(a.representation==representation,"Conditioning preserves base and repair provenance");
}
}
int main()
{
    try
    {
        for(float z:{1e-12f,1e-8f})
        {
            auto donor=[&](float pixelX,float depthV)
            {
                return SilPomGradientFromTangents({pixelX*2/1920,0,z,1},
                    {float(2/(1920*.05)),0,0,0},{0,float(-2/(1080*.01)),depthV,0},
                    {1920,1080},{1,1},SP_GRADIENT_RELIEF);
            };
            const auto a=donor(-1,1e-4f),b=donor(1,-1e-4f);
            Check(a.validity==SP_GRADIENT_VALID&&b.validity==SP_GRADIENT_VALID,"Counterexample donors are physically conditioned mappings");
            const auto raw=SilPomGradientBlend({.1f,.2f},z,a,{.2f,.3f},z,b,.5f,{.5f,0},true,SP_GRADIENT_REPAIR_DEPTH_WEIGHTED);
            Check(raw.validity==SP_GRADIENT_VALID&&abs(raw.uv.z)>1,"Counterexample exercises rational amplification");
            Coarse(SilPomGradientConditionRepair(raw),raw.representation);
        }
        // One continuous saddle relief chart near a finite far plane.
        // X=(du*(u-.15),dv*(v-.2),Z+400*(u-.15)*(v-.2)); D=(0,0,1).
        const float z=.2f/(.001f+1e-8f),du=z/(48*1.2f),dv=-z/(5.4f*1.6f);
        const float clipZ=-.001f*z+.2f,depth=clipZ/z;
        auto physical=[&](float sign)
        {
            const float hv=sign*20;
            return SilPomGradientFromTangents({sign*z/960,0,clipZ,z},
                {1.2f*du,0,0,0},{0,1.6f*dv,-.001f*hv,hv},
                {1920,1080},{1,1},SP_GRADIENT_RELIEF);
        };
        const auto a=physical(-1),b=physical(1);
        Check(a.validity==1&&b.validity==1,"Finite-far saddle donors stay valid");
        const auto physicalRepair=SilPomGradientBlend({.1f,.2f},depth,a,{.2f,.2f},depth,b,.5f,{.5f,0},true,SP_GRADIENT_REPAIR_DEPTH_WEIGHTED);
        Check(physicalRepair.validity==1&&abs(physicalRepair.uv.z)>1,"Continuous relief chart reproduces amplification");
        Coarse(SilPomGradientConditionRepair(physicalRepair),physicalRepair.representation);
        std::size_t regular=0,coarse=0,amplified=0;
        for(const auto& input:MakeGradientCases(8192))
        {
            const auto output=EvaluateGradientCase(input);
            const auto context=SilPomGradientUnpack(output.repairUv,output.repairDepth);
            if(context.validity==SP_GRADIENT_VALID&&abs(context.uv.x)<=1&&abs(context.uv.y)<=1
                &&abs(context.uv.z)<=1&&abs(context.uv.w)<=1&&abs(context.depth.x)<=1&&abs(context.depth.y)<=1)
            {Unchanged(context,SilPomGradientConditionRepair(context),"Regular repairs remain bitexact");++regular;}
            if(context.validity==SP_GRADIENT_COARSE)
            {Unchanged(context,SilPomGradientConditionRepair(context),"Existing coarse repairs remain bitexact");++coarse;}
            else if(abs(context.uv.x)>1||abs(context.uv.y)>1||abs(context.uv.z)>1||abs(context.uv.w)>1
                ||abs(context.depth.x)>1||abs(context.depth.y)>1)
            {Coarse(SilPomGradientConditionRepair(context),context.representation);++amplified;}
        }
        Check(regular>7000&&coarse>0,"Regular/coarse regression coverage");
        SilPomGradientContext threshold{{1,-1,1,-1},{1,-1},SP_GRADIENT_BASE|SP_GRADIENT_REPAIR_ARITHMETIC,SP_GRADIENT_VALID};
        Unchanged(threshold,SilPomGradientConditionRepair(threshold),"Inclusive unit bound retains its endpoint");
        auto tooLarge=threshold;tooLarge.depth.x=1.00001f;Coarse(SilPomGradientConditionRepair(tooLarge),threshold.representation);
        tooLarge=threshold;tooLarge.uv.w=std::numeric_limits<float>::quiet_NaN();Coarse(SilPomGradientConditionRepair(tooLarge),threshold.representation);
        const SilPomGradientContext empty{};Unchanged(empty,SilPomGradientConditionRepair(empty),"Empty context remains zero for unowned sidecar clearing");
        std::cout<<checks<<" repair conditioning checks passed; "<<regular<<" regular and "<<coarse
            <<" existing coarse contexts unchanged; "<<amplified<<" amplified random repairs and physical far-plane counterexamples select coarse.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
