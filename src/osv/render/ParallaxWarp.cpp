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
#include <functional>

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

/// Run `body(task)` for every task in [0, count), across `pool` when there is
/// one worth using, otherwise inline.
///
/// Returns false only when a parallel run failed part-way; the caller then
/// clears whatever the bodies accumulate into and runs every task itself.
/// (Bodies that only overwrite their own output can simply be rerun.)
[[nodiscard]] bool forTasks(ThreadPool* pool, std::size_t count, const std::function<void(std::size_t)>& body) {
    if (pool != nullptr && pool->size() > 1 && count > 1) {
        return pool->parallelRows(count, 1, body).ok();
    }
    for (std::size_t t = 0; t < count; ++t) {
        body(t);
    }
    return true;
}

/// Multiply-adds per blur pass below which blurComponent stays on the
/// calling thread: waking the pool costs tens of microseconds, which a small
/// blur never earns back.
constexpr double kBlurParallelMacs = 4.0e6;

/// Separable Gaussian blur of one interleaved component of the grid.
///
/// Longitude wraps and latitude clamps, matching how the kernel samples the
/// same table - if the smoothing used different addressing than the fetch,
/// the two would disagree exactly at the wrap meridian.
///
/// Restructured for speed without changing a bit of the result: the wrapped
/// columns are stepped round the ring instead of recomputed with two integer
/// modulos per tap, and both passes run tap-major (see below), so every
/// output sums the same products in the same order as the original
/// output-major loop.  Both passes write whole output rows from inputs
/// nobody writes during the pass, so `pool` (optional) splits large blurs by
/// row, again with bit-identical results.
void blurComponent(std::vector<float>& uv, std::uint32_t w, std::uint32_t h, int comp, double sigma,
                   ThreadPool* pool) {
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
    const int W = static_cast<int>(w);
    const std::size_t taps = static_cast<std::size_t>(2 * radius + 1);

    // Both passes run tap-major: every output of a row advances by one tap
    // before any advances by the next.  Each output still accumulates
    // acc += value * kernel[tap] for taps -radius .. +radius in that order,
    // starting from 0.0 - exactly the sum the output-major loop formed - but
    // the outputs are now independent operations the compiler can run side by
    // side, instead of one long chain of dependent double additions.

    // Horizontal pass: longitude wraps.  The row is first unrolled onto a
    // padded line (radius columns of wrap each side, already widened to
    // double - an exact conversion), so every tap reads a contiguous run.
    const auto horizontal = [&](std::size_t y) {
        const float* row = uv.data() + y * w * 2u + static_cast<std::size_t>(comp);
        std::vector<double> line(static_cast<std::size_t>(w) + taps - 1u);
        std::vector<double> acc(w, 0.0);
        // Column of line[0] is -radius, by the original wrap expression;
        // each later entry is the next column round the ring.
        int xx = ((0 - radius) % W + W) % W;
        for (double& value : line) {
            value = static_cast<double>(row[static_cast<std::size_t>(xx) * 2u]);
            if (++xx == W) {
                xx = 0;
            }
        }
        for (std::size_t t = 0; t < taps; ++t) {
            const double k = kernel[t];
            const double* src = line.data() + t;
            for (std::uint32_t x = 0; x < w; ++x) {
                acc[x] += src[x] * k;
            }
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            tmp[y * w + x] = static_cast<float>(acc[x] / ksum);
        }
    };
    // Vertical pass: latitude clamps, because the band genuinely ends.
    const auto vertical = [&](std::size_t y) {
        std::vector<double> acc(w, 0.0);
        for (int i = -radius; i <= radius; ++i) {
            const int yy = std::clamp(static_cast<int>(y) + i, 0, static_cast<int>(h) - 1);
            const double k = kernel[static_cast<std::size_t>(i + radius)];
            const float* src = tmp.data() + static_cast<std::size_t>(yy) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                acc[x] += static_cast<double>(src[x]) * k;
            }
        }
        for (std::uint32_t x = 0; x < w; ++x) {
            uv[(y * w + x) * 2u + static_cast<std::size_t>(comp)] = static_cast<float>(acc[x] / ksum);
        }
    };
    // Each pass only overwrites its own rows, so a failed parallel run is
    // simply redone on this thread.  A grid-sized blur (256 x 48 at the
    // defaults) is a few hundred thousand multiply-adds - less than the cost
    // of waking a pool - so the pool is only used for a much larger one.
    ThreadPool* const passPool =
        static_cast<double>(n) * static_cast<double>(2 * radius + 1) >= kBlurParallelMacs ? pool : nullptr;
    if (!forTasks(passPool, h, horizontal)) {
        (void)forTasks(nullptr, h, horizontal);
    }
    if (!forTasks(passPool, h, vertical)) {
        (void)forTasks(nullptr, h, vertical);
    }
}

