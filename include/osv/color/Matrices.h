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
/// Derived from a least-squares fit of the Pocket 3 colour chart against a
/// Rec.2020 reference; reused for the Osmo 360 which shares the D-Log M
/// definition.  Rows sum to 1.
inline constexpr OsvMat3f kNativeToRec2020_Pocket3 = {{
    0.785301f, 0.178838f, 0.035860f,
    -0.036655f, 1.258089f, -0.221434f,
    -0.014322f, 0.077260f, 0.937062f,
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
