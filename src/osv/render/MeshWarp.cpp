// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MeshWarp.cpp - the content-preserving mesh warp over the seam band: the
// lattice, the energy terms, the banded solver, the IRLS loop, the entry
// points, and the line-straightness metric.  The reasoning behind the energy
// is in MeshWarp.h; the line detector lives in MeshWarpLines.cpp.
//
// The order of work inside solveMeshWarp, each step explained at its site:
//
//   1. the lattice and its unknowns (two blocks: dLon, dLat; the ring folded
//      so each block is a band matrix);
//   2. one pass over each measurement: the structured gate's counts (exactly
//      gridFromFlow's, on the primary measurement) and the flow matches, each
//      a bilinear stencil; where both measurements matched a pixel, each
//      counts half;
//   3. the co-visibility of every vertex (how far the anchor lets data reach);
//   4. the line triples from the detected segments;
//   5. the shape terms (membrane + bending of V - V0);
//   6. IRLS: assemble both blocks, factor them, solve them, then reweight
//      (the benefit gate once, Cauchy on matches when asked, Huber on time);
//   7. the energies, the report and the grid.

#include "osv/render/MeshWarp.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"
#include "osv/render/osv_kernel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <initializer_list>
#include <limits>
#include <tuple>
#include <utility>

namespace osv::render {

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds elapsed since `t`.
[[nodiscard]] double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

/// Smoothstep on [0, 1], clamped outside; a NaN argument gives 0.
[[nodiscard]] double smoothstep01(double t) noexcept {
    if (!std::isfinite(t)) {
        return 0.0;
    }
    const double c = std::clamp(t, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

/// Run `body(task)` for every task in [0, count), across `pool` when it has
/// more than one worker, otherwise inline.  A failed parallel run is redone
/// inline: every body below only overwrites its own outputs.
void forTasks(ThreadPool* pool, std::size_t count, const std::function<void(std::size_t)>& body) {
    if (pool != nullptr && pool->size() > 1 && count > 1) {
        if (pool->parallelRows(count, 1, body).ok()) {
            return;
        }
        log::warn("mesh warp: a parallel pass failed, rerunning it on the calling thread");
    }
    for (std::size_t t = 0; t < count; ++t) {
        body(t);
    }
}

// ===========================================================================
//  1. The lattice
// ===========================================================================

/// Everything about the mesh's geometry and its unknowns.
///
/// UNITS.  Inside the solver every displacement and position is in BAND
/// PIXELS of the polar map the parameters name (params.parallax.band): one
/// pixel of longitude is 2 pi / equirectW radians, one of latitude pi / mapH
/// - equal for the 2:1 map renderLensBands draws - and latitude counts
/// NORTH-positive (the band's rows run south).  The weights are densities
/// per band pixel, so they keep their meaning at any mesh resolution.
///
/// UNKNOWNS.  The rows 0 and H - 1 are pinned to zero (the field's edge);
/// the free rows 1 .. H - 2 (R of them) of every column are unknowns, one
/// block per component.  Within a block the vertex (i, j) has index
/// fold(i) * R + (j - 1), where fold() orders the ring 0, W-1, 1, W-2, ...:
/// two columns at ring distance d sit at most 2 d positions apart, so the
/// terms (which couple columns at most two apart) give a band matrix of half
/// width 4 R + R - 1 - and the ring's wrap costs nothing extra.
struct MeshGeom {
    std::uint32_t W = 0;   ///< Columns (longitude, wraps).
    std::uint32_t H = 0;   ///< Rows including the two pinned edges.
    std::uint32_t R = 0;   ///< Free rows (H - 2).
    std::size_t N = 0;     ///< Free vertices per component (W * R).
    std::size_t bw = 0;    ///< Half bandwidth of each block.
    double reachRad = 0.0;       ///< Latitude of row 0 (row H - 1 is its negation).
    double latStepRad = 0.0;     ///< Latitude between rows.
    double radPerPxLon = 0.0;    ///< One band pixel of longitude, radians.
    double radPerPxLat = 0.0;    ///< One band pixel of latitude, radians.
    double hLonPx = 0.0;         ///< Mesh column spacing in band pixels.
    double hLatPx = 0.0;         ///< Mesh row spacing in band pixels.
    double cellArea = 0.0;       ///< hLonPx * hLatPx: the band area one vertex stands for.
    double bandHalfRad = 0.0;    ///< Half height of the analysed band.
    std::vector<std::uint32_t> foldOf;  ///< Folded ring position of each column.

    /// Latitude of row j.
    [[nodiscard]] double latOfRow(std::uint32_t j) const noexcept {
        return reachRad - static_cast<double>(j) * latStepRad;
    }
    /// True for a pinned row.
    [[nodiscard]] bool pinned(std::uint32_t j) const noexcept { return j == 0 || j + 1u >= H; }
    /// Block index of the free vertex (i, j); j must be free.
    [[nodiscard]] std::uint32_t index(std::uint32_t i, std::uint32_t j) const noexcept {
        return foldOf[i] * R + (j - 1u);
    }
    /// True when row j lies beyond the analysed band (a decay ring).
    [[nodiscard]] bool inRing(std::uint32_t j) const noexcept {
        return std::fabs(latOfRow(j)) > bandHalfRad + 1e-9;
    }
};

/// Build the lattice of `params` (already validated).
[[nodiscard]] MeshGeom makeGeom(const MeshWarpParams& params) {
    MeshGeom g;
    g.W = params.meshCols;
    // 2 x reach is a whole number of spacings (checkMeshWarpParams).
    const double spans = 2.0 * params.reachDeg / params.rowSpacingDeg;
    g.H = static_cast<std::uint32_t>(std::lround(spans)) + 1u;
    g.R = g.H - 2u;
    g.N = static_cast<std::size_t>(g.W) * g.R;
    g.bw = static_cast<std::size_t>(5u * g.R - 1u);
    g.reachRad = deg2rad(params.reachDeg);
    g.latStepRad = 2.0 * g.reachRad / static_cast<double>(g.H - 1u);
    // The band's pixel scale, as renderLensBands draws the polar map: equirectW
    // columns round the ring and equirectW / 2 rows pole to pole.
    const std::uint32_t mapW = params.parallax.band.equirectW;
    const std::uint32_t mapH = mapW / 2u;
    g.radPerPxLon = kTwoPi / static_cast<double>(mapW);
    g.radPerPxLat = kPi / static_cast<double>(mapH);
    g.hLonPx = (kTwoPi / static_cast<double>(g.W)) / g.radPerPxLon;
    g.hLatPx = g.latStepRad / g.radPerPxLat;
    g.cellArea = g.hLonPx * g.hLatPx;
    g.bandHalfRad = deg2rad(params.parallax.band.bandHalfDeg);
    // Fold the ring: position p holds column p / 2 (p even) or W - 1 - (p - 1) / 2 (p odd).
    g.foldOf.assign(g.W, 0u);
    for (std::uint32_t p = 0; p < g.W; ++p) {
        const std::uint32_t col = (p % 2u == 0u) ? p / 2u : g.W - 1u - (p - 1u) / 2u;
        g.foldOf[col] = p;
    }
    return g;
}

/// A bilinear stencil: the free vertices around one point and their weights.
///
/// The fetch is osvWarpSample's (the kernel's), in double: longitude wraps,
/// latitude is linear between rows 0 and H - 1, and a point beyond either
/// edge row has no stencil at all (the field is zero there).  Vertices on a
/// pinned row are left out - their value is zero, so they add nothing.
struct Stencil {
    std::array<std::uint32_t, 4> idx{};  ///< Block indices (same in both blocks).
    std::array<double, 4> w{};           ///< Bilinear weights.
    std::uint8_t n = 0;                  ///< Entries in use.
    bool inside = false;                 ///< False beyond the mesh's latitude span.
    long long col0 = 0;                  ///< Unwrapped floor column (for the band-width check).
    std::uint32_t cellCol = 0;           ///< Mesh cell column (wrapped).
    std::uint32_t cellRow = 0;           ///< Mesh cell row, in [0, H - 2].
};

/// The longitude half of a stencil: the floor column, its wrapped
/// neighbours and the weight between them.
struct StencilCol {
    long long col0 = 0;
    std::uint32_t x0 = 0;
    std::uint32_t x1 = 0;
    double tx = 0.0;
};

/// The latitude half of a stencil: the cell row and the weight between its
/// two rows; `inside` false beyond the edge rows.
struct StencilRow {
    bool inside = false;
    std::uint32_t y0 = 0;
    double ty = 0.0;
};

/// The longitude half at (unwrapped) longitude `lonRad`.
[[nodiscard]] StencilCol stencilCol(const MeshGeom& g, double lonRad) noexcept {
    StencilCol c;
    const double fx = (lonRad + kPi) / kTwoPi * static_cast<double>(g.W);
    const double flx = std::floor(fx);
    c.tx = fx - flx;
    c.col0 = static_cast<long long>(flx);
    const long long Wl = static_cast<long long>(g.W);
    c.x0 = static_cast<std::uint32_t>(((c.col0 % Wl) + Wl) % Wl);
    c.x1 = (c.x0 + 1u) % g.W;
    return c;
}

/// The latitude half at `latRad`.
[[nodiscard]] StencilRow stencilRow(const MeshGeom& g, double latRad) noexcept {
    StencilRow r;
    const double fy = (g.reachRad - latRad) / (2.0 * g.reachRad) * static_cast<double>(g.H - 1u);
    if (!(fy >= 0.0) || fy > static_cast<double>(g.H - 1u)) {
        return r;  // beyond the edge rows (or not a number): the kernel returns zero there
    }
    r.inside = true;
    const double fly = std::floor(fy);
    r.ty = fy - fly;
    r.y0 = static_cast<std::uint32_t>(fly);
    if (r.y0 + 1u >= g.H) {
        // Exactly on the last row: its lower neighbour does not exist and the
        // weight below it is zero anyway.
        r.y0 = g.H - 2u;
        r.ty = 1.0;
    }
    return r;
}

/// A stencil from its two halves.
[[nodiscard]] Stencil makeStencil(const MeshGeom& g, const StencilCol& c, const StencilRow& r) noexcept {
    Stencil s;
    if (!r.inside) {
        return s;
    }
    s.inside = true;
    s.col0 = c.col0;
    s.cellCol = c.x0;
    s.cellRow = r.y0;
    const std::uint32_t y1 = r.y0 + 1u;
    const std::uint32_t xs[4] = {c.x0, c.x1, c.x0, c.x1};
    const std::uint32_t ys[4] = {r.y0, r.y0, y1, y1};
    const double ws[4] = {(1.0 - c.tx) * (1.0 - r.ty), c.tx * (1.0 - r.ty), (1.0 - c.tx) * r.ty, c.tx * r.ty};
    for (int k = 0; k < 4; ++k) {
        if (g.pinned(ys[k]) || !(ws[k] != 0.0)) {
            continue;  // pinned to zero, or no weight: nothing to add
        }
        s.idx[s.n] = g.index(xs[k], ys[k]);
        s.w[s.n] = ws[k];
        ++s.n;
    }
    return s;
}

/// The stencil at (lonRad, latRad).  Longitude may be unwrapped.
[[nodiscard]] Stencil stencilAt(const MeshGeom& g, double lonRad, double latRad) noexcept {
    if (!std::isfinite(lonRad) || !std::isfinite(latRad)) {
        return Stencil{};  // a broken position touches nothing
    }
    return makeStencil(g, stencilCol(g, lonRad), stencilRow(g, latRad));
}

/// The stencil halves of every column and row of a band, computed once: a
/// band pixel's stencil depends on its column and its row separately, so the
/// per-pixel passes only combine two table entries.
struct BandStencils {
    std::vector<StencilCol> cols;
    std::vector<StencilRow> rows;
    std::vector<double> lon;  ///< Longitude of each column's centre (radians).
    std::vector<double> lat;  ///< Latitude of each row's centre (radians).
};

/// The stencil halves of `b`'s columns and rows.
[[nodiscard]] BandStencils bandStencils(const MeshGeom& g, const LensBands& b) {
    BandStencils bs;
    const double radPerCol = kTwoPi / static_cast<double>(b.w);
    const double radPerRow = kPi / static_cast<double>(b.mapH);
    bs.cols.resize(b.w);
    bs.lon.resize(b.w);
    for (std::uint32_t c = 0; c < b.w; ++c) {
        bs.lon[c] = (static_cast<double>(c) + 0.5) * radPerCol - kPi;
        bs.cols[c] = stencilCol(g, bs.lon[c]);
    }
    bs.rows.resize(b.h);
    bs.lat.resize(b.h);
    for (std::uint32_t r = 0; r < b.h; ++r) {
        bs.lat[r] = kHalfPi - (static_cast<double>(b.rowOffset) + static_cast<double>(r) + 0.5) * radPerRow;
        bs.rows[r] = stencilRow(g, bs.lat[r]);
    }
    return bs;
}

/// One component of the field at a stencil, from a block-major vector
/// (component c occupies [c N, (c + 1) N)).
[[nodiscard]] double fieldAt(const Stencil& s, const std::vector<double>& x, std::size_t N, int comp) noexcept {
    double v = 0.0;
    const std::size_t base = static_cast<std::size_t>(comp) * N;
    for (std::uint8_t k = 0; k < s.n; ++k) {
        v += s.w[k] * x[base + s.idx[k]];
    }
    return v;
}

/// One component of any grid at (lonRad, latRad), in RADIANS: the kernel's
/// osvWarpSample in double (wrap in longitude, linear latitude between the
/// first and last row, zero beyond them).  0 for an invalid grid or a
/// non-finite cell.
[[nodiscard]] double sampleGrid(const ParallaxWarpGrid& grid, double lonRad, double latRad, int comp) noexcept {
    if (!grid.valid() || !std::isfinite(lonRad) || !std::isfinite(latRad)) {
        return 0.0;
    }
    const double span = static_cast<double>(grid.latMaxRad) - static_cast<double>(grid.latMinRad);
    if (!(std::fabs(span) > 1e-9)) {
        return 0.0;
    }
    const double fx = (lonRad + kPi) / kTwoPi * static_cast<double>(grid.w);
    const double fy = (latRad - static_cast<double>(grid.latMinRad)) / span * static_cast<double>(grid.h - 1u);
    if (fy < 0.0 || fy > static_cast<double>(grid.h - 1u)) {
        return 0.0;
    }
    const double flx = std::floor(fx);
    const double fly = std::floor(fy);
    const double tx = fx - flx;
    const double ty = fy - fly;
    const long long Wl = static_cast<long long>(grid.w);
    const auto x0 = static_cast<std::size_t>(((static_cast<long long>(flx) % Wl) + Wl) % Wl);
    const std::size_t x1 = (x0 + 1u) % grid.w;
    const auto y0 = static_cast<std::size_t>(std::clamp(fly, 0.0, static_cast<double>(grid.h - 1u)));
    const std::size_t y1 = std::min<std::size_t>(y0 + 1u, grid.h - 1u);
    const auto at = [&](std::size_t xx, std::size_t yy) {
        const double v = static_cast<double>(grid.uv[(yy * grid.w + xx) * 2u + static_cast<std::size_t>(comp)]);
        return std::isfinite(v) ? v : 0.0;
    };
    const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const double bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bot - top) * ty;
}

/// True when `grid` has exactly the mesh's layout.
[[nodiscard]] bool hasMeshLayout(const ParallaxWarpGrid& grid, const ParallaxWarpGrid& layout) noexcept {
    return grid.valid() && grid.w == layout.w && grid.h == layout.h &&
           std::fabs(static_cast<double>(grid.latMinRad) - static_cast<double>(layout.latMinRad)) < 1e-6 &&
           std::fabs(static_cast<double>(grid.latMaxRad) - static_cast<double>(layout.latMaxRad)) < 1e-6;
}

/// A mesh-layout grid as a block-major vector of free-vertex values (band
/// pixels).  The pinned rows are left out (they are zero by definition); a
/// non-finite cell counts as zero.
[[nodiscard]] std::vector<double> gridToVector(const MeshGeom& g, const ParallaxWarpGrid& grid) {
    std::vector<double> x(2u * g.N, 0.0);
    for (std::uint32_t j = 1; j + 1u < g.H; ++j) {
        for (std::uint32_t i = 0; i < g.W; ++i) {
            const std::size_t cell = (static_cast<std::size_t>(j) * g.W + i) * 2u;
            const double lon = static_cast<double>(grid.uv[cell + 0u]);
            const double lat = static_cast<double>(grid.uv[cell + 1u]);
            const std::uint32_t v = g.index(i, j);
            x[v] = std::isfinite(lon) ? lon / g.radPerPxLon : 0.0;
            x[g.N + v] = std::isfinite(lat) ? lat / g.radPerPxLat : 0.0;
        }
    }
    return x;
}

// ===========================================================================
//  The banded SPD block and its Cholesky factor
// ===========================================================================

/// Inner product of two contiguous runs with four independent accumulators.
///
/// Under /fp:precise one accumulator is a serial chain of dependent adds (the
/// compiler may not reorder them); four partial sums run side by side and
/// the factorisation's inner products - nearly all of its time - go about
/// three times faster.  The order is fixed, so the result is deterministic.
[[nodiscard]] double dotUnrolled(const double* a, const double* b, std::size_t len) noexcept {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    std::size_t k = 0;
    for (; k + 4u <= len; k += 4u) {
        s0 += a[k] * b[k];
        s1 += a[k + 1u] * b[k + 1u];
        s2 += a[k + 2u] * b[k + 2u];
        s3 += a[k + 3u] * b[k + 3u];
    }
    for (; k < len; ++k) {
        s0 += a[k] * b[k];
    }
    return (s0 + s1) + (s2 + s3);
}

/// A symmetric positive definite N x N matrix of half bandwidth bw, stored
/// as its lower band row by row (row i holds columns i - bw .. i, padded at
/// the top), factored in place by Cholesky.
///
/// The factorisation is the row-oriented (Crout) banded Cholesky: every
/// inner product runs over two contiguous rows of the band, so it costs
/// N bw^2 / 2 multiply-adds and streams through memory in order.  Solving
/// with the factor is a forward and a backward sweep of N bw each.
class BandedSpd {
public:
    /// Size the matrix and zero it.
    void reset(std::size_t n, std::size_t bw) {
        m_n = n;
        m_bw = bw;
        m_a.assign(n * (bw + 1u), 0.0);
        m_factored = false;
    }

    /// Add `v` to A(i, j) (and so to A(j, i)).  False - and nothing added -
    /// when (i, j) lies outside the band, which the term builders rule out
    /// before they get here.
    bool add(std::size_t i, std::size_t j, double v) noexcept {
        if (i < j) {
            std::swap(i, j);
        }
        if (i >= m_n || i - j > m_bw || m_factored) {
            return false;
        }
        m_a[i * (m_bw + 1u) + (j + m_bw - i)] += v;
        return true;
    }

    /// y = A x for the UNFACTORED matrix (x, y of size N; y overwritten).
    void multiply(const double* x, double* y) const noexcept {
        for (std::size_t i = 0; i < m_n; ++i) {
            y[i] = 0.0;
        }
        if (m_factored) {
            return;  // the factor is not the matrix: nothing meaningful to multiply
        }
        for (std::size_t i = 0; i < m_n; ++i) {
            const double* row = m_a.data() + i * (m_bw + 1u);
            const std::size_t j0 = i > m_bw ? i - m_bw : 0u;
            const double* band = row + (j0 + m_bw - i);
            const double xi = x[i];
            for (std::size_t j = j0; j < i; ++j) {
                y[j] += band[j - j0] * xi;  // the symmetric upper half (independent updates)
            }
            y[i] += dotUnrolled(band, x + j0, i - j0) + row[m_bw] * xi;
        }
    }

    /// A copy of another block's (unfactored) matrix.
    void copyFrom(const BandedSpd& other) {
        m_n = other.m_n;
        m_bw = other.m_bw;
        m_a = other.m_a;
        m_factored = other.m_factored;
    }

    /// Factor A = L L^T in place.  False when A is not positive definite (a
    /// pivot fell to or below a tiny fraction of its diagonal, or is not a
    /// number); the matrix is then unusable.
    [[nodiscard]] bool factor() {
        if (m_factored) {
            return true;
        }
        const std::size_t stride = m_bw + 1u;
        for (std::size_t i = 0; i < m_n; ++i) {
            double* rowI = m_a.data() + i * stride;
            const std::size_t j0 = i > m_bw ? i - m_bw : 0u;
            for (std::size_t j = j0; j <= i; ++j) {
                const double* rowJ = m_a.data() + j * stride;
                // Inner product of rows i and j over columns j0 .. j - 1: both
                // are contiguous runs of the band storage.
                const double* a = rowI + (j0 + m_bw - i);
                const double* b = rowJ + (j0 + m_bw - j);
                const double s = rowI[j + m_bw - i] - dotUnrolled(a, b, j - j0);
                if (j < i) {
                    rowI[j + m_bw - i] = s / rowJ[m_bw];
                } else {
                    const double diag = rowI[m_bw];
                    if (!(s > 1e-14 * std::max(1.0, std::fabs(diag))) || !std::isfinite(s)) {
                        return false;
                    }
                    rowI[m_bw] = std::sqrt(s);
                }
            }
        }
        m_factored = true;
        return true;
    }

    /// x = A^-1 x with the factor (factor() must have succeeded).
    void solveInPlace(double* x) const noexcept {
        if (!m_factored) {
            return;
        }
        const std::size_t stride = m_bw + 1u;
        // Forward: L y = x.
        for (std::size_t i = 0; i < m_n; ++i) {
            const double* row = m_a.data() + i * stride;
            const std::size_t j0 = i > m_bw ? i - m_bw : 0u;
            x[i] = (x[i] - dotUnrolled(row + (j0 + m_bw - i), x + j0, i - j0)) / row[m_bw];
        }
        // Backward: L^T x = y, row i of L scattered once x_i is known.
        for (std::size_t ii = m_n; ii-- > 0;) {
            const double* row = m_a.data() + ii * stride;
            x[ii] /= row[m_bw];
            const std::size_t j0 = ii > m_bw ? ii - m_bw : 0u;
            for (std::size_t j = j0; j < ii; ++j) {
                x[j] -= row[j + m_bw - ii] * x[ii];
            }
        }
    }

private:
    std::size_t m_n = 0;
    std::size_t m_bw = 0;
    std::vector<double> m_a;  ///< The matrix (lower band), its factor once factored.
    bool m_factored = false;
};

// ===========================================================================
//  The terms
// ===========================================================================

/// One entry of a sparse term row: a free vertex and its coefficient.
struct Entry {
    std::uint32_t idx = 0;
    double coef = 0.0;
};

/// A flow match: where it is, what it measured, how much it counts.
struct Match {
    Stencil st;               ///< Bilinear stencil at the match's band position.
    double mLon = 0.0;        ///< Measured half disparity, band pixels (east +).
    double mLat = 0.0;        ///< Measured half disparity, band pixels (north +).
    double baseWeight = 0.0;  ///< alignWeight x stride^2 x structure x s x share (no robust, no benefit).
    double weight = 0.0;      ///< Current weight (all factors).
    std::uint32_t cell = 0;   ///< Mesh cell index (row * W + column) for the benefit gate.
    std::uint32_t pix = 0;    ///< Band pixel index (row * w + column) it was measured at.
};

/// A shape term on one component: up to three vertices of V - V0.
struct ShapeTerm {
    std::array<Entry, 3> e{};
    std::uint8_t n = 0;
    std::uint8_t comp = 0;
    double weight = 0.0;
    double target = 0.0;  ///< g . V0: the term is (g . V - target)^2.
};

/// A collinearity residual of one sample triple of a line: n . D2 V.
struct LineTriple {
    std::array<Entry, 12> e{};  ///< Merged stencil entries of the three samples (coef 1, -2, 1).
    std::uint8_t n = 0;
    double nLon = 0.0;           ///< The line's unit normal.
    double nLat = 0.0;
    double weight = 0.0;         ///< The line weight: the term is weight (n . D2 V)^2.
};

/// Add `coef` x stencil to a merged entry list (same vertex -> summed).
void mergeStencil(LineTriple& t, const Stencil& s, double coef) noexcept {
    for (std::uint8_t k = 0; k < s.n; ++k) {
        bool merged = false;
        for (std::uint8_t m = 0; m < t.n; ++m) {
            if (t.e[m].idx == s.idx[k]) {
                t.e[m].coef += coef * s.w[k];
                merged = true;
                break;
            }
        }
        if (!merged && t.n < t.e.size()) {
            t.e[t.n].idx = s.idx[k];
            t.e[t.n].coef = coef * s.w[k];
            ++t.n;
        }
    }
}

/// Add w g g^T for one component's entries to a block.  Entries must be
/// distinct vertices (the builders merge duplicates).  Returns false when a
/// pair fell outside the band (never expected; the caller refuses then).
bool addOuter(BandedSpd& A, const Entry* e, std::size_t n, double w) noexcept {
    bool ok = true;
    for (std::size_t a = 0; a < n; ++a) {
        for (std::size_t b = 0; b <= a; ++b) {
            ok = A.add(e[a].idx, e[b].idx, w * e[a].coef * e[b].coef) && ok;
        }
    }
    return ok;
}

/// Residual g . x of a term over one component of a block-major vector.
template <typename Term>
[[nodiscard]] double termDot(const Term& t, const std::vector<double>& x, std::size_t base) noexcept {
    double v = 0.0;
    for (std::uint8_t k = 0; k < t.n; ++k) {
        v += t.e[k].coef * x[base + t.e[k].idx];
    }
    return v;
}

/// The structured gate's numbers for one band, counted exactly as
/// gridFromFlow counts them.
struct GateCounts {
    std::uint64_t covisible = 0;
    std::uint64_t consistent = 0;
    std::uint64_t structured = 0;
    std::uint64_t consistentStructured = 0;
};

/// The shape terms of one component (see MeshWarp.h, E_shape): membrane and
/// bending along both lattice directions, ring-wrapped in longitude, boosted
/// where any vertex lies in a decay ring.  Entries on pinned rows are left
/// out (value 0, prior 0).
void buildShapeTerms(const MeshGeom& g, const MeshWarpParams& p, int comp, const std::vector<double>& x0,
                     std::vector<ShapeTerm>& out) {
    const double scale = comp == 0 ? p.crossMeridianShapeScale : 1.0;
    // Densities per band pixel turned into per-term weights: a first
    // difference over a spacing h stands for |grad|^2 over the cell area,
    // a second difference for |curvature|^2.
    const double memLon = p.shapeMembrane * scale * g.cellArea / (g.hLonPx * g.hLonPx);
    const double memLat = p.shapeMembrane * scale * g.cellArea / (g.hLatPx * g.hLatPx);
    const double bendLon = p.shapeBending * scale * g.cellArea / std::pow(g.hLonPx, 4.0);
    const double bendLat = p.shapeBending * scale * g.cellArea / std::pow(g.hLatPx, 4.0);
    const std::size_t base = static_cast<std::size_t>(comp) * g.N;

    // Push one term over the listed (column, row, coefficient) vertices.
    const auto push = [&](std::initializer_list<std::tuple<std::uint32_t, std::uint32_t, double>> verts,
                          double weight) {
        ShapeTerm t;
        t.comp = static_cast<std::uint8_t>(comp);
        bool ring = false;
        for (const auto& [i, j, c] : verts) {
            ring = ring || g.inRing(j);
            if (g.pinned(j)) {
                continue;  // fixed at zero, prior zero
            }
            const std::uint32_t v = g.index(i, j);
            t.e[t.n].idx = v;
            t.e[t.n].coef = c;
            t.target += c * x0[base + v];
            ++t.n;
        }
        if (t.n == 0 || !(weight > 0.0)) {
            return;  // every vertex pinned, or no weight: nothing to solve
        }
        t.weight = weight * (ring ? p.decayShapeBoost : 1.0);
        out.push_back(t);
    };

    const std::uint32_t W = g.W;
    for (std::uint32_t j = 0; j < g.H; ++j) {
        for (std::uint32_t i = 0; i < W; ++i) {
            const std::uint32_t ip = (i + 1u) % W;
            const std::uint32_t im = (i + W - 1u) % W;
            // Along longitude (rows of the lattice), ring-wrapped.
            if (!g.pinned(j)) {
                push({{i, j, 1.0}, {ip, j, -1.0}}, memLon);
                push({{im, j, 1.0}, {i, j, -2.0}, {ip, j, 1.0}}, bendLon);
            }
            // Along latitude (columns of the lattice), ending at the pins.
            if (j + 1u < g.H) {
                push({{i, j, 1.0}, {i, j + 1u, -1.0}}, memLat);
            }
            if (j >= 1u && j + 1u < g.H) {
                push({{i, j - 1u, 1.0}, {i, j, -2.0}, {i, j + 1u, 1.0}}, bendLat);
            }
        }
    }
}

/// Bilinear sample of a band plane at pixel-centre coordinates (x, y) =
/// (column + 0.5, row + 0.5): longitude wraps, latitude clamps (as the
/// grid's benefit gate samples, ParallaxWarp.cpp sampleBand).
[[nodiscard]] double sampleBandPlane(const std::vector<float>& plane, std::uint32_t w, std::uint32_t h, double x,
                                     double y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return 0.0;
    }
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    const double flx = std::floor(fx);
    const double fly = std::floor(fy);
    const double tx = fx - flx;
    const double ty = fy - fly;
    const long long W = static_cast<long long>(w);
    const auto x0 = static_cast<std::size_t>(((static_cast<long long>(flx) % W) + W) % W);
    const std::size_t x1 = (x0 + 1u) % w;
    const auto y0 = static_cast<std::size_t>(std::clamp(fly, 0.0, static_cast<double>(h - 1u)));
    const auto y1 = static_cast<std::size_t>(std::clamp(fly + 1.0, 0.0, static_cast<double>(h - 1u)));
    const auto at = [&](std::size_t xx, std::size_t yy) {
        const double v = static_cast<double>(plane[yy * w + xx]);
        return std::isfinite(v) ? v : 0.0;
    };
    const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const double bot = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return top + (bot - top) * ty;
}

}  // namespace

