// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests of the lens-protector field-angle correction:
//
//   * the fitted curve g(theta): measured anchors, monotonicity and the
//     inverse round trip;
//   * the fold into a Kannala-Brandt lens: exact composition to a fraction
//     of a pixel, a monotonic model, the usable FOV following the image
//     circle, "none" leaving everything bit-identical;
//   * the runtime direction check on synthetic frames where the truth is
//     known (rendered through a rig with the correction already in it), and
//     on the bare-lens sample clip, where it must switch the correction off.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/LensProtector.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/LensProtectorCheck.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::geom;
using Catch::Matchers::WithinAbs;

namespace {

/// A calibration record with the sample clip's numbers for one lens
/// (published in tests/golden/sample_probe.json), no occlusion arc.
meta::DewarpParams sampleLens(bool master) {
    meta::DewarpParams d;
    if (!master) {
        d.fx = 1043.8802f;
        d.fy = 1043.6731f;
        d.cx = 1917.0421f;
        d.cy = 1919.1294f;
        d.k = {0.0667397f, -0.0128859f, 0.0103815f, -0.0067758f, 0.0009879f, 0.0f, 0.0f, 0.0f, 0.0f};
        d.camExtriQ = {0.0026615f, 0.0019091f, -0.7056412f, 0.7085618f, true};
    } else {
        d.fx = 1043.0103f;
        d.fy = 1042.9268f;
        d.cx = 1908.8036f;
        d.cy = 1918.7257f;
        d.k = {0.0613421f, -0.0048016f, 0.0044429f, -0.0045263f, 0.0006621f, 0.0f, 0.0f, 0.0f, 0.0f};
        d.camExtriQ = {0.7036960f, 0.7103991f, -0.0046943f, -0.0110939f, true};
    }
    d.width = 3840;
    d.height = 3840;
    return d;
}

/// The sample's rig scaled to a `side` x `side` stream (per-lens focal from
/// the calibration, so no digital focal length is needed).
LensRig sampleRig(int side) {
    meta::CalibrationSet set;
    set.slave = sampleLens(false);
    set.master = sampleLens(true);
    set.sourceSlave = "native_refine_slave";
    set.sourceMaster = "native_refine_master";
    Result<StreamScaling> scaling = StreamScaling::derive(side, side, 3840, 3840, 0.0, 1043.0,
                                                          static_cast<double>(side) / StreamScaling::kVerifiedCropWidth6K);
    REQUIRE(scaling.ok());
    Result<LensRig> rig = LensRig::build(set, scaling.value(), FocalSource::ScaledCalibration, 0.0,
                                         ExtrinsicConvention{}, 195.18);
    REQUIRE(rig.ok());
    return std::move(rig).value();
}

/// A smooth, structured test world: a sum of plane waves on the sphere with
/// periods between ~6 and ~40 degrees, so a 1..3 degree misregistration
/// between the lenses is plainly measurable.  Deterministic.
struct World {
    std::vector<Vec3d> dirs;
    std::vector<double> phase;
    World() {
        std::mt19937 rng(20260923u);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        std::uniform_real_distribution<double> freq(9.0, 55.0);
        std::uniform_real_distribution<double> ph(0.0, 6.283185307179586);
        for (int i = 0; i < 24; ++i) {
            Vec3d v{u(rng), u(rng), u(rng)};
            const double n = v.norm();
            if (n < 1e-3) {
                continue;
            }
            dirs.push_back(v * (freq(rng) / n));
            phase.push_back(ph(rng));
        }
    }
    /// Brightness in [0, 1] for a unit body direction.
    [[nodiscard]] double operator()(const Vec3d& d) const {
        if (dirs.empty()) {
            return 0.5;  // A flat world.
        }
        double s = 0.0;
        for (std::size_t i = 0; i < dirs.size(); ++i) {
            s += std::sin(dirs[i].dot(d) + phase[i]);
        }
        return 0.5 + 0.5 * s / static_cast<double>(dirs.size()) * 2.2;
    }
};

/// Frame storage with the planes' lifetime tied to the frame.
struct Planes {
    std::vector<std::uint16_t> y, cb, cr;
};

/// Render what lens `i` of `truth` sees of `world`: every pixel is
/// unprojected through the TRUE lens model, so a rig with a protector
/// correction folded in produces exactly the pixels a protector would.
video::PlanarFrame16 renderLens(const LensRig& truth, int i, const World& world, ThreadPool& pool) {
    const auto w = static_cast<std::uint32_t>(truth.streamW);
    const auto h = static_cast<std::uint32_t>(truth.streamH);
    auto planes = std::make_shared<Planes>();
    planes->y.assign(static_cast<std::size_t>(w) * h, 512);
    planes->cb.assign(static_cast<std::size_t>(w / 2) * (h / 2), 512);
    planes->cr.assign(static_cast<std::size_t>(w / 2) * (h / 2), 512);
    const KannalaBrandt5& lens = truth.lens[static_cast<std::size_t>(i)];
    const Mat3d lensToBody = truth.bodyToLens[static_cast<std::size_t>(i)].transposed();
    const Status st = pool.parallelRows(h, 8, [&](std::size_t row) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const Vec2d px{static_cast<double>(x) + 0.5, static_cast<double>(row) + 0.5};
            const Result<Vec3d> dLens = lens.unproject(px);
            if (!dLens.ok()) {
                continue;
            }
            const double t = std::clamp(world(lensToBody * dLens.value()), 0.0, 1.0);
            // Narrow-range 10-bit luma, 64..940.
            planes->y[row * w + x] = static_cast<std::uint16_t>(std::lround(64.0 + 876.0 * t));
        }
    });
    REQUIRE(st.ok());
    video::PlanarFrame16 f;
    f.width = w;
    f.height = h;
    f.chromaW = w / 2;
    f.chromaH = h / 2;
    f.plane = {planes->y.data(), planes->cb.data(), planes->cr.data()};
    f.strideElems = {w, w / 2, w / 2};
    f.bitDepth = 10;
    f.bitShift = 0;
    f.chromaInterleaved = false;
    f.narrowRange = true;
    f.owner = planes;
    return f;
}

