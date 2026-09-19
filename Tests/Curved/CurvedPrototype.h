// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>
#include "CurvedInterval.h"
#include "CurvedDyadic.h"

namespace SilPOM::Curved
{
using uint = std::uint32_t;

struct Vec2
{
    double x = 0.0;
    double y = 0.0;
};

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
inline Vec2 operator*(Vec2 a, double b) { return { a.x * b, a.y * b }; }
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, double b) { return { a.x * b, a.y * b, a.z * b }; }
inline Vec3 operator/(Vec3 a, double b) { return a * (1.0 / b); }
inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline double Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) { const double n = Length(a); return n == 0.0 ? Vec3{} : a / n; }
inline bool Finite(Vec2 a) { return std::isfinite(a.x) && std::isfinite(a.y); }
inline bool Finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

struct Texture
{
    uint width = 0;
    uint height = 0;
    std::vector<double> pixels;

    double Fetch(int x, int y, uint addressMode) const
    {
        if (addressMode == 0)
        {
            x = (x % int(width) + int(width)) % int(width);
            y = (y % int(height) + int(height)) % int(height);
        }
        else
        {
            x = std::clamp(x, 0, int(width) - 1);
            y = std::clamp(y, 0, int(height) - 1);
        }
        return pixels[size_t(y) * width + size_t(x)];
    }
};

struct Triangle
{
    std::array<Vec3, 3> position{};
    std::array<Vec2, 3> uv{};
    std::array<Vec3, 3> direction{};
    uint primitiveId = 0;
};

struct Surface
{
    double baseScale = 1.0;
    double amplitude = 0.1;
    double reference = 0.5;
    uint addressMode = 0;
};

struct Ray
{
    Vec3 origin{};
    Vec3 direction{};
    double tMin = 0.0;
    double tMax = std::numeric_limits<double>::infinity();
};

enum class Status : uint { Miss, Hit, Exhausted, Invalid };

enum ExhaustionReason : uint
{
    ExhaustionNone = 0,
    ExhaustionNodeBudget = 1u << 0,
    ExhaustionMaximumDepth = 1u << 1,
    ExhaustionParameterResolution = 1u << 2,
    ExhaustionDepthResolution = 1u << 3,
    ExhaustionUncertifiedLeaf = 1u << 4,
    ExhaustionStackCapacity = 1u << 5
};

struct Hit
{
    Status status = Status::Miss;
    uint primitiveId = std::numeric_limits<uint>::max();
    double tLower = std::numeric_limits<double>::infinity();
    double tUpper = std::numeric_limits<double>::infinity();
    Vec3 barycentric{};
    Vec3 position{};
    Vec2 uv{};
    Vec3 normal{};
    uint visitedFragments = 0;
    uint subdivisionNodes = 0;
    uint maximumDepth = 0;
    double lowestUnresolvedT = std::numeric_limits<double>::infinity();
    double highestUnresolvedT = -std::numeric_limits<double>::infinity();
    uint exhaustionReasons = ExhaustionNone;
    bool singularPath = false;
    uint regularCertificates = 0;
    uint rationalCertificates = 0;
};

struct Limits
{
    uint maxNodes = 32768;
    uint maxDepth = 28;
    double parameterTolerance = 1e-8;
    double residualTolerance = 2e-11;
    double singularResidualTolerance = 2e-13;
    double tTolerance = 2e-9;
};

struct NumericalTolerances
{
    double parameter = 0.0;
    double residual = 0.0;
    double singularResidual = 0.0;
    double depth = 0.0;
    double boundary = 0.0;
};

struct Sample
{
    Vec3 position{};
    Vec2 uv{};
    Vec3 direction{};
    Vec3 du{};
    Vec3 dv{};
    Vec3 normal{};
    double height = 0.0;
    bool validNormal = false;
};

inline Vec3 Interpolate(const std::array<Vec3, 3>& a, Vec3 b)
{
    return a[0] * b.x + a[1] * b.y + a[2] * b.z;
}

inline Vec2 Interpolate(const std::array<Vec2, 3>& a, Vec3 b)
{
    return a[0] * b.x + a[1] * b.y + a[2] * b.z;
}

inline Sample Evaluate(const Triangle& triangle, const Surface& surface, const Texture& texture, Vec3 barycentric)
{
    Sample result;
    result.uv = Interpolate(triangle.uv, barycentric);
    result.direction = Interpolate(triangle.direction, barycentric);
    const Vec3 base = Interpolate(triangle.position, barycentric);

    const double qx = result.uv.x * texture.width - 0.5;
    const double qy = result.uv.y * texture.height - 0.5;
    const int ix = int(std::floor(qx));
    const int iy = int(std::floor(qy));
    const double fx = qx - ix;
    const double fy = qy - iy;
    const double h00 = texture.Fetch(ix, iy, surface.addressMode);
    const double h10 = texture.Fetch(ix + 1, iy, surface.addressMode);
    const double h01 = texture.Fetch(ix, iy + 1, surface.addressMode);
    const double h11 = texture.Fetch(ix + 1, iy + 1, surface.addressMode);
    const double hx = h10 - h00;
    const double hy = h01 - h00;
    const double hxy = h11 - h10 - h01 + h00;
    result.height = h00 + hx * fx + hy * fy + hxy * fx * fy;
    const double displacement = surface.amplitude * (result.height - surface.reference);
    result.position = base * surface.baseScale + result.direction * displacement;

    const Vec3 baseU = triangle.position[1] - triangle.position[0];
    const Vec3 baseV = triangle.position[2] - triangle.position[0];
    const Vec3 directionU = triangle.direction[1] - triangle.direction[0];
    const Vec3 directionV = triangle.direction[2] - triangle.direction[0];
    const Vec2 uvU = triangle.uv[1] - triangle.uv[0];
    const Vec2 uvV = triangle.uv[2] - triangle.uv[0];
    const double dhdqX = hx + hxy * fy;
    const double dhdqY = hy + hxy * fx;
    const double heightU = surface.amplitude * (dhdqX * uvU.x * texture.width + dhdqY * uvU.y * texture.height);
    const double heightV = surface.amplitude * (dhdqX * uvV.x * texture.width + dhdqY * uvV.y * texture.height);
    result.du = baseU * surface.baseScale + result.direction * heightU + directionU * displacement;
    result.dv = baseV * surface.baseScale + result.direction * heightV + directionV * displacement;
    const Vec3 geometric = Cross(result.du, result.dv);
    const double normalLength = Length(geometric);
    result.validNormal = std::isfinite(normalLength) && normalLength > 1e-14;
    if (result.validNormal)
    {
        result.normal = geometric / normalLength;
    }
    return result;
}

inline bool Valid(const Triangle& triangle, const Surface& surface, const Texture& texture, const Ray& ray)
{
    if (texture.width == 0 || texture.height == 0 || texture.pixels.size() != size_t(texture.width) * texture.height ||
        !std::isfinite(surface.baseScale) || surface.baseScale <= 0.0 || !std::isfinite(surface.amplitude) ||
        !std::isfinite(surface.reference) || surface.reference < 0.0 || surface.reference > 1.0 || surface.addressMode > 1 ||
        !Finite(ray.origin) || !Finite(ray.direction) || Dot(ray.direction, ray.direction) == 0.0 ||
        !std::isfinite(ray.tMin) || std::isnan(ray.tMax) || ray.tMin > ray.tMax)
    {
        return false;
    }
    for (size_t i = 0; i != 3; ++i)
    {
        if (!Finite(triangle.position[i]) || !Finite(triangle.uv[i]) || !Finite(triangle.direction[i]) ||
            std::abs(Length(triangle.direction[i]) - 1.0) > 2e-6)
        {
            return false;
        }
    }
    return Length(Cross(triangle.position[1] - triangle.position[0], triangle.position[2] - triangle.position[0])) > 1e-12;
}

