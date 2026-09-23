// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeCpu.cpp - geometry setup and the CPU pixel loop.
//
// Every output pixel is produced by osvReframeEquirectPixel() from
// include/osv/render/osv_kernel.h.  Nothing in this file reimplements the
// projection, the sampler or the half-float decoder; it only decides WHERE
// the picture goes (the render rectangle, which is the whole frame), WHICH
// WAY the camera points
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
#include "osv/geom/Presets.h"
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
//  Pixel store (declared in ReframeCpu.h)
// ---------------------------------------------------------------------------
void storePixel(void* dst, PixelLayout layout, const float rgba[4]) noexcept {
    // A null destination or source is a caller bug; ignoring it is the only
    // answer that cannot corrupt memory.
    if (!dst || !rgba) {
        return;
    }
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
/// Identify the host list from the one structure in it that cannot move.
///
/// WHY A SECOND MATCHER
/// --------------------
/// matchHostParams() walks the whole signature and needs every entry to agree.
/// That is the right thing when the host answers every GetParam, but Premiere
/// Pro 26.2 does not: a session log shows it reporting ELEVEN entries for this
/// effect while refusing to return a type for four of them -
///
///     [0]i32:8 [1]=<err> [2]i32:1 [3]f32:42.0 [4]f32:-15.0 [5]f32:7.0
///     [6]f64:100.0 [7]f64:30.0 [8]=<err> [9]=<err> [10]=<err>
///
/// - and those refusals sit exactly where the exact walk needs a type.  The
/// walk therefore failed on every such call and the caller fell back to the
/// static "AE index - 1" mapping, which for THIS list reads one control too
/// early: Pan was read out of the Preset slot and FOV out of Roll's.  Playback
/// reached the renderer by a different route, which is why playback looked
/// right while scrubbing and keyframe stepping did not.
///
/// THE ANCHOR
/// ----------
/// Our eleven controls contain exactly one adjacent Float64 PAIR - FOV and
/// Distortion are the only Float64 sliders in the effect (kValueParamKind).
/// A host list that contains exactly one adjacent Float64 pair therefore
/// pins the alignment with no ambiguity at all, whatever it did with the
/// entries it could not type: the offset is (where the pair is) minus (where
/// we put it), and every other control follows by counting.  In the log above
/// the pair sits at 6/7 against our 5/6, giving offset +1 - which places Pan,
/// Tilt and Roll at 3, 4 and 5, and the VALUES at those indices (42, -15, 7)
/// are exactly what the user had dialled in.  That is the confirmation that
/// the offset is real and not a coincidence of types.
///
/// Anything less certain is refused: no pair, or more than one, means the
/// anchor is not unique and the caller keeps its documented fallback.
///
/// [WP-CAMERA] GENERALISED FROM ONE PAIR TO ONE OFFSET
/// ---------------------------------------------------
/// The DJI camera block appended four more Float64 sliders, so "the only
/// adjacent Float64 pair" no longer exists in our own signature.  What the
/// anchor really established was an OFFSET between the host list and ours,
/// and that is what is searched for now: every offset is tried, a host entry
/// that the host typed must agree with our control at that offset (one
/// contradiction rules the offset out, exactly as before), and the offset
/// that explains the most typed entries wins - provided it is the ONLY one
/// that explains that many, it explains at least five, and among them is an
/// adjacent Float64 pair (the anchor the old rule demanded).  For the logged
/// list above the answer is still +1, and for every list the old rule
/// accepted it is the same offset.
[[nodiscard]] bool matchByFov64Pair(const HostParamKind* kinds, int count, HostParamMap* outMap) noexcept {
    if (!kinds || !outMap || count <= 0) {
        return false;
    }

    // ---- score every offset --------------------------------------------------
    // host = ours + offset, so the offsets that put at least one of our
    // controls inside the host list run from -(ours - 1) to (count - 1).
    int bestOffset = 0;
    int bestMatches = -1;
    int bestTies = 0;
    for (int offset = -(kValueParamCount - 1); offset <= count - 1; ++offset) {
        int matches = 0;
        // [WP-CAMERA] The evidence must come from the ORIGINAL controls: the
        // appended tail is a checkbox, four float sliders and ([WP-LENSUI])
        // a popup, a pattern common enough that a list of nothing else would
        // otherwise "match" at some offset without a single original control
        // agreeing.
        int originalMatches = 0;
        bool contradicted = false;
        bool anchored = false;
        for (int ours = 0; ours < kValueParamCount; ++ours) {
            const int host = ours + offset;
            if (host < 0 || host >= count || kinds[host] == HostParamKind::Unknown) {
                continue;  // outside the host list, or a type the host refused
            }
            if (kinds[host] != kValueParamKind[ours]) {
                contradicted = true;  // at this offset the list is not ours
                break;
            }
            ++matches;
            if (ours < kOriginalValueParamCount) {
                ++originalMatches;
            }
            // The anchor: two adjacent host entries agreeing as Float64.
            if (kValueParamKind[ours] == HostParamKind::Float64 && ours + 1 < kValueParamCount &&
                host + 1 < count && kinds[host + 1] == HostParamKind::Float64 &&
                kValueParamKind[ours + 1] == HostParamKind::Float64) {
                anchored = true;
            }
        }
        // The anchor pair and at least three more agreeing entries: a pair and
        // one neighbour could be coincidence in any short list.  All five
        // must be original controls (see originalMatches above).
        if (contradicted || !anchored || originalMatches < 5) {
            continue;
        }
        if (matches > bestMatches) {
            bestMatches = matches;
            bestOffset = offset;
            bestTies = 1;
        } else if (matches == bestMatches) {
            ++bestTies;
        }
    }
    // No candidate, or two offsets explain the list equally well: no single
    // right answer, so the caller keeps its documented fallback.
    if (bestMatches < 0 || bestTies != 1) {
        return false;
    }

    // ---- lay our controls down at that offset -----------------------------
    const int offset = bestOffset;
    HostParamMap map{};
    for (int i = 0; i <= OSV_REFRAME_PARAM_COUNT; ++i) {
        map.hostIndex[i] = -1;
    }
    // Only slots the host actually TYPED are mapped.
    //
    // A slot whose type the host refused is left at -1 so the control keeps
    // its documented default.  That matters because a host that cannot report
    // an entry's type usually cannot return its value either - the mock host
    // in the tests fails both with suiteError_InvalidParms, and a real
    // Premiere that refused the type gave us <err> for the value in the same
    // dump.  Mapping such a slot on the anchor's word alone would replace a
    // known-good default with whatever a failed GetParam left behind, which
    // is how an unset Output Aspect lost its letterbox, back when there was
    // one to lose.
    //
    // A slot the host DID type and that CONTRADICTS ours is fatal rather than
    // skippable: at this offset the list is demonstrably not our parameter
    // set, and mapping the rest would mean reading someone else's controls.
    int mapped = 0;
    for (int ours = 0; ours < kValueParamCount; ++ours) {
        const int host = ours + offset;
        if (host < 0 || host >= count) {
            continue;
        }
        if (kinds[host] == HostParamKind::Unknown) {
            continue;
        }
        if (kinds[host] != kValueParamKind[ours]) {
            return false;
        }
        map.hostIndex[kValueParamAeIndex[ours]] = host;
        ++mapped;
    }

    // The anchor itself is two of those; demanding more than the pair alone
    // keeps a nearly-empty list from being "identified" by coincidence.
    if (mapped < 5) {
        return false;
    }
    map.probed = true;
    *outMap = map;
    return true;
}

bool matchHostParams(const HostParamKind* kinds, int count, HostParamMap* outMap) noexcept {
    // ---- defensive gate on everything that came from the host -------------
    // A null list, a negative count or a list longer than our own can never
    // be our parameter set; refusing here is what keeps the caller's fallback
    // path the ONLY other outcome.
    if (!kinds || !outMap || count <= 0 || count > kValueParamCount) {
        return false;
    }

    // The host list is a PREFIX of our controls (a host may stop before the
    // appended DJI block) with one contiguous run removed, so it is fully
    // described by three numbers: how long the prefix is, where the gap
    // starts (in OUR index space) and how long the gap is.  The gap length
    // follows from the other two, which leaves a small space to search.
    //
    // Distinct alignments can describe the SAME table - an empty gap in any
    // position, or a gap at the very end of a longer prefix, is just a
    // shorter prefix - so uniqueness (rule 1) is judged on the tables the
    // alignments produce, not on how many alignments were enumerated.
    // Without that the unreduced case would be rejected as "ambiguous"
    // purely because the loops found the same answer more than once.
    //
    // [WP-CAMERA] The search runs from the SHORTEST gap up and stops at the
    // first gap length that explains the list: an explanation that hides
    // fewer controls in the middle of our list wins over one that hides
    // more.  That is not a guess but the only reading that keeps a known
    // layout identifiable once the list grows: the 8-entry Premiere list
    // "... FOV Distortion <bool>" is our original list with the Source group
    // hidden (gap 3), and with the Camera Model checkbox appended it could
    // also be read as "Source AND Smooth Keyframes hidden, the bool is Camera
    // Model" (gap 4) - a layout no host has produced, since Smooth Keyframes
    // sits in no group.  Two DIFFERENT tables at the same gap length are
    // still refused.
    //
    // The prefix may only end inside the appended block (it is at least
    // kOriginalValueParamCount long): a host that predates the DJI controls
    // is a real case, a host that stops before Smooth Keyframes is not, and
    // allowing it would let a two-entry "f64 f64" list pass as FOV and
    // Distortion with everything around them missing.  For the same reason
    // the gap must lie within the ORIGINAL controls - the hidden run the
    // compact layout really produces is the collapsed Source group - so a
    // list made only of appended controls can never be read as "the whole
    // original list hidden".
    //
    // And the host must still expose at least five of the original controls
    // - the same evidence bar the Float64 anchor below demands - so a hidden
    // run can never swallow so much of the list that what is left matches by
    // coincidence.
    static_assert(kOriginalValueParamCount > 0 && kOriginalValueParamCount <= kValueParamCount,
                  "the original value controls must be a prefix of the value list");
    constexpr int kMinOriginalShown = 5;
    constexpr int kMaxGap = kOriginalValueParamCount - kMinOriginalShown;
    HostParamMap found{};
    bool haveFound = false;
    const int minGap = (count < kOriginalValueParamCount) ? (kOriginalValueParamCount - count) : 0;
    const int maxGap = std::min(kValueParamCount - count, kMaxGap);
    for (int gapLength = minGap; gapLength <= maxGap && !haveFound; ++gapLength) {
        const int prefix = count + gapLength;
        const int gapPositions = (gapLength == 0) ? 1 : (prefix - gapLength + 1);
        for (int gapStart = 0; gapStart < gapPositions; ++gapStart) {
            if (gapLength > 0 && gapStart + gapLength > kOriginalValueParamCount) {
                continue;  // a hidden run reaching into the appended block
            }
            // Walk the prefix, skipping the candidate gap, and compare each
            // survivor with the next host entry.  Consuming the host list
            // strictly left to right is what enforces rule 3 (no leftovers):
            // the walk ends with hostPos == count or it is not a match.
            HostParamMap candidate{};
            for (int i = 0; i <= OSV_REFRAME_PARAM_COUNT; ++i) {
                candidate.hostIndex[i] = -1;
            }
            int hostPos = 0;
            bool ok = true;
            for (int ours = 0; ours < prefix; ++ours) {
                if (ours >= gapStart && ours < gapStart + gapLength) {
                    continue;  // this control is one the host did not expose
                }
                if (hostPos >= count) {
                    ok = false;  // ran out of host entries: not this alignment
                    break;
                }
                // HostParamKind::Unknown is deliberately excluded by the
                // equality test: an entry we could not read matches nothing,
                // so a failed GetParam can only ever reduce the candidate set.
                // (When the host refuses so many types that this exact match
                // fails outright, matchByFov64Pair below takes over.)
                if (kinds[hostPos] != kValueParamKind[ours]) {
                    ok = false;
                    break;
                }
                // Controls in the gap - and every group marker - stay at -1,
                // so the caller falls back to their documented defaults
                // instead of reading a neighbour's value.
                candidate.hostIndex[kValueParamAeIndex[ours]] = hostPos;
                ++hostPos;
            }
            if (!ok || hostPos != count) {
                continue;
            }
            if (!haveFound) {
                found = candidate;
                haveFound = true;
                continue;
            }
            // A second viable alignment: harmless if it is the same table,
            // a coin toss if it is not - and then we refuse rather than
            // commit to one of them (rule 1).
            for (int i = 0; i <= OSV_REFRAME_PARAM_COUNT; ++i) {
                if (found.hostIndex[i] != candidate.hostIndex[i]) {
                    return false;
                }
            }
        }
    }
    if (!haveFound) {
        // The exact walk could not identify the list.  Before giving up and
        // letting the caller use the static mapping - which is the mapping
        // that reads the WRONG control - try the structural anchor below.
        return matchByFov64Pair(kinds, count, outMap);
    }

    found.probed = true;
    *outMap = found;
    return true;
}

// ---------------------------------------------------------------------------
//  Output size and the render rectangle (declared in ReframeParams.h)
// ---------------------------------------------------------------------------
SizePx resolveOutputSize(Resolution resolution, SizePx sequenceSize, SizePx frameSize) noexcept {
    // A fixed entry is a pair of literals from the table and needs no host
    // cooperation at all, so it is answered first and can never fail.
    for (const ResolutionEntry& e : kResolutions) {
        if (e.value == resolution && e.width > 0 && e.height > 0) {
            return SizePx{e.width, e.height};
        }
    }

    // "Match Sequence" (and any value that fell through the table, which
    // sanitiseResolution should already have prevented).
    //
    // The Sequence Info Suite is the authority when it answered.  When it did
    // not - an older host, a hidden suite, a timeline id we could not resolve
    // during a UI event - the frame the host handed us is the next best thing
    // and is, in the ordinary case, exactly the same number.
    if (sequenceSize.valid()) {
        return sequenceSize;
    }
    if (frameSize.valid()) {
        return frameSize;
    }
    // Neither is usable.  Return an invalid size rather than inventing one:
    // buildParams() turns that into a clean rejection with a named reason,
    // which is far easier to diagnose than a frame silently rendered at a
    // resolution nobody asked for.
    return SizePx{};
}

Viewport computeViewport(int frameW, int frameH) noexcept {
    Viewport v;
    if (frameW <= 0 || frameH <= 0) {
        return v;  // zeroed: the caller treats w/h == 0 as invalid
    }
    // The picture always covers the whole frame.  See the header for why the
    // letterbox is gone; this function remains so that the one place that
    // decides "which pixels get painted" is still named and testable, and so
    // that confining the render to a sub-rectangle stays a one-line change
    // here rather than a change to the shared kernel.
    v.x = 0;
    v.y = 0;
    v.w = frameW;
    v.h = frameH;
    return v;
}

// ---------------------------------------------------------------------------
//  Automatic projection ramp (declared in ReframeParams.h)
// ---------------------------------------------------------------------------
double autoEyeOffsetForFov(double fovDeg) noexcept {
    // Garbage in gives the feature's identity element, not a NaN that would
    // propagate into the focal length and kill the frame.
    if (!std::isfinite(fovDeg)) {
        return 0.0;
    }
    constexpr double kStart = OSV_REFRAME_AUTO_EYE_FOV_START;
    constexpr double kFull = OSV_REFRAME_AUTO_EYE_FOV_FULL;
    // If the two constants were ever edited into a degenerate or inverted
    // pair the division below would be by zero or the ramp would run
    // backwards.  That is a build-time mistake, so it fails the build.
    static_assert(kFull > kStart, "the automatic eye-offset ramp must end after it starts");
    if (fovDeg <= kStart) {
        return 0.0;
    }
    if (fovDeg >= kFull) {
        return 1.0;
    }
    const double t = (fovDeg - kStart) / (kFull - kStart);
    // Smoothstep: zero derivative at both ends, so the curvature eases in at
    // 120 deg and eases out at 240 deg with no perceptible kink at either
    // join (see the header for the full reasoning).
    return t * t * (3.0 - 2.0 * t);
}

double effectiveEyeOffset(double distortionPercent, double fovDeg) noexcept {
    // The manual slider, sanitised exactly as buildParams() used to do it
    // inline: a non-finite value becomes the documented default rather than
    // poisoning the maximum below.
    const double manual = std::isfinite(distortionPercent)
                              ? std::clamp(distortionPercent, 0.0, 100.0) / 100.0
                              : (OSV_REFRAME_DISTORTION_DEFAULT / 100.0);
    const double automatic = autoEyeOffsetForFov(fovDeg);
    // Maximum, so the ramp is a FLOOR the user can always exceed and never a
    // ceiling that overrides them.
    const double d = manual > automatic ? manual : automatic;
    // The eye-offset model is only defined on [0, 1]; the clamp is redundant
    // given the two inputs above but costs nothing and makes this function's
    // postcondition true by construction rather than by argument.
    return std::clamp(d, 0.0, 1.0);
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
    case SetupReject::Viewport:         return "empty render viewport";
    case SetupReject::DegenerateCamera: return "degenerate virtual camera";
    case SetupReject::NeedsPromotion:   return "integer source was not promoted to float";
    case SetupReject::RowsBackwards:    return "source rows run backwards in memory";
    case SetupReject::SourcePointer:    return "null row pointer or non-positive pitch";
    }
    // Unreachable for any enumerator above; a value from a corrupt read lands
    // here rather than off the end of a table.
    return "unknown";
}

