// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_easing.cpp - the Keyframe Easing presets (ReframeEasing.h) and the
// Keyframe Easing popup of Open 360 Reframe, end to end.
//
// Four layers, each pinned independently:
//
//   1. the curves themselves - endpoints, monotonicity, the documented speed
//      profiles, the mirror symmetries, None and Linear as the identity -
//      against the ReframeEasing.cpp the .aex contains;
//   2. the value a preset gives a control between two keyframes, on a
//      synthetic keyframe set, and the backwards keyframe search the GPU path
//      builds out of Premiere's forwards-only GetNextKeyframeTime;
//   3. the popup itself: registered last, id 22, None by default, found by
//      the GPU filter's parameter probe on hosts that number popups from 0
//      and from 1;
//   4. the loaded module: the CPU path (PF_Cmd_RENDER, keyframes through the
//      PF Param Utils Suite) and the GPU path (Render, keyframes through the
//      Video Segment Suite) render the SAME eased picture, and "None" renders
//      exactly what the effect rendered before the popup existed.
//
// The keyframe values are chosen so the eased values are exact binary
// fractions (every curve at u = 1/2 lands on a multiple of 1/16), which lets
// the CPU comparisons be byte-exact rather than "close".

#include "GpuTestSupport.h"
#include "ReframeTestSupport.h"

#include "ReframeCpu.h"
#include "ReframeEasing.h"
#include "ReframeParams.h"

#include "MockHost.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "PrSDKPixelFormat.h"
#include "PrSDKVideoSegmentSuite.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using Catch::Approx;
using osv::premiere::mock::EffectWorld;
using osv::premiere::mock::InDataSpec;
using osv::premiere::mock::MockHost;

namespace {

/// Every preset that draws a time curve (None and Linear Smooth excluded).
constexpr KeyframeEasing kTimeCurves[] = {KeyframeEasing::Linear, KeyframeEasing::FastInSlowOut,
                                          KeyframeEasing::SlowInFastOut, KeyframeEasing::FastInFastOut,
                                          KeyframeEasing::SlowInSlowOut};

/// Every popup entry, in popup order.
constexpr KeyframeEasing kAllEasings[] = {KeyframeEasing::None,          KeyframeEasing::LinearSmooth,
                                          KeyframeEasing::FastInSlowOut, KeyframeEasing::SlowInFastOut,
                                          KeyframeEasing::FastInFastOut, KeyframeEasing::SlowInSlowOut,
                                          KeyframeEasing::Linear};

/// A synthetic keyframe set: time -> value, plus a switch that makes every
/// value read fail (the host refusing a read).
class MapTrack final : public KeyframeTrack {
public:
    std::map<double, double> keys;
    bool failValues = false;
    int valueReads = 0;

    std::optional<double> keyAtOrBefore(double t) override {
        auto it = keys.upper_bound(t);
        if (it == keys.begin()) {
            return std::nullopt;
        }
        return std::prev(it)->first;
    }
    std::optional<double> keyBefore(double t) override {
        auto it = keys.lower_bound(t);
        if (it == keys.begin()) {
            return std::nullopt;
        }
        return std::prev(it)->first;
    }
    std::optional<double> keyAfter(double t) override {
        auto it = keys.upper_bound(t);
        if (it == keys.end()) {
            return std::nullopt;
        }
        return it->first;
    }
    std::optional<double> valueAt(double t) override {
        ++valueReads;
        if (failValues) {
            return std::nullopt;
        }
        auto it = keys.find(t);
        return it == keys.end() ? std::nullopt : std::optional<double>(it->second);
    }
};

}  // namespace

// ===========================================================================
//  1. The curves
// ===========================================================================

TEST_CASE("every easing curve starts at 0 and ends at 1", "[reframe][easing]") {
    for (const KeyframeEasing e : kAllEasings) {
        INFO("easing " << static_cast<int>(e));
        CHECK(easeProgress(e, 0.0) == 0.0);
        CHECK(easeProgress(e, 1.0) == 1.0);
    }
}

TEST_CASE("None, Linear and Linear Smooth are the identity in time", "[reframe][easing]") {
    for (int i = 0; i <= 1000; ++i) {
        const double u = i / 1000.0;
        CHECK(easeProgress(KeyframeEasing::None, u) == u);
        CHECK(easeProgress(KeyframeEasing::Linear, u) == u);
        CHECK(easeProgress(KeyframeEasing::LinearSmooth, u) == u);
        CHECK(easeSpeed(KeyframeEasing::Linear, u) == 1.0);
    }
}

TEST_CASE("every easing curve is monotone and its speed never negative", "[reframe][easing]") {
    for (const KeyframeEasing e : kTimeCurves) {
        INFO("easing " << static_cast<int>(e));
        double previous = 0.0;
        for (int i = 0; i <= 4000; ++i) {
            const double u = i / 4000.0;
            const double s = easeProgress(e, u);
            CHECK(s >= previous);
            CHECK(s >= 0.0);
            CHECK(s <= 1.0);
            CHECK(easeSpeed(e, u) >= 0.0);
            previous = s;
        }
    }
}

