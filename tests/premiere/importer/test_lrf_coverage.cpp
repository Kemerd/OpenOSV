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
//   * the .LRF's native size, whatever was asked (an .LRF advertises no
//     half or quarter step: it is the small picture already);
//   * every row opaque (alpha >= 0.5) over at least 95 % of its columns -
//     ALL columns, read independently of the importer's own self-check;
//   * no row whose B, G and R are zero in every column;
//   * the importer's self-check (FrameRowCheck.h, run on every frame at
//     Trace level) agreeing, and its log saying so: one row-check line per
//     rendered frame, no "came out" warning, and the requested-vs-delivered
//     size on record for every size the host was not given as asked.
//
// A second case runs at Debug, where the self-check is NOT every frame: it
// proves the check's budget restarts for each delivered size, format and
// quality (full frames after draft thumbnails of the same size, and a size
// first asked for late, are still checked; an exhausted one is not checked
// again), and that a mismatched request size repeated many times, across
// more sizes than the importer's memo holds, is logged exactly once.
//
// A third case proves the per-frame delivery fingerprint (the 'deliver:'
// line, OPENOSV_VERIFY_DELIVERY=1) really compares what it promises on the
// host path, for the .LRF as its .OSV's proxy - where the host's frame
// numbers are the ORIGINAL's and the frame rendered for each one is a
// different proxy sample.  CPU only: Render Device = CPU, the classical flow
// on the CPU, and the proxy decoded in software (OPENOSV_IMPORTER_LRF_SOFTWARE).

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
#include <string_view>
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

/// Set the plug-in log's level for the lifetime of the object, restoring the
/// previous value afterwards.  "trace" makes the importer self-check EVERY
/// rendered frame; "debug" keeps the default level's budget (the first
/// frames of each delivered size, format and quality) but writes a summary
/// line for each frame it checks.  Must exist BEFORE the harness: the module reads
/// the level once, when its log initialises.
class LogLevel {
public:
    explicit LogLevel(const char* level) {
        char* old = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&old, &length, "OSV_PLUGIN_LOG_LEVEL") == 0 && old) {
            m_previous = old;
            m_hadPrevious = true;
        }
        std::free(old);
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", level ? level : "");
    }
    ~LogLevel() { ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", m_hadPrevious ? m_previous.c_str() : ""); }
    LogLevel(const LogLevel&) = delete;
    LogLevel& operator=(const LogLevel&) = delete;

private:
    std::string m_previous;
    bool m_hadPrevious = false;
};

