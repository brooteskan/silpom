// SPDX-License-Identifier: MIT
#pragma once
#include "CurvedPrototype.h"
#include <cstring>
#include <stdexcept>

namespace SilPOM::Curved
{
// Experimental transport, deliberately not a cooked asset ABI. Geometry is
// shared per source triangle; each cell fragment stores only its parameter
// domain and the four coefficients of that cell's bilinear height polynomial.
struct PackedFloat4 { float x, y, z, w; };
static_assert(sizeof(PackedFloat4) == 16);
inline constexpr size_t PackedTriangleStride = 8;
inline constexpr size_t PackedFragmentStride = 4;

inline float PackInteger(uint value)
{
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

struct PackedMesh
{
    std::vector<PackedFloat4> data;
    uint triangleCount = 0;
    uint fragmentCount = 0;
    uint fragmentStride = uint(PackedFragmentStride);

    uint ShaderHeader(bool fineSubdivision=false) const
    { return triangleCount | (fragmentStride == 5 ? 0x80000000u : 0u) | (fineSubdivision ? 0x40000000u : 0u); }

    std::vector<PackedFloat4> ReversedFragments() const
    {
        const size_t prefix = size_t(triangleCount) * PackedTriangleStride;
        std::vector<PackedFloat4> result(data.begin(), data.begin() + prefix);
        for (size_t i = fragmentCount; i-- > 0;)
            result.insert(result.end(), data.begin() + prefix + i * fragmentStride,
                data.begin() + prefix + (i + 1) * fragmentStride);
        return result;
    }
};

inline std::array<Vec3,10> AnalyticControls(const Fragment&,const Surface&,const Texture&);

inline PackedMesh PackMesh(const std::vector<Triangle>& triangles, const Surface& surface, const Texture& texture,
    size_t maxCellsPerTriangle = 1048576, bool includeBounds = false)
{
    PackedMesh result;
    result.fragmentStride = includeBounds ? 5u : 4u;
    if(triangles.empty()||triangles.size()>0x1fffffffu||!std::isfinite(float(surface.baseScale))||
        !std::isfinite(float(surface.amplitude)))
        throw std::invalid_argument("Mesh or profile exceeds GPU transport limits");
    const Ray validationRay{ { 0, 0, 0 }, { 0, 0, 1 }, 0, 1 };
    for (const Triangle& triangle : triangles)
    {
        if (!Valid(triangle, surface, texture, validationRay))
            throw std::invalid_argument("Invalid curved mesh packing input");
        double minX=1e300,minY=1e300,maxX=-1e300,maxY=-1e300;
        for(size_t i=0;i!=3;++i)
        {
            const Vec3 p=triangle.position[i];
            if(!std::isfinite(float(p.x))||!std::isfinite(float(p.y))||!std::isfinite(float(p.z)))
                throw std::invalid_argument("Position exceeds float transport range");
            const double x=triangle.uv[i].x*texture.width-.5,y=triangle.uv[i].y*texture.height-.5;
            if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>16777215||std::abs(y)>16777215)
                throw std::invalid_argument("Cell identity exceeds exact float transport range");
            minX=std::min(minX,std::floor(x));maxX=std::max(maxX,std::floor(x));
            minY=std::min(minY,std::floor(y));maxY=std::max(maxY,std::floor(y));
        }
        if((maxX-minX+1)*(maxY-minY+1)>double(maxCellsPerTriangle))
            throw std::invalid_argument("Cell expansion exceeds explicit preprocessing budget");
    }
    for (double height : texture.pixels)
        if (!std::isfinite(height) || height < 0 || height > 1)
            throw std::invalid_argument("Height must be finite and normalized");
    auto pack = [](Vec3 value) { return PackedFloat4{ float(value.x), float(value.y), float(value.z), 0 }; };
    result.triangleCount = uint(triangles.size());
    for (const Triangle& triangle : triangles)
    {
        for (Vec3 value : triangle.position) result.data.push_back(pack(value));
        for (Vec3 value : triangle.direction) result.data.push_back(pack(value));
        result.data.push_back({ float(triangle.uv[0].x), float(triangle.uv[0].y),
            float(triangle.uv[1].x), float(triangle.uv[1].y) });
        result.data.push_back({ float(triangle.uv[2].x), float(triangle.uv[2].y), PackInteger(triangle.primitiveId), 0 });
    }
    for (uint index = 0; index != result.triangleCount; ++index)
    {
        for (const Fragment& fragment : BuildFragments(triangles[index], texture))
        {
            if (std::abs(double(fragment.cellX)) > 16777216 || std::abs(double(fragment.cellY)) > 16777216)
                throw std::invalid_argument("Cell identity exceeds exact float transport range");
            const auto& b = fragment.domain;
            const double h00 = texture.Fetch(fragment.cellX, fragment.cellY, surface.addressMode);
            const double h10 = texture.Fetch(fragment.cellX + 1, fragment.cellY, surface.addressMode);
            const double h01 = texture.Fetch(fragment.cellX, fragment.cellY + 1, surface.addressMode);
            const double h11 = texture.Fetch(fragment.cellX + 1, fragment.cellY + 1, surface.addressMode);
            result.data.push_back({ float(b[0].y), float(b[0].z), float(b[1].y), float(b[1].z) });
            result.data.push_back({ float(b[2].y), float(b[2].z), PackInteger(index), 0 });
            result.data.push_back({ float(h00), float(h10 - h00), float(h01 - h00), float(h11 - h10 - h01 + h00) });
            if(includeBounds)
            {
                Vec3 lo{1e300,1e300,1e300},hi{-1e300,-1e300,-1e300};
                for(Vec3 control:AnalyticControls(fragment,surface,texture))for(int axis=0;axis!=3;++axis)
                {lo[axis]=std::min(lo[axis],control[axis]);hi[axis]=std::max(hi[axis],control[axis]);}
                for(int axis=0;axis!=3;++axis)
                {
                    const double error=64*std::numeric_limits<float>::epsilon()*std::max({1.,std::abs(lo[axis]),std::abs(hi[axis])});
                    lo[axis]=std::nextafter(float(lo[axis]-error),-std::numeric_limits<float>::infinity());
                    hi[axis]=std::nextafter(float(hi[axis]+error),std::numeric_limits<float>::infinity());
                    if(!std::isfinite(lo[axis])||!std::isfinite(hi[axis]))
                        throw std::invalid_argument("Resolved bounds exceed float transport range");
                }
                result.data.push_back({float(fragment.cellX),float(fragment.cellY),float(lo.x),float(lo.y)});
                result.data.push_back({float(lo.z),float(hi.x),float(hi.y),float(hi.z)});
            }
            else result.data.push_back({ float(fragment.cellX), float(fragment.cellY), 0, 0 });
            ++result.fragmentCount;
        }
    }
    return result;
}

// Algebraic counterpart to the shader's quadratic-height/linear-direction
// product. Unlike sampled reconstruction, this does not subtract nearly equal
// point samples to recover the control net of very small child domains.
inline std::array<Vec3,10> AnalyticControls(const Fragment& fragment,const Surface& surface,const Texture& texture)
{
    const Triangle& triangle=*fragment.triangle;
    std::array<Vec3,3> p,d;
    std::array<Vec2,3> q;
    for(size_t i=0;i!=3;++i)
    {
        p[i]=Interpolate(triangle.position,fragment.domain[i])*surface.baseScale;
        d[i]=Interpolate(triangle.direction,fragment.domain[i]);
        const Vec2 uv=Interpolate(triangle.uv,fragment.domain[i]);
        q[i]={uv.x*texture.width-.5-fragment.cellX,uv.y*texture.height-.5-fragment.cellY};
    }
    const double h00=texture.Fetch(fragment.cellX,fragment.cellY,surface.addressMode);
    const double hx=texture.Fetch(fragment.cellX+1,fragment.cellY,surface.addressMode)-h00;
    const double hy=texture.Fetch(fragment.cellX,fragment.cellY+1,surface.addressMode)-h00;
    const double hxy=texture.Fetch(fragment.cellX+1,fragment.cellY+1,surface.addressMode)-h00-hx-hy;
    auto height=[&](size_t i,size_t j)
    {
        return surface.amplitude*(h00-surface.reference+.5*hx*(q[i].x+q[j].x)+
            .5*hy*(q[i].y+q[j].y)+.5*hxy*(q[i].x*q[j].y+q[j].x*q[i].y));
    };
    const double h0=height(0,0),h1=height(1,1),h2=height(2,2);
    const double h01=height(0,1),h12=height(1,2),h20=height(2,0);
    return {{p[0]+d[0]*h0,p[1]+d[1]*h1,p[2]+d[2]*h2,
        (p[0]*2+p[1]+d[1]*h0+d[0]*(2*h01))/3,
        (p[0]+p[1]*2+d[0]*h1+d[1]*(2*h01))/3,
        (p[1]*2+p[2]+d[2]*h1+d[1]*(2*h12))/3,
        (p[1]+p[2]*2+d[1]*h2+d[2]*(2*h12))/3,
        (p[2]*2+p[0]+d[0]*h2+d[2]*(2*h20))/3,
        (p[2]+p[0]*2+d[2]*h0+d[0]*(2*h20))/3,
        (p[0]+p[1]+p[2]+d[2]*h01+d[0]*h12+d[1]*h20)/3}};
}
}
