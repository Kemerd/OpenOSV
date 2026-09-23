// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Photometric seam fix (docs/research/NEURAL_STITCHING.md, section 8).
//
// Stage 1: the render-only blend inset and the trusted-pixel gain.
//
// The synthetic tests render a known scene into two fisheye frames through
// the sample clip's own calibration (no clip needed): a smooth sky, a lens
// gain that is known exactly, and - where a test asks for it - a darkened
// rim on lens 0 like the one measured on the sample.  The [sample] tests
// measure the section 1.4 sky metrics on bands rendered through the real
// kernel, frames 0 / 32 / 64.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
#include "osv/render/ParallaxWarp.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"

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

// ===========================================================================
//  Synthetic dual-fisheye frames
// ===========================================================================

/// The sample clip's calibration as a stream-space rig at `streamW` pixels
/// (the verified constants, so this does not depend on the metadata decoder;
/// the same numbers tests/unit/test_render.cpp uses).
Result<geom::LensRig> makeSyntheticRig(int streamW) {
    meta::CalibrationSet cal;
    auto fill = [](meta::DewarpParams& d, float fx, float fy, float cx, float cy, const float k[5], float qw, float qx,
                   float qy, float qz) {
        d.fx = fx;
        d.fy = fy;
        d.cx = cx;
        d.cy = cy;
        for (int i = 0; i < 5; ++i) {
            d.k[static_cast<std::size_t>(i)] = k[i];
        }
        d.width = 3840;
        d.height = 3840;
        d.camExtriQ.w = qw;
        d.camExtriQ.x = qx;
        d.camExtriQ.y = qy;
        d.camExtriQ.z = qz;
        d.camExtriQ.present = true;
        for (int b = 1; b <= 11; ++b) {
            d.present.set(static_cast<std::size_t>(b));
        }
        d.present.set(28);
    };
    const float ks[5] = {0.0667397f, -0.0128859f, 0.0103815f, -0.00677581f, 0.00098791f};
    const float km[5] = {0.0613421f, -0.00480161f, 0.00444291f, -0.00452633f, 0.00066212f};
    fill(cal.slave, 1043.8802f, 1043.6731f, 1917.0421f, 1919.1294f, ks, 0.0026615f, 0.0019091f, -0.7056412f, 0.7085618f);
    fill(cal.master, 1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f, km, 0.7036960f, 0.7103991f, -0.0046943f, -0.0110939f);
    cal.sourceSlave = "test";
    cal.sourceMaster = "test";
    const double dfl = 829.3612 * (streamW / 3000.0);
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(streamW, streamW, 3840, 3840, dfl, 1043.445, streamW / 3776.0, nullptr));
    return geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength, dfl, geom::ExtrinsicConvention{});
}

/// Owns the 10-bit planar storage of one synthetic lens frame.
struct SynthFrame {
    std::shared_ptr<std::vector<std::uint16_t>> storage;
    video::PlanarFrame16 frame;
};

/// Scene-linear RGB seen by one lens along a body direction.  `thetaDeg` is
/// the ray's angle from that lens's own axis, so a lens-specific fall-off
/// can be modelled.
using Radiance = std::function<void(int lens, const Vec3d& dBody, double thetaDeg, double rgb[3])>;

/// Inverse of the D-Log M decode by table: code -> linear is monotone, so a
/// dense table plus linear interpolation is exact to far below one 10-bit
/// code step - and 1000x faster than a bisection per pixel.
class CodeEncoder {
public:
    explicit CodeEncoder(const OsvColorParams& cp) : m_cp(cp) {
        m_lin.resize(kN);
        for (int i = 0; i < kN; ++i) {
            const float c = static_cast<float>(i) / static_cast<float>(kN - 1);
            m_lin[static_cast<std::size_t>(i)] = static_cast<double>(osvDlogmToLinear(&m_cp.curve, c));
        }
        // The Y'CbCr -> R'G'B' matrix inverted once (the kernel applies the
        // forward one in osvYuvToCode).
        const double* k = nullptr;
        double m[9];
        for (int i = 0; i < 9; ++i) {
            m[i] = static_cast<double>(m_cp.yuvToRgb[i]);
        }
        k = m;
        const double det = k[0] * (k[4] * k[8] - k[5] * k[7]) - k[1] * (k[3] * k[8] - k[5] * k[6]) +
                           k[2] * (k[3] * k[7] - k[4] * k[6]);
        m_inv[0] = (k[4] * k[8] - k[5] * k[7]) / det;
        m_inv[1] = (k[2] * k[7] - k[1] * k[8]) / det;
        m_inv[2] = (k[1] * k[5] - k[2] * k[4]) / det;
        m_inv[3] = (k[5] * k[6] - k[3] * k[8]) / det;
        m_inv[4] = (k[0] * k[8] - k[2] * k[6]) / det;
        m_inv[5] = (k[2] * k[3] - k[0] * k[5]) / det;
        m_inv[6] = (k[3] * k[7] - k[4] * k[6]) / det;
        m_inv[7] = (k[1] * k[6] - k[0] * k[7]) / det;
        m_inv[8] = (k[0] * k[4] - k[1] * k[3]) / det;
    }

    /// Linear value -> log code in [0, 1].
    [[nodiscard]] double code(double linear) const {
        if (!(linear > m_lin.front())) {
            return 0.0;
        }
        if (linear >= m_lin.back()) {
            return 1.0;
        }
        const auto it = std::upper_bound(m_lin.begin(), m_lin.end(), linear);
        const std::size_t hi = static_cast<std::size_t>(it - m_lin.begin());
        const std::size_t lo = hi - 1;
        const double t = (linear - m_lin[lo]) / (m_lin[hi] - m_lin[lo]);
        return (static_cast<double>(lo) + t) / static_cast<double>(kN - 1);
    }

