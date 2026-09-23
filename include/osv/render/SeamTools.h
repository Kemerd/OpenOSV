// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SeamTools.h - the user's tweaks of the carved seam: how wide it blends,
// how softly it hands a near object from one lens to the other, and how far
// it nudges near and far content along the seam.  [WP-SEAMTOOLS]
//
// ===========================================================================
//  WHY
// ===========================================================================
// The carved seam (SeamCarve.h) picks where the two lenses meet and a
// feather per column: wide (1.5 deg) where they agree, narrow (0.35 deg)
// where they do not.  That is right for most of the sphere, but a near
// object - on the sample clip the engine nacelle a few centimetres from the
// camera - is seen from two viewpoints, and at the cut its two copies step.
// The user asked for tools, not a new default: "the seam we have right now
// is not bad ... just add tools in the effects panel to tweak it".  So every
// control here defaults to exactly today's render, and each one changes only
// the overlap band.
//
// ===========================================================================
//  THE FIVE CONTROLS (Source Settings > Stitching)
// ===========================================================================
//   Seam Blend       SeamCarveParams::wideHalfWidthDeg - the feather where
//                    the lenses agree.  Default 1.5 deg.
//   Parallax Blend   SeamCarveParams::narrowHalfWidthDeg - the feather where
//                    they disagree.  Default 0.35 deg; never wider than Seam
//                    Blend (the carve's own rule), 0 = a hard cut.
//   Seam Smoothing   a two-band blend (Burt & Adelson 1983, as DJI's
//                    stitcher blends): the LOW frequencies of both
//                    lenses mix over this half width, the high frequencies
//                    still switch at the carved seam.  A step on a near
//                    object becomes a soft gradient instead of a double
//                    image.  Default 0 = off.
//   Near Offset      a shift ALONG the seam (polar longitude: rotation about
//   Far Offset       the lens axis, up / down on screen where the seam runs
//                    vertically past the camera's side), applied where the
//                    lenses disagree (near content) / agree (far content),
//                    weighted per column by BlendSeam::nearWeight.  Default
//                    0 / 0 = none.
//
// Neither width moves the seam: the carve measures its cost over its own
// window (SeamCarveParams::costWindowDeg), so the widths change only how
// wide the lenses mix.
//
// ===========================================================================
//  THE LOW BAND (Seam Smoothing)
// ===========================================================================
// A real low band needs PRE-FILTERED images: WP-SEAM measured that a low-pass
// with a per-pixel footprint (9 taps, ~1.5 lens px) is no better than a wider
// feather, because it is not low enough to exclude a fin-sized object.  So
// each frame, per lens, the renderer builds a low-resolution copy
// (osv_kernel.h, OsvRenderParams [WP-SEAMTOOLS]):
//
//   1. 8 x 8 blocks of the fisheye box-averaged in code space and decoded
//      once (linear light; code values for passthrough), weighted by the
//      share of the block inside the lens's usable circle and outside the
//      selfie stick;
//   2. a separable Gaussian (sigma = kSeamLowSigmaPerHalfWidth x the
//      smoothing width, less the box and the bilinear lookup's own blur);
//   3. looked up bilinearly per pixel and un-premultiplied (a normalised
//      convolution), so the black beyond the image circle never bleeds in.
//
// On the GPU (NVDEC frames: the importer's GPU path and the direct path's
// engine) that is three small kernels on a 375 x 375 x 2 table; on the host
// (host frames) the same per-pixel functions on the thread pool.
//
// ===========================================================================
//  THE OFFSET (Near / Far Offset)
// ===========================================================================
// Physical parallax on a back-to-back rig runs along meridians, which the
// parallax grid corrects where its flow is trustworthy.  What the user sees
// at the nacelle is a step ALONG the seam: the grid's benefit gate switches
// itself off on that shiny, texture-poor surface, and the flow finds a small
// genuine cross-meridian misalignment around the ring anyway (ParallaxWarp.h,
// crossMeridianScale).  On the sample the seam runs vertically past the
// nacelle on a level view, so "up / down on screen" is exactly this
// direction.  The offset is added to the dLon component of the 2-D warp
// grid - the parallax grid when there is one, a grid of its own when not -
// over the overlap rows, fading to zero at the grid's latitude edges like
// the parallax grid's own decay ring, so it moves nothing outside the band.
//
// SIGN: a positive offset turns the MASTER lens's content (lens 1, the +Y
// axis, the front lens) by +offset / 2 in polar longitude and the slave's
// (the rear lens) by -offset / 2 - seen looking out through the master lens,
// its side of the seam turns clockwise (polar longitude = atan2(x, z) grows
// clockwise from the body's +Z through +X in that view).  On the sample
// clip, where the seam runs down the camera's side past the engine nacelle,
// a positive offset moves the REAR lens's side of the seam - the nacelle
// body, the "wingtip" that "needs to go down" - DOWN on a level view and
// the front lens's side (the spinner) up: measured, +1 deg = 18.8 px down /
// 19.5 px up at 40 px per degree.  On the opposite side of a camera the same
// rotation reads the other way round, which is why it is defined on the
// lenses, not on the screen.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamCarve.h"
#include "osv/render/osv_kernel.h"

#include <cstddef>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Defaults and ranges (Source Settings, the importer dialog and osvtool
//  all clamp to these)
// ===========================================================================

