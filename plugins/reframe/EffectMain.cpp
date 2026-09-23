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
//                           want frames in.  That registration is RETRIED
//                           from the first PF_Cmd_RENDER if the suite was not
//                           available here; see registerPixelFormats().
//   PF_Cmd_PARAMS_SETUP     the 19 controls, with their permanent ids (the
//                           DJI camera block, ids 16..20, and the Lens
//                           popup, id 21, appended last).
//   PF_Cmd_USER_CHANGED_PARAM  Preset writes the Classic and the DJI lens
//                           and Tilt and selects DJI's lens; the Lens popup
//                           carries the look across to the lens picked;
//                           editing a lens control flips Preset to Custom
//                           and selects that control's lens.
//   PF_Cmd_UPDATE_PARAMS_UI shows the selected lens's controls and hides the
//                           other lens's (PF_PUI_INVISIBLE through
//                           PF_UpdateParamUI), renaming DJI FOV to "FOV".
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

// The PF Param Utils Suite (PF_UpdateParamUI), which is how UPDATE_PARAMS_UI
// shows one lens's controls at a time.
#include "AE_EffectSuites.h"

#include "PrSDKAESupport.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKSequenceInfoSuite.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
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
static_assert(osv::reframe::kIndexSmooth == 15,
              "Smooth Keyframes must stay the 15th parameter: saved projects and host index maps depend on it");
static_assert(osv::reframe::kIndexCameraTopicEnd == osv::reframe::kIndexDistortion + 1,
              "the Camera group must close immediately after Distortion");
static_assert(osv::reframe::kIndexSourceTopicEnd == osv::reframe::kIndexSourceRoll + 1,
              "the Source group must close immediately after Source Roll");
