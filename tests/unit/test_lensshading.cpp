// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The per-lens shading correction (include/osv/render/LensShading.h,
// docs/research/NEURAL_STITCHING.md section 9).
//
// Synthetic tests render a known sky into two fisheye frames through the
// sample clip's own calibration (SynthFisheye.h) with a known additive ring
// in the master lens - the structure measured on the sample - and check that
// the estimate recovers it, that the kernel removes it, that texture and a
// horizon do not fool it, that everything off is bit-identical and that the
// temporal filter is deterministic.  The [sample] tests measure the sky band
// on the importer's default stitch through the real kernel, frames 0 / 32 /
// 64, plus stability over all 65 frames, the ground and GPU parity.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "SynthFisheye.h"
#include "TestSample.h"

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/LensShading.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/Renderer.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/render/SeamCarve.h"
#include "osv/video/DualStreamReader.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
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
//  A synthetic sky with a known ring
// ===========================================================================

/// BT.2020 luma weights (the model's luma units).
constexpr double kLuma[3] = {0.2627, 0.6780, 0.0593};

/// A smooth blue sky (native linear): brighter toward one side, a gentle
/// gradient across the seam.  Smooth enough that a quadratic in theta along
/// any meridian describes it to well under a percent.
void skyRgb(const Vec3d& d, double rgb[3]) {
    const double level = 0.14 * (1.0 + 0.25 * std::cos(polarLon(d)) + 0.12 * std::sin(2.0 * polarLat(d)));
    rgb[0] = level * 0.55;
    rgb[1] = level * 0.85;
    rgb[2] = level * 1.60;
}

/// The master lens's ring as measured on the sample: an ADDITIVE deficit
/// centred at 86 deg from the axis, deepest toward one azimuth, a slightly
/// warm colour (the veil's).  Values in scene-linear light.
struct Ring {
    double centreDeg = 86.0;
    double sigmaDeg = 1.6;
    double depth = 0.02;  ///< Luma units at the deepest azimuth.
    double phi0 = 0.6;    ///< Azimuth of the deepest point (radians).
    std::array<double, 3> colour{1.05, 1.0, 0.75};

    /// The colour normalised to unit luma (the model's convention).
    [[nodiscard]] std::array<double, 3> unitColour() const {
        const double l = kLuma[0] * colour[0] + kLuma[1] * colour[1] + kLuma[2] * colour[2];
        return {colour[0] / l, colour[1] / l, colour[2] / l};
    }
    /// Missing light in luma units at lens angle theta and azimuth phi.
    [[nodiscard]] double amount(double thetaDeg, double phi) const {
        const double g = std::exp(-0.5 * std::pow((thetaDeg - centreDeg) / sigmaDeg, 2.0));
        return depth * (0.55 + 0.45 * std::cos(phi - phi0)) * g;
    }
};

/// Azimuth of body direction `d` around lens `lens`'s axis (atan2(y, x) in
/// the lens frame, the model's convention).
double lensPhi(const geom::LensRig& rig, int lens, const Vec3d& d) {
    const Vec3d dl = rig.bodyToLens[static_cast<std::size_t>(lens)] * d;
    return std::atan2(dl.y, dl.x);
}

/// A sky scene, optionally with the ring subtracted from the master lens.
Radiance ringScene(const geom::LensRig& rig, const Ring* ring) {
    return [&rig, ring](int lens, const Vec3d& d, double thetaDeg, double rgb[3]) {
        skyRgb(d, rgb);
        if (ring && lens == 1) {
            const double a = ring->amount(thetaDeg, lensPhi(rig, lens, d));
            const std::array<double, 3> k = ring->unitColour();
            for (int c = 0; c < 3; ++c) {
                rgb[c] = std::max(rgb[c] - k[static_cast<std::size_t>(c)] * a, 1e-4);
            }
        }
    };
}

/// The analysis blend of the synthetic tests (no stick mask: the synthetic
/// frames have no stick).
geom::BlendParams syntheticBlend() {
    geom::BlendParams b;
    b.useOcclusionMask = false;
    return b;
}

/// Polar map geometry.
geom::EquirectMap polarMap(int w) {
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = w;
    map.h = w / 2;
    return map;
}

/// Native-linear colour block (identity matrices), like every band analysis.
OsvColorParams nativeLinear() {
    OsvColorParams linear = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
    for (int k = 0; k < 9; ++k) {
        const float id = (k % 4 == 0) ? 1.0f : 0.0f;
        linear.nativeToWorking.m[k] = id;
        linear.workingToOutput.m[k] = id;
    }
    return linear;
}

/// Rows [row0, row1) of lens `lens` alone over a polar map `w` wide, native
/// linear, with or without `model`.
std::vector<float> lensRows(const geom::LensRig& rig, const video::FramePair& pair, int lens, int w, std::uint32_t row0,
                            std::uint32_t row1, const render::LensShadingModel* model, ThreadPool& pool) {
    render::RenderParamsBuilder b;
    b.rig(rig).equirect(polarMap(w)).blend(syntheticBlend(), true).color(nativeLinear()).alphaCoverage(true);
    b.lensEnabled(lens, true).lensEnabled(1 - lens, false);
    if (model) {
        b.shading(*model, 1.0);
    }
    auto job = b.build(pair);
    REQUIRE(job.ok());
    auto rows = render::shadeJobRows(job.value(), row0, row1, pool);
    REQUIRE(rows.ok());
    return std::move(rows).value();
}

// ===========================================================================
//  Sample clip helpers
// ===========================================================================

struct SampleClip {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
};

