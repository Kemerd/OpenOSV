// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MeshWarpLines.cpp - straight line segments on the seam band, for the mesh
// warp's line term (MeshWarp.h).
//
// THE DETECTOR
// ------------
// An LSD-style detector (von Gioi, Jakubowicz, Morel & Randall, "LSD: a Line
// Segment Detector", IPOL 2012), written for the band: a 2048 x ~70 ring of
// one lens's luma with a coverage mask.  The steps are LSD's:
//
//   1. a light blur (one [1 2 1] / 4 pass, normalised over covered pixels),
//      standing in for LSD's Gaussian down-scaling: the band is a bilinear
//      resampling of a fisheye frame and its staircase would split shallow
//      edges into short pieces;
//   2. the gradient on a 2 x 2 window and its LEVEL-LINE angle; a window with
//      an uncovered pixel has no gradient, so a rim or an occlusion polygon
//      can never become a line;
//   3. pixels pseudo-ordered by gradient magnitude (1024 bins), those below
//      q / sin(tau) never used (their angle is noise);
//   4. region growing from each unused seed over the 8-neighbourhood, a pixel
//      joining when its level-line angle is within tau of the region's;
//   5. the region's rectangle (centre of mass and principal axis of the
//      gradient-weighted inertia), its density checked;
//   6. the a-contrario test: the rectangle's Number of False Alarms - the
//      expected count of rectangles at least this aligned in pure noise,
//      NT x the binomial tail B(n, k, p) with p = tau / pi - must not exceed
//      epsilon.
//
// What it leaves out, deliberately: LSD's rectangle refinement (a sparse
// region is refused, not reshaped - a missing constraint is harmless, a
// wrong one bends the field) and its exact improvement search.  The band is
// a ring: longitude wraps, and a region carries UNWRAPPED coordinates so a
// segment across the ring's seam meridian keeps its shape.

#include "osv/render/MeshWarp.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace osv::render {

namespace {

/// A gradient pixel with no usable angle (LSD's NOTDEF).
constexpr float kNoAngle = -1024.0f;

/// One lens's band prepared for detection: the gradient magnitude and the
/// level-line angle at every 2 x 2 window, NOTDEF where a window is not fully
/// covered or its luma is not finite.
struct GradientField {
    std::uint32_t w = 0;
    std::uint32_t h = 0;          ///< Rows of windows (band rows - 1).
    std::vector<float> mag;       ///< |gradient|, 0 at NOTDEF.
    std::vector<float> angle;     ///< Level-line angle (radians, (-pi, pi]), kNoAngle at NOTDEF.
};

/// Blur the covered pixels of one lens with `passes` separable [1 2 1] / 4
/// passes, normalised over the covered taps (an uncovered tap weighs
/// nothing, so black beyond a rim never bleeds in).  Longitude wraps,
/// latitude clamps.  Uncovered pixels keep their value (they are never read
/// as covered).
std::vector<double> blurCovered(const std::vector<float>& luma, const std::vector<std::uint8_t>& covered,
                                std::uint32_t w, std::uint32_t h, std::uint32_t passes) {
    std::vector<double> img(luma.size());
    for (std::size_t i = 0; i < luma.size(); ++i) {
        const double v = static_cast<double>(luma[i]);
        img[i] = std::isfinite(v) ? v : 0.0;
    }
    std::vector<double> tmp(img.size());
    for (std::uint32_t pass = 0; pass < passes; ++pass) {
        // Horizontal, wrapping round the ring.
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * w;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = row + x;
                if (!covered[i]) {
                    tmp[i] = img[i];
                    continue;
                }
                const std::size_t l = row + (x == 0 ? w - 1u : x - 1u);
                const std::size_t r = row + (x + 1u == w ? 0u : x + 1u);
                double acc = 2.0 * img[i];
                double wsum = 2.0;
                if (covered[l]) {
                    acc += img[l];
                    wsum += 1.0;
                }
                if (covered[r]) {
                    acc += img[r];
                    wsum += 1.0;
                }
                tmp[i] = acc / wsum;
            }
        }
        // Vertical, clamped at the band's ends.
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                if (!covered[i]) {
                    img[i] = tmp[i];
                    continue;
                }
                double acc = 2.0 * tmp[i];
                double wsum = 2.0;
                if (y > 0 && covered[i - w]) {
                    acc += tmp[i - w];
                    wsum += 1.0;
                }
                if (y + 1u < h && covered[i + w]) {
                    acc += tmp[i + w];
                    wsum += 1.0;
                }
                img[i] = acc / wsum;
            }
        }
    }
    return img;
}