/// Both lenses of `truth`.
video::FramePair renderPair(const LensRig& truth, const World& world, ThreadPool& pool) {
    video::FramePair pair;
    pair.lens[0] = renderLens(truth, 0, world, pool);
    pair.lens[1] = renderLens(truth, 1, world, pool);
    REQUIRE(pair.valid());
    return pair;
}

/// Measured anchor points of the protector table (degrees), a handful read
/// off it to pin the fit; the full table is only compared when the local
/// binary is available.
struct Anchor {
    double theta;
    double g;
};
constexpr Anchor kAnchors[] = {{1.0, 1.024}, {30.0, 30.523}, {60.0, 60.691}, {90.0, 91.278},
                               {95.0, 96.500}, {98.0, 99.656}};

}  // namespace

// -----------------------------------------------------------------------------
//  The curve
// -----------------------------------------------------------------------------

TEST_CASE("the protector curve matches the measured anchors and is monotonic", "[geom][protector]") {
    for (const Anchor& a : kAnchors) {
        INFO("theta " << a.theta);
        CHECK_THAT(rad2deg(lensProtectorAngle(deg2rad(a.theta))), WithinAbs(a.g, kLensProtectorFitResidualDeg + 2e-3));
    }
    // Odd, through the origin.
    CHECK(lensProtectorAngle(0.0) == 0.0);
    CHECK_THAT(lensProtectorAngle(-0.7), WithinAbs(-lensProtectorAngle(0.7), 1e-15));
    CHECK(std::isnan(lensProtectorAngle(std::nan(""))));

    // Monotonic (g' > 1: it always adds angle) over the whole range used.
    double minSlope = 10.0;
    for (int i = 0; i <= 1100; ++i) {
        minSlope = std::min(minSlope, lensProtectorAngleDerivative(deg2rad(0.1 * i)));
    }
    CHECK(minSlope > 1.003);
    // Size of the effect where the seam is: ~1.3 deg at 90 deg per lens.
    CHECK_THAT(rad2deg(lensProtectorAngle(deg2rad(90.0))) - 90.0, WithinAbs(1.278, 0.015));
}

