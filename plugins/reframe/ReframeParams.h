// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeParams.h - the ONE description of "Open 360 Reframe".
//
// Identity, out-flags, parameter ids, ranges, defaults, popup item strings
// and the preset table all live here, and this header is included by THREE
// very different consumers:
//
//   1. EffectMain.cpp   - the After Effects API side (PARAMS_SETUP, RENDER,
//                         USER_CHANGED_PARAM);
//   2. GpuFilter.cpp    - the PrGPUFilter side (match name, parameter
//                         indices, the same geometry maths);
//   3. Open360Reframe.r - the PiPL resource, preprocessed by cl /EP and fed
//                         to PiPLtool.exe.
//
// Consumer 3 is the reason for the guards below: the .r file is parsed by a
// Rez-style tool that understands nothing but its own resource grammar, so
// every C++ construct in this header (namespaces, enums, structs, constexpr)
// is hidden behind OSV_REFRAME_CPLUSPLUS.  What the .r sees is a handful of
// object-like #defines - the display name, the match name, the category, the
// version words and the two out-flag words - which it pastes straight into
// the resource.  A flag can therefore never drift between the PiPL Premiere
// reads at load time and what PF_Cmd_GLOBAL_SETUP reports at run time, which
// is a documented way for an AE effect to be rejected.
//
// Nothing here includes an Adobe header.  The AE flag constants are spelled
// out as literals with the AE_Effect.h expression that produces them in the
// comment, and EffectMain.cpp static_asserts each literal against the real
// macro, so a future SDK that renumbers a bit breaks the build instead of
// shipping a silently wrong PiPL.
#ifndef OSV_REFRAME_PARAMS_H
#define OSV_REFRAME_PARAMS_H

/* --------------------------------------------------------------------------
 *  Dialect detection.  cl /EP on the .r file runs with /TC (C, not C++), so
 *  __cplusplus is absent there and every C++ construct below is skipped.
 * -------------------------------------------------------------------------- */
#if defined(__cplusplus)
#define OSV_REFRAME_CPLUSPLUS 1
#endif

/* ==========================================================================
 *  Identity (shared with the PiPL)
 * ========================================================================== */

/* Displayed in the Effects panel.  Localisable in principle; the match name
 * below is the one that must never change. */
#define OSV_REFRAME_DISPLAY_NAME "Open 360 Reframe"

/* Bin in the Effects panel. */
#define OSV_REFRAME_CATEGORY "OpenOSV"

/* The permanent identity of this effect.  Premiere stores it in project
 * files and the plug-in cache - registered, like every AE-API effect, with an
 * "AE." prefix ("AE.OpenOSV.Open360Reframe").  The GPU entry point binds to
 * it by leaving PrGPUFilterInfo::outMatchName NULL (see GpuFilter.cpp for why
 * repeating this bare string there broke the binding).
 * NEVER change this string. */
#define OSV_REFRAME_MATCH_NAME "OpenOSV.Open360Reframe"

/* Effect version, as the three numbers a human reads. */
#define OSV_REFRAME_VERSION_MAJOR 1
#define OSV_REFRAME_VERSION_MINOR 0
#define OSV_REFRAME_VERSION_BUG 0

/* PF_VERSION(MAJOR, MINOR, BUG, STAGE, BUILD) packed by hand, because the
 * .r file cannot call the AE macro (AE_Effect.h is C++-hostile in Rez).
 * Layout from AE_Effect.h:
 *     ((MAJOR << 19) | (MINOR << 15) | (BUG << 11) | (STAGE << 9) | BUILD)
 * Stage 3 = PF_Stage_RELEASE, build 0, so 1.0.0 release packs to
 *     (1 << 19) | (3 << 9) = 524288 + 1536 = 525824 = 0x00080600.
 *
 * It has to be written as a bare DECIMAL LITERAL, not as the shift
 * expression: PiPLtool.exe's own parser evaluates the resource text and
 * fails with "PIPL_GetExpression: Matching parantheses expected!" on
 * anything more structured than a number.  EffectMain.cpp static_asserts
 * this literal against PF_VERSION() of the same numbers, so the two can
 * never disagree silently. */
#define OSV_REFRAME_STAGE 3
#define OSV_REFRAME_BUILD 0
#define OSV_REFRAME_PIPL_VERSION 525824

/* ==========================================================================
 *  Global out-flags (PiPL <-> PF_Cmd_GLOBAL_SETUP)
 *
 *  out_flags:
 *    PF_OutFlag_DEEP_COLOR_AWARE       1L << 25 = 0x02000000
 *        we handle 16-bit-per-channel and deeper worlds ourselves.
 *    PF_OutFlag_SEND_UPDATE_PARAMS_UI  1L << 26 = 0x04000000
 *        required to receive PF_Cmd_UPDATE_PARAMS_UI.
 *    PF_OutFlag_CUSTOM_UI              1L << 15 = 0x00008000
 *        we draw and handle mouse events in the Program Monitor - the
 *        interactive reframe overlay (plugins/reframe/ReframeUi*).  Without
 *        this flag PF_Cmd_EVENT is never sent at all, and the
 *        PF_CustomUIInfo that PF_Cmd_PARAMS_SETUP registers is ignored.
 *
 *  Deliberately NOT set alongside it:
 *    PF_OutFlag_FORCE_RERENDER - the overlay commits values with
 *        PF_ChangeFlag_CHANGED_VALUE, and AE_Effect.h:752 states that flag
 *        "automatically causes a re-render", so forcing one as well would
 *        only add the cache invalidation that the same paragraph warns
 *        interacts badly with undo.
 *
 *  Deliberately NOT set:
 *    PF_OutFlag_PIX_INDEPENDENT - every output pixel reads a DIFFERENT part
 *        of the sphere, so the host must never split our render by pixel
 *        assuming independence of the *input* sampling pattern.
 *    PF_OutFlag_WIDE_TIME_INPUT - "Smooth Keyframes" samples parameters (not
 *        frames) at neighbouring times, so no extra input frames are needed.
 *    PF_OutFlag_I_USE_AUDIO - no audio.
 *
 *  out_flags2:
 *    PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG 1L << 3  = 0x00000008
 *        honour PF_ParamFlag_START_COLLAPSED on our two topics.
 *    PF_OutFlag2_REVEALS_ZERO_ALPHA               1L << 7  = 0x00000080
 *        the letterbox is genuinely transparent black; the host must not
 *        assume zero alpha means "nothing here".
 *    PF_OutFlag2_FLOAT_COLOR_AWARE                1L << 12 = 0x00001000
 *        32-bit float worlds are handled natively.
 *    PF_OutFlag2_SUPPORTS_THREADED_RENDERING      1L << 27 = 0x08000000
 *        PF_Cmd_RENDER is re-entrant: sequence_data is null and every piece
 *        of per-render state is a local.
 * ========================================================================== */
#define OSV_REFRAME_OUT_FLAGS 0x06008000
#define OSV_REFRAME_OUT_FLAGS_2 0x08001088

/* AE_Effect_Info_Flags: none. */
#define OSV_REFRAME_INFO_FLAGS 0

/* AE_Reserved_Info: 8 in every AE-kind PiPL Adobe ships. */
#define OSV_REFRAME_RESERVED_INFO 8

/* ==========================================================================
 *  Parameter identifiers
 *
 *  These are the PERMANENT ids stored in project files (def.uu.id).
 *  Removing or renumbering an entry breaks every saved project, so new
 *  controls are appended with a new id and nothing here is ever reused.
 *
 *  An id is NOT a parameter index.  A parameter group occupies TWO slots in
 *  the host's parameter array - PF_Param_GROUP_START from PF_ADD_TOPIC and
 *  PF_Param_GROUP_END from PF_END_TOPIC, each of which issues its own
 *  PF_ADD_PARAM (Param_Utils.h:298-320) - and the GROUP_END slot sits in the
 *  MIDDLE of the list, not after it.  Adobe's own Transformer sample counts
 *  its XFORM_GROUP_END in XFORM_NUM_PARAMS for exactly this reason
 *  (Transformer.h:64-73).  The index table further down is therefore built
 *  by hand from the ADD ORDER in paramsSetup(); the two are kept honest by
 *  static_asserts in EffectMain.cpp and by a test that walks the real
 *  parameter list the module produced.
 * ========================================================================== */
/* Id 1 was "Output Aspect", a popup of aspect RATIOS that letterboxed the
 * picture inside the frame.  It is now "Output Resolution", a popup of pixel
 * SIZES that always fills the frame.
 *
 * The id is REUSED deliberately, which is the one place this header bends its
 * own rule, so the reasoning is worth stating.  Reuse is safe here because
 * both incarnations are a 1-based popup read through `u.pd.value`, so a saved
 * project can only ever yield a small integer - never a type confusion - and
 * sanitiseResolution() maps every value, in range or not, onto a valid entry.
 * The worst case for a project saved against the old table is that a clip
 * comes back on a different entry of the new one; it cannot crash, cannot
 * read a neighbouring control, and the user fixes it with one click.
 *
 * Appending a NEW id and retiring this one was the alternative, and it is
 * worse: the retired popup would still occupy a parameter slot and still be
 * drawn in the Effect Controls panel (AE has no way to remove a parameter
 * from an existing effect), so every user would see two output controls, one
 * of which does nothing.  A dead control that looks live is a worse bug than
 * a popup that needs re-picking once. */
