// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PluginLogPosix.cpp - PluginLog on macOS (the Windows implementation is
// PluginLog.cpp; exactly one of the two is compiled, see
// plugins/common/CMakeLists.txt).
//
// Same contract as the Windows file, platform by platform:
//
//   file       ~/Library/Logs/OpenOSV/<plugin>.log, where Console.app lists
//              every application's logs (Windows: %LOCALAPPDATA%\OpenOSV)
//   live copy  the unified log (os_log, subsystem "com.openosv.plugins"),
//              visible in Console.app and `log stream` the way DebugView
//              shows OutputDebugString on Windows
//   sharing    POSIX has no share modes: Premiere Pro and Media Encoder
//              append to the same file side by side (O_APPEND keeps each
//              line whole) and anything can read it at any time
//   levels     OSV_PLUGIN_LOG_LEVEL, as on Windows
//   rotation   at 4 MB to <plugin>.log.1, one generation kept
//
// Lines carry the pid and thread id for the same reason as on Windows:
// several host processes write to one file.

#include "PluginLog.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_set>

#if defined(__APPLE__)
#include <os/log.h>
#include <pthread.h>
#endif

namespace osv::premiere {

namespace {

/// Everything mutable lives in one function-local static, as on Windows.
struct LogState {
    std::mutex mutex;
    FILE* file = nullptr;
    std::wstring path;          ///< For filePath(); the API is wide on every platform.
    std::string pathUtf8;       ///< What is actually opened.
    std::wstring pluginName;
    unsigned long long bytesWritten = 0;
    std::unordered_set<std::string> onceKeys;
};

LogState& state() {
    static LogState s;
    return s;
}

/// The level is read on every call without taking the mutex.
std::atomic<int> g_level{static_cast<int>(PluginLog::Level::Info)};
std::atomic<bool> g_levelInitialised{false};

/// Apply OSV_PLUGIN_LOG_LEVEL once (debug / trace / info / warn / error / off).
void applyEnvironmentLevel() noexcept {
    bool expected = false;
    if (!g_levelInitialised.compare_exchange_strong(expected, true)) {
        return;
    }
    const char* raw = std::getenv("OSV_PLUGIN_LOG_LEVEL");
    if (!raw || !*raw) {
        return;
    }
    std::string value;
    for (const char* p = raw; *p && value.size() < 31; ++p) {
        const char c = *p;
        value.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (value == "trace") {
        g_level.store(static_cast<int>(PluginLog::Level::Trace));
    } else if (value == "debug") {
        g_level.store(static_cast<int>(PluginLog::Level::Debug));
    } else if (value == "info") {
        g_level.store(static_cast<int>(PluginLog::Level::Info));
    } else if (value == "warn" || value == "warning") {
        g_level.store(static_cast<int>(PluginLog::Level::Warn));
    } else if (value == "error") {
        g_level.store(static_cast<int>(PluginLog::Level::Error));
    } else if (value == "off") {
        g_level.store(static_cast<int>(PluginLog::Level::Off));
    }
}

/// UTF-32 wide string -> UTF-8 (plug-in names are ASCII; this is exact for
/// any code point anyway).
std::string toUtf8(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size());
    for (const wchar_t wc : wide) {
        const auto cp = static_cast<std::uint32_t>(wc);
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

/// ~/Library/Logs/OpenOSV (macOS) or $XDG_STATE_HOME / ~/.local/state
/// /openosv elsewhere, created when missing.  Empty on failure.
std::string logDirectory() noexcept {
    try {
        const char* home = std::getenv("HOME");
        if (!home || !*home) {
            return {};
        }
#if defined(__APPLE__)
        std::filesystem::path dir = std::filesystem::path(home) / "Library" / "Logs" / "OpenOSV";
#else
        const char* xdg = std::getenv("XDG_STATE_HOME");
        std::filesystem::path dir = (xdg && *xdg) ? std::filesystem::path(xdg) / "openosv"
                                                  : std::filesystem::path(home) / ".local" / "state" / "openosv";
#endif
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec && !std::filesystem::is_directory(dir, ec)) {
            return {};
        }
        return dir.string();
    } catch (...) {
        return {};
    }
}

/// Current wall clock as "YYYY-MM-DD HH:MM:SS.mmm" (local time).
std::string timestamp() noexcept {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
    localtime_r(&seconds, &local);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, static_cast<int>(millis));
    return buf;
}

/// This thread's id, as the OS reports it (a small number on macOS).
unsigned long long threadId() noexcept {
#if defined(__APPLE__)
    std::uint64_t tid = 0;
    if (pthread_threadid_np(nullptr, &tid) == 0) {
        return static_cast<unsigned long long>(tid);
    }
    return 0;
#else
    return static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

/// The live copy: the unified log on macOS, stderr elsewhere.
void debugOutput(const std::string& line) noexcept {
#if defined(__APPLE__)
    static os_log_t log = os_log_create("com.openosv.plugins", "PluginLog");
    os_log_with_type(log, OS_LOG_TYPE_DEFAULT, "[OpenOSV] %{public}s", line.c_str());
#else
    std::fputs(("[OpenOSV] " + line).c_str(), stderr);
#endif
}

/// Open the file in append mode and record its current size.  Caller holds
/// the mutex.
bool openFileLocked(LogState& s) noexcept {
    if (s.file) {
        return true;
    }
    if (s.pathUtf8.empty()) {
        return false;
    }
    // "a": every write lands at the end even with another process appending
    // to the same file.  The descriptor is made close-on-exec, so a child
    // the host starts does not inherit it.
    FILE* f = std::fopen(s.pathUtf8.c_str(), "a");
    if (!f) {
        return false;
    }
    (void)::fcntl(::fileno(f), F_SETFD, FD_CLOEXEC);
    s.file = f;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const off_t pos = ftello(f);
        s.bytesWritten = pos > 0 ? static_cast<unsigned long long>(pos) : 0ull;
    }
    return true;
}

/// Rotate: close, rename to ".1" (replacing an older generation) and reopen.
void rotateLocked(LogState& s) noexcept {
    if (s.file) {
        std::fclose(s.file);
        s.file = nullptr;
    }
    const std::string backup = s.pathUtf8 + ".1";
    // rename() replaces an existing target atomically; on failure we simply
    // keep appending to the big file.
    (void)std::rename(s.pathUtf8.c_str(), backup.c_str());
    s.bytesWritten = 0;
    openFileLocked(s);
}

}  // namespace

// -----------------------------------------------------------------------------
//  Public API (same contract as PluginLog.cpp)
// -----------------------------------------------------------------------------
bool PluginLog::init(std::wstring_view pluginName) noexcept {
    applyEnvironmentLevel();
    try {
        LogState& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);

        const std::wstring name = pluginName.empty() ? std::wstring(L"OpenOSV") : std::wstring(pluginName);
        if (s.file && s.pluginName == name) {
            return true;  // already open for this plug-in
        }
        if (s.file) {
            std::fclose(s.file);
            s.file = nullptr;
        }
        s.pluginName = name;
        const std::string dir = logDirectory();
        if (dir.empty()) {
            s.path.clear();
            s.pathUtf8.clear();
            debugOutput("PluginLog: no home directory, file logging disabled\n");
            return false;
        }
        s.pathUtf8 = dir + "/" + toUtf8(name) + ".log";
        s.path = std::filesystem::path(s.pathUtf8).wstring();
        if (!openFileLocked(s)) {
            debugOutput("PluginLog: cannot open " + s.pathUtf8 + "\n");
            return false;
        }
        if (s.bytesWritten >= kRotateBytes) {
            rotateLocked(s);
        }
        return s.file != nullptr;
    } catch (...) {
        return false;
    }
}

