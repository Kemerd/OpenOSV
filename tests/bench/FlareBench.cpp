// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlareBench.cpp - what the flare removal finds, removes, damages and costs
// on a real clip (WP-FLARE; docs/research/FLARE.md quotes its output).
//
// A measurement tool, not a test: it asserts nothing and is NOT registered
// with ctest, because its numbers depend on the machine and the footage.
//
//     build\<dir>\bin\osv_flare_bench.exe [clip.OSV] [--out DIR] [--reps N]
//                                         [--sweep] [--verbose]
//
// WHAT IT DOES
// ------------
//   1. Analysis (frames 0, 32, 64): the fitted model, and the time of one
//      analyseFlare() call on the CPU path (software-decoded host frames,
//      CPU sampler) and on the GPU path (NVDEC frames left in VRAM, CUDA
//      sampler) - median of --reps runs after two warm-ups.
//   2. Veil (research only - see the LEGAL note on estimateVeil): the
//      overlap estimate, and the sky step across the seam before / after.
//      --sweep adds every frame of the clip: how often the brightest ghost
//      is found and how much its fit moves from frame to frame, raw and
//      through smoothFlare.
//   3. Removal (frame 0): renders a view centred on the sun and a close-up
//      of the brightest ghost with the removal off and on, and measures per
//      ghost how visible it is against a quadratic surface fitted to a ring
//      of clean sky around it (scene-linear), plus every pixel changed
//      OUTSIDE the fitted ghosts (must be zero) and on the sun disc (must be
//      zero).
//   4. Kernel cost: CUDA render time with the removal off and on, measured
//      back to back and alternated so machine load hits both equally.
//
// --verbose logs every candidate the gates refuse, and why.
//
// With --out, the views are written as 16-bit TIFFs (Rec.709) for the
// crops in research/flare/.  Output is plain ASCII.

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/io/ImageWriter.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CpuRenderer.h"
#include "osv/render/CudaRenderer.h"
#include "osv/render/Flare.h"
#include "osv/render/FlareCuda.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace osv;

