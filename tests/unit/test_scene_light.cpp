// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Scene Light (osv/render/SceneLight.h): the metered light from exposure
// metadata, the zenith cap statistics, the cap measured through the real
// kernel on a synthetic scene, and the Day / Night decision on the numbers
// measured on a user's car-mounted 8K clips and the 6K aerial sample.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "SynthFisheye.h"
#include "TestSample.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Math.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/Blend.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"
#include "osv/render/LensShading.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/SceneLight.h"
#include "osv/video/PlanarFrame.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace osv;
using Catch::Matchers::WithinAbs;

namespace {

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

/// One frame's exposure metadata: the camera's LV (0 = not recorded), ISO
/// and a 1 / `shutterDen` s shutter.
[[nodiscard]] meta::CameraFrame cameraFrame(float lv, float iso, std::int32_t shutterDen) {
    meta::CameraFrame c;
    c.aecLv = lv;
    c.iso = iso;
    c.exposureTime = {1, shutterDen};
    return c;
}

/// `n` frames whose LV runs linearly from `lo` to `hi`.
[[nodiscard]] std::vector<meta::CameraFrame> lvRamp(float lo, float hi, int n) {
    std::vector<meta::CameraFrame> frames;
    for (int i = 0; i < n; ++i) {
        const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        frames.push_back(cameraFrame(lo + (hi - lo) * t, 100.0f, 100));
    }
    return frames;
}

/// A metered light with the given median (the camera's LV unless `ev`).
[[nodiscard]] render::MeteredLight metered(double median, bool ev = false) {
    render::MeteredLight m;
    m.valid = true;
    m.fromAecLv = !ev;
    m.median = median;
    m.p5 = median - 1.0;
    m.p95 = median + 1.0;
    m.frames = 256;
    return m;
}

/// A valid cap `stops` against grey with colour ratio `bg` (log2 B / G).
[[nodiscard]] render::SkyCap cap(double stops, double bg = 0.0) {
    render::SkyCap c;
    c.valid = true;
    c.stopsVsGrey = stops;
    c.log2BG = bg;
    c.usableFraction = 0.9;
    c.flatFraction = 0.8;
    c.frames = 3;
    return c;
}

/// A deterministic hash noise in [-1, 1] (no RNG state: the same pixel always
/// gets the same value).
[[nodiscard]] double hashNoise(std::uint32_t x, std::uint32_t y) {
    std::uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    h ^= h >> 16u;
    return static_cast<double>(h & 0xFFFFu) / 32767.5 - 1.0;
}

/// A levelled equirect strip (top `rows` rows of a `w` x `w / 2` map) of a
/// uniform neutral sky of luminance `skyY`, with pixel noise of +-`noise`
/// stops; `paint(x, y, lat, rgba)` may overwrite any pixel.
template <class Paint>
[[nodiscard]] std::vector<float> strip(std::uint32_t w, std::uint32_t rows, double skyY, double noise, Paint paint) {
    std::vector<float> rgba(static_cast<std::size_t>(w) * rows * 4u);
    const double degPerRow = 180.0 / static_cast<double>(w / 2u);
    for (std::uint32_t y = 0; y < rows; ++y) {
        const double lat = 90.0 - (static_cast<double>(y) + 0.5) * degPerRow;
        for (std::uint32_t x = 0; x < w; ++x) {
            float* p = rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4u;
            const double v = skyY * std::exp2(noise * hashNoise(x, y));
            p[0] = p[1] = p[2] = static_cast<float>(v);
            p[3] = 1.0f;
            paint(x, y, lat, p);
        }
    }
    return rgba;
}

constexpr std::uint32_t kStripW = 1024;
constexpr std::uint32_t kStripRows = 160;  // the cap (128 rows) plus the 8 deg margin

}  // namespace

// ===========================================================================
//  Metered light
// ===========================================================================

