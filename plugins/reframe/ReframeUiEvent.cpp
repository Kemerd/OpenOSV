// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeUiEvent.cpp - PF_Cmd_EVENT plumbing for the Program Monitor overlay.
//
// This file is the ONLY place in the overlay that names an Adobe type.  It
// reads a PF_EventExtra, hands the numbers to the interaction core in
// ReframeUi.cpp, draws the HUD with DrawBot and writes the new values back
// so the host records them.  All three of those jobs are separated by
// deliberate boundaries:
//
//   * the maths lives in ReframeUi.cpp and is tested without a host;
//   * the drawing lives in `drawOverlay()` and touches no parameter;
//   * the value write-back lives in `commitValues()` and draws nothing.
//
// ===========================================================================
//  HOW KEYFRAMES GET RECORDED - the mechanism, and why this one
// ===========================================================================
//
// There are two APIs that look like they change a parameter and only one of
// them actually does.
//
//   PF_ParamUtilsSuite3::PF_UpdateParamUI (AE_EffectSuites.h:203-239) is
//   documented as "These changes are COSMETIC ONLY, and don't go into the
//   undo buffer", and the list of fields it may touch - ui_flags, ui_width,
//   ui_height, name, slider_min/max, precision, display_flags - contains no
//   value field at all.  It cannot set Pan.  Using it here would produce an
//   overlay that appears to drag and records nothing.
//
//   PF_ChangeFlag_CHANGED_VALUE (AE_Effect.h:2371-2384) is documented as
//   "Set this flag for each param whose value you change when handling a
//   PF_Cmd_USER_CHANGED_PARAM or specific PF_Cmd_EVENT events
//   (PF_Event_DO_CLICK, PF_Event_DRAG, & PF_Event_KEYDOWN) ... These changes
//   are undoable and re-doable by the user", with the additional
//   requirement: "If set during PF_Cmd_EVENT, be sure to also set
//   PF_EO_HANDLED_EVENT before returning."
//
// So the mechanism is exactly the second one, and it is used exactly as
// documented:
//
//   1. write the new value into the PF_ParamDef the host handed us in
//      `params[index]` (u.ad.value for the three angles, u.fs_d.value for
//      FOV);
//   2. set `params[index]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE`
//      for EVERY parameter whose value moved, and for no others;
//   3. set `extra->evt_out_flags |= PF_EO_HANDLED_EVENT`.
//
// The host then commits the value AT THE CURRENT TIME.  When the stopwatch
// for that parameter is running, committing a value at the current time is
// precisely what creates or updates a keyframe - it is the same code path a
// typed-in number takes - so keyframing is automatic and needs no keyframe
// API of our own.  When the stopwatch is off, the same write simply moves
// the constant value.  Both behaviours are what a user expects and neither
// needed a special case.
//
// PF_OutFlag_FORCE_RERENDER is deliberately NOT set: AE_Effect.h:752 says
// "setting PF_ChangeFlag_CHANGED_VALUE automatically causes a re-render, so
// don't worry about setting PF_OutFlag_FORCE_RERENDER in that case", and the
// same block warns that FORCE_RERENDER forces cache invalidation that
// interacts badly with undo.
//
// ===========================================================================
//  WHAT THE SDK DOES NOT OFFER
// ===========================================================================
//
// There is NO mouse-wheel event.  The complete event list is
// AE_EffectUI.h:103-117 (NEW_CONTEXT, ACTIVATE, DO_CLICK, DRAG, DRAW,
// DEACTIVATE, CLOSE_CONTEXT, IDLE, KEYDOWN_OBSOLETE, ADJUST_CURSOR, KEYDOWN,
// MOUSE_EXITED) and none of them carries a wheel delta.  The only wheel-ish
// field anywhere in the event structures is PF_StylusEventInfo::stylus_wheelF
// (AE_EffectUI.h:~490), which belongs to PF_PointerEventInfo - a stylus
// struct that is not reachable from PF_EventUnion at all.  Scroll-to-zoom is
// therefore NOT implemented, because implementing it would mean installing a
// Windows mouse hook behind the host's back.  Ctrl+drag is the zoom shortcut
// instead, and docs/PREMIERE.md says so.
#include "ReframeUiEvent.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "HostSuites.h"
#include "PluginLog.h"

// The DrawBot suite declarations and the PF custom-UI suite.  The SDK ships
// an AEFX_SuiteHelper.c with AEFX_AcquireDrawbotSuites(), but this file does
// its own acquisition instead, for three reasons: that helper never acquires
// the Pen suite (leaving DRAWBOT_Suites::pen_suiteP null, which would make a
// dashed-pen feature silently do nothing); it writes a diagnostic into
// out_data->return_msg, which pops a modal error dialog in Premiere on a
// path that should degrade silently; and compiling a C file from the SDK
// into this /W4 target would mean disabling warnings for it.  The
// acquisition below is four SPBasicSuite calls and is easier to audit.
#include "AE_EffectSuites.h"

#include "adobesdk/DrawbotSuite.h"

#include "SPBasic.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

