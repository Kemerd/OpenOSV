// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SceneLight.h - is this clip a day clip or a night clip?  The answer picks
// the photometric profile the seam corrections run with.
//
// ===========================================================================
//  WHY
// ===========================================================================
// Every photometric stage of the stitch (the sky seam fix's gain field, the
// global exposure match, the lens shading correction) treats the sky in the
// lens overlap as a bright, smooth, high-SNR reference.  By day it is: on a
// user's car-mounted 8K clips the sky sits +1.7 to +2.1 stops above metered
// grey with a gradient noise of 0.026 stop / deg.  At night it is not: the
// sky is 2 to 4 stops below grey, noisy (0.09-0.33 stop / deg), and veiled by
// street lights, so the stages measure on the wrong pixels:
//
//   * the gain field's 20 deg raised-cosine decay paints a halo band around
//     each lens into a crushed black sky, and its sky correction near the
//     seam swings by +-4 codes within 3.5 s (0.18 codes by day);
//   * the global exposure match is a ratio of linear means dominated by the
//     lamps: it flips sign within a second and makes sky seam steps of up to
//     10.9 codes (4.1 with no correction at all);
//   * the lens shading gate accepts the door and the road as "sky".
//
// ===========================================================================
//  THE DECISION
// ===========================================================================
// Primary signal: the camera's own metered light value, recorded per frame
// (CameraFrame::aecLv), median over the clip.  Fallback when a firmware does
// not record it: EV100 = log2(N^2 / t) - log2(ISO / 100) from the aperture,
// shutter and ISO the camera recorded.  Measured:
//
//     clip                              LV p5 / median / p95     EV100
//     the night driving clip            2.67 / 3.72 / 4.82       2.07-4.41
//     a user's day clip (sunset)        13.11 / 13.30 / 13.55    11.11-11.66
//     the 6K aerial sample (ND filter)  9.88 / 9.89 / 9.89       9.05
//
// Metered light reads THROUGH an ND filter, so it can only err dark.  A dark
// reading is therefore confirmed by the sky itself: the zenith cap (elevation
// >= 45 deg after levelling on the measured gravity), its robust median
// scene-linear luminance against metered grey 0.18, on flat pixels (a
// noise-adaptive threshold) away from light sources (8 deg around anything
// 4 stops above grey).  Measured: night -1.9 to -3.2 stops, day +1.7 to +2.1,
// the aerial sample -0.74 with a deep blue sky (B/G +1.38).
//
//     Night  <=>  median LV < 6  AND  sky cap <= -1.5 stops
//     Day    otherwise (LV >= 8, a bright or blue cap, the ambiguous middle,
//            or a cap that could not be measured) - today's behaviour.
//
// The cap is only measured when the metered light says dark, on FIXED frames
// of the clip, so the decision never depends on which frame a host asked for
// first, and a day clip costs nothing at all.
//
// ===========================================================================
//  THE NIGHT PROFILE
// ===========================================================================
// Measured on the night driving clip's polar renders, the short decay keeps
// the field's seam-step reduction (52.5 -> 52.5, 41.0 -> 42.4, 60.9 -> 59.3
// millistops) and removes ~89 % of the correction 10-30 deg from the seam,
// where the halo lives (25.3 -> 2.9, 111.6 -> 12.7 millistops).  Day footage
// keeps today's 20 deg: the short decay measurably roughens a day sky (77.8 ->
// 85.7, maintainer 72.0 -> 86.0 millistops), which is why only the detector
// switches profiles.
//
//     sky seam fix     luma decay 20 -> 6 deg; the chroma ratios, which
//                      apply in full inside the overlap, keep their half of
//                      it (10 -> 3 deg beyond the overlap); per-cell clamp
//                      1.5 -> 0.75 stop
//     exposure match   off (identity) on every path
//     lens shading     off
//
// Why the chroma stays inside the overlap: the lenses differ in colour at
// night too.  Measured on the night driving clip (Rec.709 codes, sky seam
// step with the field's chroma / without it): Cb 0.63 / 1.59 and Cr 0.97 /
// 2.07 (LRF frame 3000), Cb 0.42 / 1.12 and Cr 0.69 / 1.81 (frame 6000), and
// the luma step rises above the uncorrected one without it (3.12 against
// 2.98 at frame 3000).  A chroma decay of 0 is no answer either: the kernel
// applies the chroma in full inside the overlap regardless, so 0 only cuts
// it off in a hard colour edge at the overlap's border (a jump of 0.4-1.0
// codes on average, up to 3.1 at the 95th percentile).  The day ratio's
// 3 deg ramp ends it smoothly (0.2-0.5, p95 0.6-1.6) and keeps every seam
// result.
#pragma once

#include "osv/color/ColorMath.h"
#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/LensShading.h"
#include "osv/render/PhotoSeam.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace osv::render {