TEST_CASE("EV100 follows the exposure triangle and refuses missing fields", "[scenelight]") {
    // The night driving clip: ISO 4140, 1/100 s, f/1.9 -> EV100 3.12, inside
    // the measured 2.07-4.41.
    const double night = render::ev100Of(cameraFrame(0.0f, 4140.0f, 100), 1.9);
    CHECK_THAT(night, WithinAbs(std::log2(1.9 * 1.9 * 100.0) - std::log2(41.4), 1e-9));
    CHECK(night > 2.07);
    CHECK(night < 4.41);
    // The aerial sample: ISO 142, 1/208 s -> 9.05 (measured 9.05).
    CHECK_THAT(render::ev100Of(cameraFrame(0.0f, 142.0f, 208), 1.9), WithinAbs(9.05, 0.01));
    // Missing or garbage inputs: NaN, never a number that looks like light.
    CHECK(std::isnan(render::ev100Of(cameraFrame(0.0f, 0.0f, 100), 1.9)));
    CHECK(std::isnan(render::ev100Of(cameraFrame(0.0f, 100.0f, 0), 1.9)));
    CHECK(std::isnan(render::ev100Of(cameraFrame(0.0f, 100.0f, 100), 0.0)));
    CHECK(std::isnan(render::ev100Of(cameraFrame(0.0f, std::nanf(""), 100), 1.9)));
    meta::CameraFrame noShutter = cameraFrame(0.0f, 100.0f, 100);
    noShutter.exposureTime.clear();
    CHECK(std::isnan(render::ev100Of(noShutter, 1.9)));
}

TEST_CASE("the f-number comes from ClipMeta, else f/1.9", "[scenelight]") {
    meta::ClipMeta clip;
    CHECK(render::fNumberOf(clip) == render::kDefaultFNumber);
    clip.fNumber = {19, 10};
    CHECK_THAT(render::fNumberOf(clip), WithinAbs(1.9, 1e-12));
    clip.fNumber = {28, 10};
    CHECK_THAT(render::fNumberOf(clip), WithinAbs(2.8, 1e-12));
    clip.fNumber = {19, 0};  // a damaged rational
    CHECK(render::fNumberOf(clip) == render::kDefaultFNumber);
    clip.fNumber = {5000, 1};  // no lens has that
    CHECK(render::fNumberOf(clip) == render::kDefaultFNumber);
}

TEST_CASE("the metered light is the camera's LV median, EV100 when most frames lack it", "[scenelight]") {
    SECTION("the night clip's LV spread: p5 2.67, median 3.72, p95 4.82") {
        // A linear ramp whose 5th / 50th / 95th percentiles are those numbers.
        const auto frames = lvRamp(2.67f - 0.06f * (4.82f - 2.67f) / 0.9f, 4.82f + 0.06f * (4.82f - 2.67f) / 0.9f, 201);
        const render::MeteredLight m = render::meteredLightOf(frames, 1.9);
        REQUIRE(m.valid);
        CHECK(m.fromAecLv);
        CHECK(m.frames == 201u);
        CHECK_THAT(m.median, WithinAbs(3.745, 0.05));
        CHECK(m.p5 < m.median);
        CHECK(m.p95 > m.median);
        CHECK(render::meteredLightSaysDark(m));
    }
    SECTION("the day clip (13.11 / 13.30 / 13.55) and the aerial sample (9.89) are not dark") {
        CHECK_FALSE(render::meteredLightSaysDark(render::meteredLightOf(lvRamp(13.11f, 13.55f, 64), 1.9)));
        CHECK_FALSE(render::meteredLightSaysDark(render::meteredLightOf(lvRamp(9.88f, 9.89f, 64), 1.9)));
    }
    SECTION("no LV on most frames: EV100 from ISO / shutter / aperture") {
        std::vector<meta::CameraFrame> frames(10, cameraFrame(0.0f, 4140.0f, 100));
        frames[0].aecLv = 12.0f;  // one stray LV must not decide
        const render::MeteredLight m = render::meteredLightOf(frames, 1.9);
        REQUIRE(m.valid);
        CHECK_FALSE(m.fromAecLv);
        CHECK_THAT(m.median, WithinAbs(render::ev100Of(frames[1], 1.9), 1e-9));
        CHECK(render::meteredLightSaysDark(m));
    }
    SECTION("nothing usable: invalid, which is never dark") {
        CHECK_FALSE(render::meteredLightOf(std::vector<meta::CameraFrame>{}, 1.9).valid);
        const std::vector<meta::CameraFrame> blank(5, cameraFrame(0.0f, 0.0f, 0));
        const render::MeteredLight m = render::meteredLightOf(blank, 1.9);
        CHECK_FALSE(m.valid);
        CHECK_FALSE(render::meteredLightSaysDark(m));
    }
    SECTION("an empty metadata track has no reading") {
        const meta::MetadataTrack empty;
        CHECK_FALSE(render::meteredLightOf(empty, 1000).valid);
    }
}

