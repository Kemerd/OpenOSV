// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Flare.cpp - sun ghost detection and fitting, the veil estimate, the kernel
// parameter writer and the seam cost hook (see osv/render/Flare.h and
// docs/research/FLARE.md).
//
// Everything here runs on the small factor-4 working image of a lens (750 x
// 750 for the 6K stream), so the whole analysis is a few milliseconds of
// arithmetic plus the downsample.  The full-resolution work - evaluating the
// fitted shapes and subtracting them - happens in the render kernel.

#include "osv/render/Flare.h"

#include "osv/core/Log.h"
#include "osv/render/RenderJob.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <utility>

namespace osv::render {

namespace {

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

/// BT.2020 luma weights, the ones every analysis in the project uses on
/// linear RGB (SeamAnalysis.cpp lumaOf).  For flare the exact weights matter
/// little: the same weights are used for the ghost and its background.
constexpr double kLumaR = 0.2627;
constexpr double kLumaG = 0.6780;
constexpr double kLumaB = 0.0593;

/// Largest analysis factor accepted (a 16 x 16 block per analysis pixel).
constexpr std::uint32_t kMaxFactor = 16;

/// Nonlinear parameters of one ghost fit, in this order.
enum FitParam : int { kPx = 0, kPy, kPhx, kPhy, kPrho, kPang, kPsoft, kNumFitParams };

/// Linear coefficients per channel: 6 quadratic background terms, then the
/// ghost's plateau, rim, x tilt and y tilt (the kernel's light model).
constexpr int kNumBackground = 6;
constexpr int kNumGhostTerms = 4;
constexpr int kNumLinear = kNumBackground + kNumGhostTerms;

/// Luma of an RGB triple.
[[nodiscard]] inline double lumaOf(const double* rgb) noexcept {
    return kLumaR * rgb[0] + kLumaG * rgb[1] + kLumaB * rgb[2];
}

/// True when every value is finite.
template <class... T>
[[nodiscard]] bool allFinite(T... v) noexcept {
    return (std::isfinite(static_cast<double>(v)) && ...);
}

// ---------------------------------------------------------------------------
//  Small float image helpers (analysis-size images only)
// ---------------------------------------------------------------------------

/// A single-channel float image.
struct Gray {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<float> v;

    Gray() = default;
    Gray(std::uint32_t width, std::uint32_t height, float fill = 0.0f)
        : w(width), h(height), v(static_cast<std::size_t>(width) * height, fill) {}

    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y) const noexcept {
        return v[static_cast<std::size_t>(y) * w + x];
    }
    [[nodiscard]] float& at(std::uint32_t x, std::uint32_t y) noexcept { return v[static_cast<std::size_t>(y) * w + x]; }
};

/// Run body(first, last) over lines [0, lines): spread over `pool` when one
/// is given (lines are independent), otherwise - or if the pool refuses -
/// in order on the calling thread.  The result is identical either way.
void forLines(ThreadPool* pool, int lines, const std::function<void(int, int)>& body) {
    if (lines <= 0) {
        return;
    }
    if (pool && lines > 32) {
        const Status st = pool->parallelFor(0, static_cast<std::size_t>(lines), 16, [&](std::size_t b, std::size_t e) {
            body(static_cast<int>(b), static_cast<int>(e));
        });
        if (st.ok()) {
            return;
        }
    }
    body(0, lines);
}

/// One horizontal or vertical box-filter pass of half width `r`, clamp to
/// edge, running sums in double so a long row cannot drift.
void boxPass(const Gray& src, Gray& dst, int r, bool horizontal, ThreadPool* pool) {
    dst.w = src.w;
    dst.h = src.h;
    dst.v.resize(src.v.size());
    if (r <= 0) {
        dst.v = src.v;
        return;
    }
    const int W = static_cast<int>(src.w);
    const int H = static_cast<int>(src.h);
    const int lines = horizontal ? H : W;
    const int len = horizontal ? W : H;
    const double norm = 1.0 / static_cast<double>(2 * r + 1);
    // Index of element i on line l, along the pass direction.
    auto idx = [&](int l, int i) -> std::size_t {
        return horizontal ? static_cast<std::size_t>(l) * static_cast<std::size_t>(W) + static_cast<std::size_t>(i)
                          : static_cast<std::size_t>(i) * static_cast<std::size_t>(W) + static_cast<std::size_t>(l);
    };
    forLines(pool, lines, [&](int first, int last) {
        for (int l = first; l < last; ++l) {
            // Seed the window centred on element 0 with clamped taps.
            double sum = 0.0;
            for (int k = -r; k <= r; ++k) {
                sum += src.v[idx(l, std::clamp(k, 0, len - 1))];
            }
            for (int i = 0; i < len; ++i) {
                dst.v[idx(l, i)] = static_cast<float>(sum * norm);
                // Slide: drop the tap leaving on the left, add the one entering.
                const int out = std::clamp(i - r, 0, len - 1);
                const int in = std::clamp(i + r + 1, 0, len - 1);
                sum += static_cast<double>(src.v[idx(l, in)]) - static_cast<double>(src.v[idx(l, out)]);
            }
        }
    });
}

/// Gaussian blur of standard deviation `sigma` approximated by three box
/// passes per axis (the sizes of Wells, "Efficient synthesis of Gaussian
/// filters by cascaded uniform filters", IEEE PAMI 1986).  Exact enough for
/// a background estimate, and O(1) per pixel whatever the sigma.
Gray gaussBoxes(const Gray& src, double sigma, ThreadPool* pool) {
    if (!(sigma > 0.3) || src.v.empty()) {
        return src;
    }
    constexpr int n = 3;
    // Ideal box width for n passes, then the nearest odd widths below and
    // above, and how many passes use the smaller one.
    const double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0) {
        --wl;
    }
    wl = std::max(wl, 1);
    const int wu = wl + 2;
    const double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    const int m = std::clamp(static_cast<int>(std::lround(mIdeal)), 0, n);
    Gray a = src;
    Gray b;
    for (int pass = 0; pass < n; ++pass) {
        const int r = ((pass < m ? wl : wu) - 1) / 2;
        boxPass(a, b, r, true, pool);
        boxPass(b, a, r, false, pool);
    }
    return a;
}

/// Exact separable Gaussian (radius ceil(3 sigma)), clamp to edge - for the
/// small sigmas where three boxes are too coarse.
Gray gaussExact(const Gray& src, double sigma, ThreadPool* pool) {
    if (!(sigma > 0.05) || src.v.empty()) {
        return src;
    }
    const int r = std::max(1, static_cast<int>(std::ceil(3.0 * sigma)));
    std::vector<double> k(static_cast<std::size_t>(2 * r + 1));
    double sum = 0.0;
    for (int i = -r; i <= r; ++i) {
        const double e = std::exp(-0.5 * (i * i) / (sigma * sigma));
        k[static_cast<std::size_t>(i + r)] = e;
        sum += e;
    }
    for (double& e : k) {
        e /= sum;
    }
    const int W = static_cast<int>(src.w);
    const int H = static_cast<int>(src.h);
    Gray tmp(src.w, src.h);
    Gray out(src.w, src.h);
    // Rows.
    forLines(pool, H, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            for (int x = 0; x < W; ++x) {
                double acc = 0.0;
                for (int i = -r; i <= r; ++i) {
                    acc += k[static_cast<std::size_t>(i + r)] *
                           src.at(static_cast<std::uint32_t>(std::clamp(x + i, 0, W - 1)), static_cast<std::uint32_t>(y));
                }
                tmp.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) = static_cast<float>(acc);
            }
        }
    });
    // Columns.
    forLines(pool, H, [&](int first, int last) {
        for (int y = first; y < last; ++y) {
            for (int x = 0; x < W; ++x) {
                double acc = 0.0;
                for (int i = -r; i <= r; ++i) {
                    acc += k[static_cast<std::size_t>(i + r)] *
                           tmp.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(std::clamp(y + i, 0, H - 1)));
                }
                out.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) = static_cast<float>(acc);
            }
        }
    });
    return out;
}

/// Median of the values selected by `keep` (0 when none are).
template <class Pred>
double medianOf(const Gray& img, Pred keep) {
    std::vector<float> vals;
    vals.reserve(img.v.size() / 2);
    for (std::uint32_t y = 0; y < img.h; ++y) {
        for (std::uint32_t x = 0; x < img.w; ++x) {
            if (keep(x, y)) {
                vals.push_back(img.at(x, y));
            }
        }
    }
    if (vals.empty()) {
        return 0.0;
    }
    auto mid = vals.begin() + static_cast<std::ptrdiff_t>(vals.size() / 2);
    std::nth_element(vals.begin(), mid, vals.end());
    return static_cast<double>(*mid);
}

// ---------------------------------------------------------------------------
//  Connected components (8-connected) on a mask
// ---------------------------------------------------------------------------

/// Statistics of one component.
struct Blob {
    std::uint32_t area = 0;
    std::uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  ///< Inclusive bounding box.
    double sx = 0.0, sy = 0.0;                      ///< Sum of pixel-centre coordinates.
    double sxx = 0.0, syy = 0.0, sxy = 0.0;         ///< Second moments (raw sums).
    double peak = -std::numeric_limits<double>::infinity();  ///< Max of the scored map inside.

    [[nodiscard]] double cx() const noexcept { return area ? sx / area : 0.0; }
    [[nodiscard]] double cy() const noexcept { return area ? sy / area : 0.0; }
    [[nodiscard]] std::uint32_t bw() const noexcept { return x1 - x0 + 1; }
    [[nodiscard]] std::uint32_t bh() const noexcept { return y1 - y0 + 1; }
    [[nodiscard]] double fill() const noexcept { return static_cast<double>(area) / (double(bw()) * double(bh())); }
    [[nodiscard]] double aspect() const noexcept {
        return static_cast<double>(std::max(bw(), bh())) / static_cast<double>(std::min(bw(), bh()));
    }
};

