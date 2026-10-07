// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Variable frame rate: a recording whose camera dropped frames.
//
//   * ClipTimeline (pure): the constant-rate timeline at the nominal rate,
//     gaps held - identity for constant-rate input, the capture clock when
//     it is trustworthy, the container's table otherwise, the nominal period
//     as the most common duration rather than sample 0's.
//   * SampleTable / HevcStreamDecoder: decodeFrame(i) returns sample i on a
//     table with long samples, in both demuxing modes.  The fixture
//     tests/fixtures/fx_vfr_h264.mp4 is 40 frames of 64 x 64 H.264 whose
//     every pixel of frame N is 16 + 5 N, muxed at 25000 ticks per second
//     with the presentation times N + 2 (N >= 1) + (N >= 10) + 2 (N >= 25) +
//     (N >= 26) frame periods: sample 0 lasts three periods, sample 9 two,
//     sample 24 (a keyframe) three and sample 25 two.  Made with
//       ffmpeg -f lavfi -i "color=c=black:s=64x64:r=25:d=1.6,format=yuv420p,
//         geq=lum='16+5*N':cb=128:cr=128,setpts='(N+2*gte(N\,1)+gte(N\,10)+
//         2*gte(N\,25)+gte(N\,26))/(25*TB)'" -frames:v 40 -c:v libx264 -bf 0
//         -g 12 -keyint_min 12 -sc_threshold 0 -qp 4 -fps_mode passthrough
//         -video_track_timescale 25000 -an -use_editlist 0 -map_metadata -1
//         -fflags +bitexact -flags:v +bitexact fx_vfr_h264.mp4
//     (keyframes at 0 / 12 / 24 / 36, no B-frames so no composition offsets).
//   * [vfr-sample]: a real variable-frame-rate clip named by the environment
//     variable OSV_VFR_SAMPLE (an .OSV or .LRF); SKIPs without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Result.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/video/ClipTimeline.h"
#include "osv/video/Decoder.h"
#include "osv/video/HwAccel.h"
#include "osv/video/PlanarFrame.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::video;
using Catch::Matchers::WithinAbs;

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// `n` durations of `d` ticks.
std::vector<std::uint64_t> uniform(std::size_t n, std::uint64_t d) { return std::vector<std::uint64_t>(n, d); }

/// Capture timestamps (us) from a camera clock that starts at `base` and
/// places sample i at `base + offsetsUs[i]`.
std::vector<std::uint64_t> capture(std::uint64_t base, const std::vector<std::uint64_t>& offsetsUs) {
    std::vector<std::uint64_t> out;
    out.reserve(offsetsUs.size());
    for (const std::uint64_t o : offsetsUs) {
        out.push_back(base + o);
    }
    return out;
}

/// The committed variable-frame-rate fixture (see the file comment).
std::filesystem::path vfrFixture() { return osvtest::fixtureDir() / "fx_vfr_h264.mp4"; }

/// Number of samples in the fixture and its per-frame luma rule.
constexpr std::uint32_t kFixtureFrames = 40;
constexpr std::uint32_t kFixtureTimescale = 25000;
[[nodiscard]] std::uint16_t fixtureLuma10(std::uint32_t sample) {
    // 8-bit 16 + 5 N, widened by the decoder to the 10-bit scale (<< 2).
    return static_cast<std::uint16_t>((16u + 5u * sample) << 2);
}

/// Presentation ticks of the fixture's samples (the rule in the file comment).
[[nodiscard]] std::int64_t fixturePts(std::uint32_t n) {
    const std::int64_t periods = static_cast<std::int64_t>(n) + 2 * (n >= 1) + (n >= 10) + 2 * (n >= 25) + (n >= 26);
    return periods * 1000;
}

/// Luma of the middle pixel of a decoded frame (host planes only).
[[nodiscard]] std::uint16_t centreLuma(const PlanarFrame16& f) {
    REQUIRE(f.plane[0] != nullptr);
    REQUIRE(f.width > 0);
    REQUIRE(f.height > 0);
    const std::size_t x = f.width / 2u;
    const std::size_t y = f.height / 2u;
    return f.plane[0][y * f.strideElems[0] + x];
}