#define OSV_REFRAME_ID_OUTPUT_RESOLUTION 1
#define OSV_REFRAME_ID_CAMERA_TOPIC 2
#define OSV_REFRAME_ID_PRESET 3
#define OSV_REFRAME_ID_PAN 4
#define OSV_REFRAME_ID_TILT 5
#define OSV_REFRAME_ID_ROLL 6
#define OSV_REFRAME_ID_FOV 7
#define OSV_REFRAME_ID_DISTORTION 8
#define OSV_REFRAME_ID_SOURCE_TOPIC 9
#define OSV_REFRAME_ID_SOURCE_PAN 10
#define OSV_REFRAME_ID_SOURCE_TILT 11
#define OSV_REFRAME_ID_SOURCE_ROLL 12
#define OSV_REFRAME_ID_SMOOTH 13
/* The two group terminators.  They carry no value and are never read, but
 * they are real parameters with real ids and real indices. */
#define OSV_REFRAME_ID_CAMERA_TOPIC_END 14
#define OSV_REFRAME_ID_SOURCE_TOPIC_END 15

/* ---- [WP-CAMERA] DJI's camera, appended ------------------------------------
 * Five controls added after Smooth Keyframes, with NEW ids and at the END of
 * the list, so every index before them - and every saved project - is
 * untouched.  An old project simply loads them at their defaults, and the
 * default Camera Model is "Classic", which renders exactly what the project
 * rendered before (see CameraModel below for the whole compatibility story).
 *
 *   16  Camera Model      checkbox "DJI": which lens model renders.
 *   17  Zoom              DJI's visible-angle read-out; editing it moves FOV
 *                         and Correction Angle along DJI's own zoom path.
 *   18  DJI FOV           DJI's vertical pinhole field of view.
 *   19  Correction Angle  DJI's eye distance behind the sphere's centre.
 *   20  Drag Sensitivity  how much faster than the hand an overlay drag turns
 *                         the view (was the constant kPanTiltSensitivity). */
#define OSV_REFRAME_ID_CAMERA_MODEL 16
#define OSV_REFRAME_ID_ZOOM 17
#define OSV_REFRAME_ID_DJI_FOV 18
#define OSV_REFRAME_ID_CORRECTION 19
#define OSV_REFRAME_ID_DRAG_SENSITIVITY 20
/* ---- end [WP-CAMERA] ------------------------------------------------------ */

/* ---- [WP-LENSUI] the Lens popup, appended ----------------------------------
 *
 *   21  Lens   popup "DJI|Classic", DJI by default: which lens renders, and
 *              which lens's controls the Effect Controls panel shows.
 *
 * It replaces the Camera Model checkbox (id 16) as the source of truth.  The
 * checkbox is NOT removed - a parameter that disappears breaks every project
 * saved with it - but registered invisible (PF_PUI_INVISIBLE) and kept in
 * step with the popup as a mirror: it holds the lens that was on screen
 * before an edit (how USER_CHANGED_PARAM tells a real switch from a re-pick)
 * and it lets a project saved by this build open on the right lens in the
 * build before it, which reads only the checkbox.
 *
 * WHY APPENDED AND NOT AT THE TOP OF THE CAMERA GROUP
 * After Effects matches saved values to parameters by these ids, so it
 * would allow an insertion anywhere (the AE SDK's "Changing Parameter
 * Orders" chapter).  Nothing in the Premiere Pro SDK guide says Premiere
 * does the same, and this effect is a Premiere effect: an insertion that
 * Premiere resolved by INDEX would load every saved project's Pan into the
 * popup and shift every control after it by one.  Appending is the one
 * placement that is safe whichever way the host binds, so the popup sits at
 * the end of the list, below Drag Sensitivity. */
#define OSV_REFRAME_ID_LENS 21
/* ---- end [WP-LENSUI] ------------------------------------------------------ */

/* ---- [WP-EASING] Keyframe Easing, appended ----------------------------------
 *
 *   22  Keyframe Easing   popup "None|Linear Smooth|Fast In, Slow Out|Slow In,
 *                         Fast Out|Fast In, Fast Out|Slow In, Slow Out|Linear",
 *                         None by default: the motion curve the effect applies
 *                         between consecutive keyframes of the camera controls
 *                         (Pan, Tilt, Roll and the selected lens's pair).
 *
 * The seven entries are DJI Studio's Keyframe Animation presets, in DJI
 * Studio's order (ReframeEasing.h has the curves and which of them are
 * DJI's exact numbers).  "None" leaves Premiere's own interpolation alone -
 * the effect then reads every control exactly as it did before this popup
 * existed, so an old project, which loads the popup at its default, renders
 * bit for bit what it rendered before.
 *
 * Appended after the Lens popup for the reason the Lens popup itself was
 * appended (see OSV_REFRAME_ID_LENS): only an append leaves every saved
 * index where it was, however the host binds saved values. */
#define OSV_REFRAME_ID_KEYFRAME_EASING 22
/* ---- end [WP-EASING] ------------------------------------------------------ */

/* Total parameters excluding the input layer: the 20 controls plus the two
 * group terminators.  out_data->num_params is this + 1. */
#define OSV_REFRAME_PARAM_COUNT 22

/* ==========================================================================
 *  Popup item strings
 *
 *  AE popup items are one string with '|' between entries.  Both lists are
 *  also mirrored as C++ arrays below so the tests can compare them item by
 *  item without re-parsing the separator.
 * ========================================================================== */
#define OSV_REFRAME_RESOLUTION_ITEMS "Match Sequence|3840 x 2160|2560 x 1440|1920 x 1080|1280 x 720"
#define OSV_REFRAME_RESOLUTION_COUNT 5

#define OSV_REFRAME_PRESET_ITEMS "Custom|Crystal Ball|Asteroid|Wide|Ultra Wide|Dewarping"
#define OSV_REFRAME_PRESET_COUNT 6

/* [WP-LENSUI] The Lens popup.  DJI first, so DJI - the default - is entry 1
 * in After Effects' numbering and entry 0 in Premiere's GPU numbering.
 *
 * WP-CAMERA made the lens a CHECKBOX because Premiere's GPU parameter reads
 * number popups from 0 (the 26.2.2 dump reads Output Resolution 0 and Preset
 * 3 at their 1-based defaults 1 and 4) while After Effects and the CPU path
 * number them from 1, so a raw "1" is DJI on one host and Classic on the
 * other.  A popup is safe now for three reasons, each checked by a test:
 *
 *   1. decodeHostPopup() learns the host's base per instance from any popup
 *      that reads unambiguously - a 0 anywhere, or an entry count;
 *   2. DJI reads 0 on a 0-based host, which settles the base by itself, so
 *      the default lens can never be misread;
 *   3. every edit the effect supervises leaves Classic paired with Preset
 *      "Custom" (picking a preset selects DJI, and every lens switch, every
 *      pick of Classic and every Classic edit sets Custom), and Custom reads
 *      0 on a 0-based host
 *      - so the one ambiguous reading, Classic's "1" on Premiere, arrives
 *      with the 0 that decodes it. */
#define OSV_REFRAME_LENS_ITEMS "DJI|Classic"
#define OSV_REFRAME_LENS_COUNT 2

/* [WP-EASING] The Keyframe Easing popup: DJI Studio's seven Keyframe
 * Animation presets, in the order DJI Studio's grid lists them.  "None" is
 * first, so the default is entry 1 in After Effects' numbering and entry 0
 * in Premiere's GPU numbering - a reading that settles the host's popup base
 * by itself (decodeHostPopup), exactly like the Lens popup's DJI default. */
#define OSV_REFRAME_EASING_ITEMS \
    "None|Linear Smooth|Fast In, Slow Out|Slow In, Fast Out|Fast In, Fast Out|Slow In, Slow Out|Linear"
#define OSV_REFRAME_EASING_COUNT 7

/* ==========================================================================
 *  Ranges and defaults (the numbers PF_ADD_* is called with)
 * ========================================================================== */
/* Output Resolution: 1-based popup value; 1 = "Match Sequence". */
#define OSV_REFRAME_RESOLUTION_DEFAULT 1

/* Preset: 1-based popup value; 4 = "Wide" (Custom is 1). */
#define OSV_REFRAME_PRESET_DEFAULT 4

/* Angles are unbounded AE angle dials in degrees; tilt is clamped to +-90 in
 * the geometry, not in the control, so a user can keyframe through the pole
 * without the dial fighting them. */
#define OSV_REFRAME_PAN_DEFAULT 0.0
#define OSV_REFRAME_TILT_DEFAULT 0.0
#define OSV_REFRAME_ROLL_DEFAULT 0.0
#define OSV_REFRAME_TILT_LIMIT_DEG 90.0

