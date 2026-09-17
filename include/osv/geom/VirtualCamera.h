// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// VirtualCamera: the reframing camera the user points into the sphere.
//
// View frame: X right, Y forward (into the scene), Z up - the same axes as
// the body frame so an identity rotation looks along the master lens.
// rotation() = Rz(yaw) * Rx(pitch) * Ry(roll + correctionAngle) maps view
// rays into the (stabilised) body frame.  Right-handed angles: positive yaw
// turns the view towards -X (to the left when seen from above), positive
// pitch looks up, positive roll rotates the image counter-clockwise about
// the forward axis.  Pixel coordinates are pixel indices; pixelToRay() adds
// the 0.5 pixel-centre offset itself.
#pragma once

#include "osv/core/Math.h"
#include "osv/geom/EquirectMap.h"

namespace osv::geom {

/// Lens model of the virtual camera.
enum class Projection {
    Rectilinear,    ///< Pinhole: r = f * tan(theta).
    Fisheye,        ///< Equidistant: r = f * theta.
    Stereographic,  ///< r = 2 f tan(theta / 2) (Crystal Ball / Asteroid looks).
    Equirect,       ///< Full equirectangular panorama (Standard layout).
    EyeOffset       ///< r = f (1 + d) sin(theta) / (d + cos(theta)); d = 0 is Rectilinear, d = 1 Stereographic.
};

/// Stable name for logs / JSON.
[[nodiscard]] const char* projectionName(Projection projection) noexcept;

/// Largest horizontal field of view (deg) the eye-offset model can be
/// inverted for at offset `d`: 2 acos(-d) minus a one degree guard band
/// (179 deg at d = 0, 359 deg at d = 1).  Non-finite or out-of-range `d`
/// is clamped to [0, 1] first.
[[nodiscard]] double eyeOffsetMaxHfovDeg(double d) noexcept;

/// The reframing camera.
struct VirtualCamera {
    Projection projection = Projection::Rectilinear;
    int w = 1920;                     ///< Output width (px).
    int h = 1080;                     ///< Output height (px).
    double hfovDeg = 120.0;           ///< Horizontal field of view (deg).
    double yawDeg = 0.0;              ///< Rotation about +Z (deg).
    double pitchDeg = 0.0;            ///< Rotation about +X (deg), positive looks up.
    double rollDeg = 0.0;             ///< Rotation about +Y (deg).
    double correctionAngleDeg = 0.0;  ///< Extra horizon roll offset composed with rollDeg (deg).
    double eyeOffset = 0.0;           ///< Eye offset d in [0, 1] (Projection::EyeOffset only).

    /// Rotation mapping view rays into the body frame:
    /// Rz(yaw) * Rx(pitch) * Ry(roll + correctionAngle).
    [[nodiscard]] Mat3d rotation() const noexcept;

    /// Field of view actually used by focalPx() / pixelToRay(): hfovDeg,
    /// except that the eye-offset projection clamps it below
    /// eyeOffsetMaxHfovDeg(eyeOffset) so the model stays invertible.
    [[nodiscard]] double effectiveHfovDeg() const noexcept;

    /// Focal length in pixels for the current projection and hfov:
    ///   Rectilinear   : (W/2) / tan(hfov/2)
    ///   Fisheye       : (W/2) / (hfov/2 in rad)
    ///   Stereographic : (W/2) / (2 tan(hfov/4))
    ///   EyeOffset     : (W/2) (d + cos(hfov/2)) / ((1 + d) sin(hfov/2))
    ///   Equirect      : W / (2 pi)
    /// (hfov = effectiveHfovDeg()).  Returns 0 for a degenerate camera.
    [[nodiscard]] double focalPx() const noexcept;

    /// Map the pixel (px, py) (pixel index; 0.5 is added to hit the centre)
    /// to a unit ray in the view frame.  False when the pixel has no ray
    /// (outside the image circle of a fisheye, degenerate camera).
    [[nodiscard]] bool pixelToRay(double px, double py, Vec3d& dirView) const noexcept;

    /// True when the size and field of view are usable for the projection.
    [[nodiscard]] bool isValid() const noexcept;
};

}  // namespace osv::geom
