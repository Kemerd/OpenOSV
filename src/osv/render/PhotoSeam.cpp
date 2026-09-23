// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PhotoSeam.cpp - the render-only seam edge inset and the section 1.4 sky
// metrics (see PhotoSeam.h and docs/research/NEURAL_STITCHING.md, section 8).

#include "osv/render/PhotoSeam.h"

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/core/Log.h"
#include "osv/core/Math.h"
#include "osv/geom/EquirectMap.h"
#include "osv/render/DeviceBandShader.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamCarve.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

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
    // The carved blend seam and the photometric seam table (job.valid()
    // checked both sizes).
    const float* blendSeam = (params.blendSeamEnabled && !job.blendSeam.empty()) ? job.blendSeam.data() : nullptr;
    const float* photo = (params.photoEnabled && !job.photoField.empty()) ? job.photoField.data() : nullptr;
    const std::size_t width = static_cast<std::size_t>(params.outW);
    std::vector<float> rgba(width * (row1 - row0) * 4u, 0.0f);
    float* base = rgba.data();

    // One row per task: the bands are a few hundred rows at most.
    Status st = pool.parallelFor(row0, row1, 1, [&](std::size_t rBegin, std::size_t rEnd) {
        for (std::size_t y = rBegin; y < rEnd; ++y) {
            float* row = base + (y - row0) * width * 4u;
            for (int x = 0; x < params.outW; ++x) {
                osvShadePixelWSP(&params, planes, seam, warp, blendSeam, photo, x, static_cast<int>(y),
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

// ===========================================================================
//  Stage 2: helpers
// ===========================================================================

namespace {

using PhotoClock = std::chrono::steady_clock;

/// Milliseconds since `t0`.
[[nodiscard]] double msSince(PhotoClock::time_point t0) noexcept {
    return std::chrono::duration<double, std::milli>(PhotoClock::now() - t0).count();
}

/// Wrap an index into [0, n) (n > 0).
[[nodiscard]] inline int wrapIndex(int i, int n) noexcept {
    i %= n;
    return i < 0 ? i + n : i;
}

/// Median of `v`, reordering it; the mean of the two middle values for an
/// even count, as numpy's median.  NaN for an empty vector.
[[nodiscard]] double medianInPlace(std::vector<double>& v) {
    if (v.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = v[mid];
    if ((v.size() % 2u) == 1u) {
        return hi;
    }
    const double lo = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (lo + hi);
}

/// Gaussian smoothing of a periodic ring, sigma in samples (3-sigma taps).
/// A sigma below a quarter sample copies.
[[nodiscard]] std::vector<double> smoothRing(const std::vector<double>& in, double sigma) {
    const int n = static_cast<int>(in.size());
    if (n == 0 || !(sigma >= 0.25) || !std::isfinite(sigma)) {
        return in;
    }
    const int radius = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    std::vector<double> taps(static_cast<std::size_t>(2 * radius + 1));
    double sum = 0.0;
    for (int k = -radius; k <= radius; ++k) {
        const double t = std::exp(-0.5 * (static_cast<double>(k) * k) / (sigma * sigma));
        taps[static_cast<std::size_t>(k + radius)] = t;
        sum += t;
    }
    std::vector<double> out(in.size(), 0.0);
    for (int i = 0; i < n; ++i) {
        double acc = 0.0;
        for (int k = -radius; k <= radius; ++k) {
            acc += taps[static_cast<std::size_t>(k + radius)] * in[static_cast<std::size_t>(wrapIndex(i + k, n))];
        }
        out[static_cast<std::size_t>(i)] = acc / sum;
    }
    return out;
}

/// Running minimum over +-k samples of a periodic ring.
[[nodiscard]] std::vector<double> runningMinRing(const std::vector<double>& in, int k) {
    const int n = static_cast<int>(in.size());
    if (n == 0 || k <= 0) {
        return in;
    }
    std::vector<double> out(in.size());
    for (int i = 0; i < n; ++i) {
        double m = in[static_cast<std::size_t>(i)];
        for (int d = -k; d <= k; ++d) {
            m = std::min(m, in[static_cast<std::size_t>(wrapIndex(i + d, n))]);
        }
        out[static_cast<std::size_t>(i)] = m;
    }
    return out;
}

/// Raw per-column rim measurements (degrees, NaN = not measured) -> the rim
/// the kernel uses, exactly as rim_experiments.py:rim_limits() finishes it:
/// unmeasured columns take the median of the measured ones, then a
/// CONSERVATIVE running minimum along longitude (a rim is never trusted
/// further out than its neighbours say), a Gaussian, and the clamp to
/// [thetaMax - rimMaxTrim, thetaMax].  With nothing measured the rim stays
/// at thetaMax: no evidence, no inset.
[[nodiscard]] std::vector<double> finishRim(const std::vector<double>& raw, double degPerSample,
                                            const PhotoSeamParams& params, double thetaMaxDeg) {
    std::vector<double> out(raw.size(), thetaMaxDeg);
    std::vector<double> measured;
    measured.reserve(raw.size());
    for (const double v : raw) {
        if (std::isfinite(v)) {
            measured.push_back(v);
        }
    }
    if (measured.empty() || !(degPerSample > 0.0)) {
        return out;
    }
    const double fill = medianInPlace(measured);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[i] = std::isfinite(raw[i]) ? raw[i] : fill;
    }
    const int k = std::max(0, static_cast<int>(std::lround(std::max(params.rimMinHalfDeg, 0.0) / degPerSample)));
    out = runningMinRing(out, k);
    out = smoothRing(out, std::max(params.rimSmoothDeg, 0.0) / degPerSample);
    const double lo = thetaMaxDeg - std::clamp(params.rimMaxTrimDeg, 0.0, 45.0);
    for (double& v : out) {
        v = std::clamp(v, lo, thetaMaxDeg);
    }
    return out;
}

/// True when every PhotoSeamParams knob the measurement divides by or loops
/// over is usable.
[[nodiscard]] bool paramsUsable(const PhotoSeamParams& p) noexcept {
    const auto pos = [](double v) { return std::isfinite(v) && v > 0.0; };
    const auto nonNeg = [](double v) { return std::isfinite(v) && v >= 0.0; };
    return p.gridW >= 8 && p.gridW <= 4096 && p.gridH >= 2 && p.gridH <= 256 && pos(p.rimDropStops) &&
           std::isfinite(p.rimCoreLoDeg) && std::isfinite(p.rimCoreHiDeg) && p.rimCoreHiDeg > p.rimCoreLoDeg &&
           pos(p.rimBinDeg) && pos(p.rimBlockDeg) && nonNeg(p.rimMinHalfDeg) && nonNeg(p.rimSmoothDeg) &&
           nonNeg(p.rimMaxTrimDeg) && std::isfinite(p.trustAlpha) && p.trustAlpha > 0.0 && p.trustAlpha <= 1.0 &&
           nonNeg(p.trustMarginDeg) && pos(p.flatLog2PerDeg) && pos(p.gradStepDeg) && pos(p.sigmaLonDeg) &&
           pos(p.sigmaLatDeg) && pos(p.fillSigmaLonDeg) && nonNeg(p.minSupport) && pos(p.maxAbsLog2Gain) &&
           nonNeg(p.minTrustedFraction);
}

/// Run `body` over [0, n) on `pool` when there is one, inline otherwise.
template <class Body>
Status forRange(ThreadPool* pool, std::size_t n, std::size_t grain, const Body& body) {
    if (n == 0) {
        return okStatus();
    }
    if (pool) {
        return pool->parallelFor(0, n, grain, body);
    }
    body(std::size_t{0}, n);
    return okStatus();
}

/// Body-frame direction of polar-axis band pixel (col, row) of a map
/// `w` x `mapH` whose band starts at `rowOffset` - the kernel's
/// osvRayForPixel for OSV_LAYOUT_POLAR_AXIS, in double.
[[nodiscard]] Vec3d polarDirection(double col, double row, std::uint32_t w, std::uint32_t rowOffset,
                                   std::uint32_t mapH) noexcept {
    const double lon = ((col + 0.5) / static_cast<double>(w)) * kTwoPi - kPi;
    const double lat = kHalfPi - ((static_cast<double>(rowOffset) + row + 0.5) / static_cast<double>(mapH)) * kPi;
    const double cl = std::cos(lat);
    return Vec3d{cl * std::sin(lon), std::sin(lat), cl * std::cos(lon)};
}

/// Angle of a body direction from lens `i`'s optical axis (radians).
[[nodiscard]] double lensTheta(const geom::LensRig& rig, int i, const Vec3d& dBody) noexcept {
    const Vec3d dl = rig.bodyToLens[static_cast<std::size_t>(i)] * dBody;
    return std::atan2(std::hypot(dl.x, dl.y), dl.z);
}

}  // namespace

// ===========================================================================
//  Stage 2: small pieces
// ===========================================================================

const char* photoSeamModeName(PhotoSeamMode mode) noexcept {
    switch (mode) {
    case PhotoSeamMode::Off: return "off";
    case PhotoSeamMode::RimOnly: return "rim";
    case PhotoSeamMode::RimAndGain: return "full";
    }
    return "unknown";
}

bool PhotoSeamField::valid() const noexcept {
    if (w < 2 || h < 2 || w > 65536 || h > 4096) {
        return false;
    }
    const std::size_t cells = static_cast<std::size_t>(w) * h;
    if (gain.size() != cells * 3u || rim.size() != static_cast<std::size_t>(w) * 2u) {
        return false;
    }
    if (!rimMeasured.empty() && rimMeasured.size() != rim.size()) {
        return false;
    }
    if (!std::isfinite(latMinRad) || !std::isfinite(latMaxRad) || !(latMaxRad - latMinRad > 1e-6f)) {
        return false;
    }
    for (const float t : thetaMaxRad) {
        if (!std::isfinite(t) || !(t > 0.0f)) {
            return false;
        }
    }
    for (const float g : gain) {
        if (!std::isfinite(g)) {
            return false;
        }
    }
    for (const float r : rim) {
        if (!std::isfinite(r) || !(r > 0.0f)) {
            return false;
        }
    }
    return true;
}

std::vector<float> PhotoSeamField::kernelTable() const {
    if (!valid()) {
        return {};
    }
    std::vector<float> table;
    table.reserve(gain.size() + rim.size());
    table.insert(table.end(), gain.begin(), gain.end());
    table.insert(table.end(), rim.begin(), rim.end());
    return table;
}

float photoCodePerStop(const OsvColorParams& color) noexcept {
    if (!color.enabled) {
        return 0.0f;
    }
    // Green channel of the decode for a neutral code (the curve is per
    // channel, so any channel gives the same answer for code, code, code).
    const auto decode = [&color](double code) {
        const float c[3] = {static_cast<float>(code), static_cast<float>(code), static_cast<float>(code)};
        float lin[3] = {0.0f, 0.0f, 0.0f};
        osvCodeToLinear(&color, c, lin);
        return static_cast<double>(lin[1]);
    };
    // Bisection for the code of a linear target on [0, 1]; the decodes are
    // monotone increasing.  NaN when the target is outside the curve.
    const auto codeFor = [&decode](double target) {
        double lo = 0.0;
        double hi = 1.0;
        const double dLo = decode(lo);
        const double dHi = decode(hi);
        if (!std::isfinite(dLo) || !std::isfinite(dHi) || !(dLo < target) || !(dHi > target)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            if (decode(mid) < target) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        return 0.5 * (lo + hi);
    };
    // One stop centred on mid grey (0.18): the log segment's slope there.
    const double c0 = codeFor(0.18 * std::exp2(-0.5));
    const double c1 = codeFor(0.18 * std::exp2(0.5));
    const double perStop = c1 - c0;
    if (!std::isfinite(perStop) || !(perStop > 0.0) || perStop > 1.0) {
        return 0.0f;
    }
    return static_cast<float>(perStop);
}

double photoRimDegAt(const PhotoSeamField& field, int lens, double lonRad) noexcept {
    if ((lens != 0 && lens != 1) || field.w < 2 || field.rim.size() != static_cast<std::size_t>(field.w) * 2u ||
        !std::isfinite(lonRad)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // The kernel's own convention: column j at lon = -pi + j 2pi / w, linear
    // in between, wrapping.
    const int W = static_cast<int>(field.w);
    const double fx = ((lonRad + kPi) / kTwoPi) * static_cast<double>(W);
    const double flx = std::floor(fx);
    const double t = fx - flx;
    const int x0 = wrapIndex(static_cast<int>(flx), W);
    const int x1 = wrapIndex(static_cast<int>(flx) + 1, W);
    const double a = field.rim[static_cast<std::size_t>(x0) * 2u + static_cast<std::size_t>(lens)];
    const double b = field.rim[static_cast<std::size_t>(x1) * 2u + static_cast<std::size_t>(lens)];
    return rad2deg(a + (b - a) * t);
}

std::vector<float> photoRimColumnsDeg(const PhotoSeamField& field, int lens, std::uint32_t columns) {
    std::vector<float> out;
    if ((lens != 0 && lens != 1) || columns == 0 || columns > 65536 || !field.valid()) {
        return out;
    }
    out.resize(columns);
    for (std::uint32_t c = 0; c < columns; ++c) {
        const double lon = ((static_cast<double>(c) + 0.5) / static_cast<double>(columns)) * kTwoPi - kPi;
        out[c] = static_cast<float>(photoRimDegAt(field, lens, lon));
    }
    return out;
}

// ===========================================================================
//  Stage 2: the bands
// ===========================================================================

Result<RgbLensBands> renderPhotoBands(const geom::LensRig& rig, const video::FramePair& frames,
                                      const geom::BlendParams& blend, const PhotoSeamParams& params,
                                      ThreadPool& pool) {
    const BandParams& band = params.band;
    if (band.equirectW < 256 || band.equirectW > 16384 || (band.equirectW % 2u) != 0 || !(band.bandHalfDeg > 0.0) ||
        band.bandHalfDeg > 45.0) {
        return Error{ErrorCode::InvalidArgument, "renderPhotoBands: bad band parameters"};
    }
    // The full polar-axis map geometry (identical to production), of which
    // only the band rows are shaded.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = static_cast<int>(band.equirectW);
    map.h = static_cast<int>(band.equirectW / 2);
    const std::uint32_t mapH = static_cast<std::uint32_t>(map.h);
    const double rowsPerDeg = static_cast<double>(mapH) / 180.0;
    const std::uint32_t halfRows = static_cast<std::uint32_t>(std::lround(band.bandHalfDeg * rowsPerDeg));
    const std::uint32_t centre = mapH / 2;
    const std::uint32_t row0 = centre > halfRows ? centre - halfRows : 0;
    const std::uint32_t row1 = std::min(mapH, centre + halfRows);
    if (row1 <= row0) {
        return Error{ErrorCode::InvalidArgument, "renderPhotoBands: band has no rows"};
    }

    // The ANALYSIS blend's occlusion, but no FOV feather: alpha is then the
    // occlusion factor alone (1 up to thetaMax), so rim decisions are made on
    // the raw fall-off rather than on a feather that already hides it.
    geom::BlendParams occlusionOnly = blend;
    occlusionOnly.featherDeg = 0.0;
    const OsvColorParams linear = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);

    RgbLensBands out;
    out.w = band.equirectW;
    out.h = row1 - row0;
    out.rowOffset = row0;
    out.mapH = mapH;
    const std::size_t n = static_cast<std::size_t>(out.w) * out.h;
    for (int lens = 0; lens < 2; ++lens) {
        RenderParamsBuilder builder;
        builder.rig(rig).equirect(map).blend(occlusionOnly, true).color(linear).alphaCoverage(true);
        builder.lensEnabled(lens, true).lensEnabled(1 - lens, false);
        OSV_TRY_ASSIGN(RenderJob job, builder.build(frames));
        std::vector<float> rgba;
        if (job.planesOnDevice[0] || job.planesOnDevice[1]) {
            // Frames in VRAM: the installed GPU band shader runs the same
            // shared kernel on the device (DeviceBandShader.h).
            std::shared_ptr<DeviceBandShader> gpu = deviceBandShader();
            if (!gpu) {
                return Error{ErrorCode::InvalidArgument,
                             "renderPhotoBands: the frames are on the GPU and no device band shader is installed "
                             "(osv::render::installCudaAnalyses)"};
            }
            OSV_TRY_ASSIGN(rgba, gpu->shadeRowsRgba(job, row0, row1));
        } else {
            OSV_TRY_ASSIGN(rgba, shadeJobRows(job, row0, row1, pool));
        }
        if (rgba.size() != n * 4u) {
            return Error{ErrorCode::Internal, "renderPhotoBands: the band shader returned the wrong size"};
        }
        out.rgba[lens] = std::move(rgba);
        out.thetaMaxRad[lens] = static_cast<float>(geom::effectiveThetaMax(lens, blend));
        out.thetaRad[lens].assign(n, 0.0f);
    }

    // Each pixel's angle from both lens axes, from the rig in double (the
    // bands' own geometry; independent of the frames).
    Status st = pool.parallelFor(0, out.h, 4, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            for (std::uint32_t c = 0; c < out.w; ++c) {
                const Vec3d d = polarDirection(static_cast<double>(c), static_cast<double>(r), out.w, out.rowOffset,
                                               out.mapH);
                const std::size_t i = r * out.w + c;
                out.thetaRad[0][i] = static_cast<float>(lensTheta(rig, 0, d));
                out.thetaRad[1][i] = static_cast<float>(lensTheta(rig, 1, d));
            }
        }
    });
    OSV_TRY(st);
    return out;
}

// ===========================================================================
//  Stage 2: the field from the bands
// ===========================================================================

Result<PhotoSeamField> photoSeamFromBands(const RgbLensBands& b, const PhotoSeamParams& P, ThreadPool* pool) {
    const auto t0 = PhotoClock::now();

    // ---- validation -------------------------------------------------------------
    const std::size_t W = b.w;
    const std::size_t H = b.h;
    const std::size_t N = W * H;
    if (W < 64 || H < 8 || b.mapH == 0 || static_cast<std::size_t>(b.rowOffset) + H > b.mapH) {
        return Error{ErrorCode::InvalidArgument, "photoSeamFromBands: bad band geometry"};
    }
    for (int i = 0; i < 2; ++i) {
        if (b.rgba[i].size() != N * 4u || b.thetaRad[i].size() != N || !std::isfinite(b.thetaMaxRad[i]) ||
            !(b.thetaMaxRad[i] > static_cast<float>(kHalfPi))) {
            return Error{ErrorCode::InvalidArgument, "photoSeamFromBands: band buffers do not match the band"};
        }
    }
    if (!paramsUsable(P)) {
        return Error{ErrorCode::InvalidArgument, "photoSeamFromBands: unusable parameters"};
    }
    const double degPerCol = 360.0 / static_cast<double>(W);
    const double degPerRow = 180.0 / static_cast<double>(b.mapH);
    const double thetaMaxDeg[2] = {rad2deg(static_cast<double>(b.thetaMaxRad[0])),
                                   rad2deg(static_cast<double>(b.thetaMaxRad[1]))};
    const float flat = static_cast<float>(P.flatLog2PerDeg);

    // ---- 1. per pixel: logs, luma, theta, validity ----------------------------------
    // lg: log2 RGB per lens (lens-major), lum: log2 BT.2020 luma per lens.
    std::vector<float> lg(2u * N * 3u, 0.0f);
    std::vector<float> lum(2u * N, 0.0f);
    std::vector<float> thDeg(2u * N, 0.0f);
    std::vector<std::uint8_t> valid(N, 0);
    OSV_TRY(forRange(pool, H, 4, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            for (std::size_t c = 0; c < W; ++c) {
                const std::size_t i = r * W + c;
                bool ok = true;
                for (int lens = 0; lens < 2; ++lens) {
                    const float* px = b.rgba[lens].data() + i * 4u;
                    const std::size_t li = static_cast<std::size_t>(lens) * N + i;
                    for (int ch = 0; ch < 3; ++ch) {
                        ok = ok && std::isfinite(px[ch]);
                        lg[li * 3u + static_cast<std::size_t>(ch)] =
                            static_cast<float>(log2Floor(static_cast<double>(px[ch])));
                    }
                    lum[li] = static_cast<float>(log2Floor(luma2020(px)));
                    const float th = b.thetaRad[lens][i];
                    thDeg[li] = static_cast<float>(rad2deg(static_cast<double>(th)));
                    ok = ok && std::isfinite(th) && std::isfinite(px[3]) &&
                         static_cast<double>(px[3]) >= P.trustAlpha;
                }
                valid[i] = ok ? 1u : 0u;
            }
        }
    }));

    // ---- 2. gradients, stops per degree ------------------------------------------------
    const int kx = std::max(1, static_cast<int>(std::lround(P.gradStepDeg / degPerCol)));
    const int ky = std::max(1, static_cast<int>(std::lround(P.gradStepDeg / degPerRow)));
    std::vector<float> gLon(2u * N, 0.0f);
    std::vector<float> gLat(2u * N, 0.0f);
    OSV_TRY(forRange(pool, H, 4, [&](std::size_t r0, std::size_t r1) {
        const int Wi = static_cast<int>(W);
        const int Hi = static_cast<int>(H);
        for (std::size_t r = r0; r < r1; ++r) {
            const int ru = std::max(static_cast<int>(r) - ky, 0);
            const int rd = std::min(static_cast<int>(r) + ky, Hi - 1);
            for (int c = 0; c < Wi; ++c) {
                const std::size_t i = r * W + static_cast<std::size_t>(c);
                const std::size_t cl = r * W + static_cast<std::size_t>(wrapIndex(c - kx, Wi));
                const std::size_t cr = r * W + static_cast<std::size_t>(wrapIndex(c + kx, Wi));
                for (int lens = 0; lens < 2; ++lens) {
                    const float* L = lum.data() + static_cast<std::size_t>(lens) * N;
                    gLon[static_cast<std::size_t>(lens) * N + i] =
                        std::fabs(L[cr] - L[cl]) / static_cast<float>(2.0 * kx * degPerCol);
                    gLat[static_cast<std::size_t>(lens) * N + i] =
                        rd > ru ? std::fabs(L[static_cast<std::size_t>(rd) * W + static_cast<std::size_t>(c)] -
                                            L[static_cast<std::size_t>(ru) * W + static_cast<std::size_t>(c)]) /
                                      static_cast<float>(static_cast<double>(rd - ru) * degPerRow)
                                : std::numeric_limits<float>::infinity();
                }
            }
        }
    }));

    // ---- 3. usable rim per lens, per longitude block ---------------------------------
    // rim_experiments.py:rim_limits(): per block, the median log2(me / other)
    // (green) over FLAT co-valid pixels in theta bins; the rim is the first
    // bin past the core whose median departs rimDropStops from the core's.
    const std::size_t block = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(P.rimBlockDeg / degPerCol)));
    const std::size_t nBlocks = (W + block - 1) / block;
    std::vector<double> rimRaw[2] = {std::vector<double>(W, std::numeric_limits<double>::quiet_NaN()),
                                     std::vector<double>(W, std::numeric_limits<double>::quiet_NaN())};
    OSV_TRY(forRange(pool, nBlocks, 1, [&](std::size_t k0, std::size_t k1) {
        std::vector<std::pair<double, double>> entries;  // (theta deg, log ratio)
        std::vector<double> sample;
        for (std::size_t k = k0; k < k1; ++k) {
            const std::size_t c0 = k * block;
            const std::size_t c1 = std::min(W, c0 + block);
            for (int me = 0; me < 2; ++me) {
                const int other = 1 - me;
                const std::size_t meOff = static_cast<std::size_t>(me) * N;
                const std::size_t otOff = static_cast<std::size_t>(other) * N;
                entries.clear();
                for (std::size_t r = 0; r < H; ++r) {
                    for (std::size_t c = c0; c < c1; ++c) {
                        const std::size_t i = r * W + c;
                        // Co-valid, flat along longitude in BOTH lenses and
                        // along latitude in the OTHER lens only: the tested
                        // lens's own radial fall-off is the signal.
                        if (!valid[i] || !(gLon[i] < flat) || !(gLon[N + i] < flat) || !(gLat[otOff + i] < flat)) {
                            continue;
                        }
                        const double ratio = static_cast<double>(lg[(meOff + i) * 3u + 1u]) -
                                             static_cast<double>(lg[(otOff + i) * 3u + 1u]);
                        entries.emplace_back(static_cast<double>(thDeg[meOff + i]), ratio);
                    }
                }
                std::sort(entries.begin(), entries.end());
                // The lens's own core ratio.
                const auto lower = [&entries](double theta) {
                    return std::lower_bound(entries.begin(), entries.end(), std::make_pair(theta, -1e300));
                };
                sample.clear();
                for (auto it = lower(P.rimCoreLoDeg); it != entries.end() && it->first < P.rimCoreHiDeg; ++it) {
                    sample.push_back(it->second);
                }
                if (sample.size() < P.rimMinCore) {
                    continue;  // not measured: too few flat pixels (textured ground, occlusion)
                }
                const double ref = medianInPlace(sample);
                double found = thetaMaxDeg[me];
                for (double lo = P.rimCoreHiDeg; lo < thetaMaxDeg[me]; lo += P.rimBinDeg) {
                    sample.clear();
                    for (auto it = lower(lo); it != entries.end() && it->first < lo + P.rimBinDeg; ++it) {
                        sample.push_back(it->second);
                    }
                    if (sample.size() < P.rimMinBin) {
                        continue;
                    }
                    if (std::fabs(medianInPlace(sample) - ref) > P.rimDropStops) {
                        found = lo;
                        break;
                    }
                }
                for (std::size_t c = c0; c < c1; ++c) {
                    rimRaw[me][c] = found;
                }
            }
        }
    }));
    const std::vector<double> rimBand[2] = {finishRim(rimRaw[0], degPerCol, P, thetaMaxDeg[0]),
                                            finishRim(rimRaw[1], degPerCol, P, thetaMaxDeg[1])};

    // ---- 4. trusted pixels and their weights -------------------------------------------
    // Trusted: co-valid AND inside both usable rims by the margin (unless the
    // guard is switched off to reproduce the research's negative result).
    // The GAIN weight is soft in texture: 1 / (1 + (g / flat)^2).
    std::vector<float> wTex(N, 0.0f);
    std::vector<std::uint8_t> trusted(N, 0);
    std::vector<float> lr(N * 3u, 0.0f);
    OSV_TRY(forRange(pool, H, 4, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            for (std::size_t c = 0; c < W; ++c) {
                const std::size_t i = r * W + c;
                if (!valid[i]) {
                    continue;
                }
                if (P.trustMask) {
                    const bool inside0 = static_cast<double>(thDeg[i]) < rimBand[0][c] - P.trustMarginDeg;
                    const bool inside1 = static_cast<double>(thDeg[N + i]) < rimBand[1][c] - P.trustMarginDeg;
                    if (!inside0 || !inside1) {
                        continue;
                    }
                }
                const float g = std::max(std::max(gLon[i], gLon[N + i]), std::max(gLat[i], gLat[N + i]));
                const float t = std::isfinite(g) ? g / flat : 1e6f;
                trusted[i] = 1u;
                wTex[i] = 1.0f / (1.0f + t * t);
                // log2(master / slave) per channel: lens 1 minus lens 0.
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    lr[i * 3u + ch] = lg[(N + i) * 3u + ch] - lg[i * 3u + ch];
                }
            }
        }
    }));
    std::uint64_t trustedCount = 0;
    for (const std::uint8_t t : trusted) {
        trustedCount += t;
    }
    if (static_cast<double>(trustedCount) < P.minTrustedFraction * static_cast<double>(N) || trustedCount < 64) {
        return Error{ErrorCode::Unsupported, "photoSeamFromBands: only " + std::to_string(trustedCount) + " of " +
                                                 std::to_string(N) + " band pixels are trusted"};
    }

    // ---- 5. the gain grid: normalised convolution sampled at the cells -----------------
    // Rows span the overlap [90 - thetaMax1, thetaMax0 - 90] (lens 1, the
    // master, looks at +lat); beyond it the kernel clamps and decays.
    const double latMinDeg = 90.0 - thetaMaxDeg[1];
    const double latMaxDeg = thetaMaxDeg[0] - 90.0;
    if (!(latMaxDeg - latMinDeg > 1e-3)) {
        return Error{ErrorCode::InvalidArgument, "photoSeamFromBands: the lenses do not overlap"};
    }
    const std::size_t gw = P.gridW;
    const std::size_t gh = P.gridH;
    std::vector<double> gridLat(gh);
    for (std::size_t j = 0; j < gh; ++j) {
        gridLat[j] = latMinDeg + (latMaxDeg - latMinDeg) * static_cast<double>(j) / static_cast<double>(gh - 1);
    }
    // Pass 1 (latitude), per band column: gh x W partial sums.
    const double sigRows = P.sigmaLatDeg / degPerRow;
    const double radRows = 3.0 * sigRows;
    std::vector<double> numL(gh * W * 3u, 0.0), denL(gh * W, 0.0), geoL(gh * W, 0.0);
    std::vector<double> massLat(gh, 0.0);
    std::vector<double> rowPos(gh, 0.0);
    for (std::size_t j = 0; j < gh; ++j) {
        // Continuous band row of grid row j (row centres at integer + 0.5).
        rowPos[j] = (90.0 - gridLat[j]) / degPerRow - static_cast<double>(b.rowOffset) - 0.5;
        for (int r = static_cast<int>(std::ceil(rowPos[j] - radRows)); r <= static_cast<int>(std::floor(rowPos[j] + radRows));
             ++r) {
            const double d = (static_cast<double>(r) - rowPos[j]) / sigRows;
            massLat[j] += std::exp(-0.5 * d * d);  // over ALL rows: outside the band counts as unsupported
        }
    }
    OSV_TRY(forRange(pool, W, 16, [&](std::size_t cBegin, std::size_t cEnd) {
        for (std::size_t c = cBegin; c < cEnd; ++c) {
            for (std::size_t j = 0; j < gh; ++j) {
                const int rA = std::max(0, static_cast<int>(std::ceil(rowPos[j] - radRows)));
                const int rB = std::min(static_cast<int>(H) - 1, static_cast<int>(std::floor(rowPos[j] + radRows)));
                double num[3] = {0.0, 0.0, 0.0};
                double den = 0.0;
                double geo = 0.0;
                for (int r = rA; r <= rB; ++r) {
                    const std::size_t i = static_cast<std::size_t>(r) * W + c;
                    if (!trusted[i]) {
                        continue;
                    }
                    const double d = (static_cast<double>(r) - rowPos[j]) / sigRows;
                    const double k = std::exp(-0.5 * d * d);
                    const double kw = k * static_cast<double>(wTex[i]);
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        num[ch] += kw * static_cast<double>(lr[i * 3u + ch]);
                    }
                    den += kw;
                    geo += k;
                }
                const std::size_t o = j * W + c;
                numL[o * 3u + 0u] = num[0];
                numL[o * 3u + 1u] = num[1];
                numL[o * 3u + 2u] = num[2];
                denL[o] = den;
                geoL[o] = geo;
            }
        }
    }));
    // Pass 2 (longitude, periodic), per grid row: the cell values.
    const double sigCols = P.sigmaLonDeg / degPerCol;
    const double radCols = 3.0 * sigCols;
    std::vector<double> D(gh * gw * 3u, 0.0);
    std::vector<std::uint8_t> have(gh * gw, 0);
    OSV_TRY(forRange(pool, gh, 1, [&](std::size_t j0, std::size_t j1) {
        const int Wi = static_cast<int>(W);
        for (std::size_t j = j0; j < j1; ++j) {
            for (std::size_t g = 0; g < gw; ++g) {
                // Grid column g sits at lon = -180 + g 360 / gw (the kernel's
                // convention); its continuous band column:
                const double lonG = -180.0 + 360.0 * static_cast<double>(g) / static_cast<double>(gw);
                const double fc = (lonG + 180.0) / degPerCol - 0.5;
                double num[3] = {0.0, 0.0, 0.0};
                double den = 0.0;
                double geo = 0.0;
                double mass = 0.0;
                for (int c = static_cast<int>(std::ceil(fc - radCols)); c <= static_cast<int>(std::floor(fc + radCols));
                     ++c) {
                    const double d = (static_cast<double>(c) - fc) / sigCols;
                    const double k = std::exp(-0.5 * d * d);
                    const std::size_t o = j * W + static_cast<std::size_t>(wrapIndex(c, Wi));
                    num[0] += k * numL[o * 3u + 0u];
                    num[1] += k * numL[o * 3u + 1u];
                    num[2] += k * numL[o * 3u + 2u];
                    den += k * denL[o];
                    geo += k * geoL[o];
                    mass += k;
                }
                const std::size_t cell = j * gw + g;
                const double support = (massLat[j] > 0.0 && mass > 0.0) ? geo / (massLat[j] * mass) : 0.0;
                if (support >= P.minSupport && den > 1e-12) {
                    have[cell] = 1u;
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        D[cell * 3u + ch] = num[ch] / den;
                    }
                }
            }
        }
    }));

    // ---- 6. rows without support: clamp outside, interpolate inside ----------------------
    std::vector<std::uint8_t> colOk(gw, 0);
    for (std::size_t g = 0; g < gw; ++g) {
        int first = -1;
        int last = -1;
        for (std::size_t j = 0; j < gh; ++j) {
            if (have[j * gw + g]) {
                first = first < 0 ? static_cast<int>(j) : first;
                last = static_cast<int>(j);
            }
        }
        if (first < 0) {
            continue;  // filled along longitude below
        }
        colOk[g] = 1u;
        int prev = first;
        for (std::size_t j = 0; j < gh; ++j) {
            const std::size_t cell = j * gw + g;
            if (have[cell]) {
                prev = static_cast<int>(j);
                continue;
            }
            std::size_t src = 0;
            if (static_cast<int>(j) < first) {
                src = static_cast<std::size_t>(first) * gw + g;
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    D[cell * 3u + ch] = D[src * 3u + ch];
                }
            } else if (static_cast<int>(j) > last) {
                src = static_cast<std::size_t>(last) * gw + g;
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    D[cell * 3u + ch] = D[src * 3u + ch];
                }
            } else {
                // A gap between supported rows: linear between its ends.
                int next = static_cast<int>(j) + 1;
                while (next <= last && !have[static_cast<std::size_t>(next) * gw + g]) {
                    ++next;
                }
                const double t = static_cast<double>(static_cast<int>(j) - prev) / static_cast<double>(next - prev);
                const std::size_t a = static_cast<std::size_t>(prev) * gw + g;
                const std::size_t z = static_cast<std::size_t>(next) * gw + g;
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    D[cell * 3u + ch] = D[a * 3u + ch] + (D[z * 3u + ch] - D[a * 3u + ch]) * t;
                }
            }
        }
    }

    // ---- 7. columns without support: a wide longitude fill ---------------------------------
    std::size_t okColumns = 0;
    for (const std::uint8_t ok : colOk) {
        okColumns += ok;
    }
    if (okColumns == 0) {
        return Error{ErrorCode::Unsupported, "photoSeamFromBands: no longitude has trusted support"};
    }
    if (okColumns < gw) {
        const double gridDegPerCol = 360.0 / static_cast<double>(gw);
        const double sigFill = P.fillSigmaLonDeg / gridDegPerCol;
        const int radFill = std::max(1, static_cast<int>(std::ceil(3.0 * sigFill)));
        // Clip-wide fallback per channel: the median of the supported cells.
        double fallback[3] = {0.0, 0.0, 0.0};
        for (std::size_t ch = 0; ch < 3; ++ch) {
            std::vector<double> v;
            for (std::size_t cell = 0; cell < gh * gw; ++cell) {
                if (colOk[cell % gw]) {
                    v.push_back(D[cell * 3u + ch]);
                }
            }
            fallback[ch] = v.empty() ? 0.0 : medianInPlace(v);
        }
        const std::vector<double> src = D;
        for (std::size_t g = 0; g < gw; ++g) {
            if (colOk[g]) {
                continue;
            }
            for (std::size_t j = 0; j < gh; ++j) {
                double num[3] = {0.0, 0.0, 0.0};
                double den = 0.0;
                for (int d = -radFill; d <= radFill; ++d) {
                    const std::size_t gg = static_cast<std::size_t>(wrapIndex(static_cast<int>(g) + d, static_cast<int>(gw)));
                    if (!colOk[gg]) {
                        continue;
                    }
                    const double k = std::exp(-0.5 * (static_cast<double>(d) * d) / (sigFill * sigFill));
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        num[ch] += k * src[(j * gw + gg) * 3u + ch];
                    }
                    den += k;
                }
                for (std::size_t ch = 0; ch < 3; ++ch) {
                    D[(j * gw + g) * 3u + ch] = den > 1e-3 ? num[ch] / den : fallback[ch];
                }
            }
        }
    }

    // ---- 8. the field -----------------------------------------------------------------------
    PhotoSeamField field;
    field.w = static_cast<std::uint32_t>(gw);
    field.h = static_cast<std::uint32_t>(gh);
    field.latMinRad = static_cast<float>(deg2rad(latMinDeg));
    field.latMaxRad = static_cast<float>(deg2rad(latMaxDeg));
    field.thetaMaxRad[0] = b.thetaMaxRad[0];
    field.thetaMaxRad[1] = b.thetaMaxRad[1];
    field.gain.resize(gh * gw * 3u);
    for (std::size_t i = 0; i < field.gain.size(); ++i) {
        field.gain[i] = static_cast<float>(std::clamp(D[i], -P.maxAbsLog2Gain, P.maxAbsLog2Gain));
    }
    // The rim at the grid columns: the finished band rim, linear between band
    // columns; the RAW measurement at the nearest band column for the clip's
    // accumulator (NaN where that column was not measured).
    field.rim.resize(gw * 2u);
    field.rimMeasured.resize(gw * 2u);
    for (std::size_t g = 0; g < gw; ++g) {
        const double lonG = -180.0 + 360.0 * static_cast<double>(g) / static_cast<double>(gw);
        const double fc = (lonG + 180.0) / degPerCol - 0.5;
        const double flc = std::floor(fc);
        const double t = fc - flc;
        const std::size_t ca = static_cast<std::size_t>(wrapIndex(static_cast<int>(flc), static_cast<int>(W)));
        const std::size_t cb = static_cast<std::size_t>(wrapIndex(static_cast<int>(flc) + 1, static_cast<int>(W)));
        const std::size_t cn = static_cast<std::size_t>(wrapIndex(static_cast<int>(std::lround(fc)), static_cast<int>(W)));
        for (int lens = 0; lens < 2; ++lens) {
            const double rimDeg = rimBand[lens][ca] + (rimBand[lens][cb] - rimBand[lens][ca]) * t;
            field.rim[g * 2u + static_cast<std::size_t>(lens)] = static_cast<float>(deg2rad(rimDeg));
            const double raw = rimRaw[lens][cn];
            field.rimMeasured[g * 2u + static_cast<std::size_t>(lens)] =
                std::isfinite(raw) ? static_cast<float>(deg2rad(raw)) : std::numeric_limits<float>::quiet_NaN();
        }
    }
    // ---- diagnostics ------------------------------------------------------------------------
    field.trustedPixels = trustedCount;
    field.bandPixels = N;
    for (std::size_t ch = 0; ch < 3; ++ch) {
        std::vector<double> v(gh * gw);
        for (std::size_t cell = 0; cell < gh * gw; ++cell) {
            v[cell] = static_cast<double>(field.gain[cell * 3u + ch]);
        }
        field.medianLog2Gain[ch] = medianInPlace(v);
    }
    for (int lens = 0; lens < 2; ++lens) {
        std::vector<double> v(gw);
        for (std::size_t g = 0; g < gw; ++g) {
            v[g] = rad2deg(static_cast<double>(field.rim[g * 2u + static_cast<std::size_t>(lens)]));
        }
        field.rimMedianDeg[lens] = medianInPlace(v);
    }
    field.statsMs = msSince(t0);
    if (!field.valid()) {
        return Error{ErrorCode::Internal, "photoSeamFromBands: the field came out invalid"};
    }
    return field;
}