// ===========================================================================
//  Parameters, layout, the table lift
// ===========================================================================

Status checkMeshWarpParams(const MeshWarpParams& p) {
    // The measurement side is the grid's own block: same checks, same rules.
    const ParallaxWarpParams& q = p.parallax;
    if (q.band.equirectW < 64 || q.band.equirectW > 16384 || !(q.band.bandHalfDeg > 0.0) ||
        q.band.bandHalfDeg > 45.0) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: the analysis band is out of range");
    }
    if (!(q.minStructureGradient >= 0.0) || !std::isfinite(q.minStructureGradient)) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: minStructureGradient must be >= 0");
    }
    if (!(q.minStructuredConsistent >= 0.0) || q.minStructuredConsistent > 1.0 ||
        !(q.fullStructuredConsistent >= q.minStructuredConsistent) || q.fullStructuredConsistent > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: the structured gate's shares are malformed");
    }
    if (!(q.requiredImprovement >= 0.0) || q.requiredImprovement >= 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: requiredImprovement must be within [0, 1)");
    }
    if (!(q.minResidual >= 0.0) || !std::isfinite(q.minResidual)) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: minResidual must be >= 0");
    }
    if (!(q.maxCorrectionDeg > 0.0) || q.maxCorrectionDeg > 45.0) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: maxCorrectionDeg out of range");
    }
    // ---- the lattice ------------------------------------------------------------
    if (p.meshCols < 16 || p.meshCols > 1024) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: meshCols must be within 16..1024");
    }
    if (!(p.reachDeg > 0.0) || p.reachDeg > 45.0 || !(p.rowSpacingDeg > 0.0) || p.rowSpacingDeg > p.reachDeg) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: reachDeg / rowSpacingDeg out of range");
    }
    const double spans = 2.0 * p.reachDeg / p.rowSpacingDeg;
    if (std::fabs(spans - std::round(spans)) > 1e-6 || spans < 4.0 || spans > 254.0) {
        return failStatus(ErrorCode::InvalidArgument,
                          "mesh warp: 2 x reachDeg must be a whole number (4..254) of rowSpacingDeg");
    }
    // The analysed band must sit inside the free rows, with at least one ring
    // row beyond it each side for the field to fall to zero over.
    if (q.band.bandHalfDeg > p.reachDeg - p.rowSpacingDeg + 1e-9) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: the analysed band reaches the mesh's pinned edge");
    }
    // ---- the weights ------------------------------------------------------------
    const auto finiteNonNeg = [](double v) { return std::isfinite(v) && v >= 0.0; };
    if (!finiteNonNeg(p.alignWeight) || !finiteNonNeg(p.lineWeight) || !finiteNonNeg(p.shapeMembrane) ||
        !finiteNonNeg(p.shapeBending) || !finiteNonNeg(p.temporalWeight) || !finiteNonNeg(p.temporalFloor)) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: a weight is negative or not finite");
    }
    // The anchor keeps every block positive definite: it must be > 0.
    if (!(p.anchorWeight > 0.0) || !std::isfinite(p.anchorWeight) || !(p.anchorFloor > 0.0) ||
        p.anchorFloor > 1.0 || !(p.anchorDataFull > 0.0) || !std::isfinite(p.anchorDataFull) ||
        !(p.anchorCovisibleScale > 0.0) || p.anchorCovisibleScale > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: the anchor must be positive (it keeps the system "
                                                      "positive definite)");
    }
    if (!(p.crossMeridianShapeScale > 0.0) || !std::isfinite(p.crossMeridianShapeScale) ||
        !(p.decayShapeBoost > 0.0) || !std::isfinite(p.decayShapeBoost)) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: shape scales must be positive");
    }
    if (!(p.structureFloor >= 0.0) || p.structureFloor > 1.0 || !(p.unstructuredWeight >= 0.0) ||
        p.unstructuredWeight > 1.0) {
        return failStatus(ErrorCode::InvalidArgument,
                          "mesh warp: structureFloor and unstructuredWeight must be within [0, 1]");
    }
    if (p.matchStride < 1 || p.matchStride > 16 || !(p.structureFullFactor > 1.0) ||
        !std::isfinite(p.structureFullFactor) || !(p.robustScalePx >= 0.0) || !std::isfinite(p.robustScalePx)) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: match sampling / robust scale out of range");
    }
    if (!(p.lineSampleSpacingPx >= 1.0) || !std::isfinite(p.lineSampleSpacingPx) ||
        !(p.temporalScalePx > 0.0) || !std::isfinite(p.temporalScalePx)) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: line spacing / temporal threshold out of range");
    }
    if (!(p.sharedRefinedShare >= 0.0) || p.sharedRefinedShare > 1.0) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: sharedRefinedShare must be within [0, 1]");
    }
    if (p.irlsIterations < 1 || p.irlsIterations > 16) {
        return failStatus(ErrorCode::InvalidArgument, "mesh warp: irlsIterations must be within 1..16");
    }
    return okStatus();
}

