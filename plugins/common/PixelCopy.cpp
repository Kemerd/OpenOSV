// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "PixelCopy.h"

#include "PluginLog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace osv::premiere::pixelcopy {

namespace {

/// Rows per work item; 16 rows of a 6000-wide float frame is ~1.5 MB, a
/// good balance between scheduling overhead and cache footprint.
constexpr std::size_t kRowGrain = 16;

/// Run `body(row)` for every row, on the pool when one is given.  A pool
/// failure (body threw, which ours never do) is reported as Internal.
template <class Body>
Status forEachRow(ThreadPool* pool, std::uint32_t rows, const Body& body) noexcept {
    try {
        if (pool && rows > kRowGrain) {
            return pool->parallelRows(rows, kRowGrain, [&body](std::size_t row) { body(static_cast<std::uint32_t>(row)); });
        }
        for (std::uint32_t y = 0; y < rows; ++y) {
            body(y);
        }
        return okStatus();
    } catch (...) {
        return failStatus(ErrorCode::Internal, "PixelCopy: exception in row loop");
    }
}

/// Validate a write target against the source image.
Status checkWrite(const render::ImageRGBAf& src, const HostFrame& dst, std::size_t bpp, const char* fn) noexcept {
    if (!src.valid()) {
        PluginLog::warn("{}: source image invalid", fn);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: source image invalid");
    }
    if (!dst.valid(bpp)) {
        PluginLog::warn("{}: destination frame invalid (base={}, rowBytes={}, {}x{})", fn, static_cast<const void*>(dst.base),
                        dst.rowBytes, dst.width, dst.height);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: destination frame invalid");
    }
    if (dst.width != src.w || dst.height != src.h) {
        PluginLog::warn("{}: size mismatch source {}x{} destination {}x{}", fn, src.w, src.h, dst.width, dst.height);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: size mismatch");
    }
    return okStatus();
}

/// Validate a read source and allocate the output image.
Result<render::ImageRGBAf> checkRead(const ConstHostFrame& src, std::size_t bpp, const char* fn) noexcept {
    if (!src.valid(bpp)) {
        PluginLog::warn("{}: source frame invalid (base={}, rowBytes={}, {}x{})", fn, static_cast<const void*>(src.base),
                        src.rowBytes, src.width, src.height);
        return Error{ErrorCode::InvalidArgument, "PixelCopy: source frame invalid"};
    }
    return render::ImageRGBAf::create(src.width, src.height);
}

// ---- per-row kernels ---------------------------------------------------------

/// RGBA floats -> BGRA floats for one row.
inline void rowRgbaToBgra32f(const float* in, float* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = in[2];
        out[1] = in[1];
        out[2] = in[0];
        out[3] = in[3];
        in += 4;
        out += 4;
    }
}

/// BGRA floats -> RGBA floats for one row.
inline void rowBgra32fToRgba(const float* in, float* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = in[2];
        out[1] = in[1];
        out[2] = in[0];
        out[3] = in[3];
        in += 4;
        out += 4;
    }
}

/// RGBA floats -> BGRA 8-bit codes for one row.
inline void rowRgbaToBgra8u(const float* in, std::uint8_t* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = floatTo8u(in[2]);
        out[1] = floatTo8u(in[1]);
        out[2] = floatTo8u(in[0]);
        out[3] = floatTo8u(in[3]);
        in += 4;
        out += 4;
    }
}

/// BGRA 8-bit codes -> RGBA floats for one row.
inline void rowBgra8uToRgba(const std::uint8_t* in, float* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = u8ToFloat(in[2]);
        out[1] = u8ToFloat(in[1]);
        out[2] = u8ToFloat(in[0]);
        out[3] = u8ToFloat(in[3]);
        in += 4;
        out += 4;
    }
}

/// RGBA floats -> BGRA Premiere 16-bit codes (0..32768) for one row.
inline void rowRgbaToBgra16u(const float* in, std::uint16_t* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = floatTo16u(in[2]);
        out[1] = floatTo16u(in[1]);
        out[2] = floatTo16u(in[0]);
        out[3] = floatTo16u(in[3]);
        in += 4;
        out += 4;
    }
}

