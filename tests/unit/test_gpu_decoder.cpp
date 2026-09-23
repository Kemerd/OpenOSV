// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for video::GpuClipDecoder (WP-A of the direct GPU pipeline):
// bit-exactness against the software decoder, decoding into a caller's CUDA
// context, the GOP-aware VRAM cache (hits, LRU, leases, budget), stream-
// ordered lease release, decode-ahead, destruction while the worker runs,
// concurrent acquires, and the zero-copy render path the frames are for.
//
// Everything that decodes needs the sample clip, a CUDA driver and NVDEC;
// each of those SKIPs cleanly when missing.  Timing assertions are generous
// on purpose (a loaded machine must not fail them); the real numbers come
// from osv_gpu_decode_bench.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/video/Decoder.h"
#include "osv/video/GpuClipDecoder.h"
#include "osv/video/HwAccel.h"
#include "osv/video/PlanarFrame.h"

#if defined(OSV_HAVE_CUDA)
#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/StreamScaling.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/render/CudaRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/video/DualStreamReader.h"
#endif

#if defined(OSV_VIDEO_HAVE_CUDA)
#include <cuda.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace osv;
using namespace osv::video;

namespace {

/// The native sample clip's layout (as in test_video.cpp): track 1 is lens 0
/// (slave), track 2 lens 1 (master), 3000 x 3000 10-bit HEVC.
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

}  // namespace

// =============================================================================
//  Argument validation (no GPU needed: checked before the driver is touched)
// =============================================================================
#if defined(OSV_VIDEO_HAVE_CUDA)
TEST_CASE("GpuClipDecoder::open rejects nonsense before touching the GPU", "[video][gpu]") {
    const meta::FormatInfo good = nativeFormat();
    const auto code = [](const Result<std::unique_ptr<GpuClipDecoder>>& r) { return r.ok() ? ErrorCode::Ok : r.error().code; };

    CHECK(code(GpuClipDecoder::open({}, good)) == ErrorCode::InvalidArgument);

    meta::FormatInfo lrf = good;
    lrf.sideBySideProxy = true;
    lrf.videoTrackIds = {1u, 1u};
    CHECK(code(GpuClipDecoder::open("clip.OSV", lrf)) == ErrorCode::InvalidArgument);

    meta::FormatInfo noTrack = good;
    noTrack.videoTrackIds = {0u, 2u};
    CHECK(code(GpuClipDecoder::open("clip.OSV", noTrack)) == ErrorCode::InvalidArgument);

    meta::FormatInfo sameTrack = good;
    sameTrack.videoTrackIds = {2u, 2u};
    CHECK(code(GpuClipDecoder::open("clip.OSV", sameTrack)) == ErrorCode::InvalidArgument);

    GpuDecoderOptions ahead;
    ahead.decodeAhead = 1000;
    CHECK(code(GpuClipDecoder::open("clip.OSV", good, ahead)) == ErrorCode::InvalidArgument);

    GpuDecoderOptions threads;
    threads.decoderThreads = 17;
    CHECK(code(GpuClipDecoder::open("clip.OSV", good, threads)) == ErrorCode::InvalidArgument);
    threads.decoderThreads = -1;
    CHECK(code(GpuClipDecoder::open("clip.OSV", good, threads)) == ErrorCode::InvalidArgument);

    GpuDecoderOptions device;
    device.cudaDevice = -3;
    CHECK(code(GpuClipDecoder::open("clip.OSV", good, device)) == ErrorCode::InvalidArgument);
}

TEST_CASE("An empty GpuFrameLease is inert", "[video][gpu]") {
    GpuFrameLease lease;
    CHECK_FALSE(lease.valid());
    CHECK(lease.frameIndex() == 0);
    CHECK_FALSE(lease.pair().onDevice());
    CHECK_FALSE(lease.pair().device[0].valid());
    CHECK(lease.releaseAfter(nullptr).code() == ErrorCode::InvalidArgument);
    lease.release();  // no-op
    GpuFrameLease moved(std::move(lease));
    CHECK_FALSE(moved.valid());
    CHECK(std::string(leaseSourceName(LeaseSource::CacheHit)) == "hit");
    CHECK(std::string(leaseSourceName(LeaseSource::WaitedForDecode)) == "waited");
    CHECK(std::string(leaseSourceName(LeaseSource::Decoded)) == "decoded");
}
#endif

#if defined(OSV_VIDEO_HAVE_CUDA)
namespace {

// =============================================================================
//  Helpers
// =============================================================================

/// FNV-1a over every visible sample on the 10-bit scale: luma rows, then the
/// Cb plane, then the Cr plane.  Planar yuv420p10 and P010 layouts of the
/// same picture hash identically; stride padding never leaks in.
std::uint64_t pictureHash(const PlanarFrame16& f) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint16_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (std::uint32_t y = 0; y < f.height; ++y) {
        const std::uint16_t* row = f.plane[0] + static_cast<std::size_t>(y) * f.strideElems[0];
        for (std::uint32_t x = 0; x < f.width; ++x) {
            mix(static_cast<std::uint16_t>(row[x] >> f.bitShift));
        }
    }
    const std::size_t step = f.chromaInterleaved ? 2 : 1;
    for (std::size_t c = 1; c <= 2; ++c) {
        for (std::uint32_t y = 0; y < f.chromaH; ++y) {
            const std::uint16_t* row = f.plane[c] + static_cast<std::size_t>(y) * f.strideElems[c];
            for (std::uint32_t x = 0; x < f.chromaW; ++x) {
                mix(static_cast<std::uint16_t>(row[x * step] >> f.bitShift));
            }
        }
    }
    return h;
}

