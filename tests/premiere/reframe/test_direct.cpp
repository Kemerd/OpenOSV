// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_direct.cpp - the direct fisheye -> view path (docs/DIRECT_GPU.md, WP-C):
// buildDirectParams(), its CPU twin, launchDirect() and the fused kernel
// osvReframeDirectKernel in the effect's fatbin.
//
// What is being proven, and how:
//
//   1. THE CONTRACT OF THE BUILDER.  The camera fields are buildView()'s, the
//      stitch fields are the importer's block untouched, Rout is R_stab *
//      Rout_view, and every unusable input is refused with a named reason.
//
//   2. FRAMING PARITY, deterministically.  The two-step path is run for real -
//      the effect's own buildParams() and osvReframeEquirectPixel() - over a
//      6000 x 3000 "direction map": an equirect whose every texel holds the
//      body-frame direction the importer's equirect render traces through
//      that texel (its own osvRayForPixel() and Rout, stabilisation
//      included).  What the effect samples out of that map IS the direction
//      the two-step path shows at each output pixel.  The direct path's
//      direction is osvRayForPixel() + Rout of the direct block.  The angle
//      between them, in units of the local output pixel pitch, must stay
//      below half a pixel for every view: rectilinear, wide eye-offset,
//      tiny planet, across the lens seam, rolled + source-rotated, and every
//      Output Resolution cover-fit case.  A negative control (the rotation
//      composed in the wrong order) shows the measurement is sensitive.
//
//   3. THE PICTURE, on the sample clip.  Both paths rendered on the CPU from
//      the same decoded frame with the importer's default stitch (horizon
//      lock, seam table, exposure match): normalised cross-correlation must
//      peak at zero offset (sub-pixel peak within 0.5 px) and be high, and at
//      a narrow field of view - where the 6000-wide equirect undersamples -
//      the direct render must carry at least as much high-frequency energy.
//
//   4. GPU vs CPU TWIN.  The kernel from the embedded fatbin, fed device P010
//      frames (NVDEC zero-copy where available), against renderDirectCpu()
//      on the very same samples: PSNR >= 60 dB after 16-bit quantisation, for
//      32f and 16f output, with the seam table and with the warp grid.  The
//      kernel is also timed at 2560 x 1440 and 3840 x 2160.

#include "ReframeTestSupport.h"

#include "DirectLaunch.h"
#include "DirectRender.h"
#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "osv/color/AutoDetect.h"
#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/Blend.h"
#include "osv/geom/ConventionProbe.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/Stabilization.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderJob.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ===========================================================================
//  The embedded fatbin - the same generated translation unit the .aex links
//  (see tests/premiere/reframe/CMakeLists.txt).  Global scope, ordinary C++
//  linkage, exactly as GpuFilter.cpp declares it.
// ===========================================================================
extern const unsigned char kOsvReframeFatbin[];
extern const std::size_t kOsvReframeFatbin_size;

using namespace osv;
using namespace osv::reframe;
using namespace osv::reframe::test;
using Catch::Approx;

namespace {

// ===========================================================================
//  Shared helpers
// ===========================================================================

/// One thread pool for every CPU render in this file; building a pool per
/// test would spin up and tear down 32 threads a dozen times for nothing.
ThreadPool& pool() {
    static ThreadPool p(0);
    return p;
}

/// Path of the sample clip: OSV_SAMPLE_FILE from the environment wins, the
/// configure-time default (only defined when the file existed) is next.
std::filesystem::path sampleClipPath() {
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        if (env[0] != '\0') {
            return std::filesystem::path(env);
        }
    }
#ifdef OSV_REFRAME_SAMPLE_CLIP
    return std::filesystem::path(OSV_REFRAME_SAMPLE_CLIP);
#else
    return {};
#endif
}

/// True when the sample clip exists and is non-empty.
bool sampleAvailable() {
    const std::filesystem::path p = sampleClipPath();
    std::error_code ec;
    return !p.empty() && std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) > 0;
}

/// SKIP the rest of a test case when the clip is not on this machine.
#define REQUIRE_SAMPLE_CLIP()                                                                                      \
    do {                                                                                                           \
        if (!sampleAvailable()) {                                                                                  \
            SKIP("sample clip not available (set OSV_SAMPLE_FILE): " << sampleClipPath().string());                \
        }                                                                                                          \
    } while (0)

/// A rig with the sample camera's real calibration numbers (the same record
/// tests/unit/test_render.cpp uses), for the tests that need a valid stitch
/// block but no footage.
Result<geom::LensRig> syntheticRig(int streamW) {
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
    fill(cal.master, 1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f, km, 0.7036960f, 0.7103991f, -0.0046943f,
         -0.0110939f);
    cal.sourceSlave = "test";
    cal.sourceMaster = "test";
    const double dfl = 829.3612 * (streamW / 3000.0);
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(streamW, streamW, 3840, 3840, dfl, 1043.445, streamW / 3776.0, nullptr));
    return geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength, dfl, geom::ExtrinsicConvention{});
}

/// A deliberately awkward stabilisation rotation (yaw 23, pitch -11, roll 7
/// degrees) so a composition in the wrong order cannot hide behind an
/// identity.  Built by the library's own camera rotation, so it is exactly a
/// proper rotation.
Mat3d synthStabilisation() {
    geom::VirtualCamera c;
    c.yawDeg = 23.0;
    c.pitchDeg = -11.0;
    c.rollDeg = 7.0;
    return c.rotation();
}

/// The importer's equirect parameter block for a rig, colour and
/// stabilisation - built exactly as ImporterInstance::renderFrame() builds it
/// (blend on, alpha = coverage, Standard layout), minus the per-frame
/// analyses the caller may add.
OsvRenderParams equirectBlock(const geom::LensRig& rig, const OsvColorParams& cp, const Mat3d& stab, int w, int h) {
    geom::BlendParams blend;
    blend.lensFovDeg = 195.18;
    blend.featherDeg = 4.0;
    blend.useOcclusionMask = true;
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = w;
    map.h = h;
    auto p = render::RenderParamsBuilder()
                 .rig(rig)
                 .color(cp)
                 .blend(blend, true)
                 .alphaCoverage(true)
                 .stabilization(stab)
                 .equirect(map)
                 .buildParams();
    REQUIRE(p.ok());
    return p.value();
}

/// A Settings block with every field stated (no reliance on defaults).
Settings makeSettings(Resolution res, double pan, double tilt, double roll, double fov, double distortion,
                      double srcPan = 0.0, double srcTilt = 0.0, double srcRoll = 0.0) {
    Settings s;
    s.resolution = res;
    s.preset = Preset::Custom;
    s.panDeg = pan;
    s.tiltDeg = tilt;
    s.rollDeg = roll;
    s.fovDeg = fov;
    s.distortion = distortion;
    s.sourcePanDeg = srcPan;
    s.sourceTiltDeg = srcTilt;
    s.sourceRollDeg = srcRoll;
    s.smoothKeyframes = false;
    return s;
}

/// The Asteroid preset exactly as the popup writes it into the controls.
Settings asteroidSettings(Resolution res) {
    const PresetEntry* e = presetEntry(Preset::Asteroid);
    Settings s = makeSettings(res, 0.0, e->tiltDeg, 0.0, e->fovDeg, e->distortion);
    s.preset = Preset::Asteroid;
    return s;
}

/// [WP-CAMERA] A Settings block on DJI's lens: Camera Model ticked, with the
/// DJI FOV and Correction Angle given; the Classic lens controls stay at
/// their defaults, which the DJI lens ignores.
Settings djiSettings(Resolution res, double pan, double tilt, double roll, double djiFov, double correction) {
    Settings s = makeSettings(res, pan, tilt, roll, OSV_REFRAME_FOV_DEFAULT, OSV_REFRAME_DISTORTION_DEFAULT);
    s.cameraModel = CameraModel::Dji;
    s.djiFovDeg = djiFov;
    s.correction = correction;
    return s;
}

/// One framing case: controls, output frame and sequence.
struct ViewCase {
    const char* name;
    Settings settings;
    int outW;
    int outH;
    SizePx sequence;
};

/// The views the acceptance criteria name, across the Output Resolution
/// cover-fit cases.
std::vector<ViewCase> framingCases() {
    std::vector<ViewCase> v;
    // Rectilinear 90 degrees, Match Sequence 2560 x 1440 at full resolution.
    v.push_back({"rectilinear 90, Match Sequence 2560x1440",
                 makeSettings(Resolution::MatchSequence, 30.0, 10.0, 0.0, 90.0, 0.0), 2560, 1440, SizePx{2560, 1440}});
    // The same camera at a half-resolution preview frame.
    v.push_back({"rectilinear 90, half-resolution preview 1280x720",
                 makeSettings(Resolution::MatchSequence, 30.0, 10.0, 0.0, 90.0, 0.0), 1280, 720, SizePx{2560, 1440}});
    // Wide eye-offset projection with Distortion on (the Ultra Wide look).
    v.push_back({"wide eye-offset 150 + distortion 40, 1920x1080",
                 makeSettings(Resolution::Fhd1920x1080, -60.0, -5.0, 0.0, 150.0, 40.0), 1920, 1080, SizePx{1920, 1080}});
    // Tiny planet: the Asteroid preset, 300 degrees straight down.
    v.push_back({"tiny planet (Asteroid 300), 3840x2160", asteroidSettings(Resolution::Uhd3840x2160), 3840, 2160,
                 SizePx{3840, 2160}});
    // Straddling the lens seam (the seam ring is at longitude +/-90).
    v.push_back({"across the lens seam (pan 90), 2560x1440",
                 makeSettings(Resolution::Qhd2560x1440, 90.0, 0.0, 0.0, 100.0, 0.0), 2560, 1440, SizePx{2560, 1440}});
    // Rolled camera inside a source-rotated sphere, portrait sequence.
    v.push_back({"rolled + source-rotated, portrait 1080x1920",
                 makeSettings(Resolution::MatchSequence, -40.0, 15.0, 25.0, 110.0, 20.0, 30.0, -12.0, 8.0), 1080, 1920,
                 SizePx{1080, 1920}});
    // Cover-fit that crops: a 16:9 resolution into a portrait frame.
    v.push_back({"3840x2160 requested into a portrait 1080x1920 frame",
                 makeSettings(Resolution::Uhd3840x2160, 10.0, -20.0, 5.0, 90.0, 0.0), 1080, 1920, SizePx{1080, 1920}});
    // Cover-fit that crops the other way: 16:9 into a 4:3 frame.
    v.push_back({"1920x1080 requested into a 4:3 1440x1080 frame",
                 makeSettings(Resolution::Fhd1920x1080, 170.0, 30.0, -15.0, 75.0, 0.0), 1440, 1080, SizePx{1440, 1080}});
    // [WP-CAMERA] DJI's lens (OSV_PROJ_DJI_SPHERE): the two DJI Studio field
    // observations, the second cover-fitted into a portrait frame.
    v.push_back({"DJI FOV 103.3 / correction 0.67 (DJI Studio screenshot 1), 1920x1080",
                 djiSettings(Resolution::MatchSequence, 144.8, -5.9, 0.0, 103.3, 0.67), 1920, 1080,
                 SizePx{1920, 1080}});
    v.push_back({"DJI FOV 150 / correction 0.34 (screenshot 2) requested 1920x1080 into a portrait 1080x1920 frame",
                 djiSettings(Resolution::Fhd1920x1080, 144.8, -5.9, 0.0, 150.0, 0.34), 1080, 1920,
                 SizePx{1080, 1920}});
    return v;
}

