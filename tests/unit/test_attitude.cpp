// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for the attitude side of osv_geom: AttitudeTrack (sparse / dense
// placement, slerp lookup, IMU clock fit), ConventionProbe and the
// stabilisation corrections.  The [sample] cases read the sample clip and
// SKIP when it is absent; everything else runs on synthetic data.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Math.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/ConventionProbe.h"
#include "osv/geom/Stabilization.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::geom;

namespace {

/// Frame period of the 59.94 fps sample clip (us).
constexpr double kFramePeriodUs = 1001.0 * 1.0e6 / 60000.0;

/// Open the sample clip and load its primary metadata track.  Both results
/// are checked so a failure is reported at the call site.  The pair lives
/// on the heap because the MetadataTrack keeps a pointer to the OsvFile it
/// was loaded from, so the two must never be moved apart.
struct SampleMeta {
    OsvFile file;
    meta::MetadataTrack track;
};

std::unique_ptr<SampleMeta> openSample() {
    auto s = std::make_unique<SampleMeta>();
    auto file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    s->file = std::move(file).value();
    auto track = meta::MetadataTrack::load(s->file);
    REQUIRE(track.ok());
    s->track = std::move(track).value();
    return s;
}

/// Encode a worldFromBody rotation into the four stored floats so that
/// reading them under `conv` reproduces it (the inverse of
/// worldFromBodyQuat()).  Used to synthesise probe data with a known truth.
meta::Quaternion encode(const Quatd& worldFromBody, const AttitudeConvention& conv) {
    // The stored rotation is either body->world or world->body.
    const Quatd stored = (conv.sense == AttitudeSense::WorldToBody) ? worldFromBody.conj() : worldFromBody;
    meta::Quaternion q;
    q.present = true;
    if (conv.order == QuatOrder::WXYZ) {
        q.w = static_cast<float>(stored.w);
        q.x = static_cast<float>(stored.x);
        q.y = static_cast<float>(stored.y);
        q.z = static_cast<float>(stored.z);
    } else {
        // toQuatdXYZW() reads the stored (w, x, y, z) fields as (x, y, z, w).
        q.w = static_cast<float>(stored.x);
        q.x = static_cast<float>(stored.y);
        q.y = static_cast<float>(stored.z);
        q.z = static_cast<float>(stored.w);
    }
    return q;
}

/// Uniformly distributed random unit quaternion.
Quatd randomQuat(std::mt19937_64& rng) {
    std::normal_distribution<double> gauss(0.0, 1.0);
    return Quatd{gauss(rng), gauss(rng), gauss(rng), gauss(rng)}.normalized();
}

bool sameConvention(const AttitudeConvention& a, const AttitudeConvention& b) {
    return a.order == b.order && a.sense == b.sense && a.up == b.up;
}

/// The up axis with its sign reversed (Y <-> -Y, Z <-> -Z).
WorldUp oppositeUp(WorldUp up) {
    switch (up) {
        case WorldUp::Y: return WorldUp::NegY;
        case WorldUp::NegY: return WorldUp::Y;
        case WorldUp::Z: return WorldUp::NegZ;
        case WorldUp::NegZ: return WorldUp::Z;
    }
    return up;
}

/// True when the two readings decode the quaternion identically and differ at
/// most in the sign of the up axis. A gravity probe cannot tell +up with +g
/// from -up with -g (the stored quaternions and the measured vectors are the
/// same numbers), so that pair counts as one recovered convention.
bool sameUpToSign(const AttitudeConvention& a, const AttitudeConvention& b) {
    return a.order == b.order && a.sense == b.sense && (a.up == b.up || a.up == oppositeUp(b.up));
}

}  // namespace

// -----------------------------------------------------------------------------
//  Convention helpers
// -----------------------------------------------------------------------------
TEST_CASE("Attitude convention helpers round trip through encode()", "[attitude]") {
    std::mt19937_64 rng(11u);
    for (const AttitudeConvention& conv : ConventionProbe::candidates()) {
        for (int n = 0; n < 50; ++n) {
            const Quatd truth = randomQuat(rng);
            const meta::Quaternion stored = encode(truth, conv);
            const Quatd decoded = worldFromBodyQuat(stored, conv);
            // float storage limits the agreement to ~1e-7 rad.
            REQUIRE(decoded.angleTo(truth) < 1e-6);
            // bodyFromWorldMatrix is the transpose of worldFromBody.
            const Mat3d bw = bodyFromWorldMatrix(stored, conv);
            REQUIRE(bw.distance(decoded.toMatrix().transposed()) < 1e-12);
        }
    }
    // Absent quaternion -> identity.
    meta::Quaternion absent;
    REQUIRE(worldFromBodyQuat(absent, AttitudeConvention{}).angleTo(Quatd::identity()) == 0.0);
    REQUIRE(worldUpVector(WorldUp::Y).y == 1.0);
    REQUIRE(worldUpVector(WorldUp::Z).z == 1.0);
    REQUIRE(attitudeConventionName(AttitudeConvention{}) == "XYZW/BodyToWorld/-Y");
    REQUIRE(std::string(attitudeSenseName(AttitudeSense::BodyToWorld)) == "BodyToWorld");
    REQUIRE(std::string(worldUpName(WorldUp::Z)) == "Z");
    REQUIRE(ConventionProbe::candidates().size() == 16);
}

