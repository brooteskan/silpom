// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/ResolveRegions.h"
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

using namespace SilPOM::ResolveRegions;
namespace ResolveRegion = SilPOM::ResolveRegion;

namespace
{
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
bool Contains(const Rect& rect, int32_t x, int32_t y)
{
    return x >= rect.minX && x < rect.maxX && y >= rect.minY && y < rect.maxY;
}
bool Same(const Rect& a, const Rect& b)
{
    return a.minX == b.minX && a.minY == b.minY && a.maxX == b.maxX && a.maxY == b.maxY;
}
std::array<Clip, 8> ProjectedBox(double x0, double y0, double z0, double x1, double y1, double z1)
{
    std::array<Clip, 8> result;
    for (size_t i = 0; i < result.size(); ++i)
    {
        const double depth = (i & 4) ? z1 : z0;
        // Reverse-Z perspective: near=.2, far=1000, W=positive view depth.
        result[i] = {(i & 1) ? x1 : x0, (i & 2) ? y1 : y0,
            .20004000800160032 - .00020004000800160032 * depth, depth};
    }
    return result;
}
void CheckUnion(const std::vector<Rect>& inputs, const Rect& limit, size_t budget, uint32_t cell)
{
    const auto output = Merge(inputs, limit, budget, cell);
    Require(output.size() <= std::max(size_t(1), budget), "Rectangle budget exceeded");
    for (size_t i = 0; i < output.size(); ++i)
    {
        const auto& rect = output[i];
        Require(!rect.Empty() && Same(ResolveRegion::Intersect(rect, limit), rect), "Output outside limit");
        Require(rect.minX == limit.minX || rect.minX % 2 == 0, "Unaligned left edge");
        Require(rect.minY == limit.minY || rect.minY % 2 == 0, "Unaligned top edge");
        Require(rect.maxX == limit.maxX || rect.maxX % 2 == 0, "Unaligned right edge");
        Require(rect.maxY == limit.maxY || rect.maxY % 2 == 0, "Unaligned bottom edge");
        for (size_t j = 0; j < i; ++j)
            Require(ResolveRegion::Intersect(rect, output[j]).Empty(), "Output rectangles overlap");
    }
    for (int32_t y = limit.minY; y < limit.maxY; ++y)
        for (int32_t x = limit.minX; x < limit.maxX; ++x)
        {
            bool expected = false;
            for (const auto& input : inputs) expected = expected || Contains(input, x, y);
            if (!expected) continue;
            unsigned count = 0;
            for (const auto& rect : output) count += Contains(rect, x, y) ? 1u : 0u;
            Require(count == 1, "Input support missing or drawn twice");
        }
}
}

