// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// `osvtool decode-replay`: replay the frame requests a plug-in session logged
// through the decoder, and compare every picture with a sequential software
// decode of the same frame.
//
//   osvtool decode-replay <clip> --trace <plug-in log> [--trace <older log>]
//                         [--hw none|d3d11va|cuda] [--level request|decode]
//                         [--from N] [--to M] [--pid P] [--threads N]
//                         [--max N] [--tracks A,B] [--csv out.csv]
//
// Why it exists
//   A plug-in log at Debug level is a complete trace of what the host asked
//   for: one "deliver: clip 'X' frame T (source S)" line per frame handed to
//   Premiere (and an "imGetSourceVideo: frame T failed" line per frame that
//   could not be), plus one "decode: track K frame F crc C ..." line per
//   picture a decoder produced, each stamped with the process and thread.
//   Replaying that order through the same reader the importer uses - same
//   options, same hardware path, the reader handed from thread to thread
//   exactly as the logged threads took turns - answers whether the decoder
//   can be made to hand out a wrong picture by the REQUEST PATTERN alone,
//   which a sequential decode never shows.
//
// The two levels
//   * request (default): every requested SOURCE frame is read as a lens pair
//     with one video::DualStreamReader (the importer's host frame path),
//     serialised by one mutex like the importer instance's own lock, each
//     request on the worker that stands for its logged thread.  Each pair is
//     fingerprinted per lens (video::frameFingerprint) and compared with the
//     pair a fresh SOFTWARE reader returns when it reads the same frames in
//     ascending order.  The logged outcome (delivered / failed) is compared
//     with the replay's.
//   * decode: every logged "decode:" line is replayed on one decoder per
//     track (HevcStreamDecoder) in the logged order, and each picture is
//     compared with the logged CRC (when the session could compute one - a
//     picture left on the GPU logs "device") and with a sequential software
//     decode of the same track.
//
// Limits (said here because the log cannot say them): every importer instance
// of a clip has its own reader, but the log lines do not name the instance,
// so all requests of one clip replay on ONE reader; requests of other clips
// are not replayed.  The importer's GPU frame path (NVDEC into VRAM) is not
// replayed - --hw cuda decodes through FFmpeg's NVDEC with host copies.
//
// Exit codes: 0 when every replayed picture equals the software reference
// (identical failures count as equal), 3 when any differs, 1 / 2 for usage
// and input errors.

#include "Commands.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"
#include "osv/core/Result.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/video/Decoder.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"
#include "osv/video/PlanarFrame.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace osvtool {

namespace {

// =============================================================================
//  Options
// =============================================================================

/// Everything CLI11 collects for the command.
struct ReplayOptions {
    std::string inputPath;                 ///< The clip to decode (.OSV / .LRF / MP4).
    std::vector<std::string> tracePaths;   ///< --trace, read in the order given.
    std::string hw = "none";               ///< --hw: the replay's decoder path.
    std::string level = "request";         ///< --level request|decode.
    long long from = -1;                   ///< --from: first source frame kept (-1 = all).
    long long to = -1;                     ///< --to: last source frame kept (-1 = all).
    long long pid = -1;                    ///< --pid: -1 = the clip's last session, 0 = every session.
    int threads = 4;                       ///< --threads: replay worker threads (and decoder threads).
    long long maxEntries = 0;              ///< --max: stop after this many trace entries (0 = all).
    std::string tracks;                    ///< --tracks A,B: lens track ids for files without metadata.
    std::string csvPath;                   ///< --csv: one row per replayed entry.
};

/// The worker / decoder threads are bounded: the tool runs beside a host
/// application and must never take the whole machine.
constexpr int kMaxThreads = 16;

// =============================================================================
//  Small helpers
// =============================================================================

/// Map a library error onto the documented process exit codes.
int exitCodeFor(const osv::Error& error) {
    switch (error.code) {
    case osv::ErrorCode::InvalidArgument: return kExitUsage;
    case osv::ErrorCode::Io:
    case osv::ErrorCode::NotFound:
    case osv::ErrorCode::Malformed:
    case osv::ErrorCode::Truncated: return kExitInput;
    default: return kExitRuntime;
    }
}

/// Print an error line (7-bit ASCII) and return its exit code.
int fail(const osv::Error& error) {
    std::fprintf(stderr, "error: %s\n", osv::log::safe(error.toString()).c_str());
    return exitCodeFor(error);
}

/// Path from a UTF-8 command line argument (main() hands UTF-8 over).
std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// ASCII lower case, for the case-insensitive clip name match.
std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/// Read an unsigned decimal at `pos` of `text`, advancing `pos` past it.
/// False when no digit is there or the value would overflow 64 bits.
bool readUint(std::string_view text, std::size_t& pos, std::uint64_t& out) noexcept {
    std::size_t p = pos;
    std::uint64_t value = 0;
    bool any = false;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[p] - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        any = true;
        ++p;
    }
    if (!any) {
        return false;
    }
    pos = p;
    out = value;
    return true;
}

