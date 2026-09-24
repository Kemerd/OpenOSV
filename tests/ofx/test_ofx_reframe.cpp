// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_reframe.cpp - OpenOSV 360 Reframe as an OpenFX filter, on the CPU:
// the picture is the Premiere effect's picture, the right way up, with the
// supervised controls behaving exactly as they do in Premiere.

#include "OfxTestSupport.h"

#include "OfxCamera.h"

#include "ReframeCpu.h"
#include "ReframeEasing.h"
#include "ReframeParams.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <optional>

using namespace osv::ofxtest;
namespace cam = osv::ofx::camera;
namespace rf = osv::reframe;

namespace {

constexpr int kProjectW = 480;
constexpr int kProjectH = 270;

/// One reframe instance with a panorama on its Source and a blank Output.
struct ReframeRig {
    std::unique_ptr<Effect> effect;
    HostImage source;
    HostImage output;
    OfxRectI frame{0, 0, kProjectW, kProjectH};

    explicit ReframeRig(bool negativeSource = false, int padSource = 0, bool negativeOutput = false) {
        Fixture& f = Fixture::get();
        OfxStatus st = kOfxStatFailed;
        effect = f.reframe.createInstance(kOfxImageEffectContextFilter, kProjectW, kProjectH, 25.0, &st);
        REQUIRE(st == kOfxStatOK);
        source = makeImage(OfxRectI{0, 0, 512, 256}, negativeSource, padSource);
        paintPanorama(source);
        output = makeImage(frame, negativeOutput, 0);
        output.fill(-7.0f);  // a sentinel no render produces
        Clip* src = effect->clip(kOfxImageEffectSimpleSourceClipName);
        Clip* out = effect->clip(kOfxImageEffectOutputClipName);
        REQUIRE(src);
        REQUIRE(out);
        src->rod = OfxRectD{0, 0, 512, 256};
        provideImage(*src, source);
        provideImage(*out, output);
    }

    Param& param(const char* name) {
        Param* p = effect->params.find(name);
        REQUIRE(p);
        return *p;
    }

    OfxStatus render(double time = 0.0) { return render(frame, time); }
    OfxStatus render(const OfxRectI& window, double time = 0.0) {
        PluginHarness::RenderArgs args;
        args.time = time;
        args.window = window;
        return Fixture::get().reframe.render(*effect, args);
    }
};

/// A KeyframeTrack over a map of keys, for the reference easing.
class MapTrack final : public rf::KeyframeTrack {
public:
    explicit MapTrack(const std::map<double, double>& keys) : m_keys(keys) {}
    std::optional<double> keyAtOrBefore(double t) override {
        std::optional<double> best;
        for (const auto& k : m_keys) {
            if (k.first <= t + 1e-9) best = k.first;
        }
        return best;
    }
    std::optional<double> keyBefore(double t) override {
        std::optional<double> best;
        for (const auto& k : m_keys) {
            if (k.first < t - 1e-9) best = k.first;
        }
        return best;
    }
    std::optional<double> keyAfter(double t) override {
        for (const auto& k : m_keys) {
            if (k.first > t + 1e-9) return k.first;
        }
        return std::nullopt;
    }
    std::optional<double> valueAt(double t) override {
        auto it = m_keys.find(t);
        return it == m_keys.end() ? std::nullopt : std::optional<double>(it->second);
    }

private:
    std::map<double, double> m_keys;
};

}  // namespace

// ===========================================================================
//  The picture
// ===========================================================================

TEST_CASE("a fresh instance renders the Premiere effect's default view", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    REQUIRE(rig.render() == kOfxStatOK);
    const HostImage ref = referenceRender(defaultSettings(), rig.source, rig.frame, {kProjectW, kProjectH});
    // Same per-pixel function, same inputs: bit-identical is the expectation.
    CHECK(maxDifference(rig.output, ref, rig.frame) == 0.0);
    CHECK(MockHost::instance().imagesOut == 0);  // every image released
}

TEST_CASE("the render matches the Premiere effect for a moved DJI camera", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    rig.param(cam::kPan).d = 37.5;
    rig.param(cam::kTilt).d = -12.0;
    rig.param(cam::kRoll).d = 8.0;
    rig.param(cam::kDjiFov).d = 72.0;
    rig.param(cam::kCorrection).d = 0.85;
    rig.param(cam::kSourceTilt).d = 3.0;
    REQUIRE(rig.render() == kOfxStatOK);

    rf::Settings s = defaultSettings();
    s.panDeg = 37.5;
    s.tiltDeg = -12.0;
    s.rollDeg = 8.0;
    s.djiFovDeg = 72.0;
    s.correction = 0.85;
    s.sourceTiltDeg = 3.0;
    const HostImage ref = referenceRender(s, rig.source, rig.frame, {kProjectW, kProjectH});
    CHECK(maxDifference(rig.output, ref, rig.frame) == 0.0);
}

