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
 *        honour PF_ParamFlag_START_COLLAPSED on the Advanced topic (and
 *        [WP-DEFAULTS] the Defaults topic).
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
/* [WP-SETTINGS] Added after the list above was shipped: a NEW id rather than a
 * reused one, placed inside the Advanced group (so its index is 13 and the
 * group terminator moved to 14 - indices are not persisted, ids are). */
#define OSV_SS_ID_DIRECT_COLOUR 14
/* [WP-LOOK] The Rec.709 display look.  A NEW id, placed right under Colour
 * Output (index 2) because the two are read together; every index after it
 * moved up by one, which is harmless - indices are not persisted, ids are. */
#define OSV_SS_ID_REC709_LOOK 15
/* [WP-FLARE] "Sun Ghost Removal", a NEW id placed inside the Stitching group
 * after Calibration (index 9); everything after it moved up by one - indices
 * are not persisted, ids are. */
#define OSV_SS_ID_FLARE_REMOVAL 16
/* [WP-PHOTO] The sky seam fix: three NEW ids placed inside the Stitching group
 * right after Sun Ghost Removal (indices 10-12); everything after them moved
 * up by three - indices are not persisted, ids are. */
#define OSV_SS_ID_PHOTO_SEAM 17
#define OSV_SS_ID_PHOTO_STRENGTH 18
#define OSV_SS_ID_SEAM_INSET 19
/* [WP-SEAMTOOLS] The carved seam's tweaks: five NEW ids (20-29 are this
 * package's range) placed inside the Stitching group right after Seam Edge
 * Inset (indices 13-17); everything after them moved up by five - indices
 * are not persisted, ids are. */
#define OSV_SS_ID_SEAM_BLEND 20
#define OSV_SS_ID_PARALLAX_BLEND 21
#define OSV_SS_ID_SEAM_SMOOTHING 22
#define OSV_SS_ID_NEAR_OFFSET 23
#define OSV_SS_ID_FAR_OFFSET 24
/* [WP-DEFAULTS] The "Defaults" group, LAST in the list (indices 25-28): two
 * buttons that store this clip's settings as the defaults every NEW clip
 * starts from, or remove them again (plugins/common/UserDefaults.h).  Ids
 * from 30 up; 20-29 belong to the Stitching group's own additions. */
#define OSV_SS_ID_DEFAULTS_TOPIC 30
#define OSV_SS_ID_SAVE_DEFAULTS 31
#define OSV_SS_ID_RESTORE_DEFAULTS 32
#define OSV_SS_ID_DEFAULTS_TOPIC_END 33
/* [WP-VIGNETTE] The lens shading correction: two NEW ids after the last one
 * in use (34-39 are this package's range), placed inside the Stitching group
 * right after Far Offset (indices 18-19); everything after them moved up by
 * two - indices are not persisted, ids are.  The Defaults group stays last. */
#define OSV_SS_ID_LENS_SHADING 34
#define OSV_SS_ID_SHADING_STRENGTH 35

/* Total parameters excluding the input layer: 22 value controls + 2 buttons
 * + 6 group markers.  out_data->num_params is this + 1. */
#define OSV_SOURCE_SETTINGS_PARAM_COUNT 30

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

/* PrefsCalibrationChoice, in POPUP order (kCalibrationChoiceByPopup in
 * SourceSettingsMapping.h): Auto, Lens Protectors / ND Filters, Underwater,
 * Native.
 *
 * The first three keep the positions of the original "Native|Lens
 * Guards|Underwater" list.  Its first entry never forced anything - calibration
 * 0 always followed the accessory the camera recorded - so it is now labelled
 * for what it does, Auto, and a saved project keeps its exact meaning.  Forcing
 * the bare-lens set is new and therefore appended as the fourth item. */
#define OSV_SS_CALIB_ITEMS "Auto (as recorded)|Lens Protectors / ND Filters|Underwater|Native (bare lenses)"
#define OSV_SS_CALIB_COUNT 4
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

