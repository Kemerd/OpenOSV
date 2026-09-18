/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * SourceSettingsParams.h - the ONE description of "OpenOSV Source Settings".
 *
 * Out-flags, parameter ids, indices, popup item strings, ranges and defaults
 * all live here, and the header is read by THREE consumers, exactly like
 * ReframeParams.h is for the reframe effect:
 *
 *   1. SourceSettingsMain.cpp        - the AE API side (PARAMS_SETUP,
 *                                      TRANSLATE_PARAMS_TO_PREFS);
 *   2. OpenOSVSourceSettings.r       - the PiPL, preprocessed by cl /EP and
 *                                      fed to PiPLtool.exe;
 *   3. tests/premiere/sourcesettings  - the tests, which walk the real
 *                                      parameter list the built module
 *                                      produced and compare it to this table.
 *
 * Consumer 2 is why every C++ construct is behind OSV_SOURCE_SETTINGS_CPLUSPLUS
 * and why the flag words are plain decimal literals: PiPLtool.exe evaluates
 * the resource text with its own parser and fails with
 * "PIPL_GetExpression: Matching parantheses expected!" on anything more
 * structured than a number.  SourceSettingsMain.cpp static_asserts each
 * literal against the real AE_Effect.h macro, so an SDK that renumbers a bit
 * breaks the build instead of shipping a wrong PiPL.
 *
 * ===========================================================================
 *  WHY THIS EFFECT CANNOT BE THE REFRAME EFFECT
 * ===========================================================================
 *
 * A source settings effect is a very particular kind of AE effect and the
 * constraint is structural, not a design preference:
 *
 *   * Premiere NEVER sends it PF_Cmd_RENDER.  It is attached to the MASTER
 *     CLIP, upstream of the timeline, and its only job is to describe how the
 *     media should be DECODED.  There is no frame for it to filter.
 *   * Its parameter values reach the importer through
 *     PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, which produces ONE FLAT BYTE BLOB -
 *     our 128-byte PrefsBlob - handed to imGetPrefs8 / imGetSourceVideo.
 *     A blob has no time axis, so a keyframe has nowhere to go: the host can
 *     only ask for "the prefs", once, for the whole clip.
 *
 * Both facts together mean every control here is necessarily STATIC per clip.
 * That is why each one carries PF_ParamFlag_CANNOT_TIME_VARY: a stopwatch on
 * a control whose keyframes can never reach the decoder is a control that
 * silently does nothing, and hiding the stopwatch is the honest UI.
 *
 * Pan / Tilt / Roll / FOV are exactly the parameters a user MUST be able to
 * keyframe, so they stay in Open360Reframe.aex, which is a normal timeline
 * effect that does receive PF_Cmd_RENDER at a time and can read its
 * parameters per frame.  The two effects are therefore separate modules, and
 * separate modules is also what the AE SDK requires - it lists "multiple
 * PiPLs in a single plug-in" among the features Premiere does not support.
 */
#ifndef OSV_SOURCE_SETTINGS_PARAMS_H
#define OSV_SOURCE_SETTINGS_PARAMS_H

/* The match name, display name and category, shared with the importer. */
#include "SourceSettingsIdentity.h"

/* ==========================================================================
 *  Version
 * ========================================================================== */
#define OSV_SOURCE_SETTINGS_VERSION_MAJOR 1
#define OSV_SOURCE_SETTINGS_VERSION_MINOR 0
#define OSV_SOURCE_SETTINGS_VERSION_BUG 0

/* PF_VERSION(MAJOR, MINOR, BUG, STAGE, BUILD) packed by hand:
 *     ((MAJOR << 19) | (MINOR << 15) | (BUG << 11) | (STAGE << 9) | BUILD)
 * Stage 3 = PF_Stage_RELEASE, build 0, so 1.0.0 release packs to
 *     (1 << 19) | (3 << 9) = 524288 + 1536 = 525824.
 * A bare decimal literal, for the PiPLtool reason in the file header; a
 * static_assert in SourceSettingsMain.cpp ties it to PF_VERSION(). */
#define OSV_SOURCE_SETTINGS_STAGE 3
#define OSV_SOURCE_SETTINGS_BUILD 0
#define OSV_SOURCE_SETTINGS_PIPL_VERSION 525824