Result<SampleClip> openSampleClip() {
    SampleClip s;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleOsv()));
    s.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(s.track, meta::MetadataTrack::load(*s.file));
    OSV_TRY_ASSIGN(s.format, meta::FormatDetector::detect(*s.file, &s.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(s.track.stream()));
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(s.format.streamW), static_cast<int>(s.format.streamH),
                                               static_cast<int>(s.format.sensorW), static_cast<int>(s.format.sensorH),
                                               s.format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(s.rig, geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength,
                                               s.format.digitalFocalLength, geom::ExtrinsicConvention{}));
    return s;
}

/// Least-squares quadratic through (x, y) pairs; false for a singular fit.
bool fitParabola(const std::vector<double>& x, const std::vector<double>& y, double coef[3]) {
    double s[5] = {0, 0, 0, 0, 0};
    double t[3] = {0, 0, 0};
    for (std::size_t i = 0; i < x.size(); ++i) {
        double p = 1.0;
        for (int k = 0; k < 5; ++k) {
            s[k] += p;
            if (k < 3) {
                t[k] += p * y[i];
            }
            p *= x[i];
        }
    }
    const double det = s[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (s[1] * s[4] - s[3] * s[2]) +
                       s[2] * (s[1] * s[3] - s[2] * s[2]);
    if (!(std::fabs(det) > 1e-12)) {
        return false;
    }
    coef[0] = (t[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (t[1] * s[4] - s[3] * t[2]) +
               s[2] * (t[1] * s[3] - s[2] * t[2])) /
              det;
    coef[1] = (s[0] * (t[1] * s[4] - t[2] * s[3]) - t[0] * (s[1] * s[4] - s[3] * s[2]) +
               s[2] * (s[1] * t[2] - t[1] * s[2])) /
              det;
    coef[2] = (s[0] * (s[2] * t[2] - s[3] * t[1]) - s[1] * (s[1] * t[2] - s[2] * t[1]) +
               t[0] * (s[1] * s[3] - s[2] * s[2])) /
              det;
    return true;
}

/// What the eye sees as a band, in millistops: per block of w/128 sky
/// columns, the blended picture's log2 luma along latitude; a quadratic
/// fitted to the sky OUTSIDE a zone (latitudes in the `fit` ranges)
/// interpolates the sky's own trend across it; the deepest point of the
/// picture below that trend inside [evalLo, evalHi]; the median over the
/// blocks.  At the seam (fit on both sides of +-10 deg, evaluate the master
/// side) the master lens's ring scored ~150 on the default stitch; the same
/// statistic in open sky away from the seam is what the sky's own shape
/// scores - the level the seam has to reach to be invisible.
double bandDepthMillistops(const render::MetricBands& b, double colBegin, double colEnd, double fitALo,
                           double fitAHi, double fitBLo, double fitBHi, double evalLo, double evalHi) {
    const std::size_t w = b.w;
    const std::size_t h = b.h;
    const std::size_t block = std::max<std::size_t>(1, w / 128u);
    const std::size_t c0 = static_cast<std::size_t>(colBegin * static_cast<double>(w));
    const std::size_t c1 = static_cast<std::size_t>(colEnd * static_cast<double>(w));
    std::vector<double> depths;
    for (std::size_t cb = c0; cb + block <= c1; cb += block) {
        std::vector<double> xs, ys;
        std::vector<std::pair<double, double>> zone;
        for (std::size_t r = 0; r < h; ++r) {
            double acc = 0.0;
            for (std::size_t c = cb; c < cb + block; ++c) {
                const float* p = b.blend.data() + (r * w + c) * 4u;
                acc += std::log2(std::max(kLuma[0] * p[0] + kLuma[1] * p[1] + kLuma[2] * p[2], 1e-6));
            }
            const double lat = b.latDeg(static_cast<std::uint32_t>(r));
            const double v = acc / static_cast<double>(block);
            if ((lat >= fitALo && lat <= fitAHi) || (lat >= fitBLo && lat <= fitBHi)) {
                xs.push_back(lat);
                ys.push_back(v);
            }
            if (lat >= evalLo && lat <= evalHi) {
                zone.emplace_back(lat, v);
            }
        }
        double coef[3];
        if (xs.size() < 8 || zone.empty() || !fitParabola(xs, ys, coef)) {
            continue;
        }
        double worst = 0.0;
        for (const auto& [lat, v] : zone) {
            worst = std::max(worst, coef[0] + coef[1] * lat + coef[2] * lat * lat - v);
        }
        depths.push_back(worst);
    }
    if (depths.empty()) {
        return 0.0;
    }
    std::nth_element(depths.begin(), depths.begin() + static_cast<std::ptrdiff_t>(depths.size() / 2), depths.end());
    return 1000.0 * depths[depths.size() / 2];
}

/// The importer's default stitch for one frame, with or without the shading
/// correction: parallax grid, carved seam (with the photometric field's rim
/// as its Rim cost), the photometric field (RimAndGain replaces the global
/// gain).  With `shading`, the field is measured on the corrected lenses and
/// the correction is applied - ImporterInstance::applyAnalyses's order.
render::RenderParamsBuilder defaultStitch(const geom::LensRig& rig, const video::FramePair& pair,
                                          const render::LensShadingModel* shading, ThreadPool& pool) {
    const geom::BlendParams analysis;
    render::ParallaxWarpParams pw;
    pw.backend = render::FlowBackendKind::Classical;
    auto grid = render::buildParallaxWarp(rig, pair, analysis, pw, nullptr, pool);
    REQUIRE(grid.ok());
    render::WarpGridView view;
    view.uv = grid.value().uv.data();
    view.w = grid.value().w;
    view.h = grid.value().h;
    view.latMinRad = grid.value().latMinRad;
    view.latMaxRad = grid.value().latMaxRad;
    render::SeamCorrection correction;
    correction.warp = &view;
    const render::PhotoSeamParams P;
    auto field = render::measurePhotoSeam(rig, pair, analysis, P, pool, shading);
    REQUIRE(field.ok());
    const auto shared = std::make_shared<const render::PhotoSeamField>(field.value());
    render::installPhotoRimPenaltyHook();
    Result<render::BlendSeam> carved = [&] {
        const render::PhotoRimPenaltyScope scope(&rig, shared);
        return render::carveSeam(rig, pair, analysis, pw.band, correction, render::SeamCarveParams{}, nullptr, pool);
    }();
    REQUIRE(carved.ok());
    render::RenderParamsBuilder b;
    b.rig(rig).blend(analysis, true);
    b.warp(grid.value().uv, grid.value().w, grid.value().h, grid.value().latMinRad, grid.value().latMaxRad);
    render::applyBlendSeam(b, carved.value());
    b.photo(*shared, P).gain(Vec3d{1, 1, 1}, Vec3d{1, 1, 1});
    if (shading) {
        b.shading(*shading, 1.0);
    }
    return b;
}

}  // namespace

// ===========================================================================
//  Synthetic: recovery, removal, robustness
// ===========================================================================

TEST_CASE("lensShadingFromBands recovers a known additive ring in one lens", "[lensshading]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    const Ring ring;
    const SynthPair synth = synthPair(rig.value(), ringScene(rig.value(), &ring), pool);
    const render::LensShadingParams P;
    auto model = render::measureLensShading(rig.value(), synth.pair, syntheticBlend(), P, pool);
    INFO((model.ok() ? std::string("measured") : model.error().message));
    REQUIRE(model.ok());
    const render::LensShadingModel& m = model.value();
    REQUIRE(m.valid());
    REQUIRE(m.active());
    WARN("synthetic ring: master peak " << m.lens[1].peakAmount << " at " << m.lens[1].peakThetaDeg << " deg ("
                                        << m.lens[1].peakStops << " stop), " << m.lens[1].measuredSectors
                                        << " sectors, colour " << m.lens[1].colour[0] << "/" << m.lens[1].colour[1]
                                        << "/" << m.lens[1].colour[2] << "; slave peak " << m.lens[0].peakAmount << "; "
                                        << m.bandMs << " + " << m.statsMs << " ms");

    // ---- the master's table against the truth, over the ring zone -------------
    double errSq = 0.0;
    double truthSq = 0.0;
    std::size_t n = 0;
    for (double th = 80.0; th <= 92.0; th += 0.5) {
        for (int s = 0; s < 48; ++s) {
            const double phi = -kPi + (s + 0.5) * kTwoPi / 48.0;
            const double got = render::lensShadingTableAt(m, 1, deg2rad(th), phi);
            const double want = ring.amount(th, phi);
            REQUIRE(std::isfinite(got));
            errSq += (got - want) * (got - want);
            truthSq += want * want;
            ++n;
        }
    }
    const double relRms = std::sqrt(errSq / truthSq);
    INFO("relative RMS error of the recovered ring " << relRms << " over " << n << " samples");
    CHECK(relRms < 0.2);
    // The deepest point where it is, and about as deep.
    CHECK(std::fabs(m.lens[1].peakThetaDeg - ring.centreDeg) <= 1.0);
    CHECK(std::fabs(m.lens[1].peakAmount - ring.depth) <= 0.25 * ring.depth);
    // The colour of the deficit (normalised to unit luma).
    const std::array<double, 3> k = ring.unitColour();
    for (int c = 0; c < 3; ++c) {
        CHECK(std::fabs(m.lens[1].colour[static_cast<std::size_t>(c)] - k[static_cast<std::size_t>(c)]) < 0.12);
    }
    // The slave has no ring: its table stays within the noise of the sky.
    double slaveWorst = 0.0;
    for (const float v : m.lens[0].amount) {
        slaveWorst = std::max(slaveWorst, static_cast<double>(std::fabs(v)));
    }
    INFO("slave worst |amount| " << slaveWorst);
    CHECK(slaveWorst < 0.1 * ring.depth);
    // The rank-2 kernel form of a rank-1 ring is as good as the table.
    CHECK(render::lensShadingSeparableError(m) < 0.05 * ring.depth);
}

TEST_CASE("the kernel adds the ring back: the corrected lens matches a ring-free render", "[lensshading]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    const Ring ring;
    const SynthPair ringed = synthPair(rig.value(), ringScene(rig.value(), &ring), pool);
    const SynthPair clean = synthPair(rig.value(), ringScene(rig.value(), nullptr), pool);
    auto model =
        render::measureLensShading(rig.value(), ringed.pair, syntheticBlend(), render::LensShadingParams{}, pool);
    REQUIRE(model.ok());
    // The master lens alone over theta 80..92 deg (polar lat -2..10).
    const int w = 1024;
    const std::uint32_t row0 = 256 - 29;  // lat +10.2
    const std::uint32_t row1 = 256 + 6;   // lat -2.1
    const std::vector<float> truth = lensRows(rig.value(), clean.pair, 1, w, row0, row1, nullptr, pool);
    const std::vector<float> before = lensRows(rig.value(), ringed.pair, 1, w, row0, row1, nullptr, pool);
    const std::vector<float> after = lensRows(rig.value(), ringed.pair, 1, w, row0, row1, &model.value(), pool);
    double eBefore = 0.0;
    double eAfter = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < truth.size() / 4u; ++i) {
        if (truth[i * 4u + 3u] < 0.99f) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const double t = truth[i * 4u + static_cast<std::size_t>(c)];
            eBefore += std::pow(before[i * 4u + static_cast<std::size_t>(c)] - t, 2.0);
            eAfter += std::pow(after[i * 4u + static_cast<std::size_t>(c)] - t, 2.0);
        }
        ++n;
    }
    REQUIRE(n > 1000);
    const double rmsBefore = std::sqrt(eBefore / static_cast<double>(3 * n));
    const double rmsAfter = std::sqrt(eAfter / static_cast<double>(3 * n));
    WARN("master lens vs the ring-free render, theta 80-92: RMS " << rmsBefore << " -> " << rmsAfter << " linear");
    CHECK(rmsAfter < 0.2 * rmsBefore);
}