// ---------------------------------------------------------------------------
//  Direction arithmetic
// ---------------------------------------------------------------------------

/// Angle in radians between two (not necessarily unit) direction vectors,
/// atan2(|a x b|, a . b): accurate at small angles, where acos is not.
double angleBetween(const double* a, const double* b) noexcept {
    const double cx = a[1] * b[2] - a[2] * b[1];
    const double cy = a[2] * b[0] - a[0] * b[2];
    const double cz = a[0] * b[1] - a[1] * b[0];
    const double cross = std::sqrt(cx * cx + cy * cy + cz * cz);
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return std::atan2(cross, dot);
}

/// The body-frame direction the DIRECT path samples at output pixel (x, y):
/// the shader's own osvRayForPixel() rotated by the block's Rout, in float,
/// exactly as osvShadePixelW() computes it.  False when there is no ray.
bool directRay(const OsvRenderParams& p, int x, int y, double out[3]) noexcept {
    float dView[3];
    if (!osvRayForPixel(&p, static_cast<float>(x), static_cast<float>(y), dView)) {
        return false;
    }
    float dBody[3];
    osvMat3MulVec(p.Rout, dView, dBody);
    out[0] = dBody[0];
    out[1] = dBody[1];
    out[2] = dBody[2];
    return true;
}

/// Angular size of one output pixel at (x, y) under the direct camera: the
/// smaller of the angles to the horizontal and vertical neighbours (the
/// stricter of the two).  Zero when no neighbour has a ray.
double localPixelPitch(const OsvRenderParams& p, int x, int y, const double* centre) noexcept {
    double best = std::numeric_limits<double>::infinity();
    const int nx = (x + 1 < p.outW) ? x + 1 : x - 1;
    const int ny = (y + 1 < p.outH) ? y + 1 : y - 1;
    double n[3];
    if (nx >= 0 && directRay(p, nx, y, n)) {
        best = std::min(best, angleBetween(centre, n));
    }
    if (ny >= 0 && directRay(p, x, ny, n)) {
        best = std::min(best, angleBetween(centre, n));
    }
    return std::isfinite(best) ? best : 0.0;
}

// ---------------------------------------------------------------------------
//  The direction map (see the file comment, point 2)
// ---------------------------------------------------------------------------

/// A W x H Standard-layout equirect, BGRA 32f, whose texels hold the body
/// direction the importer's equirect render traces through each texel.
struct DirectionMap {
    int w = 0;
    int h = 0;
    std::vector<float> texels;  ///< Packed BGRA: B = z, G = y, R = x, A = 1.

    /// The map as the effect's equirect path sees its source frame.
    [[nodiscard]] ConstFrameView view() const noexcept {
        ConstFrameView v;
        v.base = texels.data();
        v.rowBytes = static_cast<std::int32_t>(w * 16);
        v.width = w;
        v.height = h;
        v.layout = PixelLayout::Bgra32f;
        v.topDown = true;
        return v;
    }
};

/// Build the map from the importer's equirect block: per texel, the
/// importer's own osvRayForPixel() (equirect mode, Standard layout) rotated
/// by the block's Rout - the stabilisation - which is precisely the body ray
/// osvShadePixelW() projects into the lenses for that panorama pixel.
DirectionMap makeDirectionMap(const OsvRenderParams& eq) {
    DirectionMap m;
    m.w = eq.outW;
    m.h = eq.outH;
    m.texels.assign(static_cast<std::size_t>(m.w) * static_cast<std::size_t>(m.h) * 4u, 0.0f);
    float* const base = m.texels.data();
    const int w = m.w;
    const Status st = pool().parallelRows(static_cast<std::size_t>(m.h), 8, [&eq, base, w](std::size_t row) {
        const int y = static_cast<int>(row);
        float* out = base + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
        for (int x = 0; x < w; ++x) {
            float d[3];
            float b[3] = {0.0f, 0.0f, 0.0f};
            if (osvRayForPixel(&eq, static_cast<float>(x), static_cast<float>(y), d)) {
                osvMat3MulVec(eq.Rout, d, b);
            }
            out[x * 4 + 0] = b[2];  // B <- z
            out[x * 4 + 1] = b[1];  // G <- y
            out[x * 4 + 2] = b[0];  // R <- x
            out[x * 4 + 3] = 1.0f;
        }
    });
    REQUIRE(st.ok());
    return m;
}

/// The result of one framing comparison.
struct FramingResult {
    double maxErrPx = 0.0;        ///< Worst angular difference, in local output pixels.
    double maxErrArcsec = 0.0;    ///< Worst angular difference, in arcseconds.
    double meanErrPx = 0.0;       ///< Mean angular difference, in local output pixels.
    std::size_t samples = 0;      ///< Output pixels compared.
    std::size_t coverageMismatch = 0;  ///< Pixels with a ray in one path only.
};

/// Compare the direction the two-step path shows at a grid of output pixels
/// with the direction the direct block `direct` samples there.
///
/// The two-step side is the effect's real code: buildParams() on the
/// direction map and osvReframeEquirectPixel() through renderPixel().  The
/// sampled texel is a bilinear blend of neighbouring directions, so it is
/// re-normalised before the angle is taken.
FramingResult measureFraming(const ViewCase& c, const DirectionMap& map, const OsvRenderParams& direct) {
    const ConstFrameView src = map.view();
    const KernelSetup twoStep = buildParams(c.settings, src, c.outW, c.outH, c.sequence);
    REQUIRE(twoStep.valid);

    FramingResult r;
    // Roughly 240 columns of samples at every size, plus the last row and
    // column, so the frame edges (where cover-fit and projection errors
    // would be largest) are always included.
    const int step = std::max(1, c.outW / 240);
    std::vector<int> xs;
    std::vector<int> ys;
    for (int x = 0; x < c.outW; x += step) {
        xs.push_back(x);
    }
    xs.push_back(c.outW - 1);
    for (int y = 0; y < c.outH; y += step) {
        ys.push_back(y);
    }
    ys.push_back(c.outH - 1);

    double sumPx = 0.0;
    for (const int y : ys) {
        for (const int x : xs) {
            // Two-step: what the effect samples out of the map at this pixel.
            float rgba[4];
            REQUIRE(renderPixel(twoStep, src, x, y, rgba));
            const bool twoHasRay = rgba[3] > 0.5f;
            // Direct: the direction the fused kernel traces.
            double d[3];
            const bool directHasRay = directRay(direct, x, y, d);
            if (twoHasRay != directHasRay) {
                ++r.coverageMismatch;
                continue;
            }
            if (!directHasRay) {
                continue;  // neither path paints this pixel: they agree
            }
            const double two[3] = {rgba[0], rgba[1], rgba[2]};
            const double err = angleBetween(d, two);
            const double pitch = localPixelPitch(direct, x, y, d);
            if (!(pitch > 0.0)) {
                continue;  // an isolated ray with no neighbour: no pixel scale to measure in
            }
            const double px = err / pitch;
            r.maxErrPx = std::max(r.maxErrPx, px);
            r.maxErrArcsec = std::max(r.maxErrArcsec, err * (180.0 / osv::kPi) * 3600.0);
            sumPx += px;
            ++r.samples;
        }
    }
    r.meanErrPx = r.samples ? sumPx / static_cast<double>(r.samples) : 0.0;
    return r;
}

// ---------------------------------------------------------------------------
//  Frame buffers and image metrics
// ---------------------------------------------------------------------------

/// A packed top-down BGRA frame in host memory.
struct HostFrame {
    int w = 0;
    int h = 0;
    PixelLayout layout = PixelLayout::Bgra32f;
    std::vector<std::uint8_t> bytes;

    HostFrame(int width, int height, PixelLayout l) : w(width), h(height), layout(l) {
        bytes.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * bytesPerPixel(l), 0u);
    }
    [[nodiscard]] std::int32_t rowBytes() const noexcept {
        return static_cast<std::int32_t>(static_cast<std::size_t>(w) * bytesPerPixel(layout));
    }
    [[nodiscard]] FrameView view() noexcept {
        FrameView v;
        v.base = bytes.data();
        v.rowBytes = rowBytes();
        v.width = w;
        v.height = h;
        v.layout = layout;
        v.topDown = true;
        return v;
    }
    /// One pixel as straight RGBA floats.
    void rgba(int x, int y, float out[4]) const noexcept {
        if (layout == PixelLayout::Bgra16f) {
            readPixelBgra16f(bytes.data(), rowBytes(), x, y, out);
        } else {
            readPixelBgra32f(bytes.data(), rowBytes(), x, y, out);
        }
    }
    /// The frame as an RGBA float image, for render::compareImages16().
    [[nodiscard]] render::ImageRGBAf toImage() const {
        auto img = render::ImageRGBAf::create(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        REQUIRE(img.ok());
        render::ImageRGBAf out = std::move(img).value();
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                rgba(x, y, &out.data[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u]);
            }
        }
        return out;
    }
};

/// Luma of a rendered frame (BT.2020 weights on the encoded values) plus a
/// coverage mask (alpha fully opaque).
struct Luma {
    int w = 0;
    int h = 0;
    std::vector<double> y;
    std::vector<std::uint8_t> valid;
};

Luma lumaOf(const HostFrame& f) {
    Luma l;
    l.w = f.w;
    l.h = f.h;
    l.y.assign(static_cast<std::size_t>(f.w) * static_cast<std::size_t>(f.h), 0.0);
    l.valid.assign(l.y.size(), 0u);
    for (int y = 0; y < f.h; ++y) {
        for (int x = 0; x < f.w; ++x) {
            float c[4];
            f.rgba(x, y, c);
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(f.w) + x;
            l.y[i] = 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2];
            l.valid[i] = (c[3] > 0.999f && std::isfinite(l.y[i])) ? 1u : 0u;
        }
    }
    return l;
}

