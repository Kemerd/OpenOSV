// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SourceSettingsMain.cpp - the whole of OpenOSVSourceSettings.aex.
//
// This is a Premiere Pro "source settings effect": an After Effects API
// effect that the host attaches to the MASTER CLIP automatically, rather than
// one a user drags onto a timeline clip.  Its parameters appear in the Effect
// Controls panel whenever the clip is selected, and their values are handed
// to OpenOSVImporter.prm as a flat preferences blob.  It exists so the stitch
// options (colour output, output size, stabilisation, seam search, exposure
// match, calibration slot, D-Log M curve, exposure, render device, the
// Rec.709 look and the reframe effect's Program Monitor Colour) are simply
// VISIBLE, instead of hiding behind the modal dialog in imGetPrefs8.
//
// How the two halves find each other: the importer sets
// imImportInfoRec::hasSourceSettingsEffect and puts this effect's match name
// into imFileInfoRec8::sourceSettingsMatchName; Premiere then instantiates
// the installed effect with that PiPL match name.  Both sides read the string
// from plugins/common/SourceSettingsIdentity.h, and a test compares the built
// module's PiPL to it, so they cannot drift.
//
// Command flow, in call order:
//
//   PF_Cmd_ABOUT                     the About box text.
//   PF_Cmd_GLOBAL_SETUP              version + out-flags (byte-identical to
//                                    the PiPL), and - the whole point -
//                                    SetIsSourceSettingsEffect(), which is
//                                    what tells Premiere this is a master
//                                    clip settings effect and not a filter.
//   PF_Cmd_PARAMS_SETUP              the eleven controls, each flagged
//                                    PF_ParamFlag_CANNOT_TIME_VARY.
//   PF_Cmd_SEQUENCE_SETUP            PerformSourceSettingsCommand(), which
//                                    round-trips a blob through the importer
//                                    so the controls can be seeded from the
//                                    media's own "as shot" settings.
//   PF_Cmd_TRANSLATE_PARAMS_TO_PREFS the controls -> a 128-byte PrefsBlob
//                                    written into the host's prefs buffer.
//   PF_Cmd_GLOBAL_SETDOWN            close the log.
//
// ===========================================================================
//  WHAT THIS EFFECT DELIBERATELY CANNOT DO
// ===========================================================================
//
// It cannot reframe, and it never will.  Two SDK facts make that structural:
//
//   1. A source settings effect is NEVER sent PF_Cmd_RENDER.  It sits on the
//      master clip, describing how to DECODE the media; there is no frame
//      passing through it to filter.  A render handler here would be dead
//      code, which is why there is not one.
//   2. Its values reach the importer only through
//      PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, which fills ONE FLAT BLOB
//      (PF_TranslateParamsToPrefsExtra::prefsPC, AE_Effect.h:2177-2190).
//      A blob has no time axis.  The host asks for it once for the whole
//      clip, so a keyframe has literally nowhere to be stored or read.
//
// Therefore every parameter here is static per clip, and every one of them
// carries PF_ParamFlag_CANNOT_TIME_VARY so the Effect Controls panel shows no
// stopwatch.  That is honest UI: a stopwatch on a control whose keyframes can
// never reach the decoder is a control that silently does nothing.
//
// Pan / Tilt / Roll / FOV therefore stay in Open360Reframe.aex, which is an
// ordinary timeline effect: it does receive PF_Cmd_RENDER at a specific time
// and can read its parameters per frame, which is exactly what keyframed
// reframing needs.  The AE SDK also lists "multiple PiPLs in a single
// plug-in" among the features Premiere does not support, so the two effects
// must be separate modules regardless.

#include "SourceSettingsMapping.h"
#include "SourceSettingsParams.h"

#include "PluginLog.h"
#include "PrefsBlob.h"

// Adobe headers.  Everything of ours is declared before these open their
// #pragma pack(push, 1) region, and nothing of ours is declared inside it.
//
// AEConfig.h MUST come first: it turns _WIN32 into AE_OS_WIN, and
// Param_Utils.h branches on AE_OS_WIN to pick strncpy_s over the BSD strlcpy
// that does not exist on Windows.  AE_Effect.h does not include it, so every
// Adobe sample includes it by hand and so do we.
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

// kPFSourceSettingsSuite lives here, alongside the other PF_* Premiere
// extensions to the AE API.
#include "PrSDKAESupport.h"

#include <cstdio>
#include <cstring>
#include <exception>

// ===========================================================================
//  The PiPL constants really are the AE constants
//
//  SourceSettingsParams.h spells the out-flags out as decimal literals so the
//  .r file can paste them into the resource.  These assertions are what stops
//  that from being a lie: a future AE SDK that renumbers a bit breaks the
//  build here instead of shipping a PiPL Premiere rejects at load time.
// ===========================================================================
static_assert(OSV_SOURCE_SETTINGS_OUT_FLAGS == PF_OutFlag_SEND_UPDATE_PARAMS_UI,
              "OSV_SOURCE_SETTINGS_OUT_FLAGS in SourceSettingsParams.h no longer matches AE_Effect.h");