/// Hashes and timestamps of every frame of both lenses, decoded in software
/// (the bit-exact reference).  Computed once per test binary: two software
/// 6K decodes of 65 frames are the expensive part of this file.
struct SoftwareReference {
    std::array<std::vector<std::uint64_t>, 2> hash;  ///< [lens][frame]
    std::array<std::vector<std::int64_t>, 2> ptsUs;  ///< [lens][frame]
    std::string error;                               ///< Non-empty when the pass failed.
};

const SoftwareReference& softwareReference() {
    static const SoftwareReference reference = [] {
        SoftwareReference r;
        std::array<std::string, 2> errors;
        // One thread per lens; the software decoder is frame-threaded itself.
        const auto pass = [&r, &errors](std::size_t lens) {
            DecoderOptions options;
            options.useContainerSamples = true;  // frame index == sample index, like the GPU decoder
            auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), nativeFormat().videoTrackIds[lens], options);
            if (!opened.ok()) {
                errors[lens] = opened.error().toString();
                return;
            }
            HevcStreamDecoder& dec = opened.value();
            for (std::uint32_t i = 0; i < dec.frameCount(); ++i) {
                auto frame = dec.next();
                if (!frame.ok() || frame.value().frameIndex != i || !frame.value().valid()) {
                    errors[lens] = "frame " + std::to_string(i) + ": " +
                                   (frame.ok() ? std::string("bad frame") : frame.error().toString());
                    return;
                }
                r.hash[lens].push_back(pictureHash(frame.value()));
                r.ptsUs[lens].push_back(frame.value().ptsUs);
            }
        };
        std::thread lens0(pass, 0);
        pass(1);
        lens0.join();
        r.error = errors[0].empty() ? errors[1] : errors[0];
        return r;
    }();
    return reference;
}

/// One lens of a cached pair, copied to host memory and described as a
/// PlanarFrame16 (P010: bitShift 6, interleaved chroma).
struct HostLens {
    std::vector<std::uint16_t> data;
    PlanarFrame16 view;
};

/// Synchronous device-to-host copy of one lens (thread-safe: pushes the
/// decoder's context itself, reports failures as text instead of asserting,
/// because Catch2 assertions must stay on the test thread).
bool downloadLens(const DeviceFrameRef& d, void* context, HostLens& out, std::string& error) {
    if (!d.valid() || d.pitchBytes % 2 != 0) {
        error = "invalid device frame";
        return false;
    }
    const std::size_t strideElems = d.pitchBytes / 2;
    const std::uint32_t chromaW = (d.width + 1) / 2;
    const std::uint32_t chromaH = (d.height + 1) / 2;
    out.data.assign(strideElems * (static_cast<std::size_t>(d.height) + chromaH), 0);
    if (cuCtxPushCurrent(static_cast<CUcontext>(context)) != CUDA_SUCCESS) {
        error = "cuCtxPushCurrent failed";
        return false;
    }
    CUDA_MEMCPY2D luma{};
    luma.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    luma.srcDevice = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(d.yDevice));
    luma.srcPitch = d.pitchBytes;
    luma.dstMemoryType = CU_MEMORYTYPE_HOST;
    luma.dstHost = out.data.data();
    luma.dstPitch = d.pitchBytes;
    luma.WidthInBytes = static_cast<std::size_t>(d.width) * 2;
    luma.Height = d.height;
    CUDA_MEMCPY2D chroma = luma;
    chroma.srcDevice = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(d.uvDevice));
    chroma.dstHost = out.data.data() + strideElems * d.height;
    chroma.WidthInBytes = static_cast<std::size_t>(chromaW) * 4;
    chroma.Height = chromaH;
    const CUresult a = cuMemcpy2D(&luma);
    const CUresult b = cuMemcpy2D(&chroma);
    CUcontext popped = nullptr;
    cuCtxPopCurrent(&popped);
    if (a != CUDA_SUCCESS || b != CUDA_SUCCESS) {
        error = "cuMemcpy2D failed (" + std::to_string(static_cast<int>(a)) + ", " + std::to_string(static_cast<int>(b)) + ")";
        return false;
    }
    PlanarFrame16& v = out.view;
    v.width = d.width;
    v.height = d.height;
    v.chromaW = chromaW;
    v.chromaH = chromaH;
    v.plane[0] = out.data.data();
    v.plane[1] = out.data.data() + strideElems * d.height;
    v.plane[2] = v.plane[1] + 1;
    v.strideElems = {strideElems, strideElems, strideElems};
    v.bitDepth = d.bitDepth;
    v.bitShift = d.bitShift;
    v.chromaInterleaved = true;
    return true;
}

