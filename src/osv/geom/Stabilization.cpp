// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Stabilisation corrections and the Gaussian log-map smoother.

#include "osv/geom/Stabilization.h"

#include <algorithm>
#include <cmath>

namespace osv::geom {

const char* stabilizationModeName(StabilizationMode mode) noexcept {
    switch (mode) {
    case StabilizationMode::Off: return "Off";
    case StabilizationMode::HorizonLock: return "HorizonLock";
    case StabilizationMode::Full: return "Full";
    case StabilizationMode::Smooth: return "Smooth";
    case StabilizationMode::SmoothLevel: return "SmoothLevel";
    }
    return "Unknown";
}

// -----------------------------------------------------------------------------
//  Quaternion log / exp
// -----------------------------------------------------------------------------
Vec3d quatLog(const Quatd& qIn) noexcept {
    Quatd q = qIn.normalized();
    // Take the short arc: q and -q are the same rotation.
    if (q.w < 0.0) {
        q = Quatd{-q.w, -q.x, -q.y, -q.z};
    }
    const Vec3d v{q.x, q.y, q.z};
    const double sinHalf = v.norm();
    if (sinHalf < 1e-12) {
        // Tiny rotation: log(q) ~= 2 * v.
        return v * 2.0;
    }
    const double angle = 2.0 * std::atan2(sinHalf, q.w);
    return v * (angle / sinHalf);
}

Quatd quatExp(const Vec3d& rotationVector) noexcept {
    if (!rotationVector.isFinite()) {
        return Quatd::identity();
    }
    const double angle = rotationVector.norm();
    if (angle < 1e-12) {
        // First-order expansion keeps this continuous at zero.
        return Quatd{1.0, 0.5 * rotationVector.x, 0.5 * rotationVector.y, 0.5 * rotationVector.z}.normalized();
    }
    const double s = std::sin(0.5 * angle) / angle;
    return Quatd{std::cos(0.5 * angle), rotationVector.x * s, rotationVector.y * s, rotationVector.z * s};
}

// -----------------------------------------------------------------------------
//  Euler decomposition R = Rz(yaw) * Rx(pitch) * Ry(roll)
// -----------------------------------------------------------------------------
EulerZXY eulerZXY(const Mat3d& r) noexcept {
    // With the composition above:
    //   r(2,1) = sin(pitch)
    //   r(0,1) = -sin(yaw) cos(pitch),  r(1,1) = cos(yaw) cos(pitch)
    //   r(2,0) = -cos(pitch) sin(roll), r(2,2) = cos(pitch) cos(roll)
    EulerZXY e;
    const double sp = clampd(r.at(2, 1), -1.0, 1.0);
    e.pitch = std::asin(sp);
    const double cp = std::cos(e.pitch);
    if (std::fabs(cp) > 1e-9) {
        e.yaw = std::atan2(-r.at(0, 1), r.at(1, 1));
        e.roll = std::atan2(-r.at(2, 0), r.at(2, 2));
    } else {
        // Gimbal lock: yaw and roll share an axis; give it all to yaw.
        e.roll = 0.0;
        e.yaw = std::atan2(r.at(1, 0), r.at(0, 0));
    }
    return e;
}

Mat3d fromEulerZXY(const EulerZXY& e) noexcept {
    return Mat3d::rotZ(e.yaw) * Mat3d::rotX(e.pitch) * Mat3d::rotY(e.roll);
}

// -----------------------------------------------------------------------------
//  Correction matrix
// -----------------------------------------------------------------------------
namespace {

/// True when `m` is finite, orthonormal and right-handed (to 1e-6): the only
/// kind of matrix a levelling mount may be.
bool isRotation(const Mat3d& m) noexcept {
    for (const double v : m.m) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return (m.transposed() * m).distance(Mat3d::identity()) < 1e-6 && m.determinant() > 0.0;
}

/// Build an orthonormal world basis (right, forward, up) with `up` as the
/// third axis and the horizontal projection of `forwardHint` as the second.
/// Returns false when the hint is (anti)parallel to up and no basis can be
/// chosen from it.
bool horizonBasis(const Vec3d& upIn, const Vec3d& forwardHint, Mat3d& basis) noexcept {
    const Vec3d up = upIn.normalized();
    if (!(up.norm() > 0.0)) {
        return false;
    }
    // Remove the vertical component of the hint.
    Vec3d forward = forwardHint - up * forwardHint.dot(up);
    if (!(forward.norm() > 1e-9)) {
        return false;
    }
    forward = forward.normalized();
    // Right-handed: right = forward x up (X = Y x Z).
    const Vec3d right = forward.cross(up).normalized();
    basis = Mat3d::fromColumns(right, forward, up);
    return true;
}

/// The levelled world basis HorizonLock and SmoothLevel work in: true up as
/// up, the reference pose's heading as forward.  A reference that looks
/// straight up or down has no heading, so its up axis stands in for it.
/// Returns false when neither gives a basis (a degenerate reference).
bool referenceHorizonBasis(const Vec3d& worldUp, const Mat3d& rRef, Mat3d& basis) noexcept {
    // The reference body's forward axis (+Y) is the heading hint.
    if (horizonBasis(worldUp, rRef * Vec3d{0.0, 1.0, 0.0}, basis)) {
        return true;
    }
    // Reference looks straight up/down: use its up axis as heading hint.
    return horizonBasis(worldUp, rRef * Vec3d{0.0, 0.0, 1.0}, basis);
}

/// The levelling itself: the orientation `rWorldFromSource` expressed in the
/// levelled `basis`, with the locked axes of `params` zeroed.  The result is
/// levelled-from-view - the pose the view keeps.  HorizonLock passes the body
/// itself, SmoothLevel the smoothed body; that is the only difference
/// between the two modes.
Mat3d levelledFromView(const Mat3d& basis, const Mat3d& rWorldFromSource,
                       const StabilizationParams& params) noexcept {
    // Source orientation expressed in the levelled basis
    // (basis^T maps world -> levelled world, columns are unit axes).
    const Mat3d rLevelledFromSource = basis.transposed() * rWorldFromSource;
    EulerZXY e = eulerZXY(rLevelledFromSource);
    // Zero the locked axes; whatever is not locked follows the source.
    if (params.lockYaw) {
        e.yaw = 0.0;
    }
    if (params.lockPitch) {
        e.pitch = 0.0;
    }
    if (params.lockRoll) {
        e.roll = 0.0;
    }
    return fromEulerZXY(e);
}

}  // namespace

Mat3d stabilizationBodyFromWorld(const Quatd& worldFromBody, const StabilizationParams& params,
                                 const Quatd& reference, const Vec3d& worldUp,
                                 const std::optional<Quatd>& smoothed) noexcept {
    if (!worldFromBody.isFinite() || !reference.isFinite()) {
        return Mat3d::identity();
    }
    const Mat3d rWb = worldFromBody.normalized().toMatrix();   // body -> world
    const Mat3d rBw = rWb.transposed();                         // world -> body
    const Mat3d rRef = reference.normalized().toMatrix();       // reference body -> world

    switch (params.mode) {
    case StabilizationMode::Off:
        return Mat3d::identity();

    case StabilizationMode::Full:
        // View rays live in the reference body frame: to world, then into
        // the current body.
        return rBw * rRef;

    case StabilizationMode::Smooth: {
        if (!smoothed.has_value() || !smoothed->isFinite()) {
            return Mat3d::identity();
        }
        // View rays live in the smoothed body frame.
        return rBw * smoothed->normalized().toMatrix();
    }

    case StabilizationMode::HorizonLock:
    case StabilizationMode::SmoothLevel: {
        // The levelling works in the mounted frame (levellingMount): every
        // pose is taken as world <- body <- mounted, and the result mapped
        // back into the body at the end.  An identity mount - every Osmo 360
        // clip - makes this exactly the unmounted computation.  A mount that
        // is not a proper rotation (a corrupt caller) is ignored.
        const Mat3d mount = isRotation(params.mount) ? params.mount : Mat3d::identity();
        const Mat3d rWm = rWb * mount;
        const Mat3d rRefM = rRef * mount;
        // World basis with the reference heading as forward and true up as up.
        Mat3d basis;
        if (!referenceHorizonBasis(worldUp, rRefM, basis)) {
            return Mat3d::identity();
        }
        // The pose whose level part the view keeps.  HorizonLock levels the
        // body itself, so the heading carries the body's yaw shake.
        // SmoothLevel levels the smoothed body instead: the heading is the
        // smoothed heading (RockSteady) and pitch / roll are level (Horizon
        // Leveling).  Without a usable smoothed pose it levels the body -
        // the horizon stays level even when the smoothing is unavailable.
        Mat3d rWorldFromSource = rWm;
        if (params.mode == StabilizationMode::SmoothLevel && smoothed.has_value() && smoothed->isFinite()) {
            rWorldFromSource = smoothed->normalized().toMatrix() * mount;
        }
        const Mat3d rLevelledFromView = levelledFromView(basis, rWorldFromSource, params);
        // Current (mounted) body orientation expressed in the same levelled basis.
        const Mat3d rLevelledFromMounted = basis.transposed() * rWm;
        // body <- mounted <- levelled <- view.
        return mount * (rLevelledFromMounted.transposed() * rLevelledFromView);
    }
    }
    return Mat3d::identity();
}

// -----------------------------------------------------------------------------
//  Levelling mount
// -----------------------------------------------------------------------------
Mat3d levellingMount(const std::vector<Quatd>& worldFromBody, const Vec3d& worldUp) noexcept {
    const Vec3d up = worldUp.normalized();
    if (!(up.norm() > 0.5)) {
        return Mat3d::identity();
    }
    // How vertical the body's +Y (the lens axis) is over the clip: the mean of
    // |cos| to up decides the frame, the mean of the signed cos which lens is
    // the upper one.  Every sample counts once; a clip is a few thousand.
    double sumAbs = 0.0;
    double sumSigned = 0.0;
    std::size_t used = 0;
    for (const Quatd& q : worldFromBody) {
        if (!q.isFinite()) {
            continue;
        }
        const double c = (q.normalized().toMatrix() * Vec3d{0.0, 1.0, 0.0}).dot(up);
        if (!std::isfinite(c)) {
            continue;
        }
        sumAbs += std::fabs(c);
        sumSigned += c;
        ++used;
    }
    // cos(45 deg): closer to the horizon than to vertical keeps the body as
    // it is, which is every clip the Osmo 360 has recorded.
    constexpr double kVerticalLensAxisCos = 0.70710678118654752;
    if (used == 0 || sumAbs / static_cast<double>(used) <= kVerticalLensAxisCos) {
        return Mat3d::identity();
    }
    // A quarter turn about +X: columns (X, s*Z, -s*Y), right handed for either
    // sign since X x (s*Z) = -s*Y.  With +Y up on average s = -1, giving
    // (right +X, forward -Z, up +Y); with +Y down s = +1, giving (+X, +Z, -Y).
    const double s = (sumSigned >= 0.0) ? -1.0 : 1.0;
    return Mat3d::fromColumns(Vec3d{1.0, 0.0, 0.0}, Vec3d{0.0, 0.0, s}, Vec3d{0.0, -s, 0.0});
}

// -----------------------------------------------------------------------------
//  Smoother
// -----------------------------------------------------------------------------
Smoother::Smoother(double sigmaFrames) noexcept
    : m_sigma((std::isfinite(sigmaFrames) && sigmaFrames > 0.0) ? sigmaFrames : 0.0) {}

std::size_t Smoother::halfWidth() const noexcept {
    if (m_sigma <= 0.0) {
        return 0;
    }
    const double hw = std::ceil(3.0 * m_sigma);
    return static_cast<std::size_t>(std::max(1.0, hw));
}

Quatd Smoother::smoothedAt(const std::vector<Quatd>& input, std::size_t k) const noexcept {
    if (k >= input.size()) {
        return Quatd::identity();
    }
    const Quatd centre = input[k].normalized();
    if (m_sigma <= 0.0 || input.size() == 1) {
        return centre;
    }
    // Window [k - hw, k + hw] clamped to the sequence.
    const std::size_t hw = halfWidth();
    const std::size_t lo = (k >= hw) ? k - hw : 0;
    const std::size_t hi = std::min(input.size() - 1, k + hw);
    const Quatd centreInv = centre.conj();
    // Weighted mean of the tangent vectors log(q_k^-1 q_i) around q_k.
    Vec3d sum;
    double weightSum = 0.0;
    for (std::size_t i = lo; i <= hi; ++i) {
        if (!input[i].isFinite()) {
            continue;
        }
        const double d = (static_cast<double>(i) - static_cast<double>(k)) / m_sigma;
        const double wgt = std::exp(-0.5 * d * d);
        Quatd rel = centreInv * input[i].normalized();
        // Keep the relative rotation on the short arc.
        if (rel.w < 0.0) {
            rel = Quatd{-rel.w, -rel.x, -rel.y, -rel.z};
        }
        sum += quatLog(rel) * wgt;
        weightSum += wgt;
    }
    if (!(weightSum > 0.0)) {
        return centre;
    }
    const Quatd result = centre * quatExp(sum / weightSum);
    return result.isFinite() ? result.normalized() : centre;
}

std::vector<Quatd> Smoother::smooth(const std::vector<Quatd>& input) const {
    std::vector<Quatd> out;
    out.reserve(input.size());
    for (std::size_t k = 0; k < input.size(); ++k) {
        out.push_back(smoothedAt(input, k));
    }
    return out;
}

}  // namespace osv::geom
