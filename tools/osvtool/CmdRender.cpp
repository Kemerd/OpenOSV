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
#include "osv/render/LensShading.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/SeamCarve.h"
#include "osv/render/SeamTools.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"

// [WP-DEFAULTS] The Premiere plug-ins' own (SDK-free) reader of the defaults
// file, compiled into osvtool by tools/osvtool/CMakeLists.txt.
#include "UserDefaults.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
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
    bool seamCarve = false;        ///< DP-carved seam with a narrow blend (SeamCarve.h)
    std::string flowBackend = "auto";
    bool gain = false;
    // [WP-PHOTO] The RENDER blend (the analyses keep --lens-fov / --feather).
    // Unset --blend-fov means the default seam edge inset: --lens-fov minus
    // 2 x render::kDefaultSeamInsetDeg (189.98 deg at the default 195.18).
    double blendFov = 0.0;
    bool blendFovSet = false;
    double blendFeather = osv::render::kSeamInsetFeatherDeg;
    // [WP-PHOTO] The photometric seam field, measured per bucket of frames
    // with the importer's own temporal filter (PhotoSeamHistory).
    std::string photo = "off";     ///< off | rim | full
    double photoStrength = 1.0;    ///< 0..1, the gain field only
    double photoDecay = 20.0;      ///< degrees beyond the overlap
    // [WP-VIGNETTE] The per-lens shading correction, measured per bucket of
    // frames with the importer's own temporal filter (LensShadingHistory).
    std::string shading = "off";   ///< off | auto
    double shadingStrength = 1.0;  ///< 0..1
    // [WP-SEAMTOOLS] The carved seam's tweaks (--seam-carve), exactly the
    // Source Settings controls of the same names; defaults = the seam as is.
    osv::render::SeamTools seamTools;
    double seamLowSigma = -1.0;    ///< Research: low-band blur sigma, degrees (< 0 = the default).
    int seamInterval = 1;
    std::string out;
    std::string ffmpeg;
    std::string codec = "hevc_nvenc";
    int crf = 18;
    bool noAudio = false;
    // ---- [WP-DEFAULTS] render --use-user-defaults ----------------------------
    /// Start from the Source Settings defaults saved in Premiere (see
    /// applyUserDefaults); off unless asked, so a plain render is the same
    /// on every machine whatever anybody saved there.
    bool useUserDefaults = false;
    /// The render blend's seam edge inset in degrees (the importer's
    /// PrefsBlob::seamInsetDeg); --blend-fov, when given, replaces it.
    double seamInsetDeg = osv::render::kDefaultSeamInsetDeg;
    /// Equirect modes only: take the output size from the defaults' Output
    /// Size "Native" (2 x decoded lens height), known once the clip is open.
    bool nativeEquirectSize = false;
};

// ---------------------------------------------------------------------------
//  [WP-DEFAULTS] --use-user-defaults
// ---------------------------------------------------------------------------

/// The osvtool spelling of each Source Settings enum, in enum order (the
/// same order as PrefsBlob.h, so the value indexes the list).
constexpr const char* kCliColor[] = {"pq", "hlg", "709", "dlogm"};
constexpr const char* kCliStab[] = {"off", "horizon", "full", "smooth"};
constexpr const char* kCliCalib[] = {"auto", "native", "lens-guards", "underwater"};  // PrefsCalibrationChoice
constexpr const char* kCliFit[] = {"dji", "pocket3", "osmo360"};
constexpr const char* kCliDevice[] = {"auto", "cpu", "cuda", "opencl"};
constexpr const char* kCliLook[] = {"dji", "standard"};
constexpr const char* kCliFlow[] = {"auto", "classical", "neural"};
constexpr const char* kCliPhoto[] = {"off", "rim", "full"};
constexpr const char* kCliShading[] = {"off", "auto"};  // [WP-VIGNETTE]
static_assert(std::size(kCliColor) == static_cast<std::size_t>(osv::premiere::PrefsColorOutput::Count));
static_assert(std::size(kCliStab) == static_cast<std::size_t>(osv::premiere::PrefsStabilization::Count));
static_assert(std::size(kCliCalib) == static_cast<std::size_t>(osv::premiere::PrefsCalibrationChoice::Count));
static_assert(std::size(kCliFit) == static_cast<std::size_t>(osv::premiere::PrefsDlogmFit::Count));
static_assert(std::size(kCliDevice) == static_cast<std::size_t>(osv::premiere::PrefsRenderDevice::Count));
static_assert(std::size(kCliLook) == static_cast<std::size_t>(osv::premiere::PrefsLook::Count));
static_assert(std::size(kCliFlow) == static_cast<std::size_t>(osv::premiere::PrefsFlowBackend::Count));
static_assert(std::size(kCliPhoto) == static_cast<std::size_t>(osv::premiere::PrefsPhotoSeam::Count));
static_assert(std::size(kCliShading) == static_cast<std::size_t>(osv::premiere::PrefsLensShading::Count));

