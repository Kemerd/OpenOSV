// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeCpu.cpp - geometry setup and the CPU pixel loop.
//
// Every output pixel is produced by osvReframeEquirectPixel() from
// include/osv/render/osv_kernel.h.  Nothing in this file reimplements the
// projection, the sampler or the half-float decoder; it only decides WHERE
// the picture goes (the letterbox rectangle), WHICH WAY the camera points
// (Rout) and HOW the float result is stored in the host's buffer.
//
// Coordinate conventions, all from docs/GEOMETRY.md and docs/PREMIERE.md:
//   * body / view frame: X right, Y forward, Z up;
//   * the input equirect is in the Standard layout, so the centre column
//     looks along +Y and row 0 is the north pole;
//   * Rout = R_source * R_camera, i.e. the source orientation is applied
//     after the virtual camera, which is what lets a user level a tilted
//     capture once and then reframe inside the levelled sphere;
//   * output row 0 is the TOP row, always: the kernel works top-down and the
//     FrameView flags translate that to the host's own row order.

#include "ReframeCpu.h"

#include "PluginLog.h"

#include "osv/core/Math.h"
#include "osv/geom/VirtualCamera.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osv::reframe {

namespace {

/// Largest frame edge we will touch.  A corrupt PPix header claiming a
/// gigantic size must be rejected before it is multiplied into an offset.
constexpr int kMaxEdge = 65536;

/// Round-to-nearest-even IEEE 754 binary32 -> binary16.  The inverse
/// (osvHalfToFloat) lives in the shared kernel header and is used by the
/// sampler; the forward direction is only ever needed when STORING, so it
/// stays on this side.  Written with integer arithmetic only so MSVC, nvcc
/// and OpenCL would all produce the same bits.
[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept {
    // Reinterpret through a union: the one type pun every dialect accepts.
    union {
        float f;
        std::uint32_t u;
    } pun;
    pun.f = value;
    const std::uint32_t bits = pun.u;
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7FFFFFu;

    // Infinity and NaN keep their class; a NaN keeps a non-zero payload so
    // it does not silently become an infinity.
    if (((bits >> 23) & 0xFFu) == 0xFFu) {
        if (mantissa != 0u) {
            return static_cast<std::uint16_t>(sign | 0x7E00u);
        }
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }

    if (exponent >= 0x1F) {
        // Overflow: saturate to infinity, which is what the hardware
        // conversion instruction does as well.
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }

    if (exponent <= 0) {
        // Subnormal (or zero): shift the implicit one back in and round.
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        const std::uint32_t m = mantissa | 0x800000u;
        const int shift = 14 - exponent;  // 24 - 10 - exponent
        const std::uint32_t half = m >> shift;
        // Round to nearest even on the bits we dropped.
        const std::uint32_t rem = m & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1);
        std::uint32_t rounded = half;
        if (rem > halfway || (rem == halfway && (half & 1u) != 0u)) {
            rounded += 1u;
        }
        return static_cast<std::uint16_t>(sign | rounded);
    }

    // Normal: keep 10 mantissa bits, round to nearest even.
    std::uint32_t half = (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    const std::uint32_t rem = mantissa & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0u)) {
        half += 1u;  // may carry into the exponent, which is exactly right
    }
    return static_cast<std::uint16_t>(sign | half);
}

