// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_hdr_peak_paths.cpp - [WP-HDRPEAK] the PQ output's HDR peak, end to end
// through the BUILT importer module.
//
// What is pinned here, in the order a user meets it:
//
//   * the preference byte (PrefsBlob offset 54): zero - a fresh blob and every
//     blob written before the byte existed - is 1000 nits, no roll-off; a
//     corrupt byte is repaired to it; the modal dialog's mapping round-trips
//     every choice and keeps it when only other fields change;
//   * the importer's own frames: PQ renders apart per peak and never above
//     the chosen one, while HLG and Rec.709 are byte-identical whatever the
//     peak - on the GPU frame path and the host frame path alike;
//   * the Properties panel names the peak only when it is not 1000;
//   * the engine / direct path: the clip's block carries the peak, byte for
//     byte the block the library builds, a change moves the Source Settings
//     generation (which is what re-renders the Program monitor), and a
//     Rec.709 working space is untouched by it.
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

#include "osv/color/ColorMath.h"
#include "osv/color/ColorParams.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cuda.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
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
using osv::premiere::PrefsHdrPeak;
using osv::premiere::test::ImporterHarness;
using osv::premiere::test::sampleClipAvailable;
using osv::premiere::test::sampleClipPath;
namespace pc = osv::premiere::pixelcopy;