/// Compare both lenses of a lease with the software reference.  Returns an
/// empty string when bit-exact, else what differs (thread-safe).
std::string verifyLease(const GpuFrameLease& lease, void* context) {
    const SoftwareReference& ref = softwareReference();
    if (!ref.error.empty()) {
        return "reference pass failed: " + ref.error;
    }
    const FramePair& pair = lease.pair();
    const std::uint32_t k = lease.frameIndex();
    if (!lease.valid() || pair.index != k || !pair.onDevice()) {
        return "lease for frame " + std::to_string(k) + " is not a valid device pair";
    }
    for (std::size_t l = 0; l < 2; ++l) {
        HostLens host;
        std::string error;
        if (!downloadLens(pair.device[l], context, host, error)) {
            return "frame " + std::to_string(k) + " lens " + std::to_string(l) + ": " + error;
        }
        if (k >= ref.hash[l].size()) {
            return "frame " + std::to_string(k) + " is not in the reference";
        }
        if (pictureHash(host.view) != ref.hash[l][k]) {
            return "frame " + std::to_string(k) + " lens " + std::to_string(l) + " differs from software decode";
        }
        if (pair.lens[l].ptsUs != ref.ptsUs[l][k]) {
            return "frame " + std::to_string(k) + " lens " + std::to_string(l) + " pts " +
                   std::to_string(pair.lens[l].ptsUs) + " vs software " + std::to_string(ref.ptsUs[l][k]);
        }
    }
    return {};
}

/// Open a decoder on the sample clip, or SKIP when the clip, the driver or
/// NVDEC is missing.
std::unique_ptr<GpuClipDecoder> openOrSkip(const GpuDecoderOptions& options = {}) {
    std::unique_ptr<GpuClipDecoder> decoder;
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
    decoder = std::move(opened).value();
    return decoder;
}

/// Bytes of one cached frame pair on this machine (for budgets expressed in
/// slots).  Learned once from a default decoder.
std::size_t slotBytes() {
    static const std::size_t bytes = [] {
        auto dec = openOrSkip();
        return dec->stats().slotBytes;
    }();
    return bytes;
}

/// Acquire or fail the test with the decoder's message.
GpuFrameLease acquireOk(GpuClipDecoder& dec, std::uint32_t k) {
    auto r = dec.acquire(k);
    INFO("acquire(" << k << "): " << (r.ok() ? std::string("ok") : r.error().toString()));
    REQUIRE(r.ok());
    return std::move(r).value();
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

/// Free VRAM (bytes) as seen from `context`.
std::size_t freeVram(CUcontext context) {
    std::size_t freeBytes = 0;
    std::size_t totalBytes = 0;
    if (cuCtxPushCurrent(context) == CUDA_SUCCESS) {
        cuMemGetInfo(&freeBytes, &totalBytes);
        CUcontext popped = nullptr;
        cuCtxPopCurrent(&popped);
    }
    return freeBytes;
}

}  // namespace

// =============================================================================
//  Bit-exactness
// =============================================================================
TEST_CASE("GpuClipDecoder frames are bit-exact with the software decoder", "[video][gpu][sample][cuda][hwaccel]") {
    auto dec = openOrSkip();
    const SoftwareReference& ref = softwareReference();
    INFO("reference: " << ref.error);
    REQUIRE(ref.error.empty());
    REQUIRE(dec->frameCount() == 65);
    REQUIRE(dec->lensWidth() == 3000);
    REQUIRE(dec->lensHeight() == 3000);

    // Scattered on purpose: backwards, across the second GOP (sync at 60),
    // keyframes and deep non-keyframes, the last frame.
    for (const std::uint32_t k : {45u, 7u, 62u, 0u, 31u, 59u, 13u, 64u, 1u, 60u}) {
        GpuFrameLease lease = acquireOk(*dec, k);
        const FramePair& pair = lease.pair();
        // Exactly the shape RenderParamsBuilder accepts as a zero-copy frame.
        REQUIRE(pair.index == k);
        REQUIRE(pair.onDevice());
        for (std::size_t l = 0; l < 2; ++l) {
            CHECK_FALSE(pair.lens[l].valid());  // no host planes by design
            CHECK(pair.lens[l].width == 3000);
            CHECK(pair.lens[l].height == 3000);
            CHECK(pair.lens[l].frameIndex == k);
            CHECK(pair.device[l].bitDepth == 10);
            CHECK(pair.device[l].bitShift == 6);
            CHECK(pair.device[l].pitchBytes >= 3000 * 2);
            CHECK(pair.device[l].pitchBytes % 2 == 0);
            CHECK(pair.device[l].deviceIndex == dec->deviceIndex());
            CHECK(pair.device[l].owner != nullptr);
        }
        const std::string mismatch = verifyLease(lease, dec->cuContext());
        INFO(mismatch);
        CHECK(mismatch.empty());
    }

    // One frame compared sample by sample as well, against a random-access
    // software decode, so a failure says where rather than only "differs".
    auto sw = HevcStreamDecoder::open(osvtest::sampleOsv(), nativeFormat().videoTrackIds[1], DecoderOptions{});
    REQUIRE(sw.ok());
    auto swFrame = sw.value().decodeFrame(37);
    REQUIRE(swFrame.ok());
    GpuFrameLease lease = acquireOk(*dec, 37);
    HostLens host;
    std::string error;
    REQUIRE(downloadLens(lease.pair().device[1], dec->cuContext(), host, error));
    const PlanarFrame16& s = swFrame.value();
    std::uint64_t lumaDiff = 0;
    std::uint64_t chromaDiff = 0;
    for (std::uint32_t y = 0; y < s.height; ++y) {
        for (std::uint32_t x = 0; x < s.width; ++x) {
            lumaDiff += host.view.luma(x, y) != s.luma(x, y) ? 1u : 0u;
        }
    }
    for (std::uint32_t y = 0; y < s.chromaH; ++y) {
        for (std::uint32_t x = 0; x < s.chromaW; ++x) {
            chromaDiff += host.view.chroma(1, x, y) != s.chroma(1, x, y) ? 1u : 0u;
            chromaDiff += host.view.chroma(2, x, y) != s.chroma(2, x, y) ? 1u : 0u;
        }
    }
    CHECK(lumaDiff == 0);
    CHECK(chromaDiff == 0);
    // The low 6 bits of a P010 sample carry nothing.
    CHECK((host.data[1234] & 0x3Fu) == 0);
}

