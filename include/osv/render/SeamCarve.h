// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SeamCarve.h - a seam CARVED through the overlap band by dynamic
// programming, and the narrow blend that follows it.  [WP-SEAM]
//
// ===========================================================================
//  WHY
// ===========================================================================
// The kernel's default blend is a field-of-view feather: each lens fades out
// over the last `featherDeg` (4) degrees before its thetaMax, so across the
// middle of the overlap both lenses sit at 50 %.  That is harmless where the
// two lenses agree and disastrous where they do not.  An object a few
// centimetres from the camera - the wing-tip fin on the sample clip - is seen
// from two viewpoints ~3 cm apart, so each lens sees a different side of it
// and no warp can bring the two pictures into agreement (the parallax grid's
// benefit gate switches itself off there; DIS and learned flow both leave the
// local NCC at ~0.3).  Averaging the two shows BOTH: the doubled fin, the
// ghosted propeller.
//
// A seam fixes that the way production stitchers do: at any direction use
// ONE lens, put the boundary where the two lenses agree, and mix them only
// inside a narrow feather along that boundary.
//
// ===========================================================================
//  HOW (published methods, reimplemented)
// ===========================================================================
// 1. The two lenses are rendered into the polar-axis overlap band (lens axes
//    at the poles, the geometric seam on the equator) - the same LensBands
//    every other analysis uses - and seen through the correction the kernel
//    will apply (2-D parallax grid or 1-D seam table), in band space, exactly
//    as ParallaxWarp's benefit gate does.
// 2. A cost per (row, column) of putting the seam there - the optimal-seam
//    formulation of Avidan & Shamir (SIGGRAPH 2007) as used for stitching
//    (Kwatra et al. 2003; Perazzi et al., EG 2015):
//      * disagreement of the two lenses inside the feather window;
//      * coverage: a lens may not be used where it cannot see (the selfie
//        stick's occlusion, past its field of view), so the stick mask is an
//        INPUT to seam placement and the seam routes around it, with a
//        prohibitive cost for a feather that would reach a blind lens;
//      * a pull toward the geometric seam (weight 0.15, as in the reference
//        implementation);
//      * when a neighbouring bucket's seam is known, a temporal hold that is
//        relaxed where the content is close to the camera (it may need to
//        move), plus a hard clamp on how far the seam may move;
//      * optional per-lens penalties from installed hooks - a flare / ghost
//        detector (WP-FLARE) or a usable-rim limit (WP-PHOTO) makes a lens
//        expensive to SHOW where it is unreliable (setSeamPenaltyHook).
// 3. A DP over longitude, one seam row per column, bounded step between
//    neighbouring columns, on a CLOSED ring (the seam has no ends).
// 4. Per column, a feather width from the structural disagreement left along
//    the seam: wide where the lenses agree (it hides the colour difference
//    between them), narrow where they do not (so the two copies of a near
//    object are never mixed), never reaching a lens that cannot see.
//
// The result is a small table - (latitude, half width) per longitude column,
// 8 KB at 1024 columns - that the kernel samples per ray (osv_kernel.h,
// osvBlendSeamApply).  It is measured once per analysis bucket, cached, and
// glided between buckets like the parallax grid (ImporterInstance).
//
// ===========================================================================
//  WHAT WAS MEASURED (sample clip, frames 0 / 32 / 64, osv_seam_carve_bench)
// ===========================================================================
// Over the wing-tip longitudes, x1000 display luma: "ghost" = disagreeing
// content shown at partial strength (the doubled fin), "edge" = gradient in
// the blend that neither lens has (a visible cut).
//
//     feather (before)          ghost 76-77    edge 0.007-0.009
//     narrow, geometric seam    ghost 7.7-7.9  edge 0.50-0.54
//     carved (this)             ghost 5.8-6.3  edge 0.27-0.34
//
// Two things were tried and dropped on the numbers:
//   * a 2-band blend (Burt & Adelson 1983) with a small per-pixel low-pass
//     footprint (9 bilinear taps, sigma ~1.5 lens px - the reference
//     implementation's kernel): at a 0.6 degree low band it lands on the
//     same ghost/edge trade-off as simply widening the single band (ghost
//     8.7 / edge 0.22 against 8.1 / 0.24 at a 0.5 degree feather), at 1.5
//     degrees it brings a faint second fin back (ghost 24), and it cost
//     +7 % kernel time and a second decode path in the shader.  A low band
//     that excludes a fin-sized object needs a footprint of ~1 degree (~12
//     lens px), i.e. a pre-filtered pyramid, not a per-pixel footprint.
//   * a "hidden content" term (penalise hiding what only one lens shows,
//     so a near object's two copies are not both cut away): on the sample
//     it pulled the seam over a second, different structure seen by one
//     lens only and DOUBLED the fin it was meant to protect (edge 1.26-1.36).
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Extra per-lens cost (flare / ghost detector, usable-rim limit, ...)
// ===========================================================================

