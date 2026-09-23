// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_look_paths.cpp - [WP-LOOK] the DJI Studio look on the Rec.709 output
// reaches the route the user actually watches: the reframe effect's direct
// path rendering into a Rec.709 sequence working space, through the BUILT
// importer module's engine ABI.
//
// The everyday setup is a Rec.709 sequence holding PQ clips.  The direct path
// then renders each clip straight into the working space with OpenOSV's own
// conversion (PrefsDirectColour::SequenceSpace), so the Program monitor shows
// OUR Rec.709 rendering - and that must be the DJI look, byte for byte the
// block the library builds, whether the clip's own choice is PQ (the working
// space overrides the transfer) or Rec.709 (the clip's own block is used).
// The importer's own host and GPU frame paths build the same block through
// the same makeColorParams call; test_importer_bitdepth.cpp pins that those
// two paths and the Rec.709 connection-space override render identically.

#include "ImporterHarness.h"
#include "ImporterPlugin.h"

#include "OsvEngineAbi.h"

#include "osv/color/ColorParams.h"
#include "osv/color/Look.h"

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <cstddef>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using osv::premiere::PrefsBlob;
using osv::premiere::PrefsColorOutput;
using osv::premiere::test::ImporterHarness;
using osv::premiere::test::sampleClipAvailable;
using osv::premiere::test::sampleClipPath;

namespace {

/// A private CUDA context (the effect's), created before the harness so it
/// outlives imShutdown, which is when the engine frees its decoders in it.
struct LookCudaContext {
    CUcontext context = nullptr;
    std::string reason;
    LookCudaContext() {
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
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }
    ~LookCudaContext() {
        if (context) {
            (void)cuCtxDestroy(context);
        }
    }
    LookCudaContext(const LookCudaContext&) = delete;
    LookCudaContext& operator=(const LookCudaContext&) = delete;
};

/// The library's block for the default settings and `transfer`: exactly
/// what every importer route builds for a 10-bit D-Log M clip.
[[nodiscard]] OsvColorParams libraryBlock(osv::color::OutputTransfer transfer) {
    return osv::color::makeColorParams(osv::color::kDefaultDlogMFit, transfer, 0.0f,
                                       osv::color::InputEncoding::DLogM, true, 10u);
}

}  // namespace

