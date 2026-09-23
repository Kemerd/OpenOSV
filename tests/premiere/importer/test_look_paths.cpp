// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_look_paths.cpp - [WP-LOOK] the Rec.709 display look, end to end
// through the BUILT importer module.
//
// What is pinned here, in the order a user meets it:
//
//   * the preference byte (PrefsBlob offset 28): zero - a fresh blob and
//     every blob written before the byte existed - is the DJI Studio look;
//     the modal dialog's mapping round-trips it and carries it through
//     untouched when only other fields change;
//   * the importer's own frames: the two looks render visibly apart on the
//     Rec.709 output and byte-identically on PQ, on the host frame path and
//     on the GPU frame path alike (the PPix cache is keyed on the whole blob,
//     so a look change can never be served a stale frame);
//   * the engine / direct path: the everyday setup is a Rec.709 sequence
//     holding PQ clips, rendered by the reframe effect straight into the
//     working space with OpenOSV's own conversion - so the Program monitor
//     shows OUR Rec.709 rendering, and it must carry the clip's look, byte
//     for byte the block the library builds, and follow a look change through
//     the Source Settings generation like any other colour setting.
//
// [sample] tests SKIP without the 6K clip; [cuda] tests SKIP without a CUDA
// device.

#include "ImporterHarness.h"
#include "ImporterPlugin.h"

#include "MockHost.h"
#include "OsvEngineAbi.h"
#include "PixelCopy.h"
#include "PrefsBlob.h"

#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "osv/color/ColorParams.h"
#include "osv/color/Look.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cuda.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
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

using osv::premiere::DialogControls;
using osv::premiere::PrefsBlob;
using osv::premiere::PrefsColorOutput;
using osv::premiere::PrefsLook;
using osv::premiere::test::ImporterHarness;
using osv::premiere::test::sampleClipAvailable;
using osv::premiere::test::sampleClipPath;
namespace pc = osv::premiere::pixelcopy;

namespace {

/// One 59.94 fps frame in Premiere ticks (254016000000 * 1001 / 60000).
constexpr std::int64_t kTicksPerFrame5994 = 4237833600LL;

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

/// The library's block for the default fit, `transfer` and `look`: exactly
/// what every importer route builds for a 10-bit D-Log M clip.
[[nodiscard]] OsvColorParams libraryBlock(osv::color::OutputTransfer transfer,
                                          osv::color::Look look = osv::color::kDefaultLook) {
    return osv::color::makeColorParams(osv::color::kDefaultDlogMFit, transfer, 0.0f,
                                       osv::color::InputEncoding::DLogM, true, 10u, nullptr,
                                       osv::color::kBt2408SceneScale, look);
}

/// Default prefs with a colour output and a look.
[[nodiscard]] PrefsBlob prefsWith(PrefsColorOutput output, PrefsLook look) {
    PrefsBlob p = PrefsBlob::defaults();
    p.colorOutput = static_cast<std::uint8_t>(output);
    p.look = static_cast<std::uint8_t>(look);
    return p;
}

/// The engine exports this file drives, resolved from the loaded module.
struct EngineApi {
    OsvEngineAcquireFrameFn acquire = nullptr;
    OsvEngineReleaseFrameFn release = nullptr;
    OsvEngineQuerySettingsFn query = nullptr;
    EngineApi() {
        const HMODULE module = GetModuleHandleW(OSV_ENGINE_MODULE_NAME);
        if (!module) {
            return;
        }
        acquire = reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
        release = reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
        query = reinterpret_cast<OsvEngineQuerySettingsFn>(GetProcAddress(module, OSV_ENGINE_SYM_QUERY_SETTINGS));
    }
    [[nodiscard]] bool ok() const noexcept { return acquire && release && query; }
};

/// What one engine frame reports about colour: its block and the Source
/// Settings generation it was rendered with.
struct EngineColour {
    OsvColorParams color{};
    std::uint32_t generation = 0;
};

/// Acquire and release one engine frame of `path` for a working-space
/// transfer (or the clip's own output with OSV_ENGINE_TRANSFER_FROM_CLIP).
[[nodiscard]] EngineColour engineColour(const EngineApi& api, const std::wstring& path, CUcontext context,
                                        std::uint32_t frameIndex, std::int32_t transfer) {
    OsvEngineFrameRequest r{};
    r.structSize = sizeof(r);
    r.path = path.c_str();
    r.mediaTicks = kTicksPerFrame5994 * static_cast<std::int64_t>(frameIndex);
    r.purpose = OSV_ENGINE_PURPOSE_EXACT;
    r.outputTransfer = transfer;
    r.cuContext = context;
    r.cuStream = nullptr;
    OsvEngineFrame frame{};
    frame.structSize = sizeof(frame);
    char error[512] = {};
    const std::int32_t rc = api.acquire(&r, &frame, error, static_cast<std::int32_t>(sizeof(error)));
    INFO("engine error: " << error);
    REQUIRE(rc == OSV_ENGINE_OK);
    REQUIRE(frame.lease != nullptr);
    EngineColour out;
    out.color = frame.stitch.color;
    out.generation = frame.settings.generation;
    api.release(frame.lease, nullptr);
    return out;
}

/// The PPix suite of the mock host, released on scope exit.
struct PPixSuite {
    ImporterHarness& harness;
    const PrSDKPPixSuite* suite = nullptr;
    explicit PPixSuite(ImporterHarness& h) : harness(h) {
        const void* raw = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &raw) ==
                kSPNoError);
        suite = static_cast<const PrSDKPPixSuite*>(raw);
    }
    ~PPixSuite() { harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion); }
    PPixSuite(const PPixSuite&) = delete;
    PPixSuite& operator=(const PPixSuite&) = delete;
};

