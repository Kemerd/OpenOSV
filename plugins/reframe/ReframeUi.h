// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeUi.h - the interactive Program Monitor overlay of "Open 360 Reframe".
//
// This header has TWO halves and the split is deliberate:
//
//   1. The INTERACTION CORE (everything above the `#if defined(...)` guard
//      for the SDK half) is pure arithmetic over plain structs.  It knows
//      nothing about After Effects, DrawBot, Premiere or Windows - it only
//      turns "the pointer went from here to there with these modifiers" into
//      "Pan becomes this, Tilt becomes that".  Because it touches no Adobe
//      header it can be compiled straight into the test executable and its
//      sign conventions pinned by assertions, which is the only way to stop
//      a drag silently inverting when somebody edits the kernel.
//
//   2. The EVENT PLUMBING (`handleEvent`) is the thin PF_Cmd_EVENT shim that
//      reads a PF_EventExtra, calls the core, draws with DrawBot and writes
//      values back through the host.  It is declared here but every Adobe
//      type it needs is forward-declared rather than included, so a consumer
//      that only wants the maths does not drag in the SDK.
//
// WHY THE CORE IS SEPARATE FROM THE HANDLE GEOMETRY: the handles (the roll
// ring, the FOV grips) have to be in exactly the same place for hit-testing
// (PF_Event_DO_CLICK), for cursor feedback (PF_Event_ADJUST_CURSOR) and for
// drawing (PF_Event_DRAW).  Computing them once in `computeLayout()` and
// having all three read that one answer is what makes "the handle you see is
// the handle you grab" true by construction instead of by careful copying.
//
// SIGN CONVENTIONS - derived from the kernel, not guessed.  The chain is:
//
//   osvViewRay()      (include/osv/render/osv_kernel.h)
//       a pixel at centred offset (nx right, ny UP) becomes the view-frame
//       direction d_view = normalize(nx*k, 1, ny*k): +X view = right on
//       screen, +Y view = straight ahead, +Z view = up on screen.
//
//   VirtualCamera::rotation()  (src/osv/geom/VirtualCamera.cpp:69)
//       d_body = Rz(pan) * Rx(tilt) * Ry(roll) * d_view, with the
//       right-handed rotations of osv::Mat3d (include/osv/core/Math.h:135).
//
//   osvSampleEquirectRgba()    (osv_kernel.h:743)
//       lon = atan2(d_body.x, d_body.y), lat = asin(d_body.z).
//
// Feed the frame centre (nx = ny = 0, d_view = (0,1,0)) through that chain:
//
//   Rz(pan) * (0,1,0) = (-sin pan, cos pan, 0)   =>  lon = -pan
//   Rx(tilt) * (0,1,0) = (0, cos tilt, sin tilt) =>  lat = +tilt
//
// So INCREASING Pan moves the centre of the view toward SMALLER longitude,
// and increasing Tilt moves it UP.  "The world follows the cursor" therefore
// means:
//
//   * drag RIGHT (dx > 0): the picture must travel right with the hand, so
//     the content that was to the LEFT of centre (smaller longitude) has to
//     arrive at the centre => longitude must DECREASE => PAN INCREASES.
//         dPan = +kx * dx
//
//   * drag DOWN (dy > 0, screen y grows downward): the picture travels down,
//     so content from ABOVE must arrive at the centre => latitude must
//     INCREASE => TILT INCREASES.
//         dTilt = +ky * dy
//
// Both signs are asserted in tests/premiere/reframe/test_ui.cpp so that a
// future change to the kernel's ray or rotation order breaks a test rather
// than shipping a reframe tool that fights the user's hand.
#ifndef OSV_REFRAME_UI_H
#define OSV_REFRAME_UI_H

#include "ReframeParams.h"

#include <cstdint>

