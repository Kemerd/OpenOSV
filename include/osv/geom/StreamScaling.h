// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Mapping from the 3840 x 3840 calibration (sensor) frame to the pixels of a
// recorded video stream.  The camera crops the sensor centrally and scales
// the crop to the stream size, so a sensor pixel maps to a stream pixel with
//
//     stream = dstCentre + (sensor - srcCentre) * scale
//
// Verified on the 6K clip: the 3000 px stream is the central 3776 px of the
// 3840 px calibration frame scaled by 0.794492 (== 3000 / 3776).  Other modes
// are derived from digital_focal_length and flagged as unverified.
#pragma once

#include "osv/core/Math.h"
#include "osv/core/Result.h"

#include <optional>
#include <string>
#include <vector>

namespace osv::geom {

/// Similarity transform sensor px -> stream px (uniform scale + shift).
struct StreamScaling {
    double scale = 1.0;      ///< Stream px per sensor px.
    double srcCx = 0.0;      ///< Sensor frame centre x (sensorW / 2).
    double srcCy = 0.0;      ///< Sensor frame centre y (sensorH / 2).
    double dstCx = 0.0;      ///< Stream frame centre x (streamW / 2).
    double dstCy = 0.0;      ///< Stream frame centre y (streamH / 2).
    bool verified = true;    ///< False when the scale came from an unverified rule.

    /// The verified 6K crop scale: 3000 / 3776.
    static constexpr double kVerifiedCropScale6K = 0.794492;
    /// Width of the central sensor crop that becomes the 6K stream.
    static constexpr double kVerifiedCropWidth6K = 3776.0;

    /// Map a sensor-frame point to the stream frame.
    [[nodiscard]] Vec2d apply(const Vec2d& sensorPx) const noexcept;

    /// Map a stream-frame point back to the sensor frame (inverse of apply).
    /// A zero scale yields the source centre instead of infinities.
    [[nodiscard]] Vec2d applyInverse(const Vec2d& streamPx) const noexcept;

    /// Identity mapping for a stream that has the sensor's size.
    [[nodiscard]] static StreamScaling identity(int width, int height) noexcept;

    /// Derive the mapping for a stream of streamW x streamH from a sensor of
    /// sensorW x sensorH.  Rules, in order:
    ///   * overrideScale set         -> used verbatim (flagged verified=false).
    ///   * same size                 -> 1.0.
    ///   * 3000 from 3840            -> kVerifiedCropScale6K (verified on the sample clip).
    ///   * 1024 from 3840 (LRF half) -> 1024 / 3776, unverified.
    ///   * otherwise                 -> digitalFocalLength / calFxMean, unverified.
    /// Human readable explanations are appended to `notes` when non-null.
    /// Fails on non-positive sizes, a non-positive override, or when the
    /// fallback rule has no usable focal lengths.
    [[nodiscard]] static Result<StreamScaling> derive(int streamW, int streamH, int sensorW, int sensorH,
                                                      double digitalFocalLength, double calFxMean,
                                                      std::optional<double> overrideScale = std::nullopt,
                                                      std::vector<std::string>* notes = nullptr);
};

}  // namespace osv::geom
