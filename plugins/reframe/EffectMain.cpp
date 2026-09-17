// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// EffectMain.cpp - the After Effects API side of Open 360 Reframe.
//
// Premiere loads this as an AE-kind effect (PiPL Kind 'eFKT', entry point
// "EffectMain") and calls it for everything except accelerated rendering:
// the Effects panel entry, the parameter list, the supervised Preset
// behaviour and the software render path that runs whenever the sequence is
// not GPU accelerated or the GPU entry declined the device.
//
// What this file is responsible for, in call order:
//
//   PF_Cmd_ABOUT            the one-line description in the About box.
//   PF_Cmd_GLOBAL_SETUP     version, out-flags (byte-identical to the PiPL),
//                           and - in Premiere only - the pixel formats we
//                           want frames in.
//   PF_Cmd_PARAMS_SETUP     the 13 controls, with their permanent ids.
//   PF_Cmd_USER_CHANGED_PARAM  Preset writes FOV/Distortion/Tilt; editing
//                           any of those three flips Preset to Custom.
//   PF_Cmd_UPDATE_PARAMS_UI reserved (nothing is greyed today).
//   PF_Cmd_SEQUENCE_*       sequence_data stays null - see below.
//   PF_Cmd_RENDER           the CPU path, through ReframeCpu.
//   PF_Cmd_GLOBAL_SETDOWN   close the log, drop the shared context.
//
// Why sequence_data is null: PF_OutFlag2_SUPPORTS_THREADED_RENDERING is set,
// which means PF_Cmd_RENDER may run on several threads at once for the same
// effect instance.  Any mutable per-instance state would need locking, and
// there is nothing this effect needs to remember between frames - the
// renderer and the thread pool are process-wide (HostContext) and every
// other value is derived from the parameters.  So SEQUENCE_SETUP /
// _RESETUP / _FLATTEN / _SETDOWN do the minimum correct thing (clear the
// handle) rather than allocating something nobody reads.

#include "ReframeCpu.h"
#include "ReframeParams.h"
// The Program Monitor overlay.  It includes the Adobe UI headers itself and
// keeps every DrawBot and event detail behind three functions, so this file
// gains three call sites and no knowledge of how the HUD is drawn.
#include "ReframeUiEvent.h"

#include "HostContext.h"
#include "PluginLog.h"

// The library's preset table, so the static_asserts below can prove the
// effect's copy still agrees with it.  This is an ordinary library header and
// has nothing to do with the Adobe pack(1) region, so it belongs here with
// our other layers rather than after the SDK includes.
#include "osv/geom/Presets.h"

// Adobe headers.  Everything of ours is declared before these open their
// #pragma pack(push, 1) region.
//
// AEConfig.h MUST come first: it is what turns _WIN32 into AE_OS_WIN, and
// Param_Utils.h branches on AE_OS_WIN to pick strncpy_s over the BSD
// strlcpy that does not exist on Windows.  AE_Effect.h does not include it,
// so every Adobe sample includes it by hand and so do we.
#include "AEConfig.h"

#include "A.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectVers.h"
#include "AE_Macros.h"
#include "Param_Utils.h"

// SPBasic.h defines SPBasicSuite itself; AE_Effect.h only forward-declares
// it, so acquiring a suite through in_data->pica_basicP needs this header.
#include "SPBasic.h"

#include "PrSDKAESupport.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKSequenceInfoSuite.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string_view>
#include <vector>

// ===========================================================================
//  The PiPL constants really are the AE constants
//
//  ReframeParams.h spells the out-flags out as literals so the .r file can
//  paste them into the resource.  These assertions are what stops that from
//  being a lie: if a future AE SDK renumbers a bit, the build breaks here
//  instead of shipping a PiPL Premiere will reject at load time.
// ===========================================================================
static_assert(OSV_REFRAME_OUT_FLAGS ==
                  (PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI | PF_OutFlag_CUSTOM_UI),
              "OSV_REFRAME_OUT_FLAGS in ReframeParams.h no longer matches AE_Effect.h");
static_assert(OSV_REFRAME_OUT_FLAGS_2 ==
                  (PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG | PF_OutFlag2_REVEALS_ZERO_ALPHA |
                   PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_THREADED_RENDERING),
              "OSV_REFRAME_OUT_FLAGS_2 in ReframeParams.h no longer matches AE_Effect.h");
static_assert(OSV_REFRAME_PIPL_VERSION ==
                  PF_VERSION(OSV_REFRAME_VERSION_MAJOR, OSV_REFRAME_VERSION_MINOR, OSV_REFRAME_VERSION_BUG,
                             PF_Stage_RELEASE, OSV_REFRAME_BUILD),
              "OSV_REFRAME_PIPL_VERSION does not equal PF_VERSION() of the same numbers");
