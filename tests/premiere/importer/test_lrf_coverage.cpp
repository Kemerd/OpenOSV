// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_lrf_coverage.cpp - an .LRF delivers EVERY row, at the sizes a host
// really asks for.
//
// A field report showed an .LRF frame whose bottom three eighths were black.
// Nothing in the stitch can cut a flat, full-width band out of an upright
// camera's picture, and every frame path either writes every row of the PPix
// or fails the frame - but the importer was only ever tested at frame 0, at
// the size it advertises, with stabilisation off.  A host asks otherwise: a
// 2000 x 1000 proxy request on an .LRF that advertises 2048 x 1024, the
// original's 6000 x 3000 on its proxy, a 16:9 sequence size, 8-bit frames
// while playing, half-ratio drafts while scrubbing - all with the clip's
// default Source Settings (Smooth + Horizon Lock, parallax, the photometric
// seam and lens shading all on).  This file asks exactly that, for the
// sample .LRF as the proxy of its .OSV and on its own, over several frames
// and intents, and demands of every delivered frame:
//
//   * the size the importer advertises nearest to the request;
//   * every row opaque (alpha >= 0.5) over at least 95 % of its columns -
//     ALL columns, read independently of the importer's own self-check;
//   * no row whose B, G and R are zero in every column;
//   * the importer's self-check (FrameRowCheck.h, run at Debug level on
//     every frame) agreeing, and its log saying so: one row-check line per
//     rendered frame, no "came out" warning, and the requested-vs-delivered
//     size on record for every size the host was not given as asked.

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"

#include "MockHost.h"

#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "FrameRowCheck.h"
#include "PixelCopy.h"
#include "PrefsBlob.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

namespace {

/// Raise the plug-in log to DEBUG for the lifetime of the object, so the
/// importer self-checks EVERY rendered frame (not only a clip's first few)
/// and writes a summary line for each.  Must exist BEFORE the harness: the
/// module reads the level once, when its log initialises.
class DebugLogLevel {
public:
    DebugLogLevel() {
        char* old = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&old, &length, "OSV_PLUGIN_LOG_LEVEL") == 0 && old) {
            m_previous = old;
            m_hadPrevious = true;
        }
        std::free(old);
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "debug");
    }
    ~DebugLogLevel() { ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", m_hadPrevious ? m_previous.c_str() : ""); }
    DebugLogLevel(const DebugLogLevel&) = delete;
    DebugLogLevel& operator=(const DebugLogLevel&) = delete;

private:
    std::string m_previous;
    bool m_hadPrevious = false;
};

/// The importer's log file for this process (LOCALAPPDATA was redirected by
/// isolatePluginLogs() in TestMain, so this is never the user's own log).
[[nodiscard]] std::filesystem::path importerLogPath() {
    wchar_t buffer[32768] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer, n)) / L"OpenOSV" / L"OpenOSVImporter.log";
}

/// Bytes of `path` from `offset` on (empty when the file is missing or
/// shorter).  The plug-in keeps the file open with a deny-WRITE share, which
/// a reader is allowed through, and flushes every line.
[[nodiscard]] std::string readFrom(const std::filesystem::path& path, std::uintmax_t offset) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return offset < text.size() ? text.substr(static_cast<std::size_t>(offset)) : std::string();
}

/// Everything the importer logged since the log was `offset` bytes long,
/// across one rotation (the log rotates at 4 MB into <file>.1).
[[nodiscard]] std::string logSince(std::uintmax_t offset) {
    const std::filesystem::path path = importerLogPath();
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0u;
    if (size >= offset) {
        return readFrom(path, offset);
    }
    // Rotated: the tail of the previous generation, then the whole new file.
    std::filesystem::path previous = path;
    previous += L".1";
    return readFrom(previous, offset) + readFrom(path, 0);
}

/// Current length of the importer's log (0 when there is none yet).
[[nodiscard]] std::uintmax_t logSize() {
    std::error_code ec;
    const std::filesystem::path path = importerLogPath();
    return std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0u;
}

/// Occurrences of `needle` in `hay`.
[[nodiscard]] std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

/// One host-style request: a size (0 = any), a format, an intent.
struct HostRequest {
    std::int32_t width = 0;
    std::int32_t height = 0;
    PrPixelFormat format = PrPixelFormat_BGRA_4444_32f;
    imRenderIntent intent = imRenderIntent_Export;
    double playbackRatio = 1.0;
    const char* label = "";
};

