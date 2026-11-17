// SPDX-License-Identifier: MIT
#include "GradientContextTestData.h"
#include <iostream>
#include <stdexcept>
namespace SilPOM::GradientCpu
{
inline float dot(float3 a,float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline float saturate(float a){return std::clamp(a,0.f,1.f);}
#define SILPOM_GRADIENT_CONSUMERS_CPU 1
#include "../Assets/Shaders/SilPOM/GradientConsumers.azsli"
#undef SILPOM_GRADIENT_CONSUMERS_CPU
#include "../Assets/Shaders/SilPOM/NormalQueryPolicy.azsli"
}
using namespace SilPOM::GradientCpu;
namespace
{
std::size_t checks=0;
void Check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
void Near(float a,float b,const char* message){Check(std::abs(a-b)<2e-7f,message);}
float3 Unit(float3 a){return a*(1/std::sqrt(dot(a,a)));}
}
int main()
{
    try
    {
        // One authored plane approaches its projective horizon. The center
        // inverse Jacobian is .8 source UV/pixel; the +quarter-pixel probe is
        // 3.2 and therefore selects the actual shared coarse mapping policy.
        const float4 du{1,0,0,1920},dv{0,-768.f/54,0,0};
        const auto center=SilPomGradientFromTangents({0,0,.1f,768},du,dv,{1920,1080},{1,1},SP_GRADIENT_PROXY);
        const auto offset=SilPomGradientFromTangents({.4f,0,.1f,1536},du,dv,{1920,1080},{1,1},SP_GRADIENT_PROXY);
        Check(center.validity==SP_GRADIENT_VALID,"Fixture center must be conditioned");
        Near(center.uv.x,.8f,"Fixture center has the intended physical footprint");
        Check(offset.validity==SP_GRADIENT_COARSE,"Actual quarter-pixel plane probe must cross conditioning boundary");
        // A constant finite decoded normal makes the observed differences zero.
        // The mapping's coarse flag, rather than NaN detection, must force AA.
        const float3 normal{0,0,1},zero=normal-normal;
        Check(SilPomGradientFinite3(normal),"Boundary fixture returns a finite normal");
        const bool oneProbeCoarse=offset.validity!=SP_GRADIENT_VALID;
        Check(SilPomNormalQueryShouldCoarse(center.validity,oneProbeCoarse,false),"One coarse offset survives a valid center");
        Near(SilPomNormalQueryRoughness(.2f,true,center.validity,oneProbeCoarse,false,zero,zero),.38f,
            "Finite coarse probe uses maximum bounded AA, not zero measured variance");
        Near(SilPomNormalQueryRoughness(.2f,true,center.validity,false,false,zero,zero),.2f,"Regular constant normal has no variance");
        for(uint validity:{SP_GRADIENT_INVALID,SP_GRADIENT_VALID,SP_GRADIENT_COARSE})
        for(bool probe:{false,true})for(bool appearance:{false,true})
        {
            Near(SilPomNormalQueryRoughness(.2f,false,validity,probe,appearance,{9,9,9},{9,9,9}),.2f,
                "Disabled AA never adds roughness, including coarse mappings");
            Check(!SilPomNormalQueryNeedsProbes(false,true,validity),"Disabled AA never schedules normal probes");
        }
        Check(SilPomNormalQueryNeedsProbes(true,true,SP_GRADIENT_VALID),"Conditioned lit sample requests probes");
        Check(!SilPomNormalQueryNeedsProbes(true,false,SP_GRADIENT_VALID),"Diagnostic sample skips probes");
        Check(!SilPomNormalQueryNeedsProbes(true,true,SP_GRADIENT_COARSE),"Coarse source skips unusable probes");
        Check(!SilPomNormalQueryNeedsProbes(true,true,SP_GRADIENT_INVALID),"Invalid source skips probes");
        Near(SilPomNormalQueryRoughness(.2f,true,SP_GRADIENT_VALID,false,true,zero,zero),.38f,"Coarse center appearance forces maximum AA");
        Near(SilPomNormalQueryRoughness(.2f,true,SP_GRADIENT_COARSE,false,false,zero,zero),.38f,"Coarse source forces maximum AA");
        Near(SilPomNormalQueryRoughness(.95f,true,SP_GRADIENT_VALID,true,false,zero,zero),1,"Coarse maximum saturates roughness");
        const float3 dx{.1f,-.03f,.07f},dy{-.02f,.08f,.01f};
        Near(SilPomNormalQueryRoughness(.2f,true,SP_GRADIENT_VALID,false,false,dx,dy),
            SilPomSpecularAAFromDerivatives(.2f,dx,dy),"Regular policy retains Atom's variance scale and clamp");
        // Normal maps off still leave curvature in the geometric normal field.
        auto acquire=[&](bool applyAA,bool normalMapEnabled)
        {
            unsigned queries=0;float3 nx{},ny{};
            if(SilPomNormalQueryNeedsProbes(applyAA,true,SP_GRADIENT_VALID))
            {
                auto evaluate=[&](float u,float v)
                {++queries;return Unit({u+(normalMapEnabled?.1f:0),v,1});};
                nx=(evaluate(.05f,0)-evaluate(-.05f,0))*2;
                ny=(evaluate(0,.025f)-evaluate(0,-.025f))*2;
            }
            return std::pair<unsigned,float>{queries,SilPomNormalQueryRoughness(.2f,applyAA,SP_GRADIENT_VALID,false,false,nx,ny)};
        };
        const auto geometryOnly=acquire(true,false),disabled=acquire(false,false);
        Check(geometryOnly.first==4&&geometryOnly.second>.2f,"Material normal maps off still queries geometric-normal curvature");
        Check(disabled.first==0&&disabled.second==.2f,"AA off performs no queries and no extra AA");
        std::cout<<checks<<" normal-query policy checks passed; valid-center/coarse-quarter-pixel finite-normal boundary selects maximum AA; AA-off skips probes and roughness; geometric-normal-only stencil remains enabled.\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
