// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ui.cpp - the interactive Program Monitor overlay.
//
// Two very different kinds of test live here and the split matters:
//
//   1. INTERACTION MATHS.  ReframeUi.cpp is compiled into this executable,
//      so the drag-to-degrees mapping is exercised as pure arithmetic with
//      no host, no window and no DrawBot.  These tests pin the SIGNS of the
//      mapping, which is the single thing most likely to break silently: a
//      reframe tool whose drag goes the wrong way is not subtly wrong, it is
//      unusable, and nothing else in the build would notice.
//
//   2. THE MODULE'S BEHAVIOUR.  The BUILT .aex is driven through the mock
//      host: its out-flags, the custom UI it registers, and a synthetic
//      DO_CLICK / DRAG / release sequence whose effect on the parameter
//      array is read back.  That is what proves the event plumbing writes
//      values with PF_ChangeFlag_CHANGED_VALUE, which is what makes the host
//      record a keyframe.
//
// The sign convention under test, derived in ReframeUi.h from the kernel:
//
//   drag RIGHT (+dx) -> Pan INCREASES   (the picture travels right with the
//                                        hand; increasing Pan moves the view
//                                        toward smaller longitude)
//   drag DOWN  (+dy) -> Tilt INCREASES  (the picture travels down; increasing
//                                        Tilt looks up)
//
// If either of those ever flips, several assertions below fail by name.

#include "ReframeTestSupport.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"
#include "ReframeUi.h"

#include "osv/render/osv_kernel.h"

#include "MockHost.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AE_EffectSuites.h"
#include "AE_EffectUI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::ui;
using namespace osv::reframe::test;
using osv::premiere::mock::DrawbotOpKind;
using osv::premiere::mock::DrawbotRecord;
using osv::premiere::mock::DrawbotVertexKind;
using osv::premiere::mock::MockHost;
using Catch::Approx;

namespace {

// ---------------------------------------------------------------------------
//  Shared fixtures and helpers
// ---------------------------------------------------------------------------

/// A 1920x1080 viewport at the frame origin: the shape every interaction
/// test below uses, so the numbers in the assertions stay comparable.
constexpr double kFrameW = 1920.0;
constexpr double kFrameH = 1080.0;

[[nodiscard]] RectF fullFrame() noexcept { return RectF{0.0, 0.0, kFrameW, kFrameH}; }

/// Start a gesture at `anchor` on the handle the hit-test finds there.
///
/// Mirrors exactly what ReframeUiEvent.cpp's onDoClick does, so a test of
/// the maths and the real event path cannot drift apart in how a drag is
/// set up.
[[nodiscard]] DragState beginDrag(const Layout& layout, const PointF& anchor, const CameraValues& start,
                                  std::uint32_t modifiers) {
    DragState state;
    state.active = true;
    state.handle = hitTest(layout, anchor);
    state.anchor = anchor;
    state.last = anchor;
    state.layout = layout;
    state.start = start;
    state.mode = resolveDragMode(state.handle, modifiers);
    state.axisLocked = false;
    if (state.handle == Handle::Roll) {
        const double dx = anchor.x - layout.centre.x;
        const double dy = anchor.y - layout.centre.y;
        state.startRollAngleDeg = (dx == 0.0 && dy == 0.0) ? 0.0 : std::atan2(dy, dx) * (180.0 / 3.14159265358979323846);
    }
    return state;
}

/// A mock host plus one effect reference and its parameter array, set up the
/// way a custom-UI event arrives: GLOBAL_SETUP and PARAMS_SETUP have run, so
/// the parameters exist with their defaults.
struct UiFixture {
    MockHost host;
    PF_ProgPtr ref = nullptr;
    PF_InData in{};
    PF_OutData out{};

    UiFixture() {
        ref = host.createEffectRef(0x2000, 11);
        REQUIRE(ref != nullptr);

        in = host.makeInData(ref, {});
        out = host.makeOutData();

        // The host asks these two before it ever sends an event.
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);

        // in.num_params only becomes correct once the parameters exist, and
        // the overlay reads in.width / in.height for the viewport.
        in = host.makeInData(ref, {});
        in.width = static_cast<A_long>(kFrameW);
        in.height = static_cast<A_long>(kFrameH);

        // [WP-LENSUI] The effect's default lens is DJI; the overlay tests
        // below drive the Classic camera (the FOV grip, FOV-scaled drags)
        // unless they select DJI themselves (setDjiLens), so the fixture
        // starts on Classic.
        std::vector<PF_ParamDef> added = host.addedParams(ref);
        REQUIRE(added.size() >= static_cast<std::size_t>(kIndexLens));
        PF_ParamDef lens = added[static_cast<std::size_t>(kIndexLens) - 1u];
        lens.u.pd.value = static_cast<A_long>(LensPopup::Classic);
        host.setParamValue(ref, kIndexLens, lens);
    }

    ~UiFixture() {
        if (ref) {
            host.destroyEffectRef(ref);
        }
    }
    UiFixture(const UiFixture&) = delete;
    UiFixture& operator=(const UiFixture&) = delete;

    /// The parameter array the host would hand an event, with every change
    /// flag cleared so a test can tell which parameters the event marked.
    [[nodiscard]] std::vector<PF_ParamDef*> params() {
        std::vector<PF_ParamDef*> array = host.renderParams(ref);
        for (PF_ParamDef* def : array) {
            if (def) {
                def->uu.change_flags = PF_ChangeFlag_NONE;
            }
        }
        return array;
    }

    /// Set one angle parameter's value, in degrees (PF_Fixed 16.16).
    void setAngle(A_long index, double degrees) {
        std::vector<PF_ParamDef> added = host.addedParams(ref);
        REQUIRE(index >= 1);
        REQUIRE(static_cast<std::size_t>(index) <= added.size());
        PF_ParamDef def = added[static_cast<std::size_t>(index) - 1u];
        def.u.ad.value = static_cast<PF_Fixed>(std::lround(degrees * 65536.0));
        host.setParamValue(ref, index, def);
    }

    /// Set the FOV slider's value, in degrees.
    void setFov(double degrees) {
        std::vector<PF_ParamDef> added = host.addedParams(ref);
        PF_ParamDef def = added[static_cast<std::size_t>(kIndexFov) - 1u];
        def.u.fs_d.value = static_cast<PF_FpShort>(degrees);
        host.setParamValue(ref, kIndexFov, def);
    }

    /// Run one PF_Cmd_EVENT with the given extra.
    [[nodiscard]] PF_Err event(PF_EventExtra& extra, std::vector<PF_ParamDef*>& array) {
        return LoadedPlugin::instance().effectMain()(PF_Cmd_EVENT, &in, &out, array.data(), nullptr, &extra);
    }
};

/// A zeroed PF_EventExtra for the given event type, with a comp-window
/// context from the mock host.
[[nodiscard]] PF_EventExtra makeExtra(MockHost& host, PF_EventType type) {
    PF_EventExtra extra;
    std::memset(&extra, 0, sizeof(extra));
    extra.e_type = type;
    extra.contextH = host.makeCustomUiContext(PF_Window_COMP);
    extra.evt_in_flags = PF_EI_NONE;
    extra.evt_out_flags = PF_EO_NONE;
    return extra;
}

/// Degrees stored in an angle parameter.
[[nodiscard]] double angleOf(const std::vector<PF_ParamDef*>& array, int index) {
    REQUIRE(array.size() > static_cast<std::size_t>(index));
    REQUIRE(array[static_cast<std::size_t>(index)] != nullptr);
    return static_cast<double>(array[static_cast<std::size_t>(index)]->u.ad.value) / 65536.0;
}

/// Degrees stored in the FOV slider.
[[nodiscard]] double fovOf(const std::vector<PF_ParamDef*>& array) {
    return static_cast<double>(array[static_cast<std::size_t>(kIndexFov)]->u.fs_d.value);
}

/// Whether a parameter was marked as changed by the event just run.
[[nodiscard]] bool changed(const std::vector<PF_ParamDef*>& array, int index) {
    return (array[static_cast<std::size_t>(index)]->uu.change_flags & PF_ChangeFlag_CHANGED_VALUE) != 0;
}

/// The direction of the sphere under `pointer` for a camera, cast with the
/// RENDERER's own camera (buildView) and rotation (its Rout).  Deliberately
/// independent of the overlay's grab code - it is what the grab is checked
/// against - and it is the same pixel -> ray -> Rout chain the effect's
/// kernel runs for every output pixel.
[[nodiscard]] bool rayUnderPointer(const Layout& layout, double fovDeg, double distortion, const CameraValues& cam,
                                   const PointF& pointer, double out[3]) {
    Settings s;
    s.fovDeg = fovDeg;
    s.distortion = distortion;
    s.panDeg = cam.panDeg;
    s.tiltDeg = cam.tiltDeg;
    s.rollDeg = cam.rollDeg;
    const int w = static_cast<int>(std::lround(layout.viewport.w));
    const int h = static_cast<int>(std::lround(layout.viewport.h));
    const ViewSetup view = buildView(s, w, h, SizePx{w, h});
    if (!view.valid) {
        return false;
    }
    const OsvReframeParams& p = view.params;
    const float nx = static_cast<float>((pointer.x - layout.viewport.x) - 0.5 * layout.viewport.w);
    const float ny = static_cast<float>(0.5 * layout.viewport.h - (pointer.y - layout.viewport.y));
    float d[3] = {0.0f, 0.0f, 0.0f};
    if (!osvViewRay(p.projection, p.focalPx, p.eyeOffset, p.tanHalfH, p.tanHalfV, static_cast<float>(w),
                    static_cast<float>(h), nx, ny, d)) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        out[r] = static_cast<double>(p.Rout[r * 3 + 0]) * d[0] + static_cast<double>(p.Rout[r * 3 + 1]) * d[1] +
                 static_cast<double>(p.Rout[r * 3 + 2]) * d[2];
    }
    const double n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (!(n > 0.0)) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        out[r] /= n;
    }
    return true;
}

/// Angle between two unit vectors, in degrees.
[[nodiscard]] double angleBetweenDeg(const double a[3], const double b[3]) {
    const double dot = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0);
    return std::acos(dot) * (180.0 / 3.14159265358979323846);
}

/// The Pan a module-level drag of `dxPx` pixels to the right, starting on
/// the frame centre, commits with the fixture's level camera: the axis drag
/// turns Pan by the azimuth the picture spans along the horizontal centre
/// line, which from the centre is the azimuth of the ray under the pointer,
/// atan2(x, y).  Computed through the renderer's camera, never from a fixed
/// degrees-per-pixel rate.
[[nodiscard]] double grabPanDeg(double dxPx, double fovDeg = 90.0,
                                double distortion = OSV_REFRAME_DISTORTION_DEFAULT) {
    const Layout layout = computeLayout(fullFrame());
    CameraValues level;
    level.fovDeg = fovDeg;
    double v[3];
    REQUIRE(rayUnderPointer(layout, fovDeg, distortion, level, PointF{layout.centre.x + dxPx, layout.centre.y}, v));
    return std::atan2(v[0], v[1]) * (180.0 / 3.14159265358979323846);
}

/// The Tilt the same drag commits for `dyPx` pixels DOWN from the centre:
/// minus the elevation of the ray under the pointer on the vertical centre
/// line (down -> +Tilt, the picture travels with the hand).
[[nodiscard]] double grabTiltDeg(double dyPx, double fovDeg = 90.0,
                                 double distortion = OSV_REFRAME_DISTORTION_DEFAULT) {
    const Layout layout = computeLayout(fullFrame());
    CameraValues level;
    level.fovDeg = fovDeg;
    double v[3];
    REQUIRE(rayUnderPointer(layout, fovDeg, distortion, level, PointF{layout.centre.x, layout.centre.y + dyPx}, v));
    return -std::atan2(v[2], v[1]) * (180.0 / 3.14159265358979323846);
}

/// A grab started the way the shim starts one: the renderer's camera for the
/// layout's viewport, cast through the anchor.
[[nodiscard]] SphereGrab grabFor(const Layout& layout, double fovDeg, double distortion, const CameraValues& start,
                                 const PointF& anchor) {
    Settings s;
    s.fovDeg = fovDeg;
    s.distortion = distortion;
    s.panDeg = start.panDeg;
    s.tiltDeg = start.tiltDeg;
    s.rollDeg = start.rollDeg;
    const int w = static_cast<int>(std::lround(layout.viewport.w));
    const int h = static_cast<int>(std::lround(layout.viewport.h));
    const ViewSetup view = buildView(s, w, h, SizePx{w, h});
    REQUIRE(view.valid);
    return beginSphereGrab(view.params.projection, view.params.focalPx, view.params.eyeOffset, view.params.tanHalfH,
                           view.params.tanHalfV, layout, start, anchor);
}

}  // namespace

// ===========================================================================
//  1. Layout and hit-testing
// ===========================================================================

TEST_CASE("the layout is derived from the viewport's short side", "[reframe][ui]") {
    const Layout wide = computeLayout(fullFrame());
    REQUIRE(wide.valid);

    // Centre is the viewport centre, so the crosshair marks the frame's
    // middle and not the window's.
    CHECK(wide.centre.x == Approx(960.0));
    CHECK(wide.centre.y == Approx(540.0));

    // Every handle sizes off the SHORT side (1080 here), which is what keeps
    // the overlay looking the same on a 9:16 crop as on a 2.35:1 one.
    CHECK(wide.rollRingRadius == Approx(0.5 * 1080.0 * kRollRingRadiusFraction));
    CHECK(wide.fovGripSize == Approx(1080.0 * kFovGripFraction));

    // A tall frame of the same short side gets identical handles.
    const Layout tall = computeLayout(RectF{0.0, 0.0, 1080.0, 1920.0});
    REQUIRE(tall.valid);
    CHECK(tall.rollRingRadius == Approx(wide.rollRingRadius));
    CHECK(tall.fovGripSize == Approx(wide.fovGripSize));
}

TEST_CASE("a degenerate viewport yields an invalid layout instead of dividing by zero", "[reframe][ui]") {
    // Every one of these is something a real host can report: a collapsed
    // panel, a zero-size comp, or a transform that produced infinities.
    const RectF cases[] = {
        RectF{0.0, 0.0, 0.0, 0.0},
        RectF{0.0, 0.0, -100.0, 100.0},
        RectF{0.0, 0.0, 100.0, 0.0},
        RectF{0.0, 0.0, std::numeric_limits<double>::quiet_NaN(), 100.0},
        RectF{0.0, 0.0, std::numeric_limits<double>::infinity(), 100.0},
    };
    for (const RectF& r : cases) {
        const Layout layout = computeLayout(r);
        CHECK_FALSE(layout.valid);
        // And every consumer answers safely on it.
        CHECK(hitTest(layout, PointF{10.0, 10.0}) == Handle::None);
        CHECK_FALSE(onRollArc(layout, PointF{10.0, 10.0}));
        RectF grips[4];
        CHECK(fovGripRects(layout, grips) == 0);
    }
}

TEST_CASE("clicking open picture is a pan/tilt drag", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    REQUIRE(layout.valid);

    // Dead centre: unambiguously open picture (the ring is far out, the
    // grips are in the corners).
    CHECK(hitTest(layout, layout.centre) == Handle::PanTilt);

    // Just inside the ring's radius but on the vertical, where no arc is
    // drawn - so the ring must NOT steal it.
    const PointF aboveCentre{layout.centre.x, layout.centre.y - layout.rollRingRadius};
    CHECK(hitTest(layout, aboveCentre) == Handle::PanTilt);

    // Outside the picture is nobody's click.
    CHECK(hitTest(layout, PointF{-5.0, 500.0}) == Handle::None);
    CHECK(hitTest(layout, PointF{500.0, kFrameH + 5.0}) == Handle::None);
}