Result<PhotoSeamField> measurePhotoSeam(const geom::LensRig& rig, const video::FramePair& frames,
                                        const geom::BlendParams& blend, const PhotoSeamParams& params,
                                        ThreadPool& pool) {
    const auto t0 = PhotoClock::now();
    OSV_TRY_ASSIGN(RgbLensBands bands, renderPhotoBands(rig, frames, blend, params, pool));
    const double bandMs = msSince(t0);
    OSV_TRY_ASSIGN(PhotoSeamField field, photoSeamFromBands(bands, params, &pool));
    field.bandMs = bandMs;
    return field;
}

Result<PhotoSeamField> blendPhotoSeamFields(const PhotoSeamField& from, const PhotoSeamField& to, double t) {
    if (!std::isfinite(t)) {
        return Error{ErrorCode::InvalidArgument, "blendPhotoSeamFields: non-finite weight"};
    }
    const bool sameLayout = from.w == to.w && from.h == to.h && from.gain.size() == to.gain.size() &&
                            from.rim.size() == to.rim.size() &&
                            std::fabs(from.latMinRad - to.latMinRad) <= 1e-6f &&
                            std::fabs(from.latMaxRad - to.latMaxRad) <= 1e-6f;
    if (!sameLayout || !from.valid() || !to.valid()) {
        return Error{ErrorCode::InvalidArgument, "blendPhotoSeamFields: the fields do not share one layout"};
    }
    // Exact endpoints: from + (to - from) * 1 is not always `to` in floats.
    if (t <= 0.0) {
        return from;
    }
    if (t >= 1.0) {
        return to;
    }
    PhotoSeamField out = to;  // diagnostics and the raw rim come from `to`
    const float tf = static_cast<float>(t);
    for (std::size_t i = 0; i < out.gain.size(); ++i) {
        out.gain[i] = from.gain[i] + (to.gain[i] - from.gain[i]) * tf;
    }
    for (std::size_t i = 0; i < out.rim.size(); ++i) {
        out.rim[i] = from.rim[i] + (to.rim[i] - from.rim[i]) * tf;
    }
    return out;
}

