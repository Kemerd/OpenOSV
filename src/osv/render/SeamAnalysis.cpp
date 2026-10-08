// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/render/SeamAnalysis.h"
#include "osv/color/ColorParams.h"
#include "osv/core/Log.h"
#include "osv/geom/EquirectMap.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/DeviceBandShader.h"
#include "osv/render/RenderParamsBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

namespace osv::render {

namespace {

// ---------------------------------------------------------------------------
//  The installed device band shader (see DeviceBandShader.h)
// ---------------------------------------------------------------------------

/// Holder for the process-wide shader.  Allocated once and intentionally
/// never destroyed: the shader belongs to the CUDA library, and running its
/// destructor from this library's static teardown - in an order nobody
/// controls, possibly after the CUDA runtime has unloaded - buys nothing the
/// OS does not do at exit anyway.
struct ShaderSlot {
    std::mutex mutex;
    std::shared_ptr<DeviceBandShader> shader;
};

ShaderSlot& shaderSlot() {
    static ShaderSlot* slot = new ShaderSlot();  // intentionally leaked, see above
    return *slot;
}

/// True when either lens of `job` lives in device memory.
[[nodiscard]] bool jobOnDevice(const RenderJob& job) noexcept {
    return job.planesOnDevice[0] || job.planesOnDevice[1];
}

/// The shader for a device job, or the error that explains why there is none.
///
/// Worded for the person reading a log: a device job with no shader is a
/// configuration problem (the CUDA analyses were never installed), not a
/// broken frame.
Result<std::shared_ptr<DeviceBandShader>> requireDeviceShader() {
    std::shared_ptr<DeviceBandShader> gpu = deviceBandShader();
    if (!gpu) {
        return Error{ErrorCode::InvalidArgument,
                     "shadeRows: the frames are on the GPU; the band analyses need host frames or an installed "
                     "device band shader (osv::render::installCudaAnalyses)"};
    }
    return gpu;
}

/// A pixel counts as TRUSTED for gain estimation when BOTH lenses see it at
/// (almost) full production weight - DJI's colour compensation uses the same
/// alpha >= 0.99 rule.  See estimateGain.
constexpr float kTrustedAlpha = 0.99f;

/// Luma of a code-space or linear RGB triple (BT.2020 weights; for code space
/// the exact weights do not matter as long as both lenses use the same).
inline float lumaOf(const float* px) noexcept { return 0.2627f * px[0] + 0.6780f * px[1] + 0.0593f * px[2]; }

/// NCC between two equally sized sample vectors with a validity mask.
double nccMasked(const float* a, const float* b, const std::uint8_t* mask, std::size_t n) {
    double sa = 0, sb = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (mask[i]) {
            sa += a[i];
            sb += b[i];
            ++count;
        }
    }
    if (count < 16) {
        return 0.0;
    }
    const double ma = sa / static_cast<double>(count);
    const double mb = sb / static_cast<double>(count);
    double num = 0, da = 0, db = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (mask[i]) {
            const double xa = a[i] - ma;
            const double xb = b[i] - mb;
            num += xa * xb;
            da += xa * xa;
            db += xb * xb;
        }
    }
    if (da <= 0.0 || db <= 0.0) {
        return 0.0;
    }
    return num / std::sqrt(da * db);
}

/// Shade only rows [row0, row1) of `job` into a tightly packed RGBA buffer.
///
/// Every analysis here needs a thin band around the equator of a polar-axis
/// map - 68 rows of 1024 at the default size - and they used to render the
/// WHOLE map and throw the rest away, i.e. ~94% of the work was discarded.
/// That was tolerable for a one-off seam search; it is not once the parallax
/// correction runs it every frame.
///
/// The per-pixel call is exactly the one CpuRenderer makes, with the same
/// ABSOLUTE row index, so each band pixel is bit-identical to the matching
/// pixel of a full-map render.  Only the rows nobody read are skipped.
///
/// A job whose frames live in VRAM goes to the installed DeviceBandShader
/// instead - the same osvShadePixelW, run on the GPU - because reading its
/// device addresses here would fault (an access violation, seen with
/// `osvtool --hw cuda --device cuda --seam-search` before the GPU path
/// existed).  Host jobs never take that branch, so their output is unchanged.
Result<std::vector<float>> shadeRows(const RenderJob& job, std::uint32_t row0, std::uint32_t row1, ThreadPool& pool) {
    if (!job.valid()) {
        return Error{ErrorCode::InvalidArgument, "shadeRows: invalid render job"};
    }
    const OsvRenderParams params = job.params;
    if (row1 <= row0 || row1 > static_cast<std::uint32_t>(params.outH)) {
        return Error{ErrorCode::InvalidArgument, "shadeRows: row range outside the map"};
    }
    // Device-resident frames: shade on the GPU or refuse, never read here.
    if (jobOnDevice(job)) {
        OSV_TRY_ASSIGN(std::shared_ptr<DeviceBandShader> gpu, requireDeviceShader());
        return gpu->shadeRowsRgba(job, row0, row1);
    }
    const OsvPlane planes[2] = {job.planes[0], job.planes[1]};
    const float* seam = (params.seamShiftEnabled && !job.seamShiftDeg.empty()) ? job.seamShiftDeg.data() : nullptr;
    const float* warp = (params.warpEnabled && !job.warpGrid.empty()) ? job.warpGrid.data() : nullptr;
    const std::size_t width = static_cast<std::size_t>(params.outW);
    std::vector<float> rgba(width * (row1 - row0) * 4u, 0.0f);
    float* base = rgba.data();

    // One row per task: a band is only a few dozen rows, and coarser grains
    // would leave most of the pool idle.
    Status st = pool.parallelFor(row0, row1, 1, [&](std::size_t rBegin, std::size_t rEnd) {
        for (std::size_t y = rBegin; y < rEnd; ++y) {
            float* row = base + (y - row0) * width * 4u;
            for (int x = 0; x < params.outW; ++x) {
                osvShadePixelW(&params, planes, seam, warp, x, static_cast<int>(y), row + static_cast<std::size_t>(x) * 4u);
            }
        }
    });
    OSV_TRY(st);
    return rgba;
}