/// One imGetSourceVideo frame in 32-bit float BGRA, as tight host rows of
/// floats (the PPix is read and disposed).
[[nodiscard]] std::vector<float> renderFloat(ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                             const PrSDKPPixSuite* ppix, std::uint32_t frame, const PrefsBlob& prefs) {
    ImporterHarness::SourceVideoRequest request;
    request.frameTime = kTicksPerFrame5994 * static_cast<std::int64_t>(frame);
    request.format = PrPixelFormat_BGRA_4444_32f;
    request.width = 1920;
    request.height = 960;
    request.intent = imRenderIntent_Export;
    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
    REQUIRE(hand != nullptr);
    REQUIRE(ppix != nullptr);
    const auto info = harness.host().inspect(hand);
    REQUIRE(info.has_value());
    REQUIRE(info->format == PrPixelFormat_BGRA_4444_32f);

    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) == suiteError_NoError);
    REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
    REQUIRE(pixels != nullptr);
    // Tight rows so two frames compare element by element regardless of pitch.
    const std::size_t tight = static_cast<std::size_t>(info->width) * pc::kBytesPerPixel32f;
    std::vector<float> out(tight / sizeof(float) * info->height);
    for (std::uint32_t r = 0; r < info->height; ++r) {
        std::memcpy(reinterpret_cast<char*>(out.data()) + tight * r, pc::rowAddress(pixels, rowBytes, r), tight);
    }
    ppix->Dispose(hand);
    return out;
}

/// Largest per-sample difference between two frames of the same size.
[[nodiscard]] float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }
    return worst;
}

/// OPENOSV_IMPORTER_NO_GPU_DECODE for the lifetime of the object, which puts
/// the importer on its host frame path.  The .prm shares this process's CRT
/// (both /MD), so _putenv_s reaches its getenv_s.
struct HostFramePathOnly {
    HostFramePathOnly() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", "1"); }
    ~HostFramePathOnly() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", ""); }
    HostFramePathOnly(const HostFramePathOnly&) = delete;
    HostFramePathOnly& operator=(const HostFramePathOnly&) = delete;
};

}  // namespace

// =============================================================================
//  The preference byte
// =============================================================================