// ===========================================================================
//  Stage 2: time
// ===========================================================================

void PhotoRimAccumulator::add(const PhotoSeamField& bucket) {
    if (bucket.w < 2 || bucket.rimMeasured.size() != static_cast<std::size_t>(bucket.w) * 2u) {
        return;  // nothing measurable in it
    }
    if (bucket.w != m_w) {
        m_w = bucket.w;
        m_buckets = 0;
        m_history.assign(static_cast<std::size_t>(m_w) * 2u, std::deque<float>{});
    }
    for (std::size_t k = 0; k < m_history.size(); ++k) {
        const float v = bucket.rimMeasured[k];
        if (!std::isfinite(v)) {
            continue;
        }
        std::deque<float>& h = m_history[k];
        h.push_back(v);
        while (h.size() > kHistory) {
            h.pop_front();
        }
    }
    ++m_buckets;
}

void PhotoRimAccumulator::apply(PhotoSeamField& field, const PhotoSeamParams& params) const {
    if (m_buckets == 0 || field.w != m_w || field.rim.size() != m_history.size()) {
        return;
    }
    const double degPerSample = 360.0 / static_cast<double>(m_w);
    for (int lens = 0; lens < 2; ++lens) {
        std::vector<double> raw(m_w, std::numeric_limits<double>::quiet_NaN());
        std::vector<double> v;
        for (std::uint32_t g = 0; g < m_w; ++g) {
            const std::deque<float>& h = m_history[static_cast<std::size_t>(g) * 2u + static_cast<std::size_t>(lens)];
            if (h.empty()) {
                continue;
            }
            v.assign(h.begin(), h.end());
            raw[g] = rad2deg(medianInPlace(v));
        }
        const std::vector<double> fin =
            finishRim(raw, degPerSample, params, rad2deg(static_cast<double>(field.thetaMaxRad[lens])));
        std::vector<double> med;
        med.reserve(m_w);
        for (std::uint32_t g = 0; g < m_w; ++g) {
            field.rim[static_cast<std::size_t>(g) * 2u + static_cast<std::size_t>(lens)] =
                static_cast<float>(deg2rad(fin[g]));
            med.push_back(fin[g]);
        }
        field.rimMedianDeg[lens] = medianInPlace(med);
    }
}

