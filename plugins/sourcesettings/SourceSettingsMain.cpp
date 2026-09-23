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
// Rec.709 look, [WP-HDRPEAK] the PQ output's HDR peak, sun ghost removal, the
// sky seam fix, the carved seam's tweaks
// and the reframe effect's Program Monitor Colour) are simply VISIBLE, instead
// of hiding behind the modal dialog in imGetPrefs8.
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
//   PF_Cmd_PARAMS_SETUP              the twenty-three controls, each flagged
//                                    PF_ParamFlag_CANNOT_TIME_VARY.
//   PF_Cmd_SEQUENCE_SETUP            PerformSourceSettingsCommand(), which
//                                    round-trips a blob through the importer
//                                    so the controls can be seeded from the
//                                    media's own "as shot" settings.
//   PF_Cmd_TRANSLATE_PARAMS_TO_PREFS the controls -> a 128-byte PrefsBlob
//                                    written into the host's prefs buffer.
//   PF_Cmd_USER_CHANGED_PARAM        [WP-DEFAULTS] the Defaults group's two
//                                    buttons: save this clip's settings as
//                                    the defaults for new clips, or remove
//                                    them (plugins/common/UserDefaults.h).
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
#include "UserDefaults.h"

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
#include <string>
#include <string_view>

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
// [WP-DEFAULTS] The Defaults group follows the Advanced group and closes the
// list: Save, then Restore, then its terminator as the last parameter added.
static_assert(kIndexDefaultsTopic == kIndexAdvancedTopicEnd + 1,
              "the Defaults group opens immediately after the Advanced group closes");
static_assert(kIndexDefaultsTopicEnd == OSV_SOURCE_SETTINGS_PARAM_COUNT,
              "the Defaults group terminator must be the last parameter added");