TEST_CASE("clicking a corner grip is an FOV drag", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    RectF grips[4];
    REQUIRE(fovGripRects(layout, grips) == 4);

    // Order is fixed and the drawing code depends on it: TL, TR, BL, BR.
    CHECK(grips[0].x == Approx(0.0));
    CHECK(grips[0].y == Approx(0.0));
    CHECK(grips[1].x == Approx(kFrameW - layout.fovGripSize));
    CHECK(grips[2].y == Approx(kFrameH - layout.fovGripSize));
    CHECK(grips[3].x == Approx(kFrameW - layout.fovGripSize));
    CHECK(grips[3].y == Approx(kFrameH - layout.fovGripSize));

    for (const RectF& g : grips) {
        CHECK(hitTest(layout, g.centre()) == Handle::Fov);
    }
}

TEST_CASE("the roll ring is grabbable only where it is drawn", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    REQUIRE(layout.valid);

    // On the horizontal, at the ring's radius: dead centre of a drawn arc.
    const PointF onRight{layout.centre.x + layout.rollRingRadius, layout.centre.y};
    const PointF onLeft{layout.centre.x - layout.rollRingRadius, layout.centre.y};
    CHECK(onRollArc(layout, onRight));
    CHECK(onRollArc(layout, onLeft));
    CHECK(hitTest(layout, onRight) == Handle::Roll);
    CHECK(hitTest(layout, onLeft) == Handle::Roll);

    // At the ring's radius but straight up, where nothing is drawn: this is
    // open picture, because a control the user cannot see is worse than none.
    const PointF onTop{layout.centre.x, layout.centre.y - layout.rollRingRadius};
    CHECK_FALSE(onRollArc(layout, onTop));

    // Well inside the ring on the horizontal: past the grab band, so it is
    // open picture again.
    const PointF insideRing{layout.centre.x + layout.rollRingRadius - 3.0 * layout.rollGrabBand, layout.centre.y};
    CHECK_FALSE(onRollArc(layout, insideRing));
    CHECK(hitTest(layout, insideRing) == Handle::PanTilt);

    // Just past the arc's angular extent: not grabbable.
    const double justPast = (kRollArcHalfSweepDeg + 4.0) * (3.14159265358979323846 / 180.0);
    const PointF pastArc{layout.centre.x + layout.rollRingRadius * std::cos(justPast),
                         layout.centre.y + layout.rollRingRadius * std::sin(justPast)};
    CHECK_FALSE(onRollArc(layout, pastArc));
}

// ===========================================================================
//  2. The drag maths - signs, scaling and constraints
// ===========================================================================

TEST_CASE("a drag scales with the FOV so it feels the same at every zoom", "[reframe][ui]") {
    // The contract: dragging across the full viewport width turns the view
    // by exactly one field of view, so the content under the cursor stays
    // under the cursor.
    CHECK(dragScaleDegPerPixel(90.0, 1920.0) == Approx(90.0 / 1920.0));
    CHECK(dragScaleDegPerPixel(30.0, 1920.0) == Approx(30.0 / 1920.0));
    CHECK(dragScaleDegPerPixel(150.0, 1920.0) == Approx(150.0 / 1920.0));

    // A 150-degree view moves five times as fast per pixel as a 30-degree
    // one, which is exactly what makes the picture track the hand at both.
    CHECK(dragScaleDegPerPixel(150.0, 1920.0) == Approx(5.0 * dragScaleDegPerPixel(30.0, 1920.0)));

    // Garbage in, zero out - never a NaN that would poison a parameter.
    CHECK(dragScaleDegPerPixel(0.0, 1920.0) == 0.0);
    CHECK(dragScaleDegPerPixel(-90.0, 1920.0) == 0.0);
    CHECK(dragScaleDegPerPixel(90.0, 0.0) == 0.0);
    CHECK(dragScaleDegPerPixel(std::numeric_limits<double>::quiet_NaN(), 1920.0) == 0.0);
}

TEST_CASE("dragging right turns the view so the world follows the cursor", "[reframe][ui][signs]") {
    // THE sign test.  If this fails, the reframe drag goes the wrong way.
    //
    // Derivation (ReframeUi.h, from osv_kernel.h and VirtualCamera.cpp):
    // increasing Pan moves the view centre toward SMALLER longitude, so to
    // make the picture travel RIGHT with the hand - bringing content from
    // the left of centre into view - Pan must INCREASE.
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;

    DragState state = beginDrag(layout, layout.centre, start, kModNone);
    REQUIRE(state.handle == Handle::PanTilt);
    REQUIRE(state.mode == DragMode::PanTilt);

    const double dx = 480.0;  // A quarter of the frame width.
    const CameraValues after = applyDrag(state, PointF{layout.centre.x + dx, layout.centre.y}, kModNone);

    CHECK(after.panDeg > start.panDeg);
    // A quarter of the width at a 90-degree FOV is a quarter of 90 degrees.
    CHECK(after.panDeg == Approx(kPanTiltSensitivity * 90.0 * 0.25));
    CHECK(after.tiltDeg == Approx(start.tiltDeg));
    CHECK(after.rollDeg == Approx(start.rollDeg));
    CHECK(after.fovDeg == Approx(start.fovDeg));
}

TEST_CASE("dragging left turns the view the other way", "[reframe][ui][signs]") {
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;

    DragState state = beginDrag(layout, layout.centre, start, kModNone);
    const CameraValues after = applyDrag(state, PointF{layout.centre.x - 480.0, layout.centre.y}, kModNone);

    CHECK(after.panDeg < start.panDeg);
    CHECK(after.panDeg == Approx(-kPanTiltSensitivity * 90.0 * 0.25));
}

TEST_CASE("dragging down tilts the view up so the world follows the cursor", "[reframe][ui][signs]") {
    // Screen y grows DOWNWARD.  Dragging down makes the picture travel down,
    // which brings content from ABOVE into view, which means looking UP,
    // which means Tilt INCREASES (Rx(tilt) sends latitude to +tilt).
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;

    DragState state = beginDrag(layout, layout.centre, start, kModNone);
    const double dy = 270.0;
    const CameraValues after = applyDrag(state, PointF{layout.centre.x, layout.centre.y + dy}, kModNone);

    CHECK(after.tiltDeg > start.tiltDeg);
    // The rate is the same per pixel on both axes (it is derived from the
    // viewport WIDTH), so 270px is 270 * 90/1920 degrees.
    CHECK(after.tiltDeg == Approx(kPanTiltSensitivity * 270.0 * 90.0 / 1920.0));
    CHECK(after.panDeg == Approx(start.panDeg));
}

TEST_CASE("left / right turns only Pan and up / down only Tilt, at every zoom", "[reframe][ui][grab]") {
    // The field report: zoomed far out, a left / right drag tipped the view,
    // because the full sphere grab keeps one ARBITRARY grabbed point under the
    // pointer and a far off-centre point moves along a circle of latitude.
    // The drag is now per axis: horizontal travel moves Pan, vertical travel
    // moves Tilt, and neither ever moves the other - checked on exactly the
    // views where the coupling was worst (wide eye-offset, the 263-degree
    // view, a tiny planet, roll, a steep tilt) and on the plain ones.
    struct Case {
        const char* name;
        double fov, distortion, pan, tilt, roll;
        double ax, ay;  // anchor, fraction of the frame
        double dx, dy;  // drag, pixels
    };
    const Case cases[] = {
        {"90 deg, level", 90.0, 0.0, 0.0, 0.0, 0.0, 0.5, 0.5, 300.0, 200.0},
        {"90 deg, off-centre", 90.0, 50.0, 30.0, 20.0, 0.0, 0.25, 0.7, -250.0, 150.0},
        {"150 deg wide, rolled", 150.0, 30.0, 0.0, 30.0, 25.0, 0.6, 0.4, 120.0, -160.0},
        {"263 deg (field report)", 263.0, 40.0, 143.4, -0.2, 0.0, 0.55, 0.45, 180.0, 150.0},
        {"tiny planet 300 deg", 300.0, 100.0, 0.0, -60.0, 0.0, 0.5, 0.35, 90.0, 60.0},
        {"narrow 60 deg, steep tilt, rolled", 60.0, 0.0, -40.0, 60.0, 10.0, 0.45, 0.55, 50.0, -40.0},
    };
    const Layout layout = computeLayout(fullFrame());
    REQUIRE(layout.valid);
    for (const Case& c : cases) {
        INFO(c.name);
        CameraValues start;
        start.fovDeg = c.fov;
        start.panDeg = c.pan;
        start.tiltDeg = c.tilt;
        start.rollDeg = c.roll;
        const PointF anchor{c.ax * kFrameW, c.ay * kFrameH};
        auto dragTo = [&](double dx, double dy) {
            DragState state = beginDrag(layout, anchor, start, kModNone);
            REQUIRE(state.handle == Handle::PanTilt);
            state.grab = grabFor(layout, c.fov, c.distortion, start, anchor);
            REQUIRE(state.grab.cameraValid);
            return applyDrag(state, PointF{anchor.x + dx, anchor.y + dy}, kModNone);
        };

        // A purely horizontal drag: Pan moves, Tilt does not move at all.
        const CameraValues h = dragTo(c.dx, 0.0);
        CHECK(h.panDeg != Approx(c.pan));
        CHECK(h.tiltDeg == c.tilt);
        // A purely vertical drag: Tilt moves, Pan does not move at all.
        const CameraValues v = dragTo(0.0, c.dy);
        CHECK(v.panDeg == c.pan);
        CHECK(v.tiltDeg != Approx(c.tilt));
        // A diagonal drag is exactly the two combined.
        const CameraValues d = dragTo(c.dx, c.dy);
        CHECK(d.panDeg == Approx(h.panDeg).margin(1e-9));
        CHECK(d.tiltDeg == Approx(v.tiltDeg).margin(1e-9));
        // Nothing else moves.
        CHECK(d.rollDeg == Approx(c.roll));
        CHECK(d.fovDeg == Approx(c.fov));
    }
}

TEST_CASE("on a level view what sits on the centre lines stays under the pointer", "[reframe][ui][grab]") {
    // The per-axis drag still drags the SCENE, not a dial: the angles it
    // turns by are the ones the picture spans along the centre lines,
    // through the renderer's own camera, so on those lines the grabbed
    // content is exactly under the pointer after the move - at 90 degrees
    // and zoomed far out alike.
    const Layout layout = computeLayout(fullFrame());
    for (const double fov : {90.0, 200.0, 300.0}) {
        for (const double distortion : {0.0, 60.0}) {
            INFO("fov " << fov << ", distortion " << distortion);
            CameraValues start;
            start.fovDeg = fov;
            start.panDeg = 40.0;
            // Horizontal: a point on the horizontal centre line (level view).
            {
                const PointF anchor{layout.centre.x - 200.0, layout.centre.y};
                const PointF to{anchor.x + 350.0 / kPanTiltSensitivity, anchor.y};
                DragState state = beginDrag(layout, anchor, start, kModNone);
                state.grab = grabFor(layout, fov, distortion, start, anchor);
                const CameraValues after = applyDrag(state, to, kModNone);
                double grabbed[3];
                double now[3];
                REQUIRE(rayUnderPointer(layout, fov, distortion, start, anchor, grabbed));
                REQUIRE(rayUnderPointer(layout, fov, distortion, after, PointF{anchor.x + 350.0, anchor.y}, now));
                CHECK(angleBetweenDeg(grabbed, now) < 0.01);
            }
            // Vertical: a point on the vertical centre line, at any pan and
            // tilt (only roll would take the line out of the tilt plane).
            {
                CameraValues tilted = start;
                tilted.tiltDeg = -25.0;
                const PointF anchor{layout.centre.x, layout.centre.y - 120.0};
                const PointF to{anchor.x, anchor.y + 200.0 / kPanTiltSensitivity};
                DragState state = beginDrag(layout, anchor, tilted, kModNone);
                state.grab = grabFor(layout, fov, distortion, tilted, anchor);
                const CameraValues after = applyDrag(state, to, kModNone);
                double grabbed[3];
                double now[3];
                REQUIRE(rayUnderPointer(layout, fov, distortion, tilted, anchor, grabbed));
                REQUIRE(rayUnderPointer(layout, fov, distortion, after, PointF{anchor.x, anchor.y + 200.0}, now));
                CHECK(angleBetweenDeg(grabbed, now) < 0.01);
            }
        }
    }
}

TEST_CASE("the full sphere grab still keeps an arbitrary grabbed point under the pointer", "[reframe][ui][grab]") {
    // solveSphereGrab is no longer what the overlay drags with, but it stays
    // a public, exact solve: whatever direction was under the anchor is under
    // the pointer after it.
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 150.0;
    start.tiltDeg = 30.0;
    start.rollDeg = 25.0;
    const PointF anchor{0.6 * kFrameW, 0.4 * kFrameH};
    const SphereGrab grab = grabFor(layout, 150.0, 30.0, start, anchor);
    REQUIRE(grab.valid);
    const PointF to{anchor.x + 120.0, anchor.y - 160.0};
    CameraValues after;
    REQUIRE(solveSphereGrab(grab, layout, start, to, DragMode::PanTilt, after));
    double grabbed[3];
    double now[3];
    REQUIRE(rayUnderPointer(layout, 150.0, 30.0, start, anchor, grabbed));
    REQUIRE(rayUnderPointer(layout, 150.0, 30.0, after, to, now));
    CHECK(angleBetweenDeg(grabbed, now) < 0.01);
}

