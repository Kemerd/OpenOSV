// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Virtual reframing camera: orientation and pixel -> ray for each projection.

#include "osv/geom/VirtualCamera.h"

#include <cmath>

namespace osv::geom {

const char* projectionName(Projection projection) noexcept {
    switch (projection) {
    case Projection::Rectilinear: return "Rectilinear";
    case Projection::Fisheye: return "Fisheye";
    case Projection::Stereographic: return "Stereographic";
    case Projection::Equirect: return "Equirect";
    case Projection::EyeOffset: return "EyeOffset";
    }
    return "Unknown";
}

double eyeOffsetMaxHfovDeg(double d) noexcept {
    // Guard against garbage: the model is only defined for d in [0, 1].
    if (!std::isfinite(d)) {
        d = 0.0;
    }
    d = clampd(d, 0.0, 1.0);
    // The forward model r(theta) has its asymptote at theta = acos(-d); one
    // degree below twice that angle keeps the inverse well conditioned.
    return rad2deg(2.0 * std::acos(-d)) - 1.0;
}

bool VirtualCamera::isValid() const noexcept {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (!std::isfinite(hfovDeg) || hfovDeg <= 0.0) {
        return false;
    }
    if (!std::isfinite(yawDeg) || !std::isfinite(pitchDeg) || !std::isfinite(rollDeg) ||
        !std::isfinite(correctionAngleDeg)) {
        return false;
    }
    // A pinhole cannot reach 180 degrees; the curved projections can go
    // further but must still be below their own singularities.  The
    // eye-offset model clamps its field of view instead (effectiveHfovDeg),
    // so it only needs a sane offset and a sane upper bound.
    switch (projection) {
    case Projection::Rectilinear: return hfovDeg < 180.0;
    case Projection::Fisheye: return hfovDeg <= 360.0;
    case Projection::Stereographic: return hfovDeg < 360.0;
    case Projection::Equirect: return true;
    case Projection::EyeOffset:
        return std::isfinite(eyeOffset) && eyeOffset >= 0.0 && eyeOffset <= 1.0 && hfovDeg <= 360.0;
    }
    return false;
}

double VirtualCamera::effectiveHfovDeg() const noexcept {
    if (projection != Projection::EyeOffset) {
        return hfovDeg;
    }
    // Keep the requested angle unless it would reach the model's asymptote.
    const double maxDeg = eyeOffsetMaxHfovDeg(eyeOffset);
    return hfovDeg < maxDeg ? hfovDeg : maxDeg;
}

Mat3d VirtualCamera::rotation() const noexcept {
    // Yaw about up, then pitch about right, then roll (plus the horizon
    // correction) about forward - see the header for the sign conventions.
    return Mat3d::rotZ(deg2rad(yawDeg)) * Mat3d::rotX(deg2rad(pitchDeg)) *
           Mat3d::rotY(deg2rad(rollDeg + correctionAngleDeg));
}

double VirtualCamera::focalPx() const noexcept {
    if (!isValid()) {
        return 0.0;
    }
    const double halfW = 0.5 * static_cast<double>(w);
    const double halfFov = deg2rad(0.5 * effectiveHfovDeg());
    switch (projection) {
    case Projection::Rectilinear: {
        const double t = std::tan(halfFov);
        return (t > 0.0) ? halfW / t : 0.0;
    }
    case Projection::Fisheye: return halfW / halfFov;
    case Projection::Stereographic: {
        const double t = 2.0 * std::tan(0.5 * halfFov);
        return (t > 0.0) ? halfW / t : 0.0;
    }
    case Projection::EyeOffset: {
        // r/f at the half field of view: (1 + d) sin(h) / (d + cos(h)).  The
        // clamp in effectiveHfovDeg() keeps the denominator positive.
        const double denom = eyeOffset + std::cos(halfFov);
        if (!(denom > 0.0)) {
            return 0.0;
        }
        const double t = (1.0 + eyeOffset) * std::sin(halfFov) / denom;
        return (t > 0.0 && std::isfinite(t)) ? halfW / t : 0.0;
    }
    case Projection::Equirect: return static_cast<double>(w) / kTwoPi;
    }
    return 0.0;
}

bool VirtualCamera::pixelToRay(double px, double py, Vec3d& dirView) const noexcept {
    if (!isValid() || !std::isfinite(px) || !std::isfinite(py)) {
        return false;
    }
    const double wd = static_cast<double>(w);
    const double hd = static_cast<double>(h);

    // The panorama delegates to the equirect map (pixel centre offset added).
    if (projection == Projection::Equirect) {
        EquirectMap map;
        map.layout = EquirectLayout::Standard;
        map.w = w;
        map.h = h;
        return map.pixelToDir(px + 0.5, py + 0.5, dirView);
    }

    if (projection == Projection::Rectilinear) {
        // Normalised image plane at unit distance: u right, v up.
        const double t = std::tan(deg2rad(0.5 * hfovDeg));
        const double u = (2.0 * (px + 0.5) / wd - 1.0) * t;
        const double v = (1.0 - 2.0 * (py + 0.5) / hd) * t * hd / wd;
        const Vec3d d = Vec3d{u, 1.0, v}.normalized();
        if (!d.isFinite() || !(d.norm() > 0.0)) {
            return false;
        }
        dirView = d;
        return true;
    }

    // Radial projections: distance from the image centre in pixels.
    const double f = focalPx();
    if (!(f > 0.0)) {
        return false;
    }
    const double sx = (px + 0.5) - 0.5 * wd;          // right positive
    const double sy = 0.5 * hd - (py + 0.5);          // up positive
    const double r = std::hypot(sx, sy);
    double theta = 0.0;
    if (projection == Projection::Fisheye) {
        // Equidistant: r = f * theta.
        theta = r / f;
    } else if (projection == Projection::EyeOffset) {
        // Eye offset: r = f (1 + d) sin(theta) / (d + cos(theta)).  With
        // k = r / (f (1 + d)) the inverse is
        //   theta = atan(k) + asin(k d / sqrt(1 + k^2)),
        // valid below the asymptote at acos(-d).
        const double d = eyeOffset;
        const double k = r / (f * (1.0 + d));
        // Same overflow-safe form as the kernel: k^2 -> inf must give the
        // limit d, not 0.
        const double s = (k > 1.0) ? clampd(d / std::sqrt(1.0 + 1.0 / (k * k)), -1.0, 1.0)
                                   : clampd(k * d / std::sqrt(1.0 + k * k), -1.0, 1.0);
        theta = std::atan(k) + std::asin(s);
        if (!std::isfinite(theta) || theta >= std::acos(-d)) {
            return false;
        }
    } else {
        // Stereographic: r = 2 f tan(theta / 2).
        theta = 2.0 * std::atan(r / (2.0 * f));
    }
    // Nothing behind the antipode.
    if (!std::isfinite(theta) || theta > kPi) {
        return false;
    }
    // On the axis the azimuth is undefined: straight ahead.
    if (r <= 0.0) {
        dirView = Vec3d{0.0, 1.0, 0.0};
        return true;
    }
    const double s = std::sin(theta);
    dirView = Vec3d{s * (sx / r), std::cos(theta), s * (sy / r)};
    return dirView.isFinite();
}

}  // namespace osv::geom