static_assert(kParamIdByIndex[kIndexSaveDefaults - 1] == OSV_SS_ID_SAVE_DEFAULTS &&
                  kParamIdByIndex[kIndexRestoreDefaults - 1] == OSV_SS_ID_RESTORE_DEFAULTS &&
                  kParamIdByIndex[kIndexDefaultsTopicEnd - 1] == OSV_SS_ID_DEFAULTS_TOPIC_END,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(kIndexFlareRemoval == kIndexCalibration + 1,
              "Sun Ghost Removal follows Calibration inside the Stitching group");  // [WP-FLARE]
// [WP-PHOTO] the sky seam fix's three controls follow Sun Ghost Removal.
static_assert(kIndexPhotoSeam == kIndexFlareRemoval + 1 && kIndexPhotoStrength == kIndexPhotoSeam + 1 &&
                  kIndexSeamInset == kIndexPhotoStrength + 1,
              "Sky Seam Fix, Sky Seam Strength and Seam Edge Inset follow Sun Ghost Removal in that order");
// [WP-SEAMTOOLS] the seam tools' five controls follow Seam Edge Inset, and
// the Stitching group closes right after them.
static_assert(kIndexSeamBlend == kIndexSeamInset + 1 && kIndexParallaxBlend == kIndexSeamBlend + 1 &&
                  kIndexSeamSmoothing == kIndexParallaxBlend + 1 && kIndexNearOffset == kIndexSeamSmoothing + 1 &&
                  kIndexFarOffset == kIndexNearOffset + 1,
              "Seam Blend, Parallax Blend, Seam Smoothing, Near Offset and Far Offset follow Seam Edge Inset");
static_assert(kParamIdByIndex[kIndexSeamBlend - 1] == OSV_SS_ID_SEAM_BLEND &&
                  kParamIdByIndex[kIndexParallaxBlend - 1] == OSV_SS_ID_PARALLAX_BLEND &&
                  kParamIdByIndex[kIndexSeamSmoothing - 1] == OSV_SS_ID_SEAM_SMOOTHING &&
                  kParamIdByIndex[kIndexNearOffset - 1] == OSV_SS_ID_NEAR_OFFSET &&
                  kParamIdByIndex[kIndexFarOffset - 1] == OSV_SS_ID_FAR_OFFSET,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
// [WP-VIGNETTE] the lens shading correction's two controls follow Far
// Offset, and the Stitching group closes right after them.
static_assert(kIndexLensShading == kIndexFarOffset + 1 && kIndexShadingStrength == kIndexLensShading + 1,
              "Lens Shading and Shading Strength follow Far Offset in that order");
static_assert(kParamIdByIndex[kIndexLensShading - 1] == OSV_SS_ID_LENS_SHADING &&
                  kParamIdByIndex[kIndexShadingStrength - 1] == OSV_SS_ID_SHADING_STRENGTH,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
// [WP-STEADY] Parallax Grid and Lens Alignment follow Shading Strength, and
// the Stitching group closes right after them.
static_assert(kIndexParallaxGrid == kIndexShadingStrength + 1 && kIndexLensAlign == kIndexParallaxGrid + 1,
              "Parallax Grid and Lens Alignment follow Shading Strength in that order");
static_assert(kParamIdByIndex[kIndexParallaxGrid - 1] == OSV_SS_ID_PARALLAX_GRID &&
                  kParamIdByIndex[kIndexLensAlign - 1] == OSV_SS_ID_LENS_ALIGN,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(OSV_SS_ID_PARALLAX_GRID >= 40 && OSV_SS_ID_LENS_ALIGN <= 45,
              "WP-STEADY's parameter ids live in 40-45");
static_assert(kIndexStitchTopicEnd == kIndexLensAlign + 1,
              "the Stitching group must close immediately after Lens Alignment");
static_assert(kParamIdByIndex[kIndexPhotoSeam - 1] == OSV_SS_ID_PHOTO_SEAM &&
                  kParamIdByIndex[kIndexPhotoStrength - 1] == OSV_SS_ID_PHOTO_STRENGTH &&
                  kParamIdByIndex[kIndexSeamInset - 1] == OSV_SS_ID_SEAM_INSET,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
static_assert(kParamIdByIndex[kIndexFlareRemoval - 1] == OSV_SS_ID_FLARE_REMOVAL,
              "kParamIdByIndex is not aligned with the ParamIndex enum");
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
// [WP-HDRPEAK] the HDR peak sits directly under the Rec.709 look, at the top
// level, and the Output Size row follows it.
static_assert(kIndexHdrPeak == kIndexRec709Look + 1 && kIndexOutputSize == kIndexHdrPeak + 1,
              "HDR Peak sits between Look and Output Size");
static_assert(kParamIdByIndex[kIndexHdrPeak - 1] == OSV_SS_ID_HDR_PEAK,
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
using osv::premiere::UserDefaults;
using osv::premiere::UserDefaultsLogLevel;

/// [WP-DEFAULTS] The UserDefaults log sink of this module: every message of
/// plugins/common/UserDefaults.cpp lands in OpenOSVSourceSettings.log.
void logUserDefaultsMessage(UserDefaultsLogLevel level, std::string_view message) noexcept {
    PluginLog::Level mapped = PluginLog::Level::Debug;
    switch (level) {
    case UserDefaultsLogLevel::Info:  mapped = PluginLog::Level::Info; break;
    case UserDefaultsLogLevel::Warn:  mapped = PluginLog::Level::Warn; break;
    case UserDefaultsLogLevel::Error: mapped = PluginLog::Level::Error; break;
    case UserDefaultsLogLevel::Debug:
    default:                          mapped = PluginLog::Level::Debug; break;
    }
    if (PluginLog::enabled(mapped)) {
        PluginLog::write(mapped, message);
    }
}

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
    // [WP-HDRPEAK]
    if (const PF_ParamDef* p = def(kIndexHdrPeak)) {
        c.hdrPeak = static_cast<int>(p->u.pd.value);
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
    // [WP-FLARE]
    if (const PF_ParamDef* p = def(kIndexFlareRemoval)) {
        c.flareRemoval = p->u.bd.value != 0;
    }
    // [WP-PHOTO]
    if (const PF_ParamDef* p = def(kIndexPhotoSeam)) {
        c.photoSeam = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexPhotoStrength)) {
        c.photoStrengthPercent = static_cast<double>(p->u.fs_d.value);
    }
    if (const PF_ParamDef* p = def(kIndexSeamInset)) {
        c.seamInsetDeg = static_cast<double>(p->u.fs_d.value);
    }
    // [WP-SEAMTOOLS]
    if (const PF_ParamDef* p = def(kIndexSeamBlend)) {
        c.seamBlendDeg = static_cast<double>(p->u.fs_d.value);
    }
    if (const PF_ParamDef* p = def(kIndexParallaxBlend)) {
        c.parallaxBlendDeg = static_cast<double>(p->u.fs_d.value);
    }
    if (const PF_ParamDef* p = def(kIndexSeamSmoothing)) {
        c.seamSmoothingDeg = static_cast<double>(p->u.fs_d.value);
    }
    if (const PF_ParamDef* p = def(kIndexNearOffset)) {
        c.nearOffsetDeg = static_cast<double>(p->u.fs_d.value);
    }
    if (const PF_ParamDef* p = def(kIndexFarOffset)) {
        c.farOffsetDeg = static_cast<double>(p->u.fs_d.value);
    }
    // [WP-VIGNETTE]
    if (const PF_ParamDef* p = def(kIndexLensShading)) {
        c.lensShading = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexShadingStrength)) {
        c.shadingStrengthPercent = static_cast<double>(p->u.fs_d.value);
    }
    // [WP-STEADY]
    if (const PF_ParamDef* p = def(kIndexParallaxGrid)) {
        c.parallaxGrid = static_cast<int>(p->u.pd.value);
    }
    if (const PF_ParamDef* p = def(kIndexLensAlign)) {
        c.lensAlign = static_cast<int>(p->u.pd.value);
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
    setPopup(kIndexHdrPeak, wanted.hdrPeak);        // [WP-HDRPEAK]
    setPopup(kIndexOutputSize, wanted.outputSize);
    setPopup(kIndexStabilization, wanted.stabilization);
    setCheckbox(kIndexSeamSearch, wanted.seamSearch);
    setCheckbox(kIndexGainMatch, wanted.gainMatch);
    setPopup(kIndexCalibration, wanted.calibration);
    setCheckbox(kIndexFlareRemoval, wanted.flareRemoval);  // [WP-FLARE]
    setPopup(kIndexPhotoSeam, wanted.photoSeam);              // [WP-PHOTO]
    setSlider(kIndexPhotoStrength, wanted.photoStrengthPercent);
    setSlider(kIndexSeamInset, wanted.seamInsetDeg);
    setSlider(kIndexSeamBlend, wanted.seamBlendDeg);          // [WP-SEAMTOOLS]
    setSlider(kIndexParallaxBlend, wanted.parallaxBlendDeg);
    setSlider(kIndexSeamSmoothing, wanted.seamSmoothingDeg);
    setSlider(kIndexNearOffset, wanted.nearOffsetDeg);
    setSlider(kIndexFarOffset, wanted.farOffsetDeg);
    setPopup(kIndexLensShading, wanted.lensShading);  // [WP-VIGNETTE]
    setSlider(kIndexShadingStrength, wanted.shadingStrengthPercent);
    setPopup(kIndexParallaxGrid, wanted.parallaxGrid);  // [WP-STEADY]
    setPopup(kIndexLensAlign, wanted.lensAlign);        // [WP-STEADY]
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
                  "Part of OpenOSV, the independent open-source DJI Osmo 360 toolkit.  Apache-2.0.",
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
    // [WP-DEFAULTS] The defaults file's own messages go to this module's log.
    osv::premiere::setUserDefaultsLogSink(&logUserDefaultsMessage);

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

/// PF_Cmd_PARAMS_SETUP: the twenty-three controls, and [WP-DEFAULTS] the Defaults
/// group's two buttons at the end (buttons hold no value, so they have no
/// time axis to refuse and carry only PF_ParamFlag_SUPERVISE).
///
/// Every value control carries PF_ParamFlag_CANNOT_TIME_VARY.  See the file
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

    // ---- 3. HDR Peak (PQ only) [WP-HDRPEAK] --------------------------------
    // The display peak the PQ output's highlights roll off into (BT.2408
    // Annex 5 EETF): 1000 nits (the default) leaves the master untouched,
    // 600 / 400 compress only what is above their knees, 203 keeps the whole
    // picture at or below HDR reference white.  "(PQ only)" because HLG,
    // Rec. 709 and the passthrough ignore it and this effect cannot
    // dependably grey it out.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("HDR Peak (PQ only)", OSV_SS_HDR_PEAK_COUNT, OSV_SS_HDR_PEAK_DEFAULT, OSV_SS_HDR_PEAK_ITEMS,
                  kStaticFlags, OSV_SS_ID_HDR_PEAK);

    // ---- 4. Output Size ----------------------------------------------------
    // The size a new sequence built from the clip inherits, which is why the
    // labels spell the pixels out.  Every entry is 2:1 because a full
    // 360 x 180 sphere is; the 16:9 delivery crop is the reframe effect's job.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Output Size", OSV_SS_SIZE_COUNT, OSV_SS_SIZE_DEFAULT, OSV_SS_SIZE_ITEMS, kStaticFlags,
                  OSV_SS_ID_OUTPUT_SIZE);

    // ---- 5. Stabilisation --------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Stabilisation", OSV_SS_STAB_COUNT, OSV_SS_STAB_DEFAULT, OSV_SS_STAB_ITEMS, kStaticFlags,
                  OSV_SS_ID_STABILIZATION);

    // ---- 6. Stitching topic ------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Stitching", PF_ParamFlag_NONE, OSV_SS_ID_STITCH_TOPIC);

    // ---- 7. Seam Search ----------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Seam Search", OSV_SS_SEAM_SEARCH_DEFAULT, kStaticFlags, OSV_SS_ID_SEAM_SEARCH);

    // ---- 8. Exposure Match -------------------------------------------------
    // The blob field is called gainMatch; the label says what it does to a
    // user, which is match the two lenses' exposure across the seam.
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Exposure Match", OSV_SS_GAIN_MATCH_DEFAULT, kStaticFlags, OSV_SS_ID_GAIN_MATCH);

    // ---- 9. Calibration ----------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Calibration", OSV_SS_CALIB_COUNT, OSV_SS_CALIB_DEFAULT, OSV_SS_CALIB_ITEMS, kStaticFlags,
                  OSV_SS_ID_CALIBRATION);

    // ---- 10. Sun Ghost Removal [WP-FLARE] -------------------------------------
    // Subtracts the fitted reflections of a sun that is in frame
    // (docs/research/FLARE.md).  On by default, as PrefsBlob::defaults().
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Sun Ghost Removal", OSV_SS_FLARE_REMOVAL_DEFAULT, kStaticFlags, OSV_SS_ID_FLARE_REMOVAL);

    // ---- 11. Sky Seam Fix [WP-PHOTO] -----------------------------------------
    // The photometric seam field (docs/research/NEURAL_STITCHING.md, section
    // 8): each lens's blend weight ends at its measured usable rim, and in
    // "Rim and colour" a 2-D gain field evens the two lenses' brightness and
    // colour across the overlap.  Default Rim and colour, as
    // PrefsBlob::defaults().
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Sky Seam Fix", OSV_SS_PHOTO_SEAM_COUNT, OSV_SS_PHOTO_SEAM_DEFAULT, OSV_SS_PHOTO_SEAM_ITEMS,
                  kStaticFlags, OSV_SS_ID_PHOTO_SEAM);

    // ---- 12. Sky Seam Strength [WP-PHOTO] ------------------------------------
    // How much of the colour field applies, in whole percent (the blob's
    // step), shown with the host's percent sign.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Sky Seam Strength", OSV_SS_PHOTO_STRENGTH_MIN, OSV_SS_PHOTO_STRENGTH_MAX,
                         OSV_SS_PHOTO_STRENGTH_MIN, OSV_SS_PHOTO_STRENGTH_MAX, OSV_SS_PHOTO_STRENGTH_DEFAULT,
                         PF_Precision_INTEGER, PF_ValueDisplayFlag_PERCENT, kStaticFlags, OSV_SS_ID_PHOTO_STRENGTH);

    // ---- 13. Seam Edge Inset [WP-PHOTO] --------------------------------------
    // Degrees inside the calibrated field of view where the render blend
    // ends when the sky seam fix is off or refused (the fix's per-longitude
    // rim replaces it otherwise); tenths, the blob's step.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Seam Edge Inset", OSV_SS_SEAM_INSET_MIN, OSV_SS_SEAM_INSET_MAX, OSV_SS_SEAM_INSET_MIN,
                         OSV_SS_SEAM_INSET_MAX, OSV_SS_SEAM_INSET_DEFAULT, PF_Precision_TENTHS,
                         PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_SEAM_INSET);

    // ---- 14-18. The carved seam's tweaks [WP-SEAMTOOLS] --------------------
    // Degrees, hundredths shown (the blob keeps twentieths for the widths,
    // hundredths for the offsets).  Every default is the seam as it renders
    // without them, and each changes only the overlap band.
    //   14 Seam Blend      feather where the lenses agree;
    //   15 Parallax Blend  feather where they disagree (0 = a hard cut);
    //   16 Seam Smoothing  colour and shading blend this wide, detail still
    //                      switches at the seam (0 = off);
    //   17 Near Offset     nudge near content along the seam;
    //   18 Far Offset      nudge far content along the seam.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Seam Blend", OSV_SS_SEAM_BLEND_MIN, OSV_SS_SEAM_BLEND_MAX, OSV_SS_SEAM_BLEND_MIN,
                         OSV_SS_SEAM_BLEND_MAX, OSV_SS_SEAM_BLEND_DEFAULT, PF_Precision_HUNDREDTHS,
                         PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_SEAM_BLEND);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Parallax Blend", OSV_SS_PARALLAX_BLEND_MIN, OSV_SS_PARALLAX_BLEND_MAX,
                         OSV_SS_PARALLAX_BLEND_MIN, OSV_SS_PARALLAX_BLEND_MAX, OSV_SS_PARALLAX_BLEND_DEFAULT,
                         PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_PARALLAX_BLEND);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Seam Smoothing", OSV_SS_SEAM_SMOOTHING_MIN, OSV_SS_SEAM_SMOOTHING_MAX,
                         OSV_SS_SEAM_SMOOTHING_MIN, OSV_SS_SEAM_SMOOTHING_MAX, OSV_SS_SEAM_SMOOTHING_DEFAULT,
                         PF_Precision_HUNDREDTHS, PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_SEAM_SMOOTHING);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Near Offset", OSV_SS_SEAM_OFFSET_MIN, OSV_SS_SEAM_OFFSET_MAX, OSV_SS_SEAM_OFFSET_MIN,
                         OSV_SS_SEAM_OFFSET_MAX, OSV_SS_SEAM_OFFSET_DEFAULT, PF_Precision_HUNDREDTHS,
                         PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_NEAR_OFFSET);
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Far Offset", OSV_SS_SEAM_OFFSET_MIN, OSV_SS_SEAM_OFFSET_MAX, OSV_SS_SEAM_OFFSET_MIN,
                         OSV_SS_SEAM_OFFSET_MAX, OSV_SS_SEAM_OFFSET_DEFAULT, PF_Precision_HUNDREDTHS,
                         PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_FAR_OFFSET);

    // ---- 19. Lens Shading [WP-VIGNETTE] --------------------------------------
    // Each lens's own brightness structure near its rim, measured from its
    // own sky and added back before the blend (docs/research/
    // NEURAL_STITCHING.md, section 9).  Default Auto, as PrefsBlob::defaults().
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Lens Shading", OSV_SS_LENS_SHADING_COUNT, OSV_SS_LENS_SHADING_DEFAULT, OSV_SS_LENS_SHADING_ITEMS,
                  kStaticFlags, OSV_SS_ID_LENS_SHADING);

    // ---- 20. Shading Strength [WP-VIGNETTE] ----------------------------------
    // How much of the measured correction applies, in whole percent (the
    // blob's step), shown with the host's percent sign.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Shading Strength", OSV_SS_SHADING_STRENGTH_MIN, OSV_SS_SHADING_STRENGTH_MAX,
                         OSV_SS_SHADING_STRENGTH_MIN, OSV_SS_SHADING_STRENGTH_MAX, OSV_SS_SHADING_STRENGTH_DEFAULT,
                         PF_Precision_INTEGER, PF_ValueDisplayFlag_PERCENT, kStaticFlags, OSV_SS_ID_SHADING_STRENGTH);

    // ---- 21. Parallax Grid [WP-STEADY] -----------------------------------------
    // Whether the seam corrections are held still for the whole clip (a
    // rigid mount: nothing at the seam moves) or measured per moment
    // (handheld, near objects moving past); Auto decides from the clip
    // itself.  Default Auto, as PrefsBlob::defaults().
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Parallax Grid", OSV_SS_PARALLAX_GRID_COUNT, OSV_SS_PARALLAX_GRID_DEFAULT,
                  OSV_SS_PARALLAX_GRID_ITEMS, kStaticFlags, OSV_SS_ID_PARALLAX_GRID);

    // ---- 22. Lens Alignment [WP-STEADY] ----------------------------------------
    // Fit the small rotation between the two lenses once per clip and fold it
    // into the stitch, or trust the recorded calibration.  Default Auto.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Lens Alignment", OSV_SS_LENS_ALIGN_COUNT, OSV_SS_LENS_ALIGN_DEFAULT, OSV_SS_LENS_ALIGN_ITEMS,
                  kStaticFlags, OSV_SS_ID_LENS_ALIGN);

    // ---- 23. Close the Stitching group -------------------------------------
    // PF_END_TOPIC issues its own PF_ADD_PARAM (Param_Utils.h:309-316), so the
    // terminator occupies a parameter slot of its own and everything after it
    // shifts up by one.  Leaving it out would not merely lose a divider: the
    // group would stay open and the whole Advanced group would nest inside
    // Stitching, so collapsing Stitching would hide controls meant to be
    // siblings.
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OSV_SS_ID_STITCH_TOPIC_END);

    // ---- 24. Advanced topic (collapsed: most users never touch it) ----------
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX("Advanced", PF_ParamFlag_START_COLLAPSED, OSV_SS_ID_ADVANCED_TOPIC);

    // ---- 25. D-Log M Curve -------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("D-Log M Curve", OSV_SS_FIT_COUNT, OSV_SS_FIT_DEFAULT, OSV_SS_FIT_ITEMS, kStaticFlags,
                  OSV_SS_ID_DLOGM_FIT);

    // ---- 26. Exposure ------------------------------------------------------
    // Valid range is the blob's own +/- 6 stops (static_asserted below the
    // handlers); the slider shows the useful +/- 3 so a drag has resolution.
    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Exposure", OSV_SS_EXPOSURE_VALID_MIN, OSV_SS_EXPOSURE_VALID_MAX,
                         OSV_SS_EXPOSURE_SLIDER_MIN, OSV_SS_EXPOSURE_SLIDER_MAX, OSV_SS_EXPOSURE_DEFAULT,
                         PF_Precision_TENTHS, PF_ValueDisplayFlag_NONE, kStaticFlags, OSV_SS_ID_EXPOSURE);

    // ---- 27. Render Device -------------------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Render Device", OSV_SS_DEVICE_COUNT, OSV_SS_DEVICE_DEFAULT, OSV_SS_DEVICE_ITEMS, kStaticFlags,
                  OSV_SS_ID_RENDER_DEVICE);

    // ---- 28. Program Monitor Colour [WP-SETTINGS] --------------------------
    // What Open 360 Reframe shows when this clip's Colour Output is not the
    // sequence's working space: the scene rendered straight into it (fast,
    // the default) or Premiere's own conversion of the output (matches the
    // Source monitor) - see OSV_SS_DIRECT_COLOUR_ITEMS.  Static like every
    // control here: it reaches the effect through the same flat blob.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUPX("Program Monitor Colour", OSV_SS_DIRECT_COLOUR_COUNT, OSV_SS_DIRECT_COLOUR_DEFAULT,
                  OSV_SS_DIRECT_COLOUR_ITEMS, kStaticFlags, OSV_SS_ID_DIRECT_COLOUR);

    // ---- 29. Close the Advanced group --------------------------------------
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OSV_SS_ID_ADVANCED_TOPIC_END);

    // ---- 30-33. Defaults [WP-DEFAULTS] -------------------------------------
    // Two momentary buttons: store this clip's settings as the defaults every
    // NEW clip starts from, or remove them so new clips start from the
    // built-in defaults again.  Neither changes this clip.  A button carries
    // no value; a click arrives as PF_Cmd_USER_CHANGED_PARAM, which is why
    // both carry PF_ParamFlag_SUPERVISE and nothing else - exactly how
    // Adobe's own Paramarama sample declares its button for every host,
    // Premiere included.  Collapsed, like Advanced: it is used once, not per
    // clip.
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPICX(OSV_SS_DEFAULTS_TOPIC_NAME, PF_ParamFlag_START_COLLAPSED, OSV_SS_ID_DEFAULTS_TOPIC);

    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON(OSV_SS_SAVE_DEFAULTS_NAME, OSV_SS_SAVE_DEFAULTS_BUTTON, 0, PF_ParamFlag_SUPERVISE,
                  OSV_SS_ID_SAVE_DEFAULTS);

    AEFX_CLR_STRUCT(def);
    PF_ADD_BUTTON(OSV_SS_RESTORE_DEFAULTS_NAME, OSV_SS_RESTORE_DEFAULTS_BUTTON, 0, PF_ParamFlag_SUPERVISE,
                  OSV_SS_ID_RESTORE_DEFAULTS);

    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(OSV_SS_ID_DEFAULTS_TOPIC_END);

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
    // leaves the controls exactly as they were (or, for a new clip with
    // untouched controls, on the user defaults seeded just below).
    PrefsBlob blob = prefsFromControls(readControls(params));

    // [WP-DEFAULTS] A NEW clip starts from the user's saved defaults.
    //
    // The SDK guide is explicit that this selector is the new-clip moment:
    // "When a clip is first imported, the effect is called with
    // PF_Cmd_SEQUENCE_SETUP" (a saved project comes back through
    // PF_Cmd_SEQUENCE_RESETUP).  Two more guards keep an existing clip's
    // settings untouched even if a host ever sent this selector for one:
    //
    //   * only UNTOUCHED controls are replaced - controls that translate to
    //     anything but PrefsBlob::defaults() are the clip's own settings;
    //   * the importer still answers below, and a live instance holding the
    //     clip's stored blob replaces this seed with it.
    //
    // No defaults file (or a corrupt one) leaves the seed at the built-in
    // defaults, i.e. exactly what this code did before.
    bool seededFromUser = false;
    std::string seedSource;
    if (params && blob == PrefsBlob::defaults()) {
        const UserDefaults user = osv::premiere::currentUserDefaults();
        if (user.fromFile && user.prefs != blob) {
            blob = user.prefs;
            seededFromUser = true;
            seedSource = osv::premiere::userDefaultsPathForLog(user.path);
        }
    }

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
    // [WP-DEFAULTS] Say, once per new clip, whether the user defaults took:
    // the importer answers for a clip it holds stored settings for, and then
    // those win - which is the rule, not a failure.
    if (seededFromUser) {
        if (blob == osv::premiere::userDefaults()) {
            PluginLog::info("source settings: new clip - controls set to the user defaults in {}", seedSource);
        } else {
            PluginLog::info("source settings: new clip - the importer answered with this clip's own settings, "
                            "which win over the user defaults in {}",
                            seedSource);
        }
    }
    return PF_Err_NONE;
}

