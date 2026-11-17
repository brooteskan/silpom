// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace SilPOM::ResolveRegion
{
struct Clip { double x, y, z, w; };
struct Rect
{
    int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool Empty() const { return minX >= maxX || minY >= maxY; }
};
struct Viewport { double minX, minY, maxX, maxY; };

// Includes the 1e-5 NDC coverage rectangle and the rotated wall proxy's
// 2e-5 padding on each of its two orthogonal axes.
inline constexpr double RasterPaddingNdc = 4e-5;
// Classification reads the original image and never cascades repairs.
inline constexpr int RepairPixels = 2;
// Keep a complete additional derivative quad around possible owned pixels.
inline constexpr int DerivativeGuardPixels = 2;

inline Rect Intersect(const Rect& a, const Rect& b)
{
    Rect result{std::max(a.minX, b.minX), std::max(a.minY, b.minY),
        std::min(a.maxX, b.maxX), std::min(a.maxY, b.maxY)};
    return result.Empty() ? Rect{} : result;
}

// Corners must enclose EVERY record producer in the published generation:
// displaced extrema plus the ray/AABB numerical padding, base/proxy geometry,
// and recessed backup shells. They use the actual view's (jittered) shader
// projection. clipErrors bounds absolute CPU/GPU transform differences in each
// homogeneous coordinate, including cancellation before projection. The
// viewport stays unchanged; the returned rect is only a scissor.
// D3D/Atom screen Y points down. Rect maxima are exclusive, like RHI::Scissor.
inline Rect Compute(const std::array<Clip, 8>& corners, const Viewport& viewport,
    const Rect& scissor, uint32_t targetWidth, uint32_t targetHeight,
    const std::array<Clip, 8>& clipErrors = {})
{
    const auto boundedSize = [](uint32_t value)
    {
        return static_cast<int32_t>(std::min(value,
            static_cast<uint32_t>(std::numeric_limits<int32_t>::max())));
    };
    const Rect target{0, 0, boundedSize(targetWidth), boundedSize(targetHeight)};
    Rect limit = Intersect(target, scissor);
    if (limit.Empty()) return {};
    const bool validViewport = std::isfinite(viewport.minX) && std::isfinite(viewport.minY)
        && std::isfinite(viewport.maxX) && std::isfinite(viewport.maxY)
        && viewport.minX < viewport.maxX && viewport.minY < viewport.maxY;
    if (!validViewport) return limit;

    // Clamp before integer conversion, so offscreen/huge coordinates cannot
    // overflow. Fractional viewport edges can cover partial MSAA pixels.
    const auto clampX = [&](double value)
    { return std::clamp(value, double(limit.minX), double(limit.maxX)); };
    const auto clampY = [&](double value)
    { return std::clamp(value, double(limit.minY), double(limit.maxY)); };
    limit = Intersect(limit, Rect{static_cast<int32_t>(std::floor(clampX(viewport.minX))),
        static_cast<int32_t>(std::floor(clampY(viewport.minY))),
        static_cast<int32_t>(std::ceil(clampX(viewport.maxX))),
        static_cast<int32_t>(std::ceil(clampY(viewport.maxY)))});
    if (limit.Empty()) return {};

    double lowX = std::numeric_limits<double>::infinity();
    double lowY = lowX, highX = -lowX, highY = -lowX;
    for (size_t i = 0; i < corners.size(); ++i)
    {
        const auto& corner = corners[i];
        const auto& error = clipErrors[i];
        if (!std::isfinite(corner.x) || !std::isfinite(corner.y)
            || !std::isfinite(corner.z) || !std::isfinite(corner.w)) return limit;
        if (!std::isfinite(error.x) || !std::isfinite(error.y)
            || !std::isfinite(error.z) || !std::isfinite(error.w)
            || error.x < 0 || error.y < 0 || error.z < 0 || error.w < 0) return limit;
        // Atom uses reverse Z: near is w-z=0, far is z=0. Projecting only
        // corners across either cut does not bound the clipped convex volume.
        // Ambiguous eye/clip intersections conservatively retain the viewport.
        const double planeGuard = 1e-5 * std::max(1.0, std::abs(corner.w));
        if (corner.w - error.w <= planeGuard || corner.z - error.z <= planeGuard
            || corner.w - error.w - corner.z - error.z <= planeGuard) return limit;
        // Interval division accounts for either sign of X/Y. In particular,
        // large translated objects can have cancellation error even when the
        // resulting clip coordinate is small.
        for (const double w : {corner.w - error.w, corner.w + error.w})
            for (const double sign : {-1.0, 1.0})
            {
                const double x = (corner.x + sign * error.x) / w;
                const double y = (corner.y + sign * error.y) / w;
                if (!std::isfinite(x) || !std::isfinite(y)) return limit;
                lowX = std::min(lowX, x); highX = std::max(highX, x);
                lowY = std::min(lowY, y); highY = std::max(highY, y);
            }
    }

    const double width = viewport.maxX - viewport.minX;
    const double height = viewport.maxY - viewport.minY;
    if (!std::isfinite(width) || !std::isfinite(height)) return limit;
    constexpr double pixels = RepairPixels + DerivativeGuardPixels;
    const double minX = viewport.minX + (lowX - RasterPaddingNdc + 1.0) * width * .5 - pixels;
    const double maxX = viewport.minX + (highX + RasterPaddingNdc + 1.0) * width * .5 + pixels;
    const double minY = viewport.minY + (1.0 - highY - RasterPaddingNdc) * height * .5 - pixels;
    const double maxY = viewport.minY + (1.0 - lowY + RasterPaddingNdc) * height * .5 + pixels;
    if (!std::isfinite(minX) || !std::isfinite(maxX)
        || !std::isfinite(minY) || !std::isfinite(maxY)) return limit;

    // The quad grid is anchored at absolute target pixels, independent of
    // viewport origin. Clip after alignment; odd target edges are valid.
    const auto lower = [](double value) { return std::floor(value * .5) * 2.0; };
    const auto upper = [](double value) { return std::ceil(value * .5) * 2.0; };
    return Intersect(limit, Rect{static_cast<int32_t>(clampX(lower(minX))),
        static_cast<int32_t>(clampY(lower(minY))), static_cast<int32_t>(clampX(upper(maxX))),
        static_cast<int32_t>(clampY(upper(maxY)))});
}
}
