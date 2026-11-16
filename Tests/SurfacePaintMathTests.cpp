// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/SurfacePaintMath.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace SilPOM::SurfacePaintMath;
// Compile the production shader helper unchanged, adapting only its primitive
// types/builtins. Runtime shading must use the same normal definition as paint.
using float3=Vec3;
struct float4 {float x,y,z,w;};
float dot(float3 a,float3 b){return Dot(a,b);}
float3 cross(float3 a,float3 b){return Cross(a,b);}
float rsqrt(float v){return 1/std::sqrt(v);}
float clamp(float value,float low,float high){return std::clamp(value,low,high);}
#include "../Assets/Shaders/SilPOM/ReceiverUv/SurfaceNormal.azsli"
void Require(bool pass,const char* message)
{if(!pass){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
bool Near(float a,float b,float tolerance=2.e-6f){return std::abs(a-b)<=tolerance;}
bool Near(Vec3 a,Vec3 b,float tolerance=2.e-6f)
{return Near(a.x,b.x,tolerance)&&Near(a.y,b.y,tolerance)&&Near(a.z,b.z,tolerance);}
bool Exact(Vec3 a,Vec3 b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
float Angle(Vec3 a,Vec3 b)
{return std::acos(std::clamp(Dot(Unit(a),Unit(b)),-1.f,1.f))*57.295779513f;}
int main()
{
    for(Vec3 base:std::vector<Vec3>{{0,0,1},{0,0,-1},{1,0,0},{0,1,0},{.1f,.2f,.97f},{-.6f,.2f,-.5f}})
    {
        base=Unit(base);
        Require(Exact(ComposeNormal(base,{}),base),"No coverage must preserve the exact live normal");
        for(float opacity:{.01f,.25f,.8f,1.f})
        {
            NormalTexel flat;PaintNormal(flat,{0,0,1},opacity);
            Require(Exact(ComposeNormal(base,flat),base),"Flat detail must preserve the exact live normal");
            Require(Exact(ComposeNormal(base,flat),ComposeNormal(base,NormalTexel{})),"Flat paint cannot introduce receiver curvature");
        }
    }
    {
        NormalTexel normal;
        const Vec3 first=Unit({.6f,0,.8f}),second=Unit({0,.8f,.6f});
        PaintNormal(normal,first,.4f);PaintNormal(normal,second,.5f);
        Require(Near(normal.premultiplied,first*.2f+second*.5f),"Normal painting is authoritative premultiplied vector over");
        Require(Near(normal.coverage,.7f),"Normal coverage accumulates independently of material normal");
        const auto before=normal;PaintNormal(normal,first,0);
        Require(Exact(before.premultiplied,normal.premultiplied)&&before.coverage==normal.coverage,"Zero opacity must not change stored paint");
        EraseNormal(normal,.25f);
        Require(Near(normal.premultiplied,before.premultiplied*.75f)&&Near(normal.coverage,before.coverage*.75f),"Erase scales detail and coverage together");
        EraseNormal(normal,1);
        Require(Exact(normal.premultiplied,{})&&normal.coverage==0,"Full erase clears authoritative normal state");
    }
    {
        NormalTexel edge[2];PaintNormal(edge[0],{1,0,0},1);
        const float weights[2]={.25f,.75f};
        const auto filtered=FilterNormal(edge,weights,2);
        Require(Near(filtered.premultiplied,{.25f,0,0})&&Near(filtered.coverage,.25f),"Filtering keeps coverage-premultiplied detail");
        const auto result=ComposeNormal({0,0,1},filtered);
        Require(Near(result,Unit({.25f,0,.75f})),"Minified edge normal uses uncovered neutral area");
        Require(Angle(result,Unit({1,0,.75f}))>30,"Premultiplied mip vectors must not be normalized before composition");
        NormalTexel cancel;PaintNormal(cancel,{1,0,0},1);PaintNormal(cancel,{-1,0,0},.5f);
        Require(Exact(ComposeNormal({0,0,1},cancel),{0,0,1}),"Opposing grazing details have a defined neutral result");
    }
    {
        // Two 2x mip reductions must equal one linear reduction of the leaves.
        std::vector<NormalTexel> leaves(64),mip1(16),mip2(4);
        std::vector<float> weights(64,1.f/64.f);
        for(unsigned i=0;i<leaves.size();++i)
            PaintNormal(leaves[i],Unit({std::sin(float(i)),std::cos(float(i)*.7f),.4f}),float(i%7)/6);
        const float quarter[4]={.25f,.25f,.25f,.25f};
        for(unsigned i=0;i<mip1.size();++i)mip1[i]=FilterNormal(leaves.data()+i*4,quarter,4);
        for(unsigned i=0;i<mip2.size();++i)mip2[i]=FilterNormal(mip1.data()+i*4,quarter,4);
        const auto nested=FilterNormal(mip2.data(),quarter,4),direct=FilterNormal(leaves.data(),weights.data(),leaves.size());
        Require(Near(nested.premultiplied,direct.premultiplied)&&Near(nested.coverage,direct.coverage),"Mip reduction is linear in authoritative texture state");
        for(Vec3 base:std::vector<Vec3>{{0,0,1},{.7f,.2f,.5f},{-.4f,-.2f,.8f}})
            Require(Near(ComposeNormal(Unit(base),nested),ComposeNormal(Unit(base),direct)),"Minification cannot fit against a separately evaluated base normal");
    }
    {
        NormalTexel paint;PaintNormal(paint,Unit({.6f,.2f,.7f}),.65f);
        const Vec3 base=Unit({.4f,-.5f,.6f});
        for(float angle:{-.8f,.2f,1.5f})
        {
            auto turned=paint;turned.premultiplied=RotateDetail(turned.premultiplied,angle);
            Require(Near(ComposeNormal(RotateDetail(base,angle),turned),RotateDetail(ComposeNormal(base,paint),angle),4.e-6f),"Normal detail must rotate with the receiver frame");
            Require(Exact(RotateDetail({0,0,1},angle),{0,0,1}),"Brush rotation cannot change flat normal detail");
        }
        for(unsigned i=0;i<1000;++i)
        {
            const auto live=Unit({std::sin(i*.41f),std::cos(i*.17f),std::sin(i*.13f)});
            const auto out=ComposeNormal(live,paint);
            Require(Finite(out)&&Near(Dot(out,out),1,2.e-5f),"Composition must stay finite and normalized for varied live normals");
            const auto m=paint.premultiplied;
            Require(Near(SilPomComposeSurfaceNormal(live,{m.x,m.y,m.z,paint.coverage}),out),"Production shader must use the authoritative CPU normal rule");
        }
        Require(Finite(ComposeNormal({0,0,-1},paint)),"Antipodal live normal requires a defined finite rotation");
        const auto m=paint.premultiplied;
        Require(Near(SilPomComposeSurfaceNormal({0,0,-1},{m.x,m.y,m.z,paint.coverage}),ComposeNormal({0,0,-1},paint)),"Shader and CPU antipodal conventions must match");
    }
    {
        for(unsigned mode=0;mode<5;++mode)
        {
            ColorTexel first;PaintColor(first,{.2f,.5f,.9f},.3f,mode);
            Require(Near(first.premultiplied,{.06f,.15f,.27f})&&Near(first.coverage,.3f),"Every isolated blend mode places its source onto an empty layer");
            ColorTexel destination;PaintColor(destination,{.2f,.5f,.9f},.6f);
            PaintColor(destination,{.8f,.4f,.3f},.7f,mode);
            Require(Near(destination.coverage,.88f),"Color blend mode cannot change source-over coverage");
            const Vec3 d{.2f,.5f,.9f},s{.8f,.4f,.3f};
            const Vec3 expected=d*(.6f*.3f)+(s*.4f+Vec3{BlendChannel(d.x,s.x,mode),BlendChannel(d.y,s.y,mode),BlendChannel(d.z,s.z,mode)}*.6f)*.7f;
            Require(Near(destination.premultiplied,expected),"Paint color blends within the isolated layer");
            for(float channel:{destination.premultiplied.x,destination.premultiplied.y,destination.premultiplied.z})
                Require(channel>=0&&channel<=destination.coverage,"Paint color stays bounded by its coverage");
        }
        Require(Near(BlendChannel(.8f,.6f,2),1),"Add is explicitly clamped to the texture color range");
        Require(Near(BlendChannel(.2f,.7f,1),.14f),"Multiply blend definition changed");
        Require(Near(BlendChannel(.2f,.7f,3),.76f),"Screen blend definition changed");
        Require(Near(BlendChannel(.2f,.7f,4),.28f)&&Near(BlendChannel(.8f,.7f,4),.88f),"Overlay selects by destination color");
    }
    {
        const Vec3 material{.1f,.2f,.3f};
        ColorTexel flattened;PaintColor(flattened,{0,0,1},1);PaintColor(flattened,{1,0,0},1);EraseColor(flattened,1);
        Require(Exact(ComposeColor(material,flattened),material),"Erasing flattened red over blue reveals material, not overwritten blue");
        ColorTexel blue,red;PaintColor(blue,{0,0,1},1);PaintColor(red,{1,0,0},1);EraseColor(red,1);
        Require(Exact(ComposeColor(ComposeColor(material,blue),red),{0,0,1}),"Separate lower layers survive erase");
        ColorTexel edge[2];PaintColor(edge[0],{1,0,0},1);const float weights[2]={.5f,.5f};
        Require(Near(ComposeColor(material,FilterColor(edge,weights,2)),material*.5f+Vec3{.5f,0,0}),"Filtered color edges preserve premultiplied coverage");
        NormalTexel lower,top;PaintNormal(lower,Unit({.4f,0,.9f}),1);PaintNormal(top,Unit({-.2f,.3f,.8f}),1);
        const auto lowerResult=ComposeNormal({0,0,1},lower);EraseNormal(top,1);
        Require(Exact(ComposeNormal(lowerResult,top),lowerResult),"Normal erase reveals the separate lower normal layer");
    }
    {
        // The rejected representation fits one N0, but repeated normalized
        // blends are not affine in a different live base normal N1.
        const Vec3 targets[2]={Unit({.8f,.1f,.4f}),Unit({-.3f,.7f,.5f})};
        auto oldSequence=[&](Vec3 base){return Unit(Unit(base*.6f+targets[0]*.4f)*.4f+targets[1]*.6f);};
        const Vec3 base0{0,0,1},base1=Unit({-.8f,-.2f,.4f});const float transmittance=.6f*.4f;
        const Vec3 bias=oldSequence(base0)-base0*transmittance;
        Require(Angle(Unit(base1*transmittance+bias),oldSequence(base1))>1,"An affine fit cannot define a base-independent painted normal result");
    }
    std::puts("Surface paint authoritative color/normal, erasing, filtering, mip and live-normal composition tests passed");
}