TEST_CASE("texture and a horizon do not turn into a correction", "[lensshading]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    const render::LensShadingParams P;
    SECTION("a textured world: no sky, no correction") {
        const SynthPair synth = synthPair(
            rig.value(),
            [](int, const Vec3d& d, double, double rgb[3]) {
                // Ground-like texture: several octaves of high-frequency
                // pattern on a mid level, the same in both lenses.
                const double t = 0.5 + 0.25 * std::sin(90.0 * polarLon(d)) * std::cos(70.0 * polarLat(d)) +
                                 0.15 * std::sin(233.0 * d.x + 17.0 * d.z);
                for (int c = 0; c < 3; ++c) {
                    rgb[c] = 0.12 * std::max(t, 0.05) * (c == 2 ? 0.8 : 1.0);
                }
            },
            pool);
        auto model = render::measureLensShading(rig.value(), synth.pair, syntheticBlend(), P, pool);
        REQUIRE(model.ok());
        INFO("sky columns " << model.value().lens[0].skyColumns << " / " << model.value().lens[1].skyColumns);
        CHECK_FALSE(model.value().active());
    }
    SECTION("a sharp horizon at 90 deg from the master axis: the correction stays small") {
        // For the master lens every meridian crosses the horizon at the same
        // angle - a perfect ring-shaped step, the worst case for a radial
        // estimator.  It must not be "corrected".
        const SynthPair synth = synthPair(
            rig.value(),
            [](int, const Vec3d& d, double, double rgb[3]) {
                skyRgb(d, rgb);
                if (polarLat(d) < deg2rad(-0.5)) {
                    rgb[0] *= 0.5;
                    rgb[1] *= 0.35;
                    rgb[2] *= 0.2;
                }
            },
            pool);
        auto model = render::measureLensShading(rig.value(), synth.pair, syntheticBlend(), P, pool);
        REQUIRE(model.ok());
        double worstRel = 0.0;
        for (int lens = 0; lens < 2; ++lens) {
            for (double th = 78.0; th <= 94.0; th += 0.25) {
                for (int s = 0; s < 24; ++s) {
                    const double phi = -kPi + (s + 0.5) * kTwoPi / 24.0;
                    const double a = render::lensShadingTableAt(model.value(), lens, deg2rad(th), phi);
                    // Relative to the (darker) ground level, the stricter side.
                    worstRel = std::max(worstRel, std::fabs(a) / (0.14 * 0.4));
                }
            }
        }
        INFO("largest |amount| relative to the ground level: " << worstRel);
        CHECK(worstRel < 0.1);
    }
}

