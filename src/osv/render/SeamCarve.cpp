// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SeamCarve.cpp - the dynamic-programming seam through the overlap band and
// the table the kernel's narrow blend follows.  [WP-SEAM]  See SeamCarve.h
// for the why and for the measurements behind the defaults; this file is the
// how.
//
// Layout conventions (the same as every other band analysis):
//   * band column x covers longitude (x + 0.5) * 2pi / w - pi at its centre;
//   * band row r sits at latitude pi/2 - (rowOffset + r + 0.5) * pi / mapH,
//     so row 0 is the NORTH edge of the band - the master lens's side;
//   * a seam "at row r" shows the master lens on rows < r, the slave lens on
//     rows > r, and mixes them in a window of +/- m rows around r.

#include "osv/render/SeamCarve.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"
#include "osv/render/osv_kernel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

namespace osv::render {

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds since `t0`.
[[nodiscard]] double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ===========================================================================
//  Process-wide penalty hooks
// ===========================================================================

/// Holder for the installed hooks, one per slot.  Allocated once and never
/// destroyed, for the same reason as the device band shader slot in
/// SeamAnalysis.cpp: a static destructor running in an order nobody controls
/// buys nothing the process exit does not do anyway.
struct HookSlots {
    std::mutex mutex;
    std::array<SeamPenaltyHook, static_cast<std::size_t>(SeamPenaltySlot::Count)> hooks{};
};

HookSlots& hookSlots() {
    static HookSlots* slots = new HookSlots();  // intentionally leaked, see above
    return *slots;
}

// ===========================================================================
//  Small helpers
// ===========================================================================

/// Cost used in place of a non-finite cell cost: far above any legitimate
/// total (the forbidden sentinel times every column of a large ring) so it
/// is never chosen while any alternative exists, yet finite so the DP's sums
/// stay ordered.
constexpr double kNonFiniteCost = 1.0e15;

/// Clamp-to-[0,1] smoothstep, the same curve the kernel uses.
[[nodiscard]] double smoothstep01(double t) noexcept {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// Wrap an integer into [0, n) (n > 0).
[[nodiscard]] int wrapIndex(int i, int n) noexcept {
    i %= n;
    return i < 0 ? i + n : i;
}

/// Run body(first, last) over [0, n) on the pool when there is one, inline
/// otherwise.  A pool that cannot run the job is answered by running inline,
/// so the result never depends on it; bodies write disjoint ranges only, so
/// the outcome is identical either way.
template <class Body> void forRange(ThreadPool* pool, std::size_t n, std::size_t grain, const Body& body) {
    if (n == 0) {
        return;
    }
    if (pool) {
        const Status st =
            pool->parallelFor(0, n, std::max<std::size_t>(grain, 1), [&](std::size_t b, std::size_t e) { body(b, e); });
        if (st.ok()) {
            return;
        }
        log::debug("seam carve: thread pool refused a job ({}); running inline", st.error().message);
    }
    body(0, n);
}

/// Bilinear sample of a band plane at pixel-centre coordinates (x, y) =
/// (column + 0.5, row + 0.5).  Longitude wraps (the band is a ring),
/// latitude clamps (the band ends).  The same rule as ParallaxWarp's benefit
/// gate, so the seam sees the correction the way the gate judged it.
[[nodiscard]] float sampleBand(const float* plane, std::uint32_t w, std::uint32_t h, double x, double y) noexcept {
    if (!plane || w == 0 || h == 0 || !std::isfinite(x) || !std::isfinite(y)) {
        return 0.0f;
    }
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    // floor() of a finite double can still be far outside int range; clamp
    // before the conversion so a garbage displacement cannot overflow it.
    const double flx = std::clamp(std::floor(fx), -1.0e9, 1.0e9);
    const double fly = std::clamp(std::floor(fy), -1.0e9, 1.0e9);
    const double tx = std::clamp(fx - flx, 0.0, 1.0);
    const double ty = std::clamp(fy - fly, 0.0, 1.0);
    const int W = static_cast<int>(w);
    const int H = static_cast<int>(h);
    const int x0 = wrapIndex(static_cast<int>(flx), W);
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

/// Gaussian smoothing of a closed ring of values (sigma in samples).  Sigma
/// below half a sample returns the input unchanged.
[[nodiscard]] std::vector<double> smoothRing(const std::vector<double>& v, double sigma) {
    const int n = static_cast<int>(v.size());
    if (n == 0 || !(sigma >= 0.5)) {
        return v;
    }
    // Radius 3 sigma, never more than half the ring (a wider kernel would
    // count samples twice).
    const int radius = std::min(static_cast<int>(std::ceil(3.0 * sigma)), n / 2);
    std::vector<double> kernel(static_cast<std::size_t>(2 * radius + 1));
    double ksum = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        const double k = std::exp(-0.5 * static_cast<double>(i * i) / (sigma * sigma));
        kernel[static_cast<std::size_t>(i + radius)] = k;
        ksum += k;
    }
    std::vector<double> out(v.size(), 0.0);
    for (int c = 0; c < n; ++c) {
        double acc = 0.0;
        for (int i = -radius; i <= radius; ++i) {
            acc += v[static_cast<std::size_t>(wrapIndex(c + i, n))] * kernel[static_cast<std::size_t>(i + radius)];
        }
        out[static_cast<std::size_t>(c)] = acc / ksum;
    }
    return out;
}

/// Validate the tuning.  Everything a malformed value could turn into a
/// division by zero, an empty loop or a NaN seam is refused here.
[[nodiscard]] Status checkParams(const SeamCarveParams& p) {
    const auto finiteNonNeg = [](double v) { return std::isfinite(v) && v >= 0.0; };
    if (p.columns == 0 || p.columns > 65536u) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: columns must be in [1, 65536]");
    }
    // [WP-SEAMTOOLS] narrow may be 0 - a hard cut where the lenses disagree
    // ("Parallax Blend" at 0); the kernel draws a zero feather as exactly
    // that, and nothing below divides by it.
    if (!(p.narrowHalfWidthDeg >= 0.0) || !std::isfinite(p.narrowHalfWidthDeg) || !(p.wideHalfWidthDeg > 0.0) ||
        !std::isfinite(p.wideHalfWidthDeg) || p.narrowHalfWidthDeg > p.wideHalfWidthDeg || p.wideHalfWidthDeg > 30.0) {
        return failStatus(ErrorCode::InvalidArgument,
                          "carveSeam: feather half widths must satisfy 0 <= narrow <= wide <= 30 degrees");
    }
    // [WP-SEAMTOOLS] The cost window: a real window, at most a band's worth.
    if (!(p.costWindowDeg > 0.0) || !std::isfinite(p.costWindowDeg) || p.costWindowDeg > 30.0) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: costWindowDeg must be in (0, 30] degrees");
    }
    if (!finiteNonNeg(p.agreeResidual) || !std::isfinite(p.disagreeResidual) ||
        !(p.disagreeResidual > p.agreeResidual)) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: need 0 <= agreeResidual < disagreeResidual");
    }
    if (!finiteNonNeg(p.edgeRampDeg) || p.edgeRampDeg > 30.0) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: edgeRampDeg must be in [0, 30]");
    }
    if (!finiteNonNeg(p.diffWeight) || !finiteNonNeg(p.gradWeight) || !finiteNonNeg(p.centreWeight) ||
        !finiteNonNeg(p.temporalWeight) || !finiteNonNeg(p.coverageWeight) || !finiteNonNeg(p.stepPenalty) ||
        !finiteNonNeg(p.smoothSigmaCols) || !finiteNonNeg(p.widthSigmaCols) || !finiteNonNeg(p.nearSigmaCols)) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: weights must be finite and >= 0");
    }
    if (!(p.temporalNormDeg > 0.0) || !std::isfinite(p.temporalNormDeg) || !(p.temporalClampDeg > 0.0) ||
        !std::isfinite(p.temporalClampDeg) || !(p.temporalRelaxRows > 0.0) || !std::isfinite(p.temporalRelaxRows) ||
        !finiteNonNeg(p.temporalFloor) || p.temporalFloor > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: temporal parameters out of range");
    }
    if (!(p.coverageMin > 0.0) || p.coverageMin > 1.0 || !finiteNonNeg(p.coverageBlocked) ||
        p.coverageBlocked >= p.coverageMin || !(p.forbiddenCost > 0.0) || !std::isfinite(p.forbiddenCost)) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: coverage parameters out of range");
    }
    if (p.maxStepRows < 0 || p.maxStepRows > 16) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: maxStepRows must be in [0, 16]");
    }
    if (p.penalty.installed() && !finiteNonNeg(p.penalty.weight)) {
        return failStatus(ErrorCode::InvalidArgument, "carveSeam: penalty weight must be finite and >= 0");
    }
    return okStatus();
}

