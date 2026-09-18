// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlowWarp.cpp - warp both views to the middle, then blend without ghosting.
//
// The two non-obvious pieces, both explained at their site below:
//
//   1. which flow field warps which view (a backward map inverts the
//      intuition, and getting it wrong doubles the disparity);
//   2. the deghosting blend, which averages where the views agree and
//      selects where they do not.

#include "osv/render/FlowWarp.h"

#include <algorithm>
#include <cmath>

namespace osv::render {

namespace {

/// Largest edge accepted, mirroring DisFlow's guard: these images come from
/// the same band pipeline and a corrupt size must not reach the indexing.
constexpr std::uint32_t kMaxEdge = 1u << 16;

/// Smoothstep on [0, 1], clamped outside it.
///
/// Used for the cross-fade rather than a linear ramp because a linear
/// cross-fade has a slope discontinuity at both ends of the feather - its
/// derivative jumps from 0 to a constant - and that shows as a faint band
/// edge on a gradient like sky, which is most of this application's input.
[[nodiscard]] double smoothstep01(double t) noexcept {
    const double c = std::clamp(t, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

}  // namespace

// ---------------------------------------------------------------------------
//  RgbaImage
// ---------------------------------------------------------------------------
void RgbaImage::resize(std::uint32_t width, std::uint32_t height) {
    w = width;
    h = height;
    data.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0.0f);
}

void RgbaImage::sample(float x, float y, float out[4]) const noexcept {
    if (!out) {
        return;
    }
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (data.empty() || w == 0 || h == 0) {
        return;
    }
    // A non-finite coordinate can only come from a diverged flow vector.
    // Returning transparent black keeps the caller's arithmetic finite; the
    // alternative is an out-of-bounds read.
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }

    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const float tx = x - fx;
    const float ty = y - fy;

    const int xa = std::clamp(x0, 0, static_cast<int>(w) - 1);
    const int xb = std::clamp(x0 + 1, 0, static_cast<int>(w) - 1);
    const int ya = std::clamp(y0, 0, static_cast<int>(h) - 1);
    const int yb = std::clamp(y0 + 1, 0, static_cast<int>(h) - 1);

    const std::size_t rowA = static_cast<std::size_t>(ya) * w;
    const std::size_t rowB = static_cast<std::size_t>(yb) * w;
    const float* pa = &data[(rowA + static_cast<std::size_t>(xa)) * 4u];
    const float* pb = &data[(rowA + static_cast<std::size_t>(xb)) * 4u];
    const float* pc = &data[(rowB + static_cast<std::size_t>(xa)) * 4u];
    const float* pd = &data[(rowB + static_cast<std::size_t>(xb)) * 4u];

