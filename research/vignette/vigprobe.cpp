// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// vigprobe - disposable research tool (research/vignette, not part of the
// build).  Dumps what the lens shading research (NEURAL_STITCHING.md,
// section 9) needs from a clip:
//
//   att   <clip> <outdir>
//         per-frame camera attitude (worldFromBody), its rotation relative
//         to frame 0 and each lens axis in that world frame -> att.csv.
//
//   lens  <clip> <outdir> <mapW> <stab 0|1> <frame> [frame...]
//         each lens ALONE over the whole sphere, polar-axis equirect (lens
//         axes at the poles), in the camera's NATIVE scene-linear light
//         (identity colour matrices, exactly like PhotoSeam's bands), alpha
//         = the occlusion factor alone (no FOV feather).  With stab = 1 the
//         map is world-fixed (full stabilisation relative to frame 0), so a
//         distant sky pixel keeps its map position while the lenses rotate.
//         f<F>_lens<L>.f32 : RGBA float32, mapW x mapW/2
//         f<F>_theta.f32   : 2 float32 per pixel, theta0 / theta1 (deg)
//         meta.json        : geometry
//
//   field <clip> <outdir> <mapW> <frame>
//         the photometric seam field of the frame's bucket and the polar map
//         rendered with it: blended and each lens alone (native linear).
//
//   seamview <clip> <out.pgm> <w> <h> <fov> <distortion> <yaw> <pitch> <stab> <frame>
//         where the stitch seam runs in an eye-offset view (a PGM mask).
//
// Build: research\vignette\build_vigprobe.cmd (inside a VS dev shell).

#include "../../tools/osvtool/Pipeline.h"

#include "osv/color/ColorParams.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/RenderParamsBuilder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

/// Write a float buffer as raw little-endian float32.
bool writeRaw(const std::string& path, const float* data, std::size_t count) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(count * sizeof(float)));
    return static_cast<bool>(f);
}

/// Open the clip the way osvtool does: CPU render, host frames, linear.
osv::Result<std::unique_ptr<osvtool::Pipeline>> openClip(const std::string& clip, bool stab) {
    osvtool::PipelineOptions po;
    po.input = clip;
    po.device = "cpu";
    po.hw = "none";
    po.color = "linear";
    po.stab = stab ? "full" : "off";
    po.hostFramesRequired = true;
    return osvtool::Pipeline::open(po, true);
}

/// Frame time in microseconds, from the metadata track (nominal spacing as
/// the fallback), exactly like Pipeline::stabilizationFor.
double frameTimeUs(const osvtool::Pipeline& P, std::uint32_t f) {
    double tUs = P.attitude ? P.attitude->beginUs() : 0.0;
    auto fm = P.track.frame(f);
    if (fm.ok()) {
        tUs = static_cast<double>(fm.value().timestampUs);
    } else if (P.fps() > 0.0) {
        tUs += static_cast<double>(f) * 1e6 / P.fps();
    }
    return tUs;
}

int runAtt(const std::string& clip, const std::string& outDir) {
    auto pipe = openClip(clip, true);
    if (!pipe.ok()) {
        std::fprintf(stderr, "open failed: %s\n", pipe.error().toString().c_str());
        return 1;
    }
    osvtool::Pipeline& P = *pipe.value();
    if (!P.attitude) {
        std::fprintf(stderr, "no attitude track\n");
        return 1;
    }
    for (const std::string& n : P.notes) {
        std::printf("note: %s\n", n.c_str());
    }
    std::ofstream csv(outDir + "/att.csv");
    csv << "frame,tUs,qw,qx,qy,qz,rotDeg,ax0x,ax0y,ax0z,ax1x,ax1y,ax1z\n";
    const osv::Quatd q0 = P.attitude->worldFromBody(frameTimeUs(P, 0));
    const osv::Vec3d a0 = P.rig.opticalAxisBody(0);
    const osv::Vec3d a1 = P.rig.opticalAxisBody(1);
    for (std::uint32_t f = 0; f < P.frameCount(); ++f) {
        const double t = frameTimeUs(P, f);
        const osv::Quatd q = P.attitude->worldFromBody(t);
        // Rotation relative to frame 0: q0^-1 q.
        const osv::Quatd r = q0.conj() * q;
        const double ang = 2.0 * std::acos(std::fmin(1.0, std::fabs(r.w))) * 57.29577951308232;
        // Lens axes in the frame-0 body frame (the world-fixed map's frame).
        const osv::Vec3d w0 = r.rotate(a0);
        const osv::Vec3d w1 = r.rotate(a1);
        char line[512];
        std::snprintf(line, sizeof(line), "%u,%.0f,%.7f,%.7f,%.7f,%.7f,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", f, t, q.w,
                      q.x, q.y, q.z, ang, w0.x, w0.y, w0.z, w1.x, w1.y, w1.z);
        csv << line;
    }
    std::printf("att.csv written (%u frames)\n", P.frameCount());
    return 0;
}

