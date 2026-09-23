// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensShading.cpp - the per-lens shading correction: measurement from each
// lens's own sky, the temporal filter, and the kernel's separable block.
// The reasoning and the measurements are in include/osv/render/LensShading.h
// and docs/research/NEURAL_STITCHING.md, section 9.

#include "osv/render/LensShading.h"

#include "osv/core/Math.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamCarve.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>

namespace osv::render {

namespace {

// ===========================================================================
//  Constants and small helpers
// ===========================================================================

using ShadingClock = std::chrono::steady_clock;

/// Table geometry, from the kernel's parameter block.
constexpr int kKnots = OSV_SHADE_THETA_N;
constexpr int kSectors = OSV_SHADE_PHI_N;
constexpr int kRank = OSV_SHADE_RANK;

/// BT.2020 luma weights: the model's "luma units" (and the colour factors'
/// normalisation) are defined with them.
constexpr double kLumaWeight[3] = {0.2627, 0.6780, 0.0593};

/// Linear light below this is not trusted for a log fit (black, clipped
/// shadows, the image-circle edge).
constexpr float kLightFloor = 1e-5f;

/// A pixel's occlusion alpha at least this: the trusted-pixel rule the
/// photometric field and the exposure match use too.
constexpr float kTrustAlpha = 0.99f;

/// Colour factors are kept inside [1/kColourSpan, kColourSpan].
constexpr float kColourSpan = 5.0f;

/// Milliseconds since `t0`.
[[nodiscard]] double msSince(ShadingClock::time_point t0) noexcept {
    return std::chrono::duration<double, std::milli>(ShadingClock::now() - t0).count();
}

/// Median of `v`, reordering it; the mean of the two middle values for an
/// even count.  NaN for an empty vector.
[[nodiscard]] double medianInPlace(std::vector<float>& v) {
    if (v.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    const double hi = static_cast<double>(v[mid]);
    if ((v.size() % 2u) == 1u) {
        return hi;
    }
    const double lo = static_cast<double>(*std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid)));
    return 0.5 * (lo + hi);
}

/// True when every knob the measurement divides by or loops over is usable.
[[nodiscard]] bool paramsUsable(const LensShadingParams& p) noexcept {
    const auto pos = [](double v) { return std::isfinite(v) && v > 0.0; };
    const auto nonNeg = [](double v) { return std::isfinite(v) && v >= 0.0; };
    return p.band.equirectW >= 256 && p.band.equirectW <= 16384 && (p.band.equirectW % 2u) == 0 &&
           pos(p.band.bandHalfDeg) && p.band.bandHalfDeg <= 45.0 && std::isfinite(p.fitLoDeg) &&
           std::isfinite(p.anchorDeg) && p.fitLoDeg < p.anchorDeg && nonNeg(p.domainBelowThetaMaxDeg) &&
           p.minColumnPixels >= 8 && pos(p.maxColumnNoiseStops) && pos(p.maxColumnScaleStops) &&
           pos(p.maxAlongGradient) && pos(p.maxRadialGradient) && p.iterations >= 1 && p.iterations <= 16 &&
           pos(p.knotDeg) && p.knotDeg <= 5.0 && nonNeg(p.taperDeg) && p.minCellPixels >= 1 && p.minCellColumns >= 1 &&
           std::isfinite(p.minSectorCoverage) && p.minSectorCoverage >= 0.0 && p.minSectorCoverage <= 1.0 &&
           pos(p.maxRelativeAmount) && nonNeg(p.minPeakRelative);
}

/// Degrees of the centre of sector `s`.
[[nodiscard]] double sectorCentreDeg(int s) noexcept {
    return -180.0 + (static_cast<double>(s) + 0.5) * 360.0 / static_cast<double>(kSectors);
}

// ===========================================================================
//  Per-pixel azimuth around each lens axis, cached per band geometry
// ===========================================================================

/// Both lenses' azimuth (radians, atan2(y, x) in the lens frame) of every
/// pixel of one band geometry.  Immutable once built.
struct PhiTable {
    std::vector<double> key;                           ///< Both rotations, then w, h, rowOffset, mapH.
    std::shared_ptr<const std::vector<float>> phi[2];  ///< w * h each.
};

/// A couple of recently used tables, shared by every clip in the process.
struct PhiCache {
    std::mutex mutex;
    std::deque<std::shared_ptr<const PhiTable>> entries;  ///< Most recent first.
    static constexpr std::size_t kMax = 2;
};

PhiCache& phiCache() {
    static PhiCache* cache = new PhiCache();  // intentionally leaked: no teardown-order hazards
    return *cache;
}

/// The azimuth table of `rig` over the band geometry of `bands`.
Result<std::shared_ptr<const PhiTable>> phiTableFor(const geom::LensRig& rig, const RgbLensBands& bands,
                                                    ThreadPool* pool) {
    std::vector<double> key;
    key.reserve(22);
    for (std::size_t i = 0; i < 2; ++i) {
        key.insert(key.end(), std::begin(rig.bodyToLens[i].m), std::end(rig.bodyToLens[i].m));
    }
    key.insert(key.end(), {static_cast<double>(bands.w), static_cast<double>(bands.h),
                           static_cast<double>(bands.rowOffset), static_cast<double>(bands.mapH)});
    PhiCache& cache = phiCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (auto it = cache.entries.begin(); it != cache.entries.end(); ++it) {
            if ((*it)->key == key) {
                std::shared_ptr<const PhiTable> hit = *it;
                cache.entries.erase(it);
                cache.entries.push_front(hit);
                return hit;
            }
        }
    }
    // Not cached: compute outside the lock (a race builds it twice, the
    // results are identical and one wins the slot).  The band pixel's body
    // direction is the one every band analysis uses (bandPixelDirection).
    LensBands shell;
    shell.w = bands.w;
    shell.h = bands.h;
    shell.rowOffset = bands.rowOffset;
    shell.mapH = bands.mapH;
    const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
    auto p0 = std::make_shared<std::vector<float>>(n, 0.0f);
    auto p1 = std::make_shared<std::vector<float>>(n, 0.0f);
    const auto body = [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            for (std::uint32_t c = 0; c < bands.w; ++c) {
                double d[3] = {0.0, 0.0, 0.0};
                bandPixelDirection(shell, static_cast<double>(c), static_cast<double>(r), d);
                const Vec3d db{d[0], d[1], d[2]};
                const std::size_t i = r * bands.w + c;
                for (int lens = 0; lens < 2; ++lens) {
                    const Vec3d dl = rig.bodyToLens[static_cast<std::size_t>(lens)] * db;
                    (lens == 0 ? *p0 : *p1)[i] = static_cast<float>(std::atan2(dl.y, dl.x));
                }
            }
        }
    };
    if (pool) {
        OSV_TRY(pool->parallelFor(0, bands.h, 4, body));
    } else {
        body(0, bands.h);
    }
    auto table = std::make_shared<PhiTable>();
    table->key = std::move(key);
    table->phi[0] = std::move(p0);
    table->phi[1] = std::move(p1);
    std::shared_ptr<const PhiTable> done = std::move(table);
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries.push_front(done);
    while (cache.entries.size() > PhiCache::kMax) {
        cache.entries.pop_back();
    }
    return done;
}