TEST_CASE("each curve's speed is the derivative of its progress", "[reframe][easing]") {
    // A central difference against the closed-form speed: proves the two
    // documented formulas describe the same curve.
    constexpr double h = 1e-6;
    for (const KeyframeEasing e : kTimeCurves) {
        INFO("easing " << static_cast<int>(e));
        for (int i = 1; i < 100; ++i) {
            const double u = i / 100.0;
            const double numeric = (easeProgress(e, u + h) - easeProgress(e, u - h)) / (2.0 * h);
            CHECK(numeric == Approx(easeSpeed(e, u)).margin(1e-6));
        }
    }
}

TEST_CASE("the speed profiles match DJI Studio's preset shapes", "[reframe][easing]") {
    // Slow In, Slow Out: a bell - zero at both keyframes, fastest halfway.
    CHECK(easeSpeed(KeyframeEasing::SlowInSlowOut, 0.0) == 0.0);
    CHECK(easeSpeed(KeyframeEasing::SlowInSlowOut, 1.0) == 0.0);
    CHECK(easeSpeed(KeyframeEasing::SlowInSlowOut, 0.5) == Approx(1.5));
    // Fast In, Fast Out: a valley - fast at both keyframes, slowest halfway,
    // and never stopped.
    CHECK(easeSpeed(KeyframeEasing::FastInFastOut, 0.0) == Approx(2.0));
    CHECK(easeSpeed(KeyframeEasing::FastInFastOut, 1.0) == Approx(2.0));
    CHECK(easeSpeed(KeyframeEasing::FastInFastOut, 0.5) == Approx(0.5));
    // Fast In, Slow Out: leaves fast, arrives stopped, flat at both ends.
    CHECK(easeSpeed(KeyframeEasing::FastInSlowOut, 0.0) == Approx(2.0));
    CHECK(easeSpeed(KeyframeEasing::FastInSlowOut, 1.0) == Approx(0.0).margin(1e-12));
    // Slow In, Fast Out: the reverse.
    CHECK(easeSpeed(KeyframeEasing::SlowInFastOut, 0.0) == Approx(0.0).margin(1e-12));
    CHECK(easeSpeed(KeyframeEasing::SlowInFastOut, 1.0) == Approx(2.0));
    // "Flat at both ends": the speed barely moves one step in from either end.
    for (const KeyframeEasing e : {KeyframeEasing::FastInSlowOut, KeyframeEasing::SlowInFastOut}) {
        CHECK(std::fabs(easeSpeed(e, 0.01) - easeSpeed(e, 0.0)) < 1e-3);
        CHECK(std::fabs(easeSpeed(e, 0.99) - easeSpeed(e, 1.0)) < 1e-3);
    }
}

TEST_CASE("the symmetric presets are point-symmetric and the asymmetric ones are mirror images",
          "[reframe][easing]") {
    for (int i = 0; i <= 1000; ++i) {
        const double u = i / 1000.0;
        INFO("u " << u);
        CHECK(easeProgress(KeyframeEasing::SlowInSlowOut, 1.0 - u) ==
              Approx(1.0 - easeProgress(KeyframeEasing::SlowInSlowOut, u)).margin(1e-12));
        CHECK(easeProgress(KeyframeEasing::FastInFastOut, 1.0 - u) ==
              Approx(1.0 - easeProgress(KeyframeEasing::FastInFastOut, u)).margin(1e-12));
        CHECK(easeProgress(KeyframeEasing::SlowInFastOut, u) ==
              Approx(1.0 - easeProgress(KeyframeEasing::FastInSlowOut, 1.0 - u)).margin(1e-12));
    }
    // Halfway, exactly: the values the byte-exact render tests below rely on.
    CHECK(easeProgress(KeyframeEasing::SlowInSlowOut, 0.5) == 0.5);
    CHECK(easeProgress(KeyframeEasing::FastInFastOut, 0.5) == 0.5);
    CHECK(easeProgress(KeyframeEasing::FastInSlowOut, 0.5) == 13.0 / 16.0);
    CHECK(easeProgress(KeyframeEasing::SlowInFastOut, 0.5) == 3.0 / 16.0);
}

TEST_CASE("garbage times never leave the unit interval", "[reframe][easing]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const KeyframeEasing e : kAllEasings) {
        INFO("easing " << static_cast<int>(e));
        CHECK(easeProgress(e, nan) == 0.0);
        CHECK(easeProgress(e, -inf) == 0.0);
        CHECK(easeProgress(e, inf) == 1.0);
        CHECK(easeProgress(e, -3.0) == 0.0);
        CHECK(easeProgress(e, 7.0) == 1.0);
        CHECK(std::isfinite(easeSpeed(e, nan)));
    }
    // An out-of-range popup value is None, never a curve.
    CHECK(sanitiseKeyframeEasing(0) == KeyframeEasing::None);
    CHECK(sanitiseKeyframeEasing(8) == KeyframeEasing::None);
    CHECK(sanitiseKeyframeEasing(-5) == KeyframeEasing::None);
    CHECK(sanitiseKeyframeEasing(3) == KeyframeEasing::FastInSlowOut);
}

