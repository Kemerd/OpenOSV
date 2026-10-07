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
#include <string>
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

/// The reading `auto` resolves to, with the evidence behind it.
///
/// What the Osmo 360 records, measured on two car-mounted clips (a vehicle
/// turns about the true vertical, which gives an independent check):
///
///   * camera_acc is the specific force in a WORLD frame, not the body
///     frame: it stays within a few degrees of one direction for ten
///     minutes of driving while the body turns through every heading.
///   * That frame is the world frame of the stored quaternion read in
///     w,x,y,z order as world->body.  The verified reading (XYZW,
///     body->world) describes the same motion with its world frame turned a
///     quarter turn about Y, so a vector a in the accelerometer's frame is
///     (a.z, a.y, -a.x) in ours.
///   * The quaternion's world frame is NOT level: the turn axis of the car
///     sits 11 and 30 deg away from -Y on the two clips, and the mapped
///     accelerometer lands within 2 deg of it both times.  Levelling on -Y
///     alone leaves those clips' horizons tilted by that much.
///
/// So the accelerometer measures the true up directly.  Scoring it against
/// the body-frame gravity of each candidate (score / best) proves nothing,
/// and on a clean 1 g clip it latches onto a reading that rolls the horizon
/// a quarter turn.
struct AutoConvention {
    AttitudeConvention conv;          ///< The reading to build the attitude track with.
    Vec3d measuredUp{0.0, 0.0, 0.0};  ///< True up in conv's world frame; zero = not measured.
    bool upFromAccelerometer = false; ///< True when measuredUp came from camera_acc.
    double meanAccG = 0.0;            ///< |mean camera_acc| over the probed frames (g).
    double spreadDeg = 180.0;         ///< Mean angle between each frame's acc and the mean (deg).
    double tiltDeg = 0.0;             ///< Angle between the measured up and conv's axis (deg).
    std::size_t framesUsed = 0;       ///< Frames that carried a finite, non-zero acc.
    std::string reason;               ///< One line for logs.

    /// Copy the reading and the measured up into AttitudeTrack build options.
    void applyTo(AttitudeTrack::Options& options) const noexcept {
        options.conv = conv;
        options.measuredUp = measuredUp;
    }
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

    /// The reading every `auto` caller uses (importer horizon lock, osvtool).
    ///
    /// Component order and rotation sense stay at the verified default
    /// (XYZW, body->world: horizon lock checked by eye on the airborne sample,
    /// and the only reading under which a car-mounted camera turns about the
    /// body axis its fisheyes show to be vertical).  The true up is measured
    /// from the accelerometer when it carries a clean gravity reaction (mean
    /// 0.6..1.4 g, frames within 35 deg of the mean, result within 60 deg of
    /// -Y); otherwise it stays -Y.  Frames are sampled evenly over the whole
    /// clip, up to `maxFrames`.
    [[nodiscard]] static AutoConvention autoDetect(const meta::MetadataTrack& track, std::size_t maxFrames = 256);

    /// The same rule on explicit samples (tests / synthetic data).
    [[nodiscard]] static AutoConvention autoDetect(const std::vector<ProbeSample>& samples);
};

}  // namespace osv::geom