TEST_CASE("a grab moves the world with the hand in both axes", "[reframe][ui][grab][signs]") {
    // Same signs as the fixed-rate drag always had - right -> +Pan, down ->
    // +Tilt - so muscle memory and old keyframes agree with the new drag.
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;
    for (const double distortion : {0.0, 60.0}) {
        DragState right = beginDrag(layout, layout.centre, start, kModNone);
        right.grab = grabFor(layout, 90.0, distortion, start, layout.centre);
        const CameraValues r = applyDrag(right, PointF{layout.centre.x + 200.0, layout.centre.y}, kModNone);
        CHECK(r.panDeg > 0.0);
        CHECK(r.tiltDeg == Approx(0.0).margin(1e-9));

        DragState down = beginDrag(layout, layout.centre, start, kModNone);
        down.grab = grabFor(layout, 90.0, distortion, start, layout.centre);
        const CameraValues d = applyDrag(down, PointF{layout.centre.x, layout.centre.y + 200.0}, kModNone);
        CHECK(d.tiltDeg > 0.0);
        CHECK(d.panDeg == Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("returning the pointer to the grab point restores the start exactly", "[reframe][ui][grab]") {
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 200.0;
    start.panDeg = 77.0;
    start.tiltDeg = -35.0;
    start.rollDeg = 12.0;
    const PointF anchor{700.0, 300.0};
    DragState state = beginDrag(layout, anchor, start, kModNone);
    state.grab = grabFor(layout, 200.0, 20.0, start, anchor);
    REQUIRE(state.grab.valid);
    (void)applyDrag(state, PointF{1100.0, 800.0}, kModNone);
    const CameraValues back = applyDrag(state, anchor, kModNone);
    CHECK(back.panDeg == Approx(start.panDeg).margin(1e-6));
    CHECK(back.tiltDeg == Approx(start.tiltDeg).margin(1e-6));
    CHECK(back.rollDeg == Approx(start.rollDeg));
}

TEST_CASE("a Shift-constrained grab moves only the chosen axis", "[reframe][ui][grab]") {
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 120.0;
    start.panDeg = 10.0;
    start.tiltDeg = 15.0;
    {
        DragState state = beginDrag(layout, layout.centre, start, kModShift);
        state.grab = grabFor(layout, 120.0, 30.0, start, layout.centre);
        const CameraValues v = applyDrag(state, PointF{layout.centre.x + 300.0, layout.centre.y + 40.0}, kModShift);
        CHECK(v.panDeg != Approx(start.panDeg));
        CHECK(v.tiltDeg == Approx(start.tiltDeg));
    }
    {
        DragState state = beginDrag(layout, layout.centre, start, kModShift);
        state.grab = grabFor(layout, 120.0, 30.0, start, layout.centre);
        const CameraValues v = applyDrag(state, PointF{layout.centre.x + 30.0, layout.centre.y + 250.0}, kModShift);
        CHECK(v.panDeg == Approx(start.panDeg));
        CHECK(v.tiltDeg != Approx(start.tiltDeg));
    }
}

TEST_CASE("dragging a point past the pole clamps the tilt instead of flipping the view", "[reframe][ui][grab]") {
    // Grab something near the top of a steeply tilted view and drag it far
    // down: the exact solution would need a tilt beyond +90.  The dial must
    // stop at the limit, stay finite, and never jump to the other root (a
    // sudden 180-degree flip is the classic grab-the-sphere failure).
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 100.0;
    start.tiltDeg = 80.0;
    const PointF anchor{960.0, 200.0};
    DragState state = beginDrag(layout, anchor, start, kModNone);
    state.grab = grabFor(layout, 100.0, 0.0, start, anchor);
    REQUIRE(state.grab.valid);
    double previousTilt = start.tiltDeg;
    for (double y = 220.0; y <= 1060.0; y += 40.0) {
        const CameraValues v = applyDrag(state, PointF{960.0, y}, kModNone);
        REQUIRE(std::isfinite(v.panDeg));
        REQUIRE(std::isfinite(v.tiltDeg));
        CHECK(v.tiltDeg <= OSV_REFRAME_TILT_LIMIT_DEG + 1e-9);
        CHECK(v.tiltDeg >= previousTilt - 1e-6);  // monotonic: no flip back
        previousTilt = v.tiltDeg;
    }
    CHECK(previousTilt == Approx(OSV_REFRAME_TILT_LIMIT_DEG));
}

TEST_CASE("without a grab the drag keeps its fixed-rate behaviour", "[reframe][ui][grab]") {
    // A host that gave the shim nothing to build a camera from leaves the
    // grab invalid; the drag must then be exactly the old one.
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;
    DragState state = beginDrag(layout, layout.centre, start, kModNone);
    REQUIRE_FALSE(state.grab.valid);
    const CameraValues v = applyDrag(state, PointF{layout.centre.x + 480.0, layout.centre.y}, kModNone);
    CHECK(v.panDeg == Approx(kPanTiltSensitivity * 480.0 * 90.0 / kFrameW));
}

TEST_CASE("a drag is anchored, not accumulated", "[reframe][ui]") {
    // Returning the cursor to the grab point must return the value exactly;
    // an implementation that accumulated per-event deltas would drift.
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.panDeg = 33.0;
    start.fovDeg = 120.0;

    DragState state = beginDrag(layout, layout.centre, start, kModNone);

    for (double x = 1.0; x < 500.0; x += 7.0) {
        (void)applyDrag(state, PointF{layout.centre.x + x, layout.centre.y + 3.0}, kModNone);
    }
    const CameraValues back = applyDrag(state, layout.centre, kModNone);

    CHECK(back.panDeg == Approx(33.0));
    CHECK(back.tiltDeg == Approx(0.0));
}

TEST_CASE("Shift constrains a drag to the dominant axis", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;

    SECTION("a mostly horizontal drag becomes pan only") {
        DragState state = beginDrag(layout, layout.centre, start, kModShift);
        const CameraValues after =
            applyDrag(state, PointF{layout.centre.x + 300.0, layout.centre.y + 40.0}, kModShift);

        CHECK(state.mode == DragMode::PanOnly);
        CHECK(after.panDeg == Approx(kPanTiltSensitivity * 300.0 * 90.0 / 1920.0));
        CHECK(after.tiltDeg == Approx(0.0));  // The 40px of vertical travel is discarded.
    }

    SECTION("a mostly vertical drag becomes tilt only") {
        DragState state = beginDrag(layout, layout.centre, start, kModShift);
        const CameraValues after =
            applyDrag(state, PointF{layout.centre.x + 40.0, layout.centre.y + 300.0}, kModShift);

        CHECK(state.mode == DragMode::TiltOnly);
        CHECK(after.tiltDeg == Approx(kPanTiltSensitivity * 300.0 * 90.0 / 1920.0));
        CHECK(after.panDeg == Approx(0.0));
    }

    SECTION("the axis is locked once and does not swap under the hand") {
        DragState state = beginDrag(layout, layout.centre, start, kModShift);

        // Commit to horizontal.
        (void)applyDrag(state, PointF{layout.centre.x + 200.0, layout.centre.y}, kModShift);
        REQUIRE(state.mode == DragMode::PanOnly);
        REQUIRE(state.axisLocked);

        // Now drag far more vertically.  A naive "recompute the dominant
        // axis every event" implementation would swap to tilt here and the
        // picture would lurch; the lock must hold.
        const CameraValues after =
            applyDrag(state, PointF{layout.centre.x + 10.0, layout.centre.y + 900.0}, kModShift);
        CHECK(state.mode == DragMode::PanOnly);
        CHECK(after.tiltDeg == Approx(0.0));
        CHECK(after.panDeg == Approx(kPanTiltSensitivity * 10.0 * 90.0 / 1920.0));
    }

    SECTION("a drag too small to call moves nothing") {
        DragState state = beginDrag(layout, layout.centre, start, kModShift);
        const CameraValues after = applyDrag(state, PointF{layout.centre.x + 1.0, layout.centre.y + 1.0}, kModShift);

        CHECK_FALSE(state.axisLocked);
        CHECK(after.panDeg == Approx(0.0));
        CHECK(after.tiltDeg == Approx(0.0));
    }
}

TEST_CASE("a roll-handle drag changes Roll and nothing else", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    // Grab the right-hand arc, on the horizontal.
    const PointF grab{layout.centre.x + layout.rollRingRadius, layout.centre.y};
    REQUIRE(hitTest(layout, grab) == Handle::Roll);

    CameraValues start;
    start.panDeg = 12.0;
    start.tiltDeg = -7.0;
    start.rollDeg = 0.0;
    start.fovDeg = 100.0;

    DragState state = beginDrag(layout, grab, start, kModNone);
    REQUIRE(state.mode == DragMode::Roll);

    // Move 30 degrees clockwise around the ring (screen angle grows
    // clockwise because y points down).
    const double sweptRad = 30.0 * (3.14159265358979323846 / 180.0);
    const PointF moved{layout.centre.x + layout.rollRingRadius * std::cos(sweptRad),
                       layout.centre.y + layout.rollRingRadius * std::sin(sweptRad)};
    const CameraValues after = applyDrag(state, moved, kModNone);

    // Roll is a right-handed rotation about the view axis, which turns the
    // IMAGE counter-clockwise, so a clockwise ring drag DECREASES Roll.
    CHECK(after.rollDeg == Approx(-30.0).margin(0.001));

    // And nothing else moved.
    CHECK(after.panDeg == Approx(12.0));
    CHECK(after.tiltDeg == Approx(-7.0));
    CHECK(after.fovDeg == Approx(100.0));
    CHECK(changedFieldsFor(state.mode) == kChangedRoll);
}

TEST_CASE("a roll drag across the angle wrap does not spin a full turn", "[reframe][ui]") {
    // Grabbing the LEFT arc puts the anchor near +/-180 degrees, where a
    // naive angle subtraction jumps by 360 and bakes a full spin into the
    // timeline on an auto-keyframing control.
    const Layout layout = computeLayout(fullFrame());
    const PointF grab{layout.centre.x - layout.rollRingRadius, layout.centre.y};
    REQUIRE(hitTest(layout, grab) == Handle::Roll);

    CameraValues start;
    DragState state = beginDrag(layout, grab, start, kModNone);

    // Step a few degrees past the wrap in each direction.
    for (const double deltaDeg : {5.0, -5.0, 10.0, -10.0}) {
        const double angle = (180.0 + deltaDeg) * (3.14159265358979323846 / 180.0);
        const PointF p{layout.centre.x + layout.rollRingRadius * std::cos(angle),
                       layout.centre.y + layout.rollRingRadius * std::sin(angle)};
        const CameraValues after = applyDrag(state, p, kModNone);
        // At most a few degrees of roll, never ~360.
        CHECK(std::fabs(after.rollDeg) < 45.0);
    }
}

TEST_CASE("an FOV-grip drag changes FOV only and stays in range", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    RectF grips[4];
    REQUIRE(fovGripRects(layout, grips) == 4);
    const PointF grab = grips[3].centre();  // bottom-right
    REQUIRE(hitTest(layout, grab) == Handle::Fov);

    CameraValues start;
    start.panDeg = 5.0;
    start.tiltDeg = 6.0;
    start.rollDeg = 7.0;
    start.fovDeg = 120.0;

    SECTION("dragging down widens") {
        DragState state = beginDrag(layout, grab, start, kModNone);
        REQUIRE(state.mode == DragMode::Fov);

        const CameraValues after = applyDrag(state, PointF{grab.x, grab.y + 100.0}, kModNone);
        CHECK(after.fovDeg == Approx(120.0 + 100.0 * kFovDegPerPixel));
        CHECK(after.fovDeg > start.fovDeg);

        // Nothing else moved.
        CHECK(after.panDeg == Approx(5.0));
        CHECK(after.tiltDeg == Approx(6.0));
        CHECK(after.rollDeg == Approx(7.0));
        CHECK(changedFieldsFor(state.mode) == kChangedFov);
    }

    SECTION("dragging up narrows") {
        DragState state = beginDrag(layout, grab, start, kModNone);
        const CameraValues after = applyDrag(state, PointF{grab.x, grab.y - 100.0}, kModNone);
        CHECK(after.fovDeg == Approx(120.0 - 100.0 * kFovDegPerPixel));
    }

    SECTION("a huge drag clamps to the valid range, never past it") {
        DragState wide = beginDrag(layout, grab, start, kModNone);
        const CameraValues widened = applyDrag(wide, PointF{grab.x, grab.y + 100000.0}, kModNone);
        CHECK(widened.fovDeg == Approx(OSV_REFRAME_FOV_VALID_MAX));

        DragState narrow = beginDrag(layout, grab, start, kModNone);
        const CameraValues narrowed = applyDrag(narrow, PointF{grab.x, grab.y - 100000.0}, kModNone);
        CHECK(narrowed.fovDeg == Approx(OSV_REFRAME_FOV_VALID_MIN));
    }
}

TEST_CASE("modifiers retarget a drag that started on open picture", "[reframe][ui]") {
    // Ctrl zooms, Alt rolls, and a deliberate handle grab ignores both.
    CHECK(resolveDragMode(Handle::PanTilt, kModNone) == DragMode::PanTilt);
    CHECK(resolveDragMode(Handle::PanTilt, kModShift) == DragMode::PanOnly);
    CHECK(resolveDragMode(Handle::PanTilt, kModCmdCtrl) == DragMode::Fov);
    CHECK(resolveDragMode(Handle::PanTilt, kModOptAlt) == DragMode::Roll);

    // Ctrl wins over Alt when both are held - arbitrary, but a rule.
    CHECK(resolveDragMode(Handle::PanTilt, kModCmdCtrl | kModOptAlt) == DragMode::Fov);

    // A handle grab is never overridden.
    CHECK(resolveDragMode(Handle::Roll, kModCmdCtrl) == DragMode::Roll);
    CHECK(resolveDragMode(Handle::Fov, kModOptAlt) == DragMode::Fov);

    // Caps Lock is deliberately ignored.
    CHECK(resolveDragMode(Handle::PanTilt, kModCapsLock) == DragMode::PanTilt);

    // Outside the picture there is no drag at all.
    CHECK(resolveDragMode(Handle::None, kModCmdCtrl) == DragMode::None);
}

TEST_CASE("Ctrl pressed mid-drag turns a pan into a zoom immediately", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;

    DragState state = beginDrag(layout, layout.centre, start, kModNone);

    // Pan a little with no modifier.
    (void)applyDrag(state, PointF{layout.centre.x + 100.0, layout.centre.y}, kModNone);
    REQUIRE(state.mode == DragMode::PanTilt);

    // Now press Ctrl without letting go.  The SAME gesture must become a
    // zoom, measured from the same anchor.
    const CameraValues after = applyDrag(state, PointF{layout.centre.x + 100.0, layout.centre.y + 80.0}, kModCmdCtrl);
    CHECK(state.mode == DragMode::Fov);
    CHECK(after.fovDeg == Approx(90.0 + 80.0 * kFovDegPerPixel));
    // Pan snaps back to where the gesture started, because the mode changed
    // and only FOV is being written now.
    CHECK(after.panDeg == Approx(0.0));
}

TEST_CASE("the drag maths never produces a value outside the parameter ranges", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;

    // Tilt clamps at the pole: the geometry clamps there anyway, so letting
    // the control run past would make the picture stick while the number
    // climbed.
    DragState down = beginDrag(layout, layout.centre, start, kModNone);
    const CameraValues farDown = applyDrag(down, PointF{layout.centre.x, layout.centre.y + 1.0e6}, kModNone);
    CHECK(farDown.tiltDeg == Approx(OSV_REFRAME_TILT_LIMIT_DEG));

    DragState up = beginDrag(layout, layout.centre, start, kModNone);
    const CameraValues farUp = applyDrag(up, PointF{layout.centre.x, layout.centre.y - 1.0e6}, kModNone);
    CHECK(farUp.tiltDeg == Approx(-OSV_REFRAME_TILT_LIMIT_DEG));

    // Pan is deliberately NOT clamped: a full turn must be keyframeable.
    DragState spin = beginDrag(layout, layout.centre, start, kModNone);
    const CameraValues spun = applyDrag(spin, PointF{layout.centre.x + 20000.0, layout.centre.y}, kModNone);
    CHECK(spun.panDeg > 360.0);
    CHECK(std::isfinite(spun.panDeg));
}

TEST_CASE("a non-finite pointer position leaves the values untouched", "[reframe][ui]") {
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.panDeg = 42.0;
    start.fovDeg = 90.0;

    DragState state = beginDrag(layout, layout.centre, start, kModNone);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const CameraValues after = applyDrag(state, PointF{nan, nan}, kModNone);

    CHECK(after.panDeg == Approx(42.0));
    CHECK(std::isfinite(after.tiltDeg));
    CHECK(std::isfinite(after.fovDeg));
}

TEST_CASE("sanitise replaces NaN with defaults and clamps the bounded values", "[reframe][ui]") {
    // A project file can genuinely hold a NaN; one reaching the drag maths
    // would poison every subsequent value.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CameraValues bad;
    bad.panDeg = nan;
    bad.tiltDeg = 500.0;
    bad.rollDeg = std::numeric_limits<double>::infinity();
    bad.fovDeg = 1.0e9;

    const CameraValues good = sanitise(bad);
    CHECK(good.panDeg == Approx(OSV_REFRAME_PAN_DEFAULT));
    CHECK(good.tiltDeg == Approx(OSV_REFRAME_TILT_LIMIT_DEG));
    CHECK(good.rollDeg == Approx(OSV_REFRAME_ROLL_DEFAULT));
    CHECK(good.fovDeg == Approx(OSV_REFRAME_FOV_VALID_MAX));
}

TEST_CASE("each drag mode writes exactly the parameters it moved", "[reframe][ui]") {
    // Marking a parameter that did not move would make the host record a
    // spurious keyframe on it, which is what makes an auto-keyframing UI
    // infuriating to use.
    CHECK(changedFieldsFor(DragMode::PanTilt) == (kChangedPan | kChangedTilt));
    CHECK(changedFieldsFor(DragMode::PanOnly) == kChangedPan);
    CHECK(changedFieldsFor(DragMode::TiltOnly) == kChangedTilt);
    CHECK(changedFieldsFor(DragMode::Roll) == kChangedRoll);
    CHECK(changedFieldsFor(DragMode::Fov) == kChangedFov);
    CHECK(changedFieldsFor(DragMode::None) == kChangedNone);
}

// ===========================================================================
//  2b. Window -> frame coordinates
//
//  Premiere reports a 0x0 layer during a comp-window custom UI event, so the
//  overlay's size comes from a draw event's update rect instead - and that
//  rectangle is in WINDOW coordinates, so it can start anywhere.  A pointer
//  event's screen_point is in the same window space.  windowToFrame() is the
//  one transform that puts the pointer in the layout's space; if it were
//  wrong (or went back to being the identity) every click in an offset frame
//  would land on the wrong handle and every drag would be anchored in the
//  wrong place.
// ===========================================================================

namespace {

/// The geometry a draw event with this update rect would establish.
[[nodiscard]] FrameGeometry geometryAt(double x, double y, double w, double h) noexcept {
    FrameGeometry g;
    g.originX = x;
    g.originY = y;
    g.width = w;
    g.height = h;
    g.valid = true;
    return g;
}

/// The window-space origin every offset test below uses.  Chosen so that a
/// click near the frame's top-left corner, taken WITHOUT the correction,
/// lands on open picture instead of the grip - which is what makes the
/// "wrong handle" test able to tell the two mappings apart.
constexpr double kOffsetX = 400.0;
constexpr double kOffsetY = 300.0;

}  // namespace

TEST_CASE("windowToFrame is the identity until a frame geometry is established", "[reframe][ui][coords]") {
    // Before any draw has run, the pointer handlers hold a default (invalid)
    // geometry.  It must not move the point: that is the historical
    // behaviour, and a caller that forgot to check validity is then no worse
    // off than before the transform existed.
    const FrameGeometry none;
    REQUIRE_FALSE(none.valid);
    const PointF p{123.25, -45.5};
    const PointF q = windowToFrame(none, p);
    CHECK(q.x == p.x);
    CHECK(q.y == p.y);

    // An invalid geometry is ignored even when it carries an origin: only a
    // geometry a draw actually established may shift a pointer.
    FrameGeometry notEstablished = geometryAt(kOffsetX, kOffsetY, kFrameW, kFrameH);
    notEstablished.valid = false;
    const PointF r = windowToFrame(notEstablished, p);
    CHECK(r.x == p.x);
    CHECK(r.y == p.y);
}

TEST_CASE("windowToFrame subtracts the frame's window-space origin", "[reframe][ui][coords]") {
    const FrameGeometry g = geometryAt(kOffsetX, kOffsetY, kFrameW, kFrameH);

    // The frame's own top-left corner is frame (0, 0).
    const PointF topLeft = windowToFrame(g, PointF{kOffsetX, kOffsetY});
    CHECK(topLeft.x == Approx(0.0));
    CHECK(topLeft.y == Approx(0.0));

    // The frame's centre is the layout's centre.
    const PointF centre = windowToFrame(g, PointF{kOffsetX + 960.0, kOffsetY + 540.0});
    CHECK(centre.x == Approx(computeLayout(fullFrame()).centre.x));
    CHECK(centre.y == Approx(computeLayout(fullFrame()).centre.y));

    // Left of and above the frame is NEGATIVE frame space, which the
    // hit-test rejects as outside - the click is handed back to the host.
    const PointF outside = windowToFrame(g, PointF{10.0, 10.0});
    CHECK(outside.x == Approx(10.0 - kOffsetX));
    CHECK(outside.y == Approx(10.0 - kOffsetY));
    CHECK(hitTest(computeLayout(fullFrame()), outside) == Handle::None);

    // A zero origin is exactly the identity: the After Effects case, and
    // Premiere whenever the update rect covers the whole window.
    const PointF same = windowToFrame(geometryAt(0.0, 0.0, kFrameW, kFrameH), PointF{960.0, 540.0});
    CHECK(same.x == 960.0);
    CHECK(same.y == 540.0);
}

TEST_CASE("windowToFrame keeps a garbage point garbage so it is rejected downstream",
          "[reframe][ui][coords]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const Layout layout = computeLayout(fullFrame());
    const FrameGeometry g = geometryAt(kOffsetX, kOffsetY, kFrameW, kFrameH);

    // A non-finite pointer stays non-finite.  Turning it into a number here
    // would make a garbage event look like a real click at some position.
    const PointF a = windowToFrame(g, PointF{nan, 10.0});
    const PointF b = windowToFrame(g, PointF{10.0, inf});
    CHECK(std::isnan(a.x));
    CHECK(std::isinf(b.y));

    // ...which is what lets the hit-test and the drag reject it.
    CHECK(hitTest(layout, a) == Handle::None);
    CHECK(hitTest(layout, b) == Handle::None);

    // A geometry holding a non-finite origin cannot shift a point at all.
    const PointF p{50.0, 60.0};
    for (const FrameGeometry& broken : {geometryAt(nan, kOffsetY, kFrameW, kFrameH),
                                        geometryAt(kOffsetX, -inf, kFrameW, kFrameH)}) {
        const PointF q = windowToFrame(broken, p);
        CHECK(q.x == p.x);
        CHECK(q.y == p.y);
    }
}

TEST_CASE("an offset frame hit-tests and drags exactly like one at the origin", "[reframe][ui][coords]") {
    // Translation invariance: the same frame-relative gesture, expressed in
    // window coordinates of a frame drawn at (kOffsetX, kOffsetY), must grab
    // the same handle and produce the same values to the last bit that
    // matters.  This is the property that stops a drag being offset.
    const Layout layout = computeLayout(fullFrame());
    const FrameGeometry offset = geometryAt(kOffsetX, kOffsetY, kFrameW, kFrameH);

    // One point on every kind of target, in FRAME coordinates.
    const PointF framePoints[] = {
        layout.centre,                                                   // open picture
        PointF{5.0, 5.0},                                                // top-left grip
        PointF{kFrameW - 5.0, kFrameH - 5.0},                            // bottom-right grip
        PointF{layout.centre.x + layout.rollRingRadius, layout.centre.y},  // roll arc
        PointF{-5.0, 500.0},                                             // outside
    };
    for (const PointF& p : framePoints) {
        INFO("frame point " << p.x << "," << p.y);
        const PointF window{p.x + kOffsetX, p.y + kOffsetY};
        CHECK(hitTest(layout, windowToFrame(offset, window)) == hitTest(layout, p));
    }

    // A pan/tilt drag of (+96, +40) pixels from the centre.
    CameraValues start;
    start.fovDeg = 90.0;

    DragState atOrigin = beginDrag(layout, layout.centre, start, kModNone);
    const CameraValues expected =
        applyDrag(atOrigin, PointF{layout.centre.x + 96.0, layout.centre.y + 40.0}, kModNone);

    const PointF anchorWindow{layout.centre.x + kOffsetX, layout.centre.y + kOffsetY};
    DragState shifted = beginDrag(layout, windowToFrame(offset, anchorWindow), start, kModNone);
    const CameraValues got = applyDrag(
        shifted, windowToFrame(offset, PointF{anchorWindow.x + 96.0, anchorWindow.y + 40.0}), kModNone);

    CHECK(got.panDeg == Approx(expected.panDeg));
    CHECK(got.tiltDeg == Approx(expected.tiltDeg));
    // And both are the calibrated rate, not merely equal to each other.
    CHECK(got.panDeg == Approx(kPanTiltSensitivity * 96.0 * 90.0 / kFrameW));
    CHECK(got.tiltDeg == Approx(kPanTiltSensitivity * 40.0 * 90.0 / kFrameW));
}

TEST_CASE("without the origin correction an offset frame grabs the wrong handle", "[reframe][ui][coords]") {
    // Pins WHY the transform exists.  A click 5px inside the top-left corner
    // of a frame drawn at (kOffsetX, kOffsetY) is on the FOV grip; read with
    // the old identity mapping, the same window coordinates land in open
    // picture and would start a pan instead of a zoom.
    const Layout layout = computeLayout(fullFrame());
    const FrameGeometry offset = geometryAt(kOffsetX, kOffsetY, kFrameW, kFrameH);
    const PointF click{kOffsetX + 5.0, kOffsetY + 5.0};

    CHECK(hitTest(layout, windowToFrame(offset, click)) == Handle::Fov);
    CHECK(hitTest(layout, click) == Handle::PanTilt);
}

// ===========================================================================
//  2c. The live readout
//
//  While a drag is in flight the HUD shows the gesture's own numbers for the
//  fields it has written, because the host may hand the draw pass values
//  that lag behind a slow render.  mergeLiveReadout() decides what is shown;
//  these tests pin its rule, including the consistency check that stops a
//  gesture's numbers appearing on a state they do not describe.
// ===========================================================================

namespace {

[[nodiscard]] CameraValues camera(double pan, double tilt, double roll, double fov) noexcept {
    CameraValues v;
    v.panDeg = pan;
    v.tiltDeg = tilt;
    v.rollDeg = roll;
    v.fovDeg = fov;
    return v;
}

}  // namespace

TEST_CASE("the live readout takes the fields a gesture wrote and the host's for the rest",
          "[reframe][ui][readout]") {
    // A pan/tilt drag the host has not caught up with yet.
    const CameraValues host = camera(0.0, 0.0, 12.0, 90.0);
    const CameraValues live = camera(4.5, -2.0, 12.0, 90.0);

    const CameraValues shown = mergeLiveReadout(host, live, kChangedPan | kChangedTilt);
    CHECK(shown.panDeg == Approx(4.5));
    CHECK(shown.tiltDeg == Approx(-2.0));
    CHECK(shown.rollDeg == Approx(12.0));
    CHECK(shown.fovDeg == Approx(90.0));

    // Untouched fields that agree only to within the host's own round-trip
    // precision (PF_Fixed, a float slider) still count as agreeing.
    const CameraValues roundTripped = camera(0.0, 0.0, 12.0 + 1.0 / 65536.0, 90.0f);
    const CameraValues shown2 = mergeLiveReadout(roundTripped, live, kChangedPan | kChangedTilt);
    CHECK(shown2.panDeg == Approx(4.5));
    CHECK(shown2.tiltDeg == Approx(-2.0));
}

TEST_CASE("the live readout is ignored when a field the gesture never wrote disagrees",
          "[reframe][ui][readout]") {
    // An FOV-grip gesture remembered for an instance whose address was later
    // reused by a DIFFERENT effect with different values: the gesture cannot
    // have moved Pan, Tilt or Roll, so the disagreement proves `live` is not
    // a picture of this host - and NONE of its numbers may be shown.
    const CameraValues host = camera(12.5, -3.5, 45.0, 110.0);
    const CameraValues live = camera(0.0, 0.0, 0.0, 170.0);

    const CameraValues shown = mergeLiveReadout(host, live, kChangedFov);
    CHECK(shown.panDeg == Approx(12.5));
    CHECK(shown.tiltDeg == Approx(-3.5));
    CHECK(shown.rollDeg == Approx(45.0));
    CHECK(shown.fovDeg == Approx(110.0));  // not 170: the host keeps authority wholesale
}

TEST_CASE("the live readout with nothing or everything touched", "[reframe][ui][readout]") {
    const CameraValues host = camera(1.0, 2.0, 3.0, 100.0);

    // Nothing written: the host's numbers, whatever `live` says.
    const CameraValues same = mergeLiveReadout(host, host, kChangedNone);
    CHECK(same.panDeg == Approx(1.0));
    const CameraValues other = mergeLiveReadout(host, camera(9.0, 9.0, 9.0, 99.0), kChangedNone);
    CHECK(other.panDeg == Approx(1.0));
    CHECK(other.fovDeg == Approx(100.0));

    // Everything written (a pan that became a zoom and then a roll): there
    // is nothing left to cross-check, so the gesture's numbers are shown.
    const std::uint32_t all = kChangedPan | kChangedTilt | kChangedRoll | kChangedFov;
    const CameraValues live = camera(9.0, 8.0, 7.0, 99.0);
    const CameraValues shown = mergeLiveReadout(host, live, all);
    CHECK(shown.panDeg == Approx(9.0));
    CHECK(shown.tiltDeg == Approx(8.0));
    CHECK(shown.rollDeg == Approx(7.0));
    CHECK(shown.fovDeg == Approx(99.0));
}

TEST_CASE("the live readout never shows a NaN or an out-of-range value", "[reframe][ui][readout]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::uint32_t all = kChangedPan | kChangedTilt | kChangedRoll | kChangedFov;

    // Garbage on either side, in touched and untouched fields alike.
    const CameraValues cases[][2] = {
        {camera(nan, 0.0, 0.0, 90.0), camera(4.5, 0.0, 0.0, 90.0)},
        {camera(0.0, 0.0, 0.0, 90.0), camera(nan, nan, nan, nan)},
        {camera(0.0, 500.0, 0.0, 1e9), camera(0.0, -500.0, 0.0, -1e9)},
    };
    for (const auto& c : cases) {
        for (const std::uint32_t touched :
             {static_cast<std::uint32_t>(kChangedNone), static_cast<std::uint32_t>(kChangedPan), all}) {
            const CameraValues shown = mergeLiveReadout(c[0], c[1], touched);
            CHECK(std::isfinite(shown.panDeg));
            CHECK(std::isfinite(shown.tiltDeg));
            CHECK(std::isfinite(shown.rollDeg));
            CHECK(std::isfinite(shown.fovDeg));
            CHECK(std::fabs(shown.tiltDeg) <= OSV_REFRAME_TILT_LIMIT_DEG);
            CHECK(shown.fovDeg >= OSV_REFRAME_FOV_VALID_MIN);
            CHECK(shown.fovDeg <= OSV_REFRAME_FOV_VALID_MAX);
        }
    }
}

// ===========================================================================
//  3. The module: flags and registration
// ===========================================================================

TEST_CASE("the effect reports PF_OutFlag_CUSTOM_UI at global setup", "[reframe][ui][module]") {
    UiFixture f;

    // The fixture already ran GLOBAL_SETUP; re-run it on a fresh out_data so
    // the flags are read straight from the module.
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
            PF_Err_NONE);

    CHECK((out.out_flags & PF_OutFlag_CUSTOM_UI) != 0);

    // And the word is byte-identical to the constant the PiPL is built from,
    // which is the invariant that stops the resource and the run-time answer
    // drifting apart (test_pipl.cpp checks the resource side).
    CHECK(static_cast<std::uint32_t>(out.out_flags) == static_cast<std::uint32_t>(OSV_REFRAME_OUT_FLAGS));

    // PF_OutFlag_FORCE_RERENDER is deliberately absent: CHANGED_VALUE
    // already forces a re-render (AE_Effect.h:752) and forcing one as well
    // only adds cache invalidation that fights undo.
    CHECK((out.out_flags & PF_OutFlag_FORCE_RERENDER) == 0);
}

TEST_CASE("PARAMS_SETUP registers a comp-window custom UI", "[reframe][ui][module]") {
    UiFixture f;

    const std::optional<PF_CustomUIInfo> info = f.host.registeredCustomUi(f.ref);
    REQUIRE(info.has_value());

    // PF_CustomEFlag_COMP is the Program Monitor.  The AE docs note that
    // Premiere additionally requires it for a custom UI to receive keyboard
    // events at all, so it is the one flag that must be set.
    CHECK((info->events & PF_CustomEFlag_COMP) != 0);

    // The Effect Controls panel keeps its standard dials and sliders.
    CHECK((info->events & PF_CustomEFlag_EFFECT) == 0);

    // Zero means "the whole comp view" rather than a fixed pixel box; the
    // overlay tracks the picture as the monitor is zoomed.
    CHECK(info->comp_ui_width == 0);
    CHECK(info->comp_ui_height == 0);
}

// ===========================================================================
//  4. The module: a real click / drag / release sequence
// ===========================================================================

TEST_CASE("a DO_CLICK in open picture starts a drag and asks for the stream", "[reframe][ui][module]") {
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DO_CLICK);
    extra.u.do_click.screen_point.h = 960;
    extra.u.do_click.screen_point.v = 540;
    extra.u.do_click.num_clicks = 1;
    extra.u.do_click.modifiers = PF_Mod_NONE;

    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    // send_drag is what makes the host follow up with PF_Event_DRAG; without
    // it a click is a click and nothing moves.
    CHECK(extra.u.do_click.send_drag == TRUE);
    CHECK((extra.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);

    // A click with no movement must not record a keyframe on anything.
    CHECK_FALSE(changed(params, kIndexPan));
    CHECK_FALSE(changed(params, kIndexTilt));
    CHECK_FALSE(changed(params, kIndexRoll));
    CHECK_FALSE(changed(params, kIndexFov));
}

TEST_CASE("a click outside the picture is left to the host", "[reframe][ui][module]") {
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DO_CLICK);
    // Well past the right edge of the 1920x1080 frame.
    extra.u.do_click.screen_point.h = 5000;
    extra.u.do_click.screen_point.v = 540;

    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    CHECK(extra.u.do_click.send_drag == FALSE);
    CHECK((extra.evt_out_flags & PF_EO_HANDLED_EVENT) == 0);
}

TEST_CASE("a click-drag-release sequence commits Pan and Tilt for the host to keyframe",
          "[reframe][ui][module][signs]") {
    UiFixture f;

    // A known starting point and a known FOV, so the expected degrees are
    // arithmetic rather than a guess.
    f.setAngle(kIndexPan, 0.0);
    f.setAngle(kIndexTilt, 0.0);
    f.setFov(90.0);

    std::vector<PF_ParamDef*> params = f.params();
    REQUIRE(fovOf(params) == Approx(90.0));

    // ---- press in the middle of the picture -------------------------------
    PF_EventExtra click = makeExtra(f.host, PF_Event_DO_CLICK);
    click.u.do_click.screen_point.h = 960;
    click.u.do_click.screen_point.v = 540;
    click.u.do_click.num_clicks = 1;
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);

    // ---- drag 480 right and 270 down --------------------------------------
    // The host carries continue_refcon from the click into every drag; the
    // test does the same, because that is the contract.
    PF_EventExtra drag = makeExtra(f.host, PF_Event_DRAG);
    std::memcpy(drag.u.do_click.continue_refcon, click.u.do_click.continue_refcon,
                sizeof(drag.u.do_click.continue_refcon));
    drag.u.do_click.screen_point.h = 960 + 480;
    drag.u.do_click.screen_point.v = 540 + 270;
    drag.u.do_click.last_time = FALSE;

    REQUIRE(f.event(drag, params) == PF_Err_NONE);

    // PF_ChangeFlag_CHANGED_VALUE is the documented way to commit a value
    // from an event (AE_Effect.h:2376-2384), and the host turns that into a
    // keyframe at the current time when the stopwatch is running.  The SDK
    // also requires PF_EO_HANDLED_EVENT alongside it.
    CHECK(changed(params, kIndexPan));
    CHECK(changed(params, kIndexTilt));
    CHECK((drag.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);

    // The signs: right -> Pan up, down -> Tilt up (the world follows the
    // hand).  And the magnitudes are the axis-drag contract, through the
    // renderer's own camera: Pan is the azimuth the picture spans along the
    // horizontal centre line for the (sensitivity-scaled) horizontal travel,
    // Tilt the elevation along the vertical one for the vertical travel.
    CHECK(angleOf(params, kIndexPan) > 0.0);
    CHECK(angleOf(params, kIndexTilt) > 0.0);
    {
        const double distortion = static_cast<double>(params[kIndexDistortion]->u.fs_d.value);
        // The parameters are stored as 16.16 fixed point, so allow that
        // quantisation (1/65536 deg) plus float rounding in the ray.
        CHECK(angleOf(params, kIndexPan) ==
              Approx(grabPanDeg(kPanTiltSensitivity * 480.0, 90.0, distortion)).margin(0.01));
        CHECK(angleOf(params, kIndexTilt) ==
              Approx(grabTiltDeg(kPanTiltSensitivity * 270.0, 90.0, distortion)).margin(0.01));
    }

    // Roll and FOV were not part of this drag and must carry no change flag:
    // a spurious Roll keyframe from a pan is exactly the bug this guards.
    CHECK_FALSE(changed(params, kIndexRoll));
    CHECK_FALSE(changed(params, kIndexFov));

    // ---- release ----------------------------------------------------------
    const double panAtDrag = angleOf(params, kIndexPan);
    PF_EventExtra release = makeExtra(f.host, PF_Event_DRAG);
    std::memcpy(release.u.do_click.continue_refcon, click.u.do_click.continue_refcon,
                sizeof(release.u.do_click.continue_refcon));
    release.u.do_click.screen_point.h = 960 + 480;
    release.u.do_click.screen_point.v = 540 + 270;
    release.u.do_click.last_time = TRUE;

    REQUIRE(f.event(release, params) == PF_Err_NONE);

    // The value is unchanged by the release itself, and the gesture is over.
    CHECK(angleOf(params, kIndexPan) == Approx(panAtDrag).margin(1e-4));

    // A further drag with the SAME refcon must now do nothing: the slot was
    // freed, so a duplicated event cannot resume a finished gesture.
    const double panBefore = angleOf(params, kIndexPan);
    PF_EventExtra stale = makeExtra(f.host, PF_Event_DRAG);
    std::memcpy(stale.u.do_click.continue_refcon, release.u.do_click.continue_refcon,
                sizeof(stale.u.do_click.continue_refcon));
    stale.u.do_click.screen_point.h = 1900;
    stale.u.do_click.screen_point.v = 1000;
    REQUIRE(f.event(stale, params) == PF_Err_NONE);
    CHECK(angleOf(params, kIndexPan) == Approx(panBefore));
}

TEST_CASE("a drag with a refcon the module never issued is ignored", "[reframe][ui][module]") {
    // Uninitialised or foreign refcon words must not be interpreted as a
    // slot index; acting on one would move the camera from an unknown anchor.
    UiFixture f;
    f.setAngle(kIndexPan, 15.0);
    std::vector<PF_ParamDef*> params = f.params();

    PF_EventExtra drag = makeExtra(f.host, PF_Event_DRAG);
    drag.u.do_click.continue_refcon[0] = 0x1234;
    drag.u.do_click.continue_refcon[1] = 3;
    drag.u.do_click.continue_refcon[2] = 99;
    drag.u.do_click.screen_point.h = 1500;
    drag.u.do_click.screen_point.v = 900;

    REQUIRE(f.event(drag, params) == PF_Err_NONE);

    CHECK(angleOf(params, kIndexPan) == Approx(15.0));
    CHECK_FALSE(changed(params, kIndexPan));
}

TEST_CASE("a roll-ring drag through the module changes Roll alone", "[reframe][ui][module]") {
    UiFixture f;
    f.setAngle(kIndexPan, 20.0);
    f.setAngle(kIndexTilt, -10.0);
    f.setAngle(kIndexRoll, 0.0);
    f.setFov(90.0);
    std::vector<PF_ParamDef*> params = f.params();

    // The layout the module computes for a full-frame 1920x1080 viewport;
    // the test derives the grab point from the same function the module
    // uses, so it cannot be grabbing somewhere the module does not.
    const Layout layout = computeLayout(fullFrame());
    const PointF grab{layout.centre.x + layout.rollRingRadius, layout.centre.y};
    REQUIRE(hitTest(layout, grab) == Handle::Roll);

    PF_EventExtra click = makeExtra(f.host, PF_Event_DO_CLICK);
    click.u.do_click.screen_point.h = static_cast<A_short>(std::lround(grab.x));
    click.u.do_click.screen_point.v = static_cast<A_short>(std::lround(grab.y));
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);

    // 20 degrees clockwise around the ring.
    const double angle = 20.0 * (3.14159265358979323846 / 180.0);
    PF_EventExtra drag = makeExtra(f.host, PF_Event_DRAG);
    std::memcpy(drag.u.do_click.continue_refcon, click.u.do_click.continue_refcon,
                sizeof(drag.u.do_click.continue_refcon));
    drag.u.do_click.screen_point.h =
        static_cast<A_short>(std::lround(layout.centre.x + layout.rollRingRadius * std::cos(angle)));
    drag.u.do_click.screen_point.v =
        static_cast<A_short>(std::lround(layout.centre.y + layout.rollRingRadius * std::sin(angle)));
    REQUIRE(f.event(drag, params) == PF_Err_NONE);

    CHECK(changed(params, kIndexRoll));
    CHECK_FALSE(changed(params, kIndexPan));
    CHECK_FALSE(changed(params, kIndexTilt));
    CHECK_FALSE(changed(params, kIndexFov));

    // Clockwise ring drag decreases Roll (see the maths test above); the
    // pixel rounding of the screen point costs a fraction of a degree.
    CHECK(angleOf(params, kIndexRoll) == Approx(-20.0).margin(0.5));
    // The untouched values really are untouched.
    CHECK(angleOf(params, kIndexPan) == Approx(20.0));
    CHECK(angleOf(params, kIndexTilt) == Approx(-10.0));
}

