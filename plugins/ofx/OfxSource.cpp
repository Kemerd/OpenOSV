// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxSource.cpp - the OpenOSV Source generator (OfxSource.h).

#include "OfxSource.h"

#include "OfxCamera.h"
#include "OfxCuda.h"
#include "OfxFileDialog.h"
#include "OfxRender.h"
#include "OfxSourceParams.h"

#include "HostContext.h"
#include "ImporterInstance.h"
#include "PluginLog.h"
#include "PrefsBlob.h"
#include "ReframeCpu.h"
#include "SourceSettingsMapping.h"

#include "osv/meta/FormatInfo.h"
#include "osv/meta/Types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace osv::ofx::source {

using osv::premiere::HostContext;
using osv::premiere::ImporterInstance;
using osv::premiere::OutputGeometry;
using osv::premiere::PluginLog;
using osv::premiere::PrefsBlob;
using osv::premiere::RenderPurpose;

namespace {

// ===========================================================================
//  The clip cache
//
//  An ImporterInstance is heavy - a mapped container, a decoder, per-clip
//  analyses that take seconds to measure - and a Resolve timeline easily
//  holds the same .OSV many times over: every razor cut of a generator is a
//  new effect instance.  So instances are shared by (file, settings): cuts
//  with the same Source settings share one engine and its analyses, while a
//  cut with different settings gets its own, and never makes the other one
//  re-measure.
//
//  The cache holds weak references; the generator instances hold the strong
//  ones.  A clip no instance uses any more is destroyed with the last
//  instance that did, and its entry is swept on the next lookup.
// ===========================================================================

/// The identity of one engine instance: the file and the settings blob.
struct ClipKey {
    std::wstring path;                         ///< Normalised, lower-cased.
    std::array<unsigned char, PrefsBlob::kSize> prefs{};  ///< The blob's bytes.

    [[nodiscard]] bool operator<(const ClipKey& o) const noexcept {
        if (path != o.path) {
            return path < o.path;
        }
        return prefs < o.prefs;
    }
};

std::mutex g_cacheMutex;
std::map<ClipKey, std::weak_ptr<ImporterInstance>> g_cache;

/// A path spelled the way two spellings of the same file agree on: absolute,
/// lexically normalised, lower-case (NTFS is case-insensitive).
[[nodiscard]] std::wstring normalisedPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec) {
        abs = path;
    }
    std::wstring s = abs.lexically_normal().wstring();
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

/// The engine for `path` with `prefs`, opened and ready to render; null with
/// `error` set when the file cannot be opened as an Osmo 360 clip.
[[nodiscard]] std::shared_ptr<ImporterInstance> acquireClip(const std::filesystem::path& path, const PrefsBlob& prefs,
                                                           std::string& error) {
    ClipKey key;
    key.path = normalisedPath(path);
    std::memcpy(key.prefs.data(), &prefs, PrefsBlob::kSize);

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        // Sweep the entries nobody holds any more.
        for (auto it = g_cache.begin(); it != g_cache.end();) {
            it = it->second.expired() ? g_cache.erase(it) : std::next(it);
        }
        auto found = g_cache.find(key);
        if (found != g_cache.end()) {
            if (std::shared_ptr<ImporterInstance> alive = found->second.lock()) {
                return alive;
            }
        }
    }

    // Open OUTSIDE the lock: parsing the container and building the rig takes
    // a moment, and another instance's lookup must not wait for it.
    auto clip = std::make_shared<ImporterInstance>(path);
    // Not one of Premiere's instances: its settings are not published to the
    // (Premiere-only) direct path registry.
    clip->setEngineOwned(true);
    // Seeded BEFORE open(), because open() builds the lens rig from the
    // calibration in force - exactly how the importer seeds a new clip.
    clip->seedStartingPrefs(prefs, std::string());
    const Status opened = clip->open();
    if (!opened.ok()) {
        error = opened.error().message;
        return nullptr;
    }
    clip->applyPrefs(&prefs, PrefsBlob::kSize);

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    // Another instance may have opened the same clip meanwhile; the first one
    // in wins, so both share it.
    auto found = g_cache.find(key);
    if (found != g_cache.end()) {
        if (std::shared_ptr<ImporterInstance> alive = found->second.lock()) {
            return alive;
        }
    }
    g_cache[key] = clip;
    PluginLog::info("ofx source: opened '{}' ({} frames at {:.3f} fps, {})", path.filename().string(),
                    clip->frameCount(), clip->fps(), meta::modeName(clip->format().mode));
    return clip;
}