namespace osv::reframe::ui {

// ===========================================================================
//  Plain geometry types
//
//  These exist so the interaction core never has to name a PF_Point, a
//  PF_FixedPoint or a DRAWBOT_PointF32.  Adobe's headers are under
//  #pragma pack(push, 1); defining our own structs inside that scope would
//  silently give them 1-byte packing, so every type we own is declared here,
//  OUTSIDE any Adobe include, and the shim converts at the boundary.
// ===========================================================================

/// A point in FRAME pixels: the coordinate system of the rendered output
/// frame, origin top-left, +x right, +y DOWN (the screen convention, which
/// is also what PF_Event gives us after the source_to_frame callbacks).
struct PointF {
    double x = 0.0;  ///< Horizontal position in frame pixels.
    double y = 0.0;  ///< Vertical position in frame pixels, growing downward.
};

/// An axis-aligned rectangle in frame pixels (x/y = top-left corner).
struct RectF {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    /// Centre of the rectangle.  Safe on a degenerate (zero-size) rect.
    [[nodiscard]] constexpr PointF centre() const noexcept { return {x + 0.5 * w, y + 0.5 * h}; }
};

// ===========================================================================
//  Modifier keys
//
//  Mirrors of the PF_Mod_* bits (AE_EffectUI.h:252-259) so the core can be
//  compiled without the SDK.  The VALUES are identical to Adobe's on purpose
//  - the shim then passes `extra->u.do_click.modifiers` straight through
//  with a cast and a static_assert in ReframeUi.cpp keeps the two in step,
//  which is cheaper and far harder to get wrong than a translation table.
//
//  Only the three modifiers that Premiere genuinely delivers on Windows are
//  used by the interaction model:
//    Shift            constrain a pan/tilt drag to one axis;
//    Ctrl (CMD_CTRL)  turn any drag into a zoom;
//    Alt (OPT_ALT)    turn any drag into a roll.
//  PF_Mod_CAPS_LOCK_KEY and PF_Mod_MAC_CONTROL_KEY are deliberately ignored.
// ===========================================================================
enum Modifier : std::uint32_t {
    kModNone = 0x0000u,
    kModCmdCtrl = 0x0100u,  ///< PF_Mod_CMD_CTRL_KEY: ctrl on Windows.
    kModShift = 0x0200u,    ///< PF_Mod_SHIFT_KEY.
    kModCapsLock = 0x0400u, ///< PF_Mod_CAPS_LOCK_KEY (ignored by the model).
    kModOptAlt = 0x0800u,   ///< PF_Mod_OPT_ALT_KEY: alt on Windows.
};

// ===========================================================================
//  Handles and drag modes
// ===========================================================================

/// What the pointer is over, or what a drag in progress is doing.
///
/// The order matters for hit-testing: `hitTest()` checks the specific
/// handles first and falls back to `PanTilt`, so a grip that happens to
/// overlap open picture still wins.  `None` means "outside the viewport
/// entirely" - a click there is not ours and must be handed back to the host.
enum class Handle : int {
    None = 0,     ///< Outside the viewport; not our click.
    PanTilt = 1,  ///< Open picture: a pan/tilt drag.
    Roll = 2,     ///< The roll ring arc near the frame edge.
    Fov = 3,      ///< One of the four corner grips.
};

/// What a drag currently in flight is changing.
///
/// This is NOT the same as `Handle`: a drag that started on open picture
/// becomes a `Zoom` drag while Ctrl is held and a `Roll` drag while Alt is
/// held, without the grab point moving.  Resolving the two separately is
/// what lets a modifier be pressed mid-drag and take effect immediately,
/// which is how every other 360 tool behaves.
enum class DragMode : int {
    None = 0,
    PanTilt = 1,       ///< Free pan + tilt.
    PanOnly = 2,       ///< Shift, horizontal drag dominant.
    TiltOnly = 3,      ///< Shift, vertical drag dominant.
    Roll = 4,          ///< Roll ring, or Alt + drag anywhere.
    Fov = 5,           ///< Corner grip, or Ctrl + drag anywhere.
};

// ===========================================================================
//  Tuning constants
//
//  Every number the interaction uses is named here rather than buried in the
//  arithmetic, because these are the numbers a person tunes by feel and the
//  tests assert against.  They are `inline constexpr` so both the plug-in
//  and the test executable see one definition.
// ===========================================================================

/// Fraction of the viewport's SHORT side inside which a click counts as
/// grabbing a corner FOV grip.  0.11 puts a comfortable ~100px target on a
/// 1080p frame without eating into the middle of the picture.
inline constexpr double kFovGripFraction = 0.11;

/// Radius of the roll ring as a fraction of half the viewport's short side.
/// 0.84 sits it just inside the frame edge, clear of the picture's centre.
inline constexpr double kRollRingRadiusFraction = 0.84;

/// Half-thickness of the roll ring's grab band, as a fraction of the short
/// side.  The drawn arc is 1px; the GRAB band is much fatter, because a 1px
/// hit target is unusable.
inline constexpr double kRollRingGrabFraction = 0.035;

/// Angular half-extent of the drawn roll arcs, in degrees, measured from the
/// horizontal on each side.  Two symmetric arcs rather than a full circle:
/// a closed ring over the picture is exactly the "giant graphic" the design
/// brief rules out.
inline constexpr double kRollArcHalfSweepDeg = 26.0;

/// Degrees of FOV change per pixel of vertical drag on a corner grip.
/// Dragging DOWN (dy > 0) widens (zooms out), matching the "pull the corner
/// outward to see more" mental model of every grip-resize UI.
inline constexpr double kFovDegPerPixel = 0.25;

/// Degrees of roll per pixel of tangential travel along the ring, at the
/// reference radius.  The actual rate is computed from the true angle swept
/// about the centre, so this is only the fallback rate for an Alt+drag that
/// has no ring to sweep.
inline constexpr double kRollDegPerPixelAltDrag = 0.35;

/// A Shift-constrained drag picks its axis once, when the travel first
/// exceeds this many pixels.  Below it the two axes are too close to call
/// and re-deciding every mouse move makes the picture jitter between them.
inline constexpr double kAxisLockThresholdPx = 3.0;

/// How much faster than the hand a pan/tilt drag turns the view.
///
/// 1.0 is the exact grab: the point under the pointer stays under it.  The
/// field report was that this feels sluggish next to DJI Studio - reframing
/// a 360 shot means sweeping large angles, and a 1:1 grab asks for a lot of
/// mouse travel to do it.  The pointer's travel from the anchor is scaled by
/// this factor BEFORE the grab is solved, so the drag still moves the sphere
/// as one rigid piece in both axes (a quicker grab, not a distorted one);
/// the fixed-rate fallback is scaled by the same factor so the two agree.
inline constexpr double kPanTiltSensitivity = 2.0;

/// The FOV a pan/tilt drag is calibrated at.  At this FOV, dragging across
/// the full width of the viewport turns the view by exactly this many
/// degrees, so the picture tracks the cursor 1:1.  Away from it the rate
/// scales with the current FOV (see `dragScaleDegPerPixel`).
inline constexpr double kReferenceFovDeg = 90.0;

// ===========================================================================
//  Layout: where the handles are
// ===========================================================================

/// The one authoritative placement of every overlay element, in frame
/// pixels.  Hit-testing, cursor feedback and drawing all read this struct,
/// so they cannot disagree about where a handle is.
struct Layout {
    RectF viewport;              ///< The picture rectangle inside the frame.
    PointF centre;               ///< Viewport centre (the crosshair).
    double rollRingRadius = 0.0; ///< Radius of the roll ring, in pixels.
    double rollGrabBand = 0.0;   ///< Half-thickness of the ring's grab band.
    double fovGripSize = 0.0;    ///< Side length of a corner grip square.
    bool valid = false;          ///< False when the viewport was degenerate.
};

/// Build the layout for a viewport.
///
/// A viewport with a non-positive or non-finite width or height yields
/// `valid == false` and zeroed geometry; every consumer checks `valid` and
/// degrades to "no overlay" rather than dividing by zero.  This is the only
/// place the handle sizes are decided.
[[nodiscard]] Layout computeLayout(const RectF& viewport) noexcept;

// ===========================================================================
//  Frame geometry: the bridge between window and frame coordinates
//
//  WHY THIS TYPE EXISTS.  The overlay's geometry has to be derived from
//  whatever the host is willing to tell us, and the host tells us different
//  things on different events:
//
//    * PF_Event_DRAW carries PF_DrawEventInfo::update_rect, which is the
//      area to repaint IN THE WINDOW'S OWN COORDINATE SYSTEM
//      (AE_EffectUI.h:284).  It has both a size AND AN ORIGIN.
//    * PF_Event_DO_CLICK / _DRAG / _ADJUST_CURSOR carry only a
//      `screen_point`, also in window coordinates, and NO size at all.
//
//  Premiere Pro reports in_data->width/height as 0 during a comp-window
//  custom UI event, so the size can ONLY come from a draw event's update
//  rect - which means the pointer events have no way to compute a layout of
//  their own and must reuse the one the last repaint established.
//
//  Carrying the ORIGIN as well as the size is what keeps a drag from being
//  offset.  If the update rect starts at (x0, y0) then the layout built from
//  it lives in a box whose top-left is (0, 0) only when x0 == y0 == 0.  A
//  screen_point is absolute in window space, so mapping one into frame space
//  is a subtraction of that origin - the identity ONLY in the case the
//  overlay happened to be tested in.  `FrameGeometry` stores the origin so
//  the subtraction is explicit, checked and unit-tested rather than assumed.
// ===========================================================================

/// The size and window-space origin of the frame the overlay draws on.
///
/// `valid` is false until a draw event has established it; every pointer
/// event checks that before hit-testing, because acting on an unestablished
/// geometry would hit-test against zeros and grab nothing (or, worse, grab
/// the wrong thing).
struct FrameGeometry {
    double originX = 0.0;  ///< Window-space x of the frame's left edge.
    double originY = 0.0;  ///< Window-space y of the frame's top edge.
    double width = 0.0;    ///< Frame width in pixels.
    double height = 0.0;   ///< Frame height in pixels.
    bool valid = false;    ///< False until a draw event supplied a real size.
};

/// Map a point the host gave in WINDOW coordinates into FRAME coordinates.
///
/// This is the one place the two spaces are reconciled.  The transform is a
/// translation by the frame's origin and nothing more: neither space is
/// scaled or rotated relative to the other, because the update rect and the
/// screen_point are both delivered in the same window's pixels.
///
/// An invalid geometry is treated as the identity rather than as a failure -
/// that is the historical behaviour for a host that reports a zero origin,
/// and it keeps a caller that forgot to check `valid` no worse off than
/// before.  A non-finite input is passed through unchanged so the finiteness
/// checks downstream (hitTest, applyDrag) still see it and still reject it;
/// silently turning a NaN into a zero here would make a garbage event look
/// like a click at the frame's top-left corner.
[[nodiscard]] PointF windowToFrame(const FrameGeometry& geometry, const PointF& windowPoint) noexcept;

/// The four corner grip rectangles of a layout, written into `out` in the
/// order top-left, top-right, bottom-left, bottom-right.
///
/// `out` must have room for four entries; the function writes nothing and
/// returns 0 when the layout is invalid or `out` is null.  Returns the
/// number of rectangles written (0 or 4).
int fovGripRects(const Layout& layout, RectF out[4]) noexcept;

// ===========================================================================
//  Hit-testing
// ===========================================================================

/// Which handle sits under a point.
///
/// Checked in order of specificity: the corner grips first (they are small
/// and unambiguous), then the roll ring band (only on the two drawn arcs, so
/// the ring does not steal clicks along the whole circle where nothing is
/// drawn), then open picture, then nothing.
///
/// An invalid layout always answers `Handle::None`, which makes every caller
/// hand the click back to the host instead of starting a drag on garbage.
[[nodiscard]] Handle hitTest(const Layout& layout, const PointF& point) noexcept;

/// Whether a point lies on one of the two drawn roll arcs (not merely at the
/// ring's radius).  Exposed for the tests, which pin the arc extent.
[[nodiscard]] bool onRollArc(const Layout& layout, const PointF& point) noexcept;

// ===========================================================================
//  The drag
// ===========================================================================

/// The camera values the overlay reads and writes.  A snapshot, in degrees.
struct CameraValues {
    double panDeg = 0.0;
    double tiltDeg = 0.0;
    double rollDeg = 0.0;
    double fovDeg = OSV_REFRAME_FOV_DEFAULT;
};

/// The renderer's pixel -> ray model plus the grabbed point, captured when a
/// pan / tilt drag starts - "grab the sphere".
///
/// WHY.  A fixed degrees-per-pixel rate only keeps the picture under the
/// pointer near the centre of a narrow, unrolled view.  With a wide field of
/// view (the eye-offset projection bends toward stereographic as FOV grows),
/// with roll, or near the poles, the grabbed content slides away from the
/// hand, and a vertical drag reads as "the tilt dial moving" rather than as
/// dragging the scene - which is exactly how it felt next to DJI Studio.  A
/// grab instead remembers WHICH direction of the sphere was under the
/// pointer and, on every move, solves for the pan and tilt that put that
/// direction back under the pointer, through the very camera model the
/// renderer uses.  Roll is never changed by a grab.
///
/// POD on purpose: it lives inside DragState in the drag table.
struct SphereGrab {
    bool valid = false;   ///< False: fall back to the fixed-rate drag.
    int projection = 0;   ///< OSV_PROJ_* of the camera (buildView's choice).
    double focalPx = 0.0; ///< Focal length for the viewport, in viewport pixels.
    double eyeOffset = 0.0;
    double tanHalfH = 0.0;
    double tanHalfV = 0.0;
    /// The grabbed direction in the camera's PARENT frame, i.e.
    /// R_camera(start) * ray(anchor).  The source rotation (Rout =
    /// R_source * R_camera) multiplies both sides of the grab equation, so it
    /// cancels and is not needed.
    double target[3] = {0.0, 0.0, 0.0};
    /// Which of the two tilt solutions the gesture follows (+1 / -1): the
    /// side of the pointer ray's elevation peak the starting tilt was on.
    /// Fixed at the click so a long drag can never hop to the other solution
    /// (a sudden flip of the view).
    double tiltBranch = 1.0;
};

/// Start a grab: cast the anchor through the camera and remember where on
/// the sphere it landed.
///
/// `projection`..`tanHalfV` are the view fields of the camera the renderer
/// builds for a frame the size of `layout.viewport` (buildView()), and
/// `start` the camera values at the click.  Returns an invalid grab - so the
/// drag falls back to the fixed rate - when the layout is invalid, a value is
/// not finite, or the anchor lies where the projection has no ray (outside
/// the valid radius of a very wide eye-offset view).
[[nodiscard]] SphereGrab beginSphereGrab(int projection, double focalPx, double eyeOffset, double tanHalfH,
                                         double tanHalfV, const Layout& layout, const CameraValues& start,
                                         const PointF& anchor) noexcept;

/// Solve the pan and tilt that put the grabbed direction under `current`.
///
/// `mode` decides what may move: PanTilt both, PanOnly pan only (tilt kept),
/// TiltOnly tilt only (pan kept).  Roll and FOV always come from `start`.
/// The pan is unwrapped to the solution nearest `start` so the dial never
/// jumps by 360 degrees; the tilt is the root nearest `start` inside
/// +-OSV_REFRAME_TILT_LIMIT_DEG (clamped when neither root is).
///
/// Returns false - and leaves `out` alone - when the grab is invalid, the
/// pointer has no ray, or the geometry is degenerate (a pointer ray along
/// the tilt axis); the caller then uses the fixed-rate drag.
[[nodiscard]] bool solveSphereGrab(const SphereGrab& grab, const Layout& layout, const CameraValues& start,
                                   const PointF& current, DragMode mode, CameraValues& out) noexcept;

/// Everything a drag in progress needs to remember between events.
///
/// It is deliberately a plain aggregate with no pointers: the shim stores it
/// in `PF_DoClickEventInfo::continue_refcon`, four `A_intptr_t` words that
/// the host carries from the DO_CLICK to every DRAG of the same gesture.
/// Four 64-bit words is 32 bytes, which is not enough for this struct, so
/// the shim stores an index into a small process-wide table instead and this
/// struct lives there - see ReframeUi.cpp.  Keeping it POD means the table
/// needs no destructor and no locking discipline beyond one mutex.
struct DragState {
    bool active = false;         ///< A gesture is in flight.
    DragMode mode = DragMode::None;
    Handle handle = Handle::None; ///< What was grabbed (does not change mid-drag).
    PointF anchor;               ///< Where the gesture started, frame pixels.
    PointF last;                 ///< The previous position, frame pixels.
    CameraValues start;          ///< Values when the gesture started.
    Layout layout;               ///< Layout captured at grab time.
    /// Window->frame transform captured at grab time.
    ///
    /// Held for the WHOLE gesture on purpose. The live geometry can be
    /// republished by a repaint mid-drag, and converting one event of a
    /// gesture with a different origin to the one its anchor used would
    /// offset the picture by that difference in a single mouse move.
    FrameGeometry geometry;
    double startRollAngleDeg = 0.0; ///< Angle of the anchor about the centre.
    bool axisLocked = false;     ///< Shift: whether the axis is already chosen.
    /// The grabbed point of the sphere for pan / tilt drags; invalid when the
    /// shim could not build the camera, in which case the fixed-rate drag
    /// is used exactly as before.
    SphereGrab grab;
};

/// Degrees of view rotation per pixel of drag, at a given FOV and viewport
/// width.
///
/// The rate is `fov / viewportWidth`: dragging across the whole viewport
/// turns the view by one full field of view, so the content under the cursor
/// stays under the cursor. That is what makes a 30-degree drag and a
/// 150-degree drag feel identical, which the brief asks for explicitly.
///
/// Returns 0 for a non-finite or non-positive width or FOV, so a caller can
/// multiply unconditionally and simply get no movement.
[[nodiscard]] double dragScaleDegPerPixel(double fovDeg, double viewportWidth) noexcept;

/// Resolve what a drag should be doing right now.
///
/// `grabbed` is the handle the gesture started on and `modifiers` the keys
/// held AT THIS MOMENT (not at grab time), so pressing Ctrl halfway through a
/// pan turns it into a zoom without letting go. Handle-started drags keep
/// their handle: grabbing the roll ring and then pressing Ctrl still rolls,
/// because the user aimed at that handle deliberately.
[[nodiscard]] DragMode resolveDragMode(Handle grabbed, std::uint32_t modifiers) noexcept;

/// Apply one drag update and produce the new camera values.
///
/// `state` is the gesture's memory (updated in place), `current` the pointer
/// position now, `modifiers` the keys held now. The returned values are
/// always finite and always inside the parameters' valid ranges; Tilt is
/// clamped to +-90 (OSV_REFRAME_TILT_LIMIT_DEG) because the geometry clamps
/// there anyway and letting the control run past it makes the picture stick
/// while the number keeps moving.
///
/// Pan and Roll are NOT clamped: they are unbounded angle dials on purpose
/// so a user can keyframe a full turn (see ReframeParams.h).
///
/// An inactive state or a non-finite point leaves the values untouched.
[[nodiscard]] CameraValues applyDrag(DragState& state, const PointF& current, std::uint32_t modifiers) noexcept;

/// Which of the four camera parameters a drag mode writes.
///
/// A bit field over `ChangedField`. The shim uses it to mark exactly the
/// parameters that moved with PF_ChangeFlag_CHANGED_VALUE - marking a
/// parameter that did not change would make the host record a redundant
/// keyframe on it, which is precisely the "why is there a Roll keyframe?"
/// bug that makes an auto-keyframing UI infuriating.
enum ChangedField : std::uint32_t {
    kChangedNone = 0u,
    kChangedPan = 1u << 0,
    kChangedTilt = 1u << 1,
    kChangedRoll = 1u << 2,
    kChangedFov = 1u << 3,
};

/// The fields a drag mode writes.  Pure function of the mode.
[[nodiscard]] std::uint32_t changedFieldsFor(DragMode mode) noexcept;

/// Clamp a camera snapshot into the parameters' valid ranges, replacing any
/// non-finite component with the corresponding default.
///
/// Called on everything read from the host as well as on everything written
/// back: a project file can hold a NaN, and one NaN reaching the drag maths
/// would poison every subsequent value.
[[nodiscard]] CameraValues sanitise(const CameraValues& values) noexcept;

// ===========================================================================
//  The live readout
//
//  While a drag is in flight the HUD shows the values the gesture committed
//  rather than trusting the parameter array a draw event carries, because a
//  host may hand the draw pass the values of the frame it last FINISHED
//  rendering - and with a slow render those lag the hand by a long way.  The
//  shim (ReframeUiEvent.cpp) remembers the values; the decision of what to
//  SHOW is this pure function, so it is pinned by tests.
// ===========================================================================

/// Two readout values closer than this (degrees) are the same number.
///
/// PF_Fixed holds 1/65536 of a degree and the FOV slider is a float, so a
/// value that went into the host and came back can differ from what was
/// written by far less than this - and anything the readout prints (one
/// decimal place) differs by far more.
inline constexpr double kLiveReadoutMatchEpsDeg = 1e-3;

/// What the HUD should show, given what the host reports and what an
/// in-flight gesture has committed.
///
/// `touchedFields` is the union of `changedFieldsFor()` over every event of
/// the gesture so far.  The rule is:
///
///   * the fields the gesture has TOUCHED come from `live` - those are the
///     ones a lagging host can be behind on;
///   * the fields it has NOT touched must already agree with the host.  A
///     drag cannot have moved them, so if they disagree, `live` is not a
///     picture of this host's state at all (another instance, another
///     moment) and the host's values are returned untouched.
///
/// That consistency check is what makes the substitution safe even though
/// the shim can only identify an instance by an address that the host is
/// free to reuse.  Both inputs are sanitised first, so a NaN from either
/// side can neither be shown nor defeat the comparison.
[[nodiscard]] CameraValues mergeLiveReadout(const CameraValues& fromHost, const CameraValues& live,
                                            std::uint32_t touchedFields) noexcept;

// ===========================================================================
//  The SDK half lives in ReframeUiEvent.h
//
//  It is a SEPARATE header on purpose.  The Adobe headers are wrapped in
//  #pragma pack(push, 1), and any struct declared while that is in force
//  gets 1-byte packing.  Every type above is ours and must keep natural
//  alignment, so this header must never be included from inside an Adobe
//  include block - and the only way to guarantee that is for it not to name
//  an Adobe type at all.  ReframeUiEvent.h includes the SDK first and this
//  header afterwards, which is the safe order.
// ===========================================================================

}  // namespace osv::reframe::ui

#endif /* OSV_REFRAME_UI_H */
