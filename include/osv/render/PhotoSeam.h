// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PhotoSeam.h - the photometric seam fix for the sky band between the two
// lenses (docs/research/NEURAL_STITCHING.md, section 8).
//
// ---------------------------------------------------------------------------
//  What is wrong with the sky seam
// ---------------------------------------------------------------------------
// Measured on the sample clip, the visible band in the sky along the seam is
// photometric, not geometric:
//
//   1. lens 0's usable rim ends near 92.8 deg in the open-sky longitudes, not
//      at the calibrated 97.59 deg, yet the production feather still gives it
//      0.67 of the blend at 95 deg - a dark line along the overlap edge;
//   2. the two lenses disagree by a gain that changes with direction (0.71
//      stop in the sky, 0.31 on the ground, mostly R and G), which one global
//      gain per lens can only compromise on;
//   3. a tone step where lens 0's occlusion mask ends, which disappears once
//      the first two are fixed.
//
// ---------------------------------------------------------------------------
//  Stage 1 (this part): a render-only blend inset
// ---------------------------------------------------------------------------
// The kernel's FOV feather comes from geom::BlendParams.  Narrowing the RENDER
// blend so every lens's weight reaches zero a few degrees inside the
// calibrated rim removes most of the dark line without touching the kernel.
// It must stay render-only: the analyses (parallax, seam search, gain) keep
// the full calibrated FOV, because shrinking the overlap they measure in
// costs parallax quality (NCC 0.92 -> 0.75-0.86 on the ground, measured).
//
// The same header also carries the section 1.4 sky metrics, implemented
// exactly as research/neural/photo_grid_experiments.py:metrics(), so the
// quality claims are measured in C++ on bands rendered through the real
// kernel rather than in a Python re-blend.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/render/RenderJob.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Stage 1: the render-only seam edge inset
// ===========================================================================

/// Default inset of the render blend below the calibrated half FOV, degrees.
/// 97.59 - 2.6 = 94.99 deg: the research's "blend ends at 95 deg" fallback,
/// where lens 0 is still within one stop of its core on the sample clip.
inline constexpr double kDefaultSeamInsetDeg = 2.6;

/// Largest inset the UI offers, degrees.  Beyond ~6 deg the two render
/// feathers no longer overlap on a 195 deg lens pair (2 x (97.59 - 6) = 183
/// deg of coverage leaves only +-1.6 deg of blend), so a larger value would
/// trade the dark line for a hard seam.
inline constexpr double kMaxSeamInsetDeg = 6.0;

/// FOV feather width of an inset render blend, degrees.  Narrower than the
/// analysis feather (4 deg) so the blend still ends near the usable rim.
inline constexpr double kSeamInsetFeatherDeg = 3.0;

/// The render-only blend for a seam edge inset.
///
/// @param analysis    the blend the analyses use (the calibrated FOV and its
///                    feather; ImporterInstance::m_blend, osvtool's
///                    --lens-fov / --feather).
/// @param insetDeg    how far inside the calibrated half FOV the render
///                    weight reaches zero, degrees, clamped to
///                    [0, kMaxSeamInsetDeg].
/// @param featherDeg  FOV feather of the inset blend, degrees (> 0).
/// @return `analysis` UNCHANGED - every field bit for bit - when the inset is
///         zero, non-finite or negative, so "no inset" renders exactly as
///         before this function existed; otherwise `analysis` with
///         lensFovDeg reduced by 2 x inset and featherDeg replaced.
[[nodiscard]] geom::BlendParams insetRenderBlend(const geom::BlendParams& analysis, double insetDeg,
                                                 double featherDeg = kSeamInsetFeatherDeg) noexcept;

// ===========================================================================
//  Rendering band rows through the real kernel
// ===========================================================================

/// Shade rows [row0, row1) of `job` on the CPU into tightly packed RGBA
/// floats, with the job's seam table, warp grid and everything else it
/// carries - the same per-pixel call CpuRenderer makes, at the same ABSOLUTE
/// row index, so each row equals the matching row of a full render.
///
/// Host frames only (a device job is refused with InvalidArgument).  Used by
/// the quality measurements, which need a ±30 deg band of a full-resolution
/// polar map without paying for the rest of it.
[[nodiscard]] Result<std::vector<float>> shadeJobRows(const RenderJob& job, std::uint32_t row0, std::uint32_t row1,
                                                      ThreadPool& pool);