// =============================================================================
//  Caller-supplied context
// =============================================================================
TEST_CASE("GpuClipDecoder decodes into a caller-created CUDA context", "[video][gpu][sample][cuda][hwaccel]") {
    if (!osvtest::haveSample()) {
        SKIP("sample clip not available");
    }
    std::string reason;
    if (!GpuClipDecoder::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    CUdevice device = 0;
    REQUIRE(cuDeviceGet(&device, 0) == CUDA_SUCCESS);
    unsigned primaryFlags = 0;
    int primaryActive = 0;
    REQUIRE(cuDevicePrimaryCtxGetState(device, &primaryFlags, &primaryActive) == CUDA_SUCCESS);
    CUcontext before = nullptr;
    REQUIRE(cuCtxGetCurrent(&before) == CUDA_SUCCESS);

    // A context of our own, as Premiere has one of its own.  cuCtxCreate
    // makes it current; pop it so the decoder has to push it by itself.
    CUcontext mine = nullptr;
    REQUIRE(cuCtxCreate(&mine, 0, device) == CUDA_SUCCESS);
    CUcontext popped = nullptr;
    REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
    REQUIRE(popped == mine);

    const auto contextOf = [mine](const void* ptr) {
        CUcontext owner = nullptr;
        cuCtxPushCurrent(mine);
        const CUresult r = cuPointerGetAttribute(&owner, CU_POINTER_ATTRIBUTE_CONTEXT,
                                                 static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(ptr)));
        CUcontext ignored = nullptr;
        cuCtxPopCurrent(&ignored);
        return r == CUDA_SUCCESS ? owner : nullptr;
    };

    SECTION("GpuClipDecoder with its own streams") {
        GpuDecoderOptions options;
        options.cuContext = mine;
        auto dec = openOrSkip(options);
        CHECK(dec->cuContext() == static_cast<void*>(mine));
        for (const std::uint32_t k : {5u, 3u, 61u}) {
            GpuFrameLease lease = acquireOk(*dec, k);
            for (std::size_t l = 0; l < 2; ++l) {
                CHECK(contextOf(lease.pair().device[l].yDevice) == mine);
                CHECK(contextOf(lease.pair().device[l].uvDevice) == mine);
            }
            const std::string mismatch = verifyLease(lease, mine);
            INFO(mismatch);
            CHECK(mismatch.empty());
            // Every push was popped: this thread's context is untouched.
            CUcontext now = nullptr;
            REQUIRE(cuCtxGetCurrent(&now) == CUDA_SUCCESS);
            CHECK(now == before);
        }
    }

    SECTION("GpuClipDecoder on a caller stream, released on that stream") {
        CUstream stream = nullptr;
        REQUIRE(cuCtxPushCurrent(mine) == CUDA_SUCCESS);
        REQUIRE(cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);
        REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
        {
            GpuDecoderOptions options;
            options.cuContext = mine;
            options.cuStream = stream;
            auto dec = openOrSkip(options);
            GpuFrameLease lease = acquireOk(*dec, 9);
            CHECK(contextOf(lease.pair().device[0].yDevice) == mine);
            const std::string mismatch = verifyLease(lease, mine);
            INFO(mismatch);
            CHECK(mismatch.empty());
            CHECK(lease.releaseAfter(stream).ok());
            CHECK_FALSE(lease.valid());
        }
        REQUIRE(cuCtxPushCurrent(mine) == CUDA_SUCCESS);
        CHECK(cuStreamSynchronize(stream) == CUDA_SUCCESS);
        CHECK(cuStreamDestroy(stream) == CUDA_SUCCESS);
        REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
    }

    SECTION("HevcStreamDecoder honours DecoderOptions::cudaContext") {
        DecoderOptions options;
        options.hw = HwAccel::Cuda;
        options.keepOnDevice = true;
        options.cudaContext = mine;
        options.useContainerSamples = true;
        auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, options);
        if (!opened.ok()) {
            SKIP("NVDEC unavailable: " << opened.error().message);
        }
        REQUIRE(opened.value().activeHw() == HwAccel::Cuda);
        auto frame = opened.value().decodeFrame(4);
        REQUIRE(frame.ok());
        const auto ref = opened.value().lastDeviceFrame();
        REQUIRE(ref.has_value());
        CHECK(contextOf(ref->yDevice) == mine);
        CHECK(opened.value().previousSyncIndex(4) == std::optional<std::uint32_t>(0u));
        CHECK(opened.value().previousSyncIndex(62) == std::optional<std::uint32_t>(60u));
    }

    // Never created, retained or activated the primary context on the way.
    if (!primaryActive) {
        unsigned flagsAfter = 0;
        int activeAfter = 0;
        REQUIRE(cuDevicePrimaryCtxGetState(device, &flagsAfter, &activeAfter) == CUDA_SUCCESS);
        CHECK(activeAfter == 0);
    } else {
        WARN("primary context was already active in this process; its untouched state cannot be checked");
    }
    CHECK(cuCtxDestroy(mine) == CUDA_SUCCESS);
}