// ===========================================================================
//  The verdict
// ===========================================================================

/// Which photometric profile a clip renders with.
enum class SceneLight : std::uint8_t {
    Day = 0,    ///< Today's defaults: the profile every clip had before the detector existed.
    Night = 1,  ///< The short-decay, lightly clamped field; no exposure match; no lens shading.
};

/// Stable name for logs and the Properties panel ("Day" / "Night").
[[nodiscard]] const char* sceneLightName(SceneLight light) noexcept;

// ===========================================================================
//  Metered light (metadata only - free)
// ===========================================================================

/// The clip's metered light, from the camera's per-frame exposure metadata.
struct MeteredLight {
    bool valid = false;        ///< At least one frame carried a usable reading.
    bool fromAecLv = false;    ///< True: the camera's own LV; false: EV100 from ISO / shutter / aperture.
    double median = 0.0;       ///< Median over the sampled frames (LV or EV100).
    double p5 = 0.0;           ///< 5th percentile.
    double p95 = 0.0;          ///< 95th percentile.
    std::uint32_t frames = 0;  ///< Frames that contributed a reading.
};

/// The aperture assumed when the clip records none: f/1.9, the fixed
/// aperture of every Osmo 360 clip seen so far (ClipMeta f_number [19, 10]).
inline constexpr double kDefaultFNumber = 1.9;

/// EV100 of one frame: log2(N^2 / t) - log2(ISO / 100).  NaN when the ISO,
/// the shutter rational or the aperture is missing, zero or not finite.
[[nodiscard]] double ev100Of(const meta::CameraFrame& camera, double fNumber) noexcept;

/// The f-number a ClipMeta records ([num, den]); kDefaultFNumber when the
/// field is absent or unusable.
[[nodiscard]] double fNumberOf(const meta::ClipMeta& clip) noexcept;

/// Metered light of a set of frames (pure).  The camera's aecLv is used
/// when MOST frames carry it (> 0 and finite); otherwise EV100 from each
/// frame's ISO / shutter and `fNumber`.  Frames with no usable reading are
/// skipped; an empty or reading-free set returns valid = false.
[[nodiscard]] MeteredLight meteredLightOf(std::span<const meta::CameraFrame> frames, double fNumber) noexcept;

/// Metered light of a clip: at most `maxSamples` frames spread evenly over
/// [0, frameCount) (always including the first and the last), decoded from
/// `track`.  Deterministic: depends only on the clip.  Never throws.
[[nodiscard]] MeteredLight meteredLightOf(const meta::MetadataTrack& track, std::uint32_t frameCount,
                                          std::uint32_t maxSamples = 256) noexcept;

// ===========================================================================
//  The sky cap (pixels - only when the metered light says dark)
// ===========================================================================

/// How the zenith cap is measured.
struct SkyCapParams {
    std::uint32_t equirectW = 1024;   ///< Width of the levelled equirect the cap is cut from (0.35 deg / px).
    double capMinElevationDeg = 45.0; ///< The cap: directions at least this far above the horizon.
    double greyLinear = 0.18;         ///< Metered mid grey in scene-linear light.
    /// A light source: luminance at least this many stops above grey (a
    /// street lamp at night, the sun by day).  Clipped sources sit far above.
    double sourceStopsAboveGrey = 4.0;
    double sourceExclusionDeg = 8.0;  ///< Pixels within this angle of a source are not sky (lamp glow).
    /// Flat test: a pixel's 3 x 3 log2-luminance range must be at most this
    /// multiple of the cap's own median range (the noise floor: high ISO
    /// raises both together), and never needs to be below `flatFloorStops`.
    double flatNoiseMultiple = 2.0;
    double flatFloorStops = 0.15;
    /// The flat pixels decide when they cover at least this fraction of the
    /// usable cap; otherwise every usable pixel does (a cap that is mostly
    /// texture - trees, a tunnel - still has a median).
    double minFlatFraction = 0.3;
    /// The cap is only trusted when its usable (covered, non-source) pixels
    /// cover at least this fraction of it.
    double minUsableFraction = 0.2;
    double minAlpha = 0.5;            ///< Lens coverage a pixel needs (the occlusion arc is not sky).
};

/// One frame's (or a clip's) zenith cap.
struct SkyCap {
    bool valid = false;             ///< Enough usable cap pixels to say anything.
    double stopsVsGrey = 0.0;       ///< log2(median luminance / grey).
    double log2RG = 0.0;            ///< Median log2(R / G) of the same pixels.
    double log2BG = 0.0;            ///< Median log2(B / G) of the same pixels.
    double usableFraction = 0.0;    ///< Usable cap area / cap area (equal-area weights).
    double flatFraction = 0.0;      ///< Flat area / usable area.
    double sourceFraction = 0.0;    ///< Area excluded around light sources / cap area.
    std::uint32_t frames = 0;       ///< Frames combined (1 for a single frame).
};