// ===========================================================================
//  Kernel: off is off, the separable form, passthrough, the budget
// ===========================================================================

TEST_CASE("without a correction every render is bit-identical", "[lensshading]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(512);
    REQUIRE(rig.ok());
    const Ring ring;
    const SynthPair synth = synthPair(rig.value(), ringScene(rig.value(), &ring), pool);
    render::CpuRenderer cpu(pool);
    geom::VirtualCamera cam;
    cam.w = 320;
    cam.h = 180;
    cam.hfovDeg = 120.0;
    cam.yawDeg = 80.0;
    const OsvColorParams pq = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    render::RenderParamsBuilder base;
    base.rig(rig.value()).camera(cam).blend(syntheticBlend(), true).color(pq);
    auto baseParams = base.buildParams();
    REQUIRE(baseParams.ok());
    REQUIRE(baseParams.value().shadeEnabled == 0);

    // An inactive model, a zero strength and a cleared correction all leave
    // the block exactly as it was.
    const render::LensShadingModel empty = render::emptyLensShadingModel(render::LensShadingParams{});
    render::LensShadingModel active = empty;
    active.lens[1].amount[20u * OSV_SHADE_PHI_N + 3u] = 0.01f;
    REQUIRE(active.active());
    for (int variant = 0; variant < 3; ++variant) {
        render::RenderParamsBuilder b = base;
        if (variant == 0) {
            b.shading(empty, 1.0);
        } else if (variant == 1) {
            b.shading(active, 0.0);
        } else {
            b.shading(active, 1.0).clearShading();
        }
        auto params = b.buildParams();
        REQUIRE(params.ok());
        CHECK(std::memcmp(&params.value(), &baseParams.value(), sizeof(OsvRenderParams)) == 0);
    }
    // A block that carries factors but is switched off renders exactly like
    // no block at all: the kernel never reads them.
    auto jobOff = base.build(synth.pair);
    REQUIRE(jobOff.ok());
    render::RenderParamsBuilder withModel = base;
    withModel.shading(active, 1.0);
    auto jobOn = withModel.build(synth.pair);
    REQUIRE(jobOn.ok());
    REQUIRE(jobOn.value().params.shadeEnabled == 1);
    render::RenderJob disabled = jobOn.value();
    disabled.params.shadeEnabled = 0;
    auto a = cpu.render(jobOff.value());
    auto b = cpu.render(disabled);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.value().data.size() == b.value().data.size());
    CHECK(std::memcmp(a.value().data.data(), b.value().data.data(), a.value().data.size() * sizeof(float)) == 0);
}