// =============================================================================
//  The cache
// =============================================================================
TEST_CASE("GpuClipDecoder keeps every frame of the GOP it decoded", "[video][gpu][sample][cuda][hwaccel]") {
    GpuDecoderOptions options;
    options.decodeAhead = 0;  // only the foreground decodes: counters stay exact
    options.vramBudgetBytes = std::size_t{1536} << 20;
    auto dec = openOrSkip(options);
    const GpuDecoderStats opened = dec->stats();
    REQUIRE(opened.capacitySlots >= 16);
    CHECK(opened.decodeAheadWindow == 0);
    CHECK(dec->isCached(0));  // open() leaves frame 0 behind...
    CHECK(opened.allocatedSlots == 1);  // ...in the one slot it allocated; the rest grows on demand

    auto t = std::chrono::steady_clock::now();
    GpuFrameLease landing = acquireOk(*dec, 45);
    const double landingMs = msSince(t);
    CHECK(landing.source() == LeaseSource::Decoded);
    const GpuDecoderStats afterLanding = dec->stats();
    // open() left the decoder on frame 1: frames 1..45 were decoded.
    CHECK(afterLanding.framesDecoded - opened.framesDecoded == 45);
    landing.release();

    // Stepping backwards through the GOP: all hits, no decoding.
    std::vector<double> hitMs;
    for (std::uint32_t k = 44; k >= 30; --k) {
        t = std::chrono::steady_clock::now();
        GpuFrameLease lease = acquireOk(*dec, k);
        hitMs.push_back(msSince(t));
        CHECK(lease.source() == LeaseSource::CacheHit);
    }
    const GpuDecoderStats afterHits = dec->stats();
    CHECK(afterHits.framesDecoded == afterLanding.framesDecoded);
    CHECK(afterHits.cacheHits - afterLanding.cacheHits == 15);
    std::sort(hitMs.begin(), hitMs.end());
    WARN("landing on 45: " << landingMs << " ms; hit median " << hitMs[hitMs.size() / 2] << " ms, max "
                           << hitMs.back() << " ms");
    CHECK(hitMs[hitMs.size() / 2] < 5.0);  // target < 1 ms; generous for loaded machines

    // LRU: the oldest frames of the walk made room; the newest are all there.
    const std::uint32_t capacity = afterHits.capacitySlots;
    CHECK(afterHits.cachedFrames == capacity);
    for (std::uint32_t k = 46 - capacity; k <= 45; ++k) {
        CHECK(dec->isCached(k));
    }
    CHECK_FALSE(dec->isCached(1));
    CHECK(afterHits.evictions > 0);
    // Budget.
    CHECK(afterHits.allocatedSlots <= afterHits.capacitySlots);
    CHECK(afterHits.vramBytes <= afterHits.budgetBytes);
    CHECK(afterHits.vramBytes == static_cast<std::size_t>(afterHits.allocatedSlots) * afterHits.slotBytes);

    // Dropping the cache keeps the VRAM but forgets the frames.
    CHECK(dec->dropCachedFrames() == capacity);
    CHECK_FALSE(dec->isCached(45));
    CHECK(dec->stats().vramBytes == afterHits.vramBytes);

    // Out of range is an argument error, not a crash.
    CHECK(dec->acquire(65).code() == ErrorCode::InvalidArgument);
}

