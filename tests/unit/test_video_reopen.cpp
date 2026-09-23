// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for the decoder re-open work: the deferred first frame, the shared
// hardware device, the shared container parse, the reader's open record and
// the ReaderPool.  The pixel checks compare content hashes of whole frames,
// so "the same picture" means bit-exact, not "close".  Everything that
// decodes needs the sample clip and SKIPs without it; the hardware tests
// additionally SKIP when no device opens.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/video/Decoder.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"
#include "osv/video/PlanarFrame.h"
#include "osv/video/ReaderPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace osv;
using namespace osv::video;

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// The native sample clip's layout, spelled out (as in test_video.cpp) so
/// these tests do not depend on the meta module's detector.
meta::FormatInfo nativeFormat() {
    meta::FormatInfo f;
    f.mode = meta::Mode::K6;
    f.streamW = 3000;
    f.streamH = 3000;
    f.fps = 59.94;
    f.bitDepth = 10;
    f.dualFisheye = true;
    f.videoTrackIds = {1u, 2u};
    f.sideBySideProxy = false;
    return f;
}

/// FNV-1a over every visible sample of every plane on the 10-bit scale, so
/// planar and biplanar layouts of one picture hash alike and stride padding
/// never leaks in.
std::uint64_t frameHash(const PlanarFrame16& f) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint16_t v) {
        h ^= static_cast<std::uint64_t>(v & 0xFFu);
        h *= 1099511628211ull;
        h ^= static_cast<std::uint64_t>(v >> 8);
        h *= 1099511628211ull;
    };
    for (std::uint32_t y = 0; y < f.height; ++y) {
        for (std::uint32_t x = 0; x < f.width; ++x) {
            mix(f.luma(x, y));
        }
    }
    for (int c = 1; c <= 2; ++c) {
        for (std::uint32_t y = 0; y < f.chromaH; ++y) {
            for (std::uint32_t x = 0; x < f.chromaW; ++x) {
                mix(f.chroma(c, x, y));
            }
        }
    }
    return h;
}

/// Frames scattered over both GOPs of the sample (sync samples at 1 and 61
/// in presentation order), in an order that forces backward seeks, forward
/// skips inside a GOP, a jump across GOPs and both ends of the clip.  Kept
/// short: every entry is a software decode of up to a GOP of 6K frames.
constexpr std::uint32_t kScattered[] = {37, 5, 64, 0, 61, 30};

/// A shorter walk for the tests that only need "first decode lands right,
/// then backwards, then the other GOP".
constexpr std::uint32_t kShortWalk[] = {37, 5, 61};

/// Decoder options for the importer's software path in container-sample
/// mode with the deferred first frame.
DecoderOptions reopenSoftware() {
    DecoderOptions o;
    o.hw = HwAccel::None;
    o.useContainerSamples = true;
    o.deferFirstFrame = true;
    return o;
}

/// Open a decoder with `options` on track 1, or SKIP when the back-end is
/// not available on this machine.
HevcStreamDecoder openOrSkip(const DecoderOptions& options, std::uint32_t track = 1) {
    auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), track, options);
    if (!opened.ok()) {
        SKIP(std::string(hwAccelName(options.hw)) << " decoder unavailable: " << opened.error().toString());
    }
    return std::move(opened).value();
}

/// Pool limits for tests: the defaults, minus the free-memory floor.  The
/// floor is production behaviour, but whether the test machine has 2 GiB
/// free while parallel builds run is not what these tests are about.
ReaderPool::Limits testLimits() {
    ReaderPool::Limits limits;
    limits.minAvailableMemoryMiB = 0;
    return limits;
}

/// Wait up to `limit` for `done()` to become true, polling every 10 ms.
template <class Pred>
bool waitFor(Pred done, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

}  // namespace

