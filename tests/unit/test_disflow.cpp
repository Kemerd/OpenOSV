// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_disflow.cpp - does the flow solver recover motion it was given?
//
// Every test here synthesises a pair of images whose true displacement is
// known by construction, so the assertions are against ground truth rather
// than against whatever the solver happened to produce.  That matters more
// than usual for an iterative solver: one that converges to the wrong answer
// still returns a plausible-looking field, and a test that only checked the
// field was finite and non-zero would pass for a completely broken solver.

#include "osv/render/DisFlow.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numeric>
#include <vector>

using osv::render::BidirFlow;
using osv::render::DisFlowParams;
using osv::render::disFlow;
using osv::render::disFlowBidirectional;
using osv::render::FlowField;
using osv::render::GrayImage;
using osv::render::repairFlow;
using osv::render::smoothFlow;

namespace {

/// A textured test image.
///
/// Flow needs gradient to lock onto, and it needs that gradient in BOTH axes
/// or the solve is unconstrained along the featureless direction (the classic
/// aperture problem).  Summed sinusoids at incommensurate frequencies give a
/// field that is everywhere textured and nowhere periodic enough for a patch
/// to match the wrong lobe within the search range.
GrayImage textured(std::uint32_t w, std::uint32_t h, double phase = 0.0) {
    GrayImage img;
    img.w = w;
    img.h = h;
    img.data.resize(static_cast<std::size_t>(w) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const double fx = static_cast<double>(x);
            const double fy = static_cast<double>(y);
            const double value = 0.5 + 0.18 * std::sin(0.21 * fx + phase) +
                                 0.14 * std::sin(0.13 * fy + 0.7 * phase) +
                                 0.10 * std::sin(0.071 * (fx + fy)) +
                                 0.06 * std::sin(0.037 * (fx - 1.7 * fy));
            img.data[static_cast<std::size_t>(y) * w + x] = static_cast<float>(value);
        }
    }
    return img;
}

/// Resample `src` shifted by (dx, dy): dst(p) = src(p - d), so content moves
/// BY (dx, dy) and the true flow from src to dst is exactly (dx, dy).
GrayImage shifted(const GrayImage& src, double dx, double dy) {
    GrayImage dst;
    dst.w = src.w;
    dst.h = src.h;
    dst.data.resize(src.data.size());
    for (std::uint32_t y = 0; y < src.h; ++y) {
        for (std::uint32_t x = 0; x < src.w; ++x) {
            dst.data[static_cast<std::size_t>(y) * src.w + x] =
                src.sample(static_cast<float>(static_cast<double>(x) - dx),
                           static_cast<float>(static_cast<double>(y) - dy));
        }
    }
    return dst;
}

/// Mean flow over an interior region, avoiding the border where clamp-to-edge
/// sampling makes the true displacement undefined.
void meanInterior(const FlowField& flow, int margin, double& outU, double& outV) {
    double su = 0.0;
    double sv = 0.0;
    int count = 0;
    for (int y = margin; y < static_cast<int>(flow.h) - margin; ++y) {
        for (int x = margin; x < static_cast<int>(flow.w) - margin; ++x) {
            su += flow.atU(x, y);
            sv += flow.atV(x, y);
            ++count;
        }
    }
    outU = count > 0 ? su / count : 0.0;
    outV = count > 0 ? sv / count : 0.0;
}

}  // namespace

