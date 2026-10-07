// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DisFlow.h - dense optical flow by Dense Inverse Search.
//
// WHAT THIS IS AND WHY IT EXISTS
// ------------------------------
// The stitcher aligns the two fisheye views with a per-column 1-D disparity
// (render::searchSeam).  That corrects a shift along each meridian and nothing
// else, so wherever the parallax is two-dimensional - a wing tip, a propeller,
// anything close to the camera - the seam bends the picture into the wavy
// vertical bands the user reported.
//
// Fixing that needs a dense 2-D correspondence field across the overlap, which
// is what this file computes.  The algorithm is:
//
//     T. Kroeger, R. Timofte, D. Dai, L. Van Gool,
//     "Fast Optical Flow using Dense Inverse Search",
//     ECCV 2016.  https://arxiv.org/abs/1603.03590
//
// chosen because it is the algorithm DJI's own stitcher uses (patch
// inverse search with a structure-tensor step and neighbourhood averaging,
// i.e. DIS).  Matching the algorithm is the only way to match the output,
// which is the stated bar.
//
// HOW DIS WORKS, IN ONE PARAGRAPH
// -------------------------------
// Build a Gaussian pyramid of both images.  At the coarsest level, lay a grid
// of overlapping square patches over the first image.  For each patch, solve
// for the translation that best matches the second image by inverse search:
// instead of re-warping the template every iteration (forward search), warp
// the TARGET and use a per-patch inverse Hessian precomputed once from the
// first image's gradients - the structure tensor.  That makes each iteration
// a couple of multiply-adds instead of a re-interpolation, which is where the
// speed comes from.  Then densify: every pixel's flow is the weighted average
// of the patches covering it, weighted by patch match quality.  Optionally
// smooth by variational refinement.  Propagate to the next finer level and
// repeat.
//
// WHAT THIS IMPLEMENTATION DOES NOT DO
// ------------------------------------
// The paper's optional variational refinement stage (its section 3.3, which
// borrows from Brox et al.) is NOT implemented.  It buys accuracy on large
// displacements at several times the cost, and the overlap band here is a
// narrow strip with small disparities.  The gap is named rather than hidden;
// if the reversed DJI constants turn out to imply it, it goes in then.
//
// WHERE THIS DEPARTS FROM THE PAPER, AND WHY
// ------------------------------------------
// Three additions, all measured on car-mounted clips whose body sits a metre
// from the lenses.  The overlap band is only ~68 rows tall, so the pyramid
// stops after three levels and plain coarse-to-fine descent seeded at zero
// cannot reach the ~14 px (2.4 deg) the car body is offset along the
// meridian; on its roof rails the 0.5.0 solver settled near 3 px of the 14.
//
//   1. Tikhonov regularisation of the structure tensor (tensorTikhonov).
//      An edge patch - a roof rail, a horizon - has one strong eigenvalue
//      and one nearly zero, and the raw inverse turns any residual into an
//      unbounded step ALONG the edge, where nothing constrains it.  Adding a
//      tenth of the mean eigenvalue to the diagonal bounds that step while
//      leaving a well-textured patch's solve essentially unchanged.
//
//   2. A 1-D search along the band rows before the descent
//      (epipolarSearchPx).  For a back-to-back pair the epipolar direction
//      in the band is the band row (the meridian, ParallaxWarp.h), so the
//      large near-field disparity is one-dimensional: each sufficiently
//      textured patch on the coarse levels tries every whole-pixel offset
//      along v and adopts the best one only when it is a LOCATED minimum
//      (a scored candidate on both sides of it, never the end of the
//      range) and clearly the best - better than the seed by an absolute
//      margin, AND by Lowe's ratio against both the seed and the best
//      candidate two or more pixels away, which is what keeps a railing's
//      repeated bars from luring a patch one period off.  The bracket was
//      added after measuring the night car-mounted clip: a diamond-plate
//      body panel there drew patches to the END of the range, a whole
//      pattern period off, and passed the ratio test doing it.  The
//      restricted search follows Jump's stereo stitcher (Anderson et al.,
//      "Jump: Virtual Reality Video", SIGGRAPH Asia 2016); the ratio test
//      is Lowe's ("Distinctive Image Features from Scale-Invariant
//      Keypoints", IJCV 2004).
//
//   3. Revert instead of kill on runaway (revertOnRunaway).  A patch whose
//      descent wanders past maxDisplacementPx used to be disowned, which on
//      1-D edges threw away patches whose seed was right; it now returns to
//      where the descent started (the seed, or the search's pick) and keeps
//      that, weighted by its residual like any other patch.
//
// With tensorTikhonov 0, epipolarSearchPx 0 and revertOnRunaway false the
// solver is exactly the 0.5.0 one, bit for bit.
//
// Everything is plain C++ with no external dependency: OpenCV has
// cv::DISOpticalFlow and it is the reference this was checked against in
// spirit, but the library must build without it.

