// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxReframe.cpp - the OpenOSV 360 Reframe OpenFX filter (OfxReframe.h).
//
// One filter, two hosts:
//
//   * DaVinci Resolve (and any host but VEGAS): float RGBA images, Filter and
//     General contexts, CUDA images on an NVIDIA machine - exactly as before
//     VEGAS support existed (tests/ofx/golden pins the descriptors);
//   * VEGAS Pro: 8-bit or float images in R G B A or B G R A, the Filter
//     context only (VEGAS lists every declared context as a separate FX),
//     always in host memory.  The view is framed on the plug-in's own GPU
//     when there is one (OfxGpuView.h: the source uploaded in its own
//     depth, the view packed on the device); on the CPU otherwise, where an
//     8-bit source is promoted to float once per frame for the shared
//     sampler.  The output is written in the host's own format.  Levels are
//     never touched: the filter only moves pixels the host has already
//     levelled.
//
// Every difference keys on hostProfile() (OfxHost.h) and on nothing else.

#include "OfxReframe.h"

#include "OfxCamera.h"
#include "OfxCuda.h"
#include "OfxGpuView.h"
#include "OfxHostImage.h"
#include "OfxRender.h"

#include "HostContext.h"
#include "PluginLog.h"
#include "ReframeCpu.h"

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osv::ofx::reframe_filter {

using osv::premiere::HostContext;
using osv::premiere::PluginLog;

namespace {

/// True when two action names are the same string.
[[nodiscard]] bool isAction(const char* action, const char* name) noexcept {
    return action && name && std::strcmp(action, name) == 0;
}

/// An image's labels for a log line: depth, components and - where the host
/// sets one (VEGAS) - the pixel order.
[[nodiscard]] std::string labels(const ClipImage& image) {
    std::string text = image.depth + " " + image.components;
    if (!image.order.empty()) {
        text += " " + image.order;
    }
    return text;
}

/// True for a 32-bit float R G B A view: the only format the kernel on the
/// host's CUDA images knows.  Resolve's images always are; VEGAS's may not
/// be (the own GPU path in the [WP-V-GPU] region takes every format).
[[nodiscard]] bool isFloatRgbaView(const HostImageView& view) noexcept {
    return view.depth == HostDepth::Float && view.order == HostOrder::Rgba;
}

// ===========================================================================
//  kOfxActionDescribe
// ===========================================================================

OfxStatus describe(OfxImageEffectHandle effect) noexcept {
    OfxPropertySetHandle props = effectProps(effect);
    if (!props) {
        return kOfxStatErrBadHandle;
    }
    // "OpenOSV" leads the name because Resolve's Effects search matches
    // names, not groups: one search for "OpenOSV" finds both effects.
    // Premiere's effect is still "Open 360 Reframe"; the identifier
    // (org.openosv.Open360Reframe, stored in projects) never changes.
    setString(props, kOfxPropLabel, "OpenOSV 360 Reframe");
    setString(props, kOfxImageEffectPluginPropGrouping, "OpenOSV");
    setString(props, kOfxPropPluginDescription,
              "Point a keyframable virtual camera into any 360 equirectangular clip. DJI Studio's lens, presets "
              "and keyframe curves, frame for frame.");

    const HostProfile profile = hostProfile();

    // Filter for the timeline and the Color page; General for hosts (Nuke,
    // Natron) that prefer it.  Both have exactly one input called Source.
    // VEGAS turns every declared context into its own entry in the Video FX
    // list, so there the filter is a Filter and nothing else.
    setString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0);
    if (profile != HostProfile::Vegas) {
        setString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral, 1);
    }

    // Float only outside VEGAS; 8-bit and float, R G B A and B G R A in
    // VEGAS (declarePixelDepths() in OfxHost.cpp has the reasons).
    declarePixelDepths(props, profile);

    setInt(props, kOfxImageEffectPluginPropSingleInstance, 0);
    // Every render reads only its own arguments and the (read-only) images,
    // so any number of renders may run at once, on any instance - VEGAS's
    // clones included (one per render thread).
    setString(props, kOfxImageEffectPluginRenderThreadSafety, kOfxImageEffectRenderFullySafe);
    // We spread rows over our own pool; the host must not split the frame.
    setInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0);
    setInt(props, kOfxImageEffectPropSupportsMultiResolution, 1);
    // No tiles: every output pixel may read ANY part of the sphere.
    setInt(props, kOfxImageEffectPropSupportsTiles, 0);
    setInt(props, kOfxImageEffectPropTemporalClipAccess, 0);
    setInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0);
    setInt(props, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0);