/// Label the set pixels of `mask` (w * h bytes) into blobs, scoring each
/// blob's peak from `score` (may be null).
std::vector<Blob> findBlobs(const std::vector<std::uint8_t>& mask, std::uint32_t w, std::uint32_t h,
                            const Gray* score) {
    std::vector<Blob> blobs;
    if (mask.size() != static_cast<std::size_t>(w) * h) {
        return blobs;
    }
    std::vector<std::uint8_t> seen(mask.size(), 0);
    std::vector<std::uint32_t> stack;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i0 = static_cast<std::size_t>(y) * w + x;
            if (!mask[i0] || seen[i0]) {
                continue;
            }
            // Iterative flood fill from (x, y).
            Blob b;
            b.x0 = b.x1 = x;
            b.y0 = b.y1 = y;
            seen[i0] = 1;
            stack.clear();
            stack.push_back(static_cast<std::uint32_t>(i0));
            while (!stack.empty()) {
                const std::uint32_t i = stack.back();
                stack.pop_back();
                const std::uint32_t px = i % w;
                const std::uint32_t py = i / w;
                const double cxp = px + 0.5;
                const double cyp = py + 0.5;
                ++b.area;
                b.sx += cxp;
                b.sy += cyp;
                b.sxx += cxp * cxp;
                b.syy += cyp * cyp;
                b.sxy += cxp * cyp;
                b.x0 = std::min(b.x0, px);
                b.x1 = std::max(b.x1, px);
                b.y0 = std::min(b.y0, py);
                b.y1 = std::max(b.y1, py);
                if (score) {
                    b.peak = std::max(b.peak, static_cast<double>(score->v[i]));
                }
                // Visit the eight neighbours.
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        const int nx = static_cast<int>(px) + dx;
                        const int ny = static_cast<int>(py) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<int>(w) || ny >= static_cast<int>(h)) {
                            continue;
                        }
                        const std::size_t j = static_cast<std::size_t>(ny) * w + static_cast<std::size_t>(nx);
                        if (mask[j] && !seen[j]) {
                            seen[j] = 1;
                            stack.push_back(static_cast<std::uint32_t>(j));
                        }
                    }
                }
            }
            blobs.push_back(b);
        }
    }
    return blobs;
}

// ---------------------------------------------------------------------------
//  The ghost shape (double-precision twin of osvFlareGhostShape)
// ---------------------------------------------------------------------------

/// Nonlinear ghost parameters in analysis pixels.
struct ShapeParams {
    double p[kNumFitParams] = {};
};

/// The shape of `ShapeParams` with its trigonometry evaluated once, so the
/// per-pixel distance below is arithmetic only (it runs a few million times
/// per fit).
struct ShapeGeom {
    double cx = 0.0, cy = 0.0;  ///< Centre.
    double c = 1.0, s = 0.0;    ///< cos / sin of the rotation.
    double hx = 1.0, hy = 1.0;  ///< Half extents.
    double r = 0.0;             ///< Corner radius.
    double soft = 1.0;          ///< Edge half width.

    explicit ShapeGeom(const ShapeParams& sp) noexcept
        : cx(sp.p[kPx]), cy(sp.p[kPy]), c(std::cos(sp.p[kPang])), s(std::sin(sp.p[kPang])), hx(sp.p[kPhx]),
          hy(sp.p[kPhy]), r(std::clamp(sp.p[kPrho], 0.0, 1.0) * std::min(sp.p[kPhx], sp.p[kPhy])),
          soft(sp.p[kPsoft]) {}
};

/// Signed distance from (x, y) to the rounded rectangle (negative inside),
/// the same formula as the kernel's osvFlareGhostDistance; `u` / `v`
/// (optional) receive the local coordinates scaled to +/-1 at the half
/// extents.
[[nodiscard]] double shapeDistance(const ShapeGeom& g, double x, double y, double* u = nullptr,
                                   double* v = nullptr) noexcept {
    const double dx = x - g.cx;
    const double dy = y - g.cy;
    const double sx = dx * g.c + dy * g.s;
    const double sy = -dx * g.s + dy * g.c;
    if (u) {
        *u = sx / std::max(g.hx, 1e-3);
    }
    if (v) {
        *v = sy / std::max(g.hy, 1e-3);
    }
    const double qx = std::fabs(sx) - (g.hx - g.r);
    const double qy = std::fabs(sy) - (g.hy - g.r);
    const double ox = std::max(qx, 0.0);
    const double oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - g.r;
}

/// Convenience for one-off calls.
[[nodiscard]] double shapeDistance(const ShapeParams& sp, double x, double y) noexcept {
    return shapeDistance(ShapeGeom(sp), x, y);
}

/// Plateau weight from a signed distance: the kernel's smoothstep ramp
/// (osvFlarePlateau).
[[nodiscard]] double shapeWeight(double d, double soft) noexcept {
    const double s = std::max(soft, 1e-3);
    double t = (s - d) / (2.0 * s);
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// Rim bump from a signed distance: the kernel's osvFlareRim.
[[nodiscard]] double rimWeight(double d, double soft) noexcept {
    const double t = d / (3.0 * std::max(soft, 1e-3));
    if (!(t * t < 1.0)) {
        return 0.0;
    }
    const double b = 1.0 - t * t;
    return b * b;
}

/// The four ghost columns of the design at one pixel: plateau S, rim E, and
/// the tilts S * u, S * v - the kernel's osvFlareGhostLight terms.
void ghostTerms(const ShapeGeom& g, double x, double y, double* out4) noexcept {
    double u = 0.0, v = 0.0;
    const double d = shapeDistance(g, x, y, &u, &v);
    const double s = shapeWeight(d, g.soft);
    out4[0] = s;
    out4[1] = rimWeight(d, g.soft);
    out4[2] = s * u;
    out4[3] = s * v;
}

// ---------------------------------------------------------------------------
//  Ghost fit: Levenberg-Marquardt over the shape, linear solve for the rest
// ---------------------------------------------------------------------------

/// The data of one fit window.
struct FitWindow {
    std::vector<double> x, y;        ///< Pixel centres (analysis px).
    std::vector<double> rgb;         ///< Observed RGB, 3 per pixel.
    std::vector<double> basis;       ///< 6 quadratic background terms per pixel.
    double gx = 0.0, gy = 0.0;       ///< Window centre (basis origin).
    double half = 1.0;               ///< Window half size (basis scale).

    [[nodiscard]] std::size_t size() const noexcept { return x.size(); }
};

/// Solve the symmetric positive (semi)definite n x n system A x = b in place
/// by Cholesky with a relative ridge.  Returns false when it is singular even
/// after the ridge.
bool solveSpd(double* A, double* b, int n) {
    double trace = 0.0;
    for (int i = 0; i < n; ++i) {
        trace += A[i * n + i];
    }
    const double ridge = 1e-12 * std::max(trace, 1e-30);
    for (int i = 0; i < n; ++i) {
        A[i * n + i] += ridge;
    }
    // A = L L^T, L stored in the lower triangle of A.
    for (int j = 0; j < n; ++j) {
        double d = A[j * n + j];
        for (int k = 0; k < j; ++k) {
            d -= A[j * n + k] * A[j * n + k];
        }
        if (!(d > 0.0)) {
            return false;
        }
        d = std::sqrt(d);
        A[j * n + j] = d;
        for (int i = j + 1; i < n; ++i) {
            double v = A[i * n + j];
            for (int k = 0; k < j; ++k) {
                v -= A[i * n + k] * A[j * n + k];
            }
            A[i * n + j] = v / d;
        }
    }
    // Forward then backward substitution.
    for (int i = 0; i < n; ++i) {
        double v = b[i];
        for (int k = 0; k < i; ++k) {
            v -= A[i * n + k] * b[k];
        }
        b[i] = v / A[i * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double v = b[i];
        for (int k = i + 1; k < n; ++k) {
            v -= A[k * n + i] * b[k];
        }
        b[i] = v / A[i * n + i];
    }
    return true;
}

/// Result of evaluating one shape: the linear coefficients per channel, the
/// residual vector and its sum of squares.
struct Eval {
    double coef[3][kNumLinear] = {};  ///< [channel][6 background terms, plateau, rim, tilt x, tilt y]
    std::vector<double> resid;         ///< 3 per pixel.
    double ssr = std::numeric_limits<double>::infinity();
    std::vector<double> terms;         ///< kNumGhostTerms ghost columns per pixel.
};

/// Which linear terms a solve uses.
enum class Model {
    Background,  ///< The quadratic background alone.
    Plateau,     ///< Background + the flat plateau (the geometry fit).
    Full         ///< Background + plateau, rim and both tilts.
};

/// For a fixed shape, solve the background and the ghost terms of every
/// channel by linear least squares (variable projection) and return the
/// residual.
bool evaluate(const FitWindow& win, const ShapeParams& sp, Model model, Eval& out) {
    const std::size_t n = win.size();
    const bool withGhost = model != Model::Background;
    const int m = model == Model::Full ? kNumLinear : (withGhost ? kNumBackground + 1 : kNumBackground);
    out.terms.assign(n * kNumGhostTerms, 0.0);
    if (withGhost) {
        const ShapeGeom g(sp);
        for (std::size_t i = 0; i < n; ++i) {
            ghostTerms(g, win.x[i], win.y[i], &out.terms[i * kNumGhostTerms]);
        }
    }
    // Normal equations shared by the three channels (same design matrix).
    double AtA[kNumLinear * kNumLinear] = {};
    double Atb[3][kNumLinear] = {};
    double row[kNumLinear];
    for (std::size_t i = 0; i < n; ++i) {
        for (int k = 0; k < kNumBackground; ++k) {
            row[k] = win.basis[i * kNumBackground + static_cast<std::size_t>(k)];
        }
        for (int k = 0; k < kNumGhostTerms; ++k) {
            row[kNumBackground + k] = out.terms[i * kNumGhostTerms + static_cast<std::size_t>(k)];
        }
        for (int a = 0; a < m; ++a) {
            for (int b = a; b < m; ++b) {
                AtA[a * m + b] += row[a] * row[b];
            }
            for (int c = 0; c < 3; ++c) {
                Atb[c][a] += row[a] * win.rgb[i * 3 + static_cast<std::size_t>(c)];
            }
        }
    }
    // Mirror the upper triangle.
    for (int a = 0; a < m; ++a) {
        for (int b = 0; b < a; ++b) {
            AtA[a * m + b] = AtA[b * m + a];
        }
    }
    // A ghost term with no footprint in the window leaves its column empty;
    // give it a unit diagonal so its amplitude solves to zero instead of
    // making the system singular.
    for (int a = kNumBackground; a < m; ++a) {
        if (AtA[a * m + a] <= 1e-18) {
            AtA[a * m + a] = 1.0;
        }
    }
    for (int c = 0; c < 3; ++c) {
        double A[kNumLinear * kNumLinear];
        std::memcpy(A, AtA, sizeof(double) * static_cast<std::size_t>(m * m));
        double b[kNumLinear];
        std::memcpy(b, Atb[c], sizeof(double) * static_cast<std::size_t>(m));
        if (!solveSpd(A, b, m)) {
            return false;
        }
        for (int k = 0; k < kNumLinear; ++k) {
            out.coef[c][k] = k < m ? b[k] : 0.0;
        }
    }
    // Residuals.
    out.resid.resize(n * 3);
    double ssr = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            double predicted = 0.0;
            for (int k = 0; k < kNumBackground; ++k) {
                predicted += out.coef[c][k] * win.basis[i * kNumBackground + static_cast<std::size_t>(k)];
            }
            for (int k = 0; k < kNumGhostTerms; ++k) {
                predicted +=
                    out.coef[c][kNumBackground + k] * out.terms[i * kNumGhostTerms + static_cast<std::size_t>(k)];
            }
            const double r = win.rgb[i * 3 + static_cast<std::size_t>(c)] - predicted;
            out.resid[i * 3 + static_cast<std::size_t>(c)] = r;
            ssr += r * r;
        }
    }
    out.ssr = ssr;
    return std::isfinite(ssr);
}

