// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FormatInfo: the coarse description of a clip that every later stage keys
// off - recording mode, colour mode, lens accessory, which video track is
// which lens.  Filled by FormatDetector from the container and the metadata
// track; anything inferred rather than read is explained in `notes`.
#pragma once

#include "osv/meta/Types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace osv::meta {

/// Recording mode, keyed off the per-lens stream width.
enum class Mode : std::int32_t {
    Unknown = 0,  ///< Width not recognised (see FormatInfo::notes).
    K4 = 1,       ///< 1920 x 1920 per lens.
    K6 = 2,       ///< 3000 x 3000 per lens (the verified sample clip).
    K8 = 3,       ///< 3840 x 3840 per lens.
    Lrf = 4       ///< LRF proxy: one 2048 x 1024 side-by-side track (1024 x 1024 per lens).
};

/// Stable name of a Mode ("K6", "Lrf", ...).
[[nodiscard]] const char* modeName(Mode mode) noexcept;

struct FormatInfo {
    std::string cameraModel;                       ///< "Osmo 360" (ClipMeta product name, else udta tool).
    Mode mode = Mode::Unknown;                     ///< Recording mode.
    std::uint32_t streamW = 0;                     ///< Width of one lens stream (the whole track for LRF).
    std::uint32_t streamH = 0;                     ///< Height of one lens stream.
    double fps = 0.0;                              ///< Frame rate (container sample table, else metadata).
    ColorMode colorMode = ColorMode::Unknown;      ///< Colour mode of the streams.
    bool colorModeFromMetadata = false;            ///< True when colorMode was read from StreamMeta.
    ExtriLensMode lensMode = ExtriLensMode::Native;///< Accessory calibration set in use.
    std::uint32_t bitDepth = 0;                    ///< Luma bit depth (10 for HEVC Main10, 8 for the LRF).
    bool dualFisheye = false;                      ///< Two lens streams (or one side-by-side track).
    std::array<std::uint32_t, 2> videoTrackIds{};  ///< [0] slave (stream 0), [1] master (stream 1); equal for LRF.
    std::uint32_t metaTrackId = 0;                 ///< The primary djmd track (0 when none).
    std::uint32_t sensorW = 0;                     ///< Calibration frame width (3840).
    std::uint32_t sensorH = 0;                     ///< Calibration frame height (3840).
    float digitalFocalLength = 0.0f;               ///< Stream-space focal length from ClipMeta.
    bool sideBySideProxy = false;                  ///< True for the LRF layout.
    std::vector<std::string> notes;                ///< Everything that was assumed or could not be verified.

    /// Width of ONE LENS image, which is what the geometry is built from.
    ///
    /// For every dual-track mode this is `streamW`, because each track holds
    /// one fisheye circle.  For the LRF proxy it is HALF of `streamW`: that
    /// clip carries a single 2048 x 1024 side-by-side track holding two
    /// 1024 x 1024 fisheye halves, which `video::DualStreamReader` already
    /// splits before handing frames out (DualStreamReader.cpp, `sideBySide`
    /// branch: `lensWidth = width / 2`).
    ///
    /// Everything downstream of the decoder - `geom::StreamScaling::derive`,
    /// `geom::LensRig` and therefore `render::RenderParamsBuilder` - describes
    /// ONE LENS and must be given this size, not the track size.  Feeding it
    /// `streamW` is what produced the "frame size does not match the rig
    /// (1024x1024 vs 2048x1024)" rejection on every LRF frame: the reader
    /// delivered halves while the rig had been built for the whole track.
    ///
    /// Defensive: an odd or degenerate `streamW` is passed through unhalved
    /// rather than silently truncated, so a malformed clip fails in the rig
    /// builder (which reports what it saw) instead of being reinterpreted here.
    [[nodiscard]] std::uint32_t lensW() const noexcept {
        if (sideBySideProxy && streamW >= 2 && (streamW % 2) == 0) {
            return streamW / 2;
        }
        return streamW;
    }

    /// Height of one lens image.  The LRF halves are stacked side by side, so
    /// only the width is divided; this exists as a named counterpart to
    /// lensW() so call sites read symmetrically and never divide the wrong
    /// axis by hand.
    [[nodiscard]] std::uint32_t lensH() const noexcept { return streamH; }
};

}  // namespace osv::meta
