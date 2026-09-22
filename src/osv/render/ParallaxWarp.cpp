// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ParallaxWarp.cpp - measure the 2-D parallax across the overlap band and
// reduce it to the angular grid the kernel samples.
//
// The interesting steps, each explained at its site below:
//
//   1. the flow is HALVED, because both lenses move toward each other;
//   2. band pixels become angles through the polar-axis map's own scale, so
//      no new convention is introduced;
//   3. the cross-meridian component is smoothed harder than the
//      along-meridian one (anisotropic regularization);
//   4. the field is decayed to exactly zero at the grid's latitude edges, so
//      the correction cannot tear where the overlap ends;
//   5. last, every region must PROVE it helps: the finished field is applied
//      only in proportion to how much it reduces the lens-to-lens residual
//      (the benefit gate), because a self-consistent flow is not necessarily
//      a true one.

#include "osv/render/ParallaxWarp.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"
#include "osv/render/osv_kernel.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace osv::render {

namespace {

/// Smoothstep on [0, 1], clamped outside it.  Used for the decay ring:
/// a linear ramp to zero has a slope discontinuity at both of its ends, and
/// that shows as a faint band edge on smooth content - which is most of what
/// sits at the top and bottom of an overlap band.
[[nodiscard]] double smoothstep01(double t) noexcept {
    const double c = std::clamp(t, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

/// Separable Gaussian blur of one interleaved component of the grid.
///
/// Longitude wraps and latitude clamps, matching how the kernel samples the
/// same table - if the smoothing used different addressing than the fetch,
/// the two would disagree exactly at the wrap meridian.
void blurComponent(std::vector<float>& uv, std::uint32_t w, std::uint32_t h, int comp, double sigma) {
    if (sigma <= 0.0 || w == 0 || h == 0) {
        return;
    }
    const int radius = static_cast<int>(std::ceil(3.0 * sigma));
    if (radius < 1) {
        return;
    }
    std::vector<double> kernel(static_cast<std::size_t>(2 * radius + 1));
    double ksum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double k = std::exp(-0.5 * static_cast<double>(i * i) / (sigma * sigma));
        kernel[static_cast<std::size_t>(i + radius)] = k;
        ksum += k;
    }
    if (!(ksum > 0.0)) {
        return;
    }

    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<float> tmp(n, 0.0f);

    // Horizontal pass: longitude wraps.
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int xx = ((static_cast<int>(x) + i) % static_cast<int>(w) + static_cast<int>(w)) %
                               static_cast<int>(w);
                acc += static_cast<double>(uv[(static_cast<std::size_t>(y) * w + static_cast<std::uint32_t>(xx)) * 2u +
                                              static_cast<std::size_t>(comp)]) *
                       kernel[static_cast<std::size_t>(i + radius)];
            }
            tmp[static_cast<std::size_t>(y) * w + x] = static_cast<float>(acc / ksum);
        }
    }

    // Vertical pass: latitude clamps, because the band genuinely ends.
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            double acc = 0.0;
            for (int i = -radius; i <= radius; ++i) {
                const int yy = std::clamp(static_cast<int>(y) + i, 0, static_cast<int>(h) - 1);
                acc += static_cast<double>(tmp[static_cast<std::size_t>(yy) * w + x]) *
                       kernel[static_cast<std::size_t>(i + radius)];
            }
            uv[(static_cast<std::size_t>(y) * w + x) * 2u + static_cast<std::size_t>(comp)] =
                static_cast<float>(acc / ksum);
        }
    }
}

