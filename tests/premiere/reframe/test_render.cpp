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

#include "osv/geom/VirtualCamera.h"

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

/// The resolution entry the geometry tests render with: FOV spans the width
/// of the frame being rendered, and nothing is cropped.
///
/// These tests used to select "Full Frame", an aspect entry that meant "the
/// frame's own shape".  That entry is gone with the letterbox; its exact
/// equivalent now is "Match Sequence" whenever no sequence size is known,
/// because resolveOutputSize() then falls back to the frame itself and the
/// cover-fit factor is exactly 1 (a test below proves both halves of that).
/// Every buildParams() call here passes SizePx{} for the sequence, and the
/// RenderFixture timelines are never given a SequenceConfig, so the mock's
/// GetFrameRect answers suiteError_IDNotValid and the effect takes the same
/// fallback.  A test that DOES configure a sequence says so explicitly.
constexpr Resolution kFillFrame = Resolution::MatchSequence;

/// Default settings: the picture fills the output and a pixel's position maps
/// straight to a direction, with no cover-fit crop to account for.
Settings baseSettings() {
    Settings s;
    s.resolution = kFillFrame;
    s.preset = Preset::Custom;
    s.fovDeg = 90.0;
    s.distortion = 0.0;  // rectilinear: the easiest geometry to reason about
    return s;
}

}  // namespace

// ===========================================================================
//  The geometry helpers (pure functions in ReframeParams.h / ReframeCpu.cpp)
// ===========================================================================
TEST_CASE("resolveOutputSize answers the documented sizes", "[reframe][geometry]") {
    const SizePx seq{2000, 1000};
    const SizePx frame{640, 480};

    SECTION("fixed entries ignore both the sequence and the frame") {
        // Every fixed entry is exactly the pair of numbers its label spells,
        // whatever the host reports, including when it reports nothing.
        for (const ResolutionEntry& e : kResolutions) {
            if (e.value == Resolution::MatchSequence) {
                continue;
            }
            INFO("resolution '" << e.label << "'");
            for (const SizePx sequence : {seq, SizePx{}, SizePx{-5, 7}}) {
                for (const SizePx f : {frame, SizePx{}}) {
                    const SizePx r = resolveOutputSize(e.value, sequence, f);
                    CHECK(r.w == e.width);
                    CHECK(r.h == e.height);
                }
            }
        }
        CHECK(resolveOutputSize(Resolution::Uhd3840x2160, seq, frame).w == 3840);
        CHECK(resolveOutputSize(Resolution::Uhd3840x2160, seq, frame).h == 2160);
        CHECK(resolveOutputSize(Resolution::Hd1280x720, seq, frame).w == 1280);
        CHECK(resolveOutputSize(Resolution::Hd1280x720, seq, frame).h == 720);
    }

    SECTION("Match Sequence uses the sequence when there is one") {
        const SizePx r = resolveOutputSize(Resolution::MatchSequence, seq, frame);
        CHECK(r.w == 2000);
        CHECK(r.h == 1000);
    }

    SECTION("Match Sequence falls back to the frame, never to a guess") {
        // The old aspect control fell back to a fixed 16:9.  That was a shape
        // and a shape was all it could use; a size needs pixels, and when the
        // sequence cannot be asked the frame the host allocated for THIS
        // render is the exact answer, so it replaced the constant.
        //
        // A partially valid rectangle (one edge zero or negative) is not a
        // size and must take the fallback too, rather than half of it being
        // mixed with half of the frame.
        for (const SizePx bad : {SizePx{}, SizePx{0, 1080}, SizePx{1920, 0}, SizePx{-1920, 1080}, SizePx{1920, -1}}) {
            INFO("sequence " << bad.w << "x" << bad.h);
            const SizePx r = resolveOutputSize(Resolution::MatchSequence, bad, frame);
            CHECK(r.w == 640);
            CHECK(r.h == 480);
        }
    }

    SECTION("with nothing to go on the answer is invalid, not invented") {
        CHECK_FALSE(resolveOutputSize(Resolution::MatchSequence, SizePx{}, SizePx{}).valid());
    }

    SECTION("a corrupt popup value is sanitised onto Match Sequence") {
        // Includes the values a project saved against the OLD eight-entry
        // aspect table can still hold (6, 7, 8): they must land on a valid
        // entry, never index past the table.
        for (const int raw : {0, -1, 6, 7, 8, 1000, std::numeric_limits<int>::min()}) {
            INFO("popup value " << raw);
            CHECK(sanitiseResolution(raw) == Resolution::MatchSequence);
        }
        for (int raw = 1; raw <= OSV_REFRAME_RESOLUTION_COUNT; ++raw) {
            CHECK(static_cast<int>(sanitiseResolution(raw)) == raw);
        }
    }
}

TEST_CASE("computeViewport always covers the whole frame", "[reframe][geometry]") {
    // The letterbox is gone ON PURPOSE: a virtual camera fills its sensor, so
    // the painted rectangle is the frame for every size and shape.  (The old
    // expectations - a centred 2.35:1 box with bars, a pillarboxed 9:16 box -
    // described exactly the behaviour the user asked to remove.)
    for (const SizePx frame : {SizePx{1920, 1080}, SizePx{1080, 1920}, SizePx{400, 400}, SizePx{1, 1},
                               SizePx{6000, 3000}, SizePx{321, 241}}) {
        INFO("frame " << frame.w << "x" << frame.h);
        const Viewport v = computeViewport(frame.w, frame.h);
        CHECK(v.x == 0);
        CHECK(v.y == 0);
        CHECK(v.w == frame.w);
        CHECK(v.h == frame.h);
    }

    // A degenerate frame yields a zero-sized viewport, which buildParams
    // treats as invalid rather than dividing by.
    for (const SizePx bad : {SizePx{0, 0}, SizePx{0, 480}, SizePx{640, 0}, SizePx{-640, 480}}) {
        const Viewport empty = computeViewport(bad.w, bad.h);
        CHECK(empty.w == 0);
        CHECK(empty.h == 0);
    }
}

// ===========================================================================
//  The automatic projection ramp
// ===========================================================================
TEST_CASE("autoEyeOffsetForFov is a smoothstep from 120 to 240 degrees", "[reframe][geometry][autoproj]") {
    SECTION("ordinary shots are untouched") {
        // At and below the start the ramp contributes exactly nothing, so a
        // rectilinear Dewarping shot stays rectilinear.
        for (const double fov : {10.0, 30.0, 60.0, 95.0, 119.999, 120.0}) {
            INFO("fov " << fov);
            CHECK(autoEyeOffsetForFov(fov) == 0.0);
        }
    }

    SECTION("wide shots reach full stereographic") {
        for (const double fov : {240.0, 240.001, 300.0, 350.0, 1.0e9}) {
            INFO("fov " << fov);
            CHECK(autoEyeOffsetForFov(fov) == 1.0);
        }
    }

    SECTION("the curve is the documented smoothstep") {
        // t = (fov - 120) / 120, d = t^2 (3 - 2t).
        for (const double fov : {130.0, 150.0, 180.0, 200.0, 230.0}) {
            const double t = (fov - OSV_REFRAME_AUTO_EYE_FOV_START) /
                             (OSV_REFRAME_AUTO_EYE_FOV_FULL - OSV_REFRAME_AUTO_EYE_FOV_START);
            INFO("fov " << fov);
            CHECK(autoEyeOffsetForFov(fov) == Approx(t * t * (3.0 - 2.0 * t)).margin(1e-12));
        }
        // Symmetric about the midpoint, which is exactly one half.
        CHECK(autoEyeOffsetForFov(180.0) == Approx(0.5).margin(1e-12));
    }

    SECTION("it is monotonic and continuous - no pop anywhere in the slider range") {
        // Walk the whole valid range in 0.01 degree steps.  Continuity is
        // checked as a Lipschitz bound: smoothstep's steepest slope is
        // 1.5 / 120 per degree (at the midpoint), so no 0.01 degree step may
        // move d by more than 1.25e-4, plus a hair of slack for rounding.
        double previous = autoEyeOffsetForFov(OSV_REFRAME_FOV_VALID_MIN);
        double worstStep = 0.0;
        for (double fov = OSV_REFRAME_FOV_VALID_MIN + 0.01; fov <= OSV_REFRAME_FOV_VALID_MAX; fov += 0.01) {
            const double d = autoEyeOffsetForFov(fov);
            REQUIRE(d >= previous);  // never ramps backwards
            worstStep = std::max(worstStep, d - previous);
            previous = d;
        }
        INFO("largest change of d over a 0.01 degree step: " << worstStep);
        CHECK(worstStep < 1.26e-4);
    }

    SECTION("it eases in and out: zero slope at both joins") {
        // This is the property that makes the joins invisible.  A linear ramp
        // would change d at 1/120 per degree the instant FOV crossed 120; the
        // smoothstep's slope there is zero, so the first tenth of a degree
        // past each join moves d by a vanishing amount.
        const double justPastStart = autoEyeOffsetForFov(OSV_REFRAME_AUTO_EYE_FOV_START + 0.1);
        const double justBeforeFull = autoEyeOffsetForFov(OSV_REFRAME_AUTO_EYE_FOV_FULL - 0.1);
        CHECK(justPastStart < 3.0e-6);  // a linear ramp would give 8.3e-4
        CHECK(1.0 - justBeforeFull < 3.0e-6);
    }

    SECTION("garbage gives the identity, not a NaN") {
        CHECK(autoEyeOffsetForFov(std::nan("")) == 0.0);
        CHECK(autoEyeOffsetForFov(std::numeric_limits<double>::infinity()) == 0.0);
        CHECK(autoEyeOffsetForFov(-std::numeric_limits<double>::infinity()) == 0.0);
    }
}