static_assert(osv::reframe::kParamIdByIndex[osv::reframe::kIndexSmooth - 1] == OSV_REFRAME_ID_SMOOTH,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(osv::reframe::kParamIdByIndex[osv::reframe::kIndexOutputResolution - 1] ==
                  OSV_REFRAME_ID_OUTPUT_RESOLUTION,
              "kParamIdByIndex is not aligned with the ParamIndex enum");

// [WP-CAMERA] The DJI block is APPENDED: it follows Smooth Keyframes, in this
// order, and ends the list.  Inserting it anywhere else would shift the index
// of an existing control and break every saved project.
static_assert(osv::reframe::kIndexCameraModel == osv::reframe::kIndexSmooth + 1 &&
                  osv::reframe::kIndexZoom == osv::reframe::kIndexCameraModel + 1 &&
                  osv::reframe::kIndexDjiFov == osv::reframe::kIndexZoom + 1 &&
                  osv::reframe::kIndexCorrection == osv::reframe::kIndexDjiFov + 1 &&
                  osv::reframe::kIndexDragSensitivity == osv::reframe::kIndexCorrection + 1,
              "the DJI camera block must follow Smooth Keyframes in its documented order");
static_assert(osv::reframe::kParamIdByIndex[osv::reframe::kIndexDragSensitivity - 1] ==
                  OSV_REFRAME_ID_DRAG_SENSITIVITY,
              "kParamIdByIndex is not aligned with the ParamIndex enum");

// [WP-LENSUI] The Lens popup is appended after the DJI block and ends the
// list, for the same reason: no saved index may move.
static_assert(osv::reframe::kIndexLens == osv::reframe::kIndexDragSensitivity + 1,
              "the Lens popup must follow Drag Sensitivity");
static_assert(osv::reframe::kIndexLens == OSV_REFRAME_PARAM_COUNT, "the Lens popup must be the last parameter added");
static_assert(osv::reframe::kParamIdByIndex[osv::reframe::kIndexLens - 1] == OSV_REFRAME_ID_LENS,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(OSV_REFRAME_LENS_COUNT == 2 &&
                  static_cast<int>(osv::reframe::LensPopup::Classic) == OSV_REFRAME_LENS_COUNT,
              "the Lens popup is exactly DJI | Classic");
// Every lens-specific control must name a real control, and the Camera Model
// mirror must not be one of them (it is hidden under every lens).
static_assert(
    [] {
        for (const osv::reframe::LensControl& c : osv::reframe::kLensControls) {
            if (c.aeIndex < 1 || c.aeIndex > OSV_REFRAME_PARAM_COUNT ||
                c.aeIndex == osv::reframe::kIndexCameraModel || !c.shown || c.shown[0] == '\0') {
                return false;
            }
        }
        return true;
    }(),
    "ReframeParams.h: kLensControls must name real, visible-by-lens controls");

namespace {

/// The three parameter-kind tables describe one list three ways; this proves
/// they agree: every value control has the same kind in the full AE-order
/// table as in the value signature, and every other entry is a group marker.
[[nodiscard]] constexpr bool kindTablesAgree() noexcept {
    int valueSeen = 0;
    for (int aeIndex = 1; aeIndex <= OSV_REFRAME_PARAM_COUNT; ++aeIndex) {
        const osv::reframe::HostParamKind kind = osv::reframe::kParamKindByIndex[aeIndex - 1];
        if (valueSeen < osv::reframe::kValueParamCount && osv::reframe::kValueParamAeIndex[valueSeen] == aeIndex) {
            if (kind != osv::reframe::kValueParamKind[valueSeen]) {
                return false;
            }
            ++valueSeen;
        } else if (kind != osv::reframe::HostParamKind::Group) {
            return false;
        }
    }
    return valueSeen == osv::reframe::kValueParamCount;
}

}  // namespace

static_assert(kindTablesAgree(), "kParamKindByIndex, kValueParamAeIndex and kValueParamKind disagree");

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

/// [WP-CAMERA] The same contract for DJI's numbers: the effect's DJI columns
/// must be exactly osv::geom::kDjiPresets (DJI's own table), all four values
/// plus the tilt.
[[nodiscard]] constexpr bool djiPresetsAgree(osv::reframe::Preset value, std::string_view id) noexcept {
    const osv::reframe::PresetEntry* mine = osv::reframe::presetEntry(value);
    const osv::geom::DjiPreset* theirs = nullptr;
    for (const osv::geom::DjiPreset& p : osv::geom::kDjiPresets) {
        if (std::string_view(p.id) == id) {
            theirs = &p;
        }
    }
    if (!mine || !theirs) {
        return false;
    }
    return mine->djiFovLandscapeDeg == theirs->vfovLandscapeDeg &&
           mine->djiFovPortrait916Deg == theirs->vfovPortrait916Deg &&
           mine->djiFovPortrait34Deg == theirs->vfovPortrait34Deg && mine->correction == theirs->eyeDistance &&
           mine->tiltDeg == theirs->pitchDeg;
}

}  // namespace

static_assert(djiPresetsAgree(osv::reframe::Preset::CrystalBall, "crystal-ball"),
              "the effect's Crystal Ball preset no longer matches osv::geom::kDjiPresets");
static_assert(djiPresetsAgree(osv::reframe::Preset::Asteroid, "asteroid"),
              "the effect's Asteroid preset no longer matches osv::geom::kDjiPresets");
static_assert(djiPresetsAgree(osv::reframe::Preset::Wide, "wide"),
              "the effect's Wide preset no longer matches osv::geom::kDjiPresets");
static_assert(djiPresetsAgree(osv::reframe::Preset::UltraWide, "ultra-wide"),
              "the effect's Ultra Wide preset no longer matches osv::geom::kDjiPresets");
static_assert(djiPresetsAgree(osv::reframe::Preset::Dewarping, "dewarping"),
              "the effect's Dewarping preset no longer matches osv::geom::kDjiPresets");
static_assert(osv::geom::kDjiPresets.size() + 1u == static_cast<std::size_t>(OSV_REFRAME_PRESET_COUNT),
              "a DJI preset was added to one table and not the other");

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

/// Every format the CPU render path can actually produce, in PREFERENCE
/// order - which is also exactly the order they are registered in, because
/// AddSupportedPixelFormat's own documentation (PrSDKAESupport.h:150-157)
/// says the host reads the list as a preference ranking.
///
/// The list is a single named table rather than a run of calls so that
/// "which formats do we advertise" and "which formats does layoutFor()
/// accept" cannot drift apart: a static_assert below pins the two together,
/// and the render path's error message enumerates this same table.
///
/// The order, and why:
///
///   1. BGRA_4444_32f         the native working format.  The panorama is
///                            HDR-capable and reframing is a RESAMPLE, so
///                            any quantisation before it bands the sky.
///   2. BGRA_4444_32f_Linear  the same 16 bytes in the same channel order;
///                            it differs from the above ONLY in transfer
///                            function.  This effect is colour agnostic - it
///                            resamples, it never interprets a code as a
///                            luminance - so accepting it is correct rather
///                            than merely convenient, and refusing it would
///                            be refusing a format we are already able to
///                            render bit-identically.  See layoutFor().
///   3. BGRA_4444_16u         Premiere's 16-bit integer RGB.  Offered so a
///                            10-bit sequence has a high-bit-depth format in
///                            common with us; the SDK guide recommends 32f
///                            over 16u for high bit depth, which is exactly
///                            why 16u sits BELOW both float entries rather
///                            than being left out.
///   4. BGRA_4444_8u          last, so an 8-bit sequence still gets a
///                            native-format render instead of a host
///                            conversion.
///
/// VUYA is deliberately not offered at all: the shared sampler works in
/// RGBA, and converting per sample would cost more than letting the host
/// convert the frame once.
constexpr PrPixelFormat kSupportedFormats[] = {
    PrPixelFormat_BGRA_4444_32f,
    PrPixelFormat_BGRA_4444_32f_Linear,
    PrPixelFormat_BGRA_4444_16u,
    PrPixelFormat_BGRA_4444_8u,
};
constexpr int kSupportedFormatCount = static_cast<int>(sizeof(kSupportedFormats) / sizeof(kSupportedFormats[0]));

/// Decode a Premiere pixel format's fourcc into printable characters.
///
/// PrPixelFormat enumerators are fourcc codes, so an unknown one is far more
/// informative as 'Bgra' than as 0x42677261.  Non-printable bytes become '.'
/// so a value that is NOT a fourcc (a GPU format, a corrupt field) cannot
/// inject control characters into the log line.  Returns the four characters
/// in memory order, most significant byte first, plus a NUL.
struct FourCc {
    char text[5];
};
[[nodiscard]] FourCc fourCcOf(PrPixelFormat format) noexcept {
    FourCc out{};
    const auto bits = static_cast<std::uint32_t>(format);
    for (int i = 0; i < 4; ++i) {
        const auto byte = static_cast<unsigned char>((bits >> (24 - 8 * i)) & 0xFFu);
        // Printable ASCII only; anything else is not part of a fourcc.
        out.text[i] = (byte >= 0x20u && byte < 0x7Fu) ? static_cast<char>(byte) : '.';
    }
    out.text[4] = '\0';
    return out;
}

/// Ask the host for the PF Pixel Format Suite and register kSupportedFormats.
///
/// Returns true when the whole list was registered, false when the suite was
/// not available - which is NOT an error for the caller to propagate (the
/// host then picks a format itself) but IS something the render path wants to
/// know, because it is the difference between "the host chose from our list"
/// and "the host chose blind".
///
/// WHY THIS CAN FAIL, AND WHY IT IS RETRIED
/// ----------------------------------------
/// Adobe's own samples register during PF_Cmd_GLOBAL_SETUP guarded by
/// `appl_id == 'PrMr'`, which is what this effect has always done, and on a
/// real Premiere Pro 26.2 that guard and that ordering are both correct: the
/// live host log shows 43 successful GLOBAL_SETUPs between the first and the
/// last failure, so the suite IS normally there at GLOBAL_SETUP and the
/// registration normally succeeds.
///
/// What the log actually shows is that the suite is MISSING on a MINORITY of
/// GLOBAL_SETUP calls, on their own threads, interleaved with successful ones
/// milliseconds apart.  Premiere calls GLOBAL_SETUP many times over - once
/// per render session, on a pool thread, and also on short-lived probe
/// instances used to enumerate the effect - and on some of those the effect
/// reference is not one the pixel-format machinery is attached to, so the
/// suite legitimately is not offered.  There is no documented way to tell
/// those apart in advance, and `pica_basicP` is perfectly valid on all of
/// them, so there is no pointer to test.  The failure is therefore not a
/// mistake in WHEN we ask; it is a context in which the answer is genuinely
/// "not here".
///
/// The consequence used to be permanent: an instance that missed its one
/// chance at GLOBAL_SETUP never told the host anything, so on a 10-bit
/// sequence the host picked a format outside our old two-entry list and the
/// CPU path refused every frame - stepping and scrubbing showed nothing while
/// playback (the GPU path, which negotiates separately) was fine.  So the
/// registration is retried from the first PF_Cmd_RENDER, where a different
/// effect reference may well have the suite.  There is precedent in this very
/// plug-in: GpuFilter.cpp re-probes its parameter map at render time for the
/// same class of reason - a host that does not answer at setup time may
/// answer later, and the cheap retry is worth strictly more than the
/// assumption.
///
/// Both halves of the fix matter independently, and that is the point.  The
/// retry makes the registration far more likely to land; widening the
/// accepted format set (layoutFor) makes the render CORRECT even when it
/// never lands at all, because the host's unaided choice is then very likely
/// a format we can render anyway.  Neither alone would be enough: a retry
/// that also failed would still black the frame, and a wide format set with
/// no registration would still leave the host free to pick VUYA.
bool registerPixelFormats(PF_InData* in_data, bool atRender) noexcept {
    // The label is used both in the log text and - crucially - as part of the
    // once-key, because PluginLog dedups on the key alone and is process-wide.
    // Sharing one key between the setup and render call sites would mean the
    // FIRST outcome silenced the other for the life of the process, so a
    // successful GLOBAL_SETUP would hide every render-time retry and the log
    // would no longer show the very interleaving that identified this bug.
    const char* const whenLabel = atRender ? "RENDER" : "GLOBAL_SETUP";

    if (!in_data || !in_data->pica_basicP) {
        return false;  // not fatal: the host then picks its default
    }
    // AcquireSuite hands back a const void*; the suite tables themselves are
    // read-only function pointers, so the pointer stays const throughout.
    const void* raw = nullptr;
    const SPErr err = in_data->pica_basicP->AcquireSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, &raw);
    const PF_PixelFormatSuite1* suite = static_cast<const PF_PixelFormatSuite1*>(raw);
    if (err != kSPNoError || !suite) {
        // Report the ERROR CODE and WHICH COMMAND we were in.  Without the
        // code there is no way to tell "suite absent" from "asked at the
        // wrong time", and without the label there is no way to tell a failed
        // GLOBAL_SETUP from a failed render-time retry - which is precisely
        // the distinction that identified this bug.
        PluginLog::oncef(atRender ? "reframe/format/nosuite/render" : "reframe/format/nosuite/setup",
                         PluginLog::Level::Warn,
                         "reframe: PF Pixel Format Suite unavailable at {} (AcquireSuite err {}, suite {}); "
                         "the host will choose the format unaided - the render path accepts all of "
                         "32f / 32f_Linear / 16u / 8u, so this is usually still renderable",
                         whenLabel, static_cast<long>(err), suite ? "non-null" : "null");
        return false;
    }

    // A suite table with null members is not a usable suite.  Checked
    // together, before either call, so the list is never half-registered:
    // clearing and then failing to add would leave the host with an EMPTY
    // list, which is strictly worse than the list it already had.
    if (!suite->ClearSupportedPixelFormats || !suite->AddSupportedPixelFormat) {
        PluginLog::oncef(atRender ? "reframe/format/nomembers/render" : "reframe/format/nomembers/setup",
                         PluginLog::Level::Warn,
                         "reframe: PF Pixel Format Suite v1 at {} has no Clear/Add member; not registering",
                         whenLabel);
        in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);
        return false;
    }

    // Clear first: the host may carry a list from a previous load.
    suite->ClearSupportedPixelFormats(in_data->effect_ref);
    for (const PrPixelFormat format : kSupportedFormats) {
        suite->AddSupportedPixelFormat(in_data->effect_ref, format);
    }
    in_data->pica_basicP->ReleaseSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1);

    PluginLog::oncef(atRender ? "reframe/format/registered/render" : "reframe/format/registered/setup",
                     PluginLog::Level::Info,
                     "reframe: registered {} pixel formats at {} (32f, 32f_Linear, 16u, 8u)", kSupportedFormatCount,
                     whenLabel);
    return true;
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
///
/// BGRA_4444_32f_Linear maps to the SAME layout as BGRA_4444_32f, and that
/// is deliberate, not a shortcut.  The two formats are identical in
/// everything this function describes - four 32-bit floats per pixel in B, G,
/// R, A order - and differ only in the TRANSFER FUNCTION the codes are
/// understood through.  This effect never interprets a code: it computes a
/// direction per output pixel and bilinearly resamples the input at that
/// direction, so every arithmetic operation it performs is a weighted average
/// of neighbouring samples in whatever space they are already in.  A
/// weighted average commutes with nothing about the transfer curve, which is
/// why a colour-managing effect could not do this - but it also means we
/// introduce no error the host has not already accepted by asking a
/// resampler for the frame, and the output carries exactly the tag the host
/// gave the destination world.  Refusing the format would black the frame;
/// accepting it renders it correctly.  (docs/PREMIERE.md records the same
/// reasoning.)
///
/// The two integer layouts are accepted here and PROMOTED to float by the
/// render path before the kernel sees them; see promoteIntegerToFloat.
bool layoutFor(PrPixelFormat format, PixelLayout* out) noexcept {
    if (!out) {
        return false;
    }
    switch (format) {
        case PrPixelFormat_BGRA_4444_32f:
        case PrPixelFormat_BGRA_4444_32f_Linear:
            *out = PixelLayout::Bgra32f;
            return true;
        case PrPixelFormat_BGRA_4444_16u:
            *out = PixelLayout::Bgra16u;
            return true;
        case PrPixelFormat_BGRA_4444_8u:
            *out = PixelLayout::Bgra8u;
            return true;
        default:
            return false;
    }
}

