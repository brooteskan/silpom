// SPDX-License-Identifier: MIT
#pragma once
#include "ResolveRegions.h"

namespace SilPOM::RepairRegion
{
using ResolveRegion::Clip;
using ResolveRegion::Rect;
using ResolveRegion::Viewport;

enum class Mode { NoWork, Regional, FullScreenFallback };
struct Plan
{
    Mode mode = Mode::NoWork;
    std::vector<Rect> rectangles;
};

namespace Internal
{
inline bool ValidViewport(const Viewport& viewport)
{
    return std::isfinite(viewport.minX) && std::isfinite(viewport.minY)
        && std::isfinite(viewport.maxX) && std::isfinite(viewport.maxY)
        && viewport.minX < viewport.maxX && viewport.minY < viewport.maxY
        && std::isfinite(viewport.maxX - viewport.minX)
        && std::isfinite(viewport.maxY - viewport.minY);
}

inline Rect DrawableLimit(const Viewport& viewport, const Rect& scissor,
    uint32_t targetWidth, uint32_t targetHeight)
{
    // An unprojectable box selects ResolveRegion's exact target, viewport,
    // fractional-MSAA-edge and integer-clamping policy, without its padding.
    return ResolveRegion::Compute({}, viewport, scissor, targetWidth, targetHeight);
}

struct Coverage
{
    Rect rectangle;
    bool uncertain = false;
};

// This is the relaxed near-half-space projection used by
// ResolveRegions::ComputeClipped. Keep donor pixels separate from repair and
// derivative support: only the producer's own drawable domain clips this box.
inline Coverage ProjectDonors(const std::array<Clip, 8>& corners, const Viewport& viewport,
    const Rect& scissor, uint32_t targetWidth, uint32_t targetHeight,
    const std::array<Clip, 8>& clipErrors)
{
    const Rect limit = DrawableLimit(viewport, scissor, targetWidth, targetHeight);
    if (limit.Empty()) return {};
    if (!ValidViewport(viewport)) return {{}, true};
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
                return {{}, true};
            magnitude = std::max(magnitude, std::abs(components[axis]) + errors[axis]);
        }
        error.x = std::max(error.x, uncertainty.x);
        error.y = std::max(error.y, uncertainty.y);
        error.z = std::max(error.z, uncertainty.z);
        error.w = std::max(error.w, uncertainty.w);
    }
    // Separate engineering allowance for double plane tests/interpolation;
    // supplied errors retain the existing CPU/GPU float-transform contract.
    const double guard = magnitude * 1e-10;
    if (!std::isfinite(guard)) return {{}, true};
    error.x += guard; error.y += guard; error.z += guard; error.w += guard;

    std::array<double, 8> nearDistance{};
    double farMaximum = -std::numeric_limits<double>::infinity();
    double nearMaximum = farMaximum;
    for (size_t i = 0; i < corners.size(); ++i)
    {
        const auto& point = corners[i];
        farMaximum = std::max(farMaximum, point.z + error.z);
        nearDistance[i] = point.w - point.z + error.w + error.z + guard;
        nearMaximum = std::max(nearMaximum, nearDistance[i]);
    }
    // Atom reverse Z: near is W-Z >= 0 and far is Z >= 0. Reject only
    // when the complete uncertainty-expanded volume is outside a plane.
    if (farMaximum < -guard || nearMaximum < -guard) return {};

