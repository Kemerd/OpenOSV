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

#include "RailScene.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

using osv::testrail::legacyDisParams;
using osv::testrail::makeRailingScene;
using osv::testrail::makeRailScene;
using osv::testrail::RailScene;
using osv::testrail::RailStats;
using osv::testrail::railStats;
using osv::testrail::renderRailScene;
using osv::render::BidirFlow;
using osv::render::DisFlowParams;
using osv::render::disEpipolarRadius;
using osv::render::disFlow;
using osv::render::disFlowBidirectional;
using osv::render::FlowField;
using osv::render::GrayImage;
using osv::render::kMaxEpipolarRadius;
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

TEST_CASE("the pooled solve is bit-identical to the sequential one", "[render][flow]") {
    // The importer measures a parallax bucket on the render thread WITH a
    // pool and in its background worker WITHOUT one, and an export must not
    // depend on which of the two got there first.  That only holds if the
    // field is identical to the last bit, not merely close - so this compares
    // floats with ==, over a band-shaped image (wide and short, like the
    // overlap bands) whose motion varies across it so densify's overlapping
    // patches really do disagree and their summation order matters.
    GrayImage a = textured(1500, 120);
    GrayImage b = shifted(a, 1.7, -0.6);
    // A second, spatially varying displacement on the right half, so the
    // field is not one constant vector every order of summation agrees on.
    for (std::uint32_t y = 0; y < b.h; ++y) {
        for (std::uint32_t x = b.w / 2; x < b.w; ++x) {
            const double dx = 3.0 + 2.0 * std::sin(0.01 * static_cast<double>(x));
            b.data[static_cast<std::size_t>(y) * b.w + x] =
                a.sample(static_cast<float>(static_cast<double>(x) - dx), static_cast<float>(y) + 0.8f);
        }
    }

    DisFlowParams params;
    const auto sequential = disFlowBidirectional(a, b, params, nullptr);
    REQUIRE(sequential.ok());

    // Several pool sizes: a different thread count changes how the rows and
    // patches are chunked, which must change nothing.
    for (const unsigned threads : {2u, 7u, 32u}) {
        osv::ThreadPool pool(threads);
        const auto pooled = disFlowBidirectional(a, b, params, &pool);
        REQUIRE(pooled.ok());
        const BidirFlow& s = sequential.value();
        const BidirFlow& p = pooled.value();
        INFO("pool of " << threads << " threads");
        REQUIRE(p.forward.w == s.forward.w);
        REQUIRE(p.forward.h == s.forward.h);
        CHECK(p.forward.u == s.forward.u);
        CHECK(p.forward.v == s.forward.v);
        CHECK(p.backward.u == s.backward.u);
        CHECK(p.backward.v == s.backward.v);
        CHECK(p.ok == s.ok);
        CHECK(p.consistent == s.consistent);
    }
}

// ===========================================================================
//  The near-field additions: Tikhonov, the 1-D epipolar search, revert
// ===========================================================================

TEST_CASE("disEpipolarRadius covers the finest-level range at every searching level", "[render][flow]") {
    DisFlowParams p;  // 24 px, from level 1
    // Level 0 is below epipolarSearchMinLevel; coarser levels search the
    // same 24 finest-level pixels in their own units, rounded up.
    CHECK(disEpipolarRadius(p, 0) == 0);
    CHECK(disEpipolarRadius(p, 1) == 12);
    CHECK(disEpipolarRadius(p, 2) == 6);
    CHECK(disEpipolarRadius(p, 3) == 3);
    CHECK(disEpipolarRadius(p, 4) == 2);  // 1.5 rounds up
    CHECK(disEpipolarRadius(p, 7) == 1);  // 0.1875 rounds up: never a zero-width search
    CHECK(disEpipolarRadius(p, 8) == 0);  // past the deepest pyramid level
    CHECK(disEpipolarRadius(p, -1) == 0);

    SECTION("searching from the finest level") {
        p.epipolarSearchMinLevel = 0;
        CHECK(disEpipolarRadius(p, 0) == 24);
    }
    SECTION("a range that is not a whole number of coarse pixels") {
        p.epipolarSearchPx = 25.0;
        CHECK(disEpipolarRadius(p, 1) == 13);
    }
    SECTION("disabled or nonsense ranges search nothing") {
        for (const double px : {0.0, -4.0, std::nan(""), HUGE_VAL}) {
            INFO("epipolarSearchPx " << px);
            p.epipolarSearchPx = px;
            CHECK(disEpipolarRadius(p, 1) == 0);
            CHECK(disEpipolarRadius(p, 2) == 0);
        }
    }
    SECTION("an absurd range is clamped, not looped over") {
        p.epipolarSearchPx = 1.0e9;
        CHECK(disEpipolarRadius(p, 1) == kMaxEpipolarRadius);
    }
}