namespace osv::reframe::ui {

/// The plug-in's log lives in osv::premiere; pulling the name in keeps the
/// once-only degradation messages below readable without qualifying every
/// call site.
using osv::premiere::PluginLog;

namespace {

// ---------------------------------------------------------------------------
//  The modifier mirror must not drift
//
//  ReframeUi.h spells the PF_Mod_* values out so the interaction core can be
//  compiled without the SDK.  These asserts are what make that safe: a
//  future SDK that renumbered a modifier bit breaks the build here instead
//  of silently turning Shift into Alt at run time.
// ---------------------------------------------------------------------------
static_assert(static_cast<std::uint32_t>(kModCmdCtrl) == static_cast<std::uint32_t>(PF_Mod_CMD_CTRL_KEY),
              "kModCmdCtrl must mirror PF_Mod_CMD_CTRL_KEY");
static_assert(static_cast<std::uint32_t>(kModShift) == static_cast<std::uint32_t>(PF_Mod_SHIFT_KEY),
              "kModShift must mirror PF_Mod_SHIFT_KEY");
static_assert(static_cast<std::uint32_t>(kModCapsLock) == static_cast<std::uint32_t>(PF_Mod_CAPS_LOCK_KEY),
              "kModCapsLock must mirror PF_Mod_CAPS_LOCK_KEY");
static_assert(static_cast<std::uint32_t>(kModOptAlt) == static_cast<std::uint32_t>(PF_Mod_OPT_ALT_KEY),
              "kModOptAlt must mirror PF_Mod_OPT_ALT_KEY");

// ---------------------------------------------------------------------------
//  Fixed-point helpers
//
//  PF_Fixed is 16.16.  An angle dial stores degrees in it, so a full turn is
//  360 * 65536 and the representable range is about +-32768 degrees - far
//  more than anyone will keyframe, but the conversion still saturates rather
//  than wrapping, because a wrapped angle would spin the picture.
// ---------------------------------------------------------------------------
constexpr double kFixedOne = 65536.0;

/// Largest frame edge the overlay will lay itself out against.
///
/// The only values that reach the geometry come from the host, and one of
/// them (a draw event's update rect) is a pair of subtractions that a
/// nonsense rectangle could make enormous.  Anything past this is treated as
/// "no usable frame" rather than propagated into the layout maths.  It is
/// deliberately far above any real Program Monitor size, including a 16K
/// timeline on a scaled display.
constexpr long kMaxOverlayEdge = 65536;

/// PF_Fixed -> degrees.
[[nodiscard]] double fixedToDeg(PF_Fixed value) noexcept { return static_cast<double>(value) / kFixedOne; }

/// Degrees -> PF_Fixed, saturating at the type's limits.
[[nodiscard]] PF_Fixed degToFixed(double deg) noexcept {
    if (!std::isfinite(deg)) {
        return 0;
    }
    const double scaled = std::round(deg * kFixedOne);
    // PF_Fixed is A_long (32-bit signed); saturate rather than let the cast
    // be undefined.
    constexpr double kMax = 2147483647.0;
    constexpr double kMin = -2147483648.0;
    return static_cast<PF_Fixed>(std::clamp(scaled, kMin, kMax));
}

// ---------------------------------------------------------------------------
//  The drag table
//
//  PF_DoClickEventInfo::continue_refcon is four A_intptr_t words - 32 bytes
//  on a 64-bit host - which the host carries from a DO_CLICK to every DRAG
//  of the same gesture.  A DragState does not fit in 32 bytes, so the state
//  lives in this small fixed table and only a slot index plus a generation
//  counter travel in the refcon.
//
//  The generation counter is the important half: a stale refcon from a
//  previous gesture (or from a previous load of the plug-in) would otherwise
//  index a live slot and resume somebody else's drag.  A slot is only
//  accepted when its generation matches the one stored in the refcon, so a
//  stale word is rejected instead of misinterpreted.
//
//  Fixed capacity rather than a map: the host runs at most a handful of
//  monitors, allocation inside a mouse event is a latency risk, and a fixed
//  array needs no destructor at shutdown.
// ---------------------------------------------------------------------------
constexpr int kMaxConcurrentDrags = 8;

/// The magic word stored in continue_refcon[0].  Anything else means the
/// refcon is not ours (or is uninitialised memory) and is ignored.
constexpr A_intptr_t kRefconMagic = static_cast<A_intptr_t>(0x4F53565544524731LL);  // "OSVUDRG1"

/// One slot of the drag table.
struct DragSlot {
    DragState state;             ///< The gesture, or an inactive default.
    std::uint64_t generation = 0;  ///< Bumped on every acquire; 0 = never used.
    bool inUse = false;
};

/// The table and the one mutex that guards it.
///
/// Function-local statics rather than namespace-scope objects: their
/// construction is ordered and thread-safe by the standard, so a DO_CLICK
/// that arrives before any other selector cannot race the initialiser.
struct DragTable {
    std::mutex mutex;
    DragSlot slots[kMaxConcurrentDrags];
    std::uint64_t nextGeneration = 1;
};

[[nodiscard]] DragTable& dragTable() noexcept {
    static DragTable table;
    return table;
}

/// Take a free slot.  Returns -1 when every slot is busy, which the caller
/// treats as "cannot start a drag" rather than as an error - the picture
/// simply does not move, and nothing is corrupted.
[[nodiscard]] int acquireDragSlot(std::uint64_t& generationOut) noexcept {
    DragTable& table = dragTable();
    std::lock_guard<std::mutex> lock(table.mutex);
    for (int i = 0; i < kMaxConcurrentDrags; ++i) {
        if (!table.slots[i].inUse) {
            table.slots[i].inUse = true;
            table.slots[i].generation = table.nextGeneration++;
            table.slots[i].state = DragState{};
            generationOut = table.slots[i].generation;
            return i;
        }
    }
    return -1;
}

/// Release a slot.  Out-of-range indices and generation mismatches are
/// ignored, so a duplicated "last drag" event cannot free somebody else's
/// gesture.
void releaseDragSlot(int index, std::uint64_t generation) noexcept {
    if (index < 0 || index >= kMaxConcurrentDrags) {
        return;
    }
    DragTable& table = dragTable();
    std::lock_guard<std::mutex> lock(table.mutex);
    if (table.slots[index].generation != generation) {
        return;
    }
    table.slots[index].inUse = false;
    table.slots[index].state = DragState{};
}

/// Copy a slot's state out.  Returns false when the slot is free or the
/// generation does not match.
[[nodiscard]] bool readDragSlot(int index, std::uint64_t generation, DragState& out) noexcept {
    if (index < 0 || index >= kMaxConcurrentDrags) {
        return false;
    }
    DragTable& table = dragTable();
    std::lock_guard<std::mutex> lock(table.mutex);
    if (!table.slots[index].inUse || table.slots[index].generation != generation) {
        return false;
    }
    out = table.slots[index].state;
    return true;
}

/// Copy a state back into a slot.  Same guards as readDragSlot().
void writeDragSlot(int index, std::uint64_t generation, const DragState& state) noexcept {
    if (index < 0 || index >= kMaxConcurrentDrags) {
        return;
    }
    DragTable& table = dragTable();
    std::lock_guard<std::mutex> lock(table.mutex);
    if (!table.slots[index].inUse || table.slots[index].generation != generation) {
        return;
    }
    table.slots[index].state = state;
}

// ---------------------------------------------------------------------------
//  Hover tracking
//
//  PF_Event_DRAW carries no pointer position (PF_DrawEventInfo is an update
//  rect and a depth - AE_EffectUI.h:283-286), so the draw pass cannot
//  hit-test for itself.  PF_Event_ADJUST_CURSOR does carry one and arrives
//  on every mouse move over the custom UI, so the handle it computed is
//  remembered here and the next repaint highlights it.
//
//  One value for the whole process rather than one per context: two Program
//  Monitors cannot both have the pointer in them, so the last answer is
//  always the right one for whichever view is being repainted under the
//  cursor.  It is an atomic because ADJUST_CURSOR and DRAW can be dispatched
//  from different threads in AE 13.5+ and a torn read of an enum would be a
//  genuine data race, however harmless the value.
// ---------------------------------------------------------------------------
[[nodiscard]] std::atomic<int>& hoverHandleStore() noexcept {
    static std::atomic<int> hover{static_cast<int>(Handle::None)};
    return hover;
}

/// Remember what the pointer is over.
void setHoverHandle(Handle handle) noexcept {
    hoverHandleStore().store(static_cast<int>(handle), std::memory_order_relaxed);
}

/// What the pointer was last over.  An out-of-range stored value (which
/// cannot happen, but is checked anyway) reads back as None.
[[nodiscard]] Handle hoverHandle() noexcept {
    const int raw = hoverHandleStore().load(std::memory_order_relaxed);
    switch (raw) {
    case static_cast<int>(Handle::PanTilt):
        return Handle::PanTilt;
    case static_cast<int>(Handle::Roll):
        return Handle::Roll;
    case static_cast<int>(Handle::Fov):
        return Handle::Fov;
    default:
        return Handle::None;
    }
}

// ---------------------------------------------------------------------------
//  The live readout
//
//  WHY THE HUD DOES NOT SIMPLY READ `params` DURING A DRAG.  The numbers in
//  the readout are the only feedback that can be instant: the picture
//  underneath moves only as fast as the host re-renders the frame, and a 360
//  reframe of a 6K source in Premiere is anything but instant.  The
//  parameter array a PF_Event_DRAW receives is whatever the host considers
//  current for the view being painted, and nothing in the SDK promises that
//  it already holds the value a PF_Event_DRAG committed a few milliseconds
//  earlier - a host is free to hand the draw pass the values of the frame it
//  last FINISHED rendering.  If Premiere does that, a readout built from
//  `params` lags exactly as badly as the picture and the one piece of
//  instant feedback is gone.
//
//  So while a gesture is in flight, every value it commits is also
//  remembered here and the draw pass shows that instead.  The guards:
//
//    * it is keyed by the effect instance (in_data->effect_ref), so a
//      second reframe effect on another clip never shows this one's numbers;
//    * an effect reference is only an address and a host may reuse one, so
//      the values are also checked for CONSISTENCY before they are shown:
//      every field the gesture has not written must already agree with the
//      host, and only the fields it has written are substituted (see
//      mergeLiveReadout() in ReframeUi.cpp, which the tests pin);
//    * it is dropped the moment the gesture ends (the DRAG event with
//      `last_time` set) - from then on the host is the authority again, and
//      a value typed into Effect Controls afterwards shows up at once;
//    * it expires kLiveReadoutMaxAge after its last update whatever happens,
//      so a gesture the host abandons without ever sending `last_time`
//      cannot pin stale numbers on the HUD.
//
//  Deliberately NOT held past the end of the gesture.  Holding it until the
//  host "caught up" would need a guess at when that is, and an effect
//  reference is only an address: one freed and reused inside the hold
//  window would put a dead instance's numbers on a new one.
//
//  One process-wide slot rather than one per instance: there is one mouse,
//  so only one gesture can be in flight at a time.
// ---------------------------------------------------------------------------

/// How long a remembered drag value may stand in for the host's values
/// without a fresh drag event.  Every drag event restarts the clock, so a
/// long continuous drag never expires; this only bounds how long a gesture
/// the host abandoned can mislead.
constexpr auto kLiveReadoutMaxAge = std::chrono::milliseconds(2000);

/// The remembered values and their guard.
struct LiveReadout {
    std::mutex mutex;
    PF_ProgPtr effectRef = nullptr;  ///< Which instance the values belong to.
    CameraValues values;             ///< What the gesture last committed.
    std::uint32_t touched = kChangedNone;  ///< Union of the fields it has written.
    std::chrono::steady_clock::time_point stamp{};  ///< When they were committed.
    bool active = false;             ///< Whether `values` may be shown at all.
};

[[nodiscard]] LiveReadout& liveReadout() noexcept {
    static LiveReadout readout;
    return readout;
}

/// Remember what an in-flight drag event just committed, and which fields
/// it wrote.
///
/// The written fields ACCUMULATE over the gesture (a pan that becomes a zoom
/// when Ctrl goes down has touched Pan, Tilt and FOV by the end), because
/// mergeLiveReadout() needs the whole set to know which of the host's values
/// may legitimately lag.  A null effect reference could never be matched
/// against a later draw, so it is not remembered at all.
void publishLiveReadout(PF_ProgPtr effectRef, const CameraValues& values, std::uint32_t touched) noexcept {
    if (!effectRef) {
        return;
    }
    LiveReadout& readout = liveReadout();
    std::lock_guard<std::mutex> lock(readout.mutex);
    // A different instance, or nothing remembered: this is the start of the
    // set.  Otherwise it is the same gesture continuing (onDoClick clears the
    // slot when a new one begins), so the set grows.
    const bool continuing = readout.active && readout.effectRef == effectRef;
    readout.touched = continuing ? (readout.touched | touched) : touched;
    readout.effectRef = effectRef;
    readout.values = sanitise(values);
    readout.stamp = std::chrono::steady_clock::now();
    readout.active = true;
}

/// The gesture is over: hand the readout back to the host.  Only the
/// instance that owns the remembered values can clear them.
void clearLiveReadout(PF_ProgPtr effectRef) noexcept {
    LiveReadout& readout = liveReadout();
    std::lock_guard<std::mutex> lock(readout.mutex);
    if (readout.effectRef == effectRef) {
        readout.active = false;
        readout.touched = kChangedNone;
    }
}

/// The numbers the HUD should show for this instance: the in-flight
/// gesture's own values for the fields it has written, while they are fresh
/// and consistent with the host; otherwise the host's.  The consistency rule
/// lives in mergeLiveReadout() (ReframeUi.cpp) so the tests pin it.
[[nodiscard]] CameraValues readoutValues(PF_ProgPtr effectRef, const CameraValues& fromHost) noexcept {
    if (!effectRef) {
        return fromHost;
    }
    LiveReadout& readout = liveReadout();
    std::lock_guard<std::mutex> lock(readout.mutex);
    if (!readout.active || readout.effectRef != effectRef) {
        return fromHost;
    }
    if (std::chrono::steady_clock::now() - readout.stamp > kLiveReadoutMaxAge) {
        // Abandoned gesture: the host is the authority again.
        readout.active = false;
        readout.touched = kChangedNone;
        return fromHost;
    }
    return mergeLiveReadout(fromHost, readout.values, readout.touched);
}

/// Pack a slot reference into the host's continue_refcon words.
void packRefcon(A_intptr_t refcon[4], int index, std::uint64_t generation) noexcept {
    if (!refcon) {
        return;
    }
    refcon[0] = kRefconMagic;
    refcon[1] = static_cast<A_intptr_t>(index);
    refcon[2] = static_cast<A_intptr_t>(generation);
    refcon[3] = 0;
}

/// Unpack a slot reference.  Returns false unless the magic word is intact,
/// which is what rejects uninitialised or foreign refcons.
[[nodiscard]] bool unpackRefcon(const A_intptr_t refcon[4], int& index, std::uint64_t& generation) noexcept {
    if (!refcon || refcon[0] != kRefconMagic) {
        return false;
    }
    index = static_cast<int>(refcon[1]);
    generation = static_cast<std::uint64_t>(refcon[2]);
    return index >= 0 && index < kMaxConcurrentDrags;
}

// ---------------------------------------------------------------------------
//  Reading and writing the camera parameters
// ---------------------------------------------------------------------------

/// The four camera parameters, read out of the host's array.
///
/// Returns a sanitised snapshot; a null array or a null entry yields the
/// defaults, which is the only sensible answer and never a crash.
[[nodiscard]] CameraValues readCamera(PF_ParamDef* params[]) noexcept {
    CameraValues v;
    if (!params) {
        return sanitise(v);
    }
    if (params[kIndexPan]) {
        v.panDeg = fixedToDeg(params[kIndexPan]->u.ad.value);
    }
    if (params[kIndexTilt]) {
        v.tiltDeg = fixedToDeg(params[kIndexTilt]->u.ad.value);
    }
    if (params[kIndexRoll]) {
        v.rollDeg = fixedToDeg(params[kIndexRoll]->u.ad.value);
    }
    if (params[kIndexFov]) {
        v.fovDeg = static_cast<double>(params[kIndexFov]->u.fs_d.value);
    }
    // [WP-CAMERA] DJI's lens, and [WP-LENSUI] which of the two lenses
    // renders: the Lens popup, read exactly as the CPU path's readSettings()
    // reads it, so the HUD and the zoom drag always follow the lens on
    // screen.  A host array without the popup (a short array, an old caller)
    // reads as the popup's default, DJI - again what the renderer would draw.
    v.dji = (params[kIndexLens] ? cameraModelFromLensPopup(params[kIndexLens]->u.pd.value) : kDefaultCameraModel) ==
            CameraModel::Dji;
    if (params[kIndexDjiFov]) {
        v.djiFovDeg = static_cast<double>(params[kIndexDjiFov]->u.fs_d.value);
    }
    if (params[kIndexCorrection]) {
        v.correction = static_cast<double>(params[kIndexCorrection]->u.fs_d.value);
    }
    return sanitise(v);
}

/// [WP-CAMERA] The Drag Sensitivity control, or its default when the host
/// array does not carry it.  Clamped by effectiveSensitivity() at use.
[[nodiscard]] double readSensitivity(PF_ParamDef* params[]) noexcept {
    if (!params || !params[kIndexDragSensitivity]) {
        return kPanTiltSensitivity;
    }
    return effectiveSensitivity(static_cast<double>(params[kIndexDragSensitivity]->u.fs_d.value));
}

/// Start a "grab the sphere" for a pan / tilt gesture.
///
/// Builds the camera the renderer would build for a frame the size of the
/// overlay's viewport - the same buildView() the CPU and GPU paths call, fed
/// the live Output Resolution, FOV, Distortion and angles - and casts the
/// anchor through it (beginSphereGrab).  Any unreadable parameter or a
/// camera that cannot be built yields an invalid grab, and the drag then
/// falls back to the fixed-rate mapping exactly as it behaved before.
///
/// The source rotation is read for completeness but cancels out of the grab
/// equation (see SphereGrab::target), so it cannot skew the result.
[[nodiscard]] SphereGrab grabForClick(PF_ParamDef* params[], const Layout& layout, const CameraValues& start,
                                      const PointF& anchor) noexcept {
    if (!params || !layout.valid) {
        return SphereGrab{};
    }
    // Every entry the camera depends on must be present; a host that handed
    // a partial array gets the old behaviour rather than a guessed camera.
    const int needed[] = {kIndexOutputResolution, kIndexPreset,     kIndexFov,        kIndexDistortion,
                          kIndexPan,              kIndexTilt,       kIndexRoll,       kIndexSourcePan,
                          kIndexSourceTilt,       kIndexSourceRoll};
    for (const int index : needed) {
        if (!params[index]) {
            return SphereGrab{};
        }
    }
    Settings s;
    s.resolution = sanitiseResolution(params[kIndexOutputResolution]->u.pd.value);
    s.preset = sanitisePreset(params[kIndexPreset]->u.pd.value);
    s.fovDeg = start.fovDeg;
    s.distortion = static_cast<double>(params[kIndexDistortion]->u.fs_d.value);
    s.panDeg = start.panDeg;
    s.tiltDeg = start.tiltDeg;
    s.rollDeg = start.rollDeg;
    s.sourcePanDeg = fixedToDeg(params[kIndexSourcePan]->u.ad.value);
    s.sourceTiltDeg = fixedToDeg(params[kIndexSourceTilt]->u.ad.value);
    s.sourceRollDeg = fixedToDeg(params[kIndexSourceRoll]->u.ad.value);
    // [WP-CAMERA] The lens the picture is actually rendered with, so the
    // grab casts the pointer through DJI's camera when that is on screen.
    s.cameraModel = start.dji ? CameraModel::Dji : CameraModel::Classic;
    s.djiFovDeg = start.djiFovDeg;
    s.correction = start.correction;

    // The viewport IS the picture (cover-fit always fills the frame), so the
    // camera is built for a frame of the viewport's own size.
    const long long w = std::llround(layout.viewport.w);
    const long long h = std::llround(layout.viewport.h);
    if (w <= 0 || h <= 0 || w > kMaxOverlayEdge || h > kMaxOverlayEdge) {
        return SphereGrab{};
    }
    const int wi = static_cast<int>(w);
    const int hi = static_cast<int>(h);
    const ViewSetup view = buildView(s, wi, hi, SizePx{wi, hi});
    if (!view.valid) {
        return SphereGrab{};
    }
    return beginSphereGrab(view.params.projection, view.params.focalPx, view.params.eyeOffset, view.params.tanHalfH,
                           view.params.tanHalfV, layout, start, anchor);
}

/// Commit the parameters a drag changed, using the documented CHANGED_VALUE
/// route described in this file's header comment.
///
/// `changed` is the bit field from `changedFieldsFor()`, so ONLY the
/// parameters that actually moved are marked - marking an untouched
/// parameter would make the host record a spurious keyframe on it.
///
/// Returns true when at least one value was written, which is what tells the
/// caller to set PF_EO_HANDLED_EVENT.  A null array writes nothing and
/// returns false.
///
/// `aspect` (width / height of the frame the overlay draws on) is only used
/// to refresh DJI's Zoom read-out after a DJI zoom; 0 skips that refresh.
[[nodiscard]] bool commitValues(PF_ParamDef* params[], const CameraValues& values, std::uint32_t changed,
                                double aspect) noexcept {
    if (!params || changed == kChangedNone) {
        return false;
    }
    bool wrote = false;

    // Pan: an unbounded angle dial, stored as 16.16 degrees.
    if ((changed & kChangedPan) != 0u && params[kIndexPan]) {
        params[kIndexPan]->u.ad.value = degToFixed(values.panDeg);
        params[kIndexPan]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        wrote = true;
    }

    // Tilt: already clamped to +-90 by sanitise() in the interaction core.
    if ((changed & kChangedTilt) != 0u && params[kIndexTilt]) {
        params[kIndexTilt]->u.ad.value = degToFixed(values.tiltDeg);
        params[kIndexTilt]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        wrote = true;
    }

    if ((changed & kChangedRoll) != 0u && params[kIndexRoll]) {
        params[kIndexRoll]->u.ad.value = degToFixed(values.rollDeg);
        params[kIndexRoll]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        wrote = true;
    }

    // FOV: a float slider, already clamped to the VALID range (not the
    // slider range) by sanitise().
    if ((changed & kChangedFov) != 0u && params[kIndexFov]) {
        params[kIndexFov]->u.fs_d.value = static_cast<PF_FpShort>(values.fovDeg);
        params[kIndexFov]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        wrote = true;
    }

    // [WP-CAMERA] DJI's lens: the zoom gesture moves both controls.
    bool djiLensMoved = false;
    if ((changed & kChangedDjiFov) != 0u && params[kIndexDjiFov]) {
        params[kIndexDjiFov]->u.fs_d.value = static_cast<PF_FpShort>(values.djiFovDeg);
        params[kIndexDjiFov]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        wrote = true;
        djiLensMoved = true;
    }
    if ((changed & kChangedCorrection) != 0u && params[kIndexCorrection]) {
        params[kIndexCorrection]->u.fs_d.value = static_cast<PF_FpShort>(values.correction);
        params[kIndexCorrection]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        wrote = true;
        djiLensMoved = true;
    }
    // The Zoom read-out belongs to the lens: refresh it with the lens so the
    // Effect Controls never show a stale Zoom after an overlay zoom.  Zoom is
    // not animatable, so this records no keyframe.  The shape is the frame's
    // (the overlay's viewport), which is what Match Sequence frames for.
    if (djiLensMoved && params[kIndexZoom] && aspect > 0.0) {
        const double zoom = djiZoomDeg(DjiLens{values.djiFovDeg, values.correction}, aspect);
        if (std::isfinite(zoom)) {
            params[kIndexZoom]->u.fs_d.value = static_cast<PF_FpShort>(zoom);
            params[kIndexZoom]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
        }
    }

    return wrote;
}

// ---------------------------------------------------------------------------
//  Where the picture is
// ---------------------------------------------------------------------------

/// The viewport rectangle the overlay draws over, in frame pixels.
///
/// This is now simply the WHOLE frame, and the parameter array is no longer
/// consulted at all.
///
/// It used to read the "Output Aspect" popup and reproduce the effect's
/// letterbox, so the crosshair and the corner grips sat on the picture rather
/// than floating in the black bars.  With the letterbox gone - the reframe
/// camera fills the output frame whatever shape it is (see
/// ReframeParams::computeViewport) - the picture IS the frame, and the
/// overlay must cover all of it or the grips would stop short of the edges.
///
/// The function is kept rather than inlined at its call sites because it is
/// the single statement of "where the picture is" for the UI, and it is the
/// place a future sub-rectangle render would have to be mirrored.
///
/// A degenerate frame yields a zero rectangle and, through computeLayout(),
/// an invalid layout that every consumer already handles.
[[nodiscard]] RectF viewportForFrame(PF_ParamDef* params[], int frameW, int frameH) noexcept {
    // The popup no longer affects where the picture is drawn, so it is not
    // read; the parameter is still taken so every caller keeps its shape.
    (void)params;
    if (frameW <= 0 || frameH <= 0) {
        return RectF{};
    }
    const Viewport vp = computeViewport(frameW, frameH);
    return RectF{static_cast<double>(vp.x), static_cast<double>(vp.y), static_cast<double>(vp.w),
                 static_cast<double>(vp.h)};
}

// ---------------------------------------------------------------------------
//  The frame geometry cache
//
//  THE PROBLEM THIS SOLVES.  Only PF_Event_DRAW is told how big the drawing
//  area is, and in Premiere that information arrives ONLY as the draw
//  event's update rect, because in_data->width/height are zero for a
//  comp-window custom UI.  PF_Event_DO_CLICK, PF_Event_DRAG and
//  PF_Event_ADJUST_CURSOR carry a pointer position and nothing else.
//
//  Before this cache existed those three handlers built their layout from
//  in_data->width/height directly, which in Premiere meant a 0x0 viewport,
//  an INVALID layout, and an early return on every single one of them.  The
//  visible symptom was exactly what the user reported: the HUD drew (the
//  draw path had the update-rect fallback) but nothing could be grabbed,
//  because onDoClick() bailed before it ever set `send_drag` and so no
//  PF_Event_DRAG was ever delivered.  The HUD was a picture of a control
//  rather than a control.
//
//  So the last geometry a draw event established is remembered here and the
//  pointer handlers read it.  That is sound because the host always paints a
//  view before the user can point at it: a repaint necessarily precedes the
//  first click, and any resize repaints before the pointer can act on the
//  new size.
//
//  It is stored as a single 64-bit-word-per-field POD behind a mutex rather
//  than as atomics, because the four fields must be read as ONE consistent
//  snapshot - a torn read mixing a new width with an old origin would offset
//  every drag of that gesture.  The critical section is four loads and is
//  not contended in practice (one UI thread), so a mutex costs nothing
//  measurable and buys exact consistency.
// ---------------------------------------------------------------------------

/// The cache and its guard.  A function-local static for the same
/// initialisation-order reasons as dragTable().
struct FrameGeometryCache {
    std::mutex mutex;
    FrameGeometry geometry;
};

[[nodiscard]] FrameGeometryCache& frameGeometryCache() noexcept {
    static FrameGeometryCache cache;
    return cache;
}

/// Publish the geometry a draw event established.
///
/// Rejects anything that is not a usable frame, so a single malformed draw
/// event cannot poison the cache for the pointer handlers that follow: a
/// bad update rect leaves the last GOOD geometry in place, which is far
/// better than invalidating a working overlay.
void publishFrameGeometry(const FrameGeometry& geometry) noexcept {
    if (!geometry.valid) {
        return;
    }
    if (!(std::isfinite(geometry.originX) && std::isfinite(geometry.originY) && std::isfinite(geometry.width) &&
          std::isfinite(geometry.height))) {
        return;
    }
    if (!(geometry.width > 0.0) || !(geometry.height > 0.0)) {
        return;
    }
    FrameGeometryCache& cache = frameGeometryCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.geometry = geometry;
}

/// The last geometry a draw event established, or an invalid one when no
/// draw has happened yet.
[[nodiscard]] FrameGeometry currentFrameGeometry() noexcept {
    FrameGeometryCache& cache = frameGeometryCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    return cache.geometry;
}

/// Work out the frame geometry for an event, preferring what the host says
/// directly and falling back to the cache.
///
/// `hostW`/`hostH` are in_data->width/height. When the host fills them in
/// (After Effects does; Premiere does not) they are the authority and the
/// origin is zero, because a layer-sized custom UI starts at the layer's own
/// top-left. When they are zero the cached geometry from the last repaint is
/// used instead, origin included.
[[nodiscard]] FrameGeometry resolveFrameGeometry(int hostW, int hostH) noexcept {
    if (hostW > 0 && hostH > 0 && hostW <= kMaxOverlayEdge && hostH <= kMaxOverlayEdge) {
        FrameGeometry geometry;
        geometry.originX = 0.0;
        geometry.originY = 0.0;
        geometry.width = static_cast<double>(hostW);
        geometry.height = static_cast<double>(hostH);
        geometry.valid = true;
        return geometry;
    }
    return currentFrameGeometry();
}

/// Convert a point the host gave in WINDOW coordinates into frame pixels.
///
/// The host supplies `frame_to_source` / `source_to_frame` in
/// PF_EventCallbacks (AE_EffectUI.h:474-475) but they map between the
/// LAYER's source space and the composition frame, which is not what we
/// need: the overlay works in the coordinates of the rendered output frame.
///
/// The transform is a translation by the frame's window-space origin. It
/// USED to be hard-coded as the identity, which was correct only as long as
/// the layout came from the layer size (implicitly rooted at 0,0). Now that
/// the layout can come from a draw event's update rect - a rectangle in
/// WINDOW coordinates that need not start at the origin - the same identity
/// would offset every drag by the rect's top-left corner. The maths lives in
/// ReframeUi.cpp so the tests pin it.
[[nodiscard]] PointF windowToFramePoint(const FrameGeometry& geometry, const PF_Point& point) noexcept {
    return windowToFrame(geometry, PointF{static_cast<double>(point.h), static_cast<double>(point.v)});
}

// ---------------------------------------------------------------------------
//  Asking for a repaint
//
//  WHY THIS IS NEEDED.  Committing a value with PF_ChangeFlag_CHANGED_VALUE
//  makes the host re-RENDER the frame, but a render is not a repaint of the
//  custom UI: the HUD is drawn in a separate PF_Event_DRAW pass and the host
//  has no reason to think that pass is stale just because a parameter moved.
//  Without an explicit request the readout would keep showing the numbers
//  from whenever the view last happened to be invalidated, and the hover
//  highlight would never light up at all - the overlay would look frozen
//  while the picture underneath it moved.
//
//  PF_InvalidateRect (AE_EffectSuites.h:588-594) is the documented mechanism
//  and its contract is quoted there: "Use it to invalidate rect of current
//  window being drawn... Specify PF_EO_UPDATE_NOW out flag to update the
//  window immediately after the event returns. Specify rectP0 as NULL to
//  invalidate the whole window. Only valid while handling an NON-DRAW event
//  in the effect."
//
//  That last sentence is why this is never called from onDraw(): doing so
//  during a draw is explicitly out of contract and is the classic way to get
//  a host into a repaint loop that pegs a core.
//
//  A null rect (the whole window) rather than a computed dirty rectangle:
//  the HUD spans the entire picture - crosshair at the centre, grips at four
//  corners, readout at the top-left - so any honest dirty rect is very
//  nearly the whole view anyway, and getting it slightly wrong leaves
//  visible smears of stale ink.
// ---------------------------------------------------------------------------

/// Ask the host to repaint the custom UI, and to do it as soon as this
/// event returns.
///
/// Every failure is silent and harmless: without the suite or the function
/// the overlay simply refreshes on the host's own schedule, which is what it
/// did before. It must only be called while handling a NON-DRAW event.
void requestOverlayRepaint(PF_InData* in_data, PF_EventExtra* extra) noexcept {
    if (!in_data || !extra || !extra->contextH) {
        return;
    }
    SPBasicSuite* basic = in_data->pica_basicP;
    if (!basic) {
        return;
    }
    osv::premiere::SuiteHandle<PFAppSuite6> app;
    if (app.acquire(basic, kPFAppSuite, kPFAppSuiteVersion6) && app->PF_InvalidateRect) {
        // NULL rect: invalidate the whole window (see the note above).
        (void)app->PF_InvalidateRect(extra->contextH, nullptr);
    }

    // Set the out-flag whether or not the invalidate succeeded. It is the
    // half of the contract that turns "repaint eventually, during idle" into
    // "repaint the moment this event returns", and on a host that refreshes
    // on its own it is simply redundant rather than harmful.
    extra->evt_out_flags |= PF_EO_UPDATE_NOW;
}

/// The layout for a geometry: the one call every handler makes, so all four
/// of them place the handles identically by construction.
///
/// The viewport is expressed in FRAME coordinates (top-left at 0,0), which
/// is the space `windowToFrame()` maps a pointer into - so the layout and
/// the pointer always agree without either of them knowing the origin.
[[nodiscard]] Layout layoutForGeometry(PF_ParamDef* params[], const FrameGeometry& geometry) noexcept {
    if (!geometry.valid) {
        return Layout{};
    }
    // The double->int narrowing is guarded: the cache only ever holds finite
    // positive sizes below kMaxOverlayEdge, but the conversion is checked
    // anyway so a future writer cannot make this a silent truncation.
    if (!(geometry.width > 0.0) || !(geometry.height > 0.0) || geometry.width > static_cast<double>(kMaxOverlayEdge) ||
        geometry.height > static_cast<double>(kMaxOverlayEdge)) {
        return Layout{};
    }
    const int frameW = static_cast<int>(geometry.width);
    const int frameH = static_cast<int>(geometry.height);
    return computeLayout(viewportForFrame(params, frameW, frameH));
}

// ---------------------------------------------------------------------------
//  DrawBot
// ---------------------------------------------------------------------------

/// Every DrawBot suite one repaint needs, acquired together and released
/// together.
///
/// Five suites rather than the four AEFX_AcquireDrawbotSuites takes: the Pen
/// suite is acquired as well so a future dashed stroke works, and every one
/// of them is OPTIONAL.  A host that serves an older DrawBot can leave any
/// member null and the drawing code checks each pointer before it calls
/// through it, so the HUD degrades element by element instead of vanishing.
///
/// `ok()` reports only the two that nothing can be drawn without: the Draw
/// suite (which hands out the supplier and the surface) and the Surface
/// suite (which strokes).
class DrawbotBundle {
public:
    /// Acquire everything from the host's SPBasicSuite.  A null basic suite
    /// leaves every member null and ok() false.
    explicit DrawbotBundle(SPBasicSuite* basic) noexcept {
        if (!basic) {
            return;
        }
        (void)m_draw.acquire(basic, kDRAWBOT_DrawSuite, kDRAWBOT_DrawSuite_VersionCurrent);
        (void)m_supplier.acquire(basic, kDRAWBOT_SupplierSuite, kDRAWBOT_SupplierSuite_VersionCurrent);
        (void)m_surface.acquire(basic, kDRAWBOT_SurfaceSuite, kDRAWBOT_SurfaceSuite_VersionCurrent);
        (void)m_path.acquire(basic, kDRAWBOT_PathSuite, kDRAWBOT_PathSuite_VersionCurrent);
        (void)m_pen.acquire(basic, kDRAWBOT_PenSuite, kDRAWBOT_PenSuite_VersionCurrent);

        // The bundle the drawing code reads.  The const is cast away because
        // Adobe's own DRAWBOT_Suites holds non-const pointers while
        // SPBasicSuite::AcquireSuite hands back a const void*; nothing here
        // ever writes through them.
        m_suites.drawbot_suiteP = const_cast<DRAWBOT_DrawbotSuiteCurrent*>(m_draw.get());
        m_suites.supplier_suiteP = const_cast<DRAWBOT_SupplierSuiteCurrent*>(m_supplier.get());
        m_suites.surface_suiteP = const_cast<DRAWBOT_SurfaceSuiteCurrent*>(m_surface.get());
        m_suites.path_suiteP = const_cast<DRAWBOT_PathSuiteCurrent*>(m_path.get());
        m_suites.pen_suiteP = const_cast<DRAWBOT_PenSuiteCurrent*>(m_pen.get());
        m_suites.image_suiteP = nullptr;  // The HUD draws no images.
    }