int runLens(const std::string& clip, const std::string& outDir, int mapW, bool stab, int argc, char** argv,
            int firstFrameArg) {
    auto pipe = openClip(clip, stab);
    if (!pipe.ok()) {
        std::fprintf(stderr, "open failed: %s\n", pipe.error().toString().c_str());
        return 1;
    }
    osvtool::Pipeline& P = *pipe.value();

    osv::geom::EquirectMap map;
    map.layout = osv::geom::EquirectLayout::PolarAxis;
    map.w = mapW;
    map.h = mapW / 2;

    // Native scene-linear light: identity matrices, like renderPhotoBands.
    OsvColorParams linear =
        osv::color::makeColorParams(osv::color::kDefaultDlogMFit, osv::color::OutputTransfer::Linear, 0.0f);
    for (int k = 0; k < 9; ++k) {
        const float id = (k % 4 == 0) ? 1.0f : 0.0f;
        linear.nativeToWorking.m[k] = id;
        linear.workingToOutput.m[k] = id;
    }
    // Occlusion only: alpha is the stick mask's factor, 1 up to thetaMax.
    osv::geom::BlendParams occl = P.blendParams;
    occl.featherDeg = 0.0;

    {
        std::ofstream meta(outDir + "/meta.json");
        meta << "{\"w\":" << map.w << ",\"h\":" << map.h << ",\"stab\":" << (stab ? 1 : 0)
             << ",\"lensFovDeg\":" << P.blendParams.lensFovDeg << "}\n";
    }

    for (int ai = firstFrameArg; ai < argc; ++ai) {
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
        const osv::Mat3d bfw = P.stabilizationFor(static_cast<std::uint32_t>(frame));
        OsvRenderParams params{};
        for (int lens = 0; lens < 2; ++lens) {
            osv::render::RenderParamsBuilder b;
            b.rig(P.rig).equirect(map).blend(occl, true).color(linear).alphaCoverage(true).stabilization(bfw);
            b.lensEnabled(lens, true).lensEnabled(1 - lens, false);
            auto job = b.build(pair.value());
            if (!job.ok()) {
                std::fprintf(stderr, "build failed: %s\n", job.error().toString().c_str());
                return 1;
            }
            params = job.value().params;
            auto img = P.renderer->render(job.value());
            if (!img.ok()) {
                std::fprintf(stderr, "render failed: %s\n", img.error().toString().c_str());
                return 1;
            }
            const auto& I = img.value();
            const std::string path = outDir + "/f" + std::to_string(frame) + "_lens" + std::to_string(lens) + ".f32";
            if (!writeRaw(path, I.data.data(), static_cast<std::size_t>(I.w) * I.h * 4u)) {
                return 1;
            }
        }
        // Each map pixel's angle from both lens axes, through the Rout the
        // kernel received (the stabilisation moves the lenses under the map).
        std::vector<float> th(static_cast<std::size_t>(map.w) * map.h * 2u);
        for (int y = 0; y < map.h; ++y) {
            for (int x = 0; x < map.w; ++x) {
                osv::Vec3d d;
                if (!map.pixelToDir(x + 0.5, y + 0.5, d)) {
                    continue;  // a pixel outside the map: its angles stay 0
                }
                const float* Ro = params.Rout;
                const float db[3] = {static_cast<float>(Ro[0] * d.x + Ro[1] * d.y + Ro[2] * d.z),
                                     static_cast<float>(Ro[3] * d.x + Ro[4] * d.y + Ro[5] * d.z),
                                     static_cast<float>(Ro[6] * d.x + Ro[7] * d.y + Ro[8] * d.z)};
                for (int L = 0; L < 2; ++L) {
                    const float* R = params.lens[L].R;
                    const float lx = R[0] * db[0] + R[1] * db[1] + R[2] * db[2];
                    const float ly = R[3] * db[0] + R[4] * db[1] + R[5] * db[2];
                    const float lz = R[6] * db[0] + R[7] * db[1] + R[8] * db[2];
                    th[(static_cast<std::size_t>(y) * map.w + x) * 2u + L] =
                        std::atan2(std::sqrt(lx * lx + ly * ly), lz) * 57.29577951f;
                }
            }
        }
        writeRaw(outDir + "/f" + std::to_string(frame) + "_theta.f32", th.data(), th.size());
        std::printf("frame %d done\n", frame);
    }
    return 0;
}

