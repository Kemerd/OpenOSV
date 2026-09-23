// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SeamTools.cpp - the carve-width mapping, the seam smoothing's low band on
// the host, and the Near / Far Offset grid.  [WP-SEAMTOOLS]  See SeamTools.h
// for the why and the sign convention; this file is the how.

#include "osv/render/SeamTools.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace osv::render {

namespace {

/// Clamp-to-[0,1] smoothstep, the kernel's curve.
[[nodiscard]] double smoothstep01(double t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// Largest low-band edge we believe (a 65536-pixel lens at factor 2).
constexpr int kMaxLowEdge = 32768;

/// Run body(first, last) over [0, n) on the pool when there is one, inline
/// otherwise (or when the pool refuses).  Bodies write disjoint ranges, so
/// the result is identical either way.
template <class Body> void forRange(ThreadPool* pool, std::size_t n, const Body& body) {
    if (n == 0) {
        return;
    }
    if (pool) {
        const Status st = pool->parallelFor(0, n, 1, [&](std::size_t b, std::size_t e) { body(b, e); });
        if (st.ok()) {
            return;
        }
        log::debug("seam low band: thread pool refused a job ({}); running inline", st.error().message);
    }
    body(0, n);
}

/// Pixels per radian of lens `L` at the geometric seam (theta = 90 deg):
/// the focal length times the slope of its Kannala-Brandt polynomial there,
/// by a central difference in double.  0 for a lens without a usable focal
/// length or a non-finite polynomial.
[[nodiscard]] double pixelsPerRadianAtSeam(const OsvLens& L) noexcept {
    const double f = 0.5 * (static_cast<double>(L.fx) + static_cast<double>(L.fy));
    if (!(f > 0.0) || !std::isfinite(f)) {
        return 0.0;
    }
    const auto thetaD = [&L](double t) {
        const double t2 = t * t;
        double poly = 1.0;
        double pw = t2;
        for (int k = 0; k < 5; ++k) {
            poly += static_cast<double>(L.k[k]) * pw;
            pw *= t2;
        }
        return t * poly;
    };
    constexpr double kEps = 1e-3;
    const double slope = (thetaD(osv::kHalfPi + kEps) - thetaD(osv::kHalfPi - kEps)) / (2.0 * kEps);
    if (!std::isfinite(slope) || !(slope > 0.0)) {
        return 0.0;
    }
    return f * slope;
}

/// Longitude-ring linear interpolation of a per-column weight sampled at the
/// column centres (lon = -pi + (c + 0.5) 2pi / n), at longitude `lon`.
[[nodiscard]] double ringSample(const std::vector<float>& v, double lon) noexcept {
    const int n = static_cast<int>(v.size());
    if (n == 0 || !std::isfinite(lon)) {
        return 0.0;
    }
    const double fx = (lon + osv::kPi) / osv::kTwoPi * static_cast<double>(n) - 0.5;
    const double fl = std::floor(fx);
    const double t = fx - fl;
    int c0 = static_cast<int>(std::clamp(fl, -1.0e9, 1.0e9));
    c0 = ((c0 % n) + n) % n;
    const int c1 = (c0 + 1) % n;
    return static_cast<double>(v[static_cast<std::size_t>(c0)]) * (1.0 - t) +
           static_cast<double>(v[static_cast<std::size_t>(c1)]) * t;
}

}  // namespace

// ===========================================================================
//  The controls
// ===========================================================================
bool SeamTools::smoothingOn() const noexcept { return std::isfinite(smoothingDeg) && smoothingDeg > 0.0; }

bool SeamTools::offsetOn() const noexcept {
    const bool nearOn = std::isfinite(nearOffsetDeg) && nearOffsetDeg != 0.0;
    const bool farOn = std::isfinite(farOffsetDeg) && farOffsetDeg != 0.0;
    return nearOn || farOn;
}

void applySeamBlendWidths(const SeamTools& tools, SeamCarveParams& params) noexcept {
    // A non-finite width keeps the parameter's default: garbage never widens
    // or narrows the feather.
    if (std::isfinite(tools.seamBlendDeg)) {
        params.wideHalfWidthDeg = std::clamp(tools.seamBlendDeg, kMinSeamBlendDeg, kMaxSeamBlendDeg);
    }
    if (std::isfinite(tools.parallaxBlendDeg)) {
        params.narrowHalfWidthDeg = std::clamp(tools.parallaxBlendDeg, 0.0, kMaxParallaxBlendDeg);
    }
    // Where the lenses disagree the feather is never wider than where they
    // agree: the carve's own rule, applied here so the pair never fails it.
    params.narrowHalfWidthDeg = std::min(params.narrowHalfWidthDeg, params.wideHalfWidthDeg);
}

// ===========================================================================
//  Seam Smoothing: parameters
// ===========================================================================
void fillSeamSmoothParams(OsvRenderParams& p, double halfWidthDeg, double sigmaDeg) noexcept {
    // Off first: every early return below leaves the block exactly as a
    // render without smoothing has it (all zero).
    p.seamSmoothEnabled = 0;
    p.seamLowFactor = 0;
    p.seamLowW = 0;
    p.seamLowH = 0;
    p.seamLowRadius = 0;
    p.seamSmoothHalfRad = 0.0f;
    for (float& t : p.seamLowTaps) {
        t = 0.0f;
    }
    if (!std::isfinite(halfWidthDeg) || !(halfWidthDeg > 0.0)) {
        return;
    }
    const double width = std::min(halfWidthDeg, kMaxSeamSmoothingDeg);

    // ---- the low band's size: every enabled lens's frame, decimated -------
    const int F = kSeamLowFactor;
    int maxW = 0;
    int maxH = 0;
    double pxPerRad = 0.0;
    int lenses = 0;
    for (const OsvLens& L : p.lens) {
        if (!L.enabled) {
            continue;
        }
        const double ppr = pixelsPerRadianAtSeam(L);
        if (L.width <= 0 || L.height <= 0 || !(ppr > 0.0)) {
            return;  // a lens the low band cannot describe: no smoothing
        }
        maxW = std::max(maxW, L.width);
        maxH = std::max(maxH, L.height);
        pxPerRad += ppr;
        ++lenses;
    }
    if (lenses == 0) {
        return;
    }
    pxPerRad /= static_cast<double>(lenses);
    const int lowW = (maxW + F - 1) / F;
    const int lowH = (maxH + F - 1) / F;
    if (lowW <= 0 || lowH <= 0 || lowW > kMaxLowEdge || lowH > kMaxLowEdge) {
        return;
    }

    // ---- the blur, in low-band pixels -------------------------------------
    // The wanted low-pass (sigma in lens pixels) less what the pipeline blurs
    // anyway: the F x F box (variance (F^2 - 1) / 12) and the bilinear
    // lookup's tent of one low-band pixel (variance F^2 / 6, in lens pixels).
    const double sigmaWantDeg = (std::isfinite(sigmaDeg) && sigmaDeg >= 0.0) ? sigmaDeg
                                                                              : width * kSeamLowSigmaPerHalfWidth;
    const double sigmaPx = osv::deg2rad(sigmaWantDeg) * pxPerRad;
    const double ff = static_cast<double>(F) * static_cast<double>(F);
    const double residualVar = sigmaPx * sigmaPx - (ff - 1.0) / 12.0 - ff / 6.0;
    double sigmaLow = residualVar > 0.0 ? std::sqrt(residualVar) / static_cast<double>(F) : 0.0;
    // Three sigma must fit the tap table.
    sigmaLow = std::min(sigmaLow, static_cast<double>(OSV_SEAM_LOW_MAX_RADIUS) / 3.0);
    const int radius =
        sigmaLow > 0.05 ? std::min(OSV_SEAM_LOW_MAX_RADIUS, static_cast<int>(std::ceil(3.0 * sigmaLow))) : 0;

    // ---- commit -------------------------------------------------------------
    p.seamLowFactor = F;
    p.seamLowW = lowW;
    p.seamLowH = lowH;
    p.seamLowRadius = radius;
    p.seamSmoothHalfRad = static_cast<float>(osv::deg2rad(width));
    p.seamLowTaps[0] = 1.0f;
    for (int k = 1; k <= radius; ++k) {
        const double kk = static_cast<double>(k);
        p.seamLowTaps[k] = static_cast<float>(std::exp(-0.5 * kk * kk / (sigmaLow * sigmaLow)));
    }
    p.seamSmoothEnabled = 1;
}

std::size_t seamLowTableFloats(const OsvRenderParams& p) noexcept {
    if (!p.seamSmoothEnabled || p.seamLowW <= 0 || p.seamLowH <= 0 || p.seamLowW > kMaxLowEdge ||
        p.seamLowH > kMaxLowEdge) {
        return 0;
    }
    return 2u * static_cast<std::size_t>(p.seamLowW) * static_cast<std::size_t>(p.seamLowH) * 4u;
}

bool seamSmoothParamsValid(const OsvRenderParams& p) noexcept {
    if (!p.seamSmoothEnabled) {
        return true;
    }
    const int F = p.seamLowFactor;
    if (F < 2 || (F & 1) != 0 || F > 64 || seamLowTableFloats(p) == 0) {
        return false;
    }
    // The low band must cover every enabled lens's frame, or its lookup
    // would clamp a lens's rim onto the wrong texels.
    for (const OsvLens& L : p.lens) {
        if (!L.enabled) {
            continue;
        }
        if (L.width <= 0 || L.height <= 0 || (L.width + F - 1) / F > p.seamLowW ||
            (L.height + F - 1) / F > p.seamLowH) {
            return false;
        }
    }
    if (p.seamLowRadius < 0 || p.seamLowRadius > OSV_SEAM_LOW_MAX_RADIUS) {
        return false;
    }
    if (!(p.seamLowTaps[0] > 0.0f) || !std::isfinite(p.seamLowTaps[0])) {
        return false;
    }
    for (int k = 1; k <= p.seamLowRadius; ++k) {
        if (!(p.seamLowTaps[k] >= 0.0f) || !std::isfinite(p.seamLowTaps[k])) {
            return false;
        }
    }
    return std::isfinite(p.seamSmoothHalfRad) && p.seamSmoothHalfRad >= 0.0f;
}

bool RenderJob::seamSmoothFieldsValid(const OsvRenderParams& p) noexcept { return seamSmoothParamsValid(p); }

// ===========================================================================
//  Seam Smoothing: the host build
// ===========================================================================
Status buildSeamLowBand(const OsvRenderParams& p, const OsvPlane planes[2], std::vector<float>& out,
                        std::vector<float>& scratch, ThreadPool* pool) {
    if (!p.seamSmoothEnabled || !seamSmoothParamsValid(p)) {
        return failStatus(ErrorCode::InvalidArgument, "seam low band: smoothing is off or its fields are malformed");
    }
    if (!planes) {
        return failStatus(ErrorCode::InvalidArgument, "seam low band: no planes");
    }
    for (int i = 0; i < 2; ++i) {
        const OsvPlane& P = planes[i];
        if (p.lens[i].enabled && (!P.y || !P.u || !P.v || P.w <= 0 || P.h <= 0 || P.cw <= 0 || P.ch <= 0)) {
            return failStatus(ErrorCode::InvalidArgument,
                              "seam low band: lens " + std::to_string(i) + " has an empty plane");
        }
    }
    const std::size_t n = seamLowTableFloats(p);
    const std::size_t W = static_cast<std::size_t>(p.seamLowW);
    const std::size_t H = static_cast<std::size_t>(p.seamLowH);
    // Every texel is written by each stage, so no fill is needed.
    out.resize(n);
    scratch.resize(n);
    // Rows of both lenses in one index space: row r is lens r / H, row r % H.
    const std::size_t rows = 2u * H;

    // ---- 1. decimate the fisheyes into `out` ----------------------------------
    forRange(pool, rows, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const int lens = static_cast<int>(r / H);
            const int ly = static_cast<int>(r % H);
            float* row = out.data() + r * W * 4u;
            for (std::size_t x = 0; x < W; ++x) {
                osvSeamLowDecimatePixel(&p, &planes[lens], lens, static_cast<int>(x), ly, row + x * 4u);
            }
        }
    });
    // ---- 2. blur along x: out -> scratch ------------------------------------------
    forRange(pool, rows, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const int lens = static_cast<int>(r / H);
            const int ly = static_cast<int>(r % H);
            float* row = scratch.data() + r * W * 4u;
            for (std::size_t x = 0; x < W; ++x) {
                osvSeamLowBlurPixel(&p, out.data(), lens, static_cast<int>(x), ly, 1, row + x * 4u);
            }
        }
    });
    // ---- 3. blur along y: scratch -> out -------------------------------------------
    forRange(pool, rows, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const int lens = static_cast<int>(r / H);
            const int ly = static_cast<int>(r % H);
            float* row = out.data() + r * W * 4u;
            for (std::size_t x = 0; x < W; ++x) {
                osvSeamLowBlurPixel(&p, scratch.data(), lens, static_cast<int>(x), ly, 0, row + x * 4u);
            }
        }
    });
    return okStatus();
}