#if defined(OSV_OFX_HAVE_CUDA)
    // OpenFX 1.5 CUDA render: the host may hand us device images and its own
    // stream.  A host without CUDA (or a GPU that is not NVIDIA) simply never
    // sets kOfxImageEffectPropCudaEnabled, and the CPU path renders.
    setString(props, kOfxImageEffectPropCudaRenderSupported, "true");
    setString(props, kOfxImageEffectPropCudaStreamSupported, "true");
#endif
    return kOfxStatOK;
}

// ===========================================================================
//  kOfxImageEffectActionDescribeInContext
// ===========================================================================

OfxStatus describeInContext(OfxImageEffectHandle effect) noexcept {
    const OfxImageEffectSuiteV1* es = suites().effect;
    if (!es || !es->clipDefine) {
        return kOfxStatErrMissingHostFeature;
    }
    // The equirect in, the view out.  RGBA both ways: the view keeps the
    // panorama's own alpha.
    OfxPropertySetHandle source = nullptr;
    if (es->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &source) != kOfxStatOK || !source) {
        return kOfxStatErrMissingHostFeature;
    }
    setString(source, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA);
    setInt(source, kOfxImageEffectPropSupportsTiles, 0);
    setInt(source, kOfxImageClipPropOptional, 0);

    OfxPropertySetHandle output = nullptr;
    if (es->clipDefine(effect, kOfxImageEffectOutputClipName, &output) != kOfxStatOK || !output) {
        return kOfxStatErrMissingHostFeature;
    }
    setString(output, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA);
    setInt(output, kOfxImageEffectPropSupportsTiles, 0);

    camera::describe(effectParams(effect));
    return kOfxStatOK;
}

// ===========================================================================
//  kOfxImageEffectActionGetRegionsOfInterest
// ===========================================================================

OfxStatus regionsOfInterest(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs,
                            OfxPropertySetHandle outArgs) noexcept {
    const OfxImageEffectSuiteV1* es = suites().effect;
    OfxImageClipHandle source = clipHandle(effect, kOfxImageEffectSimpleSourceClipName);
    if (!es || !source || !es->clipGetRegionOfDefinition) {
        return kOfxStatReplyDefault;
    }
    // Any output pixel can look anywhere on the sphere, so the whole source
    // is always needed, whatever part of the output is being rendered.
    OfxRectD rod{0, 0, 0, 0};
    if (es->clipGetRegionOfDefinition(source, getDouble(inArgs, kOfxPropTime), &rod) != kOfxStatOK) {
        return kOfxStatReplyDefault;
    }
    const double roi[4] = {rod.x1, rod.y1, rod.x2, rod.y2};
    const std::string name = std::string("OfxImageClipPropRoI_") + kOfxImageEffectSimpleSourceClipName;
    return setDoubles(outArgs, name.c_str(), roi, 4) ? kOfxStatOK : kOfxStatReplyDefault;
}

// ===========================================================================
//  kOfxActionInstanceChanged
// ===========================================================================

OfxStatus instanceChanged(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) noexcept {
    // Only the user's own edits are supervised; our own writes come back as
    // kOfxChangePluginEdited and a time change is not an edit at all.
    if (getString(inArgs, kOfxPropChangeReason) != kOfxChangeUserEdited) {
        return kOfxStatReplyDefault;
    }
    // Parameters only: VEGAS also reports its clip "Output" changing.
    if (getString(inArgs, kOfxPropType) != kOfxTypeParameter) {
        return kOfxStatReplyDefault;
    }
    const std::string name = getString(inArgs, kOfxPropName);
    const OfxTime time = getDouble(inArgs, kOfxPropTime);
    camera::instanceChanged(effectParams(effect), name.c_str(), time, camera::projectSize(effect));
    return kOfxStatOK;
}