/* [WP-SETTINGS] "Program Monitor Colour" - PrefsDirectColour: SequenceSpace,
 * MatchSource.
 *
 * What Open 360 Reframe shows when this clip's Colour Output is not the
 * sequence's working colour space.  "Sequence space (fast)" renders straight
 * from the fisheyes into the working space with OpenOSV's own tone mapping -
 * the fast, sharp route, and in a Rec.709 sequence the DJI-matched look.
 * "Match Source monitor" hands such a clip to the importer's equirect so the
 * Program monitor shows exactly Premiere's conversion of the chosen output,
 * as the Source monitor does - slower and one resampling softer.
 * Default 1 = Sequence space (PrefsDirectColour::SequenceSpace is 0). */
#define OSV_SS_DIRECT_COLOUR_ITEMS "Sequence space (fast)|Match Source monitor"
#define OSV_SS_DIRECT_COLOUR_COUNT 2
#define OSV_SS_DIRECT_COLOUR_DEFAULT 1

/* [WP-LOOK] "Look (Rec. 709 only)" - PrefsLook: DjiStudio, Standard.
 *
 * The display look of the Rec. 709 Colour Output: DJI Studio's own D-Log M
 * rendering (fitted to DJI's Osmo 360 LUT, the default) or OpenOSV's standard
 * rendering (the HLG signal in Rec. 709 primaries, what Rec. 709 was before
 * the look existed).  PQ, HLG and the passthrough ignore it; the name says so
 * because a source settings effect has no dependable way to grey a control
 * out in Premiere.  Default 1 = DJI (PrefsLook::DjiStudio is 0). */
#define OSV_SS_LOOK_ITEMS "DJI (default)|OpenOSV standard"
#define OSV_SS_LOOK_COUNT 2
#define OSV_SS_LOOK_DEFAULT 1

/* [WP-PHOTO] "Sky Seam Fix" - PrefsPhotoSeam: Off, RimOnly, RimAndGain.
 *
 * The photometric seam field (docs/research/NEURAL_STITCHING.md, section 8):
 * "Rim only" ends each lens's blend weight at its measured usable rim per
 * longitude; "Rim and colour" also evens the two lenses' brightness and
 * colour across the overlap with a 2-D gain field (replacing Exposure Match's
 * one global gain).  "Off" leaves the seam edge inset and the global gain.
 * Default 3 = Rim and colour (PrefsPhotoSeam::RimAndGain is 2), as
 * PrefsBlob::defaults(); an older project's zero byte reads as Off. */
#define OSV_SS_PHOTO_SEAM_ITEMS "Off|Rim only|Rim and colour"
#define OSV_SS_PHOTO_SEAM_COUNT 3
#define OSV_SS_PHOTO_SEAM_DEFAULT 3

/* [WP-VIGNETTE] "Lens Shading" - PrefsLensShading: Off, Auto.
 *
 * Each lens's own brightness structure near its rim - on the sample clip a
 * soft dark ring in the lens facing the sun, which leaves a darker band on
 * every sky seam crossing - measured from that lens's own sky and added back
 * before the lenses are blended (docs/research/NEURAL_STITCHING.md, section
 * 9).  A lens whose sky shows no structure is left untouched.  Default 2 =
 * Auto (PrefsLensShading::Auto is 1), as PrefsBlob::defaults(); an older
 * project's zero byte reads as Off. */
#define OSV_SS_LENS_SHADING_ITEMS "Off|Auto"
#define OSV_SS_LENS_SHADING_COUNT 2
#define OSV_SS_LENS_SHADING_DEFAULT 2

/* ==========================================================================
 *  Checkbox and slider ranges / defaults
 * ========================================================================== */
#define OSV_SS_SEAM_SEARCH_DEFAULT 1
#define OSV_SS_GAIN_MATCH_DEFAULT 1
/* [WP-FLARE] on, as PrefsBlob::defaults().flareRemoval (a test pins it). */
#define OSV_SS_FLARE_REMOVAL_DEFAULT 1