/* FOV: the eye-offset model stays invertible up to 2*acos(-d); 350 is the
 * widest the slider will ever accept and the geometry clamps per distortion. */
#define OSV_REFRAME_FOV_VALID_MIN 10.0
#define OSV_REFRAME_FOV_VALID_MAX 350.0
#define OSV_REFRAME_FOV_SLIDER_MIN 30.0
#define OSV_REFRAME_FOV_SLIDER_MAX 180.0
#define OSV_REFRAME_FOV_DEFAULT 120.0

/* Distortion: the eye offset d as a percentage (0 = rectilinear, 100 =
 * stereographic). */
#define OSV_REFRAME_DISTORTION_VALID_MIN 0.0
#define OSV_REFRAME_DISTORTION_VALID_MAX 100.0
#define OSV_REFRAME_DISTORTION_SLIDER_MIN 0.0
#define OSV_REFRAME_DISTORTION_SLIDER_MAX 100.0
#define OSV_REFRAME_DISTORTION_DEFAULT 15.0

/* Smooth Keyframes off by default: it costs two extra parameter samples per
 * frame and changes the look, so it is opt-in. */
#define OSV_REFRAME_SMOOTH_DEFAULT 0

/* ---- [WP-CAMERA] DJI camera ranges -----------------------------------------
 * Every number below is DJI's own (docs/research/DJI_CAMERA.md has the
 * addresses).  Two DJI tools disagree on the limits, so the VALID range is
 * the wider one (DJI's Premiere plug-in: FOV 1..178, Correction 0..1.8) and
 * the SLIDER covers DJI Studio's working range, so a project typed in from
 * either tool is representable and the slider feels like DJI Studio's. */

/* [WP-LENSUI] Camera Model, now the Lens popup's hidden mirror: ticked (DJI)
 * by default, so a fresh instance's mirror agrees with the popup's default.
 *
 * It was unticked (Classic) while it was the source of truth, so that old
 * projects kept the Classic lens.  The user chose DJI as the default for
 * every project instead - one without the Lens popup loads it at DJI - and a
 * WP-CAMERA project whose checkbox the host restores as the new default is
 * then consistent with that choice too.  Nothing renders from this value any
 * more (see OSV_REFRAME_ID_LENS). */
#define OSV_REFRAME_CAMERA_MODEL_DEFAULT 1

/* [WP-LENSUI] Lens: 1-based popup value; 1 = "DJI", the user's default. */
#define OSV_REFRAME_LENS_DEFAULT 1

/* [WP-EASING] Keyframe Easing: 1-based popup value; 1 = "None" (Premiere's
 * own interpolation), so nothing changes until the user picks a preset. */
#define OSV_REFRAME_EASING_DEFAULT 1

/* DJI FOV: the vertical pinhole field of view.  DJI Studio clamps it to
 * [20, 150] (generateNewParams / setFov); DJI's plug-in allows [1, 178]. */
#define OSV_REFRAME_DJI_FOV_VALID_MIN 1.0
#define OSV_REFRAME_DJI_FOV_VALID_MAX 178.0
#define OSV_REFRAME_DJI_FOV_SLIDER_MIN 20.0
#define OSV_REFRAME_DJI_FOV_SLIDER_MAX 150.0
/* DJI's "Wide" on a landscape frame, which is also where both DJI tools
 * start a fresh clip (Studio: fov 60 / distortion 0.6; plug-in: 60.01 /
 * 0.601 sentinels that resolve to Wide). */
#define OSV_REFRAME_DJI_FOV_DEFAULT 60.0

/* Correction Angle: the eye's distance behind the sphere's centre in sphere
 * radii.  Studio clamps it to [0, 1]; the plug-in allows up to 1.8, which is
 * exactly its Crystal Ball preset (the eye outside the sphere). */
#define OSV_REFRAME_CORRECTION_VALID_MIN 0.0
#define OSV_REFRAME_CORRECTION_VALID_MAX 1.8
#define OSV_REFRAME_CORRECTION_SLIDER_MIN 0.0
#define OSV_REFRAME_CORRECTION_SLIDER_MAX 1.8
#define OSV_REFRAME_CORRECTION_DEFAULT 0.6

/* Zoom: DJI's derived visible horizontal angle.  It can legitimately exceed
 * 180 (the frame's edges look behind the camera) and approaches 360 for the
 * Asteroid look, so the valid range is the whole circle.  The default is the
 * value of the default DJI FOV / Correction on a 16:9 frame, rounded to the
 * slider's tenths (142.397 -> 142.4). */
#define OSV_REFRAME_ZOOM_VALID_MIN 0.0
#define OSV_REFRAME_ZOOM_VALID_MAX 360.0
#define OSV_REFRAME_ZOOM_SLIDER_MIN 30.0
#define OSV_REFRAME_ZOOM_SLIDER_MAX 330.0
#define OSV_REFRAME_ZOOM_DEFAULT 142.4

/* DJI Studio's zoom gesture moves
 * both lens controls together: fov += 130 * delta, correction += delta.  The
 * Zoom control and the overlay's zoom drag follow the same path. */
#define OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION 130.0
/* The limits DJI Studio clamps that path to (setFov / setDistortion). */
#define OSV_REFRAME_DJI_STUDIO_FOV_MIN 20.0
#define OSV_REFRAME_DJI_STUDIO_FOV_MAX 150.0
#define OSV_REFRAME_DJI_STUDIO_CORRECTION_MAX 1.0

/* Drag Sensitivity: how many times faster than the hand an overlay pan /
 * tilt drag turns the view.  2.0 is the value the constant used to have. */
#define OSV_REFRAME_DRAG_SENSITIVITY_VALID_MIN 0.1
#define OSV_REFRAME_DRAG_SENSITIVITY_VALID_MAX 10.0
#define OSV_REFRAME_DRAG_SENSITIVITY_SLIDER_MIN 0.25
#define OSV_REFRAME_DRAG_SENSITIVITY_SLIDER_MAX 5.0
#define OSV_REFRAME_DRAG_SENSITIVITY_DEFAULT 2.0
/* ---- end [WP-CAMERA] ------------------------------------------------------ */

/* ==========================================================================
 *  Everything below is C++ only.
 * ========================================================================== */
#if defined(OSV_REFRAME_CPLUSPLUS)

#include <cstddef>
#include <cstdint>