Result<ParallaxWarpGrid> meshWarpLayout(const MeshWarpParams& params) {
    OSV_TRY(checkMeshWarpParams(params));
    const MeshGeom g = makeGeom(params);
    ParallaxWarpGrid grid;
    grid.w = g.W;
    grid.h = g.H;
    // Row 0 is the northern edge, row h - 1 the southern one: the same
    // descending convention as the grids gridFromFlow builds.
    grid.latMinRad = static_cast<float>(g.reachRad);
    grid.latMaxRad = static_cast<float>(-g.reachRad);
    grid.uv.assign(static_cast<std::size_t>(g.W) * g.H * 2u, 0.0f);
    grid.strength = 0.0;
    return grid;
}

Result<ParallaxWarpGrid> liftSeamTable(const std::vector<float>& tableDeg, const MeshWarpParams& params) {
    OSV_TRY_ASSIGN(ParallaxWarpGrid grid, meshWarpLayout(params));
    if (tableDeg.empty()) {
        return grid;  // no table: the zero field
    }
    const MeshGeom g = makeGeom(params);
    const std::size_t n = tableDeg.size();
    // Table column k is read by the kernel for longitudes in
    // [-pi + k 2pi/n, -pi + (k+1) 2pi/n) (osvSeamColumn), i.e. it stands for
    // its centre (k + 0.5) / n of the ring.  Each vertex takes the hat-
    // weighted mean of the columns within one mesh column of it.
    const double colsPerVertex = static_cast<double>(n) / static_cast<double>(g.W);
    // The kernel's taper at each row's latitude (osvSeamShiftTaper on the
    // sine of the latitude, in full to 6 degrees, smoothstep to 0 at 12).
    std::vector<double> taper(g.H, 0.0);
    for (std::uint32_t j = 0; j < g.H; ++j) {
        if (g.pinned(j)) {
            continue;  // the edge stays exactly zero
        }
        taper[j] = static_cast<double>(osvSeamShiftTaper(static_cast<float>(std::sin(g.latOfRow(j)))));
    }
    const double maxRad = deg2rad(params.parallax.maxCorrectionDeg);
    for (std::uint32_t i = 0; i < g.W; ++i) {
        // Vertex i sits at ring fraction i / W, table position i * n / W - 0.5
        // in column-index units (column k's centre is at k + 0.5 of n).
        const double centre = static_cast<double>(i) * colsPerVertex - 0.5;
        const auto k0 = static_cast<long long>(std::floor(centre - colsPerVertex));
        const auto k1 = static_cast<long long>(std::ceil(centre + colsPerVertex));
        double sum = 0.0;
        double wsum = 0.0;
        for (long long k = k0; k <= k1; ++k) {
            const double hat = 1.0 - std::fabs(static_cast<double>(k) - centre) / colsPerVertex;
            if (!(hat > 0.0)) {
                continue;
            }
            const auto kk = static_cast<std::size_t>(((k % static_cast<long long>(n)) + static_cast<long long>(n)) %
                                                     static_cast<long long>(n));
            const double v = static_cast<double>(tableDeg[kk]);
            sum += hat * (std::isfinite(v) ? v : 0.0);  // a broken column shifts nothing
            wsum += hat;
        }
        const double tDeg = wsum > 0.0 ? sum / wsum : 0.0;
        // T / 2 away from the master's axis (the +90 pole): lower latitude.
        const double dLatRad = -0.5 * deg2rad(tDeg);
        for (std::uint32_t j = 1; j + 1u < g.H; ++j) {
            const double v = std::clamp(dLatRad * taper[j], -maxRad, maxRad);
            grid.uv[(static_cast<std::size_t>(j) * g.W + i) * 2u + 1u] = static_cast<float>(v);
        }
    }
    return grid;
}