TEST_CASE("the protector curve inverts exactly and rejects nonsense", "[geom][protector]") {
    for (int i = -1100; i <= 1100; i += 7) {
        const double t = deg2rad(0.1 * i);
        const Result<double> back = lensProtectorAngleInverse(lensProtectorAngle(t));
        REQUIRE(back.ok());
        CHECK_THAT(back.value(), WithinAbs(t, 1e-12));
    }
    CHECK(lensProtectorAngleInverse(std::nan("")).code() == ErrorCode::InvalidArgument);
    CHECK(lensProtectorAngleInverse(deg2rad(170.0)).code() == ErrorCode::InvalidArgument);
    CHECK(std::string(protectorDirectionName(ProtectorDirection::Forward)) == "forward");
}

// -----------------------------------------------------------------------------
//  Folding into a lens / rig
// -----------------------------------------------------------------------------

TEST_CASE("folding the curve into a lens reproduces the exact composition", "[geom][protector]") {
    const LensRig rig = sampleRig(3000);
    for (int i = 0; i < 2; ++i) {
        const KannalaBrandt5& bare = rig.lens[static_cast<std::size_t>(i)];
        INFO("lens " << i);

        // None is bit for bit the input.
        {
            const Result<ProtectorFold> f = foldLensProtector(bare, ProtectorDirection::None);
            REQUIRE(f.ok());
            CHECK(std::memcmp(&f.value().lens, &bare, sizeof(bare)) == 0);
            CHECK(f.value().maxResidualPx == 0.0);
        }

        for (const ProtectorDirection dir : {ProtectorDirection::Forward, ProtectorDirection::Inverse}) {
            const Result<ProtectorFold> f = foldLensProtector(bare, dir);
            REQUIRE(f.ok());
            const KannalaBrandt5& folded = f.value().lens;
            INFO(protectorDirectionName(dir) << ": residual " << f.value().maxResidualPx << " px, thetaMax "
                                             << rad2deg(folded.thetaMaxRad) << " deg");
            CHECK(f.value().maxResidualPx < 0.05);
            CHECK(folded.isMonotonic(folded.thetaMaxRad));

            // The image circle stays where it is in pixels ...
            CHECK_THAT(folded.rMaxPx, WithinAbs(bare.rMaxPx, 0.05));
            // ... so the world FOV moves by the curve at the rim.
            const double expectedMax =
                dir == ProtectorDirection::Forward ? lensProtectorAngleInverse(bare.thetaMaxRad).value()
                                                   : lensProtectorAngle(bare.thetaMaxRad);
            CHECK_THAT(folded.thetaMaxRad, WithinAbs(expectedMax, 1e-12));

            // Projection round trip: a world ray at theta through the folded
            // lens lands where the bare lens puts the ray at h(theta).
            double worst = 0.0;
            for (int deg = 1; deg <= 95; deg += 2) {
                for (const double az : {0.3, 1.9, 4.0}) {
                    const double t = deg2rad(deg);
                    const double h = dir == ProtectorDirection::Forward ? lensProtectorAngle(t)
                                                                        : lensProtectorAngleInverse(t).value();
                    const Vec3d world{std::sin(t) * std::cos(az), std::sin(t) * std::sin(az), std::cos(t)};
                    const Vec3d bent{std::sin(h) * std::cos(az), std::sin(h) * std::sin(az), std::cos(h)};
                    Vec2d a{}, b{};
                    double ta = 0.0, tb = 0.0;
                    KannalaBrandt5 wide = bare;
                    wide.thetaMaxRad = deg2rad(110.0);  // do not clip the bent ray
                    if (!folded.project(world, a, ta) || !wide.project(bent, b, tb)) {
                        continue;
                    }
                    worst = std::max(worst, (a - b).norm());
                }
            }
            INFO("projection mismatch " << worst << " px");
            CHECK(worst < 0.05);
        }
    }
    // Garbage in.
    KannalaBrandt5 broken;
    CHECK(foldLensProtector(broken, ProtectorDirection::Forward).code() == ErrorCode::InvalidArgument);
}