/// Clamp to [0, 1] and quantise to an 8-bit code with rounding; NaN -> 0.
[[nodiscard]] std::uint8_t floatTo8u(float value) noexcept {
    if (!(value > 0.0f)) {  // false for NaN as well
        return 0u;
    }
    if (value >= 1.0f) {
        return 255u;
    }
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

/// Clamp to [0, 1] and quantise to a Premiere 16-bit code with rounding;
/// NaN -> 0.
///
/// White is kBgra16uWhite (32768), not 65535 - see the constant's own comment
/// for the SDK citation.  The `!(value > 0.0f)` spelling is deliberate: it is
/// false for NaN as well as for negatives, so a NaN that survived the kernel
/// becomes transparent black instead of an undefined conversion (a float ->
/// integer cast of NaN is undefined behaviour in C++, not merely
/// unspecified).  The upper bound is tested BEFORE the multiply so a large
/// finite value cannot overflow the product either.
[[nodiscard]] std::uint16_t floatTo16u(float value) noexcept {
    if (!(value > 0.0f)) {
        return 0u;
    }
    if (value >= 1.0f) {
        return static_cast<std::uint16_t>(kBgra16uWhite);
    }
    return static_cast<std::uint16_t>(value * kBgra16uWhite + 0.5f);
}

/// Store one RGBA float quadruple as BGRA in the requested layout.
void storePixel(void* dst, PixelLayout layout, const float rgba[4]) noexcept {
    switch (layout) {
        case PixelLayout::Bgra32f: {
            float* p = static_cast<float*>(dst);
            p[0] = rgba[2];  // B
            p[1] = rgba[1];  // G
            p[2] = rgba[0];  // R
            p[3] = rgba[3];  // A
            break;
        }
        case PixelLayout::Bgra16f: {
            std::uint16_t* p = static_cast<std::uint16_t*>(dst);
            p[0] = floatToHalf(rgba[2]);
            p[1] = floatToHalf(rgba[1]);
            p[2] = floatToHalf(rgba[0]);
            p[3] = floatToHalf(rgba[3]);
            break;
        }
        case PixelLayout::Bgra8u: {
            std::uint8_t* p = static_cast<std::uint8_t*>(dst);
            p[0] = floatTo8u(rgba[2]);
            p[1] = floatTo8u(rgba[1]);
            p[2] = floatTo8u(rgba[0]);
            p[3] = floatTo8u(rgba[3]);
            break;
        }
        case PixelLayout::Bgra16u: {
            // Not memcpy-able as one 64-bit word: the destination is only
            // guaranteed to be 2-byte aligned (a host row pitch need not be a
            // multiple of 8), so the four stores stay separate and let the
            // compiler merge them when it can prove the alignment.
            std::uint16_t* p = static_cast<std::uint16_t*>(dst);
            p[0] = floatTo16u(rgba[2]);
            p[1] = floatTo16u(rgba[1]);
            p[2] = floatTo16u(rgba[0]);
            p[3] = floatTo16u(rgba[3]);
            break;
        }
    }
}

/// Shared validity rule for both frame views.
[[nodiscard]] bool frameValid(const void* base, std::int32_t rowBytes, int width, int height,
                              PixelLayout layout) noexcept {
    if (!base || width <= 0 || height <= 0 || width > kMaxEdge || height > kMaxEdge) {
        return false;
    }
    if (rowBytes == 0) {
        return false;
    }
    // The pitch must cover one whole row whichever way it points.
    const std::size_t stride = static_cast<std::size_t>(rowBytes < 0 ? -static_cast<std::int64_t>(rowBytes)
                                                                    : static_cast<std::int64_t>(rowBytes));
    return stride >= static_cast<std::size_t>(width) * bytesPerPixel(layout);
}

/// Address of top-down row `y` for a base that may point at either end of
/// the image and a pitch that may be negative.
[[nodiscard]] const char* topDownRow(const char* base, std::int32_t rowBytes, int height, bool topDown,
                                     int y) noexcept {
    // A bottom-up buffer stores image row (height-1-y) at memory row y.
    const int memoryRow = topDown ? y : (height - 1 - y);
    return base + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(memoryRow);
}

}  // namespace

// ---------------------------------------------------------------------------
//  FrameView / ConstFrameView
// ---------------------------------------------------------------------------
bool FrameView::valid() const noexcept { return frameValid(base, rowBytes, width, height, layout); }

void* FrameView::rowTopDown(int y) const noexcept {
    return const_cast<char*>(topDownRow(static_cast<const char*>(base), rowBytes, height, topDown, y));
}

const void* FrameView::constRowTopDown(int y) const noexcept {
    return topDownRow(static_cast<const char*>(base), rowBytes, height, topDown, y);
}