TEST_CASE("the metered light of a one-frame clip reads that one frame", "[scenelight][sample]") {
    // n = min(frameCount, track samples) == 1 is the case a std::clamp(.., 2, n)
    // would get wrong (lo > hi); the sample clip's own track stands in for a
    // one-frame clip by asking for its first frame only.
    OSV_REQUIRE_SAMPLE();
    // The track keeps a pointer to its file, so both live on the heap together.
    struct Sample {
        OsvFile file;
        meta::MetadataTrack track;
    };
    auto s = std::make_unique<Sample>();
    auto file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    s->file = std::move(file).value();
    auto track = meta::MetadataTrack::load(s->file);
    REQUIRE(track.ok());
    s->track = std::move(track).value();
    REQUIRE(s->track.frameCount() > 1u);

    // ---- one frame of video: exactly frame 0, whatever maxSamples says -----------
    for (const std::uint32_t maxSamples : {0u, 1u, 2u, 256u}) {
        INFO("maxSamples " << maxSamples);
        const render::MeteredLight one = render::meteredLightOf(s->track, 1u, maxSamples);
        REQUIRE(one.valid);
        CHECK(one.frames == 1u);
        CHECK(one.fromAecLv);
        // The aerial sample meters LV 9.88-9.89 on every frame.
        CHECK_THAT(one.median, WithinAbs(9.89, 0.05));
        CHECK(one.p5 == one.median);
        CHECK(one.p95 == one.median);
    }

    // ---- two frames: the first and the last, never a frame past the end --------------
    const render::MeteredLight two = render::meteredLightOf(s->track, 2u, 1u);
    REQUIRE(two.valid);
    CHECK(two.frames == 2u);
    CHECK_THAT(two.median, WithinAbs(9.89, 0.05));

    // ---- the whole clip agrees ---------------------------------------------------------
    const render::MeteredLight all = render::meteredLightOf(s->track, s->track.frameCount());
    REQUIRE(all.valid);
    CHECK(all.frames == s->track.frameCount());
    CHECK_FALSE(render::meteredLightSaysDark(all));
}

// ===========================================================================
//  Classification on the measured numbers
// ===========================================================================

TEST_CASE("the night driving clip is Night: dark meter AND dark sky", "[scenelight]") {
    // LV median 3.72; the sky cap measured -1.87 .. -3.19 stops on the LRF
    // and -2.56 / -2.63 on the 8K OSV.
    for (const double stops : {-1.87, -2.22, -2.48, -3.18, -3.19, -2.56, -2.63}) {
        INFO("cap " << stops);
        const render::SceneLightVerdict v = render::classifySceneLight(metered(3.72), cap(stops, -0.15));
        CHECK(v.light == render::SceneLight::Night);
        CHECK(v.needsCap);
        CHECK_FALSE(v.reason.empty());
    }
    // The EV100 fallback says the same (night 2.07-4.41, median 3.28).
    CHECK(render::classifySceneLight(metered(3.28, true), cap(-2.5)).light == render::SceneLight::Night);
    // The evidence line the Properties panel shows.
    const render::SceneLightVerdict v = render::classifySceneLight(metered(3.72), cap(-3.2));
    CHECK(render::sceneLightEvidence(v) == "LV 3.7, sky -3.2 stops");
}

