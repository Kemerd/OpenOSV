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
// once and why osvShiftTowardAxis is reused verbatim for the meridian
// component.
//
// THE TWO COMPONENTS
// ------------------
// A ray in the overlap needs a correction with two degrees of freedom, and
// the kernel has exactly two rotations available at a given lens:
//
//   v  along the meridian through the lens axis - the angle FROM the axis.
//      This is what osvShiftTowardAxis does and what the seam table drives.
//      It is the epipolar direction: on a back-to-back rig, parallax is
//      mostly along it.
//
//   u  about the lens axis - the angle AROUND it.  No 1-D table can express
//      this, and it is what leaves a wing tip smeared when only v is
//      corrected.
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
// The same asymmetry is applied here by scaling the two components
// differently before they become angles, and by smoothing the cross-meridian
// component harder.  See crossMeridianScale and crossMeridianSmooth.
//
// WHAT IS NOT DONE
// ----------------
// TEMPORAL FILTERING IS NOT IMPLEMENTED.  Jump's observation is that viewers
// forgive consistent ghosting but notice ghosting that CHANGES frame to
// frame, and DJI's stitcher filters its flow over time for it.  This is a
// per-frame correction with no memory, so on moving footage the correction
// will shimmer where the flow is marginal.  The interval control
// (`osvtool render --seam-interval`) reduces the visible shimmer by holding
// one grid across several frames, which is a blunt instrument rather than a
// filter.  Named here rather than hidden; it is the obvious next step.

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

    /// Scale applied to the cross-meridian (u) component, in [0, 1].
    ///
    /// The anisotropy the header describes.  Parallax on a back-to-back rig
    /// is mostly along meridians; a large perpendicular component is more
    /// often a mismatch than real motion, so it is trusted less rather than
    /// discarded - discarding it would give back exactly the 1-D behaviour
    /// this file exists to improve on.
    double crossMeridianScale = 0.5;

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
};

/// The grid the kernel samples, plus what was measured while building it.
struct ParallaxWarpGrid {
    std::uint32_t w = 0;         ///< Columns (longitude, wraps).
    std::uint32_t h = 0;         ///< Rows (latitude), including the decay rings.
    float latMinRad = 0.0f;      ///< Latitude of row 0.
    float latMaxRad = 0.0f;      ///< Latitude of row h - 1.
    /// Interleaved (u, v) half-corrections in RADIANS, w * h pairs.
    std::vector<float> uv;

    // ---- diagnostics -------------------------------------------------------
    FlowBackendKind usedBackend = FlowBackendKind::Classical;
    std::uint64_t consistentPixels = 0;  ///< Flow pixels that passed the check.
    std::uint64_t totalPixels = 0;       ///< Flow pixels examined.
    double meanAbsCorrectionDeg = 0.0;   ///< Mean |correction| over the measured rows.
    double maxAbsCorrectionDeg = 0.0;    ///< Largest |correction| after clamping.

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
/// the two compose instead of double-counting the same disparity.
///
/// Returns Unsupported when the flow was measured but too little of it passed
/// the consistency check (see minConsistentFraction) - a normal outcome on
/// featureless content such as open sky, which the caller handles by
/// rendering uncorrected.  Returns InvalidArgument for malformed parameters.
[[nodiscard]] Result<ParallaxWarpGrid> buildParallaxWarp(const geom::LensRig& rig, const video::FramePair& frames,
                                                         const geom::BlendParams& blend,
                                                         const ParallaxWarpParams& params,
                                                         const std::vector<float>* seamTable, ThreadPool& pool);

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
