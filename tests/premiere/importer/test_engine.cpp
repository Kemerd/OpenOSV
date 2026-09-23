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
#include "osv/render/RenderJob.h"
#include "osv/render/SeamTools.h"
#include "osv/video/DualStreamReader.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <algorithm>
#include <cmath>
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
    // [WP-SEAMTOOLS] Default Source Settings: no seam smoothing, no low band.
    CHECK(frame.stitch.seamSmoothEnabled == 0);
    CHECK(frame.seamLowDevice == nullptr);

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

    // ---- [WP-SEAM] the carved seam travels with the default seam setting -----
    // One (latitude, half width) pair per column, in our context, every entry
    // a usable number: the direct kernel reads it straight from here.
    CHECK(frame.stitch.blendSeamEnabled == 1);
    REQUIRE(frame.blendSeamDevice != nullptr);
    REQUIRE(frame.stitch.blendSeamColumns > 0);
    {
        REQUIRE(cuCtxPushCurrent(cuda.context) == CUDA_SUCCESS);
        CHECK(contextOf(frame.blendSeamDevice) == cuda.context);
        std::vector<float> table(static_cast<std::size_t>(frame.stitch.blendSeamColumns) * 2u);
        const auto src = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(frame.blendSeamDevice));
        REQUIRE(cuMemcpyDtoH(table.data(), src, table.size() * sizeof(float)) == CUDA_SUCCESS);
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
        std::size_t bad = 0;
        for (std::size_t i = 0; i < table.size(); i += 2) {
            // Latitude inside the overlap band, a positive feather.
            if (!std::isfinite(table[i]) || std::fabs(table[i]) > 0.2f || !(table[i + 1] > 0.0f)) {
                ++bad;
            }
        }
        CHECK(bad == 0);
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

TEST_CASE("Source Settings changed in Premiere reach the engine's next frame", "[importer][engine][cuda][sample]") {
    // Field report: dropping Exposure to -1 in Source Settings darkened the
    // Source monitor (the importer's equirect) but not the direct path's
    // view.  The engine renders with its OWN instance of the file, so the
    // settings Premiere hands its instances must be published to it and
    // applied on the next acquire.  This drives exactly that: a Premiere
    // instance receives exposure -1 through imGetInfo8, and the engine's
    // colour block must carry a 2^-1 exposure gain from the next frame on.
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

    const auto gainOf = [&](std::uint32_t frameIndex) {
        OsvEngineFrameRequest request = requestFor(path, frameIndex, cuda.context);
        OsvEngineFrame frame = emptyFrame();
        const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        const float gain = frame.stitch.color.exposureGain;
        api.release(frame.lease, nullptr);
        return gain;
    };

    // Before: default settings, unit gain.
    CHECK(gainOf(2) == 1.0f);

    // Premiere opens the clip with the user's new settings...
    osv::premiere::PrefsBlob prefs = osv::premiere::PrefsBlob::defaults();
    prefs.exposureStops = -1.0f;
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

    // ...and the engine's very next frame renders with them.
    CHECK(gainOf(3) == 0.5f);

    // And back again, so a later change is not missed either.
    prefs.exposureStops = 0.5f;
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
    CHECK(gainOf(4) == Catch::Approx(1.41421356f));
}

// =============================================================================
//  [WP-FLARE] the sun ghost removal reaches the direct path in the stitch block
// =============================================================================

