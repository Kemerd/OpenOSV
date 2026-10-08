// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ParallaxWarp.h - turn a measured optical-flow field into the 2-D warp grid
// the kernel samples, so flow-based parallax correction actually reaches the
// output picture.
//
// WHERE THIS SITS
// ---------------
// Three pieces already existed and none of them was connected to anything:
//
//     renderLensBands   ->  computeFlow        ->  warpToMiddle
//     (two views of the     (how far each          (bend both halfway
//      overlap band)         pixel disagrees)       and blend)
//
// That chain produces a corrected BAND IMAGE.  The renderer does not draw
// band images - it samples the two fisheye frames per output pixel and blends
// them (osvShadePixelW).  So a corrected band, however good, is a picture
// nobody looks at.  This file closes that gap: it converts the flow field
// into a per-direction ANGULAR correction the kernel can apply while it
// samples, which is the only form the real render path can consume.
//
// WHY A GRID AND NOT A PRE-WARP OF THE FISHEYE IMAGES
// ---------------------------------------------------
// Three routes were considered for getting flow into the kernel.
//
//   1. Pre-warp the two fisheye frames before the kernel runs.  Rejected:
//      it means resampling two 6K 10-bit frames every frame, which costs
//      more than the render itself, and it bakes one resampling generation
//      into the data before the kernel does its own - two bilinear passes
//      where one would do, and the softness is visible.
//
//   2. Carry a displacement texture in FISHEYE pixel space, one per lens.
//      Rejected: the flow is measured in the rectified band, so this needs
//      a scatter from band coordinates into each lens's distorted image,
//      and a scatter has no clean inverse - holes and folds where the
//      mapping is not injective.
//
//   3. Carry an ANGULAR correction on the sphere, sampled by the kernel for
//      the ray it is already computing.  Chosen.  The flow is measured on a
//      polar-axis equirect band, whose axes ARE longitude and latitude, so
//      band pixels convert to angles by a scale factor and nothing else.
//      Both lenses read the same grid with opposite sign, so one table
//      corrects both, and it composes with the existing seam table instead
//      of fighting it.
//
// This is also the route the existing 1-D mechanism already takes: the seam
// table is exactly this idea with one component and one dimension.  The grid
// generalises it rather than replacing it, which is why both can be on at
// once: the kernel applies the seam shift first and the warp on top of it,
// and the flow is measured on bands already rendered WITH the seam table, so
// the grid carries only the residual the table left behind.
//
// THE TWO COMPONENTS, AND WHICH WAY THEY POINT
// ---------------------------------------------
// A ray in the overlap needs a correction with two degrees of freedom.  The
// grid stores them in the band's own frame, per cell:
//
//   dLat  along the meridian.  The epipolar direction of a back-to-back
//         pair, so most real parallax lies along it, and the only direction
//         the 1-D seam table can already correct.
//
//   dLon  across the meridian.  No 1-D table can express this, and it is
//         what leaves a wing tip smeared when only the meridian is fixed.
//
// Both are the displacement of the MASTER lens's sampling direction; the
// slave lens takes the negation (see osvShadePixelW).  An earlier version
// expressed them as rotations about each lens's own axis AND flipped the
// sign between the lenses - but the two axes already point in opposite
// directions, so the flip cancelled the geometry and both lenses moved the
// same way, shifting the picture without closing any disparity.  One shared
// frame with one explicit sign rule removes the ambiguity.
//
// BOUNDARY DECAY IS NOT OPTIONAL
// ------------------------------
// Flow is only measured where both lenses see the scene.  Applying it inside
// the band and nothing outside puts a step discontinuity at the band edge,
// and a corrected seam with a torn edge is strictly WORSE than an uncorrected
// seam - the eye finds a hard edge faster than it finds a soft double image.
//
// Perazzi et al. (EG 2015) solve a Poisson equation to extrapolate the warp
// to zero outside the overlap.  Facebook's Surround360 does a cheaper
// alpha-driven diffusion.  This file takes the cheap route deliberately: the
// grid is built with a decay ring above and below the measured rows, over
// which the correction falls smoothly to zero, and the kernel returns zero
// for any ray outside the grid's latitude span.  The two together guarantee
// C0 continuity at the edge without a linear solve.  See
// ParallaxWarpParams::decayRows.
//
// ANISOTROPIC REGULARIZATION
// --------------------------
// Motion across the seam is mostly ALONG meridians, because that is the
// epipolar direction of a back-to-back pair.  A cross-meridian component of
// the same magnitude is far more likely to be a mismatch than real geometry.
// Surround360 penalises the two directions separately
// (verticalRegularizationCoef vs horizontalRegularizationCoef) and Jump
// (Anderson et al., SIGGRAPH Asia 2016) searches [0, 224] horizontally
// against [-16, 16] vertically - a 14:1 ratio.
//
// The asymmetry is applied here by smoothing the cross-meridian component
// harder than the along-meridian one (crossMeridianSmooth), and optionally by
// scaling it down (crossMeridianScale).  The scale defaults to 1 - see its
// comment for the measurement that overturned the 0.5 first used - because
// the benefit gate (requiredImprovement) turned out to be the better guard
// against mismatches: it judges a correction by whether it actually makes
// the lenses agree, in either direction.
//
// WHAT IS NOT DONE
// ----------------
// TEMPORAL FILTERING lives above this file.  Jump's observation is that
// viewers forgive consistent ghosting but notice ghosting that CHANGES frame
// to frame, and DJI's stitcher filters its flow over time for it.  This file
// measures one frame with no memory; the schedule below measures once per
// bucket of frames and glides between buckets, and render/ClipSteady.h goes
// further for rigid mounts: ONE grid per clip, the per-cell median of a
// fixed set of sample frames, so the warp does not move at all - and, since
// those frames are fixed by the clip itself, never depends on the order in
// which Premiere asked for frames.
//
// The flow backend's own cost is also untouched here: the classical solver
// takes ~215 ms of the ~225 ms a grid costs at the default 2048-column band
// (a 1536-column band measured ~135 ms at essentially the same quality).

