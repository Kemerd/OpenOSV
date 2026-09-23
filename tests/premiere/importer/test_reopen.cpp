// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The importer's reader across imQuietFile / unquiet and across instances.
//
// Premiere quiets a clip it is not reading and wakes it seconds later, and it
// opens a SECOND instance of a clip on every Source Settings change.  The
// importer parks its reader in a process-wide pool on release and the next
// open of the same file takes it back warm (see osv/video/ReaderPool.h).
// These tests drive the built .prm through those exact selector sequences and
// check two things: that the warm path is really taken (the importer says so
// in its log, which is the only window into a module this process cannot
// link against), and that the pixels are exactly the pixels a cold reader
// produces.
//
// The plug-in log is switched to INFO for these tests only (the suite runs
// at ERROR otherwise); TestMain.cpp has already pointed it at a private
// per-process directory, so nothing reaches the user's real log.

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"

#include "MockHost.h"

#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "PrefsBlob.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace osv::premiere;
using namespace osv::premiere::test;
using osv::premiere::mock::MockHost;

namespace {

/// One 59.94 fps frame in Premiere ticks (254016000000 * 1001 / 60000).
constexpr PrTime kTicksPerFrame = 4237833600LL;

/// SKIP the test when the sample clip is not present.
#define REOPEN_REQUIRE_SAMPLE_CLIP()                                                                   \
    do {                                                                                                \
        if (!sampleClipAvailable()) {                                                                   \
            SKIP("the sample clip is not present at " << sampleClipPath().string());                    \
        }                                                                                               \
    } while (false)

/// Raise the plug-in log to INFO for the lifetime of the object.  Must be
/// constructed BEFORE the harness: the module reads the level once, when it
/// initialises its log.  _putenv_s updates the CRT copy and the OS block, so
/// the module sees it through either API.
class InfoLogLevel {
public:
    InfoLogLevel() {
        char* old = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&old, &length, "OSV_PLUGIN_LOG_LEVEL") == 0 && old) {
            m_previous = old;
            m_hadPrevious = true;
        }
        std::free(old);
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "info");
    }
    ~InfoLogLevel() { ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", m_hadPrevious ? m_previous.c_str() : ""); }
    InfoLogLevel(const InfoLogLevel&) = delete;
    InfoLogLevel& operator=(const InfoLogLevel&) = delete;

private:
    std::string m_previous;
    bool m_hadPrevious = false;
};

/// Pin the importer's frames to the HOST path for the lifetime of the object.
///
/// Everything in this file is about the host decode path's reader
/// (ensureReader, ReaderPool).  With a CUDA device the importer renders its
/// frames on the GPU path instead - NVDEC into VRAM, no DualStreamReader at
/// all (docs/PREMIERE.md, "The importer's own frame") - so these tests use
/// the importer's documented switch, OPENOSV_IMPORTER_NO_GPU_DECODE, to keep
/// exercising the path they describe.  Each clip reads it at its first
/// frame, so the guard must exist before the first render.
class HostFramePath {
public:
    HostFramePath() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", "1"); }
    ~HostFramePath() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", ""); }
    HostFramePath(const HostFramePath&) = delete;
    HostFramePath& operator=(const HostFramePath&) = delete;
};

/// The importer's log file for this process (LOCALAPPDATA was redirected by
/// isolatePluginLogs() in TestMain).
[[nodiscard]] std::filesystem::path importerLogPath() {
    wchar_t buffer[32768] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, n)) / L"OpenOSV" / L"OpenOSVImporter.log";
}