/// Compile-time proof that every format we ADVERTISE is a format we can
/// actually RENDER.
///
/// Advertising a format the render path then refuses is the exact shape of
/// the bug this file was changed to fix, only inverted - so it is worth
/// making unrepresentable rather than merely avoiding.  layoutFor() is
/// constexpr-evaluable through this wrapper because it only switches on its
/// argument, so the whole check happens at compile time and an entry added to
/// kSupportedFormats without a matching case in layoutFor() breaks the build.
[[nodiscard]] constexpr bool everyAdvertisedFormatIsRenderable() noexcept {
    for (const PrPixelFormat format : kSupportedFormats) {
        switch (format) {
            case PrPixelFormat_BGRA_4444_32f:
            case PrPixelFormat_BGRA_4444_32f_Linear:
            case PrPixelFormat_BGRA_4444_16u:
            case PrPixelFormat_BGRA_4444_8u:
                break;
            default:
                return false;
        }
    }
    return true;
}
static_assert(everyAdvertisedFormatIsRenderable(),
              "kSupportedFormats advertises a pixel format layoutFor() does not accept");
static_assert(kSupportedFormatCount == 4, "the documented format list is four entries; docs/PREMIERE.md must agree");

// ===========================================================================
//  Sequence geometry (the "Match Sequence" resolution)
// ===========================================================================

/// Largest sequence edge we will believe.  A host that reports a nonsense
/// rectangle (or one we mis-parsed) must not have its number carried into a
/// camera, so anything beyond this is treated as "could not be determined"
/// and the frame is used instead.  8K is 7680 wide; 65536 leaves enormous
/// headroom while still rejecting a value that is obviously a corrupt read.
constexpr long long kMaxSequenceEdge = 65536;