/// Validate a band the way every consumer here needs it.
[[nodiscard]] Status checkBands(const LensBands& b, const char* who) {
    if (b.w == 0 || b.h == 0 || b.mapH == 0 || b.w > 65536u || b.h > 65536u) {
        return failStatus(ErrorCode::InvalidArgument, std::string(who) + ": empty or oversized band");
    }
    if (b.rowOffset + b.h > b.mapH) {
        return failStatus(ErrorCode::InvalidArgument, std::string(who) + ": band rows outside the map");
    }
    const std::size_t n = static_cast<std::size_t>(b.w) * b.h;
    for (int i = 0; i < 2; ++i) {
        if (b.luma[i].size() != n || b.alpha[i].size() != n) {
            return failStatus(ErrorCode::InvalidArgument, std::string(who) + ": band planes are the wrong size");
        }
    }
    return okStatus();
}

/// correctBandsForSeam, optionally also reporting the FULL vertical
/// disparity (band rows) the correction assumes at every band pixel - the
/// measure of how close to the camera the content there is.
Result<LensBands> correctBandsImpl(const LensBands& bands, const SeamCorrection& correction, ThreadPool* pool,
                                   std::vector<float>* disparityRows) {
    OSV_TRY(checkBands(bands, "correctBandsForSeam"));
    const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
    const bool haveWarp = correction.warp && correction.warp->valid();
    const bool haveTable = correction.seamShiftDeg && !correction.seamShiftDeg->empty();
    if (disparityRows) {
        disparityRows->assign(n, 0.0f);
    }
    if (!haveWarp && !haveTable) {
        return bands;  // nothing to correct: the bands are what the kernel blends
    }
    // A table of absurd length is a corrupt input, not a finer measurement.
    if (haveTable && correction.seamShiftDeg->size() > 65536u) {
        return Error{ErrorCode::InvalidArgument, "correctBandsForSeam: seam table is implausibly long"};
    }

    // The kernel's view of the grid, so osvWarpSample reads it exactly as
    // the renderer will (the same construction as ParallaxWarp's gate).
    OsvRenderParams kp;
    std::memset(&kp, 0, sizeof(kp));
    if (haveWarp) {
        kp.warpEnabled = 1;
        kp.warpW = static_cast<int>(correction.warp->w);
        kp.warpH = static_cast<int>(correction.warp->h);
        kp.warpLatMinRad = correction.warp->latMinRad;
        kp.warpLatMaxRad = correction.warp->latMaxRad;
    }
    const double radPerRow = osv::kPi / static_cast<double>(bands.mapH);
    const double radPerCol = osv::kTwoPi / static_cast<double>(bands.w);
    const double rowsPerDeg = static_cast<double>(bands.mapH) / 180.0;
    const int tableN = haveTable ? static_cast<int>(correction.seamShiftDeg->size()) : 0;

    LensBands out;
    out.w = bands.w;
    out.h = bands.h;
    out.rowOffset = bands.rowOffset;
    out.mapH = bands.mapH;
    for (int i = 0; i < 2; ++i) {
        out.luma[i].assign(n, 0.0f);
        out.alpha[i].assign(n, 0.0f);
    }

    forRange(pool, bands.h, 1, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            // Latitude of the row centre, as the float the kernel would see.
            const float lat = static_cast<float>(
                osv::kHalfPi - (static_cast<double>(bands.rowOffset) + static_cast<double>(r) + 0.5) * radPerRow);
            for (std::uint32_t x = 0; x < bands.w; ++x) {
                const float lon = static_cast<float>((static_cast<double>(x) + 0.5) * radPerCol - osv::kPi);
                double dx = 0.0;
                double dy = 0.0;
                if (haveTable) {
                    // osvSeamColumn's rule: the column the kernel reads for
                    // this longitude.  Each lens samples half the disparity
                    // away from its own axis - master (north pole) south,
                    // i.e. to larger rows; slave the other way.
                    double f = (static_cast<double>(lon) + osv::kPi) / osv::kTwoPi;
                    f = std::clamp(f, 0.0, 0.999999);
                    const int c = std::clamp(static_cast<int>(f * tableN), 0, tableN - 1);
                    const double delta = static_cast<double>((*correction.seamShiftDeg)[static_cast<std::size_t>(c)]);
                    if (std::isfinite(delta)) {
                        dy += 0.5 * delta * rowsPerDeg;
                    }
                }
                if (haveWarp) {
                    // Master moves by +(dLon, dLat), slave by the negation; a
                    // latitude step is a NEGATIVE row step.
                    const double dLon = osvWarpSample(&kp, correction.warp->uv, lon, lat, 0);
                    const double dLat = osvWarpSample(&kp, correction.warp->uv, lon, lat, 1);
                    if (std::isfinite(dLon) && std::isfinite(dLat)) {
                        dx += dLon / radPerCol;
                        dy += -dLat / radPerRow;
                    }
                }
                const std::size_t i = r * bands.w + x;
                const double px = static_cast<double>(x) + 0.5;
                const double py = static_cast<double>(r) + 0.5;
                out.luma[1][i] = sampleBand(bands.luma[1].data(), bands.w, bands.h, px + dx, py + dy);
                out.alpha[1][i] = sampleBand(bands.alpha[1].data(), bands.w, bands.h, px + dx, py + dy);
                out.luma[0][i] = sampleBand(bands.luma[0].data(), bands.w, bands.h, px - dx, py - dy);
                out.alpha[0][i] = sampleBand(bands.alpha[0].data(), bands.w, bands.h, px - dx, py - dy);
                if (disparityRows) {
                    (*disparityRows)[i] = static_cast<float>(2.0 * std::fabs(dy));
                }
            }
        }
    });
    return out;
}

