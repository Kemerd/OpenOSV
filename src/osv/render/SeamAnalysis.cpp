// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/render/SeamAnalysis.h"
#include "osv/color/ColorParams.h"
#include "osv/core/Log.h"
#include "osv/geom/EquirectMap.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/RenderParamsBuilder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace osv::render {

namespace {

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

}  // namespace

Result<LensBands> renderLensBands(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, bool linear,
                                  const std::vector<float>* seamTable, ThreadPool& pool, const WarpGridView* warp) {
    if (band.equirectW < 64 || band.equirectW > 16384 || band.bandHalfDeg <= 0.0 || band.bandHalfDeg > 45.0) {
        return Error{ErrorCode::InvalidArgument, "renderLensBands: bad band parameters"};
    }
    // Full polar-axis map; we only keep the band rows but rendering the whole
    // map is cheap at 2048 wide and keeps the mapping identical to production.
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
    CpuRenderer cpu(pool);
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
        OSV_TRY_ASSIGN(ImageRGBAf img, cpu.render(job));
        out.luma[lens].resize(static_cast<std::size_t>(out.w) * out.h);
        out.alpha[lens].resize(out.luma[lens].size());
        for (std::uint32_t r = 0; r < out.h; ++r) {
            const float* src = img.row(row0 + r);
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
                               const geom::BlendParams& blend, const SeamSearchParams& params, ThreadPool& pool) {
    if (params.maxShiftPx <= 0 || params.windowHalfCols < 0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: bad parameters"};
    }
    // Render a band wide enough to hold the search range on both sides.
    BandParams band = params.band;
    const double rowsPerDeg = static_cast<double>(band.equirectW / 2) / 180.0;
    band.bandHalfDeg += static_cast<double>(params.maxShiftPx) / rowsPerDeg;
    OSV_TRY_ASSIGN(LensBands b, renderLensBands(rig, frames, blend, band, false, nullptr, pool));

    const int W = static_cast<int>(b.w);
    const int H = static_cast<int>(b.h);
    const int S = params.maxShiftPx;
    const int win = params.windowHalfCols;
    // Rows of the inner (un-padded) band.
    const int inner0 = S;
    const int inner1 = H - S;
    if (inner1 <= inner0) {
        return Error{ErrorCode::InvalidArgument, "searchSeam: band too small for the shift range"};
    }

    SeamProfile profile;
    profile.columns = b.w;
    profile.shiftDeg.assign(b.w, 0.0f);
    profile.ncc.assign(b.w, 0.0f);
    std::vector<float> rawShift(b.w, std::numeric_limits<float>::quiet_NaN());

    // Per-column search, parallel over columns.
    Status st = pool.parallelFor(0, b.w, 32, [&](std::size_t c0, std::size_t c1) {
        std::vector<float> va, vb;
        std::vector<std::uint8_t> mask;
        for (std::size_t c = c0; c < c1; ++c) {
            double best = -2.0;
            int bestS = 0;
            double scores[2 * 64 + 1];  // S is capped below
            const int Sc = std::min(S, 64);
            for (int s = -Sc; s <= Sc; ++s) {
                va.clear();
                vb.clear();
                mask.clear();
                for (int r = inner0; r < inner1; ++r) {
                    for (int dc = -win; dc <= win; ++dc) {
                        const int cc = (static_cast<int>(c) + dc + W) % W;  // wrap around longitude
                        const std::size_t ia = static_cast<std::size_t>(r) * b.w + cc;
                        const std::size_t ib = static_cast<std::size_t>(r + s) * b.w + cc;
                        va.push_back(b.luma[0][ia]);
                        vb.push_back(b.luma[1][ib]);
                        mask.push_back((b.alpha[0][ia] > 0.5f && b.alpha[1][ib] > 0.5f) ? 1 : 0);
                    }
                }
                const double score = nccMasked(va.data(), vb.data(), mask.data(), va.size());
                scores[s + Sc] = score;
                if (score > best) {
                    best = score;
                    bestS = s;
                }
            }
            profile.ncc[c] = static_cast<float>(best);
            if (best >= params.minNcc) {
                // Parabolic sub-pixel refinement around the peak.
                double refined = bestS;
                if (bestS > -Sc && bestS < Sc) {
                    const double y0 = scores[bestS - 1 + Sc], y1 = scores[bestS + Sc], y2 = scores[bestS + 1 + Sc];
                    const double denom = y0 - 2.0 * y1 + y2;
                    if (std::fabs(denom) > 1e-9) {
                        const double off = 0.5 * (y0 - y2) / denom;
                        if (std::fabs(off) <= 1.0) {
                            refined += off;
                        }
                    }
                }
                rawShift[c] = static_cast<float>(refined);
            }
        }
    });
    OSV_TRY(st);

    // Fill rejected columns from their nearest accepted neighbours (wrap).
    std::uint32_t accepted = 0;
    double nccSum = 0.0;
    for (std::uint32_t c = 0; c < b.w; ++c) {
        if (!std::isnan(rawShift[c])) {
            ++accepted;
            nccSum += profile.ncc[c];
        }
    }
    profile.acceptedColumns = accepted;
    profile.meanNcc = accepted ? nccSum / accepted : 0.0;
    if (accepted == 0) {
        log::warn("searchSeam: no column reached NCC {}", params.minNcc);
        return profile;  // all-zero table = no correction
    }
    std::vector<float> filled(b.w, 0.0f);
    for (std::uint32_t c = 0; c < b.w; ++c) {
        if (!std::isnan(rawShift[c])) {
            filled[c] = rawShift[c];
            continue;
        }
        // nearest accepted column in either direction
        for (std::uint32_t d = 1; d < b.w; ++d) {
            const std::uint32_t left = (c + b.w - d) % b.w;
            const std::uint32_t right = (c + d) % b.w;
            if (!std::isnan(rawShift[left])) {
                filled[c] = rawShift[left];
                break;
            }
            if (!std::isnan(rawShift[right])) {
                filled[c] = rawShift[right];
                break;
            }
        }
    }

    // Gaussian smoothing along longitude with wrap-around.
    const double sigma = std::max(params.smoothSigmaCols, 0.0);
    std::vector<float> smoothed(b.w, 0.0f);
    if (sigma < 0.5) {
        smoothed = filled;
    } else {
        const int radius = static_cast<int>(std::ceil(3.0 * sigma));
        std::vector<double> kernel(static_cast<std::size_t>(2 * radius + 1));
        double ksum = 0.0;
        for (int i = -radius; i <= radius; ++i) {
            const double k = std::exp(-0.5 * (i * i) / (sigma * sigma));
            kernel[static_cast<std::size_t>(i + radius)] = k;
            ksum += k;
        }
        for (std::uint32_t c = 0; c < b.w; ++c) {
            double acc = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const std::uint32_t cc = (c + b.w + static_cast<std::uint32_t>((i + static_cast<int>(b.w)) % static_cast<int>(b.w))) % b.w;
                acc += filled[cc] * kernel[static_cast<std::size_t>(i + radius)];
            }
            smoothed[c] = static_cast<float>(acc / ksum);
        }
    }

    // Rows -> degrees.  Positive row shift = lens 1 content lower in the band
    // = feature farther from both axes (see osv_kernel.h).
    const float degPerRow = static_cast<float>(180.0 / static_cast<double>(b.mapH));
    for (std::uint32_t c = 0; c < b.w; ++c) {
        profile.shiftDeg[c] = smoothed[c] * degPerRow;
    }
    return profile;
}

Result<GainEstimate> estimateGain(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, ThreadPool& pool) {
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

    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
    CpuRenderer cpu(pool);
    ImageRGBAf imgs[2];
    for (int lens = 0; lens < 2; ++lens) {
        RenderParamsBuilder builder;
        builder.rig(rig).equirect(map).blend(blend, true).color(cp).lensEnabled(1 - lens, false);
        OSV_TRY_ASSIGN(RenderJob job, builder.build(frames));
        OSV_TRY_ASSIGN(imgs[lens], cpu.render(job));
    }

    GainEstimate g;
    double sum[2][3] = {{0, 0, 0}, {0, 0, 0}};
    std::uint64_t n = 0;
    for (int r = row0; r < row1; ++r) {
        const float* a = imgs[0].row(static_cast<std::uint32_t>(r));
        const float* b = imgs[1].row(static_cast<std::uint32_t>(r));
        for (int c = 0; c < map.w; ++c) {
            if (a[c * 4 + 3] > 0.5f && b[c * 4 + 3] > 0.5f) {
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
        log::warn("estimateGain: only {} co-visible pixels; gains left at 1", n);
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
