// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Luma-histogram colour-mode detection (fallback when metadata is absent).

#include "osv/color/AutoDetect.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace osv::color {

namespace {

/// Histogram resolution (bins over the normalised 0..1 luma range).
constexpr std::uint32_t kBins = 1024;

/// Fraction of samples below a percentile p (0..1) -> normalised luma code,
/// linear interpolation inside the bin so the estimate is not quantised.
float percentile(const std::array<std::uint64_t, kBins>& hist, std::uint64_t total, double p) noexcept {
    if (total == 0) {
        return 0.0f;
    }
    const double target = p * static_cast<double>(total);
    std::uint64_t cum = 0;
    for (std::uint32_t b = 0; b < kBins; ++b) {
        const std::uint64_t next = cum + hist[b];
        if (static_cast<double>(next) >= target) {
            // Interpolate within the bin.
            const double inBin = hist[b] > 0 ? (target - static_cast<double>(cum)) / static_cast<double>(hist[b]) : 0.0;
            const double frac = inBin < 0.0 ? 0.0 : (inBin > 1.0 ? 1.0 : inBin);
            return static_cast<float>((static_cast<double>(b) + frac) / static_cast<double>(kBins));
        }
        cum = next;
    }
    return 1.0f;
}

/// Distance-to-threshold helper: 0 at the threshold, 1 once `margin` away.
float margin01(float distance, float margin) noexcept {
    if (margin <= 0.0f) {
        return 0.0f;
    }
    const float v = distance / margin;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

AutoDetectResult classifyLumaStats(float p001, float p50, float p999, std::uint64_t samples) noexcept {
    AutoDetectResult r;
    r.p001 = p001;
    r.p50 = p50;
    r.p999 = p999;
    r.spread = p999 - p001;
    r.samples = samples;
    // Reject garbage before applying any rule.
    if (samples == 0 || !std::isfinite(p001) || !std::isfinite(p50) || !std::isfinite(p999)) {
        r.guess = meta::ColorMode::Unknown;
        r.confidence = 0.0f;
        return r;
    }

    // Rule 1: D-Log M is compact (highlights below 0.82) with a mid-range median.
    const bool dlog = p999 <= kDetectDlogMaxP999 && r.spread <= kDetectDlogMaxSpread && p50 >= kDetectDlogMinP50 &&
                      p50 <= kDetectDlogMaxP50;
    if (dlog) {
        r.guess = meta::ColorMode::DLogM;
        // Confidence: how far inside every threshold the statistics sit.
        const float c = std::min({margin01(kDetectDlogMaxP999 - p999, 0.10f), margin01(kDetectDlogMaxSpread - r.spread, 0.10f),
                                  margin01(p50 - kDetectDlogMinP50, 0.08f), margin01(kDetectDlogMaxP50 - p50, 0.08f)});
        r.confidence = 0.5f + 0.5f * c;
        return r;
    }
    // Rule 2: HLG fills the range with a dark-heavy distribution.
    if (p999 > kDetectHlgMinP999 && p50 < kDetectHlgMaxP50) {
        r.guess = meta::ColorMode::HLG;
        const float c = std::min(margin01(p999 - kDetectHlgMinP999, 0.08f), margin01(kDetectHlgMaxP50 - p50, 0.10f));
        r.confidence = 0.5f + 0.5f * c;
        return r;
    }
    // Rule 3: everything else is treated as Rec.709 "Normal".
    r.guess = meta::ColorMode::Normal;
    // Normal is the catch-all, so its confidence reflects how clearly the
    // other two rules were missed.
    const float missDlog = std::max({margin01(p999 - kDetectDlogMaxP999, 0.10f), margin01(r.spread - kDetectDlogMaxSpread, 0.10f),
                                     margin01(kDetectDlogMinP50 - p50, 0.08f), margin01(p50 - kDetectDlogMaxP50, 0.08f)});
    const float missHlg = std::max(margin01(kDetectHlgMinP999 - p999, 0.08f), margin01(p50 - kDetectHlgMaxP50, 0.10f));
    r.confidence = 0.4f + 0.6f * std::min(missDlog, missHlg);
    return r;
}

AutoDetectResult detectColorMode(const video::PlanarFrame16& frame, std::uint32_t subsample) noexcept {
    AutoDetectResult empty;
    // A frame without a luma plane cannot be inspected.
    if (!frame.valid()) {
        return empty;
    }
    if (subsample == 0) {
        subsample = 1;
    }
    // Normalisation constants for the frame's bit depth and range.
    std::uint32_t depth = frame.bitDepth;
    if (depth < 8) {
        depth = 8;
    }
    if (depth > 16) {
        depth = 16;
    }
    const float shift = static_cast<float>(1u << (depth - 8));
    const float black = frame.narrowRange ? 16.0f * shift : 0.0f;
    const float range = frame.narrowRange ? 219.0f * shift : static_cast<float>((1u << depth) - 1u);

    // Build the histogram from a regular sub-grid of the luma plane.
    std::array<std::uint64_t, kBins> hist{};
    std::uint64_t total = 0;
    const std::uint16_t* plane = frame.plane[0];
    const std::size_t stride = frame.strideElems[0];
    for (std::uint32_t y = 0; y < frame.height; y += subsample) {
        const std::uint16_t* row = plane + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < frame.width; x += subsample) {
            const std::uint32_t raw = static_cast<std::uint32_t>(row[x]) >> frame.bitShift;
            float code = (static_cast<float>(raw) - black) / range;
            code = code < 0.0f ? 0.0f : (code > 1.0f ? 1.0f : code);
            std::uint32_t bin = static_cast<std::uint32_t>(code * static_cast<float>(kBins));
            if (bin >= kBins) {
                bin = kBins - 1;
            }
            ++hist[bin];
            ++total;
        }
    }
    if (total == 0) {
        return empty;
    }
    // Percentiles and the decision.
    const float p001 = percentile(hist, total, 0.001);
    const float p50 = percentile(hist, total, 0.5);
    const float p999 = percentile(hist, total, 0.999);
    return classifyLumaStats(p001, p50, p999, total);
}

}  // namespace osv::color
