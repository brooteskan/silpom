// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SilPOM::SurfaceBrushShape
{
enum class Shape : uint32_t
{
    Texture=0,
    HardRound=1,
    SoftRound=2,
    HardSquare=3,
    SoftSquare=4,
    RandomEdgeRound=5
};

namespace Detail
{
inline float Smooth(float value)
{
    value=std::clamp(value,0.f,1.f);
    return value*value*(3.f-2.f*value);
}

inline uint64_t Hash(uint64_t value)
{
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ull;
    value=(value^(value>>27))*0x94d049bb133111ebull;
    return value^(value>>31);
}

inline float Lattice(uint32_t x,uint32_t y,uint64_t seed)
{
    const uint64_t bits=Hash(seed+uint64_t(x)*0x9e3779b97f4a7c15ull+uint64_t(y)*0xd1b54a32d192ed03ull);
    return static_cast<float>(bits>>40)*(1.f/16777215.f);
}

// Coordinates are nonnegative, bounded brush UVs scaled by a fixed frequency.
// Interpolating a seeded lattice makes a coherent pattern in brush space;
// sampling another receiver face cannot draw a new random value for the dab.
inline float Noise(float x,float y,uint64_t seed)
{
    const auto ix=static_cast<uint32_t>(x),iy=static_cast<uint32_t>(y);
    const float fx=Smooth(x-static_cast<float>(ix)),fy=Smooth(y-static_cast<float>(iy));
    const float a=Lattice(ix,iy,seed),b=Lattice(ix+1,iy,seed);
    const float c=Lattice(ix,iy+1,seed),d=Lattice(ix+1,iy+1,seed);
    const float lower=a+(b-a)*fx,upper=c+(d-c)*fx;
    return lower+(upper-lower)*fy;
}
}

// UVs describe the entire square brush footprint. Texture leaves source-image
// alpha unchanged. Supply one seed per dab, shared by every affected surface.
inline float Coverage(Shape shape,float u,float v,uint64_t seed=0)
{
    if(!std::isfinite(u)||!std::isfinite(v)||u<0.f||u>1.f||v<0.f||v>1.f)return 0.f;
    const float x=u*2.f-1.f,y=v*2.f-1.f;
    const float radius=std::sqrt(x*x+y*y);
    switch(shape)
    {
    case Shape::Texture:
    case Shape::HardSquare:return 1.f;
    case Shape::HardRound:return radius<=1.f?1.f:0.f;
    case Shape::SoftRound:return 1.f-Detail::Smooth(radius);
    case Shape::SoftSquare:return 1.f-Detail::Smooth(std::max(std::abs(x),std::abs(y)));
    case Shape::RandomEdgeRound:
    {
        constexpr float core=.55f;
        if(radius<=core)return 1.f;
        if(radius>=1.f)return 0.f;
        const float progress=(radius-core)/(1.f-core);
        const float noise=.75f*Detail::Noise(u*22.f,v*22.f,seed)
            +.25f*Detail::Noise(u*47.f,v*47.f,seed^0x632be59bd9b4e019ull);
        // The rising threshold cuts holes and leaves separate islands across
        // the annulus, instead of only perturbing the brush's outside radius.
        // Extend the threshold beyond [0,1] to join the solid core and empty
        // boundary continuously, including lattice extrema of exactly 0 or 1.
        constexpr float feather=.07f;
        const float threshold=progress*(1.f+2.f*feather)-feather;
        return Detail::Smooth((noise-threshold+feather)/(2.f*feather));
    }
    }
    return 0.f;
}
}