TEST_CASE("an FOV-grip drag through the module changes FOV alone and stays in range",
          "[reframe][ui][module]") {
    UiFixture f;
    f.setFov(120.0);
    f.setAngle(kIndexPan, 0.0);
    std::vector<PF_ParamDef*> params = f.params();

    const Layout layout = computeLayout(fullFrame());
    RectF grips[4];
    REQUIRE(fovGripRects(layout, grips) == 4);
    const PointF grab = grips[0].centre();  // top-left

    PF_EventExtra click = makeExtra(f.host, PF_Event_DO_CLICK);
    click.u.do_click.screen_point.h = static_cast<A_short>(std::lround(grab.x));
    click.u.do_click.screen_point.v = static_cast<A_short>(std::lround(grab.y));
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);

    PF_EventExtra drag = makeExtra(f.host, PF_Event_DRAG);
    std::memcpy(drag.u.do_click.continue_refcon, click.u.do_click.continue_refcon,
                sizeof(drag.u.do_click.continue_refcon));
    drag.u.do_click.screen_point.h = click.u.do_click.screen_point.h;
    drag.u.do_click.screen_point.v = static_cast<A_short>(click.u.do_click.screen_point.v + 200);
    REQUIRE(f.event(drag, params) == PF_Err_NONE);

    CHECK(changed(params, kIndexFov));
    CHECK_FALSE(changed(params, kIndexPan));
    CHECK_FALSE(changed(params, kIndexTilt));
    CHECK_FALSE(changed(params, kIndexRoll));

    CHECK(fovOf(params) == Approx(120.0 + 200.0 * kFovDegPerPixel).margin(0.5));
    CHECK(fovOf(params) >= OSV_REFRAME_FOV_VALID_MIN);
    CHECK(fovOf(params) <= OSV_REFRAME_FOV_VALID_MAX);
}