#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/FlowBackend.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/PlanarFrame.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace osv::render {

/// Tuning for the flow measurement and the grid it produces.
struct ParallaxWarpParams {
    /// Geometry of the analysed overlap band.  The default half-height is
    /// deliberately larger than the seam search's 4 degrees: flow needs
    /// context above and below the disparity to lock onto, and a band that
    /// only just contains the motion gives the solver nothing to match
    /// against at its own edges.
    BandParams band{2048, 6.0};

    /// Which flow implementation to ask for.  Auto prefers the neural
    /// backend when one is installed and falls back silently.
    FlowBackendKind backend = FlowBackendKind::Auto;

    /// Parameters handed to the backend (DIS tuning, model path, tolerance).
    FlowBackendParams flow;

    /// Columns of the output grid.
    ///
    /// Much coarser than the band (2048 columns) on purpose.  The correction
    /// is a smooth, low-frequency field - it describes where the geometry
    /// disagrees, not the texture - so a coarse lattice loses nothing the
    /// bilinear fetch does not put back, and it keeps the table small enough
    /// to upload every frame without thinking about it.  256 x 48 is 96 KB.
    std::uint32_t gridW = 256;

    /// Rows of the output grid ACROSS the band, excluding the decay rings.
    std::uint32_t gridRows = 32;

    /// Rows of decay added above and below the measured rows.
    ///
    /// This is the boundary-decay ring the header argues for.  Over these
    /// rows the correction is scaled smoothly from full strength to exactly
    /// zero, so a ray crossing out of the overlap sees the correction fade
    /// rather than stop.  Zero disables the ring, which is available for
    /// A/B but produces the torn edge described above.
    std::uint32_t decayRows = 8;

    /// Scale applied to the cross-meridian (dLon) component, in [0, 1].
    ///
    /// 1.0 - full trust - and that is a MEASURED choice, not the literature
    /// default.  The first version used 0.5 on the argument the header gives
    /// (perpendicular disparity on a back-to-back rig is more often a
    /// mismatch than real geometry).  On the sample clip that was wrong: the
    /// flow finds a genuine cross-meridian misalignment of up to ~0.66
    /// degrees around the ring - consistent with a small roll error between
    /// the calibrated lens orientations, which is alignment error whatever
    /// its cause - and halving it left half of it on screen.  Measured on
    /// frames 0 / 32 / 64, whole-band NCC 0.912 / 0.909 / 0.916 at 0.5
    /// against 0.918 / 0.914 / 0.920 at 1.0, and better at 1.0 on the
    /// textured ground and on the near-field wingtip alike.
    ///
    /// What made full trust safe is the benefit gate (requiredImprovement):
    /// it rejects a mismatched correction by its EFFECT, in either
    /// direction, which is a better test than distrusting one direction by a
    /// fixed factor.  The anisotropy survives as the extra smoothing below.
    double crossMeridianScale = 1.0;

    /// Extra smoothing applied to the cross-meridian component only, in grid
    /// cells.  Follows from the same asymmetry: the less trustworthy
    /// component is also the one that benefits most from being forced to
    /// agree with its neighbours.
    double crossMeridianSmooth = 2.0;

    /// Largest angular correction any grid cell may carry, in degrees.
    ///
    /// A safety clamp, not a tuning knob.  A diverged flow vector becomes a
    /// large rotation, and a large rotation samples the fisheye somewhere
    /// unrelated - which looks far worse than the parallax it was meant to
    /// fix.  Bounding the output means the worst case is an under-correction.
    double maxCorrectionDeg = 3.0;

    /// The retired all-pixel threshold, in [0, 1]: kept for diagnostics only.
    ///
    /// Up to 0.5.0 a grid was refused when fewer than this fraction of ALL
    /// co-visible band pixels passed the forward-backward check.  That share
    /// mostly measured how much of the band was sky: measured on a
    /// car-mounted day clip, 47-85 % of the co-visible pixels are flat, flat
    /// pixels come out only 9-15 % consistent whatever the solver does (it
    /// solves noise there), textured ones 49-80 %.  So the share sat at
    /// 0.19-0.35 and the verdict flipped between consecutive buckets (7 of
    /// 12 accepted on the day proxy, frames 5992-6080), switching the seam
    /// between the grid and the table every few buckets.  The gate is now
    /// the STRUCTURED share
    /// below (minStructuredConsistent and its neighbours); nothing refuses
    /// on this number any more.  osvtool seam still reports whether the
    /// all-pixel share would have met it, for before/after comparisons.
    double minConsistentFraction = 0.25;

    /// Smallest luma gradient (0..1 code scale, per band pixel) for a
    /// co-visible pixel to count as STRUCTURED: two 8-bit code steps.
    ///
    /// The measure is the magnitude of the central-difference gradient
    /// (half the difference of the two neighbours, along and across the
    /// band), taken in each lens and the SMALLER of the two kept - a match
    /// needs structure in both.  A neighbour the lens does not cover, or one
    /// beyond the band's top or bottom row, stands in with the centre pixel,
    /// so a coverage edge (a rim, an occlusion polygon) cannot pose as
    /// structure; longitude wraps, because the band is a ring.  Both rules
    /// are the seam table's texture term's (SeamSearchParams::confTextureLo);
    /// that term differences along the meridian only, because its search is
    /// 1-D along it, where the flow here is 2-D and an edge across the band
    /// (a pole crossing the seam) is structure it measures.  The flow solver
    /// has something to lock onto at such a pixel, and its forward-backward
    /// verdict there says something about the measurement rather than about
    /// the sky.
    double minStructureGradient = 2.0 / 255.0;