/// Column blocks each grid row's per-pixel work is split into when a pool
/// is available.  One task per grid row left the gate pass unbalanced (a
/// grid row gets two or three band rows) and 32 tasks cannot keep a large
/// pool busy; four blocks per row gives 128 even tasks at the default size.
constexpr std::uint32_t kCellBlocksPerRow = 4;

/// The per-pixel passes of gridFromFlow (accumulation and the benefit gate)
/// were ~8 ms of single-threaded work per measurement - more than the band
/// render and the GPU flow together - so they run as tasks of CELLS.
///
/// A task owns one grid row and a contiguous range of its columns, i.e. a
/// fixed set of cells, and walks every band pixel that falls in those cells
/// in the sequential loop's order (band rows ascending, columns ascending).
/// Every band pixel feeds exactly one cell, so no two tasks ever write the
/// same accumulator, and each cell adds its pixels in exactly the order the
/// single-threaded loop did: the per-cell double sums - and therefore the
/// grid - are bit-identical with or without a pool, on any number of
/// threads.  The same guarantee DisFlow.cpp gives for the flow.
struct CellTask {
    std::size_t cellRow = 0;  ///< Grid row the task owns.
    int firstCol = 0;         ///< First grid column it owns.
    int endCol = 0;           ///< One past its last grid column.
};

/// Number of cell tasks for this grid, and the task behind an index.
[[nodiscard]] std::uint32_t cellBlocks(ThreadPool* pool) noexcept {
    return (pool != nullptr && pool->size() > 1) ? kCellBlocksPerRow : 1u;
}