TEST_CASE("the Hermite segment honours its ends, its slopes and bad input", "[reframe][easing]") {
    // Ends.
    CHECK(hermiteValue(0.0, 2.0, 5.0, 10.0, 7.0, -1.0, 0.0) == 2.0);
    CHECK(hermiteValue(0.0, 2.0, 5.0, 10.0, 7.0, -1.0, 10.0) == Approx(7.0));
    // Slopes, by central difference just inside each end.
    constexpr double h = 1e-5;
    const double startSlope =
        (hermiteValue(0.0, 2.0, 5.0, 10.0, 7.0, -1.0, 2 * h) - hermiteValue(0.0, 2.0, 5.0, 10.0, 7.0, -1.0, 0.0)) /
        (2 * h);
    CHECK(startSlope == Approx(5.0).margin(1e-3));
    // Equal tangents on a straight line: exactly linear.
    CHECK(hermiteValue(0.0, 0.0, 1.0, 16.0, 16.0, 1.0, 4.0) == Approx(4.0).margin(1e-12));
    // Degenerate and garbage input answers the start value, never NaN.
    CHECK(hermiteValue(5.0, 3.0, 1.0, 5.0, 9.0, 1.0, 5.0) == 3.0);
    CHECK(hermiteValue(0.0, 3.0, std::numeric_limits<double>::quiet_NaN(), 1.0, 9.0, 1.0, 0.5) == 3.0);
}

// ===========================================================================
//  2. A control between its keyframes
// ===========================================================================

TEST_CASE("a preset eases a control between two keyframes and nowhere else", "[reframe][easing]") {
    MapTrack track;
    track.keys = {{0.0, 0.0}, {20.0, 16.0}};

    SECTION("None asks the host nothing and keeps its value") {
        CHECK_FALSE(easedValue(KeyframeEasing::None, track, 10.0).has_value());
        CHECK(track.valueReads == 0);
    }
    SECTION("each time curve lands on its documented value halfway") {
        CHECK(*easedValue(KeyframeEasing::Linear, track, 10.0) == 8.0);
        CHECK(*easedValue(KeyframeEasing::SlowInSlowOut, track, 10.0) == 8.0);
        CHECK(*easedValue(KeyframeEasing::FastInFastOut, track, 10.0) == 8.0);
        CHECK(*easedValue(KeyframeEasing::FastInSlowOut, track, 10.0) == 13.0);
        CHECK(*easedValue(KeyframeEasing::SlowInFastOut, track, 10.0) == 3.0);
        // A quarter of the way: v0 + (v1 - v0) * s(0.25).
        CHECK(*easedValue(KeyframeEasing::SlowInSlowOut, track, 5.0) ==
              Approx(16.0 * easeProgress(KeyframeEasing::SlowInSlowOut, 0.25)));
    }
    SECTION("at a keyframe the value is the keyframe's") {
        for (const KeyframeEasing e : kTimeCurves) {
            CHECK(*easedValue(e, track, 0.0) == 0.0);
        }
    }
    SECTION("before the first and after the last keyframe the host's value stays") {
        CHECK_FALSE(easedValue(KeyframeEasing::SlowInSlowOut, track, -1.0).has_value());
        CHECK_FALSE(easedValue(KeyframeEasing::SlowInSlowOut, track, 20.0).has_value());
        CHECK_FALSE(easedValue(KeyframeEasing::SlowInSlowOut, track, 25.0).has_value());
    }
    SECTION("a control with one keyframe, or none, is not eased") {
        MapTrack one;
        one.keys = {{0.0, 5.0}};
        CHECK_FALSE(easedValue(KeyframeEasing::SlowInSlowOut, one, 3.0).has_value());
        MapTrack none;
        CHECK_FALSE(easedValue(KeyframeEasing::SlowInSlowOut, none, 3.0).has_value());
    }
    SECTION("a host that refuses the values leaves its own value in place") {
        track.failValues = true;
        CHECK_FALSE(easedValue(KeyframeEasing::FastInSlowOut, track, 10.0).has_value());
    }
}

TEST_CASE("Linear Smooth is linear on a straight run and smooth through a corner", "[reframe][easing]") {
    SECTION("two keyframes: exactly linear") {
        MapTrack track;
        track.keys = {{0.0, 0.0}, {16.0, 16.0}};
        for (int t = 0; t < 16; ++t) {
            CHECK(*easedValue(KeyframeEasing::LinearSmooth, track, t) == Approx(t).margin(1e-12));
        }
    }
    SECTION("evenly spaced, evenly stepped keyframes: exactly linear") {
        MapTrack track;
        track.keys = {{0.0, 0.0}, {16.0, 16.0}, {32.0, 32.0}, {48.0, 48.0}};
        for (int t = 0; t < 48; ++t) {
            CHECK(*easedValue(KeyframeEasing::LinearSmooth, track, t) == Approx(t).margin(1e-12));
        }
    }
    SECTION("a corner: the tangent there is the average of the two slopes") {
        // Up at slope 1, then down at slope -1: the curve arrives at the peak
        // flat (tangent 0) instead of bouncing off it.
        MapTrack track;
        track.keys = {{0.0, 0.0}, {16.0, 16.0}, {32.0, 0.0}};
        // Halfway up: h10 * 16 * 1 + h01 * 16 = 0.125 * 16 + 0.5 * 16.
        CHECK(*easedValue(KeyframeEasing::LinearSmooth, track, 8.0) == 10.0);
        // And symmetrically on the way down.
        CHECK(*easedValue(KeyframeEasing::LinearSmooth, track, 24.0) == 10.0);
        // Speed is continuous through the peak: the slope just before and just
        // after it agree (both close to zero).
        const double before = *easedValue(KeyframeEasing::LinearSmooth, track, 15.999) - 16.0;
        const double after = 16.0 - *easedValue(KeyframeEasing::LinearSmooth, track, 16.001);
        CHECK(std::fabs(before) < 1e-4);
        CHECK(std::fabs(after) < 1e-4);
    }
}

