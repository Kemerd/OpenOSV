// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SceneLight implementation: the metered light from the camera's exposure
// metadata, the zenith cap from a levelled scene-linear render, and the
// Day / Night decision.  The policy and the measured numbers are in the
// header.

#include "osv/render/SceneLight.h"

#include "osv/core/Log.h"
#include "osv/geom/EquirectMap.h"
#include "osv/render/RenderParamsBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <utility>

namespace osv::render {

namespace {

/// BT.2020 luminance weights: the Linear output is scene-linear Rec.2020.
constexpr double kLumaR = 0.2627;
constexpr double kLumaG = 0.6780;
constexpr double kLumaB = 0.0593;

/// Floor of every luminance / channel before a log2, so a black pixel is a
/// very dark number rather than -infinity.
constexpr double kLogFloor = 1e-6;

/// The value at fraction `p` (0..1) of an UNSORTED list, by nth_element on
/// a copy-free buffer the caller owns.  `values` must not be empty.
[[nodiscard]] double percentileOf(std::vector<double>& values, double p) noexcept {
    // Index of the order statistic, rounded to the nearest sample.
    const double clamped = std::clamp(p, 0.0, 1.0);
    const std::size_t n = values.size();
    const auto k = static_cast<std::size_t>(std::lround(clamped * static_cast<double>(n - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

/// One weighted sample of a cap statistic.
struct Weighted {
    double value = 0.0;
    double weight = 0.0;
};

/// The weighted median of `samples` (sorted in place).  NaN when the list
/// is empty or carries no weight.
[[nodiscard]] double weightedMedian(std::vector<Weighted>& samples) noexcept {
    if (samples.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // Sort by value; ties keep a stable, deterministic order by weight.
    std::sort(samples.begin(), samples.end(), [](const Weighted& a, const Weighted& b) {
        return a.value < b.value || (a.value == b.value && a.weight < b.weight);
    });
    double total = 0.0;
    for (const Weighted& s : samples) {
        total += s.weight;
    }
    if (!(total > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // The first sample whose running weight reaches half the total.
    double running = 0.0;
    for (const Weighted& s : samples) {
        running += s.weight;
        if (running >= 0.5 * total) {
            return s.value;
        }
    }
    return samples.back().value;
}

/// The plain median of a list of doubles (copied, the input is left alone).
[[nodiscard]] double medianOf(std::vector<double> values) noexcept {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // Even counts take the mean of the two middle values: symmetric in the
    // samples, so the order the frames were measured in never matters.
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return (n % 2u == 1u) ? values[n / 2u] : 0.5 * (values[n / 2u - 1u] + values[n / 2u]);
}

/// A body-from-world rotation whose world +Z is `upBody`: the levelled frame
/// the cap is cut from.  The heading is arbitrary (the cap is a disc around
/// the zenith), but fixed for a given up, so the measurement is too.
[[nodiscard]] Mat3d levelledBodyFromWorld(const Vec3d& upBody) noexcept {
    const Vec3d z = upBody.normalized();
    // Any axis not parallel to up seeds the horizontal basis.
    const Vec3d seed = std::abs(z.x) < 0.9 ? Vec3d{1.0, 0.0, 0.0} : Vec3d{0.0, 1.0, 0.0};
    const Vec3d x = (seed - z * seed.dot(z)).normalized();
    const Vec3d y = z.cross(x);
    // Columns are the images of world X, Y, Z in body coordinates.
    return Mat3d::fromColumns(x, y, z);
}

}  // namespace

// =============================================================================
//  Names
// =============================================================================
const char* sceneLightName(SceneLight light) noexcept {
    switch (light) {
    case SceneLight::Day: return "Day";
    case SceneLight::Night: return "Night";
    }
    return "Day";
}

// =============================================================================
//  Metered light
// =============================================================================
double ev100Of(const meta::CameraFrame& camera, double fNumber) noexcept {
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    // Every input must be a positive, finite number; anything else means the
    // frame recorded no exposure, not a black one.
    const double iso = static_cast<double>(camera.iso);
    if (!std::isfinite(iso) || !(iso > 0.0) || !std::isfinite(fNumber) || !(fNumber > 0.0)) {
        return kNaN;
    }
    if (camera.exposureTime.size() < 2 || camera.exposureTime[0] <= 0 || camera.exposureTime[1] <= 0) {
        return kNaN;
    }
    const double seconds = static_cast<double>(camera.exposureTime[0]) / static_cast<double>(camera.exposureTime[1]);
    if (!std::isfinite(seconds) || !(seconds > 0.0)) {
        return kNaN;
    }
    // EV100 = log2(N^2 / t) - log2(ISO / 100): the light value that exposure
    // would meter at ISO 100.
    const double ev = std::log2(fNumber * fNumber / seconds) - std::log2(iso / 100.0);
    return std::isfinite(ev) ? ev : kNaN;
}

double fNumberOf(const meta::ClipMeta& clip) noexcept {
    // A [num, den] rational; anything outside a lens's physical range is a
    // damaged field, not an aperture.
    if (clip.fNumber.size() >= 2 && clip.fNumber[0] > 0 && clip.fNumber[1] > 0) {
        const double n = static_cast<double>(clip.fNumber[0]) / static_cast<double>(clip.fNumber[1]);
        if (std::isfinite(n) && n >= 0.5 && n <= 32.0) {
            return n;
        }
    }
    return kDefaultFNumber;
}

MeteredLight meteredLightOf(std::span<const meta::CameraFrame> frames, double fNumber) noexcept {
    MeteredLight out;
    if (frames.empty()) {
        return out;
    }
    try {
        // ---- the camera's own light value, when most frames carry it --------
        std::vector<double> values;
        values.reserve(frames.size());
        for (const meta::CameraFrame& c : frames) {
            const double lv = static_cast<double>(c.aecLv);
            if (std::isfinite(lv) && lv > 0.0) {
                values.push_back(lv);
            }
        }
        out.fromAecLv = !values.empty() && values.size() * 2u > frames.size();

        // ---- otherwise EV100 from ISO / shutter / aperture ---------------------
        if (!out.fromAecLv) {
            values.clear();
            const double n = (std::isfinite(fNumber) && fNumber > 0.0) ? fNumber : kDefaultFNumber;
            for (const meta::CameraFrame& c : frames) {
                const double ev = ev100Of(c, n);
                if (std::isfinite(ev)) {
                    values.push_back(ev);
                }
            }
        }
        if (values.empty()) {
            return MeteredLight{};  // no frame recorded a usable exposure
        }

        // ---- percentiles (order statistics, deterministic) ----------------------
        out.frames = static_cast<std::uint32_t>(values.size());
        out.p5 = percentileOf(values, 0.05);
        out.p95 = percentileOf(values, 0.95);
        out.median = percentileOf(values, 0.5);
        out.valid = std::isfinite(out.median) && std::isfinite(out.p5) && std::isfinite(out.p95);
        return out;
    } catch (...) {
        // Out of memory for a few hundred doubles: no reading, never a crash.
        return MeteredLight{};
    }
}

MeteredLight meteredLightOf(const meta::MetadataTrack& track, std::uint32_t frameCount,
                            std::uint32_t maxSamples) noexcept {
    try {
        // The metadata may hold fewer samples than the video has frames.
        const std::uint32_t n = std::min(frameCount, track.frameCount());
        if (n == 0) {
            return MeteredLight{};
        }
        const std::uint32_t samples = std::clamp<std::uint32_t>(maxSamples, 2u, n);

        // ---- evenly spread frames, first and last always included -----------------
        std::vector<meta::CameraFrame> cams;
        cams.reserve(samples);
        std::uint32_t previous = std::numeric_limits<std::uint32_t>::max();
        for (std::uint32_t k = 0; k < samples; ++k) {
            const auto index = static_cast<std::uint32_t>(std::llround(
                static_cast<double>(k) * static_cast<double>(n - 1u) / static_cast<double>(samples - 1u)));
            if (index == previous) {
                continue;  // a short clip: the spacing rounds onto the same frame
            }
            previous = index;
            auto frame = track.frame(index);
            if (frame.ok()) {
                cams.push_back(frame.value().camera);
            }
        }
        return meteredLightOf(std::span<const meta::CameraFrame>(cams), fNumberOf(track.clip()));
    } catch (...) {
        return MeteredLight{};
    }
}

// =============================================================================
//  The sky cap
// =============================================================================
SkyCap skyCapOf(const float* rgba, std::uint32_t w, std::uint32_t rows, const SkyCapParams& params) noexcept {
    SkyCap cap;
    // ---- guard every input ---------------------------------------------------
    if (!rgba || w < 16u || (w % 2u) != 0u || rows == 0u || rows > w / 2u) {
        return cap;
    }
    if (!(params.capMinElevationDeg > 0.0 && params.capMinElevationDeg < 90.0) || !(params.greyLinear > 0.0) ||
        !std::isfinite(params.sourceStopsAboveGrey) || !(params.sourceExclusionDeg >= 0.0) ||
        !(params.flatNoiseMultiple > 0.0) || !(params.flatFloorStops >= 0.0)) {
        return cap;
    }
    try {
        const std::uint32_t h = w / 2u;
        const double degPerRow = 180.0 / static_cast<double>(h);
        const double degPerCol = 360.0 / static_cast<double>(w);
        const std::size_t count = static_cast<std::size_t>(w) * rows;

        // ---- the cap's rows: latitude of each row centre >= the cap edge --------
        std::uint32_t capRows = 0;
        while (capRows < rows && 90.0 - (static_cast<double>(capRows) + 0.5) * degPerRow >= params.capMinElevationDeg) {
            ++capRows;
        }
        if (capRows == 0) {
            return cap;
        }

        // ---- per pixel: coverage, luminance, log luminance, source --------------
        const double sourceY = params.greyLinear * std::exp2(params.sourceStopsAboveGrey);
        std::vector<std::uint8_t> covered(count, 0);
        std::vector<std::uint8_t> source(count, 0);
        std::vector<float> logY(count, 0.0f);
        for (std::size_t i = 0; i < count; ++i) {
            const float* p = rgba + i * 4u;
            const double r = p[0], g = p[1], b = p[2], a = p[3];
            const double y = kLumaR * r + kLumaG * g + kLumaB * b;
            if (!std::isfinite(y) || !std::isfinite(a) || !(a >= params.minAlpha)) {
                continue;  // unseen (occlusion arc) or garbage: not part of the sky
            }
            covered[i] = 1;
            logY[i] = static_cast<float>(std::log2(std::max(y, kLogFloor)));
            source[i] = y >= sourceY ? 1 : 0;
        }

        // ---- exclude every pixel within sourceExclusionDeg of a source -------------
        // A separable box on the sphere: rows within the angle, then columns
        // within the angle at THIS row's latitude (wider towards the pole,
        // the whole ring above lat 90 - angle).  A box is a slightly larger
        // area than the disc, which only ever errs towards excluding glow.
        std::vector<std::uint8_t> excluded(count, 0);
        const auto radiusRows = static_cast<std::uint32_t>(std::ceil(params.sourceExclusionDeg / degPerRow));
        std::vector<std::uint8_t> vertical(count, 0);
        for (std::uint32_t y = 0; y < rows; ++y) {
            const std::uint32_t y0 = y > radiusRows ? y - radiusRows : 0u;
            const std::uint32_t y1 = std::min(rows - 1u, y + radiusRows);
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t any = 0;
                for (std::uint32_t yy = y0; yy <= y1 && !any; ++yy) {
                    any = source[static_cast<std::size_t>(yy) * w + x];
                }
                vertical[static_cast<std::size_t>(y) * w + x] = any;
            }
        }
        std::vector<std::uint32_t> prefix(static_cast<std::size_t>(w) + 1u, 0u);
        for (std::uint32_t y = 0; y < capRows; ++y) {
            // This row's column radius for the exclusion angle.
            const double latRad = deg2rad(90.0 - (static_cast<double>(y) + 0.5) * degPerRow);
            const double cosLat = std::max(std::cos(latRad), 1e-6);
            const double colsF = std::ceil(params.sourceExclusionDeg / (degPerCol * cosLat));
            const std::uint32_t radiusCols =
                colsF >= static_cast<double>(w / 2u) ? w / 2u : static_cast<std::uint32_t>(colsF);
            // Prefix sums of the vertical mask, so each window is O(1).
            const std::uint8_t* row = vertical.data() + static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                prefix[x + 1u] = prefix[x] + row[x];
            }
            if (prefix[w] == 0u) {
                continue;  // no source within reach of this row
            }
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint32_t hits = 0;
                if (radiusCols >= w / 2u) {
                    hits = prefix[w];  // the whole ring is within the angle
                } else {
                    // Window [x - r, x + r] with wrap-around in longitude.
                    const long lo = static_cast<long>(x) - static_cast<long>(radiusCols);
                    const long hi = static_cast<long>(x) + static_cast<long>(radiusCols);
                    const long W = static_cast<long>(w);
                    if (lo >= 0 && hi < W) {
                        hits = prefix[static_cast<std::size_t>(hi + 1)] - prefix[static_cast<std::size_t>(lo)];
                    } else if (lo < 0) {
                        hits = prefix[static_cast<std::size_t>(hi + 1)] +
                               (prefix[w] - prefix[static_cast<std::size_t>(lo + W)]);
                    } else {
                        hits = (prefix[w] - prefix[static_cast<std::size_t>(lo)]) +
                               prefix[static_cast<std::size_t>(hi - W + 1)];
                    }
                }
                excluded[static_cast<std::size_t>(y) * w + x] = hits > 0u ? 1 : 0;
            }
        }

        // ---- roughness: the 3 x 3 log-luminance range of every covered pixel ------
        std::vector<float> rough(count, 0.0f);
        for (std::uint32_t y = 0; y < capRows; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (!covered[i]) {
                    continue;
                }
                float lo = logY[i];
                float hi = logY[i];
                for (int dy = -1; dy <= 1; ++dy) {
                    const long yy = static_cast<long>(y) + dy;
                    if (yy < 0 || yy >= static_cast<long>(rows)) {
                        continue;
                    }
                    for (int dx = -1; dx <= 1; ++dx) {
                        const std::uint32_t xx = (x + w + static_cast<std::uint32_t>(dx + 1) - 1u) % w;
                        const std::size_t j = static_cast<std::size_t>(yy) * w + xx;
                        if (covered[j]) {
                            lo = std::min(lo, logY[j]);
                            hi = std::max(hi, logY[j]);
                        }
                    }
                }
                rough[i] = hi - lo;
            }
        }

        // ---- areas, the noise floor and the flat threshold ---------------------------
        double capArea = 0.0, usableArea = 0.0, sourceArea = 0.0;
        std::vector<Weighted> roughSamples;
        roughSamples.reserve(static_cast<std::size_t>(capRows) * w);
        for (std::uint32_t y = 0; y < capRows; ++y) {
            // Equal-area weight of an equirect row: cos(latitude).
            const double weight = std::cos(deg2rad(90.0 - (static_cast<double>(y) + 0.5) * degPerRow));
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                capArea += weight;
                if (excluded[i]) {
                    sourceArea += weight;
                }
                if (covered[i] && !excluded[i]) {
                    usableArea += weight;
                    roughSamples.push_back({static_cast<double>(rough[i]), weight});
                }
            }
        }
        if (!(capArea > 0.0)) {
            return cap;
        }
        cap.usableFraction = usableArea / capArea;
        cap.sourceFraction = sourceArea / capArea;
        if (cap.usableFraction < params.minUsableFraction || roughSamples.empty()) {
            return cap;  // too little sky left to say anything (valid stays false)
        }
        const double noiseFloor = weightedMedian(roughSamples);
        const double flatLimit = std::max(params.flatFloorStops,
                                          params.flatNoiseMultiple * (std::isfinite(noiseFloor) ? noiseFloor : 0.0));

        // ---- the pixels that decide: flat ones when there are enough ---------------------
        double flatArea = 0.0;
        for (std::uint32_t y = 0; y < capRows; ++y) {
            const double weight = std::cos(deg2rad(90.0 - (static_cast<double>(y) + 0.5) * degPerRow));
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (covered[i] && !excluded[i] && rough[i] <= flatLimit) {
                    flatArea += weight;
                }
            }
        }
        cap.flatFraction = flatArea / usableArea;
        const bool useFlat = cap.flatFraction >= params.minFlatFraction;

        // ---- weighted medians of luminance and colour ratios -------------------------------
        std::vector<Weighted> lum, rg, bg;
        lum.reserve(roughSamples.size());
        rg.reserve(roughSamples.size());
        bg.reserve(roughSamples.size());
        for (std::uint32_t y = 0; y < capRows; ++y) {
            const double weight = std::cos(deg2rad(90.0 - (static_cast<double>(y) + 0.5) * degPerRow));
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (!covered[i] || excluded[i] || (useFlat && rough[i] > flatLimit)) {
                    continue;
                }
                const float* p = rgba + i * 4u;
                const double r = std::max(static_cast<double>(p[0]), kLogFloor);
                const double g = std::max(static_cast<double>(p[1]), kLogFloor);
                const double b = std::max(static_cast<double>(p[2]), kLogFloor);
                lum.push_back({static_cast<double>(logY[i]), weight});
                rg.push_back({std::log2(r / g), weight});
                bg.push_back({std::log2(b / g), weight});
            }
        }
        const double medianLog = weightedMedian(lum);
        const double medianRG = weightedMedian(rg);
        const double medianBG = weightedMedian(bg);
        if (!std::isfinite(medianLog) || !std::isfinite(medianRG) || !std::isfinite(medianBG)) {
            return cap;
        }
        // Stops against metered grey: log2(Y) - log2(grey).
        cap.stopsVsGrey = medianLog - std::log2(params.greyLinear);
        cap.log2RG = medianRG;
        cap.log2BG = medianBG;
        cap.frames = 1;
        cap.valid = true;
        return cap;
    } catch (...) {
        // Out of memory for a 1024-wide strip: no measurement, never a crash.
        return SkyCap{};
    }
}

Result<SkyCap> measureSkyCap(const geom::LensRig& rig, const video::FramePair& frames, const geom::BlendParams& blend,
                             const Vec3d& upBody, const OsvColorParams& linearColor, ThreadPool& pool,
                             const SkyCapParams& params) {
    // ---- inputs ---------------------------------------------------------------------
    if (!upBody.isFinite() || !(upBody.norm() > 0.5)) {
        return Error{ErrorCode::InvalidArgument, "measureSkyCap: no usable up direction"};
    }
    if (params.equirectW < 64u || params.equirectW > 8192u || (params.equirectW % 2u) != 0u) {
        return Error{ErrorCode::InvalidArgument, "measureSkyCap: equirect width must be even, 64..8192"};
    }
    if (!(params.capMinElevationDeg > 0.0 && params.capMinElevationDeg < 90.0) ||
        !(params.sourceExclusionDeg >= 0.0 && params.sourceExclusionDeg < 45.0)) {
        return Error{ErrorCode::InvalidArgument, "measureSkyCap: cap or exclusion angle out of range"};
    }
    if (linearColor.transfer != OSV_TRANSFER_LINEAR) {
        return Error{ErrorCode::InvalidArgument, "measureSkyCap: the colour block must decode to scene-linear"};
    }

    // ---- a levelled equirect: world +Z (the top row) is gravity-up ------------------------
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = static_cast<int>(params.equirectW);
    map.h = static_cast<int>(params.equirectW / 2u);
    RenderParamsBuilder builder;
    builder.rig(rig).equirect(map).stabilization(levelledBodyFromWorld(upBody)).blend(blend, true).color(linearColor);
    builder.alphaCoverage(true);
    OSV_TRY_ASSIGN(RenderJob job, builder.build(frames));

    // ---- shade only the cap and the exclusion margin below it ------------------------------
    // The cap is the top quarter at 45 deg; the margin lets a lamp just below
    // the cap edge still exclude its glow inside it.
    const double degPerRow = 180.0 / static_cast<double>(map.h);
    const double capDeg = 90.0 - params.capMinElevationDeg + params.sourceExclusionDeg;
    const auto rows = static_cast<std::uint32_t>(
        std::min<double>(static_cast<double>(map.h), std::ceil(capDeg / degPerRow) + 1.0));
    OSV_TRY_ASSIGN(std::vector<float> rgba, shadeJobRows(job, 0, rows, pool));

    // ---- statistics ---------------------------------------------------------------------------
    SkyCap cap = skyCapOf(rgba.data(), params.equirectW, rows, params);
    if (!cap.valid) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("measureSkyCap: too little open sky above the horizon ({:.0f} % of the cap usable)",
                                 100.0 * cap.usableFraction)};
    }
    return cap;
}

