// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_render.cpp - PF_Cmd_RENDER, the geometry that feeds it, and the
// agreement between the CPU path and the shared kernel.
//
// The strategy throughout: render a LABELLED panorama (colour is an
// invertible function of direction, see ReframeTestSupport.h) and then read
// the colour back at a chosen output pixel.  Because R encodes longitude and
// G encodes latitude, the colour at the centre of the frame says exactly
// where the camera was pointing - so a pan of 90 degrees is not checked by
// "the image changed" but by "the centre pixel now reports longitude 90".

#include "ReframeTestSupport.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "MockHost.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "PrSDKPixelFormat.h"
#include "PrSDKSequenceInfoSuite.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using Catch::Approx;
using osv::premiere::mock::EffectWorld;
using osv::premiere::mock::InDataSpec;
using osv::premiere::mock::MockHost;

namespace {

/// The panorama every render test uses: 1024 x 512, the size docs/PREMIERE.md
/// names.  Built once because it costs a megabyte and a half of trigonometry.
const Panorama& panorama() {
    static const Panorama p = makePanorama(1024, 512);
    return p;
}

/// Longitude (deg) a sampled colour reports - decoded from the continuous
/// cos/sin pair, so a sample taken across the +/-180 seam still decodes to
/// the right angle (see ReframeTestSupport.h).
double lonOf(const float rgba[4]) noexcept { return Panorama::decodeLongitude(rgba); }
/// Latitude (deg, -90..90) a sampled colour reports, from G.
double latOf(const float rgba[4]) noexcept { return Panorama::decodeLatitude(rgba); }

/// Angular difference in degrees, wrapped into (-180, 180].
double angleDelta(double a, double b) noexcept {
    double d = std::fmod(a - b + 540.0, 360.0);
    if (d < 0.0) {
        d += 360.0;
    }
    return d - 180.0;
}

/// A ConstFrameView over the 32f packed panorama.
struct PackedSource {
    std::vector<std::uint8_t> bytes;
    ConstFrameView view;

    explicit PackedSource(const Panorama& p, std::int32_t rowBytes) {
        bytes = packBgra32f(p, rowBytes);
        view.base = bytes.data();
        view.rowBytes = rowBytes;
        view.width = p.width;
        view.height = p.height;
        view.layout = PixelLayout::Bgra32f;
        view.topDown = true;
    }
};

/// Default settings: "Full Frame" so the picture fills the output and a
/// pixel's position maps straight to a direction with no letterbox to skip.
Settings baseSettings() {
    Settings s;
    s.aspect = Aspect::FullFrame;
    s.preset = Preset::Custom;
    s.fovDeg = 90.0;
    s.distortion = 0.0;  // rectilinear: the easiest geometry to reason about
    return s;
}

}  // namespace

// ===========================================================================
//  The geometry helpers (pure functions in ReframeParams.h / ReframeCpu.cpp)
// ===========================================================================
TEST_CASE("resolveAspectRatio answers the documented ratios", "[reframe][geometry]") {
    // Fixed ratios ignore both the sequence and the frame.
    CHECK(resolveAspectRatio(Aspect::Ratio16x9, 1.0, 1.0) == Approx(16.0 / 9.0));
    CHECK(resolveAspectRatio(Aspect::Ratio9x16, 1.0, 1.0) == Approx(9.0 / 16.0));
    CHECK(resolveAspectRatio(Aspect::Ratio1x1, 2.0, 3.0) == Approx(1.0));
    CHECK(resolveAspectRatio(Aspect::Ratio4x3, 0.0, 0.0) == Approx(4.0 / 3.0));
    CHECK(resolveAspectRatio(Aspect::Ratio3x4, 0.0, 0.0) == Approx(3.0 / 4.0));
    CHECK(resolveAspectRatio(Aspect::Ratio235x1, 0.0, 0.0) == Approx(2.35));

    // "Match Sequence" uses the sequence when there is one...
    CHECK(resolveAspectRatio(Aspect::MatchSequence, 2.0, 1.0) == Approx(2.0));
    // ...and falls back to 16:9 exactly as docs/PREMIERE.md specifies.
    CHECK(resolveAspectRatio(Aspect::MatchSequence, 0.0, 1.0) == Approx(16.0 / 9.0));
    CHECK(resolveAspectRatio(Aspect::MatchSequence, -1.0, 1.0) == Approx(16.0 / 9.0));
    CHECK(resolveAspectRatio(Aspect::MatchSequence, std::nan(""), 1.0) == Approx(16.0 / 9.0));

    // "Full Frame" is the frame, whatever shape it is.
    CHECK(resolveAspectRatio(Aspect::FullFrame, 2.0, 1.5) == Approx(1.5));
    CHECK(resolveAspectRatio(Aspect::FullFrame, 2.0, 0.0) == Approx(16.0 / 9.0));
}

TEST_CASE("computeViewport centres the largest rectangle of the requested shape", "[reframe][geometry]") {
    SECTION("a 16:9 picture in a 16:9 frame fills it") {
        const Viewport v = computeViewport(1920, 1080, 16.0 / 9.0);
        CHECK(v.x == 0);
        CHECK(v.y == 0);
        CHECK(v.w == 1920);
        CHECK(v.h == 1080);
    }

    SECTION("a wider picture letterboxes") {
        const Viewport v = computeViewport(1920, 1080, 2.35);
        CHECK(v.w == 1920);
        CHECK(v.h == static_cast<int>(std::lround(1920.0 / 2.35)));
        CHECK(v.x == 0);
        CHECK(v.y == (1080 - v.h) / 2);
        CHECK(v.h < 1080);
    }

    SECTION("a taller picture pillarboxes") {
        const Viewport v = computeViewport(1920, 1080, 9.0 / 16.0);
        CHECK(v.h == 1080);
        CHECK(v.w == static_cast<int>(std::lround(1080.0 * 9.0 / 16.0)));
        CHECK(v.y == 0);
        CHECK(v.x == (1920 - v.w) / 2);
    }

    SECTION("a square picture in a wide frame") {
        const Viewport v = computeViewport(1920, 1080, 1.0);
        CHECK(v.w == 1080);
        CHECK(v.h == 1080);
        CHECK(v.x == 420);
        CHECK(v.y == 0);
    }

    SECTION("degenerate inputs never produce an out-of-frame rectangle") {
        for (const double ratio : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()}) {
            const Viewport v = computeViewport(640, 480, ratio);
            CHECK(v.w > 0);
            CHECK(v.h > 0);
            CHECK(v.x >= 0);
            CHECK(v.y >= 0);
            CHECK(v.x + v.w <= 640);
            CHECK(v.y + v.h <= 480);
        }
        // A zero-sized frame yields a zero-sized viewport, which buildParams
        // treats as invalid rather than dividing by it.
        const Viewport empty = computeViewport(0, 0, 1.0);
        CHECK(empty.w == 0);
        CHECK(empty.h == 0);
    }

    SECTION("the rectangle always fits, for every aspect in the table") {
        for (const AspectEntry& e : kAspects) {
            const double ratio = resolveAspectRatio(e.value, 16.0 / 9.0, 1920.0 / 1080.0);
            const Viewport v = computeViewport(1920, 1080, ratio);
            INFO("aspect '" << e.label << "' ratio " << ratio);
            CHECK(v.x + v.w <= 1920);
            CHECK(v.y + v.h <= 1080);
            // And it is the LARGEST such rectangle: growing either edge by
            // two pixels (one per side, keeping it centred) would overflow.
            CHECK((v.w + 2 > 1920 || v.h + 2 > 1080));
        }
    }
}

// ===========================================================================
//  buildParams
// ===========================================================================
TEST_CASE("buildParams rejects everything it cannot render", "[reframe][geometry]") {
    PackedSource src(panorama(), panorama().width * 16);
    const Settings s = baseSettings();

    SECTION("a null source") {
        ConstFrameView bad = src.view;
        bad.base = nullptr;
        CHECK_FALSE(buildParams(s, bad, 640, 360, 0.0).valid);
    }
    SECTION("a zero-sized source") {
        ConstFrameView bad = src.view;
        bad.width = 0;
        CHECK_FALSE(buildParams(s, bad, 640, 360, 0.0).valid);
    }
    SECTION("a pitch too small for one row") {
        ConstFrameView bad = src.view;
        bad.rowBytes = 16;  // one pixel, not 1024
        CHECK_FALSE(buildParams(s, bad, 640, 360, 0.0).valid);
    }
    SECTION("an 8-bit source (the kernel's sampler reads float or half only)") {
        ConstFrameView bad = src.view;
        bad.layout = PixelLayout::Bgra8u;
        CHECK_FALSE(buildParams(s, bad, 640, 360, 0.0).valid);
    }
    SECTION("a zero-sized output") {
        CHECK_FALSE(buildParams(s, src.view, 0, 360, 0.0).valid);
        CHECK_FALSE(buildParams(s, src.view, 640, 0, 0.0).valid);
    }
    SECTION("an absurd output size") {
        CHECK_FALSE(buildParams(s, src.view, 1 << 20, 360, 0.0).valid);
    }
}

