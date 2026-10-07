// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// [VFR] What Premiere is told about a clip whose camera dropped frames.
//
// A recording that dropped frames (the container records each gap as one
// longer sample) is presented at its NOMINAL rate with the previous picture
// held over each gap, so the picture lasts exactly as long as the sound; a
// constant-rate clip is presented sample for sample, exactly as before.
//
//   * [sample]       the maintainer's sample clip (constant rate): its
//                    timeline is its sample list.
//   * [vfr-sample]   OSV_VFR_SAMPLE names a variable-frame-rate .OSV or .LRF
//                    (SKIPs without it): video and audio end together.
//   * [vfr-proxy]    OSV_VFR_PROXY names a variable-frame-rate .LRF with its
//                    .OSV beside it under the same name, as the camera writes
//                    them (SKIPs without it): the proxy takes the original's
//                    timeline exactly.
//   * [hwaccel]      a copy of the sample whose second lens track puts one
//                    sample 8.3 ms late: that frame fails with the stream's
//                    own timing error, and the clip keeps its hardware
//                    decoder and its GPU frame path.

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"
#include "MockHost.h"

#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "PrefsBlob.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
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

/// SKIP when the maintainer's sample clip is not on this machine.
#define VFR_REQUIRE_SAMPLE_CLIP()                                                                  \
    do {                                                                                           \
        if (!sampleClipAvailable()) {                                                              \
            SKIP("the sample clip is not present at " << sampleClipPath().string());               \
        }                                                                                          \
    } while (false)

namespace {

/// Premiere's tick base (PrSDKTimeSuite::GetTicksPerSecond).
constexpr PrTime kTicks = 254016000000LL;

/// A path from the environment, or empty when unset / missing.
[[nodiscard]] std::filesystem::path envClip(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    std::filesystem::path p(value);
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
}

/// The imAnalysis text of an open clip (two-step protocol).
[[nodiscard]] std::string analysisText(ImporterHarness& harness, ImporterHarness::ClipHandle& clip) {
    PrefsBlob prefs = PrefsBlob::defaults();
    imAnalysisRec rec{};
    rec.privatedata = clip.privateData();
    rec.prefs = &prefs;
    rec.buffer = nullptr;
    rec.buffersize = 0;
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    REQUIRE(rec.buffersize > 0);
    std::vector<char> buffer(static_cast<std::size_t>(rec.buffersize), '\0');
    rec.buffer = buffer.data();
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    return std::string(buffer.data());
}

/// The video length imGetInfo8 declares, in seconds.
[[nodiscard]] double videoSeconds(const imFileInfoRec8& info) {
    REQUIRE(info.vidScale > 0);
    REQUIRE(info.vidSampleSize > 0);
    return static_cast<double>(info.vidDurationInFrames) * static_cast<double>(info.vidSampleSize) /
           static_cast<double>(info.vidScale);
}

/// The audio length imGetInfo8 declares, in seconds (0 without audio).
[[nodiscard]] double audioSeconds(const imFileInfoRec8& info) {
    if (!(info.audInfo.sampleRate > 0.0f)) {
        return 0.0;
    }
    return static_cast<double>(info.audDuration) / static_cast<double>(info.audInfo.sampleRate);
}

// -----------------------------------------------------------------------------
//  A stream-timing failure on a copy of the sample clip
// -----------------------------------------------------------------------------

/// Big-endian 32-bit field at `o` of a file image (the caller checked the bounds).
[[nodiscard]] std::uint32_t readU32(const std::vector<char>& b, std::size_t o) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 3]));
}

/// Store a big-endian 32-bit field at `o` (the caller checked the bounds).
void writeU32(std::vector<char>& b, std::size_t o, std::uint32_t v) {
    b[o] = static_cast<char>((v >> 24) & 0xFFu);
    b[o + 1] = static_cast<char>((v >> 16) & 0xFFu);
    b[o + 2] = static_cast<char>((v >> 8) & 0xFFu);
    b[o + 3] = static_cast<char>(v & 0xFFu);
}