    /// Below this many structured co-visible pixels the measurement is
    /// refused outright: 1500 is about 1 % of a 2048 x 68 band, a few
    /// centimetres of edge at the seam.  Fog, a night sky or open water fall
    /// below it and render with the seam table, which is the intent.
    std::uint64_t minStructuredPixels = 1500;

    /// The structured gate, in [0, 1]: the share of STRUCTURED co-visible
    /// pixels whose flow passed the forward-backward check.
    ///
    /// Below minStructuredConsistent the grid is refused (Unsupported);
    /// from fullStructuredConsistent on it applies at full strength; in
    /// between it applies at strength s = smoothstep from the one to the
    /// other, and the caller fills the rest with the seam table (the
    /// importer's per-bucket and per-clip corrections both do), so the
    /// correction moves continuously as the share drifts across the gate
    /// instead of flipping between grid and table.  Measured on the
    /// structured share with the classical solver: real frames 0.43-0.97
    /// (car-mounted day proxy 0.71-0.90, its 8K original 0.43-0.77, the
    /// night drive 0.66-0.82, the airborne sample 0.88-0.97 - all at full
    /// strength), where the all-pixel share of the same frames was
    /// 0.17-0.54.  Deliberately unrelated pairs (lens 0 of one moment of a
    /// car drive against lens 1 of another) scored 0.10-0.28 - on a rigid
    /// mount the car body is the same at both moments, so part of such a
    /// pair is not unrelated - and the floor sits at 0.30 so that every one
    /// of them is refused while the lowest real frame (0.43) keeps its full
    /// strength.
    double minStructuredConsistent = 0.30;
    /// See minStructuredConsistent: the share from which the grid applies at
    /// full strength.  Must not be below minStructuredConsistent.
    double fullStructuredConsistent = 0.40;

    /// Fraction by which a cell's correction must REDUCE the disagreement
    /// between the two lenses before it is applied at full strength, in
    /// [0, 1); 0 disables this gate.
    ///
    /// WHY THIS EXISTS - "do no harm".  The forward-backward check proves
    /// that two flow fields agree with each other, not that they describe
    /// the scene.  Where the lenses see DIFFERENT objects - something a few
    /// centimetres from one lens, such as a wingtip light cover that the
    /// other lens cannot see at all - the solver still finds self-consistent
    /// matches along edges, and warping by them bends real geometry for no
    /// gain.  On the sample clip that is exactly what happened at the wing:
    /// the correction made the local agreement WORSE (NCC 0.32 -> 0.28).
    ///
    /// So each cell is tested against the thing that actually matters: the
    /// mean |lens0 - lens1| over its co-visible pixels, warped by the FINAL
    /// field exactly as the kernel will apply it (after smoothing and decay,
    /// through osvWarpSample), pooled over its 3 x 3 neighbourhood, against
    /// the same lenses sampled
    /// WITHOUT the relative displacement at the same interpolation phases
    /// (see gridFromFlow for why the phases matter).  At a residual ratio of
    /// 1 - requiredImprovement or better the cell keeps its full correction;
    /// at 1 - requiredImprovement / 4 or worse it gets none - a small dead
    /// zone that absorbs the statistical scatter of a few-dozen-pixel cell -
    /// and smoothstep in between, so the gate cannot introduce a step of its
    /// own.
    double requiredImprovement = 0.2;

    /// Cells whose UNWARPED residual is below this (code values, 0..1 scale)
    /// have nothing measurable to fix - open sky, flat water - and are left
    /// uncorrected.  About a third of one 8-bit code step.
    double minResidual = 0.0015;
};

/// The grid the kernel samples, plus what was measured while building it.
struct ParallaxWarpGrid {
    std::uint32_t w = 0;         ///< Columns (longitude, wraps).
    std::uint32_t h = 0;         ///< Rows (latitude), including the decay rings.
    float latMinRad = 0.0f;      ///< Latitude of row 0.
    float latMaxRad = 0.0f;      ///< Latitude of row h - 1.
    /// Interleaved (dLon, dLat) in RADIANS, w * h pairs: HALF the measured
    /// disparity, as the displacement of the master lens's sampling
    /// direction (the slave takes the negation).
    std::vector<float> uv;

    /// The structured gate's strength s in [0, 1] (ParallaxWarpParams::
    /// minStructuredConsistent): `uv` already carries it - every cell is s
    /// times the measured correction - and the caller fills the remaining
    /// 1 - s with the seam table.  1 for a fully trusted measurement, and
    /// for a grid gridFromFlow() built directly (no gate ran).  A clip grid
    /// (render::clipParallaxGrid) holds the median of its samples'.
    double strength = 1.0;

    /// Per grid column (`w` entries, each in [0, 1]): the share of the
    /// column's MEASURED rows (gridRows, not the decay rings) whose cell the
    /// measurement leaves untrusted - no consistent flow landed in it (the
    /// fill copied a neighbour's value there), or the benefit gate switched it
    /// fully off.  Filled by gridFromFlow(); a clip grid holds the per-column
    /// median of its samples'.  The per-column guard (guardGridWithTable)
    /// hands such columns to the seam table where the table is sure of them
    /// and found a disparity the grid missed.
    /// Empty for a grid nothing measured (one built by hand): the guard then
    /// has nothing to go on and leaves the grid alone.
    std::vector<float> untrustedShare;

