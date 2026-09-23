// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// [WP-SEAM] The carved blend seam (SeamCarve.h, osv_kernel.h
// osvBlendSeamApply):
//
//   * the DP on synthetic cost grids with a known best seam, forbidden
//     regions and the closed longitude ring;
//   * the carve on synthetic bands: disagreement avoided, the occluded lens
//     never chosen, the temporal clamp held, the penalty hooks obeyed;
//   * the kernel's seam weights: the chosen lens alone outside the feather,
//     coverage filling in for a blind lens, nothing changed outside the
//     overlap or without a table;
//   * the builder's refusals, the glide between buckets;
//   * on the sample clip: the doubled fin gone - the ghost and seam-edge
//     measures against the feather blend and an uncarved narrow seam - and
//     the temporal clamp between buckets.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamCarve.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace osv;
using Catch::Approx;

namespace {

// ---------------------------------------------------------------------------
//  Synthetic bands
// ---------------------------------------------------------------------------

/// Band geometry of the importer's analyses: 2048 columns of a 1024-row
/// polar-axis map, +/-6 degrees (68 rows) around the equator.
constexpr std::uint32_t kBandW = 2048;
constexpr std::uint32_t kMapH = 1024;
constexpr std::uint32_t kBandH = 68;

/// Band texture in [0.3, 0.7], smooth and wrapping in x.
double texture(double x, double y) noexcept {
    const double t = osv::kTwoPi * x / static_cast<double>(kBandW);
    return 0.5 + 0.08 * std::sin(23.0 * t + 0.37 * y) + 0.07 * std::sin(0.61 * y - 11.0 * t + 0.4) +
           0.05 * std::sin(41.0 * t + 0.23 * y + 1.7);
}

/// Two identical, fully covered lens bands of the importer's geometry.
render::LensBands agreeingBands() {
    render::LensBands b;
    b.w = kBandW;
    b.h = kBandH;
    b.mapH = kMapH;
    b.rowOffset = kMapH / 2 - kBandH / 2;
    const std::size_t n = static_cast<std::size_t>(kBandW) * kBandH;
    for (int lens = 0; lens < 2; ++lens) {
        b.luma[lens].resize(n);
        b.alpha[lens].assign(n, 1.0f);
    }
    for (std::uint32_t y = 0; y < kBandH; ++y) {
        for (std::uint32_t x = 0; x < kBandW; ++x) {
            const float v = static_cast<float>(texture(x, y));
            b.luma[0][static_cast<std::size_t>(y) * kBandW + x] = v;
            b.luma[1][static_cast<std::size_t>(y) * kBandW + x] = v;
        }
    }
    return b;
}

/// The band row a seam latitude falls on (row centres at integer + 0.5).
double rowOfLat(const render::LensBands& b, double latRad) {
    return (osv::kHalfPi - latRad) / (osv::kPi / static_cast<double>(b.mapH)) - static_cast<double>(b.rowOffset) - 0.5;
}

/// Seam row at seam column c.
double seamRow(const render::LensBands& b, const render::BlendSeam& s, std::uint32_t c) {
    return rowOfLat(b, static_cast<double>(s.table[static_cast<std::size_t>(c) * 2u]));
}

/// Seam columns (of 1024) covering band columns [x0, x1).
std::pair<std::uint32_t, std::uint32_t> seamColumns(std::uint32_t x0, std::uint32_t x1) {
    return {x0 / 2, x1 / 2};
}

/// The sample clip's verified calibration as a stream-space rig of the given
/// width (the constants test_render.cpp and test_parallax.cpp use), for the
/// builder test that needs a real rig but no clip.
Result<geom::LensRig> makeCalibratedRig(int streamW) {
    meta::CalibrationSet cal;
    auto fill = [](meta::DewarpParams& d, float fx, float fy, float cx, float cy, const float k[5], float qw, float qx,
                   float qy, float qz) {
        d.fx = fx;
        d.fy = fy;
        d.cx = cx;
        d.cy = cy;
        for (int i = 0; i < 5; ++i) {
            d.k[static_cast<std::size_t>(i)] = k[i];
        }
        d.width = 3840;
        d.height = 3840;
        d.camExtriQ.w = qw;
        d.camExtriQ.x = qx;
        d.camExtriQ.y = qy;
        d.camExtriQ.z = qz;
        d.camExtriQ.present = true;
        for (int b = 1; b <= 11; ++b) {
            d.present.set(static_cast<std::size_t>(b));
        }
        d.present.set(28);
    };
    const float ks[5] = {0.0667397f, -0.0128859f, 0.0103815f, -0.00677581f, 0.00098791f};
    const float km[5] = {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f};
    fill(cal.slave, 1043.8802f, 1043.6731f, 1917.0421f, 1919.1294f, ks, 0.0026615f, 0.0019091f, -0.7056412f,
         0.7085618f);
    fill(cal.master, 1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f, km, 0.7036960f, 0.7103991f, -0.0046943f,
         -0.0110939f);
    cal.sourceSlave = "test";
    cal.sourceMaster = "test";
    const double dfl = 829.3612 * (streamW / 3000.0);
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(streamW, streamW, 3840, 3840, dfl, 1043.445, streamW / 3776.0, nullptr));
    return geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength, dfl, geom::ExtrinsicConvention{});
}