static_assert(OSV_SOURCE_SETTINGS_OUT_FLAGS_2 ==
                  (PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG | PF_OutFlag2_SUPPORTS_THREADED_RENDERING),
              "OSV_SOURCE_SETTINGS_OUT_FLAGS_2 in SourceSettingsParams.h no longer matches AE_Effect.h");
static_assert(OSV_SOURCE_SETTINGS_PIPL_VERSION ==
                  PF_VERSION(OSV_SOURCE_SETTINGS_VERSION_MAJOR, OSV_SOURCE_SETTINGS_VERSION_MINOR,
                             OSV_SOURCE_SETTINGS_VERSION_BUG, PF_Stage_RELEASE, OSV_SOURCE_SETTINGS_BUILD),
              "OSV_SOURCE_SETTINGS_PIPL_VERSION does not equal PF_VERSION() of the same numbers");
static_assert(OSV_SOURCE_SETTINGS_STAGE == PF_Stage_RELEASE, "the PiPL stage word is not PF_Stage_RELEASE");

// The index table is written out by hand because the two group terminators
// break the "index == id" shortcut.  These pin the three facts that make it
// correct, so a reordering of paramsSetup() has to come here and think.
namespace {
using namespace osv::premiere::sourcesettings;
}  // namespace
static_assert(kIndexAdvancedTopicEnd == OSV_SOURCE_SETTINGS_PARAM_COUNT,
              "the Advanced group terminator must be the last parameter added");
static_assert(kIndexStitchTopicEnd == kIndexCalibration + 1,
              "the Stitching group must close immediately after Calibration");
static_assert(kIndexDirectColour == kIndexRenderDevice + 1,
              "Program Monitor Colour follows Render Device inside the Advanced group");
static_assert(kIndexAdvancedTopicEnd == kIndexDirectColour + 1,
              "the Advanced group must close immediately after Program Monitor Colour");
static_assert(kIndexRec709Look == kIndexColorOutput + 1,
              "the Rec.709 look sits directly under Colour Output");
