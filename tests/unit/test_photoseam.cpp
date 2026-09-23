// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Photometric seam fix (docs/research/NEURAL_STITCHING.md, section 8).
//
// Stage 1: the render-only blend inset and the trusted-pixel gain.
// Stage 2: the per-longitude usable rim and the 2-D log-gain field, in the
// kernel (osvShadePixelWSP) and through time (PhotoSeamHistory); the tests
// of NEURAL_STITCHING.md section 8.4.
//
// The synthetic tests render a known scene into two fisheye frames through
// the sample clip's own calibration (no clip needed): a smooth sky, a lens
// gain that is known exactly, and - where a test asks for it - a darkened
// rim on lens 0 like the one measured on the sample.  The [sample] tests
// measure the section 1.4 sky metrics on bands rendered through the real
// kernel, frames 0 / 32 / 64.

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
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
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
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

using namespace osv;

namespace {

// The synthetic dual-fisheye frames (makeSyntheticRig, synthPair, CodeEncoder,
// polarLon / polarLat ...) live in SynthFisheye.h, shared with the seam tools
// tests (test_seam_tools.cpp).
using namespace osv::testsynth;

/// A smooth, bright, sky-like neutral radiance (linear, grey ~0.35).
double skyLevel(const Vec3d& d) {
    return 0.35 * (1.0 + 0.25 * std::cos(polarLon(d)) + 0.15 * std::sin(2.0 * polarLat(d)));
}

// ===========================================================================
//  Sample clip
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

/// Ratio helper for the report lines.
double ratio(double after, double before) { return before > 0.0 ? after / before : 0.0; }

}  // namespace

// ===========================================================================
//  Stage 1
// ===========================================================================

TEST_CASE("insetRenderBlend: zero inset is the analysis blend bit for bit", "[photoseam]") {
    geom::BlendParams a;  // 195.18 / 4, the importer's analysis blend
    a.occlusionFeatherPx = 17.0;
    a.seamShiftDeg = 0.25;

    for (const double inset : {0.0, -1.0, std::nan(""), -std::numeric_limits<double>::infinity()}) {
        const geom::BlendParams b = render::insetRenderBlend(a, inset);
        // Field by field AND byte for byte: "no inset" must render exactly as
        // before the render blend existed.
        CHECK(b.lensFovDeg == a.lensFovDeg);
        CHECK(b.featherDeg == a.featherDeg);
        CHECK(b.occlusionFeatherPx == a.occlusionFeatherPx);
        CHECK(b.seamShiftDeg == a.seamShiftDeg);
        CHECK(b.useOcclusionMask == a.useOcclusionMask);
    }

    SECTION("the default inset ends the render weight at 95 deg with a 3 deg feather") {
        const geom::BlendParams b = render::insetRenderBlend(a, render::kDefaultSeamInsetDeg);
        CHECK_THAT(b.lensFovDeg, Catch::Matchers::WithinAbs(195.18 - 5.2, 1e-12));
        CHECK(b.featherDeg == render::kSeamInsetFeatherDeg);
        CHECK_THAT(0.5 * b.lensFovDeg, Catch::Matchers::WithinAbs(94.99, 1e-9));
        // Only the FOV and feather change; the occlusion settings do not.
        CHECK(b.occlusionFeatherPx == a.occlusionFeatherPx);
        CHECK(b.seamShiftDeg == a.seamShiftDeg);
    }

    SECTION("the inset is clamped and a garbage feather keeps the analysis one") {
        const geom::BlendParams big = render::insetRenderBlend(a, 50.0);
        CHECK_THAT(big.lensFovDeg, Catch::Matchers::WithinAbs(195.18 - 2.0 * render::kMaxSeamInsetDeg, 1e-12));
        const geom::BlendParams nanFeather = render::insetRenderBlend(a, 1.0, std::nan(""));
        CHECK(nanFeather.featherDeg == a.featherDeg);
        const geom::BlendParams zeroFeather = render::insetRenderBlend(a, 1.0, 0.0);
        CHECK(zeroFeather.featherDeg == a.featherDeg);
    }
}

TEST_CASE("estimateGain reads trusted pixels only: a dark lens rim does not move the gain", "[photoseam][seam]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    // Lens 1 is exactly 0.4 stop brighter than lens 0 everywhere, so the
    // symmetric split is 2^0.2 / 2^-0.2 on every channel.
    constexpr double kStops = 0.4;
    const auto sceneWith = [&](bool darkRim) {
        return [darkRim](int lens, const Vec3d& d, double thetaDeg, double rgb[3]) {
            double v = skyLevel(d);
            if (lens == 1) {
                v *= std::exp2(kStops);
            }
            // The sample's lens-0 rim: fine to ~94 deg, then 1.5 stops per
            // degree (-1 stop at 94.7, -5 near the calibrated 97.59 edge).
            if (darkRim && lens == 0 && thetaDeg > 94.0) {
                v *= std::exp2(-1.5 * (thetaDeg - 94.0));
            }
            rgb[0] = rgb[1] = rgb[2] = v;
        };
    };
    const SynthPair clean = synthPair(rig.value(), sceneWith(false), pool);
    const SynthPair rim = synthPair(rig.value(), sceneWith(true), pool);

    geom::BlendParams blend;
    blend.useOcclusionMask = false;
    const render::BandParams band;  // the importer's default band
    auto gClean = render::estimateGain(rig.value(), clean.pair, blend, band, pool);
    auto gRim = render::estimateGain(rig.value(), rim.pair, blend, band, pool);
    REQUIRE(gClean.ok());
    REQUIRE(gRim.ok());
    REQUIRE(gRim.value().samples > 10000);

    const double expected = std::exp2(0.5 * kStops);
    const double cleanG[3] = {gClean.value().gain[0].x, gClean.value().gain[0].y, gClean.value().gain[0].z};
    const double rimG[3] = {gRim.value().gain[0].x, gRim.value().gain[0].y, gRim.value().gain[0].z};
    for (int c = 0; c < 3; ++c) {
        INFO("channel " << c << ": clean " << cleanG[c] << ", with the dark rim " << rimG[c] << ", expected "
                        << expected);
        // The known gain is recovered (10-bit quantisation of the synthetic
        // frames is the only error source) ...
        CHECK_THAT(cleanG[c], Catch::Matchers::WithinRel(expected, 0.01));
        // ... and the dark rim does not move it at all beyond that noise.
        CHECK_THAT(rimG[c], Catch::Matchers::WithinRel(cleanG[c], 0.002));
    }

    // The guard is not vacuous: the old "both alphas > 0.5" statistic, taken
    // on the same bands, IS pulled by the rim (it reads the dark rim as "lens
    // 1 is too bright").
    auto bands = render::renderLensBands(rig.value(), rim.pair, blend, band, true, nullptr, pool);
    REQUIRE(bands.ok());
    double s0 = 0.0, s1 = 0.0;
    for (std::size_t i = 0; i < bands.value().luma[0].size(); ++i) {
        if (bands.value().alpha[0][i] > 0.5f && bands.value().alpha[1][i] > 0.5f) {
            s0 += bands.value().luma[0][i];
            s1 += bands.value().luma[1][i];
        }
    }
    REQUIRE(s0 > 0.0);
    const double oldGain = std::sqrt(s1 / s0);
    INFO("old all-co-visible estimate " << oldGain << " vs trusted " << rimG[1]);
    CHECK(oldGain > rimG[1] * 1.03);
}

