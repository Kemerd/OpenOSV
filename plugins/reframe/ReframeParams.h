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
 * files and the plug-in cache, and PrGPUFilterInfo::outMatchName must be
 * byte-identical to it or the GPU entry point is never bound to the effect.
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
#define OSV_REFRAME_ID_OUTPUT_ASPECT 1
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

/* Total parameters excluding the input layer: the 13 controls plus the two
 * group terminators.  out_data->num_params is this + 1. */
#define OSV_REFRAME_PARAM_COUNT 15

/* ==========================================================================
 *  Popup item strings
 *
 *  AE popup items are one string with '|' between entries.  Both lists are
 *  also mirrored as C++ arrays below so the tests can compare them item by
 *  item without re-parsing the separator.
 * ========================================================================== */
#define OSV_REFRAME_ASPECT_ITEMS "Match Sequence|16:9|9:16|1:1|4:3|3:4|2.35:1|Full Frame"
#define OSV_REFRAME_ASPECT_COUNT 8

#define OSV_REFRAME_PRESET_ITEMS "Custom|Crystal Ball|Asteroid|Wide|Ultra Wide|Dewarping"
#define OSV_REFRAME_PRESET_COUNT 6

/* ==========================================================================
 *  Ranges and defaults (the numbers PF_ADD_* is called with)
 * ========================================================================== */
/* Output Aspect: 1-based popup value; 1 = "Match Sequence". */
#define OSV_REFRAME_ASPECT_DEFAULT 1

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
///   1  Output Aspect
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
enum ParamIndex : int {
    kIndexOutputAspect = 1,
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
};

/// The permanent id stored with each index, in index order (index 1 first).
/// EffectMain.cpp walks this when it adds the parameters and a test walks it
/// against the list the built module actually produced, so the two can never
/// drift apart silently.
inline constexpr int kParamIdByIndex[OSV_REFRAME_PARAM_COUNT] = {
    OSV_REFRAME_ID_OUTPUT_ASPECT, OSV_REFRAME_ID_CAMERA_TOPIC,     OSV_REFRAME_ID_PRESET,
    OSV_REFRAME_ID_PAN,           OSV_REFRAME_ID_TILT,             OSV_REFRAME_ID_ROLL,
    OSV_REFRAME_ID_FOV,           OSV_REFRAME_ID_DISTORTION,       OSV_REFRAME_ID_CAMERA_TOPIC_END,
    OSV_REFRAME_ID_SOURCE_TOPIC,  OSV_REFRAME_ID_SOURCE_PAN,       OSV_REFRAME_ID_SOURCE_TILT,
    OSV_REFRAME_ID_SOURCE_ROLL,   OSV_REFRAME_ID_SOURCE_TOPIC_END, OSV_REFRAME_ID_SMOOTH,
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
    Int32,    ///< A popup (Output Aspect, Preset): a small 1-based integer.
    Float32,  ///< An AE angle dial (the six Pan / Tilt / Roll controls), in degrees.
    Float64,  ///< An AE float slider (FOV, Distortion).
    Bool,     ///< A checkbox (Smooth Keyframes).
};

/// Number of controls that actually carry a value, i.e. everything except
/// the four PF_Param_GROUP_START / GROUP_END markers.  This is the list a
/// probe expects to find on the host, in this order.
inline constexpr int kValueParamCount = 11;

/// The AE indices of the eleven value-carrying controls, in ADD ORDER.
/// The group markers are absent by construction: they hold no value, so no
/// host can report one for them and nothing ever reads them.
inline constexpr int kValueParamAeIndex[kValueParamCount] = {
    kIndexOutputAspect, kIndexPreset,     kIndexPan,        kIndexTilt,
    kIndexRoll,         kIndexFov,        kIndexDistortion, kIndexSourcePan,
    kIndexSourceTilt,   kIndexSourceRoll, kIndexSmooth,
};

/// The PrParam kind each of those controls arrives in, in the same order.
///
/// Read down the column and this is the SIGNATURE the probe matches:
///
///     i32 i32 f32 f32 f32 f64 f64 f32 f32 f32 bool
///
/// It is highly distinctive - in particular the adjacent Float64 pair (FOV
/// and Distortion, the only two float sliders) and the single trailing Bool
/// (Smooth Keyframes, the only checkbox) pin the sequence down even when a
/// host reports a different number of entries than we added.
inline constexpr HostParamKind kValueParamKind[kValueParamCount] = {
    HostParamKind::Int32,   HostParamKind::Int32,   HostParamKind::Float32, HostParamKind::Float32,
    HostParamKind::Float32, HostParamKind::Float64, HostParamKind::Float64, HostParamKind::Float32,
    HostParamKind::Float32, HostParamKind::Float32, HostParamKind::Bool,
};

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
/// So the host list is our eleven controls with a CONTIGUOUS RUN dropped out
/// of the middle.  That is the shape the matcher below is built for: it
/// aligns our signature against the host list allowing exactly one contiguous
/// gap, which covers the unreduced case (an 11-entry host, gap length zero)
/// and the observed case (an 8-entry host, gap length three) with the same
/// code and no special-casing of either number.
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
//  Output aspect
// ---------------------------------------------------------------------------