/// Expect the literal `token` at `pos`, advancing past it.
bool expect(std::string_view text, std::size_t& pos, std::string_view token) noexcept {
    if (text.substr(pos, token.size()) != token) {
        return false;
    }
    pos += token.size();
    return true;
}

/// Read up to the next `'` (the clip name in a quoted log field).
bool readQuoted(std::string_view text, std::size_t& pos, std::string_view& out) noexcept {
    const std::size_t end = text.find('\'', pos);
    if (end == std::string_view::npos) {
        return false;
    }
    out = text.substr(pos, end - pos);
    pos = end + 1;
    return true;
}

/// "%016llx" of a fingerprint, as the plug-in log writes it.
std::string hex64(std::uint64_t value) {
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// =============================================================================
//  The trace
// =============================================================================

/// One entry of the trace: a frame a session asked for (request level) or a
/// picture a decoder produced (decode level).
struct TraceEntry {
    std::size_t line = 0;          ///< 1-based line number within its file.
    std::size_t file = 0;          ///< Index into ReplayOptions::tracePaths.
    std::uint64_t pid = 0;         ///< Process id (0 when the line predates pid stamps).
    std::uint64_t tid = 0;         ///< Thread id of the logged call.
    std::uint32_t track = 0;       ///< Decode level: the container track; request level: 0.
    std::uint32_t frame = 0;       ///< The source frame (request) or decoded frame (decode).
    bool loggedOk = true;          ///< Request level: delivered (true) or failed.
    std::string loggedCrc;         ///< Decode level: the logged fingerprint ("device" when none).
    std::string loggedHw;          ///< Decode level: the logged decoder path.
    std::string loggedError;       ///< Request level failures: the logged reason.
};

/// The "[pid P tid T]" (or older "[tid T]") stamp of a plug-in log line, and
/// where the message after it starts.
struct LineStamp {
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::size_t messageStart = 0;
};

/// Find the thread stamp of a log line; nullopt for lines without one.
std::optional<LineStamp> parseStamp(std::string_view line) noexcept {
    LineStamp stamp;
    std::size_t pos = line.find("[pid ");
    if (pos != std::string_view::npos) {
        pos += 5;
        if (!readUint(line, pos, stamp.pid) || !expect(line, pos, " tid ") || !readUint(line, pos, stamp.tid) ||
            !expect(line, pos, "] ")) {
            return std::nullopt;
        }
        stamp.messageStart = pos;
        return stamp;
    }
    pos = line.find("[tid ");
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += 5;
    if (!readUint(line, pos, stamp.tid) || !expect(line, pos, "] ")) {
        return std::nullopt;
    }
    stamp.messageStart = pos;
    return stamp;
}

/// What the trace reader keeps per logged thread while it waits for the
/// "imGetSourceVideo: frame T failed" line that ends a failed request: the
/// SOURCE frame the failure lines before it named.
struct PendingFailure {
    std::uint32_t source = 0;
    std::string reason;
};

/// Reads the plug-in logs into request- or decode-level entries of one clip.
class TraceReader {
public:
    TraceReader(std::string clipLower, bool decodeLevel) : m_clip(std::move(clipLower)), m_decode(decodeLevel) {}

    /// Parse one file; entries are appended in file order.
    osv::Status read(const std::filesystem::path& path, std::size_t fileIndex) {
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            return osv::failStatus(osv::ErrorCode::Io, "cannot read trace '" + osv::log::safe(path.string()) + "'");
        }
        std::string line;
        std::size_t number = 0;
        while (std::getline(in, line)) {
            ++number;
            // Windows logs end lines with CRLF; the CR is not part of any field.
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            parseLine(line, number, fileIndex);
        }
        return osv::okStatus();
    }

    /// Everything read so far.
    [[nodiscard]] std::vector<TraceEntry>& entries() noexcept { return m_entries; }

private:
    /// True when `name` (from a log field) is the clip being replayed.
    [[nodiscard]] bool isClip(std::string_view name) const { return lowerAscii(name) == m_clip; }

    /// Dispatch one line to the parser of the level asked for.
    void parseLine(std::string_view line, std::size_t number, std::size_t fileIndex) {
        const std::optional<LineStamp> stamp = parseStamp(line);
        if (!stamp) {
            return;
        }
        const std::string_view message = line.substr(stamp->messageStart);
        if (m_decode) {
            parseDecodeLine(message, *stamp, number, fileIndex);
        } else {
            parseRequestLine(message, *stamp, number, fileIndex);
        }
    }

    /// "decode: track K frame F crc C hw H key k[ (damage)] 'clip'".
    void parseDecodeLine(std::string_view message, const LineStamp& stamp, std::size_t number, std::size_t fileIndex) {
        std::size_t pos = 0;
        std::uint64_t track = 0;
        std::uint64_t frame = 0;
        if (!expect(message, pos, "decode: track ") || !readUint(message, pos, track) ||
            !expect(message, pos, " frame ") || !readUint(message, pos, frame) || !expect(message, pos, " crc ")) {
            return;
        }
        // The fingerprint (16 hex digits or "device"), then the path.
        const std::size_t crcEnd = message.find(' ', pos);
        if (crcEnd == std::string_view::npos) {
            return;
        }
        const std::string_view crc = message.substr(pos, crcEnd - pos);
        pos = crcEnd;
        if (!expect(message, pos, " hw ")) {
            return;
        }
        const std::size_t hwEnd = message.find(' ', pos);
        if (hwEnd == std::string_view::npos) {
            return;
        }
        const std::string_view hw = message.substr(pos, hwEnd - pos);
        // The clip is the LAST quoted field (a damage note may come before it).
        const std::size_t close = message.rfind('\'');
        const std::size_t open = close == std::string_view::npos || close == 0 ? std::string_view::npos
                                                                                 : message.rfind('\'', close - 1);
        if (open == std::string_view::npos || !isClip(message.substr(open + 1, close - open - 1))) {
            return;
        }
        if (track == 0 || track > 0xFFFFFFFFull || frame > 0xFFFFFFFFull) {
            return;
        }
        TraceEntry e;
        e.line = number;
        e.file = fileIndex;
        e.pid = stamp.pid;
        e.tid = stamp.tid;
        e.track = static_cast<std::uint32_t>(track);
        e.frame = static_cast<std::uint32_t>(frame);
        e.loggedCrc = std::string(crc);
        e.loggedHw = std::string(hw);
        m_entries.push_back(std::move(e));
    }

    /// The request-level lines: a delivery, the lines that name the source
    /// frame of a failure, and the failure itself.
    void parseRequestLine(std::string_view message, const LineStamp& stamp, std::size_t number,
                          std::size_t fileIndex) {
        std::size_t pos = 0;
        // ---- "deliver: clip 'X' frame T (source S) ..." -------------------------
        if (expect(message, pos, "deliver: clip '")) {
            std::string_view clip;
            std::uint64_t timeline = 0;
            std::uint64_t source = 0;
            if (!readQuoted(message, pos, clip) || !isClip(clip) || !expect(message, pos, " frame ") ||
                !readUint(message, pos, timeline) || !expect(message, pos, " (source ") ||
                !readUint(message, pos, source) || source > 0xFFFFFFFFull) {
                return;
            }
            m_pending.erase(stamp.tid);
            TraceEntry e;
            e.line = number;
            e.file = fileIndex;
            e.pid = stamp.pid;
            e.tid = stamp.tid;
            e.frame = static_cast<std::uint32_t>(source);
            e.loggedOk = true;
            m_entries.push_back(std::move(e));
            return;
        }
        // ---- the lines naming a failed request's SOURCE frame -------------------
        //   "video: GPU frame path failed on frame S of 'X' (reason)..."
        //   "video: frame S of 'X' cannot be decoded (reason)..."
        for (const std::string_view lead : {std::string_view("video: GPU frame path failed on frame "),
                                            std::string_view("video: frame ")}) {
            pos = 0;
            std::uint64_t source = 0;
            std::string_view clip;
            if (expect(message, pos, lead) && readUint(message, pos, source) && expect(message, pos, " of '") &&
                readQuoted(message, pos, clip) && isClip(clip) && source <= 0xFFFFFFFFull) {
                // "cannot be decoded" or "GPU frame path failed": either names it.
                PendingFailure p;
                p.source = static_cast<std::uint32_t>(source);
                const std::size_t open = message.find('(', pos);
                const std::size_t close = message.rfind(')');
                if (open != std::string_view::npos && close != std::string_view::npos && close > open) {
                    p.reason = std::string(message.substr(open + 1, close - open - 1));
                }
                m_pending[stamp.tid] = std::move(p);
                return;
            }
        }
        // ---- "imGetSourceVideo: frame T failed: reason" --------------------------
        pos = 0;
        std::uint64_t timeline = 0;
        if (expect(message, pos, "imGetSourceVideo: frame ") && readUint(message, pos, timeline) &&
            expect(message, pos, " failed: ")) {
            const auto it = m_pending.find(stamp.tid);
            if (it == m_pending.end()) {
                return;  // a failure of another clip (or one whose source frame was never named)
            }
            TraceEntry e;
            e.line = number;
            e.file = fileIndex;
            e.pid = stamp.pid;
            e.tid = stamp.tid;
            e.frame = it->second.source;
            e.loggedOk = false;
            e.loggedError = std::string(message.substr(pos));
            m_pending.erase(it);
            m_entries.push_back(std::move(e));
        }
    }

    std::string m_clip;    ///< The clip's file name, lower case.
    bool m_decode = false; ///< Decode level (else request level).
    std::vector<TraceEntry> m_entries;
    std::unordered_map<std::uint64_t, PendingFailure> m_pending;  ///< Per logged thread.
};

