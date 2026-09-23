// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_seam_tools.cpp - the Source Settings seam tools (SeamTools.h): Seam
// Blend, Parallax Blend, Seam Smoothing and the Near / Far Offset.  [WP-SEAMTOOLS]
//
// What is pinned here:
//   * every tool at its default leaves the parameter block, the carve and the
//     render bit for bit as they were;
//   * the blend widths change the feather and never the seam's path, and
//     only pixels both lenses see;
//   * the smoothing changes only the overlap, softens a step on a near
//     object and keeps its sharp double image within a stated bound;
//   * the smoothing renders alike on the CPU, CUDA and OpenCL;
//   * the low band is a normalised convolution (a flat scene stays flat to
//     the rim) and is sized from the lens;
//   * the offset grid adds the documented along-seam shift, with the
//     documented sign, fading to zero at the grid's edges;
//   * the carve's near weight is high where the lenses disagree and glides
//     between buckets.
//
// The scenes are synthetic dual-fisheye frames through the sample clip's own
// calibration (SynthFisheye.h), so no clip is needed.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "SynthFisheye.h"

#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/Renderer.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/render/SeamCarve.h"
#include "osv/render/SeamTools.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace osv;
using namespace osv::testsynth;

namespace {

// ===========================================================================
//  Scenes
// ===========================================================================

/// Stream size of the synthetic frames: large enough for a real low band
/// (a 128 x 128 table), small enough to render every scene in well under a
/// second.
constexpr int kStream = 1024;

/// A smooth sky-like background (linear, grey ~0.3).
double background(const Vec3d& d) {
    return 0.30 * (1.0 + 0.20 * std::cos(polarLon(d)) + 0.10 * std::sin(2.0 * polarLat(d)));
}

/// A "near object": a bright bar across the seam at polar longitude
/// 40..60 deg as lens 0 sees it and `shiftDeg` further along the seam as
/// lens 1 does - two viewpoints of something close to the camera, which no
/// seam can put back together, so the cut shows a step.
Radiance nearBar(double shiftDeg) {
    return [shiftDeg](int lens, const Vec3d& d, double, double rgb[3]) {
        const double lon = rad2deg(polarLon(d)) - (lens == 1 ? shiftDeg : 0.0);
        const double lat = rad2deg(polarLat(d));
        double v = background(d);
        if (lon > 40.0 && lon < 60.0 && std::fabs(lat) < 25.0) {
            v = 0.9;
        }
        rgb[0] = rgb[1] = rgb[2] = v;
    };
}

/// A flat, featureless grey: every lens sees 0.3 everywhere.
void flatGrey(int, const Vec3d&, double, double rgb[3]) { rgb[0] = rgb[1] = rgb[2] = 0.3; }

/// A textured scene both lenses agree on (no parallax): a longitude /
/// latitude checker of 6 deg cells.
void checker(int, const Vec3d& d, double, double rgb[3]) {
    const int a = static_cast<int>(std::floor(rad2deg(polarLon(d)) / 6.0));
    const int b = static_cast<int>(std::floor(rad2deg(polarLat(d)) / 6.0));
    const double v = ((a + b) & 1) ? 0.6 : 0.15;
    rgb[0] = rgb[1] = rgb[2] = v;
}

/// A polar-axis equirect map (lens axes at the poles, the seam on the equator).
geom::EquirectMap polarMap(int w) {
    geom::EquirectMap m;
    m.layout = geom::EquirectLayout::PolarAxis;
    m.w = w;
    m.h = w / 2;
    return m;
}

/// Linear-light colour for the renders (the blend's own space).
OsvColorParams linearColour() {
    return color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
}

/// A seam on the geometric seam (latitude 0) with the default narrow feather
/// in every column and a near weight of `near` - deterministic, so a test
/// does not depend on where a carve decides to go.
render::BlendSeam fixedSeam(double halfWidthDeg = 0.35, float near = 1.0f) {
    render::BlendSeam s;
    s.columns = 1024;
    s.table.assign(2048u, 0.0f);
    for (std::uint32_t c = 0; c < s.columns; ++c) {
        s.table[c * 2u + 1u] = static_cast<float>(deg2rad(halfWidthDeg));
    }
    s.edgeRad = static_cast<float>(deg2rad(0.5));
    s.nearWeight.assign(s.columns, near);
    return s;
}

/// Everything a render of one configuration needs.
struct Config {
    const render::BlendSeam* seam = nullptr;
    const render::ParallaxWarpGrid* grid = nullptr;
    double smoothingDeg = 0.0;
    int onlyLens = -1;
};

render::RenderParamsBuilder builderFor(const geom::LensRig& rig, const Config& c) {
    render::RenderParamsBuilder b;
    b.rig(rig).color(linearColour()).blend(geom::BlendParams{}, true).alphaCoverage(true);
    if (c.seam) {
        render::applyBlendSeam(b, *c.seam);
    }
    if (c.grid) {
        b.warp(c.grid->uv, c.grid->w, c.grid->h, c.grid->latMinRad, c.grid->latMaxRad);
    }
    if (c.smoothingDeg > 0.0) {
        b.seamSmooth(c.smoothingDeg);
    }
    if (c.onlyLens == 0 || c.onlyLens == 1) {
        b.lensEnabled(1 - c.onlyLens, false);
    }
    return b;
}

render::ImageRGBAf renderPolar(render::IRenderer& r, const geom::LensRig& rig, const video::FramePair& pair,
                               const Config& c, int w = 2048) {
    render::RenderParamsBuilder b = builderFor(rig, c);
    b.equirect(polarMap(w));
    auto job = b.build(pair);
    REQUIRE(job.ok());
    auto img = r.render(job.value());
    REQUIRE(img.ok());
    return std::move(img).value();
}

/// Rec.709 luma of a (linear) pixel.
double luma(const float* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; }

/// Pixels of `a` and `b` that differ, split by whether both lenses see the
/// pixel (`s0` / `s1`: the single-lens renders, alpha = coverage).
struct DiffSplit {
    std::size_t inside = 0;   ///< Differing pixels both lenses see.
    std::size_t outside = 0;  ///< Differing pixels only one (or no) lens sees.
};

DiffSplit diffSplit(const render::ImageRGBAf& a, const render::ImageRGBAf& b, const render::ImageRGBAf& s0,
                    const render::ImageRGBAf& s1) {
    DiffSplit d;
    for (std::uint32_t y = 0; y < a.h; ++y) {
        for (std::uint32_t x = 0; x < a.w; ++x) {
            if (std::memcmp(a.pixel(x, y), b.pixel(x, y), 4 * sizeof(float)) == 0) {
                continue;
            }
            const bool both = s0.pixel(x, y)[3] > 0.0f && s1.pixel(x, y)[3] > 0.0f;
            (both ? d.inside : d.outside) += 1;
        }
    }
    return d;
}

/// The seam metrics of the bench (tests/bench/SeamCarveBench.cpp) over
/// columns [x0, x1) and |lat| <= bandDeg of a polar map: "edge" = gradient in
/// the blend that neither lens has (the visible cut), "hfGhost" = the mix
/// solved on 5 x 5 high-passed luma (a sharp double image), x1000.
struct SeamMetrics {
    double edge = 0.0;
    double hfGhost = 0.0;
};

SeamMetrics seamMetrics(const render::ImageRGBAf& B, const render::ImageRGBAf& S0, const render::ImageRGBAf& S1,
                        int x0, int x1, double bandDeg) {
    const int W = static_cast<int>(B.w);
    const int H = static_cast<int>(B.h);
    const auto L = [&](const render::ImageRGBAf& im, int x, int y) {
        return luma(im.pixel(static_cast<std::uint32_t>(std::clamp(x, 0, W - 1)),
                             static_cast<std::uint32_t>(std::clamp(y, 0, H - 1))));
    };
    const auto grad = [&](const render::ImageRGBAf& im, int x, int y) {
        const double gx = 0.5 * (L(im, x + 1, y) - L(im, x - 1, y));
        const double gy = 0.5 * (L(im, x, y + 1) - L(im, x, y - 1));
        return std::sqrt(gx * gx + gy * gy);
    };
    const auto high = [&](const render::ImageRGBAf& im, int x, int y) {
        double s = 0.0;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                s += L(im, x + dx, y + dy);
            }
        }
        return L(im, x, y) - s / 25.0;
    };
    const int half = static_cast<int>(std::lround(bandDeg / 180.0 * H));
    double edge = 0.0;
    double hf = 0.0;
    std::size_t n = 0;
    for (int y = H / 2 - half; y <= H / 2 + half; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (!(S0.pixel(x, y)[3] > 0.5f) || !(S1.pixel(x, y)[3] > 0.5f)) {
                continue;
            }
            edge += std::max(0.0, grad(B, x, y) - std::max(grad(S0, x, y), grad(S1, x, y)) - 0.004);
            const double h0 = high(S0, x, y);
            const double h1 = high(S1, x, y);
            if (std::fabs(h0 - h1) > 0.01) {
                const double a = std::clamp((high(B, x, y) - h1) / (h0 - h1), 0.0, 1.0);
                hf += 2.0 * std::min(a, 1.0 - a) * std::fabs(h0 - h1);
            }
            ++n;
        }
    }
    SeamMetrics m;
    if (n > 0) {
        m.edge = 1000.0 * edge / static_cast<double>(n);
        m.hfGhost = 1000.0 * hf / static_cast<double>(n);
    }
    return m;
}

