// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ConventionProbe: score every reading of the attitude quaternion against
// the accelerometer.  For a static or slowly moving camera camera_acc points
// along gravity in the body frame, so the correct convention is the one for
// which R_body_from_world * up_world lines up with the normalised
// acceleration frame after frame.  All 2 x 2 x 2 = 8 combinations of
// component order, rotation sense and world-up axis are scored.
#pragma once

#include "osv/core/Math.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/meta/Types.h"

#include <cstddef>
#include <vector>

namespace osv::meta {
class MetadataTrack;
}

namespace osv::geom {

/// Result of scoring one convention.
struct ConventionScore {
    AttitudeConvention conv;              ///< The reading that was scored.
    double meanGravityAngleDeg = 180.0;   ///< Mean folded angle between predicted up and acc (deg).
    double stdDeg = 0.0;                  ///< Standard deviation of that angle (deg).
    bool accSignFlipped = false;          ///< True when acc points along -up for most frames.
    std::size_t framesUsed = 0;           ///< Frames that carried both attitude and acc.
};

/// One observation: the stored attitude and the raw acceleration of a frame.
struct ProbeSample {
    meta::Quaternion attitude;
    Vec3f acc;
};

/// Scores the eight quaternion conventions.
class ConventionProbe {
public:
    /// The eight candidate conventions in a fixed order.
    [[nodiscard]] static std::vector<AttitudeConvention> candidates();

    /// Collect up to `maxFrames` (attitude, acc) pairs from the track and
    /// score every candidate.  Frames without either value are skipped.
    [[nodiscard]] static std::vector<ConventionScore> scoreAll(const meta::MetadataTrack& track,
                                                               std::size_t maxFrames = 256);

    /// Score every candidate on explicit samples (tests / synthetic data).
    [[nodiscard]] static std::vector<ConventionScore> scoreAll(const std::vector<ProbeSample>& samples);

    /// Score a single convention on explicit samples.
    [[nodiscard]] static ConventionScore score(const std::vector<ProbeSample>& samples,
                                               const AttitudeConvention& convention) noexcept;

    /// The candidate with the smallest mean gravity angle (ties: lowest std).
    /// A default score (180 deg, 0 frames) when the track yields nothing.
    [[nodiscard]] static ConventionScore best(const meta::MetadataTrack& track, std::size_t maxFrames = 256);

    /// Pick the best entry of an existing score list.
    [[nodiscard]] static ConventionScore best(const std::vector<ConventionScore>& scores) noexcept;
};

}  // namespace osv::geom
