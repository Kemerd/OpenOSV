// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlowWarp.h - warp two overlapping views together using a flow field, and
// blend them without a visible seam.
//
// WHERE THIS SITS
// ---------------
// DisFlow.h measures WHERE the two lenses disagree.  This file acts on that
// measurement.  Together they are the parallax correction the 1-D seam search
// cannot do:
//
//     renderLensBands   ->  disFlowBidirectional  ->  warpToMiddle
//     (two views of the      (how far each pixel       (bend both halfway
//      overlap band)          disagrees)                and blend)
//
// WHY WARP BOTH VIEWS HALFWAY
// ---------------------------
// The obvious thing is to warp one view onto the other and keep the second
// as the reference.  That is wrong for a symmetric two-lens rig, for two
// reasons.  Geometrically, it moves the whole error budget onto one lens, so
// the seam is sharp on the reference side and smeared on the warped side -
// which reads as a visible discontinuity exactly where the eye is drawn.
// Perceptually, a full-magnitude warp of one image distorts real geometry by
// the entire disparity, whereas warping each halfway distorts each by half,
// and two small distortions that meet in the middle are far harder to see
// than one large one.
//
// DJI's stitcher does the same: its flow warp takes two flow fields - the
// flow is bidirectional.  A one-sided warp needs one flow field, not two.
//
// HOW THE MIDDLE IS DEFINED, AND WHICH FLOW WARPS WHICH VIEW
// ----------------------------------------------------------
// The resampling is a BACKWARD map: out(p) = src(p + flow(p) * t).  That is
// the only form a gather-based warp can take - each output pixel reads one
// source position - and it fixes which field belongs to which view.
//
// To move view A toward the middle, each output pixel of A must read from
// where that content sits in A.  The field that points from the middle back
// into A is the one measured FROM B TO A.  So:
//
//     warpedA = warpByFlow(A, flowBtoA, t)
//     warpedB = warpByFlow(B, flowAtoB, 1 - t)
//
// with t = 0.5 for the symmetric case.  Each view is warped by the OTHER
// direction's flow.  Getting this backwards produces a warp of the right
// magnitude in the wrong direction, which doubles the disparity instead of
// cancelling it - and it still looks superficially plausible, which is why
// warpByFlow is exposed and pinned by its own test.
//
// Verified against Facebook's Surround360 (NovelViewUtil::combineNovelViews,
// surround360_render/source/optical_flow/NovelView.cpp), which does exactly
// this:
//
//     outNovelViewFromL = generateNovelViewSimpleCvRemap(imageL, flowRtoL, shiftFromL);
//     outNovelViewFromR = generateNovelViewSimpleCvRemap(imageR, flowLtoR, 1.0 - shiftFromL);
//
// Note that its blend weights are ALSO complementary to its warp fractions
// (it passes 1 - shiftFromL for the L view): a view warped a long way is
// trusted less, because it has been distorted more.  blendWeightForRow()
// keeps that coupling.
//
// This is "novel view synthesis at t = 0.5" in the panoramic-stitching
// literature; the same construction appears in Anderson et al., "Jump:
// Virtual Reality Video" (SIGGRAPH Asia 2016).
//
// DEGHOSTING
// ----------
// A plain cross-fade ghosts wherever the flow was wrong, because it averages
// two views that disagree.  Surround360's answer, which this file follows, is
// to make the blend adaptive: measure the colour difference between the two
// warped views and, where it is large, sharpen the blend from a linear ramp
// into a near-selection of one view.  Averaging is right where the views
// agree; picking one is right where they do not.  See DeghostParams.

#pragma once

#include "osv/core/Result.h"
#include "osv/render/DisFlow.h"

#include <cstdint>
#include <vector>

namespace osv::render {

/// An RGBA image with float channels, the unit the warp works on.
///
/// Four channels interleaved rather than planar: the warp reads all four at
/// one bilinear position, so interleaved is one cache line instead of four
/// separate streams.  DisFlow's GrayImage is planar for the opposite reason -
/// it walks one component at a time.
struct RgbaImage {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<float> data;  ///< Row-major, 4 floats per pixel (R,G,B,A).

    [[nodiscard]] bool valid() const noexcept {
        return w > 0 && h > 0 && data.size() == static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    }

    /// Allocate w x h and clear to transparent black.
    void resize(std::uint32_t width, std::uint32_t height);

    /// Bilinear sample, clamp-to-edge, writing four channels to `out`.
    /// Writes zeroes for an empty image or a non-finite coordinate.
    void sample(float x, float y, float out[4]) const noexcept;
};

/// Tuning for the warp and the blend that follows it.
struct FlowWarpParams {
    /// How far along the flow each view is moved, in [0, 1].
    ///
    /// 0.5 is the symmetric middle and the default.  0 disables the warp
    /// entirely (useful as an A/B against the unwarped result), and 1 warps
    /// view A all the way onto view B, which is the asymmetric case this
    /// file's header argues against but which remains reachable for testing.
    double warpFraction = 0.5;

    /// Feather width of the cross-fade between the two warped views, as a
    /// fraction of the band height, in (0, 1].
    ///
    /// The blend runs across the band's short axis, because that is the
    /// direction the seam crosses.  A wider feather hides a larger residual
    /// but smears more real detail, and past about a third of the band it
    /// starts mixing content the two lenses genuinely disagree about.
    double featherFraction = 0.25;