TEST_CASE("buildParams accepts exactly the source layouts the kernel can address", "[reframe][geometry]") {
    // The shared sampler indexes rows with an UNSIGNED multiply
    // (osvFetchRgba), so image rows must run FORWARD in memory.  Two of the
    // four (topDown, pitch sign) combinations satisfy that because the two
    // inversions cancel; the other two would need the pixels themselves
    // mirrored, which no pointer arithmetic can do.
    const Panorama& p = panorama();
    const std::int32_t pitch = p.width * 16;
    const std::vector<std::uint8_t> bytes = packBgra32f(p, pitch);
    const char* first = reinterpret_cast<const char*>(bytes.data());
    const char* last = first + static_cast<std::size_t>(pitch) * static_cast<std::size_t>(p.height - 1);

    auto makeView = [&](const char* base, std::int32_t rowBytes, bool topDown) {
        ConstFrameView v;
        v.base = base;
        v.rowBytes = rowBytes;
        v.width = p.width;
        v.height = p.height;
        v.layout = PixelLayout::Bgra32f;
        v.topDown = topDown;
        return v;
    };

    SECTION("top-down with a positive pitch is the ordinary case") {
        const ConstFrameView v = makeView(first, pitch, true);
        CHECK(sourceRowsRunForward(v));
        const KernelSetup setup = buildParams(baseSettings(), v, 320, 180, 0.0);
        REQUIRE(setup.valid);
        CHECK(setup.sourceRow0 == first);
        CHECK(setup.source.pitchBytes == pitch);
    }

    SECTION("bottom-up with a negative pitch is the same frame described backwards") {
        // base points at the LAST row in memory and the rows march backwards,
        // so image row 0 is at `last` and image row 1 is at `last + pitch`.
        const ConstFrameView v = makeView(last, -pitch, false);
        CHECK(sourceRowsRunForward(v));
        const KernelSetup setup = buildParams(baseSettings(), v, 320, 180, 0.0);
        REQUIRE(setup.valid);
        // ...which is byte for byte the top-down description above.
        CHECK(setup.sourceRow0 == first);
        CHECK(setup.source.pitchBytes == pitch);
    }

    SECTION("the two mirrored arrangements are refused, not rendered upside down") {
        // Top-down base with rows running backwards.
        const ConstFrameView backwards = makeView(last, -pitch, true);
        CHECK_FALSE(sourceRowsRunForward(backwards));
        CHECK_FALSE(buildParams(baseSettings(), backwards, 320, 180, 0.0).valid);

        // Bottom-up storage with a forward pitch: the image really is upside
        // down in memory and needs a vertical flip the kernel cannot do.
        const ConstFrameView upsideDown = makeView(first, pitch, false);
        CHECK_FALSE(sourceRowsRunForward(upsideDown));
        CHECK_FALSE(buildParams(baseSettings(), upsideDown, 320, 180, 0.0).valid);
    }

    SECTION("a one-row frame has no stride to get wrong") {
        ConstFrameView v = makeView(first, pitch, false);
        v.height = 1;
        CHECK(sourceRowsRunForward(v));
    }

    SECTION("the two accepted descriptions render identically") {
        const KernelSetup a = buildParams(baseSettings(), makeView(first, pitch, true), 128, 96, 0.0);
        const KernelSetup b = buildParams(baseSettings(), makeView(last, -pitch, false), 128, 96, 0.0);
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        for (int y = 0; y < 96; y += 7) {
            for (int x = 0; x < 128; x += 7) {
                float pa[4];
                float pb[4];
                REQUIRE(renderPixel(a, makeView(first, pitch, true), x, y, pa));
                REQUIRE(renderPixel(b, makeView(last, -pitch, false), x, y, pb));
                for (int c = 0; c < 4; ++c) {
                    CHECK(pa[c] == pb[c]);
                }
            }
        }
    }
}

TEST_CASE("buildParams clamps hostile parameter values", "[reframe][geometry]") {
    PackedSource src(panorama(), panorama().width * 16);

    SECTION("NaN angles and NaN field of view still produce a usable camera") {
        Settings s = baseSettings();
        s.panDeg = std::nan("");
        s.tiltDeg = std::nan("");
        s.fovDeg = std::nan("");
        s.distortion = std::nan("");
        const KernelSetup setup = buildParams(s, src.view, 320, 180, 0.0);
        REQUIRE(setup.valid);
        CHECK(std::isfinite(setup.params.focalPx));
        CHECK(setup.params.focalPx > 0.0f);
        for (const float v : setup.params.Rout) {
            CHECK(std::isfinite(v));
        }
    }

    SECTION("tilt is clamped to the poles") {
        Settings s = baseSettings();
        s.tiltDeg = 1000.0;
        const KernelSetup a = buildParams(s, src.view, 320, 180, 0.0);
        s.tiltDeg = 90.0;
        const KernelSetup b = buildParams(s, src.view, 320, 180, 0.0);
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        // A tilt past the pole is the same camera as a tilt at the pole.
        for (int i = 0; i < 9; ++i) {
            CHECK(a.params.Rout[i] == Approx(b.params.Rout[i]).margin(1e-6));
        }
    }

    SECTION("the field of view is clamped so the eye-offset model stays invertible") {
        Settings s = baseSettings();
        s.distortion = 0.0;   // d = 0: the model degenerates at 180 degrees
        s.fovDeg = 350.0;     // far beyond it
        const KernelSetup setup = buildParams(s, src.view, 320, 180, 0.0);
        REQUIRE(setup.valid);
        CHECK(std::isfinite(setup.params.focalPx));
        CHECK(setup.params.focalPx > 0.0f);
    }
}

// ===========================================================================
//  The CPU renderer against the shared kernel
// ===========================================================================
TEST_CASE("renderCpu reproduces osvReframeEquirectPixel exactly", "[reframe][cpu]") {
    // This is the parity anchor: whatever renderCpu does around the kernel
    // (threading, row addressing, storing), the VALUE it writes must be the
    // kernel's own answer.
    PackedSource src(panorama(), panorama().width * 16);
    Settings s = baseSettings();
    s.aspect = Aspect::Ratio16x9;
    s.panDeg = 37.0;
    s.tiltDeg = -12.0;
    s.rollDeg = 5.0;
    s.fovDeg = 110.0;
    s.distortion = 25.0;

    constexpr int kW = 320;
    constexpr int kH = 240;
    const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
    REQUIRE(setup.valid);

    std::vector<std::uint8_t> out(static_cast<std::size_t>(kW) * kH * 16u, 0u);
    FrameView dst;
    dst.base = out.data();
    dst.rowBytes = kW * 16;
    dst.width = kW;
    dst.height = kH;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    REQUIRE(renderCpu(setup, src.view, dst, nullptr));

    // Sample a grid rather than every pixel: 1200 comparisons is plenty to
    // catch an addressing bug and keeps the test fast.
    double worst = 0.0;
    for (int y = 0; y < kH; y += 8) {
        for (int x = 0; x < kW; x += 8) {
            float expected[4];
            osvReframeEquirectPixel(&setup.params, &setup.source, setup.sourceRow0, x, y, expected);
            float actual[4];
            readPixelBgra32f(out.data(), dst.rowBytes, x, y, actual);
            for (int c = 0; c < 4; ++c) {
                worst = std::max(worst, std::fabs(static_cast<double>(expected[c]) - static_cast<double>(actual[c])));
            }
        }
    }
    INFO("worst channel difference: " << worst);
    // The 32f path stores the float the kernel produced, unmodified.
    CHECK(worst < 1e-5);
}