/// Run `body` over [0, n) on `pool` when there is one, inline otherwise.
template <class Body> Status forRange(ThreadPool* pool, std::size_t n, std::size_t grain, const Body& body) {
    if (n == 0) {
        return okStatus();
    }
    if (pool) {
        return pool->parallelFor(0, n, grain, body);
    }
    body(std::size_t{0}, n);
    return okStatus();
}

// ===========================================================================
//  The per-column sky fit
// ===========================================================================

/// Weighted least-squares quadratic y = a + b x + c x^2 through (x, y, w).
/// False (coefficients untouched) for a singular system.
[[nodiscard]] bool fitQuadratic(const std::vector<double>& x, const std::vector<double>& y,
                                const std::vector<double>& w, double coef[3]) noexcept {
    // Normal equations, accumulated in double.
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, t0 = 0, t1 = 0, t2 = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double wi = w[i];
        const double xi = x[i];
        const double x2 = xi * xi;
        s0 += wi;
        s1 += wi * xi;
        s2 += wi * x2;
        s3 += wi * x2 * xi;
        s4 += wi * x2 * x2;
        t0 += wi * y[i];
        t1 += wi * xi * y[i];
        t2 += wi * x2 * y[i];
    }
    // Cramer's rule on the symmetric 3x3 [s0 s1 s2; s1 s2 s3; s2 s3 s4].
    const double det = s0 * (s2 * s4 - s3 * s3) - s1 * (s1 * s4 - s3 * s2) + s2 * (s1 * s3 - s2 * s2);
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) {
        return false;
    }
    const double a = (t0 * (s2 * s4 - s3 * s3) - s1 * (t1 * s4 - s3 * t2) + s2 * (t1 * s3 - s2 * t2)) / det;
    const double b = (s0 * (t1 * s4 - t2 * s3) - t0 * (s1 * s4 - s3 * s2) + s2 * (s1 * t2 - t1 * s2)) / det;
    const double c = (s0 * (s2 * t2 - s3 * t1) - s1 * (s1 * t2 - s2 * t1) + t0 * (s1 * s3 - s2 * s2)) / det;
    if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) {
        return false;
    }
    coef[0] = a;
    coef[1] = b;
    coef[2] = c;
    return true;
}

/// The current table's amount at knot position t (fractional, >= 0) and
/// sector position u (fractional, wrapping) - the kernel's interpolation.
[[nodiscard]] double tableAmount(const std::vector<float>& amount, double tKnot, double uSector) noexcept {
    if (!(tKnot > 0.0) || amount.size() != kLensShadingCells) {
        return 0.0;
    }
    const double tc = std::min(tKnot, static_cast<double>(kKnots - 1));
    int k0 = static_cast<int>(std::floor(tc));
    k0 = std::clamp(k0, 0, kKnots - 2);
    const double ft = tc - static_cast<double>(k0);
    const double fu = std::floor(uSector);
    const double wu = uSector - fu;
    int s0 = static_cast<int>(fu) % kSectors;
    s0 = s0 < 0 ? s0 + kSectors : s0;
    const int s1 = (s0 + 1) % kSectors;
    const auto at = [&](int k, int s) {
        return static_cast<double>(amount[static_cast<std::size_t>(k) * kSectors + static_cast<std::size_t>(s)]);
    };
    const double a = at(k0, s0) + (at(k0, s1) - at(k0, s0)) * wu;
    const double b = at(k0 + 1, s0) + (at(k0 + 1, s1) - at(k0 + 1, s0)) * wu;
    return a + (b - a) * ft;
}

/// Everything one lens's estimate reads and writes per band pixel.
struct LensScratch {
    std::vector<float> logY;         ///< log2 luma, NaN where unusable.
    std::vector<std::uint8_t> flat;  ///< 1 = valid and passes both gradient gates.
    std::vector<float> tKnot;        ///< Fractional knot position of the pixel's theta.
    std::vector<float> uSector;      ///< Fractional sector position of its azimuth.
    /// 5 per accumulation CANDIDATE (see candidates below): the light missing
    /// against the fitted sky (sky - image) in luma, R, G, B - what the
    /// correction must ADD - then the sky's luma level.
    std::vector<float> resid;
    std::vector<std::uint8_t> used;         ///< Per candidate: 1 = its residual is from this round's fit.
    std::vector<std::int32_t> cellIdx;      ///< Per pixel: its (knot, sector) cell, -1 = none.
    std::vector<std::int32_t> candPos;      ///< Per pixel: its candidate slot, -1 = not a candidate.
    std::vector<std::uint32_t> candidates;  ///< Pixel index of each candidate slot.
};

/// A tiny free list of scratch sets, so a bucket's estimate reuses the ~5 MB
/// of per-pixel buffers the previous one paged in instead of faulting fresh
/// pages every time (the photometric field keeps its scratch the same way).
struct LensScratchPool {
    std::mutex mutex;
    std::vector<std::unique_ptr<LensScratch>> free;
    static constexpr std::size_t kKeep = 2;
};

LensScratchPool& lensScratchPool() {
    static LensScratchPool* pool = new LensScratchPool();  // intentionally leaked (no teardown order)
    return *pool;
}

/// RAII: takes a scratch set from the pool (or makes one) and gives it back.
class LensScratchLease {
public:
    LensScratchLease() {
        LensScratchPool& p = lensScratchPool();
        {
            std::lock_guard<std::mutex> lock(p.mutex);
            if (!p.free.empty()) {
                m_scratch = std::move(p.free.back());
                p.free.pop_back();
            }
        }
        if (!m_scratch) {
            m_scratch = std::make_unique<LensScratch>();
        }
    }
    ~LensScratchLease() {
        try {
            LensScratchPool& p = lensScratchPool();
            std::lock_guard<std::mutex> lock(p.mutex);
            if (p.free.size() < LensScratchPool::kKeep) {
                p.free.push_back(std::move(m_scratch));
            }
        } catch (...) {
            // A failed push only means the set is freed instead of kept.
        }
    }
    LensScratchLease(const LensScratchLease&) = delete;
    LensScratchLease& operator=(const LensScratchLease&) = delete;
    [[nodiscard]] LensScratch& get() noexcept { return *m_scratch; }

private:
    std::unique_ptr<LensScratch> m_scratch;
};

