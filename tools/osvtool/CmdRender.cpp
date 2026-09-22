// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osvtool render: decode, stitch/reframe and write stills or an HDR MP4.
//
// Pipeline per frame: decode pair -> optional seam/gain analysis -> build
// parameters -> renderer -> writer.  Writing runs on its own thread behind a
// bounded queue so encoding overlaps the next frame's decode and render.

#include "Commands.h"
#include "Pipeline.h"

#include "osv/core/Log.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/Presets.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/io/FfmpegPipe.h"
#include "osv/io/ImageWriter.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace osvtool {

using namespace osv;

namespace {

struct RenderOptions {
    PipelineOptions pipeline;
    int frame = -1;               ///< Single frame (-1 = use range/all)
    std::string range;            ///< "a-b"
    bool all = false;
    std::string mode = "reframe"; ///< reframe | equirect | equirect-polar
    std::string proj = "rectilinear";
    std::string preset;
    double fov = 90.0;
    bool fovSet = false;
    double distortion = 0.0;      ///< Eye offset d for --proj eye-offset (0..1)
    bool distortionSet = false;
    double yaw = 0.0, pitch = 0.0, roll = 0.0, correction = 0.0;
    std::string size = "1920x1080";
    bool seamSearch = false;
    bool parallax = false;         ///< 2-D flow-based parallax correction
    std::string flowBackend = "auto";
    bool gain = false;
    int seamInterval = 1;
    std::string out;
    std::string ffmpeg;
    std::string codec = "hevc_nvenc";
    int crf = 18;
    bool noAudio = false;
};

/// Expand an output pattern for a frame index: printf-style "%05d", or an
/// "_00000" suffix inserted before the extension for multi-frame runs.
std::filesystem::path outputPathFor(const std::string& pattern, std::uint32_t frame, bool multi) {
    if (pattern.find('%') != std::string::npos) {
        char buf[4096];
        std::snprintf(buf, sizeof(buf), pattern.c_str(), static_cast<int>(frame));
        return std::filesystem::path(buf);
    }
    std::filesystem::path p(pattern);
    if (!multi) {
        return p;
    }
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_%05u", frame);
    std::filesystem::path stem = p.stem();
    stem += suffix;
    stem += p.extension();
    return p.parent_path() / stem;
}

/// Bounded queue of rendered frames feeding the writer thread.
class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity) : m_capacity(capacity) {}

    void push(std::uint32_t index, render::ImageRGBAf image) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notFull.wait(lock, [&] { return m_items.size() < m_capacity || m_closed; });
        if (m_closed) {
            return;
        }
        m_items.emplace_back(index, std::move(image));
        m_notEmpty.notify_one();
    }

    bool pop(std::uint32_t& index, render::ImageRGBAf& image) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [&] { return !m_items.empty() || m_closed; });
        if (m_items.empty()) {
            return false;
        }
        index = m_items.front().first;
        image = std::move(m_items.front().second);
        m_items.pop_front();
        m_notFull.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_notEmpty.notify_all();
        m_notFull.notify_all();
    }

private:
    std::size_t m_capacity;
    std::deque<std::pair<std::uint32_t, render::ImageRGBAf>> m_items;
    std::mutex m_mutex;
    std::condition_variable m_notEmpty;
    std::condition_variable m_notFull;
    bool m_closed = false;
};