/// The environment's real variable-frame-rate clip, or empty.
[[nodiscard]] std::filesystem::path vfrSample() {
    const char* env = std::getenv("OSV_VFR_SAMPLE");
    if (env == nullptr || *env == '\0') {
        return {};
    }
    return std::filesystem::path(env);
}

}  // namespace

// =============================================================================
//  ClipTimeline: pure
// =============================================================================

TEST_CASE("a constant-rate table is its own timeline", "[video][vfr][timeline]") {
    // 100 samples at 60000 / 1001, a camera clock that agrees.
    const std::vector<std::uint64_t> d = uniform(100, 1001);
    std::vector<std::uint64_t> offsets;
    for (std::uint64_t i = 0; i < 100; ++i) {
        offsets.push_back(i * 16683u);
    }
    const ClipTimeline t = buildClipTimeline(60000, d, capture(5'000'000, offsets));
    CHECK(t.identity());
    CHECK(t.clock == TimelineClock::Identity);
    CHECK(t.frameCount() == 100);
    CHECK(t.sampleCount == 100);
    CHECK(t.nominalTicks == 1001);
    CHECK(t.note.empty());
    CHECK_THAT(t.fps(), WithinAbs(60000.0 / 1001.0, 1e-12));
    for (std::uint32_t k = 0; k < 100; ++k) {
        CHECK(t.sampleFor(k) == k);
    }
    // Past the end clamps to the last sample.
    CHECK(t.sampleFor(100) == 99);
    CHECK(t.sampleFor(0xFFFFFFFFu) == 99);

    // The last sample's own duration says nothing about presentation times.
    std::vector<std::uint64_t> oddLast = d;
    oddLast.back() = 0;
    CHECK(buildClipTimeline(60000, oddLast, {}).identity());
    oddLast.back() = 5000;
    CHECK(buildClipTimeline(60000, oddLast, {}).identity());
}

TEST_CASE("dropped frames are held on a nominal-rate timeline timed by the capture clock", "[video][vfr][timeline]") {
    // 10 samples at a nominal 20 ms (50 fps at 50000 ticks); the camera
    // dropped one frame after sample 3 and two after sample 7.
    const std::vector<std::uint64_t> d = {1000, 1000, 1000, 2000, 1000, 1000, 1000, 3000, 1000, 1000};
    const std::vector<std::uint64_t> cap =
        capture(77'000'000, {0, 20000, 40000, 60000, 100000, 120000, 140000, 160000, 220000, 240000});
    const ClipTimeline t = buildClipTimeline(50000, d, cap);
    REQUIRE_FALSE(t.identity());
    CHECK(t.clock == TimelineClock::Capture);
    CHECK(t.note.empty());
    CHECK(t.nominalTicks == 1000);
    CHECK_THAT(t.fps(), WithinAbs(50.0, 1e-12));
    // 240 ms of capture is 12 periods: 13 frames, three of them held.
    REQUIRE(t.frameCount() == 13);
    const std::vector<std::uint32_t> want = {0, 1, 2, 3, 3, 4, 5, 6, 7, 7, 7, 8, 9};
    for (std::uint32_t k = 0; k < 13; ++k) {
        INFO("timeline frame " << k);
        CHECK(t.sampleFor(k) == want[k]);
    }
    CHECK(t.heldFrames == 3);
    CHECK(t.skippedSamples == 0);
    CHECK_THAT(t.durationSeconds(), WithinAbs(0.26, 1e-12));
    // A held frame shows the moment of the sample it repeats.
    CHECK_THAT(t.frameMomentUs(4), WithinAbs(60000.0, 1e-9));
    CHECK_THAT(t.frameMomentUs(5), WithinAbs(100000.0, 1e-9));
}

