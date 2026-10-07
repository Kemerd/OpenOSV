// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ConventionProbe: score every plain reading of the attitude quaternion
// against the accelerometer, and the reading every `auto` caller uses.
//
// For a static or slowly moving camera camera_acc points along gravity in
// the body frame, so a convention can be scored by how well
// R_body_from_world * up_world lines up with the normalised acceleration
// frame after frame.  All 2 x 2 x 4 = 16 plain combinations of component
// order, rotation sense and world-up axis are scored (scoreAll / best, a
// research tool).  autoDetect does not score: it returns the verified
// reading and keeps the accelerometer only as a canary (see AutoConvention).
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

/// The canary's alarm threshold (deg): the accelerometer's gravity and the
/// reading's up have agreed within 1 deg on every car clip measured so far
/// (0.2-0.8 deg over 256 frames of two 8K drives, OSV and LRF), so 15 deg
/// means the reading no longer describes the camera (another body, another
/// firmware), not noise.
inline constexpr double kAttitudeCanaryWarnDeg = 15.0;

/// The reading `auto` resolves to, with the accelerometer canary beside it.
///
/// What the Osmo 360 records, measured against image truth on two car clips
/// (lamp poles, the sunset sun through a 106 deg turn) and on the airborne
/// sample (its horizon, checked by eye):
///
///   * the stored attitude is a world -> body rotation in the IMU's own
///     axes; relabelled into the rig's axes (AttitudeConvention::rigAxes)
///     its world is level, +Z up.  No accelerometer is needed to level it.
///   * camera_acc is the specific force in the BODY frame, in yet another
///     axis order: (a_y, a_x, -a_z) in rig axes.  On a car or a tripod its
///     clip mean is the gravity reaction, i.e. the body's up.
///
/// 0.5.0 read the attitude transposed (XYZW, body -> world, -Y up) and took
/// camera_acc for a world-frame vector to move that reading's up towards
/// gravity.  That repaired the world-side half of the error only; the car
/// clips stayed 28 and 8 deg off.  The accelerometer now only checks the
/// reading: the angle between its mean gravity and the reading's mean body
/// up is logged and, when it exceeds kAttitudeCanaryWarnDeg on a clean
/// gravity reaction, flagged.  The levelling never depends on it.
struct AutoConvention {
    AttitudeConvention conv;          ///< The reading to build the attitude track with.
    /// Always zero: the reading's world is level by construction.  Kept so
    /// applyTo() also clears an up a reused Options may still carry.
    Vec3d measuredUp{0.0, 0.0, 0.0};
    double meanAccG = 0.0;            ///< |mean camera_acc| over the probed frames (g).
    double spreadDeg = 180.0;         ///< Mean angle between each frame's acc and the mean (deg).
    std::size_t framesUsed = 0;       ///< Frames that carried an attitude and a finite, non-zero acc.
    /// True when the canary was computed (enough frames, a non-zero mean).
    bool canaryMeasured = false;
    /// True when the accelerometer is a clean gravity reaction (mean
    /// 0.6..1.4 g, frames within 35 deg of the mean), so the canary means
    /// something.  An airborne or aerobatic clip measures motion, not
    /// gravity, and is reported without a verdict.
    bool canaryJudged = false;
    /// Angle (deg) between the clip-mean accelerometer gravity in rig axes
    /// and the clip-mean body up of the reading, R(t)^T * up.
    double canaryDeg = 0.0;
    bool canaryWarning = false;       ///< canaryJudged and canaryDeg > kAttitudeCanaryWarnDeg.
    std::string reason;               ///< One line for logs (7-bit ASCII).

    /// Copy the reading into AttitudeTrack build options (and clear any
    /// measured up: the reading's own axis is the up).
    void applyTo(AttitudeTrack::Options& options) const noexcept {
        options.conv = conv;
        options.measuredUp = measuredUp;
    }
};

/// Scores the sixteen plain quaternion conventions; resolves `auto`.
class ConventionProbe {
public:
    /// The sixteen plain candidate conventions (rigAxes = false) in a fixed
    /// order: order-major, then sense, then up axis.
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

    /// The reading every `auto` caller uses (importer horizon lock, the OFX
    /// generator and its Playback Proxy, Scene Light, osvtool).
    ///
    /// Always the default AttitudeConvention with no measured up: the
    /// levelling depends on the attitude alone.  The accelerometer canary
    /// (AutoConvention) is computed on frames sampled evenly over the whole
    /// clip, up to `maxFrames`, and reported in `reason`; above
    /// kAttitudeCanaryWarnDeg on a clean gravity reaction it is also logged
    /// as a warning.
    [[nodiscard]] static AutoConvention autoDetect(const meta::MetadataTrack& track, std::size_t maxFrames = 256);

    /// The same rule on explicit samples (tests / synthetic data).  Samples
    /// without an attitude, or with a zero / non-finite acc, are ignored.
    [[nodiscard]] static AutoConvention autoDetect(const std::vector<ProbeSample>& samples);
};

}  // namespace osv::geom
