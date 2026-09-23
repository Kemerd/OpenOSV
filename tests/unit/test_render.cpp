// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Renderer tests: synthetic lens frames (no clip needed), the sample clip
// through the CPU reference renderer, and CPU vs CUDA / OpenCL parity.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/color/DlogM.h"
#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/Renderer.h"
#include "osv/video/DualStreamReader.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif
#if defined(OSV_HAVE_METAL)
#include "osv/render/MetalRenderer.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace osv;

namespace {

// -----------------------------------------------------------------------------
//  Synthetic frames
// -----------------------------------------------------------------------------

/// Owns planar 10-bit YCbCr storage for a constant-colour frame.
struct SyntheticFrame {
    std::shared_ptr<std::vector<std::uint16_t>> storage;
    video::PlanarFrame16 frame;
};

/// Constant frame: luma code `y`, chroma codes `cb`, `cr` (10-bit scale).
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

/// The sample clip's calibration as a stream-space rig at a given size.
/// Uses the verified constants directly so this test does not depend on the
/// metadata decoder.
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

double meanLuma(const render::ImageRGBAf& img) {
    double s = 0;
    for (std::uint32_t y = 0; y < img.h; ++y) {
        const float* r = img.row(y);
        for (std::uint32_t x = 0; x < img.w; ++x) {
            s += 0.2627 * r[x * 4] + 0.6780 * r[x * 4 + 1] + 0.0593 * r[x * 4 + 2];
        }
    }
    return s / (static_cast<double>(img.w) * img.h);
}

bool allFinite(const render::ImageRGBAf& img) {
    for (float v : img.data) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Synthetic tests (run everywhere)
// -----------------------------------------------------------------------------
TEST_CASE("flat grey lenses render to the closed-form HLG and PQ values", "[render]") {
    ThreadPool pool(4);
    render::CpuRenderer cpu(pool);
    const int W = 600;
    auto rig = makeSampleRig(W);
    REQUIRE(rig.ok());

    // D-Log M code 0.40 = narrow 10-bit luma 64 + 0.40 * 876 = 414.4 -> 414; neutral chroma 512.
    const std::uint16_t lumaCode = static_cast<std::uint16_t>(std::lround(64.0 + 0.40 * 876.0));
    auto slave = makeConstantFrame(W, W, lumaCode, 512, 512);
    auto master = makeConstantFrame(W, W, lumaCode, 512, 512);
    video::FramePair pair;
    pair.lens = {slave.frame, master.frame};

    geom::VirtualCamera cam;
    cam.projection = geom::Projection::Rectilinear;
    cam.w = 160;
    cam.h = 90;
    cam.hfovDeg = 90;

    for (const auto transfer : {color::OutputTransfer::HLG, color::OutputTransfer::PQ}) {
        const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, transfer, 0.0f);
        auto job = render::RenderParamsBuilder().rig(rig.value()).camera(cam).color(cp).build(pair);
        REQUIRE(job.ok());
        auto img = cpu.render(job.value());
        REQUIRE(img.ok());
        REQUIRE(allFinite(img.value()));

        // Expected: the same code through the colour pipeline directly.
        const float code = (static_cast<float>(lumaCode) - 64.0f) / 876.0f;
        float codes[3] = {code, code, code};
        float lin[3];
        osvCodeToLinear(&cp, codes, lin);
        float expected[3];
        osvLinearToOutput(&cp, lin, expected);
        // Every visible pixel must equal the closed-form value; alpha 1.
        const render::ImageRGBAf& out = img.value();
        std::size_t checked = 0;
        for (std::uint32_t y = 0; y < out.h; y += 7) {
            for (std::uint32_t x = 0; x < out.w; x += 5) {
                const float* px = out.pixel(x, y);
                REQUIRE(px[3] == 1.0f);
                REQUIRE_THAT(px[0], Catch::Matchers::WithinAbs(expected[0], 0.005));
                REQUIRE_THAT(px[1], Catch::Matchers::WithinAbs(expected[1], 0.005));
                REQUIRE_THAT(px[2], Catch::Matchers::WithinAbs(expected[2], 0.005));
                ++checked;
            }
        }
        REQUIRE(checked > 100);
        if (transfer == color::OutputTransfer::HLG) {
            // BT.2408 anchor: grey code 0.40 -> HLG 0.38 with the DJI refit.
            REQUIRE_THAT(expected[1], Catch::Matchers::WithinAbs(0.380, 0.012));
        }
    }
}

TEST_CASE("two flat-colour lenses: master-facing pixels take the master colour, seam blends", "[render]") {
    ThreadPool pool(4);
    render::CpuRenderer cpu(pool);
    const int W = 600;
    auto rig = makeSampleRig(W);
    REQUIRE(rig.ok());

    // Slave dark (code 0.25), master bright (code 0.60), passthrough transfer
    // so the output IS the code and we can reason about weights directly.
    const auto codeToLuma = [](double c) { return static_cast<std::uint16_t>(std::lround(64.0 + c * 876.0)); };
    auto slave = makeConstantFrame(W, W, codeToLuma(0.25), 512, 512);
    auto master = makeConstantFrame(W, W, codeToLuma(0.60), 512, 512);
    video::FramePair pair;
    pair.lens = {slave.frame, master.frame};
    const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::Passthrough, 0.0f);

    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = 360;
    map.h = 180;
    geom::BlendParams blend;
    blend.useOcclusionMask = false;
    auto job = render::RenderParamsBuilder().rig(rig.value()).equirect(map).blend(blend).color(cp).build(pair);
    REQUIRE(job.ok());
    auto img = cpu.render(job.value());
    REQUIRE(img.ok());
    const render::ImageRGBAf& out = img.value();

