// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for video::GpuDecoderPool and the GpuClipDecoder additions it needs
// (the open record, context health, trimForIdle): a parked decoder comes back
// warm with its decode position and its most recent frames, its pixels are
// the pixels a fresh decoder produces, decoders in a caller's context are
// never parked, the match is exact, a device short of VRAM (an injected
// floor) releases parked decoders - at park, at take, on trim() and from the
// reaper alone - and clear() empties the pool.
//
// Everything here needs the sample clip, a CUDA driver and NVDEC; each of
// those SKIPs cleanly when missing.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/video/GpuClipDecoder.h"
#include "osv/video/GpuDecoderPool.h"
#include "osv/video/PlanarFrame.h"

#if defined(OSV_VIDEO_HAVE_CUDA)
#include <cuda.h>
#endif

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace osv;
using namespace osv::video;

#if defined(OSV_VIDEO_HAVE_CUDA)
namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// The native sample clip's layout (as in test_gpu_decoder.cpp).
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

/// Pool limits for tests: the defaults, minus the two machine-dependent
/// floors (free RAM and free VRAM while other processes build and test are
/// not what these tests are about; the VRAM tests inject their own floor).
GpuDecoderPool::Limits testLimits() {
    GpuDecoderPool::Limits limits;
    limits.minAvailableMemoryMiB = 0;
    limits.minFreeVramMiB = 0;
    return limits;
}

/// A VRAM floor no device can meet: every VRAM check reports pressure.
constexpr std::uint64_t kImpossibleVramFloorMiB = std::uint64_t{1} << 40;

/// Open a decoder on the sample clip, or SKIP when the clip, the driver or
/// NVDEC is missing.
std::unique_ptr<GpuClipDecoder> openOrSkip(const GpuDecoderOptions& options = {}) {
    if (!osvtest::haveSample()) {
        SKIP("sample clip not available: " << osvtest::sampleOsv().string());
    }
    std::string reason;
    if (!GpuClipDecoder::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    auto opened = GpuClipDecoder::open(osvtest::sampleOsv(), nativeFormat(), options);
    if (!opened.ok() && opened.error().code == ErrorCode::Unsupported) {
        SKIP("NVDEC unavailable: " << opened.error().message);
    }
    INFO("open: " << (opened.ok() ? std::string("ok") : opened.error().toString()));
    REQUIRE(opened.ok());
    return std::move(opened).value();
}

/// FNV-1a over both lenses of a leased pair, downloaded from VRAM (luma and
/// interleaved chroma rows, visible bytes only).  Two leases of the same
/// picture hash alike; stride padding never leaks in.
std::uint64_t leaseHash(const GpuFrameLease& lease, void* context) {
    REQUIRE(lease.valid());
    REQUIRE(lease.pair().onDevice());
    std::uint64_t h = 1469598103934665603ull;
    REQUIRE(cuCtxPushCurrent(static_cast<CUcontext>(context)) == CUDA_SUCCESS);
    for (std::size_t l = 0; l < 2; ++l) {
        const DeviceFrameRef& d = lease.pair().device[l];
        const std::size_t lumaBytes = static_cast<std::size_t>(d.width) * 2;
        const std::size_t chromaBytes = static_cast<std::size_t>((d.width + 1) / 2) * 4;
        const std::uint32_t chromaH = (d.height + 1) / 2;
        std::vector<std::uint8_t> host(lumaBytes * d.height + chromaBytes * chromaH);
        CUDA_MEMCPY2D luma{};
        luma.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        luma.srcDevice = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(d.yDevice));
        luma.srcPitch = d.pitchBytes;
        luma.dstMemoryType = CU_MEMORYTYPE_HOST;
        luma.dstHost = host.data();
        luma.dstPitch = lumaBytes;
        luma.WidthInBytes = lumaBytes;
        luma.Height = d.height;
        CUDA_MEMCPY2D chroma = luma;
        chroma.srcDevice = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(d.uvDevice));
        chroma.dstHost = host.data() + lumaBytes * d.height;
        chroma.dstPitch = chromaBytes;
        chroma.WidthInBytes = chromaBytes;
        chroma.Height = chromaH;
        const CUresult a = cuMemcpy2D(&luma);
        const CUresult b = cuMemcpy2D(&chroma);
        if (a != CUDA_SUCCESS || b != CUDA_SUCCESS) {
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
            FAIL("cuMemcpy2D failed: " << static_cast<int>(a) << ", " << static_cast<int>(b));
        }
        for (const std::uint8_t byte : host) {
            h ^= byte;
            h *= 1099511628211ull;
        }
    }
    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);
    return h;
}

