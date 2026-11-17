// SPDX-License-Identifier: MIT
#include "GradientContextTestData.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace SilPOM::GradientCpu;
namespace
{
std::size_t checks=0;double maxScaledError=0;
void Require(bool condition,const std::string& message)
{++checks;if(!condition)throw std::runtime_error(message);}
void Near(double got,double wanted,const std::string& message,double absolute=2e-8,double relative=3e-5)
{
    ++checks;const double limit=absolute+relative*std::abs(wanted);
    const double error=std::abs(got-wanted);maxScaledError=std::max(maxScaledError,error/limit);
    if(!std::isfinite(got)||!std::isfinite(wanted)||error>limit)
        throw std::runtime_error(message+": got="+std::to_string(got)+" expected="+std::to_string(wanted));
}
struct D2 {double x=0,y=0;};
struct D3 {double x=0,y=0,z=0;};
D2 operator+(D2 a,D2 b){return {a.x+b.x,a.y+b.y};}
D2 operator-(D2 a,D2 b){return {a.x-b.x,a.y-b.y};}
D2 operator*(D2 a,double b){return {a.x*b,a.y*b};}
D3 operator+(D3 a,D3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
D3 operator-(D3 a,D3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
D3 operator*(D3 a,double b){return {a.x*b,a.y*b,a.z*b};}
double Dot(D3 a,D3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
double Length(D3 a){return std::sqrt(Dot(a,a));}
float2 F(D2 a){return {float(a.x),float(a.y)};}
float3 F(D3 a){return {float(a.x),float(a.y),float(a.z)};}
D2 D(float2 a){return {a.x,a.y};}
void Near(D2 a,D2 b,const std::string& message,double absolute=2e-8,double relative=3e-5)
{Near(a.x,b.x,message+" x",absolute,relative);Near(a.y,b.y,message+" y",absolute,relative);}
void Near(D3 a,D3 b,const std::string& message,double absolute=2e-8,double relative=3e-5)
{Near(a.x,b.x,message+" x",absolute,relative);Near(a.y,b.y,message+" y",absolute,relative);Near(a.z,b.z,message+" z",absolute,relative);}
D3 D(float3 a){return {a.x,a.y,a.z};}
constexpr double Width=1920,Height=1080,Sx=1.2,Sy=1.6,Ox=.08,Oy=-.04,ProjA=-.001,ProjB=.2;
float4 Clip(D3 x,double homogeneous=1)
{return {float(Sx*x.x+Ox*x.z),float(Sy*x.y+Oy*x.z),float(ProjA*x.z+ProjB*homogeneous),float(x.z)};}
D3 Project(D3 x,double width=Width,double height=Height)
{return {(Sx*x.x/x.z+Ox+1)*width*.5,(1-Sy*x.y/x.z-Oy)*height*.5,ProjA+ProjB/x.z};}
D3 Unproject(D2 pixel,double depth)
{
    const double z=ProjB/(depth-ProjA);
    return {((2*pixel.x/Width-1)-Ox)*z/Sx,((1-2*pixel.y/Height)-Oy)*z/Sy,z};
}
struct Surface
{
    D3 origin{-.2,.15,4},du{1.3,.12,.35},dv{.1,.95,-.18};
    D3 direction{.12,-.08,.75},directionDu{.06,.04,.07},directionDv{-.02,.03,-.06};
    bool relief=true;
    double HeightAt(D2 p)const
    {
        if(!relief)return 0;
        const double x=.5*p.x+.3,y=.7*p.y+.2;
        return .65*((.2*(1-x)+.7*x)*(1-y)+(.3*(1-x)+.9*x)*y-.35);
    }
    D3 At(D2 p)const
    {return origin+du*p.x+dv*p.y+(direction+directionDu*p.x+directionDv*p.y)*HeightAt(p);}
    D2 Solve(D2 target,D2 initial)const
    {
        D2 q=initial;constexpr double e=1e-5;
        for(int i=0;i<8;++i)
        {
            const auto p=Project(At(q));
            const auto pu=(Project(At(q+D2{e,0}))-Project(At(q-D2{e,0})))*(1/(2*e));
            const auto pv=(Project(At(q+D2{0,e}))-Project(At(q-D2{0,e})))*(1/(2*e));
            const double ex=p.x-target.x,ey=p.y-target.y,det=pu.x*pv.y-pv.x*pu.y;
            q.x-=(ex*pv.y-ey*pv.x)/det;q.y-=(ey*pu.x-ex*pu.y)/det;
        }
        return q;
    }
    SilPomGradientContext Context(D2 q,double width=Width,double height=Height)const
    {
        const auto h=relief?SilPomGradientBilinear({.2f,.7f,.3f,.9f},
            {float(.5*q.x+.3),float(.7*q.y+.2)},{.5f,.7f},.65f,.35f):float3{};
        float3 tu,tv;SilPomGradientReliefTangents(F(du),F(dv),F(direction+directionDu*q.x+directionDv*q.y),
            F(directionDu),F(directionDv),h,tu,tv);
        return SilPomGradientFromTangents(Clip(At(q)),Clip(D(tu),0),Clip(D(tv),0),
            {float(width),float(height)},{1,1},relief?SP_GRADIENT_RELIEF:SP_GRADIENT_BASE);
    }
};
void TestReliefPlaneProjection()
{
    constexpr double epsilon=.01;
    for(int kind=0;kind<4;++kind)
    {
        Surface s;if(kind==1)s.relief=false;
        if(kind==2){s.du={-1.1,.21,-.31};s.dv={.12,1.2,.24};}
        if(kind==3){s.relief=false;s.du={.6,.7,.2};s.dv={-.9,.2,-.3};}
        for(int i=0;i<7;++i)for(int j=0;j<7;++j)
        {
            // Fractional offsets stand in for independent 2x MSAA sample positions.
            const D2 uv{-.35+.18*i+.00031,-.15+.16*j-.00017};const auto c=s.Context(uv);
            Require(c.validity==SP_GRADIENT_VALID,"Regular surface must be conditioned");
            const auto p=Project(s.At(uv));const D2 center{p.x,p.y};
            const auto xp=s.Solve(center+D2{epsilon,0},uv),xm=s.Solve(center-D2{epsilon,0},uv);
            const auto yp=s.Solve(center+D2{0,epsilon},uv),ym=s.Solve(center-D2{0,epsilon},uv);
            Near(D(SilPomGradientUvX(c)),(xp-xm)*(1/(2*epsilon)),"Projected local UV dx");
            Near(D(SilPomGradientUvY(c)),(yp-ym)*(1/(2*epsilon)),"Projected local UV dy");
            const double zx=(Project(s.At(xp)).z-Project(s.At(xm)).z)/(2*epsilon);
            const double zy=(Project(s.At(yp)).z-Project(s.At(ym)).z)/(2*epsilon);
            Near(c.depth.x,zx,"Recorded depth dx",1e-10);Near(c.depth.y,zy,"Recorded depth dy",1e-10);
            // Inverse projection stays anchored to an intentionally offset recorded depth.
            const double recorded=p.z+2e-4;
            const float4 h{float((2*p.x/Width-1-Ox)/Sx),float((1-2*p.y/Height-Oy)/Sy),1,float((recorded-ProjA)/ProjB)};
            const float4 hx{float(2/Width/Sx),0,0,float(c.depth.x/ProjB)};
            const float4 hy{0,float(-2/Height/Sy),0,float(c.depth.y/ProjB)};
            float3 dx,dy;Require(SilPomGradientWorldPosition(h,hx,hy,dx,dy),"Valid inverse projection");
            const auto wx=(Unproject(center+D2{epsilon,0},recorded+zx*epsilon)-Unproject(center-D2{epsilon,0},recorded-zx*epsilon))*(1/(2*epsilon));
            const auto wy=(Unproject(center+D2{0,epsilon},recorded+zy*epsilon)-Unproject(center-D2{0,epsilon},recorded-zy*epsilon))*(1/(2*epsilon));
            Near(D(dx),wx,"Anchored inverse projection dx");Near(D(dy),wy,"Anchored inverse projection dy");
            const auto doubled=s.Context(uv,Width*2,Height*2);
            Near(doubled.uv.x,c.uv.x*.5,"Raster pixel units u",1e-10);
            Near(doubled.uv.w,c.uv.w*.5,"Raster pixel units v",1e-10);
            Near(doubled.depth.x,c.depth.x*.5,"Raster pixel depth units",1e-12);
        }
    }
    // The unnormalized affine direction derivatives must survive UV mirroring.
    const D2 u0{.2,-.3},u1{-1,.4},u2{.5,1.1};
    const D3 base{.3,-.1,.8},du{.21,-.3,.16},dv{-.09,.12,.03};
    auto d=[&](D2 u){return base+du*u.x+dv*u.y;};float3 actualU,actualV;
    Require(SilPomGradientDirectionPartials(F(u0),F(u1),F(u2),F(d(u0)),F(d(u1)),F(d(u2)),actualU,actualV),"Mirrored direction basis");
    Near(D(actualU),du,"Affine direction du",2e-7);Near(D(actualV),dv,"Affine direction dv",2e-7);
}
void TestBilinearAndBranches()
{
    const float4 t{.2f,.7f,.3f,.9f};
    auto value=[](double x,double y){return .65*((.2*(1-x)+.7*x)*(1-y)+(.3*(1-x)+.9*x)*y-.35);};
    for(double x:{0.,.2,.8,1.})for(double y:{0.,.3,1.})
    {
        auto h=SilPomGradientBilinear(t,{float(x),float(y)},{7,11},.65f,.35f);constexpr double e=1e-5;
        Near(h.x,value(x,y),"Bilinear height",6e-8);
        Near(h.y,(value(x+e,y)-value(x-e,y))/(2*e)*7,"Bilinear source hu",3e-7);
        Near(h.z,(value(x,y+e)-value(x,y-e))/(2*e)*11,"Bilinear source hv",3e-7);
    }
    // Piecewise-linear seam: the chosen positive-side cell owns the derivative.
    auto positive=SilPomGradientBilinear({.4f,.9f,.4f,.9f},{0,.3f},{8,8},1,0);
    auto negative=SilPomGradientBilinear({.2f,.4f,.2f,.4f},{1,.3f},{8,8},1,0);
    Near(positive.x,negative.x,"Height is continuous at cell seam",1e-7);
    Near(positive.y,4,"Positive-side seam convention",1e-7);Near(negative.y,1.6,"Other one-sided slope",1e-6);
    for(double u:{-.5,.5})
    {
        // X=(u^2,v,3) has two same-face local roots at one visible pixel.
        auto c=SilPomGradientFromTangents(Clip({u*u,.2,3}),Clip({2*u,0,0},0),Clip({0,1,0},0),{1920,1080},{1,1},SP_GRADIENT_RELIEF);
        Require(c.validity==SP_GRADIENT_VALID,"Each fold branch is regular");
        const double pixel=Project({u*u,.2,3}).x,e=.01;
        auto root=[&](double x){return std::copysign(std::sqrt(((2*x/Width-1)-Ox)*3/Sx),u);};
        Near(c.uv.x,(root(pixel+e)-root(pixel-e))/(2*e),"Same-face branch-local derivative",1e-8);
        Require((c.uv.x>0)==(u>0),"Branches keep opposite UV continuation signs");
    }
    auto tangent=SilPomGradientFromTangents(Clip({0,.2,3}),Clip({0,0,0},0),Clip({0,1,0},0),{1920,1080},{2,3},SP_GRADIENT_RELIEF);
    Require(tangent.validity==SP_GRADIENT_COARSE,"Fold tangency selects defined coarse continuation");
    Near(tangent.uv.x,2,"Coarse u span");Near(tangent.uv.w,3,"Coarse v span");
    auto almost=SilPomGradientFromTangents({0,0,.1f,1},{1,0,0,0},{1,1e-8f,0,0},{1920,1080},{1,1},SP_GRADIENT_SHELL);
    Require(almost.validity==SP_GRADIENT_COARSE,"Near singular Jacobian uses coarse footprint");
    auto bad=SilPomGradientFromTangents({0,0,.1f,-1},{1,0,0,0},{0,1,0,0},{1920,1080},{1,1},SP_GRADIENT_BASE);
    Require(bad.validity==SP_GRADIENT_COARSE,"Behind-camera projection is defined");
    auto invalid=SilPomGradientUnpack({0,0,0,0},{0,0,0,0});Require(invalid.validity==SP_GRADIENT_COARSE,"Missing context never yields zero valid footprint");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    auto nonfinite=SilPomGradientCoarse({nan,nan},SP_GRADIENT_PROXY);Require(nonfinite.uv.x==1&&nonfinite.uv.w==1,"Finite coarse fallback");
    float3 dx,dy;Require(!SilPomGradientWorldPosition({0,0,0,0},{0,0,0,0},{0,0,0,0},dx,dy),"Invalid inverse projection explicitly rejected");
}
double Smooth(double a,double b,double x)
{const double q=std::clamp((x-a)/(b-a),0.,1.);return q*q*(3-2*q);}
void TestMorph()
{
    const D2 uvA{.12,.73},uvB{.39,-.28},ax{.01,-.03},ay{-.02,.04},bx{.03,.02},by{.01,-.01};
    const D3 pa{1,2,3},pb{2,-1,4},pax{.1,.03,-.02},pay{.03,-.04,.01},pbx{-.05,.04,.07},pby{.02,.03,-.04};
    const double start=.4,end=1.7,value=1.02;const D2 ds{.03,-.02},de{-.04,.05},dv{.12,-.08};
    const float w=float(Smooth(start,end,value));auto dw=SilPomGradientSmoothstep(float(start),float(end),float(value),F(ds),F(de),F(dv));
    auto weight=[&](D2 p){return Smooth(start+ds.x*p.x+ds.y*p.y,end+de.x*p.x+de.y*p.y,value+dv.x*p.x+dv.y*p.y);};
    auto uv=[&](D2 p){const double v=weight(p);return (uvA+ax*p.x+ay*p.y)*(1-v)+(uvB+bx*p.x+by*p.y)*v;};
    auto position=[&](D2 p){const double v=weight(p);return (pa+pax*p.x+pay*p.y)*(1-v)+(pb+pbx*p.x+pby*p.y)*v;};
    const auto duv=SilPomGradientMorphUv(F(uvA),F(uvB),{float(ax.x),float(ax.y),float(ay.x),float(ay.y)},
        {float(bx.x),float(bx.y),float(by.x),float(by.y)},w,dw);
    float3 px,py;SilPomGradientMorphPosition(F(pa),F(pb),F(pax),F(pay),F(pbx),F(pby),w,dw,px,py);
    constexpr double e=1e-4;
    Near(dw.x,(weight({e,0})-weight({-e,0}))/(2*e),"Morph weight including moving endpoints",1e-7);
    Near(dw.y,(weight({0,e})-weight({0,-e}))/(2*e),"Morph y weight",1e-7);
    Near(D2{duv.x,duv.y},(uv({e,0})-uv({-e,0}))*(1/(2*e)),"Full morph UV dx",1e-7);
    Near(D2{duv.z,duv.w},(uv({0,e})-uv({0,-e}))*(1/(2*e)),"Full morph UV dy",1e-7);
    Near(D(px),(position({e,0})-position({-e,0}))*(1/(2*e)),"Full morph position dx",1e-7);
    Near(D(py),(position({0,e})-position({0,-e}))*(1/(2*e)),"Full morph position dy",1e-7);
    auto clamped=SilPomGradientSmoothstep(0,1,2,{0,0},{0,0},{1,1});Require(clamped.x==0&&clamped.y==0,"Clamped morph derivative");
}
void TestRayAlignedMorph()
{
    Surface relief,shell;shell.relief=false;
    const D2 initial{.43,.37};const auto projected=Project(relief.At(initial));const D2 pixel{projected.x,projected.y};
    constexpr double cutoff=4.4,overlap=.2,scale=.08;
    struct MorphSample {D2 a,b,uv;D3 origin,mid,direction,position;double ta,tb,start,end,weight;};
    auto sample=[&](D2 p)
    {
        MorphSample m{};m.a=relief.Solve(p,initial);m.b=shell.Solve(p,initial);
        m.origin=Unproject(p,1);m.mid=Unproject(p,.5);const auto ray=m.mid-m.origin;
        m.direction=ray*(1/Length(ray));const auto a=relief.At(m.a),b=shell.At(m.b);
        m.ta=Dot(a-m.origin,m.direction);m.tb=Dot(b-m.origin,m.direction);
        m.end=(cutoff-overlap-m.origin.z)/m.direction.z;m.start=m.end-12*scale/m.direction.z;
        m.weight=Smooth(m.start,m.end,m.tb);m.position=a*(1-m.weight)+b*m.weight;
        m.uv=m.a*(1-m.weight)+m.b*m.weight;return m;
    };
    const auto m=sample(pixel);Require(m.weight>0&&m.weight<1,"Ray morph fixture exercises the transition");
    const auto ca=relief.Context(m.a),cb=shell.Context(m.b);
    auto inverseH=[&](double z){return float4{float((2*pixel.x/Width-1-Ox)/Sx),float((1-2*pixel.y/Height-Oy)/Sy),1,float((z-ProjA)/ProjB)};};
    const float4 hx{float(2/Width/Sx),0,0,0},hy{0,float(-2/Height/Sy),0,0};
    float3 ox,oy,mx,my;
    Require(SilPomGradientWorldPosition(inverseH(1),hx,hy,ox,oy),"Ray origin projection");
    Require(SilPomGradientWorldPosition(inverseH(.5),hx,hy,mx,my),"Ray midpoint projection");
    const auto differenceX=D(mx)-D(ox),differenceY=D(my)-D(oy);
    const double rayLength=Length(m.mid-m.origin);
    const auto directionX=(differenceX-m.direction*Dot(m.direction,differenceX))*(1/rayLength);
    const auto directionY=(differenceY-m.direction*Dot(m.direction,differenceY))*(1/rayLength);
    const D2 rateDerivative{directionX.z,directionY.z};const double rate=m.direction.z;
    // Our inverse projection's H.w depends only on depth, so d(nearViewDepth)=0.
    const D2 endDerivative=rateDerivative*(-m.end/rate);
    const D2 startDerivative=endDerivative+rateDerivative*(12*scale/(rate*rate));
    auto endpointDerivatives=[&](D3 position,const SilPomGradientContext& c,float3& dx,float3& dy)
    {
        auto ex=hx,ey=hy;ex.w=float(c.depth.x/ProjB);ey.w=float(c.depth.y/ProjB);
        Require(SilPomGradientWorldPosition(inverseH(Project(position).z),ex,ey,dx,dy),"Morph endpoint projection");
    };
    float3 ax,ay,bx,by;const auto a=relief.At(m.a),b=shell.At(m.b);
    endpointDerivatives(a,ca,ax,ay);endpointDerivatives(b,cb,bx,by);
    const D2 distanceDerivative{Dot(D(bx)-D(ox),m.direction),Dot(D(by)-D(oy),m.direction)};
    const auto dw=SilPomGradientSmoothstep(float(m.start),float(m.end),float(m.tb),F(startDerivative),F(endDerivative),F(distanceDerivative));
    const auto uv=SilPomGradientMorphUv(F(m.a),F(m.b),ca.uv,cb.uv,float(m.weight),dw);
    float3 positionX,positionY;SilPomGradientMorphPosition(F(a),F(b),ax,ay,bx,by,float(m.weight),dw,positionX,positionY);
    const auto depth=SilPomGradientDepthFromClip(Clip(m.position),Clip(D(positionX),0),Clip(D(positionY),0));
    constexpr double e=.01;const auto xp=sample(pixel+D2{e,0}),xm=sample(pixel-D2{e,0});
    const auto yp=sample(pixel+D2{0,e}),ym=sample(pixel-D2{0,e});
    Near(dw.x,(xp.weight-xm.weight)/(2*e),"Complete ray/fade morph dW dx",2e-8);
    Near(dw.y,(yp.weight-ym.weight)/(2*e),"Complete ray/fade morph dW dy",2e-8);
    Near(D2{uv.x,uv.y},(xp.uv-xm.uv)*(1/(2*e)),"Complete ray morph UV dx");
    Near(D2{uv.z,uv.w},(yp.uv-ym.uv)*(1/(2*e)),"Complete ray morph UV dy");
    Near(D(positionX),(xp.position-xm.position)*(1/(2*e)),"Complete ray morph world dx");
    Near(D(positionY),(yp.position-ym.position)*(1/(2*e)),"Complete ray morph world dy");
    Near(depth.x,(Project(xp.position).z-Project(xm.position).z)/(2*e),"Complete ray morph depth dx",1e-10);
    Near(depth.y,(Project(yp.position).z-Project(ym.position).z)/(2*e),"Complete ray morph depth dy",1e-10);
}
void TestRepair()
{
    const D2 ua{-.2,.5},ub{.8,-.3},axis{3,4},dw=axis*(1/25.);const double za=.23,zb=.61,w=.37;
    const SilPomGradientContext a{{.021f,-.013f,.017f,.029f},{.0013f,-.0019f},SP_GRADIENT_SHELL,SP_GRADIENT_VALID};
    const SilPomGradientContext b{{-.011f,.023f,.031f,-.007f},{-.0021f,.0011f},SP_GRADIENT_RELIEF,SP_GRADIENT_VALID};
    auto perp=[&](D2 p){return p-axis*((p.x*axis.x+p.y*axis.y)/25.);};
    auto donorUv=[&](D2 center,const SilPomGradientContext& c,D2 p){p=perp(p);return center+D2{c.uv.x*p.x+c.uv.z*p.y,c.uv.y*p.x+c.uv.w*p.y};};
    auto donorDepth=[&](double center,const SilPomGradientContext& c,D2 p){p=perp(p);return center+c.depth.x*p.x+c.depth.y*p.y;};
    for(bool weighted:{false,true})
    {
        auto field=[&](D2 p)
        {
            const double t=w+dw.x*p.x+dw.y*p.y,zA=donorDepth(za,a,p),zB=donorDepth(zb,b,p);
            const double z=zA*(1-t)+zB*t;const auto uA=donorUv(ua,a,p),uB=donorUv(ub,b,p);
            const D2 uv=weighted?(uA*(zA*(1-t))+uB*(zB*t))*(1/z):uA*(1-t)+uB*t;
            return D3{uv.x,uv.y,z};
        };
        const auto c=SilPomGradientBlend(F(ua),float(za),a,F(ub),float(zb),b,float(w),F(dw),weighted,
            weighted?SP_GRADIENT_REPAIR_DEPTH_WEIGHTED:SP_GRADIENT_REPAIR_ARITHMETIC);
        constexpr double e=1e-4;const auto x=(field({e,0})-field({-e,0}))*(1/(2*e)),y=(field({0,e})-field({0,-e}))*(1/(2*e));
        Near(D3{c.uv.x,c.uv.y,c.depth.x},x,"Donor bridge derivative dx",1e-7);
        Near(D3{c.uv.z,c.uv.w,c.depth.y},y,"Donor bridge derivative dy",1e-7);
        Require((c.representation&SP_GRADIENT_REPRESENTATION_MASK)==SP_GRADIENT_SHELL,"Repair retains farther donor provenance");
        const auto center=field({0,0});const auto anchored=SilPomGradientBlendAnchored(F(ua),float(za),a,F(ub),float(zb),b,float(w),F(dw),weighted,
            weighted?SP_GRADIENT_REPAIR_DEPTH_WEIGHTED:SP_GRADIENT_REPAIR_ARITHMETIC,{float(center.x),float(center.y)},float(center.z));
        Near(anchored.uv.x,c.uv.x,"Exact stored center anchoring",1e-7);
    }
    SilPomGradientContext linear{{1,0,0,1},{0,0},SP_GRADIENT_BASE,SP_GRADIENT_VALID};
    auto bridge=SilPomGradientBlend({-1,0},.4f,linear,{1,0},.4f,linear,.5f,{.5f,0},false,SP_GRADIENT_REPAIR_ARITHMETIC);
    Near(bridge.uv.x,1,"Bridge derivative is not counted twice");Near(bridge.uv.w,1,"Bridge retains perpendicular donor slope");
    auto copy=SilPomGradientCopied(a);Near(copy.uv.x,a.uv.x,"Copy inherits owner slope",1e-10);
    Require(copy.representation==(SP_GRADIENT_SHELL|SP_GRADIENT_REPAIR_COPY),"Copy preserves base representation");
    auto unpack=SilPomGradientUnpack(copy.uv,SilPomGradientPackDepth(copy));Require(unpack.representation==copy.representation&&unpack.validity==copy.validity,"Numeric metadata survives float sidecar round trip");
    auto coarse=a;coarse.validity=SP_GRADIENT_COARSE;coarse.uv={2,0,0,3};
    auto mixed=SilPomGradientBlend(F(ua),float(za),coarse,F(ub),float(zb),b,float(w),F(dw),false,SP_GRADIENT_REPAIR_ARITHMETIC);
    Require(mixed.validity==SP_GRADIENT_COARSE,"Repair carries donor conditioning");
    Require(mixed.uv.x==2&&mixed.uv.w==3&&mixed.uv.y==0&&mixed.uv.z==0,"Coarse repair retains full-chart donor envelope");
    Require(mixed.depth.x==0&&mixed.depth.y==0,"Coarse repair uses defined constant recorded-depth continuation");
    Require(mixed.representation==(SP_GRADIENT_SHELL|SP_GRADIENT_REPAIR_ARITHMETIC),"Coarse repair retains selected donor and repair provenance");
    coarse.uv={0,0,0,0};
    auto degenerate=SilPomGradientBlend({0,0},.5f,coarse,{0,0},.5f,b,.5f,{.5f,0},true,SP_GRADIENT_REPAIR_DEPTH_WEIGHTED);
    Require(degenerate.uv.x>=1&&degenerate.uv.w>=1,"Equal-UV coarse bridge never collapses to zero footprint");
}
}
int main()
{
    try
    {
        TestReliefPlaneProjection();TestBilinearAndBranches();TestMorph();TestRayAlignedMorph();TestRepair();
        std::cout<<checks<<" gradient checks passed against double finite-difference oracles; max tolerance fraction="<<maxScaledError<<".\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