TEST_CASE("effectiveEyeOffset lets the user exceed the ramp but never undercut it", "[reframe][geometry][autoproj]") {
    SECTION("below the ramp the slider is used verbatim") {
        CHECK(effectiveEyeOffset(0.0, 90.0) == 0.0);
        CHECK(effectiveEyeOffset(15.0, 120.0) == Approx(0.15));
        CHECK(effectiveEyeOffset(100.0, 60.0) == Approx(1.0));
    }

    SECTION("a user who never touches Distortion still gets stereographic when zoomed out") {
        CHECK(effectiveEyeOffset(0.0, 180.0) == Approx(0.5));
        CHECK(effectiveEyeOffset(0.0, 240.0) == 1.0);
        CHECK(effectiveEyeOffset(0.0, 300.0) == 1.0);
    }

    SECTION("asking for MORE than the ramp always wins") {
        // 40% at 150 deg: the ramp only wants ~0.156 there.
        CHECK(effectiveEyeOffset(40.0, 150.0) == Approx(0.4));
        CHECK(effectiveEyeOffset(100.0, 130.0) == Approx(1.0));
    }

    SECTION("every preset renders at exactly the distortion it writes") {
        // The presets were tuned by eye before the ramp existed.  With a
        // MAXIMUM, none of them is overridden: each one's own distortion is
        // already at or above the ramp's floor at its own FOV.
        for (const PresetEntry& e : kPresetTable) {
            if (!e.writesControls) {
                continue;
            }
            INFO("preset '" << e.label << "' fov " << e.fovDeg << " distortion " << e.distortion);
            CHECK(effectiveEyeOffset(e.distortion, e.fovDeg) == Approx(e.distortion / 100.0).margin(1e-12));
        }
    }

    SECTION("the result is always a valid eye offset") {
        const double nan = std::nan("");
        const double inf = std::numeric_limits<double>::infinity();
        for (const double dist : {nan, inf, -inf, -50.0, 0.0, 50.0, 100.0, 250.0}) {
            for (const double fov : {nan, inf, -inf, -10.0, 0.0, 90.0, 180.0, 350.0, 1.0e6}) {
                INFO("distortion " << dist << " fov " << fov);
                const double d = effectiveEyeOffset(dist, fov);
                CHECK(std::isfinite(d));
                CHECK(d >= 0.0);
                CHECK(d <= 1.0);
            }
        }
    }
}

