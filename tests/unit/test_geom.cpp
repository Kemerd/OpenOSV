// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for osv_geom (part 1): the five-term Kannala-Brandt lens, stream
// scaling, extrinsic conventions, the lens rig, virtual cameras, equirect
// maps, presets and the reference blend weights.
//
// None of these tests need the sample clip: the two sample-clip calibration
// records are transcribed below (they are also the source of the numpy
// golden tables in tests/golden/lens_tables.json).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/core/Math.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/Extrinsics.h"
#include "osv/geom/KannalaBrandt5.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/Presets.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/meta/Types.h"
#include "osv/render/LensAlign.h"   // applyLensRotation: a fitted rotation folded into a rig
#include "osv/render/MountMask.h"   // Hide Mount Auto: the verdict applied to a rig
#include "osv/render/osv_kernel.h"  // OSV_MAX_OCCLUSION_POINTS, the kernels' vertex budget

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace osv;
using namespace osv::geom;

namespace {

// -----------------------------------------------------------------------------
//  Sample-clip calibration fixtures (native_refine slots 1 and 2)
// -----------------------------------------------------------------------------

/// The 14-point occlusion polygon of the slave lens (calibration px).
constexpr std::array<float, 14> kSlaveOccX = {1920.0f,   317.85f,  518.14f,  753.34f,  1012.5f,  1299.23f, 1604.83f,
                                              1920.0f,   2235.17f, 2540.77f, 2827.5f,  3086.66f, 3321.86f, 3522.15f};
constexpr std::array<float, 14> kSlaveOccY = {3735.0f,   2845.0f,  3096.30f, 3310.37f, 3491.84f, 3625.54f, 3707.43f,
                                              3735.0f,   3707.43f, 3625.54f, 3491.84f, 3310.37f, 3096.30f, 2845.0f};

/// Fill a DewarpParams the way DjmdDecoder would for the sample clip.
meta::DewarpParams makeRecord(float fx, float fy, float cx, float cy, std::array<float, 5> k, float qw, float qx,
                              float qy, float qz, bool withOcclusion) {
    meta::DewarpParams d;
    d.fx = fx;
    d.fy = fy;
    d.cx = cx;
    d.cy = cy;
    for (std::size_t i = 0; i < k.size(); ++i) {
        d.k[i] = k[i];
    }
    d.width = 3840;
    d.height = 3840;
    d.lensModel = 8.0f;
    d.temperature = -1000.0f;
    d.camExtriQ.w = qw;
    d.camExtriQ.x = qx;
    d.camExtriQ.y = qy;
    d.camExtriQ.z = qz;
    d.camExtriQ.present = true;
    d.q = {qw, qx, qy, qz};
    if (withOcclusion) {
        d.occlusionPtX.assign(kSlaveOccX.begin(), kSlaveOccX.end());
        d.occlusionPtY.assign(kSlaveOccY.begin(), kSlaveOccY.end());
    }
    // Mark the fields the decoder would have seen.
    for (int bit : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 15, 21, 24, 25, 28}) {
        d.present.set(static_cast<std::size_t>(bit));
    }
    if (withOcclusion) {
        d.present.set(22);
        d.present.set(23);
    }
    return d;
}

/// Slave lens (video track 1 / stream 0, optical axis -Y body).
meta::DewarpParams slaveRecord() {
    return makeRecord(1043.8802f, 1043.6731f, 1917.0421f, 1919.1294f,
                      {0.0667397f, -0.0128859f, 0.0103815f, -0.00677581f, 0.00098791f}, 0.0026615f, 0.0019091f,
                      -0.7056412f, 0.7085618f, true);
}

/// Master lens (video track 2 / stream 1, optical axis +Y body).  The
/// master's occlusion polygon is not part of the transcribed fixture, so
/// only the slave polygon is exercised by the blend tests.
meta::DewarpParams masterRecord() {
    return makeRecord(1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f,
                      {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f}, 0.7036960f, 0.7103991f,
                      -0.0046943f, -0.0110939f, false);
}

meta::CalibrationSet sampleCalibration() {
    meta::CalibrationSet set;
    set.slave = slaveRecord();
    set.master = masterRecord();
    set.sourceSlave = "native_refine_slave";
    set.sourceMaster = "native_refine_master";
    return set;
}

/// digital_focal_length of the 6K sample clip.
constexpr double kDigitalFocal = 829.3612;

/// The verified 6K scaling.
StreamScaling sampleScaling() {
    auto s = StreamScaling::derive(3000, 3000, 3840, 3840, kDigitalFocal, 1043.445);
    REQUIRE(s.ok());
    return s.value();
}

/// The rig built with the verified conventions.
LensRig sampleRig() {
    auto rig = LensRig::build(sampleCalibration(), sampleScaling(), FocalSource::DigitalFocalLength, kDigitalFocal,
                              ExtrinsicConvention{});
    REQUIRE(rig.ok());
    return rig.value();
}

/// Build a lens from the double-precision coefficients stored in the golden
/// JSON.  DewarpParams holds floats, so a lens built through fromDewarp()
/// carries ~1e-8 relative rounding in k1..k5 - too coarse for the 1e-9
/// table comparison, which is about the polynomial math, not float storage.
KannalaBrandt5 lensFromGolden(const nlohmann::json& golden, const char* name) {
    const auto& j = golden["lenses"][name];
    KannalaBrandt5 lens;
    lens.fx = j["fx"].get<double>();
    lens.fy = j["fy"].get<double>();
    lens.cx = j["cx"].get<double>();
    lens.cy = j["cy"].get<double>();
    REQUIRE(j["k"].size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        lens.k[i] = j["k"][i].get<double>();
    }
    lens.updateRMax();
    REQUIRE(lens.isValid());
    return lens;
}

/// Angle in degrees between two vectors.
double angleDeg(const Vec3d& a, const Vec3d& b) { return rad2deg(a.angleTo(b)); }

/// Frobenius distance of R * R^T from the identity.
double orthoError(const Mat3d& r) { return (r * r.transposed()).distance(Mat3d::identity()); }

}  // namespace

// -----------------------------------------------------------------------------
//  KannalaBrandt5
// -----------------------------------------------------------------------------
TEST_CASE("KannalaBrandt5 polynomial matches the numpy golden tables", "[geom][lens]") {
    const nlohmann::json golden = osvtest::loadGolden("lens_tables.json");
    const auto& thetaDeg = golden["theta_table"]["theta_deg"];
    REQUIRE(thetaDeg.size() == 401);

    const KannalaBrandt5 slave = lensFromGolden(golden, "slave");
    const KannalaBrandt5 master = lensFromGolden(golden, "master");
    REQUIRE(slave.thetaD(0.0) == 0.0);
    REQUIRE(master.thetaD(0.0) == 0.0);
    // The float-storage path agrees with the double coefficients to float
    // precision (this is what LensRig::build actually uses).
    const KannalaBrandt5 slaveF = KannalaBrandt5::fromDewarp(slaveRecord());
    REQUIRE(osvtest::approxRel(slaveF.thetaD(1.0), slave.thetaD(1.0), 1e-6, 0.0));
    REQUIRE(slaveF.thetaD(0.0) == 0.0);

    // Every table row must agree to 1e-9 relative (the golden uses explicit
    // powers, the library uses Horner form - the difference is rounding only).
    for (const auto& [name, lens] : {std::pair{"slave", &slave}, std::pair{"master", &master}}) {
        const auto& table = golden["theta_table"][name];
        REQUIRE(table.size() == thetaDeg.size());
        for (std::size_t i = 0; i < table.size(); ++i) {
            const double theta = deg2rad(thetaDeg[i].get<double>());
            const double expected = table[i].get<double>();
            INFO(name << " row " << i << " theta " << thetaDeg[i].get<double>() << " deg");
            REQUIRE(osvtest::approxRel(lens->thetaD(theta), expected, 1e-9, 1e-15));
        }
    }
}

TEST_CASE("KannalaBrandt5 unproject matches bisection-solved golden targets", "[geom][lens]") {
    const nlohmann::json golden = osvtest::loadGolden("lens_tables.json");
    const KannalaBrandt5 slave = lensFromGolden(golden, "slave");
    const KannalaBrandt5 master = lensFromGolden(golden, "master");

    for (const auto& [name, lens] : {std::pair{"slave", &slave}, std::pair{"master", &master}}) {
        const auto& targets = golden["unproject"][name];
        REQUIRE(targets.size() == 200);
        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto& t = targets[i];
            const Vec2d px{t["px"][0].get<double>(), t["px"][1].get<double>()};
            int iterations = 0;
            const auto dir = lens->unproject(px, &iterations);
            INFO(name << " target " << i);
            REQUIRE(dir.ok());
            REQUIRE(iterations <= 8);
            // The ray angle from the optical axis must match the bisection
            // result, and the direction must match component-wise.
            const double theta = std::atan2(std::hypot(dir.value().x, dir.value().y), dir.value().z);
            REQUIRE_THAT(theta, Catch::Matchers::WithinAbs(t["theta"].get<double>(), 1e-9));
            REQUIRE_THAT(dir.value().x, Catch::Matchers::WithinAbs(t["dir"][0].get<double>(), 1e-9));
            REQUIRE_THAT(dir.value().y, Catch::Matchers::WithinAbs(t["dir"][1].get<double>(), 1e-9));
            REQUIRE_THAT(dir.value().z, Catch::Matchers::WithinAbs(t["dir"][2].get<double>(), 1e-9));
        }
    }
}

TEST_CASE("KannalaBrandt5 project/unproject round trip on 10000 stream pixels", "[geom][lens]") {
    const LensRig rig = sampleRig();
    std::mt19937_64 rng(0x0503u);
    std::uniform_real_distribution<double> radius(0.0, 1550.0);
    std::uniform_real_distribution<double> azimuth(-kPi, kPi);

    for (int lensIndex = 0; lensIndex < kLensCount; ++lensIndex) {
        // r < 1550 stream px reaches ~103 deg, past the 97.59 deg usable FOV
        // at which project() clips, so widen the clip for the pure round trip
        // (the polynomial is monotonic to ~114 deg for both sample lenses).
        KannalaBrandt5 lens = rig.lens[static_cast<std::size_t>(lensIndex)];
        lens.thetaMaxRad = deg2rad(110.0);
        REQUIRE(lens.isValid());

        double worstPx = 0.0;
        int worstIterations = 0;
        for (int n = 0; n < 10000; ++n) {
            const double r = radius(rng);
            const double a = azimuth(rng);
            const Vec2d px{lens.cx + r * std::cos(a), lens.cy + r * std::sin(a)};
            int iterations = 0;
            const auto dir = lens.unproject(px, &iterations);
            REQUIRE(dir.ok());
            worstIterations = std::max(worstIterations, iterations);
            Vec2d back;
            double theta = 0.0;
            REQUIRE(lens.project(dir.value(), back, theta));
            worstPx = std::max(worstPx, (back - px).norm());
        }
        INFO("lens " << lensIndex << " worst round trip " << worstPx << " px, worst iterations " << worstIterations);
        REQUIRE(worstPx < 1e-4);
        REQUIRE(worstIterations <= 8);
    }
}

