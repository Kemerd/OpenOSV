// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensAlign.cpp - robust fit of the relative lens rotation to a parallax
// grid's raw cells, the per-clip combination, and the fold into the rig.
// The model and the sign rules are derived in LensAlign.h.

#include "osv/render/LensAlign.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

namespace osv::render {

namespace {

/// One usable cell: where it is and what it measured, in TRUE angles.
struct CellSample {
    Vec3d east;       ///< Design row of the east equation (e_lat).
    Vec3d north;      ///< Design row of the north equation (-e_lon).
    double mEast = 0.0;   ///< Measured east disparity, true angle (cos(lat) * dLon), radians.
    double mNorth = 0.0;  ///< Measured north disparity (dLat), radians.
};

/// A symmetric 3 x 3 matrix and a 3-vector: the normal equations.
struct Normal {
    std::array<double, 9> a{};  ///< Row-major A^T W A.
    std::array<double, 3> b{};  ///< A^T W m.
    double weight = 0.0;        ///< Sum of the equation weights (for scale-free conditioning).

    /// Add one equation row `r` with measurement `m` and weight `w`.
    void add(const Vec3d& r, double m, double w) noexcept {
        const double v[3] = {r.x, r.y, r.z};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                a[static_cast<std::size_t>(i * 3 + j)] += w * v[i] * v[j];
            }
            b[static_cast<std::size_t>(i)] += w * v[i] * m;
        }
        weight += w;
    }
};

/// Solve the 3 x 3 system by Cramer's rule; false when it is singular.
[[nodiscard]] bool solve3(const Normal& n, Vec3d& out) noexcept {
    const std::array<double, 9>& m = n.a;
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    // Relative to the matrix's own scale, so a well-posed but small-weight
    // system is not mistaken for a singular one.
    const double scale = std::max({std::fabs(m[0]), std::fabs(m[4]), std::fabs(m[8]), 1e-300});
    if (!std::isfinite(det) || std::fabs(det) <= 1e-12 * scale * scale * scale) {
        return false;
    }
    const auto detWith = [&](int col) {
        std::array<double, 9> c = m;
        for (int r = 0; r < 3; ++r) {
            c[static_cast<std::size_t>(r * 3 + col)] = n.b[static_cast<std::size_t>(r)];
        }
        return c[0] * (c[4] * c[8] - c[5] * c[7]) - c[1] * (c[3] * c[8] - c[5] * c[6]) +
               c[2] * (c[3] * c[7] - c[4] * c[6]);
    };
    out = Vec3d{detWith(0) / det, detWith(1) / det, detWith(2) / det};
    return out.isFinite();
}

/// Eigenvalues of a symmetric 3 x 3 matrix by cyclic Jacobi rotations
/// (a handful of sweeps converge to machine precision at this size).
[[nodiscard]] std::array<double, 3> symmetricEigenvalues(std::array<double, 9> m) noexcept {
    for (int sweep = 0; sweep < 32; ++sweep) {
        // Done when the off-diagonal part is negligible against the diagonal
        // (or NaN, which no number of sweeps would fix).
        const double off = std::fabs(m[1]) + std::fabs(m[2]) + std::fabs(m[5]);
        const double diag = std::fabs(m[0]) + std::fabs(m[4]) + std::fabs(m[8]);
        if (!(off > 1e-15 * diag) || !(off > 1e-300)) {
            break;
        }
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const double apq = m[static_cast<std::size_t>(p * 3 + q)];
                if (std::fabs(apq) < 1e-300) {
                    continue;
                }
                const double app = m[static_cast<std::size_t>(p * 3 + p)];
                const double aqq = m[static_cast<std::size_t>(q * 3 + q)];
                // The classic stable rotation angle (Golub & Van Loan 8.5.2).
                const double theta = (aqq - app) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                // m <- J^T m J with J the (p, q) rotation.
                for (int k = 0; k < 3; ++k) {
                    const double mkp = m[static_cast<std::size_t>(k * 3 + p)];
                    const double mkq = m[static_cast<std::size_t>(k * 3 + q)];
                    m[static_cast<std::size_t>(k * 3 + p)] = c * mkp - s * mkq;
                    m[static_cast<std::size_t>(k * 3 + q)] = s * mkp + c * mkq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double mpk = m[static_cast<std::size_t>(p * 3 + k)];
                    const double mqk = m[static_cast<std::size_t>(q * 3 + k)];
                    m[static_cast<std::size_t>(p * 3 + k)] = c * mpk - s * mqk;
                    m[static_cast<std::size_t>(q * 3 + k)] = s * mpk + c * mqk;
                }
            }
        }
    }
    return {m[0], m[4], m[8]};
}

