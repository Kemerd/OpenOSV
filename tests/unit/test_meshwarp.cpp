// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Mesh warp tests: the one-field seam correction (MeshWarp.h).
//
// What is pinned here, and why each one matters:
//
//   * A KNOWN DISPARITY IS RECOVERED AND A LINE STAYS STRAIGHT.  Synthetic
//     bands of one procedural texture, lens 1 seeing it displaced by a known
//     field, with an exact synthetic flow - and a local WRONG measurement
//     (a bump) under a detected line, the shape of the per-column guard's
//     kink.  The solve must return the disparity elsewhere and keep the line
//     straight across the bump (< 0.2 px); without the line term it must not.
//
//   * NOTHING MEASURED IS THE PRIOR, BIT FOR BIT.  No flow, no line, no
//     previous mesh - or flow on a band without structure - gives the seam
//     table's lift exactly: one field, one code path, also on fog.
//
//   * TIME.  Identical input twice gives identical output; with the previous
//     mesh as the prior a static input does not move it; a real step change
//     is followed within a few solves.
//
//   * THE RING WRAPS, the gate's counts are gridFromFlow's, the thread count
//     changes nothing, malformed input is refused, and the line detector
//     finds a straight edge but never a coverage edge.

#include <catch2/catch_test_macros.hpp>

#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/render/MeshWarp.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/osv_kernel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace osv;

namespace {

// ---------------------------------------------------------------------------
//  The synthetic band: the geometry renderLensBands draws at the defaults
// ---------------------------------------------------------------------------

constexpr std::uint32_t kW = 2048;     ///< Band width = polar map width.
constexpr std::uint32_t kMapH = 1024;  ///< Polar map height.
constexpr std::uint32_t kRow0 = 478;   ///< First map row of a +/- 6 degree band.
constexpr std::uint32_t kH = 68;       ///< Band rows.

/// Radians per band pixel (both directions on the 2:1 map).
constexpr double kRadPerPx = kTwoPi / static_cast<double>(kW);

/// Longitude / latitude of a band pixel's centre.
double lonOfCol(double c) { return (c + 0.5) * kRadPerPx - kPi; }
double latOfRow(double r) { return kHalfPi - (static_cast<double>(kRow0) + r + 0.5) * (kPi / kMapH); }

/// A procedural texture in [0.25, 0.75], periodic round the ring (whole
/// numbers of periods in x), with luma gradients of several code steps per
/// pixel almost everywhere - structured in the gate's sense.  Its vertical
/// periods (15-22 px) stay clear of the 4 and 8 px offsets the benefit-gate
/// test compares: a texture that repeats every ~8 px looks ALIGNED at an
/// 8 px error, which no real scene does and which would fool any
/// photometric judge.
double texture(double x, double y) {
    const double u = kTwoPi * x / static_cast<double>(kW);
    return 0.5 + 0.08 * std::sin(157.0 * u + 0.9 * std::sin(0.37 * y)) + 0.07 * std::sin(229.0 * u + 0.41 * y + 1.1) +
           0.05 * std::sin(0.29 * y + 31.0 * u);
}

/// The disparity of a synthetic pair: lens 0's content at p shows in lens 1
/// at p + f(p), f in band pixels (column east, row SOUTH).
struct Disparity {
    double fu = 1.0;          ///< Constant column part.
    double fv = 4.0;          ///< Constant row part.
    double fvWave = 0.0;      ///< Amplitude of a row part varying once round the ring.
    [[nodiscard]] double u(double /*x*/, double /*y*/) const { return fu; }
    [[nodiscard]] double v(double x, double /*y*/) const {
        return fv + fvWave * std::sin(kTwoPi * x / static_cast<double>(kW) + 0.4);
    }
};

/// Bands of `d` (lens 1 = texture displaced by d) and their exact flow.
/// `bumpRows` > 0 adds `bump` band rows to the MEASURED row flow (not to
/// the pictures) for columns [bumpC0, bumpC1): a wrong measurement.
struct Synthetic {
    render::LensBands bands;
    render::BidirFlow flow;
};

Synthetic makeSynthetic(const Disparity& d, double bump = 0.0, int bumpC0 = 0, int bumpC1 = 0, bool flat = false) {
    Synthetic s;
    s.bands.w = kW;
    s.bands.h = kH;
    s.bands.rowOffset = kRow0;
    s.bands.mapH = kMapH;
    const std::size_t n = static_cast<std::size_t>(kW) * kH;
    for (int lens = 0; lens < 2; ++lens) {
        s.bands.luma[lens].assign(n, 0.5f);
        s.bands.alpha[lens].assign(n, 1.0f);
    }
    s.flow.forward.resize(kW, kH);
    s.flow.backward.resize(kW, kH);
    s.flow.ok.assign(n, 1u);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * kW + x;
            const double px = static_cast<double>(x) + 0.5;
            const double py = static_cast<double>(y) + 0.5;
            const double fu = d.u(px, py);
            const double fv = d.v(px, py);
            if (!flat) {
                s.bands.luma[0][i] = static_cast<float>(texture(px, py));
                s.bands.luma[1][i] = static_cast<float>(texture(px - fu, py - fv));
            }
            double mv = fv;
            if (bump != 0.0 && static_cast<int>(x) >= bumpC0 && static_cast<int>(x) < bumpC1) {
                mv += bump;  // the measurement is wrong here; the pictures are not
            }
            s.flow.forward.u[i] = static_cast<float>(fu);
            s.flow.forward.v[i] = static_cast<float>(mv);
            s.flow.backward.u[i] = static_cast<float>(-fu);
            s.flow.backward.v[i] = static_cast<float>(-mv);
        }
    }
    s.flow.consistent = n;
    return s;
}