TEST_CASE("disFlow recovers a pure translation", "[render][flow]") {
    // THE test that matters: a known shift must come back as that shift.
    const GrayImage a = textured(128, 96);

    struct Case {
        double dx;
        double dy;
    };
    // Sub-pixel, whole-pixel, both signs, and one displacement large enough
    // that only the coarse pyramid level can find it.
    const Case cases[] = {{0.0, 0.0}, {1.0, 0.0}, {0.0, -1.0}, {2.5, 1.5}, {-3.0, 2.0}, {5.0, -4.0}};

    DisFlowParams params;
    for (const Case& c : cases) {
        const GrayImage b = shifted(a, c.dx, c.dy);
        const auto flow = disFlow(a, b, params, nullptr);
        REQUIRE(flow.ok());
        REQUIRE(flow.value().valid());
        REQUIRE(flow.value().w == a.w);
        REQUIRE(flow.value().h == a.h);

        double mu = 0.0;
        double mv = 0.0;
        meanInterior(flow.value(), 16, mu, mv);
        INFO("true (" << c.dx << ", " << c.dy << ") recovered (" << mu << ", " << mv << ")");
        // Half a pixel: the densify step is a weighted average over
        // overlapping patches, so it deliberately trades a little accuracy
        // for smoothness, and a narrow band cannot do better than this.
        CHECK_THAT(mu, Catch::Matchers::WithinAbs(c.dx, 0.5));
        CHECK_THAT(mv, Catch::Matchers::WithinAbs(c.dy, 0.5));
    }
}

TEST_CASE("disFlow is robust to an exposure difference", "[render][flow]") {
    // The two fisheye lenses do not expose identically, and the overlap band
    // is where that shows.  Mean-normalised patch matching is what makes the
    // solve survive it (DJI's stitcher does the same), so a gain
    // and an offset applied to one image must not move the answer much.
    const GrayImage a = textured(128, 96);
    GrayImage b = shifted(a, 2.0, -1.0);
    for (float& value : b.data) {
        value = value * 1.30f + 0.08f;  // +30% gain, +8% lift
    }

    DisFlowParams params;
    const auto flow = disFlow(a, b, params, nullptr);
    REQUIRE(flow.ok());
    double mu = 0.0;
    double mv = 0.0;
    meanInterior(flow.value(), 16, mu, mv);
    INFO("recovered (" << mu << ", " << mv << ") against a 1.3x + 0.08 exposure change");
    CHECK_THAT(mu, Catch::Matchers::WithinAbs(2.0, 0.6));
    CHECK_THAT(mv, Catch::Matchers::WithinAbs(-1.0, 0.6));
}

TEST_CASE("disFlow reports no motion on a featureless image", "[render][flow]") {
    // Flat grey has no gradient, so every structure tensor is singular and
    // every patch must be rejected.  The REQUIRED behaviour is a zero field,
    // not a plausible-looking one: inventing motion here is what tears a
    // warp across an empty sky, which is most of this application's input.
    GrayImage flat;
    flat.w = 64;
    flat.h = 64;
    flat.data.assign(static_cast<std::size_t>(64) * 64, 0.5f);

    DisFlowParams params;
    const auto flow = disFlow(flat, flat, params, nullptr);
    REQUIRE(flow.ok());
    REQUIRE(flow.value().valid());
    for (std::size_t i = 0; i < flow.value().u.size(); ++i) {
        REQUIRE(flow.value().u[i] == 0.0f);
        REQUIRE(flow.value().v[i] == 0.0f);
    }
}

TEST_CASE("disFlow never returns a non-finite vector", "[render][flow]") {
    // A field with one NaN in it poisons every stage downstream, and the
    // warp would read it as a coordinate.  The solve has several divisions
    // (the structure tensor inverse above all), so this is asserted rather
    // than assumed.
    const GrayImage a = textured(96, 64);
    const GrayImage b = shifted(a, 1.5, -2.5);

    DisFlowParams params;
    params.iterations = 30;      // more chances to diverge
    params.minTensorDet = 1e-12; // accept nearly-singular patches too
    const auto flow = disFlow(a, b, params, nullptr);
    REQUIRE(flow.ok());
    for (std::size_t i = 0; i < flow.value().u.size(); ++i) {
        REQUIRE(std::isfinite(flow.value().u[i]));
        REQUIRE(std::isfinite(flow.value().v[i]));
    }
}