/// Median of a copy of `v` (the mean of the two middle values for an even
/// count); 0 for an empty input.
[[nodiscard]] double medianOf(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = v[mid];
    if (v.size() % 2 == 1) {
        return hi;
    }
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lo + hi);
}

/// Validate the tuning once.
[[nodiscard]] Status checkParams(const LensRotationParams& p) {
    if (!(p.minGate >= 0.0) || p.minGate > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "fitLensRotation: minGate must be within 0..1");
    }
    if (!(p.minInlierFraction >= 0.0) || p.minInlierFraction > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "fitLensRotation: minInlierFraction must be within 0..1");
    }
    if (!(p.maxResidualDeg > 0.0) || !(p.maxAngleDeg > 0.0) || !(p.agreeDeg > 0.0) ||
        !std::isfinite(p.maxResidualDeg) || !std::isfinite(p.maxAngleDeg) || !std::isfinite(p.agreeDeg)) {
        return failStatus(ErrorCode::InvalidArgument, "fitLensRotation: tolerances must be finite and > 0");
    }
    if (!(p.minConditioning >= 0.0) || p.minConditioning > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "fitLensRotation: minConditioning must be within 0..1");
    }
    if (p.huberPasses < 0 || p.tukeyPasses < 0 || p.huberPasses > 50 || p.tukeyPasses > 50 || !(p.huberK > 0.0) ||
        !(p.tukeyC > 0.0) || !std::isfinite(p.huberK) || !std::isfinite(p.tukeyC)) {
        return failStatus(ErrorCode::InvalidArgument, "fitLensRotation: robust pass settings out of range");
    }
    if (p.minCells < 3) {
        return failStatus(ErrorCode::InvalidArgument, "fitLensRotation: minCells must be at least 3");
    }
    return okStatus();
}

}  // namespace

