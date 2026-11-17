// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace SilPOM::CameraCollisionMath
{
struct Vec
{
    double x=0,y=0,z=0;
    Vec operator+(Vec b) const{return {x+b.x,y+b.y,z+b.z};}
    Vec operator-(Vec b) const{return {x-b.x,y-b.y,z-b.z};}
    Vec operator*(double s) const{return {x*s,y*s,z*s};}
};
inline double Dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec Cross(Vec a,Vec b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline double Length(Vec v){return std::sqrt(Dot(v,v));}
inline Vec Normalized(Vec v){const double n=Length(v);return n>1.e-12?v*(1/n):Vec{};}
inline bool Finite(Vec v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
struct Hit {double fraction=1;Vec normal;};

inline Vec ClosestSegment(Vec p,Vec a,Vec b)
{
    const Vec edge=b-a;const double size=Dot(edge,edge);
    return a+edge*(size>1.e-20?std::clamp(Dot(p-a,edge)/size,0.,1.):0.);
}
inline bool InsideTriangle(Vec p,Vec a,Vec b,Vec c,Vec normal)
{
    constexpr double epsilon=-1.e-10;
    return Dot(Cross(b-a,p-a),normal)>=epsilon&&Dot(Cross(c-b,p-b),normal)>=epsilon
        &&Dot(Cross(a-c,p-c),normal)>=epsilon;
}
inline Vec ClosestTriangle(Vec p,Vec a,Vec b,Vec c,Vec normal)
{
    const Vec plane=p-normal*Dot(p-a,normal);
    if(InsideTriangle(plane,a,b,c,normal))return plane;
    Vec closest=ClosestSegment(p,a,b);double distance=Dot(p-closest,p-closest);
    for(const Vec candidate:{ClosestSegment(p,b,c),ClosestSegment(p,c,a)})
    {const double d=Dot(p-candidate,p-candidate);if(d<distance){distance=d;closest=candidate;}}
    return closest;
}

// Continuous sphere versus a two-sided triangle, including its edge capsules
// and vertex spheres. Full movement segments avoid tunnelling at high speed.
inline bool SweepTriangle(Vec start,Vec end,double radius,Vec a,Vec b,Vec c,Hit& hit)
{
    if(!Finite(start)||!Finite(end)||!Finite(a)||!Finite(b)||!Finite(c)||!std::isfinite(radius)||radius<=0)return false;
    const Vec delta=end-start,normal=Normalized(Cross(b-a,c-a));
    if(Length(delta)<1.e-12||Length(normal)<.5)return false;
    bool found=false;
    auto accept=[&](double t,Vec n)
    {
        if(std::isfinite(t)&&t>=0&&t<=hit.fraction&&t<=1&&Dot(delta,n)<-1.e-10)
        {hit.fraction=t;hit.normal=n;found=true;}
    };
    const Vec initial=start-ClosestTriangle(start,a,b,c,normal);
    const double initialDistance=Length(initial);
    // A camera enabled close to a wall can always back away from it. At an
    // exactly coincident plane, use winding only to choose a stable escape side.
    if(initialDistance<radius)
        accept(0,initialDistance>1.e-10?initial*(1/initialDistance):normal);
    const double velocity=Dot(delta,normal),planeDistance=Dot(start-a,normal);
    if(std::abs(velocity)>1.e-12)
        for(double side:{-1.,1.})
        {
            const double t=(side*radius-planeDistance)/velocity;
            if(t>=0&&t<=hit.fraction&&InsideTriangle(start+delta*t-normal*(side*radius),a,b,c,normal))
                accept(t,normal*side);
        }
    auto firstRoot=[](double aa,double bb,double cc,double& t)
    {
        if(aa<1.e-20)return false;
        const double discriminant=bb*bb-aa*cc;
        if(discriminant<0)return false;
        t=(-bb-std::sqrt(discriminant))/aa;return true;
    };
    const Vec vertices[]={a,b,c};
    for(unsigned i=0;i<3;++i)
    {
        const Vec vertex=vertices[i],edge=vertices[(i+1)%3]-vertex,q=start-vertex;
        const double edgeSize=Dot(edge,edge);
        double t=0;
        if(firstRoot(Dot(delta,delta),Dot(q,delta),Dot(q,q)-radius*radius,t))
            accept(t,Normalized(start+delta*t-vertex));
        if(edgeSize<1.e-20)continue;
        const Vec perpendicular=q-edge*(Dot(q,edge)/edgeSize);
        const Vec movement=delta-edge*(Dot(delta,edge)/edgeSize);
        if(firstRoot(Dot(movement,movement),Dot(perpendicular,movement),Dot(perpendicular,perpendicular)-radius*radius,t))
        {
            const double along=Dot(q+delta*t,edge)/edgeSize;
            if(along>=0&&along<=1)accept(t,Normalized(perpendicular+movement*t));
        }
    }
    return found;
}

struct MoveResult {Vec position;bool blocked=false;unsigned queries=0;};
// Query returns the nearest fraction and outward normal for the whole segment.
// Three contacts permit ordinary wall sliding and stop safely at corners.
template<class Query> MoveResult Slide(Vec start,Vec end,double radius,Query query,double skin=.002)
{
    MoveResult result;result.position=start;Vec remaining=end-start;
    for(unsigned iteration=0;iteration<3;++iteration)
    {
        const double length=Length(remaining);if(length<1.e-7)break;
        Hit hit;++result.queries;
        if(!query(result.position,result.position+remaining,radius,hit))
        {result.position=result.position+remaining;break;}
        result.blocked=true;
        const double advance=std::clamp(hit.fraction-skin/length,0.,1.);
        result.position=result.position+remaining*advance;
        remaining=remaining*(1-advance);
        const Vec normal=Normalized(hit.normal);
        if(Length(normal)<.5)break;
        remaining=remaining-normal*std::min(0.,Dot(remaining,normal));
    }
    return result;
}
}
