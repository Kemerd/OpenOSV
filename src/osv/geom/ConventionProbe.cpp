// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ConventionProbe: gravity-direction scoring of the attitude conventions, and
// the `auto` reading with its accelerometer canary.

#include "osv/geom/ConventionProbe.h"

#include "osv/core/Log.h"
#include "osv/meta/MetadataTrack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace osv::geom {

std::vector<AttitudeConvention> ConventionProbe::candidates() {
    std::vector<AttitudeConvention> out;
    out.reserve(16);
    // Fixed enumeration order: order-major, then sense, then up axis.  The
    // candidates are the plain readings (the IMU's axes taken as the rig's):
    // the score compares the body-frame gravity with camera_acc in its own
    // axes, which only means something when both use the same axes.
    for (const QuatOrder order : {QuatOrder::WXYZ, QuatOrder::XYZW}) {
        for (const AttitudeSense sense : {AttitudeSense::WorldToBody, AttitudeSense::BodyToWorld}) {
            for (const WorldUp up : {WorldUp::Y, WorldUp::Z, WorldUp::NegY, WorldUp::NegZ}) {
                out.push_back(AttitudeConvention{order, sense, up, false});
            }
        }
    }
    return out;
}

ConventionScore ConventionProbe::score(const std::vector<ProbeSample>& samples,
                                       const AttitudeConvention& convention) noexcept {
    ConventionScore result;
    result.conv = convention;
    const Vec3d up = worldUpVector(convention.up);

    // Welford-style accumulation of the folded angle plus a flip count.
    double sum = 0.0;
    double sumSq = 0.0;
    std::size_t used = 0;
    std::size_t flipped = 0;
    for (const ProbeSample& s : samples) {
        if (!s.attitude.present) {
            continue;
        }
        const Vec3d acc = s.acc.toDouble();
        if (!acc.isFinite() || !(acc.norm() > 0.0)) {
            continue;
        }
        // Predicted gravity direction in the body frame under this reading.
        const Vec3d predictedUp = bodyFromWorldMatrix(s.attitude, convention) * up;
        const double angleDeg = rad2deg(predictedUp.angleTo(acc));
        if (!std::isfinite(angleDeg)) {
            continue;
        }
        // The accelerometer may report +g or -g along up; fold the angle
        // and remember which sign the frame voted for.
        const bool flip = angleDeg > 90.0;
        const double folded = flip ? 180.0 - angleDeg : angleDeg;
        sum += folded;
        sumSq += folded * folded;
        ++used;
        if (flip) {
            ++flipped;
        }
    }

    result.framesUsed = used;
    if (used == 0) {
        return result;
    }
    const double n = static_cast<double>(used);
    result.meanGravityAngleDeg = sum / n;
    const double variance = sumSq / n - result.meanGravityAngleDeg * result.meanGravityAngleDeg;
    result.stdDeg = variance > 0.0 ? std::sqrt(variance) : 0.0;
    result.accSignFlipped = flipped * 2 > used;
    return result;
}

std::vector<ConventionScore> ConventionProbe::scoreAll(const std::vector<ProbeSample>& samples) {
    std::vector<ConventionScore> scores;
    const std::vector<AttitudeConvention> all = candidates();
    scores.reserve(all.size());
    for (const AttitudeConvention& c : all) {
        scores.push_back(score(samples, c));
    }
    return scores;
}

std::vector<ConventionScore> ConventionProbe::scoreAll(const meta::MetadataTrack& track, std::size_t maxFrames) {
    // Gather (attitude, acc) pairs from the first maxFrames frames.
    std::vector<ProbeSample> samples;
    const std::size_t count = std::min(static_cast<std::size_t>(track.frameCount()), maxFrames);
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Result<meta::FrameMeta> frame = track.frame(static_cast<std::uint32_t>(i));
        if (!frame.ok()) {
            continue;
        }
        const meta::FrameMeta& f = frame.value();
        if (!f.camera.attitude.present || !f.camera.accPresent) {
            continue;
        }
        samples.push_back(ProbeSample{f.camera.attitude, f.camera.acc});
    }
    return scoreAll(samples);
}