/// Validate the tuning block once, so every later step can assume it is sane.
[[nodiscard]] Status checkParams(const ParallaxWarpParams& p) {
    if (p.gridW < 8 || p.gridW > 8192) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: gridW out of range");
    }
    if (p.gridRows < 4 || p.gridRows > 4096) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: gridRows out of range");
    }
    if (p.decayRows > 4096) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: decayRows out of range");
    }
    if (!(p.crossMeridianScale >= 0.0) || p.crossMeridianScale > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: crossMeridianScale must be within 0..1");
    }
    if (!(p.maxCorrectionDeg > 0.0) || p.maxCorrectionDeg > 45.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: maxCorrectionDeg out of range");
    }
    if (!(p.minConsistentFraction >= 0.0) || p.minConsistentFraction > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: minConsistentFraction must be within 0..1");
    }
    if (!std::isfinite(p.crossMeridianSmooth) || p.crossMeridianSmooth < 0.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: crossMeridianSmooth must be >= 0");
    }
    if (!(p.requiredImprovement >= 0.0) || p.requiredImprovement >= 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: requiredImprovement must be within [0, 1)");
    }
    if (!(p.minResidual >= 0.0) || !std::isfinite(p.minResidual)) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: minResidual must be >= 0");
    }
    return okStatus();
}

/// Bilinear sample of a band plane at continuous PIXEL-CENTRE coordinates
/// (x, y) = (column + 0.5, row + 0.5) at the centre of a pixel.  Longitude
/// wraps, because the band is a ring; latitude clamps, because it ends.
[[nodiscard]] float sampleBand(const std::vector<float>& plane, std::uint32_t w, std::uint32_t h, double x,
                               double y) noexcept {
    if (w == 0 || h == 0 || plane.size() != static_cast<std::size_t>(w) * h || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return 0.0f;
    }
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    const double flx = std::floor(fx);
    const double fly = std::floor(fy);
    const double tx = fx - flx;
    const double ty = fy - fly;
    const int W = static_cast<int>(w);
    const int H = static_cast<int>(h);
    const int x0 = ((static_cast<int>(flx) % W) + W) % W;
    const int x1 = (x0 + 1) % W;
    const int y0 = std::clamp(static_cast<int>(fly), 0, H - 1);
    const int y1 = std::clamp(static_cast<int>(fly) + 1, 0, H - 1);
    const auto at = [&](int xx, int yy) {
        return static_cast<double>(plane[static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx)]);
    };
    const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const double bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return static_cast<float>(top + (bot - top) * ty);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Flow field -> angular grid
