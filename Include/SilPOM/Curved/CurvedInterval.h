// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace SilPOM::Curved::Interval
{
// Every elementary operation rounds outward. Singleton inputs denote the exact
// binary64 authored values; tolerances never change whether zero is included.
struct Range
{
    double lo = 0.0, hi = 0.0;
    Range() = default;
    Range(double value) : lo(value), hi(value) {}
    Range(double lower, double upper) : lo(lower), hi(upper) {}
    double Midpoint() const { return lo * .5 + hi * .5; }
    double Width() const { return hi - lo; }
    double Magnitude() const { return std::max(std::abs(lo), std::abs(hi)); }
    bool Finite() const { return std::isfinite(lo) && std::isfinite(hi) && lo <= hi; }
};
inline double Down(double x) { return std::nextafter(x, -std::numeric_limits<double>::infinity()); }
inline double Up(double x) { return std::nextafter(x, std::numeric_limits<double>::infinity()); }
inline Range operator+(Range a, Range b) { return { Down(a.lo + b.lo), Up(a.hi + b.hi) }; }
inline Range operator-(Range a, Range b) { return { Down(a.lo - b.hi), Up(a.hi - b.lo) }; }
inline Range operator-(Range a) { return { -a.hi, -a.lo }; }
inline Range operator*(Range a, Range b)
{
    const double aa = a.lo * b.lo, ab = a.lo * b.hi, ba = a.hi * b.lo, bb = a.hi * b.hi;
    return { Down(std::min({ aa, ab, ba, bb })), Up(std::max({ aa, ab, ba, bb })) };
}
inline Range operator/(Range a, double b)
{
    return b > 0 ? Range{ Down(a.lo / b), Up(a.hi / b) } : Range{ Down(a.hi / b), Up(a.lo / b) };
}
inline bool Inside(Range a, Range b) { return a.lo >= b.lo && a.hi <= b.hi; }
inline bool StrictlyInside(Range a, Range b) { return a.lo > b.lo && a.hi < b.hi; }
inline bool Disjoint(Range a, Range b) { return a.lo > b.hi || a.hi < b.lo; }
inline Range Intersection(Range a, Range b) { return { std::max(a.lo, b.lo), std::min(a.hi, b.hi) }; }

// Automatic differentiation keeps value and derivative bounds on precisely the
// same fixed-cell polynomial, including the h * derivative(D) term.
struct Jet
{
    Range value{}, du{}, dv{};
    Jet() = default;
    Jet(double constant) : value(constant) {}
    Jet(Range v, Range u, Range w) : value(v), du(u), dv(w) {}
};
inline Jet operator+(Jet a, Jet b) { return { a.value + b.value, a.du + b.du, a.dv + b.dv }; }
inline Jet operator-(Jet a, Jet b) { return { a.value - b.value, a.du - b.du, a.dv - b.dv }; }
inline Jet operator*(Jet a, Jet b)
{
    return { a.value * b.value, a.du * b.value + a.value * b.du, a.dv * b.value + a.value * b.dv };
}
}
