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

// ===========================================================================
//  DJI's reframe camera
//
//  DJI Studio and DJI's own Premiere reframe plug-in both render a panorama
//  the same way: the equirect is textured onto a UNIT SPHERE and an ordinary
//  perspective camera looks at it from a point on the view axis BEHIND the
//  centre.  The user controls map onto that camera directly:
//
//    FOV               the camera's VERTICAL pinhole field of view (deg);
//    Correction Angle  the eye's distance behind the centre, in sphere radii
//                      (0 = rectilinear, 1 = stereographic, > 1 = the eye is
//                      outside the sphere: DJI's "Crystal Ball");
//    Zoom              NOT an input: DJI Studio derives it from the other two
//                      and the canvas shape - it is the visible horizontal
//                      angle across the centre row of the frame.
//
//  Determined for interoperability with DJI Studio and DJI's Premiere
//  plug-in; docs/research/DJI_CAMERA.md describes the model and checks it
//  against DJI Studio's own read-outs.  The plug-in's per-pixel twin is
//  osvDjiSphereRay() in osv_kernel.h.
// ===========================================================================

/// Largest eye distance (sphere radii) the helpers below accept.  DJI's
/// Premiere plug-in stops its Correction Angle slider at 1.8; this is a
/// defensive ceiling far above that, not a feature limit.
inline constexpr double kDjiMaxEyeDistance = 16.0;

/// DJI's "Zoom" read-out: the visible horizontal angle (deg) across the
/// centre row of a frame of shape `aspect` (width / height), for a camera
/// with vertical pinhole field of view `vfovDeg` and eye distance
/// `eyeDistance`.
///
/// This is the formula behind DJI Studio's Zoom read-out,
/// including its guards, so the number matches what DJI Studio prints:
///
///     a    = tan(min(vfov, 180) / 2) * aspect        (tan of the pinhole's
///                                                     horizontal half angle)
///     zoom = 360 - 2 atan(1 / a) - 2 acos(clamp(d a / sqrt(1 + a^2), -1, 1))
///
/// which is 2 (alpha + asin(d sin alpha)) with alpha = atan(a): twice the
/// angle, seen from the sphere's centre, of the point where the edge ray of
/// the centre row leaves the sphere.  DJI's clamp keeps it defined for
/// d > 1, where the physical visible angle is limited by the tangent ray
/// instead.  Returns 0 - DJI's answer - for a non-positive or non-finite
/// aspect or field of view, a negative eye distance, or a degenerate tangent.
[[nodiscard]] double djiZoomDeg(double vfovDeg, double eyeDistance, double aspect) noexcept;

/// The pinhole half angle (rad) whose ray reaches the sphere point
/// `visibleHalfRad` away from the view axis, for eye distance `eyeDistance`
/// (0 <= d <= 1): alpha = atan2(sin theta, d + cos theta).  This is the
/// inverse of theta = alpha + asin(d sin alpha).  Returns a negative number
/// when the point is not visible from that eye (theta at or past acos(-d)) or
/// an input is not finite.
[[nodiscard]] double djiPinholeHalfAngleRad(double visibleHalfRad, double eyeDistance) noexcept;

/// DJI's camera as a stand-alone object: size, the two controls and the
/// pixel -> ray map in double precision (the reference the float kernel is
/// tested against).
struct DjiSphereCamera {
    int w = 1920;                ///< Output width (px).
    int h = 1080;                ///< Output height (px).
    double vfovDeg = 60.0;       ///< DJI "FOV": vertical pinhole field of view (deg).
    double eyeDistance = 0.6;    ///< DJI "Correction Angle": eye distance behind the centre (radii).

    /// True when the size is positive, 0 < vfov < 180 and
    /// 0 <= eyeDistance <= kDjiMaxEyeDistance, all finite.
    [[nodiscard]] bool isValid() const noexcept;

    /// Pinhole focal length in pixels, (H / 2) / tan(vfov / 2); 0 when invalid.
    [[nodiscard]] double focalPx() const noexcept;

    /// DJI's Zoom for this camera's own shape (see djiZoomDeg()).
    [[nodiscard]] double zoomDeg() const noexcept;

    /// Unit view-frame ray of pixel (px, py) (pixel index; 0.5 is added to hit
    /// the centre): the far intersection of the pinhole ray from
    /// (0, -eyeDistance, 0) with the unit sphere.  False when the camera is
    /// invalid, an input is not finite, or the ray misses the sphere (only
    /// possible for eyeDistance > 1).
    [[nodiscard]] bool pixelToRay(double px, double py, Vec3d& dirView) const noexcept;
};

}  // namespace osv::geom