/// One lens: the full back-fitting estimate.  Fills `out` (amount, sectors,
/// colour, diagnostics).
Status estimateLens(const RgbLensBands& b, const PhiTable& phiTab, int lens, const LensShadingParams& P,
                    ThreadPool* pool, LensShadingLens& out) {
    const std::size_t W = b.w;
    const std::size_t H = b.h;
    const std::size_t N = W * H;
    const float* rgba = b.rgba[lens].data();
    const std::vector<float>& theta = *b.thetaRad[lens];
    const std::vector<float>& phi = *phiTab.phi[lens];

    // ---- geometry of this lens's table ----------------------------------------
    const double thetaMaxDeg = rad2deg(static_cast<double>(b.thetaMaxRad[lens]));
    const double domHiDeg = thetaMaxDeg - P.domainBelowThetaMaxDeg;
    out = LensShadingLens{};
    out.amount.assign(kLensShadingCells, 0.0f);
    out.sectorMeasured.assign(kSectors, 0);
    // Knots inside the domain: 0 .. kDomHi (knot 0 is the anchor, always 0).
    const int kDomHi = std::min(kKnots - 1, static_cast<int>(std::floor((domHiDeg - P.anchorDeg) / P.knotDeg)));
    if (!std::isfinite(domHiDeg) || kDomHi < 4) {
        return okStatus();  // a lens whose domain holds no ring zone: nothing to correct
    }

    // ---- per-pixel inputs that never change between rounds ------------------
    LensScratchLease lease;
    LensScratch& S = lease.get();
    S.logY.assign(N, std::numeric_limits<float>::quiet_NaN());
    S.flat.assign(N, 0);
    S.tKnot.assign(N, -1.0f);
    S.uSector.assign(N, 0.0f);
    const double rowDeg = 180.0 / static_cast<double>(b.mapH);
    const double colDeg = 360.0 / static_cast<double>(W);
    const double fitLoDeg = P.fitLoDeg;
    OSV_TRY(forRange(pool, H, 8, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            for (std::size_t c = 0; c < W; ++c) {
                const std::size_t i = r * W + c;
                const float* px = rgba + i * 4u;
                const double th = rad2deg(static_cast<double>(theta[i]));
                bool ok = px[3] >= kTrustAlpha && std::isfinite(th) && th >= fitLoDeg - 1.0 && th <= domHiDeg + 1.0;
                for (int ch = 0; ch < 3 && ok; ++ch) {
                    ok = std::isfinite(px[ch]) && px[ch] > kLightFloor;
                }
                if (!ok) {
                    continue;
                }
                const double y = kLumaWeight[0] * px[0] + kLumaWeight[1] * px[1] + kLumaWeight[2] * px[2];
                S.logY[i] = static_cast<float>(std::log2(std::max(y, static_cast<double>(kLightFloor))));
                S.tKnot[i] = static_cast<float>((th - P.anchorDeg) / P.knotDeg);
                S.uSector[i] = static_cast<float>((static_cast<double>(phi[i]) + kPi) / kTwoPi * kSectors - 0.5);
            }
        }
    }));
    // Flat gates: along the ring (neighbouring columns, which wrap) and along
    // theta (neighbouring rows).  The ring itself changes by ~0.15 stop per
    // degree across theta and not at all along it, so it passes both; clouds,
    // horizons, the wing and every textured surface fail one or the other.
    OSV_TRY(forRange(pool, H, 8, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = std::max<std::size_t>(r0, 1); r < std::min(r1, H - 1); ++r) {
            const double lat = 90.0 - (static_cast<double>(b.rowOffset) + static_cast<double>(r) + 0.5) * rowDeg;
            const double along = 2.0 * colDeg * std::max(std::cos(deg2rad(lat)), 0.05);
            for (std::size_t c = 0; c < W; ++c) {
                const std::size_t i = r * W + c;
                if (!std::isfinite(S.logY[i])) {
                    continue;
                }
                const float l = S.logY[(c == 0 ? r * W + W - 1 : i - 1)];
                const float rr = S.logY[(c + 1 == W ? r * W : i + 1)];
                const float u = S.logY[i - W];
                const float d = S.logY[i + W];
                if (!std::isfinite(l) || !std::isfinite(rr) || !std::isfinite(u) || !std::isfinite(d)) {
                    continue;
                }
                const double gAlong = std::fabs(static_cast<double>(rr) - static_cast<double>(l)) / along;
                const double gRadial = std::fabs(static_cast<double>(d) - static_cast<double>(u)) / (2.0 * rowDeg);
                S.flat[i] = (gAlong <= P.maxAlongGradient && gRadial <= P.maxRadialGradient) ? 1u : 0u;
            }
        }
    }));

    // Each flat pixel's (knot, sector) cell, fixed for the whole estimate,
    // and the list of pixels that can ever feed a cell: the per-round
    // gathering then walks a third of the band instead of all of it.
    // The residuals live per candidate (candPos maps a pixel to its slot),
    // a third of the memory a per-pixel buffer would page in every bucket.
    std::vector<std::int32_t>& cellIdx = S.cellIdx;
    cellIdx.assign(N, -1);
    OSV_TRY(forRange(pool, H, 8, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t i = r0 * W; i < r1 * W; ++i) {
            if (!S.flat[i]) {
                continue;
            }
            const int k = static_cast<int>(std::lround(static_cast<double>(S.tKnot[i])));
            if (k < 0 || k > kDomHi) {
                continue;
            }
            int sct = static_cast<int>(std::floor(static_cast<double>(S.uSector[i]) + 0.5)) % kSectors;
            sct = sct < 0 ? sct + kSectors : sct;
            cellIdx[i] = k * kSectors + sct;
        }
    }));
    std::vector<std::uint32_t>& candidates = S.candidates;
    std::vector<std::int32_t>& candPos = S.candPos;
    candidates.clear();
    candPos.assign(N, -1);
    for (std::size_t i = 0; i < N; ++i) {
        if (cellIdx[i] >= 0) {
            candPos[i] = static_cast<std::int32_t>(candidates.size());
            candidates.push_back(static_cast<std::uint32_t>(i));
        }
    }
    const std::size_t C = candidates.size();
    S.resid.assign(C * 5u, 0.0f);
    S.used.assign(C, 0);

    // ---- back-fitting --------------------------------------------------------------
    std::array<double, 3> colour{1.0, 1.0, 1.0};
    std::vector<std::uint8_t> columnSky(W, 0);
    // Columns the texture gates refuse in the first round are refused in
    // every round (the correction is smooth, so it changes neither a
    // column's flat-pixel count nor its pixel noise): later rounds skip them.
    std::vector<std::uint8_t> columnDead(W, 0);
    for (int round = 0; round < P.iterations; ++round) {
        const std::vector<float> current = out.amount;
        std::fill(S.used.begin(), S.used.end(), std::uint8_t{0});  // per candidate

        // (1) per column: the sky under the current correction, and each
        //     accumulation pixel's residual against it.
        OSV_TRY(forRange(pool, W, 16, [&](std::size_t c0, std::size_t c1) {
            std::vector<double> xs, ys[4], ws, res, tmp;  // reused across the columns of this chunk
            std::vector<std::size_t> idx;
            for (std::size_t c = c0; c < c1; ++c) {
                columnSky[c] = 0;
                if (columnDead[c]) {
                    continue;
                }
                xs.clear();
                ws.clear();
                idx.clear();
                for (auto& v : ys) {
                    v.clear();
                }
                for (std::size_t r = 0; r < H; ++r) {
                    const std::size_t i = r * W + c;
                    if (!S.flat[i]) {
                        continue;
                    }
                    const double th = P.anchorDeg + static_cast<double>(S.tKnot[i]) * P.knotDeg;
                    if (th < fitLoDeg || th > domHiDeg) {
                        continue;
                    }
                    // The image with the current correction added, exactly
                    // as the kernel will add it: the sky alone.
                    const double h = tableAmount(current, S.tKnot[i], S.uSector[i]);
                    const float* px = rgba + i * 4u;
                    double yl = 0.0;
                    double yc[3];
                    for (int ch = 0; ch < 3; ++ch) {
                        const double v = static_cast<double>(px[ch]) + colour[static_cast<std::size_t>(ch)] * h;
                        yc[ch] = std::log2(std::max(v, static_cast<double>(kLightFloor)));
                        yl += kLumaWeight[ch] * std::max(v, static_cast<double>(kLightFloor));
                    }
                    xs.push_back(th - 85.0);
                    ys[0].push_back(std::log2(std::max(yl, static_cast<double>(kLightFloor))));
                    ys[1].push_back(yc[0]);
                    ys[2].push_back(yc[1]);
                    ys[3].push_back(yc[2]);
                    idx.push_back(i);
                }
                if (xs.size() < P.minColumnPixels) {
                    columnDead[c] = 1;
                    continue;
                }
                // The column's pixel noise, from second differences between
                // consecutive rows (a smooth structure - the sky's own
                // curvature, the ring - has none at this scale; texture and
                // compression noise do): 1.4826 MAD / sqrt(6).
                res.clear();
                for (std::size_t k = 1; k + 1 < xs.size(); ++k) {
                    if (idx[k] - idx[k - 1] == W && idx[k + 1] - idx[k] == W) {
                        res.push_back(std::fabs(ys[0][k + 1] - 2.0 * ys[0][k] + ys[0][k - 1]));
                    }
                }
                double noise = std::numeric_limits<double>::infinity();
                if (res.size() >= P.minColumnPixels / 2u) {
                    const std::size_t mid = res.size() / 2;
                    std::nth_element(res.begin(), res.begin() + static_cast<std::ptrdiff_t>(mid), res.end());
                    noise = 1.4826 * res[mid] / std::sqrt(6.0);
                }
                // The texture gate first (it needs no fit): pixel noise above
                // maxColumnNoiseStops is texture, and stays texture in every
                // later round.
                if (!(noise <= P.maxColumnNoiseStops)) {
                    columnDead[c] = 1;
                    continue;
                }
                // Robust luma fit: least squares, then three rounds of
                // Tukey's biweight (c = 4.685 robust sigma), which REJECTS a
                // deep ring outright instead of letting it pull the sky down
                // (what a Huber weight still does); from the second round on
                // the current correction has already taken the ring out.
                ws.assign(xs.size(), 1.0);
                double lumCoef[3] = {0.0, 0.0, 0.0};
                double scale = 0.0;
                bool fitted = true;
                for (int irls = 0; irls < 4 && fitted; ++irls) {
                    fitted = fitQuadratic(xs, ys[0], ws, lumCoef);
                    if (!fitted) {
                        break;
                    }
                    res.resize(xs.size());
                    for (std::size_t k = 0; k < xs.size(); ++k) {
                        res[k] = std::fabs(ys[0][k] - (lumCoef[0] + lumCoef[1] * xs[k] + lumCoef[2] * xs[k] * xs[k]));
                    }
                    tmp.assign(res.begin(), res.end());
                    const std::size_t mid = tmp.size() / 2;
                    std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(mid), tmp.end());
                    scale = 1.4826 * tmp[mid] + 1e-4;
                    const double cut = 4.685 * scale;
                    std::size_t kept = 0;
                    for (std::size_t k = 0; k < xs.size(); ++k) {
                        const double u = res[k] / cut;
                        ws[k] = u < 1.0 ? (1.0 - u * u) * (1.0 - u * u) : 0.0;
                        kept += ws[k] > 0.0 ? 1u : 0u;
                    }
                    fitted = kept >= P.minColumnPixels / 2u;
                }
                // The sky gate: a robust scatter about the sky model above
                // maxColumnScaleStops is not a sky a quadratic describes (a
                // horizon, a cloud edge, the aircraft).
                if (!fitted || scale > P.maxColumnScaleStops) {
                    continue;
                }
                double chCoef[3][3];
                bool allFit = true;
                for (int ch = 0; ch < 3 && allFit; ++ch) {
                    allFit = fitQuadratic(xs, ys[ch + 1], ws, chCoef[ch]);
                }
                if (!allFit) {
                    continue;
                }
                columnSky[c] = 1;
                // The light the RAW image lacks against the sky: the whole
                // correction (the current one is part of it), not an update.
                for (std::size_t k = 0; k < xs.size(); ++k) {
                    const std::size_t i = idx[k];
                    const std::int32_t slot = candPos[i];
                    if (slot < 0) {
                        continue;  // below the anchor or past the last domain knot: fit only
                    }
                    const float* px = rgba + i * 4u;
                    const double x = xs[k];
                    double eL = 0.0;
                    double lvl = 0.0;
                    float* e = S.resid.data() + static_cast<std::size_t>(slot) * 5u;
                    for (int ch = 0; ch < 3; ++ch) {
                        const double sky = std::exp2(chCoef[ch][0] + chCoef[ch][1] * x + chCoef[ch][2] * x * x);
                        const double ec = sky - static_cast<double>(px[ch]);
                        e[1 + ch] = static_cast<float>(ec);
                        eL += kLumaWeight[ch] * ec;
                        lvl += kLumaWeight[ch] * sky;
                    }
                    e[0] = static_cast<float>(eL);
                    e[4] = static_cast<float>(lvl);
                    S.used[static_cast<std::size_t>(slot)] = 1;
                }
            }
        }));

        // (2) gather each cell's samples (counting sort by cell) - over the
        //     accumulation candidates only, in row-major order.
        std::vector<std::uint32_t> cellCount(kLensShadingCells + 1u, 0);
        for (std::size_t slot = 0; slot < C; ++slot) {
            if (S.used[slot]) {
                ++cellCount[static_cast<std::size_t>(cellIdx[candidates[slot]]) + 1u];
            }
        }
        for (std::size_t k = 1; k < cellCount.size(); ++k) {
            cellCount[k] += cellCount[k - 1];
        }
        std::vector<std::uint32_t> order(cellCount.back());
        {
            std::vector<std::uint32_t> fill(cellCount.begin(), cellCount.end() - 1);
            for (std::size_t slot = 0; slot < C; ++slot) {
                if (S.used[slot]) {
                    order[fill[static_cast<std::size_t>(cellIdx[candidates[slot]])]++] =
                        static_cast<std::uint32_t>(slot);
                }
            }
        }

        // (3) per cell: the medians, and whether they are plausible shading.
        std::vector<float> cellAmount(kLensShadingCells, std::numeric_limits<float>::quiet_NaN());
        std::vector<float> cellLevel(kLensShadingCells, 0.0f);
        std::vector<std::array<float, 3>> cellChannel(kLensShadingCells, std::array<float, 3>{0.0f, 0.0f, 0.0f});
        OSV_TRY(forRange(pool, kLensShadingCells, 16, [&](std::size_t q0, std::size_t q1) {
            std::vector<float> v;
            std::vector<std::uint32_t> cols;
            for (std::size_t q = q0; q < q1; ++q) {
                const std::uint32_t b0 = cellCount[q];
                const std::uint32_t b1 = cellCount[q + 1];
                if (b1 - b0 < P.minCellPixels) {
                    continue;
                }
                cols.clear();
                for (std::uint32_t k = b0; k < b1; ++k) {
                    cols.push_back(candidates[order[k]] % static_cast<std::uint32_t>(W));
                }
                std::sort(cols.begin(), cols.end());
                const std::size_t distinct =
                    static_cast<std::size_t>(std::unique(cols.begin(), cols.end()) - cols.begin());
                if (distinct < P.minCellColumns) {
                    continue;
                }
                double med[5];
                for (int m = 0; m < 5; ++m) {
                    v.clear();
                    for (std::uint32_t k = b0; k < b1; ++k) {
                        v.push_back(S.resid[static_cast<std::size_t>(order[k]) * 5u + static_cast<std::size_t>(m)]);
                    }
                    med[m] = medianInPlace(v);
                }
                const double amount = med[0];
                const double level = med[4];
                if (!std::isfinite(amount) || !(level > 0.0)) {
                    continue;
                }
                // Plausibility: a structure deeper than a fraction of the sky
                // is a horizon or an object, not shading; and additive light
                // moves all three channels the same way, so a clearly
                // non-zero cell whose channels disagree in sign is not it.
                if (std::fabs(amount) > P.maxRelativeAmount * level) {
                    continue;
                }
                if (std::fabs(amount) > 0.02 * level) {
                    const double sgn = amount > 0.0 ? 1.0 : -1.0;
                    bool agree = true;
                    for (int ch = 0; ch < 3; ++ch) {
                        agree = agree && (med[1 + ch] * sgn > -0.005 * level);
                    }
                    if (!agree) {
                        continue;
                    }
                }
                cellAmount[q] = static_cast<float>(amount);
                cellLevel[q] = static_cast<float>(level);
                cellChannel[q] = {static_cast<float>(med[1]), static_cast<float>(med[2]), static_cast<float>(med[3])};
            }
        }));

        // (4) the new table: measured sectors only, gaps filled along theta,
        //     lightly smoothed, 0 at the anchor, tapered above the domain.
        std::vector<float> next(kLensShadingCells, 0.0f);
        std::vector<std::uint8_t> measured(kSectors, 0);
        for (int s = 0; s < kSectors; ++s) {
            int valid = 0;
            for (int k = 1; k <= kDomHi; ++k) {
                valid += std::isfinite(cellAmount[static_cast<std::size_t>(k) * kSectors + s]) ? 1 : 0;
            }
            if (static_cast<double>(valid) < P.minSectorCoverage * static_cast<double>(kDomHi)) {
                continue;  // not enough sky here: no evidence, no correction
            }
            measured[static_cast<std::size_t>(s)] = 1;
            std::vector<double> col(static_cast<std::size_t>(kDomHi) + 1u, std::numeric_limits<double>::quiet_NaN());
            col[0] = 0.0;
            for (int k = 1; k <= kDomHi; ++k) {
                col[static_cast<std::size_t>(k)] = cellAmount[static_cast<std::size_t>(k) * kSectors + s];
            }
            // Linear fill between the nearest valid knots (the last valid value
            // continues to the domain's end).
            int lastValid = 0;
            for (int k = 1; k <= kDomHi; ++k) {
                if (!std::isfinite(col[static_cast<std::size_t>(k)])) {
                    continue;
                }
                for (int g = lastValid + 1; g < k; ++g) {
                    const double t = static_cast<double>(g - lastValid) / static_cast<double>(k - lastValid);
                    col[static_cast<std::size_t>(g)] =
                        col[static_cast<std::size_t>(lastValid)] +
                        (col[static_cast<std::size_t>(k)] - col[static_cast<std::size_t>(lastValid)]) * t;
                }
                lastValid = k;
            }
            for (int g = lastValid + 1; g <= kDomHi; ++g) {
                col[static_cast<std::size_t>(g)] = col[static_cast<std::size_t>(lastValid)];
            }
            // A [1 2 1] / 4 pass along theta against per-cell median noise.
            std::vector<double> sm = col;
            for (int k = 1; k < kDomHi; ++k) {
                sm[static_cast<std::size_t>(k)] = 0.25 * col[static_cast<std::size_t>(k) - 1u] +
                                                  0.5 * col[static_cast<std::size_t>(k)] +
                                                  0.25 * col[static_cast<std::size_t>(k) + 1u];
            }
            sm[0] = 0.0;
            for (int k = 0; k <= kDomHi; ++k) {
                next[static_cast<std::size_t>(k) * kSectors + s] = static_cast<float>(sm[static_cast<std::size_t>(k)]);
            }
            // Above the domain: from the last domain knot to 0 over taperDeg.
            const double edge = sm[static_cast<std::size_t>(kDomHi)];
            for (int k = kDomHi + 1; k < kKnots; ++k) {
                const double over = static_cast<double>(k - kDomHi) * P.knotDeg;
                const double f = P.taperDeg > 0.0 ? std::max(0.0, 1.0 - over / P.taperDeg) : 0.0;
                next[static_cast<std::size_t>(k) * kSectors + s] = static_cast<float>(edge * f);
            }
        }

        // (5) the colour factors: least squares of each channel's residual on
        //     the luma amount over the valid cells, normalised to unit luma.
        double num[3] = {0.0, 0.0, 0.0};
        double den = 0.0;
        for (std::size_t q = 0; q < kLensShadingCells; ++q) {
            if (!std::isfinite(cellAmount[q])) {
                continue;
            }
            const double a = cellAmount[q];
            den += a * a;
            for (int ch = 0; ch < 3; ++ch) {
                num[ch] += a * static_cast<double>(cellChannel[q][static_cast<std::size_t>(ch)]);
            }
        }
        std::array<double, 3> nextColour{1.0, 1.0, 1.0};
        if (den > 1e-12) {
            double k[3];
            double lum = 0.0;
            for (int ch = 0; ch < 3; ++ch) {
                k[ch] = num[ch] / den;
                lum += kLumaWeight[ch] * k[ch];
            }
            if (lum > 1e-6 && std::isfinite(lum)) {
                for (int ch = 0; ch < 3; ++ch) {
                    nextColour[static_cast<std::size_t>(ch)] =
                        std::clamp(k[ch] / lum, 1.0 / kColourSpan, static_cast<double>(kColourSpan));
                }
            }
        }

        // (6) the diagnostics of this round, then the next round fits against it.
        out.amount = std::move(next);
        out.sectorMeasured = measured;
        colour = nextColour;
        out.skyColumns = static_cast<std::uint32_t>(std::count(columnSky.begin(), columnSky.end(), std::uint8_t{1}));
        double peakRel = 0.0;
        out.peakAmount = 0.0;
        for (int s = 0; s < kSectors; ++s) {
            if (!measured[static_cast<std::size_t>(s)]) {
                continue;
            }
            for (int k = 1; k <= kDomHi; ++k) {
                const std::size_t q = static_cast<std::size_t>(k) * kSectors + static_cast<std::size_t>(s);
                const double a = out.amount[q];
                const double lvl = cellLevel[q] > 0.0f ? cellLevel[q] : 0.0;
                if (lvl > 0.0 && std::fabs(a) / lvl > peakRel) {
                    peakRel = std::fabs(a) / lvl;
                    out.peakAmount = a;
                    // The lens's own dip there, luma stops (negative = darker
                    // than the sky): the image is the sky minus the amount.
                    out.peakStops = std::log2(std::max(lvl - a, 1e-9) / lvl);
                    out.peakThetaDeg = P.anchorDeg + static_cast<double>(k) * P.knotDeg;
                    out.peakPhiDeg = sectorCentreDeg(s);
                }
            }
        }
        out.measuredSectors = static_cast<std::uint32_t>(std::count(measured.begin(), measured.end(), std::uint8_t{1}));
        // Significance: a lens whose correction never leaves the noise of a
        // smooth sky is left alone entirely.
        if (round + 1 == P.iterations && peakRel < P.minPeakRelative) {
            std::fill(out.amount.begin(), out.amount.end(), 0.0f);
        }
    }
    for (int ch = 0; ch < 3; ++ch) {
        out.colour[static_cast<std::size_t>(ch)] = static_cast<float>(colour[static_cast<std::size_t>(ch)]);
    }
    return okStatus();
}