void PhotoRimAccumulator::clear() noexcept {
    m_w = 0;
    m_buckets = 0;
    m_history.clear();
}

void PhotoSeamHistory::store(std::uint32_t bucket, const std::shared_ptr<const PhotoSeamField>& measured,
                             const PhotoSeamParams& params) {
    if (!measured || !measured->valid()) {
        m_fields[bucket] = nullptr;  // a refusal: not measured again
        return;
    }
    m_rim.add(*measured);
    std::shared_ptr<const PhotoSeamField> stored = measured;
    // EMA against the previous bucket's STORED field, so noise from one
    // measurement is carried at temporalAlpha weight rather than in full.
    const double alpha = std::isfinite(params.temporalAlpha) ? std::clamp(params.temporalAlpha, 0.0, 1.0) : 1.0;
    if (bucket > 0 && alpha < 1.0) {
        if (const auto it = m_fields.find(bucket - 1); it != m_fields.end() && it->second) {
            auto ema = blendPhotoSeamFields(*it->second, *measured, alpha);
            if (ema.ok()) {
                stored = std::make_shared<const PhotoSeamField>(std::move(ema).value());
            }
        }
    }
    m_fields[bucket] = std::move(stored);
}

bool PhotoSeamHistory::measured(std::uint32_t bucket) const { return m_fields.find(bucket) != m_fields.end(); }