/* ==========================================================================
 *  Global out-flags (PiPL <-> PF_Cmd_GLOBAL_SETUP)
 *
 *  out_flags:
 *    PF_OutFlag_SEND_UPDATE_PARAMS_UI  1L << 26 = 0x04000000
 *        required to receive PF_Cmd_UPDATE_PARAMS_UI, which is the only hook
 *        for greying a control.  Nothing is greyed today; the flag is set
 *        anyway because adding it later changes the PiPL, and a changed PiPL
 *        invalidates the plug-in cache on every installed machine.
 *
 *  Deliberately NOT set, and each for a concrete reason:
 *    PF_OutFlag_DEEP_COLOR_AWARE / PF_OutFlag2_FLOAT_COLOR_AWARE
 *        both describe how PF_Cmd_RENDER handles pixels.  This effect is
 *        never sent PF_Cmd_RENDER (see the file header), so claiming a pixel
 *        capability would be a claim about code that does not exist.
 *    PF_OutFlag_CUSTOM_UI
 *        no Program Monitor overlay: there is nothing spatial to drag here.
 *    PF_OutFlag_NON_PARAM_VARY / PF_OutFlag_FORCE_RERENDER
 *        the host re-reads the prefs and refreshes the media itself when a
 *        translated blob changes; forcing a re-render on top of that only
 *        adds cache churn.
 *
 *  out_flags2:
 *    PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG 1L << 3  = 0x00000008
 *        honour PF_ParamFlag_START_COLLAPSED on the Advanced topic.
 *    PF_OutFlag2_SUPPORTS_THREADED_RENDERING      1L << 27 = 0x08000000
 *        sequence_data is null and every handler is re-entrant, so the host
 *        may call this effect from any thread.
 * ========================================================================== */
#define OSV_SOURCE_SETTINGS_OUT_FLAGS 0x04000000
#define OSV_SOURCE_SETTINGS_OUT_FLAGS_2 0x08000008

/* AE_Effect_Info_Flags: none. */
#define OSV_SOURCE_SETTINGS_INFO_FLAGS 0

/* AE_Reserved_Info: 8 in every AE-kind PiPL Adobe ships. */
#define OSV_SOURCE_SETTINGS_RESERVED_INFO 8

/* ==========================================================================
 *  Parameter identifiers
 *
 *  PERMANENT ids stored in project files (def.uu.id).  Never reused, never
 *  renumbered: a saved project that loses an id loses the user's stitch
 *  settings for that clip.
 *
 *  As in the reframe effect, an id is NOT an index: PF_ADD_TOPIC and
 *  PF_END_TOPIC each issue their own PF_ADD_PARAM (Param_Utils.h:298-320), so
 *  a group occupies two real parameter slots and the GROUP_END slot sits in
 *  the MIDDLE of the list.
 * ========================================================================== */
#define OSV_SS_ID_COLOR_OUTPUT 1
#define OSV_SS_ID_OUTPUT_SIZE 2
#define OSV_SS_ID_STABILIZATION 3
#define OSV_SS_ID_STITCH_TOPIC 4
#define OSV_SS_ID_SEAM_SEARCH 5
#define OSV_SS_ID_GAIN_MATCH 6
#define OSV_SS_ID_CALIBRATION 7
#define OSV_SS_ID_STITCH_TOPIC_END 8
#define OSV_SS_ID_ADVANCED_TOPIC 9
#define OSV_SS_ID_DLOGM_FIT 10
#define OSV_SS_ID_EXPOSURE 11
#define OSV_SS_ID_RENDER_DEVICE 12
#define OSV_SS_ID_ADVANCED_TOPIC_END 13

/* Total parameters excluding the input layer: 9 controls + 4 group markers.
 * out_data->num_params is this + 1. */
#define OSV_SOURCE_SETTINGS_PARAM_COUNT 13

/* ==========================================================================
 *  Popup item strings
 *
 *  AE popup items are one string with '|' between entries, and the popup
 *  value is 1-BASED while the PrefsBlob enum is 0-based, so every mapping in
 *  SourceSettingsMain.cpp is a deliberate -1 / +1.  The ORDER of every list
 *  below is the PrefsBlob enum order (PrefsBlob.h) and must stay that way:
 *  inserting an entry anywhere but the end would silently re-map the setting
 *  in every previously saved project.
 * ========================================================================== */

