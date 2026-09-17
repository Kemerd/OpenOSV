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
#pragma once

#include "osv/core/Math.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace osv::geom {

/// Stabilisation behaviour.
enum class StabilizationMode { Off, HorizonLock, Full, Smooth };

/// Stable name for logs / JSON.
[[nodiscard]] const char* stabilizationModeName(StabilizationMode mode) noexcept;

/// Parameters shared by every mode.
struct StabilizationParams {
    StabilizationMode mode = StabilizationMode::Off;
    bool lockYaw = false;            ///< HorizonLock: also cancel yaw (keeps the reference heading).
    bool lockPitch = true;           ///< HorizonLock: cancel pitch.
    bool lockRoll = true;            ///< HorizonLock: cancel roll.
    double smoothSigmaFrames = 15.0; ///< Smooth: Gaussian sigma of the window (frames).
};

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
/// for Smooth mode (falls back to `worldFromBody`, i.e. no correction, when
/// absent).  Degenerate inputs yield identity.
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