TEST_CASE("KannalaBrandt5 monotonicity and the four-term trap", "[geom][lens]") {
    const KannalaBrandt5 slave = KannalaBrandt5::fromDewarp(slaveRecord());
    const KannalaBrandt5 master = KannalaBrandt5::fromDewarp(masterRecord());
    REQUIRE(slave.isMonotonic(deg2rad(100.0)));
    REQUIRE(master.isMonotonic(deg2rad(100.0)));

    // A four-term fit (k5 dropped, k4 doubled to compensate) folds back well
    // inside the usable FOV - the classic OpenCV fisheye model is not enough.
    for (const KannalaBrandt5* base : {&slave, &master}) {
        KannalaBrandt5 fourTerm = *base;
        fourTerm.k[3] *= 2.0;
        fourTerm.k[4] = 0.0;
        REQUIRE_FALSE(fourTerm.isMonotonic(deg2rad(100.0)));
        // And the Newton inversion refuses the folded region (the derivative
        // is negative at the seed theta0 = rd) rather than returning a wrong
        // angle.  1.5 x theta_d(60 deg) is ~1.65 rad, well past the fold.
        const double rdFolded = fourTerm.thetaD(deg2rad(60.0)) * 1.5;
        const auto folded = fourTerm.thetaFromThetaD(rdFolded);
        REQUIRE_FALSE(folded.ok());
        REQUIRE(folded.error().code == ErrorCode::Malformed);
    }

    // Garbage inputs are rejected, never propagated.
    REQUIRE_FALSE(slave.thetaFromThetaD(-1.0).ok());
    REQUIRE_FALSE(slave.thetaFromThetaD(std::nan("")).ok());
    REQUIRE(slave.thetaFromThetaD(0.0).ok());
    REQUIRE(slave.thetaFromThetaD(0.0).value() == 0.0);
    REQUIRE_FALSE(slave.isMonotonic(0.0));
    REQUIRE_FALSE(slave.isMonotonic(std::nan("")));
}

TEST_CASE("KannalaBrandt5 project edge cases", "[geom][lens]") {
    const KannalaBrandt5 lens = KannalaBrandt5::fromDewarp(slaveRecord());
    Vec2d px;
    double theta = -1.0;
    // On-axis ray lands on the principal point.
    REQUIRE(lens.project(Vec3d{0.0, 0.0, 1.0}, px, theta));
    REQUIRE_THAT(px.x, Catch::Matchers::WithinAbs(lens.cx, 1e-12));
    REQUIRE_THAT(px.y, Catch::Matchers::WithinAbs(lens.cy, 1e-12));
    REQUIRE_THAT(theta, Catch::Matchers::WithinAbs(0.0, 1e-15));
    // +x lens -> image right, +y lens -> image down.
    REQUIRE(lens.project(Vec3d{0.5, 0.0, 1.0}, px, theta));
    REQUIRE(px.x > lens.cx);
    REQUIRE_THAT(px.y, Catch::Matchers::WithinAbs(lens.cy, 1e-9));
    REQUIRE(lens.project(Vec3d{0.0, 0.5, 1.0}, px, theta));
    REQUIRE(px.y > lens.cy);
    // Backwards ray: theta = 180 deg, beyond the usable FOV.
    REQUIRE_FALSE(lens.project(Vec3d{0.0, 0.0, -1.0}, px, theta));
    REQUIRE_THAT(theta, Catch::Matchers::WithinAbs(kPi, 1e-12));
    // Degenerate inputs.
    REQUIRE_FALSE(lens.project(Vec3d{0.0, 0.0, 0.0}, px, theta));
    REQUIRE_FALSE(lens.project(Vec3d{std::nan(""), 0.0, 1.0}, px, theta));
    REQUIRE_FALSE(lens.unproject(Vec2d{std::nan(""), 0.0}).ok());
    KannalaBrandt5 broken = lens;
    broken.fx = 0.0;
    REQUIRE_FALSE(broken.isValid());
    REQUIRE_FALSE(broken.project(Vec3d{0.0, 0.0, 1.0}, px, theta));
    REQUIRE_FALSE(broken.unproject(Vec2d{lens.cx, lens.cy}).ok());
    REQUIRE(broken.seedTable().empty());
}

TEST_CASE("KannalaBrandt5 seed table is monotonic and spans the usable FOV", "[geom][lens]") {
    const KannalaBrandt5 lens = KannalaBrandt5::fromDewarp(slaveRecord());
    const std::vector<float> table = lens.seedTable(4096);
    REQUIRE(table.size() == 4096);
    REQUIRE(table.front() == 0.0f);
    // Entries never decrease and the last one is theta at rd = theta_d(thetaMax).
    for (std::size_t i = 1; i < table.size(); ++i) {
        REQUIRE(table[i] >= table[i - 1]);
    }
    REQUIRE_THAT(static_cast<double>(table.back()), Catch::Matchers::WithinAbs(lens.thetaMaxRad, 1e-6));
    // The interior matches the double-precision inversion to float precision.
    const double rdMax = lens.seedTableRdMax();
    for (std::size_t i = 0; i < table.size(); i += 97) {
        const double rd = rdMax * static_cast<double>(i) / 4095.0;
        const auto theta = lens.thetaFromThetaD(rd);
        REQUIRE(theta.ok());
        REQUIRE_THAT(static_cast<double>(table[i]), Catch::Matchers::WithinAbs(theta.value(), 1e-6));
    }
    REQUIRE(lens.seedTable(1).empty());
    REQUIRE(lens.rMaxPx > 0.0);
}

// -----------------------------------------------------------------------------
//  StreamScaling
// -----------------------------------------------------------------------------
TEST_CASE("StreamScaling derives the verified 6K crop", "[geom][scaling]") {
    std::vector<std::string> notes;
    const auto s = StreamScaling::derive(3000, 3000, 3840, 3840, kDigitalFocal, 1043.445, std::nullopt, &notes);
    REQUIRE(s.ok());
    REQUIRE_THAT(s.value().scale, Catch::Matchers::WithinAbs(0.794492, 1e-9));
    REQUIRE(s.value().verified);
    REQUIRE_FALSE(notes.empty());
    // 3000 / 3776 is the physical meaning of that number.
    REQUIRE_THAT(3000.0 / 3776.0, Catch::Matchers::WithinAbs(StreamScaling::kVerifiedCropScale6K, 1e-6));

    // Slave principal point in stream pixels (verified values).
    const Vec2d slaveC = s.value().apply(Vec2d{1917.0421, 1919.1294});
    REQUIRE_THAT(slaveC.x, Catch::Matchers::WithinAbs(1497.650, 1e-3));
    REQUIRE_THAT(slaveC.y, Catch::Matchers::WithinAbs(1499.309, 1e-3));
    // The sensor centre maps to the stream centre and the inverse round-trips.
    const Vec2d centre = s.value().apply(Vec2d{1920.0, 1920.0});
    REQUIRE_THAT(centre.x, Catch::Matchers::WithinAbs(1500.0, 1e-12));
    REQUIRE_THAT(centre.y, Catch::Matchers::WithinAbs(1500.0, 1e-12));
    const Vec2d back = s.value().applyInverse(slaveC);
    REQUIRE_THAT(back.x, Catch::Matchers::WithinAbs(1917.0421, 1e-9));
    REQUIRE_THAT(back.y, Catch::Matchers::WithinAbs(1919.1294, 1e-9));
}

TEST_CASE("StreamScaling other rules and error handling", "[geom][scaling]") {
    // Same size: identity, verified.
    auto full = StreamScaling::derive(3840, 3840, 3840, 3840, 0.0, 0.0);
    REQUIRE(full.ok());
    REQUIRE(full.value().scale == 1.0);
    REQUIRE(full.value().verified);

    // LRF half without focal lengths: 1024 / 3776, flagged unverified.
    std::vector<std::string> notes;
    auto lrf = StreamScaling::derive(1024, 1024, 3840, 3840, 0.0, 0.0, std::nullopt, &notes);
    REQUIRE(lrf.ok());
    REQUIRE_THAT(lrf.value().scale, Catch::Matchers::WithinAbs(1024.0 / 3776.0, 1e-12));
    REQUIRE_FALSE(lrf.value().verified);
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].find("unverified") != std::string::npos);

    // LRF half of a 6K-mode clip (its ClipMeta repeats the 6K
    // digital_focal_length): the 3776 px crop, where the 6K sample's LRF
    // overlap NCC peaks.
    auto lrf6k = StreamScaling::derive(1024, 1024, 3840, 3840, kDigitalFocal, 1043.445);
    REQUIRE(lrf6k.ok());
    REQUIRE_THAT(lrf6k.value().scale, Catch::Matchers::WithinAbs(1024.0 / 3776.0, 1e-12));
    // LRF half of an 8K-mode clip (digital_focal_length 1061.5823 against a
    // mean calibration fx of 1040.68): the whole 3840 px frame, where the
    // overlap NCC of two such LRFs peaks (0.92-0.96 against 0.80-0.87).
    std::vector<std::string> notes8k;
    auto lrf8k = StreamScaling::derive(1024, 1024, 3840, 3840, 1061.5823, 1040.6721, std::nullopt, &notes8k);
    REQUIRE(lrf8k.ok());
    REQUIRE_THAT(lrf8k.value().scale, Catch::Matchers::WithinAbs(1024.0 / 3840.0, 1e-12));
    REQUIRE_FALSE(lrf8k.value().verified);
    REQUIRE(notes8k.size() == 1);
    REQUIRE(notes8k[0].find("8K") != std::string::npos);
    // The centre maps to the centre either way.
    REQUIRE(lrf8k.value().dstCx == 512.0);
    REQUIRE(lrf8k.value().srcCx == 1920.0);
    // A ratio outside both families keeps the 6K crop.
    auto lrfOdd = StreamScaling::derive(1024, 1024, 3840, 3840, 600.0, 1040.0);
    REQUIRE(lrfOdd.ok());
    REQUIRE_THAT(lrfOdd.value().scale, Catch::Matchers::WithinAbs(1024.0 / 3776.0, 1e-12));

    // Generic fallback: digital focal / calibration focal, unverified.
    auto fourK = StreamScaling::derive(1920, 1920, 3840, 3840, 520.0, 1040.0);
    REQUIRE(fourK.ok());
    REQUIRE_THAT(fourK.value().scale, Catch::Matchers::WithinAbs(0.5, 1e-12));
    REQUIRE_FALSE(fourK.value().verified);
    REQUIRE(fourK.value().dstCx == 960.0);
    REQUIRE(fourK.value().srcCx == 1920.0);

    // Fallback without focal lengths: the 3776 px crop prior, flagged
    // unverified.  (This used to be an error, and a clip missing its
    // digital_focal_length did not open at all; the next test case covers
    // the prior in full.)
    auto noFocal = StreamScaling::derive(1920, 1920, 3840, 3840, 0.0, 0.0);
    REQUIRE(noFocal.ok());
    REQUIRE_THAT(noFocal.value().scale, Catch::Matchers::WithinAbs(1920.0 / 3776.0, 1e-12));
    REQUIRE_FALSE(noFocal.value().verified);
    // Override wins and is flagged.
    auto forced = StreamScaling::derive(3000, 3000, 3840, 3840, kDigitalFocal, 1043.445, 0.8);
    REQUIRE(forced.ok());
    REQUIRE(forced.value().scale == 0.8);
    REQUIRE_FALSE(forced.value().verified);
    REQUIRE_FALSE(StreamScaling::derive(3000, 3000, 3840, 3840, kDigitalFocal, 1043.445, -1.0).ok());
    // Bad sizes.
    REQUIRE_FALSE(StreamScaling::derive(0, 3000, 3840, 3840, kDigitalFocal, 1043.445).ok());
    REQUIRE_FALSE(StreamScaling::derive(3000, 3000, 3840, -1, kDigitalFocal, 1043.445).ok());
    // Identity helper.
    const StreamScaling id = StreamScaling::identity(100, 50);
    REQUIRE(id.scale == 1.0);
    REQUIRE(id.dstCx == 50.0);
    REQUIRE(id.dstCy == 25.0);
    // Zero scale inverse does not divide by zero.
    StreamScaling zero = id;
    zero.scale = 0.0;
    const Vec2d inv = zero.applyInverse(Vec2d{10.0, 10.0});
    REQUIRE(inv.x == zero.srcCx);
}

