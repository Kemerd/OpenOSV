// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PhotoSeam.cpp - the render-only seam edge inset and the section 1.4 sky
// metrics (see PhotoSeam.h and docs/research/NEURAL_STITCHING.md, section 8).

#include "osv/render/PhotoSeam.h"

#include "osv/color/ColorParams.h"
#include "osv/core/Log.h"
#include "osv/geom/EquirectMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace osv::render {

namespace {

/// Floor of every logarithm below, as in the research scripts (EPS = 1e-5):
/// a black pixel must not turn into -inf and dominate an RMS.
constexpr double kLogFloor = 1e-5;

/// BT.2020 luma of a linear RGB triple - the same weights the research
/// (bandio.luma) and SeamAnalysis.cpp use.
[[nodiscard]] inline double luma2020(const float* px) noexcept {
    return 0.2627 * static_cast<double>(px[0]) + 0.6780 * static_cast<double>(px[1]) +
           0.0593 * static_cast<double>(px[2]);
}

/// log2 with the research floor.
[[nodiscard]] inline double log2Floor(double v) noexcept { return std::log2(std::max(v, kLogFloor)); }

/// Gaussian blur of every column of a (rows x cols) row-major matrix along
/// the ROW axis only, sigma in rows, radius ceil(4 sigma), replicated
/// borders - cv2.GaussianBlur(sigmaX ~ 0, sigmaY = sigma, BORDER_REPLICATE)
/// as the research scripts call it.  sigma <= 0 copies.
[[nodiscard]] std::vector<double> blurRows(const std::vector<double>& in, std::size_t rows, std::size_t cols,
                                           double sigma) {
    if (!(sigma > 0.0) || rows == 0 || cols == 0) {
        return in;
    }
    // Normalised kernel taps, 4-sigma radius (OpenCV's float default).
    const int radius = std::max(1, static_cast<int>(std::ceil(4.0 * sigma)));
    std::vector<double> taps(static_cast<std::size_t>(2 * radius + 1));
    double sum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double t = std::exp(-0.5 * (static_cast<double>(i) * i) / (sigma * sigma));
        taps[static_cast<std::size_t>(i + radius)] = t;
        sum += t;
    }
    for (double& t : taps) {
        t /= sum;
    }
    // Convolve each column; out-of-range rows replicate the edge row.
    std::vector<double> out(in.size(), 0.0);
    const int R = static_cast<int>(rows);
    for (int r = 0; r < R; ++r) {
        double* dst = out.data() + static_cast<std::size_t>(r) * cols;
        for (int k = -radius; k <= radius; ++k) {
            const int rr = std::clamp(r + k, 0, R - 1);
            const double t = taps[static_cast<std::size_t>(k + radius)];
            const double* src = in.data() + static_cast<std::size_t>(rr) * cols;
            for (std::size_t c = 0; c < cols; ++c) {
                dst[c] += t * src[c];
            }
        }
    }
    return out;
}

/// RMS of (a - b) over the rows whose flag is set, all columns.  Returns 0
/// for an empty selection (the caller has already refused that case).
[[nodiscard]] double rmsRows(const std::vector<double>& a, const std::vector<double>& b, std::size_t cols,
                             const std::vector<std::uint8_t>& rowOn) {
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t r = 0; r < rowOn.size(); ++r) {
        if (!rowOn[r]) {
            continue;
        }
        for (std::size_t c = 0; c < cols; ++c) {
            const double d = a[r * cols + c] - b[r * cols + c];
            acc += d * d;
            ++n;
        }
    }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

}  // namespace

// ===========================================================================
//  Stage 1: the inset
// ===========================================================================

geom::BlendParams insetRenderBlend(const geom::BlendParams& analysis, double insetDeg, double featherDeg) noexcept {
    // Zero, negative or garbage insets are "no inset": return the analysis
    // blend untouched, so the render is bit-identical to one built from it.
    if (!std::isfinite(insetDeg) || insetDeg <= 0.0) {
        return analysis;
    }
    const double inset = std::min(insetDeg, kMaxSeamInsetDeg);
    geom::BlendParams out = analysis;
    // The FOV is the full cone angle, so the half angle moves by `inset`.
    out.lensFovDeg = analysis.lensFovDeg - 2.0 * inset;
    // A garbage feather keeps the analysis one rather than a zero-width edge.
    out.featherDeg = (std::isfinite(featherDeg) && featherDeg > 0.0) ? featherDeg : analysis.featherDeg;
    return out;
}

// ===========================================================================
//  Band rows through the kernel
// ===========================================================================