bool ConstFrameView::valid() const noexcept { return frameValid(base, rowBytes, width, height, layout); }

const void* ConstFrameView::constRowTopDown(int y) const noexcept {
    return topDownRow(static_cast<const char*>(base), rowBytes, height, topDown, y);
}

bool sourceRowsRunForward(const ConstFrameView& src) noexcept {
    if (!src.valid()) {
        return false;
    }
    // A single-row frame has no stride to get wrong.
    if (src.height <= 1) {
        return true;
    }
    // Image row 0 must sit at a LOWER address than image row 1; the two
    // inversions (bottom-up storage and a negative pitch) cancel, which is
    // why this is written as the actual comparison rather than as two flags.
    const char* row0 = static_cast<const char*>(src.constRowTopDown(0));
    const char* row1 = static_cast<const char*>(src.constRowTopDown(1));
    return row1 > row0;
}

// ---------------------------------------------------------------------------
//  Host parameter identification (declared in ReframeParams.h)
// ---------------------------------------------------------------------------
bool matchHostParams(const HostParamKind* kinds, int count, HostParamMap* outMap) noexcept {
    // ---- defensive gate on everything that came from the host -------------
    // A null list, a negative count or a list longer than our own can never
    // be our parameter set; refusing here is what keeps the caller's fallback
    // path the ONLY other outcome.
    if (!kinds || !outMap || count <= 0 || count > kValueParamCount) {
        return false;
    }

    // The host list is our eleven controls with one contiguous run removed,
    // so it is fully described by two numbers: where the gap starts (in OUR
    // index space) and how long it is.  The length follows from the count,
    // which leaves a single unknown to search over.
    const int gapLength = kValueParamCount - count;

    // Every possible position an EMPTY gap could sit in describes the same
    // alignment, so a zero-length gap has exactly one candidate, not twelve.
    // Without this the uniqueness rule below would reject the unreduced
    // 11-entry case as "ambiguous" purely because the loop enumerated the
    // same answer repeatedly.
    const int gapPositions = (gapLength == 0) ? 1 : (kValueParamCount - gapLength + 1);

    int matchedGapStart = -1;
    int matchCount = 0;
    for (int gapStart = 0; gapStart < gapPositions; ++gapStart) {
        // Walk our eleven controls, skipping the candidate gap, and compare
        // each survivor with the next host entry.  Consuming the host list
        // strictly left to right is what enforces rule 3 (no leftovers): the
        // walk ends with hostPos == count or it is not a match at all.
        int hostPos = 0;
        bool ok = true;
        for (int ours = 0; ours < kValueParamCount; ++ours) {
            if (ours >= gapStart && ours < gapStart + gapLength) {
                continue;  // this control is one the host did not expose
            }
            if (hostPos >= count) {
                ok = false;  // ran out of host entries: not this alignment
                break;
            }
            // HostParamKind::Unknown is deliberately excluded by the
            // equality test: an entry we could not read matches nothing, so
            // a failed GetParam can only ever reduce the candidate set.
            if (kinds[hostPos] != kValueParamKind[ours]) {
                ok = false;
                break;
            }
            ++hostPos;
        }
        if (!ok || hostPos != count) {
            continue;
        }
        ++matchCount;
        matchedGapStart = gapStart;
        // Two viable alignments mean the answer is a coin toss.  Stop and
        // refuse rather than commit to one of them (rule 1).
        if (matchCount > 1) {
            return false;
        }
    }
    if (matchCount != 1 || matchedGapStart < 0) {
        return false;
    }

    // ---- build the table --------------------------------------------------
    // Start from "no host entry" everywhere, so the four group markers - and
    // any control inside the gap - are left at -1 and the caller falls back
    // to their documented defaults instead of reading a neighbour's value.
    HostParamMap map{};
    for (int i = 0; i <= OSV_REFRAME_PARAM_COUNT; ++i) {
        map.hostIndex[i] = -1;
    }
    int hostPos = 0;
    for (int ours = 0; ours < kValueParamCount; ++ours) {
        if (ours >= matchedGapStart && ours < matchedGapStart + gapLength) {
            continue;
        }
        map.hostIndex[kValueParamAeIndex[ours]] = hostPos;
        ++hostPos;
    }
    map.probed = true;
    *outMap = map;
    return true;
}

