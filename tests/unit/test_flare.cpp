// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Flare removal tests (WP-FLARE): the kernel primitives, ghost detection on
// synthetic frames with known parameters, the bounds of the subtraction, the
// seam cost hook, the veil estimate, CPU / GPU parity and the sample clip.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/Flare.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamCarve.h"
#include "osv/video/DualStreamReader.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#include "osv/render/FlareCuda.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <type_traits>
#include <vector>

using namespace osv;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// -----------------------------------------------------------------------------
//  Synthetic scene: a lens working image with a sun, a sky and a ghost
// -----------------------------------------------------------------------------

/// A synthetic lens in analysis pixels (factor 1): optical centre in the
/// middle of a 600 x 600 image, focal 150 px, pure equidistant model.
geom::KannalaBrandt5 syntheticLens() {
    geom::KannalaBrandt5 lens;
    lens.fx = lens.fy = 150.0;
    lens.cx = lens.cy = 300.0;
    lens.k = {0.0, 0.0, 0.0, 0.0, 0.0};
    lens.updateRMax();
    return lens;
}

/// The ghost the synthetic scenes plant (analysis = stream px, factor 1).
render::FlareGhost plantedGhost() {
    render::FlareGhost g;
    g.cx = 150.0;
    g.cy = 312.0;
    g.hx = 14.0;
    g.hy = 8.0;
    g.radius = 5.0;
    g.angleRad = 0.2;
    g.soft = 2.0;
    g.amp = {0.060, 0.050, 0.040};
    return g;
}

/// Plateau weight of `g` at (x, y), evaluated through the KERNEL's function
/// so the synthetic data is exactly the model the analysis fits.
double kernelShape(const render::FlareGhost& g, double x, double y) {
    OsvRenderParams p{};
    render::FlareModel m;
    m.lens[0].ghosts.push_back(g);
    render::applyFlare(m, p);
    return osvFlareGhostShape(&p.flare[0].ghost[0], static_cast<float>(x), static_cast<float>(y));
}

/// The kernel's ghost block for `g` (plateau, rim and tilt).
OsvFlareGhost kernelGhost(const render::FlareGhost& g) {
    OsvRenderParams p{};
    render::FlareModel m;
    m.lens[0].ghosts.push_back(g);
    render::applyFlare(m, p);
    return p.flare[0].ghost[0];
}

struct SceneOptions {
    bool sun = true;                ///< Plant the sun disc.
    bool ghost = true;              ///< Plant the ghost.
    double texture = 0.0;           ///< Relative amplitude of random scene texture.
    double noise = 0.0008;          ///< Absolute Gaussian noise.
    render::FlareGhost ghostParams = plantedGhost();
};

/// Sky with a gentle gradient, the sun at (240, 300) and optionally the
/// ghost and a textured "city" everywhere.
render::FlareImage makeScene(const SceneOptions& o) {
    render::FlareImage img;
    img.w = img.h = 600;
    img.factor = 1;
    img.rgb.assign(600u * 600u * 3u, 0.0f);
    std::mt19937 rng(1234);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const OsvFlareGhost kg = kernelGhost(o.ghostParams);
    for (std::uint32_t y = 0; y < img.h; ++y) {
        for (std::uint32_t x = 0; x < img.w; ++x) {
            const double px = x + 0.5;
            const double py = y + 0.5;
            // Blue sky, brighter toward the right and near the sun.
            const double base = 0.10 + 0.04 * px / 600.0 + 0.02 * py / 600.0;
            double rgb[3] = {0.6 * base, 0.9 * base, 1.4 * base};
            if (o.texture > 0.0) {
                // Blocky random texture (a city seen from above).
                std::mt19937 cell(static_cast<unsigned>((x / 3) * 7919u + (y / 3) * 104729u));
                std::uniform_real_distribution<double> u(-1.0, 1.0);
                const double t = 1.0 + o.texture * u(cell);
                for (double& c : rgb) {
                    c *= t;
                }
            }
            if (o.ghost) {
                // The ghost's light exactly as the kernel models it.
                float light[3] = {0.0f, 0.0f, 0.0f};
                if (osvFlareGhostLight(&kg, static_cast<float>(px), static_cast<float>(py), light)) {
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] += light[c];
                    }
                }
            }
            if (o.sun) {
                const double r = std::hypot(px - 240.0, py - 300.0);
                if (r < 8.0) {
                    rgb[0] = rgb[1] = rgb[2] = 3.76;
                }
            }
            float* dst = &img.rgb[(static_cast<std::size_t>(y) * img.w + x) * 3u];
            for (int c = 0; c < 3; ++c) {
                dst[c] = static_cast<float>(rgb[c] + o.noise * gauss(rng));
            }
        }
    }
    return img;
}

// -----------------------------------------------------------------------------
//  Synthetic lens frames for the kernel tests
// -----------------------------------------------------------------------------

/// Owns planar 10-bit YCbCr storage for a constant-colour frame.
struct SyntheticFrame {
    std::shared_ptr<std::vector<std::uint16_t>> storage;
    video::PlanarFrame16 frame;
};