WarpGridView warpGridView(const ParallaxWarpGrid& grid) noexcept {
    WarpGridView v;
    if (!grid.valid()) {
        return v;  // uv null: not valid(), renders as no warp
    }
    v.uv = grid.uv.data();
    v.w = grid.w;
    v.h = grid.h;
    v.latMinRad = grid.latMinRad;
    v.latMaxRad = grid.latMaxRad;
    return v;
}

Result<double> meanAbsGridChangeDeg(const ParallaxWarpGrid& a, const ParallaxWarpGrid& b, double maxAbsLatDeg) {
    if (!a.valid() || !b.valid() || a.w != b.w || a.h != b.h || a.latMinRad != b.latMinRad ||
        a.latMaxRad != b.latMaxRad) {
        return Error{ErrorCode::InvalidArgument, "meanAbsGridChangeDeg: the grids differ in layout or are malformed"};
    }
    if (!(maxAbsLatDeg > 0.0) || !std::isfinite(maxAbsLatDeg) || a.h < 2) {
        return Error{ErrorCode::InvalidArgument, "meanAbsGridChangeDeg: bad latitude bound or a one-row grid"};
    }
    const double latMin = static_cast<double>(a.latMinRad);
    const double span = static_cast<double>(a.latMaxRad) - latMin;
    const double bound = deg2rad(maxAbsLatDeg);
    double sum = 0.0;
    std::size_t count = 0;
    for (std::uint32_t j = 0; j < a.h; ++j) {
        const double lat = latMin + span * static_cast<double>(j) / static_cast<double>(a.h - 1u);
        if (std::fabs(lat) > bound + 1e-12) {
            continue;
        }
        for (std::uint32_t i = 0; i < a.w; ++i) {
            const std::size_t k = (static_cast<std::size_t>(j) * a.w + i) * 2u;
            const double du = static_cast<double>(a.uv[k]) - static_cast<double>(b.uv[k]);
            const double dv = static_cast<double>(a.uv[k + 1u]) - static_cast<double>(b.uv[k + 1u]);
            const double m = std::hypot(du, dv);
            if (std::isfinite(m)) {
                sum += m;
                ++count;
            }
        }
    }
    return count ? rad2deg(sum / static_cast<double>(count)) : 0.0;
}

std::string MeshWarpReport::summary() const {
    return std::format("{} s {:.2f} (structured {:.1f}%), {} matches ({} raw / {} refined, {} shared), {} lines ({} "
                       "triples), line residual {:.3f} -> {:.3f} px, energy {:.4g} -> {:.4g}, {} solves{}, "
                       "{:.1f} ms",
                       mode == MeshWarpMode::Solved ? "solved" : "prior only", strength,
                       100.0 * structuredFraction, matches, matchesPrimary, matchesRefined, matchPairs, lines,
                       lineTriples, lineResidualBeforePx, lineResidualAfterPx, before.total(), after.total(),
                       irlsIterations,
                       temporal ? std::format(", temporal |dV| {:.4f} deg", meanAbsChangeFromPreviousDeg)
                                : std::string(),
                       totalMs);
}

// ===========================================================================
//  The solve
// ===========================================================================

