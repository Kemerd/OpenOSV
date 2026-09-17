// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osvtool selfcheck: a quick health check a user can run on any machine.
// Without a clip it verifies the maths; with a clip it also decodes, checks
// the seam alignment and compares the GPU backends with the CPU reference.

#include "Commands.h"
#include "Pipeline.h"

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"
#include "osv/color/Cube.h"
#include "osv/color/DlogM.h"
#include "osv/core/Log.h"
#include "osv/geom/KannalaBrandt5.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif
#if defined(OSV_HAVE_OPENCL)
#include "osv/render/OpenClRenderer.h"
#endif

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>

namespace osvtool {

using namespace osv;

namespace {

struct SelfcheckOptions {
    PipelineOptions pipeline;
    bool haveFile = false;
};

int g_failures = 0;

void report(const char* name, bool pass, const std::string& detail) {
    std::printf("[%s] %-32s %s\n", pass ? "PASS" : "FAIL", name, log::safe(detail).c_str());
    if (!pass) {
        ++g_failures;
    }
}

void checkMath() {
    // Colour anchors.
    const float hlgHalf = osvHlgOetf(1.0f / 12.0f);
    report("HLG OETF(1/12) == 0.5", std::fabs(hlgHalf - 0.5f) < 1e-5f, std::to_string(hlgHalf));
    const float pq203 = osvPqInverseEotf(203.0f);
    report("PQ(203 nit) == 0.5807", std::fabs(pq203 - 0.5807f) < 5e-4f, std::to_string(pq203));
    const float grey = color::dlogmToLinear(color::kDlogMPocket3, 0.40f);
    report("Pocket3 code 0.40 -> 0.18", std::fabs(grey - 0.18f) < 2e-3f, std::to_string(grey));
    const OsvColorParams hlg = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::HLG, 0.0f);
    float codes[3] = {0.40f, 0.40f, 0.40f}, lin[3], out[3];
    osvCodeToLinear(&hlg, codes, lin);
    osvLinearToOutput(&hlg, lin, out);
    report("DJI refit code 0.40 -> HLG 0.38", std::fabs(out[1] - 0.380f) < 0.012f, std::to_string(out[1]));

    // Lens round trip with the sample calibration values.
    geom::KannalaBrandt5 kb;
    kb.fx = 1043.8802;
    kb.fy = 1043.6731;
    kb.cx = 1917.0421;
    kb.cy = 1919.1294;
    kb.k = {0.0667397, -0.0128859, 0.0103815, -0.00677581, 0.00098791};
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> ang(0.0, 6.283185307), rad(0.0, 1800.0);
    double worst = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const double a = ang(rng), r = rad(rng);
        const Vec2d px{kb.cx + r * std::cos(a), kb.cy + r * std::sin(a)};
        auto dir = kb.unproject(px);
        if (!dir.ok()) {
            worst = 1e9;
            break;
        }
        Vec2d back;
        double theta = 0.0;
        if (!kb.project(dir.value(), back, theta)) {
            worst = 1e9;
            break;
        }
        worst = std::max(worst, (back - px).norm());
    }
    report("lens project/unproject < 1e-4 px", worst < 1e-4, std::to_string(worst));

    // LUT round trip.
    const std::filesystem::path lut = std::filesystem::temp_directory_path() / "osv_selfcheck.cube";
    color::CubeOptions co;
    co.size = 33;
    const bool wrote = color::writeCube(lut, hlg, co).ok();
    auto read = color::readCube(lut);
    double maxErr = 0.0;
    if (wrote && read.ok()) {
        // Mildly saturated inputs only: strongly out-of-gamut corners sit on
        // the OETF singularity where trilinear interpolation cannot be exact.
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        std::uniform_real_distribution<float> sat(0.6f, 1.0f);
        for (int i = 0; i < 500; ++i) {
            const float base = u(rng);
            float in[3] = {base * sat(rng), base * sat(rng), base * sat(rng)}, direct[3], sampled[3];
            float l[3];
            osvCodeToLinear(&hlg, in, l);
            osvLinearToOutput(&hlg, l, direct);
            read.value().sample(in, sampled);
            for (int c = 0; c < 3; ++c) {
                maxErr = std::max(maxErr, static_cast<double>(std::fabs(direct[c] - sampled[c])));
            }
        }
    }
    report("cube write/read (33^3) < 0.03", wrote && read.ok() && maxErr < 0.03, std::to_string(maxErr));
    std::error_code ec;
    std::filesystem::remove(lut, ec);
}

