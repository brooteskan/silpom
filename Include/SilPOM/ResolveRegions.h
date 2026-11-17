// SPDX-License-Identifier: MIT
#pragma once
#include "ResolveRegion.h"
#include <vector>

namespace SilPOM::ResolveRegions
{
using ResolveRegion::Clip;
using ResolveRegion::Rect;
using ResolveRegion::Viewport;

inline Rect DrawableLimit(const Viewport& viewport, const Rect& scissor,
    uint32_t targetWidth, uint32_t targetHeight)
{
    // A nonprojectable box requests the existing helper's complete drawable
    // limit, retaining its exact viewport/MSAA edge and integer clamp policy.
    return ResolveRegion::Compute({}, viewport, scissor, targetWidth, targetHeight);
}

// Clip a small published coverage bound to the near half-space before dividing
// by W. Unlike the batch-wide fallback, a box crossing the near plane need not
// occupy the entire screen. Far clipping is deliberately omitted after a
// conservative whole-volume rejection, so it can only make the rectangle loose.
inline Rect ComputeClipped(const std::array<Clip, 8>& corners, const Viewport& viewport,
    const Rect& scissor, uint32_t targetWidth, uint32_t targetHeight,
    const std::array<Clip, 8>& clipErrors = {})
{
    const Rect limit = DrawableLimit(viewport, scissor, targetWidth, targetHeight);
    if (limit.Empty()) return {};
    Clip error{};
    double magnitude = 1;
    for (size_t i = 0; i < corners.size(); ++i)
    {
        const auto& point = corners[i];
        const auto& uncertainty = clipErrors[i];
        const double components[4] = {point.x, point.y, point.z, point.w};
        const double errors[4] = {uncertainty.x, uncertainty.y, uncertainty.z, uncertainty.w};
        for (unsigned axis = 0; axis < 4; ++axis)
        {
            if (!std::isfinite(components[axis]) || !std::isfinite(errors[axis]) || errors[axis] < 0)
                return limit;
            magnitude = std::max(magnitude, std::abs(components[axis]) + errors[axis]);
        }
        error.x = std::max(error.x, uncertainty.x);
        error.y = std::max(error.y, uncertainty.y);
        error.z = std::max(error.z, uncertainty.z);
        error.w = std::max(error.w, uncertainty.w);
    }
    // This is an engineering allowance for the double-precision plane tests,
    // edge interpolation and division, separate from the supplied GPU float
    // transform error. Uncertain arithmetic retains the full drawable limit.
    const double guard = magnitude * 1e-10;
    if (!std::isfinite(guard)) return limit;
    error.x += guard; error.y += guard; error.z += guard; error.w += guard;

    std::array<double, 8> nearDistance{};
    double farMaximum = -std::numeric_limits<double>::infinity();
    double nearMaximum = farMaximum;
    for (size_t i = 0; i < corners.size(); ++i)
    {
        const auto& point = corners[i];
        farMaximum = std::max(farMaximum, point.z + error.z);
        // Any true inside-near point has a nominal position in this relaxed
        // half-space: nominal(W-Z) >= -(error.W+error.Z).
        nearDistance[i] = point.w - point.z + error.w + error.z + guard;
        nearMaximum = std::max(nearMaximum, nearDistance[i]);
    }
    if (farMaximum < -guard || nearMaximum < -guard) return {};

    double lowX = std::numeric_limits<double>::infinity();
    double lowY = lowX, highX = -lowX, highY = -lowX;
    bool ambiguous = false, any = false;
    const auto include = [&](const Clip& point)
    {
        const double lowW = point.w - error.w;
        const double highW = point.w + error.w;
        if (!std::isfinite(lowW) || !std::isfinite(highW) || lowW <= guard)
        {
            ambiguous = true;
            return;
        }
        for (const double w : {lowW, highW})
            for (const double sign : {-1., 1.})
            {
                const double x = (point.x + sign * error.x) / w;
                const double y = (point.y + sign * error.y) / w;
                if (!std::isfinite(x) || !std::isfinite(y))
                {
                    ambiguous = true;
                    return;
                }
                lowX = std::min(lowX, x); highX = std::max(highX, x);
                lowY = std::min(lowY, y); highY = std::max(highY, y);
            }
        any = true;
    };
    for (size_t i = 0; i < corners.size(); ++i)
        if (nearDistance[i] >= 0) include(corners[i]);

    // A convex hull clipped by one half-space has only retained vertices and
    // intersections on its original edges. Testing every pair is a superset
    // of those edges, even if rounded corner values are not exactly affine.
    // No cap faces or intersection of multiple clip planes can be missed.
    for (size_t i = 0; i < corners.size(); ++i)
        for (size_t j = i + 1; j < corners.size(); ++j)
        {
            const double first = nearDistance[i], second = nearDistance[j];
            if ((first < 0) == (second < 0)) continue;
            const double denominator = first - second;
            if (!std::isfinite(denominator) || denominator == 0) return limit;
            const double t = first / denominator;
            if (!std::isfinite(t) || t < 0 || t > 1) return limit;
            const auto& a = corners[i]; const auto& b = corners[j];
            include({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
        }
    if (ambiguous || !any) return limit;

    // Reuse the original pixel/raster/repair/derivative padding and absolute
    // quad alignment after computing a conservative NDC interval.
    std::array<Clip, 8> projected;
    for (size_t i = 0; i < projected.size(); ++i)
        projected[i] = {(i & 1) ? highX : lowX, (i & 2) ? highY : lowY, .5, 1};
    return ResolveRegion::Compute(projected, viewport, scissor, targetWidth, targetHeight);
}

namespace Internal
{
inline int64_t FloorDiv(int64_t value, int64_t divisor)
{
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}

struct Run { size_t begin = 0, end = 0, rectangle = 0; };

inline std::vector<Rect> GridUnion(const std::vector<Rect>& inputs, const Rect& limit, int64_t cell)
{
    const int64_t minColumn = FloorDiv(limit.minX, cell), minRow = FloorDiv(limit.minY, cell);
    const int64_t maxColumn = FloorDiv(int64_t(limit.maxX) - 1, cell) + 1;
    const int64_t maxRow = FloorDiv(int64_t(limit.maxY) - 1, cell) + 1;
    const size_t columns = static_cast<size_t>(maxColumn - minColumn);
    const size_t rows = static_cast<size_t>(maxRow - minRow);
    std::vector<uint8_t> occupied(columns * rows, 0);
    for (const auto& input : inputs)
    {
        const auto rect = ResolveRegion::Intersect(input, limit);
        if (rect.Empty()) continue;
        const size_t x0 = static_cast<size_t>(FloorDiv(rect.minX, cell) - minColumn);
        const size_t y0 = static_cast<size_t>(FloorDiv(rect.minY, cell) - minRow);
        const size_t x1 = static_cast<size_t>(FloorDiv(int64_t(rect.maxX) - 1, cell) - minColumn + 1);
        const size_t y1 = static_cast<size_t>(FloorDiv(int64_t(rect.maxY) - 1, cell) - minRow + 1);
        for (size_t y = y0; y < y1; ++y)
            std::fill(occupied.begin() + y * columns + x0, occupied.begin() + y * columns + x1, uint8_t(1));
    }
    std::vector<Rect> rectangles;
    std::vector<Run> previous, current;
    for (size_t row = 0; row < rows; ++row)
    {
        current.clear();
        size_t prior = 0;
        for (size_t column = 0; column < columns;)
        {
            if (!occupied[row * columns + column]) { ++column; continue; }
            const size_t begin = column;
            while (column < columns && occupied[row * columns + column]) ++column;
            while (prior < previous.size() && previous[prior].begin < begin) ++prior;
            const int64_t bottom = (minRow + static_cast<int64_t>(row) + 1) * cell;
            if (prior < previous.size() && previous[prior].begin == begin && previous[prior].end == column)
            {
                const size_t index = previous[prior].rectangle;
                rectangles[index].maxY = static_cast<int32_t>(std::min(bottom, int64_t(limit.maxY)));
                current.push_back({begin, column, index});
            }
            else
            {
                const int64_t left = (minColumn + static_cast<int64_t>(begin)) * cell;
                const int64_t right = (minColumn + static_cast<int64_t>(column)) * cell;
                const int64_t top = (minRow + static_cast<int64_t>(row)) * cell;
                rectangles.push_back({static_cast<int32_t>(std::max(left, int64_t(limit.minX))),
                    static_cast<int32_t>(std::max(top, int64_t(limit.minY))),
                    static_cast<int32_t>(std::min(right, int64_t(limit.maxX))),
                    static_cast<int32_t>(std::min(bottom, int64_t(limit.maxY)))});
                current.push_back({begin, column, rectangles.size() - 1});
            }
        }
        previous.swap(current);
    }
    return rectangles;
}
}

// Inputs already contain repair and derivative support. Coarsening the union
// adds coverage only; it never clips that support. Rectangles partition an
// absolute even-pixel grid, so separate draws never shade one output sample
// twice and every internal boundary falls between complete derivative quads.
inline std::vector<Rect> Merge(const std::vector<Rect>& inputs, const Rect& limit,
    size_t rectangleBudget = 32, uint32_t cellPixels = 32)
{
    if (limit.Empty() || inputs.empty()) return {};
    rectangleBudget = std::max(size_t(1), rectangleBudget);
    int64_t cell = std::max(int64_t(2), (int64_t(cellPixels) + 1) & ~int64_t(1));
    constexpr uint64_t maximumCells = 262144;
    for (;;)
    {
        const uint64_t columns = static_cast<uint64_t>(Internal::FloorDiv(int64_t(limit.maxX) - 1, cell)
            - Internal::FloorDiv(limit.minX, cell) + 1);
        const uint64_t rows = static_cast<uint64_t>(Internal::FloorDiv(int64_t(limit.maxY) - 1, cell)
            - Internal::FloorDiv(limit.minY, cell) + 1);
        if (columns <= maximumCells && rows <= maximumCells && columns * rows <= maximumCells)
        {
            auto rectangles = Internal::GridUnion(inputs, limit, cell);
            if (rectangles.size() <= rectangleBudget) return rectangles;
        }
        if (cell >= (int64_t(1) << 33))
        {
            // A limit crossing absolute coordinate zero can retain multiple
            // cells forever. A single conservative rectangle is always safe.
            for (const auto& input : inputs)
            {
                const auto rect = ResolveRegion::Intersect(input, limit);
                if (!rect.Empty()) return {limit};
            }
            // The full limit also retains quad partners when input edges are
            // unaligned. This exceptional path is only for enormous domains.
            return {};
        }
        cell *= 2;
    }
}
}