/// Box constraints of the shape parameters.
struct Bounds {
    double lo[kNumFitParams];
    double hi[kNumFitParams];
};

/// Clamp every parameter into its box; the angle is wrapped instead, since
/// a rectangle turned by pi is the same rectangle.
void clampParams(ShapeParams& sp, const Bounds& bd) {
    while (sp.p[kPang] > 0.5 * kPi) {
        sp.p[kPang] -= kPi;
    }
    while (sp.p[kPang] < -0.5 * kPi) {
        sp.p[kPang] += kPi;
    }
    for (int k = 0; k < kNumFitParams; ++k) {
        if (k == kPang) {
            continue;
        }
        sp.p[k] = std::clamp(sp.p[k], bd.lo[k], bd.hi[k]);
    }
}

/// Levenberg-Marquardt on the shape parameters, the linear terms of `model`
/// solved at every step.  Returns the best shape found (never worse than the
/// start) and fills `best` with its evaluation.
ShapeParams fitShape(const FitWindow& win, ShapeParams start, const Bounds& bd, int maxIterations, Model model,
                     Eval& best) {
    clampParams(start, bd);
    if (!evaluate(win, start, model, best)) {
        best.ssr = std::numeric_limits<double>::infinity();
        return start;
    }
    // Forward-difference steps per parameter (analysis px / radians).
    static constexpr double kStep[kNumFitParams] = {0.05, 0.05, 0.05, 0.05, 0.01, 0.005, 0.02};
    const std::size_t nr = best.resid.size();
    std::vector<double> J(nr * kNumFitParams);
    ShapeParams cur = start;
    double lambda = 1e-3;
    Eval trial;
    for (int it = 0; it < maxIterations; ++it) {
        // Numeric Jacobian of the residual (variable projection: every
        // column re-solves the linear part).
        for (int k = 0; k < kNumFitParams; ++k) {
            ShapeParams sp = cur;
            sp.p[k] += kStep[k];
            if (!evaluate(win, sp, model, trial)) {
                return cur;
            }
            for (std::size_t i = 0; i < nr; ++i) {
                J[i * kNumFitParams + static_cast<std::size_t>(k)] = (trial.resid[i] - best.resid[i]) / kStep[k];
            }
        }
        // J^T J and J^T r.
        double JtJ[kNumFitParams * kNumFitParams] = {};
        double Jtr[kNumFitParams] = {};
        for (std::size_t i = 0; i < nr; ++i) {
            const double* ji = &J[i * kNumFitParams];
            for (int a = 0; a < kNumFitParams; ++a) {
                Jtr[a] += ji[a] * best.resid[i];
                for (int b = a; b < kNumFitParams; ++b) {
                    JtJ[a * kNumFitParams + b] += ji[a] * ji[b];
                }
            }
        }
        for (int a = 0; a < kNumFitParams; ++a) {
            for (int b = 0; b < a; ++b) {
                JtJ[a * kNumFitParams + b] = JtJ[b * kNumFitParams + a];
            }
        }
        // Try damped steps until one lowers the cost (or give up).
        bool improved = false;
        for (int attempt = 0; attempt < 8 && !improved; ++attempt) {
            double A[kNumFitParams * kNumFitParams];
            double delta[kNumFitParams];
            for (int a = 0; a < kNumFitParams; ++a) {
                for (int b = 0; b < kNumFitParams; ++b) {
                    A[a * kNumFitParams + b] = JtJ[a * kNumFitParams + b];
                }
                A[a * kNumFitParams + a] += lambda * (JtJ[a * kNumFitParams + a] + 1e-12);
                delta[a] = -Jtr[a];
            }
            if (!solveSpd(A, delta, kNumFitParams)) {
                lambda *= 10.0;
                continue;
            }
            ShapeParams next = cur;
            for (int k = 0; k < kNumFitParams; ++k) {
                next.p[k] += delta[k];
            }
            clampParams(next, bd);
            if (evaluate(win, next, model, trial) && trial.ssr < best.ssr) {
                const double gain = (best.ssr - trial.ssr) / std::max(best.ssr, 1e-30);
                cur = next;
                std::swap(best, trial);
                lambda = std::max(lambda / 3.0, 1e-9);
                improved = true;
                // Converged: the cost no longer moves.
                if (gain < 1e-4) {
                    return cur;
                }
            } else {
                lambda *= 4.0;
            }
        }
        if (!improved) {
            break;
        }
    }
    return cur;
}

// ---------------------------------------------------------------------------
//  Sun detection
// ---------------------------------------------------------------------------

/// The sun as found in the working image (analysis px).
struct SunBlob {
    bool found = false;
    double x = 0.0, y = 0.0, radius = 0.0;
};

SunBlob detectSun(const Gray& luma, const std::vector<std::uint8_t>& inside, const FlareParams& fp) {
    SunBlob sun;
    // Frame maximum and median over the usable image circle.
    double maxY = 0.0;
    for (std::size_t i = 0; i < luma.v.size(); ++i) {
        if (inside[i] && std::isfinite(luma.v[i])) {
            maxY = std::max(maxY, static_cast<double>(luma.v[i]));
        }
    }
    // The scene's median from every other pixel both ways: a quarter of the
    // nth_element work (the per-frame sun check runs this on every frame).
    const double med = medianOf(luma, [&](std::uint32_t x, std::uint32_t y) {
        return ((x | y) & 1u) == 0u && inside[static_cast<std::size_t>(y) * luma.w + x] != 0;
    });
    // The sun is several stops above the scene; a frame whose brightest
    // pixel is not is a frame without the sun in it.
    if (!(maxY > 0.0) || !(med > 0.0) || maxY < fp.sunMinRatioToMedian * med) {
        return sun;
    }
    // Candidate pixels: within sunLevelFraction of the maximum.
    const double level = fp.sunLevelFraction * maxY;
    std::vector<std::uint8_t> mask(luma.v.size(), 0);
    for (std::size_t i = 0; i < luma.v.size(); ++i) {
        mask[i] = (inside[i] && luma.v[i] >= level) ? 1 : 0;
    }
    // The sun is the LARGEST clipped region, and it is round and solid.
    // Picking the largest compact blob instead would crown a two-pixel glint
    // on a sunlit white fuselage whenever the fuselage itself (the largest
    // clipped region, ragged) is rejected - measured on the sample's slave
    // lens, which has no sun.  So: find the largest region, then require it
    // to be compact; if it is not, the frame has no sun we can trust.
    const std::vector<Blob> blobs = findBlobs(mask, luma.w, luma.h, nullptr);
    const Blob* best = nullptr;
    for (const Blob& b : blobs) {
        if (!best || b.area > best->area) {
            best = &b;
        }
    }
    if (!best || best->area < fp.sunMinAreaPx || best->aspect() > fp.sunMaxAspect || best->fill() < fp.sunMinFill) {
        return sun;
    }
    sun.found = true;
    sun.radius = std::sqrt(static_cast<double>(best->area) / kPi);
    // Sub-pixel centroid: every pixel near the blob weighted by how far it
    // rises above the threshold, (Y - level) / (max - level) clamped to
    // [0, 1].  The clipped core weighs 1 either way; what the weights add is
    // the blob's soft edge, which a yes/no mask quantises to whole pixels -
    // on the coarse per-frame sun check that quantisation alone moved the
    // centroid by ~1.5 stream px from frame to frame.  Pixels farther than
    // 1.5 radii + 2 from the mask centroid are left out, so a glint beside
    // the sun cannot pull it.
    const double cx0 = best->cx();
    const double cy0 = best->cy();
    const double reach = 1.5 * sun.radius + 2.0;
    const double span = std::max(maxY - level, 1e-12);
    const int x0 = std::max(0, static_cast<int>(std::floor(cx0 - reach)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy0 - reach)));
    const int x1 = std::min(static_cast<int>(luma.w) - 1, static_cast<int>(std::ceil(cx0 + reach)));
    const int y1 = std::min(static_cast<int>(luma.h) - 1, static_cast<int>(std::ceil(cy0 + reach)));
    double sw = 0.0, swx = 0.0, swy = 0.0;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * luma.w + static_cast<std::size_t>(x);
            const double px = x + 0.5;
            const double py = y + 0.5;
            if (!inside[i] || (px - cx0) * (px - cx0) + (py - cy0) * (py - cy0) > reach * reach) {
                continue;
            }
            const double w = std::clamp((static_cast<double>(luma.v[i]) - level) / span, 0.0, 1.0);
            sw += w;
            swx += w * px;
            swy += w * py;
        }
    }
    sun.x = sw > 0.0 ? swx / sw : cx0;
    sun.y = sw > 0.0 ? swy / sw : cy0;
    return sun;
}

// ---------------------------------------------------------------------------
//  Candidate seeding and fitting
// ---------------------------------------------------------------------------

/// A seed for one ghost fit.
struct Seed {
    double x = 0.0, y = 0.0;     ///< Centroid (analysis px).
    double hx = 1.0, hy = 1.0;   ///< Half extents from the second moments.
    double angle = 0.0;          ///< Principal axis.
    double extent = 1.0;         ///< Larger bounding-box side.
};

/// Seed from a blob's moments: for a uniform ellipse the variance along an
/// axis is a^2 / 4, so the half extent is 2 sqrt(eigenvalue).
Seed seedFromBlob(const Blob& b) {
    Seed s;
    s.x = b.cx();
    s.y = b.cy();
    const double n = static_cast<double>(std::max<std::uint32_t>(b.area, 1));
    const double vxx = std::max(b.sxx / n - s.x * s.x, 0.0);
    const double vyy = std::max(b.syy / n - s.y * s.y, 0.0);
    const double vxy = b.sxy / n - s.x * s.y;
    const double tr = vxx + vyy;
    const double disc = std::sqrt(std::max(0.25 * (vxx - vyy) * (vxx - vyy) + vxy * vxy, 0.0));
    const double l1 = 0.5 * tr + disc;
    const double l2 = std::max(0.5 * tr - disc, 0.0);
    s.hx = std::max(2.0 * std::sqrt(l1), 1.5);
    s.hy = std::max(2.0 * std::sqrt(l2), 1.5);
    s.angle = 0.5 * std::atan2(2.0 * vxy, vxx - vyy);
    s.extent = static_cast<double>(std::max(b.bw(), b.bh()));
    return s;
}

/// A fitted ghost in analysis pixels plus its quality numbers.
struct Fitted {
    bool ok = false;
    ShapeParams sp;
    double amp[3] = {};    ///< Plateau RGB.
    double rim[3] = {};    ///< Edge bump RGB.
    double gradX[3] = {};  ///< Tilt along local x.
    double gradY[3] = {};  ///< Tilt along local y.
    double contrast = 0.0;
    double r2 = 0.0;
    double visibility = 0.0;
};