/// Polar map column of a longitude in degrees.
int columnOf(double lonDeg, int W) { return static_cast<int>(std::lround((lonDeg + 180.0) / 360.0 * W)); }

}  // namespace

// ===========================================================================
//  Defaults
// ===========================================================================

TEST_CASE("seam tools at their defaults change nothing, bit for bit", "[seamtools]") {
    // ---- the carve parameters ---------------------------------------------------
    render::SeamCarveParams p;
    render::applySeamBlendWidths(render::SeamTools{}, p);
    const render::SeamCarveParams d;
    CHECK(p.wideHalfWidthDeg == d.wideHalfWidthDeg);
    CHECK(p.narrowHalfWidthDeg == d.narrowHalfWidthDeg);
    CHECK(p.costWindowDeg == d.narrowHalfWidthDeg);  // the window the carve always used
    CHECK(render::kDefaultSeamBlendDeg == d.wideHalfWidthDeg);
    CHECK(render::kDefaultParallaxBlendDeg == d.narrowHalfWidthDeg);
    CHECK_FALSE(render::SeamTools{}.smoothingOn());
    CHECK_FALSE(render::SeamTools{}.offsetOn());

    // ---- the parameter block ------------------------------------------------------
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    const render::BlendSeam seam = fixedSeam();
    render::RenderParamsBuilder plain = builderFor(rig.value(), Config{&seam});
    plain.equirect(polarMap(512));
    auto ref = plain.buildParams();
    REQUIRE(ref.ok());
    for (const double off : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        render::RenderParamsBuilder b = builderFor(rig.value(), Config{&seam});
        b.equirect(polarMap(512)).seamSmooth(off);
        auto got = b.buildParams();
        REQUIRE(got.ok());
        INFO("smoothing " << off);
        CHECK(std::memcmp(&got.value(), &ref.value(), sizeof(OsvRenderParams)) == 0);
    }
    // Smoothing without a carved seam has nothing to smooth: the block stays as it was.
    render::RenderParamsBuilder noSeam = builderFor(rig.value(), Config{});
    noSeam.equirect(polarMap(512));
    auto noSeamRef = noSeam.buildParams();
    noSeam.seamSmooth(2.0);
    auto noSeamGot = noSeam.buildParams();
    REQUIRE(noSeamRef.ok());
    REQUIRE(noSeamGot.ok());
    CHECK(std::memcmp(&noSeamGot.value(), &noSeamRef.value(), sizeof(OsvRenderParams)) == 0);

    // ---- the offset: zero near and far is no grid change at all -------------------
    render::ParallaxWarpGrid base;
    base.w = 8;
    base.h = 6;
    base.latMinRad = 0.15f;
    base.latMaxRad = -0.15f;
    base.uv.assign(8u * 6u * 2u, 0.001f);
    auto same = render::seamOffsetGrid(&base, seam, 0.0, 0.0);
    REQUIRE(same.ok());
    CHECK(same.value().uv == base.uv);
}

