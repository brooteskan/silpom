// SPDX-License-Identifier: MIT
#include "CpuShader.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
using namespace SilPOM::Cpu;
static int checks = 0;
static void Require(bool yes, const char* message)
{
    ++checks;
    if (!yes) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

// Independent oracle: enumerate every cell and solve its polynomial in double.
// No shared DDA, interval helper, coefficient construction, or root solver.
static double Reference(const Texture& tex, const SilPomPatch& p, SilPomRay ray)
{
    double result = std::numeric_limits<double>::infinity();
    const double ox = (double(ray.origin.x)/p.width+.5)*p.tileU*p.textureWidth+p.offsetU*p.textureWidth-.5;
    const double oy = (double(ray.origin.y)/p.height+.5)*p.tileV*p.textureHeight+p.offsetV*p.textureHeight-.5;
    const double vx = double(ray.direction.x)/p.width*p.tileU*p.textureWidth;
    const double vy = double(ray.direction.y)/p.height*p.tileV*p.textureHeight;
    const double ux0 = p.offsetU*p.textureWidth-.5, ux1 = ux0+p.tileU*p.textureWidth;
    const double uy0 = p.offsetV*p.textureHeight-.5, uy1 = uy0+p.tileV*p.textureHeight;
    for (int y=int(std::floor(std::min(uy0,uy1))); y<=int(std::floor(std::max(uy0,uy1))); ++y)
    for (int x=int(std::floor(std::min(ux0,ux1))); x<=int(std::floor(std::max(ux0,ux1))); ++x)
    {
        double lo=ray.tMin, hi=ray.tMax;
        auto slab=[&](double o,double d,double a,double b)
        {
            if(d==0) { if(o<a || o>b) hi=-1; return; }
            const double t0=(a-o)/d,t1=(b-o)/d;
            lo=std::max(lo,std::min(t0,t1)); hi=std::min(hi,std::max(t0,t1));
        };
        slab(ray.origin.x,ray.direction.x,-p.width*.5,p.width*.5);
        slab(ray.origin.y,ray.direction.y,-p.height*.5,p.height*.5);
        slab(ox,vx,x,x+1); slab(oy,vy,y,y+1);
        if(lo>hi) continue;
        auto tap=[&](int a,int b)
        {
            if(p.addressMode==0) { a=(a%int(tex.width)+int(tex.width))%int(tex.width); b=(b%int(tex.height)+int(tex.height))%int(tex.height); }
            else { a=std::clamp(a,0,int(tex.width)-1); b=std::clamp(b,0,int(tex.height)-1); }
            return double(tex.pixels[b*tex.width+a]);
        };
        auto f=[&](double s)
        {
            const double t=lo+(hi-lo)*s, u=ox+vx*t-x,v=oy+vy*t-y;
            const double h=(1-u)*(1-v)*tap(x,y)+u*(1-v)*tap(x+1,y)+(1-u)*v*tap(x,y+1)+u*v*tap(x+1,y+1);
            return ray.origin.z+ray.direction.z*t-p.scale*(h-p.reference);
        };
        const double c=f(0),a=2*(f(1)+c-2*f(.5)),b=f(1)-a-c;
        auto root=[&](double s) { if(s>=-1e-8 && s<=1+1e-8) result=std::min(result,lo+(hi-lo)*std::clamp(s,0.0,1.0)); };
        if(std::abs(a)<1e-12) { if(std::abs(b)>1e-12) root(-c/b); else if(std::abs(c)<1e-12) root(0); }
        else if(b*b-4*a*c>=0) { const double r=std::sqrt(b*b-4*a*c); root((-b-r)/(2*a)); root((-b+r)/(2*a)); }
    }
    return result;
}
int main()
{
    Texture texture{8,8,std::vector<float>(64,.5f)};
    SilPomPatch patch{2,2,.2f,.5f,1,1,0,0,8,8,0,4096};
    auto trace=[&](float3 o,float3 d,float start=0.f,float end=100.f) {return SilPomIntersect(texture,patch,{o,d,start,end});};
    auto hit=trace({0,0,1},{0,0,-1});
    Require(hit.status==SP_HIT && std::abs(hit.t-1)<1e-6,"flat front hit");
    Require(trace({0,0,-1},{0,0,1}).status==SP_HIT,"backside query");
    Require(trace({0,0,1},{1,0,0}).status==SP_MISS,"parallel miss");
    Require(trace({0,0,0},{1,0,0}).status==SP_HIT,"tangent surface");
    Require(trace({0,0,.02f},{0,0,-1}).status==SP_HIT,"origin inside proxy");
    Require(trace({2,0,1},{0,0,-1}).status==SP_MISS,"finite patch domain");
    Require(trace({0,0,1},{0,0,-1},0,.9f).status==SP_MISS,"finite shadow interval");
    Require(trace({0,0,1},{0,0,-1},1.1f,2).status==SP_MISS,"ray minimum");
    Require(std::abs(trace({0,0,1},{0,0,-2}).t-.5f)<1e-6,"unnormalized direction preserves t");
    patch.scale=0; Require(trace({0,0,1},{0,0,-1}).status==SP_HIT,"zero scale"); patch.scale=.2f;
    patch.maxCells=1; Require(trace({-.99f,0,.08f},{1,0,-.001f}).status==SP_EXHAUSTED,"exhaustion differs from miss"); patch.maxCells=4096;
    patch.width=0; Require(trace({0,0,1},{0,0,-1}).status==SP_INVALID,"invalid extent"); patch.width=2;
    std::mt19937 random(40);
    std::uniform_real_distribution<float> unit(0,1), coord(-1,1);
    for(auto& value:texture.pixels) value=unit(random);
    for(int i=0;i<20000;++i)
    {
        patch.addressMode=uint(i%2); patch.tileU=i%3==0 ? -2.f : 1.5f; patch.tileV=.75f;
        patch.offsetU=.13f; patch.offsetV=-.31f; patch.scale=i%4==0?-.2f:.2f;
        SilPomRay ray{{coord(random)*1.4f,coord(random)*1.4f,coord(random)*.25f},normalize({coord(random),coord(random),coord(random)}),0,10};
        hit=SilPomIntersect(texture,patch,ray);
        const double expected=Reference(texture,patch,ray);
        if ((hit.status==SP_HIT)!=std::isfinite(expected) || (hit.status==SP_HIT && std::abs(hit.t-expected)>2e-4))
        {
            std::cerr<<"case "<<i<<" status "<<hit.status<<" t "<<hit.t<<" expected "<<expected<<'\n';
            Require(false,"exhaustive double oracle agreement");
        }
        Require(hit.status!=SP_EXHAUSTED,"bounded reference scenes complete");
        if(hit.status==SP_HIT) Require(std::abs(dot(hit.normal,hit.normal)-1)<1e-5,"finite unit normal");
    }
    std::cout<<"SilPOM: "<<checks<<" checks passed, including 20000 independent oracle rays.\n";
}