/// One DP pass over `count` consecutive ring columns starting at `first`.
///
/// `startRow` >= 0 pins the first column's row; `endAnchor` >= 0 requires
/// the last column's row to be within maxStep of it and charges the step
/// penalty for that final connection (the ring's closure).  Writes the row
/// per visited column to `path` (size `count`).  Ties go to the smallest
/// move, then to the upward one, so the result is deterministic.
void dpPass(const std::vector<float>& cost, int cols, int rows, int first, int count, int maxStep, double stepPenalty,
            int startRow, int endAnchor, std::vector<int>& path) {
    const auto cell = [&](int c, int r) -> double {
        const float v =
            cost[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)];
        return std::isfinite(v) ? static_cast<double>(v) : kNonFiniteCost;
    };
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> prev(static_cast<std::size_t>(rows), inf);
    std::vector<double> cur(static_cast<std::size_t>(rows), inf);
    // Back pointers as the step taken INTO each cell (-maxStep..maxStep).
    std::vector<std::int8_t> back(static_cast<std::size_t>(count) * static_cast<std::size_t>(rows), 0);
    // Candidate steps in tie-break order: 0, -1, +1, -2, +2, ...
    std::vector<int> steps;
    steps.push_back(0);
    for (int a = 1; a <= maxStep; ++a) {
        steps.push_back(-a);
        steps.push_back(a);
    }

    const int c0 = wrapIndex(first, cols);
    for (int r = 0; r < rows; ++r) {
        if (startRow < 0 || r == startRow) {
            prev[static_cast<std::size_t>(r)] = cell(c0, r);
        }
    }
    for (int k = 1; k < count; ++k) {
        const int c = wrapIndex(first + k, cols);
        std::int8_t* backRow = back.data() + static_cast<std::size_t>(k) * static_cast<std::size_t>(rows);
        for (int r = 0; r < rows; ++r) {
            double best = inf;
            int bestStep = 0;
            for (const int s : steps) {
                const int from = r - s;
                if (from < 0 || from >= rows) {
                    continue;
                }
                const double v = prev[static_cast<std::size_t>(from)] + stepPenalty * static_cast<double>(std::abs(s));
                if (v < best) {
                    best = v;
                    bestStep = s;
                }
            }
            cur[static_cast<std::size_t>(r)] = best + cell(c, r);
            backRow[r] = static_cast<std::int8_t>(bestStep);
        }
        std::swap(prev, cur);
    }

    // Choose the end: free, or within reach of the anchor (plus the cost of
    // the step that closes the ring).
    double best = inf;
    int end = -1;
    for (int r = 0; r < rows; ++r) {
        double v = prev[static_cast<std::size_t>(r)];
        if (endAnchor >= 0) {
            const int d = std::abs(r - endAnchor);
            if (d > maxStep) {
                continue;
            }
            v += stepPenalty * static_cast<double>(d);
        }
        if (v < best) {
            best = v;
            end = r;
        }
    }
    if (end < 0) {
        // Nothing reaches the anchor (cannot happen for a pinned start that
        // IS the anchor, but never index with -1).
        end = endAnchor >= 0 ? std::clamp(endAnchor, 0, rows - 1) : 0;
    }
    path.assign(static_cast<std::size_t>(count), 0);
    int r = end;
    for (int k = count - 1; k >= 0; --k) {
        path[static_cast<std::size_t>(k)] = r;
        if (k > 0) {
            const int s =
                back[static_cast<std::size_t>(k) * static_cast<std::size_t>(rows) + static_cast<std::size_t>(r)];
            r = std::clamp(r - s, 0, rows - 1);
        }
    }
}

}  // namespace

