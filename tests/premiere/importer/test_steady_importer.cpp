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
//     rotation and clip correction for newer settings;
//   * Hide Mount (the calibration's occlusion mask) switched on a live clip
//     renders what a clip opened with that setting renders, both ways, and
//     the per-clip rotation it caches does not depend on it.

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

// ---- [WP-TEMPORAL] the per-moment schedule (Follows scene) -------------------

/// steadyOnlyPrefs on the per-moment schedule: every frame takes its bucket's
/// grid / table / carve, glided from the bucket before.
[[nodiscard]] static PrefsBlob followsPrefs() {
    PrefsBlob p = steadyOnlyPrefs();
    p.parallaxGrid = static_cast<std::uint8_t>(PrefsParallaxGrid::FollowsScene);
    REQUIRE(p.sanitise());
    return p;
}

TEST_CASE("per moment, an exact frame is the same rendered alone, mid-export or after a jump",
          "[importer][steady][temporal][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
    const PrefsBlob prefs = followsPrefs();

    // A: an export from the start of the clip, through frame 21 (buckets 0-2).
    std::vector<Pixels> a;
    {
        auto clip = harness.openClip(sampleClipPath(), 3201);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        for (std::uint32_t f = 0; f <= 21; ++f) {
            Pixels p = render(harness, clip, ppix, requestFor(f, imRenderIntent_Export), prefs);
            if (f >= 19) {
                a.push_back(std::move(p));
            }
        }
    }
    // B: an export whose in-point is in the middle of bucket 2 (frame 19):
    // the bucket is measured on its anchor (16), its partner on 8.
    std::vector<Pixels> b;
    {
        auto clip = harness.openClip(sampleClipPath(), 3202);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        for (std::uint32_t f = 19; f <= 21; ++f) {
            b.push_back(render(harness, clip, ppix, requestFor(f, imRenderIntent_Export), prefs));
        }
    }
    // C: frame 21 alone, after scrubbing somewhere else entirely.
    Pixels c;
    {
        auto clip = harness.openClip(sampleClipPath(), 3203);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        (void)render(harness, clip, ppix, requestFor(50, imRenderIntent_Scrubbing), prefs);
        (void)render(harness, clip, ppix, requestFor(45, imRenderIntent_Export), prefs);
        harness.host().clearCache();
        c = render(harness, clip, ppix, requestFor(21, imRenderIntent_Export), prefs);
    }
    REQUIRE(a.size() == 3u);
    REQUIRE(b.size() == 3u);
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("frame " << (19 + i) << ": " << differing(a[i], b[i]) << " pixels differ");
        CHECK(a[i] == b[i]);
    }
    INFO("frame 21 alone: " << differing(a[2], c) << " pixels differ");
    CHECK(a[2] == c);

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

// ---- Hide Mount: the calibration's occlusion mask as a Source Setting ---------

/// steadyOnlyPrefs on the CPU renderer and the per-moment schedule, with the
/// given Hide Mount.  CPU so the test never renders on the GPU; per moment so
/// the only per-clip measurement is the lens rotation, which is exactly what
/// the second test below is about.  `photometric` switches the sky seam fix
/// and the lens shading correction back on (their defaults): both keep
/// per-bucket measurements across a Source Settings change, measured through
/// the analysis blend's occlusion mask.
[[nodiscard]] static PrefsBlob hideMountPrefs(PrefsHideMount mode, bool photometric) {
    PrefsBlob p = steadyOnlyPrefs();
    p.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Cpu);
    p.parallaxGrid = static_cast<std::uint8_t>(PrefsParallaxGrid::FollowsScene);
    if (photometric) {
        p.photoSeam = static_cast<std::uint8_t>(PrefsPhotoSeam::RimAndGain);
        p.lensShading = static_cast<std::uint8_t>(PrefsLensShading::Auto);
    }
    p.hideMount = static_cast<std::uint8_t>(mode);
    REQUIRE(p.sanitise());
    REQUIRE(p.hideMountChoice() == mode);
    return p;
}

