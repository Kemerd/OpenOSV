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
//     PrPixelFormat_BGRA_4444_32f / _16u / _8u: B,G,R,A per pixel, origin
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
// and rounds when producing 8-bit or 16-bit integer codes (16u white is
// 32768, see kWhite16u), and runs rows in parallel on the
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
constexpr std::size_t kBytesPerPixel16u = 8;
constexpr std::size_t kBytesPerPixel8u = 4;

/// Code of full white in PrPixelFormat_BGRA_4444_16u.  Premiere's 16-bit
/// integer formats run from black at 0 to white at 32768, like After Effects
/// and Photoshop 16-bit ("5.4.2 Byte Order" in the SDK guide) - NOT to 65535.
/// A frame scaled to 65535 would read as almost two stops over-exposed.
constexpr std::uint16_t kWhite16u = 32768;

/// The three host frame layouts the importer produces, as one switchable
/// value (the PrPixelFormat constants live in an SDK header this file must
/// not depend on).  All three are B,G,R,A, straight alpha, bottom-left.
enum class HostPixelFormat : std::uint8_t {
    Bgra32f = 0,  ///< PrPixelFormat_BGRA_4444_32f: floats, unclamped.
    Bgra16u = 1,  ///< PrPixelFormat_BGRA_4444_16u: 0..32768, clamped, rounded.
    Bgra8u = 2,   ///< PrPixelFormat_BGRA_4444_8u: 0..255, clamped, rounded.
};

/// Bytes per pixel of a host layout (16, 8 or 4; 0 for a value outside the
/// enum, which every caller treats as "refuse").
[[nodiscard]] constexpr std::size_t bytesPerPixel(HostPixelFormat format) noexcept {
    switch (format) {
    case HostPixelFormat::Bgra32f: return kBytesPerPixel32f;
    case HostPixelFormat::Bgra16u: return kBytesPerPixel16u;
    case HostPixelFormat::Bgra8u:  return kBytesPerPixel8u;
    default:                       return 0;
    }
}

/// Stable lower-case name of a host layout for logs ("32f", "16u", "8u").
[[nodiscard]] const char* hostPixelFormatName(HostPixelFormat format) noexcept;

/// A read-only run of TOP-DOWN float RGBA rows: a whole ImageRGBAf, or one
/// band of a frame read back from the GPU into a pinned staging buffer.
/// `pitchBytes` is the distance between two rows (>= width * 16, positive).
struct RgbaRows {
    const float* base = nullptr;
    std::size_t pitchBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t rows = 0;

    [[nodiscard]] bool valid() const noexcept {
        return base != nullptr && width > 0 && rows > 0 && width <= 32768u && rows <= 32768u &&
               pitchBytes >= static_cast<std::size_t>(width) * kBytesPerPixel32f && pitchBytes % sizeof(float) == 0;
    }
    /// Row `r` of the run (no bounds check; callers validate first).
    [[nodiscard]] const float* row(std::uint32_t r) const noexcept {
        return reinterpret_cast<const float*>(reinterpret_cast<const char*>(base) +
                                              static_cast<std::ptrdiff_t>(pitchBytes) * static_cast<std::ptrdiff_t>(r));
    }
};

/// A read-only run of TOP-DOWN rows that are ALREADY in a host layout
/// (B,G,R,A in 32f, 16u or 8u) - a band of a frame the GPU packed before the
/// readback.  Only the row order still differs from a host frame.
struct PackedRows {
    const void* base = nullptr;
    std::size_t pitchBytes = 0;
    std::uint32_t width = 0;
    std::uint32_t rows = 0;

    [[nodiscard]] bool valid(std::size_t bytesPerPixel) const noexcept {
        return base != nullptr && bytesPerPixel > 0 && width > 0 && rows > 0 && width <= 32768u && rows <= 32768u &&
               pitchBytes >= static_cast<std::size_t>(width) * bytesPerPixel;
    }
    /// Row `r` of the run (no bounds check; callers validate first).
    [[nodiscard]] const char* row(std::uint32_t r) const noexcept {
        return static_cast<const char*>(base) +
               static_cast<std::ptrdiff_t>(pitchBytes) * static_cast<std::ptrdiff_t>(r);
    }
};