/// Normalised cross-correlation of `a` with `b` displaced by (dx, dy):
/// sum over pixels valid in both, `margin` pixels in from every edge.
double nccAt(const Luma& a, const Luma& b, int dx, int dy, int margin) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    std::size_t n = 0;
    for (int y = margin; y < a.h - margin; ++y) {
        for (int x = margin; x < a.w - margin; ++x) {
            const std::size_t ia = static_cast<std::size_t>(y) * a.w + x;
            const std::size_t ib = static_cast<std::size_t>(y + dy) * b.w + (x + dx);
            if (!a.valid[ia] || !b.valid[ib]) {
                continue;
            }
            const double va = a.y[ia];
            const double vb = b.y[ib];
            sa += va;
            sb += vb;
            saa += va * va;
            sbb += vb * vb;
            sab += va * vb;
            ++n;
        }
    }
    if (n < 16) {
        return 0.0;
    }
    const double inv = 1.0 / static_cast<double>(n);
    const double cov = sab * inv - (sa * inv) * (sb * inv);
    const double varA = saa * inv - (sa * inv) * (sa * inv);
    const double varB = sbb * inv - (sb * inv) * (sb * inv);
    if (!(varA > 0.0) || !(varB > 0.0)) {
        return 0.0;
    }
    return cov / std::sqrt(varA * varB);
}

/// Alignment of two renders of the same view: NCC over the 3 x 3 integer
/// displacements, and the sub-pixel peak from a parabola through the centre
/// row and column.
struct Alignment {
    double ncc[3][3] = {};  ///< [dy + 1][dx + 1].
    double peakX = 0.0;     ///< Sub-pixel peak offset in x (pixels).
    double peakY = 0.0;     ///< Sub-pixel peak offset in y (pixels).
    bool peakAtZero = false;  ///< The integer maximum is the zero displacement.
};

Alignment alignmentOf(const Luma& a, const Luma& b) {
    Alignment al;
    constexpr int kMargin = 4;
    double best = -2.0;
    int bestX = 0;
    int bestY = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const double v = nccAt(a, b, dx, dy, kMargin);
            al.ncc[dy + 1][dx + 1] = v;
            if (v > best) {
                best = v;
                bestX = dx;
                bestY = dy;
            }
        }
    }
    al.peakAtZero = (bestX == 0 && bestY == 0);
    // Parabola vertex through (-1, l), (0, c), (1, r): (l - r) / (2 (l - 2c + r)).
    auto vertex = [](double l, double c, double r) {
        const double den = l - 2.0 * c + r;
        return (std::fabs(den) > 1e-12) ? 0.5 * (l - r) / den : 0.0;
    };
    al.peakX = vertex(al.ncc[1][0], al.ncc[1][1], al.ncc[1][2]);
    al.peakY = vertex(al.ncc[0][1], al.ncc[1][1], al.ncc[2][1]);
    return al;
}

/// High-frequency energy of a render over the pixels where BOTH renders are
/// valid (so neither is scored on content the other lacks).
struct Sharpness {
    double meanGradient = 0.0;       ///< Mean central-difference gradient magnitude.
    double laplacianVariance = 0.0;  ///< Variance of the 4-neighbour Laplacian.
};

Sharpness sharpnessOf(const Luma& img, const Luma& other) {
    Sharpness s;
    double sumG = 0.0;
    double sumL = 0.0;
    double sumLL = 0.0;
    std::size_t n = 0;
    auto ok = [&](int x, int y) {
        const std::size_t i = static_cast<std::size_t>(y) * img.w + x;
        return img.valid[i] && other.valid[i];
    };
    for (int y = 2; y < img.h - 2; ++y) {
        for (int x = 2; x < img.w - 2; ++x) {
            if (!ok(x, y) || !ok(x - 1, y) || !ok(x + 1, y) || !ok(x, y - 1) || !ok(x, y + 1)) {
                continue;
            }
            const std::size_t i = static_cast<std::size_t>(y) * img.w + x;
            const double c = img.y[i];
            const double l = img.y[i - 1];
            const double r = img.y[i + 1];
            const double u = img.y[i - img.w];
            const double d = img.y[i + img.w];
            const double gx = 0.5 * (r - l);
            const double gy = 0.5 * (d - u);
            sumG += std::sqrt(gx * gx + gy * gy);
            const double lap = 4.0 * c - l - r - u - d;
            sumL += lap;
            sumLL += lap * lap;
            ++n;
        }
    }
    if (n > 0) {
        const double inv = 1.0 / static_cast<double>(n);
        s.meanGradient = sumG * inv;
        s.laplacianVariance = sumLL * inv - (sumL * inv) * (sumL * inv);
    }
    return s;
}

// ---------------------------------------------------------------------------
//  The sample clip, set up exactly as the importer sets it up
// ---------------------------------------------------------------------------

/// Everything ImporterInstance derives once per clip: rig, blend, colour and
/// the attitude track the horizon lock reads.
struct SampleClip {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
    geom::BlendParams blend;
    OsvColorParams color{};
    std::optional<geom::AttitudeTrack> attitude;
    Quatd reference;
    geom::StabilizationParams stab;
};

/// Open the clip with the importer's DEFAULT prefs: native calibration slot,
/// 195.18 degree lens FOV, 4 degree feather with the occlusion polygon, the
/// Osmo 360 D-Log M fit to PQ with the clip's own input encoding, and horizon
/// lock (ImporterInstance::rebuildRig / rebuildColor / rebuildStabilization).
std::unique_ptr<SampleClip> openSampleClip() {
    auto clip = std::make_unique<SampleClip>();
    auto file = OsvFile::open(sampleClipPath());
    REQUIRE(file.ok());
    clip->file = std::make_unique<OsvFile>(std::move(file).value());
    auto track = meta::MetadataTrack::load(*clip->file);
    REQUIRE(track.ok());
    clip->track = std::move(track).value();
    auto format = meta::FormatDetector::detect(*clip->file, &clip->track);
    REQUIRE(format.ok());
    clip->format = std::move(format).value();

    // ---- rig (rebuildRig) ---------------------------------------------------
    auto cal = meta::CalibrationSelector::select(clip->track.stream());
    REQUIRE(cal.ok());
    const meta::CalibrationSet& c = cal.value();
    const double calFxMean = 0.5 * (c.slave.fx + c.master.fx);
    auto scaling = geom::StreamScaling::derive(
        static_cast<int>(clip->format.lensW()), static_cast<int>(clip->format.lensH()),
        static_cast<int>(clip->format.sensorW), static_cast<int>(clip->format.sensorH),
        clip->format.digitalFocalLength, calFxMean, std::nullopt, nullptr);
    REQUIRE(scaling.ok());
    auto rig = geom::LensRig::build(c, scaling.value(), geom::FocalSource::DigitalFocalLength,
                                    clip->format.digitalFocalLength, geom::ExtrinsicConvention{}, 195.18);
    REQUIRE(rig.ok());
    clip->rig = std::move(rig).value();
    clip->blend.lensFovDeg = 195.18;
    clip->blend.featherDeg = 4.0;
    clip->blend.useOcclusionMask = true;

    // ---- colour (rebuildColor, default prefs) -------------------------------
    clip->color = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f,
                                         color::inputEncodingForColorMode(clip->format.colorMode), true,
                                         clip->format.bitDepth ? clip->format.bitDepth : 10u);

    // ---- horizon lock (rebuildStabilization) --------------------------------
    clip->stab.mode = geom::StabilizationMode::HorizonLock;
    geom::AttitudeTrack::Options attOpt;
    const geom::ConventionScore best = geom::ConventionProbe::best(clip->track);
    if (best.framesUsed > 0 && best.meanGravityAngleDeg < 15.0) {
        attOpt.conv = best.conv;
    }
    auto att = geom::AttitudeTrack::build(clip->track, attOpt);
    if (att.ok() && att.value().sampleCount() > 0) {
        clip->attitude = std::move(att).value();
        clip->reference = clip->attitude->worldFromBody(clip->attitude->beginUs());
    }
    return clip;
}

/// The frame's stabilisation rotation, as ImporterInstance::stabilizationFor.
Mat3d stabilisationFor(const SampleClip& clip, std::uint32_t frameIndex) {
    if (!clip.attitude) {
        return Mat3d::identity();
    }
    double tUs = clip.attitude->beginUs();
    auto fm = clip.track.frame(frameIndex);
    if (fm.ok()) {
        tUs = static_cast<double>(fm.value().timestampUs);
    }
    const Quatd wfb = clip.attitude->worldFromBody(tUs);
    return geom::stabilizationBodyFromWorld(wfb, clip.stab, clip.reference, clip.attitude->worldUp(), std::nullopt);
}

/// One decoded frame with the importer's default per-frame analyses.
struct SampleFrame {
    video::FramePair pair;          ///< Software-decoded, host planes.
    std::vector<float> seamTable;   ///< searchSeam() result (the importer default).
    std::array<Vec3d, 2> gains{Vec3d{1, 1, 1}, Vec3d{1, 1, 1}};  ///< estimateGain() result.
    Mat3d stab = Mat3d::identity(); ///< stabilisationFor(frame).
};

SampleFrame decodeSampleFrame(const SampleClip& clip, std::uint32_t frameIndex) {
    SampleFrame f;
    auto reader = video::DualStreamReader::open(sampleClipPath(), clip.format);
    REQUIRE(reader.ok());
    auto pair = reader.value().read(frameIndex);
    REQUIRE(pair.ok());
    f.pair = std::move(pair).value();
    REQUIRE(f.pair.valid());
    // Seam table and exposure match: both on in the importer's default prefs.
    auto seam = render::searchSeam(clip.rig, f.pair, clip.blend, render::SeamSearchParams{}, pool());
    REQUIRE(seam.ok());
    f.seamTable = seam.value().shiftDeg;
    auto gain = render::estimateGain(clip.rig, f.pair, clip.blend, render::BandParams{}, pool());
    if (gain.ok()) {
        f.gains = {gain.value().gain[0], gain.value().gain[1]};
    }
    f.stab = stabilisationFor(clip, frameIndex);
    return f;
}

/// The builder the importer's renderFrame() configures for a frame, before
/// the equirect map is chosen.
render::RenderParamsBuilder importerBuilder(const SampleClip& clip, const SampleFrame& f) {
    render::RenderParamsBuilder b;
    b.rig(clip.rig).color(clip.color).blend(clip.blend, true).alphaCoverage(true);
    b.seam(f.seamTable);
    b.gain(f.gains[0], f.gains[1]);
    b.stabilization(f.stab);
    return b;
}

}  // namespace

// ===========================================================================
//  1. The builder's contract
// ===========================================================================