std::shared_ptr<const PhotoSeamField> PhotoSeamHistory::fieldFor(std::uint32_t frame,
                                                                 const PhotoSeamParams& params) const {
    const std::uint32_t bucket = parallaxBucket(frame);
    const auto own = m_fields.find(bucket);
    if (own == m_fields.end() || !own->second) {
        return nullptr;
    }
    PhotoSeamField out = *own->second;
    // Glide from the previous bucket's field inside this bucket - the
    // parallax grid's schedule, so every per-bucket correction moves together.
    if (bucket > 0) {
        if (const auto prev = m_fields.find(bucket - 1); prev != m_fields.end() && prev->second) {
            auto blended = blendPhotoSeamFields(*prev->second, *own->second, parallaxCrossfadeWeight(frame));
            if (blended.ok()) {
                out = std::move(blended).value();
            }
        }
    }
    m_rim.apply(out, params);
    return std::make_shared<const PhotoSeamField>(std::move(out));
}

std::shared_ptr<const PhotoSeamField> PhotoSeamHistory::nearest(std::uint32_t bucket, std::uint32_t maxBuckets,
                                                                const PhotoSeamParams& params) const {
    for (std::uint32_t d = 0; d <= maxBuckets; ++d) {
        for (const int sign : {-1, 1}) {
            if (d == 0 && sign > 0) {
                continue;
            }
            if (sign < 0 && bucket < d) {
                continue;
            }
            const std::uint32_t b = sign < 0 ? bucket - d : bucket + d;
            if (const auto it = m_fields.find(b); it != m_fields.end() && it->second) {
                PhotoSeamField out = *it->second;
                m_rim.apply(out, params);
                return std::make_shared<const PhotoSeamField>(std::move(out));
            }
        }
    }
    return nullptr;
}