void checkClip(const PipelineOptions& po) {
    PipelineOptions opt = po;
    opt.device = "cpu";
    auto pipe = Pipeline::open(opt, true);
    report("open clip", pipe.ok(), pipe.ok() ? pipe.value()->format.cameraModel : pipe.error().toString());
    if (!pipe.ok()) {
        return;
    }
    Pipeline& P = *pipe.value();
    report("metadata: color mode", P.format.colorModeFromMetadata, std::string(meta::colorModeName(P.format.colorMode)));
    report("metadata: calibration", true, P.calibration.sourceSlave + " / " + P.calibration.sourceMaster);

    auto pair = P.reader->read(0);
    report("decode frame 0", pair.ok(),
           pair.ok() ? std::to_string(pair.value().lens[0].width) + "x" + std::to_string(pair.value().lens[0].height)
                     : pair.error().toString());
    if (!pair.ok()) {
        return;
    }

    render::BandParams band;
    band.bandHalfDeg = 4.0;
    auto ncc = render::overlapNcc(P.rig, pair.value(), P.blendParams, band, *P.pool);
    report("seam overlap NCC >= 0.8", ncc.ok() && ncc.value() >= 0.8, ncc.ok() ? std::to_string(ncc.value()) : ncc.error().toString());

    // GPU parity.
    geom::VirtualCamera cam;
    cam.w = 1280;
    cam.h = 720;
    cam.hfovDeg = 100;
    auto job = render::RenderParamsBuilder().rig(P.rig).camera(cam).color(P.color).blend(P.blendParams).build(pair.value());
    if (!job.ok()) {
        report("build render job", false, job.error().toString());
        return;
    }
    render::CpuRenderer cpu(*P.pool);
    auto ref = cpu.render(job.value());
    report("cpu render", ref.ok(), ref.ok() ? "ok" : ref.error().toString());
    if (!ref.ok()) {
        return;
    }
#if defined(OSV_HAVE_CUDA)
    {
        std::string reason;
        if (render::CudaRenderer::available(&reason)) {
            auto r = render::CudaRenderer::create(0);
            if (r.ok()) {
                auto img = r.value()->render(job.value());
                if (img.ok()) {
                    const auto stats = render::compareImages16(ref.value(), img.value());
                    report("cuda parity PSNR >= 60 dB", stats.psnrDb >= 60.0, std::to_string(stats.psnrDb) + " dB");
                } else {
                    report("cuda render", false, img.error().toString());
                }
            } else {
                report("cuda create", false, r.error().toString());
            }
        } else {
            std::printf("[SKIP] cuda: %s\n", log::safe(reason).c_str());
        }
    }
#endif
#if defined(OSV_HAVE_OPENCL)
    {
        std::string reason;
        if (render::OpenClRenderer::available(&reason)) {
            auto r = render::OpenClRenderer::create(0);
            if (r.ok()) {
                auto img = r.value()->render(job.value());
                if (img.ok()) {
                    const auto stats = render::compareImages16(ref.value(), img.value());
                    report("opencl parity PSNR >= 60 dB", stats.psnrDb >= 60.0, std::to_string(stats.psnrDb) + " dB");
                } else {
                    report("opencl render", false, img.error().toString());
                }
            } else {
                report("opencl create", false, r.error().toString());
            }
        } else {
            std::printf("[SKIP] opencl: %s\n", log::safe(reason).c_str());
        }
    }
#endif
}

int runSelfcheck(const SelfcheckOptions& o) {
    g_failures = 0;
    checkMath();
    if (o.haveFile) {
        checkClip(o.pipeline);
    }
    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? kExitOk : kExitRuntime;
}

}  // namespace

void registerSelfcheckCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<SelfcheckOptions>();
    CLI::App* sub = app.add_subcommand("selfcheck", "Verify maths, decoding, seam alignment and GPU parity");
    sub->add_option("file", opt->pipeline.input, "Optional .OSV clip to test with")->each([opt](const std::string&) {
        opt->haveFile = true;
    });
    sub->callback([opt, &ctx]() { ctx.exitCode = runSelfcheck(*opt); });
}

}  // namespace osvtool