TEST_CASE("a carve with default tools is the carve before the tools existed", "[seamtools][seamcarve]") {
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), nearBar(1.5), pool);
    const render::BandParams band = render::ParallaxWarpParams{}.band;
    auto bands = render::renderLensBands(rig.value(), scene.pair, geom::BlendParams{}, band, false, nullptr, pool);
    REQUIRE(bands.ok());
    render::SeamCarveParams tuned;
    render::applySeamBlendWidths(render::SeamTools{}, tuned);
    auto a = render::carveSeamFromBands(bands.value(), {}, render::SeamCarveParams{}, nullptr, &pool);
    auto b = render::carveSeamFromBands(bands.value(), {}, tuned, nullptr, &pool);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK(a.value().table == b.value().table);
    CHECK(a.value().nearWeight == b.value().nearWeight);
}

// ===========================================================================
//  Seam Blend / Parallax Blend
// ===========================================================================

TEST_CASE("the blend widths change the feather, never the seam's path, and only the overlap", "[seamtools]") {
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), nearBar(1.5), pool);
    const render::BandParams band = render::ParallaxWarpParams{}.band;
    auto bands = render::renderLensBands(rig.value(), scene.pair, geom::BlendParams{}, band, false, nullptr, pool);
    REQUIRE(bands.ok());

    // ---- the carve: same latitudes, other widths ------------------------------------
    render::SeamTools wide;
    wide.seamBlendDeg = 4.0;
    wide.parallaxBlendDeg = 1.0;
    render::SeamCarveParams wp;
    render::applySeamBlendWidths(wide, wp);
    auto base = render::carveSeamFromBands(bands.value(), {}, render::SeamCarveParams{}, nullptr, &pool);
    auto tuned = render::carveSeamFromBands(bands.value(), {}, wp, nullptr, &pool);
    REQUIRE(base.ok());
    REQUIRE(tuned.ok());
    std::size_t moved = 0;
    std::size_t wider = 0;
    for (std::uint32_t c = 0; c < base.value().columns; ++c) {
        moved += base.value().table[c * 2u] != tuned.value().table[c * 2u] ? 1u : 0u;
        wider += tuned.value().table[c * 2u + 1u] > base.value().table[c * 2u + 1u] ? 1u : 0u;
    }
    CHECK(moved == 0);                       // the path is the same...
    CHECK(wider > base.value().columns / 2);  // ...the feather wider
    CHECK(tuned.value().meanHalfWidthDeg > base.value().meanHalfWidthDeg);

    // ---- Parallax Blend never wider than Seam Blend, 0 is a hard cut ------------------
    render::SeamTools crossed;
    crossed.seamBlendDeg = 0.5;
    crossed.parallaxBlendDeg = 3.0;
    render::SeamCarveParams cp;
    render::applySeamBlendWidths(crossed, cp);
    CHECK(cp.narrowHalfWidthDeg == cp.wideHalfWidthDeg);
    render::SeamTools hard;
    hard.parallaxBlendDeg = 0.0;
    render::SeamCarveParams hp;
    render::applySeamBlendWidths(hard, hp);
    CHECK(hp.narrowHalfWidthDeg == 0.0);
    CHECK(render::carveSeamFromBands(bands.value(), {}, hp, nullptr, &pool).ok());
    // Out-of-range widths are clamped, garbage keeps the default.
    render::SeamTools wild;
    wild.seamBlendDeg = 99.0;
    wild.parallaxBlendDeg = std::numeric_limits<double>::quiet_NaN();
    render::SeamCarveParams wl;
    render::applySeamBlendWidths(wild, wl);
    CHECK(wl.wideHalfWidthDeg == render::kMaxSeamBlendDeg);
    CHECK(wl.narrowHalfWidthDeg == render::kDefaultParallaxBlendDeg);

    // ---- the render: only pixels both lenses see change --------------------------------
    render::CpuRenderer cpu(pool);
    const render::ImageRGBAf s0 = renderPolar(cpu, rig.value(), scene.pair, Config{nullptr, nullptr, 0.0, 0});
    const render::ImageRGBAf s1 = renderPolar(cpu, rig.value(), scene.pair, Config{nullptr, nullptr, 0.0, 1});
    const render::ImageRGBAf a = renderPolar(cpu, rig.value(), scene.pair, Config{&base.value()});
    const render::ImageRGBAf b = renderPolar(cpu, rig.value(), scene.pair, Config{&tuned.value()});
    const DiffSplit d = diffSplit(a, b, s0, s1);
    INFO("changed pixels: " << d.inside << " in the overlap, " << d.outside << " outside it");
    CHECK(d.inside > 0);
    CHECK(d.outside == 0);
}