/// A straight segment of `lens` between two band pixel positions.
render::SeamLine segment(int lens, double x0, double y0, double x1, double y1) {
    render::SeamLine l;
    l.lens = lens;
    l.lon0Rad = lonOfCol(x0 - 0.5);
    l.lat0Rad = latOfRow(y0 - 0.5);
    l.lon1Rad = lonOfCol(x1 - 0.5);
    l.lat1Rad = latOfRow(y1 - 0.5);
    l.lengthPx = std::hypot(x1 - x0, y1 - y0);
    return l;
}

/// The field a grid gives at a band pixel centre, band pixels (east, north),
/// through the kernel's own fetch.
void fieldAtPixel(const render::ParallaxWarpGrid& g, double x, double y, double& dLon, double& dLat) {
    OsvRenderParams kp{};
    kp.warpEnabled = 1;
    kp.warpW = static_cast<int>(g.w);
    kp.warpH = static_cast<int>(g.h);
    kp.warpLatMinRad = g.latMinRad;
    kp.warpLatMaxRad = g.latMaxRad;
    const float lon = static_cast<float>(lonOfCol(x));
    const float lat = static_cast<float>(latOfRow(y));
    dLon = static_cast<double>(osvWarpSample(&kp, g.uv.data(), lon, lat, 0)) / kRadPerPx;
    dLat = static_cast<double>(osvWarpSample(&kp, g.uv.data(), lon, lat, 1)) / kRadPerPx;
}

/// Mesh parameters for the synthetic band (the defaults; the band is the
/// default analysis band).
render::MeshWarpParams testParams() {
    render::MeshWarpParams p;
    p.parallax.band.equirectW = kW;
    p.parallax.band.bandHalfDeg = 6.0;
    return p;
}

/// The worst |error| of a grid against the half disparity of `d` over the
/// band's middle rows, skipping columns [skip0, skip1).
double worstError(const render::ParallaxWarpGrid& g, const Disparity& d, int skip0, int skip1) {
    double worst = 0.0;
    for (std::uint32_t y = 14; y < kH - 14; y += 4) {
        for (std::uint32_t x = 0; x < kW; x += 8) {
            if (static_cast<int>(x) >= skip0 && static_cast<int>(x) < skip1) {
                continue;
            }
            double dl = 0.0;
            double dt = 0.0;
            fieldAtPixel(g, x, y, dl, dt);
            const double px = static_cast<double>(x) + 0.5;
            const double py = static_cast<double>(y) + 0.5;
            // Master displacement = half the disparity; a row step is south.
            worst = std::max(worst, std::fabs(dl - 0.5 * d.u(px, py)));
            worst = std::max(worst, std::fabs(dt - (-0.5 * d.v(px, py))));
        }
    }
    return worst;
}

}  // namespace

// ===========================================================================
//  Layout, parameters and the table lift
// ===========================================================================

TEST_CASE("mesh warp: the layout is the kernel grid the mesh is", "[render][mesh]") {
    const render::MeshWarpParams p = testParams();
    auto layout = render::meshWarpLayout(p);
    REQUIRE(layout.ok());
    const render::ParallaxWarpGrid& g = layout.value();
    CHECK(g.w == 128u);
    CHECK(g.h == 13u);  // +/- 12 degrees in 2-degree rows
    CHECK(std::fabs(static_cast<double>(g.latMinRad) - deg2rad(12.0)) < 1e-6);
    CHECK(std::fabs(static_cast<double>(g.latMaxRad) + deg2rad(12.0)) < 1e-6);
    CHECK(g.valid());
    CHECK(std::all_of(g.uv.begin(), g.uv.end(), [](float v) { return v == 0.0f; }));
}

TEST_CASE("mesh warp: malformed parameters are refused", "[render][mesh]") {
    const auto refused = [](const render::MeshWarpParams& p) { return !render::checkMeshWarpParams(p).ok(); };
    render::MeshWarpParams p = testParams();
    CHECK(render::checkMeshWarpParams(p).ok());
    p.meshCols = 8;
    CHECK(refused(p));
    p = testParams();
    p.rowSpacingDeg = 5.0;  // 24 / 5 is not a whole number of rows
    CHECK(refused(p));
    p = testParams();
    p.reachDeg = 7.0;  // the +/- 6 degree band would reach the pinned edge
    CHECK(refused(p));
    p = testParams();
    p.anchorWeight = 0.0;  // the anchor keeps the system positive definite
    CHECK(refused(p));
    p = testParams();
    p.lineWeight = std::numeric_limits<double>::quiet_NaN();
    CHECK(refused(p));
    p = testParams();
    p.irlsIterations = 0;
    CHECK(refused(p));
    p = testParams();
    p.parallax.band.equirectW = 16;
    CHECK(refused(p));
}