/// The cap statistics of a levelled Standard-layout equirect (pure; the
/// image's top row is the zenith, rows cover 180 deg).  `rgba` holds the
/// TOP `rows` rows of a `w` x `w / 2` map in scene-linear Rec.2020 light
/// with coverage alpha; rows below the cap may be omitted.
[[nodiscard]] SkyCap skyCapOf(const float* rgba, std::uint32_t w, std::uint32_t rows,
                              const SkyCapParams& params = {}) noexcept;

/// Measure one frame's zenith cap: shade the cap rows of a levelled
/// equirect (world +Z = `upBody`, the gravity-up direction in body
/// coordinates) through the real kernel on the CPU, in scene-linear light
/// decoded with `linearColor` (a makeColorParams block whose transfer is
/// Linear), then skyCapOf().  Host frames only.  Fails on a bad up vector,
/// bad parameters or a frame the rig does not fit.
[[nodiscard]] Result<SkyCap> measureSkyCap(const geom::LensRig& rig, const video::FramePair& frames,
                                           const geom::BlendParams& blend, const Vec3d& upBody,
                                           const OsvColorParams& linearColor, ThreadPool& pool,
                                           const SkyCapParams& params = {});

/// The clip's cap from its frames' caps: the median of each statistic over
/// the valid ones (deterministic for a fixed frame set).  Invalid when none
/// is valid.
[[nodiscard]] SkyCap combineSkyCaps(std::span<const SkyCap> caps) noexcept;

/// Gravity-up in body coordinates at frame `frame`, from the attitude track
/// (the frame's own metadata timestamp, else nominal spacing at `fps`).
/// nullopt when the attitude yields no finite direction.
[[nodiscard]] std::optional<Vec3d> bodyUpAt(const geom::AttitudeTrack& attitude, const meta::MetadataTrack& track,
                                            std::uint32_t frame, double fps) noexcept;

// ===========================================================================
//  Classification
// ===========================================================================

/// The thresholds of the decision (see the file comment for the numbers).
struct SceneLightThresholds {
    double nightMaxLv = 6.0;          ///< Metered light below this may be night.
    double dayMinLv = 8.0;            ///< Metered light at or above this is day, whatever the cap.
    double nightMaxCapStops = -1.5;   ///< A cap at or below this confirms night.
    double dayMinCapStops = -1.0;     ///< A cap at or above this (and blue) is day.
    double blueMinLog2BG = 0.3;       ///< "Blue": log2(B / G) above this.
};

/// What the decision was made from, and the decision.
struct SceneLightVerdict {
    SceneLight light = SceneLight::Day;
    MeteredLight metered;           ///< The metered light (valid = false: no reading).
    std::optional<SkyCap> cap;      ///< The cap, when it was measured.
    bool needsCap = false;          ///< The metered light said dark: only the cap can confirm night.
    std::string reason;             ///< One short human sentence.
};

/// True when the metered light alone leaves night possible (valid and below
/// nightMaxLv), i.e. when the cap has to be measured.
[[nodiscard]] bool meteredLightSaysDark(const MeteredLight& metered,
                                        const SceneLightThresholds& thresholds = {}) noexcept;

/// Decide.  Night only when the metered light is below nightMaxLv AND a
/// valid cap is at or below nightMaxCapStops; Day in every other case.
[[nodiscard]] SceneLightVerdict classifySceneLight(const MeteredLight& metered, const std::optional<SkyCap>& cap,
                                                   const SceneLightThresholds& thresholds = {});

/// "LV 3.7, sky -3.2 stops" / "EV100 3.3" / "no exposure metadata": the
/// measurements behind a verdict, for the Properties panel and the log.
[[nodiscard]] std::string sceneLightEvidence(const SceneLightVerdict& verdict);

// ===========================================================================
//  The night profile
// ===========================================================================

/// The sky seam fix's luma decay at night, degrees beyond the overlap.
inline constexpr double kNightPhotoDecayDeg = 6.0;
/// The per-cell gain clamp at night, stops.
inline constexpr double kNightPhotoMaxAbsLog2Gain = 0.75;

/// Turn `params` into the night profile's field (luma decay, clamp).  Mode
/// and strength are the user's and stay untouched, and so does the chroma
/// decay SCALE: the chroma ratios keep applying in full inside the overlap
/// and decay over half the (now 6 deg) luma decay beyond it - see the file
/// comment for why neither "no chroma" nor a chroma decay of 0 is better.
void applyNightPhotoProfile(PhotoSeamParams& params) noexcept;

/// The lens shading correction at night: Off (its sky gate accepts lit
/// surfaces).  Strength stays the user's.
void applyNightShadingProfile(LensShadingParams& params) noexcept;

}  // namespace osv::render