/// Acquire `index` on `decoder` and hash it (the lease is released after).
std::uint64_t frameHash(GpuClipDecoder& decoder, std::uint32_t index, LeaseSource* source = nullptr) {
    auto lease = decoder.acquire(index);
    INFO("acquire " << index << ": " << (lease.ok() ? std::string("ok") : lease.error().toString()));
    REQUIRE(lease.ok());
    if (source) {
        *source = lease.value().source();
    }
    return leaseHash(lease.value(), decoder.cuContext());
}

/// Wait up to `limit` for `done()`, polling every 10 ms.
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
//  trimForIdle
// =============================================================================
TEST_CASE("trimForIdle keeps the last requested frame, its neighbours and pinned ones, and the cache grows back",
          "[video][gpu][pool][sample]") {
    // No decode-ahead: which frames are cached must not depend on how far a
    // background thread got.
    GpuDecoderOptions options;
    options.decodeAhead = 0;
    std::unique_ptr<GpuClipDecoder> dec = openOrSkip(options);
    if (dec->stats().capacitySlots < 12) {
        SKIP("the frame cache holds only " << dec->stats().capacitySlots << " frames on this device right now");
    }
    // A lease held on an OLD frame: its slot must survive any trim.
    auto held = dec->acquire(20);
    REQUIRE(held.ok());
    for (std::uint32_t i = 21; i <= 27; ++i) {
        REQUIRE(dec->acquire(i).ok());
    }
    // Frame 0 (primed), every frame decoded on the way from the sync sample
    // to 20 (the cache keeps them all), and 21..27: as many as fit.
    const GpuDecoderStats before = dec->stats();
    REQUIRE(before.allocatedSlots >= 9);
    REQUIRE(before.cachedFrames == before.allocatedSlots);

    const std::size_t freed = dec->trimForIdle(3);
    const GpuDecoderStats after = dec->stats();
    INFO("allocated " << before.allocatedSlots << " -> " << after.allocatedSlots << ", freed " << freed);
    // Kept: 27 (requested last) and its nearest cached neighbours 26 and 25,
    // plus 20 because a lease pins it - four slots.
    REQUIRE(after.allocatedSlots == 4);
    REQUIRE(freed == static_cast<std::size_t>(before.allocatedSlots - after.allocatedSlots) * before.slotBytes);
    REQUIRE(dec->isCached(20));
    REQUIRE(dec->isCached(27));
    REQUIRE(dec->isCached(26));
    REQUIRE(dec->isCached(25));
    REQUIRE_FALSE(dec->isCached(24));
    REQUIRE_FALSE(dec->isCached(21));
    REQUIRE_FALSE(dec->isCached(0));
    held.value().release();

    // Growing back: new frames get fresh slots up to the original capacity,
    // and 28 continues from the decode position instead of restarting.
    const std::uint64_t restarts = dec->stats().restarts;
    for (std::uint32_t i = 28; i <= 33; ++i) {
        REQUIRE(dec->acquire(i).ok());
    }
    REQUIRE(dec->stats().restarts == restarts);
    REQUIRE(dec->stats().allocatedSlots == 10);
    REQUIRE(dec->stats().allocatedSlots <= dec->stats().capacitySlots);

    // Everything unpinned, nothing kept.
    REQUIRE(dec->trimForIdle(0) > 0);
    REQUIRE(dec->stats().allocatedSlots == 0);
    REQUIRE(dec->stats().cachedFrames == 0);
    REQUIRE(dec->acquire(40).ok());
    REQUIRE(dec->stats().allocatedSlots >= 1);
}