// -----------------------------------------------------------------------------
//  AttitudeTrack on synthetic samples
// -----------------------------------------------------------------------------
TEST_CASE("AttitudeTrack slerp midpoint, clamping and tidying", "[attitude]") {
    AttitudeTrack::Options options;
    std::vector<AttitudeTrack::Sample> samples;
    // Deliberately unsorted, with a duplicate time and a NaN entry.
    samples.push_back({1000.0, Quatd::fromAxisAngle(Vec3d{0.0, 0.0, 1.0}, deg2rad(90.0))});
    samples.push_back({0.0, Quatd::identity()});
    samples.push_back({0.0, Quatd::fromAxisAngle(Vec3d{1.0, 0.0, 0.0}, 1.0)});  // duplicate time, dropped
    samples.push_back({std::nan(""), Quatd::identity()});                       // dropped
    samples.push_back({2000.0, Quatd::fromAxisAngle(Vec3d{0.0, 0.0, 1.0}, deg2rad(180.0))});

    auto built = AttitudeTrack::fromSamples(samples, options);
    REQUIRE(built.ok());
    const AttitudeTrack& track = built.value();
    REQUIRE(track.sampleCount() == 3);
    REQUIRE(track.beginUs() == 0.0);
    REQUIRE(track.endUs() == 2000.0);
    REQUIRE(track.samples()[0].tUs == 0.0);
    REQUIRE(track.samples()[0].worldFromBody.angleTo(Quatd::identity()) < 1e-12);

    // Midpoint of a 90 deg turn is a 45 deg turn about the same axis.
    const Quatd mid = track.worldFromBody(500.0);
    REQUIRE(mid.angleTo(Quatd::fromAxisAngle(Vec3d{0.0, 0.0, 1.0}, deg2rad(45.0))) < 1e-12);
    const Vec3d turned = mid.rotate(Vec3d{0.0, 1.0, 0.0});
    REQUIRE_THAT(turned.x, Catch::Matchers::WithinAbs(-std::sqrt(0.5), 1e-12));
    REQUIRE_THAT(turned.y, Catch::Matchers::WithinAbs(std::sqrt(0.5), 1e-12));
    // Exact hits and clamping at both ends.
    REQUIRE(track.worldFromBody(1000.0).angleTo(samples[0].worldFromBody) < 1e-12);
    REQUIRE(track.worldFromBody(-5000.0).angleTo(Quatd::identity()) < 1e-12);
    REQUIRE(track.worldFromBody(9999.0).angleTo(samples[4].worldFromBody) < 1e-12);
    REQUIRE(track.worldFromBody(std::nan("")).angleTo(Quatd::identity()) < 1e-12);
    // The interpolation is continuous across a sample: 0.002 us on a track
    // turning 90 deg per 1000 us is 3.14e-6 rad, nothing more.
    const double step = track.worldFromBody(999.999).angleTo(track.worldFromBody(1000.001));
    REQUIRE(step < 1e-5);
    REQUIRE_THAT(step, Catch::Matchers::WithinAbs(0.002 / 1000.0 * kHalfPi, 1e-7));
    REQUIRE(track.worldUp().y == -1.0);
    REQUIRE(track.clockFit().n == 0);

    // Nothing usable -> error, and an empty track answers identity.
    REQUIRE_FALSE(AttitudeTrack::fromSamples({}, options).ok());
    REQUIRE_FALSE(AttitudeTrack::fromSamples({{std::nan(""), Quatd::identity()}}, options).ok());
    AttitudeTrack empty;
    REQUIRE(empty.sampleCount() == 0);
    REQUIRE(empty.worldFromBody(0.0).angleTo(Quatd::identity()) == 0.0);
}

TEST_CASE("AttitudeTrack::fitClock recovers an exact linear clock", "[attitude]") {
    // ts = 1.0004 * t + 12345 ticks, 17 samples per batch, 16 batches.
    std::vector<AttitudeTrack::ClockObservation> obs;
    for (int i = 0; i < 16; ++i) {
        const double t = 30669420766.0 + i * kFramePeriodUs;
        obs.push_back({t, 1.0004 * t + 12345.0, 17});
    }
    const AttitudeTrack::ClockFit fit = AttitudeTrack::fitClock(obs);
    REQUIRE(fit.n == 16);
    REQUIRE_THAT(fit.ticksPerUs, Catch::Matchers::WithinAbs(1.0004, 1e-9));
    REQUIRE(fit.rmsUs < 1e-3);
    REQUIRE_THAT(fit.ticksPerSample, Catch::Matchers::WithinAbs(1.0004 * kFramePeriodUs / 17.0, 1e-6));
    // Too few / non-finite observations give no fit rather than garbage.
    REQUIRE(AttitudeTrack::fitClock({}).n == 0);
    REQUIRE(AttitudeTrack::fitClock({obs[0]}).n == 0);
    REQUIRE(AttitudeTrack::fitClock({obs[0], {std::nan(""), 1.0, 17}}).n == 0);
}

