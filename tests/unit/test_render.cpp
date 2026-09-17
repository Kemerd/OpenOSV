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

    for (const render::RenderJob* job : {&jobA.value(), &jobB.value()}) {
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