void PluginLog::shutdown() noexcept {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file) {
        std::fflush(s.file);
        std::fclose(s.file);
        s.file = nullptr;
    }
    s.path.clear();
    s.pathUtf8.clear();
    s.pluginName.clear();
    s.bytesWritten = 0;
}

void PluginLog::setLevel(Level level) noexcept {
    g_levelInitialised.store(true);
    g_level.store(static_cast<int>(level));
}

PluginLog::Level PluginLog::level() noexcept {
    applyEnvironmentLevel();
    return static_cast<Level>(g_level.load());
}

bool PluginLog::enabled(Level level) noexcept {
    applyEnvironmentLevel();
    return static_cast<int>(level) >= g_level.load() && level != Level::Off;
}

const char* PluginLog::levelName(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Off: return "OFF  ";
    }
    return "?????";
}

void PluginLog::write(Level level, std::string_view text) noexcept {
    if (!enabled(level)) {
        return;
    }
    try {
        // Build the full line outside the lock; only the write is serialised.
        std::string line;
        line.reserve(text.size() + 64);
        line += timestamp();
        line += " [";
        line += levelName(level);
        line += "] [pid ";
        line += std::to_string(static_cast<long long>(::getpid()));
        line += " tid ";
        line += std::to_string(threadId());
        line += "] ";
        line.append(text.data(), text.size());
        // One record per line, whatever the message contained.
        for (char& c : line) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        line += "\n";

        LogState& s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.file || (!s.pathUtf8.empty() && openFileLocked(s))) {
                std::fwrite(line.data(), 1, line.size(), s.file);
                std::fflush(s.file);
                s.bytesWritten += line.size();
                if (s.bytesWritten >= kRotateBytes) {
                    rotateLocked(s);
                }
            }
        }

        debugOutput(line);
    } catch (...) {
        // Dropping a log line is the only acceptable failure mode here.
    }
}

bool PluginLog::once(std::string_view key, Level level, std::string_view text) noexcept {
    if (!enabled(level)) {
        return false;
    }
    try {
        LogState& s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            // Bound the set so a key built from a counter cannot grow it
            // without limit.
            if (s.onceKeys.size() > 4096) {
                s.onceKeys.clear();
            }
            if (!s.onceKeys.emplace(key).second) {
                return false;
            }
        }
        write(level, text);
        return true;
    } catch (...) {
        return false;
    }
}

std::wstring PluginLog::filePath() noexcept {
    try {
        LogState& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.path;
    } catch (...) {
        return {};
    }
}

}  // namespace osv::premiere