TEST_CASE("buildDirectParams takes the camera from buildView and the stitch from the importer block",
          "[reframe][direct]") {
    auto rig = syntheticRig(3000);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    const OsvRenderParams eq = equirectBlock(rig.value(), cp, synthStabilisation(), 6000, 3000);
    const std::vector<float> seam(360, 0.25f);
    const std::vector<float> warp(64 * 8 * 2, 0.001f);

    StitchState stitch;
    stitch.equirect = eq;
    stitch.equirect.seamShiftEnabled = 1;
    stitch.equirect.seamColumns = static_cast<int>(seam.size());
    stitch.equirect.warpEnabled = 1;
    stitch.equirect.warpW = 64;
    stitch.equirect.warpH = 8;
    stitch.equirect.warpLatMinRad = -0.1f;
    stitch.equirect.warpLatMaxRad = 0.1f;
    stitch.equirect.warpSinLatLo = std::sin(-0.1f);
    stitch.equirect.warpSinLatHi = std::sin(0.1f);
    stitch.seamTable = seam.data();
    stitch.warpGrid = warp.data();

    const Settings s = makeSettings(Resolution::Uhd3840x2160, 35.0, -12.0, 9.0, 130.0, 25.0, 10.0, 4.0, -3.0);
    const DirectSetup d = buildDirectParams(s, stitch, 1080, 1920, SizePx{1080, 1920});
    INFO("reject: " << directRejectName(d.reject));
    REQUIRE(d.valid);
    CHECK(d.reject == DirectReject::None);

    // ---- the camera is buildView()'s, field for field ----------------------
    const ViewSetup v = buildView(s, 1080, 1920, SizePx{1080, 1920});
    REQUIRE(v.valid);
    CHECK(d.params.mode == OSV_MODE_REFRAME);
    CHECK(d.params.outW == 1080);
    CHECK(d.params.outH == 1920);
    CHECK(d.params.projection == v.params.projection);
    CHECK(d.params.focalPx == v.params.focalPx);
    CHECK(d.params.eyeOffset == v.params.eyeOffset);
    CHECK(d.params.tanHalfH == v.params.tanHalfH);
    CHECK(d.params.tanHalfV == v.params.tanHalfV);

    // ---- Rout = R_stab * Rout_view -------------------------------------------
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double expected = 0.0;
            for (int k = 0; k < 3; ++k) {
                expected += static_cast<double>(eq.Rout[r * 3 + k]) * static_cast<double>(v.params.Rout[k * 3 + c]);
            }
            CHECK(d.params.Rout[r * 3 + c] == Approx(expected).margin(1e-6));
        }
    }

    // ---- the stitch is the importer's block, untouched -----------------------
    CHECK(std::memcmp(&d.params.lens[0], &stitch.equirect.lens[0], sizeof(OsvLens)) == 0);
    CHECK(std::memcmp(&d.params.lens[1], &stitch.equirect.lens[1], sizeof(OsvLens)) == 0);
    CHECK(std::memcmp(&d.params.color, &stitch.equirect.color, sizeof(OsvColorParams)) == 0);
    CHECK(d.params.blendEnabled == stitch.equirect.blendEnabled);
    CHECK(d.params.outputAlphaCoverage == stitch.equirect.outputAlphaCoverage);
    CHECK(d.params.seamShiftEnabled == 1);
    CHECK(d.params.seamColumns == 360);
    CHECK(d.params.warpEnabled == 1);
    CHECK(d.params.warpW == 64);
    CHECK(d.params.warpH == 8);
    CHECK(d.params.warpLatMinRad == stitch.equirect.warpLatMinRad);
    CHECK(d.params.warpSinLatHi == stitch.equirect.warpSinLatHi);
    CHECK(d.seamTable == seam.data());
    CHECK(d.warpGrid == warp.data());

    // ---- and it fits the kernel's parameter space ----------------------------
    CHECK(sizeof(OsvRenderParams) + sizeof(OsvDirectPlanes) + 3 * sizeof(void*) + 2 * sizeof(int) <= 4096);
}

TEST_CASE("buildDirectParams forwards seam and warp pointers only with their feature", "[reframe][direct]") {
    auto rig = syntheticRig(3000);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    StitchState stitch;
    stitch.equirect = equirectBlock(rig.value(), cp, Mat3d::identity(), 6000, 3000);
    const float dummy[4] = {0, 0, 0, 0};
    stitch.seamTable = dummy;  // present, but the block says the seam is off
    stitch.warpGrid = dummy;
    const DirectSetup d = buildDirectParams(makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0), stitch, 640, 360,
                                            SizePx{});
    REQUIRE(d.valid);
    CHECK(d.seamTable == nullptr);
    CHECK(d.warpGrid == nullptr);
}

TEST_CASE("buildDirectParams refuses every stitch state a kernel could misbehave on", "[reframe][direct]") {
    auto rig = syntheticRig(3000);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    StitchState good;
    good.equirect = equirectBlock(rig.value(), cp, synthStabilisation(), 6000, 3000);
    const Settings s = makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0);
    REQUIRE(buildDirectParams(s, good, 640, 360, SizePx{}).valid);

    auto expectReject = [&](const StitchState& st, DirectReject why) {
        const DirectSetup d = buildDirectParams(s, st, 640, 360, SizePx{});
        INFO("expected " << directRejectName(why) << ", got " << directRejectName(d.reject));
        CHECK_FALSE(d.valid);
        CHECK(d.reject == why);
    };

    SECTION("a reframe block instead of an equirect block") {
        StitchState st = good;
        st.equirect.mode = OSV_MODE_REFRAME;
        expectReject(st, DirectReject::StitchLayout);
    }
    SECTION("a polar-axis equirect block") {
        StitchState st = good;
        st.equirect.layout = OSV_LAYOUT_POLAR_AXIS;
        expectReject(st, DirectReject::StitchLayout);
    }
    SECTION("a NaN in the stabilisation") {
        StitchState st = good;
        st.equirect.Rout[4] = std::nanf("");
        expectReject(st, DirectReject::Rotation);
    }
    SECTION("a scaled stabilisation matrix") {
        StitchState st = good;
        for (float& v : st.equirect.Rout) {
            v *= 2.0f;
        }
        expectReject(st, DirectReject::Rotation);
    }
    SECTION("a mirroring stabilisation matrix (determinant -1)") {
        StitchState st = good;
        for (int c = 0; c < 3; ++c) {
            st.equirect.Rout[c] = -st.equirect.Rout[c];
        }
        expectReject(st, DirectReject::Rotation);
    }
    SECTION("a NaN focal length") {
        StitchState st = good;
        st.equirect.lens[1].fx = std::nanf("");
        expectReject(st, DirectReject::Lens);
    }
    SECTION("an occlusion count past the polygon arrays") {
        StitchState st = good;
        st.equirect.lens[0].occlN = OSV_MAX_OCCLUSION_POINTS + 1;
        expectReject(st, DirectReject::Lens);
    }
    SECTION("no enabled lens at all") {
        StitchState st = good;
        st.equirect.lens[0].enabled = 0;
        st.equirect.lens[1].enabled = 0;
        expectReject(st, DirectReject::Lens);
    }
    SECTION("a NaN in the colour block") {
        StitchState st = good;
        st.equirect.color.exposureGain = std::nanf("");
        expectReject(st, DirectReject::Color);
    }
    SECTION("seam shift on with no table") {
        StitchState st = good;
        st.equirect.seamShiftEnabled = 1;
        st.equirect.seamColumns = 360;
        expectReject(st, DirectReject::SeamTable);
    }
    SECTION("seam shift on with no columns") {
        StitchState st = good;
        const float table[1] = {0.0f};
        st.equirect.seamShiftEnabled = 1;
        st.equirect.seamColumns = 0;
        st.seamTable = table;
        expectReject(st, DirectReject::SeamTable);
    }
    SECTION("warp on with no grid") {
        StitchState st = good;
        st.equirect.warpEnabled = 1;
        st.equirect.warpW = 16;
        st.equirect.warpH = 4;
        st.equirect.warpLatMinRad = -0.1f;
        st.equirect.warpLatMaxRad = 0.1f;
        expectReject(st, DirectReject::WarpGrid);
    }
    SECTION("warp on with a degenerate latitude span") {
        StitchState st = good;
        const float grid[16 * 4 * 2] = {};
        st.equirect.warpEnabled = 1;
        st.equirect.warpW = 16;
        st.equirect.warpH = 4;
        st.equirect.warpLatMinRad = 0.1f;
        st.equirect.warpLatMaxRad = 0.1f;
        st.warpGrid = grid;
        expectReject(st, DirectReject::WarpGrid);
    }
    SECTION("an unusable output size is the camera's refusal, named") {
        const DirectSetup d = buildDirectParams(s, good, 0, 360, SizePx{});
        CHECK_FALSE(d.valid);
        CHECK(d.reject == DirectReject::View);
        CHECK(d.viewReject == SetupReject::OutputSize);
        const DirectSetup huge = buildDirectParams(s, good, 1 << 20, 360, SizePx{});
        CHECK_FALSE(huge.valid);
        CHECK(huge.viewReject == SetupReject::OutputSize);
    }
}

TEST_CASE("buildDirectParams treats hostile CONTROL values exactly like the equirect path", "[reframe][direct]") {
    // NaN angles and FOV are not refusals: buildView() replaces them with the
    // documented defaults, and the direct path must do what the equirect
    // path does for the same frame - render it.
    auto rig = syntheticRig(3000);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    StitchState stitch;
    stitch.equirect = equirectBlock(rig.value(), cp, synthStabilisation(), 6000, 3000);
    Settings s = makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0);
    s.panDeg = std::nan("");
    s.tiltDeg = std::numeric_limits<double>::infinity();
    s.fovDeg = std::nan("");
    s.distortion = std::nan("");
    const DirectSetup d = buildDirectParams(s, stitch, 320, 180, SizePx{});
    REQUIRE(d.valid);
    CHECK(std::isfinite(d.params.focalPx));
    for (const float v : d.params.Rout) {
        CHECK(std::isfinite(v));
    }
    const ViewSetup v = buildView(s, 320, 180, SizePx{});
    REQUIRE(v.valid);
    CHECK(d.params.focalPx == v.params.focalPx);
}