// =============================================================================
//  The replay
// =============================================================================

/// What one replayed (or reference) read produced.
struct ReadOutcome {
    bool ok = false;                       ///< A picture (pair) came back.
    std::string error;                     ///< The error otherwise.
    std::uint32_t index = 0;               ///< The frame the picture carries (a held frame differs from the request).
    std::array<std::uint64_t, 2> crc{};    ///< Fingerprints: both lenses (request) or crc[0] only (decode).
    double ms = 0.0;                       ///< Wall time of the read.
    std::string state;                     ///< The decoder state after the read (for the report).
};

/// The importer's reader options for one decoder path (importerReaderOptions):
/// four frame threads at most here, because the tool runs beside a host.
osv::video::DecoderOptions readerOptions(osv::video::HwAccel hw, int threads) {
    osv::video::DecoderOptions o;
    o.hw = hw;
    o.threads = std::clamp(threads, 1, kMaxThreads);
    o.keepOnDevice = false;
    o.useContainerSamples = true;
    o.shareHwDevice = true;
    o.deferFirstFrame = true;
    return o;
}

/// One line describing a decoder: where it is, and what it said about the
/// last picture it produced or refused.
std::string describeDecoder(const osv::video::HevcStreamDecoder* d, std::uint32_t request) {
    if (d == nullptr || !d->isOpen()) {
        return "decoder not open";
    }
    std::string out = "track " + std::to_string(d->trackId()) + " on " + osv::video::hwAccelName(d->activeHw()) +
                      ", next " + std::to_string(d->nextIndex());
    if (const auto sync = d->previousSyncIndex(request)) {
        out += ", decode start for " + std::to_string(request) + " is " + std::to_string(*sync);
    }
    if (const auto info = d->lastFrameInfo()) {
        out += ", last picture " + std::to_string(info->index) + " (key " + std::to_string(info->keyFrame ? 1 : 0) +
               ", corrupt " + std::to_string(info->corrupt ? 1 : 0) + ", error flags " +
               std::to_string(info->decodeErrorFlags) + ")";
    }
    return out;
}