/// The token for an enum byte; the list's first entry for a value past it
/// (the blob is sanitised, so that is a guard, not a path).
template <std::size_t N>
[[nodiscard]] std::string cliToken(const char* const (&tokens)[N], std::uint8_t value) {
    return tokens[value < N ? value : 0u];
}

/// Replace every Source Settings option the user did NOT give on the command
/// line with the value saved as the default for new clips in Premiere
/// (plugins/common/UserDefaults.h: OPENOSV_DEFAULTS_FILE, else
/// %APPDATA%\OpenOSV\defaults.json).  The file is a complete set - a key it
/// lacks is the importer's built-in value - so the result renders the clip
/// the way Premiere renders a NEW clip, except for what the command line
/// says.  Sun ghost removal and Program Monitor Colour have no osvtool
/// equivalent and are reported, not applied.
void applyUserDefaults(RenderOptions& o, const CLI::App& sub) {
    namespace pr = osv::premiere;
    const pr::UserDefaults user = pr::currentUserDefaults();
    const pr::PrefsBlob& p = user.prefs;
    // Given on the command line wins; everything else follows the file.
    const auto given = [&sub](const char* name) { return sub.count(name) > 0; };

    if (user.fromFile) {
        log::info("--use-user-defaults: {} - {}", pr::userDefaultsPathForLog(user.path), pr::userDefaultsSummary(p));
    } else {
        log::info("--use-user-defaults: no usable defaults file ({}); using the built-in Source Settings defaults",
                  user.path.empty() ? std::string("no location") : pr::userDefaultsPathForLog(user.path));
    }

    // ---- the pipeline's options -------------------------------------------------
    if (!given("--color")) {
        o.pipeline.color = cliToken(kCliColor, p.colorOutput);
    }
    if (!given("--look")) {
        o.pipeline.look = cliToken(kCliLook, p.look);
    }
    if (!given("--stab")) {
        o.pipeline.stab = cliToken(kCliStab, p.stabilization);
    }
    if (!given("--calib")) {
        o.pipeline.calib = cliToken(kCliCalib, static_cast<std::uint8_t>(p.calibrationChoice()));
    }
    if (!given("--fit")) {
        o.pipeline.fit = cliToken(kCliFit, p.dlogmFit);
    }
    if (!given("--exposure")) {
        o.pipeline.exposureStops = static_cast<double>(p.exposureStops);
    }
    if (!given("--device")) {
        o.pipeline.device = cliToken(kCliDevice, p.renderDevice);
    }

    // ---- the stitch -------------------------------------------------------------
    // The negated spellings (--no-seam-search, ...) count as given, so a
    // default can always be switched off for one render.
    if (!given("--seam-search")) {
        o.seamSearch = p.seamSearch != 0;
    }
    if (!given("--gain")) {
        o.gain = p.gainMatch != 0;
    }
    if (!given("--parallax")) {
        o.parallax = p.parallaxEnabled();
    }
    if (!given("--flow-backend")) {
        o.flowBackend = cliToken(kCliFlow, p.flowBackend);
    }
    if (!given("--photo")) {
        o.photo = cliToken(kCliPhoto, p.photoSeam);
    }
    if (!given("--photo-strength")) {
        o.photoStrength = p.photoStrengthPercent() / 100.0;
    }
    if (!given("--blend-fov")) {
        o.seamInsetDeg = p.seamInsetDeg();
    }
    // [WP-VIGNETTE] the lens shading correction.
    if (!given("--shading")) {
        o.shading = cliToken(kCliShading, p.lensShading);
    }
    if (!given("--shading-strength")) {
        o.shadingStrength = p.shadingStrengthPercent() / 100.0;
    }
    // [WP-SEAMTOOLS] the carved seam's tweaks (they act with --seam-carve).
    if (!given("--seam-blend")) {
        o.seamTools.seamBlendDeg = p.seamBlendDeg();
    }
    if (!given("--parallax-blend")) {
        o.seamTools.parallaxBlendDeg = p.parallaxBlendDeg();
    }
    if (!given("--seam-smoothing")) {
        o.seamTools.smoothingDeg = p.seamSmoothingDeg();
    }
    if (!given("--near-offset")) {
        o.seamTools.nearOffsetDeg = p.nearOffsetDeg();
    }
    if (!given("--far-offset")) {
        o.seamTools.farOffsetDeg = p.farOffsetDeg();
    }

    // ---- the equirect size -------------------------------------------------------
    // Output Size is the importer's EQUIRECT size; a reframe's --size is the
    // virtual camera's and stays as given.
    if ((o.mode == "equirect" || o.mode == "equirect-polar") && !given("--size")) {
        switch (p.size()) {
        case pr::PrefsOutputSize::UHD4K:   o.size = "3840x1920"; break;
        case pr::PrefsOutputSize::QHD2560: o.size = "2560x1280"; break;
        case pr::PrefsOutputSize::HD2K:    o.size = "1920x960"; break;
        case pr::PrefsOutputSize::Native:
        case pr::PrefsOutputSize::Count:
        default:                           o.nativeEquirectSize = true; break;
        }
    }

    // ---- what osvtool does not do ------------------------------------------------
    if (p.flareRemoval != 0) {
        log::info("--use-user-defaults: sun ghost removal is a Premiere importer stage; osvtool renders without it");
    }
}

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
    // The per-frame analyses shade bands from host planes, so they decide
    // whether a CUDA decode may keep its frames on the GPU.
    PipelineOptions pipelineOptions = o.pipeline;
    pipelineOptions.hostFramesRequired =
        o.seamSearch || o.gain || o.parallax || o.seamCarve || o.photo != "off" || o.shading != "off";
    auto pipe = Pipeline::open(pipelineOptions, true);
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
    // [WP-DEFAULTS] --use-user-defaults with Output Size "Native": the
    // importer's native equirect, 2 x the decoded lens height.
    std::string sizeText = o.size;
    if (o.nativeEquirectSize && P.format.lensH() > 0) {
        sizeText = std::to_string(2u * P.format.lensH()) + "x" + std::to_string(P.format.lensH());
    }
    if (!parseSize(sizeText, w, h)) {
        std::fprintf(stderr, "error: --size must be WxH\n");
        return kExitUsage;
    }
    // [WP-PHOTO] The render-only blend: the kernel's FOV feather ends a few
    // degrees inside the calibrated rim, where the sample's lens 0 is still
    // within a stop of its core, while every analysis below keeps
    // P.blendParams - narrowing those costs parallax quality.
    if (!(o.blendFeather >= 0.0) || o.blendFeather > 30.0) {
        std::fprintf(stderr, "error: --blend-feather must be within 0..30 degrees\n");
        return kExitUsage;
    }
    // The seam edge inset is the default one unless --use-user-defaults set
    // the saved Seam Edge Inset ([WP-DEFAULTS]).
    geom::BlendParams renderBlend = render::insetRenderBlend(P.blendParams, o.seamInsetDeg, o.blendFeather);
    if (o.blendFovSet) {
        if (!(o.blendFov > 90.0) || o.blendFov > P.blendParams.lensFovDeg) {
            std::fprintf(stderr, "error: --blend-fov must be above 90 and at most --lens-fov (%.2f)\n",
                         P.blendParams.lensFovDeg);
            return kExitUsage;
        }
        renderBlend = P.blendParams;
        renderBlend.lensFovDeg = o.blendFov;
        renderBlend.featherDeg = o.blendFeather;
    }
    log::debug("render blend: FOV {:.2f} deg, feather {:.2f} deg (analyses: {:.2f} / {:.2f})", renderBlend.lensFovDeg,
               renderBlend.featherDeg, P.blendParams.lensFovDeg, P.blendParams.featherDeg);
    render::RenderParamsBuilder builder;
    builder.rig(P.rig).color(P.color).blend(renderBlend, o.pipeline.blend);
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
    render::BlendSeam blendSeam;  // the carved seam in force (--seam-carve)
    // [WP-PHOTO] --photo: the parameters, the per-clip field history (EMA,
    // cross-fade, the rim median over the run of buckets - exactly the
    // importer's) and the global gain in force, which a full field replaces
    // and a refusal restores.
    render::PhotoSeamParams photoParams;
    if (o.photo == "rim") {
        photoParams.mode = render::PhotoSeamMode::RimOnly;
    } else if (o.photo == "full") {
        photoParams.mode = render::PhotoSeamMode::RimAndGain;
    } else if (o.photo == "off") {
        photoParams.mode = render::PhotoSeamMode::Off;
    } else {
        std::fprintf(stderr, "error: unknown --photo '%s' (off|rim|full)\n", log::safe(o.photo).c_str());
        return kExitUsage;
    }
    if (!(o.photoStrength >= 0.0 && o.photoStrength <= 1.0) || !(o.photoDecay >= 0.0 && o.photoDecay <= 90.0)) {
        std::fprintf(stderr, "error: --photo-strength must be within 0..1 and --photo-decay within 0..90\n");
        return kExitUsage;
    }
    photoParams.strength = o.photoStrength;
    photoParams.decayDeg = o.photoDecay;
    // [WP-VIGNETTE] --shading / --shading-strength.
    render::LensShadingParams shadingParams;
    if (o.shading == "auto") {
        shadingParams.mode = render::LensShadingMode::Auto;
    } else if (o.shading == "off") {
        shadingParams.mode = render::LensShadingMode::Off;
    } else {
        std::fprintf(stderr, "error: unknown --shading '%s' (off|auto)\n", log::safe(o.shading).c_str());
        return kExitUsage;
    }
    if (!(o.shadingStrength >= 0.0 && o.shadingStrength <= 1.0)) {
        std::fprintf(stderr, "error: --shading-strength must be within 0..1\n");
        return kExitUsage;
    }
    shadingParams.strength = o.shadingStrength;
    render::LensShadingHistory shadingHistory;
    // [WP-SEAMTOOLS] The ranges the Source Settings sliders offer.
    const render::SeamTools& tools = o.seamTools;
    const auto inRange = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!inRange(tools.seamBlendDeg, render::kMinSeamBlendDeg, render::kMaxSeamBlendDeg) ||
        !inRange(tools.parallaxBlendDeg, 0.0, render::kMaxParallaxBlendDeg) ||
        !inRange(tools.smoothingDeg, 0.0, render::kMaxSeamSmoothingDeg) ||
        !inRange(tools.nearOffsetDeg, -render::kMaxSeamOffsetDeg, render::kMaxSeamOffsetDeg) ||
        !inRange(tools.farOffsetDeg, -render::kMaxSeamOffsetDeg, render::kMaxSeamOffsetDeg)) {
        std::fprintf(stderr, "error: --seam-blend 0.2..8, --parallax-blend 0..4, --seam-smoothing 0..8 and "
                             "--near-offset / --far-offset -3..3 degrees\n");
        return kExitUsage;
    }
    render::SeamCarveParams carveParams;
    render::applySeamBlendWidths(tools, carveParams);
    render::PhotoSeamHistory photoHistory;
    Vec3d globalGain[2] = {Vec3d{1, 1, 1}, Vec3d{1, 1, 1}};
    bool haveBlendSeam = false;
    for (std::uint32_t f = first; f <= last && !writerFailed; ++f) {
        auto pair = P.reader->read(f);
        if (!pair.ok()) {
            std::fprintf(stderr, "error: frame %u: %s\n", f, log::safe(pair.error().toString()).c_str());
            exitCode = kExitRuntime;
            break;
        }
        // Optional per-frame analysis (every seamInterval frames).
        const bool analyse = (o.seamSearch || o.gain || o.parallax || o.seamCarve) &&
                             ((f - first) % static_cast<std::uint32_t>(std::max(1, o.seamInterval)) == 0);
        // 2-D parallax correction first.  When it yields a grid, the grid
        // REPLACES the seam table rather than composing with it - the same
        // policy as the Premiere importer, so this command's A/B shows the
        // picture a user actually gets there.  Measured on the sample clip
        // (whole-band overlap NCC, frames 0 / 32 / 64): table alone
        // 0.897 / 0.902 / 0.900, grid alone 0.916 / 0.918 / 0.921, table +
        // grid 0.906 / 0.902 / 0.909.  See ImporterInstance::renderFrame.
        //
        // A refused grid is NOT fatal: on featureless content the flow cannot
        // be trusted and buildParallaxWarp says so, and the seam table (when
        // --seam-search is on) becomes the fallback for that stretch.
        if (analyse && o.parallax) {
            render::ParallaxWarpParams pw;
            pw.backend = parallaxBackend;
            const auto tWarp0 = std::chrono::steady_clock::now();
            auto grid = render::buildParallaxWarp(P.rig, pair.value(), P.blendParams, pw, nullptr, *P.pool);
            const double warpMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tWarp0).count();
            if (grid.ok()) {
                warpGrid = std::move(grid).value();
                haveWarp = true;
                log::info("frame {}: parallax {} in {:.0f} ms (flow {:.0f}), consistent {:.1f}%, gated {}/{}, "
                          "disparity mean {:.3f} / max {:.3f} deg",
                          f, render::flowBackendName(warpGrid.usedBackend), warpMs, warpGrid.flowMs,
                          100.0 * warpGrid.consistentFraction(), warpGrid.gatedCells, warpGrid.measuredCells,
                          warpGrid.meanAbsCorrectionDeg, warpGrid.maxAbsCorrectionDeg);
            } else {
                haveWarp = false;
                log::warn("frame {}: parallax correction refused ({}){}", f, log::safe(grid.error().message),
                          o.seamSearch ? "; using the seam table" : "; rendering without it");
            }
        }
        const bool useWarp = o.parallax && haveWarp;

        if (analyse && o.seamSearch && !useWarp) {
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
        // [WP-VIGNETTE] The lens shading correction for this frame, FIRST:
        // the photometric field and the exposure match below are measured
        // on the corrected lenses, as the importer does.  One measurement
        // per bucket, then the bucket's model cross-faded from the previous.
        std::shared_ptr<const render::LensShadingModel> shadingFrame;
        if (shadingParams.mode != render::LensShadingMode::Off) {
            const std::uint32_t bucket = render::parallaxBucket(f);
            if (!shadingHistory.measured(bucket)) {
                auto model = render::measureLensShading(P.rig, pair.value(), P.blendParams, shadingParams, *P.pool);
                if (model.ok()) {
                    const render::LensShadingModel& m = model.value();
                    log::info("frame {}: lens shading in {:.1f} ms (bands {:.1f}); slave: {} sky columns, {} "
                              "sectors, peak {:+.4f} ({:+.3f} stop at {:.1f} deg); master: {} sky columns, {} "
                              "sectors, peak {:+.4f} ({:+.3f} stop at {:.1f} deg)",
                              f, m.bandMs + m.statsMs, m.bandMs, m.lens[0].skyColumns, m.lens[0].measuredSectors,
                              m.lens[0].peakAmount, m.lens[0].peakStops, m.lens[0].peakThetaDeg,
                              m.lens[1].skyColumns, m.lens[1].measuredSectors, m.lens[1].peakAmount,
                              m.lens[1].peakStops, m.lens[1].peakThetaDeg);
                    shadingHistory.store(bucket, std::make_shared<const render::LensShadingModel>(std::move(model).value()),
                                         shadingParams);
                } else {
                    log::warn("frame {}: lens shading refused ({}); rendering without it", f,
                              log::safe(model.error().message));
                    shadingHistory.store(bucket, nullptr, shadingParams);
                }
            }
            shadingFrame = shadingHistory.modelFor(f, shadingParams);
        }
        const render::LensShadingModel* shadingNow =
            (shadingFrame && shadingFrame->active()) ? shadingFrame.get() : nullptr;

        // [WP-PHOTO] The photometric seam field for this frame, chosen BEFORE
        // the carve like the importer does: its usable rim is the carved
        // seam's Rim cost (SeamPenaltySlot::Rim), in force on this thread
        // for the rest of the frame's analyses.  One measurement per bucket
        // (the analysis blend, never the inset render blend), then the
        // bucket's field cross-faded from the previous one.
        std::shared_ptr<const render::PhotoSeamField> photoFrame;
        if (photoParams.mode != render::PhotoSeamMode::Off) {
            const std::uint32_t bucket = render::parallaxBucket(f);
            if (!photoHistory.measured(bucket)) {
                auto field =
                    render::measurePhotoSeam(P.rig, pair.value(), P.blendParams, photoParams, *P.pool, shadingNow);
                if (field.ok()) {
                    const render::PhotoSeamField& pf = field.value();
                    log::info("frame {}: photometric seam field in {:.1f} ms (bands {:.1f}), trusted {:.1f}%, usable "
                              "rim {:.2f} / {:.2f} deg, median gain {:+.3f} / {:+.3f} / {:+.3f} stops",
                              f, pf.bandMs + pf.statsMs, pf.bandMs,
                              100.0 * static_cast<double>(pf.trustedPixels) / static_cast<double>(pf.bandPixels),
                              pf.rimMedianDeg[0], pf.rimMedianDeg[1], pf.medianLog2Gain[0], pf.medianLog2Gain[1],
                              pf.medianLog2Gain[2]);
                    photoHistory.store(bucket, std::make_shared<const render::PhotoSeamField>(std::move(field).value()),
                                       photoParams);
                } else {
                    log::warn("frame {}: photometric seam field refused ({}); keeping the inset and the global gain",
                              f, log::safe(field.error().message));
                    photoHistory.store(bucket, nullptr, photoParams);
                }
            }
            photoFrame = photoHistory.fieldFor(f, photoParams);
        }
        if (photoFrame) {
            render::installPhotoRimPenaltyHook();
        }
        const render::PhotoRimPenaltyScope photoRimScope(photoFrame ? &P.rig : nullptr, photoFrame);

        // The carved seam, through whichever correction is in force this
        // frame, steered by the previous one (frames render in order here).
        if (analyse && o.seamCarve) {
            render::WarpGridView warpView;
            render::SeamCorrection correction;
            if (useWarp) {
                warpView.uv = warpGrid.uv.data();
                warpView.w = warpGrid.w;
                warpView.h = warpGrid.h;
                warpView.latMinRad = warpGrid.latMinRad;
                warpView.latMaxRad = warpGrid.latMaxRad;
                correction.warp = &warpView;
            } else if (o.seamSearch && !seamTable.empty()) {
                correction.seamShiftDeg = &seamTable;
            }
            auto carved = render::carveSeam(P.rig, pair.value(), P.blendParams, render::ParallaxWarpParams{}.band,
                                            correction, carveParams, haveBlendSeam ? &blendSeam : nullptr, *P.pool);
            if (carved.ok()) {
                blendSeam = std::move(carved).value();
                haveBlendSeam = true;
                log::info("frame {}: seam carved in {:.1f} ms, latitude mean {:+.2f} / max {:.2f} deg, feather {:.2f} "
                          "deg mean, {} narrow / {} forced columns",
                          f, blendSeam.carveMs, blendSeam.meanLatDeg, blendSeam.maxAbsLatDeg,
                          blendSeam.meanHalfWidthDeg, blendSeam.narrowColumns, blendSeam.forcedColumns);
            } else {
                log::warn("frame {}: seam carve failed ({}); using the feather blend", f,
                          log::safe(carved.error().message));
            }
        }
        if (analyse && o.gain) {
            render::BandParams band;
            auto g = render::estimateGain(P.rig, pair.value(), P.blendParams, band, *P.pool, shadingNow);
            if (g.ok()) {
                globalGain[0] = g.value().gain[0];  // [WP-PHOTO] remembered for frames a field overrides
                globalGain[1] = g.value().gain[1];
            }
        }
        builder.gain(globalGain[0], globalGain[1]);
        // [WP-PHOTO] Applied, the field's rim replaces the inset and - in full
        // mode - its gain the global one.
        builder.clearPhoto();
        builder.blend(renderBlend, o.pipeline.blend);
        if (photoFrame) {
            builder.photo(*photoFrame, photoParams);
            builder.blend(P.blendParams, o.pipeline.blend);
            if (photoParams.mode == render::PhotoSeamMode::RimAndGain) {
                builder.gain(Vec3d{1, 1, 1}, Vec3d{1, 1, 1});
            }
        }
        // The builder persists across frames, so both corrections are set
        // explicitly every frame: whichever is in force, the other is off.
        if (useWarp) {
            builder.warp(warpGrid.uv, warpGrid.w, warpGrid.h, warpGrid.latMinRad, warpGrid.latMaxRad);
            builder.seam(std::vector<float>{});
        } else {
            builder.clearWarp();
            if (o.seamSearch) {
                builder.seam(seamTable);
            }
        }
        if (o.seamCarve && haveBlendSeam) {
            render::applyBlendSeam(builder, blendSeam);
            // [WP-SEAMTOOLS] Near / Far Offset into the warp grid in force,
            // Seam Smoothing on the seam - both no-ops at their defaults.
            if (tools.offsetOn()) {
                auto shifted = render::seamOffsetGrid(useWarp ? &warpGrid : nullptr, blendSeam, tools.nearOffsetDeg,
                                                      tools.farOffsetDeg);
                if (shifted.ok()) {
                    const render::ParallaxWarpGrid& g = shifted.value();
                    builder.warp(g.uv, g.w, g.h, g.latMinRad, g.latMaxRad);
                } else {
                    log::warn("frame {}: seam offset refused ({})", f, log::safe(shifted.error().message));
                }
            }
            if (tools.smoothingOn()) {
                builder.seamSmooth(tools.smoothingDeg, o.seamLowSigma);
            } else {
                builder.clearSeamSmooth();
            }
        } else {
            builder.clearBlendSeam();
            builder.clearSeamSmooth();  // [WP-SEAMTOOLS]
        }
        // [WP-VIGNETTE] set explicitly every frame (the builder persists).
        if (shadingNow) {
            builder.shading(*shadingNow, shadingParams.strength);
        } else {
            builder.clearShading();
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
    // The --no-* spellings exist for --use-user-defaults: they switch a saved
    // default off for one render.  Without that flag they change nothing.
    outGeom->add_flag("--seam-search,!--no-seam-search", opt->seamSearch, "Per-column seam disparity correction");
    outGeom->add_flag("--parallax,!--no-parallax", opt->parallax,
                      "2-D optical-flow parallax correction at the seam; when accepted it replaces --seam-search, "
                      "which remains the fallback");
    outGeom->add_flag("--seam-carve", opt->seamCarve,
                      "Carve the seam through the overlap by dynamic programming and blend narrowly along it "
                      "(no doubled near objects); composes with --parallax / --seam-search");
    outGeom->add_option("--flow-backend", opt->flowBackend, "auto|classical|neural")->default_str("auto");
    outGeom->add_flag("--gain,!--no-gain", opt->gain, "Exposure matching between lenses");
    outGeom
        ->add_option("--blend-fov", opt->blendFov,
                     "Render-blend lens FOV in degrees; the analyses keep --lens-fov (default: --lens-fov minus "
                     "5.2, the 2.6 deg seam edge inset)")
        ->each([opt](const std::string&) { opt->blendFovSet = true; });
    outGeom->add_option("--blend-feather", opt->blendFeather, "Render-blend feather in degrees (analyses: --feather)")
        ->default_val(osv::render::kSeamInsetFeatherDeg);
    outGeom->add_option("--photo", opt->photo,
                        "Photometric seam field: off | rim (per-longitude usable rim) | full (rim + 2-D gain; "
                        "replaces --gain)")
        ->default_str("off");
    outGeom->add_option("--photo-strength", opt->photoStrength, "Gain-field strength 0..1 (--photo full)")
        ->default_val(1.0);
    outGeom->add_option("--photo-decay", opt->photoDecay, "Gain-field decay beyond the overlap, degrees")
        ->default_val(20.0);
    // [WP-VIGNETTE] The Source Settings "Lens Shading" / "Shading Strength".
    outGeom->add_option("--shading", opt->shading,
                        "Lens shading correction: off | auto (each lens's rim structure measured from its own sky "
                        "and added back before the blend; the photometric field and --gain are then measured on the "
                        "corrected lenses)")
        ->default_str("off");
    outGeom->add_option("--shading-strength", opt->shadingStrength, "Lens shading strength 0..1 (--shading auto)")
        ->default_val(1.0);
    // [WP-SEAMTOOLS] The Source Settings seam tools (with --seam-carve).
    outGeom->add_option("--seam-blend", opt->seamTools.seamBlendDeg,
                        "Seam Blend: carved-seam feather where the lenses agree, degrees 0.2..8")
        ->default_val(osv::render::kDefaultSeamBlendDeg);
    outGeom->add_option("--parallax-blend", opt->seamTools.parallaxBlendDeg,
                        "Parallax Blend: feather where they disagree, degrees 0..4 (0 = hard cut)")
        ->default_val(osv::render::kDefaultParallaxBlendDeg);
    outGeom->add_option("--seam-smoothing", opt->seamTools.smoothingDeg,
                        "Seam Smoothing: two-band blend half width, degrees 0..8 (0 = off)")
        ->default_val(0.0);
    outGeom->add_option("--near-offset", opt->seamTools.nearOffsetDeg,
                        "Near Offset: shift along the seam where the lenses disagree, degrees -3..3")
        ->default_val(0.0);
    outGeom->add_option("--far-offset", opt->seamTools.farOffsetDeg,
                        "Far Offset: shift along the seam where they agree, degrees -3..3")
        ->default_val(0.0);
    outGeom->add_option("--seam-low-sigma", opt->seamLowSigma,
                        "Seam Smoothing low-band blur sigma, degrees (research; default: a third of the width)")
        ->default_val(-1.0);
    outGeom->add_option("--seam-interval", opt->seamInterval, "Re-run the analyses every N frames")->default_val(1);
    outGeom->add_option("--out", opt->out, "Output: image (.png/.tif/.exr, %05d pattern) or .mp4")->required();
    outGeom->add_option("--ffmpeg", opt->ffmpeg, "ffmpeg executable for .mp4 output");
    outGeom->add_option("--codec", opt->codec, "Video encoder (hevc_nvenc, libx265, ...)")->default_str("hevc_nvenc");
    outGeom->add_option("--crf", opt->crf, "Quality (crf / cq)")->default_val(18);
    outGeom->add_flag("--no-audio", opt->noAudio, "Do not copy the source audio into the .mp4");

    // [WP-DEFAULTS] Opt-in only, so osvtool stays deterministic: a render
    // without this flag never reads the Premiere defaults file.
    sub->add_flag("--use-user-defaults", opt->useUserDefaults,
                  "Start from the Source Settings saved in Premiere as the default for new clips "
                  "(OPENOSV_DEFAULTS_FILE, else %APPDATA%\\OpenOSV\\defaults.json); options given here still win");

    sub->callback([opt, sub, &ctx]() {
        if (opt->useUserDefaults) {
            // A lookup or an allocation failing here must end the command
            // with a message, not escape through CLI11's parse().
            try {
                applyUserDefaults(*opt, *sub);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "error: --use-user-defaults: %s\n", log::safe(e.what()).c_str());
                ctx.exitCode = kExitRuntime;
                return;
            }
        }
        ctx.exitCode = runRender(*opt);
    });
}

}  // namespace osvtool