// -----------------------------------------------------------------------------
//  ConventionProbe on synthetic gravity data
// -----------------------------------------------------------------------------
TEST_CASE("ConventionProbe recovers every convention from synthetic gravity", "[attitude][probe]") {
    std::mt19937_64 rng(2026u);
    std::normal_distribution<double> noise(0.0, 0.02);
    for (const AttitudeConvention& truth : ConventionProbe::candidates()) {
        // Random orientations; the accelerometer sees world-up in the body
        // frame (plus a little noise) - as +g, then as -g to exercise the flip.
        for (const double sign : {1.0, -1.0}) {
            std::vector<ProbeSample> samples;
            const Vec3d up = worldUpVector(truth.up);
            for (int n = 0; n < 200; ++n) {
                const Quatd worldFromBody = randomQuat(rng);
                const Vec3d accBody = worldFromBody.conj().rotate(up) * (9.81 * sign) +
                                      Vec3d{noise(rng), noise(rng), noise(rng)};
                samples.push_back({encode(worldFromBody, truth), Vec3f(accBody)});
            }
            const std::vector<ConventionScore> scores = ConventionProbe::scoreAll(samples);
            REQUIRE(scores.size() == 16);
            const ConventionScore best = ConventionProbe::best(scores);
            INFO("truth " << attitudeConventionName(truth) << " sign " << sign << " best "
                          << attitudeConventionName(best.conv) << " mean " << best.meanGravityAngleDeg);
            REQUIRE(sameUpToSign(best.conv, truth));
            REQUIRE(best.framesUsed == 200);
            REQUIRE(best.meanGravityAngleDeg < 1.0);
            // The sign flag absorbs whichever of the two equivalent readings
            // won: the same up axis sees the flip directly, the opposite axis
            // sees it inverted.
            const bool expectFlip = sameConvention(best.conv, truth) ? (sign < 0.0) : (sign > 0.0);
            REQUIRE(best.accSignFlipped == expectFlip);
            // Every wrong reading is far off (random orientations average
            // ~90 deg folded to ~45 deg).
            for (const ConventionScore& s : scores) {
                if (!sameUpToSign(s.conv, truth)) {
                    REQUIRE(s.meanGravityAngleDeg > 8.0);
                }
            }
        }
    }
    // No data: a default score with zero frames.
    const ConventionScore none = ConventionProbe::best(ConventionProbe::scoreAll(std::vector<ProbeSample>{}));
    REQUIRE(none.framesUsed == 0);
    // Frames without attitude or with a zero acc are ignored.
    std::vector<ProbeSample> junk;
    junk.push_back({meta::Quaternion{}, Vec3f{0.0f, 0.0f, 1.0f}});
    junk.push_back({encode(Quatd::identity(), AttitudeConvention{}), Vec3f{0.0f, 0.0f, 0.0f}});
    REQUIRE(ConventionProbe::score(junk, AttitudeConvention{}).framesUsed == 0);
}

// -----------------------------------------------------------------------------
//  Stabilisation
// -----------------------------------------------------------------------------
TEST_CASE("Stabilization corrections: Off, Full, HorizonLock, Smooth", "[attitude][stab]") {
    const Vec3d worldUp{0.0, 0.0, 1.0};
    const Quatd reference = Quatd::fromAxisAngle(Vec3d{0.0, 0.0, 1.0}, deg2rad(30.0));  // heading 30 deg
    std::mt19937_64 rng(5u);
    const Quatd moved = randomQuat(rng);
    StabilizationParams params;

    // Off: identity whatever the motion.
    params.mode = StabilizationMode::Off;
    REQUIRE(stabilizationBodyFromWorld(moved, params, reference, worldUp).distance(Mat3d::identity()) == 0.0);

    // Full: identity on the reference frame itself ...
    params.mode = StabilizationMode::Full;
    REQUIRE(stabilizationBodyFromWorld(reference, params, reference, worldUp).distance(Mat3d::identity()) < 1e-12);
    // ... and otherwise it glues the view to the reference pose: a view ray
    // mapped into the current body and then to world equals the same ray
    // mapped through the reference pose.
    {
        const Mat3d c = stabilizationBodyFromWorld(moved, params, reference, worldUp);
        const Vec3d ray{0.3, 0.9, -0.1};
        const Vec3d viaCurrent = moved.toMatrix() * (c * ray);
        const Vec3d viaReference = reference.toMatrix() * ray;
        REQUIRE((viaCurrent - viaReference).norm() < 1e-12);
        REQUIRE((c * c.transposed()).distance(Mat3d::identity()) < 1e-12);
    }

    // HorizonLock: a body rolled 10 deg about its forward axis is levelled;
    // its heading is kept.
    params.mode = StabilizationMode::HorizonLock;
    {
        const Quatd rolled = Quatd::fromAxisAngle(Vec3d{0.0, 1.0, 0.0}, deg2rad(10.0));
        const Mat3d c = stabilizationBodyFromWorld(rolled, params, Quatd::identity(), worldUp);
        // The view's up axis must coincide with the world up seen from the body.
        const Vec3d viewUpInBody = c * Vec3d{0.0, 0.0, 1.0};
        const Vec3d worldUpInBody = rolled.toMatrix().transposed() * worldUp;
        REQUIRE((viewUpInBody - worldUpInBody).norm() < 1e-12);
        // The view's forward axis is horizontal in the world.
        const Vec3d viewForwardWorld = rolled.toMatrix() * (c * Vec3d{0.0, 1.0, 0.0});
        REQUIRE_THAT(viewForwardWorld.dot(worldUp), Catch::Matchers::WithinAbs(0.0, 1e-12));
        REQUIRE_THAT(viewForwardWorld.y, Catch::Matchers::WithinAbs(1.0, 1e-12));
        // A body pitched up 20 deg with a 45 deg heading: the heading survives,
        // the pitch is cancelled.
        const Quatd pitched = Quatd::fromAxisAngle(Vec3d{0.0, 0.0, 1.0}, deg2rad(45.0)) *
                              Quatd::fromAxisAngle(Vec3d{1.0, 0.0, 0.0}, deg2rad(20.0));
        const Mat3d c2 = stabilizationBodyFromWorld(pitched, params, Quatd::identity(), worldUp);
        const Vec3d fwdWorld = pitched.toMatrix() * (c2 * Vec3d{0.0, 1.0, 0.0});
        REQUIRE_THAT(fwdWorld.z, Catch::Matchers::WithinAbs(0.0, 1e-12));
        REQUIRE_THAT(rad2deg(std::atan2(-fwdWorld.x, fwdWorld.y)), Catch::Matchers::WithinAbs(45.0, 1e-9));
        // With lockYaw the heading snaps back to the reference heading (0).
        StabilizationParams yawLocked = params;
        yawLocked.lockYaw = true;
        const Mat3d c3 = stabilizationBodyFromWorld(pitched, yawLocked, Quatd::identity(), worldUp);
        const Vec3d fwdWorld3 = pitched.toMatrix() * (c3 * Vec3d{0.0, 1.0, 0.0});
        REQUIRE((fwdWorld3 - Vec3d{0.0, 1.0, 0.0}).norm() < 1e-12);
        // With nothing locked the view follows the body: identity.
        StabilizationParams free = params;
        free.lockPitch = false;
        free.lockRoll = false;
        REQUIRE(stabilizationBodyFromWorld(pitched, free, Quatd::identity(), worldUp).distance(Mat3d::identity()) <
                1e-12);
    }

    // Smooth: with the smoothed orientation equal to the current one the
    // correction is identity; without a smoothed value it is identity too.
    params.mode = StabilizationMode::Smooth;
    REQUIRE(stabilizationBodyFromWorld(moved, params, reference, worldUp, moved).distance(Mat3d::identity()) < 1e-12);
    REQUIRE(stabilizationBodyFromWorld(moved, params, reference, worldUp).distance(Mat3d::identity()) == 0.0);
    // Degenerate input never produces NaNs.
    params.mode = StabilizationMode::Full;
    const Quatd nanQ{std::nan(""), 0.0, 0.0, 0.0};
    REQUIRE(stabilizationBodyFromWorld(nanQ, params, reference, worldUp).distance(Mat3d::identity()) == 0.0);
    REQUIRE(std::string(stabilizationModeName(StabilizationMode::HorizonLock)) == "HorizonLock");
}