namespace {

/// digital_focal_length as the Osmo 360 writes it: 0.2764537 x the full-size
/// clip's lens width.  The two values below are the float32 numbers of the
/// 6K and 8K clips (829.3612 = x3000, 1061.5823 = x3840).
constexpr double kDfl6K = 829.3612060546875;
constexpr double kDfl8K = 1061.582275390625;
/// The same convention for a 1920 px (4K) lens.
constexpr double kDfl4K = kDfl6K / 3000.0 * 1920.0;

/// True when any note contains `needle`.
bool anyNoteHas(const std::vector<std::string>& notes, const char* needle) {
    for (const std::string& n : notes) {
        if (n.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("StreamScaling: a clip without usable focal lengths opens on the 3776 px prior", "[geom][scaling]") {
    // The 4K case (1920 px per lens): no rule keys on its size, so it used to
    // need digital_focal_length / fx and failed without it.
    std::vector<std::string> notes;
    auto s = StreamScaling::derive(1920, 1920, 3840, 3840, 0.0, 0.0, std::nullopt, &notes);
    REQUIRE(s.ok());
    REQUIRE_THAT(s.value().scale, Catch::Matchers::WithinAbs(1920.0 / 3776.0, 1e-12));
    REQUIRE_FALSE(s.value().verified);
    // Centred as every other rule: sensor centre -> stream centre.
    REQUIRE(s.value().dstCx == 960.0);
    REQUIRE(s.value().srcCx == 1920.0);
    // One note, saying it is the unverified prior and why.
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].find("prior") != std::string::npos);
    REQUIRE(notes[0].find("unverified") != std::string::npos);

    // Either focal alone is not enough, and garbage never reaches the scale.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::pair<double, double> unusable[] = {
        {0.0, 1043.445}, {kDfl4K, 0.0}, {nan, 1043.445}, {kDfl4K, nan}, {inf, 1043.445},
        {kDfl4K, inf},   {-5.0, 1043.445}, {kDfl4K, -1.0},
    };
    for (const auto& [dfl, fx] : unusable) {
        INFO("dfl " << dfl << " fx " << fx);
        auto prior = StreamScaling::derive(1920, 1920, 3840, 3840, dfl, fx);
        REQUIRE(prior.ok());
        REQUIRE_THAT(prior.value().scale, Catch::Matchers::WithinAbs(1920.0 / 3776.0, 1e-12));
        REQUIRE_FALSE(prior.value().verified);
    }

    // On a sensor narrower than the 3776 px crop, the prior is the whole
    // sensor instead: never a crop wider than the sensor it is cut from.
    auto narrow = StreamScaling::derive(1000, 1000, 2000, 2000, 0.0, 0.0);
    REQUIRE(narrow.ok());
    REQUIRE_THAT(narrow.value().scale, Catch::Matchers::WithinAbs(0.5, 1e-12));

    // Sizes are still validated first: no prior rescues a zero stream.
    REQUIRE_FALSE(StreamScaling::derive(0, 1920, 3840, 3840, 0.0, 0.0).ok());
    REQUIRE_FALSE(StreamScaling::derive(1920, 1920, 0, 3840, 0.0, 0.0).ok());
}

TEST_CASE("StreamScaling: a proxy never takes its parent's focal", "[geom][scaling]") {
    // An LRF half from a sensor that is not 3840 px wide misses Rule 3 (keyed
    // on the Osmo 360's sensor).  It used to fall to digital_focal_length /
    // fx - the PARENT's scale, ~1.02 for an 8K parent - and so built a lens
    // ~3.7x too long on a 1024 px image.
    const double fxs[] = {1033.4852, 1040.6721, 1043.445, 1047.9333, 1087.0};
    const double dfls[] = {kDfl8K, kDfl6K, kDfl4K, 0.0, std::numeric_limits<double>::quiet_NaN(), 1.0e9, -5.0,
                           kDfl6K / 3000.0 * 4000.0};
    for (const double fx : fxs) {
        for (const double dfl : dfls) {
            INFO("fx " << fx << " dfl " << dfl);
            auto s = StreamScaling::derive(1024, 1024, 4000, 4000, dfl, fx);
            REQUIRE(s.ok());
            REQUIRE(std::isfinite(s.value().scale));
            REQUIRE(s.value().scale > 0.0);
            REQUIRE(s.value().scale <= 0.3);
            REQUIRE_FALSE(s.value().verified);
            REQUIRE(s.value().dstCx == 512.0);
            REQUIRE(s.value().srcCx == 2000.0);
        }
    }

    // The exact value: the proxy takes the scale its 3840 px parent gets
    // (digital_focal_length / fx on this sensor), shrunk by 1024 / 3840.
    std::vector<std::string> notes;
    auto lrf8k = StreamScaling::derive(1024, 1024, 4000, 4000, kDfl8K, 1040.6721, std::nullopt, &notes);
    REQUIRE(lrf8k.ok());
    REQUIRE_THAT(lrf8k.value().scale, Catch::Matchers::WithinAbs(kDfl8K / 1040.6721 * 1024.0 / 3840.0, 1e-12));
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].find("proxy") != std::string::npos);
    REQUIRE(notes[0].find("3840x3840") != std::string::npos);
    // A parent that is the whole sensor maps the proxy as the whole sensor.
    auto fullSensor = StreamScaling::derive(1024, 1024, 4000, 4000, kDfl6K / 3000.0 * 4000.0, 1087.0);
    REQUIRE(fullSensor.ok());
    REQUIRE_THAT(fullSensor.value().scale, Catch::Matchers::WithinAbs(1024.0 / 4000.0, 1e-12));

    // A garbage calibration focal cannot push a proxy past 1024 / 3000.
    std::vector<std::string> cappedNotes;
    auto capped = StreamScaling::derive(1024, 1024, 4000, 4000, kDfl8K, 100.0, std::nullopt, &cappedNotes);
    REQUIRE(capped.ok());
    REQUIRE_THAT(capped.value().scale, Catch::Matchers::WithinAbs(1024.0 / 3000.0, 1e-12));
    REQUIRE(anyNoteHas(cappedNotes, "capped"));

    // On the Osmo 360's own sensor a proxy of another size inherits the
    // verified rules of its parent: the 8K frame at 1:1, the 6K crop.
    auto half8k = StreamScaling::derive(960, 960, 3840, 3840, kDfl8K, 1040.6721);
    REQUIRE(half8k.ok());
    REQUIRE_THAT(half8k.value().scale, Catch::Matchers::WithinAbs(960.0 / 3840.0, 1e-12));
    auto half6k = StreamScaling::derive(960, 960, 3840, 3840, kDfl6K, 1043.445);
    REQUIRE(half6k.ok());
    REQUIRE_THAT(half6k.value().scale,
                 Catch::Matchers::WithinAbs(StreamScaling::kVerifiedCropScale6K * 960.0 / 3000.0, 1e-12));
    REQUIRE_FALSE(half6k.value().verified);
}

TEST_CASE("StreamScaling: the measured modes keep their scales", "[geom][scaling]") {
    // 8K OSV / 8K LRF (the reporter's camera) and 6K OSV / 6K LRF (the
    // sample): the four scales every render of those clips is built on.
    // Each digital_focal_length follows the convention, so no rule adds a
    // second note.
    constexpr double kFx8K = 0.5 * (1033.4852 + 1047.9333);
    constexpr double kFx6K = 1043.445;
    struct Mode {
        const char* name;
        int lensW;
        double dfl;
        double fx;
        double scale;
    };
    const Mode modes[] = {
        {"8K OSV", 3840, kDfl8K, kFx8K, 1.0},
        {"8K LRF", 1024, kDfl8K, kFx8K, 1024.0 / 3840.0},
        {"6K OSV", 3000, kDfl6K, kFx6K, StreamScaling::kVerifiedCropScale6K},
        {"6K LRF", 1024, kDfl6K, kFx6K, 1024.0 / 3776.0},
    };
    for (const Mode& m : modes) {
        INFO(m.name);
        std::vector<std::string> notes;
        auto s = StreamScaling::derive(m.lensW, m.lensW, 3840, 3840, m.dfl, m.fx, std::nullopt, &notes);
        REQUIRE(s.ok());
        REQUIRE(s.value().scale == m.scale);
        REQUIRE(notes.size() == 1);
        REQUIRE_FALSE(anyNoteHas(notes, "convention"));
    }
    // The brief's rounded figures, for the record.
    REQUIRE_THAT(1024.0 / 3840.0, Catch::Matchers::WithinAbs(0.2666667, 1e-7));
    REQUIRE_THAT(1024.0 / 3776.0, Catch::Matchers::WithinAbs(0.2711864, 1e-7));

    // 4K keeps Rule 4 exactly (digital_focal_length / fx), unchanged.
    auto fourK = StreamScaling::derive(1920, 1920, 3840, 3840, kDfl4K, kFx6K);
    REQUIRE(fourK.ok());
    REQUIRE(fourK.value().scale == kDfl4K / kFx6K);
}

TEST_CASE("StreamScaling: a digital_focal_length off the Osmo 360 convention is logged, never used differently",
          "[geom][scaling]") {
    // 520 px on a 1920 px stream is 2 % off 0.2764537 x 1920 = 530.79.  The
    // scale is still the ratio (Rule 4 unchanged); one extra note says so.
    std::vector<std::string> notes;
    auto s = StreamScaling::derive(1920, 1920, 3840, 3840, 520.0, 1040.0, std::nullopt, &notes);
    REQUIRE(s.ok());
    REQUIRE_THAT(s.value().scale, Catch::Matchers::WithinAbs(0.5, 1e-12));
    REQUIRE(notes.size() == 2);
    REQUIRE(notes[1].find("does not follow the Osmo 360 convention") != std::string::npos);

    // The same check on the full-frame and 6K rules: logged, scale untouched.
    std::vector<std::string> notes8k;
    auto full = StreamScaling::derive(3840, 3840, 3840, 3840, kDfl8K * 1.01, 1040.0, std::nullopt, &notes8k);
    REQUIRE(full.ok());
    REQUIRE(full.value().scale == 1.0);
    REQUIRE(anyNoteHas(notes8k, "does not follow"));
    std::vector<std::string> notes6k;
    auto sixK = StreamScaling::derive(3000, 3000, 3840, 3840, kDfl6K * 0.99, 1043.445, std::nullopt, &notes6k);
    REQUIRE(sixK.ok());
    REQUIRE(sixK.value().scale == StreamScaling::kVerifiedCropScale6K);
    REQUIRE(anyNoteHas(notes6k, "does not follow"));
    // Within 0.2 % stays quiet.
    std::vector<std::string> quiet;
    REQUIRE(StreamScaling::derive(3840, 3840, 3840, 3840, kDfl8K * 1.0015, 1040.0, std::nullopt, &quiet).ok());
    REQUIRE_FALSE(anyNoteHas(quiet, "does not follow"));
}

