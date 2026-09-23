// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GpuAnalysisBench.cpp - what one parallax measurement costs, CPU vs GPU.
//
// A measurement tool, not a test: it asserts nothing and is deliberately NOT
// registered with ctest, because its numbers depend on the machine.  Run it
// by hand:
//
//     build\<dir>\bin\osv_gpu_analysis_bench.exe [clip.OSV] [--reps N]
//
// The clip defaults to OSV_SAMPLE_FILE (environment first, then the path the
// build was configured with).
//
// WHAT IT MEASURES
// ----------------
// docs/DIRECT_GPU.md, WP-B: one bucket's parallax measurement - the two lens
// bands, bidirectional flow across them, and the grid built from the flow -
// on frames 0, 32 and 64 of the clip, two ways:
//
//   CPU   frames decoded to host memory; bands shaded on the CPU across the
//         thread pool; flow by the Classical (CPU) DIS on the same pool.  This
//         is the path the importer runs today.
//
//   GPU   frames decoded by NVDEC and LEFT IN VRAM (keepOnDevice); bands
//         shaded on the GPU from those frames (the installed device band
//         shader); flow by the ClassicalCuda backend.  This is the path the
//         direct GPU pipeline runs.
//
// The grid step (gridFromFlow) is the same CPU code in both columns.  Each
// stage is timed on its own - median of --reps runs after two warm-up runs,
// so first-use costs (CUDA module load, workspace allocation) are excluded -
// and "total" is one end-to-end measurement (measureParallaxBands +
// parallaxFromBands, the importer's own call sequence), also a median.
//
// Output is plain ASCII so it survives any Windows console code page.

#include "osv/container/OsvFile.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CudaAnalysis.h"
#include "osv/render/FlowBackend.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/PhotoSeam.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace osv;
using render::FlowBackendKind;