// ---------------------------------------------------------------------------
Result<ParallaxWarpGrid> gridFromFlow(const LensBands& bands, const BidirFlow& flow,
                                      const ParallaxWarpParams& params) {
    OSV_TRY(checkParams(params));
    if (bands.w == 0 || bands.h == 0 || bands.mapH == 0) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: empty band"};
    }
    if (!flow.valid() || flow.backward.w != flow.forward.w || flow.backward.h != flow.forward.h) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: invalid flow field"};
    }
    if (flow.forward.w != bands.w || flow.forward.h != bands.h) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: flow size does not match the band"};
    }
    const std::size_t bandPixels = static_cast<std::size_t>(bands.w) * static_cast<std::size_t>(bands.h);
    if (bands.alpha[0].size() != bandPixels || bands.alpha[1].size() != bandPixels) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: band coverage planes are the wrong size"};
    }
    if (static_cast<std::uint64_t>(bands.rowOffset) + bands.h > bands.mapH) {
        return Error{ErrorCode::InvalidArgument, "gridFromFlow: band lies outside its own map"};
    }

    const std::uint32_t gridW = params.gridW;
    const std::uint32_t gridRows = params.gridRows;
    const std::uint32_t decay = params.decayRows;

    ParallaxWarpGrid grid;
    grid.w = gridW;
    grid.h = gridRows + 2u * decay;
    grid.uv.assign(static_cast<std::size_t>(grid.w) * grid.h * 2u, 0.0f);

    // ---- band geometry -> angles ------------------------------------------
    // The band came from a polar-axis equirect map: its columns ARE
    // longitude and its rows ARE latitude, linearly.  So one band pixel is a
    // fixed angle in each direction and no new convention enters here.
    const double radPerRow = osv::kPi / static_cast<double>(bands.mapH);
    const double radPerCol = osv::kTwoPi / static_cast<double>(bands.w);

    // Latitude at the CENTRE of band row r.  The kernel samples pixel centres
    // (py + 0.5), and so does the grid lookup; measuring rows at their top
    // edge instead would bias the whole field by half a band row.
    const auto latOfBandRow = [&](double r) {
        return osv::kHalfPi - (static_cast<double>(bands.rowOffset) + r + 0.5) * radPerRow;
    };
    const double latTop = latOfBandRow(0.0);
    const double latBottom = latOfBandRow(static_cast<double>(bands.h) - 1.0);

    // Measured grid row k (0 .. gridRows-1) sits at latTop - k * latPerGridRow
    // and the decay rings extend the same spacing outward on both sides.  The
    // kernel maps latitude linearly from latMinRad (row 0) to latMaxRad
    // (row h-1); rows DESCEND in latitude, so latMinRad > latMaxRad
    // numerically and the kernel's signed span takes care of it.
    const double latPerGridRow = (latTop - latBottom) / static_cast<double>(gridRows - 1);
    grid.latMinRad = static_cast<float>(latTop + latPerGridRow * static_cast<double>(decay));
    grid.latMaxRad = static_cast<float>(latBottom - latPerGridRow * static_cast<double>(decay));

    const double maxRad = params.maxCorrectionDeg * osv::kPi / 180.0;

    // ---- accumulate the flow into the grid cells ---------------------------
    // Each cell averages the SYMMETRIC flow 0.5 * (forward - backward) over
    // the band pixels nearest to it.
    //
    // Why symmetric: forward(q) is measured at q in lens 0's band, and that
    // content sits at q + f/2 in the middle image the kernel produces;
    // backward(q) is measured at q in lens 1's band, and that content sits at
    // q - f/2.  Averaging the two cancels the half-disparity offset to first
    // order, which is exactly the estimate wanted for the MIDDLE position,
    // i.e. the output pixel the grid is sampled at.
    //
    // Only co-visible pixels count (one lens alone says nothing about
    // parallax), and only those whose flow passed the forward-backward check:
    // a repaired vector is plausible but not measured, and it must not
    // outvote a neighbour that was.
    const std::size_t cells = static_cast<std::size_t>(gridW) * gridRows;
    std::vector<double> accLon(cells, 0.0);
    std::vector<double> accLat(cells, 0.0);
    std::vector<double> accN(cells, 0.0);
    std::uint64_t consistent = 0;
    std::uint64_t covisible = 0;
    const double colsPerCell = static_cast<double>(bands.w) / static_cast<double>(gridW);
    const double rowsPerCell =
        bands.h > 1 ? static_cast<double>(bands.h - 1) / static_cast<double>(gridRows - 1) : 1.0;

    for (std::uint32_t r = 0; r < bands.h; ++r) {
        // Nearest measured grid row to this band row's centre.
        int gr = static_cast<int>(std::lround(static_cast<double>(r) / rowsPerCell));
        gr = std::clamp(gr, 0, static_cast<int>(gridRows) - 1);
        for (std::uint32_t c = 0; c < bands.w; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * bands.w + c;
            if (!(bands.alpha[0][i] > 0.5f) || !(bands.alpha[1][i] > 0.5f)) {
                continue;
            }
            ++covisible;
            if (flow.ok[i] == 0u) {
                continue;
            }
            const double fu =
                0.5 * (static_cast<double>(flow.forward.u[i]) - static_cast<double>(flow.backward.u[i]));
            const double fv =
                0.5 * (static_cast<double>(flow.forward.v[i]) - static_cast<double>(flow.backward.v[i]));
            if (!std::isfinite(fu) || !std::isfinite(fv)) {
                continue;
            }
            ++consistent;
            // Nearest grid column to this band column's centre.  The kernel
            // reads cell gc at longitude fraction gc / gridW, which is band
            // position gc * colsPerCell; ROUNDING keeps each cell centred on
            // the position it is read back at.  Flooring - the first version
            // - put every cell half a cell (0.7 degrees at 256 columns) east
            // of where it had been measured.
            int gc = static_cast<int>(std::lround((static_cast<double>(c) + 0.5) / colsPerCell));
            gc = ((gc % static_cast<int>(gridW)) + static_cast<int>(gridW)) % static_cast<int>(gridW);
            const std::size_t gi = static_cast<std::size_t>(gr) * gridW + static_cast<std::size_t>(gc);
            // Pixels -> the MASTER lens's half displacement in radians.
            //
            // The master (lens 1) must sample at q + f/2 and the slave at
            // q - f/2.  A column step is +longitude; a ROW step is -latitude
            // (row 0 is the +90 pole), hence the minus on dLat.
            accLon[gi] += 0.5 * fu * radPerCol;
            accLat[gi] += -0.5 * fv * radPerRow;
            accN[gi] += 1.0;
        }
    }
    grid.consistentPixels = consistent;
    grid.totalPixels = covisible;

    // ---- per-cell averages, optional cross-meridian scale, safety clamp ---
    // Along the meridian (dLat) is the epipolar direction of a back-to-back
    // pair and is always taken at face value.  Across it (dLon) the value can
    // be scaled down by crossMeridianScale - 1 by default, because on the
    // sample clip the cross-meridian misalignment turned out to be real (see
    // that parameter); the benefit gate below is what rejects a mismatch.
    std::vector<float> measured(cells * 2u, 0.0f);
    for (std::size_t gi = 0; gi < cells; ++gi) {
        if (!(accN[gi] > 0.0)) {
            continue;
        }
        const double lonv = (accLon[gi] / accN[gi]) * params.crossMeridianScale;
        const double latv = accLat[gi] / accN[gi];
        measured[gi * 2u + 0u] = static_cast<float>(std::clamp(lonv, -maxRad, maxRad));
        measured[gi * 2u + 1u] = static_cast<float>(std::clamp(latv, -maxRad, maxRad));
    }

    // ---- fill cells nothing measured -------------------------------------
    // An empty cell left at zero is a claim that there is no parallax there,
    // and a zero island inside a corrected region tears exactly like an
    // unwarped one.  First along longitude within the row (the ring wraps),
    // then whole empty rows from the nearest row that has data - which is
    // what happens at the band's top and bottom, where one lens's coverage
    // has already dropped below the co-visibility threshold.
    std::vector<std::uint8_t> rowHasData(gridRows, 0u);
    for (std::uint32_t gr = 0; gr < gridRows; ++gr) {
        const std::size_t base = static_cast<std::size_t>(gr) * gridW;
        for (std::uint32_t gc = 0; gc < gridW; ++gc) {
            if (accN[base + gc] > 0.0) {
                rowHasData[gr] = 1u;
                break;
            }
        }
        if (!rowHasData[gr]) {
            continue;
        }
        for (std::uint32_t gc = 0; gc < gridW; ++gc) {
            if (accN[base + gc] > 0.0) {
                continue;
            }
            for (std::uint32_t d = 1; d < gridW; ++d) {
                const std::uint32_t left = (gc + gridW - d) % gridW;
                const std::uint32_t right = (gc + d) % gridW;
                std::uint32_t src = gridW;  // sentinel: nothing found yet
                if (accN[base + left] > 0.0) {
                    src = left;
                } else if (accN[base + right] > 0.0) {
                    src = right;
                }
                if (src != gridW) {
                    measured[(base + gc) * 2u + 0u] = measured[(base + src) * 2u + 0u];
                    measured[(base + gc) * 2u + 1u] = measured[(base + src) * 2u + 1u];
                    break;
                }
            }
        }
    }
    for (std::uint32_t gr = 0; gr < gridRows; ++gr) {
        if (rowHasData[gr]) {
            continue;
        }
        // Nearest row with data, either direction.  None at all leaves the
        // row at zero, and the consistency gate in buildParallaxWarp rejects
        // such a field long before it reaches a render.
        for (std::uint32_t d = 1; d < gridRows; ++d) {
            const int up = static_cast<int>(gr) - static_cast<int>(d);
            const int dn = static_cast<int>(gr) + static_cast<int>(d);
            int src = -1;
            if (up >= 0 && rowHasData[static_cast<std::size_t>(up)]) {
                src = up;
            } else if (dn < static_cast<int>(gridRows) && rowHasData[static_cast<std::size_t>(dn)]) {
                src = dn;
            }
            if (src >= 0) {
                std::copy_n(measured.begin() +
                                static_cast<std::ptrdiff_t>(static_cast<std::size_t>(src) * gridW * 2u),
                            gridW * 2u,
                            measured.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(gr) * gridW * 2u));
                break;
            }
            if (up < 0 && dn >= static_cast<int>(gridRows)) {
                break;
            }
        }
    }

    // ---- place into the full grid, extending the edges into the rings ------
    // The decay rings start as COPIES of the nearest measured row, not as
    // zeros.  Blurring a field that already has zeros in its rings drags the
    // measured edge rows toward zero and makes the ramp much steeper than
    // decayRows asks for - which the first version did; extending first and
    // ramping afterwards gives the full ring width to the fall-off.
    for (std::uint32_t gy = 0; gy < grid.h; ++gy) {
        const int k = std::clamp(static_cast<int>(gy) - static_cast<int>(decay), 0, static_cast<int>(gridRows) - 1);
        std::copy_n(measured.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(k) * gridW * 2u),
                    gridW * 2u,
                    grid.uv.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(gy) * gridW * 2u));
    }

    // ---- anisotropic smoothing --------------------------------------------
    // The cross-meridian component is smoothed harder than the along-meridian
    // one: it is the less trustworthy of the two (the epipolar direction is
    // along the meridian), and forcing it to agree with its neighbours is how
    // that distrust is expressed without discarding its amplitude.  The
    // light blur on dLat removes cell-to-cell noise, which would otherwise
    // show as a fine ripple once it becomes a rotation.
    blurComponent(grid.uv, grid.w, grid.h, 0, params.crossMeridianSmooth);
    blurComponent(grid.uv, grid.w, grid.h, 1, 1.0);

    // ---- boundary decay ----------------------------------------------------
    // The correction must reach EXACTLY zero at the outermost grid rows,
    // because the kernel returns zero for any ray beyond them; anything else
    // is a step at the band edge, and a corrected seam with a torn edge is
    // worse than no correction.  Applied AFTER smoothing so nothing can pull
    // a non-zero value back into the edge rows.
    //
    // Ring row j (0 = outermost) scales by smoothstep(j / decay): exactly 0
    // at the edge, rising to 1 at the first measured row.
    if (decay > 0) {
        for (std::uint32_t gy = 0; gy < grid.h; ++gy) {
            std::uint32_t j = decay;  // distance in rows from the outer edge, capped at `decay`
            if (gy < decay) {
                j = gy;
            } else if (gy >= decay + gridRows) {
                j = grid.h - 1u - gy;
            }
            if (j >= decay) {
                continue;
            }
            const double scale = smoothstep01(static_cast<double>(j) / static_cast<double>(decay));
            float* row = grid.uv.data() + static_cast<std::size_t>(gy) * gridW * 2u;
            for (std::uint32_t k = 0; k < gridW * 2u; ++k) {
                row[k] = static_cast<float>(static_cast<double>(row[k]) * scale);
            }
        }
    }

    // ---- benefit gate: never apply a correction that does not help --------
    // See ParallaxWarpParams::requiredImprovement for why the consistency
    // check alone is not enough.
    //
    // WHAT IS TESTED IS WHAT WILL BE RENDERED.  The gate runs on the FINAL
    // field - after the anisotropic blur and the decay ring - and samples it
    // at every co-visible band pixel through osvWarpSample, the kernel's own
    // lookup.  An earlier version judged each cell's raw mean displacement
    // before the blur; on smooth parallax the two are the same, but where the
    // flow is wild (two lenses seeing different objects) the blurred,
    // interpolated field the kernel applies is NOT the one that was judged,
    // and cells that passed their own test still made the picture worse.
    //
    // For each pixel p with displacement d(p) (half correction, band
    // pixels) the warped residual is |slave(p - d) - master(p + d)|, and it
    // is compared with a NULL residual at the SAME interpolation phases -
    // both lenses at p - d, and both at p + d, averaged.  Comparing against
    // the plain unwarped |slave(p) - master(p)| instead is biased: samples
    // that land between pixels are bilinear averages with lower contrast,
    // and lower contrast shrinks the residual whether or not the
    // displacement is right.  Measured on two unrelated textures, that bias
    // let about half of a meaningless correction through.
    //
    // The per-cell sums are pooled over a 3 x 3 neighbourhood before the
    // decision: one cell averages a few dozen pixels, and its ratio scatters
    // by ~10% on content with nothing in common - enough for chance to buy a
    // visible weight.  The resulting weights are smoothed before they scale
    // the field, so the gate cannot introduce a step of its own, and they
    // MULTIPLY a field whose edge rows are already exactly zero, so the
    // boundary guarantee above survives the gate.
    std::uint32_t measuredCells = 0;
    std::uint32_t gatedCells = 0;
    for (std::size_t gi = 0; gi < cells; ++gi) {
        if (accN[gi] > 0.0) {
            ++measuredCells;
        }
    }
    if (params.requiredImprovement > 0.0) {
        if (bands.luma[0].size() != bandPixels || bands.luma[1].size() != bandPixels) {
            return Error{ErrorCode::InvalidArgument, "gridFromFlow: band luma planes are the wrong size"};
        }
        // The kernel's view of this grid, so osvWarpSample reads it exactly
        // as the renderer will.
        OsvRenderParams kp{};
        kp.warpEnabled = 1;
        kp.warpW = static_cast<int>(grid.w);
        kp.warpH = static_cast<int>(grid.h);
        kp.warpLatMinRad = grid.latMinRad;
        kp.warpLatMaxRad = grid.latMaxRad;

        std::vector<double> errNull(cells, 0.0);
        std::vector<double> errWarped(cells, 0.0);
        std::vector<double> errN(cells, 0.0);
        for (std::uint32_t r = 0; r < bands.h; ++r) {
            int gr = static_cast<int>(std::lround(static_cast<double>(r) / rowsPerCell));
            gr = std::clamp(gr, 0, static_cast<int>(gridRows) - 1);
            const float lat = static_cast<float>(latOfBandRow(static_cast<double>(r)));
            for (std::uint32_t c = 0; c < bands.w; ++c) {
                const std::size_t i = static_cast<std::size_t>(r) * bands.w + c;
                if (!(bands.alpha[0][i] > 0.5f) || !(bands.alpha[1][i] > 0.5f)) {
                    continue;
                }
                int gc = static_cast<int>(std::lround((static_cast<double>(c) + 0.5) / colsPerCell));
                gc = ((gc % static_cast<int>(gridW)) + static_cast<int>(gridW)) % static_cast<int>(gridW);
                const std::size_t gi = static_cast<std::size_t>(gr) * gridW + static_cast<std::size_t>(gc);

                // The displacement the kernel will apply at this pixel, in
                // band pixels.  A longitude step is a column step; a latitude
                // step is a NEGATIVE row step (row 0 is the +90 pole).
                const float lon =
                    static_cast<float>((static_cast<double>(c) + 0.5) * radPerCol - osv::kPi);
                const double dLon = osvWarpSample(&kp, grid.uv.data(), lon, lat, 0);
                const double dLat = osvWarpSample(&kp, grid.uv.data(), lon, lat, 1);
                const double dx = dLon / radPerCol;
                const double dy = -dLat / radPerRow;
                const double x = static_cast<double>(c) + 0.5;
                const double y = static_cast<double>(r) + 0.5;
                const double slaveMinus = sampleBand(bands.luma[0], bands.w, bands.h, x - dx, y - dy);
                const double masterPlus = sampleBand(bands.luma[1], bands.w, bands.h, x + dx, y + dy);
                const double slavePlus = sampleBand(bands.luma[0], bands.w, bands.h, x + dx, y + dy);
                const double masterMinus = sampleBand(bands.luma[1], bands.w, bands.h, x - dx, y - dy);
                errNull[gi] += 0.5 * (std::fabs(slaveMinus - masterMinus) + std::fabs(slavePlus - masterPlus));
                errWarped[gi] += std::fabs(slaveMinus - masterPlus);
                errN[gi] += 1.0;
            }
        }

        // 3 x 3 pooling (longitude wraps, latitude clamps).
        const auto pooled = [&](const std::vector<double>& v, std::size_t gi) {
            const int gr = static_cast<int>(gi / gridW);
            const int gc = static_cast<int>(gi % gridW);
            double sum = 0.0;
            for (int dr = -1; dr <= 1; ++dr) {
                const int rr = std::clamp(gr + dr, 0, static_cast<int>(gridRows) - 1);
                for (int dc = -1; dc <= 1; ++dc) {
                    const int cc =
                        ((gc + dc) % static_cast<int>(gridW) + static_cast<int>(gridW)) % static_cast<int>(gridW);
                    sum += v[static_cast<std::size_t>(rr) * gridW + static_cast<std::size_t>(cc)];
                }
            }
            return sum;
        };

        // Per-cell weight: ratio 1 - requiredImprovement or better -> 1;
        // 1 - requiredImprovement / 4 or worse -> 0 (a small dead zone for
        // the scatter that pooling does not remove); smoothstep between.  A
        // cell with nothing measurable to fix (null residual below
        // minResidual) gets 0: there is nothing there to justify bending it.
        const double zeroAt = 1.0 - 0.25 * params.requiredImprovement;
        const double fullAt = 1.0 - params.requiredImprovement;
        std::vector<float> weight(cells, 0.0f);
        for (std::size_t gi = 0; gi < cells; ++gi) {
            const double n = pooled(errN, gi);
            double w = 0.0;
            if (n > 0.0) {
                const double eu = pooled(errNull, gi) / n;
                const double ew = pooled(errWarped, gi) / n;
                if (eu >= params.minResidual) {
                    w = smoothstep01((zeroAt - ew / eu) / (zeroAt - fullAt));
                }
            }
            weight[gi] = static_cast<float>(w);
            if (accN[gi] > 0.0 && w <= 0.0) {
                ++gatedCells;
            }
        }

        // Spread the weights over the full grid (the rings take the weight of
        // the nearest measured row), smooth them, and scale the field.
        std::vector<float> wGrid(static_cast<std::size_t>(grid.w) * grid.h * 2u, 0.0f);
        for (std::uint32_t gy = 0; gy < grid.h; ++gy) {
            const int k = std::clamp(static_cast<int>(gy) - static_cast<int>(decay), 0, static_cast<int>(gridRows) - 1);
            for (std::uint32_t gc = 0; gc < gridW; ++gc) {
                wGrid[(static_cast<std::size_t>(gy) * gridW + gc) * 2u] =
                    weight[static_cast<std::size_t>(k) * gridW + gc];
            }
        }
        blurComponent(wGrid, grid.w, grid.h, 0, 1.0);
        for (std::size_t k = 0; k < static_cast<std::size_t>(grid.w) * grid.h; ++k) {
            const float w = std::clamp(wGrid[k * 2u], 0.0f, 1.0f);
            grid.uv[k * 2u + 0u] *= w;
            grid.uv[k * 2u + 1u] *= w;
        }
    }
    grid.measuredCells = measuredCells;
    grid.gatedCells = gatedCells;

    // ---- diagnostics -------------------------------------------------------
    double sumAbs = 0.0;
    double maxAbs = 0.0;
    std::size_t counted = 0;
    for (std::uint32_t gy = decay; gy < decay + gridRows; ++gy) {
        const float* row = grid.uv.data() + static_cast<std::size_t>(gy) * gridW * 2u;
        for (std::uint32_t gc = 0; gc < gridW; ++gc) {
            const double mag =
                std::hypot(static_cast<double>(row[gc * 2u]), static_cast<double>(row[gc * 2u + 1u]));
            sumAbs += mag;
            maxAbs = std::max(maxAbs, mag);
            ++counted;
        }
    }
    // Reported as the FULL disparity between the lenses (twice the stored
    // half), because that is the number comparable to the seam table and to
    // what a viewer sees as the size of the double image.
    grid.meanAbsCorrectionDeg = counted ? 2.0 * osv::rad2deg(sumAbs / static_cast<double>(counted)) : 0.0;
    grid.maxAbsCorrectionDeg = 2.0 * osv::rad2deg(maxAbs);
    return grid;
}

