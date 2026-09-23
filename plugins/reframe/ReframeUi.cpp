// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeUi.cpp - the interaction core of the Program Monitor overlay.
//
// Pure arithmetic: no Adobe header, no Windows header, no allocation, no
// logging.  Everything here is a total function of its inputs, which is what
// lets tests/premiere/reframe/test_ui.cpp pin the drag-to-degrees mapping
// (including its SIGNS) without a host in the loop.
//
// The SDK-facing shim - events, DrawBot, writing values back - is in
// ReframeUiEvent.cpp.  Splitting them is what keeps this file testable.
#include "ReframeUi.h"

// The renderer's camera function (osvViewRay).  The one dependency this
// otherwise self-contained unit takes, on purpose: the grab must cast the
// pointer through exactly the camera the picture is rendered with.  The
// header is plain C, inline, and links nothing.
#include "osv/render/osv_kernel.h"

#include <algorithm>
#include <cmath>

namespace osv::reframe::ui {

namespace {

/// Radians per degree.  Spelled out rather than pulled from the library so
/// this translation unit stays dependency-free.
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

/// True when every component of a point is a finite number.
///
/// A non-finite coordinate can genuinely arrive: the host's
/// source_to_frame callback can fail and leave the fixed-point value
/// untouched, and a degenerate layer transform produces infinities.  Every
/// entry point checks this before doing arithmetic that would otherwise
/// propagate a NaN into a parameter value and into the project file.
[[nodiscard]] bool isFinitePoint(const PointF& p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y);
}

/// A finite, positive number, or `fallback`.
[[nodiscard]] double positiveOr(double value, double fallback) noexcept {
    return (std::isfinite(value) && value > 0.0) ? value : fallback;
}

/// Angle of `point` about `centre`, in degrees, measured the same way the
/// DrawBot path suite measures arcs: 0 at 3 o'clock, growing CLOCKWISE on
/// screen (DrawbotSuite.h:295-297 "zero start degrees == 3 o'clock, sweep is
/// clockwise").  Screen y grows downward, so a plain atan2(dy, dx) already
/// increases clockwise and no sign flip is needed.
///
/// Returns 0 when the point coincides with the centre, where the angle is
/// genuinely undefined; callers treat that as "no rotation this frame".
[[nodiscard]] double angleAboutDeg(const PointF& centre, const PointF& point) noexcept {
    const double dx = point.x - centre.x;
    const double dy = point.y - centre.y;
    if (!(std::isfinite(dx) && std::isfinite(dy))) {
        return 0.0;
    }
    if (dx == 0.0 && dy == 0.0) {
        return 0.0;
    }
    return std::atan2(dy, dx) * kRadToDeg;
}

/// The shortest signed difference between two angles in degrees, in
/// (-180, 180].
///
/// Without this a roll drag that crosses the +/-180 boundary would jump by a
/// full turn in one mouse move, which on an auto-keyframing control means a
/// 360-degree spin baked into the timeline.
[[nodiscard]] double wrapDeltaDeg(double deltaDeg) noexcept {
    if (!std::isfinite(deltaDeg)) {
        return 0.0;
    }
    double d = std::fmod(deltaDeg + 180.0, 360.0);
    if (d < 0.0) {
        d += 360.0;
    }
    return d - 180.0;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------
Layout computeLayout(const RectF& viewport) noexcept {
    Layout layout;

    // A degenerate viewport is not an error - it is what a 0x0 comp or a
    // collapsed panel reports - so it produces an explicitly invalid layout
    // instead of an exception or a divide by zero further down.
    if (!(std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.w) &&
          std::isfinite(viewport.h))) {
        return layout;
    }
    if (!(viewport.w > 0.0) || !(viewport.h > 0.0)) {
        return layout;
    }

    layout.viewport = viewport;
    layout.centre = viewport.centre();

    // Every handle is sized off the SHORT side so the overlay looks the same
    // on a 2.35:1 crop as on a 9:16 one; sizing off the width alone would
    // make the grips swallow a vertical frame.
    const double shortSide = std::min(viewport.w, viewport.h);

    layout.rollRingRadius = 0.5 * shortSide * kRollRingRadiusFraction;
    layout.rollGrabBand = shortSide * kRollRingGrabFraction;
    layout.fovGripSize = shortSide * kFovGripFraction;

    // The grips must never grow so large that the four of them meet in the
    // middle: on a very small viewport that would leave no open picture at
    // all and pan/tilt would become unreachable.  A quarter of the short
    // side is the hard ceiling, which keeps at least half the frame open.
    layout.fovGripSize = std::min(layout.fovGripSize, 0.25 * shortSide);

    layout.valid = true;
    return layout;
}

PointF windowToFrame(const FrameGeometry& geometry, const PointF& windowPoint) noexcept {
    // A geometry that no draw event has filled in yet, or one holding a
    // nonsense origin, degrades to the identity.  That is deliberately the
    // same answer the overlay gave before the origin was tracked at all, so
    // a host that reports a zero-origin update rect is unaffected by this
    // function existing.
    if (!geometry.valid || !std::isfinite(geometry.originX) || !std::isfinite(geometry.originY)) {
        return windowPoint;
    }

    // Non-finite input is passed through UNCHANGED rather than zeroed: the
    // callers all test finiteness themselves, and a NaN that arrived here
    // must stay a NaN so it is rejected instead of being mistaken for a
    // legitimate click at the origin.
    if (!isFinitePoint(windowPoint)) {
        return windowPoint;
    }

    return PointF{windowPoint.x - geometry.originX, windowPoint.y - geometry.originY};
}

int fovGripRects(const Layout& layout, RectF out[4]) noexcept {
    if (!out || !layout.valid) {
        return 0;
    }
    const double s = layout.fovGripSize;
    if (!(std::isfinite(s) && s > 0.0)) {
        return 0;
    }
    const RectF& v = layout.viewport;

    // Order is fixed: top-left, top-right, bottom-left, bottom-right.  The
    // drawing code and the tests both depend on it.
    out[0] = {v.x, v.y, s, s};
    out[1] = {v.x + v.w - s, v.y, s, s};
    out[2] = {v.x, v.y + v.h - s, s, s};
    out[3] = {v.x + v.w - s, v.y + v.h - s, s, s};
    return 4;
}

// ---------------------------------------------------------------------------
//  Hit-testing
// ---------------------------------------------------------------------------
namespace {

/// Whether a point is inside a rectangle, edges included.
[[nodiscard]] bool inRect(const RectF& r, const PointF& p) noexcept {
    return p.x >= r.x && p.x <= r.x + r.w && p.y >= r.y && p.y <= r.y + r.h;
}

}  // namespace

bool onRollArc(const Layout& layout, const PointF& point) noexcept {
    if (!layout.valid || !isFinitePoint(point)) {
        return false;
    }
    const double dx = point.x - layout.centre.x;
    const double dy = point.y - layout.centre.y;
    const double r = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(r)) {
        return false;
    }