// -----------------------------------------------------------------------------
//  Extrinsics / LensRig
// -----------------------------------------------------------------------------
TEST_CASE("Extrinsics: verified convention puts the lens axes on +/-Y and image-down on -Z", "[geom][extrinsics]") {
    const LensRig rig = sampleRig();
    REQUIRE(rig.streamW == 3000);
    REQUIRE(rig.streamH == 3000);

    // Optical axes: master looks along +Y, slave along -Y.
    REQUIRE(angleDeg(rig.opticalAxisBody(kMasterLens), Vec3d{0.0, 1.0, 0.0}) < 1.5);
    REQUIRE(angleDeg(rig.opticalAxisBody(kSlaveLens), Vec3d{0.0, -1.0, 0.0}) < 1.5);
    // Image-down of both lenses points to -Z (the camera is upright).
    REQUIRE(angleDeg(rig.imageDownBody(kMasterLens), Vec3d{0.0, 0.0, -1.0}) < 1.5);
    REQUIRE(angleDeg(rig.imageDownBody(kSlaveLens), Vec3d{0.0, 0.0, -1.0}) < 1.5);
    // Image-right: +X for the master, -X for the slave (mirror pair).
    REQUIRE(angleDeg(rig.imageRightBody(kMasterLens), Vec3d{1.0, 0.0, 0.0}) < 1.5);
    REQUIRE(angleDeg(rig.imageRightBody(kSlaveLens), Vec3d{-1.0, 0.0, 0.0}) < 1.5);
    // Rotation matrices are proper and orthonormal.
    for (int i = 0; i < kLensCount; ++i) {
        REQUIRE(orthoError(rig.bodyToLens[static_cast<std::size_t>(i)]) < 1e-12);
        REQUIRE_THAT(rig.bodyToLens[static_cast<std::size_t>(i)].determinant(), Catch::Matchers::WithinAbs(1.0, 1e-12));
    }
    // Bad indices are harmless.
    REQUIRE(rig.opticalAxisBody(-1).norm() == 0.0);
    REQUIRE(rig.opticalAxisBody(2).norm() == 0.0);
    REQUIRE_FALSE(LensRig::validIndex(2));
}

TEST_CASE("Extrinsics: the LensToBody reading is wrong for the master lens", "[geom][extrinsics]") {
    // Documents the convention: the master's extrinsic is a ~90 deg rotation
    // so its transpose points the axis the wrong way.  (The slave's is a
    // ~180 deg rotation, which is its own inverse, so it cannot separate the
    // two senses on its own.)
    ExtrinsicConvention wrong;
    wrong.sense = RotationSense::LensToBody;
    auto rig = LensRig::build(sampleCalibration(), sampleScaling(), FocalSource::DigitalFocalLength, kDigitalFocal,
                              wrong);
    REQUIRE(rig.ok());
    REQUIRE(angleDeg(rig.value().opticalAxisBody(kMasterLens), Vec3d{0.0, 1.0, 0.0}) > 80.0);
    // The XYZW reading is also off for both lenses.
    ExtrinsicConvention wrongOrder;
    wrongOrder.order = QuatOrder::XYZW;
    auto rig2 = LensRig::build(sampleCalibration(), sampleScaling(), FocalSource::DigitalFocalLength, kDigitalFocal,
                               wrongOrder);
    REQUIRE(rig2.ok());
    REQUIRE(angleDeg(rig2.value().opticalAxisBody(kMasterLens), Vec3d{0.0, 1.0, 0.0}) > 80.0);
    REQUIRE(angleDeg(rig2.value().opticalAxisBody(kSlaveLens), Vec3d{0.0, -1.0, 0.0}) > 80.0);
    // Absent quaternion -> identity, never garbage.
    meta::Quaternion absent;
    REQUIRE(bodyToLensMatrix(absent, ExtrinsicConvention{}).distance(Mat3d::identity()) == 0.0);
    REQUIRE(std::string(quatOrderName(QuatOrder::WXYZ)) == "WXYZ");
    REQUIRE(std::string(rotationSenseName(RotationSense::LensToBody)) == "LensToBody");
}

TEST_CASE("LensRig: stream-space intrinsics and build failures", "[geom][rig]") {
    const LensRig rig = sampleRig();
    const KannalaBrandt5& slave = rig.lens[kSlaveLens];
    // Focal from digital_focal_length, principal point through the scaling.
    REQUIRE_THAT(slave.fx, Catch::Matchers::WithinAbs(kDigitalFocal, 1e-9));
    REQUIRE_THAT(slave.fy, Catch::Matchers::WithinAbs(kDigitalFocal, 1e-9));
    REQUIRE_THAT(slave.cx, Catch::Matchers::WithinAbs(1497.650, 1e-3));
    REQUIRE_THAT(slave.cy, Catch::Matchers::WithinAbs(1499.309, 1e-3));
    REQUIRE_THAT(slave.thetaMaxRad, Catch::Matchers::WithinAbs(deg2rad(97.59), 1e-12));
    // The scaled calibration focal is within a pixel of digital_focal_length
    // (the relationship that verified the crop scale in the first place).
    REQUIRE_THAT(0.5 * (1043.8802 + 1043.6731) * 0.794492, Catch::Matchers::WithinAbs(kDigitalFocal, 1.0));
    // Occlusion polygon mapped to stream px.  The metadata stores an OPEN arc
    // of 14 entries (13 distinct - the apex is repeated); buildOcclusion drops
    // the duplicate and closes the arc outward to the rim, so the closed
    // polygon has 13 arc vertices plus 13 rim vertices.  See buildOcclusion:
    // using the stored points directly gives a self-intersecting bowtie.
    REQUIRE(rig.occlusionPolyStream[kSlaveLens].size() == 26);
    REQUIRE(rig.occlusionPolyStream[kMasterLens].empty());
    for (const Vec2d& p : rig.occlusionPolyStream[kSlaveLens]) {
        REQUIRE(p.y > slave.cy);
    }
    REQUIRE_FALSE(rig.notes.empty());

    // On the 6K sample digital_focal_length sits within 0.1 % of both lenses'
    // calibration * scale, so both lenses take it (the verified behaviour).
    REQUIRE_THAT(rig.lens[kMasterLens].fx, Catch::Matchers::WithinAbs(kDigitalFocal, 1e-9));

    // ScaledCalibration focal source.
    auto scaled = LensRig::build(sampleCalibration(), sampleScaling(), FocalSource::ScaledCalibration, 0.0,
                                 ExtrinsicConvention{});
    REQUIRE(scaled.ok());
    // (The calibration record stores floats, hence the float casts.)
    REQUIRE_THAT(scaled.value().lens[kSlaveLens].fx,
                 Catch::Matchers::WithinAbs(static_cast<double>(1043.8802f) * 0.794492, 1e-9));
    REQUIRE_THAT(scaled.value().lens[kSlaveLens].fy,
                 Catch::Matchers::WithinAbs(static_cast<double>(1043.6731f) * 0.794492, 1e-9));

    // projectBody: the master axis lands on the master principal point.
    Vec2d px;
    double theta = 0.0;
    REQUIRE(rig.projectBody(kMasterLens, rig.opticalAxisBody(kMasterLens), px, theta));
    REQUIRE_THAT(px.x, Catch::Matchers::WithinAbs(rig.lens[kMasterLens].cx, 1e-9));
    REQUIRE_THAT(theta, Catch::Matchers::WithinAbs(0.0, 1e-12));
    REQUIRE_FALSE(rig.projectBody(kMasterLens, rig.opticalAxisBody(kSlaveLens), px, theta));
    REQUIRE_FALSE(rig.projectBody(5, Vec3d{0.0, 1.0, 0.0}, px, theta));

    // Build failures: a record without its core fields, a bad FOV, bad scaling.
    meta::CalibrationSet broken = sampleCalibration();
    broken.master.fx = 0.0;
    REQUIRE_FALSE(LensRig::build(broken, sampleScaling(), FocalSource::DigitalFocalLength, kDigitalFocal,
                                 ExtrinsicConvention{})
                      .ok());
    REQUIRE_FALSE(LensRig::build(sampleCalibration(), sampleScaling(), FocalSource::DigitalFocalLength, kDigitalFocal,
                                 ExtrinsicConvention{}, 0.0)
                      .ok());
    StreamScaling badScale = sampleScaling();
    badScale.dstCx = 0.0;
    REQUIRE_FALSE(LensRig::build(sampleCalibration(), badScale, FocalSource::DigitalFocalLength, kDigitalFocal,
                                 ExtrinsicConvention{})
                      .ok());
}

TEST_CASE("LensRig: an 8K-mode digital_focal_length that misses the lenses' calibration is not used",
          "[geom][rig]") {
    // An 8K-mode camera (stream = the whole 3840 px frame, scale 1.0): its
    // digital_focal_length of 1061.5823 px is 2.7 % and 1.3 % longer than the
    // two lenses' calibrated focals.  Taking it misregistered every depth in
    // the overlap by 3-4 deg; the per-lens calibration aligns it.
    meta::CalibrationSet set = sampleCalibration();
    set.slave.fx = 1033.4852f;
    set.slave.fy = 1033.3813f;
    set.master.fx = 1047.9333f;
    set.master.fy = 1047.8409f;
    constexpr double kDigitalFocal8K = 1061.5823;
    auto scaling = StreamScaling::derive(3840, 3840, 3840, 3840, kDigitalFocal8K,
                                         0.5 * (set.slave.fx + set.master.fx));
    REQUIRE(scaling.ok());
    REQUIRE(scaling.value().scale == 1.0);
    auto rig = LensRig::build(set, scaling.value(), FocalSource::DigitalFocalLength, kDigitalFocal8K,
                              ExtrinsicConvention{});
    REQUIRE(rig.ok());
    // Each lens keeps its own calibrated focal (records are floats).
    REQUIRE_THAT(rig.value().lens[kSlaveLens].fx, Catch::Matchers::WithinAbs(static_cast<double>(set.slave.fx), 1e-9));
    REQUIRE_THAT(rig.value().lens[kSlaveLens].fy, Catch::Matchers::WithinAbs(static_cast<double>(set.slave.fy), 1e-9));
    REQUIRE_THAT(rig.value().lens[kMasterLens].fx,
                 Catch::Matchers::WithinAbs(static_cast<double>(set.master.fx), 1e-9));
    // The notes say why.
    bool explained = false;
    for (const std::string& note : rig.value().notes) {
        explained = explained || note.find("using the calibrated focal") != std::string::npos;
    }
    REQUIRE(explained);

    // A value within half a percent of a lens is still taken for that lens.
    const double nearSlave = 0.5 * (static_cast<double>(set.slave.fx) + set.slave.fy) * 1.004;
    auto near = LensRig::build(set, scaling.value(), FocalSource::DigitalFocalLength, nearSlave,
                               ExtrinsicConvention{});
    REQUIRE(near.ok());
    REQUIRE_THAT(near.value().lens[kSlaveLens].fx, Catch::Matchers::WithinAbs(nearSlave, 1e-9));
    // ...while the master, 1.0 % away from that value, keeps its calibration.
    REQUIRE_THAT(near.value().lens[kMasterLens].fx,
                 Catch::Matchers::WithinAbs(static_cast<double>(set.master.fx), 1e-9));
}