namespace osv::reframe {

/// AE parameter index of a control.  Index 0 is the input layer, which AE
/// inserts itself, so the first control we add is index 1.
///
/// This table is the ADD ORDER in paramsSetup(), which is NOT the id order:
/// the two PF_Param_GROUP_END markers sit in the middle of the list, after
/// the last control of their group, so every control that follows a closed
/// group is shifted up by one per group already closed.  Writing the numbers
/// out literally is deliberate - deriving them from the ids is exactly the
/// mistake that made the groups unbalanced in the first place.
///
///   1  Output Resolution
///   2  Camera            (GROUP_START)
///   3    Preset
///   4    Pan
///   5    Tilt
///   6    Roll
///   7    FOV
///   8    Distortion
///   9  (GROUP_END, Camera)
///  10  Source            (GROUP_START)
///  11    Source Pan
///  12    Source Tilt
///  13    Source Roll
///  14  (GROUP_END, Source)
///  15  Smooth Keyframes
///  16  Camera Model          [WP-CAMERA]  (hidden since [WP-LENSUI])
///  17  Zoom                  [WP-CAMERA]
///  18  DJI FOV               [WP-CAMERA]
///  19  Correction Angle      [WP-CAMERA]
///  20  Drag Sensitivity      [WP-CAMERA]
///  21  Lens                  [WP-LENSUI]
///  22  Keyframe Easing       [WP-EASING]
///
/// 16..22 are siblings of Smooth Keyframes, outside both groups, and
/// deliberately so: appending is the only change that leaves the index of
/// every existing control - and so every saved project and every host index
/// map - exactly where it was.
enum ParamIndex : int {
    kIndexOutputResolution = 1,
    kIndexCameraTopic = 2,
    kIndexPreset = 3,
    kIndexPan = 4,
    kIndexTilt = 5,
    kIndexRoll = 6,
    kIndexFov = 7,
    kIndexDistortion = 8,
    kIndexCameraTopicEnd = 9,
    kIndexSourceTopic = 10,
    kIndexSourcePan = 11,
    kIndexSourceTilt = 12,
    kIndexSourceRoll = 13,
    kIndexSourceTopicEnd = 14,
    kIndexSmooth = 15,
    // [WP-CAMERA]
    kIndexCameraModel = 16,
    kIndexZoom = 17,
    kIndexDjiFov = 18,
    kIndexCorrection = 19,
    kIndexDragSensitivity = 20,
    // [WP-LENSUI]
    kIndexLens = 21,
    // [WP-EASING]
    kIndexKeyframeEasing = 22,
};

/// The permanent id stored with each index, in index order (index 1 first).
/// EffectMain.cpp walks this when it adds the parameters and a test walks it
/// against the list the built module actually produced, so the two can never
/// drift apart silently.
inline constexpr int kParamIdByIndex[OSV_REFRAME_PARAM_COUNT] = {
    OSV_REFRAME_ID_OUTPUT_RESOLUTION, OSV_REFRAME_ID_CAMERA_TOPIC,   OSV_REFRAME_ID_PRESET,
    OSV_REFRAME_ID_PAN,           OSV_REFRAME_ID_TILT,             OSV_REFRAME_ID_ROLL,
    OSV_REFRAME_ID_FOV,           OSV_REFRAME_ID_DISTORTION,       OSV_REFRAME_ID_CAMERA_TOPIC_END,
    OSV_REFRAME_ID_SOURCE_TOPIC,  OSV_REFRAME_ID_SOURCE_PAN,       OSV_REFRAME_ID_SOURCE_TILT,
    OSV_REFRAME_ID_SOURCE_ROLL,   OSV_REFRAME_ID_SOURCE_TOPIC_END, OSV_REFRAME_ID_SMOOTH,
    // [WP-CAMERA]
    OSV_REFRAME_ID_CAMERA_MODEL,  OSV_REFRAME_ID_ZOOM,             OSV_REFRAME_ID_DJI_FOV,
    OSV_REFRAME_ID_CORRECTION,    OSV_REFRAME_ID_DRAG_SENSITIVITY,
    // [WP-LENSUI]
    OSV_REFRAME_ID_LENS,
    // [WP-EASING]
    OSV_REFRAME_ID_KEYFRAME_EASING,
};

/// Number of user-visible parameters (excludes the input layer).
inline constexpr int kParamCount = OSV_REFRAME_PARAM_COUNT;

/// Index passed to PrSDKVideoSegmentSuite::GetParam, which does not count
/// the input layer (PrGPUFilterModule.h:153 "GPU filters do not include the
/// input frame").
///
/// This is the STATIC mapping, and it is only correct for a host whose
/// GetParam index space is the AE parameter list verbatim, group markers
/// included.  Real Premiere Pro 26.2 does not do that (see HostParamMap
/// below), so the GPU path probes at CreateInstance and uses this only as
/// the fallback.
[[nodiscard]] inline constexpr int gpuParamIndex(int aeIndex) noexcept { return aeIndex - 1; }

// ---------------------------------------------------------------------------
//  Host parameter index map
// ---------------------------------------------------------------------------

/// The PrParam variant a control arrives in, which is what makes a runtime
/// probe possible at all.  Mirrors the subset of PrParamType we care about,
/// spelled independently so this header still needs no Adobe include.
enum class HostParamKind : int {
    Unknown = 0,
    Int32,    ///< A popup (Output Resolution, Preset, Lens, Keyframe Easing): a small integer (see decodeHostPopup()).
    Float32,  ///< An AE angle dial (the six Pan / Tilt / Roll controls), in degrees.
    Float64,  ///< An AE float slider (FOV, Distortion, Zoom, DJI FOV, Correction Angle, Drag Sensitivity).
    Bool,     ///< A checkbox (Smooth Keyframes, Camera Model).
    /// A PF_Param_GROUP_START / GROUP_END marker.  It carries no value; a host
    /// that lists it at all reports it as a Bool or refuses to type it.  Only
    /// kParamKindByIndex below uses this - the value signature never does.
    Group,
};

/// Number of controls that actually carry a value, i.e. everything except
/// the four PF_Param_GROUP_START / GROUP_END markers.  This is the list a
/// probe expects to find on the host, in this order.
inline constexpr int kValueParamCount = 18;

/// [WP-CAMERA] How many of those the effect had before the DJI camera block
/// was appended.  A host list may stop after these (it then exposes none of
/// the appended controls), never before them: matchHostParams() only lets a
/// list end early inside the appended block.
inline constexpr int kOriginalValueParamCount = 11;

/// The AE indices of the eighteen value-carrying controls, in ADD ORDER.
/// The group markers are absent by construction: they hold no value, so no
/// host can report one for them and nothing ever reads them.
inline constexpr int kValueParamAeIndex[kValueParamCount] = {
    kIndexOutputResolution, kIndexPreset,     kIndexPan,        kIndexTilt,
    kIndexRoll,             kIndexFov,        kIndexDistortion, kIndexSourcePan,
    kIndexSourceTilt,       kIndexSourceRoll, kIndexSmooth,
    // [WP-CAMERA]
    kIndexCameraModel,      kIndexZoom,       kIndexDjiFov,     kIndexCorrection,
    kIndexDragSensitivity,
    // [WP-LENSUI]
    kIndexLens,
    // [WP-EASING]
    kIndexKeyframeEasing,
};

/// The PrParam kind each of those controls arrives in, in the same order.
///
/// Read down the column and this is the SIGNATURE the probe matches:
///
///     i32 i32 f32 f32 f32 f64 f64 f32 f32 f32 bool | bool f64 f64 f64 f64 i32 i32
///
/// The part before the bar is the effect's original list and is still
/// distinctive on its own - the adjacent FOV / Distortion Float64 pair
/// between the angle dials, and the Bool that closes it (Smooth Keyframes).
/// The part after it was appended later: the DJI camera block of
/// [WP-CAMERA] (the Camera Model checkbox and four float sliders), the Lens
/// popup of [WP-LENSUI] and the Keyframe Easing popup of [WP-EASING].
/// matchHostParams() accepts a host list that stops anywhere inside that
/// tail, so a host that has not (yet) exposed the appended controls still
/// maps the original ones - and a control it does not expose reads its
/// default.
inline constexpr HostParamKind kValueParamKind[kValueParamCount] = {
    HostParamKind::Int32,   HostParamKind::Int32,   HostParamKind::Float32, HostParamKind::Float32,
    HostParamKind::Float32, HostParamKind::Float64, HostParamKind::Float64, HostParamKind::Float32,
    HostParamKind::Float32, HostParamKind::Float32, HostParamKind::Bool,
    // [WP-CAMERA]
    HostParamKind::Bool,    HostParamKind::Float64, HostParamKind::Float64, HostParamKind::Float64,
    HostParamKind::Float64,
    // [WP-LENSUI]
    HostParamKind::Int32,
    // [WP-EASING]
    HostParamKind::Int32,
};

/// The kind of EVERY parameter in AE index order (index 1 first), group
/// markers included - the full list PF_Cmd_PARAMS_SETUP adds, for a probe
/// that sees the host list with its group entries still in it.  Derived
/// from the same facts as the two tables above; a static_assert in
/// EffectMain.cpp checks the three agree.
inline constexpr HostParamKind kParamKindByIndex[OSV_REFRAME_PARAM_COUNT] = {
    HostParamKind::Int32,   HostParamKind::Group,   HostParamKind::Int32,   HostParamKind::Float32,
    HostParamKind::Float32, HostParamKind::Float32, HostParamKind::Float64, HostParamKind::Float64,
    HostParamKind::Group,   HostParamKind::Group,   HostParamKind::Float32, HostParamKind::Float32,
    HostParamKind::Float32, HostParamKind::Group,   HostParamKind::Bool,
    // [WP-CAMERA]
    HostParamKind::Bool,    HostParamKind::Float64, HostParamKind::Float64, HostParamKind::Float64,
    HostParamKind::Float64,
    // [WP-LENSUI]
    HostParamKind::Int32,
    // [WP-EASING]
    HostParamKind::Int32,
};

/// [WP-CAMERA] How a host numbers popup entries on the GPU side.
///
/// After Effects - and therefore the CPU path, PF_ParamDef::u.pd.value -
/// numbers popup entries from 1.  Premiere Pro's Video Segment Suite was
/// seen to hand GPU filters the SAME controls numbered from 0: the 26.2.2
/// dump reads Output Resolution 0 and Preset 3 at their defaults 1 and 4,
/// and DJI's own Premiere plug-in adds 1 to every popup it reads there.  The
/// mock host, like After Effects, serves them from 1.  Which base a host uses
/// is learned from the values themselves (decodeHostPopup()).
enum class PopupBase : int {
    Unknown = -1,  ///< Nothing seen yet that settles it; read as 1-based.
    Zero = 0,      ///< A popup read 0: entries count from 0.
    One = 1,       ///< A popup read its own entry count: entries count from 1.
};

/// Translate a popup value read from a host into the 1-based value every
/// sanitiser in this header expects, learning the host's base as it goes.
///
/// `raw` is what the host returned, `entryCount` the popup's number of
/// entries, and `base` the instance's knowledge so far (updated in place).
/// Two readings are unambiguous and settle the base for the instance:
///
///   raw == 0            only a 0-based host can say that   -> Zero;
///   raw == entryCount   only a 1-based host can say that   -> One.
///
/// Anything in between reads as the base already learned, and as 1-based
/// while nothing is known - the After Effects convention and this effect's
/// historical behaviour.  Getting it wrong while unknown is cheap by design:
/// Output Resolution's fixed entries are all 16:9 (so a one-step misread
/// cannot change the framing of a fixed size) and its "Match Sequence"
/// default reads 0 on a 0-based host; [WP-LENSUI] the Lens popup's DJI
/// default reads 0 there too, and Classic always travels with Preset
/// "Custom", which reads 0 (see OSV_REFRAME_LENS_ITEMS) - so every reading
/// that could flip the lens arrives with the 0 that settles the base.  A
/// null `base` is treated as Unknown and not updated.
[[nodiscard]] inline int decodeHostPopup(int raw, int entryCount, PopupBase* base) noexcept {
    PopupBase known = base ? *base : PopupBase::Unknown;
    if (raw == 0) {
        known = PopupBase::Zero;
    } else if (entryCount > 0 && raw == entryCount) {
        known = PopupBase::One;
    }
    if (base) {
        *base = known;
    }
    return (known == PopupBase::Zero) ? raw + 1 : raw;
}

/// Runtime map from an AE parameter index to the index the host's
/// VideoSegmentSuite::GetParam wants.
///
/// WHY THIS EXISTS
/// ---------------
/// `gpuParamIndex()` above encodes the documented rule, "AE index minus one,
/// because GPU filters do not see the input layer" (PrGPUFilterModule.h:153).
/// That rule silently assumes the host's index space is the parameter list
/// After Effects built, group markers and all.  Premiere Pro 26.2 proves it
/// does not: this effect adds 15 parameters (11 controls + 4 group markers)
/// and the host's own GetParamCount answers 8.
///
/// With the static mapping every read past the first group therefore lands on
/// a neighbouring control: FOV came back as garbage, buildParams() rejected
/// it, and the render bailed - the whole effect did nothing.
///
/// Rather than replace one guess with another, this map is DISCOVERED.
/// `probe()` walks 0 .. count-1, asks the host for each entry's type, and
/// matches the resulting type sequence against kValueParamKind.  A match
/// gives an exact, host-verified mapping; anything ambiguous falls back to
/// the static rule and says so in the log.  Building the map is one GetParam
/// call per host index, done once per instance, and can never fail fatally -
/// the worst case is the behaviour we already had.
struct HostParamMap {
    /// Host index for each AE index; -1 means "no host entry for this one".
    /// Sized for the whole AE list so it can be subscripted with a kIndex*
    /// constant directly, including the group markers (which stay -1).
    int hostIndex[OSV_REFRAME_PARAM_COUNT + 1];