/// Popup values of "Output Aspect" (1-based, matching AE popup semantics).
enum class Aspect : int {
    MatchSequence = 1,  ///< Ask the Sequence Info Suite; 16:9 when it cannot answer.
    Ratio16x9 = 2,
    Ratio9x16 = 3,
    Ratio1x1 = 4,
    Ratio4x3 = 5,
    Ratio3x4 = 6,
    Ratio235x1 = 7,
    FullFrame = 8,  ///< The whole input frame, whatever shape it is.
};

/// One entry of the aspect table.
struct AspectEntry {
    Aspect value;        ///< Popup value.
    const char* label;   ///< Exactly the text in OSV_REFRAME_ASPECT_ITEMS.
    double widthUnits;   ///< 0 for the two dynamic entries.
    double heightUnits;  ///< 0 for the two dynamic entries.
};

/// The aspect table.  Order and labels must match OSV_REFRAME_ASPECT_ITEMS.
inline constexpr AspectEntry kAspects[OSV_REFRAME_ASPECT_COUNT] = {
    {Aspect::MatchSequence, "Match Sequence", 0.0, 0.0},
    {Aspect::Ratio16x9, "16:9", 16.0, 9.0},
    {Aspect::Ratio9x16, "9:16", 9.0, 16.0},
    {Aspect::Ratio1x1, "1:1", 1.0, 1.0},
    {Aspect::Ratio4x3, "4:3", 4.0, 3.0},
    {Aspect::Ratio3x4, "3:4", 3.0, 4.0},
    {Aspect::Ratio235x1, "2.35:1", 2.35, 1.0},
    {Aspect::FullFrame, "Full Frame", 0.0, 0.0},
};

/// Clamp an arbitrary popup value (a corrupt project can hold anything) into
/// the valid range.
[[nodiscard]] inline constexpr Aspect sanitiseAspect(int popupValue) noexcept {
    if (popupValue < 1 || popupValue > OSV_REFRAME_ASPECT_COUNT) {
        return Aspect::MatchSequence;
    }
    return static_cast<Aspect>(popupValue);
}

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

/// One preset: the three values the popup writes into the controls.  The
/// numbers mirror osv::geom::kPresets (include/osv/geom/Presets.h) - the
/// same looks the CLI renderer offers - with the distortion expressed as the
/// percentage the slider shows.
struct PresetEntry {
    Preset value;       ///< Popup value.
    const char* label;  ///< Exactly the text in OSV_REFRAME_PRESET_ITEMS.
    double fovDeg;      ///< Written into FOV.
    double distortion;  ///< Written into Distortion (percent, = 100 * eye offset).
    double tiltDeg;     ///< Written into Tilt.
    bool writesControls;  ///< False for Custom, which only reflects manual edits.
};

/// The preset table.  Order and labels must match OSV_REFRAME_PRESET_ITEMS.
inline constexpr PresetEntry kPresetTable[OSV_REFRAME_PRESET_COUNT] = {
    {Preset::Custom, "Custom", OSV_REFRAME_FOV_DEFAULT, OSV_REFRAME_DISTORTION_DEFAULT, 0.0, false},
    {Preset::CrystalBall, "Crystal Ball", 240.0, 100.0, 0.0, true},
    {Preset::Asteroid, "Asteroid", 300.0, 100.0, -90.0, true},
    {Preset::Wide, "Wide", 120.0, 15.0, 0.0, true},
    {Preset::UltraWide, "Ultra Wide", 150.0, 40.0, 0.0, true},
    {Preset::Dewarping, "Dewarping", 95.0, 0.0, 0.0, true},
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
//  The resolved parameter set
// ---------------------------------------------------------------------------

/// Every control, read from the host at one instant in time and already
/// sanitised.  Both the CPU and the GPU path build one of these and nothing
/// downstream ever touches a PF_ParamDef or a PrParam again.
struct Settings {
    Aspect aspect = Aspect::MatchSequence;
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
};

/// The aspect the picture is drawn at, as a width/height ratio.
///
/// `sequenceAspect` is what the Sequence Info Suite reported (<= 0 when it
/// could not be asked) and `frameAspect` the shape of the frame we are
/// rendering into.  "Match Sequence" falls back to 16:9 exactly as
/// docs/PREMIERE.md specifies, and "Full Frame" is the frame itself.
[[nodiscard]] double resolveAspectRatio(Aspect aspect, double sequenceAspect, double frameAspect) noexcept;

/// The centred rectangle of the given aspect ratio that fits inside a
/// `frameW` x `frameH` frame.  Always at least 1x1 and never larger than the
/// frame; a non-finite or non-positive ratio yields the whole frame.
struct Viewport {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};
[[nodiscard]] Viewport computeViewport(int frameW, int frameH, double aspectRatio) noexcept;

}  // namespace osv::reframe

#endif /* OSV_REFRAME_CPLUSPLUS */

#endif /* OSV_REFRAME_PARAMS_H */