TEST_CASE("the ramp keeps every slider FOV renderable without the invertibility clamp",
          "[reframe][geometry][autoproj]") {
    // The eye-offset model can only be inverted below 2 acos(-d), and
    // VirtualCamera silently CLAMPS a wider request - so before the ramp, a
    // user at Distortion 0 who dragged FOV past 179 degrees got 179 degrees,
    // however far they dragged.  With the ramp as a floor on d, the requested
    // field of view is the RENDERED field of view across the whole valid
    // range, for a user who never touches Distortion at all.
    for (double fov = OSV_REFRAME_FOV_VALID_MIN; fov <= OSV_REFRAME_FOV_VALID_MAX; fov += 0.5) {
        osv::geom::VirtualCamera camera;
        camera.projection = osv::geom::Projection::EyeOffset;
        camera.w = 1920;
        camera.h = 1080;
        camera.hfovDeg = fov;
        camera.eyeOffset = effectiveEyeOffset(0.0, fov);
        INFO("fov " << fov << " d " << camera.eyeOffset);
        CHECK(camera.effectiveHfovDeg() == Approx(fov).margin(1e-9));
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
        CHECK_FALSE(buildParams(s, bad, 640, 360, SizePx{}).valid);
    }
    SECTION("a zero-sized source") {
        ConstFrameView bad = src.view;
        bad.width = 0;
        CHECK_FALSE(buildParams(s, bad, 640, 360, SizePx{}).valid);
    }
    SECTION("a pitch too small for one row") {
        ConstFrameView bad = src.view;
        bad.rowBytes = 16;  // one pixel, not 1024
        CHECK_FALSE(buildParams(s, bad, 640, 360, SizePx{}).valid);
    }
    SECTION("an 8-bit source (the kernel's sampler reads float or half only)") {
        ConstFrameView bad = src.view;
        bad.layout = PixelLayout::Bgra8u;
        CHECK_FALSE(buildParams(s, bad, 640, 360, SizePx{}).valid);
    }
    SECTION("a zero-sized output") {
        CHECK_FALSE(buildParams(s, src.view, 0, 360, SizePx{}).valid);
        CHECK_FALSE(buildParams(s, src.view, 640, 0, SizePx{}).valid);
    }
    SECTION("an absurd output size") {
        CHECK_FALSE(buildParams(s, src.view, 1 << 20, 360, SizePx{}).valid);
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
        const KernelSetup setup = buildParams(baseSettings(), v, 320, 180, SizePx{});
        REQUIRE(setup.valid);
        CHECK(setup.sourceRow0 == first);
        CHECK(setup.source.pitchBytes == pitch);
    }

    SECTION("bottom-up with a negative pitch is the same frame described backwards") {
        // base points at the LAST row in memory and the rows march backwards,
        // so image row 0 is at `last` and image row 1 is at `last + pitch`.
        const ConstFrameView v = makeView(last, -pitch, false);
        CHECK(sourceRowsRunForward(v));
        const KernelSetup setup = buildParams(baseSettings(), v, 320, 180, SizePx{});
        REQUIRE(setup.valid);
        // ...which is byte for byte the top-down description above.
        CHECK(setup.sourceRow0 == first);
        CHECK(setup.source.pitchBytes == pitch);
    }

    SECTION("the two mirrored arrangements are accepted through flipY") {
        // These two store the image the other way up in memory.  They used to
        // be REFUSED, which meant the effect rendered NOTHING for frames
        // Premiere legitimately hands out - a session log shows a top-down
        // world with rowBytes = -40960 rejected on every single frame, which
        // is why no control appeared to do anything.
        //
        // They are now described to the kernel by pointing at the far end of
        // the buffer and setting flipY, so every byte offset stays
        // non-negative (the sampler indexes rows with an unsigned multiply).

        // Top-down base with rows running backwards.
        const ConstFrameView backwards = makeView(last, -pitch, true);
        CHECK_FALSE(sourceRowsRunForward(backwards));
        const KernelSetup bw = buildParams(baseSettings(), backwards, 320, 180, SizePx{});
        REQUIRE(bw.valid);
        CHECK(bw.source.flipY == 1);
        CHECK(bw.source.pitchBytes == pitch);
        // The kernel must start from the row at the LOWEST address, which for
        // this arrangement is the last image row.
        CHECK(bw.sourceRow0 == backwards.constRowTopDown(backwards.height - 1));

        // Bottom-up storage with a forward pitch.
        const ConstFrameView upsideDown = makeView(first, pitch, false);
        CHECK_FALSE(sourceRowsRunForward(upsideDown));
        const KernelSetup ud = buildParams(baseSettings(), upsideDown, 320, 180, SizePx{});
        REQUIRE(ud.valid);
        CHECK(ud.source.flipY == 1);
        CHECK(ud.source.pitchBytes == pitch);
        CHECK(ud.sourceRow0 == upsideDown.constRowTopDown(upsideDown.height - 1));
    }

    SECTION("the flipped arrangements render the mirrored picture, not nothing") {
        // Honest limitation, stated rather than papered over.
        //
        // flipY re-addresses the ROWS so every byte offset stays
        // non-negative, which is what lets these frames render at all - they
        // used to be refused outright, and that is why the effect appeared to
        // ignore every control on a host that hands out such worlds.
        //
        // What it does NOT do is mirror the PIXELS.  For these two
        // arrangements image row 0 sits at the highest address, so walking
        // rows in image order walks memory backwards; flipY makes that walk
        // legal, and the sampler then reads row 0 of the image from what is
        // physically the last row of the buffer.  The result is the panorama
        // flipped top to bottom.
        //
        // That is a deliberate trade: a vertically mirrored picture is a
        // visible, reportable bug, whereas refusing the frame renders NOTHING
        // and looks like a dead effect.  If a real host is ever confirmed to
        // hand us one of these, the fix is to flip the OUTPUT row index in
        // renderCpu, not to refuse the frame.
        const ConstFrameView upright = makeView(first, pitch, true);
        const ConstFrameView flipped = makeView(last, -pitch, true);
        const KernelSetup up = buildParams(baseSettings(), upright, 128, 96, SizePx{});
        const KernelSetup fl = buildParams(baseSettings(), flipped, 128, 96, SizePx{});
        REQUIRE(up.valid);
        REQUIRE(fl.valid);
        CHECK(up.source.flipY == 0);
        CHECK(fl.source.flipY == 1);

        // Both produce a real picture: finite, in range, and not all zero.
        int nonZero = 0;
        for (int y = 0; y < 96; y += 7) {
            for (int x = 0; x < 128; x += 7) {
                float pf[4];
                REQUIRE(renderPixel(fl, flipped, x, y, pf));
                for (int c = 0; c < 4; ++c) {
                    CHECK(std::isfinite(pf[c]));
                    CHECK(pf[c] >= 0.0f);
                    CHECK(pf[c] <= 1.0f);
                }
                if (pf[0] > 0.0f || pf[1] > 0.0f || pf[2] > 0.0f) {
                    ++nonZero;
                }
            }
        }
        CHECK(nonZero > 0);

        // And the two are genuinely different pictures, which is the
        // mirroring being asserted rather than assumed.  Comparing a SINGLE
        // pixel is not enough: a reframe of a mostly-sky panorama has plenty
        // of rows where the flipped and upright samples coincide (the first
        // attempt at this check picked one, read 0.999983 from both and
        // "passed" for the wrong reason).  So the whole sampled grid is
        // compared and at least one pixel must differ.
        int differing = 0;
        for (int y = 0; y < 96; y += 7) {
            for (int x = 0; x < 128; x += 7) {
                float a[4];
                float b[4];
                REQUIRE(renderPixel(up, upright, x, y, a));
                REQUIRE(renderPixel(fl, flipped, x, y, b));
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) {
                    ++differing;
                }
            }
        }
        INFO("pixels differing between the upright and flipped renders: " << differing);
        CHECK(differing > 0);
    }

    SECTION("a one-row frame has no stride to get wrong") {
        ConstFrameView v = makeView(first, pitch, false);
        v.height = 1;
        CHECK(sourceRowsRunForward(v));
    }

    SECTION("the two accepted descriptions render identically") {
        const KernelSetup a = buildParams(baseSettings(), makeView(first, pitch, true), 128, 96, SizePx{});
        const KernelSetup b = buildParams(baseSettings(), makeView(last, -pitch, false), 128, 96, SizePx{});
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
        const KernelSetup setup = buildParams(s, src.view, 320, 180, SizePx{});
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
        const KernelSetup a = buildParams(s, src.view, 320, 180, SizePx{});
        s.tiltDeg = 90.0;
        const KernelSetup b = buildParams(s, src.view, 320, 180, SizePx{});
        REQUIRE(a.valid);
        REQUIRE(b.valid);
        // A tilt past the pole is the same camera as a tilt at the pole.
        for (int i = 0; i < 9; ++i) {
            CHECK(a.params.Rout[i] == Approx(b.params.Rout[i]).margin(1e-6));
        }
    }

    SECTION("an extreme field of view at Distortion 0 renders instead of degenerating") {
        // This used to be a test of the invertibility CLAMP: at d = 0 the
        // model degenerates at 180 degrees, so a 350 degree request was cut
        // back to 179.  The automatic ramp changed the premise on purpose -
        // Distortion 0 no longer means d = 0 once FOV passes 120 degrees; at
        // 350 the ramp has reached d = 1, which is invertible up to 359.  So
        // the camera must now be finite AND render the full 350 degrees:
        // its focal length is the unclamped eye-offset focal at d = 1,
        //     f = (W/2) (d + cos(fov/2)) / ((1 + d) sin(fov/2)).
        Settings s = baseSettings();
        s.distortion = 0.0;
        s.fovDeg = 350.0;
        const KernelSetup setup = buildParams(s, src.view, 320, 180, SizePx{});
        REQUIRE(setup.valid);
        CHECK(std::isfinite(setup.params.focalPx));
        CHECK(setup.params.focalPx > 0.0f);
        CHECK(setup.params.eyeOffset == 1.0f);
        const double half = 0.5 * 350.0 * 3.14159265358979323846 / 180.0;
        const double expectedFocal = 160.0 * (1.0 + std::cos(half)) / (2.0 * std::sin(half));
        CHECK(setup.params.focalPx == Approx(expectedFocal).epsilon(1e-5));
    }

    SECTION("the eye offset handed to the kernel is effectiveEyeOffset()") {
        // Distortion and FOV reach the kernel through exactly one function,
        // so the CPU path, the GPU path (which shares buildParams) and the
        // tests above cannot disagree about the curve.
        for (const double fov : {60.0, 120.0, 150.0, 180.0, 239.0, 300.0}) {
            for (const double dist : {0.0, 15.0, 60.0, 100.0}) {
                Settings s = baseSettings();
                s.fovDeg = fov;
                s.distortion = dist;
                const KernelSetup setup = buildParams(s, src.view, 320, 180, SizePx{});
                INFO("fov " << fov << " distortion " << dist);
                REQUIRE(setup.valid);
                CHECK(setup.params.eyeOffset == static_cast<float>(effectiveEyeOffset(dist, fov)));
            }
        }
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
    // A 16:9 resolution on the 4:3 frame below.  This used to be a 16:9
    // LETTERBOX, which made the anchor cover transparent-bar pixels too; with
    // the letterbox gone the same shape mismatch now exercises the cover-fit
    // (a focal length scaled by 4/3), so the anchor still covers the
    // non-trivial geometry path rather than only the identity.
    s.resolution = Resolution::Fhd1920x1080;
    s.panDeg = 37.0;
    s.tiltDeg = -12.0;
    s.rollDeg = 5.0;
    s.fovDeg = 110.0;
    s.distortion = 25.0;

    constexpr int kW = 320;
    constexpr int kH = 240;
    const KernelSetup setup = buildParams(s, src.view, kW, kH, SizePx{});
    REQUIRE(setup.valid);
    // Prove the cover-fit really is in play, so this anchor cannot silently
    // degrade into the identity case.
    const KernelSetup identity = [&] {
        Settings same = s;
        same.resolution = kFillFrame;
        return buildParams(same, src.view, kW, kH, SizePx{});
    }();
    REQUIRE(identity.valid);
    CHECK(setup.params.focalPx == Approx(identity.params.focalPx * (1920.0 * kH) / (1080.0 * kW)).epsilon(1e-6));

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
    const KernelSetup setup = buildParams(s, src.view, kW, kH, SizePx{});
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
    const KernelSetup setup = buildParams(baseSettings(), src.view, 320, 180, SizePx{});
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

        const KernelSetup setup = buildParams(s, src.view, kW, kH, SizePx{});
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
        const KernelSetup setup = buildParams(s, src.view, kW, kH, SizePx{});
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
    const KernelSetup setup = buildParams(s, src.view, kW, kH, SizePx{});
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
//  Coverage and cover-fit (these replaced the letterbox tests)
//
//  The letterbox was removed ON PURPOSE - the user's requirement is that the
//  reframe always fills the output frame.  The old tests asserted transparent
//  bars above and below a 2.35:1 picture and a distinct letterbox per aspect
//  entry; those expectations describe precisely the behaviour that was asked
//  to go, so they are replaced rather than loosened.  The new tests are at
//  least as strict: they check EVERY pixel's alpha rather than a few rows,
//  and they pin the cover-fit geometry to a closed-form angle.
// ===========================================================================
namespace {

/// Render a whole frame into a packed 32f buffer; REQUIREs success.
std::vector<std::uint8_t> renderWhole(const KernelSetup& setup, const ConstFrameView& src, int w, int h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 16u, 0u);
    FrameView dst;
    dst.base = out.data();
    dst.rowBytes = w * 16;
    dst.width = w;
    dst.height = h;
    dst.layout = PixelLayout::Bgra32f;
    dst.topDown = true;
    REQUIRE(renderCpu(setup, src, dst, nullptr));
    return out;
}

/// Horizontal angle (deg) between the centre-row pixel `x` and the frame
/// centre, read back from the labelled panorama.  With the camera level and
/// rectilinear, this is exactly atan(dx / focal).
double horizontalAngleFromCentre(const std::vector<std::uint8_t>& out, int w, int h, int x) {
    float centre[4];
    float edge[4];
    readPixelBgra32f(out.data(), w * 16, w / 2, h / 2, centre);
    readPixelBgra32f(out.data(), w * 16, x, h / 2, edge);
    return std::fabs(angleDelta(lonOf(edge), lonOf(centre)));
}

}  // namespace