static_assert(kParamIdByIndex[kIndexColorOutput - 1] == OSV_SS_ID_COLOR_OUTPUT,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(kParamIdByIndex[kIndexRec709Look - 1] == OSV_SS_ID_REC709_LOOK,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(kParamIdByIndex[kIndexRenderDevice - 1] == OSV_SS_ID_RENDER_DEVICE,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(kParamIdByIndex[kIndexDirectColour - 1] == OSV_SS_ID_DIRECT_COLOUR,
              "kParamIdByIndex is not aligned with the ParamIndex enum");

namespace {

/// Premiere identifies itself to an AE effect with this application id.  A
/// source settings effect only means anything inside Premiere, so several
/// paths below are gated on it.
constexpr A_long kPremiereApplId = 'PrMr';

/// Log file base name (%LOCALAPPDATA%\OpenOSV\OpenOSVSourceSettings.log).
constexpr const wchar_t* kLogName = L"OpenOSVSourceSettings";

using osv::premiere::PluginLog;
using osv::premiere::PrefsBlob;

// ===========================================================================
//  The Source Settings Suite
// ===========================================================================

/// RAII holder for the PF Source Settings Suite.
///
/// Acquired at v2 with a fallback to v1: both versions have the same two
/// members in the same order (PrSDKAESupport.h:1620-1637 -
/// PF_SourceSettingsSuite2 is a typedef of PF_SourceSettingsSuite), so a host
/// that only offers v1 is served by identical code rather than by a second
/// implementation.  A null suite is never fatal: the effect then behaves as a
/// plain parameter panel, which still translates prefs correctly - it just
/// does not get the host's "this is a source settings effect" treatment, and
/// says so in the log exactly once.
class SourceSettingsSuite {
public:
    explicit SourceSettingsSuite(PF_InData* in_data) noexcept {
        if (!in_data || !in_data->pica_basicP) {
            return;
        }
        m_basic = in_data->pica_basicP;
        for (const int version : {kPFSourceSettingsSuiteVersion2, kPFSourceSettingsSuiteVersion1}) {
            const void* raw = nullptr;
            if (m_basic->AcquireSuite(kPFSourceSettingsSuite, version, &raw) == kSPNoError && raw) {
                m_suite = static_cast<const PF_SourceSettingsSuite*>(raw);
                m_version = version;
                return;
            }
        }
        m_basic = nullptr;  // nothing acquired, so nothing to release
    }

    ~SourceSettingsSuite() {
        if (m_basic && m_suite) {
            m_basic->ReleaseSuite(kPFSourceSettingsSuite, m_version);
        }
    }

    SourceSettingsSuite(const SourceSettingsSuite&) = delete;
    SourceSettingsSuite& operator=(const SourceSettingsSuite&) = delete;

    [[nodiscard]] const PF_SourceSettingsSuite* operator->() const noexcept { return m_suite; }
    [[nodiscard]] bool ok() const noexcept { return m_suite != nullptr; }
    [[nodiscard]] int version() const noexcept { return m_version; }

private:
    SPBasicSuite* m_basic = nullptr;
    const PF_SourceSettingsSuite* m_suite = nullptr;
    int m_version = 0;
};

// ===========================================================================
//  Reading the controls
// ===========================================================================

/// Pull every control value out of the params array the host handed us.
///
/// `params[0]` is the input layer AE inserts; the controls start at index 1,
/// and the kIndex* constants are the only correct subscripts because the two
/// group terminators shift everything after the first group (see
/// SourceSettingsParams.h).
///
/// Defensive throughout: a null array, or a null entry inside it, yields the
/// defaults rather than a wild read.  Premiere has been observed to hand an
/// AE effect a short array during project load, and a crash there takes the
/// host down before the user ever sees a panel.
[[nodiscard]] ControlValues readControls(PF_ParamDef* params[]) noexcept {
    ControlValues c;
    if (!params) {
        return c;
    }

    // One bounds- and null-checked accessor, used for every field, so no
    // individual read can be the one that forgot to check.
    auto def = [params](int index) -> const PF_ParamDef* {
        if (index < 1 || index > OSV_SOURCE_SETTINGS_PARAM_COUNT) {
            return nullptr;
        }
        return params[index];
    };

    if (const PF_ParamDef* p = def(kIndexColorOutput)) {
        c.colorOutput = static_cast<int>(p->u.pd.value);
    }
    // [WP-LOOK]
    if (const PF_ParamDef* p = def(kIndexRec709Look)) {
        c.rec709Look = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexOutputSize)) {
        c.outputSize = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexStabilization)) {
        c.stabilization = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexSeamSearch)) {
        c.seamSearch = p->u.bd.value != 0;
    }
    if (const PF_ParamDef* p = def(kIndexGainMatch)) {
        c.gainMatch = p->u.bd.value != 0;
    }
    if (const PF_ParamDef* p = def(kIndexCalibration)) {
        c.calibration = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexDlogmFit)) {
        c.dlogmFit = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexExposure)) {
        c.exposureStops = static_cast<double>(p->u.fs_d.value);
    }
    if (const PF_ParamDef* p = def(kIndexRenderDevice)) {
        c.renderDevice = static_cast<int>(p->u.pd.value);
    }
    // [WP-SETTINGS]
    if (const PF_ParamDef* p = def(kIndexDirectColour)) {
        c.directColour = static_cast<int>(p->u.pd.value);
    }
    return c;
}

/// Write a set of control values back into the host's params array, marking
/// each one PF_ChangeFlag_CHANGED_VALUE so the host records the edit.
///
/// Only the parameters whose value actually MOVED are flagged.  Flagging an
/// untouched parameter makes the host record a spurious change, which shows
/// up to the user as settings they never made appearing modified - and on a
/// master clip effect that means an unnecessary media refresh and a re-stitch
/// of the whole clip.
void writeControls(PF_ParamDef* params[], const ControlValues& wanted) noexcept {
    if (!params) {
        return;
    }
    auto setPopup = [params](int index, int value) {
        if (index < 1 || index > OSV_SOURCE_SETTINGS_PARAM_COUNT || !params[index]) {
            return;
        }
        if (params[index]->u.pd.value == static_cast<A_long>(value)) {
            return;  // unchanged: do not flag it
        }
        params[index]->u.pd.value = static_cast<A_long>(value);
        params[index]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    };
    auto setCheckbox = [params](int index, bool value) {
        if (index < 1 || index > OSV_SOURCE_SETTINGS_PARAM_COUNT || !params[index]) {
            return;
        }
        const A_long wantedValue = value ? 1 : 0;
        if (params[index]->u.bd.value == wantedValue) {
            return;
        }
        params[index]->u.bd.value = wantedValue;
        params[index]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    };
    auto setSlider = [params](int index, double value) {
        if (index < 1 || index > OSV_SOURCE_SETTINGS_PARAM_COUNT || !params[index]) {
            return;
        }
        const PF_FpShort wantedValue = static_cast<PF_FpShort>(value);
        if (params[index]->u.fs_d.value == wantedValue) {
            return;
        }
        params[index]->u.fs_d.value = wantedValue;
        params[index]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    };

    setPopup(kIndexColorOutput, wanted.colorOutput);
    setPopup(kIndexRec709Look, wanted.rec709Look);  // [WP-LOOK]
    setPopup(kIndexOutputSize, wanted.outputSize);
    setPopup(kIndexStabilization, wanted.stabilization);
    setCheckbox(kIndexSeamSearch, wanted.seamSearch);
    setCheckbox(kIndexGainMatch, wanted.gainMatch);
    setPopup(kIndexCalibration, wanted.calibration);
    setPopup(kIndexDlogmFit, wanted.dlogmFit);
    setSlider(kIndexExposure, wanted.exposureStops);
    setPopup(kIndexRenderDevice, wanted.renderDevice);
    setPopup(kIndexDirectColour, wanted.directColour);  // [WP-SETTINGS]
}

