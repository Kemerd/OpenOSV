// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// AttitudeTrack: a time-ordered sequence of body orientations built from the
// djmd metadata, queried as worldFromBody(t).
//
// Two sources are supported:
//   * sparse  : one quaternion per video frame (FrameMeta.camera.attitude)
//               placed at the frame timestamp;
//   * dense   : every quaternion of the per-frame IMU batches
//               (FrameMeta.imu->current.q), sample i of frame k placed at
//               frameTs_k + (i - batchAnchorIndex) * 1e6 / imuSamplingRate + extraOffsetUs.
//               batchAnchorIndex = 4 because camera_attitude == batch[4] on
//               the sample clip, i.e. entry 4 is the exposure-time sample.
//
// The quaternion interpretation (component order, rotation sense, which
// world axis is up, and whether the IMU's axes are relabelled into the rig's)
// is switchable through AttitudeConvention.  The default is the reading that
// levels every clip measured so far against image truth (lamp poles, the
// sun's elevation through a 106 deg turn, the airborne sample's horizon):
// the stored floats f0..f3 give q' = (w = f3; x = -f1, y = f2, z = f0), a
// body -> world rotation into a world whose +Z is up (docs/GEOMETRY.md).
// Whatever the convention, the track stores and returns worldFromBody:
// d_world = q * d_body * q^-1.
#pragma once

#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/geom/Extrinsics.h"
#include "osv/meta/Types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace osv::meta {
class MetadataTrack;
}

namespace osv::geom {

/// Direction of the stored attitude rotation.
enum class AttitudeSense {
    WorldToBody,  ///< R(q) * d_world = d_body   (the Osmo 360, with rigAxes)
    BodyToWorld   ///< R(q) * d_body = d_world
};

/// Which world axis points up (against gravity) in the IMU's world frame.
enum class WorldUp { Y, Z, NegY, NegZ };

/// How the stored quaternion floats are turned into a rotation.
///
/// The default is the Osmo 360's reading: the floats read (x, y, z, w) are a
/// world -> body rotation in the IMU's own axes, which are relabelled into
/// the rig's axes (rigAxes), and the world's +Z is up.  Together that is
/// q' = (w = f3; x = -f1, y = f2, z = f0) body -> world.
///
/// The reading 0.4.x / 0.5.0 used (XYZW, body -> world, -Y up) is the
/// transpose of that rotation with its axes relabelled.  It was checked only
/// on the airborne sample, whose attitude barely changes (4.9 deg over the
/// clip): at that one pose the two readings agree within 0.1-0.5 deg, so the
/// sample could not tell them apart.  On car clips that turn it leaves the
/// horizon 7-28 deg off (lamp poles lean, the sunset sun sits at +35 deg).
/// It stays reachable for comparisons (osvtool --attitude-convention
/// xyzw-b2w-ny), as do the other plain readings.
struct AttitudeConvention {
    QuatOrder order = QuatOrder::XYZW;                ///< Component order of the stored floats.
    AttitudeSense sense = AttitudeSense::WorldToBody; ///< Direction of the stored rotation.
    WorldUp up = WorldUp::Z;                          ///< Up axis of the (relabelled) world frame.
    /// Relabel the IMU's axes into the rig's (body X right, Y forward, Z up)
    /// on both sides of the rotation: K * R * K^T with K: (x, y, z) ->
    /// (y, -z, -x), a proper rotation.  For a quaternion (w, v) that is
    /// (w, K v) = (w; v.y, -v.z, -v.x).  false = the plain reading, the
    /// IMU's axes taken as the rig's (every reading before 0.5.1).
    bool rigAxes = true;
};

/// Stable names for logs / JSON.
[[nodiscard]] const char* attitudeSenseName(AttitudeSense sense) noexcept;
[[nodiscard]] const char* worldUpName(WorldUp up) noexcept;
/// Short compound name such as "XYZW/WorldToBody/Y"; a reading with the
/// rig's axes carries a "/rig" suffix ("XYZW/WorldToBody/Z/rig").
[[nodiscard]] std::string attitudeConventionName(const AttitudeConvention& convention);

/// Unit vector of the world-up axis for a convention.
[[nodiscard]] Vec3d worldUpVector(WorldUp up) noexcept;

/// Interpret one stored quaternion as worldFromBody under `convention`.
/// Absent / degenerate input yields identity.
[[nodiscard]] Quatd worldFromBodyQuat(const meta::Quaternion& q, const AttitudeConvention& convention) noexcept;

/// Interpret one stored quaternion as the matrix R_body_from_world.
[[nodiscard]] Mat3d bodyFromWorldMatrix(const meta::Quaternion& q, const AttitudeConvention& convention) noexcept;

/// Time-ordered orientation samples with slerp interpolation.
class AttitudeTrack {
public:
    /// Build options.
    struct Options {
        AttitudeConvention conv;      ///< Quaternion interpretation.
        bool dense = false;           ///< Use every IMU batch sample instead of one per frame.
        int batchAnchorIndex = 4;     ///< Batch entry that coincides with the frame timestamp.
        double extraOffsetUs = 0.0;   ///< Additional time offset applied to dense samples (us).
        /// An externally measured world-up direction in the convention's
        /// world frame, overriding conv.up.  Zero length = conv.up, which is
        /// what `auto` passes: under the default reading the world frame is
        /// level by construction (its +Z is up).
        Vec3d measuredUp{0.0, 0.0, 0.0};
    };

