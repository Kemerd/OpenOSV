// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MountMask: Source Settings "Hide Mount: Auto" - keep the calibration's
// occlusion polygons only where they hide the camera's mount, and give the
// lens overlap back where they hide scene both lenses see.
//
// WHY
// ---
// The camera records one occlusion polygon per lens: the sliver between the
// mount's arc and the rim of the image circle.  On a selfie stick it hides
// the stick.  On a car, helmet or suction mount both lenses' polygons can
// start before the 90 deg seam plane and leave a long arc of the seam ring
// with NO overlap at all (97.6 deg of it on a measured car clip): nothing -
// flow grid, seam table, carved seam - can align near objects there, so a
// roof line steps at a forced cut.  Hide Mount Off removes the polygons
// everywhere (and the mount can show); Auto decides per stretch of the seam.
//
// HOW
// ---
// Once per clip, on the nine sample frames the clip correction uses:
//
//   1. Both lenses are rendered ALONE, without the mask, into the polar-axis
//      band around the seam (render::renderLensBands, the analyses' mapping).
//   2. The polygons' arc - the band columns where either lens's polygon
//      reaches the usable image circle - is cut into 16-column windows.
//   3. Per window and frame: the best 2-D normalised cross-correlation of
//      lens 0's rows |phi| <= 5 deg against lens 1, searched +/- 8 deg along
//      the meridian (near objects shift along it) and +/- 2 deg across.  The
//      same search scores the two halves on their own: phi > 0 is lens 0's
//      far side (inside its polygon) against lens 1's near side, phi < 0 the
//      other way round.
//   4. Per window over the frames: the median correlation (the agreement),
//      the median halves and the median luma sigma of each lens.
//   5. A textured window whose agreement is >= 0.8 is RELEASED: both lenses
//      see the same scene there, so the polygon only removes overlap.  A flat
//      window (sigma < 0.01 in both lenses) agrees with anything, so it is
//      released only when both its neighbours are released textured windows
//      - a stick over open sky stays hidden.  Every kept window is dilated by
//      12 columns into its released neighbours.
//   6. In each run of kept columns, the lens whose far side agrees better
//      with the other lens's near side is the lens that does NOT image the
//      mount there.  Where neither lens is fully trusted on the seam plane
//      (both polygons start before it and their feathers overlap, the
//      0.72-0.79 coverage strip on the car clip), that lens's polygon is
//      clamped to start beyond 90.5 deg plus the occlusion feather, so it
//      covers the seam at full weight.
//   7. applyMountMask turns the per-column verdict into polar-angle stretches
//      of each lens's fisheye and rebuilds its polygon
//      (geom::clipOcclusionPolygon).  The kernels and shaders are unchanged;
//      the carve still routes around whatever polygon is left through its
//      coverage cost.
//
// A verdict that releases nothing and clamps nothing leaves the polygons bit
// for bit as the calibration drew them, so such a clip renders exactly as
// with Hide Mount On.  When fewer than five of the nine samples can be
// scored the measurement fails, and the caller keeps the full polygons.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/ClipSteady.h"
#include "osv/render/SeamAnalysis.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace osv::render {

// ===========================================================================
//  Parameters
// ===========================================================================

/// Everything Hide Mount Auto's measurement and decision depend on.  The
/// defaults are the shipped rule; a cache key must hash every field
/// (SteadyStage does).
struct MountMaskParams {
    std::uint32_t equirectW = 2048;  ///< Columns of the polar-axis band (the seam analyses' resolution).
    double rowsHalfDeg = 5.0;        ///< Rows scored: |phi| <= this many degrees from the seam.
    double searchAlongDeg = 8.0;     ///< Search along the meridian (rows), +/- degrees.
    double searchAcrossDeg = 2.0;    ///< Search across the meridian (columns), +/- degrees.
    std::uint32_t windowCols = 16;   ///< Window width in band columns (2.8 deg at 2048).
    double releaseNcc = 0.8;         ///< A textured window with this median agreement or more is released.
    double flatSigma = 0.01;         ///< A window is flat when both lenses' luma sigma is below this.
    std::uint32_t dilateCols = 12;   ///< Kept windows grow by this many columns into released ones.
    /// A shift is scored only when at least this fraction of the window (of
    /// the half, for a half) is covered by both lenses.
    double minCovalidFraction = 0.4;
    /// Fewest frames that must be scored (and fewest scores a window needs
    /// for an agreement); capped at the number of frames asked for.
    std::uint32_t minFrames = kClipSteadyMinSamples;
    /// The lens that does not image the mount is fully trusted up to this
    /// angle from its axis wherever neither lens was trusted on the seam.
    double clampThetaDeg = 90.5;
};