// ---------------------------------------------------------------------------
//  Aspect resolution and the letterbox rectangle (declared in ReframeParams.h)
// ---------------------------------------------------------------------------
double resolveAspectRatio(Aspect aspect, double sequenceAspect, double frameAspect) noexcept {
    switch (aspect) {
        case Aspect::MatchSequence:
            // The Sequence Info Suite answers on a real host; without it
            // (older host, suite hidden, a timeline id we cannot resolve)
            // docs/PREMIERE.md prescribes 16:9.
            if (std::isfinite(sequenceAspect) && sequenceAspect > 0.0) {
                return sequenceAspect;
            }
            return 16.0 / 9.0;
        case Aspect::FullFrame:
            // Whatever shape the frame is; a degenerate frame falls back to
            // 16:9 rather than producing a zero-width viewport.
            if (std::isfinite(frameAspect) && frameAspect > 0.0) {
                return frameAspect;
            }
            return 16.0 / 9.0;
        default:
            break;
    }
    for (const AspectEntry& e : kAspects) {
        if (e.value == aspect && e.heightUnits > 0.0) {
            return e.widthUnits / e.heightUnits;
        }
    }
    return 16.0 / 9.0;
}

Viewport computeViewport(int frameW, int frameH, double aspectRatio) noexcept {
    Viewport v;
    if (frameW <= 0 || frameH <= 0) {
        return v;  // zeroed: the caller treats w/h == 0 as invalid
    }
    if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0) {
        // Unusable ratio: fill the frame instead of producing nothing.
        v.x = 0;
        v.y = 0;
        v.w = frameW;
        v.h = frameH;
        return v;
    }

    // Largest rectangle of the requested shape that fits, centred.  Compare
    // in doubles, then round to whole pixels and clamp: the rounding can
    // otherwise produce w = frameW + 1 for a ratio that is a hair too wide.
    const double frameRatio = static_cast<double>(frameW) / static_cast<double>(frameH);
    int w = frameW;
    int h = frameH;
    if (aspectRatio > frameRatio) {
        // Wider than the frame: full width, letterbox above and below.
        h = static_cast<int>(std::lround(static_cast<double>(frameW) / aspectRatio));
    } else if (aspectRatio < frameRatio) {
        // Taller than the frame: full height, pillarbox left and right.
        w = static_cast<int>(std::lround(static_cast<double>(frameH) * aspectRatio));
    }
    w = std::clamp(w, 1, frameW);
    h = std::clamp(h, 1, frameH);

    v.w = w;
    v.h = h;
    // Centre, biasing the odd pixel to the left / top so a 1-pixel remainder
    // is deterministic rather than depending on rounding mode.
    v.x = (frameW - w) / 2;
    v.y = (frameH - h) / 2;
    return v;
}

// ---------------------------------------------------------------------------
//  Kernel parameter construction
// ---------------------------------------------------------------------------
/// Names for the rejection reasons, so a log line says which check fired
/// instead of only that one did.
const char* setupRejectName(SetupReject reason) noexcept {
    switch (reason) {
    case SetupReject::None:             return "none";
    case SetupReject::SourceInvalid:    return "source frame invalid";
    case SetupReject::OutputSize:       return "output size out of range";
    case SetupReject::Viewport:         return "empty letterbox viewport";
    case SetupReject::DegenerateCamera: return "degenerate virtual camera";
    case SetupReject::NeedsPromotion:   return "integer source was not promoted to float";
    case SetupReject::RowsBackwards:    return "source rows run backwards in memory";
    case SetupReject::SourcePointer:    return "null row pointer or non-positive pitch";
    }
    // Unreachable for any enumerator above; a value from a corrupt read lands
    // here rather than off the end of a table.
    return "unknown";
}