SyntheticFrame makeConstantFrame(std::uint32_t w, std::uint32_t h, std::uint16_t y, std::uint16_t cb,
                                 std::uint16_t cr) {
    SyntheticFrame s;
    const std::uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    const std::size_t lumaN = static_cast<std::size_t>(w) * h;
    const std::size_t chromaN = static_cast<std::size_t>(cw) * ch;
    s.storage = std::make_shared<std::vector<std::uint16_t>>(lumaN + 2 * chromaN);
    std::uint16_t* base = s.storage->data();
    std::fill(base, base + lumaN, y);
    std::fill(base + lumaN, base + lumaN + chromaN, cb);
    std::fill(base + lumaN + chromaN, base + lumaN + 2 * chromaN, cr);
    s.frame.width = w;
    s.frame.height = h;
    s.frame.chromaW = cw;
    s.frame.chromaH = ch;
    s.frame.plane = {base, base + lumaN, base + lumaN + chromaN};
    s.frame.strideElems = {w, cw, cw};
    s.frame.bitDepth = 10;
    s.frame.bitShift = 0;
    s.frame.chromaInterleaved = false;
    s.frame.narrowRange = true;
    s.frame.owner = s.storage;
    return s;
}

/// The sample clip's calibration as a stream-space rig (verified constants,
/// no metadata decoder involved), the same helper test_render.cpp uses.
Result<geom::LensRig> makeSampleRig(int streamW) {
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

// -----------------------------------------------------------------------------
//  Sample clip
// -----------------------------------------------------------------------------

struct SamplePipeline {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
    video::FramePair pair;
};

Result<SamplePipeline> openSampleFrame(std::uint32_t frameIndex) {
    SamplePipeline s;
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
    OSV_TRY_ASSIGN(video::DualStreamReader reader, video::DualStreamReader::open(osvtest::sampleOsv(), s.format));
    OSV_TRY_ASSIGN(s.pair, reader.read(frameIndex));
    return s;
}

OsvColorParams linearColor() {
    return color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
}

}  // namespace

// =============================================================================
//  Kernel primitives
// =============================================================================

TEST_CASE("flare ghost shape: plateau inside, half at the edge, exactly zero past its reach", "[flare]") {
    render::FlareGhost g = plantedGhost();
    g.angleRad = 0.0;
    OsvRenderParams p{};
    render::FlareModel m;
    m.lens[1].ghosts.push_back(g);
    render::applyFlare(m, p);
    REQUIRE(p.flareEnabled == 1);
    REQUIRE(p.flare[1].ghostCount == 1);
    REQUIRE(p.flare[0].ghostCount == 0);
    const OsvFlareGhost& k = p.flare[1].ghost[0];
    // Centre and the middle of the long edge.
    REQUIRE_THAT(osvFlareGhostShape(&k, 150.0f, 312.0f), WithinAbs(1.0, 1e-6));
    REQUIRE_THAT(osvFlareGhostShape(&k, 150.0f + 14.0f, 312.0f), WithinAbs(0.5, 1e-5));
    REQUIRE_THAT(osvFlareGhostShape(&k, 150.0f, 312.0f + 8.0f), WithinAbs(0.5, 1e-5));
    // Past the ramp: zero.
    REQUIRE(osvFlareGhostShape(&k, 150.0f + 14.0f + 2.01f, 312.0f) == 0.0f);
    // Anywhere beyond the bounding radius: exactly zero.
    const double reach = g.reach();
    for (int a = 0; a < 16; ++a) {
        const double ang = a * kTwoPi / 16.0;
        const float x = static_cast<float>(150.0 + (reach + 1.5) * std::cos(ang));
        const float y = static_cast<float>(312.0 + (reach + 1.5) * std::sin(ang));
        REQUIRE(osvFlareGhostShape(&k, x, y) == 0.0f);
    }
    // Rotating by 90 degrees and swapping the extents describes the same shape.
    render::FlareGhost g2 = g;
    g2.angleRad = 0.5 * kPi;
    std::swap(g2.hx, g2.hy);
    OsvRenderParams p2{};
    render::FlareModel m2;
    m2.lens[0].ghosts.push_back(g2);
    render::applyFlare(m2, p2);
    for (int i = 0; i < 50; ++i) {
        const float x = 130.0f + static_cast<float>(i);
        const float y = 305.0f + 0.3f * static_cast<float>(i);
        REQUIRE_THAT(osvFlareGhostShape(&p2.flare[0].ghost[0], x, y), WithinAbs(osvFlareGhostShape(&k, x, y), 1e-5));
    }
}

TEST_CASE("flare soft subtraction never makes light negative and never inverts a gradient", "[flare]") {
    // Identity for a zero estimate and for non-positive inputs.
    REQUIRE(osvFlareSoftSubtract(0.3f, 0.0f) == 0.3f);
    REQUIRE(osvFlareSoftSubtract(-0.1f, 0.2f) == -0.1f);
    REQUIRE(osvFlareSoftSubtract(0.0f, 0.2f) == 0.0f);
    // Full subtraction well above the estimate.
    REQUIRE_THAT(osvFlareSoftSubtract(10.0f, 0.1f), WithinAbs(9.9, 1e-4));
    // Over a dense grid: f >= 0.47 x, strictly increasing in x.
    for (float g : {0.001f, 0.01f, 0.05f, 0.2f, 1.0f, 4.0f}) {
        float prev = -1.0f;
        for (int i = 1; i <= 4000; ++i) {
            const float x = 1e-4f * static_cast<float>(i) * static_cast<float>(i);
            const float f = osvFlareSoftSubtract(x, g);
            REQUIRE(f >= 0.47f * x);
            REQUIRE(f <= x);
            REQUIRE(f > prev);
            prev = f;
        }
    }
}