TEST_CASE("the kernel's separable block reproduces the measured table", "[lensshading]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    const Ring ring;
    const SynthPair synth = synthPair(rig.value(), ringScene(rig.value(), &ring), pool);
    auto model =
        render::measureLensShading(rig.value(), synth.pair, syntheticBlend(), render::LensShadingParams{}, pool);
    REQUIRE(model.ok());
    OsvRenderParams p;
    std::memset(&p, 0, sizeof(p));
    REQUIRE(render::fillLensShadingBlock(model.value(), 1.0, p));
    // Fill the lens blocks the kernel reads for the azimuth (cx, cy, fx, fy).
    render::RenderParamsBuilder b;
    b.rig(rig.value()).equirect(polarMap(512)).color(nativeLinear());
    auto full = b.buildParams();
    REQUIRE(full.ok());
    p.lens[0] = full.value().lens[0];
    p.lens[1] = full.value().lens[1];
    const double sepErr = render::lensShadingSeparableError(model.value());
    double worst = 0.0;
    for (double th = 77.0; th <= 96.0; th += 0.37) {
        for (int s = 0; s < 36; ++s) {
            const double phi = -kPi + (s + 0.25) * kTwoPi / 36.0;
            // The lens pixel the kernel would see for that (theta, phi).
            const geom::KannalaBrandt5& kb = rig.value().lens[1];
            const Vec3d dl{std::sin(deg2rad(th)) * std::cos(phi), std::sin(deg2rad(th)) * std::sin(phi),
                           std::cos(deg2rad(th))};
            Vec2d px;
            double thetaLens = 0.0;
            if (!kb.project(dl, px, thetaLens)) {
                continue;
            }
            const float amount = osvShadeAmount(&p, 1, static_cast<float>(deg2rad(th)), static_cast<float>(px.x),
                                                static_cast<float>(px.y));
            const double table = render::lensShadingTableAt(model.value(), 1, deg2rad(th), phi);
            worst = std::max(worst, std::fabs(static_cast<double>(amount) - table));
        }
    }
    INFO("kernel vs table: worst " << worst << ", separable error at the knots " << sepErr);
    CHECK(worst <= sepErr + 1e-4);
    // At or below the anchor the kernel adds nothing, whatever the table says.
    CHECK(osvShadeAmount(&p, 1, static_cast<float>(deg2rad(75.9)), 100.0f, 100.0f) == 0.0f);
    CHECK(osvShadeAmount(&p, 1, std::numeric_limits<float>::quiet_NaN(), 100.0f, 100.0f) == 0.0f);
    p.shadeEnabled = 0;
    CHECK(osvShadeAmount(&p, 1, static_cast<float>(deg2rad(86.0)), 100.0f, 100.0f) == 0.0f);
}

TEST_CASE("passthrough output moves each code to the code of its light plus the amount", "[lensshading]") {
    for (const int encoding : {OSV_INPUT_DLOGM, OSV_INPUT_HLG, OSV_INPUT_REC709_NORMAL}) {
        OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Passthrough, 0.0f);
        cp.inputEncoding = encoding;
        for (const float code : {0.25f, 0.4f, 0.55f, 0.7f}) {
            for (const float add : {0.0f, 0.004f, 0.02f, -0.003f}) {
                const float out = osvShadeCodeAdd(&cp, code, add);
                const float c3[3] = {code, code, code};
                const float o3[3] = {out, out, out};
                float lin[3];
                float linOut[3];
                osvCodeToLinear(&cp, c3, lin);
                osvCodeToLinear(&cp, o3, linOut);
                INFO("encoding " << encoding << ", code " << code << ", add " << add);
                CHECK_THAT(linOut[1], Catch::Matchers::WithinAbs(static_cast<double>(lin[1] + add),
                                                                 2e-4 * std::max(1.0, static_cast<double>(lin[1]))));
            }
        }
    }
}

TEST_CASE("the parameter block keeps the 4 KB kernel budget with the shading block", "[lensshading]") {
    static_assert(sizeof(OsvRenderParams) <= 4096, "OsvRenderParams must stay within the 4 KB kernel budget");
    static_assert(sizeof(OsvShadeLens) == (3 + OSV_SHADE_RANK * (OSV_SHADE_THETA_N + OSV_SHADE_PHI_N)) * 4,
                  "OsvShadeLens is plain floats");
    // The direct kernel passes the block plus its planes, six pointers and
    // two ints (tests/premiere/reframe/test_direct.cpp checks the same).
    CHECK(sizeof(OsvRenderParams) + 2 * sizeof(OsvPlane) + 6 * sizeof(void*) + 2 * sizeof(int) <= 4096);
}

// ===========================================================================
//  Time
// ===========================================================================

TEST_CASE("blendLensShadingModels: exact endpoints, layout checks, cell-wise glide", "[lensshading]") {
    const render::LensShadingParams P;
    render::LensShadingModel a = render::emptyLensShadingModel(P);
    render::LensShadingModel b = a;
    b.lens[1].amount[5u * OSV_SHADE_PHI_N + 2u] = 0.02f;
    b.lens[1].sectorMeasured[2] = 1;
    b.lens[1].colour = {1.2f, 1.0f, 0.8f};
    auto t0 = render::blendLensShadingModels(a, b, 0.0);
    auto t1 = render::blendLensShadingModels(a, b, 1.0);
    auto th = render::blendLensShadingModels(a, b, 0.25);
    REQUIRE(t0.ok());
    REQUIRE(t1.ok());
    REQUIRE(th.ok());
    CHECK(t0.value().lens[1].amount == a.lens[1].amount);
    CHECK(t1.value().lens[1].amount == b.lens[1].amount);
    CHECK_THAT(th.value().lens[1].amount[5u * OSV_SHADE_PHI_N + 2u], Catch::Matchers::WithinAbs(0.005, 1e-7));
    CHECK(th.value().lens[1].sectorMeasured[2] == 1);
    CHECK_FALSE(render::blendLensShadingModels(a, b, std::numeric_limits<double>::quiet_NaN()).ok());
    render::LensShadingModel other = b;
    other.dThetaRad *= 2.0f;
    CHECK_FALSE(render::blendLensShadingModels(a, other, 0.5).ok());
    render::LensShadingModel broken = b;
    broken.lens[0].amount.pop_back();
    CHECK_FALSE(broken.valid());
    CHECK_FALSE(render::blendLensShadingModels(a, broken, 0.5).ok());
}