// ---------------------------------------------------------------------------
//  The seam-shift table's estimator (see SeamAnalysis.h)
// ---------------------------------------------------------------------------

/// Score of a shift that was not scored (too few co-visible pixels).  Below
/// any real NCC, so it can never win or be taken for a second peak.
constexpr double kUnscored = -2.0;

/// Largest search range the per-column score buffer holds (rows, +/-).
constexpr int kMaxSearchRows = 64;

/// Half width (rows) of the 1-row-sigma Gaussian the texture measure blurs
/// with before differencing: 4 sigma, where the tail is below 3.4e-4.
constexpr int kTextureBlurRadius = 4;

/// Columns of the ring copied onto each end of the open system the smoother
/// solves.  The smoother's influence decays over (lambda2 / weight)^(1/4)
/// columns - ~12 where columns are confident, ~67 where only the 1e-3 prior
/// holds them - so 512 makes the cut ends invisible in the kept centre.
constexpr int kSmootherWrapPad = 512;

/// Smoothstep from 0 at `lo` to 1 at `hi`, clamped outside.  A non-finite
/// `x` gives 0 (no evidence), so a NaN feature can never add confidence.
[[nodiscard]] double smoothRamp(double lo, double hi, double x) noexcept {
    if (!std::isfinite(x)) {
        return 0.0;
    }
    const double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// True when a smoothstep ramp is usable: finite ends, rising.
[[nodiscard]] bool validRamp(double lo, double hi) noexcept {
    return std::isfinite(lo) && std::isfinite(hi) && hi > lo;
}

/// Check every SeamSearchParams field the estimator reads; the message names
/// the first bad one.
[[nodiscard]] Status validateSearchParams(const SeamSearchParams& p) {
    if (p.maxShiftPx <= 0 || p.windowHalfCols < 0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: bad parameters"};
    }
    if (!std::isfinite(p.minNcc)) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: minNcc is not finite"};
    }
    if (!validRamp(p.confNccLo, p.confNccHi) || !validRamp(p.confTextureLo, p.confTextureHi) ||
        !validRamp(p.confDistinctLo, p.confDistinctHi) || !validRamp(p.confCoverageLo, p.confCoverageHi)) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: a confidence ramp is not finite and rising"};
    }
    if (!std::isfinite(p.minCovalidFraction) || p.minCovalidFraction < 0.0 || p.minCovalidFraction > 1.0 ||
        !std::isfinite(p.unmeasuredFraction) || p.unmeasuredFraction < 0.0 || p.unmeasuredFraction > 1.0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: a co-visibility fraction is outside [0, 1]"};
    }
    if (!std::isfinite(p.unmeasuredInheritDeg) || p.unmeasuredInheritDeg < 0.0 || p.unmeasuredInheritDeg > 180.0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: unmeasuredInheritDeg is outside [0, 180]"};
    }
    if (!std::isfinite(p.smoothLambda2) || p.smoothLambda2 < 0.0 || !std::isfinite(p.smoothLambda1) ||
        p.smoothLambda1 < 0.0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: a smoothing weight is negative or not finite"};
    }
    // The prior weights keep the system positive definite: they must be > 0.
    // +infinity is allowed for the unmeasured one only (it pins).
    if (!std::isfinite(p.priorWeight) || p.priorWeight <= 0.0 || std::isnan(p.unmeasuredPriorWeight) ||
        p.unmeasuredPriorWeight <= 0.0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: a prior weight is not positive"};
    }
    if (!std::isfinite(p.huberDeg) || p.huberDeg <= 0.0 || p.irlsIterations < 1 || p.irlsIterations > 32) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: bad robust-smoother parameters"};
    }
    return okStatus();
}