TEST_CASE("the look preference byte defaults to the DJI Studio look and repairs corruption",
          "[importer][prefs][look]") {
    // A fresh blob and every blob written before the byte existed (zero) read
    // as the DJI Studio look, which is also what makeColorParams builds when
    // no look is named - the two defaults cannot disagree.
    PrefsBlob p = PrefsBlob::defaults();
    CHECK(p.look == 0);
    CHECK(p.lookChoice() == PrefsLook::DjiStudio);
    CHECK(osv::color::kDefaultLook == osv::color::Look::DjiStudio);
    // Both valid values survive sanitise().
    p.look = static_cast<std::uint8_t>(PrefsLook::Standard);
    REQUIRE(p.sanitise());
    CHECK(p.lookChoice() == PrefsLook::Standard);
    // A corrupt byte lands on the default; the unused byte of the range is
    // zeroed.
    p.look = 0xC3;
    p.padAfterLook = 0x5A;
    REQUIRE_FALSE(p.sanitise());
    CHECK(p.lookChoice() == PrefsLook::DjiStudio);
    CHECK(p.padAfterLook == 0);
    // And the byte sits in the range assigned to it, inside the 128 bytes.
    static_assert(offsetof(PrefsBlob, look) == 28, "look sits at 28");
    static_assert(sizeof(PrefsBlob) == PrefsBlob::kSize, "the blob stays 128 bytes");
}

TEST_CASE("the Source Settings dialog round-trips the look and keeps it when other fields change",
          "[importer][prefs][look]") {
    // Every value through the dialog mapping, both directions.
    for (const PrefsLook look : {PrefsLook::DjiStudio, PrefsLook::Standard}) {
        const PrefsBlob in = prefsWith(PrefsColorOutput::Rec709, look);
        const DialogControls c = osv::premiere::controlsFromPrefs(in);
        CHECK(c.look == static_cast<int>(look));
        const PrefsBlob back = osv::premiere::prefsFromControls(c, in);
        CHECK(back == in);
    }
    // An old project's blob (the byte was zero before it existed) opens the
    // dialog on "DJI (default)".
    PrefsBlob old = PrefsBlob::defaults();
    old.look = 0;
    CHECK(osv::premiere::controlsFromPrefs(old).look == static_cast<int>(PrefsLook::DjiStudio));
    // Changing only the colour output keeps the chosen look: the look is a
    // property of the Rec.709 rendering, not reset by visiting another output.
    const PrefsBlob standard = prefsWith(PrefsColorOutput::Rec709, PrefsLook::Standard);
    DialogControls c = osv::premiere::controlsFromPrefs(standard);
    c.colorOutput = static_cast<int>(PrefsColorOutput::PQ);
    const PrefsBlob pq = osv::premiere::prefsFromControls(c, standard);
    CHECK(pq.color() == PrefsColorOutput::PQ);
    CHECK(pq.lookChoice() == PrefsLook::Standard);
    // A combo with no selection (-1) or a garbage index lands on the default.
    for (const int hostile : {-1, 2, 99}) {
        c.look = hostile;
        CHECK(osv::premiere::prefsFromControls(c, standard).lookChoice() == PrefsLook::DjiStudio);
    }
}

// =============================================================================
//  The importer's own frames
// =============================================================================

TEST_CASE("the importer renders the two looks apart on Rec.709 and identically on PQ",
          "[importer][video][color][look][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    // Both frame paths: the GPU one (the default when a CUDA device exists)
    // and the host one it replaces.  Where there is no GPU both runs take the
    // host path, which is still a valid (if duplicated) check.
    const bool gpuPath = GENERATE(true, false);
    INFO((gpuPath ? "GPU frame path" : "host frame path"));
    std::unique_ptr<HostFramePathOnly> hostOnly;
    if (!gpuPath) {
        hostOnly = std::make_unique<HostFramePathOnly>();
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);
    auto clip = harness.openClip(sampleClipPath(), gpuPath ? 71 : 72);
    REQUIRE(clip.open());
    constexpr std::uint32_t kFrame = 3;

    // The PPix cache is keyed on the whole blob, so each of these is a real
    // render even though the frame is the same.
    const auto dji709 = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(PrefsColorOutput::Rec709,
                                                                                  PrefsLook::DjiStudio));
    const auto std709 = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(PrefsColorOutput::Rec709,
                                                                                  PrefsLook::Standard));
    const auto djiPq = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(PrefsColorOutput::PQ,
                                                                                 PrefsLook::DjiStudio));
    const auto stdPq = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(PrefsColorOutput::PQ,
                                                                                 PrefsLook::Standard));

    // Rec.709: two genuinely different renderings (the look lifts highlights,
    // crushes the toe and saturates the sky by several percent of signal).
    const float lookDelta = maxDiff(dji709, std709);
    INFO("Rec.709 DJI vs standard: max difference " << lookDelta);
    CHECK(lookDelta > 0.02f);
    // PQ: the look does not exist there, so the frames are the same bytes.
    CHECK(maxDiff(djiPq, stdPq) == 0.0f);
    CHECK(std::memcmp(djiPq.data(), stdPq.data(), djiPq.size() * sizeof(float)) == 0);
}