    /// One orientation sample.
    struct Sample {
        double tUs = 0.0;             ///< Time in metadata microseconds.
        Quatd worldFromBody;          ///< Unit quaternion, body -> world.
    };

    /// Least-squares fit of the IMU attitude clock (ImuBatch.ts, in ticks)
    /// against the frame timestamps (us):  ts_ticks ~= ticksPerUs * tUs + offsetTicks.
    struct ClockFit {
        double ticksPerUs = 0.0;      ///< Slope (about 1.0004 on the sample clip).
        double offsetTicks = 0.0;     ///< Intercept.
        double rmsUs = 0.0;           ///< RMS residual expressed in microseconds.
        double ticksPerSample = 0.0;  ///< Mean ticks between consecutive IMU samples (~1006).
        std::size_t n = 0;            ///< Number of frames that contributed (0 = no fit).
    };

    AttitudeTrack() = default;

    /// Build from the metadata track.  Fails when the track has no frames,
    /// when no frame carries the needed attitude data, or (dense mode) when
    /// imu_sampling_rate is missing.
    [[nodiscard]] static Result<AttitudeTrack> build(const meta::MetadataTrack& track, const Options& options);

    /// Build from already interpreted samples (tests, synthetic data).  The
    /// samples are sorted by time; duplicates and non-finite entries are
    /// dropped.  Fails when nothing usable remains.
    [[nodiscard]] static Result<AttitudeTrack> fromSamples(std::vector<Sample> samples, const Options& options);

    /// Orientation at time `tUs`: slerp between the neighbouring samples,
    /// clamped to the first / last sample outside the covered range.
    /// Identity when the track is empty or the time is not finite.
    [[nodiscard]] Quatd worldFromBody(double tUs) const noexcept;

    /// Time of the first / last sample (0 when empty).
    [[nodiscard]] double beginUs() const noexcept;
    [[nodiscard]] double endUs() const noexcept;
    /// Number of samples.
    [[nodiscard]] std::size_t sampleCount() const noexcept { return m_samples.size(); }
    /// Read-only access to the samples (sorted by time).
    [[nodiscard]] const std::vector<Sample>& samples() const noexcept { return m_samples; }

    /// The clock fit computed at build time (n == 0 when no IMU batches).
    [[nodiscard]] const ClockFit& clockFit() const noexcept { return m_clockFit; }

    /// Unit world-up vector the track was built with: Options::measuredUp
    /// when one was measured, else the convention's axis.
    [[nodiscard]] Vec3d worldUp() const noexcept;

    /// The options used to build the track.
    [[nodiscard]] const Options& options() const noexcept { return m_options; }

    /// Compute the clock fit from (frame time us, batch ts ticks, batch sample
    /// count) triples.  Exposed for tests; returns n == 0 when fewer than two
    /// usable frames exist.
    struct ClockObservation {
        double frameTimeUs = 0.0;
        double batchTicks = 0.0;
        std::size_t batchSamples = 0;
    };
    [[nodiscard]] static ClockFit fitClock(const std::vector<ClockObservation>& observations) noexcept;

private:
    std::vector<Sample> m_samples;
    ClockFit m_clockFit;
    Options m_options;
};

}  // namespace osv::geom