// ---------------------------------------------------------------------------
//  [WP-CAMERA] DJI camera helpers (declared in ReframeParams.h)
// ---------------------------------------------------------------------------
namespace {

/// The shape every parameter-UI computation falls back to when no frame and
/// no sequence can be asked: DJI Studio's default canvas.
constexpr double kDefaultFramingAspect = 16.0 / 9.0;

/// A finite, positive aspect or the 16:9 fallback.
[[nodiscard]] double aspectOr169(double aspect) noexcept {
    return (std::isfinite(aspect) && aspect > 0.0) ? aspect : kDefaultFramingAspect;
}

/// Rout = R_source * R_camera, the body <- view rotation of every lens model.
///
/// Both factors use the same Rz(yaw) Rx(pitch) Ry(roll) order
/// (geom::VirtualCamera::rotation) so a source rotation reads exactly like a
/// camera one.  Non-finite angles become 0; the two tilts are clamped to
/// +-OSV_REFRAME_TILT_LIMIT_DEG in code because the dials themselves are
/// unbounded (so a user can keyframe smoothly through a turn).
[[nodiscard]] Mat3d viewRotation(const Settings& settings) noexcept {
    const double pan = std::isfinite(settings.panDeg) ? settings.panDeg : 0.0;
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

    geom::VirtualCamera camera;
    camera.yawDeg = pan;
    camera.pitchDeg = tilt;
    camera.rollDeg = roll;
    geom::VirtualCamera sourceCamera;
    sourceCamera.yawDeg = srcPan;
    sourceCamera.pitchDeg = srcTilt;
    sourceCamera.rollDeg = srcRoll;
    return sourceCamera.rotation() * camera.rotation();
}

/// buildView()'s DJI half: the frame checks and the requested size are
/// already done by the caller, and `reqWxOutH` / `reqHxOutW` are its exact
/// cover-fit cross products.
///
/// DJI's field of view is VERTICAL and spans the HEIGHT of the requested
/// picture (GLKMatrix4MakePerspective's fovy; DJI's plug-in renders into a
/// viewport of the chosen resolution).  That picture is cover-fitted onto
/// the frame exactly like the Classic one:
///
///   requested the same shape, or relatively WIDER than the frame
///       its height is the frame height, the sides are cropped:
///       f = (outH / 2) / tan(fov / 2) - and for the same shape (Match
///       Sequence, every preview-scaled frame) that is exact, with no
///       floating-point ratio anywhere near it;
///   requested relatively TALLER
///       its width is the frame width, so its height in frame pixels is
///       outW * reqH / reqW and the top and bottom are cropped.
///
/// The eye distance is the Correction Angle as the user typed it (no ramp:
/// DJI has none), and the kernel's OSV_PROJ_DJI_SPHERE does the rest.
[[nodiscard]] ViewSetup buildDjiView(const Settings& settings, const Viewport& view, int outW, int outH,
                                     std::int64_t reqWxOutH, std::int64_t reqHxOutW, SizePx requestedSize) noexcept {
    ViewSetup setup;
    if (view.w <= 0 || view.h <= 0 || !requestedSize.valid()) {
        setup.reject = SetupReject::Viewport;
        return setup;
    }

    // ---- the lens ------------------------------------------------------------
    const DjiLens lens = sanitiseDjiLens(DjiLens{settings.djiFovDeg, settings.correction});

    // ---- which edge of the requested picture meets the frame -----------------
    double halfSpanPx = 0.5 * static_cast<double>(outH);
    if (reqWxOutH < reqHxOutW) {
        halfSpanPx = 0.5 * static_cast<double>(outW) * static_cast<double>(requestedSize.h) /
                     static_cast<double>(requestedSize.w);
    }

    // ---- the pinhole focal length --------------------------------------------
    // The field of view is inside [1, 178] after sanitising, so the tangent is
    // finite and positive; the check below is the backstop for a clamp that
    // somebody widens past 180 in future.
    const double t = std::tan(0.5 * lens.fovDeg * kPi / 180.0);
    const double focal = (std::isfinite(t) && t > 0.0) ? halfSpanPx / t : 0.0;
    if (!(focal > 0.0) || !std::isfinite(focal)) {
        osv::premiere::PluginLog::oncef("reframe/geometry/dji-degenerate", osv::premiere::PluginLog::Level::Error,
                                        "reframe: degenerate DJI camera ({}x{} frame, FOV {} deg, correction {}); "
                                        "no frame can be built",
                                        outW, outH, lens.fovDeg, lens.correction);
        setup.reject = SetupReject::DegenerateCamera;
        return setup;
    }

    // ---- fill the plain-old-data block ----------------------------------------
    const Mat3d rout = viewRotation(settings);
    OsvReframeParams& p = setup.params;
    p.outW = outW;
    p.outH = outH;
    p.viewX = view.x;
    p.viewY = view.y;
    p.viewW = view.w;
    p.viewH = view.h;
    p.projection = OSV_PROJ_DJI_SPHERE;
    // The eye distance travels in the eye-offset slot; OSV_PROJ_DJI_SPHERE
    // documents that it may exceed 1 (the eye outside the sphere).
    p.eyeOffset = static_cast<float>(lens.correction);
    p.focalPx = static_cast<float>(focal);
    // The tangent-plane half extents of the frame through this pinhole - the
    // same meaning the rectilinear helpers have for every other projection.
    p.tanHalfH = static_cast<float>(0.5 * static_cast<double>(view.w) / focal);
    p.tanHalfV = static_cast<float>(0.5 * static_cast<double>(view.h) / focal);
    for (int i = 0; i < 9; ++i) {
        p.Rout[i] = static_cast<float>(rout.m[i]);
    }
    // Rays that miss the sphere (eye outside it, Crystal Ball) come back
    // transparent, which on Premiere's black is DJI's black surround.
    p.fillAlphaOne = 0;

    setup.valid = true;
    setup.reject = SetupReject::None;
    return setup;
}

}  // namespace

