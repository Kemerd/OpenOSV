// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_flare_importer.cpp - sun ghost removal through the BUILT importer
// (WP-FLARE; plugins/importer/FlareStage.h, docs/research/FLARE.md).
//
// What is proven, end to end through imGetSourceVideo on the sample clip:
//
//   * with the removal on, an export changes pixels ONLY inside the fitted
//     ghosts' footprints - the model is recomputed here with the library, on
//     the same decoded frame, and every changed pixel of the equirect must map
//     into one of its ghosts through the clip's own lens rig;
//   * with it off - and for a blob saved before the option existed, and for
//     a draft request - the frame is bit-identical to a render without it;
//   * an interactive request never waits for the analysis: the first one
//     comes back uncorrected, bit for bit, and later ones receive the
//     background result.

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"

#include "MockHost.h"

#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "PrefsBlob.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/Flare.h"
#include "osv/video/DualStreamReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::test;
using osv::premiere::mock::MockHost;

namespace {

/// One 59.94 fps frame in Premiere ticks (254016000000 * 1001 / 60000).
constexpr PrTime kTicksPerFrame5994 = 4237833600LL;

/// Half the native ladder: 3000 x 1500 renders fast and still resolves the
/// ghosts (the pill covers ~900 px of it).
constexpr std::uint32_t kWidth = 3000;
constexpr std::uint32_t kHeight = 1500;

#define REQUIRE_SAMPLE_CLIP()                                                                  \
    do {                                                                                        \
        if (!sampleClipAvailable()) {                                                           \
            SKIP("the sample clip is not present at " << sampleClipPath().string());            \
        }                                                                                       \
    } while (false)

/// A decoded frame, top-down RGBA floats.
struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;

    [[nodiscard]] const float* pixel(std::uint32_t x, std::uint32_t y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
    }
};

/// Read a 32f BGRA PPix back, flipping the host's bottom-up rows.
[[nodiscard]] Frame readPPix(MockHost& host, const PrSDKPPixSuite* ppix, PPixHand hand) {
    Frame frame;
    REQUIRE(ppix != nullptr);
    REQUIRE(hand != nullptr);
    const auto info = host.inspect(hand);
    REQUIRE(info.has_value());
    REQUIRE(info->format == PrPixelFormat_BGRA_4444_32f);
    frame.width = info->width;
    frame.height = info->height;
    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) == suiteError_NoError);
    REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
    REQUIRE(pixels != nullptr);
    frame.rgba.assign(static_cast<std::size_t>(frame.width) * frame.height * 4u, 0.0f);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const std::uint32_t hostRow = frame.height - 1u - y;
        const auto* src = reinterpret_cast<const float*>(pixels + static_cast<std::ptrdiff_t>(rowBytes) *
                                                                      static_cast<std::ptrdiff_t>(hostRow));
        float* dst = frame.rgba.data() + static_cast<std::size_t>(y) * frame.width * 4u;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    return frame;
}

/// Render one frame and read it back.
[[nodiscard]] Frame render(ImporterHarness& harness, ImporterHarness::ClipHandle& clip, const PrSDKPPixSuite* ppix,
                           std::uint32_t index, imRenderIntent intent, const PrefsBlob& prefs,
                           PrRenderQuality quality = kPrRenderQuality_High) {
    ImporterHarness::SourceVideoRequest r;
    r.frameTime = kTicksPerFrame5994 * static_cast<PrTime>(index);
    r.width = static_cast<csSDK_int32>(kWidth);
    r.height = static_cast<csSDK_int32>(kHeight);
    r.intent = intent;
    r.quality = quality;
    // Real-time playback: below 1.0 a Playing request is a DRAFT, which
    // skips the removal altogether and would never exercise the worker.
    r.playbackRatio = 1.0;
    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, r, prefs, hand) == imNoErr);
    REQUIRE(hand != nullptr);
    Frame f = readPPix(harness.host(), ppix, hand);
    ppix->Dispose(hand);
    return f;
}

/// Everything else that changes pixels OFF - no seam work, no gains, no
/// stabilisation (so body == view in the equirect), no parallax - so a
/// difference between two renders is the ghost removal and nothing else.
[[nodiscard]] PrefsBlob flareOnlyPrefs(bool flareOn) {
    PrefsBlob p = PrefsBlob::defaults();
    p.seamSearch = 0;
    p.gainMatch = 0;
    p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
    p.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    p.flareRemoval = flareOn ? 1u : 0u;
    return p;
}

/// Pixels that differ between two renders.
[[nodiscard]] std::uint64_t changedPixels(const Frame& a, const Frame& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    std::uint64_t n = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        if (std::memcmp(&a.rgba[i], &b.rgba[i], 4 * sizeof(float)) != 0) {
            ++n;
        }
    }
    return n;
}

