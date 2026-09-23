// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensProtector implementation: the fitted curve, its inverse and the
// Kannala-Brandt refit that folds it into a lens (see the header for the
// optics and the provenance of the curve).

#include "osv/geom/LensProtector.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace osv::geom {

namespace {

// -----------------------------------------------------------------------------
//  Small dense least squares (Householder QR)
// -----------------------------------------------------------------------------

/// Number of Kannala-Brandt coefficients refitted: f (the theta term) and
/// k1..k5.
constexpr int kTerms = 6;

/// Solve min |A x - b| for a column-major m x kTerms matrix by Householder
/// QR.  Normal equations would square the condition number of an odd
/// monomial basis up to u^11, which is exactly the kind of basis that loses
/// digits that way; QR keeps them.  Returns false on a rank-deficient system.
bool leastSquares(std::vector<double>& a, std::vector<double>& b, int m, std::array<double, kTerms>& x) noexcept {
    if (m < kTerms || static_cast<int>(a.size()) != m * kTerms || static_cast<int>(b.size()) != m) {
        return false;
    }
    auto at = [&a, m](int row, int col) -> double& { return a[static_cast<std::size_t>(col) * m + row]; };

    // ---- factorise: A = Q R, applying each reflector to b as we go --------
    for (int k = 0; k < kTerms; ++k) {
        double norm = 0.0;
        for (int i = k; i < m; ++i) {
            norm += at(i, k) * at(i, k);
        }
        norm = std::sqrt(norm);
        if (!(norm > 0.0) || !std::isfinite(norm)) {
            return false;  // A zero column: the basis is degenerate.
        }
        // Reflect onto -sign(a_kk) * |a| e_k to avoid cancellation.
        const double alpha = at(k, k) > 0.0 ? -norm : norm;
        const double v0 = at(k, k) - alpha;
        // v = (a_k - alpha e_k), stored in place below the diagonal.
        at(k, k) = v0;
        double vv = 0.0;
        for (int i = k; i < m; ++i) {
            vv += at(i, k) * at(i, k);
        }
        if (!(vv > 0.0)) {
            return false;
        }
        // Apply H = I - 2 v v^T / (v^T v) to the remaining columns and to b.
        for (int j = k + 1; j < kTerms; ++j) {
            double dot = 0.0;
            for (int i = k; i < m; ++i) {
                dot += at(i, k) * at(i, j);
            }
            const double s = 2.0 * dot / vv;
            for (int i = k; i < m; ++i) {
                at(i, j) -= s * at(i, k);
            }
        }
        double dotB = 0.0;
        for (int i = k; i < m; ++i) {
            dotB += at(i, k) * b[static_cast<std::size_t>(i)];
        }
        const double sb = 2.0 * dotB / vv;
        for (int i = k; i < m; ++i) {
            b[static_cast<std::size_t>(i)] -= sb * at(i, k);
        }
        // The diagonal of R is alpha; keep it where back-substitution reads it.
        at(k, k) = alpha;
    }

    // ---- back-substitute R x = Q^T b ----------------------------------------
    for (int k = kTerms - 1; k >= 0; --k) {
        double sum = b[static_cast<std::size_t>(k)];
        for (int j = k + 1; j < kTerms; ++j) {
            sum -= at(k, j) * x[static_cast<std::size_t>(j)];
        }
        const double diag = at(k, k);
        if (!(std::fabs(diag) > 1e-300)) {
            return false;
        }
        x[static_cast<std::size_t>(k)] = sum / diag;
        if (!std::isfinite(x[static_cast<std::size_t>(k)])) {
            return false;
        }
    }
    return true;
}

/// The angle the native model must be evaluated at for a world ray at
/// `theta`: g(theta) for Forward, g^-1(theta) for Inverse.
Result<double> composedAngle(ProtectorDirection direction, double theta) noexcept {
    switch (direction) {
    case ProtectorDirection::Forward: return lensProtectorAngle(theta);
    case ProtectorDirection::Inverse: return lensProtectorAngleInverse(theta);
    case ProtectorDirection::None: return theta;
    }
    return Error{ErrorCode::InvalidArgument, "unknown protector direction"};
}

/// The world angle whose composed angle is `lensAngle` - the inverse of
/// composedAngle(), used to move the usable FOV so it keeps its pixels.
Result<double> worldAngleFor(ProtectorDirection direction, double lensAngle) noexcept {
    switch (direction) {
    case ProtectorDirection::Forward: return lensProtectorAngleInverse(lensAngle);
    case ProtectorDirection::Inverse: return lensProtectorAngle(lensAngle);
    case ProtectorDirection::None: return lensAngle;
    }
    return Error{ErrorCode::InvalidArgument, "unknown protector direction"};
}

}  // namespace

// -----------------------------------------------------------------------------
//  The curve
// -----------------------------------------------------------------------------