TEST_CASE("the capture clock wins over a container that over-counts the gaps", "[video][vfr][timeline]") {
    // The .LRF habit: the container writes a 60 ms gap as 80 ms; the camera's
    // timestamps say 60.  At a nominal 40 ms the capture clock's timeline is
    // the shorter, correct one.  200 samples, three such gaps.
    std::vector<std::uint64_t> d = uniform(200, 1000);
    d[20] = d[90] = d[150] = 2000;
    std::vector<std::uint64_t> offsets;
    std::uint64_t t = 0;
    for (std::size_t i = 0; i < d.size(); ++i) {
        offsets.push_back(t);
        t += (d[i] == 2000) ? 60000u : 40000u;
    }
    const ClipTimeline byCapture = buildClipTimeline(25000, d, capture(1'000'000, offsets));
    REQUIRE_FALSE(byCapture.identity());
    CHECK(byCapture.clock == TimelineClock::Capture);
    // Capture span 199 * 40 + 3 * 20 = 8020 ms = 200.5 periods -> 201 + 1
    // frames; the container's 8080 ms would have given 202 + 1.
    CHECK(byCapture.frameCount() == 202);
    const ClipTimeline byContainer = buildClipTimeline(25000, d, {});
    REQUIRE_FALSE(byContainer.identity());
    CHECK(byContainer.clock == TimelineClock::Container);
    CHECK(byContainer.note == "no per-frame capture timestamps");
    CHECK(byContainer.frameCount() == 203);
}

TEST_CASE("the nominal period is the most common duration, not sample 0's", "[video][vfr][timeline]") {
    // A long first sample must not halve the rate of the whole clip.
    std::vector<std::uint64_t> d = uniform(21, 1000);
    d[0] = 3000;
    CHECK(nominalSampleDuration(d) == 1000);
    const ClipTimeline t = buildClipTimeline(50000, d, {});
    REQUIRE_FALSE(t.identity());
    CHECK(t.nominalTicks == 1000);
    CHECK_THAT(t.fps(), WithinAbs(50.0, 1e-12));
    // Sample 20 sits at 3000 + 19 * 1000 ticks = 22 periods: 23 frames, the
    // first three showing sample 0.
    REQUIRE(t.frameCount() == 23);
    CHECK(t.sampleFor(0) == 0);
    CHECK(t.sampleFor(1) == 0);
    CHECK(t.sampleFor(2) == 0);
    CHECK(t.sampleFor(3) == 1);
    CHECK(t.sampleFor(22) == 20);
    CHECK(t.heldFrames == 2);

    // Ties go to the shorter period; zero durations never vote; the last
    // sample's duration never votes; a single sample answers its own.
    CHECK(nominalSampleDuration(std::vector<std::uint64_t>{2000, 1000, 2000, 1000, 7}) == 1000);
    CHECK(nominalSampleDuration(std::vector<std::uint64_t>{0, 0, 0, 1001, 1001}) == 1001);
    CHECK(nominalSampleDuration(std::vector<std::uint64_t>{1001, 1001, 5005}) == 1001);
    CHECK(nominalSampleDuration(std::vector<std::uint64_t>{512}) == 512);
    CHECK(nominalSampleDuration(std::vector<std::uint64_t>{}) == 0);
}