    for (int c = 0; c < 4; ++c) {
        const float top = pa[c] + (pb[c] - pa[c]) * tx;
        const float bot = pc[c] + (pd[c] - pc[c]) * tx;
        out[c] = top + (bot - top) * ty;
    }
}

// ---------------------------------------------------------------------------
//  Geometry and blending primitives
// ---------------------------------------------------------------------------
double blendWeightForRow(int row, int height, double seamPosition, double featherFraction) noexcept {
    if (height <= 1) {
        return 0.5;  // a one-row band has no ramp to speak of
    }
    const double pos = static_cast<double>(row) / static_cast<double>(height - 1);
    const double centre = std::clamp(seamPosition, 0.0, 1.0);
    const double feather = std::clamp(featherFraction, 1e-6, 1.0);

    // Map position onto the feather window centred on the seam, so the ramp
    // is 0 a half-feather before the seam and 1 a half-feather after it, and
    // saturated outside.  A zero-width feather would divide by zero, which
    // is why `feather` is floored above rather than merely validated.
    const double t = (pos - (centre - 0.5 * feather)) / feather;
    return smoothstep01(t);
}

double deghostWeight(const float a[4], const float b[4], double colorDiffCoef) noexcept {
    if (!a || !b || colorDiffCoef <= 0.0) {
        return 0.0;  // deghosting disabled: caller uses the geometric ramp
    }
    // Mean absolute difference over RGB only.  Alpha is coverage, not colour,
    // and a coverage difference at the band edge is expected rather than
    // evidence of bad flow - including it would fire deghosting exactly where
    // it is least useful.
    const double diff = (std::fabs(static_cast<double>(a[0]) - b[0]) +
                         std::fabs(static_cast<double>(a[1]) - b[1]) +
                         std::fabs(static_cast<double>(a[2]) - b[2])) /
                        3.0;
    // tanh saturates, so a large disagreement cannot push the weight past 1
    // and the response is smooth in the colour difference - which matters
    // because a discontinuous deghost weight would itself be visible.
    return std::tanh(diff * colorDiffCoef);
}

// ---------------------------------------------------------------------------
//  The warp
// ---------------------------------------------------------------------------
Result<RgbaImage> warpByFlow(const RgbaImage& in, const FlowField& flow, double fraction) {
    if (!in.valid()) {
        return Error{ErrorCode::InvalidArgument, "warpByFlow: the input image is empty or malformed"};
    }
    if (!flow.valid()) {
        return Error{ErrorCode::InvalidArgument, "warpByFlow: the flow field is empty or malformed"};
    }
    if (flow.w != in.w || flow.h != in.h) {
        return Error{ErrorCode::InvalidArgument, "warpByFlow: the flow field does not match the image size"};
    }
    if (!std::isfinite(fraction)) {
        return Error{ErrorCode::InvalidArgument, "warpByFlow: a non-finite warp fraction"};
    }

    RgbaImage out;
    out.resize(in.w, in.h);
    const float t = static_cast<float>(fraction);
    for (std::uint32_t y = 0; y < in.h; ++y) {
        for (std::uint32_t x = 0; x < in.w; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * in.w + x;
            // BACKWARD map: this output pixel reads the source at
            // p + flow * t.  See the header for which field belongs here.
            const float sx = static_cast<float>(x) + flow.u[idx] * t;
            const float sy = static_cast<float>(y) + flow.v[idx] * t;
            in.sample(sx, sy, &out.data[idx * 4u]);
        }
    }
    return out;
}

Result<WarpedBand> warpToMiddle(const RgbaImage& a, const RgbaImage& b, const BidirFlow& flow,
                                const FlowWarpParams& params) {
    // ---- validate everything from the caller ------------------------------
    if (!a.valid() || !b.valid()) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: an input view is empty or malformed"};
    }
    if (a.w != b.w || a.h != b.h) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: the two views differ in size"};
    }
    if (a.w > kMaxEdge || a.h > kMaxEdge) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: view edge beyond the supported maximum"};
    }
    if (!flow.valid()) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: the bidirectional flow is incomplete"};
    }
    if (flow.forward.w != a.w || flow.forward.h != a.h) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: the flow does not match the view size"};
    }
    if (!(params.warpFraction >= 0.0 && params.warpFraction <= 1.0)) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: warpFraction must be in [0, 1]"};
    }
    if (!(params.featherFraction > 0.0 && params.featherFraction <= 1.0)) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: featherFraction must be in (0, 1]"};
    }
    if (!(params.unreliableWarpScale >= 0.0 && params.unreliableWarpScale <= 1.0)) {
        return Error{ErrorCode::InvalidArgument, "warpToMiddle: unreliableWarpScale must be in [0, 1]"};
    }

    WarpedBand out;

    // ---- prepare the two fields -------------------------------------------
    // Copy, because repairing and smoothing mutate them and the caller's
    // measurement is still needed elsewhere (diagnostics, the seam search).
    FlowField fwd = flow.forward;
    FlowField bwd = flow.backward;

    // Holes first, then smoothing.  Repair borrows from neighbours that DID
    // pass the consistency check, which leaves a faint discontinuity at the
    // hole boundary; the blur removes it.  Doing it the other way round
    // would smear the zeros before they were replaced.
    (void)repairFlow(fwd, flow.ok);
    (void)repairFlow(bwd, flow.ok);
    smoothFlow(fwd, params.postRepairSmoothSigmaPx);
    smoothFlow(bwd, params.postRepairSmoothSigmaPx);

    // Scale down the warp wherever the measurement was not trusted.  Doing
    // this per pixel in the FIELD, rather than branching in the warp loop,
    // keeps the warp a straight gather and means the reduced vectors are
    // themselves smoothed by the pass above.
    for (std::size_t i = 0; i < fwd.u.size(); ++i) {
        if (flow.ok[i] != 0) {
            ++out.reliablePixels;
            continue;
        }
        ++out.unreliablePixels;
        const float scale = static_cast<float>(params.unreliableWarpScale);
        fwd.u[i] *= scale;
        fwd.v[i] *= scale;
        bwd.u[i] *= scale;
        bwd.v[i] *= scale;
    }

    // ---- warp each view by the OTHER direction's field --------------------
    // a is moved toward the middle by the b->a field, and b by the a->b
    // field, at complementary fractions.  The header derives why.
    const double t = params.warpFraction;
    OSV_TRY_ASSIGN(out.warpedA, warpByFlow(a, bwd, t));
    OSV_TRY_ASSIGN(out.warpedB, warpByFlow(b, fwd, 1.0 - t));

    // ---- blend ------------------------------------------------------------
    out.blended.resize(a.w, a.h);
    for (std::uint32_t y = 0; y < a.h; ++y) {
        // The geometric cross-fade: 0 selects the A view, 1 selects B.
        const double ramp = blendWeightForRow(static_cast<int>(y), static_cast<int>(a.h), params.seamPosition,
                                              params.featherFraction);
        for (std::uint32_t x = 0; x < a.w; ++x) {
            const std::size_t px = (static_cast<std::size_t>(y) * a.w + x) * 4u;
            const float* pa = &out.warpedA.data[px];
            const float* pb = &out.warpedB.data[px];

            // Where the two warped views disagree, the flow was wrong and
            // averaging them would double the image.  Blend the geometric
            // ramp toward a decisive selection in proportion to how much
            // they disagree.
            const double ghost = deghostWeight(pa, pb, params.colorDiffCoef);
            double weightB = ramp;
            if (ghost > 0.0 && params.softmaxSharpness > 0.0) {
                // Two-way softmax over the ramp, which reduces to the ramp
                // at sharpness 0 and to a hard pick as sharpness grows.
                // Written as a logistic of the ramp's distance from centre
                // because that is the two-class softmax in closed form and
                // needs one exp instead of two.
                const double z = params.softmaxSharpness * (2.0 * ramp - 1.0);
                const double pick = 1.0 / (1.0 + std::exp(-z));
                weightB = ramp + (pick - ramp) * ghost;
            }
            const float wb = static_cast<float>(std::clamp(weightB, 0.0, 1.0));
            const float wa = 1.0f - wb;
            for (int c = 0; c < 4; ++c) {
                out.blended.data[px + static_cast<std::size_t>(c)] = pa[c] * wa + pb[c] * wb;
            }
        }
    }

    return out;
}

}  // namespace osv::render