/// The sample's rig and the library's own model of frame `index`, computed
/// exactly as the importer computes it (same decode, same colour decode,
/// same analysis parameters).
struct Reference {
    osv::geom::LensRig rig;
    osv::render::FlareModel model;
};

[[nodiscard]] Reference referenceModel(std::uint32_t index) {
    Reference ref;
    auto file = osv::OsvFile::open(sampleClipPath());
    REQUIRE(file.ok());
    auto track = osv::meta::MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    auto format = osv::meta::FormatDetector::detect(file.value(), &track.value());
    REQUIRE(format.ok());
    auto cal = osv::meta::CalibrationSelector::select(track.value().stream());
    REQUIRE(cal.ok());
    const osv::meta::FormatInfo& f = format.value();
    auto scaling = osv::geom::StreamScaling::derive(static_cast<int>(f.streamW), static_cast<int>(f.streamH),
                                                    static_cast<int>(f.sensorW), static_cast<int>(f.sensorH),
                                                    f.digitalFocalLength,
                                                    0.5 * (cal.value().slave.fx + cal.value().master.fx));
    REQUIRE(scaling.ok());
    auto rig = osv::geom::LensRig::build(cal.value(), scaling.value(), osv::geom::FocalSource::DigitalFocalLength,
                                         f.digitalFocalLength, osv::geom::ExtrinsicConvention{});
    REQUIRE(rig.ok());
    ref.rig = rig.value();
    auto reader = osv::video::DualStreamReader::open(sampleClipPath(), f);
    REQUIRE(reader.ok());
    auto pair = reader.value().read(index);
    REQUIRE(pair.ok());
    // The analysis only reads the input decode (D-Log M, the default curve).
    const OsvColorParams color =
        osv::color::makeColorParams(osv::color::kDefaultDlogMFit, osv::color::OutputTransfer::PQ, 0.0f);
    osv::ThreadPool pool;
    auto model = osv::render::analyseFlare(ref.rig, pair.value(), color, osv::render::FlareParams{}, pool);
    REQUIRE(model.ok());
    ref.model = model.value();
    return ref;
}

/// True when Standard-layout equirect pixel (x, y) of a w x h frame (no
/// stabilisation: body == view) lands, through `lens`, within `marginPx` of
/// one of its fitted ghosts' footprints.
[[nodiscard]] bool insideAGhost(const Reference& ref, int lens, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                                std::uint32_t h, double marginPx) {
    constexpr double kPi = 3.14159265358979323846;
    const double lon = (static_cast<double>(x) + 0.5) / w * 2.0 * kPi - kPi;
    const double lat = 0.5 * kPi - (static_cast<double>(y) + 0.5) / h * kPi;
    // osvRayForPixel, Standard layout: +Y at the centre, Z up.
    const osv::Vec3d d{std::sin(lon) * std::cos(lat), std::cos(lon) * std::cos(lat), std::sin(lat)};
    osv::Vec2d px;
    double theta = 0.0;
    if (!ref.rig.projectBody(lens, d, px, theta)) {
        return false;
    }
    for (const osv::render::FlareGhost& g : ref.model.lens[static_cast<std::size_t>(lens)].ghosts) {
        if (std::hypot(px.x - g.cx, px.y - g.cy) <= g.reach() + marginPx) {
            return true;
        }
    }
    return false;
}

}  // namespace

// =============================================================================
//  Pixels
// =============================================================================