TEST_CASE("the epipolar search measures a 14 px near-field shift along the band rows", "[render][flow]") {
    // A car roof a metre from the lenses: rails and rivets offset 14 band px
    // along the meridian, sensor noise of one 8-bit code - the case the
    // 0.5.0 solver could not reach from zero on a three-level pyramid.  The
    // true flow from a to b is (0, -14) on every row both bands share.
    const std::uint32_t w = 2048;
    const std::uint32_t h = 68;
    const RailScene scene = makeRailScene(w);
    const GrayImage a = renderRailScene(scene, w, h, 0.0, 1.0, 11u);
    const GrayImage b = renderRailScene(scene, w, h, 14.0, 1.0, 29u);
    // Rows 24..57 of a: their content is inside b (rows 10..43), away from
    // the band edges and from the rows the patch grid does not cover.
    const int x0 = 32;
    const int x1 = static_cast<int>(w) - 32;
    const int y0 = 24;
    const int y1 = 58;

    const auto now = disFlowBidirectional(a, b, DisFlowParams{}, nullptr);
    REQUIRE(now.ok());
    const RailStats s = railStats(scene, now.value(), x0, x1, y0, y1);
    REQUIRE(s.pixels > 10000);
    INFO("defaults: on the rails u " << s.meanU << " v " << s.meanV << ", consistent " << 100.0 * s.consistent
                                     << " % of " << s.pixels << " px");
    CHECK_THAT(s.meanV, Catch::Matchers::WithinAbs(-14.0, 0.5));
    CHECK_THAT(s.meanU, Catch::Matchers::WithinAbs(0.0, 0.5));
    CHECK(s.consistent >= 0.80);

    SECTION("the 0.5.0 solver does not reach it, which is what the search is for") {
        const auto old = disFlowBidirectional(a, b, legacyDisParams(), nullptr);
        REQUIRE(old.ok());
        const RailStats o = railStats(scene, old.value(), x0, x1, y0, y1);
        INFO("legacy: on the rails v " << o.meanV << ", consistent " << 100.0 * o.consistent << " %");
        CHECK(std::fabs(o.meanV + 14.0) > 5.0);
    }

    SECTION("the pooled solve is bit-identical with the search active") {
        osv::ThreadPool pool(5);
        const auto pooled = disFlowBidirectional(a, b, DisFlowParams{}, &pool);
        REQUIRE(pooled.ok());
        CHECK(pooled.value().forward.u == now.value().forward.u);
        CHECK(pooled.value().forward.v == now.value().forward.v);
        CHECK(pooled.value().backward.u == now.value().backward.u);
        CHECK(pooled.value().backward.v == now.value().backward.v);
        CHECK(pooled.value().ok == now.value().ok);
    }
}

TEST_CASE("the epipolar search moves the near field and leaves the far field alone", "[render][flow]") {
    // The car body on the left half (14 px), distant scenery on the right
    // (no disparity): each half must come out as itself, so the search acts
    // per patch and not as one global shift.
    const std::uint32_t w = 2048;
    const std::uint32_t h = 68;
    const RailScene scene = makeRailScene(w);
    const GrayImage a = renderRailScene(scene, w, h, 0.0, 1.0, 11u);
    const GrayImage b = renderRailScene(scene, w, h, 14.0, 1.0, 29u, 1024u, 0.0);
    const auto f = disFlowBidirectional(a, b, DisFlowParams{}, nullptr);
    REQUIRE(f.ok());
    // 64 px clear of the boundary either side, where the halves blend.
    const RailStats nearHalf = railStats(scene, f.value(), 32, 960, 24, 58);
    const RailStats farHalf = railStats(scene, f.value(), 1088, static_cast<int>(w) - 32, 24, 58);
    INFO("near half v " << nearHalf.meanV << " (" << 100.0 * nearHalf.consistent << " % consistent), far half v "
                        << farHalf.meanV << " (" << 100.0 * farHalf.consistent << " %)");
    CHECK_THAT(nearHalf.meanV, Catch::Matchers::WithinAbs(-14.0, 0.5));
    CHECK_THAT(farHalf.meanV, Catch::Matchers::WithinAbs(0.0, 0.5));
    CHECK(nearHalf.consistent >= 0.80);
    CHECK(farHalf.consistent >= 0.80);
}