    // ---- diagnostics -------------------------------------------------------
    FlowBackendKind usedBackend = FlowBackendKind::Classical;
    std::uint64_t consistentPixels = 0;  ///< Co-visible pixels whose flow passed the check.
    std::uint64_t totalPixels = 0;       ///< Co-visible pixels examined.
    /// Co-visible pixels with structure in both lenses (see
    /// ParallaxWarpParams::minStructureGradient).
    std::uint64_t structuredPixels = 0;
    /// Of those, the ones whose flow passed the check.
    std::uint64_t consistentStructuredPixels = 0;
    double bandMs = 0.0;                 ///< Time spent rendering the two lens bands.
    double flowMs = 0.0;                 ///< Time spent in the flow backend.
    double gridMs = 0.0;                 ///< Time spent turning flow into the grid.
    std::uint32_t measuredCells = 0;     ///< Grid cells that received consistent flow.
    std::uint32_t gatedCells = 0;        ///< Of those, cells the benefit gate switched fully off.
    double meanAbsCorrectionDeg = 0.0;   ///< Mean FULL disparity corrected, measured rows (deg), after strength.
    double maxAbsCorrectionDeg = 0.0;    ///< Largest FULL disparity corrected, after clamping and strength (deg).

    [[nodiscard]] bool valid() const noexcept {
        return w > 0 && h > 0 && uv.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 2u;
    }

    /// Fraction of ALL co-visible flow pixels that passed the consistency
    /// check, in [0, 1] - the retired gate's number, kept for diagnostics.
    [[nodiscard]] double consistentFraction() const noexcept {
        return totalPixels ? static_cast<double>(consistentPixels) / static_cast<double>(totalPixels) : 0.0;
    }

    /// Fraction of the STRUCTURED co-visible pixels that passed the
    /// consistency check, in [0, 1]: the number the gate judges.  0 when no
    /// pixel had structure.
    [[nodiscard]] double structuredFraction() const noexcept {
        return structuredPixels ? static_cast<double>(consistentStructuredPixels) /
                                      static_cast<double>(structuredPixels)
                                : 0.0;
    }
};

/// What each MEASURED cell of a grid saw, before anything shaped it.
///
/// The grid the kernel samples is the flow after the cross-meridian scale,
/// the safety clamp, the fill of empty cells, the anisotropic blur, the decay
/// ring and the benefit gate - every one of those is right for RENDERING and
/// wrong for fitting a model to the measurement: the blur spreads a near
/// object into its neighbours, the gate zeroes the sky, the fill invents
/// values.  A global model (the per-clip lens rotation, render/LensAlign.h)
/// wants the raw per-cell means and a note of which cells the benefit gate
/// trusted, so gridFromFlow() can hand them out on the side.
///
/// Layout: `w` columns (longitude, the grid's own gridW) by `rows` measured
/// rows (gridRows, no decay rings), row-major.  Cell (c, r) sits at longitude
/// c * 2 pi / w - pi (the kernel reads cell c at longitude fraction c / w) and
/// latitude latTopRad - r * latStepRad.
struct ParallaxCellStats {
    std::uint32_t w = 0;       ///< Columns (== ParallaxWarpParams::gridW).
    std::uint32_t rows = 0;    ///< Measured rows (== ParallaxWarpParams::gridRows).
    double latTopRad = 0.0;    ///< Latitude of measured row 0 (the band's top row centre).
    double latStepRad = 0.0;   ///< Latitude step per row (rows descend, so this is positive).
    /// Interleaved (dLon, dLat) in RADIANS per cell: the mean HALF disparity of
    /// the consistent co-visible pixels - the master lens's displacement, the
    /// same quantity the grid stores - before scale, clamp, fill, blur, decay
    /// and gate.  Zero for a cell with no consistent pixel.
    std::vector<float> halfFlow;
    /// Consistent co-visible band pixels that fed each cell.
    std::vector<std::uint32_t> pixels;
    /// The benefit gate's verdict per cell, in [0, 1] (the pooled 3 x 3
    /// decision before its smoothing): 1 = warping by the flow measurably
    /// reduced the lens-to-lens residual here.  All 1 when the gate is off.
    std::vector<float> gate;

    // ---- the band-wide counts the structured gate judges ------------------
    // The same four numbers ParallaxWarpGrid carries, set together with the
    // cells - so a caller of parallaxFromBands() can still report them when
    // the measurement is refused and no grid comes back.
    std::uint64_t covisiblePixels = 0;             ///< == ParallaxWarpGrid::totalPixels.
    std::uint64_t consistentPixels = 0;            ///< == ParallaxWarpGrid::consistentPixels.
    std::uint64_t structuredPixels = 0;            ///< == ParallaxWarpGrid::structuredPixels.
    std::uint64_t consistentStructuredPixels = 0;  ///< == ParallaxWarpGrid::consistentStructuredPixels.

    /// True when every per-cell array matches w * rows.
    [[nodiscard]] bool valid() const noexcept {
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(rows);
        return w > 0 && rows > 0 && halfFlow.size() == n * 2u && pixels.size() == n && gate.size() == n &&
               std::isfinite(latTopRad) && std::isfinite(latStepRad);
    }
};

/// Measure the parallax across the overlap band and build the kernel grid.
///
/// Renders the two per-lens bands, computes bidirectional flow across them
/// through the requested backend, converts the flow to angular corrections,
/// applies the anisotropic scaling and the boundary decay, and returns the
/// grid ready to hand to RenderParamsBuilder::warp().
///
/// `seamTable` (optional) is the 1-D seam profile already in force.  Passing
/// it makes the flow measure only what the seam table did NOT correct, so
/// the two compose instead of double-counting the same disparity.  Note that
/// on the sample clip composing measured WORSE than the grid alone (whole-
/// band overlap NCC, robust table and fixed solver, LRF 30 / OSV 60: 0.9782 /
/// 0.9128 against 0.9793 / 0.9197), so the importer and `osvtool render` pass
/// nullptr and use the grid INSTEAD of the table, keeping the table as the
/// fallback when a grid is refused and for the columns the per-column guard
/// hands it (guardGridWithTable).
///
/// Returns Unsupported when the flow was measured but the structured gate
/// refused it (too few structured pixels, or too small a consistent share of
/// them: see minStructuredConsistent) - a normal outcome on featureless
/// content such as fog or open sky, which the caller handles with the seam
/// table or by rendering uncorrected; the message carries the structured and
/// the all-pixel shares.  An accepted grid between the gate's two shares
/// comes back scaled by its strength (ParallaxWarpGrid::strength).  Returns
/// InvalidArgument for malformed parameters.
[[nodiscard]] Result<ParallaxWarpGrid> buildParallaxWarp(const geom::LensRig& rig, const video::FramePair& frames,
                                                         const geom::BlendParams& blend,
                                                         const ParallaxWarpParams& params,
                                                         const std::vector<float>* seamTable, ThreadPool& pool);