// ===========================================================================
//  Seam Smoothing
// ===========================================================================

TEST_CASE("seam smoothing changes only the overlap, softens a step and keeps the detail single", "[seamtools]") {
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), nearBar(1.5), pool);
    const render::BlendSeam seam = fixedSeam();
    render::CpuRenderer cpu(pool);
    const int W = 2048;
    const render::ImageRGBAf s0 = renderPolar(cpu, rig.value(), scene.pair, Config{nullptr, nullptr, 0.0, 0}, W);
    const render::ImageRGBAf s1 = renderPolar(cpu, rig.value(), scene.pair, Config{nullptr, nullptr, 0.0, 1}, W);
    const render::ImageRGBAf off = renderPolar(cpu, rig.value(), scene.pair, Config{&seam}, W);
    const render::ImageRGBAf on = renderPolar(cpu, rig.value(), scene.pair, Config{&seam, nullptr, 2.0}, W);

    // ---- only the overlap, and only near the seam ------------------------------------
    const DiffSplit d = diffSplit(off, on, s0, s1);
    INFO("changed pixels: " << d.inside << " in the overlap, " << d.outside << " outside it");
    CHECK(d.inside > 0);
    CHECK(d.outside == 0);
    // Beyond the widened feather (2 deg) the two bands' weights agree exactly.
    const int H = W / 2;
    const int beyond = static_cast<int>(std::ceil(2.05 / 180.0 * H));
    std::size_t far = 0;
    for (int y = 0; y < H; ++y) {
        if (std::abs(y - H / 2) <= beyond) {
            continue;
        }
        for (int x = 0; x < W; ++x) {
            far += std::memcmp(off.pixel(x, y), on.pixel(x, y), 4 * sizeof(float)) != 0 ? 1u : 0u;
        }
    }
    CHECK(far == 0);

    // ---- the step softens; the sharp double image stays within its bound -------------
    // The step: the bar's two edges sit 1.5 deg apart along the seam in the
    // two lenses, so on the columns between them the picture jumps across
    // the cut.  Measured as the largest across-seam (latitude) gradient
    // within the seam's rows on those columns - the bar has no edge along
    // the seam there, so any such gradient is the cut.  Smoothing moves the
    // low frequencies of the jump into a gradient over the wide band and
    // leaves only the high band's jump at the cut.
    const auto step = [&](const render::ImageRGBAf& im) {
        double sum = 0.0;
        int n = 0;
        for (const double lo : {39.8, 59.8}) {
            for (int x = columnOf(lo, W); x < columnOf(lo + 1.9, W); ++x) {
                double worst = 0.0;
                for (int y = H / 2 - 6; y <= H / 2 + 6; ++y) {
                    const double g = 0.5 * std::fabs(luma(im.pixel(x, y + 1)) - luma(im.pixel(x, y - 1)));
                    worst = std::max(worst, g);
                }
                sum += worst;
                ++n;
            }
        }
        return n ? sum / n : 0.0;
    };
    const double stepOff = step(off);
    const double stepOn = step(on);
    // The bound on doubling: the high-frequency ghost (a sharp second copy)
    // over the bar and its surroundings, |lat| <= 3 deg, at most doubles
    // (+0.5 absolute).  On the sample clip the bench measures +55 % at 2 deg
    // against +198 % for a Parallax Blend of 1 deg.
    const int x0 = columnOf(30.0, W);
    const int x1 = columnOf(75.0, W);
    const SeamMetrics mOff = seamMetrics(off, s0, s1, x0, x1, 3.0);
    const SeamMetrics mOn = seamMetrics(on, s0, s1, x0, x1, 3.0);
    WARN("near bar across the seam: step " << stepOff << " -> " << stepOn << ", high-frequency ghost "
                                           << mOff.hfGhost << " -> " << mOn.hfGhost << ", edge " << mOff.edge
                                           << " -> " << mOn.edge << " (smoothing 2 deg)");
    CHECK(stepOn < 0.6 * stepOff);
    CHECK(mOn.hfGhost <= 2.0 * mOff.hfGhost + 0.5);
}