namespace {

/// One 59.94 fps frame in Premiere ticks (254016000000 * 1001 / 60000).
constexpr std::int64_t kTicksPerFrame5994 = 4237833600LL;

/// A private CUDA context (the effect's), created before the harness so it
/// outlives imShutdown, which is when the engine frees its decoders in it.
struct PeakCudaContext {
    CUcontext context = nullptr;
    std::string reason;
    PeakCudaContext() {
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
    ~PeakCudaContext() {
        if (context) {
            (void)cuCtxDestroy(context);
        }
    }
    PeakCudaContext(const PeakCudaContext&) = delete;
    PeakCudaContext& operator=(const PeakCudaContext&) = delete;
};

/// The library's block for the default fit, `transfer` and `peak`: exactly
/// what every importer route builds for a 10-bit D-Log M clip.
[[nodiscard]] OsvColorParams libraryBlock(osv::color::OutputTransfer transfer, float peak) {
    return osv::color::makeColorParams(osv::color::kDefaultDlogMFit, transfer, 0.0f,
                                       osv::color::InputEncoding::DLogM, true, 10u, nullptr,
                                       osv::color::kBt2408SceneScale, osv::color::kDefaultLook, peak);
}

/// Default prefs with a colour output and an HDR peak.
[[nodiscard]] PrefsBlob prefsWith(PrefsColorOutput output, PrefsHdrPeak peak) {
    PrefsBlob p = PrefsBlob::defaults();
    p.colorOutput = static_cast<std::uint8_t>(output);
    p.hdrPeak = static_cast<std::uint8_t>(peak);
    return p;
}

/// The engine exports this file drives, resolved from the loaded module.
struct EngineApi {
    OsvEngineAcquireFrameFn acquire = nullptr;
    OsvEngineReleaseFrameFn release = nullptr;
    EngineApi() {
        const HMODULE module = GetModuleHandleW(OSV_ENGINE_MODULE_NAME);
        if (!module) {
            return;
        }
        acquire = reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
        release = reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
    }
    [[nodiscard]] bool ok() const noexcept { return acquire && release; }
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

/// The largest colour sample of a BGRA float frame (every fourth, the alpha,
/// is skipped).
[[nodiscard]] float maxColour(const std::vector<float>& bgra) {
    float m = 0.0f;
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
        m = std::max({m, bgra[i], bgra[i + 1], bgra[i + 2]});
    }
    return m;
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

/// The imAnalysis text for `clip` under `prefs` (the Properties panel).
[[nodiscard]] std::string analysisText(ImporterHarness& harness, ImporterHarness::ClipHandle& clip, PrefsBlob prefs) {
    imAnalysisRec rec{};
    rec.privatedata = clip.privateData();
    rec.prefs = &prefs;
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);  // size first
    REQUIRE(rec.buffersize > 0);
    std::vector<char> buffer(static_cast<std::size_t>(rec.buffersize), '\0');
    rec.buffer = buffer.data();
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    return std::string(buffer.data());
}

}  // namespace

// =============================================================================
//  The preference byte
// =============================================================================

TEST_CASE("the HDR peak byte defaults to 1000 nits and repairs corruption", "[importer][prefs][hdrpeak]") {
    // A fresh blob and every blob written before the byte existed (zero) read
    // as 1000 nits, which is also what makeColorParams uses when no peak is
    // named - the two defaults cannot disagree.
    PrefsBlob p = PrefsBlob::defaults();
    CHECK(p.hdrPeak == 0);
    CHECK(p.hdrPeakChoice() == PrefsHdrPeak::Nits1000);
    CHECK(p.hdrPeakNits() == 1000.0f);
    CHECK(osv::color::kDefaultHdrPeakNits == p.hdrPeakNits());
    // The persisted table and the library's list are the same four targets.
    for (std::size_t i = 0; i < std::size(osv::premiere::kPrefsHdrPeakNits); ++i) {
        CHECK(osv::premiere::kPrefsHdrPeakNits[i] == osv::color::kHdrPeakChoicesNits[i]);
    }
    // Every valid value survives sanitise().
    for (int v = 0; v < static_cast<int>(PrefsHdrPeak::Count); ++v) {
        p.hdrPeak = static_cast<std::uint8_t>(v);
        REQUIRE(p.sanitise());
        CHECK(static_cast<int>(p.hdrPeakChoice()) == v);
    }
    // A corrupt byte lands on the default; an unsanitised one already reads
    // as the default; the padding around it is zeroed.
    p.hdrPeak = 0xC3;
    CHECK(p.hdrPeakNits() == 1000.0f);
    p.padAfterHdrPeak = 0x5A;
    p.padBeforeHdrPeak[7] = 0x11;
    REQUIRE_FALSE(p.sanitise());
    CHECK(p.hdrPeakChoice() == PrefsHdrPeak::Nits1000);
    CHECK(p.padAfterHdrPeak == 0);
    CHECK(p.padBeforeHdrPeak[7] == 0);
    // And the byte sits in the range assigned to it, inside the 128 bytes.
    static_assert(offsetof(PrefsBlob, hdrPeak) == 54, "hdrPeak sits at 54");
    static_assert(sizeof(PrefsBlob) == PrefsBlob::kSize, "the blob stays 128 bytes");
}

TEST_CASE("the Source Settings dialog round-trips the HDR peak and keeps it when other fields change",
          "[importer][prefs][hdrpeak]") {
    // Every value through the dialog mapping, both directions.
    for (int v = 0; v < static_cast<int>(PrefsHdrPeak::Count); ++v) {
        const PrefsBlob in = prefsWith(PrefsColorOutput::PQ, static_cast<PrefsHdrPeak>(v));
        const DialogControls c = osv::premiere::controlsFromPrefs(in);
        CHECK(c.hdrPeak == v);
        const PrefsBlob back = osv::premiere::prefsFromControls(c, in);
        CHECK(back == in);
    }
    // An old project's blob opens the dialog on "1000 nits (default)".
    PrefsBlob old = PrefsBlob::defaults();
    old.hdrPeak = 0;
    CHECK(osv::premiere::controlsFromPrefs(old).hdrPeak == static_cast<int>(PrefsHdrPeak::Nits1000));
    // Visiting another colour output keeps the chosen peak for PQ.
    const PrefsBlob rolled = prefsWith(PrefsColorOutput::PQ, PrefsHdrPeak::Nits400);
    DialogControls c = osv::premiere::controlsFromPrefs(rolled);
    c.colorOutput = static_cast<int>(PrefsColorOutput::Rec709);
    const PrefsBlob rec709 = osv::premiere::prefsFromControls(c, rolled);
    CHECK(rec709.color() == PrefsColorOutput::Rec709);
    CHECK(rec709.hdrPeakChoice() == PrefsHdrPeak::Nits400);
    // A combo with no selection (-1) or a garbage index lands on the default.
    for (const int hostile : {-1, 4, 99}) {
        c.hdrPeak = hostile;
        CHECK(osv::premiere::prefsFromControls(c, rolled).hdrPeakChoice() == PrefsHdrPeak::Nits1000);
    }
}

// =============================================================================
//  The importer's own frames
// =============================================================================

TEST_CASE("the importer rolls PQ off into the chosen peak and leaves HLG and Rec.709 alone",
          "[importer][video][color][hdrpeak][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    // Both frame paths: the GPU one (the default when a CUDA device exists)
    // and the host one it replaces.
    const bool gpuPath = GENERATE(true, false);
    INFO((gpuPath ? "GPU frame path" : "host frame path"));
    std::unique_ptr<HostFramePathOnly> hostOnly;
    if (!gpuPath) {
        hostOnly = std::make_unique<HostFramePathOnly>();
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);
    auto clip = harness.openClip(sampleClipPath(), gpuPath ? 91 : 92);
    REQUIRE(clip.open());
    // Frame 30: the sunlit white aircraft, the sun and the glossy wing.
    constexpr std::uint32_t kFrame = 30;

    // ---- PQ: a different rendering per peak, never above it --------------------
    const auto pq1000 = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(PrefsColorOutput::PQ,
                                                                                  PrefsHdrPeak::Nits1000));
    const auto pq400 = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(PrefsColorOutput::PQ,
                                                                                 PrefsHdrPeak::Nits400));
    const auto pq203 = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(PrefsColorOutput::PQ,
                                                                                 PrefsHdrPeak::Nits203));
    const float peak1000 = osvPqEotf(maxColour(pq1000));
    const float peak400 = osvPqEotf(maxColour(pq400));
    const float peak203 = osvPqEotf(maxColour(pq203));
    INFO("brightest component: 1000 -> " << peak1000 << " nits, 400 -> " << peak400 << ", 203 -> " << peak203);
    CHECK(peak1000 > 600.0f);  // the frame has highlights to roll off
    CHECK(peak400 <= 400.0f * 1.0001f);
    CHECK(peak203 <= 203.0f * 1.0001f);
    CHECK(maxDiff(pq1000, pq400) > 0.01f);

    // ---- HLG and Rec.709: the peak does not exist there -----------------------
    for (const PrefsColorOutput output : {PrefsColorOutput::HLG, PrefsColorOutput::Rec709}) {
        const auto full = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(output, PrefsHdrPeak::Nits1000));
        const auto asked = renderFloat(harness, clip, ppix.suite, kFrame, prefsWith(output, PrefsHdrPeak::Nits203));
        INFO("output " << static_cast<int>(output));
        CHECK(std::memcmp(full.data(), asked.data(), full.size() * sizeof(float)) == 0);
    }
}