// =============================================================================
//  Deferred first frame
// =============================================================================
TEST_CASE("deferFirstFrame reports the probed geometry without decoding anything", "[video][reopen][sample]") {
    OSV_REQUIRE_SAMPLE();
    DecoderOptions probed;
    probed.useContainerSamples = true;
    HevcStreamDecoder a = openOrSkip(probed);
    HevcStreamDecoder b = openOrSkip(reopenSoftware());

    // The probe really ran for one and really did not for the other.
    REQUIRE_FALSE(a.openTimings().firstFrameDeferred);
    REQUIRE(b.openTimings().firstFrameDeferred);
    REQUIRE(b.openTimings().firstFrameMs < a.openTimings().firstFrameMs);

    // Everything open() reports is the same either way.
    REQUIRE(b.width() == a.width());
    REQUIRE(b.height() == a.height());
    REQUIRE(b.width() == 3000);
    REQUIRE(b.height() == 3000);
    REQUIRE(b.sourceBitDepth() == a.sourceBitDepth());
    REQUIRE(b.sourceBitDepth() == 10);
    REQUIRE(b.frameCount() == a.frameCount());
    REQUIRE(b.fps() == a.fps());
    REQUIRE(b.nextIndex() == 0);
    REQUIRE_FALSE(b.lastPts().has_value());

    // And the pictures are the same pictures.
    for (const std::uint32_t i : kShortWalk) {
        auto fa = a.decodeFrame(i);
        auto fb = b.decodeFrame(i);
        REQUIRE(fa.ok());
        REQUIRE(fb.ok());
        INFO("frame " << i);
        REQUIRE(fb.value().frameIndex == i);
        REQUIRE(fb.value().ptsUs == fa.value().ptsUs);
        REQUIRE(frameHash(fb.value()) == frameHash(fa.value()));
    }
}

TEST_CASE("a deferred decoder's next() starts at frame 0", "[video][reopen][sample]") {
    OSV_REQUIRE_SAMPLE();
    HevcStreamDecoder deferred = openOrSkip(reopenSoftware());
    HevcStreamDecoder probed = openOrSkip(DecoderOptions{});
    for (std::uint32_t i = 0; i < 3; ++i) {
        auto fd = deferred.next();
        auto fp = probed.next();
        REQUIRE(fd.ok());
        REQUIRE(fp.ok());
        REQUIRE(fd.value().frameIndex == i);
        REQUIRE(frameHash(fd.value()) == frameHash(fp.value()));
    }
}

// =============================================================================
//  Container samples vs libavformat, bit for bit, at scattered frames
// =============================================================================
TEST_CASE("container-sample decode is bit-exact with libavformat decode at scattered frames",
          "[video][reopen][sample]") {
    OSV_REQUIRE_SAMPLE();
    // Track 2 here: track 1's container-sample decode is already pinned to
    // the sequential libavformat pass in test_video.cpp.  Fresh decoders for
    // each side so neither inherits a position.
    for (const std::uint32_t track : {2u}) {
        HevcStreamDecoder viaFormat = openOrSkip(DecoderOptions{}, track);
        HevcStreamDecoder viaSamples = openOrSkip(reopenSoftware(), track);
        REQUIRE(viaSamples.usesContainerSamples());
        REQUIRE_FALSE(viaFormat.usesContainerSamples());
        for (const std::uint32_t i : kScattered) {
            auto a = viaFormat.decodeFrame(i);
            auto b = viaSamples.decodeFrame(i);
            REQUIRE(a.ok());
            REQUIRE(b.ok());
            INFO("track " << track << " frame " << i);
            REQUIRE(b.value().frameIndex == i);
            REQUIRE(b.value().ptsUs == a.value().ptsUs);
            REQUIRE(frameHash(b.value()) == frameHash(a.value()));
        }
    }
}