TEST_CASE("the low band is a normalised convolution sized from the lens", "[seamtools]") {
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), flatGrey, pool);
    const render::BlendSeam seam = fixedSeam();

    // ---- sizing -------------------------------------------------------------------------
    render::RenderParamsBuilder b = builderFor(rig.value(), Config{&seam, nullptr, 4.0});
    b.equirect(polarMap(512));
    auto job = b.build(scene.pair);
    REQUIRE(job.ok());
    const OsvRenderParams& p = job.value().params;
    REQUIRE(p.seamSmoothEnabled == 1);
    CHECK(p.seamLowFactor == render::kSeamLowFactor);
    CHECK(p.seamLowW == (kStream + render::kSeamLowFactor - 1) / render::kSeamLowFactor);
    CHECK(p.seamLowH == p.seamLowW);
    CHECK(p.seamLowRadius >= 1);
    CHECK(p.seamLowRadius <= OSV_SEAM_LOW_MAX_RADIUS);
    CHECK(p.seamLowTaps[0] == 1.0f);
    for (int k = 1; k <= p.seamLowRadius; ++k) {
        CHECK(p.seamLowTaps[k] < p.seamLowTaps[k - 1]);
    }
    CHECK_THAT(p.seamSmoothHalfRad, Catch::Matchers::WithinAbs(deg2rad(4.0), 1e-6));
    CHECK(render::seamSmoothParamsValid(p));
    CHECK(render::seamLowTableFloats(p) == 2u * static_cast<std::size_t>(p.seamLowW) * p.seamLowH * 4u);
    // A wider smoothing blurs more.
    render::RenderParamsBuilder narrow = builderFor(rig.value(), Config{&seam, nullptr, 1.0});
    auto np = narrow.equirect(polarMap(512)).buildParams();
    REQUIRE(np.ok());
    CHECK(np.value().seamLowRadius < p.seamLowRadius);

    // ---- a flat scene stays flat right up to the rim ------------------------------------
    std::vector<float> low;
    std::vector<float> scratch;
    const OsvPlane planes[2] = {job.value().planes[0], job.value().planes[1]};
    REQUIRE(render::buildSeamLowBand(p, planes, low, scratch, &pool).ok());
    REQUIRE(low.size() == render::seamLowTableFloats(p));
    std::size_t covered = 0;
    std::size_t empty = 0;
    double worst = 0.0;
    for (std::size_t i = 0; i < low.size(); i += 4) {
        const float a = low[i + 3];
        if (a > 0.05f) {
            ++covered;
            // Un-premultiplied, every covered texel is the scene's grey: no
            // black from beyond the circle leaked in, not even at the rim.
            worst = std::max(worst, std::fabs(static_cast<double>(low[i + 1] / a) - 0.3) / 0.3);
        } else if (a == 0.0f) {
            ++empty;
        }
    }
    CHECK(covered > low.size() / 4 / 2);  // most of each lens's square is inside its circle
    CHECK(empty > 0);                      // the corners beyond it are not
    CHECK(worst < 0.01);

    // ---- refusals ------------------------------------------------------------------------
    OsvRenderParams bad = p;
    bad.seamLowFactor = 3;  // odd: breaks the 2 x 2 chroma walk
    CHECK_FALSE(render::seamSmoothParamsValid(bad));
    CHECK_FALSE(render::buildSeamLowBand(bad, planes, low, scratch, &pool).ok());
    bad = p;
    bad.seamLowRadius = OSV_SEAM_LOW_MAX_RADIUS + 1;
    CHECK_FALSE(render::seamSmoothParamsValid(bad));
    bad = p;
    bad.seamLowW = 3;  // does not cover the lens
    CHECK_FALSE(render::seamSmoothParamsValid(bad));
    render::RenderJob broken = job.value();
    broken.params.seamLowTaps[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(broken.valid());
    OsvRenderParams offParams = p;
    offParams.seamSmoothEnabled = 0;
    CHECK(render::seamSmoothParamsValid(offParams));
    CHECK_FALSE(render::buildSeamLowBand(offParams, planes, low, scratch, &pool).ok());
}

