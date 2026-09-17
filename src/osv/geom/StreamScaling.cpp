// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Sensor -> stream pixel mapping and the rules that derive it per mode.

#include "osv/geom/StreamScaling.h"

#include <cmath>
#include <format>

namespace osv::geom {

namespace {

/// Sensor size the verified rules are keyed on.
constexpr int kOsmo360SensorSide = 3840;
/// 6K stream side (the verified crop rule).
constexpr int kStream6KSide = 3000;
/// LRF proxy: each half of the 2048 x 1024 side-by-side frame.
constexpr int kLrfHalfSide = 1024;

/// Append a note when the caller asked for them.
void addNote(std::vector<std::string>* notes, std::string text) {
    if (notes) {
        notes->push_back(std::move(text));
    }
}

}  // namespace

// -----------------------------------------------------------------------------
//  Transform
// -----------------------------------------------------------------------------
Vec2d StreamScaling::apply(const Vec2d& sensorPx) const noexcept {
    // Uniform scale about the sensor centre, re-centred on the stream.
    return Vec2d{dstCx + (sensorPx.x - srcCx) * scale, dstCy + (sensorPx.y - srcCy) * scale};
}

Vec2d StreamScaling::applyInverse(const Vec2d& streamPx) const noexcept {
    // Guard the division: a zero scale maps everything to the source centre.
    if (!(scale != 0.0) || !std::isfinite(scale)) {
        return Vec2d{srcCx, srcCy};
    }
    return Vec2d{srcCx + (streamPx.x - dstCx) / scale, srcCy + (streamPx.y - dstCy) / scale};
}

StreamScaling StreamScaling::identity(int width, int height) noexcept {
    StreamScaling s;
    s.scale = 1.0;
    // Negative sizes are clamped so the centre stays finite and sane.
    s.srcCx = s.dstCx = 0.5 * static_cast<double>(width < 0 ? 0 : width);
    s.srcCy = s.dstCy = 0.5 * static_cast<double>(height < 0 ? 0 : height);
    s.verified = true;
    return s;
}

// -----------------------------------------------------------------------------
//  Derivation rules
// -----------------------------------------------------------------------------
Result<StreamScaling> StreamScaling::derive(int streamW, int streamH, int sensorW, int sensorH,
                                            double digitalFocalLength, double calFxMean,
                                            std::optional<double> overrideScale, std::vector<std::string>* notes) {
    // Sizes must be positive for any of the rules to make sense.
    if (streamW <= 0 || streamH <= 0) {
        return Error{ErrorCode::InvalidArgument, std::format("StreamScaling: bad stream size {}x{}", streamW, streamH)};
    }
    if (sensorW <= 0 || sensorH <= 0) {
        return Error{ErrorCode::InvalidArgument, std::format("StreamScaling: bad sensor size {}x{}", sensorW, sensorH)};
    }

    StreamScaling s;
    // Centres: the crop is central on both sides.
    s.srcCx = 0.5 * static_cast<double>(sensorW);
    s.srcCy = 0.5 * static_cast<double>(sensorH);
    s.dstCx = 0.5 * static_cast<double>(streamW);
    s.dstCy = 0.5 * static_cast<double>(streamH);

    // Rule 0: explicit override wins, flagged as unverified because nothing
    // in the file confirms it.
    if (overrideScale.has_value()) {
        const double v = *overrideScale;
        if (!std::isfinite(v) || v <= 0.0) {
            return Error{ErrorCode::InvalidArgument, std::format("StreamScaling: bad override scale {}", v)};
        }
        s.scale = v;
        s.verified = false;
        addNote(notes, std::format("stream scale {:.6f} from user override (unverified)", v));
        return s;
    }

    // Rule 1: the stream is the whole sensor (8K mode).
    if (streamW == sensorW && streamH == sensorH) {
        s.scale = 1.0;
        s.verified = true;
        addNote(notes, "stream scale 1.0: stream equals the calibration frame");
        return s;
    }

    // Rule 2: the verified 6K crop (3000 px = central 3776 px of 3840 * 0.794492).
    if (streamW == kStream6KSide && streamH == kStream6KSide && sensorW == kOsmo360SensorSide &&
        sensorH == kOsmo360SensorSide) {
        s.scale = kVerifiedCropScale6K;
        s.verified = true;
        addNote(notes, std::format("stream scale {:.6f}: verified 6K crop (3000 = central 3776 of 3840)",
                                   kVerifiedCropScale6K));
        return s;
    }

    // Rule 3: LRF proxy halves (1024 px each) - assumed to use the same crop.
    if (streamW == kLrfHalfSide && streamH == kLrfHalfSide && sensorW == kOsmo360SensorSide &&
        sensorH == kOsmo360SensorSide) {
        s.scale = static_cast<double>(kLrfHalfSide) / kVerifiedCropWidth6K;
        s.verified = false;
        addNote(notes, std::format("stream scale {:.6f}: LRF half assumed to be the 3776 px crop (unverified)",
                                   s.scale));
        return s;
    }

    // Rule 4: generic fallback from the focal lengths (4K and unknown modes).
    if (!std::isfinite(digitalFocalLength) || digitalFocalLength <= 0.0 || !std::isfinite(calFxMean) ||
        calFxMean <= 0.0) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("StreamScaling: no rule for {}x{} from {}x{} and no usable focal lengths ({} / {})",
                                 streamW, streamH, sensorW, sensorH, digitalFocalLength, calFxMean)};
    }
    s.scale = digitalFocalLength / calFxMean;
    s.verified = false;
    addNote(notes, std::format("stream scale {:.6f} = digital_focal_length {:.4f} / calibration fx {:.4f} (unverified)",
                               s.scale, digitalFocalLength, calFxMean));
    return s;
}

}  // namespace osv::geom