TEST_CASE("the render matches the Premiere effect for the Classic lens", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    rig.param(cam::kLens).i = rf::lensPopupValue(rf::CameraModel::Classic) - 1;
    rig.param(cam::kFov).d = 160.0;
    rig.param(cam::kDistortion).d = 30.0;
    rig.param(cam::kPan).d = -120.0;
    REQUIRE(rig.render() == kOfxStatOK);

    rf::Settings s = defaultSettings();
    s.cameraModel = rf::CameraModel::Classic;
    s.fovDeg = 160.0;
    s.distortion = 30.0;
    s.panDeg = -120.0;
    const HostImage ref = referenceRender(s, rig.source, rig.frame, {kProjectW, kProjectH});
    CHECK(maxDifference(rig.output, ref, rig.frame) == 0.0);
}

TEST_CASE("any row order and pitch the host uses gives the same picture", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig plain;
    plain.param(cam::kPan).d = 21.0;
    REQUIRE(plain.render() == kOfxStatOK);

    // Top-down source memory (negative pitch) with padded rows, and a
    // top-down output: all legal in OpenFX, all the same picture.
    ReframeRig odd(/*negativeSource=*/true, /*padSource=*/12, /*negativeOutput=*/true);
    odd.param(cam::kPan).d = 21.0;
    REQUIRE(odd.render() == kOfxStatOK);
    CHECK(maxDifference(plain.output, odd.output, plain.frame) == 0.0);
}

TEST_CASE("up is up: the sky of the panorama is the top of the view", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    // Sky red, ground green, by latitude - painted in OpenFX coordinates,
    // where y grows UP.
    for (int y = 0; y < rig.source.height(); ++y) {
        const bool sky = y >= rig.source.height() / 2;
        for (int x = 0; x < rig.source.width(); ++x) {
            float* p = rig.source.pixel(x, y);
            p[0] = sky ? 1.0f : 0.0f;
            p[1] = sky ? 0.0f : 1.0f;
            p[2] = 0.0f;
            p[3] = 1.0f;
        }
    }
    REQUIRE(rig.render() == kOfxStatOK);
    // Level camera: the TOP row of the frame (y = H - 1 in OpenFX) sees sky,
    // the bottom row (y = 0) sees ground.
    const float* top = rig.output.pixel(kProjectW / 2, kProjectH - 1);
    const float* bottom = rig.output.pixel(kProjectW / 2, 0);
    REQUIRE(top);
    REQUIRE(bottom);
    CHECK(top[0] == Catch::Approx(1.0f));
    CHECK(top[1] == Catch::Approx(0.0f));
    CHECK(bottom[0] == Catch::Approx(0.0f));
    CHECK(bottom[1] == Catch::Approx(1.0f));

    // Tilt up 60 degrees: the whole middle of the view is sky.
    rig.param(cam::kTilt).d = 60.0;
    REQUIRE(rig.render() == kOfxStatOK);
    const float* centre = rig.output.pixel(kProjectW / 2, kProjectH / 2);
    REQUIRE(centre);
    CHECK(centre[0] == Catch::Approx(1.0f));
}

TEST_CASE("a render window renders only inside itself", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    rig.param(cam::kPan).d = 15.0;
    const OfxRectI window{100, 40, 300, 200};
    REQUIRE(rig.render(window) == kOfxStatOK);

    rf::Settings s = defaultSettings();
    s.panDeg = 15.0;
    const HostImage ref = referenceRender(s, rig.source, rig.frame, {kProjectW, kProjectH});
    CHECK(maxDifference(rig.output, ref, window) == 0.0);
    // Outside the window the sentinel survived.
    CHECK(rig.output.pixel(10, 10)[0] == -7.0f);
    CHECK(rig.output.pixel(kProjectW - 1, kProjectH - 1)[3] == -7.0f);
}

TEST_CASE("the filter asks for the whole source whatever it renders", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    OfxRectD roi{0, 0, 0, 0};
    REQUIRE(Fixture::get().reframe.regionsOfInterest(*rig.effect, 0.0, OfxRectD{10, 10, 20, 20},
                                                     kOfxImageEffectSimpleSourceClipName, roi) == kOfxStatOK);
    CHECK(roi.x1 == 0.0);
    CHECK(roi.y1 == 0.0);
    CHECK(roi.x2 == 512.0);
    CHECK(roi.y2 == 256.0);
}

TEST_CASE("a source without a picture renders transparent black", "[ofx][reframe]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    rig.effect->clip(kOfxImageEffectSimpleSourceClipName)->provide = [](double, PropertySet&) { return false; };
    REQUIRE(rig.render() == kOfxStatOK);
    for (int c = 0; c < 4; ++c) {
        CHECK(rig.output.pixel(5, 5)[c] == 0.0f);
    }
    CHECK(MockHost::instance().imagesOut == 0);
}