    double lowX = std::numeric_limits<double>::infinity();
    double lowY = lowX, highX = -lowX, highY = -lowX;
    bool ambiguous = false, any = false;
    const auto include = [&](const Clip& point)
    {
        const double lowW = point.w - error.w, highW = point.w + error.w;
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
    // Every pair includes the hull's edges even if rounded corners are not
    // exactly affine. Extra intersections can only enlarge the bound.
    for (size_t i = 0; i < corners.size(); ++i)
        for (size_t j = i + 1; j < corners.size(); ++j)
        {
            const double first = nearDistance[i], second = nearDistance[j];
            if ((first < 0) == (second < 0)) continue;
            const double denominator = first - second;
            if (!std::isfinite(denominator) || denominator == 0) return {{}, true};
            const double t = first / denominator;
            if (!std::isfinite(t) || t < 0 || t > 1) return {{}, true};
            const auto& a = corners[i]; const auto& b = corners[j];
            include({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
        }
    if (ambiguous || !any) return {{}, true};

    const double width = viewport.maxX - viewport.minX;
    const double height = viewport.maxY - viewport.minY;
    constexpr double padding = ResolveRegion::RasterPaddingNdc;
    const double minX = viewport.minX + (lowX - padding + 1) * width * .5;
    const double maxX = viewport.minX + (highX + padding + 1) * width * .5;
    const double minY = viewport.minY + (1 - highY - padding) * height * .5;
    const double maxY = viewport.minY + (1 - lowY + padding) * height * .5;
    if (!std::isfinite(minX) || !std::isfinite(maxX)
        || !std::isfinite(minY) || !std::isfinite(maxY)) return {{}, true};
    // No derivative quad alignment: both repair shaders use explicit sample
    // loads and stored gradients. Clamp doubles before conversion to int.
    const auto x = [&](double value) { return std::clamp(value, double(limit.minX), double(limit.maxX)); };
    const auto y = [&](double value) { return std::clamp(value, double(limit.minY), double(limit.maxY)); };
    return {ResolveRegion::Intersect(limit, {static_cast<int32_t>(std::floor(x(minX))),
        static_cast<int32_t>(std::floor(y(minY))), static_cast<int32_t>(std::ceil(x(maxX))),
        static_cast<int32_t>(std::ceil(y(maxY)))}), false};
}

inline Rect ExpandInto(const Rect& donor, const Rect& domain)
{
    if (donor.Empty() || domain.Empty()) return {};
    constexpr int64_t pixels = ResolveRegion::RepairPixels;
    const int64_t minX = std::max(int64_t(domain.minX), int64_t(donor.minX) - pixels);
    const int64_t minY = std::max(int64_t(domain.minY), int64_t(donor.minY) - pixels);
    const int64_t maxX = std::min(int64_t(domain.maxX), int64_t(donor.maxX) + pixels);
    const int64_t maxY = std::min(int64_t(domain.maxY), int64_t(donor.maxY) + pixels);
    if (minX >= maxX || minY >= maxY) return {};
    return {static_cast<int32_t>(minX), static_cast<int32_t>(minY),
        static_cast<int32_t>(maxX), static_cast<int32_t>(maxY)};
}
}

// One frame/view's pure scheduling calculation. The caller publishes the
// returned value once and shares that same immutable plan between both passes.
// Add every enabled possible wall producer, regardless of cache eligibility.
class PlanBuilder
{
public:
    PlanBuilder(const Viewport& repairViewport, const Rect& repairScissor,
        uint32_t targetWidth, uint32_t targetHeight)
        : m_domain(Internal::DrawableLimit(repairViewport, repairScissor, targetWidth, targetHeight))
        , m_targetWidth(targetWidth), m_targetHeight(targetHeight)
    {
        if (!m_domain.Empty() && !Internal::ValidViewport(repairViewport)) RequireFullScreen();
    }

    void RequireFullScreen()
    {
        if (!m_domain.Empty()) m_plan = {Mode::FullScreenFallback, {m_domain}};
    }

    void AddProducer(const std::array<Clip, 8>& corners, const Viewport& producerViewport,
        const Rect& producerScissor, const std::array<Clip, 8>& clipErrors = {})
    {
        if (m_domain.Empty() || m_plan.mode == Mode::FullScreenFallback) return;
        const auto coverage = Internal::ProjectDonors(corners, producerViewport, producerScissor,
            m_targetWidth, m_targetHeight, clipErrors);
        if (coverage.uncertain) { RequireFullScreen(); return; }
        // Expand BEFORE clipping to repair domain. A donor outside the repair
        // scissor/viewport can support an inside repair; its own clipping above
        // remains essential because pixels outside the producer draw do not exist.
        const Rect region = Internal::ExpandInto(coverage.rectangle, m_domain);
        if (region.Empty()) return;
        m_plan.mode = Mode::Regional;
        m_plan.rectangles.push_back(region);
        // Bound accumulated producer support as well as final draw count.
        // Coarsening only adds pixels, so earlier support remains conservative.
        if (m_plan.rectangles.size() >= 1024)
            m_plan.rectangles = ResolveRegions::Merge(m_plan.rectangles, m_domain);
    }

    Plan Finish(size_t rectangleBudget = 32, uint32_t cellPixels = 32) const
    {
        if (m_plan.mode != Mode::Regional) return m_plan;
        // Merge partitions the grid into disjoint half-open rectangles. Both
        // passes submit this exact list: no sample is classified or filled twice.
        return {Mode::Regional, ResolveRegions::Merge(m_plan.rectangles, m_domain, rectangleBudget, cellPixels)};
    }
    const Rect& Domain() const { return m_domain; }

private:
    Rect m_domain;
    uint32_t m_targetWidth, m_targetHeight;
    Plan m_plan;
};
}