TEST_CASE("renderCpu gives the same picture with and without the thread pool", "[reframe][cpu]") {
    PackedSource src(panorama(), panorama().width * 16);
    Settings s = baseSettings();
    s.panDeg = 80.0;

    constexpr int kW = 256;
    constexpr int kH = 144;
    const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
    REQUIRE(setup.valid);

    auto render = [&](osv::ThreadPool* pool) {
        std::vector<std::uint8_t> out(static_cast<std::size_t>(kW) * kH * 16u, 0xCDu);
        FrameView dst;
        dst.base = out.data();
        dst.rowBytes = kW * 16;
        dst.width = kW;
        dst.height = kH;
        dst.layout = PixelLayout::Bgra32f;
        dst.topDown = true;
        REQUIRE(renderCpu(setup, src.view, dst, pool));
        return out;
    };

    osv::ThreadPool pool(4);
    // Parallel row work must be deterministic: each row writes only its own
    // destination row and reads only the const source.
    CHECK(render(nullptr) == render(&pool));
}

TEST_CASE("renderCpu refuses a mismatched destination without writing anything", "[reframe][cpu]") {
    PackedSource src(panorama(), panorama().width * 16);
    const KernelSetup setup = buildParams(baseSettings(), src.view, 320, 180, 0.0);
    REQUIRE(setup.valid);

    constexpr std::uint8_t kSentinel = 0xA5;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(320) * 180u * 16u, kSentinel);
    FrameView dst;
    dst.base = out.data();
    dst.rowBytes = 320 * 16;
    dst.width = 320;
    dst.height = 179;  // NOT what the setup was built for
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;

    CHECK_FALSE(renderCpu(setup, src.view, dst, nullptr));
    // A half-written frame reaching the host is worse than an untouched one.
    CHECK(std::all_of(out.begin(), out.end(), [](std::uint8_t b) { return b == kSentinel; }));
}

// ===========================================================================
//  Where the camera actually looks
// ===========================================================================
TEST_CASE("the centre pixel reports the direction the camera is pointed at", "[reframe][cpu][geometry]") {
    PackedSource src(panorama(), panorama().width * 16);

    // Every (pan, tilt) docs/PREMIERE.md's verification section names.
    struct Case {
        double pan;
        double tilt;
    };
    const Case cases[] = {{0.0, 0.0}, {90.0, 0.0}, {180.0, 0.0}, {-90.0, 0.0},
                          {0.0, 45.0}, {0.0, -45.0}, {90.0, 45.0}, {180.0, -45.0}};

    constexpr int kW = 321;  // odd, so there is an exact centre pixel
    constexpr int kH = 241;

    for (const Case& c : cases) {
        INFO("pan " << c.pan << " tilt " << c.tilt);
        Settings s = baseSettings();
        s.panDeg = c.pan;
        s.tiltDeg = c.tilt;

        const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
        REQUIRE(setup.valid);

        float centre[4];
        REQUIRE(renderPixel(setup, src.view, kW / 2, kH / 2, centre));

        // The label decodes to a direction; that direction must be the one
        // the pan and tilt dials asked for.  A positive yaw turns the view
        // towards -X, which in the Standard layout is a NEGATIVE longitude,
        // so the expected longitude is -pan.
        CHECK(std::fabs(angleDelta(lonOf(centre), -c.pan)) < 1.0);
        CHECK(std::fabs(latOf(centre) - c.tilt) < 1.0);
        CHECK(centre[3] == Approx(1.0).margin(1e-5));
    }
}

TEST_CASE("panning moves the picture and wrapping round returns it", "[reframe][cpu][geometry]") {
    PackedSource src(panorama(), panorama().width * 16);
    constexpr int kW = 161;
    constexpr int kH = 161;

    auto centreColour = [&](double pan) {
        Settings s = baseSettings();
        s.panDeg = pan;
        const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
        REQUIRE(setup.valid);
        float rgba[4];
        REQUIRE(renderPixel(setup, src.view, kW / 2, kH / 2, rgba));
        return std::array<float, 4>{rgba[0], rgba[1], rgba[2], rgba[3]};
    };

    const auto at0 = centreColour(0.0);
    const auto at360 = centreColour(360.0);
    const auto at90 = centreColour(90.0);

    // A full turn is the same camera.
    for (int c = 0; c < 4; ++c) {
        CHECK(at360[c] == Approx(at0[c]).margin(2e-3));
    }
    // A quarter turn is not: the decoded longitude must have moved by 90.
    CHECK(std::fabs(angleDelta(lonOf(at90.data()), lonOf(at0.data()) - 90.0)) < 1.5);
}

TEST_CASE("the longitude seam is sampled without a discontinuity", "[reframe][cpu][geometry]") {
    // Looking straight at the +/-180 seam is the case a naive sampler gets
    // wrong: it clamps instead of wrapping and paints a hard edge.
    PackedSource src(panorama(), panorama().width * 16);
    Settings s = baseSettings();
    s.panDeg = 180.0;
    s.fovDeg = 60.0;

    constexpr int kW = 201;
    constexpr int kH = 101;
    const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
    REQUIRE(setup.valid);

    // Walk the centre row across the seam.  R encodes longitude, which jumps
    // by 1.0 at the seam BY CONSTRUCTION, so compare the SPHERICAL angle
    // instead: consecutive pixels must be a fraction of a degree apart.
    double worstStep = 0.0;
    double previousLon = 0.0;
    bool havePrevious = false;
    for (int x = 0; x < kW; ++x) {
        float rgba[4];
        REQUIRE(renderPixel(setup, src.view, x, kH / 2, rgba));
        const double lon = lonOf(rgba);
        if (havePrevious) {
            worstStep = std::max(worstStep, std::fabs(angleDelta(lon, previousLon)));
        }
        previousLon = lon;
        havePrevious = true;
    }
    INFO("largest angular step between neighbouring pixels: " << worstStep);
    // 60 degrees over 201 pixels is ~0.3 deg per pixel; anything near 180
    // would be the clamp artefact.
    CHECK(worstStep < 2.0);
}

// ===========================================================================
//  The letterbox
// ===========================================================================
TEST_CASE("the letterbox is transparent and the picture is opaque", "[reframe][cpu][geometry]") {
    PackedSource src(panorama(), panorama().width * 16);

    // A tall frame with a wide picture: bars top and bottom.
    constexpr int kW = 400;
    constexpr int kH = 400;
    Settings s = baseSettings();
    s.aspect = Aspect::Ratio235x1;

    const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
    REQUIRE(setup.valid);
    const Viewport view = computeViewport(kW, kH, 2.35);
    REQUIRE(view.h < kH);
    REQUIRE(view.w == kW);

    std::vector<std::uint8_t> out(static_cast<std::size_t>(kW) * kH * 16u, 0u);
    FrameView dst;
    dst.base = out.data();
    dst.rowBytes = kW * 16;
    dst.width = kW;
    dst.height = kH;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    REQUIRE(renderCpu(setup, src.view, dst, nullptr));

    SECTION("outside the viewport every channel is zero") {
        // A row above the picture and a row below it.
        for (const int y : {0, view.y - 1, view.y + view.h, kH - 1}) {
            if (y < 0 || y >= kH) {
                continue;
            }
            INFO("letterbox row " << y);
            for (int x = 0; x < kW; x += 17) {
                float rgba[4];
                readPixelBgra32f(out.data(), dst.rowBytes, x, y, rgba);
                CHECK(rgba[0] == 0.0f);
                CHECK(rgba[1] == 0.0f);
                CHECK(rgba[2] == 0.0f);
                // Transparent, not black-opaque: that is what
                // PF_OutFlag2_REVEALS_ZERO_ALPHA promises the host.
                CHECK(rgba[3] == 0.0f);
            }
        }
    }

    SECTION("inside the viewport alpha is the panorama's, which is 1") {
        for (int y = view.y + 2; y < view.y + view.h - 2; y += 13) {
            for (int x = 2; x < kW - 2; x += 29) {
                float rgba[4];
                readPixelBgra32f(out.data(), dst.rowBytes, x, y, rgba);
                INFO("picture pixel " << x << "," << y);
                CHECK(rgba[3] == Approx(1.0).margin(1e-5));
            }
        }
    }

    SECTION("the boundary is exactly where computeViewport says") {
        float insideTop[4];
        float outsideTop[4];
        readPixelBgra32f(out.data(), dst.rowBytes, kW / 2, view.y, insideTop);
        readPixelBgra32f(out.data(), dst.rowBytes, kW / 2, view.y - 1, outsideTop);
        CHECK(insideTop[3] > 0.5f);
        CHECK(outsideTop[3] == 0.0f);
    }
}