// --------------------------------------------------------------------------
//  Host frames (bottom-left origin)
// --------------------------------------------------------------------------

/// ImageRGBAf -> PrPixelFormat_BGRA_4444_32f host frame (bottom-left).
/// Sizes must match exactly.
Status rgbaToHostBgra32f(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// ImageRGBAf -> PrPixelFormat_BGRA_4444_8u host frame (bottom-left), values
/// clamped to [0, 1] and rounded to nearest.
Status rgbaToHostBgra8u(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// ImageRGBAf -> PrPixelFormat_BGRA_4444_16u host frame (bottom-left),
/// values clamped to [0, 1] and rounded to nearest code of 0..32768.
Status rgbaToHostBgra16u(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept;

/// ImageRGBAf -> host frame in any of the three layouts (dispatches to the
/// three functions above; one entry point for a caller that picked the
/// layout at run time).
Status rgbaToHost(const render::ImageRGBAf& src, const HostFrame& dst, HostPixelFormat format,
                  ThreadPool* pool) noexcept;

/// A BAND of top-down float RGBA rows -> the matching rows of a bottom-left
/// host frame in `format`.
///
/// `src` holds picture rows [firstRow, firstRow + src.rows); they land in
/// host rows (dst.height - 1 - firstRow - i), so a frame streamed through
/// this function band by band ends up exactly as rgbaToHost() would have
/// written it in one go - the importer's GPU path reads its frame back in
/// pinned bands and converts each band while the next one is in flight.
/// The widths must match and the band must lie inside the frame.  Rows are
/// split over `pool` in small enough pieces that even a band of a few dozen
/// rows keeps every worker busy.
Status rgbaRowsToHost(const RgbaRows& src, std::uint32_t firstRow, const HostFrame& dst, HostPixelFormat format,
                      ThreadPool* pool) noexcept;

/// A band of rows already packed in `format` -> the matching rows of a
/// bottom-left host frame: a plain row copy with the flip, nothing else.
/// Picture row firstRow + i lands in host row (dst.height - 1 - firstRow - i),
/// exactly as rgbaRowsToHost() places it.
Status packedRowsToHost(const PackedRows& src, std::uint32_t firstRow, const HostFrame& dst, HostPixelFormat format,
                        ThreadPool* pool) noexcept;

/// BGRA_4444_32f host frame (bottom-left) -> ImageRGBAf (top-down).
Result<render::ImageRGBAf> hostBgra32fToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept;

/// BGRA_4444_16u host frame (bottom-left) -> ImageRGBAf, codes / 32768.
Result<render::ImageRGBAf> hostBgra16uToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept;

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

/// Clamp to [0, 1] and quantise to a Premiere 16-bit code (0..kWhite16u)
/// with rounding to nearest.  NaN maps to 0.  Codes above kWhite16u are never
/// produced: the format has no super-white.
[[nodiscard]] std::uint16_t floatTo16u(float value) noexcept;

/// Premiere 16-bit code -> float, code / 32768 (exact: 32768 is a power of
/// two, so every code maps to a distinct float and back).
[[nodiscard]] constexpr float u16ToFloat(std::uint16_t code) noexcept {
    return static_cast<float>(code) * (1.0f / static_cast<float>(kWhite16u));
}

/// Address of host row `row` for a signed pitch (no bounds check; callers
/// validate the frame first).
[[nodiscard]] inline char* rowAddress(char* base, std::int32_t rowBytes, std::uint32_t row) noexcept {
    return base + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(row);
}
[[nodiscard]] inline const char* rowAddress(const char* base, std::int32_t rowBytes, std::uint32_t row) noexcept {
    return base + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(row);
}

}  // namespace osv::premiere::pixelcopy