    // Centre column looks along +Y = master axis.
    const float* centre = out.pixel(180, 90);
    REQUIRE_THAT(centre[0], Catch::Matchers::WithinAbs(0.60, 0.01));
    REQUIRE(centre[3] == 1.0f);
    // Column 0 looks along -Y = slave axis.
    const float* back = out.pixel(0, 90);
    REQUIRE_THAT(back[0], Catch::Matchers::WithinAbs(0.25, 0.01));
    // The seam (+/-X, columns 90 and 270) blends both: strictly between.
    const float* seamPx = out.pixel(90, 90);
    REQUIRE(seamPx[0] > 0.26f);
    REQUIRE(seamPx[0] < 0.59f);
    // Everything is covered (no black holes) in the equirect.
    std::size_t uncovered = 0;
    for (std::uint32_t y = 0; y < out.h; ++y) {
        for (std::uint32_t x = 0; x < out.w; ++x) {
            if (out.pixel(x, y)[3] < 0.999f) {
                ++uncovered;
            }
        }
    }
    REQUIRE(uncovered == 0);
}

TEST_CASE("RenderParamsBuilder validates its inputs", "[render]") {
    render::RenderParamsBuilder b;
    REQUIRE(b.buildParams().error().code == ErrorCode::InvalidArgument);
    auto rig = makeSampleRig(600);
    REQUIRE(rig.ok());
    b.rig(rig.value());
    REQUIRE(b.buildParams().error().code == ErrorCode::InvalidArgument);
    b.color(color::makeColorParams(color::DlogMFit::Pocket3, color::OutputTransfer::HLG, 0.0f));
    REQUIRE(b.buildParams().error().code == ErrorCode::InvalidArgument);
    geom::VirtualCamera cam;
    cam.w = 64;
    cam.h = 64;
    b.camera(cam);
    auto p = b.buildParams();
    REQUIRE(p.ok());
    REQUIRE(p.value().outW == 64);
    REQUIRE(p.value().lens[0].enabled == 1);
    REQUIRE(p.value().lens[1].width == 600);
    REQUIRE(sizeof(OsvRenderParams) <= 4096);
}

