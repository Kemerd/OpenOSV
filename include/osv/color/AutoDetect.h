// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Fallback colour-mode detection from picture statistics, for clips whose
// metadata track is missing or damaged (the normal path reads
// StreamMeta.colorMode).  It looks only at the luma histogram of one frame:
//
//   * D-Log M places 18 % grey at code 0.40 and never reaches the top of the
//     range (clip white ~0.8), so the histogram is compact and mid-heavy.
//   * HLG uses the full range with a dark-heavy distribution (median low,
//     highlights reach > 0.85).
//   * Rec.709 "Normal" spreads over the full range with a median anywhere.
//
// Nothing is masked: the corners of a dual-fisheye frame lie outside the
// image circle and read about luma code 70 (narrow-range 10-bit, i.e.
// normalised ~0.007), which drags p001 to ~0 in every mode and makes
// `spread` behave like p999.  The thresholds below were chosen with that in
// mind, so callers do not need to mask the frame first.
#pragma once

#include "osv/color/ColorParams.h"
#include "osv/meta/Types.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>

namespace osv::color {

/**
 * @brief The clip's own colour mode -> the encoding the pipeline must decode.
 *
 * This is the single rule for "what IS this footage", and it deliberately has
 * nothing to do with what the user asked the output to be.  Keeping the two
 * apart is what prevents the one genuinely destructive failure in this
 * pipeline: decoding an already display-referred clip as if it were log.
 * An HLG or Rec.709 source run through the D-Log M curve is double-converted
 * - a de-log applied to a signal that was never logged - which crushes the
 * shadows and blows the highlights, and no downstream grade recovers it.
 * So the input encoding follows the METADATA and the output transfer follows
 * the PREFERENCE, always, and never the other way round.
 *
 * Unknown maps to D-Log M rather than to Rec.709 on purpose.  A clip with no
 * usable colour metadata is overwhelmingly D-Log M in this container (it is
 * the mode the project targets and the mode the camera writes for the .OSV
 * dual-fisheye format), and the statistical fallback (detectColorMode) is the
 * intended second opinion when a decoded frame is available.  Guessing
 * Rec.709 for a log clip would leave it flat and washed out with no hint as
 * to why; guessing D-Log M for the rare non-log clip is visible immediately.
 *
 * The modes that are neither log nor HLG nor Normal - D-Cinelike, Vivid,
 * D-Log, D-Log2 - fall through to D-Log M as well.  They are not produced by
 * the Osmo 360 in this container, and none of them has its own curve here, so
 * the honest choice is the default rather than a silently wrong branch.
 *
 * @param mode  StreamMeta.colorMode (19 = D-Log M, 9 = HLG, 0 = Normal).
 */
[[nodiscard]] constexpr InputEncoding inputEncodingForColorMode(meta::ColorMode mode) noexcept {
    switch (mode) {
    case meta::ColorMode::HLG:    return InputEncoding::HLG;
    case meta::ColorMode::Normal: return InputEncoding::Rec709Normal;
    case meta::ColorMode::DLogM:
    case meta::ColorMode::DLog:
    case meta::ColorMode::DLog2:
    case meta::ColorMode::DCinelike:
    case meta::ColorMode::Vivid:
    case meta::ColorMode::Unknown:
    default:                      return InputEncoding::DLogM;
    }
}

/// Result of detectColorMode.
struct AutoDetectResult {
    meta::ColorMode guess = meta::ColorMode::Unknown;  ///< DLogM, HLG or Normal (Unknown when the frame is unusable).
    float confidence = 0.0f;   ///< 0..1, distance of the statistics from the decision thresholds.
    float p001 = 0.0f;         ///< 0.1 % luma percentile (normalised 0..1 code).
    float p50 = 0.0f;          ///< Median luma.
    float p999 = 0.0f;         ///< 99.9 % luma percentile.
    float spread = 0.0f;       ///< p999 - p001.
    std::uint64_t samples = 0; ///< Number of luma samples inspected.
};

/// Decision thresholds (public so the CLI can print them).
inline constexpr float kDetectDlogMaxP999 = 0.82f;    ///< D-Log M: p999 <= this.
inline constexpr float kDetectDlogMaxSpread = 0.70f;  ///< D-Log M: spread <= this.
inline constexpr float kDetectDlogMinP50 = 0.22f;     ///< D-Log M: p50 >= this.
inline constexpr float kDetectDlogMaxP50 = 0.62f;     ///< D-Log M: p50 <= this.
inline constexpr float kDetectHlgMinP999 = 0.85f;     ///< HLG: p999 > this.
inline constexpr float kDetectHlgMaxP50 = 0.35f;      ///< HLG: p50 < this.

/**
 * @brief Guess the colour mode of a decoded frame from its luma histogram.
 *
 * Rules (luma normalised with the frame's range: narrow (Y-64)/876 for
 * 10-bit, full Y/1023):
 *   D-Log M  if p999 <= 0.82 && spread <= 0.70 && p50 in [0.22, 0.62]
 *   HLG      else if p999 > 0.85 && p50 < 0.35
 *   Normal   otherwise
 *
 * @param frame      Any valid PlanarFrame16 (planar or P010 layout).
 * @param subsample  Inspect every Nth pixel in x and y (>= 1; 8 by default,
 *                   which is ~140k samples on a 3000 x 3000 lens).
 * @return           guess == Unknown with samples == 0 when the frame is invalid.
 */
[[nodiscard]] AutoDetectResult detectColorMode(const video::PlanarFrame16& frame,
                                               std::uint32_t subsample = 8) noexcept;

/// Apply the decision rules to precomputed statistics (exposed for tests).
[[nodiscard]] AutoDetectResult classifyLumaStats(float p001, float p50, float p999,
                                                 std::uint64_t samples) noexcept;

}  // namespace osv::color
