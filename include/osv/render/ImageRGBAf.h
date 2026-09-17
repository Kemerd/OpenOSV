// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImageRGBAf: a float RGBA image in host memory, the output of every renderer
// backend.  Interleaved (r, g, b, a) per pixel, row-major, no padding.
#pragma once

#include "osv/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace osv::render {

struct ImageRGBAf {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<float> data;  ///< 4 * w * h floats.

    /// Allocate a zero-filled image.  Sizes above 32768 on a side are refused
    /// (the host applications cannot use them either).
    static Result<ImageRGBAf> create(std::uint32_t width, std::uint32_t height) {
        if (width == 0 || height == 0 || width > 32768 || height > 32768) {
            return Error{ErrorCode::InvalidArgument, "ImageRGBAf::create: unsupported size"};
        }
        ImageRGBAf img;
        img.w = width;
        img.h = height;
        img.data.assign(static_cast<std::size_t>(width) * height * 4u, 0.0f);
        return img;
    }

    [[nodiscard]] bool valid() const noexcept { return w > 0 && h > 0 && data.size() == static_cast<std::size_t>(w) * h * 4u; }

    /// Pointer to the first float of row `y` (nullptr when out of range).
    [[nodiscard]] float* row(std::uint32_t y) noexcept {
        if (y >= h || !valid()) {
            return nullptr;
        }
        return data.data() + static_cast<std::size_t>(y) * w * 4u;
    }
    [[nodiscard]] const float* row(std::uint32_t y) const noexcept {
        if (y >= h || !valid()) {
            return nullptr;
        }
        return data.data() + static_cast<std::size_t>(y) * w * 4u;
    }

    /// Pixel accessor with clamping (never reads out of bounds).
    [[nodiscard]] const float* pixel(std::uint32_t x, std::uint32_t y) const noexcept {
        if (!valid()) {
            static const float kBlack[4] = {0, 0, 0, 0};
            return kBlack;
        }
        x = x < w ? x : w - 1;
        y = y < h ? y : h - 1;
        return data.data() + (static_cast<std::size_t>(y) * w + x) * 4u;
    }

    /// Bytes per row.
    [[nodiscard]] std::size_t pitchBytes() const noexcept { return static_cast<std::size_t>(w) * 4u * sizeof(float); }
};

/// Peak signal-to-noise ratio between two images after quantising both to
/// 16-bit (the metric used by the backend parity tests).  Alpha is included.
/// Returns +inf for identical images, a negative value for mismatched sizes.
double psnr16(const ImageRGBAf& a, const ImageRGBAf& b);

/// Statistics of |a - b| after 16-bit quantisation.
struct ImageDiffStats {
    double psnrDb = 0.0;
    std::uint32_t maxAbsCode = 0;      ///< Largest per-channel difference in 16-bit codes.
    double fractionWithin2 = 0.0;      ///< Fraction of channel samples differing by <= 2 codes.
    bool alphaIdentical = false;
};
ImageDiffStats compareImages16(const ImageRGBAf& a, const ImageRGBAf& b);

}  // namespace osv::render