/// Per-column texture of one lens, summed over the inner rows: for every
/// column, the sum of |blurred(r + 1) - blurred(r)| over rows r of
/// [inner0, inner1) where both rows are covered, and how many such pairs.
///
/// "blurred" is a 1-row-sigma Gaussian along the rows, as a NORMALISED
/// convolution over covered pixels: inside the coverage it is the plain
/// Gaussian, and at a coverage edge (a rim, an occlusion polygon) the black
/// beyond it never enters, so the edge cannot pose as texture.
void columnTexture(const std::vector<float>& luma, const std::vector<float>& alpha, std::uint32_t w,
                   std::uint32_t h, int inner0, int inner1, std::uint32_t c, double& sum, double& count) {
    sum = 0.0;
    count = 0.0;
    // Gaussian taps, sigma 1 row.
    double taps[2 * kTextureBlurRadius + 1];
    for (int k = -kTextureBlurRadius; k <= kTextureBlurRadius; ++k) {
        taps[k + kTextureBlurRadius] = std::exp(-0.5 * static_cast<double>(k * k));
    }
    const auto covered = [&](int r) {
        return r >= 0 && r < static_cast<int>(h) && alpha[static_cast<std::size_t>(r) * w + c] > 0.5f;
    };
    const auto blurred = [&](int r) {
        double acc = 0.0;
        double wsum = 0.0;
        for (int k = -kTextureBlurRadius; k <= kTextureBlurRadius; ++k) {
            const int rr = r + k;
            if (!covered(rr)) {
                continue;
            }
            const double v = luma[static_cast<std::size_t>(rr) * w + c];
            if (!std::isfinite(v)) {
                continue;  // a broken pixel weighs nothing
            }
            acc += taps[k + kTextureBlurRadius] * v;
            wsum += taps[k + kTextureBlurRadius];
        }
        return wsum > 0.0 ? acc / wsum : std::numeric_limits<double>::quiet_NaN();
    };
    // Walk the rows once, carrying the previous blurred value.
    double previous = covered(inner0) ? blurred(inner0) : std::numeric_limits<double>::quiet_NaN();
    for (int r = inner0; r < inner1; ++r) {
        const double next = covered(r + 1) ? blurred(r + 1) : std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(previous) && std::isfinite(next)) {
            sum += std::fabs(next - previous);
            count += 1.0;
        }
        previous = next;
    }
}

/// The robust, confidence-weighted Whittaker smoother on the closed ring
/// (SeamAnalysis.h): solves
///
///     (W + l2 D2'D2 + l1 D1'D1 + M) T = W m + M p
///
/// `pinned` columns are held at p exactly (the limit of an infinite prior
/// weight, eliminated from the system so the matrix stays well scaled).
///
/// The ring is unrolled with kSmootherWrapPad columns copied onto each end
/// and the open pentadiagonal system is solved by a banded LDL'
/// factorisation - O(n), well under a millisecond for 2048 columns - after
/// which only the centre is kept.  Huber IRLS: after each solve the weight
/// of a column whose measurement sits more than `huberDeg` from the curve is
/// scaled by huberDeg / |m - T|, so a lone wrong column cannot pull its
/// neighbours along.
///
/// Every vector has the ring's column count; `m` may hold anything where
/// `w` is 0.  Returns the table in degrees.
[[nodiscard]] Result<std::vector<double>> smoothSeamRing(const std::vector<double>& m, const std::vector<double>& w,
                                                         const std::vector<double>& mu,
                                                         const std::vector<std::uint8_t>& pinned,
                                                         const std::vector<double>& p, double lambda2, double lambda1,
                                                         double huberDeg, int iterations) {
    const std::size_t cols = m.size();
    if (cols == 0 || w.size() != cols || mu.size() != cols || pinned.size() != cols || p.size() != cols) {
        return Error{ErrorCode::InvalidArgument, "smoothSeamRing: mismatched inputs"};
    }
    const std::size_t pad = kSmootherWrapPad;
    const std::size_t n = cols + 2 * pad;
    // Ring column of unrolled index i (the padding repeats the ring, as many
    // turns as it takes when the ring is narrower than the pad).
    const auto ringCol = [&](std::size_t i) {
        const long long k = static_cast<long long>(i) - static_cast<long long>(pad);
        const long long c = static_cast<long long>(cols);
        return static_cast<std::size_t>(((k % c) + c) % c);
    };

    // ---- the fixed part of the matrix: the two difference penalties ----------
    // Open (non-cyclic) D1'D1 and D2'D2 on n points; n >= 1024 > 4 always.
    std::vector<double> pen0(n), pen1(n, 0.0), pen2(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double d2 = (i == 0 || i == n - 1) ? 1.0 : (i == 1 || i == n - 2) ? 5.0 : 6.0;
        const double d1 = (i == 0 || i == n - 1) ? 1.0 : 2.0;
        pen0[i] = lambda2 * d2 + lambda1 * d1;
        if (i + 1 < n) {
            const double o2 = (i == 0 || i == n - 2) ? -2.0 : -4.0;
            pen1[i] = lambda2 * o2 - lambda1;
        }
        if (i + 2 < n) {
            pen2[i] = lambda2;
        }
    }

    std::vector<double> ww = w;  // the Huber-scaled weights of the current pass
    std::vector<double> d0(n), d1(n), d2(n), rhs(n), x(n);
    std::vector<double> diag(n), e(n, 0.0), f(n, 0.0);
    for (int pass = 0; pass < iterations; ++pass) {
        // ---- assemble A (diagonal + two super-diagonals) and the right side --------
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t c = ringCol(i);
            d0[i] = ww[c] + mu[c] + pen0[i];
            d1[i] = pen1[i];
            d2[i] = pen2[i];
            rhs[i] = ww[c] * m[c] + mu[c] * p[c];
        }
        // ---- pinned columns: T = p exactly ----------------------------------------
        // Move each pinned column's coupling to its free neighbours' right
        // sides, then make its row the identity.  Symmetric, so A stays SPD.
        for (std::size_t i = 0; i < n; ++i) {
            if (!pinned[ringCol(i)]) {
                continue;
            }
            const double pi = p[ringCol(i)];
            // (i, i+1) and (i, i+2) live in row i; (i-1, i) and (i-2, i) in the rows above.
            const auto release = [&](std::size_t j, double& coupling) {
                if (!pinned[ringCol(j)]) {
                    rhs[j] -= coupling * pi;
                }
                coupling = 0.0;
            };
            if (i + 1 < n) {
                release(i + 1, d1[i]);
            }
            if (i + 2 < n) {
                release(i + 2, d2[i]);
            }
            if (i >= 1) {
                release(i - 1, d1[i - 1]);
            }
            if (i >= 2) {
                release(i - 2, d2[i - 2]);
            }
            d0[i] = 1.0;
            rhs[i] = pi;
        }

        // ---- banded LDL': A = L D L', L unit lower with two sub-diagonals (e, f) -----
        for (std::size_t i = 0; i < n; ++i) {
            f[i] = i >= 2 ? d2[i - 2] / diag[i - 2] : 0.0;
            e[i] = i >= 1 ? (d1[i - 1] - (i >= 2 ? f[i] * e[i - 1] * diag[i - 2] : 0.0)) / diag[i - 1] : 0.0;
            diag[i] = d0[i] - (i >= 1 ? e[i] * e[i] * diag[i - 1] : 0.0) - (i >= 2 ? f[i] * f[i] * diag[i - 2] : 0.0);
            if (!(diag[i] > 0.0) || !std::isfinite(diag[i])) {
                return Error{ErrorCode::Internal, "smoothSeamRing: the system is not positive definite"};
            }
        }
        // Forward (L y = b), scale (z = y / D), backward (L' x = z).
        for (std::size_t i = 0; i < n; ++i) {
            x[i] = rhs[i] - (i >= 1 ? e[i] * x[i - 1] : 0.0) - (i >= 2 ? f[i] * x[i - 2] : 0.0);
        }
        for (std::size_t i = 0; i < n; ++i) {
            x[i] /= diag[i];
        }
        for (std::size_t k = n; k-- > 0;) {
            x[k] -= (k + 1 < n ? e[k + 1] * x[k + 1] : 0.0) + (k + 2 < n ? f[k + 2] * x[k + 2] : 0.0);
        }

        // ---- Huber reweighting for the next pass ---------------------------------
        if (pass + 1 < iterations) {
            for (std::size_t c = 0; c < cols; ++c) {
                const double r = std::fabs(m[c] - x[pad + c]);
                ww[c] = (std::isfinite(r) && r > huberDeg) ? w[c] * (huberDeg / r) : w[c];
            }
        }
    }

    // The centre: one turn of the ring.
    std::vector<double> table(cols);
    for (std::size_t c = 0; c < cols; ++c) {
        table[c] = x[pad + c];
        if (!std::isfinite(table[c])) {
            return Error{ErrorCode::Internal, "smoothSeamRing: non-finite solution"};
        }
    }
    return table;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Device band shader registry