TEST_CASE("LensRig: Lens Focal Camera trusts digital_focal_length for this stream size, Calibration never",
          "[geom][rig]") {
    // Source Settings "Lens Focal".  Camera is the rule before the 8K-mode
    // measurements: the recorded value whenever it is within 1.2x of each
    // lens's calibration * scale.  Calibration takes each lens's own always.
    meta::CalibrationSet set = sampleCalibration();
    set.slave.fx = 1033.4852f;
    set.slave.fy = 1033.3813f;
    set.master.fx = 1047.9333f;
    set.master.fy = 1047.8409f;
    constexpr double kDigitalFocal8K = 1061.5823;
    auto scaling = StreamScaling::derive(3840, 3840, 3840, 3840, kDigitalFocal8K,
                                         0.5 * (set.slave.fx + set.master.fx));
    REQUIRE(scaling.ok());

    // ---- Camera: both lenses take the recorded 1061.58 px (2.7 % / 1.3 % off) ----
    auto camera = LensRig::build(set, scaling.value(), FocalSource::DigitalFocalLengthSameStream, kDigitalFocal8K,
                                 ExtrinsicConvention{});
    REQUIRE(camera.ok());
    REQUIRE(camera.value().focalSource == FocalSource::DigitalFocalLengthSameStream);
    for (const int i : {kSlaveLens, kMasterLens}) {
        REQUIRE_THAT(camera.value().lens[i].fx, Catch::Matchers::WithinAbs(kDigitalFocal8K, 1e-9));
        REQUIRE_THAT(camera.value().lens[i].fy, Catch::Matchers::WithinAbs(kDigitalFocal8K, 1e-9));
    }
    // ...but a value for another stream size (an LRF proxy repeating its
    // clip's) is still refused: off by more than 1.2x.
    auto stale = LensRig::build(set, scaling.value(), FocalSource::DigitalFocalLengthSameStream, kDigitalFocal8K * 1.3,
                                ExtrinsicConvention{});
    REQUIRE(stale.ok());
    REQUIRE_THAT(stale.value().lens[kSlaveLens].fx,
                 Catch::Matchers::WithinAbs(static_cast<double>(set.slave.fx), 1e-9));
    // ...and an unusable value falls back to the calibration as well.
    auto unusable = LensRig::build(set, scaling.value(), FocalSource::DigitalFocalLengthSameStream, 0.0,
                                   ExtrinsicConvention{});
    REQUIRE(unusable.ok());
    REQUIRE_THAT(unusable.value().lens[kMasterLens].fx,
                 Catch::Matchers::WithinAbs(static_cast<double>(set.master.fx), 1e-9));

    // ---- Calibration: each lens's own, even where the recorded value agrees ----------
    const double agreeing = 0.5 * (static_cast<double>(set.slave.fx) + set.slave.fy);
    auto calibration = LensRig::build(set, scaling.value(), FocalSource::ScaledCalibration, agreeing,
                                      ExtrinsicConvention{});
    REQUIRE(calibration.ok());
    REQUIRE_THAT(calibration.value().lens[kSlaveLens].fx,
                 Catch::Matchers::WithinAbs(static_cast<double>(set.slave.fx), 1e-9));
    REQUIRE_THAT(calibration.value().lens[kSlaveLens].fy,
                 Catch::Matchers::WithinAbs(static_cast<double>(set.slave.fy), 1e-9));

    // ---- Auto (DigitalFocalLength) is unchanged: the agreeing value is taken -----------
    auto automatic = LensRig::build(set, scaling.value(), FocalSource::DigitalFocalLength, agreeing,
                                    ExtrinsicConvention{});
    REQUIRE(automatic.ok());
    REQUIRE_THAT(automatic.value().lens[kSlaveLens].fx, Catch::Matchers::WithinAbs(agreeing, 1e-9));
    REQUIRE(std::string(focalSourceName(FocalSource::DigitalFocalLengthSameStream)) ==
            "DigitalFocalLengthSameStream");
}

