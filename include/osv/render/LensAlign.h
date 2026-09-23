// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensAlign.h - the per-clip relative rotation between the two lenses: fit
// it from the parallax flow the importer already measures, and fold it into
// the rig.
//
// ===========================================================================
//  WHY
// ===========================================================================
// The calibration the camera records stores each lens's orientation, and on
// the sample clip the two orientations disagree with the footage by one small
// rigid rotation: 0.35-0.38 degrees, the same from three independent flows
// (per-pixel DIS, the production DIS grid, SEA-RAFT) and on frames 0, 32 and
// 64 to within 0.008 degrees (docs/research/AI_STITCHING.md, section 3.3).
// Uncorrected it leaves the far-field ground at an overlap NCC of 0.37; three
// numbers per clip take it to 0.89 with no flow grid at all, fix the sky -
// where no flow can measure anything - and, being one constant per clip,
// cannot flicker.  The parallax grid then only has to carry true parallax.
//
// ===========================================================================
//  THE MODEL
// ===========================================================================
// In the polar-axis band (lens axes at the poles, the body frame's own
// lon = atan2(x, z), lat = asin(y)), a small rotation w between the lenses
// displaces every direction d by w x d.  Projected on the local east / north
// unit vectors (e_lon, e_lat) that is, since (d, e_lon, e_lat) is a
// right-handed orthonormal frame (d x e_lon = e_lat, d x e_lat = -e_lon):
//
//     east  (true angle)  = (w x d) . e_lon =  w . e_lat
//     north (true angle)  = (w x d) . e_lat = -w . e_lon
//
// Linear in w: three unknowns, two equations per grid cell.  The flow the
// grid measures from lens 0 to lens 1 is the rotation plus the parallax of
// near content plus noise; near content is a minority of the ring and is
// removed as outliers by a robust fit (least squares, Huber, then Tukey's
// biweight), so nothing has to know where the wing is.
//
// ===========================================================================
//  THE FOLD
// ===========================================================================
// The kernel samples the master (lens 1) at the output direction displaced by
// +g and the slave (lens 0) by -g, where g is HALF the measured lens 0 -> lens 1
// disparity (ParallaxWarp.h).  A disparity that is the rotation field of w is
// therefore removed by sampling the master at R(+w/2) d and the slave at
// R(-w/2) d - i.e. by
//
//     bodyToLens[master] <- bodyToLens[master] * R(+w/2)
//     bodyToLens[slave]  <- bodyToLens[slave]  * R(-w/2)
//
// Splitting it half and half leaves the stitched world orientation - and so
// stabilisation and the horizon - where it was.  No kernel, ABI or table
// change: every renderer (CPU, CUDA, OpenCL, the direct path) gets it through
// the rig.
#pragma once

#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/geom/LensRig.h"
#include "osv/render/ParallaxWarp.h"

#include <cstdint>
#include <string>
#include <vector>

