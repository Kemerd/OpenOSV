// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// D-Log M curve constants and host-side helpers.
//
// Three curves are shipped:
//   * kDlogMPocket3  - the public DJI Pocket 3 D-Log M -> linear constants.
//   * kDlogMDjiRefit - the same seven-parameter form re-fitted by
//                      scripts/fit_dlogm.py to 64 neutral-axis measurements
//                      of a DJI D-Log M -> HLG conversion (Pocket-3 era).
//   * kDlogMOsmo360  - the same form fitted to the neutral axis of DJI's own
//                      Osmo 360 D-Log M -> Rec.709 LUT (the default; see the
//                      comment on the constant for why it replaced the refit).
// All three use the branch-intersection cut so the curve is C0 continuous and
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

/// DJI-matched refit against Pocket-3-era D-Log M -> HLG measurements.
///
/// Superseded as the default by kDlogMOsmo360 once a genuine Osmo 360
/// reference became available (this curve was fitted before that and is up to
/// 0.30 stops off through the upper mids against it).  Kept verbatim, and
/// selectable as `--fit refit`, so a project graded against it keeps rendering
/// the same way.
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

/// Osmo 360 fit -- the project default.
///
/// Provenance: produced by
///     python scripts/fit_dlogm.py --from-cube DJI_Osmo360_DLogM_to_Rec709.cube
///                                 --cube-transfer 709
/// which samples the neutral (R == G == B) diagonal of DJI's own Osmo 360
/// D-Log M -> Rec.709 LUT at all 33 of its grid points and least-squares fits
/// this seven-parameter form to it.  Only these constants are shipped; no LUT
/// data from that file is redistributed (see NOTICE).
///
/// Why a Rec.709 reference can be fitted in HLG-signal space: OpenOSV's
/// Rec.709 output *is* the HLG signal computed in Rec.709 primaries (BT.2390
/// "HLG on an SDR display", docs/COLOR.md).  On the neutral axis both 3x3
/// primaries matrices are the identity (their rows sum to 1), so the whole
/// Rec.709 branch of osvLinearToOutput collapses to
///     out(code) = hlgOetf(sceneScale * lin(code))
/// which is exactly the HLG branch.  Inverting DJI's diagonal through that
/// expression recovers a smooth, strictly monotonic scene-linear curve
/// (3.74 at code 1.0, 18 % grey at code 0.406), confirming that DJI's 709
/// rendering is an HLG-in-709 rendering and not a separate tone map -- had it
/// been one, the recovered curve would show a roll-off kink near diffuse
/// white.  So the curve is what differs from DJI, not our output rendering,
/// and the curve is what was refitted.
///
/// Why this is the default rather than kDlogMDjiRefit: the refit was fitted to
/// Pocket-3-era D-Log M -> HLG measurements.  Against a genuine Osmo 360
/// reference it is up to 0.30 stops off through the upper mids and 0.66 stops
/// too bright in the toe.  Neutral-axis error against DJI's Osmo 360 LUT
/// (HLG code units, all 33 points): kDlogMDjiRefit 0.0318 RMS / 0.0659 worst;
/// this curve 0.0209 RMS / 0.0462 worst.  For code >= 0.24 (above the crushed
/// part of the reference) 0.0259 -> 0.0160 RMS.  kDlogMDjiRefit is kept
/// verbatim so existing projects can pin the old rendering with --fit refit.
///
/// Residual 0.0160 RMS is the honest ceiling of this seven-parameter family
/// for this data, not a solver failure: relaxing the slope-ratio bound from 3
/// to unbounded (ratio 40.8) moves the RMS above code 0.24 by 0.00002 and the
/// worst residual by 0.0004, so the bound costs nothing here and is kept for
/// the shadow-gradient and .cube-interpolation reasons documented in
/// scripts/fit_dlogm.py.
///
/// Anchors: code 0.400 -> lin 0.18000 -> HLG 0.3800 (pinned exactly, BT.2408
/// 18 % grey); code 0.714 -> lin 0.95775 -> HLG 0.7433.  The second anchor is
/// 0.0067 below BT.2408's nominal 75 % diffuse white, but DJI's own file reads
/// 0.7404 at its nearest grid point (code 0.71875), so this curve is *closer*
/// to the camera manufacturer's placement than kDlogMDjiRefit's 0.7548.
/// Code 1.000 -> lin 3.7647 -> HLG 1.0012, which the output clamp flattens to
/// 1.0; only codes above 0.9986 are affected (DJI's own LUT likewise reaches
/// exactly 1.0 at code 1.0).  Strictly monotonic over 4097 samples; the branch
/// cut is reached at code 0.125, i.e. the derivative jump sits far below the
/// usable shadows instead of at code 0.28 as in kDlogMDjiRefit.
inline constexpr OsvDlogMCurve kDlogMOsmo360 = {
    -2.360862594f,     // xShift
    0.630835854f,      // yShift
    6.691455736f,      // scale
    1.011886004f,      // slope
    3.035658045f,      // slope2
    0.822056039f,      // intercept
    0.00786506109f,    // midGrayScaling (pins code 0.40 -> 0.18)
    0.406199919f,      // cut (== intercept / (slope2 - slope))
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
