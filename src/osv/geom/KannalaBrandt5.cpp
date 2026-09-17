// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Kannala-Brandt five-term fisheye model: forward polynomial, Newton
// inversion, projection / unprojection and the GPU seed table.

#include "osv/geom/KannalaBrandt5.h"

#include <cmath>
#include <limits>

namespace osv::geom {

namespace {

/// Smallest ray angle treated as "on the optical axis" (avoids atan2(0, 0)
/// noise and a division by zero in the azimuth).
constexpr double kAxisEpsilon = 1e-15;

}  // namespace

// -----------------------------------------------------------------------------
//  Construction
// -----------------------------------------------------------------------------
KannalaBrandt5 KannalaBrandt5::fromDewarp(const meta::DewarpParams& params) noexcept {
    KannalaBrandt5 lens;
    // Intrinsics are copied verbatim in calibration pixel units.
    lens.fx = static_cast<double>(params.fx);
    lens.fy = static_cast<double>(params.fy);
    lens.cx = static_cast<double>(params.cx);
    lens.cy = static_cast<double>(params.cy);
    // The record carries nine radial terms; the model uses the first five
    // (k1..k5).  k6..k9 are zero on every clip seen so far.
    for (std::size_t i = 0; i < lens.k.size() && i < params.k.size(); ++i) {
        lens.k[i] = static_cast<double>(params.k[i]);
    }
    // Any non-finite input is neutralised so later math cannot spread NaNs.
    for (double& term : lens.k) {
        if (!std::isfinite(term)) {
            term = 0.0;
        }
    }
    lens.updateRMax();
    return lens;
}

void KannalaBrandt5::updateRMax() noexcept {
    // Image circle radius at the usable FOV, using the mean focal length.
    const double fMean = 0.5 * (fx + fy);
    const double rd = thetaD(thetaMaxRad);
    rMaxPx = (std::isfinite(fMean) && std::isfinite(rd) && fMean > 0.0) ? fMean * rd : 0.0;
}

bool KannalaBrandt5::isValid() const noexcept {
    // Focal lengths must be positive and everything finite.
    if (!(fx > 0.0) || !(fy > 0.0)) {
        return false;
    }
    if (!std::isfinite(fx) || !std::isfinite(fy) || !std::isfinite(cx) || !std::isfinite(cy)) {
        return false;
    }
    if (!std::isfinite(thetaMaxRad) || thetaMaxRad <= 0.0) {
        return false;
    }
    for (const double term : k) {
        if (!std::isfinite(term)) {
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
//  Radial polynomial
// -----------------------------------------------------------------------------
double KannalaBrandt5::thetaD(double theta) const noexcept {
    // theta * (1 + k1 t^2 + k2 t^4 + k3 t^6 + k4 t^8 + k5 t^10), Horner form
    // in t^2 for fewer roundings.
    const double t2 = theta * theta;
    const double poly = 1.0 + t2 * (k[0] + t2 * (k[1] + t2 * (k[2] + t2 * (k[3] + t2 * k[4]))));
    return theta * poly;
}

double KannalaBrandt5::dThetaD(double theta) const noexcept {
    // d/dtheta of the polynomial above:
    // 1 + 3 k1 t^2 + 5 k2 t^4 + 7 k3 t^6 + 9 k4 t^8 + 11 k5 t^10.
    const double t2 = theta * theta;
    return 1.0 + t2 * (3.0 * k[0] + t2 * (5.0 * k[1] + t2 * (7.0 * k[2] + t2 * (9.0 * k[3] + t2 * 11.0 * k[4]))));
}

// -----------------------------------------------------------------------------
//  Newton inversion
// -----------------------------------------------------------------------------
Result<double> KannalaBrandt5::thetaFromThetaD(double rd, int* iterations) const noexcept {
    if (iterations) {
        *iterations = 0;
    }
    // Garbage in -> explicit failure rather than a NaN that spreads.
    if (!std::isfinite(rd)) {
        return Error{ErrorCode::InvalidArgument, "thetaFromThetaD: non-finite input"};
    }
    // Negative radii have no meaning; the axis itself is trivial.
    if (rd < 0.0) {
        return Error{ErrorCode::InvalidArgument, "thetaFromThetaD: negative radius"};
    }
    if (rd <= kAxisEpsilon) {
        return 0.0;
    }

    // Newton's method on f(theta) = thetaD(theta) - rd starting at theta = rd
    // (exact for an ideal equidistant lens, so a good seed for small k).
    double theta = rd;
    for (int iter = 1; iter <= kNewtonMaxIterations; ++iter) {
        const double derivative = dThetaD(theta);
        // A non-positive slope means the polynomial folds back: the pixel
        // cannot be inverted unambiguously.
        if (!(derivative > 0.0) || !std::isfinite(derivative)) {
            if (iterations) {
                *iterations = iter;
            }
            return Error{ErrorCode::Malformed, "thetaFromThetaD: lens model is non-monotonic at this radius"};
        }
        const double step = (thetaD(theta) - rd) / derivative;
        theta -= step;
        if (iterations) {
            *iterations = iter;
        }
        // Keep the iterate physical; a negative angle would mirror the ray.
        if (theta < 0.0) {
            theta = 0.0;
        }
        if (!std::isfinite(theta)) {
            return Error{ErrorCode::Internal, "thetaFromThetaD: iteration diverged"};
        }
        if (std::fabs(step) < kNewtonTolerance) {
            return theta;
        }
    }
    return Error{ErrorCode::Internal, "thetaFromThetaD: Newton did not converge"};
}

// -----------------------------------------------------------------------------
//  Projection
// -----------------------------------------------------------------------------
bool KannalaBrandt5::project(const Vec3d& dLens, Vec2d& px, double& theta) const noexcept {
    if (!dLens.isFinite() || !isValid()) {
        return false;
    }
    // Angle from the optical axis (+z) and azimuth in the image plane.
    const double rxy = std::hypot(dLens.x, dLens.y);
    const double norm = std::hypot(rxy, dLens.z);
    if (!(norm > 0.0)) {
        return false;
    }
    theta = std::atan2(rxy, dLens.z);
    // Beyond the usable field of view: no pixel.
    if (theta > thetaMaxRad) {
        return false;
    }
    // On the axis the azimuth is undefined; the pixel is the principal point.
    if (rxy <= kAxisEpsilon * norm) {
        px = Vec2d{cx, cy};
        return true;
    }
    const double rd = thetaD(theta);
    // cos(phi) = x / rxy, sin(phi) = y / rxy without calling atan2 / cos / sin.
    const Vec2d out{cx + fx * rd * (dLens.x / rxy), cy + fy * rd * (dLens.y / rxy)};
    if (!std::isfinite(out.x) || !std::isfinite(out.y)) {
        return false;
    }
    px = out;
    return true;
}

Result<Vec3d> KannalaBrandt5::unproject(const Vec2d& px, int* iterations) const noexcept {
    if (iterations) {
        *iterations = 0;
    }
    if (!isValid()) {
        return Error{ErrorCode::InvalidArgument, "unproject: invalid lens model"};
    }
    if (!std::isfinite(px.x) || !std::isfinite(px.y)) {
        return Error{ErrorCode::InvalidArgument, "unproject: non-finite pixel"};
    }
    // Normalised image coordinates (theta_d * cos(phi), theta_d * sin(phi)).
    const double nx = (px.x - cx) / fx;
    const double ny = (px.y - cy) / fy;
    const double rd = std::hypot(nx, ny);
    // Solve for theta and rebuild the ray.
    OSV_TRY_ASSIGN(const double theta, thetaFromThetaD(rd, iterations));
    if (rd <= kAxisEpsilon) {
        return Vec3d{0.0, 0.0, 1.0};
    }
    const double sinTheta = std::sin(theta);
    const double cosTheta = std::cos(theta);
    const Vec3d dir{sinTheta * (nx / rd), sinTheta * (ny / rd), cosTheta};
    if (!dir.isFinite()) {
        return Error{ErrorCode::Internal, "unproject: non-finite direction"};
    }
    return dir;
}

// -----------------------------------------------------------------------------
//  Monotonicity / seed table
// -----------------------------------------------------------------------------
bool KannalaBrandt5::isMonotonic(double thetaMax, std::size_t samples) const noexcept {
    if (!std::isfinite(thetaMax) || thetaMax <= 0.0) {
        return false;
    }
    if (samples < 2) {
        samples = 2;
    }
    // The derivative must stay strictly positive on the whole range,
    // including the end point.
    for (std::size_t i = 0; i < samples; ++i) {
        const double theta = thetaMax * static_cast<double>(i) / static_cast<double>(samples - 1);
        const double derivative = dThetaD(theta);
        if (!(derivative > 0.0) || !std::isfinite(derivative)) {
            return false;
        }
    }
    return true;
}

double KannalaBrandt5::seedTableRdMax() const noexcept {
    const double rd = thetaD(thetaMaxRad);
    return (std::isfinite(rd) && rd > 0.0) ? rd : 0.0;
}

std::vector<float> KannalaBrandt5::seedTable(std::size_t n) const {
    std::vector<float> table;
    if (n < 2 || !isValid()) {
        return table;
    }
    const double rdMax = seedTableRdMax();
    if (rdMax <= 0.0) {
        return table;
    }
    table.reserve(n);
    double lastGood = 0.0;
    // Uniform in rd so the GPU can index with rd / rdMax * (n - 1).
    for (std::size_t i = 0; i < n; ++i) {
        const double rd = rdMax * static_cast<double>(i) / static_cast<double>(n - 1);
        const auto theta = thetaFromThetaD(rd);
        if (theta.ok()) {
            lastGood = theta.value();
        }
        // A failed inversion repeats the previous entry (keeps the table
        // monotonic and finite); it can only happen past a fold of the model.
        table.push_back(static_cast<float>(lastGood));
    }
    return table;
}

}  // namespace osv::geom