static_assert(OSV_REFRAME_STAGE == PF_Stage_RELEASE, "the PiPL stage word is not PF_Stage_RELEASE");

// The index table in ReframeParams.h is written out by hand because the two
// group terminators break the "index == id" shortcut.  These assertions pin
// the three facts that make it correct, so a reordering of paramsSetup() has
// to come here and think rather than silently shifting every GPU GetParam.
static_assert(osv::reframe::kIndexSmooth == OSV_REFRAME_PARAM_COUNT,
              "Smooth Keyframes must be the last parameter added");
static_assert(osv::reframe::kIndexCameraTopicEnd == osv::reframe::kIndexDistortion + 1,
              "the Camera group must close immediately after Distortion");
static_assert(osv::reframe::kIndexSourceTopicEnd == osv::reframe::kIndexSourceRoll + 1,
              "the Source group must close immediately after Source Roll");
static_assert(osv::reframe::kParamIdByIndex[osv::reframe::kIndexSmooth - 1] == OSV_REFRAME_ID_SMOOTH,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(osv::reframe::kParamIdByIndex[osv::reframe::kIndexOutputAspect - 1] == OSV_REFRAME_ID_OUTPUT_ASPECT,
              "kParamIdByIndex is not aligned with the ParamIndex enum");

// ---------------------------------------------------------------------------
//  The effect's presets really are the library's presets
//
//  ReframeParams.h declares that kPresetTable "mirrors osv::geom::kPresets",
//  which is the contract that keeps this effect's looks identical to the ones
//  osvtool renders.  Nothing enforced it, so retuning geom::kPresets would
//  have silently desynchronised the two with every test still green.
//
//  ReframeParams.h cannot include the library header itself: it is also fed
//  to the resource compiler (cl /EP /TC), which sees only object-like macros.
//  So the cross-check lives here, in the one C++ translation unit that
//  already includes both, and it is a static_assert rather than a test so it
//  cannot be skipped.
// ---------------------------------------------------------------------------
namespace {

/// Look one of the library's presets up by its stable CLI id.  constexpr, so
/// the whole comparison happens at compile time.
[[nodiscard]] constexpr const osv::geom::Preset* libraryPreset(std::string_view id) noexcept {
    for (const osv::geom::Preset& p : osv::geom::kPresets) {
        // std::string_view::operator== is constexpr; Preset::id is a literal.
        if (std::string_view(p.id) == id) {
            return &p;
        }
    }
    return nullptr;
}

/// True when the effect's entry for `value` matches the library preset `id`
/// on all three numbers the popup writes.  Distortion is the eye offset as a
/// percentage, which is the only unit conversion between the two tables.
[[nodiscard]] constexpr bool presetsAgree(osv::reframe::Preset value, std::string_view id) noexcept {
    const osv::reframe::PresetEntry* mine = osv::reframe::presetEntry(value);
    const osv::geom::Preset* theirs = libraryPreset(id);
    if (!mine || !theirs) {
        return false;
    }
    return mine->fovDeg == theirs->hfovDeg && mine->tiltDeg == theirs->pitchDeg &&
           mine->distortion == theirs->eyeOffset * 100.0;
}

}  // namespace

static_assert(presetsAgree(osv::reframe::Preset::CrystalBall, "crystal-ball"),
              "the effect's Crystal Ball preset no longer matches osv::geom::kPresets");
static_assert(presetsAgree(osv::reframe::Preset::Asteroid, "asteroid"),
              "the effect's Asteroid preset no longer matches osv::geom::kPresets");
static_assert(presetsAgree(osv::reframe::Preset::Wide, "wide"),
              "the effect's Wide preset no longer matches osv::geom::kPresets");
static_assert(presetsAgree(osv::reframe::Preset::UltraWide, "ultra-wide"),
              "the effect's Ultra Wide preset no longer matches osv::geom::kPresets");
static_assert(presetsAgree(osv::reframe::Preset::Dewarping, "dewarping"),
              "the effect's Dewarping preset no longer matches osv::geom::kPresets");
// Custom is deliberately NOT in the library table: it is the "no preset"
// entry, so the counts differ by exactly one.
static_assert(osv::geom::kPresets.size() + 1u == static_cast<std::size_t>(OSV_REFRAME_PRESET_COUNT),
              "a preset was added to one table and not the other");

namespace {

using osv::premiere::PluginLog;
using namespace osv::reframe;

/// Premiere identifies itself to an AE effect with this application id;
/// several code paths below only make sense inside Premiere.
constexpr A_long kPremiereApplId = 'PrMr';

// ===========================================================================
//  Pixel format negotiation (Premiere only)
// ===========================================================================

/// Tell Premiere which formats we can render, most preferred first.
///
/// 32-bit float first because the panorama is HDR-capable and the reframe is
/// a resampling operation - quantising to 8 bits before it would band the
/// sky.  8u second so an 8-bit sequence still gets a native-format render
/// instead of a host conversion.  VUYA is deliberately not offered: the
/// shared sampler works in RGBA and converting per sample would cost more
/// than letting the host convert the frame once.
PF_Err registerPixelFormats(PF_InData* in_data) noexcept {
    if (!in_data || !in_data->pica_basicP) {
        return PF_Err_NONE;  // not fatal: the host then picks its default
    }
    // AcquireSuite hands back a const void*; the suite tables themselves are
    // read-only function pointers, so the pointer stays const throughout.
    const PF_PixelFormatSuite1* suite = nullptr;
    const void* raw = nullptr;
    const SPErr err = in_data->pica_basicP->AcquireSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, &raw);
    suite = static_cast<const PF_PixelFormatSuite1*>(raw);
    if (err != kSPNoError || !suite) {
        PluginLog::warn("reframe: no PF Pixel Format Suite; the host will choose the format");
        return PF_Err_NONE;
    }

    // Clear first: the host may carry a list from a previous load.
    if (suite->ClearSupportedPixelFormats) {
        suite->ClearSupportedPixelFormats(in_data->effect_ref);
    }
    if (suite->AddSupportedPixelFormat) {
        suite->AddSupportedPixelFormat(in_data->effect_ref, PrPixelFormat_BGRA_4444_32f);
        suite->AddSupportedPixelFormat(in_data->effect_ref, PrPixelFormat_BGRA_4444_8u);
    }
    in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
    return PF_Err_NONE;
}

/// The Premiere pixel format of an effect world, or PrPixelFormat_Invalid
/// when the suite cannot tell us (which happens in After Effects, where the
/// suite does not exist at all).
PrPixelFormat worldFormat(PF_InData* in_data, PF_EffectWorld* world) noexcept {
    if (!in_data || !in_data->pica_basicP || !world) {
        return PrPixelFormat_Invalid;
    }
    const void* raw = nullptr;
    if (in_data->pica_basicP->AcquireSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, &raw) != kSPNoError ||
        !raw) {
        return PrPixelFormat_Invalid;
    }
    const PF_PixelFormatSuite1* suite = static_cast<const PF_PixelFormatSuite1*>(raw);
    if (!suite->GetPixelFormat) {
        in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
        return PrPixelFormat_Invalid;
    }
    PrPixelFormat format = PrPixelFormat_Invalid;
    if (suite->GetPixelFormat(world, &format) != PF_Err_NONE) {
        format = PrPixelFormat_Invalid;
    }
    in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
    return format;
}

