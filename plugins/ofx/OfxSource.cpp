// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxSource.cpp - the OpenOSV Source generator (OfxSource.h).
//
// One generator, two hosts:
//
//   * DaVinci Resolve (and any host but VEGAS): a 32-bit float RGBA output,
//     stated in the clip preferences, at full levels; instance-safe renders;
//     the "Choose .OSV File..." button, because Resolve's file field has no
//     Browse button - exactly as before VEGAS support existed;
//   * VEGAS Pro: an 8-bit or float output in R G B A or B G R A, in the depth
//     VEGAS picks for the project (the clip preferences leave it alone), at
//     the levels the VEGAS-only Output Levels control names; unsafe renders,
//     because VEGAS clones any safer plug-in once per render thread and every
//     clone would open its own decoder; and no Choose button, because VEGAS
//     gives the file field a Browse button of its own.
//
// Every difference keys on hostProfile() (OfxHost.h) and on nothing else.

#include "OfxSource.h"

#include "OfxCamera.h"
#include "OfxCuda.h"
#include "OfxFileDialog.h"
#include "OfxGpuView.h"
#include "OfxHostImage.h"
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

/// A path spelled the way two spellings of the same file agree on: absolute
/// and lexically normalised - and lower-case on Windows, where NTFS ignores
/// case.  Not on macOS: an APFS volume may be case-SENSITIVE, and folding
/// case there would give two different clips one engine.
[[nodiscard]] std::wstring normalisedPath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (ec) {
        abs = path;
    }
    std::wstring s = abs.lexically_normal().wstring();