// ---------------------------------------------------------------------------
//  Penalty hooks for the hook tests
// ---------------------------------------------------------------------------

/// Makes SHOWING the master lens expensive on the band's south half in
/// band columns [800, 1000): the seam must move north there.
bool masterSouthPenalty(const render::LensBands& b, std::vector<float>& slave, std::vector<float>& master,
                        void* user) noexcept {
    (void)slave;
    (void)user;
    for (std::uint32_t y = b.h / 2; y < b.h; ++y) {
        for (std::uint32_t x = 800; x < 1000 && x < b.w; ++x) {
            master[static_cast<std::size_t>(y) * b.w + x] = 0.2f;
        }
    }
    return true;
}

/// A hook that misbehaves: NaN and negative penalties everywhere.
bool garbagePenalty(const render::LensBands& b, std::vector<float>& slave, std::vector<float>& master,
                    void* user) noexcept {
    (void)b;
    (void)user;
    std::fill(slave.begin(), slave.end(), std::numeric_limits<float>::quiet_NaN());
    std::fill(master.begin(), master.end(), -5.0f);
    return true;
}

// ---------------------------------------------------------------------------
//  Kernel helper
// ---------------------------------------------------------------------------

/// A parameter block for osvBlendSeamApply alone: two lenses looking along
/// +Y (master) and -Y (slave), thetaMax 97.5 degrees, 4 degree feather, a
/// constant seam at `seamLatDeg` with half width `hwDeg`, `columns` columns.
struct SeamKernel {
    OsvRenderParams p{};
    std::vector<float> table;

    SeamKernel(double seamLatDeg, double hwDeg, int columns = 8) {
        std::memset(&p, 0, sizeof(p));
        p.blendEnabled = 1;
        p.blendSeamEnabled = 1;
        p.blendSeamColumns = columns;
        p.blendSeamEdgeRad = static_cast<float>(deg2rad(0.5));
        for (int i = 0; i < 2; ++i) {
            p.lens[i].thetaMax = static_cast<float>(deg2rad(97.5));
            p.lens[i].featherRad = static_cast<float>(deg2rad(4.0));
            p.lens[i].enabled = 1;
        }
        table.resize(static_cast<std::size_t>(columns) * 2u);
        for (int c = 0; c < columns; ++c) {
            table[static_cast<std::size_t>(c) * 2u] = static_cast<float>(deg2rad(seamLatDeg));
            table[static_cast<std::size_t>(c) * 2u + 1u] = static_cast<float>(deg2rad(hwDeg));
        }
    }

    /// Apply the seam to a ray at polar latitude `latDeg` (longitude 0),
    /// with the given shader lens weights; returns the new sum.
    float apply(double latDeg, float w0, float w1, float out[2]) const {
        const double lat = deg2rad(latDeg);
        const float d[3] = {0.0f, static_cast<float>(std::sin(lat)), static_cast<float>(std::cos(lat))};
        // Angle from each axis: master at +Y, slave at -Y.
        const float theta[2] = {static_cast<float>(osv::kHalfPi + lat), static_cast<float>(osv::kHalfPi - lat)};
        float w[2] = {w0, w1};
        const float sum = osvBlendSeamApply(&p, table.data(), d, theta, w, w0 + w1);
        out[0] = w[0];
        out[1] = w[1];
        return sum;
    }
};

// ---------------------------------------------------------------------------
//  Sample clip
// ---------------------------------------------------------------------------

struct SampleClip {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
};

Result<SampleClip> openSample() {
    SampleClip c;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleOsv()));
    c.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(c.track, meta::MetadataTrack::load(*c.file));
    OSV_TRY_ASSIGN(c.format, meta::FormatDetector::detect(*c.file, &c.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(c.track.stream()));
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(c.format.streamW), static_cast<int>(c.format.streamH),
                                               static_cast<int>(c.format.sensorW), static_cast<int>(c.format.sensorH),
                                               c.format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(c.rig, geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength,
                                               c.format.digitalFocalLength, geom::ExtrinsicConvention{}));
    return c;
}

/// Rec.709 luma of a display-referred pixel.
double luma(const float* px) noexcept {
    return 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2];
}

/// The two measures of the bench (tests/bench/SeamCarveBench.cpp), over the
/// co-visible band |lat| <= 8 degrees of a polar-axis render, optionally
/// restricted to polar-map columns [x0, W) + [0, x1):
///   ghost    mean 2 min(a, 1 - a) |S0 - S1|, a solved from B = a S0 + (1 - a) S1
///   edge     mean max(0, |grad B| - max(|grad S0|, |grad S1|) - 0.004)
struct Measures {
    double ghost = 0.0;
    double edge = 0.0;
};

