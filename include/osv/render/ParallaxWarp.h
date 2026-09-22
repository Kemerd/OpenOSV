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
// TEMPORAL FILTERING IS NOT IMPLEMENTED.  Jump's observation is that viewers
// forgive consistent ghosting but notice ghosting that CHANGES frame to
// frame, and DJI's stitcher filters its flow over time for it.  This is a
// per-frame correction with no memory, so on moving footage the correction
// can shimmer where the flow is marginal.  `osvtool render --seam-interval`
// holds one grid across several frames, which is a blunt instrument rather
// than a filter; the Premiere importer measures every non-draft frame on its
// own and caches the grid by frame index, which keeps a revisited frame
// deterministic but does nothing for frame-to-frame stability.  A filter
// there also has to cope with Premiere asking for frames out of order, so
// the previous frame is not generally at hand.  Named here rather than
// hidden; it is the obvious next step.
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

    /// Fraction of the band's flow that must pass the forward-backward
    /// consistency check for the result to be used at all, in [0, 1].
    ///
    /// Below this the measurement is not trustworthy enough to act on and
    /// the grid is reported as unusable, so the caller renders uncorrected
    /// rather than applying a field that is mostly repaired guesses.
    double minConsistentFraction = 0.25;

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

    // ---- diagnostics -------------------------------------------------------
    FlowBackendKind usedBackend = FlowBackendKind::Classical;
    std::uint64_t consistentPixels = 0;  ///< Co-visible pixels whose flow passed the check.
    std::uint64_t totalPixels = 0;       ///< Co-visible pixels examined.
    double bandMs = 0.0;                 ///< Time spent rendering the two lens bands.
    double flowMs = 0.0;                 ///< Time spent in the flow backend.
    double gridMs = 0.0;                 ///< Time spent turning flow into the grid.
    std::uint32_t measuredCells = 0;     ///< Grid cells that received consistent flow.
    std::uint32_t gatedCells = 0;        ///< Of those, cells the benefit gate switched fully off.
    double meanAbsCorrectionDeg = 0.0;   ///< Mean FULL disparity corrected, measured rows (deg).
    double maxAbsCorrectionDeg = 0.0;    ///< Largest FULL disparity corrected, after clamping (deg).

    [[nodiscard]] bool valid() const noexcept {
        return w > 0 && h > 0 && uv.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 2u;
    }

    /// Fraction of flow pixels that passed the consistency check, in [0, 1].
    [[nodiscard]] double consistentFraction() const noexcept {
        return totalPixels ? static_cast<double>(consistentPixels) / static_cast<double>(totalPixels) : 0.0;
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
/// band overlap NCC 0.902-0.909 against 0.916-0.921), so the importer and
/// `osvtool render` pass nullptr and use the grid INSTEAD of the table,
/// keeping the table only as the fallback when a grid is refused.
///
/// Returns Unsupported when the flow was measured but too little of it passed
/// the consistency check (see minConsistentFraction) - a normal outcome on
/// featureless content such as open sky, which the caller handles by
/// rendering uncorrected.  Returns InvalidArgument for malformed parameters.
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
/// including Unsupported for a measurement too inconsistent to use.
[[nodiscard]] Result<ParallaxWarpGrid> parallaxFromBands(const LensBands& bands, const ParallaxWarpParams& params,
                                                         ThreadPool* pool, double bandMs = 0.0);

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

/// Convert a band flow field into the angular grid, without rendering.
///
/// Split out from buildParallaxWarp so the geometry - band rows to latitude,
/// pixels to radians, the decay ring, the anisotropy - can be tested against
/// a synthetic flow field with no clip, no decoder and no renderer involved.
///
/// `bands` supplies the band geometry (size, row offset, map height) and
/// `flow` the measured field, which must match the band's dimensions.
[[nodiscard]] Result<ParallaxWarpGrid> gridFromFlow(const LensBands& bands, const BidirFlow& flow,
                                                    const ParallaxWarpParams& params);

}  // namespace osv::render