/// Set one environment variable for the lifetime of the object, restoring the
/// previous value (or its absence) afterwards.  _putenv_s, because the .prm
/// shares this process's CRT (/MD) and reads its switches with getenv_s.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : m_name(name ? name : "") {
        if (m_name.empty()) {
            return;  // nothing to set, nothing to restore
        }
        char* old = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&old, &length, m_name.c_str()) == 0 && old) {
            m_previous = old;
            m_hadPrevious = true;
        }
        std::free(old);
        ::_putenv_s(m_name.c_str(), value ? value : "");
    }
    ~ScopedEnv() {
        if (!m_name.empty()) {
            // An empty value removes the variable, which is "unset" again.
            ::_putenv_s(m_name.c_str(), m_hadPrevious ? m_previous.c_str() : "");
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string m_name;
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

/// Lines of `log` that contain `needle`, and of those, how many also contain
/// `also` (the row-check line ends in ", draft" for a draft frame).
void countLines(const std::string& log, const std::string& needle, const std::string& also, std::size_t& withNeedle,
                std::size_t& withBoth) {
    withNeedle = 0;
    withBoth = 0;
    std::size_t start = 0;
    while (start < log.size()) {
        std::size_t end = log.find('\n', start);
        if (end == std::string::npos) {
            end = log.size();
        }
        const std::string_view line(log.data() + start, end - start);
        if (line.find(needle) != std::string_view::npos) {
            ++withNeedle;
            withBoth += line.find(also) != std::string_view::npos ? 1u : 0u;
        }
        start = end + 1;
    }
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

/// The size the importer must deliver for a request on an .LRF that
/// advertises `baseW` x `baseH`: that size, whatever was asked.  An .LRF is
/// the small picture already (2000 x 1000 as a 6K original's proxy, 2048 x
/// 1024 alone), so it advertises no half or quarter step and a request for
/// less - a 1/2 playback resolution, a scrub draft, a thumbnail - is
/// delivered at its native size for the host to scale down.  (An .OSV keeps
/// the native / half / quarter ladder; test_importer.cpp holds that rule.)
/// The documented rule, restated here as the test's own oracle.
void expectedSize(std::int32_t baseW, std::int32_t baseH, const HostRequest& request, std::int32_t& outW,
                  std::int32_t& outH) {
    (void)request;  // every request, "any" size included, gets the native frame
    outW = baseW;
    outH = baseH;
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

    // ---- the size: an .LRF's native size, whatever was asked --------------------
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

/// One request of `frame` at `width` x `height` in `format`, the host cache
/// cleared first or not; true when a frame came back (it is disposed).  The
/// budget case below only reads the log, so nothing else is checked here.
[[nodiscard]] bool requestOnly(ImporterHarness& harness, ImporterHarness::ClipHandle& clip, const imFileInfoRec8& info,
                               const PrSDKPPixSuite* ppix, std::int64_t frame, std::int32_t width, std::int32_t height,
                               PrPixelFormat format, imRenderIntent intent, bool clearCache, const PrefsBlob& prefs) {
    if (clearCache) {
        harness.host().clearCache();
    }
    ImporterHarness::SourceVideoRequest source;
    source.frameTime = static_cast<PrTime>(frame) * info.vidInfo.frameRate;
    source.format = format;
    source.width = width;
    source.height = height;
    source.intent = intent;
    PPixHand hand = nullptr;
    const csSDK_int32 result = harness.getSourceVideo(clip, source, prefs, hand);
    if (result != imNoErr || !hand) {
        return false;
    }
    if (ppix && ppix->Dispose) {
        ppix->Dispose(hand);
    }
    return true;
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
    // Every rendered frame was self-checked (Trace level: every frame) ...
    CHECK(countOf(log, " row check ") >= rendered);
    // ... and none came out with an empty band.
    CHECK(log.find("came out") == std::string::npos);
    // The request lines carry the requested size and the PPix's row pitch.
    CHECK(log.find("imGetSourceVideo #1: ") != std::string::npos);
    CHECK(log.find(", requested ") != std::string::npos);
    CHECK(log.find(", row bytes ") != std::string::npos);
}

/// One 'deliver:' line of the importer's log, taken apart.
struct DeliverLine {
    std::uint32_t frame = 0;   ///< The host's (timeline) frame.
    std::uint32_t source = 0;  ///< The clip's own frame rendered for it.
    std::string text;          ///< The whole line, for messages.
};

/// Parse the unsigned decimal number at `pos` in `line`; false when there is
/// none (or it does not fit 32 bits).  Advances `pos` past the digits.
[[nodiscard]] bool parseNumber(std::string_view line, std::size_t& pos, std::uint32_t& out) {
    std::uint64_t value = 0;
    const std::size_t start = pos;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        value = value * 10u + static_cast<std::uint64_t>(line[pos] - '0');
        if (value > 0xFFFFFFFFull) {
            return false;
        }
        ++pos;
    }
    out = static_cast<std::uint32_t>(value);
    return pos > start;
}

/// Every 'deliver:' line of `log`, with its two frame numbers
/// ("deliver: clip '<name>' frame N (source S) ...").  A line that does not
/// carry both numbers is returned with `source` = UINT32_MAX, so a test sees
/// the malformed line instead of losing it.
[[nodiscard]] std::vector<DeliverLine> deliverLines(const std::string& log) {
    std::vector<DeliverLine> out;
    std::size_t start = 0;
    while (start < log.size()) {
        std::size_t end = log.find('\n', start);
        if (end == std::string::npos) {
            end = log.size();
        }
        const std::string_view line(log.data() + start, end - start);
        start = end + 1;
        const std::size_t tag = line.find("deliver: clip '");
        if (tag == std::string_view::npos) {
            continue;
        }
        DeliverLine d;
        d.text = std::string(line);
        d.source = 0xFFFFFFFFu;
        // The clip name is quoted; the numbers follow its closing quote.
        const std::size_t frameAt = line.find("' frame ", tag);
        if (frameAt != std::string_view::npos) {
            std::size_t pos = frameAt + std::string_view("' frame ").size();
            std::uint32_t frame = 0;
            std::uint32_t source = 0;
            if (parseNumber(line, pos, frame) && line.substr(pos, 9) == " (source " &&
                (pos += 9, parseNumber(line, pos, source))) {
                d.frame = frame;
                d.source = source;
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

/// True when some line of `log` contains both `a` and `b`.
[[nodiscard]] bool anyLineWithBoth(const std::string& log, std::string_view a, std::string_view b) {
    std::size_t start = 0;
    while (start < log.size()) {
        std::size_t end = log.find('\n', start);
        if (end == std::string::npos) {
            end = log.size();
        }
        const std::string_view line(log.data() + start, end - start);
        start = end + 1;
        if (line.find(a) != std::string_view::npos && line.find(b) != std::string_view::npos) {
            return true;
        }
    }
    return false;
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
    // Trace: every rendered frame is self-checked, not only the first few of
    // each size, format and quality.
    const LogLevel traceLog("trace");

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

TEST_CASE("at Debug the row self-check covers the first frames of every delivered size and quality, and a "
          "repeated mismatched size is logged once",
          "[importer][lrf][proxy][rowcheck][sample]") {
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
    // Debug, the level the benchmarks run at: the summary line for each
    // checked frame, but the default level's budget, not every frame.
    const LogLevel debugLog("debug");

    const std::uintmax_t logStart = logSize();
    {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(proxyPath);
        INFO("open result " << clip.openResult());
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info) == imNoErr);
        // The proxy of the 6K original advertises 2000 x 1000, and that size
        // only: a request for less is delivered at it (the host scales).
        REQUIRE(info.vidInfo.imageWidth == 2000);
        REQUIRE(info.vidInfo.imageHeight == 1000);
        REQUIRE(info.vidInfo.frameRate > 0);
        REQUIRE(info.vidDurationInFrames > 8);

        const void* suite = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) ==
                kSPNoError);
        const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
        const PrefsBlob prefs = PrefsBlob::defaults();

        // ---- draft thumbnails at the advertised size come first ---------------
        // As in a bin: four of them, of which the budget checks the first few
        // - and those must not spend the full-quality frames' budget below.
        for (std::int64_t frame = 10; frame < 14; ++frame) {
            INFO("2000 x 1000 32f thumbnail, frame " << frame);
            CHECK(requestOnly(harness, clip, info, ppix, frame, 2000, 1000, PrPixelFormat_BGRA_4444_32f,
                              imRenderIntent_Thumbnail, true, prefs));
        }
        // ---- six frames at the advertised size: only the budget's first few ----
        for (std::int64_t frame = 0; frame < 6; ++frame) {
            INFO("2000 x 1000 32f, frame " << frame);
            CHECK(requestOnly(harness, clip, info, ppix, frame, 2000, 1000, PrPixelFormat_BGRA_4444_32f,
                              imRenderIntent_Export, true, prefs));
        }
        // ---- a format first asked for after that: its own budget ---------------
        // Asked at half the proxy's size, as a 1/2 playback resolution asks:
        // a proxy never sub-sizes (it is the small picture already), so these
        // are delivered at 2000 x 1000 16u - a new format, hence a new budget.
        for (std::int64_t frame = 6; frame < 8; ++frame) {
            INFO("1000 x 500 16u, frame " << frame);
            CHECK(requestOnly(harness, clip, info, ppix, frame, 1000, 500, PrPixelFormat_BGRA_4444_16u,
                              imRenderIntent_Stopped, true, prefs));
        }
        // ---- ten mismatched sizes, three times each, interleaved -----------------
        // More sizes than the importer's memo holds (rowcheck::kSeenSizeSlots),
        // all delivered at 2000 x 1000 32f - a geometry whose budget is spent -
        // and the host cache left alone, as Premiere does.
        for (int round = 0; round < 3; ++round) {
            for (std::int32_t k = 1; k <= 10; ++k) {
                INFO("round " << round << ", requested " << 2 * (1000 + k) << "x" << 1000 + k);
                CHECK(requestOnly(harness, clip, info, ppix, 0, 2 * (1000 + k), 1000 + k,
                                  PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export, false, prefs));
            }
        }
        harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
    }
    const std::string log = logSince(logStart);
    INFO("importer log of the run:\n" << log.substr(0, std::min<std::size_t>(log.size(), 6000u)));

    // The advertised size: checked for the first rowcheck::kFramesPerGeometry
    // drafts (four thumbnails) and, separately, the first as many full frames
    // (six rendered, thirty more requested) - so the thumbnails did not spend
    // the export's budget, and Debug never scans a frame the default level
    // would not.
    std::size_t advertised = 0;
    std::size_t advertisedDrafts = 0;
    countLines(log, " row check 2000x1000 32f:", ", draft", advertised, advertisedDrafts);
    CHECK(advertisedDrafts == rowcheck::kFramesPerGeometry);
    CHECK(advertised - advertisedDrafts == rowcheck::kFramesPerGeometry);
    // The format asked for late: still checked, every frame of its (smaller
    // than the budget) run - at the proxy's native size, never at the half
    // size asked for.
    CHECK(countOf(log, " row check 2000x1000 16u:") == 2u);
    CHECK(countOf(log, " row check 1000x500 16u:") == 0u);
    CHECK(countOf(log, "requested 1000x500 BGRA 16u; delivering 2000x1000") == 1u);
    CHECK(log.find("came out") == std::string::npos);
    // Every mismatched size on record exactly once, the two past the memo's
    // slots included.
    for (std::int32_t k = 1; k <= 10; ++k) {
        const std::string line = "requested " + std::to_string(2 * (1000 + k)) + "x" + std::to_string(1000 + k) +
                                 " BGRA 32f; delivering 2000x1000";
        INFO(line);
        CHECK(countOf(log, line) == 1u);
    }
}

// -----------------------------------------------------------------------------
//  An .LRF advertises its native size only
// -----------------------------------------------------------------------------
// The camera's .LRF is the small picture already (2000 x 1000 as a 6K
// original's proxy, 2048 x 1024 on its own).  Premiere's 1/2 and 1/4 playback
// resolutions ask a clip for the half and quarter sizes it advertises, and a
// 500 x 250 frame magnified forty-odd times by a 60 degree reframe is the
// blocky, smeared playback a proxy on the timeline was reported with.  So an
// .LRF enumerates one size (imGetPreferredFrameSize) and delivers a smaller
// request at its native size, which the host scales down.
TEST_CASE("an .LRF advertises its native size only, as a proxy and on its own",
          "[importer][lrf][proxy][format][sample]") {
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

    /// The sizes `clip` enumerates: index 0 is the native size and ends the
    /// enumeration (imNoErr, not imIterateFrameSizes); index 1 is refused.
    const auto checkLadder = [](ImporterHarness& harness, ImporterHarness::ClipHandle& clip, std::int32_t nativeW,
                                std::int32_t nativeH) {
        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
        imPreferredFrameSizeRec first{};
        first.inPrivateData = clip.privateData();
        first.inPrefs = &prefs;
        first.inPixelFormat = PrPixelFormat_BGRA_4444_32f;
        first.inIndex = 0;
        REQUIRE(harness.send(imGetPreferredFrameSize, &first, nullptr) == imNoErr);
        CHECK(first.outWidth == nativeW);
        CHECK(first.outHeight == nativeH);
        imPreferredFrameSizeRec half{};
        half.inPrivateData = clip.privateData();
        half.inPrefs = &prefs;
        half.inPixelFormat = PrPixelFormat_BGRA_4444_32f;
        half.inIndex = 1;
        REQUIRE(harness.send(imGetPreferredFrameSize, &half, nullptr) == imOtherErr);
    };

    /// A request for `askW` x `askH` of frame 0 is delivered as a PPix of
    /// `nativeW` x `nativeH`.
    const auto checkDelivery = [](ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                  const imFileInfoRec8& info, std::int32_t askW, std::int32_t askH,
                                  std::int32_t nativeW, std::int32_t nativeH) {
        const void* suite = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) ==
                kSPNoError);
        const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);
        harness.host().clearCache();
        ImporterHarness::SourceVideoRequest source;
        source.frameTime = 0;
        source.format = PrPixelFormat_BGRA_4444_16u;
        source.width = askW;
        source.height = askH;
        source.intent = imRenderIntent_Playing;
        source.playbackRatio = 0.5;  // a 1/2-resolution playback frame, as Premiere asks
        PPixHand hand = nullptr;
        REQUIRE(harness.getSourceVideo(clip, source, PrefsBlob::defaults(), hand) == imNoErr);
        REQUIRE(hand != nullptr);
        const auto delivered = harness.host().inspect(hand);
        REQUIRE(delivered.has_value());
        CHECK(delivered->width == static_cast<std::uint32_t>(nativeW));
        CHECK(delivered->height == static_cast<std::uint32_t>(nativeH));
        ppix->Dispose(hand);
        harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        (void)info;
    };

    SECTION("as the proxy of its .OSV: 2000 x 1000, and a half-size request is delivered at it") {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(proxyPath);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info) == imNoErr);
        REQUIRE(info.vidInfo.imageWidth == 2000);
        REQUIRE(info.vidInfo.imageHeight == 1000);
        checkLadder(harness, clip, 2000, 1000);
        checkDelivery(harness, clip, info, 1000, 500, 2000, 1000);
        checkDelivery(harness, clip, info, 500, 250, 2000, 1000);
    }
    SECTION("on its own: 2048 x 1024, and a half-size request is delivered at it") {
        const std::filesystem::path alone =
            std::filesystem::temp_directory_path() / "openosv-lrf-native-only" / proxyPath.filename();
        std::filesystem::create_directories(alone.parent_path(), ec);
        std::filesystem::copy_file(proxyPath, alone, std::filesystem::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
        {
            ImporterHarness harness;
            REQUIRE(harness.loaded());
            auto clip = harness.openClip(alone);
            REQUIRE(clip.open());
            imFileInfoRec8 info{};
            REQUIRE(harness.getInfo8(clip, info) == imNoErr);
            REQUIRE(info.vidInfo.imageWidth == 2048);
            REQUIRE(info.vidInfo.imageHeight == 1024);
            checkLadder(harness, clip, 2048, 1024);
            checkDelivery(harness, clip, info, 1024, 512, 2048, 1024);
        }
        std::filesystem::remove_all(alone.parent_path(), ec);
    }
}

