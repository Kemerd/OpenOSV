// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Host-side D-Log M helpers: double precision forward curve, the closed-form
// inverse with a bisection safety net, and curve validation.

#include "osv/color/DlogM.h"

#include <cmath>
#include <limits>

namespace osv::color {

namespace {

/// Double precision cut, mirroring osvDlogmCut exactly.
double cutD(const OsvDlogMCurve& c) noexcept {
    if (c.cutMode == OSV_DLOGM_CUT_INTERSECTION) {
        const double denom = static_cast<double>(c.slope2) - static_cast<double>(c.slope);
        if (std::fabs(denom) > 1e-12) {
            return static_cast<double>(c.intercept) / denom;
        }
    }
    return static_cast<double>(c.cut);
}

/// Forward curve in double precision (not clamped, like the kernel).
double forwardD(const OsvDlogMCurve& c, double code) noexcept {
    const double tmp = std::exp2(static_cast<double>(c.scale) * code + static_cast<double>(c.yShift)) +
                       static_cast<double>(c.xShift);
    const double cut = cutD(c);
    const double pw = (tmp < cut) ? (tmp * static_cast<double>(c.slope) + static_cast<double>(c.intercept))
                                  : (tmp * static_cast<double>(c.slope2));
    return pw * static_cast<double>(c.midGrayScaling);
}

/// Bisection inverse used when the closed form is not applicable.  The curve
/// is monotonic for valid constants, so a bracket of [lo, hi] in code space
/// shrinks to the answer in ~60 iterations of double precision.
double bisectInverseD(const OsvDlogMCurve& c, double lin) noexcept {
    double lo = -1.0;
    double hi = 2.0;
    // Widen the bracket if the target lies outside the initial range.
    for (int i = 0; i < 16 && forwardD(c, hi) < lin; ++i) {
        hi *= 2.0;
    }
    for (int i = 0; i < 16 && forwardD(c, lo) > lin; ++i) {
        lo *= 2.0;
    }
    // Standard bisection; the loop count bounds the cost for any input.
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (mid == lo || mid == hi) {
            break;
        }
        if (forwardD(c, mid) < lin) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5 * (lo + hi);
}

}  // namespace

float dlogmCut(const OsvDlogMCurve& curve) noexcept { return osvDlogmCut(&curve); }

double dlogmCutD(const OsvDlogMCurve& curve) noexcept { return cutD(curve); }

float dlogmToLinear(const OsvDlogMCurve& curve, float code) noexcept { return osvDlogmToLinear(&curve, code); }

double dlogmToLinearD(const OsvDlogMCurve& curve, double code) noexcept { return forwardD(curve, code); }

bool dlogmCurveValid(const OsvDlogMCurve& curve) noexcept {
    // Every parameter must be a finite number.
    const float values[] = {curve.xShift, curve.yShift, curve.scale,          curve.slope,
                            curve.slope2, curve.intercept, curve.midGrayScaling, curve.cut};
    for (const float v : values) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    // Monotonic increasing needs positive slopes, scale and final gain.
    if (curve.scale <= 0.0f || curve.slope <= 0.0f || curve.slope2 <= 0.0f || curve.midGrayScaling <= 0.0f) {
        return false;
    }
    // The cut mode must be one of the two documented values.
    return curve.cutMode == OSV_DLOGM_CUT_GIVEN || curve.cutMode == OSV_DLOGM_CUT_INTERSECTION;
}

double linearToDlogmD(const OsvDlogMCurve& curve, double lin) noexcept {
    // Garbage in: return black rather than propagating NaN.
    if (!std::isfinite(lin)) {
        return 0.0;
    }
    if (!dlogmCurveValid(curve)) {
        return 0.0;
    }
    const double mgs = static_cast<double>(curve.midGrayScaling);
    const double slope = static_cast<double>(curve.slope);
    const double slope2 = static_cast<double>(curve.slope2);
    const double intercept = static_cast<double>(curve.intercept);
    const double xShift = static_cast<double>(curve.xShift);
    const double yShift = static_cast<double>(curve.yShift);
    const double scale = static_cast<double>(curve.scale);
    const double cut = cutD(curve);

    // Undo the final gain to get the piecewise value.
    const double pw = lin / mgs;
    // Piecewise value at the cut decides which branch to invert.  With the
    // intersection rule both branches agree there; with a given cut the
    // forward function uses `tmp < cut`, so the toe branch applies to
    // pw < cut*slope+intercept.
    const double pwAtCut = cut * slope + intercept;
    double tmp;
    if (pw < pwAtCut) {
        tmp = (pw - intercept) / slope;
    } else {
        tmp = pw / slope2;
    }
    // Invert the exponential part; the argument of log2 must be positive.
    const double arg = tmp - xShift;
    if (arg <= 0.0) {
        // Below the curve's floor: fall back to bisection which clamps at the
        // bracket edge, then clamp at 0 (the curve's natural floor).
        const double b = bisectInverseD(curve, lin);
        return b < 0.0 ? 0.0 : b;
    }
    const double code = (std::log2(arg) - yShift) / scale;
    if (!std::isfinite(code)) {
        return 0.0;
    }
    return code;
}

float linearToDlogm(const OsvDlogMCurve& curve, float lin) noexcept {
    return static_cast<float>(linearToDlogmD(curve, static_cast<double>(lin)));
}

}  // namespace osv::color
