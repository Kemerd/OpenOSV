// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_flowwarp.cpp - does warping actually reduce the misalignment?
//
// That is the only claim that matters, and it is the one asserted here: two
// views of the same content, offset by a known parallax, must agree with each
// other MORE after warpToMiddle than before it.  A warp of the right
// magnitude in the wrong direction doubles the disparity instead of
// cancelling it and still produces a plausible-looking image, so the sign of
// the improvement is checked, not merely its presence.

#include "osv/render/DisFlow.h"
#include "osv/render/FlowWarp.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using osv::render::BidirFlow;
using osv::render::blendWeightForRow;
using osv::render::deghostWeight;
using osv::render::DisFlowParams;
using osv::render::disFlowBidirectional;
using osv::render::FlowField;
using osv::render::FlowWarpParams;
using osv::render::GrayImage;
using osv::render::RgbaImage;
using osv::render::warpByFlow;
using osv::render::warpToMiddle;

namespace {

/// A textured RGBA band, with each channel given a different pattern so a
/// channel swap or a channel-wide smear cannot pass unnoticed.
RgbaImage texturedRgba(std::uint32_t w, std::uint32_t h, double shiftX = 0.0, double shiftY = 0.0) {
    RgbaImage img;
    img.resize(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const double fx = static_cast<double>(x) - shiftX;
            const double fy = static_cast<double>(y) - shiftY;
            const std::size_t px = (static_cast<std::size_t>(y) * w + x) * 4u;
            img.data[px + 0] = static_cast<float>(0.5 + 0.3 * std::sin(0.19 * fx) * std::cos(0.11 * fy));
            img.data[px + 1] = static_cast<float>(0.5 + 0.25 * std::sin(0.13 * fx + 0.9 * fy));
            img.data[px + 2] = static_cast<float>(0.5 + 0.2 * std::cos(0.07 * (fx + fy)));
            img.data[px + 3] = 1.0f;
        }
    }
    return img;
}

/// Grey version of an RGBA image, for feeding the flow solver.
GrayImage lumaOf(const RgbaImage& img) {
    GrayImage grey;
    grey.w = img.w;
    grey.h = img.h;
    grey.data.resize(static_cast<std::size_t>(img.w) * img.h);
    for (std::size_t i = 0; i < grey.data.size(); ++i) {
        const std::size_t px = i * 4u;
        grey.data[i] = 0.299f * img.data[px] + 0.587f * img.data[px + 1] + 0.114f * img.data[px + 2];
    }
    return grey;
}

/// Mean absolute RGB difference between two images over an interior region.
///
/// The margin skips the border, where clamp-to-edge sampling means the two
/// views genuinely cannot agree - a warp cannot invent content that was
/// never in the frame, and counting that would mask a real improvement.
double meanRgbDiff(const RgbaImage& a, const RgbaImage& b, int margin) {
    double sum = 0.0;
    std::uint64_t count = 0;
    for (int y = margin; y < static_cast<int>(a.h) - margin; ++y) {
        for (int x = margin; x < static_cast<int>(a.w) - margin; ++x) {
            const std::size_t px = (static_cast<std::size_t>(y) * a.w + static_cast<std::size_t>(x)) * 4u;
            for (int c = 0; c < 3; ++c) {
                sum += std::fabs(static_cast<double>(a.data[px + static_cast<std::size_t>(c)]) -
                                 b.data[px + static_cast<std::size_t>(c)]);
                ++count;
            }
        }
    }
    return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

}  // namespace