KernelSetup buildParams(const Settings& settings, const ConstFrameView& src, int outW, int outH,
                        double sequenceAspect) noexcept {
    KernelSetup setup;

    // ---- defensive checks on everything that came from the host ----------
    if (!src.valid()) {
        setup.reject = SetupReject::SourceInvalid;
        return setup;
    }
    if (outW <= 0 || outH <= 0 || outW > kMaxEdge || outH > kMaxEdge) {
        setup.reject = SetupReject::OutputSize;
        return setup;
    }

    // ---- the letterbox rectangle -----------------------------------------
    const double frameAspect = static_cast<double>(outW) / static_cast<double>(outH);
    const double ratio = resolveAspectRatio(settings.aspect, sequenceAspect, frameAspect);
    const Viewport view = computeViewport(outW, outH, ratio);
    if (view.w <= 0 || view.h <= 0) {
        setup.reject = SetupReject::Viewport;
        return setup;
    }

    // ---- the virtual camera ----------------------------------------------
    // Distortion is a percentage of the eye offset d; everything else comes
    // straight from the controls.  Non-finite values (a corrupt project, an
    // expression that produced NaN) are replaced with the defaults rather
    // than propagated into the rotation matrix.
    const double distortion = std::isfinite(settings.distortion) ? std::clamp(settings.distortion, 0.0, 100.0)
                                                                 : OSV_REFRAME_DISTORTION_DEFAULT;
    const double fov = std::isfinite(settings.fovDeg)
                           ? std::clamp(settings.fovDeg, OSV_REFRAME_FOV_VALID_MIN, OSV_REFRAME_FOV_VALID_MAX)
                           : OSV_REFRAME_FOV_DEFAULT;
    const double pan = std::isfinite(settings.panDeg) ? settings.panDeg : 0.0;
    // Tilt is clamped in code (the dial itself is unbounded so it can be
    // keyframed smoothly through a turn).
    const double tilt = std::isfinite(settings.tiltDeg)
                            ? std::clamp(settings.tiltDeg, -OSV_REFRAME_TILT_LIMIT_DEG, OSV_REFRAME_TILT_LIMIT_DEG)
                            : 0.0;
    const double roll = std::isfinite(settings.rollDeg) ? settings.rollDeg : 0.0;
    const double srcPan = std::isfinite(settings.sourcePanDeg) ? settings.sourcePanDeg : 0.0;
    const double srcTilt = std::isfinite(settings.sourceTiltDeg)
                               ? std::clamp(settings.sourceTiltDeg, -OSV_REFRAME_TILT_LIMIT_DEG,
                                            OSV_REFRAME_TILT_LIMIT_DEG)
                               : 0.0;
    const double srcRoll = std::isfinite(settings.sourceRollDeg) ? settings.sourceRollDeg : 0.0;

    // The library's own camera does the focal-length maths, including the
    // clamp that keeps the eye-offset model invertible (the effective field
    // of view is never allowed to reach 2 acos(-d)).
    geom::VirtualCamera camera;
    camera.projection = geom::Projection::EyeOffset;
    camera.w = view.w;
    camera.h = view.h;
    camera.hfovDeg = fov;
    camera.eyeOffset = distortion / 100.0;
    camera.yawDeg = pan;
    camera.pitchDeg = tilt;
    camera.rollDeg = roll;
    // The focal length is the one number a bad parameter read can turn into
    // something unusable, so it gets a second chance rather than killing the
    // frame.
    //
    // A field of view that is merely WRONG has already been clamped into
    // [OSV_REFRAME_FOV_VALID_MIN, OSV_REFRAME_FOV_VALID_MAX] above, and a
    // non-finite one replaced by the default, so reaching here with a
    // non-positive focal means the camera is degenerate for some other
    // reason.  Retrying at the default field of view costs one more
    // focalPx() call and turns a dead effect into a visibly wrong one - and
    // a frame the user can see is a frame they can report.  Returning an
    // invalid setup here is what made a single bad FOV read black the whole
    // effect out; only a camera that is degenerate AT THE DEFAULT is beyond
    // saving, and that can only be a NaN or infinity the clamps let through.
    double focal = camera.focalPx();
    if (!(focal > 0.0) || !std::isfinite(focal)) {
        const double requested = camera.hfovDeg;
        camera.hfovDeg = OSV_REFRAME_FOV_DEFAULT;
        focal = camera.focalPx();
        if (focal > 0.0 && std::isfinite(focal)) {
            osv::premiere::PluginLog::oncef(
                "reframe/geometry/fov-fallback", osv::premiere::PluginLog::Level::Warn,
                "reframe: field of view {} deg (distortion {}%) gives no usable focal length; "
                "rendering at the default {} deg instead",
                requested, distortion, static_cast<double>(OSV_REFRAME_FOV_DEFAULT));
        } else {
            // Genuinely degenerate: a NaN or infinity that survived every
            // clamp, or a zero-sized viewport.  Better no frame than a frame
            // of NaN, which would poison everything downstream of us.
            osv::premiere::PluginLog::oncef(
                "reframe/geometry/degenerate", osv::premiere::PluginLog::Level::Error,
                "reframe: degenerate virtual camera ({}x{} viewport, fov {} deg, distortion {}%); "
                "no frame can be built",
                view.w, view.h, requested, distortion);
            setup.reject = SetupReject::DegenerateCamera;
            return setup;
        }
    }

    // Rout = R_source * R_camera.  Both use the same Rz(yaw) Rx(pitch)
    // Ry(roll) order so a source rotation reads exactly like a camera one.
    geom::VirtualCamera sourceCamera;
    sourceCamera.yawDeg = srcPan;
    sourceCamera.pitchDeg = srcTilt;
    sourceCamera.rollDeg = srcRoll;
    const Mat3d rout = sourceCamera.rotation() * camera.rotation();

    // ---- fill the plain-old-data blocks ----------------------------------
    OsvReframeParams& p = setup.params;
    p.outW = outW;
    p.outH = outH;
    p.viewX = view.x;
    p.viewY = view.y;
    p.viewW = view.w;
    p.viewH = view.h;
    p.projection = OSV_PROJ_EYE_OFFSET;
    p.eyeOffset = static_cast<float>(camera.eyeOffset);
    p.focalPx = static_cast<float>(focal);
    // The rectilinear helpers are unused by the eye-offset branch but are
    // filled anyway so the struct never carries stale garbage into a device
    // buffer (and so a future projection switch needs no extra plumbing).
    const double halfFov = 0.5 * camera.effectiveHfovDeg() * kPi / 180.0;
    const double tanHalf = std::tan(std::min(halfFov, 1.55));  // guard the pole
    p.tanHalfH = static_cast<float>(tanHalf);
    p.tanHalfV = static_cast<float>(tanHalf * static_cast<double>(view.h) / static_cast<double>(view.w));
    for (int i = 0; i < 9; ++i) {
        p.Rout[i] = static_cast<float>(rout.m[i]);
    }
    // The letterbox must be transparent, but inside the picture the alpha of
    // the panorama is what the user sees; the importer emits opaque frames
    // so this is 1 there anyway.  Leaving it at 0 means "use the sampled
    // alpha", which is the honest behaviour for an arbitrary input clip.
    p.fillAlphaOne = 0;

    // Integer-coded sources (8u, 16u) are not supported by the kernel's
    // sampler - it reads float or half only - so they are rejected here
    // rather than reinterpreted as float and read four or two times too far.
    //
    // This is NOT the effect refusing the format: the render path promotes
    // such a frame to float BEFORE calling buildParams (see
    // promoteIntegerToFloat), so by the time the setup is built the source is
    // always float or half.  The check remains as the backstop that turns a
    // caller who forgot to promote into an invalid setup instead of an
    // out-of-bounds read.
    if (layoutNeedsPromotion(src.layout)) {
        setup.reject = SetupReject::NeedsPromotion;
        return setup;
    }

    // The sampler cannot walk rows backwards (see sourceRowsRunForward).
    if (!sourceRowsRunForward(src)) {
        setup.reject = SetupReject::RowsBackwards;
        return setup;
    }

    OsvRgbaSource& s = setup.source;
    s.w = src.width;
    s.h = src.height;
    s.isHalf = (src.layout == PixelLayout::Bgra16f) ? 1 : 0;
    s.isBgra = 1;  // every layout we accept is BGRA
    // Image row 0 is the top row; the check above guarantees the rows then
    // run forward, so the pitch the kernel wants is simply |rowBytes|.
    s.pitchBytes = static_cast<int>(src.rowBytes < 0 ? -static_cast<std::int64_t>(src.rowBytes)
                                                     : static_cast<std::int64_t>(src.rowBytes));
    setup.sourceRow0 = src.constRowTopDown(0);
    if (!setup.sourceRow0 || s.pitchBytes <= 0) {
        setup.reject = SetupReject::SourcePointer;
        return setup;
    }

    setup.valid = true;
    setup.reject = SetupReject::None;
    return setup;
}

