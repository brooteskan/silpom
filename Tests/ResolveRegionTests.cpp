// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/ResolveRegion.h"
#include <iostream>
#include <random>

using namespace SilPOM::ResolveRegion;

namespace
{
bool Same(const Rect& a, const Rect& b)
{
    return a.minX == b.minX && a.minY == b.minY && a.maxX == b.maxX && a.maxY == b.maxY;
}
bool Contains(const Rect& rect, int x, int y)
{
    return x >= rect.minX && x < rect.maxX && y >= rect.minY && y < rect.maxY;
}
std::array<Clip, 8> Box(double x0, double y0, double x1, double y1)
{
    std::array<Clip, 8> result;
    for (unsigned i = 0; i < 8; ++i)
        result[i] = {i & 1 ? x1 : x0, i & 2 ? y1 : y0, .5, 1};
    return result;
}
}

int main()
{
    const Viewport viewport{11, 7, 812, 610};
    const Rect scissor{13, 9, 809, 607};
    auto corners = Box(-.1, -.2, .2, .3);
    auto region = Compute(corners, viewport, scissor, 813, 611);
    if (region.Empty() || region.minX % 2 || region.minY % 2 || region.maxX % 2 || region.maxY % 2) return 1;
    for (unsigned i = 0; i < 8; ++i)
    {
        const auto saved = corners[i];
        for (const Clip invalid : {Clip{0, 0, 0, 0}, Clip{0, 0, 1, 1}, Clip{0, 0, 0, 1},
            Clip{0, 0, 1.1, 1}, Clip{0, 0, -.1, 1},
            Clip{std::numeric_limits<double>::quiet_NaN(), 0, .5, 1},
            Clip{0, 0, .5, std::numeric_limits<double>::infinity()}})
        {
            corners[i] = invalid;
            if (!Same(Compute(corners, viewport, scissor, 813, 611), scissor)) return 2;
        }
        corners[i] = saved;
    }
    if (!Compute(Box(2, 2, 3, 3), viewport, scissor, 813, 611).Empty()
        || !Compute(corners, viewport, scissor, 0, 611).Empty()
        || !Compute(corners, viewport, Rect{}, 813, 611).Empty()) return 3;
    if (!Same(Compute(Box(-2, -2, 2, 2), Viewport{0, 0, 813, 611},
        Rect{0, 0, 813, 611}, 813, 611), Rect{0, 0, 813, 611})) return 4;
    if (!Same(Compute(corners, Viewport{0, 0, 0, 0}, scissor, 813, 611), scissor)) return 5;
    // A subpixel excursion outside the target may still receive repaired
    // ownership. Rejecting the unexpanded projected box would lose this pixel.
    const auto edge = Compute(Box(-1.002, -.1, -1.001, .1), Viewport{0, 0, 801, 603},
        Rect{0, 0, 801, 603}, 801, 603);
    if (!Contains(edge, 0, 301)) return 6;

    // Include uncertainty before perspective division rather than padding an
    // already rounded NDC value. A small clip result can hide large operands.
    corners = Box(-.1, -.1, .1, .1);
    std::array<Clip, 8> errors;
    for (auto& error : errors) error = {.3, .2, .01, .1};
    const Viewport errorViewport{0, 0, 801, 603};
    const Rect errorLimit{0, 0, 801, 603};
    region = Compute(corners, errorViewport, errorLimit, 801, 603, errors);
    for (size_t i = 0; i < corners.size(); ++i)
        for (double sw : {-1., 1.})
            for (double sx : {-1., 1.})
                for (double sy : {-1., 1.})
                {
                    const double w = corners[i].w + sw * errors[i].w;
                    const int x = int(std::floor(((corners[i].x + sx * errors[i].x) / w + 1) * 801 * .5));
                    const int y = int(std::floor((1 - (corners[i].y + sy * errors[i].y) / w) * 603 * .5));
                    if (!Contains(region, x, y)) return 8;
                }
    errors[0].w = .51;
    if (!Same(Compute(corners, errorViewport, errorLimit, 801, 603, errors), errorLimit)) return 9;
    errors[0].w = -1;
    if (!Same(Compute(corners, errorViewport, errorLimit, 801, 603, errors), errorLimit)) return 10;

    std::mt19937 random(57125);
    std::uniform_real_distribution<double> unit(0, 1);
    // Independent convex combinations exercise changing perspective W.
    // Every owned sample, its possible two-pixel repair destinations, and
    // their derivative quad must remain scheduled inside the original limit.
    for (unsigned trial = 0; trial < 2000; ++trial)
    {
        const unsigned width = 25 + random() % 1500, height = 25 + random() % 900;
        const double left = random() % 15 + .25 * (random() % 4);
        const double top = random() % 15 + .25 * (random() % 4);
        const Viewport vp{left, top, double(width) - .25, double(height) - .5};
        const Rect limit{int(left) + 1, int(top) + 1, int(width) - 1, int(height) - 1};
        const double centerX = unit(random) * 3 - 1.5, centerY = unit(random) * 3 - 1.5;
        for (size_t i = 0; i < corners.size(); ++i)
        {
            const double w = .2 + 10 * unit(random);
            corners[i] = {(centerX + (unit(random) - .5) * .4) * w,
                (centerY + (unit(random) - .5) * .4) * w, .5 * w, w};
            errors[i] = {.001 * w, .002 * w, .001 * w, .001 * w};
        }
        region = Compute(corners, vp, limit, width, height, errors);
        for (unsigned sample = 0; sample < 20; ++sample)
        {
            Clip point{};
            for (size_t i = 0; i < corners.size(); ++i)
            {
                const double weight = unit(random);
                const auto& corner = corners[i]; const auto& error = errors[i];
                point.x += weight * (corner.x + (unit(random) * 2 - 1) * error.x);
                point.y += weight * (corner.y + (unit(random) * 2 - 1) * error.y);
                point.w += weight * (corner.w + (unit(random) * 2 - 1) * error.w);
            }
            // Homogeneous division cancels normalization of the weights.
            const double sx = vp.minX + (point.x / point.w + 1) * (vp.maxX - vp.minX) * .5;
            const double sy = vp.minY + (1 - point.y / point.w) * (vp.maxY - vp.minY) * .5;
            const int x = int(std::floor(sx)), y = int(std::floor(sy));
            for (int dy = -RepairPixels; dy <= RepairPixels; ++dy)
                for (int dx = -RepairPixels; dx <= RepairPixels; ++dx)
                {
                    // Derivative partners share the absolute 2x2 pixel quad.
                    const int qx = int(std::floor(double(x + dx) * .5)) * 2;
                    const int qy = int(std::floor(double(y + dy) * .5)) * 2;
                    for (int yy = qy; yy < qy + 2; ++yy)
                        for (int xx = qx; xx < qx + 2; ++xx)
                            if (Contains(limit, xx, yy) && !Contains(region, xx, yy)) return 7;
                }
        }
    }
    std::cout << "Resolve regions retain projected ownership, repair and derivative support\n";
}
