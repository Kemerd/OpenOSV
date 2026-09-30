// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxHostImage.h - the pixel formats an OpenFX host can hand the effects,
// described once for the CPU loops (OfxRender.*, OfxSource.cpp) and the GPU
// paths (OfxGpuView.*, the CUDA kernels).
//
// ===========================================================================
//  Why this exists
// ===========================================================================
// DaVinci Resolve hands an OpenFX effect 32-bit float R, G, B, A images and
// nothing else, which is all the effects accepted until VEGAS Pro.  VEGAS
// works in two depths and two channel orders:
//
//   * 8-bit projects hand BYTE images, 32-bit projects FLOAT images (there
//     is no 16-bit);
//   * R, G, B, A is the OpenFX default, but VEGAS itself is B, G, R, A, and a
//     plug-in that lists the Sony extension depths "OfxBitDepthByteBGR" /
//     "OfxBitDepthFloatBGR" among its supported pixel depths may be handed
//     BGRA images directly (no host-side swizzle).  Each image then says
//     which order it is in through "OfxImageEffectPropPixelOrder".
//
// VEGAS also has "video levels" projects (studio RGB: black at 16, white at
// 235 of 255, in 8-bit AND in "32-bit floating point (video levels)"), and it
// never level-converts a generator's output.  A generator whose pixels must
// sit next to decoded footage therefore has to produce studio levels itself
// in such a project: OutputLevels below.
//
// ===========================================================================
//  Rows
// ===========================================================================
// Every image here is an OpenFX image: y points UP, `data` is the pixel at
// (bounds.x1, bounds.y1) - the BOTTOM-left corner - and row y starts at
//
//     data + (y - bounds.y1) * rowBytes
//
// `rowBytes` may exceed width * bytesPerPixel (padding) and may, in
// principle, be negative (a host that stores rows top-down).  Nothing in
// this header assumes a sign.
#pragma once

#include "ofxCore.h"

#include <cstddef>
#include <cstdint>

namespace osv::ofx {

// ===========================================================================
//  Formats
// ===========================================================================

/// Bits per channel of a host image.  OpenFX also knows 16-bit and half, but
/// no host the effects support hands them out, so they are refused upstream.
enum class HostDepth : std::uint8_t {
    Byte = 0,   ///< kOfxBitDepthByte: one unsigned byte per channel, 0..255.
    Float = 1,  ///< kOfxBitDepthFloat: one 32-bit IEEE float per channel.
};

/// Channel order of a host image's four components.
enum class HostOrder : std::uint8_t {
    Rgba = 0,  ///< The OpenFX default (and Resolve's only order).
    Bgra = 1,  ///< The Sony / VEGAS extension order.
};

/// The levels a generator writes its RGB in.  Alpha is never touched.
enum class OutputLevels : std::uint8_t {
    Full = 0,    ///< 0..1 (0..255): computer RGB, what Resolve and full-range VEGAS projects expect.
    Studio = 1,  ///< 16/255..235/255: studio RGB, what VEGAS video-levels projects expect.
};

// ---- the Sony / VEGAS OpenFX extension names (ofxSonyVegas.h) --------------
// The header is not part of OpenFX 1.5 any more, so its strings live here.

/// Clip / image property: the channel order of an image ("...RGBA" / "...BGRA").
inline constexpr const char* kPropPixelOrder = "OfxImageEffectPropPixelOrder";
/// Value of kPropPixelOrder for R, G, B, A.
inline constexpr const char* kPixelOrderRgba = "OfxImagePixelOrderRGBA";
/// Value of kPropPixelOrder for B, G, R, A.
inline constexpr const char* kPixelOrderBgra = "OfxImagePixelOrderBGRA";
/// Supported-pixel-depth token: "I also take BYTE images in B, G, R, A".
inline constexpr const char* kBitDepthByteBgr = "OfxBitDepthByteBGR";
/// Supported-pixel-depth token: "I also take FLOAT images in B, G, R, A".
inline constexpr const char* kBitDepthFloatBgr = "OfxBitDepthFloatBGR";

/// Bytes per pixel of a four-component image at `depth`.
[[nodiscard]] constexpr int bytesPerPixel(HostDepth depth) noexcept {
    return depth == HostDepth::Float ? 16 : 4;
}

// ===========================================================================
//  One host image, as the render loops see it
// ===========================================================================

/// A host image reduced to what a render loop needs: where the pixels are,
/// which rectangle of pixel space they cover, and how they are laid out.
/// It owns nothing - the ClipImage (or GPU staging buffer) it describes
/// must outlive it.
struct HostImageView {
    void* data = nullptr;              ///< The pixel at (bounds.x1, bounds.y1): the bottom-left corner.
    int rowBytes = 0;                  ///< Byte distance between row y and row y + 1 (may be negative).
    OfxRectI bounds{0, 0, 0, 0};       ///< The pixels the image covers (x2, y2 exclusive).
    HostDepth depth = HostDepth::Float;  ///< Bits per channel.
    HostOrder order = HostOrder::Rgba;   ///< Channel order.