// ===========================================================================
//  Hooks and geometry
// ===========================================================================
void setSeamPenaltyHook(SeamPenaltySlot slot, const SeamPenaltyHook& hook) noexcept {
    const auto i = static_cast<std::size_t>(slot);
    if (i >= static_cast<std::size_t>(SeamPenaltySlot::Count)) {
        return;
    }
    HookSlots& slots = hookSlots();
    std::lock_guard<std::mutex> lock(slots.mutex);
    slots.hooks[i] = hook;
}

SeamPenaltyHook seamPenaltyHook(SeamPenaltySlot slot) noexcept {
    const auto i = static_cast<std::size_t>(slot);
    if (i >= static_cast<std::size_t>(SeamPenaltySlot::Count)) {
        return {};
    }
    HookSlots& slots = hookSlots();
    std::lock_guard<std::mutex> lock(slots.mutex);
    return slots.hooks[i];
}

bool bandPixelDirection(const LensBands& bands, double col, double row, double d[3]) noexcept {
    if (!d) {
        return false;
    }
    d[0] = d[1] = d[2] = 0.0;
    if (bands.w == 0 || bands.mapH == 0 || !std::isfinite(col) || !std::isfinite(row)) {
        return false;
    }
    const double lon = (col + 0.5) * osv::kTwoPi / static_cast<double>(bands.w) - osv::kPi;
    const double lat =
        osv::kHalfPi - (static_cast<double>(bands.rowOffset) + row + 0.5) * osv::kPi / static_cast<double>(bands.mapH);
    const double cl = std::cos(lat);
    d[0] = cl * std::sin(lon);
    d[1] = std::sin(lat);
    d[2] = cl * std::cos(lon);
    return true;
}

// ===========================================================================
//  BlendSeam
// ===========================================================================
bool BlendSeam::valid() const noexcept {
    if (columns == 0 || table.size() != static_cast<std::size_t>(columns) * 2u) {
        return false;
    }
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (!std::isfinite(table[i])) {
            return false;
        }
        if ((i % 2u) == 1u && table[i] < 0.0f) {
            return false;
        }
    }
    // [WP-SEAMTOOLS] The near weight is optional, but when present it is one
    // weight in [0, 1] per column - an offset composer indexes it by column.
    if (!nearWeight.empty()) {
        if (nearWeight.size() != static_cast<std::size_t>(columns)) {
            return false;
        }
        for (const float v : nearWeight) {
            if (!(v >= 0.0f && v <= 1.0f)) {
                return false;  // NaN fails both comparisons
            }
        }
    }
    return std::isfinite(edgeRad) && edgeRad >= 0.0f;
}

// ===========================================================================
//  The DP
// ===========================================================================
Result<std::vector<int>> solveSeamDp(const std::vector<float>& cost, std::uint32_t cols, std::uint32_t rows,
                                     int maxStep, double stepPenalty, bool closedRing) {
    if (cols == 0 || rows == 0 || cols > 65536u || rows > 65536u) {
        return Error{ErrorCode::InvalidArgument, "solveSeamDp: empty or oversized grid"};
    }
    if (cost.size() != static_cast<std::size_t>(cols) * rows) {
        return Error{ErrorCode::InvalidArgument, "solveSeamDp: cost grid does not match its size"};
    }
    if (maxStep < 0 || maxStep > 127 || !std::isfinite(stepPenalty) || stepPenalty < 0.0) {
        return Error{ErrorCode::InvalidArgument, "solveSeamDp: bad step limit or penalty"};
    }
    const int C = static_cast<int>(cols);
    const int R = static_cast<int>(rows);
    std::vector<int> path;
    if (!closedRing || C < 4) {
        dpPass(cost, C, R, 0, C, maxStep, stepPenalty, -1, -1, path);
        return path;
    }
    // Closed ring.  First an open pass over the second half of the ring and
    // on to column 0: after half a turn of burn-in the path no longer
    // remembers where it started, so its row at column 0 is a sound anchor.
    // Then the real pass, pinned to that row at column 0 and required to come
    // back within one step of it, so the seam closes on itself.
    const int half = C / 2;
    std::vector<int> burnIn;
    dpPass(cost, C, R, half, C - half + 1, maxStep, stepPenalty, -1, -1, burnIn);
    const int anchor = burnIn.back();
    dpPass(cost, C, R, 0, C, maxStep, stepPenalty, anchor, anchor, path);
    return path;
}

// ===========================================================================
//  Band correction
// ===========================================================================
Result<LensBands> correctBandsForSeam(const LensBands& bands, const SeamCorrection& correction, ThreadPool* pool) {
    return correctBandsImpl(bands, correction, pool, nullptr);
}