    DrawbotBundle(const DrawbotBundle&) = delete;
    DrawbotBundle& operator=(const DrawbotBundle&) = delete;

    /// Whether the two indispensable suites are present.
    [[nodiscard]] bool ok() const noexcept { return m_suites.drawbot_suiteP != nullptr && m_suites.surface_suiteP != nullptr; }

    [[nodiscard]] const DRAWBOT_Suites& suites() const noexcept { return m_suites; }

private:
    osv::premiere::SuiteHandle<DRAWBOT_DrawbotSuiteCurrent> m_draw;
    osv::premiere::SuiteHandle<DRAWBOT_SupplierSuiteCurrent> m_supplier;
    osv::premiere::SuiteHandle<DRAWBOT_SurfaceSuiteCurrent> m_surface;
    osv::premiere::SuiteHandle<DRAWBOT_PathSuiteCurrent> m_path;
    osv::premiere::SuiteHandle<DRAWBOT_PenSuiteCurrent> m_pen;
    DRAWBOT_Suites m_suites = {};
};

/// The colours of the HUD.
///
/// Light strokes over a dark 1px outline: the overlay has to stay legible
/// over a blown-out sky and over a night shot, and an outline underneath is
/// the cheapest way to get that without a filled panel (which the design
/// brief rules out).  Alpha is kept well below 1 so the HUD never competes
/// with the picture.
constexpr DRAWBOT_ColorRGBA kInkLight = {1.0f, 1.0f, 1.0f, 0.78f};
constexpr DRAWBOT_ColorRGBA kInkShadow = {0.0f, 0.0f, 0.0f, 0.55f};
constexpr DRAWBOT_ColorRGBA kInkHot = {1.0f, 0.78f, 0.20f, 0.95f};  ///< A highlighted handle.

/// The DrawBot objects one draw pass needs, released in reverse order.
///
/// A tiny RAII holder rather than raw calls: every DrawBot object must be
/// released through ReleaseObject, and an early return on a failed call
/// would otherwise leak one per repaint - which, at monitor refresh rate,
/// is a leak that matters.
class DrawScope {
public:
    DrawScope(const DRAWBOT_Suites& suites, DRAWBOT_SupplierRef supplier) noexcept
        : m_suites(suites), m_supplier(supplier) {}