/// Runs entries on `workers` threads in exactly the logged order: entry i
/// runs on the worker that stands for its logged thread, and only once entry
/// i - 1 has finished, so a decoder is handed from thread to thread the way
/// the host's threads took turns on it.
class Turnstile {
public:
    /// @param count    Number of entries.
    /// @param workerOf Worker index of each entry (same length).
    /// @param workers  Number of workers (>= 1).
    /// @param run      Called once per entry index, on its worker; must not throw.
    template <class Fn>
    static void run(std::size_t count, const std::vector<int>& workerOf, int workers, Fn&& run) {
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t next = 0;
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(workers));
        for (int w = 0; w < workers; ++w) {
            threads.emplace_back([&, w]() noexcept {
                for (std::size_t i = 0; i < count; ++i) {
                    if (workerOf[i] != w) {
                        continue;
                    }
                    // ---- wait for this entry's turn -------------------------------
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [&] { return next == i; });
                    }
                    run(i);
                    // ---- hand the turn on -----------------------------------------
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        next = i + 1;
                    }
                    cv.notify_all();
                }
            });
        }
        for (std::thread& t : threads) {
            t.join();
        }
    }
};

/// Worker index per entry: logged threads in order of first appearance,
/// spread over `workers` replay threads.
std::vector<int> assignWorkers(const std::vector<TraceEntry>& entries, int workers, std::size_t& distinctThreads) {
    std::unordered_map<std::uint64_t, int> slot;
    std::vector<int> out;
    out.reserve(entries.size());
    for (const TraceEntry& e : entries) {
        auto it = slot.find(e.tid);
        if (it == slot.end()) {
            it = slot.emplace(e.tid, static_cast<int>(slot.size())).first;
        }
        out.push_back(it->second % std::max(1, workers));
    }
    distinctThreads = slot.size();
    return out;
}

