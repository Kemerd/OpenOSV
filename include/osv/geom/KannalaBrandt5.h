// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Five-term Kannala-Brandt fisheye model used by the Osmo 360 calibration:
//
//     theta_d = theta * (1 + k1*theta^2 + k2*theta^4 + k3*theta^6
//                          + k4*theta^8 + k5*theta^10)
//     px      = (cx + fx * theta_d * cos(phi), cy + fy * theta_d * sin(phi))
//
// where theta is the angle between the ray and the optical axis and phi the
// azimuth in the image plane.  The classic four-term OpenCV model is NOT what
// the camera writes: it becomes non-monotonic beyond ~88 degrees and produces
// a wrong image circle, so k5 is always carried (see docs/GEOMETRY.md).
//
// Lens frame convention (all lenses): +z is the optical axis, +x points to
// image right, +y points to image down.  Pixel coordinates are continuous
// (this class never adds a pixel-centre offset; callers add 0.5 themselves).
#pragma once

#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/meta/Types.h"

#include <array>
#include <cstddef>
#include <vector>

namespace osv::geom {

/// Kannala-Brandt fisheye with five radial terms.  All values in double and
/// in whatever pixel unit the intrinsics are expressed in (calibration or
/// stream pixels - see StreamScaling / LensRig).
struct KannalaBrandt5 {
    double fx = 0.0;                       ///< Focal length along x (px).
    double fy = 0.0;                       ///< Focal length along y (px).
    double cx = 0.0;                       ///< Principal point x (px).
    double cy = 0.0;                       ///< Principal point y (px).
    std::array<double, 5> k{};             ///< Radial terms k1..k5.
    double thetaMaxRad = deg2rad(97.59);   ///< Usable half FOV (195.18 deg lens).
    double rMaxPx = 0.0;                   ///< Image-circle radius at thetaMax (px, mean focal); 0 = not computed.

    /// Default Newton tolerance on theta (radians).
    static constexpr double kNewtonTolerance = 1e-12;
    /// Maximum Newton iterations before giving up.
    static constexpr int kNewtonMaxIterations = 20;

    /// Build from a calibration record: fx, fy, cx, cy, k1..k5 taken as-is
    /// (calibration pixel units, 3840 x 3840 on the Osmo 360).  thetaMaxRad
    /// keeps its default; rMaxPx is computed.
    [[nodiscard]] static KannalaBrandt5 fromDewarp(const meta::DewarpParams& params) noexcept;

    /// Distorted angle theta_d(theta) - the radial polynomial.
    [[nodiscard]] double thetaD(double theta) const noexcept;

    /// Derivative d(theta_d)/d(theta); <= 0 means the model folds back.
    [[nodiscard]] double dThetaD(double theta) const noexcept;

    /// Invert theta_d -> theta by Newton iteration starting at theta0 = rd.
    /// Fails when the derivative is not positive along the way (non-monotonic
    /// region) or when the iteration does not converge.  `iterations`
    /// (optional) receives the number of Newton steps taken.
    [[nodiscard]] Result<double> thetaFromThetaD(double rd, int* iterations = nullptr) const noexcept;

    /// Project a direction given in the lens frame (+z optical axis) to a
    /// pixel.  Returns false (and leaves `px` untouched) when the ray lies
    /// beyond thetaMaxRad, when the direction is degenerate or when the
    /// result is not finite.  `theta` receives the ray angle whenever the
    /// direction itself is valid so callers can feed it to the blend weights.
    [[nodiscard]] bool project(const Vec3d& dLens, Vec2d& px, double& theta) const noexcept;

    /// Inverse of project(): pixel -> unit direction in the lens frame.  The
    /// angle is NOT clipped at thetaMaxRad (callers decide what to do with
    /// rays past the usable FOV) but the inversion fails in a non-monotonic
    /// region.  `iterations` (optional) receives the Newton step count.
    [[nodiscard]] Result<Vec3d> unproject(const Vec2d& px, int* iterations = nullptr) const noexcept;

    /// True when d(theta_d)/d(theta) > 0 at `samples` evenly spaced angles in
    /// [0, thetaMax] (i.e. the model can be inverted on that range).
    [[nodiscard]] bool isMonotonic(double thetaMax, std::size_t samples = 4096) const noexcept;

    /// Lookup table theta(rd) for the GPU path: entry i corresponds to
    /// rd = i / (n - 1) * seedTableRdMax().  Entries that cannot be inverted
    /// repeat the previous good value so the table stays monotonic.  An
    /// empty vector is returned for n < 2 or an invalid model.
    [[nodiscard]] std::vector<float> seedTable(std::size_t n = 4096) const;

    /// Upper end of the seedTable() domain: theta_d(thetaMaxRad).
    [[nodiscard]] double seedTableRdMax() const noexcept;

    /// Recompute rMaxPx from the current focal lengths and thetaMaxRad.
    void updateRMax() noexcept;

    /// True when fx, fy > 0 and every field is finite.
    [[nodiscard]] bool isValid() const noexcept;
};

}  // namespace osv::geom