/// One box of a file image: where it starts and where it ends.
struct BoxSpan {
    std::size_t at = 0;   ///< Offset of the size field.
    std::size_t end = 0;  ///< One past the last byte.
    bool found = false;   ///< False when no such box exists (or the layout does not parse).
};

/// The first box of type `tag` among the boxes laid out in [begin, end).  A
/// 64-bit size is understood (the sample's mdat may use one); every box this
/// helper patches uses a 32-bit size, which the caller checks.
[[nodiscard]] BoxSpan findBox(const std::vector<char>& b, std::size_t begin, std::size_t end, const char* tag) {
    std::size_t pos = begin;
    while (end <= b.size() && pos + 8u <= end) {
        std::uint64_t size = readU32(b, pos);
        if (size == 1u && pos + 16u <= end) {
            size = (static_cast<std::uint64_t>(readU32(b, pos + 8u)) << 32) | readU32(b, pos + 12u);
        } else if (size == 0u) {
            size = end - pos;  // "to the end of the file"
        }
        if (size < 8u || size > end - pos) {
            return {};
        }
        if (std::memcmp(b.data() + pos + 4, tag, 4) == 0) {
            return {pos, pos + static_cast<std::size_t>(size), true};
        }
        pos += static_cast<std::size_t>(size);
    }
    return {};
}

/// Copy `source` to `target` with lens track `trackId`'s sample `shifted`
/// presented half a frame period late, every other time of every track
/// unchanged - the two lens tracks then disagree about exactly one moment.
///
/// The track's single-run time table (n samples of d ticks) becomes four runs
/// (shifted - 1) x d, 1 x (d + d/2), 1 x (d - d/2), rest x d: same sample
/// count, same total, so the clip's rate and length stay what they were.  The
/// table grows by 24 bytes inside the movie box; every enclosing box grows
/// with it.  Only valid when all sample data lies before the movie box (the
/// camera writes the mdat first), so no chunk offset moves - checked.
[[nodiscard]] bool writeLensTimingCopy(const std::filesystem::path& source, const std::filesystem::path& target,
                                       std::uint32_t trackId, std::uint32_t shifted) {
    std::vector<char> b;
    {
        std::ifstream in(source, std::ios::binary);
        if (!in) {
            return false;
        }
        b.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // ---- moov after mdat: inserting into moov moves no sample data -----------------
    const BoxSpan mdat = findBox(b, 0, b.size(), "mdat");
    const BoxSpan moov = findBox(b, 0, b.size(), "moov");
    if (!mdat.found || !moov.found || mdat.end > moov.at) {
        return false;
    }
    // ---- the trak whose tkhd names `trackId` -----------------------------------------
    BoxSpan trak;
    for (std::size_t pos = moov.at + 8u; pos < moov.end;) {
        const BoxSpan t = findBox(b, pos, moov.end, "trak");
        if (!t.found) {
            return false;
        }
        const BoxSpan tkhd = findBox(b, t.at + 8u, t.end, "tkhd");
        if (tkhd.found && tkhd.at + 32u <= tkhd.end) {
            // version 0: creation, modification (4 bytes each) then track_ID;
            // version 1: 8-byte times.
            const std::size_t idAt = tkhd.at + 12u + (static_cast<std::uint8_t>(b[tkhd.at + 8u]) == 1u ? 16u : 8u);
            if (idAt + 4u <= tkhd.end && readU32(b, idAt) == trackId) {
                trak = t;
                break;
            }
        }
        pos = t.end;
    }
    if (!trak.found) {
        return false;
    }
    const BoxSpan mdia = findBox(b, trak.at + 8u, trak.end, "mdia");
    const BoxSpan minf = mdia.found ? findBox(b, mdia.at + 8u, mdia.end, "minf") : BoxSpan{};
    const BoxSpan stbl = minf.found ? findBox(b, minf.at + 8u, minf.end, "stbl") : BoxSpan{};
    const BoxSpan stts = stbl.found ? findBox(b, stbl.at + 8u, stbl.end, "stts") : BoxSpan{};
    if (!stts.found || stts.end - stts.at != 24u || readU32(b, stts.at + 12u) != 1u) {
        return false;  // not the single-run table this patch is written for
    }
    const std::uint32_t count = readU32(b, stts.at + 16u);
    const std::uint32_t delta = readU32(b, stts.at + 20u);
    if (shifted < 2u || shifted + 2u > count || delta < 4u) {
        return false;
    }

    // ---- the four-run table ------------------------------------------------------------
    const std::uint32_t half = delta / 2u;
    const std::uint32_t runs[4][2] = {
        {shifted - 1u, delta}, {1u, delta + half}, {1u, delta - half}, {count - shifted - 1u, delta}};
    std::vector<char> table(8u + 8u + 4u * 8u, '\0');
    writeU32(table, 0, static_cast<std::uint32_t>(table.size()));
    std::memcpy(table.data() + 4, "stts", 4);
    writeU32(table, 12, 4u);
    for (std::size_t r = 0; r < 4; ++r) {
        writeU32(table, 16u + 8u * r, runs[r][0]);
        writeU32(table, 20u + 8u * r, runs[r][1]);
    }
    const std::uint32_t grown = static_cast<std::uint32_t>(table.size()) - 24u;

    // ---- splice it in; the enclosing boxes grow with it -----------------------------
    // Each of them must carry its size in the plain 32-bit field this rewrites.
    for (const BoxSpan& box : {moov, trak, mdia, minf, stbl}) {
        if (readU32(b, box.at) != box.end - box.at) {
            return false;
        }
    }
    for (const BoxSpan& box : {moov, trak, mdia, minf, stbl}) {
        writeU32(b, box.at, readU32(b, box.at) + grown);
    }
    b.erase(b.begin() + static_cast<std::ptrdiff_t>(stts.at), b.begin() + static_cast<std::ptrdiff_t>(stts.end));
    b.insert(b.begin() + static_cast<std::ptrdiff_t>(stts.at), table.begin(), table.end());

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
    return out.good();
}

/// The importer's log file for this process (LOCALAPPDATA was redirected by
/// isolatePluginLogs() in TestMain).
[[nodiscard]] std::string importerLog() {
    wchar_t buffer[32768] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    const std::filesystem::path path =
        std::filesystem::path(std::wstring(buffer, n)) / L"OpenOSV" / L"OpenOSVImporter.log";
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

/// Set an environment variable for the lifetime of the object (the importer
/// is /MD and reads the CRT's environment, so _putenv_s reaches it).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : m_name(name) {
        char* old = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&old, &length, name) == 0 && old) {
            m_previous = old;
        }
        std::free(old);
        ::_putenv_s(name, value);
    }
    ~ScopedEnv() { ::_putenv_s(m_name, m_previous.c_str()); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    std::string m_previous;
};