// ---------------------------------------------------------------------------
void setDeviceBandShader(std::shared_ptr<DeviceBandShader> shader) noexcept {
    ShaderSlot& slot = shaderSlot();
    std::shared_ptr<DeviceBandShader> previous;
    {
        std::lock_guard<std::mutex> lock(slot.mutex);
        previous = std::move(slot.shader);
        slot.shader = std::move(shader);
    }
    // `previous` is released here, outside the lock, so a shader whose
    // destructor does real work can never deadlock against a reader.
}

std::shared_ptr<DeviceBandShader> deviceBandShader() noexcept {
    ShaderSlot& slot = shaderSlot();
    std::lock_guard<std::mutex> lock(slot.mutex);
    return slot.shader;
}

Result<LensBands> renderLensBands(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, bool linear,
                                  const std::vector<float>* seamTable, ThreadPool& pool, const WarpGridView* warp) {
    if (band.equirectW < 64 || band.equirectW > 16384 || band.bandHalfDeg <= 0.0 || band.bandHalfDeg > 45.0) {
        return Error{ErrorCode::InvalidArgument, "renderLensBands: bad band parameters"};
    }
    // Full polar-axis map GEOMETRY, so the mapping is identical to production,
    // but only the band rows are actually shaded (see shadeRows).
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

    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit,
                                                     linear ? color::OutputTransfer::Linear
                                                            : color::OutputTransfer::Passthrough,
                                                     0.0f);
    if (row1 <= row0) {
        return Error{ErrorCode::InvalidArgument, "renderLensBands: band has no rows"};
    }
    LensBands out;
    out.w = band.equirectW;
    out.h = row1 - row0;
    out.rowOffset = row0;
    out.mapH = mapH;

    for (int lens = 0; lens < 2; ++lens) {
        RenderParamsBuilder builder;
        builder.rig(rig).equirect(map).blend(blend, true).color(cp).lensEnabled(1 - lens, false);
        if (seamTable && !seamTable->empty()) {
            builder.seam(*seamTable);
        }
        // Each lens is rendered ALONE here, but the kernel still applies the
        // warp with the sign that belongs to that lens index - so the two
        // single-lens bands end up at the same middle position the blended
        // render would put them at.  That is what makes an NCC measured on
        // these bands describe the real output rather than an approximation.
        if (warp && warp->valid()) {
            const std::size_t n = static_cast<std::size_t>(warp->w) * warp->h * 2u;
            builder.warp(std::vector<float>(warp->uv, warp->uv + n), warp->w, warp->h, warp->latMinRad,
                         warp->latMaxRad);
        }
        OSV_TRY_ASSIGN(RenderJob job, builder.build(frames));
        // Frames in VRAM: shade AND reduce to luma / coverage on the GPU, so
        // only the two planes the analyses keep are downloaded - a quarter of
        // the RGBA traffic, and no CPU pass over the band at all.
        if (jobOnDevice(job)) {
            if (row1 > static_cast<std::uint32_t>(job.params.outH)) {
                return Error{ErrorCode::InvalidArgument, "renderLensBands: band outside the map"};
            }
            OSV_TRY_ASSIGN(std::shared_ptr<DeviceBandShader> gpu, requireDeviceShader());
            OSV_TRY(gpu->shadeRowsLumaAlpha(job, row0, row1, out.luma[lens], out.alpha[lens]));
            const std::size_t expected = static_cast<std::size_t>(out.w) * out.h;
            if (out.luma[lens].size() != expected || out.alpha[lens].size() != expected) {
                return Error{ErrorCode::Internal, "renderLensBands: the device band shader returned the wrong size"};
            }
            continue;
        }
        OSV_TRY_ASSIGN(std::vector<float> rgba, shadeRows(job, row0, row1, pool));
        out.luma[lens].resize(static_cast<std::size_t>(out.w) * out.h);
        out.alpha[lens].resize(out.luma[lens].size());
        for (std::uint32_t r = 0; r < out.h; ++r) {
            const float* src = rgba.data() + static_cast<std::size_t>(r) * out.w * 4u;
            for (std::uint32_t c = 0; c < out.w; ++c) {
                out.luma[lens][static_cast<std::size_t>(r) * out.w + c] = lumaOf(src + c * 4);
                out.alpha[lens][static_cast<std::size_t>(r) * out.w + c] = src[c * 4 + 3];
            }
        }
    }
    return out;
}