TEST_CASE("untrustworthy capture timestamps fall back to the container", "[video][vfr][timeline]") {
    const std::vector<std::uint64_t> d = {1000, 1000, 1000, 2000, 1000, 1000, 1000, 1000};
    const std::vector<std::uint64_t> good = {0, 20000, 40000, 60000, 100000, 120000, 140000, 160000};

    SECTION("not increasing") {
        std::vector<std::uint64_t> junk = good;
        junk[5] = junk[4];
        const ClipTimeline t = buildClipTimeline(50000, d, capture(9'000'000, junk));
        CHECK(t.clock == TimelineClock::Container);
        CHECK(t.note.find("do not increase at sample 5") != std::string::npos);
        CHECK(t.frameCount() == 9);
    }
    SECTION("going backwards") {
        std::vector<std::uint64_t> junk = good;
        junk[2] = 10;
        const ClipTimeline t = buildClipTimeline(50000, d, capture(9'000'000, junk));
        CHECK(t.clock == TimelineClock::Container);
        CHECK(t.note.find("do not increase") != std::string::npos);
    }
    SECTION("one too few") {
        std::vector<std::uint64_t> shortList = good;
        shortList.pop_back();
        const ClipTimeline t = buildClipTimeline(50000, d, capture(9'000'000, shortList));
        CHECK(t.clock == TimelineClock::Container);
        CHECK(t.note == "7 capture timestamps for 8 samples");
    }
    SECTION("a span that is not the clip's") {
        // Twice as long as the container says: some other clock.
        std::vector<std::uint64_t> stretched;
        for (const std::uint64_t g : good) {
            stretched.push_back(2u * g);
        }
        const ClipTimeline t = buildClipTimeline(50000, d, capture(9'000'000, stretched));
        CHECK(t.clock == TimelineClock::Container);
        CHECK(t.note.find("capture timestamps span") != std::string::npos);
    }
    SECTION("a span within the tolerance is trusted") {
        // 1 % shorter than the container, like the night .LRF.
        std::vector<std::uint64_t> close = good;
        close.back() = 158400;
        const ClipTimeline t = buildClipTimeline(50000, d, capture(9'000'000, close));
        CHECK(t.clock == TimelineClock::Capture);
        CHECK(t.note.empty());
    }
}

TEST_CASE("a table that cannot be placed stays sample for sample", "[video][vfr][timeline]") {
    // Zero durations between samples and no timestamps: nothing orders them.
    const ClipTimeline zeros = buildClipTimeline(50000, std::vector<std::uint64_t>{1000, 0, 0, 1000, 1000}, {});
    CHECK(zeros.identity());
    CHECK(zeros.frameCount() == 5);
    CHECK_FALSE(zeros.note.empty());
    // ...but trustworthy timestamps can still place them.
    const ClipTimeline stamped = buildClipTimeline(50000, std::vector<std::uint64_t>{1000, 0, 0, 1000, 1000},
                                                   capture(1, {0, 20000, 60000, 80000, 100000}));
    CHECK_FALSE(stamped.identity());
    CHECK(stamped.clock == TimelineClock::Capture);
    CHECK(stamped.frameCount() == 6);

    // A corrupt multi-hour gap is not presented as hours of one held frame.
    std::vector<std::uint64_t> gap = uniform(10, 1000);
    gap[4] = 1000000;
    const ClipTimeline absurd = buildClipTimeline(50000, gap, {});
    CHECK(absurd.identity());
    CHECK(absurd.note.find("frames for 10 samples") != std::string::npos);

    // No timing at all.
    CHECK(buildClipTimeline(0, uniform(4, 1000), {}).identity());
    CHECK(buildClipTimeline(50000, std::vector<std::uint64_t>{}, {}).frameCount() == 0);
    CHECK(buildClipTimeline(50000, std::vector<std::uint64_t>{}, {}).sampleFor(7) == 0);
}

TEST_CASE("a proxy recording is matched to the timeline by capture moment", "[video][vfr][timeline]") {
    // The original: 50 fps, one dropped frame after sample 3 (the timeline in
    // the dropped-frames test above, shortened).
    const std::vector<std::uint64_t> d = {1000, 1000, 1000, 2000, 1000, 1000};
    const std::vector<std::uint64_t> cap = capture(10'000'000, {0, 20000, 40000, 60000, 100000, 120000});
    const ClipTimeline original = buildClipTimeline(50000, d, cap);
    REQUIRE(original.frameCount() == 7);
    // The proxy: 25 fps on the same camera clock, starting 20 ms earlier, so
    // the original's sample 0 is at +20 ms on the proxy's clock.
    const SampleClock proxy = buildSampleClock(25000, uniform(5, 1000),
                                               capture(9'980'000, {0, 40000, 80000, 120000, 160000}));
    REQUIRE(proxy.valid());
    CHECK(proxy.clock == TimelineClock::Capture);
    const std::vector<std::uint32_t> map = mapTimelineToClock(original, proxy, 20000.0);
    // Original moments on the proxy clock: 20, 40, 60, 80, 80 (held), 120, 140
    // ms -> nearest proxy samples (ties to the later): 1, 1, 2, 2, 2, 3, 4.
    const std::vector<std::uint32_t> want = {1, 1, 2, 2, 2, 3, 4};
    CHECK(map == want);

    // Nearest-sample edge cases.
    CHECK(proxy.nearestSample(-5.0) == 0);
    CHECK(proxy.nearestSample(1e12) == 4);
    CHECK(proxy.nearestSample(std::nan("")) == 0);
    CHECK(SampleClock{}.nearestSample(1.0) == 0);
    CHECK(mapTimelineToClock(original, SampleClock{}, 0.0).empty());
}