TEST_CASE("GpuClipDecoder LRU eviction never touches a leased slot", "[video][gpu][sample][cuda][hwaccel]") {
    GpuDecoderOptions options;
    options.decodeAhead = 0;
    options.vramBudgetBytes = 4 * slotBytes() + slotBytes() / 2;  // room for exactly four pairs
    auto dec = openOrSkip(options);
    REQUIRE(dec->stats().capacitySlots == 4);

    GpuFrameLease held = acquireOk(*dec, 10);
    const std::string before = verifyLease(held, dec->cuContext());
    INFO(before);
    REQUIRE(before.empty());

    // Thirty more frames through a four-slot cache: plenty of evictions,
    // none of them the leased frame.
    acquireOk(*dec, 40).release();
    const GpuDecoderStats churned = dec->stats();
    CHECK(churned.evictions >= 25);
    CHECK(dec->isCached(10));
    const std::string after = verifyLease(held, dec->cuContext());
    INFO(after);
    CHECK(after.empty());  // the pixels were not overwritten either

    // Pin every slot: a new frame has nowhere to go and says so.
    GpuFrameLease a = acquireOk(*dec, 40);
    GpuFrameLease b = acquireOk(*dec, 39);
    GpuFrameLease c = acquireOk(*dec, 38);
    CHECK(a.source() == LeaseSource::CacheHit);
    CHECK(dec->stats().leasedSlots == 4);
    auto full = dec->acquire(50);
    REQUIRE_FALSE(full.ok());
    CHECK(full.error().code == ErrorCode::Unsupported);
    CHECK_FALSE(dec->isCached(50));

    // Free one: the same request now succeeds and is correct.
    c.release();
    GpuFrameLease fifty = acquireOk(*dec, 50);
    const std::string mismatch = verifyLease(fifty, dec->cuContext());
    INFO(mismatch);
    CHECK(mismatch.empty());
    CHECK(dec->isCached(10));

    const GpuDecoderStats end = dec->stats();
    CHECK(end.allocatedSlots == 4);
    CHECK(end.vramBytes <= end.budgetBytes);
    // A budget below one frame pair is refused outright.
    GpuDecoderOptions tiny;
    tiny.vramBudgetBytes = slotBytes() / 2;
    auto refused = GpuClipDecoder::open(osvtest::sampleOsv(), nativeFormat(), tiny);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
}

namespace {

/// State shared with the host function below.
struct SlowConsumer {
    std::atomic<bool> finished{false};
};

/// Runs on the consumer stream: stands in for a long kernel that reads the
/// leased frame.  Must not call CUDA.
void CUDA_CB slowConsumerFunc(void* user) {
    auto* consumer = static_cast<SlowConsumer*>(user);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    consumer->finished = true;
}

}  // namespace

TEST_CASE("A lease released on a stream holds its slot until the stream gets there",
          "[video][gpu][sample][cuda][hwaccel]") {
    // One slot, so the next frame MUST overwrite the released one.
    GpuDecoderOptions options;
    options.vramBudgetBytes = slotBytes() + slotBytes() / 2;
    auto dec = openOrSkip(options);
    REQUIRE(dec->stats().capacitySlots == 1);
    CHECK(dec->stats().decodeAheadWindow == 0);  // no room to decode ahead

    CUcontext context = static_cast<CUcontext>(dec->cuContext());
    CUstream consumerStream = nullptr;
    CUcontext popped = nullptr;
    REQUIRE(cuCtxPushCurrent(context) == CUDA_SUCCESS);
    REQUIRE(cuStreamCreate(&consumerStream, CU_STREAM_NON_BLOCKING) == CUDA_SUCCESS);
    REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);

    GpuFrameLease five = acquireOk(*dec, 5);
    SlowConsumer consumer;
    REQUIRE(cuCtxPushCurrent(context) == CUDA_SUCCESS);
    REQUIRE(cuLaunchHostFunc(consumerStream, &slowConsumerFunc, &consumer) == CUDA_SUCCESS);
    REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
    // The "kernel" is enqueued, so the lease may go at once.
    REQUIRE(five.releaseAfter(consumerStream).ok());

    // Frame 6 needs the only slot.  The overwrite waits on the GPU for the
    // consumer, and acquire() returns complete data, so by the time it
    // returns the consumer must have finished.
    const auto t = std::chrono::steady_clock::now();
    GpuFrameLease six = acquireOk(*dec, 6);
    const double waitedMs = msSince(t);
    CHECK(consumer.finished.load());
    CHECK(waitedMs > 500.0);
    const std::string mismatch = verifyLease(six, context);
    INFO(mismatch);
    CHECK(mismatch.empty());

    REQUIRE(cuCtxPushCurrent(context) == CUDA_SUCCESS);
    CHECK(cuStreamSynchronize(consumerStream) == CUDA_SUCCESS);
    CHECK(cuStreamDestroy(consumerStream) == CUDA_SUCCESS);
    REQUIRE(cuCtxPopCurrent(&popped) == CUDA_SUCCESS);
}