    /// Where the cross-fade is centred across the band, in [0, 1].
    ///
    /// 0.5 means the geometric middle of the overlap.  A seam search that has
    /// placed the seam off-centre moves this to follow it, which is why it is
    /// a parameter and not the constant 0.5.
    double seamPosition = 0.5;

    /// Pixels whose flow failed the forward-backward consistency check are
    /// warped by this fraction of the (repaired) flow instead of the full
    /// `warpFraction`, in [0, 1].
    ///
    /// Not zero by default, and not one either.  Zero would leave an
    /// unwarped island inside a warped region - a hard edge exactly where
    /// the measurement was least certain.  One would trust a vector that
    /// failed its own consistency test.  Half of the usual warp is the
    /// honest compromise: the repaired flow came from neighbours that DID
    /// pass, so it is plausible but not measured.
    double unreliableWarpScale = 0.5;

    /// Re-smooth the flow after masking and repairing, sigma in pixels, or
    /// <= 0 to skip.  Repair fills holes from neighbours, which leaves a
    /// faint seam at the hole boundary; a light blur removes it.
    double postRepairSmoothSigmaPx = 1.0;

    /// How hard the blend switches from averaging to selecting where the two
    /// warped views disagree.  0 disables deghosting entirely (a plain
    /// cross-fade); larger values select more aggressively.
    ///
    /// The mechanism, and why it is needed: a cross-fade of two views that
    /// disagree is a double image.  Scaling the colour difference through a
    /// tanh gives a per-pixel "how wrong is the flow here" signal in [0, 1],
    /// and that signal interpolates the blend weight from the geometric ramp
    /// toward a softmax that picks the better-supported view.  Where the
    /// views agree the tanh is ~0 and the blend is the ordinary ramp, so
    /// nothing is lost in the easy case.
    ///
    /// Surround360's combineNovelViews uses kColorDiffCoef = 10 for this and
    /// kSoftmaxSharpness = 10 below; those are the starting values here.
    double colorDiffCoef = 10.0;

    /// Sharpness of the selection softmax, i.e. how decisively it picks one
    /// view once colorDiffCoef has decided the flow is untrustworthy.
    double softmaxSharpness = 10.0;
};

/// The result of warping and blending one overlap band.
struct WarpedBand {
    RgbaImage blended;              ///< The merged band.
    RgbaImage warpedA;              ///< View A at the middle position.
    RgbaImage warpedB;              ///< View B at the middle position.
    std::uint64_t reliablePixels = 0;   ///< Pixels whose flow passed the check.
    std::uint64_t unreliablePixels = 0; ///< Pixels warped at the reduced scale.

    [[nodiscard]] bool valid() const noexcept { return blended.valid(); }
};

/// Warp both views to the middle and blend them.
///
/// `flow.forward` is the field from `a` to `b` and `flow.backward` the field
/// from `b` to `a`; `warpToMiddle` applies each to the OPPOSITE view, as the
/// header explains.  `flow.ok` selects which pixels are warped at full
/// strength; where it is 0 the flow is repaired from neighbours first and
/// then applied at `unreliableWarpScale`.
///
/// `a` and `b` must be the same non-zero size and match the flow fields.
///
/// Returns InvalidArgument for mismatched or empty inputs, or for a
/// parameter outside its documented range.  On success every output image is
/// fully written; there is no partial result.
[[nodiscard]] Result<WarpedBand> warpToMiddle(const RgbaImage& a, const RgbaImage& b, const BidirFlow& flow,
                                              const FlowWarpParams& params);

/// Warp one image along a flow field scaled by `fraction`, with no blending.
///
/// The map is BACKWARD: out(p) = in(p + fraction * flow(p)).  So to move a
/// view toward the other one, pass the flow measured FROM the other view TO
/// this one (see the header).
///
/// Exposed on its own because the tests pin it directly: a warp of the right
/// magnitude in the wrong direction is invisible once two of them have been
/// averaged, and it doubles the disparity instead of cancelling it.
[[nodiscard]] Result<RgbaImage> warpByFlow(const RgbaImage& in, const FlowField& flow, double fraction);

/// Deghosting weight in [0, 1] for one pair of co-located pixels.
///
/// 0 means "the two views agree, blend them linearly"; 1 means "they
/// disagree, pick one".  A pure function of the two colours and the
/// coefficient so the tests can pin its shape without building an image.
[[nodiscard]] double deghostWeight(const float a[4], const float b[4], double colorDiffCoef) noexcept;

/// Cross-fade weight for a band row, in [0, 1], where 0 selects view A.
///
/// Pure function of geometry, so the tests can pin the ramp without building
/// an image: `row` and `height` describe the band, and the two parameters are
/// as documented on FlowWarpParams.  A smoothstep rather than a linear ramp,
/// because a linear cross-fade has a visible slope discontinuity at both ends
/// of the feather where its derivative jumps.
[[nodiscard]] double blendWeightForRow(int row, int height, double seamPosition, double featherFraction) noexcept;

}  // namespace osv::render