// =============================================================================
//  Park / take
// =============================================================================
TEST_CASE("GpuDecoderPool hands a parked decoder back warm with the same pixels", "[video][gpu][pool][sample]") {
    // No decode-ahead, so "the next frame continues from the decode position"
    // is a statement about this test, not about a background thread's luck.
    GpuDecoderOptions options;
    options.decodeAhead = 0;
    std::unique_ptr<GpuClipDecoder> dec = openOrSkip(options);
    std::unique_ptr<GpuClipDecoder> fresh = openOrSkip();
    GpuDecoderPool pool(testLimits());

    // The decoder shows frames 20..27, as a clip being looked at would.
    for (std::uint32_t i = 20; i < 27; ++i) {
        REQUIRE(dec->acquire(i).ok());
    }
    const std::uint64_t shown = frameHash(*dec, 27);
    REQUIRE(GpuDecoderPool::poolable(*dec));
    REQUIRE(dec->usesRetainedPrimaryContext());
    REQUIRE(dec->contextAlive());
    REQUIRE(dec->fileIdentity().size() > 0);
    REQUIRE(dec->trackIds() == std::array<std::uint32_t, 2>{1u, 2u});
    const GpuClipDecoder* identity = dec.get();
    const std::uint64_t restartsBefore = dec->stats().restarts;

    REQUIRE(pool.park(std::move(dec)));
    REQUIRE(dec == nullptr);
    REQUIRE(pool.idleCount() == 1);
    REQUIRE(pool.stats().parked == 1);
    REQUIRE(pool.stats().trimmedBytes > 0);  // 9 frames cached, 4 kept

    // Options must match exactly: the default decode-ahead is another decoder.
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), GpuDecoderOptions{}) == nullptr);
    std::unique_ptr<GpuClipDecoder> warm = pool.take(osvtest::sampleOsv(), nativeFormat(), options);
    REQUIRE(warm != nullptr);
    REQUIRE(warm.get() == identity);
    REQUIRE(pool.idleCount() == 0);
    REQUIRE(pool.stats().hits == 1);
    // Exclusive: nobody else gets it while it is out.
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), options) == nullptr);

    // The frame the user was looking at is still in VRAM, bit for bit.
    LeaseSource source = LeaseSource::Decoded;
    REQUIRE(frameHash(*warm, 27, &source) == shown);
    REQUIRE(source == LeaseSource::CacheHit);
    REQUIRE(warm->stats().cachedFrames == 4);

    // The next frame continues from the decode position (no GOP restart) and
    // is the picture a fresh decoder produces.
    REQUIRE(frameHash(*warm, 28, &source) == frameHash(*fresh, 28));
    REQUIRE(source == LeaseSource::Decoded);
    REQUIRE(warm->stats().restarts == restartsBefore);
    // Elsewhere in the clip too.
    REQUIRE(frameHash(*warm, 62) == frameHash(*fresh, 62));
    REQUIRE(frameHash(*warm, 3) == frameHash(*fresh, 3));
}

TEST_CASE("GpuDecoderPool matches file, tracks and options exactly", "[video][gpu][pool][sample]") {
    std::unique_ptr<GpuClipDecoder> dec = openOrSkip();
    GpuDecoderPool pool(testLimits());
    REQUIRE(pool.park(std::move(dec)));

    GpuDecoderOptions threads;
    threads.decoderThreads = 2;
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), threads) == nullptr);
    GpuDecoderOptions ahead;
    ahead.decodeAhead = 3;
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), ahead) == nullptr);
    meta::FormatInfo swapped = nativeFormat();
    swapped.videoTrackIds = {2u, 1u};
    REQUIRE(pool.take(osvtest::sampleOsv(), swapped, GpuDecoderOptions{}) == nullptr);
    GpuDecoderOptions caller;
    caller.cuContext = reinterpret_cast<void*>(std::uintptr_t{0x1000});
    REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), caller) == nullptr);
    REQUIRE(pool.take(osvtest::tempDir() / "not-there.OSV", nativeFormat(), GpuDecoderOptions{}) == nullptr);
    REQUIRE(pool.idleCount() == 1);
    REQUIRE(pool.stats().misses == 5);

    // The same file spelled differently is the same file.
    std::wstring spelled = osvtest::sampleOsv().wstring();
    for (wchar_t& c : spelled) {
        if (c == L'/') {
            c = L'\\';
        } else if (c >= L'a' && c <= L'z') {
            c = static_cast<wchar_t>(c - L'a' + L'A');
        }
    }
    REQUIRE(pool.take(std::filesystem::path(spelled), nativeFormat(), GpuDecoderOptions{}) != nullptr);
}

