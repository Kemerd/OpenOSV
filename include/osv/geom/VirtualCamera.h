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
    Equirect        ///< Full equirectangular panorama (Standard layout).
};

/// Stable name for logs / JSON.
[[nodiscard]] const char* projectionName(Projection projection) noexcept;

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

    /// Rotation mapping view rays into the body frame:
    /// Rz(yaw) * Rx(pitch) * Ry(roll + correctionAngle).
    [[nodiscard]] Mat3d rotation() const noexcept;

    /// Focal length in pixels for the current projection and hfov:
    ///   Rectilinear   : (W/2) / tan(hfov/2)
    ///   Fisheye       : (W/2) / (hfov/2 in rad)
    ///   Stereographic : (W/2) / (2 tan(hfov/4))
    ///   Equirect      : W / (2 pi)
    /// Returns 0 for a degenerate camera.
    [[nodiscard]] double focalPx() const noexcept;

    /// Map the pixel (px, py) (pixel index; 0.5 is added to hit the centre)
    /// to a unit ray in the view frame.  False when the pixel has no ray
    /// (outside the image circle of a fisheye, degenerate camera).
    [[nodiscard]] bool pixelToRay(double px, double py, Vec3d& dirView) const noexcept;

    /// True when the size and field of view are usable for the projection.
    [[nodiscard]] bool isValid() const noexcept;
};

}  // namespace osv::geom