// =============================================================================
//  Decode-ahead
// =============================================================================
TEST_CASE("GpuClipDecoder decode-ahead turns forward playback into cache hits",
          "[video][gpu][sample][cuda][hwaccel]") {
    auto dec = openOrSkip();  // decodeAhead = 8
    REQUIRE(dec->stats().decodeAheadWindow == 8);
    acquireOk(*dec, 0).release();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    acquireOk(*dec, 1).release();  // k then k + 1: playing forward

    // 60 fps playback: the worker stays ahead of the host.
    std::uint32_t hits = 0;
    std::uint32_t requests = 0;
    for (std::uint32_t k = 2; k <= 40; ++k) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        GpuFrameLease lease = acquireOk(*dec, k);
        ++requests;
        hits += lease.source() == LeaseSource::CacheHit ? 1u : 0u;
        if (k == 17 || k == 40) {
            const std::string mismatch = verifyLease(lease, dec->cuContext());
            INFO(mismatch);
            CHECK(mismatch.empty());
        }
    }
    // Let the worker finish the window it was given (41..48) before counting.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const GpuDecoderStats s = dec->stats();
    WARN("decode-ahead: " << hits << "/" << requests << " hits, " << s.framesDecodedAhead << " frames decoded ahead");
    CHECK(dec->isCached(48));
    CHECK(hits >= requests - 3);  // allow a scheduling hiccup or two
    CHECK(s.framesDecodedAhead >= 30);

    // A jump cancels the run: nothing is decoded ahead of a lone landing.
    acquireOk(*dec, 62).release();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const GpuDecoderStats afterJump = dec->stats();
    CHECK_FALSE(dec->isCached(64));
    CHECK(afterJump.framesDecodedAhead - s.framesDecodedAhead <= 1);  // at most the frame in flight
}

TEST_CASE("Destroying GpuClipDecoder mid decode-ahead is prompt and leaks nothing",
          "[video][gpu][sample][cuda][hwaccel]") {
    if (!osvtest::haveSample()) {
        SKIP("sample clip not available");
    }
    std::string reason;
    if (!GpuClipDecoder::available(&reason)) {
        SKIP("CUDA unavailable: " << reason);
    }
    // Hold the primary context ourselves so that it (and the free-VRAM
    // reading) outlives every decoder.
    CUdevice device = 0;
    REQUIRE(cuDeviceGet(&device, 0) == CUDA_SUCCESS);
    CUcontext primary = nullptr;
    REQUIRE(cuDevicePrimaryCtxRetain(&primary, device) == CUDA_SUCCESS);
    const std::size_t freeBefore = freeVram(primary);

    GpuFrameLease survivor;
    double destroyMs = 0.0;
    {
        GpuDecoderOptions options;
        options.decodeAhead = 64;  // clamped to the capacity
        auto dec = openOrSkip(options);
        acquireOk(*dec, 0).release();
        survivor = acquireOk(*dec, 1);  // sequential: the worker starts on 2..
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto t = std::chrono::steady_clock::now();
        dec.reset();  // worker is mid-run
        destroyMs = msSince(t);
    }
    WARN("destroyed mid decode-ahead in " << destroyMs << " ms");
    CHECK(destroyMs < 500.0);

    // A lease outlives its decoder: the VRAM store stays until it goes.
    REQUIRE(survivor.valid());
    const std::string mismatch = verifyLease(survivor, primary);
    INFO(mismatch);
    CHECK(mismatch.empty());
    survivor.release();

    // Everything went back: slots, NVDEC surfaces, streams, events.
    const std::size_t freeAfter = freeVram(primary);
    const double leakedMiB = (static_cast<double>(freeBefore) - static_cast<double>(freeAfter)) / 1048576.0;
    WARN("free VRAM before " << freeBefore / 1048576 << " MiB, after " << freeAfter / 1048576 << " MiB");
    CHECK(leakedMiB < 64.0);
    cuDevicePrimaryCtxRelease(device);
}

// =============================================================================
//  Concurrency
// =============================================================================
TEST_CASE("Four threads acquiring overlapping frames get bit-exact pairs", "[video][gpu][sample][cuda][hwaccel]") {
    GpuDecoderOptions options;
    options.vramBudgetBytes = 10 * slotBytes() + slotBytes() / 2;  // small: forces evictions under load
    auto dec = openOrSkip(options);
    REQUIRE(softwareReference().error.empty());
    void* context = dec->cuContext();

    // Overlapping, backwards, across the GOP boundary, and one thread that
    // plays forward so the worker runs at the same time.
    std::array<std::vector<std::uint32_t>, 4> plans{};
    plans[0] = {45, 44, 46, 10, 11, 12, 13, 14, 63, 62, 30, 31};
    plans[1] = {45, 20, 21, 22, 23, 59, 60, 61, 5, 6, 7, 8};
    plans[3] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::uint32_t seed = 12345u;
    for (int i = 0; i < 14; ++i) {
        seed = seed * 1664525u + 1013904223u;
        plans[2].push_back((seed >> 8) % 65u);
    }

    std::mutex failuresMutex;
    std::vector<std::string> failures;
    std::atomic<int> served{0};
    const auto run = [&](std::size_t t) {
        // Each thread releases on a stream of its own every other frame.
        CUstream stream = nullptr;
        CUcontext popped = nullptr;
        if (cuCtxPushCurrent(static_cast<CUcontext>(context)) == CUDA_SUCCESS) {
            cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);
            cuCtxPopCurrent(&popped);
        }
        bool onStream = false;
        for (const std::uint32_t k : plans[t]) {
            auto r = dec->acquire(k);
            std::string problem;
            if (!r.ok()) {
                problem = "thread " + std::to_string(t) + " acquire(" + std::to_string(k) + "): " + r.error().toString();
            } else {
                GpuFrameLease lease = std::move(r).value();
                problem = verifyLease(lease, context);
                if (lease.frameIndex() != k) {
                    problem = "thread " + std::to_string(t) + " asked for " + std::to_string(k) + ", got " +
                              std::to_string(lease.frameIndex());
                }
                onStream = !onStream;
                if (onStream && stream) {
                    const Status released = lease.releaseAfter(stream);
                    if (!released.ok() && problem.empty()) {
                        problem = "releaseAfter: " + released.error().toString();
                    }
                }
                ++served;
            }
            if (!problem.empty()) {
                std::lock_guard<std::mutex> lock(failuresMutex);
                failures.push_back(problem);
            }
        }
        if (stream && cuCtxPushCurrent(static_cast<CUcontext>(context)) == CUDA_SUCCESS) {
            cuStreamSynchronize(stream);
            cuStreamDestroy(stream);
            cuCtxPopCurrent(&popped);
        }
    };
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < plans.size(); ++t) {
        threads.emplace_back(run, t);
    }
    for (std::thread& th : threads) {
        th.join();
    }
    for (const std::string& f : failures) {
        UNSCOPED_INFO(f);
    }
    CHECK(failures.empty());
    std::size_t planned = 0;
    for (const auto& p : plans) {
        planned += p.size();
    }
    CHECK(served.load() == static_cast<int>(planned));
    const GpuDecoderStats s = dec->stats();
    CHECK(s.leasedSlots == 0);
    CHECK(s.vramBytes <= s.budgetBytes);
    CHECK(s.acquires >= planned);
}