/// Pixel size of the sequence this effect instance sits on, or an invalid
/// size when it cannot be determined.
///
/// This used to return only the ASPECT RATIO, because the old "Output Aspect"
/// control could not use anything more.  "Output Resolution" needs the real
/// pixel dimensions for its "Match Sequence" entry, and GetFrameRect has
/// always reported exactly those - the ratio was computed from them and the
/// pixels thrown away.  Returning the size loses nothing and is what the
/// control actually asks for.
///
/// On the CPU side the timeline id is not handed to us directly: the PF
/// Utility Suite's GetContainingTimelineID answers it (PrSDKAESupport.h,
/// a v4 member so every host since CC has it), and the Sequence Info Suite
/// then reports the frame rectangle.
SizePx sequenceSize(PF_InData* in_data) noexcept {
    if (!in_data || !in_data->pica_basicP) {
        return SizePx{};
    }
    SPBasicSuite* basic = in_data->pica_basicP;

    // Step 1: which timeline?
    const void* rawUtility = nullptr;
    if (basic->AcquireSuite(kPFUtilitySuite, kPFUtilitySuiteVersion4, &rawUtility) != kSPNoError || !rawUtility) {
        return SizePx{};
    }
    const PF_UtilitySuite4* utility = static_cast<const PF_UtilitySuite4*>(rawUtility);
    if (!utility->GetContainingTimelineID) {
        basic->ReleaseSuite(kPFUtilitySuite, kPFUtilitySuiteVersion4);
        return SizePx{};
    }
    PrTimelineID timeline = 0;
    const PF_Err timelineErr = utility->GetContainingTimelineID(in_data->effect_ref, &timeline);
    basic->ReleaseSuite(kPFUtilitySuite, kPFUtilitySuiteVersion4);
    if (timelineErr != PF_Err_NONE || timeline == 0) {
        return SizePx{};
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
        return SizePx{};
    }

    SizePx size{};
    prRect rect{};
    if (sequence->GetFrameRect && sequence->GetFrameRect(timeline, &rect) == suiteError_NoError) {
        // The rectangle is host-supplied, so it is validated rather than
        // trusted: a right < left or an absurd edge yields an invalid size
        // and "Match Sequence" falls back to the frame.
        const long long w = static_cast<long long>(rect.right) - static_cast<long long>(rect.left);
        const long long h = static_cast<long long>(rect.bottom) - static_cast<long long>(rect.top);
        if (w > 0 && h > 0 && w <= kMaxSequenceEdge && h <= kMaxSequenceEdge) {
            size.w = static_cast<int>(w);
            size.h = static_cast<int>(h);
        }
    }
    basic->ReleaseSuite(kPrSDKSequenceInfoSuite, acquiredVersion);
    return size;
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
    s.resolution = sanitiseResolution(params[kIndexOutputResolution]->u.pd.value);
    s.preset = sanitisePreset(params[kIndexPreset]->u.pd.value);
    s.fovDeg = static_cast<double>(params[kIndexFov]->u.fs_d.value);
    s.distortion = static_cast<double>(params[kIndexDistortion]->u.fs_d.value);
    s.smoothKeyframes = params[kIndexSmooth]->u.bd.value != 0;

    // [WP-LENSUI] The lens is the Lens popup (1-based here: this is the AE
    // parameter array).  A missing entry means the popup's default, DJI -
    // the same lens a host that stored no value for it would restore - and
    // never a crash.  The hidden Camera Model mirror is not read at all.
    s.cameraModel = params[kIndexLens] ? cameraModelFromLensPopup(params[kIndexLens]->u.pd.value)
                                       : kDefaultCameraModel;

    // [WP-CAMERA] The DJI block.  Each entry is null-checked: the host sizes
    // the array from num_params, but a defensive read costs nothing and a
    // missing entry must mean "the default", never a crash.
    if (params[kIndexZoom]) {
        s.zoomDeg = static_cast<double>(params[kIndexZoom]->u.fs_d.value);
    }
    if (params[kIndexDjiFov]) {
        s.djiFovDeg = static_cast<double>(params[kIndexDjiFov]->u.fs_d.value);
    }
    if (params[kIndexCorrection]) {
        s.correction = static_cast<double>(params[kIndexCorrection]->u.fs_d.value);
    }
    if (params[kIndexDragSensitivity]) {
        s.dragSensitivity = static_cast<double>(params[kIndexDragSensitivity]->u.fs_d.value);
    }

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
                  "Part of OpenOSV, the independent open-source DJI Osmo 360 toolkit.\r"
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

    // Premiere only: tell the host which formats we can render.  The guard is
    // Adobe's own (their samples test appl_id == 'PrMr' here too) and is
    // correct - After Effects has no such suite at all.
    //
    // The return value is deliberately ignored HERE: a failure is not a setup
    // failure, and the render path retries on its own (see the retry block in
    // render()).  Reporting it would only turn a recoverable negotiation miss
    // into a host-visible broken effect.
    if (in_data && in_data->appl_id == kPremiereApplId) {
        (void)registerPixelFormats(in_data, /*atRender=*/false);
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

    // ---- 1. Output Resolution ---------------------------------------------
    // Named sizes, not aspect ratios: the render cost of this effect is one
    // kernel evaluation per OUTPUT PIXEL, so the pixel count is the only
    // control that changes how long a frame takes.  "Match Sequence" is the
    // default and reads the real sequence size through the Sequence Info
    // Suite (sequenceSize() below).
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Output Resolution", OSV_REFRAME_RESOLUTION_COUNT, OSV_REFRAME_RESOLUTION_DEFAULT,
                  OSV_REFRAME_RESOLUTION_ITEMS, PF_ParamFlag_SUPERVISE, OSV_REFRAME_ID_OUTPUT_RESOLUTION);

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

    // ---- [WP-CAMERA] 16..20. DJI's camera, appended ------------------------
    // Appended after every existing control so no saved index moves (see
    // ReframeParams.h).
    //
    // 16. Camera Model: WP-CAMERA's lens checkbox, retired by the Lens popup
    //     ([WP-LENSUI], index 21) but kept, because a parameter that
    //     disappears breaks every project saved with it.  Registered
    //     INVISIBLE - AE_Effect.h documents PF_PUI_INVISIBLE for exactly this,
    //     "hidden data parameters" - and kept in step with the popup as its
    //     mirror (see OSV_REFRAME_ID_LENS).  Still supervised: a host that
    //     shows every control lets the user tick it, and that must switch the
    //     lens like the popup does.  Not animatable, like the popup.
    AEFX_CLR_STRUCT(def);
    def.ui_flags = PF_PUI_INVISIBLE;
    PF_ADD_CHECKBOX("Camera Model", "DJI", OSV_REFRAME_CAMERA_MODEL_DEFAULT,
                    PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY, OSV_REFRAME_ID_CAMERA_MODEL);

    // 17. Zoom: DJI's derived visible angle.  Supervised: editing it moves
    //     DJI FOV and Correction Angle along DJI Studio's own zoom path, and
    //     editing either of those refreshes it.  Not animatable, because it is
    //     never rendered from - DJI keyframes FOV and Correction, not Zoom.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Zoom", OSV_REFRAME_ZOOM_VALID_MIN, OSV_REFRAME_ZOOM_VALID_MAX, OSV_REFRAME_ZOOM_SLIDER_MIN,
                         OSV_REFRAME_ZOOM_SLIDER_MAX, OSV_REFRAME_ZOOM_DEFAULT, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_NONE, PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY,
                         OSV_REFRAME_ID_ZOOM);

    // 18. DJI FOV: the vertical pinhole field of view, one decimal like DJI.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("DJI FOV", OSV_REFRAME_DJI_FOV_VALID_MIN, OSV_REFRAME_DJI_FOV_VALID_MAX,
                         OSV_REFRAME_DJI_FOV_SLIDER_MIN, OSV_REFRAME_DJI_FOV_SLIDER_MAX, OSV_REFRAME_DJI_FOV_DEFAULT,
                         PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, PF_ParamFlag_SUPERVISE, OSV_REFRAME_ID_DJI_FOV);

    // 19. Correction Angle: the eye distance, two decimals like DJI.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Correction Angle", OSV_REFRAME_CORRECTION_VALID_MIN, OSV_REFRAME_CORRECTION_VALID_MAX,
                         OSV_REFRAME_CORRECTION_SLIDER_MIN, OSV_REFRAME_CORRECTION_SLIDER_MAX,
                         OSV_REFRAME_CORRECTION_DEFAULT, PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE,
                         PF_ParamFlag_SUPERVISE, OSV_REFRAME_ID_CORRECTION);

    // 20. Drag Sensitivity: an overlay preference, never rendered from, so
    //     not animatable and not supervised.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Drag Sensitivity", OSV_REFRAME_DRAG_SENSITIVITY_VALID_MIN,
                         OSV_REFRAME_DRAG_SENSITIVITY_VALID_MAX, OSV_REFRAME_DRAG_SENSITIVITY_SLIDER_MIN,
                         OSV_REFRAME_DRAG_SENSITIVITY_SLIDER_MAX, OSV_REFRAME_DRAG_SENSITIVITY_DEFAULT,
                         PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, PF_ParamFlag_CANNOT_TIME_VARY,
                         OSV_REFRAME_ID_DRAG_SENSITIVITY);

    // ---- [WP-LENSUI] 21. Lens ----------------------------------------------
    // "DJI | Classic", DJI by default: which lens renders and which lens's
    // controls the panel shows.  Appended, not placed at the top of the
    // Camera group, because only an append is safe however the host binds
    // saved values (ReframeParams.h, OSV_REFRAME_ID_LENS).  Supervised:
    // switching carries the look across and keeps the hidden Camera Model
    // mirror in step.  Not animatable, for the reason Camera Model was not: a
    // lens that changes mid-shot is not a framing anybody keyframes, and a
    // host-held keyframe would make the conversion ambiguous.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Lens", OSV_REFRAME_LENS_COUNT, OSV_REFRAME_LENS_DEFAULT, OSV_REFRAME_LENS_ITEMS,
                  PF_ParamFlag_SUPERVISE | PF_ParamFlag_CANNOT_TIME_VARY, OSV_REFRAME_ID_LENS);

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

// ---------------------------------------------------------------------------
//  [WP-CAMERA] Writing supervised values back
// ---------------------------------------------------------------------------

/// Write a float slider and mark it as an undoable edit.  A null entry (a
/// host that handed a short array) is skipped, never dereferenced.
void writeSlider(PF_ParamDef* def, double value) noexcept {
    if (!def || !std::isfinite(value)) {
        return;
    }
    def->u.fs_d.value = static_cast<PF_FpShort>(value);
    def->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
}