// =============================================================================
//  The engine / direct path
// =============================================================================

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
    const EngineApi api;
    REQUIRE(api.ok());
    const std::wstring path = sampleClipPath().wstring();
    std::uint32_t frameIndex = 1;
    const auto colourFor = [&](std::int32_t transfer) {
        return engineColour(api, path, cuda.context, frameIndex++, transfer).color;
    };

    // ---- a PQ clip (the default) in a Rec.709 sequence -----------------------
    auto pqClip = harness.openClip(sampleClipPath(), 61);
    REQUIRE(pqClip.open());
    PrefsBlob pq = prefsWith(PrefsColorOutput::PQ, PrefsLook::DjiStudio);
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
    PrefsBlob rec709 = prefsWith(PrefsColorOutput::Rec709, PrefsLook::DjiStudio);
    REQUIRE(harness.getInfo8(rec709Clip, info, &rec709) == imNoErr);
    const OsvColorParams own = colourFor(OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(own.transfer == OSV_TRANSFER_REC709);
    CHECK(own.look.id == OSV_LOOK_DJI);
    CHECK(std::memcmp(&own, &viaWorkingSpace, sizeof(OsvColorParams)) == 0);
}

TEST_CASE("the engine follows a look change through the Source Settings generation",
          "[importer][engine][settings][color][look][cuda][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    LookCudaContext cuda;  // before the harness: outlives imShutdown
    if (!cuda.context) {
        SKIP("CUDA unavailable: " << cuda.reason);
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const EngineApi api;
    REQUIRE(api.ok());
    const std::wstring path = sampleClipPath().wstring();
    std::uint32_t frameIndex = 5;
    imFileInfoRec8 info{};

    // Each change arrives the way Premiere delivers one: a NEWER importer
    // instance of the clip is handed the changed blob and publishes it.
    csSDK_int32 importerId = 81;
    std::vector<ImporterHarness::ClipHandle> instances;
    const auto publish = [&](const PrefsBlob& prefs) {
        instances.push_back(harness.openClip(sampleClipPath(), importerId++));
        REQUIRE(instances.back().open());
        PrefsBlob copy = prefs;
        REQUIRE(harness.getInfo8(instances.back(), info, &copy) == imNoErr);
    };

    // ---- Rec.709 with the DJI look --------------------------------------------
    publish(prefsWith(PrefsColorOutput::Rec709, PrefsLook::DjiStudio));
    const EngineColour dji = engineColour(api, path, cuda.context, frameIndex++, OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(dji.color.look.id == OSV_LOOK_DJI);
    CHECK(dji.generation != 0u);

    // ---- the user picks "OpenOSV standard": a new generation, a new block -----
    publish(prefsWith(PrefsColorOutput::Rec709, PrefsLook::Standard));
    const EngineColour standard =
        engineColour(api, path, cuda.context, frameIndex++, OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(standard.generation > dji.generation);
    CHECK(standard.color.transfer == OSV_TRANSFER_REC709);
    CHECK(standard.color.look.id == OSV_LOOK_STANDARD);
    const OsvColorParams expectedStandard =
        libraryBlock(osv::color::OutputTransfer::Rec709, osv::color::Look::Standard);
    CHECK(std::memcmp(&standard.color, &expectedStandard, sizeof(OsvColorParams)) == 0);
    // A PQ clip rendered into a Rec.709 working space follows the choice too.
    CHECK(engineColour(api, path, cuda.context, frameIndex++, OSV_TRANSFER_REC709).color.look.id ==
          OSV_LOOK_STANDARD);

    // ---- and back: the DJI look returns with yet another generation -----------
    publish(prefsWith(PrefsColorOutput::Rec709, PrefsLook::DjiStudio));
    const EngineColour back = engineColour(api, path, cuda.context, frameIndex++, OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(back.generation > standard.generation);
    CHECK(back.color.look.id == OSV_LOOK_DJI);
    CHECK(std::memcmp(&back.color, &dji.color, sizeof(OsvColorParams)) == 0);
}