// =============================================================================
//  The sample table as a clock (fixture)
// =============================================================================

TEST_CASE("the sample table answers which sample a time belongs to", "[video][vfr][container]") {
    REQUIRE(std::filesystem::exists(vfrFixture()));
    auto opened = OsvFile::open(vfrFixture());
    REQUIRE(opened.ok());
    const auto videos = opened.value().movie().tracksOfKind(TrackKind::Video);
    REQUIRE(videos.size() == 1u);
    const TrackInfo& track = *videos.front();
    REQUIRE(track.timescale == kFixtureTimescale);
    const SampleTable& table = track.samples;
    REQUIRE(table.count() == kFixtureFrames);
    CHECK(table.dtsStrictlyIncreasing());
    CHECK_FALSE(table.hasCompositionOffsets());
    for (std::uint32_t n = 0; n < kFixtureFrames; ++n) {
        INFO("sample " << n);
        CHECK(static_cast<std::int64_t>(table.sampleDts(n)) == fixturePts(n));
        CHECK(table.sampleAtOrBeforeDts(table.sampleDts(n)) == n);
        if (n + 1u < kFixtureFrames) {
            CHECK(table.sampleAtOrBeforeDts(table.sampleDts(n + 1u) - 1u) == n);
        }
    }
    // Before the first and far past the last sample: clamped.
    CHECK(table.sampleAtOrBeforeDts(0) == 0);
    CHECK(table.sampleAtOrBeforeDts(2999) == 0);
    CHECK(table.sampleAtOrBeforeDts(10'000'000) == kFixtureFrames - 1u);

    // The timeline of the fixture by its container clock: the last sample at
    // 45 periods, so 46 frames, six of them held.
    const ClipTimeline t = clipTimelineFor(track, nullptr);
    REQUIRE_FALSE(t.identity());
    CHECK(t.clock == TimelineClock::Container);
    CHECK(t.nominalTicks == 1000);
    CHECK(t.frameCount() == 46);
    CHECK(t.heldFrames == 6);
    CHECK(t.skippedSamples == 0);
}

// =============================================================================
//  The decoder: frame i is sample i (fixture)
// =============================================================================

TEST_CASE("decodeFrame(i) returns sample i on a table with long samples, in both modes", "[video][vfr][decoder]") {
    REQUIRE(std::filesystem::exists(vfrFixture()));
    for (const bool containerSamples : {false, true}) {
        INFO("container samples " << containerSamples);
        DecoderOptions opt;
        opt.hw = HwAccel::None;
        opt.useContainerSamples = containerSamples;
        auto opened = HevcStreamDecoder::open(vfrFixture(), 1, opt);
        REQUIRE(opened.ok());
        HevcStreamDecoder& dec = opened.value();
        REQUIRE(dec.frameCount() == kFixtureFrames);

        // Random access: backwards, then every third, then forwards - each a
        // different mix of seeks and in-GOP steps across the long samples.
        std::vector<std::uint32_t> order;
        for (std::uint32_t n = kFixtureFrames; n-- > 0;) {
            order.push_back(n);
        }
        for (std::uint32_t n = 0; n < kFixtureFrames; n += 3) {
            order.push_back(n);
        }
        for (std::uint32_t n = 0; n < kFixtureFrames; ++n) {
            order.push_back(n);
        }
        for (const std::uint32_t n : order) {
            INFO("frame " << n);
            auto frame = dec.decodeFrame(n);
            INFO((frame.ok() ? std::string("ok") : frame.error().toString()));
            REQUIRE(frame.ok());
            CHECK(frame.value().frameIndex == n);
            CHECK(centreLuma(frame.value()) == fixtureLuma10(n));
            REQUIRE(dec.lastPts().has_value());
            CHECK(*dec.lastPts() == fixturePts(n));
        }

        // Sequential: next() from a fresh seek walks every sample once.
        REQUIRE(dec.seek(0).ok());
        for (std::uint32_t n = 0; n < kFixtureFrames; ++n) {
            INFO("next() at " << n);
            auto frame = dec.next();
            INFO((frame.ok() ? std::string("ok") : frame.error().toString()));
            REQUIRE(frame.ok());
            CHECK(centreLuma(frame.value()) == fixtureLuma10(n));
        }
        CHECK(dec.next().error().code == ErrorCode::NotFound);
    }
}