TEST_CASE("GpuDecoderPool never parks a decoder in a caller's context", "[video][gpu][pool][sample]") {
    if (!osvtest::haveSample()) {
        SKIP("sample clip not available");
    }
    std::string reason;
    if (!GpuClipDecoder::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    GpuDecoderPool pool(testLimits());
    REQUIRE_FALSE(pool.park(nullptr));

    // A context the caller owns (retained here, released after the decoder).
    CUdevice device = 0;
    REQUIRE(cuDeviceGet(&device, 0) == CUDA_SUCCESS);
    CUcontext context = nullptr;
    REQUIRE(cuDevicePrimaryCtxRetain(&context, device) == CUDA_SUCCESS);
    {
        GpuDecoderOptions options;
        options.cuContext = context;
        std::unique_ptr<GpuClipDecoder> dec = openOrSkip(options);
        REQUIRE_FALSE(dec->usesRetainedPrimaryContext());
        REQUIRE(dec->contextAlive());
        REQUIRE_FALSE(GpuDecoderPool::poolable(*dec));
        REQUIRE_FALSE(pool.park(std::move(dec)));
        REQUIRE(dec == nullptr);  // released, not handed back
    }
    REQUIRE(pool.idleCount() == 0);
    REQUIRE(pool.stats().rejected == 2);
    REQUIRE(pool.stats().parked == 0);
    cuDevicePrimaryCtxRelease(device);
}

// =============================================================================
//  VRAM pressure (injected floor)
// =============================================================================
TEST_CASE("a parked decoder is released when its device runs short of VRAM", "[video][gpu][pool][sample]") {
    std::unique_ptr<GpuClipDecoder> dec = openOrSkip();
    std::size_t freeBytes = 0;
    std::size_t totalBytes = 0;
    REQUIRE(dec->deviceMemory(freeBytes, totalBytes).ok());
    REQUIRE(totalBytes > 0);
    REQUIRE(freeBytes <= totalBytes);

    GpuDecoderPool::Limits pressure = testLimits();
    pressure.minFreeVramMiB = kImpossibleVramFloorMiB;

    SECTION("nothing is parked while the device is short") {
        GpuDecoderPool pool(pressure);
        REQUIRE_FALSE(pool.park(std::move(dec)));
        REQUIRE(pool.idleCount() == 0);
        REQUIRE(pool.stats().rejected == 1);
    }
    SECTION("trim() releases a parked decoder once the device is short") {
        GpuDecoderPool pool(testLimits());
        REQUIRE(pool.park(std::move(dec)));
        pool.setLimits(pressure);
        pool.trim();
        REQUIRE(pool.idleCount() == 0);
        REQUIRE(pool.stats().releasedForVram == 1);
    }
    SECTION("take() never hands out a decoder whose device is short") {
        GpuDecoderPool pool(testLimits());
        REQUIRE(pool.park(std::move(dec)));
        pool.setLimits(pressure);
        REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), GpuDecoderOptions{}) == nullptr);
        REQUIRE(pool.idleCount() == 0);
        REQUIRE(pool.stats().releasedForVram == 1);
    }
    SECTION("the reaper releases it on its own") {
        GpuDecoderPool pool(testLimits());
        REQUIRE(pool.park(std::move(dec)));
        REQUIRE(pool.reaperRunning());
        pool.setLimits(pressure);  // nobody calls the pool after this
        REQUIRE(waitFor([&] { return pool.idleCount() == 0; }, std::chrono::seconds(5)));
        REQUIRE(pool.stats().releasedForVram == 1);
        REQUIRE(waitFor([&] { return !pool.reaperRunning(); }, std::chrono::seconds(5)));
    }
}

// =============================================================================
//  Bounds, expiry, clear
// =============================================================================
TEST_CASE("GpuDecoderPool stays within its bound, expires and clears", "[video][gpu][pool][sample]") {
    GpuDecoderPool::Limits limits = testLimits();
    limits.maxIdleDecoders = 1;
    GpuDecoderPool pool(limits);

    SECTION("a second parked decoder pushes the first out") {
        std::unique_ptr<GpuClipDecoder> a = openOrSkip();
        std::unique_ptr<GpuClipDecoder> b = openOrSkip();
        const GpuClipDecoder* newest = b.get();
        REQUIRE(pool.park(std::move(a)));
        REQUIRE(pool.park(std::move(b)));
        REQUIRE(pool.idleCount() == 1);
        REQUIRE(pool.stats().evictedForRoom == 1);
        REQUIRE(pool.take(osvtest::sampleOsv(), nativeFormat(), GpuDecoderOptions{}).get() == newest);
    }
    SECTION("an idle decoder expires without anyone asking") {
        GpuDecoderPool::Limits quick = limits;
        quick.idleTtl = std::chrono::milliseconds(150);
        pool.setLimits(quick);
        REQUIRE(pool.park(openOrSkip()));
        REQUIRE(waitFor([&] { return pool.idleCount() == 0; }, std::chrono::seconds(5)));
        REQUIRE(pool.stats().expired == 1);
    }
    SECTION("clear() releases everything and the reaper is gone when it returns") {
        REQUIRE(pool.park(openOrSkip()));
        REQUIRE(pool.reaperRunning());
        pool.clear();
        REQUIRE(pool.idleCount() == 0);
        REQUIRE_FALSE(pool.reaperRunning());
        REQUIRE(pool.stats().cleared == 1);
        // Still usable.
        REQUIRE(pool.park(openOrSkip()));
        REQUIRE(pool.idleCount() == 1);
    }
}

TEST_CASE("a GPU pool destroyed with a decoder parked releases it", "[video][gpu][pool][sample]") {
    {
        GpuDecoderPool pool(testLimits());
        REQUIRE(pool.park(openOrSkip()));
    }
    // The next open on the same device must work normally.
    std::unique_ptr<GpuClipDecoder> dec = openOrSkip();
    REQUIRE(dec->acquire(5).ok());
}
#endif