TEST_CASE("planesMatch vets every descriptor the shader will index", "[reframe][direct]") {
    auto rig = syntheticRig(600);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    StitchState stitch;
    stitch.equirect = equirectBlock(rig.value(), cp, Mat3d::identity(), 1200, 600);
    const DirectSetup setup =
        buildDirectParams(makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0), stitch, 64, 36, SizePx{});
    REQUIRE(setup.valid);

    // A P010-shaped host frame: luma plus interleaved CbCr, pitch 640.
    std::vector<osv_u16> y(640u * 600u, 0u);
    std::vector<osv_u16> uv(640u * 300u, 0u);
    OsvPlane good{};
    good.y = y.data();
    good.u = uv.data();
    good.v = uv.data() + 1;
    good.w = 600;
    good.h = 600;
    good.cw = 300;
    good.ch = 300;
    good.strideY = 640;
    good.strideC = 640;
    good.bitShift = 6;
    good.chromaInterleaved = 1;
    OsvPlane planes[2] = {good, good};
    REQUIRE(planesMatch(setup, planes));

    SECTION("a null plane array") { CHECK_FALSE(planesMatch(setup, nullptr)); }
    SECTION("a null luma pointer") {
        planes[1].y = nullptr;
        CHECK_FALSE(planesMatch(setup, planes));
    }
    SECTION("a frame of the wrong size") {
        planes[0].w = 599;
        CHECK_FALSE(planesMatch(setup, planes));
    }
    SECTION("4:4:4 chroma") {
        planes[0].cw = 600;
        planes[0].ch = 600;
        planes[0].strideC = 1280;
        CHECK_FALSE(planesMatch(setup, planes));
    }
    SECTION("a chroma stride that cannot hold an interleaved row") {
        planes[1].strideC = 599;
        CHECK_FALSE(planesMatch(setup, planes));
    }
    SECTION("interleaved chroma whose Cr is not right after Cb") {
        planes[0].v = uv.data() + 2;
        CHECK_FALSE(planesMatch(setup, planes));
    }
    SECTION("a bit shift no 16-bit word can carry") {
        planes[0].bitShift = 16;
        CHECK_FALSE(planesMatch(setup, planes));
    }
    SECTION("a disabled lens is not inspected") {
        StitchState oneLens = stitch;
        oneLens.equirect.lens[0].enabled = 0;
        const DirectSetup s1 =
            buildDirectParams(makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0), oneLens, 64, 36, SizePx{});
        REQUIRE(s1.valid);
        OsvPlane partial[2] = {OsvPlane{}, good};
        CHECK(planesMatch(s1, partial));
    }
}

TEST_CASE("renderDirectCpu refuses a mismatched destination without writing anything", "[reframe][direct]") {
    auto rig = syntheticRig(600);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    StitchState stitch;
    stitch.equirect = equirectBlock(rig.value(), cp, Mat3d::identity(), 1200, 600);
    const DirectSetup setup =
        buildDirectParams(makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0), stitch, 64, 36, SizePx{});
    REQUIRE(setup.valid);
    std::vector<osv_u16> y(600u * 600u, 512u << 6);
    std::vector<osv_u16> uv(600u * 300u, 512u << 6);
    OsvPlane p{};
    p.y = y.data();
    p.u = uv.data();
    p.v = uv.data() + 1;
    p.w = p.h = 600;
    p.cw = p.ch = 300;
    p.strideY = p.strideC = 600;
    p.bitShift = 6;
    p.chromaInterleaved = 1;
    const OsvPlane planes[2] = {p, p};

    HostFrame wrong(65, 36, PixelLayout::Bgra32f);
    std::fill(wrong.bytes.begin(), wrong.bytes.end(), std::uint8_t{0xAB});
    CHECK_FALSE(renderDirectCpu(setup, planes, wrong.view(), &pool()));
    CHECK(std::all_of(wrong.bytes.begin(), wrong.bytes.end(), [](std::uint8_t b) { return b == 0xAB; }));

    // The right size renders, and every pixel is written (mid-grey planes
    // give an opaque, finite picture wherever a lens sees).
    HostFrame right(64, 36, PixelLayout::Bgra32f);
    std::fill(right.bytes.begin(), right.bytes.end(), std::uint8_t{0xAB});
    REQUIRE(renderDirectCpu(setup, planes, right.view(), &pool()));
    float c[4];
    right.rgba(32, 18, c);
    CHECK(std::isfinite(c[0]));
    CHECK(c[3] == Approx(1.0f));
}

TEST_CASE("launchDirect refuses bad inputs before touching the driver", "[reframe][direct]") {
    // Every refusal below fires before cuPointerGetAttribute / cuLaunchKernel,
    // so these run without a GPU.  The non-null kernel handle is a sentinel
    // that is never dereferenced because an earlier check always fails.
    auto rig = syntheticRig(600);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    StitchState stitch;
    stitch.equirect = equirectBlock(rig.value(), cp, Mat3d::identity(), 1200, 600);
    const DirectSetup setup =
        buildDirectParams(makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0), stitch, 64, 36, SizePx{});
    REQUIRE(setup.valid);
    std::vector<osv_u16> y(600u * 600u, 0u);
    std::vector<osv_u16> uv(600u * 300u, 0u);
    OsvPlane p{};
    p.y = y.data();
    p.u = uv.data();
    p.v = uv.data() + 1;
    p.w = p.h = 600;
    p.cw = p.ch = 300;
    p.strideY = p.strideC = 600;
    p.bitShift = 6;
    p.chromaInterleaved = 1;
    const OsvPlane planes[2] = {p, p};
    std::vector<float> frame(64u * 36u * 4u);
    DirectOutput out{frame.data(), 64 * 16, 64, 36, false};
    const CUfunction sentinel = reinterpret_cast<CUfunction>(static_cast<std::uintptr_t>(0x10));

    CHECK(launchDirect(nullptr, nullptr, setup, planes, out).reject == DirectLaunchReject::Kernel);
    CHECK(launchDirect(sentinel, nullptr, DirectSetup{}, planes, out).reject == DirectLaunchReject::Setup);
    CHECK(launchDirect(sentinel, nullptr, setup, nullptr, out).reject == DirectLaunchReject::Planes);
    DirectOutput wrongSize = out;
    wrongSize.height = 35;
    CHECK(launchDirect(sentinel, nullptr, setup, planes, wrongSize).reject == DirectLaunchReject::Output);
    DirectOutput shortPitch = out;
    shortPitch.rowBytes = 64 * 16 - 4;
    CHECK(launchDirect(sentinel, nullptr, setup, planes, shortPitch).reject == DirectLaunchReject::Output);
    DirectOutput misaligned = out;
    misaligned.rowBytes = 64 * 16 + 2;
    CHECK(launchDirect(sentinel, nullptr, setup, planes, misaligned).reject == DirectLaunchReject::Alignment);
    const DirectLaunchResult none = launchDirect(nullptr, nullptr, setup, planes, out);
    CHECK_FALSE(none.ok());
    CHECK(none.result == CUDA_ERROR_INVALID_VALUE);
}

// ===========================================================================
//  2. Framing parity, deterministically
// ===========================================================================

TEST_CASE("the direct path frames every view exactly as the two-step path", "[reframe][direct][geometry]") {
    auto rig = syntheticRig(3000);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);

    // Both a real-world-sized stabilisation and none at all.
    struct StabCase {
        const char* name;
        Mat3d rotation;
    };
    const StabCase stabs[] = {{"stabilised (yaw 23, pitch -11, roll 7)", synthStabilisation()},
                              {"unstabilised", Mat3d::identity()}};

    for (const StabCase& stab : stabs) {
        // The importer's native equirect, and the map of what it traces.
        const OsvRenderParams eq = equirectBlock(rig.value(), cp, stab.rotation, 6000, 3000);
        const DirectionMap map = makeDirectionMap(eq);
        StitchState stitch;
        stitch.equirect = eq;

        for (const ViewCase& c : framingCases()) {
            const DirectSetup d = buildDirectParams(c.settings, stitch, c.outW, c.outH, c.sequence);
            INFO(stab.name << " / " << c.name << ": reject " << directRejectName(d.reject));
            REQUIRE(d.valid);
            const FramingResult r = measureFraming(c, map, d.params);
            WARN("framing [" << stab.name << "] " << c.name << ": max " << r.maxErrPx << " px ("
                             << r.maxErrArcsec << " arcsec), mean " << r.meanErrPx << " px over " << r.samples
                             << " pixels, coverage mismatches " << r.coverageMismatch);
            CHECK(r.samples > 1000);
            CHECK(r.maxErrPx < 0.5);
            // Coverage must agree too: a pixel painted by one path and left
            // transparent by the other is a framing difference as well.
            CHECK(r.coverageMismatch == 0);
        }
    }
}

TEST_CASE("the framing measurement catches a rotation composed in the wrong order", "[reframe][direct][geometry]") {
    // Negative control for the test above: compose the stabilisation on the
    // wrong side (Rout_view * R_stab) and the error must be many pixels.  If
    // this ever passed, the parity test would be measuring nothing.
    auto rig = syntheticRig(3000);
    REQUIRE(rig.ok());
    const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
    const OsvRenderParams eq = equirectBlock(rig.value(), cp, synthStabilisation(), 6000, 3000);
    const DirectionMap map = makeDirectionMap(eq);
    StitchState stitch;
    stitch.equirect = eq;
    const ViewCase c = framingCases().front();
    DirectSetup d = buildDirectParams(c.settings, stitch, c.outW, c.outH, c.sequence);
    REQUIRE(d.valid);
    const ViewSetup v = buildView(c.settings, c.outW, c.outH, c.sequence);
    REQUIRE(v.valid);
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += static_cast<double>(v.params.Rout[r * 3 + k]) * static_cast<double>(eq.Rout[k * 3 + col]);
            }
            d.params.Rout[r * 3 + col] = static_cast<float>(sum);
        }
    }
    const FramingResult wrong = measureFraming(c, map, d.params);
    WARN("negative control (wrong composition order): max " << wrong.maxErrPx << " px");
    CHECK(wrong.maxErrPx > 20.0);
}

// ===========================================================================
//  3. The picture, on the sample clip (CPU)
// ===========================================================================