/// Map a Premiere pixel format onto the layouts the CPU renderer handles.
/// Returns false for anything else (VUYA, ARGB, a compressed format), which
/// makes RENDER decline rather than misinterpret the bytes.
bool layoutFor(PrPixelFormat format, PixelLayout* out) noexcept {
    if (!out) {
        return false;
    }
    switch (format) {
        case PrPixelFormat_BGRA_4444_32f:
            *out = PixelLayout::Bgra32f;
            return true;
        case PrPixelFormat_BGRA_4444_8u:
            *out = PixelLayout::Bgra8u;
            return true;
        default:
            return false;
    }
}

// ===========================================================================
//  Sequence geometry (the "Match Sequence" aspect)
// ===========================================================================

/// Aspect ratio of the sequence this effect instance sits on, or 0 when it
/// cannot be determined.
///
/// On the CPU side the timeline id is not handed to us directly: the PF
/// Utility Suite's GetContainingTimelineID answers it (PrSDKAESupport.h,
/// a v4 member so every host since CC has it), and the Sequence Info Suite
/// then reports the frame rectangle.
double sequenceAspect(PF_InData* in_data) noexcept {
    if (!in_data || !in_data->pica_basicP) {
        return 0.0;
    }
    SPBasicSuite* basic = in_data->pica_basicP;

    // Step 1: which timeline?
    const void* rawUtility = nullptr;
    if (basic->AcquireSuite(kPFUtilitySuite, kPFUtilitySuiteVersion4, &rawUtility) != kSPNoError || !rawUtility) {
        return 0.0;
    }
    const PF_UtilitySuite4* utility = static_cast<const PF_UtilitySuite4*>(rawUtility);
    if (!utility->GetContainingTimelineID) {
        basic->ReleaseSuite(kPFUtilitySuite, kPFUtilitySuiteVersion4);
        return 0.0;
    }
    PrTimelineID timeline = 0;
    const PF_Err timelineErr = utility->GetContainingTimelineID(in_data->effect_ref, &timeline);
    basic->ReleaseSuite(kPFUtilitySuite, kPFUtilitySuiteVersion4);
    if (timelineErr != PF_Err_NONE || timeline == 0) {
        return 0.0;
    }

    // Step 2: how big is its frame?  Acquire the newest Sequence Info Suite
    // the host provides; only GetFrameRect is used and it is the first
    // member of every version.
    const PrSDKSequenceInfoSuite* sequence = nullptr;
    int acquiredVersion = 0;
    for (const int version : {kPrSDKSequenceInfoSuiteVersion, 8, 7, 6, 5}) {
        const void* suite = nullptr;
        if (basic->AcquireSuite(kPrSDKSequenceInfoSuite, version, &suite) == kSPNoError && suite) {
            sequence = static_cast<const PrSDKSequenceInfoSuite*>(suite);
            acquiredVersion = version;
            break;
        }
    }
    if (!sequence) {
        return 0.0;
    }

    double aspect = 0.0;
    prRect rect{};
    if (sequence->GetFrameRect && sequence->GetFrameRect(timeline, &rect) == suiteError_NoError) {
        const double w = static_cast<double>(rect.right) - static_cast<double>(rect.left);
        const double h = static_cast<double>(rect.bottom) - static_cast<double>(rect.top);
        if (w > 0.0 && h > 0.0) {
            aspect = w / h;
        }
    }
    basic->ReleaseSuite(kPrSDKSequenceInfoSuite, acquiredVersion);
    return aspect;
}

