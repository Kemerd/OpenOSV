// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Queue.h - OpenOSV Studio's render queue: the clips, their state, and the
// folder scan that turns whatever was dropped on the window into clips.
//
// Pure C++ (std::filesystem only, no UI, no process code), so the unit tests
// drive it on every platform.  Every entry point is noexcept: a filesystem
// error (an unplugged card, a folder without read access) becomes a message
// in the scan result, never an exception into the UI loop.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace osvgui {

// ===========================================================================
//  Paths as UTF-8
// ===========================================================================

/// The path as UTF-8 text (what ImGui draws and the settings file stores).
/// Never throws: an unconvertible path comes back as "?".
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path) noexcept;

/// A path from UTF-8 text (drops, the settings file, the command line).
/// Never throws: an allocation failure gives an empty path.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8) noexcept;

// ===========================================================================
//  Clip items
// ===========================================================================

/// Where one clip is in its life in the queue.
enum class ClipStatus : std::uint8_t {
    Queued,     ///< Waiting for its turn.
    Rendering,  ///< osvtool is working on it right now.
    Done,       ///< Rendered; `output` holds the file.
    Failed,     ///< osvtool failed; `message` holds its error line.
    Stopped,    ///< The user stopped it half way.
    Skipped,    ///< Not rendered (its output already existed).
};

/// A short, human word for a status ("Queued", "Done", ...).
[[nodiscard]] const char* statusName(ClipStatus status) noexcept;

/// True for the states a batch has finished with (done, failed, stopped,
/// skipped): what "Clear finished" removes.
[[nodiscard]] bool isFinished(ClipStatus status) noexcept;

/// One clip in the queue.
struct ClipItem {
    std::uint64_t id = 0;          ///< Stable identity for the UI (never reused in a session).
    std::filesystem::path path;    ///< The .OSV itself.
    std::string name;              ///< File name, UTF-8.
    std::string folder;            ///< Parent folder, UTF-8.
    std::uintmax_t sizeBytes = 0;  ///< File size when it was added.
    ClipStatus status = ClipStatus::Queued;

    // ---- progress: live while rendering, the last values afterwards --------
    std::uint32_t framesDone = 0;   ///< Frames osvtool has finished.
    std::uint32_t framesTotal = 0;  ///< Frames it will render (0 = not known yet).
    double fps = 0.0;               ///< Average render speed so far.
    std::string device;             ///< Renderer osvtool reported ("cuda", "opencl", "cpu").
    double elapsedSec = 0.0;        ///< Wall time spent on it.
    std::string message;            ///< Error line (failed), reason (skipped), or empty.
    std::filesystem::path output;   ///< The file it was (or is being) rendered to.
};

// ===========================================================================
//  Folder scan
// ===========================================================================

/// True when `path` names a DJI .OSV clip by its extension (any case), and
/// is not one of the "._name" AppleDouble companions macOS leaves on exFAT
/// cards (they carry the same extension but hold only Finder metadata).
[[nodiscard]] bool isOsvClip(const std::filesystem::path& path) noexcept;

/// True for the camera's low-resolution proxy (.LRF, any case).
[[nodiscard]] bool isProxyClip(const std::filesystem::path& path) noexcept;

/// What a scan found.
struct ScanResult {
    std::vector<std::filesystem::path> clips;  ///< .OSV files, sorted per dropped folder.
    std::vector<std::string> problems;         ///< Human-readable notes (missing path, unreadable folder).
    std::size_t skippedProxies = 0;            ///< .LRF files passed over.
    std::size_t skippedEmpty = 0;              ///< Zero-byte .OSV files passed over.
    std::size_t skippedOther = 0;              ///< Dropped files that are not clips.
    bool truncated = false;                    ///< Stopped at `maxClips`.
};

/// Turn dropped or picked paths into clips.  A folder contributes every
/// .OSV below it (recursively, symlinked folders not followed so a loop can
/// never hang the scan); a file contributes itself when it is a clip.  .LRF
/// proxies and zero-byte clips are skipped and counted.  Stops after
/// `maxClips` clips.  Never throws.
[[nodiscard]] ScanResult scanPaths(const std::vector<std::filesystem::path>& inputs,
                                   std::size_t maxClips = 20000) noexcept;

// ===========================================================================
//  The queue
// ===========================================================================

/// The ordered list of clips.  Owned and touched by the UI thread only.
class ClipQueue {
public:
    /// Append clips that are not in the queue yet (the same file dropped
    /// twice is one entry; paths compare case-insensitively on Windows).
    /// Returns how many were added.
    std::size_t add(const std::vector<std::filesystem::path>& clips) noexcept;

    /// Remove one clip by id.  False when there is no such clip.
    bool remove(std::uint64_t id) noexcept;

    /// Remove every clip.
    void clear() noexcept;

    /// Remove every finished clip (done, failed, stopped, skipped); returns
    /// how many went.
    std::size_t removeFinished() noexcept;

    /// Move the clip at `from` to position `to` (both indices into items()).
    /// False when either index is out of range.
    bool move(std::size_t from, std::size_t to) noexcept;

    /// The clip with this id, or null.
    [[nodiscard]] ClipItem* find(std::uint64_t id) noexcept;
    [[nodiscard]] const ClipItem* find(std::uint64_t id) const noexcept;

    /// The first clip still waiting, or null.
    [[nodiscard]] ClipItem* firstQueued() noexcept;
    [[nodiscard]] const ClipItem* firstQueued() const noexcept;

    /// How many clips are in `status`.
    [[nodiscard]] std::size_t count(ClipStatus status) const noexcept;

    /// Sum of the file sizes of every clip.
    [[nodiscard]] std::uintmax_t totalBytes() const noexcept;

    [[nodiscard]] const std::vector<ClipItem>& items() const noexcept { return m_items; }
    [[nodiscard]] std::vector<ClipItem>& items() noexcept { return m_items; }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }

private:
    std::vector<ClipItem> m_items;
    std::uint64_t m_nextId = 1;
};

// ===========================================================================
//  Formatting helpers shared by the UI and the log
// ===========================================================================

/// "812 KB", "4.2 GB" - decimal units, as Explorer and Finder show them.
[[nodiscard]] std::string formatBytes(std::uintmax_t bytes) noexcept;

/// "12 s", "4:05", "1:02:09" (negative or non-finite = "--").
[[nodiscard]] std::string formatDuration(double seconds) noexcept;

}  // namespace osvgui