// ===========================================================================
//  The supervised controls (EffectMain.cpp userChangedParam, in OpenFX)
// ===========================================================================

TEST_CASE("one lens's controls are shown, starting with DJI", "[ofx][reframe][supervision]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    CHECK(rig.param(cam::kDjiFov).props.getInt(kOfxParamPropSecret) == 0);
    CHECK(rig.param(cam::kCorrection).props.getInt(kOfxParamPropSecret) == 0);
    CHECK(rig.param(cam::kZoom).props.getInt(kOfxParamPropSecret) == 0);
    CHECK(rig.param(cam::kFov).props.getInt(kOfxParamPropSecret) == 1);
    CHECK(rig.param(cam::kDistortion).props.getInt(kOfxParamPropSecret) == 1);
    CHECK(rig.param(cam::kFov).props.getInt(kOfxParamPropEnabled) == 0);
    CHECK(rig.param(cam::kLensMirror).props.getInt(kOfxParamPropSecret) == 1);
}

TEST_CASE("picking a preset writes DJI's look and selects the DJI lens", "[ofx][reframe][supervision]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    // Start on Classic, to see the preset switch back to DJI.
    rig.param(cam::kLens).i = 1;
    rig.param(cam::kLensMirror).i = 0;
    rig.param(cam::kPreset).i = static_cast<int>(rf::Preset::Asteroid) - 1;
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kPreset, kOfxChangeUserEdited, 0.0) ==
            kOfxStatOK);

    const rf::PresetEntry* asteroid = rf::presetEntry(rf::Preset::Asteroid);
    const double aspect = double(kProjectW) / double(kProjectH);
    CHECK(rig.param(cam::kTilt).d == asteroid->tiltDeg);
    CHECK(rig.param(cam::kFov).d == asteroid->fovDeg);
    CHECK(rig.param(cam::kDistortion).d == asteroid->distortion);
    CHECK(rig.param(cam::kDjiFov).d == Catch::Approx(rf::djiPresetFovDeg(*asteroid, aspect)));
    CHECK(rig.param(cam::kCorrection).d == Catch::Approx(asteroid->correction));
    CHECK(rig.param(cam::kLens).i == 0);        // DJI
    CHECK(rig.param(cam::kLensMirror).i == 1);  // mirror in step
    CHECK(rig.param(cam::kZoom).d ==
          Catch::Approx(rf::djiZoomDeg(rf::DjiLens{rig.param(cam::kDjiFov).d, rig.param(cam::kCorrection).d}, aspect)));
    CHECK(rig.effect->params.editDepth == 0);  // every edit group closed
    CHECK(rig.effect->params.editGroups >= 1);
    // The DJI controls are on screen again.
    CHECK(rig.param(cam::kDjiFov).props.getInt(kOfxParamPropSecret) == 0);
}

TEST_CASE("editing a Classic control selects Classic and flips Preset to Custom", "[ofx][reframe][supervision]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    rig.param(cam::kFov).d = 150.0;
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kFov, kOfxChangeUserEdited, 0.0) == kOfxStatOK);
    CHECK(rig.param(cam::kLens).i == 1);  // Classic
    CHECK(rig.param(cam::kLensMirror).i == 0);
    CHECK(rig.param(cam::kPreset).i == static_cast<int>(rf::Preset::Custom) - 1);
    CHECK(rig.param(cam::kFov).props.getInt(kOfxParamPropSecret) == 0);
    CHECK(rig.param(cam::kDjiFov).props.getInt(kOfxParamPropSecret) == 1);
}

TEST_CASE("switching lens carries the look across", "[ofx][reframe][supervision]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    const double aspect = double(kProjectW) / double(kProjectH);
    rig.param(cam::kDjiFov).d = 80.0;
    rig.param(cam::kCorrection).d = 0.4;
    rig.param(cam::kLens).i = 1;  // the user picks Classic; the mirror still says DJI
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kLens, kOfxChangeUserEdited, 0.0) ==
            kOfxStatOK);
    const rf::ClassicLens expected = rf::classicFromDji(rf::DjiLens{80.0, 0.4}, aspect);
    CHECK(rig.param(cam::kFov).d == Catch::Approx(expected.fovDeg));
    CHECK(rig.param(cam::kDistortion).d == Catch::Approx(expected.distortion));
    CHECK(rig.param(cam::kLensMirror).i == 0);

    // Picking Classic again is a re-pick: nothing converts.
    const int writesBefore = rig.param(cam::kFov).pluginWrites;
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kLens, kOfxChangeUserEdited, 0.0) ==
            kOfxStatOK);
    CHECK(rig.param(cam::kFov).pluginWrites == writesBefore);
}