// ===========================================================================
//  The rank-OSV_SHADE_RANK separable form (for the parameter block)
// ===========================================================================

/// Eigen-decomposition of a symmetric n x n matrix (cyclic Jacobi), values
/// descending with their vectors as columns of `vec` (row-major n x n).
void symmetricEigen(std::vector<double> a, int n, std::vector<double>& val, std::vector<double>& vec) {
    vec.assign(static_cast<std::size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) {
        vec[static_cast<std::size_t>(i) * n + i] = 1.0;
    }
    const auto A = [&](int r, int c) -> double& { return a[static_cast<std::size_t>(r) * n + c]; };
    const auto V = [&](int r, int c) -> double& { return vec[static_cast<std::size_t>(r) * n + c]; };
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                off += A(p, q) * A(p, q);
            }
        }
        if (off < 1e-30) {
            break;
        }
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                if (std::fabs(A(p, q)) < 1e-300) {
                    continue;
                }
                const double theta = (A(q, q) - A(p, p)) / (2.0 * A(p, q));
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double akp = A(k, p);
                    const double akq = A(k, q);
                    A(k, p) = c * akp - s * akq;
                    A(k, q) = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {
                    const double apk = A(p, k);
                    const double aqk = A(q, k);
                    A(p, k) = c * apk - s * aqk;
                    A(q, k) = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = V(k, p);
                    const double vkq = V(k, q);
                    V(k, p) = c * vkp - s * vkq;
                    V(k, q) = s * vkp + c * vkq;
                }
            }
        }
    }
    // Sort by eigenvalue, descending.
    std::vector<int> ord(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        ord[static_cast<std::size_t>(i)] = i;
    }
    std::sort(ord.begin(), ord.end(), [&](int x, int y) { return A(x, x) > A(y, y); });
    std::vector<double> sortedVec(static_cast<std::size_t>(n) * n);
    val.assign(static_cast<std::size_t>(n), 0.0);
    for (int j = 0; j < n; ++j) {
        const int src = ord[static_cast<std::size_t>(j)];
        val[static_cast<std::size_t>(j)] = A(src, src);
        for (int i = 0; i < n; ++i) {
            sortedVec[static_cast<std::size_t>(i) * n + j] = V(i, src);
        }
    }
    vec = std::move(sortedVec);
}