TEST_CASE("osvFlareRemove leaves every pixel outside the ghosts bit-identical", "[flare]") {
    render::FlareModel m;
    m.lens[0].ghosts.push_back(plantedGhost());
    OsvRenderParams p{};
    render::applyFlare(m, p);
    const double reach = plantedGhost().reach();
    int inside = 0;
    for (int y = 250; y < 380; y += 3) {
        for (int x = 80; x < 230; x += 3) {
            float rgb[3] = {0.12f, 0.2f, 0.31f};
            const float before[3] = {rgb[0], rgb[1], rgb[2]};
            osvFlareRemove(&p.flare[0], static_cast<float>(x), static_cast<float>(y), rgb);
            if (std::hypot(x - 150.0, y - 312.0) > reach + 1.0) {
                REQUIRE(std::memcmp(rgb, before, sizeof(rgb)) == 0);
            } else if (kernelShape(plantedGhost(), x, y) > 0.99) {
                ++inside;
                // On the plateau the full amplitude comes off (to the soft
                // knee's precision) and nothing goes negative.
                for (int c = 0; c < 3; ++c) {
                    REQUIRE(rgb[c] > 0.0f);
                    REQUIRE(rgb[c] < before[c]);
                }
                REQUIRE_THAT(rgb[0], WithinAbs(0.12 - 0.06, 0.02));
            }
        }
    }
    REQUIRE(inside > 5);
}

// =============================================================================
//  Detection and fitting on synthetic frames with known parameters
// =============================================================================

TEST_CASE("analyseLensFlare finds the sun and recovers a planted ghost", "[flare]") {
    const render::FlareImage img = makeScene(SceneOptions{});
    auto res = render::analyseLensFlare(img, syntheticLens(), render::FlareParams{});
    REQUIRE(res.ok());
    const render::LensFlare& lf = res.value();
    REQUIRE(lf.sunFound);
    REQUIRE_THAT(lf.sunX, WithinAbs(240.0, 0.5));
    REQUIRE_THAT(lf.sunY, WithinAbs(300.0, 0.5));
    REQUIRE_THAT(lf.sunRadiusPx, WithinAbs(8.0, 1.0));
    // 60 px left of centre at f = 150: theta = 0.4 rad.
    REQUIRE_THAT(lf.sunThetaRad, WithinAbs(0.4, 0.01));
    REQUIRE(lf.ghosts.size() == 1);
    const render::FlareGhost& g = lf.ghosts[0];
    const render::FlareGhost t = plantedGhost();
    INFO("fit: c(" << g.cx << ", " << g.cy << ") h(" << g.hx << ", " << g.hy << ") r " << g.radius << " a "
                   << g.angleRad << " soft " << g.soft << " amp " << g.amp[0] << " " << g.amp[1] << " " << g.amp[2]
                   << " contrast " << g.contrast << " R2 " << g.fitR2);
    REQUIRE_THAT(g.cx, WithinAbs(t.cx, 0.3));
    REQUIRE_THAT(g.cy, WithinAbs(t.cy, 0.3));
    REQUIRE_THAT(g.hx, WithinAbs(t.hx, 0.5));
    REQUIRE_THAT(g.hy, WithinAbs(t.hy, 0.5));
    REQUIRE_THAT(g.angleRad, WithinAbs(t.angleRad, 0.03));
    REQUIRE_THAT(g.soft, WithinAbs(t.soft, 0.5));
    for (std::size_t c = 0; c < 3; ++c) {
        REQUIRE_THAT(g.amp[c], WithinRel(t.amp[c], 0.08));
        // Nothing planted, nothing fitted.
        REQUIRE_THAT(g.rim[c], WithinAbs(0.0, 0.004));
        REQUIRE_THAT(g.gradX[c], WithinAbs(0.0, 0.003));
        REQUIRE_THAT(g.gradY[c], WithinAbs(0.0, 0.003));
    }
    REQUIRE(g.fitR2 > 0.9);
}

TEST_CASE("analyseLensFlare recovers a ghost's caustic rim and brightness tilt", "[flare]") {
    SceneOptions o;
    o.ghostParams.hx = 18.0;
    o.ghostParams.hy = 11.0;
    o.ghostParams.rim = {0.030, 0.025, 0.020};
    o.ghostParams.gradX = {0.015, 0.012, 0.010};
    o.ghostParams.gradY = {-0.010, -0.008, -0.006};
    auto res = render::analyseLensFlare(makeScene(o), syntheticLens(), render::FlareParams{});
    REQUIRE(res.ok());
    REQUIRE(res.value().ghosts.size() == 1);
    const render::FlareGhost& g = res.value().ghosts[0];
    INFO("rim " << g.rim[0] << " gradX " << g.gradX[0] << " gradY " << g.gradY[0] << " amp " << g.amp[0]);
    REQUIRE_THAT(g.cx, WithinAbs(o.ghostParams.cx, 0.3));
    REQUIRE_THAT(g.hx, WithinAbs(o.ghostParams.hx, 0.5));
    for (std::size_t c = 0; c < 3; ++c) {
        REQUIRE_THAT(g.amp[c], WithinAbs(o.ghostParams.amp[c], 0.004));
        REQUIRE_THAT(g.rim[c], WithinAbs(o.ghostParams.rim[c], 0.004));
        REQUIRE_THAT(g.gradX[c], WithinAbs(o.ghostParams.gradX[c], 0.003));
        REQUIRE_THAT(g.gradY[c], WithinAbs(o.ghostParams.gradY[c], 0.003));
    }
}