TEST_CASE("every resolution fills every pixel of the frame", "[reframe][cpu][geometry]") {
    PackedSource src(panorama(), panorama().width * 16);

    // A square frame, a tall frame and a wide one: every entry is a
    // different shape from at least two of them, so each one is exercised
    // in both cover-fit directions.  Odd sizes catch an off-by-one at the
    // far edge.
    for (const SizePx frame : {SizePx{121, 121}, SizePx{91, 161}, SizePx{201, 85}}) {
        for (const ResolutionEntry& e : kResolutions) {
            INFO("frame " << frame.w << "x" << frame.h << ", resolution '" << e.label << "'");
            Settings s = baseSettings();
            s.resolution = e.value;
            // Give "Match Sequence" a shape unlike every frame so it takes
            // the cover-fit path as well, not only the identity.
            const KernelSetup setup = buildParams(s, src.view, frame.w, frame.h, SizePx{3000, 1000});
            REQUIRE(setup.valid);

            // The painted rectangle is the frame itself...
            CHECK(setup.params.viewX == 0);
            CHECK(setup.params.viewY == 0);
            CHECK(setup.params.viewW == frame.w);
            CHECK(setup.params.viewH == frame.h);

            // ...and every single pixel of it carries the panorama, which is
            // opaque.  A letterbox bar, a pillarbox or an unpainted edge row
            // would all show up here as alpha 0.
            const std::vector<std::uint8_t> out = renderWhole(setup, src.view, frame.w, frame.h);
            int transparent = 0;
            for (int y = 0; y < frame.h; ++y) {
                for (int x = 0; x < frame.w; ++x) {
                    float rgba[4];
                    readPixelBgra32f(out.data(), frame.w * 16, x, y, rgba);
                    if (!(rgba[3] > 0.999f)) {
                        ++transparent;
                    }
                }
            }
            CHECK(transparent == 0);
        }
    }
}

TEST_CASE("a resolution of the frame's own shape is exactly the identity", "[reframe][cpu][geometry]") {
    // "Match Sequence" and every preview-scaled frame land here: Premiere
    // renders a 1920x1080 sequence at 960x540 or 480x270 while scrubbing,
    // and the picture must be the SAME picture, only smaller.  The cover-fit
    // test is exact integer arithmetic, so the setup must be bit-identical to
    // the frame-only one - not merely close.
    PackedSource src(panorama(), panorama().width * 16);
    Settings s = baseSettings();
    s.fovDeg = 100.0;
    s.distortion = 20.0;
    s.panDeg = 33.0;

    constexpr int kW = 480;
    constexpr int kH = 270;
    const KernelSetup reference = buildParams(s, src.view, kW, kH, SizePx{});
    REQUIRE(reference.valid);

    struct Case {
        Resolution resolution;
        SizePx sequence;
        const char* what;
    };
    const Case cases[] = {
        {Resolution::MatchSequence, SizePx{1920, 1080}, "Match Sequence, quarter-resolution preview"},
        {Resolution::MatchSequence, SizePx{3840, 2160}, "Match Sequence, UHD sequence"},
        {Resolution::Uhd3840x2160, SizePx{}, "3840 x 2160"},
        {Resolution::Qhd2560x1440, SizePx{}, "2560 x 1440"},
        {Resolution::Fhd1920x1080, SizePx{}, "1920 x 1080"},
        {Resolution::Hd1280x720, SizePx{}, "1280 x 720"},
    };
    for (const Case& c : cases) {
        INFO(c.what);
        Settings t = s;
        t.resolution = c.resolution;
        const KernelSetup setup = buildParams(t, src.view, kW, kH, c.sequence);
        REQUIRE(setup.valid);
        CHECK(setup.params.focalPx == reference.params.focalPx);
        CHECK(setup.params.eyeOffset == reference.params.eyeOffset);
        CHECK(setup.params.tanHalfH == reference.params.tanHalfH);
        CHECK(setup.params.tanHalfV == reference.params.tanHalfV);
        for (int i = 0; i < 9; ++i) {
            CHECK(setup.params.Rout[i] == reference.params.Rout[i]);
        }
    }
}

TEST_CASE("a WIDER resolution crops the sides and keeps FOV across its own width", "[reframe][cpu][geometry]") {
    // A 16:9 deliverable framed on a square frame.  The field of view spans
    // the width of the 16:9 image; that image is scaled until its HEIGHT
    // fills the frame, and the overflowing sides are cropped - no bars.
    //
    // Rectilinear at 90 degrees keeps the maths closed-form:
    //     identity:  f = (W/2) / tan(45) = W/2
    //     cover:     f' = f * (16/9) / (W/H)            (here W = H)
    // and the centre-row pixel x sits at atan((x + 0.5 - W/2) / f) from the
    // centre, which the labelled panorama reports directly.
    PackedSource src(panorama(), panorama().width * 16);
    constexpr int kW = 401;
    constexpr int kH = 401;
    Settings s = baseSettings();  // fov 90, distortion 0

    const KernelSetup fill = buildParams(s, src.view, kW, kH, SizePx{});
    s.resolution = Resolution::Fhd1920x1080;
    const KernelSetup cover = buildParams(s, src.view, kW, kH, SizePx{});
    REQUIRE(fill.valid);
    REQUIRE(cover.valid);

    const double scale = (1920.0 * kH) / (1080.0 * kW);
    CHECK(fill.params.focalPx == Approx(kW / 2.0).epsilon(1e-5));
    CHECK(cover.params.focalPx == Approx(fill.params.focalPx * scale).epsilon(1e-6));

    const std::vector<std::uint8_t> fillOut = renderWhole(fill, src.view, kW, kH);
    const std::vector<std::uint8_t> coverOut = renderWhole(cover, src.view, kW, kH);

    // The right-hand edge pixel: 44.9 degrees from centre when FOV spans the
    // frame, but only ~29.4 degrees under the 16:9 cover-fit, because the
    // other 15.5 degrees on each side are the cropped part of the wider image.
    const double dx = (kW - 1) + 0.5 - kW / 2.0;
    const double expectFill = std::atan(dx / fill.params.focalPx) * 180.0 / 3.14159265358979323846;
    const double expectCover = std::atan(dx / cover.params.focalPx) * 180.0 / 3.14159265358979323846;
    CHECK(horizontalAngleFromCentre(fillOut, kW, kH, kW - 1) == Approx(expectFill).margin(0.5));
    CHECK(horizontalAngleFromCentre(coverOut, kW, kH, kW - 1) == Approx(expectCover).margin(0.5));
    CHECK(expectCover < expectFill - 10.0);

    // The centre does not move: cover-fit is a uniform scale about it.
    float a[4];
    float b[4];
    readPixelBgra32f(fillOut.data(), kW * 16, kW / 2, kH / 2, a);
    readPixelBgra32f(coverOut.data(), kW * 16, kW / 2, kH / 2, b);
    CHECK(std::fabs(angleDelta(lonOf(a), lonOf(b))) < 0.05);
    CHECK(std::fabs(latOf(a) - latOf(b)) < 0.05);
}