#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"

#include <cstdint>
#include <vector>

namespace osv::render {

/// A dense 2-D flow field, one vector per pixel.
///
/// Stored as two separate planes rather than interleaved pairs: every stage
/// that consumes a flow field (smoothing, warping, the confidence pass) walks
/// one component at a time, and splitting them keeps those loops
/// unit-stride.  DJI's own fields are CV_32FC2, i.e. interleaved, which is an
/// interface difference and not an algorithmic one.
struct FlowField {
    std::uint32_t w = 0;    ///< Width in pixels.
    std::uint32_t h = 0;    ///< Height in pixels.
    std::vector<float> u;   ///< Horizontal displacement, w*h, pixels.
    std::vector<float> v;   ///< Vertical displacement, w*h, pixels.

    /// True when the dimensions are non-zero and both planes are the right size.
    [[nodiscard]] bool valid() const noexcept {
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        return w > 0 && h > 0 && u.size() == n && v.size() == n;
    }

    /// Allocate (or reallocate) for w x h and zero the field.
    void resize(std::uint32_t width, std::uint32_t height);

    /// Flow at (x, y), clamped to the edge.  Returns zero for an empty field.
    [[nodiscard]] float atU(int x, int y) const noexcept;
    [[nodiscard]] float atV(int x, int y) const noexcept;
};

/// Tuning for the DIS solver.
///
/// Most of these match the values DJI's own stitcher uses, determined for
/// interoperability (docs/LEGAL.md lists them).
/// Each field says where its default came from.  Four of them - pyramid
/// levels and iteration count among them - keep the paper's operating point
/// and say so at their own declaration.
struct DisFlowParams {
    /// Coarsest-to-finest pyramid levels actually solved.  The paper uses 5
    /// for 1024-wide images; a band a few hundred rows tall cannot support
    /// that many halvings, so buildPyramid() also stops when a level would
    /// fall below kMinPyramidEdge whatever this says.
    int levels = 4;

    /// Patch side in pixels at every level (the paper's theta_ps = 8).
    int patchSize = 8;

    /// Stride between patch origins in PIXELS.
    ///
    /// 5, as DJI's stitcher uses (patch 8, stride 5, border 2, grid
    /// nx = (W - 8) / 5 + 1).  Expressed in pixels rather than as a fraction
    /// of the patch because that is how it is actually specified there, and a
    /// fraction would reintroduce a rounding decision that has already been
    /// made for us.
    int patchStridePx = 5;

    /// Inverse-search iterations per patch per level.
    ///
    /// The paper's theta_it = 12: DJI's iteration count is not known, so this
    /// is the one significant DIS parameter that keeps the paper's value.
    int iterations = 12;

    /// Multiplier on each inverse-search step, in (0, 1].
    ///
    /// 0.4, as DJI's stitcher uses.  Damping the step below 1 trades
    /// convergence speed for stability: the undamped Gauss-Newton step of an
    /// inverse-search iteration overshoots on a patch whose tensor is only
    /// marginally well conditioned, and the overshoot is what produces the
    /// occasional wild vector that a spatial smoothing pass then spreads.
    double stepScale = 0.4;

    /// Give up on a patch once its step is smaller than this, in pixels.
    /// Purely a speed guard: the remaining motion is below what the densify
    /// step can represent anyway.
    double minStepPx = 0.01;

    /// Reject a patch whose structure tensor determinant falls below this.
    ///
    /// 0.001, as DJI's stitcher uses, and compared against the RAW
    /// determinant of the tensor computed on the **8-BIT SCALE** - see
    /// `intensityScale` below, which is what makes that comparison valid.
    ///
    /// An earlier version used a scale-free test (det / trace^2) against
    /// 1e-6, reasoning that a raw threshold conflates contrast with
    /// conditioning.  That reasoning is sound in general but it is not what
    /// DJI does, and matching their output is the requirement, so the raw
    /// test is used and the scale-free variant is gone rather than left as a
    /// dead option.
    double minTensorDet = 0.001;

    /// Reject a patch whose final SSD exceeds this.  1e7, as DJI's stitcher uses,
    /// and likewise on the 8-bit scale.
    double maxPatchSsd = 1.0e7;

    /// Multiplier applied to image values before the solve, so that the
    /// thresholds shared with DJI's stitcher mean what they mean.
    ///
    /// THIS IS NOT COSMETIC.  minTensorDet and maxPatchSsd are absolute
    /// numbers, and a structure tensor determinant scales with the FOURTH
    /// power of the intensity range: measured on a textured test image, one
    /// patch gives det = 9.5e-05 with values in [0, 1] and det = 4.0e+05
    /// with the same image in [0, 255].  The first is rejected by the 0.001
    /// threshold and the second passes comfortably, so feeding [0, 1] data
    /// to DJI's constants rejects EVERY patch and the solver returns a
    /// uniformly zero field - which is exactly what happened when these
    /// constants were first dropped in.
    ///
    /// The library's own images are normalised to [0, 1], DJI's stitcher works
    /// on 8-bit-scaled values, and 255 reconciles the two.  Set it to 1.0
    /// only alongside thresholds derived for a [0, 1] range.
    double intensityScale = 255.0;