int runRender(const RenderOptions& o) {
    // ---- open the pipeline ------------------------------------------------------
    auto pipe = Pipeline::open(o.pipeline, true);
    if (!pipe.ok()) {
        std::fprintf(stderr, "error: %s\n", log::safe(pipe.value() ? "" : pipe.error().toString()).c_str());
        return pipe.error().code == ErrorCode::Io || pipe.error().code == ErrorCode::Malformed ? kExitInput
                                                                                                  : kExitRuntime;
    }
    Pipeline& P = *pipe.value();
    for (const std::string& n : P.notes) {
        log::info("{}", log::safe(n));
    }

    // ---- frame selection ----------------------------------------------------------
    const std::uint32_t total = P.frameCount();
    if (total == 0) {
        std::fprintf(stderr, "error: clip has no frames\n");
        return kExitInput;
    }
    std::uint32_t first = 0, last = 0;
    if (o.all) {
        last = total - 1;
    } else if (!o.range.empty()) {
        const std::size_t dash = o.range.find('-');
        try {
            first = static_cast<std::uint32_t>(std::stoul(o.range.substr(0, dash)));
            last = dash == std::string::npos ? first : static_cast<std::uint32_t>(std::stoul(o.range.substr(dash + 1)));
        } catch (...) {
            std::fprintf(stderr, "error: --range must be a-b\n");
            return kExitUsage;
        }
    } else {
        first = last = o.frame < 0 ? 0 : static_cast<std::uint32_t>(o.frame);
    }
    if (first > last || last >= total) {
        std::fprintf(stderr, "error: frame range %u-%u outside 0-%u\n", first, last, total - 1);
        return kExitUsage;
    }
    const bool multi = last > first;

    // ---- output geometry ------------------------------------------------------------
    int w = 0, h = 0;
    if (!parseSize(o.size, w, h)) {
        std::fprintf(stderr, "error: --size must be WxH\n");
        return kExitUsage;
    }
    render::RenderParamsBuilder builder;
    builder.rig(P.rig).color(P.color).blend(P.blendParams, o.pipeline.blend);
    geom::VirtualCamera cam;
    geom::EquirectMap map;
    const std::string mode = o.mode;
    if (mode == "equirect" || mode == "equirect-polar") {
        map.layout = mode == "equirect-polar" ? geom::EquirectLayout::PolarAxis : geom::EquirectLayout::Standard;
        map.w = w;
        map.h = h;
        builder.equirect(map);
    } else if (mode == "reframe") {
        cam.w = w;
        cam.h = h;
        if (!o.preset.empty()) {
            const geom::Preset* preset = geom::findPreset(o.preset);
            if (!preset) {
                std::fprintf(stderr, "error: unknown --preset '%s'\n", log::safe(o.preset).c_str());
                return kExitUsage;
            }
            geom::applyPreset(*preset, cam);
        } else {
            const std::string proj = o.proj;
            if (proj == "rectilinear") {
                cam.projection = geom::Projection::Rectilinear;
            } else if (proj == "fisheye") {
                cam.projection = geom::Projection::Fisheye;
            } else if (proj == "stereographic") {
                cam.projection = geom::Projection::Stereographic;
            } else if (proj == "eye-offset") {
                cam.projection = geom::Projection::EyeOffset;
            } else {
                std::fprintf(stderr, "error: unknown --proj '%s'\n", log::safe(proj).c_str());
                return kExitUsage;
            }
        }
        if (o.fovSet || o.preset.empty()) {
            cam.hfovDeg = o.fov;
        }
        // --distortion overrides the preset's eye offset; without a preset it
        // is the offset of --proj eye-offset (and ignored by the other models).
        if (o.distortionSet || o.preset.empty()) {
            if (!std::isfinite(o.distortion) || o.distortion < 0.0 || o.distortion > 1.0) {
                std::fprintf(stderr, "error: --distortion must be within 0..1\n");
                return kExitUsage;
            }
            cam.eyeOffset = o.distortion;
        }
        cam.yawDeg = o.yaw;
        cam.pitchDeg += o.pitch;
        cam.rollDeg = o.roll;
        cam.correctionAngleDeg = o.correction;
        if (!cam.isValid()) {
            std::fprintf(stderr, "error: invalid virtual camera (check --fov)\n");
            return kExitUsage;
        }
        // The eye-offset model silently clamps its FOV; tell the user when
        // the request was reduced so the framing is not a surprise.
        if (cam.projection == geom::Projection::EyeOffset && cam.effectiveHfovDeg() < cam.hfovDeg) {
            log::warn("--fov {:.1f} exceeds the eye-offset limit for distortion {:.2f}; using {:.1f} degrees",
                      cam.hfovDeg, cam.eyeOffset, cam.effectiveHfovDeg());
        }
        builder.camera(cam);
    } else {
        std::fprintf(stderr, "error: unknown --mode '%s'\n", log::safe(mode).c_str());
        return kExitUsage;
    }

    // ---- output sink --------------------------------------------------------------------
    const bool toVideo = std::filesystem::path(o.out).extension() == ".mp4" ||
                         std::filesystem::path(o.out).extension() == ".mov";
    std::optional<io::FfmpegPipeWriter> pipeWriter;
    io::ImageTag tag;
    tag.rec2020 = P.outputTransfer != color::OutputTransfer::Rec709;
    switch (P.outputTransfer) {
    case color::OutputTransfer::HLG: tag.transfer = io::ImageTransfer::HLG; break;
    case color::OutputTransfer::PQ: tag.transfer = io::ImageTransfer::PQ; break;
    case color::OutputTransfer::Rec709: tag.transfer = io::ImageTransfer::Rec709; break;
    case color::OutputTransfer::Linear: tag.transfer = io::ImageTransfer::Linear; break;
    case color::OutputTransfer::Passthrough: tag.transfer = io::ImageTransfer::DLogM; break;
    }
    if (toVideo) {
        if (P.outputTransfer == color::OutputTransfer::Linear || P.outputTransfer == color::OutputTransfer::Passthrough) {
            std::fprintf(stderr, "error: video output needs --color pq|hlg|709\n");
            return kExitUsage;
        }
        io::FfmpegPipeOptions fo;
        fo.ffmpegExe = o.ffmpeg;
        fo.width = static_cast<std::uint32_t>(w);
        fo.height = static_cast<std::uint32_t>(h);
        fo.fps = P.fps();
        fo.codec = o.codec;
        fo.crf = o.crf;
        fo.transfer = P.outputTransfer == color::OutputTransfer::HLG      ? io::PipeTransfer::HLG
                      : P.outputTransfer == color::OutputTransfer::Rec709 ? io::PipeTransfer::Rec709
                                                                           : io::PipeTransfer::PQ;
        if (!o.noAudio) {
            fo.audioSource = o.pipeline.input;
        }
        auto pw = io::FfmpegPipeWriter::open(fo, o.out);
        if (!pw.ok()) {
            std::fprintf(stderr, "error: %s\n", log::safe(pw.error().toString()).c_str());
            return kExitRuntime;
        }
        pipeWriter.emplace(std::move(pw).value());
    }
    const io::ImageFormat imageFormat = io::formatFromExtension(o.out);
    if (!toVideo && imageFormat == io::ImageFormat::Exr && P.outputTransfer != color::OutputTransfer::Linear) {
        log::warn("writing non-linear values into an EXR; use --color linear for scene-referred output");
    }

    // ---- writer thread ----------------------------------------------------------------
    FrameQueue queue(3);
    std::atomic<bool> writerFailed{false};
    std::string writerError;
    std::mutex writerErrorMutex;
    std::thread writer([&] {
        std::uint32_t index = 0;
        render::ImageRGBAf image;
        while (queue.pop(index, image)) {
            Status st = okStatus();
            if (pipeWriter) {
                st = pipeWriter->writeFrame(image);
            } else {
                st = io::writeImage(outputPathFor(o.out, index, multi), image, imageFormat, tag);
            }
            if (!st.ok()) {
                std::lock_guard<std::mutex> lock(writerErrorMutex);
                writerError = st.error().toString();
                writerFailed = true;
                queue.close();
                return;
            }
        }
    });

    // ---- flow backend selection ------------------------------------------------------------
    render::FlowBackendKind parallaxBackend = render::FlowBackendKind::Auto;
    if (o.flowBackend == "auto") {
        parallaxBackend = render::FlowBackendKind::Auto;
    } else if (o.flowBackend == "classical") {
        parallaxBackend = render::FlowBackendKind::Classical;
    } else if (o.flowBackend == "neural") {
        parallaxBackend = render::FlowBackendKind::Neural;
    } else {
        std::fprintf(stderr, "error: unknown --flow-backend '%s'\n", log::safe(o.flowBackend).c_str());
        return kExitUsage;
    }

    // ---- main loop ------------------------------------------------------------------------
    const auto t0 = std::chrono::steady_clock::now();
    int exitCode = kExitOk;
    std::vector<float> seamTable;
    render::ParallaxWarpGrid warpGrid;
    bool haveWarp = false;
    for (std::uint32_t f = first; f <= last && !writerFailed; ++f) {
        auto pair = P.reader->read(f);
        if (!pair.ok()) {
            std::fprintf(stderr, "error: frame %u: %s\n", f, log::safe(pair.error().toString()).c_str());
            exitCode = kExitRuntime;
            break;
        }
        // Optional per-frame analysis (every seamInterval frames).
        const bool analyse = (o.seamSearch || o.gain || o.parallax) &&
                             ((f - first) % static_cast<std::uint32_t>(std::max(1, o.seamInterval)) == 0);
        if (analyse && o.seamSearch) {
            render::SeamSearchParams sp;
            auto profile = render::searchSeam(P.rig, pair.value(), P.blendParams, sp, *P.pool);
            if (profile.ok()) {
                seamTable = profile.value().shiftDeg;
                log::debug("frame {}: seam meanNcc {:.3f}, accepted {}", f, profile.value().meanNcc,
                           profile.value().acceptedColumns);
            } else {
                log::warn("frame {}: seam search failed: {}", f, profile.error().message);
            }
        }
        if (analyse && o.gain) {
            render::BandParams band;
            auto g = render::estimateGain(P.rig, pair.value(), P.blendParams, band, *P.pool);
            if (g.ok()) {
                builder.gain(g.value().gain[0], g.value().gain[1]);
            }
        }
        if (o.seamSearch) {
            builder.seam(seamTable);
        }
        // 2-D parallax correction.  Measured on the residual left by the seam
        // table (which is why seamTable is passed in), so the two compose.
        //
        // A failure here is NOT fatal: on featureless content the flow cannot
        // be trusted and buildParallaxWarp says so, in which case the frame
        // renders with whatever correction was already in force rather than
        // with a field of repaired guesses.
        if (analyse && o.parallax) {
            render::ParallaxWarpParams pw;
            pw.backend = parallaxBackend;
            const std::vector<float>* seamIn = (o.seamSearch && !seamTable.empty()) ? &seamTable : nullptr;
            const auto tWarp0 = std::chrono::steady_clock::now();
            auto grid = render::buildParallaxWarp(P.rig, pair.value(), P.blendParams, pw, seamIn, *P.pool);
            const double warpMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tWarp0).count();
            if (grid.ok()) {
                warpGrid = std::move(grid).value();
                haveWarp = true;
                log::info("frame {}: parallax {} grid {}x{}, consistent {:.1f}%, mean {:.3f} deg, max {:.3f} deg, {:.0f} ms",
                          f, render::flowBackendName(warpGrid.usedBackend), warpGrid.w, warpGrid.h,
                          100.0 * warpGrid.consistentFraction(), warpGrid.meanAbsCorrectionDeg,
                          warpGrid.maxAbsCorrectionDeg, warpMs);
            } else {
                log::warn("frame {}: parallax correction unavailable ({}); rendering without it", f,
                          log::safe(grid.error().message));
            }
        }
        if (o.parallax && haveWarp) {
            builder.warp(warpGrid.uv, warpGrid.w, warpGrid.h, warpGrid.latMinRad, warpGrid.latMaxRad);
        }
        builder.stabilization(P.stabilizationFor(f));

        auto job = builder.build(pair.value());
        if (!job.ok()) {
            std::fprintf(stderr, "error: %s\n", log::safe(job.error().toString()).c_str());
            exitCode = kExitRuntime;
            break;
        }
        auto img = P.renderer->render(job.value());
        if (!img.ok()) {
            std::fprintf(stderr, "error: frame %u: %s\n", f, log::safe(img.error().toString()).c_str());
            exitCode = kExitRuntime;
            break;
        }
        queue.push(f, std::move(img).value());

        if ((f - first) % 10 == 9 || f == last) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double done = static_cast<double>(f - first + 1);
            std::fprintf(stderr, "  %u/%u frames  %.1f fps  (%s)\n", f - first + 1, last - first + 1,
                         sec > 0 ? done / sec : 0.0, P.rendererName.c_str());
        }
    }
    queue.close();
    writer.join();
    if (writerFailed) {
        std::fprintf(stderr, "error: writer: %s\n", log::safe(writerError).c_str());
        exitCode = kExitRuntime;
    }
    if (pipeWriter) {
        Status st = pipeWriter->close();
        if (!st.ok()) {
            std::fprintf(stderr, "error: %s\n", log::safe(st.error().toString()).c_str());
            exitCode = kExitRuntime;
        }
    }
    return exitCode;
}

}  // namespace

void registerRenderCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<RenderOptions>();
    CLI::App* sub = app.add_subcommand("render", "Reframe or stitch frames to stills or an HDR MP4");
    addPipelineOptions(sub, opt->pipeline);

    auto* sel = sub->add_option_group("Frames");
    sel->add_option("--frame", opt->frame, "Single frame index")->default_val(-1);
    sel->add_option("--range", opt->range, "Frame range a-b");
    sel->add_flag("--all", opt->all, "Every frame");

    auto* outGeom = sub->add_option_group("Output");
    outGeom->add_option("--mode", opt->mode, "reframe|equirect|equirect-polar")->default_str("reframe");
    outGeom->add_option("--proj", opt->proj, "rectilinear|fisheye|stereographic|eye-offset")->default_str("rectilinear");
    outGeom->add_option("--preset", opt->preset, "crystal-ball|asteroid|wide|ultra-wide|dewarping");
    outGeom->add_option("--fov", opt->fov, "Horizontal FOV in degrees")->default_val(90.0)->each([opt](const std::string&) {
        opt->fovSet = true;
    });
    outGeom
        ->add_option("--distortion", opt->distortion,
                     "Eye offset for --proj eye-offset: 0 = rectilinear, 1 = stereographic (overrides the preset)")
        ->default_val(0.0)
        ->each([opt](const std::string&) { opt->distortionSet = true; });
    outGeom->add_option("--yaw", opt->yaw, "Pan angle (deg)")->default_val(0.0);
    outGeom->add_option("--pitch", opt->pitch, "Tilt angle (deg)")->default_val(0.0);
    outGeom->add_option("--roll", opt->roll, "Roll angle (deg)")->default_val(0.0);
    outGeom->add_option("--correction", opt->correction, "Correction (horizon) angle (deg)")->default_val(0.0);
    outGeom->add_option("--size", opt->size, "Output size WxH")->default_str("1920x1080");
    outGeom->add_flag("--seam-search", opt->seamSearch, "Per-column seam disparity correction");
    outGeom->add_flag("--parallax", opt->parallax,
                      "2-D optical-flow parallax correction at the seam (fixes ghosting on close objects)");
    outGeom->add_option("--flow-backend", opt->flowBackend, "auto|classical|neural")->default_str("auto");
    outGeom->add_flag("--gain", opt->gain, "Exposure matching between lenses");
    outGeom->add_option("--seam-interval", opt->seamInterval, "Re-run the analyses every N frames")->default_val(1);
    outGeom->add_option("--out", opt->out, "Output: image (.png/.tif/.exr, %05d pattern) or .mp4")->required();
    outGeom->add_option("--ffmpeg", opt->ffmpeg, "ffmpeg executable for .mp4 output");
    outGeom->add_option("--codec", opt->codec, "Video encoder (hevc_nvenc, libx265, ...)")->default_str("hevc_nvenc");
    outGeom->add_option("--crf", opt->crf, "Quality (crf / cq)")->default_val(18);
    outGeom->add_flag("--no-audio", opt->noAudio, "Do not copy the source audio into the .mp4");

    sub->callback([opt, &ctx]() { ctx.exitCode = runRender(*opt); });
}

}  // namespace osvtool