TEST_CASE("a TALLER resolution crops top and bottom and leaves FOV untouched", "[reframe][cpu][geometry]") {
    // 16:9 on a 2.35:1 frame: the frame is relatively wider, so the WIDTH
    // binds, FOV spans the frame width exactly as before and only the top
    // and bottom of the 16:9 image are cropped.  The factor is exactly 1.
    PackedSource src(panorama(), panorama().width * 16);
    constexpr int kW = 470;
    constexpr int kH = 200;
    Settings s = baseSettings();
    const KernelSetup fill = buildParams(s, src.view, kW, kH, SizePx{});
    s.resolution = Resolution::Hd1280x720;
    const KernelSetup cover = buildParams(s, src.view, kW, kH, SizePx{});
    REQUIRE(fill.valid);
    REQUIRE(cover.valid);
    CHECK(cover.params.focalPx == fill.params.focalPx);
}

TEST_CASE("Match Sequence cover-fits a sequence of a different shape", "[reframe][cpu][geometry]") {
    // The sequence size is only a SHAPE to the effect: a 2:1 sequence on a
    // 16:9 frame crops the sides by exactly 2 / (16/9) = 1.125, and the same
    // sequence on a 1:2 frame crops by 4.
    PackedSource src(panorama(), panorama().width * 16);
    Settings s = baseSettings();
    s.resolution = Resolution::MatchSequence;

    const KernelSetup wide = buildParams(s, src.view, 480, 270, SizePx{2000, 1000});
    const KernelSetup wideRef = buildParams(s, src.view, 480, 270, SizePx{});
    REQUIRE(wide.valid);
    REQUIRE(wideRef.valid);
    CHECK(wide.params.focalPx == Approx(wideRef.params.focalPx * 1.125).epsilon(1e-6));

    const KernelSetup tall = buildParams(s, src.view, 100, 200, SizePx{2000, 1000});
    const KernelSetup tallRef = buildParams(s, src.view, 100, 200, SizePx{});
    REQUIRE(tall.valid);
    REQUIRE(tallRef.valid);
    CHECK(tall.params.focalPx == Approx(tallRef.params.focalPx * 4.0).epsilon(1e-6));

    // A square sequence on a 16:9 frame is relatively TALLER: width binds,
    // factor 1, top and bottom cropped.
    const KernelSetup square = buildParams(s, src.view, 480, 270, SizePx{1080, 1080});
    REQUIRE(square.valid);
    CHECK(square.params.focalPx == wideRef.params.focalPx);
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
    const KernelSetup setup = buildParams(s, src.view, kW, kH, SizePx{});
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
    const KernelSetup setup = buildParams(s, src.view, kW, kH, SizePx{});
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
    const KernelSetup setup = buildParams(baseSettings(), src.view, kW, kH, SizePx{});
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
    const KernelSetup setup32 = buildParams(s, src32.view, kW, kH, SizePx{});
    const KernelSetup setup16 = buildParams(s, src16, kW, kH, SizePx{});
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

        if (inputFormat == PrPixelFormat_BGRA_4444_32f || inputFormat == PrPixelFormat_BGRA_4444_32f_Linear) {
            // _32f_Linear has the identical byte layout - it differs only in
            // transfer function - so the same packer fills either world.
            const std::vector<std::uint8_t> packed = packBgra32f(p, input->rowBytes());
            REQUIRE(packed.size() <= static_cast<std::size_t>(input->rowBytes()) * static_cast<std::size_t>(p.height));
            std::memcpy(input->pixels(), packed.data(), packed.size());
        } else if (inputFormat == PrPixelFormat_BGRA_4444_16u) {
            // Quantised on the documented 0..32768 scale, which is exactly
            // the inverse of the effect's own promotion, so a 16u-input
            // render can be compared against a 32f-input render of the SAME
            // quantised data.
            const std::vector<std::uint8_t> packed = packBgra16u(p, input->rowBytes());
            REQUIRE(!packed.empty());
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

        // [WP-LENSUI] The effect's default lens is DJI.  Every render here
        // drives the Classic camera (FOV / Distortion) and compares it with
        // Classic references, so the fixture selects Classic in the Lens
        // popup; test_dji_camera.cpp covers DJI's lens through the module.
        setPopup(kIndexLens, static_cast<int>(LensPopup::Classic));
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
    f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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

namespace {

/// Horizontal angle (deg) from the frame centre to the right-hand edge pixel
/// of the centre row of a PF_Cmd_RENDER output, read from the labelled
/// panorama.  With a level rectilinear camera this is atan(dx / focal), so it
/// measures the cover-fit through the whole loaded module.
double renderedEdgeAngle(const RenderFixture& f, int w, int h) {
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
    float centre[4];
    float edge[4];
    readPixelBgra32f(pixels, f.output->rowBytes(), w / 2, h / 2, centre);
    readPixelBgra32f(pixels, f.output->rowBytes(), w - 1, h / 2, edge);
    return std::fabs(angleDelta(lonOf(edge), lonOf(centre)));
}

/// True when every pixel of the output is opaque - i.e. no letterbox, no
/// pillarbox and no unpainted edge.
bool everyPixelOpaque(const RenderFixture& f, int w, int h) {
    const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float rgba[4];
            readPixelBgra32f(pixels, f.output->rowBytes(), x, y, rgba);
            if (!(rgba[3] > 0.999f)) {
                return false;
            }
        }
    }
    return true;
}

/// atan(dx / f) in degrees for the right-hand edge pixel of a `w`-wide frame.
double expectedEdgeAngle(int w, double focal) {
    const double dx = (w - 1) + 0.5 - w / 2.0;
    return std::atan(dx / focal) * 180.0 / 3.14159265358979323846;
}

}  // namespace

TEST_CASE("PF_Cmd_RENDER fills the frame and cover-fits a fixed resolution", "[reframe][render]") {
    // This replaced "PF_Cmd_RENDER letterboxes according to the aspect
    // popup".  A 2.35:1 entry on a square frame used to leave transparent
    // bars above and below; the letterbox was removed on purpose, so the
    // same situation - a fixed shape on a frame of another shape - must now
    // fill every pixel and crop the overflow instead.
    constexpr int kW = 401;
    constexpr int kH = 401;
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
    f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat(kIndexFov, 90.0);
    f.setFloat(kIndexDistortion, 0.0);

    // Frame-shaped first: FOV spans the frame, the edge is ~44.9 degrees out.
    f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
    REQUIRE(f.render() == PF_Err_NONE);
    CHECK(everyPixelOpaque(f, kW, kH));
    const double fillFocal = kW / 2.0;  // rectilinear, 90 degrees
    CHECK(renderedEdgeAngle(f, kW, kH) == Approx(expectedEdgeAngle(kW, fillFocal)).margin(0.5));

    // 2560 x 1440 on the same square frame: still no bars anywhere, and the
    // sides are cropped by exactly the 16:9 cover factor.
    f.setPopup(kIndexOutputResolution, static_cast<int>(Resolution::Qhd2560x1440));
    REQUIRE(f.render() == PF_Err_NONE);
    CHECK(everyPixelOpaque(f, kW, kH));
    const double coverFocal = fillFocal * (2560.0 * kH) / (1440.0 * kW);
    CHECK(renderedEdgeAngle(f, kW, kH) == Approx(expectedEdgeAngle(kW, coverFocal)).margin(0.5));
}

