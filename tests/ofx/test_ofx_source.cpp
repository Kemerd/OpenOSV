// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_source.cpp - the OpenOSV Source generator: which clip frame a
// host time shows, how it behaves without a usable clip, and - on the sample
// clip - that its picture is the importer's stitch and the Premiere effect's
// framing of it.
//
// The [sample] tests need example_footage_dlogm.OSV (OSV_OFX_SAMPLE_CLIP, or
// the OSV_SAMPLE_FILE environment variable); without it they SKIP.

#include "OfxTestSupport.h"

#include "OfxCamera.h"
#include "OfxSource.h"
#include "OfxSourceParams.h"

#include "ReframeCpu.h"

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace osv::ofxtest;
namespace cam = osv::ofx::camera;
namespace src = osv::ofx::source;
namespace rf = osv::reframe;

namespace {

/// The sample clip, or empty when this machine has none.
std::string sampleClip() {
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        if (std::filesystem::exists(env)) {
            return env;
        }
    }
#if defined(OSV_OFX_SAMPLE_CLIP)
    if (std::filesystem::exists(OSV_OFX_SAMPLE_CLIP)) {
        return OSV_OFX_SAMPLE_CLIP;
    }
#endif
    return {};
}

/// One generator instance rendering into a `w` x `h` output.
struct SourceRig {
    std::unique_ptr<Effect> effect;
    HostImage output;
    OfxRectI frame;

    SourceRig(int w, int h, double fps = 29.97) : frame{0, 0, w, h} {
        OfxStatus st = kOfxStatFailed;
        effect = Fixture::get().source.createInstance(kOfxImageEffectContextGenerator, w, h, fps, &st);
        REQUIRE(st == kOfxStatOK);
        output = makeImage(frame);
        output.fill(-7.0f);
        Clip* out = effect->clip(kOfxImageEffectOutputClipName);
        REQUIRE(out);
        provideImage(*out, output);
    }
    ~SourceRig() {
        if (effect) {
            (void)Fixture::get().source.destroyInstance(*effect);
        }
    }

    Param& param(const char* name) {
        Param* p = effect->params.find(name);
        REQUIRE(p);
        return *p;
    }

    OfxStatus render(double time = 0.0) {
        output.fill(-7.0f);
        PluginHarness::RenderArgs args;
        args.time = time;
        args.window = frame;
        return Fixture::get().source.render(*effect, args);
    }

    [[nodiscard]] bool allTransparent() const {
        for (int y = frame.y1; y < frame.y2; ++y) {
            for (int x = frame.x1; x < frame.x2; ++x) {
                const float* p = output.pixel(x, y);
                if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f || p[3] != 0.0f) {
                    return false;
                }
            }
        }
        return true;
    }
};

}  // namespace

// ===========================================================================
//  Which frame
// ===========================================================================

TEST_CASE("host time maps to clip frames through seconds", "[ofx][source]") {
    // Same rate: frame for frame from the generator's first frame.
    CHECK(src::frameForTime(0.0, 0.0, 29.97, 29.97, 0) == 0);
    CHECK(src::frameForTime(17.0, 0.0, 29.97, 29.97, 0) == 17);
    // A range that starts where the time does (timeline frames both ways).
    CHECK(src::frameForTime(86400.0, 86400.0, 24.0, 24.0, 0) == 0);
    CHECK(src::frameForTime(86410.0, 86400.0, 24.0, 24.0, 0) == 10);
    // A 59.94 clip on a 29.97 timeline plays at real speed: every other frame.
    CHECK(src::frameForTime(1.0, 0.0, 30000.0 / 1001.0, 60000.0 / 1001.0, 0) == 2);
    CHECK(src::frameForTime(3.0, 0.0, 30000.0 / 1001.0, 60000.0 / 1001.0, 0) == 6);
    // A 29.97 clip on a 59.94 timeline holds each frame twice.
    CHECK(src::frameForTime(3.0, 0.0, 60000.0 / 1001.0, 30000.0 / 1001.0, 0) == 1);
    // Start Frame slides the clip under the generator.
    CHECK(src::frameForTime(5.0, 0.0, 25.0, 25.0, 100) == 105);
    // A range start the time has not reached: the host counts from zero.
    CHECK(src::frameForTime(12.0, 86400.0, 25.0, 25.0, 0) == 12);
    // Garbage rates fall back to frame-for-frame, never to a crash.
    CHECK(src::frameForTime(7.0, 0.0, 0.0, 0.0, 0) == 7);
    CHECK(src::frameForTime(7.0, 0.0, std::nan(""), 25.0, 0) == 7);
    // Before the clip, or not a time at all.
    CHECK(src::frameForTime(-3.0, 0.0, 25.0, 25.0, 0) < 0);
    CHECK(src::frameForTime(std::nan(""), 0.0, 25.0, 25.0, 0) < 0);
}

