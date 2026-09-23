// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// seamprobe - disposable research tool (research/aistitch, not part of the
// build).  Measures how the carved blend seam moves from frame to frame,
// with the release build's own analyses:
//
//   * every frame: the parallax grid (classical DIS, production defaults) and
//     the lens bands the carve sees, BOTH uncorrected and seen through that
//     grid (correctBandsForSeam), code-space luma + coverage;
//   * the importer's schedule: one carve per 8-frame bucket, at the bucket's
//     first frame, through that frame's grid, steered by the previous
//     bucket's seam (ImporterInstance::applyAnalyses' carve);
//   * every frame carved on its own, steered by the previous frame's seam.
//
// Output under <outdir>:
//   f<F>_luma0.f32 / f<F>_luma1.f32 / f<F>_alpha0.f32 / f<F>_alpha1.f32   corrected bands
//   bucket_<B>.f32   carved table of bucket B (columns x 2 floats: lat rad, half width rad)
//   frame_<F>.f32    carved table of frame F (per-frame schedule)
//   meta.json        band geometry
//
// Usage: seamprobe <clip.OSV> <outdir> <firstFrame> <lastFrame>

#include "../../tools/osvtool/Pipeline.h"

#include "osv/render/FlowBackend.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/SeamCarve.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

/// Write a float buffer as raw little-endian float32.
bool writeRaw(const std::string& path, const std::vector<float>& v) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(float)));
    return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: seamprobe <clip> <outdir> <firstFrame> <lastFrame>\n");
        return 2;
    }
    const std::string clip = argv[1];
    const std::string outDir = argv[2];
    const int first = std::atoi(argv[3]);
    const int last = std::atoi(argv[4]);
    if (first < 0 || last < first) {
        std::fprintf(stderr, "bad frame range\n");
        return 2;
    }

    // ---- open the clip as osvtool does (CPU render, host frames) ------------
    osvtool::PipelineOptions po;
    po.input = clip;
    po.device = "cpu";
    po.hw = "none";
    po.color = "linear";
    po.hostFramesRequired = true;
    auto pipe = osvtool::Pipeline::open(po, true);
    if (!pipe.ok()) {
        std::fprintf(stderr, "open failed: %s\n", pipe.error().toString().c_str());
        return 1;
    }
    osvtool::Pipeline& P = *pipe.value();

    osv::render::ParallaxWarpParams pw;  // production defaults: 2048 x +-6 deg
    pw.backend = osv::render::FlowBackendKind::Classical;
    const osv::render::SeamCarveParams carveParams;  // production defaults

    std::shared_ptr<osv::render::BlendSeam> prevBucket;
    std::shared_ptr<osv::render::BlendSeam> prevFrame;
    bool metaWritten = false;

    for (int frame = first; frame <= last; ++frame) {
        if (static_cast<std::uint32_t>(frame) >= P.frameCount()) {
            break;
        }
        auto pair = P.reader->read(static_cast<std::uint32_t>(frame));
        if (!pair.ok()) {
            std::fprintf(stderr, "decode %d failed: %s\n", frame, pair.error().toString().c_str());
            continue;
        }

        // ---- this frame's parallax grid ---------------------------------------
        auto grid = osv::render::buildParallaxWarp(P.rig, pair.value(), P.blendParams, pw, nullptr, *P.pool);
        osv::render::WarpGridView view;
        osv::render::SeamCorrection corr;
        if (grid.ok() && grid.value().valid()) {
            const auto& g = grid.value();
            view.uv = g.uv.data();
            view.w = g.w;
            view.h = g.h;
            view.latMinRad = g.latMinRad;
            view.latMaxRad = g.latMaxRad;
            corr.warp = &view;
            // The grid itself (half corrections, radians), per frame, so the
            // glide the importer applies between buckets can be replayed.
            writeRaw(outDir + "/grid_" + std::to_string(frame) + ".f32", g.uv);
            std::ofstream gm(outDir + "/grid_meta.json");
            gm << "{\"w\":" << g.w << ",\"h\":" << g.h << ",\"latMinRad\":" << g.latMinRad
               << ",\"latMaxRad\":" << g.latMaxRad << "}\n";
        } else {
            std::fprintf(stderr, "frame %d: parallax grid refused\n", frame);
        }

        // ---- the bands the carve sees, through the grid ------------------------
        auto bands = osv::render::renderLensBands(P.rig, pair.value(), P.blendParams, pw.band, false, nullptr,
                                                  *P.pool);
        if (!bands.ok()) {
            std::fprintf(stderr, "bands %d failed\n", frame);
            return 1;
        }
        auto seen = osv::render::correctBandsForSeam(bands.value(), corr, P.pool.get());
        if (!seen.ok()) {
            std::fprintf(stderr, "correct %d failed\n", frame);
            return 1;
        }
        const osv::render::LensBands& b = seen.value();
        const std::string base = outDir + "/f" + std::to_string(frame) + "_";
        writeRaw(base + "luma0.f32", b.luma[0]);
        writeRaw(base + "luma1.f32", b.luma[1]);
        writeRaw(base + "alpha0.f32", b.alpha[0]);
        writeRaw(base + "alpha1.f32", b.alpha[1]);
        if (!metaWritten) {
            std::ofstream meta(outDir + "/meta.json");
            meta << "{\"w\":" << b.w << ",\"h\":" << b.h << ",\"rowOffset\":" << b.rowOffset << ",\"mapH\":" << b.mapH
                 << ",\"columns\":" << carveParams.columns << "}\n";
            metaWritten = true;
        }

        // ---- importer schedule: one carve per bucket, steered by the last ------
        if (frame % static_cast<int>(osv::render::kParallaxBucketFrames) == 0) {
            auto s = osv::render::carveSeamFromBands(bands.value(), corr, carveParams, prevBucket.get(),
                                                     P.pool.get());
            if (s.ok()) {
                prevBucket = std::make_shared<osv::render::BlendSeam>(std::move(s).value());
                writeRaw(outDir + "/bucket_" + std::to_string(osv::render::parallaxBucket(frame)) + ".f32",
                         prevBucket->table);
            }
        }

        // ---- every frame, steered by the previous frame ------------------------
        auto s = osv::render::carveSeamFromBands(bands.value(), corr, carveParams, prevFrame.get(), P.pool.get());
        if (s.ok()) {
            prevFrame = std::make_shared<osv::render::BlendSeam>(std::move(s).value());
            writeRaw(outDir + "/frame_" + std::to_string(frame) + ".f32", prevFrame->table);
        }
        std::printf("frame %d done\n", frame);
    }
    return 0;
}