TEST_CASE("the delivery fingerprint compares every host-path frame of a proxy .LRF with the frame rendered for it",
          "[importer][lrf][proxy][deliver][sample]") {
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

    // ---- the switches, all before the harness loads the module -----------------
    // Debug: the decoder's per-picture 'decode:' lines too, to line the two
    // up.  OPENOSV_VERIFY_DELIVERY: the 'deliver:' line at Info for every
    // frame.  OPENOSV_IMPORTER_LRF_SOFTWARE: the proxy decodes on the CPU.
    const LogLevel debugLog("debug");
    const ScopedEnv verifyDelivery("OPENOSV_VERIFY_DELIVERY", "1");
    const ScopedEnv proxySoftware("OPENOSV_IMPORTER_LRF_SOFTWARE", "1");

    // ---- the requests: several sizes, formats and qualities --------------------
    // The original runs at twice the proxy's rate, so neighbouring timeline
    // frames can land on one proxy sample (on the sample clip, 1 and 2 both
    // do): the second is then served from the instance's own one-frame cache
    // - still the host path, still comparable.
    struct Request {
        std::int64_t frame;
        std::int32_t width;
        std::int32_t height;
        PrPixelFormat format;
        imRenderIntent intent;
    };
    const Request requests[] = {
        {0, 2000, 1000, PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export},
        {1, 2000, 1000, PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export},
        {2, 2000, 1000, PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export},
        {20, 1000, 500, PrPixelFormat_BGRA_4444_16u, imRenderIntent_Thumbnail},  // a draft
        {40, 1920, 1080, PrPixelFormat_BGRA_4444_8u, imRenderIntent_Playing},
        {64, 2000, 1000, PrPixelFormat_BGRA_4444_32f, imRenderIntent_Export},
    };

    const std::uintmax_t logStart = logSize();
    std::size_t delivered = 0;
    {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(proxyPath);
        INFO("open result " << clip.openResult());
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info) == imNoErr);
        // Presented as the 6K original's proxy: 2000 x 1000 on its timeline.
        REQUIRE(info.vidInfo.imageWidth == 2000);
        REQUIRE(info.vidInfo.imageHeight == 1000);
        REQUIRE(info.vidInfo.frameRate > 0);

        const void* suite = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) ==
                kSPNoError);
        const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

        // The clip's defaults, rendered on the CPU: the host frame path,
        // which is the one the fingerprint can compare, and the explicit
        // classical flow, which stays on the CPU (Auto would take the GPU).
        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Cpu);
        prefs.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Classical);

        for (const Request& r : requests) {
            INFO("timeline frame " << r.frame << ", " << r.width << "x" << r.height);
            REQUIRE(r.frame < info.vidDurationInFrames);
            // The host cache is cleared, so every request reaches the render.
            CHECK(requestOnly(harness, clip, info, ppix, r.frame, r.width, r.height, r.format, r.intent, true, prefs));
            ++delivered;
        }
        harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
    }
    const std::string log = logSince(logStart);

    // ---- one comparable line per delivered frame ---------------------------------
    const std::vector<DeliverLine> lines = deliverLines(log);
    std::string summary;
    for (const DeliverLine& d : lines) {
        summary += d.text + "\n";
    }
    INFO("deliver lines:\n" << summary);
    REQUIRE(lines.size() == delivered);
    // The proxy decoded in software, as the switch asks.
    CHECK(log.find("decodes in software (OPENOSV_IMPORTER_LRF_SOFTWARE is set)") != std::string::npos);
    // Every frame took the host path and was compared: none claims the GPU
    // path, none lost its rendered frame, none differs.
    CHECK(countOf(log, "(matches the rendered frame)") == delivered);
    CHECK(log.find("gpu path") == std::string::npos);
    CHECK(log.find("no rendered frame held") == std::string::npos);
    CHECK(log.find("DIFFERS") == std::string::npos);

    // ---- the two numbers, and the decoder's line for each source frame --------------
    bool sawRemapped = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const DeliverLine& d = lines[i];
        INFO(d.text);
        REQUIRE(d.source != 0xFFFFFFFFu);
        // The lines come in request order; the host's number is the one asked for.
        CHECK(d.frame == static_cast<std::uint32_t>(requests[i].frame));
        sawRemapped = sawRemapped || d.source != d.frame;
        // The source number is the decoder's: its 'decode:' line names it.
        const std::string decodedAs = " frame " + std::to_string(d.source) + " crc ";
        CHECK(anyLineWithBoth(log, "decode: track ", decodedAs));
    }
    // The proxy's own samples are not the original's frame numbers, so at
    // least one line must show the two apart - the case the comparison used
    // to miss (it looked the rendered frame up by the host's number).
    CHECK(sawRemapped);
}

