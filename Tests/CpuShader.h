// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../Include/SilPOM/SurfaceDescriptor.h"
namespace SilPOM::Cpu
{
using uint = std::uint32_t;
using std::min; using std::max; using std::abs; using std::sqrt; using std::floor;
struct float2 { float x = 0, y = 0; float2() = default; float2(float a, float b):x(a),y(b) {} };
struct float3
{
    float x = 0, y = 0, z = 0;
    float3() = default; float3(float a,float b,float c):x(a),y(b),z(c) {}
    float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline float3 operator+(float3 a,float3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
inline float3 operator-(float3 a,float3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
inline float3 operator*(float3 a,float b) { return {a.x*b,a.y*b,a.z*b}; }
inline float dot(float3 a,float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline float3 cross(float3 a,float3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float length(float3 a) { return std::sqrt(dot(a,a)); }
inline float3 min(float3 a,float3 b) { return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)}; }
inline float3 max(float3 a,float3 b) { return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)}; }
inline float3 normalize(float3 a) { return a*(1.f/std::sqrt(dot(a,a))); }
struct Texture
{
    uint width,height;
    std::vector<float> pixels;
    mutable std::uint64_t loads=0;
    float Load(int x,int y) const { ++loads; return pixels[y*width+x]; }
};
struct BoundsTexture
{
    uint width,height;
    std::vector<float2> pixels;
    float2 Load(int x,int y) const { return pixels[y*width+x]; }
};
#define SILPOM_CPU 1
#define SP_REF(T) T&
#define SP_TEX const Texture&
#define SP_FETCH(texture,x,y) texture.Load(x,y)
#define SP_BOUND_TEX const BoundsTexture&
#define SP_FETCH_BOUND(texture,x,y) texture.Load(x,y)
#define SP_ZERO(T) T{}
#include "../Assets/Shaders/SilPOM/Heightfield.azsli"
static_assert(sizeof(SilPomPatch)==sizeof(SilPOM::SurfaceDescriptor));
static_assert(offsetof(SilPomPatch,maxCells)==offsetof(SilPOM::SurfaceDescriptor,maxCells));
#undef SILPOM_CPU
#undef SP_REF
#undef SP_TEX
#undef SP_FETCH
#undef SP_BOUND_TEX
#undef SP_FETCH_BOUND
#undef SP_ZERO
}
