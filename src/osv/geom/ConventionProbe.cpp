// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ConventionProbe: gravity-direction scoring of the attitude conventions.

#include "osv/geom/ConventionProbe.h"

#include "osv/meta/MetadataTrack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace osv::geom {

std::vector<AttitudeConvention> ConventionProbe::candidates() {
    std::vector<AttitudeConvention> out;
    out.reserve(8);
    // Fixed enumeration order: order-major, then sense, then up axis.
    for (const QuatOrder order : {QuatOrder::WXYZ, QuatOrder::XYZW}) {
        for (const AttitudeSense sense : {AttitudeSense::WorldToBody, AttitudeSense::BodyToWorld}) {
            for (const WorldUp up : {WorldUp::Y, WorldUp::Z, WorldUp::NegY, WorldUp::NegZ}) {
                out.push_back(AttitudeConvention{order, sense, up});
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
//  camera_acc is the specific force in the accelerometer's world frame.  A
//  camera at rest, cruising or riding a car feels 1 g straight up, so the
//  mean of the samples IS the up direction there; one fixed quarter turn
//  about Y carries it into the verified reading's world frame (see
//  AutoConvention).  Order and sense cannot be measured this way and keep the
//  verified default.
namespace {

/// The accelerometer's world frame -> the verified reading's world frame.
/// R = [[0,0,1],[0,1,0],[-1,0,0]], a quarter turn about +Y.
[[nodiscard]] Vec3d accelerometerToWorld(const Vec3d& a) noexcept { return Vec3d{a.z, a.y, -a.x}; }

}  // namespace

AutoConvention ConventionProbe::autoDetect(const std::vector<ProbeSample>& samples) {
    AutoConvention out;
    out.conv = AttitudeConvention{};

    // ---- mean specific force over every usable sample ------------------------
    Vec3d sum{0.0, 0.0, 0.0};
    std::vector<Vec3d> accs;
    accs.reserve(samples.size());
    for (const ProbeSample& s : samples) {
        const Vec3d acc = s.acc.toDouble();
        if (!acc.isFinite() || !(acc.norm() > 1e-6)) {
            continue;
        }
        sum += acc;
        accs.push_back(acc);
    }
    out.framesUsed = accs.size();

    // A handful of frames is not a measurement; keep the default.
    constexpr std::size_t kMinFrames = 8;
    if (out.framesUsed < kMinFrames) {
        out.reason = "level on -Y (the clip carries " + std::to_string(out.framesUsed) +
                     " accelerometer frames, too few to measure gravity)";
        return out;
    }
    const Vec3d mean = sum / static_cast<double>(out.framesUsed);
    out.meanAccG = mean.norm();
    if (!(out.meanAccG > 1e-6)) {
        out.reason = "level on -Y (the accelerometer averages to zero)";
        return out;
    }
    const Vec3d meanDir = mean / out.meanAccG;

    // ---- how steady the direction is -------------------------------------------
    double spread = 0.0;
    for (const Vec3d& a : accs) {
        spread += rad2deg(a.angleTo(meanDir));
    }
    out.spreadDeg = spread / static_cast<double>(out.framesUsed);

    // ---- the measured up in the verified world frame ---------------------------
    const Vec3d up = accelerometerToWorld(meanDir).normalized();
    const Vec3d axis = worldUpVector(out.conv.up);
    out.tiltDeg = rad2deg(up.angleTo(axis));

    // ---- accept only a clean gravity reaction ----------------------------------
    // Thresholds: a car or a hand-held walk keeps the mean near 1 g and every
    // frame within a few tens of degrees; an aerobatic clip (the airborne
    // sample swings 0.4..3.4 g) does not, and then -Y stands.  The frame tilt
    // measured so far is 9..29 deg; past 60 deg the reading is not trusted.
    constexpr double kMinG = 0.6;
    constexpr double kMaxG = 1.4;
    constexpr double kMaxSpreadDeg = 35.0;
    constexpr double kMaxTiltDeg = 60.0;
    const bool clean = out.meanAccG >= kMinG && out.meanAccG <= kMaxG && out.spreadDeg <= kMaxSpreadDeg &&
                       out.tiltDeg <= kMaxTiltDeg && up.isFinite();
    char numbers[192];
    std::snprintf(numbers, sizeof(numbers), "mean %.2f g, spread %.1f deg, %.1f deg from -Y, %zu frames",
                  out.meanAccG, out.spreadDeg, out.tiltDeg, out.framesUsed);
    if (!clean) {
        out.reason = std::string("level on -Y (accelerometer not a clean gravity reaction: ") + numbers + ")";
        return out;
    }
    out.measuredUp = up;
    out.upFromAccelerometer = true;
    out.reason = std::string("level on the measured gravity (") + numbers + ")";
    return out;
}

AutoConvention ConventionProbe::autoDetect(const meta::MetadataTrack& track, std::size_t maxFrames) {
    // Even sampling over the whole clip, so a launch or a hard brake in the
    // first seconds cannot dominate the mean.
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
        if (!f.camera.accPresent) {
            continue;
        }
        samples.push_back(ProbeSample{f.camera.attitude, f.camera.acc});
    }
    return autoDetect(samples);
}

}  // namespace osv::geom