// ---------------------------------------------------------------------------
//  One grid
// ---------------------------------------------------------------------------
Result<LensRotationFit> fitLensRotation(const ParallaxCellStats& cells, const LensRotationParams& params) {
    OSV_TRY(checkParams(params));
    if (!cells.valid()) {
        return Error{ErrorCode::InvalidArgument, "fitLensRotation: malformed cell statistics"};
    }

    // ---- gather the usable cells, in true angles --------------------------
    // A cell must carry enough consistent pixels to be a measurement, and
    // the benefit gate must have trusted it (see LensRotationParams).
    std::vector<CellSample> samples;
    samples.reserve(static_cast<std::size_t>(cells.w) * cells.rows);
    for (std::uint32_t r = 0; r < cells.rows; ++r) {
        const double lat = cells.latTopRad - static_cast<double>(r) * cells.latStepRad;
        const double cl = std::cos(lat);
        const double sl = std::sin(lat);
        for (std::uint32_t c = 0; c < cells.w; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * cells.w + c;
            if (cells.pixels[i] < params.minPixels || !(cells.gate[i] >= params.minGate)) {
                continue;
            }
            const double halfLon = static_cast<double>(cells.halfFlow[i * 2u + 0u]);
            const double halfLat = static_cast<double>(cells.halfFlow[i * 2u + 1u]);
            if (!std::isfinite(halfLon) || !std::isfinite(halfLat)) {
                continue;
            }
            // The kernel reads cell c at longitude fraction c / w of the ring.
            const double lon = static_cast<double>(c) * kTwoPi / static_cast<double>(cells.w) - kPi;
            const double cL = std::cos(lon);
            const double sL = std::sin(lon);
            // Local east / north unit vectors of d = (cl sL, sl, cl cL).
            const Vec3d eLon{cL, 0.0, -sL};
            const Vec3d eLat{-sl * sL, cl, -sl * cL};
            CellSample s;
            s.east = eLat;
            s.north = -eLon;
            // The grid stores HALF the disparity; the model is the full one.
            s.mEast = cl * 2.0 * halfLon;
            s.mNorth = 2.0 * halfLat;
            samples.push_back(s);
        }
    }
    if (samples.size() < params.minCells) {
        return Error{ErrorCode::Unsupported,
                     std::format("too few trusted flow cells for a lens rotation ({} of {} needed)", samples.size(),
                                 params.minCells)};
    }

    // ---- iteratively reweighted least squares ------------------------------
    // Pass 0 is ordinary least squares; then Huber passes pull the solution
    // off the outliers smoothly, and Tukey passes (redescending) drop them.
    const std::size_t n = samples.size();
    std::vector<double> weight(n, 1.0);
    std::vector<double> resid(n, 0.0);
    Vec3d w{};
    Normal normal;
    // Noise floor for the robust scale: 0.002 deg, far below anything the
    // flow resolves, so a perfect synthetic fit cannot divide by zero.
    const double sigmaFloor = deg2rad(0.002);
    const int passes = 1 + params.huberPasses + params.tukeyPasses;
    for (int pass = 0; pass < passes; ++pass) {
        normal = Normal{};
        for (std::size_t i = 0; i < n; ++i) {
            if (!(weight[i] > 0.0)) {
                continue;
            }
            normal.add(samples[i].east, samples[i].mEast, weight[i]);
            normal.add(samples[i].north, samples[i].mNorth, weight[i]);
        }
        if (!solve3(normal, w)) {
            return Error{ErrorCode::Unsupported, "the trusted flow cells cannot pin a lens rotation (singular fit)"};
        }
        // Residual magnitude per cell (both components together: a cell is
        // an outlier as a whole, not one of its components).
        for (std::size_t i = 0; i < n; ++i) {
            const double re = samples[i].mEast - w.dot(samples[i].east);
            const double rn = samples[i].mNorth - w.dot(samples[i].north);
            resid[i] = std::hypot(re, rn);
        }
        if (pass + 1 == passes) {
            break;  // the last solve's weights stand; the residuals are for the report
        }
        // Robust scale from the median residual.  For a 2-D Gaussian the
        // magnitude is Rayleigh distributed, whose median is sigma * 1.1774.
        const double sigma = std::max(medianOf(resid) / 1.1774, sigmaFloor);
        const bool huber = pass < params.huberPasses;
        for (std::size_t i = 0; i < n; ++i) {
            const double rho = resid[i];
            if (huber) {
                const double k = params.huberK * sigma;
                weight[i] = rho <= k ? 1.0 : k / rho;
            } else {
                const double c = params.tukeyC * sigma;
                const double u = rho / c;
                weight[i] = u < 1.0 ? (1.0 - u * u) * (1.0 - u * u) : 0.0;
            }
        }
    }

    // ---- quality of the final solve ------------------------------------------
    LensRotationFit fit;
    fit.wRad = w;
    fit.angleDeg = rad2deg(w.norm());
    fit.cells = static_cast<std::uint32_t>(n);
    double sumSq = 0.0;
    std::uint32_t inliers = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // Inliers are cells the final robust weights still count at half
        // strength or more.
        if (weight[i] > 0.5) {
            ++inliers;
            sumSq += resid[i] * resid[i];
        }
    }
    fit.inliers = inliers;
    fit.residualRmsDeg = inliers ? rad2deg(std::sqrt(sumSq / static_cast<double>(inliers))) : 0.0;
    // Conditioning of the weighted normal matrix, scale-free.
    if (normal.weight > 0.0) {
        std::array<double, 9> scaled = normal.a;
        for (double& v : scaled) {
            v /= normal.weight;
        }
        const std::array<double, 3> ev = symmetricEigenvalues(scaled);
        const double hi = std::max({ev[0], ev[1], ev[2]});
        const double lo = std::min({ev[0], ev[1], ev[2]});
        fit.conditioning = hi > 0.0 ? std::max(lo, 0.0) / hi : 0.0;
    }

    // ---- refuse what is not a calibration residual -----------------------------
    if (!fit.wRad.isFinite()) {
        return Error{ErrorCode::Unsupported, "the lens rotation fit did not converge"};
    }
    if (fit.conditioning < params.minConditioning) {
        return Error{ErrorCode::Unsupported,
                     std::format("the trusted flow cells span too little of the ring to fix all three rotation axes "
                                 "(conditioning {:.4f} < {:.4f})",
                                 fit.conditioning, params.minConditioning)};
    }
    if (static_cast<double>(inliers) < params.minInlierFraction * static_cast<double>(n) ||
        inliers < params.minCells / 2u) {
        return Error{ErrorCode::Unsupported,
                     std::format("too few cells agree on one lens rotation ({} of {})", inliers, n)};
    }
    if (fit.residualRmsDeg > params.maxResidualDeg) {
        return Error{ErrorCode::Unsupported,
                     std::format("the cells that agree on a lens rotation still disagree by {:.3f} deg RMS (> {:.3f})",
                                 fit.residualRmsDeg, params.maxResidualDeg)};
    }
    if (fit.angleDeg > params.maxAngleDeg) {
        return Error{ErrorCode::Unsupported,
                     std::format("a {:.2f} deg lens rotation is not a calibration residual (> {:.2f})", fit.angleDeg,
                                 params.maxAngleDeg)};
    }
    return fit;
}