TEST_CASE("the Properties panel names the HDR peak only when it is not 1000", "[importer][hdrpeak][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath(), 93);
    REQUIRE(clip.open());
    // The default says nothing about it.
    const std::string plain = analysisText(harness, clip, prefsWith(PrefsColorOutput::PQ, PrefsHdrPeak::Nits1000));
    CHECK(plain.find("HDR peak") == std::string::npos);
    // A rolled-off PQ output names the peak and where the roll-off starts.
    const std::string rolled = analysisText(harness, clip, prefsWith(PrefsColorOutput::PQ, PrefsHdrPeak::Nits600));
    INFO(rolled);
    CHECK(rolled.find("HDR peak: 600 nits (highlights above 464 nits roll off, BT.2408 EETF)") != std::string::npos);
    // Another output with a peak set says it does not use it.
    const std::string hlg = analysisText(harness, clip, prefsWith(PrefsColorOutput::HLG, PrefsHdrPeak::Nits400));
    CHECK(hlg.find("HDR peak: 400 nits (PQ output only; this output does not use it)") != std::string::npos);
}

// =============================================================================
//  The engine / direct path
// =============================================================================

TEST_CASE("the engine follows an HDR peak change through the Source Settings generation",
          "[importer][engine][settings][color][hdrpeak][cuda][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    PeakCudaContext cuda;  // before the harness: outlives imShutdown
    if (!cuda.context) {
        SKIP("CUDA unavailable: " << cuda.reason);
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const EngineApi api;
    REQUIRE(api.ok());
    const std::wstring path = sampleClipPath().wstring();
    std::uint32_t frameIndex = 7;
    imFileInfoRec8 info{};

    // Each change arrives the way Premiere delivers one: a NEWER importer
    // instance of the clip is handed the changed blob and publishes it.
    csSDK_int32 importerId = 95;
    std::vector<ImporterHarness::ClipHandle> instances;
    const auto publish = [&](const PrefsBlob& prefs) {
        instances.push_back(harness.openClip(sampleClipPath(), importerId++));
        REQUIRE(instances.back().open());
        PrefsBlob copy = prefs;
        REQUIRE(harness.getInfo8(instances.back(), info, &copy) == imNoErr);
    };

    // ---- PQ at 1000 nits: no roll-off, today's block ----------------------------
    publish(prefsWith(PrefsColorOutput::PQ, PrefsHdrPeak::Nits1000));
    const EngineColour full = engineColour(api, path, cuda.context, frameIndex++, OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(full.generation != 0u);
    CHECK(full.color.transfer == OSV_TRANSFER_PQ);
    CHECK(full.color.hdrPeakNits == 0.0f);
    const OsvColorParams expectedFull = libraryBlock(osv::color::OutputTransfer::PQ, 1000.0f);
    CHECK(std::memcmp(&full.color, &expectedFull, sizeof(OsvColorParams)) == 0);

    // ---- the user picks 400 nits: a new generation, a new block -----------------
    publish(prefsWith(PrefsColorOutput::PQ, PrefsHdrPeak::Nits400));
    const EngineColour rolled = engineColour(api, path, cuda.context, frameIndex++, OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(rolled.generation > full.generation);
    CHECK(rolled.color.hdrPeakNits == 400.0f);
    const OsvColorParams expectedRolled = libraryBlock(osv::color::OutputTransfer::PQ, 400.0f);
    CHECK(std::memcmp(&rolled.color, &expectedRolled, sizeof(OsvColorParams)) == 0);
    // The PQ working-space override carries the same peak; a Rec.709 working
    // space is untouched by it.
    CHECK(engineColour(api, path, cuda.context, frameIndex++, OSV_TRANSFER_PQ).color.hdrPeakNits == 400.0f);
    const OsvColorParams rec709 = engineColour(api, path, cuda.context, frameIndex++, OSV_TRANSFER_REC709).color;
    const OsvColorParams expected709 = libraryBlock(osv::color::OutputTransfer::Rec709, 1000.0f);
    CHECK(rec709.hdrPeakNits == 0.0f);
    CHECK(std::memcmp(&rec709, &expected709, sizeof(OsvColorParams)) == 0);

    // ---- a Rec.709 clip in a PQ sequence rolls off into its own peak -------------
    publish(prefsWith(PrefsColorOutput::Rec709, PrefsHdrPeak::Nits600));
    const EngineColour viaPq = engineColour(api, path, cuda.context, frameIndex++, OSV_TRANSFER_PQ);
    CHECK(viaPq.generation > rolled.generation);
    CHECK(viaPq.color.transfer == OSV_TRANSFER_PQ);
    CHECK(viaPq.color.hdrPeakNits == 600.0f);

    // ---- and back to 1000: yet another generation, today's block again ----------
    publish(prefsWith(PrefsColorOutput::PQ, PrefsHdrPeak::Nits1000));
    const EngineColour back = engineColour(api, path, cuda.context, frameIndex++, OSV_ENGINE_TRANSFER_FROM_CLIP);
    CHECK(back.generation > viaPq.generation);
    CHECK(std::memcmp(&back.color, &full.color, sizeof(OsvColorParams)) == 0);
}