// -----------------------------------------------------------------------------
//  SmoothLevel: RockSteady and Horizon Leveling at the same time
// -----------------------------------------------------------------------------
namespace {

/// The view's world heading in degrees (the angle of its forward axis about
/// world +Z, measured from +Y towards -X, as VirtualCamera's yaw).
double viewHeadingDeg(const Quatd& worldFromBody, const Mat3d& correction) {
    const Vec3d fwd = worldFromBody.toMatrix() * (correction * Vec3d{0.0, 1.0, 0.0});
    return rad2deg(std::atan2(-fwd.x, fwd.y));
}

/// Angle in degrees between the view's up axis in the world and world up:
/// zero for a level horizon.
double viewTiltDeg(const Quatd& worldFromBody, const Mat3d& correction, const Vec3d& worldUp) {
    const Vec3d up = worldFromBody.toMatrix() * (correction * Vec3d{0.0, 0.0, 1.0});
    return rad2deg(up.angleTo(worldUp));
}

/// Body pose from a heading, pitch and roll in degrees (VirtualCamera order).
Quatd poseDeg(double headingDeg, double pitchDeg, double rollDeg) {
    return Quatd::fromAxisAngle(Vec3d{0.0, 0.0, 1.0}, deg2rad(headingDeg)) *
           Quatd::fromAxisAngle(Vec3d{1.0, 0.0, 0.0}, deg2rad(pitchDeg)) *
           Quatd::fromAxisAngle(Vec3d{0.0, 1.0, 0.0}, deg2rad(rollDeg));
}

}  // namespace