// ===========================================================================
//  Command handlers
// ===========================================================================

PF_Err about(PF_InData* in_data, PF_OutData* out_data) noexcept {
    (void)in_data;
    if (!out_data) {
        return PF_Err_NONE;
    }
    // out_data->return_msg is a fixed A_char buffer of PF_MAX_EFFECT_MSG_LEN
    // + 1 bytes; snprintf bounds the write and NUL terminates.  '\r' is the
    // line separator the About box expects.
    std::snprintf(out_data->return_msg, PF_MAX_EFFECT_MSG_LEN,
                  "%s v%d.%d.%d\r"
                  "Stitch and decode options for an OpenOSV 360 clip.\r"
                  "Applied to the master clip; the values reach the importer as prefs,\r"
                  "so they cannot be keyframed.  Reframing lives in Open 360 Reframe.\r"
                  "Part of OpenOSV, the clean-room DJI Osmo 360 toolkit.  Apache-2.0.",
                  OSV_SOURCE_SETTINGS_DISPLAY_NAME, OSV_SOURCE_SETTINGS_VERSION_MAJOR,
                  OSV_SOURCE_SETTINGS_VERSION_MINOR, OSV_SOURCE_SETTINGS_VERSION_BUG);
    return PF_Err_NONE;
}

/// PF_Cmd_GLOBAL_SETUP: identity, out-flags, and the one call that makes this
/// a source settings effect at all.
PF_Err globalSetup(PF_InData* in_data, PF_OutData* out_data) noexcept {
    if (!out_data) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // The log file is opened here rather than in DllMain: DllMain runs under
    // the loader lock and must not touch the file system.
    PluginLog::init(kLogName);

    out_data->my_version =
        PF_VERSION(OSV_SOURCE_SETTINGS_VERSION_MAJOR, OSV_SOURCE_SETTINGS_VERSION_MINOR,
                   OSV_SOURCE_SETTINGS_VERSION_BUG, PF_Stage_RELEASE, OSV_SOURCE_SETTINGS_BUILD);
    // Exactly the PiPL words; the static_asserts above tie the two together.
    // A mismatch between the PiPL and GLOBAL_SETUP is a documented way for an
    // AE effect to be rejected at load time.
    out_data->out_flags = OSV_SOURCE_SETTINGS_OUT_FLAGS;
    out_data->out_flags2 = OSV_SOURCE_SETTINGS_OUT_FLAGS_2;

    // ---- the declaration that matters --------------------------------------
    // Without this the host treats the module as an ordinary video filter:
    // it appears in the Effects panel to be dragged onto clips, it is never
    // attached to a master clip, and PF_Cmd_TRANSLATE_PARAMS_TO_PREFS never
    // arrives - so the panel would show ten controls that do nothing.
    //
    // It is only meaningful inside Premiere (After Effects has no such
    // concept and does not publish the suite), so the call is gated on the
    // application id and a missing suite is a logged degradation, never an
    // error return: a plug-in that fails GLOBAL_SETUP is dropped entirely,
    // and a panel that works minus the master-clip attachment is strictly
    // better than no panel.
    if (in_data && in_data->appl_id == kPremiereApplId) {
        const SourceSettingsSuite suite(in_data);
        if (suite.ok() && suite->SetIsSourceSettingsEffect) {
            const PF_Err err = suite->SetIsSourceSettingsEffect(in_data->effect_ref, TRUE);
            if (err != PF_Err_NONE) {
                PluginLog::warn("source settings: SetIsSourceSettingsEffect returned {}", static_cast<int>(err));
            } else {
                PluginLog::info("source settings: registered as a source settings effect (suite v{})",
                                suite.version());
            }
        } else {
            PluginLog::oncef("ss/no-suite", PluginLog::Level::Warn,
                             "source settings: no PF Source Settings Suite; the effect will not be attached to "
                             "master clips automatically");
        }
    }

    PluginLog::info("source settings: global setup (host '{}{}{}{}', flags 0x{:08X}/0x{:08X})",
                    in_data ? static_cast<char>((in_data->appl_id >> 24) & 0xFF) : '?',
                    in_data ? static_cast<char>((in_data->appl_id >> 16) & 0xFF) : '?',
                    in_data ? static_cast<char>((in_data->appl_id >> 8) & 0xFF) : '?',
                    in_data ? static_cast<char>(in_data->appl_id & 0xFF) : '?',
                    static_cast<unsigned>(out_data->out_flags), static_cast<unsigned>(out_data->out_flags2));
    return PF_Err_NONE;
}

