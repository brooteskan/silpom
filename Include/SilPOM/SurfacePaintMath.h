// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace SilPOM::SurfacePaintMath
{
struct Vec3
{
    float x=0,y=0,z=0;
    Vec3()=default;
    Vec3(float a,float b,float c):x(a),y(b),z(c){}
    Vec3 operator+(Vec3 b) const {return {x+b.x,y+b.y,z+b.z};}
    Vec3 operator-(Vec3 b) const {return {x-b.x,y-b.y,z-b.z};}
    Vec3 operator*(float s) const {return {x*s,y*s,z*s};}
};
inline float Dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec3 Cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline bool Finite(Vec3 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
inline Vec3 Unit(Vec3 v)
{
    const float length2=Dot(v,v);
    return std::isfinite(length2)&&length2>1.e-16f?v*(1.f/std::sqrt(length2)):Vec3{0,0,1};
}
inline float Opacity(float a){return std::isfinite(a)?std::clamp(a,0.f,1.f):0.f;}

// Each tuple is authoritative texel data and maps directly to one float4.
// Filter all four components linearly, including at mip boundaries. In
// particular, never normalize the stored normal vector after painting or
// during mip generation: its magnitude retains coverage and cancellation.
struct ColorTexel {Vec3 premultiplied;float coverage=0;};
struct NormalTexel {Vec3 premultiplied;float coverage=0;};
static_assert(sizeof(ColorTexel)==16&&sizeof(NormalTexel)==16);

inline float BlendChannel(float destination,float source,unsigned mode)
{
    switch(mode)
    {
    case 1:return destination*source; // Multiply
    case 2:return std::min(1.f,destination+source); // Bounded Add
    case 3:return 1-(1-destination)*(1-source); // Screen
    case 4:return destination<=.5f?2*destination*source:1-2*(1-destination)*(1-source); // Overlay
    default:return source; // Alpha
    }
}
inline void PaintColor(ColorTexel& texel,Vec3 linearColor,float opacity,unsigned blendMode=0)
{
    const float a=Opacity(opacity);if(a==0||!Finite(linearColor))return;
    linearColor={std::clamp(linearColor.x,0.f,1.f),std::clamp(linearColor.y,0.f,1.f),std::clamp(linearColor.z,0.f,1.f)};
    Vec3 blended=linearColor;
    if(texel.coverage>0&&blendMode)
    {
        const Vec3 destination=texel.premultiplied*(1/texel.coverage);
        blended={BlendChannel(destination.x,linearColor.x,blendMode),BlendChannel(destination.y,linearColor.y,blendMode),BlendChannel(destination.z,linearColor.z,blendMode)};
    }
    // Blends are defined within the isolated paint layer, never against a
    // separately evaluated material. An empty layer receives the source color.
    texel.premultiplied=texel.premultiplied*(1-a)+(linearColor*(1-texel.coverage)+blended*texel.coverage)*a;
    texel.coverage=texel.coverage*(1-a)+a;
}
inline void PaintNormal(NormalTexel& texel,Vec3 receiverDetail,float opacity)
{
    const float a=Opacity(opacity);if(a==0||!Finite(receiverDetail))return;
    // Normal details are in the outward hemisphere of the receiver frame.
    // Decoded flat detail (0,0,1) is neutral for every live material normal.
    receiverDetail.z=std::max(0.f,receiverDetail.z);
    const Vec3 detail=Unit(receiverDetail);
    texel.premultiplied=texel.premultiplied*(1-a)+detail*a;
    texel.coverage=texel.coverage*(1-a)+a;
}
template<class Texel>
inline void Erase(Texel& texel,float opacity)
{
    const float a=Opacity(opacity);if(a==0)return;
    if(a==1){texel={};return;}
    texel.premultiplied=texel.premultiplied*(1-a);
    texel.coverage*=1-a;
}
inline void EraseColor(ColorTexel& texel,float opacity){Erase(texel,opacity);}
inline void EraseNormal(NormalTexel& texel,float opacity){Erase(texel,opacity);}

template<class Texel>
inline Texel Filter(const Texel* values,const float* weights,size_t count)
{
    Texel result;float total=0;
    for(size_t i=0;i<count;++i)
    {
        const float weight=weights[i];if(!std::isfinite(weight)||weight<=0)continue;
        result.premultiplied=result.premultiplied+values[i].premultiplied*weight;
        result.coverage+=values[i].coverage*weight;total+=weight;
    }
    if(total>0&&total!=1)
    {result.premultiplied=result.premultiplied*(1/total);result.coverage/=total;}
    return result;
}
inline ColorTexel FilterColor(const ColorTexel* values,const float* weights,size_t count){return Filter(values,weights,count);}
inline NormalTexel FilterNormal(const NormalTexel* values,const float* weights,size_t count){return Filter(values,weights,count);}
inline Vec3 ComposeColor(Vec3 liveColor,const ColorTexel& paint)
{
    if(paint.coverage<=0)return liveColor;
    return liveColor*(1-paint.coverage)+paint.premultiplied;
}
inline Vec3 RotateDetail(Vec3 detail,float radians)
{
    const float c=std::cos(radians),s=std::sin(radians);
    return {detail.x*c-detail.y*s,detail.x*s+detail.y*c,detail.z};
}
inline Vec3 ComposeNormal(Vec3 liveNormalInReceiverFrame,const NormalTexel& paint)
{
    // Preserve the exact live value for untouched texels and neutral brushes.
    if(paint.coverage<=0||(paint.premultiplied.x==0&&paint.premultiplied.y==0))return liveNormalInReceiverFrame;
    const Vec3 base=Unit(liveNormalInReceiverFrame);
    const Vec3 detail=Unit(paint.premultiplied+Vec3{0,0,1-Opacity(paint.coverage)});
    // The shortest arc from the receiver frame's +Z to the live normal.
    // This is one defined composition, not an affine fit to projected history.
    const Vec3 axis{-base.y,base.x,0};
    if(base.z<=-1.f+1.e-5f)return Unit(Vec3{detail.x,-detail.y,-detail.z});
    return Unit(detail+Cross(axis,detail)+Cross(axis,Cross(axis,detail))*(1/(1+base.z)));
}
}
