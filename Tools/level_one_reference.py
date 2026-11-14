"""Independent FP64 bilinear-cell root checks for affine displacement directions.

Parameterize the ray/surface equation by displacement height, not a neural seed.
UV and ray distance become rational polynomials. Split at every texel/triangle
boundary and solve each cell's polynomial (degree <= 5). This does not call the
shader, its Newton solver, its bounds clipping, or the training teacher.

This is a numerical audit, not a certified interval-arithmetic oracle. Singular
parameterizations and ill-conditioned candidate roots are reported unresolved.
"""
import itertools
import numpy as np
from numpy.polynomial import polynomial as P


def _trim(poly):
    poly = np.asarray(poly, dtype=np.float64)
    size = np.max(np.abs(poly), initial=0)
    if not size:
        return np.zeros(1)
    while len(poly) > 1 and abs(poly[-1]) < size * 2e-13:
        poly = poly[:-1]
    return poly


def _roots(poly):
    poly = _trim(poly)
    if len(poly) <= 1:
        return []
    roots = P.polyroots(poly / np.max(np.abs(poly)))
    return [float(r.real) for r in roots if abs(r.imag) < 1e-7 and -1e-9 <= r.real <= 1 + 1e-9]


def _det(a, b, c):
    value = np.zeros(1)
    for order in itertools.permutations(range(3)):
        sign = -1 if sum(order[i] > order[j] for i in range(3) for j in range(i + 1, 3)) % 2 else 1
        term = P.polymul(P.polymul(a[order[0]], b[order[1]]), c[order[2]])
        value = P.polyadd(value, term * sign)
    return _trim(value)


def trace(height, packed_face, ray, scale, reference, address, t_min=0, t_max=150, max_cells=50000):
    face, ray = np.asarray(packed_face, dtype=np.float64), np.asarray(ray, dtype=np.float64)
    origin, du, dv = face[:3], face[3:6], face[6:9]
    uv, directions = face[9:15].reshape(3, 2), face[15:24].reshape(3, 3)
    width, rows = height.shape[1], height.shape[0]
    inv = np.linalg.inv((uv[1:] - uv[0]).T)
    d_u, d_v = np.linalg.solve(uv[1:] - uv[0], directions[1:] - directions[0])
    low, high = sorted((-scale * reference, scale * (1 - reference)))
    h = np.array([low, high - low])
    u = np.column_stack((du + d_u * low, d_u * (high - low)))
    v = np.column_stack((dv + d_v * low, d_v * (high - low)))
    w = -ray[3:, None]
    rhs = np.column_stack((ray[:3] - origin - directions[0] * low, -directions[0] * (high - low)))
    den = _det(u, v, w)
    if np.max(np.abs(den)) < 1e-14 or high == low:
        return {"status": "unresolved", "reason": "singular height parameterization", "roots": []}
    a, b, distance = _det(rhs, v, w), _det(u, rhs, w), _det(u, v, rhs)
    nx = P.polyadd(a * width, den * (uv[0, 0] * width - .5))
    ny = P.polyadd(b * rows, den * (uv[0, 1] * rows - .5))
    bary_b = P.polyadd(a * inv[0, 0], b * inv[0, 1])
    bary_c = P.polyadd(a * inv[1, 0], b * inv[1, 1])
    boundaries = [0., 1.] + _roots(den)
    for poly in (bary_b, bary_c, P.polysub(P.polysub(den, bary_b), bary_c),
                 P.polysub(distance, den * t_min), P.polysub(distance, den * t_max)):
        boundaries.extend(_roots(poly))
    tex_min, tex_max = np.floor(uv.min(axis=0) * [width, rows] - .5), np.ceil(uv.max(axis=0) * [width, rows] - .5)
    if np.sum(tex_max - tex_min) > max_cells:
        return {"status": "unresolved", "reason": "cell audit budget", "roots": []}
    for axis, numerator in enumerate((nx, ny)):
        for cell in range(int(tex_min[axis]), int(tex_max[axis]) + 1):
            boundaries.extend(_roots(P.polysub(numerator, den * cell)))
    boundaries = sorted(set(min(1., max(0., value)) for value in boundaries))
    den2 = P.polymul(den, den)

    def domain(s):
        divisor = P.polyval(s, den)
        if abs(divisor) < np.max(np.abs(den)) * 1e-12:
            return None
        q = np.array([P.polyval(s, a), P.polyval(s, b)]) / divisor
        bary = inv @ q
        t = P.polyval(s, distance) / divisor
        if min(*bary, 1 - bary.sum()) < -2e-8 or not t_min - 1e-8 <= t <= t_max + 1e-8:
            return None
        return uv[0] + q, t, q

    def tap(x, y):
        if address:
            x, y = min(max(x, 0), width - 1), min(max(y, 0), rows - 1)
        else:
            x, y = x % width, y % rows
        return float(height[y, x])

    candidates, uncertain, cells = [], 0, 0
    for left, right in zip(boundaries, boundaries[1:]):
        if right - left < 1e-12:
            continue
        middle = (left + right) / 2
        if domain(middle) is None:
            continue
        cells += 1
        x, y = (P.polyval(middle, n) / P.polyval(middle, den) for n in (nx, ny))
        ix, iy = int(np.floor(x)), int(np.floor(y))
        h00, h10, h01, h11 = tap(ix, iy), tap(ix + 1, iy), tap(ix, iy + 1), tap(ix + 1, iy + 1)
        fx, fy = P.polysub(nx, den * ix), P.polysub(ny, den * iy)
        field = den2 * (h00 - reference)
        field = P.polyadd(field, P.polymul(fx, den) * (h10 - h00))
        field = P.polyadd(field, P.polymul(fy, den) * (h01 - h00))
        field = P.polyadd(field, P.polymul(fx, fy) * (h11 - h10 - h01 + h00))
        equation = P.polysub(P.polymul(h, den2), field * scale)
        for s in [left, right] + [r for r in _roots(equation) if left - 1e-9 <= r <= right + 1e-9]:
            found = domain(s)
            if found is None:
                continue
            coords, t, q = found
            xx, yy = coords * [width, rows] - .5
            cx, cy = int(np.floor(xx)), int(np.floor(yy))
            dx, dy = xx - cx, yy - cy
            aa, bb, cc, dd = tap(cx, cy), tap(cx + 1, cy), tap(cx, cy + 1), tap(cx + 1, cy + 1)
            actual_h = scale * ((aa * (1 - dx) + bb * dx) * (1 - dy) + (cc * (1 - dx) + dd * dx) * dy - reference)
            expected_h = P.polyval(s, h)
            point = origin + du * q[0] + dv * q[1] + (directions[0] + d_u * q[0] + d_v * q[1]) * actual_h
            residual = float(np.linalg.norm(point - ray[:3] - ray[3:] * t))
            if abs(actual_h - expected_h) < 2e-9 and residual < 2e-8:
                candidates.append({"t": float(t), "uv": coords.tolist(), "residual_m": residual})
            elif left < s < right:
                uncertain += 1
    candidates.sort(key=lambda item: item["t"])
    distinct = []
    for item in candidates:
        if not distinct or abs(item["t"] - distinct[-1]["t"]) > 1e-7:
            distinct.append(item)
    return {"status": "unresolved" if uncertain else "checked", "roots": distinct,
            "nearest_t": distinct[0]["t"] if distinct else None, "cells": cells,
            "ill_conditioned_candidates": uncertain}