TEST_CASE("LensShadingHistory: EMA, glide, refusal, and a frame renders the same whatever came after",
          "[lensshading]") {
    const render::LensShadingParams P;
    const auto modelWith = [&P](float v) {
        render::LensShadingModel m = render::emptyLensShadingModel(P);
        for (std::size_t s = 0; s < OSV_SHADE_PHI_N; ++s) {
            m.lens[1].amount[10u * OSV_SHADE_PHI_N + s] = v;
            m.lens[1].sectorMeasured[s] = 1;
        }
        return std::make_shared<const render::LensShadingModel>(std::move(m));
    };
    const std::size_t cell = 10u * OSV_SHADE_PHI_N + 4u;
    render::LensShadingHistory h;
    h.store(0, modelWith(0.02f), P);
    REQUIRE(h.measured(0));
    // The first bucket: as measured (no EMA partner), no glide.
    CHECK(h.modelFor(3, P)->lens[1].amount[cell] == 0.02f);
    // The second: EMA against the first, then a glide inside the bucket.
    h.store(1, modelWith(0.01f), P);
    const float ema = 0.02f + (0.01f - 0.02f) * static_cast<float>(P.temporalAlpha);
    CHECK_THAT(h.modelFor(15, P)->lens[1].amount[cell], Catch::Matchers::WithinAbs(ema, 1e-7));
    const float glide8 = h.modelFor(8, P)->lens[1].amount[cell];
    CHECK(glide8 > ema);
    CHECK(glide8 < 0.02f);
    // A frame renders the same after a later bucket is stored.
    const float before = h.modelFor(12, P)->lens[1].amount[cell];
    h.store(2, modelWith(0.03f), P);
    h.store(5, modelWith(0.04f), P);
    CHECK(h.modelFor(12, P)->lens[1].amount[cell] == before);
    // A refusal renders without, and is not measured again.
    h.store(3, nullptr, P);
    CHECK(h.measured(3));
    CHECK(h.modelFor(27, P) == nullptr);
    // Trim keeps the requested bucket.
    h.trim(2, 1);
    CHECK(h.size() == 2);
    CHECK(h.measured(1));
    h.clear();
    CHECK(h.size() == 0);
}

// ===========================================================================
//  Sample clip
// ===========================================================================

TEST_CASE("the sample's master lens carries a rim ring the slave does not", "[lensshading][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());
        auto model =
            render::measureLensShading(clip.value().rig, pair.value(), analysis, render::LensShadingParams{}, pool);
        REQUIRE(model.ok());
        const render::LensShadingModel& m = model.value();
        WARN("frame " << frame << ": master peak " << m.lens[1].peakAmount << " linear, " << m.lens[1].peakStops
                      << " stop at " << m.lens[1].peakThetaDeg << " deg / phi " << m.lens[1].peakPhiDeg << ", "
                      << m.lens[1].measuredSectors << " sectors, colour " << m.lens[1].colour[0] << "/"
                      << m.lens[1].colour[1] << "/" << m.lens[1].colour[2] << "; slave peak " << m.lens[0].peakAmount
                      << " (" << m.lens[0].peakStops << " stop at " << m.lens[0].peakThetaDeg << " deg); analysis "
                      << m.bandMs << " + " << m.statsMs << " ms");
        REQUIRE(m.active());
        CHECK(m.lens[1].peakThetaDeg >= 84.5);
        CHECK(m.lens[1].peakThetaDeg <= 88.0);
        CHECK(m.lens[1].peakStops < -0.15);
        CHECK(m.lens[1].peakStops > -0.45);
        // Neutral in linear light: the three channels within a factor 1.6.
        CHECK(m.lens[1].colour[2] > 0.6f);
        CHECK(m.lens[1].colour[0] < 1.6f);
        // The slave has no ring: over the master's ring zone and beyond, its
        // table stays at the level of a smooth sky's misfit (measured: 2-3 %
        // of the sky, 0.004 linear, against the master's 0.022).
        double slaveWorst = 0.0;
        for (double th = 80.0; th <= 91.0; th += 0.5) {
            for (int s = 0; s < 24; ++s) {
                const double phi = -kPi + (s + 0.5) * kTwoPi / 24.0;
                slaveWorst = std::max(slaveWorst, std::fabs(render::lensShadingTableAt(m, 0, deg2rad(th), phi)));
            }
        }
        CHECK(slaveWorst < 0.25 * m.lens[1].peakAmount);
    }
}

