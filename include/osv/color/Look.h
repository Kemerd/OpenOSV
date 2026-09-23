// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Display looks for the Rec.709 output: the "DJI Studio" look (the default)
// and the standard rendering it replaced.
//
// Why a look exists at all
// ------------------------
// DJI Studio renders an Osmo 360 D-Log M clip through its bundled
// "DJI Osmo 360 D-Log M to Rec.709 V1.cube": the clip's automatic "D-LOG M"
// filter (slug LOG_Osmo360_DLogM, service mika.lut2, strength 1.0, recorded in
// the user's DJI Studio project files).  That file is also byte-identical to
// DJI's Pocket 3 D-Log M LUT; its header dates it to the Mavic 3 Pro.  It is a
// *look*, not a colour-space conversion:
//
//   * a crushed toe (code 0.0625 -> 0.0115 where a straight conversion gives
//     0.058) and an S-shaped mid-tone section: grey is pinned (code 0.40625 ->
//     0.388) but code 0.50 -> 0.487 and code 0.875 -> 0.904 (our standard
//     rendering: 0.506 and 0.892);
//   * a real highlight shoulder: its slope falls to 0.54 at code 1.0 where the
//     HLG-in-709 standard rendering still has 0.83, so bright sky next to the
//     sun is compressed harder;
//   * more saturation through the mid-tones and the sky (a D-Log M sky code
//     (0.5, 0.5, 0.75) renders with blue 0.888 against 0.808 standard);
//   * shadow colours far more saturated than their mid-tone versions - a
//     per-channel curve after the primaries matrix, not a ratio-preserving
//     tone map;
//   * orange-stays-orange highlights and gamut compression instead of clipping
//     for colours outside Rec.709.
//
// Measured over DJI's whole 33^3 input cube, the standard rendering is 2.81
// dE2000 mean / 6.57 p95 / 15.8 max away from DJI's; on the sample clip's real
// pixel distribution 2.15 mean / 4.86 p95.  The fitted look below is 1.23 /
// 2.80 / 6.1 over the cube and 0.50 / 1.18 on the sample clip, with the
// neutral axis within 0.42 dE2000 (0.006 of signal) everywhere.  A dE2000 of
// about 1 is the usual threshold of a just-noticeable difference side by side.
//
// What is shipped is OUR model, not DJI's data
// --------------------------------------------
// kLookDjiRec709 holds 17 tone knots, two 3x3 matrices and eleven scalars
// fitted by scripts/fit_look.py (least squares in CIELAB over all 35937 entries
// of DJI's file plus the sample clip's pixels).  No LUT entry is copied or
// redistributed (see NOTICE); the .cube files in luts/ are generated from this
// model by our own writer.
//
// Stage by stage (the kernel is osvLookApply in ColorMath.h), with the
// measurement that justifies each one:
//
//   1. toLook matrix, then a per-channel tone curve T through a D-Log M shaper.
//      A D-Log M orange three stops under grey renders in DJI's file with a
//      green/red ratio of 0.27 against 0.44 at three stops over: a ratio-
//      preserving tone map would hold it constant, a per-channel curve after a
//      matrix reproduces it.  T is indexed by the D-Log M code of the light
//      (the shaper is kDlogMOsmo360 inverted), so on the neutral axis T IS
//      DJI's grey scale, knot for knot.
//   2. Highlight hue preservation.  With a clipped red channel DJI keeps
//      orange orange ((1.0, 0.72, 0.5) -> green 0.595); per-channel shouldering
//      alone slides it to yellow (0.708).
//   3. Signal-space matrix: the extra mid-tone and sky saturation.
//   4. Soft gamut compression: (1.0, 0.4, 0.4) renders with 0.19 of green in
//      DJI's file where matrix-and-clamp gives 0.
//
// Additive-model tests showed the reference is not separable in any single
// domain (code, linear, display signal), which is why every two-stage
// curve-plus-matrix model plateaued near 2 dE2000; each stage above bought a
// measured improvement on the same data.
//
// HDR outputs
// -----------
// DJI Studio has no colour-managed HDR rendering for the Osmo 360 to match.
// Its only D-Log M HDR asset is an optional creative style
// (FT_StyleGeneralDlogm2HLG: an 8-bit 64^3 PNG LUT, default strength 0.8,
// blacks lifted to 3/255) - the same source the kDjiHlgTable measurements in
// the tests and the kDlogMDjiRefit curve came from.  HLG and PQ therefore keep
// the standard rendering; the look only applies to the Rec.709 output.
#pragma once

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"

