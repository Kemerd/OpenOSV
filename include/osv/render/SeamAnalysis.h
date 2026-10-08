// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Seam analysis on the polar-axis equirect band where both lenses overlap:
//   * overlapNcc   - the regression metric (how well the two lenses agree)
//   * searchSeam   - per-column 1-D disparity search that feeds the kernel's
//                    seam table (parallax correction for near objects)
//   * estimateGain - exposure / white balance matching between the lenses
//
// All three render each lens alone into a small polar-axis equirect band with
// the CPU reference renderer, so they are backend independent and exercise
// exactly the mapping the final render uses.
//
// Frames that live in VRAM (a keepOnDevice decode, RenderJob::planesOnDevice)
// are shaded by the installed DeviceBandShader instead - the same shared
// osvShadePixelW on the GPU (see DeviceBandShader.h, installCudaAnalyses()).
// Without one installed, such frames are refused with InvalidArgument rather
// than read as host memory.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace osv::render {

struct LensShadingModel;  // [WP-VIGNETTE] osv/render/LensShading.h

/// Band geometry shared by the three analyses.
struct BandParams {
    std::uint32_t equirectW = 2048;  ///< Width of the polar-axis map (columns = longitude).
    double bandHalfDeg = 6.0;        ///< Half height of the analysed band around the seam (deg).
};

/// Per-lens luma band images (code space) plus coverage.
struct LensBands {
    std::uint32_t w = 0;         ///< Band width (columns).
    std::uint32_t h = 0;         ///< Band height (rows).
    std::uint32_t rowOffset = 0; ///< First equirect row of the band.
    std::uint32_t mapH = 0;      ///< Full equirect height (for row -> degree).
    std::vector<float> luma[2];  ///< Luma per lens, row-major w*h.
    std::vector<float> alpha[2]; ///< Coverage per lens.
};

/// A 2-D parallax warp grid as the band renderer needs to see it.
///
/// Deliberately a plain view rather than a ParallaxWarpGrid: SeamAnalysis is
/// the LOWER layer (ParallaxWarp.h includes it, not the other way round), so
/// taking the full type here would be a circular dependency.  Pointing at the
/// caller's data also keeps the measurement path free of a copy of a grid it
/// only reads.
struct WarpGridView {
    const float* uv = nullptr;  ///< Interleaved (u, v) radians, w * h pairs.
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    float latMinRad = 0.0f;     ///< Latitude of row 0.
    float latMaxRad = 0.0f;     ///< Latitude of row h - 1.

    [[nodiscard]] bool valid() const noexcept { return uv != nullptr && w > 0 && h > 1; }
};

/// Render the two per-lens luma bands.  `linear` selects scene-linear values
/// (gain estimation) instead of D-Log M codes (matching).
///
/// `warp` (optional) applies a 2-D parallax grid while rendering, which is
/// what makes an after-correction NCC measure the same geometry the real
/// render path produces rather than an approximation of it.
Result<LensBands> renderLensBands(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, bool linear,
                                  const std::vector<float>* seamTable, ThreadPool& pool,
                                  const WarpGridView* warp = nullptr);

/// Normalised cross-correlation of the two lenses over the co-visible band.
/// Returns a value in [-1, 1]; 0 when nothing is co-visible.
Result<double> overlapNcc(const geom::LensRig& rig, const video::FramePair& frames, const geom::BlendParams& blend,
                          const BandParams& band, ThreadPool& pool, const std::vector<float>* seamTable = nullptr,
                          const WarpGridView* warp = nullptr);

// ===========================================================================
//  The seam-shift table: a per-column search, then a robust smoother
// ===========================================================================
//
// searchSeam scans, for every band column, a +/- maxShiftPx row shift of
// lens 1 against lens 0 and keeps the best normalised cross-correlation.
// That raw measurement is only trustworthy where the column has something to
// lock onto, so each column also gets a CONFIDENCE in [0, 1]:
//
//     w = sstep(confNccLo, confNccHi, best NCC)                 the match itself
//       * sstep(confTextureLo, confTextureHi, texture)          something to match
//       * sstep(confDistinctLo, confDistinctHi, best - second)  one clear peak
//       * sstep(confCoverageLo, confCoverageHi, coverage)       enough of a window
//
// with sstep the smoothstep, "texture" the mean |d luma / d row| over the
// matching window (after a 1-row Gaussian, the smaller of the two lenses,
// covered pixels only), "second" the best score more than
// kSeamDistinctGapRows rows away from the peak (the peak-ratio confidence of
// Hu & Mordohai, TPAMI 2012) and "coverage" the co-visible fraction of the
// unshifted window.  A smooth sky ramp correlates at almost any shift, so it
// scores a high NCC but no texture and no distinct peak: w = 0.
//
// Co-visibility is part of the measurement.  A column whose UNSHIFTED window
// holds fewer than unmeasuredFraction co-visible pixels is UNMEASURED and
// not scored at all: on a car-mounted clip the calibration's occlusion
// polygons leave a ~98 deg arc of the seam ring with no overlap, where the
// only shifts that correlate pair different content across the polygon gap.
// In a measured column a shift is scored only when it keeps at least
// minCovalidFraction of the column's unshifted co-visible pixels, and a
// column that rests on a thin overlap (the maintainer's sample has 14-19
// co-visible rows of 68 along its selfie-stick arc) earns little weight.
//
// The table is then the solution of a confidence-weighted, robust Whittaker
// smoother on the closed ring (Eilers, Anal. Chem. 2003; Huber IRLS):
//
//     (W + l2 D2'D2 + l1 D1'D1 + M) T = W m + M p
//
// with m the measured (parabolic-refined) shift, W = diag(w), D1 / D2 the
// first and second differences along longitude, M = diag(priorWeight) and
// p the prior (0, or an optional caller table).  Unmeasured columns more
// than unmeasuredInheritDeg from a measured one are pinned to 0 exactly (see
// unmeasuredPriorWeight), so nothing is ever interpolated across the arc;
// the few inside that margin inherit a measured neighbour's value and fade
// to the pin.  The kernel applies the table out to 12 deg from the seam, so
// any noise along the seam bends a line that runs parallel to it; the
// smoother is what keeps such lines straight.

