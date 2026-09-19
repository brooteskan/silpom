// SPDX-License-Identifier: MIT
#pragma once

#include "CurvedPrototype.h"
#include "CurvedDyadic.h"

#include <functional>
#include <numeric>

namespace SilPOM::Curved::Exact
{
// The oracle deliberately has no dependency on the candidate patch construction.
// IEEE-754 inputs are converted to integers times powers of two, resultants are
// formed exactly, and a signed pseudo-remainder Sturm chain isolates every real
// resultant root.  Floating point is used only after isolation to pair the two
// algebraic coordinates and evaluate the public surface contract.

using IntegerPolynomial = std::vector<Integer>;

inline void Trim(IntegerPolynomial& polynomial)
{
    while (!polynomial.empty() && polynomial.back().IsZero())
    {
        polynomial.pop_back();
    }
}

inline IntegerPolynomial Add(const IntegerPolynomial& a, const IntegerPolynomial& b)
{
    IntegerPolynomial result(std::max(a.size(), b.size()));
    for (size_t i = 0; i != result.size(); ++i)
    {
        result[i] = (i < a.size() ? a[i] : Integer{}) + (i < b.size() ? b[i] : Integer{});
    }
    Trim(result);
    return result;
}

inline IntegerPolynomial Multiply(const IntegerPolynomial& a, const IntegerPolynomial& b)
{
    if (a.empty() || b.empty()) return {};
    IntegerPolynomial result(a.size() + b.size() - 1);
    for (size_t i = 0; i != a.size(); ++i)
    {
        for (size_t j = 0; j != b.size(); ++j)
        {
            result[i + j] = result[i + j] + a[i] * b[j];
        }
    }
    Trim(result);
    return result;
}

inline IntegerPolynomial Negate(IntegerPolynomial value)
{
    for (Integer& coefficient : value) coefficient = -coefficient;
    return value;
}

inline void RemovePowerOfTwoContent(IntegerPolynomial& polynomial)
{
    unsigned common = std::numeric_limits<unsigned>::max();
    for (const Integer& coefficient : polynomial)
    {
        if (!coefficient.IsZero()) common = std::min(common, coefficient.TrailingZeroBits());
    }
    if (common != std::numeric_limits<unsigned>::max() && common != 0)
    {
        for (Integer& coefficient : polynomial) coefficient.ShiftRightExact(common);
    }
}

inline IntegerPolynomial Derivative(const IntegerPolynomial& polynomial)
{
    if (polynomial.size() <= 1) return {};
    IntegerPolynomial result(polynomial.size() - 1);
    for (size_t i = 1; i != polynomial.size(); ++i)
    {
        result[i - 1] = polynomial[i] * Integer(std::int64_t(i));
    }
    Trim(result);
    return result;
}

inline IntegerPolynomial NegativeRemainder(const IntegerPolynomial& dividend, const IntegerPolynomial& divisor)
{
    IntegerPolynomial remainder = dividend;
    const int divisorDegree = int(divisor.size()) - 1;
    const Integer leading = divisor.back();
    unsigned iterations = 0;
    while (!remainder.empty() && int(remainder.size()) - 1 >= divisorDegree)
    {
        const int offset = int(remainder.size()) - 1 - divisorDegree;
        const Integer factor = remainder.back();
        for (Integer& coefficient : remainder) coefficient = coefficient * leading;
        for (int i = 0; i <= divisorDegree; ++i)
        {
            remainder[size_t(i + offset)] = remainder[size_t(i + offset)] - factor * divisor[size_t(i)];
        }
        Trim(remainder);
        ++iterations;
    }
    // The pseudo remainder is leading^iterations times the field remainder.
    // Correct its sign before applying Sturm's leading minus.
    const bool pseudoScaleNegative = leading.Negative() && (iterations & 1u) != 0;
    if (!pseudoScaleNegative)
    {
        remainder = Negate(std::move(remainder));
    }
    RemovePowerOfTwoContent(remainder);
    return remainder;
}

inline std::vector<IntegerPolynomial> SturmChain(IntegerPolynomial polynomial)
{
    Trim(polynomial);
    RemovePowerOfTwoContent(polynomial);
    std::vector<IntegerPolynomial> chain;
    if (polynomial.empty()) return chain;
    chain.push_back(std::move(polynomial));
    IntegerPolynomial derivative = Derivative(chain.front());
    if (derivative.empty()) return chain;
    RemovePowerOfTwoContent(derivative);
    chain.push_back(std::move(derivative));
    while (!chain.back().empty())
    {
        IntegerPolynomial next = NegativeRemainder(chain[chain.size() - 2], chain.back());
        if (next.empty()) break;
        chain.push_back(std::move(next));
        if (chain.size() > 32) break;
    }
    return chain;
}

inline Dyadic Evaluate(const IntegerPolynomial& polynomial, double value)
{
    const Dyadic argument(value);
    Dyadic result;
    for (size_t i = polynomial.size(); i-- > 0;)
    {
        result = result * argument + Dyadic(polynomial[i]);
    }
    return result;
}

inline int SideSign(IntegerPolynomial polynomial, double value, bool right)
{
    unsigned derivativeOrder = 0;
    while (!polynomial.empty())
    {
        const int sign = Evaluate(polynomial, value).Sign();
        if (sign != 0)
        {
            return !right && (derivativeOrder & 1u) != 0 ? -sign : sign;
        }
        polynomial = Derivative(polynomial);
        ++derivativeOrder;
    }
    return 0;
}

inline int Variations(const std::vector<IntegerPolynomial>& chain, double value, bool right)
{
    int previous = 0;
    int variations = 0;
    for (const IntegerPolynomial& polynomial : chain)
    {
        const int sign = SideSign(polynomial, value, right);
        if (sign != 0)
        {
            if (previous != 0 && sign != previous) ++variations;
            previous = sign;
        }
    }
    return variations;
}

struct RootInterval
{
    double lower = 0.0;
    double upper = 0.0;
};

inline std::vector<RootInterval> IsolateUnitRoots(const IntegerPolynomial& polynomial)
{
    std::vector<RootInterval> roots;
    if (polynomial.size() <= 1) return roots;
    const auto chain = SturmChain(polynomial);
    if (Evaluate(polynomial, 0.0).IsZero()) roots.push_back({ 0.0, 0.0 });
    if (Evaluate(polynomial, 1.0).IsZero()) roots.push_back({ 1.0, 1.0 });
    struct Interval { double lower; double upper; int count; unsigned depth; };
    std::vector<Interval> pending;
    const int interior = Variations(chain, 0.0, true) - Variations(chain, 1.0, false);
    if (interior > 0) pending.push_back({ 0.0, 1.0, interior, 0 });
    while (!pending.empty())
    {
        const Interval interval = pending.back();
        pending.pop_back();
        if (interval.count == 1 && (interval.depth >= 48 || interval.upper - interval.lower <= 4e-14))
        {
            roots.push_back({ interval.lower, interval.upper });
            continue;
        }
        if (interval.depth >= 56)
        {
            roots.push_back({ interval.lower, interval.upper });
            continue;
        }
        const double middle = (interval.lower + interval.upper) * 0.5;
        const bool middleRoot = Evaluate(polynomial, middle).IsZero();
        if (middleRoot) roots.push_back({ middle, middle });
        const int left = Variations(chain, interval.lower, true) - Variations(chain, middle, false);
        const int right = Variations(chain, middle, true) - Variations(chain, interval.upper, false);
        if (right > 0) pending.push_back({ middle, interval.upper, right, interval.depth + 1 });
        if (left > 0) pending.push_back({ interval.lower, middle, left, interval.depth + 1 });
    }
    std::sort(roots.begin(), roots.end(), [](const RootInterval& a, const RootInterval& b) { return a.lower < b.lower; });
    roots.erase(std::unique(roots.begin(), roots.end(), [](const RootInterval& a, const RootInterval& b)
        { return std::max(a.lower, b.lower) <= std::min(a.upper, b.upper) + 1e-15; }), roots.end());
    return roots;
}

struct Bivariate
{
    std::array<std::array<Dyadic, 4>, 4> coefficient{}; // coefficient[x power][y power]
};

inline Bivariate Constant(double value)
{
    Bivariate result;
    result.coefficient[0][0] = Dyadic(value);
    return result;
}

inline Bivariate Affine(double atZero, double xDelta, double yDelta)
{
    Bivariate result = Constant(atZero);
    result.coefficient[1][0] = Dyadic(xDelta);
    result.coefficient[0][1] = Dyadic(yDelta);
    return result;
}

inline Bivariate AffineCorners(double first, double second, double third)
{
    Bivariate result = Constant(first);
    result.coefficient[1][0] = Dyadic(second) - Dyadic(first);
    result.coefficient[0][1] = Dyadic(third) - Dyadic(first);
    return result;
}

inline Bivariate operator+(const Bivariate& a, const Bivariate& b)
{
    Bivariate result;
    for (int x = 0; x != 4; ++x) for (int y = 0; y != 4; ++y)
        result.coefficient[x][y] = a.coefficient[x][y] + b.coefficient[x][y];
    return result;
}

inline Bivariate operator-(const Bivariate& a, const Bivariate& b)
{
    Bivariate result;
    for (int x = 0; x != 4; ++x) for (int y = 0; y != 4; ++y)
        result.coefficient[x][y] = a.coefficient[x][y] - b.coefficient[x][y];
    return result;
}

inline Bivariate operator*(const Bivariate& a, const Bivariate& b)
{
    Bivariate result;
    for (int ax = 0; ax != 4; ++ax) for (int ay = 0; ay != 4; ++ay)
        for (int bx = 0; bx + ax != 4; ++bx) for (int by = 0; by + ay != 4; ++by)
            result.coefficient[ax + bx][ay + by] = result.coefficient[ax + bx][ay + by] +
                a.coefficient[ax][ay] * b.coefficient[bx][by];
    return result;
}

inline Bivariate operator*(const Bivariate& value, double scale)
{
    Bivariate result;
    const Dyadic exactScale(scale);
    for (int x = 0; x != 4; ++x) for (int y = 0; y != 4; ++y)
        result.coefficient[x][y] = value.coefficient[x][y] * exactScale;
    return result;
}

inline std::array<std::array<Integer, 4>, 4> ToInteger(const Bivariate& polynomial)
{
    int commonExponent = 0;
    bool haveCoefficient = false;
    for (const auto& row : polynomial.coefficient) for (const Dyadic& coefficient : row)
    {
        if (!coefficient.IsZero())
        {
            commonExponent = haveCoefficient ? std::min(commonExponent, coefficient.exponent) : coefficient.exponent;
            haveCoefficient = true;
        }
    }
    std::array<std::array<Integer, 4>, 4> result{};
    if (!haveCoefficient) return result;
    for (int x = 0; x != 4; ++x) for (int y = 0; y != 4; ++y)
    {
        const Dyadic& coefficient = polynomial.coefficient[x][y];
        if (!coefficient.IsZero())
            result[x][y] = coefficient.numerator.ShiftedLeft(unsigned(coefficient.exponent - commonExponent));
    }
    return result;
}

inline IntegerPolynomial Resultant(const Bivariate& first, const Bivariate& second, bool eliminateY)
{
    auto a = ToInteger(first);
    auto b = ToInteger(second);
    auto coefficient = [&](const auto& values, int outer, int inner) -> const Integer&
    {
        return eliminateY ? values[outer][inner] : values[inner][outer];
    };
    auto innerDegree = [&](const auto& values)
    {
        for (int inner = 3; inner >= 0; --inner)
            for (int outer = 0; outer != 4; ++outer)
                if (!coefficient(values, outer, inner).IsZero()) return inner;
        return -1;
    };
    const int m = innerDegree(a);
    const int n = innerDegree(b);
    if (m < 0 || n < 0) return {};
    if (m == 0 && n == 0) return {};
    const int size = m + n;
    std::vector<std::vector<IntegerPolynomial>> matrix(size, std::vector<IntegerPolynomial>(size));
    auto outerPolynomial = [&](const auto& values, int inner)
    {
        IntegerPolynomial result(4);
        for (int outer = 0; outer != 4; ++outer) result[size_t(outer)] = coefficient(values, outer, inner);
        Trim(result);
        return result;
    };
    for (int row = 0; row != n; ++row)
        for (int inner = 0; inner <= m; ++inner)
            matrix[row][row + m - inner] = outerPolynomial(a, inner);
    for (int row = 0; row != m; ++row)
        for (int inner = 0; inner <= n; ++inner)
            matrix[n + row][row + n - inner] = outerPolynomial(b, inner);

    IntegerPolynomial determinant;
    std::vector<bool> used(size, false);
    std::function<void(int, int, IntegerPolynomial)> visit = [&](int row, int parity, IntegerPolynomial product)
    {
        if (row == size)
        {
            determinant = Add(determinant, parity ? Negate(std::move(product)) : product);
            return;
        }
        for (int column = 0; column != size; ++column)
        {
            if (used[column] || matrix[row][column].empty()) continue;
            int crossings = 0;
            for (int previous = column + 1; previous != size; ++previous) crossings += used[previous] ? 1 : 0;
            used[column] = true;
            visit(row + 1, parity ^ (crossings & 1), Multiply(product, matrix[row][column]));
            used[column] = false;
        }
    };
    visit(0, 0, IntegerPolynomial{ Integer(1) });
    RemovePowerOfTwoContent(determinant);
    return determinant;
}

inline double Evaluate(const Bivariate& polynomial, double x, double y)
{
    double result = 0.0;
    for (int i = 3; i >= 0; --i)
    {
        double row = 0.0;
        for (int j = 3; j >= 0; --j) row = row * y + polynomial.coefficient[i][j].ToDouble();
        result = result * x + row;
    }
    return result;
}

inline double Derivative(const Bivariate& polynomial, double x, double y, bool xDerivative)
{
    double result = 0.0;
    for (int i = 0; i != 4; ++i) for (int j = 0; j != 4; ++j)
    {
        if (xDerivative && i != 0)
            result += i * polynomial.coefficient[i][j].ToDouble() * std::pow(x, i - 1) * std::pow(y, j);
        if (!xDerivative && j != 0)
            result += j * polynomial.coefficient[i][j].ToDouble() * std::pow(x, i) * std::pow(y, j - 1);
    }
    return result;
}

inline double CoefficientScale(const Bivariate& polynomial)
{
    double scale = 1.0;
    for (const auto& row : polynomial.coefficient) for (const Dyadic& value : row)
        scale = std::max(scale, std::abs(value.ToDouble()));
    return scale;
}

struct System
{
    Bivariate first{};
    Bivariate second{};
};

inline System BuildSystem(const Triangle& triangle, const Surface& surface, const Texture& texture,
    const Ray& ray, Projection projection, int cellX, int cellY)
{
    const Bivariate uvX = AffineCorners(triangle.uv[0].x, triangle.uv[1].x, triangle.uv[2].x);
    const Bivariate uvY = AffineCorners(triangle.uv[0].y, triangle.uv[1].y, triangle.uv[2].y);
    const Bivariate fx = uvX * double(texture.width) - Constant(double(cellX) + 0.5);
    const Bivariate fy = uvY * double(texture.height) - Constant(double(cellY) + 0.5);
    const double h00 = texture.Fetch(cellX, cellY, surface.addressMode);
    const double h10 = texture.Fetch(cellX + 1, cellY, surface.addressMode);
    const double h01 = texture.Fetch(cellX, cellY + 1, surface.addressMode);
    const double h11 = texture.Fetch(cellX + 1, cellY + 1, surface.addressMode);
    const Bivariate height = Constant(h00) + fx * (Constant(h10) - Constant(h00)) +
        fy * (Constant(h01) - Constant(h00)) +
        (fx * fy) * (Constant(h11) - Constant(h10) - Constant(h01) + Constant(h00));
    std::array<Bivariate, 3> position;
    for (int axis = 0; axis != 3; ++axis)
    {
        const Bivariate base = AffineCorners(triangle.position[0][axis], triangle.position[1][axis], triangle.position[2][axis]);
        const Bivariate direction = AffineCorners(triangle.direction[0][axis], triangle.direction[1][axis], triangle.direction[2][axis]);
        position[axis] = base * surface.baseScale + (height - Constant(surface.reference)) * direction * surface.amplitude;
    }
    const int k = projection.major;
    const int i = projection.first;
    const int j = projection.second;
    return { (position[i] - Constant(ray.origin[i])) * ray.direction[k] -
                 (position[k] - Constant(ray.origin[k])) * ray.direction[i],
        (position[j] - Constant(ray.origin[j])) * ray.direction[k] -
                 (position[k] - Constant(ray.origin[k])) * ray.direction[j] };
}

enum class Status
{
    Complete,
    PositiveDimensional
};

struct Root
{
    double t = 0.0;
    uint primitiveId = std::numeric_limits<uint>::max();
    Vec3 barycentric{};
    Vec3 position{};
    Vec2 uv{};
    Vec3 normal{};
};

struct Result
{
    Status status = Status::Complete;
    std::vector<Root> roots;
};

inline Result IntersectAll(const std::vector<Triangle>& triangles, const Surface& surface,
    const Texture& texture, const Ray& ray)
{
    Result result;
    if (triangles.empty()) return result;
    for (const Triangle& triangle : triangles)
    {
        if (!Valid(triangle, surface, texture, ray))
        {
            result.status = Status::PositiveDimensional;
            return result;
        }
    }
    const Projection projection = MakeProjection(ray.direction);
    for (const Triangle& triangle : triangles)
    {
        double minimumX = std::numeric_limits<double>::infinity();
        double minimumY = minimumX;
        double maximumX = -minimumX;
        double maximumY = -minimumX;
        for (const Vec2 uv : triangle.uv)
        {
            const double x = uv.x * texture.width - 0.5;
            const double y = uv.y * texture.height - 0.5;
            minimumX = std::min(minimumX, x); maximumX = std::max(maximumX, x);
            minimumY = std::min(minimumY, y); maximumY = std::max(maximumY, y);
        }
        for (int cellY = int(std::floor(minimumY)); cellY <= int(std::floor(maximumY)); ++cellY)
        {
            for (int cellX = int(std::floor(minimumX)); cellX <= int(std::floor(maximumX)); ++cellX)
            {
                const System system = BuildSystem(triangle, surface, texture, ray, projection, cellX, cellY);
                const IntegerPolynomial resultantX = Resultant(system.first, system.second, true);
                const IntegerPolynomial resultantY = Resultant(system.first, system.second, false);
                if (resultantX.empty() || resultantY.empty())
                {
                    result.status = Status::PositiveDimensional;
                    continue;
                }
                const auto xRoots = IsolateUnitRoots(resultantX);
                const auto yRoots = IsolateUnitRoots(resultantY);
                const double residualScale = std::max(CoefficientScale(system.first), CoefficientScale(system.second));
                for (const RootInterval& xRoot : xRoots)
                {
                    for (const RootInterval& yRoot : yRoots)
                    {
                        double x = (xRoot.lower + xRoot.upper) * 0.5;
                        double y = (yRoot.lower + yRoot.upper) * 0.5;
                        for (int iteration = 0; iteration != 12; ++iteration)
                        {
                            const double f = Evaluate(system.first, x, y);
                            const double g = Evaluate(system.second, x, y);
                            const double fx = Derivative(system.first, x, y, true);
                            const double fy = Derivative(system.first, x, y, false);
                            const double gx = Derivative(system.second, x, y, true);
                            const double gy = Derivative(system.second, x, y, false);
                            const double determinant = fx * gy - fy * gx;
                            const double scale = std::max({ 1.0, std::abs(fx), std::abs(fy), std::abs(gx), std::abs(gy) });
                            if (std::abs(determinant) <= 128.0 * std::numeric_limits<double>::epsilon() * scale * scale) break;
                            const double dx = (-f * gy + fy * g) / determinant;
                            const double dy = (-fx * g + f * gx) / determinant;
                            x += dx; y += dy;
                            if (std::max(std::abs(dx), std::abs(dy)) <= 2e-15) break;
                        }
                        const double intervalSlop = 4e-11;
                        if (x < xRoot.lower - intervalSlop || x > xRoot.upper + intervalSlop ||
                            y < yRoot.lower - intervalSlop || y > yRoot.upper + intervalSlop ||
                            x < -intervalSlop || y < -intervalSlop || x + y > 1.0 + intervalSlop)
                            continue;
                        const double residual = std::max(std::abs(Evaluate(system.first, x, y)),
                            std::abs(Evaluate(system.second, x, y)));
                        if (residual > 2e-9 * residualScale) continue;
                        const Vec3 barycentric{ 1.0 - x - y, x, y };
                        const Vec2 uv = Interpolate(triangle.uv, barycentric);
                        const double texelX = uv.x * texture.width - 0.5;
                        const double texelY = uv.y * texture.height - 0.5;
                        if (texelX < cellX - 2e-9 || texelX > cellX + 1.0 + 2e-9 ||
                            texelY < cellY - 2e-9 || texelY > cellY + 1.0 + 2e-9) continue;
                        const Sample sample = SilPOM::Curved::Evaluate(triangle, surface, texture, barycentric);
                        if (!sample.validNormal) continue;
                        const double t = (sample.position[projection.major] - ray.origin[projection.major]) /
                            ray.direction[projection.major];
                        if (t < ray.tMin - 2e-9 || t > ray.tMax + 2e-9) continue;
                        Root root{ t, triangle.primitiveId, barycentric, sample.position, sample.uv, sample.normal };
                        bool duplicate = false;
                        for (Root& existing : result.roots)
                        {
                            if (std::abs(existing.t - root.t) <= 2e-8 && Length(existing.position - root.position) <= 2e-8)
                            {
                                duplicate = true;
                                if (root.primitiveId < existing.primitiveId) existing = root;
                                break;
                            }
                        }
                        if (!duplicate) result.roots.push_back(root);
                    }
                }
            }
        }
    }
    std::sort(result.roots.begin(), result.roots.end(), [](const Root& a, const Root& b)
        { return a.t < b.t || (a.t == b.t && a.primitiveId < b.primitiveId); });
    return result;
}
}