/// Read one lens pair and fingerprint it.
ReadOutcome readPair(osv::video::DualStreamReader& reader, std::uint32_t index) {
    ReadOutcome out;
    const auto t0 = std::chrono::steady_clock::now();
    auto pair = reader.read(index);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!pair.ok()) {
        out.error = pair.error().toString();
        out.state = "lens 0: " + describeDecoder(reader.decoder(0), index) + "; lens 1: " +
                    describeDecoder(reader.decoder(1), index);
        return out;
    }
    out.ok = true;
    out.index = pair.value().lens[0].frameIndex;
    for (std::size_t l = 0; l < 2; ++l) {
        out.crc[l] = osv::video::frameFingerprint(pair.value().lens[l]);
    }
    return out;
}

/// Decode one picture of one track and fingerprint it.
ReadOutcome readOne(osv::video::HevcStreamDecoder& decoder, std::uint32_t index) {
    ReadOutcome out;
    const auto t0 = std::chrono::steady_clock::now();
    auto frame = decoder.decodeFrame(index);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!frame.ok()) {
        out.error = frame.error().toString();
        out.state = describeDecoder(&decoder, index);
        return out;
    }
    out.ok = true;
    out.index = frame.value().frameIndex;
    out.crc[0] = osv::video::frameFingerprint(frame.value());
    return out;
}

/// Whether a replayed read and the reference read of the same frame agree:
/// the same pictures, or both failing.
bool sameOutcome(const ReadOutcome& a, const ReadOutcome& b, bool pair) {
    if (a.ok != b.ok) {
        return false;
    }
    if (!a.ok) {
        return true;  // both failed: the file, not the request pattern
    }
    return a.index == b.index && a.crc[0] == b.crc[0] && (!pair || a.crc[1] == b.crc[1]);
}

// =============================================================================
//  The clip
// =============================================================================

/// The clip's layout: detected from its metadata (or the container alone),
/// with --tracks overriding the lens tracks for files without metadata.
osv::Result<osv::meta::FormatInfo> resolveFormat(const std::filesystem::path& path, const std::string& tracks) {
    OSV_TRY_ASSIGN(const osv::OsvFile file, osv::OsvFile::open(path));
    osv::meta::FormatInfo format;
    auto meta = osv::meta::MetadataTrack::load(file);
    auto detected = meta.ok() ? osv::meta::FormatDetector::detect(file, &meta.value())
                              : osv::meta::FormatDetector::detect(file, nullptr);
    if (detected.ok()) {
        format = std::move(detected).value();
    } else if (tracks.empty()) {
        return detected.error();
    }
    if (!tracks.empty()) {
        // "A,B": the two lens tracks of a plain dual-track file.
        std::size_t pos = 0;
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        if (!readUint(tracks, pos, a) || !expect(tracks, pos, ",") || !readUint(tracks, pos, b) ||
            pos != tracks.size() || a == 0 || b == 0 || a == b || a > 0xFFFFu || b > 0xFFFFu) {
            return osv::Error{osv::ErrorCode::InvalidArgument, "--tracks wants two different track ids, e.g. 1,2"};
        }
        format.videoTrackIds = {static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b)};
        format.sideBySideProxy = false;
    }
    if (format.videoTrackIds[0] == 0) {
        return osv::Error{osv::ErrorCode::NotFound, "the file has no video track the reader can use"};
    }
    return format;
}

// =============================================================================
//  run
// =============================================================================