TEST_CASE("every aspect produces its own letterbox", "[reframe][cpu][geometry]") {
    PackedSource src(panorama(), panorama().width * 16);
    constexpr int kW = 480;
    constexpr int kH = 270;  // a 16:9 frame

    for (const AspectEntry& e : kAspects) {
        INFO("aspect '" << e.label << "'");
        Settings s = baseSettings();
        s.aspect = e.value;
        // Give "Match Sequence" a square sequence so it differs from the
        // 16:9 frame and the test actually exercises the lookup.
        const double sequenceAspect = 1.0;

        const KernelSetup setup = buildParams(s, src.view, kW, kH, sequenceAspect);
        REQUIRE(setup.valid);

        const double ratio = resolveAspectRatio(e.value, sequenceAspect, static_cast<double>(kW) / kH);
        const Viewport expected = computeViewport(kW, kH, ratio);
        CHECK(setup.params.viewX == expected.x);
        CHECK(setup.params.viewY == expected.y);
        CHECK(setup.params.viewW == expected.w);
        CHECK(setup.params.viewH == expected.h);

        // Render and confirm the alpha boundary matches the rectangle.
        std::vector<std::uint8_t> out(static_cast<std::size_t>(kW) * kH * 16u, 0u);
        FrameView dst;
        dst.base = out.data();
        dst.rowBytes = kW * 16;
        dst.width = kW;
        dst.height = kH;
        dst.layout = PixelLayout::Bgra32f;
        dst.topDown = true;
        REQUIRE(renderCpu(setup, src.view, dst, nullptr));

        float inside[4];
        readPixelBgra32f(out.data(), dst.rowBytes, expected.x + expected.w / 2, expected.y + expected.h / 2, inside);
        CHECK(inside[3] > 0.5f);

        if (expected.x > 0) {
            float outside[4];
            readPixelBgra32f(out.data(), dst.rowBytes, expected.x - 1, expected.y + expected.h / 2, outside);
            CHECK(outside[3] == 0.0f);
        }
        if (expected.y > 0) {
            float outside[4];
            readPixelBgra32f(out.data(), dst.rowBytes, expected.x + expected.w / 2, expected.y - 1, outside);
            CHECK(outside[3] == 0.0f);
        }
    }
}

// ===========================================================================
//  Output layouts
// ===========================================================================
TEST_CASE("the 8-bit output agrees with the float output within one code", "[reframe][cpu]") {
    PackedSource src(panorama(), panorama().width * 16);
    Settings s = baseSettings();
    s.panDeg = 45.0;
    s.tiltDeg = 20.0;

    constexpr int kW = 200;
    constexpr int kH = 120;
    const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
    REQUIRE(setup.valid);

    std::vector<std::uint8_t> out32(static_cast<std::size_t>(kW) * kH * 16u, 0u);
    FrameView dst32;
    dst32.base = out32.data();
    dst32.rowBytes = kW * 16;
    dst32.width = kW;
    dst32.height = kH;
    dst32.layout = PixelLayout::Bgra32f;
    dst32.topDown = true;
    REQUIRE(renderCpu(setup, src.view, dst32, nullptr));

    std::vector<std::uint8_t> out8(static_cast<std::size_t>(kW) * kH * 4u, 0u);
    FrameView dst8 = dst32;
    dst8.base = out8.data();
    dst8.rowBytes = kW * 4;
    dst8.layout = PixelLayout::Bgra8u;
    REQUIRE(renderCpu(setup, src.view, dst8, nullptr));

    double worst = 0.0;
    for (int y = 0; y < kH; y += 3) {
        for (int x = 0; x < kW; x += 3) {
            float a[4];
            float b[4];
            readPixelBgra32f(out32.data(), dst32.rowBytes, x, y, a);
            readPixelBgra8u(out8.data(), dst8.rowBytes, x, y, b);
            for (int c = 0; c < 4; ++c) {
                worst = std::max(worst, std::fabs(static_cast<double>(a[c]) - static_cast<double>(b[c])));
            }
        }
    }
    INFO("worst 8u vs 32f difference: " << worst);
    // Rounding to nearest costs at most half a code; allow a whole one.
    CHECK(worst <= 1.0 / 255.0 + 1e-6);
}

TEST_CASE("the 16f output agrees with the float output to half precision", "[reframe][cpu]") {
    PackedSource src(panorama(), panorama().width * 16);
    Settings s = baseSettings();
    s.panDeg = -60.0;

    constexpr int kW = 192;
    constexpr int kH = 108;
    const KernelSetup setup = buildParams(s, src.view, kW, kH, 0.0);
    REQUIRE(setup.valid);

    std::vector<std::uint8_t> out32(static_cast<std::size_t>(kW) * kH * 16u, 0u);
    FrameView dst32;
    dst32.base = out32.data();
    dst32.rowBytes = kW * 16;
    dst32.width = kW;
    dst32.height = kH;
    dst32.layout = PixelLayout::Bgra32f;
    dst32.topDown = true;
    REQUIRE(renderCpu(setup, src.view, dst32, nullptr));

    std::vector<std::uint8_t> out16(static_cast<std::size_t>(kW) * kH * 8u, 0u);
    FrameView dst16 = dst32;
    dst16.base = out16.data();
    dst16.rowBytes = kW * 8;
    dst16.layout = PixelLayout::Bgra16f;
    REQUIRE(renderCpu(setup, src.view, dst16, nullptr));

    // The comparison uses this file's OWN half decoder, not the plug-in's,
    // so a wrong conversion cannot hide behind a matching inverse.
    for (int y = 0; y < kH; y += 5) {
        for (int x = 0; x < kW; x += 5) {
            float a[4];
            float b[4];
            readPixelBgra32f(out32.data(), dst32.rowBytes, x, y, a);
            readPixelBgra16f(out16.data(), dst16.rowBytes, x, y, b);
            for (int c = 0; c < 4; ++c) {
                INFO("pixel " << x << "," << y << " channel " << c);
                // Round to nearest even: the stored half is the closest one,
                // so the error is bounded by half an ulp (~2^-11 near 1.0).
                CHECK(static_cast<double>(b[c]) == Approx(static_cast<double>(a[c])).margin(5e-4));
                CHECK(b[c] == halfToFloat(floatToHalf(a[c])));
            }
        }
    }
}

TEST_CASE("a negative destination pitch writes the same picture upside down in memory", "[reframe][cpu]") {
    // Effect worlds may legally have a negative rowbytes.  The renderer must
    // honour the sign, not assume forward rows.
    PackedSource src(panorama(), panorama().width * 16);
    constexpr int kW = 64;
    constexpr int kH = 48;
    const KernelSetup setup = buildParams(baseSettings(), src.view, kW, kH, 0.0);
    REQUIRE(setup.valid);

    const std::int32_t pitch = kW * 16;
    std::vector<std::uint8_t> forward(static_cast<std::size_t>(pitch) * kH, 0u);
    FrameView dstForward;
    dstForward.base = forward.data();
    dstForward.rowBytes = pitch;
    dstForward.width = kW;
    dstForward.height = kH;
    dstForward.layout = PixelLayout::Bgra32f;
    dstForward.topDown = true;
    REQUIRE(renderCpu(setup, src.view, dstForward, nullptr));

    std::vector<std::uint8_t> backward(static_cast<std::size_t>(pitch) * kH, 0u);
    FrameView dstBackward = dstForward;
    // Base points at the LAST row and the pitch runs backwards.
    dstBackward.base = backward.data() + static_cast<std::size_t>(pitch) * static_cast<std::size_t>(kH - 1);
    dstBackward.rowBytes = -pitch;
    REQUIRE(renderCpu(setup, src.view, dstBackward, nullptr));

    // Row y of the image is at forward[y] and at backward[kH-1-y].
    for (int y = 0; y < kH; ++y) {
        const std::uint8_t* a = forward.data() + static_cast<std::size_t>(pitch) * static_cast<std::size_t>(y);
        const std::uint8_t* b =
            backward.data() + static_cast<std::size_t>(pitch) * static_cast<std::size_t>(kH - 1 - y);
        INFO("row " << y);
        CHECK(std::memcmp(a, b, static_cast<std::size_t>(kW) * 16u) == 0);
    }
}