// -----------------------------------------------------------------------------
//  VirtualCamera
// -----------------------------------------------------------------------------
TEST_CASE("VirtualCamera rectilinear rays and rotation conventions", "[geom][camera]") {
    VirtualCamera cam;
    cam.projection = Projection::Rectilinear;
    cam.w = 1920;
    cam.h = 1080;
    cam.hfovDeg = 120.0;
    REQUIRE(cam.isValid());

    // Centre of the image looks straight down +Y (pixel index W/2 - 0.5 is
    // the exact centre once the 0.5 pixel-centre offset is added).
    Vec3d d;
    REQUIRE(cam.pixelToRay(0.5 * cam.w - 0.5, 0.5 * cam.h - 0.5, d));
    REQUIRE_THAT(d.x, Catch::Matchers::WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(d.y, Catch::Matchers::WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(d.z, Catch::Matchers::WithinAbs(0.0, 1e-12));
    // The right edge of the image is exactly hfov / 2 away from the centre.
    REQUIRE(cam.pixelToRay(cam.w - 0.5, 0.5 * cam.h - 0.5, d));
    REQUIRE_THAT(rad2deg(d.angleTo(Vec3d{0.0, 1.0, 0.0})), Catch::Matchers::WithinAbs(60.0, 1e-9));
    REQUIRE(d.x > 0.0);
    // Up in the image is +Z.
    REQUIRE(cam.pixelToRay(0.5 * cam.w - 0.5, 0.0, d));
    REQUIRE(d.z > 0.0);
    REQUIRE_THAT(d.x, Catch::Matchers::WithinAbs(0.0, 1e-12));
    // focalPx = (W/2) / tan(hfov/2).
    REQUIRE_THAT(cam.focalPx(), Catch::Matchers::WithinAbs(960.0 / std::tan(deg2rad(60.0)), 1e-9));

    // Rotation: positive yaw turns the forward axis towards -X, positive
    // pitch tilts it up, roll leaves it alone.
    cam.yawDeg = 90.0;
    Vec3d fwd = cam.rotation() * Vec3d{0.0, 1.0, 0.0};
    REQUIRE_THAT(fwd.x, Catch::Matchers::WithinAbs(-1.0, 1e-12));
    cam.yawDeg = 0.0;
    cam.pitchDeg = 90.0;
    fwd = cam.rotation() * Vec3d{0.0, 1.0, 0.0};
    REQUIRE_THAT(fwd.z, Catch::Matchers::WithinAbs(1.0, 1e-12));
    cam.pitchDeg = 0.0;
    cam.rollDeg = 30.0;
    cam.correctionAngleDeg = -30.0;
    fwd = cam.rotation() * Vec3d{0.0, 1.0, 0.0};
    REQUIRE_THAT(fwd.y, Catch::Matchers::WithinAbs(1.0, 1e-12));
    REQUIRE(cam.rotation().distance(Mat3d::identity()) < 1e-12);
    REQUIRE(orthoError(cam.rotation()) < 1e-12);

    // Invalid cameras never produce rays.
    VirtualCamera bad = cam;
    bad.hfovDeg = 180.0;
    REQUIRE_FALSE(bad.isValid());
    REQUIRE_FALSE(bad.pixelToRay(0.0, 0.0, d));
    REQUIRE(bad.focalPx() == 0.0);
    bad = cam;
    bad.w = 0;
    REQUIRE_FALSE(bad.isValid());
    REQUIRE_FALSE(cam.pixelToRay(std::nan(""), 0.0, d));
}

TEST_CASE("VirtualCamera stereographic and fisheye radial formulas", "[geom][camera]") {
    VirtualCamera cam;
    cam.w = 1000;
    cam.h = 1000;
    cam.hfovDeg = 240.0;
    cam.projection = Projection::Stereographic;
    REQUIRE(cam.isValid());
    // focalPx = (W/2) / (2 tan(hfov/4)).
    const double f = cam.focalPx();
    REQUIRE_THAT(f, Catch::Matchers::WithinAbs(500.0 / (2.0 * std::tan(deg2rad(60.0))), 1e-9));
    // theta(r) = 2 atan(r / (2 f)) along the +x axis, checked at several radii.
    for (const double r : {50.0, 200.0, 400.0, 499.0}) {
        Vec3d d;
        REQUIRE(cam.pixelToRay(0.5 * cam.w - 0.5 + r, 0.5 * cam.h - 0.5, d));
        const double theta = d.angleTo(Vec3d{0.0, 1.0, 0.0});
        REQUIRE_THAT(theta, Catch::Matchers::WithinAbs(2.0 * std::atan(r / (2.0 * f)), 1e-12));
        REQUIRE(d.x > 0.0);
    }
    // The right edge sits at hfov / 2 = 120 deg.
    Vec3d edge;
    REQUIRE(cam.pixelToRay(cam.w - 0.5, 0.5 * cam.h - 0.5, edge));
    REQUIRE_THAT(rad2deg(edge.angleTo(Vec3d{0.0, 1.0, 0.0})), Catch::Matchers::WithinAbs(120.0, 1e-9));

    // Fisheye: theta = r / f with f = (W/2) / (hfov/2).
    cam.projection = Projection::Fisheye;
    cam.hfovDeg = 180.0;
    const double ff = cam.focalPx();
    REQUIRE_THAT(ff, Catch::Matchers::WithinAbs(500.0 / kHalfPi, 1e-9));
    for (const double r : {10.0, 250.0, 499.0}) {
        Vec3d d;
        REQUIRE(cam.pixelToRay(0.5 * cam.w - 0.5, 0.5 * cam.h - 0.5 - r, d));  // up the image
        REQUIRE_THAT(d.angleTo(Vec3d{0.0, 1.0, 0.0}), Catch::Matchers::WithinAbs(r / ff, 1e-12));
        REQUIRE(d.z > 0.0);
    }
    // Beyond the antipode there is no ray (r / f > pi).
    cam.hfovDeg = 360.0;
    Vec3d none;
    REQUIRE_FALSE(cam.pixelToRay(cam.w + 600.0, 0.5 * cam.h - 0.5, none));

    // Equirect delegates to the Standard map (pixel centre).
    cam.projection = Projection::Equirect;
    cam.w = 4000;
    cam.h = 2000;
    Vec3d eq;
    REQUIRE(cam.pixelToRay(2000.0 - 0.5, 1000.0 - 0.5, eq));
    REQUIRE_THAT(eq.y, Catch::Matchers::WithinAbs(1.0, 1e-12));
    EquirectMap map;
    map.w = 4000;
    map.h = 2000;
    Vec3d ref;
    REQUIRE(map.pixelToDir(123.5, 456.5, ref));
    REQUIRE(cam.pixelToRay(123.0, 456.0, eq));
    REQUIRE((eq - ref).norm() < 1e-12);
    REQUIRE(std::string(projectionName(Projection::Stereographic)) == "Stereographic");
}

// -----------------------------------------------------------------------------
//  EquirectMap
// -----------------------------------------------------------------------------
TEST_CASE("EquirectMap round trips and axis placement in both layouts", "[geom][equirect]") {
    for (const EquirectLayout layout : {EquirectLayout::Standard, EquirectLayout::PolarAxis}) {
        EquirectMap map;
        map.layout = layout;
        map.w = 3600;
        map.h = 1800;
        REQUIRE(map.isValid());
        // pixel -> dir -> pixel over a grid that avoids the exact poles (where
        // longitude is undefined) and the wrap column.
        for (int yi = 1; yi < 18; ++yi) {
            for (int xi = 0; xi < 36; ++xi) {
                const Vec2d px{xi * 100.0 + 37.25, yi * 100.0 + 11.5};
                Vec3d d;
                REQUIRE(map.pixelToDir(px.x, px.y, d));
                REQUIRE_THAT(d.norm(), Catch::Matchers::WithinAbs(1.0, 1e-12));
                Vec2d back;
                REQUIRE(map.dirToPixel(d, back));
                INFO(equirectLayoutName(layout) << " px " << px.x << "," << px.y);
                REQUIRE_THAT(back.x, Catch::Matchers::WithinAbs(px.x, 1e-9));
                REQUIRE_THAT(back.y, Catch::Matchers::WithinAbs(px.y, 1e-9));
            }
        }
        // dir -> pixel -> dir on random directions.
        std::mt19937_64 rng(7u);
        std::normal_distribution<double> gauss(0.0, 1.0);
        for (int n = 0; n < 2000; ++n) {
            const Vec3d d = Vec3d{gauss(rng), gauss(rng), gauss(rng)}.normalized();
            if (!(d.norm() > 0.0)) {
                continue;
            }
            Vec2d px;
            REQUIRE(map.dirToPixel(d, px));
            REQUIRE(px.x >= 0.0);
            REQUIRE(px.x < map.w);
            Vec3d back;
            REQUIRE(map.pixelToDir(px.x, px.y, back));
            REQUIRE((back - d).norm() < 1e-9);
        }
    }

    // Standard: centre = +Y forward, top = +Z up, left edge = -X... at
    // px = 0 the longitude is -180 deg (behind); a quarter turn right is +X.
    EquirectMap standard;
    standard.w = 360;
    standard.h = 180;
    Vec3d d;
    REQUIRE(standard.pixelToDir(180.0, 90.0, d));
    REQUIRE((d - Vec3d{0.0, 1.0, 0.0}).norm() < 1e-12);
    REQUIRE(standard.pixelToDir(180.0, 0.0, d));
    REQUIRE((d - Vec3d{0.0, 0.0, 1.0}).norm() < 1e-12);
    REQUIRE(standard.pixelToDir(270.0, 90.0, d));
    REQUIRE((d - Vec3d{1.0, 0.0, 0.0}).norm() < 1e-12);
    // PolarAxis: the poles are the lens axes +/-Y, the equator row is the seam.
    EquirectMap polar;
    polar.layout = EquirectLayout::PolarAxis;
    polar.w = 360;
    polar.h = 180;
    REQUIRE(polar.pixelToDir(180.0, 0.0, d));
    REQUIRE((d - Vec3d{0.0, 1.0, 0.0}).norm() < 1e-12);
    REQUIRE(polar.pixelToDir(180.0, 180.0, d));
    REQUIRE((d - Vec3d{0.0, -1.0, 0.0}).norm() < 1e-12);
    REQUIRE(polar.pixelToDir(180.0, 90.0, d));
    REQUIRE((d - Vec3d{0.0, 0.0, 1.0}).norm() < 1e-12);
    Vec2d px;
    REQUIRE(polar.dirToPixel(Vec3d{0.0, 0.0, -1.0}, px));
    REQUIRE_THAT(px.y, Catch::Matchers::WithinAbs(90.0, 1e-9));
    // Degenerate inputs.
    EquirectMap empty;
    REQUIRE_FALSE(empty.pixelToDir(0.0, 0.0, d));
    REQUIRE_FALSE(standard.dirToPixel(Vec3d{0.0, 0.0, 0.0}, px));
    REQUIRE_FALSE(standard.pixelToDir(std::nan(""), 0.0, d));
}

// -----------------------------------------------------------------------------
//  Presets
// -----------------------------------------------------------------------------
TEST_CASE("Presets table is sane and searchable", "[geom][presets]") {
    REQUIRE_FALSE(kPresets.empty());
    for (const Preset& p : kPresets) {
        REQUIRE(p.id != nullptr);
        REQUIRE(p.name != nullptr);
        REQUIRE(p.hfovDeg > 0.0);
        REQUIRE(p.hfovDeg <= 360.0);
        // Every preset must produce a valid camera at a common output size.
        VirtualCamera cam;
        cam.w = 1920;
        cam.h = 1080;
        applyPreset(p, cam);
        INFO(p.id);
        REQUIRE(cam.isValid());
        REQUIRE(findPreset(p.id) == &p);
        REQUIRE(findPreset(p.name) == &p);
    }
    // The documented defaults: every look is an eye-offset camera so one
    // distortion control moves between them.
    const Preset* crystal = findPreset("Crystal Ball");
    REQUIRE(crystal != nullptr);
    REQUIRE(crystal->projection == Projection::EyeOffset);
    REQUIRE(crystal->eyeOffset == 1.0);
    REQUIRE(crystal->hfovDeg == 240.0);
    const Preset* asteroid = findPreset("asteroid");
    REQUIRE(asteroid != nullptr);
    REQUIRE(asteroid->pitchDeg == -90.0);
    REQUIRE(asteroid->eyeOffset == 1.0);
    REQUIRE(findPreset("wide")->eyeOffset == 0.15);
    REQUIRE(findPreset("ultra-wide")->eyeOffset == 0.4);
    REQUIRE(findPreset("dewarping")->eyeOffset == 0.0);
    // Crystal Ball at offset 1 must reproduce the stereographic rays exactly
    // (the look did not change when the table switched projections).
    {
        VirtualCamera eye;
        eye.w = 1000;
        eye.h = 1000;
        applyPreset(*crystal, eye);
        VirtualCamera stereo = eye;
        stereo.projection = Projection::Stereographic;
        REQUIRE(eye.isValid());
        REQUIRE(stereo.isValid());
        REQUIRE_THAT(eye.focalPx(), Catch::Matchers::WithinRel(stereo.focalPx(), 1e-12));
        for (const double r : {0.0, 50.0, 200.0, 400.0, 499.0}) {
            Vec3d a, b;
            REQUIRE(eye.pixelToRay(0.5 * eye.w - 0.5 + r, 0.5 * eye.h - 0.5 - 0.3 * r, a));
            REQUIRE(stereo.pixelToRay(0.5 * eye.w - 0.5 + r, 0.5 * eye.h - 0.5 - 0.3 * r, b));
            REQUIRE((a - b).norm() < 1e-12);
        }
    }
    REQUIRE(findPreset("ULTRA_WIDE") != nullptr);
    REQUIRE(findPreset("ultra wide") != nullptr);
    REQUIRE(findPreset("nope") == nullptr);
    REQUIRE(findPreset("") == nullptr);
    // applyPreset keeps the framing fields the user set.
    VirtualCamera cam;
    cam.w = 1280;
    cam.h = 720;
    cam.yawDeg = 12.0;
    cam.rollDeg = 3.0;
    applyPreset(*asteroid, cam);
    REQUIRE(cam.w == 1280);
    REQUIRE(cam.yawDeg == 12.0);
    REQUIRE(cam.rollDeg == 3.0);
    REQUIRE(cam.pitchDeg == -90.0);
    REQUIRE(cam.projection == Projection::EyeOffset);
    REQUIRE(cam.eyeOffset == 1.0);
}

// -----------------------------------------------------------------------------
//  Blend
// -----------------------------------------------------------------------------
TEST_CASE("Blend weight: FOV feather", "[geom][blend]") {
    const LensRig rig = sampleRig();
    BlendParams params;
    params.useOcclusionMask = false;
    const Vec2d centre{rig.lens[kMasterLens].cx, rig.lens[kMasterLens].cy};
    const double thetaMax = deg2rad(0.5 * params.lensFovDeg);
    REQUIRE_THAT(effectiveThetaMax(kMasterLens, params), Catch::Matchers::WithinAbs(thetaMax, 1e-15));

    // 1 on the axis, 0 beyond thetaMax, monotone non-increasing through the feather.
    REQUIRE(lensWeightRef(rig, kMasterLens, 0.0, centre, params) == 1.0);
    REQUIRE(lensWeightRef(rig, kMasterLens, thetaMax + 1e-9, centre, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kMasterLens, kPi, centre, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kMasterLens, thetaMax - deg2rad(params.featherDeg) - 1e-9, centre, params) == 1.0);
    double previous = 1.0;
    for (int i = 0; i <= 400; ++i) {
        const double theta = thetaMax - deg2rad(params.featherDeg) + deg2rad(params.featherDeg) * i / 400.0;
        const double w = lensWeightRef(rig, kMasterLens, theta, centre, params);
        REQUIRE(w <= previous + 1e-15);
        REQUIRE(w >= 0.0);
        REQUIRE(w <= 1.0);
        previous = w;
    }
    // Mid-feather is exactly the smoothstep midpoint.
    REQUIRE_THAT(lensWeightRef(rig, kMasterLens, thetaMax - 0.5 * deg2rad(params.featherDeg), centre, params),
                 Catch::Matchers::WithinAbs(0.5, 1e-12));

    // Outside the frame there is no data.
    REQUIRE(lensWeightRef(rig, kMasterLens, 0.0, Vec2d{-1.0, 10.0}, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kMasterLens, 0.0, Vec2d{10.0, 3000.0}, params) == 0.0);
    REQUIRE(lensWeightRef(rig, 7, 0.0, centre, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kMasterLens, std::nan(""), centre, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kMasterLens, -0.1, centre, params) == 0.0);

    // Seam shift: +2 deg gives the master more, the slave less.
    params.seamShiftDeg = 2.0;
    REQUIRE_THAT(effectiveThetaMax(kMasterLens, params), Catch::Matchers::WithinAbs(thetaMax + deg2rad(2.0), 1e-15));
    REQUIRE_THAT(effectiveThetaMax(kSlaveLens, params), Catch::Matchers::WithinAbs(thetaMax - deg2rad(2.0), 1e-15));
    REQUIRE(lensWeightRef(rig, kMasterLens, thetaMax + deg2rad(1.0), centre, params) > 0.0);
    // A zero feather is a hard edge.
    params.seamShiftDeg = 0.0;
    params.featherDeg = 0.0;
    REQUIRE(lensWeightRef(rig, kMasterLens, thetaMax - 1e-6, centre, params) == 1.0);
    REQUIRE(lensWeightRef(rig, kMasterLens, thetaMax + 1e-6, centre, params) == 0.0);
}

TEST_CASE("Blend weight: occlusion polygon geometry and feather", "[geom][blend]") {
    const LensRig rig = sampleRig();
    const KannalaBrandt5& slave = rig.lens[kSlaveLens];
    const std::vector<Vec2d>& poly = rig.occlusionPolyStream[kSlaveLens];
    // 13 distinct arc vertices closed outward along the rim (see above).
    REQUIRE(poly.size() == 26);

    // The defect this polygon construction exists to prevent: in the order the
    // metadata stores them the vertices form a SELF-INTERSECTING bowtie, whose
    // even-odd fill marks a broad band across the bottom of the frame instead
    // of the stick.  That painted a large black ellipse into reframed output.
    // A simple polygon has no two non-adjacent edges crossing.
    const auto segmentsCross = [](const Vec2d& a, const Vec2d& b, const Vec2d& c, const Vec2d& d) {
        const auto orient = [](const Vec2d& p, const Vec2d& q, const Vec2d& r) {
            const double v = (q.y - p.y) * (r.x - q.x) - (q.x - p.x) * (r.y - q.y);
            return v > 1e-9 ? 1 : (v < -1e-9 ? 2 : 0);
        };
        return orient(a, b, c) != orient(a, b, d) && orient(c, d, a) != orient(c, d, b);
    };
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            // Adjacent edges share an endpoint, so they always "touch".
            if (j == i + 1 || (i == 0 && j == n - 1)) {
                continue;
            }
            INFO("edges " << i << " and " << j << " of the occlusion polygon cross");
            REQUIRE_FALSE(segmentsCross(poly[i], poly[(i + 1) % n], poly[j], poly[(j + 1) % n]));
        }
    }

    // Coordinate-space sanity: the polygon hugs the bottom of the image
    // circle.  Every vertex lies in the bottom half and its radial distance
    // from the principal point is just inside the 195.18 deg rim (measured
    // 20..77 px inside on the sample clip, stream px).
    const double rim = slave.rMaxPx;
    REQUIRE(rim > 1400.0);
    REQUIRE(rim < 1600.0);
    for (const Vec2d& p : poly) {
        const double r = (p - Vec2d{slave.cx, slave.cy}).norm();
        INFO("vertex " << p.x << "," << p.y << " r " << r << " rim " << rim);
        REQUIRE(p.y > slave.cy);
        REQUIRE(r > rim - 100.0);
        REQUIRE(r < rim + 10.0);
    }

    // The occluded region is the sliver BETWEEN the arc and the rim, so a
    // point just outside an arc vertex - radially, away from the principal
    // point - must be inside it, while the principal point itself is not.
    // Vertex 6 is near the apex of the arc, the widest part of the sliver.
    const Vec2d centre{slave.cx, slave.cy};
    const Vec2d apex = poly[6];
    const Vec2d radial = (apex - centre) * (1.0 / (apex - centre).norm());
    const Vec2d inside = apex + radial * 8.0;
    REQUIRE(pointInPolygon(poly, inside));
    // The sliver is thin, so the interior point sits only a few px from an
    // edge; asserting a large depth would be asserting the wrong shape.
    REQUIRE(signedDistanceToPolygon(poly, inside) < 0.0);
    const Vec2d outside{slave.cx, slave.cy};
    REQUIRE_FALSE(pointInPolygon(poly, outside));
    REQUIRE(signedDistanceToPolygon(poly, outside) > 1000.0);
    // Signed distance is continuous across the boundary: on a vertex it is 0.
    REQUIRE_THAT(signedDistanceToPolygon(poly, apex), Catch::Matchers::WithinAbs(0.0, 1e-9));
    // Fewer than three vertices occlude nothing.
    REQUIRE(signedDistanceToPolygon({}, inside) == std::numeric_limits<double>::infinity());
    REQUIRE_FALSE(pointInPolygon({poly[0], poly[1]}, inside));

    // The whole point of the sliver: it must cover only a small part of the
    // frame.  The self-intersecting bowtie covered 3.8 % of a 3000x3000
    // stream and closing the arc across its chord instead would cover 13.8 %;
    // the correct outward closure covers under 2.5 %.
    int masked = 0;
    int sampled = 0;
    for (int y = 0; y < rig.streamH; y += 10) {
        for (int x = 0; x < rig.streamW; x += 10) {
            ++sampled;
            if (pointInPolygon(poly, Vec2d{static_cast<double>(x), static_cast<double>(y)})) {
                ++masked;
            }
        }
    }
    REQUIRE(sampled > 0);
    const double maskedFraction = static_cast<double>(masked) / static_cast<double>(sampled);
    INFO("occlusion polygon masks " << 100.0 * maskedFraction << " % of the frame");
    REQUIRE(maskedFraction > 0.005);
    REQUIRE(maskedFraction < 0.025);

    // Weights: the inside pixel gets 0 through the mask, 1 without it; the
    // principal point is unaffected either way.
    BlendParams params;
    const double thetaInside = std::atan2((inside - Vec2d{slave.cx, slave.cy}).norm(), slave.fx);  // any theta in FOV
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, inside, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kSlaveLens, thetaInside, inside, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, outside, params) == 1.0);
    params.useOcclusionMask = false;
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, inside, params) == 1.0);
    params.useOcclusionMask = true;

    // The occlusion feather ramps linearly from 0 at the boundary to 1 at
    // occlusionFeatherPx outside.  The occluded sliver lies between the arc
    // and the rim, so leaving it means walking RADIALLY INWARD from an arc
    // vertex, towards the principal point.  (Walking +y from the arc would go
    // deeper into the sliver, not out of it.)  The arc is nearly
    // perpendicular to the radius here, so the perpendicular distance grows
    // at very nearly the walked distance.
    const Vec2d inward = radial * -1.0;
    double prev = -1.0;
    for (int i = 0; i <= 80; ++i) {
        const Vec2d p = apex + inward * (0.5 * i);
        const double w = lensWeightRef(rig, kSlaveLens, 0.0, p, params);
        REQUIRE(w >= prev - 1e-12);
        REQUIRE(w >= 0.0);
        REQUIRE(w <= 1.0);
        prev = w;
    }
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, apex + inward * (2.0 * params.occlusionFeatherPx), params) == 1.0);
    const double half = lensWeightRef(rig, kSlaveLens, 0.0, apex + inward * (0.5 * params.occlusionFeatherPx), params);
    REQUIRE(half > 0.2);
    REQUIRE(half < 0.8);
    // A zero feather is a hard mask.
    params.occlusionFeatherPx = 0.0;
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, inside, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, apex + inward * 1.0, params) == 1.0);
    // The master has no polygon in this fixture: nothing is masked.
    REQUIRE(lensWeightRef(rig, kMasterLens, 0.0, Vec2d{1500.0, 2900.0}, params) == 1.0);
}