TEST_CASE("the epipolar search does not jump a railing's period", "[render][flow]") {
    // Identical bars every 10 px and nothing else, with no disparity: every
    // offset that is a multiple of the period matches as well as the truth
    // up to noise.  The ratio test and the bracket must keep (almost) every
    // patch at its seed; a patch that does jump must at least fail the
    // forward-backward check, so it never reaches a warp.  Measured: 0.2 %
    // of the interior keeps a consistent jumped vector with the ratio test,
    // 2.8 % without it (and 0 % with the 0.5.0 solver, which cannot jump).
    const std::uint32_t w = 2048;
    const std::uint32_t h = 68;
    const RailScene railing = makeRailingScene(10.0);
    const GrayImage a = renderRailScene(railing, w, h, 0.0, 1.0, 11u);
    const GrayImage b = renderRailScene(railing, w, h, 0.0, 1.0, 29u);

    const auto countJumps = [&](const DisFlowParams& p, std::uint64_t& jumped, std::uint64_t& jumpedOk,
                                std::uint64_t& pixels) {
        const auto f = disFlowBidirectional(a, b, p, nullptr);
        REQUIRE(f.ok());
        jumped = 0;
        jumpedOk = 0;
        pixels = 0;
        for (int y = 24; y < 58; ++y) {
            for (int x = 32; x < static_cast<int>(w) - 32; ++x) {
                const std::size_t idx = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x);
                const bool jump = std::fabs(f.value().forward.v[idx]) > 2.0f;
                jumped += jump ? 1u : 0u;
                jumpedOk += (jump && f.value().ok[idx] != 0) ? 1u : 0u;
                ++pixels;
            }
        }
    };

    std::uint64_t jumped = 0;
    std::uint64_t jumpedOk = 0;
    std::uint64_t pixels = 0;
    countJumps(DisFlowParams{}, jumped, jumpedOk, pixels);
    REQUIRE(pixels > 0);
    const double okFraction = static_cast<double>(jumpedOk) / static_cast<double>(pixels);
    INFO("defaults: " << jumped << " of " << pixels << " interior px a period off, " << jumpedOk
                      << " of them consistent (" << 100.0 * okFraction << " %)");
    CHECK(okFraction < 0.005);

    SECTION("Lowe's ratio is what keeps them out") {
        DisFlowParams noRatio;
        noRatio.epipolarRatio = 1.0;
        std::uint64_t jumpedNoRatio = 0;
        std::uint64_t jumpedOkNoRatio = 0;
        std::uint64_t pixelsNoRatio = 0;
        countJumps(noRatio, jumpedNoRatio, jumpedOkNoRatio, pixelsNoRatio);
        INFO("without the ratio test: " << jumpedNoRatio << " px a period off, " << jumpedOkNoRatio
                                        << " consistent; with it " << jumpedOk);
        CHECK(jumpedOkNoRatio > 4u * jumpedOk);
    }
}

TEST_CASE("a one-pixel search can only keep the seed: the winner must be bracketed", "[render][flow]") {
    // With a radius of 1 the only candidate with a scored neighbour on both
    // sides is the seed itself, so the search must never move a patch - the
    // field must equal the field without any search, bit for bit.  A minimum
    // at the end of the scanned range is a curve still falling where the scan
    // stopped (on the night clip, a diamond-plate panel pulled patches to the
    // end of the range a whole pattern period off), and is never adopted.
    const std::uint32_t w = 2048;
    const std::uint32_t h = 68;
    const RailScene scene = makeRailScene(w);
    const GrayImage a = renderRailScene(scene, w, h, 0.0, 1.0, 11u);
    const GrayImage b = renderRailScene(scene, w, h, 14.0, 1.0, 29u);
    DisFlowParams oneStep;
    oneStep.epipolarSearchPx = 2.0;  // radius 1 on levels 1 and 2
    REQUIRE(disEpipolarRadius(oneStep, 1) == 1);
    REQUIRE(disEpipolarRadius(oneStep, 2) == 1);
    DisFlowParams none = oneStep;
    none.epipolarSearchPx = 0.0;
    const auto searched = disFlowBidirectional(a, b, oneStep, nullptr);
    const auto plain = disFlowBidirectional(a, b, none, nullptr);
    REQUIRE(searched.ok());
    REQUIRE(plain.ok());
    CHECK(searched.value().forward.u == plain.value().forward.u);
    CHECK(searched.value().forward.v == plain.value().forward.v);
    CHECK(searched.value().backward.v == plain.value().backward.v);
    CHECK(searched.value().ok == plain.value().ok);
}