    /// True when probe() positively identified the controls; false when the
    /// table is the static fallback.  Only used for logging - a caller reads
    /// the table the same way either way.
    bool probed;

    /// Fill the table from the static "AE index - 1" rule.
    void setStatic() noexcept {
        for (int i = 0; i <= OSV_REFRAME_PARAM_COUNT; ++i) {
            hostIndex[i] = gpuParamIndex(i);
        }
        hostIndex[0] = -1;  // the input layer is never read through GetParam
        probed = false;
    }

    /// Host index for an AE index, or -1 when there is none.  Bounds-checked:
    /// a caller passing a nonsense index gets -1, not a wild read.
    [[nodiscard]] int operator[](int aeIndex) const noexcept {
        if (aeIndex < 0 || aeIndex > OSV_REFRAME_PARAM_COUNT) {
            return -1;
        }
        return hostIndex[aeIndex];
    }
};

/// Try to identify our controls inside a host parameter list.
///
/// `kinds` is what the host reported at indices 0 .. count-1 (entries the
/// host refused are HostParamKind::Unknown) and `count` how many there were.
/// On success `outMap` holds an AE-index -> host-index table and the function
/// returns true; on failure `outMap` is untouched and the caller keeps the
/// static mapping.
///
/// WHAT THE HOST ACTUALLY SHOWS
/// ----------------------------
/// Premiere does not hand a GPU filter the AE parameter list verbatim.  Two
/// separate reductions apply, and the observed count of 8 is exactly both of
/// them together:
///
///     15 AE parameters
///   -  4 group markers (2 x GROUP_START + 2 x GROUP_END carry no value)
///   =  11 value-carrying controls
///   -  3 controls inside the START_COLLAPSED "Source" group
///   =  8, which is what GetParamCount reports.
///
/// So the host list is our controls with a CONTIGUOUS RUN dropped out of the
/// middle.  That is the shape the matcher below is built for: it aligns our
/// signature against the host list allowing exactly one contiguous gap, which
/// covers the unreduced case (gap length zero) and the observed case (the
/// three collapsed Source controls) with the same code and no special-casing
/// of either number.
///
/// [WP-CAMERA] The list may also STOP EARLY: the DJI camera controls were
/// appended after Smooth Keyframes, and a host whose list ends before them
/// (the 8-entry layout above, recorded before they existed) is still our
/// list - it just exposes none of the new controls.  The matcher therefore
/// aligns against every PREFIX of the signature (one gap inside it), and the
/// uniqueness rule is applied to the resulting MAPPINGS, so two alignments
/// that describe the same table are one answer, not an ambiguity.
///
/// Three rules make the result safe rather than merely plausible:
///
///   1. the alignment must be UNIQUE.  If our signature can be placed inside
///      the host list in more than one way the answer is ambiguous and the
///      function refuses it, because picking either would be a guess.
///   2. Unknown entries never match anything, so a host that failed a read
///      narrows the candidate set instead of widening it.
///   3. every host entry must be consumed.  A host list with a leftover entry
///      we cannot account for is not our parameter list, and mapping onto it
///      would be reading someone else's control.
///
/// Controls that fall in the gap get hostIndex -1; the caller then uses their
/// documented default, which is exactly right - a control the host does not
/// expose is a control the user cannot have changed.
///
/// Implemented as a free function with no Adobe types in its signature so the
/// unit tests can exercise it directly with a synthetic host list.
[[nodiscard]] bool matchHostParams(const HostParamKind* kinds, int count, HostParamMap* outMap) noexcept;

// ---------------------------------------------------------------------------
//  Output resolution
//
//  WHY A RESOLUTION AND NOT AN ASPECT
//  ----------------------------------
//  This control used to pick an aspect RATIO, and buildParams() letterboxed a
//  box of that shape inside the output frame.  That was wrong twice over.
//
//  Visibly wrong: a reframe is a virtual camera pointed into a sphere, and a
//  virtual camera has no reason to leave black bars.  Whatever shape the
//  sequence is, the camera should simply be built with THAT shape's field of
//  view and fill it.  Letterboxing threw away real output pixels and gave the
//  user bars they then had to crop.
//
//  Structurally wrong: a ratio says nothing about the deliverable.  Users
//  think in deliverables - "this cut is 2560 x 1440" - and "Match Sequence"
//  can only be answered exactly with the sequence's real pixel size, which
//  the Sequence Info Suite reports and the old control threw away.
//
//  WHAT THE CONTROL DOES, AND WHAT IT CANNOT DO
//  --------------------------------------------
//  The named size decides the camera's FRAMING: the field of view spans the
//  width of that size, and the resulting image is cover-fitted onto the
//  output frame - scaled uniformly until it fills the frame, overflow cropped,
//  never letterboxed (buildParams() documents the maths).  When the named
//  shape matches the frame, which is always the case for "Match Sequence" and
//  for every preview-scaled frame, the cover-fit is the identity and FOV
//  spans the frame width exactly.
//
//  It does NOT change how many pixels this effect renders, and it cannot:
//  in Premiere the HOST allocates the output world (sequence size times the
//  playback resolution) before PF_Cmd_RENDER or the GPU Render call ever
//  runs.  The effect is already one kernel evaluation per output pixel, so it
//  already renders exactly the pixels the sequence needs.  The pixel count
//  that dominates frame time lives upstream, in how large a sphere the
//  importer stitches (ImporterVideo.cpp, nearestAdvertisedSize and
//  geometryForLocked).  This module links no decoder and no lens model, the
//  effect only ever sees the equirect frame Premiere hands it, and there is
//  no channel through which it could ask the importer for fewer pixels.  Do
//  not add a code path here that pretends otherwise.
// ---------------------------------------------------------------------------

/// Popup values of "Output Resolution" (1-based, matching AE popup semantics).
enum class Resolution : int {
    MatchSequence = 1,  ///< Ask the Sequence Info Suite; the frame itself when it cannot answer.
    Uhd3840x2160 = 2,
    Qhd2560x1440 = 3,
    Fhd1920x1080 = 4,
    Hd1280x720 = 5,
};

/// One entry of the resolution table.
struct ResolutionEntry {
    Resolution value;   ///< Popup value.
    const char* label;  ///< Exactly the text in OSV_REFRAME_RESOLUTION_ITEMS.
    int width;          ///< 0 for "Match Sequence", which is resolved at render time.
    int height;         ///< 0 for "Match Sequence".
};

/// The resolution table.  Order and labels must match
/// OSV_REFRAME_RESOLUTION_ITEMS.
inline constexpr ResolutionEntry kResolutions[OSV_REFRAME_RESOLUTION_COUNT] = {
    {Resolution::MatchSequence, "Match Sequence", 0, 0},
    {Resolution::Uhd3840x2160, "3840 x 2160", 3840, 2160},
    {Resolution::Qhd2560x1440, "2560 x 1440", 2560, 1440},
    {Resolution::Fhd1920x1080, "1920 x 1080", 1920, 1080},
    {Resolution::Hd1280x720, "1280 x 720", 1280, 720},
};

/// Clamp an arbitrary popup value (a corrupt project, or a project saved
/// against the old "Output Aspect" table, can hold anything) into the valid
/// range.  Out of range means "Match Sequence", which is both the default and
/// the entry that cannot be wrong for any sequence.
[[nodiscard]] inline constexpr Resolution sanitiseResolution(int popupValue) noexcept {
    if (popupValue < 1 || popupValue > OSV_REFRAME_RESOLUTION_COUNT) {
        return Resolution::MatchSequence;
    }
    return static_cast<Resolution>(popupValue);
}

/// A pixel size.  Used both for what the sequence reported and for what the
/// control resolved to.
struct SizePx {
    int w = 0;
    int h = 0;