SkyCap combineSkyCaps(std::span<const SkyCap> caps) noexcept {
    try {
        // Each statistic's median over the valid frames: one dark frame (a
        // tunnel) or one bright one (a lamp overhead) cannot decide alone.
        std::vector<double> stops, rg, bg, usable, flat, source;
        for (const SkyCap& c : caps) {
            if (!c.valid) {
                continue;
            }
            stops.push_back(c.stopsVsGrey);
            rg.push_back(c.log2RG);
            bg.push_back(c.log2BG);
            usable.push_back(c.usableFraction);
            flat.push_back(c.flatFraction);
            source.push_back(c.sourceFraction);
        }
        SkyCap out;
        if (stops.empty()) {
            return out;
        }
        out.frames = static_cast<std::uint32_t>(stops.size());
        out.stopsVsGrey = medianOf(std::move(stops));
        out.log2RG = medianOf(std::move(rg));
        out.log2BG = medianOf(std::move(bg));
        out.usableFraction = medianOf(std::move(usable));
        out.flatFraction = medianOf(std::move(flat));
        out.sourceFraction = medianOf(std::move(source));
        out.valid = std::isfinite(out.stopsVsGrey) && std::isfinite(out.log2BG) && std::isfinite(out.log2RG);
        return out;
    } catch (...) {
        return SkyCap{};
    }
}