int runReplay(const ReplayOptions& opt) {
    // ---- arguments --------------------------------------------------------------
    if (opt.tracePaths.empty()) {
        return fail(osv::Error{osv::ErrorCode::InvalidArgument, "--trace names no log"});
    }
    const bool decodeLevel = opt.level == "decode";
    if (!decodeLevel && opt.level != "request") {
        return fail(osv::Error{osv::ErrorCode::InvalidArgument, "--level must be request or decode"});
    }
    const std::optional<osv::video::HwAccel> hw = osv::video::parseHwAccel(opt.hw);
    if (!hw || *hw == osv::video::HwAccel::Auto) {
        return fail(osv::Error{osv::ErrorCode::InvalidArgument, "--hw must be none, d3d11va or cuda"});
    }
    if (opt.threads < 1 || opt.threads > kMaxThreads) {
        return fail(osv::Error{osv::ErrorCode::InvalidArgument,
                               "--threads must be 1.." + std::to_string(kMaxThreads)});
    }
    if (opt.from >= 0 && opt.to >= 0 && opt.to < opt.from) {
        return fail(osv::Error{osv::ErrorCode::InvalidArgument, "--to is before --from"});
    }
    const std::filesystem::path clip = pathFromUtf8(opt.inputPath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(clip, ec)) {
        return fail(osv::Error{osv::ErrorCode::NotFound, "no such clip: " + osv::log::safe(opt.inputPath)});
    }
    const std::string clipName = osv::log::safe(clip.filename().string());

    // ---- the trace ----------------------------------------------------------------
    TraceReader reader(lowerAscii(clip.filename().string()), decodeLevel);
    for (std::size_t f = 0; f < opt.tracePaths.size(); ++f) {
        const osv::Status read = reader.read(pathFromUtf8(opt.tracePaths[f]), f);
        if (!read.ok()) {
            return fail(read.error());
        }
    }
    std::vector<TraceEntry>& all = reader.entries();
    if (all.empty()) {
        return fail(osv::Error{osv::ErrorCode::NotFound, "the trace has no " +
                                                             std::string(decodeLevel ? "decode" : "request") +
                                                             " lines for '" + clipName + "'"});
    }
    // The session: the clip's last one unless --pid says otherwise.
    const std::uint64_t pid = opt.pid < 0 ? all.back().pid : static_cast<std::uint64_t>(opt.pid);
    std::vector<TraceEntry> entries;
    for (TraceEntry& e : all) {
        if (pid != 0 && e.pid != pid) {
            continue;
        }
        if ((opt.from >= 0 && e.frame < opt.from) || (opt.to >= 0 && e.frame > opt.to)) {
            continue;
        }
        entries.push_back(std::move(e));
        if (opt.maxEntries > 0 && static_cast<long long>(entries.size()) >= opt.maxEntries) {
            break;
        }
    }
    if (entries.empty()) {
        return fail(osv::Error{osv::ErrorCode::NotFound, "no trace entry of '" + clipName + "' is left after the "
                                                         "--pid / --from / --to / --max filters"});
    }

    // ---- the clip -------------------------------------------------------------------
    auto format = resolveFormat(clip, opt.tracks);
    if (!format.ok()) {
        return fail(format.error());
    }
    std::size_t distinctThreads = 0;
    const std::vector<int> workerOf = assignWorkers(entries, opt.threads, distinctThreads);
    const int workers = static_cast<int>(std::min<std::size_t>(std::max<std::size_t>(distinctThreads, 1),
                                                               static_cast<std::size_t>(opt.threads)));
    std::printf("decode-replay: '%s', %zu %s entries of pid %llu from %zu logged threads, replayed on %d threads, "
                "decoder %s, level %s\n",
                clipName.c_str(), entries.size(), decodeLevel ? "decode" : "request",
                static_cast<unsigned long long>(pid), distinctThreads, workers, osv::video::hwAccelName(*hw),
                opt.level.c_str());

    std::vector<ReadOutcome> replayed(entries.size());
    std::map<std::pair<std::uint32_t, std::uint32_t>, ReadOutcome> reference;  // (track, frame) -> outcome
    std::string replayPath;
    double replayMs = 0.0;
    double referenceMs = 0.0;

    if (!decodeLevel) {
        // ---- request level: one importer reader, handed between threads ------------
        auto opened = osv::video::DualStreamReader::open(clip, format.value(), readerOptions(*hw, opt.threads));
        if (!opened.ok()) {
            return fail(opened.error());
        }
        osv::video::DualStreamReader replayReader = std::move(opened).value();
        std::mutex instanceMutex;  // the importer instance's lock
        const auto t0 = std::chrono::steady_clock::now();
        Turnstile::run(entries.size(), workerOf, workers, [&](std::size_t i) noexcept {
            try {
                std::lock_guard<std::mutex> lock(instanceMutex);
                replayed[i] = readPair(replayReader, entries[i].frame);
            } catch (const std::exception& e) {
                replayed[i].error = std::string("exception: ") + e.what();
            } catch (...) {
                replayed[i].error = "unknown exception";
            }
        });
        replayMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const osv::video::HevcStreamDecoder* d = replayReader.decoder(0);
        replayPath = osv::video::hwAccelName(d != nullptr ? d->activeHw() : osv::video::HwAccel::None);

        // ---- reference: software, every frame once, ascending ----------------------
        auto refOpened = osv::video::DualStreamReader::open(clip, format.value(),
                                                            readerOptions(osv::video::HwAccel::None, opt.threads));
        if (!refOpened.ok()) {
            return fail(refOpened.error());
        }
        std::set<std::uint32_t> frames;
        for (const TraceEntry& e : entries) {
            frames.insert(e.frame);
        }
        const auto r0 = std::chrono::steady_clock::now();
        for (const std::uint32_t f : frames) {
            reference[{0u, f}] = readPair(refOpened.value(), f);
        }
        referenceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
    } else {
        // ---- decode level: one decoder per logged track ------------------------------
        std::map<std::uint32_t, std::unique_ptr<osv::video::HevcStreamDecoder>> decoders;
        std::map<std::uint32_t, std::set<std::uint32_t>> framesOf;
        for (const TraceEntry& e : entries) {
            framesOf[e.track].insert(e.frame);
            if (decoders.count(e.track) != 0) {
                continue;
            }
            auto opened = osv::video::HevcStreamDecoder::open(clip, e.track, readerOptions(*hw, opt.threads));
            if (!opened.ok()) {
                return fail(opened.error());
            }
            decoders[e.track] = std::make_unique<osv::video::HevcStreamDecoder>(std::move(opened).value());
        }
        std::mutex decoderMutex;  // one decoder is never used by two threads at once
        const auto t0 = std::chrono::steady_clock::now();
        Turnstile::run(entries.size(), workerOf, workers, [&](std::size_t i) noexcept {
            try {
                std::lock_guard<std::mutex> lock(decoderMutex);
                replayed[i] = readOne(*decoders.at(entries[i].track), entries[i].frame);
            } catch (const std::exception& e) {
                replayed[i].error = std::string("exception: ") + e.what();
            } catch (...) {
                replayed[i].error = "unknown exception";
            }
        });
        replayMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        replayPath = decoders.empty() ? "none" : osv::video::hwAccelName(decoders.begin()->second->activeHw());

        // ---- reference: software, per track, ascending --------------------------------
        const auto r0 = std::chrono::steady_clock::now();
        for (const auto& [track, frames] : framesOf) {
            auto opened = osv::video::HevcStreamDecoder::open(
                clip, track, readerOptions(osv::video::HwAccel::None, opt.threads));
            if (!opened.ok()) {
                return fail(opened.error());
            }
            for (const std::uint32_t f : frames) {
                reference[{track, f}] = readOne(opened.value(), f);
            }
        }
        referenceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
    }

    // ---- compare ---------------------------------------------------------------------
    std::size_t replayOk = 0;
    std::size_t replayFailed = 0;
    std::size_t same = 0;
    std::size_t different = 0;
    std::size_t sameFailures = 0;
    std::size_t held = 0;
    std::size_t loggedCompared = 0;
    std::size_t loggedDiffer = 0;
    std::size_t outcomeDiffer = 0;
    std::optional<std::size_t> firstDifference;
    std::optional<std::size_t> firstFailure;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const TraceEntry& e = entries[i];
        const ReadOutcome& r = replayed[i];
        const ReadOutcome& ref = reference[{decodeLevel ? e.track : 0u, e.frame}];
        (r.ok ? replayOk : replayFailed) += 1;
        if (r.ok && r.index != e.frame) {
            ++held;
        }
        if (!r.ok && !firstFailure) {
            firstFailure = i;
        }
        if (sameOutcome(r, ref, !decodeLevel)) {
            ++same;
            if (!r.ok) {
                ++sameFailures;
            }
        } else {
            ++different;
            if (!firstDifference) {
                firstDifference = i;
            }
        }
        // The session's own record of the same entry.
        if (decodeLevel) {
            if (e.loggedCrc != "device" && r.ok) {
                ++loggedCompared;
                if (e.loggedCrc != hex64(r.crc[0])) {
                    ++loggedDiffer;
                }
            }
        } else if (e.loggedOk != r.ok) {
            ++outcomeDiffer;
        }
    }

    // ---- report ---------------------------------------------------------------------
    std::printf("  replay: %zu ok, %zu failed (%zu served a held frame), decoded on %s, %.0f ms (%.2f ms per entry)\n",
                replayOk, replayFailed, held, replayPath.c_str(), replayMs,
                replayMs / static_cast<double>(entries.size()));
    std::printf("  reference: software, %zu frames in ascending order, %.0f ms\n", reference.size(), referenceMs);
    std::printf("  compared %zu: %zu identical (%zu of them failing identically), %zu different\n", entries.size(),
                same, sameFailures, different);
    if (decodeLevel) {
        std::printf("  logged fingerprints: %zu compared, %zu differ from the replay\n", loggedCompared, loggedDiffer);
    } else {
        std::printf("  logged outcomes: %zu of %zu differ from the replay (delivered vs failed)\n", outcomeDiffer,
                    entries.size());
    }
    const auto describeEntry = [&](std::size_t i) {
        const TraceEntry& e = entries[i];
        const ReadOutcome& r = replayed[i];
        const ReadOutcome& ref = reference[{decodeLevel ? e.track : 0u, e.frame}];
        std::printf("    entry %zu (trace %zu line %zu, tid %llu): %s%u: replay %s, reference %s\n", i, e.file + 1,
                    e.line, static_cast<unsigned long long>(e.tid),
                    decodeLevel ? ("track " + std::to_string(e.track) + " frame ").c_str() : "frame ", e.frame,
                    r.ok ? ("frame " + std::to_string(r.index) + " " + hex64(r.crc[0]) +
                            (decodeLevel ? std::string() : "/" + hex64(r.crc[1])))
                               .c_str()
                         : osv::log::safe(r.error).c_str(),
                    ref.ok ? ("frame " + std::to_string(ref.index) + " " + hex64(ref.crc[0]) +
                              (decodeLevel ? std::string() : "/" + hex64(ref.crc[1])))
                                 .c_str()
                           : osv::log::safe(ref.error).c_str());
        if (!r.state.empty()) {
            std::printf("      decoder state: %s\n", osv::log::safe(r.state).c_str());
        }
    };
    if (firstDifference) {
        std::printf("  first difference:\n");
        describeEntry(*firstDifference);
    } else {
        std::printf("  first difference: none - the replay is bit-exact with the sequential software decode\n");
    }
    if (firstFailure) {
        std::printf("  first failure:\n");
        describeEntry(*firstFailure);
    }

    // ---- CSV --------------------------------------------------------------------------
    if (!opt.csvPath.empty()) {
        std::ofstream csv(pathFromUtf8(opt.csvPath), std::ios::binary | std::ios::trunc);
        if (!csv.good()) {
            return fail(osv::Error{osv::ErrorCode::Io, "cannot write " + osv::log::safe(opt.csvPath)});
        }
        csv << "entry,trace,line,pid,tid,track,frame,logged,logged_crc,replay,replay_frame,replay_crc0,replay_crc1,"
               "reference,reference_frame,reference_crc0,reference_crc1,same,ms,error\n";
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const TraceEntry& e = entries[i];
            const ReadOutcome& r = replayed[i];
            const ReadOutcome& ref = reference[{decodeLevel ? e.track : 0u, e.frame}];
            std::string error = osv::log::safe(r.ok ? std::string() : r.error);
            std::replace(error.begin(), error.end(), ',', ';');
            csv << i << ',' << (e.file + 1) << ',' << e.line << ',' << e.pid << ',' << e.tid << ',' << e.track << ','
                << e.frame << ',' << (decodeLevel ? "decoded" : (e.loggedOk ? "delivered" : "failed")) << ','
                << e.loggedCrc << ',' << (r.ok ? "ok" : "failed") << ',' << r.index << ',' << hex64(r.crc[0]) << ','
                << hex64(r.crc[1]) << ',' << (ref.ok ? "ok" : "failed") << ',' << ref.index << ','
                << hex64(ref.crc[0]) << ',' << hex64(ref.crc[1]) << ',' << (sameOutcome(r, ref, !decodeLevel) ? 1 : 0)
                << ',' << r.ms << ',' << error << '\n';
        }
        if (!csv.good()) {
            return fail(osv::Error{osv::ErrorCode::Io, "writing " + osv::log::safe(opt.csvPath) + " failed"});
        }
        std::printf("  rows: %s\n", osv::log::safe(opt.csvPath).c_str());
    }
    return different == 0 ? kExitOk : kExitRuntime;
}

}  // namespace