    /// Largest displacement any single patch may report, in pixels, per
    /// level.  The overlap band is narrow and the true disparity is a few
    /// pixels; a patch that claims 50 has mismatched, and letting it through
    /// tears the warp.  What becomes of a patch whose descent passes it is
    /// revertOnRunaway's decision.
    double maxDisplacementPx = 24.0;

    /// Gaussian sigma for the flow smoothing pass that follows densification,
    /// in pixels, or <= 0 to skip it.  DJI's stitcher smooths its flow as
    /// well, so this stage is theirs as much as the paper's.
    double smoothSigmaPx = 1.5;

    /// Discard a flow vector whose forward and backward estimates disagree by
    /// more than this many pixels (the standard forward-backward consistency
    /// check).  Only used by disFlowBidirectional().
    double consistencyTolPx = 1.5;

    // ---- Near-field additions (see "WHERE THIS DEPARTS FROM THE PAPER") ----
    // None of these is a DJI constant.  Each was chosen on the car-mounted
    // day and night clips and the 6K sample, measured as the overlap NCC the
    // kernel renders with the resulting grid; 0 / false turns each one off.

    /// Tikhonov term added to both diagonal entries of every patch's
    /// structure tensor before it is inverted, as a fraction of the tensor's
    /// mean eigenvalue: H + (tensorTikhonov * trace(H) / 2) * I.
    ///
    /// Relative to the trace, so it means the same on any contrast and on
    /// any intensityScale.  With 0.1 the inverse's gain along an edge is at
    /// most 20 / trace (21x the across-edge gain on a pure edge) instead of
    /// 1 / lambda_min, which grows without bound as the patch approaches a
    /// pure edge.  Measured on the day clip it raised the consistent fraction
    /// from 0.19 to 0.31.  <= 0 (or non-finite) inverts the raw tensor.
    double tensorTikhonov = 0.1;

    /// Half range of the 1-D search along v (the band row, the epipolar
    /// direction of the back-to-back pair), in FINEST-level pixels; each
    /// pyramid level searches ceil(epipolarSearchPx / 2^level) whole pixels
    /// either side of its seed (see disEpipolarRadius()).  24 matches
    /// maxDisplacementPx, the most any patch may report.  <= 0 disables it.
    ///
    /// Whatever the range, the winning offset must be bracketed: a minimum
    /// found at either end of it (or beside a candidate the cap excluded) is
    /// never adopted, so a level whose radius is 1 can only keep its seed.
    double epipolarSearchPx = 24.0;

    /// Finest pyramid level (0 = full resolution) that runs the search.  1
    /// searches the coarse levels only: their few patches are cheap and
    /// their pick seeds the finer levels.  Searching at level 0 alone (four
    /// times the patches of level 1, twice the radius) was measured to lose
    /// most of the gain.
    int epipolarSearchMinLevel = 1;

    /// Only patches with at least this much texture search: the tensor's
    /// trace BEFORE the Tikhonov term, per pixel of the patch, in
    /// (intensityScale codes)^2.  1.0 keeps flat sky, where every offset
    /// matches equally badly, out of the search.
    double epipolarMinTracePerPx = 1.0;

    /// Lowe's ratio: the best offset is adopted only when its residual is
    /// below this fraction of the seed's AND of the best candidate at least
    /// two pixels away from it.  Without it the 6K sample's OSV frame 60
    /// lost 0.007 NCC to patches that jumped a repeated structure's period.
    /// It is not sufficient on its own: two periods of a real (never quite
    /// periodic) pattern can differ by more than 15 %, which is why the
    /// winner must also be bracketed (see epipolarSearchPx).
    double epipolarRatio = 0.85;

    /// ...and only when it also beats the seed's residual by this many
    /// codes (mean |residual| on the intensityScale range), so sensor noise
    /// alone never moves a patch.
    double epipolarMarginCodes = 0.05;