PF_Err globalSetdown(PF_InData*, PF_OutData*) noexcept {
    // Nothing device-specific is ever set up here (this effect renders
    // nothing and owns no GPU resources), so closing the log is the whole
    // teardown.  HostContext is deliberately NOT touched: this module never
    // creates one, and tearing down a context the importer or the reframe
    // effect owns would be reaching into another module's state.
    PluginLog::info("source settings: global setdown");
    PluginLog::shutdown();
    return PF_Err_NONE;
}

/// PF_Cmd_PARAMS_SETUP: the eleven controls.
///
/// Every one of them carries PF_ParamFlag_CANNOT_TIME_VARY.  See the file
/// header for why that is a correctness requirement rather than a style
/// choice: the values travel to the importer as one flat blob with no time
/// axis, so a keyframe could never be read back.
PF_Err paramsSetup(PF_InData* in_data, PF_OutData* out_data) noexcept {
    if (!in_data || !out_data) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    PF_ParamDef def{};

    // The flag every control shares.  Named once so no control can be added
    // later without it - the compiler does not check that, but a reader does.
    constexpr A_long kStaticFlags = PF_ParamFlag_CANNOT_TIME_VARY;

    // ---- 1. Colour Output --------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Colour Output", OSV_SS_COLOR_COUNT, OSV_SS_COLOR_DEFAULT, OSV_SS_COLOR_ITEMS, kStaticFlags,
                  OSV_SS_ID_COLOR_OUTPUT);

    // ---- 2. Look (Rec. 709 only) [WP-LOOK] ---------------------------------
    // The Rec. 709 output's display look, right under the output it belongs
    // to: DJI Studio's rendering (the default) or OpenOSV's standard one.
    // The name carries "(Rec. 709 only)" because PQ, HLG and the passthrough
    // ignore it and a source settings effect cannot dependably grey it out.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Look (Rec. 709 only)", OSV_SS_LOOK_COUNT, OSV_SS_LOOK_DEFAULT, OSV_SS_LOOK_ITEMS, kStaticFlags,
                  OSV_SS_ID_REC709_LOOK);

    // ---- 3. Output Size ----------------------------------------------------
    // The size a new sequence built from the clip inherits, which is why the
    // labels spell the pixels out.  Every entry is 2:1 because a full
    // 360 x 180 sphere is; the 16:9 delivery crop is the reframe effect's job.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Output Size", OSV_SS_SIZE_COUNT, OSV_SS_SIZE_DEFAULT, OSV_SS_SIZE_ITEMS, kStaticFlags,
                  OSV_SS_ID_OUTPUT_SIZE);

    // ---- 4. Stabilisation --------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Stabilisation", OSV_SS_STAB_COUNT, OSV_SS_STAB_DEFAULT, OSV_SS_STAB_ITEMS, kStaticFlags,
                  OSV_SS_ID_STABILIZATION);

    // ---- 5. Stitching topic ------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Stitching", PF_ParamFlag_NONE, OSV_SS_ID_STITCH_TOPIC);

    // ---- 6. Seam Search ----------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Seam Search", OSV_SS_SEAM_SEARCH_DEFAULT, kStaticFlags, OSV_SS_ID_SEAM_SEARCH);

    // ---- 7. Exposure Match -------------------------------------------------
    // The blob field is called gainMatch; the label says what it does to a
    // user, which is match the two lenses' exposure across the seam.
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Exposure Match", OSV_SS_GAIN_MATCH_DEFAULT, kStaticFlags, OSV_SS_ID_GAIN_MATCH);

    // ---- 8. Calibration ----------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Calibration", OSV_SS_CALIB_COUNT, OSV_SS_CALIB_DEFAULT, OSV_SS_CALIB_ITEMS, kStaticFlags,
                  OSV_SS_ID_CALIBRATION);

    // ---- 9. Close the Stitching group --------------------------------------
    // PF_END_TOPIC issues its own PF_ADD_PARAM (Param_Utils.h:309-316), so the
    // terminator occupies a parameter slot of its own and everything after it
    // shifts up by one.  Leaving it out would not merely lose a divider: the
    // group would stay open and the whole Advanced group would nest inside
    // Stitching, so collapsing Stitching would hide controls meant to be
    // siblings.
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OSV_SS_ID_STITCH_TOPIC_END);

    // ---- 10. Advanced topic (collapsed: most users never touch it) ----------
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Advanced", PF_ParamFlag_START_COLLAPSED, OSV_SS_ID_ADVANCED_TOPIC);

    // ---- 11. D-Log M Curve -------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("D-Log M Curve", OSV_SS_FIT_COUNT, OSV_SS_FIT_DEFAULT, OSV_SS_FIT_ITEMS, kStaticFlags,
                  OSV_SS_ID_DLOGM_FIT);

    // ---- 12. Exposure ------------------------------------------------------
    // Valid range is the blob's own +/- 6 stops (static_asserted below the
    // handlers); the slider shows the useful +/- 3 so a drag has resolution.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Exposure", OSV_SS_EXPOSURE_VALID_MIN, OSV_SS_EXPOSURE_VALID_MAX,
                         OSV_SS_EXPOSURE_SLIDER_MIN, OSV_SS_EXPOSURE_SLIDER_MAX, OSV_SS_EXPOSURE_DEFAULT,
                         PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_EXPOSURE);

    // ---- 13. Render Device -------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Render Device", OSV_SS_DEVICE_COUNT, OSV_SS_DEVICE_DEFAULT, OSV_SS_DEVICE_ITEMS, kStaticFlags,
                  OSV_SS_ID_RENDER_DEVICE);

    // ---- 14. Program Monitor Colour [WP-SETTINGS] --------------------------
    // What Open 360 Reframe shows when this clip's Colour Output is not the
    // sequence's working space: the scene rendered straight into it (fast,
    // the default) or Premiere's own conversion of the output (matches the
    // Source monitor) - see OSV_SS_DIRECT_COLOUR_ITEMS.  Static like every
    // control here: it reaches the effect through the same flat blob.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Program Monitor Colour", OSV_SS_DIRECT_COLOUR_COUNT, OSV_SS_DIRECT_COLOUR_DEFAULT,
                  OSV_SS_DIRECT_COLOUR_ITEMS, kStaticFlags, OSV_SS_ID_DIRECT_COLOUR);

    // ---- 15. Close the Advanced group --------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OSV_SS_ID_ADVANCED_TOPIC_END);

    out_data->num_params = OSV_SOURCE_SETTINGS_PARAM_COUNT + 1;  // + the input layer
    return PF_Err_NONE;
}