void PhotoSeamHistory::trim(std::size_t limit, std::uint32_t keep) {
    while (m_fields.size() > limit && !m_fields.empty()) {
        auto victim = m_fields.begin();
        if (victim->first == keep) {
            victim = std::prev(m_fields.end());
            if (victim->first == keep) {
                break;
            }
        }
        m_fields.erase(victim);
    }
}

void PhotoSeamHistory::clear() {
    m_fields.clear();
    m_rim.clear();
}

// ===========================================================================
//  Stage 2: metrics trust mask
// ===========================================================================

std::vector<std::uint8_t> rimTrustMask(const MetricBands& bands, const geom::LensRig& rig,
                                       const PhotoSeamField& field, double marginDeg) {
    std::vector<std::uint8_t> mask = coValidTrustMask(bands);
    if (!field.valid() || mask.empty() || !std::isfinite(marginDeg)) {
        return std::vector<std::uint8_t>(mask.size(), 0);
    }
    for (std::uint32_t r = 0; r < bands.h; ++r) {
        for (std::uint32_t c = 0; c < bands.w; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * bands.w + c;
            if (!mask[i]) {
                continue;
            }
            const Vec3d d = polarDirection(c, r, bands.w, bands.rowOffset, bands.mapH);
            const double lon = std::atan2(d.x, d.z);
            bool inside = true;
            for (int lens = 0; lens < 2 && inside; ++lens) {
                inside = rad2deg(lensTheta(rig, lens, d)) < photoRimDegAt(field, lens, lon) - marginDeg;
            }
            mask[i] = inside ? 1u : 0u;
        }
    }
    return mask;
}