/// The size the importer must deliver for a request on a clip that
/// advertises `baseW` x `baseH`: the nearest by height of native, half and
/// quarter (2:1 each), native for "any" - the documented rule, restated here
/// as the test's own oracle.
void expectedSize(std::int32_t baseW, std::int32_t baseH, const HostRequest& request, std::int32_t& outW,
                  std::int32_t& outH) {
    outW = baseW;
    outH = baseH;
    if (request.width <= 0 && request.height <= 0) {
        return;
    }
    const std::int64_t target = request.height > 0 ? request.height : request.width / 2;
    std::int64_t best = std::llabs(static_cast<std::int64_t>(baseH) - target);
    for (std::int32_t divisor : {2, 4}) {
        const std::int32_t h = baseH / divisor;
        if (h < 16) {
            break;
        }
        const std::int64_t d = std::llabs(static_cast<std::int64_t>(h) - target);
        if (d < best) {
            best = d;
            outH = h;
            outW = h * 2;
        }
    }
}

/// The host layout of a PrPixelFormat the importer delivers.
[[nodiscard]] bool layoutOf(PrPixelFormat format, pixelcopy::HostPixelFormat& out) {
    switch (format) {
    case PrPixelFormat_BGRA_4444_32f: out = pixelcopy::HostPixelFormat::Bgra32f; return true;
    case PrPixelFormat_BGRA_4444_16u: out = pixelcopy::HostPixelFormat::Bgra16u; return true;
    case PrPixelFormat_BGRA_4444_8u:  out = pixelcopy::HostPixelFormat::Bgra8u;  return true;
    default:                          return false;
    }
}

/// Alpha and "is B, G, R all zero" of pixel x of a host row in `layout`.
void readPixel(const char* row, std::uint32_t x, pixelcopy::HostPixelFormat layout, float& alpha, bool& black) {
    switch (layout) {
    case pixelcopy::HostPixelFormat::Bgra32f: {
        float p[4] = {};
        std::memcpy(p, row + static_cast<std::size_t>(x) * 16u, sizeof(p));
        alpha = std::isfinite(p[3]) ? p[3] : 0.0f;
        black = p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f;
        return;
    }
    case pixelcopy::HostPixelFormat::Bgra16u: {
        std::uint16_t p[4] = {};
        std::memcpy(p, row + static_cast<std::size_t>(x) * 8u, sizeof(p));
        alpha = pixelcopy::u16ToFloat(p[3]);
        black = p[0] == 0 && p[1] == 0 && p[2] == 0;
        return;
    }
    case pixelcopy::HostPixelFormat::Bgra8u:
    default: {
        const auto* p = reinterpret_cast<const std::uint8_t*>(row) + static_cast<std::size_t>(x) * 4u;
        alpha = pixelcopy::u8ToFloat(p[3]);
        black = p[0] == 0 && p[1] == 0 && p[2] == 0;
        return;
    }
    }
}