/// PF_Cmd_SEQUENCE_SETUP: ask the importer what the media is currently being
/// decoded with, and seed the controls from it.
///
/// PerformSourceSettingsCommand (PrSDKAESupport.h:1627-1630) hands a buffer
/// to the matching importer's imPerformSourceSettingsCommand (selector 66),
/// which fills it in.  The exchange is entirely private between the two
/// halves of this plug-in - the SDK is explicit that "the data can be anything
/// as long as both the importer and the source settings effect both know what
/// it is" (PrSDKImport.h:996) - so the payload is simply a PrefsBlob.
///
/// Why this matters: without it a user who set 4K output on a clip, saved,
/// and reopened the project would see every control back at its global
/// default even though the clip is still being decoded at 4K.  The importer
/// is the only party that knows the clip's real, current prefs.
///
/// Every failure is a silent, logged degradation ending in PF_Err_NONE: the
/// controls then keep whatever the project stored for them, which is the
/// next-best truth and is never wrong enough to justify refusing the
/// sequence.  sequence_data stays null throughout - this effect keeps no
/// per-instance state, so there is nothing to allocate, flatten or free.
PF_Err sequenceSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) noexcept {
    if (out_data) {
        // Do this FIRST and unconditionally.  A non-null sequence_data the
        // host tries to flatten later would be a pointer we never allocated.
        out_data->sequence_data = nullptr;
    }
    if (!in_data || in_data->appl_id != kPremiereApplId) {
        return PF_Err_NONE;  // After Effects: no importer to ask
    }

    const SourceSettingsSuite suite(in_data);
    if (!suite.ok() || !suite->PerformSourceSettingsCommand) {
        PluginLog::oncef("ss/no-perform", PluginLog::Level::Debug,
                         "source settings: PerformSourceSettingsCommand is unavailable; the controls keep their "
                         "stored values");
        return PF_Err_NONE;
    }

    // Seed the buffer with what the controls currently say, so an importer
    // that chooses to ACCEPT rather than override (a clip whose prefs are not
    // yet established) receives a valid, sanitised blob instead of zeros -
    // and so a host that calls through without reaching the importer at all
    // leaves the controls exactly as they were.
    PrefsBlob blob = prefsFromControls(readControls(params));

    const PF_Err err = suite->PerformSourceSettingsCommand(in_data->effect_ref, &blob,
                                                           static_cast<csSDK_uint32>(PrefsBlob::kSize));
    if (err != PF_Err_NONE) {
        PluginLog::debug("source settings: PerformSourceSettingsCommand returned {}", static_cast<int>(err));
        return PF_Err_NONE;
    }

    // The importer may have written anything into the buffer, including
    // nothing at all, so the result is validated exactly like a blob off
    // disk before it is allowed near the UI.
    if (!blob.isValid()) {
        PluginLog::oncef("ss/perform-invalid", PluginLog::Level::Debug,
                         "source settings: the importer returned a blob that is not ours; keeping the stored "
                         "control values");
        return PF_Err_NONE;
    }
    blob.sanitise();

    // writeControls only flags what actually moved, so a clip whose prefs
    // already match the controls records no change and triggers no refresh.
    writeControls(params, controlsFromPrefs(blob));
    PluginLog::debug("source settings: seeded from the media - colour {}, size {}, stab {}, seam {}, gain {}, "
                     "calib {}, fit {}, exposure {:+.2f}, device {}",
                     blob.colorOutput, blob.outputSize, blob.stabilization, blob.seamSearch, blob.gainMatch,
                     blob.calibration, blob.dlogmFit, static_cast<double>(blob.exposureStops), blob.renderDevice);
    return PF_Err_NONE;
}

