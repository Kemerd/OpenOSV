// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// bandprobe - disposable research tool (research/neural, not part of the
// build).  Renders EACH LENS ALONE over the polar-axis overlap band, in
// scene-linear RGB, so the photometric disagreement between the two lenses
// can be measured at identical directions.  Also writes the current
// production blend (both lenses, feather, no gain) of the same band and the
// per-pixel angle from each lens axis.
//
// Output per frame F, under <outdir>:
//   f<F>_lens0.f32 / f<F>_lens1.f32   RGBA float32, w x h (alpha = coverage)
//   f<F>_blend.f32                    RGBA float32, production feather blend
//   f<F>_theta.f32                    2 floats per pixel: theta0, theta1 (deg)
//   meta.json                         geometry of the band
//
// Usage: bandprobe <clip.OSV> <outdir> <mapW> <bandHalfDeg> <frame> [frame...]

#include "../../tools/osvtool/Pipeline.h"

#include "osv/color/ColorParams.h"
#include "osv/geom/EquirectMap.h"
#include "osv/render/RenderParamsBuilder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Write a float buffer as raw little-endian float32.
bool writeRaw(const std::string& path, const float* data, std::size_t count) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(count * sizeof(float)));
    return static_cast<bool>(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: bandprobe <clip> <outdir> <mapW> <bandHalfDeg> <frame> [frame...]\n");
        return 2;
    }
    const std::string clip = argv[1];
    const std::string outDir = argv[2];
    const int mapW = std::atoi(argv[3]);
    const double bandHalfDeg = std::atof(argv[4]);
    if (mapW < 256 || mapW > 16384 || bandHalfDeg <= 0.0 || bandHalfDeg > 45.0) {
        std::fprintf(stderr, "bad mapW / bandHalfDeg\n");
        return 2;
    }

    // ---- open the clip exactly as osvtool does (CPU render, host frames) ----
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

    // ---- band geometry: full polar map, only the band rows are kept ---------
    osv::geom::EquirectMap map;
    map.layout = osv::geom::EquirectLayout::PolarAxis;
    map.w = mapW;
    map.h = mapW / 2;
    const double rowsPerDeg = static_cast<double>(map.h) / 180.0;
    const int halfRows = static_cast<int>(std::lround(bandHalfDeg * rowsPerDeg));
    const int row0 = map.h / 2 - halfRows;
    const int row1 = map.h / 2 + halfRows;
    const int bandH = row1 - row0;

    // Scene-linear output, no exposure offset: the analysis domain.
    const OsvColorParams cpLinear =
        osv::color::makeColorParams(osv::color::kDefaultDlogMFit, osv::color::OutputTransfer::Linear, 0.0f);

    {
        std::ofstream meta(outDir + "/meta.json");
        meta << "{\"w\":" << map.w << ",\"mapH\":" << map.h << ",\"row0\":" << row0 << ",\"h\":" << bandH
             << ",\"featherDeg\":" << P.blendParams.featherDeg << ",\"lensFovDeg\":" << P.blendParams.lensFovDeg
             << "}\n";
    }

    for (int ai = 5; ai < argc; ++ai) {
        const int frame = std::atoi(argv[ai]);
        if (frame < 0 || static_cast<std::uint32_t>(frame) >= P.frameCount()) {
            std::fprintf(stderr, "frame %d out of range\n", frame);
            continue;
        }
        auto pair = P.reader->read(static_cast<std::uint32_t>(frame));
        if (!pair.ok()) {
            std::fprintf(stderr, "decode %d failed: %s\n", frame, pair.error().toString().c_str());
            continue;
        }

        // variant 0 = lens 0 alone, 1 = lens 1 alone, 2 = production blend
        for (int variant = 0; variant < 3; ++variant) {
            osv::render::RenderParamsBuilder b;
            b.rig(P.rig).equirect(map).blend(P.blendParams, true).color(cpLinear);
            if (variant < 2) {
                // Alone, and with alpha = this lens's own blend weight
                // (FOV feather x occlusion ramp), so the research scripts can
                // re-blend with exactly the production weights.
                b.lensEnabled(1 - variant, false).alphaCoverage(true);
            }
            auto job = b.build(pair.value());
            if (!job.ok()) {
                std::fprintf(stderr, "build failed: %s\n", job.error().toString().c_str());
                return 1;
            }
            auto img = P.renderer->render(job.value());
            if (!img.ok()) {
                std::fprintf(stderr, "render failed: %s\n", img.error().toString().c_str());
                return 1;
            }
            const auto& I = img.value();
            const float* band = I.data.data() + static_cast<std::size_t>(row0) * I.w * 4u;
            const char* tag = variant == 0 ? "lens0" : (variant == 1 ? "lens1" : "blend");
            const std::string path = outDir + "/f" + std::to_string(frame) + "_" + tag + ".f32";
            if (!writeRaw(path, band, static_cast<std::size_t>(bandH) * I.w * 4u)) {
                return 1;
            }

            // Angle of each band pixel from each lens axis, from the params the
            // kernel actually receives (includes the calibrated extrinsics).
            if (variant == 2 && ai == 5) {
                auto params = b.buildParams();
                if (!params.ok()) {
                    return 1;
                }
                const OsvRenderParams& p = params.value();
                // Lens intrinsics as the kernel receives them (stream pixels),
                // for relating band angles back to fisheye radii.
                {
                    std::ofstream lj(outDir + "/lenses.json");
                    lj << "[";
                    for (int L = 0; L < 2; ++L) {
                        const OsvLens& q = p.lens[L];
                        lj << (L ? "," : "") << "{\"fx\":" << q.fx << ",\"fy\":" << q.fy << ",\"cx\":" << q.cx
                           << ",\"cy\":" << q.cy << ",\"k\":[" << q.k[0] << "," << q.k[1] << "," << q.k[2] << ","
                           << q.k[3] << "," << q.k[4] << "],\"thetaMax\":" << q.thetaMax << ",\"R\":[";
                        for (int i = 0; i < 9; ++i) {
                            lj << (i ? "," : "") << q.R[i];
                        }
                        lj << "],\"w\":" << q.width << ",\"h\":" << q.height << "}";
                    }
                    lj << "]\n";
                }
                std::vector<float> th(static_cast<std::size_t>(bandH) * map.w * 2u);
                for (int y = 0; y < bandH; ++y) {
                    for (int x = 0; x < map.w; ++x) {
                        osv::Vec3d d;
                        map.pixelToDir(x + 0.5, row0 + y + 0.5, d);
                        const float db[3] = {static_cast<float>(d.x), static_cast<float>(d.y),
                                             static_cast<float>(d.z)};
                        for (int L = 0; L < 2; ++L) {
                            const float* R = p.lens[L].R;
                            const float lx = R[0] * db[0] + R[1] * db[1] + R[2] * db[2];
                            const float ly = R[3] * db[0] + R[4] * db[1] + R[5] * db[2];
                            const float lz = R[6] * db[0] + R[7] * db[1] + R[8] * db[2];
                            const float t = std::atan2(std::sqrt(lx * lx + ly * ly), lz);
                            th[(static_cast<std::size_t>(y) * map.w + x) * 2u + L] = t * 57.29577951f;
                        }
                    }
                }
                writeRaw(outDir + "/theta.f32", th.data(), th.size());
            }
        }
        std::printf("frame %d done\n", frame);
    }
    return 0;
}