TEST_CASE("mesh warp: the seam table's lift is the 1-D shift along the meridian", "[render][mesh]") {
    const render::MeshWarpParams p = testParams();
    // A constant table of +2 degrees: half of it, away from the master's
    // axis (the +90 pole) - i.e. -1 degree of master latitude - in full
    // within 6 degrees of the seam, tapered by the kernel's own taper beyond.
    const std::vector<float> table(2048, 2.0f);
    auto lift = render::liftSeamTable(table, p);
    REQUIRE(lift.ok());
    const render::ParallaxWarpGrid& g = lift.value();
    for (std::uint32_t j = 0; j < g.h; ++j) {
        const double lat = static_cast<double>(g.latMinRad) +
                           (static_cast<double>(g.latMaxRad) - static_cast<double>(g.latMinRad)) * j / (g.h - 1.0);
        const bool edge = j == 0 || j + 1 == g.h;
        const double taper = edge ? 0.0 : static_cast<double>(osvSeamShiftTaper(static_cast<float>(std::sin(lat))));
        for (std::uint32_t i = 0; i < g.w; i += 17) {
            const std::size_t k = (static_cast<std::size_t>(j) * g.w + i) * 2u;
            CHECK(g.uv[k] == 0.0f);  // no cross-meridian part
            CHECK(std::fabs(static_cast<double>(g.uv[k + 1]) - (-deg2rad(1.0) * taper)) < 1e-7);
        }
    }
    // Not-a-number columns shift nothing; an empty table is the zero field.
    auto nan = render::liftSeamTable(std::vector<float>(2048, std::numeric_limits<float>::quiet_NaN()), p);
    REQUIRE(nan.ok());
    CHECK(std::all_of(nan.value().uv.begin(), nan.value().uv.end(), [](float v) { return v == 0.0f; }));
    auto none = render::liftSeamTable({}, p);
    REQUIRE(none.ok());
    CHECK(std::all_of(none.value().uv.begin(), none.value().uv.end(), [](float v) { return v == 0.0f; }));
}

// ===========================================================================
//  Nothing measured: the prior, exactly
// ===========================================================================

TEST_CASE("mesh warp: with nothing measured the field is the table's lift, bit for bit", "[render][mesh]") {
    const render::MeshWarpParams p = testParams();
    std::vector<float> table(2048);
    for (std::size_t c = 0; c < table.size(); ++c) {
        table[c] = static_cast<float>(1.5 * std::sin(kTwoPi * static_cast<double>(c) / 2048.0 * 3.0));
    }
    auto lift = render::liftSeamTable(table, p);
    REQUIRE(lift.ok());

    SECTION("no flow, no line, no previous mesh") {
        render::MeshWarpInputs in;
        in.prior = &lift.value();
        auto r = render::solveMeshWarp(in, p);
        REQUIRE(r.ok());
        CHECK(r.value().report.mode == render::MeshWarpMode::PriorOnly);
        CHECK(r.value().grid.uv == lift.value().uv);
    }
    SECTION("flow on a band without structure (fog): the structured gate weighs the data to nothing") {
        const Synthetic s = makeSynthetic(Disparity{}, 0.0, 0, 0, true);
        render::MeshWarpInputs in;
        in.bands = &s.bands;
        in.flow = &s.flow;
        in.prior = &lift.value();
        auto r = render::solveMeshWarp(in, p);
        REQUIRE(r.ok());
        CHECK(r.value().report.strength == 0.0);
        CHECK(r.value().report.mode == render::MeshWarpMode::PriorOnly);
        CHECK(r.value().grid.uv == lift.value().uv);
        CHECK(r.value().grid.structuredPixels == 0u);
    }
}

// ===========================================================================
//  Recovery and straightness
// ===========================================================================

TEST_CASE("mesh warp: a known disparity is recovered and a line stays straight across a wrong measurement",
          "[render][mesh]") {
    render::MeshWarpParams p = testParams();
    // The benefit gate would judge the wrong bump by its effect and drop it
    // (its own test below); off here, the line term alone must hold the line.
    p.parallax.requiredImprovement = 0.0;
    // The synthetic band is ALL structure; the real ones measured are 3-10 %
    // structured (the data density the weights are calibrated on), so the
    // data weigh as they would there.
    p.alignWeight = 0.05;
    const Disparity d;  // 1 px across, 4 px along the meridian (half: 0.5 / 2 px)
    const Synthetic s = makeSynthetic(d, 3.0, 380, 420);
    // Lines through the bump (one per lens) and an oblique one elsewhere.
    const std::vector<render::SeamLine> lines = {segment(1, 250.0, 34.0, 550.0, 34.0),
                                                 segment(0, 300.0, 22.0, 500.0, 22.0),
                                                 segment(1, 1000.0, 10.0, 1200.0, 58.0)};
    render::MeshWarpInputs in;
    in.bands = &s.bands;
    in.flow = &s.flow;
    in.lines = &lines;
    auto r = render::solveMeshWarp(in, p);
    REQUIRE(r.ok());
    const render::MeshWarpResult& m = r.value();
    CHECK(m.report.mode == render::MeshWarpMode::Solved);
    CHECK(m.report.strength == 1.0);
    CHECK(m.report.lines == 3u);
    // The disparity, away from the bump and from the lines over it: the line
    // term spreads a wrong bump under a line along the line (affine) rather
    // than leaving it a bump - that is its purpose - so the lines' columns
    // (250-550) and the smoothing reach beyond them are not "away".
    const double err = worstError(m.grid, d, 200, 600);
    INFO("worst disparity error away from the bump: " << err << " px");
    CHECK(err < 0.1);
    // The lines, warped as the kernel moves them: straight to < 0.2 px RMS,
    // although the measurement under them is 1.5 px of half disparity wrong
    // over 40 columns (the worst single sample stays under 0.4 px).
    auto straight = render::measureLineStraightness(lines, kW, &m.grid, nullptr);
    REQUIRE(straight.ok());
    INFO("with the line term: RMS " << straight.value().rmsPx << " px, max " << straight.value().maxPx << " px");
    CHECK(straight.value().lines == 3u);
    CHECK(straight.value().rmsPx < 0.2);
    CHECK(straight.value().maxPx < 0.4);

    // Without the line term the bump bends the line: the term is what holds it.
    p.lineWeight = 0.0;
    auto bent = render::solveMeshWarp(in, p);
    REQUIRE(bent.ok());
    auto bentStraight = render::measureLineStraightness(lines, kW, &bent.value().grid, nullptr);
    REQUIRE(bentStraight.ok());
    INFO("without the line term: RMS " << bentStraight.value().rmsPx << " px");
    CHECK(bentStraight.value().rmsPx > 2.0 * straight.value().rmsPx);
    CHECK(bentStraight.value().rmsPx > 0.2);
}