/// Check every field; the message names the first bad one.
[[nodiscard]] Status validateMountMaskParams(const MountMaskParams& params);

// ===========================================================================
//  The verdict
// ===========================================================================

/// Per band column of the verdict (MountMask::state).
inline constexpr std::uint8_t kMountKeepClean0 = 0;  ///< Polygon kept; lens 0 does not image the mount here.
inline constexpr std::uint8_t kMountKeepClean1 = 1;  ///< Polygon kept; lens 1 does not image the mount here.
inline constexpr std::uint8_t kMountRelease = 2;     ///< Polygons released: both lenses see the scene here.

/// The polygons' footprint on the band ring (mountArcColumns).
struct MountArc {
    std::uint32_t columns = 0;            ///< Ring width.
    std::vector<std::uint8_t> inArc;      ///< 1 where either lens's polygon reaches its usable image circle.
    /// Per column, the lens the calibration says images the mount LESS there:
    /// the only lens without a polygon, or the one whose polygon starts
    /// further from its axis.  The fallback when the data cannot tell.
    std::vector<std::uint8_t> geometricClean;
    std::uint32_t arcColumns = 0;         ///< Columns with inArc set.
};

/// One window in one frame (NaN where it could not be scored).
struct MountWindowScore {
    float ncc = std::numeric_limits<float>::quiet_NaN();     ///< Best NCC of the whole window.
    float upper = std::numeric_limits<float>::quiet_NaN();   ///< Best NCC of phi > 0 (lens 0's far side).
    float lower = std::numeric_limits<float>::quiet_NaN();   ///< Best NCC of phi < 0 (lens 1's far side).
    float sigma0 = std::numeric_limits<float>::quiet_NaN();  ///< Lens 0 luma sigma over its covered pixels.
    float sigma1 = std::numeric_limits<float>::quiet_NaN();  ///< Lens 1 luma sigma (same unshifted window).
};

/// One window of the arc over all frames.
struct MountWindow {
    std::uint32_t col0 = 0;                                           ///< First band column.
    double agreement = std::numeric_limits<double>::quiet_NaN();      ///< Median best NCC over the frames.
    double upperNcc = std::numeric_limits<double>::quiet_NaN();       ///< Median of the phi > 0 halves.
    double lowerNcc = std::numeric_limits<double>::quiet_NaN();       ///< Median of the phi < 0 halves.
    double sigma[2] = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
    std::uint32_t frames = 0;  ///< Frames whose whole-window NCC was scored.
    bool flat = false;         ///< Both lenses' sigma below MountMaskParams::flatSigma.
    bool released = false;     ///< Released by the rule (before the dilation of its neighbours).
};

/// Hide Mount Auto's verdict for one clip.
struct MountMask {
    std::uint32_t columns = 0;        ///< Ring width (MountMaskParams::equirectW).
    /// Per band column: kMountKeepClean0 / kMountKeepClean1 / kMountRelease.
    /// Columns outside the arc are kept (no polygon reaches them).
    std::vector<std::uint8_t> state;
    std::vector<MountWindow> windows;  ///< The arc's windows (diagnostics; not cached).
    std::uint32_t arcColumns = 0;      ///< Columns some polygon reaches.
    std::uint32_t releasedColumns = 0; ///< Arc columns released after the dilation.
    std::vector<std::uint32_t> frames;       ///< Frames measured (substitutes included).
    std::vector<std::string> sampleNotes;    ///< Samples that could not be decoded.
    double decodeMs = 0.0;             ///< Time in the frame source.
    double measureMs = 0.0;            ///< Bands + correlation + decision.