TEST_CASE("a 16f source is sampled as accurately as a 32f one", "[reframe][cpu]") {
    // The shared sampler decodes half on the fly (OsvRgbaSource::isHalf), so
    // a half input must land within half-float precision of a float input.
    const Panorama& p = panorama();
    PackedSource src32(p, p.width * 16);

    std::vector<std::uint8_t> bytes16 = packBgra16f(p, p.width * 8);
    ConstFrameView src16;
    src16.base = bytes16.data();
    src16.rowBytes = p.width * 8;
    src16.width = p.width;
    src16.height = p.height;
    src16.layout = PixelLayout::Bgra16f;
    src16.topDown = true;

    Settings s = baseSettings();
    s.panDeg = 25.0;
    s.tiltDeg = -8.0;

    constexpr int kW = 128;
    constexpr int kH = 96;
    const KernelSetup setup32 = buildParams(s, src32.view, kW, kH, 0.0);
    const KernelSetup setup16 = buildParams(s, src16, kW, kH, 0.0);
    REQUIRE(setup32.valid);
    REQUIRE(setup16.valid);
    CHECK(setup16.source.isHalf == 1);
    CHECK(setup32.source.isHalf == 0);

    double worst = 0.0;
    for (int y = 0; y < kH; y += 3) {
        for (int x = 0; x < kW; x += 3) {
            float a[4];
            float b[4];
            REQUIRE(renderPixel(setup32, src32.view, x, y, a));
            REQUIRE(renderPixel(setup16, src16, x, y, b));
            for (int c = 0; c < 4; ++c) {
                worst = std::max(worst, std::fabs(static_cast<double>(a[c]) - static_cast<double>(b[c])));
            }
        }
    }
    INFO("worst 16f-source vs 32f-source difference: " << worst);
    CHECK(worst < 1e-3);
}

// ===========================================================================
//  PF_Cmd_RENDER through the real module
// ===========================================================================
namespace {

/// Everything PF_Cmd_RENDER needs: a mock host, an effect ref with the
/// parameters registered, an input world holding the panorama and an output
/// world to render into.
struct RenderFixture {
    MockHost host;
    PF_ProgPtr ref = nullptr;
    std::unique_ptr<EffectWorld> input;
    std::unique_ptr<EffectWorld> output;

    /// `inputFormat` selects the INPUT world's layout.  The default 32f is
    /// what every existing case uses; BGRA_4444_8u exercises the 8-bit
    /// promotion path, which the effect advertises in GLOBAL_SETUP and so
    /// must accept.
    RenderFixture(int outW, int outH, PrPixelFormat format, PrTimelineID timeline = 0x2000,
                  PrPixelFormat inputFormat = PrPixelFormat_BGRA_4444_32f) {
        ref = host.createEffectRef(timeline, 11);
        REQUIRE(ref != nullptr);

        PF_InData in = host.makeInData(ref, {});
        PF_OutData out = host.makeOutData();
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);

        // The input world carries the panorama; `inputFormat` selects its
        // layout and `format` the OUTPUT world's.
        const Panorama& p = panorama();
        input = host.createWorld(static_cast<std::uint32_t>(p.width), static_cast<std::uint32_t>(p.height),
                                 inputFormat);
        REQUIRE(input != nullptr);

        if (inputFormat == PrPixelFormat_BGRA_4444_32f) {
            const std::vector<std::uint8_t> packed = packBgra32f(p, input->rowBytes());
            REQUIRE(packed.size() <= static_cast<std::size_t>(input->rowBytes()) * static_cast<std::size_t>(p.height));
            std::memcpy(input->pixels(), packed.data(), packed.size());
        } else {
            REQUIRE(inputFormat == PrPixelFormat_BGRA_4444_8u);
            // Quantise the float panorama to 8-bit codes.  This is the exact
            // inverse of the promotion the effect performs (code / 255), so a
            // pixel that survives both round trips is unchanged and the test
            // can compare an 8u-input render against a 32f-input render of
            // the SAME quantised data.
            const std::int32_t srcPitch = p.width * 16;
            const std::vector<std::uint8_t> packed = packBgra32f(p, srcPitch);
            auto* dst = reinterpret_cast<std::uint8_t*>(input->pixels());
            for (int y = 0; y < p.height; ++y) {
                const auto* srcRow =
                    reinterpret_cast<const float*>(packed.data() + static_cast<std::size_t>(y) * srcPitch);
                std::uint8_t* dstRow = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(input->rowBytes());
                for (int x = 0; x < p.width * 4; ++x) {
                    const float v = srcRow[x];
                    const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    dstRow[x] = static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
                }
            }
        }
        host.setInputWorld(ref, input.get());

        output = host.createWorld(static_cast<std::uint32_t>(outW), static_cast<std::uint32_t>(outH), format);
        REQUIRE(output != nullptr);
    }

    ~RenderFixture() {
        if (ref) {
            host.destroyEffectRef(ref);
        }
    }
    RenderFixture(const RenderFixture&) = delete;
    RenderFixture& operator=(const RenderFixture&) = delete;

    /// Set one parameter's value before rendering.
    void setPopup(int aeIndex, int value) {
        std::vector<PF_ParamDef> params = host.addedParams(ref);
        PF_ParamDef def = params[static_cast<std::size_t>(aeIndex) - 1u];
        def.u.pd.value = value;
        host.setParamValue(ref, aeIndex, def);
    }
    void setAngle(int aeIndex, double degrees) {
        std::vector<PF_ParamDef> params = host.addedParams(ref);
        PF_ParamDef def = params[static_cast<std::size_t>(aeIndex) - 1u];
        def.u.ad.value = static_cast<PF_Fixed>(std::llround(degrees * 65536.0));
        host.setParamValue(ref, aeIndex, def);
    }
    void setFloat(int aeIndex, double value) {
        std::vector<PF_ParamDef> params = host.addedParams(ref);
        PF_ParamDef def = params[static_cast<std::size_t>(aeIndex) - 1u];
        def.u.fs_d.value = static_cast<PF_FpShort>(value);
        host.setParamValue(ref, aeIndex, def);
    }
    void setCheckbox(int aeIndex, bool value) {
        std::vector<PF_ParamDef> params = host.addedParams(ref);
        PF_ParamDef def = params[static_cast<std::size_t>(aeIndex) - 1u];
        def.u.bd.value = value ? 1 : 0;
        host.setParamValue(ref, aeIndex, def);
    }

    /// Install an AE-side keyframe: checkout_param returns `degrees` for this
    /// angle parameter at exactly `time`, and the setAngle() value at every
    /// other time.  This is what makes the Smooth Keyframes average
    /// observable: with one value for all times, ANY normalised combination
    /// of three samples returns it.
    void setAngleAtTime(int aeIndex, A_long time, double degrees) {
        std::vector<PF_ParamDef> params = host.addedParams(ref);
        PF_ParamDef def = params[static_cast<std::size_t>(aeIndex) - 1u];
        def.u.ad.value = static_cast<PF_Fixed>(std::llround(degrees * 65536.0));
        host.setParamValueAtTime(ref, aeIndex, time, def);
    }

    /// Run PF_Cmd_RENDER at the default time.
    PF_Err render() { return render(InDataSpec{}); }

    /// Run PF_Cmd_RENDER with an explicit time spec, so a test can place the
    /// render at a chosen current_time / time_step.
    PF_Err render(const InDataSpec& spec) {
        PF_InData in = host.makeInData(ref, spec);
        PF_OutData out = host.makeOutData();
        std::vector<PF_ParamDef*> params = host.renderParams(ref);
        return LoadedPlugin::instance().effectMain()(PF_Cmd_RENDER, &in, &out, params.data(), &output->world(),
                                                     nullptr);
    }
};

/// Snapshot a rendered world's bytes so a second render into a different
/// fixture can be compared against it.
[[nodiscard]] std::vector<std::uint8_t> copyWorld(const EffectWorld& world, int height) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(world.pixels());
    const std::size_t bytes = static_cast<std::size_t>(world.rowBytes()) * static_cast<std::size_t>(height);
    return std::vector<std::uint8_t>(base, base + bytes);
}

}  // namespace