TEST_CASE("sun ghost removal changes only the fitted ghosts, and off renders exactly as without it",
          "[importer][video][flare][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    const PrefsBlob off = flareOnlyPrefs(false);
    const PrefsBlob on = flareOnlyPrefs(true);
    constexpr std::uint32_t kFrame = 0;
    const Frame frameOff = render(harness, clip, ppix, kFrame, imRenderIntent_Export, off);
    const Frame frameOn = render(harness, clip, ppix, kFrame, imRenderIntent_Export, on);
    REQUIRE(frameOff.width == kWidth);
    REQUIRE(frameOff.height == kHeight);

    // ---- every changed pixel is inside a fitted ghost of the master lens ----
    // The sun and all its ghosts are in the master lens on this clip (the
    // library test pins that).  A margin of 3 stream px covers the kernel's
    // own one-pixel slack on the reach and bilinear sampling at the edge.
    const Reference ref = referenceModel(kFrame);
    REQUIRE(ref.model.lens[1].sunFound);
    REQUIRE_FALSE(ref.model.lens[1].ghosts.empty());
    REQUIRE(ref.model.lens[0].ghosts.empty());
    std::uint64_t changedInside = 0;
    std::uint64_t changedOutside = 0;
    std::uint64_t brighter = 0;
    double maxChannelRise = 0.0;
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const float* a = frameOff.pixel(x, y);
            const float* b = frameOn.pixel(x, y);
            if (std::memcmp(a, b, 4 * sizeof(float)) == 0) {
                continue;
            }
            if (insideAGhost(ref, 1, x, y, kWidth, kHeight, 3.0)) {
                ++changedInside;
            } else {
                ++changedOutside;
            }
            // Removal only ever takes light away.  At the faint outer edge of
            // a ghost the subtraction is itself ~1e-6, and the colour chain
            // after it (native -> output matrix, OOTF, PQ) rounds at that
            // level, so "brighter" means brighter than float rounding.
            const double lumaA = 0.2627 * a[0] + 0.6780 * a[1] + 0.0593 * a[2];
            const double lumaB = 0.2627 * b[0] + 0.6780 * b[1] + 0.0593 * b[2];
            if (lumaB > lumaA + 1e-5) {
                ++brighter;
            }
            for (int c = 0; c < 3; ++c) {
                maxChannelRise = std::max(maxChannelRise, static_cast<double>(b[c] - a[c]));
            }
        }
    }
    INFO("changed pixels: " << changedInside << " inside the fitted ghosts, " << changedOutside << " outside, "
                            << brighter << " brighter; largest single-channel rise " << maxChannelRise);
    REQUIRE(changedOutside == 0);
    REQUIRE(changedInside > 500);
    REQUIRE(brighter == 0);
    // Rounding only (measured: 5.8e-6 at most on the sample).
    REQUIRE(maxChannelRise < 1e-4);

    SECTION("a project saved before the option existed renders with it off") {
        std::uint8_t bytes[PrefsBlob::kSize];
        std::memcpy(bytes, &on, PrefsBlob::kSize);
        bytes[offsetof(PrefsBlob, flareRemoval)] = 0;
        const PrefsBlob old = PrefsBlob::fromBytes(bytes, sizeof(bytes));
        REQUIRE(old.flareRemoval == 0u);
        harness.host().clearCache();
        REQUIRE(changedPixels(render(harness, clip, ppix, kFrame, imRenderIntent_Export, old), frameOff) == 0);
    }

    SECTION("a draft request never pays for it") {
        harness.host().clearCache();
        const Frame draft = render(harness, clip, ppix, kFrame, imRenderIntent_Export, on, kPrRenderQuality_Low);
        REQUIRE(changedPixels(draft, frameOff) == 0);
    }

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// =============================================================================
//  Schedule
// =============================================================================

TEST_CASE("an interactive request never waits for the ghost analysis, and later frames receive it",
          "[importer][video][flare][async][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    const PrefsBlob off = flareOnlyPrefs(false);
    const PrefsBlob on = flareOnlyPrefs(true);
    const Frame off1 = render(harness, clip, ppix, 1, imRenderIntent_Export, off);
    const Frame off2 = render(harness, clip, ppix, 2, imRenderIntent_Export, off);
    harness.host().clearCache();

    // The first interactive request finds nothing measured, so it must come
    // back WITHOUT removal, bit for bit - anything else means it waited for
    // the fit.  Its time is reported (a shared machine makes it noisy).
    const auto t0 = std::chrono::steady_clock::now();
    const Frame first = render(harness, clip, ppix, 1, imRenderIntent_Playing, on);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    WARN("first interactive frame with the removal on: " << ms << " ms (decode + sun check + working images + "
                                                          "stitch + copy)");
    REQUIRE(changedPixels(off1, first) == 0);

    // Later frames of the same bucket (the sun has not moved past the
    // tolerance on this clip) pick up the background result.  Frames 2 and 1
    // alternate so the importer's one-frame cache cannot hand back the
    // stand-in it has just built.
    bool removed = false;
    std::uint32_t frame = 2;
    std::uint32_t attempts = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!removed && std::chrono::steady_clock::now() < deadline) {
        harness.host().clearCache();
        const Frame f = render(harness, clip, ppix, frame, imRenderIntent_Playing, on);
        removed = changedPixels(frame == 1 ? off1 : off2, f) > 500;
        frame = frame == 1 ? 2 : 1;
        ++attempts;
        if (!removed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    INFO("interactive requests until the background analysis arrived: " << attempts);
    REQUIRE(removed);

    // And an export of a frame the stand-in was built for is never served
    // the stand-in: it is removed exactly as a fresh export would be.
    harness.host().clearCache();
    const Frame exported = render(harness, clip, ppix, 1, imRenderIntent_Export, on);
    REQUIRE(changedPixels(off1, exported) > 500);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}