TEST_CASE("revertOnRunaway keeps the position the descent started from", "[render][flow]") {
    // A step scaled a thousandfold throws every patch past the displacement
    // cap on its first iteration.  Searching every level, each patch starts
    // its descent from the 1-D search's whole-pixel pick, so with revert the
    // field is those picks (the true -5.3 px, to within the half pixel a
    // whole-pixel search can miss it by); without, every patch is disowned
    // and the field is zero.
    const GrayImage a = textured(128, 96);
    const GrayImage b = shifted(a, 0.0, -5.3);
    DisFlowParams p;
    p.stepScale = 1000.0;
    p.epipolarSearchMinLevel = 0;

    SECTION("with revert: the start is kept") {
        const auto f = disFlow(a, b, p, nullptr);
        REQUIRE(f.ok());
        double mu = 0.0;
        double mv = 0.0;
        meanInterior(f.value(), 16, mu, mv);
        INFO("recovered (" << mu << ", " << mv << ") against (0, -5.3)");
        CHECK_THAT(mu, Catch::Matchers::WithinAbs(0.0, 0.1));
        CHECK_THAT(mv, Catch::Matchers::WithinAbs(-5.3, 0.5));
    }

    SECTION("without revert: every patch is disowned") {
        p.revertOnRunaway = false;
        const auto f = disFlow(a, b, p, nullptr);
        REQUIRE(f.ok());
        for (std::size_t i = 0; i < f.value().u.size(); ++i) {
            REQUIRE(f.value().u[i] == 0.0f);
            REQUIRE(f.value().v[i] == 0.0f);
        }
    }
}

TEST_CASE("no reported vector exceeds the displacement cap, revert or not", "[render][flow]") {
    // The truth (14 px) lies beyond a 6 px cap: the search must not adopt a
    // candidate past it, and revert must not keep a start past it, so no
    // patch - hence no densified, smoothed pixel - reports more than 6.
    const std::uint32_t w = 2048;
    const std::uint32_t h = 68;
    const RailScene scene = makeRailScene(w);
    const GrayImage a = renderRailScene(scene, w, h, 0.0, 1.0, 11u);
    const GrayImage b = renderRailScene(scene, w, h, 14.0, 1.0, 29u);
    for (const bool revert : {true, false}) {
        DisFlowParams p;
        p.maxDisplacementPx = 6.0;
        p.revertOnRunaway = revert;
        const auto f = disFlowBidirectional(a, b, p, nullptr);
        REQUIRE(f.ok());
        float largest = 0.0f;
        for (const FlowField* field : {&f.value().forward, &f.value().backward}) {
            for (std::size_t i = 0; i < field->u.size(); ++i) {
                largest = std::max(largest, std::max(std::fabs(field->u[i]), std::fabs(field->v[i])));
            }
        }
        INFO("revert " << revert << ": largest |component| " << largest);
        CHECK(largest <= 6.0f);
    }
}

TEST_CASE("the near-field additions are off when their parameters say so", "[render][flow]") {
    // tensorTikhonov 0, epipolarSearchPx 0 and revertOnRunaway false are the
    // 0.5.0 solver.  A negative or NaN Tikhonov term means the same as 0 (an
    // indefinite tensor is never inverted), so those must give the 0.5.0
    // field exactly too.
    const GrayImage a = textured(1500, 120);
    const GrayImage b = shifted(a, 1.7, -0.6);
    const auto legacy = disFlowBidirectional(a, b, legacyDisParams(), nullptr);
    REQUIRE(legacy.ok());
    for (const double tik : {-0.5, std::nan("")}) {
        INFO("tensorTikhonov " << tik);
        DisFlowParams p = legacyDisParams();
        p.tensorTikhonov = tik;
        const auto f = disFlowBidirectional(a, b, p, nullptr);
        REQUIRE(f.ok());
        CHECK(f.value().forward.u == legacy.value().forward.u);
        CHECK(f.value().forward.v == legacy.value().forward.v);
        CHECK(f.value().ok == legacy.value().ok);
    }
}