TEST_CASE("analyseLensFlare removes nothing without a sun, on texture, or off the sun line", "[flare]") {
    render::FlareParams fp;
    SECTION("no sun in the frame") {
        SceneOptions o;
        o.sun = false;
        auto res = render::analyseLensFlare(makeScene(o), syntheticLens(), fp);
        REQUIRE(res.ok());
        REQUIRE_FALSE(res.value().sunFound);
        REQUIRE(res.value().ghosts.empty());
    }
    SECTION("a bump on scene texture is never taken for a ghost") {
        SceneOptions o;
        o.texture = 0.25;
        auto res = render::analyseLensFlare(makeScene(o), syntheticLens(), fp);
        REQUIRE(res.ok());
        REQUIRE(res.value().sunFound);
        REQUIRE(res.value().ghosts.empty());
    }
    SECTION("a bump far off the sun line is scene, not a reflection") {
        SceneOptions o;
        // Straight above the optical centre: 90 degrees from the sun line.
        o.ghostParams.cx = 300.0;
        o.ghostParams.cy = 180.0;
        auto res = render::analyseLensFlare(makeScene(o), syntheticLens(), fp);
        REQUIRE(res.ok());
        REQUIRE(res.value().ghosts.empty());
    }
    SECTION("the mirrored side of the axis is searched too") {
        SceneOptions o;
        o.ghostParams.cx = 420.0;  // opposite the sun, point-symmetric side
        o.ghostParams.cy = 296.0;
        auto res = render::analyseLensFlare(makeScene(o), syntheticLens(), fp);
        REQUIRE(res.ok());
        REQUIRE(res.value().ghosts.size() == 1);
        REQUIRE_THAT(res.value().ghosts[0].cx, WithinAbs(420.0, 0.3));
    }
    SECTION("invalid inputs are refused") {
        render::FlareImage bad;
        REQUIRE_FALSE(render::analyseLensFlare(bad, syntheticLens(), fp).ok());
        geom::KannalaBrandt5 noLens;
        REQUIRE_FALSE(render::analyseLensFlare(makeScene(SceneOptions{}), noLens, fp).ok());
        render::FlareParams badParams;
        badParams.sunLevelFraction = std::nan("");
        REQUIRE_FALSE(render::analyseLensFlare(makeScene(SceneOptions{}), syntheticLens(), badParams).ok());
    }
}

TEST_CASE("subtracting a fitted ghost flattens it and leaves the rest of the frame alone", "[flare]") {
    const render::FlareImage img = makeScene(SceneOptions{});
    SceneOptions clean;
    clean.ghost = false;
    const render::FlareImage ref = makeScene(clean);
    auto res = render::analyseLensFlare(img, syntheticLens(), render::FlareParams{});
    REQUIRE(res.ok());
    REQUIRE(res.value().ghosts.size() == 1);
    render::FlareModel m;
    m.lens[0] = res.value();
    OsvRenderParams p{};
    render::applyFlare(m, p);
    double errBefore = 0.0, errAfter = 0.0;
    std::size_t touched = 0;
    for (std::uint32_t y = 0; y < img.h; ++y) {
        for (std::uint32_t x = 0; x < img.w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * img.w + x) * 3u;
            float rgb[3] = {img.rgb[i], img.rgb[i + 1], img.rgb[i + 2]};
            const float before[3] = {rgb[0], rgb[1], rgb[2]};
            osvFlareRemove(&p.flare[0], x + 0.5f, y + 0.5f, rgb);
            if (std::memcmp(rgb, before, sizeof(rgb)) != 0) {
                ++touched;
                // Only ever inside the fitted ghost's reach.
                REQUIRE(std::hypot(x + 0.5 - res.value().ghosts[0].cx, y + 0.5 - res.value().ghosts[0].cy) <=
                        res.value().ghosts[0].reach() + 1.0);
                for (int c = 0; c < 3; ++c) {
                    REQUIRE(rgb[c] >= 0.0f);
                    errBefore += std::fabs(before[c] - ref.rgb[i + static_cast<std::size_t>(c)]);
                    errAfter += std::fabs(rgb[c] - ref.rgb[i + static_cast<std::size_t>(c)]);
                }
            }
        }
    }
    REQUIRE(touched > 100);
    INFO("mean abs error vs the clean scene: before " << errBefore / touched << ", after " << errAfter / touched);
    // At least 85 % of the ghost's light is gone.
    REQUIRE(errAfter < 0.15 * errBefore);
}

TEST_CASE("a ghost removed in the render kernel changes only the pixels it covers", "[flare]") {
    // Two flat grey lenses, a big ghost planted at the master's centre: a
    // forward-looking rectilinear view sees it, and every pixel of the render
    // outside its reach must be bit-identical to the render without removal.
    auto rig = makeSampleRig(1024);
    REQUIRE(rig.ok());
    SyntheticFrame s = makeConstantFrame(1024, 1024, 500, 512, 512);
    SyntheticFrame m = makeConstantFrame(1024, 1024, 500, 512, 512);
    video::FramePair pair;
    pair.lens[0] = s.frame;
    pair.lens[1] = m.frame;
    geom::VirtualCamera cam;
    cam.w = 320;
    cam.h = 180;
    cam.hfovDeg = 90;
    const OsvColorParams cp = linearColor();
    auto job = render::RenderParamsBuilder().rig(rig.value()).camera(cam).color(cp).build(pair);
    REQUIRE(job.ok());
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    auto plain = cpu.render(job.value());
    REQUIRE(plain.ok());

    render::FlareModel model;
    render::FlareGhost g;
    g.cx = rig.value().lens[1].cx + 60.0;
    g.cy = rig.value().lens[1].cy;
    g.hx = 40.0;
    g.hy = 25.0;
    g.radius = 10.0;
    g.soft = 4.0;
    g.amp = {0.05, 0.05, 0.05};
    model.lens[1].ghosts.push_back(g);
    render::RenderJob flared = job.value();
    render::applyFlare(model, flared.params);
    auto removed = cpu.render(flared);
    REQUIRE(removed.ok());

    std::size_t changed = 0;
    std::size_t identical = 0;
    for (std::size_t i = 0; i < plain.value().data.size(); ++i) {
        const float a = plain.value().data[i];
        const float b = removed.value().data[i];
        REQUIRE(b >= 0.0f);
        REQUIRE(b <= a);
        if (a != b) {
            ++changed;
        } else {
            ++identical;
        }
    }
    // The ghost covers a small part of the view: most of it must be untouched.
    INFO("changed " << changed << " identical " << identical);
    REQUIRE(changed > 0);
    REQUIRE(identical > 10 * changed);
}