TEST_CASE("a frame the decoder left on the GPU builds a device job that host renderers refuse", "[render]") {
    // A keepOnDevice decode delivers PlanarFrame16s with a size but NO host
    // planes, plus a DeviceFrameRef.  The builder used to reject exactly that
    // ("lens frame 0 is invalid"), which broke the zero-copy CUDA path it was
    // designed for.  The pointers below are never dereferenced: the builder
    // only describes them, and the CPU renderer must refuse the job instead
    // of reading them as host memory.
    auto rig = makeSampleRig(600);
    REQUIRE(rig.ok());
    std::vector<std::uint16_t> fakeY(16), fakeUv(16);  // addresses only
    video::FramePair pair;
    for (int i = 0; i < 2; ++i) {
        video::PlanarFrame16& f = pair.lens[static_cast<std::size_t>(i)];
        f.width = 600;
        f.height = 600;
        REQUIRE_FALSE(f.valid());  // no host planes, as the decoder leaves it
        video::DeviceFrameRef& d = pair.device[static_cast<std::size_t>(i)];
        d.yDevice = fakeY.data();
        d.uvDevice = fakeUv.data();
        d.pitchBytes = 1280;  // P010 rows padded past 600 * 2 bytes
        d.width = 600;
        d.height = 600;
        d.bitDepth = 10;
        d.bitShift = 6;
        REQUIRE(d.valid());
    }
    geom::VirtualCamera cam;
    cam.w = 64;
    cam.h = 36;
    const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::PQ, 0.0f);
    auto job = render::RenderParamsBuilder().rig(rig.value()).camera(cam).color(cp).build(pair);
    REQUIRE(job.ok());
    for (int i = 0; i < 2; ++i) {
        const OsvPlane& pl = job.value().planes[static_cast<std::size_t>(i)];
        CHECK(job.value().planesOnDevice[static_cast<std::size_t>(i)]);
        CHECK(pl.y == fakeY.data());
        CHECK(pl.u == fakeUv.data());
        CHECK(pl.v == fakeUv.data() + 1);
        CHECK(pl.chromaInterleaved == 1);
        CHECK(pl.strideY == 640);
        CHECK(pl.bitShift == 6);
    }
    ThreadPool pool(2);
    render::CpuRenderer cpu(pool);
    auto img = cpu.render(job.value());
    REQUIRE_FALSE(img.ok());
    CHECK(img.error().code == ErrorCode::InvalidArgument);

    // And a frame with neither host planes nor a device frame is still an
    // error, with a message that says which.
    video::FramePair empty;
    empty.lens[0].width = empty.lens[1].width = 600;
    empty.lens[0].height = empty.lens[1].height = 600;
    auto bad = render::RenderParamsBuilder().rig(rig.value()).camera(cam).color(cp).build(empty);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("neither host planes nor a device frame") != std::string::npos);
}

// -----------------------------------------------------------------------------
//  Sample clip
// -----------------------------------------------------------------------------
namespace {

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

}  // namespace

TEST_CASE("sample clip renders on the CPU reference renderer", "[render][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto sp = openSampleFrame(0);
    REQUIRE(sp.ok());
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::PQ, 0.0f);

    SECTION("rectilinear 640x360 has no NaN, full alpha and real content") {
        geom::VirtualCamera cam;
        cam.w = 640;
        cam.h = 360;
        cam.hfovDeg = 90;
        auto job = render::RenderParamsBuilder().rig(sp.value().rig).camera(cam).color(cp).build(sp.value().pair);
        REQUIRE(job.ok());
        auto img = cpu.render(job.value());
        REQUIRE(img.ok());
        REQUIRE(allFinite(img.value()));
        double variance = 0, mean = 0;
        std::size_t n = 0;
        for (std::uint32_t y = 0; y < 360; ++y) {
            for (std::uint32_t x = 0; x < 640; ++x) {
                const float* px = img.value().pixel(x, y);
                REQUIRE(px[3] == 1.0f);
                mean += px[1];
                ++n;
            }
        }
        mean /= static_cast<double>(n);
        for (std::uint32_t y = 0; y < 360; ++y) {
            for (std::uint32_t x = 0; x < 640; ++x) {
                const double d = img.value().pixel(x, y)[1] - mean;
                variance += d * d;
            }
        }
        variance /= static_cast<double>(n);
        REQUIRE(variance > 1e-4);
        // Deterministic: a second render is bit identical.
        auto again = cpu.render(job.value());
        REQUIRE(again.ok());
        REQUIRE(again.value().data == img.value().data);
    }

    SECTION("equirect 1024x512 is almost fully covered") {
        geom::EquirectMap map;
        map.w = 1024;
        map.h = 512;
        auto job = render::RenderParamsBuilder().rig(sp.value().rig).equirect(map).color(cp).build(sp.value().pair);
        REQUIRE(job.ok());
        auto img = cpu.render(job.value());
        REQUIRE(img.ok());
        std::size_t uncovered = 0, uncoveredBottom = 0;
        for (std::uint32_t y = 0; y < 512; ++y) {
            for (std::uint32_t x = 0; x < 1024; ++x) {
                if (img.value().pixel(x, y)[3] < 0.5f) {
                    ++uncovered;
                    if (y >= 512 * 3 / 4) {
                        ++uncoveredBottom;
                    }
                }
            }
        }
        // Only the occlusion cap around the nadir (selfie-stick side, bottom
        // rows of the equirect) may be uncovered: measured ~5 % on the sample.
        INFO("uncovered fraction " << static_cast<double>(uncovered) / (1024.0 * 512.0));
        REQUIRE(static_cast<double>(uncovered) / (1024.0 * 512.0) < 0.10);
        REQUIRE(uncoveredBottom * 10 >= uncovered * 9);
    }

    SECTION("exposure +1 stop doubles linear output") {
        geom::VirtualCamera cam;
        cam.w = 160;
        cam.h = 90;
        cam.hfovDeg = 90;
        const OsvColorParams lin0 = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::Linear, 0.0f);
        const OsvColorParams lin1 = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::Linear, 1.0f);
        auto j0 = render::RenderParamsBuilder().rig(sp.value().rig).camera(cam).color(lin0).build(sp.value().pair);
        auto j1 = render::RenderParamsBuilder().rig(sp.value().rig).camera(cam).color(lin1).build(sp.value().pair);
        REQUIRE(j0.ok());
        REQUIRE(j1.ok());
        const double m0 = meanLuma(cpu.render(j0.value()).value());
        const double m1 = meanLuma(cpu.render(j1.value()).value());
        REQUIRE(m0 > 0.0);
        REQUIRE_THAT(m1 / m0, Catch::Matchers::WithinRel(2.0, 0.01));
    }
}

