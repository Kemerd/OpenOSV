// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// C++ wrappers around the device-safe transfer functions in ColorMath.h plus
// double-precision reference implementations (namespace osv::color::ref) that
// the tests and scripts/colour_reference.py cross-check against.
//
// Anchors (BT.2100 / BT.2408):
//   hlgOetf(1/12) = 0.5, hlgOetf(1) = 1, hlgInverseOetf(0.75) = 0.26496
//   pqInverseEotf(203 nit) = 0.5807, (100) = 0.5081, (1000) = 0.7518
//   rec709Oetf(0.018) = 0.081
#pragma once

#include "osv/color/ColorMath.h"

namespace osv::color {

/// BT.2408 scene-linear -> HLG scene scale: 0.18 grey * 0.2674 = 0.04813
/// which the HLG OETF maps to the 38 % signal reference grey.
inline constexpr float kBt2408SceneScale = 0.2674f;

/// Default OOTF display peak used for PQ and Rec.709 output (nits).
inline constexpr float kDefaultPeakNits = 1000.0f;

/// HLG OOTF system gamma for a 1000 nit display.
inline constexpr float kDefaultOotfGamma = 1.2f;

/// Rec.709 SDR reference peak (nits) used as the BT.2390 EETF target.
inline constexpr float kDefaultSdrPeakNits = 100.0f;

// -----------------------------------------------------------------------------
//  float wrappers (identical to the kernel math)
// -----------------------------------------------------------------------------

/// HLG OETF, scene-linear E -> signal.
[[nodiscard]] inline float hlgOetf(float e) noexcept { return osvHlgOetf(e); }
/// HLG inverse OETF, signal -> scene-linear E.
[[nodiscard]] inline float hlgInverseOetf(float ep) noexcept { return osvHlgInverseOetf(ep); }
/// HLG OOTF luminance multiplier (see osvHlgOotfScale).
[[nodiscard]] inline float hlgOotfScale(float ys, float peakNits, float gamma) noexcept {
    return osvHlgOotfScale(ys, peakNits, gamma);
}
/// PQ inverse EOTF, nits -> code.
[[nodiscard]] inline float pqInverseEotf(float nits) noexcept { return osvPqInverseEotf(nits); }
/// PQ EOTF, code -> nits.
[[nodiscard]] inline float pqEotf(float code) noexcept { return osvPqEotf(code); }
/// BT.709 OETF, display-linear -> signal.
[[nodiscard]] inline float rec709Oetf(float e) noexcept { return osvRec709Oetf(e); }
/// BT.709 inverse OETF, signal -> display-linear.
[[nodiscard]] inline float rec709InverseOetf(float v) noexcept { return osvRec709InverseOetf(v); }
/// BT.2390 EETF on PQ codes.
[[nodiscard]] inline float bt2390Eetf(float pqCode, float srcPeakNits, float dstPeakNits) noexcept {
    return osvBt2390Eetf(pqCode, srcPeakNits, dstPeakNits);
}

// -----------------------------------------------------------------------------
//  double-precision reference implementations
// -----------------------------------------------------------------------------
namespace ref {

/// HLG OETF in double precision.
[[nodiscard]] double hlgOetf(double e) noexcept;
/// HLG inverse OETF in double precision.
[[nodiscard]] double hlgInverseOetf(double ep) noexcept;
/// HLG OOTF multiplier in double precision.
[[nodiscard]] double hlgOotfScale(double ys, double peakNits, double gamma) noexcept;
/// PQ inverse EOTF in double precision.
[[nodiscard]] double pqInverseEotf(double nits) noexcept;
/// PQ EOTF in double precision.
[[nodiscard]] double pqEotf(double code) noexcept;
/// BT.709 OETF in double precision.
[[nodiscard]] double rec709Oetf(double e) noexcept;
/// BT.709 inverse OETF in double precision.
[[nodiscard]] double rec709InverseOetf(double v) noexcept;
/// BT.2390 EETF in double precision.
[[nodiscard]] double bt2390Eetf(double pqCode, double srcPeakNits, double dstPeakNits) noexcept;

}  // namespace ref

}  // namespace osv::color