/// Whole text of the importer log (empty when there is none yet).  The
/// plug-in keeps the file open with a deny-WRITE share, which a reader is
/// allowed through, and flushes every line.
[[nodiscard]] std::string importerLog() {
    const std::filesystem::path path = importerLogPath();
    if (path.empty()) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Occurrences of `needle` in `hay`.
[[nodiscard]] std::size_t countOf(const std::string& hay, const std::string& needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

/// The two lines ensureReader writes: a new reader, or one from the pool.
constexpr const char* kWarmLine = "warm reader from the pool";
constexpr const char* kColdLine = "(opened in ";

/// Readers handed to instances so far, warm or new.
[[nodiscard]] std::size_t readerLines(const std::string& log) {
    return countOf(log, kWarmLine) + countOf(log, kColdLine);
}

/// Prefs that keep the render cheap and deterministic: a small output, and
/// no analysis whose result could depend on what was measured before.
[[nodiscard]] PrefsBlob cheapPrefs() {
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;
    prefs.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    return prefs;
}

/// The PPix suite, acquired for the lifetime of the object.
class PPixSuite {
public:
    explicit PPixSuite(MockHost& host) : m_host(host) {
        const void* p = nullptr;
        if (host.basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &p) == kSPNoError) {
            m_suite = static_cast<const PrSDKPPixSuite*>(p);
        }
    }
    ~PPixSuite() {
        if (m_suite) {
            m_host.basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        }
    }
    PPixSuite(const PPixSuite&) = delete;
    PPixSuite& operator=(const PPixSuite&) = delete;
    [[nodiscard]] const PrSDKPPixSuite* get() const noexcept { return m_suite; }

private:
    MockHost& m_host;
    const PrSDKPPixSuite* m_suite = nullptr;
};

/// Render frame `index` of `clip` at 1920x960 8u with the host cache
/// emptied first (so the importer really renders), and return its visible
/// bytes row by row.
[[nodiscard]] std::vector<std::uint8_t> renderBytes(ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                                    const PrSDKPPixSuite* ppix, std::uint32_t index) {
    REQUIRE(ppix != nullptr);
    harness.host().clearCache();
    ImporterHarness::SourceVideoRequest request;
    request.frameTime = kTicksPerFrame * static_cast<PrTime>(index);
    request.format = PrPixelFormat_BGRA_4444_8u;
    request.width = 1920;
    request.height = 960;
    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, cheapPrefs(), hand) == imNoErr);
    REQUIRE(hand != nullptr);

    const auto info = harness.host().inspect(hand);
    REQUIRE(info.has_value());
    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) == suiteError_NoError);
    REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
    REQUIRE(pixels != nullptr);
    const std::size_t visible = static_cast<std::size_t>(info->width) * 4u;
    const std::size_t stride = static_cast<std::size_t>(rowBytes < 0 ? -rowBytes : rowBytes);
    REQUIRE(stride >= visible);
    std::vector<std::uint8_t> out(visible * info->height);
    for (std::uint32_t y = 0; y < info->height; ++y) {
        const char* row = pixels + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(y);
        std::memcpy(out.data() + visible * y, row, visible);
    }
    ppix->Dispose(hand);
    return out;
}

}  // namespace

// =============================================================================
//  Quiet / unquiet
// =============================================================================
TEST_CASE("an unquiet takes the reader back warm and renders the same pixels", "[importer][reopen][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    InfoLogLevel info;
    HostFramePath hostPath;
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());

    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 fileInfo{};
    REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);

    // The first frame gets a reader: a new one, or - when an earlier test in
    // this process left one parked - a warm one.  Either way exactly one.
    const std::size_t readersBefore = readerLines(importerLog());
    const std::vector<std::uint8_t> before = renderBytes(harness, clip, ppix.get(), 17);
    REQUIRE(readerLines(importerLog()) == readersBefore + 1);
    const std::size_t warmBefore = countOf(importerLog(), kWarmLine);
    const std::size_t coldBefore = countOf(importerLog(), kColdLine);

    // Quiet, then the same frame again: served by the parked reader.
    REQUIRE(clip.quiet() == imNoErr);
    const std::vector<std::uint8_t> after = renderBytes(harness, clip, ppix.get(), 17);
    const std::string log = importerLog();
    INFO("importer log: " << importerLogPath().string());
    REQUIRE(countOf(log, kWarmLine) == warmBefore + 1);
    REQUIRE(countOf(log, kColdLine) == coldBefore);  // no second open
    REQUIRE(after == before);

    // And it keeps decoding correctly elsewhere in the clip after the round trip.
    REQUIRE(clip.quiet() == imNoErr);
    const std::vector<std::uint8_t> otherGop = renderBytes(harness, clip, ppix.get(), 62);
    REQUIRE(countOf(importerLog(), kWarmLine) == warmBefore + 2);
    REQUIRE(otherGop != before);
}