Result<MeshWarpResult> solveMeshWarp(const MeshWarpInputs& in, const MeshWarpParams& params, ThreadPool* pool) {
    const auto tStart = Clock::now();
    OSV_TRY(checkMeshWarpParams(params));
    OSV_TRY_ASSIGN(ParallaxWarpGrid layout, meshWarpLayout(params));
    const MeshGeom g = makeGeom(params);
    const ParallaxWarpParams& pp = params.parallax;

    // ---- the inputs, checked before anything is read --------------------------------
    // A band set must be of the analysis band's polar map, inside its own map,
    // with all four planes of its size; a flow must match its bands; every
    // band set of one solve shares one geometry (pixel p is one place on the
    // sphere in all of them).
    const auto checkBands = [&](const LensBands* b, const char* what) -> Status {
        if (b == nullptr) {
            return okStatus();
        }
        const std::size_t n = static_cast<std::size_t>(b->w) * b->h;
        if (b->w == 0 || b->h == 0 || b->mapH == 0 || b->w != pp.band.equirectW ||
            b->mapH != pp.band.equirectW / 2u) {
            return failStatus(ErrorCode::InvalidArgument,
                              std::format("solveMeshWarp: the {} bands are empty or not of the analysis band's map",
                                          what));
        }
        if (static_cast<std::uint64_t>(b->rowOffset) + b->h > b->mapH) {
            return failStatus(ErrorCode::InvalidArgument,
                              std::format("solveMeshWarp: the {} band lies outside its own map", what));
        }
        for (int lens = 0; lens < 2; ++lens) {
            if (b->luma[lens].size() != n || b->alpha[lens].size() != n) {
                return failStatus(ErrorCode::InvalidArgument,
                                  std::format("solveMeshWarp: a {} band plane is the wrong size", what));
            }
        }
        if (in.bands != nullptr && b != in.bands &&
            (b->w != in.bands->w || b->h != in.bands->h || b->rowOffset != in.bands->rowOffset)) {
            return failStatus(ErrorCode::InvalidArgument,
                              std::format("solveMeshWarp: the {} bands are not of the primary bands' geometry", what));
        }
        return okStatus();
    };
    const auto checkFlow = [&](const BidirFlow* f, const LensBands* b, const char* what) -> Status {
        if (f == nullptr) {
            return okStatus();
        }
        if (b == nullptr) {
            return failStatus(ErrorCode::InvalidArgument,
                              std::format("solveMeshWarp: a {} flow without the bands it was measured on", what));
        }
        if (!f->valid() || f->forward.w != b->w || f->forward.h != b->h || f->backward.w != b->w ||
            f->backward.h != b->h) {
            return failStatus(ErrorCode::InvalidArgument,
                              std::format("solveMeshWarp: the {} flow does not match its bands", what));
        }
        return okStatus();
    };
    OSV_TRY(checkBands(in.bands, "primary"));
    OSV_TRY(checkFlow(in.flow, in.bands, "primary"));
    OSV_TRY(checkBands(in.bands2, "refined"));
    OSV_TRY(checkFlow(in.flow2, in.bands2, "refined"));
    OSV_TRY(checkBands(in.verifyBands, "verification"));
    if ((in.bandWarp != nullptr && !in.bandWarp->valid()) || (in.bandWarp2 != nullptr && !in.bandWarp2->valid())) {
        return Error{ErrorCode::InvalidArgument, "solveMeshWarp: a band warp grid is malformed"};
    }
    if (in.flow2 != nullptr && in.flow == nullptr) {
        return Error{ErrorCode::InvalidArgument, "solveMeshWarp: a refined measurement without a primary one"};
    }
    if (in.prior != nullptr && !hasMeshLayout(*in.prior, layout)) {
        return Error{ErrorCode::InvalidArgument, "solveMeshWarp: the prior is not of the mesh's layout"};
    }
    if (in.previous != nullptr && !hasMeshLayout(*in.previous, layout)) {
        return Error{ErrorCode::InvalidArgument, "solveMeshWarp: the previous mesh is not of the mesh's layout"};
    }
    const LensBands* bands = in.bands;
    // The bands every photometric judgement is made on (the benefit gate, the
    // co-visibility), and the field they were rendered with: the raw bands
    // when given, else the primary ones as rendered.
    const LensBands* verify = in.verifyBands != nullptr ? in.verifyBands : in.bands;
    const ParallaxWarpGrid* verifyWarp = in.verifyBands != nullptr ? nullptr : in.bandWarp;

    MeshWarpResult result;
    MeshWarpReport& rep = result.report;
    const std::size_t N = g.N;
    // The prior (V0) and the previous mesh as vectors of free vertices.  A
    // prior's edge rows are never read: the edge is pinned to zero.
    const std::vector<double> x0 = in.prior != nullptr ? gridToVector(g, *in.prior) : std::vector<double>(2u * N, 0.0);
    const std::vector<double> xPrev =
        in.previous != nullptr ? gridToVector(g, *in.previous) : std::vector<double>();
    const double maxPx = deg2rad(pp.maxCorrectionDeg) / g.radPerPxLon;
    // Mesh cells (W x (H - 1)): the benefit gate's and the untrusted share's unit.
    const std::size_t cellCount = static_cast<std::size_t>(g.W) * (g.H - 1u);

    // =========================================================================
    //  2. One pass over each measurement: the gate's counts and the matches
    // =========================================================================
    // Per band row (a task): the counts and the row's matches, joined in row
    // order afterwards so the result is the same on any number of threads.
    const auto tMatch = Clock::now();
    GateCounts counts;
    const double thr = pp.minStructureGradient;
    const double thrSq = thr * thr;
    const double structSpan = thr * (params.structureFullFactor - 1.0);
    const std::uint32_t stride = params.matchStride;
    const auto extract = [&](const LensBands& b, const BidirFlow* flow, const ParallaxWarpGrid* warp,
                             GateCounts* countsOut, std::vector<Match>& outMatches) {
        const std::uint32_t bw = b.w;
        const std::uint32_t bh = b.h;
        const BandStencils bs = bandStencils(g, b);
        std::vector<GateCounts> rowCounts(bh);
        std::vector<std::vector<Match>> rowMatches(bh);
        const auto rowTask = [&](std::size_t rIndex) {
            const auto r = static_cast<std::uint32_t>(rIndex);
            GateCounts c;
            std::vector<Match>& out = rowMatches[rIndex];
            out.clear();
            const bool matchRow = (r % stride) == 0u;
            for (std::uint32_t col = 0; col < bw; ++col) {
                const std::size_t i = static_cast<std::size_t>(r) * bw + col;
                if (!(b.alpha[0][i] > 0.5f) || !(b.alpha[1][i] > 0.5f)) {
                    continue;  // one lens alone says nothing about parallax
                }
                const bool matchPixel = matchRow && (col % stride) == 0u;
                // Without counts to keep (the refined measurement) only the
                // match pixels need their structure.
                if (countsOut == nullptr && !matchPixel) {
                    continue;
                }
                ++c.covisible;
                // The structured test, exactly gridFromFlow's (both lenses).
                const double g0 = bandLumaGradientSq(b.luma[0], b.alpha[0], bw, bh, r, col);
                const double g1 = bandLumaGradientSq(b.luma[1], b.alpha[1], bw, bh, r, col);
                const bool structured = g0 >= thrSq && g1 >= thrSq;
                c.structured += structured ? 1u : 0u;
                if (flow == nullptr || flow->ok[i] == 0u) {
                    continue;
                }
                const double fu =
                    0.5 * (static_cast<double>(flow->forward.u[i]) - static_cast<double>(flow->backward.u[i]));
                const double fv =
                    0.5 * (static_cast<double>(flow->forward.v[i]) - static_cast<double>(flow->backward.v[i]));
                if (!std::isfinite(fu) || !std::isfinite(fv)) {
                    continue;
                }
                ++c.consistent;
                c.consistentStructured += structured ? 1u : 0u;
                if (!matchPixel || (!structured && !(params.unstructuredWeight > 0.0))) {
                    continue;
                }
                // ---- a match ----
                Match m;
                m.st = makeStencil(g, bs.cols[col], bs.rows[r]);
                if (!m.st.inside || m.st.n == 0) {
                    continue;
                }
                // The structure weight: the smaller lens gradient, from the
                // gate's threshold up to structureFullFactor times it.  An
                // unstructured consistent pixel counts at unstructuredWeight:
                // its flow is the solver's interpolation from around it.
                double sw = params.unstructuredWeight;
                if (structured) {
                    const double gMin = std::sqrt(std::min(g0, g1));
                    const double ramp = structSpan > 0.0 ? smoothstep01((gMin - thr) / structSpan) : 1.0;
                    sw = params.structureFloor + (1.0 - params.structureFloor) * ramp;
                }
                m.baseWeight = params.alignWeight * static_cast<double>(stride) * static_cast<double>(stride) * sw;
                if (!(m.baseWeight > 0.0)) {
                    continue;  // no weight: nothing to add
                }
                // Half disparity in band pixels (a column step is east, a row
                // step is SOUTH), plus the field the bands were rendered with:
                // the flow measured only what that field left.
                double mLon = 0.5 * fu;
                double mLat = -0.5 * fv;
                if (warp != nullptr) {
                    mLon += sampleGrid(*warp, bs.lon[col], bs.lat[r], 0) / g.radPerPxLon;
                    mLat += sampleGrid(*warp, bs.lon[col], bs.lat[r], 1) / g.radPerPxLat;
                }
                m.mLon = std::clamp(mLon, -maxPx, maxPx);
                m.mLat = std::clamp(mLat, -maxPx, maxPx);
                m.cell = m.st.cellRow * g.W + m.st.cellCol;
                m.pix = static_cast<std::uint32_t>(i);
                out.push_back(m);
            }
            rowCounts[rIndex] = c;
        };
        forTasks(pool, bh, rowTask);
        for (std::uint32_t r = 0; r < bh; ++r) {
            if (countsOut != nullptr) {
                countsOut->covisible += rowCounts[r].covisible;
                countsOut->consistent += rowCounts[r].consistent;
                countsOut->structured += rowCounts[r].structured;
                countsOut->consistentStructured += rowCounts[r].consistentStructured;
            }
            outMatches.insert(outMatches.end(), rowMatches[r].begin(), rowMatches[r].end());
        }
    };
    // The primary measurement carries the structured gate's counts (the
    // numbers 0.5.1 logged for its grid on the same bands).
    std::vector<Match> matches;
    std::vector<Match> refined;
    if (bands != nullptr) {
        extract(*bands, in.flow, in.bandWarp, &counts, matches);
    }
    if (in.bands2 != nullptr && in.flow2 != nullptr) {
        extract(*in.bands2, in.flow2, in.bandWarp2, nullptr, refined);
    }
    rep.matchesPrimary = static_cast<std::uint32_t>(matches.size());
    rep.matchesRefined = static_cast<std::uint32_t>(refined.size());
    // Where both measurements matched one pixel, each is a hypothesis for the
    // disparity there and counts half: the pixel's evidence stays one
    // match's worth.  (A photometric pick between the two - patch residual on
    // the raw bands - was measured and changed no score by more than 0.001:
    // where the two differ by more than noise, only one of them is consistent
    // at all, so they rarely meet.)  Both lists are in pixel order.
    std::vector<double> share(matches.size(), 1.0);
    std::vector<double> shareRefined(refined.size(), 1.0);
    for (std::size_t a = 0, b = 0; a < matches.size() && b < refined.size();) {
        if (matches[a].pix == refined[b].pix) {
            share[a] = 1.0 - params.sharedRefinedShare;
            shareRefined[b] = params.sharedRefinedShare;
            ++rep.matchPairs;
            ++a;
            ++b;
        } else if (matches[a].pix < refined[b].pix) {
            ++a;
        } else {
            ++b;
        }
    }
    for (std::size_t a = 0; a < matches.size(); ++a) {
        matches[a].baseWeight *= share[a];
    }
    for (std::size_t b = 0; b < refined.size(); ++b) {
        refined[b].baseWeight *= shareRefined[b];
    }
    matches.insert(matches.end(), refined.begin(), refined.end());

    // The structured gate as the data term's WEIGHT: below its floor the data
    // count for nothing, between its shares they ramp in - one field either
    // way, never a switch to another one.
    const double structuredShare =
        counts.structured ? static_cast<double>(counts.consistentStructured) / static_cast<double>(counts.structured)
                          : 0.0;
    const double s = counts.structured < pp.minStructuredPixels ? 0.0 : parallaxGateStrength(structuredShare, pp);
    rep.strength = s;
    rep.structuredFraction = structuredShare;
    for (Match& m : matches) {
        m.baseWeight *= s;
        m.weight = m.baseWeight;
    }
    if (s <= 0.0) {
        matches.clear();  // weightless: nothing to assemble
    }

    // =========================================================================
    //  3. Co-visibility per vertex: how far the anchor lets data reach
    // =========================================================================
    // Where BOTH lenses see a direction but nothing was measured (an
    // occluding stick, a featureless strip between two measured ones), the
    // two lenses still blend there and the best guess for the correction is
    // the smooth continuation of its neighbours' - so the anchor toward the
    // prior is weak.  Where only ONE lens sees it (the zero-overlap arc of a
    // car mount), any correction moves that lens's picture with nothing to
    // align it to - so the anchor holds the prior.  Counted per mesh cell on
    // the verification bands, averaged over a vertex's (up to four) cells.
    std::vector<double> covisibleShare(N, 0.0);
    BandStencils verifyStencils;
    if (verify != nullptr) {
        verifyStencils = bandStencils(g, *verify);
        std::vector<double> cellCov(cellCount, 0.0);
        std::vector<double> cellAll(cellCount, 0.0);
        for (std::uint32_t r = 0; r < verify->h; ++r) {
            const StencilRow& sr = verifyStencils.rows[r];
            if (!sr.inside) {
                continue;
            }
            const std::size_t rowBase = static_cast<std::size_t>(sr.y0) * g.W;
            for (std::uint32_t c = 0; c < verify->w; ++c) {
                const std::size_t i = static_cast<std::size_t>(r) * verify->w + c;
                const std::size_t k = rowBase + verifyStencils.cols[c].x0;
                cellAll[k] += 1.0;
                cellCov[k] += (verify->alpha[0][i] > 0.5f && verify->alpha[1][i] > 0.5f) ? 1.0 : 0.0;
            }
        }
        for (std::uint32_t j = 1; j + 1u < g.H; ++j) {
            for (std::uint32_t i = 0; i < g.W; ++i) {
                const std::uint32_t im = (i + g.W - 1u) % g.W;
                double cov = 0.0;
                double all = 0.0;
                for (const std::uint32_t cr : {j - 1u, j}) {
                    for (const std::uint32_t cc : {im, i}) {
                        const std::size_t k = static_cast<std::size_t>(cr) * g.W + cc;
                        cov += cellCov[k];
                        all += cellAll[k];
                    }
                }
                covisibleShare[g.index(i, j)] = all > 0.0 ? cov / all : 0.0;
            }
        }
    }
    rep.matchMs = msSince(tMatch);

    // =========================================================================
    //  4. The line triples
    // =========================================================================
    std::vector<LineTriple> lineTriples;
    if (in.lines != nullptr && params.lineWeight > 0.0) {
        // Sample spacing: at most half a mesh column, so three consecutive
        // samples never span more than two columns (the blocks' band width).
        const double spacing = std::min(params.lineSampleSpacingPx, 0.5 * g.hLonPx);
        for (const SeamLine& ln : *in.lines) {
            // The segment in band pixels (longitude east, latitude north).
            const double x0p = ln.lon0Rad / g.radPerPxLon;
            const double y0p = ln.lat0Rad / g.radPerPxLat;
            const double x1p = ln.lon1Rad / g.radPerPxLon;
            const double y1p = ln.lat1Rad / g.radPerPxLat;
            const double len = std::hypot(x1p - x0p, y1p - y0p);
            if (!std::isfinite(len) || len < 2.0 * spacing || (ln.lens != 0 && ln.lens != 1)) {
                continue;  // too short for one triple, or malformed
            }
            const double nLon = -(y1p - y0p) / len;
            const double nLat = (x1p - x0p) / len;
            const auto K = static_cast<std::uint32_t>(std::ceil(len / spacing)) + 1u;
            std::vector<Stencil> st(K);
            for (std::uint32_t k = 0; k < K; ++k) {
                const double t = static_cast<double>(k) / static_cast<double>(K - 1u);
                st[k] = stencilAt(g, ln.lon0Rad + t * (ln.lon1Rad - ln.lon0Rad),
                                  ln.lat0Rad + t * (ln.lat1Rad - ln.lat0Rad));
            }
            std::uint32_t used = 0;
            for (std::uint32_t k = 1; k + 1u < K; ++k) {
                const Stencil& a = st[k - 1u];
                const Stencil& b = st[k];
                const Stencil& c = st[k + 1u];
                if (!a.inside || !b.inside || !c.inside) {
                    ++rep.lineTriplesDropped;
                    continue;
                }
                // The three samples touch columns floor(fx) .. floor(fx) + 1;
                // over the triple that must stay within ring distance 2.
                const long long lo = std::min({a.col0, b.col0, c.col0});
                const long long hi = std::max({a.col0, b.col0, c.col0}) + 1;
                if (hi - lo > 2) {
                    ++rep.lineTriplesDropped;
                    continue;
                }
                LineTriple t;
                mergeStencil(t, a, 1.0);
                mergeStencil(t, b, -2.0);
                mergeStencil(t, c, 1.0);
                if (t.n == 0) {
                    continue;  // every vertex pinned
                }
                // (n . D2V)^2: the second difference across the line, which
                // mixes dLon and dLat wherever the line is oblique - kept
                // exact by the interleaved system (step 5).
                t.nLon = nLon;
                t.nLat = nLat;
                t.weight = params.lineWeight;
                lineTriples.push_back(t);
                ++used;
            }
            rep.lines += used > 0 ? 1u : 0u;
        }
    }
    rep.lineTriples = static_cast<std::uint32_t>(lineTriples.size());

    // ---- the grid out of a solution, and its diagnostics ----------------------------
    const auto finishGrid = [&](const std::vector<double>& x) {
        ParallaxWarpGrid out = layout;
        const double maxRad = deg2rad(pp.maxCorrectionDeg);
        for (std::uint32_t j = 1; j + 1u < g.H; ++j) {
            for (std::uint32_t i = 0; i < g.W; ++i) {
                const std::uint32_t v = g.index(i, j);
                const double lon = x[v] * g.radPerPxLon;
                const double lat = x[N + v] * g.radPerPxLat;
                const std::size_t k = (static_cast<std::size_t>(j) * g.W + i) * 2u;
                out.uv[k + 0u] = std::isfinite(lon) ? static_cast<float>(std::clamp(lon, -maxRad, maxRad)) : 0.0f;
                out.uv[k + 1u] = std::isfinite(lat) ? static_cast<float>(std::clamp(lat, -maxRad, maxRad)) : 0.0f;
            }
        }
        return out;
    };
    const auto fillDiagnostics = [&](ParallaxWarpGrid& out, const std::vector<double>& cellWeight,
                                     const std::vector<float>& cellGate) {
        out.strength = s;
        out.usedBackend = in.usedBackend;
        out.totalPixels = counts.covisible;
        out.consistentPixels = counts.consistent;
        out.structuredPixels = counts.structured;
        out.consistentStructuredPixels = counts.consistentStructured;
        out.bandMs = in.bandMs;
        out.flowMs = in.flowMs;
        // The band cells: those whose centre lies inside the analysed band.
        std::uint32_t measured = 0;
        std::uint32_t gated = 0;
        std::vector<std::uint32_t> untrusted(g.W, 0u);
        std::uint32_t bandCellRows = 0;
        for (std::uint32_t j = 0; j + 1u < g.H; ++j) {
            const double centre = 0.5 * (g.latOfRow(j) + g.latOfRow(j + 1u));
            if (std::fabs(centre) > g.bandHalfRad) {
                continue;
            }
            ++bandCellRows;
            for (std::uint32_t i = 0; i < g.W; ++i) {
                const std::size_t c = static_cast<std::size_t>(j) * g.W + i;
                const bool hasData = c < cellWeight.size() && cellWeight[c] > 0.0;
                const bool isGated = hasData && c < cellGate.size() && !(cellGate[c] > 0.0f);
                measured += hasData ? 1u : 0u;
                gated += isGated ? 1u : 0u;
                untrusted[i] += (!hasData || isGated) ? 1u : 0u;
            }
        }
        out.measuredCells = measured;
        out.gatedCells = gated;
        out.untrustedShare.assign(g.W, 0.0f);
        for (std::uint32_t i = 0; i < g.W && bandCellRows > 0; ++i) {
            out.untrustedShare[i] =
                static_cast<float>(static_cast<double>(untrusted[i]) / static_cast<double>(bandCellRows));
        }
        // The correction applied over the band rows, as FULL disparity.
        double sumAbs = 0.0;
        double maxAbs = 0.0;
        std::size_t n = 0;
        for (std::uint32_t j = 0; j < g.H; ++j) {
            if (std::fabs(g.latOfRow(j)) > g.bandHalfRad + 1e-9) {
                continue;
            }
            for (std::uint32_t i = 0; i < g.W; ++i) {
                const std::size_t k = (static_cast<std::size_t>(j) * g.W + i) * 2u;
                const double mag = std::hypot(static_cast<double>(out.uv[k]), static_cast<double>(out.uv[k + 1u]));
                sumAbs += mag;
                maxAbs = std::max(maxAbs, mag);
                ++n;
            }
        }
        out.meanAbsCorrectionDeg = n ? 2.0 * rad2deg(sumAbs / static_cast<double>(n)) : 0.0;
        out.maxAbsCorrectionDeg = 2.0 * rad2deg(maxAbs);
    };

    // ---- the nothing-to-solve case: the prior, exactly ------------------------------
    if (matches.empty() && lineTriples.empty() && in.previous == nullptr) {
        // Nothing measured, nothing to keep straight, nothing to follow: the
        // field IS the prior (a copy, bit for bit), or zero without one.
        result.grid = in.prior != nullptr ? *in.prior : layout;
        // Its pinned rows are zero by definition.
        for (std::uint32_t i = 0; i < g.W; ++i) {
            for (const std::uint32_t j : {0u, g.H - 1u}) {
                const std::size_t k = (static_cast<std::size_t>(j) * g.W + i) * 2u;
                result.grid.uv[k] = 0.0f;
                result.grid.uv[k + 1u] = 0.0f;
            }
        }
        fillDiagnostics(result.grid, {}, {});
        rep.mode = MeshWarpMode::PriorOnly;
        rep.totalMs = msSince(tStart);
        result.grid.gridMs = rep.totalMs;
        // No previous mesh took part, so the field on its own IS the field.
        if (in.solveAlone) {
            result.alone = result.grid;
        }
        return result;
    }

    // =========================================================================
    //  5. The shape terms, and the constant part of the system
    // =========================================================================
    // ONE banded system over both components, interleaved per vertex
    // (unknown 2 v + c for component c of block index v): the line term's
    // normal n mixes dLon and dLat at an oblique line, and interleaving keeps
    // that coupling inside the band (half width 2 bw + 1), so one banded
    // Cholesky solves the exact energy with no iteration.  (Two separate
    // blocks with the cross part left to conjugate gradients needed 20-130
    // iterations per solve: an oblique line is stiff along its normal and
    // free along its tangent, which no block preconditioner captures.)
    const auto tAssemble0 = Clock::now();
    std::vector<ShapeTerm> shape;
    shape.reserve(8u * static_cast<std::size_t>(g.W) * g.H);
    buildShapeTerms(g, params, 0, x0, shape);
    buildShapeTerms(g, params, 1, x0, shape);
    const std::size_t n2 = 2u * N;
    const std::size_t bw2 = 2u * g.bw + 1u;
    // Interleaved position of block-major entry (component c, vertex v).
    const auto il = [](std::uint32_t v, int c) { return 2u * static_cast<std::size_t>(v) + static_cast<std::size_t>(c); };
    // Add w g g^T for entries of ONE component (shape, data, anchor, time).
    const auto addOuterComp = [&](BandedSpd& A, const Entry* e, std::size_t n, double w, int c) {
        bool ok = true;
        for (std::size_t a = 0; a < n; ++a) {
            for (std::size_t b2 = 0; b2 <= a; ++b2) {
                ok = A.add(il(e[a].idx, c), il(e[b2].idx, c), w * e[a].coef * e[b2].coef) && ok;
            }
        }
        return ok;
    };
    // Shape and lines do not change over the IRLS iterations: assembled once.
    BandedSpd baseA;  // never factored: copied into each iteration's system
    baseA.reset(n2, bw2);
    std::vector<double> baseB(n2, 0.0);  // interleaved
    std::uint64_t outsideBand = 0;
    for (const ShapeTerm& t : shape) {
        outsideBand += addOuterComp(baseA, t.e.data(), t.n, t.weight, t.comp) ? 0u : 1u;
        for (std::uint8_t k = 0; k < t.n; ++k) {
            baseB[il(t.e[k].idx, t.comp)] += t.weight * t.e[k].coef * t.target;
        }
    }
    // The exact line term: the row g = [nLon a ; nLat a] over both components.
    for (const LineTriple& t : lineTriples) {
        std::array<Entry, 24> e{};
        std::size_t n = 0;
        for (std::uint8_t k = 0; k < t.n; ++k) {
            if (t.nLon != 0.0) {
                e[n].idx = static_cast<std::uint32_t>(il(t.e[k].idx, 0));
                e[n].coef = t.nLon * t.e[k].coef;
                ++n;
            }
            if (t.nLat != 0.0) {
                e[n].idx = static_cast<std::uint32_t>(il(t.e[k].idx, 1));
                e[n].coef = t.nLat * t.e[k].coef;
                ++n;
            }
        }
        outsideBand += addOuter(baseA, e.data(), n, t.weight) ? 0u : 1u;
    }
    if (outsideBand != 0) {
        // The builders keep every term inside the band; reaching here means a
        // bug, and a silently truncated matrix would solve a different problem.
        return Error{ErrorCode::Internal, std::format("solveMeshWarp: {} terms fell outside the solver's band",
                                                      outsideBand)};
    }
    rep.assembleMs += msSince(tAssemble0);

    // =========================================================================
    //  6. IRLS: assemble, factor, solve, reweight
    // =========================================================================
    // Every iteration is solved exactly by its own factorisation; the
    // iterations only refine the weights.
    std::vector<double> x = !xPrev.empty() ? xPrev : x0;  // block-major, as everything else
    std::vector<double> cellGateWeight(cellCount, 1.0);    // the benefit gate's verdict per cell
    std::vector<float> cellGate(cellCount, 1.0f);
    bool gateRan = false;
    std::vector<double> temporalW(N, 0.0);
    std::vector<double> anchorW(N, 0.0);
    std::vector<double> dataDiag(N, 0.0);
    std::vector<double> dataSum(N, 0.0);
    std::vector<double> xFree;  // the first solve, made without the temporal term
    const bool benefitOn = pp.requiredImprovement > 0.0 && verify != nullptr && !matches.empty();
    BandedSpd system;  // this iteration's matrix, factored in place

    // ---- one exact solve at the current weights ---------------------------------------
    // Assembles the system from the constant part (shape, lines), the matches
    // at their current weights and the per-vertex anchor - plus, with
    // `withTemporal`, the temporal pull toward the previous mesh - then
    // factors it and solves into `xOut` (block-major).  Without the temporal
    // term NOTHING of it is added (not even zeros), so a solve without a
    // previous mesh and the "alone" solve below assemble the same numbers in
    // the same order.
    const auto assembleAndSolve = [&](bool withTemporal, std::vector<double>& xOut) -> Status {
        const auto tAsm = Clock::now();
        std::vector<double> b = baseB;  // interleaved
        system.copyFrom(baseA);
        for (const Match& m : matches) {
            if (!(m.weight > 0.0)) {
                continue;
            }
            std::array<Entry, 4> e{};
            for (std::uint8_t k = 0; k < m.st.n; ++k) {
                e[k].idx = m.st.idx[k];
                e[k].coef = m.st.w[k];
            }
            (void)addOuterComp(system, e.data(), m.st.n, m.weight, 0);
            (void)addOuterComp(system, e.data(), m.st.n, m.weight, 1);
            for (std::uint8_t k = 0; k < m.st.n; ++k) {
                b[il(m.st.idx[k], 0)] += m.weight * m.st.w[k] * m.mLon;
                b[il(m.st.idx[k], 1)] += m.weight * m.st.w[k] * m.mLat;
            }
        }
        const bool temporalOn = withTemporal && !xPrev.empty();
        for (std::size_t v = 0; v < N; ++v) {
            const auto vi = static_cast<std::uint32_t>(v);
            for (int c = 0; c < 2; ++c) {
                const std::size_t base = static_cast<std::size_t>(c) * N;
                if (temporalOn) {
                    (void)system.add(il(vi, c), il(vi, c), anchorW[v] + temporalW[v]);
                } else {
                    (void)system.add(il(vi, c), il(vi, c), anchorW[v]);
                }
                b[il(vi, c)] += anchorW[v] * x0[base + v];
                if (temporalOn) {
                    b[il(vi, c)] += temporalW[v] * xPrev[base + v];
                }
            }
        }
        rep.assembleMs += msSince(tAsm);

        // ---- solve exactly: factor this system, two triangular sweeps ---------------------
        // About 17 million multiply-adds at the defaults (2 x 128 x 11
        // unknowns, half width 109): cheaper and more predictable than
        // conjugate gradients on the previous iteration's factor, which the
        // benefit gate's reweighting left 17-44 steps from converging.
        const auto tFactor = Clock::now();
        if (!system.factor()) {
            return failStatus(ErrorCode::Internal, "solveMeshWarp: the normal equations are not positive definite");
        }
        system.solveInPlace(b.data());
        xOut.assign(2u * N, 0.0);
        for (std::size_t v = 0; v < N; ++v) {
            xOut[v] = b[2u * v];
            xOut[N + v] = b[2u * v + 1u];
        }
        rep.factorMs += msSince(tFactor);
        return okStatus();
    };

    for (int it = 0; it < params.irlsIterations; ++it) {
        const auto tAsm = Clock::now();
        // ---- the match weights: benefit x robust (from the last iterate) ----------
        if (it > 0) {
            // robustScalePx 0: no robust reweighting (the benefit gate alone).
            const double c2 = params.robustScalePx * params.robustScalePx;
            for (Match& m : matches) {
                double robust = 1.0;
                if (c2 > 0.0) {
                    const double rLon = fieldAt(m.st, x, N, 0) - m.mLon;
                    const double rLat = fieldAt(m.st, x, N, 1) - m.mLat;
                    const double r2 = rLon * rLon + rLat * rLat;
                    robust = std::isfinite(r2) ? 1.0 / (1.0 + r2 / c2) : 0.0;
                }
                m.weight = m.baseWeight * robust * cellGateWeight[m.cell];
            }
        }
        // ---- per vertex: the data evidence, then the anchor and the temporal pull ----
        std::fill(dataDiag.begin(), dataDiag.end(), 0.0);
        std::fill(dataSum.begin(), dataSum.end(), 0.0);
        for (const Match& m : matches) {
            for (std::uint8_t k = 0; k < m.st.n; ++k) {
                dataDiag[m.st.idx[k]] += m.weight * m.st.w[k] * m.st.w[k];
                dataSum[m.st.idx[k]] += m.weight * m.st.w[k];
            }
        }
        for (std::size_t v = 0; v < N; ++v) {
            // Jiang & Gu's E_gs: strong where nothing supports the vertex,
            // down to the floor where its data density is full - and, without
            // data, weakened where both lenses see the vertex's cells.
            const double density = dataSum[v] / g.cellArea;
            const double support = smoothstep01(density / params.anchorDataFull);
            const double noData =
                (1.0 - support) * (1.0 - (1.0 - params.anchorCovisibleScale) * covisibleShare[v]);
            anchorW[v] =
                params.anchorWeight * g.cellArea * (params.anchorFloor + (1.0 - params.anchorFloor) * noData);
            // Jiang & Gu's E_gt, weighted - like their sigma - by how much the
            // scene moved: the change the DATA ask for (the first solve, made
            // without this term) against temporalScalePx through a Cauchy
            // weight, so measurement noise is held and a real change is
            // followed.  Judged on the held-back iterate instead, a real
            // change never looked larger than the scale and moved only a
            // quarter of the way per solve.  With a single IRLS solve there
            // is no free solve to judge by: the term applies in full.
            temporalW[v] = 0.0;
            if (!xPrev.empty() && (it > 0 || params.irlsIterations == 1)) {
                double robust = 1.0;
                if (!xFree.empty()) {
                    const double dl = xFree[v] - xPrev[v];
                    const double dt = xFree[N + v] - xPrev[N + v];
                    const double d2 = (dl * dl + dt * dt) / (params.temporalScalePx * params.temporalScalePx);
                    robust = std::isfinite(d2) ? 1.0 / (1.0 + d2) : 0.0;
                }
                temporalW[v] =
                    robust * (params.temporalWeight * dataDiag[v] + params.temporalFloor * g.cellArea);
            }
        }
        // ---- assemble the system ------------------------------------------------------
        // (assembleAndSolve, below the loop's weights: the same assembly also
        // serves the solve WITHOUT the temporal term after the loop.)
        rep.assembleMs += msSince(tAsm);
        if (const Status solved = assembleAndSolve(true, x); !solved.ok()) {
            return solved.error();
        }
        ++rep.irlsIterations;
        if (it == 0 && !xPrev.empty() && params.irlsIterations > 1) {
            xFree = x;  // what the data ask for: the temporal term judges the change by it
        }
        for (const double v : x) {
            if (!std::isfinite(v)) {
                return Error{ErrorCode::Internal, "solveMeshWarp: the solve produced a non-finite field"};
            }
        }

        // ---- the benefit gate, once, on the first solve ------------------------------------
        // ParallaxWarpParams::requiredImprovement: a correction must make the
        // two lenses agree better than the bands as rendered before its data
        // keep their say.  Per band pixel: the residual with the solve's
        // displacement on top of the band warp (slave at p - d, master at
        // p + d) against the null residual at the same interpolation phases;
        // pooled per mesh cell over 3 x 3 cells, the ratio decides the
        // cell's weight (smoothstep, with the grid's dead zone).
        if (benefitOn && !gateRan && it + 1 < params.irlsIterations) {
            const auto tGate = Clock::now();
            gateRan = true;
            const std::uint32_t bw = verify->w;
            const std::uint32_t bh = verify->h;
            const double radPerCol = kTwoPi / static_cast<double>(bw);
            const double radPerRow = kPi / static_cast<double>(verify->mapH);
            // Per band row: the sums of one cell row (a band row lies in one).
            struct RowSums {
                std::vector<double> nul, warped, n;
            };
            std::vector<RowSums> rows(bh);
            const auto gateRow = [&](std::size_t rIndex) {
                const auto r = static_cast<std::uint32_t>(rIndex);
                RowSums& rs = rows[rIndex];
                rs.nul.assign(g.W, 0.0);
                rs.warped.assign(g.W, 0.0);
                rs.n.assign(g.W, 0.0);
                const StencilRow& sr = verifyStencils.rows[r];
                if (!sr.inside) {
                    return;
                }
                // Every second pixel of every second row: a cell pools 3 x 3
                // cells of >= 136 pixels each, so a quarter of them still
                // gives the ratio hundreds of samples, at a quarter of the cost.
                if (r % 2u != 0u) {
                    return;
                }
                for (std::uint32_t col = 0; col < bw; col += 2u) {
                    const std::size_t i = static_cast<std::size_t>(r) * bw + col;
                    if (!(verify->alpha[0][i] > 0.5f) || !(verify->alpha[1][i] > 0.5f)) {
                        continue;
                    }
                    const Stencil st = makeStencil(g, verifyStencils.cols[col], sr);
                    // The solve's displacement relative to the bands as rendered.
                    double dLon = fieldAt(st, x, N, 0);
                    double dLat = fieldAt(st, x, N, 1);
                    if (verifyWarp != nullptr) {
                        dLon -= sampleGrid(*verifyWarp, verifyStencils.lon[col], verifyStencils.lat[r], 0) /
                                g.radPerPxLon;
                        dLat -= sampleGrid(*verifyWarp, verifyStencils.lon[col], verifyStencils.lat[r], 1) /
                                g.radPerPxLat;
                    }
                    // Band pixels: east is +column, north is -row.
                    const double dx = dLon * (g.radPerPxLon / radPerCol);
                    const double dy = -dLat * (g.radPerPxLat / radPerRow);
                    const double px = static_cast<double>(col) + 0.5;
                    const double py = static_cast<double>(r) + 0.5;
                    const double sM = sampleBandPlane(verify->luma[0], bw, bh, px - dx, py - dy);
                    const double mM = sampleBandPlane(verify->luma[1], bw, bh, px - dx, py - dy);
                    const double sP = sampleBandPlane(verify->luma[0], bw, bh, px + dx, py + dy);
                    const double mP = sampleBandPlane(verify->luma[1], bw, bh, px + dx, py + dy);
                    rs.nul[st.cellCol] += 0.5 * (std::fabs(sM - mM) + std::fabs(sP - mP));
                    rs.warped[st.cellCol] += std::fabs(sM - mP);
                    rs.n[st.cellCol] += 1.0;
                }
            };
            forTasks(pool, bh, gateRow);
            std::vector<double> nul(cellCount, 0.0), warped(cellCount, 0.0), cnt(cellCount, 0.0);
            for (std::uint32_t r = 0; r < bh; ++r) {
                const StencilRow& sr = verifyStencils.rows[r];
                if (!sr.inside) {
                    continue;
                }
                for (std::uint32_t i = 0; i < g.W; ++i) {
                    const std::size_t c = static_cast<std::size_t>(sr.y0) * g.W + i;
                    nul[c] += rows[r].nul[i];
                    warped[c] += rows[r].warped[i];
                    cnt[c] += rows[r].n[i];
                }
            }
            const double zeroAt = 1.0 - 0.25 * pp.requiredImprovement;
            const double fullAt = 1.0 - pp.requiredImprovement;
            const int rowsOfCells = static_cast<int>(g.H - 1u);
            const int Wi = static_cast<int>(g.W);
            for (int cr = 0; cr < rowsOfCells; ++cr) {
                for (int cc = 0; cc < Wi; ++cc) {
                    double n = 0.0, sn = 0.0, sw = 0.0;
                    for (int dr = -1; dr <= 1; ++dr) {
                        const int rr = std::clamp(cr + dr, 0, rowsOfCells - 1);
                        for (int dc = -1; dc <= 1; ++dc) {
                            const int c2 = (cc + dc + Wi) % Wi;
                            const std::size_t k = static_cast<std::size_t>(rr) * g.W + static_cast<std::size_t>(c2);
                            n += cnt[k];
                            sn += nul[k];
                            sw += warped[k];
                        }
                    }
                    const std::size_t k = static_cast<std::size_t>(cr) * g.W + static_cast<std::size_t>(cc);
                    double wgt = 1.0;  // no pixel nearby: no verdict, nothing to gate
                    if (n > 0.0) {
                        const double eu = sn / n;
                        const double ew = sw / n;
                        // Nothing measurable to fix: nothing to justify a bend.
                        wgt = eu >= pp.minResidual ? smoothstep01((zeroAt - ew / eu) / (zeroAt - fullAt)) : 0.0;
                    }
                    cellGateWeight[k] = wgt;
                    cellGate[k] = static_cast<float>(wgt);
                }
            }
            rep.benefitMs += msSince(tGate);
        }
    }

    // =========================================================================
    //  7. Energies, report, grid
    // =========================================================================
    const auto energyAt = [&](const std::vector<double>& v, double& lineRms) {
        MeshWarpEnergy e;
        for (const Match& m : matches) {
            const double rLon = fieldAt(m.st, v, N, 0) - m.mLon;
            const double rLat = fieldAt(m.st, v, N, 1) - m.mLat;
            e.alignment += m.weight * (rLon * rLon + rLat * rLat);
        }
        double sq = 0.0;
        for (const LineTriple& t : lineTriples) {
            const double dl = termDot(t, v, 0u);
            const double dt = termDot(t, v, N);
            // The exact term: the second difference across the line.
            const double r = t.nLon * dl + t.nLat * dt;
            e.lines += t.weight * r * r;
            sq += r * r;
        }
        lineRms = lineTriples.empty() ? 0.0 : std::sqrt(sq / static_cast<double>(lineTriples.size()));
        for (const ShapeTerm& t : shape) {
            const double r = termDot(t, v, static_cast<std::size_t>(t.comp) * N) - t.target;
            e.shape += t.weight * r * r;
        }
        for (std::size_t k = 0; k < N; ++k) {
            const double al = v[k] - x0[k];
            const double at = v[N + k] - x0[N + k];
            e.anchor += anchorW[k] * (al * al + at * at);
            if (!xPrev.empty()) {
                const double tl = v[k] - xPrev[k];
                const double tt = v[N + k] - xPrev[N + k];
                e.temporal += temporalW[k] * (tl * tl + tt * tt);
            }
        }
        return e;
    };
    rep.before = energyAt(x0, rep.lineResidualBeforePx);
    rep.after = energyAt(x, rep.lineResidualAfterPx);
    rep.mode = MeshWarpMode::Solved;
    rep.temporal = !xPrev.empty();
    for (const Match& m : matches) {
        if (m.weight > 0.0) {
            ++rep.matches;
            rep.matchWeight += m.weight;
        }
    }
    if (!xPrev.empty()) {
        double sum = 0.0;
        std::size_t n = 0;
        for (std::uint32_t j = 1; j + 1u < g.H; ++j) {
            if (std::fabs(g.latOfRow(j)) > g.bandHalfRad + 1e-9) {
                continue;
            }
            for (std::uint32_t i = 0; i < g.W; ++i) {
                const std::uint32_t v = g.index(i, j);
                const double dl = (x[v] - xPrev[v]) * g.radPerPxLon;
                const double dt = (x[N + v] - xPrev[N + v]) * g.radPerPxLat;
                sum += std::hypot(dl, dt);
                ++n;
            }
        }
        rep.meanAbsChangeFromPreviousDeg = n ? rad2deg(sum / static_cast<double>(n)) : 0.0;
    }

    result.grid = finishGrid(x);
    // The cells' data weight BEFORE the benefit gate decides "gated": a cell
    // with matches whose gate is 0 is gated, one without matches unmeasured.
    std::vector<double> preGateWeight(cellCount, 0.0);
    for (const Match& m : matches) {
        preGateWeight[m.cell] += m.baseWeight;
    }
    fillDiagnostics(result.grid, preGateWeight, cellGate);
    rep.benefitGatedCells = result.grid.gatedCells;

    // ---- the field on its own: the last solve again, without the temporal term ----------
    // The match weights (benefit gate, Cauchy), the anchor and the constant
    // part stay exactly as the last iteration left them; only the pull toward
    // the previous mesh is left out.  At the defaults those weights never
    // depended on the previous mesh (the gate judged the first, temporal-free
    // solve), so this is the solve a caller without a previous mesh gets.
    if (in.solveAlone) {
        if (xPrev.empty()) {
            result.alone = result.grid;  // nothing temporal took part
        } else {
            const auto tAlone = Clock::now();
            std::vector<double> xAlone;
            if (const Status solved = assembleAndSolve(false, xAlone); !solved.ok()) {
                return solved.error();
            }
            for (const double v : xAlone) {
                if (!std::isfinite(v)) {
                    return Error{ErrorCode::Internal, "solveMeshWarp: the solve without the temporal term produced a "
                                                      "non-finite field"};
                }
            }
            ParallaxWarpGrid alone = finishGrid(xAlone);
            fillDiagnostics(alone, preGateWeight, cellGate);
            result.alone = std::move(alone);
            rep.aloneMs = msSince(tAlone);
        }
    }
    rep.totalMs = msSince(tStart);
    result.grid.gridMs = rep.totalMs;
    if (result.alone) {
        result.alone->gridMs = rep.totalMs;
    }
    return result;
}