/// Per-lens penalty maps over the analysis band.
///
/// Called once per carve with the bands exactly as the seam sees them (after
/// the parallax correction).  The hook fills `penaltySlave` / `penaltyMaster`
/// - each sized by the caller to warped.w * warped.h, zero-filled, row-major
/// - with values >= 0: the cost of SHOWING that lens at that band pixel.  A
/// flare ghost present in one lens only makes that lens expensive where the
/// ghost is; a lens past its usable rim is expensive there.  The seam then
/// puts the other lens on that side.  Return false to contribute nothing
/// this time.  Must not throw; may run on a background thread; must not
/// resize the maps.
///
/// Scale: the seam's own disagreement cost is in luma code-value
/// differences (0..1) averaged over the feather window, and the penalty is SUMMED over
/// the rows a lens would be shown on, so a penalty of ~0.05 per row over a
/// few degrees of latitude is already decisive.  See bandPixelDirection()
/// for the geometry of a band pixel.
using SeamLensPenaltyFn = bool (*)(const LensBands& warped, std::vector<float>& penaltySlave,
                                   std::vector<float>& penaltyMaster, void* user) noexcept;

/// A penalty hook plus its weight in the seam cost.
struct SeamPenaltyHook {
    SeamLensPenaltyFn fn = nullptr;  ///< The hook, or null for none.
    void* user = nullptr;            ///< Passed back to `fn` untouched.
    double weight = 1.0;             ///< Scale of the penalty sums in the seam cost (>= 0).

    [[nodiscard]] bool installed() const noexcept { return fn != nullptr; }
};

/// Independent process-wide hook slots, so separate packages can each
/// install theirs without replacing the other's.  Every installed slot
/// contributes to every carve (plus SeamCarveParams::penalty, if set).
enum class SeamPenaltySlot : std::uint8_t {
    Flare = 0,  ///< Flare / sun-ghost detector: a ghost present in one lens only.
    Rim = 1,    ///< Usable-rim limit: pixels past a lens's reliable field of view.
    Extra = 2,  ///< Anything else.
    Count
};

/// Install (or, with a default-constructed hook, remove) the hook of one
/// slot.  Thread-safe; takes effect for carves that start afterwards.
void setSeamPenaltyHook(SeamPenaltySlot slot, const SeamPenaltyHook& hook) noexcept;

/// The hook installed in `slot` (empty when none).  Thread-safe.
[[nodiscard]] SeamPenaltyHook seamPenaltyHook(SeamPenaltySlot slot) noexcept;