/// [WP-DEFAULTS] Put a short confirmation where the host may show it.
///
/// out_data->return_msg is always filled (a host that shows it gets the
/// sentence), but PF_OutFlag_DISPLAY_ERROR_MESSAGE - which turns it into a
/// modal alert - is only raised outside Premiere.  That is exactly what
/// Adobe's Paramarama sample does for its button (it sets the flag only when
/// appl_id is not Premiere's), and a modal alert after every click of a
/// settings button would be the "annoying" kind of confirmation anyway.  In
/// Premiere the confirmation is the plug-in log line.
void reportDefaultsAction(const PF_InData* in_data, PF_OutData* out_data, const char* message) noexcept {
    if (!out_data || !message) {
        return;
    }
    std::snprintf(out_data->return_msg, PF_MAX_EFFECT_MSG_LEN, "%s", message);
    if (in_data && in_data->appl_id != kPremiereApplId) {
        out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
    }
}

/// [WP-DEFAULTS] PF_Cmd_USER_CHANGED_PARAM: the Defaults group's buttons.
///
///   Save as Default for New Clips - the controls as they stand, translated
///       exactly as PF_Cmd_TRANSLATE_PARAMS_TO_PREFS translates them (so the
///       defaults are precisely what this clip is decoded with), are written
///       to the user's defaults file.
///   Restore Built-in Defaults - the file is removed, so new clips start
///       from PrefsBlob::defaults() again.
///
/// Neither touches this clip's controls or its prefs: a default is about the
/// NEXT clip.  Every other parameter is ignored (none is supervised), and
/// every failure is logged and reported, never returned as an error - a
/// failing button must not make Premiere report a broken effect.
PF_Err userChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                        const PF_UserChangedParamExtra* extra) noexcept {
    if (!extra) {
        return PF_Err_NONE;
    }
    const PF_ParamIndex which = extra->param_index;

    if (which == kIndexSaveDefaults) {
        // Without the parameter values there is nothing true to save:
        // readControls() would hand back the built-in defaults, and saving
        // those silently is worse than saying no.
        if (!params || !params[kIndexColorOutput]) {
            PluginLog::warn("source settings: Save as Default arrived without parameter values; nothing saved");
            reportDefaultsAction(in_data, out_data, "Nothing saved: the host sent no settings.");
            return PF_Err_NONE;
        }
        const PrefsBlob blob = prefsFromControls(readControls(params));
        const osv::Status saved = osv::premiere::saveUserDefaults(blob);
        if (saved.ok()) {
            PluginLog::info("source settings: Save as Default for New Clips - saved");
            reportDefaultsAction(in_data, out_data, "Saved. New clips start with these settings.");
        } else {
            PluginLog::error("source settings: Save as Default for New Clips failed: {}", saved.error().message);
            reportDefaultsAction(in_data, out_data, "Could not save the defaults. See OpenOSVSourceSettings.log.");
        }
        return PF_Err_NONE;
    }

    if (which == kIndexRestoreDefaults) {
        const osv::Status removed = osv::premiere::resetUserDefaults();
        if (removed.ok()) {
            PluginLog::info("source settings: Restore Built-in Defaults - done");
            reportDefaultsAction(in_data, out_data, "Restored. New clips start with the built-in settings.");
        } else {
            PluginLog::error("source settings: Restore Built-in Defaults failed: {}", removed.error().message);
            reportDefaultsAction(in_data, out_data, "Could not restore the defaults. See OpenOSVSourceSettings.log.");
        }
        return PF_Err_NONE;
    }
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

    PluginLog::debug("source settings: translated - colour {}, look {}, HDR peak {:.0f} nits, size {}, stab {}, "
                     "seam {}, gain {}, calib {}, fit {}, exposure {:+.2f}, device {}, sun ghost removal {}, sky seam "
                     "fix {} at {:.0f} %, seam edge inset {:.1f} deg, seam blend {:.2f} / parallax blend {:.2f} / "
                     "smoothing {:.2f} deg, near / far offset {:+.2f} / {:+.2f} deg, lens shading {} at {:.0f} %",
                     blob.colorOutput, blob.look, static_cast<double>(blob.hdrPeakNits()), blob.outputSize,
                     blob.stabilization, blob.seamSearch,
                     blob.gainMatch, blob.calibration, blob.dlogmFit, static_cast<double>(blob.exposureStops),
                     blob.renderDevice, blob.flareRemoval, blob.photoSeam, blob.photoStrengthPercent(),
                     blob.seamInsetDeg(), blob.seamBlendDeg(), blob.parallaxBlendDeg(), blob.seamSmoothingDeg(),
                     blob.nearOffsetDeg(), blob.farOffsetDeg(), blob.lensShading, blob.shadingStrengthPercent());
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
static_assert(OSV_SS_CALIB_COUNT == static_cast<int>(osv::premiere::PrefsCalibrationChoice::Count),
              "the Calibration popup does not list every PrefsCalibrationChoice value");
static_assert(OSV_SS_FIT_COUNT == static_cast<int>(osv::premiere::PrefsDlogmFit::Count),
              "the D-Log M Curve popup does not list every PrefsDlogmFit value");
static_assert(OSV_SS_DEVICE_COUNT == static_cast<int>(osv::premiere::PrefsRenderDevice::Count),
              "the Render Device popup does not list every PrefsRenderDevice value");
static_assert(OSV_SS_DIRECT_COLOUR_COUNT == static_cast<int>(osv::premiere::PrefsDirectColour::Count),
              "the Program Monitor Colour popup does not list every PrefsDirectColour value");
static_assert(OSV_SS_LOOK_COUNT == static_cast<int>(osv::premiere::PrefsLook::Count),
              "the Rec.709 look popup does not list every PrefsLook value");
// [WP-HDRPEAK] The HDR peak popup lists every PrefsHdrPeak value, and its
// default is the zero byte (1000 nits, no roll-off).
static_assert(OSV_SS_HDR_PEAK_COUNT == static_cast<int>(osv::premiere::PrefsHdrPeak::Count),
              "the HDR Peak popup does not list every PrefsHdrPeak value");
static_assert(OSV_SS_HDR_PEAK_DEFAULT == static_cast<int>(osv::premiere::PrefsHdrPeak::Nits1000) + 1,
              "the HDR Peak popup's default is not PrefsBlob::defaults()' 1000 nits");
// [WP-PHOTO] The sky seam fix: the popup covers PrefsPhotoSeam and the two
// sliders offer exactly the range the blob can store - whole percent
// 0..100 (codes 1..101) and tenths 0.0..6.0 (codes 1..61) - with the blob's
// own defaults.
static_assert(OSV_SS_PHOTO_SEAM_COUNT == static_cast<int>(osv::premiere::PrefsPhotoSeam::Count),
              "the Sky Seam Fix popup does not list every PrefsPhotoSeam value");
static_assert(OSV_SS_PHOTO_SEAM_DEFAULT == static_cast<int>(osv::premiere::PrefsPhotoSeam::RimAndGain) + 1,
              "the Sky Seam Fix popup's default is not PrefsBlob::defaults()' RimAndGain");
// [WP-STEADY] Both popups list every choice, and their defaults are the
// blob's (item 1 is Auto in both tables; a test also checks defaults()).
static_assert(OSV_SS_PARALLAX_GRID_COUNT == static_cast<int>(osv::premiere::PrefsParallaxGrid::Count),
              "the Parallax Grid popup does not list every PrefsParallaxGrid value");
static_assert(osv::premiere::sourcesettings::kParallaxGridByPopup[OSV_SS_PARALLAX_GRID_DEFAULT - 1] ==
                  osv::premiere::PrefsParallaxGrid::Auto,
              "the Parallax Grid popup's default is not PrefsBlob::defaults()' Auto");
static_assert(OSV_SS_LENS_ALIGN_COUNT == static_cast<int>(osv::premiere::PrefsLensAlign::Count),
              "the Lens Alignment popup does not list every PrefsLensAlign value");
static_assert(osv::premiere::sourcesettings::kLensAlignByPopup[OSV_SS_LENS_ALIGN_DEFAULT - 1] ==
                  osv::premiere::PrefsLensAlign::Auto,
              "the Lens Alignment popup's default is not PrefsBlob::defaults()' Auto");
static_assert(OSV_SS_PHOTO_STRENGTH_MIN == 0.0 &&
                  OSV_SS_PHOTO_STRENGTH_MAX == static_cast<double>(osv::premiere::PrefsBlob::kMaxPhotoStrengthCode - 1),
              "the Sky Seam Strength range does not match PrefsBlob::photoStrength");
static_assert(OSV_SS_PHOTO_STRENGTH_DEFAULT == 100.0, "Sky Seam Strength defaults to 100 %, stored as code 0");
static_assert(OSV_SS_SEAM_INSET_MIN == 0.0 &&
                  OSV_SS_SEAM_INSET_MAX == static_cast<double>(osv::premiere::PrefsBlob::kMaxSeamInsetCode - 1) / 10.0,
              "the Seam Edge Inset range does not match PrefsBlob::seamInset");
static_assert(OSV_SS_SEAM_INSET_DEFAULT == static_cast<double>(osv::premiere::PrefsBlob::kDefaultSeamInsetTenths) / 10.0,
              "the Seam Edge Inset default does not match PrefsBlob::kDefaultSeamInsetTenths");
// [WP-SEAMTOOLS] Each slider offers exactly the range its blob field stores,
// with the blob's own default.
static_assert(OSV_SS_SEAM_BLEND_MIN == static_cast<double>(osv::premiere::PrefsBlob::kMinSeamBlendCode - 1) /
                                           osv::premiere::PrefsBlob::kSeamToolStepsPerDeg &&
                  OSV_SS_SEAM_BLEND_MAX == static_cast<double>(osv::premiere::PrefsBlob::kMaxSeamBlendCode - 1) /
                                               osv::premiere::PrefsBlob::kSeamToolStepsPerDeg,
              "the Seam Blend range does not match PrefsBlob::seamBlend");
static_assert(OSV_SS_SEAM_BLEND_DEFAULT == osv::premiere::PrefsBlob::kDefaultSeamBlendDeg,
              "the Seam Blend default does not match PrefsBlob::kDefaultSeamBlendDeg");
static_assert(OSV_SS_PARALLAX_BLEND_MIN == 0.0 &&
                  OSV_SS_PARALLAX_BLEND_MAX ==
                      static_cast<double>(osv::premiere::PrefsBlob::kMaxParallaxBlendCode - 1) /
                          osv::premiere::PrefsBlob::kSeamToolStepsPerDeg,
              "the Parallax Blend range does not match PrefsBlob::parallaxBlend");
static_assert(OSV_SS_PARALLAX_BLEND_DEFAULT == osv::premiere::PrefsBlob::kDefaultParallaxBlendDeg,
              "the Parallax Blend default does not match PrefsBlob::kDefaultParallaxBlendDeg");
static_assert(OSV_SS_SEAM_SMOOTHING_MIN == 0.0 && OSV_SS_SEAM_SMOOTHING_DEFAULT == 0.0 &&
                  OSV_SS_SEAM_SMOOTHING_MAX ==
                      static_cast<double>(osv::premiere::PrefsBlob::kMaxSeamSmoothingCode - 1) /
                          osv::premiere::PrefsBlob::kSeamToolStepsPerDeg,
              "the Seam Smoothing range does not match PrefsBlob::seamSmoothing");
static_assert(OSV_SS_SEAM_OFFSET_MAX == static_cast<double>(osv::premiere::PrefsBlob::kMaxSeamOffsetHundredths) / 100.0 &&
                  OSV_SS_SEAM_OFFSET_MIN == -OSV_SS_SEAM_OFFSET_MAX && OSV_SS_SEAM_OFFSET_DEFAULT == 0.0,
              "the Near / Far Offset range does not match PrefsBlob::nearOffset / farOffset");
// [WP-VIGNETTE] The lens shading correction: the popup covers
// PrefsLensShading with defaults()' Auto, and the slider offers exactly the
// whole percent 0..100 (codes 1..101) the blob stores.
static_assert(OSV_SS_LENS_SHADING_COUNT == static_cast<int>(osv::premiere::PrefsLensShading::Count),
              "the Lens Shading popup does not list every PrefsLensShading value");
static_assert(OSV_SS_LENS_SHADING_DEFAULT == static_cast<int>(osv::premiere::PrefsLensShading::Auto) + 1,
              "the Lens Shading popup's default is not PrefsBlob::defaults()' Auto");
static_assert(OSV_SS_SHADING_STRENGTH_MIN == 0.0 &&
                  OSV_SS_SHADING_STRENGTH_MAX ==
                      static_cast<double>(osv::premiere::PrefsBlob::kMaxShadingStrengthCode - 1),
              "the Shading Strength range does not match PrefsBlob::shadingStrength");
static_assert(OSV_SS_SHADING_STRENGTH_DEFAULT == 100.0, "Shading Strength defaults to 100 %, stored as code 0");

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

            // [WP-DEFAULTS] A click on one of the Defaults group's buttons.
            case PF_Cmd_USER_CHANGED_PARAM:
                return userChangedParam(in_data, out_data, params,
                                        static_cast<const PF_UserChangedParamExtra*>(extra));

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
