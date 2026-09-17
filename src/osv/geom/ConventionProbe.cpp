// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ConventionProbe: gravity-direction scoring of the attitude conventions.

#include "osv/geom/ConventionProbe.h"

#include "osv/meta/MetadataTrack.h"

#include <algorithm>
#include <cmath>

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

}  // namespace osv::geom