std::optional<Vec3d> bodyUpAt(const geom::AttitudeTrack& attitude, const meta::MetadataTrack& track,
                              std::uint32_t frame, double fps) noexcept {
    try {
        if (attitude.sampleCount() == 0) {
            return std::nullopt;
        }
        // The frame's own metadata timestamp, as the stabilisation reads it;
        // nominal spacing from the track start when the sample is unreadable.
        double tUs = attitude.beginUs();
        auto meta = track.frame(frame);
        if (meta.ok()) {
            tUs = static_cast<double>(meta.value().timestampUs);
        } else if (std::isfinite(fps) && fps > 0.0) {
            tUs += static_cast<double>(frame) * 1e6 / fps;
        }
        // World up seen from the body: the inverse rotation of world up.
        const Quatd worldFromBody = attitude.worldFromBody(tUs);
        const Vec3d up = worldFromBody.conj().rotate(attitude.worldUp());
        if (!up.isFinite() || !(up.norm() > 0.5)) {
            return std::nullopt;
        }
        return up.normalized();
    } catch (...) {
        return std::nullopt;
    }
}

// =============================================================================
//  Classification
// =============================================================================
bool meteredLightSaysDark(const MeteredLight& metered, const SceneLightThresholds& thresholds) noexcept {
    return metered.valid && std::isfinite(metered.median) && metered.median < thresholds.nightMaxLv;
}