// =============================================================================
//  CPU downsample
// =============================================================================

TEST_CASE("flareDownsample averages linear light over each block", "[flare]") {
    // A frame whose left half is code 300 and right half code 700: every
    // 4 x 4 block is pure, except none straddle the split at x = 64.
    const std::uint32_t w = 128, h = 64;
    SyntheticFrame f = makeConstantFrame(w, h, 300, 512, 512);
    auto* luma = f.storage->data();
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = w / 2; x < w; ++x) {
            luma[static_cast<std::size_t>(y) * w + x] = 700;
        }
    }
    ThreadPool pool;
    const OsvColorParams cp = linearColor();
    auto img = render::flareDownsample(f.frame, cp, 4, pool);
    REQUIRE(img.ok());
    REQUIRE(img.value().w == 32);
    REQUIRE(img.value().h == 16);
    float lo[3], hi[3];
    osvYuvToCode(&cp, 300.0f, 512.0f, 512.0f, lo);
    osvCodeToLinear(&cp, lo, lo);
    osvYuvToCode(&cp, 700.0f, 512.0f, 512.0f, hi);
    osvCodeToLinear(&cp, hi, hi);
    REQUIRE_THAT(img.value().rgb[0], WithinRel(lo[0], 1e-5f));
    const std::size_t right = (5u * 32u + 20u) * 3u;
    REQUIRE_THAT(img.value().rgb[right + 1], WithinRel(hi[1], 1e-5f));
    // Odd sizes: the last block is partial and still averaged correctly.
    SyntheticFrame odd = makeConstantFrame(13, 9, 400, 512, 512);
    auto small = render::flareDownsample(odd.frame, cp, 4, pool);
    REQUIRE(small.ok());
    REQUIRE(small.value().w == 4);
    REQUIRE(small.value().h == 3);
    REQUIRE_THAT(small.value().rgb.back(), WithinRel(small.value().rgb[2], 1e-6f));
    // Refusals.
    REQUIRE_FALSE(render::flareDownsample(f.frame, cp, 0, pool).ok());
    REQUIRE_FALSE(render::flareDownsample(f.frame, cp, 17, pool).ok());
    video::PlanarFrame16 empty;
    REQUIRE_FALSE(render::flareDownsample(empty, cp, 4, pool).ok());
}

// =============================================================================
//  Kernel parameter writer and temporal smoothing
// =============================================================================

TEST_CASE("applyFlare validates, clamps and caps what reaches the kernel", "[flare]") {
    render::FlareModel m;
    OsvRenderParams p{};
    render::applyFlare(m, p);
    REQUIRE(p.flareEnabled == 0);
    // Six ghosts, one of them garbage, one with a negative channel.
    for (int i = 0; i < 6; ++i) {
        render::FlareGhost g = plantedGhost();
        g.cx += 40.0 * i;
        m.lens[1].ghosts.push_back(g);
    }
    m.lens[1].ghosts[1].hx = std::nan("");
    m.lens[1].ghosts[2].amp[1] = -0.5;
    m.lens[0].veil = {0.01, -0.02, std::numeric_limits<double>::infinity()};
    render::applyFlare(m, p);
    REQUIRE(p.flareEnabled == 1);
    REQUIRE(p.flare[1].ghostCount == render::kFlareMaxGhosts);
    for (int k = 0; k < p.flare[1].ghostCount; ++k) {
        REQUIRE(std::isfinite(p.flare[1].ghost[k].hx));
        for (float a : p.flare[1].ghost[k].amp) {
            REQUIRE(a >= 0.0f);
        }
    }
    // A non-finite veil is refused as a whole.
    REQUIRE(p.flare[0].veil[0] == 0.0f);
    REQUIRE(p.flare[0].veil[2] == 0.0f);
    render::clearFlare(p);
    REQUIRE(p.flareEnabled == 0);
    REQUIRE(p.flare[1].ghostCount == 0);
}

TEST_CASE("smoothFlare interpolates matched ghosts and fades new and lost ones", "[flare]") {
    render::FlareModel a;
    render::FlareModel b;
    render::FlareGhost g = plantedGhost();
    a.lens[1].ghosts.push_back(g);
    g.cx += 2.0;
    g.amp = {0.1, 0.1, 0.1};
    b.lens[1].ghosts.push_back(g);
    render::FlareGhost fresh = plantedGhost();
    fresh.cx = 400.0;
    b.lens[1].ghosts.push_back(fresh);
    a.lens[0].ghosts.push_back(plantedGhost());  // lost in b

    const render::FlareModel s = render::smoothFlare(a, b, 0.25);
    REQUIRE(s.lens[1].ghosts.size() == 2);
    REQUIRE_THAT(s.lens[1].ghosts[0].cx, WithinAbs(150.5, 1e-9));
    REQUIRE_THAT(s.lens[1].ghosts[0].amp[0], WithinAbs(0.06 + 0.25 * 0.04, 1e-9));
    REQUIRE_THAT(s.lens[1].ghosts[1].amp[0], WithinAbs(0.25 * 0.06, 1e-9));
    REQUIRE(s.lens[0].ghosts.size() == 1);
    REQUIRE_THAT(s.lens[0].ghosts[0].amp[0], WithinAbs(0.75 * 0.06, 1e-9));
    // Weight 1 is "no memory".
    const render::FlareModel same = render::smoothFlare(a, b, 1.0);
    REQUIRE(same.lens[1].ghosts.size() == b.lens[1].ghosts.size());
    REQUIRE(same.lens[0].ghosts.empty());
}