TEST_CASE("the backwards keyframe search agrees with a brute-force scan", "[reframe][easing]") {
    // A forwards-only host, like Premiere's GetNextKeyframeTime, over a random
    // keyframe set; the search must find the same keyframe a linear scan does.
    std::mt19937_64 rng(20260923);
    std::uniform_int_distribution<std::int64_t> gap(1, 5000);
    std::vector<std::int64_t> keys;
    std::int64_t at = -3000;
    for (int i = 0; i < 300; ++i) {
        at += gap(rng);
        keys.push_back(at);
    }
    int calls = 0;
    const auto nextAfter = [&](std::int64_t t) -> std::optional<std::int64_t> {
        ++calls;
        auto it = std::upper_bound(keys.begin(), keys.end(), t);
        return it == keys.end() ? std::nullopt : std::optional<std::int64_t>(*it);
    };
    std::uniform_int_distribution<std::int64_t> when(-6000, at + 6000);
    int maxCalls = 0;
    for (int i = 0; i < 2000; ++i) {
        const std::int64_t t = (i < 300) ? keys[static_cast<std::size_t>(i)] : when(rng);
        std::optional<std::int64_t> expected;
        for (const std::int64_t k : keys) {
            if (k <= t) {
                expected = k;
            }
        }
        calls = 0;
        const std::optional<std::int64_t> got = keyAtOrBeforeFromNext(nextAfter, t, -1'000'000'000, 30, 4096);
        INFO("t " << t);
        CHECK(got == expected);
        maxCalls = std::max(maxCalls, calls);
    }
    // Logarithmic in the distance back, plus a short walk: far below a scan
    // of 300 keyframes.
    INFO("most host calls for one search: " << maxCalls);
    CHECK(maxCalls < 60);
}

TEST_CASE("the backwards keyframe search refuses a host that answers out of order or too long",
          "[reframe][easing]") {
    // A host that answers "the next keyframe after t" with t itself would loop
    // a naive walk forever.
    const auto stuck = [](std::int64_t t) -> std::optional<std::int64_t> { return t; };
    CHECK_FALSE(keyAtOrBeforeFromNext(stuck, 100, -1000, 1, 4096).has_value());
    // A call budget of one cannot finish any search that needs a walk.
    const auto dense = [](std::int64_t t) -> std::optional<std::int64_t> { return t + 1; };
    CHECK_FALSE(keyAtOrBeforeFromNext(dense, 100, -1000, 1, 1).has_value());
    // A throwing callback is contained.
    const auto throwing = [](std::int64_t) -> std::optional<std::int64_t> { throw std::runtime_error("host"); };
    CHECK_FALSE(keyAtOrBeforeFromNext(throwing, 100, -1000, 1, 4096).has_value());
    // No keyframe at or before t.
    const auto late = [](std::int64_t t) -> std::optional<std::int64_t> {
        return t < 500 ? std::optional<std::int64_t>(500) : std::nullopt;
    };
    CHECK_FALSE(keyAtOrBeforeFromNext(late, 100, -1000, 1, 4096).has_value());
    CHECK(keyAtOrBeforeFromNext(late, 600, -1000, 1, 4096) == std::optional<std::int64_t>(500));
}

// ===========================================================================
//  3. The popup
// ===========================================================================

TEST_CASE("Keyframe Easing is appended last as a None-by-default popup", "[reframe][easing][params]") {
    STATIC_REQUIRE(kIndexKeyframeEasing == OSV_REFRAME_PARAM_COUNT);
    STATIC_REQUIRE(kIndexKeyframeEasing == kIndexLens + 1);
    STATIC_REQUIRE(kParamIdByIndex[kIndexKeyframeEasing - 1] == OSV_REFRAME_ID_KEYFRAME_EASING);
    STATIC_REQUIRE(kValueParamAeIndex[kValueParamCount - 1] == kIndexKeyframeEasing);
    STATIC_REQUIRE(kValueParamKind[kValueParamCount - 1] == HostParamKind::Int32);
    STATIC_REQUIRE(kParamKindByIndex[kIndexKeyframeEasing - 1] == HostParamKind::Int32);
    // Every id before it is where it always was.
    STATIC_REQUIRE(OSV_REFRAME_ID_LENS == 21);
    STATIC_REQUIRE(kIndexLens == 21);

    // Through the loaded module's PF_Cmd_PARAMS_SETUP.
    REQUIRE(LoadedPlugin::instance().ok());
    MockHost host;
    PF_ProgPtr ref = host.createEffectRef(0x3000, 7);
    REQUIRE(ref != nullptr);
    PF_InData in = host.makeInData(ref, {});
    PF_OutData out = host.makeOutData();
    REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
            PF_Err_NONE);
    const std::vector<PF_ParamDef> params = host.addedParams(ref);
    REQUIRE(params.size() == static_cast<std::size_t>(kParamCount));
    const PF_ParamDef& def = params[kIndexKeyframeEasing - 1];
    CHECK(def.param_type == PF_Param_POPUP);
    CHECK(std::string(def.PF_DEF_NAME) == "Keyframe Easing");
    CHECK(def.uu.id == OSV_REFRAME_ID_KEYFRAME_EASING);
    CHECK(def.u.pd.num_choices == OSV_REFRAME_EASING_COUNT);
    CHECK(def.u.pd.dephault == OSV_REFRAME_EASING_DEFAULT);
    CHECK(def.u.pd.value == OSV_REFRAME_EASING_DEFAULT);
    CHECK((def.flags & PF_ParamFlag_CANNOT_TIME_VARY) != 0);
    REQUIRE(def.u.pd.u.namesptr != nullptr);
    CHECK(std::string(def.u.pd.u.namesptr) ==
          "None|Linear Smooth|Fast In, Slow Out|Slow In, Fast Out|Fast In, Fast Out|Slow In, Slow Out|Linear");
    CHECK(out.num_params == kParamCount + 1);
    host.destroyEffectRef(ref);
}