/// Seam Blend default: SeamCarveParams{}.wideHalfWidthDeg (a test pins it).
inline constexpr double kDefaultSeamBlendDeg = 1.5;
/// Seam Blend range, degrees.
inline constexpr double kMinSeamBlendDeg = 0.2;
inline constexpr double kMaxSeamBlendDeg = 8.0;
/// Parallax Blend default: SeamCarveParams{}.narrowHalfWidthDeg.
inline constexpr double kDefaultParallaxBlendDeg = 0.35;
/// Parallax Blend range, degrees (0 = a hard cut).
inline constexpr double kMaxParallaxBlendDeg = 4.0;
/// Seam Smoothing range, degrees (0 = off, the default).
inline constexpr double kMaxSeamSmoothingDeg = 8.0;
/// Near / Far Offset range, degrees either way (0 = none, the default).
inline constexpr double kMaxSeamOffsetDeg = 3.0;

/// Lens pixels per low-band pixel.  8 turns a 3000 x 3000 fisheye into a
/// 375 x 375 low band - one texel ~0.55 deg at the rim, finer than the
/// narrowest smoothing worth asking for - and keeps the whole two-lens table
/// at 4.5 MB.
inline constexpr int kSeamLowFactor = 8;

/// Gaussian sigma of the low band per degree of smoothing half width.  A band
/// blended over a transition of half width W is only free of visible double
/// edges when its content varies more slowly than ~W (Burt & Adelson 1983:
/// transition width ~ the band's wavelength, a Gaussian's ~6 sigma across),
/// so sigma = W / 3.
inline constexpr double kSeamLowSigmaPerHalfWidth = 1.0 / 3.0;

/// The core of the latitude profile the offset is applied with: full
/// strength within this many degrees of the geometric seam (the parallax
/// grid's measured rows), fading to zero at the grid's outer rows.
inline constexpr double kSeamOffsetCoreDeg = 6.0;

/// The five controls, in degrees.  Default-constructed = today's render.
struct SeamTools {
    double seamBlendDeg = kDefaultSeamBlendDeg;          ///< Feather where the lenses agree.
    double parallaxBlendDeg = kDefaultParallaxBlendDeg;  ///< Feather where they disagree.
    double smoothingDeg = 0.0;                           ///< Low-band blend half width; 0 = off.
    double nearOffsetDeg = 0.0;                          ///< Along-seam shift where they disagree.
    double farOffsetDeg = 0.0;                           ///< Along-seam shift where they agree.

    /// True when the smoothing is on (a finite width above zero).
    [[nodiscard]] bool smoothingOn() const noexcept;
    /// True when either offset is non-zero (and finite).
    [[nodiscard]] bool offsetOn() const noexcept;
};

/// Put the two blend widths into carve parameters, clamped to their ranges
/// and with Parallax Blend never above Seam Blend (the carve refuses a
/// narrow feather wider than the wide one).  Non-finite values leave the
/// parameter at its default.  Default tools leave `params` bit-identical.
void applySeamBlendWidths(const SeamTools& tools, SeamCarveParams& params) noexcept;

// ===========================================================================
//  Seam Smoothing: the low band
// ===========================================================================

/// Fill the [WP-SEAMTOOLS] fields of `p` for a smoothing half width of
/// `halfWidthDeg` (the lens blocks must already be filled: the low band's
/// size and its blur in low-band pixels follow from the lens geometry).
/// `sigmaDeg` < 0 picks kSeamLowSigmaPerHalfWidth x halfWidthDeg.  A width
/// that is not above zero, not finite, or lenses without a usable focal
/// length clear the fields instead (smoothing off).
void fillSeamSmoothParams(OsvRenderParams& p, double halfWidthDeg, double sigmaDeg = -1.0) noexcept;

/// Floats of the two-lens low-band table the fields of `p` describe
/// (2 x seamLowW x seamLowH x 4), or 0 when smoothing is off or the shape is
/// implausible.
[[nodiscard]] std::size_t seamLowTableFloats(const OsvRenderParams& p) noexcept;

/// True when the [WP-SEAMTOOLS] fields of `p` describe a table a kernel can
/// build and read safely: an even factor, a low band that covers every
/// enabled lens's frame, a radius within OSV_SEAM_LOW_MAX_RADIUS, finite
/// non-negative taps with a positive centre and a finite half width.  Also
/// true when smoothing is off.
[[nodiscard]] bool seamSmoothParamsValid(const OsvRenderParams& p) noexcept;

/// Build the low-band table on the host from HOST planes (the three stages
/// above, rows in parallel on `pool`, inline when it is null or refuses).
/// `out` receives seamLowTableFloats(p) floats; `scratch` is a reused
/// buffer.  InvalidArgument when smoothing is off, the fields are malformed
/// or a plane of an enabled lens is empty.
[[nodiscard]] Status buildSeamLowBand(const OsvRenderParams& p, const OsvPlane planes[2], std::vector<float>& out,
                                      std::vector<float>& scratch, ThreadPool* pool);

// ===========================================================================
//  Near / Far Offset
// ===========================================================================

/// The warp grid with the Near / Far Offset added to its dLon component.
///
/// `base` is the parallax grid in force (nullptr or invalid: a zero grid of
/// the default parallax geometry is made); `seam` the carved seam in force,
/// whose nearWeight picks near vs far per column (a seam without one counts
/// every column as far).  Per grid cell the relative shift is
/// near x w + far x (1 - w), degrees, of which the master takes -half as its
/// sampling displacement (its content moves by +half; see SIGN in the
/// header), times the latitude profile (1 within kSeamOffsetCoreDeg,
/// smoothstep to 0 at the outer rows).
///
/// InvalidArgument for non-finite offsets or a malformed seam; the offsets
/// are clamped to kMaxSeamOffsetDeg.
[[nodiscard]] Result<ParallaxWarpGrid> seamOffsetGrid(const ParallaxWarpGrid* base, const BlendSeam& seam,
                                                      double nearDeg, double farDeg);

}  // namespace osv::render