/// Write a popup (1-based) and mark it changed - only when it actually
/// changes, so an unchanged popup never gets a redundant undo step.
void writePopup(PF_ParamDef* def, int value) noexcept {
    if (!def || def->u.pd.value == static_cast<A_long>(value)) {
        return;
    }
    def->u.pd.value = static_cast<A_long>(value);
    def->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
}

/// [WP-LENSUI] Select a lens: the Lens popup and its hidden Camera Model
/// mirror, each marked changed only when it actually changes - so the
/// control the user just set is never re-marked, and a lens that is already
/// selected adds nothing to the undo step.  A null entry is skipped.
void writeLens(PF_ParamDef* params[], CameraModel model) noexcept {
    if (!params) {
        return;
    }
    // The popup: the source of truth.
    if (PF_ParamDef* lens = params[kIndexLens]) {
        if (lens->u.pd.value != static_cast<A_long>(lensPopupValue(model))) {
            // Comparing the raw value, not the model it reads as, also
            // normalises an out-of-range value that merely READS as the
            // right lens (a corrupt project's 7 reads as DJI), so the host is
            // left holding a real entry.
            lens->u.pd.value = static_cast<A_long>(lensPopupValue(model));
            lens->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        }
    }
    // The mirror: ticked is DJI, exactly as the build before this one reads it.
    if (PF_ParamDef* mirror = params[kIndexCameraModel]) {
        if (cameraModelFromCheckbox(mirror->u.bd.value) != model) {
            mirror->u.bd.value = (model == CameraModel::Dji) ? 1 : 0;
            mirror->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        }
    }
}

/// Flip Preset to Custom after a manual edit: the look is no longer the
/// preset, and a popup that still named it would be lying.
void presetToCustom(PF_ParamDef* params[], PF_ParamIndex changed) noexcept {
    if (!params || !params[kIndexPreset]) {
        return;
    }
    if (sanitisePreset(params[kIndexPreset]->u.pd.value) != Preset::Custom) {
        writePopup(params[kIndexPreset], static_cast<int>(Preset::Custom));
        PluginLog::debug("reframe: manual edit of param {} -> preset Custom", static_cast<int>(changed));
    }
}

/// The frame shape the DJI numbers are computed for: the named Output
/// Resolution, else the sequence, else DJI's 16:9 (framingAspect()).
[[nodiscard]] double framingAspectFor(PF_InData* in_data, PF_ParamDef* params[]) noexcept {
    const Resolution resolution = (params && params[kIndexOutputResolution])
                                      ? sanitiseResolution(params[kIndexOutputResolution]->u.pd.value)
                                      : Resolution::MatchSequence;
    return framingAspect(resolution, sequenceSize(in_data));
}

/// The Classic lens as the controls currently hold it.
[[nodiscard]] ClassicLens classicLensOf(PF_ParamDef* params[]) noexcept {
    ClassicLens lens;
    if (params && params[kIndexFov]) {
        lens.fovDeg = static_cast<double>(params[kIndexFov]->u.fs_d.value);
    }
    if (params && params[kIndexDistortion]) {
        lens.distortion = static_cast<double>(params[kIndexDistortion]->u.fs_d.value);
    }
    return lens;
}

/// The DJI lens as the controls currently hold it, sanitised.
[[nodiscard]] DjiLens djiLensOf(PF_ParamDef* params[]) noexcept {
    DjiLens lens;
    if (params && params[kIndexDjiFov]) {
        lens.fovDeg = static_cast<double>(params[kIndexDjiFov]->u.fs_d.value);
    }
    if (params && params[kIndexCorrection]) {
        lens.correction = static_cast<double>(params[kIndexCorrection]->u.fs_d.value);
    }
    return sanitiseDjiLens(lens);
}

/// Write a whole DJI lens back, with the Zoom read-out that belongs to it.
void writeDjiLens(PF_ParamDef* params[], const DjiLens& lens, double aspect, bool writeFov,
                  bool writeCorrection) noexcept {
    if (!params) {
        return;
    }
    if (writeFov) {
        writeSlider(params[kIndexDjiFov], lens.fovDeg);
    }
    if (writeCorrection) {
        writeSlider(params[kIndexCorrection], lens.correction);
    }
    writeSlider(params[kIndexZoom], djiZoomDeg(lens, aspect));
}

/// [WP-LENSUI] The lens the Lens popup selects - the lens on screen.  A
/// missing entry is the popup's default, exactly as readSettings() reads it.
[[nodiscard]] CameraModel selectedLens(PF_ParamDef* params[]) noexcept {
    if (!params || !params[kIndexLens]) {
        return kDefaultCameraModel;
    }
    return cameraModelFromLensPopup(params[kIndexLens]->u.pd.value);
}

[[nodiscard]] bool isDjiModel(PF_ParamDef* params[]) noexcept { return selectedLens(params) == CameraModel::Dji; }

/// [WP-LENSUI] Carry the picture from one lens's controls to the other's,
/// so switching lenses does not make the framing jump.
///
/// `to` is the lens now selected.  Classic -> DJI is exact (djiFromClassic);
/// DJI -> Classic is exact unless the Classic ramp or a Correction above 1
/// makes the DJI look unrepresentable, and then the nearest Classic look
/// (classicFromDji).  Only the controls of the lens switched TO are written.
void carryLookTo(PF_InData* in_data, PF_ParamDef* params[], CameraModel to) noexcept {
    if (!params) {
        return;
    }
    const double aspect = framingAspectFor(in_data, params);
    if (to == CameraModel::Dji) {
        // Classic -> DJI: DJI's controls take the Classic look, Zoom with it.
        writeDjiLens(params, djiFromClassic(classicLensOf(params), aspect), aspect, true, true);
    } else {
        // DJI -> Classic: the nearest Classic look.
        const ClassicLens classic = classicFromDji(djiLensOf(params), aspect);
        writeSlider(params[kIndexFov], classic.fovDeg);
        writeSlider(params[kIndexDistortion], classic.distortion);
    }
}