// ===========================================================================
//  Per-instance state (kOfxPropInstanceData)
// ===========================================================================

/// What one generator instance keeps between actions.
struct Instance {
    std::mutex mutex;                           ///< Guards everything below.
    std::shared_ptr<ImporterInstance> clip;     ///< The engine it renders from.
    std::filesystem::path clipPath;             ///< The file `clip` was opened for.
    PrefsBlob clipPrefs = PrefsBlob::defaults();///< The settings `clip` was opened with.
    std::string lastProblem;                    ///< The last message shown, so it is shown once.
    bool loggedTiming = false;                  ///< The first render's timing has been logged.
};

[[nodiscard]] Instance* instanceOf(OfxImageEffectHandle effect) noexcept {
    return static_cast<Instance*>(getPointer(effectProps(effect), kOfxPropInstanceData));
}

/// UTF-8 path text -> a filesystem path.
[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& text) {
    std::u8string u8;
    u8.reserve(text.size());
    for (char c : text) {
        u8.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(u8);
}

/// The engine for this instance's file and settings, re-acquired when either
/// changed.  Null with `problem` set when there is nothing to render.
[[nodiscard]] std::shared_ptr<ImporterInstance> clipFor(Instance& inst, const std::string& pathText,
                                                       const PrefsBlob& prefs, std::string& problem) {
    if (pathText.empty()) {
        problem = "Choose an .OSV file for OpenOSV Source.";
        return nullptr;
    }
    const std::filesystem::path path = pathFromUtf8(pathText);
    std::lock_guard<std::mutex> lock(inst.mutex);
    if (inst.clip && inst.clipPath == path && std::memcmp(&inst.clipPrefs, &prefs, PrefsBlob::kSize) == 0) {
        return inst.clip;
    }
    std::string error;
    std::shared_ptr<ImporterInstance> clip = acquireClip(path, prefs, error);
    if (!clip) {
        problem = std::format("OpenOSV Source cannot open '{}': {}", path.filename().string(), error);
        return nullptr;
    }
    inst.clip = clip;
    inst.clipPath = path;
    inst.clipPrefs = prefs;
    return clip;
}

/// The Clip read-out: what the user needs to trim the generator to.
[[nodiscard]] std::string describeClip(const ImporterInstance& clip) {
    const double fps = clip.fps();
    const std::uint32_t frames = clip.frameCount();
    const double seconds = (fps > 0.0) ? static_cast<double>(frames) / fps : 0.0;
    const auto& format = clip.format();
    const char* mode = meta::modeName(format.mode);
    return std::format("{} - {} frames at {:.3f} fps ({:.2f} s) - {} - {}", mode, frames, fps, seconds,
                       meta::colorModeName(format.colorMode), clip.path().filename().string());
}

/// Show a problem once per distinct message (Resolve would otherwise pop the
/// same dialog on every frame of playback).
void reportOnce(OfxImageEffectHandle effect, Instance& inst, const std::string& problem) {
    {
        std::lock_guard<std::mutex> lock(inst.mutex);
        if (inst.lastProblem == problem) {
            return;
        }
        inst.lastProblem = problem;
    }
    PluginLog::warn("ofx source: {}", problem);
    postMessage(effect, kOfxMessageError, problem);
}

/// The settings blob the controls describe at `time`.
[[nodiscard]] PrefsBlob prefsAt(OfxParamSetHandle params, OfxTime time) noexcept {
    return premiere::sourcesettings::prefsFromControls(source_params::read(params, time));
}

/// Refresh the Clip read-out for the file the controls name.
void refreshClipInfo(OfxImageEffectHandle effect, OfxTime time) {
    OfxParamSetHandle params = effectParams(effect);
    Instance* inst = instanceOf(effect);
    if (!params || !inst) {
        return;
    }
    const std::string pathText = cleanPath(stringAt(params, kFile, time));
    if (pathText.empty()) {
        writeString(params, kClipInfo, "No clip chosen.");
        return;
    }
    std::string problem;
    std::shared_ptr<ImporterInstance> clip = clipFor(*inst, pathText, prefsAt(params, time), problem);
    writeString(params, kClipInfo, clip ? describeClip(*clip).c_str() : problem.c_str());
}

// ===========================================================================
//  Describe
// ===========================================================================

OfxStatus describe(OfxImageEffectHandle effect) noexcept {
    OfxPropertySetHandle props = effectProps(effect);
    if (!props) {
        return kOfxStatErrBadHandle;
    }
    setString(props, kOfxPropLabel, "OpenOSV Source");
    setString(props, kOfxImageEffectPluginPropGrouping, "OpenOSV");
    setString(props, kOfxPropPluginDescription,
              "A DJI Osmo 360 .OSV clip, stitched by OpenOSV: the full sphere, or a keyframable reframed view "
              "straight from the camera's own sphere.");
    setString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGenerator, 0);
    setString(props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);
    setInt(props, kOfxImageEffectPluginPropSingleInstance, 0);
    // One render at a time per instance: the engine behind an instance is
    // serialised anyway (one lock per clip), and its frame cache is too.
    setString(props, kOfxImageEffectPluginRenderThreadSafety, kOfxImageEffectRenderInstanceSafe);
    setInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0);
    setInt(props, kOfxImageEffectPropSupportsMultiResolution, 1);
    setInt(props, kOfxImageEffectPropSupportsTiles, 0);
    setInt(props, kOfxImageEffectPropTemporalClipAccess, 0);
    setInt(props, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0);
    // No GPU render declared: the stitch runs on the engine's own GPU
    // renderer and lands in host memory; the host uploads it.
    return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect) noexcept {
    const OfxImageEffectSuiteV1* es = suites().effect;
    if (!es || !es->clipDefine) {
        return kOfxStatErrMissingHostFeature;
    }
    OfxPropertySetHandle output = nullptr;
    if (es->clipDefine(effect, kOfxImageEffectOutputClipName, &output) != kOfxStatOK || !output) {
        return kOfxStatErrMissingHostFeature;
    }
    setString(output, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA);
    setInt(output, kOfxImageEffectPropSupportsTiles, 0);

    OfxParamSetHandle params = effectParams(effect);
    if (!params) {
        return kOfxStatErrBadHandle;
    }

    // ---- the clip ------------------------------------------------------------
    defineString(params, kFile,
                 {"OSV File", "The .OSV (or its .LRF proxy) to stitch. Paste a path, or use Choose .OSV File.",
                  nullptr, false},
                 kOfxParamStringIsFilePath, "");
    OfxPropertySetHandle button = nullptr;
    const OfxParameterSuiteV1* ps = suites().param;
    if (ps && ps->paramDefine &&
        ps->paramDefine(params, kOfxParamTypePushButton, kChooseFile, &button) == kOfxStatOK && button) {
        setString(button, kOfxPropLabel, "Choose .OSV File...");
        setString(button, kOfxParamPropHint, "Opens the Windows file browser.");
        setString(button, kOfxParamPropScriptName, kChooseFile);
    }
    defineString(params, kClipInfo,
                 {"Clip", "The chosen clip's length and format. Trim the generator to this length.", nullptr, false},
                 kOfxParamStringIsLabel, "No clip chosen.");
    defineChoice(params, kOutput,
                 {"Output",
                  "Reframed view points the camera below into the sphere. 360 equirect outputs the whole sphere "
                  "at the timeline's size, for a 2:1 timeline or a 360 export.",
                  nullptr, false},
                 kOutputItems, kOutputReframed);
    defineInt(params, kStartFrame,
              {"Start Frame", "The clip frame shown on the generator's first frame. Slides the clip under the cut.",
               nullptr, false},
              0, 0, 10000000);

    // ---- the camera, then the stitch ----------------------------------------
    camera::describe(params);
    source_params::describe(params);
    return kOfxStatOK;
}