TEST_CASE("disFlow rejects unusable inputs", "[render][flow]") {
    const GrayImage good = textured(64, 64);
    DisFlowParams params;

    SECTION("an empty image") {
        const GrayImage empty;
        CHECK_FALSE(disFlow(empty, good, params, nullptr).ok());
        CHECK_FALSE(disFlow(good, empty, params, nullptr).ok());
    }

    SECTION("mismatched sizes") {
        const GrayImage other = textured(48, 64);
        CHECK_FALSE(disFlow(good, other, params, nullptr).ok());
    }

    SECTION("a malformed image whose data does not match its size") {
        GrayImage bad = good;
        bad.data.pop_back();
        CHECK_FALSE(bad.valid());
        CHECK_FALSE(disFlow(bad, good, params, nullptr).ok());
    }

    SECTION("parameters that describe no solvable problem") {
        DisFlowParams bad = params;
        bad.patchSize = 1;
        CHECK_FALSE(disFlow(good, good, bad, nullptr).ok());
        bad = params;
        bad.iterations = 0;
        CHECK_FALSE(disFlow(good, good, bad, nullptr).ok());
    }
}

TEST_CASE("disFlowBidirectional agrees with itself on a clean pair", "[render][flow]") {
    // The forward and backward fields of a pure translation must be
    // negatives of one another, so nearly every pixel should pass the
    // consistency test.  A solver that passed the one-directional test but
    // failed this one would be returning a field that happens to average
    // correctly while disagreeing pixel by pixel.
    const GrayImage a = textured(128, 96);
    const GrayImage b = shifted(a, 2.0, 1.0);

    DisFlowParams params;
    const auto bidir = disFlowBidirectional(a, b, params, nullptr);
    REQUIRE(bidir.ok());
    const BidirFlow& f = bidir.value();
    REQUIRE(f.valid());

    // Count agreement in the interior, where the true flow is defined.
    std::uint64_t interior = 0;
    std::uint64_t consistent = 0;
    for (int y = 16; y < static_cast<int>(f.forward.h) - 16; ++y) {
        for (int x = 16; x < static_cast<int>(f.forward.w) - 16; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * f.forward.w + static_cast<std::size_t>(x);
            ++interior;
            if (f.ok[idx] != 0) {
                ++consistent;
            }
        }
    }
    REQUIRE(interior > 0);
    const double fraction = static_cast<double>(consistent) / static_cast<double>(interior);
    INFO("forward-backward consistent over " << 100.0 * fraction << " % of the interior");
    CHECK(fraction > 0.90);

    // And the two directions really are opposite.
    double fu = 0.0;
    double fv = 0.0;
    double bu = 0.0;
    double bv = 0.0;
    meanInterior(f.forward, 16, fu, fv);
    meanInterior(f.backward, 16, bu, bv);
    CHECK_THAT(fu + bu, Catch::Matchers::WithinAbs(0.0, 0.5));
    CHECK_THAT(fv + bv, Catch::Matchers::WithinAbs(0.0, 0.5));
}

TEST_CASE("the consistency check rejects an impossible pair", "[render][flow]") {
    // Two unrelated images have no true correspondence, so the forward and
    // backward solves have no reason to agree.  This is the test that the
    // mask means something: if it passed everything, it would be useless as
    // the signal for where a warp may be trusted.
    const GrayImage a = textured(96, 96, 0.0);
    const GrayImage b = textured(96, 96, 2.4);  // different phase entirely

    DisFlowParams params;
    params.consistencyTolPx = 0.25;  // strict, since the point is rejection
    const auto bidir = disFlowBidirectional(a, b, params, nullptr);
    REQUIRE(bidir.ok());
    const double fraction =
        static_cast<double>(bidir.value().consistent) / static_cast<double>(bidir.value().ok.size());
    INFO("consistent fraction on an unrelated pair: " << 100.0 * fraction << " %");
    CHECK(fraction < 0.80);
}

