// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_steady_importer.cpp - [WP-STEADY] the steady seam corrections and the
// lens alignment through the BUILT importer (mock host, real
// OpenOSVImporter.prm, the sample clip).
//
// What is pinned, and why each matters to a user:
//
//   * once the clip correction exists, an Interactive frame (the Source
//     monitor during playback) and an Exact frame (a paused frame, an export)
//     of the same frame are the same pixels - the "ghosting in the Source
//     monitor, shifting in the Program monitor" field report was the two
//     paths rendering different corrections;
//   * a frame renders the same whatever was rendered before it, and
//     whichever instance measured the clip - the clip correction comes from
//     fixed sample frames, never from what Premiere asked for first;
//   * a project saved before the controls existed renders exactly as it did,
//     even in a process that has already measured and cached this clip's
//     rotation and clip correction for newer settings.

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"

#include "MockHost.h"

#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "PrefsBlob.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::test;

namespace {

/// Premiere ticks per frame at 59.94 fps (254016000000 * 1001 / 60000).
constexpr PrTime kTicksPerFrame5994 = 4237833600LL;

#define REQUIRE_SAMPLE_CLIP()                                                                                          \
    do {                                                                                                               \
        if (!sampleClipAvailable()) {                                                                                  \
            SKIP("the sample clip is not present at " << sampleClipPath().string());                                  \
        }                                                                                                              \
    } while (0)

/// A frame's raw 32f pixels, exactly as the host received them.
struct Pixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> values;  ///< Host rows, BGRA.

    [[nodiscard]] bool operator==(const Pixels& o) const noexcept {
        return width == o.width && height == o.height && values == o.values;
    }
};

/// Request frame `frame` at 3000 x 1500 with `intent`, 32f.
[[nodiscard]] ImporterHarness::SourceVideoRequest requestFor(std::uint32_t frame, imRenderIntent intent) {
    ImporterHarness::SourceVideoRequest r;
    r.frameTime = kTicksPerFrame5994 * static_cast<PrTime>(frame);
    r.format = PrPixelFormat_BGRA_4444_32f;
    r.width = 3000;
    r.height = 1500;
    r.intent = intent;
    // Real-time playback: a Playing request below 1.0 is a DRAFT, which skips
    // the corrections altogether and would test nothing.
    r.playbackRatio = 1.0;
    return r;
}

/// Render one frame and copy its pixels out.
[[nodiscard]] Pixels render(ImporterHarness& harness, ImporterHarness::ClipHandle& clip, const PrSDKPPixSuite* ppix,
                            const ImporterHarness::SourceVideoRequest& request, const PrefsBlob& prefs) {
    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
    REQUIRE(hand != nullptr);
    char* data = nullptr;
    csSDK_int32 rowBytes = 0;
    REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &data) == suiteError_NoError);
    REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
    REQUIRE(data != nullptr);
    Pixels p;
    p.width = static_cast<std::uint32_t>(request.width);
    p.height = static_cast<std::uint32_t>(request.height);
    p.values.resize(static_cast<std::size_t>(p.width) * p.height * 4u);
    for (std::uint32_t y = 0; y < p.height; ++y) {
        const auto* row = reinterpret_cast<const float*>(data + static_cast<std::ptrdiff_t>(rowBytes) * y);
        std::copy(row, row + static_cast<std::size_t>(p.width) * 4u,
                  p.values.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * p.width * 4u));
    }
    ppix->Dispose(hand);
    return p;
}

/// Pixels that differ between two frames of one size.
[[nodiscard]] std::size_t differing(const Pixels& a, const Pixels& b) {
    REQUIRE(a.values.size() == b.values.size());
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.values.size(); i += 4) {
        if (a.values[i] != b.values[i] || a.values[i + 1] != b.values[i + 1] || a.values[i + 2] != b.values[i + 2] ||
            a.values[i + 3] != b.values[i + 3]) {
            ++n;
        }
    }
    return n;
}

/// The new defaults with the stages that keep their own per-bucket history
/// (sun ghosts, the photometric field, lens shading, exposure match) OFF, so a
/// difference between two renders can only come from the seam corrections and
/// the rig.  Stabilisation off: the frame is the body frame.
[[nodiscard]] PrefsBlob steadyOnlyPrefs() {
    PrefsBlob p = PrefsBlob::defaults();
    p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
    p.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Classical);
    p.flareRemoval = 0;
    p.gainMatch = 0;
    p.photoSeam = static_cast<std::uint8_t>(PrefsPhotoSeam::Off);
    p.lensShading = static_cast<std::uint8_t>(PrefsLensShading::Off);
    p.parallaxGrid = static_cast<std::uint8_t>(PrefsParallaxGrid::Steady);
    p.lensAlign = static_cast<std::uint8_t>(PrefsLensAlign::Auto);
    REQUIRE(p.sanitise());
    return p;
}

