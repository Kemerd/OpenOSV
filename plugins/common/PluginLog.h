// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PluginLog: the plug-in side logger.
//
// Inside Premiere there is no console, so every message goes to a file in
// %LOCALAPPDATA%\OpenOSV\<plugin>.log and, in the same call, to
// OutputDebugStringW so a debugger / DebugView shows it live.  Properties:
//
//   * thread-safe (one mutex around the file write);
//   * never throws: formatting errors, a missing directory or a full disk
//     degrade to "message dropped", never to an exception crossing the host;
//   * UTF-8 on disk; the debugger copy is converted to UTF-16;
//   * every line carries a timestamp with milliseconds, the level and the
//     thread id (Premiere calls importers from many threads);
//   * rotates when the file exceeds 4 MB (renamed to <plugin>.log.1, one
//     generation kept);
//   * once(key, ...) logs a message the first time only, for per-selector
//     "unsupported" notices that would otherwise repeat thousands of times.
//
// init() is idempotent and cheap; the plug-in entry points call it before
// their first log line and shutdown() from imShutdown / effect global
// setdown.  Logging before init() (or after shutdown()) still reaches
// OutputDebugStringW.
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace osv::premiere {

class PluginLog {
public:
    /// Severity levels, ascending.
    enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

    /// Open (or create) %LOCALAPPDATA%\OpenOSV\<pluginName>.log.  Safe to
    /// call repeatedly; later calls with a different name switch files.
    /// Returns true when the file could be opened.
    static bool init(std::wstring_view pluginName) noexcept;

    /// Flush and close the file.  Logging afterwards only reaches the
    /// debugger until init() is called again.
    static void shutdown() noexcept;

    /// Minimum level that is written (default Info; Debug when the
    /// environment variable OSV_PLUGIN_LOG_LEVEL is "debug" or "trace").
    static void setLevel(Level level) noexcept;
    [[nodiscard]] static Level level() noexcept;
    [[nodiscard]] static bool enabled(Level level) noexcept;

    /// Write one already formatted line.
    static void write(Level level, std::string_view text) noexcept;

    /// Write a message only the first time `key` is seen.  Returns true when
    /// the message was written.
    static bool once(std::string_view key, Level level, std::string_view text) noexcept;

    /// Path of the current log file (empty before init()).
    [[nodiscard]] static std::wstring filePath() noexcept;

    /// Size in bytes at which the file is rotated.
    static constexpr unsigned long long kRotateBytes = 4ull * 1024ull * 1024ull;

    // ---- formatted helpers -------------------------------------------------
    template <class... Args>
    static void logf(Level level, std::format_string<Args...> fmt, Args&&... args) noexcept {
        if (!enabled(level)) {
            return;
        }
        write(level, safeFormat(fmt, std::forward<Args>(args)...));
    }
    template <class... Args>
    static void trace(std::format_string<Args...> fmt, Args&&... args) noexcept {
        logf(Level::Trace, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    static void debug(std::format_string<Args...> fmt, Args&&... args) noexcept {
        logf(Level::Debug, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    static void info(std::format_string<Args...> fmt, Args&&... args) noexcept {
        logf(Level::Info, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    static void warn(std::format_string<Args...> fmt, Args&&... args) noexcept {
        logf(Level::Warn, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    static void error(std::format_string<Args...> fmt, Args&&... args) noexcept {
        logf(Level::Error, fmt, std::forward<Args>(args)...);
    }

    /// Formatted variant of once().
    template <class... Args>
    static bool oncef(std::string_view key, Level level, std::format_string<Args...> fmt, Args&&... args) noexcept {
        if (!enabled(level)) {
            return false;
        }
        return once(key, level, safeFormat(fmt, std::forward<Args>(args)...));
    }

    /// Short name of a level ("INFO"), for callers that build their own
    /// lines.
    [[nodiscard]] static const char* levelName(Level level) noexcept;

private:
    /// std::format that turns any exception (bad_alloc, format_error) into
    /// a placeholder string instead of propagating.
    template <class... Args>
    static std::string safeFormat(std::format_string<Args...> fmt, Args&&... args) noexcept {
        try {
            return std::format(fmt, std::forward<Args>(args)...);
        } catch (...) {
            return std::string("<log format error>");
        }
    }
};

}  // namespace osv::premiere