TEST_CASE("day clips and the ND-filtered aerial sample are Day", "[scenelight]") {
    // A user's day clip: LV 13.30 (EV100 11.1-11.66); its cap (+1.7 .. +2.1)
    // is never even needed.
    render::SceneLightVerdict day = render::classifySceneLight(metered(13.30), std::nullopt);
    CHECK(day.light == render::SceneLight::Day);
    CHECK_FALSE(day.needsCap);
    CHECK(render::classifySceneLight(metered(11.11, true), std::nullopt).light == render::SceneLight::Day);
    // A bright meter wins whatever the cap says.
    CHECK(render::classifySceneLight(metered(13.30), cap(-3.0)).light == render::SceneLight::Day);
    // The 6K aerial sample: LV 9.89 / EV100 9.05.
    CHECK(render::classifySceneLight(metered(9.89), std::nullopt).light == render::SceneLight::Day);
    CHECK(render::classifySceneLight(metered(9.05, true), std::nullopt).light == render::SceneLight::Day);
    // The ND guard: a meter reading dark through a heavy ND filter, under the
    // sample's deep blue sky (-0.74 stops, B/G +1.38) - daylight.
    const render::SceneLightVerdict nd = render::classifySceneLight(metered(4.0), cap(-0.74, 1.38));
    CHECK(nd.light == render::SceneLight::Day);
    CHECK(nd.needsCap);
}

TEST_CASE("everything ambiguous stays on the day profile", "[scenelight]") {
    // The middle of the meter (6 <= LV < 8): Day, the profile every clip had.
    CHECK(render::classifySceneLight(metered(7.0), cap(-3.0)).light == render::SceneLight::Day);
    CHECK(render::classifySceneLight(metered(6.0), cap(-3.0)).light == render::SceneLight::Day);
    // A dark meter, but a sky only a little below grey: Day.
    CHECK(render::classifySceneLight(metered(3.7), cap(-1.2)).light == render::SceneLight::Day);
    CHECK(render::classifySceneLight(metered(3.7), cap(-0.5, -0.1)).light == render::SceneLight::Day);
    // A dark meter with no cap (no attitude, nothing decoded): Day.
    const render::SceneLightVerdict noCap = render::classifySceneLight(metered(3.7), std::nullopt);
    CHECK(noCap.light == render::SceneLight::Day);
    CHECK(noCap.needsCap);
    render::SkyCap invalid = cap(-3.0);
    invalid.valid = false;
    CHECK(render::classifySceneLight(metered(3.7), invalid).light == render::SceneLight::Day);
    // No exposure metadata at all: Day.
    const render::SceneLightVerdict none = render::classifySceneLight(render::MeteredLight{}, cap(-3.0));
    CHECK(none.light == render::SceneLight::Day);
    CHECK(render::sceneLightEvidence(none) == "no exposure metadata");
    // The threshold itself (-1.5 stops) is night.
    CHECK(render::classifySceneLight(metered(5.9), cap(-1.5)).light == render::SceneLight::Night);
}

// ===========================================================================
//  The cap statistics
// ===========================================================================