// ---------------------------------------------------------------------------
//  Several frames
// ---------------------------------------------------------------------------
Result<LensRotationFit> combineLensRotations(const std::vector<LensRotationFit>& fits,
                                             const LensRotationParams& params) {
    OSV_TRY(checkParams(params));
    // Only finite fits take part; a non-finite one is a caller bug, not data.
    std::vector<const LensRotationFit*> usable;
    for (const LensRotationFit& f : fits) {
        if (f.wRad.isFinite() && std::isfinite(f.residualRmsDeg)) {
            usable.push_back(&f);
        }
    }
    if (usable.size() < std::max<std::uint32_t>(params.minFits, 1u)) {
        return Error{ErrorCode::Unsupported, std::format("only {} frame(s) gave a lens rotation ({} needed)",
                                                         usable.size(), std::max<std::uint32_t>(params.minFits, 1u))};
    }

    // ---- component-wise median ---------------------------------------------
    std::vector<double> xs, ys, zs, res;
    for (const LensRotationFit* f : usable) {
        xs.push_back(f->wRad.x);
        ys.push_back(f->wRad.y);
        zs.push_back(f->wRad.z);
        res.push_back(f->residualRmsDeg);
    }
    LensRotationFit out;
    out.wRad = Vec3d{medianOf(xs), medianOf(ys), medianOf(zs)};
    out.angleDeg = rad2deg(out.wRad.norm());
    out.residualRmsDeg = medianOf(res);
    out.fits = static_cast<std::uint32_t>(usable.size());
    out.conditioning = std::numeric_limits<double>::max();
    for (const LensRotationFit* f : usable) {
        out.cells += f->cells;
        out.inliers += f->inliers;
        out.conditioning = std::min(out.conditioning, f->conditioning);
        // The spread: how far each frame's rotation lies from the median, as
        // the angle of the rotation that separates them (small-angle exact).
        out.spreadDeg = std::max(out.spreadDeg, rad2deg((f->wRad - out.wRad).norm()));
    }

    // ---- agreement: one RIGID rotation, or none ----------------------------------
    if (out.spreadDeg > params.agreeDeg) {
        return Error{ErrorCode::Unsupported,
                     std::format("the frames disagree about the lens rotation by up to {:.3f} deg (> {:.3f}); "
                                 "keeping the calibration",
                                 out.spreadDeg, params.agreeDeg)};
    }
    return out;
}

// ---------------------------------------------------------------------------
//  The fold
// ---------------------------------------------------------------------------
Mat3d rotationFromVector(const Vec3d& wRad) noexcept {
    const double angle = wRad.norm();
    if (!(angle > 0.0) || !std::isfinite(angle)) {
        return Mat3d::identity();
    }
    return Mat3d::axisAngle(wRad / angle, angle);
}

Status applyLensRotation(geom::LensRig& rig, const Vec3d& wRad) {
    if (!wRad.isFinite()) {
        return failStatus(ErrorCode::InvalidArgument, "applyLensRotation: non-finite rotation");
    }
    // Ten degrees is far beyond any calibration residual; refusing it keeps a
    // corrupt cache line or a caller bug from turning the stitch inside out.
    if (rad2deg(wRad.norm()) > 10.0) {
        return failStatus(ErrorCode::InvalidArgument, "applyLensRotation: rotation beyond 10 degrees");
    }
    // Half into each lens, in opposite senses (LensAlign.h, THE FOLD).
    const Mat3d plusHalf = rotationFromVector(wRad * 0.5);
    const Mat3d minusHalf = rotationFromVector(wRad * -0.5);
    rig.bodyToLens[geom::kMasterLens] = rig.bodyToLens[geom::kMasterLens] * plusHalf;
    rig.bodyToLens[geom::kSlaveLens] = rig.bodyToLens[geom::kSlaveLens] * minusHalf;
    return okStatus();
}

std::string describeLensRotation(const LensRotationFit& fit) {
    const Vec3d axis = fit.wRad.normalized();
    std::string text = std::format("{:.3f} deg about ({:+.2f}, {:+.2f}, {:+.2f}), residual {:.3f} deg RMS, {} / {} "
                                   "cells",
                                   fit.angleDeg, axis.x, axis.y, axis.z, fit.residualRmsDeg, fit.inliers, fit.cells);
    if (fit.fits > 1) {
        text += std::format(", {} frames within {:.3f} deg", fit.fits, fit.spreadDeg);
    }
    return text;
}

}  // namespace osv::render