TEST_CASE("D3D11VA container-sample decode is bit-exact with D3D11VA libavformat decode",
          "[video][reopen][sample][hwaccel]") {
    OSV_REQUIRE_SAMPLE();
    DecoderOptions viaFormatOpt;
    viaFormatOpt.hw = HwAccel::D3D11VA;
    DecoderOptions viaSamplesOpt = reopenSoftware();
    viaSamplesOpt.hw = HwAccel::D3D11VA;
    viaSamplesOpt.threads = 4;  // the importer's hardware setting
    HevcStreamDecoder viaFormat = openOrSkip(viaFormatOpt);
    HevcStreamDecoder viaSamples = openOrSkip(viaSamplesOpt);
    if (viaFormat.activeHw() != HwAccel::D3D11VA) {
        SKIP("D3D11VA fell back to " << hwAccelName(viaFormat.activeHw()));
    }
    for (const std::uint32_t i : kScattered) {
        auto a = viaFormat.decodeFrame(i);
        auto b = viaSamples.decodeFrame(i);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        INFO("frame " << i);
        REQUIRE(b.value().frameIndex == i);
        REQUIRE(frameHash(b.value()) == frameHash(a.value()));
    }
    // The deferred decoder found its hardware path on the first decode.
    REQUIRE(viaSamples.activeHw() == HwAccel::D3D11VA);
}

// =============================================================================
//  Shared hardware device
// =============================================================================
TEST_CASE("a live shared hardware device serves the next decoder", "[video][reopen][sample][hwaccel]") {
    OSV_REQUIRE_SAMPLE();
    int tested = 0;
    for (const HwAccel hw : {HwAccel::D3D11VA, HwAccel::Cuda}) {
        INFO("back-end " << hwAccelName(hw));
        {
            DecoderOptions o = reopenSoftware();
            o.hw = hw;
            o.hwDeviceSlot = 7;  // a slot no other test in this process uses
            auto first = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, o);
            if (!first.ok()) {
                // Not on this machine; the other back-end may still be.
                WARN(hwAccelName(hw) << " unavailable: " << first.error().toString());
                continue;
            }
            ++tested;
            REQUIRE_FALSE(first.value().openTimings().hwDeviceReused);

            // Same slot while the first is alive: the device is reused.
            auto second = HevcStreamDecoder::open(osvtest::sampleOsv(), 2, o);
            REQUIRE(second.ok());
            REQUIRE(second.value().openTimings().hwDeviceReused);
            REQUIRE(second.value().openTimings().hwDeviceMs < 50.0);

            // A different slot, or sharing turned off: a device of its own.
            DecoderOptions otherSlot = o;
            otherSlot.hwDeviceSlot = 8;
            auto third = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, otherSlot);
            REQUIRE(third.ok());
            REQUIRE_FALSE(third.value().openTimings().hwDeviceReused);
            DecoderOptions privateDevice = o;
            privateDevice.shareHwDevice = false;
            auto fourth = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, privateDevice);
            REQUIRE(fourth.ok());
            REQUIRE_FALSE(fourth.value().openTimings().hwDeviceReused);

            // All of them decode, concurrently, on the shared devices.
            std::array<HevcStreamDecoder*, 4> all{&first.value(), &second.value(), &third.value(), &fourth.value()};
            std::array<bool, 4> ok{};
            std::vector<std::thread> workers;
            for (std::size_t k = 0; k < all.size(); ++k) {
                workers.emplace_back([&, k] { ok[k] = all[k]->decodeFrame(20).ok(); });
            }
            for (std::thread& t : workers) {
                t.join();
            }
            for (std::size_t k = 0; k < ok.size(); ++k) {
                INFO("decoder " << k);
                REQUIRE(ok[k]);
            }

            // Held weakly: with every user gone the next decoder creates anew.
            first = Error{ErrorCode::Internal, "released"};
            second = Error{ErrorCode::Internal, "released"};
            auto fifth = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, o);
            REQUIRE(fifth.ok());
            REQUIRE_FALSE(fifth.value().openTimings().hwDeviceReused);
        }
    }
    if (tested == 0) {
        SKIP("no hardware decoder is available on this machine");
    }
}

