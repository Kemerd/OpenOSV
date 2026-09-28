// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Queue.cpp - the clip list and the folder scan (see Queue.h).

#include "Queue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iterator>
#include <system_error>
#include <unordered_set>

namespace osvgui {

namespace fs = std::filesystem;

// ===========================================================================
//  Paths as UTF-8
// ===========================================================================

std::string pathToUtf8(const fs::path& path) noexcept {
    try {
        // u8string() converts from the native encoding (UTF-16 on Windows)
        // without going through the ANSI code page, so every name survives.
        const std::u8string text = path.u8string();
        return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    } catch (...) {
        return "?";
    }
}

fs::path pathFromUtf8(std::string_view utf8) noexcept {
    try {
        // A char8_t string tells std::filesystem the bytes ARE UTF-8; a plain
        // std::string would be read in the ANSI code page on Windows.
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    } catch (...) {
        return {};
    }
}

namespace {

/// ASCII lower-case of one native path character; non-ASCII passes through.
template <typename Char> [[nodiscard]] constexpr Char asciiLower(Char c) noexcept {
    return (c >= Char('A') && c <= Char('Z')) ? static_cast<Char>(c - Char('A') + Char('a')) : c;
}

/// True when the path's extension is `wanted` (lower-case, with the dot),
/// compared without regard to ASCII case.
[[nodiscard]] bool hasExtension(const fs::path& path, std::string_view wanted) noexcept {
    try {
        using Char = fs::path::value_type;  // wchar_t on Windows, char elsewhere
        const fs::path::string_type ext = path.extension().native();
        if (ext.size() != wanted.size()) {
            return false;
        }
        for (std::size_t i = 0; i < ext.size(); ++i) {
            // Every character of `wanted` is ASCII, so a wide character above
            // 0x7F can never match and the comparison stays exact.
            if (asciiLower(ext[i]) != static_cast<Char>(wanted[i])) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

/// The key two paths are compared by for de-duplication: absolute and
/// lexically normal, and on Windows (case-insensitive file systems) folded
/// to lower case so "D:\DCIM\a.OSV" and "d:\dcim\A.osv" are one clip.
[[nodiscard]] fs::path::string_type identityKey(const fs::path& path) noexcept {
    try {
        std::error_code ec;
        fs::path absolute = fs::absolute(path, ec);
        if (ec) {
            absolute = path;
        }
        fs::path::string_type key = absolute.lexically_normal().native();
#if defined(_WIN32)
        for (auto& c : key) {
            c = asciiLower(c);
        }
#endif
        return key;
    } catch (...) {
        return {};
    }
}

/// Look at one file found by the scan: a clip goes into `out`, anything
/// else is counted.  `dropped` says whether the user dropped this very file
/// (a stray .txt then counts as "skipped"; inside a folder it is ignored).
void considerFile(const fs::path& file, bool dropped, std::vector<fs::path>& out, ScanResult& result) {
    if (isProxyClip(file)) {
        ++result.skippedProxies;
        return;
    }
    if (!isOsvClip(file)) {
        if (dropped) {
            ++result.skippedOther;
        }
        return;
    }
    // Zero-byte clips are what an interrupted card copy leaves behind: osvtool
    // could only fail on them, so they never enter the queue.
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec || size == 0) {
        ++result.skippedEmpty;
        return;
    }
    out.push_back(file);
}

}  // namespace

// ===========================================================================
//  Status
// ===========================================================================

const char* statusName(ClipStatus status) noexcept {
    switch (status) {
    case ClipStatus::Queued:
        return "Queued";
    case ClipStatus::Rendering:
        return "Rendering";
    case ClipStatus::Done:
        return "Done";
    case ClipStatus::Failed:
        return "Failed";
    case ClipStatus::Stopped:
        return "Stopped";
    case ClipStatus::Skipped:
        return "Skipped";
    }
    return "Unknown";
}

bool isFinished(ClipStatus status) noexcept {
    return status == ClipStatus::Done || status == ClipStatus::Failed || status == ClipStatus::Stopped ||
           status == ClipStatus::Skipped;
}

// ===========================================================================
//  Scan
// ===========================================================================

bool isOsvClip(const fs::path& path) noexcept {
    try {
        // "._CAM_0001.OSV": macOS's AppleDouble companion on exFAT/FAT cards.
        // It has the clip's extension but holds a few KB of Finder metadata.
        const std::string name = pathToUtf8(path.filename());
        if (name.size() >= 2 && name[0] == '.' && name[1] == '_') {
            return false;
        }
        return hasExtension(path, ".osv");
    } catch (...) {
        return false;
    }
}

bool isProxyClip(const fs::path& path) noexcept {
    return hasExtension(path, ".lrf");
}

ScanResult scanPaths(const std::vector<fs::path>& inputs, std::size_t maxClips) noexcept {
    ScanResult result;
    try {
        for (const fs::path& input : inputs) {
            if (result.clips.size() >= maxClips) {
                result.truncated = true;
                break;
            }
            if (input.empty()) {
                continue;
            }

            // ---- what the path is ------------------------------------------------
            std::error_code ec;
            const fs::file_status st = fs::status(input, ec);
            if (ec || !fs::exists(st)) {
                result.problems.push_back("Not found: " + pathToUtf8(input));
                continue;
            }

            // ---- a single file ---------------------------------------------------
            if (!fs::is_directory(st)) {
                if (fs::is_regular_file(st)) {
                    considerFile(input, true, result.clips, result);
                } else {
                    ++result.skippedOther;
                }
                continue;
            }

            // ---- a folder, all the way down ----------------------------------------
            // skip_permission_denied: a sub-folder the user may not read (a
            // "System Volume Information" on a card) is passed over, not fatal.
            // Symlinked folders are not followed, so a loop cannot hang the scan.
            std::vector<fs::path> found;
            fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
            if (ec) {
                result.problems.push_back("Can't open " + pathToUtf8(input) + ": " + ec.message());
                continue;
            }
            const fs::recursive_directory_iterator end;
            while (it != end) {
                std::error_code entryEc;
                if (it->is_regular_file(entryEc) && !entryEc) {
                    considerFile(it->path(), false, found, result);
                    if (result.clips.size() + found.size() >= maxClips) {
                        result.truncated = true;
                        break;
                    }
                }
                it.increment(ec);
                if (ec) {
                    // The listing broke off (card pulled, network share gone):
                    // keep what was found so far and say so.
                    result.problems.push_back("Stopped reading " + pathToUtf8(input) + ": " + ec.message());
                    break;
                }
            }

            // Directory order is whatever the file system returns; clips are
            // queued in name order, which for DJI's names is shooting order.
            std::sort(found.begin(), found.end());
            result.clips.insert(result.clips.end(), found.begin(), found.end());
        }
    } catch (const std::exception& e) {
        result.problems.push_back(std::string("Scan failed: ") + e.what());
    } catch (...) {
        result.problems.push_back("Scan failed.");
    }
    return result;
}

// ===========================================================================
//  The queue
// ===========================================================================

std::size_t ClipQueue::add(const std::vector<fs::path>& clips) noexcept {
    std::size_t added = 0;
    try {
        // Keys of what is queued already, then of what this call adds, so a
        // clip listed twice in one drop is also added once.
        std::unordered_set<fs::path::string_type> known;
        known.reserve(m_items.size() + clips.size());
        for (const ClipItem& item : m_items) {
            known.insert(identityKey(item.path));
        }
        for (const fs::path& clip : clips) {
            if (clip.empty()) {
                continue;
            }
            const auto key = identityKey(clip);
            if (key.empty() || !known.insert(key).second) {
                continue;
            }
            ClipItem item;
            item.id = m_nextId++;
            item.path = clip;
            item.name = pathToUtf8(clip.filename());
            item.folder = pathToUtf8(clip.parent_path());
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(clip, ec);
            item.sizeBytes = ec ? 0 : size;
            m_items.push_back(std::move(item));
            ++added;
        }
    } catch (...) {
        // Out of memory half way: what was added stays added.
    }
    return added;
}

bool ClipQueue::remove(std::uint64_t id) noexcept {
    const auto it = std::find_if(m_items.begin(), m_items.end(), [id](const ClipItem& c) { return c.id == id; });
    if (it == m_items.end()) {
        return false;
    }
    m_items.erase(it);
    return true;
}

void ClipQueue::clear() noexcept {
    m_items.clear();
}

std::size_t ClipQueue::removeFinished() noexcept {
    const std::size_t before = m_items.size();
    m_items.erase(
        std::remove_if(m_items.begin(), m_items.end(), [](const ClipItem& c) { return isFinished(c.status); }),
        m_items.end());
    return before - m_items.size();
}

bool ClipQueue::move(std::size_t from, std::size_t to) noexcept {
    if (from >= m_items.size() || to >= m_items.size()) {
        return false;
    }
    if (from == to) {
        return true;
    }
    // A rotate keeps every other clip's relative order, which is what a drag
    // in the list means.
    if (from < to) {
        std::rotate(m_items.begin() + static_cast<std::ptrdiff_t>(from),
                    m_items.begin() + static_cast<std::ptrdiff_t>(from) + 1,
                    m_items.begin() + static_cast<std::ptrdiff_t>(to) + 1);
    } else {
        std::rotate(m_items.begin() + static_cast<std::ptrdiff_t>(to),
                    m_items.begin() + static_cast<std::ptrdiff_t>(from),
                    m_items.begin() + static_cast<std::ptrdiff_t>(from) + 1);
    }
    return true;
}

ClipItem* ClipQueue::find(std::uint64_t id) noexcept {
    for (ClipItem& item : m_items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

const ClipItem* ClipQueue::find(std::uint64_t id) const noexcept {
    for (const ClipItem& item : m_items) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

ClipItem* ClipQueue::firstQueued() noexcept {
    for (ClipItem& item : m_items) {
        if (item.status == ClipStatus::Queued) {
            return &item;
        }
    }
    return nullptr;
}

const ClipItem* ClipQueue::firstQueued() const noexcept {
    for (const ClipItem& item : m_items) {
        if (item.status == ClipStatus::Queued) {
            return &item;
        }
    }
    return nullptr;
}

std::size_t ClipQueue::count(ClipStatus status) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(m_items.begin(), m_items.end(), [status](const ClipItem& c) { return c.status == status; }));
}

std::uintmax_t ClipQueue::totalBytes() const noexcept {
    std::uintmax_t total = 0;
    for (const ClipItem& item : m_items) {
        total += item.sizeBytes;
    }
    return total;
}

// ===========================================================================
//  Formatting
// ===========================================================================

std::string formatBytes(std::uintmax_t bytes) noexcept {
    try {
        // Decimal units: a 4 GB clip reads "4.0 GB" here and in Explorer's
        // details pane alike (Explorer's "size" column is binary, its
        // properties dialog both; decimal is what the camera box says).
        static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
        double value = static_cast<double>(bytes);
        std::size_t unit = 0;
        while (value >= 1000.0 && unit + 1 < std::size(kUnits)) {
            value /= 1000.0;
            ++unit;
        }
        char buf[32];
        if (unit == 0) {
            std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
        } else if (value < 10.0) {
            std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
        } else {
            std::snprintf(buf, sizeof(buf), "%.0f %s", value, kUnits[unit]);
        }
        return buf;
    } catch (...) {
        return "?";
    }
}

std::string formatDuration(double seconds) noexcept {
    try {
        if (!std::isfinite(seconds) || seconds < 0.0) {
            return "--";
        }
        // Round to whole seconds once, so 59.6 s reads "1:00" and never "0:60".
        const auto total = static_cast<long long>(std::llround(seconds));
        const long long h = total / 3600;
        const long long m = (total % 3600) / 60;
        const long long s = total % 60;
        char buf[40];
        if (total < 60) {
            std::snprintf(buf, sizeof(buf), "%lld s", s);
        } else if (h == 0) {
            std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
        } else {
            std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
        }
        return buf;
    } catch (...) {
        return "--";
    }
}

}  // namespace osvgui
