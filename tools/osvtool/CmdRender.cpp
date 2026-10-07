// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osvtool render: decode, stitch/reframe and write stills or an HDR MP4.
//
// Two engines produce the frames:
//
//   plugin   (the default) the clip engine of the Premiere and Resolve
//            plug-ins, ImporterInstance, compiled in without any Adobe or
//            OpenFX header.  A frame is exactly the frame Premiere shows for
//            a new clip with the same Source Settings: parallax grid, carved
//            seam, photometric field, lens shading, sun ghost removal, the
//            steady per-clip analyses and lens alignment, on the GPU where
//            there is one.  Options the command line does not give come from
//            the Source Settings defaults (or, with --use-user-defaults, from
//            the ones saved in Premiere).
//   classic  the research pipeline below: decode pair -> optional seam /
//            gain / parallax analysis -> build parameters -> renderer, every
//            analysis per frame and each one off unless asked for.  It keeps
//            the geometry-convention and blend options the plug-ins take
//            from the clip itself.
//
// Either way, writing runs on its own thread behind a bounded queue so
// encoding overlaps the next frame's decode and render.

#include "Commands.h"
#include "Pipeline.h"

// The plug-ins' clip engine and its (SDK-free) layer, compiled into osvtool
// by tools/osvtool/CMakeLists.txt with OSV_CLIP_ENGINE_WITHOUT_PREMIERE.
#include "HostContext.h"
#include "ImporterInstance.h"
#include "PluginLog.h"
#include "PrefsBlob.h"