TEST_CASE("the two lenses of a reader share one container parse", "[video][reopen][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto reader = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), reopenSoftware());
    REQUIRE(reader.ok());
    const HevcStreamDecoder* d0 = reader.value().decoder(0);
    const HevcStreamDecoder* d1 = reader.value().decoder(1);
    REQUIRE(d0 != nullptr);
    REQUIRE(d1 != nullptr);
    // The lenses open in parallel; exactly one of them parsed.
    REQUIRE(d0->openTimings().containerReused != d1->openTimings().containerReused);
    // A second reader while the first is alive parses nothing.
    auto again = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), reopenSoftware());
    REQUIRE(again.ok());
    REQUIRE(again.value().decoder(0)->openTimings().containerReused);
    REQUIRE(again.value().decoder(1)->openTimings().containerReused);
}

// =============================================================================
//  The reader's open record
// =============================================================================
TEST_CASE("DualStreamReader records what it was opened on", "[video][reopen][sample]") {
    OSV_REQUIRE_SAMPLE();
    DecoderOptions o = reopenSoftware();
    o.threads = 3;
    auto reader = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(reader.ok());
    const DualStreamReader& r = reader.value();
    REQUIRE(r.path() == osvtest::sampleOsv());
    REQUIRE(r.options().threads == 3);
    REQUIRE(r.options().useContainerSamples);
    REQUIRE(r.options().deferFirstFrame);
    REQUIRE(r.trackIds() == std::array<std::uint32_t, 2>{1u, 2u});
    REQUIRE_FALSE(r.fileIdentity().empty());
    // Both lenses honoured the deferred first frame.
    REQUIRE(r.decoder(0)->openTimings().firstFrameDeferred);
    REQUIRE(r.decoder(1)->openTimings().firstFrameDeferred);

    DualStreamReader empty;
    REQUIRE(empty.fileIdentity().empty());
    REQUIRE(empty.trackIds() == std::array<std::uint32_t, 2>{0u, 0u});
    REQUIRE(empty.path().empty());
}

// =============================================================================
//  ReaderPool
// =============================================================================
TEST_CASE("ReaderPool hands a parked reader back to one taker only", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    ReaderPool pool(testLimits());
    const DecoderOptions o = reopenSoftware();
    auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(opened.ok());
    auto reader = std::make_unique<DualStreamReader>(std::move(opened).value());
    REQUIRE(reader->read(10).ok());
    const DualStreamReader* identity = reader.get();

    REQUIRE(pool.park(std::move(reader)));
    REQUIRE(reader == nullptr);
    REQUIRE(pool.idleCount() == 1);

    // A different request misses and leaves the parked reader alone.
    DecoderOptions otherThreads = o;
    otherThreads.threads = 2;
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), otherThreads) == nullptr);
    meta::FormatInfo swapped = nativeFormat();
    swapped.videoTrackIds = {2u, 1u};
    REQUIRE(pool.take(osvtest::sampleOsv(), swapped, o) == nullptr);
    REQUIRE(pool.idleCount() == 1);

    // deferFirstFrame does not matter for an open reader.
    DecoderOptions probed = o;
    probed.deferFirstFrame = false;
    std::unique_ptr<DualStreamReader> taken = pool.take(osvtest::sampleOsv(), nativeFormat(), probed);
    REQUIRE(taken != nullptr);
    REQUIRE(taken.get() == identity);
    REQUIRE(pool.idleCount() == 0);

    // Exclusive: nobody else gets it while it is out.
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), o) == nullptr);

    const ReaderPool::Stats s = pool.stats();
    REQUIRE(s.parked == 1);
    REQUIRE(s.hits == 1);
    REQUIRE(s.misses == 3);
}

TEST_CASE("a pooled reader decodes the same pictures as a fresh one", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    ReaderPool pool(testLimits());
    const DecoderOptions o = reopenSoftware();
    auto fresh = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(fresh.ok());

    // A reader that had shown frame 40, parked, taken back.
    auto used = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(used.ok());
    REQUIRE(used.value().read(40).ok());
    REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(used).value())));
    std::unique_ptr<DualStreamReader> warm = pool.take(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(warm != nullptr);

    // Forward in its GOP, backwards, across GOPs: all bit-exact.
    for (const std::uint32_t i : {44u, 3u, 63u}) {
        auto a = fresh.value().read(i);
        auto b = warm->read(i);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        INFO("frame " << i);
        for (std::size_t lens = 0; lens < 2; ++lens) {
            REQUIRE(frameHash(b.value().lens[lens]) == frameHash(a.value().lens[lens]));
        }
    }
}

