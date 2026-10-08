// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Sensor -> stream pixel mapping and the rules that derive it per mode.

#include "osv/geom/StreamScaling.h"

#include <algorithm>
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

/// digital_focal_length per lens pixel of the FULL-SIZE clip.
///
/// The Osmo 360 does not measure this field per camera or per mode: on all
/// six clips seen so far (two cameras, 6K and 8K, two firmware versions) it
/// is exactly 0.2764537 x the full-size clip's lens width - 829.3612 for the
/// 3000 px 6K stream, 1061.5823 for the 3840 px 8K stream (a ratio of
/// 1.2800000 to float precision).  An LRF repeats its parent clip's value,
/// so on a proxy the field names the PARENT's lens width, not its own.
constexpr double kDflPerLensPx = 829.3612060546875 / 3000.0;

/// Relative tolerance of the convention check (log only).  The six clips
/// sit within 1e-7 of the convention; 0.2 % is far outside float rounding
/// and still well inside the 1.7 % that separates the 3776 px crop from the
/// whole 3840 px frame.
constexpr double kDflConventionTolerance = 0.002;

/// A stream is taken for a proxy when its digital_focal_length names a lens
/// at least this many times wider than the stream itself.
///
/// Every full-size stream measured reads 1.000; the narrowest proxy ratio
/// is 1.875 (a 1024 px LRF half of a 1920 px 4K clip; 6K and 8K parents
/// read 2.93 and 3.75).  1.4 sits close to the geometric midpoint of 1.000
/// and 1.875, so both sides keep a wide margin.
constexpr double kProxyWidthRatio = 1.4;

/// The narrowest sensor crop (px) a proxy can show.  The narrowest crop of
/// any measured mode is 3776 px (6K); a dual-fisheye mode cannot crop much
/// below it without cutting into the image circles.  A proxy handed its
/// parent's focal instead of its own reads 1.9-3.75x too long, far past
/// this bound, so the cap catches exactly that failure.
constexpr double kMinProxyCropWidth = 3000.0;

/// The widest parent stream a digital_focal_length may name, as a multiple
/// of the sensor side.  No recording mode is wider than its sensor (8K is
/// the sensor at 1:1); a value naming a stream more than twice as wide is
/// garbage, not a parent.
constexpr double kMaxParentPerSensor = 2.0;

/// Absolute bound on any parent side (px), so a hostile sensor size cannot
/// push the rounding below past the range of an int.
constexpr double kMaxParentSide = 65536.0;

/// Append a note when the caller asked for them.
void addNote(std::vector<std::string>* notes, std::string text) {
    if (notes) {
        notes->push_back(std::move(text));
    }
}

/// True for a focal length that can enter a ratio: finite and positive.
[[nodiscard]] bool usableFocal(double f) noexcept {
    return std::isfinite(f) && f > 0.0;
}

/// Log-only cross-check of digital_focal_length against the convention for
/// a full-size stream whose lens width is `parentW`.  Adds an info note when
/// the value is more than kDflConventionTolerance away: such a clip comes
/// from a camera or firmware that writes something else into the field,
/// which is worth knowing when its seam looks wrong.  Never changes a scale.
void addConventionNote(std::vector<std::string>* notes, double digitalFocalLength, int parentW) {
    // Nothing to check without a usable value, a width, or a listener.
    if (!notes || !usableFocal(digitalFocalLength) || parentW <= 0) {
        return;
    }
    const double expected = kDflPerLensPx * static_cast<double>(parentW);
    const double deviation = digitalFocalLength / expected - 1.0;
    if (std::isfinite(deviation) && std::fabs(deviation) <= kDflConventionTolerance) {
        return;
    }
    addNote(notes, std::format("digital_focal_length {:.4f} does not follow the Osmo 360 convention ({:.7f} x {} px "
                               "= {:.4f}, off by {:+.3f} %)",
                               digitalFocalLength, kDflPerLensPx, parentW, expected, 100.0 * deviation));
}