TEST_CASE("Hide Mount switched on a live clip renders exactly what a clip opened with it renders",
          "[importer][hidemount][sample]") {
    REQUIRE_SAMPLE_CLIP();
    // Every instance measures everything itself: what one renders cannot come
    // from another instance's caches, only from its own history.
    ScopedEnv noSharing("OPENOSV_STEADY_NO_SHARED_CACHE", "1");
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
    const PrefsBlob on = hideMountPrefs(PrefsHideMount::On, true);
    const PrefsBlob off = hideMountPrefs(PrefsHideMount::Off, true);

    // Instance X: On, then Off, then On again - Source Settings changes
    // arriving on one live clip.
    Pixels a;
    Pixels b;
    Pixels c;
    {
        auto clip = harness.openClip(sampleClipPath(), 3301);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &on) == imNoErr);
        harness.host().clearCache();
        a = render(harness, clip, ppix, requestFor(20, imRenderIntent_Export), on);
        harness.host().clearCache();
        b = render(harness, clip, ppix, requestFor(20, imRenderIntent_Export), off);
        harness.host().clearCache();
        c = render(harness, clip, ppix, requestFor(20, imRenderIntent_Export), on);
    }
    // Instance Y: opened with Off from the start.
    Pixels offFresh;
    {
        auto clip = harness.openClip(sampleClipPath(), 3302);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &off) == imNoErr);
        harness.host().clearCache();
        offFresh = render(harness, clip, ppix, requestFor(20, imRenderIntent_Export), off);
    }

    // Off changes the frame where the occlusion polygons cut the lenses (the
    // sample's mount sits there): the switch is not vacuous ...
    const std::size_t changed = differing(a, b);
    INFO(changed << " pixels differ between On and Off");
    CHECK(changed > 1000u);
    // ... nothing measured through the mask (the photometric field, the lens
    // shading models, the per-bucket analyses) leaks into the Off frame ...
    INFO(differing(b, offFresh) << " pixels differ between Off after On and Off from the start");
    CHECK(b == offFresh);
    // ... and back On, nothing measured without it leaks into the masked
    // frame: it is the first frame, bit for bit.
    INFO(differing(a, c) << " pixels differ between the first On render and the On render after Off");
    CHECK(c == a);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("the lens rotation cached by a Hide Mount Off clip is the one an On clip measures",
          "[importer][hidemount][steady][sample]") {
    REQUIRE_SAMPLE_CLIP();
    // The rotation cache is keyed by the file and the rig alone, so the
    // rotation must not depend on Hide Mount: it is always measured through
    // the calibration's mask.  Otherwise an On clip opened after an Off one
    // would stitch with a rotation fitted to the mount's parallax.
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
    const PrefsBlob on = hideMountPrefs(PrefsHideMount::On, false);
    const PrefsBlob off = hideMountPrefs(PrefsHideMount::Off, false);

    const auto renderOnce = [&](const PrefsBlob& prefs, csSDK_int32 id) {
        auto clip = harness.openClip(sampleClipPath(), id);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        return render(harness, clip, ppix, requestFor(32, imRenderIntent_Export), prefs);
    };
    // A: On, measuring its own rotation and sharing nothing.
    Pixels fresh;
    {
        ScopedEnv noSharing("OPENOSV_STEADY_NO_SHARED_CACHE", "1");
        fresh = renderOnce(on, 3311);
    }
    // B: Off with the process-wide caches on - it measures the clip's
    // rotation and leaves it there for every later instance ...
    const Pixels unmasked = renderOnce(off, 3312);
    CHECK(differing(fresh, unmasked) > 1000u);  // the test is not vacuous
    // ... C: On, served that rotation from the cache: the frame A rendered.
    const Pixels cached = renderOnce(on, 3313);
    INFO(differing(fresh, cached) << " pixels differ");
    CHECK(cached == fresh);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// ---- Hide Mount Auto: the per-clip mount mask ----------------------------------

TEST_CASE("Hide Mount Auto keeps the 6K sample's polygons: its frame is On's, bit for bit",
          "[importer][hidemount][sample]") {
    REQUIRE_SAMPLE_CLIP();
    // On the 6K sample none of the polygon arc's windows agrees at 0.8 over
    // the nine sample frames (the stick and its mount are in them), so Auto
    // keeps the calibration's polygons and must render exactly what On does,
    // photometric field and lens shading included.
    ScopedEnv noSharing("OPENOSV_STEADY_NO_SHARED_CACHE", "1");
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
    const PrefsBlob on = hideMountPrefs(PrefsHideMount::On, true);
    const PrefsBlob automatic = hideMountPrefs(PrefsHideMount::Auto, true);

    const auto renderOnce = [&](const PrefsBlob& prefs, csSDK_int32 id) {
        auto clip = harness.openClip(sampleClipPath(), id);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        return render(harness, clip, ppix, requestFor(20, imRenderIntent_Export), prefs);
    };
    const Pixels masked = renderOnce(on, 3401);
    const Pixels measured = renderOnce(automatic, 3402);
    INFO(differing(masked, measured) << " pixels differ between On and Auto");
    CHECK(measured == masked);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("Hide Mount Auto: a second instance of the clip stitches with the verdict the first measured",
          "[importer][hidemount][sample]") {
    // The effect's direct path opens a second engine for the clip it shows;
    // it must stitch with the same polygons as the importer's own instance,
    // not with a verdict of its own.  On the sample's LRF proxy Auto releases
    // part of the arc, so the frames say whether the polygons were rebuilt.
    const std::filesystem::path proxy = sampleProxyPath();
    std::error_code ec;
    if (proxy.empty() || !std::filesystem::exists(proxy, ec)) {
        SKIP("the .LRF proxy is not present at " << proxy.string());
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
    const PrefsBlob on = hideMountPrefs(PrefsHideMount::On, false);
    const PrefsBlob automatic = hideMountPrefs(PrefsHideMount::Auto, false);

    const auto renderOnce = [&](const PrefsBlob& prefs, csSDK_int32 id) {
        auto clip = harness.openClip(proxy, id);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        harness.host().clearCache();
        // The proxy advertises its own size (2000 x 1000 beside the 6K
        // original): ask for exactly that, as Premiere does.
        REQUIRE(info.vidInfo.imageWidth > 0);
        REQUIRE(info.vidInfo.imageHeight > 0);
        ImporterHarness::SourceVideoRequest request = requestFor(20, imRenderIntent_Export);
        request.width = info.vidInfo.imageWidth;
        request.height = info.vidInfo.imageHeight;
        return render(harness, clip, ppix, request, prefs);
    };
    // A: an instance that shares nothing measures its own verdict.
    Pixels own;
    {
        ScopedEnv noSharing("OPENOSV_STEADY_NO_SHARED_CACHE", "1");
        own = renderOnce(automatic, 3411);
    }
    // The verdict changes the stitch (the test is not vacuous) ...
    const Pixels masked = renderOnce(on, 3412);
    CHECK(differing(own, masked) > 1000u);
    // ... B measures with the shared caches on and leaves the verdict there;
    // C, a second instance of the same clip, is served it.  Both render the
    // frame A measured for itself: one verdict per clip, whoever asks.
    const Pixels first = renderOnce(automatic, 3413);
    const Pixels second = renderOnce(automatic, 3414);
    INFO(differing(own, first) << " / " << differing(first, second) << " pixels differ");
    CHECK(first == own);
    CHECK(second == first);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}