// =============================================================================
//  Registration
// =============================================================================
void registerDecodeReplayCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<ReplayOptions>();
    CLI::App* sub = app.add_subcommand(
        "decode-replay",
        "Replay the frame requests a plug-in log recorded through the decoder and compare every picture with a "
        "sequential software decode");
    sub->add_option("file", opt->inputPath, "The clip the log is about (.OSV, .LRF or a dual-track MP4)")->required();
    sub->add_option("--trace", opt->tracePaths,
                    "Plug-in log(s) at Debug level, e.g. %LOCALAPPDATA%\\OpenOSV\\OpenOSVImporter.log.1 then .log "
                    "(repeat the option, oldest first)")
        ->required();
#if defined(__APPLE__)
    sub->add_option("--hw", opt->hw, "Decoder of the replay: none, videotoolbox")->capture_default_str();
#else
    sub->add_option("--hw", opt->hw, "Decoder of the replay: none, d3d11va, cuda (FFmpeg NVDEC, host copies)")
        ->capture_default_str();
#endif
    sub->add_option("--level", opt->level,
                    "request: the frames the host asked for, read as lens pairs by one importer reader; decode: every "
                    "logged 'decode:' picture, per track")
        ->capture_default_str();
    sub->add_option("--from", opt->from, "First source frame kept (default: all)");
    sub->add_option("--to", opt->to, "Last source frame kept (default: all)");
    sub->add_option("--pid", opt->pid, "Session (process id) to replay; default the clip's last, 0 = every session");
    sub->add_option("--threads", opt->threads, "Replay threads and decoder threads (1-16)")->capture_default_str();
    sub->add_option("--max", opt->maxEntries, "Stop after this many trace entries (0 = all)");
    sub->add_option("--tracks", opt->tracks, "Lens track ids A,B for a file without camera metadata");
    sub->add_option("--csv", opt->csvPath, "Write one row per replayed entry");
    sub->callback([opt, &ctx]() { ctx.exitCode = runReplay(*opt); });
}

}  // namespace osvtool