Result<double> overlapNcc(const geom::LensRig& rig, const video::FramePair& frames, const geom::BlendParams& blend,
                          const BandParams& band, ThreadPool& pool, const std::vector<float>* seamTable,
                          const WarpGridView* warp) {
    OSV_TRY_ASSIGN(LensBands b, renderLensBands(rig, frames, blend, band, false, seamTable, pool, warp));
    const std::size_t n = b.luma[0].size();
    std::vector<std::uint8_t> mask(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        mask[i] = (b.alpha[0][i] > 0.5f && b.alpha[1][i] > 0.5f) ? 1 : 0;
    }
    return nccMasked(b.luma[0].data(), b.luma[1].data(), mask.data(), n);
}

Result<SeamProfile> searchSeam(const geom::LensRig& rig, const video::FramePair& frames,
                               const geom::BlendParams& blend, const SeamSearchParams& params, ThreadPool& pool,
                               const std::vector<float>* seamPrior) {
    // Checked before the (comparatively expensive) band render.
    OSV_TRY(validateSearchParams(params));
    // Render a band wide enough to hold the search range on both sides.
    BandParams band = params.band;
    const double rowsPerDeg = static_cast<double>(band.equirectW / 2) / 180.0;
    band.bandHalfDeg += static_cast<double>(params.maxShiftPx) / rowsPerDeg;
    OSV_TRY_ASSIGN(LensBands b, renderLensBands(rig, frames, blend, band, false, nullptr, pool));
    return searchSeamFromBands(b, params, &pool, seamPrior);
}