ConventionScore ConventionProbe::best(const std::vector<ConventionScore>& scores) noexcept {
    ConventionScore bestScore;
    bool have = false;
    for (const ConventionScore& s : scores) {
        // Scores without frames carry no information.
        if (s.framesUsed == 0 || !std::isfinite(s.meanGravityAngleDeg)) {
            continue;
        }
        if (!have || s.meanGravityAngleDeg < bestScore.meanGravityAngleDeg ||
            (s.meanGravityAngleDeg == bestScore.meanGravityAngleDeg && s.stdDeg < bestScore.stdDeg)) {
            bestScore = s;
            have = true;
        }
    }
    return bestScore;
}

ConventionScore ConventionProbe::best(const meta::MetadataTrack& track, std::size_t maxFrames) {
    return best(scoreAll(track, maxFrames));
}

// -----------------------------------------------------------------------------
//  autoDetect: the reading `auto` resolves to
// -----------------------------------------------------------------------------
//
//  The default reading's world is level (+Z up) by construction, so `auto`
//  levels on the attitude alone, on every clip, whatever its accelerometer
//  says.  camera_acc is kept as a canary: on a car, a tripod or a hand-held
//  walk its clip mean is the gravity reaction in the body, and that must
//  agree with the body up the reading gives.  A large disagreement on a
//  clean gravity reaction means the reading does not describe this camera
//  (another body or firmware storing its attitude differently), which is
//  worth a warning in every host's log, never a silent re-levelling.
namespace {

/// camera_acc in its own axes -> the rig's body axes (X right, Y forward,
/// Z up): (a_y, a_x, -a_z).  The matrix [[0,1,0],[1,0,0],[0,0,-1]] is a
/// proper rotation (det +1), a half turn about (1, 1, 0) / sqrt(2).
/// Measured on the car clips: the clip mean lands within 1 deg of the
/// default reading's body up there.
[[nodiscard]] Vec3d accelerometerToRig(const Vec3d& a) noexcept { return Vec3d{a.y, a.x, -a.z}; }

/// Fewer frames than this are not a measurement.
constexpr std::size_t kCanaryMinFrames = 8;
/// A clean gravity reaction: the mean near 1 g and every frame within a few
/// tens of degrees of it.  A car or a hand-held walk passes; an aerobatic
/// clip (the airborne sample swings 0.4..3.4 g, spread 59-61 deg) does not,
/// and its canary is reported without a verdict.
constexpr double kCanaryMinG = 0.6;
constexpr double kCanaryMaxG = 1.4;
constexpr double kCanaryMaxSpreadDeg = 35.0;

}  // namespace