// ---------------------------------------------------------------------------
//  The pixel loop
// ---------------------------------------------------------------------------
bool renderPixel(const KernelSetup& setup, const ConstFrameView& src, int px, int py, float out[4]) noexcept {
    if (!out) {
        return false;
    }
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!setup.valid || !setup.sourceRow0 || !src.valid()) {
        return false;
    }
    osvReframeEquirectPixel(&setup.params, &setup.source, setup.sourceRow0, px, py, out);
    return true;
}

bool renderCpu(const KernelSetup& setup, const ConstFrameView& src, const FrameView& dst, ThreadPool* pool) noexcept {
    // Validate everything before a single byte is written: a partially
    // rendered frame reaching the host is worse than an untouched one.
    if (!setup.valid || !setup.sourceRow0) {
        return false;
    }
    if (!src.valid() || !dst.valid()) {
        return false;
    }
    if (dst.width != setup.params.outW || dst.height != setup.params.outH) {
        return false;
    }

    const std::size_t bpp = bytesPerPixel(dst.layout);
    if (bpp == 0) {
        return false;
    }

    // One row at a time.  Each row is independent, reads only the (const)
    // source and writes only its own destination row, which is why
    // PF_OutFlag2_SUPPORTS_THREADED_RENDERING is safe to declare.
    const auto renderRow = [&](std::size_t row) noexcept {
        const int y = static_cast<int>(row);
        char* dstRow = static_cast<char*>(dst.rowTopDown(y));
        for (int x = 0; x < dst.width; ++x) {
            float rgba[4];
            osvReframeEquirectPixel(&setup.params, &setup.source, setup.sourceRow0, x, y, rgba);
            storePixel(dstRow + static_cast<std::ptrdiff_t>(x) * static_cast<std::ptrdiff_t>(bpp), dst.layout, rgba);
        }
    };

    if (pool) {
        // A grain of one row is plenty: a 4K row is ~4000 kernel evaluations,
        // far more than the cost of taking a chunk from the queue.
        const Status status = pool->parallelRows(static_cast<std::size_t>(dst.height), 1, renderRow);
        if (!status.ok()) {
            return false;
        }
        return true;
    }

    for (int y = 0; y < dst.height; ++y) {
        renderRow(static_cast<std::size_t>(y));
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Integer input promotion
// ---------------------------------------------------------------------------

ConstFrameView promoteIntegerToFloat(const ConstFrameView& src, std::vector<float>& scratch,
                                     ThreadPool* pool) noexcept {
    // An invalid view is returned on every failure path: the caller tests
    // valid() and never sees a partially filled buffer.
    ConstFrameView out;

    if (!layoutNeedsPromotion(src.layout) || !src.valid()) {
        return out;
    }

    // Guard the allocation size before asking for it.  width and height are
    // already bounded by ConstFrameView::valid(), but the product is what
    // actually gets allocated, so it is the product that must be checked.
    const std::size_t pixels = static_cast<std::size_t>(src.width) * static_cast<std::size_t>(src.height);
    const std::size_t floats = pixels * 4u;
    if (pixels == 0 || floats / 4u != pixels) {
        return out;
    }

    try {
        scratch.resize(floats);
    } catch (...) {
        // A frame this big is a legitimate out-of-memory; say so by staying
        // invalid rather than by letting the exception cross into the host.
        return out;
    }

    float* const dstBase = scratch.data();
    if (!dstBase) {
        return out;
    }

    const int width = src.width;
    const bool is16u = (src.layout == PixelLayout::Bgra16u);
    // The reciprocal of the layout's white point, computed ONCE outside the
    // pixel loop.  Both values are exact powers of two divided by an integer
    // - 1/255 is not, 1/32768 is - so the 16u path is exact and the 8u path
    // reproduces the previous behaviour bit for bit.
    const float scale = is16u ? (1.0f / kBgra16uWhite) : (1.0f / 255.0f);

    // One row per job.  The promoted buffer is packed and top-down, so row y
    // of the output is always dstBase + y * width * 4 regardless of how the
    // source stores its rows - constRowTopDown() hides that.
    //
    // The 8u / 16u branch is taken once per ROW, not once per component: the
    // two loops are written out separately so neither carries a per-pixel
    // test, and the sample type is a compile-time constant inside each.
    auto promoteRow = [&src, dstBase, width, is16u, scale](std::size_t rowIndex) noexcept {
        const int y = static_cast<int>(rowIndex);
        const void* inRaw = src.constRowTopDown(y);
        float* outRow = dstBase + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
        if (!inRaw || !outRow) {
            return;
        }
        // Codes * (1 / white) is the exact inverse of the matching store
        // path's quantisation, so a promote-then-store round trip is lossless
        // for every code.  The channel order is preserved: the sampler is
        // told isBgra, so it reorders, and reordering here as well would swap
        // R and B twice.
        //
        // Nothing is clamped on the way IN.  A 16-bit host world may legally
        // carry a code above kBgra16uWhite (the SDK calls those out of gamut,
        // not invalid), and clamping it here would crush highlight detail
        // before the resample instead of carrying it through and letting the
        // store path decide.
        if (is16u) {
            const std::uint16_t* in = static_cast<const std::uint16_t*>(inRaw);
            for (int x = 0; x < width; ++x) {
                const std::size_t o = static_cast<std::size_t>(x) * 4u;
                outRow[o + 0] = static_cast<float>(in[o + 0]) * scale;
                outRow[o + 1] = static_cast<float>(in[o + 1]) * scale;
                outRow[o + 2] = static_cast<float>(in[o + 2]) * scale;
                outRow[o + 3] = static_cast<float>(in[o + 3]) * scale;
            }
            return;
        }
        const std::uint8_t* in = static_cast<const std::uint8_t*>(inRaw);
        for (int x = 0; x < width; ++x) {
            const std::size_t o = static_cast<std::size_t>(x) * 4u;
            outRow[o + 0] = static_cast<float>(in[o + 0]) * scale;
            outRow[o + 1] = static_cast<float>(in[o + 1]) * scale;
            outRow[o + 2] = static_cast<float>(in[o + 2]) * scale;
            outRow[o + 3] = static_cast<float>(in[o + 3]) * scale;
        }
    };

    if (pool) {
        const Status status = pool->parallelRows(static_cast<std::size_t>(src.height), 1, promoteRow);
        if (!status.ok()) {
            return out;
        }
    } else {
        for (int y = 0; y < src.height; ++y) {
            promoteRow(static_cast<std::size_t>(y));
        }
    }

    // The promoted view is packed, top-down and positively pitched, which is
    // exactly the one arrangement sourceRowsRunForward() accepts - so the
    // promotion also makes the two rejected row arrangements renderable when
    // they arrive in an integer layout.
    out.base = dstBase;
    out.rowBytes = static_cast<std::int32_t>(static_cast<std::size_t>(width) * 4u * sizeof(float));
    out.width = width;
    out.height = src.height;
    out.layout = PixelLayout::Bgra32f;
    out.topDown = true;
    return out;
}

}  // namespace osv::reframe