TEST_CASE("a listed sync sample that is not a random access point is not where a decode starts",
          "[video][vfr][decoder]") {
    // One lens of a recording that dropped frames: the camera writes one sync
    // table for both lens tracks, and after a gap one encoder's IDRs sit a
    // sample after the listed entries.  Reproduced on the fixture by listing
    // sample 11 (a P picture) instead of 12 (the IDR) in its sync table.
    REQUIRE(std::filesystem::exists(vfrFixture()));
    std::vector<char> bytes;
    {
        std::ifstream in(vfrFixture(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // stss: size, 'stss', version + flags, entry count, then 1-based entries.
    const std::string tag = "stss";
    const auto at = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
    REQUIRE(at != bytes.end());
    const std::size_t body = static_cast<std::size_t>(at - bytes.begin()) + 4u;
    const auto u32 = [&](std::size_t o) {
        return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[o])) << 24) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[o + 1])) << 16) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[o + 2])) << 8) |
               static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[o + 3]));
    };
    REQUIRE(u32(body + 4u) == 4u);           // four sync samples: 1, 13, 25, 37
    REQUIRE(u32(body + 12u) == 13u);         // sample index 12, the second IDR
    bytes[body + 15u] = static_cast<char>(12);  // now lists index 11, a P picture
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "openosv-vfr-badsync";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path patched = dir / "fx_vfr_badsync_h264.mp4";
    {
        std::ofstream out(patched, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
    }

    for (const bool containerSamples : {false, true}) {
        DecoderOptions opt;
        opt.hw = HwAccel::None;
        opt.useContainerSamples = containerSamples;
        auto opened = HevcStreamDecoder::open(patched, 1, opt);
        REQUIRE(opened.ok());
        HevcStreamDecoder& dec = opened.value();
        // The listed entry is checked against the picture: 11 starts nowhere
        // (back to the IDR at 0); 12..23 start at the real IDR, 12.
        CHECK(dec.previousSyncIndex(11) == 0u);
        CHECK(dec.previousSyncIndex(12) == 12u);
        CHECK(dec.previousSyncIndex(20) == 12u);
        CHECK(dec.previousSyncIndex(30) == 24u);
        // Every frame, each by a fresh random access, is the right picture.
        for (const std::uint32_t n : {11u, 12u, 13u, 23u, 10u, 11u, 30u, 11u}) {
            INFO("container samples " << containerSamples << ", frame " << n);
            auto frame = dec.decodeFrame(n);
            INFO((frame.ok() ? std::string("ok") : frame.error().toString()));
            REQUIRE(frame.ok());
            CHECK(centreLuma(frame.value()) == fixtureLuma10(n));
        }
    }
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("a stream-timing failure carries its own error code", "[video][vfr][decoder]") {
    // The code exists, has a stable name, and is not the decoder's.
    CHECK(std::string(errorCodeName(ErrorCode::Timing)) == "Timing");
    CHECK(ErrorCode::Timing != ErrorCode::Decoder);
}

// =============================================================================
//  A real variable-frame-rate recording (OSV_VFR_SAMPLE)
// =============================================================================

TEST_CASE("a real variable-frame-rate clip: timeline and sample-exact decoding", "[video][vfr][vfr-sample]") {
    const std::filesystem::path clip = vfrSample();
    std::error_code ec;
    if (clip.empty() || !std::filesystem::exists(clip, ec)) {
        SKIP("OSV_VFR_SAMPLE does not name a variable-frame-rate clip");
    }
    auto opened = OsvFile::open(clip);
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    auto meta = meta::MetadataTrack::load(file);
    REQUIRE(meta.ok());
    auto format = meta::FormatDetector::detect(file, &meta.value());
    REQUIRE(format.ok());
    const std::uint32_t trackId = format.value().videoTrackIds[0];
    const TrackInfo* video = file.track(trackId);
    REQUIRE(video != nullptr);

    // ---- the timeline ----------------------------------------------------------------
    const ClipTimeline t = clipTimelineFor(*video, &meta.value());
    INFO("note: " << t.note);
    REQUIRE_FALSE(t.identity());
    CHECK(t.clock == TimelineClock::Capture);
    CHECK(t.skippedSamples == 0);
    CHECK(t.frameCount() == t.sampleCount + t.heldFrames);
    // Every sample is shown, in order, and frames never go backwards.
    std::uint32_t previous = 0;
    for (std::uint32_t k = 0; k < t.frameCount(); ++k) {
        const std::uint32_t s = t.sampleFor(k);
        REQUIRE(s >= previous);
        REQUIRE(s <= previous + 1u);
        previous = s;
    }
    CHECK(previous == t.sampleCount - 1u);
    // The timeline lasts as long as the camera recorded, to within a frame.
    const std::vector<std::uint64_t> stamps = captureTimestampsOf(meta.value());
    REQUIRE(stamps.size() == t.sampleCount);
    const double captured = static_cast<double>(stamps.back() - stamps.front()) / 1e6 + 1.0 / t.fps();
    CHECK_THAT(t.durationSeconds(), WithinAbs(captured, 1.0 / t.fps()));

    // ---- decoding: around the first long sample and at the last listed sync
    // samples (on one lens of a dropped-frame recording those can be trailing
    // pictures), every lens track, both modes, each frame by random access ---
    const std::vector<std::uint64_t> d = sampleDurationsOf(*video);
    const auto longOne = std::find_if(d.begin(), d.end(), [&](std::uint64_t v) { return v > t.nominalTicks; });
    REQUIRE(longOne != d.end());
    const std::uint32_t first = static_cast<std::uint32_t>(longOne - d.begin());
    std::vector<std::uint32_t> probes = {first > 0 ? first - 1u : 0u, first, first + 1u, first + 2u,
                                         t.sampleCount - 1u};
    const std::vector<std::uint32_t>& syncs = video->samples.syncSamples();
    if (syncs.size() >= 2u) {
        probes.push_back(syncs[syncs.size() - 2u]);
        probes.push_back(syncs.back());
    }
    std::vector<std::uint32_t> tracks = {trackId};
    if (format.value().videoTrackIds[1] != 0 && format.value().videoTrackIds[1] != trackId) {
        tracks.push_back(format.value().videoTrackIds[1]);
    }
    for (const std::uint32_t lensTrack : tracks) {
        for (const bool containerSamples : {false, true}) {
            DecoderOptions opt;
            opt.hw = HwAccel::None;
            opt.useContainerSamples = containerSamples;
            auto dec = HevcStreamDecoder::open(clip, lensTrack, opt);
            REQUIRE(dec.ok());
            for (const std::uint32_t n : probes) {
                INFO("track " << lensTrack << ", container samples " << containerSamples << ", frame " << n);
                // A fresh position for every probe: each is a real random access.
                REQUIRE(dec.value().seek(0).ok());
                auto frame = dec.value().decodeFrame(n);
                INFO((frame.ok() ? std::string("ok") : frame.error().toString()));
                REQUIRE(frame.ok());
                REQUIRE(dec.value().lastPts().has_value());
                CHECK(*dec.value().lastPts() == static_cast<std::int64_t>(video->samples.sampleDts(n)));
            }
        }
    }
}