// ===========================================================================
//  The entry points that compute the flows, and render the bands
// ===========================================================================

namespace {

/// The bidirectional flow between a band set's two lenses with the
/// parameters' backend; `used` receives what actually ran.
Result<BidirFlow> bandFlow(const LensBands& bands, const MeshWarpParams& params, ThreadPool* pool,
                           FlowBackendKind& used) {
    if (bands.w == 0 || bands.h == 0) {
        return Error{ErrorCode::InvalidArgument, "meshWarpFromBands: empty bands"};
    }
    // The band luma planes are already the flow solver's input shape.
    GrayImage a;
    a.w = bands.w;
    a.h = bands.h;
    a.data = bands.luma[0];
    GrayImage b;
    b.w = bands.w;
    b.h = bands.h;
    b.data = bands.luma[1];
    if (!a.valid() || !b.valid()) {
        return Error{ErrorCode::InvalidArgument, "meshWarpFromBands: the band planes are malformed"};
    }
    used = params.parallax.backend;
    return computeFlow(params.parallax.backend, a, b, params.parallax.flow, pool, &used);
}

}  // namespace

Result<MeshWarpResult> meshWarpFromBands(const LensBands& raw, const LensBands* prewarped,
                                         const ParallaxWarpGrid* prewarp, const std::vector<SeamLine>* lines,
                                         const ParallaxWarpGrid* prior, const ParallaxWarpGrid* previous,
                                         const MeshWarpParams& params, ThreadPool* pool, double bandMs,
                                         bool solveAlone, std::string* flowFailure) {
    if (flowFailure != nullptr) {
        flowFailure->clear();
    }
    OSV_TRY(checkMeshWarpParams(params));
    if (prewarped != nullptr && (prewarp == nullptr || !prewarp->valid())) {
        return Error{ErrorCode::InvalidArgument, "meshWarpFromBands: prewarped bands without the field they "
                                                 "were rendered with"};
    }
    // ---- the primary measurement: the flow on the raw bands --------------------------
    // A flow that fails leaves its measurement out instead of failing the
    // field: the solve then runs on the lines, the prior and the previous
    // mesh, like a band with nothing to match (one field, one code path).
    const auto tFlow = Clock::now();
    FlowBackendKind used = params.parallax.backend;
    std::string failure;
    std::optional<BidirFlow> flow;
    if (auto f = bandFlow(raw, params, pool, used); f.ok()) {
        flow = std::move(f).value();
    } else {
        failure = f.error().message;
    }
    // ---- the refined one: the flow on the prewarped bands (the residual) -------------
    // Only beside a primary one (solveMeshWarp's rule): the refined flow alone
    // would carry no structured gate.
    std::optional<BidirFlow> flow2;
    if (prewarped != nullptr && flow) {
        FlowBackendKind used2 = params.parallax.backend;
        if (auto f = bandFlow(*prewarped, params, pool, used2); f.ok()) {
            flow2 = std::move(f).value();
        } else {
            failure = f.error().message;
        }
    }
    if (!failure.empty()) {
        log::warn("mesh warp: a band flow failed ({}); the field is solved without its matches", failure);
        if (flowFailure != nullptr) {
            *flowFailure = failure;
        }
    }
    MeshWarpInputs in;
    in.bands = &raw;
    in.flow = flow ? &*flow : nullptr;
    in.bandWarp = nullptr;
    if (prewarped != nullptr && flow2) {
        in.bands2 = prewarped;
        in.flow2 = &*flow2;
        in.bandWarp2 = prewarp;
    }
    in.verifyBands = &raw;
    in.lines = lines;
    in.prior = prior;
    in.previous = previous;
    in.solveAlone = solveAlone;
    in.usedBackend = used;
    in.bandMs = bandMs;
    in.flowMs = msSince(tFlow);
    return solveMeshWarp(in, params, pool);
}