TEST_CASE("a device frame carries the fitted sun ghosts in its stitch block, and none when switched off",
          "[importer][engine][flare][cuda][sample]") {
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
    char error[512] = {};

    // ---- default settings: removal on, analysed from the frames in VRAM -------
    // An EXACT request measures its bucket now (the GPU sampler reduces both
    // lenses on the device), so the block must already carry the model.
    {
        OsvEngineFrameRequest request = requestFor(path, 5, cuda.context);
        OsvEngineFrame frame = emptyFrame();
        const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        CHECK(frame.exact == 1);
        CHECK(frame.stitch.flareEnabled == 1);
        // The sun and its ghosts are in the master lens (stream 1) only.
        CHECK(frame.stitch.flare[0].ghostCount == 0);
        REQUIRE(frame.stitch.flare[1].ghostCount >= 1);
        REQUIRE(frame.stitch.flare[1].ghostCount <= OSV_FLARE_MAX_GHOSTS);
        // The bright pill ghost, where the library finds it on the host frame
        // (tests/unit/test_flare.cpp), with a sane shape and positive light.
        bool pill = false;
        for (int k = 0; k < frame.stitch.flare[1].ghostCount; ++k) {
            const OsvFlareGhost& g = frame.stitch.flare[1].ghost[k];
            CHECK(std::isfinite(g.cx));
            CHECK(g.hx > 0.0f);
            CHECK(g.hy > 0.0f);
            CHECK(g.soft > 0.0f);
            CHECK(g.reach2 > g.hx * g.hx);
            CHECK(g.amp[1] > 0.0f);
            pill = pill || std::hypot(g.cx - 1182.0f, g.cy - 1547.0f) < 12.0f;
        }
        CHECK(pill);
        api.release(frame.lease, nullptr);
    }

    // ---- switched off in Source Settings: the block carries nothing -------------
    osv::premiere::PrefsBlob prefs = osv::premiere::PrefsBlob::defaults();
    prefs.flareRemoval = 0;
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
    {
        OsvEngineFrameRequest request = requestFor(path, 6, cuda.context);
        OsvEngineFrame frame = emptyFrame();
        const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        CHECK(frame.stitch.flareEnabled == 0);
        CHECK(frame.stitch.flare[1].ghostCount == 0);
        api.release(frame.lease, nullptr);
    }
}

// =============================================================================
//  [WP-VIGNETTE] the lens shading correction reaches the direct path
// =============================================================================

TEST_CASE("a device frame carries the lens shading correction in its stitch block, and none when switched off",
          "[importer][engine][lensshading][cuda][sample]") {
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
    char error[512] = {};

    // ---- default settings: Auto, measured from the frames in VRAM --------------
    // The correction travels inside the stitch block itself (no table), so the
    // direct kernel adds exactly what the importer's equirect adds.  On the
    // sample the master lens (stream 1) carries the ring at ~86 deg; the
    // slave's table, if any, stays small.
    {
        OsvEngineFrameRequest request = requestFor(path, 12, cuda.context);
        OsvEngineFrame frame = emptyFrame();
        const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        CHECK(frame.exact == 1);
        REQUIRE(frame.stitch.shadeEnabled == 1);
        CHECK(frame.stitch.shadeStrength == Catch::Approx(1.0));
        CHECK(frame.stitch.shadeDThetaRad > 0.0f);
        // The master's radial factor peaks near 86 deg: knot (86 - 76) / 0.5.
        const float thetaDeg0 = frame.stitch.shadeTheta0Rad * 57.2957795f;
        const float stepDeg = frame.stitch.shadeDThetaRad * 57.2957795f;
        float peak = 0.0f;
        int peakKnot = -1;
        for (int k = 0; k < OSV_SHADE_THETA_N; ++k) {
            // The product with the largest azimuth weight, for either sign
            // convention of the separable factors.
            float amax = 0.0f;
            for (int s = 0; s < OSV_SHADE_PHI_N; ++s) {
                amax = std::max(amax, std::fabs(frame.stitch.shade[1].azimuth[0][s]));
            }
            const float v = std::fabs(frame.stitch.shade[1].radial[0][k]) * amax;
            if (v > peak) {
                peak = v;
                peakKnot = k;
            }
        }
        REQUIRE(peakKnot >= 0);
        const float peakDeg = thetaDeg0 + stepDeg * static_cast<float>(peakKnot);
        INFO("master peak " << peak << " at " << peakDeg << " deg");
        CHECK(peakDeg > 84.0f);
        CHECK(peakDeg < 88.5f);
        CHECK(peak > 0.005f);
        api.release(frame.lease, nullptr);
    }

    // ---- switched off in Source Settings: the block carries nothing -------------
    osv::premiere::PrefsBlob prefs = osv::premiere::PrefsBlob::defaults();
    prefs.lensShading = static_cast<std::uint8_t>(osv::premiere::PrefsLensShading::Off);
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
    {
        OsvEngineFrameRequest request = requestFor(path, 13, cuda.context);
        OsvEngineFrame frame = emptyFrame();
        const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        CHECK(frame.stitch.shadeEnabled == 0);
        CHECK(frame.stitch.shade[1].radial[0][20] == 0.0f);
        api.release(frame.lease, nullptr);
    }
}