TEST_CASE("Stabilization SmoothLevel levels pitch and roll and follows the smoothed heading", "[attitude][stab]") {
    const Vec3d worldUp{0.0, 0.0, 1.0};
    StabilizationParams level;
    level.mode = StabilizationMode::SmoothLevel;
    StabilizationParams horizon;
    horizon.mode = StabilizationMode::HorizonLock;

    // Only the two smoothing modes read the smoothed sequence.
    REQUIRE(stabilizationUsesSmoothing(StabilizationMode::Smooth));
    REQUIRE(stabilizationUsesSmoothing(StabilizationMode::SmoothLevel));
    REQUIRE_FALSE(stabilizationUsesSmoothing(StabilizationMode::Off));
    REQUIRE_FALSE(stabilizationUsesSmoothing(StabilizationMode::HorizonLock));
    REQUIRE_FALSE(stabilizationUsesSmoothing(StabilizationMode::Full));
    REQUIRE(std::string(stabilizationModeName(StabilizationMode::SmoothLevel)) == "SmoothLevel");

    SECTION("one frame: the heading is the smoothed one, pitch and roll are level") {
        // A shaken body (heading 40, pitched and rolled) whose smoothed pose
        // heads 30 deg with a different tilt.  The reference heading (25 deg)
        // only sets the levelled basis and must not leak into the result.
        const Quatd body = poseDeg(40.0, 15.0, -8.0);
        const Quatd smoothed = poseDeg(30.0, 5.0, 3.0);
        const Quatd reference = poseDeg(25.0, 0.0, 0.0);
        const Mat3d c = stabilizationBodyFromWorld(body, level, reference, worldUp, smoothed);
        // A proper rotation.
        REQUIRE((c * c.transposed()).distance(Mat3d::identity()) < 1e-12);
        // Level: the view's up is world up, its forward is horizontal.
        REQUIRE(viewTiltDeg(body, c, worldUp) < 1e-9);
        const Vec3d fwd = body.toMatrix() * (c * Vec3d{0.0, 1.0, 0.0});
        REQUIRE_THAT(fwd.z, Catch::Matchers::WithinAbs(0.0, 1e-12));
        // The smoothed heading, not the body's 40 deg.
        REQUIRE_THAT(viewHeadingDeg(body, c), Catch::Matchers::WithinAbs(30.0, 1e-9));
        // HorizonLock on the same body keeps the body's own heading.
        const Mat3d h = stabilizationBodyFromWorld(body, horizon, reference, worldUp);
        REQUIRE_THAT(viewHeadingDeg(body, h), Catch::Matchers::WithinAbs(40.0, 1e-9));
        // Smooth on the same pair keeps the smoothed tilt: not level.
        StabilizationParams smooth;
        smooth.mode = StabilizationMode::Smooth;
        const Mat3d s = stabilizationBodyFromWorld(body, smooth, reference, worldUp, smoothed);
        REQUIRE(viewTiltDeg(body, s, worldUp) > 4.0);
    }

    SECTION("equals HorizonLock when the orientation is already smooth or no smoothed pose is given") {
        std::mt19937_64 rng(17u);
        for (int n = 0; n < 50; ++n) {
            const Quatd body = randomQuat(rng);
            const Quatd reference = randomQuat(rng);
            const Mat3d h = stabilizationBodyFromWorld(body, horizon, reference, worldUp);
            // Smoothed == body (a still camera, or smoothing that changed nothing).
            REQUIRE(stabilizationBodyFromWorld(body, level, reference, worldUp, body).distance(h) < 1e-12);
            // No smoothed pose, or one that is not finite: the horizon lock of the body.
            REQUIRE(stabilizationBodyFromWorld(body, level, reference, worldUp).distance(h) < 1e-12);
            const Quatd nanQ{std::nan(""), 0.0, 0.0, 0.0};
            REQUIRE(stabilizationBodyFromWorld(body, level, reference, worldUp, nanQ).distance(h) < 1e-12);
        }
        // A non-finite body is identity, as for every mode.
        const Quatd nanQ{std::nan(""), 0.0, 0.0, 0.0};
        REQUIRE(stabilizationBodyFromWorld(nanQ, level, Quatd::identity(), worldUp, Quatd::identity())
                    .distance(Mat3d::identity()) == 0.0);
    }

    SECTION("the axis locks mean what they mean for HorizonLock") {
        // Yaw locked too: the smoothed heading no longer matters, the view
        // holds the reference heading exactly as HorizonLock does.
        StabilizationParams levelYaw = level;
        levelYaw.lockYaw = true;
        StabilizationParams horizonYaw = horizon;
        horizonYaw.lockYaw = true;
        const Quatd body = poseDeg(40.0, 15.0, -8.0);
        const Quatd smoothed = poseDeg(30.0, 5.0, 3.0);
        const Quatd reference = poseDeg(25.0, 0.0, 0.0);
        const Mat3d a = stabilizationBodyFromWorld(body, levelYaw, reference, worldUp, smoothed);
        const Mat3d b = stabilizationBodyFromWorld(body, horizonYaw, reference, worldUp);
        REQUIRE(a.distance(b) < 1e-12);
        REQUIRE_THAT(viewHeadingDeg(body, a), Catch::Matchers::WithinAbs(25.0, 1e-9));
        // Nothing locked: the view is the smoothed pose itself, as Smooth.
        StabilizationParams free = level;
        free.lockPitch = false;
        free.lockRoll = false;
        StabilizationParams smooth;
        smooth.mode = StabilizationMode::Smooth;
        REQUIRE(stabilizationBodyFromWorld(body, free, reference, worldUp, smoothed)
                    .distance(stabilizationBodyFromWorld(body, smooth, reference, worldUp, smoothed)) < 1e-12);
    }

    SECTION("a shaky pan: the shake leaves the heading, the horizon stays level") {
        // A steady 0.5 deg / frame pan with +-3 deg of heading shake, a
        // camera held 10 deg nose-up with +-2 deg of pitch shake and +-1.5
        // deg of roll shake, alternating every frame.
        constexpr int kFrames = 121;
        std::vector<Quatd> bodies;
        bodies.reserve(kFrames);
        for (int k = 0; k < kFrames; ++k) {
            const double shake = (k % 2 == 0) ? 1.0 : -1.0;
            bodies.push_back(poseDeg(0.5 * k + 3.0 * shake, 10.0 + 2.0 * shake, -1.5 * shake));
        }
        const std::vector<Quatd> smoothed = Smoother(15.0).smooth(bodies);
        REQUIRE(smoothed.size() == bodies.size());
        const Quatd reference = bodies.front();
        double worstLevelDeg = 0.0;
        double worstHeadingErrLevel = 0.0;
        double worstHeadingErrHorizon = 0.0;
        double leastSmoothTiltDeg = 180.0;
        StabilizationParams smooth;
        smooth.mode = StabilizationMode::Smooth;
        for (int k = 0; k < kFrames; ++k) {
            const Quatd& body = bodies[static_cast<std::size_t>(k)];
            const Quatd& sm = smoothed[static_cast<std::size_t>(k)];
            const Mat3d c = stabilizationBodyFromWorld(body, level, reference, worldUp, sm);
            worstLevelDeg = std::max(worstLevelDeg, viewTiltDeg(body, c, worldUp));
            // Away from the ends, where the window is whole, the heading is
            // the pan itself: the Gaussian mean of the shake is ~0.
            if (k >= 45 && k <= kFrames - 46) {
                const double pan = 0.5 * k;
                worstHeadingErrLevel = std::max(worstHeadingErrLevel, std::fabs(viewHeadingDeg(body, c) - pan));
                const Mat3d h = stabilizationBodyFromWorld(body, horizon, reference, worldUp);
                worstHeadingErrHorizon = std::max(worstHeadingErrHorizon, std::fabs(viewHeadingDeg(body, h) - pan));
                const Mat3d s = stabilizationBodyFromWorld(body, smooth, reference, worldUp, sm);
                leastSmoothTiltDeg = std::min(leastSmoothTiltDeg, viewTiltDeg(body, s, worldUp));
            }
        }
        // Level on every frame, the ends included.
        REQUIRE(worstLevelDeg < 1e-9);
        // The shake is gone from the heading ...
        REQUIRE(worstHeadingErrLevel < 0.25);
        // ... where HorizonLock carries all 3 deg of it ...
        REQUIRE(worstHeadingErrHorizon > 2.9);
        // ... and Smooth alone keeps the 10 deg nose-up tilt.
        REQUIRE(leastSmoothTiltDeg > 9.0);
    }

    SECTION("smoothing disabled: every frame is HorizonLock") {
        std::mt19937_64 rng(23u);
        std::vector<Quatd> bodies;
        for (int k = 0; k < 20; ++k) {
            bodies.push_back(randomQuat(rng));
        }
        // sigma <= 0 hands the input straight back.
        const std::vector<Quatd> smoothed = Smoother(0.0).smooth(bodies);
        REQUIRE(smoothed.size() == bodies.size());
        for (std::size_t k = 0; k < bodies.size(); ++k) {
            const Mat3d c = stabilizationBodyFromWorld(bodies[k], level, bodies.front(), worldUp, smoothed[k]);
            const Mat3d h = stabilizationBodyFromWorld(bodies[k], horizon, bodies.front(), worldUp);
            REQUIRE(c.distance(h) < 1e-12);
        }
    }
}

