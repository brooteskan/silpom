// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace SilPOM
{
struct SampleExtent { std::uint32_t width = 0, height = 0; };
constexpr std::uint32_t SampleColumns(std::uint32_t pixels, std::uint32_t stride)
{
    return pixels / stride + (pixels % stride != 0);
}
constexpr std::uint64_t SampleCount(SampleExtent extent, std::uint32_t stride)
{
    return std::uint64_t(SampleColumns(extent.width, stride)) * SampleColumns(extent.height, stride);
}
// One power-of-two block size across the batch's views. Never drops a view,
// clips its rectangle or exceeds the shared record budget. Zero means no fit.
inline std::uint32_t ChoosePreviewStride(const SampleExtent* extents, std::size_t count, std::uint32_t budget)
{
    for (std::uint32_t stride = 1; ; stride *= 2)
    {
        std::uint64_t used = 0;
        bool fits = true;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto samples = SampleCount(extents[i], stride);
            if (samples > budget - used) { fits = false; break; }
            used += samples;
        }
        if (fits) return stride;
        if (stride > std::numeric_limits<std::uint32_t>::max() / 2) return 0;
    }
}
}