/// One lens's table as its best rank-kRank separable approximation: radial
/// factors (kRank x kKnots) and azimuth factors (kRank x kSectors), from the
/// eigenvectors of M^T M (M = the kKnots x kSectors table).
void separableLens(const LensShadingLens& lens, OsvShadeLens& out) noexcept {
    std::memset(&out, 0, sizeof(out));
    if (lens.amount.size() != kLensShadingCells || lens.isZero()) {
        return;
    }
    try {
        // G = M^T M (kSectors x kSectors).
        std::vector<double> g(static_cast<std::size_t>(kSectors) * kSectors, 0.0);
        for (int s = 0; s < kSectors; ++s) {
            for (int t = s; t < kSectors; ++t) {
                double acc = 0.0;
                for (int k = 0; k < kKnots; ++k) {
                    acc += static_cast<double>(lens.amount[static_cast<std::size_t>(k) * kSectors + s]) *
                           static_cast<double>(lens.amount[static_cast<std::size_t>(k) * kSectors + t]);
                }
                g[static_cast<std::size_t>(s) * kSectors + t] = acc;
                g[static_cast<std::size_t>(t) * kSectors + s] = acc;
            }
        }
        std::vector<double> val;
        std::vector<double> vec;
        symmetricEigen(std::move(g), kSectors, val, vec);
        for (int r = 0; r < kRank; ++r) {
            if (!(val[static_cast<std::size_t>(r)] > 1e-20)) {
                continue;  // the table has lower rank: this term stays zero
            }
            // Azimuth factor: the unit eigenvector, signed so its largest
            // entry is positive (cosmetic; the product is sign-free).
            double big = 0.0;
            for (int s = 0; s < kSectors; ++s) {
                const double v = vec[static_cast<std::size_t>(s) * kSectors + r];
                big = std::fabs(v) > std::fabs(big) ? v : big;
            }
            const double sgn = big < 0.0 ? -1.0 : 1.0;
            for (int s = 0; s < kSectors; ++s) {
                out.azimuth[r][s] = static_cast<float>(sgn * vec[static_cast<std::size_t>(s) * kSectors + r]);
            }
            // Radial factor: M times that vector (sigma times the left vector).
            for (int k = 0; k < kKnots; ++k) {
                double acc = 0.0;
                for (int s = 0; s < kSectors; ++s) {
                    acc += static_cast<double>(lens.amount[static_cast<std::size_t>(k) * kSectors + s]) *
                           static_cast<double>(out.azimuth[r][s]);
                }
                out.radial[r][k] = static_cast<float>(acc);
            }
        }
        for (int ch = 0; ch < 3; ++ch) {
            out.colour[ch] = lens.colour[static_cast<std::size_t>(ch)];
        }
    } catch (...) {
        std::memset(&out, 0, sizeof(out));  // allocation failure: no correction for this lens
    }
}