TEST_CASE("applying the correction to a rig moves the usable FOV and nothing else", "[geom][protector]") {
    const LensRig bare = sampleRig(3000);

    LensRig none = bare;
    const Result<ProtectorRigFold> n = applyLensProtector(none, ProtectorDirection::None, 195.18);
    REQUIRE(n.ok());
    CHECK(n.value().lensFovDeg == 195.18);
    CHECK(std::memcmp(&none.lens, &bare.lens, sizeof(bare.lens)) == 0);

    LensRig fwd = bare;
    const Result<ProtectorRigFold> f = applyLensProtector(fwd, ProtectorDirection::Forward, 195.18);
    REQUIRE(f.ok());
    // 2 * g^-1(97.59 deg): a protector narrows the world FOV by ~1.5 deg per lens.
    CHECK_THAT(f.value().lensFovDeg, WithinAbs(2.0 * rad2deg(lensProtectorAngleInverse(deg2rad(97.59)).value()), 1e-9));
    CHECK(f.value().lensFovDeg < 193.0);
    CHECK(f.value().lensFovDeg > 191.5);
    CHECK(fwd.lensFovDeg == f.value().lensFovDeg);
    // Extrinsics, centres and occlusion are untouched.
    for (int i = 0; i < 2; ++i) {
        CHECK(fwd.lens[static_cast<std::size_t>(i)].cx == bare.lens[static_cast<std::size_t>(i)].cx);
        CHECK(fwd.lens[static_cast<std::size_t>(i)].cy == bare.lens[static_cast<std::size_t>(i)].cy);
        CHECK(std::memcmp(&fwd.bodyToLens[static_cast<std::size_t>(i)], &bare.bodyToLens[static_cast<std::size_t>(i)],
                          sizeof(Mat3d)) == 0);
    }
    CHECK(fwd.notes.back().find("lens protector correction (forward)") != std::string::npos);

    LensRig bad = bare;
    CHECK(applyLensProtector(bad, ProtectorDirection::Forward, std::nan("")).code() == ErrorCode::InvalidArgument);
    CHECK(std::memcmp(&bad.lens, &bare.lens, sizeof(bare.lens)) == 0);  // untouched on failure
}

// -----------------------------------------------------------------------------
//  Direction check
// -----------------------------------------------------------------------------