/// PF_Cmd_USER_CHANGED_PARAM: the supervised behaviour.
///
/// Changing Preset writes the preset's look - the Classic FOV / Distortion,
/// DJI's FOV / Correction Angle / Zoom for the frame's shape, and Tilt - and
/// selects the DJI lens, marking each value with PF_ChangeFlag_CHANGED_VALUE
/// so the host records one undoable edit.
///
/// [WP-LENSUI] Picking a lens in the Lens popup converts the current look
/// into that lens's controls (carryLookTo), updates the hidden Camera Model
/// mirror and flips Preset to Custom; the panel then shows that lens's
/// controls when the host sends PF_Cmd_UPDATE_PARAMS_UI.  The mirror holds
/// the lens that was on screen BEFORE the edit, which is how a re-pick of the
/// lens already selected is told from a switch: it converts nothing.  Ticking
/// or unticking the mirror itself - only a host that ignores PF_PUI_INVISIBLE
/// shows it - is the same switch, with the popup as the thing updated.
///
/// Editing a lens control by hand means the look is no longer the preset, so
/// Preset flips to Custom - which is exactly how every other reframe UI
/// behaves and stops the popup from lying.  [WP-CAMERA] It also selects the
/// lens that control belongs to, carrying the current look across so the
/// picture does not jump.  With one lens's controls shown at a time this only
/// happens through a host that shows them all, or through a keyframe:
///
///   * DJI FOV / Correction Angle: Lens -> DJI; if it was Classic, the OTHER
///     DJI control is set from the Classic look first; Zoom is refreshed.
///   * Zoom: Lens -> DJI, and DJI FOV / Correction Angle move along DJI
///     Studio's zoom path (fov += 130 d, correction += d) until the lens
///     shows that Zoom; Zoom is then rewritten with what was reached (a
///     request past DJI's limits stops at them, as DJI's own does).
///   * Classic FOV / Distortion: Lens -> Classic.
PF_Err userChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                        const PF_UserChangedParamExtra* extra) noexcept {
    if (!params || !extra) {
        return PF_Err_NONE;
    }

    const PF_ParamIndex changed = extra->param_index;
    // A host index outside our list is not a control of ours to supervise.
    if (changed < 1 || changed > OSV_REFRAME_PARAM_COUNT || !params[changed]) {
        return PF_Err_NONE;
    }

    if (changed == kIndexPreset) {
        const Preset preset = sanitisePreset(params[kIndexPreset]->u.pd.value);
        const PresetEntry* entry = presetEntry(preset);
        if (!entry || !entry->writesControls) {
            return PF_Err_NONE;  // "Custom" writes nothing
        }
        writeSlider(params[kIndexFov], entry->fovDeg);
        writeSlider(params[kIndexDistortion], entry->distortion);
        // The angle control stores fixed 16.16 degrees.
        if (params[kIndexTilt]) {
            params[kIndexTilt]->u.ad.value = static_cast<PF_Fixed>(std::lround(entry->tiltDeg * 65536.0));
            params[kIndexTilt]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        }
        // [WP-CAMERA] DJI's numbers for the same look, for this frame's shape
        // (DJI keeps separate landscape / 9:16 / 3:4 columns), and the switch
        // to the DJI model that makes them the picture.
        const double aspect = framingAspectFor(in_data, params);
        const DjiLens lens = sanitiseDjiLens(DjiLens{djiPresetFovDeg(*entry, aspect), entry->correction});
        writeDjiLens(params, lens, aspect, /*writeFov=*/true, /*writeCorrection=*/true);
        writeLens(params, CameraModel::Dji);
        PluginLog::debug("reframe: preset '{}' -> classic fov {} distortion {}, DJI fov {} correction {} "
                         "(aspect {:.4f}), tilt {}",
                         entry->label, entry->fovDeg, entry->distortion, lens.fovDeg, lens.correction, aspect,
                         entry->tiltDeg);
        return PF_Err_NONE;
    }

    if (changed == kIndexFov || changed == kIndexDistortion) {
        presetToCustom(params, changed);
        // A Classic control was edited: the Classic lens is the picture now.
        writeLens(params, CameraModel::Classic);
        return PF_Err_NONE;
    }

    if (changed == kIndexTilt) {
        presetToCustom(params, changed);
        return PF_Err_NONE;
    }

    // ---- [WP-CAMERA] the DJI controls --------------------------------------
    if (changed == kIndexDjiFov || changed == kIndexCorrection) {
        const double aspect = framingAspectFor(in_data, params);
        DjiLens lens = djiLensOf(params);
        if (!isDjiModel(params)) {
            // Coming from Classic: the control the user did NOT touch takes
            // its value from the current look, so only their edit shows.
            const DjiLens carried = djiFromClassic(classicLensOf(params), aspect);
            if (changed == kIndexDjiFov) {
                lens.correction = carried.correction;
            } else {
                lens.fovDeg = carried.fovDeg;
            }
        }
        writeDjiLens(params, lens, aspect, /*writeFov=*/changed != kIndexDjiFov,
                     /*writeCorrection=*/changed != kIndexCorrection);
        writeLens(params, CameraModel::Dji);
        presetToCustom(params, changed);
        return PF_Err_NONE;
    }

    if (changed == kIndexZoom) {
        const double aspect = framingAspectFor(in_data, params);
        const double target = static_cast<double>(params[kIndexZoom]->u.fs_d.value);
        // Start from the lens that is on screen, whichever model draws it.
        const DjiLens from = isDjiModel(params) ? djiLensOf(params) : djiFromClassic(classicLensOf(params), aspect);
        const DjiLens to = djiZoomTo(target, from, aspect);
        writeDjiLens(params, to, aspect, /*writeFov=*/true, /*writeCorrection=*/true);
        writeLens(params, CameraModel::Dji);
        presetToCustom(params, changed);
        PluginLog::debug("reframe: zoom {} -> DJI fov {} correction {} (aspect {:.4f})", target, to.fovDeg,
                         to.correction, aspect);
        return PF_Err_NONE;
    }

    // ---- [WP-LENSUI] a lens switch: the Lens popup, or its hidden mirror ----
    if (changed == kIndexLens || changed == kIndexCameraModel) {
        // What the user asked for, and what was on screen before the edit.
        // The control NOT edited still holds the old lens: the mirror when
        // the popup moved, the popup when the mirror was ticked.
        const bool fromPopup = (changed == kIndexLens);
        const CameraModel requested = fromPopup ? cameraModelFromLensPopup(params[kIndexLens]->u.pd.value)
                                                : cameraModelFromCheckbox(params[kIndexCameraModel]->u.bd.value);
        const CameraModel previous =
            fromPopup ? (params[kIndexCameraModel] ? cameraModelFromCheckbox(params[kIndexCameraModel]->u.bd.value)
                                                   // No mirror to ask: treat it as a real switch.
                                                   : (requested == CameraModel::Dji ? CameraModel::Classic
                                                                                    : CameraModel::Dji))
                      : selectedLens(params);

        // Popup and mirror now agree on the requested lens (the edited
        // control already says it, so only the other one is written).
        writeLens(params, requested);

        // A real switch carries the look across.  A re-pick of the lens
        // already on screen converts nothing.
        const bool switched = (requested != previous);
        if (switched) {
            carryLookTo(in_data, params, requested);
        }
        // Preset stops naming a look the numbers no longer are after a
        // switch.  And Classic is ALWAYS left beside "Custom", even on a
        // re-pick: the GPU path decodes Classic's ambiguous "1" on Premiere
        // with the 0 that Custom reads there (ReframeParams.h,
        // OSV_REFRAME_LENS_ITEMS), and a project whose mirror went stale
        // (a WP-CAMERA project saved unticked, then opened on DJI) reaches
        // Classic through what looks like a re-pick.  On a true Classic
        // re-pick Preset is already Custom and nothing is written.
        if (switched || requested == CameraModel::Classic) {
            presetToCustom(params, changed);
        }

        // Ask for the Effect Controls panel to be redrawn, which is what
        // brings the PF_Cmd_UPDATE_PARAMS_UI that shows the selected lens's
        // controls - on every pick, because a stale mirror can make a real
        // change of the panel look like a re-pick.  Adobe's Supervisor sample
        // returns the same flag from USER_CHANGED_PARAM when a mode popup
        // changes which controls are visible (Supervisor.cpp,
        // UserChangedParam).  The visibility itself is deliberately NOT set
        // here: Premiere 25 was reported to ignore PF_PUI_INVISIBLE changes
        // made during USER_CHANGED_PARAM on the first instance of an effect
        // (Adobe tracking DVARC-3737), and the workaround is to make them in
        // UPDATE_PARAMS_UI only.
        if (out_data) {
            out_data->out_flags |= PF_OutFlag_REFRESH_UI;
        }
        PluginLog::debug("reframe: lens {} -> {} (from the {}{})", previous == CameraModel::Dji ? "DJI" : "Classic",
                         requested == CameraModel::Dji ? "DJI" : "Classic",
                         fromPopup ? "Lens popup" : "Camera Model checkbox", switched ? "" : ", nothing to convert");
        return PF_Err_NONE;
    }

    return PF_Err_NONE;
}

// ---------------------------------------------------------------------------
//  [WP-LENSUI] Showing one lens at a time
// ---------------------------------------------------------------------------

