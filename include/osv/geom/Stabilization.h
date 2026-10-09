// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Stabilization: turn the measured body orientation into a correction the
// renderer applies between the virtual camera and the lens rig.
//
// Frames involved (all rotations are body-from-X, applied to column vectors):
//   * body  : the camera body at the current frame (lens rig frame).
//   * world : the IMU's inertial frame (up axis per AttitudeConvention).
//   * view  : the frame the virtual camera rays are expressed in.
//
// The renderer computes  d_body = C * R_cam * d_pixel  where R_cam is
// VirtualCamera::rotation() and C is the matrix returned by
// stabilizationBodyFromWorld().  Per mode:
//   Off         : C = I (the view is glued to the body).
//   Full        : the view is glued to the reference body pose:
//                 C = R_wb^T * R_ref  (R_wb = worldFromBody now, R_ref = worldFromBody at the reference).
//   HorizonLock : the view keeps the body's yaw about world-up but pitch and
//                 roll are cancelled (each axis individually switchable).
//   Smooth      : the view follows a Gaussian-smoothed orientation:
//                 C = R_wb^T * R_smooth.
//   SmoothLevel : Smooth and HorizonLock together, the way DJI Studio runs
//                 RockSteady and Horizon Leveling at the same time.  The view
//                 keeps only the level part of the smoothed orientation - its
//                 heading about world-up - so the shake is gone AND the
//                 horizon is level:
//                 C = R_wb^T * R_level(R_smooth),
//                 where R_level is exactly the HorizonLock levelling, applied
//                 to R_smooth instead of R_wb.  With smoothing disabled (or an
//                 orientation that is already smooth) it is HorizonLock.
#pragma once

#include "osv/core/Math.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace osv::geom {

/// Stabilisation behaviour.
///
/// The values are listed in the order they were introduced; SmoothLevel is
/// the newest.  Nothing persists this enum (the plug-ins store their own
/// PrefsStabilization byte and map it), but the order is kept append-only
/// anyway so logs from different builds read the same.
enum class StabilizationMode {
    Off,          ///< No correction: the view is glued to the body.
    HorizonLock,  ///< Heading follows the body, pitch and roll are levelled.
    Full,         ///< The view is locked to the reference (first) pose.
    Smooth,       ///< The view follows the Gaussian-smoothed orientation.
    SmoothLevel   ///< Smooth heading, levelled pitch and roll (Smooth + HorizonLock).
};

/// Stable name for logs / JSON.
[[nodiscard]] const char* stabilizationModeName(StabilizationMode mode) noexcept;

/// True for the modes whose correction reads the smoothed orientation
/// sequence (Smooth and SmoothLevel).  Every caller that builds the
/// Smoother output asks this one question, so a mode that needs the
/// smoothed attitude can never be left without it.
[[nodiscard]] constexpr bool stabilizationUsesSmoothing(StabilizationMode mode) noexcept {
    return mode == StabilizationMode::Smooth || mode == StabilizationMode::SmoothLevel;
}

/// The vibration cutoff every stabilisation mode builds its attitude track
/// with (AttitudeTrack::Options::vibrationCutoffHz), in Hz.
///
/// Stabilisation counter-rotates the view by the camera's measured
/// rotation.  Rotation faster than this is VIBRATION, not a shake a viewer
/// could see: on the airborne sample clip the camera buzzes with the engine
/// at 22 / 44 / 89 / 134 Hz, 0.02-0.05 deg each, measured on the 1 kHz IMU
/// batches.  One attitude sample per frame reads the 89 Hz line as a
/// +-0.2 deg sign flip every frame (it aliases to 29.5 Hz at 59.94 fps), and
/// counter-rotating that held the world still while the engine nacelle -
/// bolted to the wing the camera is bolted to, so it shares every bit of the
/// vibration - shook by a pixel against it.  Everything mounted with the
/// camera shares its vibration; what a viewer wants steady is the mount.
/// Below this cutoff the sample clip's rotation is under 1 deg/s rms: the
/// real motion, which is what the modes remove.  Hand shake (under ~10 Hz)
/// and vehicle motion are well inside it; a per-frame track (no IMU
/// batches) is left as it is.
inline constexpr double kStabilisationVibrationCutoffHz = 12.0;

