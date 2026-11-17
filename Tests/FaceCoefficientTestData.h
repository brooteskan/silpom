// SPDX-License-Identifier: MIT
#pragma once
#define SILPOM_FACE_COEFFICIENT_CACHE 1
#define SILPOM_FACE_COEFFICIENT_CPU 1
#define SILPOM_HIT_DEPTH 1
#include "CpuShader.h"
#include <array>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
namespace SilPOM::Cpu
{
struct float4 {float x=0,y=0,z=0,w=0;float4()=default;float4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){}};
struct uint4 {uint x=0,y=0,z=0,w=0;uint4()=default;uint4(uint a,uint b,uint c,uint d):x(a),y(b),z(c),w(d){}};
}
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100 4244) // Existing CPU shader adapters under HIT_DEPTH.
#endif
#include "BlendedFaceCpu.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
namespace SilPOM::Cpu
{
#define SP_TEX const Texture&
#define SP_REF(T) T&
#include "FaceCoefficientLegacy.azsli"
#undef SP_TEX
#undef SP_REF
#define SILPOM_GRADIENT_CPU 1
#include "../Assets/Shaders/SilPOM/GradientContext.azsli"
#undef SILPOM_GRADIENT_CPU
struct PackedCoefficientFace {float4 blocks[9];};
static_assert(sizeof(PackedCoefficientFace)==144);
static_assert(sizeof(SilPomFaceCoefficients)==48);
inline float3 CoefficientXyz(float4 v){return {v.x,v.y,v.z};}
inline SilPomMeshFace LoadCoefficientFace(const PackedCoefficientFace& p)
{
    SilPomMeshFace f{};f.origin=CoefficientXyz(p.blocks[0]);f.du=CoefficientXyz(p.blocks[1]);
    f.dv=CoefficientXyz(p.blocks[2]);f.normal=CoefficientXyz(p.blocks[3]);
    f.uv0={p.blocks[4].x,p.blocks[4].y};f.uv1={p.blocks[4].z,p.blocks[4].w};f.uv2={p.blocks[5].x,p.blocks[5].y};
    f.d0=CoefficientXyz(p.blocks[6]);f.d1=CoefficientXyz(p.blocks[7]);f.d2=CoefficientXyz(p.blocks[8]);return f;
}
inline PackedCoefficientFace PackCoefficientFace(SilPomMeshFace f)
{
    PackedCoefficientFace p{};
    p.blocks[0]={f.origin.x,f.origin.y,f.origin.z,0};p.blocks[1]={f.du.x,f.du.y,f.du.z,0};
    p.blocks[2]={f.dv.x,f.dv.y,f.dv.z,0};p.blocks[3]={f.normal.x,f.normal.y,f.normal.z,0};
    p.blocks[4]={f.uv0.x,f.uv0.y,f.uv1.x,f.uv1.y};p.blocks[5]={f.uv2.x,f.uv2.y,0,0};
    p.blocks[6]={f.d0.x,f.d0.y,f.d0.z,0};p.blocks[7]={f.d1.x,f.d1.y,f.d1.z,0};p.blocks[8]={f.d2.x,f.d2.y,f.d2.z,0};return p;
}
inline std::vector<PackedCoefficientFace> MakeCoefficientFaces()
{
    std::vector<PackedCoefficientFace> result;std::mt19937 rng(13071);
    std::uniform_real_distribution<float> unit(-1,1),positive(.02f,3);
    for(unsigned i=0;i<2048;++i)
    {
        SilPomMeshFace f{};f.origin={unit(rng)*10,unit(rng)*10,unit(rng)*10};
        f.du={positive(rng),unit(rng)*.2f,unit(rng)*.2f};f.dv={unit(rng)*.2f,positive(rng),unit(rng)*.2f};
        f.normal=normalize(cross(f.du,f.dv));f.uv0={unit(rng),unit(rng)};
        f.uv1={f.uv0.x+positive(rng),f.uv0.y+.02f*unit(rng)};
        f.uv2={f.uv0.x+.02f*unit(rng),f.uv0.y+positive(rng)};
        if(i%2)std::swap(f.uv1,f.uv2);
        f.d0={unit(rng)*.3f,unit(rng)*.3f,positive(rng)};
        f.d1=f.d0+float3{unit(rng)*.1f,unit(rng)*.1f,unit(rng)*.1f};
        f.d2=f.d0+float3{unit(rng)*.1f,unit(rng)*.1f,unit(rng)*.1f};
        result.push_back(PackCoefficientFace(f));
    }
    // Preserve raw signed area and distinguish Newton from gradient guards.
    for(float area:{0.f,1e-30f,-1e-30f,1e-20f,-1e-20f,1e-10f,-1e-10f})
    {
        auto f=LoadCoefficientFace(result[0]);f.uv0={0,0};f.uv1={1,0};f.uv2={0,area};
        f.d0={0,0,1};f.d1={.1f,0,1};f.d2={0,.1f,1};result.push_back(PackCoefficientFace(f));
    }
    return result;
}
inline void AppendPackedCoefficientFaces(std::vector<PackedCoefficientFace>& faces,const char* path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open packed face fixture");
    const auto bytes=file.tellg();if(bytes<=0||bytes%144!=0)throw std::runtime_error("Packed face fixture must use canonical144B records");
    const auto begin=faces.size();faces.resize(begin+std::size_t(bytes)/144);file.seekg(0);
    file.read(reinterpret_cast<char*>(faces.data()+begin),bytes);if(!file)throw std::runtime_error("Cannot read packed face fixture");
}
}