TEST_CASE("mesh warp: the benefit gate drops a correction the pictures do not support", "[render][mesh]") {
    render::MeshWarpParams p = testParams();
    p.lineWeight = 0.0;  // no lines: the gate alone
    // Only along the meridian, so the gate judges the wrong part alone: it
    // weighs a correction against NONE, and a measurement that fixes one
    // component while it breaks the other can still beat none (it does
    // with a 1 px cross-meridian part here - the old grid's gate the same).
    Disparity d;
    d.fu = 0.0;
    // Over 96 columns the flow measures 8 px more than there is: the field
    // would sample 8 px apart where nothing (no correction) is 4 px apart.
    // The region covers whole mesh cells (16 band columns each, 1008-1104):
    // the gate judges cells, and a cell half over a wrong region keeps its
    // wrong half's data - the shape term then carries it into the gated
    // cells (measured: 8 px of overshoot there with the bending term alone,
    // the reason shapeMembrane is not smaller).
    const Synthetic s = makeSynthetic(d, 8.0, 1008, 1104);
    render::MeshWarpInputs in;
    in.bands = &s.bands;
    in.flow = &s.flow;
    // Error inside the bump, gate on (the defaults) and off.
    const auto bumpError = [&](const render::ParallaxWarpGrid& g) {
        double worst = 0.0;
        for (std::uint32_t y = 20; y < kH - 20; y += 4) {
            for (std::uint32_t x = 1036; x < 1076; x += 4) {
                double dl = 0.0;
                double dt = 0.0;
                fieldAtPixel(g, x, y, dl, dt);
                worst = std::max(worst, std::fabs(dt - (-0.5 * d.v(x + 0.5, y + 0.5))));
            }
        }
        return worst;
    };
    auto gated = render::solveMeshWarp(in, p);
    REQUIRE(gated.ok());
    p.parallax.requiredImprovement = 0.0;
    auto ungated = render::solveMeshWarp(in, p);
    REQUIRE(ungated.ok());
    const double eg = bumpError(gated.value().grid);
    const double eu = bumpError(ungated.value().grid);
    INFO("bump error with the gate " << eg << " px, without " << eu << " px");
    CHECK(gated.value().report.benefitGatedCells > 0u);
    CHECK(eu > 3.0);       // the wrong 4 px of half disparity are followed
    CHECK(eg < 0.5 * eu);  // the gate holds the field near the truth
}

// ===========================================================================
//  Time
// ===========================================================================

TEST_CASE("mesh warp: time - identical input is identical output, a static scene holds, a step is followed",
          "[render][mesh][temporal]") {
    const render::MeshWarpParams p = testParams();
    const Disparity a;  // half disparity along the meridian: 2 px
    const Synthetic sa = makeSynthetic(a);
    render::MeshWarpInputs in;
    in.bands = &sa.bands;
    in.flow = &sa.flow;
    auto first = render::solveMeshWarp(in, p);
    auto second = render::solveMeshWarp(in, p);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    CHECK(first.value().grid.uv == second.value().grid.uv);

    // The same input with its own result as the previous mesh: it stays put.
    in.previous = &first.value().grid;
    auto held = render::solveMeshWarp(in, p);
    REQUIRE(held.ok());
    CHECK(held.value().report.temporal);
    auto moved = render::meanAbsGridChangeDeg(held.value().grid, first.value().grid, 6.0);
    REQUIRE(moved.ok());
    INFO("static scene: mean change " << moved.value() << " deg");
    CHECK(moved.value() < 1e-5);

    // A step: the scene now asks for 5 px of half disparity (3 px more).
    Disparity b;
    b.fv = 10.0;
    const Synthetic sb = makeSynthetic(b);
    render::MeshWarpInputs inB;
    inB.bands = &sb.bands;
    inB.flow = &sb.flow;
    render::ParallaxWarpGrid prev = first.value().grid;
    std::vector<double> errors;
    for (int k = 0; k < 4; ++k) {
        inB.previous = &prev;
        auto step = render::solveMeshWarp(inB, p);
        REQUIRE(step.ok());
        errors.push_back(worstError(step.value().grid, b, 0, 0));
        prev = step.value().grid;
    }
    INFO("step errors over four solves: " << errors[0] << " " << errors[1] << " " << errors[2] << " " << errors[3]);
    CHECK(errors[0] < 3.0);         // it moved
    CHECK(errors[1] < errors[0]);   // and keeps converging
    CHECK(errors[1] < 0.75);        // within a quarter of the step after two solves
    CHECK(errors[3] < 0.2);         // and essentially there after four
}

