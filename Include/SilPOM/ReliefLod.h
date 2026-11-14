// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace SilPOM::ReliefLod
{
struct Clip {double x,y,z,w;};
// Projective bound, including changing perspective W. The six displaced
// corners enclose the affine relief. A patch switches before any inference.
inline double PixelError(const std::array<Clip,3>& base,const std::array<Clip,6>& relief,double width,double height)
{
    if(!(width>0&&height>0))return std::numeric_limits<double>::infinity();
    double minW=std::numeric_limits<double>::infinity(),maxX=0,maxY=0,dx=0,dy=0,dw=0;
    for(const auto& b:base)
    {
        if(!(b.w>1e-5)||b.z<0||b.z>b.w)return std::numeric_limits<double>::infinity();
        maxX=std::max(maxX,std::abs(b.x/b.w));maxY=std::max(maxY,std::abs(b.y/b.w));
    }
    for(size_t i=0;i<6;++i)
    {
        const auto& r=relief[i];const auto& b=base[i/2];
        if(!(r.w>1e-5)||r.z<0||r.z>r.w)return std::numeric_limits<double>::infinity();
        minW=std::min(minW,r.w);dx=std::max(dx,std::abs(r.x-b.x));
        dy=std::max(dy,std::abs(r.y-b.y));dw=std::max(dw,std::abs(r.w-b.w));
    }
    const double px=(dx+maxX*dw)*width*.5/minW,py=(dy+maxY*dw)*height*.5/minW;
    return std::hypot(px,py);
}
inline bool Select(bool wasFar,bool protectedBoundary,double facing,double error,double budget)
{
    if(!std::isfinite(error)||!std::isfinite(budget)||budget<=0||facing<(protectedBoundary?.5:.35))return false;
    // A tall wall triangle can touch a rim/window even when its interior is
    // distant. Permit it only once the entire boundary error is subpixel.
    const double limit=protectedBoundary?std::min(budget,.25):budget;
    return error<=limit*(wasFar?1.:.75);
}
}