TEST_CASE("warpToMiddle reduces the disagreement between two views", "[render][warp]") {
    // THE test.  Two views of the same content, 4 px of horizontal parallax
    // and 2 px of vertical.  After warping both to the middle they must
    // agree better than they did before.
    constexpr std::uint32_t kW = 192;
    constexpr std::uint32_t kH = 96;
    const RgbaImage a = texturedRgba(kW, kH, 0.0, 0.0);
    const RgbaImage b = texturedRgba(kW, kH, 4.0, 2.0);

    const double before = meanRgbDiff(a, b, 24);
    INFO("mean |RGB| difference before warping: " << before);
    REQUIRE(before > 0.01);  // the fixture really is misaligned

    DisFlowParams flowParams;
    const auto flow = disFlowBidirectional(lumaOf(a), lumaOf(b), flowParams, nullptr);
    REQUIRE(flow.ok());

    FlowWarpParams warpParams;
    const auto warped = warpToMiddle(a, b, flow.value(), warpParams);
    REQUIRE(warped.ok());
    REQUIRE(warped.value().valid());
    REQUIRE(warped.value().blended.w == kW);
    REQUIRE(warped.value().blended.h == kH);

    const double after = meanRgbDiff(warped.value().warpedA, warped.value().warpedB, 24);
    INFO("mean |RGB| difference after warping to the middle: " << after);
    // The sign of this comparison is the entire point: a warp applied with
    // the fields swapped would make `after` LARGER than `before`.
    CHECK(after < before);
    // And it should be a real improvement, not a rounding-level one.
    CHECK(after < 0.5 * before);
}

TEST_CASE("warpByFlow moves content in the direction the flow says", "[render][warp]") {
    // Pinned on its own because the two-view test averages two warps
    // together, which hides a sign error in either one.
    constexpr std::uint32_t kW = 64;
    constexpr std::uint32_t kH = 64;
    RgbaImage img;
    img.resize(kW, kH);
    // A single bright pixel, so its position after the warp is unambiguous.
    const std::size_t spot = (static_cast<std::size_t>(32) * kW + 20) * 4u;
    img.data[spot + 0] = 1.0f;
    img.data[spot + 1] = 1.0f;
    img.data[spot + 2] = 1.0f;
    img.data[spot + 3] = 1.0f;

    // A constant field of +6 in x.  The map is BACKWARD, so out(p) reads
    // in(p + 6): content appears to move by -6, i.e. the spot at x = 20
    // lands at x = 14.
    FlowField flow;
    flow.resize(kW, kH);
    for (float& value : flow.u) {
        value = 6.0f;
    }

    const auto warped = warpByFlow(img, flow, 1.0);
    REQUIRE(warped.ok());

    const auto brightestX = [&](const RgbaImage& src, int row) {
        int bestX = -1;
        float best = 0.0f;
        for (std::uint32_t x = 0; x < src.w; ++x) {
            const float value = src.data[(static_cast<std::size_t>(row) * src.w + x) * 4u];
            if (value > best) {
                best = value;
                bestX = static_cast<int>(x);
            }
        }
        return bestX;
    };
    CHECK(brightestX(img, 32) == 20);
    CHECK(brightestX(warped.value(), 32) == 14);

    SECTION("a zero fraction is the identity") {
        const auto same = warpByFlow(img, flow, 0.0);
        REQUIRE(same.ok());
        CHECK(same.value().data == img.data);
    }

    SECTION("half the fraction moves half as far") {
        const auto half = warpByFlow(img, flow, 0.5);
        REQUIRE(half.ok());
        CHECK(brightestX(half.value(), 32) == 17);
    }
}