Measures measure(const render::ImageRGBAf& B, const render::ImageRGBAf& S0, const render::ImageRGBAf& S1, int x0,
                 int x1) {
    const int W = static_cast<int>(B.w);
    const int H = static_cast<int>(B.h);
    const int half = static_cast<int>(std::lround(8.0 / 180.0 * H));
    const auto inWindow = [&](int x) { return x0 < 0 || x >= x0 || x < x1; };
    const auto lumaAt = [&](const render::ImageRGBAf& im, int x, int y) {
        return luma(im.pixel(static_cast<std::uint32_t>(((x % W) + W) % W), static_cast<std::uint32_t>(y)));
    };
    const auto gradAt = [&](const render::ImageRGBAf& im, int x, int y) {
        const double gx = 0.5 * (lumaAt(im, x + 1, y) - lumaAt(im, x - 1, y));
        const double gy = 0.5 * (lumaAt(im, x, y + 1) - lumaAt(im, x, y - 1));
        return std::sqrt(gx * gx + gy * gy);
    };
    double ghost = 0.0;
    double edge = 0.0;
    std::size_t n = 0;
    for (int y = std::max(1, H / 2 - half); y <= std::min(H - 2, H / 2 + half); ++y) {
        for (int x = 0; x < W; ++x) {
            if (!inWindow(x)) {
                continue;
            }
            const float* p0 = S0.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const float* p1 = S1.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            if (!(p0[3] > 0.5f) || !(p1[3] > 0.5f)) {
                continue;
            }
            const double d = luma(p0) - luma(p1);
            if (std::fabs(d) > 0.02) {
                const double a = std::clamp((lumaAt(B, x, y) - luma(p1)) / d, 0.0, 1.0);
                ghost += 2.0 * std::min(a, 1.0 - a) * std::fabs(d);
            }
            edge += std::max(0.0, gradAt(B, x, y) - std::max(gradAt(S0, x, y), gradAt(S1, x, y)) - 0.004);
            ++n;
        }
    }
    Measures m;
    if (n > 0) {
        m.ghost = 1000.0 * ghost / static_cast<double>(n);
        m.edge = 1000.0 * edge / static_cast<double>(n);
    }
    return m;
}

}  // namespace

// ===========================================================================
//  1. The DP
// ===========================================================================

TEST_CASE("solveSeamDp follows a known cheapest seam round a closed ring", "[render][seamcarve]") {
    constexpr std::uint32_t C = 256;
    constexpr std::uint32_t R = 24;
    // A sinusoidal valley of zero cost in a field of ones.  Its slope (at
    // most 6 * 2 pi * 2 / 256 ~ 0.3 rows per column) is well inside one step.
    std::vector<int> valley(C);
    std::vector<float> cost(static_cast<std::size_t>(C) * R, 1.0f);
    for (std::uint32_t c = 0; c < C; ++c) {
        valley[c] = static_cast<int>(std::lround(11.0 + 6.0 * std::sin(osv::kTwoPi * 2.0 * c / C)));
        cost[static_cast<std::size_t>(valley[c]) * C + c] = 0.0f;
    }
    auto path = render::solveSeamDp(cost, C, R, 2, 0.01, true);
    REQUIRE(path.ok());
    REQUIRE(path.value().size() == C);
    for (std::uint32_t c = 0; c < C; ++c) {
        INFO("column " << c);
        CHECK(path.value()[c] == valley[c]);
    }
}

TEST_CASE("solveSeamDp routes around a forbidden block and closes the ring", "[render][seamcarve]") {
    constexpr std::uint32_t C = 200;
    constexpr std::uint32_t R = 30;
    // Cheapest everywhere on row 15, except a forbidden block across it.
    std::vector<float> cost(static_cast<std::size_t>(C) * R, 0.5f);
    for (std::uint32_t c = 0; c < C; ++c) {
        cost[15u * C + c] = 0.0f;
    }
    for (std::uint32_t c = 80; c < 120; ++c) {
        for (std::uint32_t r = 8; r <= 22; ++r) {
            cost[static_cast<std::size_t>(r) * C + c] = 1.0e6f;
        }
    }
    // One non-finite cell must count as forbidden, not poison the sums.
    cost[15u * C + 50u] = std::numeric_limits<float>::quiet_NaN();

    auto path = render::solveSeamDp(cost, C, R, 2, 0.05, true);
    REQUIRE(path.ok());
    const std::vector<int>& p = path.value();
    for (std::uint32_t c = 80; c < 120; ++c) {
        INFO("column " << c << " row " << p[c]);
        CHECK((p[c] < 8 || p[c] > 22));
    }
    CHECK(p[50] != 15);
    // Every step, including the closure from the last column to the first,
    // is within the step limit.
    for (std::uint32_t c = 0; c < C; ++c) {
        const int next = p[(c + 1) % C];
        CHECK(std::abs(next - p[c]) <= 2);
    }
    // Away from the block and the NaN it sits on its valley.
    CHECK(p[10] == 15);
    CHECK(p[170] == 15);
}

