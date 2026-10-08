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
//   5. every region must PROVE it helps: the finished field is applied only
//      in proportion to how much it reduces the lens-to-lens residual (the
//      benefit gate), because a self-consistent flow is not necessarily a
//      true one;
//   6. last, the measurement as a whole must be trustworthy where it COULD
//      be: the share of the structured pixels whose flow passed the
//      forward-backward check decides refusal and, softly, the strength the
//      whole grid applies at (the structured gate, parallaxFromBands).

#include "osv/render/ParallaxWarp.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"
#include "osv/render/osv_kernel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <limits>

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
    // The structured gate: a finite, non-negative gradient bar, and two
    // shares inside [0, 1] that rise (equal shares are a plain step).
    if (!(p.minStructureGradient >= 0.0) || !std::isfinite(p.minStructureGradient)) {
        return failStatus(ErrorCode::InvalidArgument, "buildParallaxWarp: minStructureGradient must be >= 0");
    }
    if (!(p.minStructuredConsistent >= 0.0) || p.minStructuredConsistent > 1.0 ||
        !(p.fullStructuredConsistent >= p.minStructuredConsistent) || p.fullStructuredConsistent > 1.0) {
        return failStatus(ErrorCode::InvalidArgument,
                          "buildParallaxWarp: the structured gate needs 0 <= minStructuredConsistent <= "
                          "fullStructuredConsistent <= 1");
    }
    return okStatus();
}

