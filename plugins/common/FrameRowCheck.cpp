// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "FrameRowCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace osv::premiere::rowcheck {

namespace {

/// One sampled pixel, as floats in the layout's own scale (codes / white),
/// plus whether any channel was non-finite.
struct Sample {
    float b = 0.0f;
    float g = 0.0f;
    float r = 0.0f;
    float a = 0.0f;
    bool finite = true;
};

/// Read the pixel at `p` in layout F.
///
/// memcpy rather than a typed dereference: a PPix is always aligned, but a
/// buffer a caller built by hand need not be, and an unaligned float load is
/// undefined behaviour.  The compiler turns each copy into a plain load.
template <pixelcopy::HostPixelFormat F>
[[nodiscard]] Sample readPixel(const char* p) noexcept {
    Sample s;
    if constexpr (F == pixelcopy::HostPixelFormat::Bgra32f) {
        // B, G, R, A floats, unclamped: NaN / infinity are possible here and
        // nowhere else, so this is the one layout that flags them.
        float v[4] = {};
        std::memcpy(v, p, sizeof(v));
        s.b = v[0];
        s.g = v[1];
        s.r = v[2];
        s.a = v[3];
        s.finite = std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]);
    } else if constexpr (F == pixelcopy::HostPixelFormat::Bgra16u) {
        // B, G, R, A codes of 0..32768 (Premiere's 16-bit white, kWhite16u).
        std::uint16_t v[4] = {};
        std::memcpy(v, p, sizeof(v));
        s.b = pixelcopy::u16ToFloat(v[0]);
        s.g = pixelcopy::u16ToFloat(v[1]);
        s.r = pixelcopy::u16ToFloat(v[2]);
        s.a = pixelcopy::u16ToFloat(v[3]);
    } else {
        // B, G, R, A codes of 0..255.
        std::uint8_t v[4] = {};
        std::memcpy(v, p, sizeof(v));
        s.b = pixelcopy::u8ToFloat(v[0]);
        s.g = pixelcopy::u8ToFloat(v[1]);
        s.r = pixelcopy::u8ToFloat(v[2]);
        s.a = pixelcopy::u8ToFloat(v[3]);
    }
    return s;
}

/// What the samples of one row add up to.
struct RowSummary {
    std::uint32_t opaque = 0;     ///< Samples with alpha >= kTransparentAlpha.
    double alphaSum = 0.0;        ///< Sum of the sampled alphas, each clamped to [0, 1] (non-finite = 0).
    bool allBlack = true;         ///< Every sampled B, G, R exactly zero.
    std::uint32_t nonFinite = 0;  ///< Samples with a NaN / infinite channel.
};

/// Summarise the `columns` samples of one row starting at `row`, at the
/// byte offsets in `offsets`.
template <pixelcopy::HostPixelFormat F>
[[nodiscard]] RowSummary summariseRow(const char* row, const std::array<std::size_t, kSampleColumns>& offsets,
                                      std::uint32_t columns) noexcept {
    RowSummary sum;
    for (std::uint32_t k = 0; k < columns; ++k) {
        const Sample s = readPixel<F>(row + offsets[k]);
        // ---- alpha: a non-finite one is no coverage at all -----------------
        float alpha = std::isfinite(s.a) ? s.a : 0.0f;
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        sum.alphaSum += static_cast<double>(alpha);
        if (alpha >= kTransparentAlpha) {
            ++sum.opaque;
        }
        // ---- colour: exactly zero in all three channels --------------------
        // A NaN compares unequal to zero, so a NaN pixel is never "black";
        // it is counted on its own instead.
        if (!(s.b == 0.0f && s.g == 0.0f && s.r == 0.0f)) {
            sum.allBlack = false;
        }
        if (!s.finite) {
            ++sum.nonFinite;
        }
    }
    return sum;
}