TEST_CASE("PF_Cmd_RENDER produces the expected picture through the loaded module", "[reframe][render]") {
    constexpr int kW = 321;
    constexpr int kH = 241;
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);

    // Full Frame, rectilinear, 90 degrees: the simplest geometry.
    f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
    f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat(kIndexFov, 90.0);
    f.setFloat(kIndexDistortion, 0.0);

    struct Case {
        double pan;
        double tilt;
    };
    const Case cases[] = {{0.0, 0.0}, {90.0, 0.0}, {180.0, 0.0}, {0.0, 45.0}, {0.0, -45.0}};

    for (const Case& c : cases) {
        INFO("pan " << c.pan << " tilt " << c.tilt);
        f.setAngle(kIndexPan, c.pan);
        f.setAngle(kIndexTilt, c.tilt);
        REQUIRE(f.render() == PF_Err_NONE);

        float centre[4];
        readPixelBgra32f(reinterpret_cast<const std::uint8_t*>(f.output->pixels()), f.output->rowBytes(), kW / 2,
                         kH / 2, centre);
        CHECK(std::fabs(angleDelta(lonOf(centre), -c.pan)) < 1.0);
        CHECK(std::fabs(latOf(centre) - c.tilt) < 1.0);
        CHECK(centre[3] == Approx(1.0).margin(1e-5));
    }
}

TEST_CASE("PF_Cmd_RENDER letterboxes according to the aspect popup", "[reframe][render]") {
    constexpr int kW = 400;
    constexpr int kH = 400;
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
    f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::Ratio235x1));
    REQUIRE(f.render() == PF_Err_NONE);

    const Viewport view = computeViewport(kW, kH, 2.35);
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());

    float insideCentre[4];
    readPixelBgra32f(pixels, f.output->rowBytes(), kW / 2, view.y + view.h / 2, insideCentre);
    CHECK(insideCentre[3] > 0.5f);

    float aboveBar[4];
    readPixelBgra32f(pixels, f.output->rowBytes(), kW / 2, 0, aboveBar);
    CHECK(aboveBar[3] == 0.0f);
    CHECK(aboveBar[0] == 0.0f);
}

TEST_CASE("PF_Cmd_RENDER honours Match Sequence through the Sequence Info Suite", "[reframe][render]") {
    constexpr int kW = 400;
    constexpr int kH = 400;
    constexpr PrTimelineID kTimeline = 0x2222;
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f, kTimeline);

    // Tell the mock the sequence is 2:1; "Match Sequence" must pick that up
    // through GetContainingTimelineID + GetFrameRect rather than fall back.
    osv::premiere::mock::SequenceConfig config = f.host.sequence(kTimeline);
    prSetRect(&config.frameRect, 0, 0, 2000, 1000);
    f.host.setSequence(kTimeline, config);

    f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::MatchSequence));
    REQUIRE(f.render() == PF_Err_NONE);

    const Viewport expected = computeViewport(kW, kH, 2.0);
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());

    float inside[4];
    float outside[4];
    readPixelBgra32f(pixels, f.output->rowBytes(), kW / 2, expected.y + 1, inside);
    readPixelBgra32f(pixels, f.output->rowBytes(), kW / 2, expected.y - 2, outside);
    CHECK(inside[3] > 0.5f);
    CHECK(outside[3] == 0.0f);
    // And it is NOT the 16:9 fallback, which would put the boundary
    // elsewhere.
    const Viewport fallback = computeViewport(kW, kH, 16.0 / 9.0);
    CHECK(expected.h != fallback.h);
}

TEST_CASE("Match Sequence falls back to 16:9 with no Sequence Info Suite", "[reframe][render]") {
    constexpr int kW = 400;
    constexpr int kH = 400;
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);

    // Hide every version the effect tries; a host that cannot answer must
    // produce the documented fallback, not a failure.
    for (const int version : {9, 8, 7, 6, 5}) {
        f.host.setSuiteAvailable(kPrSDKSequenceInfoSuite, version, false);
    }

    f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::MatchSequence));
    REQUIRE(f.render() == PF_Err_NONE);

    const Viewport expected = computeViewport(kW, kH, 16.0 / 9.0);
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
    float inside[4];
    float outside[4];
    readPixelBgra32f(pixels, f.output->rowBytes(), kW / 2, expected.y + 1, inside);
    readPixelBgra32f(pixels, f.output->rowBytes(), kW / 2, expected.y - 2, outside);
    CHECK(inside[3] > 0.5f);
    CHECK(outside[3] == 0.0f);

    for (const int version : {9, 8, 7, 6, 5}) {
        f.host.setSuiteAvailable(kPrSDKSequenceInfoSuite, version, true);
    }
}

TEST_CASE("PF_Cmd_RENDER ACCEPTS an 8-bit input world", "[reframe][render]") {
    constexpr int kW = 160;
    constexpr int kH = 120;

    // GLOBAL_SETUP advertises PrPixelFormat_BGRA_4444_8u, so the host may
    // hand us one, and an effect that advertises a format must accept it.
    // This used to return PF_Err_BAD_CALLBACK_PARAM on the undocumented hope
    // that Premiere would renegotiate to 32f - and no test ever built an 8u
    // INPUT world, so the refusal was never exercised at all.
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f, 0x2000, PrPixelFormat_BGRA_4444_8u);
    f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
    f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat(kIndexFov, 90.0);
    f.setFloat(kIndexDistortion, 0.0);
    f.setAngle(kIndexPan, 30.0);
    REQUIRE(f.render() == PF_Err_NONE);

    // A real picture came out: the centre of a Full Frame render is inside
    // the viewport, so it is opaque and not uniformly black.
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
    float centre[4];
    readPixelBgra32f(pixels, f.output->rowBytes(), kW / 2, kH / 2, centre);
    CHECK(centre[3] > 0.5f);

    double sum = 0.0;
    int n = 0;
    for (int y = 4; y < kH - 4; y += 4) {
        for (int x = 4; x < kW - 4; x += 4) {
            float rgba[4];
            readPixelBgra32f(pixels, f.output->rowBytes(), x, y, rgba);
            sum += (static_cast<double>(rgba[0]) + rgba[1] + rgba[2]) / 3.0;
            ++n;
        }
    }
    REQUIRE(n > 0);
    INFO("mean of the 8u-input render: " << (sum / n));
    CHECK(sum / n > 1e-3);
}

TEST_CASE("an 8-bit input renders the same picture as the equivalent float input", "[reframe][render]") {
    constexpr int kW = 160;
    constexpr int kH = 120;

    // The fixture's 8u input is the 32f panorama quantised with the SAME rule
    // the promotion inverts (round(v * 255) then code / 255), so the promoted
    // frame differs from the float one only by that quantisation.  The two
    // renders must therefore agree to roughly one 8-bit step, which proves
    // the promotion reproduces the picture rather than merely producing one.
    auto renderWith = [](PrPixelFormat inputFormat) {
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f, 0x2000, inputFormat);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setAngle(kIndexPan, 30.0);
        REQUIRE(f.render() == PF_Err_NONE);
        std::vector<float> samples;
        const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
        for (int y = 4; y < kH - 4; y += 4) {
            for (int x = 4; x < kW - 4; x += 4) {
                float rgba[4];
                readPixelBgra32f(pixels, f.output->rowBytes(), x, y, rgba);
                for (const float c : rgba) {
                    samples.push_back(c);
                }
            }
        }
        return samples;
    };

    const std::vector<float> fromFloat = renderWith(PrPixelFormat_BGRA_4444_32f);
    const std::vector<float> from8u = renderWith(PrPixelFormat_BGRA_4444_8u);
    REQUIRE(fromFloat.size() == from8u.size());
    REQUIRE(!fromFloat.empty());

    double worst = 0.0;
    for (std::size_t i = 0; i < fromFloat.size(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(fromFloat[i]) - static_cast<double>(from8u[i])));
    }
    INFO("worst 8u-input vs 32f-input difference: " << worst);
    // One 8-bit step is 1/255; bilinear sampling can blend two quantised
    // neighbours, so allow a little over one step and no more.
    CHECK(worst <= 2.0 / 255.0);
}

TEST_CASE("PF_Cmd_RENDER into an 8-bit world matches the float render", "[reframe][render]") {
    constexpr int kW = 160;
    constexpr int kH = 120;

    auto renderInto = [](PrPixelFormat format) {
        RenderFixture f(kW, kH, format);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        f.setAngle(kIndexPan, 30.0);
        REQUIRE(f.render() == PF_Err_NONE);
        std::vector<float> samples;
        const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
        for (int y = 2; y < kH - 2; y += 4) {
            for (int x = 2; x < kW - 2; x += 4) {
                float rgba[4];
                if (format == PrPixelFormat_BGRA_4444_32f) {
                    readPixelBgra32f(pixels, f.output->rowBytes(), x, y, rgba);
                } else {
                    readPixelBgra8u(pixels, f.output->rowBytes(), x, y, rgba);
                }
                samples.insert(samples.end(), rgba, rgba + 4);
            }
        }
        return samples;
    };

    const std::vector<float> f32 = renderInto(PrPixelFormat_BGRA_4444_32f);
    const std::vector<float> u8 = renderInto(PrPixelFormat_BGRA_4444_8u);
    REQUIRE(f32.size() == u8.size());
    REQUIRE_FALSE(f32.empty());

    double worst = 0.0;
    for (std::size_t i = 0; i < f32.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(f32[i]) - static_cast<double>(u8[i])));
    }
    INFO("worst 8u vs 32f difference through the module: " << worst);
    CHECK(worst <= 1.0 / 255.0 + 1e-6);
}

