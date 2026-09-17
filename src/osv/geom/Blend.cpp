// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Reference blend weights: FOV feather times occlusion feather.

#include "osv/geom/Blend.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace osv::geom {

namespace {

/// Distance from point p to the segment ab.
double distanceToSegment(const Vec2d& a, const Vec2d& b, const Vec2d& p) noexcept {
    const Vec2d ab = b - a;
    const double len2 = ab.dot(ab);
    // Degenerate segment: distance to the point.
    if (!(len2 > 0.0)) {
        return (p - a).norm();
    }
    const double t = clampd((p - a).dot(ab) / len2, 0.0, 1.0);
    const Vec2d closest = a + ab * t;
    return (p - closest).norm();
}

/// Hermite smoothstep on [0, 1].
double smoothstep01(double x) noexcept {
    const double t = clampd(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

double effectiveThetaMax(int lens, const BlendParams& params) noexcept {
    // The seam shift enlarges one lens's share and shrinks the other's.
    const double shift = std::isfinite(params.seamShiftDeg) ? params.seamShiftDeg : 0.0;
    const double half = 0.5 * params.lensFovDeg + ((lens == kMasterLens) ? shift : -shift);
    return deg2rad(half);
}

bool pointInPolygon(const std::vector<Vec2d>& polygon, const Vec2d& p) noexcept {
    const std::size_t n = polygon.size();
    if (n < 3) {
        return false;
    }
    // Even-odd rule: count edges crossed by a horizontal ray towards +x.
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2d& a = polygon[i];
        const Vec2d& b = polygon[j];
        const bool straddles = (a.y > p.y) != (b.y > p.y);
        if (!straddles) {
            continue;
        }
        const double xCross = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
        if (p.x < xCross) {
            inside = !inside;
        }
    }
    return inside;
}

double signedDistanceToPolygon(const std::vector<Vec2d>& polygon, const Vec2d& p) noexcept {
    const std::size_t n = polygon.size();
    if (n < 3 || !std::isfinite(p.x) || !std::isfinite(p.y)) {
        return std::numeric_limits<double>::infinity();
    }
    // Unsigned distance to the closest edge.
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double d = distanceToSegment(polygon[j], polygon[i], p);
        if (d < best) {
            best = d;
        }
    }
    // Sign from the even-odd containment test.
    return pointInPolygon(polygon, p) ? -best : best;
}

double lensWeightRef(const LensRig& rig, int lens, double theta, const Vec2d& px, const BlendParams& params) noexcept {
    if (!LensRig::validIndex(lens) || !std::isfinite(theta) || theta < 0.0) {
        return 0.0;
    }
    if (!std::isfinite(px.x) || !std::isfinite(px.y)) {
        return 0.0;
    }
    // Outside the frame: no data.
    if (px.x < 0.0 || px.y < 0.0 || px.x >= static_cast<double>(rig.streamW) ||
        px.y >= static_cast<double>(rig.streamH)) {
        return 0.0;
    }
    // FOV feather: 1 until thetaMax - feather, smoothstep to 0 at thetaMax.
    const double thetaMax = effectiveThetaMax(lens, params);
    if (!(thetaMax > 0.0) || theta > thetaMax) {
        return 0.0;
    }
    const double feather = deg2rad(std::isfinite(params.featherDeg) ? std::max(params.featherDeg, 0.0) : 0.0);
    double weight = 1.0;
    if (feather > 0.0) {
        const double start = thetaMax - feather;
        if (theta > start) {
            weight = 1.0 - smoothstep01((theta - start) / feather);
        }
    }

    // Occlusion feather: 0 inside the polygon, ramping to 1 at featherPx.
    if (params.useOcclusionMask) {
        const std::vector<Vec2d>& poly = rig.occlusionPolyStream[static_cast<std::size_t>(lens)];
        if (poly.size() >= 3) {
            const double sd = signedDistanceToPolygon(poly, px);
            const double ramp = (std::isfinite(params.occlusionFeatherPx) && params.occlusionFeatherPx > 0.0)
                                    ? params.occlusionFeatherPx
                                    : 0.0;
            double occ = 1.0;
            if (ramp > 0.0) {
                occ = clampd(sd / ramp, 0.0, 1.0);
            } else {
                occ = (sd > 0.0) ? 1.0 : 0.0;
            }
            weight *= occ;
        }
    }
    return clampd(weight, 0.0, 1.0);
}

}  // namespace osv::geom
