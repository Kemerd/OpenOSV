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

#include "ReframeParams.h"
#include "ReframeUi.h"

#include "MockHost.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "AE_EffectSuites.h"
#include "AE_EffectUI.h"

#include <cmath>
#include <cstring>
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
    CHECK(after.panDeg == Approx(90.0 * 0.25));
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
    CHECK(after.panDeg == Approx(-90.0 * 0.25));
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
    CHECK(after.tiltDeg == Approx(270.0 * 90.0 / 1920.0));
    CHECK(after.panDeg == Approx(start.panDeg));
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
        CHECK(after.panDeg == Approx(300.0 * 90.0 / 1920.0));
        CHECK(after.tiltDeg == Approx(0.0));  // The 40px of vertical travel is discarded.
    }

    SECTION("a mostly vertical drag becomes tilt only") {
        DragState state = beginDrag(layout, layout.centre, start, kModShift);
        const CameraValues after =
            applyDrag(state, PointF{layout.centre.x + 40.0, layout.centre.y + 300.0}, kModShift);

        CHECK(state.mode == DragMode::TiltOnly);
        CHECK(after.tiltDeg == Approx(300.0 * 90.0 / 1920.0));
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
        CHECK(after.panDeg == Approx(10.0 * 90.0 / 1920.0));
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

    // The signs and the magnitudes, pinned.  Right -> Pan up, down -> Tilt up.
    CHECK(angleOf(params, kIndexPan) == Approx(480.0 * 90.0 / 1920.0).margin(0.01));
    CHECK(angleOf(params, kIndexTilt) == Approx(270.0 * 90.0 / 1920.0).margin(0.01));

    // Roll and FOV were not part of this drag and must carry no change flag:
    // a spurious Roll keyframe from a pan is exactly the bug this guards.
    CHECK_FALSE(changed(params, kIndexRoll));
    CHECK_FALSE(changed(params, kIndexFov));

    // ---- release ----------------------------------------------------------
    PF_EventExtra release = makeExtra(f.host, PF_Event_DRAG);
    std::memcpy(release.u.do_click.continue_refcon, click.u.do_click.continue_refcon,
                sizeof(release.u.do_click.continue_refcon));
    release.u.do_click.screen_point.h = 960 + 480;
    release.u.do_click.screen_point.v = 540 + 270;
    release.u.do_click.last_time = TRUE;

    REQUIRE(f.event(release, params) == PF_Err_NONE);

    // The value is unchanged by the release itself, and the gesture is over.
    CHECK(angleOf(params, kIndexPan) == Approx(480.0 * 90.0 / 1920.0).margin(0.01));

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

    PF_EventExtra click = makeExtra(f.host, PF_Event_DO_CLICK);
    click.u.do_click.screen_point.h = 0;
    click.u.do_click.screen_point.v = 0;
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
        CHECK(angleOf(params, kIndexPan) == Approx(96.0 * 90.0 / 1920.0).margin(0.01));
    }
}