// ===========================================================================
//  Near / Far Offset
// ===========================================================================
Result<ParallaxWarpGrid> seamOffsetGrid(const ParallaxWarpGrid* base, const BlendSeam& seam, double nearDeg,
                                        double farDeg) {
    if (!std::isfinite(nearDeg) || !std::isfinite(farDeg)) {
        return Error{ErrorCode::InvalidArgument, "seamOffsetGrid: non-finite offset"};
    }
    if (!seam.valid()) {
        return Error{ErrorCode::InvalidArgument, "seamOffsetGrid: the seam is empty or malformed"};
    }
    const double nearClamped = std::clamp(nearDeg, -kMaxSeamOffsetDeg, kMaxSeamOffsetDeg);
    const double farClamped = std::clamp(farDeg, -kMaxSeamOffsetDeg, kMaxSeamOffsetDeg);

    // ---- the grid to add to: the parallax grid, or a zero grid of its shape ----
    ParallaxWarpGrid grid;
    if (base && base->valid() && base->h > 1) {
        grid = *base;
    } else {
        // The default parallax geometry: gridRows across the +/- band, a
        // decay ring of decayRows at the same pitch on either side.
        const ParallaxWarpParams defaults;
        const std::uint32_t rows = std::max<std::uint32_t>(defaults.gridRows, 2u);
        const double core = osv::deg2rad(defaults.band.bandHalfDeg);
        const double pitch = 2.0 * core / static_cast<double>(rows - 1u);
        grid.w = std::max<std::uint32_t>(defaults.gridW, 1u);
        grid.h = rows + 2u * defaults.decayRows;
        grid.latMinRad = static_cast<float>(core + pitch * static_cast<double>(defaults.decayRows));
        grid.latMaxRad = static_cast<float>(-core - pitch * static_cast<double>(defaults.decayRows));
        grid.uv.assign(static_cast<std::size_t>(grid.w) * grid.h * 2u, 0.0f);
    }

    // ---- the latitude profile: full across the core, smoothstep to 0 at the edge
    const double edge = std::max(std::fabs(static_cast<double>(grid.latMinRad)),
                                 std::fabs(static_cast<double>(grid.latMaxRad)));
    const double core = std::min(osv::deg2rad(kSeamOffsetCoreDeg), 0.66 * edge);
    std::vector<double> profile(grid.h, 0.0);
    for (std::uint32_t gy = 0; gy < grid.h; ++gy) {
        const double lat = static_cast<double>(grid.latMinRad) +
                           (static_cast<double>(grid.latMaxRad) - static_cast<double>(grid.latMinRad)) *
                               static_cast<double>(gy) / static_cast<double>(grid.h - 1u);
        const double a = std::fabs(lat);
        profile[gy] = (a <= core || !(edge > core)) ? 1.0 : 1.0 - smoothstep01((a - core) / (edge - core));
    }
    // The outermost rows must be exactly zero: the kernel returns zero past
    // them, and any residue would be a step at the grid's edge.
    profile.front() = 0.0;
    profile.back() = 0.0;

    // ---- per column: near / far by the carve's disagreement ------------------
    for (std::uint32_t gx = 0; gx < grid.w; ++gx) {
        // Grid column gx sits at lon = -pi + gx 2pi / w (osvWarpSample).
        const double lon = -osv::kPi + static_cast<double>(gx) * osv::kTwoPi / static_cast<double>(grid.w);
        const double w = seam.nearWeight.empty() ? 0.0 : std::clamp(ringSample(seam.nearWeight, lon), 0.0, 1.0);
        const double offsetRad = osv::deg2rad(nearClamped * w + farClamped * (1.0 - w));
        // A positive offset turns the master's content toward +longitude
        // (see SIGN in SeamTools.h): sampling at lon - d shows the content
        // that sits at lon - d at lon, i.e. moves it by +d.  So the master's
        // sampling displacement is MINUS half the relative shift, and the
        // kernel gives the slave the negation.
        const double masterDLon = -0.5 * offsetRad;
        for (std::uint32_t gy = 0; gy < grid.h; ++gy) {
            float& u = grid.uv[(static_cast<std::size_t>(gy) * grid.w + gx) * 2u];
            u = static_cast<float>(static_cast<double>(u) + masterDLon * profile[gy]);
        }
    }
    return grid;
}

}  // namespace osv::render