TEST_CASE("stage 1: the render inset and the trusted gain thin the sky seam", "[photoseam][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;

    const geom::BlendParams analysis;  // 195.18 / 4, the importer's m_blend
    const geom::BlendParams inset = render::insetRenderBlend(analysis, render::kDefaultSeamInsetDeg);
    const render::MetricBandRequest req;  // 2048 columns, +-30 deg
    double sumOff[4] = {}, sumOn[4] = {};
    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());

        // Production: the calibrated blend, no gain (the research's baseline).
        render::RenderParamsBuilder off;
        off.rig(clip.value().rig).blend(analysis, true);
        auto offBands = render::renderMetricBands(off, pair.value(), req, pool);
        REQUIRE(offBands.ok());
        // Stage 1: the inset render blend plus ONE trusted-pixel gain.
        auto g = render::estimateGain(clip.value().rig, pair.value(), analysis, render::BandParams{}, pool);
        REQUIRE(g.ok());
        render::RenderParamsBuilder on;
        on.rig(clip.value().rig).blend(inset, true).gain(g.value().gain[0], g.value().gain[1]);
        auto onBands = render::renderMetricBands(on, pair.value(), req, pool);
        REQUIRE(onBands.ok());

        // One trust mask per frame (from the uncorrected render) for both.
        const std::vector<std::uint8_t> trust = render::coValidTrustMask(offBands.value());
        auto mOff = render::skySeamMetrics(offBands.value(), trust, 0.20, 0.44);
        auto mOn = render::skySeamMetrics(onBands.value(), trust, 0.20, 0.44);
        REQUIRE(mOff.ok());
        REQUIRE(mOn.ok());
        const render::SkySeamMetrics& a = mOff.value();
        const render::SkySeamMetrics& b = mOn.value();
        WARN("frame " << frame << " sky metrics (millistops) production -> stage 1: line " << a.line << " -> "
                      << b.line << ", band " << a.band << " -> " << b.band << ", broad " << a.broad << " -> "
                      << b.broad << ", dE " << a.dE << " -> " << b.dE << " (trusted " << a.trustedPixels
                      << "), gain lens0 " << g.value().gain[0].x << "/" << g.value().gain[0].y << "/"
                      << g.value().gain[0].z);
        const double va[4] = {a.line, a.band, a.broad, a.dE};
        const double vb[4] = {b.line, b.band, b.broad, b.dE};
        for (int k = 0; k < 4; ++k) {
            sumOff[k] += va[k];
            sumOn[k] += vb[k];
        }
    }
    WARN("stage 1 over frames 0/32/64: line x" << ratio(sumOn[0], sumOff[0]) << ", band x" << ratio(sumOn[1], sumOff[1])
                                               << ", broad x" << ratio(sumOn[2], sumOff[2]) << ", dE x"
                                               << ratio(sumOn[3], sumOff[3]));
    // Section 8.2 acceptance: line <= 0.6x, band <= 0.65x, dE <= 0.7x.
    CHECK(sumOn[0] <= 0.60 * sumOff[0]);
    CHECK(sumOn[1] <= 0.65 * sumOff[1]);
    CHECK(sumOn[3] <= 0.70 * sumOff[3]);
}

// ===========================================================================
//  Stage 2: helpers
// ===========================================================================
namespace {

/// Gain cell (j, g) of `f`, channel c, and its grid position.
double cellGain(const render::PhotoSeamField& f, std::uint32_t j, std::uint32_t g, int c) {
    return static_cast<double>(f.gain[(static_cast<std::size_t>(j) * f.w + g) * 3u + static_cast<std::size_t>(c)]);
}
double cellLonRad(const render::PhotoSeamField& f, std::uint32_t g) {
    return -kPi + kTwoPi * static_cast<double>(g) / static_cast<double>(f.w);
}
double cellLatRad(const render::PhotoSeamField& f, std::uint32_t j) {
    return static_cast<double>(f.latMinRad) +
           (static_cast<double>(f.latMaxRad) - static_cast<double>(f.latMinRad)) * static_cast<double>(j) /
               static_cast<double>(f.h - 1);
}

/// A small, valid, smooth synthetic field (no measurement): the kernel-side
/// tests need one with known values.
render::PhotoSeamField syntheticField(std::uint32_t w, std::uint32_t h, float gainScale) {
    render::PhotoSeamField f;
    f.w = w;
    f.h = h;
    f.latMinRad = static_cast<float>(deg2rad(-7.59));
    f.latMaxRad = static_cast<float>(deg2rad(7.59));
    f.thetaMaxRad[0] = f.thetaMaxRad[1] = static_cast<float>(deg2rad(97.59));
    f.gain.resize(static_cast<std::size_t>(w) * h * 3u);
    for (std::uint32_t j = 0; j < h; ++j) {
        for (std::uint32_t g = 0; g < w; ++g) {
            const double lon = cellLonRad(f, g);
            const double lat = cellLatRad(f, j);
            for (int c = 0; c < 3; ++c) {
                f.gain[(static_cast<std::size_t>(j) * w + g) * 3u + static_cast<std::size_t>(c)] = static_cast<float>(
                    gainScale * (0.5 + 0.2 * std::sin(lon + c) + 0.1 * std::cos(3.0 * lat + 0.5 * lon)));
            }
        }
    }
    f.rim.resize(static_cast<std::size_t>(w) * 2u);
    for (std::uint32_t g = 0; g < w; ++g) {
        f.rim[g * 2u + 0u] = static_cast<float>(deg2rad(93.0 + 1.5 * std::sin(cellLonRad(f, g))));
        f.rim[g * 2u + 1u] = static_cast<float>(deg2rad(96.0));
    }
    return f;
}

/// The half log gain the kernel applies at polar-axis (lon, lat) for
/// `params` / `table` (the kernel's own function, on the host).
std::array<float, 3> kernelHalf(const OsvRenderParams& params, const std::vector<float>& table, double lon,
                                double lat) {
    const float d[3] = {static_cast<float>(std::cos(lat) * std::sin(lon)), static_cast<float>(std::sin(lat)),
                        static_cast<float>(std::cos(lat) * std::cos(lon))};
    OsvPhotoPixel ph;
    osvPhotoBegin(&params, table.data(), d, &ph);
    return {ph.halfLog2[0], ph.halfLog2[1], ph.halfLog2[2]};
}

/// Polar map of a size the synthetic tests render.
geom::EquirectMap polarMap(int w) {
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::PolarAxis;
    map.w = w;
    map.h = w / 2;
    return map;
}

}  // namespace

// ===========================================================================
//  Stage 2: 8.4 test 1 - synthetic recovery
// ===========================================================================

TEST_CASE("photoSeamFromBands recovers a known gain field and a lens rim", "[photoseam]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    // A smooth, direction-dependent, per-channel gain (native linear, where
    // the kernel applies it) and lens 0's rim falling from 93 deg - 1.5 stops
    // per degree - over the half of the longitudes with lon in (0, pi).
    const auto trueG = [](double lon, double lat, int c) {
        const double base[3] = {0.5, 0.4, 0.15};
        return base[c] + (c < 2 ? 0.15 * std::sin(lon) : 0.0) + 0.03 * rad2deg(lat) / 10.0;
    };
    const SynthPair synth = synthPair(
        rig.value(),
        [&trueG](int lens, const Vec3d& d, double thetaDeg, double rgb[3]) {
            const double lon = polarLon(d);
            const double lat = polarLat(d);
            for (int c = 0; c < 3; ++c) {
                double v = skyLevel(d);
                if (lens == 1) {
                    v *= std::exp2(trueG(lon, lat, c));
                }
                if (lens == 0 && lon > 0.0 && thetaDeg > 93.0) {
                    v *= std::exp2(-1.5 * (thetaDeg - 93.0));
                }
                rgb[c] = v;
            }
        },
        pool);
    geom::BlendParams blend;
    blend.useOcclusionMask = false;
    const render::PhotoSeamParams P;
    auto field = render::measurePhotoSeam(rig.value(), synth.pair, blend, P, pool);
    INFO((field.ok() ? std::string("measured") : field.error().message));
    REQUIRE(field.ok());
    const render::PhotoSeamField& f = field.value();
    REQUIRE(f.valid());

    // The gain on the cells inside the trusted latitudes (lens 0's usable
    // rim ends at 93 deg on half the ring, i.e. lat 2.5 deg after the margin).
    double acc = 0.0;
    std::size_t n = 0;
    for (std::uint32_t j = 0; j < f.h; ++j) {
        const double latDeg = rad2deg(cellLatRad(f, j));
        if (latDeg < -5.0 || latDeg > 1.5) {
            continue;
        }
        for (std::uint32_t g = 0; g < f.w; ++g) {
            for (int c = 0; c < 3; ++c) {
                const double e = cellGain(f, j, g, c) - trueG(cellLonRad(f, g), cellLatRad(f, j), c);
                acc += e * e;
                ++n;
            }
        }
    }
    REQUIRE(n > 0);
    const double rms = std::sqrt(acc / static_cast<double>(n));
    INFO("gain RMS error " << rms << " stops over " << n << " cell channels");
    CHECK(rms < 0.02);

    // The rim: 93 deg where the fall-off is (away from the transitions,
    // which the conservative running minimum widens), thetaMax elsewhere,
    // and lens 1 has none.
    const double thetaMaxDeg = 0.5 * blend.lensFovDeg;
    for (std::uint32_t g = 0; g < f.w; ++g) {
        const double lonDeg = rad2deg(cellLonRad(f, g));
        const double rim0 = render::photoRimDegAt(f, 0, cellLonRad(f, g));
        const double rim1 = render::photoRimDegAt(f, 1, cellLonRad(f, g));
        INFO("lon " << lonDeg << ": rim0 " << rim0 << ", rim1 " << rim1);
        if (lonDeg > 25.0 && lonDeg < 155.0) {
            CHECK(std::fabs(rim0 - 93.0) < 0.5);
        }
        if (lonDeg < -25.0 && lonDeg > -155.0) {
            CHECK(rim0 > thetaMaxDeg - 0.3);
        }
        CHECK(rim1 > thetaMaxDeg - 0.3);
        CHECK(rim0 <= thetaMaxDeg + 1e-3);
    }
}