TEST_CASE("the sky band at the seam crossings disappears on the default stitch", "[lensshading][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    const render::MetricBandRequest req;  // 2048 columns, +-30 deg
    double depthOff = 0.0;
    double depthOn = 0.0;
    double refOff = 0.0;
    double refOn = 0.0;
    std::array<double, 4> sumOff{}, sumOn{};
    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());
        auto model =
            render::measureLensShading(clip.value().rig, pair.value(), analysis, render::LensShadingParams{}, pool);
        REQUIRE(model.ok());
        const render::RenderParamsBuilder off = defaultStitch(clip.value().rig, pair.value(), nullptr, pool);
        const render::RenderParamsBuilder on = defaultStitch(clip.value().rig, pair.value(), &model.value(), pool);
        auto offBands = render::renderMetricBands(off, pair.value(), req, pool);
        auto onBands = render::renderMetricBands(on, pair.value(), req, pool);
        REQUIRE(offBands.ok());
        REQUIRE(onBands.ok());
        // The band: the master side of the seam, lat -2..8 (theta 82-92),
        // against the sky's trend from both sides of +-10 deg.
        const double dOff = bandDepthMillistops(offBands.value(), 0.20, 0.44, -22.0, -10.0, 10.0, 22.0, -2.0, 8.0);
        const double dOn = bandDepthMillistops(onBands.value(), 0.20, 0.44, -22.0, -10.0, 10.0, 22.0, -2.0, 8.0);
        // The same statistic in open sky away from the seam, on the slave's
        // side (which carries no ring, so the correction leaves it alone):
        // what the sky's own shape scores.
        const auto openSky = [](const render::MetricBands& m) {
            return bandDepthMillistops(m, 0.20, 0.44, -29.0, -24.0, -11.0, -4.0, -23.0, -12.0);
        };
        const double rOff = openSky(offBands.value());
        const double rOn = openSky(onBands.value());
        // The section 1.4 metrics on one trust mask per frame.
        const std::vector<std::uint8_t> trust = render::coValidTrustMask(offBands.value());
        auto mOff = render::skySeamMetrics(offBands.value(), trust, 0.20, 0.44);
        auto mOn = render::skySeamMetrics(onBands.value(), trust, 0.20, 0.44);
        REQUIRE(mOff.ok());
        REQUIRE(mOn.ok());
        const std::array<double, 4> a{mOff.value().line, mOff.value().band, mOff.value().broad, mOff.value().dE};
        const std::array<double, 4> b{mOn.value().line, mOn.value().band, mOn.value().broad, mOn.value().dE};
        WARN("frame " << frame << " default stitch, without -> with lens shading: band depth at the seam " << dOff
                      << " -> " << dOn << " millistops (open sky away from the seam: " << rOff << " -> " << rOn
                      << "); line " << a[0] << " -> " << b[0] << ", band " << a[1] << " -> " << b[1] << ", broad "
                      << a[2] << " -> " << b[2] << ", dE " << a[3] << " -> " << b[3]);
        depthOff += dOff;
        depthOn += dOn;
        refOff += rOff;
        refOn += rOn;
        for (std::size_t k = 0; k < 4; ++k) {
            sumOff[k] += a[k];
            sumOn[k] += b[k];
        }
    }
    WARN("frames 0/32/64: band depth " << depthOff / 3.0 << " -> " << depthOn / 3.0 << " millistops (sky away from "
                                       << "the seam " << refOff / 3.0 << " -> " << refOn / 3.0 << "); line x"
                                       << sumOn[0] / sumOff[0] << ", band x" << sumOn[1] / sumOff[1] << ", broad x"
                                       << sumOn[2] / sumOff[2] << ", dE x" << sumOn[3] / sumOff[3]);
    // The band: gone to the level of the sky's own shape away from the seam.
    CHECK(depthOn <= 0.25 * depthOff);
    CHECK(depthOn <= refOn + 10.0);
    // And no seam metric gets worse.
    CHECK(sumOn[0] <= 1.05 * sumOff[0]);
    CHECK(sumOn[1] <= sumOff[1]);
    CHECK(sumOn[2] <= sumOff[2]);
    CHECK(sumOn[3] <= 1.05 * sumOff[3]);
}

TEST_CASE("the photometric field and the exposure match stay consistent on the corrected lenses",
          "[lensshading][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    auto pair = reader.value().read(32);
    REQUIRE(pair.ok());
    auto model =
        render::measureLensShading(clip.value().rig, pair.value(), analysis, render::LensShadingParams{}, pool);
    REQUIRE(model.ok());
    const render::PhotoSeamParams P;
    auto raw = render::measurePhotoSeam(clip.value().rig, pair.value(), analysis, P, pool);
    auto fixed = render::measurePhotoSeam(clip.value().rig, pair.value(), analysis, P, pool, &model.value());
    auto gRaw = render::estimateGain(clip.value().rig, pair.value(), analysis, render::BandParams{}, pool);
    auto gFixed =
        render::estimateGain(clip.value().rig, pair.value(), analysis, render::BandParams{}, pool, &model.value());
    REQUIRE(raw.ok());
    REQUIRE(fixed.ok());
    REQUIRE(gRaw.ok());
    REQUIRE(gFixed.ok());
    WARN("photometric field without -> with lens shading: median gain "
         << raw.value().medianLog2Gain[1] << " -> " << fixed.value().medianLog2Gain[1] << " stop, usable rim "
         << raw.value().rimMedianDeg[0] << "/" << raw.value().rimMedianDeg[1] << " -> " << fixed.value().rimMedianDeg[0]
         << "/" << fixed.value().rimMedianDeg[1] << " deg, trusted " << raw.value().trustedPixels << " -> "
         << fixed.value().trustedPixels << "; exposure match slave G gain " << gRaw.value().gain[0].y << " -> "
         << gFixed.value().gain[0].y);
    // The ring sits mostly outside what the field trusts, so its lens ratio
    // moves by little; the gain must not swing, and neither may the rims.
    CHECK(std::fabs(fixed.value().medianLog2Gain[1] - raw.value().medianLog2Gain[1]) < 0.08);
    CHECK(std::fabs(fixed.value().rimMedianDeg[1] - raw.value().rimMedianDeg[1]) < 1.0);
    CHECK(std::fabs(std::log2(gFixed.value().gain[0].y / gRaw.value().gain[0].y)) < 0.05);
}