// =============================================================================
//  Seam cost hook
// =============================================================================

TEST_CASE("flareCost is bounded, zero on clean pixels and rises with the ghost", "[flare]") {
    render::LensFlare lf;
    // Nothing measured: zero everywhere.
    REQUIRE(render::flareCost(lf, 100.0, 100.0) == 0.0f);
    lf.ghosts.push_back(plantedGhost());
    // Far away: zero.  On the plateau: flare / (flare + signal).
    REQUIRE(render::flareCost(lf, 500.0, 500.0) == 0.0f);
    const double ampY = 0.2627 * 0.06 + 0.6780 * 0.05 + 0.0593 * 0.04;
    REQUIRE_THAT(render::flareCost(lf, 150.0, 312.0, 0.2), WithinAbs(ampY / (ampY + 0.2), 1e-6));
    // Reference signal when none is given.
    REQUIRE_THAT(render::flareCost(lf, 150.0, 312.0), WithinAbs(ampY / (ampY + 0.18), 1e-6));
    // A brighter pixel is less contaminated by the same flare.
    REQUIRE(render::flareCost(lf, 150.0, 312.0, 1.0) < render::flareCost(lf, 150.0, 312.0, 0.1));
    // Sun glare: maximal on the disc, gone past sunGlareRadii.
    lf.sunFound = true;
    lf.sunX = 240.0;
    lf.sunY = 300.0;
    lf.sunRadiusPx = 8.0;
    REQUIRE(render::flareCost(lf, 240.0, 300.0) > 0.99f);
    REQUIRE(render::flareCost(lf, 240.0 + 8.0 * 4.5, 300.0) == 0.0f);
    // Bounded and robust.
    lf.veil = {10.0, 10.0, 10.0};
    const float c = render::flareCost(lf, 150.0, 312.0, 1e-9);
    REQUIRE(c >= 0.0f);
    REQUIRE(c < 1.0f);
    REQUIRE(render::flareCost(lf, std::nan(""), 1.0) == 0.0f);
    REQUIRE(render::flareCost(lf, 1.0, std::numeric_limits<double>::infinity()) == 0.0f);
}

TEST_CASE("flareCostBand maps the polar-axis band through each lens", "[flare]") {
    auto rig = makeSampleRig(3000);
    REQUIRE(rig.ok());
    render::FlareModel model;
    model.lens[1].veil = {0.05, 0.05, 0.05};
    std::array<std::vector<float>, 2> cost;
    const std::uint32_t mapW = 512, mapH = 256, row0 = 110, rows = 36;
    REQUIRE(render::flareCostBand(model, rig.value(), mapW, mapH, row0, rows, nullptr, cost).ok());
    REQUIRE(cost[0].size() == static_cast<std::size_t>(mapW) * rows);
    // Slave: clean wherever it sees; master: the veil fraction everywhere it sees.
    const float veilCost = static_cast<float>(0.05 / (0.05 + 0.18));
    std::size_t seenBoth = 0;
    for (std::size_t i = 0; i < cost[0].size(); ++i) {
        REQUIRE(cost[0][i] >= 0.0f);
        REQUIRE(cost[0][i] <= 1.0f);
        if (cost[0][i] < 1.0f && cost[1][i] < 1.0f) {
            ++seenBoth;
            REQUIRE(cost[0][i] == 0.0f);
            REQUIRE_THAT(cost[1][i], WithinAbs(veilCost, 1e-6));
        }
    }
    // The equator rows of the band are seen by both lenses.
    REQUIRE(seenBoth > static_cast<std::size_t>(mapW) * 4);
    // Bad geometry is refused.
    REQUIRE_FALSE(render::flareCostBand(model, rig.value(), mapW, mapH, 250, 10, nullptr, cost).ok());
    std::vector<float> wrong(3);
    const std::array<const std::vector<float>*, 2> sig{&wrong, nullptr};
    REQUIRE_FALSE(render::flareCostBand(model, rig.value(), mapW, mapH, row0, rows, &sig, cost).ok());
}