TEST_CASE("PF_Cmd_RENDER honours Match Sequence through the Sequence Info Suite", "[reframe][render]") {
    constexpr int kW = 401;
    constexpr int kH = 401;
    constexpr PrTimelineID kTimeline = 0x2222;
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f, kTimeline);
    f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat(kIndexFov, 90.0);
    f.setFloat(kIndexDistortion, 0.0);

    // Tell the mock the sequence is 2:1; "Match Sequence" must pick that up
    // through GetContainingTimelineID + GetFrameRect rather than fall back.
    // It used to show up as a 2:1 LETTERBOX; the letterbox is gone on
    // purpose, so it now shows up as a 2:1 cover-fit: every pixel filled and
    // the sides cropped by (2000 * 401) / (1000 * 401) = 2.
    osv::premiere::mock::SequenceConfig config = f.host.sequence(kTimeline);
    prSetRect(&config.frameRect, 0, 0, 2000, 1000);
    f.host.setSequence(kTimeline, config);

    f.setPopup(kIndexOutputResolution, static_cast<int>(Resolution::MatchSequence));
    REQUIRE(f.render() == PF_Err_NONE);
    CHECK(everyPixelOpaque(f, kW, kH));

    const double fillFocal = kW / 2.0;
    const double sequenceFocal = fillFocal * 2.0;
    const double measured = renderedEdgeAngle(f, kW, kH);
    CHECK(measured == Approx(expectedEdgeAngle(kW, sequenceFocal)).margin(0.5));
    // And it is NOT the frame fallback, which would put the edge ~18 degrees
    // further out - so the suite genuinely answered.
    CHECK(measured < expectedEdgeAngle(kW, fillFocal) - 10.0);
}

TEST_CASE("Match Sequence falls back to the frame with no Sequence Info Suite", "[reframe][render]") {
    // This used to fall back to a fixed 16:9 letterbox.  Both halves of that
    // changed on purpose: there is no letterbox, and "Match Sequence" now
    // needs a SIZE, for which the frame the host allocated is the exact
    // answer rather than a guessed shape (resolveOutputSize documents why).
    // What must NOT change is that a host that cannot answer produces a real
    // picture instead of a failure.
    constexpr int kW = 401;
    constexpr int kH = 401;
    constexpr PrTimelineID kTimeline = 0x2323;
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f, kTimeline);
    f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat(kIndexFov, 90.0);
    f.setFloat(kIndexDistortion, 0.0);

    // Configure a sequence that WOULD change the picture, then hide every
    // version of the suite the effect tries, so the only way to get the
    // frame-shaped answer below is the fallback itself.
    osv::premiere::mock::SequenceConfig config = f.host.sequence(kTimeline);
    prSetRect(&config.frameRect, 0, 0, 2000, 1000);
    f.host.setSequence(kTimeline, config);
    for (const int version : {9, 8, 7, 6, 5}) {
        f.host.setSuiteAvailable(kPrSDKSequenceInfoSuite, version, false);
    }

    f.setPopup(kIndexOutputResolution, static_cast<int>(Resolution::MatchSequence));
    REQUIRE(f.render() == PF_Err_NONE);
    CHECK(everyPixelOpaque(f, kW, kH));
    CHECK(renderedEdgeAngle(f, kW, kH) == Approx(expectedEdgeAngle(kW, kW / 2.0)).margin(0.5));

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
    f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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

// ===========================================================================
//  High-bit-depth pixel formats
//
//  These are the regression tests for the "plays fine but does not update
//  when I step a frame" bug.  On a 10-bit HDR sequence Premiere handed the
//  CPU path a format that was not one of the only two it accepted, so
//  PF_Cmd_RENDER - which is what scrubbing and stepping use - refused every
//  frame, while playback went through the separately negotiated GPU entry and
//  looked correct.  Each case below fails against the old two-format code.
// ===========================================================================

TEST_CASE("PF_Cmd_RENDER into and out of a 16u world produces a correct picture",
          "[reframe][render][format16u]") {
    constexpr int kW = 200;
    constexpr int kH = 150;

    // BOTH worlds 16u, which is what a 10-bit sequence actually hands us -
    // not a 16u output fed from a float input.
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_16u, 0x2000, PrPixelFormat_BGRA_4444_16u);
    f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
    f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
    f.setFloat(kIndexFov, 90.0);
    f.setFloat(kIndexDistortion, 0.0);

    // The render must SUCCEED.  This single assertion is the bug: it returned
    // PF_Err_BAD_CALLBACK_PARAM before the fix.
    REQUIRE(f.render() == PF_Err_NONE);

    // And it must aim where it is told, decoded from the labelled panorama
    // exactly as the 32f cases do - so this proves a correct picture, not
    // merely a non-error.
    const struct {
        double pan;
        double tilt;
    } cases[] = {{0.0, 0.0}, {90.0, 0.0}, {-90.0, 0.0}, {0.0, 40.0}};

    for (const auto& c : cases) {
        INFO("pan " << c.pan << " tilt " << c.tilt);
        f.setAngle(kIndexPan, c.pan);
        f.setAngle(kIndexTilt, c.tilt);
        REQUIRE(f.render() == PF_Err_NONE);

        float centre[4];
        readPixelBgra16u(reinterpret_cast<const std::uint8_t*>(f.output->pixels()), f.output->rowBytes(), kW / 2,
                         kH / 2, centre);
        CHECK(std::fabs(angleDelta(lonOf(centre), -c.pan)) < 1.5);
        CHECK(std::fabs(latOf(centre) - c.tilt) < 1.5);
        // Alpha must be full scale, which on this format is 32768 and not
        // 65535: readPixelBgra16u divides by the documented white point, so a
        // renderer using the wrong scale reports 0.5 here and fails.
        CHECK(centre[3] == Approx(1.0).margin(1e-3));
    }
}

TEST_CASE("the 16u render matches the 32f render within the 0..32768 quantisation step",
          "[reframe][render][format16u]") {
    constexpr int kW = 160;
    constexpr int kH = 120;

    // Both renders read a 16u INPUT, so the only difference between them is
    // the output store.  Feeding one a float input would also fold the input
    // promotion into the comparison and make the tolerance meaningless.
    auto renderInto = [](PrPixelFormat outFormat) {
        RenderFixture f(kW, kH, outFormat, 0x2000, PrPixelFormat_BGRA_4444_16u);
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setAngle(kIndexPan, 30.0);
        REQUIRE(f.render() == PF_Err_NONE);

        std::vector<float> samples;
        const auto* pixels = reinterpret_cast<const std::uint8_t*>(f.output->pixels());
        for (int y = 2; y < kH - 2; y += 4) {
            for (int x = 2; x < kW - 2; x += 4) {
                float rgba[4];
                if (outFormat == PrPixelFormat_BGRA_4444_32f) {
                    readPixelBgra32f(pixels, f.output->rowBytes(), x, y, rgba);
                } else {
                    readPixelBgra16u(pixels, f.output->rowBytes(), x, y, rgba);
                }
                samples.insert(samples.end(), rgba, rgba + 4);
            }
        }
        return samples;
    };

    const std::vector<float> f32 = renderInto(PrPixelFormat_BGRA_4444_32f);
    const std::vector<float> u16 = renderInto(PrPixelFormat_BGRA_4444_16u);
    REQUIRE(f32.size() == u16.size());
    REQUIRE_FALSE(f32.empty());

    double worst = 0.0;
    for (std::size_t i = 0; i < f32.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(f32[i]) - static_cast<double>(u16[i])));
    }

    // One quantisation step on the 0..32768 scale, plus a rounding epsilon.
    // This bound is the point of the test: it is ~128x TIGHTER than the 8-bit
    // one, so a store that used 65535 as white (halving every value) or 255
    // (saturating everything) misses it by orders of magnitude and cannot
    // pass by luck.
    const double step = 1.0 / static_cast<double>(kBgra16uWhiteRef);
    INFO("worst 16u vs 32f difference through the module: " << worst << " (one step = " << step << ")");
    CHECK(worst <= step + 1e-7);

    // Guard against the test passing because BOTH renders were black.
    double peak = 0.0;
    for (const float v : f32) {
        peak = std::max(peak, static_cast<double>(v));
    }
    CHECK(peak > 0.25);
}