// =============================================================================
//  [WP-SEAMTOOLS] the seam smoothing's low band reaches the direct path
// =============================================================================

TEST_CASE("with Seam Smoothing on, the engine carries the frame's low band in the caller's context",
          "[importer][engine][seamtools][cuda][sample]") {
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
    char error[512] = {};

    // ---- the user turns Seam Smoothing to 2 deg in Source Settings -------------
    osv::premiere::PrefsBlob prefs = osv::premiere::PrefsBlob::defaults();
    prefs.setSeamSmoothingDeg(2.0);
    auto clip = harness.openClip(sampleClipPath(), 91);
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

    constexpr std::uint32_t kFrame = 9;
    OsvEngineFrameRequest request = requestFor(path, kFrame, cuda.context);
    OsvEngineFrame frame = emptyFrame();
    const std::int32_t rc = api.acquire(&request, &frame, error, sizeof(error));
    INFO("engine error: " << error);
    REQUIRE(rc == OSV_ENGINE_OK);

    // ---- the block and the table ------------------------------------------------
    // The default stitch carves a seam, so the smoothing is on, sized from the
    // 3000 x 3000 lenses, with its 2 deg half width.
    REQUIRE(frame.stitch.blendSeamEnabled == 1);
    REQUIRE(frame.stitch.seamSmoothEnabled == 1);
    CHECK(frame.stitch.seamLowW == 375);
    CHECK(frame.stitch.seamLowH == 375);
    CHECK(frame.stitch.seamSmoothHalfRad == Catch::Approx(2.0 * 3.14159265358979 / 180.0));
    REQUIRE(frame.seamLowDevice != nullptr);
    const std::size_t floats = 2u * 375u * 375u * 4u;
    std::vector<float> device(floats);
    REQUIRE(cuCtxPushCurrent(cuda.context) == CUDA_SUCCESS);
    CHECK(contextOf(frame.seamLowDevice) == cuda.context);
    // Synchronous copy on the legacy stream: orders after the engine's build
    // on the request's (null = default) stream.
    REQUIRE(cuMemcpyDtoH(device.data(), static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(frame.seamLowDevice)),
                         floats * sizeof(float)) == CUDA_SUCCESS);
    CUcontext popped = nullptr;
    (void)cuCtxPopCurrent(&popped);

    // ---- ...and it is the low band of THIS frame -----------------------------------
    // Built again on the host from the software decode of the same frame with
    // the same block: the same numbers up to the GPU's float rounding.
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
    OsvPlane planes[2] = {};
    REQUIRE(osv::render::fillPlane(host.value().lens[0], planes[0]));
    REQUIRE(osv::render::fillPlane(host.value().lens[1], planes[1]));
    std::vector<float> reference;
    std::vector<float> scratch;
    REQUIRE(osv::render::buildSeamLowBand(frame.stitch, planes, reference, scratch, nullptr).ok());
    REQUIRE(reference.size() == floats);
    double worst = 0.0;
    std::size_t covered = 0;
    for (std::size_t i = 0; i < floats; i += 4) {
        const double a = reference[i + 3];
        CHECK(std::fabs(device[i + 3] - reference[i + 3]) < 1e-5);  // coverage: the same rule, exactly or nearly
        if (a > 0.05) {
            ++covered;
            for (int c = 0; c < 3; ++c) {
                const double r = reference[i + static_cast<std::size_t>(c)] / a;
                const double d = device[i + static_cast<std::size_t>(c)] / device[i + 3];
                worst = std::max(worst, std::fabs(d - r) / std::max(1e-3, std::fabs(r)));
            }
        }
    }
    INFO("worst relative difference over " << covered << " covered texels: " << worst);
    CHECK(covered > floats / 4 / 2);
    CHECK(worst < 1e-3);
    api.release(frame.lease, nullptr);
}