// ===========================================================================
//  Stage 2: the usable rim as a seam cost
// ===========================================================================

bool photoRimPenalty(const LensBands& bands, const geom::LensRig& rig, const PhotoSeamField& field,
                     std::vector<float>& penaltySlave, std::vector<float>& penaltyMaster, double rampDeg) noexcept {
    try {
        const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
        if (n == 0 || penaltySlave.size() != n || penaltyMaster.size() != n || !field.valid() ||
            !std::isfinite(rampDeg) || !(rampDeg > 0.0)) {
            return false;
        }
        std::vector<float>* maps[2] = {&penaltySlave, &penaltyMaster};
        for (std::uint32_t r = 0; r < bands.h; ++r) {
            for (std::uint32_t c = 0; c < bands.w; ++c) {
                double dir[3];
                if (!bandPixelDirection(bands, c, r, dir)) {
                    return false;
                }
                const Vec3d d{dir[0], dir[1], dir[2]};
                const double lon = std::atan2(d.x, d.z);
                for (int lens = 0; lens < 2; ++lens) {
                    // 0 until rampDeg below the usable rim, 1 at it and beyond.
                    const double theta = rad2deg(lensTheta(rig, lens, d));
                    const double rim = photoRimDegAt(field, lens, lon);
                    const double t = std::clamp((theta - (rim - rampDeg)) / rampDeg, 0.0, 1.0);
                    (*maps[lens])[static_cast<std::size_t>(r) * bands.w + c] = static_cast<float>(t);
                }
            }
        }
        return true;
    } catch (...) {
        return false;  // allocation failure: contribute nothing
    }
}