TEST_CASE("Quaternion log/exp and ZXY Euler round trips", "[attitude][stab]") {
    std::mt19937_64 rng(9u);
    for (int n = 0; n < 200; ++n) {
        const Quatd q = randomQuat(rng);
        // angleTo() resolves only ~3e-8 near identity (acos of 1 - eps), so
        // compare components, allowing for the q / -q sign ambiguity.
        const Quatd back = quatExp(quatLog(q));
        const double dPlus = std::fabs(back.w - q.w) + std::fabs(back.x - q.x) + std::fabs(back.y - q.y) +
                             std::fabs(back.z - q.z);
        const double dMinus = std::fabs(back.w + q.w) + std::fabs(back.x + q.x) + std::fabs(back.y + q.y) +
                              std::fabs(back.z + q.z);
        REQUIRE(std::min(dPlus, dMinus) < 1e-12);
        const Vec3d v = quatLog(q);
        REQUIRE(v.norm() <= kPi + 1e-12);
        // Euler decomposition of a rotation built from the same angles.
        const Mat3d r = q.toMatrix();
        const EulerZXY e = eulerZXY(r);
        REQUIRE(fromEulerZXY(e).distance(r) < 1e-9);
    }
    // Known angles come back as given.
    EulerZXY given;
    given.yaw = deg2rad(20.0);
    given.pitch = deg2rad(-35.0);
    given.roll = deg2rad(12.0);
    const EulerZXY back = eulerZXY(fromEulerZXY(given));
    REQUIRE_THAT(back.yaw, Catch::Matchers::WithinAbs(given.yaw, 1e-12));
    REQUIRE_THAT(back.pitch, Catch::Matchers::WithinAbs(given.pitch, 1e-12));
    REQUIRE_THAT(back.roll, Catch::Matchers::WithinAbs(given.roll, 1e-12));
    // Small and zero rotations.
    REQUIRE(quatLog(Quatd::identity()).norm() == 0.0);
    REQUIRE(quatExp(Vec3d{}).angleTo(Quatd::identity()) == 0.0);
    REQUIRE(quatExp(Vec3d{std::nan(""), 0.0, 0.0}).angleTo(Quatd::identity()) == 0.0);
    const Vec3d tiny{1e-9, -2e-9, 3e-9};
    REQUIRE((quatLog(quatExp(tiny)) - tiny).norm() < 1e-15);
}

TEST_CASE("Smoother: Gaussian log-map window", "[attitude][stab]") {
    // A constant sequence is unchanged.
    const Quatd constant = Quatd::fromAxisAngle(Vec3d{1.0, 2.0, 3.0}, 0.7);
    std::vector<Quatd> flat(50, constant);
    const Smoother smoother(5.0);
    REQUIRE(smoother.halfWidth() == 15);
    for (const Quatd& q : smoother.smooth(flat)) {
        REQUIRE(q.angleTo(constant) < 1e-12);
    }
    // A slow rotation with jitter: the smoothed track stays close to the
    // underlying slow rotation and is smoother than the input.
    std::mt19937_64 rng(3u);
    std::normal_distribution<double> jitter(0.0, deg2rad(2.0));
    std::vector<Quatd> noisy;
    std::vector<Quatd> clean;
    for (int i = 0; i < 300; ++i) {
        const Quatd base = Quatd::fromAxisAngle(Vec3d{0.0, 0.0, 1.0}, deg2rad(0.2 * i));
        clean.push_back(base);
        noisy.push_back(base * quatExp(Vec3d{jitter(rng), jitter(rng), jitter(rng)}));
    }
    const std::vector<Quatd> smoothed = smoother.smooth(noisy);
    REQUIRE(smoothed.size() == noisy.size());
    double noisyErr = 0.0;
    double smoothErr = 0.0;
    for (std::size_t i = 20; i + 20 < noisy.size(); ++i) {
        noisyErr += noisy[i].angleTo(clean[i]);
        smoothErr += smoothed[i].angleTo(clean[i]);
    }
    REQUIRE(smoothErr < 0.5 * noisyErr);
    // Sigma <= 0 disables smoothing; out-of-range index is identity.
    const Smoother off(0.0);
    REQUIRE(off.halfWidth() == 0);
    REQUIRE(off.smoothedAt(noisy, 7).angleTo(noisy[7]) < 1e-12);
    REQUIRE(smoother.smoothedAt(noisy, 999).angleTo(Quatd::identity()) == 0.0);
    REQUIRE(smoother.smooth({}).empty());
}