    // Radially: within the grab band around the ring's radius.
    if (std::fabs(r - layout.rollRingRadius) > layout.rollGrabBand) {
        return false;
    }

    // Angularly: only on the two drawn arcs, which straddle the horizontal
    // at 0 and 180 degrees.  Grabbing the ring where nothing is drawn would
    // be a control the user cannot see, which is worse than no control.
    const double angle = angleAboutDeg(layout.centre, point);
    const double fromRight = std::fabs(wrapDeltaDeg(angle - 0.0));
    const double fromLeft = std::fabs(wrapDeltaDeg(angle - 180.0));
    return fromRight <= kRollArcHalfSweepDeg || fromLeft <= kRollArcHalfSweepDeg;
}

Handle hitTest(const Layout& layout, const PointF& point) noexcept {
    if (!layout.valid || !isFinitePoint(point)) {
        return Handle::None;
    }
    // Outside the picture entirely: not our click.  Returning None here is
    // what makes the shim leave PF_EO_HANDLED_EVENT clear so the host can do
    // whatever it would normally do with a click on the pasteboard.
    if (!inRect(layout.viewport, point)) {
        return Handle::None;
    }

    // Corner grips first: they are the smallest targets, so they win any
    // overlap with the roll ring or with open picture.
    RectF grips[4];
    if (fovGripRects(layout, grips) == 4) {
        for (const RectF& g : grips) {
            if (inRect(g, point)) {
                return Handle::Fov;
            }
        }
    }

    // Then the roll ring, but only where it is actually drawn.
    if (onRollArc(layout, point)) {
        return Handle::Roll;
    }

    // Everything else inside the picture pans and tilts.
    return Handle::PanTilt;
}