/// BGRA Premiere 16-bit codes -> RGBA floats for one row.
inline void rowBgra16uToRgba(const std::uint16_t* in, float* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = u16ToFloat(in[2]);
        out[1] = u16ToFloat(in[1]);
        out[2] = u16ToFloat(in[0]);
        out[3] = u16ToFloat(in[3]);
        in += 4;
        out += 4;
    }
}

/// RGBA floats -> BGRA halves for one row.
inline void rowRgbaToBgra16f(const float* in, std::uint16_t* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = floatToHalf(in[2]);
        out[1] = floatToHalf(in[1]);
        out[2] = floatToHalf(in[0]);
        out[3] = floatToHalf(in[3]);
        in += 4;
        out += 4;
    }
}

/// BGRA halves -> RGBA floats for one row.
inline void rowBgra16fToRgba(const std::uint16_t* in, float* out, std::uint32_t width) noexcept {
    for (std::uint32_t x = 0; x < width; ++x) {
        out[0] = halfToFloat(in[2]);
        out[1] = halfToFloat(in[1]);
        out[2] = halfToFloat(in[0]);
        out[3] = halfToFloat(in[3]);
        in += 4;
        out += 4;
    }
}

/// Host row index for image row `y`: flipped for bottom-left layouts.
inline std::uint32_t hostRow(std::uint32_t y, std::uint32_t height, bool bottomLeft) noexcept {
    return bottomLeft ? (height - 1u - y) : y;
}

// ---- generic drivers -----------------------------------------------------------

template <class Pixel, class Kernel>
Status writeFrame(const render::ImageRGBAf& src, const HostFrame& dst, bool bottomLeft, ThreadPool* pool, const char* fn,
                  const Kernel& kernel) noexcept {
    OSV_TRY(checkWrite(src, dst, sizeof(Pixel) * 4, fn));
    return forEachRow(pool, src.h, [&](std::uint32_t y) {
        const float* in = src.row(y);
        Pixel* out = reinterpret_cast<Pixel*>(rowAddress(dst.base, dst.rowBytes, hostRow(y, dst.height, bottomLeft)));
        if (in && out) {
            kernel(in, out, dst.width);
        }
    });
}

/// Rows per work item for a band of `rows` rows.
///
/// kRowGrain (16) suits a whole frame, but a pinned readback band is only a
/// few hundred rows: 16-row items would leave most of a 32-thread pool idle
/// while the band's DMA successor is already waiting.  Two items per worker
/// (the calling thread participates too) keeps everyone busy and absorbs
/// uneven finishing times; never below one row, never above kRowGrain.
std::size_t bandGrain(const ThreadPool* pool, std::uint32_t rows) noexcept {
    const std::size_t workers = pool ? static_cast<std::size_t>(pool->size()) + 1u : 1u;
    const std::size_t pieces = workers * 2u;
    const std::size_t grain = (static_cast<std::size_t>(rows) + pieces - 1u) / pieces;
    return std::clamp<std::size_t>(grain, 1u, kRowGrain);
}

/// Write a band of top-down float RGBA rows into the matching rows of a
/// bottom-left host frame through `kernel` (one of the row kernels above).
template <class Pixel, class Kernel>
Status writeBand(const RgbaRows& src, std::uint32_t firstRow, const HostFrame& dst, ThreadPool* pool, const char* fn,
                 const Kernel& kernel) noexcept {
    // ---- validate everything the row loop would otherwise trust -----------
    if (!src.valid()) {
        PluginLog::warn("{}: source band invalid (base={}, pitch={}, {}x{})", fn, static_cast<const void*>(src.base),
                        src.pitchBytes, src.width, src.rows);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: source band invalid");
    }
    if (!dst.valid(sizeof(Pixel) * 4)) {
        PluginLog::warn("{}: destination frame invalid (base={}, rowBytes={}, {}x{})", fn,
                        static_cast<const void*>(dst.base), dst.rowBytes, dst.width, dst.height);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: destination frame invalid");
    }
    if (src.width != dst.width) {
        PluginLog::warn("{}: width mismatch source {} destination {}", fn, src.width, dst.width);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: band width mismatch");
    }
    // 64-bit sum: firstRow + rows must not wrap before it is compared.
    if (static_cast<std::uint64_t>(firstRow) + src.rows > dst.height) {
        PluginLog::warn("{}: band rows {}..{} outside a {}-row frame", fn, firstRow,
                        static_cast<std::uint64_t>(firstRow) + src.rows, dst.height);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: band outside the frame");
    }

    // ---- convert, one band row per body call ------------------------------
    const auto body = [&](std::size_t i) {
        const auto r = static_cast<std::uint32_t>(i);
        const float* in = src.row(r);
        // Picture row firstRow + r lives in host row height - 1 - (firstRow + r).
        Pixel* out = reinterpret_cast<Pixel*>(rowAddress(dst.base, dst.rowBytes, hostRow(firstRow + r, dst.height, true)));
        if (in && out) {
            kernel(in, out, dst.width);
        }
    };
    try {
        if (pool && src.rows > 1u) {
            return pool->parallelRows(src.rows, bandGrain(pool, src.rows), body);
        }
        for (std::uint32_t r = 0; r < src.rows; ++r) {
            body(r);
        }
        return okStatus();
    } catch (...) {
        return failStatus(ErrorCode::Internal, "PixelCopy: exception in band loop");
    }
}