// ===========================================================================
//  Reading the parameters
// ===========================================================================

/// The value of an angle control out of a PF_ParamDef (fixed 16.16 degrees).
[[nodiscard]] double angleValue(const PF_ParamDef& def) noexcept {
    return static_cast<double>(def.u.ad.value) / 65536.0;
}

/// Sample one angle parameter at a time offset of `frames` from the render
/// time.  Used by the Smooth Keyframes average; a checkout that fails
/// yields the fallback rather than a zero, so a host that refuses the
/// neighbouring time simply produces no smoothing for that sample.
double checkoutAngle(PF_InData* in_data, int index, A_long timeOffsetFrames, double fallback) noexcept {
    if (!in_data || !in_data->inter.checkout_param) {
        return fallback;
    }
    // in_data->time_step is one frame in the effect's own time scale.
    const A_long time = in_data->current_time + timeOffsetFrames * in_data->time_step;
    if (time < 0) {
        return fallback;  // before the clip start
    }
    PF_ParamDef def{};
    const PF_Err err = PF_CHECKOUT_PARAM(in_data, index, time, in_data->time_step, in_data->time_scale, &def);
    if (err != PF_Err_NONE) {
        return fallback;
    }
    const double value = angleValue(def);
    PF_CHECKIN_PARAM(in_data, &def);
    return value;
}

/// Build the resolved Settings from the params array the host handed
/// PF_Cmd_RENDER, applying the three-sample smoothing when it is on.
Settings readSettings(PF_InData* in_data, PF_ParamDef* params[]) noexcept {
    Settings s;
    if (!params) {
        return s;
    }

    // params[0] is the input layer; the controls start at 1.  The index is
    // NOT the permanent id - the two group terminators shift everything after
    // the first group - so the kIndex* constants are the only correct
    // subscripts here (ReframeParams.h spells the whole list out).
    s.aspect = sanitiseAspect(params[kIndexOutputAspect]->u.pd.value);
    s.preset = sanitisePreset(params[kIndexPreset]->u.pd.value);
    s.fovDeg = static_cast<double>(params[kIndexFov]->u.fs_d.value);
    s.distortion = static_cast<double>(params[kIndexDistortion]->u.fs_d.value);
    s.smoothKeyframes = params[kIndexSmooth]->u.bd.value != 0;

    const int angleIndices[6] = {kIndexPan,       kIndexTilt,       kIndexRoll,
                                 kIndexSourcePan, kIndexSourceTilt, kIndexSourceRoll};
    double angles[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6; ++i) {
        angles[i] = angleValue(*params[angleIndices[i]]);
    }

    // Smoothing averages the parameter at t-1, t and t+1 frames.  It is done
    // here, not in the geometry, so the GPU path can reproduce the same
    // numbers from GetParam at the same three times.
    if (s.smoothKeyframes && in_data && in_data->time_step != 0) {
        for (int i = 0; i < 6; ++i) {
            const double centre = angles[i];
            const double prev = checkoutAngle(in_data, angleIndices[i], -1, centre);
            const double next = checkoutAngle(in_data, angleIndices[i], +1, centre);
            angles[i] = (prev + centre + next) / 3.0;
        }
    }

    s.panDeg = angles[0];
    s.tiltDeg = angles[1];
    s.rollDeg = angles[2];
    s.sourcePanDeg = angles[3];
    s.sourceTiltDeg = angles[4];
    s.sourceRollDeg = angles[5];
    return s;
}

// ===========================================================================
//  Command handlers
// ===========================================================================