// ===========================================================================
//  5. ADJUST_CURSOR
// ===========================================================================

TEST_CASE("the cursor tells the user what the handle under it will do", "[reframe][ui][module]") {
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();
    const Layout layout = computeLayout(fullFrame());
    RectF grips[4];
    REQUIRE(fovGripRects(layout, grips) == 4);

    struct Case {
        const char* what;
        PointF where;
        PF_Modifiers modifiers;
        PF_CursorType expected;
        bool handled;
    };
    const PointF onRing{layout.centre.x + layout.rollRingRadius, layout.centre.y};
    const Case cases[] = {
        {"open picture", layout.centre, PF_Mod_NONE, PF_Cursor_PAN, true},
        {"open picture with ctrl", layout.centre, PF_Mod_CMD_CTRL_KEY, PF_Cursor_SCALE_DIAG_LR, true},
        {"open picture with alt", layout.centre, PF_Mod_OPT_ALT_KEY, PF_Cursor_CROSS_ROTATE, true},
        {"open picture with shift", layout.centre, PF_Mod_SHIFT_KEY, PF_Cursor_PAN, true},
        {"the roll ring", onRing, PF_Mod_NONE, PF_Cursor_CROSS_ROTATE, true},
        {"a corner grip", grips[3].centre(), PF_Mod_NONE, PF_Cursor_SCALE_DIAG_LR, true},
        // Outside the picture the cursor is left alone: PF_Cursor_NONE is
        // the documented "do not override" answer.
        {"outside the picture", PointF{5000.0, 540.0}, PF_Mod_NONE, PF_Cursor_NONE, false},
    };

    for (const Case& c : cases) {
        INFO(c.what);
        PF_EventExtra extra = makeExtra(f.host, PF_Event_ADJUST_CURSOR);
        extra.u.adjust_cursor.screen_point.h = static_cast<A_short>(std::lround(c.where.x));
        extra.u.adjust_cursor.screen_point.v = static_cast<A_short>(std::lround(c.where.y));
        extra.u.adjust_cursor.modifiers = c.modifiers;
        extra.u.adjust_cursor.set_cursor = PF_Cursor_ARROW;

        REQUIRE(f.event(extra, params) == PF_Err_NONE);
        CHECK(extra.u.adjust_cursor.set_cursor == c.expected);
        CHECK(((extra.evt_out_flags & PF_EO_HANDLED_EVENT) != 0) == c.handled);
    }
}

// ===========================================================================
//  6. PF_Event_DRAW
// ===========================================================================