// ---------------------------------------------------------------------------
//  Full measurement
// ---------------------------------------------------------------------------
Result<ParallaxWarpGrid> buildParallaxWarp(const geom::LensRig& rig, const video::FramePair& frames,
                                           const geom::BlendParams& blend, const ParallaxWarpParams& params,
                                           const std::vector<float>* seamTable, ThreadPool& pool) {
    OSV_TRY(checkParams(params));

    // Render the two per-lens bands.  Passing the seam table through means
    // the flow measures the RESIDUAL parallax the 1-D correction left behind,
    // so the two mechanisms compose instead of both claiming the same
    // disparity and over-correcting it.
    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    const auto tBand = Clock::now();
    OSV_TRY_ASSIGN(LensBands bands, renderLensBands(rig, frames, blend, params.band, false, seamTable, pool));
    const double bandMs = msSince(tBand);
    if (bands.w == 0 || bands.h == 0) {
        return Error{ErrorCode::Internal, "buildParallaxWarp: band render produced nothing"};
    }

    // The band luma planes are already exactly the shape the flow solver
    // wants, so there is no conversion here beyond wrapping them.
    GrayImage a;
    a.w = bands.w;
    a.h = bands.h;
    a.data = bands.luma[0];
    GrayImage b;
    b.w = bands.w;
    b.h = bands.h;
    b.data = bands.luma[1];
    if (!a.valid() || !b.valid()) {
        return Error{ErrorCode::Internal, "buildParallaxWarp: band planes are malformed"};
    }

    FlowBackendKind used = params.backend;
    const auto tFlow = Clock::now();
    OSV_TRY_ASSIGN(BidirFlow flow, computeFlow(params.backend, a, b, params.flow, &pool, &used));
    const double flowMs = msSince(tFlow);

    const auto tGrid = Clock::now();
    OSV_TRY_ASSIGN(ParallaxWarpGrid grid, gridFromFlow(bands, flow, params));
    grid.usedBackend = used;
    grid.bandMs = bandMs;
    grid.flowMs = flowMs;
    grid.gridMs = msSince(tGrid);

    // Refuse to act on a measurement that mostly failed its own consistency
    // check.  On featureless content - open sky, water - the solver has
    // nothing to lock onto and most of the field is repaired guesses;
    // applying that is worse than applying nothing.
    if (grid.consistentFraction() < params.minConsistentFraction) {
        return Error{ErrorCode::Unsupported, "parallax flow too inconsistent to use"};
    }
    return grid;
}

}  // namespace osv::render