TEST_CASE("seam smoothing renders the same on the CPU, CUDA and OpenCL", "[seamtools][cuda][opencl]") {
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), nearBar(1.5), pool);
    const render::BlendSeam seam = fixedSeam();
    std::vector<render::RenderJob> jobs;
    for (const bool equirect : {false, true}) {
        for (const color::OutputTransfer t : {color::OutputTransfer::PQ, color::OutputTransfer::Passthrough}) {
            render::RenderParamsBuilder b = builderFor(rig.value(), Config{&seam, nullptr, 3.0});
            b.color(color::makeColorParams(color::kDefaultDlogMFit, t, 0.0f));
            if (equirect) {
                b.equirect(polarMap(1024));
            } else {
                geom::VirtualCamera cam;
                cam.w = 1280;
                cam.h = 720;
                cam.hfovDeg = 60.0;
                cam.yawDeg = 90.0;  // the seam runs through the middle
                b.camera(cam);
            }
            auto job = b.build(scene.pair);
            REQUIRE(job.ok());
            REQUIRE(job.value().params.seamSmoothEnabled == 1);
            jobs.push_back(std::move(job).value());
        }
    }
    std::vector<std::pair<std::string, std::unique_ptr<render::IRenderer>>> gpus;
#if defined(OSV_HAVE_CUDA)
    if (render::CudaRenderer::available(nullptr)) {
        auto r = render::CudaRenderer::create(0);
        REQUIRE(r.ok());
        gpus.emplace_back("cuda", std::move(r).value());
    }
#endif
#if defined(OSV_HAVE_OPENCL)
    if (render::OpenClRenderer::available(nullptr)) {
        auto r = render::OpenClRenderer::create(0);
        REQUIRE(r.ok());
        gpus.emplace_back("opencl", std::move(r).value());
    }
#endif
    if (gpus.empty()) {
        SKIP("no GPU backend");
    }
    render::CpuRenderer cpu(pool);
    for (const render::RenderJob& job : jobs) {
        auto ref = cpu.render(job);
        REQUIRE(ref.ok());
        for (auto& [name, gpu] : gpus) {
            auto test = gpu->render(job);
            REQUIRE(test.ok());
            const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
            WARN(name << " vs CPU with seam smoothing (" << job.params.outW << "x" << job.params.outH << ", transfer "
                      << job.params.color.transfer << "): PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode
                      << " codes");
            CHECK(stats.psnrDb >= 60.0);
        }
    }
}

// ===========================================================================
//  Near / Far Offset
// ===========================================================================