TEST_CASE("mesh warp: the field alone is the solve without the previous mesh, bit for bit",
          "[render][mesh][temporal]") {
    // The plug-ins take bucket b's temporal prior from bucket b - 1 solved
    // ALONE (MeshWarpResult::alone), so a field depends on two anchors only.
    // That is only true if `alone` is exactly what a solve without the
    // previous mesh returns - checked here on a field that a previous mesh
    // really pulls (a different scene before), with a line in the line term.
    const render::MeshWarpParams p = testParams();
    Disparity d;
    d.fvWave = 1.0;
    const Synthetic s = makeSynthetic(d, 2.0, 600, 640);
    const std::vector<render::SeamLine> lines = {segment(1, 500.0, 30.0, 760.0, 34.0)};
    render::MeshWarpInputs in;
    in.bands = &s.bands;
    in.flow = &s.flow;
    in.lines = &lines;

    // The reference: no previous mesh at all.
    auto reference = render::solveMeshWarp(in, p);
    REQUIRE(reference.ok());

    // Without a previous mesh, `alone` is the field itself.
    in.solveAlone = true;
    auto self = render::solveMeshWarp(in, p);
    REQUIRE(self.ok());
    REQUIRE(self.value().alone.has_value());
    CHECK(self.value().alone->uv == self.value().grid.uv);
    CHECK(self.value().grid.uv == reference.value().grid.uv);

    // With a previous mesh from another scene: the field moves toward it,
    // `alone` does not.
    Disparity other;
    other.fv = 9.0;
    const Synthetic so = makeSynthetic(other);
    render::MeshWarpInputs inOther;
    inOther.bands = &so.bands;
    inOther.flow = &so.flow;
    auto previous = render::solveMeshWarp(inOther, p);
    REQUIRE(previous.ok());
    in.previous = &previous.value().grid;
    ThreadPool pool(4);
    auto pulled = render::solveMeshWarp(in, p, &pool);
    REQUIRE(pulled.ok());
    REQUIRE(pulled.value().alone.has_value());
    CHECK(pulled.value().report.temporal);
    CHECK(pulled.value().report.aloneMs > 0.0);
    CHECK(pulled.value().alone->uv == reference.value().grid.uv);   // bit for bit (no pool there, a pool here)
    CHECK(pulled.value().grid.uv != reference.value().grid.uv);     // the previous mesh did pull the field
    auto moved = render::meanAbsGridChangeDeg(pulled.value().grid, reference.value().grid, 6.0);
    REQUIRE(moved.ok());
    INFO("the temporal prior moved the field by " << moved.value() << " deg on average");
    CHECK(moved.value() > 1e-4);

    // Nothing measured, nothing to keep straight, no previous mesh: the prior,
    // and `alone` with it.
    render::MeshWarpInputs empty;
    empty.solveAlone = true;
    auto prior = render::solveMeshWarp(empty, p);
    REQUIRE(prior.ok());
    CHECK(prior.value().report.mode == render::MeshWarpMode::PriorOnly);
    REQUIRE(prior.value().alone.has_value());
    CHECK(prior.value().alone->uv == prior.value().grid.uv);

    // A previous mesh but nothing measured and no line (open sky, fog): the
    // call is a solve (the previous mesh takes part), but the field ALONE is
    // exactly what the call without it returns - the prior, bit for bit,
    // never a Cholesky round trip of it that leaves rounding where the prior
    // is 0.  Otherwise the next bucket's temporal prior would depend on
    // whether this bucket was measured with a prior of its own (a cold
    // landing) or without (a sequential render).
    std::vector<float> table(512, 0.0f);
    for (std::size_t c = 100; c < 300; ++c) {
        table[c] = 1.25f;  // a 1.25 degree shift over part of the ring, 0 elsewhere
    }
    auto lift = render::liftSeamTable(table, p);
    REQUIRE(lift.ok());
    render::MeshWarpInputs skyOnly;
    skyOnly.prior = &lift.value();
    skyOnly.solveAlone = true;
    auto skyReference = render::solveMeshWarp(skyOnly, p);
    REQUIRE(skyReference.ok());
    CHECK(skyReference.value().report.mode == render::MeshWarpMode::PriorOnly);
    CHECK(skyReference.value().grid.uv == lift.value().uv);  // the lift's edge rows are already zero
    skyOnly.previous = &previous.value().grid;
    auto skyPulled = render::solveMeshWarp(skyOnly, p, &pool);
    REQUIRE(skyPulled.ok());
    CHECK(skyPulled.value().report.temporal);
    CHECK(skyPulled.value().report.mode == render::MeshWarpMode::Solved);
    REQUIRE(skyPulled.value().alone.has_value());
    CHECK(skyPulled.value().alone->uv == skyReference.value().grid.uv);  // bit for bit
}