// ===========================================================================
//  Instance lifetime and changes
// ===========================================================================

OfxStatus createInstance(OfxImageEffectHandle effect) noexcept {
    OfxPropertySetHandle props = effectProps(effect);
    if (!props) {
        return kOfxStatErrBadHandle;
    }
    auto* inst = new (std::nothrow) Instance();
    if (!inst) {
        return kOfxStatErrMemory;
    }
    if (!setPointer(props, kOfxPropInstanceData, inst)) {
        delete inst;
        return kOfxStatFailed;
    }
    camera::applyVisibility(effectParams(effect), 0.0);
    // The Clip read-out is NOT refreshed here: OpenFX allows parameter writes
    // only from kOfxActionInstanceChanged (and interacts), and a reopened
    // project restores the read-out's saved text with everything else.
    return kOfxStatOK;
}

OfxStatus destroyInstance(OfxImageEffectHandle effect) noexcept {
    OfxPropertySetHandle props = effectProps(effect);
    Instance* inst = instanceOf(effect);
    if (props) {
        setPointer(props, kOfxPropInstanceData, nullptr);
    }
    // Dropping the instance drops its reference to the engine; the engine
    // itself goes when the last cut using it does.
    delete inst;
    return kOfxStatOK;
}

OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    if (getString(inArgs, kOfxPropType) != kOfxTypeParameter) {
        return kOfxStatReplyDefault;
    }
    const std::string reason = getString(inArgs, kOfxPropChangeReason);
    const std::string name = getString(inArgs, kOfxPropName);
    const OfxTime time = getDouble(inArgs, kOfxPropTime);
    OfxParamSetHandle params = effectParams(effect);

    // The file changed - typed, pasted, restored or chosen - so the read-out
    // follows whatever the reason.
    if (name == kFile) {
        refreshClipInfo(effect, time);
        return kOfxStatOK;
    }
    if (reason != kOfxChangeUserEdited) {
        return kOfxStatReplyDefault;
    }
    if (name == kChooseFile) {
        const std::string current = stringAt(params, kFile, time);
        if (const std::optional<std::string> chosen = chooseOsvFile(current)) {
            EditGroup group(params, "Choose .OSV File");
            writeString(params, kFile, chosen->c_str());
            // Some hosts do not report our own write back as a change, so
            // the read-out is refreshed here as well.
            refreshClipInfo(effect, time);
        }
        return kOfxStatOK;
    }
    if (camera::instanceChanged(params, name.c_str(), time, camera::projectSize(effect))) {
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

// ===========================================================================
//  Render
// ===========================================================================

/// Copy the rows of a top-down RGBA float equirect `image` (exactly the
/// camera frame's size) into the OpenFX output over `window`.
void copyEquirect(const render::ImageRGBAf& image, const ClipImage& output, const OfxRectI& window,
                  const OfxRectI& frame, ThreadPool* pool) {
    const OfxRectI area = intersect(intersect(window, output.bounds), frame);
    // Output pixels outside the frame (a window wider than the RoD) are
    // transparent black.
    clearCpu(output, window);
    if (empty(area)) {
        return;
    }
    const std::size_t bytes = static_cast<std::size_t>(area.x2 - area.x1) * 16u;
    const auto copyRow = [&](std::size_t index) noexcept {
        const int y = area.y1 + static_cast<int>(index);
        const std::uint32_t imageRow = static_cast<std::uint32_t>(frame.y2 - 1 - y);
        const float* src = image.row(imageRow);
        if (!src) {
            return;
        }
        char* dst = static_cast<char*>(output.data) +
                    static_cast<std::ptrdiff_t>(y - output.bounds.y1) * static_cast<std::ptrdiff_t>(output.rowBytes) +
                    static_cast<std::ptrdiff_t>(area.x1 - output.bounds.x1) * 16;
        std::memcpy(dst, src + static_cast<std::ptrdiff_t>(area.x1 - frame.x1) * 4, bytes);
    };
    const std::size_t rows = static_cast<std::size_t>(area.y2 - area.y1);
    if (pool) {
        (void)pool->parallelRows(rows, 8, copyRow);
        return;
    }
    for (std::size_t r = 0; r < rows; ++r) {
        copyRow(r);
    }
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    Instance* inst = instanceOf(effect);
    OfxParamSetHandle params = effectParams(effect);
    if (!inst || !params) {
        return kOfxStatErrBadHandle;
    }
    const OfxTime time = getDouble(inArgs, kOfxPropTime);
    int w[4] = {0, 0, 0, 0};
    if (!getInts(inArgs, kOfxImageEffectPropRenderWindow, w, 4)) {
        return kOfxStatFailed;
    }
    const OfxRectI window{w[0], w[1], w[2], w[3]};
    double sx = 1.0;
    double sy = 1.0;
    renderScale(inArgs, sx, sy);

    // ---- the output image -------------------------------------------------------
    OfxImageClipHandle outputClip = clipHandle(effect, kOfxImageEffectOutputClipName);
    ClipImage output(outputClip, time);
    if (!output.valid()) {
        return kOfxStatFailed;
    }
    if (!output.isFloatRgba(true)) {
        PluginLog::oncef("ofx/source/format", PluginLog::Level::Error,
                         "ofx source: output image is '{}' '{}' - only 32-bit float RGBA is supported", output.depth,
                         output.components);
        return kOfxStatErrImageFormat;
    }
    OfxPropertySetHandle effectPropSet = effectProps(effect);
    const double par = getDouble(effectPropSet, kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.0);
    const OfxRectI frame = cameraFrame(outputClip, time, sx, sy, par, output.bounds);
    const int frameW = frame.x2 - frame.x1;
    const int frameH = frame.y2 - frame.y1;

    // ---- the clip ---------------------------------------------------------------
    const std::string pathText = cleanPath(stringAt(params, kFile, time));
    const PrefsBlob prefs = prefsAt(params, time);
    std::string problem;
    std::shared_ptr<ImporterInstance> clip = clipFor(*inst, pathText, prefs, problem);
    if (!clip) {
        clearCpu(output, window);
        if (!pathText.empty()) {
            reportOnce(effect, *inst, problem);
        }
        return kOfxStatOK;
    }

    // ---- which frame ------------------------------------------------------------
    OfxPropertySetHandle outputProps = nullptr;
    const OfxImageEffectSuiteV1* es = suites().effect;
    if (es && es->clipGetPropertySet) {
        (void)es->clipGetPropertySet(outputClip, &outputProps);
    }
    double range[2] = {0.0, 0.0};
    const bool haveRange = getDoubles(outputProps, kOfxImageEffectPropFrameRange, range, 2);
    double hostFps = getDouble(outputProps, kOfxImageEffectPropFrameRate, 0, 0.0);
    if (!(hostFps > 0.0)) {
        hostFps = getDouble(effectPropSet, kOfxImageEffectPropFrameRate, 0, 0.0);
    }
    const long long startFrame = intAt(params, kStartFrame, time, 0);
    const long long index = frameForTime(time, haveRange ? range[0] : 0.0, hostFps, clip->fps(), startFrame);
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        if (!inst->loggedTiming) {
            // What the host really passes a generator is not documented; the
            // first render of every instance records it, so a wrong mapping
            // can be diagnosed from the log alone.
            inst->loggedTiming = true;
            PluginLog::info("ofx source: first render of '{}': host '{}', time {}, output range {} [{}, {}], "
                            "host fps {}, clip fps {:.3f}, start frame {} -> clip frame {} of {}; frame {}x{}, "
                            "render scale {}x{}",
                            clip->path().filename().string(), hostName(), time, haveRange ? "known" : "unknown",
                            range[0], range[1], hostFps, clip->fps(), startFrame, index, clip->frameCount(), frameW,
                            frameH, sx, sy);
        }
    }
    if (index < 0 || index >= static_cast<long long>(clip->frameCount())) {
        clearCpu(output, window);  // before or past the clip: nothing to show
        return kOfxStatOK;
    }

    // ---- how carefully ----------------------------------------------------------
    // Playback may not wait for a per-bucket analysis; a paused frame and a
    // Deliver render must (the importer's RenderPurpose).  Draft quality skips
    // the seam search and the parallax correction, as it does in Premiere.
    const bool interactive = getInt(inArgs, kOfxImageEffectPropInteractiveRenderStatus, 0, 0) != 0;
    const bool draft = getInt(inArgs, kOfxImageEffectPropRenderQualityDraft, 0, 0) != 0;
    const RenderPurpose purpose = interactive ? RenderPurpose::Interactive : RenderPurpose::Exact;
    const int mode = intAt(params, kOutput, time, kOutputReframed);
    std::shared_ptr<ThreadPool> pool = HostContext::instance().threadPoolShared();

    // Leave the host's CUDA context current on this thread whatever the
    // engine's own GPU renderer does to it.
    cuda::CurrentContextGuard cudaGuard;
    std::lock_guard<std::mutex> clipLock(clip->lock());

    if (mode == kOutputEquirect) {
        // ---- the whole sphere, straight at the frame's size -----------------
        const OutputGeometry geometry{frameW, frameH};
        auto rendered = clip->renderFrame(static_cast<std::uint32_t>(index), geometry, draft, purpose);
        if (!rendered.ok() || !rendered.value()) {
            reportOnce(effect, *inst, "OpenOSV Source could not stitch frame " + std::to_string(index) + ": " +
                                          (rendered.ok() ? std::string("no image") : rendered.error().message));
            clearCpu(output, window);
            return kOfxStatFailed;
        }
        copyEquirect(*rendered.value(), output, window, frame, pool.get());
        return kOfxStatOK;
    }

    // ---- a camera into the native sphere ------------------------------------
    const OutputGeometry sphere = clip->geometryForLocked(prefs);
    if (!sphere.valid()) {
        clearCpu(output, window);
        return kOfxStatFailed;
    }
    auto rendered = clip->renderFrame(static_cast<std::uint32_t>(index), sphere, draft, purpose);
    if (!rendered.ok() || !rendered.value()) {
        reportOnce(effect, *inst, "OpenOSV Source could not stitch frame " + std::to_string(index) + ": " +
                                      (rendered.ok() ? std::string("no image") : rendered.error().message));
        clearCpu(output, window);
        return kOfxStatFailed;
    }
    const render::ImageRGBAf& image = *rendered.value();

    // The sphere as a reframe source: top-down rows, positive pitch, R G B A.
    reframe::ConstFrameView view;
    view.base = image.data.data();
    view.rowBytes = static_cast<std::int32_t>(image.w * 16u);
    view.width = static_cast<int>(image.w);
    view.height = static_cast<int>(image.h);
    view.layout = reframe::PixelLayout::Bgra32f;
    view.topDown = true;
    const reframe::Settings settings = camera::read(params, time);
    reframe::KernelSetup setup = reframe::buildParams(settings, view, frameW, frameH, camera::projectSize(effect));
    if (!setup.valid) {
        PluginLog::oncef("ofx/source/setup", PluginLog::Level::Warn, "ofx source: no camera for a {}x{} frame ({})",
                         frameW, frameH, reframe::setupRejectName(setup.reject));
        clearCpu(output, window);
        return kOfxStatFailed;
    }
    setup.source.isBgra = 0;  // the engine's image is R, G, B, A
    if (!renderReframeCpu(setup, output, window, frame, pool.get())) {
        return kOfxStatFailed;
    }
    return kOfxStatOK;
}