// -----------------------------------------------------------------------------
//  Backend parity
// -----------------------------------------------------------------------------
namespace {

void checkParity(render::IRenderer& gpu, const char* label) {
    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    auto sp = openSampleFrame(0);
    REQUIRE(sp.ok());
    const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::PQ, 0.0f);

    geom::VirtualCamera cam;
    cam.w = 1920;
    cam.h = 1080;
    cam.hfovDeg = 110;
    cam.yawDeg = 35;
    cam.pitchDeg = -10;
    auto jobA = render::RenderParamsBuilder().rig(sp.value().rig).camera(cam).color(cp).build(sp.value().pair);
    REQUIRE(jobA.ok());
    geom::EquirectMap map;
    map.w = 2048;
    map.h = 1024;
    auto jobB = render::RenderParamsBuilder().rig(sp.value().rig).equirect(map).color(cp).build(sp.value().pair);
    REQUIRE(jobB.ok());
    // Eye-offset projection at d = 0.5: proves the GPU kernels carry the new
    // OSV_PROJ_EYE_OFFSET branch (asinf / acosf included) and not only the
    // rectilinear and equirect paths.
    geom::VirtualCamera eye;
    eye.projection = geom::Projection::EyeOffset;
    eye.eyeOffset = 0.5;
    eye.w = 1920;
    eye.h = 1080;
    eye.hfovDeg = 150;
    eye.yawDeg = -20;
    eye.pitchDeg = 15;
    eye.rollDeg = 5;
    auto jobC = render::RenderParamsBuilder().rig(sp.value().rig).camera(eye).color(cp).build(sp.value().pair);
    REQUIRE(jobC.ok());
    REQUIRE(jobC.value().params.projection == OSV_PROJ_EYE_OFFSET);