// =============================================================================
//  Hide Mount Auto: rebuilding an occlusion polygon per azimuth stretch
// =============================================================================
namespace {

/// True when no two non-adjacent edges of `poly` cross (a simple polygon).
bool isSimplePolygon(const std::vector<Vec2d>& poly) {
    const auto orient = [](const Vec2d& p, const Vec2d& q, const Vec2d& r) {
        const double v = (q.y - p.y) * (r.x - q.x) - (q.x - p.x) * (r.y - q.y);
        return v > 1e-9 ? 1 : (v < -1e-9 ? 2 : 0);
    };
    const auto cross = [&](const Vec2d& a, const Vec2d& b, const Vec2d& c, const Vec2d& d) {
        return orient(a, b, c) != orient(a, b, d) && orient(c, d, a) != orient(c, d, b);
    };
    const std::size_t n = poly.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (j == i + 1 || (i == 0 && j == n - 1)) {
                continue;  // adjacent edges share a vertex
            }
            if (cross(poly[i], poly[(i + 1) % n], poly[j], poly[(j + 1) % n])) {
                return false;
            }
        }
    }
    return true;
}

/// Exactly the same vertices (no tolerance: "unchanged" means bit for bit).
bool sameVertices(const std::vector<Vec2d>& a, const std::vector<Vec2d>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].x != b[i].x || a[i].y != b[i].y) {
            return false;
        }
    }
    return true;
}

/// A point at polar angle `deg` (fisheye image convention) and `radius` px.
Vec2d atPolar(const Vec2d& centre, double deg, double radius) {
    return Vec2d{centre.x + radius * std::cos(deg2rad(deg)), centre.y + radius * std::sin(deg2rad(deg))};
}

}  // namespace

TEST_CASE("Hide Mount Auto: the ray radii of the calibration's occlusion sliver", "[geom][rig][hidemount]") {
    const LensRig rig = sampleRig();
    const KannalaBrandt5& slave = rig.lens[kSlaveLens];
    const std::vector<Vec2d>& poly = rig.occlusionPolyStream[kSlaveLens];
    const Vec2d centre{slave.cx, slave.cy};
    // Straight down (polar angle 90 deg, image y grows downwards) the ray
    // meets the arc's apex first and the rim last.
    const auto down = occlusionRayRadii(poly, centre, deg2rad(90.0));
    REQUIRE(down.has_value());
    CHECK(down->first > 0.5 * slave.rMaxPx);
    CHECK(down->first < down->second);
    CHECK(down->second >= slave.rMaxPx * 0.999);
    // Straight up and to the right the sliver is nowhere: no crossing.
    CHECK_FALSE(occlusionRayRadii(poly, centre, deg2rad(-90.0)).has_value());
    CHECK_FALSE(occlusionRayRadii(poly, centre, 0.0).has_value());
    // Garbage in, nothing out.
    CHECK_FALSE(occlusionRayRadii(poly, Vec2d{std::nan(""), 0.0}, 0.0).has_value());
    CHECK_FALSE(occlusionRayRadii(std::vector<Vec2d>(2), centre, 0.0).has_value());
}

TEST_CASE("Hide Mount Auto: a polygon with nothing released is the calibration's, bit for bit",
          "[geom][rig][hidemount]") {
    const LensRig rig = sampleRig();
    const KannalaBrandt5& slave = rig.lens[kSlaveLens];
    const std::vector<Vec2d>& poly = rig.occlusionPolyStream[kSlaveLens];
    const Vec2d centre{slave.cx, slave.cy};
    OcclusionClipParams params;
    params.usableRadiusPx = slave.rMaxPx + 24.0;
    params.clampRadiusPx = 10.0;  // far inside where the polygon starts
    // No span at all, an all-Keep span, and a Clamp the polygon already honours.
    for (const std::vector<OcclusionSpan>& spans :
         {std::vector<OcclusionSpan>{},
          std::vector<OcclusionSpan>{{deg2rad(0.0), deg2rad(359.0), OcclusionSpanState::Keep}},
          std::vector<OcclusionSpan>{{deg2rad(20.0), deg2rad(160.0), OcclusionSpanState::Clamp}}}) {
        auto out = clipOcclusionPolygon(poly, centre, spans, params);
        REQUIRE(out.ok());
        CHECK(sameVertices(out.value(), poly));
    }
}

TEST_CASE("Hide Mount Auto: a released stretch gives the lens back and keeps the rest", "[geom][rig][hidemount]") {
    const LensRig rig = sampleRig();
    const KannalaBrandt5& slave = rig.lens[kSlaveLens];
    const std::vector<Vec2d>& poly = rig.occlusionPolyStream[kSlaveLens];
    const Vec2d centre{slave.cx, slave.cy};
    const double feather = 24.0;
    OcclusionClipParams params;
    params.usableRadiusPx = slave.rMaxPx + feather;
    params.maxVertices = 32;
    // Release the middle of the arc (polar angle 80..100 deg).
    const std::vector<OcclusionSpan> spans{{deg2rad(80.0), deg2rad(100.0), OcclusionSpanState::Release}};
    auto out = clipOcclusionPolygon(poly, centre, spans, params);
    REQUIRE(out.ok());
    const std::vector<Vec2d>& clipped = out.value();
    REQUIRE(clipped.size() >= 3);
    CHECK(clipped.size() <= params.maxVertices);
    CHECK(isSimplePolygon(clipped));

    // Inside the released stretch: every pixel the lens can weigh is outside
    // the polygon and beyond its feather (the kernel's factor is 1).
    for (const double deg : {82.0, 90.0, 98.0}) {
        const auto r = occlusionRayRadii(poly, centre, deg2rad(deg));
        REQUIRE(r.has_value());
        // (The calibration's own rim is a chord every ~10 deg, up to ~6 px
        // inside rMax between its vertices, so the last probe stays 10 px in.)
        for (const double radius : {r->first + 2.0, 0.5 * (r->first + slave.rMaxPx), slave.rMaxPx - 10.0}) {
            const Vec2d p = atPolar(centre, deg, radius);
            INFO("angle " << deg << " radius " << radius);
            CHECK(pointInPolygon(poly, p));  // the calibration hid it
            CHECK_FALSE(pointInPolygon(clipped, p));
            CHECK(signedDistanceToPolygon(clipped, p) >= feather);
        }
    }
    // Outside it: the calibration's mask, edge for edge.
    for (const double deg : {45.0, 60.0, 120.0, 135.0}) {
        const auto r = occlusionRayRadii(poly, centre, deg2rad(deg));
        const auto c = occlusionRayRadii(clipped, centre, deg2rad(deg));
        REQUIRE(r.has_value());
        REQUIRE(c.has_value());
        INFO("angle " << deg);
        CHECK_THAT(c->first, Catch::Matchers::WithinAbs(r->first, 1e-6));
        const Vec2d hidden = atPolar(centre, deg, r->first + 3.0);
        CHECK(pointInPolygon(clipped, hidden));
        const Vec2d seen = atPolar(centre, deg, r->first - 3.0);
        CHECK_FALSE(pointInPolygon(clipped, seen));
    }
}