Result<std::vector<float>> shadeJobRows(const RenderJob& job, std::uint32_t row0, std::uint32_t row1,
                                        ThreadPool& pool) {
    if (!job.valid()) {
        return Error{ErrorCode::InvalidArgument, "shadeJobRows: invalid render job"};
    }
    if (job.planesOnDevice[0] || job.planesOnDevice[1]) {
        return Error{ErrorCode::InvalidArgument, "shadeJobRows: the frames are on the GPU; host frames are required"};
    }
    // Copies of the POD blocks, exactly as CpuRenderer takes them.
    const OsvRenderParams params = job.params;
    if (row1 <= row0 || row1 > static_cast<std::uint32_t>(params.outH)) {
        return Error{ErrorCode::InvalidArgument, "shadeJobRows: row range outside the map"};
    }
    const OsvPlane planes[2] = {job.planes[0], job.planes[1]};
    const float* seam = (params.seamShiftEnabled && !job.seamShiftDeg.empty()) ? job.seamShiftDeg.data() : nullptr;
    const float* warp = (params.warpEnabled && !job.warpGrid.empty()) ? job.warpGrid.data() : nullptr;
    const std::size_t width = static_cast<std::size_t>(params.outW);
    std::vector<float> rgba(width * (row1 - row0) * 4u, 0.0f);
    float* base = rgba.data();

    // One row per task: the bands are a few hundred rows at most.
    Status st = pool.parallelFor(row0, row1, 1, [&](std::size_t rBegin, std::size_t rEnd) {
        for (std::size_t y = rBegin; y < rEnd; ++y) {
            float* row = base + (y - row0) * width * 4u;
            for (int x = 0; x < params.outW; ++x) {
                osvShadePixelW(&params, planes, seam, warp, x, static_cast<int>(y),
                               row + static_cast<std::size_t>(x) * 4u);
            }
        }
    });
    OSV_TRY(st);
    return rgba;
}

// ===========================================================================
//  Metrics
// ===========================================================================

double MetricBands::latDeg(std::uint32_t r) const noexcept {
    // Row centres, polar-axis layout: +90 at map row 0.
    if (mapH == 0) {
        return 0.0;
    }
    return 90.0 - (static_cast<double>(rowOffset) + static_cast<double>(r) + 0.5) * 180.0 / static_cast<double>(mapH);
}

Result<MetricBands> renderMetricBands(const RenderParamsBuilder& builder, const video::FramePair& frames,
                                      const MetricBandRequest& request, ThreadPool& pool) {
    if (request.mapW < 256 || request.mapW > 16384 || (request.mapW % 2) != 0 || !(request.halfDeg > 10.0) ||
        request.halfDeg > 60.0) {
        return Error{ErrorCode::InvalidArgument, "renderMetricBands: bad band geometry"};
    }
    // The full polar map's geometry, of which only the band rows are shaded.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = static_cast<int>(request.mapW);
    map.h = static_cast<int>(request.mapW / 2);
    const std::uint32_t mapH = static_cast<std::uint32_t>(map.h);
    const double rowsPerDeg = static_cast<double>(mapH) / 180.0;
    const std::uint32_t halfRows = static_cast<std::uint32_t>(std::lround(request.halfDeg * rowsPerDeg));
    const std::uint32_t centre = mapH / 2;
    const std::uint32_t row0 = centre > halfRows ? centre - halfRows : 0;
    const std::uint32_t row1 = std::min(mapH, centre + halfRows);
    if (row1 <= row0) {
        return Error{ErrorCode::InvalidArgument, "renderMetricBands: band has no rows"};
    }

    // Scene-linear output with the default decode, like every band analysis.
    const OsvColorParams linear =
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);

    MetricBands out;
    out.w = request.mapW;
    out.h = row1 - row0;
    out.rowOffset = row0;
    out.mapH = mapH;
    // Variant 0 = blended, 1 = lens 0 alone, 2 = lens 1 alone.
    for (int variant = 0; variant < 3; ++variant) {
        RenderParamsBuilder b = builder;
        b.equirect(map).color(linear).alphaCoverage(true);
        if (variant > 0) {
            const int lens = variant - 1;
            b.lensEnabled(lens, true).lensEnabled(1 - lens, false);
        } else {
            b.lensEnabled(0, true).lensEnabled(1, true);
        }
        OSV_TRY_ASSIGN(RenderJob job, b.build(frames));
        OSV_TRY_ASSIGN(std::vector<float> rows, shadeJobRows(job, row0, row1, pool));
        if (variant == 0) {
            out.blend = std::move(rows);
        } else {
            out.lens[variant - 1] = std::move(rows);
        }
    }
    return out;
}

std::vector<std::uint8_t> coValidTrustMask(const MetricBands& bands) {
    const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
    std::vector<std::uint8_t> mask(n, 0);
    if (bands.lens[0].size() != n * 4u || bands.lens[1].size() != n * 4u) {
        return mask;  // nothing trusted rather than an out-of-bounds read
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float* a = bands.lens[0].data() + i * 4u;
        const float* b = bands.lens[1].data() + i * 4u;
        bool ok = a[3] >= 0.99f && b[3] >= 0.99f;
        for (int c = 0; c < 3 && ok; ++c) {
            ok = std::isfinite(a[c]) && std::isfinite(b[c]);
        }
        mask[i] = ok ? 1u : 0u;
    }
    return mask;
}