TEST_CASE("smoothFlow blurs without moving the mean", "[render][flow]") {
    // A Gaussian is normalised, so it must preserve the average exactly (to
    // rounding).  Checking that catches a mis-normalised kernel, which would
    // otherwise show up only as a subtly wrong warp magnitude.
    FlowField flow;
    flow.resize(64, 48);
    for (std::uint32_t y = 0; y < flow.h; ++y) {
        for (std::uint32_t x = 0; x < flow.w; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * flow.w + x;
            // Signed arithmetic on purpose: x and y are unsigned, so
            // (x % 7) - 3 would wrap to about 4e9 instead of going negative.
            flow.u[idx] = static_cast<float>(static_cast<int>(x % 7) - 3);
            flow.v[idx] = static_cast<float>(static_cast<int>(y % 5) - 2);
        }
    }
    const double beforeU =
        std::accumulate(flow.u.begin(), flow.u.end(), 0.0) / static_cast<double>(flow.u.size());
    const double beforeV =
        std::accumulate(flow.v.begin(), flow.v.end(), 0.0) / static_cast<double>(flow.v.size());

    smoothFlow(flow, 2.0);

    const double afterU =
        std::accumulate(flow.u.begin(), flow.u.end(), 0.0) / static_cast<double>(flow.u.size());
    const double afterV =
        std::accumulate(flow.v.begin(), flow.v.end(), 0.0) / static_cast<double>(flow.v.size());
    // Clamp-to-edge replicates the border, which shifts the mean slightly;
    // the tolerance covers that and nothing more.
    CHECK_THAT(afterU, Catch::Matchers::WithinAbs(beforeU, 0.25));
    CHECK_THAT(afterV, Catch::Matchers::WithinAbs(beforeV, 0.25));

    SECTION("a non-positive sigma is a no-op") {
        FlowField copy = flow;
        smoothFlow(copy, 0.0);
        CHECK(copy.u == flow.u);
        smoothFlow(copy, -1.0);
        CHECK(copy.u == flow.u);
    }
}

TEST_CASE("repairFlow fills holes from measured neighbours", "[render][flow]") {
    FlowField flow;
    flow.resize(32, 32);
    std::vector<std::uint8_t> ok(flow.u.size(), 1u);
    // A uniform field with a square hole punched in the middle.
    for (float& value : flow.u) {
        value = 3.0f;
    }
    for (float& value : flow.v) {
        value = -2.0f;
    }
    for (int y = 12; y < 20; ++y) {
        for (int x = 12; x < 20; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * flow.w + static_cast<std::size_t>(x);
            ok[idx] = 0u;
            flow.u[idx] = 0.0f;  // the "zero is a claim" case
            flow.v[idx] = 0.0f;
        }
    }

    const std::uint64_t unfilled = repairFlow(flow, ok);
    CHECK(unfilled == 0);
    // The hole must now carry its neighbours' value, not zero.
    for (int y = 12; y < 20; ++y) {
        for (int x = 12; x < 20; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * flow.w + static_cast<std::size_t>(x);
            INFO("hole pixel " << x << "," << y);
            CHECK_THAT(flow.u[idx], Catch::Matchers::WithinAbs(3.0, 1e-4));
            CHECK_THAT(flow.v[idx], Catch::Matchers::WithinAbs(-2.0, 1e-4));
        }
    }

    SECTION("an all-invalid field cannot be repaired and says so") {
        FlowField empty;
        empty.resize(8, 8);
        const std::vector<std::uint8_t> none(empty.u.size(), 0u);
        CHECK(repairFlow(empty, none) == empty.u.size());
    }

    SECTION("a mask of the wrong length is refused") {
        FlowField f;
        f.resize(8, 8);
        const std::vector<std::uint8_t> wrong(3, 1u);
        CHECK(repairFlow(f, wrong) == 0);
    }
}