// ===========================================================================
//  kOfxImageEffectActionRender
// ===========================================================================

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) noexcept {
    const OfxTime time = getDouble(inArgs, kOfxPropTime);
    int w[4] = {0, 0, 0, 0};
    if (!getInts(inArgs, kOfxImageEffectPropRenderWindow, w, 4)) {
        return kOfxStatFailed;
    }
    const OfxRectI window{w[0], w[1], w[2], w[3]};
    double sx = 1.0;
    double sy = 1.0;
    renderScale(inArgs, sx, sy);
    const bool cudaEnabled = getInt(inArgs, kOfxImageEffectPropCudaEnabled, 0, 0) != 0;
    void* stream = getPointer(inArgs, kOfxImageEffectPropCudaStream);
    const HostProfile profile = hostProfile();

    // ---- the two images ---------------------------------------------------------
    // Each is reduced to a HostImageView - where the pixels are and how they
    // are laid out - by the one rule for this host (ClipImage::view()).
    OfxImageClipHandle outputClip = clipHandle(effect, kOfxImageEffectOutputClipName);
    OfxImageClipHandle sourceClip = clipHandle(effect, kOfxImageEffectSimpleSourceClipName);
    ClipImage output(outputClip, time);
    if (!output.valid()) {
        return kOfxStatFailed;
    }
    const std::optional<HostImageView> outputView = output.view(profile);
    if (!outputView) {
        PluginLog::oncef("ofx/reframe/format", PluginLog::Level::Error,
                         "ofx reframe: output image is {} - only {} is supported", labels(output),
                         acceptedFormats(profile));
        return kOfxStatErrImageFormat;
    }
    ClipImage source(sourceClip, time);
    const std::optional<HostImageView> sourceImageView = source.view(profile);
    if (!sourceImageView) {
        // No picture to reframe (a gap, an unsupported format): transparent
        // black on the CPU, a refusal on the GPU, where we cannot clear.
        if (cudaEnabled) {
            return kOfxStatFailed;
        }
        clearCpu(*outputView, window);
        return kOfxStatOK;
    }

    // ---- the camera ---------------------------------------------------------------
    OfxParamSetHandle params = effectParams(effect);
    const reframe::Settings settings = camera::read(params, time);
    const double par = getDouble(effectProps(effect), kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.0);
    const OfxRectI frame = cameraFrame(outputClip, time, sx, sy, par, output.bounds);

    // ---- [WP-V-GPU] begin - CPU images framed on our own GPU ----------------------
    // A host that hands CPU images (VEGAS always does) still gets the view
    // framed on the GPU: the source uploaded in its OWN depth (an 8-bit
    // source crosses the bus at 4 bytes a pixel - no float copy is made for
    // this path), framed, packed in the output's own depth and order, read
    // back.  "Not mine" (no GPU path) falls through to the CPU loop below, as
    // does a GPU failure, logged once.  The filter keeps the host's levels:
    // it only moves pixels.  A host's CUDA images never come here.
    if (!cudaEnabled) {
        // The camera half of buildParams() - buildView(), bit for bit the
        // params buildParams() would return - with the source described by its
        // size and order: the GPU reads the source as the host holds it, so
        // it needs neither the promotion nor a sampler pointer into it.
        const reframe::ViewSetup gpuView =
            reframe::buildView(settings, frame.x2 - frame.x1, frame.y2 - frame.y1, camera::projectSize(effect));
        if (gpuView.valid) {
            reframe::KernelSetup gpuSetup;
            gpuSetup.params = gpuView.params;
            gpuSetup.source.w = sourceImageView->width();
            gpuSetup.source.h = sourceImageView->height();
            gpuSetup.source.isBgra = sourceImageView->order == HostOrder::Bgra ? 1 : 0;
            gpuSetup.valid = true;
            gpuSetup.reject = reframe::SetupReject::None;
            gpu::HostTarget gpuTarget;
            gpuTarget.image = *outputView;
            gpuTarget.window = window;
            gpuTarget.frame = frame;
            gpuTarget.levels = OutputLevels::Full;
            std::string gpuError;
            if (gpu::renderReframeFromHostGpu(gpuSetup, *sourceImageView, gpuTarget, gpuError)) {
                gpu::notePath(effect, gpu::Hook::ReframeFilter, true, gpuError);
                return kOfxStatOK;
            }
            // Once per instance: which path took its first frame, and why.
            gpu::notePath(effect, gpu::Hook::ReframeFilter, false, gpuError);
            if (!gpuError.empty()) {
                PluginLog::oncef("ofx/reframe/gpu-host", PluginLog::Level::Warn,
                                 "ofx reframe: GPU path for CPU images failed, framing on the CPU: {}", gpuError);
            }
        }
        // No camera for this frame: the setup below refuses it and says why,
        // exactly as before the GPU path existed.
    }
    // [WP-V-GPU] end

    // ---- the source as the shared sampler reads it -----------------------------
    // The sampler reads float (or half) only.  An 8-bit source - VEGAS's
    // 8-bit projects - is promoted to a float copy once per frame, in the
    // same channel order, on all cores; a float source is read in place.
    // Only the CPU loop and the host-CUDA path get here: the own GPU path
    // above samples 8-bit codes directly.
    reframe::ConstFrameView samplerSource = sourceView(*sourceImageView);
    std::vector<float> promoted;
    if (reframe::layoutNeedsPromotion(samplerSource.layout)) {
        std::shared_ptr<ThreadPool> promotePool = HostContext::instance().threadPoolShared();
        samplerSource = reframe::promoteIntegerToFloat(samplerSource, promoted, promotePool.get());
        if (!samplerSource.valid()) {
            PluginLog::oncef("ofx/reframe/promote", PluginLog::Level::Error,
                             "ofx reframe: could not make a float copy of a {}x{} 8-bit source",
                             sourceImageView->width(), sourceImageView->height());
            return kOfxStatErrMemory;
        }
    }

    // ---- the sampler's setup --------------------------------------------------------
    reframe::KernelSetup setup = reframe::buildParams(settings, samplerSource, frame.x2 - frame.x1,
                                                      frame.y2 - frame.y1, camera::projectSize(effect));
    if (!setup.valid) {
        PluginLog::oncef("ofx/reframe/setup", PluginLog::Level::Warn,
                         "ofx reframe: no camera for a {}x{} frame from a {}x{} source ({})", frame.x2 - frame.x1,
                         frame.y2 - frame.y1, source.width(), source.height(), reframe::setupRejectName(setup.reject));
        return kOfxStatFailed;
    }
    // buildParams() described a Premiere frame (B, G, R, A); the host's image
    // says its own order - R, G, B, A everywhere but in some VEGAS images.
    setup.source.isBgra = sourceImageView->order == HostOrder::Bgra ? 1 : 0;

    // ---- the GPU path: the host's CUDA images, on the host's stream -------------
    if (cudaEnabled) {
        // The kernel reads and writes 32-bit float R, G, B, A, the one format
        // a CUDA host (Resolve) hands out; anything else is refused rather
        // than misread on the device.
        if (!isFloatRgbaView(*outputView) || !isFloatRgbaView(*sourceImageView)) {
            PluginLog::oncef("ofx/reframe/cuda-format", PluginLog::Level::Error,
                             "ofx reframe: CUDA images must be 32-bit float RGBA (output {}, source {})",
                             labels(output), labels(source));
            return kOfxStatErrImageFormat;
        }
        const OfxRectI area = intersect(window, output.bounds);
        OsvOfxTarget target{};
        target.boundsX1 = output.bounds.x1;
        target.boundsY1 = output.bounds.y1;
        target.windowX1 = area.x1;
        target.windowY1 = area.y1;
        target.windowW = area.x2 - area.x1;
        target.windowH = area.y2 - area.y1;
        target.frameX1 = frame.x1;
        target.frameY2 = frame.y2;
        target.rowBytes = output.rowBytes;
        std::string error;
        if (!cuda::launchReframe(setup.params, setup.source, setup.sourceRow0, output.data, target, stream, error)) {
            PluginLog::oncef("ofx/reframe/cuda", PluginLog::Level::Error, "ofx reframe: CUDA render failed: {}",
                             error);
            return kOfxStatFailed;
        }
        return kOfxStatOK;
    }

    // ---- the CPU path -------------------------------------------------------------
    // Written in the output's own depth and order; full levels, because the
    // filter keeps whatever levels the host's source was in.
    std::shared_ptr<ThreadPool> pool = HostContext::instance().threadPoolShared();
    if (!renderReframeCpu(setup, *outputView, window, frame, OutputLevels::Full, pool.get())) {
        return kOfxStatFailed;
    }
    return kOfxStatOK;
}

}  // namespace