Result<MeshWarpResult> buildMeshWarp(const geom::LensRig& rig, const video::FramePair& frames,
                                     const geom::BlendParams& blend, const MeshWarpParams& params,
                                     const std::vector<float>* tableDeg, bool prewarp,
                                     const ParallaxWarpGrid* previous, ThreadPool& pool, MeshWarpBuildInfo* info,
                                     bool solveAlone) {
    OSV_TRY(checkMeshWarpParams(params));
    MeshWarpBuildInfo local;
    MeshWarpBuildInfo& inf = info != nullptr ? *info : local;
    inf = MeshWarpBuildInfo{};

    // ---- the raw bands: the lines, the primary flow, the verification ------------------
    const auto tRaw = Clock::now();
    OSV_TRY_ASSIGN(LensBands raw, renderLensBands(rig, frames, blend, params.parallax.band, false, nullptr, pool));
    inf.rawBandMs = msSince(tRaw);
    OSV_TRY_ASSIGN(inf.lines, detectSeamLines(raw, LineDetectParams{}, &pool));

    // ---- the prior: the table's lift (zero without a table) ---------------------------
    const bool haveTable = tableDeg != nullptr && !tableDeg->empty();
    OSV_TRY_ASSIGN(ParallaxWarpGrid prior,
                   haveTable ? liftSeamTable(*tableDeg, params) : meshWarpLayout(params));

    // ---- the refined measurement's bands: prewarped by the prior ------------------------
    if (haveTable && prewarp) {
        const WarpGridView view = warpGridView(prior);
        const auto tWarped = Clock::now();
        OSV_TRY_ASSIGN(LensBands warped,
                       renderLensBands(rig, frames, blend, params.parallax.band, false, nullptr, pool, &view));
        inf.warpedBandMs = msSince(tWarped);
        inf.bandsPrewarped = true;
        return meshWarpFromBands(raw, &warped, &prior, &inf.lines.lines, &prior, previous, params, &pool,
                                 inf.rawBandMs + inf.warpedBandMs, solveAlone, &inf.flowFailure);
    }
    return meshWarpFromBands(raw, nullptr, nullptr, &inf.lines.lines, haveTable ? &prior : nullptr, previous,
                             params, &pool, inf.rawBandMs, solveAlone, &inf.flowFailure);
}