TEST_CASE("the parameter probe maps the Keyframe Easing popup, and a list without it reads None",
          "[reframe][easing][probe]") {
    // The compact layout (markers dropped, the collapsed Source group
    // missing), now eighteen controls long: the popup is the last host entry.
    std::vector<HostParamKind> compact;
    for (int i = 0; i < kValueParamCount; ++i) {
        const int ae = kValueParamAeIndex[i];
        if (ae == kIndexSourcePan || ae == kIndexSourceTilt || ae == kIndexSourceRoll) {
            continue;
        }
        compact.push_back(kValueParamKind[i]);
    }
    HostParamMap map{};
    REQUIRE(matchHostParams(compact.data(), static_cast<int>(compact.size()), &map));
    CHECK(map[kIndexKeyframeEasing] == static_cast<int>(compact.size()) - 1);
    CHECK(map[kIndexLens] == static_cast<int>(compact.size()) - 2);

    // A host whose list stops before the popup (every host until this build)
    // still maps everything else, and the popup to nothing - its default.
    std::vector<HostParamKind> older(compact.begin(), compact.end() - 1);
    HostParamMap olderMap{};
    REQUIRE(matchHostParams(older.data(), static_cast<int>(older.size()), &olderMap));
    CHECK(olderMap[kIndexKeyframeEasing] == -1);
    CHECK(olderMap[kIndexLens] == static_cast<int>(older.size()) - 1);
}

// ===========================================================================
//  4. The loaded module
// ===========================================================================

namespace {

/// Output resolution that frames the view on the frame itself.
constexpr Resolution kFillFrame = Resolution::MatchSequence;

/// The labelled panorama every render samples.
[[nodiscard]] const Panorama& panorama() {
    static const Panorama p = makePanorama(1024, 512);
    return p;
}

/// A PF_Cmd_RENDER harness around the loaded module: one effect instance on
/// the mock host, a panorama input and a 32f output, Classic lens at 90
/// degrees rectilinear so a pan is a clean horizontal shift.
struct CpuFixture {
    MockHost host;
    PF_ProgPtr ref = nullptr;
    std::unique_ptr<EffectWorld> input;
    std::unique_ptr<EffectWorld> output;
    int w = 0;
    int h = 0;

    CpuFixture(int width, int height) : w(width), h(height) {
        ref = host.createEffectRef(0x2200, 22);
        REQUIRE(ref != nullptr);
        PF_InData in = host.makeInData(ref, {});
        PF_OutData out = host.makeOutData();
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        const Panorama& p = panorama();
        input = host.createWorld(static_cast<std::uint32_t>(p.width), static_cast<std::uint32_t>(p.height),
                                 PrPixelFormat_BGRA_4444_32f);
        REQUIRE(input != nullptr);
        const std::vector<std::uint8_t> packed = packBgra32f(p, input->rowBytes());
        std::memcpy(input->pixels(), packed.data(), packed.size());
        host.setInputWorld(ref, input.get());
        output = host.createWorld(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                  PrPixelFormat_BGRA_4444_32f);
        REQUIRE(output != nullptr);
        // The Classic camera, 90 degrees, no distortion, framed on the frame.
        setPopup(kIndexLens, static_cast<int>(LensPopup::Classic));
        setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
        setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        setFloat(kIndexFov, 90.0);
        setFloat(kIndexDistortion, 0.0);
    }
    ~CpuFixture() {
        if (ref) {
            host.destroyEffectRef(ref);
        }
    }
    CpuFixture(const CpuFixture&) = delete;
    CpuFixture& operator=(const CpuFixture&) = delete;

