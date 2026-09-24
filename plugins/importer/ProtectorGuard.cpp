// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProtectorGuard implementation: file-identity key, memory + disk cache,
// and the one-frame measurement (see the header for the policy).

#include "ProtectorGuard.h"

#include "HostContext.h"
#include "NumberParse.h"
#include "PluginLog.h"

#include "osv/render/LensProtectorCheck.h"
#include "osv/video/DualStreamReader.h"

#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace osv::premiere {

namespace {

// -----------------------------------------------------------------------------
//  Cache
// -----------------------------------------------------------------------------

/// One remembered verdict.
struct Verdict {
    geom::ProtectorDirection direction = geom::ProtectorDirection::Forward;
    std::array<double, 3> ncc{};
};

/// Identity of a clip file: a changed size or modification time is a
/// different file as far as the verdict is concerned.
struct FileKey {
    std::string path;          ///< Absolute path, UTF-8, ASCII lower-cased (Windows paths are case-insensitive).
    std::uintmax_t size = 0;   ///< Bytes.
    long long mtime = 0;       ///< last_write_time ticks.

    [[nodiscard]] bool operator<(const FileKey& o) const noexcept {
        if (path != o.path) {
            return path < o.path;
        }
        if (size != o.size) {
            return size < o.size;
        }
        return mtime < o.mtime;
    }
};

/// Everything shared between instances, behind one mutex.  The mutex is
/// also held for a whole measurement, so two instances of the same clip
/// opening together decode it once (the second finds the first's verdict).
struct GuardState {
    std::mutex mutex;
    std::map<FileKey, Verdict> verdicts;
    bool diskLoaded = false;
};

GuardState& state() {
    static GuardState s;
    return s;
}

/// The file identity of `path`; nullopt when the file cannot be inspected.
std::optional<FileKey> keyFor(const std::filesystem::path& path) noexcept {
    try {
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(path, ec);
        if (ec) {
            return std::nullopt;
        }
        FileKey key;
        const std::u8string u8 = abs.u8string();
        key.path.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
        for (char& c : key.path) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        key.size = std::filesystem::file_size(abs, ec);
        if (ec) {
            return std::nullopt;
        }
        const auto t = std::filesystem::last_write_time(abs, ec);
        if (ec) {
            return std::nullopt;
        }
        key.mtime = static_cast<long long>(t.time_since_epoch().count());
        return key;
    } catch (...) {
        return std::nullopt;
    }
}

/// The on-disk cache path (next to the plug-in log); empty when the log has
/// no file (then only the memory cache is used).
std::filesystem::path diskCachePath() noexcept {
    try {
        const std::wstring log = PluginLog::filePath();
        if (log.empty()) {
            return {};
        }
        return std::filesystem::path(log).parent_path() / kProtectorGuardCacheFile;
    } catch (...) {
        return {};
    }
}

/// Parse a whole string as a number (integer or floating point); false on
/// anything but a complete, in-range parse.
template <class T>
bool parseNumber(const std::string& text, T& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = first + text.size();
    // std::from_chars, or its locale-free twin where the standard library
    // has no floating-point from_chars (NumberParse.h).
    return parseWholeNumber(first, last, out);
}

/// Parse a direction token written by appendDisk().
std::optional<geom::ProtectorDirection> parseDirection(const std::string& token) noexcept {
    if (token == "none") {
        return geom::ProtectorDirection::None;
    }
    if (token == "forward") {
        return geom::ProtectorDirection::Forward;
    }
    if (token == "inverse") {
        return geom::ProtectorDirection::Inverse;
    }
    return std::nullopt;
}

/// Load every well-formed line of the disk cache into `s.verdicts`.  Lines:
///   1 <TAB> size <TAB> mtime <TAB> direction <TAB> ncc0 <TAB> ncc1 <TAB> ncc2 <TAB> path
/// Anything malformed (a truncated last line, a future version) is skipped.
void loadDisk(GuardState& s) noexcept {
    s.diskLoaded = true;
    try {
        const std::filesystem::path file = diskCachePath();
        if (file.empty()) {
            return;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in.good()) {
            return;  // No cache yet.
        }
        std::string line;
        std::size_t loaded = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::vector<std::string> f;
            std::size_t start = 0;
            for (int i = 0; i < 7; ++i) {
                const std::size_t tab = line.find('\t', start);
                if (tab == std::string::npos) {
                    break;
                }
                f.push_back(line.substr(start, tab - start));
                start = tab + 1;
            }
            if (f.size() != 7 || f[0] != "1" || start >= line.size()) {
                continue;
            }
            const auto dir = parseDirection(f[3]);
            if (!dir) {
                continue;
            }
            // Numbers through from_chars: locale independent (a host that
            // set a comma decimal locale must not break the cache) and
            // exception free, so one damaged line skips only itself.
            FileKey key;
            key.path = line.substr(start);
            Verdict v;
            v.direction = *dir;
            bool ok = parseNumber(f[1], key.size) && parseNumber(f[2], key.mtime);
            for (int i = 0; ok && i < 3; ++i) {
                ok = parseNumber(f[static_cast<std::size_t>(4 + i)], v.ncc[static_cast<std::size_t>(i)]);
            }
            if (!ok) {
                continue;
            }
            s.verdicts[key] = v;  // Later lines win (a re-measured file).
            ++loaded;
        }
        PluginLog::debug("lens protector guard: {} cached verdict(s) loaded", loaded);
    } catch (...) {
        // A damaged cache only costs a re-measurement.
        PluginLog::warn("lens protector guard: the cache file could not be read; clips will be re-measured");
    }
}