TEST_CASE("the direction check finds the truth on synthetic frames", "[render][protector]") {
    ThreadPool pool;
    const World world;
    const LensRig base = sampleRig(1024);
    geom::BlendParams blend;
    blend.lensFovDeg = 195.18;
    blend.useOcclusionMask = false;

    auto truthRig = [&base](ProtectorDirection dir) {
        LensRig r = base;
        REQUIRE(applyLensProtector(r, dir, 195.18).ok());
        return r;
    };

    SECTION("footage shot through a protector: forward wins and is kept") {
        const video::FramePair frames = renderPair(truthRig(ProtectorDirection::Forward), world, pool);
        const Result<render::ProtectorScores> s = render::scoreLensProtector(base, blend, frames, pool);
        REQUIRE(s.ok());
        INFO(s.value().summary);
        CHECK(s.value().reliable);
        CHECK(s.value().ncc[1] > s.value().ncc[0] + 0.05);
        CHECK(s.value().ncc[1] > s.value().ncc[2]);
        CHECK(s.value().pick == ProtectorDirection::Forward);
    }

    SECTION("bare-lens footage: the guard switches the correction off") {
        const video::FramePair frames = renderPair(base, world, pool);
        const Result<render::ProtectorScores> s = render::scoreLensProtector(base, blend, frames, pool);
        REQUIRE(s.ok());
        INFO(s.value().summary);
        CHECK(s.value().reliable);
        CHECK(s.value().ncc[0] > s.value().ncc[1] + render::kProtectorClearlyWorse);
        CHECK(s.value().ncc[0] > s.value().ncc[2]);
        CHECK(s.value().pick == ProtectorDirection::None);
    }

    SECTION("footage bent the other way: inverse scores best, forward is refused") {
        const video::FramePair frames = renderPair(truthRig(ProtectorDirection::Inverse), world, pool);
        const Result<render::ProtectorScores> s = render::scoreLensProtector(base, blend, frames, pool);
        REQUIRE(s.ok());
        INFO(s.value().summary);
        CHECK(s.value().ncc[2] > s.value().ncc[0]);
        CHECK(s.value().ncc[2] > s.value().ncc[1]);
        // The guard only decides between the preferred direction and none.
        CHECK(s.value().pick == ProtectorDirection::None);
    }

    SECTION("a flat frame keeps the preferred direction") {
        World flat;
        flat.dirs.clear();
        flat.phase.clear();
        const video::FramePair frames = renderPair(base, flat, pool);
        const Result<render::ProtectorScores> s = render::scoreLensProtector(base, blend, frames, pool);
        REQUIRE(s.ok());
        CHECK_FALSE(s.value().reliable);
        CHECK(s.value().pick == ProtectorDirection::Forward);
    }

    SECTION("bad input is refused") {
        video::FramePair empty;
        CHECK(render::scoreLensProtector(base, blend, empty, pool).code() == ErrorCode::InvalidArgument);
        const video::FramePair frames = renderPair(base, world, pool);
        CHECK(render::scoreLensProtector(base, blend, frames, pool, ProtectorDirection::None).code() ==
              ErrorCode::InvalidArgument);
    }
}

TEST_CASE("on the bare-lens sample the direction check switches the correction off",
          "[render][protector][sample]") {
    OSV_REQUIRE_SAMPLE();
    Result<OsvFile> file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    Result<meta::MetadataTrack> track = meta::MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    Result<meta::FormatInfo> format = meta::FormatDetector::detect(file.value(), &track.value());
    REQUIRE(format.ok());
    Result<meta::CalibrationSet> set = meta::CalibrationSelector::select(track.value().stream());
    REQUIRE(set.ok());

    // The importer's rig: 6K scaling, focal from digital_focal_length.
    const meta::FormatInfo& f = format.value();
    Result<StreamScaling> scaling =
        StreamScaling::derive(static_cast<int>(f.lensW()), static_cast<int>(f.lensH()), static_cast<int>(f.sensorW),
                              static_cast<int>(f.sensorH), f.digitalFocalLength,
                              0.5 * (set.value().slave.fx + set.value().master.fx));
    REQUIRE(scaling.ok());
    Result<LensRig> rig = LensRig::build(set.value(), scaling.value(), FocalSource::DigitalFocalLength,
                                         f.digitalFocalLength, ExtrinsicConvention{}, 195.18);
    REQUIRE(rig.ok());

    video::DecoderOptions opt;
    opt.hw = video::HwAccel::None;
    Result<video::DualStreamReader> reader = video::DualStreamReader::open(osvtest::sampleOsv(), f, opt);
    REQUIRE(reader.ok());
    Result<video::FramePair> pair = reader.value().read(0);
    REQUIRE(pair.ok());

    ThreadPool pool;
    geom::BlendParams blend;
    const Result<render::ProtectorScores> s = render::scoreLensProtector(rig.value(), blend, pair.value(), pool);
    REQUIRE(s.ok());
    INFO(s.value().summary);
    WARN("sample frame 0: " << s.value().summary);
    CHECK(s.value().reliable);
    // The sample was shot WITHOUT protectors (extri_lens_mode 0): no
    // correction must match best, and forward must lose clearly.
    CHECK(s.value().ncc[0] > s.value().ncc[1] + render::kProtectorClearlyWorse);
    CHECK(s.value().pick == ProtectorDirection::None);
}