#if defined(_WIN32)
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
#endif
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
    const HostProfile profile = hostProfile();
    setString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGenerator, 0);
    // Float only outside VEGAS; 8-bit and float, R G B A and B G R A in
    // VEGAS (declarePixelDepths() in OfxHost.cpp has the reasons).
    declarePixelDepths(props, profile);
    setInt(props, kOfxImageEffectPluginPropSingleInstance, 0);
    // One render at a time per instance: the engine behind an instance is
    // serialised anyway (one lock per clip), and its frame cache is too.
    //
    // VEGAS clones an instance- or fully-safe plug-in once per render
    // thread, and a clone of this one would open its own decoder and engine
    // for the same clip - several copies of the heaviest object in the
    // module, each re-measuring the clip's analyses.  Unsafe keeps VEGAS on
    // the one instance, whose engine already renders one frame at a time.
    setString(props, kOfxImageEffectPluginRenderThreadSafety,
              profile == HostProfile::Vegas ? kOfxImageEffectRenderUnsafe : kOfxImageEffectRenderInstanceSafe);
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
    const HostProfile profile = hostProfile();

    // ---- the clip ------------------------------------------------------------
    // The hint names the file picker the host really shows: our Choose
    // button, or - under VEGAS, where ours is hidden - VEGAS's own Browse
    // button on the file field.
    const char* fileHint = profile == HostProfile::Vegas
                               ? "The .OSV (or its .LRF proxy) to stitch. Paste a path, or use Browse."
                               : "The .OSV (or its .LRF proxy) to stitch. Paste a path, or use Choose .OSV File.";
    defineString(params, kFile, {"OSV File", fileHint, nullptr, false}, kOfxParamStringIsFilePath, "");
    OfxPropertySetHandle button = nullptr;
    const OfxParameterSuiteV1* ps = suites().param;
    if (ps && ps->paramDefine &&
        ps->paramDefine(params, kOfxParamTypePushButton, kChooseFile, &button) == kOfxStatOK && button) {
        setString(button, kOfxPropLabel, "Choose .OSV File...");
        setString(button, kOfxParamPropHint, "Opens the Windows file browser.");
        setString(button, kOfxParamPropScriptName, kChooseFile);
        // VEGAS gives the file field a Browse button of its own, so ours
        // would be a second button doing the same thing: defined (projects
        // and scripts may name it) but hidden.
        if (profile == HostProfile::Vegas) {
            setInt(button, kOfxParamPropSecret, 1);
        }
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

    // ---- the camera, then the stitch (and, in VEGAS, the output levels) ------
    camera::describe(params);
    source_params::describe(params, profile);
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
    // Only parameters: VEGAS also reports its clip "Output" changing, which
    // is nothing this generator supervises.
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
        // VEGAS names its main window; the dialog then belongs to it.  Other
        // hosts leave the property unset and the active window owns it.
        void* owner = getPointer(hostProperties(), kPropVegasHostHWnd);
        if (const std::optional<std::string> chosen = chooseOsvFile(current, owner)) {
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

/// A double pair property of `set` as "[a, b]", or "absent".
[[nodiscard]] std::string rangeText(OfxPropertySetHandle set, const char* name) {
    double values[2] = {0.0, 0.0};
    if (!getDoubles(set, name, values, 2)) {
        return "absent";
    }
    return std::format("[{}, {}]", values[0], values[1]);
}

/// A double property of `set` as text, or "absent".
[[nodiscard]] std::string doubleText(OfxPropertySetHandle set, const char* name) {
    double value = 0.0;
    if (!getDoubles(set, name, &value, 1)) {
        return "absent";
    }
    return std::format("{}", value);
}

/// A string property of `set`, quoted, or "absent".
[[nodiscard]] std::string stringText(OfxPropertySetHandle set, const char* name) {
    if (dimension(set, name) < 1) {
        return "absent";
    }
    return "'" + getString(set, name) + "'";
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
    const HostProfile profile = hostProfile();
    OfxImageClipHandle outputClip = clipHandle(effect, kOfxImageEffectOutputClipName);
    ClipImage output(outputClip, time);
    if (!output.valid()) {
        return kOfxStatFailed;
    }
    // Lenient: a generator's output may arrive unlabelled (Resolve), and the
    // host's own pitch then proves the format.
    const std::optional<HostImageView> outputView = output.view(profile, true);
    if (!outputView) {
        // The pitch and bounds too: they tell a mislabelled image apart from
        // a genuinely different format.
        PluginLog::oncef("ofx/source/format", PluginLog::Level::Error,
                         "ofx source: output image is '{}' '{}'{} ({}x{}, {} bytes per row{}) - only {} is supported",
                         output.depth, output.components, output.order.empty() ? "" : " '" + output.order + "'",
                         output.width(), output.height(), output.rowBytes,
                         output.rowBytesFromHost ? "" : ", pitch not reported", acceptedFormats(profile));
        return kOfxStatErrImageFormat;
    }
    // The levels every pixel of this render is packed in: always full range
    // outside VEGAS; the Output Levels control in VEGAS.
    const OutputLevels levels = source_params::outputLevelsAt(params, time, profile);
    OfxPropertySetHandle effectPropSet = effectProps(effect);
    const double par = getDouble(effectPropSet, kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.0);
    const OfxRectI frame = cameraFrame(outputClip, time, sx, sy, par, output.bounds);
    const int frameW = frame.x2 - frame.x1;
    const int frameH = frame.y2 - frame.y1;

    // Where the GPU path (OfxGpuView.h) writes when it takes the frame: the
    // host's output image as it really is - its depth and channel order -
    // the render window, the camera frame and the levels the output is
    // packed in.
    gpu::HostTarget gpuTarget;
    gpuTarget.image = *outputView;
    gpuTarget.window = window;
    gpuTarget.frame = frame;
    gpuTarget.levels = levels;

    // ---- the clip ---------------------------------------------------------------
    const std::string pathText = cleanPath(stringAt(params, kFile, time));
    const PrefsBlob prefs = prefsAt(params, time);
    std::string problem;
    std::shared_ptr<ImporterInstance> clip = clipFor(*inst, pathText, prefs, problem);
    if (!clip) {
        clearCpu(*outputView, window, levels);
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
            // Besides the timing: the output's format and levels, and what
            // the host says about the generator's extent - the unmapped
            // range, the effect's duration and, in VEGAS, where the instance
            // lives.  VEGAS documents its generator timing no better than
            // Resolve does.
            PluginLog::info("ofx source: first render of '{}': host '{}', time {}, output range {} [{}, {}], "
                            "unmapped range {}, effect duration {}, host fps {}, clip fps {:.3f}, start frame {} -> "
                            "clip frame {} of {}; frame {}x{}, render scale {}x{}; output {} {} at {} levels; "
                            "VEGAS context {}",
                            clip->path().filename().string(), hostName(), time, haveRange ? "known" : "unknown",
                            range[0], range[1], rangeText(outputProps, kOfxImageEffectPropUnmappedFrameRange),
                            doubleText(effectPropSet, kOfxImageEffectInstancePropEffectDuration), hostFps,
                            clip->fps(), startFrame, index, clip->frameCount(), frameW, frameH, sx, sy,
                            hostDepthName(outputView->depth), hostOrderName(outputView->order),
                            outputLevelsName(levels), stringText(effectPropSet, kPropVegasContext));
        }
    }
    if (index < 0 || index >= static_cast<long long>(clip->frameCount())) {
        clearCpu(*outputView, window, levels);  // before or past the clip: nothing to show
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
        // [WP-V-GPU] begin - stitched and packed on the GPU; only the finished
        // pixels cross the bus.  "Not mine" (no GPU path) falls through to
        // the CPU copy below, as does a GPU failure, logged once.
        {
            std::string gpuError;
            if (gpu::renderSourceEquirectGpu(*clip, static_cast<std::uint32_t>(index), draft, purpose, gpuTarget,
                                             gpuError)) {
                gpu::notePath(effect, gpu::Hook::SourceEquirect, true, gpuError);
                return kOfxStatOK;
            }
            // Once per instance: which path took its first frame, and why.
            gpu::notePath(effect, gpu::Hook::SourceEquirect, false, gpuError);
            if (!gpuError.empty()) {
                PluginLog::oncef("ofx/source/gpu-equirect", PluginLog::Level::Warn,
                                 "ofx source: GPU sphere path failed, using the CPU copy: {}", gpuError);
            }
        }
        // [WP-V-GPU] end
        const OutputGeometry geometry{frameW, frameH};
        auto rendered = clip->renderFrame(static_cast<std::uint32_t>(index), geometry, draft, purpose);
        if (!rendered.ok() || !rendered.value()) {
            reportOnce(effect, *inst, "OpenOSV Source could not stitch frame " + std::to_string(index) + ": " +
                                          (rendered.ok() ? std::string("no image") : rendered.error().message));
            clearCpu(*outputView, window, levels);
            return kOfxStatFailed;
        }
        // The stitch is already at the frame's size: one copy into the
        // host's format and levels.
        if (!copyStitchedFrame(*rendered.value(), *outputView, window, frame, levels, pool.get())) {
            PluginLog::oncef("ofx/source/copy", PluginLog::Level::Error,
                             "ofx source: the stitched {}x{} frame does not fit a {}x{} frame", rendered.value()->w,
                             rendered.value()->h, frameW, frameH);
            clearCpu(*outputView, window, levels);
            return kOfxStatFailed;
        }
        return kOfxStatOK;
    }

    // ---- a camera into the native sphere ------------------------------------
    const OutputGeometry sphere = clip->geometryForLocked(prefs);
    if (!sphere.valid()) {
        clearCpu(*outputView, window, levels);
        return kOfxStatFailed;
    }
    // [WP-V-GPU] begin - the sphere stays in VRAM, the camera frames it on
    // the GPU, and only the view is read back.  "Not mine" (no GPU path)
    // falls through to the CPU framing below, as does a GPU failure, logged
    // once.
    {
        std::string gpuError;
        if (gpu::renderSourceViewGpu(*clip, static_cast<std::uint32_t>(index), sphere, draft, purpose,
                                     camera::read(params, time), camera::projectSize(effect), gpuTarget, gpuError)) {
            gpu::notePath(effect, gpu::Hook::SourceView, true, gpuError);
            return kOfxStatOK;
        }
        // Once per instance: which path took its first frame, and why.
        gpu::notePath(effect, gpu::Hook::SourceView, false, gpuError);
        if (!gpuError.empty()) {
            PluginLog::oncef("ofx/source/gpu-view", PluginLog::Level::Warn,
                             "ofx source: GPU view path failed, framing on the CPU: {}", gpuError);
        }
    }
    // [WP-V-GPU] end
    auto rendered = clip->renderFrame(static_cast<std::uint32_t>(index), sphere, draft, purpose);
    if (!rendered.ok() || !rendered.value()) {
        reportOnce(effect, *inst, "OpenOSV Source could not stitch frame " + std::to_string(index) + ": " +
                                      (rendered.ok() ? std::string("no image") : rendered.error().message));
        clearCpu(*outputView, window, levels);
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
        clearCpu(*outputView, window, levels);
        return kOfxStatFailed;
    }
    setup.source.isBgra = 0;  // the engine's image is R, G, B, A
    // Framed straight into the host's format, packed in the output levels.
    if (!renderReframeCpu(setup, *outputView, window, frame, levels, pool.get())) {
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
            // A generator has no input for the host to copy a format from,
            // so the output's components and depth are stated here.  Left
            // unstated, DaVinci Resolve hands the generator an image
            // labelled OfxImageComponentNone.  The property names are the
            // specification's "<property>_<clip name>" form.
            setString(outArgs, "OfxImageClipPropComponents_" kOfxImageEffectOutputClipName, kOfxImageComponentRGBA);
            // The depth, though, is VEGAS's to choose: it follows the
            // project (8-bit or 32-bit float), the generator writes either,
            // and a forced float would make an 8-bit project convert every
            // frame.  Only other hosts are told float.
            if (hostProfile() != HostProfile::Vegas) {
                setString(outArgs, "OfxImageClipPropDepth_" kOfxImageEffectOutputClipName, kOfxBitDepthFloat);
            }
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