/// The controls whose Effect Controls visibility the Lens popup decides: the
/// five lens-specific controls and the hidden Camera Model mirror.
constexpr int kManagedUiIndices[] = {kIndexCameraModel, kIndexFov,    kIndexDistortion,
                                     kIndexZoom,        kIndexDjiFov, kIndexCorrection};

/// Re-apply the slider display fields PF_UpdateParamUI is documented to
/// change (AE_EffectSuites.h, PF_UpdateParamUI: "slider_min, slider_max,
/// precision, display_flags of any slider type") from the SAME constants
/// PF_Cmd_PARAMS_SETUP registers.  The def handed to PF_UpdateParamUI is the
/// host's copy with our changes on top, and a host whose copy arrived with
/// empty slider fields would otherwise have its slider collapsed to 0..0 by
/// our own update.  A test compares these with the registered values, so
/// the two lists cannot drift apart silently.  Non-slider indices are left
/// alone.
void restoreSliderDisplay(PF_ParamDef& def, int aeIndex) noexcept {
    // One place per slider, the PARAMS_SETUP arguments verbatim.
    double sliderMin = 0.0;
    double sliderMax = 0.0;
    A_short precision = PF_Precision_TENTHS;
    PF_ValueDisplayFlags display = PF_ValueDisplayFlag_NONE;
    switch (aeIndex) {
        case kIndexFov:
            sliderMin = OSV_REFRAME_FOV_SLIDER_MIN;
            sliderMax = OSV_REFRAME_FOV_SLIDER_MAX;
            break;
        case kIndexDistortion:
            sliderMin = OSV_REFRAME_DISTORTION_SLIDER_MIN;
            sliderMax = OSV_REFRAME_DISTORTION_SLIDER_MAX;
            display = PF_ValueDisplayFlag_PERCENT;
            break;
        case kIndexZoom:
            sliderMin = OSV_REFRAME_ZOOM_SLIDER_MIN;
            sliderMax = OSV_REFRAME_ZOOM_SLIDER_MAX;
            break;
        case kIndexDjiFov:
            sliderMin = OSV_REFRAME_DJI_FOV_SLIDER_MIN;
            sliderMax = OSV_REFRAME_DJI_FOV_SLIDER_MAX;
            break;
        case kIndexCorrection:
            sliderMin = OSV_REFRAME_CORRECTION_SLIDER_MIN;
            sliderMax = OSV_REFRAME_CORRECTION_SLIDER_MAX;
            precision = PF_Precision_HUNDREDTHS;
            break;
        default:
            return;  // not one of our float sliders
    }
    def.u.fs_d.slider_min = static_cast<PF_FpShort>(sliderMin);
    def.u.fs_d.slider_max = static_cast<PF_FpShort>(sliderMax);
    def.u.fs_d.precision = precision;
    def.u.fs_d.display_flags = display;
}

/// The def PF_UpdateParamUI receives for one managed control while `lens` is
/// selected.
///
/// It starts from the host's own def when there is one - Adobe's Supervisor
/// sample passes a COPY of the params entry, because the array handed to
/// UPDATE_PARAMS_UI is the host's and must not be written - and then sets
/// every field the update is allowed to change from this effect's own
/// constants, so the result never depends on what the host left in them:
/// the type, the name (a copy that arrived nameless would otherwise blank the
/// label - a Premiere report of exactly that is in docs/PREMIERE.md), the
/// PF_PUI_INVISIBLE bit and the slider display.  No value field is touched,
/// and no change flag is set: UPDATE_PARAMS_UI may only make cosmetic
/// changes (AE_Effect.h, PF_Cmd_UPDATE_PARAMS_UI).
[[nodiscard]] PF_ParamDef managedUiDef(const PF_ParamDef* host, int aeIndex, CameraModel lens) noexcept {
    PF_ParamDef def{};
    if (host) {
        def = *host;
    }
    const bool visible = controlVisible(aeIndex, lens);
    if (aeIndex == kIndexCameraModel) {
        // The hidden mirror: a checkbox, under its registered name and label.
        def.param_type = PF_Param_CHECKBOX;
        std::snprintf(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "%s", "Camera Model");
        // The checkbox's own label: a def built from nothing (no host copy)
        // must not hand the host a null label pointer.
        if (!def.u.bd.u.PF_DEF_NAMEPTR) {
            def.u.bd.u.PF_DEF_NAMEPTR = "DJI";
        }
    } else {
        // A lens-specific float slider, under the name it is SHOWN with.
        const LensControl* c = lensControl(aeIndex);
        def.param_type = PF_Param_FLOAT_SLIDER;
        std::snprintf(def.PF_DEF_NAME, sizeof(def.PF_DEF_NAME), "%s", (c && c->shown) ? c->shown : "");
        restoreSliderDisplay(def, aeIndex);
    }
    if (visible) {
        def.ui_flags &= ~static_cast<PF_ParamUIFlags>(PF_PUI_INVISIBLE);
    } else {
        def.ui_flags |= static_cast<PF_ParamUIFlags>(PF_PUI_INVISIBLE);
    }
    return def;
}