#include "osv/core/Log.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/Presets.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/io/FfmpegPipe.h"
#include "osv/io/ImageWriter.h"
#include "osv/io/SphericalMetadata.h"
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace osvtool {

using namespace osv;

namespace {

struct RenderOptions {
    PipelineOptions pipeline;
    /// plugin (the plug-ins' clip engine, the default) | classic (the
    /// research pipeline).  See the file comment.
    std::string engine = "plugin";
    // ---- plugin engine only: Source Settings with no classic equivalent ----
    bool flare = true;            ///< Sun ghost removal (--flare / --no-flare).
    std::string parallaxGrid;     ///< follows | steady | auto (empty = the defaults').
    std::string lensAlign;        ///< off | auto (empty = the defaults').
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
    /// Write the coverage alpha as a fourth channel into still outputs
    /// (--alpha).  Off by default, so every existing render stays
    /// byte-identical; a video carries no alpha and says so.
    bool alpha = false;
    std::string ffmpeg;
    std::string codec = "hevc_nvenc";
    int crf = 18;
    bool noAudio = false;
    // ---- 360 metadata ---------------------------------------------------------
    /// Tag an equirect .mp4 / .mov as 360 video (Spherical Video V1 + V2) once
    /// ffmpeg has finished it.  On by default; --no-spherical-metadata turns
    /// it off.  Flat outputs (--mode reframe, equirect-polar, stills) never
    /// get it.
    bool sphericalMetadata = true;
    /// True when --spherical-metadata or --no-spherical-metadata was given,
    /// so an explicit request on a flat output can be reported, not ignored.
    bool sphericalMetadataGiven = false;
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
constexpr const char* kCliStab[] = {"off", "horizon", "full", "smooth", "smooth-horizon"};
constexpr const char* kCliCalib[] = {"auto", "native", "lens-guards", "underwater"};  // PrefsCalibrationChoice
constexpr const char* kCliFit[] = {"dji", "pocket3", "osmo360", "avata360"};
constexpr const char* kCliDevice[] = {"auto", "cpu", "cuda", "opencl"};
constexpr const char* kCliLook[] = {"dji", "standard"};
constexpr const char* kCliFlow[] = {"auto", "classical", "neural"};
constexpr const char* kCliPhoto[] = {"off", "rim", "full"};
constexpr const char* kCliShading[] = {"off", "auto"};  // [WP-VIGNETTE]
constexpr const char* kCliHdrPeak[] = {"1000", "600", "400", "203"};  // [WP-HDRPEAK] PrefsHdrPeak order
static_assert(std::size(kCliHdrPeak) == static_cast<std::size_t>(osv::premiere::PrefsHdrPeak::Count));
// [WP-HDRTONE] PrefsHdrTone order, spelled as osv::color::hdrToneName does.
constexpr const char* kCliHdrTone[] = {"aces-bright", "aces-detailed", "bt2408-natural", "bt2408-punchy",
                                       "bt2408-neutral"};
static_assert(std::size(kCliHdrTone) == static_cast<std::size_t>(osv::premiere::PrefsHdrTone::Count));
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
    if (!given("--hdr-peak")) {  // [WP-HDRPEAK]
        o.pipeline.hdrPeak = cliToken(kCliHdrPeak, p.hdrPeak);
    }
    if (!given("--tone")) {  // [WP-HDRTONE]
        o.pipeline.tone = cliToken(kCliHdrTone, p.hdrTone);
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
        log::info("--use-user-defaults: sun ghost removal is a stage of the plug-ins' engine; --engine classic "
                  "renders without it");
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

/// The frames --frame / --range / --all ask for, checked against the clip's
/// `total`.  kExitOk with [first, last] filled, or the exit code after the
/// message was printed.
int selectFrames(const RenderOptions& o, std::uint32_t total, std::uint32_t& first, std::uint32_t& last) {
    if (total == 0) {
        std::fprintf(stderr, "error: clip has no frames\n");
        return kExitInput;
    }
    first = 0;
    last = 0;
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
    return kExitOk;
}

/// The virtual camera of --mode reframe: --preset, --proj, --fov,
/// --distortion, --yaw / --pitch / --roll and --correction, at `w` x `h`.
/// Both engines frame with it, so a view is the same view whichever one
/// stitched it.  kExitOk with `cam` filled, or the exit code after the
/// message was printed.
int buildCamera(const RenderOptions& o, int w, int h, geom::VirtualCamera& cam) {
    cam = geom::VirtualCamera{};
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
    return kExitOk;
}

/// Where the frames go: an ffmpeg pipe for .mp4 / .mov, stills otherwise,
/// written on a thread of their own behind a bounded queue so encoding
/// overlaps the next frame's decode and render.  Both engines write
/// through it, so an output means the same thing whichever one rendered it.
class FrameSink {
public:
    FrameSink() = default;
    FrameSink(const FrameSink&) = delete;
    FrameSink& operator=(const FrameSink&) = delete;
    ~FrameSink() { stopWriter(); }

    /// Open the pipe (video) or pick the image format (stills) for frames of
    /// `w` x `h` at `fps` encoded with `transfer`; `pqPeakNits` is what a PQ
    /// still records as its peak.  Starts the writer thread.  kExitOk, or
    /// the exit code after the message was printed.
    int open(const RenderOptions& o, int w, int h, double fps, color::OutputTransfer transfer, float pqPeakNits,
             bool multi) {
        m_out = o.out;
        m_multi = multi;
        const bool toVideo = std::filesystem::path(o.out).extension() == ".mp4" ||
                             std::filesystem::path(o.out).extension() == ".mov";
        m_tag.rec2020 = transfer != color::OutputTransfer::Rec709;
        switch (transfer) {
        case color::OutputTransfer::HLG: m_tag.transfer = io::ImageTransfer::HLG; break;
        case color::OutputTransfer::PQ: m_tag.transfer = io::ImageTransfer::PQ; break;
        case color::OutputTransfer::Rec709: m_tag.transfer = io::ImageTransfer::Rec709; break;
        case color::OutputTransfer::Linear: m_tag.transfer = io::ImageTransfer::Linear; break;
        case color::OutputTransfer::Passthrough: m_tag.transfer = io::ImageTransfer::DLogM; break;
        }
        // [WP-HDRPEAK] A PQ still records the peak its highlights were rolled
        // off into (1000, the OOTF display, when --hdr-peak is left alone).
        if (transfer == color::OutputTransfer::PQ) {
            m_tag.peakNits = pqPeakNits;
        }
        if (toVideo) {
            if (transfer == color::OutputTransfer::Linear || transfer == color::OutputTransfer::Passthrough) {
                std::fprintf(stderr, "error: video output needs --color pq|hlg|709\n");
                return kExitUsage;
            }
            io::FfmpegPipeOptions fo;
            fo.ffmpegExe = o.ffmpeg;
            fo.width = static_cast<std::uint32_t>(w);
            fo.height = static_cast<std::uint32_t>(h);
            fo.fps = fps;
            fo.codec = o.codec;
            fo.crf = o.crf;
            fo.transfer = transfer == color::OutputTransfer::HLG      ? io::PipeTransfer::HLG
                          : transfer == color::OutputTransfer::Rec709 ? io::PipeTransfer::Rec709
                                                                      : io::PipeTransfer::PQ;
            if (!o.noAudio) {
                fo.audioSource = o.pipeline.input;
            }
            auto pw = io::FfmpegPipeWriter::open(fo, o.out);
            if (!pw.ok()) {
                std::fprintf(stderr, "error: %s\n", log::safe(pw.error().toString()).c_str());
                return kExitRuntime;
            }
            m_pipe.emplace(std::move(pw).value());
        }
        // 360 metadata: only a standard equirect is a 360 video a player can
        // map as it is.  The polar layout puts the lens axes at the poles
        // (neither V1 nor V2 'equi' describes that), a reframe is flat, and
        // stills are not MP4s.  An explicit request that cannot apply is
        // reported; --no-spherical-metadata is always accepted quietly.
        m_spherical = toVideo && o.sphericalMetadata && o.mode == "equirect";
        if (o.sphericalMetadataGiven && o.sphericalMetadata && !m_spherical) {
            const char* why = "--mode equirect-polar is not a standard 360 layout";
            if (!toVideo) {
                why = "only an .mp4 / .mov output carries it";
            } else if (o.mode == "reframe") {
                why = "--mode reframe writes flat video";
            }
            log::warn("--spherical-metadata ignored: {}", why);
        }
        m_imageFormat = io::formatFromExtension(o.out);
        if (!toVideo && m_imageFormat == io::ImageFormat::Exr && transfer != color::OutputTransfer::Linear) {
            log::warn("writing non-linear values into an EXR; use --color linear for scene-referred output");
        }
        // --alpha: the coverage alpha as a 4th channel.  Every still format
        // carries it (.exr float A, .tif RGBA 16-bit, .png RGBA 16-bit), and
        // it is what a host composites with - a transparent band shows black
        // over a black background while the RGB looks complete.  The pipe to
        // ffmpeg is RGB only, so a video says so instead of silently dropping it.
        if (o.alpha) {
            if (toVideo) {
                log::warn("--alpha ignored: a .mp4 / .mov output carries no alpha; write .exr, .tif or .png");
            } else {
                m_tag.includeAlpha = true;
            }
        }
        m_writer = std::thread([this] { writerLoop(); });
        return kExitOk;
    }

    /// Queue one frame (blocks while the queue is full).
    void push(std::uint32_t index, render::ImageRGBAf image) { m_queue.push(index, std::move(image)); }

    /// True once the writer has failed; the render loop stops feeding it.
    [[nodiscard]] bool failed() const noexcept { return m_failed.load(); }

    /// Drain the queue, stop the writer and close the pipe, then tag a
    /// finished equirect video as 360 video.  Returns `exitCode`, or
    /// kExitRuntime (after the message) when writing or tagging failed.
    int finish(int exitCode) {
        stopWriter();
        if (m_failed) {
            std::fprintf(stderr, "error: writer: %s\n", log::safe(m_error).c_str());
            exitCode = kExitRuntime;
        }
        const bool hadPipe = m_pipe.has_value();
        if (m_pipe) {
            Status st = m_pipe->close();
            if (!st.ok()) {
                std::fprintf(stderr, "error: %s\n", log::safe(st.error().toString()).c_str());
                exitCode = kExitRuntime;
            }
            m_pipe.reset();
        }
        // ---- 360 metadata, once ffmpeg has written the whole file ------------
        // Only a render that succeeded is tagged.  The rewrite goes through a
        // temporary file, so a failure leaves the finished video untouched.
        if (hadPipe && m_spherical && exitCode == kExitOk) {
            const Result<io::SphericalInjectReport> tagged = io::injectSphericalMetadata(m_out);
            if (!tagged.ok()) {
                std::fprintf(stderr,
                             "error: 360 metadata: %s (the video itself is complete; osvtool spherical retries the "
                             "tag)\n",
                             log::safe(tagged.error().toString()).c_str());
                exitCode = kExitRuntime;
            } else {
                log::info("360 metadata: {} tagged as equirectangular 360 video (Spherical Video V1 + V2)",
                          log::safe(m_out));
            }
        }
        return exitCode;
    }

private:
    void writerLoop() {
        std::uint32_t index = 0;
        render::ImageRGBAf image;
        while (m_queue.pop(index, image)) {
            Status st = okStatus();
            if (m_pipe) {
                st = m_pipe->writeFrame(image);
            } else {
                st = io::writeImage(outputPathFor(m_out, index, m_multi), image, m_imageFormat, m_tag);
            }
            if (!st.ok()) {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                m_error = st.error().toString();
                m_failed = true;
                m_queue.close();
                return;
            }
        }
    }

    void stopWriter() {
        m_queue.close();
        if (m_writer.joinable()) {
            m_writer.join();
        }
    }

    std::string m_out;
    bool m_multi = false;
    bool m_spherical = false;  ///< Tag the finished video as 360 video (see open()).
    io::ImageTag m_tag;
    io::ImageFormat m_imageFormat = io::ImageFormat::Png16;
    std::optional<io::FfmpegPipeWriter> m_pipe;
    FrameQueue m_queue{3};
    std::thread m_writer;
    std::atomic<bool> m_failed{false};
    std::string m_error;
    std::mutex m_errorMutex;
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
    std::uint32_t first = 0, last = 0;
    if (const int selected = selectFrames(o, P.frameCount(), first, last); selected != kExitOk) {
        return selected;
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
        if (const int built = buildCamera(o, w, h, cam); built != kExitOk) {
            return built;
        }
        builder.camera(cam);
    } else {
        std::fprintf(stderr, "error: unknown --mode '%s'\n", log::safe(mode).c_str());
        return kExitUsage;
    }

    // ---- output sink and its writer thread --------------------------------------------
    FrameSink sink;
    if (const int opened = sink.open(o, w, h, P.fps(), P.outputTransfer, color::hdrPeakNitsOf(P.color), multi);
        opened != kExitOk) {
        return opened;
    }

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
    for (std::uint32_t f = first; f <= last && !sink.failed(); ++f) {
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
        sink.push(f, std::move(img).value());

        if ((f - first) % 10 == 9 || f == last) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double done = static_cast<double>(f - first + 1);
            std::fprintf(stderr, "  %u/%u frames  %.1f fps  (%s)\n", f - first + 1, last - first + 1,
                         sec > 0 ? done / sec : 0.0, P.rendererName.c_str());
        }
    }
    return sink.finish(exitCode);
}

// ===========================================================================
//  --engine plugin: the plug-ins' clip engine
// ===========================================================================

/// The engine's log lines on osvtool's console (PluginLog::setMirror): the
/// clip engine logs through the plug-ins' logger, which osvtool never points
/// at a file, so nothing it does lands in the plug-ins' own log files.
void mirrorEngineLog(premiere::PluginLog::Level level, std::string_view text) noexcept {
    try {
        const std::string line = log::safe(text);
        switch (level) {
        case premiere::PluginLog::Level::Trace:
        case premiere::PluginLog::Level::Debug: log::debug("engine: {}", line); break;
        case premiere::PluginLog::Level::Info: log::info("engine: {}", line); break;
        case premiere::PluginLog::Level::Warn: log::warn("engine: {}", line); break;
        case premiere::PluginLog::Level::Error: log::error("engine: {}", line); break;
        case premiere::PluginLog::Level::Off:
        default: break;
        }
    } catch (...) {
        // A console line is never worth an exception into the engine.
    }
}

/// Installs the mirror for one render and takes it down again, whatever path
/// the render leaves by.
struct EngineLogScope {
    EngineLogScope() noexcept { premiere::PluginLog::setMirror(&mirrorEngineLog); }
    ~EngineLogScope() { premiere::PluginLog::setMirror(nullptr); }
    EngineLogScope(const EngineLogScope&) = delete;
    EngineLogScope& operator=(const EngineLogScope&) = delete;
};

/// Index of `token` in `tokens`, or -1.
template <std::size_t N>
[[nodiscard]] int tokenIndex(const char* const (&tokens)[N], std::string_view token) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (token == tokens[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// "a | b | c" for an error message.
template <std::size_t N>
[[nodiscard]] std::string tokenList(const char* const (&tokens)[N]) {
    std::string out;
    for (std::size_t i = 0; i < N; ++i) {
        out += (i == 0 ? "" : " | ");
        out += tokens[i];
    }
    return out;
}

/// Options of the classic pipeline that the plug-ins' engine takes from the
/// clip itself (the geometry conventions, the calibrated lens FOV and blend)
/// or has no use for.  Given with --engine plugin they are an error, never
/// silently ignored: a render that looks different from what was asked for
/// is worse than one that does not start.
constexpr const char* kClassicOnlyOptions[] = {
    "--protector",   "--stitch-distance", "--crop-scale",   "--focal-source",   "--extrinsic-order",
    "--extrinsic-sense", "--lens-fov",    "--feather",      "--occlusion",      "--blend",
    "--attitude-convention", "--smooth-sigma", "--input-encoding", "--hw",      "--threads",
    "--blend-fov",   "--blend-feather",   "--photo-decay",  "--seam-low-sigma", "--seam-interval",
};

/// The plugin engine's own options, which the classic pipeline has no use for.
constexpr const char* kPluginOnlyOptions[] = {"--flare", "--parallax-grid", "--lens-align"};

/// The Source Settings a plugin-engine render uses: the built-in defaults of
/// a new clip - or, with --use-user-defaults, the ones saved in Premiere -
/// with every option given on the command line on top.  `colorGiven` says
/// whether --color was one of them (otherwise an SDR recording keeps the
/// Rec.709 output the engine starts it with).  kExitOk, or kExitUsage after
/// the message was printed.
int enginePrefs(const RenderOptions& o, const CLI::App& sub, premiere::PrefsBlob& out, bool& colorGiven) {
    namespace pr = osv::premiere;
    const auto given = [&sub](const char* name) { return sub.count(name) > 0; };
    const auto usage = [](const std::string& message) {
        std::fprintf(stderr, "error: %s\n", log::safe(message).c_str());
        return kExitUsage;
    };

    // ---- where the settings start -------------------------------------------------
    if (o.useUserDefaults) {
        const pr::UserDefaults user = pr::currentUserDefaults();
        out = user.prefs;
        if (user.fromFile) {
            log::info("--use-user-defaults: {} - {}", pr::userDefaultsPathForLog(user.path),
                      pr::userDefaultsSummary(user.prefs));
        } else {
            log::info("--use-user-defaults: no usable defaults file ({}); using the built-in Source Settings "
                      "defaults",
                      user.path.empty() ? std::string("no location") : pr::userDefaultsPathForLog(user.path));
        }
    } else {
        out = pr::PrefsBlob::defaults();
    }
    if (!out.isValid()) {
        out = pr::PrefsBlob::defaults();
    }

    // ---- colour ---------------------------------------------------------------------
    colorGiven = given("--color");
    if (colorGiven) {
        color::OutputTransfer transfer = color::OutputTransfer::PQ;
        if (!color::parseOutputTransfer(o.pipeline.color, transfer)) {
            return usage("unknown --color '" + o.pipeline.color + "' (pq | hlg | 709 | dlogm)");
        }
        switch (transfer) {
        case color::OutputTransfer::PQ: out.colorOutput = static_cast<std::uint8_t>(pr::PrefsColorOutput::PQ); break;
        case color::OutputTransfer::HLG: out.colorOutput = static_cast<std::uint8_t>(pr::PrefsColorOutput::HLG); break;
        case color::OutputTransfer::Rec709:
            out.colorOutput = static_cast<std::uint8_t>(pr::PrefsColorOutput::Rec709);
            break;
        case color::OutputTransfer::Passthrough:
            out.colorOutput = static_cast<std::uint8_t>(pr::PrefsColorOutput::DLogM);
            break;
        case color::OutputTransfer::Linear:
        default:
            return usage("--color linear is a research output of the classic pipeline; add --engine classic");
        }
    }
    if (given("--look")) {
        color::Look look = color::kDefaultLook;
        if (!color::parseLook(o.pipeline.look, look)) {
            return usage("unknown --look '" + o.pipeline.look + "' (dji | standard)");
        }
        out.look = static_cast<std::uint8_t>(look == color::Look::Standard ? pr::PrefsLook::Standard
                                                                             : pr::PrefsLook::DjiStudio);
    }
    if (given("--hdr-peak")) {
        float nits = 0.0f;
        const int index = color::parseHdrPeak(o.pipeline.hdrPeak, nits)
                              ? tokenIndex(kCliHdrPeak, std::to_string(std::lround(nits)))
                              : -1;
        if (index < 0) {
            return usage("unknown --hdr-peak '" + o.pipeline.hdrPeak + "' (" + tokenList(kCliHdrPeak) + ")");
        }
        out.hdrPeak = static_cast<std::uint8_t>(index);
    }
    if (given("--tone")) {
        color::HdrTone tone = color::kDefaultHdrTone;
        const int index =
            color::parseHdrTone(o.pipeline.tone, tone) ? tokenIndex(kCliHdrTone, color::hdrToneName(tone)) : -1;
        if (index < 0) {
            return usage("unknown --tone '" + o.pipeline.tone + "' (" + tokenList(kCliHdrTone) + ")");
        }
        out.hdrTone = static_cast<std::uint8_t>(index);
    }
    if (given("--fit")) {
        color::DlogMFit fit = color::kDefaultDlogMFit;
        const int index =
            color::parseDlogMFit(o.pipeline.fit, fit) ? tokenIndex(kCliFit, color::dlogMFitName(fit)) : -1;
        if (index < 0) {
            return usage("--fit '" + o.pipeline.fit + "' is not a Source Settings curve (" + tokenList(kCliFit) +
                         "); add --engine classic to render with it");
        }
        out.dlogmFit = static_cast<std::uint8_t>(index);
    }
    if (given("--exposure")) {
        if (!std::isfinite(o.pipeline.exposureStops) || o.pipeline.exposureStops < pr::PrefsBlob::kMinExposureStops ||
            o.pipeline.exposureStops > pr::PrefsBlob::kMaxExposureStops) {
            return usage("--exposure must be within -6..6 stops");
        }
        out.exposureStops = static_cast<float>(o.pipeline.exposureStops);
    }

    // ---- stabilisation, calibration, device -----------------------------------------------
    if (given("--stab")) {
        const int index = tokenIndex(kCliStab, o.pipeline.stab);
        if (index < 0) {
            return usage("unknown --stab '" + o.pipeline.stab + "' (" + tokenList(kCliStab) + ")");
        }
        out.stabilization = static_cast<std::uint8_t>(index);
    }
    if (given("--calib")) {
        const int index = tokenIndex(kCliCalib, o.pipeline.calib);
        if (index < 0) {
            return usage("unknown --calib '" + o.pipeline.calib + "' (" + tokenList(kCliCalib) + ")");
        }
        out.setCalibrationChoice(static_cast<pr::PrefsCalibrationChoice>(index));
    }
    if (given("--device")) {
        const int index = tokenIndex(kCliDevice, o.pipeline.device);
        if (index < 0) {
            return usage("unknown --device '" + o.pipeline.device + "' (" + tokenList(kCliDevice) + ")");
        }
        out.renderDevice = static_cast<std::uint8_t>(index);
    }

    // ---- the stitch -------------------------------------------------------------------------
    // "Seam search" is both halves of the seam in the plug-ins: the disparity
    // correction and the carved seam.  --seam-carve therefore turns it on (it
    // is on by default already); --no-seam-search turns both off.
    if (given("--seam-search")) {
        out.seamSearch = o.seamSearch ? 1 : 0;
    }
    if (given("--seam-carve") && o.seamCarve) {
        out.seamSearch = 1;
    }
    if (given("--gain")) {
        out.gainMatch = o.gain ? 1 : 0;
    }
    if (given("--parallax")) {
        out.parallax = static_cast<std::uint8_t>(o.parallax ? pr::PrefsParallax::On : pr::PrefsParallax::Off);
    }
    if (given("--flow-backend")) {
        const int index = tokenIndex(kCliFlow, o.flowBackend);
        if (index < 0) {
            return usage("unknown --flow-backend '" + o.flowBackend + "' (" + tokenList(kCliFlow) + ")");
        }
        out.flowBackend = static_cast<std::uint8_t>(index);
    }
    if (given("--photo")) {
        const int index = tokenIndex(kCliPhoto, o.photo);
        if (index < 0) {
            return usage("unknown --photo '" + o.photo + "' (" + tokenList(kCliPhoto) + ")");
        }
        out.photoSeam = static_cast<std::uint8_t>(index);
    }
    if (given("--photo-strength")) {
        if (!(o.photoStrength >= 0.0 && o.photoStrength <= 1.0)) {
            return usage("--photo-strength must be within 0..1");
        }
        out.setPhotoStrengthPercent(100.0 * o.photoStrength);
    }
    if (given("--shading")) {
        const int index = tokenIndex(kCliShading, o.shading);
        if (index < 0) {
            return usage("unknown --shading '" + o.shading + "' (" + tokenList(kCliShading) + ")");
        }
        out.lensShading = static_cast<std::uint8_t>(index);
    }
    if (given("--shading-strength")) {
        if (!(o.shadingStrength >= 0.0 && o.shadingStrength <= 1.0)) {
            return usage("--shading-strength must be within 0..1");
        }
        out.setShadingStrengthPercent(100.0 * o.shadingStrength);
    }
    // The seam tools, in the ranges the Source Settings sliders offer.
    const auto inRange = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    const render::SeamTools& tools = o.seamTools;
    if (given("--seam-blend")) {
        if (!inRange(tools.seamBlendDeg, render::kMinSeamBlendDeg, render::kMaxSeamBlendDeg)) {
            return usage("--seam-blend must be within 0.2..8 degrees");
        }
        out.setSeamBlendDeg(tools.seamBlendDeg);
    }
    if (given("--parallax-blend")) {
        if (!inRange(tools.parallaxBlendDeg, 0.0, render::kMaxParallaxBlendDeg)) {
            return usage("--parallax-blend must be within 0..4 degrees");
        }
        out.setParallaxBlendDeg(tools.parallaxBlendDeg);
    }
    if (given("--seam-smoothing")) {
        if (!inRange(tools.smoothingDeg, 0.0, render::kMaxSeamSmoothingDeg)) {
            return usage("--seam-smoothing must be within 0..8 degrees");
        }
        out.setSeamSmoothingDeg(tools.smoothingDeg);
    }
    if (given("--near-offset")) {
        if (!inRange(tools.nearOffsetDeg, -render::kMaxSeamOffsetDeg, render::kMaxSeamOffsetDeg)) {
            return usage("--near-offset must be within -3..3 degrees");
        }
        out.setNearOffsetDeg(tools.nearOffsetDeg);
    }
    if (given("--far-offset")) {
        if (!inRange(tools.farOffsetDeg, -render::kMaxSeamOffsetDeg, render::kMaxSeamOffsetDeg)) {
            return usage("--far-offset must be within -3..3 degrees");
        }
        out.setFarOffsetDeg(tools.farOffsetDeg);
    }

    // ---- what only the plugin engine has --------------------------------------------------
    if (given("--flare")) {
        out.flareRemoval = o.flare ? 1 : 0;
    }
    if (given("--parallax-grid")) {
        constexpr const char* kGrid[] = {"follows", "steady", "auto"};  // PrefsParallaxGrid order
        static_assert(std::size(kGrid) == static_cast<std::size_t>(pr::PrefsParallaxGrid::Count));
        const int index = tokenIndex(kGrid, o.parallaxGrid);
        if (index < 0) {
            return usage("unknown --parallax-grid '" + o.parallaxGrid + "' (" + tokenList(kGrid) + ")");
        }
        out.parallaxGrid = static_cast<std::uint8_t>(index);
    }
    if (given("--lens-align")) {
        constexpr const char* kAlign[] = {"off", "auto"};  // PrefsLensAlign order
        static_assert(std::size(kAlign) == static_cast<std::size_t>(pr::PrefsLensAlign::Count));
        const int index = tokenIndex(kAlign, o.lensAlign);
        if (index < 0) {
            return usage("unknown --lens-align '" + o.lensAlign + "' (" + tokenList(kAlign) + ")");
        }
        out.lensAlign = static_cast<std::uint8_t>(index);
    }
    out.sanitise();
    return kExitOk;
}

/// OSV output transfer of a Source Settings colour output.
[[nodiscard]] color::OutputTransfer transferOf(premiere::PrefsColorOutput output) noexcept {
    switch (output) {
    case premiere::PrefsColorOutput::HLG: return color::OutputTransfer::HLG;
    case premiere::PrefsColorOutput::Rec709: return color::OutputTransfer::Rec709;
    case premiere::PrefsColorOutput::DLogM: return color::OutputTransfer::Passthrough;
    case premiere::PrefsColorOutput::PQ:
    case premiere::PrefsColorOutput::Count:
    default: return color::OutputTransfer::PQ;
    }
}

/// Render with the plug-ins' clip engine: every frame exactly as Premiere
/// (or Resolve) renders it for a new clip with these Source Settings.
int runEngineRender(const RenderOptions& o, const CLI::App& sub) {
    namespace pr = osv::premiere;
    const EngineLogScope logScope;

    // ---- what the plugin engine takes and what it does not ------------------------------
    for (const char* name : kClassicOnlyOptions) {
        if (sub.count(name) > 0) {
            std::fprintf(stderr,
                         "error: %s belongs to the classic pipeline; add --engine classic to use it (the default "
                         "engine is the plug-ins' own, which takes this from the clip)\n",
                         name);
            return kExitUsage;
        }
    }
    if (o.mode != "equirect" && o.mode != "reframe") {
        std::fprintf(stderr, "error: --mode %s needs --engine classic (the plug-ins render equirect and reframe)\n",
                     log::safe(o.mode).c_str());
        return kExitUsage;
    }

    // ---- the Source Settings ------------------------------------------------------------------
    pr::PrefsBlob prefs = pr::PrefsBlob::defaults();
    bool colorGiven = false;
    if (const int built = enginePrefs(o, sub, prefs, colorGiven); built != kExitOk) {
        return built;
    }

    // ---- the clip -------------------------------------------------------------------------------
    // Opened exactly as the plug-ins open a NEW clip: seeded with its starting
    // settings before open() (which builds the lens rig from the calibration
    // in force), then handed the settings to render with.  Its own starting
    // point decides the one thing the command line may leave to it: an SDR
    // recording keeps the Rec.709 output it starts with unless --color says
    // otherwise.
    auto clip = std::make_unique<pr::ImporterInstance>(o.pipeline.input);
    clip->setEngineOwned(true);
    clip->seedStartingPrefs(prefs, std::string());
    const Status opened = clip->open();
    if (!opened.ok()) {
        std::fprintf(stderr, "error: %s\n", log::safe(opened.error().toString()).c_str());
        return opened.error().code == ErrorCode::Io || opened.error().code == ErrorCode::Malformed ? kExitInput
                                                                                                    : kExitRuntime;
    }
    pr::PrefsBlob settings = clip->prefs();
    if (colorGiven) {
        settings.colorOutput = prefs.colorOutput;
    }
    clip->applyPrefs(&settings, pr::PrefsBlob::kSize);
    log::info("engine: plug-in clip engine, {} frames at {:.3f} fps", clip->frameCount(), clip->fps());

    // ---- frames -----------------------------------------------------------------------------------
    std::uint32_t first = 0, last = 0;
    if (const int selected = selectFrames(o, clip->frameCount(), first, last); selected != kExitOk) {
        return selected;
    }
    const bool multi = last > first;

    // ---- output geometry ------------------------------------------------------------------------
    // An equirect without --size is the Source Settings Output Size (Native,
    // 3840 x 1920, 2560 x 1280 or 1920 x 960), as Premiere sizes it.
    pr::OutputGeometry geometry;
    if (o.mode == "equirect" && sub.count("--size") == 0) {
        geometry = clip->geometryFor(settings);
    } else {
        int w = 0, h = 0;
        if (!parseSize(o.size, w, h)) {
            std::fprintf(stderr, "error: --size must be WxH\n");
            return kExitUsage;
        }
        geometry.width = w;
        geometry.height = h;
        if (o.mode == "reframe") {
            geom::VirtualCamera cam;
            if (const int built = buildCamera(o, w, h, cam); built != kExitOk) {
                return built;
            }
            geometry.view = cam;
        }
    }
    if (!geometry.valid()) {
        std::fprintf(stderr, "error: no usable output size for this clip\n");
        return kExitUsage;
    }

    // ---- output sink ------------------------------------------------------------------------------
    const OsvColorParams frameColor = clip->colorParams();
    const color::OutputTransfer transfer = transferOf(settings.color());
    FrameSink sink;
    if (const int sinkOpened = sink.open(o, geometry.width, geometry.height, clip->fps(), transfer,
                                         color::hdrPeakNitsOf(frameColor), multi);
        sinkOpened != kExitOk) {
        return sinkOpened;
    }

    // ---- main loop ----------------------------------------------------------------------------------
    // Exact renders: every analysis a frame needs is made before it is drawn,
    // as for a Premiere export.  The frame is copied out of the engine's
    // frame cache under the clip's lock, because the next render reuses it.
    const auto t0 = std::chrono::steady_clock::now();
    int exitCode = kExitOk;
    for (std::uint32_t f = first; f <= last && !sink.failed(); ++f) {
        render::ImageRGBAf frame;
        {
            std::lock_guard<std::mutex> lock(clip->lock());
            auto rendered = clip->renderFrame(f, geometry, false, pr::RenderPurpose::Exact);
            if (!rendered.ok() || rendered.value() == nullptr) {
                std::fprintf(stderr, "error: frame %u: %s\n", f,
                             rendered.ok() ? "no image" : log::safe(rendered.error().toString()).c_str());
                exitCode = kExitRuntime;
                break;
            }
            frame = *rendered.value();
        }
        sink.push(f, std::move(frame));

        if ((f - first) % 10 == 9 || f == last) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            const double done = static_cast<double>(f - first + 1);
            std::fprintf(stderr, "  %u/%u frames  %.1f fps  (%s)\n", f - first + 1, last - first + 1,
                         sec > 0 ? done / sec : 0.0, clip->rendererName().c_str());
        }
    }
    exitCode = sink.finish(exitCode);

    // The clip (its decoders, analysis workers) before the renderers it
    // leased, while the GPU runtime is still there.
    clip.reset();
    if (pr::HostContext::exists()) {
        pr::HostContext::shutdown();
    }
    return exitCode;
}

}  // namespace

void registerRenderCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<RenderOptions>();
    CLI::App* sub = app.add_subcommand("render", "Reframe or stitch frames to stills or an HDR MP4");
    addPipelineOptions(sub, opt->pipeline);

    // Which engine renders (see the file comment).
    auto* engine = sub->add_option_group("Engine");
    engine
        ->add_option("--engine", opt->engine,
                     "plugin (default): the Premiere / Resolve plug-ins' own clip engine - every frame as Premiere "
                     "renders a new clip, with its Source Settings defaults for whatever is not given here | "
                     "classic: the research pipeline, every analysis off unless asked for")
        ->default_str("plugin")
        ->check(CLI::IsMember({"plugin", "classic"}));
    engine->add_flag("--flare,!--no-flare", opt->flare, "plugin engine: sun ghost removal (Source Settings default: on)");
    engine->add_option("--parallax-grid", opt->parallaxGrid,
                       "plugin engine: follows (per moment) | steady (one correction for the clip) | auto (default)");
    engine->add_option("--lens-align", opt->lensAlign,
                       "plugin engine: off | auto (default; fit the small rotation between the lenses per clip)");

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
    outGeom->add_flag("--alpha", opt->alpha,
                      "Write the coverage alpha as a 4th channel into .exr / .tif / .png stills (default: RGB "
                      "only; videos carry no alpha)");
    outGeom->add_option("--ffmpeg", opt->ffmpeg, "ffmpeg executable for .mp4 output");
    outGeom->add_option("--codec", opt->codec, "Video encoder (hevc_nvenc, libx265, ...)")->default_str("hevc_nvenc");
    outGeom->add_option("--crf", opt->crf, "Quality (crf / cq)")->default_val(18);
    outGeom->add_flag("--no-audio", opt->noAudio, "Do not copy the source audio into the .mp4");
    // On by default: an equirect video without the tag plays as a flat
    // 2:1 picture on YouTube and in VR players.
    outGeom->add_flag("--spherical-metadata,!--no-spherical-metadata", opt->sphericalMetadata,
                      "Tag an --mode equirect .mp4 / .mov as 360 video (Spherical Video V1 + V2) for YouTube, VR "
                      "players and 360 editors (default: on; no effect on flat outputs)");

    // [WP-DEFAULTS] Opt-in only, so osvtool stays deterministic: a render
    // without this flag never reads the Premiere defaults file.
    sub->add_flag("--use-user-defaults", opt->useUserDefaults,
                  "Start from the Source Settings saved in Premiere as the default for new clips "
                  "(OPENOSV_DEFAULTS_FILE, else %APPDATA%\\OpenOSV\\defaults.json); options given here still win");

    sub->callback([opt, sub, &ctx]() {
        // A lookup or an allocation failing here must end the command with a
        // message, not escape through CLI11's parse().
        try {
            // Either spelling counts, as for the other negatable flags.
            opt->sphericalMetadataGiven = sub->count("--spherical-metadata") > 0;
            if (opt->engine == "plugin") {
                // The plug-ins' engine reads --use-user-defaults itself, as a
                // whole Source Settings blob (enginePrefs).
                ctx.exitCode = runEngineRender(*opt, *sub);
                return;
            }
            for (const char* name : kPluginOnlyOptions) {
                if (sub->count(name) > 0) {
                    std::fprintf(stderr, "error: %s is an option of the plug-ins' engine; drop --engine classic\n",
                                 name);
                    ctx.exitCode = kExitUsage;
                    return;
                }
            }
            if (opt->useUserDefaults) {
                applyUserDefaults(*opt, *sub);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", log::safe(e.what()).c_str());
            ctx.exitCode = kExitRuntime;
            return;
        }
        ctx.exitCode = runRender(*opt);
    });
}

}  // namespace osvtool