struct PolygonVertex
{
    Vec3 barycentric{};
    Vec2 texel{};
};

struct Fragment
{
    const Triangle* triangle = nullptr;
    std::array<Vec3, 3> domain{};
    int cellX = 0;
    int cellY = 0;
};

template<class Inside, class Intersect>
inline std::vector<PolygonVertex> Clip(const std::vector<PolygonVertex>& input, Inside inside, Intersect intersect)
{
    std::vector<PolygonVertex> output;
    if (input.empty())
    {
        return output;
    }
    PolygonVertex previous = input.back();
    bool previousInside = inside(previous);
    for (const PolygonVertex& current : input)
    {
        const bool currentInside = inside(current);
        if (currentInside != previousInside)
        {
            output.push_back(intersect(previous, current));
        }
        if (currentInside)
        {
            output.push_back(current);
        }
        previous = current;
        previousInside = currentInside;
    }
    return output;
}

inline std::vector<Fragment> BuildFragments(const Triangle& triangle, const Texture& texture)
{
    std::array<PolygonVertex, 3> vertices{};
    double minX = std::numeric_limits<double>::infinity();
    double minY = minX;
    double maxX = -minX;
    double maxY = -minX;
    for (int i = 0; i != 3; ++i)
    {
        vertices[i].barycentric[i] = 1.0;
        vertices[i].texel = { triangle.uv[i].x * texture.width - 0.5, triangle.uv[i].y * texture.height - 0.5 };
        minX = std::min(minX, vertices[i].texel.x);
        minY = std::min(minY, vertices[i].texel.y);
        maxX = std::max(maxX, vertices[i].texel.x);
        maxY = std::max(maxY, vertices[i].texel.y);
    }
    std::vector<Fragment> fragments;
    for (int y = int(std::floor(minY)); y <= int(std::floor(maxY)); ++y)
    {
        for (int x = int(std::floor(minX)); x <= int(std::floor(maxX)); ++x)
        {
            std::vector<PolygonVertex> polygon(vertices.begin(), vertices.end());
            auto clipAxis = [&](bool xAxis, double boundary, bool keepGreater)
            {
                auto coordinate = [=](const PolygonVertex& value) { return xAxis ? value.texel.x : value.texel.y; };
                polygon = Clip(polygon,
                    [&](const PolygonVertex& value) { return keepGreater ? coordinate(value) >= boundary : coordinate(value) <= boundary; },
                    [&](const PolygonVertex& a, const PolygonVertex& b)
                    {
                        const double denominator = coordinate(b) - coordinate(a);
                        const double t = denominator == 0.0 ? 0.0 : (boundary - coordinate(a)) / denominator;
                        return PolygonVertex{ a.barycentric + (b.barycentric - a.barycentric) * t, a.texel + (b.texel - a.texel) * t };
                    });
            };
            clipAxis(true, double(x), true);
            clipAxis(true, double(x + 1), false);
            clipAxis(false, double(y), true);
            clipAxis(false, double(y + 1), false);
            if (polygon.size() < 3)
            {
                continue;
            }
            for (size_t i = 1; i + 1 < polygon.size(); ++i)
            {
                const auto& a = polygon[0].barycentric;
                const auto& b = polygon[i].barycentric;
                const auto& c = polygon[i + 1].barycentric;
                if (Length(Cross(b - a, c - a)) > 1e-15)
                {
                    fragments.push_back({ &triangle, { a, b, c }, x, y });
                }
            }
        }
    }
    return fragments;
}

inline Vec3 DomainPoint(const std::array<Vec3, 3>& domain, Vec3 local)
{
    return domain[0] * local.x + domain[1] * local.y + domain[2] * local.z;
}

struct Projected
{
    double x = 0.0;
    double y = 0.0;
    double t = 0.0;
};

struct Projection
{
    int major = 2;
    int first = 0;
    int second = 1;
};

inline Projection MakeProjection(Vec3 direction)
{
    Projection result;
    if (std::abs(direction.x) >= std::abs(direction.y) && std::abs(direction.x) >= std::abs(direction.z))
    {
        result = { 0, 1, 2 };
    }
    else if (std::abs(direction.y) >= std::abs(direction.z))
    {
        result = { 1, 0, 2 };
    }
    return result;
}

inline NumericalTolerances DeriveTolerances(const std::vector<Triangle>& triangles, const Surface& surface,
    const Ray& ray, Projection projection, const Limits& requested)
{
    double worldScale = std::max({ 1.0, std::abs(ray.origin.x), std::abs(ray.origin.y), std::abs(ray.origin.z),
        std::abs(surface.amplitude) });
    for (const Triangle& triangle : triangles)
    {
        for (size_t corner = 0; corner != 3; ++corner)
        {
            worldScale = std::max(worldScale, Length(triangle.position[corner]) * surface.baseScale +
                std::abs(surface.amplitude) * Length(triangle.direction[corner]));
        }
    }
    const double directionScale = std::max({ std::abs(ray.direction.x), std::abs(ray.direction.y),
        std::abs(ray.direction.z), 1e-300 });
    const double projectedScale = worldScale * directionScale;
    const double epsilon = std::numeric_limits<double>::epsilon();
    NumericalTolerances result;
    result.parameter = std::max(requested.parameterTolerance, 16.0 * epsilon);
    result.residual = std::max(requested.residualTolerance * 10.0, 512.0 * epsilon * projectedScale);
    result.singularResidual = std::max(requested.singularResidualTolerance, 1024.0 * epsilon * projectedScale);
    result.depth = std::max(requested.tTolerance,
        512.0 * epsilon * worldScale / std::abs(ray.direction[projection.major]));
    result.boundary = std::max(8.0 * std::sqrt(epsilon), result.parameter * 0.125);
    return result;
}

inline Projected Project(const Ray& ray, Projection projection, Vec3 position)
{
    const int k = projection.major;
    const int i = projection.first;
    const int j = projection.second;
    const Vec3 relative = position - ray.origin;
    return {
        ray.direction[k] * relative[i] - ray.direction[i] * relative[k],
        ray.direction[k] * relative[j] - ray.direction[j] * relative[k],
        relative[k] / ray.direction[k]
    };
}

