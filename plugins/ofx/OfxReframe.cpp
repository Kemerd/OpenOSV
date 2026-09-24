// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxReframe.cpp - the OpenOSV 360 Reframe OpenFX filter (OfxReframe.h).

#include "OfxReframe.h"

#include "OfxCamera.h"
#include "OfxCuda.h"
#include "OfxRender.h"

#include "HostContext.h"
#include "PluginLog.h"
#include "ReframeCpu.h"

#include <cstring>
#include <string>
#include <string_view>

namespace osv::ofx::reframe_filter {

using osv::premiere::HostContext;
using osv::premiere::PluginLog;

namespace {

/// True when two action names are the same string.
[[nodiscard]] bool isAction(const char* action, const char* name) noexcept {
    return action && name && std::strcmp(action, name) == 0;
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

    // Filter for the timeline and the Color page; General for hosts (Nuke,
    // Natron) that prefer it.  Both have exactly one input called Source.
    setString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0);
    setString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral, 1);

    // 32-bit float only: the panorama is HDR (PQ / HLG / log) more often than
    // not, and the sampler reads float.  Resolve always offers float.
    setString(props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);

    setInt(props, kOfxImageEffectPluginPropSingleInstance, 0);
    // Every render reads only its own arguments and the (read-only) images,
    // so any number of renders may run at once, on any instance.
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

    // ---- the two images ---------------------------------------------------------
    OfxImageClipHandle outputClip = clipHandle(effect, kOfxImageEffectOutputClipName);
    OfxImageClipHandle sourceClip = clipHandle(effect, kOfxImageEffectSimpleSourceClipName);
    ClipImage output(outputClip, time);
    if (!output.valid()) {
        return kOfxStatFailed;
    }
    if (!output.isFloatRgba()) {
        PluginLog::oncef("ofx/reframe/format", PluginLog::Level::Error,
                         "ofx reframe: output image is {} {} - only 32-bit float RGBA is supported", output.depth,
                         output.components);
        return kOfxStatErrImageFormat;
    }
    ClipImage source(sourceClip, time);
    if (!source.isFloatRgba()) {
        // No picture to reframe (a gap, an unsupported format): transparent
        // black on the CPU, a refusal on the GPU, where we cannot clear.
        if (cudaEnabled) {
            return kOfxStatFailed;
        }
        clearCpu(output, window);
        return kOfxStatOK;
    }

    // ---- the camera ---------------------------------------------------------------
    OfxParamSetHandle params = effectParams(effect);
    const reframe::Settings settings = camera::read(params, time);
    const double par = getDouble(effectProps(effect), kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.0);
    const OfxRectI frame = cameraFrame(outputClip, time, sx, sy, par, output.bounds);
    reframe::KernelSetup setup = reframe::buildParams(settings, sourceView(source), frame.x2 - frame.x1,
                                                      frame.y2 - frame.y1, camera::projectSize(effect));
    if (!setup.valid) {
        PluginLog::oncef("ofx/reframe/setup", PluginLog::Level::Warn,
                         "ofx reframe: no camera for a {}x{} frame from a {}x{} source ({})", frame.x2 - frame.x1,
                         frame.y2 - frame.y1, source.width(), source.height(), reframe::setupRejectName(setup.reject));
        return kOfxStatFailed;
    }
    // OpenFX images are R, G, B, A; buildParams() described a Premiere frame.
    setup.source.isBgra = 0;

    // ---- the GPU path: the host's CUDA images, on the host's stream -------------
    if (cudaEnabled) {
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
    std::shared_ptr<ThreadPool> pool = HostContext::instance().threadPoolShared();
    if (!renderReframeCpu(setup, output, window, frame, pool.get())) {
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