// ===========================================================================
//  The entry point
// ===========================================================================

OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                    OfxPropertySetHandle outArgs) noexcept {
    try {
        // The handle of every image effect action is the effect itself.
        OfxImageEffectHandle effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
        if (isAction(action, kOfxImageEffectActionRender)) {
            return render(effect, inArgs);
        }
        if (isAction(action, kOfxImageEffectActionGetRegionsOfInterest)) {
            return regionsOfInterest(effect, inArgs, outArgs);
        }
        if (isAction(action, kOfxActionInstanceChanged)) {
            return instanceChanged(effect, inArgs);
        }
        if (isAction(action, kOfxActionCreateInstance)) {
            // One lens's controls on screen from the start.
            camera::applyVisibility(effectParams(effect), 0.0);
            return kOfxStatOK;
        }
        if (isAction(action, kOfxActionDescribe)) {
            return describe(effect);
        }
        if (isAction(action, kOfxImageEffectActionDescribeInContext)) {
            return describeInContext(effect);
        }
        // Load / Unload are answered by the module (OfxMain.cpp); everything
        // else - identity, clip preferences, RoD, begin / end sequence - has
        // exactly the default behaviour we want.
        return kOfxStatReplyDefault;
    } catch (...) {
        PluginLog::error("ofx reframe: exception in action '{}'", action ? action : "?");
        return kOfxStatFailed;
    }
}

}  // namespace osv::ofx::reframe_filter