// -----------------------------------------------------------------------------
//  The camera's .LRF beside an .OSV renamed with a copy suffix
// -----------------------------------------------------------------------------
// A user's pairs arrive as CAM_..._D-001.OSV beside CAM_..._D.LRF: the .OSV
// went through a download or a copy that added "-001", the .LRF kept the
// camera's name.  Premiere's Attach Proxies only plays such a pair right when
// the .LRF presents itself on the .OSV's timeline, which needs the importer
// to find the pair.  One renamed .OSV is the pair; two are ambiguous and the
// .LRF keeps its own timeline.  Only imGetInfo8 is asked: nothing renders.
TEST_CASE("an .LRF finds an .OSV renamed with a copy suffix, and only when the choice is unique",
          "[importer][lrf][proxy][sample]") {
    const std::filesystem::path proxy = sampleProxyPath();
    const std::filesystem::path clipPath = sampleClipPath();
    std::error_code ec;
    if (proxy.empty() || !std::filesystem::exists(proxy, ec) || clipPath.empty() ||
        !std::filesystem::exists(clipPath, ec)) {
        SKIP("the sample .OSV / .LRF pair is not present");
    }
    // ---- a folder laid out the way the user's was --------------------------------
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("openosv-lrf-copy-suffix-" + std::to_string(::GetCurrentProcessId()));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    REQUIRE_FALSE(ec);
    const std::filesystem::path lrf = dir / "CAM_20260101000000_0001_D.LRF";
    const std::filesystem::path osv1 = dir / "CAM_20260101000000_0001_D-001.OSV";
    std::filesystem::copy_file(proxy, lrf, std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::copy_file(clipPath, osv1, std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    {
        // ---- one renamed .OSV: the .LRF is its proxy -------------------------------
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(lrf);
        INFO("open result " << clip.openResult());
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info) == imNoErr);
        // The original's rate and length (the sample: 65 frames at 60000 / 1001).
        CHECK(info.vidScale == 60000);
        CHECK(info.vidSampleSize == 1001);
        CHECK(info.vidDurationInFrames == 65);
        CHECK(info.vidInfo.imageWidth == 2000);
        CHECK(info.vidInfo.imageHeight == 1000);
    }

    // ---- a second renamed copy: ambiguous, so the .LRF is on its own -----------------
    const std::filesystem::path osv2 = dir / "CAM_20260101000000_0001_D-002.OSV";
    std::filesystem::copy_file(clipPath, osv2, std::filesystem::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(lrf);
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info) == imNoErr);
        // As recorded: 124 frames at 30000 / 1001, 2048 x 1024.
        CHECK(info.vidScale == 30000);
        CHECK(info.vidSampleSize == 1001);
        CHECK(info.vidDurationInFrames == 124);
        CHECK(info.vidInfo.imageWidth == 2048);
        CHECK(info.vidInfo.imageHeight == 1024);
    }
    // The clips are closed with their harnesses, so the folder can go.
    std::filesystem::remove_all(dir, ec);
}