inline std::array<Projected, 10> CubicControls(
    const Fragment& fragment, const Surface& surface, const Texture& texture, const Ray& ray, Projection projection)
{
    auto evaluate = [&](Vec3 local)
    {
        return Project(ray, projection, Evaluate(*fragment.triangle, surface, texture, DomainPoint(fragment.domain, local)).position);
    };
    auto add = [](Projected a, Projected b) { return Projected{ a.x + b.x, a.y + b.y, a.t + b.t }; };
    auto subtract = [](Projected a, Projected b) { return Projected{ a.x - b.x, a.y - b.y, a.t - b.t }; };
    auto scale = [](Projected a, double b) { return Projected{ a.x * b, a.y * b, a.t * b }; };

    std::array<Projected, 10> controls{};
    controls[0] = evaluate({ 1, 0, 0 });
    controls[1] = evaluate({ 0, 1, 0 });
    controls[2] = evaluate({ 0, 0, 1 });
    auto edge = [&](int first, int second, Vec3 oneThird, Vec3 twoThird)
    {
        const Projected p1 = evaluate(oneThird);
        const Projected p2 = evaluate(twoThird);
        const Projected a = subtract(subtract(scale(p1, 27.0), scale(controls[first], 8.0)), controls[second]);
        const Projected b = subtract(subtract(scale(p2, 27.0), controls[first]), scale(controls[second], 8.0));
        return std::pair{ scale(subtract(scale(a, 2.0), b), 1.0 / 18.0),
            scale(subtract(scale(b, 2.0), a), 1.0 / 18.0) };
    };
    auto e01 = edge(0, 1, { 2.0 / 3.0, 1.0 / 3.0, 0 }, { 1.0 / 3.0, 2.0 / 3.0, 0 });
    auto e12 = edge(1, 2, { 0, 2.0 / 3.0, 1.0 / 3.0 }, { 0, 1.0 / 3.0, 2.0 / 3.0 });
    auto e20 = edge(2, 0, { 1.0 / 3.0, 0, 2.0 / 3.0 }, { 2.0 / 3.0, 0, 1.0 / 3.0 });
    controls[3] = e01.first;
    controls[4] = e01.second;
    controls[5] = e12.first;
    controls[6] = e12.second;
    controls[7] = e20.first;
    controls[8] = e20.second;
    Projected sumCorners = add(add(controls[0], controls[1]), controls[2]);
    Projected sumEdges{};
    for (int i = 3; i != 9; ++i)
    {
        sumEdges = add(sumEdges, controls[i]);
    }
    controls[9] = scale(subtract(subtract(evaluate({ 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 }),
        scale(sumCorners, 1.0 / 27.0)), scale(sumEdges, 1.0 / 9.0)), 4.5);
    return controls;
}

struct Bounds
{
    double xMin = std::numeric_limits<double>::infinity();
    double xMax = -std::numeric_limits<double>::infinity();
    double yMin = std::numeric_limits<double>::infinity();
    double yMax = -std::numeric_limits<double>::infinity();
    double tMin = std::numeric_limits<double>::infinity();
    double tMax = -std::numeric_limits<double>::infinity();
    bool projectedContainsOrigin = true;
};