/* PrefsColorOutput: PQ, HLG, Rec709, DLogM.
 *
 * "D-Log M (no transform)" is the grade-it-yourself option: the sphere comes
 * through in the camera's own log encoding and gamut, ready for a D-Log M LUT
 * or a Lumetri log conversion downstream.  The label says "no transform"
 * rather than just "D-Log M" because the other three entries name what the
 * output IS, and this one has to say that nothing was done to it - a user who
 * reads it as "convert to D-Log M" would then apply a LUT on top and
 * double-convert. */
#define OSV_SS_COLOR_ITEMS "BT.2100 PQ|BT.2100 HLG|Rec. 709|D-Log M (no transform)"
#define OSV_SS_COLOR_COUNT 4
#define OSV_SS_COLOR_DEFAULT 1

/* PrefsOutputSize: Native, UHD4K, QHD2560, HD2K.  Every entry is 2:1 - a
 * full 360 x 180 sphere always is - and the label spells the pixels out
 * because the number a user cares about is the one a new sequence inherits. */
#define OSV_SS_SIZE_ITEMS "Native (2 x decoded height)|4K (3840 x 1920)|2560 x 1280|2K (1920 x 960)"
#define OSV_SS_SIZE_COUNT 4
/* 1 = Native (the popup is 1-based, PrefsOutputSize::Native is 0), which is
 * PrefsBlob::defaults().outputSize.  A static_assert in
 * SourceSettingsMain.cpp checks this literal against the blob so the popup
 * default and the decoder default cannot disagree.
 *
 * Native rather than a fixed size because the importer's job is to hand over
 * every pixel the camera recorded; a reframe crops a small window out of the
 * sphere and therefore MAGNIFIES it, so a downscaled default made zooming
 * soft for no reason.  The timeline size is chosen by the sequence presets,
 * which is where that decision belongs. */
#define OSV_SS_SIZE_DEFAULT 1

/* PrefsStabilization: Off, HorizonLock, Full, Smooth. */
#define OSV_SS_STAB_ITEMS "Off|Horizon Lock|Full|Smooth"
#define OSV_SS_STAB_COUNT 4
#define OSV_SS_STAB_DEFAULT 2 /* HorizonLock */

/* PrefsCalibration: Native, LensGuards, Underwater. */
#define OSV_SS_CALIB_ITEMS "Native|Lens Guards|Underwater"
#define OSV_SS_CALIB_COUNT 3
#define OSV_SS_CALIB_DEFAULT 1

/* PrefsDlogmFit: DjiRefit, Pocket3, Osmo360.  The enum is append-only (the
 * values are persisted), so Osmo 360 is last in the list even though it is
 * the default; the default is the 1-based popup index 3. */
#define OSV_SS_FIT_ITEMS "DJI Refit|Pocket 3|Osmo 360"
#define OSV_SS_FIT_COUNT 3
#define OSV_SS_FIT_DEFAULT 3

/* PrefsRenderDevice: Auto, Cpu, Cuda, OpenCl. */
#define OSV_SS_DEVICE_ITEMS "Auto|CPU|CUDA|OpenCL"
#define OSV_SS_DEVICE_COUNT 4
#define OSV_SS_DEVICE_DEFAULT 1

/* ==========================================================================
 *  Checkbox and slider ranges / defaults
 * ========================================================================== */
#define OSV_SS_SEAM_SEARCH_DEFAULT 1
#define OSV_SS_GAIN_MATCH_DEFAULT 1

/* Exposure, in stops.  The valid range is PrefsBlob::kMinExposureStops ..
 * kMaxExposureStops; a static_assert checks these literals against the blob's
 * own constants so the slider cannot offer a value sanitise() would clamp. */
#define OSV_SS_EXPOSURE_VALID_MIN -6.0
#define OSV_SS_EXPOSURE_VALID_MAX 6.0
#define OSV_SS_EXPOSURE_SLIDER_MIN -3.0
#define OSV_SS_EXPOSURE_SLIDER_MAX 3.0
#define OSV_SS_EXPOSURE_DEFAULT 0.0