const char* protectorDirectionName(ProtectorDirection direction) noexcept {
    switch (direction) {
    case ProtectorDirection::None: return "none";
    case ProtectorDirection::Forward: return "forward";
    case ProtectorDirection::Inverse: return "inverse";
    }
    return "unknown";
}

double lensProtectorAngle(double thetaRad) noexcept {
    if (!std::isfinite(thetaRad)) {
        return std::nan("");
    }
    // theta * (a0 + a1 t^2 + a2 t^4 + a3 t^6 + a4 t^8), Horner in t^2.
    const auto& a = kLensProtectorCoefficients;
    const double t2 = thetaRad * thetaRad;
    return thetaRad * (a[0] + t2 * (a[1] + t2 * (a[2] + t2 * (a[3] + t2 * a[4]))));
}

double lensProtectorAngleDerivative(double thetaRad) noexcept {
    if (!std::isfinite(thetaRad)) {
        return std::nan("");
    }
    // a0 + 3 a1 t^2 + 5 a2 t^4 + 7 a3 t^6 + 9 a4 t^8.
    const auto& a = kLensProtectorCoefficients;
    const double t2 = thetaRad * thetaRad;
    return a[0] + t2 * (3.0 * a[1] + t2 * (5.0 * a[2] + t2 * (7.0 * a[3] + t2 * 9.0 * a[4])));
}

Result<double> lensProtectorAngleInverse(double phiRad) noexcept {
    if (!std::isfinite(phiRad)) {
        return Error{ErrorCode::InvalidArgument, "lens protector inverse: angle is not finite"};
    }
    // Odd curve: solve for |phi| and restore the sign.
    const double sign = phiRad < 0.0 ? -1.0 : 1.0;
    const double target = std::fabs(phiRad);
    const double hi = deg2rad(kLensProtectorMonotonicMaxDeg);
    const double gHi = lensProtectorAngle(hi);
    if (target > gHi) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("lens protector inverse: {:.3f} deg is beyond the curve's monotonic range",
                                 rad2deg(target))};
    }
    // Safeguarded Newton: g is monotonic on [0, hi], so a bracket plus
    // Newton steps that fall back to bisection always converge.
    double lo = 0.0;
    double up = hi;
    double t = target / kLensProtectorCoefficients[0];
    for (int it = 0; it < 64; ++it) {
        if (!(t > lo && t < up)) {
            t = 0.5 * (lo + up);
        }
        const double f = lensProtectorAngle(t) - target;
        if (std::fabs(f) < 1e-15) {
            break;
        }
        if (f > 0.0) {
            up = t;
        } else {
            lo = t;
        }
        const double d = lensProtectorAngleDerivative(t);
        t = (d > 0.0) ? t - f / d : 0.5 * (lo + up);
        if (up - lo < 1e-15) {
            break;
        }
    }
    return sign * t;
}

// -----------------------------------------------------------------------------
//  Folding into a lens
// -----------------------------------------------------------------------------