TEST_CASE("FlareSeamPenalty feeds the seam a ghost in the overlap and nothing else", "[flare]") {
    auto rig = makeSampleRig(3000);
    REQUIRE(rig.ok());
    // A band like the seam's: 1024 columns of a 1024 x 512 polar-axis map,
    // +/-6 degrees around the equator.
    render::LensBands band;
    band.w = 1024;
    band.mapH = 512;
    band.rowOffset = 256 - 17;
    band.h = 34;
    const std::size_t n = static_cast<std::size_t>(band.w) * band.h;
    // Put a ghost in the master exactly where band pixel (300, 17) looks.
    const std::uint32_t col = 300, row = 17;
    const double lon = (col + 0.5) / band.w * 2.0 * kPi - kPi;
    const double lat = 0.5 * kPi - (band.rowOffset + row + 0.5) / band.mapH * kPi;
    const Vec3d d{std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)};
    Vec2d px;
    double theta = 0.0;
    REQUIRE(rig.value().projectBody(1, d, px, theta));
    render::FlareModel model;
    model.lens[1].sunFound = true;
    model.lens[1].sunX = 100.0;  // far from the band: no glare term there
    model.lens[1].sunY = 100.0;
    model.lens[1].sunRadiusPx = 20.0;
    render::FlareGhost g = plantedGhost();
    g.cx = px.x;
    g.cy = px.y;
    model.lens[1].ghosts.push_back(g);
    model.lens[1].veil = {0.05, 0.05, 0.05};  // must NOT reach the seam by default

    render::FlareSeamPenalty source;
    std::vector<float> ps(n, 0.0f), pm(n, 0.0f);
    // No model yet: contributes nothing and leaves the maps alone.
    REQUIRE_FALSE(render::FlareSeamPenalty::hook(band, ps, pm, &source));
    REQUIRE_FALSE(render::FlareSeamPenalty::hook(band, ps, pm, nullptr));
    source.update(model, rig.value());
    REQUIRE(render::FlareSeamPenalty::hook(band, ps, pm, &source));
    const std::size_t k = static_cast<std::size_t>(row) * band.w + col;
    const double ampY = 0.2627 * 0.06 + 0.6780 * 0.05 + 0.0593 * 0.04;
    REQUIRE_THAT(pm[k], WithinAbs(ampY / (ampY + 0.18), 1e-3));
    REQUIRE(ps[k] == 0.0f);
    // Every value is a bounded cost; everything away from the ghost is 0
    // (no veil, no coverage term).
    std::size_t nonzero = 0;
    for (std::size_t i = 0; i < n; ++i) {
        REQUIRE(ps[i] == 0.0f);
        REQUIRE(pm[i] >= 0.0f);
        REQUIRE(pm[i] < 1.0f);
        nonzero += pm[i] > 0.0f ? 1u : 0u;
    }
    REQUIRE(nonzero > 0);
    REQUIRE(nonzero < n / 50);
    // Wrongly sized maps are refused, not resized.
    std::vector<float> small(10, 0.0f);
    REQUIRE_FALSE(render::FlareSeamPenalty::hook(band, small, pm, &source));
    REQUIRE(small.size() == 10);
    // No sun anywhere: nothing to steer around.
    model.lens[1].sunFound = false;
    source.update(model, rig.value());
    REQUIRE_FALSE(render::FlareSeamPenalty::hook(band, ps, pm, &source));
    source.clear();
    REQUIRE_FALSE(render::FlareSeamPenalty::hook(band, ps, pm, &source));
}

// The adapter is WP-SEAM's hook type exactly: the one registration call the
// importer needs compiles, installs and uninstalls.
static_assert(std::is_same_v<decltype(&render::FlareSeamPenalty::hook), render::SeamLensPenaltyFn>,
              "FlareSeamPenalty::hook must match SeamLensPenaltyFn");

TEST_CASE("FlareSeamPenalty registers in WP-SEAM's flare slot", "[flare]") {
    const render::SeamPenaltyHook before = render::seamPenaltyHook(render::SeamPenaltySlot::Flare);
    render::FlareSeamPenalty source;
    render::setSeamPenaltyHook(render::SeamPenaltySlot::Flare, {&render::FlareSeamPenalty::hook, &source, 1.0});
    const render::SeamPenaltyHook installed = render::seamPenaltyHook(render::SeamPenaltySlot::Flare);
    REQUIRE(installed.installed());
    REQUIRE(installed.fn == &render::FlareSeamPenalty::hook);
    REQUIRE(installed.user == &source);
    // Leave the process-wide slot as it was for the rest of the suite.
    render::setSeamPenaltyHook(render::SeamPenaltySlot::Flare, before);
    REQUIRE(render::seamPenaltyHook(render::SeamPenaltySlot::Flare).fn == before.fn);
}

// =============================================================================
//  Veil estimate
// =============================================================================

TEST_CASE("estimateVeil separates an additive veil from an exposure mismatch", "[flare]") {
    render::LensBands b;
    b.w = 2048;
    b.h = 20;
    const std::size_t n = static_cast<std::size_t>(b.w) * b.h;
    for (int i = 0; i < 2; ++i) {
        b.luma[i].assign(n, 0.0f);
        b.alpha[i].assign(n, 1.0f);
    }
    // Clean lens: piecewise-constant levels between 0.05 and 0.3 per 64
    // columns; the sun lens reads 1.05 x clean + 0.04.
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> lvl(0.05, 0.3);
    for (std::uint32_t c0 = 0; c0 < b.w; c0 += 64) {
        const double v = lvl(rng);
        for (std::uint32_t r = 0; r < b.h; ++r) {
            for (std::uint32_t c = c0; c < c0 + 64; ++c) {
                const std::size_t i = static_cast<std::size_t>(r) * b.w + c;
                b.luma[0][i] = static_cast<float>(v);
                b.luma[1][i] = static_cast<float>(1.05 * v + 0.04);
            }
        }
    }
    render::FlareModel model;
    SECTION("the veil goes to the lens that sees the sun") {
        model.lens[1].sunFound = true;
        auto veil = render::estimateVeil(b, model);
        REQUIRE(veil.ok());
        REQUIRE_THAT(veil.value()[1][0], WithinAbs(0.04, 1e-4));
        REQUIRE(veil.value()[0][0] == 0.0);
    }
    SECTION("no sun, or the sun in both lenses: no estimate") {
        auto none = render::estimateVeil(b, model);
        REQUIRE(none.ok());
        REQUIRE(none.value()[0][0] == 0.0);
        REQUIRE(none.value()[1][0] == 0.0);
        model.lens[0].sunFound = model.lens[1].sunFound = true;
        auto both = render::estimateVeil(b, model);
        REQUIRE(both.ok());
        REQUIRE(both.value()[1][0] == 0.0);
    }
    SECTION("a darker sun lens is not a veil") {
        model.lens[0].sunFound = true;  // lens 0 reads darker by construction
        auto veil = render::estimateVeil(b, model);
        REQUIRE(veil.ok());
        REQUIRE(veil.value()[0][0] == 0.0);
    }
    SECTION("mismatched sizes are refused") {
        b.luma[1].resize(10);
        model.lens[1].sunFound = true;
        REQUIRE_FALSE(render::estimateVeil(b, model).ok());
    }
}