// ===========================================================================
//  Stage 2: 8.4 test 2 - texture robustness
// ===========================================================================

TEST_CASE("a textured, misregistered overlap moves neither the rim nor the gain", "[photoseam]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    // High-contrast texture (period 9 deg, ~0.5 stop per degree), lens 1
    // looking 0.2 deg (about one pixel) off along longitude and 0.3 stop
    // brighter.  No rim anywhere.
    constexpr double kGain = 0.3;
    const Mat3d shift = Mat3d::rotY(deg2rad(0.2));
    const SynthPair synth = synthPair(
        rig.value(),
        [&shift](int lens, const Vec3d& dIn, double /*thetaDeg*/, double rgb[3]) {
            const Vec3d d = lens == 1 ? shift * dIn : dIn;
            const double tex = 0.3 * std::exp2(0.8 * std::sin(40.0 * polarLon(d)) * std::sin(40.0 * polarLat(d)));
            const double v = lens == 1 ? tex * std::exp2(kGain) : tex;
            rgb[0] = rgb[1] = rgb[2] = v;
        },
        pool);
    geom::BlendParams blend;
    blend.useOcclusionMask = false;
    auto field = render::measurePhotoSeam(rig.value(), synth.pair, blend, render::PhotoSeamParams{}, pool);
    INFO((field.ok() ? std::string("measured") : field.error().message));
    REQUIRE(field.ok());
    const render::PhotoSeamField& f = field.value();
    // The flat-pixel gate keeps the texture out of the rim search: no rim.
    const double thetaMaxDeg = 0.5 * blend.lensFovDeg;
    std::size_t atMax = 0;
    for (std::uint32_t g = 0; g < f.w; ++g) {
        atMax += (render::photoRimDegAt(f, 0, cellLonRad(f, g)) > thetaMaxDeg - 0.3 &&
                  render::photoRimDegAt(f, 1, cellLonRad(f, g)) > thetaMaxDeg - 0.3)
                     ? 1u
                     : 0u;
    }
    INFO("rim at thetaMax in " << atMax << " of " << f.w << " columns");
    CHECK(atMax >= f.w * 95u / 100u);
    // The gain is unbiased: the misregistered texture averages out.
    for (int c = 0; c < 3; ++c) {
        std::vector<double> v;
        for (std::uint32_t j = 0; j < f.h; ++j) {
            for (std::uint32_t g = 0; g < f.w; ++g) {
                v.push_back(cellGain(f, j, g, c));
            }
        }
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        INFO("channel " << c << ": median gain " << v[v.size() / 2] << " (true " << kGain << ")");
        CHECK(std::fabs(v[v.size() / 2] - kGain) < 0.02);
    }
}

// ===========================================================================
//  Stage 2: 8.4 test 3 - the negative result as a guard
// ===========================================================================

TEST_CASE("without the trust mask a dark rim darkens the other lens; with it, it does not", "[photoseam]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    constexpr double kGain = 0.4;
    const SynthPair synth = synthPair(
        rig.value(),
        [](int lens, const Vec3d& d, double thetaDeg, double rgb[3]) {
            double v = skyLevel(d);
            if (lens == 1) {
                v *= std::exp2(kGain);
            }
            if (lens == 0 && thetaDeg > 93.0) {
                v *= std::exp2(-1.5 * (thetaDeg - 93.0));
            }
            rgb[0] = rgb[1] = rgb[2] = v;
        },
        pool);
    geom::BlendParams blend;
    blend.useOcclusionMask = false;
    const auto meanGain = [&](bool trustMask) {
        render::PhotoSeamParams P;
        P.trustMask = trustMask;
        auto field = render::measurePhotoSeam(rig.value(), synth.pair, blend, P, pool);
        REQUIRE(field.ok());
        double acc = 0.0;
        for (const float g : field.value().gain) {
            acc += static_cast<double>(g);
        }
        return acc / static_cast<double>(field.value().gain.size());
    };
    const double without = meanGain(false);
    const double with = meanGain(true);
    INFO("mean log2 gain (master / slave): without the trust mask " << without << ", with it " << with << ", true "
                                                                     << kGain);
    // Positive G darkens the master (lens 1): the rim is read as "lens 1 is
    // too bright" - the research's negative result, reproduced.
    CHECK(without - kGain > 0.05);
    CHECK(std::fabs(with - kGain) < 0.03);
}

// ===========================================================================
//  Stage 2: 8.4 test 4 - continuity of the applied correction
// ===========================================================================

TEST_CASE("the applied correction is continuous across the span edge and the longitude wrap", "[photoseam]") {
    auto rig = makeSyntheticRig(512);
    REQUIRE(rig.ok());
    const render::PhotoSeamField f = syntheticField(64, 8, 1.0f);
    REQUIRE(f.valid());
    render::PhotoSeamParams P;
    geom::BlendParams blend;
    render::RenderParamsBuilder b;
    b.rig(rig.value()).equirect(polarMap(512)).blend(blend, true).color(
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f));
    b.photo(f, P);
    auto params = b.buildParams();
    REQUIRE(params.ok());
    REQUIRE(params.value().photoEnabled == 1);
    const std::vector<float> table = f.kernelTable();
    const double eps = 1e-6;
    const auto maxStep = [&](double lonA, double latA, double lonB, double latB) {
        const auto a = kernelHalf(params.value(), table, lonA, latA);
        const auto c = kernelHalf(params.value(), table, lonB, latB);
        double m = 0.0;
        for (int k = 0; k < 3; ++k) {
            m = std::max(m, 2.0 * std::fabs(static_cast<double>(a[k] - c[k])));  // full stops
        }
        return m;
    };
    for (const double lon : {-2.5, -1.0, 0.3, 1.7, 3.0}) {
        // The span's two edges and the ends of the luma and chroma decays.
        for (const double edge : {static_cast<double>(f.latMaxRad), static_cast<double>(f.latMinRad),
                                  static_cast<double>(f.latMaxRad) + deg2rad(P.decayDeg),
                                  static_cast<double>(f.latMaxRad) + deg2rad(P.decayDeg * P.chromaDecayScale),
                                  static_cast<double>(f.latMinRad) - deg2rad(P.decayDeg)}) {
            INFO("lon " << lon << ", lat " << edge);
            CHECK(maxStep(lon, edge - eps, lon, edge + eps) < 1e-4);
        }
    }
    // Longitude wraps: +pi and -pi are the same meridian.
    for (const double lat : {-0.1, 0.0, 0.05, 0.12}) {
        INFO("lat " << lat);
        CHECK(maxStep(kPi - eps, lat, -kPi + eps, lat) < 1e-4);
        const double r0 = render::photoRimDegAt(f, 0, kPi - eps);
        const double r1 = render::photoRimDegAt(f, 0, -kPi + eps);
        CHECK(std::fabs(r0 - r1) < 1e-3);
    }
    // And nothing at all far from the seam (the decay reached zero).
    const auto far = kernelHalf(params.value(), table, 0.3, deg2rad(45.0));
    CHECK(far[0] == 0.0f);
    CHECK(far[1] == 0.0f);
    CHECK(far[2] == 0.0f);
}