TEST_CASE("the direct path renders a Rec.709 working space with the DJI Studio look",
          "[importer][engine][color][look][cuda][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    LookCudaContext cuda;  // before the harness: outlives imShutdown
    if (!cuda.context) {
        SKIP("CUDA unavailable: " << cuda.reason);
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const HMODULE module = GetModuleHandleW(OSV_ENGINE_MODULE_NAME);
    REQUIRE(module != nullptr);
    const auto acquireFn =
        reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
    const auto releaseFn =
        reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
    REQUIRE(acquireFn != nullptr);
    REQUIRE(releaseFn != nullptr);
    const std::wstring path = sampleClipPath().wstring();

    // One frame's colour block from the engine, for a working space (or the
    // clip's own output with OSV_ENGINE_TRANSFER_FROM_CLIP).
    constexpr std::int64_t kTicksPerFrame5994 = 4237833600LL;
    std::uint32_t frameIndex = 1;
    const auto colourFor = [&](std::int32_t transfer) {
        OsvEngineFrameRequest r{};
        r.structSize = sizeof(r);
        r.path = path.c_str();
        r.mediaTicks = kTicksPerFrame5994 * static_cast<std::int64_t>(frameIndex++);
        r.purpose = OSV_ENGINE_PURPOSE_EXACT;
        r.outputTransfer = transfer;
        r.cuContext = cuda.context;
        r.cuStream = nullptr;
        OsvEngineFrame frame{};
        frame.structSize = sizeof(frame);
        char error[512] = {};
        const std::int32_t rc = acquireFn(&r, &frame, error, static_cast<std::int32_t>(sizeof(error)));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        REQUIRE(frame.lease != nullptr);
        const OsvColorParams colour = frame.stitch.color;
        releaseFn(frame.lease, nullptr);
        return colour;
    };

    // ---- a PQ clip (the default) in a Rec.709 sequence -----------------------
    auto pqClip = harness.openClip(sampleClipPath(), 61);
    REQUIRE(pqClip.open());
    PrefsBlob pq = PrefsBlob::defaults();
    pq.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::PQ);
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(pqClip, info, &pq) == imNoErr);

    // The working space overrides the transfer, and the look comes with it.
    const OsvColorParams viaWorkingSpace = colourFor(OSV_TRANSFER_REC709);
    CHECK(viaWorkingSpace.transfer == OSV_TRANSFER_REC709);
    CHECK(viaWorkingSpace.look.id == OSV_LOOK_DJI);
    CHECK(osv::color::lookOf(viaWorkingSpace) == osv::color::Look::DjiStudio);
    const OsvColorParams expected = libraryBlock(osv::color::OutputTransfer::Rec709);
    CHECK(std::memcmp(&viaWorkingSpace, &expected, sizeof(OsvColorParams)) == 0);

    // The HDR working spaces and the clip's own PQ carry no look.
    CHECK(colourFor(OSV_TRANSFER_PQ).look.id == OSV_LOOK_STANDARD);
    CHECK(colourFor(OSV_TRANSFER_HLG).look.id == OSV_LOOK_STANDARD);
    CHECK(colourFor(OSV_ENGINE_TRANSFER_FROM_CLIP).look.id == OSV_LOOK_STANDARD);

    // ---- the same clip with Rec.709 chosen in Source Settings -----------------
    // A newer instance publishes the change, as Premiere does; the clip's own
    // block is then the same DJI-look block the working-space override built.
    auto rec709Clip = harness.openClip(sampleClipPath(), 62);
    REQUIRE(rec709Clip.open());
    PrefsBlob rec709 = PrefsBlob::defaults();
    rec709.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    REQUIRE(harness.getInfo8(rec709Clip, info, &rec709) == imNoErr);
    const OsvColorParams own = colourFor(OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(own.transfer == OSV_TRANSFER_REC709);
    CHECK(own.look.id == OSV_LOOK_DJI);
    CHECK(std::memcmp(&own, &viaWorkingSpace, sizeof(OsvColorParams)) == 0);
}

TEST_CASE("the look preference byte defaults to the DJI Studio look and repairs corruption",
          "[importer][prefs][look]") {
    // A fresh blob and every blob written before the byte existed (zero) read
    // as the DJI Studio look, which is also what makeColorParams builds when
    // no look is named - the two defaults cannot disagree.
    PrefsBlob p = PrefsBlob::defaults();
    CHECK(p.look == 0);
    CHECK(p.lookChoice() == osv::premiere::PrefsLook::DjiStudio);
    CHECK(osv::color::kDefaultLook == osv::color::Look::DjiStudio);
    // Both valid values survive sanitise().
    p.look = static_cast<std::uint8_t>(osv::premiere::PrefsLook::Standard);
    REQUIRE(p.sanitise());
    CHECK(p.lookChoice() == osv::premiere::PrefsLook::Standard);
    // A corrupt byte lands on the default; the unused byte of the range is
    // zeroed.
    p.look = 0xC3;
    p.padAfterLook = 0x5A;
    REQUIRE_FALSE(p.sanitise());
    CHECK(p.lookChoice() == osv::premiere::PrefsLook::DjiStudio);
    CHECK(p.padAfterLook == 0);
    // And the byte sits in the range assigned to it, inside the 128 bytes.
    static_assert(offsetof(PrefsBlob, look) == 28, "look sits at 28");
    static_assert(sizeof(PrefsBlob) == PrefsBlob::kSize, "the blob stays 128 bytes");
}