// ===========================================================================
//  The carve
// ===========================================================================
Result<BlendSeam> carveSeamFromBands(const LensBands& bands, const SeamCorrection& correction,
                                     const SeamCarveParams& params, const BlendSeam* prior, ThreadPool* pool) {
    const auto t0 = Clock::now();

    OSV_TRY(checkParams(params));
    OSV_TRY(checkBands(bands, "carveSeam"));
    if (bands.h < 5) {
        return Error{ErrorCode::InvalidArgument, "carveSeam: the band needs at least five rows"};
    }
    if (bands.w % params.columns != 0) {
        return Error{ErrorCode::InvalidArgument, "carveSeam: the band width (" + std::to_string(bands.w) +
                                                     ") is not a multiple of the seam columns (" +
                                                     std::to_string(params.columns) + ")"};
    }

    // ---- 1. the bands as the kernel will blend them ------------------------
    std::vector<float> disparity;
    OSV_TRY_ASSIGN(LensBands wb, correctBandsImpl(bands, correction, pool, &disparity));
    const double correctMs = msSince(t0);
    const auto tCost = Clock::now();

    const std::uint32_t W = wb.w;
    const std::uint32_t H = wb.h;
    const std::uint32_t N = params.columns;
    const std::uint32_t kc = W / N;  // band columns per seam column
    const std::size_t n = static_cast<std::size_t>(W) * H;
    const double rowsPerDeg = static_cast<double>(wb.mapH) / 180.0;
    const double radPerRow = osv::kPi / static_cast<double>(wb.mapH);
    const float blocked = static_cast<float>(params.coverageBlocked);

    // ---- 2. per-pixel disagreement (band resolution) -----------------------
    // A lens "delivers" a pixel when its coverage is above the blocked level
    // and its luma is a number.
    std::vector<std::uint8_t> valid0(n, 0);
    std::vector<std::uint8_t> valid1(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        valid0[i] = (wb.alpha[0][i] > blocked && std::isfinite(wb.luma[0][i])) ? 1 : 0;
        valid1[i] = (wb.alpha[1][i] > blocked && std::isfinite(wb.luma[1][i])) ? 1 : 0;
    }
    std::vector<float> diff(n, 0.0f);  // |L1 - L0|
    std::vector<float> grad(n, 0.0f);  // |grad L1 - grad L0|: structure, blind to an exposure offset
    forRange(pool, H, 1, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            const std::size_t up = (r > 0 ? r - 1 : r) * W;
            const std::size_t dn = (r + 1 < H ? r + 1 : r) * W;
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::size_t i = r * W + x;
                if (!valid0[i] || !valid1[i]) {
                    continue;  // coverage decides there, not disagreement
                }
                diff[i] = std::fabs(wb.luma[1][i] - wb.luma[0][i]);
                // Central differences, only across neighbours both lenses
                // deliver (a coverage edge is black, not structure).
                const std::size_t xl =
                    r * W + static_cast<std::size_t>(wrapIndex(static_cast<int>(x) - 1, static_cast<int>(W)));
                const std::size_t xr =
                    r * W + static_cast<std::size_t>(wrapIndex(static_cast<int>(x) + 1, static_cast<int>(W)));
                const std::size_t yu = up + x;
                const std::size_t yd = dn + x;
                float g = 0.0f;
                if (valid0[xl] && valid1[xl] && valid0[xr] && valid1[xr]) {
                    const float gx0 = 0.5f * (wb.luma[0][xr] - wb.luma[0][xl]);
                    const float gx1 = 0.5f * (wb.luma[1][xr] - wb.luma[1][xl]);
                    g += std::fabs(gx1 - gx0);
                }
                if (valid0[yu] && valid1[yu] && valid0[yd] && valid1[yd]) {
                    const float gy0 = 0.5f * (wb.luma[0][yd] - wb.luma[0][yu]);
                    const float gy1 = 0.5f * (wb.luma[1][yd] - wb.luma[1][yu]);
                    g += std::fabs(gy1 - gy0);
                }
                grad[i] = g;
            }
        }
    });

    // ---- 3. per-lens penalties from the installed hooks ---------------------
    // Summed, weighted, into one map per lens.  A hook that misbehaves (NaN,
    // negative, resized maps) contributes nothing rather than steering the
    // seam with garbage.
    std::vector<float> pen0;
    std::vector<float> pen1;
    bool havePenalty = false;
    {
        std::vector<SeamPenaltyHook> hooks;
        if (params.penalty.installed()) {
            hooks.push_back(params.penalty);
        }
        for (std::size_t s = 0; s < static_cast<std::size_t>(SeamPenaltySlot::Count); ++s) {
            const SeamPenaltyHook h = seamPenaltyHook(static_cast<SeamPenaltySlot>(s));
            if (h.installed()) {
                hooks.push_back(h);
            }
        }
        std::vector<float> h0;
        std::vector<float> h1;
        for (const SeamPenaltyHook& hook : hooks) {
            if (!(hook.weight > 0.0) || !std::isfinite(hook.weight)) {
                continue;
            }
            h0.assign(n, 0.0f);
            h1.assign(n, 0.0f);
            if (!hook.fn(wb, h0, h1, hook.user)) {
                continue;
            }
            if (h0.size() != n || h1.size() != n) {
                log::warn("seam carve: a penalty hook resized its maps; ignoring it");
                continue;
            }
            if (!havePenalty) {
                pen0.assign(n, 0.0f);
                pen1.assign(n, 0.0f);
                havePenalty = true;
            }
            const float wgt = static_cast<float>(hook.weight);
            for (std::size_t i = 0; i < n; ++i) {
                pen0[i] += (std::isfinite(h0[i]) && h0[i] > 0.0f) ? wgt * h0[i] : 0.0f;
                pen1[i] += (std::isfinite(h1[i]) && h1[i] > 0.0f) ? wgt * h1[i] : 0.0f;
            }
        }
    }

    // ---- 4. pool into seam columns -------------------------------------------
    // Means over the kc band columns of each seam column; "blocked" if ANY of
    // them is, "thin" (a lens below coverageMin) on the mean coverage.
    const std::size_t m = static_cast<std::size_t>(N) * H;
    std::vector<double> localC(m, 0.0), gradC(m, 0.0), def0C(m, 0.0), def1C(m, 0.0), pen0C(m, 0.0), pen1C(m, 0.0);
    std::vector<std::uint8_t> blockC(m, 0);
    std::vector<std::uint8_t> thinC(m, 0);
    const double covMin = params.coverageMin;
    const double invKc = 1.0 / static_cast<double>(kc);
    forRange(pool, H, 1, [&](std::size_t r0, std::size_t r1) {
        for (std::size_t r = r0; r < r1; ++r) {
            for (std::uint32_t c = 0; c < N; ++c) {
                double sd = 0, sg = 0, d0 = 0, d1 = 0, p0 = 0, p1 = 0, a0 = 0, a1 = 0;
                bool blk = false;
                for (std::uint32_t k = 0; k < kc; ++k) {
                    const std::size_t i = r * W + static_cast<std::size_t>(c) * kc + k;
                    sd += diff[i];
                    sg += grad[i];
                    const double al0 = std::isfinite(wb.alpha[0][i]) ? wb.alpha[0][i] : 0.0;
                    const double al1 = std::isfinite(wb.alpha[1][i]) ? wb.alpha[1][i] : 0.0;
                    a0 += al0;
                    a1 += al1;
                    d0 += std::clamp((covMin - al0) / covMin, 0.0, 1.0);
                    d1 += std::clamp((covMin - al1) / covMin, 0.0, 1.0);
                    blk = blk || !valid0[i] || !valid1[i];
                    if (havePenalty) {
                        p0 += pen0[i];
                        p1 += pen1[i];
                    }
                }
                const std::size_t j = r * N + c;
                gradC[j] = sg * invKc;
                localC[j] = params.diffWeight * (sd * invKc) + params.gradWeight * gradC[j];
                def0C[j] = d0 * invKc;
                def1C[j] = d1 * invKc;
                pen0C[j] = p0 * invKc;
                pen1C[j] = p1 * invKc;
                blockC[j] = blk ? 1 : 0;
                thinC[j] = (a0 * invKc < covMin || a1 * invKc < covMin) ? 1 : 0;
            }
        }
    });

    // ---- 5. temporal hold, relaxed where the content is near --------------
    // Mean of the largest fifth of the column's vertical disparity (band
    // rows): a column holding something close to the camera may follow it,
    // a far-field column should stay put (reference implementation: 4 px,
    // floor 0.1).
    std::vector<double> holdC(N, 1.0);
    std::vector<double> priorRow(N, -1.0);
    bool usePrior = false;
    if (prior && prior->valid() && prior->columns == N) {
        usePrior = true;
        for (std::uint32_t c = 0; c < N; ++c) {
            const double lat = static_cast<double>(prior->table[static_cast<std::size_t>(c) * 2u]);
            priorRow[c] = (osv::kHalfPi - lat) / radPerRow - static_cast<double>(wb.rowOffset) - 0.5;
        }
        std::vector<float> sample;
        sample.reserve(static_cast<std::size_t>(kc) * H);
        for (std::uint32_t c = 0; c < N; ++c) {
            sample.clear();
            for (std::uint32_t r = 0; r < H; ++r) {
                for (std::uint32_t k = 0; k < kc; ++k) {
                    const std::size_t i = static_cast<std::size_t>(r) * W + static_cast<std::size_t>(c) * kc + k;
                    if (valid0[i] && valid1[i] && std::isfinite(disparity[i])) {
                        sample.push_back(disparity[i]);
                    }
                }
            }
            double top = 0.0;
            if (!sample.empty()) {
                const std::size_t keep = std::max<std::size_t>(1, sample.size() / 5);
                std::nth_element(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(sample.size() - keep),
                                 sample.end());
                double s = 0.0;
                for (std::size_t k = sample.size() - keep; k < sample.size(); ++k) {
                    s += sample[k];
                }
                top = s / static_cast<double>(keep);
            }
            const double near = std::clamp(top / params.temporalRelaxRows, 0.0, 1.0);
            holdC[c] = params.temporalFloor + (1.0 - params.temporalFloor) * (1.0 - near);
        }
    } else if (prior && prior->columns != N) {
        log::debug("seam carve: prior seam has {} columns, not {}; carving without it", prior->columns, N);
    }

    // ---- 6. the cost of a seam at each (row, column) -----------------------
    // Cost window: +/- mw rows around the seam row, where the default narrow
    // feather mixes both lenses.  [WP-SEAMTOOLS] Its own parameter (equal to
    // the default narrow feather), so the Source Settings feather widths
    // never move the seam.
    const int mw = std::max(1, static_cast<int>(std::lround(params.costWindowDeg * rowsPerDeg)));
    const int Hi = static_cast<int>(H);
    const double rc = 0.5 * static_cast<double>(Hi - 1);  // the geometric seam: the band is symmetric about it
    const double halfH = std::max(1.0, 0.5 * static_cast<double>(Hi));
    const double normRows = std::max(0.5, params.temporalNormDeg * rowsPerDeg);
    const double clampRows = params.temporalClampDeg * rowsPerDeg;
    // A move past the clamp is prohibitive but still below a coverage
    // violation: seeing the scene beats holding still.
    const double clampCost = 0.1 * params.forbiddenCost;
    std::vector<float> cost(m, 0.0f);
    forRange(pool, N, 16, [&](std::size_t cBegin, std::size_t cEnd) {
        // Prefix sums over rows for one column, reused for every row.
        std::vector<double> sLocal(H + 1u), sD0(H + 1u), sD1(H + 1u), sP0(H + 1u), sP1(H + 1u), sBlk(H + 1u);
        for (std::size_t c = cBegin; c < cEnd; ++c) {
            sLocal[0] = sD0[0] = sD1[0] = sP0[0] = sP1[0] = sBlk[0] = 0.0;
            for (std::uint32_t r = 0; r < H; ++r) {
                const std::size_t j = static_cast<std::size_t>(r) * N + c;
                sLocal[r + 1] = sLocal[r] + localC[j];
                sD0[r + 1] = sD0[r] + def0C[j];
                sD1[r + 1] = sD1[r] + def1C[j];
                sP0[r + 1] = sP0[r] + pen0C[j];
                sP1[r + 1] = sP1[r] + pen1C[j];
                sBlk[r + 1] = sBlk[r] + static_cast<double>(blockC[j]);
            }
            for (int r = 0; r < Hi; ++r) {
                const std::size_t j = static_cast<std::size_t>(r) * N + c;
                const int lo = r - mw;
                const int hi = r + mw;
                if (lo < 0 || hi > Hi - 1) {
                    // The feather would leave the band: nothing is known
                    // about the lenses there.
                    cost[j] = static_cast<float>(params.forbiddenCost);
                    continue;
                }
                const auto L = static_cast<std::size_t>(lo);
                const auto U = static_cast<std::size_t>(hi) + 1u;  // one past the window
                // Disagreement inside the feather window.
                double v = (sLocal[U] - sLocal[L]) / static_cast<double>(hi - lo + 1);
                // Coverage: master shown on rows [0, hi], slave on [lo, H).
                v += params.coverageWeight * (sD1[U] + (sD0[H] - sD0[L]));
                // Installed penalties: the cost of SHOWING each lens there.
                if (havePenalty) {
                    v += sP1[U] + (sP0[H] - sP0[L]);
                }
                // A lens that cannot take part at all inside the window.
                if (sBlk[U] - sBlk[L] > 0.5) {
                    v += params.forbiddenCost;
                }
                // Pull toward the geometric seam.
                v += params.centreWeight * std::fabs(static_cast<double>(r) - rc) / halfH;
                // Temporal hold and clamp.
                if (usePrior) {
                    const double dist = std::fabs(static_cast<double>(r) - priorRow[c]);
                    v += params.temporalWeight * holdC[c] * std::min(1.0, dist / normRows);
                    if (dist > clampRows) {
                        v += clampCost;
                    }
                }
                cost[j] = static_cast<float>(v);
            }
        }
    });
    const double costMs = msSince(tCost);

    // ---- 7. the DP on the closed longitude ring ------------------------------
    const auto tDp = Clock::now();
    OSV_TRY_ASSIGN(std::vector<int> path, solveSeamDp(cost, N, H, params.maxStepRows, params.stepPenalty, true));
    const double dpMs = msSince(tDp);

    BlendSeam seam;
    seam.columns = N;
    seam.usedPrior = usePrior;
    for (std::uint32_t c = 0; c < N; ++c) {
        const std::size_t j = static_cast<std::size_t>(path[c]) * N + c;
        if (cost[j] >= 0.5 * params.forbiddenCost) {
            ++seam.forcedColumns;
        }
    }

    // ---- 8. smooth the path, re-apply the hard limits ------------------------
    std::vector<double> rows(N);
    for (std::uint32_t c = 0; c < N; ++c) {
        rows[c] = static_cast<double>(path[c]);
    }
    rows = smoothRing(rows, params.smoothSigmaCols);
    for (std::uint32_t c = 0; c < N; ++c) {
        double r = rows[c];
        if (usePrior) {
            r = std::clamp(r, priorRow[c] - clampRows, priorRow[c] + clampRows);
        }
        rows[c] = std::clamp(r, static_cast<double>(mw), static_cast<double>(Hi - 1 - mw));
    }

    // ---- 9. feather width per column -----------------------------------------
    // Wide where the lenses agree structurally along the seam, narrow where
    // they do not.
    std::vector<double> width(N, params.wideHalfWidthDeg);
    std::vector<double> residual(N, 0.0);
    // [WP-SEAMTOOLS] The same disagreement weight, kept for the Near / Far
    // Offset (BlendSeam::nearWeight): 0 = the lenses agree, 1 = they do not.
    std::vector<double> disagree(N, 0.0);
    for (std::uint32_t c = 0; c < N; ++c) {
        const int rr = std::clamp(static_cast<int>(std::lround(rows[c])), 0, Hi - 1);
        double e = 0.0;
        double cnt = 0.0;
        for (int r = std::max(0, rr - mw); r <= std::min(Hi - 1, rr + mw); ++r) {
            e += gradC[static_cast<std::size_t>(r) * N + c];
            cnt += 1.0;
        }
        e = cnt > 0.0 ? e / cnt : 0.0;
        residual[c] = e;
        const double t = smoothstep01((e - params.agreeResidual) / (params.disagreeResidual - params.agreeResidual));
        width[c] = params.wideHalfWidthDeg + (params.narrowHalfWidthDeg - params.wideHalfWidthDeg) * t;
        disagree[c] = t;
    }
    width = smoothRing(width, params.widthSigmaCols);
    // [WP-SEAMTOOLS] Smoothed along the ring on its own (wider) scale; a
    // Gaussian of values in [0, 1] stays in [0, 1], the clamp only absorbs
    // rounding.
    disagree = smoothRing(disagree, params.nearSigmaCols);

    seam.table.assign(static_cast<std::size_t>(N) * 2u, 0.0f);
    seam.nearWeight.assign(N, 0.0f);  // [WP-SEAMTOOLS]
    for (std::uint32_t c = 0; c < N; ++c) {
        seam.nearWeight[c] = static_cast<float>(std::clamp(disagree[c], 0.0, 1.0));
    }
    double latSum = 0.0, latMax = 0.0, widthSum = 0.0, residualSum = 0.0, priorStep = 0.0;
    for (std::uint32_t c = 0; c < N; ++c) {
        // Never so wide that the feather reaches a row where either lens is
        // thin: count the rows of room above and below the seam.
        const int rr = std::clamp(static_cast<int>(std::lround(rows[c])), 0, Hi - 1);
        int up = 0;
        while (rr - up - 1 >= 0 && !thinC[static_cast<std::size_t>(rr - up - 1) * N + c]) {
            ++up;
        }
        int down = 0;
        while (rr + down + 1 <= Hi - 1 && !thinC[static_cast<std::size_t>(rr + down + 1) * N + c]) {
            ++down;
        }
        const double roomDeg = (static_cast<double>(std::min(up, down)) + 0.5) / rowsPerDeg;
        // Never below a quarter of the narrow width: a (near) hard cut is
        // still better than a feather into a lens that cannot see.
        const double hwDeg = std::max(std::min(width[c], roomDeg), 0.25 * params.narrowHalfWidthDeg);
        const double lat = osv::kHalfPi - (static_cast<double>(wb.rowOffset) + rows[c] + 0.5) * radPerRow;
        seam.table[static_cast<std::size_t>(c) * 2u] = static_cast<float>(lat);
        seam.table[static_cast<std::size_t>(c) * 2u + 1u] = static_cast<float>(osv::deg2rad(hwDeg));
        latSum += osv::rad2deg(lat);
        latMax = std::max(latMax, std::fabs(osv::rad2deg(lat)));
        widthSum += hwDeg;
        residualSum += residual[c];
        if (hwDeg < 0.5 * (params.narrowHalfWidthDeg + params.wideHalfWidthDeg)) {
            ++seam.narrowColumns;
        }
        if (usePrior) {
            priorStep = std::max(priorStep, std::fabs(rows[c] - priorRow[c]) / rowsPerDeg);
        }
    }
    seam.edgeRad = static_cast<float>(osv::deg2rad(params.edgeRampDeg));
    seam.meanLatDeg = latSum / static_cast<double>(N);
    seam.maxAbsLatDeg = latMax;
    seam.meanHalfWidthDeg = widthSum / static_cast<double>(N);
    seam.meanResidual = residualSum / static_cast<double>(N);
    seam.maxPriorStepDeg = priorStep;
    seam.correctMs = correctMs;
    seam.costMs = costMs;
    seam.dpMs = dpMs;
    seam.carveMs = msSince(t0);
    if (!seam.valid()) {
        return Error{ErrorCode::Internal, "carveSeam: produced a non-finite seam"};
    }
    return seam;
}