TEST_CASE("Smooth Keyframes averages three DIFFERENT sampled angles", "[reframe][render]") {
    constexpr int kW = 161;
    constexpr int kH = 161;

    // The render sits at t = 10 with one frame = 2 time units, so the three
    // samples the effect takes are t = 8, 10 and 12.
    InDataSpec spec;
    spec.currentTime = 10;
    spec.timeStep = 2;
    spec.totalTime = 100;

    // Three distinct pan angles whose mean is a fourth, different value:
    // (10 + 40 + 100) / 3 = 50.  A sum would give 150, a median 40, and code
    // that ignored the neighbours 40 - all three are distinguishable from 50,
    // which is exactly what the old constant-value version of this test could
    // not tell apart.
    constexpr double kPrev = 10.0;
    constexpr double kCentre = 40.0;
    constexpr double kNext = 100.0;
    constexpr double kMean = (kPrev + kCentre + kNext) / 3.0;
    STATIC_REQUIRE(kMean == 50.0);

    const std::size_t frameBytes =
        static_cast<std::size_t>(kW) * 4u * sizeof(float) * static_cast<std::size_t>(kH);

    // ---- the reference: no smoothing, pan pinned at the MEAN -------------
    std::vector<std::uint8_t> reference;
    {
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setCheckbox(kIndexSmooth, false);
        f.setAngle(kIndexPan, kMean);
        REQUIRE(f.render(spec) == PF_Err_NONE);
        reference = copyWorld(*f.output, kH);
    }

    // ---- smoothing on, three real keyframes ------------------------------
    std::vector<std::uint8_t> smoothed;
    {
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setCheckbox(kIndexSmooth, true);
        // The centre value is also the fallback the effect uses if a
        // neighbouring checkout fails, so a failed checkout would show up as
        // a pan of 40 or 50 rather than silently passing.
        f.setAngle(kIndexPan, kCentre);
        f.setAngleAtTime(kIndexPan, 8, kPrev);
        f.setAngleAtTime(kIndexPan, 10, kCentre);
        f.setAngleAtTime(kIndexPan, 12, kNext);
        REQUIRE(f.render(spec) == PF_Err_NONE);
        smoothed = copyWorld(*f.output, kH);
    }

    REQUIRE(reference.size() == smoothed.size());
    REQUIRE(reference.size() >= frameBytes);
    CHECK(std::memcmp(reference.data(), smoothed.data(), reference.size()) == 0);

    // ---- and the guard against a vacuous pass ----------------------------
    // If the effect ignored the neighbours entirely it would render the
    // centre value, so prove a centre-only render is DIFFERENT from the mean
    // render.  Without this the memcmp above could pass for the wrong reason.
    std::vector<std::uint8_t> centreOnly;
    {
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setCheckbox(kIndexSmooth, false);
        f.setAngle(kIndexPan, kCentre);
        REQUIRE(f.render(spec) == PF_Err_NONE);
        centreOnly = copyWorld(*f.output, kH);
    }
    CHECK(std::memcmp(reference.data(), centreOnly.data(), reference.size()) != 0);
}

TEST_CASE("Smooth Keyframes at the clip start falls back to the centre sample", "[reframe][render]") {
    constexpr int kW = 97;
    constexpr int kH = 97;

    // current_time 0 with a step of 2: t-1 is -2, which is BEFORE the clip.
    // The effect must use the centre value for that sample rather than
    // checking out a negative time (checkoutAngle returns the fallback when
    // time < 0), so the average becomes (centre + centre + next) / 3.
    InDataSpec spec;
    spec.currentTime = 0;
    spec.timeStep = 2;

    constexpr double kCentre = 30.0;
    constexpr double kNext = 90.0;
    constexpr double kExpected = (kCentre + kCentre + kNext) / 3.0;  // 50

    std::vector<std::uint8_t> reference;
    {
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setCheckbox(kIndexSmooth, false);
        f.setAngle(kIndexPan, kExpected);
        REQUIRE(f.render(spec) == PF_Err_NONE);
        reference = copyWorld(*f.output, kH);
    }

    std::vector<std::uint8_t> smoothed;
    {
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setCheckbox(kIndexSmooth, true);
        f.setAngle(kIndexPan, kCentre);
        f.setAngleAtTime(kIndexPan, 0, kCentre);
        f.setAngleAtTime(kIndexPan, 2, kNext);
        REQUIRE(f.render(spec) == PF_Err_NONE);
        smoothed = copyWorld(*f.output, kH);
    }

    REQUIRE(reference.size() == smoothed.size());
    CHECK(std::memcmp(reference.data(), smoothed.data(), reference.size()) == 0);
}

TEST_CASE("PF_Cmd_RENDER rejects a call with no worlds instead of crashing", "[reframe][render]") {
    MockHost host;
    PF_ProgPtr ref = host.createEffectRef(0x3000, 5);
    REQUIRE(ref != nullptr);
    PF_InData in = host.makeInData(ref, {});
    PF_OutData out = host.makeOutData();

    // Premiere should never do this, but a null output world must produce an
    // error return, not a fault inside the host's render thread.
    CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_RENDER, &in, &out, nullptr, nullptr, nullptr) !=
          PF_Err_NONE);
    host.destroyEffectRef(ref);
}

TEST_CASE("the presets produce visibly different framings", "[reframe][render]") {
    constexpr int kW = 161;
    constexpr int kH = 161;

    // Each preset is applied the way the UI applies it (through
    // USER_CHANGED_PARAM), then rendered; the results must differ, which is
    // what tells us the preset table actually reaches the geometry.
    std::vector<std::vector<std::uint8_t>> frames;
    for (const PresetEntry& entry : kPresetTable) {
        if (!entry.writesControls) {
            continue;
        }
        INFO("preset '" << entry.label << "'");
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
        f.setPopup(kIndexOutputAspect, static_cast<int>(Aspect::FullFrame));
        // The supervised handler writes FOV / Distortion / Tilt for us.
        f.setPopup(kIndexPreset, static_cast<int>(entry.value));
        f.setFloat(kIndexFov, entry.fovDeg);
        f.setFloat(kIndexDistortion, entry.distortion);
        f.setAngle(kIndexTilt, entry.tiltDeg);
        REQUIRE(f.render() == PF_Err_NONE);

        const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
        frames.emplace_back(pixels, pixels + static_cast<std::size_t>(f.output->rowBytes()) *
                                                 static_cast<std::size_t>(kH));
    }
    REQUIRE(frames.size() >= 4);

    for (std::size_t i = 0; i < frames.size(); ++i) {
        for (std::size_t j = i + 1; j < frames.size(); ++j) {
            INFO("presets " << i << " and " << j);
            CHECK(frames[i] != frames[j]);
        }
    }
}

