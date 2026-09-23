// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensProtector: the field-angle correction for footage shot through the
// Osmo 360's transparent lens protectors (and through ND filters that mount
// the same way).
//
// What it models
// --------------
// A protector is a thin glass dome in front of the fisheye.  It bends every
// incoming ray a little, so a world ray at angle theta from the optical axis
// reaches the lens as if it came in at g(theta) > theta.  The native
// calibration still describes the lens itself, so the right projection is
//
//     image point = native_lens( g(theta_world) )
//
// That is the order DJI's own software applies it, determined for
// interoperability: each output direction's angle from the lens axis is
// warped through the protector curve BEFORE the Kannala-Brandt projection
// with the native calibration.  "Forward" below is that order;
// "Inverse" (native_lens(g^-1(theta))) exists only so a runtime check can
// score the alternative.
//
// The curve
// ---------
// g is OUR smooth fit, not DJI's table: an odd polynomial in theta (radians)
//
//     g(theta) = theta * (a0 + a1 theta^2 + a2 theta^4 + a3 theta^6 + a4 theta^8)
//
// least-squares fitted to the measured table over 1..98 degrees (past 98 the
// table is a constant +2 degree extrapolation, not a measurement).  Fit
// residual: max 0.0129 deg, rms 0.0052 deg - about 0.16 px on a 6K stream
// at the seam.  g' >= 1.0035 on [0, 110] deg, so g is invertible there.
//
// Size of the effect: +0.52 deg at 30 deg, +1.28 deg at 90 deg, +1.66 deg
// at 98 deg.  At the seam (~90 deg from both axes) the two lenses err in
// opposite directions, so an uncorrected protector clip misregisters by
// about 2.6 deg - roughly 40 px on a 6000-px equirect.
//
// How it is applied
// -----------------
// foldLensProtector() refits the Kannala-Brandt polynomial so that the lens
// model itself includes the correction (no kernel change): it finds f', k'
// such that f' thetaD'(theta) ~= f thetaD(g(theta)).  The usable FOV moves
// with it, because the image circle is fixed in PIXELS: the rim that the
// bare lens reached at 97.59 deg is reached at g^-1(97.59) = 96.1 deg of
// world angle behind a protector.
#pragma once

#include "osv/core/Result.h"
#include "osv/geom/KannalaBrandt5.h"
#include "osv/geom/LensRig.h"

#include <array>
#include <cstdint>
#include <string>

namespace osv::geom {

/// Which way the protector curve is composed with the lens model.
enum class ProtectorDirection : std::uint8_t {
    None = 0,     ///< Bare lens: the model is left alone.
    Forward = 1,  ///< native_lens(g(theta)) - DJI's order, the physical one.
    Inverse = 2   ///< native_lens(g^-1(theta)) - scored only, as the alternative.
};

/// Stable name of a direction ("none", "forward", "inverse").
[[nodiscard]] const char* protectorDirectionName(ProtectorDirection direction) noexcept;

/// The fitted curve's coefficients a0..a4 (radians, odd polynomial).
inline constexpr std::array<double, 5> kLensProtectorCoefficients = {
    1.02286564, -0.0232912688, 0.0163263985, -0.00487570422, 0.000609627157};

/// Upper end of the measured range the curve was fitted on (degrees).
inline constexpr double kLensProtectorFitMaxDeg = 98.0;

/// Largest absolute fit residual against the measured table (degrees).
inline constexpr double kLensProtectorFitResidualDeg = 0.0129;

/// Upper end of the range on which g is guaranteed monotonic (degrees).
inline constexpr double kLensProtectorMonotonicMaxDeg = 110.0;

/// g(theta): the angle a world ray at `thetaRad` reaches the lens at, behind
/// a protector.  Odd in theta.  Non-finite input returns NaN.
[[nodiscard]] double lensProtectorAngle(double thetaRad) noexcept;

/// g'(theta).
[[nodiscard]] double lensProtectorAngleDerivative(double thetaRad) noexcept;

/// g^-1(phi) by safeguarded Newton iteration.  InvalidArgument for a
/// non-finite phi or one outside g([0, kLensProtectorMonotonicMaxDeg]).
[[nodiscard]] Result<double> lensProtectorAngleInverse(double phiRad) noexcept;

/// What a fold did to one lens.
struct ProtectorFold {
    KannalaBrandt5 lens;            ///< The corrected lens (stream px).
    double thetaMaxRad = 0.0;       ///< New usable half FOV (world angle), == lens.thetaMaxRad.
    double maxResidualPx = 0.0;     ///< Worst |refit - exact composition| over [0, thetaMax], in px.
};

/// Fold the protector curve into `lens`.  None returns the lens unchanged
/// (residual 0).  InvalidArgument for an invalid lens; Internal when the
/// refit is not finite, not monotonic over the usable FOV, or misses the
/// exact composition by more than half a pixel.
[[nodiscard]] Result<ProtectorFold> foldLensProtector(const KannalaBrandt5& lens,
                                                      ProtectorDirection direction) noexcept;

/// What applyLensProtector() did to a rig.
struct ProtectorRigFold {
    double lensFovDeg = 0.0;      ///< New usable FOV (degrees) for BlendParams::lensFovDeg.
    double maxResidualPx = 0.0;   ///< Worst refit residual of the two lenses (px).
};

/// Fold the curve into both lenses of `rig` and update rig.lensFovDeg.  The
/// FOV the caller's BlendParams must use is returned (the kernel takes its
/// thetaMax from the blend parameters, so leaving the blend at the bare-lens
/// FOV would sample past the image circle).  On failure the rig is left
/// untouched.  `baseLensFovDeg` is the bare-lens usable FOV (195.18).
[[nodiscard]] Result<ProtectorRigFold> applyLensProtector(LensRig& rig, ProtectorDirection direction,
                                                          double baseLensFovDeg) noexcept;

}  // namespace osv::geom