TEST_CASE("DRAW puts a crosshair, a roll ring and four corner grips on the picture",
          "[reframe][ui][module][draw]") {
    UiFixture f;
    f.setAngle(kIndexPan, 12.5);
    f.setAngle(kIndexTilt, -3.5);
    f.setAngle(kIndexRoll, 45.0);
    f.setFov(110.0);
    std::vector<PF_ParamDef*> params = f.params();

    f.host.clearDrawbotRecord();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    extra.u.draw.depth = 32;
    REQUIRE(f.event(extra, params) == PF_Err_NONE);
    CHECK((extra.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);

    const DrawbotRecord record = f.host.drawbotRecord();

    // Something was actually drawn.
    CHECK_FALSE(record.ops.empty());

    // The roll ring: two arcs, one straddling 0 degrees (3 o'clock) and one
    // straddling 180, each the full drawn sweep.  These come from the same
    // constants the hit-test uses, so the drawn shape and the grab target
    // cannot drift apart.
    const std::size_t arcs = record.countVertices(DrawbotVertexKind::Arc);
    CHECK(arcs == 2u);

    const std::vector<osv::premiere::mock::DrawbotVertex> vertices = record.allVertices();
    std::vector<float> arcStarts;
    for (const osv::premiere::mock::DrawbotVertex& v : vertices) {
        if (v.kind == DrawbotVertexKind::Arc) {
            arcStarts.push_back(v.b);
            CHECK(v.c == Approx(2.0 * kRollArcHalfSweepDeg));
            // Centred on the frame centre, at the layout's ring radius.
            CHECK(v.x == Approx(960.0).margin(0.5));
            CHECK(v.y == Approx(540.0).margin(0.5));
            CHECK(v.a == Approx(computeLayout(fullFrame()).rollRingRadius).margin(0.5));
        }
    }
    REQUIRE(arcStarts.size() == 2u);
    CHECK(arcStarts[0] == Approx(-kRollArcHalfSweepDeg));
    CHECK(arcStarts[1] == Approx(180.0 - kRollArcHalfSweepDeg));

    // The crosshair is four separate segments (a gap in the middle, so the
    // exact centre pixel of the frame is never covered), and the four corner
    // grips are two segments each: twelve MoveTo/LineTo pairs in all.
    CHECK(record.countVertices(DrawbotVertexKind::MoveTo) == 12u);
    CHECK(record.countVertices(DrawbotVertexKind::LineTo) == 12u);

    // NOTHING is filled.  A filled overlay over the picture is exactly what
    // a non-intrusive HUD is not, so this must stay at zero.
    CHECK(record.paintRects.empty());
    for (const osv::premiere::mock::DrawbotDrawOp& op : record.ops) {
        CHECK(op.kind == DrawbotOpKind::StrokePath);
    }

    // The readout: one string per draw, twice over (shadow then ink), naming
    // every value in degrees.
    REQUIRE_FALSE(record.strings.empty());
    const std::string& text = record.strings.front().text;
    INFO("readout: " << text);
    CHECK(text.find("Pan") != std::string::npos);
    CHECK(text.find("Tilt") != std::string::npos);
    CHECK(text.find("Roll") != std::string::npos);
    CHECK(text.find("FOV") != std::string::npos);
    CHECK(text.find("12.5") != std::string::npos);
    CHECK(text.find("45.0") != std::string::npos);
    CHECK(text.find("110.0") != std::string::npos);
}

TEST_CASE("every light stroke is backed by a dark one so the HUD stays legible",
          "[reframe][ui][module][draw]") {
    // The legibility strategy: a 1px light line with a 1px dark line one
    // pixel below and right of it, so the overlay reads over a blown-out sky
    // and over a night shot alike.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();
    f.host.clearDrawbotRecord();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    const DrawbotRecord record = f.host.drawbotRecord();
    REQUIRE_FALSE(record.ops.empty());

    std::size_t shadowStrokes = 0;
    std::size_t inkStrokes = 0;
    for (const osv::premiere::mock::DrawbotDrawOp& op : record.ops) {
        // The shadow pass is the one drawn under a 1px translation.
        if (op.dx != 0.0f || op.dy != 0.0f) {
            ++shadowStrokes;
            CHECK(op.dx == Approx(1.0f));
            CHECK(op.dy == Approx(1.0f));
            // And it is dark.
            CHECK(op.colour.red < 0.2f);
            CHECK(op.colour.green < 0.2f);
            CHECK(op.colour.blue < 0.2f);
        } else {
            ++inkStrokes;
        }
    }
    CHECK(shadowStrokes > 0u);
    CHECK(shadowStrokes == inkStrokes);

    // The surface state stack is balanced: every push popped, and never a
    // pop on an empty stack.
    CHECK(record.pushCount == record.popCount);
    CHECK(record.unbalancedPops == 0u);

    // Nothing is opaque: the HUD must never compete with the picture.
    for (const osv::premiere::mock::DrawbotDrawOp& op : record.ops) {
        CHECK(op.colour.alpha < 1.0f);
        CHECK(op.colour.alpha > 0.0f);
    }
}

TEST_CASE("DRAW releases every DrawBot object it created", "[reframe][ui][module][draw]") {
    // At monitor refresh rate a leaked pen per repaint is a leak that
    // matters within seconds.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();
    f.host.clearDrawbotRecord();

    for (int i = 0; i < 5; ++i) {
        PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
        REQUIRE(f.event(extra, params) == PF_Err_NONE);
    }

    const DrawbotRecord record = f.host.drawbotRecord();
    CHECK(record.liveObjects == 0u);
    // And no stroke ever used an object that had already been released.
    for (const osv::premiere::mock::DrawbotDrawOp& op : record.ops) {
        CHECK_FALSE(op.staleObject);
    }
}

TEST_CASE("DRAW with no DrawBot drawing reference returns cleanly and draws nothing",
          "[reframe][ui][module][draw]") {
    // The "missing suite / null context" degradation the design requires:
    // no overlay, no crash, no error to the host.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    f.host.setDrawbotProvidesDrawRef(false);
    f.host.clearDrawbotRecord();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    const DrawbotRecord record = f.host.drawbotRecord();
    CHECK(record.ops.empty());
    CHECK(record.paths.empty());
    CHECK(record.strings.empty());
    // The event was not handled, so the host draws whatever it normally would.
    CHECK((extra.evt_out_flags & PF_EO_HANDLED_EVENT) == 0);

    f.host.setDrawbotProvidesDrawRef(true);
}

TEST_CASE("DRAW with the DrawBot suites hidden returns cleanly", "[reframe][ui][module][draw]") {
    // An older host that serves no DrawBot at all.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    f.host.setSuiteAvailable(kDRAWBOT_DrawSuite, kDRAWBOT_DrawSuite_VersionCurrent, false);
    f.host.setSuiteAvailable(kDRAWBOT_SurfaceSuite, kDRAWBOT_SurfaceSuite_Version2, false);
    f.host.clearDrawbotRecord();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    CHECK(f.host.drawbotRecord().ops.empty());
    CHECK((extra.evt_out_flags & PF_EO_HANDLED_EVENT) == 0);

    f.host.setSuiteAvailable(kDRAWBOT_DrawSuite, kDRAWBOT_DrawSuite_VersionCurrent, true);
    f.host.setSuiteAvailable(kDRAWBOT_SurfaceSuite, kDRAWBOT_SurfaceSuite_Version2, true);
}

TEST_CASE("DRAW without the custom UI suite returns cleanly", "[reframe][ui][module][draw]") {
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    f.host.setSuiteAvailable(kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion2, false);
    f.host.clearDrawbotRecord();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    CHECK(f.host.drawbotRecord().ops.empty());

    f.host.setSuiteAvailable(kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion2, true);
}

TEST_CASE("DRAW draws the graphics even when the supplier has no text", "[reframe][ui][module][draw]") {
    // A supplier that reports no text support is legal (SupportsText exists
    // precisely for that), and losing the readout must not lose the HUD.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    f.host.setDrawbotSupportsText(false);
    f.host.clearDrawbotRecord();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    const DrawbotRecord record = f.host.drawbotRecord();
    CHECK(record.strings.empty());          // no readout
    CHECK_FALSE(record.ops.empty());        // but the graphics are there
    CHECK(record.countVertices(DrawbotVertexKind::Arc) == 2u);
    CHECK(record.liveObjects == 0u);

    f.host.setDrawbotSupportsText(true);
}

TEST_CASE("DRAW survives a supplier that cannot create a pen or a path", "[reframe][ui][module][draw]") {
    // Proves the drawing code checks every DrawBot result instead of
    // dereferencing a null.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    SECTION("no pen") {
        f.host.setDrawbotFailures(true, false);
        f.host.clearDrawbotRecord();
        PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
        REQUIRE(f.event(extra, params) == PF_Err_NONE);
        // Without a pen there is nothing worth stroking.
        CHECK(f.host.drawbotRecord().ops.empty());
    }

    SECTION("no path") {
        f.host.setDrawbotFailures(false, true);
        f.host.clearDrawbotRecord();
        PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
        REQUIRE(f.event(extra, params) == PF_Err_NONE);
        CHECK(f.host.drawbotRecord().ops.empty());
        CHECK(f.host.drawbotRecord().liveObjects == 0u);
    }

    f.host.setDrawbotFailures(false, false);
}

TEST_CASE("DRAW honours the host's hide-overlays flag", "[reframe][ui][module][draw]") {
    // PF_EI_DONT_DRAW is the host saying controls are hidden right now.
    // Drawing anyway would put the HUD on a frame the user asked to see
    // clean.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();
    f.host.clearDrawbotRecord();

    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    extra.evt_in_flags = PF_EI_DONT_DRAW;
    REQUIRE(f.event(extra, params) == PF_Err_NONE);

    CHECK(f.host.drawbotRecord().ops.empty());
    CHECK((extra.evt_out_flags & PF_EO_HANDLED_EVENT) == 0);
}

// ===========================================================================
//  7. Robustness of the event entry point
// ===========================================================================

TEST_CASE("the event entry point tolerates null arguments and unknown events", "[reframe][ui][module]") {
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    // A null extra: the host should never do this, but returning an error
    // would make it report a broken effect.
    CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_EVENT, &f.in, &f.out, params.data(), nullptr, nullptr) ==
          PF_Err_NONE);

    // Every event type in the SDK, including the ones the overlay ignores.
    for (int type = PF_Event_NEW_CONTEXT; type < PF_Event_NUM_EVENTS; ++type) {
        INFO("event type " << type);
        PF_EventExtra extra = makeExtra(f.host, static_cast<PF_EventType>(type));
        CHECK(f.event(extra, params) == PF_Err_NONE);
    }

    // A context-less event.
    PF_EventExtra noContext = makeExtra(f.host, PF_Event_DRAW);
    noContext.contextH = nullptr;
    CHECK(f.event(noContext, params) == PF_Err_NONE);

    // A null parameter array on an event that would otherwise read values.
    PF_EventExtra noParams = makeExtra(f.host, PF_Event_DO_CLICK);
    noParams.u.do_click.screen_point.h = 960;
    noParams.u.do_click.screen_point.v = 540;
    CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_EVENT, &f.in, &f.out, nullptr, nullptr, &noParams) ==
          PF_Err_NONE);
}

TEST_CASE("a degenerate frame produces no overlay and no crash", "[reframe][ui][module]") {
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();

    // A 0x0 frame is what a collapsed panel reports.
    f.in.width = 0;
    f.in.height = 0;
    f.host.clearDrawbotRecord();

    PF_EventExtra draw = makeExtra(f.host, PF_Event_DRAW);
    CHECK(f.event(draw, params) == PF_Err_NONE);
    CHECK(f.host.drawbotRecord().ops.empty());

    // A click with a 0x0 layer.  This used to click at (0, 0) and expect no
    // drag, on the assumption that a 0x0 layer means nothing is grabbable.
    // That assumption is exactly what made the overlay inert in Premiere,
    // which reports 0x0 on EVERY event: a click now uses the frame the last
    // sized repaint established (section 8 pins that), and in a process
    // where an earlier test drew one, (0, 0) is the top-left FOV grip.  So
    // the click is placed left of and above ANY frame the process could have
    // cached - every cached origin is >= 0 - which keeps the contract this
    // test is about (no crash, no gesture from nowhere) independent of test
    // order.
    PF_EventExtra click = makeExtra(f.host, PF_Event_DO_CLICK);
    click.u.do_click.screen_point.h = -5;
    click.u.do_click.screen_point.v = -5;
    CHECK(f.event(click, params) == PF_Err_NONE);
    CHECK(click.u.do_click.send_drag == FALSE);
}

TEST_CASE("many gestures in a row do not exhaust the drag table", "[reframe][ui][module]") {
    // The drag table is a fixed array; a gesture that is never released
    // would leak a slot and eventually stop the overlay working. Every
    // gesture here ends properly, so the table must never fill.
    UiFixture f;
    f.setFov(90.0);
    std::vector<PF_ParamDef*> params = f.params();

    for (int i = 0; i < 64; ++i) {
        INFO("gesture " << i);
        f.setAngle(kIndexPan, 0.0);
        params = f.params();

        PF_EventExtra click = makeExtra(f.host, PF_Event_DO_CLICK);
        click.u.do_click.screen_point.h = 960;
        click.u.do_click.screen_point.v = 540;
        REQUIRE(f.event(click, params) == PF_Err_NONE);
        REQUIRE(click.u.do_click.send_drag == TRUE);

        PF_EventExtra drag = makeExtra(f.host, PF_Event_DRAG);
        std::memcpy(drag.u.do_click.continue_refcon, click.u.do_click.continue_refcon,
                    sizeof(drag.u.do_click.continue_refcon));
        drag.u.do_click.screen_point.h = 960 + 96;
        drag.u.do_click.screen_point.v = 540;
        drag.u.do_click.last_time = TRUE;
        REQUIRE(f.event(drag, params) == PF_Err_NONE);

        // The same answer every time: the table is not degrading.
        CHECK(angleOf(params, kIndexPan) == Approx(grabPanDeg(kPanTiltSensitivity * 96.0)).margin(0.01));
    }
}

// ===========================================================================
//  8. Premiere's comp-window events: no layer size, an update rect instead
//
//  Premiere reports in_data->width/height as 0x0 during a comp-window custom
//  UI event.  The UiFixture above is sized the way After Effects sizes it, so
//  none of the tests before this section ever reproduced Premiere - which is
//  how an overlay that DREW but could never be GRABBED shipped: the draw
//  path had learned to fall back to the update rect, the click path had not,
//  so every click saw an invalid layout and never asked for a drag.
//
//  Every test here zeroes the layer size first, so the geometry can only come
//  from what a draw event's update rect established.
// ===========================================================================