/// The separable value at knot k, sector s (exact at the grid points).
[[nodiscard]] double separableAt(const OsvShadeLens& L, int k, int s) noexcept {
    double v = 0.0;
    for (int r = 0; r < kRank; ++r) {
        v += static_cast<double>(L.radial[r][k]) * static_cast<double>(L.azimuth[r][s]);
    }
    return v;
}

}  // namespace

// ===========================================================================
//  Small pieces
// ===========================================================================

const char* lensShadingModeName(LensShadingMode mode) noexcept {
    switch (mode) {
    case LensShadingMode::Off:
        return "off";
    case LensShadingMode::Auto:
        return "auto";
    }
    return "unknown";
}

bool LensShadingLens::isZero() const noexcept {
    return std::all_of(amount.begin(), amount.end(), [](float v) { return v == 0.0f; });
}

bool LensShadingModel::valid() const noexcept {
    if (!(theta0Rad > 0.0f) || !(dThetaRad > 0.0f) || !std::isfinite(theta0Rad) || !std::isfinite(dThetaRad)) {
        return false;
    }
    for (const LensShadingLens& L : lens) {
        if (L.amount.size() != kLensShadingCells || L.sectorMeasured.size() != static_cast<std::size_t>(kSectors)) {
            return false;
        }
        for (const float v : L.amount) {
            if (!std::isfinite(v)) {
                return false;
            }
        }
        for (const float c : L.colour) {
            if (!std::isfinite(c)) {
                return false;
            }
        }
    }
    return true;
}