// -----------------------------------------------------------------------------
//  Sample clip
// -----------------------------------------------------------------------------
TEST_CASE("AttitudeTrack sparse track reproduces the per-frame attitude", "[attitude][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::unique_ptr<SampleMeta> sample = openSample();
    const SampleMeta& s = *sample;
    REQUIRE(s.track.frameCount() == 65);

    AttitudeTrack::Options options;
    auto built = AttitudeTrack::build(s.track, options);
    REQUIRE(built.ok());
    const AttitudeTrack& track = built.value();
    REQUIRE(track.sampleCount() == 65);
    REQUIRE(track.beginUs() < track.endUs());

    for (std::uint32_t i = 0; i < s.track.frameCount(); ++i) {
        const auto frame = s.track.frame(i);
        REQUIRE(frame.ok());
        REQUIRE(frame.value().camera.attitude.present);
        const Quatd expected = worldFromBodyQuat(frame.value().camera.attitude, options.conv);
        const Quatd got = track.worldFromBody(static_cast<double>(frame.value().timestampUs));
        INFO("frame " << i);
        REQUIRE(got.angleTo(expected) < 1e-6);
    }
    // Between frames the track moves a little but never jumps (static clip).
    const double t0 = static_cast<double>(s.track.frame(0).value().timestampUs);
    REQUIRE(track.worldFromBody(t0 + 0.5 * kFramePeriodUs).angleTo(track.worldFromBody(t0)) < deg2rad(0.5));
}

TEST_CASE("AttitudeTrack dense track: anchor 4 lands on the frame attitude", "[attitude][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::unique_ptr<SampleMeta> sample = openSample();
    const SampleMeta& s = *sample;

    AttitudeTrack::Options options;
    options.dense = true;
    options.batchAnchorIndex = 4;
    auto built = AttitudeTrack::build(s.track, options);
    REQUIRE(built.ok());
    const AttitudeTrack& dense = built.value();
    // 16-17 quaternions per frame.
    REQUIRE(dense.sampleCount() >= 65u * 16u);
    REQUIRE(dense.sampleCount() <= 65u * 17u);

    for (std::uint32_t i = 0; i < s.track.frameCount(); ++i) {
        const auto frame = s.track.frame(i);
        REQUIRE(frame.ok());
        REQUIRE(frame.value().imu.has_value());
        REQUIRE(frame.value().imu->current.q.size() > 4);
        const Quatd expected = worldFromBodyQuat(frame.value().camera.attitude, options.conv);
        const Quatd got = dense.worldFromBody(static_cast<double>(frame.value().timestampUs));
        INFO("frame " << i);
        REQUIRE(got.angleTo(expected) < 1e-4);
    }
    // Dense mode needs the IMU rate.
    REQUIRE(s.track.clip().imuSamplingRate == 1000);
}

TEST_CASE("AttitudeTrack clock fit: 1006 ticks per IMU sample", "[attitude][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::unique_ptr<SampleMeta> sample = openSample();
    const SampleMeta& s = *sample;
    auto built = AttitudeTrack::build(s.track, AttitudeTrack::Options{});
    REQUIRE(built.ok());
    const AttitudeTrack::ClockFit fit = built.value().clockFit();
    REQUIRE(fit.n == 65);
    // The attitude clock advances 1006 ticks per ~1 kHz IMU sample and runs
    // at the same rate as the frame timestamps (about 16684 ticks per frame
    // period).
    REQUIRE_THAT(fit.ticksPerSample, Catch::Matchers::WithinAbs(1006.0, 5.0));
    REQUIRE_THAT(fit.ticksPerUs, Catch::Matchers::WithinAbs(1.0, 1e-3));
    REQUIRE_THAT(fit.ticksPerUs * kFramePeriodUs, Catch::Matchers::WithinAbs(16683.5, 20.0));
    // Each batch starts on the first IMU sample after the frame time, so the
    // residual is uniform over one 1 ms sample period: rms ~ 1000 / sqrt(12)
    // = 289 us.  Anything much larger would mean the clocks drift apart.
    REQUIRE(fit.rmsUs < 400.0);
    REQUIRE(fit.rmsUs > 0.0);
}

TEST_CASE("ConventionProbe on the sample clip", "[attitude][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::unique_ptr<SampleMeta> sample = openSample();
    const SampleMeta& s = *sample;

    // camera_acc on this (static) clip is a motion measurement, not a clean
    // gravity vector: its magnitude swings between 0.4 and 3.4 while the
    // attitude drifts 1.35 deg over the whole clip, so no reading scores
    // well against it.  The probe must still run over every frame and
    // return a well-formed ranking.
    const std::vector<ConventionScore> scores = ConventionProbe::scoreAll(s.track);
    REQUIRE(scores.size() == 16);
    for (const ConventionScore& sc : scores) {
        REQUIRE(sc.framesUsed == 65);
        REQUIRE(std::isfinite(sc.meanGravityAngleDeg));
        REQUIRE(sc.meanGravityAngleDeg >= 0.0);
        REQUIRE(sc.meanGravityAngleDeg <= 90.0);
        REQUIRE(sc.stdDeg >= 0.0);
    }
    const ConventionScore best = ConventionProbe::best(s.track);
    REQUIRE(best.framesUsed == 65);
    for (const ConventionScore& sc : scores) {
        REQUIRE(best.meanGravityAngleDeg <= sc.meanGravityAngleDeg);
    }

    // Discriminating check (verified by rendering with horizon lock): the
    // sample clip was shot from an aircraft with the lens axes horizontal and
    // the sky centred on the body -X direction.  Under the default reading
    // (XYZW, body->world, world up = -Y) the world up vector lands near body
    // -X on every frame; the previously assumed reading (XYZW, world->body,
    // +Y up) does not level the picture at all.
    const auto frame0 = s.track.frame(0);
    REQUIRE(frame0.ok());
    const meta::Quaternion att = frame0.value().camera.attitude;
    REQUIRE(att.present);
    REQUIRE_THAT(att.w, Catch::Matchers::WithinAbs(0.46131432, 1e-6));
    const AttitudeConvention documented{};
    REQUIRE(documented.order == QuatOrder::XYZW);
    REQUIRE(documented.sense == AttitudeSense::BodyToWorld);
    REQUIRE(documented.up == WorldUp::NegY);
    const Vec3d skyBody{-1.0, 0.0, 0.0};
    const Vec3d upDocumented = bodyFromWorldMatrix(att, documented) * worldUpVector(documented.up);
    INFO("world up in body frame: " << upDocumented.x << ", " << upDocumented.y << ", " << upDocumented.z);
    REQUIRE(rad2deg(upDocumented.angleTo(skyBody)) < 15.0);
    const AttitudeConvention previous{QuatOrder::XYZW, AttitudeSense::WorldToBody, WorldUp::Y};
    const Vec3d upPrevious = bodyFromWorldMatrix(att, previous) * worldUpVector(WorldUp::Y);
    REQUIRE(rad2deg(upPrevious.angleTo(skyBody)) > 30.0);
    // And it holds for every frame of the clip, not just the first.
    for (std::uint32_t i = 0; i < s.track.frameCount(); ++i) {
        const auto f = s.track.frame(i);
        REQUIRE(f.ok());
        const Vec3d up = bodyFromWorldMatrix(f.value().camera.attitude, documented) * worldUpVector(documented.up);
        REQUIRE(rad2deg(up.angleTo(skyBody)) < 15.0);
    }
}