PF_Err about(PF_InData* in_data, PF_OutData* out_data) noexcept {
    if (!out_data) {
        return PF_Err_NONE;
    }
    // out_data->return_msg is a fixed A_char buffer of PF_MAX_EFFECT_MSG_LEN
    // + 1 bytes; snprintf bounds the write and NUL terminates.  '\r' is the
    // line separator the About box expects.
    std::snprintf(out_data->return_msg, PF_MAX_EFFECT_MSG_LEN,
                  "%s v%d.%d.%d\r"
                  "Reframe an equirectangular 360 clip with a virtual camera.\r"
                  "Part of OpenOSV, the clean-room DJI Osmo 360 toolkit.\r"
                  "Apache-2.0.",
                  OSV_REFRAME_DISPLAY_NAME, OSV_REFRAME_VERSION_MAJOR, OSV_REFRAME_VERSION_MINOR,
                  OSV_REFRAME_VERSION_BUG);
    (void)in_data;
    return PF_Err_NONE;
}

PF_Err globalSetup(PF_InData* in_data, PF_OutData* out_data) noexcept {
    if (!out_data) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // The log file is opened here rather than in DllMain: DllMain runs under
    // the loader lock and must not touch the file system.
    PluginLog::init(L"Open360Reframe");

    out_data->my_version =
        PF_VERSION(OSV_REFRAME_VERSION_MAJOR, OSV_REFRAME_VERSION_MINOR, OSV_REFRAME_VERSION_BUG, PF_Stage_RELEASE,
                   OSV_REFRAME_BUILD);
    // Exactly the PiPL words; the static_asserts above tie the two together.
    out_data->out_flags = OSV_REFRAME_OUT_FLAGS;
    out_data->out_flags2 = OSV_REFRAME_OUT_FLAGS_2;

    if (in_data && in_data->appl_id == kPremiereApplId) {
        registerPixelFormats(in_data);
    }

    PluginLog::info("reframe: global setup (host '{}{}{}{}', flags 0x{:08X}/0x{:08X})",
                    in_data ? static_cast<char>((in_data->appl_id >> 24) & 0xFF) : '?',
                    in_data ? static_cast<char>((in_data->appl_id >> 16) & 0xFF) : '?',
                    in_data ? static_cast<char>((in_data->appl_id >> 8) & 0xFF) : '?',
                    in_data ? static_cast<char>(in_data->appl_id & 0xFF) : '?',
                    static_cast<unsigned>(out_data->out_flags), static_cast<unsigned>(out_data->out_flags2));
    return PF_Err_NONE;
}

PF_Err globalSetdown(PF_InData*, PF_OutData*) noexcept {
    // Drop any half-finished overlay gesture BEFORE the render context goes,
    // so a reload of the plug-in cannot inherit a drag anchored in a frame
    // that no longer exists.
    osv::reframe::ui::shutdown();

    // Tear the process-wide context down explicitly.  Never from DllMain:
    // by then the CUDA / OpenCL runtimes may already be unloaded.
    osv::premiere::HostContext::shutdown();
    PluginLog::info("reframe: global setdown");
    PluginLog::shutdown();
    return PF_Err_NONE;
}