/* Exposure, in stops.  The valid range is PrefsBlob::kMinExposureStops ..
 * kMaxExposureStops; a static_assert checks these literals against the blob's
 * own constants so the slider cannot offer a value sanitise() would clamp. */
#define OSV_SS_EXPOSURE_VALID_MIN -6.0
#define OSV_SS_EXPOSURE_VALID_MAX 6.0
#define OSV_SS_EXPOSURE_SLIDER_MIN -3.0
#define OSV_SS_EXPOSURE_SLIDER_MAX 3.0
#define OSV_SS_EXPOSURE_DEFAULT 0.0

/* [WP-PHOTO] "Sky Seam Strength", in percent: how much of the colour field
 * is applied (Rim and colour only).  The blob stores whole percent 0..100
 * (PrefsBlob::photoStrengthPercent); static_asserts tie the limits to it. */
#define OSV_SS_PHOTO_STRENGTH_MIN 0.0
#define OSV_SS_PHOTO_STRENGTH_MAX 100.0
#define OSV_SS_PHOTO_STRENGTH_DEFAULT 100.0

/* [WP-PHOTO] "Seam Edge Inset", in degrees: how far inside the calibrated
 * field of view the render blend ends when the sky seam fix is off or
 * refused (the fix's own per-longitude rim replaces it otherwise).  The blob
 * stores tenths 0.0..6.0 (PrefsBlob::seamInsetDeg); default 2.6. */
#define OSV_SS_SEAM_INSET_MIN 0.0
#define OSV_SS_SEAM_INSET_MAX 6.0
#define OSV_SS_SEAM_INSET_DEFAULT 2.6

/* [WP-SEAMTOOLS] The carved seam's tweaks, in degrees (osv/render/SeamTools.h).
 * The blob stores twentieths of a degree for the three widths and hundredths
 * for the two offsets (PrefsBlob::seamBlendDeg ...); static_asserts in
 * SourceSettingsMain.cpp tie every limit and default below to it.
 *
 *   Seam Blend       the feather where the lenses agree (default 1.5 = the
 *                    carved seam as it has always been);
 *   Parallax Blend   the feather where they disagree (0.35; 0 = a hard cut;
 *                    never wider than Seam Blend);
 *   Seam Smoothing   blends colour and shading across this half width while
 *                    detail still switches at the seam (0 = off);
 *   Near Offset      nudges near content along the seam (up / down where the
 *   Far Offset       seam runs vertically past the camera), far content. */
#define OSV_SS_SEAM_BLEND_MIN 0.2
#define OSV_SS_SEAM_BLEND_MAX 8.0
#define OSV_SS_SEAM_BLEND_DEFAULT 1.5
#define OSV_SS_PARALLAX_BLEND_MIN 0.0
#define OSV_SS_PARALLAX_BLEND_MAX 4.0
#define OSV_SS_PARALLAX_BLEND_DEFAULT 0.35
#define OSV_SS_SEAM_SMOOTHING_MIN 0.0
#define OSV_SS_SEAM_SMOOTHING_MAX 8.0
#define OSV_SS_SEAM_SMOOTHING_DEFAULT 0.0
#define OSV_SS_SEAM_OFFSET_MIN -3.0
#define OSV_SS_SEAM_OFFSET_MAX 3.0
#define OSV_SS_SEAM_OFFSET_DEFAULT 0.0

/* [WP-VIGNETTE] "Shading Strength", in percent: how much of the measured
 * lens shading correction is applied.  The blob stores whole percent 0..100
 * (PrefsBlob::shadingStrengthPercent); static_asserts tie the limits to it. */
#define OSV_SS_SHADING_STRENGTH_MIN 0.0
#define OSV_SS_SHADING_STRENGTH_MAX 100.0
#define OSV_SS_SHADING_STRENGTH_DEFAULT 100.0

/* [WP-DEFAULTS] The Defaults group's two buttons: the parameter names (the
 * panel's left column) and the words on the buttons themselves. */