Result<SkySeamMetrics> skySeamMetrics(const MetricBands& bands, const std::vector<std::uint8_t>& trust,
                                      double colBeginFrac, double colEndFrac) {
    const std::size_t w = bands.w;
    const std::size_t h = bands.h;
    const std::size_t n = w * h;
    if (w < 128 || h < 3 || bands.mapH == 0 || bands.blend.size() != n * 4u || bands.lens[0].size() != n * 4u ||
        bands.lens[1].size() != n * 4u || trust.size() != n) {
        return Error{ErrorCode::InvalidArgument, "skySeamMetrics: band buffers do not match the band size"};
    }
    if (!(colBeginFrac >= 0.0) || !(colEndFrac <= 1.0) || !(colEndFrac > colBeginFrac)) {
        return Error{ErrorCode::InvalidArgument, "skySeamMetrics: bad column range"};
    }
    // Research: cols[int(0.20 * w):int(0.44 * w)].
    const std::size_t c0 = static_cast<std::size_t>(colBeginFrac * static_cast<double>(w));
    const std::size_t c1 = std::min(w, static_cast<std::size_t>(colEndFrac * static_cast<double>(w)));
    // Block size: 32 of 4096 columns, i.e. w / 128, at least 1.
    const std::size_t block = std::max<std::size_t>(1, w / 128u);
    const std::size_t nb = (c1 > c0) ? (c1 - c0) / block : 0;
    if (nb == 0) {
        return Error{ErrorCode::InvalidArgument, "skySeamMetrics: column range narrower than one block"};
    }

    // ---- block-averaged log2 luma profile -----------------------------------
    std::vector<double> blk(h * nb, 0.0);
    for (std::size_t r = 0; r < h; ++r) {
        for (std::size_t b = 0; b < nb; ++b) {
            double acc = 0.0;
            for (std::size_t k = 0; k < block; ++k) {
                const std::size_t c = c0 + b * block + k;
                acc += log2Floor(luma2020(bands.blend.data() + (r * w + c) * 4u));
            }
            blk[r * nb + b] = acc / static_cast<double>(block);
        }
    }

    // ---- latitude row selections ---------------------------------------------
    std::vector<std::uint8_t> r10(h, 0);
    std::vector<std::uint8_t> r25(h, 0);
    bool any10 = false;
    bool any25 = false;
    for (std::size_t r = 0; r < h; ++r) {
        const double lat = std::fabs(bands.latDeg(static_cast<std::uint32_t>(r)));
        r10[r] = lat <= 10.0 ? 1u : 0u;
        r25[r] = lat <= 25.0 ? 1u : 0u;
        any10 = any10 || r10[r];
        any25 = any25 || r25[r];
    }
    if (!any10 || !any25) {
        return Error{ErrorCode::InvalidArgument, "skySeamMetrics: the band does not reach +-10 deg"};
    }

    // ---- the three luminance terms --------------------------------------------
    // Research: bl(x, s) blurs along latitude with sigma s degrees in rows.
    const double rowsPerDeg = static_cast<double>(bands.mapH) / 180.0;
    const auto bl = [&](double sDeg) { return blurRows(blk, h, nb, sDeg * rowsPerDeg); };
    SkySeamMetrics m;
    m.line = rmsRows(blk, bl(1.5), nb, r10) * 1000.0;
    m.band = rmsRows(bl(0.5), bl(4.0), nb, r10) * 1000.0;
    m.broad = rmsRows(bl(2.0), bl(10.0), nb, r25) * 1000.0;

    // ---- colour term: the two lenses' chroma on trusted sky pixels -------------
    double acc = 0.0;
    std::uint64_t count = 0;
    for (std::size_t r = 0; r < h; ++r) {
        for (std::size_t c = c0; c < c1; ++c) {
            const std::size_t i = r * w + c;
            if (!trust[i]) {
                continue;
            }
            const float* a = bands.lens[0].data() + i * 4u;
            const float* b = bands.lens[1].data() + i * 4u;
            // log2(R/G) and log2(B/G) of each lens, floored like the research.
            const double ga = std::max(static_cast<double>(a[1]), kLogFloor);
            const double gb = std::max(static_cast<double>(b[1]), kLogFloor);
            const double ra = std::log2(std::max(static_cast<double>(a[0]), kLogFloor) / ga);
            const double ba = std::log2(std::max(static_cast<double>(a[2]), kLogFloor) / ga);
            const double rb = std::log2(std::max(static_cast<double>(b[0]), kLogFloor) / gb);
            const double bb = std::log2(std::max(static_cast<double>(b[2]), kLogFloor) / gb);
            acc += (ra - rb) * (ra - rb) + (ba - bb) * (ba - bb);
            count += 2;  // both ratios count as samples, as numpy's mean over [..., 2] does
        }
    }
    m.trustedPixels = count / 2;
    m.dE = count ? std::sqrt(acc / static_cast<double>(count)) * 1000.0 : 0.0;
    return m;
}

}  // namespace osv::render