// ---------------------------------------------------------------------------
//  Drag maths
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Grab the sphere
// ---------------------------------------------------------------------------

namespace {

/// Unit view-frame ray under a point of the viewport, through the renderer's
/// own camera function (osvViewRay, osv_kernel.h) so the overlay and the
/// picture can never disagree about what is under the pointer.
///
/// View frame: X right, Y forward, Z up.  The pointer position is continuous
/// (no pixel-centre offset): it is a point, not a pixel.
[[nodiscard]] bool grabRay(const SphereGrab& g, const Layout& layout, const PointF& p, double out[3]) noexcept {
    if (!layout.valid || !isFinitePoint(p)) {
        return false;
    }
    const double w = layout.viewport.w;
    const double h = layout.viewport.h;
    if (!(w > 0.0) || !(h > 0.0)) {
        return false;
    }
    // Centred offsets, +ny = up, exactly as osvReframeEquirectPixel forms them.
    const float nx = static_cast<float>((p.x - layout.viewport.x) - 0.5 * w);
    const float ny = static_cast<float>(0.5 * h - (p.y - layout.viewport.y));
    float d[3] = {0.0f, 0.0f, 0.0f};
    if (!osvViewRay(g.projection, static_cast<float>(g.focalPx), static_cast<float>(g.eyeOffset),
                    static_cast<float>(g.tanHalfH), static_cast<float>(g.tanHalfV), static_cast<float>(w),
                    static_cast<float>(h), nx, ny, d)) {
        return false;  // outside the projection's valid radius
    }
    const double n = std::sqrt(static_cast<double>(d[0]) * d[0] + static_cast<double>(d[1]) * d[1] +
                               static_cast<double>(d[2]) * d[2]);
    if (!(n > 0.0) || !std::isfinite(n)) {
        return false;
    }
    out[0] = d[0] / n;
    out[1] = d[1] / n;
    out[2] = d[2] / n;
    return true;
}

// The camera's rotation is R = Rz(pan) Rx(tilt) Ry(roll) (VirtualCamera::
// rotation, the same order buildView uses), with the right-handed matrices
// of osv/core/Math.h.  These apply one factor to a vector in place.

void applyRotZ(double rad, double v[3]) noexcept {
    const double c = std::cos(rad), s = std::sin(rad);
    const double x = c * v[0] - s * v[1];
    const double y = s * v[0] + c * v[1];
    v[0] = x;
    v[1] = y;
}

void applyRotX(double rad, double v[3]) noexcept {
    const double c = std::cos(rad), s = std::sin(rad);
    const double y = c * v[1] - s * v[2];
    const double z = s * v[1] + c * v[2];
    v[1] = y;
    v[2] = z;
}

void applyRotY(double rad, double v[3]) noexcept {
    const double c = std::cos(rad), s = std::sin(rad);
    const double x = c * v[0] + s * v[2];
    const double z = -s * v[0] + c * v[2];
    v[0] = x;
    v[2] = z;
}

/// Wrap an angle in radians into (-pi, pi].
[[nodiscard]] double wrapRad(double a) noexcept {
    constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
    a = std::fmod(a, kTwoPi);
    if (a <= -0.5 * kTwoPi) {
        a += kTwoPi;
    } else if (a > 0.5 * kTwoPi) {
        a -= kTwoPi;
    }
    return a;
}

/// Horizontal length below which an azimuth is undefined (looking straight
/// up or down): the pan is then kept rather than read from noise.
constexpr double kAzimuthEpsilon = 1e-9;

}  // namespace