/// Body-frame unit direction of the CENTRE of band pixel (col, row): the
/// band is a stretch of the polar-axis equirect whose poles are the lens
/// axes, so lon = (col + 0.5) 2pi / w - pi and lat = pi/2 - (rowOffset + row
/// + 0.5) pi / mapH, and d = (cos lat sin lon, sin lat, cos lat cos lon).
/// A hook that needs a lens angle multiplies this by the rig's bodyToLens
/// rotation (theta = acos of the z component); for a quick estimate, the
/// master lens's theta is ~pi/2 - lat and the slave's ~pi/2 + lat.  Returns
/// false (and zeros) for an empty band.
bool bandPixelDirection(const LensBands& bands, double col, double row, double d[3]) noexcept;

// ===========================================================================
//  Inputs and outputs
// ===========================================================================

/// The correction the kernel will apply, which the seam must see the bands
/// through.  Both optional; with both null the bands are used as rendered.
struct SeamCorrection {
    const WarpGridView* warp = nullptr;                ///< 2-D parallax grid in force (half corrections).
    const std::vector<float>* seamShiftDeg = nullptr;  ///< 1-D seam table in force (full disparity, degrees).
};

/// Tuning of the carve.  Weights are dimensionless: the band luma is in code
/// values on a 0..1 scale.  The defaults are the measured ones (see above).
struct SeamCarveParams {
    /// Longitude columns of the DP and of the output table.  The band width
    /// must be a whole multiple (2048-column bands -> 2 band columns per
    /// seam column): 1024 columns is 0.35 degrees each, fine enough to route
    /// around a fin, coarse enough that the DP sees each column's structure
    /// rather than its noise.
    std::uint32_t columns = 1024;

    // ---- feather ----------------------------------------------------------
    /// Feather half width where the lenses disagree (degrees): two band rows,
    /// enough to hide the pixel step of a hard cut, far too little for a
    /// second copy of anything to show through.  0 is a hard cut.
    /// [WP-SEAMTOOLS] "Parallax Blend" in Source Settings.
    double narrowHalfWidthDeg = 0.35;
    /// Feather half width where they agree (degrees): wide enough to hide the
    /// residual colour / vignetting difference between the lenses.
    /// [WP-SEAMTOOLS] "Seam Blend" in Source Settings.
    double wideHalfWidthDeg = 1.5;
    /// [WP-SEAMTOOLS] Half width (degrees) of the window the seam's cost and
    /// each column's structural disagreement are measured over.  It was the
    /// narrow feather until the feather widths became user controls; kept
    /// separate so those controls change how WIDE the lenses mix and never
    /// WHERE the seam runs.  The default is the narrow feather's, so a carve
    /// with default parameters is bit-identical to the one before the split.
    double costWindowDeg = 0.35;
    /// Structural disagreement (mean gradient difference over the feather
    /// window, code values per band pixel) at or below which a column gets
    /// the wide feather, and at or above which it gets the narrow one.
    double agreeResidual = 0.004;
    double disagreeResidual = 0.012;
    /// Kernel: validity ramp below each lens's thetaMax (degrees), which
    /// replaces the FOV feather wherever the carved seam decides the mix.
    double edgeRampDeg = 0.5;

    // ---- cost ------------------------------------------------------------------
    double diffWeight = 1.0;  ///< Absolute luma difference inside the feather window.
    /// Gradient difference inside the feather window.  0 by measurement: on
    /// the sample it moved nothing (0.25: identical metrics) - the absolute
    /// difference already sees every structural disagreement that matters.
    double gradWeight = 0.0;
    double centreWeight = 0.15;    ///< Pull toward the geometric seam (reference implementation: 0.15).
    double temporalWeight = 0.5;   ///< Cost of leaving the neighbouring bucket's seam (per column).
    double temporalNormDeg = 0.5;  ///< Deviation (degrees) at which the temporal cost saturates.
    /// Largest move from the neighbouring bucket's seam, per column (degrees):
    /// prohibitive in the DP and enforced again exactly after smoothing, so a
    /// glide between buckets moves the seam at most this far over
    /// kParallaxBucketFrames frames.
    double temporalClampDeg = 2.0;
    /// Vertical disparity (band rows, full) at which the temporal hold relaxes
    /// to its floor: a column whose content is close to the camera may follow
    /// it (reference implementation: 4 px, floor 0.1).
    double temporalRelaxRows = 4.0;
    double temporalFloor = 0.1;