/// A directory of the test's own, created on construction and removed with
/// everything in it on destruction - on every way out of the test, so a
/// failed REQUIRE never leaves a copy of the sample clip behind.
class ScratchDir {
public:
    explicit ScratchDir(std::filesystem::path path) : m_path(std::move(path)) {
        std::error_code ec;
        std::filesystem::create_directories(m_path, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

/// Ask for frame `index` with the host cache emptied first (so the importer
/// really renders) and hand back the selector result; a delivered PPix is
/// disposed at once.
[[nodiscard]] csSDK_int32 requestFrame(ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                       const imFileInfoRec8& info, std::uint32_t index) {
    harness.host().clearCache();
    ImporterHarness::SourceVideoRequest request;
    request.frameTime = static_cast<PrTime>(info.vidInfo.frameRate) * static_cast<PrTime>(index);
    request.format = PrPixelFormat_BGRA_4444_8u;
    request.width = 1920;
    request.height = 960;
    // A small, analysis-free render: what is under test is the decode.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;
    prefs.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    PPixHand hand = nullptr;
    const csSDK_int32 result = harness.getSourceVideo(clip, request, prefs, hand);
    if (hand != nullptr) {
        const void* suite = nullptr;
        auto* basic = harness.host().basicSuite();
        if (basic && basic->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError && suite) {
            static_cast<const PrSDKPPixSuite*>(suite)->Dispose(hand);
            basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        }
    }
    return result;
}

}  // namespace

TEST_CASE("a constant-rate clip's timeline is its sample list", "[importer][vfr][sample]") {
    VFR_REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    // 65 samples at 60000 / 1001: 65 frames, nothing held, nothing added.
    CHECK(info.vidScale == 60000);
    CHECK(info.vidSampleSize == 1001);
    CHECK(info.vidDurationInFrames == 65);
    CHECK(info.vidInfo.frameRate == kTicks * 1001 / 60000);
    const std::string text = analysisText(harness, clip);
    INFO(text);
    CHECK(text.find("Frames: 65\r\n") != std::string::npos);
    CHECK(text.find("on the timeline") == std::string::npos);
}

TEST_CASE("a clip that dropped frames is presented at its nominal rate and ends with its sound",
          "[importer][vfr][vfr-sample]") {
    const std::filesystem::path path = envClip("OSV_VFR_SAMPLE");
    if (path.empty()) {
        SKIP("OSV_VFR_SAMPLE does not name a variable-frame-rate clip");
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(path);
    INFO("open result " << clip.openResult());
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);

    // The nominal rate: a standard camera rate, not the average a sample
    // count over the recording's length would give (49.48 or 24.51 fps).
    const double fps = static_cast<double>(info.vidScale) / static_cast<double>(info.vidSampleSize);
    INFO("rate " << info.vidScale << " / " << info.vidSampleSize << ", " << info.vidDurationInFrames << " frames");
    const double nearestWhole = std::round(fps);
    CHECK((std::fabs(fps - nearestWhole) < 1e-9 || std::fabs(fps * 1.001 - nearestWhole) < 1e-6));
    CHECK(info.vidInfo.frameRate == kTicks * info.vidSampleSize / info.vidScale);

    // Video and audio end together - within two frames - where a sample
    // count at the nominal rate ends seconds early.
    const double video = videoSeconds(info);
    const double audio = audioSeconds(info);
    INFO("video " << video << " s, audio " << audio << " s");
    REQUIRE(audio > 0.0);
    CHECK(std::fabs(video - audio) < 2.0 / fps);

    // File > Properties says what happened.
    const std::string text = analysisText(harness, clip);
    INFO(text);
    CHECK(text.find("on the timeline") != std::string::npos);
    CHECK(text.find("held over") != std::string::npos);
}

TEST_CASE("a variable-frame-rate .LRF beside its .OSV takes the original's timeline exactly",
          "[importer][vfr][vfr-proxy]") {
    const std::filesystem::path proxy = envClip("OSV_VFR_PROXY");
    if (proxy.empty()) {
        SKIP("OSV_VFR_PROXY does not name an .LRF with its .OSV beside it");
    }
    std::filesystem::path original = proxy;
    original.replace_extension(".OSV");
    std::error_code ec;
    if (!std::filesystem::exists(original, ec)) {
        SKIP("no .OSV beside " << proxy.string());
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto originalClip = harness.openClip(original, 8);
    REQUIRE(originalClip.open());
    imFileInfoRec8 originalInfo{};
    REQUIRE(harness.getInfo8(originalClip, originalInfo) == imNoErr);
    auto proxyClip = harness.openClip(proxy);
    REQUIRE(proxyClip.open());
    imFileInfoRec8 proxyInfo{};
    REQUIRE(harness.getInfo8(proxyClip, proxyInfo) == imNoErr);

    // Adobe's attach rule: the same frame rate and the same length.
    CHECK(proxyInfo.vidScale == originalInfo.vidScale);
    CHECK(proxyInfo.vidSampleSize == originalInfo.vidSampleSize);
    CHECK(proxyInfo.vidInfo.frameRate == originalInfo.vidInfo.frameRate);
    CHECK(proxyInfo.vidDurationInFrames == originalInfo.vidDurationInFrames);
    // And that length is the original's sound's.
    const double fps = static_cast<double>(originalInfo.vidScale) / static_cast<double>(originalInfo.vidSampleSize);
    CHECK(std::fabs(videoSeconds(originalInfo) - audioSeconds(originalInfo)) < 2.0 / fps);
}

TEST_CASE("a frame whose lens tracks disagree about its moment fails without costing the clip its hardware decoder",
          "[importer][vfr][sample][hwaccel]") {
    // The two lens tracks of a copy of the sample disagree about frame 30 by
    // half a frame period: the file's own timing, ErrorCode::Timing.  Software
    // would read the same tables to the same failure, so that frame fails -
    // and nothing else changes: no switch to software decoding, no strike
    // against the GPU frame path, no new reader.
    VFR_REQUIRE_SAMPLE_CLIP();
    constexpr std::uint32_t kShifted = 30;
    const ScratchDir dir(std::filesystem::temp_directory_path() /
                         ("openosv-vfr-timing-" + std::to_string(::GetCurrentProcessId())));
    ScopedEnv infoLevel("OSV_PLUGIN_LOG_LEVEL", "info");

    // Both of the importer's own frame paths, each on a copy of its own so the
    // reader pool can never hand one section's reader to the other.
    bool gpuPath = false;
    std::string name;
    SECTION("the host frame path") {
        gpuPath = false;
        name = "lens_timing_host.OSV";
    }
    SECTION("the GPU frame path") {
        gpuPath = true;
        name = "lens_timing_gpu.OSV";
    }
    const std::filesystem::path patched = dir.path() / name;
    INFO("patched copy: " << patched.string() << " (needs the sample's size free on that drive)");
    REQUIRE(writeLensTimingCopy(sampleClipPath(), patched, 2u, kShifted));
    ScopedEnv framePath("OPENOSV_IMPORTER_NO_GPU_DECODE", gpuPath ? "" : "1");

    // ---- the clip: a good frame, the broken one three times, good ones after ----
    const std::size_t logBefore = importerLog().size();
    {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(patched);
        INFO("open result " << clip.openResult());
        REQUIRE(clip.open());
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info) == imNoErr);
        REQUIRE(info.vidDurationInFrames == 65);  // the clip's own timeline is untouched

        CHECK(requestFrame(harness, clip, info, 0) == imNoErr);
        // Three in a row: as many as retire the GPU frame path, and two more
        // than it takes to latch software decoding, for a real decoder failure.
        for (int attempt = 0; attempt < 3; ++attempt) {
            INFO("attempt " << attempt);
            CHECK(requestFrame(harness, clip, info, kShifted) == imDecompressionError);
        }
        CHECK(requestFrame(harness, clip, info, 1) == imNoErr);
        CHECK(requestFrame(harness, clip, info, kShifted + 1u) == imNoErr);
    }
    const std::string all = importerLog();
    const std::string log = all.substr(std::min(logBefore, all.size()));
    INFO("importer log of this section:\n" << log);

    // ---- one hardware reader, never replaced --------------------------------------
    // The FIRST reader decides whether there was hardware to keep (a software
    // reader opened later is exactly the failure this test is about).
    const std::string opened = "'" + name + "' decoding with ";
    const std::size_t first = log.find(opened);
    REQUIRE(first != std::string::npos);
    if (log.compare(first + opened.size(), 4, "none") == 0) {
        SKIP("no hardware decoder on this machine: nothing to keep");
    }
    CHECK(countOf(log, opened) == 1u);
    CHECK(countOf(log, opened + "none") == 0u);
    CHECK(countOf(log, "switching this clip to software decoding") == 0u);
    // Every attempt reached the host decoder, which said whose fault it was.
    CHECK(countOf(log, "the stream's timing is at fault, so ") >= 3u);

    // ---- the GPU frame path took no strike -------------------------------------
    if (gpuPath) {
        if (countOf(log, "'" + name + "' keeps the host frame path") > 0) {
            WARN("the GPU frame path does not apply on this machine; only the host path was exercised");
        } else {
            CHECK(countOf(log, "the stream's timing is at fault, not the GPU path") >= 3u);
            CHECK(countOf(log, "three in a row") == 0u);
        }
    }
}