// ===========================================================================
//  Stage 2: 8.4 test 5 - identity
// ===========================================================================

TEST_CASE("everything off renders bit-identically; a neutral field is production bit for bit", "[photoseam]") {
    ThreadPool pool;
    auto rig = makeSyntheticRig(768);
    REQUIRE(rig.ok());
    const SynthPair synth = synthPair(
        rig.value(),
        [](int lens, const Vec3d& d, double thetaDeg, double rgb[3]) {
            const double v = skyLevel(d) * (lens == 1 ? 1.3 : 1.0) * (thetaDeg > 94.0 ? 0.6 : 1.0);
            rgb[0] = v;
            rgb[1] = 0.9 * v;
            rgb[2] = 0.7 * v;
        },
        pool);
    render::CpuRenderer cpu(pool);
    const geom::BlendParams blend;  // 195.18 / 4 with the occlusion mask
    const OsvColorParams pq = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    geom::VirtualCamera cam;
    cam.w = 320;
    cam.h = 180;
    cam.hfovDeg = 100.0;
    cam.yawDeg = 90.0;  // across the seam
    const auto draw = [&](const render::RenderParamsBuilder& b, bool equirect) {
        render::RenderParamsBuilder bb = b;
        if (equirect) {
            bb.equirect(polarMap(512));
        } else {
            bb.camera(cam);
        }
        auto job = bb.build(synth.pair);
        REQUIRE(job.ok());
        auto img = cpu.render(job.value());
        REQUIRE(img.ok());
        return std::move(img).value();
    };
    render::RenderParamsBuilder base;
    base.rig(rig.value()).color(pq).blend(blend, true);

    SECTION("mode Off (and a cleared field) is the render without a field") {
        render::PhotoSeamParams off;
        off.mode = render::PhotoSeamMode::Off;
        render::RenderParamsBuilder withOff = base;
        withOff.photo(syntheticField(64, 8, 1.0f), off);
        auto params = withOff.equirect(polarMap(512)).buildParams();
        REQUIRE(params.ok());
        CHECK(params.value().photoEnabled == 0);
        render::RenderParamsBuilder cleared = base;
        cleared.photo(syntheticField(64, 8, 1.0f), render::PhotoSeamParams{}).clearPhoto();
        for (const bool eq : {true, false}) {
            const render::ImageRGBAf a = draw(base, eq);
            const render::ImageRGBAf b = draw(withOff, eq);
            const render::ImageRGBAf c = draw(cleared, eq);
            CHECK(std::memcmp(a.data.data(), b.data.data(), a.data.size() * sizeof(float)) == 0);
            CHECK(std::memcmp(a.data.data(), c.data.data(), a.data.size() * sizeof(float)) == 0);
        }
    }

    SECTION("a zero gain with the rim at thetaMax and the production feather changes no bit") {
        render::PhotoSeamField neutral = syntheticField(64, 8, 0.0f);  // all-zero gain
        for (int lens = 0; lens < 2; ++lens) {
            const float thetaMax = static_cast<float>(geom::effectiveThetaMax(lens, blend));
            neutral.thetaMaxRad[lens] = thetaMax;
            for (std::uint32_t g = 0; g < neutral.w; ++g) {
                neutral.rim[g * 2u + static_cast<std::uint32_t>(lens)] = thetaMax;
            }
        }
        render::PhotoSeamParams P;
        P.rimFeatherDeg = blend.featherDeg;  // the production feather
        render::RenderParamsBuilder withNeutral = base;
        withNeutral.photo(neutral, P);
        auto params = withNeutral.equirect(polarMap(512)).buildParams();
        REQUIRE(params.ok());
        REQUIRE(params.value().photoEnabled == 1);
        for (const bool eq : {true, false}) {
            const render::ImageRGBAf a = draw(base, eq);
            const render::ImageRGBAf b = draw(withNeutral, eq);
            CHECK(std::memcmp(a.data.data(), b.data.data(), a.data.size() * sizeof(float)) == 0);
        }
    }
}

// ===========================================================================
//  Stage 2: 8.4 test 7 - sizes, blending, validation
// ===========================================================================

TEST_CASE("the parameter block stays within 4 KB and fields blend exactly", "[photoseam]") {
    static_assert(sizeof(OsvRenderParams) <= 4096, "OsvRenderParams must stay within the 4 KB kernel budget");
    CHECK(sizeof(OsvRenderParams) <= 4096u);

    const render::PhotoSeamField a = syntheticField(32, 6, 1.0f);
    const render::PhotoSeamField b = syntheticField(32, 6, -0.5f);
    auto at0 = render::blendPhotoSeamFields(a, b, 0.0);
    auto at1 = render::blendPhotoSeamFields(a, b, 1.0);
    auto mid = render::blendPhotoSeamFields(a, b, 0.25);
    REQUIRE(at0.ok());
    REQUIRE(at1.ok());
    REQUIRE(mid.ok());
    CHECK(at0.value().gain == a.gain);
    CHECK(at1.value().gain == b.gain);
    CHECK(at0.value().rim == a.rim);
    CHECK(at1.value().rim == b.rim);
    CHECK_THAT(mid.value().gain[5], Catch::Matchers::WithinAbs(a.gain[5] + (b.gain[5] - a.gain[5]) * 0.25, 1e-6));
    // Clamped, not extrapolated.
    CHECK(render::blendPhotoSeamFields(a, b, 7.0).value().gain == b.gain);
    // Non-finite weights and layout mismatches are refused.
    CHECK(render::blendPhotoSeamFields(a, b, std::nan("")).error().code == ErrorCode::InvalidArgument);
    CHECK(render::blendPhotoSeamFields(a, syntheticField(33, 6, 1.0f), 0.5).error().code ==
          ErrorCode::InvalidArgument);
    render::PhotoSeamField otherSpan = b;
    otherSpan.latMaxRad += 0.01f;
    CHECK(render::blendPhotoSeamFields(a, otherSpan, 0.5).error().code == ErrorCode::InvalidArgument);

    // valid() refuses what the kernel could misread.
    render::PhotoSeamField bad = a;
    bad.gain[3] = std::nanf("");
    CHECK_FALSE(bad.valid());
    CHECK(bad.kernelTable().empty());
    bad = a;
    bad.rim.pop_back();
    CHECK_FALSE(bad.valid());
    bad = a;
    bad.latMaxRad = bad.latMinRad;
    CHECK_FALSE(bad.valid());
    // A table with the kernel's layout: gain, then rim.
    const std::vector<float> table = a.kernelTable();
    REQUIRE(table.size() == a.gain.size() + a.rim.size());
    CHECK(std::equal(a.rim.begin(), a.rim.end(), table.begin() + static_cast<std::ptrdiff_t>(a.gain.size())));
}