// =============================================================================
//  Sample clip and GPU parity
// =============================================================================

TEST_CASE("the sample clip's sun and its brightest ghost are found in the master lens", "[flare][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto sp = openSampleFrame(0);
    REQUIRE(sp.ok());
    ThreadPool pool;
    auto model = render::analyseFlare(sp.value().rig, sp.value().pair, linearColor(), render::FlareParams{}, pool);
    REQUIRE(model.ok());
    const render::LensFlare& master = model.value().lens[1];
    const render::LensFlare& slave = model.value().lens[0];
    // Measured by hand on the raw lens frame: sun disc centroid (1346.5,
    // 1494.1), about 10 degrees off the master's axis; no sun in the slave.
    REQUIRE(master.sunFound);
    REQUIRE_FALSE(slave.sunFound);
    REQUIRE(slave.ghosts.empty());
    REQUIRE_THAT(master.sunX, WithinAbs(1346.5, 4.0));
    REQUIRE_THAT(master.sunY, WithinAbs(1494.5, 4.0));
    REQUIRE_THAT(master.sunThetaRad * 180.0 / kPi, WithinAbs(9.9, 0.5));
    // The bright rounded-rectangle ghost at about (1182, 1547).
    REQUIRE_FALSE(master.ghosts.empty());
    const bool foundPill = std::any_of(master.ghosts.begin(), master.ghosts.end(), [](const render::FlareGhost& g) {
        return std::hypot(g.cx - 1182.0, g.cy - 1547.0) < 12.0 && g.contrast > 0.15;
    });
    REQUIRE(foundPill);
}

#if defined(OSV_HAVE_CUDA)
TEST_CASE("the CUDA flare sampler matches the CPU downsample", "[flare][cuda]") {
    std::string reason;
    if (!render::cudaFlareAvailable(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    // A frame with real structure: a gradient in luma and chroma.
    const std::uint32_t w = 257, h = 131;
    SyntheticFrame f = makeConstantFrame(w, h, 64, 512, 512);
    auto* base = f.storage->data();
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            base[static_cast<std::size_t>(y) * w + x] = static_cast<std::uint16_t>(64 + (x * 3 + y * 5) % 876);
        }
    }
    const std::size_t lumaN = static_cast<std::size_t>(w) * h;
    const std::size_t chromaN = static_cast<std::size_t>(f.frame.chromaW) * f.frame.chromaH;
    for (std::size_t i = 0; i < chromaN; ++i) {
        base[lumaN + i] = static_cast<std::uint16_t>(300 + (i * 7) % 400);
        base[lumaN + chromaN + i] = static_cast<std::uint16_t>(700 - (i * 11) % 400);
    }
    ThreadPool pool;
    const OsvColorParams cp = linearColor();
    auto cpu = render::flareDownsample(f.frame, cp, 4, pool);
    REQUIRE(cpu.ok());
    OsvPlane plane{};
    REQUIRE(render::fillPlane(f.frame, plane));
    auto gpu = render::cudaFlareDownsample(plane, false, cp, 4);
    REQUIRE(gpu.ok());
    REQUIRE(gpu.value().w == cpu.value().w);
    REQUIRE(gpu.value().h == cpu.value().h);
    double worst = 0.0;
    for (std::size_t i = 0; i < cpu.value().rgb.size(); ++i) {
        const double a = cpu.value().rgb[i];
        const double b = gpu.value().rgb[i];
        worst = std::max(worst, std::fabs(a - b) / std::max(std::fabs(a), 1e-3));
    }
    INFO("worst relative difference " << worst);
    REQUIRE(worst < 1e-5);
}

TEST_CASE("the CUDA renderer removes flare exactly like the CPU reference", "[flare][sample][cuda]") {
    OSV_REQUIRE_SAMPLE();
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto sp = openSampleFrame(0);
    REQUIRE(sp.ok());
    ThreadPool pool;
    auto model = render::analyseFlare(sp.value().rig, sp.value().pair, linearColor(), render::FlareParams{}, pool);
    REQUIRE(model.ok());
    REQUIRE(model.value().any());
    // Aim at the sun (master lens, ~10 degrees left of its axis) so the
    // ghosts are in view, in a display transfer the parity tests use.
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    geom::VirtualCamera cam;
    cam.w = 960;
    cam.h = 540;
    cam.hfovDeg = 90;
    cam.yawDeg = 10.0;
    auto job = render::RenderParamsBuilder().rig(sp.value().rig).camera(cam).color(cp).build(sp.value().pair);
    REQUIRE(job.ok());
    render::applyFlare(model.value(), job.value().params);
    REQUIRE(job.value().params.flareEnabled == 1);
    render::CpuRenderer cpu(pool);
    auto gpuR = render::CudaRenderer::create(0);
    REQUIRE(gpuR.ok());
    auto a = cpu.render(job.value());
    auto b = gpuR.value()->render(job.value());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    const render::ImageDiffStats stats = render::compareImages16(a.value(), b.value());
    INFO("flare on: PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode << " codes");
    REQUIRE(stats.psnrDb >= 60.0);
    REQUIRE(stats.fractionWithin2 >= 0.9995);
}
#endif
