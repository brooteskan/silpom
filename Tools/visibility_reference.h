// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SilPOM::VisibilityReference
{
// Independent FP64 cell traversal. The maximum of the bilinear height minus
// the straight light ray is a quadratic on each cell, so thin occluders between
// the baseline's discrete samples are included. -1 means numerical/budget doubt.
inline double Trace(const float* pixels,int width,int height,double x0,double y0,double dx,double dy,
    double start,double rise,double bias,double scale,double reference,double reach,bool clamp,uint32_t& cells)
{
    const auto tap=[&](int x,int y)
    {
        if(clamp) {x=std::clamp(x,0,width-1);y=std::clamp(y,0,height-1);}
        else {x=(x%width+width)%width;y=(y%height+height)%height;}
        return double(pixels[size_t(y)*width+x]);
    };
    double t=0;bool uncertain=false;cells=0;
    const double rate=std::max(std::abs(dx),std::abs(dy));
    if(reach<=0) return 1;
    while(t<reach && cells<25000)
    {
        const double probe=std::min(reach,t+1e-8/std::max(1.,rate));
        const int ix=int(std::floor(x0+dx*probe)),iy=int(std::floor(y0+dy*probe));
        const double tx=dx==0?reach:((dx>0?ix+1:ix)-x0)/dx;
        const double ty=dy==0?reach:((dy>0?iy+1:iy)-y0)/dy;
        const double end=std::min(reach,std::min(tx>t?tx:reach,ty>t?ty:reach));
        if(end<=t) return -1;
        const double fx=x0+dx*t-ix,fy=y0+dy*t-iy;
        const double a=tap(ix,iy),b=tap(ix+1,iy)-a,c=tap(ix,iy+1)-a,d=tap(ix+1,iy+1)-a-b-c;
        const double A=scale*d*dx*dy;
        const double B=scale*(b*dx+c*dy+d*(fx*dy+fy*dx))-rise;
        const double C=scale*(a+b*fx+c*fy+d*fx*fy-reference)-(start+rise*t+bias);
        const double dt=end-t;
        double maximum=std::max(C,(A*dt+B)*dt+C);
        if(A<0)
        {
            const double vertex=-B/(2*A);
            if(vertex>0 && vertex<dt) maximum=std::max(maximum,(A*vertex+B)*vertex+C);
        }
        ++cells;
        if(maximum>1e-9) return 0;
        if(maximum>-1e-9) uncertain=true;
        t=end;
    }
    return cells>=25000 || uncertain?-1:1;
}
}
