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
//               the moment it was measured (the batch's own stamp plus i
//               sample periods), on a time axis where each frame's stamp
//               reads the attitude of the middle of ITS picture - see
//               "When a frame was really taken" below.  A clip that records
//               no sensor readout time, or a batch whose stamp does not fit
//               its frame, keeps the older rule: sample i at
//               frameTs_k + (i - batchAnchorIndex) * 1e6 / imuSamplingRate
//               (batchAnchorIndex = 4: camera_attitude == batch[4] on the
//               6K sample clip).  extraOffsetUs is added either way.
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

// =============================================================================
//  When a frame was really taken
// =============================================================================
//
// A frame's metadata timestamp is the moment the sensor STARTS reading it
// out.  The sensor is a rolling shutter: row y leaves it y * (line time)
// later, and each row's light was gathered over the exposure before that.
// So the middle of the picture was taken
//
//     frameTs + readout / 2 - exposure / 2
//
// after the stamp.  Measured against the pictures themselves (the gyro time
// shift that best explains the far field's frame-to-frame motion, patch by
// patch down the lens image):
//
//     clip                     readout      exposure   centre row, measured / rule
//     8K 50, daylight          16.2 ms      1/2567 s   +8.08 ms / +7.92 ms
//     8K 50, night             16.2 ms      1/100 s    +3.27 ms / +3.11 ms
//     6K 59.94, airborne       13.2 ms      1/208 s    +4.33 ms / +4.18 ms
//
// and the row-to-row slope is the clip's own line time (ClipMeta::lro, in
// nanoseconds per sensor line) to within 4 % on all three.  The 0.15 ms the
// rule sits early by is under a sixth of one IMU sample.
//
// Why it matters: a stabiliser that counter-rotates by the attitude of the
// WRONG moment leaves the difference in the picture, and for anything that
// changes within a few milliseconds - a mount's vibration, a camera swinging
// under a drone - it adds a second copy of the motion instead of removing
// the first.

/// @brief How long the sensor takes to read one frame's rows out.
///
/// ClipMeta::lro is the line time in nanoseconds and ClipMeta::sensorH the
/// number of sensor lines, so the product is the time between the first and
/// the last row of one picture.
///
/// @param clip  The clip's metadata.
/// @return Microseconds; 0 when the clip does not record it or the product
///         is not a believable readout (longer than 50 ms).
[[nodiscard]] double sensorReadoutUs(const meta::ClipMeta& clip) noexcept;

/// @brief When the middle row of a frame was taken, relative to its timestamp.
///
/// readout / 2 - exposure / 2 (see the block above).  The exposure is the
/// frame's own (CameraFrame::exposureTime); a frame without a usable one is
/// taken as an instant.
///
/// @param clip    The clip's metadata (line time, sensor lines).
/// @param camera  The frame's camera metadata (exposure time).
/// @return Microseconds after the frame timestamp; 0 when the clip records
///         no readout time (nothing is known, nothing is shifted).
[[nodiscard]] double captureCentreOffsetUs(const meta::ClipMeta& clip, const meta::CameraFrame& camera) noexcept;

/// @brief The vibration cutoff matched to how long one frame takes to capture.
///
/// One rotation per frame can only be right for one moment, and the rows of
/// a frame are spread over the readout.  The single rotation that is best
/// for all of them is the attitude AVERAGED over that span: rotation slower
/// than the readout is the same in every row and is removed, rotation that
/// swings within it is different in every row and is better left alone.  A
/// Gaussian of the readout window's own variance (sigma = readout / sqrt 12)
/// is that average without the side lobes of a box, and its -3 dB point is
/// 0.459 / readout: 28 Hz for the 8K mode's 16.2 ms, 35 Hz for 6K's 13.2 ms.
///
/// Measured far-field jerk left in the picture (px rms at the lens image's
/// scale, no stabilisation / the old 12 Hz at the old moment / this):
/// 1.47 / 1.38 / 1.04 by day at 8K, 2.17 / 1.58 / 0.82 at night, 1.45 / 1.23
/// / 0.78 on the airborne 6K sample - whose 89 Hz engine line still passes
/// at only 10 %, so its nacelle does not shake against the world.
///
/// @param clip        The clip's metadata.
/// @param fallbackHz  The cutoff for a clip that records no readout time.
/// @return Hz, never below `fallbackHz` and never above 60.
[[nodiscard]] double captureMatchedCutoffHz(const meta::ClipMeta& clip, double fallbackHz) noexcept;

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
        /// Vibration cutoff for a track that feeds the stabilisation, in Hz
        /// (0 = none).  With it, and with the per-frame IMU batches present,
        /// the track is built from EVERY batch sample (dense, whatever
        /// `dense` says) and low-passed in time (a Gaussian, -3 dB at this
        /// frequency; see lowPass()), so rotation faster than a shake a
        /// viewer could see - a mount's vibration, which everything mounted
        /// with the camera shares - stays out of the track and out of the
        /// counter-rotation the stabilisation applies.  One attitude sample
        /// per frame cannot do that: a 90 Hz vibration sampled at 60 Hz
        /// reads as a sign flip every frame, and the levelling shakes the
        /// whole picture by it.  Without batches (or their rate) the sparse
        /// per-frame samples are kept as they are, and vibrationCutoffHz()
        /// reports 0.
        double vibrationCutoffHz = 0.0;
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

    /// One orientation per video frame of `track`, at each frame's own
    /// timestamp - what the per-frame smoothing (Smoother) and the mount
    /// measurement (levellingMount) read, whether this track holds one
    /// sample per frame or every IMU sample.  A frame without metadata
    /// repeats the previous frame's orientation (identity for the first).
    [[nodiscard]] std::vector<Quatd> perFrame(const meta::MetadataTrack& track) const;

    /// Low-pass `samples` in time: each orientation becomes the Gaussian
    /// log-map average of its neighbours within +-3 sigma, with sigma chosen
    /// so the response is -3 dB at `cutoffHz` (sigma = 0.1325 / cutoffHz
    /// seconds: exp(-2 pi^2 f^2 sigma^2) = 1 / sqrt 2).  Irregular spacing is
    /// fine: the weights are by time, not by index.  Samples are tidied
    /// (sorted, duplicates and non-finite entries dropped) first.  A cutoff
    /// that is not finite or not positive returns the tidied samples as they
    /// are.
    [[nodiscard]] static std::vector<Sample> lowPass(std::vector<Sample> samples, double cutoffHz);

    /// True when the track holds every IMU batch sample (dense placement),
    /// false for one sample per frame.
    [[nodiscard]] bool dense() const noexcept { return m_dense; }

    /// The vibration cutoff the samples were low-passed with (Hz), 0 when
    /// they were not (no cutoff asked for, or no IMU batches to apply it to).
    [[nodiscard]] double vibrationCutoffHz() const noexcept { return m_vibrationCutoffHz; }

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
    bool m_dense = false;               ///< Every batch sample, not one per frame.
    double m_vibrationCutoffHz = 0.0;   ///< The low-pass applied (0 = none).
};

}  // namespace osv::geom