namespace {

/// A DRAW event carrying the given update rect, as Premiere sends one.
[[nodiscard]] PF_EventExtra makeDrawWithRect(MockHost& host, A_long left, A_long top, A_long right, A_long bottom) {
    PF_EventExtra extra = makeExtra(host, PF_Event_DRAW);
    extra.u.draw.update_rect.left = left;
    extra.u.draw.update_rect.top = top;
    extra.u.draw.update_rect.right = right;
    extra.u.draw.update_rect.bottom = bottom;
    return extra;
}

/// A DO_CLICK at a window-space point.
[[nodiscard]] PF_EventExtra makeClickAt(MockHost& host, A_long h, A_long v) {
    PF_EventExtra extra = makeExtra(host, PF_Event_DO_CLICK);
    extra.u.do_click.screen_point.h = h;
    extra.u.do_click.screen_point.v = v;
    return extra;
}

/// A DRAG continuing the gesture `click` started, to a window-space point.
[[nodiscard]] PF_EventExtra makeDragFrom(MockHost& host, const PF_EventExtra& click, A_long h, A_long v, bool last) {
    PF_EventExtra extra = makeExtra(host, PF_Event_DRAG);
    std::memcpy(extra.u.do_click.continue_refcon, click.u.do_click.continue_refcon,
                sizeof(extra.u.do_click.continue_refcon));
    extra.u.do_click.screen_point.h = h;
    extra.u.do_click.screen_point.v = v;
    extra.u.do_click.last_time = last ? TRUE : FALSE;
    return extra;
}

/// An ADJUST_CURSOR at a window-space point.  The event is returned so the
/// test can read both the cursor and the out-flags.
[[nodiscard]] PF_EventExtra makeCursorAt(MockHost& host, A_long h, A_long v) {
    PF_EventExtra extra = makeExtra(host, PF_Event_ADJUST_CURSOR);
    extra.u.adjust_cursor.screen_point.h = h;
    extra.u.adjust_cursor.screen_point.v = v;
    return extra;
}

/// Whether any string the last draw put on screen contains `needle`.
[[nodiscard]] bool drewText(const DrawbotRecord& record, const std::string& needle) {
    for (const osv::premiere::mock::DrawbotStringOp& op : record.strings) {
        if (op.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// The start of the readout for a given Pan, exactly as the HUD formats it
/// ("Pan %.1f" and then the two-space separator), so "Pan 4.5" cannot be
/// mistaken for "Pan 4.55".
[[nodiscard]] std::string panReadout(double panDeg) {
    char buffer[32] = {};
    (void)std::snprintf(buffer, sizeof(buffer), "Pan %.1f ", panDeg);
    return std::string(buffer);
}

}  // namespace

TEST_CASE("with a 0x0 layer a DRAW's update rect makes the overlay grabbable", "[reframe][ui][module][premiere]") {
    // The regression this whole section exists for.
    UiFixture f;
    f.setFov(90.0);
    std::vector<PF_ParamDef*> params = f.params();
    f.in.width = 0;
    f.in.height = 0;

    // The repaint establishes the frame, and the HUD is drawn against it.
    f.host.clearDrawbotRecord();
    PF_EventExtra draw = makeDrawWithRect(f.host, 0, 0, 1920, 1080);
    REQUIRE(f.event(draw, params) == PF_Err_NONE);
    CHECK((draw.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);
    CHECK_FALSE(f.host.drawbotRecord().ops.empty());

    // A click on open picture now starts a gesture and asks for the drag
    // stream.  Before the fix send_drag stayed FALSE here, and the host
    // never sent a single PF_Event_DRAG.
    PF_EventExtra click = makeClickAt(f.host, 960, 540);
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);
    CHECK((click.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);

    // And the drag commits Pan at the calibrated rate.
    PF_EventExtra drag = makeDragFrom(f.host, click, 960 + 96, 540, true);
    REQUIRE(f.event(drag, params) == PF_Err_NONE);
    CHECK(angleOf(params, kIndexPan) == Approx(grabPanDeg(kPanTiltSensitivity * 96.0)).margin(0.01));
    CHECK(angleOf(params, kIndexTilt) == Approx(0.0).margin(1e-4));
    CHECK(changed(params, kIndexPan));
    CHECK((drag.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);
}

TEST_CASE("an update rect that does not start at the window origin does not offset the gesture",
          "[reframe][ui][module][premiere]") {
    UiFixture f;
    f.setFov(90.0);
    std::vector<PF_ParamDef*> params = f.params();
    f.in.width = 0;
    f.in.height = 0;

    // The frame drawn at (400, 300) in window space.
    constexpr A_long kX = 400;
    constexpr A_long kY = 300;
    PF_EventExtra draw = makeDrawWithRect(f.host, kX, kY, kX + 1920, kY + 1080);
    REQUIRE(f.event(draw, params) == PF_Err_NONE);
    REQUIRE((draw.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);

    // The cursor feedback agrees with the hit-test in FRAME space: just
    // inside the frame's top-left corner is the FOV grip, just outside it is
    // not ours, and the frame's centre is open picture.
    PF_EventExtra onGrip = makeCursorAt(f.host, kX + 5, kY + 5);
    REQUIRE(f.event(onGrip, params) == PF_Err_NONE);
    CHECK(onGrip.u.adjust_cursor.set_cursor == PF_Cursor_SCALE_DIAG_LR);

    PF_EventExtra beside = makeCursorAt(f.host, kX - 5, kY - 5);
    REQUIRE(f.event(beside, params) == PF_Err_NONE);
    CHECK(beside.u.adjust_cursor.set_cursor == PF_Cursor_NONE);

    PF_EventExtra overPicture = makeCursorAt(f.host, kX + 960, kY + 540);
    REQUIRE(f.event(overPicture, params) == PF_Err_NONE);
    CHECK(overPicture.u.adjust_cursor.set_cursor == PF_Cursor_PAN);

    // A purely horizontal drag from the frame's centre moves Pan by the
    // calibrated amount and Tilt by EXACTLY nothing: an origin that leaked
    // into one axis but not the other would show up here as a Tilt.
    PF_EventExtra click = makeClickAt(f.host, kX + 960, kY + 540);
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);

    PF_EventExtra drag = makeDragFrom(f.host, click, kX + 960 + 96, kY + 540, true);
    REQUIRE(f.event(drag, params) == PF_Err_NONE);
    CHECK(angleOf(params, kIndexPan) == Approx(grabPanDeg(kPanTiltSensitivity * 96.0)).margin(0.01));
    CHECK(angleOf(params, kIndexTilt) == Approx(0.0).margin(1e-4));
}

TEST_CASE("a sizeless DRAW draws nothing and does not erase an established frame",
          "[reframe][ui][module][premiere]") {
    // What a host probe, a hidden panel or a pre-roll sends: no layer size
    // and no usable update rect.  It must draw nothing - and, just as
    // important, it must not wipe the frame a real monitor established, or
    // one probe would make the overlay ungrabbable until the next repaint.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();
    f.in.width = 0;
    f.in.height = 0;

    PF_EventExtra real = makeDrawWithRect(f.host, 0, 0, 1920, 1080);
    REQUIRE(f.event(real, params) == PF_Err_NONE);
    REQUIRE((real.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);

    // Empty, inverted and absurd rects: each one is a no-op for the overlay.
    struct Rect {
        A_long l, t, r, b;
    };
    const Rect sizeless[] = {
        {0, 0, 0, 0},              // what the probe sends
        {500, 500, 100, 100},      // inverted
        {0, 0, 1 << 20, 1 << 20},  // far past kMaxOverlayEdge
    };
    for (const Rect& r : sizeless) {
        INFO("update rect " << r.l << "," << r.t << "," << r.r << "," << r.b);
        f.host.clearDrawbotRecord();
        PF_EventExtra probe = makeDrawWithRect(f.host, r.l, r.t, r.r, r.b);
        CHECK(f.event(probe, params) == PF_Err_NONE);
        CHECK(f.host.drawbotRecord().ops.empty());
        CHECK((probe.evt_out_flags & PF_EO_HANDLED_EVENT) == 0);
    }

    // The frame the real draw established survived every probe.
    PF_EventExtra click = makeClickAt(f.host, 960, 540);
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    CHECK(click.u.do_click.send_drag == TRUE);

    // Finish the gesture so the drag table is left as it was found.
    PF_EventExtra release = makeDragFrom(f.host, click, 960, 540, true);
    CHECK(f.event(release, params) == PF_Err_NONE);
}

TEST_CASE("a drag that moves a value asks for an immediate overlay repaint", "[reframe][ui][module][repaint]") {
    // PF_InvalidateRect + PF_EO_UPDATE_NOW is the SDK's "repaint the custom
    // UI now" (AE_EffectSuites.h:588-594).  Without it the readout only
    // refreshes when something else invalidates the view - in practice when
    // the slow frame render lands - so the HUD would look frozen mid-drag.
    //
    // The mock host serves no App suite, so this pins the out-flag half of
    // the contract.  The PF_InvalidateRect half is acquired defensively and
    // simply skipped when the suite is absent, which this also proves.
    UiFixture f;
    f.setFov(90.0);
    std::vector<PF_ParamDef*> params = f.params();

    PF_EventExtra click = makeClickAt(f.host, 960, 540);
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);

    PF_EventExtra move = makeDragFrom(f.host, click, 960 + 48, 540, false);
    REQUIRE(f.event(move, params) == PF_Err_NONE);
    CHECK((move.evt_out_flags & PF_EO_HANDLED_EVENT) != 0);
    CHECK((move.evt_out_flags & PF_EO_UPDATE_NOW) != 0);

    // ALWAYS_UPDATE means "re-render the comp", which is the slow thing the
    // repaint exists to avoid waiting for.  CHANGED_VALUE already schedules
    // the one render that is needed, so the overlay must never ask for more.
    CHECK((move.evt_out_flags & PF_EO_ALWAYS_UPDATE) == 0);

    PF_EventExtra release = makeDragFrom(f.host, click, 960 + 96, 540, true);
    REQUIRE(f.event(release, params) == PF_Err_NONE);
    CHECK((release.evt_out_flags & PF_EO_UPDATE_NOW) != 0);

    // A drag the module does not recognise changes nothing, so it asks for
    // nothing either.
    PF_EventExtra foreign = makeExtra(f.host, PF_Event_DRAG);
    foreign.u.do_click.screen_point.h = 1000;
    foreign.u.do_click.screen_point.v = 540;
    REQUIRE(f.event(foreign, params) == PF_Err_NONE);
    CHECK(foreign.evt_out_flags == PF_EO_NONE);
}

TEST_CASE("the hover highlight asks for a repaint only when the handle under the cursor changes",
          "[reframe][ui][module][repaint]") {
    // ADJUST_CURSOR arrives on every mouse move.  Repainting the whole view
    // on each one would keep the Program Monitor busy just because the
    // pointer is travelling across it, so the repaint is requested only on a
    // transition between handles.
    UiFixture f;
    std::vector<PF_ParamDef*> params = f.params();
    const Layout layout = computeLayout(fullFrame());
    const A_long cx = static_cast<A_long>(layout.centre.x);
    const A_long cy = static_cast<A_long>(layout.centre.y);

    // Settle on open picture first.  Whether THIS asks for a repaint depends
    // on what an earlier test left hovered, so it is not asserted.
    PF_EventExtra settle = makeCursorAt(f.host, cx, cy);
    REQUIRE(f.event(settle, params) == PF_Err_NONE);

    // Moving within the same handle: no repaint.
    PF_EventExtra same = makeCursorAt(f.host, cx + 10, cy + 10);
    REQUIRE(f.event(same, params) == PF_Err_NONE);
    CHECK((same.evt_out_flags & PF_EO_UPDATE_NOW) == 0);

    // Onto a grip: the highlight moves, so repaint.
    PF_EventExtra grip = makeCursorAt(f.host, 5, 5);
    REQUIRE(f.event(grip, params) == PF_Err_NONE);
    CHECK((grip.evt_out_flags & PF_EO_UPDATE_NOW) != 0);

    // Still on the grip: no repaint.
    PF_EventExtra gripAgain = makeCursorAt(f.host, 8, 8);
    REQUIRE(f.event(gripAgain, params) == PF_Err_NONE);
    CHECK((gripAgain.evt_out_flags & PF_EO_UPDATE_NOW) == 0);

    // Leaving the view drops the highlight, once.
    PF_EventExtra exited = makeExtra(f.host, PF_Event_MOUSE_EXITED);
    REQUIRE(f.event(exited, params) == PF_Err_NONE);
    CHECK((exited.evt_out_flags & PF_EO_UPDATE_NOW) != 0);

    PF_EventExtra exitedAgain = makeExtra(f.host, PF_Event_MOUSE_EXITED);
    REQUIRE(f.event(exitedAgain, params) == PF_Err_NONE);
    CHECK((exitedAgain.evt_out_flags & PF_EO_UPDATE_NOW) == 0);
}

TEST_CASE("while a drag is in flight the readout shows its values even if the host's are stale",
          "[reframe][ui][module][readout]") {
    // A host is free to hand a DRAW the values of the frame it last FINISHED
    // rendering.  With a slow render that is several drag events behind, and
    // a readout built from those values would lag as badly as the picture.
    // A stale copy of the parameter array stands in for such a host.
    UiFixture f;
    f.setFov(90.0);
    std::vector<PF_ParamDef*> params = f.params();

    std::vector<PF_ParamDef> staleDefs;
    staleDefs.reserve(params.size());
    for (PF_ParamDef* def : params) {
        REQUIRE(def != nullptr);
        staleDefs.push_back(*def);
    }
    std::vector<PF_ParamDef*> stale;
    stale.reserve(staleDefs.size());
    for (PF_ParamDef& def : staleDefs) {
        stale.push_back(&def);
    }

    PF_EventExtra click = makeClickAt(f.host, 960, 540);
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);

    // First move: the host's own array now holds the grab's Pan for 96 px,
    // the stale one still 0.  The readout shows what the array holds, so the
    // expected text is formatted from the committed value itself.
    PF_EventExtra move1 = makeDragFrom(f.host, click, 960 + 96, 540, false);
    REQUIRE(f.event(move1, params) == PF_Err_NONE);
    REQUIRE(angleOf(params, kIndexPan) == Approx(grabPanDeg(kPanTiltSensitivity * 96.0)).margin(0.01));
    const double panMove1 = angleOf(params, kIndexPan);
    REQUIRE(angleOf(stale, kIndexPan) == Approx(0.0).margin(1e-6));

    f.host.clearDrawbotRecord();
    PF_EventExtra draw1 = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(draw1, stale) == PF_Err_NONE);
    CHECK(drewText(f.host.drawbotRecord(), panReadout(panMove1)));

    // It tracks every move, not just the first.
    PF_EventExtra move2 = makeDragFrom(f.host, click, 960 + 192, 540, false);
    REQUIRE(f.event(move2, params) == PF_Err_NONE);
    REQUIRE(angleOf(params, kIndexPan) == Approx(grabPanDeg(kPanTiltSensitivity * 192.0)).margin(0.01));
    const double panMove2 = angleOf(params, kIndexPan);
    f.host.clearDrawbotRecord();
    PF_EventExtra draw2 = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(draw2, stale) == PF_Err_NONE);
    CHECK(drewText(f.host.drawbotRecord(), panReadout(panMove2)));

    // Another reframe instance never shows this gesture's numbers.
    {
        PF_ProgPtr other = f.host.createEffectRef(0x2001, 12);
        REQUIRE(other != nullptr);
        PF_InData otherIn = f.host.makeInData(other, {});
        PF_OutData otherOut = f.host.makeOutData();
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &otherIn, &otherOut, nullptr, nullptr,
                                                      nullptr) == PF_Err_NONE);
        otherIn = f.host.makeInData(other, {});
        std::vector<PF_ParamDef*> otherParams = f.host.renderParams(other);

        f.host.clearDrawbotRecord();
        PF_EventExtra otherDraw = makeExtra(f.host, PF_Event_DRAW);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_EVENT, &otherIn, &otherOut, otherParams.data(),
                                                      nullptr, &otherDraw) == PF_Err_NONE);
        CHECK(drewText(f.host.drawbotRecord(), panReadout(OSV_REFRAME_PAN_DEFAULT)));
        CHECK_FALSE(drewText(f.host.drawbotRecord(), panReadout(panMove2)));
        f.host.destroyEffectRef(other);
    }

    // Release: from here on the host is the authority again, so a stale
    // array shows stale numbers and the real one shows the committed value.
    PF_EventExtra release = makeDragFrom(f.host, click, 960 + 192, 540, true);
    REQUIRE(f.event(release, params) == PF_Err_NONE);

    f.host.clearDrawbotRecord();
    PF_EventExtra afterStale = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(afterStale, stale) == PF_Err_NONE);
    CHECK(drewText(f.host.drawbotRecord(), panReadout(0.0)));

    f.host.clearDrawbotRecord();
    PF_EventExtra afterReal = makeExtra(f.host, PF_Event_DRAW);
    REQUIRE(f.event(afterReal, params) == PF_Err_NONE);
    CHECK(drewText(f.host.drawbotRecord(), panReadout(panMove2)));
}

// ===========================================================================
//  [WP-CAMERA] DJI's lens and the Drag Sensitivity control
// ===========================================================================
namespace {

/// Set a float slider of the fixture's instance.
void setSlider(UiFixture& f, int index, double value) {
    std::vector<PF_ParamDef> added = f.host.addedParams(f.ref);
    REQUIRE(static_cast<std::size_t>(index) <= added.size());
    PF_ParamDef def = added[static_cast<std::size_t>(index) - 1u];
    REQUIRE(def.param_type == PF_Param_FLOAT_SLIDER);
    def.u.fs_d.value = static_cast<PF_FpShort>(value);
    f.host.setParamValue(f.ref, index, def);
}

/// [WP-LENSUI] Pick DJI or Classic in the Lens popup of the fixture's
/// instance.  The hidden Camera Model mirror is deliberately set to the
/// OPPOSITE lens, so an overlay that still read the checkbox would show the
/// wrong read-out and drag the wrong lens - the popup must be what decides.
void setDjiLens(UiFixture& f, bool on) {
    std::vector<PF_ParamDef> added = f.host.addedParams(f.ref);
    PF_ParamDef def = added[static_cast<std::size_t>(kIndexLens) - 1u];
    REQUIRE(def.param_type == PF_Param_POPUP);
    def.u.pd.value = static_cast<A_long>(on ? LensPopup::Dji : LensPopup::Classic);
    f.host.setParamValue(f.ref, kIndexLens, def);
    PF_ParamDef mirror = added[static_cast<std::size_t>(kIndexCameraModel) - 1u];
    REQUIRE(mirror.param_type == PF_Param_CHECKBOX);
    mirror.u.bd.value = on ? 0 : 1;
    f.host.setParamValue(f.ref, kIndexCameraModel, mirror);
}

/// A float slider's value in an event's parameter array.
[[nodiscard]] double sliderOf(const std::vector<PF_ParamDef*>& array, int index) {
    return static_cast<double>(array[static_cast<std::size_t>(index)]->u.fs_d.value);
}

/// The direction of the sphere under `pointer` through DJI's lens - the
/// renderer's own camera (buildView on DJI's lens) and rotation.
[[nodiscard]] bool djiRayUnderPointer(const Layout& layout, double djiFov, double correction,
                                      const CameraValues& cam, const PointF& pointer, double out[3]) {
    Settings s;
    s.cameraModel = CameraModel::Dji;
    s.djiFovDeg = djiFov;
    s.correction = correction;
    s.panDeg = cam.panDeg;
    s.tiltDeg = cam.tiltDeg;
    s.rollDeg = cam.rollDeg;
    const int w = static_cast<int>(std::lround(layout.viewport.w));
    const int h = static_cast<int>(std::lround(layout.viewport.h));
    const ViewSetup view = buildView(s, w, h, SizePx{w, h});
    if (!view.valid) {
        return false;
    }
    const OsvReframeParams& p = view.params;
    const float nx = static_cast<float>((pointer.x - layout.viewport.x) - 0.5 * layout.viewport.w);
    const float ny = static_cast<float>(0.5 * layout.viewport.h - (pointer.y - layout.viewport.y));
    float d[3] = {0.0f, 0.0f, 0.0f};
    if (!osvViewRay(p.projection, p.focalPx, p.eyeOffset, p.tanHalfH, p.tanHalfV, static_cast<float>(w),
                    static_cast<float>(h), nx, ny, d)) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        out[r] = static_cast<double>(p.Rout[r * 3 + 0]) * d[0] + static_cast<double>(p.Rout[r * 3 + 1]) * d[1] +
                 static_cast<double>(p.Rout[r * 3 + 2]) * d[2];
    }
    const double n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (!(n > 0.0)) {
        return false;
    }
    for (int r = 0; r < 3; ++r) {
        out[r] /= n;
    }
    return true;
}

}  // namespace