/// The second correlation peak counts only this many rows or more away from
/// the best one (|s - s*| > 3): closer shifts are the same peak's shoulder.
inline constexpr int kSeamDistinctGapRows = 3;

struct SeamSearchParams {
    BandParams band;
    int maxShiftPx = 24;          ///< Search range in band rows (+/-).
    int windowHalfCols = 4;       ///< Half width of the matching window (columns).
    /// Columns whose best correlation stays below this are not counted as
    /// accepted (SeamProfile::acceptedColumns / meanNcc) and carry no weight.
    double minNcc = 0.5;

    // ---- per-column confidence (smoothstep ramps, see above) -------------------
    // Measured on a car-mounted day clip (LRF 5872-5920): only 21.7 % of the
    // columns the old NCC >= 0.5 test accepted have w > 0.5, and 43 % of all
    // columns were accepted with w < 0.1 - featureless sky with random peaks.
    double confNccLo = 0.60;        ///< Best NCC giving w = 0.
    double confNccHi = 0.85;        ///< Best NCC giving full weight.
    double confTextureLo = 0.8e-3;  ///< Mean |d luma / d row| (D-Log M codes per row) giving w = 0.
    double confTextureHi = 2.5e-3;  ///< ... giving full weight.
    double confDistinctLo = 0.03;   ///< Best minus second peak giving w = 0.
    double confDistinctHi = 0.10;   ///< ... giving full weight.
    /// Co-visible fraction of the unshifted window ((2 windowHalfCols + 1) x
    /// matched rows) giving w = 0, and full weight.  A match on a quarter of
    /// its window rests on a quarter of the evidence, and its candidate
    /// shifts are cut short on one side.  Measured on the maintainer's
    /// sample (overlap NCC after the table, frame 0): without this ramp a
    /// handful of such columns along the selfie-stick arc set the table to
    /// -3 deg over 300 columns and the NCC fell from 0.897 to 0.842.
    double confCoverageLo = 0.25;
    double confCoverageHi = 0.50;

    // ---- co-visibility -----------------------------------------------------------
    /// In a measured column, a shift is scored only when its window keeps at
    /// least this fraction of the co-visible pixels the column has unshifted.
    /// Relative to the column's own support rather than the full window: a
    /// thin-overlap column would otherwise lose the unshifted match itself
    /// and keep only shifts that slide one lens's covered rows over the
    /// other's - a one-sided candidate set, measured at -2.8 deg or below on
    /// every such column of the sample.
    double minCovalidFraction = 0.5;
    /// A column whose unshifted window has fewer co-visible pixels than this
    /// fraction of it is unmeasured: not scored, weight 0, and 0 in the table
    /// (beyond unmeasuredInheritDeg).
    double unmeasuredFraction = 0.25;
    /// Bounded inheritance: an unmeasured column within this many degrees of
    /// a measured one is not pinned but left free (weight 0, the plain prior
    /// weight), so the smoother carries a near object's measured value a
    /// short way into the arc and bends to 0 there - instead of bending the
    /// measured side down to a pin at the arc's edge.  32 band columns of
    /// 2048.  Measured on the car clip's LRF 5872-5920 (band NCC after the
    /// table, mean): pinning at the edge 0.9880, 16 columns 0.9910, 32
    /// columns 0.9919 (the old table: 0.9898); the hood beside the arc
    /// dropped from 0.969 to 0.934 with the pin at the edge.
    double unmeasuredInheritDeg = 5.625;