/// The prior: the 3776 px crop, every mode's scale when nothing in the file
/// tells it (the same assumption Rule 3 makes for an LRF half without
/// usable focal lengths).  On a sensor narrower than that crop the whole
/// sensor is the widest view the stream can hold, so it is used instead.
/// Always flagged unverified, never an error: a clip missing its
/// digital_focal_length still opens and stitches on the prior.
[[nodiscard]] StreamScaling priorCrop(StreamScaling s, int streamW, int sensorW, std::vector<std::string>* notes,
                                      const std::string& why) {
    // sensorW was validated positive by the caller; the min keeps the crop
    // inside the sensor on a camera narrower than the Osmo 360's.
    const double cropW = std::min(StreamScaling::kVerifiedCropWidth6K, static_cast<double>(sensorW));
    s.scale = static_cast<double>(streamW) / cropW;
    s.verified = false;
    addNote(notes, std::format("stream scale {:.6f}: assumed the {:.0f} px crop, the prior (unverified: {})", s.scale,
                               cropW, why));
    return s;
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
        // The camera's own focal length for the stream, beside the rule: on
        // the 6K clip digital_focal_length / fx reproduces the verified crop
        // scale to five digits, so on an 8K clip it says whether the stream
        // really is the calibration frame at 1:1.
        if (std::isfinite(digitalFocalLength) && digitalFocalLength > 0.0 && std::isfinite(calFxMean) &&
            calFxMean > 0.0) {
            addNote(notes, std::format("stream scale 1.0: stream equals the calibration frame (the recorded "
                                       "digital_focal_length / fx is {:.6f})",
                                       digitalFocalLength / calFxMean));
        } else {
            addNote(notes, "stream scale 1.0: stream equals the calibration frame");
        }
        // Log only: a full-size stream, so the field should name this width.
        addConventionNote(notes, digitalFocalLength, streamW);
        return s;
    }

    // Rule 2: the verified 6K crop (3000 px = central 3776 px of 3840 * 0.794492).
    if (streamW == kStream6KSide && streamH == kStream6KSide && sensorW == kOsmo360SensorSide &&
        sensorH == kOsmo360SensorSide) {
        s.scale = kVerifiedCropScale6K;
        s.verified = true;
        addNote(notes, std::format("stream scale {:.6f}: verified 6K crop (3000 = central 3776 of 3840)",
                                   kVerifiedCropScale6K));
        // Log only: the 6K stream is full-size, so the field should name 3000 px.
        addConventionNote(notes, digitalFocalLength, streamW);
        return s;
    }

    // Rule 3: LRF proxy halves (1024 px each).  The proxy shows the same
    // field of view as the clip it was recorded beside, and the LRF repeats
    // that clip's digital_focal_length, so the ratio to the calibration names
    // the mode the proxy was cut from:
    //
    //   * ~0.7945 (the verified 6K crop scale): the 3776 px crop.  On the 6K
    //     sample's LRF the overlap NCC peaks at 1024/3776 (0.924 vs 0.878 at
    //     1024/3840).
    //   * ~1.0-1.03 (8K mode, where the stream is the whole 3840 px frame):
    //     the full frame.  On eleven frames of two 8K-mode LRFs the NCC peaks
    //     at 1024/3840 (0.92-0.96 vs 0.80-0.87 at 1024/3776).
    //
    // Anything else keeps the 6K crop, as before.
    if (streamW == kLrfHalfSide && streamH == kLrfHalfSide && sensorW == kOsmo360SensorSide &&
        sensorH == kOsmo360SensorSide) {
        const bool haveRatio = std::isfinite(digitalFocalLength) && digitalFocalLength > 0.0 &&
                               std::isfinite(calFxMean) && calFxMean > 0.0;
        const double ratio = haveRatio ? digitalFocalLength / calFxMean : 0.0;
        // Window widths: the two families sit 0.23 apart, the per-camera
        // spread seen so far is 2 %.
        constexpr double kFullFrameMin = 0.97;
        constexpr double kFullFrameMax = 1.05;
        const bool fullFrame = haveRatio && ratio >= kFullFrameMin && ratio <= kFullFrameMax;
        s.scale = static_cast<double>(kLrfHalfSide) /
                  static_cast<double>(fullFrame ? kOsmo360SensorSide : kVerifiedCropWidth6K);
        s.verified = false;
        if (fullFrame) {
            addNote(notes, std::format("stream scale {:.6f}: LRF half of an 8K-mode clip, the full 3840 px frame "
                                       "(digital_focal_length / fx {:.4f})",
                                       s.scale, ratio));
        } else if (haveRatio) {
            addNote(notes, std::format("stream scale {:.6f}: LRF half of the 3776 px crop (digital_focal_length / "
                                       "fx {:.4f})",
                                       s.scale, ratio));
        } else {
            addNote(notes, std::format("stream scale {:.6f}: LRF half assumed to be the 3776 px crop (unverified: "
                                       "no usable focal lengths to tell the mode)",
                                       s.scale));
        }
        return s;
    }

    // Rule 4: generic fallback from the focal lengths (4K and unknown modes).
    //
    // Without both focal lengths there is no ratio to take.  That used to
    // be an error, and the clip did not open at all; the 3776 px crop prior
    // is what every other rule assumes when the file says nothing, so the
    // clip now opens on it, flagged unverified.
    if (!usableFocal(digitalFocalLength) || !usableFocal(calFxMean)) {
        return priorCrop(s, streamW, sensorW, notes,
                         std::format("no rule for {}x{} from {}x{} and no usable focal lengths ({} / {})", streamW,
                                     streamH, sensorW, sensorH, digitalFocalLength, calFxMean));
    }

    // Which lens width does digital_focal_length describe?  On a full-size
    // stream it is the stream itself (ratio 1.000 on every clip measured).
    // A proxy repeats its parent's value, so the field names a lens 1.9-3.75x
    // wider, and digital_focal_length / fx is the PARENT's scale: on a 1024 px
    // half it builds a lens ~3.7x too long.  Rule 3 catches the Osmo 360's
    // own LRF halves; this catches every other proxy (another size, another
    // sensor) before the ratio is misread.
    const double dflLensW = digitalFocalLength / kDflPerLensPx;
    if (dflLensW >= kProxyWidthRatio * static_cast<double>(streamW)) {
        // A value naming an absurdly wide parent is garbage, not a parent:
        // the prior, rather than a stream size no camera records.
        const double maxParentW =
            std::min(kMaxParentPerSensor * static_cast<double>(std::max(sensorW, sensorH)), kMaxParentSide);
        const double parentHf = dflLensW * static_cast<double>(streamH) / static_cast<double>(streamW);
        if (!(dflLensW <= maxParentW) || !(parentHf <= kMaxParentSide)) {
            return priorCrop(s, streamW, sensorW, notes,
                             std::format("digital_focal_length {:.4f} names a {:.0f} px lens, wider than any stream "
                                         "this {}x{} sensor records",
                                         digitalFocalLength, dflLensW, sensorW, sensorH));
        }
        // The parent's size, keeping the proxy's aspect.  parentW > streamW
        // (the ratio is at least 1.4 and the rounding moves it by half a
        // pixel at most), so parentW >= 2 and the parent's own ratio is
        // within 0.5 / parentW of 1: the call below can never be taken for a
        // proxy again, and recurses exactly once.
        const int parentW = static_cast<int>(std::lround(dflLensW));
        const int parentH = static_cast<int>(std::lround(parentHf));
        if (parentW <= streamW || parentH <= 0) {
            return priorCrop(s, streamW, sensorW, notes,
                             std::format("digital_focal_length {:.4f} names no usable parent for a {}x{} proxy",
                                         digitalFocalLength, streamW, streamH));
        }
        // The proxy shows its parent's field of view (the premise Rule 3 was
        // measured on), so it takes the scale the parent gets from these same
        // rules, shrunk by the width ratio.
        std::vector<std::string> parentNotes;
        Result<StreamScaling> parent = derive(parentW, parentH, sensorW, sensorH, digitalFocalLength, calFxMean,
                                              std::nullopt, &parentNotes);
        if (!parent.ok() || !std::isfinite(parent.value().scale) || parent.value().scale <= 0.0) {
            return priorCrop(s, streamW, sensorW, notes,
                             std::format("the {}x{} parent named by digital_focal_length {:.4f} has no usable scale",
                                         parentW, parentH, digitalFocalLength));
        }
        const double parentScale = parent.value().scale;
        double scale = parentScale * static_cast<double>(streamW) / static_cast<double>(parentW);
        // Never let a proxy show less than kMinProxyCropWidth sensor px (or
        // the whole sensor, on one narrower than that): a scale beyond it
        // is a parent's focal applied to a proxy, whatever produced it.
        const double capCropW = std::min(kMinProxyCropWidth, static_cast<double>(sensorW));
        const double cap = static_cast<double>(streamW) / capCropW;
        const bool capped = !(scale <= cap);
        if (capped) {
            scale = cap;
        }
        s.scale = scale;
        s.verified = false;
        addNote(notes, std::format("stream scale {:.6f}: a {}x{} proxy of a {}x{} stream (digital_focal_length {:.4f} "
                                   "names that width), the parent's {:.6f} x {}/{} (unverified){}{}",
                                   s.scale, streamW, streamH, parentW, parentH, digitalFocalLength, parentScale,
                                   streamW, parentW,
                                   capped ? std::format("; capped at {}/{:.0f}", streamW, capCropW) : std::string(),
                                   parentNotes.empty() ? std::string() : "; parent: " + parentNotes.front()));
        return s;
    }

    // A full-size stream: its own recorded focal over the calibration's.
    s.scale = digitalFocalLength / calFxMean;
    s.verified = false;
    addNote(notes, std::format("stream scale {:.6f} = digital_focal_length {:.4f} / calibration fx {:.4f} (unverified)",
                               s.scale, digitalFocalLength, calFxMean));
    // Log only: the field should name this stream's width.
    addConventionNote(notes, digitalFocalLength, streamW);
    return s;
}

}  // namespace osv::geom