/// Squared central-difference luma gradient of one lens at band pixel
/// (r, c), on the 0..1 code scale (ParallaxWarpParams::minStructureGradient
/// documents the rule).
///
/// Half the difference of the two neighbours along the band (longitude,
/// wrapping: the band is a ring) and across it (latitude).  A neighbour this
/// lens does not cover (alpha <= 0.5), a non-finite one, or one beyond the
/// band's top or bottom row is replaced by the centre pixel - a one-sided
/// half difference, the same as edge padding - so the black beyond a rim or
/// an occlusion polygon never reads as structure.  NaN for a non-finite
/// centre, which then fails every comparison: no structure.
///
/// @pre r < h, c < w, both planes w * h (gridFromFlow checks the sizes).
[[nodiscard]] double lumaGradientSq(const std::vector<float>& luma, const std::vector<float>& alpha, std::uint32_t w,
                                    std::uint32_t h, std::uint32_t r, std::uint32_t c) noexcept {
    const std::size_t i = static_cast<std::size_t>(r) * w + c;
    const double centre = static_cast<double>(luma[i]);
    if (!std::isfinite(centre)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // A neighbour's value, or the centre where the neighbour cannot speak.
    const auto at = [&](std::size_t j) noexcept {
        const double v = static_cast<double>(luma[j]);
        return (alpha[j] > 0.5f && std::isfinite(v)) ? v : centre;
    };
    const std::size_t rowBase = static_cast<std::size_t>(r) * w;
    // Longitude wraps round the ring.
    const std::uint32_t cl = c == 0 ? w - 1u : c - 1u;
    const std::uint32_t cr = c + 1u == w ? 0u : c + 1u;
    const double left = at(rowBase + cl);
    const double right = at(rowBase + cr);
    // Latitude ends: the missing row is the centre.
    const double up = r > 0 ? at(rowBase - w + c) : centre;
    const double down = r + 1u < h ? at(rowBase + w + c) : centre;
    const double gx = 0.5 * (right - left);
    const double gy = 0.5 * (down - up);
    return gx * gx + gy * gy;
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

    // ---- structure, for the structured gate (parallaxFromBands) -------------
    // A co-visible pixel is STRUCTURED when both lenses show a luma gradient
    // of at least minStructureGradient there (lumaGradientSq): the flow can
    // lock onto it, so its forward-backward verdict says something about the
    // measurement.  Compared squared, so no square root per pixel.  Bands
    // without luma planes of the band's size (a caller that only exercises
    // the conversion) have no measurable structure: none is counted, and
    // the gate then refuses such a measurement rather than guess.
    const bool lumaUsable = bands.luma[0].size() == bandPixels && bands.luma[1].size() == bandPixels;
    const double structureSq = params.minStructureGradient * params.minStructureGradient;
    const auto structuredAt = [&](std::uint32_t r, std::uint32_t c) noexcept {
        return lumaUsable &&
               lumaGradientSq(bands.luma[0], bands.alpha[0], bands.w, bands.h, r, c) >= structureSq &&
               lumaGradientSq(bands.luma[1], bands.alpha[1], bands.w, bands.h, r, c) >= structureSq;
    };

    // Tasks of cells (see CellTask): each owns its cells outright and walks
    // their pixels in the sequential row-major order, so every cell's sums
    // are bit-identical to the single-threaded loop's.  The four counters are
    // integers, summed per task and then added, which no order changes.
    const std::uint32_t blocks = cellBlocks(pool);
    const std::size_t taskCount = static_cast<std::size_t>(gridRows) * blocks;
    std::vector<std::uint64_t> covisibleOf(taskCount, 0u);
    std::vector<std::uint64_t> consistentOf(taskCount, 0u);
    std::vector<std::uint64_t> structuredOf(taskCount, 0u);
    std::vector<std::uint64_t> consistentStructuredOf(taskCount, 0u);
    const auto accumulateTask = [&](std::size_t index) {
        const CellTask task = cellTask(index, blocks, gridW);
        std::uint64_t covisible = 0;
        std::uint64_t consistent = 0;
        std::uint64_t structuredCount = 0;
        std::uint64_t consistentStructured = 0;
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
                // Counted before the consistency test: the gate's
                // denominator is every structured co-visible pixel.
                const bool structured = structuredAt(r, c);
                structuredCount += structured ? 1u : 0u;
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
                consistentStructured += structured ? 1u : 0u;
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
        structuredOf[index] = structuredCount;
        consistentStructuredOf[index] = consistentStructured;
    };
    if (!forTasks(pool, taskCount, accumulateTask)) {
        // A parallel run that failed part-way leaves partial sums: start
        // again from zero on this thread, which gives the same result.  (The
        // per-task counters are overwritten, not added to: no reset needed.)
        std::fill(accLon.begin(), accLon.end(), 0.0);
        std::fill(accLat.begin(), accLat.end(), 0.0);
        std::fill(accN.begin(), accN.end(), 0.0);
        (void)forTasks(nullptr, taskCount, accumulateTask);
    }
    std::uint64_t consistent = 0;
    std::uint64_t covisible = 0;
    std::uint64_t structured = 0;
    std::uint64_t consistentStructured = 0;
    for (std::size_t t = 0; t < taskCount; ++t) {
        covisible += covisibleOf[t];
        consistent += consistentOf[t];
        structured += structuredOf[t];
        consistentStructured += consistentStructuredOf[t];
    }
    grid.consistentPixels = consistent;
    grid.totalPixels = covisible;
    grid.structuredPixels = structured;
    grid.consistentStructuredPixels = consistentStructured;

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
        // The gate's band-wide counts ride along, so a refused measurement
        // can still say how close it came (parallaxFromBands refuses AFTER
        // this function returns, and then hands back no grid).
        cellStats->covisiblePixels = covisible;
        cellStats->consistentPixels = consistent;
        cellStats->structuredPixels = structured;
        cellStats->consistentStructuredPixels = consistentStructured;
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
    // Per grid column, the measured-row cells the measurement leaves
    // untrusted (ParallaxWarpGrid::untrustedShare): unmeasured ones counted
    // here, gated ones added by the benefit gate below.
    std::vector<std::uint32_t> untrustedRows(gridW, 0u);
    for (std::size_t gi = 0; gi < cells; ++gi) {
        if (accN[gi] > 0.0) {
            ++measuredCells;
        } else {
            ++untrustedRows[gi % gridW];  // no consistent flow: the fill invented this cell
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
                ++untrustedRows[gi % gridW];  // measured, but warping by it did not help
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

    // ---- the untrusted share per column, for the per-column guard -------------
    // Counted over the measured rows only: the decay rings are copies of the
    // edge rows and say nothing of their own.  gridRows >= 4 (checkParams).
    grid.untrustedShare.assign(gridW, 0.0f);
    for (std::uint32_t gc = 0; gc < gridW; ++gc) {
        grid.untrustedShare[gc] =
            static_cast<float>(static_cast<double>(untrustedRows[gc]) / static_cast<double>(gridRows));
    }

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

    // ---- the structured gate ---------------------------------------------------
    // Judge the measurement where it COULD be right.  On a flat pixel - sky,
    // water, white paint - the solver has nothing to lock onto and solves
    // noise, so the forward-backward check fails there whatever the solver
    // (9-15 % consistent on the car-mounted day clip, against 49-80 % on
    // textured pixels).  The 0.5.0 rule took the consistent share of ALL
    // co-visible pixels and so mostly measured how much of the band was
    // sky: it sat at 0.19-0.35 on that clip and flipped between grid and
    // table from one bucket to the next.  The share of the STRUCTURED
    // pixels separates a real measurement from a meaningless one instead
    // (0.71-0.82 on the same buckets, 0.10-0.28 on unrelated lens pairs).
    const double structuredShare = grid.structuredFraction();
    if (grid.structuredPixels < params.minStructuredPixels ||
        !(structuredShare >= params.minStructuredConsistent)) {
        // Both shares in the message: a caller that only sees the Error (a
        // log line, a list of refusals) still knows how close it came.
        return Error{ErrorCode::Unsupported,
                     std::format("parallax flow too inconsistent to use: structured {:.1f}% of {} px (needs {:.0f}% of "
                                 "at least {}), all co-visible {:.1f}%",
                                 100.0 * structuredShare, grid.structuredPixels,
                                 100.0 * params.minStructuredConsistent, params.minStructuredPixels,
                                 100.0 * grid.consistentFraction())};
    }

    // Between the gate's two shares the grid applies at strength s - the
    // whole field, uniformly - and the caller fills the rest with the seam
    // table (seamTableUnderGrid), so the correction glides as the share
    // drifts across the gate instead of flipping.  At full strength nothing
    // is touched: the grid is bit for bit what gridFromFlow built.
    const double s = parallaxGateStrength(structuredShare, params);
    grid.strength = s;
    if (s < 1.0) {
        for (float& v : grid.uv) {
            v = static_cast<float>(static_cast<double>(v) * s);
        }
        // The diagnostics describe what is applied (FULL disparity).
        grid.meanAbsCorrectionDeg *= s;
        grid.maxAbsCorrectionDeg *= s;
    }
    return grid;
}

double parallaxGateStrength(double structuredFraction, const ParallaxWarpParams& params) noexcept {
    const double lo = params.minStructuredConsistent;
    const double hi = params.fullStructuredConsistent;
    // No trust in a share or a gate that is not a number, or a gate whose
    // full-strength share sits below its refusal share.
    if (!std::isfinite(structuredFraction) || !std::isfinite(lo) || !std::isfinite(hi) || hi < lo) {
        return 0.0;
    }
    // Equal shares: a plain step at that share.
    if (!(hi > lo)) {
        return structuredFraction >= hi ? 1.0 : 0.0;
    }
    return smoothstep01((structuredFraction - lo) / (hi - lo));
}

void seamTableUnderGrid(const std::vector<float>& table, double gridStrength, std::vector<float>& out) {
    // A non-finite strength is no trust in the grid: the whole table.
    const double s = std::isfinite(gridStrength) ? std::clamp(gridStrength, 0.0, 1.0) : 0.0;
    const double share = 1.0 - s;
    // The caller may hand the same vector in as table and out (scale in
    // place): clearing `out` first would then wipe the table before it is
    // read, so the output is only cleared once it is known to be distinct,
    // and an in-place call scales the columns where they are.
    const bool inPlace = (&table == &out);
    if (table.empty() || !(share > 0.0)) {
        out.clear();  // nothing to fill: no table, or a grid trusted in full
        return;
    }
    if (!inPlace) {
        out.resize(table.size());
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double v = static_cast<double>(table[i]);
        // share == 1 multiplies exactly, so a refused grid's table is the
        // table bit for bit (non-finite columns aside, which shift nothing).
        out[i] = std::isfinite(v) ? static_cast<float>(v * share) : 0.0f;
    }
}

// The guard's confidence ramp is the table glide's step ramp (see the header):
// one notion of "a table column this sure is a real measurement".
static_assert(kGridGuardConfidenceLo == kSeamTableStepConfLo && kGridGuardConfidenceHi == kSeamTableStepConfHi,
              "the per-column guard and the table glide must agree on what a confident table column is");
static_assert(kGridGuardConfidenceHi > kGridGuardConfidenceLo, "the guard's confidence ramp must rise");
static_assert(kGridGuardUntrustedHi > kGridGuardUntrustedLo && kGridGuardUntrustedLo >= 0.0 &&
                  kGridGuardUntrustedHi <= 1.0,
              "the guard's untrusted-share ramp must rise inside [0, 1]");
// A disagreement counts from the table's own bucket-to-bucket noise on: the
// glide's noise floor (kSeamTableGlideNoiseDeg) is the same measurement noise.
static_assert(kGridGuardDisagreeDeg == kSeamTableGlideNoiseDeg && kGridGuardAgreeDeg >= 0.0 &&
                  kGridGuardAgreeDeg < kGridGuardDisagreeDeg,
              "the guard's disagreement ramp must rise to the table's noise floor");

Result<GuardedCorrection> guardGridWithTable(const ParallaxWarpGrid& grid, const std::vector<float>& table,
                                             const std::vector<float>& confidence) {
    if (!grid.valid()) {
        return Error{ErrorCode::InvalidArgument, "guardGridWithTable: the grid is empty or malformed"};
    }
    GuardedCorrection out;
    // The grid's own strength: the table already fills 1 - s of every column
    // under a partly trusted grid (seamTableUnderGrid), with or without the guard.
    const double s = std::isfinite(grid.strength) ? std::clamp(grid.strength, 0.0, 1.0) : 0.0;
    const std::uint32_t W = grid.w;
    const std::size_t n = table.size();

    // ---- without what the guard needs: the rule before it ------------------------
    // No table, a confidence that is not the table's column for column, or a
    // grid that carries no untrusted shares (built by hand, not measured).
    if (n == 0 || confidence.size() != n || grid.untrustedShare.size() != W) {
        seamTableUnderGrid(table, s, out.table);
        // The table's share is 1 - s in every column (none when it is empty).
        out.tableFraction.assign(out.table.size(), static_cast<float>(1.0 - s));
        return out;
    }

    // ---- the table's confidence and value per grid column ----------------------------
    // Table column i sits at longitude fraction (i + 0.5) / n; the grid cell
    // nearest it is the one gridFromFlow accumulated that longitude into
    // (cellColOf: the rounded position), so each grid column averages exactly
    // the table columns its cells were measured over when the two share a
    // band width.  A value that is not a number counts as 0 (no confidence,
    // no shift).
    std::vector<double> confSum(W, 0.0);
    std::vector<double> shiftSum(W, 0.0);
    std::vector<double> count(W, 0.0);
    const double gridPerTable = static_cast<double>(W) / static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        const long long nearest = std::llround((static_cast<double>(i) + 0.5) * gridPerTable);
        const auto gc = static_cast<std::size_t>(((nearest % static_cast<long long>(W)) + W) % W);
        const double c = static_cast<double>(confidence[i]);
        const double v = static_cast<double>(table[i]);
        confSum[gc] += std::isfinite(c) ? std::clamp(c, 0.0, 1.0) : 0.0;
        shiftSum[gc] += std::isfinite(v) ? v : 0.0;
        count[gc] += 1.0;
    }

    // ---- the grid's own along-meridian correction per column, as a table value ------
    // G = -2 dLat in degrees: the grid stores the master lens's displacement
    // (HALF the disparity), the master's axis is body +Y - the +90 deg pole of
    // the polar-axis band - and a positive table value moves each lens AWAY
    // from its own axis, i.e. the master toward lower latitude.  Averaged over
    // the rows where the kernel applies the table in full (|latitude| up to
    // OSV_SEAM_SHIFT_FULL_DEG, osvSeamShiftTaper), since that is where the two
    // corrections stand for the same thing; every row when none lies there.
    const double fullRad = static_cast<double>(OSV_SEAM_SHIFT_FULL_DEG) * osv::kPi / 180.0;
    const double latMin = static_cast<double>(grid.latMinRad);
    const double latSpan = static_cast<double>(grid.latMaxRad) - latMin;
    std::vector<std::uint32_t> rows;
    rows.reserve(grid.h);
    for (std::uint32_t gy = 0; gy < grid.h; ++gy) {
        // Rows run linearly from latMinRad (row 0) to latMaxRad (row h - 1),
        // as osvWarpSample reads them.
        const double lat =
            grid.h > 1 ? latMin + latSpan * static_cast<double>(gy) / static_cast<double>(grid.h - 1u) : latMin;
        if (std::isfinite(lat) && std::fabs(lat) <= fullRad) {
            rows.push_back(gy);
        }
    }
    if (rows.empty()) {
        for (std::uint32_t gy = 0; gy < grid.h; ++gy) {
            rows.push_back(gy);
        }
    }
    out.gridAlongDeg.assign(W, 0.0f);
    for (std::uint32_t gc = 0; gc < W; ++gc) {
        double sumLat = 0.0;
        for (const std::uint32_t gy : rows) {
            const double v = static_cast<double>(grid.uv[(static_cast<std::size_t>(gy) * W + gc) * 2u + 1u]);
            sumLat += std::isfinite(v) ? v : 0.0;  // a cell that is not a number corrects nothing
        }
        const double meanLat = sumLat / static_cast<double>(rows.size());
        out.gridAlongDeg[gc] = static_cast<float>(-2.0 * meanLat * 180.0 / osv::kPi);
    }

    // ---- the guard weight per grid column, smoothed round the ring ------------------
    // g = (the grid did not measure the column) x (the table did) x (the table
    // found a disparity the correction on screen misses), each a smoothstep
    // so the hand-over has no threshold step of its own.  The correction on
    // screen along the meridian is the grid's G plus the table's 1 - s share,
    // so its distance from the table's T is |G + (1 - s) T - T| = |G - s T|.
    // Interleaved as blurComponent expects (component 0, one row), so the
    // smoothing wraps at +/-180 deg exactly as the kernel's grid fetch does.
    std::vector<float> weight(static_cast<std::size_t>(W) * 2u, 0.0f);
    bool any = false;
    for (std::uint32_t gc = 0; gc < W; ++gc) {
        const double share = static_cast<double>(grid.untrustedShare[gc]);
        const double u = std::isfinite(share) ? std::clamp(share, 0.0, 1.0) : 0.0;
        const double untrusted =
            smoothstep01((u - kGridGuardUntrustedLo) / (kGridGuardUntrustedHi - kGridGuardUntrustedLo));
        const double conf = count[gc] > 0.0 ? confSum[gc] / count[gc] : 0.0;
        const double sure =
            smoothstep01((conf - kGridGuardConfidenceLo) / (kGridGuardConfidenceHi - kGridGuardConfidenceLo));
        const double tableDeg = count[gc] > 0.0 ? shiftSum[gc] / count[gc] : 0.0;
        const double miss = std::fabs(static_cast<double>(out.gridAlongDeg[gc]) - s * tableDeg);
        const double disagree =
            std::isfinite(miss)
                ? smoothstep01((miss - kGridGuardAgreeDeg) / (kGridGuardDisagreeDeg - kGridGuardAgreeDeg))
                : 0.0;
        const double g = untrusted * sure * disagree;
        weight[static_cast<std::size_t>(gc) * 2u] = static_cast<float>(g);
        any = any || g > 0.0;
    }
    if (!any) {
        // Nothing guarded: the grid as it is and the table's 1 - s share,
        // the rule before the guard.
        seamTableUnderGrid(table, s, out.table);
        out.tableFraction.assign(out.table.size(), static_cast<float>(1.0 - s));
        return out;
    }
    blurComponent(weight, W, 1u, 0, kGridGuardSmoothCols, nullptr);
    out.guard.assign(W, 0.0f);
    double sumGuard = 0.0;
    for (std::uint32_t gc = 0; gc < W; ++gc) {
        const float g = weight[static_cast<std::size_t>(gc) * 2u];
        out.guard[gc] = std::isfinite(g) ? std::clamp(g, 0.0f, 1.0f) : 0.0f;
        sumGuard += static_cast<double>(out.guard[gc]);
        if (static_cast<double>(out.guard[gc]) >= kGridGuardCountedWeight) {
            ++out.guardedColumns;
        }
    }
    out.meanGuard = sumGuard / static_cast<double>(W);
    out.changed = true;

    // ---- the grid: every row of a column keeps 1 - g of its correction -----------------
    // The decay rings included, so the column's fall-off to zero keeps its
    // shape.  A cell that is not a number comes out as no correction.
    out.grid = grid;
    for (std::uint32_t gy = 0; gy < out.grid.h; ++gy) {
        float* row = out.grid.uv.data() + static_cast<std::size_t>(gy) * W * 2u;
        for (std::uint32_t gc = 0; gc < W; ++gc) {
            const float keep = 1.0f - out.guard[gc];
            for (std::uint32_t comp = 0; comp < 2u; ++comp) {
                float& v = row[gc * 2u + comp];
                v = std::isfinite(v) ? v * keep : 0.0f;
            }
        }
    }

    // ---- the table: its 1 - s share plus s of every column handed over ---------------
    // g at the table column's longitude is interpolated linearly between the
    // two grid columns around it - the kernel's own bilinear grid fetch
    // (osvWarpSample) - so wherever the grid gives up a share, the table takes
    // exactly that share over.
    out.table.resize(n);
    out.tableFraction.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double fx = (static_cast<double>(i) + 0.5) * gridPerTable;
        const double fl = std::floor(fx);
        const double t = fx - fl;
        const long long x0 = static_cast<long long>(fl);
        const auto c0 = static_cast<std::size_t>(((x0 % static_cast<long long>(W)) + W) % W);
        const auto c1 = static_cast<std::size_t>((c0 + 1u) % W);
        const double g0 = static_cast<double>(out.guard[c0]);
        const double g = std::clamp(g0 + (static_cast<double>(out.guard[c1]) - g0) * t, 0.0, 1.0);
        const double fraction = 1.0 - s * (1.0 - g);
        out.tableFraction[i] = static_cast<float>(fraction);
        const double v = static_cast<double>(table[i]);
        // No measurement here (not a number): no shift.
        out.table[i] = std::isfinite(v) ? static_cast<float>(v * fraction) : 0.0f;
    }
    return out;
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

ParallaxWarpGrid zeroParallaxGridLike(const ParallaxWarpGrid& like) {
    // A copy keeps the layout and the diagnostics; only the correction goes.
    ParallaxWarpGrid zero = like;
    std::fill(zero.uv.begin(), zero.uv.end(), 0.0f);
    return zero;
}

void blendSeamTables(const std::vector<float>* from, const std::vector<float>* to, double t, std::vector<float>& out,
                     double noiseDeg, double stepDeg, const std::vector<float>* fromConf,
                     const std::vector<float>* toConf) {
    out.clear();
    // ---- inputs: the weight, which sides exist, the agreement band -----------
    t = std::isfinite(t) ? std::clamp(t, 0.0, 1.0) : 1.0;
    bool hasFrom = from != nullptr && !from->empty();
    const bool hasTo = to != nullptr && !to->empty();
    if (hasFrom && hasTo && from->size() != to->size()) {
        hasFrom = false;  // two widths: `to` alone, faded in
    }
    if (!hasFrom && !hasTo) {
        return;  // nothing on either side
    }
    // A malformed band (inverted, non-finite) means "always glide": the
    // plain linear blend, which is never worse than the glide before this.
    const bool agreementTest =
        std::isfinite(noiseDeg) && std::isfinite(stepDeg) && noiseDeg >= 0.0 && stepDeg > noiseDeg;

    // ---- per column ------------------------------------------------------------
    const std::size_t n = hasTo ? to->size() : from->size();
    // A side's confidence is used only for a table that is used, and only
    // when it has one entry per column; otherwise that side is "sure" and
    // the gate is left to the other one (1 on both: the ungated rule).
    const bool gateFrom = hasFrom && fromConf != nullptr && fromConf->size() == n;
    const bool gateTo = hasTo && toConf != nullptr && toConf->size() == n;
    const auto confAt = [](const std::vector<float>& conf, std::size_t i) {
        const double c = static_cast<double>(conf[i]);
        return std::isfinite(c) ? std::clamp(c, 0.0, 1.0) : 0.0;  // NaN: unsure, glide
    };
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Each side's shift at this column; a missing or non-finite side
        // shifts nothing.
        double a = hasFrom ? static_cast<double>((*from)[i]) : 0.0;
        double b = hasTo ? static_cast<double>((*to)[i]) : 0.0;
        a = std::isfinite(a) ? a : 0.0;
        b = std::isfinite(b) ? b : 0.0;
        // Where the two disagree by more than measurement noise the scene at
        // the seam has changed: the newer table takes over (smoothstep) -
        // as far as both measurements are confident.  A large change that
        // either side is unsure of is matching noise, and glides.
        double k = t;
        if (agreementTest) {
            double x = std::clamp((std::abs(b - a) - noiseDeg) / (stepDeg - noiseDeg), 0.0, 1.0);
            x = x * x * (3.0 - 2.0 * x);
            if (gateFrom || gateTo) {
                const double c = std::min(gateFrom ? confAt(*fromConf, i) : 1.0, gateTo ? confAt(*toConf, i) : 1.0);
                x *= smoothstep01((c - kSeamTableStepConfLo) / (kSeamTableStepConfHi - kSeamTableStepConfLo));
            }
            k = t + (1.0 - t) * x;
        }
        const double v = a + (b - a) * k;
        out[i] = std::isfinite(v) ? static_cast<float>(v) : 0.0f;
    }
}

}  // namespace osv::render