    [[nodiscard]] PF_ParamDef defOf(int aeIndex) const {
        return host.addedParams(ref)[static_cast<std::size_t>(aeIndex) - 1u];
    }
    void setPopup(int aeIndex, int value) {
        PF_ParamDef def = defOf(aeIndex);
        def.u.pd.value = value;
        host.setParamValue(ref, aeIndex, def);
    }
    void setFloat(int aeIndex, double value) {
        PF_ParamDef def = defOf(aeIndex);
        def.u.fs_d.value = static_cast<PF_FpShort>(value);
        host.setParamValue(ref, aeIndex, def);
    }
    void setAngle(int aeIndex, double degrees) {
        PF_ParamDef def = defOf(aeIndex);
        def.u.ad.value = static_cast<PF_Fixed>(std::llround(degrees * 65536.0));
        host.setParamValue(ref, aeIndex, def);
    }
    /// An AE-side keyframe: PF_FindKeyframeTime reports it and checkout_param
    /// returns this value at exactly this time.
    void keyAngle(int aeIndex, A_long time, double degrees) {
        PF_ParamDef def = defOf(aeIndex);
        def.u.ad.value = static_cast<PF_Fixed>(std::llround(degrees * 65536.0));
        host.setParamValueAtTime(ref, aeIndex, time, def);
    }
    void keyFloat(int aeIndex, A_long time, double value) {
        PF_ParamDef def = defOf(aeIndex);
        def.u.fs_d.value = static_cast<PF_FpShort>(value);
        host.setParamValueAtTime(ref, aeIndex, time, def);
    }

    /// PF_Cmd_RENDER at `time` (one unit per frame, 30 units per second).
    [[nodiscard]] std::vector<std::uint8_t> render(A_long time) {
        InDataSpec spec;
        spec.currentTime = time;
        spec.timeStep = 1;
        spec.timeScale = 30;
        spec.totalTime = 300;
        PF_InData in = host.makeInData(ref, spec);
        PF_OutData out = host.makeOutData();
        std::vector<PF_ParamDef*> params = host.renderParams(ref);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_RENDER, &in, &out, params.data(), &output->world(),
                                                      nullptr) == PF_Err_NONE);
        const auto* base = reinterpret_cast<const std::uint8_t*>(output->pixels());
        return std::vector<std::uint8_t>(base, base + static_cast<std::size_t>(output->rowBytes()) *
                                                          static_cast<std::size_t>(h));
    }
};

/// The picture a constant pan renders: the reference every eased render is
/// compared against byte for byte.
[[nodiscard]] std::vector<std::uint8_t> constantPanRender(int w, int h, double pan) {
    CpuFixture f(w, h);
    f.setAngle(kIndexPan, pan);
    return f.render(10);
}

/// The pan keyframes every CPU case uses: 0 degrees at t = 0, 16 degrees at
/// t = 20.  The render at t = 10 is halfway, where every curve lands on a
/// multiple of 1/16 of the move.  The params array itself holds 40 degrees at
/// the render time - a value no curve can produce - so a render that shows
/// the eased value proves the easing replaced the host's value.
void keyPan(CpuFixture& f) {
    f.setAngle(kIndexPan, 40.0);
    f.keyAngle(kIndexPan, 0, 0.0);
    f.keyAngle(kIndexPan, 20, 16.0);
}

}  // namespace

TEST_CASE("the CPU path renders each preset's eased pan", "[reframe][easing][render]") {
    REQUIRE(LoadedPlugin::instance().ok());
    constexpr int kW = 161;
    constexpr int kH = 121;
    struct Case {
        KeyframeEasing easing;
        double expectedPan;
    };
    const Case cases[] = {
        {KeyframeEasing::Linear, 8.0},         {KeyframeEasing::SlowInSlowOut, 8.0},
        {KeyframeEasing::FastInFastOut, 8.0},  {KeyframeEasing::FastInSlowOut, 13.0},
        {KeyframeEasing::SlowInFastOut, 3.0},  {KeyframeEasing::LinearSmooth, 8.0},
    };
    for (const Case& c : cases) {
        INFO("easing " << static_cast<int>(c.easing) << ", expected pan " << c.expectedPan);
        // One MockHost at a time: the eased render's fixture is gone before
        // the reference's is built.
        std::vector<std::uint8_t> eased;
        {
            CpuFixture f(kW, kH);
            keyPan(f);
            f.setPopup(kIndexKeyframeEasing, static_cast<int>(c.easing));
            eased = f.render(10);
            CHECK(f.host.findKeyframeCalls(f.ref) > 0u);
        }
        const std::vector<std::uint8_t> reference = constantPanRender(kW, kH, c.expectedPan);
        REQUIRE(eased.size() == reference.size());
        CHECK(std::memcmp(eased.data(), reference.data(), eased.size()) == 0);
    }
}