// ===========================================================================
//  The ring, the counts, the threads, malformed input
// ===========================================================================

TEST_CASE("mesh warp: the field is continuous across the ring's wrap, a line across it stays straight",
          "[render][mesh]") {
    const render::MeshWarpParams p = testParams();
    Disparity d;
    d.fvWave = 1.5;  // varies once round the ring: continuous at +/-180 degrees
    const Synthetic s = makeSynthetic(d);
    // A segment across the wrap meridian (unwrapped end beyond +pi).
    std::vector<render::SeamLine> lines = {segment(1, 1950.0, 30.0, 2150.0, 30.0)};
    render::MeshWarpInputs in;
    in.bands = &s.bands;
    in.flow = &s.flow;
    in.lines = &lines;
    auto r = render::solveMeshWarp(in, p);
    REQUIRE(r.ok());
    const render::ParallaxWarpGrid& g = r.value().grid;
    // Neighbouring columns across the wrap differ no more than any others.
    const std::uint32_t row = g.h / 2u;
    const auto lat = [&](std::uint32_t c) { return g.uv[(static_cast<std::size_t>(row) * g.w + c) * 2u + 1u]; };
    double maxStep = 0.0;
    for (std::uint32_t c = 0; c + 1 < g.w; ++c) {
        maxStep = std::max(maxStep, static_cast<double>(std::fabs(lat(c + 1) - lat(c))));
    }
    const double wrapStep = std::fabs(static_cast<double>(lat(0) - lat(g.w - 1u)));
    INFO("wrap step " << wrapStep << " rad, largest other step " << maxStep << " rad");
    CHECK(wrapStep <= 1.5 * maxStep + 1e-6);
    CHECK(worstError(g, d, 0, 0) < 0.15);
    auto straight = render::measureLineStraightness(lines, kW, &g, nullptr);
    REQUIRE(straight.ok());
    CHECK(straight.value().lines == 1u);
    CHECK(straight.value().rmsPx < 0.2);
}

TEST_CASE("mesh warp: the structured gate's counts are gridFromFlow's", "[render][mesh]") {
    const render::MeshWarpParams p = testParams();
    Synthetic s = makeSynthetic(Disparity{});
    // Some pixels inconsistent and some uncovered, so every count differs.
    for (std::size_t i = 0; i < s.flow.ok.size(); i += 7) {
        s.flow.ok[i] = 0u;
    }
    for (std::size_t i = 0; i < s.bands.alpha[1].size(); i += 11) {
        s.bands.alpha[1][i] = 0.0f;
    }
    render::MeshWarpInputs in;
    in.bands = &s.bands;
    in.flow = &s.flow;
    auto mesh = render::solveMeshWarp(in, p);
    REQUIRE(mesh.ok());
    auto grid = render::gridFromFlow(s.bands, s.flow, p.parallax);
    REQUIRE(grid.ok());
    CHECK(mesh.value().grid.totalPixels == grid.value().totalPixels);
    CHECK(mesh.value().grid.consistentPixels == grid.value().consistentPixels);
    CHECK(mesh.value().grid.structuredPixels == grid.value().structuredPixels);
    CHECK(mesh.value().grid.consistentStructuredPixels == grid.value().consistentStructuredPixels);
    CHECK(mesh.value().report.structuredFraction == grid.value().structuredFraction());
}

TEST_CASE("mesh warp: the thread count does not change the field", "[render][mesh]") {
    const render::MeshWarpParams p = testParams();
    Disparity d;
    d.fvWave = 1.0;
    const Synthetic s = makeSynthetic(d, 2.0, 600, 640);
    const std::vector<render::SeamLine> lines = {segment(1, 500.0, 30.0, 760.0, 34.0)};
    render::MeshWarpInputs in;
    in.bands = &s.bands;
    in.flow = &s.flow;
    in.lines = &lines;
    ThreadPool pool(4);
    auto single = render::solveMeshWarp(in, p, nullptr);
    auto parallel = render::solveMeshWarp(in, p, &pool);
    REQUIRE(single.ok());
    REQUIRE(parallel.ok());
    CHECK(single.value().grid.uv == parallel.value().grid.uv);
}