Result<BlendSeam> carveSeam(const geom::LensRig& rig, const video::FramePair& frames, const geom::BlendParams& blend,
                            const BandParams& band, const SeamCorrection& correction, const SeamCarveParams& params,
                            const BlendSeam* prior, ThreadPool& pool) {
    // Uncorrected, code-space bands: the correction is applied in band space
    // by carveSeamFromBands, so these and the parallax measurement's bands
    // are interchangeable inputs.
    OSV_TRY_ASSIGN(LensBands bands, renderLensBands(rig, frames, blend, band, false, nullptr, pool));
    return carveSeamFromBands(bands, correction, params, prior, &pool);
}

Result<BlendSeam> blendSeams(const BlendSeam& from, const BlendSeam& to, double t) {
    if (!from.valid() || !to.valid()) {
        return Error{ErrorCode::InvalidArgument, "blendSeams: a seam is empty or malformed"};
    }
    if (from.columns != to.columns) {
        return Error{ErrorCode::InvalidArgument, "blendSeams: the two seams have different column counts"};
    }
    if (!std::isfinite(t)) {
        return Error{ErrorCode::InvalidArgument, "blendSeams: non-finite blend weight"};
    }
    const float k = static_cast<float>(std::clamp(t, 0.0, 1.0));
    BlendSeam out = to;
    // Written as a weighted sum rather than from + (to - from) k so that the
    // ends are EXACT: the last frame of a bucket (t = 1) renders precisely
    // that bucket's own seam, not one rounding step away from it.
    for (std::size_t i = 0; i < out.table.size(); ++i) {
        out.table[i] = from.table[i] * (1.0f - k) + to.table[i] * k;
    }
    // [WP-SEAMTOOLS] The near weight glides with the seam, by the same exact-
    // ends rule; a seam without one leaves `to`'s (possibly empty) as it is.
    if (from.nearWeight.size() == to.nearWeight.size() && !to.nearWeight.empty()) {
        for (std::size_t i = 0; i < out.nearWeight.size(); ++i) {
            out.nearWeight[i] = std::clamp(from.nearWeight[i] * (1.0f - k) + to.nearWeight[i] * k, 0.0f, 1.0f);
        }
    }
    return out;
}

void applyBlendSeam(RenderParamsBuilder& builder, const BlendSeam& seam) {
    if (!seam.valid()) {
        builder.clearBlendSeam();
        return;
    }
    builder.blendSeam(seam.table, seam.columns, seam.edgeRad);
}

}  // namespace osv::render
