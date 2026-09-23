// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Host side of the display looks: turns the fitted constants in Look.h into
// the kernel block osvLookApply reads, and validates such blocks.

#include "osv/color/Look.h"

#include "osv/color/DlogM.h"
#include "osv/color/Matrices.h"

#include <cmath>
#include <cstring>

namespace osv::color {

namespace {

/// Tolerance on a matrix row sum: float32 storage of a unit-row-sum matrix
/// misses 1 by a few ulp, never by more than this.
constexpr float kRowSumTolerance = 1e-4f;

/// A zeroed block: id OSV_LOOK_STANDARD, which the kernel skips entirely.
OsvLookParams noLook() noexcept {
    OsvLookParams p{};
    std::memset(&p, 0, sizeof(p));
    p.id = OSV_LOOK_STANDARD;
    return p;
}

/// True when every element is finite.
bool finite(const float* values, int count) noexcept {
    if (values == nullptr || count < 0) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

/// True when all three rows of `m` sum to 1 (white in, white out).
bool unitRows(const OsvMat3f& m) noexcept {
    for (int r = 0; r < 3; ++r) {
        if (std::fabs(mat3RowSum(m, r) - 1.0f) > kRowSumTolerance) {
            return false;
        }
    }
    return true;
}

/**
 * @brief ACES reference gamut compression scale for one channel.
 *
 * The compression curve is d' = t + (d - t) / (1 + ((d - t) / s)^p)^(1/p);
 * choosing s = (l - t) / (((1 - t) / (l - t))^-p - 1)^(1/p) makes a distance
 * of exactly `limit` land on 1, i.e. the colour the matrices pushed `limit`
 * times as far below the max as a zero channel is brought back onto the gamut
 * boundary.  Degenerate inputs (limit <= threshold, threshold >= 1) return a
 * huge scale, which makes the compression the identity rather than a NaN.
 */
float gamutScale(float threshold, float limit, float power) noexcept {
    const double t = threshold;
    const double l = limit;
    const double p = power < 1.0f ? 1.0 : static_cast<double>(power);
    if (!(l > t) || !(t < 1.0) || !(l > 1.0)) {
        return 1e30f;
    }
    const double base = std::pow((1.0 - t) / (l - t), -p) - 1.0;
    if (!(base > 0.0)) {
        return 1e30f;
    }
    const double s = (l - t) / std::pow(base, 1.0 / p);
    return std::isfinite(s) && s > 0.0 ? static_cast<float>(s) : 1e30f;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Queries
// -----------------------------------------------------------------------------
bool lookApplies(Look look, OutputTransfer transfer) noexcept {
    // The only fitted look is the DJI Studio Rec.709 rendering; DJI ships no
    // colour-managed HDR rendering to fit an HLG or PQ look against (Look.h).
    return look == Look::DjiStudio && transfer == OutputTransfer::Rec709;
}

Look lookOf(const OsvColorParams& params) noexcept {
    return params.look.id == OSV_LOOK_DJI ? Look::DjiStudio : Look::Standard;
}

// -----------------------------------------------------------------------------
//  Tangents
// -----------------------------------------------------------------------------
bool monotoneTangents(const float* tone, int count, float* slopes) noexcept {
    if (slopes == nullptr) {
        return false;
    }
    if (tone == nullptr || count < 2 || count > OSV_LOOK_MAX_KNOTS) {
        for (int i = 0; i < OSV_LOOK_MAX_KNOTS; ++i) {
            slopes[i] = 0.0f;
        }
        return false;
    }
    // Secant slopes per unit u on the uniform grid (spacing 1 / (count - 1)).
    const double inv = static_cast<double>(count - 1);
    double secant[OSV_LOOK_MAX_KNOTS] = {};
    for (int i = 0; i + 1 < count; ++i) {
        secant[i] = (static_cast<double>(tone[i + 1]) - static_cast<double>(tone[i])) * inv;
    }
    // Ends take the adjacent secant; interior knots the harmonic mean of the
    // two secants, or zero at a local extremum (Fritsch-Carlson): the choice
    // that guarantees a monotonic knot set stays monotonic between knots.
    slopes[0] = static_cast<float>(secant[0]);
    slopes[count - 1] = static_cast<float>(secant[count - 2]);
    for (int i = 1; i + 1 < count; ++i) {
        const double a = secant[i - 1];
        const double b = secant[i];
        slopes[i] = (a * b <= 0.0) ? 0.0f : static_cast<float>(2.0 / (1.0 / a + 1.0 / b));
    }
    // Unused tail entries are zero so the block is fully defined.
    for (int i = count; i < OSV_LOOK_MAX_KNOTS; ++i) {
        slopes[i] = 0.0f;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  Kernel block
// -----------------------------------------------------------------------------
OsvLookParams makeLookParams(Look look, OutputTransfer transfer) noexcept {
    if (!lookApplies(look, transfer)) {
        return noLook();
    }
    const LookFit& fit = kLookDjiRec709;
    OsvLookParams p = noLook();
    p.id = OSV_LOOK_DJI;
    p.knots = OSV_LOOK_MAX_KNOTS;

    // The fit maps Osmo 360 native light; the kernel hands the look Rec.2020
    // working light (after nativeToWorking), so undo that matrix first.  A
    // singular camera matrix cannot happen with the shipped constants, but if
    // it ever did the look is dropped rather than applied to the wrong space.
    OsvMat3f workingToNative{};
    if (!mat3Inverse(kNativeToRec2020_Osmo360, workingToNative)) {
        return noLook();
    }
    p.toLook = mat3Mul(fit.nativeToLook, workingToNative);

    // Shaper: the Osmo 360 D-Log M curve, with its tangent at code 0 for the
    // linear continuation below.  d(lin)/d(code) at code 0 is analytic:
    // ln 2 * scale * 2^yShift times the slope of whichever branch code 0 is on
    // (the toe for every shipped curve), times midGrayScaling - the same
    // expression scripts/fit_look.py fitted with, in double.
    p.shaper = kDlogMOsmo360;
    p.shaperLin0 = dlogmToLinear(kDlogMOsmo360, 0.0f);
    {
        const OsvDlogMCurve& c = kDlogMOsmo360;
        const double tmp0 = std::exp2(static_cast<double>(c.yShift)) + static_cast<double>(c.xShift);
        const double branch = tmp0 < dlogmCutD(c) ? static_cast<double>(c.slope) : static_cast<double>(c.slope2);
        const double d = std::log(2.0) * static_cast<double>(c.scale) * std::exp2(static_cast<double>(c.yShift)) *
                         branch * static_cast<double>(c.midGrayScaling);
        p.shaperCodePerLin0 = d > 1e-12 ? static_cast<float>(1.0 / d) : 0.0f;
    }

    // Tone knots and their monotone tangents.
    for (int i = 0; i < OSV_LOOK_MAX_KNOTS; ++i) {
        p.tone[i] = fit.tone[i];
    }
    if (!monotoneTangents(p.tone, p.knots, p.toneSlope)) {
        return noLook();
    }

    // Highlight hue preservation, display matrix, gamut compression.
    p.hueStart = fit.hueStart;
    p.hueWidth = fit.hueWidth;
    p.hueAmount = fit.hueAmount;
    p.hueExponent = fit.hueExponent;
    p.display = fit.display;
    p.gamutPower = fit.gamutPower < 1.0f ? 1.0f : fit.gamutPower;
    for (int c = 0; c < 3; ++c) {
        p.gamutThreshold[c] = fit.gamutThreshold[c];
        p.gamutScale[c] = gamutScale(fit.gamutThreshold[c], fit.gamutLimit[c], p.gamutPower);
    }
    return lookParamsValid(p) ? p : noLook();
}

void setLook(OsvColorParams& params, Look look) noexcept {
    // Sanitise the stored transfer the same way makeColorParams does, so a
    // corrupt block cannot select a look for an unknown output.
    const int t = params.transfer;
    if (t < OSV_TRANSFER_HLG || t > OSV_TRANSFER_PASSTHROUGH) {
        params.look = noLook();
        return;
    }
    params.look = makeLookParams(look, static_cast<OutputTransfer>(t));
}

bool lookParamsValid(const OsvLookParams& look) noexcept {
    // "No look" is always valid: the kernel never reads the rest of it.
    if (look.id == OSV_LOOK_STANDARD) {
        return true;
    }
    if (look.id != OSV_LOOK_DJI) {
        return false;
    }
    if (look.knots < 2 || look.knots > OSV_LOOK_MAX_KNOTS) {
        return false;
    }
    // Every float that is read must be finite.
    const float scalars[] = {look.shaperLin0, look.shaperCodePerLin0, look.hueStart, look.hueWidth,
                             look.hueAmount, look.hueExponent, look.gamutPower};
    if (!finite(scalars, static_cast<int>(sizeof(scalars) / sizeof(scalars[0]))) ||
        !finite(look.toLook.m, 9) || !finite(look.display.m, 9) || !finite(look.tone, look.knots) ||
        !finite(look.toneSlope, look.knots) || !finite(look.gamutThreshold, 3) || !finite(look.gamutScale, 3)) {
        return false;
    }
    // White in, white out: both matrices must have unit row sums.
    if (!unitRows(look.toLook) || !unitRows(look.display)) {
        return false;
    }
    // The shaper must be a usable monotonic curve.
    if (!dlogmCurveValid(look.shaper)) {
        return false;
    }
    // Strictly increasing knots with non-negative tangents: T monotonic.
    for (int i = 0; i + 1 < look.knots; ++i) {
        if (!(look.tone[i + 1] > look.tone[i])) {
            return false;
        }
    }
    for (int i = 0; i < look.knots; ++i) {
        if (look.toneSlope[i] < 0.0f) {
            return false;
        }
    }
    // The colour stages' ranges.
    if (!(look.hueWidth > 0.0f) || look.hueAmount < 0.0f || look.hueAmount > 1.0f || !(look.hueExponent > 0.0f)) {
        return false;
    }
    if (look.gamutPower < 1.0f) {
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        if (look.gamutThreshold[c] < 0.0f || look.gamutThreshold[c] >= 1.0f || !(look.gamutScale[c] > 0.0f)) {
            return false;
        }
    }
    return true;
}

}  // namespace osv::color