    DrawScope(const DrawScope&) = delete;
    DrawScope& operator=(const DrawScope&) = delete;

    ~DrawScope() { releaseAll(); }

    /// Create a pen, remembering it for release.  Returns null on failure,
    /// which every caller checks.
    [[nodiscard]] DRAWBOT_PenRef newPen(const DRAWBOT_ColorRGBA& colour, float width) noexcept {
        if (!m_suites.supplier_suiteP || !m_suites.supplier_suiteP->NewPen || !m_supplier) {
            return nullptr;
        }
        DRAWBOT_PenRef pen = nullptr;
        if (m_suites.supplier_suiteP->NewPen(m_supplier, &colour, width, &pen) != kSPNoError || !pen) {
            return nullptr;
        }
        track(reinterpret_cast<DRAWBOT_ObjectRef>(pen));
        return pen;
    }

    /// Create a brush (used only for text).
    [[nodiscard]] DRAWBOT_BrushRef newBrush(const DRAWBOT_ColorRGBA& colour) noexcept {
        if (!m_suites.supplier_suiteP || !m_suites.supplier_suiteP->NewBrush || !m_supplier) {
            return nullptr;
        }
        DRAWBOT_BrushRef brush = nullptr;
        if (m_suites.supplier_suiteP->NewBrush(m_supplier, &colour, &brush) != kSPNoError || !brush) {
            return nullptr;
        }
        track(reinterpret_cast<DRAWBOT_ObjectRef>(brush));
        return brush;
    }