/// Parameters shared by every mode.
struct StabilizationParams {
    StabilizationMode mode = StabilizationMode::Off;
    bool lockYaw = false;            ///< HorizonLock / SmoothLevel: also cancel yaw (keeps the reference heading).
    bool lockPitch = true;           ///< HorizonLock / SmoothLevel: cancel pitch.
    bool lockRoll = true;            ///< HorizonLock / SmoothLevel: cancel roll.
    double smoothSigmaFrames = 15.0; ///< Smooth / SmoothLevel: Gaussian sigma of the window (frames).
    /// HorizonLock / SmoothLevel: how the lens rig is mounted, body <- levelling
    /// frame (see levellingMount).  Identity - the default - levels the body
    /// itself, heading from its +Y, which is every Osmo 360 clip.
    Mat3d mount;
};

/// The levelling frame of a clip: which body axes Horizon Leveling treats as
/// right, forward and up, measured over the clip's whole attitude track.
///
/// Levelling decomposes the pose as yaw * pitch * roll about the body's
/// +Y (forward), which is only well conditioned while +Y is closer to the
/// horizon than to vertical.  A camera held with its lenses level (the Osmo
/// 360 in the hand or on a stick) always is.  One flown with a lens up and a
/// lens down (the Avata 360's 360 mode) never is: +Y is within a few degrees
/// of vertical on every frame, the decomposition sits in gimbal lock, and a
/// 5 degree tilt forward or sideways swings the heading by up to 90 degrees.
///
/// Returns the identity when the mean |cos| between the body's +Y and up is
/// at most cos(45 deg) over the samples (and for no usable sample at all).
/// Otherwise it returns the quarter turn about the body's +X that makes the
/// body's horizontal +/-Z axis the levelling frame's forward and whichever of
/// +/-Y points up on average its up; as a matrix, its columns are those body
/// directions (right, forward, up).  It is a proper rotation either way.
[[nodiscard]] Mat3d levellingMount(const std::vector<Quatd>& worldFromBody, const Vec3d& worldUp) noexcept;

/// Quaternion logarithm: rotation vector (axis * angle) of a unit quaternion.
[[nodiscard]] Vec3d quatLog(const Quatd& q) noexcept;

/// Quaternion exponential: unit quaternion from a rotation vector.
[[nodiscard]] Quatd quatExp(const Vec3d& rotationVector) noexcept;

/// Yaw / pitch / roll (radians) such that R = Rz(yaw) * Rx(pitch) * Ry(roll),
/// the VirtualCamera composition order.  Near gimbal lock the roll is set
/// to zero and the yaw absorbs the remaining rotation.
struct EulerZXY {
    double yaw = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
};
[[nodiscard]] EulerZXY eulerZXY(const Mat3d& r) noexcept;
[[nodiscard]] Mat3d fromEulerZXY(const EulerZXY& e) noexcept;

/// Compute the correction matrix C described in the file header.
/// `worldFromBody` is the orientation of the current frame, `reference` the
/// orientation of the reference frame (usually the first), `worldUp` the
/// unit up axis of the world frame, and `smoothed` the smoothed orientation
/// for Smooth and SmoothLevel.  When it is absent (or not finite) both fall
/// back to `worldFromBody` as the smoothed pose: Smooth then applies no
/// correction and SmoothLevel still levels the horizon, exactly as
/// HorizonLock.  Degenerate inputs yield identity.
[[nodiscard]] Mat3d stabilizationBodyFromWorld(const Quatd& worldFromBody, const StabilizationParams& params,
                                               const Quatd& reference, const Vec3d& worldUp = Vec3d{0.0, 0.0, 1.0},
                                               const std::optional<Quatd>& smoothed = std::nullopt) noexcept;

/// Gaussian-window log-map average of an orientation sequence (Smooth mode).
/// For index k the mean is q_k * exp(sum_i w_i * log(q_k^-1 * q_i)) over a
/// window of +-3 sigma, clamped at the sequence ends.
class Smoother {
public:
    /// `sigmaFrames` <= 0 disables smoothing (output == input).
    explicit Smoother(double sigmaFrames) noexcept;

    /// Smooth the whole sequence.
    [[nodiscard]] std::vector<Quatd> smooth(const std::vector<Quatd>& input) const;

    /// Smoothed orientation at index `k` (identity when out of range).
    [[nodiscard]] Quatd smoothedAt(const std::vector<Quatd>& input, std::size_t k) const noexcept;

    /// The sigma this smoother was built with.
    [[nodiscard]] double sigmaFrames() const noexcept { return m_sigma; }

    /// Window half width in frames (3 sigma, at least 1 when enabled).
    [[nodiscard]] std::size_t halfWidth() const noexcept;

private:
    double m_sigma = 0.0;
};

}  // namespace osv::geom
