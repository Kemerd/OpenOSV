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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <string>
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

    // LRF half: 1024 / 3776, flagged unverified.
    std::vector<std::string> notes;
    auto lrf = StreamScaling::derive(1024, 1024, 3840, 3840, 0.0, 0.0, std::nullopt, &notes);
    REQUIRE(lrf.ok());
    REQUIRE_THAT(lrf.value().scale, Catch::Matchers::WithinAbs(1024.0 / 3776.0, 1e-12));
    REQUIRE_FALSE(lrf.value().verified);
    REQUIRE(notes.size() == 1);
    REQUIRE(notes[0].find("unverified") != std::string::npos);

    // Generic fallback: digital focal / calibration focal, unverified.
    auto fourK = StreamScaling::derive(1920, 1920, 3840, 3840, 520.0, 1040.0);
    REQUIRE(fourK.ok());
    REQUIRE_THAT(fourK.value().scale, Catch::Matchers::WithinAbs(0.5, 1e-12));
    REQUIRE_FALSE(fourK.value().verified);
    REQUIRE(fourK.value().dstCx == 960.0);
    REQUIRE(fourK.value().srcCx == 1920.0);

    // Fallback without focal lengths is an error, not a guess.
    REQUIRE_FALSE(StreamScaling::derive(1920, 1920, 3840, 3840, 0.0, 0.0).ok());
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
    // Occlusion polygon mapped to stream px: 14 vertices, bottom half.
    REQUIRE(rig.occlusionPolyStream[kSlaveLens].size() == 14);
    REQUIRE(rig.occlusionPolyStream[kMasterLens].empty());
    for (const Vec2d& p : rig.occlusionPolyStream[kSlaveLens]) {
        REQUIRE(p.y > slave.cy);
    }
    REQUIRE_FALSE(rig.notes.empty());

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
    // The documented defaults.
    const Preset* crystal = findPreset("Crystal Ball");
    REQUIRE(crystal != nullptr);
    REQUIRE(crystal->projection == Projection::Stereographic);
    REQUIRE(crystal->hfovDeg == 240.0);
    const Preset* asteroid = findPreset("asteroid");
    REQUIRE(asteroid != nullptr);
    REQUIRE(asteroid->pitchDeg == -90.0);
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
    REQUIRE(cam.projection == Projection::Stereographic);
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
    REQUIRE(poly.size() == 14);

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

    // A point between the chord P0-P1 and the arc vertex P3 is inside the
    // crescent (the region between the arc and the chords is the occluded
    // area), far from the principal point is outside.
    const Vec2d p0 = poly[0];
    const Vec2d p1 = poly[1];
    const Vec2d p3 = poly[3];
    const double t = (p3.x - p0.x) / (p1.x - p0.x);
    const Vec2d chord{p3.x, p0.y + t * (p1.y - p0.y)};
    const Vec2d inside = (chord + p3) * 0.5;
    REQUIRE(pointInPolygon(poly, inside));
    REQUIRE(signedDistanceToPolygon(poly, inside) < -24.0);
    const Vec2d outside{slave.cx, slave.cy};
    REQUIRE_FALSE(pointInPolygon(poly, outside));
    REQUIRE(signedDistanceToPolygon(poly, outside) > 1000.0);
    // Signed distance is continuous across the boundary: on a vertex it is 0.
    REQUIRE_THAT(signedDistanceToPolygon(poly, p3), Catch::Matchers::WithinAbs(0.0, 1e-9));
    // Fewer than three vertices occlude nothing.
    REQUIRE(signedDistanceToPolygon({}, inside) == std::numeric_limits<double>::infinity());
    REQUIRE_FALSE(pointInPolygon({p0, p1}, inside));

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
    // occlusionFeatherPx outside.  The crescent lies between the arc and
    // the chords (towards the image centre), so walking straight down from
    // the arc vertex P3 (+y, towards the rim) leaves the polygon.  The arc
    // edge is slanted (~39 deg at P3) so the perpendicular distance grows at
    // ~0.78 x the walked distance; margins below allow for that.
    const Vec2d down{0.0, 1.0};
    double prev = -1.0;
    for (int i = 0; i <= 80; ++i) {
        const Vec2d p = p3 + down * (0.5 * i);
        const double w = lensWeightRef(rig, kSlaveLens, 0.0, p, params);
        REQUIRE(w >= prev - 1e-12);
        REQUIRE(w >= 0.0);
        REQUIRE(w <= 1.0);
        prev = w;
    }
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, p3 + down * (2.0 * params.occlusionFeatherPx), params) == 1.0);
    const double half = lensWeightRef(rig, kSlaveLens, 0.0, p3 + down * (0.5 * params.occlusionFeatherPx), params);
    REQUIRE(half > 0.2);
    REQUIRE(half < 0.8);
    // A zero feather is a hard mask.
    params.occlusionFeatherPx = 0.0;
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, inside, params) == 0.0);
    REQUIRE(lensWeightRef(rig, kSlaveLens, 0.0, p3 + down * 1.0, params) == 1.0);
    // The master has no polygon in this fixture: nothing is masked.
    REQUIRE(lensWeightRef(rig, kMasterLens, 0.0, Vec2d{1500.0, 2900.0}, params) == 1.0);
}