namespace {

using Clock = std::chrono::steady_clock;

/// Median of `reps` timed runs of `body` in milliseconds, after two untimed
/// warm-up runs.  Returns a negative number if any run fails, having printed
/// why - a failed stage must not print a plausible-looking time.
double medianMs(int reps, const std::function<Status()>& body, const char* what) {
    for (int i = 0; i < 2; ++i) {
        const Status st = body();
        if (!st.ok()) {
            std::printf("  %s failed: %s\n", what, st.error().message.c_str());
            return -1.0;
        }
    }
    std::vector<double> ms;
    ms.reserve(static_cast<std::size_t>(reps));
    for (int i = 0; i < reps; ++i) {
        const auto t0 = Clock::now();
        const Status st = body();
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (!st.ok()) {
            std::printf("  %s failed: %s\n", what, st.error().message.c_str());
            return -1.0;
        }
        ms.push_back(elapsed);
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

/// Clip, rig and format - everything but the frames.
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

/// One column of the table: per-stage medians for one frame on one path.
struct Row {
    double bands = -1.0;
    double flow = -1.0;
    double grid = -1.0;
    double gridOneThread = -1.0;
    double total = -1.0;
    double consistentPct = 0.0;
    std::uint32_t gated = 0;
    std::uint32_t measured = 0;
};

/// Time the stages of one measurement of `pair` with `backend`.
Row timeFrame(const Clip& clip, const video::FramePair& pair, FlowBackendKind backend, ThreadPool& pool, int reps) {
    Row row;
    const geom::BlendParams blend;
    render::ParallaxWarpParams params;
    params.backend = backend;

    // Stage 1: the two lens bands.
    render::LensBands bands;
    row.bands = medianMs(
        reps,
        [&]() -> Status {
            OSV_TRY_ASSIGN(bands, render::measureParallaxBands(clip.rig, pair, blend, params, nullptr, pool));
            return okStatus();
        },
        "bands");
    if (row.bands < 0.0) {
        return row;
    }

    // Stage 2: bidirectional flow across them, through the public selector
    // so the backend that ran is the one the importer would get.
    render::GrayImage a;
    a.w = bands.w;
    a.h = bands.h;
    a.data = bands.luma[0];
    render::GrayImage b = a;
    b.data = bands.luma[1];
    render::BidirFlow flow;
    row.flow = medianMs(
        reps,
        [&]() -> Status {
            FlowBackendKind used = FlowBackendKind::Count;
            OSV_TRY_ASSIGN(flow, render::computeFlow(backend, a, b, params.flow, &pool, &used));
            if (used != backend) {
                return failStatus(ErrorCode::Internal, std::string("asked for ") + render::flowBackendName(backend) +
                                                           ", ran " + render::flowBackendName(used));
            }
            return okStatus();
        },
        "flow");
    if (row.flow < 0.0) {
        return row;
    }

    // Stage 3: the grid (the same CPU code for both paths), on the pool as
    // parallaxFromBands runs it, and on one thread as a background worker
    // without a pool would.
    render::ParallaxWarpGrid grid;
    row.grid = medianMs(
        reps,
        [&]() -> Status {
            OSV_TRY_ASSIGN(grid, render::gridFromFlow(bands, flow, params, &pool));
            return okStatus();
        },
        "grid");
    row.gridOneThread = medianMs(
        reps,
        [&]() -> Status {
            OSV_TRY_ASSIGN(render::ParallaxWarpGrid g, render::gridFromFlow(bands, flow, params, nullptr));
            (void)g;
            return okStatus();
        },
        "grid (one thread)");
    row.consistentPct = 100.0 * grid.consistentFraction();
    row.gated = grid.gatedCells;
    row.measured = grid.measuredCells;

    // End to end, the way the importer calls it.
    row.total = medianMs(
        reps,
        [&]() -> Status {
            OSV_TRY_ASSIGN(render::LensBands bb,
                           render::measureParallaxBands(clip.rig, pair, blend, params, nullptr, pool));
            OSV_TRY_ASSIGN(render::ParallaxWarpGrid g, render::parallaxFromBands(bb, params, &pool));
            (void)g;
            return okStatus();
        },
        "total");
    return row;
}

// [WP-PHOTO] One photometric seam field measurement: the per-lens RGB bands
// (GPU band shader for device frames, CPU rows for host frames) and the
// statistics, each a median; budget (NEURAL_STITCHING.md 8.1): <= 3 ms per
// bucket from device frames, <= 10 ms from host frames.
struct PhotoRow {
    double bands = -1.0;
    double stats = -1.0;
    double total = -1.0;
};

PhotoRow timePhoto(const Clip& clip, const video::FramePair& pair, ThreadPool& pool, int reps) {
    PhotoRow row;
    const geom::BlendParams blend;
    const render::PhotoSeamParams params;
    render::RgbLensBands bands;
    row.bands = medianMs(
        reps,
        [&]() -> Status {
            OSV_TRY_ASSIGN(bands, render::renderPhotoBands(clip.rig, pair, blend, params, pool));
            return okStatus();
        },
        "photo bands");
    if (row.bands < 0.0) {
        return row;
    }
    row.stats = medianMs(
        reps,
        [&]() -> Status {
            OSV_TRY_ASSIGN(render::PhotoSeamField f, render::photoSeamFromBands(bands, params, &pool));
            (void)f;
            return okStatus();
        },
        "photo stats");
    row.total = medianMs(
        reps,
        [&]() -> Status {
            OSV_TRY_ASSIGN(render::PhotoSeamField f, render::measurePhotoSeam(clip.rig, pair, blend, params, pool));
            (void)f;
            return okStatus();
        },
        "photo total");
    return row;
}

void printRow(const char* path, std::uint32_t frame, const Row& r) {
    std::printf("  %-4s  frame %2u   bands %7.3f   flow %7.3f   grid %7.3f (1 thread %6.3f)   total %7.3f ms"
                "   (consistent %5.1f %%, gated %u/%u)\n",
                path, frame, r.bands, r.flow, r.grid, r.gridOneThread, r.total, r.consistentPct, r.gated, r.measured);
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
    int reps = 25;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = std::max(1, std::atoi(argv[++i]));
        } else {
            clipPath = argv[i];
        }
    }
    std::printf("osv_gpu_analysis_bench - one parallax measurement (bands + bidirectional flow + grid)\n");
    std::printf("clip: %s\nmedian of %d runs after 2 warm-up runs; times in ms\n", clipPath.string().c_str(), reps);

    auto clip = openClip(clipPath);
    if (!clip.ok()) {
        std::printf("cannot open the clip: %s\n", clip.error().message.c_str());
        return 1;
    }

    // ---- the GPU path needs the analyses installed and NVDEC ---------------
    const Status installed = render::installCudaAnalyses();
    if (!installed.ok()) {
        std::printf("GPU analyses unavailable: %s\n", installed.error().message.c_str());
        return 1;
    }
    video::DecoderOptions devOpt;
    devOpt.hw = video::HwAccel::Cuda;
    devOpt.keepOnDevice = true;
    auto devReader = video::DualStreamReader::open(clipPath, clip.value().format, devOpt);
    if (!devReader.ok()) {
        std::printf("NVDEC unavailable: %s\n", devReader.error().message.c_str());
        return 1;
    }
    auto hostReader = video::DualStreamReader::open(clipPath, clip.value().format);
    if (!hostReader.ok()) {
        std::printf("software decoder unavailable: %s\n", hostReader.error().message.c_str());
        return 1;
    }

    ThreadPool pool;  // every core, as the importer's render pool
    std::printf("CPU pool: %u threads\n\n", pool.size());

    for (const std::uint32_t frame : {0u, 32u, 64u}) {
        auto hostPair = hostReader.value().read(frame);
        auto devPair = devReader.value().read(frame);
        if (!hostPair.ok() || !devPair.ok()) {
            std::printf("frame %u: decode failed (%s)\n", frame,
                        (!hostPair.ok() ? hostPair.error() : devPair.error()).message.c_str());
            return 1;
        }
        if (!devPair.value().onDevice()) {
            std::printf("frame %u: NVDEC did not leave the frame on the device\n", frame);
            return 1;
        }
        const Row cpu = timeFrame(clip.value(), hostPair.value(), FlowBackendKind::Classical, pool, reps);
        const Row gpu = timeFrame(clip.value(), devPair.value(), FlowBackendKind::ClassicalCuda, pool, reps);
        printRow("CPU", frame, cpu);
        printRow("GPU", frame, gpu);
        if (cpu.total > 0.0 && gpu.total > 0.0) {
            std::printf("        speed-up: bands %.1fx, flow %.1fx, total %.1fx\n\n", cpu.bands / gpu.bands,
                        cpu.flow / gpu.flow, cpu.total / gpu.total);
        }
        // [WP-PHOTO] the photometric seam field on the same frames.
        const PhotoRow pc = timePhoto(clip.value(), hostPair.value(), pool, reps);
        const PhotoRow pg = timePhoto(clip.value(), devPair.value(), pool, reps);
        std::printf("  photo CPU frame %2u   bands %7.3f   stats %7.3f   total %7.3f ms\n", frame, pc.bands, pc.stats,
                    pc.total);
        std::printf("  photo GPU frame %2u   bands %7.3f   stats %7.3f   total %7.3f ms\n\n", frame, pg.bands,
                    pg.stats, pg.total);
    }
    render::uninstallCudaAnalyses();
    return 0;
}