// ===========================================================================
//  Line straightness
// ===========================================================================

Result<LineStraightness> measureLineStraightness(const std::vector<SeamLine>& lines, std::uint32_t bandEquirectW,
                                                 const ParallaxWarpGrid* grid, const std::vector<float>* tableDeg,
                                                 double spacingPx) {
    if (bandEquirectW < 64 || bandEquirectW > 16384 || !(spacingPx >= 0.25) || !std::isfinite(spacingPx)) {
        return Error{ErrorCode::InvalidArgument, "measureLineStraightness: bad band width or sample spacing"};
    }
    if (grid != nullptr && !grid->valid()) {
        return Error{ErrorCode::InvalidArgument, "measureLineStraightness: the grid is malformed"};
    }
    // One band pixel in radians, both directions (the 2:1 polar map).
    const double radPerPxLon = kTwoPi / static_cast<double>(bandEquirectW);
    const double radPerPxLat = kPi / static_cast<double>(bandEquirectW / 2u);
    const std::vector<float>* table = (tableDeg != nullptr && !tableDeg->empty()) ? tableDeg : nullptr;

    // The master's displacement at (lon, lat), band pixels (east, north):
    // the grid as osvWarpSample fetches it plus the 1-D table as the kernel's
    // 1-D path moves the ray (its column by osvSeamColumn's rule, half the
    // value away from the master's axis - south - tapered).
    const auto displacement = [&](double lon, double lat, double& dLon, double& dLat) {
        dLon = 0.0;
        dLat = 0.0;
        if (grid != nullptr) {
            dLon += sampleGrid(*grid, lon, lat, 0) / radPerPxLon;
            dLat += sampleGrid(*grid, lon, lat, 1) / radPerPxLat;
        }
        if (table != nullptr && std::isfinite(lon) && std::isfinite(lat)) {
            const double wrapped = lon - std::floor((lon + kPi) / kTwoPi) * kTwoPi;
            const double f = std::clamp((wrapped + kPi) / kTwoPi, 0.0, 0.999999);
            const auto n = static_cast<long long>(table->size());
            const long long c = std::clamp(static_cast<long long>(f * static_cast<double>(n)), 0LL, n - 1);
            const double t = static_cast<double>((*table)[static_cast<std::size_t>(c)]);
            const double taper = static_cast<double>(osvSeamShiftTaper(static_cast<float>(std::sin(lat))));
            if (std::isfinite(t)) {
                dLat += -0.5 * deg2rad(t) * taper / radPerPxLat;
            }
        }
    };

    LineStraightness out;
    out.perLineRmsPx.assign(lines.size(), std::numeric_limits<float>::quiet_NaN());
    double sumSq = 0.0;
    double sumLineRms = 0.0;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const SeamLine& ln = lines[li];
        if (ln.lens != 0 && ln.lens != 1) {
            continue;
        }
        const double x0 = ln.lon0Rad / radPerPxLon;
        const double y0 = ln.lat0Rad / radPerPxLat;
        const double x1 = ln.lon1Rad / radPerPxLon;
        const double y1 = ln.lat1Rad / radPerPxLat;
        const double len = std::hypot(x1 - x0, y1 - y0);
        if (!std::isfinite(len) || len < 2.0 * spacingPx) {
            continue;  // fewer than three samples
        }
        const auto K = static_cast<std::size_t>(std::ceil(len / spacingPx)) + 1u;
        // The master samples at y + V(y), so its raw sample q shows at the y
        // with y + V(y) = q; the slave samples at y - V(y).  Fixed-point
        // iteration from y = q: the field is smooth and far below a pixel per
        // pixel, so four steps reach well under a thousandth of a pixel.
        const double sgn = ln.lens == 1 ? 1.0 : -1.0;
        std::vector<double> px(K), py(K);
        for (std::size_t k = 0; k < K; ++k) {
            const double t = static_cast<double>(k) / static_cast<double>(K - 1u);
            const double qx = x0 + t * (x1 - x0);
            const double qy = y0 + t * (y1 - y0);
            double yx = qx;
            double yy = qy;
            for (int iter = 0; iter < 4; ++iter) {
                double dl = 0.0;
                double dt = 0.0;
                displacement(yx * radPerPxLon, yy * radPerPxLat, dl, dt);
                yx = qx - sgn * dl;
                yy = qy - sgn * dt;
            }
            px[k] = yx;
            py[k] = yy;
        }
        // Total least squares: the line through the centroid along the
        // principal axis; residuals are the perpendicular distances.
        double mx = 0.0, my = 0.0;
        for (std::size_t k = 0; k < K; ++k) {
            mx += px[k];
            my += py[k];
        }
        mx /= static_cast<double>(K);
        my /= static_cast<double>(K);
        double sxx = 0.0, syy = 0.0, sxy = 0.0;
        for (std::size_t k = 0; k < K; ++k) {
            const double dx = px[k] - mx;
            const double dy = py[k] - my;
            sxx += dx * dx;
            syy += dy * dy;
            sxy += dx * dy;
        }
        const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
        const double nx = -std::sin(angle);
        const double ny = std::cos(angle);
        double lineSq = 0.0;
        double lineMax = 0.0;
        for (std::size_t k = 0; k < K; ++k) {
            const double r = (px[k] - mx) * nx + (py[k] - my) * ny;
            lineSq += r * r;
            lineMax = std::max(lineMax, std::fabs(r));
        }
        if (!std::isfinite(lineSq)) {
            continue;
        }
        const double lineRms = std::sqrt(lineSq / static_cast<double>(K));
        out.perLineRmsPx[li] = static_cast<float>(lineRms);
        out.maxPx = std::max(out.maxPx, lineMax);
        sumSq += lineSq;
        sumLineRms += lineRms;
        out.maxLineRmsPx = std::max(out.maxLineRmsPx, lineRms);
        out.samples += static_cast<std::uint32_t>(K);
        ++out.lines;
    }
    out.rmsPx = out.samples ? std::sqrt(sumSq / static_cast<double>(out.samples)) : 0.0;
    out.meanLineRmsPx = out.lines ? sumLineRms / static_cast<double>(out.lines) : 0.0;
    return out;
}

}  // namespace osv::render