    /// Width in pixels (0 for an inverted rectangle).
    [[nodiscard]] int width() const noexcept { return bounds.x2 > bounds.x1 ? bounds.x2 - bounds.x1 : 0; }

    /// Height in pixels (0 for an inverted rectangle).
    [[nodiscard]] int height() const noexcept { return bounds.y2 > bounds.y1 ? bounds.y2 - bounds.y1 : 0; }

    /// Bytes per pixel of this image's depth.
    [[nodiscard]] int pixelBytes() const noexcept { return bytesPerPixel(depth); }

    /// True when the view can be read or written safely: a data pointer, a
    /// non-empty rectangle, and a pitch that holds at least one full row.
    [[nodiscard]] bool usable() const noexcept {
        if (!data || width() <= 0 || height() <= 0) {
            return false;
        }
        // |rowBytes| without calling std::abs on INT_MIN.
        const long long pitch = rowBytes < 0 ? -static_cast<long long>(rowBytes) : static_cast<long long>(rowBytes);
        return pitch >= static_cast<long long>(width()) * pixelBytes();
    }

    /// The first byte of pixel (x, y), in OpenFX pixel coordinates.  The
    /// caller guarantees (x, y) lies inside `bounds`; nothing is checked here
    /// because this sits in the innermost loops.
    [[nodiscard]] char* pixel(int x, int y) const noexcept {
        return static_cast<char*>(data) +
               static_cast<std::ptrdiff_t>(y - bounds.y1) * static_cast<std::ptrdiff_t>(rowBytes) +
               static_cast<std::ptrdiff_t>(x - bounds.x1) * static_cast<std::ptrdiff_t>(pixelBytes());
    }
};

// ===========================================================================
//  Levels and quantisation - the one definition every path must match
// ===========================================================================
// The CUDA kernels cannot include this header (it is C++ and they are built
// by nvcc from a C ABI); they repeat these two formulas with the same
// constants, and the tests hold the GPU output to the CPU output below.

/// Studio-RGB black, as a fraction of full scale (16 of 255).
inline constexpr float kStudioBlack = 16.0f / 255.0f;
/// Studio-RGB span from black to white, as a fraction of full scale (219 of 255).
inline constexpr float kStudioSpan = 219.0f / 255.0f;

/// A full-range channel value in studio levels: 0 -> 16/255, 1 -> 235/255.
/// Linear, so float values outside [0, 1] (super-whites, HDR) keep their
/// meaning; only the byte conversion below clamps.
[[nodiscard]] constexpr float studioFromFull(float value) noexcept {
    return kStudioBlack + value * kStudioSpan;
}

/// `value` (an R, G or B channel) in the requested levels.  Never call this
/// on alpha: coverage is not a level.
[[nodiscard]] constexpr float applyLevels(float value, OutputLevels levels) noexcept {
    return levels == OutputLevels::Studio ? studioFromFull(value) : value;
}

/// A float channel as a byte code: clamped to [0, 1] and rounded to nearest,
/// exactly as plugins/common/PixelCopy.cpp quantises (value * 255 + 0.5,
/// truncated), so the OpenFX and Premiere byte paths agree code for code.
/// NaN becomes 0.
[[nodiscard]] inline std::uint8_t toByte(float value) noexcept {
    if (!(value > 0.0f)) {
        return 0;  // negative, zero and NaN
    }
    if (value >= 1.0f) {
        return 255;
    }
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

// ===========================================================================
//  One pixel in and out of a host image - the packing rule every CPU writer
//  shares (OfxRender.cpp), and the one the GPU pack kernels must reproduce
// ===========================================================================
// A render produces straight R, G, B, A floats at FULL range.  Storing one
// in a host image is always the same three steps, in this order:
//
//   1. levels:  applyLevels() on R, G and B - never on alpha;
//   2. depth:   Float keeps the floats as they are, Byte goes through
//               toByte() (clamp, round to nearest, NaN -> 0);
//   3. order:   Rgba stores R, G, B, A; Bgra stores B, G, R, A.
//
// Transparent black is no exception: it is (0, 0, 0, 0) BEFORE levels, so
// a Studio-levels image stores it as (16/255, 16/255, 16/255, 0) - code 16
// in a Byte image - wherever the effects write it (outside the camera frame,
// before or after the clip, a missing source).  Alpha 0 keeps it invisible
// wherever the host composites; the RGB stays at studio black for a host
// that does not.
//
// Reading is the exact inverse of the depth step (codes * 1/255, the same
// scale reframe::promoteIntegerToFloat() uses), without levels: the reframe
// filter moves pixels the host has already levelled.

/// Short name of a depth, for logs ("byte", "float").
[[nodiscard]] constexpr const char* hostDepthName(HostDepth depth) noexcept {
    return depth == HostDepth::Byte ? "byte" : "float";
}

/// Short name of a channel order, for logs ("RGBA", "BGRA").
[[nodiscard]] constexpr const char* hostOrderName(HostOrder order) noexcept {
    return order == HostOrder::Bgra ? "BGRA" : "RGBA";
}

/// Short name of an output levels choice, for logs ("full", "studio").
[[nodiscard]] constexpr const char* outputLevelsName(OutputLevels levels) noexcept {
    return levels == OutputLevels::Studio ? "studio" : "full";
}

/// Store the straight, full-range RGBA float quadruple `rgba` as one pixel
/// of a `depth` / `order` host image at `dst`, in `levels` (see above).
/// `dst` must address one whole pixel (bytesPerPixel(depth) bytes); a null
/// `dst` or `rgba` writes nothing.  A Float pixel is stored as four floats,
/// so `dst` must then be float-aligned, which every host image pixel is.
inline void storeHostPixel(void* dst, HostDepth depth, HostOrder order, OutputLevels levels,
                           const float rgba[4]) noexcept {
    if (!dst || !rgba) {
        return;
    }
    // Step 1: levels on colour, never on coverage.
    const float r = applyLevels(rgba[0], levels);
    const float g = applyLevels(rgba[1], levels);
    const float b = applyLevels(rgba[2], levels);
    const float a = rgba[3];
    // Step 3's order decides which colour lands in slot 0 and which in slot 2.
    const float first = (order == HostOrder::Bgra) ? b : r;
    const float third = (order == HostOrder::Bgra) ? r : b;
    // Step 2: the depth's own storage.
    if (depth == HostDepth::Byte) {
        std::uint8_t* out = static_cast<std::uint8_t*>(dst);
        out[0] = toByte(first);
        out[1] = toByte(g);
        out[2] = toByte(third);
        out[3] = toByte(a);
        return;
    }
    float* out = static_cast<float*>(dst);
    out[0] = first;
    out[1] = g;
    out[2] = third;
    out[3] = a;
}

/// Read one pixel of a `depth` / `order` host image at `src` as straight RGBA
/// floats (byte codes * 1/255, no levels: see above).  A null `src` or `rgba`
/// reads nothing.
inline void loadHostPixel(const void* src, HostDepth depth, HostOrder order, float rgba[4]) noexcept {
    if (!src || !rgba) {
        return;
    }
    float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (depth == HostDepth::Byte) {
        // Multiplying by the reciprocal, exactly as the promotion does, so a
        // test reference built here matches the promoted source bit for bit.
        const std::uint8_t* in = static_cast<const std::uint8_t*>(src);
        constexpr float kScale = 1.0f / 255.0f;
        for (int i = 0; i < 4; ++i) {
            c[i] = static_cast<float>(in[i]) * kScale;
        }
    } else {
        const float* in = static_cast<const float*>(src);
        for (int i = 0; i < 4; ++i) {
            c[i] = in[i];
        }
    }
    // Slot 0 holds blue in a BGRA image.
    rgba[0] = (order == HostOrder::Bgra) ? c[2] : c[0];
    rgba[1] = c[1];
    rgba[2] = (order == HostOrder::Bgra) ? c[0] : c[2];
    rgba[3] = c[3];
}

}  // namespace osv::ofx