SphereGrab beginSphereGrab(int projection, double focalPx, double eyeOffset, double tanHalfH, double tanHalfV,
                           const Layout& layout, const CameraValues& start, const PointF& anchor) noexcept {
    SphereGrab g;
    if (!layout.valid || !std::isfinite(focalPx) || !(focalPx > 0.0) || !std::isfinite(eyeOffset) ||
        !std::isfinite(tanHalfH) || !std::isfinite(tanHalfV)) {
        return g;
    }
    g.projection = projection;
    g.focalPx = focalPx;
    g.eyeOffset = eyeOffset;
    g.tanHalfH = tanHalfH;
    g.tanHalfV = tanHalfV;

    // The grabbed direction in the camera's parent frame: R_camera(start)
    // applied to the anchor's view ray, factor by factor (roll first).
    double v[3];
    if (!grabRay(g, layout, anchor, v)) {
        return SphereGrab{};
    }
    const CameraValues s = sanitise(start);
    applyRotY(s.rollDeg * kDegToRad, v);

    // The tilt branch, decided once for the whole gesture: which side of its
    // elevation peak the starting tilt sits on for the anchor's own ray (the
    // start tilt solves the grab equation exactly there).  See
    // solveSphereGrab for why it must never be re-derived mid-drag.
    const double peak0 = wrapRad(0.5 * 3.14159265358979323846 - std::atan2(v[2], v[1]));
    g.tiltBranch = (wrapRad(s.tiltDeg * kDegToRad - peak0) >= 0.0) ? 1.0 : -1.0;

    applyRotX(s.tiltDeg * kDegToRad, v);
    applyRotZ(s.panDeg * kDegToRad, v);
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
        return SphereGrab{};
    }
    g.target[0] = v[0];
    g.target[1] = v[1];
    g.target[2] = v[2];
    g.valid = true;
    return g;
}

bool solveSphereGrab(const SphereGrab& grab, const Layout& layout, const CameraValues& start, const PointF& current,
                     DragMode mode, CameraValues& out) noexcept {
    if (!grab.valid) {
        return false;
    }
    if (mode != DragMode::PanTilt && mode != DragMode::PanOnly && mode != DragMode::TiltOnly) {
        return false;
    }
    const CameraValues s = sanitise(start);

    // The pointer's view ray with the (unchanged) roll applied: u = Ry(roll) v.
    // What remains to solve is  Rz(pan) Rx(tilt) u = target.
    double u[3];
    if (!grabRay(grab, layout, current, u)) {
        return false;
    }
    applyRotY(s.rollDeg * kDegToRad, u);
    const double* t = grab.target;

    // ---- tilt -----------------------------------------------------------
    // Rz(pan) leaves z alone, so the z component alone fixes the tilt:
    //     (Rx(p) u).z = u.y sin p + u.z cos p = t.z
    // With R = |(u.y, u.z)| and phi = atan2(u.z, u.y) the left side is
    // R sin(p + phi), largest (= R) at p* = pi/2 - phi, where the pointer's
    // ray is lifted as high as any tilt can lift it.  The two solutions are
    // symmetric about that peak:
    //     p = p* + acos(t.z / R)   or   p = p* - acos(t.z / R).
    //
    // The drag must stay on ONE of them for its whole life.  Picking
    // "whichever root is nearer the start" instead jumps to the other root
    // the moment the near one leaves the dial's range, which flips the view
    // (the classic grab-the-sphere failure: tilt 89 -> 66 in one mouse move,
    // pinned by a test).  The branch was fixed at the click (see
    // beginSphereGrab) - re-deriving it here from the moving peak would
    // switch it as soon as the peak passed the start tilt, which is the same
    // flip by another route.  A solution past the tilt limit is clamped to the limit
    // on that same branch, and a target the pointer cannot reach at all
    // (|t.z| > R: the point was dragged past a pole) takes the peak itself,
    // the tilt that brings it closest.  Both keep the tilt monotonic.
    const double tilt0 = s.tiltDeg * kDegToRad;
    const double limit = OSV_REFRAME_TILT_LIMIT_DEG * kDegToRad;
    double tilt = tilt0;
    if (mode != DragMode::PanOnly) {
        const double r = std::hypot(u[1], u[2]);
        if (!(r > 1e-9)) {
            return false;  // pointer ray along the tilt axis: tilt undetermined
        }
        const double cosArg = std::clamp(t[2] / r, -1.0, 1.0);
        const double phi = std::atan2(u[2], u[1]);
        const double peak = wrapRad(0.5 * 3.14159265358979323846 - phi);
        const double root = wrapRad(peak + grab.tiltBranch * std::acos(cosArg));
        tilt = std::clamp(root, -limit, limit);
    }

    // ---- pan --------------------------------------------------------------
    // With the tilt fixed, the pan is the azimuth that turns Rx(tilt) u onto
    // the target about the vertical axis.
    double pan = s.panDeg * kDegToRad;
    if (mode != DragMode::TiltOnly) {
        double w[3] = {u[0], u[1], u[2]};
        applyRotX(tilt, w);
        const double wh = std::hypot(w[0], w[1]);
        const double th = std::hypot(t[0], t[1]);
        if (wh > kAzimuthEpsilon && th > kAzimuthEpsilon) {
            const double delta = std::atan2(t[1], t[0]) - std::atan2(w[1], w[0]);
            // Unwrap relative to the starting pan so the dial moves by the
            // small angle, never by a full turn.
            pan = pan + wrapRad(delta - pan);
        }
    }

    const double panDeg = pan * kRadToDeg;
    const double tiltDeg = tilt * kRadToDeg;
    if (!std::isfinite(panDeg) || !std::isfinite(tiltDeg)) {
        return false;
    }
    out = s;
    out.panDeg = panDeg;
    out.tiltDeg = tiltDeg;
    return true;
}