/// The first half of buildParallaxWarp: render the two per-lens bands.
///
/// WHY IT IS SPLIT OUT: this is the only stage that needs the decoded frame
/// and the rig, and it is cheap (the band is 68 rows).  Everything after it -
/// the flow solve, which is ~95 % of the analysis cost, and the grid build -
/// needs nothing but the bands.  So a caller that cannot afford to wait (the
/// importer during playback and scrubbing) runs this on the render thread,
/// hands the small owned result to a background worker, and never blocks on
/// the expensive half.  The frame it came from can be released the moment
/// this returns.
///
/// Parameters and the `seamTable` rule are exactly buildParallaxWarp's.
[[nodiscard]] Result<LensBands> measureParallaxBands(const geom::LensRig& rig, const video::FramePair& frames,
                                                     const geom::BlendParams& blend,
                                                     const ParallaxWarpParams& params,
                                                     const std::vector<float>* seamTable, ThreadPool& pool);

/// The second half of buildParallaxWarp: flow, grid and the refusal rule,
/// from bands produced by measureParallaxBands().
///
/// Thread-safe for concurrent calls on different bands: it touches no shared
/// state beyond what computeFlow() documents.  `pool` may be nullptr, which
/// is what a background worker should pass - sharing the render pool would
/// make the worker's parallel sections queue behind (and hold up) the frame
/// renders it exists to stay out of the way of.  `bandMs` is recorded on the
/// grid purely for diagnostics.
///
/// Returns exactly what buildParallaxWarp returns for the same bands,
/// including Unsupported for a measurement the structured gate refuses, and
/// a grid scaled by its strength between the gate's two shares.
///
/// `cells` (optional) receives the raw per-cell measurement and the gate's
/// band-wide counts (ParallaxCellStats) - filled whenever the grid itself
/// was built, so also when the gate then refuses it.  Passing it changes
/// nothing about the grid.
[[nodiscard]] Result<ParallaxWarpGrid> parallaxFromBands(const LensBands& bands, const ParallaxWarpParams& params,
                                                         ThreadPool* pool, double bandMs = 0.0,
                                                         ParallaxCellStats* cells = nullptr);

/// The structured gate's strength for a measured structured share (see
/// ParallaxWarpParams::minStructuredConsistent): 0 at or below
/// `params.minStructuredConsistent`, 1 at or above
/// `params.fullStructuredConsistent`, smoothstep between - so the strength,
/// and with it the applied correction, has no step anywhere.  Equal shares
/// make it a plain step at that share.  A non-finite share, or a malformed
/// pair of shares (non-finite, or full below min), gives 0: no trust.
[[nodiscard]] double parallaxGateStrength(double structuredFraction, const ParallaxWarpParams& params) noexcept;

/// The share of a seam table that fills in under a grid of strength
/// `gridStrength` (ParallaxWarpGrid::strength), into `out` (its capacity is
/// reused): every column times 1 - gridStrength, so a fully trusted grid
/// leaves no table (empty `out`), a refused measurement's strength of 0 the
/// whole table bit for bit, and a partly trusted grid the rest - the grid
/// and the table then add up to one correction, as the kernel applies the
/// table first and the warp on top.  The strength is clamped to [0, 1]; a
/// non-finite one counts as 0 (the grid is not trusted, the table is the
/// correction).  Non-finite entries become 0.  An empty table gives an empty
/// `out`.
void seamTableUnderGrid(const std::vector<float>& table, double gridStrength, std::vector<float>& out);