TEST_CASE("blendWeightForRow ramps monotonically through the seam", "[render][warp]") {
    constexpr int kH = 100;

    // Saturated on both sides, so the two views are untouched away from the
    // seam and only the feather mixes them.
    CHECK(blendWeightForRow(0, kH, 0.5, 0.2) == Catch::Approx(0.0));
    CHECK(blendWeightForRow(kH - 1, kH, 0.5, 0.2) == Catch::Approx(1.0));
    // Half way AT the seam.  The seam of a 100-row band sits at position
    // 0.5, i.e. row 49.5 - there is no integer row exactly on it - so the
    // check is that the two rows straddling it bracket 0.5 rather than that
    // either one equals it.  Asserting row 50 == 0.5 would be asserting an
    // off-by-one (its position is 50/99 = 0.505).
    const double below = blendWeightForRow(49, kH, 0.5, 0.2);
    const double above = blendWeightForRow(50, kH, 0.5, 0.2);
    INFO("rows 49/50 straddle the seam: " << below << " .. " << above);
    CHECK(below < 0.5);
    CHECK(above > 0.5);

    SECTION("monotonic, so the cross-fade never reverses") {
        double previous = -1.0;
        for (int row = 0; row < kH; ++row) {
            const double weight = blendWeightForRow(row, kH, 0.5, 0.25);
            INFO("row " << row << " weight " << weight);
            CHECK(weight >= previous - 1e-12);
            CHECK(weight >= 0.0);
            CHECK(weight <= 1.0);
            previous = weight;
        }
    }

    SECTION("the seam position moves the ramp") {
        // A seam at 0.25 must cross 0.5 at a quarter of the band, not at the
        // middle - this is what lets the blend follow a searched seam.  The
        // crossing row is seam * (h - 1), so it is bracketed rather than
        // hit exactly, as above.
        for (const double seam : {0.25, 0.5, 0.75}) {
            const int crossing = static_cast<int>(seam * (kH - 1));
            INFO("seam " << seam << " crossing near row " << crossing);
            CHECK(blendWeightForRow(crossing - 1, kH, seam, 0.2) < 0.5);
            CHECK(blendWeightForRow(crossing + 2, kH, seam, 0.2) > 0.5);
            // And the ramp is centred there, not merely passing through:
            // equal distances either side must be symmetric about 0.5.
            const double lo = blendWeightForRow(crossing - 5, kH, seam, 0.4);
            const double hi = blendWeightForRow(crossing + 6, kH, seam, 0.4);
            CHECK_THAT(lo + hi, Catch::Matchers::WithinAbs(1.0, 0.1));
        }
    }

    SECTION("degenerate inputs do not divide by zero") {
        CHECK(std::isfinite(blendWeightForRow(0, 1, 0.5, 0.2)));
        CHECK(std::isfinite(blendWeightForRow(0, 0, 0.5, 0.2)));
        CHECK(std::isfinite(blendWeightForRow(5, 10, 0.5, 0.0)));
        CHECK(std::isfinite(blendWeightForRow(5, 10, -3.0, 5.0)));
    }
}

TEST_CASE("deghostWeight is zero on agreement and rises with disagreement", "[render][warp]") {
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const float same[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const float close[4] = {0.52f, 0.5f, 0.5f, 1.0f};
    const float far[4] = {0.9f, 0.1f, 0.8f, 1.0f};

    CHECK(deghostWeight(grey, same, 10.0) == Catch::Approx(0.0));
    const double near = deghostWeight(grey, close, 10.0);
    const double wide = deghostWeight(grey, far, 10.0);
    INFO("near " << near << " wide " << wide);
    CHECK(near > 0.0);
    CHECK(wide > near);
    CHECK(wide <= 1.0);  // tanh saturates rather than overshooting

    SECTION("alpha is excluded, because coverage is not colour") {
        // A band edge differs in coverage but not in colour, and deghosting
        // there would fire where it is least useful.
        const float transparent[4] = {0.5f, 0.5f, 0.5f, 0.0f};
        CHECK(deghostWeight(grey, transparent, 10.0) == Catch::Approx(0.0));
    }

    SECTION("a non-positive coefficient disables it") {
        CHECK(deghostWeight(grey, far, 0.0) == Catch::Approx(0.0));
        CHECK(deghostWeight(grey, far, -1.0) == Catch::Approx(0.0));
    }

    SECTION("null inputs are refused rather than dereferenced") {
        CHECK(deghostWeight(nullptr, far, 10.0) == Catch::Approx(0.0));
        CHECK(deghostWeight(grey, nullptr, 10.0) == Catch::Approx(0.0));
    }
}

TEST_CASE("warpToMiddle counts reliable and unreliable pixels", "[render][warp]") {
    // The counts are the diagnostic that says whether the flow was usable at
    // all, so they must add up to the pixel count exactly.
    const RgbaImage a = texturedRgba(64, 48, 0.0, 0.0);
    const RgbaImage b = texturedRgba(64, 48, 2.0, 0.0);

    BidirFlow flow;
    flow.forward.resize(64, 48);
    flow.backward.resize(64, 48);
    flow.ok.assign(flow.forward.u.size(), 1u);
    // Mark a block as failed.
    for (int y = 10; y < 20; ++y) {
        for (int x = 10; x < 30; ++x) {
            flow.ok[static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)] = 0u;
        }
    }

    FlowWarpParams params;
    const auto warped = warpToMiddle(a, b, flow, params);
    REQUIRE(warped.ok());
    CHECK(warped.value().unreliablePixels == 10u * 20u);
    CHECK(warped.value().reliablePixels + warped.value().unreliablePixels == flow.forward.u.size());
}