TEST_CASE("skyCapOf reads a levelled strip's zenith cap against grey", "[scenelight]") {
    const double skyY = 0.18 * std::exp2(-2.5);
    SECTION("a uniform night sky with high-ISO noise") {
        const auto rgba = strip(kStripW, kStripRows, skyY, 0.3, [](std::uint32_t, std::uint32_t, double, float*) {});
        const render::SkyCap c = render::skyCapOf(rgba.data(), kStripW, kStripRows);
        REQUIRE(c.valid);
        CHECK_THAT(c.stopsVsGrey, WithinAbs(-2.5, 0.05));
        CHECK_THAT(c.log2BG, WithinAbs(0.0, 1e-6));
        CHECK(c.usableFraction > 0.99);
        // The noise-adaptive threshold keeps the noisy sky (a fixed 0.15 stop
        // test would drop most of it).
        CHECK(c.flatFraction > 0.5);
        CHECK(c.sourceFraction == 0.0);
    }
    SECTION("street lamps and their glow are excluded") {
        // Three lamps of 20 (6.8 stops over grey) at elevation 60, each with a
        // +1.5 stop glow 5 deg around it.
        const auto lamp = [skyY](std::uint32_t x, std::uint32_t, double lat, float* p) {
            for (const double lon0 : {-120.0, 0.0, 120.0}) {
                const double lon = (static_cast<double>(x) + 0.5) / kStripW * 360.0 - 180.0;
                const double dLon = (lon - lon0) * std::cos(deg2rad(lat));
                const double d = std::hypot(dLon, lat - 60.0);
                if (d < 1.0) {
                    p[0] = p[1] = p[2] = 20.0f;
                } else if (d < 5.0) {
                    p[0] = p[1] = p[2] = static_cast<float>(skyY * std::exp2(1.5));
                }
            }
        };
        const auto rgba = strip(kStripW, kStripRows, skyY, 0.05, lamp);
        const render::SkyCap c = render::skyCapOf(rgba.data(), kStripW, kStripRows);
        REQUIRE(c.valid);
        CHECK(c.sourceFraction > 0.02);
        CHECK_THAT(c.stopsVsGrey, WithinAbs(-2.5, 0.05));
    }
    SECTION("a lit door filling a quarter of the cap does not decide") {
        // +1.5 stops over grey with texture, at the cap's edge.
        const auto door = [](std::uint32_t x, std::uint32_t y, double lat, float* p) {
            if (lat < 60.0 && x < kStripW / 2u) {
                const double v = 0.18 * std::exp2(1.5 + 0.8 * hashNoise(x / 3u, y / 3u + 7u));
                p[0] = p[1] = p[2] = static_cast<float>(v);
            }
        };
        const auto rgba = strip(kStripW, kStripRows, skyY, 0.2, door);
        const render::SkyCap c = render::skyCapOf(rgba.data(), kStripW, kStripRows);
        REQUIRE(c.valid);
        CHECK(c.stopsVsGrey < -1.5);  // still night
    }
    SECTION("a blue day sky") {
        const auto blue = [](std::uint32_t, std::uint32_t, double, float* p) {
            p[0] = 0.25f;
            p[1] = 0.40f;
            p[2] = 0.90f;
        };
        const auto rgba = strip(kStripW, kStripRows, 0.0, 0.0, blue);
        const render::SkyCap c = render::skyCapOf(rgba.data(), kStripW, kStripRows);
        REQUIRE(c.valid);
        CHECK_THAT(c.log2BG, WithinAbs(std::log2(0.9 / 0.4), 1e-5));
        CHECK(c.stopsVsGrey > 0.5);
    }
    SECTION("an uncovered cap, garbage and bad arguments are not measurements") {
        const auto none = strip(kStripW, kStripRows, skyY, 0.0,
                                [](std::uint32_t, std::uint32_t, double, float* p) { p[3] = 0.0f; });
        CHECK_FALSE(render::skyCapOf(none.data(), kStripW, kStripRows).valid);
        const auto nan = strip(kStripW, kStripRows, skyY, 0.0,
                               [](std::uint32_t, std::uint32_t, double, float* p) { p[1] = std::nanf(""); });
        CHECK_FALSE(render::skyCapOf(nan.data(), kStripW, kStripRows).valid);
        const auto ok = strip(kStripW, kStripRows, skyY, 0.0, [](std::uint32_t, std::uint32_t, double, float*) {});
        CHECK_FALSE(render::skyCapOf(nullptr, kStripW, kStripRows).valid);
        CHECK_FALSE(render::skyCapOf(ok.data(), 0, kStripRows).valid);
        CHECK_FALSE(render::skyCapOf(ok.data(), kStripW + 1u, kStripRows).valid);
        CHECK_FALSE(render::skyCapOf(ok.data(), kStripW, kStripW).valid);  // more rows than the map has
        render::SkyCapParams bad;
        bad.capMinElevationDeg = 95.0;
        CHECK_FALSE(render::skyCapOf(ok.data(), kStripW, kStripRows, bad).valid);
    }
}