namespace osv::color {

/// True when `look` changes the rendering of `transfer` (DjiStudio on Rec.709).
[[nodiscard]] bool lookApplies(Look look, OutputTransfer transfer) noexcept;

/// The fitted constants of a look, in the fit's own terms (before any of the
/// host-side derivation makeLookParams does for the kernel).
struct LookFit {
    OsvMat3f nativeToLook;                 ///< Osmo 360 native scene-linear -> look RGB (rows sum to 1).
    float tone[OSV_LOOK_MAX_KNOTS];        ///< T at code k / 16; T(0) = 0, T(1) = 1, strictly increasing.
    float hueStart;                        ///< Tone of max(x) where hue preservation starts.
    float hueWidth;                        ///< Tone span of the smoothstep ramp.
    float hueAmount;                       ///< Blend weight at full ramp.
    float hueExponent;                     ///< Exponent on the channel ratio.
    OsvMat3f display;                      ///< Signal-space matrix (rows sum to 1).
    float gamutThreshold[3];               ///< Untouched distance below max, per channel.
    float gamutLimit[3];                   ///< Distance that compresses exactly onto 1.
    float gamutPower;                      ///< Compression curve power.
};

/// The DJI Studio Rec.709 look for Osmo 360 D-Log M.
///
/// Provenance: `python scripts/fit_look.py --samples <4 equirect D-Log M
/// frames of the sample clip>` against DJI Studio 1.0.0.24724's
/// "DJI Osmo 360 D-Log M to Rec.709 V1.cube" (MD5 1993aa72...7c86).  Fit
/// report: whole cube 1.231 dE2000 mean / 2.798 p95 / 6.117 max (standard
/// rendering 2.813 / 6.568 / 15.77); sample clip 0.503 / 1.177 (2.151 /
/// 4.858); neutral axis 0.134 mean / 0.417 max.  Neutral axis strictly
/// increasing; the worst luminance drop along 400 random exposure ramps is
/// 8.8e-6 of display light (below one 16-bit code).
///
/// The matrix is in the fit's native terms; makeLookParams composes it with
/// the inverse of kNativeToRec2020_Osmo360 so the kernel sees Rec.2020
/// working light.  The composition is exact for the Osmo 360 camera fit and a
/// consistent Rec.2020-referred look for the others.
inline constexpr LookFit kLookDjiRec709 = {
    {{
        1.305243161e+00f, -3.067455967e-01f, 1.502435588e-03f,
        -4.783984831e-02f, 1.106111295e+00f, -5.827144624e-02f,
        -2.956935995e-02f, -2.118211932e-01f, 1.241390553e+00f,
    }},
    {
        0.000000000e+00f, 1.000000000e-03f, 5.577001003e-02f, 1.199362692e-01f,
        1.947440343e-01f, 2.698302334e-01f, 3.472114270e-01f, 4.206987747e-01f,
        4.870118839e-01f, 5.542483955e-01f, 6.304355793e-01f, 7.091414992e-01f,
        7.804295909e-01f, 8.442920793e-01f, 9.026825179e-01f, 9.588373808e-01f,
        1.000000000e+00f,
    },
    4.789921576e-01f,  // hueStart
    1.762934664e-01f,  // hueWidth
    3.348795292e-01f,  // hueAmount
    4.509397421e-01f,  // hueExponent
    {{
        1.053558156e+00f, -8.111585751e-02f, 2.755770183e-02f,
        -5.272273560e-02f, 1.032485695e+00f, 2.023704091e-02f,
        -7.701505059e-02f, -7.395891813e-02f, 1.150973969e+00f,
    }},
    {9.868637841e-01f, 5.708230628e-01f, 4.076825014e-01f},  // gamutThreshold
    {4.000000000e+00f, 1.140381260e+00f, 1.260506564e+00f},  // gamutLimit
    1.000000210e+00f,                                          // gamutPower
};

/// Fritsch-Carlson monotone tangents (dT/du) for `count` knots on a uniform
/// grid over [0, 1].  `slopes` must hold OSV_LOOK_MAX_KNOTS floats; entries
/// from `count` on are zeroed.  Writes zeros and returns false for a count
/// outside 2..OSV_LOOK_MAX_KNOTS or a null `tone`; returns false without
/// writing for a null `slopes`.  Exposed so the tests can check the kernel
/// block independently.
bool monotoneTangents(const float* tone, int count, float* slopes) noexcept;

/// The kernel block for `look` on `transfer`.  A look that does not apply to
/// the transfer (or Look::Standard) returns a zeroed block, which the kernel
/// treats as "no look".
[[nodiscard]] OsvLookParams makeLookParams(Look look, OutputTransfer transfer) noexcept;

/// Replace the look of an already built parameter block, for its own transfer.
void setLook(OsvColorParams& params, Look look) noexcept;

/// The look a parameter block carries (Standard for a zeroed / foreign block).
[[nodiscard]] Look lookOf(const OsvColorParams& params) noexcept;

/// True when the block is either "no look" or a complete, finite, monotone,
/// in-range look (knot count, unit row sums, positive width / scales / power).
[[nodiscard]] bool lookParamsValid(const OsvLookParams& look) noexcept;

}  // namespace osv::color
