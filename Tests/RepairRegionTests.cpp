// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/RepairRegion.h"
#include <iostream>
#include <random>
#include <stdexcept>

using namespace SilPOM::RepairRegion;
namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
bool Contains(const Rect& rect, int32_t x, int32_t y)
{
    return x >= rect.minX && x < rect.maxX && y >= rect.minY && y < rect.maxY;
}
const Rect& Only(const Plan& plan)
{
    Require(plan.rectangles.size() == 1, "Expected one rectangle");
    return plan.rectangles.front();
}
bool Contains(const Plan& plan, int32_t x, int32_t y)
{
    return std::any_of(plan.rectangles.begin(), plan.rectangles.end(),
        [&](const Rect& rect) { return Contains(rect, x, y); });
}
void CheckDisjoint(const Plan& plan, const Rect& domain, size_t budget = 32)
{
    Require(plan.rectangles.size() <= budget, "Repair rectangle budget exceeded");
    Require((plan.mode == Mode::NoWork) == plan.rectangles.empty(), "Empty work mode disagrees with rectangle list");
    for (size_t i = 0; i < plan.rectangles.size(); ++i)
    {
        const auto& a = plan.rectangles[i];
        Require(!a.Empty() && a.minX >= domain.minX && a.minY >= domain.minY
            && a.maxX <= domain.maxX && a.maxY <= domain.maxY, "Repair rectangle escaped domain");
        for (size_t j = 0; j < i; ++j)
            Require(SilPOM::ResolveRegion::Intersect(a, plan.rectangles[j]).Empty(),
                "Repair rectangles would process a sample twice");
    }
}
bool Same(const Rect& a, const Rect& b)
{
    return a.minX == b.minX && a.minY == b.minY && a.maxX == b.maxX && a.maxY == b.maxY;
}
std::array<Clip, 8> PixelBox(const Viewport& vp, double x0, double y0, double x1, double y1)
{
    std::array<Clip, 8> corners;
    for (size_t i = 0; i < corners.size(); ++i)
        corners[i] = {2 * (((i & 1) ? x1 : x0) - vp.minX) / (vp.maxX - vp.minX) - 1,
            1 - 2 * (((i & 2) ? y1 : y0) - vp.minY) / (vp.maxY - vp.minY), .5, 1};
    return corners;
}
std::array<Clip, 8> PerspectiveBox(double x0, double y0, double d0, double x1, double y1, double d1)
{
    std::array<Clip, 8> corners;
    for (size_t i = 0; i < corners.size(); ++i)
    {
        const double depth = (i & 4) ? d1 : d0;
        corners[i] = {(i & 1) ? x1 : x0, (i & 2) ? y1 : y0,
            .20004000800160032 - .00020004000800160032 * depth, depth};
    }
    return corners;
}
}