TEST_CASE("Hide Mount Auto: a clamped stretch starts no closer than the clamp radius", "[geom][rig][hidemount]") {
    const LensRig rig = sampleRig();
    const KannalaBrandt5& slave = rig.lens[kSlaveLens];
    const std::vector<Vec2d>& poly = rig.occlusionPolyStream[kSlaveLens];
    const Vec2d centre{slave.cx, slave.cy};
    const auto apex = occlusionRayRadii(poly, centre, deg2rad(90.0));
    REQUIRE(apex.has_value());
    OcclusionClipParams params;
    params.usableRadiusPx = slave.rMaxPx + 24.0;
    params.clampRadiusPx = apex->first + 30.0;  // binds across the middle of the arc
    const std::vector<OcclusionSpan> spans{{deg2rad(60.0), deg2rad(120.0), OcclusionSpanState::Clamp}};
    auto out = clipOcclusionPolygon(poly, centre, spans, params);
    REQUIRE(out.ok());
    const std::vector<Vec2d>& clamped = out.value();
    CHECK(isSimplePolygon(clamped));
    for (double deg = 62.0; deg <= 118.0; deg += 4.0) {
        const auto c = occlusionRayRadii(clamped, centre, deg2rad(deg));
        REQUIRE(c.has_value());
        INFO("angle " << deg);
        CHECK(c->first >= params.clampRadiusPx - 1e-6);
        // Beyond the clamp the mask still holds.
        CHECK(pointInPolygon(clamped, atPolar(centre, deg, params.clampRadiusPx + 6.0)));
    }
}

TEST_CASE("Hide Mount Auto: a polygon the kernels cannot hold is refused, garbage is rejected",
          "[geom][rig][hidemount]") {
    const LensRig rig = sampleRig();
    const KannalaBrandt5& slave = rig.lens[kSlaveLens];
    const std::vector<Vec2d>& poly = rig.occlusionPolyStream[kSlaveLens];
    const Vec2d centre{slave.cx, slave.cy};
    OcclusionClipParams params;
    params.usableRadiusPx = slave.rMaxPx + 24.0;
    params.maxVertices = 32;
    // Twelve released slivers of 4 deg, 10 deg apart: far over the budget.
    std::vector<OcclusionSpan> many;
    for (int k = 0; k < 12; ++k) {
        const double a = 32.0 + 10.0 * k;
        many.push_back({deg2rad(a), deg2rad(a + 4.0), OcclusionSpanState::Release});
    }
    auto refused = clipOcclusionPolygon(poly, centre, many, params);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Unsupported);
    // Malformed input.
    const std::vector<OcclusionSpan> one{{deg2rad(80.0), deg2rad(100.0), OcclusionSpanState::Release}};
    CHECK(clipOcclusionPolygon(std::vector<Vec2d>(2), centre, one, params).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(clipOcclusionPolygon(poly, Vec2d{std::nan(""), 0.0}, one, params).error().code ==
          ErrorCode::InvalidArgument);
    OcclusionClipParams noRadius = params;
    noRadius.usableRadiusPx = 0.0;
    CHECK(clipOcclusionPolygon(poly, centre, one, noRadius).error().code == ErrorCode::InvalidArgument);
    const std::vector<OcclusionSpan> nanSpan{{std::nan(""), 1.0, OcclusionSpanState::Release}};
    CHECK(clipOcclusionPolygon(poly, centre, nanSpan, params).error().code == ErrorCode::InvalidArgument);
    // A polygon around the lens centre has no inner and outer run.
    const std::vector<Vec2d> around{{centre.x - 10, centre.y - 10}, {centre.x + 10, centre.y - 10},
                                    {centre.x + 10, centre.y + 10}, {centre.x - 10, centre.y + 10}};
    CHECK(clipOcclusionPolygon(around, centre, one, params).error().code == ErrorCode::Unsupported);
}

TEST_CASE("Hide Mount Auto: a verdict that keeps everything leaves the 6K rig's polygons untouched",
          "[geom][rig][hidemount]") {
    // Both lenses with the transcribed arc, on the verified 6K geometry: the
    // polygons start beyond the seam plane plus the occlusion feather, so a
    // kept verdict needs no clamp and must change nothing - the promise that
    // makes Auto byte-identical wherever it keeps the whole polygon.
    meta::CalibrationSet set = sampleCalibration();
    set.master = makeRecord(1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f,
                            {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f}, 0.7036960f,
                            0.7103991f, -0.0046943f, -0.0110939f, true);
    auto built = LensRig::build(set, sampleScaling(), FocalSource::DigitalFocalLength, kDigitalFocal,
                                ExtrinsicConvention{});
    REQUIRE(built.ok());
    const LensRig rig = built.value();
    REQUIRE(rig.occlusionPolyStream[kSlaveLens].size() >= 3);
    REQUIRE(rig.occlusionPolyStream[kMasterLens].size() >= 3);

    const render::MountMaskParams mp;
    auto arc = render::mountArcColumns(rig, mp.equirectW);
    REQUIRE(arc.ok());
    REQUIRE(arc.value().arcColumns > 0u);

    BlendParams blend;
    render::MountMask kept;
    kept.columns = mp.equirectW;
    kept.state.assign(mp.equirectW, render::kMountKeepClean0);
    LensRig same = rig;
    auto applied = render::applyMountMask(same, kept, blend, mp);
    REQUIRE(applied.ok());
    CHECK_FALSE(applied.value().changed[0]);
    CHECK_FALSE(applied.value().changed[1]);
    CHECK(sameVertices(same.occlusionPolyStream[kSlaveLens], rig.occlusionPolyStream[kSlaveLens]));
    CHECK(sameVertices(same.occlusionPolyStream[kMasterLens], rig.occlusionPolyStream[kMasterLens]));

    // Release the arc's first contiguous 64 columns: the slave's polygon is
    // rebuilt, still simple, inside the kernels' budget, and no longer
    // reaches the seam plane there.
    render::MountMask released = kept;
    std::uint32_t first = 0;
    while (first < mp.equirectW && !arc.value().inArc[first]) {
        ++first;
    }
    std::uint32_t done = 0;
    for (std::uint32_t c = first; c < mp.equirectW && done < 64u && arc.value().inArc[c]; ++c, ++done) {
        released.state[c] = render::kMountRelease;
    }
    REQUIRE(done > 16u);
    LensRig changed = rig;
    auto applied2 = render::applyMountMask(changed, released, blend, mp);
    REQUIRE(applied2.ok());
    CHECK((applied2.value().changed[0] || applied2.value().changed[1]));
    CHECK(applied2.value().releasedColumns == done);
    for (int i = 0; i < kLensCount; ++i) {
        const auto& p = changed.occlusionPolyStream[static_cast<std::size_t>(i)];
        CHECK(p.size() <= static_cast<std::size_t>(OSV_MAX_OCCLUSION_POINTS));
        CHECK(isSimplePolygon(p));
    }
    // Garbage verdicts are refused and leave the rig alone.
    render::MountMask bad = kept;
    bad.state[0] = 7;
    LensRig untouched = rig;
    CHECK_FALSE(render::applyMountMask(untouched, bad, blend, mp).ok());
    CHECK(sameVertices(untouched.occlusionPolyStream[kSlaveLens], rig.occlusionPolyStream[kSlaveLens]));
    render::MountMask shortMask = kept;
    shortMask.state.resize(10);
    CHECK_FALSE(render::applyMountMask(untouched, shortMask, blend, mp).ok());
}

TEST_CASE("Hide Mount Auto: a verdict folded into a rotated rig is the calibration rig's rebuild",
          "[geom][rig][hidemount]") {
    // The verdict's columns are the calibration rig's band columns.  A rig
    // with a fitted lens rotation sees the seam at slightly other fisheye
    // pixels, so rebuilding through it would shift every stretch by the
    // rotation; applyMountMaskFrom rebuilds against the calibration rig and
    // copies the polygons over, whichever of the two settled first.
    meta::CalibrationSet set = sampleCalibration();
    set.master = makeRecord(1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f,
                            {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f}, 0.7036960f,
                            0.7103991f, -0.0046943f, -0.0110939f, true);
    auto built = LensRig::build(set, sampleScaling(), FocalSource::DigitalFocalLength, kDigitalFocal,
                                ExtrinsicConvention{});
    REQUIRE(built.ok());
    const LensRig rig = built.value();
    const render::MountMaskParams mp;
    auto arc = render::mountArcColumns(rig, mp.equirectW);
    REQUIRE(arc.ok());
    REQUIRE(arc.value().arcColumns > 0u);

    // Release the arc's first contiguous 64 columns, the rest stays as drawn.
    render::MountMask released;
    released.columns = mp.equirectW;
    released.state.assign(mp.equirectW, render::kMountKeepNoClamp);
    std::uint32_t first = 0;
    while (first < mp.equirectW && !arc.value().inArc[first]) {
        ++first;
    }
    std::uint32_t done = 0;
    for (std::uint32_t c = first; c < mp.equirectW && done < 64u && arc.value().inArc[c]; ++c, ++done) {
        released.state[c] = render::kMountRelease;
    }
    REQUIRE(done > 16u);
    BlendParams blend;
    LensRig reference = rig;
    auto direct = render::applyMountMask(reference, released, blend, mp);
    REQUIRE(direct.ok());

    // A rotation of about half a degree, the size the lens alignment fits.
    LensRig rotated = rig;
    REQUIRE(render::applyLensRotation(rotated, Vec3d{deg2rad(0.4), deg2rad(-0.3), deg2rad(0.2)}).ok());
    const LensRig rotatedBefore = rotated;
    auto folded = render::applyMountMaskFrom(rig, rotated, released, blend, mp);
    REQUIRE(folded.ok());
    for (int i = 0; i < kLensCount; ++i) {
        const auto lens = static_cast<std::size_t>(i);
        INFO("lens " << i);
        // The calibration rig's rebuild, vertex for vertex ...
        CHECK(sameVertices(rotated.occlusionPolyStream[lens], reference.occlusionPolyStream[lens]));
        CHECK(folded.value().changed[i] == direct.value().changed[i]);
        // ... and the rotation itself untouched.
        CHECK(rotated.bodyToLens[lens].distance(rotatedBefore.bodyToLens[lens]) == 0.0);
    }
    CHECK(folded.value().releasedColumns == direct.value().releasedColumns);

    // A "no clamp" verdict everywhere changes nothing, bit for bit.
    render::MountMask nothing = released;
    nothing.state.assign(mp.equirectW, render::kMountKeepNoClamp);
    LensRig same = rotatedBefore;
    auto kept = render::applyMountMaskFrom(rig, same, nothing, blend, mp);
    REQUIRE(kept.ok());
    CHECK_FALSE(kept.value().changed[0]);
    CHECK_FALSE(kept.value().changed[1]);
    for (int i = 0; i < kLensCount; ++i) {
        const auto lens = static_cast<std::size_t>(i);
        CHECK(sameVertices(same.occlusionPolyStream[lens], rig.occlusionPolyStream[lens]));
    }

    // A rig whose polygons are not the calibration's (a verdict already
    // folded in) is refused and left untouched: rebuilding from a rebuilt
    // outline would compound the verdict.
    LensRig twice = rotated;
    auto refused = render::applyMountMaskFrom(rig, twice, released, blend, mp);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    for (int i = 0; i < kLensCount; ++i) {
        const auto lens = static_cast<std::size_t>(i);
        CHECK(sameVertices(twice.occlusionPolyStream[lens], rotated.occlusionPolyStream[lens]));
    }
}