    /// Create an empty path.
    [[nodiscard]] DRAWBOT_PathRef newPath() noexcept {
        if (!m_suites.supplier_suiteP || !m_suites.supplier_suiteP->NewPath || !m_supplier) {
            return nullptr;
        }
        DRAWBOT_PathRef path = nullptr;
        if (m_suites.supplier_suiteP->NewPath(m_supplier, &path) != kSPNoError || !path) {
            return nullptr;
        }
        track(reinterpret_cast<DRAWBOT_ObjectRef>(path));
        return path;
    }

    /// Create the default font at the supplier's default size, or null when
    /// the supplier does not do text at all (which is legal - see
    /// SupportsText - and simply means no readout).
    [[nodiscard]] DRAWBOT_FontRef newDefaultFont(float size) noexcept {
        if (!m_suites.supplier_suiteP || !m_suites.supplier_suiteP->NewDefaultFont || !m_supplier) {
            return nullptr;
        }
        DRAWBOT_FontRef font = nullptr;
        if (m_suites.supplier_suiteP->NewDefaultFont(m_supplier, size, &font) != kSPNoError || !font) {
            return nullptr;
        }
        track(reinterpret_cast<DRAWBOT_ObjectRef>(font));
        return font;
    }

private:
    /// Remember an object for release.  Silently drops the object when the
    /// table is full, which cannot happen with the current draw pass but
    /// would be a leak rather than a crash if a future one grew.
    void track(DRAWBOT_ObjectRef obj) noexcept {
        if (m_count < kMaxObjects) {
            m_objects[m_count++] = obj;
        }
    }

    void releaseAll() noexcept {
        if (!m_suites.supplier_suiteP || !m_suites.supplier_suiteP->ReleaseObject) {
            return;
        }
        // Reverse order: DrawBot does not require it, but releasing in the
        // opposite order to creation is the habit that keeps a future
        // dependency between objects correct.
        while (m_count > 0) {
            --m_count;
            if (m_objects[m_count]) {
                (void)m_suites.supplier_suiteP->ReleaseObject(m_objects[m_count]);
                m_objects[m_count] = nullptr;
            }
        }
    }