    // [WP-SEAM] The carved blend seam: a wavy seam (+/-2.5 degrees) with a
    // feather that varies from 0.3 to 1.2 degrees, so the lookup's
    // interpolation, the wrap, the narrow and the wide feather and the
    // occlusion fill (the sample rig carries the stick polygon) all run -
    // once in a view straight across the seam, once over the whole
    // polar-axis band.
    constexpr std::uint32_t kSeamColumns = 512;
    std::vector<float> blendSeamTable(kSeamColumns * 2u);
    for (std::uint32_t c = 0; c < kSeamColumns; ++c) {
        const double t = osv::kTwoPi * static_cast<double>(c) / kSeamColumns;
        blendSeamTable[c * 2u] = static_cast<float>(deg2rad(2.5 * std::sin(3.0 * t)));
        blendSeamTable[c * 2u + 1u] = static_cast<float>(deg2rad(0.3 + 0.9 * (0.5 + 0.5 * std::sin(5.0 * t))));
    }
    geom::VirtualCamera across;
    across.w = 1920;
    across.h = 1080;
    across.hfovDeg = 100;
    across.yawDeg = 90;  // the seam runs through the middle of the view
    auto jobD = render::RenderParamsBuilder()
                    .rig(sp.value().rig)
                    .camera(across)
                    .color(cp)
                    .blendSeam(blendSeamTable, kSeamColumns, static_cast<float>(deg2rad(0.5)))
                    .build(sp.value().pair);
    REQUIRE(jobD.ok());
    REQUIRE(jobD.value().params.blendSeamEnabled == 1);
    geom::EquirectMap polar;
    polar.layout = geom::EquirectLayout::PolarAxis;
    polar.w = 2048;
    polar.h = 1024;
    auto jobE = render::RenderParamsBuilder()
                    .rig(sp.value().rig)
                    .equirect(polar)
                    .color(cp)
                    .blendSeam(blendSeamTable, kSeamColumns, static_cast<float>(deg2rad(0.5)))
                    .build(sp.value().pair);
    REQUIRE(jobE.ok());
    REQUIRE(jobE.value().params.blendSeamEnabled == 1);

    for (const render::RenderJob* job :
         {&jobA.value(), &jobB.value(), &jobC.value(), &jobD.value(), &jobE.value()}) {
        auto ref = cpu.render(*job);
        auto test = gpu.render(*job);
        REQUIRE(ref.ok());
        REQUIRE(test.ok());
        const render::ImageDiffStats stats = render::compareImages16(ref.value(), test.value());
        // Locate the worst pixels so a regression is diagnosable from the log.
        std::string worst;
        {
            const render::ImageRGBAf& a = ref.value();
            const render::ImageRGBAf& b = test.value();
            std::vector<std::pair<float, std::size_t>> diffs;
            for (std::size_t i = 0; i < a.data.size(); i += 4) {
                float d = 0.0f;
                for (int c = 0; c < 4; ++c) {
                    d = std::max(d, std::fabs(a.data[i + c] - b.data[i + c]));
                }
                if (d > 4.0f / 65535.0f) {
                    diffs.emplace_back(d, i / 4);
                }
            }
            std::sort(diffs.rbegin(), diffs.rend());
            for (std::size_t k = 0; k < std::min<std::size_t>(diffs.size(), 6); ++k) {
                const std::size_t px = diffs[k].second;
                const float* pa = a.data.data() + px * 4;
                const float* pb = b.data.data() + px * 4;
                worst += " [" + std::to_string(px % a.w) + "," + std::to_string(px / a.w) + "] cpu(" +
                         std::to_string(pa[0]) + "," + std::to_string(pa[1]) + "," + std::to_string(pa[2]) + "," +
                         std::to_string(pa[3]) + ") gpu(" + std::to_string(pb[0]) + "," + std::to_string(pb[1]) + "," +
                         std::to_string(pb[2]) + "," + std::to_string(pb[3]) + ")";
            }
            worst += " (" + std::to_string(diffs.size()) + " pixels differ by > 4 codes)";
        }
        INFO(label << ": PSNR " << stats.psnrDb << " dB, max diff " << stats.maxAbsCode << " codes, within2 "
                   << stats.fractionWithin2 << worst);
        REQUIRE(stats.psnrDb >= 60.0);
        REQUIRE(stats.fractionWithin2 >= 0.9995);
        REQUIRE(stats.maxAbsCode <= 64);
    }
}

}  // namespace

#if defined(OSV_HAVE_CUDA)
TEST_CASE("CUDA renderer matches the CPU reference", "[render][sample][cuda]") {
    OSV_REQUIRE_SAMPLE();
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto r = render::CudaRenderer::create(0);
    REQUIRE(r.ok());
    checkParity(*r.value(), "cuda");
}