    /// Linear RGB -> 10-bit narrow Y, Cb, Cr codes (unrounded).
    void yuv(const double rgb[3], double out[3]) const {
        const double c[3] = {code(rgb[0]), code(rgb[1]), code(rgb[2])};
        const double yn = m_inv[0] * c[0] + m_inv[1] * c[1] + m_inv[2] * c[2];
        const double un = m_inv[3] * c[0] + m_inv[4] * c[1] + m_inv[5] * c[2];
        const double vn = m_inv[6] * c[0] + m_inv[7] * c[1] + m_inv[8] * c[2];
        out[0] = yn / static_cast<double>(m_cp.yuvScaleY) + static_cast<double>(m_cp.yuvBlack);
        out[1] = un / static_cast<double>(m_cp.yuvScaleC) + 512.0;
        out[2] = vn / static_cast<double>(m_cp.yuvScaleC) + 512.0;
    }

private:
    static constexpr int kN = 8192;
    OsvColorParams m_cp;
    std::vector<double> m_lin;
    double m_inv[9] = {};
};

/// Quantise to a 10-bit code.
std::uint16_t q10(double v) { return static_cast<std::uint16_t>(std::clamp(std::lround(v), 0L, 1023L)); }

/// Render lens `lens` of `rig` from `scene`: every luma pixel and every
/// chroma site is unprojected through the lens model to a body ray, lit by
/// `scene`, D-Log M encoded and stored as 10-bit narrow Y'CbCr - the inverse
/// of what the kernel does, so the kernel reads back what the scene says.
SynthFrame synthLens(const geom::LensRig& rig, int lens, const Radiance& scene, const CodeEncoder& enc,
                     ThreadPool& pool) {
    const std::uint32_t w = static_cast<std::uint32_t>(rig.streamW);
    const std::uint32_t h = static_cast<std::uint32_t>(rig.streamH);
    const std::uint32_t cw = (w + 1) / 2;
    const std::uint32_t ch = (h + 1) / 2;
    SynthFrame s;
    s.storage = std::make_shared<std::vector<std::uint16_t>>(static_cast<std::size_t>(w) * h +
                                                             2u * static_cast<std::size_t>(cw) * ch);
    std::uint16_t* Y = s.storage->data();
    std::uint16_t* U = Y + static_cast<std::size_t>(w) * h;
    std::uint16_t* V = U + static_cast<std::size_t>(cw) * ch;
    const geom::KannalaBrandt5& kb = rig.lens[static_cast<std::size_t>(lens)];
    const Mat3d lensToBody = rig.bodyToLens[static_cast<std::size_t>(lens)].transposed();

    // YCbCr of the scene at continuous lens pixel (px, py); black outside the circle.
    const auto sample = [&](double px, double py, double out[3]) {
        auto d = kb.unproject(Vec2d{px, py});
        if (!d.ok()) {
            out[0] = 64.0;
            out[1] = out[2] = 512.0;
            return;
        }
        const Vec3d dl = d.value();
        const double theta = rad2deg(std::atan2(std::hypot(dl.x, dl.y), dl.z));
        double rgb[3] = {0, 0, 0};
        scene(lens, lensToBody * dl, theta, rgb);
        enc.yuv(rgb, out);
    };
    // Luma at pixel centres; chroma at the kernel's left-sited positions
    // (chroma sample (j, i) sits at luma (2j + 0.5, 2i + 1), osvSampleYuv).
    (void)pool.parallelFor(0, h, 8, [&](std::size_t y0, std::size_t y1) {
        for (std::size_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                double v[3];
                sample(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5, v);
                Y[y * w + x] = q10(v[0]);
            }
        }
    });
    (void)pool.parallelFor(0, ch, 8, [&](std::size_t i0, std::size_t i1) {
        for (std::size_t i = i0; i < i1; ++i) {
            for (std::uint32_t j = 0; j < cw; ++j) {
                double v[3];
                sample(2.0 * j + 0.5, 2.0 * static_cast<double>(i) + 1.0, v);
                U[i * cw + j] = q10(v[1]);
                V[i * cw + j] = q10(v[2]);
            }
        }
    });
    s.frame.width = w;
    s.frame.height = h;
    s.frame.chromaW = cw;
    s.frame.chromaH = ch;
    s.frame.plane = {Y, U, V};
    s.frame.strideElems = {w, cw, cw};
    s.frame.bitDepth = 10;
    s.frame.bitShift = 0;
    s.frame.chromaInterleaved = false;
    s.frame.narrowRange = true;
    s.frame.owner = s.storage;
    return s;
}

/// Both lenses of a synthetic scene, kept alive together with their pair.
struct SynthPair {
    SynthFrame lens[2];
    video::FramePair pair;
};

SynthPair synthPair(const geom::LensRig& rig, const Radiance& scene, ThreadPool& pool) {
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
    const CodeEncoder enc(cp);
    SynthPair p;
    for (int i = 0; i < 2; ++i) {
        p.lens[i] = synthLens(rig, i, scene, enc, pool);
    }
    p.pair.lens = {p.lens[0].frame, p.lens[1].frame};
    return p;
}

/// Polar-axis longitude / latitude (radians) of a body direction.
double polarLon(const Vec3d& d) { return std::atan2(d.x, d.z); }
double polarLat(const Vec3d& d) { return std::asin(std::clamp(d.y, -1.0, 1.0)); }

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