    /// A size is usable only when BOTH edges are positive.  A half-filled
    /// rect from a host that answered partially is not a size.
    [[nodiscard]] constexpr bool valid() const noexcept { return w > 0 && h > 0; }
};

// ---------------------------------------------------------------------------
//  Presets
// ---------------------------------------------------------------------------

/// Popup values of "Preset" (1-based).
enum class Preset : int {
    Custom = 1,
    CrystalBall = 2,
    Asteroid = 3,
    Wide = 4,
    UltraWide = 5,
    Dewarping = 6,
};

/// One preset: the values the popup writes into the controls.
///
/// A preset carries BOTH lens descriptions of its look:
///
///   * the Classic numbers (fovDeg / distortion) mirror osv::geom::kPresets
///     (include/osv/geom/Presets.h) - the looks the CLI renderer offers -
///     with the distortion expressed as the percentage the slider shows;
///   * [WP-CAMERA] the DJI numbers mirror osv::geom::kDjiPresets - DJI's own
///     preset table, matching DJI's Premiere plug-in and DJI
///     Studio's list - with the field of view given per output shape.
///
/// Choosing a preset writes both sets and selects the DJI lens, so the
/// picture is DJI's preset and the Classic controls hold the nearest Classic
/// look should the user switch back.
struct PresetEntry {
    Preset value;       ///< Popup value.
    const char* label;  ///< Exactly the text in OSV_REFRAME_PRESET_ITEMS.
    double fovDeg;      ///< Written into FOV (Classic).
    double distortion;  ///< Written into Distortion (Classic, percent, = 100 * eye offset).
    double tiltDeg;     ///< Written into Tilt.
    bool writesControls;  ///< False for Custom, which only reflects manual edits.
    // [WP-CAMERA] DJI's numbers for the same look.
    double djiFovLandscapeDeg;   ///< DJI FOV on a landscape or square frame.
    double djiFovPortrait916Deg; ///< DJI FOV on a 9:16 frame.
    double djiFovPortrait34Deg;  ///< DJI FOV on a 3:4 frame.
    double correction;           ///< Written into Correction Angle.
};

/// The preset table.  Order and labels must match OSV_REFRAME_PRESET_ITEMS.
inline constexpr PresetEntry kPresetTable[OSV_REFRAME_PRESET_COUNT] = {
    {Preset::Custom, "Custom", OSV_REFRAME_FOV_DEFAULT, OSV_REFRAME_DISTORTION_DEFAULT, 0.0, false,
     OSV_REFRAME_DJI_FOV_DEFAULT, OSV_REFRAME_DJI_FOV_DEFAULT, OSV_REFRAME_DJI_FOV_DEFAULT,
     OSV_REFRAME_CORRECTION_DEFAULT},
    {Preset::CrystalBall, "Crystal Ball", 240.0, 100.0, 0.0, true, 75.0, 110.0, 87.0, 1.8},
    {Preset::Asteroid, "Asteroid", 300.0, 100.0, -90.0, true, 138.0, 147.0, 147.0, 1.0},
    {Preset::Wide, "Wide", 120.0, 15.0, 0.0, true, 60.0, 90.0, 72.0, 0.6},
    {Preset::UltraWide, "Ultra Wide", 150.0, 40.0, 0.0, true, 78.0, 110.0, 95.0, 0.5},
    {Preset::Dewarping, "Dewarping", 95.0, 0.0, 0.0, true, 80.0, 112.0, 97.0, 0.2},
};

/// Clamp an arbitrary popup value into the valid range.
[[nodiscard]] inline constexpr Preset sanitisePreset(int popupValue) noexcept {
    if (popupValue < 1 || popupValue > OSV_REFRAME_PRESET_COUNT) {
        return Preset::Custom;
    }
    return static_cast<Preset>(popupValue);
}

/// The table entry for a popup value (never null: an out-of-range value maps
/// to Custom).
[[nodiscard]] inline constexpr const PresetEntry* presetEntry(Preset value) noexcept {
    for (const PresetEntry& e : kPresetTable) {
        if (e.value == value) {
            return &e;
        }
    }
    return &kPresetTable[0];
}

// ---------------------------------------------------------------------------
//  [WP-CAMERA] The camera model
//
//  WHY TWO MODELS, AND WHY "CLASSIC" IS THE DEFAULT
//  ------------------------------------------------
//  The effect's original lens is the eye-offset projection driven by FOV
//  (the visible horizontal angle) and Distortion (the eye offset, with an
//  automatic ramp by FOV).  DJI's tools describe the SAME family of cameras -
//  a pinhole looking at the panorama sphere from behind its centre - but by
//  different numbers: a vertical pinhole field of view and the eye distance,
//  with the visible angle ("Zoom") derived.  Typing DJI's numbers into the
//  Classic controls therefore gave a different picture, which is the bug the
//  DJI model fixes: in DJI mode the same three numbers give the same frame.
//
//  The switch is an explicit control rather than an inference from which
//  controls look "set", because nothing can tell an old project from a new
//  instance: both load every appended control at its default.
//
//  [WP-LENSUI] That control is now the "Lens" popup (DJI | Classic), and its
//  default is DJI - the user's choice, accepted with its consequence that a
//  project saved before the popup existed opens on the DJI lens.  The
//  Effect Controls panel shows only the selected lens's controls (the
//  visibility table below), so the two FOVs never appear side by side.
//  Picking a preset still selects DJI, and editing a control of the other
//  lens - possible only through a host that shows every control - still
//  selects that lens; every switch carries the current look across, so the
//  picture does not jump.  The WP-CAMERA "Camera Model" checkbox stays in the
//  list, hidden, as the popup's mirror (see OSV_REFRAME_ID_LENS).
// ---------------------------------------------------------------------------

/// The two lens models, as the renderer sees them.
///
/// Settings default-constructs to Classic so a Settings block built by hand
/// describes the Classic camera bit for bit; the READERS of the host's
/// parameters apply the Lens popup's own default (DJI) when a host has no
/// value for it.
enum class CameraModel : int {
    Classic = 0,  ///< FOV (visible angle) + Distortion (eye offset, auto ramp).
    Dji = 1,      ///< DJI FOV (vertical pinhole) + Correction Angle; Zoom derived.
};

/// The model a checkbox value selects.  Any non-zero value is ticked, the
/// same rule the host's own checkbox uses; only exactly 0 is Classic.  Used
/// for the hidden Camera Model mirror, which a host that shows every control
/// still lets the user tick.
[[nodiscard]] inline constexpr CameraModel cameraModelFromCheckbox(long checkboxValue) noexcept {
    return (checkboxValue != 0) ? CameraModel::Dji : CameraModel::Classic;
}

// ---------------------------------------------------------------------------
//  [WP-LENSUI] The Lens popup
// ---------------------------------------------------------------------------

/// Popup values of "Lens" (1-based, After Effects numbering).
enum class LensPopup : int {
    Dji = 1,      ///< "DJI" - the default.
    Classic = 2,  ///< "Classic".
};

/// The model a Lens popup value selects (1-based).  Exactly "Classic" is
/// Classic; everything else - "DJI", a corrupt project's out-of-range value,
/// garbage - is the default, DJI, the same rule every other sanitiser in
/// this header applies to a value it cannot place.
[[nodiscard]] inline constexpr CameraModel cameraModelFromLensPopup(long popupValue) noexcept {
    return (popupValue == static_cast<long>(LensPopup::Classic)) ? CameraModel::Classic : CameraModel::Dji;
}

/// The 1-based Lens popup value that selects a model.
[[nodiscard]] inline constexpr int lensPopupValue(CameraModel model) noexcept {
    return static_cast<int>(model == CameraModel::Classic ? LensPopup::Classic : LensPopup::Dji);
}

/// The model the Lens popup selects by default (a new instance, a project
/// saved before the popup existed, a host that does not expose it).
inline constexpr CameraModel kDefaultCameraModel = cameraModelFromLensPopup(OSV_REFRAME_LENS_DEFAULT);
static_assert(kDefaultCameraModel == CameraModel::Dji, "the Lens popup defaults to DJI (the user's choice)");
static_assert(cameraModelFromCheckbox(OSV_REFRAME_CAMERA_MODEL_DEFAULT) == kDefaultCameraModel,
              "the hidden Camera Model mirror must default to the Lens popup's default");

// ---------------------------------------------------------------------------
//  [WP-EASING] The Keyframe Easing popup
// ---------------------------------------------------------------------------

/// Popup values of "Keyframe Easing" (1-based, After Effects numbering), in
/// DJI Studio's Keyframe Animation order.  ReframeEasing.h defines the curve
/// each one draws and says which are DJI's exact numbers.
enum class KeyframeEasing : int {
    None = 1,           ///< Premiere's own interpolation - the effect does nothing.
    LinearSmooth = 2,   ///< Through every keyframe without stopping; speed changes smoothly.
    FastInSlowOut = 3,  ///< Leaves a keyframe fast, arrives at the next one slowly.
    SlowInFastOut = 4,  ///< Leaves a keyframe slowly, arrives at the next one fast.
    FastInFastOut = 5,  ///< Fast at both keyframes, slowest halfway.
    SlowInSlowOut = 6,  ///< Starts and stops gently at both keyframes.
    Linear = 7,         ///< Constant speed between keyframes, whatever Premiere's own type.
};

/// The easing a popup value selects.  Anything outside the list - a corrupt
/// project, garbage - is None: the one entry that cannot change a picture the
/// user already approved.
[[nodiscard]] inline constexpr KeyframeEasing sanitiseKeyframeEasing(long popupValue) noexcept {
    if (popupValue < 1 || popupValue > OSV_REFRAME_EASING_COUNT) {
        return KeyframeEasing::None;
    }
    return static_cast<KeyframeEasing>(popupValue);
}

static_assert(sanitiseKeyframeEasing(OSV_REFRAME_EASING_DEFAULT) == KeyframeEasing::None,
              "Keyframe Easing must default to None, so old projects render exactly as before");
static_assert(static_cast<int>(KeyframeEasing::Linear) == OSV_REFRAME_EASING_COUNT,
              "the Keyframe Easing enum and its popup must have the same number of entries");

// ---------------------------------------------------------------------------
//  [WP-LENSUI] What the Effect Controls panel shows per lens
//
//  Only the selected lens's controls are visible, so a user never sees two
//  "FOV" sliders that mean different angles - the question that started
//  this.  PF_Cmd_UPDATE_PARAMS_UI applies the table through
//  PF_UpdateParamUI with PF_PUI_INVISIBLE, which Premiere honours
//  dynamically (AE_Effect.h, PF_PUI_INVISIBLE: "for PPro only, the flag is
//  dynamic and can be cleared to make the parameter visible again"); the
//  Camera Model mirror is registered invisible and stays so.  Everything
//  else - Output Resolution, Preset, Pan / Tilt / Roll, the Source group,
//  Smooth Keyframes, Drag Sensitivity and the Lens popup itself - is always
//  shown.
// ---------------------------------------------------------------------------

/// One control that belongs to one lens.
struct LensControl {
    int aeIndex;        ///< kIndex* of the control.
    CameraModel lens;   ///< Shown only while this lens is selected.
    const char* shown;  ///< The name the panel shows while it is visible.
};

/// Every lens-specific control.
///
/// DJI FOV is registered as "DJI FOV" and SHOWN as "FOV": the registered name
/// is what a host that ignores dynamic UI changes (and therefore shows every
/// control) displays, where the two FOVs must stay distinguishable; while
/// the panel shows one lens at a time there is one FOV on screen, and it is
/// simply "FOV" in either lens.  Every other control is shown under the name
/// it is registered with.
inline constexpr LensControl kLensControls[] = {
    {kIndexFov, CameraModel::Classic, "FOV"},
    {kIndexDistortion, CameraModel::Classic, "Distortion"},
    {kIndexZoom, CameraModel::Dji, "Zoom"},
    {kIndexDjiFov, CameraModel::Dji, "FOV"},
    {kIndexCorrection, CameraModel::Dji, "Correction Angle"},
};

/// The lens-specific entry for a control, or null for a control every lens
/// shows (and for any index outside the list).
[[nodiscard]] inline constexpr const LensControl* lensControl(int aeIndex) noexcept {
    for (const LensControl& c : kLensControls) {
        if (c.aeIndex == aeIndex) {
            return &c;
        }
    }
    return nullptr;
}

/// Whether the Effect Controls panel shows the control at `aeIndex` while
/// `lens` is selected.  The Camera Model mirror is never shown; a
/// lens-specific control only with its lens; every other control always.
[[nodiscard]] inline constexpr bool controlVisible(int aeIndex, CameraModel lens) noexcept {
    if (aeIndex == kIndexCameraModel) {
        return false;
    }
    const LensControl* c = lensControl(aeIndex);
    return c ? (c->lens == lens) : true;
}

// ---------------------------------------------------------------------------
//  The resolved parameter set
// ---------------------------------------------------------------------------

/// Every control, read from the host at one instant in time and already
/// sanitised.  Both the CPU and the GPU path build one of these and nothing
/// downstream ever touches a PF_ParamDef or a PrParam again.
struct Settings {
    Resolution resolution = Resolution::MatchSequence;
    Preset preset = Preset::Wide;
    double panDeg = OSV_REFRAME_PAN_DEFAULT;
    double tiltDeg = OSV_REFRAME_TILT_DEFAULT;
    double rollDeg = OSV_REFRAME_ROLL_DEFAULT;
    double fovDeg = OSV_REFRAME_FOV_DEFAULT;
    double distortion = OSV_REFRAME_DISTORTION_DEFAULT;  ///< Percent, 0..100.
    double sourcePanDeg = 0.0;
    double sourceTiltDeg = 0.0;
    double sourceRollDeg = 0.0;
    bool smoothKeyframes = false;
    // [WP-CAMERA]
    /// Which lens renders: the Lens popup, decoded by the reader.  Classic
    /// here only so a hand-built block is the Classic camera (see CameraModel).
    CameraModel cameraModel = CameraModel::Classic;
    double zoomDeg = OSV_REFRAME_ZOOM_DEFAULT;               ///< Read-out only; never rendered from.
    double djiFovDeg = OSV_REFRAME_DJI_FOV_DEFAULT;          ///< DJI vertical pinhole FOV (deg).
    double correction = OSV_REFRAME_CORRECTION_DEFAULT;      ///< DJI eye distance (sphere radii).
    double dragSensitivity = OSV_REFRAME_DRAG_SENSITIVITY_DEFAULT;  ///< Overlay only; never rendered from.
    // [WP-EASING]
    /// The Keyframe Easing popup, as read.  The readers have ALREADY applied
    /// it to the angles and lens values above; it is carried here so a log
    /// line on a rejected setup can say which curve produced them.
    KeyframeEasing easing = KeyframeEasing::None;
};

// ---------------------------------------------------------------------------
//  [WP-CAMERA] DJI camera helpers (implemented in ReframeCpu.cpp)
// ---------------------------------------------------------------------------

/// The two numbers that fully describe DJI's lens.
struct DjiLens {
    double fovDeg = OSV_REFRAME_DJI_FOV_DEFAULT;       ///< Vertical pinhole field of view (deg).
    double correction = OSV_REFRAME_CORRECTION_DEFAULT;  ///< Eye distance behind the centre (radii).
};

/// The two numbers that describe the Classic lens, as the controls hold them.
struct ClassicLens {
    double fovDeg = OSV_REFRAME_FOV_DEFAULT;             ///< Visible horizontal angle (deg).
    double distortion = OSV_REFRAME_DISTORTION_DEFAULT;  ///< Eye offset in percent.
};

/// Clamp a DJI lens into the controls' VALID ranges, replacing non-finite
/// values with the defaults.  Every helper below sanitises its inputs with
/// this, so none of them can hand a NaN back to a parameter.
[[nodiscard]] DjiLens sanitiseDjiLens(DjiLens lens) noexcept;

/// DJI's "Zoom" for a lens on a frame of shape `aspect` (width / height):
/// DJI Studio's own Zoom formula (osv::geom::djiZoomDeg).  A lens or
/// aspect that DJI would answer 0 for answers 0 here too.
[[nodiscard]] double djiZoomDeg(DjiLens lens, double aspect) noexcept;

/// Move a lens along DJI Studio's zoom path until its Zoom equals
/// `targetZoomDeg`.
///
/// DJI Studio has no inverse for Zoom: its zoom gesture moves the two real
/// controls together, fov += 130 * delta and correction += delta, each
/// clamped (FOV to [20, 150], Correction to [0, 1]).  This finds the delta
/// whose result shows the requested Zoom, by bisection - Zoom grows
/// monotonically along the path, so the answer is unique.  A target past
/// either end of the path returns that end.  The clamps are widened to
/// include the starting lens, so a Crystal Ball (correction 1.8) is not
/// snapped back to 1.0 by the first zoom edit.
[[nodiscard]] DjiLens djiZoomTo(double targetZoomDeg, DjiLens from, double aspect) noexcept;

/// The DJI lens that frames exactly what a Classic lens frames, on a frame
/// of shape `aspect`.
///
/// Classic FOV is the visible angle across the frame WIDTH, at the eye offset
/// effectiveEyeOffset(distortion, fov) (the ramp included), clamped exactly
/// as the renderer clamps it.  With that eye distance d, the pinhole's
/// horizontal half angle is alpha = atan2(sin(fov/2), d + cos(fov/2)) and
/// DJI's vertical FOV follows from the shape: 2 atan(tan(alpha) / aspect).
/// Used when the user switches to DJI, so the picture carries straight over.
[[nodiscard]] DjiLens djiFromClassic(ClassicLens classic, double aspect) noexcept;

/// The Classic lens closest to a DJI lens: FOV = the DJI Zoom (the same
/// visible angle across the width) and Distortion = 100 x Correction.
///
/// Exact whenever the Classic ramp does not raise the eye offset above the
/// DJI correction and the correction is at most 1; beyond that the Classic
/// model cannot express the DJI look and this is the nearest it can do (the
/// values are clamped to the Classic controls' valid ranges).
[[nodiscard]] ClassicLens classicFromDji(DjiLens lens, double aspect) noexcept;

/// The DJI FOV a preset uses on a frame of shape `aspect` - the preset's
/// landscape, 9:16 or 3:4 column (see osv::geom::djiPresetVfovDeg).
[[nodiscard]] double djiPresetFovDeg(const PresetEntry& preset, double aspect) noexcept;

/// The shape (width / height) the camera frames for: the resolution the user
/// named, falling back to the sequence and then to 16:9 - the shape DJI
/// Studio projects default to - when neither is known.  Used by the
/// parameter UI (Zoom and the conversions), which runs without a frame.
[[nodiscard]] double framingAspect(Resolution resolution, SizePx sequenceSize) noexcept;

/// The pixel size the user asked for.
///
/// `sequenceSize` is what the Sequence Info Suite reported (invalid when it
/// could not be asked) and `frameSize` the frame we have actually been given
/// to render into.
///
/// "Match Sequence" prefers the sequence and falls back to the FRAME, not to
/// a fixed 16:9 guess as the old aspect control did.  That fallback is the
/// honest one: when nobody can tell us how big the sequence is, the frame the
/// host allocated for this render IS the sequence frame in every case that
/// matters, so using it is exact rather than approximate.  The old 16:9
/// constant was only ever a shape, and a shape was all the old control could
/// use; now that we need pixels there is a strictly better answer available.
///
/// The returned size is always valid() as long as `frameSize` is, so callers
/// never have to handle a zero.
[[nodiscard]] SizePx resolveOutputSize(Resolution resolution, SizePx sequenceSize, SizePx frameSize) noexcept;

/// The rectangle of the output frame that receives the picture.
///
/// This ALWAYS covers the whole frame - `x` and `y` are zero and `w` / `h`
/// are the frame's own size.  It stays a struct, and buildParams() still
/// fills OsvReframeParams::viewX..viewH from it, because the kernel's
/// viewport fields are the mechanism by which a render can be confined to a
/// sub-rectangle and removing them would be a gratuitous change to shared
/// kernel source that the CLI renderer also compiles.
///
/// WHY THERE IS NO LETTERBOX ANY MORE
/// ----------------------------------
/// The picture used to be drawn into a centred box of the chosen aspect,
/// leaving transparent bars.  A virtual camera has no reason to do that: if
/// the frame is a different shape than the user's chosen resolution, the
/// right answer is to fill the frame and crop whatever does not fit, which
/// is what buildParams() now does.  The chosen resolution influences the
/// camera's framing (which shape FOV is measured across), never the
/// coverage.
///
/// A degenerate frame still yields a zero rectangle, which buildParams()
/// rejects rather than dividing by.
struct Viewport {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
[[nodiscard]] Viewport computeViewport(int frameW, int frameH) noexcept;

// ---------------------------------------------------------------------------
//  Automatic projection ramp
// ---------------------------------------------------------------------------

/// Field of view (deg) at or below which the automatic ramp contributes
/// nothing, so a normal shot is exactly as rectilinear as the user asked for.
///
/// 120 is the default FOV and the "Wide" preset, i.e. the widest view most
/// users ever reach deliberately.  Below it, straight lines staying straight
/// is the whole point and any curvature would read as a defect.
#define OSV_REFRAME_AUTO_EYE_FOV_START 120.0

/// Field of view (deg) at which the ramp has reached full stereographic.
///
/// A rectilinear projection has a hard mathematical wall at 180 deg (the
/// tangent goes to infinity), and it becomes unusable well before it - at
/// 170 deg a rectilinear frame has already stretched the corners past any
/// tolerable amount.  The ramp therefore has to be FINISHED before the wall,
/// not at it.  240 is where the stereographic look is fully established; it
/// is also exactly the "Crystal Ball" preset's field of view, so the ramp
/// reproduces that preset's d = 1 at that preset's FOV, which is a useful
/// consistency: zooming out to 240 deg by hand lands on the same look the
/// preset gives.
#define OSV_REFRAME_AUTO_EYE_FOV_FULL 240.0

/// The eye offset `d` the automatic ramp asks for at a given field of view.
///
/// WHY THIS EXISTS
/// ---------------
/// Zooming out is the one gesture every user tries first, and past about
/// 170 deg a rectilinear camera simply cannot answer it - r = f tan(theta)
/// diverges at 180 and the corners tear long before that.  DJI Studio solves
/// this by sliding towards a stereographic projection as the view widens,
/// which is what produces the familiar "Tiny Planet" look; the user never
/// picks a projection, they just zoom out and the picture stays sane.
///
/// We already had the machinery - Projection::EyeOffset interpolates
/// continuously from rectilinear (d = 0) to stereographic (d = 1) - but it
/// was only reachable through the Distortion slider, which a user who is
/// simply dragging the FOV has no reason to have found.  This function is
/// what connects the two, so the default behaviour matches the expectation.
///
/// THE CURVE
/// ---------
/// A smoothstep in FOV between the two constants above:
///
///     t = (fov - START) / (FULL - START)   clamped to [0, 1]
///     d = t * t * (3 - 2t)
///
/// Smoothstep rather than a straight line for one concrete reason: its
/// DERIVATIVE is zero at both ends.  The ramp has to join the
/// no-auto-distortion region at 120 deg and the fully-stereographic region at
/// 240 deg, and if d changed at a non-zero rate across either join, a user
/// slowly dragging the FOV through it would see the curvature start or stop
/// abruptly - a visible kink, exactly the "pop" that has to be avoided.  With
/// smoothstep the value AND its first derivative are continuous everywhere,
/// so the distortion eases in and eases out and the drag feels like one
/// continuous motion.  (A cubic is enough here; smootherstep's additional
/// second-derivative continuity buys nothing a viewer can perceive in a
/// quantity that is itself a gentle geometric warp.)
///
/// Below START this returns exactly 0, so ordinary shots are untouched.
/// A non-finite field of view returns 0 as well: the safe answer for garbage
/// is the projection the user would have had without this feature at all.
[[nodiscard]] double autoEyeOffsetForFov(double fovDeg) noexcept;

/// The eye offset actually used, combining the manual Distortion control with
/// the automatic ramp.
///
/// `distortionPercent` is the slider, 0..100, and `fovDeg` the field of view.
///
/// The two are combined with a MAXIMUM, not a sum or a replacement, and that
/// choice is the whole design of this feature:
///
///   * it cannot fight the user.  Asking for more distortion than the ramp
///     wants always wins, so the slider never feels ignored or clamped - at
///     any FOV, dragging Distortion to 100 gives exactly stereographic, and
///     the presets (Crystal Ball and Asteroid both ask for 100) still land on
///     precisely the look they always did.
///   * it cannot produce an unusable frame.  Leaving the slider at 0 and
///     zooming out to 300 deg still ramps up to stereographic, because the
///     ramp's floor applies regardless, which is the entire point: a user who
///     has never found the Distortion control gets the right projection.
///   * it stays continuous.  A maximum of two continuous functions is
///     continuous, so there is still no value of FOV or Distortion at which
///     the picture jumps.
///
/// A sum would have broken the first property (the slider would over-drive
/// past stereographic and need clamping, which WOULD feel ignored) and a
/// replacement would have broken it outright.
///
/// Returns a value in [0, 1] for every input, including non-finite ones.
[[nodiscard]] double effectiveEyeOffset(double distortionPercent, double fovDeg) noexcept;

}  // namespace osv::reframe

#endif /* OSV_REFRAME_CPLUSPLUS */

#endif /* OSV_REFRAME_PARAMS_H */