TEST_CASE("a zero-copy NVDEC frame renders like the software-decoded one", "[render][sample][cuda][hwaccel]") {
    // End to end through the path the direct-GPU pipeline is built on: NVDEC
    // decodes into device memory (keepOnDevice), the builder describes those
    // surfaces in place, and the CUDA renderer samples them with no host
    // copy.  HEVC decoding is bit-exact by specification, so the only
    // differences allowed are the renderer's own GPU-vs-GPU nothing: the
    // same kernel on the same device, fed P010 instead of yuv420p10.
    OSV_REQUIRE_SAMPLE();
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto sp = openSampleFrame(7);  // not a keyframe: the decoder has to run forward
    REQUIRE(sp.ok());

    video::DecoderOptions opt;
    opt.hw = video::HwAccel::Cuda;
    opt.keepOnDevice = true;
    auto reader = video::DualStreamReader::open(osvtest::sampleOsv(), sp.value().format, opt);
    if (!reader.ok()) {
        SKIP("NVDEC unavailable: " << reader.error().message);
    }
    auto devicePair = reader.value().read(7);
    REQUIRE(devicePair.ok());
    REQUIRE(devicePair.value().onDevice());

    const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::PQ, 0.0f);
    geom::VirtualCamera cam;
    cam.w = 1920;
    cam.h = 1080;
    cam.hfovDeg = 100;
    cam.yawDeg = 80;  // looks across the seam
    auto hostJob = render::RenderParamsBuilder().rig(sp.value().rig).camera(cam).color(cp).build(sp.value().pair);
    auto devJob = render::RenderParamsBuilder().rig(sp.value().rig).camera(cam).color(cp).build(devicePair.value());
    REQUIRE(hostJob.ok());
    REQUIRE(devJob.ok());
    REQUIRE(devJob.value().planesOnDevice[0]);
    REQUIRE(devJob.value().planesOnDevice[1]);

    auto gpu = render::CudaRenderer::create(0);
    REQUIRE(gpu.ok());
    auto a = gpu.value()->render(hostJob.value());
    auto b = gpu.value()->render(devJob.value());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    const render::ImageDiffStats stats = render::compareImages16(a.value(), b.value());
    INFO("host-decoded vs NVDEC zero-copy: PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode << " codes");
    CHECK(stats.maxAbsCode <= 1);
}
#endif

#if defined(OSV_HAVE_OPENCL)
TEST_CASE("OpenCL renderer matches the CPU reference", "[render][sample][opencl]") {
    OSV_REQUIRE_SAMPLE();
    std::string reason;
    if (!render::OpenClRenderer::available(&reason)) {
        SKIP("OpenCL unavailable: " << reason);
    }
    auto r = render::OpenClRenderer::create(0);
    if (!r.ok()) {
        FAIL("OpenCL renderer creation failed: " << r.error().message);
    }
    checkParity(*r.value(), "opencl");
}
#endif

#if defined(OSV_HAVE_METAL)
TEST_CASE("Metal renderer matches the CPU reference", "[render][sample][metal]") {
    OSV_REQUIRE_SAMPLE();
    std::string reason;
    if (!render::MetalRenderer::available(&reason)) {
        SKIP("Metal unavailable: " << reason);
    }
    auto r = render::MetalRenderer::create(0);
    if (!r.ok()) {
        FAIL("Metal renderer creation failed: " << r.error().message);
    }
    checkParity(*r.value(), "metal");
}
#endif

TEST_CASE("makeRenderer resolves names", "[render]") {
    ThreadPool pool(2);
    std::string chosen;
    auto cpu = render::makeRenderer("cpu", pool, &chosen);
    REQUIRE(cpu.ok());
    REQUIRE(chosen == "cpu");
    auto autoR = render::makeRenderer("auto", pool, &chosen);
    REQUIRE(autoR.ok());
    REQUIRE_FALSE(chosen.empty());
    REQUIRE(render::makeRenderer("vulkan", pool, nullptr).error().code == ErrorCode::InvalidArgument);
}