/// Fit one seed and apply the acceptance gates.
Fitted fitSeed(const FlareImage& img, const Seed& seed, const FlareParams& fp) {
    Fitted f;
    // Window: the blob plus as much background again on every side, so the
    // quadratic background is pinned by pixels the ghost does not touch.
    // Capped at 48 analysis px (96 x 96 pixels, ~9 k residuals per channel):
    // the sample's largest ghost needs 32, and one long, thin seed with an
    // uncapped window cost more than every other fit of the frame together.
    const double half = std::clamp(1.3 * seed.extent, 12.0, 48.0);
    const int x0 = std::max(0, static_cast<int>(std::floor(seed.x - half)));
    const int y0 = std::max(0, static_cast<int>(std::floor(seed.y - half)));
    const int x1 = std::min(static_cast<int>(img.w), static_cast<int>(std::ceil(seed.x + half)));
    const int y1 = std::min(static_cast<int>(img.h), static_cast<int>(std::ceil(seed.y + half)));
    if (x1 - x0 < 8 || y1 - y0 < 8) {
        return f;
    }
    FitWindow win;
    win.gx = seed.x;
    win.gy = seed.y;
    win.half = half;
    const std::size_t n = static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
    win.x.reserve(n);
    win.y.reserve(n);
    win.rgb.reserve(n * 3);
    win.basis.reserve(n * 6);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float* px = &img.rgb[(static_cast<std::size_t>(y) * img.w + static_cast<std::size_t>(x)) * 3u];
            if (!allFinite(px[0], px[1], px[2])) {
                continue;
            }
            const double cx = x + 0.5;
            const double cy = y + 0.5;
            win.x.push_back(cx);
            win.y.push_back(cy);
            win.rgb.push_back(px[0]);
            win.rgb.push_back(px[1]);
            win.rgb.push_back(px[2]);
            const double u = (cx - seed.x) / half;
            const double v = (cy - seed.y) / half;
            win.basis.insert(win.basis.end(), {1.0, u, v, u * u, u * v, v * v});
        }
    }
    if (win.size() < 64) {
        return f;
    }
    // Start and bounds.
    ShapeParams sp;
    sp.p[kPx] = seed.x;
    sp.p[kPy] = seed.y;
    sp.p[kPhx] = seed.hx;
    sp.p[kPhy] = seed.hy;
    sp.p[kPrho] = 0.7;
    sp.p[kPang] = seed.angle;
    sp.p[kPsoft] = 1.0;
    Bounds bd{};
    bd.lo[kPx] = seed.x - 0.5 * half;
    bd.hi[kPx] = seed.x + 0.5 * half;
    bd.lo[kPy] = seed.y - 0.5 * half;
    bd.hi[kPy] = seed.y + 0.5 * half;
    bd.lo[kPhx] = bd.lo[kPhy] = 1.0;
    bd.hi[kPhx] = bd.hi[kPhy] = half;
    bd.lo[kPrho] = 0.0;
    bd.hi[kPrho] = 1.0;
    bd.lo[kPang] = -0.5 * kPi;
    bd.hi[kPang] = 0.5 * kPi;
    bd.lo[kPsoft] = 0.3;
    bd.hi[kPsoft] = std::max(0.5, fp.maxSoftPx);
    // Stage 1: the geometry with the flat plateau alone.  Fitted jointly
    // from the seed, the rim and tilt terms let the rectangle stretch to
    // wherever a rim fits best (on the sample it pushed the brightest ghost
    // past the aspect gate on two frames of three).
    Eval geomFit;
    const ShapeParams flat = fitShape(win, sp, bd, std::max(1, fp.maxIterations), Model::Plateau, geomFit);
    // A flat fit that already sits on its softness or size bound is not a
    // ghost, and the refinement below cannot move it far enough to become
    // one: stop here rather than spend most of the frame's analysis time on
    // it (one such seed on the sample cost 100 ms, every real ghost ~10).
    const double sf = static_cast<double>(img.factor);
    if (flat.p[kPsoft] >= 0.95 * bd.hi[kPsoft] || std::max(flat.p[kPhx], flat.p[kPhy]) >= 0.95 * half) {
        log::debug("flare: candidate at ({:.0f}, {:.0f}) rejected: flat fit on its bounds", seed.x * sf, seed.y * sf);
        return f;
    }
    // Stage 2: refine with every term, anchored within a quarter of the
    // flat fit's size, so the rim can move the edge to where it belongs
    // without the shape wandering off.
    Bounds near = bd;
    const double span = 0.25 * std::max(flat.p[kPhx], flat.p[kPhy]);
    near.lo[kPx] = std::max(bd.lo[kPx], flat.p[kPx] - span);
    near.hi[kPx] = std::min(bd.hi[kPx], flat.p[kPx] + span);
    near.lo[kPy] = std::max(bd.lo[kPy], flat.p[kPy] - span);
    near.hi[kPy] = std::min(bd.hi[kPy], flat.p[kPy] + span);
    near.lo[kPhx] = std::max(bd.lo[kPhx], 0.75 * flat.p[kPhx]);
    near.hi[kPhx] = std::min(bd.hi[kPhx], 1.25 * flat.p[kPhx]);
    near.lo[kPhy] = std::max(bd.lo[kPhy], 0.75 * flat.p[kPhy]);
    near.hi[kPhy] = std::min(bd.hi[kPhy], 1.25 * flat.p[kPhy]);
    Eval fullFit;
    const ShapeParams fit = std::isfinite(geomFit.ssr)
                                ? fitShape(win, flat, near, std::max(1, fp.maxIterations / 2), Model::Full, fullFit)
                                : flat;
    // Every refusal says why, in stream pixels, for whoever tunes the gates.
    auto reject = [&](const char* why, double value) {
        log::debug("flare: candidate at ({:.0f}, {:.0f}) rejected: {} ({:.3f})", seed.x * sf, seed.y * sf, why, value);
        return f;
    };
    if (!std::isfinite(geomFit.ssr)) {
        return reject("the fit diverged", geomFit.ssr);
    }
    // ---- gate 1: a real, bright, well-bounded shape -----------------------
    const double hx = fit.p[kPhx];
    const double hy = fit.p[kPhy];
    if (!(std::min(hx, hy) >= 1.5)) {
        return reject("too small", std::min(hx, hy));
    }
    if (std::max(hx, hy) / std::min(hx, hy) > fp.maxAspect) {
        return reject("too elongated", std::max(hx, hy) / std::min(hx, hy));
    }
    // A fit that ran into its box did not find a shape, it found the edge
    // of what it was allowed to try.
    if (fit.p[kPsoft] >= 0.95 * bd.hi[kPsoft]) {
        return reject("edge softness at its bound", fit.p[kPsoft]);
    }
    if (std::max(hx, hy) >= 0.95 * half) {
        return reject("size at its bound", std::max(hx, hy));
    }
    // ---- the appearance at that geometry: plateau, rim and tilt -----------
    Eval best;
    if (!evaluate(win, fit, Model::Full, best)) {
        return reject("the appearance solve failed", 0.0);
    }
    // Background luma under the ghost centre from the fitted quadratic.
    const double u = (fit.p[kPx] - seed.x) / half;
    const double v = (fit.p[kPy] - seed.y) / half;
    const double basis[6] = {1.0, u, v, u * u, u * v, v * v};
    double bg[3] = {};
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 6; ++k) {
            bg[c] += best.coef[c][k] * basis[k];
        }
    }
    const double bgY = lumaOf(bg);
    // Contrast: the mean light the ghost adds over its plateau (every term,
    // clamped non-negative as the kernel does) against the background.
    double lightSum = 0.0;
    std::size_t lightN = 0;
    for (std::size_t i = 0; i < win.size(); ++i) {
        const double* t = &best.terms[i * kNumGhostTerms];
        if (t[0] < 0.5) {
            continue;
        }
        double light[3];
        for (int c = 0; c < 3; ++c) {
            double l = 0.0;
            for (int k = 0; k < kNumGhostTerms; ++k) {
                l += best.coef[c][kNumBackground + k] * t[k];
            }
            light[c] = std::max(l, 0.0);
        }
        lightSum += lumaOf(light);
        ++lightN;
    }
    const double ampY = lightN > 0 ? lightSum / static_cast<double>(lightN) : 0.0;
    if (!(ampY > 0.0) || !(bgY > 0.0)) {
        return reject("not brighter than its background", ampY);
    }
    const double contrast = ampY / bgY;
    if (contrast < fp.minContrast) {
        return reject("contrast too low", contrast);
    }
    // ---- gate 2: the ghost term explains its own footprint -----------------
    // Compare, over the ghost plus a margin, the residual of the full model
    // with that of a background-only fit of the same window.
    Eval bgOnly;
    if (!evaluate(win, fit, Model::Background, bgOnly)) {
        return reject("background fit failed", 0.0);
    }
    // The rim bump reaches 3 soft past the edge.
    const double margin = 3.0 * fit.p[kPsoft] + 2.0;
    double ssrFull = 0.0;
    double ssrBg = 0.0;
    const ShapeGeom fitGeom(fit);
    for (std::size_t i = 0; i < win.size(); ++i) {
        if (shapeDistance(fitGeom, win.x[i], win.y[i]) > margin) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const std::size_t k = i * 3 + static_cast<std::size_t>(c);
            ssrFull += best.resid[k] * best.resid[k];
            ssrBg += bgOnly.resid[k] * bgOnly.resid[k];
        }
    }
    const double r2 = ssrBg > 0.0 ? 1.0 - ssrFull / ssrBg : 0.0;
    if (r2 < fp.minFitR2) {
        return reject("the shape explains too little of its footprint", r2);
    }
    // Accepted.  Light is additive: a channel that fitted slightly negative
    // (noise on a coloured ghost) contributes nothing rather than adding.
    // The rim and the tilts keep their sign: they shape the light, and the
    // kernel clamps the per-pixel total at zero.
    f.ok = true;
    f.sp = fit;
    for (int c = 0; c < 3; ++c) {
        f.amp[c] = std::max(best.coef[c][kNumBackground], 0.0);
        f.rim[c] = best.coef[c][kNumBackground + 1];
        f.gradX[c] = best.coef[c][kNumBackground + 2];
        f.gradY[c] = best.coef[c][kNumBackground + 3];
    }
    f.contrast = contrast;
    f.r2 = r2;
    // Visibility: contrast times the linear size of the ghost.
    f.visibility = contrast * std::sqrt(hx * hy);
    return f;
}

/// True when ghost a's centre lies inside ghost b's footprint or vice versa -
/// two fits of the same reflection.
bool overlapping(const Fitted& a, const Fitted& b) {
    return shapeDistance(b.sp, a.sp.p[kPx], a.sp.p[kPy]) < 0.0 || shapeDistance(a.sp, b.sp.p[kPx], b.sp.p[kPy]) < 0.0;
}