inline bool ProjectedContainsOrigin(const std::array<Projected, 10>& controls, double tolerance)
{
    std::vector<Vec2> points;
    points.reserve(controls.size());
    for (const Projected& value : controls)
    {
        points.push_back({ value.x, value.y });
    }
    std::sort(points.begin(), points.end(), [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    points.erase(std::unique(points.begin(), points.end(), [](Vec2 a, Vec2 b)
        { return a.x == b.x && a.y == b.y; }), points.end());
    if (points.size() == 1)
    {
        return std::abs(points[0].x) <= tolerance && std::abs(points[0].y) <= tolerance;
    }
    auto cross2 = [](Vec2 a, Vec2 b, Vec2 c)
    {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    std::vector<Vec2> hull(points.size() * 2);
    size_t count = 0;
    for (const Vec2& point : points)
    {
        while (count >= 2 && cross2(hull[count - 2], hull[count - 1], point) <= 0.0)
        {
            --count;
        }
        hull[count++] = point;
    }
    const size_t lower = count;
    for (size_t i = points.size() - 1; i-- > 0;)
    {
        const Vec2 point = points[i];
        while (count > lower && cross2(hull[count - 2], hull[count - 1], point) <= 0.0)
        {
            --count;
        }
        hull[count++] = point;
    }
    if (count > 1)
    {
        --count;
    }
    if (count == 2)
    {
        const Vec2 a = hull[0];
        const Vec2 b = hull[1];
        const double cross = a.x * b.y - a.y * b.x;
        const double dot = a.x * b.x + a.y * b.y;
        return std::abs(cross) <= tolerance && dot <= tolerance;
    }
    for (size_t i = 0; i < count; ++i)
    {
        const Vec2 a = hull[i];
        const Vec2 b = hull[(i + 1) % count];
        if (a.x * b.y - a.y * b.x < -tolerance)
        {
            return false;
        }
    }
    return true;
}

inline Bounds Bound(const std::array<Projected, 10>& controls)
{
    Bounds result;
    double magnitude = 1.0;
    for (const Projected& value : controls)
    {
        result.xMin = std::min(result.xMin, value.x);
        result.xMax = std::max(result.xMax, value.x);
        result.yMin = std::min(result.yMin, value.y);
        result.yMax = std::max(result.yMax, value.y);
        result.tMin = std::min(result.tMin, value.t);
        result.tMax = std::max(result.tMax, value.t);
        magnitude = std::max({ magnitude, std::abs(value.x), std::abs(value.y), std::abs(value.t) });
    }
    const double outward = std::nextafter(magnitude * 64.0 * std::numeric_limits<double>::epsilon(),
        std::numeric_limits<double>::infinity());
    result.xMin -= outward;
    result.xMax += outward;
    result.yMin -= outward;
    result.yMax += outward;
    result.tMin -= outward;
    result.tMax += outward;
    result.projectedContainsOrigin = ProjectedContainsOrigin(controls, outward * magnitude * 4.0);
    return result;
}

inline bool Contains(Vec3 barycentric, double tolerance = 1e-10)
{
    return barycentric.x >= -tolerance && barycentric.y >= -tolerance && barycentric.z >= -tolerance &&
        barycentric.x <= 1.0 + tolerance && barycentric.y <= 1.0 + tolerance && barycentric.z <= 1.0 + tolerance;
}

inline bool DomainContains(const std::array<Vec3, 3>& domain, Vec3 barycentric, double tolerance = 1e-7)
{
    const Vec3 e0 = domain[1] - domain[0];
    const Vec3 e1 = domain[2] - domain[0];
    const Vec3 relative = barycentric - domain[0];
    const double a = Dot(e0, e0);
    const double b = Dot(e0, e1);
    const double c = Dot(e1, e1);
    const double d = Dot(relative, e0);
    const double e = Dot(relative, e1);
    const double determinant = a * c - b * b;
    if (std::abs(determinant) < 1e-24)
    {
        return false;
    }
    const double u = (d * c - e * b) / determinant;
    const double v = (e * a - d * b) / determinant;
    return u >= -tolerance && v >= -tolerance && u + v <= 1.0 + tolerance;
}

inline bool WindingCertificate(const Fragment& fragment, const Surface& surface, const Texture& texture,
    const Ray& ray, Projection projection)
{
    constexpr int samplesPerEdge = 24;
    constexpr double pi = 3.1415926535897932384626433832795;
    const std::array<Vec3, 3> corners{ Vec3{ 1, 0, 0 }, Vec3{ 0, 1, 0 }, Vec3{ 0, 0, 1 } };
    double winding = 0.0;
    bool havePrevious = false;
    double previousAngle = 0.0;
    double firstAngle = 0.0;
    for (int edge = 0; edge != 3; ++edge)
    {
        for (int sampleIndex = 0; sampleIndex != samplesPerEdge; ++sampleIndex)
        {
            const double t = double(sampleIndex) / samplesPerEdge;
            const Vec3 local = corners[edge] * (1.0 - t) + corners[(edge + 1) % 3] * t;
            const Vec3 barycentric = DomainPoint(fragment.domain, local);
            const Projected projected = Project(ray, projection,
                Evaluate(*fragment.triangle, surface, texture, barycentric).position);
            if (projected.x * projected.x + projected.y * projected.y < 1e-30)
            {
                return false;
            }
            const double angle = std::atan2(projected.y, projected.x);
            if (!havePrevious)
            {
                firstAngle = angle;
                havePrevious = true;
            }
            else
            {
                double delta = angle - previousAngle;
                while (delta > pi) delta -= 2.0 * pi;
                while (delta < -pi) delta += 2.0 * pi;
                winding += delta;
            }
            previousAngle = angle;
        }
    }
    double closing = firstAngle - previousAngle;
    while (closing > pi) closing -= 2.0 * pi;
    while (closing < -pi) closing += 2.0 * pi;
    winding += closing;
    return std::abs(winding) > pi;
}

inline bool SolveCandidate(const Fragment& fragment, const Surface& surface, const Texture& texture,
    const Ray& ray, Projection projection, const NumericalTolerances& tolerances,
    Vec3& barycentric, double& t, bool& singular)
{
    Vec3 local{ 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0 };
    singular = false;
    for (int iteration = 0; iteration != 64; ++iteration)
    {
        barycentric = DomainPoint(fragment.domain, local);
        const Sample sample = Evaluate(*fragment.triangle, surface, texture, barycentric);
        const Projected value = Project(ray, projection, sample.position);
        const Vec3 baryU = fragment.domain[1] - fragment.domain[0];
        const Vec3 baryV = fragment.domain[2] - fragment.domain[0];
        const Vec3 surfaceU = sample.du * baryU.y + sample.dv * baryU.z;
        const Vec3 surfaceV = sample.du * baryV.y + sample.dv * baryV.z;
        const Projected projectedU = Project({ {}, ray.direction, 0, 0 }, projection, surfaceU);
        const Projected projectedV = Project({ {}, ray.direction, 0, 0 }, projection, surfaceV);
        const double determinant = projectedU.x * projectedV.y - projectedV.x * projectedU.y;
        const double scale = std::max({ 1e-300, std::abs(projectedU.x), std::abs(projectedU.y),
            std::abs(projectedV.x), std::abs(projectedV.y) });
        double deltaU = 0.0;
        double deltaV = 0.0;
        if (std::abs(determinant) < 1e-13 * scale * scale)
        {
            singular = true;
            // Damped least squares continues toward an isolated multiple root when
            // ordinary Newton is singular at a tangent contact.
            const double aa = projectedU.x * projectedU.x + projectedU.y * projectedU.y;
            const double ab = projectedU.x * projectedV.x + projectedU.y * projectedV.y;
            const double bb = projectedV.x * projectedV.x + projectedV.y * projectedV.y;
            const double ga = projectedU.x * value.x + projectedU.y * value.y;
            const double gb = projectedV.x * value.x + projectedV.y * value.y;
            const double residual = std::max(std::abs(value.x), std::abs(value.y));
            const double damping = std::max(1e-30, residual * 1e-12) * scale * scale;
            const double dampedDeterminant = (aa + damping) * (bb + damping) - ab * ab;
            if (dampedDeterminant == 0.0)
            {
                break;
            }
            deltaU = (-(bb + damping) * ga + ab * gb) / dampedDeterminant;
            deltaV = (ab * ga - (aa + damping) * gb) / dampedDeterminant;
        }
        else
        {
            deltaU = (-value.x * projectedV.y + projectedV.x * value.y) / determinant;
            deltaV = (-projectedU.x * value.y + value.x * projectedU.y) / determinant;
        }
        local.y += deltaU;
        local.z += deltaV;
        local.x = 1.0 - local.y - local.z;
        if (std::max(std::abs(deltaU), std::abs(deltaV)) < 1e-15)
        {
            break;
        }
    }
    {
        const Vec3 bary = DomainPoint(fragment.domain, local);
        const Sample sample = Evaluate(*fragment.triangle, surface, texture, bary);
        const Projected projectedU = Project({ {}, ray.direction, 0, 0 }, projection, sample.du);
        const Projected projectedV = Project({ {}, ray.direction, 0, 0 }, projection, sample.dv);
        const double globalScale = std::max({ 1.0, std::abs(projectedU.x), std::abs(projectedU.y),
            std::abs(projectedV.x), std::abs(projectedV.y) });
        const double globalDeterminant = (projectedU.x * projectedV.y - projectedV.x * projectedU.y) /
            (globalScale * globalScale);
        singular = singular || std::abs(globalDeterminant) < 1e-7;
    }
    if (singular)
    {
        // Deflate a generic tangent by adding the rank-loss condition det(J)=0.
        // This turns an isolated double contact into a regular least-squares root.
        auto deflated = [&](Vec3 point)
        {
            const Vec3 bary = DomainPoint(fragment.domain, point);
            const Sample sample = Evaluate(*fragment.triangle, surface, texture, bary);
            const Projected value = Project(ray, projection, sample.position);
            // Use the original triangle parameter derivatives here. The current
            // subdivision domain may be tiny; scaling the determinant by that
            // domain would erase the rank-loss signal that performs deflation.
            const Projected projectedU = Project({ {}, ray.direction, 0, 0 }, projection, sample.du);
            const Projected projectedV = Project({ {}, ray.direction, 0, 0 }, projection, sample.dv);
            const double jacobianScale = std::max({ 1.0, std::abs(projectedU.x), std::abs(projectedU.y),
                std::abs(projectedV.x), std::abs(projectedV.y) });
            const double determinant = (projectedU.x * projectedV.y - projectedV.x * projectedU.y) /
                (jacobianScale * jacobianScale);
            return Vec3{ value.x, value.y, determinant };
        };
        for (int iteration = 0; iteration != 32; ++iteration)
        {
            const Vec3 residual = deflated(local);
            const double step = 1e-6;
            Vec3 localUPlus = local;
            Vec3 localUMinus = local;
            Vec3 localVPlus = local;
            Vec3 localVMinus = local;
            localUPlus.y += step; localUPlus.x -= step;
            localUMinus.y -= step; localUMinus.x += step;
            localVPlus.z += step; localVPlus.x -= step;
            localVMinus.z -= step; localVMinus.x += step;
            const Vec3 derivativeU = (deflated(localUPlus) - deflated(localUMinus)) / (2.0 * step);
            const Vec3 derivativeV = (deflated(localVPlus) - deflated(localVMinus)) / (2.0 * step);
            const double aa = Dot(derivativeU, derivativeU);
            const double ab = Dot(derivativeU, derivativeV);
            const double bb = Dot(derivativeV, derivativeV);
            const double ga = Dot(derivativeU, residual);
            const double gb = Dot(derivativeV, residual);
            const double damping = 1e-24;
            const double determinant = (aa + damping) * (bb + damping) - ab * ab;
            if (determinant == 0.0)
            {
                break;
            }
            const double deltaU = (-(bb + damping) * ga + ab * gb) / determinant;
            const double deltaV = (ab * ga - (aa + damping) * gb) / determinant;
            local.y += deltaU;
            local.z += deltaV;
            local.x = 1.0 - local.y - local.z;
            if (std::max(std::abs(deltaU), std::abs(deltaV)) < 1e-14)
            {
                break;
            }
        }
    }
    if (!Contains(local, 1e-7))
    {
        return false;
    }
    barycentric = DomainPoint(fragment.domain, local);
    const Sample sample = Evaluate(*fragment.triangle, surface, texture, barycentric);
    const Projected value = Project(ray, projection, sample.position);
    const double residual = std::max(std::abs(value.x), std::abs(value.y));
    t = value.t;
    const double acceptedResidual = singular ? tolerances.singularResidual : tolerances.residual;
    bool singularCertified = true;
    if (singular)
    {
        const Projected projectedU = Project({ {}, ray.direction, 0, 0 }, projection, sample.du);
        const Projected projectedV = Project({ {}, ray.direction, 0, 0 }, projection, sample.dv);
        const double scale = std::max({ 1.0, std::abs(projectedU.x), std::abs(projectedU.y),
            std::abs(projectedV.x), std::abs(projectedV.y) });
        const double determinant = (projectedU.x * projectedV.y - projectedV.x * projectedU.y) / (scale * scale);
        singularCertified = std::abs(determinant) <= std::max(2e-10, tolerances.residual * 8.0);
    }
    const double minimumLocal = std::min({ local.x, local.y, local.z });
    const bool boundaryCertificate = minimumLocal <= tolerances.boundary && residual <= tolerances.residual;
    const bool existenceCertificate = singular ? singularCertified :
        (boundaryCertificate || WindingCertificate(fragment, surface, texture, ray, projection));
    return residual <= acceptedResidual && existenceCertificate && t >= ray.tMin - tolerances.depth &&
        t <= ray.tMax + tolerances.depth && sample.validNormal;
}

inline std::array<std::array<Vec3, 3>, 4> Subdivide(const std::array<Vec3, 3>& domain)
{
    const Vec3 ab = (domain[0] + domain[1]) * 0.5;
    const Vec3 bc = (domain[1] + domain[2]) * 0.5;
    const Vec3 ca = (domain[2] + domain[0]) * 0.5;
    return { std::array<Vec3, 3>{ domain[0], ab, ca }, std::array<Vec3, 3>{ ab, domain[1], bc },
        std::array<Vec3, 3>{ ca, bc, domain[2] }, std::array<Vec3, 3>{ ab, bc, ca } };
}

struct Work
{
    Fragment fragment{};
    Bounds bounds{};
    uint depth = 0;
};

struct WorkLater
{
    bool operator()(const Work& a, const Work& b) const { return a.bounds.tMin > b.bounds.tMin; }
};

inline double DomainDiameter(const std::array<Vec3, 3>& domain)
{
    return std::max({ Length(domain[0] - domain[1]), Length(domain[1] - domain[2]), Length(domain[2] - domain[0]) });
}

inline std::array<Interval::Jet, 3> CellIntervals(const Fragment& fragment, const Surface& surface,
    const Texture& texture, Interval::Range u, Interval::Range v)
{
    using Interval::Jet;
    const auto& triangle = *fragment.triangle;
    const Jet bu{ u, 1.0, 0.0 }, bv{ v, 0.0, 1.0 };
    auto affine = [&](double a, double b, double c)
    {
        // Subtract inside interval arithmetic, not in binary64 before enclosure.
        return Jet(a) + (Jet(b) - Jet(a)) * bu + (Jet(c) - Jet(a)) * bv;
    };
    const Jet x = affine(triangle.uv[0].x, triangle.uv[1].x, triangle.uv[2].x) * Jet(double(texture.width)) -
        Jet(double(fragment.cellX) + .5);
    const Jet y = affine(triangle.uv[0].y, triangle.uv[1].y, triangle.uv[2].y) * Jet(double(texture.height)) -
        Jet(double(fragment.cellY) + .5);
    const Jet h00(texture.Fetch(fragment.cellX, fragment.cellY, surface.addressMode));
    const Jet h10(texture.Fetch(fragment.cellX + 1, fragment.cellY, surface.addressMode));
    const Jet h01(texture.Fetch(fragment.cellX, fragment.cellY + 1, surface.addressMode));
    const Jet h11(texture.Fetch(fragment.cellX + 1, fragment.cellY + 1, surface.addressMode));
    const Jet displacement = (h00 + (h10 - h00) * x + (h01 - h00) * y +
        (h11 - h10 - h01 + h00) * x * y - Jet(surface.reference)) * Jet(surface.amplitude);
    std::array<Jet, 3> result;
    for (int axis = 0; axis != 3; ++axis)
        result[axis] = affine(triangle.position[0][axis], triangle.position[1][axis], triangle.position[2][axis]) *
            Jet(surface.baseScale) +
            affine(triangle.direction[0][axis], triangle.direction[1][axis], triangle.direction[2][axis]) * displacement;
    return result;
}

struct RegularCertificate
{
    bool included = false;
    bool excluded = false;
    Vec3 barycentric{};
    Interval::Range depth{};
};

inline RegularCertificate CertifyRegular(const Fragment& fragment, const Surface& surface, const Texture& texture,
    const Ray& ray, Projection projection, const NumericalTolerances& tolerances)
{
    using namespace Interval;
    RegularCertificate result;
    Range u{ 1.0, 0.0 }, v{ 1.0, 0.0 };
    for (Vec3 corner : fragment.domain)
    {
        u.lo = std::min(u.lo, corner.y); u.hi = std::max(u.hi, corner.y);
        v.lo = std::min(v.lo, corner.z); v.hi = std::max(v.hi, corner.z);
    }
    // A box covers the entire child; a small extension lets a regular root on
    // an artificial subdivision edge be strictly interior to the proof domain.
    const double padding = std::max(u.Width(), v.Width()) * .125;
    u = { Down(u.lo - padding), Up(u.hi + padding) };
    v = { Down(v.lo - padding), Up(v.hi + padding) };
    bool included = false;
    auto projected = [&](Range x, Range y)
    {
        const auto point = CellIntervals(fragment, surface, texture, x, y);
        return std::array<Jet, 2>{
            (point[projection.first] - Jet(ray.origin[projection.first])) * Jet(ray.direction[projection.major]) -
                (point[projection.major] - Jet(ray.origin[projection.major])) * Jet(ray.direction[projection.first]),
            (point[projection.second] - Jet(ray.origin[projection.second])) * Jet(ray.direction[projection.major]) -
                (point[projection.major] - Jet(ray.origin[projection.major])) * Jet(ray.direction[projection.second]) };
    };
    for (int iteration = 0; iteration != 12; ++iteration)
    {
        const auto function = projected(u, v);
        const Range j00 = function[0].du, j01 = function[0].dv, j10 = function[1].du, j11 = function[1].dv;
        const double a = j00.Midpoint(), b = j01.Midpoint(), c = j10.Midpoint(), d = j11.Midpoint();
        const double determinant = a * d - b * c;
        if (!std::isfinite(determinant) || determinant == 0.0) return result;
        const Range r00(d / determinant), r01(-b / determinant), r10(-c / determinant), r11(a / determinant);
        const Range m00 = Range(1) - r00 * j00 - r01 * j10, m01 = -r00 * j01 - r01 * j11;
        const Range m10 = -r10 * j00 - r11 * j10, m11 = Range(1) - r10 * j01 - r11 * j11;
        const Range middleU(u.Midpoint()), middleV(v.Midpoint());
        const auto center = projected(middleU, middleV);
        const Range ku = middleU - r00 * center[0].value - r01 * center[1].value +
            m00 * (u - middleU) + m01 * (v - middleV);
        const Range kv = middleV - r10 * center[0].value - r11 * center[1].value +
            m10 * (u - middleU) + m11 * (v - middleV);
        if (!ku.Finite() || !kv.Finite()) return result;
        if (Disjoint(ku, u) || Disjoint(kv, v))
        {
            result.excluded = !included;
            return result;
        }
        // Strict inclusion proves existence. The contraction norm additionally
        // proves uniqueness throughout the original box, including all of the
        // child triangle, so skipping this child cannot lose a closer root.
        const bool contraction = Up(m00.Magnitude() + m01.Magnitude()) < 1.0 &&
            Up(m10.Magnitude() + m11.Magnitude()) < 1.0;
        included = included || (contraction && StrictlyInside(ku, u) && StrictlyInside(kv, v));
        if (!included) return result;
        u = Intersection(ku, u); v = Intersection(kv, v);
        const auto& tri = *fragment.triangle;
        const Range w = Range(1) - u - v;
        auto texel = [&](double a0, double a1, double a2, uint size)
        {
            return (Range(a0) + (Range(a1) - Range(a0)) * u + (Range(a2) - Range(a0)) * v) *
                Range(double(size)) - Range(.5);
        };
        const Range qx = texel(tri.uv[0].x, tri.uv[1].x, tri.uv[2].x, texture.width);
        const Range qy = texel(tri.uv[0].y, tri.uv[1].y, tri.uv[2].y, texture.height);
        const bool inSurface = Inside(u, { 0, 1 }) && Inside(v, { 0, 1 }) && Inside(w, { 0, 1 }) &&
            Inside(qx, { double(fragment.cellX), double(fragment.cellX + 1) }) &&
            Inside(qy, { double(fragment.cellY), double(fragment.cellY + 1) });
        result.depth = (CellIntervals(fragment, surface, texture, u, v)[projection.major].value -
            Range(ray.origin[projection.major])) / ray.direction[projection.major];
        if (inSurface && result.depth.Finite() && result.depth.lo >= ray.tMin && result.depth.hi <= ray.tMax &&
            result.depth.Width() <= tolerances.depth * 2.0 && std::max(u.Width(), v.Width()) <= tolerances.parameter)
        {
            result.included = true;
            result.barycentric = { 1.0 - u.Midpoint() - v.Midpoint(), u.Midpoint(), v.Midpoint() };
            return result;
        }
    }
    return result;
}

// Enclose a rational number by comparing the proposed floating-point endpoints
// to its exact numerator/denominator. Conversion is only a locator here too.
inline bool RationalEnclosure(Exact::Dyadic numerator, Exact::Dyadic denominator, Interval::Range& result)
{
    using Exact::Dyadic;
    if (denominator.IsZero()) return false;
    if (denominator.Sign() < 0) { numerator = -numerator; denominator = -denominator; }
    const double estimate = numerator.ToDouble() / denominator.ToDouble();
    if (!std::isfinite(estimate)) return false;
    result = { estimate, estimate };
    for (int attempt = 0; attempt != 16; ++attempt)
    {
        const bool low = (Dyadic(result.lo) * denominator - numerator).Sign() <= 0;
        const bool high = (Dyadic(result.hi) * denominator - numerator).Sign() >= 0;
        if (low && high) return true;
        if (!low) result.lo = Interval::Down(result.lo);
        if (!high) result.hi = Interval::Up(result.hi);
    }
    return false;
}

struct RationalCertificate : RegularCertificate
{
    bool singular = false;
};

inline RationalCertificate CertifyRational(const Fragment& fragment, const Surface& surface, const Texture& texture,
    const Ray& ray, Projection projection)
{
    using Exact::Dyadic;
    // Restricted exact path: affine height and one affine projected constraint.
    // Elimination then leaves a quadratic. A negative discriminant proves a
    // miss; a zero discriminant proves the isolated double root. General cubic
    // systems and positive-dimensional contacts continue through subdivision.
    RationalCertificate result;
    const double heights[4]{ texture.Fetch(fragment.cellX, fragment.cellY, surface.addressMode),
        texture.Fetch(fragment.cellX + 1, fragment.cellY, surface.addressMode),
        texture.Fetch(fragment.cellX, fragment.cellY + 1, surface.addressMode),
        texture.Fetch(fragment.cellX + 1, fragment.cellY + 1, surface.addressMode) };
    if (heights[3] - heights[1] - heights[2] + heights[0] != 0.0) return result;
    if (!(Dyadic(heights[3]) - Dyadic(heights[1]) - Dyadic(heights[2]) + Dyadic(heights[0])).IsZero()) return result;
    using Polynomial = std::array<Dyadic, 6>; // 1, u, v, uu, uv, vv
    using Linear = std::array<Dyadic, 3>;
    auto affine = [](double a, double b, double c) { return Linear{ Dyadic(a), Dyadic(b) - Dyadic(a), Dyadic(c) - Dyadic(a) }; };
    const auto& triangle = *fragment.triangle;
    auto x = affine(triangle.uv[0].x, triangle.uv[1].x, triangle.uv[2].x);
    auto y = affine(triangle.uv[0].y, triangle.uv[1].y, triangle.uv[2].y);
    for (int i = 0; i != 3; ++i) { x[i] = x[i] * Dyadic(double(texture.width)); y[i] = y[i] * Dyadic(double(texture.height)); }
    x[0] = x[0] - Dyadic(double(fragment.cellX) + .5);
    y[0] = y[0] - Dyadic(double(fragment.cellY) + .5);
    Linear height{};
    for (int i = 0; i != 3; ++i)
        height[i] = ((i == 0 ? Dyadic(heights[0]) - Dyadic(surface.reference) : Dyadic{}) +
            x[i] * (Dyadic(heights[1]) - Dyadic(heights[0])) +
            y[i] * (Dyadic(heights[2]) - Dyadic(heights[0]))) * Dyadic(surface.amplitude);
    std::array<Polynomial, 3> position{};
    for (int axis = 0; axis != 3; ++axis)
    {
        const auto p = affine(triangle.position[0][axis], triangle.position[1][axis], triangle.position[2][axis]);
        const auto d = affine(triangle.direction[0][axis], triangle.direction[1][axis], triangle.direction[2][axis]);
        auto& q = position[axis];
        q[0] = p[0] * Dyadic(surface.baseScale) + height[0] * d[0];
        q[1] = p[1] * Dyadic(surface.baseScale) + height[0] * d[1] + height[1] * d[0];
        q[2] = p[2] * Dyadic(surface.baseScale) + height[0] * d[2] + height[2] * d[0];
        q[3] = height[1] * d[1]; q[4] = height[1] * d[2] + height[2] * d[1]; q[5] = height[2] * d[2];
    }
    auto project = [&](int axis)
    {
        Polynomial q{};
        for (int i = 0; i != 6; ++i)
            q[i] = (position[axis][i] - (i == 0 ? Dyadic(ray.origin[axis]) : Dyadic{})) * Dyadic(ray.direction[projection.major]) -
                (position[projection.major][i] - (i == 0 ? Dyadic(ray.origin[projection.major]) : Dyadic{})) * Dyadic(ray.direction[axis]);
        return q;
    };
    Polynomial linear = project(projection.first), quadratic = project(projection.second);
    auto isLinear = [](const Polynomial& p) { return p[3].IsZero() && p[4].IsZero() && p[5].IsZero(); };
    if (!isLinear(linear)) std::swap(linear, quadratic);
    if (!isLinear(linear)) return result;
    bool swap = linear[2].IsZero();
    if (swap)
    {
        std::swap(linear[1], linear[2]); std::swap(quadratic[1], quadratic[2]); std::swap(quadratic[3], quadratic[5]);
    }
    if (linear[2].IsZero()) return result;
    const Dyadic d = linear[2], n0 = -linear[0], n1 = -linear[1], two(2.0);
    const auto& q = quadratic;
    const Dyadic a = q[3] * d * d + q[4] * d * n1 + q[5] * n1 * n1;
    const Dyadic b = q[1] * d * d + q[2] * d * n1 + q[4] * d * n0 + two * q[5] * n0 * n1;
    const Dyadic c = q[0] * d * d + q[2] * d * n0 + q[5] * n0 * n0;
    Dyadic rootNumerator, rootDenominator;
    if (a.IsZero())
    {
        if (b.IsZero()) { result.excluded = !c.IsZero(); return result; }
        rootNumerator = -c; rootDenominator = b;
    }
    else
    {
        const Dyadic discriminant = b * b - Dyadic(4.0) * a * c;
        if (discriminant.Sign() < 0) { result.excluded = true; return result; }
        if (!discriminant.IsZero()) return result;
        rootNumerator = -b; rootDenominator = two * a;
        result.singular = true;
    }
    Dyadic denominator = rootDenominator * d;
    Dyadic u = rootNumerator * d, v = n0 * rootDenominator + n1 * rootNumerator;
    if (swap) std::swap(u, v);
    if (denominator.Sign() < 0) { denominator = -denominator; u = -u; v = -v; }
    const Dyadic w = denominator - u - v;
    auto closedUnit = [&](const Dyadic& value) { return value.Sign() >= 0 && (denominator - value).Sign() >= 0; };
    const Dyadic cellU = x[0] * denominator + x[1] * u + x[2] * v;
    const Dyadic cellV = y[0] * denominator + y[1] * u + y[2] * v;
    if (!closedUnit(u) || !closedUnit(v) || !closedUnit(w) || !closedUnit(cellU) || !closedUnit(cellV))
    { result.excluded = true; return result; }
    const auto& depth = position[projection.major];
    const Dyadic squared = denominator * denominator;
    Dyadic tNumerator = (depth[0] - Dyadic(ray.origin[projection.major])) * squared +
        depth[1] * u * denominator + depth[2] * v * denominator + depth[3] * u * u + depth[4] * u * v + depth[5] * v * v;
    Dyadic tDenominator = squared * Dyadic(ray.direction[projection.major]);
    if (tDenominator.Sign() < 0) { tNumerator = -tNumerator; tDenominator = -tDenominator; }
    if ((tNumerator - Dyadic(ray.tMin) * tDenominator).Sign() < 0 ||
        (std::isfinite(ray.tMax) && (tNumerator - Dyadic(ray.tMax) * tDenominator).Sign() > 0))
    { result.excluded = true; return result; }
    Interval::Range rootU, rootV;
    if (!RationalEnclosure(u, denominator, rootU) || !RationalEnclosure(v, denominator, rootV) ||
        !RationalEnclosure(tNumerator, tDenominator, result.depth)) return result;
    result.barycentric = { 1.0 - rootU.Midpoint() - rootV.Midpoint(), rootU.Midpoint(), rootV.Midpoint() };
    result.included = true;
    return result;
}

inline Hit Intersect(const std::vector<Triangle>& triangles, const Surface& surface, const Texture& texture,
    const Ray& ray, Limits limits = {})
{
    Hit result;
    if (triangles.empty())
    {
        return result;
    }
    if (!std::isfinite(limits.parameterTolerance) || limits.parameterTolerance < 0 ||
        !std::isfinite(limits.residualTolerance) || limits.residualTolerance < 0 ||
        !std::isfinite(limits.singularResidualTolerance) || limits.singularResidualTolerance < 0 ||
        !std::isfinite(limits.tTolerance) || limits.tTolerance < 0)
    { result.status = Status::Invalid; return result; }
    for (double height : texture.pixels)
        if (!std::isfinite(height) || height < 0.0 || height > 1.0)
        { result.status = Status::Invalid; return result; }
    for (const Triangle& triangle : triangles)
    {
        if (!Valid(triangle, surface, texture, ray))
        {
            result.status = Status::Invalid;
            return result;
        }
    }
    const Projection projection = MakeProjection(ray.direction);
    const NumericalTolerances tolerances = DeriveTolerances(triangles, surface, ray, projection, limits);
    std::priority_queue<Work, std::vector<Work>, WorkLater> queue;
    auto enqueue = [&](Fragment fragment, uint depth)
    {
        const Bounds bounds = Bound(CubicControls(fragment, surface, texture, ray, projection));
        if (!bounds.projectedContainsOrigin || bounds.xMin > 0.0 || bounds.xMax < 0.0 || bounds.yMin > 0.0 || bounds.yMax < 0.0 ||
            bounds.tMax < ray.tMin || bounds.tMin > ray.tMax || bounds.tMin > result.tUpper)
        {
            return;
        }
        queue.push({ fragment, bounds, depth });
    };
    for (const Triangle& triangle : triangles)
    {
        auto fragments = BuildFragments(triangle, texture);
        result.visitedFragments += uint(fragments.size());
        for (const Fragment& fragment : fragments)
        {
            enqueue(fragment, 0);
        }
    }
    struct Unresolved
    {
        Bounds bounds{};
        uint reason = ExhaustionNone;
    };
    std::vector<Unresolved> unresolved;
    while (!queue.empty())
    {
        if (result.subdivisionNodes >= limits.maxNodes)
        {
            // Preserve every queued candidate for both nearest-hit correctness and
            // complete diagnostics.  Keeping only queue.top() happened to preserve
            // the minimum lower bound, but lost the unresolved upper range and made
            // future ordering changes unsafe.
            while (!queue.empty())
            {
                unresolved.push_back({ queue.top().bounds, ExhaustionNodeBudget });
                queue.pop();
            }
            break;
        }
        Work work = queue.top();
        queue.pop();
        ++result.subdivisionNodes;
        result.maximumDepth = std::max(result.maximumDepth, work.depth);
        if (work.bounds.tMin > result.tUpper)
        {
            continue;
        }
        if (work.depth == 0)
        {
            const auto certificate = CertifyRational(work.fragment, surface, texture, ray, projection);
            if (certificate.excluded) continue;
            if (certificate.included && certificate.depth.Width() <= tolerances.depth * 2)
            {
                const Sample sample = Evaluate(*work.fragment.triangle, surface, texture, certificate.barycentric);
                if (!sample.validNormal) { result.status = Status::Invalid; return result; }
                ++result.rationalCertificates;
                if (certificate.depth.hi < result.tLower ||
                    (certificate.depth.lo <= result.tUpper && work.fragment.triangle->primitiveId < result.primitiveId))
                {
                    result.status = Status::Hit; result.primitiveId = work.fragment.triangle->primitiveId;
                    result.tLower = certificate.depth.lo; result.tUpper = certificate.depth.hi;
                    result.barycentric = certificate.barycentric; result.position = sample.position;
                    result.uv = sample.uv; result.normal = sample.normal; result.singularPath = certificate.singular;
                }
                continue;
            }
        }
        if (work.depth >= 3)
        {
            const auto certificate = CertifyRegular(work.fragment, surface, texture, ray, projection, tolerances);
            if (certificate.excluded) continue;
            if (certificate.included)
            {
                const Sample sample = Evaluate(*work.fragment.triangle, surface, texture, certificate.barycentric);
                if (!sample.validNormal) { result.status = Status::Invalid; return result; }
                ++result.regularCertificates;
                if (certificate.depth.hi < result.tLower ||
                    (certificate.depth.lo <= result.tUpper && work.fragment.triangle->primitiveId < result.primitiveId))
                {
                    result.status = Status::Hit;
                    result.primitiveId = work.fragment.triangle->primitiveId;
                    result.tLower = certificate.depth.lo;
                    result.tUpper = certificate.depth.hi;
                    result.barycentric = certificate.barycentric;
                    result.position = sample.position;
                    result.uv = sample.uv;
                    result.normal = sample.normal;
                    result.singularPath = false;
                }
                continue;
            }
        }
        const bool leaf = work.depth >= limits.maxDepth || DomainDiameter(work.fragment.domain) <= tolerances.parameter ||
            (work.bounds.tMax - work.bounds.tMin) <= tolerances.depth;
        if (leaf)
        {
            Vec3 barycentric{};
            double t = 0.0;
            bool singular = false;
            if (SolveCandidate(work.fragment, surface, texture, ray, projection, tolerances, barycentric, t, singular) && !singular)
            {
                const Sample sample = Evaluate(*work.fragment.triangle, surface, texture, barycentric);
                double error = std::max(tolerances.depth,
                    32.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(t)));
                if (t + error < result.tUpper || (std::abs(t - result.tUpper) <= error &&
                    work.fragment.triangle->primitiveId < result.primitiveId))
                {
                    result.status = Status::Hit;
                    result.primitiveId = work.fragment.triangle->primitiveId;
                    result.tLower = t - error;
                    result.tUpper = t + error;
                    result.barycentric = barycentric;
                    result.position = sample.position;
                    result.uv = sample.uv;
                    result.normal = sample.normal;
                    result.singularPath = singular;
                }
            }
            else
            {
                // Closed cell fragments and adjacent source triangles deliberately overlap on
                // boundaries. Once a certified owner hit exists, a leaf containing that same
                // parameter point is a duplicate representation rather than an unresolved
                // nearer interval.
                const bool samePrimitiveBoundary = work.fragment.triangle->primitiveId == result.primitiveId &&
                    DomainContains(work.fragment.domain, result.barycentric);
                const double tieMargin = std::max(tolerances.depth * 64.0,
                    128.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(result.tUpper)));
                const bool tiedPrimitiveBoundary = work.bounds.tMin >= result.tLower - tieMargin &&
                    work.bounds.tMax <= result.tUpper + tieMargin;
                const bool duplicateBoundary = result.status == Status::Hit &&
                    work.bounds.tMin <= result.tUpper + tieMargin && work.bounds.tMax >= result.tLower - tieMargin &&
                    (samePrimitiveBoundary || tiedPrimitiveBoundary);
                if (!duplicateBoundary)
                {
                    uint reason = ExhaustionUncertifiedLeaf;
                    if (work.depth >= limits.maxDepth) reason |= ExhaustionMaximumDepth;
                    if (DomainDiameter(work.fragment.domain) <= tolerances.parameter)
                        reason |= ExhaustionParameterResolution;
                    if (work.bounds.tMax - work.bounds.tMin <= tolerances.depth)
                        reason |= ExhaustionDepthResolution;
                    unresolved.push_back({ work.bounds, reason });
                }
                else
                {
                    result.tLower = std::min(result.tLower, work.bounds.tMin);
                    result.tUpper = std::max(result.tUpper, work.bounds.tMax);
                }
            }
            continue;
        }
        for (const auto& child : Subdivide(work.fragment.domain))
        {
            Fragment fragment = work.fragment;
            fragment.domain = child;
            enqueue(fragment, work.depth + 1);
        }
    }
    bool blockingUnresolved = false;
    result.lowestUnresolvedT = std::numeric_limits<double>::infinity();
    result.highestUnresolvedT = -std::numeric_limits<double>::infinity();
    for (const Unresolved& pending : unresolved)
    {
        const Bounds& bounds = pending.bounds;
        const double tieMargin = std::max(tolerances.depth * 64.0,
            128.0 * std::numeric_limits<double>::epsilon() * std::max(1.0, std::abs(result.tUpper)));
        const bool tied = result.status == Status::Hit && bounds.tMin >= result.tLower - tieMargin &&
            bounds.tMax <= result.tUpper + tieMargin;
        const bool farther = result.status == Status::Hit && bounds.tMin > result.tUpper;
        if (!tied && !farther)
        {
            blockingUnresolved = true;
            result.lowestUnresolvedT = std::min(result.lowestUnresolvedT, bounds.tMin);
            result.highestUnresolvedT = std::max(result.highestUnresolvedT, bounds.tMax);
            result.exhaustionReasons |= pending.reason;
        }
    }
    if (blockingUnresolved)
    {
        result.status = Status::Exhausted;
    }
    return result;
}

// Independent reference: recursively tessellate the exact surface and intersect its
// converging linear approximation. It intentionally shares neither polynomial bounds
// nor candidate traversal. Refinement convergence is checked by the tests.
inline bool RayTriangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c, double& t, Vec3& barycentric)
{
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 p = Cross(ray.direction, e2);
    const double determinant = Dot(e1, p);
    if (std::abs(determinant) < 1e-15)
    {
        return false;
    }
    const double inverse = 1.0 / determinant;
    const Vec3 s = ray.origin - a;
    const double u = Dot(s, p) * inverse;
    const Vec3 q = Cross(s, e1);
    const double v = Dot(ray.direction, q) * inverse;
    const double candidate = Dot(e2, q) * inverse;
    if (u < -1e-12 || v < -1e-12 || u + v > 1.0 + 1e-12 || candidate < ray.tMin || candidate > ray.tMax)
    {
        return false;
    }
    t = candidate;
    barycentric = { 1.0 - u - v, u, v };
    return true;
}