/// True when two action names are the same string.
[[nodiscard]] bool isAction(const char* action, const char* name) noexcept {
    return action && name && std::strcmp(action, name) == 0;
}

}  // namespace

// ===========================================================================
//  The entry point
// ===========================================================================

OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                    OfxPropertySetHandle outArgs) noexcept {
    (void)outArgs;
    try {
        OfxImageEffectHandle effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
        if (isAction(action, kOfxImageEffectActionRender)) {
            return render(effect, inArgs);
        }
        if (isAction(action, kOfxActionInstanceChanged)) {
            return instanceChanged(effect, inArgs);
        }
        if (isAction(action, kOfxActionCreateInstance)) {
            return createInstance(effect);
        }
        if (isAction(action, kOfxActionDestroyInstance)) {
            return destroyInstance(effect);
        }
        if (isAction(action, kOfxActionDescribe)) {
            return describe(effect);
        }
        if (isAction(action, kOfxImageEffectActionDescribeInContext)) {
            return describeInContext(effect);
        }
        if (isAction(action, kOfxImageEffectActionGetClipPreferences)) {
            // Every frame differs (it is a movie), and the picture carries
            // straight coverage alpha - the importer's own declaration.
            setInt(outArgs, kOfxImageEffectFrameVarying, 1);
            setString(outArgs, kOfxImageEffectPropPreMultiplication, kOfxImageUnPreMultiplied);
            return kOfxStatOK;
        }
        return kOfxStatReplyDefault;
    } catch (const std::bad_alloc&) {
        PluginLog::error("ofx source: out of memory in action '{}'", action ? action : "?");
        return kOfxStatErrMemory;
    } catch (const std::exception& e) {
        PluginLog::error("ofx source: exception in action '{}': {}", action ? action : "?", e.what());
        return kOfxStatFailed;
    } catch (...) {
        PluginLog::error("ofx source: exception in action '{}'", action ? action : "?");
        return kOfxStatFailed;
    }
}

void shutdown() noexcept {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.clear();
}

}  // namespace osv::ofx::source