TEST_CASE("solveSeamDp refuses malformed input", "[render][seamcarve]") {
    std::vector<float> cost(10 * 5, 0.0f);
    CHECK(render::solveSeamDp(cost, 10, 6, 1, 0.1, true).error().code == ErrorCode::InvalidArgument);
    CHECK(render::solveSeamDp(cost, 0, 5, 1, 0.1, true).error().code == ErrorCode::InvalidArgument);
    CHECK(render::solveSeamDp(cost, 10, 5, -1, 0.1, true).error().code == ErrorCode::InvalidArgument);
    CHECK(render::solveSeamDp(cost, 10, 5, 1, std::numeric_limits<double>::infinity(), true).error().code ==
          ErrorCode::InvalidArgument);
    // An open (non-ring) solve with a step limit of 0 is a straight row.
    for (std::size_t c = 0; c < 10; ++c) {
        cost[3u * 10u + c] = -1.0f;  // cheapest row 3
    }
    auto straight = render::solveSeamDp(cost, 10, 5, 0, 0.0, false);
    REQUIRE(straight.ok());
    for (int r : straight.value()) {
        CHECK(r == 3);
    }
}

// ===========================================================================
//  2. The carve on synthetic bands
// ===========================================================================

TEST_CASE("the carve keeps its feather out of where the lenses disagree", "[render][seamcarve]") {
    render::LensBands b = agreeingBands();
    // The master sees something the slave does not on rows 26..44 of band
    // columns 600..800 - right across the geometric seam (row 33.5).
    for (std::uint32_t y = 26; y <= 44; ++y) {
        for (std::uint32_t x = 600; x < 800; ++x) {
            b.luma[1][static_cast<std::size_t>(y) * kBandW + x] = 0.05f;
        }
    }
    ThreadPool pool(4);
    render::SeamCarveParams params;
    auto seam = render::carveSeamFromBands(b, {}, params, nullptr, &pool);
    REQUIRE(seam.ok());
    REQUIRE(seam.value().valid());
    CHECK(seam.value().columns == 1024);
    CHECK(seam.value().forcedColumns == 0);
    const auto [c0, c1] = seamColumns(610, 790);
    const double mw = params.narrowHalfWidthDeg * kMapH / 180.0;
    for (std::uint32_t c = c0; c < c1; ++c) {
        const double r = seamRow(b, seam.value(), c);
        INFO("seam column " << c << " row " << r);
        CHECK((r + mw < 26.0 || r - mw > 44.0));
    }
    // Where the lenses agree it stays near the geometric seam.
    CHECK(std::fabs(seamRow(b, seam.value(), 100) - 33.5) < 3.0);
    // ... and gets the wide feather there, the narrow one at the object's
    // edges is not required (the seam avoids it), but no feather may be
    // wider than the configured wide one.
    for (std::uint32_t c = 0; c < 1024; ++c) {
        CHECK(seam.value().table[c * 2u + 1u] <= static_cast<float>(deg2rad(params.wideHalfWidthDeg)) + 1e-6f);
    }
}

TEST_CASE("the carve never shows a lens where it cannot see: the stick routes the seam", "[render][seamcarve]") {
    render::LensBands b = agreeingBands();
    // The slave (lens 0) is blind on rows 0..44 of band columns 1000..1200:
    // the seam must pass south of that, so the master covers it.
    for (std::uint32_t y = 0; y <= 44; ++y) {
        for (std::uint32_t x = 1000; x < 1200; ++x) {
            b.alpha[0][static_cast<std::size_t>(y) * kBandW + x] = 0.0f;
            b.luma[0][static_cast<std::size_t>(y) * kBandW + x] = 0.0f;
        }
    }
    ThreadPool pool(4);
    render::SeamCarveParams params;
    auto seam = render::carveSeamFromBands(b, {}, params, nullptr, &pool);
    REQUIRE(seam.ok());
    CHECK(seam.value().forcedColumns == 0);
    const auto [c0, c1] = seamColumns(1000, 1200);
    for (std::uint32_t c = c0; c < c1; ++c) {
        const double r = seamRow(b, seam.value(), c);
        const double hwRows = static_cast<double>(seam.value().table[c * 2u + 1u]) / (osv::kPi / kMapH);
        INFO("seam column " << c << " row " << r << " half width " << hwRows << " rows");
        // The whole feather lies in rows the slave sees.
        CHECK(r - hwRows > 44.0);
    }
}

TEST_CASE("the temporal clamp holds the seam near its neighbour's", "[render][seamcarve]") {
    const render::LensBands b = agreeingBands();
    ThreadPool pool(4);
    render::SeamCarveParams params;

    // On its own the seam sits on the geometric seam (the centre pull).
    auto free = render::carveSeamFromBands(b, {}, params, nullptr, &pool);
    REQUIRE(free.ok());
    CHECK_FALSE(free.value().usedPrior);
    CHECK(std::fabs(free.value().meanLatDeg) < 0.5);

    // A neighbour 4 degrees north: the clamp (2 degrees) keeps this one
    // within reach of it, whatever the content prefers.
    render::BlendSeam prior = free.value();
    for (std::uint32_t c = 0; c < prior.columns; ++c) {
        prior.table[c * 2u] = static_cast<float>(deg2rad(4.0));
    }
    auto held = render::carveSeamFromBands(b, {}, params, &prior, &pool);
    REQUIRE(held.ok());
    CHECK(held.value().usedPrior);
    CHECK(held.value().maxPriorStepDeg <= params.temporalClampDeg + 1e-6);
    for (std::uint32_t c = 0; c < held.value().columns; ++c) {
        const double lat = rad2deg(held.value().table[c * 2u]);
        CHECK(lat >= 4.0 - params.temporalClampDeg - 1e-3);
    }

    // A prior of another size is ignored rather than misread.
    render::BlendSeam other = prior;
    other.columns = 512;
    other.table.resize(1024);
    auto ignored = render::carveSeamFromBands(b, {}, params, &other, &pool);
    REQUIRE(ignored.ok());
    CHECK_FALSE(ignored.value().usedPrior);
}

