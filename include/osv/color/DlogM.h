// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// D-Log M curve constants and host-side helpers.
//
// Two curves are shipped:
//   * kDlogMPocket3  - the public DJI Pocket 3 D-Log M -> linear constants.
//   * kDlogMDjiRefit - the same seven-parameter form re-fitted by
//                      scripts/fit_dlogm.py to 64 neutral-axis measurements
//                      of DJI's own D-Log M -> HLG conversion for the Osmo 360.
// Both use the branch-intersection cut so the curve is C0 continuous and
// have a closed-form inverse (linearToDlogm).
#pragma once

#include "osv/color/ColorMath.h"

namespace osv::color {

/// Public DJI Pocket 3 D-Log M constants.  The cut is defined as the branch
/// intersection intercept / (slope2 - slope) = 0.6034245, which is exactly
/// the published cut value, so the curve is continuous.
/// Anchors: code 0.40 -> 0.1800, code 1.00 -> 2.4735.
inline constexpr OsvDlogMCurve kDlogMPocket3 = {
    -2.428226947784424f,   // xShift
    0.9327186346054077f,   // yShift
    5.612990379333496f,    // scale
    1.0151796340942383f,   // slope
    1.8734303712844849f,   // slope2
    0.5178895592689514f,   // intercept
    0.18f / 12.4054f,      // midGrayScaling (0.014509...)
    0.6034245491027832f,   // cut (informational; intersection rule is used)
    OSV_DLOGM_CUT_INTERSECTION,
};

/// DJI-matched refit for the Osmo 360.
///
/// Provenance: produced by `python scripts/fit_dlogm.py` (numpy LM solver)
/// from the 64 DJI-matched neutral-axis points (D-Log M code i/63 -> HLG
/// signal) mapped through lin * 0.2674 -> HLG OETF, least squares in HLG-code
/// space, toe below code 0.24 weighted 0.15 (first sample 0.05) because that
/// part of the reference is crushed 8-bit data, pinned so that
/// lin(0.400) == 0.18 exactly (HLG 0.380), cut = intercept / (slope2 - slope)
/// (C0 continuous), lin(0) >= 0 and slope2 / slope <= 3 (the derivative jump
/// at the cut; unconstrained the solver reaches 8.8, which imitates DJI's
/// crushed toe with a hinge at code 0.23 and triples the trilinear error of
/// the .cube LUTs without improving the fit above code 0.30).
/// Fit report: RMS residual 0.0057 HLG for code >= 0.24 (max 0.0179 at
/// code 0.254), 0.0241 over all 64 points (the toe is deliberately not
/// crushed); code 0.714 -> HLG 0.7548, code 1.000 -> lin 3.429 -> HLG 0.9841;
/// cut reached at code 0.279; strictly monotonic over 4097 samples.
inline constexpr OsvDlogMCurve kDlogMDjiRefit = {
    -2.722814610f,     // xShift
    0.244082124f,      // yShift
    5.598943717f,      // scale
    1.010442692f,      // slope
    3.031328816f,      // slope2
    1.554539968f,      // intercept
    0.02068748227f,    // midGrayScaling (pins code 0.40 -> 0.18)
    0.769236796f,      // cut (== intercept / (slope2 - slope))
    OSV_DLOGM_CUT_INTERSECTION,
};

/// Effective branch cut of a curve (see osvDlogmCut).
[[nodiscard]] float dlogmCut(const OsvDlogMCurve& curve) noexcept;

/// Effective branch cut evaluated in double precision.
[[nodiscard]] double dlogmCutD(const OsvDlogMCurve& curve) noexcept;

/// D-Log M code -> scene-linear (float, identical to the kernel math).
[[nodiscard]] float dlogmToLinear(const OsvDlogMCurve& curve, float code) noexcept;

/// D-Log M code -> scene-linear evaluated in double precision (test reference).
[[nodiscard]] double dlogmToLinearD(const OsvDlogMCurve& curve, double code) noexcept;

/// Host inverse: scene-linear -> D-Log M code.
///
/// Uses the closed-form inverse of the piecewise curve (both branches are
/// invertible: log2 of the exponential part) computed in double precision and
/// verified/polished by a bisection fallback when the closed form is not
/// applicable (e.g. tmp - xShift <= 0 for a mis-configured curve).  Inputs
/// below lin(0) return 0 and NaN inputs return 0.
[[nodiscard]] float linearToDlogm(const OsvDlogMCurve& curve, float lin) noexcept;

/// Double precision version of linearToDlogm.
[[nodiscard]] double linearToDlogmD(const OsvDlogMCurve& curve, double lin) noexcept;

/// True when every parameter is finite, slope/slope2/midGrayScaling are
/// positive and scale is positive (i.e. the curve is monotonic increasing).
[[nodiscard]] bool dlogmCurveValid(const OsvDlogMCurve& curve) noexcept;

}  // namespace osv::color