/// Request one frame and hold the delivered PPix to the rules in the file
/// comment.  `rendered` counts the frames the importer actually rendered
/// (the host cache is cleared first, so that is every successful request).
void requestAndCheck(ImporterHarness& harness, ImporterHarness::ClipHandle& clip, const imFileInfoRec8& info,
                     const PrSDKPPixSuite* ppix, std::int64_t frame, const HostRequest& request,
                     const PrefsBlob& prefs, std::size_t& rendered) {
    INFO("frame " << frame << ", " << request.label << ": requested " << request.width << "x" << request.height);
    // ---- render, never from the cache --------------------------------------
    harness.host().clearCache();
    ImporterHarness::SourceVideoRequest source;
    source.frameTime = static_cast<PrTime>(frame) * info.vidInfo.frameRate;
    source.format = request.format;
    source.width = request.width;
    source.height = request.height;
    source.intent = request.intent;
    source.playbackRatio = request.playbackRatio;
    PPixHand hand = nullptr;
    const csSDK_int32 result = harness.getSourceVideo(clip, source, prefs, hand);
    INFO("imGetSourceVideo returned " << result);
    REQUIRE(result == imNoErr);
    REQUIRE(hand != nullptr);
    ++rendered;

    // ---- the size: the advertised one nearest to the request -------------------
    const auto pinfo = harness.host().inspect(hand);
    REQUIRE(pinfo.has_value());
    std::int32_t wantW = 0;
    std::int32_t wantH = 0;
    expectedSize(info.vidInfo.imageWidth, info.vidInfo.imageHeight, request, wantW, wantH);
    CHECK(pinfo->width == static_cast<std::uint32_t>(wantW));
    CHECK(pinfo->height == static_cast<std::uint32_t>(wantH));
    pixelcopy::HostPixelFormat layout{};
    REQUIRE(layoutOf(pinfo->format, layout));

    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) == suiteError_NoError);
    REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
    REQUIRE(pixels != nullptr);
    REQUIRE(rowBytes != 0);

    // ---- every row, every column -------------------------------------------------
    // Independent of the importer's sampled self-check: the worst row's
    // opaque share and the number of all-black rows over EVERY pixel.
    double worstOpaque = 1.0;
    std::uint32_t worstRow = 0;
    std::uint32_t blackRows = 0;
    std::uint32_t firstBlackRow = 0;
    for (std::uint32_t y = 0; y < pinfo->height; ++y) {
        // Picture row y is host row (height - 1 - y): host row 0 is the bottom.
        const char* row = pixelcopy::rowAddress(pixels, rowBytes, pinfo->height - 1u - y);
        std::uint32_t opaque = 0;
        bool allBlack = true;
        for (std::uint32_t x = 0; x < pinfo->width; ++x) {
            float alpha = 0.0f;
            bool black = false;
            readPixel(row, x, layout, alpha, black);
            opaque += alpha >= 0.5f ? 1u : 0u;
            allBlack = allBlack && black;
        }
        const double share = static_cast<double>(opaque) / static_cast<double>(pinfo->width);
        if (share < worstOpaque) {
            worstOpaque = share;
            worstRow = y;
        }
        if (allBlack) {
            if (blackRows == 0) {
                firstBlackRow = y;
            }
            ++blackRows;
        }
    }
    INFO("worst row " << worstRow << " of " << pinfo->height << " is opaque over " << 100.0 * worstOpaque
                      << "% of its columns; " << blackRows << " all-black rows (first " << firstBlackRow << ")");
    CHECK(worstOpaque >= 0.95);
    CHECK(blackRows == 0u);

    // ---- the importer's own self-check agrees -------------------------------------
    const rowcheck::RowScan scan =
        rowcheck::scanHostFrame(pixelcopy::ConstHostFrame(pixels, rowBytes, pinfo->width, pinfo->height), layout);
    REQUIRE(scan.scanned);
    INFO("self-check: " << rowcheck::runKindName(scan) << ", " << scan.badRows << " bad rows");
    CHECK_FALSE(scan.defective());
    CHECK(scan.worstRowOpaqueFraction() >= 0.95);

    if (ppix->Dispose) {
        ppix->Dispose(hand);
    }
}

/// The requests a host makes of a clip that advertises `baseW` x `baseH`:
/// the advertised size, the sizes the diagnosis named, a 16:9 sequence size,
/// 8-bit playback, a half-ratio scrub draft and an any-size thumbnail.
[[nodiscard]] std::vector<HostRequest> hostRequests(bool proxy) {
    std::vector<HostRequest> requests;
    if (proxy) {
        // The proxy of a 6K original advertises 2000 x 1000.
        requests.push_back({2000, 1000, PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export, 1.0, "as advertised"});
        requests.push_back({6000, 3000, PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export, 1.0, "the original's size"});
    } else {
        // On its own it advertises 2048 x 1024; 2000 x 1000 is the proxy size
        // a host may still ask for, 1920 x 960 Output Size's smallest.
        requests.push_back({2000, 1000, PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export, 1.0, "the proxy's size"});
        requests.push_back({1920, 960, PrPixelFormat_BGRA_4444_16u, imRenderIntent_Stopped, 1.0, "1920 x 960, 16u"});
    }
    requests.push_back({1920, 1080, PrPixelFormat_BGRA_4444_8u, imRenderIntent_Playing, 1.0, "16:9 sequence, 8u"});
    requests.push_back({960, 480, PrPixelFormat_BGRA_4444_16u, imRenderIntent_Scrubbing, 0.5, "scrub draft"});
    requests.push_back({0, 0, PrPixelFormat_Any, imRenderIntent_Thumbnail, 1.0, "thumbnail, any size"});
    return requests;
}