// ---------------------------------------------------------------------------
//  Device sampler slot (see FlareDeviceSampler)
// ---------------------------------------------------------------------------

/// Holder for the process-wide device sampler.  Intentionally leaked for the
/// same reason as SeamAnalysis.cpp's shader slot: the sampler belongs to the
/// CUDA library and must not be destroyed from this library's static
/// teardown in an order nobody controls.
struct SamplerSlot {
    std::mutex mutex;
    std::shared_ptr<FlareDeviceSampler> sampler;
};

SamplerSlot& samplerSlot() {
    static SamplerSlot* slot = new SamplerSlot();  // intentionally leaked, see above
    return *slot;
}

/// Kernel-ready float copy of one ghost (stream px), or false when any value
/// is unusable.
bool toKernelGhost(const FlareGhost& g, OsvFlareGhost& k) noexcept {
    if (!allFinite(g.cx, g.cy, g.hx, g.hy, g.radius, g.angleRad, g.soft, g.amp[0], g.amp[1], g.amp[2], g.rim[0],
                   g.rim[1], g.rim[2], g.gradX[0], g.gradX[1], g.gradX[2], g.gradY[0], g.gradY[1], g.gradY[2])) {
        return false;
    }
    if (!(g.hx > 0.0) || !(g.hy > 0.0) || !(g.soft > 0.0)) {
        return false;
    }
    k.cx = static_cast<float>(g.cx);
    k.cy = static_cast<float>(g.cy);
    k.hx = static_cast<float>(g.hx);
    k.hy = static_cast<float>(g.hy);
    k.radius = static_cast<float>(std::clamp(g.radius, 0.0, std::min(g.hx, g.hy)));
    k.cosA = static_cast<float>(std::cos(g.angleRad));
    k.sinA = static_cast<float>(std::sin(g.angleRad));
    k.soft = static_cast<float>(g.soft);
    for (int c = 0; c < 3; ++c) {
        const std::size_t ci = static_cast<std::size_t>(c);
        k.amp[c] = static_cast<float>(std::max(g.amp[ci], 0.0));
        k.rim[c] = static_cast<float>(g.rim[ci]);
        k.gradX[c] = static_cast<float>(g.gradX[ci]);
        k.gradY[c] = static_cast<float>(g.gradY[ci]);
    }
    // Reach squared, with a pixel of slack so float rounding in the kernel
    // can never cut the ramp's last sliver.
    const double reach = g.reach() + 1.0;
    k.reach2 = static_cast<float>(reach * reach);
    return true;
}

/// Luma of the light of all ghosts of `lens` at stream pixel (px, py) - the
/// kernel's osvFlareGhostLight in double precision.
double ghostLumaAt(const LensFlare& lens, double px, double py) noexcept {
    double sum = 0.0;
    for (const FlareGhost& g : lens.ghosts) {
        const double dx = px - g.cx;
        const double dy = py - g.cy;
        const double reach = g.reach();
        if (dx * dx + dy * dy > reach * reach) {
            continue;
        }
        ShapeParams sp;
        sp.p[kPx] = g.cx;
        sp.p[kPy] = g.cy;
        sp.p[kPhx] = g.hx;
        sp.p[kPhy] = g.hy;
        sp.p[kPrho] = std::min(g.hx, g.hy) > 0.0 ? g.radius / std::min(g.hx, g.hy) : 0.0;
        sp.p[kPang] = g.angleRad;
        sp.p[kPsoft] = g.soft;
        double terms[kNumGhostTerms];
        ghostTerms(ShapeGeom(sp), px, py, terms);
        double light[3];
        for (std::size_t c = 0; c < 3; ++c) {
            light[c] = std::max(g.amp[c] * terms[0] + g.rim[c] * terms[1] + g.gradX[c] * terms[2] +
                                    g.gradY[c] * terms[3],
                                0.0);
        }
        sum += lumaOf(light);
    }
    return sum;
}

/// A working image reduced to what the sun detector and the ghost search
/// share: luma, the usable image circle, and the sun blob.
struct SunScan {
    Gray luma;
    std::vector<std::uint8_t> inside;  ///< 1 inside 97 % of the image circle.
    double ocx = 0.0, ocy = 0.0;       ///< Optical centre (analysis px).
    double rMax = 0.0;                 ///< Image-circle radius (analysis px).
    SunBlob sun;                       ///< The sun, when there is one.
};

/// Luma, image circle and sun of one working image.  The caller has
/// validated the image and the lens.
SunScan scanForSun(const FlareImage& image, const geom::KannalaBrandt5& lens, const FlareParams& fp) {
    SunScan s;
    const double f = static_cast<double>(image.factor);
    const std::uint32_t W = image.w;
    const std::uint32_t H = image.h;
    // ---- luma -----------------------------------------------------------------
    s.luma = Gray(W, H);
    for (std::size_t i = 0; i < s.luma.v.size(); ++i) {
        const float* p = &image.rgb[i * 3u];
        const double y = kLumaR * p[0] + kLumaG * p[1] + kLumaB * p[2];
        s.luma.v[i] = std::isfinite(y) ? static_cast<float>(y) : 0.0f;
    }
    // ---- the usable image circle ------------------------------------------------
    s.ocx = lens.cx / f;
    s.ocy = lens.cy / f;
    double rMax = lens.rMaxPx;
    if (!(rMax > 0.0)) {
        rMax = 0.5 * (lens.fx + lens.fy) * lens.thetaD(lens.thetaMaxRad);
    }
    s.rMax = rMax / f;
    s.inside.assign(s.luma.v.size(), 0);
    const double r2Max = (0.97 * s.rMax) * (0.97 * s.rMax);
    for (std::uint32_t y = 0; y < H; ++y) {
        const double dy = y + 0.5 - s.ocy;
        for (std::uint32_t x = 0; x < W; ++x) {
            const double dx = x + 0.5 - s.ocx;
            s.inside[static_cast<std::size_t>(y) * W + x] = dx * dx + dy * dy < r2Max ? 1 : 0;
        }
    }
    // ---- the sun ------------------------------------------------------------------
    s.sun = detectSun(s.luma, s.inside, fp);
    return s;
}

/// The parameter checks analyseLensFlare and locateSun share.
[[nodiscard]] bool flareParamsValid(const FlareParams& fp) noexcept {
    return allFinite(fp.sunLevelFraction, fp.sunMinRatioToMedian, fp.corridorDeg, fp.backgroundSigmaPx,
                     fp.seedContrast, fp.maxTexture, fp.minContrast, fp.minFitR2) &&
           fp.sunLevelFraction > 0.0 && fp.sunLevelFraction <= 1.0 && fp.backgroundSigmaPx > 0.5 &&
           fp.maxGhosts >= 0 && fp.corridorDeg >= 0.0;
}

/// Scale every light term of a ghost (fade in / out) - its shape stays.
void scaleLight(FlareGhost& g, double k) noexcept {
    for (std::size_t c = 0; c < 3; ++c) {
        g.amp[c] *= k;
        g.rim[c] *= k;
        g.gradX[c] *= k;
        g.gradY[c] *= k;
    }
}

}  // namespace

// ===========================================================================
//  Public: small helpers
// ===========================================================================

double FlareGhost::reach() const noexcept {
    // The farthest point of the rectangle plus the rim bump's outer half
    // (3 soft; the plateau ramp ends at 1 soft).
    return std::sqrt(hx * hx + hy * hy) + 3.0 * std::max(soft, 0.0);
}

bool FlareModel::any() const noexcept {
    for (const LensFlare& l : lens) {
        if (!l.ghosts.empty() || l.veil[0] > 0.0 || l.veil[1] > 0.0 || l.veil[2] > 0.0) {
            return true;
        }
    }
    return false;
}

void setFlareDeviceSampler(std::shared_ptr<FlareDeviceSampler> sampler) {
    SamplerSlot& slot = samplerSlot();
    std::lock_guard<std::mutex> lock(slot.mutex);
    slot.sampler = std::move(sampler);
}

std::shared_ptr<FlareDeviceSampler> flareDeviceSampler() {
    SamplerSlot& slot = samplerSlot();
    std::lock_guard<std::mutex> lock(slot.mutex);
    return slot.sampler;
}

// ===========================================================================
//  Public: downsample
// ===========================================================================

Result<FlareImage> flareDownsample(const OsvPlane& hostPlane, const OsvColorParams& color, std::uint32_t factor,
                                   ThreadPool& pool) {
    // ---- validate everything the per-pixel function will index -----------
    if (factor < 1 || factor > kMaxFactor) {
        return Error{ErrorCode::InvalidArgument, "flareDownsample: factor must be 1..16"};
    }
    if (!hostPlane.y || !hostPlane.u || !hostPlane.v || hostPlane.w <= 0 || hostPlane.h <= 0 || hostPlane.cw <= 0 ||
        hostPlane.ch <= 0 || hostPlane.strideY < hostPlane.w || hostPlane.bitShift < 0 || hostPlane.bitShift > 15) {
        return Error{ErrorCode::InvalidArgument, "flareDownsample: malformed plane"};
    }
    const int chromaStep = hostPlane.chromaInterleaved ? 2 : 1;
    if (hostPlane.strideC < hostPlane.cw * chromaStep) {
        return Error{ErrorCode::InvalidArgument, "flareDownsample: chroma stride shorter than a row"};
    }
    FlareImage img;
    img.factor = factor;
    img.w = flareAnalysisSize(static_cast<std::uint32_t>(hostPlane.w), factor);
    img.h = flareAnalysisSize(static_cast<std::uint32_t>(hostPlane.h), factor);
    try {
        img.rgb.assign(static_cast<std::size_t>(img.w) * img.h * 3u, 0.0f);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "flareDownsample: out of memory"};
    }
    // ---- one analysis row per task, through the shared kernel function ----
    const OsvPlane plane = hostPlane;
    const OsvColorParams cp = color;
    const int f = static_cast<int>(factor);
    float* out = img.rgb.data();
    const std::uint32_t w = img.w;
    OSV_TRY(pool.parallelRows(img.h, 4, [&](std::size_t row) {
        for (std::uint32_t x = 0; x < w; ++x) {
            osvFlareDownsamplePixel(&plane, &cp, f, static_cast<int>(x), static_cast<int>(row),
                                    out + (row * w + x) * 3u);
        }
    }));
    return img;
}

Result<FlareImage> flareDownsample(const video::PlanarFrame16& frame, const OsvColorParams& color,
                                   std::uint32_t factor, ThreadPool& pool) {
    OsvPlane plane{};
    if (!fillPlane(frame, plane)) {
        return Error{ErrorCode::InvalidArgument, "flareDownsample: invalid frame"};
    }
    return flareDownsample(plane, color, factor, pool);
}

// ===========================================================================
//  Public: analysis
// ===========================================================================