TEST_CASE("the offset grid adds the along-seam shift near and far, fading to zero at its edges", "[seamtools]") {
    // ---- a synthesized grid of the parallax geometry ----------------------------------
    const render::BlendSeam near = fixedSeam(0.35, 1.0f);
    auto g = render::seamOffsetGrid(nullptr, near, 1.0, 0.0);
    REQUIRE(g.ok());
    const render::ParallaxWarpGrid& grid = g.value();
    const render::ParallaxWarpParams defaults;
    REQUIRE(grid.valid());
    CHECK(grid.w == defaults.gridW);
    CHECK(grid.h == defaults.gridRows + 2u * defaults.decayRows);
    CHECK(grid.latMinRad > 0.0f);
    CHECK(grid.latMaxRad < 0.0f);
    const auto at = [&](const render::ParallaxWarpGrid& gg, std::uint32_t x, std::uint32_t y, int comp) {
        return gg.uv[(static_cast<std::size_t>(y) * gg.w + x) * 2u + static_cast<std::size_t>(comp)];
    };
    // SIGN: +1 deg turns the master's content by +0.5 deg, so the master's
    // sampling displacement is -0.5 deg in the core rows.
    const std::uint32_t mid = grid.h / 2;
    for (std::uint32_t x = 0; x < grid.w; x += 17) {
        CHECK_THAT(at(grid, x, mid, 0), Catch::Matchers::WithinAbs(-deg2rad(0.5), 1e-6));
        CHECK(at(grid, x, mid, 1) == 0.0f);        // along the seam only
        CHECK(at(grid, x, 0, 0) == 0.0f);          // zero at the outer rows
        CHECK(at(grid, x, grid.h - 1, 0) == 0.0f);
    }
    // Monotone fade from the core to the edge.
    for (std::uint32_t y = 1; y < mid; ++y) {
        CHECK(std::fabs(at(grid, 5, y, 0)) >= std::fabs(at(grid, 5, y - 1, 0)));
    }

    // ---- near vs far by the seam's weight ------------------------------------------------
    const render::BlendSeam farSeam = fixedSeam(0.35, 0.0f);
    auto nearOnFar = render::seamOffsetGrid(nullptr, farSeam, 1.0, 0.0);
    auto farOnFar = render::seamOffsetGrid(nullptr, farSeam, 0.0, -2.0);
    REQUIRE(nearOnFar.ok());
    REQUIRE(farOnFar.ok());
    CHECK(at(nearOnFar.value(), 3, mid, 0) == 0.0f);  // near content only: nothing here
    CHECK_THAT(at(farOnFar.value(), 3, mid, 0), Catch::Matchers::WithinAbs(deg2rad(1.0), 1e-6));
    render::BlendSeam half = fixedSeam(0.35, 0.5f);
    auto mixed = render::seamOffsetGrid(nullptr, half, 1.0, 3.0);
    REQUIRE(mixed.ok());
    CHECK_THAT(at(mixed.value(), 3, mid, 0), Catch::Matchers::WithinAbs(-deg2rad(0.5 * (0.5 * 1.0 + 0.5 * 3.0)), 1e-6));

    // ---- added to a parallax grid, whose dLat it leaves alone -----------------------------
    render::ParallaxWarpGrid base;
    base.w = 16;
    base.h = 12;
    base.latMinRad = static_cast<float>(deg2rad(9.0));
    base.latMaxRad = static_cast<float>(-deg2rad(9.0));
    base.uv.assign(16u * 12u * 2u, 0.0f);
    for (std::size_t i = 0; i < base.uv.size(); ++i) {
        base.uv[i] = 0.001f * static_cast<float>(i % 7);
    }
    auto added = render::seamOffsetGrid(&base, near, -1.0, 0.0);
    REQUIRE(added.ok());
    REQUIRE(added.value().uv.size() == base.uv.size());
    for (std::size_t i = 1; i < base.uv.size(); i += 2) {
        CHECK(added.value().uv[i] == base.uv[i]);  // dLat untouched
    }
    CHECK(added.value().uv[(6u * 16u + 4u) * 2u] > base.uv[(6u * 16u + 4u) * 2u]);  // -1 deg: +0.5 deg master sampling

    // ---- refusals and clamping --------------------------------------------------------------
    CHECK_FALSE(render::seamOffsetGrid(nullptr, near, std::numeric_limits<double>::quiet_NaN(), 0.0).ok());
    render::BlendSeam broken = near;
    broken.nearWeight.resize(3);
    CHECK_FALSE(render::seamOffsetGrid(nullptr, broken, 1.0, 0.0).ok());
    auto clamped = render::seamOffsetGrid(nullptr, near, 50.0, 0.0);
    REQUIRE(clamped.ok());
    CHECK_THAT(at(clamped.value(), 0, mid, 0), Catch::Matchers::WithinAbs(-deg2rad(0.5 * render::kMaxSeamOffsetDeg), 1e-6));
}

