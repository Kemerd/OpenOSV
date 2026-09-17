// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Equirectangular pixel <-> direction mapping.

#include "osv/geom/EquirectMap.h"

#include <cmath>

namespace osv::geom {

const char* equirectLayoutName(EquirectLayout layout) noexcept {
    switch (layout) {
    case EquirectLayout::Standard: return "Standard";
    case EquirectLayout::PolarAxis: return "PolarAxis";
    }
    return "Unknown";
}

bool EquirectMap::pixelToDir(double px, double py, Vec3d& dir) const noexcept {
    if (!isValid() || !std::isfinite(px) || !std::isfinite(py)) {
        return false;
    }
    // Longitude spans [-pi, pi) across the width, latitude [pi/2, -pi/2]
    // from the top row to the bottom row.
    const double lon = (px / static_cast<double>(w) - 0.5) * kTwoPi;
    const double lat = (0.5 - py / static_cast<double>(h)) * kPi;
    const double cosLat = std::cos(lat);
    const double sinLat = std::sin(lat);
    const double sinLon = std::sin(lon);
    const double cosLon = std::cos(lon);
    if (layout == EquirectLayout::PolarAxis) {
        // Poles on +/-Y (the lens axes), lon measured around Y from +Z.
        dir = Vec3d{cosLat * sinLon, sinLat, cosLat * cosLon};
    } else {
        // Standard: up = +Z, centre column = +Y forward.
        dir = Vec3d{sinLon * cosLat, cosLon * cosLat, sinLat};
    }
    return dir.isFinite();
}

bool EquirectMap::dirToPixel(const Vec3d& dirIn, Vec2d& px) const noexcept {
    if (!isValid() || !dirIn.isFinite()) {
        return false;
    }
    // Work on a unit vector so asin() stays in range.
    const Vec3d d = dirIn.normalized();
    if (!(d.norm() > 0.0)) {
        return false;
    }
    double lon = 0.0;
    double lat = 0.0;
    if (layout == EquirectLayout::PolarAxis) {
        lat = std::asin(clampd(d.y, -1.0, 1.0));
        lon = std::atan2(d.x, d.z);
    } else {
        lat = std::asin(clampd(d.z, -1.0, 1.0));
        lon = std::atan2(d.x, d.y);
    }
    // Wrap the longitude into [0, w) and map latitude top-down.
    double x = (lon / kTwoPi + 0.5) * static_cast<double>(w);
    const double wd = static_cast<double>(w);
    x = std::fmod(x, wd);
    if (x < 0.0) {
        x += wd;
    }
    if (x >= wd) {
        x -= wd;
    }
    const double y = (0.5 - lat / kPi) * static_cast<double>(h);
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    px = Vec2d{x, y};
    return true;
}

}  // namespace osv::geom
