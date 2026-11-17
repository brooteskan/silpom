// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/SurfaceBrushShape.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace SilPOM::SurfaceBrushShape;
namespace
{
void Require(bool pass,const char* message)
{if(!pass){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
bool Near(float a,float b,float tolerance=2.e-6f){return std::abs(a-b)<=tolerance;}
constexpr std::array<Shape,6> Shapes{{Shape::Texture,Shape::HardRound,Shape::SoftRound,
    Shape::HardSquare,Shape::SoftSquare,Shape::RandomEdgeRound}};
constexpr std::array<uint64_t,5> Seeds{{0,1,0x100000000ull,0x123456789abcdef0ull,UINT64_MAX}};
constexpr float Pi=3.14159265358979323846f;
float Polar(Shape shape,float radius,float angle,uint64_t seed=0)
{return Coverage(shape,.5f+.5f*radius*std::cos(angle),.5f+.5f*radius*std::sin(angle),seed);}
}

int main()
{
    static_assert(static_cast<uint32_t>(Shape::Texture)==0&&static_cast<uint32_t>(Shape::HardRound)==1
        &&static_cast<uint32_t>(Shape::SoftRound)==2&&static_cast<uint32_t>(Shape::HardSquare)==3
        &&static_cast<uint32_t>(Shape::SoftSquare)==4&&static_cast<uint32_t>(Shape::RandomEdgeRound)==5);
    for(Shape shape:Shapes)
    {
        Require(Coverage(shape,.5f,.5f)==1.f,"Every brush must paint its center fully");
        for(float invalid:{-.001f,1.001f,std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::max()})
        {
            Require(Coverage(shape,invalid,.5f)==0.f&&Coverage(shape,.5f,invalid)==0.f,
                "Invalid or out-of-domain brush UVs must have no coverage");
        }
        for(uint64_t seed:Seeds)
            for(unsigned y=0;y<=128;++y)
                for(unsigned x=0;x<=128;++x)
                {
                    const float alpha=Coverage(shape,float(x)/128.f,float(y)/128.f,seed);
                    Require(std::isfinite(alpha)&&alpha>=0.f&&alpha<=1.f,"Coverage must be finite and bounded for all shapes and seeds");
                }
    }
    Require(Coverage(static_cast<Shape>(100),.5f,.5f)==0.f,"An unrecognized shape must not paint");
    Require(Coverage(Shape::Texture,0,0)==1.f&&Coverage(Shape::HardSquare,1,1)==1.f,
        "Texture and hard square cover the full square footprint");
    Require(Coverage(Shape::HardRound,1,.5f)==1.f&&Coverage(Shape::HardRound,1,1)==0.f,
        "Hard round must include its circular edge and exclude square corners");
    Require(Coverage(Shape::HardRound,.999f,.5f)==1.f&&Coverage(Shape::HardSquare,.001f,.001f)==1.f,
        "Hard brushes must have no soft interior falloff");
    Require(Coverage(Shape::SoftRound,1,.5f)==0.f&&Coverage(Shape::SoftSquare,1,.2f)==0.f,
        "Soft brushes must fall to zero at their footprint boundary");
    Require(Near(Coverage(Shape::SoftRound,.75f,.5f),.5f)
        &&Near(Coverage(Shape::SoftSquare,.75f,.75f),.5f),"Soft falloff must be smooth over the full radius");
    Require(Coverage(Shape::SoftRound,.75f,.75f)<Coverage(Shape::SoftSquare,.75f,.75f),
        "Round and square brushes must use their respective distances");
    for(Shape shape:{Shape::SoftRound,Shape::SoftSquare})
    {
        float previous=1.f;
        for(unsigned i=0;i<=1000;++i)
        {
            const float radius=float(i)/1000.f;
            const float alpha=Coverage(shape,.5f+.5f*radius,.5f);
            Require(alpha<=previous,"Soft coverage must decrease monotonically from center to edge");previous=alpha;
            const float u=float(i%101)/100.f,v=float((i*37)%101)/100.f;
            Require(Near(Coverage(shape,u,v),Coverage(shape,v,u))
                &&Near(Coverage(shape,u,v),Coverage(shape,1.f-u,v)),"Soft brush profiles must be symmetric");
        }
    }
    for(uint64_t seed:Seeds)
    {
        for(unsigned angle=0;angle<128;++angle)
        {
            const float theta=2.f*Pi*float(angle)/128.f;
            Require(Polar(Shape::RandomEdgeRound,.54f,theta,seed)==1.f,
                "Random edge must retain a solid inner core for every seed and direction");
            Require(Polar(Shape::RandomEdgeRound,1.01f,theta,seed)==0.f,
                "Random edge must remain inside its round footprint");
        }
        Require(Coverage(Shape::RandomEdgeRound,1,.5f,seed)==0.f,
            "Random edge must meet the circular boundary with zero coverage");
    }
    unsigned changedLow=0,changedHigh=0,solid=0,empty=0,partial=0,reappearing=0;
    double innerCoverage=0,outerCoverage=0;
    for(unsigned y=0;y<192;++y)
        for(unsigned x=0;x<192;++x)
        {
            const float u=(float(x)+.5f)/192.f,v=(float(y)+.5f)/192.f;
            const float dx=u*2.f-1.f,dy=v*2.f-1.f,radius=std::sqrt(dx*dx+dy*dy);
            const float alpha=Coverage(Shape::RandomEdgeRound,u,v,0);
            Require(alpha==Coverage(Shape::RandomEdgeRound,u,v,0),"A dab must reproduce the same mask at the same UV");
            Require(std::abs(alpha-Coverage(Shape::RandomEdgeRound,u+1.e-6f,v,0))<.002f,
                "Random patches must be continuous in brush space, without per-texel rerandomization");
            if(radius>.65f&&radius<.9f)
            {
                changedLow+=std::abs(alpha-Coverage(Shape::RandomEdgeRound,u,v,1))>.1f;
                changedHigh+=std::abs(alpha-Coverage(Shape::RandomEdgeRound,u,v,0x100000000ull))>.1f;
                solid+=alpha>.99f;empty+=alpha<.01f;partial+=alpha>.01f&&alpha<.99f;
            }
        }
    for(unsigned angle=0;angle<256;++angle)
    {
        const float theta=2.f*Pi*float(angle)/256.f;
        innerCoverage+=Polar(Shape::RandomEdgeRound,.65f,theta,1);
        outerCoverage+=Polar(Shape::RandomEdgeRound,.9f,theta,1);
        bool foundHole=false;
        for(unsigned step=0;step<=100;++step)
        {
            const float alpha=Polar(Shape::RandomEdgeRound,.6f+.0035f*float(step),theta,1);
            if(alpha<.05f)foundHole=true;
            if(foundHole&&alpha>.8f){++reappearing;break;}
        }
    }
    Require(changedLow>500&&changedHigh>500,"Changing either half of the dab seed must visibly change edge patches");
    Require(solid>500&&empty>500&&partial>500,"The random annulus must contain solid patches, holes, and smooth transitions");
    Require(innerCoverage>outerCoverage*3,"Random coverage must decrease toward the outer edge on average");
    Require(reappearing>10,"Random edges must contain interior holes and islands, not only a jittered outside radius");
    std::puts("Surface brush shape, soft falloff, coherent random edge, seed and domain tests passed");
}