/* ==========================================================================
 *  Everything below is C++ only.
 * ========================================================================== */
#if defined(OSV_SOURCE_SETTINGS_CPLUSPLUS)

#include <cstddef>

namespace osv::premiere::sourcesettings {

/// AE parameter index of each control.  Index 0 is the input layer, which AE
/// inserts itself, so the first control we add is index 1.
///
/// This is the ADD ORDER in paramsSetup(), which is NOT the id order: the two
/// PF_Param_GROUP_END markers sit after the last control of their group, so
/// every control that follows a closed group is shifted up by one per group
/// already closed.  Written out literally rather than derived, because
/// deriving it from the ids is precisely the mistake that unbalances groups.
///
///   1  Colour Output
///   2  Output Size
///   3  Stabilisation
///   4  Stitching          (GROUP_START)
///   5    Seam Search
///   6    Exposure Match
///   7    Calibration
///   8  (GROUP_END, Stitching)
///   9  Advanced           (GROUP_START, starts collapsed)
///  10    D-Log M Curve
///  11    Exposure
///  12    Render Device
///  13  (GROUP_END, Advanced)
enum ParamIndex : int {
    kIndexColorOutput = 1,
    kIndexOutputSize = 2,
    kIndexStabilization = 3,
    kIndexStitchTopic = 4,
    kIndexSeamSearch = 5,
    kIndexGainMatch = 6,
    kIndexCalibration = 7,
    kIndexStitchTopicEnd = 8,
    kIndexAdvancedTopic = 9,
    kIndexDlogmFit = 10,
    kIndexExposure = 11,
    kIndexRenderDevice = 12,
    kIndexAdvancedTopicEnd = 13,
};

/// The permanent id stored at each index, in index order (index 1 first).
/// paramsSetup() adds them in this order and a test walks this table against
/// the list the built module actually produced.
inline constexpr int kParamIdByIndex[OSV_SOURCE_SETTINGS_PARAM_COUNT] = {
    OSV_SS_ID_COLOR_OUTPUT,     OSV_SS_ID_OUTPUT_SIZE,   OSV_SS_ID_STABILIZATION,
    OSV_SS_ID_STITCH_TOPIC,     OSV_SS_ID_SEAM_SEARCH,   OSV_SS_ID_GAIN_MATCH,
    OSV_SS_ID_CALIBRATION,      OSV_SS_ID_STITCH_TOPIC_END, OSV_SS_ID_ADVANCED_TOPIC,
    OSV_SS_ID_DLOGM_FIT,        OSV_SS_ID_EXPOSURE,      OSV_SS_ID_RENDER_DEVICE,
    OSV_SS_ID_ADVANCED_TOPIC_END,
};

/// Number of user-visible parameters (excludes the input layer).
inline constexpr int kParamCount = OSV_SOURCE_SETTINGS_PARAM_COUNT;

/// Number of controls that actually carry a value, i.e. everything except the
/// four GROUP_START / GROUP_END markers.  This is the count that has to round
/// trip through a PrefsBlob.
inline constexpr int kValueParamCount = 9;

/// The parameter names, in index order, so a test can compare the built
/// module's list without repeating the strings.
///
/// The two GROUP_END entries are deliberately EMPTY.  PF_ADD_TOPIC takes a
/// name and sets it; PF_END_TOPIC takes only an id (Param_Utils.h:309-316)
/// and leaves the name field zeroed, because a group terminator is a divider
/// rather than a labelled control.  Writing "Stitching" here would have
/// described a field the SDK never fills.
inline constexpr const char* kParamNameByIndex[OSV_SOURCE_SETTINGS_PARAM_COUNT] = {
    "Colour Output", "Output Size",   "Stabilisation", "Stitching", "Seam Search",
    "Exposure Match", "Calibration",  "",              "Advanced",  "D-Log M Curve",
    "Exposure",       "Render Device", "",
};

}  // namespace osv::premiere::sourcesettings

#endif /* OSV_SOURCE_SETTINGS_CPLUSPLUS */

#endif /* OSV_SOURCE_SETTINGS_PARAMS_H */