TEST_CASE("the builder and RenderJob refuse a half-configured photo table", "[photoseam]") {
    auto rig = makeSyntheticRig(512);
    REQUIRE(rig.ok());
    render::RenderParamsBuilder b;
    b.rig(rig.value()).equirect(polarMap(256)).color(
        color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f));
    render::PhotoSeamField invalid = syntheticField(16, 4, 1.0f);
    invalid.rim.clear();
    b.photo(invalid, render::PhotoSeamParams{});
    CHECK(b.buildParams().value().photoEnabled == 0);
    // RimOnly with no rim feather asks for nothing: off.
    render::PhotoSeamParams nothing;
    nothing.mode = render::PhotoSeamMode::RimOnly;
    nothing.rimFeatherDeg = 0.0;
    b.photo(syntheticField(16, 4, 1.0f), nothing);
    CHECK(b.buildParams().value().photoEnabled == 0);
    // RimOnly: rim on, gain strength 0.
    render::PhotoSeamParams rimOnly;
    rimOnly.mode = render::PhotoSeamMode::RimOnly;
    b.photo(syntheticField(16, 4, 1.0f), rimOnly);
    const OsvRenderParams p = b.buildParams().value();
    CHECK(p.photoEnabled == 1);
    CHECK(p.photoStrength == 0.0f);
    CHECK(p.photoRimFeatherRad > 0.0f);
    CHECK(p.photoSinLatLo < p.photoSinLatHi);
    // A job whose table does not match its declared shape is invalid.
    render::RenderJob job;
    job.params = p;
    job.params.outW = 16;
    job.params.outH = 8;
    static const osv_u16 dummy[4] = {0, 0, 0, 0};
    for (OsvPlane& plane : job.planes) {
        plane.y = plane.u = plane.v = dummy;
        plane.w = plane.h = plane.cw = plane.ch = 1;
    }
    job.photoField.assign(render::RenderJob::photoTableSize(p), 0.0f);
    CHECK(job.valid());
    job.photoField.pop_back();
    CHECK_FALSE(job.valid());
}

// ===========================================================================
//  Stage 2: time - the EMA, the cross-fade, the rim accumulator
// ===========================================================================

TEST_CASE("PhotoSeamHistory glides between buckets and accumulates the rim", "[photoseam]") {
    render::PhotoSeamParams P;
    P.temporalAlpha = 0.5;
    auto f0 = std::make_shared<render::PhotoSeamField>(syntheticField(32, 6, 0.0f));
    auto f1 = std::make_shared<render::PhotoSeamField>(syntheticField(32, 6, 1.0f));
    // Raw rim measurements: bucket 0 says 93.0 everywhere, bucket 1 says 94.0
    // but has not measured the first four columns.
    f0->rimMeasured.assign(f0->rim.size(), static_cast<float>(deg2rad(93.0)));
    f1->rimMeasured.assign(f1->rim.size(), static_cast<float>(deg2rad(94.0)));
    for (std::size_t k = 0; k < 8; ++k) {
        f1->rimMeasured[k] = std::nanf("");
    }
    render::PhotoSeamHistory history;
    CHECK_FALSE(history.measured(0));
    history.store(0, f0, P);
    history.store(1, f1, P);
    history.store(2, nullptr, P);  // a refusal
    CHECK(history.measured(0));
    CHECK(history.measured(2));
    CHECK(history.fieldFor(2 * render::kParallaxBucketFrames, P) == nullptr);  // refused bucket: no field
    CHECK(history.fieldFor(3 * render::kParallaxBucketFrames, P) == nullptr);  // never measured

    // Bucket 1 was stored as the EMA 0.5 * f0 + 0.5 * f1 (= half of f1's gain,
    // f0's being zero); its last frame applies exactly that.
    const std::uint32_t last1 = 2 * render::kParallaxBucketFrames - 1;
    auto atLast = history.fieldFor(last1, P);
    REQUIRE(atLast);
    CHECK_THAT(atLast->gain[7], Catch::Matchers::WithinAbs(0.5 * f1->gain[7], 1e-6));
    // Its first frame glides 1/8 of the way from bucket 0's field.
    auto atFirst = history.fieldFor(render::kParallaxBucketFrames, P);
    REQUIRE(atFirst);
    CHECK_THAT(atFirst->gain[7], Catch::Matchers::WithinAbs(0.5 * f1->gain[7] / 8.0, 1e-6));
    // The rim: the median of each column's measurements, post-processed; the
    // clip-wide median fills the columns bucket 1 did not measure.
    for (std::uint32_t g = 0; g < atLast->w; ++g) {
        const double rim0 = render::photoRimDegAt(*atLast, 0, cellLonRad(*atLast, g));
        CHECK(rim0 >= 93.0 - 1e-3);
        CHECK(rim0 <= 94.0 + 1e-3);
    }
    // A draft's stand-in: the nearest accepted bucket.
    auto near = history.nearest(3, 4, P);
    REQUIRE(near);
    history.trim(1, 1);
    CHECK(history.size() == 1u);
    CHECK(history.measured(1));
    // The survivor keeps gliding from bucket 0's field though bucket 0 left.
    auto afterTrim = history.fieldFor(render::kParallaxBucketFrames, P);
    REQUIRE(afterTrim);
    CHECK(afterTrim->gain == atFirst->gain);
    history.clear();
    CHECK(history.size() == 0u);
}

TEST_CASE("PhotoSeamHistory: a frame renders the same whatever was measured before or after it", "[photoseam]") {
    // The direct path's Exact contract (plugins/reframe/DirectPath.cpp) and
    // two live instances of one clip (tests/premiere/importer/test_reopen.cpp)
    // need a frame's field to be a function of the frame, not of which other
    // buckets happened to be measured first.
    const render::PhotoSeamParams P;
    const auto make = [](float scale, double rimDeg) {
        auto f = std::make_shared<render::PhotoSeamField>(syntheticField(32, 6, scale));
        f->rimMeasured.assign(f->rim.size(), static_cast<float>(deg2rad(rimDeg)));
        return std::shared_ptr<const render::PhotoSeamField>(std::move(f));
    };
    const auto b0 = make(0.2f, 92.0);
    const auto b1 = make(0.6f, 93.0);
    const auto b2 = make(1.0f, 94.0);
    const auto b4 = make(0.4f, 91.8);
    const auto b5 = make(0.8f, 95.0);
    constexpr std::uint32_t N = render::kParallaxBucketFrames;
    const auto same = [](const std::shared_ptr<const render::PhotoSeamField>& x,
                         const std::shared_ptr<const render::PhotoSeamField>& y) {
        return x && y && x->gain == y->gain && x->rim == y->rim;
    };

    // Instance A renders frames 20 then 45, instance B frames 20, 5 then 45
    // (the reopen test's order): frames 20 and 45 agree bit for bit.
    render::PhotoSeamHistory a;
    a.store(2, b2, P);
    a.store(5, b5, P);
    render::PhotoSeamHistory b;
    b.store(2, b2, P);
    b.store(0, b0, P);
    b.store(5, b5, P);
    CHECK(same(a.fieldFor(2 * N + 4, P), b.fieldFor(2 * N + 4, P)));
    CHECK(same(a.fieldFor(5 * N + 5, P), b.fieldFor(5 * N + 5, P)));

    // A bucket stored AFTER another never changes the other's frames: its
    // glide partner, EMA and rim were fixed when it was stored.
    const auto frame20 = a.fieldFor(2 * N + 4, P);
    const auto frame45 = a.fieldFor(5 * N + 5, P);
    a.store(1, b1, P);
    a.store(4, b4, P);
    CHECK(same(a.fieldFor(2 * N + 4, P), frame20));
    CHECK(same(a.fieldFor(5 * N + 5, P), frame45));

    // Stored in order (playback), a bucket glides from its predecessor and
    // carries the median rim of the run: bucket 2 after 0 and 1 has
    // median(92, 93, 94) = 93 deg for both lenses.
    render::PhotoSeamHistory played;
    played.store(0, b0, P);
    played.store(1, b1, P);
    played.store(2, b2, P);
    const auto last2 = played.fieldFor(3 * N - 1, P);
    const auto first2 = played.fieldFor(2 * N, P);
    REQUIRE(last2);
    REQUIRE(first2);
    CHECK_FALSE(same(first2, last2));
    for (std::uint32_t g = 0; g < last2->w; ++g) {
        for (int lens = 0; lens < 2; ++lens) {
            CHECK_THAT(render::photoRimDegAt(*last2, lens, cellLonRad(*last2, g)),
                       Catch::Matchers::WithinAbs(93.0, 1e-3));
        }
    }
    // Out of order, the same bucket has no run before it: its own rim.
    const auto alone = a.fieldFor(3 * N - 1, P);
    REQUIRE(alone);
    CHECK_THAT(render::photoRimDegAt(*alone, 0, 0.0), Catch::Matchers::WithinAbs(94.0, 1e-3));
}

