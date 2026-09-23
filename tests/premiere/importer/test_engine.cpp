// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_engine.cpp - the importer's direct-GPU engine, through its C ABI.
//
// The reframe effect never links the importer: it finds the loaded module
// and calls three exported functions (plugins/common/OsvEngineAbi.h).  These
// tests do exactly that against the BUILT module, with a CUDA context the
// test creates itself - a private one, as Premiere's is (the first real
// session's log: "host CUDA context ... is NOT the primary context") - so
// what is proven is the thing Premiere will call:
//
//   * the exports exist and speak the ABI version the header says;
//   * a frame comes back as device planes IN THE CALLER'S CONTEXT, holding
//     exactly the pixels a software decode of that frame holds;
//   * media time maps to the frame the importer's own imGetSourceVideo
//     would pick, and the stitch block matches the clip's settings;
//   * every argument error is refused with the documented code, and leases
//     are released cleanly.

#include "ImporterHarness.h"

#include "OsvEngineAbi.h"

#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/container/OsvFile.h"
#include "osv/video/DualStreamReader.h"

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using osv::premiere::test::ImporterHarness;
using osv::premiere::test::sampleClipAvailable;
using osv::premiere::test::sampleClipPath;

namespace {

/// Premiere ticks per frame at 59.94 fps (254016000000 * 1001 / 60000).
constexpr std::int64_t kTicksPerFrame5994 = 4237833600LL;

/// A private CUDA context for the test, destroyed LAST: it is declared before
/// the harness so it outlives imShutdown, which is when the engine releases
/// the decoders it created in it.
struct TestContext {
    CUcontext context = nullptr;
    std::string reason;

    TestContext() {
        if (cuInit(0) != CUDA_SUCCESS) {
            reason = "cuInit failed";
            return;
        }
        CUdevice device = 0;
        if (cuDeviceGet(&device, 0) != CUDA_SUCCESS) {
            reason = "no CUDA device";
            return;
        }
        if (cuCtxCreate(&context, 0, device) != CUDA_SUCCESS) {
            context = nullptr;
            reason = "cuCtxCreate failed";
            return;
        }
        // Leave the thread as we found it: the engine must push the context
        // itself, which is part of what these tests prove.
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }
    ~TestContext() {
        if (context) {
            (void)cuCtxDestroy(context);
        }
    }
    TestContext(const TestContext&) = delete;
    TestContext& operator=(const TestContext&) = delete;
};

/// The three exports, resolved from the loaded module.
struct EngineApi {
    OsvEngineAbiVersionFn version = nullptr;
    OsvEngineAcquireFrameFn acquire = nullptr;
    OsvEngineReleaseFrameFn release = nullptr;

    [[nodiscard]] bool ok() const noexcept { return version && acquire && release; }
};

[[nodiscard]] EngineApi resolveEngine() {
    EngineApi api;
    const HMODULE module = GetModuleHandleW(OSV_ENGINE_MODULE_NAME);
    if (!module) {
        return api;
    }
    api.version = reinterpret_cast<OsvEngineAbiVersionFn>(GetProcAddress(module, OSV_ENGINE_SYM_ABI_VERSION));
    api.acquire = reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
    api.release = reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
    return api;
}

/// A request for `frame` of the sample clip in `context`.
[[nodiscard]] OsvEngineFrameRequest requestFor(const std::wstring& path, std::uint32_t frame, CUcontext context,
                                               int transfer = OSV_ENGINE_TRANSFER_FROM_CLIP) {
    OsvEngineFrameRequest r{};
    r.structSize = sizeof(OsvEngineFrameRequest);
    r.path = path.c_str();
    r.mediaTicks = kTicksPerFrame5994 * static_cast<std::int64_t>(frame);
    r.purpose = OSV_ENGINE_PURPOSE_EXACT;
    r.outputTransfer = transfer;
    r.cuContext = context;
    r.cuStream = nullptr;
    return r;
}

[[nodiscard]] OsvEngineFrame emptyFrame() {
    OsvEngineFrame f{};
    f.structSize = sizeof(OsvEngineFrame);
    return f;
}

/// The context a device pointer was allocated in.
[[nodiscard]] CUcontext contextOf(const void* devicePtr) {
    CUcontext owner = nullptr;
    if (cuPointerGetAttribute(&owner, CU_POINTER_ATTRIBUTE_CONTEXT,
                              static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(devicePtr))) != CUDA_SUCCESS) {
        return nullptr;
    }
    return owner;
}

}  // namespace