/// PF_Cmd_TRANSLATE_PARAMS_TO_PREFS: the controls become the importer's prefs.
///
/// This is the only route by which anything set in the Effect Controls panel
/// reaches the decoder.  `extra` is a PF_TranslateParamsToPrefsExtra whose
/// `prefsPC` points at a buffer the HOST owns and this effect fills
/// (AE_Effect.h:2177-2190).
///
/// Three defensive rules, each of which is a real failure mode:
///
///   * a null extra or a null prefsPC means the host asked without providing
///     a buffer.  Returning PF_Err_NONE leaves the importer on its existing
///     prefs, which is correct; writing through the null would take the host
///     down.
///   * prefs_sizeLu SMALLER than a PrefsBlob means the host's idea of the
///     prefs size disagrees with ours - which happens on a project saved by
///     an older build.  Writing 128 bytes into a smaller buffer is a heap
///     overflow in the host's allocator, so the write is refused and logged.
///     The importer's own two-step imGetPrefs8 protocol then re-establishes
///     the correct size.
///   * a LARGER buffer is fine and is not an error: only the first
///     sizeof(PrefsBlob) bytes are ours, and PrefsBlob::fromBytes on the
///     importer side reads exactly that many.  The tail is left untouched
///     rather than zeroed, because it is not our memory to define.
PF_Err translateParamsToPrefs(PF_InData* in_data, PF_ParamDef* params[],
                              PF_TranslateParamsToPrefsExtra* extra) noexcept {
    (void)in_data;
    if (!extra || !extra->prefsPC) {
        PluginLog::oncef("ss/translate-null", PluginLog::Level::Warn,
                         "source settings: TRANSLATE_PARAMS_TO_PREFS with no prefs buffer");
        return PF_Err_NONE;
    }
    if (extra->prefs_sizeLu < static_cast<A_u_long>(PrefsBlob::kSize)) {
        PluginLog::oncef("ss/translate-small", PluginLog::Level::Error,
                         "source settings: the host's prefs buffer is {} bytes, {} are needed; not writing",
                         static_cast<unsigned>(extra->prefs_sizeLu), static_cast<unsigned>(PrefsBlob::kSize));
        return PF_Err_NONE;
    }

    const PrefsBlob blob = prefsFromControls(readControls(params));
    // prefsPC is an opaque PF_ImporterPrefsDataPtr; the SDK's contract is
    // that the importer and the effect agree on the bytes behind it, so a
    // memcpy of exactly our struct size is the whole write.
    std::memcpy(extra->prefsPC, &blob, PrefsBlob::kSize);

    PluginLog::debug("source settings: translated - colour {}, look {}, size {}, stab {}, seam {}, gain {}, "
                     "calib {}, fit {}, exposure {:+.2f}, device {}",
                     blob.colorOutput, blob.look, blob.outputSize, blob.stabilization, blob.seamSearch,
                     blob.gainMatch, blob.calibration, blob.dlogmFit, static_cast<double>(blob.exposureStops),
                     blob.renderDevice);
    return PF_Err_NONE;
}

/// PF_Cmd_UPDATE_PARAMS_UI: nothing is greyed today.  The handler exists (and
/// PF_OutFlag_SEND_UPDATE_PARAMS_UI is set) because it is the only hook for
/// enabling or disabling a control, and adding it later would change the PiPL
/// flags - which invalidates the plug-in cache on every installed machine.
PF_Err updateParamsUi(PF_InData*, PF_OutData*, PF_ParamDef*[]) noexcept { return PF_Err_NONE; }

}  // namespace

