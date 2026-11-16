// SPDX-License-Identifier: MIT
#pragma once
#include "SurfacePaintImage.h"

namespace SilPOM::SurfacePaintLayout
{
inline constexpr uint32_t MaxCharts=128;
inline constexpr uint64_t MaxBytes=256ull*1024*1024;
enum class Error {None,Resolution,Charts,Bytes};
struct Usage
{
    uint32_t charts=0;
    uint64_t bytes=0;
    Error Add(uint32_t resolution)
    {
        if(resolution<SurfacePaintImage::MinResolution||resolution>SurfacePaintImage::MaxResolution
            ||(resolution&(resolution-1u)))return Error::Resolution;
        uint64_t added=0;
        for(uint32_t size=resolution;size;size/=2)added+=uint64_t(size)*size*sizeof(SurfacePaintImage::Pixel);
        if(charts>=MaxCharts)return Error::Charts;
        if(added>MaxBytes||bytes>MaxBytes-added)return Error::Bytes;
        ++charts;bytes+=added;return Error::None;
    }
};
}