/// Append one verdict to the disk cache.  Failures are logged, not fatal.
void appendDisk(const FileKey& key, const Verdict& v) noexcept {
    try {
        const std::filesystem::path file = diskCachePath();
        if (file.empty()) {
            return;
        }
        std::ofstream out(file, std::ios::binary | std::ios::app);
        if (!out.good()) {
            PluginLog::warn("lens protector guard: cannot write the cache file");
            return;
        }
        // to_chars, like from_chars on the way in: the file must not depend
        // on the host's locale.
        std::string text = "1\t" + std::to_string(key.size) + '\t' + std::to_string(key.mtime) + '\t' +
                           geom::protectorDirectionName(v.direction);
        for (const double x : v.ncc) {
            char buf[48] = {};
            const auto r = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::fixed, 6);
            text += '\t';
            text.append(buf, r.ec == std::errc{} ? r.ptr : buf);
        }
        text += '\t' + key.path + '\n';
        out << text;
    } catch (...) {
        PluginLog::warn("lens protector guard: cannot write the cache file");
    }
}

// -----------------------------------------------------------------------------
//  Measurement
// -----------------------------------------------------------------------------

/// Decode frame 0 of `path` into host planes: D3D11VA (copied back) first,
/// software second.  The reader is local, so nothing of the clip's own
/// decoder state is touched.
Result<std::pair<std::unique_ptr<video::DualStreamReader>, video::FramePair>> decodeFirstFrame(
    const std::filesystem::path& path, const meta::FormatInfo& format) {
    Error last{ErrorCode::Decoder, "no decoder could be opened"};
    for (const video::HwAccel hw : {video::kHostFrameHwAccel, video::HwAccel::None}) {
        video::DecoderOptions opt;
        opt.hw = hw;
        opt.keepOnDevice = false;  // the band analysis reads host planes
        auto reader = video::DualStreamReader::open(path, format, opt);
        if (!reader.ok()) {
            last = reader.error();
            continue;
        }
        auto owned = std::make_unique<video::DualStreamReader>(std::move(reader).value());
        auto pair = owned->read(0);
        if (!pair.ok()) {
            last = pair.error();
            continue;
        }
        if (!pair.value().valid()) {
            last = Error{ErrorCode::Decoder, "frame 0 has no host planes"};
            continue;
        }
        // The planes are owned by the frames (shared owners), but the reader
        // is returned too so nothing it might still reference is freed early.
        return std::make_pair(std::move(owned), std::move(pair).value());
    }
    return last;
}

}  // namespace

ProtectorGuardResult resolveProtectorGuard(const std::filesystem::path& path, const meta::FormatInfo& format,
                                           const geom::LensRig& baseRig,
                                           const geom::BlendParams& baseBlend) noexcept {
    ProtectorGuardResult result;
    try {
        const std::optional<FileKey> key = keyFor(path);
        GuardState& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);

        // ---- cache ---------------------------------------------------------------
        if (!s.diskLoaded) {
            loadDisk(s);
        }
        if (key) {
            const auto it = s.verdicts.find(*key);
            if (it != s.verdicts.end()) {
                result.direction = it->second.direction;
                result.ncc = it->second.ncc;
                result.measured = true;
                result.fromCache = true;
                result.summary = std::format("none {:.4f}, forward {:.4f}, inverse {:.4f} -> {} (cached)",
                                             result.ncc[0], result.ncc[1], result.ncc[2],
                                             geom::protectorDirectionName(result.direction));
                return result;
            }
        }

        // ---- measure on frame 0 ----------------------------------------------------
        const auto t0 = std::chrono::steady_clock::now();
        auto decoded = decodeFirstFrame(path, format);
        if (!decoded.ok()) {
            result.direction = geom::ProtectorDirection::Forward;
            result.summary = std::format("forward, unverified: frame 0 could not be decoded ({})",
                                         decoded.error().message);
            return result;  // Not cached: a later open can still verify it.
        }
        std::shared_ptr<ThreadPool> pool = HostContext::instance().threadPoolShared();
        if (!pool) {
            result.summary = "forward, unverified: no thread pool";
            return result;
        }
        auto scores = render::scoreLensProtector(baseRig, baseBlend, decoded.value().second, *pool,
                                                 geom::ProtectorDirection::Forward);
        result.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!scores.ok()) {
            result.summary = std::format("forward, unverified: the overlap could not be scored ({})",
                                         scores.error().message);
            return result;
        }
        result.direction = scores.value().pick;
        result.ncc = scores.value().ncc;
        result.measured = true;
        result.summary = std::format("{} (measured on frame 0 in {:.0f} ms)", scores.value().summary, result.millis);

        // ---- remember it ---------------------------------------------------------------
        if (key) {
            Verdict v;
            v.direction = result.direction;
            v.ncc = result.ncc;
            s.verdicts[*key] = v;
            appendDisk(*key, v);
        }
        return result;
    } catch (...) {
        result.direction = geom::ProtectorDirection::Forward;
        result.measured = false;
        result.summary = "forward, unverified: the guard failed unexpectedly";
        return result;
    }
}

}  // namespace osv::premiere