double dragScaleDegPerPixel(double fovDeg, double viewportWidth) noexcept {
    if (!std::isfinite(fovDeg) || !(fovDeg > 0.0)) {
        return 0.0;
    }
    if (!std::isfinite(viewportWidth) || !(viewportWidth > 0.0)) {
        return 0.0;
    }
    // One viewport width of travel = one field of view of rotation.  This is
    // the whole reason a drag feels identical at 30 and at 150 degrees: the
    // rate is proportional to the FOV, so the angular size of what is under
    // the cursor cancels out.
    return fovDeg / viewportWidth;
}

DragMode resolveDragMode(Handle grabbed, std::uint32_t modifiers) noexcept {
    switch (grabbed) {
    case Handle::Roll:
        // A deliberate grab of a handle is never overridden by a modifier.
        return DragMode::Roll;
    case Handle::Fov:
        return DragMode::Fov;
    case Handle::PanTilt:
        break;
    case Handle::None:
    default:
        return DragMode::None;
    }

    // Open picture: the modifiers decide.  Ctrl wins over Alt when both are
    // held, arbitrarily but consistently - a rule is better than a race.
    if ((modifiers & kModCmdCtrl) != 0u) {
        return DragMode::Fov;
    }
    if ((modifiers & kModOptAlt) != 0u) {
        return DragMode::Roll;
    }
    if ((modifiers & kModShift) != 0u) {
        // The axis is not chosen here: applyDrag() picks it from the travel
        // once the gesture has moved far enough to be unambiguous, and
        // remembers it in the state.  PanOnly is the placeholder until then.
        return DragMode::PanOnly;
    }
    return DragMode::PanTilt;
}

std::uint32_t changedFieldsFor(DragMode mode) noexcept {
    switch (mode) {
    case DragMode::PanTilt:
        return kChangedPan | kChangedTilt;
    case DragMode::PanOnly:
        return kChangedPan;
    case DragMode::TiltOnly:
        return kChangedTilt;
    case DragMode::Roll:
        return kChangedRoll;
    case DragMode::Fov:
        return kChangedFov;
    case DragMode::None:
    default:
        return kChangedNone;
    }
}

