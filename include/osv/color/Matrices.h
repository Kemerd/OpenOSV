// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Colour matrices used by the pipeline.  All are row-major 3x3 OsvMat3f
// constants applied as out = M * in with column vectors (see osvMat3Apply).
// Every RGB->RGB matrix has rows that sum to 1 so white is preserved; the
// tests assert this.
#pragma once

#include "osv/color/ColorMath.h"

namespace osv::color {

/// Identity (used for HLG/PQ output where working == output primaries).
inline constexpr OsvMat3f kIdentity3 = {{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}};

/// Camera native primaries (DJI D-Log M, Pocket 3 fit) -> Rec.2020 linear.
/// Thatcher Freeman's published colour-chart matrix for the Pocket 3 (camera
/// native -> DaVinci Wide Gamut, "DJI Pocket 3 D-Log M to DWG.dctl",
/// github.com/thatcherfreeman/dwg-transforms) composed with the DaVinci Wide
/// Gamut -> Rec.2020 primaries conversion (see NOTICE).  Rows sum to 1.
///
/// This was the Osmo 360 default until kNativeToRec2020_Osmo360 below was
/// fitted, on the assumption that a shared D-Log M curve implied shared
/// primaries.  It does not: measured against DJI's own Osmo 360 reference LUT
/// this matrix carries more than twice the full-cube error of the Osmo 360
/// fit (0.0947 vs 0.0450 RMS in HLG code units).  It is kept because it is the
/// provenance record of what shipped, because other DJI bodies may genuinely
/// use these primaries, and because a project already graded against it must
/// keep rendering the same way (`--fit pocket3`).
///
/// It is also not physically realisable as a set of camera primaries: its
/// implied blue primary lands at x = 0.1417, y = -0.0809 with luminance
/// Y = -0.0851.  A negative-luminance primary cannot exist, which is a second,
/// independent reason not to use it for the Osmo 360.  scripts/fit_primaries.py
/// prints this check for both matrices.
inline constexpr OsvMat3f kNativeToRec2020_Pocket3 = {{
    0.785301f, 0.178838f, 0.035860f,
    -0.036655f, 1.258089f, -0.221434f,
    -0.014322f, 0.077260f, 0.937062f,
}};

/// Osmo 360 camera native primaries -> Rec.2020 linear.  The default matrix,
/// paired with kDlogMOsmo360 (see osv::color::nativeToWorkingForFit).
///
/// Provenance: produced by
///     python scripts/fit_primaries.py \
///         --from-cube DJI_Osmo360_DLogM_to_Rec709.cube
/// which measures all 35937 entries of DJI's own Osmo 360 D-Log M -> Rec.709
/// LUT.  Only these nine constants are shipped; no LUT data from that file is
/// redistributed (see NOTICE).  The tone curve kDlogMOsmo360 was fitted from
/// the same file's 33 neutral-axis entries; this matrix comes from the other
/// 35904, which is where the gamut information lives.
///
/// Why a tone curve cannot supply this: a per-channel curve can never move
/// energy between channels, yet the reference plainly does - a red-only input
/// of 0.500 renders as (0.6071, 0.0000, 0.0534), leaking 0.053 into blue, and
/// a blue-only 0.500 renders as (0.0066, 0.0000, 0.6201), leaking into red.
/// Cross-channel terms of that shape are exactly what a primaries matrix
/// produces, so they are recoverable from the file by inverting the output
/// transform and solving for the 3x3.
///
/// Method (the algebra is spelled out in scripts/fit_primaries.py): for every
/// entry, decode the input codes to native linear with kDlogMOsmo360, then
/// least-squares solve the 3x3 against the reference output through the exact
/// forward chain this header's matrices feed -
///     native -> M -> x sceneScale -> kRec2020ToRec709 -> HLG OETF -> clamp
/// - minimising the residual in output HLG code units (not in scene-linear,
/// which would over-weight highlights by orders of magnitude because the HLG
/// OETF is log-like).  Six free parameters: the third column of each row is
/// parameterised as 1 - a - b so unit row sums hold identically rather than
/// approximately.  Four spread-out starts all converge to this basin, with an
/// RMS spread of 5e-5.
///
/// Results against that reference, in HLG code units, Pocket 3 -> this matrix:
///   full cube (35937 entries) : 0.094685 -> 0.044988 RMS (52.5 % lower),
///                               worst 0.603319 -> 0.262716;
///   saturated entries (83.9 %): 0.100171 -> 0.046485 RMS (53.6 % lower);
///   neutral axis (33 entries) : 0.023251 -> 0.023251 RMS, i.e. unchanged.
///
/// The neutral axis is unchanged *necessarily*, not coincidentally.  For a
/// neutral native triple (L, L, L) and any matrix whose rows sum to 1,
///     (M * (L, L, L))[j] = L * (M[j][0] + M[j][1] + M[j][2]) = L,
/// so M acts as the identity on neutrals and the output depends only on the
/// tone curve.  Swapping one unit-row-sum matrix for another therefore cannot
/// move 18 % grey off HLG 0.380 or disturb any BT.2408 anchor; the measured
/// swing over the reference's neutral axis is 4.1e-7, which is float round-off
/// in the two matrix products.  tests/unit/test_color.cpp asserts this rather
/// than assuming it.
///
/// Physically plausible, checked rather than asserted: determinant +0.873348,
/// diagonal (0.8073, 0.9907, 1.1039) all positive, rows summing to 1 exactly
/// in float32, and implied native primaries at R x=0.6914 y=0.3206,
/// G x=0.2616 y=0.8225, B x=0.1448 y=0.0372 - a gamut a little wider than
/// Rec.709 and a little narrower than Rec.2020, which is what a 1/1.7"-class
/// sensor should look like.  All three primaries have positive luminance,
/// unlike kNativeToRec2020_Pocket3.  The white point lands on D65 exactly,
/// which the unit row sums guarantee.
///
/// What this does NOT fit, stated plainly: about 37 % of the reference's
/// entries sit on the 0 or 1 output boundary and roughly 71 % of the cube is
/// outside the Rec.709 output gamut, so on most saturated entries DJI's table
/// holds a *gamut-mapped* value rather than a matrixed one - deeply saturated
/// reds retain ~0.27 of green where a matrix plus clamp yields 0.  No 3x3 can
/// reproduce that, because it is not a linear operation, and those entries
/// dominate the residual worst case (0.2627) and always will.  Restricting the
/// fit to the ~24 % of entries that are clean on both sides does not improve
/// it, which is the evidence that the remaining error is DJI's gamut
/// compression and not a mis-fitted matrix.  Reproducing that compression is
/// out of scope for a primaries matrix and is not claimed here.
inline constexpr OsvMat3f kNativeToRec2020_Osmo360 = {{
    0.807268560f, 0.152663648f, 0.040067792f,
    0.042878162f, 0.990737677f, -0.033615828f,
    -0.009603872f, -0.094263740f, 1.103867650f,
}};

/// DJI Avata 360 native primaries -> Rec.2020 linear, paired with
/// kDlogMAvata360 (see osv::color::nativeToWorkingForFit).
///
/// Provenance: fitted jointly with kDlogMAvata360 to paired footage (a D-Log
/// M clip against DJI Studio's export of it), on all samples rather than the
/// neutral ones only; the method and the held-out result are in the comment
/// on kDlogMAvata360.  Rows sum to 1 exactly in float32, so white stays white
/// and the neutral axis depends on the curve alone.  Determinant +1.075248,
/// positive diagonal.
///
/// What it is not: a sensor characterisation.  The footage was a room of
/// mostly white and beige surfaces, so saturated colours are weakly
/// constrained.  All three implied native primaries have positive luminance,
/// but the red one lies outside the spectral locus (Z < 0, x = 4.15), and some
/// saturated codes land outside Rec.2020 (code 0.6 / 0.3 / 0.2 gives blue
/// -0.068).  It models DJI Studio's rendering of this camera, no more.
inline constexpr OsvMat3f kNativeToRec2020_Avata360 = {{
    0.706524789f, 0.176516764f, 0.116958447f,
    -0.183691555f, 1.116191272f, 0.067500283f,
    -0.302259748f, 0.033496898f, 1.268762850f,
}};

/// Rec.2020 linear -> Rec.709 linear (BT.2087 / derived from the primaries).
inline constexpr OsvMat3f kRec2020ToRec709 = {{
    1.660491f, -0.587641f, -0.072850f,
    -0.124550f, 1.132900f, -0.008349f,
    -0.018151f, -0.100579f, 1.118730f,
}};

/// Rec.709 linear -> Rec.2020 linear (inverse of the above).
inline constexpr OsvMat3f kRec709ToRec2020 = {{
    0.627404f, 0.329283f, 0.043313f,
    0.069097f, 0.919540f, 0.011362f,
    0.016391f, 0.088013f, 0.895595f,
}};

/// BT.709 Y'CbCr -> R'G'B' on range-expanded values (Y' in [0,1], Cb/Cr in
/// [-0.5, 0.5]).  Kr = 0.2126, Kb = 0.0722.  "Narrow" refers to the range
/// expansion that precedes it in osvYuvToCode; the matrix itself is the same
/// for full range.
inline constexpr OsvMat3f kYuvToRgb709Narrow = {{
    1.0f, 0.0f, 1.5748f,
    1.0f, -0.1873243f, -0.4681243f,
    1.0f, 1.8556f, 0.0f,
}};

/// BT.2020 non-constant-luminance Y'CbCr -> R'G'B' (Kr = 0.2627, Kb = 0.0593),
/// used for HLG clips which the camera tags as BT.2020.
inline constexpr OsvMat3f kYuvToRgb2020Narrow = {{
    1.0f, 0.0f, 1.4746f,
    1.0f, -0.1645531f, -0.5713529f,
    1.0f, 1.8814f, 0.0f,
}};

/// Sum of one row of a matrix (helper for white-preservation checks).
[[nodiscard]] constexpr float mat3RowSum(const OsvMat3f& m, int row) noexcept {
    if (row < 0 || row > 2) {
        return 0.0f;
    }
    return m.m[row * 3] + m.m[row * 3 + 1] + m.m[row * 3 + 2];
}

/// Product a * b (row-major), for composing matrices on the host.
[[nodiscard]] constexpr OsvMat3f mat3Mul(const OsvMat3f& a, const OsvMat3f& b) noexcept {
    OsvMat3f r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
        }
    }
    return r;
}

/// Inverse of a 3x3 matrix (adjugate / determinant).  Returns false and
/// leaves `out` as the identity when the matrix is singular (|det| < 1e-12),
/// so a caller can never divide by zero.
[[nodiscard]] constexpr bool mat3Inverse(const OsvMat3f& a, OsvMat3f& out) noexcept {
    const float* m = a.m;
    // Cofactors of the first row give the determinant by expansion.
    const float c00 = m[4] * m[8] - m[5] * m[7];
    const float c01 = m[3] * m[8] - m[5] * m[6];
    const float c02 = m[3] * m[7] - m[4] * m[6];
    const float det = m[0] * c00 - m[1] * c01 + m[2] * c02;
    if (det < 1e-12f && det > -1e-12f) {
        out = kIdentity3;
        return false;
    }
    const float inv = 1.0f / det;
    // Adjugate (transposed cofactor matrix) scaled by 1 / det.
    out.m[0] = c00 * inv;
    out.m[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
    out.m[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
    out.m[3] = -c01 * inv;
    out.m[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
    out.m[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
    out.m[6] = c02 * inv;
    out.m[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
    out.m[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
    return true;
}

}  // namespace osv::color