/// The gradient field of one lens (step 2).
GradientField gradientField(const std::vector<float>& luma, const std::vector<float>& alpha, std::uint32_t w,
                            std::uint32_t h, std::uint32_t blurPasses) {
    GradientField gf;
    gf.w = w;
    gf.h = h > 0 ? h - 1u : 0u;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<std::uint8_t> covered(n, 0u);
    for (std::size_t i = 0; i < n; ++i) {
        covered[i] = (alpha[i] > 0.5f && std::isfinite(luma[i])) ? 1u : 0u;
    }
    const std::vector<double> img = blurCovered(luma, covered, w, h, blurPasses);
    gf.mag.assign(static_cast<std::size_t>(gf.w) * gf.h, 0.0f);
    gf.angle.assign(gf.mag.size(), kNoAngle);
    for (std::uint32_t y = 0; y < gf.h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t x1 = x + 1u == w ? 0u : x + 1u;  // the ring wraps
            const std::size_t a = static_cast<std::size_t>(y) * w + x;
            const std::size_t b = static_cast<std::size_t>(y) * w + x1;
            const std::size_t c = a + w;
            const std::size_t d = b + w;
            if (!covered[a] || !covered[b] || !covered[c] || !covered[d]) {
                continue;  // a coverage edge is never a line
            }
            // LSD's 2 x 2 gradient: com1 along one diagonal, com2 along the other.
            const double com1 = img[d] - img[a];
            const double com2 = img[b] - img[c];
            const double gx = 0.5 * (com1 + com2);
            const double gy = 0.5 * (com1 - com2);
            const double m = std::sqrt(gx * gx + gy * gy);
            const std::size_t k = static_cast<std::size_t>(y) * w + x;
            gf.mag[k] = static_cast<float>(m);
            // The level-line angle: perpendicular to the gradient.
            gf.angle[k] = m > 0.0 ? static_cast<float>(std::atan2(gx, -gy)) : kNoAngle;
        }
    }
    return gf;
}

/// |a - b| folded onto [0, pi] (LSD's angle_diff).
[[nodiscard]] double angleDiff(double a, double b) noexcept {
    double d = a - b;
    while (d <= -kPi) {
        d += kTwoPi;
    }
    while (d > kPi) {
        d -= kTwoPi;
    }
    return std::fabs(d);
}

/// log10 of the binomial tail sum_{i >= k} C(n, i) p^i (1 - p)^(n - i).
///
/// Evaluated from its first term in log space and the term ratio
/// (n - i) / (i + 1) x p / (1 - p), stopping when the remaining terms cannot
/// change the sum - the same series LSD's nfa() sums, without its tabulated
/// log-gamma.
[[nodiscard]] double log10BinomialTail(std::uint64_t n, std::uint64_t k, double p) noexcept {
    if (k == 0) {
        return 0.0;  // the whole distribution
    }
    if (k > n) {
        return -std::numeric_limits<double>::infinity();
    }
    const double nd = static_cast<double>(n);
    const double kd = static_cast<double>(k);
    const double logTerm = std::lgamma(nd + 1.0) - std::lgamma(kd + 1.0) - std::lgamma(nd - kd + 1.0) +
                           kd * std::log(p) + (nd - kd) * std::log1p(-p);
    double sum = 1.0;
    double term = 1.0;
    const double q = p / (1.0 - p);
    for (std::uint64_t i = k; i < n; ++i) {
        term *= (nd - static_cast<double>(i)) / (static_cast<double>(i) + 1.0) * q;
        sum += term;
        if (term < sum * 1e-12) {
            break;  // the tail of the tail is below double precision
        }
    }
    return (logTerm + std::log(sum)) / std::log(10.0);
}

/// One pixel of a growing region, with its UNWRAPPED column.
struct RegionPixel {
    std::uint32_t idx = 0;  ///< Index into the gradient field.
    long long ux = 0;       ///< Unwrapped column.
    long long uy = 0;       ///< Row.
};

