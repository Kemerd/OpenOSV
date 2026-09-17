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

    case StabilizationMode::HorizonLock: {
        // World basis with the reference heading as forward and true up as up.
        Mat3d basis;
        Vec3d hint = rRef * Vec3d{0.0, 1.0, 0.0};
        if (!horizonBasis(worldUp, hint, basis)) {
            // Reference looks straight up/down: use its up axis as heading hint.
            hint = rRef * Vec3d{0.0, 0.0, 1.0};
            if (!horizonBasis(worldUp, hint, basis)) {
                return Mat3d::identity();
            }
        }
        // Current body orientation expressed in that levelled basis
        // (basis^T maps world -> levelled world, columns are unit axes).
        const Mat3d rLevelledFromBody = basis.transposed() * rWb;
        EulerZXY e = eulerZXY(rLevelledFromBody);
        // Zero the locked axes; whatever is not locked follows the body.
        if (params.lockYaw) {
            e.yaw = 0.0;
        }
        if (params.lockPitch) {
            e.pitch = 0.0;
        }
        if (params.lockRoll) {
            e.roll = 0.0;
        }
        const Mat3d rLevelledFromView = fromEulerZXY(e);
        // body <- levelled <- view.
        return rLevelledFromBody.transposed() * rLevelledFromView;
    }
    }
    return Mat3d::identity();
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