bool LensShadingModel::active() const noexcept {
    return valid() && (!lens[0].isZero() || !lens[1].isZero());
}

LensShadingModel emptyLensShadingModel(const LensShadingParams& params) {
    LensShadingModel m;
    m.theta0Rad = static_cast<float>(deg2rad(params.anchorDeg));
    m.dThetaRad = static_cast<float>(deg2rad(params.knotDeg));
    for (LensShadingLens& L : m.lens) {
        L.amount.assign(kLensShadingCells, 0.0f);
        L.sectorMeasured.assign(kSectors, 0);
    }
    return m;
}

LensShadingModel scaledLensShadingModel(const LensShadingModel& model, double strength) {
    const float s = std::isfinite(strength) ? static_cast<float>(std::clamp(strength, 0.0, 1.0)) : 0.0f;
    LensShadingModel out = model;
    for (LensShadingLens& L : out.lens) {
        for (float& v : L.amount) {
            v *= s;
        }
    }
    return out;
}

// ===========================================================================
//  Measurement
// ===========================================================================

Result<RgbLensBands> renderShadingBands(const geom::LensRig& rig, const video::FramePair& frames,
                                        const geom::BlendParams& blend, const LensShadingParams& params,
                                        ThreadPool& pool) {
    if (!paramsUsable(params)) {
        return Error{ErrorCode::InvalidArgument, "renderShadingBands: unusable parameters"};
    }
    // The photometric field's band shader, over this band: each lens alone,
    // native linear light, occlusion alpha and no FOV feather.
    PhotoSeamParams bandParams;
    bandParams.band = params.band;
    return renderPhotoBands(rig, frames, blend, bandParams, pool);
}

Result<LensShadingModel> lensShadingFromBands(const RgbLensBands& bands, const geom::LensRig& rig,
                                              const LensShadingParams& params, ThreadPool* pool) {
    const auto t0 = ShadingClock::now();
    if (!paramsUsable(params)) {
        return Error{ErrorCode::InvalidArgument, "lensShadingFromBands: unusable parameters"};
    }
    // ---- the bands must be what renderShadingBands makes ----------------------
    const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
    if (bands.w < 64 || bands.h < 16 || bands.mapH == 0 ||
        static_cast<std::size_t>(bands.rowOffset) + bands.h > bands.mapH) {
        return Error{ErrorCode::InvalidArgument, "lensShadingFromBands: degenerate band geometry"};
    }
    for (int lens = 0; lens < 2; ++lens) {
        if (bands.rgba[lens].size() != n * 4u || !bands.thetaRad[lens] || bands.thetaRad[lens]->size() != n ||
            !(bands.thetaMaxRad[lens] > 0.0f) || !std::isfinite(bands.thetaMaxRad[lens])) {
            return Error{ErrorCode::InvalidArgument, "lensShadingFromBands: band buffers do not match the band"};
        }
    }
    OSV_TRY_ASSIGN(std::shared_ptr<const PhiTable> phi, phiTableFor(rig, bands, pool));
    if (!phi || !phi->phi[0] || !phi->phi[1] || phi->phi[0]->size() != n || phi->phi[1]->size() != n) {
        return Error{ErrorCode::Internal, "lensShadingFromBands: azimuth table size mismatch"};
    }

    // ---- both lenses ----------------------------------------------------------------
    LensShadingModel model = emptyLensShadingModel(params);
    try {
        for (int lens = 0; lens < 2; ++lens) {
            OSV_TRY(estimateLens(bands, *phi, lens, params, pool, model.lens[lens]));
        }
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "lensShadingFromBands: out of memory"};
    }
    model.statsMs = msSince(t0);
    if (!model.valid()) {
        return Error{ErrorCode::Internal, "lensShadingFromBands: the estimate is not finite"};
    }
    return model;
}

Result<LensShadingModel> measureLensShading(const geom::LensRig& rig, const video::FramePair& frames,
                                            const geom::BlendParams& blend, const LensShadingParams& params,
                                            ThreadPool& pool) {
    const auto t0 = ShadingClock::now();
    OSV_TRY_ASSIGN(RgbLensBands bands, renderShadingBands(rig, frames, blend, params, pool));
    const double bandMs = msSince(t0);
    OSV_TRY_ASSIGN(LensShadingModel model, lensShadingFromBands(bands, rig, params, &pool));
    model.bandMs = bandMs;
    return model;
}

// ===========================================================================
//  Time
// ===========================================================================

Result<LensShadingModel> blendLensShadingModels(const LensShadingModel& from, const LensShadingModel& to, double t) {
    if (!std::isfinite(t)) {
        return Error{ErrorCode::InvalidArgument, "blendLensShadingModels: non-finite weight"};
    }
    if (!from.valid() || !to.valid() || std::fabs(from.theta0Rad - to.theta0Rad) > 1e-7f ||
        std::fabs(from.dThetaRad - to.dThetaRad) > 1e-7f) {
        return Error{ErrorCode::InvalidArgument, "blendLensShadingModels: the models do not share one geometry"};
    }
    // Exact endpoints: from + (to - from) * 1 is not always `to` in floats.
    if (t <= 0.0) {
        return from;
    }
    if (t >= 1.0) {
        return to;
    }
    LensShadingModel out = to;  // diagnostics come from `to`
    const float tf = static_cast<float>(t);
    for (int lens = 0; lens < 2; ++lens) {
        const LensShadingLens& a = from.lens[lens];
        const LensShadingLens& b = to.lens[lens];
        LensShadingLens& o = out.lens[lens];
        for (std::size_t q = 0; q < kLensShadingCells; ++q) {
            o.amount[q] = a.amount[q] + (b.amount[q] - a.amount[q]) * tf;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            o.colour[c] = a.colour[c] + (b.colour[c] - a.colour[c]) * tf;
        }
        for (std::size_t s = 0; s < static_cast<std::size_t>(kSectors); ++s) {
            o.sectorMeasured[s] = (a.sectorMeasured[s] || b.sectorMeasured[s]) ? 1u : 0u;
        }
    }
    return out;
}