TEST_CASE("a 16u input renders the same picture as the equivalent float input",
          "[reframe][render][format16u]") {
    constexpr int kW = 160;
    constexpr int kH = 120;

    // Both write 32f, so the only difference is the INPUT promotion: 16u
    // codes / 32768 must reproduce the float panorama it was quantised from.
    auto renderFrom = [](PrPixelFormat inFormat) {
        RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f, 0x2000, inFormat);
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 90.0);
        f.setFloat(kIndexDistortion, 0.0);
        f.setAngle(kIndexPan, 25.0);
        REQUIRE(f.render() == PF_Err_NONE);
        return copyWorld(*f.output, kH);
    };

    const std::vector<std::uint8_t> fromFloat = renderFrom(PrPixelFormat_BGRA_4444_32f);
    const std::vector<std::uint8_t> from16u = renderFrom(PrPixelFormat_BGRA_4444_16u);
    REQUIRE(fromFloat.size() == from16u.size());

    // Compare as floats: the inputs differ by the input quantisation, which
    // the bilinear sampler then averages, so the results are close rather
    // than identical.
    const std::int32_t pitch = static_cast<std::int32_t>(fromFloat.size() / static_cast<std::size_t>(kH));
    double worst = 0.0;
    for (int y = 2; y < kH - 2; y += 3) {
        for (int x = 2; x < kW - 2; x += 3) {
            float a[4];
            float b[4];
            readPixelBgra32f(fromFloat.data(), pitch, x, y, a);
            readPixelBgra32f(from16u.data(), pitch, x, y, b);
            for (int c = 0; c < 4; ++c) {
                worst = std::max(worst, std::fabs(static_cast<double>(a[c]) - static_cast<double>(b[c])));
            }
        }
    }
    INFO("worst 16u-input vs 32f-input difference: " << worst);
    CHECK(worst <= 1.0 / static_cast<double>(kBgra16uWhiteRef) + 1e-6);
}

TEST_CASE("BGRA_4444_32f_Linear is accepted and renders identically to 32f",
          "[reframe][render][formatlinear]") {
    constexpr int kW = 176;
    constexpr int kH = 132;

    // _32f_Linear differs from _32f ONLY in transfer function.  This effect
    // resamples and never interprets a code, so the two must produce BIT
    // IDENTICAL output - not merely similar - and that is what is asserted.
    auto renderWith = [](PrPixelFormat inFormat, PrPixelFormat outFormat) {
        RenderFixture f(kW, kH, outFormat, 0x2000, inFormat);
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
        f.setPopup(kIndexPreset, static_cast<int>(Preset::Custom));
        f.setFloat(kIndexFov, 100.0);
        f.setFloat(kIndexDistortion, 20.0);
        f.setAngle(kIndexPan, 42.0);
        f.setAngle(kIndexTilt, -15.0);
        REQUIRE(f.render() == PF_Err_NONE);
        return copyWorld(*f.output, kH);
    };

    const std::vector<std::uint8_t> plain =
        renderWith(PrPixelFormat_BGRA_4444_32f, PrPixelFormat_BGRA_4444_32f);

    SECTION("a linear INPUT world is accepted") {
        const std::vector<std::uint8_t> linearIn =
            renderWith(PrPixelFormat_BGRA_4444_32f_Linear, PrPixelFormat_BGRA_4444_32f);
        CHECK(linearIn == plain);
    }
    SECTION("a linear OUTPUT world is accepted") {
        const std::vector<std::uint8_t> linearOut =
            renderWith(PrPixelFormat_BGRA_4444_32f, PrPixelFormat_BGRA_4444_32f_Linear);
        CHECK(linearOut == plain);
    }
    SECTION("both linear is accepted") {
        const std::vector<std::uint8_t> both =
            renderWith(PrPixelFormat_BGRA_4444_32f_Linear, PrPixelFormat_BGRA_4444_32f_Linear);
        CHECK(both == plain);
    }

    // Not all black, or the equality above would be vacuous.
    bool nonZero = false;
    for (const std::uint8_t b : plain) {
        if (b != 0u) {
            nonZero = true;
            break;
        }
    }
    CHECK(nonZero);
}

TEST_CASE("a genuinely unknown pixel format is refused, not misread", "[reframe][render][format]") {
    constexpr int kW = 120;
    constexpr int kH = 90;

    // A world the effect cannot render.  VUYA_4444_32f is a real Premiere
    // format of the same 16-byte width as BGRA_4444_32f, which makes it the
    // honest test: the bytes are readable and plausibly sized, so nothing but
    // the format check stands between us and silently interpreting luma as
    // blue.  The mock reports the format through the same
    // PF_PixelFormatSuite::GetPixelFormat call the real host answers.
    RenderFixture f(kW, kH, PrPixelFormat_BGRA_4444_32f);
    REQUIRE(f.render() == PF_Err_NONE);  // the fixture itself is sound

    // Relabel the OUTPUT world's format without touching its bytes, so the
    // only thing that changed is what the host says it is.
    f.host.setWorldFormat(&f.output->world(), PrPixelFormat_VUYA_4444_32f);

    // Refused with the documented error, and no crash.
    CHECK(f.render() == PF_Err_BAD_CALLBACK_PARAM);

    // Refused again, still without crashing: the log line is emitted once
    // (PluginLog::oncef) but the REFUSAL must be every time, never a
    // "logged already, so let it through" cache.
    CHECK(f.render() == PF_Err_BAD_CALLBACK_PARAM);
}

TEST_CASE("GLOBAL_SETUP registers every renderable format in preference order",
          "[reframe][setup][format]") {
    MockHost host;
    PF_ProgPtr ref = host.createEffectRef(0x3100, 11);
    REQUIRE(ref != nullptr);

    PF_InData in = host.makeInData(ref, {});
    PF_OutData out = host.makeOutData();
    REQUIRE(in.appl_id == kAppID_Premiere);
    REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
            PF_Err_NONE);

    const std::vector<PrPixelFormat> formats = host.supportedPixelFormats(ref);

    // The exact list, in the exact order, as docs/PREMIERE.md records it.
    // Order is a PREFERENCE ranking to the host (PrSDKAESupport.h:150-157),
    // so it is asserted positionally rather than as a set: float first so an
    // HDR panorama is never quantised before being resampled, then linear
    // float, then 16u for a 10-bit timeline, then 8u last.
    REQUIRE(formats.size() == 4);
    CHECK(formats[0] == PrPixelFormat_BGRA_4444_32f);
    CHECK(formats[1] == PrPixelFormat_BGRA_4444_32f_Linear);
    CHECK(formats[2] == PrPixelFormat_BGRA_4444_16u);
    CHECK(formats[3] == PrPixelFormat_BGRA_4444_8u);

    host.destroyEffectRef(ref);
}