/// Detect the segments of one lens (steps 3 to 6), appending to `out`.
void detectLens(const GradientField& gf, const LensBands& bands, int lens, const LineDetectParams& p,
                std::vector<SeamLine>& out, std::uint32_t& regions, std::uint32_t& rejected) {
    const std::uint32_t w = gf.w;
    const std::uint32_t h = gf.h;
    if (w == 0 || h == 0) {
        return;
    }
    const double tau = deg2rad(p.angleToleranceDeg);
    const double prec = tau;
    const double pAligned = tau / kPi;
    const double rho = p.gradientQuantization / std::sin(tau);
    // The number of tests: (W H)^(5/2) rectangles, LSD's NT.
    const double logNT = 2.5 * (std::log10(static_cast<double>(w)) + std::log10(static_cast<double>(h)));
    // A region smaller than this could never be meaningful (LSD's min_reg_size).
    const auto minRegion = static_cast<std::size_t>(std::max(2.0, -logNT / std::log10(pAligned)));

    // ---- pseudo-ordering by magnitude (1024 bins, strongest first) ----------------
    const std::size_t n = gf.mag.size();
    float maxMag = 0.0f;
    for (const float m : gf.mag) {
        maxMag = std::max(maxMag, m);
    }
    std::vector<std::uint8_t> used(n, 1u);
    if (!(maxMag > static_cast<float>(rho))) {
        return;  // no pixel strong enough to carry an angle
    }
    constexpr std::size_t kBins = 1024;
    std::vector<std::uint32_t> binCount(kBins, 0u);
    const auto binOf = [&](float m) {
        const auto b = static_cast<std::size_t>(static_cast<double>(m) / static_cast<double>(maxMag) *
                                                static_cast<double>(kBins - 1u));
        return std::min(b, kBins - 1u);
    };
    for (std::size_t i = 0; i < n; ++i) {
        if (gf.angle[i] != kNoAngle && gf.mag[i] > static_cast<float>(rho)) {
            used[i] = 0u;
            ++binCount[binOf(gf.mag[i])];
        }
    }
    // Strongest bin first: start offsets in descending bin order.
    std::vector<std::uint32_t> binStart(kBins, 0u);
    std::uint32_t acc = 0;
    for (std::size_t b = kBins; b-- > 0;) {
        binStart[b] = acc;
        acc += binCount[b];
    }
    std::vector<std::uint32_t> order(acc);
    for (std::size_t i = 0; i < n; ++i) {
        if (!used[i]) {
            order[binStart[binOf(gf.mag[i])]++] = static_cast<std::uint32_t>(i);
        }
    }

    const double radPerCol = kTwoPi / static_cast<double>(bands.w);
    const double radPerRow = kPi / static_cast<double>(bands.mapH);
    std::vector<RegionPixel> reg;
    reg.reserve(1024);
    for (const std::uint32_t seed : order) {
        if (used[seed]) {
            continue;
        }
        // ---- region growing (step 4) ----------------------------------------------
        reg.clear();
        used[seed] = 1u;
        reg.push_back({seed, static_cast<long long>(seed % w), static_cast<long long>(seed / w)});
        double sumDx = std::cos(static_cast<double>(gf.angle[seed]));
        double sumDy = std::sin(static_cast<double>(gf.angle[seed]));
        double regAngle = static_cast<double>(gf.angle[seed]);
        for (std::size_t r = 0; r < reg.size(); ++r) {
            const RegionPixel cur = reg[r];
            for (int dy = -1; dy <= 1; ++dy) {
                const long long yy = cur.uy + dy;
                if (yy < 0 || yy >= static_cast<long long>(h)) {
                    continue;
                }
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) {
                        continue;
                    }
                    const long long uxx = cur.ux + dx;
                    const long long xx = ((uxx % static_cast<long long>(w)) + static_cast<long long>(w)) %
                                         static_cast<long long>(w);
                    const auto k = static_cast<std::uint32_t>(yy * static_cast<long long>(w) + xx);
                    if (used[k] || angleDiff(static_cast<double>(gf.angle[k]), regAngle) > prec) {
                        continue;
                    }
                    used[k] = 1u;
                    reg.push_back({k, uxx, yy});
                    sumDx += std::cos(static_cast<double>(gf.angle[k]));
                    sumDy += std::sin(static_cast<double>(gf.angle[k]));
                    regAngle = std::atan2(sumDy, sumDx);
                }
            }
        }
        ++regions;
        if (reg.size() < minRegion) {
            ++rejected;
            continue;
        }

        // ---- the rectangle (step 5) -----------------------------------------------
        double sumW = 0.0, cx = 0.0, cy = 0.0;
        for (const RegionPixel& q : reg) {
            const double m = static_cast<double>(gf.mag[q.idx]);
            sumW += m;
            cx += m * static_cast<double>(q.ux);
            cy += m * static_cast<double>(q.uy);
        }
        if (!(sumW > 0.0)) {
            ++rejected;
            continue;
        }
        cx /= sumW;
        cy /= sumW;
        double ixx = 0.0, iyy = 0.0, ixy = 0.0;
        for (const RegionPixel& q : reg) {
            const double m = static_cast<double>(gf.mag[q.idx]);
            const double ddx = static_cast<double>(q.ux) - cx;
            const double ddy = static_cast<double>(q.uy) - cy;
            ixx += m * ddy * ddy;
            iyy += m * ddx * ddx;
            ixy -= m * ddx * ddy;
        }
        // The principal axis, as LSD's get_theta computes it, turned to agree
        // with the region's angle.
        const double lambda = 0.5 * (ixx + iyy - std::sqrt((ixx - iyy) * (ixx - iyy) + 4.0 * ixy * ixy));
        double theta = std::fabs(ixx) > std::fabs(iyy) ? std::atan2(lambda - ixx, ixy) : std::atan2(ixy, lambda - iyy);
        if (angleDiff(theta, regAngle) > prec) {
            theta += kPi;
        }
        const double dxu = std::cos(theta);
        const double dyu = std::sin(theta);
        double lMin = 0.0, lMax = 0.0, wMin = 0.0, wMax = 0.0;
        for (const RegionPixel& q : reg) {
            const double ddx = static_cast<double>(q.ux) - cx;
            const double ddy = static_cast<double>(q.uy) - cy;
            const double l = ddx * dxu + ddy * dyu;
            const double ww = -ddx * dyu + ddy * dxu;
            lMin = std::min(lMin, l);
            lMax = std::max(lMax, l);
            wMin = std::min(wMin, ww);
            wMax = std::max(wMax, ww);
        }
        const double length = lMax - lMin;
        const double width = std::max(1.0, wMax - wMin);
        if (length < p.minLengthPx) {
            ++rejected;
            continue;
        }
        const double density = static_cast<double>(reg.size()) / (length * width);
        if (density < p.minDensity) {
            ++rejected;  // a curve or a blob: refused rather than reshaped
            continue;
        }

        // ---- the a-contrario test (step 6) -------------------------------------------
        // Every window inside the rectangle counts; the aligned ones are those
        // whose level-line angle is within tau of the rectangle's.
        const double lMid = 0.5 * (lMin + lMax);
        const double wMid = 0.5 * (wMin + wMax);
        const double halfL = 0.5 * length;
        const double halfW = 0.5 * width;
        const double ext = halfL + halfW + 1.0;
        const auto x0 = static_cast<long long>(std::floor(cx - ext));
        const auto x1 = static_cast<long long>(std::ceil(cx + ext));
        const auto y0 = static_cast<long long>(std::max(0.0, std::floor(cy - ext)));
        const auto y1 = static_cast<long long>(std::min(static_cast<double>(h - 1u), std::ceil(cy + ext)));
        std::uint64_t total = 0;
        std::uint64_t aligned = 0;
        for (long long yy = y0; yy <= y1; ++yy) {
            for (long long uxx = x0; uxx <= x1; ++uxx) {
                const double ddx = static_cast<double>(uxx) - cx;
                const double ddy = static_cast<double>(yy) - cy;
                const double l = ddx * dxu + ddy * dyu - lMid;
                const double ww = -ddx * dyu + ddy * dxu - wMid;
                if (std::fabs(l) > halfL || std::fabs(ww) > halfW) {
                    continue;
                }
                ++total;
                const long long xx = ((uxx % static_cast<long long>(w)) + static_cast<long long>(w)) %
                                     static_cast<long long>(w);
                const std::size_t k = static_cast<std::size_t>(yy) * w + static_cast<std::size_t>(xx);
                const float a = gf.angle[k];
                if (a != kNoAngle && angleDiff(static_cast<double>(a), theta) <= prec) {
                    ++aligned;
                }
            }
        }
        const double logNfa = logNT + log10BinomialTail(total, aligned, pAligned);
        if (!(logNfa <= p.logEpsilon)) {
            ++rejected;  // as likely in noise as in the picture
            continue;
        }

        // ---- the segment, in the band's polar-axis frame -------------------------------
        // A window at (x, y) is centred at pixel-centre coordinate (x + 1, y + 1).
        const double ex0 = cx + lMin * dxu + 1.0;
        const double ey0 = cy + lMin * dyu + 1.0;
        const double ex1 = cx + lMax * dxu + 1.0;
        const double ey1 = cy + lMax * dyu + 1.0;
        SeamLine ln;
        ln.lens = lens;
        ln.lon0Rad = ex0 * radPerCol - kPi;
        ln.lat0Rad = kHalfPi - (static_cast<double>(bands.rowOffset) + ey0) * radPerRow;
        ln.lon1Rad = ex1 * radPerCol - kPi;
        ln.lat1Rad = kHalfPi - (static_cast<double>(bands.rowOffset) + ey1) * radPerRow;
        // The first endpoint into [-pi, pi); the second follows it unwrapped.
        const double wrap = std::floor((ln.lon0Rad + kPi) / kTwoPi) * kTwoPi;
        ln.lon0Rad -= wrap;
        ln.lon1Rad -= wrap;
        ln.lengthPx = length;
        ln.widthPx = width;
        ln.logNfa = logNfa;
        ln.support = static_cast<std::uint32_t>(reg.size());
        out.push_back(ln);
    }
}

}  // namespace