TEST_CASE("mesh warp: malformed input is refused, broken values count as missing", "[render][mesh]") {
    const render::MeshWarpParams p = testParams();
    const Synthetic good = makeSynthetic(Disparity{});

    SECTION("a band plane of the wrong size") {
        Synthetic s = good;
        s.bands.luma[1].pop_back();
        render::MeshWarpInputs in;
        in.bands = &s.bands;
        in.flow = &s.flow;
        CHECK(!render::solveMeshWarp(in, p).ok());
    }
    SECTION("a flow that does not match the band") {
        Synthetic s = good;
        s.flow.forward.resize(kW, kH - 1u);
        render::MeshWarpInputs in;
        in.bands = &s.bands;
        in.flow = &s.flow;
        CHECK(!render::solveMeshWarp(in, p).ok());
    }
    SECTION("a flow without its bands") {
        render::MeshWarpInputs in;
        in.flow = &good.flow;
        CHECK(!render::solveMeshWarp(in, p).ok());
    }
    SECTION("a band of another polar map") {
        Synthetic s = good;
        s.bands.mapH = 900;
        render::MeshWarpInputs in;
        in.bands = &s.bands;
        CHECK(!render::solveMeshWarp(in, p).ok());
    }
    SECTION("a prior or previous mesh of another layout") {
        render::ParallaxWarpGrid other;
        other.w = 64;
        other.h = 13;
        other.latMinRad = 0.2f;
        other.latMaxRad = -0.2f;
        other.uv.assign(64u * 13u * 2u, 0.0f);
        render::MeshWarpInputs in;
        in.bands = &good.bands;
        in.flow = &good.flow;
        in.prior = &other;
        CHECK(!render::solveMeshWarp(in, p).ok());
        in.prior = nullptr;
        in.previous = &other;
        CHECK(!render::solveMeshWarp(in, p).ok());
    }
    SECTION("not-a-number flow vectors and luma are missing data, never values") {
        Synthetic s = good;
        for (std::size_t i = 0; i < s.flow.forward.u.size(); i += 5) {
            s.flow.forward.u[i] = std::numeric_limits<float>::quiet_NaN();
        }
        for (std::size_t i = 3; i < s.bands.luma[0].size(); i += 13) {
            s.bands.luma[0][i] = std::numeric_limits<float>::infinity();
        }
        render::MeshWarpInputs in;
        in.bands = &s.bands;
        in.flow = &s.flow;
        auto r = render::solveMeshWarp(in, p);
        REQUIRE(r.ok());
        CHECK(std::all_of(r.value().grid.uv.begin(), r.value().grid.uv.end(), [](float v) { return std::isfinite(v); }));
        CHECK(worstError(r.value().grid, Disparity{}, 0, 0) < 0.15);
    }
    SECTION("the line detector refuses a malformed band") {
        render::LensBands b = good.bands;
        b.alpha[0].clear();
        CHECK(!render::detectSeamLines(b, render::LineDetectParams{}).ok());
    }
}

// ===========================================================================
//  The line detector and the metric
// ===========================================================================

TEST_CASE("mesh warp: the line detector finds a straight edge and never a coverage edge", "[render][mesh][lines]") {
    render::LensBands b;
    b.w = kW;
    b.h = kH;
    b.rowOffset = kRow0;
    b.mapH = kMapH;
    const std::size_t n = static_cast<std::size_t>(kW) * kH;
    for (int lens = 0; lens < 2; ++lens) {
        b.luma[lens].assign(n, 0.5f);
        b.alpha[lens].assign(n, 1.0f);
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * kW + x;
                // A horizontal edge between rows 29 and 30 over columns 100-700.
                if (x >= 100 && x < 700) {
                    b.luma[lens][i] = y < 30 ? 0.3f : 0.7f;
                }
                // A vertical edge at column 1200.
                if (x >= 1200 && x < 1260) {
                    b.luma[lens][i] = 0.75f;
                }
                // A coverage hole with black beyond it: a strong luma edge
                // at the coverage boundary that must not become a line.
                if (x >= 1500 && x < 1600) {
                    b.alpha[lens][i] = 0.0f;
                    b.luma[lens][i] = 0.0f;
                }
            }
        }
    }
    auto found = render::detectSeamLines(b, render::LineDetectParams{});
    REQUIRE(found.ok());
    const render::SeamLineSet& set = found.value();
    bool horizontal[2] = {false, false};
    bool vertical[2] = {false, false};
    for (const render::SeamLine& l : set.lines) {
        // Band pixel-centre coordinates of the endpoints.
        const double x0 = (l.lon0Rad + kPi) / kRadPerPx;
        const double x1 = (l.lon1Rad + kPi) / kRadPerPx;
        const double y0 = (kHalfPi - l.lat0Rad) / (kPi / kMapH) - kRow0;
        const double y1 = (kHalfPi - l.lat1Rad) / (kPi / kMapH) - kRow0;
        // Nothing near the coverage boundary columns.
        CHECK(!(std::fabs(x0 - 1500.0) < 3.0 && std::fabs(x1 - 1500.0) < 3.0));
        CHECK(!(std::fabs(x0 - 1600.0) < 3.0 && std::fabs(x1 - 1600.0) < 3.0));
        if (std::fabs(y0 - 30.0) < 1.0 && std::fabs(y1 - 30.0) < 1.0 && std::fabs(x1 - x0) > 400.0) {
            horizontal[l.lens] = true;
        }
        if ((std::fabs(x0 - 1200.0) < 1.5 || std::fabs(x0 - 1260.0) < 1.5) && std::fabs(x1 - x0) < 1.5 &&
            std::fabs(y1 - y0) > 40.0) {
            vertical[l.lens] = true;
        }
    }
    CHECK(set.perLens[0] > 0u);
    CHECK(horizontal[0]);
    CHECK(horizontal[1]);
    CHECK(vertical[0]);
    CHECK(vertical[1]);
    // A featureless band has no line at all.
    render::LensBands flat = b;
    for (int lens = 0; lens < 2; ++lens) {
        std::fill(flat.luma[lens].begin(), flat.luma[lens].end(), 0.5f);
        std::fill(flat.alpha[lens].begin(), flat.alpha[lens].end(), 1.0f);
    }
    auto none = render::detectSeamLines(flat, render::LineDetectParams{});
    REQUIRE(none.ok());
    CHECK(none.value().lines.empty());
}