TEST_CASE("the render path still produces a picture when the pixel format suite is ABSENT",
          "[reframe][render][format]") {
    // THE regression test for the reported bug.  The host log showed the PF
    // Pixel Format Suite missing on some GLOBAL_SETUP calls; the effect then
    // told the host nothing, the host chose a format unaided, and on a 10-bit
    // sequence the CPU path refused it - so stepping a frame showed nothing
    // while playback (the GPU path) was fine.
    //
    // "No suite at setup" must therefore NOT mean "no picture".  With the
    // suite hidden for the whole of GLOBAL_SETUP, a subsequent render into
    // the high-bit-depth format such a host would pick must still succeed and
    // still be correct.
    constexpr int kW = 144;
    constexpr int kH = 108;

    MockHost host;
    PF_ProgPtr ref = host.createEffectRef(0x3200, 11);
    REQUIRE(ref != nullptr);

    // Hidden for GLOBAL_SETUP and PARAMS_SETUP...
    host.setSuiteAvailable(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, false);
    {
        PF_InData in = host.makeInData(ref, {});
        PF_OutData out = host.makeOutData();
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
    }
    // ...and nothing was registered, exactly as on the failing host.
    CHECK(host.supportedPixelFormats(ref).empty());

    // The suite comes back for rendering (the render path needs
    // GetPixelFormat to identify the worlds at all, and on the real host the
    // suite was present on most calls - it was the registration that got
    // missed).  This is also what lets the render-time RETRY run.
    host.setSuiteAvailable(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, true);

    const Panorama& p = panorama();
    std::unique_ptr<EffectWorld> input =
        host.createWorld(static_cast<std::uint32_t>(p.width), static_cast<std::uint32_t>(p.height),
                         PrPixelFormat_BGRA_4444_16u);
    REQUIRE(input != nullptr);
    const std::vector<std::uint8_t> packed = packBgra16u(p, input->rowBytes());
    REQUIRE(!packed.empty());
    std::memcpy(input->pixels(), packed.data(), packed.size());
    host.setInputWorld(ref, input.get());

    std::unique_ptr<EffectWorld> output = host.createWorld(kW, kH, PrPixelFormat_BGRA_4444_16u);
    REQUIRE(output != nullptr);

    // Point the camera somewhere unambiguous.
    {
        std::vector<PF_ParamDef> params = host.addedParams(ref);
        REQUIRE(params.size() >= static_cast<std::size_t>(kIndexSmooth));
        PF_ParamDef resolution = params[static_cast<std::size_t>(kIndexOutputResolution) - 1u];
        resolution.u.pd.value = static_cast<int>(kFillFrame);
        host.setParamValue(ref, kIndexOutputResolution, resolution);
        PF_ParamDef preset = params[static_cast<std::size_t>(kIndexPreset) - 1u];
        preset.u.pd.value = static_cast<int>(Preset::Custom);
        host.setParamValue(ref, kIndexPreset, preset);
        PF_ParamDef fov = params[static_cast<std::size_t>(kIndexFov) - 1u];
        fov.u.fs_d.value = static_cast<PF_FpShort>(90.0);
        host.setParamValue(ref, kIndexFov, fov);
        PF_ParamDef dist = params[static_cast<std::size_t>(kIndexDistortion) - 1u];
        dist.u.fs_d.value = static_cast<PF_FpShort>(0.0);
        host.setParamValue(ref, kIndexDistortion, dist);
        // [WP-LENSUI] The Classic lens those two numbers describe (the
        // effect's default lens is DJI).
        PF_ParamDef lens = params[static_cast<std::size_t>(kIndexLens) - 1u];
        lens.u.pd.value = static_cast<A_long>(LensPopup::Classic);
        host.setParamValue(ref, kIndexLens, lens);
    }

    PF_InData in = host.makeInData(ref, {});
    PF_OutData out = host.makeOutData();
    std::vector<PF_ParamDef*> params = host.renderParams(ref);

    // No suite at setup must not mean no picture.
    REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_RENDER, &in, &out, params.data(), &output->world(),
                                                  nullptr) == PF_Err_NONE);

    float centre[4];
    readPixelBgra16u(reinterpret_cast<const std::uint8_t*>(output->pixels()), output->rowBytes(), kW / 2, kH / 2,
                     centre);
    CHECK(centre[3] == Approx(1.0).margin(1e-3));
    // Pan is 0, so the centre must look along the panorama's centre column.
    CHECK(std::fabs(angleDelta(lonOf(centre), 0.0)) < 1.5);

    // And the render-time retry registered the list the setup call could not.
    const std::vector<PrPixelFormat> formats = host.supportedPixelFormats(ref);
    INFO("formats registered by the render-time retry: " << formats.size());
    REQUIRE(formats.size() == 4);
    CHECK(formats[0] == PrPixelFormat_BGRA_4444_32f);
    CHECK(formats[3] == PrPixelFormat_BGRA_4444_8u);

    host.destroyEffectRef(ref);
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
        f.setPopup(kIndexOutputResolution, static_cast<int>(kFillFrame));
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
    HostParamKind::Int32,    // Output Resolution
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
    CHECK(map[kIndexOutputResolution] == 0);
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
    CHECK(map[kIndexOutputResolution] == 0);
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

TEST_CASE("matchHostParams tolerates entries Premiere cannot type", "[reframe][params]") {
    // THE regression test for the keyframing bug.
    //
    // This is a verbatim kind sequence from a real session log
    // (%LOCALAPPDATA%/OpenOSV/Open360Reframe.log): Premiere reported 11
    // entries - our full list, one to one - but could not return a type for
    // four of them:
    //
    //   [0]i32:8 [1]=<err> [2]i32:1 [3]f32:42.0 [4]f32:-15.0 [5]f32:7.0
    //   [6]f64:100.0 [7]f64:30.0 [8]=<err> [9]=<err> [10]=<err>
    //
    // Note the VALUES: 42 / -15 / 7 / 100 / 30 are exactly the Pan / Tilt /
    // Roll / FOV / Distortion the user had dialled in, sitting at indices
    // 3..7 - one further along than the 8-entry layout puts them.  Reading
    // them with the static mapping therefore returns a neighbouring control.
    //
    // Treating Unknown as "matches nothing" rejected this alignment on the
    // first hole, so all 74 renders in that log fell back to the static
    // mapping.  Playback happened to take another path, which is why
    // scrubbing and keyframe stepping looked broken while playback did not.
    const std::vector<HostParamKind> logged = {
        HostParamKind::Int32,    // [0]  an entry ahead of our list
        HostParamKind::Unknown,  // [1]  the host refused this one's type
        HostParamKind::Int32,    // [2]  Output Resolution
        HostParamKind::Float32,  // [3]  Pan        (42)
        HostParamKind::Float32,  // [4]  Tilt       (-15)
        HostParamKind::Float32,  // [5]  Roll       (7)
        HostParamKind::Float64,  // [6]  FOV        (100)  <- the anchor pair
        HostParamKind::Float64,  // [7]  Distortion (30)   <-
        HostParamKind::Unknown,  // [8]  Source Pan
        HostParamKind::Unknown,  // [9]  Source Tilt
        HostParamKind::Unknown,  // [10] Source Roll
    };
    // The log was recorded when the effect had eleven value controls; the
    // [WP-CAMERA] block appended five more, so a list of this length is now a
    // host that stops before them (which the matcher accepts) rather than the
    // whole signature.
    REQUIRE(logged.size() <= static_cast<std::size_t>(kValueParamCount));

    HostParamMap map{};
    REQUIRE(matchKinds(logged, &map));
    CHECK(map.probed);

    // The whole point: the angles must resolve to the indices that actually
    // held them.  The logged VALUES are the independent confirmation - 42,
    // -15 and 7 were the Pan, Tilt and Roll the user had dialled in, and they
    // sit at 3, 4 and 5, one further along than the static mapping assumes.
    CHECK(map[kIndexPan] == 3);
    CHECK(map[kIndexTilt] == 4);
    CHECK(map[kIndexRoll] == 5);
    CHECK(map[kIndexFov] == 6);
    CHECK(map[kIndexDistortion] == 7);

    // Group markers still carry no value.
    CHECK(map[kIndexCameraTopic] == -1);
    CHECK(map[kIndexSourceTopic] == -1);
}

TEST_CASE("the Float64 anchor refuses lists it cannot pin", "[reframe][params]") {
    HostParamMap map{};

    SECTION("no Float64 pair at all") {
        // Without FOV and Distortion adjacent there is nothing to anchor on.
        const std::vector<HostParamKind> noPair(11, HostParamKind::Float32);
        CHECK_FALSE(matchKinds(noPair, &map));
    }

    SECTION("two Float64 pairs are two candidate anchors") {
        std::vector<HostParamKind> twoPairs(11, HostParamKind::Float32);
        twoPairs[1] = HostParamKind::Float64;
        twoPairs[2] = HostParamKind::Float64;
        twoPairs[7] = HostParamKind::Float64;
        twoPairs[8] = HostParamKind::Float64;
        CHECK_FALSE(matchKinds(twoPairs, &map));
    }

    SECTION("an anchor with almost nothing around it is not enough") {
        // A pair and one neighbour could be coincidence in any short list.
        const std::vector<HostParamKind> tiny = {HostParamKind::Float64, HostParamKind::Float64};
        CHECK_FALSE(matchKinds(tiny, &map));
    }
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

    SECTION("enough unreadable entries make the answer ambiguous") {
        // An unreadable entry is a WILDCARD (see matchHostParams), so one
        // hole does not break the match - the remaining types still pin the
        // alignment.  Blank enough of them and several alignments become
        // viable, and then the matcher must decline rather than guess.
        std::vector<HostParamKind> mostlyBlank(8, HostParamKind::Unknown);
        CHECK_FALSE(matchKinds(mostlyBlank, &map));
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
    CHECK(map[kIndexOutputResolution] == gpuParamIndex(kIndexOutputResolution));
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
    s.resolution = kFillFrame;
    s.fovDeg = hostile;
    const KernelSetup setup = buildParams(s, src.view, 320, 180, SizePx{});
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
        s.resolution = kFillFrame;
        s.fovDeg = bad;
        const KernelSetup setup = buildParams(s, src.view, 320, 180, SizePx{});
        CHECK(setup.valid);
        CHECK(std::isfinite(setup.params.focalPx));
        CHECK(setup.params.focalPx > 0.0f);
    }
}