TEST_CASE("direct and two-step renders of the sample clip align, and the direct one is sharper",
          "[reframe][direct][sample]") {
    REQUIRE_SAMPLE_CLIP();
    const auto clip = openSampleClip();
    constexpr std::uint32_t kFrame = 32;
    const SampleFrame frame = decodeSampleFrame(*clip, kFrame);

    // ---- the two-step source: the importer's native equirect ------------------
    render::RenderParamsBuilder builder = importerBuilder(*clip, frame);
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 6000;
    map.h = 3000;
    builder.equirect(map);
    auto job = builder.build(frame.pair);
    REQUIRE(job.ok());
    render::CpuRenderer cpu(pool());
    auto rendered = cpu.render(job.value());
    REQUIRE(rendered.ok());
    // Handed to the effect as BGRA 32f, top-down - what a GPU frame is.
    std::vector<float> equirectBgra(static_cast<std::size_t>(map.w) * map.h * 4u);
    {
        const render::ImageRGBAf& img = rendered.value();
        for (std::size_t i = 0; i < static_cast<std::size_t>(map.w) * map.h; ++i) {
            equirectBgra[i * 4 + 0] = img.data[i * 4 + 2];
            equirectBgra[i * 4 + 1] = img.data[i * 4 + 1];
            equirectBgra[i * 4 + 2] = img.data[i * 4 + 0];
            equirectBgra[i * 4 + 3] = img.data[i * 4 + 3];
        }
    }
    ConstFrameView eqView;
    eqView.base = equirectBgra.data();
    eqView.rowBytes = map.w * 16;
    eqView.width = map.w;
    eqView.height = map.h;
    eqView.layout = PixelLayout::Bgra32f;
    eqView.topDown = true;

    // ---- the direct stitch state: the SAME block the equirect was rendered with
    StitchState stitch;
    stitch.equirect = job.value().params;
    stitch.seamTable = job.value().seamShiftDeg.empty() ? nullptr : job.value().seamShiftDeg.data();
    REQUIRE(stitch.equirect.seamShiftEnabled == 1);

    struct ImageCase {
        const char* name;
        Settings settings;
        int w;
        int h;
        bool sharpnessGate;  ///< Narrow FOV: the direct render must be at least as sharp.
    };
    const ImageCase cases[] = {
        {"narrow 40 deg, master lens centre, 2560x1440",
         makeSettings(Resolution::MatchSequence, 0.0, 0.0, 0.0, 40.0, 0.0), 2560, 1440, true},
        {"narrow 40 deg, slave lens centre, 2560x1440",
         makeSettings(Resolution::MatchSequence, 180.0, -10.0, 0.0, 40.0, 0.0), 2560, 1440, true},
        {"rectilinear 90, 1920x1080", makeSettings(Resolution::MatchSequence, 30.0, 10.0, 0.0, 90.0, 0.0), 1920, 1080,
         false},
        {"wide eye-offset 150 + distortion 40, 1920x1080",
         makeSettings(Resolution::MatchSequence, -60.0, -5.0, 0.0, 150.0, 40.0), 1920, 1080, false},
        {"tiny planet (Asteroid 300), 1920x1080", asteroidSettings(Resolution::MatchSequence), 1920, 1080, false},
        {"across the lens seam (pan 90), 1920x1080",
         makeSettings(Resolution::MatchSequence, 90.0, 0.0, 0.0, 100.0, 0.0), 1920, 1080, false},
        {"rolled + source-rotated, 1920x1080",
         makeSettings(Resolution::MatchSequence, -40.0, 15.0, 25.0, 110.0, 20.0, 30.0, -12.0, 8.0), 1920, 1080,
         false},
    };

    for (const ImageCase& c : cases) {
        INFO(c.name);
        // Two-step: the effect's equirect path on the importer's panorama.
        const KernelSetup twoSetup = buildParams(c.settings, eqView, c.w, c.h, SizePx{});
        REQUIRE(twoSetup.valid);
        HostFrame two(c.w, c.h, PixelLayout::Bgra32f);
        REQUIRE(renderCpu(twoSetup, eqView, two.view(), &pool()));

        // Direct: the CPU twin straight from the decoded fisheyes.
        const DirectSetup directSetup = buildDirectParams(c.settings, stitch, c.w, c.h, SizePx{});
        REQUIRE(directSetup.valid);
        HostFrame direct(c.w, c.h, PixelLayout::Bgra32f);
        REQUIRE(renderDirectCpu(directSetup, job.value().planes.data(), direct.view(), &pool()));

        const Luma a = lumaOf(two);
        const Luma b = lumaOf(direct);
        const Alignment al = alignmentOf(a, b);
        const Sharpness sTwo = sharpnessOf(a, b);
        const Sharpness sDirect = sharpnessOf(b, a);
        WARN("image [" << c.name << "]: NCC " << al.ncc[1][1] << " (peak at zero: " << al.peakAtZero
                       << ", sub-pixel peak " << al.peakX << ", " << al.peakY << " px); mean gradient two-step "
                       << sTwo.meanGradient << " vs direct " << sDirect.meanGradient << " ("
                       << (sDirect.meanGradient / sTwo.meanGradient) << "x); Laplacian variance two-step "
                       << sTwo.laplacianVariance << " vs direct " << sDirect.laplacianVariance << " ("
                       << (sDirect.laplacianVariance / sTwo.laplacianVariance) << "x)");
        CHECK(al.ncc[1][1] > 0.95);
        CHECK(al.peakAtZero);
        CHECK(std::fabs(al.peakX) < 0.5);
        CHECK(std::fabs(al.peakY) < 0.5);
        if (c.sharpnessGate) {
            CHECK(sDirect.meanGradient >= sTwo.meanGradient);
            CHECK(sDirect.laplacianVariance >= sTwo.laplacianVariance);
        }
    }
}

// ===========================================================================
//  4. The GPU kernel against its CPU twin
// ===========================================================================

namespace {

/// Everything one GPU test needs, torn down in the only safe order: our
/// module and allocations first, then the frames (and with them, on the
/// NVDEC route, the context they were decoded in).
///
/// The kernel is loaded into the context the FRAMES live in - FFmpeg's CUDA
/// context on the NVDEC route, the primary context on the upload route -
/// which is exactly the production arrangement: the GPU clip decoder
/// (WP-A) decodes into Premiere's context, where the effect's module lives.
struct GpuRig {
    // ---- where the frames came from ------------------------------------------
    std::optional<video::DualStreamReader> nvdecReader;  ///< Keeps FFmpeg's device context alive.
    video::FramePair nvdecPair;                         ///< Keeps the NVDEC surfaces alive.
    std::string route;                                  ///< "NVDEC zero-copy" or "host upload".

    // ---- the context and our objects in it -----------------------------------
    CUcontext context = nullptr;
    CUdevice cuDevice = 0;
    bool retainedPrimary = false;
    CUmodule module = nullptr;
    CUfunction kernel = nullptr;
    CUstream stream = nullptr;
    std::vector<CUdeviceptr> allocations;

    // ---- the two lens frames, device and host views of the SAME samples -------
    OsvPlane devicePlanes[2]{};
    std::vector<osv_u16> hostY[2];
    std::vector<osv_u16> hostUV[2];
    OsvPlane hostPlanes[2]{};

    GpuRig() = default;
    GpuRig(const GpuRig&) = delete;
    GpuRig& operator=(const GpuRig&) = delete;

    ~GpuRig() {
        if (context && cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            if (stream) {
                cuStreamSynchronize(stream);
                cuStreamDestroy(stream);
            }
            for (const CUdeviceptr p : allocations) {
                cuMemFree(p);
            }
            if (module) {
                cuModuleUnload(module);
            }
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
        }
        if (retainedPrimary) {
            cuDevicePrimaryCtxRelease(cuDevice);
        }
        // nvdecPair and nvdecReader are destroyed after this body, i.e. after
        // everything of ours in their context is gone.
    }

    /// Allocate `bytes` of device memory tracked for release.
    CUdeviceptr alloc(std::size_t bytes) {
        CUdeviceptr p = 0;
        REQUIRE(cuMemAlloc(&p, bytes) == CUDA_SUCCESS);
        allocations.push_back(p);
        return p;
    }
    /// Allocate a pitched 2-D region tracked for release.
    CUdeviceptr allocPitch(std::size_t widthBytes, std::size_t rows, std::size_t& pitch) {
        CUdeviceptr p = 0;
        REQUIRE(cuMemAllocPitch(&p, &pitch, widthBytes, rows, 16) == CUDA_SUCCESS);
        allocations.push_back(p);
        return p;
    }
};

/// RAII push / pop of the rig's context around the test's own driver calls.
class RigContext {
public:
    explicit RigContext(CUcontext c) noexcept : m_ok(c && cuCtxPushCurrent(c) == CUDA_SUCCESS) {}
    ~RigContext() {
        if (m_ok) {
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
        }
    }
    RigContext(const RigContext&) = delete;
    RigContext& operator=(const RigContext&) = delete;
    [[nodiscard]] bool ok() const noexcept { return m_ok; }

private:
    bool m_ok = false;
};

/// A host OsvPlane over tight P010 buffers (luma pitch w, CbCr pitch 2 cw).
OsvPlane hostP010Plane(const std::vector<osv_u16>& y, const std::vector<osv_u16>& uv, int w, int h) {
    OsvPlane p{};
    p.y = y.data();
    p.u = uv.data();
    p.v = uv.data() + 1;
    p.w = w;
    p.h = h;
    p.cw = (w + 1) / 2;
    p.ch = (h + 1) / 2;
    p.strideY = w;
    p.strideC = 2 * p.cw;
    p.bitShift = 6;
    p.chromaInterleaved = 1;
    return p;
}

/// Is there a CUDA device at all?  Empty string when yes, the reason when no.
std::string cudaUnavailableReason() {
    const CUresult init = cuInit(0);
    if (init != CUDA_SUCCESS) {
        return "cuInit failed (" + std::to_string(static_cast<int>(init)) + ")";
    }
    int count = 0;
    if (cuDeviceGetCount(&count) != CUDA_SUCCESS || count <= 0) {
        return "no CUDA device";
    }
    return {};
}

/// Build the rig for frame `frameIndex`: NVDEC keepOnDevice frames when the
/// hardware decoder is available (their context found from the pointer), a
/// host upload of the software-decoded frame into the primary context
/// otherwise.  Either way `host` holds the very samples `device` holds.
void prepareGpuRig(GpuRig& g, const SampleClip& clip, const SampleFrame& soft, std::uint32_t frameIndex) {
    // ---- route 1: NVDEC, frames already in VRAM ------------------------------
    video::DecoderOptions opt;
    opt.hw = video::HwAccel::Cuda;
    opt.keepOnDevice = true;
    auto reader = video::DualStreamReader::open(sampleClipPath(), clip.format, opt);
    if (reader.ok()) {
        g.nvdecReader.emplace(std::move(reader).value());
        auto pair = g.nvdecReader->read(frameIndex);
        if (pair.ok() && pair.value().onDevice()) {
            g.nvdecPair = std::move(pair).value();
            CUcontext ctx = nullptr;
            const CUdeviceptr y0 = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(g.nvdecPair.device[0].yDevice));
            if (cuPointerGetAttribute(&ctx, CU_POINTER_ATTRIBUTE_CONTEXT, y0) == CUDA_SUCCESS && ctx) {
                g.context = ctx;
                g.route = "NVDEC zero-copy";
            }
        }
    }