CameraValues sanitise(const CameraValues& values) noexcept {
    CameraValues out;

    // Pan and Roll are unbounded dials (a full turn must be keyframeable),
    // so they are only checked for finiteness, never clamped.
    out.panDeg = std::isfinite(values.panDeg) ? values.panDeg : OSV_REFRAME_PAN_DEFAULT;
    out.rollDeg = std::isfinite(values.rollDeg) ? values.rollDeg : OSV_REFRAME_ROLL_DEFAULT;

    // Tilt clamps at the pole: the geometry clamps there anyway
    // (ReframeCpu.cpp buildParams), and letting the control run past it
    // makes the picture stick while the number keeps climbing, which reads
    // as a broken control.
    out.tiltDeg = std::isfinite(values.tiltDeg)
                      ? std::clamp(values.tiltDeg, -OSV_REFRAME_TILT_LIMIT_DEG, OSV_REFRAME_TILT_LIMIT_DEG)
                      : OSV_REFRAME_TILT_DEFAULT;

    // FOV clamps to the parameter's VALID range, not its slider range: the
    // slider is only the comfortable part of a wider legal interval, and the
    // presets themselves (Asteroid at 300) live outside it.
    out.fovDeg = std::isfinite(values.fovDeg)
                     ? std::clamp(values.fovDeg, OSV_REFRAME_FOV_VALID_MIN, OSV_REFRAME_FOV_VALID_MAX)
                     : OSV_REFRAME_FOV_DEFAULT;
    return out;
}

CameraValues mergeLiveReadout(const CameraValues& fromHost, const CameraValues& live,
                              std::uint32_t touchedFields) noexcept {
    // Sanitise both sides BEFORE comparing: a NaN compares unequal to
    // everything, which would silently turn "consistent" into "not", and a
    // NaN must never reach the screen either way.
    const CameraValues host = sanitise(fromHost);
    const CameraValues gesture = sanitise(live);

    // One field at a time: (host, gesture, the field's bit).  A table keeps
    // the four checks identical by construction rather than by copy-paste.
    struct Field {
        double hostValue;
        double gestureValue;
        std::uint32_t bit;
    };
    const Field fields[] = {
        {host.panDeg, gesture.panDeg, kChangedPan},
        {host.tiltDeg, gesture.tiltDeg, kChangedTilt},
        {host.rollDeg, gesture.rollDeg, kChangedRoll},
        {host.fovDeg, gesture.fovDeg, kChangedFov},
    };

    // Consistency: every field the gesture never wrote must already agree
    // with the host.  If one does not, `live` describes some other state and
    // the host stays the authority - wholesale, not field by field, because
    // a mismatch means NONE of the gesture's numbers can be trusted here.
    for (const Field& f : fields) {
        if ((touchedFields & f.bit) == 0u && std::fabs(f.hostValue - f.gestureValue) > kLiveReadoutMatchEpsDeg) {
            return host;
        }
    }

    // Consistent: the touched fields come from the gesture, the rest from
    // the host (which agrees with the gesture on them anyway).
    CameraValues out = host;
    if ((touchedFields & kChangedPan) != 0u) {
        out.panDeg = gesture.panDeg;
    }
    if ((touchedFields & kChangedTilt) != 0u) {
        out.tiltDeg = gesture.tiltDeg;
    }
    if ((touchedFields & kChangedRoll) != 0u) {
        out.rollDeg = gesture.rollDeg;
    }
    if ((touchedFields & kChangedFov) != 0u) {
        out.fovDeg = gesture.fovDeg;
    }
    return out;
}