    /// A verdict with one state per column.
    [[nodiscard]] bool valid() const noexcept { return columns > 0 && state.size() == columns; }
};

/// "released 52 of 717 arc columns (9.1 deg) in 1 stretch; 3 of 45 windows released (1 flat)".
[[nodiscard]] std::string describeMountMask(const MountMask& mask);

// ===========================================================================
//  Steps (exposed for tests and osvtool)
// ===========================================================================

/// The polygons' footprint on the ring of `equirectW` band columns, from the
/// rig's geometry alone: a column is in the arc when, for either lens, the
/// ray from the lens centre at that column's polar angle meets the polygon
/// inside the usable image circle.
[[nodiscard]] Result<MountArc> mountArcColumns(const geom::LensRig& rig, std::uint32_t equirectW);

/// Score one frame's bands (rendered without the mask, half height
/// rowsHalfDeg + searchAlongDeg): one MountWindowScore per entry of
/// `windowStarts` (first band column of each window).  `pool` may be null
/// (serial).
[[nodiscard]] Result<std::vector<MountWindowScore>> scoreMountBands(const LensBands& bands,
                                                                    const std::vector<std::uint32_t>& windowStarts,
                                                                    const MountMaskParams& params, ThreadPool* pool);

/// The decision (steps 4-6 of the header) from the per-frame scores
/// (`perFrame[f][w]` for window `windowStarts[w]`).
[[nodiscard]] Result<MountMask> decideMountMask(const MountArc& arc, const std::vector<std::uint32_t>& windowStarts,
                                                const std::vector<std::vector<MountWindowScore>>& perFrame,
                                                const MountMaskParams& params);

/// The whole measurement on the clip's sample `frames` through `rig` (the
/// calibration rig with its full polygons; `blend` is the analysis blend,
/// its occlusion switch is ignored).  A frame that cannot be decoded is
/// replaced by the first of `alternates(frame)` that can, or skipped (noted);
/// fails when fewer than min(minFrames, frames.size()) frames are scored,
/// for a band render failure, malformed input or cancellation.
[[nodiscard]] Result<MountMask> measureMountMask(const geom::LensRig& rig, const geom::BlendParams& blend,
                                                 const std::vector<std::uint32_t>& frames,
                                                 const ClipFrameSource& source, const MountMaskParams& params,
                                                 ThreadPool& pool, const ClipCancel& cancelled = {},
                                                 const ClipFrameAlternates& alternates = {});

/// What applyMountMask changed.
struct MountMaskApplied {
    bool changed[2] = {false, false};             ///< Lens i's polygon was rebuilt.
    std::uint32_t releasedColumns = 0;            ///< Arc columns released.
    std::uint32_t clampedColumns[2] = {0u, 0u};   ///< Kept columns where lens i's polygon is clamped.
    std::uint32_t mergedStretches = 0;            ///< Stretches kept to fit the kernels' vertex budget.
};

/// Rebuild `rig`'s occlusion polygons from the verdict (step 7).  `rig` must
/// carry the calibration's polygons (the verdict is defined against them);
/// a rotation folded into the rig is fine - the polygons live in fisheye
/// pixels.  `blend` gives the occlusion feather and the usable field.  On
/// failure the rig is left untouched.
[[nodiscard]] Result<MountMaskApplied> applyMountMask(geom::LensRig& rig, const MountMask& mask,
                                                      const geom::BlendParams& blend, const MountMaskParams& params);

/// The per-column states as compact text for the caches: runs of
/// "<state>:<first>-<last>" joined by ',' (e.g. "0:0-299,2:300-351,0:352-2047").
[[nodiscard]] std::string encodeMountColumns(const std::vector<std::uint8_t>& state);

/// The inverse of encodeMountColumns for a ring of `columns`; InvalidArgument
/// for anything that does not cover every column exactly once with a known
/// state.
[[nodiscard]] Result<std::vector<std::uint8_t>> decodeMountColumns(std::string_view text, std::uint32_t columns);

}  // namespace osv::render