namespace osv::render {

/// The parallax grid's benefit gate (ParallaxWarpParams::requiredImprovement)
/// on a rig that carries a fitted lens rotation.
///
/// The default 0.2 was tuned on bands that still held the 0.36 deg rotation,
/// where every textured cell had a large residual for the grid to remove.
/// With the rotation folded that systematic part is gone, the residual left
/// per cell is small, and a 20 % bar switches off corrections that are right:
/// the ground falls to NCC 0.892-0.904 against 0.922-0.932 today.  Measured
/// on the sample (frames 0 / 32 / 64, rendered through the kernel):
///
///     gate    ground               wing                 whole band
///     0.20    0.892 0.892 0.904    0.307 0.293 0.309    0.913 0.913 0.915
///     0.10    0.916 0.922 0.930    0.305 0.317 0.341    0.918 0.921 0.921
///     0.05    0.932 0.942 0.946    0.324 0.323 0.339    0.923 0.924 0.925   <- this
///     0.03    0.937 0.948 0.950    0.338 0.303 0.334    0.924 0.924 0.926
///     today   0.922 0.932 0.932    0.310 0.288 0.332    0.916 0.918 0.921   (no rotation, 0.20)
///
/// 0.05 beats today in every region on every frame; below it the wing starts
/// to lose (0.303 at 0.03), which is the harm the gate exists to prevent.
/// A rig WITHOUT a folded rotation keeps the default, so every project that
/// does not use Lens Alignment renders exactly as before.
inline constexpr double kAlignedRequiredImprovement = 0.05;

/// Tuning of the rotation fit.  The defaults are the measured ones.
struct LensRotationParams {
    /// A cell counts only with at least this many consistent flow pixels:
    /// fewer is a handful of matches whose mean is mostly noise.
    std::uint32_t minPixels = 12;
    /// ... and only where the benefit gate trusted it (ParallaxCellStats::gate):
    /// the gate is what tells a real correspondence (textured ground) from a
    /// self-consistent drift (DIS on open sky moves 2-4 degrees there).
    double minGate = 0.5;
    /// Fewer usable cells than this is not a fit (256 x 32 grid: 8192 cells).
    std::uint32_t minCells = 48;
    /// Of the usable cells, at least this fraction must end up inliers.
    double minInlierFraction = 0.2;
    /// Refuse a fit whose inliers still disagree by more than this, RMS of
    /// the full disparity (degrees): the model does not describe the clip.
    double maxResidualDeg = 0.1;
    /// Refuse a rotation larger than this (degrees): a calibration residual
    /// is a fraction of a degree, a fit of several degrees means the flow
    /// locked onto something else (a moving object, a mismatch).
    double maxAngleDeg = 2.0;
    /// Refuse when the smallest eigenvalue of the normal matrix is below
    /// this fraction of the largest: the usable cells span too little of the
    /// ring to pin all three components (e.g. only one short arc of ground).
    double minConditioning = 0.02;
    /// Robust passes: Huber (k = huberK sigma), then Tukey (c = tukeyC sigma).
    int huberPasses = 2;
    double huberK = 2.0;
    int tukeyPasses = 3;
    double tukeyC = 4.685;
    /// combineLensRotations: the per-frame fits must agree to within this
    /// (degrees, angle between each fit and their median).
    double agreeDeg = 0.05;
    /// combineLensRotations: at least this many accepted, agreeing fits.
    std::uint32_t minFits = 2;
};

/// One fitted rotation.
struct LensRotationFit {
    /// The rotation vector (axis * angle, radians) in the BODY frame such that
    /// the lens 0 -> lens 1 disparity is w x d.  applyLensRotation() folds it.
    Vec3d wRad;
    double angleDeg = 0.0;        ///< |w| in degrees.
    double residualRmsDeg = 0.0;  ///< RMS of the inliers' residual (full disparity, degrees).
    std::uint32_t cells = 0;      ///< Usable cells offered to the fit.
    std::uint32_t inliers = 0;    ///< Cells the final robust weights kept (weight > 0.5).
    double conditioning = 0.0;    ///< Smallest / largest eigenvalue of the weighted normal matrix.
    std::uint32_t fits = 1;       ///< Frames combined (combineLensRotations), 1 for a single fit.
    double spreadDeg = 0.0;       ///< Largest angle between a frame's fit and the result (combined only).
};

/// Fit the rotation to one grid's raw cells (see the header for the model).
///
/// Unsupported - a normal outcome, e.g. a clip of open sky - when there are
/// too few usable cells, the fit is ill-conditioned, too few cells agree
/// with it, the inliers' residual is too large, or the angle is implausible.
/// InvalidArgument for malformed stats or parameters.
[[nodiscard]] Result<LensRotationFit> fitLensRotation(const ParallaxCellStats& cells,
                                                      const LensRotationParams& params = {});

/// Combine per-frame fits into the clip's rotation: the component-wise
/// median of the accepted fits, refused unless at least params.minFits of them
/// exist and every one lies within params.agreeDeg of the median (a clip
/// whose frames disagree about a RIGID rotation has something else going on,
/// and a wrong constant correction is worse than none).
[[nodiscard]] Result<LensRotationFit> combineLensRotations(const std::vector<LensRotationFit>& fits,
                                                           const LensRotationParams& params = {});

/// Rotation matrix of a rotation vector (axis * angle, radians), Rodrigues.
/// Identity for a zero or non-finite vector.
[[nodiscard]] Mat3d rotationFromVector(const Vec3d& wRad) noexcept;

/// Fold a fitted rotation into `rig`, half into each lens (see the header).
/// InvalidArgument for a non-finite vector or one beyond 10 degrees (never a
/// calibration residual); the rig is untouched then.
[[nodiscard]] Status applyLensRotation(geom::LensRig& rig, const Vec3d& wRad);

/// "0.372 deg about (+0.23, +0.05, -0.97), residual 0.041 deg, 1650 / 2210
/// cells" - the log line's words for a fit.
[[nodiscard]] std::string describeLensRotation(const LensRotationFit& fit);

}  // namespace osv::render