#define OSV_SS_DEFAULTS_TOPIC_NAME "Defaults"
#define OSV_SS_SAVE_DEFAULTS_NAME "Save"
#define OSV_SS_SAVE_DEFAULTS_BUTTON "Save as Default for New Clips"
#define OSV_SS_RESTORE_DEFAULTS_NAME "Restore"
#define OSV_SS_RESTORE_DEFAULTS_BUTTON "Restore Built-in Defaults"

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
///   2  Look (Rec. 709 only)     [WP-LOOK]
///   3  Output Size
///   4  Stabilisation
///   5  Stitching          (GROUP_START)
///   6    Seam Search
///   7    Exposure Match
///   8    Calibration
///   9    Sun Ghost Removal        [WP-FLARE]
///  10    Sky Seam Fix             [WP-PHOTO]
///  11    Sky Seam Strength        [WP-PHOTO]
///  12    Seam Edge Inset          [WP-PHOTO]
///  13    Seam Blend               [WP-SEAMTOOLS]
///  14    Parallax Blend           [WP-SEAMTOOLS]
///  15    Seam Smoothing           [WP-SEAMTOOLS]
///  16    Near Offset              [WP-SEAMTOOLS]
///  17    Far Offset               [WP-SEAMTOOLS]
///  18    Lens Shading             [WP-VIGNETTE]
///  19    Shading Strength         [WP-VIGNETTE]
///  20  (GROUP_END, Stitching)
///  21  Advanced           (GROUP_START, starts collapsed)
///  22    D-Log M Curve
///  23    Exposure
///  24    Render Device
///  25    Program Monitor Colour   [WP-SETTINGS]
///  26  (GROUP_END, Advanced)
///  27  Defaults           (GROUP_START, starts collapsed)   [WP-DEFAULTS]
///  28    Save       [Save as Default for New Clips]
///  29    Restore    [Restore Built-in Defaults]
///  30  (GROUP_END, Defaults)
enum ParamIndex : int {
    kIndexColorOutput = 1,
    kIndexRec709Look = 2,
    kIndexOutputSize = 3,
    kIndexStabilization = 4,
    kIndexStitchTopic = 5,
    kIndexSeamSearch = 6,
    kIndexGainMatch = 7,
    kIndexCalibration = 8,
    kIndexFlareRemoval = 9,     // [WP-FLARE]
    kIndexPhotoSeam = 10,       // [WP-PHOTO]
    kIndexPhotoStrength = 11,   // [WP-PHOTO]
    kIndexSeamInset = 12,       // [WP-PHOTO]
    kIndexSeamBlend = 13,       // [WP-SEAMTOOLS]
    kIndexParallaxBlend = 14,   // [WP-SEAMTOOLS]
    kIndexSeamSmoothing = 15,   // [WP-SEAMTOOLS]
    kIndexNearOffset = 16,      // [WP-SEAMTOOLS]
    kIndexFarOffset = 17,       // [WP-SEAMTOOLS]
    kIndexLensShading = 18,     // [WP-VIGNETTE]
    kIndexShadingStrength = 19, // [WP-VIGNETTE]
    kIndexStitchTopicEnd = 20,
    kIndexAdvancedTopic = 21,
    kIndexDlogmFit = 22,
    kIndexExposure = 23,
    kIndexRenderDevice = 24,
    kIndexDirectColour = 25,
    kIndexAdvancedTopicEnd = 26,
    // [WP-DEFAULTS] Always the last group, so its indices are written
    // relative to the Advanced terminator: a control added to an earlier
    // group moves them with it and nothing here has to be renumbered.
    kIndexDefaultsTopic = kIndexAdvancedTopicEnd + 1,
    kIndexSaveDefaults = kIndexDefaultsTopic + 1,
    kIndexRestoreDefaults = kIndexSaveDefaults + 1,
    kIndexDefaultsTopicEnd = kIndexRestoreDefaults + 1,
};