AutoConvention ConventionProbe::autoDetect(const std::vector<ProbeSample>& samples) {
    AutoConvention out;
    // The reading is fixed: the levelling never depends on the accelerometer.
    out.conv = AttitudeConvention{};
    out.measuredUp = Vec3d{0.0, 0.0, 0.0};
    const Vec3d worldUp = worldUpVector(out.conv.up);
    const std::string level = std::string("level on the attitude's own up (") + worldUpName(out.conv.up) + ")";

    // ---- the frames the canary can use: an attitude and a usable acc ---------
    // Both clip means run over the same frames, so a clip whose attitude
    // drops out for a while cannot bias one side of the comparison.
    Vec3d accSum{0.0, 0.0, 0.0};
    Vec3d upSum{0.0, 0.0, 0.0};
    std::vector<Vec3d> accs;
    accs.reserve(samples.size());
    for (const ProbeSample& s : samples) {
        if (!s.attitude.present) {
            continue;
        }
        const Vec3d acc = accelerometerToRig(s.acc.toDouble());
        if (!acc.isFinite() || !(acc.norm() > 1e-6)) {
            continue;
        }
        // The reading's up seen from the body: R(t)^T * up.
        const Vec3d bodyUp = bodyFromWorldMatrix(s.attitude, out.conv) * worldUp;
        if (!bodyUp.isFinite()) {
            continue;
        }
        accSum += acc;
        upSum += bodyUp;
        accs.push_back(acc);
    }
    out.framesUsed = accs.size();

    // ---- too little data: level anyway, say why there is no canary -------------
    if (out.framesUsed < kCanaryMinFrames) {
        out.reason = level + "; no accelerometer canary: " + std::to_string(out.framesUsed) +
                     " frames carry both an attitude and an acceleration, " + std::to_string(kCanaryMinFrames) +
                     " needed";
        return out;
    }
    const Vec3d accMean = accSum / static_cast<double>(out.framesUsed);
    out.meanAccG = accMean.norm();
    const double upNorm = upSum.norm();
    if (!(out.meanAccG > 1e-6) || !(upNorm > 1e-6) || !std::isfinite(out.meanAccG) || !std::isfinite(upNorm)) {
        out.reason = level + "; no accelerometer canary: the acceleration or the body up averages to zero";
        return out;
    }
    const Vec3d accDir = accMean / out.meanAccG;
    const Vec3d upDir = upSum / upNorm;

    // ---- how steady the acceleration is (is it gravity at all?) ----------------
    double spread = 0.0;
    for (const Vec3d& a : accs) {
        spread += rad2deg(a.angleTo(accDir));
    }
    out.spreadDeg = spread / static_cast<double>(out.framesUsed);

    // ---- the canary: the accelerometer's gravity against the reading's up -------
    out.canaryDeg = rad2deg(accDir.angleTo(upDir));
    out.canaryMeasured = std::isfinite(out.canaryDeg);
    out.canaryJudged = out.canaryMeasured && out.meanAccG >= kCanaryMinG && out.meanAccG <= kCanaryMaxG &&
                       out.spreadDeg <= kCanaryMaxSpreadDeg;
    out.canaryWarning = out.canaryJudged && out.canaryDeg > kAttitudeCanaryWarnDeg;

    // ---- one line for every host's log ----------------------------------------
    char numbers[160];
    std::snprintf(numbers, sizeof(numbers), "mean %.2f g, spread %.1f deg, %zu frames", out.meanAccG,
                  out.spreadDeg, out.framesUsed);
    char canary[96];
    std::snprintf(canary, sizeof(canary), "gravity %.1f deg from the reading's up", out.canaryDeg);
    if (!out.canaryJudged) {
        out.reason = level + "; accelerometer canary not judged (not a clean gravity reaction: " + numbers +
                     "; " + canary + ")";
        return out;
    }
    if (out.canaryWarning) {
        char limit[32];
        std::snprintf(limit, sizeof(limit), "%.0f", kAttitudeCanaryWarnDeg);
        out.reason = level + "; WARNING accelerometer canary: " + canary + ", more than " + limit +
                     " deg: this camera may store its attitude differently, check the horizon (" + numbers + ")";
        log::warn("attitude: {}", out.reason);
        return out;
    }
    out.reason = level + "; accelerometer canary: " + canary + " (" + numbers + ")";
    return out;
}

AutoConvention ConventionProbe::autoDetect(const meta::MetadataTrack& track, std::size_t maxFrames) {
    // Even sampling over the whole clip, so a launch or a hard brake in the
    // first seconds cannot dominate the canary's means.
    std::vector<ProbeSample> samples;
    const std::size_t count = static_cast<std::size_t>(track.frameCount());
    if (count == 0 || maxFrames == 0) {
        return autoDetect(samples);
    }
    const std::size_t take = std::min(count, maxFrames);
    samples.reserve(take);
    for (std::size_t k = 0; k < take; ++k) {
        // The first frame of each of `take` equal slices of the clip.
        const std::size_t i = take == count ? k : (k * count) / take;
        const Result<meta::FrameMeta> frame = track.frame(static_cast<std::uint32_t>(i));
        if (!frame.ok()) {
            continue;
        }
        const meta::FrameMeta& f = frame.value();
        // The canary compares the two per frame: a frame missing either has
        // nothing to say.
        if (!f.camera.accPresent || !f.camera.attitude.present) {
            continue;
        }
        samples.push_back(ProbeSample{f.camera.attitude, f.camera.acc});
    }
    return autoDetect(samples);
}

}  // namespace osv::geom