TEST_CASE("the importer exports the direct-GPU engine at the documented ABI version", "[importer][engine]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const EngineApi api = resolveEngine();
    REQUIRE(api.ok());
    CHECK(api.version() == OSV_ENGINE_ABI_VERSION);
}

TEST_CASE("the engine refuses bad requests with the documented codes", "[importer][engine]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const EngineApi api = resolveEngine();
    REQUIRE(api.ok());
    char error[256] = {};

    OsvEngineFrame frame = emptyFrame();
    CHECK(api.acquire(nullptr, &frame, error, sizeof(error)) == OSV_ENGINE_ERR_ARGUMENT);

    const std::wstring path = L"C:\\definitely\\not\\a\\clip.OSV";
    OsvEngineFrameRequest request = requestFor(path, 0, reinterpret_cast<CUcontext>(0x1));
    request.structSize = 8;  // an older caller's smaller struct
    CHECK(api.acquire(&request, &frame, error, sizeof(error)) == OSV_ENGINE_ERR_VERSION);

    request = requestFor(path, 0, nullptr);
    CHECK(api.acquire(&request, &frame, error, sizeof(error)) == OSV_ENGINE_ERR_ARGUMENT);
    CHECK(frame.lease == nullptr);

    request = requestFor(path, 0, reinterpret_cast<CUcontext>(0x1));
    request.purpose = 7;
    CHECK(api.acquire(&request, &frame, error, sizeof(error)) == OSV_ENGINE_ERR_ARGUMENT);

    request = requestFor(path, 0, reinterpret_cast<CUcontext>(0x1));
    request.outputTransfer = 99;
    CHECK(api.acquire(&request, &frame, error, sizeof(error)) == OSV_ENGINE_ERR_ARGUMENT);

    // A file that does not exist is a source error, reported before any CUDA
    // call touches the (fake) context.
    request = requestFor(path, 0, reinterpret_cast<CUcontext>(0x1));
    CHECK(api.acquire(&request, &frame, error, sizeof(error)) == OSV_ENGINE_ERR_SOURCE);
    CHECK(std::string(error).find("cannot open") != std::string::npos);

    // Releasing nothing, or something that is not a lease, is harmless.
    api.release(nullptr, nullptr);
    std::uint32_t notALease[16] = {};
    api.release(notALease, nullptr);
}