/// The permanent id stored at each index, in index order (index 1 first).
/// paramsSetup() adds them in this order and a test walks this table against
/// the list the built module actually produced.
inline constexpr int kParamIdByIndex[OSV_SOURCE_SETTINGS_PARAM_COUNT] = {
    OSV_SS_ID_COLOR_OUTPUT,     OSV_SS_ID_REC709_LOOK,   OSV_SS_ID_OUTPUT_SIZE,   OSV_SS_ID_STABILIZATION,
    OSV_SS_ID_STITCH_TOPIC,     OSV_SS_ID_SEAM_SEARCH,   OSV_SS_ID_GAIN_MATCH,
    OSV_SS_ID_CALIBRATION,      OSV_SS_ID_FLARE_REMOVAL, OSV_SS_ID_PHOTO_SEAM, OSV_SS_ID_PHOTO_STRENGTH,
    OSV_SS_ID_SEAM_INSET,       OSV_SS_ID_SEAM_BLEND,    OSV_SS_ID_PARALLAX_BLEND, OSV_SS_ID_SEAM_SMOOTHING,
    OSV_SS_ID_NEAR_OFFSET,      OSV_SS_ID_FAR_OFFSET,
    OSV_SS_ID_LENS_SHADING,     OSV_SS_ID_SHADING_STRENGTH,  // [WP-VIGNETTE]
    OSV_SS_ID_STITCH_TOPIC_END, OSV_SS_ID_ADVANCED_TOPIC,
    OSV_SS_ID_DLOGM_FIT,        OSV_SS_ID_EXPOSURE,      OSV_SS_ID_RENDER_DEVICE,
    OSV_SS_ID_DIRECT_COLOUR,    OSV_SS_ID_ADVANCED_TOPIC_END,
    // [WP-DEFAULTS]
    OSV_SS_ID_DEFAULTS_TOPIC,   OSV_SS_ID_SAVE_DEFAULTS, OSV_SS_ID_RESTORE_DEFAULTS, OSV_SS_ID_DEFAULTS_TOPIC_END,
};

/// Number of user-visible parameters (excludes the input layer).
inline constexpr int kParamCount = OSV_SOURCE_SETTINGS_PARAM_COUNT;

/// Number of controls that actually carry a value, i.e. everything except the
/// GROUP_START / GROUP_END markers and [WP-DEFAULTS] the two Defaults buttons
/// (a button has no value; it only triggers PF_Cmd_USER_CHANGED_PARAM).
/// This is the count that has to round trip through a PrefsBlob
/// ([WP-SEAMTOOLS] five more since the seam tools, [WP-VIGNETTE] two more
/// since the lens shading correction).
inline constexpr int kValueParamCount = 22;

/// The parameter names, in index order, so a test can compare the built
/// module's list without repeating the strings.
///
/// The two GROUP_END entries are deliberately EMPTY.  PF_ADD_TOPIC takes a
/// name and sets it; PF_END_TOPIC takes only an id (Param_Utils.h:309-316)
/// and leaves the name field zeroed, because a group terminator is a divider
/// rather than a labelled control.  Writing "Stitching" here would have
/// described a field the SDK never fills.
inline constexpr const char* kParamNameByIndex[OSV_SOURCE_SETTINGS_PARAM_COUNT] = {
    "Colour Output", "Look (Rec. 709 only)", "Output Size", "Stabilisation", "Stitching", "Seam Search",
    "Exposure Match", "Calibration",  "Sun Ghost Removal",  "Sky Seam Fix", "Sky Seam Strength",
    "Seam Edge Inset", "Seam Blend",  "Parallax Blend", "Seam Smoothing", "Near Offset", "Far Offset",
    "Lens Shading",   "Shading Strength",  // [WP-VIGNETTE]
    "",               "Advanced",     "D-Log M Curve",
    "Exposure",       "Render Device", "Program Monitor Colour", "",
    // [WP-DEFAULTS]
    OSV_SS_DEFAULTS_TOPIC_NAME, OSV_SS_SAVE_DEFAULTS_NAME, OSV_SS_RESTORE_DEFAULTS_NAME, "",
};

}  // namespace osv::premiere::sourcesettings

#endif /* OSV_SOURCE_SETTINGS_CPLUSPLUS */

#endif /* OSV_SOURCE_SETTINGS_PARAMS_H */