TEST_CASE("the CPU path eases the selected lens's controls and the other angles", "[reframe][easing][render]") {
    REQUIRE(LoadedPlugin::instance().ok());
    constexpr int kW = 161;
    constexpr int kH = 121;
    // Classic FOV keyed 80 -> 96 and Tilt keyed 0 -> -16, Slow In / Fast Out:
    // halfway is 83 and -3.
    std::vector<std::uint8_t> eased;
    {
        CpuFixture f(kW, kH);
        f.setPopup(kIndexKeyframeEasing, static_cast<int>(KeyframeEasing::SlowInFastOut));
        f.setFloat(kIndexFov, 120.0);
        f.keyFloat(kIndexFov, 0, 80.0);
        f.keyFloat(kIndexFov, 20, 96.0);
        f.setAngle(kIndexTilt, 30.0);
        f.keyAngle(kIndexTilt, 0, 0.0);
        f.keyAngle(kIndexTilt, 20, -16.0);
        eased = f.render(10);
    }
    std::vector<std::uint8_t> reference;
    {
        CpuFixture f(kW, kH);
        f.setFloat(kIndexFov, 83.0);
        f.setAngle(kIndexTilt, -3.0);
        reference = f.render(10);
    }
    REQUIRE(eased.size() == reference.size());
    CHECK(std::memcmp(eased.data(), reference.data(), eased.size()) == 0);
}

TEST_CASE("None renders exactly what the effect rendered before the popup existed", "[reframe][easing][render]") {
    REQUIRE(LoadedPlugin::instance().ok());
    constexpr int kW = 161;
    constexpr int kH = 121;
    // Keyframes installed and the popup left at its default: the host's value
    // (40) is rendered, and the keyframe API is never asked anything.
    std::vector<std::uint8_t> none;
    {
        CpuFixture f(kW, kH);
        keyPan(f);
        none = f.render(10);
        CHECK(f.host.findKeyframeCalls(f.ref) == 0u);
    }
    const std::vector<std::uint8_t> reference = constantPanRender(kW, kH, 40.0);
    REQUIRE(none.size() == reference.size());
    CHECK(std::memcmp(none.data(), reference.data(), none.size()) == 0);
}

// ---- the GPU path -----------------------------------------------------------------

namespace {

constexpr csSDK_int32 kNode = 6022;
constexpr PrTimelineID kTimeline = 0x6022;
constexpr PrTime kFrame = osv::premiere::mock::kTicksPerSecond / 30;

/// PSNR of a GPU render against the CPU renderer's picture of `s`.
[[nodiscard]] double psnrAgainstCpu(const std::vector<float>& gpu, const Settings& s, int w, int h) {
    const Panorama& p = panorama();
    const std::vector<std::uint8_t> srcBytes = packBgra32f(p, p.width * 16);
    ConstFrameView src;
    src.base = srcBytes.data();
    src.rowBytes = p.width * 16;
    src.width = p.width;
    src.height = p.height;
    src.layout = PixelLayout::Bgra32f;
    src.topDown = true;
    const KernelSetup setup = buildParams(s, src, w, h, SizePx{});
    if (!setup.valid) {
        return 0.0;
    }
    std::vector<std::uint8_t> cpuBytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 16u, 0u);
    FrameView dst;
    dst.base = cpuBytes.data();
    dst.rowBytes = w * 16;
    dst.width = w;
    dst.height = h;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    if (!renderCpu(setup, src, dst, nullptr)) {
        return 0.0;
    }
    std::vector<float> cpu(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            readPixelBgra32f(cpuBytes.data(), dst.rowBytes, x, y,
                             cpu.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u);
        }
    }
    return psnr(gpu, cpu);
}

/// One Float32 keyframe of an angle control on the node.
void keyGpuAngle(MockHost& host, int aeIndex, PrTime time, double degrees) {
    PrParam p{};
    p.mType = kPrParamType_Float32;
    p.mFloat32 = static_cast<float>(degrees);
    host.setParam(kNode, gpuParamIndex(aeIndex), time, p);
}

/// Render the node at `clipTime` through a fresh filter instance; empty when
/// there is no device (the caller has already skipped) or a step failed.
[[nodiscard]] std::vector<float> renderGpu(MockHost& host, int w, int h, PrTime clipTime) {
    GpuEntryScope scope(host);
    if (scope.result() != suiteError_NoError) {
        return {};
    }
    GpuFrame in(host, panorama().width, panorama().height, false);
    GpuFrame out(host, w, h, false);
    if (!in.valid() || !out.valid() || !in.upload(packBgra32f(panorama(), in.rowBytes()))) {
        return {};
    }
    FilterInstance instance(scope, host, kNode, kTimeline);
    if (instance.created() != suiteError_NoError) {
        return {};
    }
    if (instance.render(in, out, clipTime, kFrame) != suiteError_NoError) {
        return {};
    }
    std::vector<float> image = out.downloadRgba();
    (void)instance.dispose();
    return image;
}

}  // namespace