TEST_CASE("penalty hooks steer the seam and garbage from a hook is ignored", "[render][seamcarve]") {
    const render::LensBands b = agreeingBands();
    ThreadPool pool(4);
    render::SeamCarveParams params;
    const auto [c0, c1] = seamColumns(820, 980);

    SECTION("a per-carve hook") {
        params.penalty.fn = &masterSouthPenalty;
        auto seam = render::carveSeamFromBands(b, {}, params, nullptr, &pool);
        REQUIRE(seam.ok());
        for (std::uint32_t c = c0; c < c1; ++c) {
            // The master is shown on rows [0, seam + feather]; the penalty
            // sits on rows [34, 68) of these columns, so the cheapest seam
            // there lies north of row 34 and shows none of it.
            CHECK(seamRow(b, seam.value(), c) < 34.0);
        }
    }
    SECTION("an installed slot, removed again") {
        render::SeamPenaltyHook hook;
        hook.fn = &masterSouthPenalty;
        render::setSeamPenaltyHook(render::SeamPenaltySlot::Flare, hook);
        auto seam = render::carveSeamFromBands(b, {}, params, nullptr, &pool);
        render::setSeamPenaltyHook(render::SeamPenaltySlot::Flare, {});
        REQUIRE(seam.ok());
        for (std::uint32_t c = c0; c < c1; ++c) {
            CHECK(seamRow(b, seam.value(), c) < 34.0);
        }
        CHECK_FALSE(render::seamPenaltyHook(render::SeamPenaltySlot::Flare).installed());
    }
    SECTION("NaN and negative penalties contribute nothing") {
        params.penalty.fn = &garbagePenalty;
        auto seam = render::carveSeamFromBands(b, {}, params, nullptr, &pool);
        auto plain = render::carveSeamFromBands(b, {}, render::SeamCarveParams{}, nullptr, &pool);
        REQUIRE(seam.ok());
        REQUIRE(plain.ok());
        CHECK(seam.value().table == plain.value().table);
    }
}

TEST_CASE("the band correction moves each lens the way the kernel does", "[render][seamcarve]") {
    // A constant grid of +k rows of latitude: the master samples k rows
    // north (row - k), the slave k rows south, exactly as the kernel and
    // ParallaxWarp's gate apply it.
    const render::LensBands b = agreeingBands();
    constexpr int k = 3;
    const double radPerRow = osv::kPi / kMapH;
    std::vector<float> uv(16u * 4u * 2u);
    for (std::size_t i = 0; i < uv.size(); i += 2) {
        uv[i + 1] = static_cast<float>(k * radPerRow);
    }
    render::WarpGridView view{uv.data(), 16, 4, static_cast<float>(deg2rad(20.0)), static_cast<float>(deg2rad(-20.0))};
    render::SeamCorrection corr;
    corr.warp = &view;
    auto warped = render::correctBandsForSeam(b, corr, nullptr);
    REQUIRE(warped.ok());
    double errMaster = 0.0;
    double errSlave = 0.0;
    for (std::uint32_t y = 10; y < kBandH - 10; ++y) {
        for (std::uint32_t x = 0; x < kBandW; x += 7) {
            const std::size_t i = static_cast<std::size_t>(y) * kBandW + x;
            const double dm = std::fabs(static_cast<double>(warped.value().luma[1][i]) -
                                        b.luma[1][static_cast<std::size_t>(y - k) * kBandW + x]);
            const double ds = std::fabs(static_cast<double>(warped.value().luma[0][i]) -
                                        b.luma[0][static_cast<std::size_t>(y + k) * kBandW + x]);
            errMaster = std::max(errMaster, dm);
            errSlave = std::max(errSlave, ds);
        }
    }
    CHECK(errMaster < 1e-4);
    CHECK(errSlave < 1e-4);

    // No correction: the bands come back untouched.
    auto same = render::correctBandsForSeam(b, {}, nullptr);
    REQUIRE(same.ok());
    CHECK(same.value().luma[0] == b.luma[0]);
    CHECK(same.value().alpha[1] == b.alpha[1]);
}