TEST_CASE("mesh warp: the straightness metric - zero without a correction, a bump bends, the table counts",
          "[render][mesh][lines]") {
    const render::MeshWarpParams p = testParams();
    const std::vector<render::SeamLine> lines = {segment(1, 200.0, 34.0, 600.0, 34.0)};
    auto zero = render::measureLineStraightness(lines, kW, nullptr, nullptr);
    REQUIRE(zero.ok());
    CHECK(zero.value().lines == 1u);
    CHECK(zero.value().rmsPx < 1e-9);
    // A field with a bump along the meridian under the line.
    auto layout = render::meshWarpLayout(p);
    REQUIRE(layout.ok());
    render::ParallaxWarpGrid bump = layout.value();
    for (std::uint32_t j = 1; j + 1 < bump.h; ++j) {
        bump.uv[(static_cast<std::size_t>(j) * bump.w + 25u) * 2u + 1u] = static_cast<float>(2.0 * kRadPerPx);
    }
    auto bent = render::measureLineStraightness(lines, kW, &bump, nullptr);
    REQUIRE(bent.ok());
    CHECK(bent.value().rmsPx > 0.3);
    // A table with a step under the line bends it too (the kernel's 1-D path).
    std::vector<float> table(2048, 0.0f);
    for (std::size_t c = 380; c < 420; ++c) {
        table[c] = 1.0f;
    }
    auto tabled = render::measureLineStraightness(lines, kW, nullptr, &table);
    REQUIRE(tabled.ok());
    CHECK(tabled.value().rmsPx > 0.3);
    // Change between two grids: zero for one grid, the bump's for the pair.
    auto same = render::meanAbsGridChangeDeg(bump, bump, 6.0);
    REQUIRE(same.ok());
    CHECK(same.value() == 0.0);
    auto diff = render::meanAbsGridChangeDeg(bump, layout.value(), 6.0);
    REQUIRE(diff.ok());
    CHECK(diff.value() > 0.0);
    render::ParallaxWarpGrid other = layout.value();
    other.w = 64;
    other.uv.resize(64u * other.h * 2u);
    CHECK(!render::meanAbsGridChangeDeg(bump, other, 6.0).ok());
}

// ===========================================================================
//  End to end with the flow solver, and the cost
// ===========================================================================

TEST_CASE("mesh warp: end to end with the classical flow recovers a shift", "[render][mesh]") {
    render::MeshWarpParams p = testParams();
    p.parallax.backend = render::FlowBackendKind::Classical;
    const Disparity d;  // 1 px across, 4 px along the meridian
    const Synthetic s = makeSynthetic(d);
    ThreadPool pool(4);
    auto r = render::meshWarpFromBands(s.bands, nullptr, nullptr, nullptr, nullptr, nullptr, p, &pool);
    REQUIRE(r.ok());
    // The mean over the band's middle rows (the flow solver is not exact at
    // every pixel; the mesh must carry its consensus).
    double sumLon = 0.0;
    double sumLat = 0.0;
    int n = 0;
    for (std::uint32_t y = 20; y < kH - 20; y += 4) {
        for (std::uint32_t x = 0; x < kW; x += 16) {
            double dl = 0.0;
            double dt = 0.0;
            fieldAtPixel(r.value().grid, x, y, dl, dt);
            sumLon += dl;
            sumLat += dt;
            ++n;
        }
    }
    INFO("mean field " << sumLon / n << " / " << sumLat / n << " px against 0.5 / -2");
    CHECK(std::fabs(sumLon / n - 0.5) < 0.2);
    CHECK(std::fabs(sumLat / n + 2.0) < 0.3);
}

TEST_CASE("mesh warp: a 2048-column band solves within the per-bucket budget", "[render][mesh][perf]") {
    const render::MeshWarpParams p = testParams();
    Disparity d;
    d.fvWave = 1.0;
    const Synthetic s = makeSynthetic(d, 2.0, 600, 640);
    std::vector<render::SeamLine> lines;
    for (int k = 0; k < 40; ++k) {
        const double x = 40.0 + 50.0 * k;
        lines.push_back(segment(k % 2, x, 10.0 + (k % 5) * 10.0, x + 40.0, 14.0 + (k % 5) * 10.0));
    }
    render::MeshWarpInputs in;
    in.bands = &s.bands;
    in.flow = &s.flow;
    in.lines = &lines;
    ThreadPool pool(4);
    (void)render::solveMeshWarp(in, p, &pool);  // warm the caches and the pool
    double best = std::numeric_limits<double>::infinity();
    for (int k = 0; k < 3; ++k) {
        auto r = render::solveMeshWarp(in, p, &pool);
        REQUIRE(r.ok());
        best = std::min(best, r.value().report.totalMs);
    }
    std::printf("mesh warp: 2048-column band solved in %.1f ms on 4 threads (goal 15 ms)\n", best);
    // The 15 ms goal is reported, not enforced (a loaded CI machine is
    // slower); a solve this size taking a second would be a real fault.
    CHECK(best < 1000.0);
}