// ===========================================================================
//  The per-column guard: the seam table where the grid measured nothing
//
//  An accepted grid REPLACES the seam table (see buildParallaxWarp for the
//  measurement that keeps it that way).  The two measure differently: the
//  table searches each column along the meridian over +/- 24 band rows, the
//  flow follows a pyramid that does not reach a large near-field offset
//  everywhere.  On a car mount the car body crosses the seam ~2.5 deg out of
//  place; half or more of its grid cells come out unmeasured (no consistent
//  flow: the fill copies a neighbour's value in) or gated (the benefit gate
//  saw the flow make things no better), the rest under-correct, and the
//  rails stay doubled - while the table measured exactly those columns with
//  confidence.  Measured on the day proxy at frame 3423 (car body, band
//  columns 1548-1659): table +2.15..+2.73 deg, the grid's along-meridian
//  correction +0.96..+2.07 deg; overlap NCC table 0.93-0.98, grid 0.26-0.92.
//
//  Per grid column the guard weighs three things, each a smoothstep so the
//  hand-over has no threshold step of its own:
//    u  the column's untrusted share (ParallaxWarpGrid::untrustedShare):
//       kGridGuardUntrustedLo .. kGridGuardUntrustedHi - the grid did not
//       measure the column;
//    c  the table's confidence there (SeamProfile::confidence):
//       kGridGuardConfidenceLo .. kGridGuardConfidenceHi - the table did;
//    d  how far the correction the column renders along the meridian is
//       from the table's: |G - s T| (G the grid's own along-meridian
//       correction as a table value, s its strength, T the table):
//       kGridGuardAgreeDeg .. kGridGuardDisagreeDeg - the table found a
//       disparity the grid missed.
//  g = sstep(u) * sstep(c) * sstep(d), smoothed along the ring like the
//  benefit gate's weights, then
//      grid         column x (1 - g)
//      table share  T x (1 - s (1 - g))
//  so the grid and the table still add up to ONE correction everywhere: a
//  column where any of the three says no keeps the grid exactly as before
//  (g = 0, the table's 1 - s share of seamTableUnderGrid), a column all three
//  hand over takes the table in full.  The per-cell rule (a cell the grid
//  could not measure goes to a table that is sure of it) becomes a
//  per-column one because the table is one number per column.
//
//  G is -2 dLat in degrees, averaged over the rows where the kernel applies
//  the table in full (osvSeamShiftTaper): the grid stores the master lens's
//  displacement (HALF the disparity), the master's axis is body +Y - the +90
//  deg pole of the polar-axis band - and a positive table value moves each
//  lens AWAY from its own axis.
//
//  Measured and rejected on the way (osvtool seam, classical flow,
//  kernel-rendered overlap NCC):
//    * g = u * c (no smoothstep on u, no d): the car body went only half to
//      the table (about half of its cells pass the benefit gate, which
//      compares a correction against NONE, not against the table) and the
//      proxy's car window median stayed at the grid's 0.951;
//    * without d: with the maintainer's 6K sample's lens rotation folded (the
//      plug-ins' default) the aligned ground is half gated - nothing left to
//      fix - and the table took 64-83 grid columns over at a shift of ~0.01
//      deg, dropping the grid's small 2-D gains (OSV 60 0.9138 -> 0.9131,
//      the clip correction -0.0009 on every sample frame); there the table
//      and the grid agree to 0.05 deg, on the car body they differ by
//      0.5-1.3 deg;
//    * the grid kept whole and the table adding g (s T - G): a column of a
//      car mount is part car body (under-corrected) and part background
//      (near zero), and a column-wide offset mis-corrected both (car window
//      median 0.93, below the grid alone).
//
//  Composing the two everywhere (the grid measured on bands the table has
//  already corrected) was measured as the alternative and NOT adopted: it
//  wins on the car body, but costs the 6K sample's OSV 0.007 of whole-band
//  overlap NCC (frame 60: 0.9128 against the grid alone's 0.9197) - the
//  table's smoothed shift stays where the grid does better.
// ===========================================================================

/// Table confidence (SeamProfile::confidence) at or below which the guard
/// never hands a column to the table: the confidence below which a table
/// change never steps either (kSeamTableStepConfLo, below).
inline constexpr double kGridGuardConfidenceLo = 0.3;
/// Table confidence from which an untrusted grid column can go to the table
/// in full: a measurement this sure steps at a bucket's anchor
/// (kSeamTableStepConfHi, below), and is sure enough to replace the grid.
inline constexpr double kGridGuardConfidenceHi = 0.6;
/// Untrusted share (ParallaxWarpGrid::untrustedShare) up to which a grid
/// column stays the grid's: a quarter of its cells unmeasured or gated is the
/// sky above or the road below a feature the flow did measure.
inline constexpr double kGridGuardUntrustedLo = 0.25;
/// Untrusted share from which a grid column can go to a confident table in
/// full: half or more of its cells unmeasured or gated means the grid did
/// not measure that column.  Measured on the day proxy at frame 3423: the
/// car-body columns the table aligns (NCC 0.93-0.98) and the grid does not
/// (0.26-0.92) are 47-81 % untrusted; the columns beside them where the grid
/// does better (0.88-0.97 against 0.80-0.92) are 25-31 %.
inline constexpr double kGridGuardUntrustedHi = 0.5;
/// Disagreement (degrees, |G - s T|) up to which the grid and the table say
/// the same thing along the meridian and the grid keeps the column: half the
/// table's own bucket-to-bucket noise (kGridGuardDisagreeDeg), 1.4 rows of a
/// 2048-column band.
inline constexpr double kGridGuardAgreeDeg = 0.25;
/// Disagreement (degrees) from which the table has found a disparity the
/// grid missed: the top of the table's bucket-to-bucket measurement noise
/// (kSeamTableGlideNoiseDeg, below - 2.8 px of a 2048 px equirect).  The
/// car body at frame 3423 disagrees by 0.5-1.3 deg, the 6K sample's aligned
/// ground by at most 0.05.
inline constexpr double kGridGuardDisagreeDeg = 0.5;
/// Smoothing of the per-column guard weight along the ring, in grid columns
/// (the benefit gate's own weight smoothing), so the hand-over between grid
/// and table has no step from one grid column to the next.
inline constexpr double kGridGuardSmoothCols = 1.0;
/// A grid column counts as guarded in the diagnostics from this weight on.
inline constexpr double kGridGuardCountedWeight = 0.5;

