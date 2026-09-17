// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PixelCopy: conversions between the library's float RGBA image
// (osv::render::ImageRGBAf: interleaved r,g,b,a floats, row 0 = top,
// straight alpha, no padding) and the buffers Premiere Pro hands a plug-in.
//
// Two host layouts exist (docs/PREMIERE.md, decision D15):
//
//   * Host frames (importer output, PPix from the PPix Creator suites) in
//     PrPixelFormat_BGRA_4444_32f / _8u: bytes B,G,R,A per pixel, origin
//     BOTTOM-LEFT (the first row in memory is the bottom scanline) and a
//     row pitch from PPixSuite::GetRowBytes that may be NEGATIVE.  Row r in
//     host order lives at base + r * rowBytes, so top-down row y of the
//     image is host row (height - 1 - y).
//
//   * GPU frames and AE effect worlds in the effect: B,G,R,A as 32-bit float
//     or 16-bit half, origin TOP-LEFT, pitch positive (GPU) or of either
//     sign (PF_EffectWorld::rowbytes).  Row y lives at base + y * rowBytes.
//
// Every function validates its arguments, never throws, clamps to [0, 1]
// and rounds when producing 8-bit, and runs rows in parallel on the
// supplied ThreadPool (nullptr = calling thread only).  The half<->float
// conversion is exact IEEE 754 binary16: round-to-nearest-even, subnormals,
// infinities and NaN preserved.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/render/ImageRGBAf.h"

#include <cstddef>
#include <cstdint>

namespace osv::premiere::pixelcopy {

/// A writable host buffer: base address of host row 0, signed row pitch
/// in bytes and the pixel dimensions.  Whether host row 0 is the bottom or
/// the top scanline is decided by the function the view is passed to.
struct HostFrame {
    char* base = nullptr;
    std::int32_t rowBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool valid(std::size_t bytesPerPixel) const noexcept {
        return base != nullptr && rowBytes != 0 && width > 0 && height > 0 && width <= 32768u && height <= 32768u &&
               static_cast<std::size_t>(rowBytes < 0 ? -rowBytes : rowBytes) >= static_cast<std::size_t>(width) * bytesPerPixel;
    }
};

/// Read-only counterpart of HostFrame.
struct ConstHostFrame {
    const char* base = nullptr;
    std::int32_t rowBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    ConstHostFrame() = default;
    ConstHostFrame(const char* b, std::int32_t rb, std::uint32_t w, std::uint32_t h) noexcept
        : base(b), rowBytes(rb), width(w), height(h) {}
    ConstHostFrame(const HostFrame& f) noexcept : base(f.base), rowBytes(f.rowBytes), width(f.width), height(f.height) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool valid(std::size_t bytesPerPixel) const noexcept {
        return base != nullptr && rowBytes != 0 && width > 0 && height > 0 && width <= 32768u && height <= 32768u &&
               static_cast<std::size_t>(rowBytes < 0 ? -rowBytes : rowBytes) >= static_cast<std::size_t>(width) * bytesPerPixel;
    }
};

/// Bytes per pixel of the layouts handled here.
constexpr std::size_t kBytesPerPixel32f = 16;
constexpr std::size_t kBytesPerPixel16f = 8;
constexpr std::size_t kBytesPerPixel8u = 4;

// --------------------------------------------------------------------------
//  Host frames (bottom-left origin)
// --------------------------------------------------------------------------

/// ImageRGBAf -> PrPixelFormat_BGRA_4444_32f host frame (bottom-left).
/// Sizes must match exactly.
Status rgbaToHostBgra32f(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// ImageRGBAf -> PrPixelFormat_BGRA_4444_8u host frame (bottom-left), values
/// clamped to [0, 1] and rounded to nearest.
Status rgbaToHostBgra8u(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// BGRA_4444_32f host frame (bottom-left) -> ImageRGBAf (top-down).
Result<render::ImageRGBAf> hostBgra32fToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept;

/// BGRA_4444_8u host frame (bottom-left) -> ImageRGBAf, codes / 255.
Result<render::ImageRGBAf> hostBgra8uToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept;

// --------------------------------------------------------------------------
//  Top-left frames (GPU BGRA 32f / 16f, AE effect worlds on the CPU path)
// --------------------------------------------------------------------------

/// ImageRGBAf -> BGRA 32f, top-left origin.
Status rgbaToTopLeftBgra32f(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// ImageRGBAf -> BGRA 16f (IEEE half), top-left origin.
Status rgbaToTopLeftBgra16f(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// ImageRGBAf -> BGRA 8u, top-left origin (AE 8-bit worlds in Premiere).
Status rgbaToTopLeftBgra8u(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// BGRA 32f top-left -> ImageRGBAf.
Result<render::ImageRGBAf> topLeftBgra32fToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept;

/// BGRA 16f top-left -> ImageRGBAf.
Result<render::ImageRGBAf> topLeftBgra16fToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept;

/// BGRA 8u top-left -> ImageRGBAf.
Result<render::ImageRGBAf> topLeftBgra8uToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept;

// --------------------------------------------------------------------------
//  Scalar helpers
// --------------------------------------------------------------------------

/// IEEE 754 binary32 -> binary16 with round-to-nearest-even.  Values beyond
/// the half range become +-infinity, NaN stays NaN (quiet), subnormal halves
/// are produced for tiny magnitudes.
[[nodiscard]] std::uint16_t floatToHalf(float value) noexcept;

/// IEEE 754 binary16 -> binary32 (exact).
[[nodiscard]] float halfToFloat(std::uint16_t half) noexcept;

/// Clamp to [0, 1] and quantise to an 8-bit code with rounding to nearest.
/// NaN maps to 0.
[[nodiscard]] std::uint8_t floatTo8u(float value) noexcept;

/// 8-bit code -> float in [0, 1].
[[nodiscard]] constexpr float u8ToFloat(std::uint8_t code) noexcept { return static_cast<float>(code) * (1.0f / 255.0f); }

/// Address of host row `row` for a signed pitch (no bounds check; callers
/// validate the frame first).
[[nodiscard]] inline char* rowAddress(char* base, std::int32_t rowBytes, std::uint32_t row) noexcept {
    return base + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(row);
}
[[nodiscard]] inline const char* rowAddress(const char* base, std::int32_t rowBytes, std::uint32_t row) noexcept {
    return base + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(row);
}

}  // namespace osv::premiere::pixelcopy