/// PF_Cmd_UPDATE_PARAMS_UI: show the selected lens's controls, hide the
/// other lens's, and keep the Camera Model mirror hidden.
///
/// HOW, AND THE EVIDENCE THAT PREMIERE DOES IT
/// -------------------------------------------
/// PF_UpdateParamUI (PF Param Utils Suite v3) with PF_PUI_INVISIBLE set or
/// cleared, for each managed control:
///
///   * AE_Effect.h, PF_PUI_INVISIBLE: "in Premiere since earlier than [CS6],
///     this hides the parameter UI in the Effect Controls, which includes the
///     keyframe track; for PPro only, the flag is dynamic and can be cleared
///     to make the parameter visible again";
///   * AE_EffectSuites.h, PF_UpdateParamUI: the fields it may change are
///     "ui_flags: PF_PUI_ECW_SEPARATOR, PF_PUI_DISABLED only (and
///     PF_PUI_INVISIBLE in Premiere)", the name, and the slider display;
///   * Adobe's Supervisor sample hides and shows its advanced controls in
///     Premiere exactly this way, from UPDATE_PARAMS_UI (its After Effects
///     branch needs the AEGP Dynamic Stream Suite instead, which Premiere does
///     not provide).
///
/// The update is applied on EVERY call, not only when the host's copy looks
/// different: nothing documents that the params array handed to
/// UPDATE_PARAMS_UI reflects earlier PF_UpdateParamUI calls, and trusting it
/// would risk a control that stays hidden after switching back.  Six calls
/// per panel refresh cost nothing measurable.
///
/// Every failure is non-fatal and returns PF_Err_NONE: a host without the
/// suite, or one that refuses an update, shows every control - the layout
/// the effect had before this - which is a worse panel but a working effect.
PF_Err updateParamsUi(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) noexcept {
    (void)out_data;
    if (!in_data || !params) {
        return PF_Err_NONE;  // nothing to show; never an error
    }
    const CameraModel lens = selectedLens(params);

    // ---- the suite ---------------------------------------------------------
    if (!in_data->pica_basicP) {
        PluginLog::oncef("reframe/ui/params/nobasic", PluginLog::Level::Warn,
                         "reframe: UPDATE_PARAMS_UI without a suite table; every lens control stays visible");
        return PF_Err_NONE;
    }
    const void* raw = nullptr;
    const SPErr acquired = in_data->pica_basicP->AcquireSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, &raw);
    const PF_ParamUtilsSuite3* suite = static_cast<const PF_ParamUtilsSuite3*>(raw);
    if (acquired != kSPNoError || !suite) {
        PluginLog::oncef("reframe/ui/params/nosuite", PluginLog::Level::Warn,
                         "reframe: the PF Param Utils Suite v3 is unavailable (AcquireSuite err {}); every lens "
                         "control stays visible",
                         static_cast<long>(acquired));
        return PF_Err_NONE;
    }
    if (!suite->PF_UpdateParamUI) {
        in_data->pica_basicP->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);
        PluginLog::oncef("reframe/ui/params/nomember", PluginLog::Level::Warn,
                         "reframe: the PF Param Utils Suite v3 has no PF_UpdateParamUI; every lens control stays "
                         "visible");
        return PF_Err_NONE;
    }

    // ---- one update per managed control ------------------------------------
    int shown = 0;
    int hidden = 0;
    for (const int aeIndex : kManagedUiIndices) {
        const PF_ParamDef def = managedUiDef(params[aeIndex], aeIndex, lens);
        const PF_Err err = suite->PF_UpdateParamUI(in_data->effect_ref, aeIndex, &def);
        if (err != PF_Err_NONE) {
            // One refused control is not a reason to leave the others wrong.
            PluginLog::oncef("reframe/ui/params/refused", PluginLog::Level::Warn,
                             "reframe: PF_UpdateParamUI refused parameter {} (err {}); the panel may show a "
                             "control of the other lens",
                             aeIndex, static_cast<long>(err));
            continue;
        }
        // Counted for the one debug line below.
        if ((def.ui_flags & PF_PUI_INVISIBLE) != 0) {
            ++hidden;
        } else {
            ++shown;
        }
    }
    in_data->pica_basicP->ReleaseSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3);

    PluginLog::debug("reframe: Effect Controls show the {} lens ({} controls shown, {} hidden)",
                     lens == CameraModel::Dji ? "DJI" : "Classic", shown, hidden);
    return PF_Err_NONE;
}

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

    // ---- second chance at the format negotiation --------------------------
    // If GLOBAL_SETUP could not acquire the PF Pixel Format Suite, this
    // effect instance never told the host what it can render, and the host
    // picked a format unaided.  That is the root of the "plays but does not
    // update when I step" bug: the live host log shows the suite missing on
    // some GLOBAL_SETUP calls, after which a 10-bit sequence got a format the
    // CPU path refused - while playback, which goes through the separately
    // negotiated GPU entry, kept working.
    //
    // It cannot help the CURRENT frame - the worlds are already allocated -
    // which is precisely why widening layoutFor() was the other half of the
    // fix; what it does is stop the miss from being permanent for the rest of
    // the session.
    //
    // The retry is UNCONDITIONAL rather than memoised, and that is a
    // deliberate choice about correctness over a micro-optimisation.  The
    // registration is keyed on `effect_ref` (PrSDKAESupport.h:104-106), so a
    // memo would have to be keyed on it too - and an effect_ref is a foreign
    // pointer with no destruction hook this effect receives.  (SEQUENCE_SETDOWN
    // does not reliably pair with it, and this effect deliberately keeps
    // sequence_data null.)  A table of such pointers can therefore be matched
    // by a NEW reference that the host allocated at a recycled address, and
    // would then suppress the retry for the one instance that actually needs
    // it - reintroducing this very bug, intermittently and unreproducibly.
    //
    // The cost of not memoising is provably negligible: AcquireSuite is a
    // name lookup in the host's suite table, and this same function already
    // acquires this same suite TWICE per frame in worldFormat() below, so the
    // retry adds a third to a path that then does width * height kernel
    // evaluations.  A correctness hazard is not worth a third of nothing.
    if (in_data->appl_id == kPremiereApplId) {
        (void)registerPixelFormats(in_data, /*atRender=*/true);
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
    const bool inOk = layoutFor(inFormat, &inLayout);
    const bool outOk = layoutFor(outFormat, &outLayout);
    if (!inOk || !outOk) {
        // Degrade gracefully with ONE clear line that NAMES the format.  A
        // bare hex word is nearly useless in a bug report: PrPixelFormat
        // enumerators are fourccs, so the offending format is spelled out as
        // characters as well, and the line says which SIDE was wrong and what
        // we would have accepted instead.  A silent refusal here is what made
        // this bug take a live host log to find.
        const FourCc inCc = fourCcOf(inFormat);
        const FourCc outCc = fourCcOf(outFormat);
        PluginLog::oncef("reframe/render/format", PluginLog::Level::Error,
                         "reframe: unsupported world format - input '{}' (0x{:08X}) {}, output '{}' (0x{:08X}) {}; "
                         "this effect renders BGRA_4444_32f, _32f_Linear, _16u and _8u only, so the frame is "
                         "declined rather than misread",
                         inCc.text, static_cast<unsigned>(inFormat), inOk ? "ok" : "REJECTED", outCc.text,
                         static_cast<unsigned>(outFormat), outOk ? "ok" : "REJECTED");
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

    // ---- promote an integer-coded input -----------------------------------
    // GLOBAL_SETUP advertises PrPixelFormat_BGRA_4444_8u AND _16u, so the
    // host is entitled to hand us either, and an effect that advertises a
    // format must accept it.  The shared sampler reads float or half only, so
    // the frame is promoted once into a scratch buffer and rendered from
    // that.  (This used to return PF_Err_BAD_CALLBACK_PARAM in the hope the
    // host would retry with 32f, which nothing in the SDK promises: a host
    // that took us at our word got a hard error on every frame.)
    //
    // `promoted` must outlive `src`, so it is declared in this scope.
    std::vector<float> promoted;
    if (layoutNeedsPromotion(src.layout)) {
        const ConstFrameView promotedView = promoteIntegerToFloat(src, promoted, pool);
        if (!promotedView.valid()) {
            PluginLog::oncef("reframe/render/promote-in", PluginLog::Level::Error,
                             "reframe: could not promote a {}-bit integer input world ({}x{})",
                             src.layout == PixelLayout::Bgra16u ? 16 : 8, src.width, src.height);
            return PF_Err_OUT_OF_MEMORY;
        }
        src = promotedView;
    }

    // ---- parameters and geometry ------------------------------------------
    const Settings settings = readSettings(in_data, params);
    const KernelSetup setup = buildParams(settings, src, dst.width, dst.height, sequenceSize(in_data));
    if (!setup.valid) {
        // Log the SETTINGS too, not just the sizes: every rejection inside
        // buildParams() is driven by a parameter value, so the sizes alone
        // never say which one.  This is the line that identifies whether the
        // CPU path read a sane FOV or garbage.
        PluginLog::oncef("reframe/render/setup-values", PluginLog::Level::Error,
                         "reframe: setup rejected with resolution={} preset={} lens={} fov={:.3f} distortion={:.3f} "
                         "djiFov={:.3f} correction={:.3f} pan={:.3f} tilt={:.3f} roll={:.3f} srcPan={:.3f} "
                         "srcTilt={:.3f} srcRoll={:.3f} smooth={} seq={}x{}",
                         static_cast<int>(settings.resolution), static_cast<int>(settings.preset),
                         settings.cameraModel == CameraModel::Dji ? "DJI" : "Classic", settings.fovDeg,
                         settings.distortion, settings.djiFovDeg, settings.correction, settings.panDeg,
                         settings.tiltDeg, settings.rollDeg,
                         settings.sourcePanDeg, settings.sourceTiltDeg, settings.sourceRollDeg,
                         settings.smoothKeyframes ? 1 : 0, sequenceSize(in_data).w, sequenceSize(in_data).h);
        PluginLog::oncef("reframe/render/setup", PluginLog::Level::Error,
                         "reframe: could not build the kernel parameters ({}x{} -> {}x{}): {} "
                         "[src layout={} rowBytes={} topDown={}]",
                         src.width, src.height, dst.width, dst.height, setupRejectName(setup.reject),
                         static_cast<int>(src.layout), src.rowBytes, src.topDown ? 1 : 0);
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