    // ---- coverage -------------------------------------------------------------
    /// A lens whose band coverage (FOV feather x occlusion) is below this is
    /// "not fully seeing"; each such row on that lens's side of the seam
    /// costs coverageWeight times the shortfall (reference implementation:
    /// 6.0 for its dilated forbidden mask).
    double coverageMin = 0.5;
    double coverageWeight = 6.0;
    /// Coverage below which a lens cannot take part in the blend at all: a
    /// seam whose feather window contains such a row costs forbiddenCost.
    double coverageBlocked = 0.02;
    double forbiddenCost = 1.0e6;  ///< Reference implementation's DP sentinel.

    // ---- DP ---------------------------------------------------------------------
    int maxStepRows = 2;           ///< Largest seam move between neighbouring columns (band rows).
    double stepPenalty = 0.03;     ///< Cost per band row moved between neighbouring columns.
    double smoothSigmaCols = 1.5;  ///< Gaussian smoothing of the DP path along longitude (columns).
    double widthSigmaCols = 3.0;   ///< Gaussian smoothing of the per-column feather width.
    /// [WP-SEAMTOOLS] Gaussian smoothing of the per-column near weight
    /// (BlendSeam::nearWeight) along longitude, in columns.  Wider than the
    /// feather's: the Near / Far Offset it steers MOVES content, and a shift
    /// that changes from column to column tears straight lines along the
    /// seam, where a feather that changes only softens them.  8 columns is
    /// ~2.8 degrees - about the grid pitch the offset is applied through.
    double nearSigmaCols = 8.0;

    /// Optional per-lens penalty of this carve alone, on top of the installed
    /// slot hooks.
    SeamPenaltyHook penalty;
};

/// A carved seam: what the kernel needs, plus what was measured.
struct BlendSeam {
    std::uint32_t columns = 0;  ///< Longitude columns.
    /// Interleaved (latitude, feather half width) in RADIANS per column, the
    /// polar-axis layout's latitude (+ = the master lens's side).
    std::vector<float> table;
    float edgeRad = 0.0f;  ///< Kernel validity ramp (OsvRenderParams::blendSeamEdgeRad).
    /// [WP-SEAMTOOLS] Per column, how much the lenses DISAGREE along the seam,
    /// 0 (agree: far content) .. 1 (disagree: near content) - the same
    /// agreeResidual / disagreeResidual smoothstep that picks the feather,
    /// measured on the bands through the frame's correction (never through
    /// a Near / Far Offset, so the mask cannot chase its own shift) and
    /// smoothed by nearSigmaCols.  Steers the Near / Far Offset
    /// (SeamTools.h).  Empty on a seam built by hand; `columns` long
    /// otherwise.
    std::vector<float> nearWeight;

    // ---- diagnostics ---------------------------------------------------------
    double carveMs = 0.0;             ///< Time spent in carveSeamFromBands.
    double correctMs = 0.0;           ///< ... of which seeing the bands through the correction.
    double costMs = 0.0;              ///< ... of which the cost grid.
    double dpMs = 0.0;                ///< ... of which the DP.
    double meanLatDeg = 0.0;          ///< Mean seam latitude.
    double maxAbsLatDeg = 0.0;        ///< Largest |latitude|.
    double meanHalfWidthDeg = 0.0;    ///< Mean feather half width.
    double meanResidual = 0.0;        ///< Mean structural disagreement along the seam.
    double maxPriorStepDeg = 0.0;     ///< Largest move from the prior seam (0 without one).
    std::uint32_t narrowColumns = 0;  ///< Columns whose feather came out narrow (disagreeing).
    std::uint32_t forcedColumns = 0;  ///< Columns where the path had to cross a forbidden cell.
    bool usedPrior = false;           ///< A neighbouring bucket's seam steered this one.