TEST_CASE("the GPU path renders the CPU path's eased picture, on 1-based and 0-based hosts",
          "[reframe][easing][gpu][probe][cuda]") {
    constexpr int kW = 320;
    constexpr int kH = 180;
    struct Case {
        const char* host;
        bool zeroBased;
        KeyframeEasing easing;
        double expectedPan;
    };
    const Case cases[] = {
        {"1-based", false, KeyframeEasing::FastInSlowOut, 13.0},
        {"0-based", true, KeyframeEasing::FastInSlowOut, 13.0},
        {"1-based", false, KeyframeEasing::SlowInFastOut, 3.0},
        {"0-based", true, KeyframeEasing::SlowInSlowOut, 8.0},
    };
    for (const Case& c : cases) {
        INFO(c.host << " host, easing " << static_cast<int>(c.easing));
        MockHost host;
        if (!host.gpuAvailable()) {
            SKIP("no CUDA device: " << host.gpuFailureReason());
        }
        // Every popup in the host's own numbering: Premiere 26.2.2 counts
        // from 0 on the GPU side, After Effects (and the mock) from 1.
        const int base = c.zeroBased ? 0 : 1;
        Controls controls;
        controls.resolution = base + 0;  // Match Sequence
        controls.preset = base + 0;      // Custom
        controls.lens = base + 1;        // Classic
        controls.easing = base + static_cast<int>(c.easing) - 1;
        controls.fov = 90.0;
        controls.distortion = 0.0;
        writeVerbatimControls(host, kNode, controls);
        // Pan keyed 0 -> 16 over twenty frames (the mock itself interpolates
        // linearly, so without the easing the render would show 8).
        keyGpuAngle(host, kIndexPan, 0, 0.0);
        keyGpuAngle(host, kIndexPan, 20 * kFrame, 16.0);

        host.clearParamReads();
        const std::vector<float> gpu = renderGpu(host, kW, kH, 10 * kFrame);
        REQUIRE(!gpu.empty());
        // The keyframes were walked through GetNextKeyframeTime.
        CHECK(host.nextKeyframeCalls() > 0u);

        Settings expected = settingsOf(controls);
        CHECK(expected.easing == c.easing);  // the popup decoded in either numbering
        expected.panDeg = c.expectedPan;
        const double db = psnrAgainstCpu(gpu, expected, kW, kH);
        INFO("GPU vs CPU PSNR at the eased pan: " << db << " dB");
        CHECK(db >= 60.0);
        // And the eased picture is not the host's linear one.
        if (c.expectedPan != 8.0) {
            Settings linear = expected;
            linear.panDeg = 8.0;
            CHECK(psnrAgainstCpu(gpu, linear, kW, kH) < 40.0);
        }
    }
}

TEST_CASE("the GPU path's Linear Smooth walks the neighbouring keyframes", "[reframe][easing][gpu][cuda]") {
    constexpr int kW = 320;
    constexpr int kH = 180;
    MockHost host;
    if (!host.gpuAvailable()) {
        SKIP("no CUDA device: " << host.gpuFailureReason());
    }
    Controls controls;
    controls.easing = static_cast<int>(KeyframeEasing::LinearSmooth);
    writeVerbatimControls(host, kNode, controls);
    // Up 16 degrees over 16 frames, then back down: at frame 8 the spline is
    // at 10 (ReframeEasing.h: the corner's tangent is the average slope, 0).
    keyGpuAngle(host, kIndexPan, 0, 0.0);
    keyGpuAngle(host, kIndexPan, 16 * kFrame, 16.0);
    keyGpuAngle(host, kIndexPan, 32 * kFrame, 0.0);
    const std::vector<float> gpu = renderGpu(host, kW, kH, 8 * kFrame);
    REQUIRE(!gpu.empty());
    Settings expected = settingsOf(controls);
    expected.panDeg = 10.0;
    const double db = psnrAgainstCpu(gpu, expected, kW, kH);
    INFO("GPU vs CPU PSNR: " << db << " dB");
    CHECK(db >= 60.0);
}

TEST_CASE("easing None never walks keyframes on the GPU path", "[reframe][easing][gpu][cuda]") {
    constexpr int kW = 160;
    constexpr int kH = 90;
    MockHost host;
    if (!host.gpuAvailable()) {
        SKIP("no CUDA device: " << host.gpuFailureReason());
    }
    Controls controls;
    controls.easing = static_cast<int>(KeyframeEasing::None);
    writeVerbatimControls(host, kNode, controls);
    keyGpuAngle(host, kIndexPan, 0, 0.0);
    keyGpuAngle(host, kIndexPan, 20 * kFrame, 16.0);
    host.clearParamReads();
    const std::vector<float> gpu = renderGpu(host, kW, kH, 10 * kFrame);
    REQUIRE(!gpu.empty());
    CHECK(host.nextKeyframeCalls() == 0u);
    // The host's own (linear) value: 8 degrees.
    Settings expected = settingsOf(controls);
    expected.panDeg = 8.0;
    CHECK(psnrAgainstCpu(gpu, expected, kW, kH) >= 60.0);
}
