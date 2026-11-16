// SPDX-License-Identifier: MIT
#pragma once
#include "DecalCompositeMath.h"

namespace SilPOM::PaintMath
{
// 3 spatial dimensions, four independent height selections, tiled in a 2D
// image. A one-texel gutter around every slice prevents cross-slice filtering.
struct BakeGrid
{
    uint32_t x=2,y=2,z=2,bands=4,columns=1,width=4,height=4;
    uint64_t Bytes() const{return uint64_t(width)*height*32;}// four RGBA16F images
    void Layout()
    {
        columns=std::clamp(uint32_t(std::ceil(std::sqrt(float(z*bands)*float(y+2)/float(x+2)))),1u,z*bands);
        width=(x+2)*columns;height=(y+2)*((z*bands+columns-1)/columns);
    }
};
inline BakeGrid MakeBakeGrid(const Bounds& bounds,float density,uint64_t budget,uint32_t bands=4)
{
    const Vec span=bounds.high-bounds.low;
    BakeGrid g;g.bands=bands;
    g.x=uint32_t(std::clamp(std::ceil(span.x*density)+1.f,2.f,512.f));
    g.y=uint32_t(std::clamp(std::ceil(span.y*density)+1.f,2.f,512.f));
    g.z=uint32_t(std::clamp(std::ceil(span.z*density)+1.f,2.f,512.f));
    g.Layout();
    while(g.Bytes()>budget&&(g.x>2||g.y>2||g.z>2))
    {
        const float rx=g.x>2?(g.x-1)/std::max(span.x,1e-6f):0;
        const float ry=g.y>2?(g.y-1)/std::max(span.y,1e-6f):0;
        const float rz=g.z>2?(g.z-1)/std::max(span.z,1e-6f):0;
        if(rx>=ry&&rx>=rz&&g.x>2)--g.x;
        else if(ry>=rz&&g.y>2)--g.y;
        else if(g.z>2)--g.z;
        g.Layout();
    }
    return g;
}
inline bool BakeBlendSupported(unsigned blend){return blend==0||blend==1||blend==3;}
// Bound on the dependence of normalize(lerp(n,m,a)) on an unknown unit n.
// Normalizing has Lipschitz constant <=1/minLength; minLength>=|1-2a|.
// Saturation is certified instead of assuming that blended normal maps compose
// affinely. Uncertified normals retain ordered projection evaluation.
inline float NormalDependence(float previous,float alpha)
{
    if(alpha<=0)return previous;
    if(alpha>=1)return 0;
    const float denominator=std::abs(1-2*alpha)-.002f;
    return denominator<1e-6f?2.f:std::min(2.f,previous*(1-alpha)/denominator+.002f);
}
// Interval version includes uncertainty in attenuation as well as the incoming
// normal. Used by tests against brute-force normal chains, matching the shader.
inline float NormalIntervalDependence(float previous,float alpha,float low,float high)
{
    if(high<=0)return previous;
    if(low>=1)return 0;
    if(low<=.501f&&high>=.499f)return 2;
    const float denominator=std::min(std::abs(1-2*low),std::abs(1-2*high))-.002f;
    const float delta=std::max(alpha-low,high-alpha);
    return denominator<=0?2.f:std::min(2.f,((1-low)*previous+2*delta)/denominator+.002f);
}
inline float NormalConeDependence(float previous,float alpha,float low,float high)
{
    if(high<=0)return previous;
    if(low>=1)return 0;
    const float middle=std::clamp(.5f,low,high);
    const float denominator=std::sqrt(1-2*middle*(1-middle))-.002f;
    return std::min(2.f,((1-low)*previous+2*std::max(alpha-low,high-alpha))/denominator+.002f);
}
inline void BakeColor(float color,float alpha,unsigned blend,bool erase,float& scale,float& bias)
{
    if(erase){scale=scale*(1-alpha)+alpha;bias*=1-alpha;}
    else if(blend==1){const float f=1-alpha+alpha*color;scale*=f;bias*=f;}
    else if(blend==3){const float f=1-alpha*color;scale*=f;bias=bias*f+alpha*color;}
    else {scale*=1-alpha;bias=bias*(1-alpha)+alpha*color;}
}
}