TEST_CASE("typing a Zoom moves FOV and Correction along DJI Studio's path", "[ofx][reframe][supervision]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    const double aspect = double(kProjectW) / double(kProjectH);
    rig.param(cam::kZoom).d = 100.0;
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kZoom, kOfxChangeUserEdited, 0.0) ==
            kOfxStatOK);
    const rf::DjiLens to = rf::djiZoomTo(
        100.0, rf::DjiLens{OSV_REFRAME_DJI_FOV_DEFAULT, OSV_REFRAME_CORRECTION_DEFAULT}, aspect);
    CHECK(rig.param(cam::kDjiFov).d == Catch::Approx(to.fovDeg));
    CHECK(rig.param(cam::kCorrection).d == Catch::Approx(to.correction));
    CHECK(rig.param(cam::kZoom).d == Catch::Approx(rf::djiZoomDeg(to, aspect)));
}

TEST_CASE("the plug-in's own writes and time changes are not supervised", "[ofx][reframe][supervision]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    rig.param(cam::kPreset).i = static_cast<int>(rf::Preset::CrystalBall) - 1;
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kPreset, kOfxChangePluginEdited, 0.0) ==
            kOfxStatReplyDefault);
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kPreset, kOfxChangeTime, 0.0) ==
            kOfxStatReplyDefault);
    CHECK(rig.param(cam::kTilt).pluginWrites == 0);
    CHECK(rig.param(cam::kDjiFov).pluginWrites == 0);
}

TEST_CASE("a preset on a keyframed control lands as a key where the user stands", "[ofx][reframe][supervision]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    Param& tilt = rig.param(cam::kTilt);
    tilt.dkeys[0.0] = 5.0;
    tilt.dkeys[20.0] = -5.0;
    rig.param(cam::kPreset).i = static_cast<int>(rf::Preset::Asteroid) - 1;
    REQUIRE(Fixture::get().reframe.instanceChanged(*rig.effect, cam::kPreset, kOfxChangeUserEdited, 12.0) ==
            kOfxStatOK);
    REQUIRE(tilt.dkeys.count(12.0) == 1);
    CHECK(tilt.dkeys[12.0] == rf::presetEntry(rf::Preset::Asteroid)->tiltDeg);
    CHECK(tilt.dkeys.size() == 3);  // the user's two keys are untouched
}

// ===========================================================================
//  Keyframe Easing and Smooth Keyframes
// ===========================================================================

TEST_CASE("Keyframe Easing moves the camera along DJI Studio's curve", "[ofx][reframe][easing]") {
    REQUIRE(Fixture::get().ready);
    const std::map<double, double> panKeys = {{0.0, 0.0}, {20.0, 90.0}};
    const rf::KeyframeEasing easing = rf::KeyframeEasing::SlowInSlowOut;

    ReframeRig rig;
    rig.param(cam::kPan).dkeys = panKeys;
    rig.param(cam::kKeyframeEasing).i = static_cast<int>(easing) - 1;
    REQUIRE(rig.render(6.0) == kOfxStatOK);

    MapTrack track(panKeys);
    const std::optional<double> pan = rf::easedValue(easing, track, 6.0);
    REQUIRE(pan);
    // The curve really is not the host's straight line at this time.
    CHECK(std::fabs(*pan - 27.0) > 1.0);
    rf::Settings s = defaultSettings();
    s.panDeg = *pan;
    s.easing = easing;
    const HostImage ref = referenceRender(s, rig.source, rig.frame, {kProjectW, kProjectH});
    CHECK(maxDifference(rig.output, ref, rig.frame) == 0.0);
}

TEST_CASE("Smooth Keyframes averages each angle over three frames", "[ofx][reframe][easing]") {
    REQUIRE(Fixture::get().ready);
    ReframeRig rig;
    // A kink at frame 5: the host's own interpolation gives 40, 50, 50 at
    // frames 4, 5, 6.
    rig.param(cam::kPan).dkeys = {{0.0, 0.0}, {5.0, 50.0}, {10.0, 50.0}};
    rig.param(cam::kSmoothKeyframes).i = 1;
    REQUIRE(rig.render(5.0) == kOfxStatOK);

    // The host's own values at the three frames, summed in the plug-in's
    // order, so the reference angle is the very same double.
    const Param& pan = rig.param(cam::kPan);
    rf::Settings s = defaultSettings();
    s.panDeg = (pan.doubleAt(4.0) + pan.doubleAt(5.0) + pan.doubleAt(6.0)) / 3.0;
    CHECK(s.panDeg == Catch::Approx((40.0 + 50.0 + 50.0) / 3.0));
    s.smoothKeyframes = true;
    const HostImage ref = referenceRender(s, rig.source, rig.frame, {kProjectW, kProjectH});
    CHECK(maxDifference(rig.output, ref, rig.frame) == 0.0);
}