/// Sets an environment variable for the lifetime of the object (the test and
/// the .prm share one CRT, so the importer's getenv_s sees it).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : m_name(name) { ::_putenv_s(m_name, value); }
    ~ScopedEnv() { ::_putenv_s(m_name, ""); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
};

}  // namespace

TEST_CASE("once the clip correction exists, Interactive and Exact renders of a frame are the same pixels",
          "[importer][steady][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
    const PrefsBlob prefs = steadyOnlyPrefs();

    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

    // An Exact frame waits for the rotation and the clip correction (once
    // per clip) - the Program monitor's path asks Exact for every frame.
    harness.host().clearCache();
    const Pixels exact = render(harness, clip, ppix, requestFor(20, imRenderIntent_Export), prefs);

    // Interactive requests of other frames move the instance's one-frame
    // cache on, then the Source monitor's request of the same frame: it is
    // rendered afresh, with the clip correction, not a stand-in ...
    harness.host().clearCache();
    (void)render(harness, clip, ppix, requestFor(33, imRenderIntent_Playing), prefs);
    (void)render(harness, clip, ppix, requestFor(5, imRenderIntent_Playing), prefs);
    harness.host().clearCache();
    const auto before = harness.host().cacheStats();
    const Pixels interactive = render(harness, clip, ppix, requestFor(20, imRenderIntent_Playing), prefs);
    INFO(differing(exact, interactive) << " pixels differ");
    CHECK(interactive == exact);
    // ... and final, so it went into the host's frame cache like an Exact one.
    CHECK(harness.host().cacheStats().entries > before.entries);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("a steady frame is the same whatever was asked for first and whichever instance measured the clip",
          "[importer][steady][sample]") {
    REQUIRE_SAMPLE_CLIP();
    // Every instance measures its own rotation and clip correction: nothing
    // is shared between the two below, so equal pixels mean equal
    // measurements from different request histories.
    ScopedEnv noSharing("OPENOSV_STEADY_NO_SHARED_CACHE", "1");
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
    const PrefsBlob prefs = steadyOnlyPrefs();

    // Instance A: exports in order, the first at the start of the clip.
    Pixels a;
    {
        auto clip = harness.openClip(sampleClipPath(), 3101);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        (void)render(harness, clip, ppix, requestFor(0, imRenderIntent_Export), prefs);
        (void)render(harness, clip, ppix, requestFor(64, imRenderIntent_Export), prefs);
        a = render(harness, clip, ppix, requestFor(32, imRenderIntent_Export), prefs);
    }
    // Instance B: scrubbing elsewhere first (Interactive, stand-ins while the
    // analyses run), then the export.
    Pixels b;
    {
        auto clip = harness.openClip(sampleClipPath(), 3102);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        (void)render(harness, clip, ppix, requestFor(50, imRenderIntent_Scrubbing), prefs);
        (void)render(harness, clip, ppix, requestFor(10, imRenderIntent_Playing), prefs);
        harness.host().clearCache();
        b = render(harness, clip, ppix, requestFor(32, imRenderIntent_Export), prefs);
    }
    INFO(differing(a, b) << " pixels differ");
    CHECK(a == b);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("a project saved before the steady corrections renders exactly as it did",
          "[importer][steady][legacy][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    // An older project's blob: its bytes 50-51 are zero, which reads as the
    // per-moment corrections and the calibration alone.
    PrefsBlob legacy = PrefsBlob::defaults();
    legacy.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    legacy.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
    legacy.flareRemoval = 0;
    legacy.parallaxGrid = 0;
    legacy.lensAlign = 0;
    REQUIRE(legacy.sanitise());
    REQUIRE(legacy.parallaxGridChoice() == PrefsParallaxGrid::FollowsScene);
    REQUIRE(legacy.lensAlignChoice() == PrefsLensAlign::Off);
    PrefsBlob current = legacy;
    current.parallaxGrid = static_cast<std::uint8_t>(PrefsParallaxGrid::Auto);
    current.lensAlign = static_cast<std::uint8_t>(PrefsLensAlign::Auto);

    const auto renderOnce = [&](const PrefsBlob& prefs, csSDK_int32 id) {
        auto clip = harness.openClip(sampleClipPath(), id);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        return render(harness, clip, ppix, requestFor(12, imRenderIntent_Export), prefs);
    };
    // Before this process has measured anything for the clip ...
    const Pixels first = renderOnce(legacy, 3201);
    // ... the new settings measure and cache the rotation and the clip
    // correction (and render a different stitch - the test is not vacuous) ...
    const Pixels steady = renderOnce(current, 3202);
    CHECK(differing(first, steady) > 1000u);
    // ... and none of it reaches an older project's frame.
    const Pixels again = renderOnce(legacy, 3203);
    INFO(differing(first, again) << " pixels differ");
    CHECK(again == first);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}