    /// On a patch whose descent passes maxDisplacementPx, return it to the
    /// position the descent started from and keep it (quality from its
    /// residual as usual) instead of disowning it.  A start position that is
    /// itself beyond the cap is still disowned, so no patch ever reports more
    /// than maxDisplacementPx.
    bool revertOnRunaway = true;
};

/// Below this width or height a pyramid level is not built: the patch grid
/// would have fewer than two patches per axis and the solve degenerates.
inline constexpr std::uint32_t kMinPyramidEdge = 16;

/// Hard ceiling on the 1-D search radius at any level, in whole pixels, so a
/// nonsense epipolarSearchPx cannot turn one patch into an unbounded loop.
inline constexpr int kMaxEpipolarRadius = 64;

/// Whole pixels the 1-D epipolar search covers either side of a patch's
/// seed at pyramid level `level` (0 = finest): ceil(epipolarSearchPx /
/// 2^level), clamped to kMaxEpipolarRadius, or 0 when the level does not
/// search (below epipolarSearchMinLevel, a negative level, or the search
/// disabled / non-finite).
///
/// Shared by the CPU solver and the CUDA port so both search exactly the
/// same candidates.
[[nodiscard]] int disEpipolarRadius(const DisFlowParams& params, int level) noexcept;

/// A single-channel image plane, the input the solver works on.
///
/// The band images the stitcher already produces (render::LensBands) are
/// exactly this shape, so no conversion is needed at the call site.
struct GrayImage {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<float> data;  ///< Row-major w*h.

    [[nodiscard]] bool valid() const noexcept {
        return w > 0 && h > 0 && data.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    }

    /// Bilinear sample with clamp-to-edge addressing.  Returns 0 when empty.
    [[nodiscard]] float sample(float x, float y) const noexcept;

    /// Nearest-pixel fetch with clamp-to-edge.  Returns 0 when empty.
    [[nodiscard]] float at(int x, int y) const noexcept;
};

/// Compute flow from `from` to `to`: the field says, for each pixel of
/// `from`, where that content moved to in `to`.
///
/// Both images must be the same non-zero size.  `pool` parallelises every
/// stage - pyramid, gradients, tensors, the patch solve, densify and the
/// smoothing; nullptr runs on the calling thread.  The field is bit-identical
/// either way (each worker writes only its own rows or patches, and densify
/// sums overlapping patches in the sequential order), so a caller may mix
/// pooled and unpooled solves and still get reproducible results.
///
/// Returns InvalidArgument for mismatched or empty inputs, or for parameters
/// that describe no solvable problem (a non-positive patch size, zero
/// iterations).  Never throws and never leaves a partially written field.
[[nodiscard]] Result<FlowField> disFlow(const GrayImage& from, const GrayImage& to, const DisFlowParams& params,
                                        ThreadPool* pool);

/// Forward and backward flow plus a consistency mask.
///
/// DJI computes both directions - their assertions name flow_l2r and
/// flow_r2l, both CV_32FC2 and the same size - and so does this.  Two
/// one-directional fields are strictly more information than one: where they
/// disagree, neither is trustworthy, and that is precisely where a warp must
/// fall back to the unwarped image rather than invent geometry.
struct BidirFlow {
    FlowField forward;            ///< from -> to.
    FlowField backward;           ///< to -> from.
    std::vector<std::uint8_t> ok; ///< 1 where the two agree, w*h of `forward`.
    std::uint64_t consistent = 0; ///< Count of ok == 1, for diagnostics.

    [[nodiscard]] bool valid() const noexcept {
        return forward.valid() && backward.valid() && ok.size() == forward.u.size();
    }
};

/// Compute both directions and cross-check them.
///
/// A pixel is marked ok when following the forward flow and then the backward
/// flow returns to within `consistencyTolPx` of where it started.  That single
/// test rejects occlusions, the band's own edges and flat regions where the
/// solve was unconstrained, which is most of what goes wrong.
[[nodiscard]] Result<BidirFlow> disFlowBidirectional(const GrayImage& a, const GrayImage& b,
                                                     const DisFlowParams& params, ThreadPool* pool);

/// In-place Gaussian blur of both flow components, sigma in pixels.
///
/// Separable, clamp-to-edge, and a no-op for sigma <= 0 or a field smaller
/// than the kernel.  Exposed because the warp stage wants to re-smooth after
/// it has masked inconsistent vectors out, not only where disFlow() does it.
/// `pool` (optional) splits both passes by rows; the result is bit-identical
/// with or without it.
void smoothFlow(FlowField& flow, double sigmaPx, ThreadPool* pool = nullptr);

/// Replace flow vectors where `ok` is 0 by an average of their valid
/// neighbours, spreading outward until every hole is filled or no progress is
/// possible.
///
/// DJI's stitcher repairs its flow the same way: an inconsistent
/// vector must not simply be zeroed, because zero is itself a claim (that the
/// content did not move) and a zero island inside a moving region shows up as
/// a visible tear.  Borrowing from neighbours is the honest default.
///
/// Returns the number of pixels still unfilled, which is non-zero only when
/// `ok` was entirely 0.
std::uint64_t repairFlow(FlowField& flow, const std::vector<std::uint8_t>& ok);

}  // namespace osv::render
