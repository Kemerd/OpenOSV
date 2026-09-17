// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Equirectangular pixel <-> direction mapping in two layouts.
//
//   Standard  : the usual 360 x 180 panorama.  Longitude runs along x
//               (image centre = +Y forward = master lens), latitude along y
//               (top = +Z up).  Direction = (sin lon cos lat, cos lon cos lat, sin lat).
//   PolarAxis : the two lens axes (+/-Y) sit at the poles so the stitching
//               seam becomes the equator row h / 2.  Used internally by the
//               seam search.  Direction = (cos lat sin lon, sin lat, cos lat cos lon).
//
// Pixel coordinates are continuous: px = 0 is the left edge, px = w the
// right edge.  Add 0.5 to address a pixel centre.
#pragma once

#include "osv/core/Math.h"

namespace osv::geom {

/// Which axis arrangement the panorama uses.
enum class EquirectLayout {
    Standard,   ///< Up = +Z at the top row, forward = +Y at the centre column.
    PolarAxis   ///< Poles = +/-Y lens axes, seam = equator row.
};

/// Stable name for logs / JSON.
[[nodiscard]] const char* equirectLayoutName(EquirectLayout layout) noexcept;

/// Equirectangular map of a given pixel size.
struct EquirectMap {
    EquirectLayout layout = EquirectLayout::Standard;
    int w = 0;  ///< Width in pixels (360 degrees of longitude).
    int h = 0;  ///< Height in pixels (180 degrees of latitude).

    /// Continuous pixel -> unit direction in the body frame.  False for a
    /// zero-sized map or non-finite input.
    [[nodiscard]] bool pixelToDir(double px, double py, Vec3d& dir) const noexcept;

    /// Unit direction -> continuous pixel.  False for a zero-sized map or a
    /// zero / non-finite direction.  Longitude is wrapped into [0, w).
    [[nodiscard]] bool dirToPixel(const Vec3d& dir, Vec2d& px) const noexcept;

    /// True when w and h are positive.
    [[nodiscard]] constexpr bool isValid() const noexcept { return w > 0 && h > 0; }
};

}  // namespace osv::geom