Result<SeamProfile> searchSeamFromBands(const LensBands& b, const SeamSearchParams& params, ThreadPool* pool,
                                        const std::vector<float>* seamPrior) {
    OSV_TRY(validateSearchParams(params));
    // ---- the bands: sizes must agree, or every index below is a guess ----------
    const std::size_t planeSize = static_cast<std::size_t>(b.w) * b.h;
    if (b.w == 0 || b.h == 0 || b.mapH == 0 || b.luma[0].size() != planeSize || b.luma[1].size() != planeSize ||
        b.alpha[0].size() != planeSize || b.alpha[1].size() != planeSize) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: the bands are empty or their planes disagree in size"};
    }
    const int W = static_cast<int>(b.w);
    const int H = static_cast<int>(b.h);
    const int S = params.maxShiftPx;
    const int win = params.windowHalfCols;
    if (2 * win + 1 > W) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: the matching window is wider than the band"};
    }
    // Rows of the inner (un-padded) band.
    const int inner0 = S;
    const int inner1 = H - S;
    if (inner1 <= inner0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: band too small for the shift range"};
    }

    // ---- per-column texture of each lens (summed over the inner rows) ------------
    // Summed per column first, so the window below is a cheap 9-column sum.
    std::vector<double> texSum[2], texCount[2];
    for (int lens = 0; lens < 2; ++lens) {
        texSum[lens].assign(b.w, 0.0);
        texCount[lens].assign(b.w, 0.0);
    }
    const auto textureBody = [&](std::size_t c0, std::size_t c1) {
        for (std::size_t c = c0; c < c1; ++c) {
            for (int lens = 0; lens < 2; ++lens) {
                columnTexture(b.luma[lens], b.alpha[lens], b.w, b.h, inner0, inner1, static_cast<std::uint32_t>(c),
                              texSum[lens][c], texCount[lens][c]);
            }
        }
    };
    if (pool != nullptr) {
        OSV_TRY(pool->parallelFor(0, b.w, 64, textureBody));
    } else {
        textureBody(0, b.w);
    }

    // ---- the per-column search ----------------------------------------------------------
    SeamProfile profile;
    profile.columns = b.w;
    profile.shiftDeg.assign(b.w, 0.0f);
    profile.ncc.assign(b.w, 0.0f);
    profile.confidence.assign(b.w, 0.0f);
    profile.measuredDeg.assign(b.w, std::numeric_limits<float>::quiet_NaN());
    // Per column: is it unmeasured (too few co-visible pixels unshifted), and
    // did it pass minNcc (statistics).
    std::vector<std::uint8_t> unmeasured(b.w, 0), accepted(b.w, 0);
    // Pixel counts of one window, the thresholds of the co-visibility rules.
    const double windowPixels = static_cast<double>(2 * win + 1) * static_cast<double>(inner1 - inner0);
    const double unmeasuredBelow = params.unmeasuredFraction * windowPixels;
    // Rows -> degrees.  Positive row shift = lens 1 content lower in the band
    // = feature farther from both axes (see osv_kernel.h).
    const double degPerRow = 180.0 / static_cast<double>(b.mapH);

    const auto searchBody = [&](std::size_t c0, std::size_t c1) {
        std::vector<float> va, vb;
        std::vector<std::uint8_t> mask;
        // Gather column c's window at shift s into va / vb / mask and return
        // how many of its pixel pairs are co-visible (covered and finite).
        const auto gather = [&](std::size_t c, int s) {
            va.clear();
            vb.clear();
            mask.clear();
            std::size_t covalid = 0;
            for (int r = inner0; r < inner1; ++r) {
                for (int dc = -win; dc <= win; ++dc) {
                    const int cc = (static_cast<int>(c) + dc + W) % W;  // wrap around longitude
                    const std::size_t ia = static_cast<std::size_t>(r) * b.w + cc;
                    const std::size_t ib = static_cast<std::size_t>(r + s) * b.w + cc;
                    const bool both = b.alpha[0][ia] > 0.5f && b.alpha[1][ib] > 0.5f &&
                                      std::isfinite(b.luma[0][ia]) && std::isfinite(b.luma[1][ib]);
                    va.push_back(b.luma[0][ia]);
                    vb.push_back(b.luma[1][ib]);
                    mask.push_back(both ? 1 : 0);
                    covalid += both ? 1u : 0u;
                }
            }
            return covalid;
        };
        for (std::size_t c = c0; c < c1; ++c) {
            double scores[2 * kMaxSearchRows + 1];
            const int Sc = std::min(S, kMaxSearchRows);
            // ---- the unshifted window decides whether the column is measured ----
            // Across an occlusion arc nothing is co-visible unshifted, and the
            // only shifts that pair anything pair DIFFERENT content (lens 0
            // above the polygon gap with lens 1 below it), which correlates
            // by chance: such a column is not scored at all.
            const std::size_t covalidAtZero = gather(c, 0);
            unmeasured[c] = static_cast<double>(covalidAtZero) < unmeasuredBelow ? 1 : 0;
            if (unmeasured[c]) {
                continue;  // ncc 0, weight 0, no measurement
            }
            // A shift is scored only when it keeps enough of the column's own
            // unshifted support - never one that pairs a sliver of it.
            const double minCovalid = params.minCovalidFraction * static_cast<double>(covalidAtZero);
            double best = kUnscored;
            int bestS = 0;
            for (int s = -Sc; s <= Sc; ++s) {
                const std::size_t covalid = gather(c, s);
                if (static_cast<double>(covalid) < minCovalid || covalid == 0) {
                    scores[s + Sc] = kUnscored;
                    continue;
                }
                const double score = nccMasked(va.data(), vb.data(), mask.data(), va.size());
                scores[s + Sc] = score;
                if (score > best) {
                    best = score;
                    bestS = s;
                }
            }
            if (best <= kUnscored) {
                continue;  // nothing measured here: ncc 0, weight 0
            }
            profile.ncc[c] = static_cast<float>(best);

            // Parabolic sub-pixel refinement around the peak - only between
            // two scored neighbours, never against an unscored -2.
            double refined = bestS;
            if (bestS > -Sc && bestS < Sc && scores[bestS - 1 + Sc] > kUnscored && scores[bestS + 1 + Sc] > kUnscored) {
                const double y0 = scores[bestS - 1 + Sc], y1 = scores[bestS + Sc], y2 = scores[bestS + 1 + Sc];
                const double denom = y0 - 2.0 * y1 + y2;
                if (std::fabs(denom) > 1e-9) {
                    const double off = 0.5 * (y0 - y2) / denom;
                    if (std::fabs(off) <= 1.0) {
                        refined += off;
                    }
                }
            }
            profile.measuredDeg[c] = static_cast<float>(refined * degPerRow);

            // ---- the confidence of this column's measurement ----------------
            // A column below minNcc keeps weight 0 and does not count as
            // accepted.
            if (best < params.minNcc) {
                continue;
            }
            accepted[c] = 1;
            // The second peak: the best scored shift clearly away from this one.
            // None at all means the peak could not be told apart from
            // anything (too few shifts scored): no distinctness.
            double second = kUnscored;
            for (int s = -Sc; s <= Sc; ++s) {
                if (std::abs(s - bestS) > kSeamDistinctGapRows && scores[s + Sc] > second) {
                    second = scores[s + Sc];
                }
            }
            const double distinct = second > kUnscored ? best - second : std::numeric_limits<double>::quiet_NaN();
            // Texture over the window, the smaller of the two lenses: a match
            // needs structure in both.
            double texture[2];
            for (int lens = 0; lens < 2; ++lens) {
                double sum = 0.0, count = 0.0;
                for (int dc = -win; dc <= win; ++dc) {
                    const std::size_t cc = static_cast<std::size_t>((static_cast<int>(c) + dc + W) % W);
                    sum += texSum[lens][cc];
                    count += texCount[lens][cc];
                }
                texture[lens] = count > 0.0 ? sum / count : 0.0;
            }
            // How much of the window the match rests on.
            const double coverage = static_cast<double>(covalidAtZero) / windowPixels;
            const double structure = std::min(texture[0], texture[1]);
            const double conf = smoothRamp(params.confNccLo, params.confNccHi, best) *
                                smoothRamp(params.confTextureLo, params.confTextureHi, structure) *
                                smoothRamp(params.confDistinctLo, params.confDistinctHi, distinct) *
                                smoothRamp(params.confCoverageLo, params.confCoverageHi, coverage);
            profile.confidence[c] = static_cast<float>(std::clamp(conf, 0.0, 1.0));
        }
    };
    if (pool != nullptr) {
        OSV_TRY(pool->parallelFor(0, b.w, 32, searchBody));
    } else {
        searchBody(0, b.w);
    }

    // ---- statistics ---------------------------------------------------------------------
    double nccSum = 0.0;
    double confSum = 0.0;
    for (std::uint32_t c = 0; c < b.w; ++c) {
        if (accepted[c]) {
            ++profile.acceptedColumns;
            nccSum += profile.ncc[c];
        }
        profile.unmeasuredColumns += unmeasured[c];
        profile.confidentColumns += profile.confidence[c] >= 0.5f ? 1u : 0u;
        confSum += profile.confidence[c];
    }
    profile.meanNcc = profile.acceptedColumns ? nccSum / profile.acceptedColumns : 0.0;
    profile.meanConfidence = confSum / static_cast<double>(b.w);
    if (profile.acceptedColumns == 0) {
        log::warn("searchSeam: no column reached NCC {}", params.minNcc);
    }

    // ---- the smoother's inputs -------------------------------------------------------------
    // A prior of the wrong length is a caller bug: ignored (0) with a warning
    // rather than failing the table.
    const bool priorUsable = seamPrior != nullptr && seamPrior->size() == b.w;
    if (seamPrior != nullptr && !priorUsable) {
        log::warn("searchSeam: seam prior has {} columns, the band {}; ignored", seamPrior->size(), b.w);
    }
    // Ring distance (columns) from every column to the nearest measured one:
    // two sweeps, each wrapping once around the ring.  Stays "far" (b.w)
    // when no column is measured at all.
    std::vector<std::uint32_t> toMeasured(b.w, b.w);
    for (int sweep = 0; sweep < 2; ++sweep) {
        std::uint32_t run = b.w;  // distance carried along the sweep
        for (std::uint32_t k = 0; k < 2 * b.w; ++k) {
            const std::uint32_t c = sweep == 0 ? k % b.w : (2 * b.w - 1 - k) % b.w;
            run = unmeasured[c] ? std::min(run + 1, b.w) : 0;
            toMeasured[c] = std::min(toMeasured[c], run);
        }
    }
    const auto inheritCols = static_cast<std::uint32_t>(
        std::lround(params.unmeasuredInheritDeg * static_cast<double>(b.w) / 360.0));

    const bool pinUnmeasured = std::isinf(params.unmeasuredPriorWeight);
    std::vector<double> m(b.w, 0.0), w(b.w, 0.0), mu(b.w, params.priorWeight), p(b.w, 0.0);
    std::vector<std::uint8_t> pinned(b.w, 0);
    for (std::uint32_t c = 0; c < b.w; ++c) {
        const float measured = profile.measuredDeg[c];
        w[c] = std::isfinite(measured) ? static_cast<double>(profile.confidence[c]) : 0.0;
        m[c] = std::isfinite(measured) ? static_cast<double>(measured) : 0.0;
        if (unmeasured[c]) {
            // Nothing measured it.  Within the inheritance margin it is free
            // (weight 0, the plain prior weight, prior 0), so a measured
            // neighbour's value fades into it; beyond, it is the calibrated
            // geometry (0) - never a value interpolated across the arc.
            if (toMeasured[c] > inheritCols) {
                pinned[c] = pinUnmeasured ? 1 : 0;
                mu[c] = pinUnmeasured ? 1.0 : params.unmeasuredPriorWeight;
            }
            continue;
        }
        if (priorUsable && std::isfinite((*seamPrior)[c])) {
            p[c] = static_cast<double>((*seamPrior)[c]);
        }
    }

    // ---- the robust smoother ---------------------------------------------------------------
    // The penalties are per column; scaling them by the column count keeps
    // the smoothness the same in degrees at any band width.
    const double scale = static_cast<double>(b.w) / 2048.0;
    const double lambda2 = params.smoothLambda2 * scale * scale * scale * scale;
    const double lambda1 = params.smoothLambda1 * scale * scale;
    OSV_TRY_ASSIGN(std::vector<double> table,
                   smoothSeamRing(m, w, mu, pinned, p, lambda2, lambda1, params.huberDeg, params.irlsIterations));
    for (std::uint32_t c = 0; c < b.w; ++c) {
        profile.shiftDeg[c] = static_cast<float>(table[c]);
    }
    return profile;
}