// ===========================================================================
//  The LRF proxy: one side-by-side track, two lenses
//
//  The .LRF proxy DJI writes beside every .OSV is a single 2048 x 1024 track
//  holding two 1024 x 1024 fisheye halves, not two tracks of one lens each.
//  video::DualStreamReader has always split it correctly, but everything
//  downstream was built from FormatInfo::streamW - the WHOLE TRACK - so the
//  rig described a 2048 x 1024 lens while the reader handed out 1024 x 1024
//  frames, and render::RenderParamsBuilder rejected every frame with
//  "frame size does not match the rig (1024x1024 vs 2048x1024)".  The proxy
//  therefore opened, reported its geometry correctly and rendered nothing.
//
//  FormatInfo::lensW() / lensH() are the fix: one named per-lens size that
//  every rig builder uses, halved for the side-by-side layout and identical
//  to streamW / streamH for every other mode.
// ===========================================================================
namespace {

/// Everything the LRF needs to reach a rendered frame, built the way the
/// importer and osvtool both build it.
struct LrfPipeline {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::StreamScaling scaling;
    geom::LensRig rig;
    video::FramePair pair;
};

Result<LrfPipeline> openLrfFrame(std::uint32_t frameIndex) {
    LrfPipeline s;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(osvtest::sampleLrf()));
    s.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(s.track, meta::MetadataTrack::load(*s.file));
    OSV_TRY_ASSIGN(s.format, meta::FormatDetector::detect(*s.file, &s.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(s.track.stream()));
    // lensW()/lensH(), exactly as ImporterInstance::rebuildRig() and
    // tools/osvtool/Pipeline.cpp do it.
    OSV_TRY_ASSIGN(s.scaling,
                   geom::StreamScaling::derive(static_cast<int>(s.format.lensW()), static_cast<int>(s.format.lensH()),
                                               static_cast<int>(s.format.sensorW), static_cast<int>(s.format.sensorH),
                                               s.format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(s.rig, geom::LensRig::build(cal, s.scaling, geom::FocalSource::DigitalFocalLength,
                                               s.format.digitalFocalLength, geom::ExtrinsicConvention{}));
    OSV_TRY_ASSIGN(video::DualStreamReader reader, video::DualStreamReader::open(osvtest::sampleLrf(), s.format));
    OSV_TRY_ASSIGN(s.pair, reader.read(frameIndex));
    return s;
}

}  // namespace

TEST_CASE("FormatInfo::lensW halves only the side-by-side layout", "[meta][lrf]") {
    // A pure unit check, no clip needed: the accessor is the single place the
    // side-by-side split is expressed, so it is worth pinning on its own.
    meta::FormatInfo f;
    f.streamW = 3000;
    f.streamH = 3000;
    f.sideBySideProxy = false;
    CHECK(f.lensW() == 3000u);
    CHECK(f.lensH() == 3000u);

    f.streamW = 2048;
    f.streamH = 1024;
    f.sideBySideProxy = true;
    CHECK(f.lensW() == 1024u);
    CHECK(f.lensH() == 1024u * 0u + 1024u);

    // Defensive: an odd or degenerate width is passed through rather than
    // silently truncated, so a malformed clip fails in the rig builder (which
    // reports what it saw) instead of being reinterpreted here.
    f.streamW = 2049;
    CHECK(f.lensW() == 2049u);
    f.streamW = 1;
    CHECK(f.lensW() == 1u);
    f.streamW = 0;
    CHECK(f.lensW() == 0u);
}

TEST_CASE("the LRF proxy builds a rig that matches its decoded halves", "[render][lrf][sample]") {
    OSV_REQUIRE_SAMPLE_LRF();
    auto lp = openLrfFrame(0);
    REQUIRE(lp.ok());
    const LrfPipeline& s = lp.value();

    // The track is 2048 x 1024...
    REQUIRE(s.format.sideBySideProxy);
    REQUIRE(s.format.streamW == 2048u);
    REQUIRE(s.format.streamH == 1024u);
    // ...and one lens is 1024 x 1024.
    REQUIRE(s.format.lensW() == 1024u);
    REQUIRE(s.format.lensH() == 1024u);

    // The reader splits the track into two halves of exactly that size.
    REQUIRE(s.pair.lens[0].width == 1024u);
    REQUIRE(s.pair.lens[0].height == 1024u);
    REQUIRE(s.pair.lens[1].width == 1024u);
    REQUIRE(s.pair.lens[1].height == 1024u);

    // And the rig now describes the SAME size.  This is the assertion that
    // was false before the fix - the rig said 2048 x 1024 - and it is the one
    // RenderParamsBuilder checks per frame.
    REQUIRE(s.rig.streamW == 1024);
    REQUIRE(s.rig.streamH == 1024);

    // The two halves are different pictures, not the same one twice: a split
    // that forgot to advance the pointer for the right half would pass every
    // size check above and still be wrong.
    bool differs = false;
    for (std::uint32_t y = 0; y < 1024u && !differs; y += 64u) {
        for (std::uint32_t x = 0; x < 1024u; x += 64u) {
            const std::uint16_t a = s.pair.lens[0].luma(x, y);
            const std::uint16_t b = s.pair.lens[1].luma(x, y);
            if (a != b) {
                differs = true;
                break;
            }
        }
    }
    CHECK(differs);
}

TEST_CASE("the LRF proxy renders an equirect frame", "[render][lrf][sample]") {
    OSV_REQUIRE_SAMPLE_LRF();
    auto lp = openLrfFrame(0);
    REQUIRE(lp.ok());
    const LrfPipeline& s = lp.value();

    ThreadPool pool;
    render::CpuRenderer cpu(pool);
    const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::PQ, 0.0f);

    // The proxy's native equirect is 2 x the lens height, i.e. 2048 x 1024 -
    // the same number as the track width, which is a coincidence of this
    // layout and exactly the sort of thing that hid the bug.
    geom::VirtualCamera cam;
    cam.projection = geom::Projection::Equirect;
    cam.w = 2048;
    cam.h = 1024;
    auto job = render::RenderParamsBuilder().rig(s.rig).camera(cam).color(cp).build(s.pair);
    REQUIRE(job.ok());  // this is the call that used to fail

    auto img = cpu.render(job.value());
    REQUIRE(img.ok());
    REQUIRE(allFinite(img.value()));

    // Coverage: with a correct rig the two 195-degree lenses fill the sphere,
    // so the overwhelming majority of the frame is opaque.  A rig built from
    // the whole track would have projected each lens into a small disc and
    // left most of the panorama transparent, which is what the wrong-focal
    // variant of this bug looked like on screen.
    std::size_t opaque = 0;
    std::size_t total = 0;
    for (std::uint32_t y = 0; y < 1024u; y += 4u) {
        for (std::uint32_t x = 0; x < 2048u; x += 4u) {
            const float* px = img.value().pixel(x, y);
            if (px[3] > 0.5f) {
                ++opaque;
            }
            ++total;
        }
    }
    REQUIRE(total > 0u);
    const double coverage = static_cast<double>(opaque) / static_cast<double>(total);
    INFO("LRF equirect alpha coverage: " << coverage);
    CHECK(coverage > 0.90);
}

TEST_CASE("the LRF's stale digital_focal_length does not reach the lens", "[render][lrf][sample]") {
    OSV_REQUIRE_SAMPLE_LRF();
    auto lp = openLrfFrame(0);
    REQUIRE(lp.ok());
    const LrfPipeline& s = lp.value();

    // The proxy's ClipMeta repeats the 6K camera's digital_focal_length
    // verbatim - it is the value for a 3000 px stream, not for a 1024 px one.
    // Taken at face value it builds a lens nearly three times too long and
    // the fisheye circle collapses to a disc in the middle of the panorama.
    //
    // LensRig::build() now checks the field against the calibration mapped
    // through the same scaling and refuses it when the two disagree by more
    // than a factor of 1.2, so the lens ends up at the calibrated focal.
    const double stale = static_cast<double>(s.format.digitalFocalLength);
    const double actual = s.rig.lens[geom::kSlaveLens].fx;
    INFO("digital_focal_length " << stale << " vs lens fx " << actual);
    CHECK(stale > 800.0);           // the 6K value really is in the file
    CHECK(actual < stale / 2.0);    // and it really was rejected
    CHECK(actual > 200.0);          // for something physically sensible
    CHECK(actual < 400.0);

    // The rejection is recorded, not silent: a human reading the notes can
    // see which number was used and why.
    bool noted = false;
    for (const std::string& n : s.rig.notes) {
        if (n.find("digital_focal_length") != std::string::npos && n.find("disagrees") != std::string::npos) {
            noted = true;
            break;
        }
    }
    CHECK(noted);
}

TEST_CASE("the 6K clip still trusts its digital_focal_length", "[render][lrf][sample]") {
    // The other side of the same guard: on the verified 6K clip the field and
    // the scaled calibration agree to a fraction of a percent, so the field
    // must still win and the rig must be bit-identical to what it always was.
    OSV_REQUIRE_SAMPLE();
    auto sp = openSampleFrame(0);
    REQUIRE(sp.ok());
    const double dfl = static_cast<double>(sp.value().format.digitalFocalLength);
    CHECK(sp.value().rig.lens[geom::kSlaveLens].fx == dfl);
    CHECK(sp.value().rig.lens[geom::kMasterLens].fx == dfl);
}