    if (g.context) {
        RigContext scope(g.context);
        REQUIRE(scope.ok());
        REQUIRE(cuCtxGetDevice(&g.cuDevice) == CUDA_SUCCESS);
        for (int i = 0; i < 2; ++i) {
            const video::DeviceFrameRef& ref = g.nvdecPair.device[static_cast<std::size_t>(i)];
            REQUIRE(render::fillDevicePlane(ref, g.devicePlanes[i]));
            // Download the same P010 samples for the CPU twin.
            const int w = static_cast<int>(ref.width);
            const int h = static_cast<int>(ref.height);
            const int cw = (w + 1) / 2;
            const int ch = (h + 1) / 2;
            g.hostY[i].assign(static_cast<std::size_t>(w) * h, 0u);
            g.hostUV[i].assign(static_cast<std::size_t>(2 * cw) * ch, 0u);
            CUDA_MEMCPY2D c{};
            c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            c.srcDevice = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(ref.yDevice));
            c.srcPitch = ref.pitchBytes;
            c.dstMemoryType = CU_MEMORYTYPE_HOST;
            c.dstHost = g.hostY[i].data();
            c.dstPitch = static_cast<std::size_t>(w) * 2u;
            c.WidthInBytes = static_cast<std::size_t>(w) * 2u;
            c.Height = static_cast<std::size_t>(h);
            REQUIRE(cuMemcpy2D(&c) == CUDA_SUCCESS);
            c.srcDevice = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(ref.uvDevice));
            c.dstHost = g.hostUV[i].data();
            c.dstPitch = static_cast<std::size_t>(2 * cw) * 2u;
            c.WidthInBytes = static_cast<std::size_t>(2 * cw) * 2u;
            c.Height = static_cast<std::size_t>(ch);
            REQUIRE(cuMemcpy2D(&c) == CUDA_SUCCESS);
            g.hostPlanes[i] = hostP010Plane(g.hostY[i], g.hostUV[i], w, h);
        }
    } else {
        // ---- route 2: upload the software-decoded frame as P010 -------------
        REQUIRE(cuDeviceGet(&g.cuDevice, 0) == CUDA_SUCCESS);
        REQUIRE(cuDevicePrimaryCtxRetain(&g.context, g.cuDevice) == CUDA_SUCCESS);
        g.retainedPrimary = true;
        g.route = "host upload";
        RigContext scope(g.context);
        REQUIRE(scope.ok());
        for (int i = 0; i < 2; ++i) {
            const video::PlanarFrame16& f = soft.pair.lens[static_cast<std::size_t>(i)];
            const int w = static_cast<int>(f.width);
            const int h = static_cast<int>(f.height);
            const int cw = (w + 1) / 2;
            const int ch = (h + 1) / 2;
            // Repack into P010: 10-bit values in the top bits, CbCr interleaved.
            g.hostY[i].assign(static_cast<std::size_t>(w) * h, 0u);
            g.hostUV[i].assign(static_cast<std::size_t>(2 * cw) * ch, 0u);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    g.hostY[i][static_cast<std::size_t>(y) * w + x] =
                        static_cast<osv_u16>(f.luma(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) << 6);
                }
            }
            for (int y = 0; y < ch; ++y) {
                for (int x = 0; x < cw; ++x) {
                    const std::size_t o = static_cast<std::size_t>(y) * (2 * cw) + 2 * x;
                    g.hostUV[i][o + 0] = static_cast<osv_u16>(
                        f.chroma(1, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) << 6);
                    g.hostUV[i][o + 1] = static_cast<osv_u16>(
                        f.chroma(2, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) << 6);
                }
            }
            g.hostPlanes[i] = hostP010Plane(g.hostY[i], g.hostUV[i], w, h);
            // One pitched allocation per plane, like a decoder surface.
            std::size_t pitchY = 0;
            std::size_t pitchC = 0;
            const CUdeviceptr dy = g.allocPitch(static_cast<std::size_t>(w) * 2u, static_cast<std::size_t>(h), pitchY);
            const CUdeviceptr duv =
                g.allocPitch(static_cast<std::size_t>(2 * cw) * 2u, static_cast<std::size_t>(ch), pitchC);
            CUDA_MEMCPY2D c{};
            c.srcMemoryType = CU_MEMORYTYPE_HOST;
            c.srcHost = g.hostY[i].data();
            c.srcPitch = static_cast<std::size_t>(w) * 2u;
            c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            c.dstDevice = dy;
            c.dstPitch = pitchY;
            c.WidthInBytes = static_cast<std::size_t>(w) * 2u;
            c.Height = static_cast<std::size_t>(h);
            REQUIRE(cuMemcpy2D(&c) == CUDA_SUCCESS);
            c.srcHost = g.hostUV[i].data();
            c.srcPitch = static_cast<std::size_t>(2 * cw) * 2u;
            c.dstDevice = duv;
            c.dstPitch = pitchC;
            c.WidthInBytes = static_cast<std::size_t>(2 * cw) * 2u;
            c.Height = static_cast<std::size_t>(ch);
            REQUIRE(cuMemcpy2D(&c) == CUDA_SUCCESS);
            OsvPlane d = g.hostPlanes[i];
            d.y = reinterpret_cast<const osv_u16*>(static_cast<std::uintptr_t>(dy));
            d.u = reinterpret_cast<const osv_u16*>(static_cast<std::uintptr_t>(duv));
            d.v = d.u + 1;
            d.strideY = static_cast<int>(pitchY / 2u);
            d.strideC = static_cast<int>(pitchC / 2u);
            g.devicePlanes[i] = d;
        }
    }

    // ---- the kernel, from the fatbin the .aex embeds ---------------------------
    RigContext scope(g.context);
    REQUIRE(scope.ok());
    REQUIRE(kOsvReframeFatbin_size > 0);
    REQUIRE(cuModuleLoadFatBinary(&g.module, kOsvReframeFatbin) == CUDA_SUCCESS);
    REQUIRE(cuModuleGetFunction(&g.kernel, g.module, kDirectKernelName) == CUDA_SUCCESS);
    REQUIRE(cuStreamCreate(&g.stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);
}

/// Upload a float table and return its device address (tracked by the rig).
const float* uploadTable(GpuRig& g, const std::vector<float>& table) {
    RigContext scope(g.context);
    REQUIRE(scope.ok());
    const CUdeviceptr p = g.alloc(table.size() * sizeof(float));
    REQUIRE(cuMemcpyHtoD(p, table.data(), table.size() * sizeof(float)) == CUDA_SUCCESS);
    return reinterpret_cast<const float*>(static_cast<std::uintptr_t>(p));
}

/// Render one view on the GPU into a pitched device frame and download it.
HostFrame renderOnGpu(GpuRig& g, const DirectSetup& setup, bool isHalf) {
    RigContext scope(g.context);
    REQUIRE(scope.ok());
    const int w = setup.params.outW;
    const int h = setup.params.outH;
    const PixelLayout layout = isHalf ? PixelLayout::Bgra16f : PixelLayout::Bgra32f;
    const std::size_t bpp = bytesPerPixel(layout);
    std::size_t pitch = 0;
    const CUdeviceptr out = g.allocPitch(static_cast<std::size_t>(w) * bpp, static_cast<std::size_t>(h), pitch);
    DirectOutput o;
    o.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(out));
    o.rowBytes = static_cast<std::int32_t>(pitch);
    o.width = w;
    o.height = h;
    o.isHalf = isHalf;
    const DirectLaunchResult r = launchDirect(g.kernel, g.stream, setup, g.devicePlanes, o);
    INFO("launch: " << directLaunchRejectName(r.reject) << " (CUresult " << static_cast<int>(r.result) << ")");
    REQUIRE(r.ok());
    REQUIRE(cuStreamSynchronize(g.stream) == CUDA_SUCCESS);
    HostFrame frame(w, h, layout);
    CUDA_MEMCPY2D c{};
    c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    c.srcDevice = out;
    c.srcPitch = pitch;
    c.dstMemoryType = CU_MEMORYTYPE_HOST;
    c.dstHost = frame.bytes.data();
    c.dstPitch = static_cast<std::size_t>(w) * bpp;
    c.WidthInBytes = static_cast<std::size_t>(w) * bpp;
    c.Height = static_cast<std::size_t>(h);
    REQUIRE(cuMemcpy2D(&c) == CUDA_SUCCESS);
    return frame;
}

/// Mean time of one kernel launch at the setup's size, from CUDA events
/// around `iterations` back-to-back launches (after a warm-up).
double kernelMs(GpuRig& g, const DirectSetup& setup, int iterations) {
    RigContext scope(g.context);
    REQUIRE(scope.ok());
    const int w = setup.params.outW;
    const int h = setup.params.outH;
    std::size_t pitch = 0;
    const CUdeviceptr out = g.allocPitch(static_cast<std::size_t>(w) * 16u, static_cast<std::size_t>(h), pitch);
    DirectOutput o{reinterpret_cast<void*>(static_cast<std::uintptr_t>(out)), static_cast<std::int32_t>(pitch), w, h,
                   false};
    for (int i = 0; i < 3; ++i) {
        REQUIRE(launchDirect(g.kernel, g.stream, setup, g.devicePlanes, o).ok());
    }
    CUevent start = nullptr;
    CUevent stop = nullptr;
    REQUIRE(cuEventCreate(&start, CU_EVENT_DEFAULT) == CUDA_SUCCESS);
    REQUIRE(cuEventCreate(&stop, CU_EVENT_DEFAULT) == CUDA_SUCCESS);
    REQUIRE(cuEventRecord(start, g.stream) == CUDA_SUCCESS);
    for (int i = 0; i < iterations; ++i) {
        REQUIRE(launchDirect(g.kernel, g.stream, setup, g.devicePlanes, o).ok());
    }
    REQUIRE(cuEventRecord(stop, g.stream) == CUDA_SUCCESS);
    REQUIRE(cuEventSynchronize(stop) == CUDA_SUCCESS);
    float ms = 0.0f;
    REQUIRE(cuEventElapsedTime(&ms, start, stop) == CUDA_SUCCESS);
    cuEventDestroy(start);
    cuEventDestroy(stop);
    return static_cast<double>(ms) / static_cast<double>(iterations);
}

}  // namespace