TEST_CASE("Drag Sensitivity sets how much faster than the hand a drag turns the view", "[reframe][ui][dji]") {
    // The fixed-rate path (no grab): the rate is sensitivity * FOV / width,
    // so doubling the control doubles the turn for the same hand movement.
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.fovDeg = 90.0;
    for (const double s : {1.0, 2.0, 4.0}) {
        INFO("sensitivity " << s);
        DragState state = beginDrag(layout, layout.centre, start, kModNone);
        REQUIRE_FALSE(state.grab.valid);
        state.sensitivity = s;
        const CameraValues v = applyDrag(state, PointF{layout.centre.x + 100.0, layout.centre.y}, kModNone);
        CHECK(v.panDeg == Approx(s * 90.0 / kFrameW * 100.0).margin(1e-9));
    }
    // A fresh gesture uses the old constant, and nonsense falls back to it.
    CHECK(DragState{}.sensitivity == kPanTiltSensitivity);
    CHECK(kPanTiltSensitivity == OSV_REFRAME_DRAG_SENSITIVITY_DEFAULT);
    CHECK(effectiveSensitivity(std::numeric_limits<double>::quiet_NaN()) == kPanTiltSensitivity);
    CHECK(effectiveSensitivity(1e9) == OSV_REFRAME_DRAG_SENSITIVITY_VALID_MAX);
    CHECK(effectiveSensitivity(-3.0) == OSV_REFRAME_DRAG_SENSITIVITY_VALID_MIN);
    CHECK(effectiveSensitivity(1.25) == 1.25);
}

TEST_CASE("a drag through DJI's lens turns Pan and Tilt per axis", "[reframe][ui][grab][dji]") {
    // The drag is cast through whatever lens renders; on DJI's lens (the
    // pinhole-behind-the-sphere camera) the contract is exactly the Classic
    // one: horizontal travel turns Pan by the azimuth the picture spans along
    // the horizontal centre line, vertical travel turns Tilt by the elevation
    // along the vertical one, each for the sensitivity-scaled travel.
    const Layout layout = computeLayout(fullFrame());
    CameraValues start;
    start.dji = true;
    start.djiFovDeg = 103.3;
    start.correction = 0.67;
    start.panDeg = 20.0;
    start.tiltDeg = -5.9;
    const PointF anchor{layout.centre.x + 150.0, layout.centre.y - 80.0};

    Settings s;
    s.cameraModel = CameraModel::Dji;
    s.djiFovDeg = start.djiFovDeg;
    s.correction = start.correction;
    s.panDeg = start.panDeg;
    s.tiltDeg = start.tiltDeg;
    const ViewSetup view = buildView(s, 1920, 1080, SizePx{1920, 1080});
    REQUIRE(view.valid);
    REQUIRE(view.params.projection == OSV_PROJ_DJI_SPHERE);

    // The expected angles, from the renderer's DJI camera with a level view
    // (identity rotation): azimuth on the horizontal centre line, minus the
    // elevation on the vertical one.
    CameraValues level = start;
    level.panDeg = 0.0;
    level.tiltDeg = 0.0;
    auto azimuthDeg = [&](double x) {
        double r[3];
        REQUIRE(djiRayUnderPointer(layout, start.djiFovDeg, start.correction, level, PointF{x, layout.centre.y}, r));
        return std::atan2(r[0], r[1]) * (180.0 / 3.14159265358979323846);
    };
    auto dropDeg = [&](double y) {
        double r[3];
        REQUIRE(djiRayUnderPointer(layout, start.djiFovDeg, start.correction, level, PointF{layout.centre.x, y}, r));
        return -std::atan2(r[2], r[1]) * (180.0 / 3.14159265358979323846);
    };

    for (const double sensitivity : {1.0, 2.0}) {
        INFO("sensitivity " << sensitivity);
        DragState state = beginDrag(layout, anchor, start, kModNone);
        state.sensitivity = sensitivity;
        state.grab = beginSphereGrab(view.params.projection, view.params.focalPx, view.params.eyeOffset,
                                     view.params.tanHalfH, view.params.tanHalfV, layout, start, anchor);
        REQUIRE(state.grab.cameraValid);
        const CameraValues after = applyDrag(state, PointF{anchor.x + 120.0, anchor.y + 60.0}, kModNone);
        const double quickX = anchor.x + sensitivity * 120.0;
        const double quickY = anchor.y + sensitivity * 60.0;
        CHECK(after.panDeg == Approx(start.panDeg + azimuthDeg(quickX) - azimuthDeg(anchor.x)).margin(1e-3));
        CHECK(after.tiltDeg == Approx(start.tiltDeg + dropDeg(quickY) - dropDeg(anchor.y)).margin(1e-3));
        // A drag changes neither the lens nor the roll.
        CHECK(after.djiFovDeg == start.djiFovDeg);
        CHECK(after.correction == start.correction);
        CHECK(after.rollDeg == start.rollDeg);
    }
}

TEST_CASE("a zoom drag on DJI's lens walks DJI Studio's zoom path", "[reframe][ui][dji]") {
    const Layout layout = computeLayout(fullFrame());
    RectF grips[4];
    REQUIRE(fovGripRects(layout, grips) == 4);
    CameraValues start;
    start.dji = true;
    start.fovDeg = 120.0;
    start.djiFovDeg = 60.0;
    start.correction = 0.6;

    SECTION("both DJI controls move together; the Classic FOV does not") {
        DragState state = beginDrag(layout, grips[3].centre(), start, kModNone);
        REQUIRE(state.handle == Handle::Fov);
        const CameraValues v = applyDrag(state, PointF{grips[3].centre().x, grips[3].centre().y + 80.0}, kModNone);
        // FOV at the Classic rate, correction 1/130 of it (DJI's gesture).
        CHECK(v.djiFovDeg == Approx(60.0 + 80.0 * kFovDegPerPixel));
        CHECK(v.correction == Approx(0.6 + 80.0 * kFovDegPerPixel / OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION));
        CHECK(v.fovDeg == 120.0);
        CHECK(changedFieldsFor(DragMode::Fov, true) == (kChangedDjiFov | kChangedCorrection));
    }
    SECTION("the path stops at DJI Studio's limits") {
        DragState state = beginDrag(layout, grips[3].centre(), start, kModNone);
        const CameraValues out =
            applyDrag(state, PointF{grips[3].centre().x, grips[3].centre().y + 5000.0}, kModNone);
        CHECK(out.djiFovDeg == Approx(150.0));
        CHECK(out.correction == Approx(1.0));
        DragState state2 = beginDrag(layout, grips[3].centre(), start, kModNone);
        const CameraValues in =
            applyDrag(state2, PointF{grips[3].centre().x, grips[3].centre().y - 5000.0}, kModNone);
        CHECK(in.djiFovDeg == Approx(20.0));
        CHECK(in.correction == Approx(0.0));
    }
    SECTION("a Crystal Ball is not snapped back to DJI Studio's 1.0") {
        CameraValues crystal = start;
        crystal.djiFovDeg = 75.0;
        crystal.correction = 1.8;
        DragState state = beginDrag(layout, grips[3].centre(), crystal, kModNone);
        const CameraValues v = applyDrag(state, PointF{grips[3].centre().x, grips[3].centre().y - 40.0}, kModNone);
        CHECK(v.correction < 1.8);
        CHECK(v.correction > 1.0);
        CHECK(v.djiFovDeg < 75.0);
    }
    SECTION("every other mode writes the same fields on either lens") {
        for (const DragMode m :
             {DragMode::PanTilt, DragMode::PanOnly, DragMode::TiltOnly, DragMode::Roll, DragMode::None}) {
            CHECK(changedFieldsFor(m, true) == changedFieldsFor(m));
        }
        CHECK(changedFieldsFor(DragMode::Fov, false) == kChangedFov);
    }
}

TEST_CASE("the live readout and sanitise know DJI's lens", "[reframe][ui][readout][dji]") {
    CameraValues host;
    host.dji = true;
    host.djiFovDeg = 60.0;
    host.correction = 0.6;
    CameraValues live = host;
    live.djiFovDeg = 80.0;
    live.correction = 0.75;
    // The gesture's DJI numbers are shown while it is in flight...
    const CameraValues shown = mergeLiveReadout(host, live, kChangedDjiFov | kChangedCorrection);
    CHECK(shown.djiFovDeg == 80.0);
    CHECK(shown.correction == 0.75);
    // ...unless it was made on the other lens: then it is some other state.
    CameraValues otherLens = live;
    otherLens.dji = false;
    const CameraValues hostWins = mergeLiveReadout(host, otherLens, kChangedDjiFov | kChangedCorrection);
    CHECK(hostWins.djiFovDeg == 60.0);
    CHECK(hostWins.correction == 0.6);

    CameraValues wild;
    wild.djiFovDeg = 1e6;
    wild.correction = std::numeric_limits<double>::quiet_NaN();
    const CameraValues clean = sanitise(wild);
    CHECK(clean.djiFovDeg == OSV_REFRAME_DJI_FOV_VALID_MAX);
    CHECK(clean.correction == OSV_REFRAME_CORRECTION_DEFAULT);
    wild.correction = 9.0;
    CHECK(sanitise(wild).correction == OSV_REFRAME_CORRECTION_VALID_MAX);
}

TEST_CASE("DRAW on DJI's lens reads Zoom, FOV and Correction as DJI Studio prints them",
          "[reframe][ui][module][draw][dji]") {
    UiFixture f;
    setDjiLens(f, true);
    setSlider(f, kIndexDjiFov, 103.3);
    setSlider(f, kIndexCorrection, 0.67);
    f.setAngle(kIndexPan, 144.8);
    f.setAngle(kIndexTilt, -5.9);
    std::vector<PF_ParamDef*> params = f.params();

    f.host.clearDrawbotRecord();
    PF_EventExtra extra = makeExtra(f.host, PF_Event_DRAW);
    extra.u.draw.depth = 32;
    REQUIRE(f.event(extra, params) == PF_Err_NONE);
    const DrawbotRecord record = f.host.drawbotRecord();
    REQUIRE_FALSE(record.strings.empty());
    const std::string& text = record.strings.front().text;
    INFO("readout: " << text);
    // DJI Studio printed Zoom 207.1 for these numbers; the formula at the
    // displayed Correction gives 207.5 (docs/research/DJI_CAMERA.md: the
    // difference is DJI's rounding of the Correction it displays).
    CHECK(text.find("Zoom 207.5") != std::string::npos);
    CHECK(text.find("FOV 103.3") != std::string::npos);
    CHECK(text.find("Correction 0.67") != std::string::npos);
    CHECK(text.find("Pan 144.8") != std::string::npos);
    CHECK(text.find("Tilt -5.9") != std::string::npos);

    // With Classic picked in the Lens popup (and the hidden checkbox now
    // ticked, the opposite), the HUD is the Classic readout it always was.
    setDjiLens(f, false);
    params = f.params();
    f.host.clearDrawbotRecord();
    PF_EventExtra classic = makeExtra(f.host, PF_Event_DRAW);
    classic.u.draw.depth = 32;
    REQUIRE(f.event(classic, params) == PF_Err_NONE);
    REQUIRE_FALSE(f.host.drawbotRecord().strings.empty());
    const std::string classicText = f.host.drawbotRecord().strings.front().text;
    CHECK(classicText.find("Zoom") == std::string::npos);
    CHECK(classicText.find("FOV 120.0") != std::string::npos);
}

TEST_CASE("a new instance's HUD and zoom drag follow the default lens, DJI", "[reframe][ui][module][lens]") {
    // [WP-LENSUI] Put the Lens popup back exactly as registered - its
    // default, DJI - undoing the fixture's Classic.
    UiFixture f;
    {
        std::vector<PF_ParamDef> added = f.host.addedParams(f.ref);
        PF_ParamDef lens = added[static_cast<std::size_t>(kIndexLens) - 1u];
        lens.u.pd.value = lens.u.pd.dephault;
        REQUIRE(cameraModelFromLensPopup(lens.u.pd.value) == CameraModel::Dji);
        f.host.setParamValue(f.ref, kIndexLens, lens);
    }
    std::vector<PF_ParamDef*> params = f.params();

    // The read-out is DJI's three numbers at their defaults.
    f.host.clearDrawbotRecord();
    PF_EventExtra draw = makeExtra(f.host, PF_Event_DRAW);
    draw.u.draw.depth = 32;
    REQUIRE(f.event(draw, params) == PF_Err_NONE);
    REQUIRE_FALSE(f.host.drawbotRecord().strings.empty());
    const std::string text = f.host.drawbotRecord().strings.front().text;
    INFO("readout: " << text);
    CHECK(text.find("Zoom") != std::string::npos);
    CHECK(text.find("FOV 60.0") != std::string::npos);
    CHECK(text.find("Correction 0.60") != std::string::npos);

    // And a Ctrl-drag zooms DJI's lens, leaving the hidden Classic FOV alone.
    PF_EventExtra click = makeClickAt(f.host, 960, 540);
    click.u.do_click.modifiers = PF_Mod_CMD_CTRL_KEY;
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);
    PF_EventExtra drag = makeDragFrom(f.host, click, 960, 540 + 40, false);
    drag.u.do_click.modifiers = PF_Mod_CMD_CTRL_KEY;
    REQUIRE(f.event(drag, params) == PF_Err_NONE);
    CHECK(changed(params, kIndexDjiFov));
    CHECK(changed(params, kIndexCorrection));
    CHECK_FALSE(changed(params, kIndexFov));
}

TEST_CASE("a Ctrl-drag zoom on DJI's lens commits DJI FOV, Correction and Zoom through the module",
          "[reframe][ui][module][dji]") {
    UiFixture f;
    setDjiLens(f, true);
    setSlider(f, kIndexDjiFov, 60.0);
    setSlider(f, kIndexCorrection, 0.6);
    std::vector<PF_ParamDef*> params = f.params();

    PF_EventExtra click = makeClickAt(f.host, 960, 540);
    click.u.do_click.modifiers = PF_Mod_CMD_CTRL_KEY;
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    REQUIRE(click.u.do_click.send_drag == TRUE);
    PF_EventExtra drag = makeDragFrom(f.host, click, 960, 540 + 80, false);
    drag.u.do_click.modifiers = PF_Mod_CMD_CTRL_KEY;
    REQUIRE(f.event(drag, params) == PF_Err_NONE);

    CHECK(changed(params, kIndexDjiFov));
    CHECK(changed(params, kIndexCorrection));
    CHECK(changed(params, kIndexZoom));
    CHECK_FALSE(changed(params, kIndexFov));
    CHECK_FALSE(changed(params, kIndexPan));
    const double fov = sliderOf(params, kIndexDjiFov);
    const double cor = sliderOf(params, kIndexCorrection);
    CHECK(fov == Approx(60.0 + 80.0 * kFovDegPerPixel).margin(1e-3));
    CHECK(cor == Approx(0.6 + 80.0 * kFovDegPerPixel / OSV_REFRAME_DJI_ZOOM_FOV_PER_CORRECTION).margin(1e-4));
    // The Zoom read-out follows the lens (the frame is 16:9).
    CHECK(sliderOf(params, kIndexZoom) == Approx(djiZoomDeg(DjiLens{fov, cor}, kFrameW / kFrameH)).margin(1e-2));
}

TEST_CASE("the Drag Sensitivity control reaches the module's drag", "[reframe][ui][module][dji]") {
    // With the control at 1.0 the drag turns exactly the angles the picture
    // spans under the hand's own travel - not twice as far.
    UiFixture f;
    f.setAngle(kIndexPan, 0.0);
    f.setAngle(kIndexTilt, 0.0);
    f.setFov(90.0);
    setSlider(f, kIndexDragSensitivity, 1.0);
    std::vector<PF_ParamDef*> params = f.params();

    PF_EventExtra click = makeClickAt(f.host, 960, 540);
    REQUIRE(f.event(click, params) == PF_Err_NONE);
    PF_EventExtra drag = makeDragFrom(f.host, click, 960 + 300, 540 + 100, false);
    REQUIRE(f.event(drag, params) == PF_Err_NONE);
    REQUIRE(changed(params, kIndexPan));

    const double distortion = static_cast<double>(params[kIndexDistortion]->u.fs_d.value);
    CHECK(angleOf(params, kIndexPan) == Approx(grabPanDeg(300.0, 90.0, distortion)).margin(0.01));
    CHECK(angleOf(params, kIndexTilt) == Approx(grabTiltDeg(100.0, 90.0, distortion)).margin(0.01));
}