/// field <clip> <outdir> <mapW> <frame>: the photometric seam field of the
/// frame's own bucket (no temporal history), then the polar map rendered
/// with it (RimAndGain, neutral global gain, the analysis blend) three ways:
/// blended, lens 0 alone and lens 1 alone (alpha = that lens's rim-limited
/// weight), in native linear light.  Also dumps the field's rim and gain.
int runField(const std::string& clip, const std::string& outDir, int mapW, int frame) {
    auto pipe = openClip(clip, false);
    if (!pipe.ok()) {
        std::fprintf(stderr, "open failed: %s\n", pipe.error().toString().c_str());
        return 1;
    }
    osvtool::Pipeline& P = *pipe.value();
    auto pair = P.reader->read(static_cast<std::uint32_t>(frame));
    if (!pair.ok()) {
        std::fprintf(stderr, "decode failed\n");
        return 1;
    }
    osv::render::PhotoSeamParams pp;
    auto field = osv::render::measurePhotoSeam(P.rig, pair.value(), P.blendParams, pp, *P.pool);
    if (!field.ok()) {
        std::fprintf(stderr, "field refused: %s\n", field.error().toString().c_str());
        return 1;
    }
    const osv::render::PhotoSeamField& F = field.value();
    std::printf("field %ux%u lat %.3f..%.3f deg, rim median %.2f / %.2f, gain median %+.3f %+.3f %+.3f\n", F.w, F.h,
                F.latMinRad * 57.29578, F.latMaxRad * 57.29578, F.rimMedianDeg[0], F.rimMedianDeg[1],
                F.medianLog2Gain[0], F.medianLog2Gain[1], F.medianLog2Gain[2]);
    writeRaw(outDir + "/field_gain.f32", F.gain.data(), F.gain.size());
    writeRaw(outDir + "/field_rim.f32", F.rim.data(), F.rim.size());
    {
        std::ofstream meta(outDir + "/field.json");
        meta << "{\"w\":" << F.w << ",\"h\":" << F.h << ",\"latMin\":" << F.latMinRad << ",\"latMax\":" << F.latMaxRad
             << ",\"mapW\":" << mapW << "}\n";
    }
    osv::geom::EquirectMap map;
    map.layout = osv::geom::EquirectLayout::PolarAxis;
    map.w = mapW;
    map.h = mapW / 2;
    OsvColorParams linear =
        osv::color::makeColorParams(osv::color::kDefaultDlogMFit, osv::color::OutputTransfer::Linear, 0.0f);
    for (int k = 0; k < 9; ++k) {
        const float id = (k % 4 == 0) ? 1.0f : 0.0f;
        linear.nativeToWorking.m[k] = id;
        linear.workingToOutput.m[k] = id;
    }
    const char* tags[3] = {"blend", "lens0", "lens1"};
    for (int variant = 0; variant < 3; ++variant) {
        osv::render::RenderParamsBuilder b;
        b.rig(P.rig).equirect(map).blend(P.blendParams, true).color(linear).alphaCoverage(true);
        b.photo(F, pp);
        if (variant > 0) {
            b.lensEnabled(variant - 1, true).lensEnabled(2 - variant, false);
        }
        auto job = b.build(pair.value());
        if (!job.ok()) {
            std::fprintf(stderr, "build failed: %s\n", job.error().toString().c_str());
            return 1;
        }
        auto img = P.renderer->render(job.value());
        if (!img.ok()) {
            return 1;
        }
        const auto& I = img.value();
        writeRaw(outDir + "/fld_" + tags[variant] + ".f32", I.data.data(), static_cast<std::size_t>(I.w) * I.h * 4u);
    }
    std::printf("done\n");
    return 0;
}

}  // namespace