TEST_CASE("the carve refuses malformed bands and parameters", "[render][seamcarve]") {
    render::LensBands b = agreeingBands();
    render::SeamCarveParams params;
    ThreadPool pool(2);

    params.columns = 1000;  // 2048 is not a multiple
    CHECK(render::carveSeamFromBands(b, {}, params, nullptr, &pool).error().code == ErrorCode::InvalidArgument);
    params = {};
    params.narrowHalfWidthDeg = 2.0;  // wider than the wide feather
    CHECK(render::carveSeamFromBands(b, {}, params, nullptr, &pool).error().code == ErrorCode::InvalidArgument);
    params = {};
    params.stepPenalty = std::numeric_limits<double>::quiet_NaN();
    CHECK(render::carveSeamFromBands(b, {}, params, nullptr, &pool).error().code == ErrorCode::InvalidArgument);
    params = {};
    b.alpha[1].pop_back();
    CHECK(render::carveSeamFromBands(b, {}, params, nullptr, &pool).error().code == ErrorCode::InvalidArgument);
    b = agreeingBands();
    b.rowOffset = kMapH;  // rows outside the map
    CHECK(render::carveSeamFromBands(b, {}, params, nullptr, &pool).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("blendSeams glides column by column and refuses mismatched seams", "[render][seamcarve]") {
    render::BlendSeam a;
    a.columns = 4;
    a.table = {0.0f, 0.01f, 0.1f, 0.01f, 0.0f, 0.02f, -0.1f, 0.01f};
    a.edgeRad = 0.01f;
    render::BlendSeam b = a;
    b.table = {0.2f, 0.03f, 0.1f, 0.01f, 0.2f, 0.02f, 0.1f, 0.05f};
    b.meanLatDeg = 7.0;
    auto mid = render::blendSeams(a, b, 0.25);
    REQUIRE(mid.ok());
    CHECK(mid.value().table[0] == Approx(0.05f));
    CHECK(mid.value().table[1] == Approx(0.015f));
    CHECK(mid.value().table[6] == Approx(-0.05f));
    CHECK(mid.value().meanLatDeg == 7.0);      // the newer diagnostics
    auto end = render::blendSeams(a, b, 5.0);  // clamped to 1
    REQUIRE(end.ok());
    CHECK(end.value().table == b.table);

    render::BlendSeam wrong = b;
    wrong.columns = 2;
    wrong.table.resize(4);
    CHECK(render::blendSeams(a, wrong, 0.5).error().code == ErrorCode::InvalidArgument);
    CHECK(render::blendSeams(a, b, std::numeric_limits<double>::quiet_NaN()).error().code ==
          ErrorCode::InvalidArgument);
    render::BlendSeam broken = b;
    broken.table[3] = -1.0f;  // negative half width
    CHECK_FALSE(broken.valid());
    CHECK(render::blendSeams(a, broken, 0.5).error().code == ErrorCode::InvalidArgument);
}

// ===========================================================================
//  3. The kernel's seam weights and the builder
// ===========================================================================

TEST_CASE("osvBlendSeamApply: one lens outside the feather, a smooth crossover inside", "[render][seamcarve]") {
    const SeamKernel k(1.0, 0.5);
    float w[2];
    // North of the feather: the master alone, weights summing to 1.
    CHECK(k.apply(3.0, 1.0f, 1.0f, w) == Approx(1.0f));
    CHECK(w[0] == 0.0f);
    CHECK(w[1] == 1.0f);
    // South of it: the slave alone.
    k.apply(-2.0, 1.0f, 1.0f, w);
    CHECK(w[0] == 1.0f);
    CHECK(w[1] == 0.0f);
    // On the seam: half each.
    k.apply(1.0, 1.0f, 1.0f, w);
    CHECK(w[0] == Approx(0.5f));
    CHECK(w[1] == Approx(0.5f));
    // Monotonic across the feather.
    float prev = -1.0f;
    for (double lat = 0.4; lat <= 1.6; lat += 0.05) {
        k.apply(lat, 1.0f, 1.0f, w);
        CHECK(w[1] >= prev);
        CHECK(w[0] + w[1] == Approx(1.0f));
        prev = w[1];
    }
}

TEST_CASE("osvBlendSeamApply: coverage fills in for a blind lens", "[render][seamcarve]") {
    const SeamKernel k(0.0, 0.3);
    float w[2];
    // North of the seam the master is chosen, but occluded to 20 % (the FOV
    // feather is 1 this close to the axis, so the lens weight IS the
    // occlusion factor): the slave takes over what the master cannot see.
    const float sum = k.apply(2.0, 1.0f, 0.2f, w);
    CHECK(sum == Approx(1.0f));
    CHECK(w[1] == Approx(0.2f + 0.8f * 0.2f / 1.2f));
    CHECK(w[0] == Approx(0.8f * 1.0f / 1.2f));
    // A ray only ONE lens sees is none of the seam's business: untouched.
    CHECK(k.apply(2.0, 0.0f, 0.7f, w) == Approx(0.7f));
    CHECK(w[0] == 0.0f);
    CHECK(w[1] == 0.7f);
    // Nearest-lens renders and a missing table leave the weights alone.
    SeamKernel off(0.0, 0.3);
    off.p.blendEnabled = 0;
    CHECK(off.apply(2.0, 1.0f, 1.0f, w) == Approx(2.0f));
    CHECK(w[0] == 1.0f);
    off.p.blendEnabled = 1;
    off.p.blendSeamEnabled = 0;
    CHECK(off.apply(2.0, 1.0f, 1.0f, w) == Approx(2.0f));
}

TEST_CASE("osvBlendSeamApply interpolates the table between column centres and wraps", "[render][seamcarve]") {
    SeamKernel k(0.0, 0.2, 4);
    // Column centres at longitudes -135, -45, 45, 135 degrees; the seam is
    // at +2 degrees in column 3 only.
    k.table[3u * 2u] = static_cast<float>(deg2rad(2.0));
    float lat = 0.0f;
    float hw = 0.0f;
    osvBlendSeamLookup(&k.p, k.table.data(), static_cast<float>(deg2rad(135.0)), &lat, &hw);
    CHECK(rad2deg(lat) == Approx(2.0).margin(1e-4));
    osvBlendSeamLookup(&k.p, k.table.data(), static_cast<float>(deg2rad(90.0)), &lat, &hw);
    CHECK(rad2deg(lat) == Approx(1.0).margin(1e-4));  // half way from column 2
    osvBlendSeamLookup(&k.p, k.table.data(), static_cast<float>(deg2rad(180.0)), &lat, &hw);
    CHECK(rad2deg(lat) == Approx(1.0).margin(1e-4));  // half way to column 0, across the wrap
    osvBlendSeamLookup(&k.p, k.table.data(), static_cast<float>(deg2rad(-180.0)), &lat, &hw);
    CHECK(rad2deg(lat) == Approx(1.0).margin(1e-4));
    CHECK(rad2deg(hw) == Approx(0.2).margin(1e-5));
}

TEST_CASE("RenderParamsBuilder refuses a half-configured blend seam", "[render][seamcarve]") {
    auto rig = makeCalibratedRig(600);
    REQUIRE(rig.ok());
    geom::VirtualCamera cam;
    cam.w = 64;
    cam.h = 36;
    render::RenderParamsBuilder b;
    b.rig(rig.value())
        .camera(cam)
        .color(color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f));
    const std::vector<float> good = {0.0f, 0.01f, 0.01f, 0.01f, -0.01f, 0.02f};

    const auto enabledWith = [&](const std::vector<float>& table, std::uint32_t columns, float edge) {
        b.blendSeam(good, 3, 0.01f);  // start from a configured seam every time
        b.blendSeam(table, columns, edge);
        auto p = b.buildParams();
        REQUIRE(p.ok());
        return p.value();
    };
    // A good table configures the kernel fields.
    const OsvRenderParams on = enabledWith(good, 3, 0.01f);
    CHECK(on.blendSeamEnabled == 1);
    CHECK(on.blendSeamColumns == 3);
    CHECK(on.blendSeamEdgeRad == 0.01f);
    // Every malformed one DISABLES the seam rather than half-setting it.
    std::vector<float> shortTable = good;
    shortTable.pop_back();
    CHECK(enabledWith(shortTable, 3, 0.01f).blendSeamEnabled == 0);
    std::vector<float> nanTable = good;
    nanTable[2] = std::numeric_limits<float>::quiet_NaN();
    CHECK(enabledWith(nanTable, 3, 0.01f).blendSeamEnabled == 0);
    std::vector<float> wideTable = good;
    wideTable[1] = 3.0f;  // a "feather" wider than a quarter turn
    CHECK(enabledWith(wideTable, 3, 0.01f).blendSeamEnabled == 0);
    CHECK(enabledWith(good, 3, -1.0f).blendSeamEnabled == 0);  // negative edge ramp
    CHECK(enabledWith(good, 0, 0.01f).blendSeamEnabled == 0);
    // Cleared explicitly: every field back to zero, i.e. the pre-seam block.
    b.blendSeam(good, 3, 0.01f);
    b.clearBlendSeam();
    auto cleared = b.buildParams();
    REQUIRE(cleared.ok());
    CHECK(cleared.value().blendSeamEnabled == 0);
    CHECK(cleared.value().blendSeamColumns == 0);
    CHECK(cleared.value().blendSeamEdgeRad == 0.0f);

    // RenderJob::valid() is the renderers' gate: a table that does not
    // match its column count never reaches a kernel.
    render::RenderJob job;
    job.params.outW = 8;
    job.params.outH = 8;
    static const osv_u16 px[4] = {0, 0, 0, 0};
    for (OsvPlane& plane : job.planes) {
        plane.y = plane.u = plane.v = px;
        plane.w = plane.h = plane.cw = plane.ch = 1;
    }
    REQUIRE(job.valid());
    job.params.blendSeamEnabled = 1;
    job.params.blendSeamColumns = 3;
    CHECK_FALSE(job.valid());
    job.blendSeam = good;
    CHECK(job.valid());
    job.blendSeam.pop_back();
    CHECK_FALSE(job.valid());
}

// ===========================================================================
//  4. The sample clip
// ===========================================================================

TEST_CASE("on the sample clip the carved seam removes the doubled fin without drawing a cut",
          "[render][seamcarve][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSample();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    auto pair = reader.value().read(0);
    REQUIRE(pair.ok());
    ThreadPool pool;
    const geom::BlendParams blend;

    // The importer's default correction: the frame's parallax grid.
    render::ParallaxWarpParams pw;
    pw.backend = render::FlowBackendKind::Classical;
    auto grid = render::buildParallaxWarp(clip.value().rig, pair.value(), blend, pw, nullptr, pool);
    REQUIRE(grid.ok());
    render::WarpGridView view{grid.value().uv.data(), grid.value().w, grid.value().h, grid.value().latMinRad,
                              grid.value().latMaxRad};
    render::SeamCorrection corr;
    corr.warp = &view;

    render::SeamCarveParams params;
    auto seam = render::carveSeam(clip.value().rig, pair.value(), blend, pw.band, corr, params, nullptr, pool);
    REQUIRE(seam.ok());
    const render::BlendSeam& s = seam.value();
    INFO("seam: latitude mean " << s.meanLatDeg << " / max " << s.maxAbsLatDeg << " deg, feather mean "
                                << s.meanHalfWidthDeg << " deg, " << s.narrowColumns << " narrow, " << s.forcedColumns
                                << " forced, carved in " << s.carveMs << " ms");
    CHECK(s.forcedColumns == 0);
    CHECK(std::fabs(s.meanLatDeg) < 2.0);
    CHECK(s.maxAbsLatDeg < 6.0);

    // Render the polar-axis map: feather, an uncarved narrow seam on the
    // geometric seam, the carved seam, and each lens alone.
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f);
    render::CpuRenderer cpu(pool);
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = 2048;
    map.h = 1024;
    const auto renderWith = [&](const render::BlendSeam* bs, int onlyLens) {
        render::RenderParamsBuilder b;
        b.rig(clip.value().rig).color(cp).blend(blend, true).equirect(map);
        b.warp(grid.value().uv, grid.value().w, grid.value().h, grid.value().latMinRad, grid.value().latMaxRad);
        if (bs) {
            render::applyBlendSeam(b, *bs);
        }
        if (onlyLens >= 0) {
            b.lensEnabled(1 - onlyLens, false);
        }
        auto job = b.build(pair.value());
        REQUIRE(job.ok());
        auto img = cpu.render(job.value());
        REQUIRE(img.ok());
        return std::move(img).value();
    };
    render::BlendSeam fixed = s;
    for (std::uint32_t c = 0; c < fixed.columns; ++c) {
        fixed.table[c * 2u] = 0.0f;
        fixed.table[c * 2u + 1u] = static_cast<float>(deg2rad(params.narrowHalfWidthDeg));
    }
    const render::ImageRGBAf feather = renderWith(nullptr, -1);
    const render::ImageRGBAf narrow = renderWith(&fixed, -1);
    const render::ImageRGBAf carved = renderWith(&s, -1);
    const render::ImageRGBAf s0 = renderWith(nullptr, 0);
    const render::ImageRGBAf s1 = renderWith(nullptr, 1);

    // Outside the overlap nothing may change (|lat| > 9.5 degrees).
    const int guard = static_cast<int>(std::lround(9.5 / 180.0 * map.h));
    std::size_t changedOutside = 0;
    for (int y = 0; y < map.h; ++y) {
        if (std::abs(y - map.h / 2) <= guard) {
            continue;
        }
        for (int x = 0; x < map.w; ++x) {
            const float* a = feather.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const float* c = carved.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            if (std::memcmp(a, c, 4 * sizeof(float)) != 0) {
                ++changedOutside;
            }
        }
    }
    CHECK(changedOutside == 0);

    // The wing tip and its fin: polar-map longitudes ~136 .. -159 degrees.
    const int wingX0 = map.w * 1800 / 2048;
    const int wingX1 = map.w * 120 / 2048;
    const Measures fBand = measure(feather, s0, s1, -1, -1);
    const Measures nBand = measure(narrow, s0, s1, -1, -1);
    const Measures cBand = measure(carved, s0, s1, -1, -1);
    const Measures fWing = measure(feather, s0, s1, wingX0, wingX1);
    const Measures nWing = measure(narrow, s0, s1, wingX0, wingX1);
    const Measures cWing = measure(carved, s0, s1, wingX0, wingX1);
    INFO("band ghost / edge: feather " << fBand.ghost << " / " << fBand.edge << ", narrow " << nBand.ghost << " / "
                                       << nBand.edge << ", carved " << cBand.ghost << " / " << cBand.edge);
    INFO("wing ghost / edge: feather " << fWing.ghost << " / " << fWing.edge << ", narrow " << nWing.ghost << " / "
                                       << nWing.edge << ", carved " << cWing.ghost << " / " << cWing.edge);
    // The doubled fin: disagreeing content shown at partial strength.
    CHECK(cWing.ghost < 0.25 * fWing.ghost);
    CHECK(cBand.ghost < 0.25 * fBand.ghost);
    // Carving beats merely narrowing: fewer cut edges, no more ghosting at
    // the wing.
    CHECK(cBand.edge < nBand.edge);
    CHECK(cWing.edge < nWing.edge);
    CHECK(cWing.ghost <= nWing.ghost);

    SECTION("the next bucket is held within the clamp of this one") {
        auto next = reader.value().read(render::kParallaxBucketFrames);
        REQUIRE(next.ok());
        auto nextSeam = render::carveSeam(clip.value().rig, next.value(), blend, pw.band, corr, params, &s, pool);
        REQUIRE(nextSeam.ok());
        CHECK(nextSeam.value().usedPrior);
        CHECK(nextSeam.value().maxPriorStepDeg <= params.temporalClampDeg + 1e-6);
    }
}