TEST_CASE("Stabilization Full on the sample clip is identity at the reference frame", "[attitude][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::unique_ptr<SampleMeta> sample = openSample();
    const SampleMeta& s = *sample;
    auto built = AttitudeTrack::build(s.track, AttitudeTrack::Options{});
    REQUIRE(built.ok());
    const AttitudeTrack& track = built.value();
    const Quatd reference = track.worldFromBody(track.beginUs());
    StabilizationParams params;
    params.mode = StabilizationMode::Full;
    REQUIRE(stabilizationBodyFromWorld(reference, params, reference, track.worldUp()).distance(Mat3d::identity()) <
            1e-12);
    // The last frame drifted about 1.35 deg from the first: the correction
    // is a rotation of that size, not identity and not garbage.
    const Quatd last = track.worldFromBody(track.endUs());
    const Mat3d c = stabilizationBodyFromWorld(last, params, reference, track.worldUp());
    const double angleDeg = rad2deg(Quatd::fromMatrix(c).angleTo(Quatd::identity()));
    REQUIRE(angleDeg > 0.5);
    REQUIRE(angleDeg < 3.0);
    // HorizonLock with the documented Y-up convention keeps the view level.
    params.mode = StabilizationMode::HorizonLock;
    const Mat3d h = stabilizationBodyFromWorld(last, params, reference, track.worldUp());
    const Vec3d viewUpWorld = last.toMatrix() * (h * Vec3d{0.0, 0.0, 1.0});
    REQUIRE(rad2deg(viewUpWorld.angleTo(track.worldUp())) < 1e-6);
}

TEST_CASE("Stabilization SmoothLevel on the sample clip is level on every frame and steadier in heading",
          "[attitude][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::unique_ptr<SampleMeta> sample = openSample();
    auto built = AttitudeTrack::build(sample->track, AttitudeTrack::Options{});
    REQUIRE(built.ok());
    const AttitudeTrack& track = built.value();
    REQUIRE(track.sampleCount() > 2);
    // The per-sample sequence and its smoothing, exactly as the importer
    // builds them (ImporterInstance::rebuildStabilization).
    std::vector<Quatd> bodies;
    for (const auto& s : track.samples()) {
        bodies.push_back(s.worldFromBody);
    }
    StabilizationParams level;
    level.mode = StabilizationMode::SmoothLevel;
    StabilizationParams horizon;
    horizon.mode = StabilizationMode::HorizonLock;
    const std::vector<Quatd> smoothed = Smoother(level.smoothSigmaFrames).smooth(bodies);
    REQUIRE(smoothed.size() == bodies.size());
    const Quatd reference = track.worldFromBody(track.beginUs());
    const Vec3d up = track.worldUp();

    // The view's forward axis in the world, on the horizontal plane.
    const auto forward = [](const Quatd& body, const Mat3d& c) {
        return body.toMatrix() * (c * Vec3d{0.0, 1.0, 0.0});
    };
    double worstTiltDeg = 0.0;
    double levelTravelDeg = 0.0;
    double horizonTravelDeg = 0.0;
    Vec3d prevLevel;
    Vec3d prevHorizon;
    for (std::size_t k = 0; k < bodies.size(); ++k) {
        const Mat3d c = stabilizationBodyFromWorld(bodies[k], level, reference, up, smoothed[k]);
        const Mat3d hz = stabilizationBodyFromWorld(bodies[k], horizon, reference, up);
        worstTiltDeg = std::max(worstTiltDeg, viewTiltDeg(bodies[k], c, up));
        // Frame-to-frame heading change: how much the view swings.
        const Vec3d fl = forward(bodies[k], c);
        const Vec3d fh = forward(bodies[k], hz);
        if (k > 0) {
            levelTravelDeg += rad2deg(fl.angleTo(prevLevel));
            horizonTravelDeg += rad2deg(fh.angleTo(prevHorizon));
        }
        prevLevel = fl;
        prevHorizon = fh;
    }
    INFO("heading travel: smooth + horizon lock " << levelTravelDeg << " deg, horizon lock " << horizonTravelDeg
                                                  << " deg");
    // Level on every sample, like HorizonLock ...
    REQUIRE(worstTiltDeg < 1e-6);
    // ... and the view swings no more than HorizonLock's, which carries the
    // body's heading shake.
    REQUIRE(levelTravelDeg <= horizonTravelDeg + 1e-9);
}