TEST_CASE("the direct kernel matches its CPU twin on the sample clip", "[reframe][direct][cuda][sample]") {
    const std::string noCuda = cudaUnavailableReason();
    if (!noCuda.empty()) {
        SKIP("no CUDA device: " << noCuda);
    }
    REQUIRE_SAMPLE_CLIP();
    const auto clip = openSampleClip();
    constexpr std::uint32_t kFrame = 32;
    const SampleFrame soft = decodeSampleFrame(*clip, kFrame);

    GpuRig g;
    prepareGpuRig(g, *clip, soft, kFrame);
    WARN("direct kernel test: device frames via " << g.route);

    // The importer's native panorama: only its layout matters to the direct
    // path (the size is overwritten by the view), but the builder needs one.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 6000;
    map.h = 3000;

    // ---- stitch A: the importer default (seam table) ---------------------------
    render::RenderParamsBuilder seamBuilder = importerBuilder(*clip, soft);
    auto seamBlock = seamBuilder.equirect(map).buildParams();
    REQUIRE(seamBlock.ok());
    REQUIRE(seamBlock.value().seamShiftEnabled == 1);
    StitchState seamHost;
    seamHost.equirect = seamBlock.value();
    seamHost.seamTable = soft.seamTable.data();
    StitchState seamDevice = seamHost;
    seamDevice.seamTable = uploadTable(g, soft.seamTable);

    // ---- stitch B: the 2-D parallax warp grid (replaces the seam table) --------
    render::ParallaxWarpParams pw;
    pw.backend = render::FlowBackendKind::Classical;
    auto grid = render::buildParallaxWarp(clip->rig, soft.pair, clip->blend, pw, nullptr, pool());
    INFO("parallax grid: " << (grid.ok() ? std::string("accepted") : grid.error().message));
    REQUIRE(grid.ok());
    render::RenderParamsBuilder warpBuilder;
    warpBuilder.rig(clip->rig).color(clip->color).blend(clip->blend, true).alphaCoverage(true);
    warpBuilder.warp(grid.value().uv, grid.value().w, grid.value().h, grid.value().latMinRad, grid.value().latMaxRad);
    warpBuilder.gain(soft.gains[0], soft.gains[1]).stabilization(soft.stab);
    auto warpBlock = warpBuilder.equirect(map).buildParams();
    REQUIRE(warpBlock.ok());
    REQUIRE(warpBlock.value().warpEnabled == 1);
    StitchState warpHost;
    warpHost.equirect = warpBlock.value();
    warpHost.warpGrid = grid.value().uv.data();
    StitchState warpDevice = warpHost;
    warpDevice.warpGrid = uploadTable(g, grid.value().uv);

    struct GpuCase {
        const char* name;
        Settings settings;
        const StitchState* host;
        const StitchState* device;
    };
    const GpuCase cases[] = {
        {"across the seam, seam table", makeSettings(Resolution::MatchSequence, 90.0, 0.0, 0.0, 100.0, 0.0), &seamHost,
         &seamDevice},
        {"narrow 40, warp grid", makeSettings(Resolution::MatchSequence, 95.0, 5.0, 0.0, 40.0, 0.0), &warpHost,
         &warpDevice},
        {"tiny planet, seam table", asteroidSettings(Resolution::MatchSequence), &seamHost, &seamDevice},
        {"rolled + source-rotated eye-offset, warp grid",
         makeSettings(Resolution::MatchSequence, -40.0, 15.0, 25.0, 150.0, 40.0, 30.0, -12.0, 8.0), &warpHost,
         &warpDevice},
    };

    for (const GpuCase& c : cases) {
        for (const bool isHalf : {false, true}) {
            INFO(c.name << (isHalf ? " (16f)" : " (32f)"));
            const DirectSetup cpuSetup = buildDirectParams(c.settings, *c.host, 1920, 1080, SizePx{});
            const DirectSetup gpuSetup = buildDirectParams(c.settings, *c.device, 1920, 1080, SizePx{});
            REQUIRE(cpuSetup.valid);
            REQUIRE(gpuSetup.valid);

            const HostFrame gpu = renderOnGpu(g, gpuSetup, isHalf);
            HostFrame cpuFrame(1920, 1080, isHalf ? PixelLayout::Bgra16f : PixelLayout::Bgra32f);
            REQUIRE(renderDirectCpu(cpuSetup, g.hostPlanes, cpuFrame.view(), &pool()));

            const render::ImageDiffStats stats = render::compareImages16(gpu.toImage(), cpuFrame.toImage());
            WARN("GPU vs CPU twin [" << c.name << (isHalf ? ", 16f" : ", 32f") << "]: PSNR " << stats.psnrDb
                                     << " dB, max " << stats.maxAbsCode << " codes, "
                                     << (100.0 * stats.fractionWithin2) << "% within 2 codes");
            CHECK(stats.psnrDb >= 60.0);
        }
    }

    // ---- timing -------------------------------------------------------------------
    for (const auto& size : {std::pair<int, int>{2560, 1440}, std::pair<int, int>{3840, 2160}}) {
        const Settings s = makeSettings(Resolution::MatchSequence, 90.0, 0.0, 0.0, 100.0, 0.0);
        const DirectSetup seamSetup = buildDirectParams(s, seamDevice, size.first, size.second, SizePx{});
        const DirectSetup warpSetup = buildDirectParams(s, warpDevice, size.first, size.second, SizePx{});
        REQUIRE(seamSetup.valid);
        REQUIRE(warpSetup.valid);
        const double seamMs = kernelMs(g, seamSetup, 50);
        const double warpMs = kernelMs(g, warpSetup, 50);
        // Host-side cost of the launch itself (validation + pointer queries).
        RigContext scope(g.context);
        REQUIRE(scope.ok());
        std::size_t pitch = 0;
        const CUdeviceptr out =
            g.allocPitch(static_cast<std::size_t>(size.first) * 16u, static_cast<std::size_t>(size.second), pitch);
        const DirectOutput o{reinterpret_cast<void*>(static_cast<std::uintptr_t>(out)),
                             static_cast<std::int32_t>(pitch), size.first, size.second, false};
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 50; ++i) {
            REQUIRE(launchDirect(g.kernel, g.stream, seamSetup, g.devicePlanes, o).ok());
        }
        const double hostUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 50.0;
        REQUIRE(cuStreamSynchronize(g.stream) == CUDA_SUCCESS);
        WARN("direct kernel " << size.first << "x" << size.second << " (32f, view across the seam): " << seamMs
                              << " ms with the seam table, " << warpMs << " ms with the warp grid; launchDirect host "
                              << "cost " << hostUs << " us per call");
        CHECK(seamMs > 0.0);
    }
}

TEST_CASE("launchDirect refuses host memory and a too-small output before launching", "[reframe][direct][cuda]") {
    const std::string noCuda = cudaUnavailableReason();
    if (!noCuda.empty()) {
        SKIP("no CUDA device: " << noCuda);
    }
    // A real module and context, synthetic frames: this is about the
    // refusals, which must hold whatever the pixels are.
    CUdevice dev = 0;
    REQUIRE(cuDeviceGet(&dev, 0) == CUDA_SUCCESS);
    CUcontext ctx = nullptr;
    REQUIRE(cuDevicePrimaryCtxRetain(&ctx, dev) == CUDA_SUCCESS);
    {
        RigContext scope(ctx);
        REQUIRE(scope.ok());
        CUmodule module = nullptr;
        REQUIRE(cuModuleLoadFatBinary(&module, kOsvReframeFatbin) == CUDA_SUCCESS);
        CUfunction fn = nullptr;
        REQUIRE(cuModuleGetFunction(&fn, module, kDirectKernelName) == CUDA_SUCCESS);

        auto rig = syntheticRig(600);
        REQUIRE(rig.ok());
        const OsvColorParams cp = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
        StitchState stitch;
        stitch.equirect = equirectBlock(rig.value(), cp, Mat3d::identity(), 1200, 600);
        const DirectSetup setup =
            buildDirectParams(makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0), stitch, 64, 36, SizePx{});
        REQUIRE(setup.valid);

        // Device P010 planes of the right shape.
        std::vector<osv_u16> hy(600u * 600u, 512u << 6);
        std::vector<osv_u16> huv(600u * 300u, 512u << 6);
        CUdeviceptr dy = 0;
        CUdeviceptr duv = 0;
        REQUIRE(cuMemAlloc(&dy, hy.size() * 2u) == CUDA_SUCCESS);
        REQUIRE(cuMemAlloc(&duv, huv.size() * 2u) == CUDA_SUCCESS);
        REQUIRE(cuMemcpyHtoD(dy, hy.data(), hy.size() * 2u) == CUDA_SUCCESS);
        REQUIRE(cuMemcpyHtoD(duv, huv.data(), huv.size() * 2u) == CUDA_SUCCESS);
        const OsvPlane hostPlane = hostP010Plane(hy, huv, 600, 600);
        OsvPlane devPlane = hostPlane;
        devPlane.y = reinterpret_cast<const osv_u16*>(static_cast<std::uintptr_t>(dy));
        devPlane.u = reinterpret_cast<const osv_u16*>(static_cast<std::uintptr_t>(duv));
        devPlane.v = devPlane.u + 1;
        const OsvPlane devPlanes[2] = {devPlane, devPlane};
        const OsvPlane hostPlanes[2] = {hostPlane, hostPlane};

        // An output one row short of the frame it claims to be.
        CUdeviceptr shortOut = 0;
        REQUIRE(cuMemAlloc(&shortOut, 64u * 16u * 35u) == CUDA_SUCCESS);
        CUdeviceptr fullOut = 0;
        REQUIRE(cuMemAlloc(&fullOut, 64u * 16u * 36u) == CUDA_SUCCESS);
        const DirectOutput shortFrame{reinterpret_cast<void*>(static_cast<std::uintptr_t>(shortOut)), 64 * 16, 64, 36,
                                      false};
        const DirectOutput fullFrame{reinterpret_cast<void*>(static_cast<std::uintptr_t>(fullOut)), 64 * 16, 64, 36,
                                     false};
        std::vector<float> hostOut(64u * 36u * 4u);
        const DirectOutput hostFrame{hostOut.data(), 64 * 16, 64, 36, false};

        CHECK(launchDirect(fn, nullptr, setup, hostPlanes, fullFrame).reject == DirectLaunchReject::Memory);
        CHECK(launchDirect(fn, nullptr, setup, devPlanes, shortFrame).reject == DirectLaunchReject::Memory);
        CHECK(launchDirect(fn, nullptr, setup, devPlanes, hostFrame).reject == DirectLaunchReject::Memory);
        // A seam table claimed longer than its allocation.
        StitchState seamStitch = stitch;
        CUdeviceptr seam = 0;
        REQUIRE(cuMemAlloc(&seam, 100u * sizeof(float)) == CUDA_SUCCESS);
        seamStitch.equirect.seamShiftEnabled = 1;
        seamStitch.equirect.seamColumns = 360;
        seamStitch.seamTable = reinterpret_cast<const float*>(static_cast<std::uintptr_t>(seam));
        const DirectSetup seamSetup =
            buildDirectParams(makeSettings(Resolution::MatchSequence, 0, 0, 0, 90, 0), seamStitch, 64, 36, SizePx{});
        REQUIRE(seamSetup.valid);
        CHECK(launchDirect(fn, nullptr, seamSetup, devPlanes, fullFrame).reject == DirectLaunchReject::Memory);

        // And the well-formed call launches and completes cleanly.
        const DirectLaunchResult ok = launchDirect(fn, nullptr, setup, devPlanes, fullFrame);
        CHECK(ok.ok());
        CHECK(cuCtxSynchronize() == CUDA_SUCCESS);

        cuMemFree(seam);
        cuMemFree(fullOut);
        cuMemFree(shortOut);
        cuMemFree(duv);
        cuMemFree(dy);
        cuModuleUnload(module);
    }
    cuDevicePrimaryCtxRelease(dev);
}