Result<LensFlare> analyseLensFlare(const FlareImage& image, const geom::KannalaBrandt5& lens,
                                   const FlareParams& fp, ThreadPool* pool) {
    // ---- validate -----------------------------------------------------------
    if (!image.valid()) {
        return Error{ErrorCode::InvalidArgument, "analyseLensFlare: invalid working image"};
    }
    if (!lens.isValid()) {
        return Error{ErrorCode::InvalidArgument, "analyseLensFlare: invalid lens model"};
    }
    if (!flareParamsValid(fp)) {
        return Error{ErrorCode::InvalidArgument, "analyseLensFlare: invalid parameters"};
    }
    LensFlare out;
    const auto tStart = std::chrono::steady_clock::now();
    const double f = static_cast<double>(image.factor);
    const std::uint32_t W = image.w;
    const std::uint32_t H = image.h;

    // ---- luma, the usable image circle and the sun ---------------------------
    const SunScan scan = scanForSun(image, lens, fp);
    const Gray& luma = scan.luma;
    const double ocx = scan.ocx;
    const double ocy = scan.ocy;
    const double rMax = scan.rMax;
    const SunBlob& sun = scan.sun;
    if (!sun.found) {
        return out;  // no sun, nothing to remove - not an error
    }
    out.sunFound = true;
    out.sunX = sun.x * f;
    out.sunY = sun.y * f;
    out.sunRadiusPx = sun.radius * f;
    if (auto dir = lens.unproject(Vec2d{out.sunX, out.sunY}); dir.ok()) {
        out.sunThetaRad = std::acos(std::clamp(dir.value().z, -1.0, 1.0));
    }
    if (fp.maxGhosts == 0) {
        return out;
    }

    // ---- relative band-pass and local texture --------------------------------
    const Gray bg = gaussBoxes(luma, fp.backgroundSigmaPx, pool);
    const Gray fine = gaussExact(luma, 1.0, pool);
    Gray rel(W, H);
    for (std::size_t i = 0; i < rel.v.size(); ++i) {
        rel.v[i] = static_cast<float>((fine.v[i] - bg.v[i]) / std::max(static_cast<double>(bg.v[i]), 1e-6));
    }
    // Texture: mean absolute detail below sigma 2, relative to the
    // background, averaged over sigma 4.  Sky is ~0.005, a city ~0.1.
    Gray detail(W, H);
    {
        const Gray s2 = gaussBoxes(luma, 2.0, pool);
        for (std::size_t i = 0; i < detail.v.size(); ++i) {
            detail.v[i] =
                static_cast<float>(std::fabs(luma.v[i] - s2.v[i]) / std::max(static_cast<double>(bg.v[i]), 1e-6));
        }
    }
    const Gray texture = gaussBoxes(detail, 4.0, pool);

    // ---- candidate mask: bright bumps near the sun line, away from the sun ---
    const double sunAz = std::atan2(sun.y - ocy, sun.x - ocx);
    const double sunR = std::hypot(sun.x - ocx, sun.y - ocy);
    // Too close to the axis for an azimuth to mean anything: search all round.
    const bool anyAzimuth = sunR < 2.0 * sun.radius;
    const double corridor = fp.corridorDeg * kPi / 180.0;
    const double exclusion = fp.sunExclusionRadii * sun.radius;
    std::vector<std::uint8_t> mask(luma.v.size(), 0);
    for (std::uint32_t y = 0; y < H; ++y) {
        for (std::uint32_t x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * W + x;
            if (rel.v[i] <= fp.seedContrast) {
                continue;
            }
            const double px = x + 0.5;
            const double py = y + 0.5;
            if (std::hypot(px - ocx, py - ocy) >= fp.rimFraction * rMax) {
                continue;
            }
            if (std::hypot(px - sun.x, py - sun.y) <= exclusion) {
                continue;
            }
            if (!anyAzimuth) {
                // Angle to the sun line, folded so both the sun's side and
                // the mirrored side of the axis count.
                double d = std::atan2(py - ocy, px - ocx) - sunAz;
                d = std::remainder(d, 2.0 * kPi);
                const double off = std::min(std::fabs(d), kPi - std::fabs(d));
                if (off > corridor) {
                    continue;
                }
            }
            mask[i] = 1;
        }
    }
    const std::vector<Blob> blobs = findBlobs(mask, W, H, &rel);

    // ---- screen the blobs ----------------------------------------------------
    std::vector<Seed> seeds;
    for (const Blob& b : blobs) {
        if (b.area < 6 || static_cast<double>(b.area) > fp.maxCandidateAreaPx) {
            continue;
        }
        if (b.aspect() > fp.maxAspect || b.fill() < 0.35) {
            continue;
        }
        if (static_cast<double>(std::max(b.bw(), b.bh())) * f > fp.maxCandidateExtentStreamPx) {
            continue;
        }
        // Texture around the blob: its box grown by its own size each way.
        const std::uint32_t gx0 = b.x0 >= b.bw() ? b.x0 - b.bw() : 0u;
        const std::uint32_t gy0 = b.y0 >= b.bh() ? b.y0 - b.bh() : 0u;
        const std::uint32_t gx1 = std::min(W - 1, b.x1 + b.bw());
        const std::uint32_t gy1 = std::min(H - 1, b.y1 + b.bh());
        std::vector<float> tv;
        tv.reserve(static_cast<std::size_t>(gx1 - gx0 + 1) * (gy1 - gy0 + 1));
        for (std::uint32_t y = gy0; y <= gy1; ++y) {
            for (std::uint32_t x = gx0; x <= gx1; ++x) {
                tv.push_back(texture.at(x, y));
            }
        }
        auto mid = tv.begin() + static_cast<std::ptrdiff_t>(tv.size() / 2);
        std::nth_element(tv.begin(), mid, tv.end());
        const double tex = static_cast<double>(*mid);
        // Ghosts sit on smooth sky; on texture a bump is far more likely to
        // be the scene itself, and the scene must never be subtracted.
        if (tex > fp.maxTexture || b.peak < fp.minPeakOverTexture * tex) {
            continue;
        }
        seeds.push_back(seedFromBlob(b));
    }
    out.candidates = static_cast<std::uint32_t>(seeds.size());

    const auto tSeeds = std::chrono::steady_clock::now();

    // ---- fit every seed ------------------------------------------------------
    // Each fit is independent (its own window, its own solve), so with a pool
    // they run side by side; without one, in order.  Either way the result
    // is the same: every fit writes only its own slot.
    std::vector<Fitted> fits(seeds.size());
    if (pool && seeds.size() > 1) {
        OSV_TRY(pool->parallelFor(0, seeds.size(), 1, [&](std::size_t b, std::size_t e) {
            for (std::size_t i = b; i < e; ++i) {
                fits[i] = fitSeed(image, seeds[i], fp);
            }
        }));
    } else {
        for (std::size_t i = 0; i < seeds.size(); ++i) {
            fits[i] = fitSeed(image, seeds[i], fp);
        }
    }

    const auto tFits = std::chrono::steady_clock::now();
    log::debug("flare: {} seeds; detection {:.1f} ms, fits {:.1f} ms", seeds.size(),
               std::chrono::duration<double, std::milli>(tSeeds - tStart).count(),
               std::chrono::duration<double, std::milli>(tFits - tSeeds).count());

    // ---- keep the most visible, one fit per reflection ------------------------
    std::vector<Fitted> accepted;
    for (const Fitted& ft : fits) {
        if (ft.ok) {
            accepted.push_back(ft);
        }
    }
    std::sort(accepted.begin(), accepted.end(),
              [](const Fitted& a, const Fitted& b) { return a.visibility > b.visibility; });
    std::vector<Fitted> kept;
    for (const Fitted& ft : accepted) {
        const bool dup = std::any_of(kept.begin(), kept.end(), [&](const Fitted& k) { return overlapping(ft, k); });
        if (dup) {
            continue;
        }
        if (static_cast<int>(kept.size()) >= std::min(fp.maxGhosts, kFlareMaxGhosts)) {
            break;
        }
        kept.push_back(ft);
    }
    out.rejected = out.candidates - static_cast<std::uint32_t>(kept.size());

    // ---- convert to stream pixels --------------------------------------------
    for (const Fitted& ft : kept) {
        FlareGhost g;
        g.cx = ft.sp.p[kPx] * f;
        g.cy = ft.sp.p[kPy] * f;
        g.hx = ft.sp.p[kPhx] * f;
        g.hy = ft.sp.p[kPhy] * f;
        g.radius = std::clamp(ft.sp.p[kPrho], 0.0, 1.0) * std::min(g.hx, g.hy);
        g.angleRad = ft.sp.p[kPang];
        g.soft = ft.sp.p[kPsoft] * f;
        for (int c = 0; c < 3; ++c) {
            const std::size_t ci = static_cast<std::size_t>(c);
            g.amp[ci] = ft.amp[c];
            g.rim[ci] = ft.rim[c];
            g.gradX[ci] = ft.gradX[c];
            g.gradY[ci] = ft.gradY[c];
        }
        g.contrast = ft.contrast;
        g.fitR2 = ft.r2;
        out.ghosts.push_back(g);
    }
    return out;
}

Result<FlareModel> analyseFlare(const geom::LensRig& rig, const video::FramePair& frames, const OsvColorParams& color,
                                const FlareParams& params, ThreadPool& pool) {
    FlareModel model;
    for (int i = 0; i < 2; ++i) {
        const std::size_t li = static_cast<std::size_t>(i);
        OSV_TRY_ASSIGN(FlareImage image, flareDownsampleLens(frames, i, color, params.factor, pool));
        OSV_TRY_ASSIGN(model.lens[li], analyseLensFlare(image, rig.lens[li], params, &pool));
    }
    return model;
}

Result<FlareImage> flareDownsampleLens(const video::FramePair& frames, int lens, const OsvColorParams& color,
                                       std::uint32_t factor, ThreadPool& pool) {
    if (lens < 0 || lens > 1) {
        return Error{ErrorCode::InvalidArgument, "flareDownsampleLens: lens must be 0 or 1"};
    }
    const std::size_t li = static_cast<std::size_t>(lens);
    // Host frame first: it is what the CPU reference path has.
    if (frames.lens[li].valid()) {
        return flareDownsample(frames.lens[li], color, factor, pool);
    }
    if (!frames.device[li].valid()) {
        return Error{ErrorCode::InvalidArgument, "flareDownsampleLens: the pair holds no frame for this lens"};
    }
    // Device-only frame: the installed GPU sampler, or a refusal that says
    // what is missing.
    const std::shared_ptr<FlareDeviceSampler> gpu = flareDeviceSampler();
    if (!gpu) {
        return Error{ErrorCode::InvalidArgument,
                     "flare analysis: the frames are on the GPU and no device sampler is installed "
                     "(osv::render::installCudaFlareSampler)"};
    }
    OsvPlane plane{};
    if (!fillDevicePlane(frames.device[li], plane)) {
        return Error{ErrorCode::InvalidArgument, "flareDownsampleLens: invalid device frame"};
    }
    return gpu->downsample(plane, color, factor);
}