SceneLightVerdict classifySceneLight(const MeteredLight& metered, const std::optional<SkyCap>& cap,
                                     const SceneLightThresholds& thresholds) {
    SceneLightVerdict v;
    v.metered = metered;
    v.cap = cap;
    const char* unit = metered.fromAecLv ? "LV" : "EV100";

    // ---- no exposure metadata at all: today's profile -------------------------------------
    if (!metered.valid || !std::isfinite(metered.median)) {
        v.reason = "no exposure metadata, so the day profile";
        return v;
    }
    // ---- clearly bright: the cap cannot make it night --------------------------------------
    if (metered.median >= thresholds.dayMinLv) {
        v.reason = std::format("metered light {} {:.1f} is daylight", unit, metered.median);
        return v;
    }
    if (!meteredLightSaysDark(metered, thresholds)) {
        v.reason = std::format("metered light {} {:.1f} is between night and day, so the day profile", unit,
                               metered.median);
        return v;
    }

    // ---- dark metered light: only the sky can confirm night ----------------------------------
    v.needsCap = true;
    if (!cap || !cap->valid) {
        v.reason = std::format("metered light {} {:.1f} is dark, but the sky could not be measured, so the day "
                               "profile",
                               unit, metered.median);
        return v;
    }
    // A bright or blue sky is daylight through an ND filter, whatever the meter says.
    if (cap->stopsVsGrey >= thresholds.dayMinCapStops && cap->log2BG > thresholds.blueMinLog2BG) {
        v.reason = std::format("the sky is bright and blue ({:+.1f} stops): daylight through an ND filter",
                               cap->stopsVsGrey);
        return v;
    }
    if (cap->stopsVsGrey <= thresholds.nightMaxCapStops) {
        v.light = SceneLight::Night;
        v.reason = std::format("metered light {} {:.1f} and a sky {:.1f} stops below grey", unit, metered.median,
                               -cap->stopsVsGrey);
        return v;
    }
    v.reason = std::format("the sky is not dark enough for night ({:+.1f} stops), so the day profile",
                           cap->stopsVsGrey);
    return v;
}

std::string sceneLightEvidence(const SceneLightVerdict& verdict) {
    if (!verdict.metered.valid) {
        return "no exposure metadata";
    }
    std::string text = std::format("{} {:.1f}", verdict.metered.fromAecLv ? "LV" : "EV100", verdict.metered.median);
    if (verdict.cap && verdict.cap->valid) {
        text += std::format(", sky {:+.1f} stops", verdict.cap->stopsVsGrey);
    }
    return text;
}

// =============================================================================
//  The night profile
// =============================================================================
void applyNightPhotoProfile(PhotoSeamParams& params) noexcept {
    params.decayDeg = kNightPhotoDecayDeg;
    params.chromaDecayScale = kNightPhotoChromaDecayScale;
    params.maxAbsLog2Gain = kNightPhotoMaxAbsLog2Gain;
}

void applyNightShadingProfile(LensShadingParams& params) noexcept { params.mode = LensShadingMode::Off; }

}  // namespace osv::render