    static constexpr int kMaxObjects = 16;
    const DRAWBOT_Suites& m_suites;
    DRAWBOT_SupplierRef m_supplier = nullptr;
    DRAWBOT_ObjectRef m_objects[kMaxObjects] = {};
    int m_count = 0;
};

/// Stroke a path twice: once in the dark shadow colour offset by one pixel,
/// then in the light ink on top.
///
/// This is the whole legibility strategy.  A single white 1px line vanishes
/// over a white wall; the same line with a black line one pixel below and
/// right of it stays visible over anything, and costs one extra stroke.
void strokeWithShadow(const DRAWBOT_Suites& suites, DRAWBOT_SurfaceRef surface, DRAWBOT_PathRef path,
                      DRAWBOT_PenRef shadowPen, DRAWBOT_PenRef inkPen) noexcept {
    if (!suites.surface_suiteP || !suites.surface_suiteP->StrokePath || !surface || !path) {
        return;
    }
    // The shadow is drawn by translating the surface rather than by building
    // a second, offset path: one transform is cheaper than duplicating every
    // vertex, and it keeps the two strokes provably the same shape.
    if (shadowPen && suites.surface_suiteP->PushStateStack && suites.surface_suiteP->PopStateStack &&
        suites.surface_suiteP->Transform) {
        if (suites.surface_suiteP->PushStateStack(surface) == kSPNoError) {
            DRAWBOT_MatrixF32 offset = {};
            offset.mat[0][0] = 1.0f;
            offset.mat[1][1] = 1.0f;
            offset.mat[2][2] = 1.0f;
            offset.mat[2][0] = 1.0f;  // +1px in x
            offset.mat[2][1] = 1.0f;  // +1px in y
            (void)suites.surface_suiteP->Transform(surface, &offset);
            (void)suites.surface_suiteP->StrokePath(surface, shadowPen, path);
            (void)suites.surface_suiteP->PopStateStack(surface);
        }
    }
    if (inkPen) {
        (void)suites.surface_suiteP->StrokePath(surface, inkPen, path);
    }
}

/// Append a straight segment to a path.  Null-safe.
void addLine(const DRAWBOT_Suites& suites, DRAWBOT_PathRef path, double x0, double y0, double x1, double y1) noexcept {
    if (!suites.path_suiteP || !suites.path_suiteP->MoveTo || !suites.path_suiteP->LineTo || !path) {
        return;
    }
    (void)suites.path_suiteP->MoveTo(path, static_cast<float>(x0), static_cast<float>(y0));
    (void)suites.path_suiteP->LineTo(path, static_cast<float>(x1), static_cast<float>(y1));
}

/// Append an open rectangle outline to a path (four segments, closed).
void addRectOutline(const DRAWBOT_Suites& suites, DRAWBOT_PathRef path, const RectF& r) noexcept {
    if (!suites.path_suiteP || !suites.path_suiteP->AddRect || !path) {
        return;
    }
    const DRAWBOT_RectF32 rect = {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                                  static_cast<float>(r.h)};
    (void)suites.path_suiteP->AddRect(path, &rect);
}

/// UTF-8 (ASCII only, which is all the readout produces) -> the UTF-16 that
/// DrawString wants.
///
/// Truncates rather than overflowing; `out` must have room for `capacity`
/// units including the terminator.  Returns false when nothing usable could
/// be produced, which simply means the readout is skipped.
[[nodiscard]] bool toDrawbotString(const char* text, DRAWBOT_UTF16Char* out, int capacity) noexcept {
    if (!text || !out || capacity <= 1) {
        return false;
    }
    int i = 0;
    for (; text[i] != '\0' && i < capacity - 1; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        // Anything outside ASCII would need a real conversion; the readout
        // is built from digits and Latin letters only, so a non-ASCII byte
        // means the buffer was corrupted and the string is abandoned.
        if (c >= 0x80u) {
            return false;
        }
        out[i] = static_cast<DRAWBOT_UTF16Char>(c);
    }
    out[i] = 0;
    return i > 0;
}

/// Draw one line of text with the same shadow-then-ink treatment as the
/// strokes.  Does nothing when the supplier has no text support.
void drawTextWithShadow(const DRAWBOT_Suites& suites, DRAWBOT_SurfaceRef surface, DRAWBOT_FontRef font,
                        DRAWBOT_BrushRef shadowBrush, DRAWBOT_BrushRef inkBrush, const char* text, double x,
                        double y) noexcept {
    if (!suites.surface_suiteP || !suites.surface_suiteP->DrawString || !surface || !font || !text) {
        return;
    }
    DRAWBOT_UTF16Char buffer[128] = {};
    if (!toDrawbotString(text, buffer, 128)) {
        return;
    }
    if (shadowBrush) {
        const DRAWBOT_PointF32 shadowOrigin = {static_cast<float>(x + 1.0), static_cast<float>(y + 1.0)};
        (void)suites.surface_suiteP->DrawString(surface, shadowBrush, font, buffer, &shadowOrigin,
                                               kDRAWBOT_TextAlignment_Left, kDRAWBOT_TextTruncation_None, 0.0f);
    }
    if (inkBrush) {
        const DRAWBOT_PointF32 origin = {static_cast<float>(x), static_cast<float>(y)};
        (void)suites.surface_suiteP->DrawString(surface, inkBrush, font, buffer, &origin, kDRAWBOT_TextAlignment_Left,
                                               kDRAWBOT_TextTruncation_None, 0.0f);
    }
}

/// Draw the whole HUD.
///
/// Every element is optional in the sense that a failed DrawBot call skips
/// it and the rest still draws: a partial HUD is far better than none, and
/// there is nothing to roll back because nothing outside the surface was
/// touched.
///
/// `hot` is the handle the cursor is currently over (Handle::None for
/// nothing), which is highlighted so the user can see what they are about to
/// grab before they press the button.
void drawOverlay(const DRAWBOT_Suites& suites, DRAWBOT_DrawRef drawRef, const Layout& layout,
                 const CameraValues& values, Handle hot) noexcept {
    if (!layout.valid || !drawRef) {
        return;
    }
    if (!suites.drawbot_suiteP || !suites.drawbot_suiteP->GetSupplier || !suites.drawbot_suiteP->GetSurface) {
        return;
    }

    DRAWBOT_SupplierRef supplier = nullptr;
    DRAWBOT_SurfaceRef surface = nullptr;
    if (suites.drawbot_suiteP->GetSupplier(drawRef, &supplier) != kSPNoError || !supplier) {
        return;
    }
    if (suites.drawbot_suiteP->GetSurface(drawRef, &surface) != kSPNoError || !surface) {
        return;
    }

    DrawScope scope(suites, supplier);

    // Anti-aliasing on: a 1px arc without it is a staircase, and the HUD is
    // small enough that the cost is irrelevant.  Failure is ignored - the
    // HUD simply looks slightly rougher.
    if (suites.surface_suiteP && suites.surface_suiteP->SetAntiAliasPolicy) {
        (void)suites.surface_suiteP->SetAntiAliasPolicy(surface, kDRAWBOT_AntiAliasPolicy_High);
    }

    DRAWBOT_PenRef shadowPen = scope.newPen(kInkShadow, 1.0f);
    DRAWBOT_PenRef inkPen = scope.newPen(kInkLight, 1.0f);
    DRAWBOT_PenRef hotPen = scope.newPen(kInkHot, 1.6f);
    if (!inkPen) {
        // Without the main pen there is nothing worth drawing; the shadow
        // alone would be a black smear.
        return;
    }

    // ---- the centre crosshair -------------------------------------------
    // Deliberately a small cross with a GAP in the middle rather than two
    // full lines: the exact centre of the frame is the one pixel a user is
    // most likely to be judging, and covering it with our own ink is the
    // classic way to make a framing overlay useless.
    if (DRAWBOT_PathRef cross = scope.newPath()) {
        const double arm = std::max(6.0, 0.02 * std::min(layout.viewport.w, layout.viewport.h));
        const double gap = arm * 0.35;
        const PointF c = layout.centre;
        addLine(suites, cross, c.x - arm, c.y, c.x - gap, c.y);
        addLine(suites, cross, c.x + gap, c.y, c.x + arm, c.y);
        addLine(suites, cross, c.x, c.y - arm, c.x, c.y - gap);
        addLine(suites, cross, c.x, c.y + gap, c.x, c.y + arm);
        strokeWithShadow(suites, surface, cross, shadowPen, inkPen);
    }

    // ---- the roll ring arcs ---------------------------------------------
    // Two symmetric arcs straddling the horizontal, exactly where
    // onRollArc() says the grab band is.  The drawn shape and the hit target
    // come from the same constants, so they cannot drift apart.
    if (suites.path_suiteP && suites.path_suiteP->AddArc) {
        if (DRAWBOT_PathRef ring = scope.newPath()) {
            const DRAWBOT_PointF32 centre = {static_cast<float>(layout.centre.x), static_cast<float>(layout.centre.y)};
            const float radius = static_cast<float>(layout.rollRingRadius);
            const float sweep = static_cast<float>(2.0 * kRollArcHalfSweepDeg);
            // DrawBot arcs: 0 degrees at 3 o'clock, sweeping clockwise, so
            // the right-hand arc starts at -halfSweep and the left-hand one
            // at 180 - halfSweep.
            (void)suites.path_suiteP->AddArc(ring, &centre, radius, static_cast<float>(-kRollArcHalfSweepDeg), sweep);
            (void)suites.path_suiteP->AddArc(ring, &centre, radius, static_cast<float>(180.0 - kRollArcHalfSweepDeg),
                                             sweep);
            strokeWithShadow(suites, surface, ring, shadowPen, (hot == Handle::Roll && hotPen) ? hotPen : inkPen);
        }
    }

    // ---- the four FOV corner grips --------------------------------------
    // Drawn as two short strokes per corner (an "L") rather than a closed
    // box: a box reads as a crop rectangle, an L reads as a resize grip,
    // which is what it is.
    RectF grips[4];
    if (fovGripRects(layout, grips) == 4) {
        if (DRAWBOT_PathRef gripPath = scope.newPath()) {
            const double len = layout.fovGripSize * 0.55;
            // Top-left: arms point right and down.
            addLine(suites, gripPath, grips[0].x, grips[0].y, grips[0].x + len, grips[0].y);
            addLine(suites, gripPath, grips[0].x, grips[0].y, grips[0].x, grips[0].y + len);
            // Top-right: left and down.
            const double trx = grips[1].x + grips[1].w;
            addLine(suites, gripPath, trx, grips[1].y, trx - len, grips[1].y);
            addLine(suites, gripPath, trx, grips[1].y, trx, grips[1].y + len);
            // Bottom-left: right and up.
            const double bly = grips[2].y + grips[2].h;
            addLine(suites, gripPath, grips[2].x, bly, grips[2].x + len, bly);
            addLine(suites, gripPath, grips[2].x, bly, grips[2].x, bly - len);
            // Bottom-right: left and up.
            const double brx = grips[3].x + grips[3].w;
            const double bry = grips[3].y + grips[3].h;
            addLine(suites, gripPath, brx, bry, brx - len, bry);
            addLine(suites, gripPath, brx, bry, brx, bry - len);
            strokeWithShadow(suites, surface, gripPath, shadowPen, (hot == Handle::Fov && hotPen) ? hotPen : inkPen);
        }
    }

    // ---- the readout ----------------------------------------------------
    // Four numbers in the top-left of the picture, far enough in not to be
    // clipped by the monitor's own chrome.  Text is optional: a supplier
    // that reports no text support simply gets the graphics.
    DRAWBOT_Boolean supportsText = 0;
    if (suites.supplier_suiteP && suites.supplier_suiteP->SupportsText) {
        (void)suites.supplier_suiteP->SupportsText(supplier, &supportsText);
    }
    if (supportsText) {
        float fontSize = 11.0f;
        if (suites.supplier_suiteP->GetDefaultFontSize) {
            (void)suites.supplier_suiteP->GetDefaultFontSize(supplier, &fontSize);
        }
        DRAWBOT_FontRef font = scope.newDefaultFont(fontSize);
        DRAWBOT_BrushRef inkBrush = scope.newBrush(kInkLight);
        DRAWBOT_BrushRef shadowBrush = scope.newBrush(kInkShadow);
        if (font && inkBrush) {
            // snprintf, not std::format: this runs on the UI thread inside a
            // repaint and must not allocate.
            char line[128] = {};
            int written = 0;
            if (values.dji) {
                // [WP-CAMERA] DJI's lens: the three numbers DJI Studio shows,
                // in its order and precision (Zoom and FOV to a tenth of a
                // degree, Correction Angle to a hundredth), so a framing can
                // be read straight across.  Zoom is derived for the shape of
                // the picture on screen, exactly as DJI derives it for its
                // canvas.
                const double aspect = (layout.viewport.h > 0.0) ? layout.viewport.w / layout.viewport.h : 0.0;
                const double zoom = djiZoomDeg(DjiLens{values.djiFovDeg, values.correction}, aspect);
                written = std::snprintf(line, sizeof(line),
                                        "Pan %.1f  Tilt %.1f  Roll %.1f  Zoom %.1f  FOV %.1f  Correction %.2f",
                                        values.panDeg, values.tiltDeg, values.rollDeg, zoom, values.djiFovDeg,
                                        values.correction);
            } else {
                written = std::snprintf(line, sizeof(line), "Pan %.1f  Tilt %.1f  Roll %.1f  FOV %.1f",
                                        values.panDeg, values.tiltDeg, values.rollDeg, values.fovDeg);
            }
            if (written > 0 && static_cast<std::size_t>(written) < sizeof(line)) {
                const double margin = std::max(8.0, 0.02 * layout.viewport.w);
                drawTextWithShadow(suites, surface, font, shadowBrush, inkBrush, line, layout.viewport.x + margin,
                                   layout.viewport.y + margin + static_cast<double>(fontSize));
            }
        }
    }

    // Flush so the HUD appears even if the host batches; failure is
    // harmless because the host flushes at the end of the repaint anyway.
    if (suites.surface_suiteP && suites.surface_suiteP->Flush) {
        (void)suites.surface_suiteP->Flush(surface);
    }
}

// ---------------------------------------------------------------------------
//  Event handlers
// ---------------------------------------------------------------------------

/// PF_Event_DRAW: acquire DrawBot, get the drawing reference, draw.
///
/// A missing suite or a null drawing reference is logged ONCE (via
/// PluginLog::oncef, which dedupes by key) and then silently degrades to no
/// overlay forever after - an effect that logged a line per repaint would
/// fill a disk in minutes.
PF_Err onDraw(PF_InData* in_data, PF_ParamDef* params[], PF_EventExtra* extra) noexcept {
    if (!in_data || !extra || !extra->contextH) {
        return PF_Err_NONE;
    }
    // PF_EI_DONT_DRAW is the host saying "controls are hidden right now"
    // (the user pressed the hide-overlays shortcut).  Honouring it is not
    // optional: drawing anyway puts our HUD on an exported frame preview.
    if ((extra->evt_in_flags & PF_EI_DONT_DRAW) != 0) {
        return PF_Err_NONE;
    }

    // How big is the thing we are drawing on?
    //
    // in_data->width/height are the LAYER's dimensions, which is what an AE
    // comp-window custom UI is normally sized against.  Premiere Pro 26.2
    // hands us zeroes there - a session log shows "degenerate frame 0x0" on
    // every draw - so the overlay bailed before it acquired a single suite.
    //
    // PF_DrawEventInfo::update_rect (AE_EffectUI.h:284) is the area the host
    // is asking us to repaint, in the window's own coordinates, and it is
    // filled for a comp-window draw.  It is used only as a FALLBACK: when the
    // layer size is sane it remains the authority, because update_rect can be
    // a partial invalidation rather than the whole frame.
    FrameGeometry geometry;
    if (in_data->width > 0 && in_data->height > 0 && in_data->width <= kMaxOverlayEdge &&
        in_data->height <= kMaxOverlayEdge) {
        // After Effects fills these in and they are the authority: a
        // layer-sized custom UI is rooted at the layer's own top-left, so
        // the origin is zero and window space IS frame space.
        geometry.originX = 0.0;
        geometry.originY = 0.0;
        geometry.width = static_cast<double>(in_data->width);
        geometry.height = static_cast<double>(in_data->height);
        geometry.valid = true;
    } else {
        // Premiere's comp-window path. The update rect is the only size the
        // host offers, and unlike the layer size it has an ORIGIN as well -
        // it is expressed in the window's coordinate system
        // (AE_EffectUI.h:284). Both are captured, because the origin is what
        // `windowToFrame()` subtracts from a screen_point to put a click in
        // the same space as the layout.
        const PF_UnionableRect& r = extra->u.draw.update_rect;
        const long rectW = static_cast<long>(r.right) - static_cast<long>(r.left);
        const long rectH = static_cast<long>(r.bottom) - static_cast<long>(r.top);
        // Guard the subtraction: an empty or inverted rect is not a frame,
        // and kMaxOverlayEdge keeps a nonsense value out of the geometry.
        if (rectW > 0 && rectH > 0 && rectW <= kMaxOverlayEdge && rectH <= kMaxOverlayEdge) {
            geometry.originX = static_cast<double>(r.left);
            geometry.originY = static_cast<double>(r.top);
            geometry.width = static_cast<double>(rectW);
            geometry.height = static_cast<double>(rectH);
            geometry.valid = true;
            (void)PluginLog::oncef("reframe.ui.updaterect", PluginLog::Level::Info,
                                   "reframe ui: the host reports a {}x{} layer; drawing against the "
                                   "event's {}x{} update rect at origin {},{} instead",
                                   in_data->width, in_data->height, rectW, rectH,
                                   static_cast<int>(r.left), static_cast<int>(r.top));
        }
    }

    const Layout layout = layoutForGeometry(params, geometry);
    if (!layout.valid) {
        // Neither source gave a usable size. This is NOT a malfunction and
        // must not be reported as one: Premiere sends draw events to
        // throwaway plug-in instances that have no window behind them at
        // all. The session log shows exactly that - one such event per
        // freshly loaded instance during the startup scan, each immediately
        // after its own GLOBAL_SETUP, and never again once a real Program
        // Monitor exists.
        //
        // PF_Context::w_type (AE_EffectUI.h:414) is what separates the two
        // cases. A comp-window context with no size is a view that is not on
        // screen yet (a hidden or collapsed panel, or a pre-roll); anything
        // else is a context this overlay never asked to draw in, because
        // registerCustomUi() requests PF_CustomEFlag_COMP alone. Both are
        // expected, so both are logged at Info - a Warn here trains the user
        // to ignore the log, which is worse than no log at all.
        long windowType = -1;
        if (extra->contextH && *extra->contextH) {
            windowType = static_cast<long>((*extra->contextH)->w_type);
        }
        (void)PluginLog::oncef("reframe.ui.viewport", PluginLog::Level::Info,
                               "reframe ui: no overlay for a sizeless context (layer {}x{}, update rect "
                               "{},{},{},{}, window type {}); this is normal for a host probe or an "
                               "off-screen view and the overlay appears as soon as a sized one draws",
                               in_data->width, in_data->height,
                               static_cast<int>(extra->u.draw.update_rect.left),
                               static_cast<int>(extra->u.draw.update_rect.top),
                               static_cast<int>(extra->u.draw.update_rect.right),
                               static_cast<int>(extra->u.draw.update_rect.bottom), windowType);
        return PF_Err_NONE;
    }

    // Publish BEFORE drawing, so that even if a DrawBot suite is missing and
    // the HUD never appears, the click handlers still know how big the view
    // is. Dragging without a visible overlay is odd but it is strictly
    // better than a monitor that ignores the mouse.
    publishFrameGeometry(geometry);

    // Every suite below comes from the host's SPBasicSuite, which the effect
    // is handed in in_data.  Without it there is nothing to acquire.
    SPBasicSuite* basic = in_data->pica_basicP;
    if (!basic) {
        (void)PluginLog::oncef("reframe.ui.basic", PluginLog::Level::Warn,
                               "reframe ui: no overlay, host provided no SPBasicSuite");
        return PF_Err_NONE;
    }

    // The custom UI suite hands out the DrawBot reference for this context.
    //
    // v2 was frozen in AE 13.5 and is what a current host should offer, but a
    // session log from Premiere Pro 26.2 shows it refusing that version -
    // "PF Effect Custom UI Suite v2 unavailable" on every draw, so no overlay
    // ever appeared.  v1 (frozen in 10.0) is therefore tried as well.
    //
    // This is safe rather than hopeful: the two structures are
    // layout-compatible where it matters.  v1 has exactly one member and v2
    // declares that SAME member first, adding only PF_GetContextAsyncManager
    // after it (AE_EffectSuitesOld.h:274, AE_EffectSuites.h:649).  The
    // overlay uses nothing but PF_GetDrawingReference, so a v1 suite read
    // through either pointer is correct; the async manager is never touched
    // and would require PF_OutFlag2_CUSTOM_UI_ASYNC_MANAGER, which this
    // effect does not set.
    osv::premiere::SuiteHandle<PF_EffectCustomUISuite2> customUi;
    osv::premiere::SuiteHandle<PF_EffectCustomUISuite1> customUiV1;
    PF_Err (*getDrawingReference)(const PF_ContextH, DRAWBOT_DrawRef*) = nullptr;

    if (customUi.acquire(basic, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion2) &&
        customUi->PF_GetDrawingReference) {
        getDrawingReference = customUi->PF_GetDrawingReference;
    } else if (customUiV1.acquire(basic, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion1) &&
               customUiV1->PF_GetDrawingReference) {
        getDrawingReference = customUiV1->PF_GetDrawingReference;
        (void)PluginLog::oncef("reframe.ui.customsuite.v1", PluginLog::Level::Info,
                               "reframe ui: the host offers no v2 custom UI suite; using v1");
    }

    if (!getDrawingReference) {
        (void)PluginLog::oncef("reframe.ui.customsuite", PluginLog::Level::Warn,
                               "reframe ui: no overlay, PF Effect Custom UI Suite unavailable at v2 or v1");
        return PF_Err_NONE;
    }

    DRAWBOT_DrawRef drawRef = nullptr;
    const PF_Err refErr = getDrawingReference(extra->contextH, &drawRef);
    if (refErr != PF_Err_NONE || !drawRef) {
        // Name the common codes.  512 is PF_Err_INTERNAL_STRUCT_DAMAGED
        // (PF_FIRST_ERR, AE_Effect.h:337) and is what a host returns when it
        // does not accept the context handle we passed - the code seen in a
        // Premiere Pro 26.2 log alongside the v2 suite being refused.
        const char* meaning = "unknown";
        switch (refErr) {
            case PF_Err_INTERNAL_STRUCT_DAMAGED: meaning = "INTERNAL_STRUCT_DAMAGED (host rejected the context)"; break;
            case PF_Err_INVALID_INDEX:           meaning = "INVALID_INDEX"; break;
            case PF_Err_INVALID_CALLBACK:        meaning = "INVALID_CALLBACK"; break;
            case PF_Err_BAD_CALLBACK_PARAM:      meaning = "BAD_CALLBACK_PARAM"; break;
            case PF_Err_OUT_OF_MEMORY:           meaning = "OUT_OF_MEMORY"; break;
            default: break;
        }
        (void)PluginLog::oncef("reframe.ui.drawref", PluginLog::Level::Warn,
                               "reframe ui: no overlay, no drawing reference (err {} = {}, context {})",
                               static_cast<int>(refErr), meaning,
                               extra->contextH ? "non-null" : "NULL");
        return PF_Err_NONE;
    }

    // The DrawBot suites themselves.  Released by the bundle's destructor on
    // every path out of this function, including the early returns above.
    const DrawbotBundle bundle(basic);
    if (!bundle.ok()) {
        (void)PluginLog::oncef("reframe.ui.drawbot", PluginLog::Level::Warn,
                               "reframe ui: no overlay, DrawBot suites unavailable");
        return PF_Err_NONE;
    }

    // Which handle is under the cursor right now, so it can be highlighted.
    // PF_Event_DRAW carries no pointer position, so the last answer from
    // PF_Event_ADJUST_CURSOR is reused; see hoverHandle().
    //
    // The numbers come through readoutValues() rather than straight from
    // `params`, so a drag's values appear the moment it commits them instead
    // of whenever the host's (possibly slow) re-render catches up; see the
    // live readout section above.
    const CameraValues shown = readoutValues(in_data->effect_ref, readCamera(params));
    drawOverlay(bundle.suites(), drawRef, layout, shown, hoverHandle());

    // The overlay was drawn, so the host must not also draw its own default
    // controls over the same pixels.
    extra->evt_out_flags |= PF_EO_HANDLED_EVENT;
    return PF_Err_NONE;
}

/// PF_Event_DO_CLICK: hit-test, start a gesture, and ask for drags.
///
/// Setting `send_drag` is what makes the host follow up with PF_Event_DRAG
/// events; without it a click is a click and nothing moves.  The gesture's
/// state is put in the drag table and referenced from `continue_refcon`.
PF_Err onDoClick(PF_InData* in_data, PF_ParamDef* params[], PF_EventExtra* extra) noexcept {
    if (!in_data || !extra || !params) {
        return PF_Err_NONE;
    }
    PF_DoClickEventInfo& click = extra->u.do_click;

    // A click event carries a pointer position and NOTHING about the size of
    // the view, so the geometry comes from the last repaint (see the frame
    // geometry cache). Building it from in_data->width/height here - which
    // is what this handler used to do - produced a 0x0 viewport in Premiere,
    // an invalid layout, and an early return on EVERY click: `send_drag` was
    // never set, the host never sent a single PF_Event_DRAG, and the overlay
    // was inert no matter how correct the drag maths underneath it was.
    const FrameGeometry geometry = resolveFrameGeometry(in_data->width, in_data->height);
    const Layout layout = layoutForGeometry(params, geometry);
    if (!layout.valid) {
        return PF_Err_NONE;  // No picture to grab; leave the click to the host.
    }

    const PointF where = windowToFramePoint(geometry, click.screen_point);
    const Handle handle = hitTest(layout, where);
    if (handle == Handle::None) {
        // Outside the picture: emphatically not ours.  PF_EO_HANDLED_EVENT
        // stays clear so the host does whatever it normally would.
        return PF_Err_NONE;
    }

    std::uint64_t generation = 0;
    const int slot = acquireDragSlot(generation);
    if (slot < 0) {
        (void)PluginLog::oncef("reframe.ui.slots", PluginLog::Level::Warn,
                               "reframe ui: no drag slot free, gesture ignored");
        return PF_Err_NONE;
    }

    DragState state;
    state.active = true;
    state.handle = handle;
    state.anchor = where;
    state.last = where;
    state.layout = layout;
    // Pin the coordinate transform for the gesture; see DragState::geometry.
    state.geometry = geometry;
    state.start = readCamera(params);
    // [WP-CAMERA] The Drag Sensitivity control, fixed for the whole gesture.
    state.sensitivity = readSensitivity(params);
    state.mode = resolveDragMode(handle, static_cast<std::uint32_t>(click.modifiers));
    state.axisLocked = false;
    // Pan / tilt drags grab the sphere: remember which direction is under
    // the pointer so the drag keeps it there (see SphereGrab).  Built for
    // every open-picture click, since a modifier can turn the gesture into a
    // pan / tilt at any point; the handles never use it.
    if (handle == Handle::PanTilt) {
        state.grab = grabForClick(params, layout, state.start, where);
    }
    // Remember where on the ring the grab happened, so a roll drag can
    // measure the angle actually swept rather than a pixel distance.
    state.startRollAngleDeg = 0.0;
    if (handle == Handle::Roll) {
        const double dx = where.x - layout.centre.x;
        const double dy = where.y - layout.centre.y;
        state.startRollAngleDeg = (dx == 0.0 && dy == 0.0) ? 0.0 : std::atan2(dy, dx) * (180.0 / 3.14159265358979323846);
    }

    writeDragSlot(slot, generation, state);
    packRefcon(click.continue_refcon, slot, generation);

    // A new gesture starts with a clean live readout for this instance, so
    // the set of touched fields describes THIS gesture only, even if the
    // host abandoned the previous one without a final drag event.
    clearLiveReadout(in_data->effect_ref);

    // Ask the host to send us the drag stream.
    click.send_drag = TRUE;

    // The click itself changes no value - a click without movement should
    // not record a keyframe - but it IS consumed, so the host does not also
    // treat it as a selection click in the monitor.
    extra->evt_out_flags |= PF_EO_HANDLED_EVENT;
    return PF_Err_NONE;
}

/// PF_Event_DRAG: move the camera and commit the values.
///
/// This is where keyframes are actually recorded; see this file's header for
/// the mechanism and why PF_UpdateParamUI is not it.
PF_Err onDrag(PF_InData* in_data, PF_ParamDef* params[], PF_EventExtra* extra) noexcept {
    if (!in_data || !extra || !params) {
        return PF_Err_NONE;
    }
    // A drag reuses the do_click union member (AE_EffectUI.h:395 "also
    // drag"), so continue_refcon and last_time are read from there.
    PF_DoClickEventInfo& drag = extra->u.do_click;

    int slot = -1;
    std::uint64_t generation = 0;
    if (!unpackRefcon(drag.continue_refcon, slot, generation)) {
        // A drag whose refcon is not ours: either a stale event or a gesture
        // that began before this module was loaded.  Ignoring it is correct;
        // acting on it would move the camera from an unknown anchor.
        return PF_Err_NONE;
    }

    DragState state;
    if (!readDragSlot(slot, generation, state) || !state.active) {
        return PF_Err_NONE;
    }

    // The geometry used here is the one captured when the gesture STARTED,
    // carried in the drag state, not the live cache. A repaint arriving
    // mid-drag (the overlay asks for one on every move) could publish a
    // slightly different update rect, and converting this event's pointer
    // with a different origin to the one the anchor was converted with would
    // make the picture jump by that difference in the middle of the drag.
    // Pinning the origin for the whole gesture is what makes a drag smooth.
    const PointF where = windowToFrame(state.geometry, PointF{static_cast<double>(drag.screen_point.h),
                                                              static_cast<double>(drag.screen_point.v)});
    const std::uint32_t modifiers = static_cast<std::uint32_t>(drag.modifiers);

    const CameraValues updated = applyDrag(state, where, modifiers);
    // [WP-CAMERA] Which parameters moved depends on the lens: a zoom on
    // DJI's lens moves DJI FOV and Correction Angle, not the Classic FOV.
    const std::uint32_t changed = changedFieldsFor(state.mode, state.start.dji);

    // Persist the state (applyDrag updated the axis lock and the last
    // position) BEFORE the last-time check, so a gesture that ends on this
    // very event still had its final position accounted for.
    writeDragSlot(slot, generation, state);

    const double frameAspect = (state.layout.valid && state.layout.viewport.h > 0.0)
                                   ? state.layout.viewport.w / state.layout.viewport.h
                                   : 0.0;
    if (commitValues(params, updated, changed, frameAspect)) {
        // Required by the SDK whenever CHANGED_VALUE is set during an event
        // (AE_Effect.h:2380): "If set during PF_Cmd_EVENT, be sure to also
        // set PF_EO_HANDLED_EVENT before returning."
        extra->evt_out_flags |= PF_EO_HANDLED_EVENT;

        // Remember what was just committed so the very next repaint shows it
        // even if the host has not propagated it into the draw pass's
        // parameter array yet (see the live readout section).  The final
        // event of a gesture is not remembered: it is cleared just below.
        if (!drag.last_time) {
            publishLiveReadout(in_data->effect_ref, updated, changed);
        }

        // The value moved, so the readout in the HUD is now stale. Ask for
        // the overlay to be redrawn; without this the numbers only refresh
        // when something else happens to invalidate the view, which makes a
        // drag look like it is doing nothing.
        requestOverlayRepaint(in_data, extra);
    }

    if (drag.last_time) {
        // The gesture is over: free the slot so a long session cannot leak
        // the fixed table, and clear the refcon so a duplicated final event
        // cannot resume the finished gesture.
        //
        // The live readout goes with it: from here on the host's own values
        // are the authority (see the live readout section for why it is not
        // held any longer than the gesture).
        clearLiveReadout(in_data->effect_ref);
        releaseDragSlot(slot, generation);
        if (drag.continue_refcon) {
            drag.continue_refcon[0] = 0;
        }
    }
    return PF_Err_NONE;
}

/// PF_Event_ADJUST_CURSOR: tell the host which cursor to show.
///
/// The cursor is the only affordance the user gets before pressing the
/// button, so it has to be honest: a hand over open picture, a rotate cursor
/// over the ring, a diagonal resize over a grip.  The modifiers are read
/// too, so holding Ctrl over open picture already shows the zoom cursor.
PF_Err onAdjustCursor(PF_InData* in_data, PF_ParamDef* params[], PF_EventExtra* extra) noexcept {
    if (!in_data || !extra || !params) {
        return PF_Err_NONE;
    }
    PF_AdjustCursorEventInfo& info = extra->u.adjust_cursor;

    // Same story as onDoClick: no size in the event, so the cached geometry
    // from the last repaint is the only way to know where the picture is.
    const FrameGeometry geometry = resolveFrameGeometry(in_data->width, in_data->height);
    const Layout layout = layoutForGeometry(params, geometry);
    if (!layout.valid) {
        return PF_Err_NONE;
    }

    const PointF where = windowToFramePoint(geometry, info.screen_point);
    const Handle handle = hitTest(layout, where);

    // Remember the answer for the next repaint, which has no pointer of its
    // own and needs this to highlight the handle under the cursor.
    //
    // The repaint is requested ONLY when the answer actually changed.
    // ADJUST_CURSOR arrives on every mouse move over the view - hundreds a
    // second - and invalidating the whole window on each one would make the
    // plug-in repaint the Program Monitor continuously while the pointer
    // merely travels across it. Comparing first means the cost is paid once
    // per handle transition, which is exactly when the highlight needs to
    // move.
    const Handle previous = hoverHandle();
    setHoverHandle(handle);
    if (handle != previous) {
        requestOverlayRepaint(in_data, extra);
    }

    if (handle == Handle::None) {
        // Leave the cursor alone outside the picture: PF_Cursor_NONE is the
        // documented "do not override" answer (AE_EffectUI.h:266-269).
        info.set_cursor = PF_Cursor_NONE;
        return PF_Err_NONE;
    }

    switch (resolveDragMode(handle, static_cast<std::uint32_t>(info.modifiers))) {
    case DragMode::Roll:
        info.set_cursor = PF_Cursor_CROSS_ROTATE;
        break;
    case DragMode::Fov:
        info.set_cursor = PF_Cursor_SCALE_DIAG_LR;
        break;
    case DragMode::PanOnly:
    case DragMode::TiltOnly:
    case DragMode::PanTilt:
        // The pan cursor (the four-way / grabbing hand) is exactly the
        // "you can drag the picture" affordance every 360 tool uses.
        info.set_cursor = PF_Cursor_PAN;
        break;
    case DragMode::None:
    default:
        info.set_cursor = PF_Cursor_NONE;
        return PF_Err_NONE;
    }

    extra->evt_out_flags |= PF_EO_HANDLED_EVENT;
    return PF_Err_NONE;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Public entry points
// ---------------------------------------------------------------------------
PF_Err registerCustomUi(PF_InData* in_data, PF_OutData* out_data) noexcept {
    // out_data is not written by this function - PF_OutData has no custom-UI
    // member (AE_Effect.h:3091-3108); the ONLY route is the register_ui
    // interaction callback below.  The argument is kept because the caller
    // is PARAMS_SETUP, which has both, and because a null out_data still
    // means the host handed us a malformed selector and we should say so.
    if (!out_data) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // A local, zeroed before use.  PF_CustomUIInfo is Adobe's own struct
    // from inside their #pragma pack scope, which is exactly why it is only
    // ever declared as a variable here and never redefined by us.
    PF_CustomUIInfo info;
    std::memset(&info, 0, sizeof(info));

    // PF_CustomEFlag_COMP: draw and receive events in the composition
    // window, which in Premiere is the Program Monitor.  The AE docs note
    // that Premiere additionally requires this flag for a custom UI to get
    // keyboard events at all, so it is the one flag that must be here.
    //
    // PF_CustomEFlag_EFFECT is deliberately NOT set: that would ask for a
    // custom-drawn control inside the Effect Controls panel, which we do not
    // want - the standard dials and sliders there are exactly right.
    info.events = PF_CustomEFlag_COMP;

    // Zero means "the whole comp view" rather than a fixed pixel box.  The
    // overlay tracks the picture as the monitor is resized or zoomed, so
    // pinning it to a fixed size would be wrong at every zoom level but one.
    info.comp_ui_width = 0;
    info.comp_ui_height = 0;
    info.comp_ui_alignment = PF_UIAlignment_NONE;

    // The layer and preview sizes stay at the zero memset() left, because
    // PF_CustomEFlag_LAYER and PF_CustomEFlag_PREVIEW are not requested:
    // Premiere has no layer window and the preview flag is an After Effects
    // feature we do not draw into.  The alignment fields are documented as
    // unused (AE_EffectUI.h:567-577) and are left zero for the same reason.

    // register_ui is the interaction callback the host provides; calling it
    // is what actually installs the custom UI.  A host that does not provide
    // it (or refuses) simply means no overlay - the effect still renders.
    if (in_data && in_data->inter.register_ui) {
        const PF_Err err = in_data->inter.register_ui(in_data->effect_ref, &info);
        if (err != PF_Err_NONE) {
            (void)PluginLog::oncef("reframe.ui.register", PluginLog::Level::Warn,
                                   "reframe ui: register_ui refused (err {}), no overlay", static_cast<int>(err));
        }
    }
    return PF_Err_NONE;
}

PF_Err handleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* extra) noexcept {
    // out_data is part of the selector signature and is kept so a future
    // handler that does need to report something back (a return_msg, an
    // out-flag) does not have to change every call site.  Nothing today
    // writes to it: the overlay reports everything through
    // extra->evt_out_flags and the parameter change flags.
    (void)out_data;

    // The whole body is wrapped: an exception crossing back into Premiere's
    // C stack is undefined behaviour, and a UI event is exactly the place a
    // std::bad_alloc from a snprintf buffer or a suite call would surface.
    try {
        if (!extra) {
            return PF_Err_NONE;
        }
        switch (extra->e_type) {
        case PF_Event_DRAW:
            // out_data is unused by the draw path: every suite comes from
            // in_data->pica_basicP and nothing is reported back through
            // out_data, so it is deliberately not forwarded.
            return onDraw(in_data, params, extra);
        case PF_Event_DO_CLICK:
            return onDoClick(in_data, params, extra);
        case PF_Event_DRAG:
            return onDrag(in_data, params, extra);
        case PF_Event_ADJUST_CURSOR:
            return onAdjustCursor(in_data, params, extra);

        // NEW_CONTEXT / CLOSE_CONTEXT arrive with no usable parameter list
        // (AE_EffectUI.h:96-100) and we keep no per-context state, so there
        // is genuinely nothing to do.  Accepting them silently is correct;
        // returning an error would make the host report a broken effect.
        case PF_Event_MOUSE_EXITED:
            // The pointer left the view, so nothing is hovered any more and
            // the next repaint must drop the highlight.  Without this a
            // handle stays lit after the mouse has gone elsewhere.
            //
            // A repaint is requested when a handle really was lit, so the
            // highlight visibly goes out instead of waiting for whatever
            // invalidates the view next.
            if (hoverHandle() != Handle::None) {
                setHoverHandle(Handle::None);
                requestOverlayRepaint(in_data, extra);
            }
            return PF_Err_NONE;

        case PF_Event_NEW_CONTEXT:
        case PF_Event_CLOSE_CONTEXT:
        case PF_Event_ACTIVATE:
        case PF_Event_DEACTIVATE:
        case PF_Event_IDLE:
        case PF_Event_KEYDOWN:
        default:
            return PF_Err_NONE;
        }
    } catch (const std::exception& e) {
        (void)PluginLog::oncef("reframe.ui.exception", PluginLog::Level::Error, "reframe ui: exception in event: {}",
                               e.what());
        return PF_Err_NONE;
    } catch (...) {
        (void)PluginLog::oncef("reframe.ui.exception2", PluginLog::Level::Error, "reframe ui: unknown exception in event");
        return PF_Err_NONE;
    }
}

void shutdown() noexcept {
    DragTable& table = dragTable();
    std::lock_guard<std::mutex> lock(table.mutex);
    for (DragSlot& slot : table.slots) {
        slot.inUse = false;
        slot.state = DragState{};
        slot.generation = 0;
    }
}

}  // namespace osv::reframe::ui