DjiLens sanitiseDjiLens(DjiLens lens) noexcept {
    DjiLens out;
    out.fovDeg = std::isfinite(lens.fovDeg)
                     ? std::clamp(lens.fovDeg, OSV_REFRAME_DJI_FOV_VALID_MIN, OSV_REFRAME_DJI_FOV_VALID_MAX)
                     : OSV_REFRAME_DJI_FOV_DEFAULT;
    out.correction = std::isfinite(lens.correction)
                         ? std::clamp(lens.correction, OSV_REFRAME_CORRECTION_VALID_MIN, OSV_REFRAME_CORRECTION_VALID_MAX)
                         : OSV_REFRAME_CORRECTION_DEFAULT;
    return out;
}

double djiZoomDeg(DjiLens lens, double aspect) noexcept {
    // Sanitised first so a NaN control reads as its default, never as 0.
    const DjiLens s = sanitiseDjiLens(lens);
    return geom::djiZoomDeg(s.fovDeg, s.correction, aspectOr169(aspect));
}

DjiLens djiZoomTo(double targetZoomDeg, DjiLens from, double aspect) noexcept {
    const DjiLens start = sanitiseDjiLens(from);
    const double a = aspectOr169(aspect);
    if (!std::isfinite(targetZoomDeg)) {
        return start;  // nothing sensible to aim at: leave the lens alone
    }

    // DJI Studio's limits for the path, widened to include the start so a
    // lens outside them (a Crystal Ball, a project typed in from the plug-in)
    // is never snapped back by the first zoom edit.
    const double fovMin = std::min(OSV_REFRAME_DJI_STUDIO_FOV_MIN, start.fovDeg);
    const double fovMax = std::max(OSV_REFRAME_DJI_STUDIO_FOV_MAX, start.fovDeg);
    const double corMin = std::min(OSV_REFRAME_CORRECTION_VALID_MIN, start.correction);
    const double corMax = std::max(OSV_REFRAME_DJI_STUDIO_CORRECTION_MAX, start.correction);
    constexpr double kRate = OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION;

    // The lens at path parameter delta: both controls move, each clamped.
    const auto at = [&](double delta) noexcept {
        DjiLens l;
        l.fovDeg = std::clamp(start.fovDeg + kRate * delta, fovMin, fovMax);
        l.correction = std::clamp(start.correction + delta, corMin, corMax);
        return l;
    };

    // The whole path: from both controls at their minimum to both at their
    // maximum.  Zoom never decreases along it (both controls only grow), so
    // bisection finds the one delta with the requested Zoom.
    double lo = std::min((fovMin - start.fovDeg) / kRate, corMin - start.correction);
    double hi = std::max((fovMax - start.fovDeg) / kRate, corMax - start.correction);
    if (djiZoomDeg(at(lo), a) >= targetZoomDeg) {
        return at(lo);
    }
    if (djiZoomDeg(at(hi), a) <= targetZoomDeg) {
        return at(hi);
    }
    // 64 halvings take any path length below double resolution.
    for (int i = 0; i < 64; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (djiZoomDeg(at(mid), a) < targetZoomDeg) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return at(0.5 * (lo + hi));
}

DjiLens djiFromClassic(ClassicLens classic, double aspect) noexcept {
    const double a = aspectOr169(aspect);

    // The Classic lens exactly as the renderer resolves it: the same clamps,
    // the same automatic eye-offset ramp, the same invertibility clamp.
    const double distortion = std::isfinite(classic.distortion) ? std::clamp(classic.distortion, 0.0, 100.0)
                                                                : OSV_REFRAME_DISTORTION_DEFAULT;
    const double fov = std::isfinite(classic.fovDeg)
                           ? std::clamp(classic.fovDeg, OSV_REFRAME_FOV_VALID_MIN, OSV_REFRAME_FOV_VALID_MAX)
                           : OSV_REFRAME_FOV_DEFAULT;
    geom::VirtualCamera camera;
    camera.projection = geom::Projection::EyeOffset;
    camera.hfovDeg = fov;
    camera.eyeOffset = effectiveEyeOffset(distortion, fov);

    // The visible half angle across the width, and the pinhole half angle
    // that reaches it from an eye camera.eyeOffset radii behind the centre.
    const double visibleHalf = 0.5 * camera.effectiveHfovDeg() * kPi / 180.0;
    const double alpha = geom::djiPinholeHalfAngleRad(visibleHalf, camera.eyeOffset);
    DjiLens lens;
    lens.correction = camera.eyeOffset;
    lens.fovDeg = (alpha > 0.0) ? 2.0 * std::atan(std::tan(alpha) / a) * 180.0 / kPi : OSV_REFRAME_DJI_FOV_DEFAULT;
    return sanitiseDjiLens(lens);
}

ClassicLens classicFromDji(DjiLens lens, double aspect) noexcept {
    const DjiLens s = sanitiseDjiLens(lens);
    ClassicLens classic;
    // The visible angle across the width is exactly what Classic FOV means.
    const double zoom = geom::djiZoomDeg(s.fovDeg, s.correction, aspectOr169(aspect));
    classic.fovDeg = (zoom > 0.0) ? std::clamp(zoom, OSV_REFRAME_FOV_VALID_MIN, OSV_REFRAME_FOV_VALID_MAX)
                                  : OSV_REFRAME_FOV_DEFAULT;
    classic.distortion = std::clamp(100.0 * s.correction, OSV_REFRAME_DISTORTION_VALID_MIN,
                                    OSV_REFRAME_DISTORTION_VALID_MAX);
    return classic;
}

double djiPresetFovDeg(const PresetEntry& preset, double aspect) noexcept {
    // One mapping rule from shape to DJI column, shared with the library.
    geom::DjiPreset columns{};
    columns.vfovLandscapeDeg = preset.djiFovLandscapeDeg;
    columns.vfovPortrait916Deg = preset.djiFovPortrait916Deg;
    columns.vfovPortrait34Deg = preset.djiFovPortrait34Deg;
    return geom::djiPresetVfovDeg(columns, aspect);
}

double framingAspect(Resolution resolution, SizePx sequenceSize) noexcept {
    // The named resolution, else the sequence; no frame exists here, so the
    // last resort is DJI Studio's default canvas shape.
    const SizePx size = resolveOutputSize(resolution, sequenceSize, SizePx{});
    if (!size.valid()) {
        return kDefaultFramingAspect;
    }
    return static_cast<double>(size.w) / static_cast<double>(size.h);
}

ViewSetup buildView(const Settings& settings, int outW, int outH, SizePx sequenceSize) noexcept {
    ViewSetup setup;

    // ---- defensive checks on everything that came from the host ----------
    if (outW <= 0 || outH <= 0 || outW > kMaxEdge || outH > kMaxEdge) {
        setup.reject = SetupReject::OutputSize;
        return setup;
    }

    // ---- the rectangle we paint ------------------------------------------
    // Always the whole frame: a virtual camera fills its sensor, so there is
    // no letterbox to compute and none to leave black.  The chosen
    // resolution below decides the camera's FRAMING (see coverScale), never
    // its coverage.
    const Viewport view = computeViewport(outW, outH);
    if (view.w <= 0 || view.h <= 0) {
        setup.reject = SetupReject::Viewport;
        return setup;
    }

    // ---- what resolution did the user ask for? ---------------------------
    // The size is what the control NAMES, which is not necessarily the size
    // of the frame the host gave us: Premiere renders previews and scrubs at
    // a fraction of full resolution, so `outW x outH` is routinely smaller.
    //
    // Only its SHAPE is used below.  The host allocates the output world, so
    // no control value can change how many pixels this function is asked to
    // fill; what the named size can honestly decide is which shape the field
    // of view is measured across when that shape differs from the frame.
    const SizePx requestedSize = resolveOutputSize(settings.resolution, sequenceSize, SizePx{outW, outH});
    if (!requestedSize.valid()) {
        // Nothing could tell us a size: no sequence, and a frame that failed
        // its own validity check above (which cannot happen here, but the
        // branch costs nothing and makes the postcondition unconditional).
        setup.reject = SetupReject::OutputSize;
        return setup;
    }

    // ---- cover-fit the requested shape onto the frame --------------------
    // The field of view spans the WIDTH of the requested resolution.  That
    // image is then scaled uniformly until it covers the whole frame, and
    // whatever overflows is cropped - never letterboxed.
    //
    //   requested relatively WIDER than the frame  (reqW / reqH > outW / outH)
    //       the height binds: the virtual image is outH tall and wider than
    //       the frame, so the sides are cropped and the visible horizontal
    //       field of view is narrower than FOV.  The focal length grows by
    //       virtualWidth / outW = (reqW * outH) / (reqH * outW) > 1.
    //
    //   requested the same shape, or TALLER
    //       the width binds: FOV spans the frame width exactly as before, the
    //       top and bottom (if any) are cropped, and the factor is 1.
    //
    // The comparison is done in EXACT integer arithmetic (int64 cross
    // products of two positive ints, which cannot overflow).  That matters: for "Match Sequence" and
    // for every preview-scaled frame the shapes are identical, the factor is
    // exactly 1.0 and the render is bit-for-bit the full-frame render - a
    // floating-point ratio test would let a rounding error nudge the focal
    // length on frames where nothing was asked to change.
    //
    // A factor >= 1 only ever NARROWS the visible field of view, so the
    // eye-offset invertibility clamp VirtualCamera applies below (measured
    // across the frame width) remains a valid bound for what is shown.
    const std::int64_t reqWxOutH = static_cast<std::int64_t>(requestedSize.w) * static_cast<std::int64_t>(outH);
    const std::int64_t reqHxOutW = static_cast<std::int64_t>(requestedSize.h) * static_cast<std::int64_t>(outW);
    double coverScale = 1.0;
    if (reqWxOutH > reqHxOutW && reqHxOutW > 0) {
        coverScale = static_cast<double>(reqWxOutH) / static_cast<double>(reqHxOutW);
    }
    if (!std::isfinite(coverScale) || !(coverScale >= 1.0)) {
        // Unreachable with validated sizes, but a non-finite factor would
        // turn into a NaN focal length and a NaN frame, so it is neutralised.
        coverScale = 1.0;
    }

    // ---- [WP-CAMERA] DJI's camera ----------------------------------------
    // A separate lens with its own parameterisation; everything above (the
    // frame, the requested size) is shared, and the Classic code below
    // computes exactly what it always did (its six angles moved into
    // viewRotation() with the same arithmetic), so a Classic render - every
    // old project - is bit for bit what it was.
    if (settings.cameraModel == CameraModel::Dji) {
        return buildDjiView(settings, view, outW, outH, reqWxOutH, reqHxOutW, requestedSize);
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
    // The eye offset the camera will actually use: the Distortion slider
    // raised to the automatic ramp's floor for this field of view.  A user
    // who only ever drags FOV therefore slides from rectilinear towards
    // stereographic on the way out - the "Tiny Planet" behaviour - while a
    // user who does touch Distortion is never overridden, because the two are
    // combined with a maximum.  effectiveEyeOffset() documents the choice.
    //
    // Note it is computed from the CLAMPED fov, not the raw setting, so the
    // ramp and the focal length below always agree about which field of view
    // is being rendered.
    const double eyeOffset = effectiveEyeOffset(distortion, fov);

    // The library's own camera does the focal-length maths, including the
    // clamp that keeps the eye-offset model invertible (the effective field
    // of view is never allowed to reach 2 acos(-d)).  Its orientation comes
    // from viewRotation() below, the one place the six angles are sanitised
    // for both lens models.
    geom::VirtualCamera camera;
    camera.projection = geom::Projection::EyeOffset;
    camera.w = view.w;
    camera.h = view.h;
    camera.hfovDeg = fov;
    camera.eyeOffset = eyeOffset;
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

    // Rout = R_source * R_camera (see viewRotation()).
    const Mat3d rout = viewRotation(settings);

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
    // The cover-fit factor is applied HERE, after the fallback above, so the
    // degenerate-camera logic still reasons about the unscaled camera and a
    // factor of exactly 1.0 leaves the focal length untouched.
    p.focalPx = static_cast<float>(focal * coverScale);
    // The rectilinear helpers are unused by the eye-offset branch but are
    // filled anyway so the struct never carries stale garbage into a device
    // buffer (and so a future projection switch needs no extra plumbing).
    // They are divided by the same cover factor so they keep describing the
    // same (cropped) picture the focal length does: tan(half fov) = (W/2) / f.
    const double halfFov = 0.5 * camera.effectiveHfovDeg() * kPi / 180.0;
    const double tanHalf = std::tan(std::min(halfFov, 1.55)) / coverScale;  // guard the pole
    p.tanHalfH = static_cast<float>(tanHalf);
    p.tanHalfV = static_cast<float>(tanHalf * static_cast<double>(view.h) / static_cast<double>(view.w));
    for (int i = 0; i < 9; ++i) {
        p.Rout[i] = static_cast<float>(rout.m[i]);
    }
    // The alpha of the panorama is what the user sees; the importer emits
    // opaque frames so this is 1 in practice.  Leaving it at 0 means "use the
    // sampled alpha", which is the honest behaviour for an arbitrary input
    // clip.  (There is no letterbox to keep transparent any more - the
    // picture covers the whole frame - but a ray that misses the sphere
    // entirely still comes back transparent, which is correct.)
    p.fillAlphaOne = 0;

    setup.valid = true;
    setup.reject = SetupReject::None;
    return setup;
}

KernelSetup buildParams(const Settings& settings, const ConstFrameView& src, int outW, int outH,
                        SizePx sequenceSize) noexcept {
    KernelSetup setup;

    // ---- defensive checks on everything that came from the host ----------
    // The source is checked FIRST, exactly as before the camera was split
    // out, so a call that is wrong in several ways still names the same
    // reason it always did.
    if (!src.valid()) {
        setup.reject = SetupReject::SourceInvalid;
        return setup;
    }

    // ---- the camera ---------------------------------------------------------
    // One function builds it for every renderer; see buildView() for the
    // cover-fit, the eye offset and the rotation order.
    const ViewSetup view = buildView(settings, outW, outH, sequenceSize);
    if (!view.valid) {
        setup.reject = view.reject;
        return setup;
    }
    setup.params = view.params;

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

    OsvRgbaSource& s = setup.source;
    s.w = src.width;
    s.h = src.height;
    s.isHalf = (src.layout == PixelLayout::Bgra16f) ? 1 : 0;
    s.isBgra = 1;  // every layout we accept is BGRA
    // The kernel wants a positive pitch, because it indexes rows with an
    // unsigned multiply; |rowBytes| is that pitch whichever way the rows run.
    s.pitchBytes = static_cast<int>(src.rowBytes < 0 ? -static_cast<std::int64_t>(src.rowBytes)
                                                     : static_cast<std::int64_t>(src.rowBytes));

    // Which end of the buffer does the kernel start from?
    //
    // Two of the four (topDown, sign-of-pitch) combinations put image row 0
    // at the LOWEST address and the rest ascending, which is what the sampler
    // reads natively.  The other two store the image the other way round, and
    // those used to be REFUSED - which is why the effect rendered nothing at
    // all for frames Premiere legitimately hands out (a log shows topDown
    // with rowBytes = -40960, rejected on every single frame).
    //
    // They are now handled instead: point at the LAST image row and set
    // flipY, so the kernel walks forward in memory while walking down the
    // image.  Every offset stays non-negative and no pixels are copied.
    if (sourceRowsRunForward(src)) {
        s.flipY = 0;
        setup.sourceRow0 = src.constRowTopDown(0);
    } else {
        s.flipY = 1;
        setup.sourceRow0 = src.constRowTopDown(src.height - 1);
    }
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