TEST_CASE("warpToMiddle rejects unusable inputs", "[render][warp]") {
    const RgbaImage good = texturedRgba(48, 32);
    BidirFlow flow;
    flow.forward.resize(48, 32);
    flow.backward.resize(48, 32);
    flow.ok.assign(flow.forward.u.size(), 1u);
    const FlowWarpParams params;

    SECTION("an empty view") {
        const RgbaImage empty;
        CHECK_FALSE(warpToMiddle(empty, good, flow, params).ok());
        CHECK_FALSE(warpToMiddle(good, empty, flow, params).ok());
    }

    SECTION("views that differ in size") {
        const RgbaImage other = texturedRgba(32, 32);
        CHECK_FALSE(warpToMiddle(good, other, flow, params).ok());
    }

    SECTION("a flow field that does not match the views") {
        BidirFlow wrong;
        wrong.forward.resize(16, 16);
        wrong.backward.resize(16, 16);
        wrong.ok.assign(wrong.forward.u.size(), 1u);
        CHECK_FALSE(warpToMiddle(good, good, wrong, params).ok());
    }

    SECTION("an incomplete bidirectional flow") {
        BidirFlow partial;
        partial.forward.resize(48, 32);
        // backward left empty, ok left empty
        CHECK_FALSE(warpToMiddle(good, good, partial, params).ok());
    }

    SECTION("parameters outside their documented range") {
        FlowWarpParams bad = params;
        bad.warpFraction = 1.5;
        CHECK_FALSE(warpToMiddle(good, good, flow, bad).ok());
        bad = params;
        bad.warpFraction = -0.1;
        CHECK_FALSE(warpToMiddle(good, good, flow, bad).ok());
        bad = params;
        bad.featherFraction = 0.0;
        CHECK_FALSE(warpToMiddle(good, good, flow, bad).ok());
        bad = params;
        bad.unreliableWarpScale = 2.0;
        CHECK_FALSE(warpToMiddle(good, good, flow, bad).ok());
    }
}

TEST_CASE("a zero flow field leaves both views untouched", "[render][warp]") {
    // The no-parallax case must be a pure cross-fade: if a zero flow moved
    // anything, the warp would be adding distortion of its own.
    const RgbaImage a = texturedRgba(64, 48);
    const RgbaImage b = texturedRgba(64, 48);

    BidirFlow flow;
    flow.forward.resize(64, 48);
    flow.backward.resize(64, 48);
    flow.ok.assign(flow.forward.u.size(), 1u);

    FlowWarpParams params;
    const auto warped = warpToMiddle(a, b, flow, params);
    REQUIRE(warped.ok());
    // Bit-exact: a zero displacement samples pixel centres, where bilinear
    // interpolation is the identity.
    CHECK(warped.value().warpedA.data == a.data);
    CHECK(warped.value().warpedB.data == b.data);
    // And with both views identical, any blend of them is that same image.
    CHECK_THAT(meanRgbDiff(warped.value().blended, a, 4), Catch::Matchers::WithinAbs(0.0, 1e-6));
}

TEST_CASE("the warp never produces a non-finite sample", "[render][warp]") {
    // A flow vector that points far outside the image must clamp, not read
    // out of bounds or produce a NaN that poisons the blend.
    const RgbaImage img = texturedRgba(48, 32);
    FlowField flow;
    flow.resize(48, 32);
    for (std::size_t i = 0; i < flow.u.size(); ++i) {
        flow.u[i] = (i % 2 == 0) ? 1.0e6f : -1.0e6f;
        flow.v[i] = 1.0e6f;
    }
    const auto warped = warpByFlow(img, flow, 1.0);
    REQUIRE(warped.ok());
    for (const float value : warped.value().data) {
        REQUIRE(std::isfinite(value));
    }
}