int main()
{
    try
    {
        const Viewport viewport{0, 0, 801, 603};
        const Rect limit{0, 0, 801, 603};
        auto box = ProjectedBox(-.01, -.01, -.1, .01, .01, 2);
        auto rect = ComputeClipped(box, viewport, limit, 801, 603);
        Require(!rect.Empty() && !Same(rect, limit), "Ordinary eye/near crossing did not tighten");
        Require(Contains(rect, 400, 301), "Clipped center missing");
        Require(ComputeClipped(ProjectedBox(-1, -1, -.3, 1, 1, .1), viewport, limit, 801, 603).Empty(),
            "Wholly before-near box was not rejected");
        Require(ComputeClipped(ProjectedBox(-1, -1, 1100, 1, 1, 1200), viewport, limit, 801, 603).Empty(),
            "Wholly beyond-far box was not rejected");
        box[0].x = std::numeric_limits<double>::quiet_NaN();
        Require(Same(ComputeClipped(box, viewport, limit, 801, 603), limit), "Invalid projection did not fall back");
        box.fill({0, 0, 0, 0});
        Require(Same(ComputeClipped(box, viewport, limit, 801, 603), limit), "Ambiguous W did not fall back");
        box = ProjectedBox(-.01, -.01, .5, .01, .01, 2);
        std::array<Clip, 8> errors{};
        errors[0].w = -1;
        Require(Same(ComputeClipped(box, viewport, limit, 801, 603, errors), limit), "Negative error did not fall back");
        box.fill({.1, -.1, 1.00001, 1});
        errors.fill({.3, .2, .00002, .1});
        rect = ComputeClipped(box, viewport, limit, 801, 603, errors);
        Require(Contains(rect, 400, 301), "Near-plane error allowance lost valid support");
        box.fill({0, 0, -.00001, 1});
        errors.fill({0, 0, .00002, 0});
        Require(Contains(ComputeClipped(box, viewport, limit, 801, 603, errors), 400, 301),
            "Far-plane error allowance lost valid support");
        box.fill({0, 0, .01, .02});
        errors.fill({.1, .1, .1, .1});
        Require(Same(ComputeClipped(box, viewport, limit, 801, 603, errors), limit),
            "Ill-conditioned interval divide did not fall back");

        CheckUnion({{1, 1, 12, 8}, {17, 17, 30, 30}, {4, 3, 10, 20}}, {1, 1, 31, 33}, 32, 4);
        CheckUnion({{-15, -13, -8, -7}, {8, 11, 15, 16}}, {-17, -15, 19, 21}, 1, 2);
        CheckUnion({}, {0, 0, 5, 7}, 32, 32);
        CheckUnion({{0, 0, 5, 7}}, {0, 0, 5, 7}, 32, 32);
        const auto sparse = Merge({{1, 1, 7, 7}, {81, 81, 87, 87}}, {0, 0, 128, 128}, 32, 8);
        Require(sparse.size() == 2, "Separated support did not retain disjoint rectangles");
        Require(Merge({{100, 100, 110, 110}}, {0, 0, 25, 27}).empty(), "Offscreen support was retained");
        const auto huge = Merge({{100, 100, 110, 110}}, {0, 0, INT32_MAX, INT32_MAX}, 1, 2);
        Require(huge.size() == 1 && Contains(huge.front(), 105, 105), "Huge-domain coarsening failed");

        std::mt19937 random(923571);
        std::uniform_real_distribution<double> unit(0, 1);
        for (unsigned trial = 0; trial < 500; ++trial)
        {
            const int width = 17 + int(random() % 100), height = 17 + int(random() % 80);
            const Rect clipped{1, 3, width, height};
            std::vector<Rect> inputs;
            for (unsigned i = 0; i < 30; ++i)
            {
                const int x = int(random() % unsigned(width + 20)) - 10;
                const int y = int(random() % unsigned(height + 20)) - 10;
                inputs.push_back({x, y, x + int(random() % 10), y + int(random() % 10)});
            }
            CheckUnion(inputs, clipped, 1 + random() % 32, 2u << (random() % 4));
        }

        // Independent convex combinations and independent homogeneous errors
        // exercise front/behind/near/far boxes. Every true visible point, its
        // repair destinations and derivative partners must remain scheduled.
        for (unsigned trial = 0; trial < 3000; ++trial)
        {
            const double centerX = unit(random) * 4 - 2, centerY = unit(random) * 4 - 2;
            const double minDepth = unit(random) * 3 - 1;
            box = ProjectedBox(centerX - unit(random), centerY - unit(random), minDepth,
                centerX + unit(random), centerY + unit(random), minDepth + .1 + unit(random) * 5);
            if (trial % 2)
            {
                // A general affine local-box transform gives correlated X/Y/W
                // extrema rather than an axis-aligned view-space box.
                const double a[9] = {unit(random), unit(random) - .5, unit(random) - .5,
                    unit(random) - .5, unit(random), unit(random) - .5,
                    unit(random) - .5, unit(random) - .5, unit(random)};
                for (size_t i = 0; i < box.size(); ++i)
                {
                    const double x = (i & 1) ? 1. : -1.;
                    const double y = (i & 2) ? 1. : -1.;
                    const double z = (i & 4) ? 1. : -1.;
                    const double depth = minDepth + a[6] * x + a[7] * y + a[8] * z;
                    box[i] = {centerX + a[0] * x + a[1] * y + a[2] * z,
                        centerY + a[3] * x + a[4] * y + a[5] * z,
                        .20004000800160032 - .00020004000800160032 * depth, depth};
                    // Independent rounding of the nominal corners must not
                    // require a perfect parallelepiped or assumed edge list.
                    box[i].x += (unit(random) - .5) * .00001;
                    box[i].w += (unit(random) - .5) * .00001;
                }
            }
            for (auto& error : errors) error = {.0003, .0002, .00001, .0002};
            rect = ComputeClipped(box, viewport, limit, 801, 603, errors);
            for (unsigned sample = 0; sample < 50; ++sample)
            {
                Clip point{}; double weightSum = 0;
                for (size_t i = 0; i < box.size(); ++i)
                {
                    // Sparse combinations concentrate samples on edges and
                    // near the newly clipped cap, not only near the center.
                    const double weight = sample % 2 == 0 ? unit(random)
                        : ((i == sample % 8 || i == (sample + 3) % 8) ? unit(random) : 0.);
                    weightSum += weight;
                    point.x += box[i].x * weight; point.y += box[i].y * weight;
                    point.z += box[i].z * weight; point.w += box[i].w * weight;
                }
                point.x = point.x / weightSum + (unit(random) * 2 - 1) * .0003;
                point.y = point.y / weightSum + (unit(random) * 2 - 1) * .0002;
                point.z = point.z / weightSum + (unit(random) * 2 - 1) * .00001;
                point.w = point.w / weightSum + (unit(random) * 2 - 1) * .0002;
                if (point.w <= 0 || point.z < 0 || point.z > point.w) continue;
                const int x = int(std::floor((point.x / point.w + 1) * 801 * .5));
                const int y = int(std::floor((1 - point.y / point.w) * 603 * .5));
                for (int dy = -ResolveRegion::RepairPixels; dy <= ResolveRegion::RepairPixels; ++dy)
                    for (int dx = -ResolveRegion::RepairPixels; dx <= ResolveRegion::RepairPixels; ++dx)
                    {
                        const int qx = int(std::floor(double(x + dx) * .5)) * 2;
                        const int qy = int(std::floor(double(y + dy) * .5)) * 2;
                        for (int yy = qy; yy < qy + 2; ++yy)
                            for (int xx = qx; xx < qx + 2; ++xx)
                                if (Contains(limit, xx, yy)) Require(Contains(rect, xx, yy), "Clipped support lost");
                    }
            }
        }
        std::cout << "Disjoint resolve regions retain clipped ownership, repair and derivative quads\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