// ===========================================================================
//  Public: the per-frame sun check
// ===========================================================================

std::uint32_t flareSunCheckFactor(std::uint32_t lensW) noexcept {
    // 375-750 analysis px across: 8 at 6K (3000 px lenses, 375 px), 2 for
    // the 1024 px proxy (512 px).  Fine enough that the sun disc (39 px
    // radius at 6K, 13 at the proxy) is dozens of samples and its centroid
    // lands within a pixel or two, coarse enough to cost a sixteenth of the
    // full analysis' decodes at 6K.
    const std::uint32_t f = lensW / 375u;
    return std::clamp<std::uint32_t>(f, 1u, 16u);
}

double flareSunTolerancePx(std::uint32_t lensW) noexcept {
    // 3 px at 6K, scaled with the lens (0.2 degrees at every size).  On the
    // sample clip the brightest ghost moved 0.45 px per px of sun motion
    // across the image (sun +16 px, ghost +7 px over 65 frames), so a model
    // is at most ~1.4 px off within this tolerance - under a quarter of its
    // 6.6 px edge ramp, invisible.  A sun further away than this needs its
    // own measurement.
    const double w = lensW > 0 ? static_cast<double>(lensW) : 3000.0;
    return std::max(1.0, 3.0 * w / 3000.0);
}

FlareSunFix locateSun(const FlareImage& image, const geom::KannalaBrandt5& lens, const FlareParams& params) noexcept {
    FlareSunFix fix;
    try {
        if (!image.valid() || !lens.isValid() || !flareParamsValid(params)) {
            return fix;
        }
        const SunScan scan = scanForSun(image, lens, params);
        if (!scan.sun.found) {
            return fix;
        }
        const double f = static_cast<double>(image.factor);
        fix.found = true;
        fix.x = scan.sun.x * f;
        fix.y = scan.sun.y * f;
        fix.radiusPx = scan.sun.radius * f;
    } catch (...) {
        // Allocation failure: report no sun, which removes nothing.
        fix = FlareSunFix{};
    }
    return fix;
}

Result<FlareSunFixes> locateSuns(const geom::LensRig& rig, const video::FramePair& frames, const OsvColorParams& color,
                                 const FlareParams& params, ThreadPool& pool) {
    FlareSunFixes fixes{};
    for (int i = 0; i < 2; ++i) {
        const std::size_t li = static_cast<std::size_t>(i);
        // The lens's width decides the check's resolution.
        const std::uint32_t lensW = frames.lens[li].valid() ? frames.lens[li].width : frames.device[li].width;
        OSV_TRY_ASSIGN(FlareImage image,
                       flareDownsampleLens(frames, i, color, flareSunCheckFactor(lensW), pool));
        fixes[li] = locateSun(image, rig.lens[li], params);
    }
    return fixes;
}

bool flareSunsMatch(const FlareSunFixes& a, const FlareSunFixes& b, double tolerancePx) noexcept {
    if (!std::isfinite(tolerancePx) || tolerancePx < 0.0) {
        return false;
    }
    for (std::size_t i = 0; i < 2; ++i) {
        if (a[i].found != b[i].found) {
            return false;  // the sun entered or left this lens
        }
        if (a[i].found && !(std::hypot(a[i].x - b[i].x, a[i].y - b[i].y) <= tolerancePx)) {
            return false;  // it moved too far (or a position is not a number)
        }
    }
    return true;
}

// ===========================================================================
//  Public: veil
// ===========================================================================

Result<std::array<std::array<double, 3>, 2>> estimateVeil(const LensBands& bands, const FlareModel& model) {
    std::array<std::array<double, 3>, 2> veil{};
    // ---- validate -----------------------------------------------------------
    const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
    if (bands.w < 64 || bands.h == 0) {
        return Error{ErrorCode::InvalidArgument, "estimateVeil: empty bands"};
    }
    for (int i = 0; i < 2; ++i) {
        if (bands.luma[i].size() != n || bands.alpha[i].size() != n) {
            return Error{ErrorCode::InvalidArgument, "estimateVeil: band sizes do not match"};
        }
    }
    // Attribute the veil to the one lens that sees the sun.  With the sun in
    // neither (no source) or both (it sits on the seam, attribution is
    // ambiguous) the honest answer is no estimate.
    const bool s0 = model.lens[0].sunFound;
    const bool s1 = model.lens[1].sunFound;
    if (s0 == s1) {
        return veil;
    }
    const int veiled = s1 ? 1 : 0;
    const int clean = 1 - veiled;

    // ---- rows both lenses see at full strength ---------------------------------
    // In the polar-axis band a row's latitude is 90 deg minus the master's
    // ray angle and the slave's minus 90 deg.  Beyond ~92.8 deg from its
    // axis a lens's rim darkens (a GAIN field, WP-PHOTO's), and letting those
    // rows in would read a vignetting ratio as an additive veil - so only
    // rows within kVeilMaxLatDeg of the equator are used, where both lenses
    // look at 87.5..92.5 deg.  Unknown geometry (mapH == 0) keeps every row.
    constexpr double kVeilMaxLatDeg = 2.5;
    std::vector<std::uint8_t> rowOk(bands.h, 1);
    if (bands.mapH > 0) {
        for (std::uint32_t r = 0; r < bands.h; ++r) {
            const double lat = 90.0 - (static_cast<double>(bands.rowOffset + r) + 0.5) / bands.mapH * 180.0;
            rowOk[r] = std::fabs(lat) <= kVeilMaxLatDeg ? 1 : 0;
        }
    }

    // ---- column-block means over the co-visible pixels ------------------------
    constexpr std::uint32_t kBlocks = 32;
    const std::uint32_t blockW = std::max<std::uint32_t>(1, bands.w / kBlocks);
    std::vector<double> xs, ys;
    for (std::uint32_t b0 = 0; b0 + blockW <= bands.w; b0 += blockW) {
        double sv = 0.0, sc = 0.0, sc2 = 0.0;
        std::uint32_t cnt = 0;
        for (std::uint32_t r = 0; r < bands.h; ++r) {
            if (!rowOk[r]) {
                continue;
            }
            for (std::uint32_t c = b0; c < b0 + blockW; ++c) {
                const std::size_t i = static_cast<std::size_t>(r) * bands.w + c;
                const double av = bands.alpha[veiled][i];
                const double ac = bands.alpha[clean][i];
                const double lv = bands.luma[veiled][i];
                const double lc = bands.luma[clean][i];
                if (!(av > 0.99) || !(ac > 0.99) || !allFinite(lv, lc)) {
                    continue;
                }
                sv += lv;
                sc += lc;
                sc2 += lc * lc;
                ++cnt;
            }
        }
        if (cnt < 64) {
            continue;
        }
        const double mv = sv / cnt;
        const double mc = sc / cnt;
        const double sd = std::sqrt(std::max(sc2 / cnt - mc * mc, 0.0));
        // Smooth blocks only: on texture a residual misregistration between
        // the lenses mixes different scene points into the means.
        if (!(mc > 1e-4) || sd / mc > 0.35) {
            continue;
        }
        xs.push_back(mc);
        ys.push_back(mv);
    }
    if (xs.size() < 6) {
        return veil;
    }
    // ---- fit y = g x + v, trimming the worst fifth once ------------------------
    auto fitLine = [](const std::vector<double>& x, const std::vector<double>& y, const std::vector<std::uint8_t>& use,
                      double& g, double& v) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        double k = 0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (!use[i]) {
                continue;
            }
            sx += x[i];
            sy += y[i];
            sxx += x[i] * x[i];
            sxy += x[i] * y[i];
            k += 1.0;
        }
        const double den = k * sxx - sx * sx;
        if (k < 3.0 || !(std::fabs(den) > 1e-18)) {
            return false;
        }
        g = (k * sxy - sx * sy) / den;
        v = (sy - g * sx) / k;
        return true;
    };
    std::vector<std::uint8_t> use(xs.size(), 1);
    double g = 1.0;
    double v = 0.0;
    const auto [minIt, maxIt] = std::minmax_element(xs.begin(), xs.end());
    const bool spread = *maxIt >= 1.3 * *minIt;
    if (spread && fitLine(xs, ys, use, g, v)) {
        std::vector<std::pair<double, std::size_t>> res;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            res.emplace_back(std::fabs(ys[i] - (g * xs[i] + v)), i);
        }
        std::sort(res.rbegin(), res.rend());
        for (std::size_t i = 0; i < res.size() / 5; ++i) {
            use[res[i].second] = 0;
        }
        if (!fitLine(xs, ys, use, g, v)) {
            return veil;
        }
    } else {
        // Too little brightness spread to separate gain from offset: assume
        // matched exposure and take the median offset.
        std::vector<double> d;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            d.push_back(ys[i] - xs[i]);
        }
        std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
        g = 1.0;
        v = d[d.size() / 2];
    }
    // ---- plausibility: a veil adds light, and never most of it ---------------
    std::vector<double> ysorted = ys;
    std::nth_element(ysorted.begin(), ysorted.begin() + static_cast<std::ptrdiff_t>(ysorted.size() / 2),
                     ysorted.end());
    const double medY = ysorted[ysorted.size() / 2];
    if (!allFinite(g, v) || g < 0.7 || g > 1.4 || !(v > 0.0) || v > 0.5 * medY) {
        return veil;
    }
    veil[static_cast<std::size_t>(veiled)] = {v, v, v};
    return veil;
}

// ===========================================================================
//  Public: kernel parameters
// ===========================================================================

void clearFlare(OsvRenderParams& params) noexcept {
    params.flareEnabled = 0;
    std::memset(&params.flare[0], 0, sizeof(params.flare));
}

void applyFlare(const FlareModel& model, OsvRenderParams& params) noexcept {
    clearFlare(params);
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        const LensFlare& lf = model.lens[static_cast<std::size_t>(i)];
        OsvFlareLens& k = params.flare[i];
        // Veil: finite and non-negative, or nothing.
        if (allFinite(lf.veil[0], lf.veil[1], lf.veil[2])) {
            for (int c = 0; c < 3; ++c) {
                k.veil[c] = static_cast<float>(std::max(lf.veil[static_cast<std::size_t>(c)], 0.0));
                any = any || k.veil[c] > 0.0f;
            }
        }
        // Ghosts: the first kFlareMaxGhosts usable ones.
        int n = 0;
        for (const FlareGhost& g : lf.ghosts) {
            if (n >= kFlareMaxGhosts) {
                break;
            }
            if (toKernelGhost(g, k.ghost[n])) {
                ++n;
            }
        }
        k.ghostCount = n;
        any = any || n > 0;
    }
    params.flareEnabled = any ? 1 : 0;
}