inline std::vector<std::pair<double, uint>> ReferenceAll(const std::vector<Triangle>& triangles, const Surface& surface,
    const Texture& texture, const Ray& ray, uint divisions)
{
    std::vector<std::pair<double, uint>> roots;
    for (const Triangle& triangle : triangles)
    {
        for (uint y = 0; y < divisions; ++y)
        {
            for (uint x = 0; x + y < divisions; ++x)
            {
                const auto bary = [&](uint i, uint j)
                {
                    return Vec3{ 1.0 - double(i + j) / divisions, double(i) / divisions, double(j) / divisions };
                };
                const Vec3 b0 = bary(x, y);
                const Vec3 b1 = bary(x + 1, y);
                const Vec3 b2 = bary(x, y + 1);
                double t = 0.0;
                Vec3 local{};
                if (RayTriangle(ray, Evaluate(triangle, surface, texture, b0).position,
                    Evaluate(triangle, surface, texture, b1).position, Evaluate(triangle, surface, texture, b2).position, t, local))
                {
                    roots.emplace_back(t, triangle.primitiveId);
                }
                if (x + y + 1 < divisions)
                {
                    const Vec3 b3 = bary(x + 1, y + 1);
                    if (RayTriangle(ray, Evaluate(triangle, surface, texture, b1).position,
                        Evaluate(triangle, surface, texture, b3).position, Evaluate(triangle, surface, texture, b2).position, t, local))
                    {
                        roots.emplace_back(t, triangle.primitiveId);
                    }
                }
            }
        }
    }
    std::sort(roots.begin(), roots.end());
    std::vector<std::pair<double, uint>> unique;
    for (const auto& root : roots)
    {
        if (unique.empty() || std::abs(root.first - unique.back().first) > 2e-6 || root.second != unique.back().second)
        {
            unique.push_back(root);
        }
    }
    return unique;
}
}