TEST_CASE("the correction is stable in time over the whole clip", "[lensshading][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    const render::LensShadingParams P;
    render::LensShadingHistory history;
    std::shared_ptr<const render::LensShadingModel> previous;
    double worst = 0.0;
    double peak = 0.0;
    std::uint32_t worstFrame = 0;
    double worstMs = 0.0;
    const std::uint32_t frames = std::min<std::uint32_t>(65u, reader.value().frameCount());
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());
        const std::uint32_t bucket = render::parallaxBucket(frame);
        if (!history.measured(bucket)) {
            auto model = render::measureLensShading(clip.value().rig, pair.value(), analysis, P, pool);
            REQUIRE(model.ok());
            worstMs = std::max(worstMs, model.value().bandMs + model.value().statsMs);
            history.store(bucket, std::make_shared<const render::LensShadingModel>(std::move(model).value()), P);
        }
        auto applied = history.modelFor(frame, P);
        REQUIRE(applied);
        for (const float v : applied->lens[1].amount) {
            peak = std::max(peak, static_cast<double>(std::fabs(v)));
        }
        if (previous) {
            for (int lens = 0; lens < 2; ++lens) {
                for (std::size_t q = 0; q < applied->lens[lens].amount.size(); ++q) {
                    const double d = std::fabs(static_cast<double>(applied->lens[lens].amount[q]) -
                                               static_cast<double>(previous->lens[lens].amount[q]));
                    if (d > worst) {
                        worst = d;
                        worstFrame = frame;
                    }
                }
            }
        }
        previous = applied;
    }
    WARN("over " << frames << " frames: largest per-cell change " << worst << " linear between consecutive frames "
                 << "(frame " << worstFrame << "), peak amount " << peak << "; slowest analysis " << worstMs
                 << " ms per bucket (host frames, CPU; shared machine)");
    // A tenth of the ring's depth per frame at most: the band cannot flicker.
    CHECK(worst <= 0.1 * peak);
}

TEST_CASE("the correction does no harm on the ground", "[lensshading][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    const render::MetricBandRequest req;
    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());
        auto model =
            render::measureLensShading(clip.value().rig, pair.value(), analysis, render::LensShadingParams{}, pool);
        REQUIRE(model.ok());
        render::RenderParamsBuilder off;
        off.rig(clip.value().rig).blend(analysis, true);
        render::RenderParamsBuilder on = off;
        on.shading(model.value(), 1.0);
        auto a = render::renderMetricBands(off, pair.value(), req, pool);
        auto b = render::renderMetricBands(on, pair.value(), req, pool);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        // Ground columns 0.54w-0.83w (the research's), every row of the band:
        // the largest change of any pixel, relative to its level.
        const std::size_t w = a.value().w;
        double worst = 0.0;
        for (std::size_t r = 0; r < a.value().h; ++r) {
            for (std::size_t c = static_cast<std::size_t>(0.54 * w); c < static_cast<std::size_t>(0.83 * w); ++c) {
                const float* p = a.value().blend.data() + (r * w + c) * 4u;
                const float* q = b.value().blend.data() + (r * w + c) * 4u;
                const double l = kLuma[0] * p[0] + kLuma[1] * p[1] + kLuma[2] * p[2];
                const double m = kLuma[0] * q[0] + kLuma[1] * q[1] + kLuma[2] * q[2];
                if (l > 1e-3) {
                    worst = std::max(worst, std::fabs(std::log2(m / l)));
                }
            }
        }
        WARN("frame " << frame << ": largest change on the ground " << worst << " stop");
        CHECK(worst < 0.02);
    }
}

TEST_CASE("the corrected stitch renders the same on the CPU, CUDA and OpenCL", "[lensshading][sample][cuda][opencl]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    auto pair = reader.value().read(32);
    REQUIRE(pair.ok());
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    const geom::BlendParams analysis;
    auto model =
        render::measureLensShading(clip.value().rig, pair.value(), analysis, render::LensShadingParams{}, pool);
    REQUIRE(model.ok());
    REQUIRE(model.value().active());
    const OsvColorParams pq = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    const OsvColorParams log =
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Passthrough, 0.0f);
    geom::VirtualCamera cam;
    cam.w = 1280;
    cam.h = 720;
    cam.hfovDeg = 100.0;
    cam.yawDeg = -90.0;  // the seam crossing in the sky
    std::vector<render::RenderJob> jobs;
    for (const OsvColorParams& cp : {pq, log}) {
        for (const bool equirect : {false, true}) {
            render::RenderParamsBuilder b;
            b.rig(clip.value().rig).color(cp).blend(analysis, true).shading(model.value(), 1.0);
            if (equirect) {
                b.equirect(polarMap(1024));
            } else {
                b.camera(cam);
            }
            auto job = b.build(pair.value());
            REQUIRE(job.ok());
            REQUIRE(job.value().params.shadeEnabled == 1);
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
    for (const render::RenderJob& job : jobs) {
        auto ref = cpu.render(job);
        REQUIRE(ref.ok());
        for (auto& [name, gpu] : gpus) {
            auto test = gpu->render(job);
            REQUIRE(test.ok());
            const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
            WARN(name << " vs CPU with the lens shading correction (" << job.params.outW << "x" << job.params.outH
                      << ", transfer " << job.params.color.transfer << "): PSNR " << stats.psnrDb << " dB, max "
                      << stats.maxAbsCode << " codes");
            CHECK(stats.psnrDb >= 60.0);
        }
    }
}
