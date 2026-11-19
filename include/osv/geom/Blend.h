// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Blend weights: how much each lens contributes to a ray.  This is the host
// reference implementation the render kernels are checked against.
//
// weight = fovFeather(theta) * occlusionFeather(px)
//   fovFeather      : 1 up to (thetaMax - feather), then a smoothstep down to
//                     0 at thetaMax, 0 beyond.
//   occlusionFeather: 0 inside the occlusion polygon (selfie stick / grip),
//                     rising linearly to 1 at occlusionFeatherPx outside it.
// The weight is 0 for pixels outside the stream frame.
#pragma once

#include "osv/core/Math.h"
#include "osv/geom/LensRig.h"

#include <vector>

namespace osv::geom {

/// Blend parameters.
struct BlendParams {
    double lensFovDeg = 195.18;      ///< Usable lens FOV; thetaMax = half of it.
    double featherDeg = 4.0;         ///< Width of the FOV feather below thetaMax (deg).
    /// Width of the ramp outside the occlusion polygon, in pixels of a
    /// FULL-SIZE stream (6K or 8K).  A smaller stream of the same camera (the
    /// LRF proxy) takes fewer pixels for the same angle: every consumer reads
    /// the width through occlusionFeatherStreamPx(), never this field.
    double occlusionFeatherPx = 24.0;
    double seamShiftDeg = 0.0;       ///< Moves the seam: master (+) / slave (-) gains this many degrees.
    bool useOcclusionMask = true;    ///< Apply the calibration occlusion polygon.
};

/// The occlusion feather of `params` in pixels of `rig`'s own stream.
///
/// The feather is an ANGLE on the lens: it has to clear the mount's edge and
/// end before the seam plane, and both are fixed on the sphere, not on the
/// pixel grid.  `occlusionFeatherPx` was tuned on the 6K stream (24 px there
/// is 1.67 deg), so a stream with fewer pixels per sensor pixel gets the
/// width scaled down by `scaling.scale / kVerifiedCropScale6K`.  Without
/// that, 24 px on the 1024 px LRF half of a clip is 5 deg: the two lenses'
/// ramps then reach across the seam plane into each other, their weights sum
/// to ~0.7 along the mount's meridian, and the coverage alpha shows the host's
/// black through the stitch as a dark stripe.
///
/// Full-size streams keep the width exactly as given (the 8K stream is not
/// widened to 6K's angle): what shipped for them is unchanged, bit for bit.
/// Returns 0 (a hard mask) for a non-finite or non-positive width, and the
/// width as given when the rig's scale is unusable.
[[nodiscard]] double occlusionFeatherStreamPx(const LensRig& rig, const BlendParams& params) noexcept;

/// Effective half FOV of lens `lens` (radians) after the seam shift.
[[nodiscard]] double effectiveThetaMax(int lens, const BlendParams& params) noexcept;

/// Signed distance from `p` to the polygon boundary: negative inside,
/// positive outside (even-odd rule).  Returns +infinity for fewer than three
/// vertices (nothing is occluded).
[[nodiscard]] double signedDistanceToPolygon(const std::vector<Vec2d>& polygon, const Vec2d& p) noexcept;

/// True when `p` is inside the polygon (even-odd rule).
[[nodiscard]] bool pointInPolygon(const std::vector<Vec2d>& polygon, const Vec2d& p) noexcept;

/// Reference blend weight in [0, 1] for a ray that hits lens `lens` at angle
/// `theta` (radians from the optical axis) and pixel `px` (stream px).
[[nodiscard]] double lensWeightRef(const LensRig& rig, int lens, double theta, const Vec2d& px,
                                   const BlendParams& params) noexcept;

}  // namespace osv::geom