// ===========================================================================
//  The popup item lists really cover the PrefsBlob enums
//
//  Every popup's item COUNT is tied to the matching enum's Count, and the
//  Exposure slider's valid range to the blob's own clamp limits.  An enum
//  that gains a value therefore breaks the build here instead of shipping a
//  popup that cannot express the new setting - and a slider can never offer a
//  value sanitise() would silently clamp behind the user's back.
//
//  The 1-based popup DEFAULTS cannot be checked this way, because
//  PrefsBlob::defaults() is a runtime function (it memsets, then assigns).
//  tests/premiere/sourcesettings/test_params.cpp compares each one against
//  defaults() instead, which is where that fact is asserted.
// ===========================================================================
static_assert(OSV_SS_EXPOSURE_VALID_MIN == static_cast<double>(osv::premiere::PrefsBlob::kMinExposureStops),
              "the Exposure slider's minimum does not match PrefsBlob::kMinExposureStops");
static_assert(OSV_SS_EXPOSURE_VALID_MAX == static_cast<double>(osv::premiere::PrefsBlob::kMaxExposureStops),
              "the Exposure slider's maximum does not match PrefsBlob::kMaxExposureStops");
static_assert(OSV_SS_COLOR_COUNT == static_cast<int>(osv::premiere::PrefsColorOutput::Count),
              "the Colour Output popup does not list every PrefsColorOutput value");
static_assert(OSV_SS_SIZE_COUNT == static_cast<int>(osv::premiere::PrefsOutputSize::Count),
              "the Output Size popup does not list every PrefsOutputSize value");
static_assert(OSV_SS_STAB_COUNT == static_cast<int>(osv::premiere::PrefsStabilization::Count),
              "the Stabilisation popup does not list every PrefsStabilization value");
static_assert(OSV_SS_CALIB_COUNT == static_cast<int>(osv::premiere::PrefsCalibration::Count),
              "the Calibration popup does not list every PrefsCalibration value");
static_assert(OSV_SS_FIT_COUNT == static_cast<int>(osv::premiere::PrefsDlogmFit::Count),
              "the D-Log M Curve popup does not list every PrefsDlogmFit value");
static_assert(OSV_SS_DEVICE_COUNT == static_cast<int>(osv::premiere::PrefsRenderDevice::Count),
              "the Render Device popup does not list every PrefsRenderDevice value");
static_assert(OSV_SS_DIRECT_COLOUR_COUNT == static_cast<int>(osv::premiere::PrefsDirectColour::Count),
              "the Program Monitor Colour popup does not list every PrefsDirectColour value");
static_assert(OSV_SS_LOOK_COUNT == static_cast<int>(osv::premiere::PrefsLook::Count),
              "the Rec.709 look popup does not list every PrefsLook value");

// ===========================================================================
//  The exported entry point
//
//  Named exactly as the PiPL's CodeWin64X86 property says.  Everything is
//  wrapped in a try/catch: an exception unwinding into Premiere's C stack is
//  undefined behaviour and in practice takes the host down.
// ===========================================================================
extern "C" __declspec(dllexport) PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
                                                   PF_ParamDef* params[], PF_LayerDef* output, void* extra) {
    (void)output;  // never used: this effect is never sent PF_Cmd_RENDER
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
            case PF_Cmd_UPDATE_PARAMS_UI:
                return updateParamsUi(in_data, out_data, params);

            // The two selectors that make this a source settings effect.
            case PF_Cmd_SEQUENCE_SETUP:
                return sequenceSetup(in_data, out_data, params);
            case PF_Cmd_TRANSLATE_PARAMS_TO_PREFS:
                return translateParamsToPrefs(in_data, params,
                                              static_cast<PF_TranslateParamsToPrefsExtra*>(extra));

            // No per-instance state, so the remaining sequence commands only
            // have to leave the handle null.  SEQUENCE_SETUP is handled above
            // because it is the one that talks to the importer.
            case PF_Cmd_SEQUENCE_RESETUP:
            case PF_Cmd_SEQUENCE_FLATTEN:
            case PF_Cmd_SEQUENCE_SETDOWN:
                if (out_data) {
                    out_data->sequence_data = nullptr;
                }
                return PF_Err_NONE;

            default:
                // Every other selector - PF_Cmd_RENDER included, which this
                // effect is never sent and would have nothing to do with -
                // is genuinely not handled.  PF_Err_NONE is the documented
                // "ignored" answer; returning an error would make the host
                // report a broken effect.
                return PF_Err_NONE;
        }
    } catch (const std::exception& e) {
        PluginLog::error("source settings: exception in selector {}: {}", static_cast<int>(cmd), e.what());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    } catch (...) {
        PluginLog::error("source settings: unknown exception in selector {}", static_cast<int>(cmd));
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}