// ===========================================================================
//  Without a usable clip
// ===========================================================================

TEST_CASE("a generator with no file renders transparent black and says nothing", "[ofx][source]") {
    REQUIRE(Fixture::get().ready);
    const std::size_t before = MockHost::instance().messages.size();
    SourceRig rig(320, 180);
    REQUIRE(rig.render() == kOfxStatOK);
    CHECK(rig.allTransparent());
    CHECK(MockHost::instance().messages.size() == before);
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("a file that is not an Osmo 360 clip is reported once and renders nothing", "[ofx][source]") {
    REQUIRE(Fixture::get().ready);
    const std::filesystem::path junk = std::filesystem::temp_directory_path() / "openosv-ofx-not-a-clip.OSV";
    {
        std::ofstream out(junk, std::ios::binary);
        out << "this is not an ISO BMFF container";
    }
    const std::size_t before = MockHost::instance().messages.size();
    SourceRig rig(320, 180);
    // Pasted the way Explorer's "Copy as path" pastes: in quotes.
    rig.param(src::kFile).s = "\"" + junk.string() + "\"";
    REQUIRE(rig.render(0.0) == kOfxStatOK);
    REQUIRE(rig.render(1.0) == kOfxStatOK);
    CHECK(rig.allTransparent());
    // One message for the problem, not one per frame.
    REQUIRE(MockHost::instance().messages.size() == before + 1);
    CHECK(MockHost::instance().messages.back().find("cannot open") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove(junk, ec);
}

// ===========================================================================
//  The sample clip
// ===========================================================================

TEST_CASE("the generator's sphere is the importer's stitch of the clip", "[ofx][source][sample]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    SourceRig rig(1024, 512);
    rig.param(src::kFile).s = clip;
    rig.param(src::kOutput).i = src::kOutputEquirect;
    // Choosing the file fills the Clip read-out.
    REQUIRE(Fixture::get().source.instanceChanged(*rig.effect, src::kFile, kOfxChangeUserEdited, 0.0) == kOfxStatOK);
    const std::string info = rig.param(src::kClipInfo).s;
    INFO(info);
    CHECK(info.find("frames at") != std::string::npos);
    CHECK(info.find("fps") != std::string::npos);

    REQUIRE(rig.render(0.0) == kOfxStatOK);
    // A real stitched sphere: every pixel written, nearly all of it covered
    // by a lens, and colour values in a plausible range for Rec. 709.
    std::size_t covered = 0;
    std::size_t total = 0;
    double sum = 0.0;
    for (int y = 0; y < 512; ++y) {
        for (int x = 0; x < 1024; ++x) {
            const float* p = rig.output.pixel(x, y);
            REQUIRE(p[3] != -7.0f);
            ++total;
            if (p[3] > 0.99f) {
                ++covered;
            }
            sum += p[0] + p[1] + p[2];
        }
    }
    CHECK(covered > total * 95 / 100);
    const double mean = sum / (3.0 * static_cast<double>(total));
    CHECK(mean > 0.05);
    CHECK(mean < 0.95);
    CHECK(MockHost::instance().imagesOut == 0);
}

TEST_CASE("Start Frame shows the same frame a later time does", "[ofx][source][sample]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    // Start Frame counts CLIP frames and the host counts TIMELINE frames, so
    // the timeline runs at the clip's own rate here (read off the Clip
    // read-out) to make "12 frames later" mean the same on both sides.
    double clipFps = 0.0;
    {
        SourceRig probe(64, 32);
        probe.param(src::kFile).s = clip;
        REQUIRE(Fixture::get().source.instanceChanged(*probe.effect, src::kFile, kOfxChangeUserEdited, 0.0) ==
                kOfxStatOK);
        const std::string info = probe.param(src::kClipInfo).s;
        const std::size_t at = info.find("frames at ");
        REQUIRE(at != std::string::npos);
        clipFps = std::stod(info.substr(at + 10));
    }
    REQUIRE(clipFps > 1.0);

    SourceRig later(512, 256, clipFps);
    later.param(src::kFile).s = clip;
    later.param(src::kOutput).i = src::kOutputEquirect;
    REQUIRE(later.render(12.0) == kOfxStatOK);

    SourceRig shifted(512, 256, clipFps);
    shifted.param(src::kFile).s = clip;
    shifted.param(src::kOutput).i = src::kOutputEquirect;
    shifted.param(src::kStartFrame).i = 12;
    REQUIRE(shifted.render(0.0) == kOfxStatOK);

    CHECK(maxDifference(later.output, shifted.output, later.frame) == 0.0);

    // And past the end of the clip there is nothing to show.
    REQUIRE(later.render(1.0e6) == kOfxStatOK);
    CHECK(later.allTransparent());
}

TEST_CASE("the reframed view is the Premiere effect's view of the importer's sphere", "[ofx][source][sample]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    // The native sphere, straight from the generator: a 6K clip stitches to
    // 6000 x 3000 at "Native", and the equirect output renders at the frame.
    SourceRig sphere(6000, 3000);
    sphere.param(src::kFile).s = clip;
    sphere.param(src::kOutput).i = src::kOutputEquirect;
    REQUIRE(sphere.render(3.0) == kOfxStatOK);

    // The reframed view of the same frame, with a moved camera.
    SourceRig view(640, 360);
    view.param(src::kFile).s = clip;
    view.param(cam::kPan).d = 40.0;
    view.param(cam::kTilt).d = -10.0;
    view.param(cam::kDjiFov).d = 80.0;
    REQUIRE(view.render(3.0) == kOfxStatOK);

    // Premiere's two-step path: the importer's native sphere, then Open 360
    // Reframe's CPU render of it with the same controls.
    rf::Settings s = defaultSettings();
    s.panDeg = 40.0;
    s.tiltDeg = -10.0;
    s.djiFovDeg = 80.0;
    const HostImage ref = referenceRender(s, sphere.output, view.frame, {640, 360});
    CHECK(maxDifference(view.output, ref, view.frame) == 0.0);
}

TEST_CASE("a generator render leaves the host's CUDA context current", "[ofx][source][sample][cuda]") {
    REQUIRE(Fixture::get().ready);
    const std::string clip = sampleClip();
    if (clip.empty()) {
        SKIP("no sample clip");
    }
    if (cuInit(0) != CUDA_SUCCESS) {
        SKIP("no CUDA driver");
    }
    CUdevice device = 0;
    CUcontext hostContext = nullptr;
    if (cuDeviceGet(&device, 0) != CUDA_SUCCESS || cuCtxCreate(&hostContext, 0, device) != CUDA_SUCCESS) {
        SKIP("no CUDA device");
    }
    // The engine's CUDA renderer selects its device through the runtime,
    // which makes the PRIMARY context current - the guard must undo that.
    SourceRig rig(512, 256);
    rig.param(src::kFile).s = clip;
    rig.param(src::kOutput).i = src::kOutputEquirect;
    rig.param(osv::ofx::source_params::kRenderDevice).i = 2;  // CUDA
    REQUIRE(rig.render(0.0) == kOfxStatOK);
    CUcontext current = nullptr;
    REQUIRE(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
    CHECK(current == hostContext);
    cuCtxDestroy(hostContext);
}