/// A grid and the seam table share that renders with it, after the guard.
struct GuardedCorrection {
    /// The grid with each column scaled by 1 - g - filled only when
    /// `changed`; otherwise empty, and the input grid renders as it is.
    ParallaxWarpGrid grid;
    /// The table share at every table column, T x (1 - s (1 - g)) - empty
    /// when nothing of the table applies (a fully trusted grid and no guarded
    /// column), exactly as seamTableUnderGrid leaves it.
    std::vector<float> table;
    /// The fraction of the table that renders at every table column,
    /// 1 - s (1 - g) in [0, 1]: how much of that column's correction along
    /// the meridian is the table's (the rest is the grid's).  Same length as
    /// `table`, empty with it.  A glide between two buckets weighs the
    /// table's confidence by it (render::blendSeamTables): where the grid
    /// carries a column, a change of the table there is not the table's to
    /// step.
    std::vector<float> tableFraction;
    /// The guard weight g per grid column, in [0, 1]; empty when no column
    /// was guarded (`changed` false).
    std::vector<float> guard;
    /// The grid's own along-meridian correction per grid column, as a table
    /// value in degrees (G above); filled whenever the guard could weigh the
    /// columns (a table, its confidence and the grid's untrusted shares).
    std::vector<float> gridAlongDeg;
    /// Grid columns guarded by at least kGridGuardCountedWeight.
    std::uint32_t guardedColumns = 0;
    /// Mean guard weight over the ring (0 when unchanged).
    double meanGuard = 0.0;
    /// True when at least one column was guarded at all: `grid` is then the
    /// one to render and the share differs from seamTableUnderGrid's.
    bool changed = false;
};

/// Apply the per-column guard (see above) to an accepted `grid` with the
/// seam `table` measured on the same frame and its per-column `confidence`:
/// the grid and the table share that render together.
///
/// Without what the guard needs - an empty table, a confidence that does not
/// match the table column for column, or a grid without its untrusted shares
/// - it falls back to the rule before it: the grid as it is and
/// seamTableUnderGrid's share of the table (if any).  A non-finite share,
/// confidence, grid cell or table entry counts as 0 (no hand-over, no
/// shift); the strength is clamped to [0, 1] and a non-finite one counts as
/// 0.  InvalidArgument for a malformed grid.
[[nodiscard]] Result<GuardedCorrection> guardGridWithTable(const ParallaxWarpGrid& grid,
                                                          const std::vector<float>& table,
                                                          const std::vector<float>& confidence);

// ===========================================================================
//  Temporal schedule: measure once per bucket of frames, glide between them
//
//  The flow solve costs ~220 ms of CPU per measured frame, and a frame's
//  parallax differs from its neighbours' by very little: the rig is rigid,
//  and what sits near the seam (a wing, the ground) moves slowly relative to
//  it.  Measuring every frame made the importer ~5x slower for no visible
//  gain and, because every frame was measured independently, let the
//  correction shimmer.  So a video measures once per BUCKET of
//  kParallaxBucketFrames frames, and each frame BLENDS from the previous
//  bucket's grid to its own - a correction that glides instead of stepping
//  every bucket edge.
//
//  These are pure functions so the schedule is pinned by unit tests rather
//  than inferred from rendered pixels.
// ===========================================================================

/// Frames per measured bucket.  8 at 60 fps is one measurement every 133 ms
/// - about 27 ms of flow per exported frame instead of ~220 - while still
/// following changes a viewer could notice.
inline constexpr std::uint32_t kParallaxBucketFrames = 8;

/// The bucket frame `frame` belongs to.  A zero bucket size is treated as 1
/// (every frame its own bucket) rather than dividing by zero.
[[nodiscard]] constexpr std::uint32_t parallaxBucket(std::uint32_t frame,
                                                     std::uint32_t bucketFrames = kParallaxBucketFrames) noexcept {
    return bucketFrames == 0 ? frame : frame / bucketFrames;
}

/// Weight of the frame's OWN bucket grid when blending from the previous
/// bucket's grid, in (0, 1].
///
/// (frame mod N + 1) / N: the last frame of a bucket is its own grid alone,
/// and the first frame of the next bucket moves only 1/N of the way to the
/// new grid - so the correction is continuous across the edge, changing by
/// at most 1/N of the difference between neighbouring measurements per frame.
[[nodiscard]] constexpr double parallaxCrossfadeWeight(std::uint32_t frame,
                                                       std::uint32_t bucketFrames = kParallaxBucketFrames) noexcept {
    if (bucketFrames <= 1) {
        return 1.0;
    }
    return static_cast<double>(frame % bucketFrames + 1) / static_cast<double>(bucketFrames);
}

/// Blend two grids: from + (to - from) * t, with t clamped to [0, 1].
///
/// Both must share one layout (size and latitude span) - which grids built
/// with the same parameters always do - and the result carries `to`'s
/// diagnostic fields.  Blending the corrections, rather than switching
/// between them, is what removes the step at a bucket edge; the benefit
/// gate's per-cell decisions blend with them, so a cell one grid gated off
/// fades out instead of vanishing.
///
/// Returns InvalidArgument for mismatched layouts, a malformed grid, or a
/// non-finite t.
[[nodiscard]] Result<ParallaxWarpGrid> blendParallaxGrids(const ParallaxWarpGrid& from, const ParallaxWarpGrid& to,
                                                          double t);

/// A grid of `like`'s layout (size, latitude span, diagnostics) that corrects
/// nothing: every (dLon, dLat) is 0.  A refused bucket stands for it when a
/// neighbour's grid glides into or out of it, since blendParallaxGrids needs
/// one layout on both sides.  An invalid `like` gives an invalid copy.
[[nodiscard]] ParallaxWarpGrid zeroParallaxGridLike(const ParallaxWarpGrid& like);