// =============================================================================
//  A second instance of the same clip (the Source Settings case)
// =============================================================================
TEST_CASE("a new instance of a clip takes the reader a closed instance released", "[importer][reopen][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    InfoLogLevel info;
    HostFramePath hostPath;
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());

    std::vector<std::uint8_t> first;
    {
        auto clip = harness.openClip(sampleClipPath(), 7);
        REQUIRE(clip.open());
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
        first = renderBytes(harness, clip, ppix.get(), 30);
        REQUIRE(clip.close() == imNoErr);
    }
    const std::size_t warmBefore = countOf(importerLog(), kWarmLine);

    auto again = harness.openClip(sampleClipPath(), 8);
    REQUIRE(again.open());
    imFileInfoRec8 fileInfo{};
    REQUIRE(harness.getInfo8(again, fileInfo) == imNoErr);
    const std::vector<std::uint8_t> second = renderBytes(harness, again, ppix.get(), 30);
    REQUIRE(countOf(importerLog(), kWarmLine) == warmBefore + 1);
    REQUIRE(second == first);
}

TEST_CASE("two live instances of one clip each decode with their own reader", "[importer][reopen][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    InfoLogLevel info;
    HostFramePath hostPath;
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());

    auto a = harness.openClip(sampleClipPath(), 11);
    auto b = harness.openClip(sampleClipPath(), 12);
    REQUIRE(a.open());
    REQUIRE(b.open());
    imFileInfoRec8 infoA{};
    imFileInfoRec8 infoB{};
    REQUIRE(harness.getInfo8(a, infoA) == imNoErr);
    REQUIRE(harness.getInfo8(b, infoB) == imNoErr);

    // Both render, each with a reader of its own (new, or parked by an
    // earlier test in this process - never the same one twice).
    const std::size_t readersBefore = readerLines(importerLog());
    const std::vector<std::uint8_t> a20 = renderBytes(harness, a, ppix.get(), 20);
    const std::vector<std::uint8_t> b20 = renderBytes(harness, b, ppix.get(), 20);
    REQUIRE(readerLines(importerLog()) == readersBefore + 2);
    REQUIRE(a20 == b20);

    // Interleaved requests at different places do not disturb each other.
    const std::vector<std::uint8_t> a45 = renderBytes(harness, a, ppix.get(), 45);
    const std::vector<std::uint8_t> b5 = renderBytes(harness, b, ppix.get(), 5);
    const std::vector<std::uint8_t> a5 = renderBytes(harness, a, ppix.get(), 5);
    const std::vector<std::uint8_t> b45 = renderBytes(harness, b, ppix.get(), 45);
    REQUIRE(a45 == b45);
    REQUIRE(a5 == b5);
    REQUIRE(a45 != a5);

    // One closes (its reader goes to the pool) while the other keeps going.
    REQUIRE(a.close() == imNoErr);
    const std::vector<std::uint8_t> b50 = renderBytes(harness, b, ppix.get(), 50);
    REQUIRE_FALSE(b50.empty());

    // The survivor quiets and wakes: it takes a parked reader - its own or
    // the one A left, both exact matches for the file, so either is correct.
    // What matters is that no reader is ever in two places, which the pixels
    // prove: the frame is right.
    REQUIRE(b.quiet() == imNoErr);
    const std::vector<std::uint8_t> b20again = renderBytes(harness, b, ppix.get(), 20);
    REQUIRE(b20again == a20);
}

TEST_CASE("imShutdown with a parked reader neither hangs nor crashes", "[importer][reopen][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    HostFramePath hostPath;  // the host path is the one that parks a reader
    {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        PPixSuite ppix(harness.host());
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
        (void)renderBytes(harness, clip, ppix.get(), 3);
        // Quiet parks the reader; then the clip closes and the harness sends
        // imShutdown and unloads the module with the reader still parked.
        REQUIRE(clip.quiet() == imNoErr);
    }
    // A fresh load in the same process still works end to end.
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 fileInfo{};
    REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
    REQUIRE_FALSE(renderBytes(harness, clip, ppix.get(), 3).empty());
}