namespace {

/// The Rim hook's reference on this thread (PhotoRimPenaltyScope).
thread_local const PhotoRimPenaltyScope::Context* t_rimContext = nullptr;

/// The process-wide Rim hook: judges the carve by this thread's scope.
bool rimPenaltyHook(const LensBands& warped, std::vector<float>& penaltySlave, std::vector<float>& penaltyMaster,
                    void* /*user*/) noexcept {
    const PhotoRimPenaltyScope::Context* ctx = t_rimContext;
    if (!ctx || !ctx->rig || !ctx->field) {
        return false;
    }
    return photoRimPenalty(warped, *ctx->rig, *ctx->field, penaltySlave, penaltyMaster);
}

}  // namespace

void installPhotoRimPenaltyHook() noexcept {
    static std::once_flag once;
    try {
        std::call_once(once, [] {
            SeamPenaltyHook hook;
            hook.fn = &rimPenaltyHook;
            hook.user = nullptr;
            hook.weight = kPhotoRimPenaltyWeight;
            setSeamPenaltyHook(SeamPenaltySlot::Rim, hook);
        });
    } catch (...) {
        // call_once only rethrows what the callable throws, and it throws nothing.
    }
}

PhotoRimPenaltyScope::PhotoRimPenaltyScope(const geom::LensRig* rig,
                                           std::shared_ptr<const PhotoSeamField> field) noexcept
    : m_previous(t_rimContext) {
    if (rig && field && field->valid()) {
        m_context.rig = rig;
        m_context.field = std::move(field);
    }
    t_rimContext = &m_context;
}

PhotoRimPenaltyScope::~PhotoRimPenaltyScope() { t_rimContext = m_previous; }

}  // namespace osv::render