// ===========================================================================
//  Stage 2: passthrough output and the seam cost
// ===========================================================================

TEST_CASE("photoCodePerStop matches the D-Log M curve around grey", "[photoseam]") {
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Passthrough, 0.0f);
    const float perStop = render::photoCodePerStop(cp);
    INFO("code per stop " << perStop);
    REQUIRE(perStop > 0.02f);
    REQUIRE(perStop < 0.2f);
    // Decoding code + perStop gives twice the light of code, near grey.
    const auto lin = [&cp](float code) {
        const float c[3] = {code, code, code};
        float out[3];
        osvCodeToLinear(&cp, c, out);
        return static_cast<double>(out[1]);
    };
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (lo + hi);
        (lin(static_cast<float>(mid)) < 0.18 ? lo : hi) = mid;
    }
    const float grey = static_cast<float>(lo);
    // One stop CENTRED on grey (the log segment's local slope there).
    CHECK_THAT(std::log2(lin(grey + 0.5f * perStop) / lin(grey - 0.5f * perStop)),
               Catch::Matchers::WithinAbs(1.0, 0.01));
    OsvColorParams disabled = cp;
    disabled.enabled = 0;
    CHECK(render::photoCodePerStop(disabled) == 0.0f);
}

TEST_CASE("the usable rim is a seam cost through the Rim penalty slot", "[photoseam][seam]") {
    auto rig = makeSyntheticRig(512);
    REQUIRE(rig.ok());
    const render::PhotoSeamField f = syntheticField(64, 8, 0.0f);  // lens 0 rim ~93 +- 1.5, lens 1 at 96
    render::LensBands bands;
    bands.w = 512;
    bands.mapH = 256;
    bands.rowOffset = 108;
    bands.h = 40;  // +-14 deg around the equator
    const std::size_t n = static_cast<std::size_t>(bands.w) * bands.h;
    std::vector<float> p0(n, 0.0f), p1(n, 0.0f);
    REQUIRE(render::photoRimPenalty(bands, rig.value(), f, p0, p1));
    // Deep inside both lenses: no cost; past a lens's rim: full cost.
    std::size_t past0 = 0, inside0 = 0;
    for (std::uint32_t r = 0; r < bands.h; ++r) {
        for (std::uint32_t c = 0; c < bands.w; ++c) {
            double d[3];
            REQUIRE(render::bandPixelDirection(bands, c, r, d));
            const Vec3d dl = rig.value().bodyToLens[0] * Vec3d{d[0], d[1], d[2]};
            const double theta0 = rad2deg(std::atan2(std::hypot(dl.x, dl.y), dl.z));
            const double rim0 = render::photoRimDegAt(f, 0, std::atan2(d[0], d[2]));
            const float pen = p0[static_cast<std::size_t>(r) * bands.w + c];
            if (theta0 >= rim0) {
                CHECK(pen == 1.0f);
                ++past0;
            } else if (theta0 < rim0 - 0.5) {
                CHECK(pen == 0.0f);
                ++inside0;
            }
        }
    }
    CHECK(past0 > 0u);
    CHECK(inside0 > 0u);
    // Size mismatches contribute nothing and leave the maps alone.
    std::vector<float> small(n - 1, 0.0f);
    CHECK_FALSE(render::photoRimPenalty(bands, rig.value(), f, small, p1));

    // Through the process-wide slot: nothing without a scope on this thread,
    // the same maps with one.
    render::installPhotoRimPenaltyHook();
    const render::SeamPenaltyHook hook = render::seamPenaltyHook(render::SeamPenaltySlot::Rim);
    REQUIRE(hook.installed());
    CHECK(hook.weight == render::kPhotoRimPenaltyWeight);
    std::vector<float> h0(n, 0.0f), h1(n, 0.0f);
    CHECK_FALSE(hook.fn(bands, h0, h1, hook.user));
    {
        const render::PhotoRimPenaltyScope scope(&rig.value(), std::make_shared<const render::PhotoSeamField>(f));
        CHECK(hook.fn(bands, h0, h1, hook.user));
        CHECK(h0 == p0);
        CHECK(h1 == p1);
    }
    CHECK_FALSE(hook.fn(bands, h0, h1, hook.user));
    // The slot is process wide; leave it installed as the importer does (the
    // hook contributes nothing without a scope).
}

// ===========================================================================
//  Stage 2: a lens-protector rig - everything follows the rig's own FOV
// ===========================================================================

TEST_CASE("a narrower (lens-protector) FOV moves the inset render blend and the rim clamp with it",
          "[photoseam]") {
    // WP-CALIB folds the protector's field-angle correction into the rig and
    // sets the analysis blend's usable FOV to 192.08 deg.
    geom::BlendParams protector;
    protector.lensFovDeg = 192.08;
    const geom::BlendParams renderBlend = render::insetRenderBlend(protector, render::kDefaultSeamInsetDeg);
    CHECK_THAT(renderBlend.lensFovDeg, Catch::Matchers::WithinAbs(192.08 - 5.2, 1e-9));

    ThreadPool pool;
    auto rig = makeSyntheticRig(1024);
    REQUIRE(rig.ok());
    const SynthPair synth = synthPair(
        rig.value(),
        [](int lens, const Vec3d& d, double, double rgb[3]) {
            rgb[0] = rgb[1] = rgb[2] = skyLevel(d) * (lens == 1 ? 1.25 : 1.0);
        },
        pool);
    protector.useOcclusionMask = false;
    auto field = render::measurePhotoSeam(rig.value(), synth.pair, protector, render::PhotoSeamParams{}, pool);
    REQUIRE(field.ok());
    const double thetaMax = 0.5 * 192.08;
    CHECK_THAT(rad2deg(static_cast<double>(field.value().thetaMaxRad[0])), Catch::Matchers::WithinAbs(thetaMax, 1e-4));
    CHECK_THAT(rad2deg(static_cast<double>(field.value().latMaxRad)), Catch::Matchers::WithinAbs(thetaMax - 90.0, 1e-4));
    for (const float r : field.value().rim) {
        CHECK(rad2deg(static_cast<double>(r)) <= thetaMax + 1e-3);
    }
}

// ===========================================================================
//  Stage 2: [sample] - quality, texture, time, parity, cost
// ===========================================================================

namespace {

/// The four sky metrics of one variant as an array.
std::array<double, 4> metricArray(const render::SkySeamMetrics& m) { return {m.line, m.band, m.broad, m.dE}; }

/// NCC of the two lenses' code-space luma over band columns [c0, c1) of a
/// polar band rendered through the kernel with `builder` (co-visible pixels,
/// alpha > 0.5 in both, as overlapNcc).
double regionNcc(const render::RenderParamsBuilder& builder, const video::FramePair& pair, double c0Frac,
                 double c1Frac, ThreadPool& pool) {
    const geom::EquirectMap map = polarMap(2048);
    const std::uint32_t row0 = 512 - 23;  // +-4 deg, as osvtool seam's NCC band
    const std::uint32_t row1 = 512 + 23;
    std::vector<float> rows[2];
    for (int lens = 0; lens < 2; ++lens) {
        render::RenderParamsBuilder b = builder;
        b.equirect(map)
            .color(color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Passthrough, 0.0f))
            .alphaCoverage(true)
            .lensEnabled(lens, true)
            .lensEnabled(1 - lens, false);
        auto job = b.build(pair);
        REQUIRE(job.ok());
        auto shaded = render::shadeJobRows(job.value(), row0, row1, pool);
        REQUIRE(shaded.ok());
        rows[lens] = std::move(shaded).value();
    }
    const std::size_t w = 2048;
    const std::size_t c0 = static_cast<std::size_t>(c0Frac * w);
    const std::size_t c1 = static_cast<std::size_t>(c1Frac * w);
    std::vector<double> a, b;
    for (std::size_t r = 0; r < row1 - row0; ++r) {
        for (std::size_t c = c0; c < c1; ++c) {
            const float* pa = rows[0].data() + (r * w + c) * 4u;
            const float* pb = rows[1].data() + (r * w + c) * 4u;
            if (pa[3] > 0.5f && pb[3] > 0.5f) {
                a.push_back(0.2627 * pa[0] + 0.6780 * pa[1] + 0.0593 * pa[2]);
                b.push_back(0.2627 * pb[0] + 0.6780 * pb[1] + 0.0593 * pb[2]);
            }
        }
    }
    REQUIRE(a.size() > 1000);
    double ma = 0, mb = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(a.size());
    mb /= static_cast<double>(b.size());
    double num = 0, da = 0, db = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        num += (a[i] - ma) * (b[i] - mb);
        da += (a[i] - ma) * (a[i] - ma);
        db += (b[i] - mb) * (b[i] - mb);
    }
    return num / std::sqrt(da * db);
}

}  // namespace