// ---------------------------------------------------------------------------
//  The seam table's glide between two buckets
// ---------------------------------------------------------------------------
//
// Two buckets' seam tables (SeamProfile::shiftDeg, one disparity per column)
// glide like the grids do - but a column glides only while the two tables
// AGREE.  The glide exists to hide measurement noise at a bucket edge; where
// the scene at the seam has changed between the two anchors (a near object
// came or went), the older table is simply wrong for the new content, and
// gliding keeps it on screen for most of the bucket.
//
// Measured on consecutive anchors (8 frames apart), |change| per column:
//     the maintainer's aerial sample   median 0.03 deg, 90th pct 0.24-0.52 deg
//     a user's car-mounted day clip    median 0.12-0.25 deg, 10-25 % of columns over 1 deg
//     the night driving clip           median 0.38-0.68 deg, 22-40 % of columns over 1 deg
// On the night clip a traffic-light pole crossed the seam right after an
// anchor: gliding from the previous table showed it twice for three frames,
// where stepping (the behaviour before the glide) showed it once.
//
// So per column the glide weight t rises to 1 between kSeamTableGlideNoiseDeg
// (below it the change is noise: a pure glide) and kSeamTableStepDeg (above
// it the column steps to the new table at the anchor, as before the glide):
//     t' = t + (1 - t) * smoothstep(noise, step, |to - from|) * g
// A table changes smoothly along longitude (the robust smoother in
// searchSeam, SeamAnalysis.h), so t' does too, and the stepped and glided
// columns meet without a seam of their own.  0.5 deg (2.8 px of a 2048 px
// equirect) is the top of the aerial sample's bucket-to-bucket noise, so its
// tables glide as before; 1.5 deg (8.5 px) of stale shift is what doubled
// the pole.  Measured with the step metric (mean frame-to-frame change at
// bucket edges over the other frames, seam band) with parallax off, plain
// glide -> this rule -> stepping:
//     day clip 3000-3095     1.25 -> 1.77 -> 2.70
//     night clip 1200-1295   0.88 (the pole doubled) -> 1.03 -> 1.12
// and the pole and a lamp arm are single again from the anchor on.  A
// narrower band (0.25 / 0.75 deg) stepped more of the day clip (2.11) for no
// visible gain on the night one.
//
// g is the CONFIDENCE gate: smoothstep(kSeamTableStepConfLo,
// kSeamTableStepConfHi, min(confidence of the two columns)), with each
// table's per-column confidence from SeamProfile::confidence.  A large
// change between two confident measurements is a real one (the pole above)
// and still steps; a large change where either measurement is unsure is
// matching noise and glides.  Measured on the car-mounted day clip (LRF
// 5872-5920) before the gate: 16 % of the columns jumped at every bucket
// start (p99 3.4 deg in one frame, 0 % between bucket starts), and 99.9 % of
// the columns that stepped had a confidence below 0.1 in one of the two
// anchors.

/// Below this per-column change (degrees) two buckets' tables glide fully.
inline constexpr double kSeamTableGlideNoiseDeg = 0.5;
/// Above this per-column change (degrees) the column steps to the new table.
inline constexpr double kSeamTableStepDeg = 1.5;
/// Below this confidence (the smaller of the two columns') a change never steps.
inline constexpr double kSeamTableStepConfLo = 0.3;
/// From this confidence on a large change steps fully.
inline constexpr double kSeamTableStepConfHi = 0.6;

/// Glide two seam tables for one frame into `out` (its capacity is reused).
///
/// Per column: from + (to - from) * t', with t' the agreement-weighted glide
/// weight above (`t` clamped to [0, 1]; a non-finite t counts as 1, the
/// newer table).  A missing (null or empty) side is an all-zero table - no
/// shift - so a table fades in from, or out to, the uncorrected geometry, and
/// a large column of it steps instead.  Tables of two lengths cannot be mixed
/// column by column: `to` alone then, as if `from` were missing.  Non-finite
/// entries of either side count as 0, and the output is always finite.
/// `out` is left empty when neither side has a table.  `noiseDeg` >=
/// `stepDeg` (or a non-finite threshold) disables the agreement test: a plain
/// linear glide.
///
/// `fromConf` / `toConf` (optional) are the two tables' per-column
/// confidences (SeamProfile::confidence) and gate the step: a column steps
/// only as far as smoothstep(kSeamTableStepConfLo, kSeamTableStepConfHi,
/// min(fromConf, toConf)) allows.  A side without a confidence vector (null,
/// empty, or not the table's length) counts as fully confident - the
/// behaviour before the gate - so a missing TABLE side, which is the exact
/// "no shift", leaves the gate to the other side's confidence.  A non-finite
/// confidence counts as 0 (glide), entries are clamped to [0, 1].
void blendSeamTables(const std::vector<float>* from, const std::vector<float>* to, double t, std::vector<float>& out,
                     double noiseDeg = kSeamTableGlideNoiseDeg, double stepDeg = kSeamTableStepDeg,
                     const std::vector<float>* fromConf = nullptr, const std::vector<float>* toConf = nullptr);

/// Convert a band flow field into the angular grid, without rendering.
///
/// Split out from buildParallaxWarp so the geometry - band rows to latitude,
/// pixels to radians, the decay ring, the anisotropy - can be tested against
/// a synthetic flow field with no clip, no decoder and no renderer involved.
///
/// `bands` supplies the band geometry (size, row offset, map height) and
/// `flow` the measured field, which must match the band's dimensions.
///
/// `pool` (optional) splits the two per-pixel passes - the accumulation and
/// the benefit gate, ~8 ms single-threaded on a 2048 x 68 band - by grid row.
/// Each cell still sums its pixels in the sequential order, so the grid is
/// bit-identical with or without a pool, on any number of threads.
///
/// `cells` (optional) receives the raw per-cell measurement before the grid
/// is shaped (ParallaxCellStats); the grid is identical with or without it.
///
/// The grid's pixel counts (consistent, structured - see
/// ParallaxWarpParams::minStructureGradient) are filled here, but no gate is
/// applied: the returned grid always has strength 1, and refusal and
/// strength are parallaxFromBands' decisions.  Bands without luma planes of
/// the band's size count no structured pixel.
[[nodiscard]] Result<ParallaxWarpGrid> gridFromFlow(const LensBands& bands, const BidirFlow& flow,
                                                    const ParallaxWarpParams& params, ThreadPool* pool = nullptr,
                                                    ParallaxCellStats* cells = nullptr);

}  // namespace osv::render