TEST_CASE("the engine serves a frame as device planes in the caller's context, pixel-exact",
          "[importer][engine][cuda][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    TestContext cuda;  // before the harness: outlives imShutdown
    if (!cuda.context) {
        SKIP("CUDA unavailable: " << cuda.reason);
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const EngineApi api = resolveEngine();
    REQUIRE(api.ok());

    const std::wstring path = sampleClipPath().wstring();
    constexpr std::uint32_t kFrame = 7;  // not a keyframe: the decoder must run forward
    OsvEngineFrameRequest request = requestFor(path, kFrame, cuda.context);
    OsvEngineFrame frame = emptyFrame();
    char error[512] = {};
    const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
    INFO("engine error: " << error);
    REQUIRE(rc == OSV_ENGINE_OK);
    REQUIRE(frame.lease != nullptr);

    // ---- the block --------------------------------------------------------
    CHECK(frame.paramsSize == sizeof(OsvRenderParams));
    CHECK(frame.frameIndex == kFrame);
    CHECK(frame.exact == 1);
    CHECK(frame.stitch.mode == OSV_MODE_EQUIRECT);
    CHECK(frame.stitch.lens[0].width == 3000);
    CHECK(frame.stitch.lens[1].height == 3000);
    // Default Source Settings: PQ out, and the defaults correct the seam
    // (the parallax grid, or the seam table when the grid is refused).
    CHECK(frame.stitch.color.transfer == OSV_TRANSFER_PQ);
    CHECK((frame.warpDevice != nullptr || frame.seamDevice != nullptr));
    CHECK((frame.warpDevice != nullptr) == (frame.stitch.warpEnabled != 0));
    CHECK((frame.seamDevice != nullptr) == (frame.stitch.seamShiftEnabled != 0));
    for (int i = 0; i < 9; ++i) {
        CHECK(frame.bodyFromWorld[i] == frame.stitch.Rout[i]);
    }

    // ---- the planes live in OUR context -------------------------------------
    for (const OsvPlane& plane : frame.planes) {
        REQUIRE(plane.y != nullptr);
        REQUIRE(plane.u != nullptr);
        CHECK(plane.chromaInterleaved == 1);
        CHECK(plane.bitShift == 6);
        REQUIRE(cuCtxPushCurrent(cuda.context) == CUDA_SUCCESS);
        CHECK(contextOf(plane.y) == cuda.context);
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }
    if (frame.warpDevice) {
        REQUIRE(cuCtxPushCurrent(cuda.context) == CUDA_SUCCESS);
        CHECK(contextOf(frame.warpDevice) == cuda.context);
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }

    // ---- ...and hold exactly the software decode's pixels -------------------
    auto file = osv::OsvFile::open(sampleClipPath());
    REQUIRE(file.ok());
    auto track = osv::meta::MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    auto format = osv::meta::FormatDetector::detect(file.value(), &track.value());
    REQUIRE(format.ok());
    auto reader = osv::video::DualStreamReader::open(sampleClipPath(), format.value());
    REQUIRE(reader.ok());
    auto host = reader.value().read(kFrame);
    REQUIRE(host.ok());

    for (int lens = 0; lens < 2; ++lens) {
        const OsvPlane& plane = frame.planes[lens];
        const osv::video::PlanarFrame16& ref = host.value().lens[static_cast<std::size_t>(lens)];
        const std::size_t w = ref.width;
        std::vector<std::uint16_t> row(w);
        REQUIRE(cuCtxPushCurrent(cuda.context) == CUDA_SUCCESS);
        std::size_t mismatches = 0;
        for (const std::uint32_t y : {0u, 777u, 1500u, 2999u}) {
            const auto* src = plane.y + static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.strideY);
            REQUIRE(cuMemcpyDtoH(row.data(), static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(src)),
                                 w * sizeof(std::uint16_t)) == CUDA_SUCCESS);
            const std::uint16_t* expected = ref.plane[0] + static_cast<std::size_t>(y) * ref.strideElems[0];
            for (std::size_t x = 0; x < w; ++x) {
                if (static_cast<std::uint16_t>(row[x] >> 6) != expected[x]) {
                    ++mismatches;
                }
            }
        }
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
        INFO("lens " << lens << " luma mismatches vs software decode: " << mismatches);
        CHECK(mismatches == 0);
    }

    api.release(frame.lease, nullptr);
}

TEST_CASE("the engine honours the working-space transfer and survives many acquire/release cycles",
          "[importer][engine][cuda][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    TestContext cuda;
    if (!cuda.context) {
        SKIP("CUDA unavailable: " << cuda.reason);
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const EngineApi api = resolveEngine();
    REQUIRE(api.ok());
    const std::wstring path = sampleClipPath().wstring();
    char error[512] = {};

    // A sequence working in HLG gets an HLG block from a PQ clip.
    {
        OsvEngineFrameRequest request = requestFor(path, 3, cuda.context, OSV_TRANSFER_HLG);
        OsvEngineFrame frame = emptyFrame();
        REQUIRE(api.acquire(&request, &frame, error, sizeof(error)) == OSV_ENGINE_OK);
        CHECK(frame.stitch.color.transfer == OSV_TRANSFER_HLG);
        api.release(frame.lease, nullptr);
    }

    // Far more acquires than the VRAM cache has slots: only works if every
    // release really gives its slot back.  Media times off the frame grid by
    // a few ticks must still round to the frame the importer would serve.
    for (std::uint32_t i = 0; i < 90; ++i) {
        const std::uint32_t target = (i * 37u) % 65u;
        OsvEngineFrameRequest request = requestFor(path, target, cuda.context);
        request.mediaTicks += (i % 2 == 0) ? 3 : -3;
        if (request.mediaTicks < 0) {
            request.mediaTicks = 0;
        }
        request.purpose = (i % 3 == 0) ? OSV_ENGINE_PURPOSE_INTERACTIVE : OSV_ENGINE_PURPOSE_EXACT;
        OsvEngineFrame frame = emptyFrame();
        const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
        INFO("cycle " << i << " frame " << target << ": " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        CHECK(frame.frameIndex == target);
        api.release(frame.lease, nullptr);
    }
}