Result<GainEstimate> estimateGain(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, ThreadPool& pool,
                                  const LensShadingModel* shading) {
    // Render linear RGB bands (not just luma): we need per-channel means.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = static_cast<int>(band.equirectW);
    map.h = static_cast<int>(band.equirectW / 2);
    const double rowsPerDeg = static_cast<double>(map.h) / 180.0;
    const int halfRows = static_cast<int>(std::lround(band.bandHalfDeg * rowsPerDeg));
    const int centre = map.h / 2;
    const int row0 = std::max(0, centre - halfRows);
    const int row1 = std::min(map.h, centre + halfRows);

    if (row1 <= row0) {
        return Error{ErrorCode::InvalidArgument, "estimateGain: band has no rows"};
    }

    // Only the band rows are shaded (see shadeRows).  Alpha is each lens's own
    // production weight (coverage alpha, the builder default, stated here
    // because the trust test below reads it): FOV feather x occlusion ramp.
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
    std::vector<float> bandRgba[2];
    for (int lens = 0; lens < 2; ++lens) {
        RenderParamsBuilder builder;
        builder.rig(rig).equirect(map).blend(blend, true).color(cp).alphaCoverage(true).lensEnabled(1 - lens, false);
        // [WP-VIGNETTE] the lens as the kernel will blend it (null: raw).
        if (shading) {
            builder.shading(*shading, 1.0);
        }
        OSV_TRY_ASSIGN(RenderJob job, builder.build(frames));
        OSV_TRY_ASSIGN(bandRgba[lens], shadeRows(job, static_cast<std::uint32_t>(row0),
                                                  static_cast<std::uint32_t>(row1), pool));
    }

    GainEstimate g;
    double sum[2][3] = {{0, 0, 0}, {0, 0, 0}};
    std::uint64_t n = 0;
    const std::size_t rowFloats = static_cast<std::size_t>(map.w) * 4u;
    for (int r = row0; r < row1; ++r) {
        const float* a = bandRgba[0].data() + static_cast<std::size_t>(r - row0) * rowFloats;
        const float* b = bandRgba[1].data() + static_cast<std::size_t>(r - row0) * rowFloats;
        for (int c = 0; c < map.w; ++c) {
            // TRUSTED pixels only: both lenses at full weight (DJI's
            // alpha >= 0.99 rule).  With the analysis feather that keeps
            // theta below ~93.8 deg in both lenses and every pixel clear of
            // the occlusion ramp, so a lens's darkened rim (lens 0 on the
            // sample loses 1 stop at 95 deg and 4 past 97) never enters the
            // means.  The previous alpha > 0.5 test let that rim in and read
            // it as "lens 1 is too bright" (NEURAL_STITCHING.md 1.4).
            if (a[c * 4 + 3] >= kTrustedAlpha && b[c * 4 + 3] >= kTrustedAlpha) {
                for (int k = 0; k < 3; ++k) {
                    sum[0][k] += a[c * 4 + k];
                    sum[1][k] += b[c * 4 + k];
                }
                ++n;
            }
        }
    }
    g.samples = n;
    if (n < 64) {
        log::warn("estimateGain: only {} trusted co-visible pixels; gains left at 1", n);
        return g;
    }
    const double inv = 1.0 / static_cast<double>(n);
    double m0[3], m1[3];
    for (int k = 0; k < 3; ++k) {
        m0[k] = sum[0][k] * inv;
        m1[k] = sum[1][k] * inv;
    }
    g.overlapMean[0] = Vec3d{m0[0], m0[1], m0[2]};
    g.overlapMean[1] = Vec3d{m1[0], m1[1], m1[2]};
    double g0[3];
    for (int k = 0; k < 3; ++k) {
        // Symmetric split of the ratio; clamp so a black band cannot explode.
        double ratio = (m0[k] > 1e-6 && m1[k] > 1e-6) ? m1[k] / m0[k] : 1.0;
        g0[k] = clampd(std::sqrt(ratio), 0.5, 2.0);
    }
    g.gain[0] = Vec3d{g0[0], g0[1], g0[2]};
    g.gain[1] = Vec3d{1.0 / g0[0], 1.0 / g0[1], 1.0 / g0[2]};
    return g;
}

}  // namespace osv::render