CameraValues applyDrag(DragState& state, const PointF& current, std::uint32_t modifiers) noexcept {
    // Nothing in flight, or a garbage position: hand back what we started
    // with, unchanged.  Returning the START values rather than some partial
    // result means a bad event can never move the picture.
    if (!state.active || !isFinitePoint(current) || !state.layout.valid) {
        return sanitise(state.start);
    }

    // Re-resolve the mode every event so a modifier pressed or released
    // MID-DRAG takes effect at once.  A Shift drag keeps whichever axis it
    // already locked, because re-deciding would swap axes under the hand.
    DragMode mode = resolveDragMode(state.handle, modifiers);

    const double dxTotal = current.x - state.anchor.x;
    const double dyTotal = current.y - state.anchor.y;

    if (mode == DragMode::PanOnly || mode == DragMode::TiltOnly) {
        if (state.axisLocked) {
            // Already decided; keep it.
            mode = state.mode;
        } else if (std::fabs(dxTotal) >= kAxisLockThresholdPx || std::fabs(dyTotal) >= kAxisLockThresholdPx) {
            // Far enough from the anchor for the dominant axis to be
            // meaningful.  Ties go to Pan, which is the far more common
            // intent when reframing a 360 shot.
            mode = (std::fabs(dxTotal) >= std::fabs(dyTotal)) ? DragMode::PanOnly : DragMode::TiltOnly;
            state.axisLocked = true;
        } else {
            // Too close to call: move nothing at all this event rather than
            // guessing and jittering between the two axes.
            state.mode = mode;
            state.last = current;
            return sanitise(state.start);
        }
    } else {
        // Leaving a Shift drag (or never being in one) releases the lock, so
        // pressing Shift again later re-decides from the new travel.
        state.axisLocked = false;
    }

    state.mode = mode;

    // Every mode computes from the ANCHOR, not from the previous event.
    // Accumulating per-event deltas would let rounding drift the value away
    // from where the cursor actually is over a long drag; anchoring means
    // returning the cursor to the grab point returns the value exactly.
    CameraValues result = state.start;

    switch (mode) {
    case DragMode::PanTilt:
    case DragMode::PanOnly:
    case DragMode::TiltOnly: {
        // Grab the sphere: keep the grabbed direction under the pointer,
        // solved through the renderer's own camera (see SphereGrab).  The
        // fixed rate below is the fallback when no grab could be built or
        // the pointer has left the projection's valid area.
        CameraValues grabbed;
        if (solveSphereGrab(state.grab, state.layout, state.start, current, mode, grabbed)) {
            result.panDeg = grabbed.panDeg;
            result.tiltDeg = grabbed.tiltDeg;
            break;
        }

        // Rate scales with the FOV AT GRAB TIME.  Using the live FOV would
        // change the rate mid-drag if a preset or another keyframe moved it,
        // which would make the picture slide out from under the cursor.
        const double rate = dragScaleDegPerPixel(state.start.fovDeg, state.layout.viewport.w);

        // The signs are derived in the header from the kernel's ray,
        // rotation order and equirect lookup.  In short: increasing Pan
        // moves the view toward smaller longitude and increasing Tilt moves
        // it up, so "the world follows the hand" is +dx -> +Pan and
        // +dy (downward) -> +Tilt.
        if (mode != DragMode::TiltOnly) {
            result.panDeg = state.start.panDeg + rate * dxTotal;
        }
        if (mode != DragMode::PanOnly) {
            result.tiltDeg = state.start.tiltDeg + rate * dyTotal;
        }
        break;
    }

    case DragMode::Roll: {
        if (state.handle == Handle::Roll) {
            // Grabbed the ring: roll by the angle actually swept about the
            // centre, so the ring tracks the finger exactly.  Measuring the
            // true swept angle rather than a pixel rate is what makes the
            // handle feel attached to the pointer.
            const double nowAngle = angleAboutDeg(state.layout.centre, current);
            const double swept = wrapDeltaDeg(nowAngle - state.startRollAngleDeg);

            // Screen angle grows clockwise; Roll is a right-handed rotation
            // about the view axis (+Y body), which rotates the IMAGE
            // counter-clockwise (VirtualCamera.h).  Dragging the ring
            // clockwise must turn the picture clockwise, so the sign flips.
            result.rollDeg = state.start.rollDeg - swept;
        } else {
            // Alt + drag on open picture: no ring to sweep, so a plain
            // horizontal rate.  Dragging right rolls the picture clockwise,
            // matching the ring's direction at the top of the circle.
            result.rollDeg = state.start.rollDeg - kRollDegPerPixelAltDrag * dxTotal;
        }
        break;
    }

    case DragMode::Fov: {
        // Vertical drag only: dragging DOWN widens.  Pulling a corner grip
        // outward (down, away from the centre for the bottom grips) showing
        // more of the world is the resize-grip mental model, and Ctrl+drag
        // inherits the same direction so the two never disagree.
        result.fovDeg = state.start.fovDeg + kFovDegPerPixel * dyTotal;
        break;
    }

    case DragMode::None:
    default:
        break;
    }

    state.last = current;
    return sanitise(result);
}

}  // namespace osv::reframe::ui