template <class Pixel, class Kernel>
Result<render::ImageRGBAf> readFrame(const ConstHostFrame& src, bool bottomLeft, ThreadPool* pool, const char* fn,
                                     const Kernel& kernel) noexcept {
    OSV_TRY_ASSIGN(render::ImageRGBAf img, checkRead(src, sizeof(Pixel) * 4, fn));
    const Status st = forEachRow(pool, src.height, [&](std::uint32_t y) {
        const Pixel* in =
            reinterpret_cast<const Pixel*>(rowAddress(src.base, src.rowBytes, hostRow(y, src.height, bottomLeft)));
        float* out = img.row(y);
        if (in && out) {
            kernel(in, out, src.width);
        }
    });
    if (!st.ok()) {
        return Error(st.error());
    }
    return img;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Host frames (bottom-left)
// -----------------------------------------------------------------------------
Status rgbaToHostBgra32f(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept {
    return writeFrame<float>(src, dst, true, pool, "rgbaToHostBgra32f", rowRgbaToBgra32f);
}

Status rgbaToHostBgra8u(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept {
    return writeFrame<std::uint8_t>(src, dst, true, pool, "rgbaToHostBgra8u", rowRgbaToBgra8u);
}

Status rgbaToHostBgra16u(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept {
    return writeFrame<std::uint16_t>(src, dst, true, pool, "rgbaToHostBgra16u", rowRgbaToBgra16u);
}

Status rgbaToHost(const render::ImageRGBAf& src, const HostFrame& dst, HostPixelFormat format,
                  ThreadPool* pool) noexcept {
    // One switch, so a layout added to the enum without a case here fails
    // loudly instead of silently writing the wrong bytes.
    switch (format) {
    case HostPixelFormat::Bgra32f: return rgbaToHostBgra32f(src, dst, pool);
    case HostPixelFormat::Bgra16u: return rgbaToHostBgra16u(src, dst, pool);
    case HostPixelFormat::Bgra8u:  return rgbaToHostBgra8u(src, dst, pool);
    default:
        PluginLog::warn("rgbaToHost: unknown host pixel layout {}", static_cast<int>(format));
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: unknown host pixel layout");
    }
}

Status rgbaRowsToHost(const RgbaRows& src, std::uint32_t firstRow, const HostFrame& dst, HostPixelFormat format,
                      ThreadPool* pool) noexcept {
    // The same row kernels the whole-frame functions use, so a frame
    // streamed band by band is byte-identical to one written in one go.
    switch (format) {
    case HostPixelFormat::Bgra32f:
        return writeBand<float>(src, firstRow, dst, pool, "rgbaRowsToHost(32f)", rowRgbaToBgra32f);
    case HostPixelFormat::Bgra16u:
        return writeBand<std::uint16_t>(src, firstRow, dst, pool, "rgbaRowsToHost(16u)", rowRgbaToBgra16u);
    case HostPixelFormat::Bgra8u:
        return writeBand<std::uint8_t>(src, firstRow, dst, pool, "rgbaRowsToHost(8u)", rowRgbaToBgra8u);
    default:
        PluginLog::warn("rgbaRowsToHost: unknown host pixel layout {}", static_cast<int>(format));
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: unknown host pixel layout");
    }
}

Status packedRowsToHost(const PackedRows& src, std::uint32_t firstRow, const HostFrame& dst, HostPixelFormat format,
                        ThreadPool* pool) noexcept {
    // ---- validate ------------------------------------------------------------
    const std::size_t bpp = bytesPerPixel(format);
    if (bpp == 0) {
        PluginLog::warn("packedRowsToHost: unknown host pixel layout {}", static_cast<int>(format));
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: unknown host pixel layout");
    }
    if (!src.valid(bpp)) {
        PluginLog::warn("packedRowsToHost: source band invalid (base={}, pitch={}, {}x{})",
                        static_cast<const void*>(src.base), src.pitchBytes, src.width, src.rows);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: source band invalid");
    }
    if (!dst.valid(bpp)) {
        PluginLog::warn("packedRowsToHost: destination frame invalid (base={}, rowBytes={}, {}x{})",
                        static_cast<const void*>(dst.base), dst.rowBytes, dst.width, dst.height);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: destination frame invalid");
    }
    if (src.width != dst.width) {
        PluginLog::warn("packedRowsToHost: width mismatch source {} destination {}", src.width, dst.width);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: band width mismatch");
    }
    if (static_cast<std::uint64_t>(firstRow) + src.rows > dst.height) {
        PluginLog::warn("packedRowsToHost: band rows {}..{} outside a {}-row frame", firstRow,
                        static_cast<std::uint64_t>(firstRow) + src.rows, dst.height);
        return failStatus(ErrorCode::InvalidArgument, "PixelCopy: band outside the frame");
    }

    // ---- copy with the flip ----------------------------------------------------
    const std::size_t rowBytes = static_cast<std::size_t>(dst.width) * bpp;
    const auto body = [&](std::size_t i) {
        const auto r = static_cast<std::uint32_t>(i);
        char* out = rowAddress(dst.base, dst.rowBytes, hostRow(firstRow + r, dst.height, true));
        std::memcpy(out, src.row(r), rowBytes);
    };
    try {
        if (pool && src.rows > 1u) {
            return pool->parallelRows(src.rows, bandGrain(pool, src.rows), body);
        }
        for (std::uint32_t r = 0; r < src.rows; ++r) {
            body(r);
        }
        return okStatus();
    } catch (...) {
        return failStatus(ErrorCode::Internal, "PixelCopy: exception in packed band loop");
    }
}

Result<render::ImageRGBAf> hostBgra32fToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept {
    return readFrame<float>(src, true, pool, "hostBgra32fToRgba", rowBgra32fToRgba);
}

Result<render::ImageRGBAf> hostBgra16uToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept {
    return readFrame<std::uint16_t>(src, true, pool, "hostBgra16uToRgba", rowBgra16uToRgba);
}

Result<render::ImageRGBAf> hostBgra8uToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept {
    return readFrame<std::uint8_t>(src, true, pool, "hostBgra8uToRgba", rowBgra8uToRgba);
}

// -----------------------------------------------------------------------------
//  Top-left frames
// -----------------------------------------------------------------------------
Status rgbaToTopLeftBgra32f(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept {
    return writeFrame<float>(src, dst, false, pool, "rgbaToTopLeftBgra32f", rowRgbaToBgra32f);
}

Status rgbaToTopLeftBgra16f(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept {
    return writeFrame<std::uint16_t>(src, dst, false, pool, "rgbaToTopLeftBgra16f", rowRgbaToBgra16f);
}

Status rgbaToTopLeftBgra8u(const render::ImageRGBAf& src, const HostFrame& dst, ThreadPool* pool) noexcept {
    return writeFrame<std::uint8_t>(src, dst, false, pool, "rgbaToTopLeftBgra8u", rowRgbaToBgra8u);
}

Result<render::ImageRGBAf> topLeftBgra32fToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept {
    return readFrame<float>(src, false, pool, "topLeftBgra32fToRgba", rowBgra32fToRgba);
}

Result<render::ImageRGBAf> topLeftBgra16fToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept {
    return readFrame<std::uint16_t>(src, false, pool, "topLeftBgra16fToRgba", rowBgra16fToRgba);
}

Result<render::ImageRGBAf> topLeftBgra8uToRgba(const ConstHostFrame& src, ThreadPool* pool) noexcept {
    return readFrame<std::uint8_t>(src, false, pool, "topLeftBgra8uToRgba", rowBgra8uToRgba);
}

// -----------------------------------------------------------------------------
//  Scalar helpers
// -----------------------------------------------------------------------------
std::uint8_t floatTo8u(float value) noexcept {
    // NaN fails both comparisons and falls through to 0.
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    // +0.5 then truncate == round half up, which for non-negative inputs is
    // round to nearest.
    return static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

std::uint16_t floatTo16u(float value) noexcept {
    // Same shape as floatTo8u: NaN fails the first comparison and lands on
    // black, anything at or above white is white (the format has no
    // super-white, so clamping is the only honest mapping).
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return kWhite16u;
    }
    // value * 32768 is exact in float (a power-of-two scale), so the only
    // rounding is the deliberate +0.5 truncation: round half up, i.e. round
    // to nearest for the non-negative inputs that reach here.  The result is
    // at most 32768.0 - never past kWhite16u.
    return static_cast<std::uint16_t>(value * static_cast<float>(kWhite16u) + 0.5f);
}

const char* hostPixelFormatName(HostPixelFormat format) noexcept {
    switch (format) {
    case HostPixelFormat::Bgra32f: return "32f";
    case HostPixelFormat::Bgra16u: return "16u";
    case HostPixelFormat::Bgra8u:  return "8u";
    default:                       return "unknown";
    }
}

std::uint16_t floatToHalf(float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23) & 0xFFu;
    const std::uint32_t mantissa = bits & 0x7FFFFFu;

    // Infinity and NaN: keep the class, keep NaN quiet with a non-zero
    // payload.
    if (exponent == 0xFFu) {
        if (mantissa == 0) {
            return static_cast<std::uint16_t>(sign | 0x7C00u);
        }
        return static_cast<std::uint16_t>(sign | 0x7C00u | 0x200u | (mantissa >> 13));
    }

    // Re-bias: float exponent 127, half exponent 15.
    const int e = static_cast<int>(exponent) - 127 + 15;

    // Too large for a half: round to infinity.
    if (e >= 0x1F) {
        return static_cast<std::uint16_t>(sign | 0x7C00u);
    }

    if (e <= 0) {
        // Subnormal half (or zero).  Shift the full 24-bit significand
        // (implicit 1 included) right by (1 - e) + 13 and round to nearest
        // even on the bits that fall off.
        if (e < -10) {
            return static_cast<std::uint16_t>(sign);  // underflows to zero
        }
        const std::uint32_t significand = mantissa | 0x800000u;
        const int shift = 14 - e;  // 13 + (1 - e)
        std::uint32_t half = significand >> shift;
        const std::uint32_t remainder = significand & ((1u << shift) - 1u);
        const std::uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half & 1u))) {
            ++half;  // may carry into the exponent field: that is correct
        }
        return static_cast<std::uint16_t>(sign | half);
    }

    // Normal half: keep 10 mantissa bits, round to nearest even on the 13
    // discarded bits.
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10) | (mantissa >> 13);
    const std::uint32_t remainder = mantissa & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) {
        ++half;  // carry into exponent yields the correct next value / infinity
    }
    return static_cast<std::uint16_t>(sign | half);
}

float halfToFloat(std::uint16_t half) noexcept {
    const std::uint32_t sign = (static_cast<std::uint32_t>(half) & 0x8000u) << 16;
    const std::uint32_t exponent = (half >> 10) & 0x1Fu;
    std::uint32_t mantissa = half & 0x3FFu;

    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;  // signed zero
        } else {
            // Subnormal half: normalise by shifting the mantissa until the
            // implicit bit appears, adjusting the exponent accordingly.
            int e = -1;
            do {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0);
            mantissa &= 0x3FFu;
            const std::uint32_t floatExp = static_cast<std::uint32_t>(127 - 15 - e);
            bits = sign | (floatExp << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        // Infinity or NaN.
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

}  // namespace osv::premiere::pixelcopy