/// Open `path`, then request every frame in `frames` at every host request,
/// with the clip's default Source Settings.  Returns the log written meanwhile.
std::string runClip(const std::filesystem::path& path, bool proxy, const std::vector<std::int64_t>& frames,
                    std::size_t& rendered, std::int32_t& advertisedW, std::int32_t& advertisedH) {
    const std::uintmax_t logStart = logSize();
    {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(path);
        INFO("open result " << clip.openResult());
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info) == imNoErr);
        advertisedW = info.vidInfo.imageWidth;
        advertisedH = info.vidInfo.imageHeight;
        REQUIRE(advertisedW == 2 * advertisedH);
        REQUIRE(info.vidInfo.frameRate > 0);

        const void* suite = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) ==
                kSPNoError);
        const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

        // The defaults a new clip gets: Smooth + Horizon Lock, parallax, the
        // photometric seam, lens shading, PQ - every stage that could, in
        // principle, touch which rows are covered.
        const PrefsBlob prefs = PrefsBlob::defaults();
        const std::vector<HostRequest> requests = hostRequests(proxy);
        for (const std::int64_t frame : frames) {
            REQUIRE(frame < info.vidDurationInFrames);
            for (const HostRequest& request : requests) {
                requestAndCheck(harness, clip, info, ppix, frame, request, prefs, rendered);
            }
        }
        harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
    }
    return logSince(logStart);
}

/// The log of a clip run holds what the diagnostics promise.
void checkLog(const std::string& log, std::size_t rendered) {
    INFO("importer log of the run:\n" << log.substr(0, std::min<std::size_t>(log.size(), 6000u)));
    // Every rendered frame was self-checked (Debug level: every frame) ...
    CHECK(countOf(log, " row check ") >= rendered);
    // ... and none came out with an empty band.
    CHECK(log.find("came out") == std::string::npos);
    // The request lines carry the requested size and the PPix's row pitch.
    CHECK(log.find("imGetSourceVideo #1: ") != std::string::npos);
    CHECK(log.find(", requested ") != std::string::npos);
    CHECK(log.find(", row bytes ") != std::string::npos);
}

}  // namespace

TEST_CASE("an .LRF delivers every row at host-style mismatched sizes, as its .OSV's proxy and on its own",
          "[importer][lrf][proxy][sample]") {
    const std::filesystem::path proxyPath = sampleProxyPath();
    std::error_code ec;
    if (proxyPath.empty() || !std::filesystem::exists(proxyPath, ec)) {
        SKIP("the .LRF proxy is not present at " << proxyPath.string());
    }
    std::filesystem::path original = proxyPath;
    original.replace_extension(".OSV");
    if (!std::filesystem::exists(original, ec)) {
        SKIP("the sample .LRF is not beside its .OSV: " << original.string());
    }
    const DebugLogLevel debugLog;

    SECTION("as the proxy of its .OSV") {
        // 65 timeline frames at 59.94 (the original's): first, middle, last.
        std::size_t rendered = 0;
        std::int32_t w = 0;
        std::int32_t h = 0;
        const std::string log = runClip(proxyPath, true, {0, 32, 64}, rendered, w, h);
        CHECK(w == 2000);
        CHECK(h == 1000);
        CHECK(rendered == 15u);
        checkLog(log, rendered);
        // The original's size on the proxy is the disagreement the old log
        // could not show; now it is one line.
        CHECK(log.find("requested 6000x3000 BGRA 32f; delivering 2000x1000") != std::string::npos);
        CHECK(log.find("requested 6000x3000, chose 2000x1000") != std::string::npos);
    }
    SECTION("on its own") {
        // A copy with no .OSV beside it: its own 124 frames at 29.97.
        const std::filesystem::path alone =
            std::filesystem::temp_directory_path() / "openosv-lrf-coverage" / proxyPath.filename();
        std::filesystem::create_directories(alone.parent_path(), ec);
        std::filesystem::copy_file(proxyPath, alone, std::filesystem::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
        std::size_t rendered = 0;
        std::int32_t w = 0;
        std::int32_t h = 0;
        const std::string log = runClip(alone, false, {0, 61, 123}, rendered, w, h);
        CHECK(w == 2048);
        CHECK(h == 1024);
        CHECK(rendered == 15u);
        checkLog(log, rendered);
        // The proxy's size asked of the .LRF alone: snapped to 2048 x 1024.
        CHECK(log.find("requested 2000x1000 BGRA 32f; delivering 2048x1024") != std::string::npos);
        CHECK(log.find("requested 2000x1000, chose 2048x1024") != std::string::npos);
        // The harness (and with it the clip) is gone, so the copy can go.
        std::filesystem::remove_all(alone.parent_path(), ec);
    }
}