TEST_CASE("combineSkyCaps takes the median of the valid frames", "[scenelight]") {
    render::SkyCap broken = cap(9.0);
    broken.valid = false;
    const std::vector<render::SkyCap> caps{cap(-1.99, -0.20), broken, cap(-2.75, -0.15), cap(-1.81, -0.11)};
    const render::SkyCap c = render::combineSkyCaps(caps);
    REQUIRE(c.valid);
    CHECK(c.frames == 3u);
    CHECK_THAT(c.stopsVsGrey, WithinAbs(-1.99, 1e-12));
    CHECK_THAT(c.log2BG, WithinAbs(-0.15, 1e-12));
    CHECK_FALSE(render::combineSkyCaps(std::vector<render::SkyCap>{broken}).valid);
    CHECK_FALSE(render::combineSkyCaps(std::vector<render::SkyCap>{}).valid);
}

TEST_CASE("gravity-up in body coordinates follows the attitude", "[scenelight]") {
    geom::AttitudeTrack::Options options;
    options.measuredUp = Vec3d{0.0, 0.0, 1.0};
    const meta::MetadataTrack noMetadata;  // nominal frame times
    SECTION("upright: body up is world up") {
        auto track = geom::AttitudeTrack::fromSamples({{0.0, Quatd::identity()}, {1e6, Quatd::identity()}}, options);
        REQUIRE(track.ok());
        const std::optional<Vec3d> up = render::bodyUpAt(track.value(), noMetadata, 10, 25.0);
        REQUIRE(up);
        CHECK(up->angleTo(Vec3d{0.0, 0.0, 1.0}) < 1e-9);
    }
    SECTION("on its side: up turns with the body") {
        // Body rolled 90 deg about +Y (a camera lying on a car door).
        const Quatd roll = Quatd::fromAxisAngle(Vec3d{0.0, 1.0, 0.0}, deg2rad(90.0));
        auto track = geom::AttitudeTrack::fromSamples({{0.0, roll}, {1e6, roll}}, options);
        REQUIRE(track.ok());
        const std::optional<Vec3d> up = render::bodyUpAt(track.value(), noMetadata, 0, 25.0);
        REQUIRE(up);
        // World up seen from the body is the inverse rotation of +Z.
        CHECK(up->angleTo(roll.conj().rotate(Vec3d{0.0, 0.0, 1.0})) < 1e-9);
        CHECK(std::abs(up->z) < 1e-9);
    }
    SECTION("an empty track has no up") {
        const geom::AttitudeTrack empty;
        CHECK_FALSE(render::bodyUpAt(empty, noMetadata, 0, 25.0));
    }
}

// ===========================================================================
//  The cap through the real kernel
// ===========================================================================