void LensShadingHistory::store(std::uint32_t bucket, const std::shared_ptr<const LensShadingModel>& measured,
                               const LensShadingParams& params) {
    if (!measured || !measured->valid()) {
        m_models[bucket] = Entry{};  // a refusal: not measured again
        return;
    }
    // ---- the previous bucket AS STORED NOW ------------------------------------
    // Read only what is stored at this moment and freeze it into this entry,
    // so a bucket measured later never changes how this one's frames render.
    std::shared_ptr<const LensShadingModel> previous;
    if (bucket > 0) {
        if (const auto it = m_models.find(bucket - 1); it != m_models.end()) {
            previous = it->second.model;  // null when that bucket was refused
        }
    }
    // ---- EMA against the previous bucket's STORED model -------------------------
    // Cell by cell, so a sector the new bucket did not measure (its amount is
    // zero there) decays toward zero and a newly measured one ramps in.
    LensShadingModel stored = *measured;
    const double alpha = std::isfinite(params.temporalAlpha) ? std::clamp(params.temporalAlpha, 0.0, 1.0) : 1.0;
    if (previous && alpha < 1.0) {
        auto ema = blendLensShadingModels(*previous, *measured, alpha);
        if (ema.ok()) {
            stored = std::move(ema).value();
        }
    }
    Entry entry;
    entry.model = std::make_shared<const LensShadingModel>(std::move(stored));
    entry.from = std::move(previous);
    m_models[bucket] = std::move(entry);
}

bool LensShadingHistory::measured(std::uint32_t bucket) const {
    return m_models.find(bucket) != m_models.end();
}

std::shared_ptr<const LensShadingModel> LensShadingHistory::modelFor(std::uint32_t frame,
                                                                     const LensShadingParams& params) const {
    (void)params;  // everything was fixed when the bucket was stored
    const auto own = m_models.find(parallaxBucket(frame));
    if (own == m_models.end() || !own->second.model) {
        return nullptr;
    }
    const Entry& e = own->second;
    // No glide partner, or the last frame of a bucket: its own model exactly.
    const double w = parallaxCrossfadeWeight(frame);
    if (!e.from || w >= 1.0) {
        return e.model;
    }
    // Glide from the previous bucket's model (as it was when this bucket was
    // stored) on the parallax grid's schedule, so every per-bucket
    // correction moves together.
    auto blended = blendLensShadingModels(*e.from, *e.model, w);
    if (blended.ok()) {
        return std::make_shared<const LensShadingModel>(std::move(blended).value());
    }
    return e.model;
}

void LensShadingHistory::trim(std::size_t limit, std::uint32_t keep) {
    while (m_models.size() > limit && !m_models.empty()) {
        auto victim = m_models.begin();
        if (victim->first == keep) {
            victim = std::prev(m_models.end());
            if (victim->first == keep) {
                break;
            }
        }
        m_models.erase(victim);
    }
}

void LensShadingHistory::clear() {
    m_models.clear();
}

// ===========================================================================
//  Evaluation and the kernel block
// ===========================================================================

double lensShadingTableAt(const LensShadingModel& model, int lens, double thetaRad, double phiRad) noexcept {
    if ((lens != 0 && lens != 1) || !model.valid() || !std::isfinite(thetaRad) || !std::isfinite(phiRad)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double t = (thetaRad - static_cast<double>(model.theta0Rad)) / static_cast<double>(model.dThetaRad);
    const double u = (phiRad + kPi) / kTwoPi * static_cast<double>(kSectors) - 0.5;
    return tableAmount(model.lens[lens].amount, t, u);
}

bool fillLensShadingBlock(const LensShadingModel& model, double strength, OsvRenderParams& p) noexcept {
    // All zero first: the block is either complete or off, never half set.
    p.shadeEnabled = 0;
    p.shadeTheta0Rad = 0.0f;
    p.shadeDThetaRad = 0.0f;
    p.shadeStrength = 0.0f;
    std::memset(&p.shade[0], 0, sizeof(p.shade));
    if (!model.active() || !std::isfinite(strength) || !(strength > 0.0)) {
        return false;
    }
    for (int lens = 0; lens < 2; ++lens) {
        separableLens(model.lens[lens], p.shade[lens]);
    }
    p.shadeEnabled = 1;
    p.shadeTheta0Rad = model.theta0Rad;
    p.shadeDThetaRad = model.dThetaRad;
    p.shadeStrength = static_cast<float>(std::min(strength, 1.0));
    return true;
}

bool lensShadingBlockValid(const OsvRenderParams& p) noexcept {
    if (p.shadeEnabled == 0) {
        return true;  // never read by a kernel
    }
    if (p.shadeEnabled != 1 || !std::isfinite(p.shadeTheta0Rad) || !std::isfinite(p.shadeDThetaRad) ||
        !(p.shadeDThetaRad > 0.0f) || !(p.shadeTheta0Rad >= 0.0f) || !(p.shadeStrength >= 0.0f) ||
        !(p.shadeStrength <= 1.0f)) {
        return false;
    }
    for (const OsvShadeLens& L : p.shade) {
        for (const float c : L.colour) {
            if (!std::isfinite(c)) {
                return false;
            }
        }
        for (int r = 0; r < kRank; ++r) {
            for (const float v : L.radial[r]) {
                if (!std::isfinite(v)) {
                    return false;
                }
            }
            for (const float v : L.azimuth[r]) {
                if (!std::isfinite(v)) {
                    return false;
                }
            }
        }
    }
    return true;
}

double lensShadingSeparableError(const LensShadingModel& model) noexcept {
    if (!model.active()) {
        return 0.0;
    }
    double worst = 0.0;
    for (int lens = 0; lens < 2; ++lens) {
        OsvShadeLens L;
        separableLens(model.lens[lens], L);
        for (int k = 0; k < kKnots; ++k) {
            for (int s = 0; s < kSectors; ++s) {
                const double table =
                    model.lens[lens].amount[static_cast<std::size_t>(k) * kSectors + static_cast<std::size_t>(s)];
                worst = std::max(worst, std::fabs(separableAt(L, k, s) - table));
            }
        }
    }
    return worst;
}

}  // namespace osv::render