namespace {

using Clock = std::chrono::steady_clock;

/// Median of `reps` timed runs of `body` (ms) after two warm-ups; negative
/// when a run fails.
double medianMs(int reps, const std::function<Status()>& body) {
    for (int i = 0; i < 2; ++i) {
        if (!body().ok()) {
            return -1.0;
        }
    }
    std::vector<double> ms;
    for (int i = 0; i < reps; ++i) {
        const auto t0 = Clock::now();
        if (!body().ok()) {
            return -1.0;
        }
        ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

/// Clip, rig and format.
struct Clip {
    std::unique_ptr<OsvFile> file;
    meta::MetadataTrack track;
    meta::FormatInfo format;
    geom::LensRig rig;
};

Result<Clip> openClip(const std::filesystem::path& path) {
    Clip c;
    OSV_TRY_ASSIGN(OsvFile f, OsvFile::open(path));
    c.file = std::make_unique<OsvFile>(std::move(f));
    OSV_TRY_ASSIGN(c.track, meta::MetadataTrack::load(*c.file));
    OSV_TRY_ASSIGN(c.format, meta::FormatDetector::detect(*c.file, &c.track));
    OSV_TRY_ASSIGN(meta::CalibrationSet cal, meta::CalibrationSelector::select(c.track.stream()));
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   geom::StreamScaling::derive(static_cast<int>(c.format.streamW), static_cast<int>(c.format.streamH),
                                               static_cast<int>(c.format.sensorW), static_cast<int>(c.format.sensorH),
                                               c.format.digitalFocalLength, 0.5 * (cal.slave.fx + cal.master.fx)));
    OSV_TRY_ASSIGN(c.rig, geom::LensRig::build(cal, scaling, geom::FocalSource::DigitalFocalLength,
                                               c.format.digitalFocalLength, geom::ExtrinsicConvention{}));
    return c;
}

/// Print one lens's model.
void printLens(const char* name, const render::LensFlare& lf) {
    if (!lf.sunFound) {
        std::printf("    %s: no sun\n", name);
        return;
    }
    std::printf("    %s: sun (%.1f, %.1f) r %.1f px, %.2f deg off axis; %u candidates, %zu ghosts kept\n", name,
                lf.sunX, lf.sunY, lf.sunRadiusPx, lf.sunThetaRad * 180.0 / kPi, lf.candidates, lf.ghosts.size());
    for (const render::FlareGhost& g : lf.ghosts) {
        std::printf("      ghost c(%.1f, %.1f) half(%.1f x %.1f) corner %.1f angle %.1f deg soft %.1f  "
                    "amp(%.4f %.4f %.4f)  contrast %.3f  R2 %.2f\n",
                    g.cx, g.cy, g.hx, g.hy, g.radius, g.angleRad * 180.0 / kPi, g.soft, g.amp[0], g.amp[1], g.amp[2],
                    g.contrast, g.fitR2);
    }
    if (lf.veil[0] > 0.0) {
        std::printf("      veil %.4f\n", lf.veil[0]);
    }
}

/// Body direction of master-lens pixel (px, py).
Vec3d masterPixelDir(const geom::LensRig& rig, double px, double py) {
    auto d = rig.lens[1].unproject(Vec2d{px, py});
    if (!d.ok()) {
        return Vec3d{0.0, 1.0, 0.0};
    }
    return rig.bodyToLens[1].transposed() * d.value();
}

/// A virtual camera looking at body direction `d`.
geom::VirtualCamera lookAt(const Vec3d& d, double hfov, int w, int h) {
    geom::VirtualCamera cam;
    cam.w = w;
    cam.h = h;
    cam.hfovDeg = hfov;
    // rotation() = Rz(yaw) Rx(pitch): forward (0,1,0) -> (-cos p sin y, cos p cos y, sin p).
    cam.yawDeg = std::atan2(-d.x, d.y) * 180.0 / kPi;
    cam.pitchDeg = std::asin(std::clamp(d.z, -1.0, 1.0)) * 180.0 / kPi;
    return cam;
}

/// Per output pixel: the master-lens pixel it samples and each ghost's
/// plateau weight there - the geometry every metric below is taken in.
struct GhostMap {
    std::uint32_t w = 0, h = 0;
    std::vector<float> lx, ly;                  ///< Master-lens pixel (NaN when unseen).
    std::vector<std::vector<float>> weight;     ///< [ghost][pixel].
};

GhostMap ghostMap(const OsvRenderParams& p) {
    GhostMap m;
    m.w = static_cast<std::uint32_t>(p.outW);
    m.h = static_cast<std::uint32_t>(p.outH);
    const std::size_t n = static_cast<std::size_t>(m.w) * m.h;
    m.lx.assign(n, std::nanf(""));
    m.ly.assign(n, std::nanf(""));
    const int count = p.flare[1].ghostCount;
    m.weight.assign(static_cast<std::size_t>(count), std::vector<float>(n, 0.0f));
    for (std::uint32_t y = 0; y < m.h; ++y) {
        for (std::uint32_t x = 0; x < m.w; ++x) {
            float dv[3];
            if (!osvRayForPixel(&p, static_cast<float>(x), static_cast<float>(y), dv)) {
                continue;
            }
            float db[3];
            osvMat3MulVec(p.Rout, dv, db);
            float px = 0.0f, py = 0.0f, th = 0.0f;
            if (!osvProjectLens(&p.lens[1], db, &px, &py, &th)) {
                continue;
            }
            const std::size_t i = static_cast<std::size_t>(y) * m.w + x;
            m.lx[i] = px;
            m.ly[i] = py;
            for (int k = 0; k < count; ++k) {
                m.weight[static_cast<std::size_t>(k)][i] = osvFlareGhostShape(&p.flare[1].ghost[k], px, py);
            }
        }
    }
    return m;
}

double lumaAt(const render::ImageRGBAf& img, std::size_t i) {
    const float* p = img.data.data() + i * 4u;
    return 0.2627 * p[0] + 0.6780 * p[1] + 0.0593 * p[2];
}

/// Solve the n x n SPD system A x = b by Cholesky (n <= 6), false if singular.
bool cholSolve(double* A, double* b, int n) {
    for (int j = 0; j < n; ++j) {
        double d = A[j * n + j];
        for (int k = 0; k < j; ++k) {
            d -= A[j * n + k] * A[j * n + k];
        }
        if (!(d > 0.0)) {
            return false;
        }
        d = std::sqrt(d);
        A[j * n + j] = d;
        for (int i = j + 1; i < n; ++i) {
            double v = A[i * n + j];
            for (int k = 0; k < j; ++k) {
                v -= A[i * n + k] * A[j * n + k];
            }
            A[i * n + j] = v / d;
        }
    }
    for (int i = 0; i < n; ++i) {
        double v = b[i];
        for (int k = 0; k < i; ++k) {
            v -= A[i * n + k] * b[k];
        }
        b[i] = v / A[i * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double v = b[i];
        for (int k = i + 1; k < n; ++k) {
            v -= A[k * n + i] * b[k];
        }
        b[i] = v / A[i * n + i];
    }
    return true;
}

/// How visible ghost k still is in `img`.
///
/// A quadratic surface is fitted (in output pixels) to a ring of clean sky
/// around the ghost - 1.15 to 1.8 times its reach, no ghost, clear of the sun
/// - and interpolated under it, which follows the steep glow gradient near
/// the sun that a plain ring mean would bias.  Reported against that
/// surface: the plateau's mean excess (contrast) and the RMS deviation over
/// the whole footprint, rim included (both relative to the surface).
struct Visibility {
    double contrast = std::nan("");
    double footprintRms = std::nan("");
    std::size_t plateauPx = 0;
};

Visibility ghostVisibility(const render::ImageRGBAf& img, const GhostMap& gm, const render::LensFlare& lf,
                           std::size_t k) {
    Visibility vis;
    const render::FlareGhost& g = lf.ghosts[k];
    const double reach = g.reach();
    // Normalisation of the surface coordinates: the view's own size.
    const double sx = 0.5 * gm.w;
    const double sy = 0.5 * gm.h;
    double A[36] = {};
    double b[6] = {};
    std::size_t nr = 0;
    auto basis = [&](std::size_t i, double* t) {
        const double x = (static_cast<double>(i % gm.w) + 0.5) / sx - 1.0;
        const double y = (static_cast<double>(i / gm.w) + 0.5) / sy - 1.0;
        t[0] = 1.0;
        t[1] = x;
        t[2] = y;
        t[3] = x * x;
        t[4] = x * y;
        t[5] = y * y;
    };
    for (std::size_t i = 0; i < gm.lx.size(); ++i) {
        if (!std::isfinite(gm.lx[i])) {
            continue;
        }
        const double rho = std::hypot(gm.lx[i] - g.cx, gm.ly[i] - g.cy) / reach;
        if (rho < 1.15 || rho > 1.8) {
            continue;
        }
        bool clean = true;
        for (const auto& wv : gm.weight) {
            clean = clean && wv[i] == 0.0f;
        }
        if (lf.sunFound && std::hypot(gm.lx[i] - lf.sunX, gm.ly[i] - lf.sunY) < 3.0 * lf.sunRadiusPx) {
            clean = false;
        }
        if (!clean) {
            continue;
        }
        double t[6];
        basis(i, t);
        const double yv = lumaAt(img, i);
        for (int a = 0; a < 6; ++a) {
            b[a] += t[a] * yv;
            for (int c = 0; c < 6; ++c) {
                A[a * 6 + c] += t[a] * t[c];
            }
        }
        ++nr;
    }
    if (nr < 64 || !cholSolve(A, b, 6)) {
        return vis;
    }
    double sumP = 0.0, sumS = 0.0, sq = 0.0, sumF = 0.0;
    std::size_t nf = 0;
    for (std::size_t i = 0; i < gm.lx.size(); ++i) {
        if (!std::isfinite(gm.lx[i])) {
            continue;
        }
        const double rho = std::hypot(gm.lx[i] - g.cx, gm.ly[i] - g.cy) / reach;
        if (rho > 1.0) {
            continue;
        }
        double t[6];
        basis(i, t);
        double surf = 0.0;
        for (int a = 0; a < 6; ++a) {
            surf += b[a] * t[a];
        }
        const double yv = lumaAt(img, i);
        sq += (yv - surf) * (yv - surf);
        sumF += surf;
        ++nf;
        if (gm.weight[k][i] >= 0.9f) {
            sumP += yv - surf;
            sumS += surf;
            ++vis.plateauPx;
        }
    }
    if (vis.plateauPx > 0 && sumS > 0.0) {
        vis.contrast = sumP / sumS;
    }
    if (nf > 0 && sumF > 0.0) {
        vis.footprintRms = std::sqrt(sq / nf) / (sumF / nf);
    }
    return vis;
}

/// Pixels that changed outside every ghost footprint, and on the sun disc.
void damageReport(const render::ImageRGBAf& a, const render::ImageRGBAf& b, const GhostMap& gm,
                  const render::LensFlare& lf, const char* label) {
    std::size_t outside = 0, outsideChanged = 0, sun = 0, sunChanged = 0, changed = 0;
    double maxOutside = 0.0;
    for (std::size_t i = 0; i < gm.lx.size(); ++i) {
        bool diff = false;
        double d = 0.0;
        for (int c = 0; c < 4; ++c) {
            const float va = a.data[i * 4u + static_cast<std::size_t>(c)];
            const float vb = b.data[i * 4u + static_cast<std::size_t>(c)];
            if (va != vb) {
                diff = true;
                d = std::max(d, static_cast<double>(std::fabs(va - vb)));
            }
        }
        changed += diff ? 1u : 0u;
        bool inGhost = false;
        if (std::isfinite(gm.lx[i])) {
            for (std::size_t k = 0; k < lf.ghosts.size(); ++k) {
                const double rho = std::hypot(gm.lx[i] - lf.ghosts[k].cx, gm.ly[i] - lf.ghosts[k].cy);
                inGhost = inGhost || rho <= lf.ghosts[k].reach() + 1.0;
            }
            if (lf.sunFound && std::hypot(gm.lx[i] - lf.sunX, gm.ly[i] - lf.sunY) <= lf.sunRadiusPx) {
                ++sun;
                sunChanged += diff ? 1u : 0u;
            }
        }
        if (!inGhost) {
            ++outside;
            if (diff) {
                ++outsideChanged;
                maxOutside = std::max(maxOutside, d);
            }
        }
    }
    std::printf("    %s: %zu px changed; outside the ghosts %zu of %zu changed (max %.3g); sun disc %zu of %zu "
                "changed\n",
                label, changed, outsideChanged, outside, maxOutside, sunChanged, sun);
}

/// Render `job` on `r`, or print why not.
bool renderOk(render::IRenderer& r, const render::RenderJob& job, render::ImageRGBAf& out) {
    auto img = r.render(job);
    if (!img.ok()) {
        std::printf("    render failed: %s\n", img.error().message.c_str());
        return false;
    }
    out = std::move(img).value();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // ---- arguments -----------------------------------------------------------
    std::filesystem::path clipPath;
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        clipPath = env;
    } else {
        clipPath = OSV_SAMPLE_FILE;
    }
    std::filesystem::path outDir;
    int reps = 15;
    bool sweep = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--sweep") == 0) {
            sweep = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            // Every candidate the gates refuse, and why.
            log::setLevel(log::Level::Debug);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else {
            clipPath = argv[i];
        }
    }
    std::printf("osv_flare_bench - sun ghost / veil removal on %s\n", clipPath.string().c_str());
    std::printf("times: median of %d runs after 2 warm-ups (ms); a shared machine makes them noisy\n\n", reps);

    auto clip = openClip(clipPath);
    if (!clip.ok()) {
        std::printf("cannot open the clip: %s\n", clip.error().message.c_str());
        return 1;
    }
    const geom::LensRig& rig = clip.value().rig;
    ThreadPool pool;
    const OsvColorParams linear = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);
    const OsvColorParams rec709 = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Rec709, 0.0f);
    const render::FlareParams fp;

    // ---- decoders: host (CPU path) and NVDEC left on the device (GPU path) --
    auto hostReader = video::DualStreamReader::open(clipPath, clip.value().format);
    if (!hostReader.ok()) {
        std::printf("software decoder unavailable: %s\n", hostReader.error().message.c_str());
        return 1;
    }
    const bool gpuSampler = render::installCudaFlareSampler().ok();
    video::DecoderOptions devOpt;
    devOpt.hw = video::HwAccel::Cuda;
    devOpt.keepOnDevice = true;
    auto devReader = video::DualStreamReader::open(clipPath, clip.value().format, devOpt);
    const bool gpuPath = gpuSampler && devReader.ok();
    std::printf("CPU pool %u threads; GPU path %s\n\n", pool.size(), gpuPath ? "available" : "unavailable");

    // ---- 1 + 2: analysis and veil, three frames ----------------------------------
    render::FlareModel model0;
    std::array<std::array<double, 3>, 2> veil0{};
    video::FramePair pair0;
    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto hostPair = hostReader.value().read(frame);
        if (!hostPair.ok()) {
            std::printf("frame %u: decode failed: %s\n", frame, hostPair.error().message.c_str());
            return 1;
        }
        render::FlareModel model;
        const double cpuMs = medianMs(reps, [&]() -> Status {
            OSV_TRY_ASSIGN(model, render::analyseFlare(rig, hostPair.value(), linear, fp, pool));
            return okStatus();
        });
        double gpuMs = -1.0;
        double gpuSampleMs = -1.0;
        if (gpuPath) {
            auto devPair = devReader.value().read(frame);
            if (devPair.ok() && devPair.value().onDevice()) {
                video::FramePair onlyDevice = devPair.value();
                onlyDevice.lens[0] = video::PlanarFrame16{};  // force the device sampler
                onlyDevice.lens[1] = video::PlanarFrame16{};
                render::FlareModel gm;
                gpuMs = medianMs(reps, [&]() -> Status {
                    OSV_TRY_ASSIGN(gm, render::analyseFlare(rig, onlyDevice, linear, fp, pool));
                    return okStatus();
                });
                OsvPlane plane{};
                if (render::fillDevicePlane(onlyDevice.device[1], plane)) {
                    gpuSampleMs = medianMs(reps, [&]() -> Status {
                        OSV_TRY_ASSIGN(render::FlareImage im, render::cudaFlareDownsample(plane, true, linear, 4));
                        return okStatus();
                    });
                }
                // The two paths must agree on what they found.
                std::printf("  frame %u: GPU path found %zu ghosts in the master (CPU path %zu)\n", frame,
                            gm.lens[1].ghosts.size(), model.lens[1].ghosts.size());
            }
        }
        const double cpuSampleMs = medianMs(reps, [&]() -> Status {
            OSV_TRY_ASSIGN(render::FlareImage im, render::flareDownsample(hostPair.value().lens[1], linear, 4, pool));
            return okStatus();
        });
        // The same on a two-thread pool: what a background worker with no
        // share of the render pool pays.
        {
            ThreadPool small(1);
            const double smallMs = medianMs(std::max(1, reps / 3), [&]() -> Status {
                OSV_TRY_ASSIGN(render::FlareImage im,
                               render::flareDownsample(hostPair.value().lens[1], linear, 4, small));
                return okStatus();
            });
            std::printf("  frame %u: downsample one lens on a 2-thread pool %.1f ms\n", frame, smallMs);
        }
        // The detection + fit alone on a ready working image, with and without
        // the pool spreading the fits.
        double fitPoolMs = -1.0, fitSerialMs = -1.0;
        if (auto im = render::flareDownsample(hostPair.value().lens[1], linear, 4, pool); im.ok()) {
            fitPoolMs = medianMs(reps, [&]() -> Status {
                OSV_TRY_ASSIGN(render::LensFlare lf, render::analyseLensFlare(im.value(), rig.lens[1], fp, &pool));
                return okStatus();
            });
            fitSerialMs = medianMs(reps, [&]() -> Status {
                OSV_TRY_ASSIGN(render::LensFlare lf, render::analyseLensFlare(im.value(), rig.lens[1], fp, nullptr));
                return okStatus();
            });
        }
        // The per-frame sun check the importer pays on every wanted frame
        // (both lenses at ~375 px), from host frames and from device frames.
        const double sunCpuMs = medianMs(reps, [&]() -> Status {
            OSV_TRY_ASSIGN(render::FlareSunFixes fx, render::locateSuns(rig, hostPair.value(), linear, fp, pool));
            return okStatus();
        });
        double sunGpuMs = -1.0;
        if (gpuPath) {
            if (auto devPair = devReader.value().read(frame); devPair.ok() && devPair.value().onDevice()) {
                video::FramePair onlyDevice = devPair.value();
                onlyDevice.lens[0] = video::PlanarFrame16{};
                onlyDevice.lens[1] = video::PlanarFrame16{};
                sunGpuMs = medianMs(reps, [&]() -> Status {
                    OSV_TRY_ASSIGN(render::FlareSunFixes fx, render::locateSuns(rig, onlyDevice, linear, fp, pool));
                    return okStatus();
                });
            }
        }
        std::printf("  frame %u: sun check (both lenses) CPU %.2f ms, GPU %.2f ms\n", frame, sunCpuMs, sunGpuMs);
        std::printf("frame %u: analyseFlare CPU %.1f ms (downsample one lens %.1f), GPU %.1f ms (downsample one lens "
                    "%.1f); master detection + fits %.1f ms pooled, %.1f ms serial\n",
                    frame, cpuMs, cpuSampleMs, gpuMs, gpuSampleMs, fitPoolMs, fitSerialMs);
        printLens("slave ", model.lens[0]);
        printLens("master", model.lens[1]);

        // Veil (research only): overlap estimate and the sky step across the seam.
        render::BandParams band;
        auto bands = render::renderLensBands(rig, hostPair.value(), geom::BlendParams{}, band, true, nullptr, pool);
        if (bands.ok()) {
            auto veil = render::estimateVeil(bands.value(), model);
            if (veil.ok()) {
                std::printf("    veil estimate (overlap, research only): slave %.4f, master %.4f\n", veil.value()[0][0],
                            veil.value()[1][0]);
                if (frame == 0) {
                    veil0 = veil.value();
                }
                // Sky step: mean (master - slave) over co-visible equator rows of the sky columns.
                const render::LensBands& b = bands.value();
                double before = 0.0, after = 0.0, level = 0.0;
                std::size_t n = 0;
                for (std::uint32_t r = 0; r < b.h; ++r) {
                    const double lat = 90.0 - (b.rowOffset + r + 0.5) / b.mapH * 180.0;
                    if (std::fabs(lat) > 2.5) {
                        continue;
                    }
                    for (std::uint32_t c = 0; c < b.w; ++c) {
                        const std::size_t i = static_cast<std::size_t>(r) * b.w + c;
                        if (b.alpha[0][i] < 0.99f || b.alpha[1][i] < 0.99f) {
                            continue;
                        }
                        const double s = b.luma[0][i];
                        const double m = b.luma[1][i];
                        const float mv = osvFlareSoftSubtract(static_cast<float>(m),
                                                              static_cast<float>(veil.value()[1][0]));
                        const float sv = osvFlareSoftSubtract(static_cast<float>(s),
                                                              static_cast<float>(veil.value()[0][0]));
                        before += m - s;
                        after += static_cast<double>(mv) - static_cast<double>(sv);
                        level += s;
                        ++n;
                    }
                }
                if (n > 0) {
                    std::printf("    overlap step master - slave (linear, equator rows): before %+.4f, after %+.4f "
                                "(slave level %.4f)\n",
                                before / n, after / n, level / n);
                }
            }
        }
        if (frame == 0) {
            model0 = model;
            pair0 = hostPair.value();
        }
    }

    // ---- 2b: temporal stability of the brightest ghost over the whole clip -----
    // Per-frame fits jitter; smoothFlare (weight 0.5 on the new fit) is what a
    // video path would run.  Reported: how often the ghost is found, and the
    // frame-to-frame RMS change of its centre, size and plateau luma, raw and
    // smoothed.
    if (sweep) {
        const std::uint32_t frames = hostReader.value().frameCount();
        render::FlareModel prev;
        bool havePrev = false;
        std::uint32_t found = 0, total = 0;
        double dRaw = 0.0, dSm = 0.0, sRaw = 0.0, sSm = 0.0, aRaw = 0.0, aSm = 0.0;
        std::uint32_t pairs = 0;
        render::FlareGhost lastRaw, lastSm;
        bool haveLast = false;
        // The importer's schedule (plugins/importer/FlareStage), replayed in
        // frame order: a frame reuses a model measured within 4 buckets whose
        // sun check matches its own; otherwise it pays a measurement.
        struct Measured {
            std::uint32_t bucket;
            render::FlareSunFixes suns;
        };
        std::vector<Measured> measured;
        std::vector<std::uint32_t> measuredFrames;
        const double tolerance = render::flareSunTolerancePx(static_cast<std::uint32_t>(std::max(rig.streamW, 0)));
        double sunFirstX = 0.0, sunFirstY = 0.0, sunLastX = 0.0, sunLastY = 0.0;
        bool haveSun = false;
        for (std::uint32_t f = 0; f < frames; ++f) {
            auto pr = hostReader.value().read(f);
            if (!pr.ok()) {
                break;
            }
            if (auto fx = render::locateSuns(rig, pr.value(), linear, fp, pool); fx.ok()) {
                const std::uint32_t bucket = render::parallaxBucket(f);
                bool reused = false;
                for (const Measured& m : measured) {
                    const std::uint32_t gap = m.bucket > bucket ? m.bucket - bucket : bucket - m.bucket;
                    if (gap <= 4 && render::flareSunsMatch(m.suns, fx.value(), tolerance)) {
                        reused = true;
                        break;
                    }
                }
                if (!reused && (fx.value()[0].found || fx.value()[1].found)) {
                    measured.push_back({bucket, fx.value()});
                    measuredFrames.push_back(f);
                }
                if (fx.value()[1].found) {
                    if (!haveSun) {
                        sunFirstX = fx.value()[1].x;
                        sunFirstY = fx.value()[1].y;
                        haveSun = true;
                    }
                    sunLastX = fx.value()[1].x;
                    sunLastY = fx.value()[1].y;
                }
            }
            auto m = render::analyseFlare(rig, pr.value(), linear, fp, pool);
            if (!m.ok()) {
                continue;
            }
            ++total;
            const render::FlareModel sm = havePrev ? render::smoothFlare(prev, m.value(), 0.5) : m.value();
            prev = sm;
            havePrev = true;
            // The pill: the ghost nearest (1183, 1548).
            auto pick = [](const render::LensFlare& lf, render::FlareGhost& out) {
                double best = 60.0;
                bool ok = false;
                for (const render::FlareGhost& g : lf.ghosts) {
                    const double d = std::hypot(g.cx - 1183.0, g.cy - 1548.0);
                    if (d < best) {
                        best = d;
                        out = g;
                        ok = true;
                    }
                }
                return ok;
            };
            render::FlareGhost gr, gs;
            const bool okR = pick(m.value().lens[1], gr);
            const bool okS = pick(sm.lens[1], gs);
            found += okR ? 1u : 0u;
            if (okR && okS && haveLast) {
                auto lumaA = [](const render::FlareGhost& g) {
                    return 0.2627 * g.amp[0] + 0.6780 * g.amp[1] + 0.0593 * g.amp[2];
                };
                dRaw += std::pow(std::hypot(gr.cx - lastRaw.cx, gr.cy - lastRaw.cy), 2);
                dSm += std::pow(std::hypot(gs.cx - lastSm.cx, gs.cy - lastSm.cy), 2);
                sRaw += std::pow(std::sqrt(gr.hx * gr.hy) - std::sqrt(lastRaw.hx * lastRaw.hy), 2);
                sSm += std::pow(std::sqrt(gs.hx * gs.hy) - std::sqrt(lastSm.hx * lastSm.hy), 2);
                aRaw += std::pow(lumaA(gr) - lumaA(lastRaw), 2);
                aSm += std::pow(lumaA(gs) - lumaA(lastSm), 2);
                ++pairs;
            }
            if (okR && okS) {
                lastRaw = gr;
                lastSm = gs;
                haveLast = true;
            }
        }
        std::printf("\nschedule: an in-order pass over %u frames measures %zu times (tolerance %.1f px; sun moved "
                    "%.1f px):", frames, measuredFrames.size(), tolerance,
                    std::hypot(sunLastX - sunFirstX, sunLastY - sunFirstY));
        for (const std::uint32_t f : measuredFrames) {
            std::printf(" %u", f);
        }
        std::printf("\n");
        if (pairs > 0) {
            std::printf("\nsweep over %u frames: the pill ghost found in %u; frame-to-frame RMS change raw / smoothed: "
                        "centre %.2f / %.2f px, size %.2f / %.2f px, plateau luma %.4f / %.4f\n",
                        total, found, std::sqrt(dRaw / pairs), std::sqrt(dSm / pairs), std::sqrt(sRaw / pairs),
                        std::sqrt(sSm / pairs), std::sqrt(aRaw / pairs), std::sqrt(aSm / pairs));
        }
    }

    // ---- 3: removal quality on frame 0 ---------------------------------------------
    const render::LensFlare& master = model0.lens[1];
    if (!master.sunFound || master.ghosts.empty()) {
        std::printf("\nno ghost in frame 0: nothing to measure\n");
        return 0;
    }
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        std::printf("\nCUDA renderer unavailable (%s): removal not measured\n", reason.c_str());
        return 0;
    }
    auto cuda = render::CudaRenderer::create(0);
    if (!cuda.ok()) {
        std::printf("\nCUDA renderer creation failed: %s\n", cuda.error().message.c_str());
        return 1;
    }
    // Views: the sun at 100 deg (what an editor frames), the brightest ghost
    // close up at 30 deg, and the full equirect.
    const geom::VirtualCamera sunView = lookAt(masterPixelDir(rig, master.sunX, master.sunY), 100.0, 1920, 1080);
    const geom::VirtualCamera ghostView =
        lookAt(masterPixelDir(rig, master.ghosts[0].cx, master.ghosts[0].cy), 30.0, 1280, 720);
    struct View {
        const char* name;
        render::RenderJob job;
    };
    std::vector<View> views;
    for (const auto& [name, cam] : {std::pair<const char*, geom::VirtualCamera>{"sun100", sunView},
                                    std::pair<const char*, geom::VirtualCamera>{"ghost30", ghostView}}) {
        auto job = render::RenderParamsBuilder().rig(rig).camera(cam).color(linear).build(pair0);
        if (job.ok()) {
            views.push_back({name, std::move(job).value()});
        }
    }
    {
        geom::EquirectMap map;
        map.w = 4096;
        map.h = 2048;
        auto job = render::RenderParamsBuilder().rig(rig).equirect(map).color(linear).build(pair0);
        if (job.ok()) {
            views.push_back({"equirect", std::move(job).value()});
        }
    }
    std::printf("\nremoval on frame 0 (CUDA renderer, contrasts in scene-linear luma)\n");
    for (View& v : views) {
        render::RenderJob off = v.job;
        render::clearFlare(off.params);
        render::RenderJob on = v.job;
        render::applyFlare(model0, on.params);
        render::ImageRGBAf a, b;
        if (!renderOk(*cuda.value(), off, a) || !renderOk(*cuda.value(), on, b)) {
            continue;
        }
        const GhostMap gm = ghostMap(on.params);
        std::printf("  view %s (%dx%d)\n", v.name, on.params.outW, on.params.outH);
        for (std::size_t k = 0; k < master.ghosts.size(); ++k) {
            const Visibility v0 = ghostVisibility(a, gm, master, k);
            const Visibility v1 = ghostVisibility(b, gm, master, k);
            if (v0.plateauPx == 0) {
                continue;
            }
            std::printf("    ghost %zu: plateau %zu px; contrast before %+.4f, after %+.4f; footprint RMS before %.4f, "
                        "after %.4f\n",
                        k, v0.plateauPx, v0.contrast, v1.contrast, v0.footprintRms, v1.footprintRms);
        }
        damageReport(a, b, gm, master, "damage");

        // Kernel cost, alternated back to back.
        if (reps > 0) {
            std::vector<double> tOff, tOn;
            render::ImageRGBAf scratch;
            for (int i = 0; i < reps + 2; ++i) {
                auto t0 = Clock::now();
                (void)cuda.value()->renderInto(off, scratch);
                auto t1 = Clock::now();
                (void)cuda.value()->renderInto(on, scratch);
                auto t2 = Clock::now();
                if (i >= 2) {
                    tOff.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                    tOn.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
                }
            }
            std::sort(tOff.begin(), tOff.end());
            std::sort(tOn.begin(), tOn.end());
            std::printf("    CUDA render (incl. host download): off %.2f ms, on %.2f ms\n", tOff[tOff.size() / 2],
                        tOn[tOn.size() / 2]);
        }

        // Rec.709 stills for the crops.
        if (!outDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(outDir, ec);
            io::ImageTag tag;
            tag.transfer = io::ImageTransfer::Rec709;
            tag.rec2020 = false;
            tag.writeSidecar = false;
            render::RenderJob off709 = off;
            render::RenderJob on709 = on;
            off709.params.color = rec709;
            on709.params.color = rec709;
            render::ImageRGBAf a7, b7;
            if (renderOk(*cuda.value(), off709, a7) && renderOk(*cuda.value(), on709, b7)) {
                const std::string base = (outDir / v.name).string();
                (void)io::writeImage(base + "_before.tif", a7, io::ImageFormat::Tiff16, tag);
                (void)io::writeImage(base + "_after.tif", b7, io::ImageFormat::Tiff16, tag);
                // The ghost plateau weights, for the measurement script.
                render::ImageRGBAf wimg;
                if (auto wi = render::ImageRGBAf::create(gm.w, gm.h); wi.ok()) {
                    wimg = std::move(wi).value();
                    for (std::size_t i = 0; i < gm.lx.size(); ++i) {
                        float w = 0.0f;
                        for (const auto& wv : gm.weight) {
                            w = std::max(w, wv[i]);
                        }
                        wimg.data[i * 4u] = wimg.data[i * 4u + 1] = wimg.data[i * 4u + 2] = w;
                        wimg.data[i * 4u + 3] = 1.0f;
                    }
                    io::ImageTag lin;
                    lin.transfer = io::ImageTransfer::Linear;
                    lin.writeSidecar = false;
                    (void)io::writeImage(base + "_ghostmask.tif", wimg, io::ImageFormat::Tiff16, lin);
                }
            }
        }
    }
    // ---- research only: the equirect with the overlap veil removed too --------
    // Not a shipping path (see the LEGAL note on estimateVeil): rendered so
    // the research note can show what the veil does to the seam band.
    if (!outDir.empty() && (veil0[0][0] > 0.0 || veil0[1][0] > 0.0)) {
        geom::EquirectMap map;
        map.w = 4096;
        map.h = 2048;
        auto job = render::RenderParamsBuilder().rig(rig).equirect(map).color(rec709).build(pair0);
        if (job.ok()) {
            render::FlareModel withVeil = model0;
            withVeil.lens[0].veil = veil0[0];
            withVeil.lens[1].veil = veil0[1];
            render::applyFlare(withVeil, job.value().params);
            render::ImageRGBAf img;
            if (renderOk(*cuda.value(), job.value(), img)) {
                io::ImageTag tag;
                tag.transfer = io::ImageTransfer::Rec709;
                tag.rec2020 = false;
                tag.writeSidecar = false;
                (void)io::writeImage((outDir / "equirect_veil_after.tif").string(), img, io::ImageFormat::Tiff16, tag);
            }
        }
    }
    std::printf("\nsizeof(OsvRenderParams) = %zu bytes (limit 4096)\n", sizeof(OsvRenderParams));
    render::uninstallCudaFlareSampler();
    return 0;
}
