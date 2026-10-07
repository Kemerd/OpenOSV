// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// [VFR] What Premiere is told about a clip whose camera dropped frames.
//
// A recording that dropped frames (the container records each gap as one
// longer sample) is presented at its NOMINAL rate with the previous picture
// held over each gap, so the picture lasts exactly as long as the sound; a
// constant-rate clip is presented sample for sample, exactly as before.
//
//   * [sample]       the maintainer's sample clip (constant rate): its
//                    timeline is its sample list.
//   * [vfr-sample]   OSV_VFR_SAMPLE names a variable-frame-rate .OSV or .LRF
//                    (SKIPs without it): video and audio end together.
//   * [vfr-proxy]    OSV_VFR_PROXY names a variable-frame-rate .LRF with its
//                    .OSV beside it under the same name, as the camera writes
//                    them (SKIPs without it): the proxy takes the original's
//                    timeline exactly.

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"

#include "PrefsBlob.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::test;

/// SKIP when the maintainer's sample clip is not on this machine.
#define VFR_REQUIRE_SAMPLE_CLIP()                                                                  \
    do {                                                                                           \
        if (!sampleClipAvailable()) {                                                              \
            SKIP("the sample clip is not present at " << sampleClipPath().string());               \
        }                                                                                          \
    } while (false)

namespace {

/// Premiere's tick base (PrSDKTimeSuite::GetTicksPerSecond).
constexpr PrTime kTicks = 254016000000LL;

/// A path from the environment, or empty when unset / missing.
[[nodiscard]] std::filesystem::path envClip(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    std::filesystem::path p(value);
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
}

/// The imAnalysis text of an open clip (two-step protocol).
[[nodiscard]] std::string analysisText(ImporterHarness& harness, ImporterHarness::ClipHandle& clip) {
    PrefsBlob prefs = PrefsBlob::defaults();
    imAnalysisRec rec{};
    rec.privatedata = clip.privateData();
    rec.prefs = &prefs;
    rec.buffer = nullptr;
    rec.buffersize = 0;
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    REQUIRE(rec.buffersize > 0);
    std::vector<char> buffer(static_cast<std::size_t>(rec.buffersize), '\0');
    rec.buffer = buffer.data();
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    return std::string(buffer.data());
}

/// The video length imGetInfo8 declares, in seconds.
[[nodiscard]] double videoSeconds(const imFileInfoRec8& info) {
    REQUIRE(info.vidScale > 0);
    REQUIRE(info.vidSampleSize > 0);
    return static_cast<double>(info.vidDurationInFrames) * static_cast<double>(info.vidSampleSize) /
           static_cast<double>(info.vidScale);
}

/// The audio length imGetInfo8 declares, in seconds (0 without audio).
[[nodiscard]] double audioSeconds(const imFileInfoRec8& info) {
    if (!(info.audInfo.sampleRate > 0.0f)) {
        return 0.0;
    }
    return static_cast<double>(info.audDuration) / static_cast<double>(info.audInfo.sampleRate);
}

}  // namespace

TEST_CASE("a constant-rate clip's timeline is its sample list", "[importer][vfr][sample]") {
    VFR_REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    // 65 samples at 60000 / 1001: 65 frames, nothing held, nothing added.
    CHECK(info.vidScale == 60000);
    CHECK(info.vidSampleSize == 1001);
    CHECK(info.vidDurationInFrames == 65);
    CHECK(info.vidInfo.frameRate == kTicks * 1001 / 60000);
    const std::string text = analysisText(harness, clip);
    INFO(text);
    CHECK(text.find("Frames: 65\r\n") != std::string::npos);
    CHECK(text.find("on the timeline") == std::string::npos);
}

TEST_CASE("a clip that dropped frames is presented at its nominal rate and ends with its sound",
          "[importer][vfr][vfr-sample]") {
    const std::filesystem::path path = envClip("OSV_VFR_SAMPLE");
    if (path.empty()) {
        SKIP("OSV_VFR_SAMPLE does not name a variable-frame-rate clip");
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(path);
    INFO("open result " << clip.openResult());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);

    // The nominal rate: a standard camera rate, not the average a sample
    // count over the recording's length would give (49.48 or 24.51 fps).
    const double fps = static_cast<double>(info.vidScale) / static_cast<double>(info.vidSampleSize);
    INFO("rate " << info.vidScale << " / " << info.vidSampleSize << ", " << info.vidDurationInFrames << " frames");
    const double nearestWhole = std::round(fps);
    CHECK((std::fabs(fps - nearestWhole) < 1e-9 || std::fabs(fps * 1.001 - nearestWhole) < 1e-6));
    CHECK(info.vidInfo.frameRate == kTicks * info.vidSampleSize / info.vidScale);

    // Video and audio end together - within two frames - where a sample
    // count at the nominal rate ends seconds early.
    const double video = videoSeconds(info);
    const double audio = audioSeconds(info);
    INFO("video " << video << " s, audio " << audio << " s");
    REQUIRE(audio > 0.0);
    CHECK(std::fabs(video - audio) < 2.0 / fps);

    // File > Properties says what happened.
    const std::string text = analysisText(harness, clip);
    INFO(text);
    CHECK(text.find("on the timeline") != std::string::npos);
    CHECK(text.find("held over") != std::string::npos);
}

TEST_CASE("a variable-frame-rate .LRF beside its .OSV takes the original's timeline exactly",
          "[importer][vfr][vfr-proxy]") {
    const std::filesystem::path proxy = envClip("OSV_VFR_PROXY");
    if (proxy.empty()) {
        SKIP("OSV_VFR_PROXY does not name an .LRF with its .OSV beside it");
    }
    std::filesystem::path original = proxy;
    original.replace_extension(".OSV");
    std::error_code ec;
    if (!std::filesystem::exists(original, ec)) {
        SKIP("no .OSV beside " << proxy.string());
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto originalClip = harness.openClip(original, 8);
    REQUIRE(originalClip.open());
    imFileInfoRec8 originalInfo{};
    REQUIRE(harness.getInfo8(originalClip, originalInfo) == imNoErr);
    auto proxyClip = harness.openClip(proxy);
    REQUIRE(proxyClip.open());
    imFileInfoRec8 proxyInfo{};
    REQUIRE(harness.getInfo8(proxyClip, proxyInfo) == imNoErr);

    // Adobe's attach rule: the same frame rate and the same length.
    CHECK(proxyInfo.vidScale == originalInfo.vidScale);
    CHECK(proxyInfo.vidSampleSize == originalInfo.vidSampleSize);
    CHECK(proxyInfo.vidInfo.frameRate == originalInfo.vidInfo.frameRate);
    CHECK(proxyInfo.vidDurationInFrames == originalInfo.vidDurationInFrames);
    // And that length is the original's sound's.
    const double fps = static_cast<double>(originalInfo.vidScale) / static_cast<double>(originalInfo.vidSampleSize);
    CHECK(std::fabs(videoSeconds(originalInfo) - audioSeconds(originalInfo)) < 2.0 / fps);
}