// =============================================================================
//  The frames render zero-copy (the reason the decoder exists)
// =============================================================================
#if defined(OSV_HAVE_CUDA)
TEST_CASE("GpuClipDecoder frames render zero-copy like the software-decoded pair",
          "[video][gpu][render][sample][cuda][hwaccel]") {
    if (!osvtest::haveSample()) {
        SKIP("sample clip not available");
    }
    std::string reason;
    if (!render::CudaRenderer::available(&reason)) {
        SKIP("CUDA renderer unavailable: " << reason);
    }
    // The rig exactly as test_render.cpp / the importer build it.
    auto file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    auto track = meta::MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    auto format = meta::FormatDetector::detect(file.value(), &track.value());
    REQUIRE(format.ok());
    auto cal = meta::CalibrationSelector::select(track.value().stream());
    REQUIRE(cal.ok());
    const meta::FormatInfo& f = format.value();
    auto scaling = geom::StreamScaling::derive(static_cast<int>(f.lensW()), static_cast<int>(f.lensH()),
                                               static_cast<int>(f.sensorW), static_cast<int>(f.sensorH),
                                               f.digitalFocalLength, 0.5 * (cal.value().slave.fx + cal.value().master.fx));
    REQUIRE(scaling.ok());
    auto rig = geom::LensRig::build(cal.value(), scaling.value(), geom::FocalSource::DigitalFocalLength,
                                    f.digitalFocalLength, geom::ExtrinsicConvention{});
    REQUIRE(rig.ok());

    // Software pair (host planes) and the GPU decoder's pair (device only),
    // both through the detected format so the lens order matches.
    auto reader = DualStreamReader::open(osvtest::sampleOsv(), f);
    REQUIRE(reader.ok());
    auto hostPair = reader.value().read(23);
    REQUIRE(hostPair.ok());
    auto opened = GpuClipDecoder::open(osvtest::sampleOsv(), f);
    if (!opened.ok() && opened.error().code == ErrorCode::Unsupported) {
        SKIP("NVDEC unavailable: " << opened.error().message);
    }
    REQUIRE(opened.ok());
    std::unique_ptr<GpuClipDecoder> dec = std::move(opened).value();
    GpuFrameLease lease = acquireOk(*dec, 23);

    const OsvColorParams cp = color::makeColorParams(color::DlogMFit::DjiRefit, color::OutputTransfer::PQ, 0.0f);
    geom::VirtualCamera cam;
    cam.w = 1920;
    cam.h = 1080;
    cam.hfovDeg = 100;
    cam.yawDeg = 80;  // across the seam: both lenses contribute
    auto hostJob = render::RenderParamsBuilder().rig(rig.value()).camera(cam).color(cp).build(hostPair.value());
    auto devJob = render::RenderParamsBuilder().rig(rig.value()).camera(cam).color(cp).build(lease.pair());
    REQUIRE(hostJob.ok());
    INFO("device job: " << (devJob.ok() ? std::string("ok") : devJob.error().toString()));
    REQUIRE(devJob.ok());
    REQUIRE(devJob.value().planesOnDevice[0]);
    REQUIRE(devJob.value().planesOnDevice[1]);

    auto gpu = render::CudaRenderer::create(dec->deviceIndex());
    REQUIRE(gpu.ok());
    auto a = gpu.value()->render(hostJob.value());
    auto b = gpu.value()->render(devJob.value());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    const render::ImageDiffStats stats = render::compareImages16(a.value(), b.value());
    INFO("software pair vs GpuClipDecoder pair: PSNR " << stats.psnrDb << " dB, max " << stats.maxAbsCode << " codes");
    CHECK(stats.maxAbsCode <= 1);
}
#endif  // OSV_HAVE_CUDA

#endif  // OSV_VIDEO_HAVE_CUDA
