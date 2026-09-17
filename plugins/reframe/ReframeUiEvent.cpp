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
    return sanitise(v);
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
[[nodiscard]] bool commitValues(PF_ParamDef* params[], const CameraValues& values, std::uint32_t changed) noexcept {
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

    return wrote;
}

// ---------------------------------------------------------------------------
//  Where the picture is
// ---------------------------------------------------------------------------

/// The viewport rectangle the overlay draws over, in frame pixels.
///
/// The effect letterboxes its output into a centred rectangle of the chosen
/// aspect (ReframeParams::computeViewport), and the overlay must sit on THAT
/// rectangle rather than on the whole frame - otherwise the crosshair would
/// float in the black bars and a corner grip would be unreachable.
///
/// The aspect popup is read from the parameter array; "Match Sequence" has
/// no sequence to ask during an event, so it falls back to the frame's own
/// shape, which is what Match Sequence resolves to for a sequence-sized
/// frame anyway.
///
/// A degenerate frame yields a zero rectangle and, through computeLayout(),
/// an invalid layout that every consumer already handles.
[[nodiscard]] RectF viewportForFrame(PF_ParamDef* params[], int frameW, int frameH) noexcept {
    if (frameW <= 0 || frameH <= 0) {
        return RectF{};
    }
    const double frameAspect = static_cast<double>(frameW) / static_cast<double>(frameH);

    Aspect aspect = Aspect::MatchSequence;
    if (params && params[kIndexOutputAspect]) {
        aspect = sanitiseAspect(params[kIndexOutputAspect]->u.pd.value);
    }

    // -1 for the sequence aspect: the Sequence Info Suite is not safe to ask
    // during a UI event (it wants a timeline id we do not have here), and
    // resolveAspectRatio() documents a non-positive value as "could not
    // ask".  For Match Sequence that lands on the frame's own shape below.
    const double ratio = resolveAspectRatio(aspect, -1.0, frameAspect);

    const Viewport vp = computeViewport(frameW, frameH, ratio);
    return RectF{static_cast<double>(vp.x), static_cast<double>(vp.y), static_cast<double>(vp.w),
                 static_cast<double>(vp.h)};
}

/// Convert a point the host gave in WINDOW coordinates into frame pixels.
///
/// The host supplies `frame_to_source` / `source_to_frame` in
/// PF_EventCallbacks (AE_EffectUI.h:474-475) but they map between the
/// LAYER's source space and the composition frame, which is not what we
/// need: the overlay works in the coordinates of the rendered output frame,
/// and the event's screen_point is already in that frame's space for a comp
/// window custom UI.
///
/// So the conversion is the identity, and this function exists to say so
/// explicitly and to do the one thing that is genuinely needed: reject a
/// non-finite or absurd coordinate before it reaches the maths.
[[nodiscard]] PointF windowToFrame(const PF_Point& point) noexcept {
    return PointF{static_cast<double>(point.h), static_cast<double>(point.v)};
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
            char line[96] = {};
            const int written =
                std::snprintf(line, sizeof(line), "Pan %.1f  Tilt %.1f  Roll %.1f  FOV %.1f", values.panDeg,
                              values.tiltDeg, values.rollDeg, values.fovDeg);
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

    const RectF viewport = viewportForFrame(params, in_data->width, in_data->height);
    const Layout layout = computeLayout(viewport);
    if (!layout.valid) {
        (void)PluginLog::oncef("reframe.ui.viewport", PluginLog::Level::Warn,
                               "reframe ui: no overlay, degenerate frame {}x{}", in_data->width, in_data->height);
        return PF_Err_NONE;
    }

    // Every suite below comes from the host's SPBasicSuite, which the effect
    // is handed in in_data.  Without it there is nothing to acquire.
    SPBasicSuite* basic = in_data->pica_basicP;
    if (!basic) {
        (void)PluginLog::oncef("reframe.ui.basic", PluginLog::Level::Warn,
                               "reframe ui: no overlay, host provided no SPBasicSuite");
        return PF_Err_NONE;
    }

    // The custom UI suite hands out the DrawBot reference for this context.
    // It is a v2 suite frozen in AE 13.5, which every Premiere we target
    // ships, but a missing suite is still handled rather than assumed.
    osv::premiere::SuiteHandle<PF_EffectCustomUISuite2> customUi;
    if (!customUi.acquire(basic, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion2) ||
        !customUi->PF_GetDrawingReference) {
        (void)PluginLog::oncef("reframe.ui.customsuite", PluginLog::Level::Warn,
                               "reframe ui: no overlay, PF Effect Custom UI Suite v2 unavailable");
        return PF_Err_NONE;
    }

    DRAWBOT_DrawRef drawRef = nullptr;
    const PF_Err refErr = customUi->PF_GetDrawingReference(extra->contextH, &drawRef);
    if (refErr != PF_Err_NONE || !drawRef) {
        (void)PluginLog::oncef("reframe.ui.drawref", PluginLog::Level::Warn,
                               "reframe ui: no overlay, no drawing reference (err {})", static_cast<int>(refErr));
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
    drawOverlay(bundle.suites(), drawRef, layout, readCamera(params), hoverHandle());

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

    const RectF viewport = viewportForFrame(params, in_data->width, in_data->height);
    const Layout layout = computeLayout(viewport);
    if (!layout.valid) {
        return PF_Err_NONE;  // No picture to grab; leave the click to the host.
    }

    const PointF where = windowToFrame(click.screen_point);
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
    state.start = readCamera(params);
    state.mode = resolveDragMode(handle, static_cast<std::uint32_t>(click.modifiers));
    state.axisLocked = false;
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

    const PointF where = windowToFrame(drag.screen_point);
    const std::uint32_t modifiers = static_cast<std::uint32_t>(drag.modifiers);

    const CameraValues updated = applyDrag(state, where, modifiers);
    const std::uint32_t changed = changedFieldsFor(state.mode);

    // Persist the state (applyDrag updated the axis lock and the last
    // position) BEFORE the last-time check, so a gesture that ends on this
    // very event still had its final position accounted for.
    writeDragSlot(slot, generation, state);

    if (commitValues(params, updated, changed)) {
        // Required by the SDK whenever CHANGED_VALUE is set during an event
        // (AE_Effect.h:2380): "If set during PF_Cmd_EVENT, be sure to also
        // set PF_EO_HANDLED_EVENT before returning."
        extra->evt_out_flags |= PF_EO_HANDLED_EVENT;
    }

    if (drag.last_time) {
        // The gesture is over: free the slot so a long session cannot leak
        // the fixed table, and clear the refcon so a duplicated final event
        // cannot resume the finished gesture.
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

    const RectF viewport = viewportForFrame(params, in_data->width, in_data->height);
    const Layout layout = computeLayout(viewport);
    if (!layout.valid) {
        return PF_Err_NONE;
    }

    const PointF where = windowToFrame(info.screen_point);
    const Handle handle = hitTest(layout, where);

    // Remember the answer for the next repaint, which has no pointer of its
    // own and needs this to highlight the handle under the cursor.
    setHoverHandle(handle);

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
            setHoverHandle(Handle::None);
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