TEST_CASE("two readers of one file are pooled and handed out separately", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    ReaderPool pool(testLimits());
    const DecoderOptions o = reopenSoftware();
    // Two instances of one clip alive at once (a Source Settings change).
    auto a = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    auto b = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    auto ra = std::make_unique<DualStreamReader>(std::move(a).value());
    auto rb = std::make_unique<DualStreamReader>(std::move(b).value());
    const DualStreamReader* pa = ra.get();
    const DualStreamReader* pb = rb.get();

    // One closes while the other still decodes.
    REQUIRE(pool.park(std::move(ra)));
    REQUIRE(rb->read(7).ok());
    REQUIRE(pool.park(std::move(rb)));
    REQUIRE(pool.idleCount() == 2);

    // Newest first, and each exactly once.
    std::unique_ptr<DualStreamReader> first = pool.take(osvtest::sampleOsv(), nativeFormat(), o);
    std::unique_ptr<DualStreamReader> second = pool.take(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(first.get() == pb);
    REQUIRE(second.get() == pa);
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), o) == nullptr);
    REQUIRE(first->read(8).ok());
    REQUIRE(second->read(8).ok());
}

TEST_CASE("the same file spelled differently finds its reader", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    ReaderPool pool(testLimits());
    const DecoderOptions o = reopenSoftware();
    auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(opened.ok());
    REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
    // Upper-case path with the other separator: the same file on Windows.
    std::wstring spelled = osvtest::sampleOsv().wstring();
    for (wchar_t& c : spelled) {
        if (c == L'/') {
            c = L'\\';
        } else if (c >= L'a' && c <= L'z') {
            c = static_cast<wchar_t>(c - L'a' + L'A');
        }
    }
    REQUIRE(pool.take(std::filesystem::path(spelled), nativeFormat(), o) != nullptr);
}

TEST_CASE("ReaderPool refuses what it cannot serve and stays within its bound", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    ReaderPool::Limits limits = testLimits();
    limits.maxIdleReaders = 2;
    ReaderPool pool(limits);
    const DecoderOptions o = reopenSoftware();

    // Nothing, and an unopened reader, are refused.
    REQUIRE_FALSE(pool.park(nullptr));
    REQUIRE_FALSE(pool.park(std::make_unique<DualStreamReader>()));
    REQUIRE(pool.stats().rejected == 2);
    REQUIRE(pool.idleCount() == 0);

    // Three parked into two places: the oldest goes.
    std::vector<const DualStreamReader*> order;
    for (int k = 0; k < 3; ++k) {
        auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
        REQUIRE(opened.ok());
        auto reader = std::make_unique<DualStreamReader>(std::move(opened).value());
        order.push_back(reader.get());
        REQUIRE(pool.park(std::move(reader)));
    }
    REQUIRE(pool.idleCount() == 2);
    REQUIRE(pool.stats().evictedForRoom == 1);
    std::unique_ptr<DualStreamReader> newest = pool.take(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(newest.get() == order[2]);

    // A zero bound disables parking.
    ReaderPool::Limits off = limits;
    off.maxIdleReaders = 0;
    pool.setLimits(off);
    REQUIRE(pool.idleCount() == 0);
    REQUIRE_FALSE(pool.park(std::move(newest)));
    REQUIRE(pool.idleCount() == 0);
}

TEST_CASE("idle readers expire on their own and clear() releases everything", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    ReaderPool::Limits limits = testLimits();
    limits.idleTtl = std::chrono::milliseconds(150);
    ReaderPool pool(limits);
    const DecoderOptions o = reopenSoftware();

    SECTION("the reaper releases an expired reader nobody asks for") {
        auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
        REQUIRE(opened.ok());
        REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
        REQUIRE(pool.reaperRunning());
        // Nobody touches the pool; the reaper alone must release it and exit.
        REQUIRE(waitFor([&] { return pool.idleCount() == 0; }, std::chrono::seconds(5)));
        REQUIRE(waitFor([&] { return !pool.reaperRunning(); }, std::chrono::seconds(5)));
        REQUIRE(pool.stats().expired == 1);
        // A later park starts a new reaper.
        auto again = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
        REQUIRE(again.ok());
        REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(again).value())));
        REQUIRE(pool.reaperRunning());
    }
    SECTION("an expired reader is never handed out") {
        ReaderPool::Limits longer = limits;
        longer.idleTtl = std::chrono::milliseconds(60000);
        pool.setLimits(longer);
        auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
        REQUIRE(opened.ok());
        REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
        // Shrinking the limit expires it at once.
        pool.setLimits(limits);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), o) == nullptr);
        REQUIRE(pool.stats().expired == 1);
    }
    SECTION("clear() empties the pool and the reaper is gone when it returns") {
        ReaderPool::Limits longer = limits;
        longer.idleTtl = std::chrono::milliseconds(60000);
        pool.setLimits(longer);
        for (int k = 0; k < 2; ++k) {
            auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
            REQUIRE(opened.ok());
            REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
        }
        REQUIRE(pool.idleCount() == 2);
        pool.clear();
        REQUIRE(pool.idleCount() == 0);
        REQUIRE_FALSE(pool.reaperRunning());
        REQUIRE(pool.stats().cleared == 2);
        // Still usable afterwards.
        auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
        REQUIRE(opened.ok());
        REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
        REQUIRE(pool.idleCount() == 1);
    }
}