TEST_CASE("a positive offset turns the master's content along the seam, the slave's the other way", "[seamtools]") {
    // The sign on real pixels: a textured scene, each lens alone, with and
    // without +1 deg of Far Offset.  The master's picture must move by
    // +0.5 deg of polar longitude (to the right in the polar map), the
    // slave's by -0.5 deg; nothing moves outside the offset grid's latitudes.
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    const SynthPair scene = synthPair(rig.value(), checker, pool);
    const render::BlendSeam seam = fixedSeam(0.35, 0.0f);
    auto grid = render::seamOffsetGrid(nullptr, seam, 0.0, 1.0);
    REQUIRE(grid.ok());
    render::CpuRenderer cpu(pool);
    const int W = 3600;  // 10 px per degree
    const int H = W / 2;
    for (const int lens : {0, 1}) {
        const render::ImageRGBAf plain = renderPolar(cpu, rig.value(), scene.pair, Config{nullptr, nullptr, 0.0, lens}, W);
        const render::ImageRGBAf moved =
            renderPolar(cpu, rig.value(), scene.pair, Config{nullptr, &grid.value(), 0.0, lens}, W);
        // Search the horizontal shift that best maps `plain` onto `moved` on
        // the row just inside this lens's side of the seam.
        const int y = H / 2 + (lens == 1 ? -10 : 10);
        double bestErr = 1e30;
        int best = 0;
        for (int dx = -12; dx <= 12; ++dx) {
            double err = 0.0;
            for (int x = 200; x < W - 200; ++x) {
                const double a = luma(plain.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)));
                const double b = luma(moved.pixel(static_cast<std::uint32_t>(x + dx), static_cast<std::uint32_t>(y)));
                err += (a - b) * (a - b);
            }
            if (err < bestErr) {
                bestErr = err;
                best = dx;
            }
        }
        INFO("lens " << lens << ": best shift " << best << " px (10 px per degree)");
        CHECK(best == (lens == 1 ? 5 : -5));
        // Far from the seam (beyond the grid) nothing moved.
        const int yFar = lens == 1 ? H / 2 - 150 : H / 2 + 150;
        for (int x = 0; x < W; x += 97) {
            CHECK(std::memcmp(plain.pixel(x, yFar), moved.pixel(x, yFar), 4 * sizeof(float)) == 0);
        }
    }
}

// ===========================================================================
//  The carve's near weight
// ===========================================================================

TEST_CASE("the carve's near weight marks where the lenses disagree and glides between buckets", "[seamtools][seamcarve]") {
    auto rig = makeSyntheticRig(kStream);
    REQUIRE(rig.ok());
    ThreadPool pool(4);
    // A textured scene both lenses agree on, except a patch of longitudes
    // 100..140 deg where lens 1 sees a different, strongly textured object
    // spanning the whole band (something only it sees up close).
    const Radiance scene = [](int lens, const Vec3d& d, double, double rgb[3]) {
        const double lon = rad2deg(polarLon(d));
        const double lat = rad2deg(polarLat(d));
        double v = background(d) + 0.05 * std::sin(deg2rad(lon) * 90.0) * std::sin(deg2rad(lat) * 90.0);
        if (lens == 1 && lon > 100.0 && lon < 140.0) {
            v = (static_cast<int>(std::floor(lon * 2.0) + std::floor(lat * 2.0)) & 1) ? 0.8 : 0.05;
        }
        rgb[0] = rgb[1] = rgb[2] = v;
    };
    const SynthPair pair = synthPair(rig.value(), scene, pool);
    const render::BandParams band = render::ParallaxWarpParams{}.band;
    auto bands = render::renderLensBands(rig.value(), pair.pair, geom::BlendParams{}, band, false, nullptr, pool);
    REQUIRE(bands.ok());
    auto seam = render::carveSeamFromBands(bands.value(), {}, render::SeamCarveParams{}, nullptr, &pool);
    REQUIRE(seam.ok());
    const render::BlendSeam& s = seam.value();
    REQUIRE(s.valid());
    REQUIRE(s.nearWeight.size() == s.columns);
    const auto meanOver = [&](double lo, double hi) {
        double sum = 0.0;
        int n = 0;
        for (std::uint32_t c = 0; c < s.columns; ++c) {
            const double lon = (static_cast<double>(c) + 0.5) * 360.0 / s.columns - 180.0;
            if (lon > lo && lon < hi) {
                sum += s.nearWeight[c];
                ++n;
            }
        }
        return n ? sum / n : 0.0;
    };
    const double inside = meanOver(108.0, 132.0);
    const double outside = meanOver(-150.0, 60.0);
    WARN("near weight: " << inside << " where the lenses disagree, " << outside << " where they agree");
    CHECK(inside > 0.8);
    CHECK(outside < 0.2);
    for (const float w : s.nearWeight) {
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
    }

    // ---- it glides with the seam, exact at the ends ---------------------------------------
    render::BlendSeam a = s;
    render::BlendSeam b = s;
    std::fill(a.nearWeight.begin(), a.nearWeight.end(), 0.0f);
    std::fill(b.nearWeight.begin(), b.nearWeight.end(), 1.0f);
    auto mid = render::blendSeams(a, b, 0.25);
    auto end = render::blendSeams(a, b, 1.0);
    REQUIRE(mid.ok());
    REQUIRE(end.ok());
    CHECK_THAT(mid.value().nearWeight[7], Catch::Matchers::WithinAbs(0.25, 1e-6));
    CHECK(end.value().nearWeight == b.nearWeight);
    // A malformed weight makes the seam invalid.
    render::BlendSeam bad = s;
    bad.nearWeight[3] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(bad.valid());
    bad = s;
    bad.nearWeight.pop_back();
    CHECK_FALSE(bad.valid());
}