    // ---- the robust smoother ---------------------------------------------------
    /// Second-difference penalty at 2048 columns (scaled by (columns/2048)^4,
    /// so the smoothness in degrees does not depend on the band width).
    double smoothLambda2 = 2e4;
    /// First-difference penalty at 2048 columns (scaled by (columns/2048)^2).
    double smoothLambda1 = 2.0;
    /// Pull of a measured column towards the prior: a featureless stretch of
    /// the seam settles on the prior instead of drifting.
    double priorWeight = 1e-3;
    /// Pull of an UNMEASURED column (beyond unmeasuredInheritDeg) towards 0.
    /// +infinity (the default) pins it there exactly; a finite weight is a
    /// soft pull, which lets the inherited value leak on into the arc, so it
    /// exists for comparison only (test_seam.cpp measures the leak).
    double unmeasuredPriorWeight = std::numeric_limits<double>::infinity();
    double huberDeg = 0.3;    ///< Huber threshold of the IRLS reweighting (degrees).
    int irlsIterations = 3;   ///< Solves (the first unweighted by Huber, then reweighted).
};

struct SeamProfile {
    std::uint32_t columns = 0;
    std::vector<float> shiftDeg;    ///< Disparity per column in degrees (kernel seam table).
    std::vector<float> ncc;         ///< Best correlation per column (0 where no shift could be scored).
    /// Per-column confidence of the measurement, 0..1 (0: unmeasured, or
    /// nothing to trust).  Carried with the table so a glide between two
    /// buckets can tell a real change from noise (render::blendSeamTables).
    std::vector<float> confidence;
    /// The raw per-column measurement (parabolic-refined, degrees) the
    /// smoother was fed; NaN where no shift could be scored.  Diagnostics.
    std::vector<float> measuredDeg;
    double meanNcc = 0.0;               ///< Mean best NCC of the accepted columns.
    std::uint32_t acceptedColumns = 0;  ///< Measured columns with best NCC >= minNcc.
    /// Columns without co-visible pixels (0 in the table beyond the
    /// unmeasuredInheritDeg margin).
    std::uint32_t unmeasuredColumns = 0;
    std::uint32_t confidentColumns = 0;   ///< Columns with confidence >= 0.5.
    double meanConfidence = 0.0;          ///< Mean confidence over all columns.
};

/// Per-column disparity search.  Positive values mean the same feature sits
/// farther from both lens axes than the calibration predicts (near object).
///
/// `seamPrior` (optional) is the table a featureless measured column settles
/// on instead of 0 - e.g. a clip-steady table.  It must have one entry per
/// band column; a non-finite entry means "no prior here" (0).  Unmeasured
/// columns ignore it (their prior is always 0).  A prior of the wrong size is
/// ignored with a warning.
Result<SeamProfile> searchSeam(const geom::LensRig& rig, const video::FramePair& frames,
                               const geom::BlendParams& blend, const SeamSearchParams& params, ThreadPool& pool,
                               const std::vector<float>* seamPrior = nullptr);

/// The search and the smoother on bands already rendered (searchSeam renders
/// them first).  `bands` must hold the search band: maxShiftPx rows of
/// padding above and below the matched rows.  Split out so the estimator can
/// be tested on synthetic bands with no clip, decoder or renderer involved.
/// `pool` (optional) splits the per-column search; the result is identical
/// with or without it.
Result<SeamProfile> searchSeamFromBands(const LensBands& bands, const SeamSearchParams& params,
                                        ThreadPool* pool = nullptr, const std::vector<float>* seamPrior = nullptr);

struct GainEstimate {
    Vec3d gain[2] = {Vec3d{1, 1, 1}, Vec3d{1, 1, 1}};  ///< Per-lens linear gains (slave, master).
    Vec3d overlapMean[2] = {};                          ///< Mean linear RGB per lens over the trusted pixels.
    std::uint64_t samples = 0;                          ///< Trusted co-visible pixels used.
};

/// Symmetric per-channel gains g0 = sqrt(m1/m0), g1 = 1/g0 (clamped to
/// [0.5, 2]) that make the two lenses agree in the overlap band.
///
/// The means m0, m1 come from TRUSTED pixels only: both lenses' production
/// weight (FOV feather x occlusion ramp, `blend`) must be >= 0.99.  A lens's
/// darkened rim therefore never enters the statistics - measured on the
/// sample clip, lens 0 is 1 stop dark at 95 deg while the old alpha > 0.5
/// rule still counted it, which the estimate read as "lens 1 is too bright"
/// (docs/research/NEURAL_STITCHING.md, sections 1.2 and 1.4).
///
/// [WP-VIGNETTE] `shading`, when given and active, is applied to both lenses
/// exactly as the kernel applies it (LensShading.h), so the gains match the
/// lenses the blend will see; null (the default) is the raw lenses, bit for
/// bit as before.
Result<GainEstimate> estimateGain(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, ThreadPool& pool,
                                  const LensShadingModel* shading = nullptr);

}  // namespace osv::render