TEST_CASE("a pool destroyed with readers parked releases them and its reaper", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    const DecoderOptions o = reopenSoftware();
    {
        ReaderPool pool(testLimits());
        auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
        REQUIRE(opened.ok());
        REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
        REQUIRE(pool.reaperRunning());
    }
    // The reaper outlives the pool only until its next wake-up; nothing here
    // can observe it directly, but the process must neither crash nor hang
    // (ctest's timeout is the judge), and a new pool works normally.
    ReaderPool next(testLimits());
    auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
    REQUIRE(opened.ok());
    REQUIRE(next.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
    REQUIRE(next.take(osvtest::sampleOsv(), nativeFormat(), o) != nullptr);
}

TEST_CASE("ReaderPool never hands one reader to two threads", "[video][reopen][pool][sample]") {
    OSV_REQUIRE_SAMPLE();
    ReaderPool::Limits limits = testLimits();
    limits.maxIdleReaders = 4;
    ReaderPool pool(limits);
    const DecoderOptions o = reopenSoftware();
    constexpr int kReaders = 2;
    for (int k = 0; k < kReaders; ++k) {
        auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), o);
        REQUIRE(opened.ok());
        REQUIRE(pool.park(std::make_unique<DualStreamReader>(std::move(opened).value())));
    }

    // Six threads take / hold / park as fast as they can.  A reader seen by
    // two holders at once is the bug this guards against.
    std::mutex heldMutex;
    std::set<const DualStreamReader*> held;
    std::atomic<int> overlaps{0};
    std::atomic<int> takes{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                std::unique_ptr<DualStreamReader> r = pool.take(osvtest::sampleOsv(), nativeFormat(), o);
                if (!r) {
                    std::this_thread::yield();
                    continue;
                }
                ++takes;
                {
                    std::lock_guard<std::mutex> lock(heldMutex);
                    if (!held.insert(r.get()).second) {
                        ++overlaps;
                    }
                }
                std::this_thread::yield();
                {
                    std::lock_guard<std::mutex> lock(heldMutex);
                    held.erase(r.get());
                }
                pool.park(std::move(r));
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    REQUIRE(overlaps.load() == 0);
    REQUIRE(takes.load() > 0);
    // Every reader came back.
    REQUIRE(pool.idleCount() == static_cast<std::size_t>(kReaders));
}