int main()
{
    try
    {
        const Viewport vp{0, 0, 100, 100};
        const Rect target{0, 0, 100, 100}, repair{10, 10, 30, 30};
        PlanBuilder outside(vp, repair, 100, 100);
        outside.AddProducer(PixelBox(vp, 9.2, 9.2, 9.3, 9.3), vp, target);
        auto plan = outside.Finish(32, 2);
        Require(plan.mode == Mode::Regional && Same(Only(plan), {10, 10, 12, 12}),
            "Donor outside repair scissor lost diagonal two-pixel support");
        Require(Contains(plan, 11, 11) && !Contains(plan, 12, 12),
            "Repair expansion must be square, without derivative or quad padding");

        PlanBuilder producerClip(vp, repair, 100, 100);
        producerClip.AddProducer(PixelBox(vp, 9.2, 9.2, 9.3, 9.3), vp, repair);
        Require(producerClip.Finish().mode == Mode::NoWork,
            "Pixels clipped by producer scissor became donors");
        PlanBuilder separate(vp, target, 100, 100);
        separate.AddProducer(PixelBox(vp, 10.2, 20.2, 10.3, 20.3), vp, target);
        separate.AddProducer(PixelBox(vp, 80.2, 70.2, 80.3, 70.3), vp, target);
        plan = separate.Finish();
        Require(plan.mode == Mode::Regional && plan.rectangles.size() == 2,
            "Separated producers must retain separate repair islands");
        CheckDisjoint(plan, target);
        Require(Contains(plan, 10, 20) && Contains(plan, 80, 70) && !Contains(plan, 50, 50),
            "Separated supports were lost or their large gap was filled");

        PlanBuilder overlap(vp, target, 100, 100);
        overlap.AddProducer(PixelBox(vp, 10, 10, 55, 40), vp, target);
        overlap.AddProducer(PixelBox(vp, 35, 25, 80, 75), vp, target);
        plan = overlap.Finish();
        CheckDisjoint(plan, target);
        for (int32_t y = 8; y < 77; ++y)
            for (int32_t x = 8; x < 82; ++x)
                if ((x < 57 && y < 42) || (x >= 33 && y >= 23))
                    Require(Contains(plan, x, y), "Overlapping supports lost union coverage");

        const Viewport largeViewport{0, 0, 1024, 512};
        const Rect largeTarget{0, 0, 1024, 512};
        PlanBuilder budgeted(largeViewport, largeTarget, 1024, 512);
        for (int32_t y = 16; y < 512; y += 64)
            for (int32_t x = 16; x < 1024; x += 64)
                budgeted.AddProducer(PixelBox(largeViewport, x + .2, y + .2, x + .3, y + .3),
                    largeViewport, largeTarget);
        plan = budgeted.Finish(4);
        CheckDisjoint(plan, largeTarget, 4);
        for (int32_t y = 16; y < 512; y += 64)
            for (int32_t x = 16; x < 1024; x += 64)
                for (int32_t dy = -2; dy <= 2; ++dy)
                    for (int32_t dx = -2; dx <= 2; ++dx)
                        Require(Contains(plan, x + dx, y + dy), "Budget coarsening clipped support");

        PlanBuilder accumulated(vp, target, 100, 100);
        for (unsigned i = 0; i < 2500; ++i)
        {
            const double x = i % 2 ? 80.2 : 10.2;
            accumulated.AddProducer(PixelBox(vp, x, x, x + .1, x + .1), vp, target);
        }
        plan = accumulated.Finish();
        CheckDisjoint(plan, target);
        Require(Contains(plan, 8, 8) && Contains(plan, 82, 82),
            "Bounded input accumulation lost earlier support");

        const Viewport fractionalProducer{3.25, 5.5, 45.75, 43.25};
        PlanBuilder boundaries({0, 0, 80, 70}, {-99, -99, 999, 999}, 45, 43);
        boundaries.AddProducer(PixelBox(fractionalProducer, 44.9, 42.9, 44.95, 42.95),
            fractionalProducer, {-99, -99, 999, 999});
        plan = boundaries.Finish(32, 2);
        Require(plan.mode == Mode::Regional && Same(Only(plan), {42, 40, 45, 43}),
            "Fractional producer viewport or odd attachment edge clipped support");
        PlanBuilder unequal({10.25, 10.25, 60.75, 60.75}, target, 100, 100);
        unequal.AddProducer(PixelBox(vp, 9.2, 9.2, 9.3, 9.3), vp, target);
        Require(Same(Only(unequal.Finish(32, 2)), {10, 10, 12, 12}),
            "Producer viewport was incorrectly replaced by repair viewport");

        PlanBuilder empty(vp, target, 100, 100);
        Require(empty.Finish().mode == Mode::NoWork, "Empty complete producer inventory has work");
        empty.AddProducer(PixelBox(vp, 105, 105, 106, 106), vp, target);
        empty.AddProducer(PerspectiveBox(-1, -1, -.3, 1, 1, .1), vp, target);
        empty.AddProducer(PerspectiveBox(-1, -1, 1100, 1, 1, 1200), vp, target);
        Require(empty.Finish().mode == Mode::NoWork, "Proven clipped volumes retained repair work");
        PlanBuilder zero(vp, target, 0, 100);
        zero.RequireFullScreen();
        Require(zero.Finish().mode == Mode::NoWork, "Empty target should submit zero draws");
        PlanBuilder emptyScissor(vp, {}, 100, 100);
        emptyScissor.RequireFullScreen();
        Require(emptyScissor.Finish().mode == Mode::NoWork, "Empty repair domain should submit zero draws");

        PlanBuilder unknown(vp, repair, 100, 100);
        unknown.RequireFullScreen();
        unknown.AddProducer(PixelBox(vp, 20, 20, 21, 21), vp, target);
        Require(unknown.Finish().mode == Mode::FullScreenFallback
            && Same(Only(unknown.Finish()), repair), "Unknown producer did not retain full repair domain");
        PlanBuilder invalidViewport(vp, repair, 100, 100);
        invalidViewport.AddProducer(PixelBox(vp, 20, 20, 21, 21), {0, 0, 0, 0}, target);
        Require(invalidViewport.Finish().mode == Mode::FullScreenFallback,
            "Unsupported producer viewport did not fall back");
        auto box = PerspectiveBox(-.01, -.01, -.1, .01, .01, 2);
        PlanBuilder nearClip(vp, target, 100, 100);
        nearClip.AddProducer(box, vp, target);
        plan = nearClip.Finish();
        Require(plan.mode == Mode::Regional && Contains(plan, 50, 50)
            && !Same(Only(plan), target), "Near crossing failed conservative clipping");
        std::array<Clip, 8> errors{};
        box.fill({0, 0, .01, .02});
        errors.fill({.1, .1, .1, .1});
        PlanBuilder ambiguous(vp, repair, 100, 100);
        ambiguous.AddProducer(box, vp, target, errors);
        Require(ambiguous.Finish().mode == Mode::FullScreenFallback
            && Same(Only(ambiguous.Finish()), repair), "Ambiguous W interval was treated as empty");
        box.fill({0, 0, 1.00001, 1});
        errors.fill({.001, .001, .00002, .00002});
        PlanBuilder uncertainNear(vp, target, 100, 100);
        uncertainNear.AddProducer(box, vp, target, errors);
        Require(Contains(uncertainNear.Finish(), 50, 50),
            "Near-plane transform uncertainty lost valid donors");
        errors[0].w = -1;
        PlanBuilder badError(vp, target, 100, 100);
        badError.AddProducer(box, vp, target, errors);
        Require(badError.Finish().mode == Mode::FullScreenFallback, "Invalid errors did not fall back");
        box[0].x = std::numeric_limits<double>::quiet_NaN();
        PlanBuilder badProjection(vp, target, 100, 100);
        badProjection.AddProducer(box, vp, target);
        Require(badProjection.Finish().mode == Mode::FullScreenFallback, "Nonfinite projection did not fall back");

        // Independent convex combinations and perturbations exercise the same
        // homogeneous error contract as the producer projection. Verify every
        // visible donor's square neighborhood in a distinct repair scissor.
        std::mt19937 random(278541);
        std::uniform_real_distribution<double> unit(0, 1);
        uint32_t checkedUnionPixels = 0;
        for (unsigned trial = 0; trial < 200; ++trial)
        {
            const Viewport unionViewport{0, 0, 123, 91};
            const Rect unionDomain{3, 5, 119, 88};
            PlanBuilder builder(unionViewport, unionDomain, 123, 91);
            std::vector<Rect> supports;
            for (unsigned input = 0; input < 12; ++input)
            {
                const double x = unit(random) * 145 - 10;
                const double y = unit(random) * 110 - 10;
                const auto corners = PixelBox(unionViewport, x, y,
                    x + .1 + unit(random) * 15, y + .1 + unit(random) * 15);
                builder.AddProducer(corners, unionViewport, {0, 0, 123, 91});
                const auto coverage = SilPOM::RepairRegion::Internal::ProjectDonors(
                    corners, unionViewport, {0, 0, 123, 91}, 123, 91, {});
                Require(!coverage.uncertain, "Pixel-space test producer became uncertain");
                supports.push_back(SilPOM::RepairRegion::Internal::ExpandInto(coverage.rectangle, unionDomain));
            }
            const size_t budget = 1 + random() % 8;
            plan = builder.Finish(budget, 2 + random() % 31);
            CheckDisjoint(plan, unionDomain, budget);
            for (const auto& support : supports)
                for (int32_t y = support.minY; y < support.maxY; ++y)
                    for (int32_t x = support.minX; x < support.maxX; ++x)
                    {
                        Require(Contains(plan, x, y), "Randomized disjoint union lost expanded producer support");
                        ++checkedUnionPixels;
                    }
        }
        // Keep the prior randomized projection sequence reproducible.
        random.seed(278541);
        uint32_t checkedDonors = 0;
        for (unsigned trial = 0; trial < 2000; ++trial)
        {
            const uint32_t width = 33 + random() % 200, height = 27 + random() % 160;
            const Viewport producer{.25 + random() % 7, .5 + random() % 7,
                double(width) - .25, double(height) - .75};
            const Viewport repairViewport{0, 0, double(width), double(height)};
            const Rect producerScissor{3, 4, int32_t(width) - 2, int32_t(height) - 3};
            const Rect repairScissor{8, 9, int32_t(width) - 4, int32_t(height) - 5};
            PlanBuilder builder(repairViewport, repairScissor, width, height);
            const double centerX = unit(random) * 3 - 1.5, centerY = unit(random) * 3 - 1.5;
            const double minDepth = unit(random) * 3 - 1;
            box = PerspectiveBox(centerX - unit(random), centerY - unit(random), minDepth,
                centerX + unit(random), centerY + unit(random), minDepth + .1 + unit(random) * 5);
            errors.fill({.0003, .0002, .00001, .0002});
            builder.AddProducer(box, producer, producerScissor, errors);
            plan = builder.Finish();
            CheckDisjoint(plan, repairScissor);
            for (unsigned sample = 0; sample < 40; ++sample)
            {
                Clip point{}; double weights = 0;
                for (size_t i = 0; i < box.size(); ++i)
                {
                    const double weight = sample % 2 ? unit(random)
                        : ((i == sample % 8 || i == (sample + 3) % 8) ? unit(random) : 0);
                    weights += weight;
                    point.x += weight * box[i].x; point.y += weight * box[i].y;
                    point.z += weight * box[i].z; point.w += weight * box[i].w;
                }
                point.x = point.x / weights + (unit(random) * 2 - 1) * .0003;
                point.y = point.y / weights + (unit(random) * 2 - 1) * .0002;
                point.z = point.z / weights + (unit(random) * 2 - 1) * .00001;
                point.w = point.w / weights + (unit(random) * 2 - 1) * .0002;
                if (point.w <= 0 || point.z < 0 || point.z > point.w) continue;
                const double sx = producer.minX + (point.x / point.w + 1) * (producer.maxX - producer.minX) * .5;
                const double sy = producer.minY + (1 - point.y / point.w) * (producer.maxY - producer.minY) * .5;
                if (sx < producer.minX || sx >= producer.maxX || sy < producer.minY || sy >= producer.maxY) continue;
                const int32_t x = int32_t(std::floor(sx)), y = int32_t(std::floor(sy));
                if (!Contains(producerScissor, x, y)) continue;
                ++checkedDonors;
                for (int32_t dy = -2; dy <= 2; ++dy)
                    for (int32_t dx = -2; dx <= 2; ++dx)
                        if (Contains(repairScissor, x + dx, y + dy))
                            Require(Contains(plan, x + dx, y + dy), "Uncertain projected donor lost repair support");
            }
        }
        Require(checkedDonors > 10000, "Randomized property test did not exercise enough visible donors");
        std::cout << "Repair plan retains disjoint coverage, square support and safe fallback; "
            << checkedDonors << " visible donors and " << checkedUnionPixels << " union pixels checked\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