[[nodiscard]] CellTask cellTask(std::size_t index, std::uint32_t blocks, std::uint32_t gridW) noexcept {
    CellTask t;
    t.cellRow = index / blocks;
    const std::size_t block = index % blocks;
    t.firstCol = static_cast<int>(block * gridW / blocks);
    t.endCol = static_cast<int>((block + 1) * gridW / blocks);
    return t;
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

/// sampleBand() of two same-sized planes at one position.
///
/// The benefit gate samples both lenses at the same two positions for every
/// band pixel; the wrap, clamp and weight arithmetic depends only on the
/// position, so it is done once here and applied to both planes with
/// sampleBand's own expressions - each result is exactly what sampleBand()
/// returns for that plane.  Anything sampleBand would refuse is handed to it
/// plane by plane, so even the degenerate cases agree.
void sampleBandPair(const std::vector<float>& planeA, const std::vector<float>& planeB, std::uint32_t w,
                    std::uint32_t h, double x, double y, float& outA, float& outB) noexcept {
    const std::size_t n = static_cast<std::size_t>(w) * h;
    if (w == 0 || h == 0 || planeA.size() != n || planeB.size() != n || !std::isfinite(x) || !std::isfinite(y)) {
        outA = sampleBand(planeA, w, h, x, y);
        outB = sampleBand(planeB, w, h, x, y);
        return;
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
    const std::size_t i00 = static_cast<std::size_t>(y0) * w + static_cast<std::size_t>(x0);
    const std::size_t i10 = static_cast<std::size_t>(y0) * w + static_cast<std::size_t>(x1);
    const std::size_t i01 = static_cast<std::size_t>(y1) * w + static_cast<std::size_t>(x0);
    const std::size_t i11 = static_cast<std::size_t>(y1) * w + static_cast<std::size_t>(x1);
    const auto lerp2 = [&](const std::vector<float>& p) {
        const double a = p[i00];
        const double b = p[i10];
        const double c = p[i01];
        const double d = p[i11];
        const double top = a + (b - a) * tx;
        const double bot = c + (d - c) * tx;
        return static_cast<float>(top + (bot - top) * ty);
    };
    outA = lerp2(planeA);
    outB = lerp2(planeB);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Flow field -> angular grid
// ---------------------------------------------------------------------------
Result<ParallaxWarpGrid> gridFromFlow(const LensBands& bands, const BidirFlow& flow,
                                      const ParallaxWarpParams& params, ThreadPool* pool, ParallaxCellStats* cellStats) {
    OSV_TRY(checkParams(params));
    // A caller's stats block is emptied first, so a refusal below never
    // leaves a previous measurement's numbers looking current.
    if (cellStats != nullptr) {
        *cellStats = ParallaxCellStats{};
    }
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
    const double colsPerCell = static_cast<double>(bands.w) / static_cast<double>(gridW);
    const double rowsPerCell =
        bands.h > 1 ? static_cast<double>(bands.h - 1) / static_cast<double>(gridRows - 1) : 1.0;

    // The cell each band row and each band column falls in, computed once:
    // they depend on one coordinate each, and the per-pixel lround they
    // replace was a measurable share of this function's time.  Same
    // expressions, so the same cells.
    //
    // Row: nearest measured grid row to the band row's centre.
    std::vector<int> cellRowOf(bands.h);
    for (std::uint32_t r = 0; r < bands.h; ++r) {
        const int gr = static_cast<int>(std::lround(static_cast<double>(r) / rowsPerCell));
        cellRowOf[r] = std::clamp(gr, 0, static_cast<int>(gridRows) - 1);
    }
    // Column: nearest grid column to the band column's centre.  The kernel
    // reads cell gc at longitude fraction gc / gridW, which is band position
    // gc * colsPerCell; ROUNDING keeps each cell centred on the position it
    // is read back at.  Flooring - the first version - put every cell half a
    // cell (0.7 degrees at 256 columns) east of where it had been measured.
    std::vector<int> cellColOf(bands.w);
    for (std::uint32_t c = 0; c < bands.w; ++c) {
        const int gc = static_cast<int>(std::lround((static_cast<double>(c) + 0.5) / colsPerCell));
        cellColOf[c] = ((gc % static_cast<int>(gridW)) + static_cast<int>(gridW)) % static_cast<int>(gridW);
    }

    // Tasks of cells (see CellTask): each owns its cells outright and walks
    // their pixels in the sequential row-major order, so every cell's sums
    // are bit-identical to the single-threaded loop's.  The two counters are
    // integers, summed per task and then added, which no order changes.
    const std::uint32_t blocks = cellBlocks(pool);
    const std::size_t taskCount = static_cast<std::size_t>(gridRows) * blocks;
    std::vector<std::uint64_t> covisibleOf(taskCount, 0u);
    std::vector<std::uint64_t> consistentOf(taskCount, 0u);
    const auto accumulateTask = [&](std::size_t index) {
        const CellTask task = cellTask(index, blocks, gridW);
        std::uint64_t covisible = 0;
        std::uint64_t consistent = 0;
        for (std::uint32_t r = 0; r < bands.h; ++r) {
            if (static_cast<std::size_t>(cellRowOf[r]) != task.cellRow) {
                continue;
            }
            const std::size_t rowBase = task.cellRow * gridW;
            for (std::uint32_t c = 0; c < bands.w; ++c) {
                if (cellColOf[c] < task.firstCol || cellColOf[c] >= task.endCol) {
                    continue;  // another task's cell
                }
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
                const std::size_t gi = rowBase + static_cast<std::size_t>(cellColOf[c]);
                // Pixels -> the MASTER lens's half displacement in radians.
                //
                // The master (lens 1) must sample at q + f/2 and the slave at
                // q - f/2.  A column step is +longitude; a ROW step is
                // -latitude (row 0 is the +90 pole), hence the minus on dLat.
                accLon[gi] += 0.5 * fu * radPerCol;
                accLat[gi] += -0.5 * fv * radPerRow;
                accN[gi] += 1.0;
            }
        }
        covisibleOf[index] = covisible;
        consistentOf[index] = consistent;
    };
    if (!forTasks(pool, taskCount, accumulateTask)) {
        // A parallel run that failed part-way leaves partial sums: start
        // again from zero on this thread, which gives the same result.
        std::fill(accLon.begin(), accLon.end(), 0.0);
        std::fill(accLat.begin(), accLat.end(), 0.0);
        std::fill(accN.begin(), accN.end(), 0.0);
        (void)forTasks(nullptr, taskCount, accumulateTask);
    }
    std::uint64_t consistent = 0;
    std::uint64_t covisible = 0;
    for (std::size_t t = 0; t < taskCount; ++t) {
        covisible += covisibleOf[t];
        consistent += consistentOf[t];
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

    // ---- the raw per-cell measurement, for a caller that fits a model -------
    // Taken HERE, before the fill, the blur, the decay and the gate reshape
    // the field for rendering (ParallaxCellStats says why each of those would
    // mislead a fit).  The gate's verdict is filled in below, once known; with
    // the gate off every measured cell counts as trusted.
    if (cellStats != nullptr) {
        cellStats->w = gridW;
        cellStats->rows = gridRows;
        cellStats->latTopRad = latTop;
        cellStats->latStepRad = latPerGridRow;
        cellStats->halfFlow.assign(cells * 2u, 0.0f);
        cellStats->pixels.assign(cells, 0u);
        cellStats->gate.assign(cells, params.requiredImprovement > 0.0 ? 0.0f : 1.0f);
        for (std::size_t gi = 0; gi < cells; ++gi) {
            if (!(accN[gi] > 0.0)) {
                continue;  // nothing consistent landed here: zero flow, zero pixels
            }
            // The unscaled, unclamped mean half displacement, radians.
            cellStats->halfFlow[gi * 2u + 0u] = static_cast<float>(accLon[gi] / accN[gi]);
            cellStats->halfFlow[gi * 2u + 1u] = static_cast<float>(accLat[gi] / accN[gi]);
            // accN counts pixels one at a time, so it is an exact integer.
            cellStats->pixels[gi] = static_cast<std::uint32_t>(std::min(accN[gi], 4294967295.0));
        }
    }

    // ---- fill cells nothing measured -------------------------------------
    // An empty cell left at zero is a claim that there is no parallax there,
    // and a zero island inside a corrected region tears exactly like an
    // unwarped one.  First along longitude within the row (the ring wraps),
    // then whole empty rows from the nearest row that has data - which is
    // what happens at the band's top and bottom, where one lens's coverage
    // has already dropped below the co-visibility threshold.
    //
    // Each empty cell copies the nearest measured cell of its row, looking
    // outward one step at a time and preferring the LEFT neighbour when both
    // are equally near.  That rule is evaluated with two sweeps round the
    // ring - the nearest measured cell to each side of every empty one -
    // rather than by searching outward from each empty cell, which cost
    // O(gridW^2) modulo steps on a sparsely measured row.  Same sources,
    // same copies.
    std::vector<std::uint8_t> rowHasData(gridRows, 0u);
    std::vector<std::uint32_t> leftSrc(gridW);
    std::vector<std::uint32_t> rightSrc(gridW);
    for (std::uint32_t gr = 0; gr < gridRows; ++gr) {
        const std::size_t base = static_cast<std::size_t>(gr) * gridW;
        const auto has = [&](std::uint32_t gc) { return accN[base + gc] > 0.0; };
        std::uint32_t anchor = gridW;  // any measured column of this row
        for (std::uint32_t gc = 0; gc < gridW; ++gc) {
            if (has(gc)) {
                anchor = gc;
                break;
            }
        }
        if (anchor == gridW) {
            continue;  // an empty row: filled from its neighbours below
        }
        rowHasData[gr] = 1u;
        // Walk right from the anchor: the last measured column passed is the
        // nearest one to the LEFT of each empty column reached.
        std::uint32_t last = anchor;
        for (std::uint32_t step = 1; step < gridW; ++step) {
            const std::uint32_t gc = (anchor + step) % gridW;
            if (has(gc)) {
                last = gc;
            } else {
                leftSrc[gc] = last;
            }
        }
        // Walk left from the anchor for the nearest one to the RIGHT.
        last = anchor;
        for (std::uint32_t step = 1; step < gridW; ++step) {
            const std::uint32_t gc = (anchor + gridW - step) % gridW;
            if (has(gc)) {
                last = gc;
            } else {
                rightSrc[gc] = last;
            }
        }
        for (std::uint32_t gc = 0; gc < gridW; ++gc) {
            if (has(gc)) {
                continue;
            }
            const std::uint32_t distLeft = (gc + gridW - leftSrc[gc]) % gridW;
            const std::uint32_t distRight = (rightSrc[gc] + gridW - gc) % gridW;
            const std::uint32_t src = distLeft <= distRight ? leftSrc[gc] : rightSrc[gc];  // ties go left
            measured[(base + gc) * 2u + 0u] = measured[(base + src) * 2u + 0u];
            measured[(base + gc) * 2u + 1u] = measured[(base + src) * 2u + 1u];
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
    blurComponent(grid.uv, grid.w, grid.h, 0, params.crossMeridianSmooth, pool);
    blurComponent(grid.uv, grid.w, grid.h, 1, 1.0, pool);

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
        // Longitude of each band column's centre, as the kernel will see it
        // (float, like the ray it derives from) - computed once per column
        // instead of once per pixel, with the identical expression.
        std::vector<float> lonOf(bands.w);
        for (std::uint32_t c = 0; c < bands.w; ++c) {
            lonOf[c] = static_cast<float>((static_cast<double>(c) + 0.5) * radPerCol - osv::kPi);
        }
        // The same cell tasks as the accumulation above, for the same reason:
        // each cell's three sums keep the sequential pixel order.
        const auto gateTask = [&](std::size_t index) {
            const CellTask task = cellTask(index, blocks, gridW);
            const std::size_t rowBase = task.cellRow * gridW;
            for (std::uint32_t r = 0; r < bands.h; ++r) {
                if (static_cast<std::size_t>(cellRowOf[r]) != task.cellRow) {
                    continue;
                }
                const float lat = static_cast<float>(latOfBandRow(static_cast<double>(r)));
                for (std::uint32_t c = 0; c < bands.w; ++c) {
                    if (cellColOf[c] < task.firstCol || cellColOf[c] >= task.endCol) {
                        continue;  // another task's cell
                    }
                    const std::size_t i = static_cast<std::size_t>(r) * bands.w + c;
                    if (!(bands.alpha[0][i] > 0.5f) || !(bands.alpha[1][i] > 0.5f)) {
                        continue;
                    }
                    const std::size_t gi = rowBase + static_cast<std::size_t>(cellColOf[c]);

                    // The displacement the kernel will apply at this pixel,
                    // in band pixels.  A longitude step is a column step; a
                    // latitude step is a NEGATIVE row step (row 0 is the +90
                    // pole).
                    const float lon = lonOf[c];
                    const double dLon = osvWarpSample(&kp, grid.uv.data(), lon, lat, 0);
                    const double dLat = osvWarpSample(&kp, grid.uv.data(), lon, lat, 1);
                    const double dx = dLon / radPerCol;
                    const double dy = -dLat / radPerRow;
                    const double x = static_cast<double>(c) + 0.5;
                    const double y = static_cast<double>(r) + 0.5;
                    // Both lenses at p - d, then both at p + d.
                    float sampled[4];
                    sampleBandPair(bands.luma[0], bands.luma[1], bands.w, bands.h, x - dx, y - dy, sampled[0],
                                   sampled[1]);
                    sampleBandPair(bands.luma[0], bands.luma[1], bands.w, bands.h, x + dx, y + dy, sampled[2],
                                   sampled[3]);
                    const double slaveMinus = sampled[0];
                    const double masterMinus = sampled[1];
                    const double slavePlus = sampled[2];
                    const double masterPlus = sampled[3];
                    errNull[gi] += 0.5 * (std::fabs(slaveMinus - masterMinus) + std::fabs(slavePlus - masterPlus));
                    errWarped[gi] += std::fabs(slaveMinus - masterPlus);
                    errN[gi] += 1.0;
                }
            }
        };
        if (!forTasks(pool, taskCount, gateTask)) {
            // Partial sums from a failed parallel run: redo from zero here.
            std::fill(errNull.begin(), errNull.end(), 0.0);
            std::fill(errWarped.begin(), errWarped.end(), 0.0);
            std::fill(errN.begin(), errN.end(), 0.0);
            (void)forTasks(nullptr, taskCount, gateTask);
        }

        // 3 x 3 pooling (longitude wraps, latitude clamps) of all three sums
        // in one walk of the neighbourhood.  Each sum still adds the same
        // nine cells in the same order (rows, then columns, -1 to +1), so it
        // is the sum a separate walk per array gave.  The wrap is one
        // conditional add or subtract - gridW >= 8, so a +/-1 step never
        // wraps twice - where it was two integer modulos per tap.
        const int gridWi = static_cast<int>(gridW);
        const auto pooled3 = [&](std::size_t gi, double& n, double& sumNull, double& sumWarped) {
            const int gr = static_cast<int>(gi / gridW);
            const int gc = static_cast<int>(gi % gridW);
            n = 0.0;
            sumNull = 0.0;
            sumWarped = 0.0;
            for (int dr = -1; dr <= 1; ++dr) {
                const int rr = std::clamp(gr + dr, 0, static_cast<int>(gridRows) - 1);
                for (int dc = -1; dc <= 1; ++dc) {
                    int cc = gc + dc;
                    if (cc < 0) {
                        cc += gridWi;
                    } else if (cc >= gridWi) {
                        cc -= gridWi;
                    }
                    const std::size_t k = static_cast<std::size_t>(rr) * gridW + static_cast<std::size_t>(cc);
                    n += errN[k];
                    sumNull += errNull[k];
                    sumWarped += errWarped[k];
                }
            }
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
            double n = 0.0;
            double sumNull = 0.0;
            double sumWarped = 0.0;
            pooled3(gi, n, sumNull, sumWarped);
            double w = 0.0;
            if (n > 0.0) {
                const double eu = sumNull / n;
                const double ew = sumWarped / n;
                if (eu >= params.minResidual) {
                    w = smoothstep01((zeroAt - ew / eu) / (zeroAt - fullAt));
                }
            }
            weight[gi] = static_cast<float>(w);
            if (accN[gi] > 0.0 && w <= 0.0) {
                ++gatedCells;
            }
        }
        // The gate's per-cell verdict, before the smoothing below spreads it:
        // what a model fit should trust (ParallaxCellStats::gate).
        if (cellStats != nullptr && cellStats->gate.size() == cells) {
            std::copy(weight.begin(), weight.end(), cellStats->gate.begin());
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
        blurComponent(wGrid, grid.w, grid.h, 0, 1.0, pool);
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
Result<LensBands> measureParallaxBands(const geom::LensRig& rig, const video::FramePair& frames,
                                       const geom::BlendParams& blend, const ParallaxWarpParams& params,
                                       const std::vector<float>* seamTable, ThreadPool& pool) {
    OSV_TRY(checkParams(params));

    // Render the two per-lens bands.  Passing the seam table through means
    // the flow measures the RESIDUAL parallax the 1-D correction left behind,
    // so the two mechanisms compose instead of both claiming the same
    // disparity and over-correcting it.
    OSV_TRY_ASSIGN(LensBands bands, renderLensBands(rig, frames, blend, params.band, false, seamTable, pool));
    if (bands.w == 0 || bands.h == 0) {
        return Error{ErrorCode::Internal, "measureParallaxBands: band render produced nothing"};
    }
    return bands;
}

Result<ParallaxWarpGrid> buildParallaxWarp(const geom::LensRig& rig, const video::FramePair& frames,
                                           const geom::BlendParams& blend, const ParallaxWarpParams& params,
                                           const std::vector<float>* seamTable, ThreadPool& pool) {
    // Exactly the two halves in sequence, so a caller that splits them (the
    // importer, to keep the flow solve off its render thread) gets a result
    // identical to one that does not.
    const auto tBand = std::chrono::steady_clock::now();
    OSV_TRY_ASSIGN(LensBands bands, measureParallaxBands(rig, frames, blend, params, seamTable, pool));
    const double bandMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBand).count();
    return parallaxFromBands(bands, params, &pool, bandMs);
}

Result<ParallaxWarpGrid> parallaxFromBands(const LensBands& bands, const ParallaxWarpParams& params,
                                           ThreadPool* pool, double bandMs, ParallaxCellStats* cells) {
    OSV_TRY(checkParams(params));
    if (bands.w == 0 || bands.h == 0) {
        return Error{ErrorCode::InvalidArgument, "parallaxFromBands: empty bands"};
    }

    using Clock = std::chrono::steady_clock;
    const auto msSince = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

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
        return Error{ErrorCode::Internal, "parallaxFromBands: band planes are malformed"};
    }

    FlowBackendKind used = params.backend;
    const auto tFlow = Clock::now();
    OSV_TRY_ASSIGN(BidirFlow flow, computeFlow(params.backend, a, b, params.flow, pool, &used));
    const double flowMs = msSince(tFlow);

    const auto tGrid = Clock::now();
    OSV_TRY_ASSIGN(ParallaxWarpGrid grid, gridFromFlow(bands, flow, params, pool, cells));
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

Result<ParallaxWarpGrid> blendParallaxGrids(const ParallaxWarpGrid& from, const ParallaxWarpGrid& to, double t) {
    if (!from.valid() || !to.valid()) {
        return Error{ErrorCode::InvalidArgument, "blendParallaxGrids: a grid is empty or malformed"};
    }
    // Same layout or nothing: blending cell i of one grid with cell i of
    // another is only meaningful when cell i is the same place on the sphere
    // in both.  Grids built with one parameter set always agree; a mismatch
    // means the caller mixed grids from different settings.
    if (from.w != to.w || from.h != to.h || from.latMinRad != to.latMinRad || from.latMaxRad != to.latMaxRad) {
        return Error{ErrorCode::InvalidArgument, "blendParallaxGrids: the two grids have different layouts"};
    }
    if (!std::isfinite(t)) {
        return Error{ErrorCode::InvalidArgument, "blendParallaxGrids: non-finite blend weight"};
    }
    const float k = static_cast<float>(std::clamp(t, 0.0, 1.0));

    // Start from `to` so every diagnostic field describes the newer
    // measurement, then overwrite only the correction itself.
    ParallaxWarpGrid out = to;
    for (std::size_t i = 0; i < out.uv.size(); ++i) {
        out.uv[i] = from.uv[i] + (to.uv[i] - from.uv[i]) * k;
    }
    return out;
}

}  // namespace osv::render