// =============================================================================
//  The GPU frame path: NVDEC decoders parked in GpuDecoderPool
// =============================================================================
//
// With a CUDA device the importer renders its own frame on the GPU path
// (NVDEC into VRAM, stitched in place) and never builds a DualStreamReader.
// Its decoder is parked in video::GpuDecoderPool by releaseHeavy() and taken
// back by the next open of the same file.  The importer says which happened
// in its log, which is how these tests tell a warm decoder from a new one.

namespace {

/// Make sure the GPU frame path is allowed for the lifetime of the object
/// (clears the switch the host-path tests above set and restore).
class GpuFramePath {
public:
    GpuFramePath() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", ""); }
    ~GpuFramePath() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", ""); }
    GpuFramePath(const GpuFramePath&) = delete;
    GpuFramePath& operator=(const GpuFramePath&) = delete;
};

/// The two lines the GPU frame path writes when it gets its decoder: a new
/// one, or a warm one from the pool.
constexpr const char* kGpuColdLine = "importer frames now decode on NVDEC into VRAM";
constexpr const char* kGpuWarmLine = "warm decoder from the pool";

/// GPU-path decoders handed to instances so far, warm or new.
[[nodiscard]] std::size_t gpuDecoderLines(const std::string& log) {
    return countOf(log, kGpuColdLine) + countOf(log, kGpuWarmLine);
}

/// Render the first frame of a clip and SKIP when it did not take the GPU
/// path (no CUDA device, no NVDEC): nothing below applies then.
[[nodiscard]] std::vector<std::uint8_t> firstGpuFrame(ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                                      const PrSDKPPixSuite* ppix, std::uint32_t index) {
    const std::size_t before = gpuDecoderLines(importerLog());
    std::vector<std::uint8_t> bytes = renderBytes(harness, clip, ppix, index);
    if (gpuDecoderLines(importerLog()) != before + 1) {
        SKIP("the importer frame did not take the GPU path on this machine (no CUDA renderer or NVDEC)");
    }
    return bytes;
}

}  // namespace

TEST_CASE("an unquiet on the GPU frame path takes the NVDEC decoder back warm with the same pixels",
          "[importer][reopen][gpu][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    InfoLogLevel info;
    GpuFramePath gpuPath;
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());

    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 fileInfo{};
    REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);

    const std::vector<std::uint8_t> before = firstGpuFrame(harness, clip, ppix.get(), 17);
    const std::size_t warmBefore = countOf(importerLog(), kGpuWarmLine);
    const std::size_t coldBefore = countOf(importerLog(), kGpuColdLine);

    // Quiet (the decoder is parked), then the same frame: the warm decoder
    // serves it - from its kept frames - and the pixels are the same.
    REQUIRE(clip.quiet() == imNoErr);
    const std::vector<std::uint8_t> after = renderBytes(harness, clip, ppix.get(), 17);
    INFO("importer log: " << importerLogPath().string());
    REQUIRE(countOf(importerLog(), kGpuWarmLine) == warmBefore + 1);
    REQUIRE(countOf(importerLog(), kGpuColdLine) == coldBefore);  // no second open
    REQUIRE(after == before);

    // Round trip again, then a frame in the other GOP (the warm decoder has
    // to restart there) - still right, still no new decoder.
    REQUIRE(clip.quiet() == imNoErr);
    const std::vector<std::uint8_t> otherGop = renderBytes(harness, clip, ppix.get(), 62);
    REQUIRE(countOf(importerLog(), kGpuWarmLine) == warmBefore + 2);
    REQUIRE(countOf(importerLog(), kGpuColdLine) == coldBefore);
    REQUIRE(otherGop != before);
    // And back: frame 17 through a decoder that has been parked twice.
    REQUIRE(renderBytes(harness, clip, ppix.get(), 17) == before);
}