TEST_CASE("stage 2: the photometric seam field thins the sky seam through the real kernel",
          "[photoseam][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    const render::PhotoSeamParams P;
    const render::MetricBandRequest req;
    std::array<double, 4> sumOff{}, sumOn{};
    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());
        auto field = render::measurePhotoSeam(clip.value().rig, pair.value(), analysis, P, pool);
        REQUIRE(field.ok());
        const render::PhotoSeamField& f = field.value();
        // Regression pins on the sample (section 1.2): lens 0's usable rim in
        // the open sky ~92.8 deg, lens 1's ~95.8, and the lens ratio +0.35
        // stop in green.
        std::vector<double> sky0, sky1;
        for (std::uint32_t g = static_cast<std::uint32_t>(0.20 * f.w); g < static_cast<std::uint32_t>(0.33 * f.w); ++g) {
            sky0.push_back(render::photoRimDegAt(f, 0, cellLonRad(f, g)));
            sky1.push_back(render::photoRimDegAt(f, 1, cellLonRad(f, g)));
        }
        std::sort(sky0.begin(), sky0.end());
        std::sort(sky1.begin(), sky1.end());
        const double med0 = sky0[sky0.size() / 2];
        const double med1 = sky1[sky1.size() / 2];

        render::RenderParamsBuilder off;
        off.rig(clip.value().rig).blend(analysis, true);
        render::RenderParamsBuilder on;
        on.rig(clip.value().rig).blend(analysis, true).photo(f, P);
        auto offBands = render::renderMetricBands(off, pair.value(), req, pool);
        auto onBands = render::renderMetricBands(on, pair.value(), req, pool);
        REQUIRE(offBands.ok());
        REQUIRE(onBands.ok());
        const std::vector<std::uint8_t> trust = render::rimTrustMask(offBands.value(), clip.value().rig, f);
        auto mOff = render::skySeamMetrics(offBands.value(), trust, 0.20, 0.44);
        auto mOn = render::skySeamMetrics(onBands.value(), trust, 0.20, 0.44);
        REQUIRE(mOff.ok());
        REQUIRE(mOn.ok());
        const auto a = metricArray(mOff.value());
        const auto b = metricArray(mOn.value());
        WARN("frame " << frame << " stage 2 (millistops) off -> field: line " << a[0] << " -> " << b[0] << ", band "
                      << a[1] << " -> " << b[1] << ", broad " << a[2] << " -> " << b[2] << ", dE " << a[3] << " -> "
                      << b[3] << "; sky rim " << med0 << " / " << med1 << " deg, median gain " << f.medianLog2Gain[0]
                      << " / " << f.medianLog2Gain[1] << " / " << f.medianLog2Gain[2] << "; analysis "
                      << f.bandMs << " + " << f.statsMs << " ms");
        CHECK(std::fabs(med0 - 92.8) < 0.8);
        CHECK(std::fabs(med1 - 95.8) < 0.8);
        CHECK(f.medianLog2Gain[1] > 0.2);
        CHECK(f.medianLog2Gain[1] < 0.5);
        for (int k = 0; k < 4; ++k) {
            sumOff[static_cast<std::size_t>(k)] += a[static_cast<std::size_t>(k)];
            sumOn[static_cast<std::size_t>(k)] += b[static_cast<std::size_t>(k)];
        }
    }
    WARN("stage 2 over frames 0/32/64: line x" << ratio(sumOn[0], sumOff[0]) << ", band x" << ratio(sumOn[1], sumOff[1])
                                               << ", broad x" << ratio(sumOn[2], sumOff[2]) << ", dE x"
                                               << ratio(sumOn[3], sumOff[3]));
    // Section 8.4 test 8: line <= 0.60x, band <= 0.60x, broad <= 0.75x, dE <= 0.35x.
    CHECK(sumOn[0] <= 0.60 * sumOff[0]);
    CHECK(sumOn[1] <= 0.60 * sumOff[1]);
    CHECK(sumOn[2] <= 0.75 * sumOff[2]);
    CHECK(sumOn[3] <= 0.35 * sumOff[3]);
}

TEST_CASE("stage 2 on the importer's default stitch: parallax grid, carved seam, global gain", "[photoseam][sample]") {
    // The research baseline above is the plain calibrated blend.  The
    // importer's default stitch is the parallax grid plus WP-SEAM's carved
    // seam (a narrow blend along a DP seam) plus the global gain; this is
    // what the field changes for a user.  The carve sees the field's rim as
    // its Rim cost, exactly as ImporterInstance::applyAnalyses arranges it.
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    const render::PhotoSeamParams P;
    const render::MetricBandRequest req;
    std::array<double, 4> sumOff{}, sumOn{};
    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());
        // ---- the default analyses ------------------------------------------------
        render::ParallaxWarpParams pw;
        pw.backend = render::FlowBackendKind::Classical;
        auto grid = render::buildParallaxWarp(clip.value().rig, pair.value(), analysis, pw, nullptr, pool);
        REQUIRE(grid.ok());
        render::WarpGridView view;
        view.uv = grid.value().uv.data();
        view.w = grid.value().w;
        view.h = grid.value().h;
        view.latMinRad = grid.value().latMinRad;
        view.latMaxRad = grid.value().latMaxRad;
        render::SeamCorrection correction;
        correction.warp = &view;
        auto gain = render::estimateGain(clip.value().rig, pair.value(), analysis, render::BandParams{}, pool);
        REQUIRE(gain.ok());
        auto field = render::measurePhotoSeam(clip.value().rig, pair.value(), analysis, P, pool);
        REQUIRE(field.ok());
        const auto shared = std::make_shared<const render::PhotoSeamField>(field.value());
        // Production: carved without a rim; stage 2: carved with the field's
        // rim as the Rim cost (the importer's order: field first, then carve).
        auto carvedOff = render::carveSeam(clip.value().rig, pair.value(), analysis, pw.band, correction,
                                           render::SeamCarveParams{}, nullptr, pool);
        REQUIRE(carvedOff.ok());
        render::installPhotoRimPenaltyHook();
        Result<render::BlendSeam> carvedOn = [&] {
            const render::PhotoRimPenaltyScope scope(&clip.value().rig, shared);
            return render::carveSeam(clip.value().rig, pair.value(), analysis, pw.band, correction,
                                     render::SeamCarveParams{}, nullptr, pool);
        }();
        REQUIRE(carvedOn.ok());

        // ---- the two stitches -----------------------------------------------------
        render::RenderParamsBuilder off;
        off.rig(clip.value().rig).blend(analysis, true).gain(gain.value().gain[0], gain.value().gain[1]);
        off.warp(grid.value().uv, grid.value().w, grid.value().h, grid.value().latMinRad, grid.value().latMaxRad);
        render::RenderParamsBuilder on = off;
        render::applyBlendSeam(off, carvedOff.value());
        render::applyBlendSeam(on, carvedOn.value());
        on.photo(*shared, P).gain(Vec3d{1, 1, 1}, Vec3d{1, 1, 1});
        auto offBands = render::renderMetricBands(off, pair.value(), req, pool);
        auto onBands = render::renderMetricBands(on, pair.value(), req, pool);
        REQUIRE(offBands.ok());
        REQUIRE(onBands.ok());
        const std::vector<std::uint8_t> trust = render::rimTrustMask(offBands.value(), clip.value().rig, *shared);
        auto mOff = render::skySeamMetrics(offBands.value(), trust, 0.20, 0.44);
        auto mOn = render::skySeamMetrics(onBands.value(), trust, 0.20, 0.44);
        REQUIRE(mOff.ok());
        REQUIRE(mOn.ok());
        const auto a = metricArray(mOff.value());
        const auto b = metricArray(mOn.value());
        WARN("frame " << frame << " default stitch (millistops) without -> with the field: line " << a[0] << " -> "
                      << b[0] << ", band " << a[1] << " -> " << b[1] << ", broad " << a[2] << " -> " << b[2]
                      << ", dE " << a[3] << " -> " << b[3]);
        for (std::size_t k = 0; k < 4; ++k) {
            sumOff[k] += a[k];
            sumOn[k] += b[k];
        }
    }
    WARN("default stitch over frames 0/32/64, with the field: line x" << ratio(sumOn[0], sumOff[0]) << ", band x"
                                                                      << ratio(sumOn[1], sumOff[1]) << ", broad x"
                                                                      << ratio(sumOn[2], sumOff[2]) << ", dE x"
                                                                      << ratio(sumOn[3], sumOff[3]));
    // Measured: line x0.71, band x0.98, broad x0.97, dE x0.34.  The carved
    // seam's narrow blend already removes most of the band-scale bump the
    // plain blend shows (band 51 vs 106 millistops), so what the field adds
    // on this stitch is the thin line and the colour step; it must not make
    // the band-scale terms worse.
    CHECK(sumOn[0] <= 0.85 * sumOff[0]);
    CHECK(sumOn[1] <= sumOff[1]);
    CHECK(sumOn[2] <= sumOff[2]);
    CHECK(sumOn[3] <= 0.50 * sumOff[3]);
}

