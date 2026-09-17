// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/render/ImageRGBAf.h"

#include <cmath>
#include <limits>

namespace osv::render {

namespace {

/// Quantise a float to a 16-bit code the way a 16-bit PNG writer would:
/// clamp to [0, 1] and round to nearest.
inline std::uint32_t quantise16(float v) noexcept {
    if (!(v > 0.0f)) {  // also catches NaN
        return 0;
    }
    if (v >= 1.0f) {
        return 65535u;
    }
    return static_cast<std::uint32_t>(v * 65535.0f + 0.5f);
}

}  // namespace

ImageDiffStats compareImages16(const ImageRGBAf& a, const ImageRGBAf& b) {
    ImageDiffStats stats;
    if (!a.valid() || !b.valid() || a.w != b.w || a.h != b.h) {
        stats.psnrDb = -1.0;
        return stats;
    }
    const std::size_t count = a.data.size();
    double sumSq = 0.0;
    std::size_t within2 = 0;
    bool alphaSame = true;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t qa = quantise16(a.data[i]);
        const std::uint32_t qb = quantise16(b.data[i]);
        const std::uint32_t diff = qa > qb ? qa - qb : qb - qa;
        if ((i & 3u) == 3u) {
            // alpha channel
            if (diff != 0) {
                alphaSame = false;
            }
        }
        if (diff > stats.maxAbsCode) {
            stats.maxAbsCode = diff;
        }
        if (diff <= 2) {
            ++within2;
        }
        const double d = static_cast<double>(diff) / 65535.0;
        sumSq += d * d;
    }
    stats.fractionWithin2 = count ? static_cast<double>(within2) / static_cast<double>(count) : 1.0;
    stats.alphaIdentical = alphaSame;
    const double mse = count ? sumSq / static_cast<double>(count) : 0.0;
    stats.psnrDb = mse > 0.0 ? 10.0 * std::log10(1.0 / mse) : std::numeric_limits<double>::infinity();
    return stats;
}

double psnr16(const ImageRGBAf& a, const ImageRGBAf& b) { return compareImages16(a, b).psnrDb; }

}  // namespace osv::render