PF_Err paramsSetup(PF_InData* in_data, PF_OutData* out_data) noexcept {
    if (!in_data || !out_data) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    PF_ParamDef def{};

    // ---- 1. Output Aspect -------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Output Aspect", OSV_REFRAME_ASPECT_COUNT, OSV_REFRAME_ASPECT_DEFAULT, OSV_REFRAME_ASPECT_ITEMS,
                  PF_ParamFlag_SUPERVISE, OSV_REFRAME_ID_OUTPUT_ASPECT);

    // ---- 2. Camera topic --------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Camera", PF_ParamFlag_NONE, OSV_REFRAME_ID_CAMERA_TOPIC);

    // ---- 3. Preset (supervised: writes FOV / Distortion / Tilt) -----------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Preset", OSV_REFRAME_PRESET_COUNT, OSV_REFRAME_PRESET_DEFAULT, OSV_REFRAME_PRESET_ITEMS,
                  PF_ParamFlag_SUPERVISE, OSV_REFRAME_ID_PRESET);

    // ---- 4..6. Pan / Tilt / Roll -----------------------------------------
    // Angle dials are unbounded so a user can keyframe a full turn; Tilt is
    // clamped to +-90 in the geometry rather than in the control.  Tilt is
    // supervised because the preset writes it and editing it must flip the
    // preset back to Custom.
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Pan", OSV_REFRAME_PAN_DEFAULT, OSV_REFRAME_ID_PAN);

    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_SUPERVISE;
    PF_ADD_ANGLE("Tilt", OSV_REFRAME_TILT_DEFAULT, OSV_REFRAME_ID_TILT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Roll", OSV_REFRAME_ROLL_DEFAULT, OSV_REFRAME_ID_ROLL);

    // ---- 7. FOV -----------------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("FOV", OSV_REFRAME_FOV_VALID_MIN, OSV_REFRAME_FOV_VALID_MAX, OSV_REFRAME_FOV_SLIDER_MIN,
                         OSV_REFRAME_FOV_SLIDER_MAX, OSV_REFRAME_FOV_DEFAULT, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_SUPERVISE, OSV_REFRAME_ID_FOV);

    // ---- 8. Distortion ----------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Distortion", OSV_REFRAME_DISTORTION_VALID_MIN, OSV_REFRAME_DISTORTION_VALID_MAX,
                         OSV_REFRAME_DISTORTION_SLIDER_MIN, OSV_REFRAME_DISTORTION_SLIDER_MAX,
                         OSV_REFRAME_DISTORTION_DEFAULT, PF_Precision_TENTHS, PF_ValueDisplayFlag_PERCENT,
                         PF_ParamFlag_SUPERVISE, OSV_REFRAME_ID_DISTORTION);

    // ---- 9. Close the Camera group ----------------------------------------
    // PF_END_TOPIC issues its own PF_ADD_PARAM (Param_Utils.h:309-316), so
    // the terminator occupies a parameter slot of its own and every control
    // after it is shifted up by one.  Leaving it out does not "just" lose a
    // divider: the group stays open and every following control - including
    // the whole Source group - nests inside Camera, so collapsing Camera
    // hides controls that are meant to be siblings.
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OSV_REFRAME_ID_CAMERA_TOPIC_END);

    // ---- 10. Source topic (collapsed: most users never touch it) ----------
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Source", PF_ParamFlag_START_COLLAPSED, OSV_REFRAME_ID_SOURCE_TOPIC);

    // ---- 11..13. Source Pan / Tilt / Roll ---------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Source Pan", 0.0, OSV_REFRAME_ID_SOURCE_PAN);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Source Tilt", 0.0, OSV_REFRAME_ID_SOURCE_TILT);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Source Roll", 0.0, OSV_REFRAME_ID_SOURCE_ROLL);

    // ---- 14. Close the Source group ---------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OSV_REFRAME_ID_SOURCE_TOPIC_END);

    // ---- 15. Smooth Keyframes (a sibling of Output Aspect, not a member of
    //          either group - which is only true because both groups closed) -
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Smooth Keyframes", OSV_REFRAME_SMOOTH_DEFAULT, PF_ParamFlag_NONE, OSV_REFRAME_ID_SMOOTH);

    out_data->num_params = OSV_REFRAME_PARAM_COUNT + 1;  // + the input layer

    // ---- The Program Monitor overlay --------------------------------------
    // Registered here rather than at global setup because PF_CustomUIInfo
    // travels in out_data alongside the parameter list, and the host reads
    // both at the end of PARAMS_SETUP.  The call asks for
    // PF_CustomEFlag_COMP - the composition window, which in Premiere is the
    // Program Monitor - and pairs with PF_OutFlag_CUSTOM_UI in the out-flags.
    // It never fails the setup: a host that refuses a custom UI simply gets
    // an effect with no overlay, which still renders correctly.
    (void)osv::reframe::ui::registerCustomUi(in_data, out_data);

    return PF_Err_NONE;
}

/// PF_Cmd_USER_CHANGED_PARAM: the supervised behaviour.
///
/// Changing Preset writes FOV, Distortion and Tilt from the preset table and
/// marks each with PF_ChangeFlag_CHANGED_VALUE so the host records an
/// undoable edit.  Changing FOV, Distortion or Tilt by hand means the look
/// is no longer the preset, so Preset flips to Custom - which is exactly how
/// every other reframe UI behaves and stops the popup from lying.
PF_Err userChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                        const PF_UserChangedParamExtra* extra) noexcept {
    if (!params || !extra) {
        return PF_Err_NONE;
    }
    (void)in_data;
    (void)out_data;

    const PF_ParamIndex changed = extra->param_index;

    if (changed == kIndexPreset) {
        const Preset preset = sanitisePreset(params[kIndexPreset]->u.pd.value);
        const PresetEntry* entry = presetEntry(preset);
        if (!entry || !entry->writesControls) {
            return PF_Err_NONE;  // "Custom" writes nothing
        }
        params[kIndexFov]->u.fs_d.value = static_cast<PF_FpShort>(entry->fovDeg);
        params[kIndexFov]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        params[kIndexDistortion]->u.fs_d.value = static_cast<PF_FpShort>(entry->distortion);
        params[kIndexDistortion]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        // The angle control stores fixed 16.16 degrees.
        params[kIndexTilt]->u.ad.value = static_cast<PF_Fixed>(std::lround(entry->tiltDeg * 65536.0));
        params[kIndexTilt]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        PluginLog::debug("reframe: preset '{}' -> fov {} distortion {} tilt {}", entry->label, entry->fovDeg,
                         entry->distortion, entry->tiltDeg);
        return PF_Err_NONE;
    }

    if (changed == kIndexFov || changed == kIndexDistortion || changed == kIndexTilt) {
        if (sanitisePreset(params[kIndexPreset]->u.pd.value) != Preset::Custom) {
            params[kIndexPreset]->u.pd.value = static_cast<A_long>(Preset::Custom);
            params[kIndexPreset]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
            PluginLog::debug("reframe: manual edit of param {} -> preset Custom", static_cast<int>(changed));
        }
        return PF_Err_NONE;
    }

    return PF_Err_NONE;
}