Result<SeamLineSet> detectSeamLines(const LensBands& bands, const LineDetectParams& params, ThreadPool* pool) {
    const auto t0 = std::chrono::steady_clock::now();
    // ---- inputs -------------------------------------------------------------------------
    if (bands.w < 8 || bands.h < 3 || bands.mapH == 0) {
        return Error{ErrorCode::InvalidArgument, "detectSeamLines: the band is empty or too small"};
    }
    const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
    for (int lens = 0; lens < 2; ++lens) {
        if (bands.luma[lens].size() != n || bands.alpha[lens].size() != n) {
            return Error{ErrorCode::InvalidArgument, "detectSeamLines: a band plane is the wrong size"};
        }
    }
    if (!(params.angleToleranceDeg > 0.0) || params.angleToleranceDeg >= 90.0 ||
        !(params.gradientQuantization > 0.0) || !std::isfinite(params.gradientQuantization) ||
        !std::isfinite(params.logEpsilon) || !(params.minDensity >= 0.0) || params.minDensity > 1.0 ||
        !(params.minLengthPx >= 2.0) || !std::isfinite(params.minLengthPx) || params.blurPasses > 8) {
        return Error{ErrorCode::InvalidArgument, "detectSeamLines: parameters out of range"};
    }

    // ---- the two lenses, side by side when a pool can ---------------------------------
    std::vector<SeamLine> perLens[2];
    std::uint32_t regions[2] = {0, 0};
    std::uint32_t rejected[2] = {0, 0};
    const auto lensTask = [&](std::size_t lens) {
        const GradientField gf =
            gradientField(bands.luma[lens], bands.alpha[lens], bands.w, bands.h, params.blurPasses);
        detectLens(gf, bands, static_cast<int>(lens), params, perLens[lens], regions[lens], rejected[lens]);
    };
    if (pool != nullptr && pool->size() > 1) {
        if (!pool->parallelRows(2u, 1u, lensTask).ok()) {
            // Each task only writes its own lens's outputs: redo them here.
            for (std::size_t lens = 0; lens < 2; ++lens) {
                perLens[lens].clear();
                regions[lens] = 0;
                rejected[lens] = 0;
                lensTask(lens);
            }
        }
    } else {
        lensTask(0);
        lensTask(1);
    }

    SeamLineSet set;
    for (int lens = 0; lens < 2; ++lens) {
        set.perLens[lens] = static_cast<std::uint32_t>(perLens[lens].size());
        set.regions += regions[lens];
        set.rejected += rejected[lens];
        set.lines.insert(set.lines.end(), perLens[lens].begin(), perLens[lens].end());
    }
    set.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return set;
}

}  // namespace osv::render