// ===========================================================================
//  matchHostParams - the runtime host parameter probe
//
//  This is the regression cover for the blocker that made every effect
//  control do nothing: Premiere's VideoSegmentSuite::GetParam index space is
//  NOT the AE parameter list.  The effect adds 15 parameters (11 controls +
//  4 group markers) and the host's GetParamCount answers 8, so the static
//  "AE index - 1" rule read the wrong control for everything after the first
//  group - FOV came back as garbage and the render bailed out.
//
//  The matcher is a pure function over a list of PrParam TYPES, so it can be
//  driven here with a synthetic host list and no Adobe suite at all.
// ===========================================================================
namespace {

/// The signature of our eleven value-carrying controls, in add order.
/// Written out literally rather than copied from kValueParamKind so the test
/// would notice a change to that table instead of following it.
const std::vector<HostParamKind> kFullSignature = {
    HostParamKind::Int32,    // Output Aspect
    HostParamKind::Int32,    // Preset
    HostParamKind::Float32,  // Pan
    HostParamKind::Float32,  // Tilt
    HostParamKind::Float32,  // Roll
    HostParamKind::Float64,  // FOV
    HostParamKind::Float64,  // Distortion
    HostParamKind::Float32,  // Source Pan
    HostParamKind::Float32,  // Source Tilt
    HostParamKind::Float32,  // Source Roll
    HostParamKind::Bool,     // Smooth Keyframes
};

/// What Premiere Pro 26.2 actually reports: the same list with the three
/// Source angles - the contents of the START_COLLAPSED group - missing.
const std::vector<HostParamKind> kPremiereSignature = {
    HostParamKind::Int32,   HostParamKind::Int32,   HostParamKind::Float32, HostParamKind::Float32,
    HostParamKind::Float32, HostParamKind::Float64, HostParamKind::Float64, HostParamKind::Bool,
};

/// Run the matcher over a vector, for brevity at the call sites.
[[nodiscard]] bool matchKinds(const std::vector<HostParamKind>& kinds, HostParamMap* out) {
    return matchHostParams(kinds.data(), static_cast<int>(kinds.size()), out);
}

}  // namespace

TEST_CASE("matchHostParams maps an unreduced host list one to one", "[reframe][params]") {
    HostParamMap map{};
    REQUIRE(matchKinds(kFullSignature, &map));
    CHECK(map.probed);

    // Every control present, in order, with no gap.
    CHECK(map[kIndexOutputAspect] == 0);
    CHECK(map[kIndexPreset] == 1);
    CHECK(map[kIndexPan] == 2);
    CHECK(map[kIndexTilt] == 3);
    CHECK(map[kIndexRoll] == 4);
    CHECK(map[kIndexFov] == 5);
    CHECK(map[kIndexDistortion] == 6);
    CHECK(map[kIndexSourcePan] == 7);
    CHECK(map[kIndexSourceTilt] == 8);
    CHECK(map[kIndexSourceRoll] == 9);
    CHECK(map[kIndexSmooth] == 10);

    // The four group markers carry no value and must never be addressed.
    CHECK(map[kIndexCameraTopic] == -1);
    CHECK(map[kIndexCameraTopicEnd] == -1);
    CHECK(map[kIndexSourceTopic] == -1);
    CHECK(map[kIndexSourceTopicEnd] == -1);
}

TEST_CASE("matchHostParams recovers the real Premiere mapping from 8 entries", "[reframe][params]") {
    // THE regression test.  A host that reports 8 parameters, with the group
    // markers absent and the collapsed group's contents hidden, must still
    // yield the right host index for every control it DOES expose.
    REQUIRE(kPremiereSignature.size() == 8u);

    HostParamMap map{};
    REQUIRE(matchKinds(kPremiereSignature, &map));
    CHECK(map.probed);

    // The seven controls before the gap keep their positions...
    CHECK(map[kIndexOutputAspect] == 0);
    CHECK(map[kIndexPreset] == 1);
    CHECK(map[kIndexPan] == 2);
    CHECK(map[kIndexTilt] == 3);
    CHECK(map[kIndexRoll] == 4);
    CHECK(map[kIndexFov] == 5);
    CHECK(map[kIndexDistortion] == 6);
    // ...the three hidden ones map to nothing...
    CHECK(map[kIndexSourcePan] == -1);
    CHECK(map[kIndexSourceTilt] == -1);
    CHECK(map[kIndexSourceRoll] == -1);
    // ...and Smooth Keyframes closes the list at 7, NOT at the 14 the static
    // rule would have used.  Those two numbers are the whole bug: under the
    // old mapping FOV was read from host index 6 - which is Distortion - and
    // Smooth from index 14, which does not exist.
    CHECK(map[kIndexSmooth] == 7);
    CHECK(map[kIndexSmooth] != gpuParamIndex(kIndexSmooth));
    CHECK(map[kIndexFov] != gpuParamIndex(kIndexFov));
}

TEST_CASE("matchHostParams refuses lists it cannot identify", "[reframe][params]") {
    HostParamMap map{};

    SECTION("a null list or a nonsense count") {
        CHECK_FALSE(matchHostParams(nullptr, 8, &map));
        CHECK_FALSE(matchKinds(kFullSignature, nullptr));
        CHECK_FALSE(matchHostParams(kFullSignature.data(), 0, &map));
        CHECK_FALSE(matchHostParams(kFullSignature.data(), -3, &map));
    }

    SECTION("a list longer than our own cannot be our parameter set") {
        std::vector<HostParamKind> tooLong = kFullSignature;
        tooLong.push_back(HostParamKind::Int32);
        CHECK_FALSE(matchKinds(tooLong, &map));
    }

    SECTION("an entry the host refused matches nothing") {
        // Blanking FOV's slot makes the alignment impossible rather than
        // letting a neighbouring control slide into its place.
        std::vector<HostParamKind> withHole = kFullSignature;
        withHole[5] = HostParamKind::Unknown;
        CHECK_FALSE(matchKinds(withHole, &map));
    }

    SECTION("a list of the right length but the wrong types") {
        std::vector<HostParamKind> wrong(kFullSignature.size(), HostParamKind::Int32);
        CHECK_FALSE(matchKinds(wrong, &map));
    }

    SECTION("a gap that could be placed two ways is ambiguous") {
        // Eight entries of nothing but angles: our signature's three-long gap
        // could sit in several places, so there is no single right answer and
        // the matcher must decline rather than pick one.
        const std::vector<HostParamKind> allAngles(8, HostParamKind::Float32);
        CHECK_FALSE(matchKinds(allAngles, &map));
    }

    SECTION("the right length with one type changed is not our list") {
        // Mapping onto it would mean reading a control that is not ours.
        std::vector<HostParamKind> trailing = kFullSignature;
        trailing.back() = HostParamKind::Int32;
        CHECK_FALSE(matchKinds(trailing, &map));
    }
}

TEST_CASE("HostParamMap::setStatic reproduces the documented rule", "[reframe][params]") {
    // The fallback must stay exactly what the SDK sample does, so a host the
    // probe cannot identify behaves as it did before the probe existed.
    HostParamMap map{};
    map.setStatic();
    CHECK_FALSE(map.probed);
    CHECK(map[kIndexOutputAspect] == gpuParamIndex(kIndexOutputAspect));
    CHECK(map[kIndexFov] == gpuParamIndex(kIndexFov));
    CHECK(map[kIndexSmooth] == gpuParamIndex(kIndexSmooth));
    // The input layer is never read through GetParam.
    CHECK(map[0] == -1);
    // Out-of-range subscripts are answered, not dereferenced.
    CHECK(map[-1] == -1);
    CHECK(map[OSV_REFRAME_PARAM_COUNT + 1] == -1);
}

// ===========================================================================
//  buildParams degrades rather than dying on a bad field of view
// ===========================================================================
TEST_CASE("buildParams clamps a hostile field of view instead of failing", "[reframe][geometry]") {
    // The other half of the blocker: once a bad GetParam read put garbage in
    // FOV, buildParams() returned an invalid setup and PF_Cmd_RENDER logged
    // "could not build the kernel parameters" and drew nothing.  A wrong but
    // visible frame is strictly better than a dead effect, so every finite
    // value now renders.
    PackedSource src(panorama(), panorama().width * 16);

    const double hostile = GENERATE(-1.0e9, -180.0, 0.0, 1.0e-9, 359.999, 1.0e9);
    INFO("field of view " << hostile);
    Settings s;
    s.aspect = Aspect::FullFrame;
    s.fovDeg = hostile;
    const KernelSetup setup = buildParams(s, src.view, 320, 180, 0.0);
    CHECK(setup.valid);
    CHECK(std::isfinite(setup.params.focalPx));
    CHECK(setup.params.focalPx > 0.0f);
}

TEST_CASE("buildParams survives a non-finite field of view", "[reframe][geometry]") {
    // NaN and infinity are replaced by the DEFAULT before the clamp, so these
    // render rather than failing - that is deliberate, and pinning it here
    // stops it regressing back into a black frame.
    PackedSource src(panorama(), panorama().width * 16);

    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()}) {
        Settings s;
        s.aspect = Aspect::FullFrame;
        s.fovDeg = bad;
        const KernelSetup setup = buildParams(s, src.view, 320, 180, 0.0);
        CHECK(setup.valid);
        CHECK(std::isfinite(setup.params.focalPx));
        CHECK(setup.params.focalPx > 0.0f);
    }
}