TEST_CASE("stage 2 does no harm on texture: ground NCC with parallax, whole-band nccAfter", "[photoseam][sample]") {
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
        render::ParallaxWarpParams pw;
        pw.backend = render::FlowBackendKind::Classical;
        auto grid = render::buildParallaxWarp(clip.value().rig, pair.value(), analysis, pw, nullptr, pool);
        REQUIRE(grid.ok());
        auto field = render::measurePhotoSeam(clip.value().rig, pair.value(), analysis, render::PhotoSeamParams{}, pool);
        REQUIRE(field.ok());
        render::RenderParamsBuilder withGrid;
        withGrid.rig(clip.value().rig).blend(analysis, true);
        withGrid.warp(grid.value().uv, grid.value().w, grid.value().h, grid.value().latMinRad, grid.value().latMaxRad);
        render::RenderParamsBuilder withBoth = withGrid;
        withBoth.photo(field.value(), render::PhotoSeamParams{});
        const double before = regionNcc(withGrid, pair.value(), 0.54, 0.83, pool);
        const double after = regionNcc(withBoth, pair.value(), 0.54, 0.83, pool);
        // The analyses never see the field, so the whole-band parallax
        // number is the one the render blend change cannot touch.
        render::WarpGridView view;
        view.uv = grid.value().uv.data();
        view.w = grid.value().w;
        view.h = grid.value().h;
        view.latMinRad = grid.value().latMinRad;
        view.latMaxRad = grid.value().latMaxRad;
        render::BandParams band;
        band.bandHalfDeg = 4.0;
        auto nccAfter = render::overlapNcc(clip.value().rig, pair.value(), analysis, band, pool, nullptr, &view);
        REQUIRE(nccAfter.ok());
        WARN("frame " << frame << ": ground NCC (parallax on) " << before << " -> " << after << " with the field; "
                      << "whole-band nccAfter " << nccAfter.value());
        CHECK(std::fabs(after - before) < 0.005);
        CHECK(nccAfter.value() >= 0.915);
    }
}

TEST_CASE("stage 2 is stable in time: the applied gain moves <= 0.02 stop per frame", "[photoseam][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto clip = openSampleClip();
    REQUIRE(clip.ok());
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), clip.value().format);
    REQUIRE(reader.ok());
    ThreadPool pool;
    const geom::BlendParams analysis;
    const render::PhotoSeamParams P;
    // The importer's own schedule: one measurement per bucket, the EMA, the
    // cross-fade and the accumulated rim (PhotoSeamHistory).
    render::PhotoSeamHistory history;
    std::shared_ptr<const render::PhotoSeamField> previous;
    double worstGain = 0.0;
    double worstRimDeg = 0.0;
    std::uint32_t worstFrame = 0;
    std::size_t worstCell = 0;
    const std::uint32_t frames = std::min<std::uint32_t>(65u, reader.value().frameCount());
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto pair = reader.value().read(frame);
        REQUIRE(pair.ok());
        const std::uint32_t bucket = render::parallaxBucket(frame);
        if (!history.measured(bucket)) {
            auto field = render::measurePhotoSeam(clip.value().rig, pair.value(), analysis, P, pool);
            REQUIRE(field.ok());
            history.store(bucket, std::make_shared<const render::PhotoSeamField>(std::move(field).value()), P);
        }
        auto applied = history.fieldFor(frame, P);
        REQUIRE(applied);
        if (previous) {
            REQUIRE(previous->gain.size() == applied->gain.size());
            for (std::size_t i = 0; i < applied->gain.size(); ++i) {
                const double d = std::fabs(static_cast<double>(applied->gain[i] - previous->gain[i]));
                if (d > worstGain) {
                    worstGain = d;
                    worstFrame = frame;
                    worstCell = i / 3u;
                }
            }
            for (std::size_t i = 0; i < applied->rim.size(); ++i) {
                worstRimDeg = std::max(worstRimDeg, rad2deg(std::fabs(static_cast<double>(applied->rim[i] -
                                                                                           previous->rim[i]))));
            }
        }
        previous = applied;
    }
    WARN("over " << frames << " frames: largest per-cell gain change " << worstGain << " stop (frame " << worstFrame
                 << ", cell lon " << (previous ? rad2deg(cellLonRad(*previous, static_cast<std::uint32_t>(worstCell % previous->w))) : 0.0)
                 << " lat " << (previous ? rad2deg(cellLatRad(*previous, static_cast<std::uint32_t>(worstCell / previous->w))) : 0.0)
                 << "), largest rim change " << worstRimDeg << " deg between consecutive frames");
    CHECK(worstGain <= 0.02);
}

TEST_CASE("stage 2 renders the same on the CPU, CUDA and OpenCL", "[photoseam][sample][cuda][opencl]") {
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
    auto field = render::measurePhotoSeam(clip.value().rig, pair.value(), analysis, render::PhotoSeamParams{}, pool);
    REQUIRE(field.ok());
    const OsvColorParams pq = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    const OsvColorParams log = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Passthrough, 0.0f);
    geom::VirtualCamera cam;
    cam.w = 1280;
    cam.h = 720;
    cam.hfovDeg = 100.0;
    cam.yawDeg = 80.0;  // across the seam, in the sky
    cam.pitchDeg = 20.0;
    std::vector<render::RenderJob> jobs;
    for (const OsvColorParams& cp : {pq, log}) {
        for (const bool equirect : {false, true}) {
            render::RenderParamsBuilder b;
            b.rig(clip.value().rig).color(cp).blend(analysis, true).photo(field.value(), render::PhotoSeamParams{});
            if (equirect) {
                b.equirect(polarMap(1024));
            } else {
                b.camera(cam);
            }
            auto job = b.build(pair.value());
            REQUIRE(job.ok());
            REQUIRE(job.value().params.photoEnabled == 1);
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
            WARN(name << " vs CPU with the photometric field (" << job.params.outW << "x" << job.params.outH
                      << ", transfer " << job.params.color.transfer << "): PSNR " << stats.psnrDb << " dB, max "
                      << stats.maxAbsCode << " codes");
            CHECK(stats.psnrDb >= 60.0);
        }
    }
}