/// The scan proper, for one layout.  `bpp` is that layout's pixel size and
/// the frame has already been validated against it.
template <pixelcopy::HostPixelFormat F>
void scanRows(const pixelcopy::ConstHostFrame& frame, std::size_t bpp, RowScan& scan) noexcept {
    // ---- the sampled columns -------------------------------------------------
    // The centres of `columns` equal slices of the row: (2k + 1) * W / (2 n).
    // For a frame narrower than kSampleColumns that is every column once.
    // 64-bit maths: 32767 * 129 fits easily, but the habit costs nothing.
    std::array<std::size_t, kSampleColumns> offsets{};
    const std::uint32_t columns = scan.columns;
    for (std::uint32_t k = 0; k < columns; ++k) {
        const std::uint64_t x = ((2ull * k + 1ull) * frame.width) / (2ull * columns);
        offsets[k] = static_cast<std::size_t>(std::min<std::uint64_t>(x, frame.width - 1u)) * bpp;
    }

    // ---- every row, top to bottom --------------------------------------------
    // Picture row y is host row (height - 1 - y): host row 0 is the bottom
    // scanline (PixelCopy.h), and the pitch may be negative.
    std::uint32_t current = 0;          // length of the bad run ending at the previous row
    std::uint32_t currentStart = 0;     // its first row
    std::uint32_t currentTransparent = 0;
    std::uint32_t currentBlack = 0;
    scan.worstRowOpaque = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const char* row = pixelcopy::rowAddress(frame.base, frame.rowBytes, frame.height - 1u - y);
        const RowSummary sum = summariseRow<F>(row, offsets, columns);
        scan.nonFiniteSamples += sum.nonFinite;

        // ---- the worst row by opaque samples ---------------------------------
        if (sum.opaque < scan.worstRowOpaque) {
            scan.worstRowOpaque = sum.opaque;
            scan.worstRow = y;
        }

        // ---- classify the row ------------------------------------------------
        const bool transparent = sum.alphaSum < static_cast<double>(kTransparentAlpha) * static_cast<double>(columns);
        const bool black = sum.allBlack;
        if (transparent) {
            ++scan.transparentRows;
        }
        if (black) {
            ++scan.blackRows;
        }
        if (!transparent && !black) {
            current = 0;
            continue;
        }
        ++scan.badRows;

        // ---- extend (or start) the run, and keep the longest -----------------
        if (current == 0) {
            currentStart = y;
            currentTransparent = 0;
            currentBlack = 0;
        }
        ++current;
        currentTransparent += transparent ? 1u : 0u;
        currentBlack += black ? 1u : 0u;
        // Strictly longer only, so on a tie the first run is the one reported.
        if (current > scan.runLength) {
            scan.runLength = current;
            scan.runFirst = currentStart;
            scan.runLast = y;
            scan.runTransparentRows = currentTransparent;
            scan.runBlackRows = currentBlack;
        }
    }
}

}  // namespace

RowScan scanHostFrame(const pixelcopy::ConstHostFrame& frame, pixelcopy::HostPixelFormat format) noexcept {
    RowScan scan;
    // ---- refuse anything that cannot be read safely --------------------------
    // bytesPerPixel() is 0 for a layout outside the enum, and valid() then
    // fails too; valid() also catches a null base, a zero pitch, a pitch
    // narrower than a row and an absurd size.
    const std::size_t bpp = pixelcopy::bytesPerPixel(format);
    if (bpp == 0 || !frame.valid(bpp)) {
        return scan;
    }
    scan.scanned = true;
    scan.width = frame.width;
    scan.height = frame.height;
    scan.columns = std::min(kSampleColumns, frame.width);

    // ---- one instantiation per layout ----------------------------------------
    switch (format) {
    case pixelcopy::HostPixelFormat::Bgra32f:
        scanRows<pixelcopy::HostPixelFormat::Bgra32f>(frame, bpp, scan);
        break;
    case pixelcopy::HostPixelFormat::Bgra16u:
        scanRows<pixelcopy::HostPixelFormat::Bgra16u>(frame, bpp, scan);
        break;
    case pixelcopy::HostPixelFormat::Bgra8u:
        scanRows<pixelcopy::HostPixelFormat::Bgra8u>(frame, bpp, scan);
        break;
    default:
        // Unreachable (bpp was 0 above); kept so the switch is total.
        return RowScan{};
    }
    return scan;
}

const char* runKindName(const RowScan& scan) noexcept {
    if (!scan.scanned) {
        return "unscanned";
    }
    if (scan.runLength == 0) {
        return "clean";
    }
    // ---- what the run's rows are -------------------------------------------
    const bool anyTransparent = scan.runTransparentRows > 0;
    const bool anyBlack = scan.runBlackRows > 0;
    if (anyTransparent && anyBlack) {
        return "transparent and black";
    }
    return anyTransparent ? "transparent" : "black";
}

std::string describeRun(const RowScan& scan) noexcept {
    if (!scan.defective()) {
        return {};
    }
    // std::format can throw (bad_alloc); a log phrase is never worth that.
    try {
        return std::format("rows {}..{} ({:.1f}% of {}) came out {}", scan.runFirst, scan.runLast,
                           scan.runPercentOfHeight(), scan.height, runKindName(scan));
    } catch (...) {
        return {};
    }
}

}  // namespace osv::premiere::rowcheck