Result<ProtectorFold> foldLensProtector(const KannalaBrandt5& lens, ProtectorDirection direction) noexcept {
    if (!lens.isValid()) {
        return Error{ErrorCode::InvalidArgument, "lens protector fold: the input lens is invalid"};
    }
    ProtectorFold out;
    out.lens = lens;
    out.thetaMaxRad = lens.thetaMaxRad;
    if (direction == ProtectorDirection::None) {
        return out;  // Bare lens: nothing to fold, bit for bit.
    }
    if (direction != ProtectorDirection::Forward && direction != ProtectorDirection::Inverse) {
        return Error{ErrorCode::InvalidArgument, "lens protector fold: unknown direction"};
    }

    try {
        // ---- the new usable FOV: same image-circle pixels, new world angle --
        OSV_TRY_ASSIGN(const double thetaMaxWorld, worldAngleFor(direction, lens.thetaMaxRad));
        if (!(thetaMaxWorld > 0.0) || !std::isfinite(thetaMaxWorld)) {
            return Error{ErrorCode::Internal, "lens protector fold: the usable FOV collapsed"};
        }

        // ---- samples of the exact composition thetaD(h(theta)) --------------
        // Fitted a couple of degrees past the usable FOV so the feather and
        // the analyses, which look slightly beyond thetaMax, stay accurate.
        const double thetaFit = thetaMaxWorld + deg2rad(2.0);
        constexpr int kSamples = 480;
        std::vector<double> a(static_cast<std::size_t>(kSamples) * kTerms);
        std::vector<double> b(static_cast<std::size_t>(kSamples));
        for (int j = 0; j < kSamples; ++j) {
            const double theta = thetaFit * static_cast<double>(j) / static_cast<double>(kSamples - 1);
            OSV_TRY_ASSIGN(const double h, composedAngle(direction, theta));
            b[static_cast<std::size_t>(j)] = lens.thetaD(h);
            // Odd basis in u = theta / thetaFit (in [0, 1]) keeps the columns
            // comparable in size; rescaled to theta powers after the solve.
            const double u = theta / thetaFit;
            double p = u;
            for (int c = 0; c < kTerms; ++c) {
                a[static_cast<std::size_t>(c) * kSamples + j] = p;
                p *= u * u;
            }
        }

        // ---- solve and convert to Kannala-Brandt form ------------------------
        std::array<double, kTerms> cu{};
        if (!leastSquares(a, b, kSamples, cu)) {
            return Error{ErrorCode::Internal, "lens protector fold: the refit is rank deficient"};
        }
        // c_i (theta basis) = cu_i / thetaFit^(2i+1).
        std::array<double, kTerms> c{};
        double scale = thetaFit;
        for (int i = 0; i < kTerms; ++i) {
            c[static_cast<std::size_t>(i)] = cu[static_cast<std::size_t>(i)] / scale;
            scale *= thetaFit * thetaFit;
        }
        const double s = c[0];
        if (!(s > 0.0) || !std::isfinite(s)) {
            return Error{ErrorCode::Internal, "lens protector fold: the refitted focal scale is not positive"};
        }
        KannalaBrandt5 folded = lens;
        folded.fx = lens.fx * s;
        folded.fy = lens.fy * s;
        for (int i = 0; i < 5; ++i) {
            folded.k[static_cast<std::size_t>(i)] = c[static_cast<std::size_t>(i) + 1] / s;
        }
        folded.thetaMaxRad = thetaMaxWorld;
        folded.updateRMax();
        if (!folded.isValid()) {
            return Error{ErrorCode::Internal, "lens protector fold: the folded lens is not finite"};
        }
        if (!folded.isMonotonic(thetaFit)) {
            return Error{ErrorCode::Internal, "lens protector fold: the folded lens folds back inside its FOV"};
        }

        // ---- residual against the exact composition, in pixels -------------
        const double fMean = 0.5 * (lens.fx + lens.fy);
        double worst = 0.0;
        constexpr int kCheck = 2048;
        for (int j = 0; j <= kCheck; ++j) {
            const double theta = thetaMaxWorld * static_cast<double>(j) / kCheck;
            OSV_TRY_ASSIGN(const double h, composedAngle(direction, theta));
            const double exact = fMean * lens.thetaD(h);
            const double fitted = 0.5 * (folded.fx + folded.fy) * folded.thetaD(theta);
            worst = std::max(worst, std::fabs(fitted - exact));
        }
        if (!(worst < 0.5)) {
            return Error{ErrorCode::Internal,
                         std::format("lens protector fold: refit misses the exact curve by {:.3f} px", worst)};
        }
        out.lens = folded;
        out.thetaMaxRad = thetaMaxWorld;
        out.maxResidualPx = worst;
        return out;
    } catch (...) {
        // The sample vectors are the only allocation; a failure there must
        // not escape a noexcept function.
        return Error{ErrorCode::Internal, "lens protector fold: out of memory"};
    }
}

Result<ProtectorRigFold> applyLensProtector(LensRig& rig, ProtectorDirection direction,
                                            double baseLensFovDeg) noexcept {
    if (!std::isfinite(baseLensFovDeg) || baseLensFovDeg <= 0.0 || baseLensFovDeg > 360.0) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("lens protector: bad base lens FOV {} deg", baseLensFovDeg)};
    }
    ProtectorRigFold result;
    result.lensFovDeg = baseLensFovDeg;
    if (direction == ProtectorDirection::None) {
        return result;  // Nothing changes; the rig is untouched.
    }

    // ---- fold both lenses into locals first (all or nothing) --------------
    std::array<KannalaBrandt5, kLensCount> folded{};
    for (int i = 0; i < kLensCount; ++i) {
        OSV_TRY_ASSIGN(const ProtectorFold f, foldLensProtector(rig.lens[static_cast<std::size_t>(i)], direction));
        folded[static_cast<std::size_t>(i)] = f.lens;
        result.maxResidualPx = std::max(result.maxResidualPx, f.maxResidualPx);
    }

    // ---- the blend FOV follows the same image-circle rule -----------------
    OSV_TRY_ASSIGN(const double halfWorld, worldAngleFor(direction, deg2rad(0.5 * baseLensFovDeg)));
    result.lensFovDeg = 2.0 * rad2deg(halfWorld);

    // ---- commit -----------------------------------------------------------------
    rig.lens = folded;
    rig.lensFovDeg = result.lensFovDeg;
    try {
        rig.notes.push_back(std::format("lens protector correction ({}): usable FOV {:.2f} -> {:.2f} deg, "
                                        "refit residual {:.4f} px",
                                        protectorDirectionName(direction), baseLensFovDeg, result.lensFovDeg,
                                        result.maxResidualPx));
    } catch (...) {
        // A note that could not be allocated is not worth failing the rig for.
    }
    return result;
}

}  // namespace osv::geom
