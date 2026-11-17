// SPDX-License-Identifier: MIT
#include "FaceCoefficientTestData.h"
#include <iostream>
using namespace SilPOM::Cpu;
namespace
{
size_t checks=0;
void Check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
bool SameFloat(float a,float b){return std::memcmp(&a,&b,4)==0||(std::isnan(a)&&std::isnan(b));}
void Same(float a,float b,const char* message){Check(SameFloat(a,b),message);}
void Same(float3 a,float3 b,const char* message){Same(a.x,b.x,message);Same(a.y,b.y,message);Same(a.z,b.z,message);}
}
int main(int argc,char** argv)
{
    try
    {
        auto faces=MakeCoefficientFaces();const auto synthetic=faces.size();
        if(argc==2)AppendPackedCoefficientFaces(faces,argv[1]);
        Texture texture{4,4,{.2f,.6f,.3f,.8f,.7f,.4f,.9f,.1f,.3f,.8f,.2f,.6f,.9f,.1f,.7f,.4f}};
        size_t newtonCases=0,gradientCases=0;
        for(const auto& packed:faces)
        {
            auto f=LoadCoefficientFace(packed);const auto c=SilPomPrepareFaceCoefficients(f);
            SilPomSetFaceCoefficients(f,c);Same(SilPomUvArea(f),LegacyFaceUvArea(f),"Raw signed area changed");
            const float area=LegacyFaceUvArea(f);
            float3 du,dv;float tolerance;
            const bool cachedNewton=SilPomTryFaceNewtonCoefficients(f,du,dv,tolerance);
            if(cachedNewton)
            {
                const float3 oldDu=((f.d1-f.d0)*(f.uv2.y-f.uv0.y)-(f.d2-f.d0)*(f.uv1.y-f.uv0.y))*(1/area);
                const float3 oldDv=((f.d2-f.d0)*(f.uv1.x-f.uv0.x)-(f.d1-f.d0)*(f.uv2.x-f.uv0.x))*(1/area);
                Same(du,oldDu,"Newton Du changed");Same(dv,oldDv,"Newton Dv changed");
                Same(tolerance,2e-6f*max(1.f,max(length(f.du),length(f.dv))),"Base tolerance changed");
                for(float multiplier:{.25f,1.f,5.f})Same(tolerance*multiplier,2e-6f*max(1.f,max(length(f.du),length(f.dv)))*multiplier,"Tolerance application order changed");
            }
            float3 gdU,gdV,legacyGu,legacyGv;
            const bool gradient=SilPomFaceGradientDirectionPartials(f,gdU,gdV);
            const bool oldGradient=SilPomGradientDirectionPartials(f.uv0,f.uv1,f.uv2,f.d0,f.d1,f.d2,legacyGu,legacyGv);
            Check(gradient==oldGradient,"Gradient guard changed");
            if(gradient){Same(gdU,legacyGu,"Gradient Du changed");Same(gdV,legacyGv,"Gradient Dv changed");++gradientCases;}
            if(abs(area)>1e-9f)
            {
                const float2 uv{(f.uv0.x+f.uv1.x+f.uv2.x)/3,(f.uv0.y+f.uv1.y+f.uv2.y)/3};
                Same(SilPomFaceWeights(f,uv),LegacyFaceWeights(f,uv),"Barycentric division changed");
                Same(SilPomFaceDirection(f,uv),LegacyFaceDirection(f,uv),"Affine unnormalized direction interpolation changed");
                const float2 low{min(f.uv0.x,min(f.uv1.x,f.uv2.x)),min(f.uv0.y,min(f.uv1.y,f.uv2.y))};
                const float2 high{max(f.uv0.x,max(f.uv1.x,f.uv2.x)),max(f.uv0.y,max(f.uv1.y,f.uv2.y))};
                SilPomPatch p{high.x-low.x,high.y-low.y,.08f,.5f,high.x-low.x,high.y-low.y,low.x,low.y,4,4,0,4096};
                const auto h=SilPomFaceHeight(texture,p,uv,low);
                const auto point=f.origin+f.du*(uv.x-f.uv0.x)+f.dv*(uv.y-f.uv0.y)+LegacyFaceDirection(f,uv)*h.x;
                SilPomRay ray{point+f.normal*.2f,f.normal*(-1),0,1};
                for(float multiplier:{.25f,1.f,5.f})
                {
                    float2 a{uv.x+.001f,uv.y-.001f},b=a;float ta=.18f,tb=ta;
                    const auto sa=LegacyRefineFace(texture,p,f,ray,low,a,ta,8,multiplier);
                    const auto sb=SilPomRefineFace(texture,p,f,ray,low,b,tb,8,multiplier);
                    Check(sa==sb,"Newton status changed");Same(a.x,b.x,"Newton UVu changed");Same(a.y,b.y,"Newton UVv changed");Same(ta,tb,"Newton depth changed");++newtonCases;
                }
            }
            // Unavailable/wrong-version cache must execute the original path.
            auto absent=SilPomEmptyFaceCoefficients();SilPomSetFaceCoefficients(f,absent);
            Same(SilPomUvArea(f),LegacyFaceUvArea(f),"Missing cache area fallback changed");
            auto wrong=c;wrong.metadata.y=999;SilPomSetFaceCoefficients(f,wrong);
            Check(!SilPomTryFaceNewtonCoefficients(f,du,dv,tolerance),"Unsupported coefficient version was consumed");
        }
        auto tiny=LoadCoefficientFace(faces[synthetic-6]);auto c=SilPomPrepareFaceCoefficients(tiny);
        Check((c.metadata.x&SP_FACE_COEFFICIENT_NEWTON)!=0&&(c.metadata.x&SP_FACE_COEFFICIENT_GRADIENT)==0,"Tiny area must preserve different Newton and gradient validity policies");
        std::cout<<checks<<" coefficient checks passed across "<<faces.size()<<" canonical packed faces ("<<faces.size()-synthetic
            <<" frozen input records), "<<newtonCases<<" Newton comparisons and "<<gradientCases<<" gradient comparisons.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