    /// True when the table is complete and every entry is finite, and the
    /// near weight [WP-SEAMTOOLS] is either absent or one value in [0, 1]
    /// per column.
    [[nodiscard]] bool valid() const noexcept;
};

// ===========================================================================
//  The carve
// ===========================================================================

/// See the two lenses' bands through the kernel's correction, in band space.
///
/// Master (lens 1) is sampled at p + d and slave (lens 0) at p - d, where d
/// is the half correction at p read through osvWarpSample (the kernel's own
/// lookup), plus half the 1-D seam shift along the meridian - so each output
/// pixel of the result holds what the kernel will blend there.  Luma and
/// coverage are both resampled (bilinear, longitude wraps, latitude clamps).
[[nodiscard]] Result<LensBands> correctBandsForSeam(const LensBands& bands, const SeamCorrection& correction,
                                                    ThreadPool* pool = nullptr);

/// Carve the seam from UNCORRECTED bands (as renderLensBands /
/// measureParallaxBands produce them) seen through `correction`.
///
/// `prior` (optional) is a neighbouring bucket's seam: it adds the temporal
/// hold and the clamp.  It must have the same column count, else it is
/// ignored.  `pool` may be nullptr (a background worker should pass that).
///
/// Returns InvalidArgument for malformed bands or parameters.  A band with
/// no co-visible pixels at all still yields a seam (near the geometric seam),
/// never an error.
[[nodiscard]] Result<BlendSeam> carveSeamFromBands(const LensBands& bands, const SeamCorrection& correction,
                                                   const SeamCarveParams& params, const BlendSeam* prior,
                                                   ThreadPool* pool);

/// Render the uncorrected bands (renderLensBands, code space - from host or
/// device frames alike) and carve.
[[nodiscard]] Result<BlendSeam> carveSeam(const geom::LensRig& rig, const video::FramePair& frames,
                                          const geom::BlendParams& blend, const BandParams& band,
                                          const SeamCorrection& correction, const SeamCarveParams& params,
                                          const BlendSeam* prior, ThreadPool& pool);

/// Blend two seams column by column: from + (to - from) * t, t clamped to
/// [0, 1].  Both must have the same column count; the result carries `to`'s
/// kernel parameters and diagnostics.  This is the glide between buckets.
/// [WP-SEAMTOOLS] The near weight glides the same way when both seams carry
/// one (so a Near / Far Offset moves with the seam, never jumps at a bucket
/// edge); otherwise the result keeps `to`'s.
[[nodiscard]] Result<BlendSeam> blendSeams(const BlendSeam& from, const BlendSeam& to, double t);

/// Hand a seam to a render: builder.blendSeam(...) with its table and kernel
/// parameters.  An invalid seam clears the builder's seam instead.
void applyBlendSeam(RenderParamsBuilder& builder, const BlendSeam& seam);

// ===========================================================================
//  The DP on its own (exposed for tests)
// ===========================================================================

/// Solve for one row per column through a cost grid.
///
/// `cost` is row-major, `rows` x `cols` (cost[r * cols + c]).  Moving from
/// row a in column c to row b in column c + 1 is allowed for |a - b| <=
/// maxStep and costs stepPenalty * |a - b| on top of the cells' costs.
/// With `closedRing` the last column also connects back to the first under
/// the same rule (a longitude ring); otherwise both ends are free.
///
/// Returns the row per column, or InvalidArgument for an empty or mis-sized
/// grid, a negative step or a non-finite penalty.  Non-finite cell costs are
/// treated as forbidden (a very large cost), never propagated.
[[nodiscard]] Result<std::vector<int>> solveSeamDp(const std::vector<float>& cost, std::uint32_t cols,
                                                   std::uint32_t rows, int maxStep, double stepPenalty,
                                                   bool closedRing);

}  // namespace osv::render
