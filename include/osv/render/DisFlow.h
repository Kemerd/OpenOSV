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
/// The defaults are the paper's own "DIS-Fast" operating point adapted to a
/// narrow overlap band, NOT DJI's numbers - those live in their binary's data
/// section and are being recovered separately.  Every one of them is a named
/// field precisely so a reversed constant can be dropped in without touching
/// the solver, so treat these as a starting point and not as gospel.
struct DisFlowParams {
    /// Coarsest-to-finest pyramid levels actually solved.  The paper uses 5
    /// for 1024-wide images; a band a few hundred rows tall cannot support
    /// that many halvings, so buildPyramid() also stops when a level would
    /// fall below kMinPyramidEdge whatever this says.
    int levels = 4;

    /// Patch side in pixels at every level (the paper's theta_ps = 8).
    int patchSize = 8;

    /// Stride between patch origins, as a fraction of patchSize.  The paper's
    /// theta_ov = 0.3 overlap means a stride of 0.7 * patchSize; expressed as
    /// a stride so the grid maths has no rounding surprise.
    double patchStrideFraction = 0.5;

    /// Inverse-search iterations per patch per level (theta_it = 12).
    int iterations = 12;

    /// Give up on a patch once its step is smaller than this, in pixels.
    /// Purely a speed guard: the remaining motion is below what the densify
    /// step can represent anyway.
    double minStepPx = 0.01;

    /// Reject a patch whose structure tensor is near-singular.  A patch on
    /// flat sky has no gradient to match on, and inverting its tensor
    /// amplifies noise into a large bogus displacement.  Compared against the
    /// tensor's determinant normalised by its trace squared.
    double minTensorDet = 1e-6;

    /// Largest displacement any single patch may report, in pixels, per
    /// level.  The overlap band is narrow and the true disparity is a few
    /// pixels; a patch that claims 50 has mismatched, and letting it through
    /// tears the warp.
    double maxDisplacementPx = 24.0;

    /// Gaussian sigma for the flow smoothing pass that follows densification,
    /// in pixels, or <= 0 to skip it.  DJI's stitcher smooths its flow as
    /// well, so this stage is theirs as much as the paper's.
    double smoothSigmaPx = 1.5;

    /// Discard a flow vector whose forward and backward estimates disagree by
    /// more than this many pixels (the standard forward-backward consistency
    /// check).  Only used by disFlowBidirectional().
    double consistencyTolPx = 1.5;
};

/// Below this width or height a pyramid level is not built: the patch grid
/// would have fewer than two patches per axis and the solve degenerates.
inline constexpr std::uint32_t kMinPyramidEdge = 16;

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
/// Both images must be the same non-zero size.  `pool` parallelises the patch
/// solve and the densify across rows of the patch grid; nullptr runs on the
/// calling thread.
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
void smoothFlow(FlowField& flow, double sigmaPx);

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