// ===========================================================================
//  Section 1.4 sky metrics
// ===========================================================================

/// RGBA bands of a polar-axis equirect (lens axes at the poles, the seam on
/// the equator), rendered through the kernel in scene-linear light: the
/// blended picture and each lens alone (with whatever correction the variant
/// under test applies to it).
struct MetricBands {
    std::uint32_t w = 0;          ///< Columns (the whole 360 deg of longitude).
    std::uint32_t h = 0;          ///< Rows.
    std::uint32_t rowOffset = 0;  ///< First map row of the band.
    std::uint32_t mapH = 0;       ///< Height of the whole polar map (w / 2).
    std::vector<float> blend;     ///< Blended RGBA, w * h * 4.
    std::vector<float> lens[2];   ///< Each lens alone, RGBA, w * h * 4 (alpha = its weight).

    /// Latitude of band row `r`, degrees (+ towards lens 1, the master).
    [[nodiscard]] double latDeg(std::uint32_t r) const noexcept;
};

/// The four seam-visibility numbers of NEURAL_STITCHING.md table 1.4, in
/// millistops (lower is better).
struct SkySeamMetrics {
    double line = 0.0;   ///< RMS(profile - 1.5 deg blur), |lat| <= 10: thin lines.
    double band = 0.0;   ///< RMS(0.5 deg blur - 4 deg blur), |lat| <= 10: band-scale bumps.
    double broad = 0.0;  ///< RMS(2 deg blur - 10 deg blur), |lat| <= 25: soft bands, decay ramps.
    double dE = 0.0;     ///< RMS log2 chroma (R/G, B/G) difference of the two lenses on trusted pixels.
    std::uint64_t trustedPixels = 0;  ///< Pixels the dE term was averaged over.
};

/// Geometry of the metric bands (see renderMetricBands).
struct MetricBandRequest {
    std::uint32_t mapW = 2048;  ///< Polar map width (columns per 360 deg).
    double halfDeg = 30.0;      ///< Half height of the band around the seam, degrees.
};

/// Render the metric bands of one variant: a polar-axis map `request.mapW`
/// wide, rows within `request.halfDeg` of the seam, blended and per lens, all
/// through the shared kernel on the CPU.
///
/// `builder` is copied and completed here: the polar equirect map, a
/// scene-linear colour block and alpha coverage are forced; everything else -
/// rig, blend, gains, seam table, warp grid, photometric field - is the
/// caller's variant.  Host frames only.
[[nodiscard]] Result<MetricBands> renderMetricBands(const RenderParamsBuilder& builder,
                                                    const video::FramePair& frames,
                                                    const MetricBandRequest& request, ThreadPool& pool);

/// Compute the metrics over longitude columns [colBegin, colEnd) of the band
/// (fractions of the width, 0.20 / 0.44 = the sample's open sky).
///
/// `trust` (w * h, nonzero = trusted) selects the pixels the colour term is
/// averaged over; the research uses "co-valid and inside both usable rims".
/// Exactly photo_grid_experiments.py:metrics(): log2 luma (BT.2020 weights)
/// averaged over blocks of w/128 columns, Gaussian blurs along latitude only
/// with a 4-sigma radius and replicated borders.
///
/// Errors: InvalidArgument for mismatched sizes, an empty column range or a
/// band without the +-10 deg rows the thin-line terms need.
[[nodiscard]] Result<SkySeamMetrics> skySeamMetrics(const MetricBands& bands, const std::vector<std::uint8_t>& trust,
                                                    double colBeginFrac, double colEndFrac);

/// The simplest trust mask for skySeamMetrics: both lenses' alpha >= 0.99
/// (production weights: unoccluded and clear of the FOV feather) and both
/// finite.  Take it from the UNCORRECTED variant's bands so every variant of
/// one frame is scored on the same pixels.
[[nodiscard]] std::vector<std::uint8_t> coValidTrustMask(const MetricBands& bands);

}  // namespace osv::render