TEST_CASE("measureSkyCap reads a synthetic night sky through the kernel, levelled on any up", "[scenelight]") {
    ThreadPool pool(4);
    auto rig = testsynth::makeSyntheticRig(512);
    REQUIRE(rig.ok());
    geom::BlendParams blend;
    blend.lensFovDeg = 195.18;
    blend.featherDeg = 4.0;
    blend.useOcclusionMask = false;  // the synthetic rig has no occlusion arc
    const OsvColorParams linear = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::Linear, 0.0f);

    // A night sky 2.5 stops below grey above the horizon, a bright lit ground
    // below it, and a street lamp at elevation 55.
    const double skyY = 0.18 * std::exp2(-2.5);
    for (const Vec3d& upIn : {Vec3d{0.0, 0.0, 1.0}, Vec3d{1.0, 0.0, 0.0}, Vec3d{0.3, 0.5, 0.8}}) {
        const Vec3d up = upIn.normalized();
        // Any horizontal direction: the lamp's azimuth.
        const Vec3d seed = std::abs(up.x) < 0.9 ? Vec3d{1.0, 0.0, 0.0} : Vec3d{0.0, 1.0, 0.0};
        const Vec3d east = (seed - up * seed.dot(up)).normalized();
        const Vec3d lampDir = (up * std::sin(deg2rad(55.0)) + east * std::cos(deg2rad(55.0))).normalized();
        const testsynth::Radiance scene = [&](int, const Vec3d& d, double, double rgb[3]) {
            const double elevation = rad2deg(std::asin(std::clamp(d.normalized().dot(up), -1.0, 1.0)));
            double v = elevation > 5.0 ? skyY : 0.6;
            if (rad2deg(d.normalized().angleTo(lampDir)) < 1.5) {
                v = 30.0;
            }
            rgb[0] = rgb[1] = rgb[2] = v;
        };
        const testsynth::SynthPair pair = testsynth::synthPair(rig.value(), scene, pool);
        INFO("up " << up.x << ", " << up.y << ", " << up.z);
        auto measured = render::measureSkyCap(rig.value(), pair.pair, blend, up, linear, pool);
        REQUIRE(measured.ok());
        const render::SkyCap& c = measured.value();
        CHECK(c.valid);
        CHECK_THAT(c.stopsVsGrey, WithinAbs(-2.5, 0.1));
        CHECK(c.sourceFraction > 0.0);
        // Deterministic: the same frame measures the same, bit for bit.
        auto again = render::measureSkyCap(rig.value(), pair.pair, blend, up, linear, pool);
        REQUIRE(again.ok());
        CHECK(again.value().stopsVsGrey == c.stopsVsGrey);
        // The verdict on the night clip's meter.
        CHECK(render::classifySceneLight(metered(3.72), c).light == render::SceneLight::Night);
    }

    SECTION("bad inputs are refused, not measured") {
        const testsynth::Radiance flat = [&](int, const Vec3d&, double, double rgb[3]) {
            rgb[0] = rgb[1] = rgb[2] = skyY;
        };
        const testsynth::SynthPair pair = testsynth::synthPair(rig.value(), flat, pool);
        CHECK_FALSE(render::measureSkyCap(rig.value(), pair.pair, blend, Vec3d{0, 0, 0}, linear, pool).ok());
        CHECK_FALSE(
            render::measureSkyCap(rig.value(), pair.pair, blend, Vec3d{std::nan(""), 0, 1}, linear, pool).ok());
        const OsvColorParams pq = color::makeColorParams(color::kDefaultDlogMFit, color::OutputTransfer::PQ, 0.0f);
        CHECK_FALSE(render::measureSkyCap(rig.value(), pair.pair, blend, Vec3d{0, 0, 1}, pq, pool).ok());
        render::SkyCapParams odd;
        odd.equirectW = 1023;
        CHECK_FALSE(render::measureSkyCap(rig.value(), pair.pair, blend, Vec3d{0, 0, 1}, linear, pool, odd).ok());
    }
}

// ===========================================================================
//  The night profile
// ===========================================================================

TEST_CASE("the night profile changes only the field's reach, never the user's mode or strength", "[scenelight]") {
    render::PhotoSeamParams p;
    p.mode = render::PhotoSeamMode::RimAndGain;
    p.strength = 0.6;
    const render::PhotoSeamParams day = p;
    render::applyNightPhotoProfile(p);
    CHECK(p.decayDeg == 6.0);
    // The chroma decay keeps its ratio to the luma decay (3 deg at night):
    // a scale of 0 would end the overlap's full chroma in a hard edge.
    CHECK(p.chromaDecayScale == day.chromaDecayScale);
    CHECK(p.decayDeg * p.chromaDecayScale == 3.0);
    CHECK(p.maxAbsLog2Gain == 0.75);
    CHECK(p.mode == day.mode);
    CHECK(p.strength == day.strength);
    CHECK(p.temporalAlpha == day.temporalAlpha);
    // The day profile is exactly today's defaults.
    const render::PhotoSeamParams defaults;
    CHECK(defaults.decayDeg == 20.0);
    CHECK(defaults.chromaDecayScale == 0.5);
    CHECK(defaults.maxAbsLog2Gain == 1.5);

    render::LensShadingParams s;
    s.mode = render::LensShadingMode::Auto;
    s.strength = 0.4;
    render::applyNightShadingProfile(s);
    CHECK(s.mode == render::LensShadingMode::Off);
    CHECK(s.strength == 0.4);
    CHECK(std::string(render::sceneLightName(render::SceneLight::Night)) == "Night");
    CHECK(std::string(render::sceneLightName(render::SceneLight::Day)) == "Day");
}