FlareModel smoothFlare(const FlareModel& previous, const FlareModel& current, double weight) {
    // weight = 1 (or garbage) means "no memory".
    if (!std::isfinite(weight) || weight >= 1.0) {
        return current;
    }
    const double w = std::max(weight, 0.0);
    FlareModel out = current;
    for (std::size_t i = 0; i < 2; ++i) {
        const LensFlare& prev = previous.lens[i];
        const LensFlare& cur = current.lens[i];
        LensFlare& res = out.lens[i];
        res.ghosts.clear();
        std::vector<std::uint8_t> used(prev.ghosts.size(), 0);
        // Every current ghost: blend with its nearest previous match.
        for (const FlareGhost& g : cur.ghosts) {
            std::size_t best = prev.ghosts.size();
            double bestD = std::numeric_limits<double>::infinity();
            for (std::size_t k = 0; k < prev.ghosts.size(); ++k) {
                if (used[k]) {
                    continue;
                }
                const double d = std::hypot(prev.ghosts[k].cx - g.cx, prev.ghosts[k].cy - g.cy);
                if (d < bestD) {
                    bestD = d;
                    best = k;
                }
            }
            FlareGhost m = g;
            if (best < prev.ghosts.size() && bestD <= std::max(g.hx, g.hy)) {
                used[best] = 1;
                const FlareGhost& p = prev.ghosts[best];
                auto lerp = [w](double a, double b) { return a + (b - a) * w; };
                m.cx = lerp(p.cx, g.cx);
                m.cy = lerp(p.cy, g.cy);
                m.hx = lerp(p.hx, g.hx);
                m.hy = lerp(p.hy, g.hy);
                m.radius = lerp(p.radius, g.radius);
                // Angles are modulo pi: blend along the shorter way round.
                const double da = std::remainder(g.angleRad - p.angleRad, kPi);
                m.angleRad = p.angleRad + da * w;
                m.soft = lerp(p.soft, g.soft);
                for (std::size_t c = 0; c < 3; ++c) {
                    m.amp[c] = lerp(p.amp[c], g.amp[c]);
                    m.rim[c] = lerp(p.rim[c], g.rim[c]);
                    m.gradX[c] = lerp(p.gradX[c], g.gradX[c]);
                    m.gradY[c] = lerp(p.gradY[c], g.gradY[c]);
                }
            } else {
                // New ghost: enters at `w` of its strength.
                scaleLight(m, w);
            }
            res.ghosts.push_back(m);
        }
        // Ghosts the current fit lost fade out instead of vanishing.
        for (std::size_t k = 0; k < prev.ghosts.size(); ++k) {
            if (used[k] || res.ghosts.size() >= static_cast<std::size_t>(kFlareMaxGhosts)) {
                continue;
            }
            FlareGhost m = prev.ghosts[k];
            scaleLight(m, 1.0 - w);
            res.ghosts.push_back(m);
        }
        for (std::size_t c = 0; c < 3; ++c) {
            res.veil[c] = prev.veil[c] + (cur.veil[c] - prev.veil[c]) * w;
        }
    }
    return out;
}

// ===========================================================================
//  Public: seam cost hook
// ===========================================================================

float flareCost(const LensFlare& lens, double px, double py, double signal, const FlareCostParams& cp) noexcept {
    if (!allFinite(px, py)) {
        return 0.0f;
    }
    // Predicted additive flare luma at this pixel.
    double flare = cp.veilWeight * lumaOf(lens.veil.data());
    if (cp.ghostWeight > 0.0 && !lens.ghosts.empty()) {
        flare += cp.ghostWeight * ghostLumaAt(lens, px, py);
    }
    // Glare around the sun disc: 1 on the disc, a smooth fall to 0 at
    // sunGlareRadii radii.  Expressed as a fraction, so it enters the ratio
    // below as "as much flare as signal" at full strength.
    double glareFraction = 0.0;
    if (lens.sunFound && cp.sunGlareWeight > 0.0 && lens.sunRadiusPx > 0.0 && cp.sunGlareRadii > 1.0) {
        const double d = std::hypot(px - lens.sunX, py - lens.sunY) / lens.sunRadiusPx;
        if (d < cp.sunGlareRadii) {
            double t = std::clamp((cp.sunGlareRadii - d) / (cp.sunGlareRadii - 1.0), 0.0, 1.0);
            glareFraction = cp.sunGlareWeight * t * t * (3.0 - 2.0 * t);
        }
    }
    const double s = (std::isfinite(signal) && signal > 0.0) ? signal : std::max(cp.referenceSignal, 1e-6);
    flare = std::max(flare, 0.0);
    // Fraction of the observed light that is flare, combined with the glare
    // fraction as independent contaminations: 1 - (1 - a)(1 - b).
    const double a = flare / (flare + s);
    const double b = std::clamp(glareFraction, 0.0, 1.0);
    const double cost = 1.0 - (1.0 - a) * (1.0 - b);
    if (!std::isfinite(cost)) {
        return 0.0f;
    }
    // Strictly below 1 unless the pixel is saturated sun: keep [0, 1).
    return static_cast<float>(std::clamp(cost, 0.0, 0.999999));
}

namespace {

/// flareCost() over a polar-axis band into `cost` (already sized mapW *
/// rows); `unseen` is written where a lens does not see the band pixel.
/// The geometry is the kernel's OSV_LAYOUT_POLAR_AXIS (and WP-SEAM's
/// bandPixelDirection): lon = (col + 0.5) 2pi / w - pi, lat = pi/2 -
/// (row0 + row + 0.5) pi / mapH, d = (cos lat sin lon, sin lat, cos lat cos lon).
void fillBandCost(const FlareModel& model, const geom::LensRig& rig, std::uint32_t mapW, std::uint32_t mapH,
                  std::uint32_t row0, std::uint32_t rows, const std::array<const std::vector<float>*, 2>* signal,
                  const FlareCostParams& params, float unseen, std::array<float*, 2> cost) noexcept {
    for (std::uint32_t r = 0; r < rows; ++r) {
        const double sy = static_cast<double>(row0 + r) + 0.5;
        const double lat = 0.5 * kPi - (sy / mapH) * kPi;
        const double cl = std::cos(lat);
        for (std::uint32_t x = 0; x < mapW; ++x) {
            const double sx = static_cast<double>(x) + 0.5;
            const double lon = (sx / mapW) * 2.0 * kPi - kPi;
            // OSV_LAYOUT_POLAR_AXIS: poles on the lens axes (+/-Y).
            const Vec3d d{cl * std::sin(lon), std::sin(lat), cl * std::cos(lon)};
            const std::size_t k = static_cast<std::size_t>(r) * mapW + x;
            for (int i = 0; i < 2; ++i) {
                const std::size_t li = static_cast<std::size_t>(i);
                Vec2d px;
                double theta = 0.0;
                if (!rig.projectBody(i, d, px, theta)) {
                    cost[li][k] = unseen;  // not seen by this lens
                    continue;
                }
                double sig = -1.0;
                if (signal) {
                    const std::vector<float>* sv = (*signal)[li];
                    if (sv) {
                        sig = (*sv)[k];
                    }
                }
                cost[li][k] = flareCost(model.lens[li], px.x, px.y, sig, params);
            }
        }
    }
}

}  // namespace

Status flareCostBand(const FlareModel& model, const geom::LensRig& rig, std::uint32_t mapW, std::uint32_t mapH,
                     std::uint32_t row0, std::uint32_t rows, const std::array<const std::vector<float>*, 2>* signal,
                     std::array<std::vector<float>, 2>& cost, const FlareCostParams& params) {
    // ---- validate -----------------------------------------------------------
    if (mapW == 0 || mapH == 0 || mapW > 32768 || mapH > 32768 || rows == 0 || row0 >= mapH || rows > mapH - row0) {
        return failStatus(ErrorCode::InvalidArgument, "flareCostBand: band outside the map");
    }
    const std::size_t n = static_cast<std::size_t>(mapW) * rows;
    if (signal) {
        for (int i = 0; i < 2; ++i) {
            const std::vector<float>* sv = (*signal)[static_cast<std::size_t>(i)];
            if (sv && sv->size() != n) {
                return failStatus(ErrorCode::InvalidArgument, "flareCostBand: signal band has the wrong size");
            }
        }
    }
    try {
        cost[0].assign(n, 1.0f);
        cost[1].assign(n, 1.0f);
    } catch (const std::bad_alloc&) {
        return failStatus(ErrorCode::Internal, "flareCostBand: out of memory");
    }
    // ---- evaluate: a lens that does not see a pixel costs 1 there ----------
    fillBandCost(model, rig, mapW, mapH, row0, rows, signal, params, 1.0f, {cost[0].data(), cost[1].data()});
    return okStatus();
}

// ===========================================================================
//  Public: the WP-SEAM penalty source
// ===========================================================================

FlareCostParams FlareSeamPenalty::seamDefaults() noexcept {
    FlareCostParams p;
    // A uniform veil is removed by subtraction; moving the seam cannot help
    // it, so it must not pull the seam around.
    p.veilWeight = 0.0;
    return p;
}

void FlareSeamPenalty::update(const FlareModel& model, const geom::LensRig& rig, const FlareCostParams& params) {
    auto snap = std::make_shared<Snapshot>();
    snap->model = model;
    snap->rig = rig;
    snap->params = params;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = std::move(snap);
}

void FlareSeamPenalty::clear() noexcept {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.reset();
}

std::shared_ptr<const FlareSeamPenalty::Snapshot> FlareSeamPenalty::snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool FlareSeamPenalty::hook(const LensBands& warped, std::vector<float>& penaltySlave,
                            std::vector<float>& penaltyMaster, void* user) noexcept {
    try {
        const auto* self = static_cast<const FlareSeamPenalty*>(user);
        if (self == nullptr) {
            return false;
        }
        const std::shared_ptr<const Snapshot> snap = self->snapshot();
        // Nothing measured, or no sun anywhere: nothing to steer around.
        if (!snap || (!snap->model.lens[0].sunFound && !snap->model.lens[1].sunFound)) {
            return false;
        }
        // The band geometry must be one the kernel mapping can describe, and
        // the maps exactly the size the caller promised (never resized here).
        const std::size_t n = static_cast<std::size_t>(warped.w) * warped.h;
        if (warped.w == 0 || warped.h == 0 || warped.mapH == 0 || warped.rowOffset >= warped.mapH ||
            warped.h > warped.mapH - warped.rowOffset || penaltySlave.size() != n || penaltyMaster.size() != n) {
            return false;
        }
        // Coverage is the seam's own cost: an unseen pixel adds nothing here.
        fillBandCost(snap->model, snap->rig, warped.w, warped.mapH, warped.rowOffset, warped.h, nullptr,
                     snap->params, 0.0f, {penaltySlave.data(), penaltyMaster.data()});
        return true;
    } catch (...) {
        // A copy of a shared_ptr under a mutex can only fail on a broken
        // lock; the contract is "must not throw", so contribute nothing.
        return false;
    }
}

}  // namespace osv::render