TEST_CASE("a new instance on the GPU frame path takes the decoder a closed instance parked",
          "[importer][reopen][gpu][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    InfoLogLevel info;
    GpuFramePath gpuPath;
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());

    std::vector<std::uint8_t> first;
    {
        auto clip = harness.openClip(sampleClipPath(), 21);
        REQUIRE(clip.open());
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
        first = firstGpuFrame(harness, clip, ppix.get(), 30);
        REQUIRE(clip.close() == imNoErr);  // what Premiere does to the old instance
    }
    const std::size_t warmBefore = countOf(importerLog(), kGpuWarmLine);
    const std::size_t coldBefore = countOf(importerLog(), kGpuColdLine);

    auto again = harness.openClip(sampleClipPath(), 22);
    REQUIRE(again.open());
    imFileInfoRec8 fileInfo{};
    REQUIRE(harness.getInfo8(again, fileInfo) == imNoErr);
    const std::vector<std::uint8_t> second = renderBytes(harness, again, ppix.get(), 30);
    REQUIRE(countOf(importerLog(), kGpuWarmLine) == warmBefore + 1);
    REQUIRE(countOf(importerLog(), kGpuColdLine) == coldBefore);
    REQUIRE(second == first);
}

TEST_CASE("two live instances on the GPU frame path decode with their own decoders, interleaved",
          "[importer][reopen][gpu][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    InfoLogLevel info;
    GpuFramePath gpuPath;
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());

    auto a = harness.openClip(sampleClipPath(), 31);
    auto b = harness.openClip(sampleClipPath(), 32);
    REQUIRE(a.open());
    REQUIRE(b.open());
    imFileInfoRec8 infoA{};
    imFileInfoRec8 infoB{};
    REQUIRE(harness.getInfo8(a, infoA) == imNoErr);
    REQUIRE(harness.getInfo8(b, infoB) == imNoErr);

    // Each gets a decoder of its own (new, or one an earlier test parked -
    // never the same one twice: the pool hands each out once).
    const std::size_t before = gpuDecoderLines(importerLog());
    const std::vector<std::uint8_t> a20 = firstGpuFrame(harness, a, ppix.get(), 20);
    const std::vector<std::uint8_t> b20 = renderBytes(harness, b, ppix.get(), 20);
    REQUIRE(gpuDecoderLines(importerLog()) == before + 2);
    REQUIRE(a20 == b20);

    // Interleaved requests at different places do not disturb each other.
    const std::vector<std::uint8_t> a45 = renderBytes(harness, a, ppix.get(), 45);
    const std::vector<std::uint8_t> b5 = renderBytes(harness, b, ppix.get(), 5);
    const std::vector<std::uint8_t> a5 = renderBytes(harness, a, ppix.get(), 5);
    const std::vector<std::uint8_t> b45 = renderBytes(harness, b, ppix.get(), 45);
    REQUIRE(a45 == b45);
    REQUIRE(a5 == b5);
    REQUIRE(a45 != a5);

    // A closes (its decoder is parked) while B keeps rendering; then B
    // quiets and wakes and takes a parked decoder - A's or its own, both
    // exact matches.  The pixels prove no decoder is ever in two places.
    const std::size_t warmBefore = countOf(importerLog(), kGpuWarmLine);
    REQUIRE(a.close() == imNoErr);
    REQUIRE(renderBytes(harness, b, ppix.get(), 50) != a45);
    REQUIRE(b.quiet() == imNoErr);
    REQUIRE(renderBytes(harness, b, ppix.get(), 20) == a20);
    REQUIRE(countOf(importerLog(), kGpuWarmLine) == warmBefore + 1);
    REQUIRE(renderBytes(harness, b, ppix.get(), 45) == a45);
}

TEST_CASE("imShutdown with a parked NVDEC decoder neither hangs nor crashes", "[importer][reopen][gpu][sample]") {
    REOPEN_REQUIRE_SAMPLE_CLIP();
    GpuFramePath gpuPath;
    {
        InfoLogLevel info;
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        PPixSuite ppix(harness.host());
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
        (void)firstGpuFrame(harness, clip, ppix.get(), 3);
        // Quiet parks the decoder in the primary context; then the clip
        // closes and the harness sends imShutdown - which must clear the GPU
        // pool before the renderer pool goes - and unloads the module.
        REQUIRE(clip.quiet() == imNoErr);
    }
    // A fresh load in the same process still decodes on the GPU end to end.
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness.host());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 fileInfo{};
    REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
    REQUIRE_FALSE(renderBytes(harness, clip, ppix.get(), 3).empty());
}