/// PF_Cmd_UPDATE_PARAMS_UI: nothing is enabled or disabled today.  The
/// handler exists (and the out-flag is set) because it is the only hook for
/// greying controls, and adding it later would change the PiPL flags, which
/// invalidates the plug-in cache on every installed machine.
PF_Err updateParamsUi(PF_InData*, PF_OutData*, PF_ParamDef*[]) noexcept { return PF_Err_NONE; }

/// PF_Cmd_RENDER: the software path.
PF_Err render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) noexcept {
    (void)out_data;
    if (!in_data || !params || !output) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    PF_LayerDef* input = &params[0]->u.ld;
    if (!input->data || !output->data) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // ---- what layout are the worlds in? ----------------------------------
    // In Premiere the PF Pixel Format Suite answers; without it (After
    // Effects, or a host that hid the suite) the world is an AE-native
    // ARGB/float world we do not claim to handle, so decline rather than
    // reinterpret the channel order.
    PixelLayout inLayout = PixelLayout::Bgra32f;
    PixelLayout outLayout = PixelLayout::Bgra32f;
    const PrPixelFormat inFormat = worldFormat(in_data, input);
    const PrPixelFormat outFormat = worldFormat(in_data, output);
    if (!layoutFor(inFormat, &inLayout) || !layoutFor(outFormat, &outLayout)) {
        PluginLog::oncef("reframe/render/format", PluginLog::Level::Error,
                         "reframe: unsupported world format (in 0x{:08X}, out 0x{:08X})",
                         static_cast<unsigned>(inFormat), static_cast<unsigned>(outFormat));
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // ---- describe the two worlds -----------------------------------------
    // Effect worlds are addressed as data + y * rowbytes, and rowbytes may
    // legally be negative.
    //
    // Both worlds are described with topDown = true, and that is correct even
    // though the SDK guide's section 5.4.2 says "uncompressed formats have a
    // lower-left origin".  That sentence is in the PIXEL FORMAT chapter and
    // describes PPix buffers - the importer's output, where PixelCopy really
    // does flip rows (PixelCopy.h) - not PF_EffectWorld.  For an AE-API
    // effect the two worlds share whatever origin the host chose, so the
    // source and destination conventions cancel: reading row k of the input
    // and writing row k of the output maps the frame onto itself either way.
    // Adobe's own Premiere AE-effect CPU sample confirms it by walking src
    // and dst with the same forward `data += rowbytes` loop and offering a
    // vertical flip only as a deliberate user control
    // (SDK_CrossDissolve_CPU.cpp:123-134).
    //
    // What would NOT cancel is a geometric transform that depends on
    // absolute row position - which is exactly what this effect does, since
    // an equirect input's row 0 is the zenith.  That is why the INPUT
    // orientation is pinned by the panorama's own content (the test renders
    // a labelled panorama and decodes the aimed direction), not by trusting
    // this flag: if the host ever handed us a bottom-up world the aim test
    // would fail, rather than a silent mirror slipping through.
    ConstFrameView src;
    src.base = input->data;
    src.rowBytes = static_cast<std::int32_t>(input->rowbytes);
    src.width = static_cast<int>(input->width);
    src.height = static_cast<int>(input->height);
    src.layout = inLayout;
    src.topDown = true;

    FrameView dst;
    dst.base = output->data;
    dst.rowBytes = static_cast<std::int32_t>(output->rowbytes);
    dst.width = static_cast<int>(output->width);
    dst.height = static_cast<int>(output->height);
    dst.layout = outLayout;
    dst.topDown = true;

    // ---- the shared thread pool -------------------------------------------
    // Taken here because BOTH the 8-bit promotion below and the render itself
    // want it.
    //
    // A shared_ptr LEASE, not a raw pointer into the singleton.  We declare
    // PF_OutFlag2_SUPPORTS_THREADED_RENDERING, so this function runs on
    // several threads at once, while GLOBAL_SETDOWN runs on the main thread
    // and deletes the HostContext - which drops the last reference to the
    // pool and JOINS its workers.  Holding the lease for the whole render
    // keeps the pool alive until the last row is done, however the host
    // interleaves the two.  That is exactly what threadPoolShared() is for.
    const std::shared_ptr<osv::ThreadPool> poolLease = osv::premiere::HostContext::instance().threadPoolShared();
    osv::ThreadPool* pool = poolLease.get();

    // ---- promote an 8-bit input -------------------------------------------
    // GLOBAL_SETUP advertises PrPixelFormat_BGRA_4444_8u, so the host is
    // entitled to hand us one, and an effect that advertises a format must
    // accept it.  The shared sampler reads float or half only, so the frame
    // is promoted once into a scratch buffer and rendered from that.  (This
    // used to return PF_Err_BAD_CALLBACK_PARAM in the hope the host would
    // retry with 32f, which nothing in the SDK promises: a host that took us
    // at our word got a hard error on every frame.)
    //
    // `promoted` must outlive `src`, so it is declared in this scope.
    std::vector<float> promoted;
    if (src.layout == PixelLayout::Bgra8u) {
        const ConstFrameView promotedView = promoteBgra8uToFloat(src, promoted, pool);
        if (!promotedView.valid()) {
            PluginLog::oncef("reframe/render/8u-in", PluginLog::Level::Error,
                             "reframe: could not promote an 8-bit input world ({}x{})", src.width, src.height);
            return PF_Err_OUT_OF_MEMORY;
        }
        src = promotedView;
    }

    // ---- parameters and geometry ------------------------------------------
    const Settings settings = readSettings(in_data, params);
    const KernelSetup setup = buildParams(settings, src, dst.width, dst.height, sequenceAspect(in_data));
    if (!setup.valid) {
        PluginLog::oncef("reframe/render/setup", PluginLog::Level::Error,
                         "reframe: could not build the kernel parameters ({}x{} -> {}x{})", src.width, src.height,
                         dst.width, dst.height);
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // ---- render ------------------------------------------------------------
    // The process-wide pool (leased above), so a dozen concurrent effect
    // instances share one set of worker threads instead of each spawning its
    // own.  instance() creates the context on first use; it is torn down in
    // GLOBAL_SETDOWN, which the lease survives.
    if (!renderCpu(setup, src, dst, pool)) {
        PluginLog::error("reframe: the CPU render failed ({}x{})", dst.width, dst.height);
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return PF_Err_NONE;
}

}  // namespace

// ===========================================================================
//  The exported entry point
//
//  Named exactly as the PiPL's CodeWin64X86 property says.  Everything is
//  wrapped in a try/catch: an exception unwinding into Premiere's C stack is
//  undefined behaviour.
// ===========================================================================
extern "C" __declspec(dllexport) PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
                                                   PF_ParamDef* params[], PF_LayerDef* output, void* extra) {
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:
                return about(in_data, out_data);
            case PF_Cmd_GLOBAL_SETUP:
                return globalSetup(in_data, out_data);
            case PF_Cmd_GLOBAL_SETDOWN:
                return globalSetdown(in_data, out_data);
            case PF_Cmd_PARAMS_SETUP:
                return paramsSetup(in_data, out_data);
            case PF_Cmd_USER_CHANGED_PARAM:
                return userChangedParam(in_data, out_data, params,
                                        static_cast<const PF_UserChangedParamExtra*>(extra));
            case PF_Cmd_UPDATE_PARAMS_UI:
                return updateParamsUi(in_data, out_data, params);
            case PF_Cmd_RENDER:
                return render(in_data, out_data, params, output);

            // The interactive Program Monitor overlay.  Everything about it
            // - hit-testing, DrawBot, and committing dragged values so the
            // host records keyframes - lives in ReframeUiEvent.cpp; `extra`
            // is the PF_EventExtra the host filled in.
            case PF_Cmd_EVENT:
                return osv::reframe::ui::handleEvent(in_data, out_data, params,
                                                     static_cast<PF_EventExtra*>(extra));

            // The effect keeps no per-instance state (see the file header),
            // so the sequence commands only have to leave the handle null.
            case PF_Cmd_SEQUENCE_SETUP:
            case PF_Cmd_SEQUENCE_RESETUP:
            case PF_Cmd_SEQUENCE_FLATTEN:
            case PF_Cmd_SEQUENCE_SETDOWN:
                if (out_data) {
                    out_data->sequence_data = nullptr;
                }
                return PF_Err_NONE;

            default:
                // Every other selector is genuinely not handled; PF_Err_NONE
                // is the documented "ignored" answer and returning an error
                // here would make the host report a broken effect.
                return PF_Err_NONE;
        }
    } catch (const std::exception& e) {
        PluginLog::error("reframe: exception in selector {}: {}", static_cast<int>(cmd), e.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    } catch (...) {
        PluginLog::error("reframe: unknown exception in selector {}", static_cast<int>(cmd));
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}