/// seamview <clip> <out.pgm> <w> <h> <fov> <distortion> <yaw> <pitch> <stab 0|1> <frame>: where the stitch
/// seam (polar latitude 0, the plane between the lens axes) runs in an
/// eye-offset view: white within 0.25 deg of it, grey where polar |lat| <
/// 7.6 deg (the overlap), black elsewhere.  Plain PGM for inspection.
int runSeamView(int argc, char** argv) {
    if (argc < 12) {
        return 2;
    }
    osvtool::PipelineOptions po;
    po.input = argv[2];
    po.device = "cpu";
    po.hw = "none";
    po.color = "linear";
    po.stab = std::atoi(argv[10]) != 0 ? "horizon" : "off";
    auto pipe = osvtool::Pipeline::open(po, false);
    if (!pipe.ok()) {
        std::fprintf(stderr, "open failed: %s\n", pipe.error().toString().c_str());
        return 1;
    }
    osvtool::Pipeline& P = *pipe.value();
    osv::geom::VirtualCamera cam;
    cam.w = std::atoi(argv[4]);
    cam.h = std::atoi(argv[5]);
    cam.projection = osv::geom::Projection::EyeOffset;
    cam.hfovDeg = std::atof(argv[6]);
    cam.eyeOffset = std::atof(argv[7]);
    cam.yawDeg = std::atof(argv[8]);
    cam.pitchDeg = std::atof(argv[9]);
    osv::render::RenderParamsBuilder b;
    b.rig(P.rig).camera(cam).color(P.color).stabilization(
        P.stabilizationFor(static_cast<std::uint32_t>(std::atoi(argv[11]))));
    auto params = b.buildParams();
    if (!params.ok()) {
        return 1;
    }
    const OsvRenderParams& p = params.value();
    std::vector<unsigned char> img(static_cast<std::size_t>(cam.w) * cam.h, 0);
    for (int y = 0; y < cam.h; ++y) {
        for (int x = 0; x < cam.w; ++x) {
            float dv[3];
            if (!osvRayForPixel(&p, static_cast<float>(x), static_cast<float>(y), dv)) {
                continue;
            }
            float db[3];
            osvMat3MulVec(p.Rout, dv, db);
            const double lat = std::asin(std::fmax(-1.0, std::fmin(1.0, static_cast<double>(db[1])))) * 57.29577951;
            img[static_cast<std::size_t>(y) * cam.w + x] =
                std::fabs(lat) < 0.25 ? 255 : (std::fabs(lat) < 7.6 ? 90 : 0);
        }
    }
    std::ofstream f(argv[3], std::ios::binary);
    f << "P5\n" << cam.w << " " << cam.h << "\n255\n";
    f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    std::printf("seam map written\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "seamview") {
        return runSeamView(argc, argv);
    }
    if (argc >= 6 && std::string(argv[1]) == "field") {
        return runField(argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]));
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: vigprobe att <clip> <outdir>\n"
                             "       vigprobe lens <clip> <outdir> <mapW> <stab 0|1> <frame> [frame...]\n");
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "att") {
        return runAtt(argv[2], argv[3]);
    }
    if (mode == "lens" && argc >= 7) {
        const int mapW = std::atoi(argv[4]);
        if (mapW < 256 || mapW > 8192 || (mapW % 2) != 0) {
            std::fprintf(stderr, "bad mapW\n");
            return 2;
        }
        return runLens(argv[2], argv[3], mapW, std::atoi(argv[5]) != 0, argc, argv, 6);
    }
    std::fprintf(stderr, "bad arguments\n");
    return 2;
}
