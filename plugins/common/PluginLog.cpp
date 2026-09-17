// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "PluginLog.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <share.h>  // _SH_DENYWR for _wfsopen
#include <string>
#include <unordered_set>

namespace osv::premiere {

namespace {

/// Everything mutable lives in one function-local static so there is no
/// static-initialisation-order dependency between translation units and no
/// destructor work at process exit beyond closing the file.
struct LogState {
    std::mutex mutex;
    FILE* file = nullptr;
    std::wstring path;
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

/// Apply OSV_PLUGIN_LOG_LEVEL once (debug / trace / info / warn / error).
void applyEnvironmentLevel() noexcept {
    bool expected = false;
    if (!g_levelInitialised.compare_exchange_strong(expected, true)) {
        return;
    }
    wchar_t buffer[32] = {};
    const DWORD n = GetEnvironmentVariableW(L"OSV_PLUGIN_LOG_LEVEL", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return;
    }
    std::wstring value(buffer, n);
    for (wchar_t& c : value) {
        c = static_cast<wchar_t>(towlower(c));
    }
    if (value == L"trace") {
        g_level.store(static_cast<int>(PluginLog::Level::Trace));
    } else if (value == L"debug") {
        g_level.store(static_cast<int>(PluginLog::Level::Debug));
    } else if (value == L"info") {
        g_level.store(static_cast<int>(PluginLog::Level::Info));
    } else if (value == L"warn" || value == L"warning") {
        g_level.store(static_cast<int>(PluginLog::Level::Warn));
    } else if (value == L"error") {
        g_level.store(static_cast<int>(PluginLog::Level::Error));
    } else if (value == L"off") {
        g_level.store(static_cast<int>(PluginLog::Level::Off));
    }
}

/// %LOCALAPPDATA%\OpenOSV, created when missing.  Empty on failure.
std::wstring logDirectory() noexcept {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    std::wstring dir(buffer, n);
    dir += L"\\OpenOSV";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return dir;
}

/// UTF-8 -> UTF-16 for OutputDebugStringW; invalid sequences are replaced.
std::wstring toWide(std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return L"<invalid utf-8>";
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

/// Current wall clock as "YYYY-MM-DD HH:MM:SS.mmm".
std::string timestamp() noexcept {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

/// Open the file in append mode and record its current size.  Caller holds
/// the mutex.
bool openFileLocked(LogState& s) noexcept {
    if (s.file) {
        return true;
    }
    if (s.path.empty()) {
        return false;
    }
    // "ab" keeps earlier sessions; "ccs" is deliberately not used so the
    // bytes we write (UTF-8) land unchanged.
    //
    // _wfsopen with _SH_DENYWR instead of _wfopen_s: _wfopen_s opens with no
    // sharing at all, so nothing else could even read the file while the
    // plug-in is loaded - not a user tailing it, not DebugView, and not a
    // second host process (Premiere Pro and Media Encoder run side by side
    // and both load the importer).  _SH_DENYWR lets any number of readers in
    // while still keeping a second writer out, which is what makes the
    // interleaved lines meaningful.
    FILE* f = _wfsopen(s.path.c_str(), L"ab", _SH_DENYWR);
    if (!f) {
        return false;
    }
    s.file = f;
    // Size for the rotation check.
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        const long long pos = _ftelli64(f);
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
    const std::wstring backup = s.path + L".1";
    // MoveFileExW with REPLACE_EXISTING is atomic enough for a log file; on
    // failure we simply keep appending to the big file.
    MoveFileExW(s.path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING);
    s.bytesWritten = 0;
    openFileLocked(s);
}

}  // namespace

// -----------------------------------------------------------------------------
//  Public API
// -----------------------------------------------------------------------------
bool PluginLog::init(std::wstring_view pluginName) noexcept {
    applyEnvironmentLevel();
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
    const std::wstring dir = logDirectory();
    if (dir.empty()) {
        s.path.clear();
        OutputDebugStringW(L"[OpenOSV] PluginLog: LOCALAPPDATA unavailable, file logging disabled\n");
        return false;
    }
    s.path = dir + L"\\" + name + L".log";
    if (!openFileLocked(s)) {
        OutputDebugStringW((L"[OpenOSV] PluginLog: cannot open " + s.path + L"\n").c_str());
        return false;
    }
    if (s.bytesWritten >= kRotateBytes) {
        rotateLocked(s);
    }
    return s.file != nullptr;
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
        line += "] [tid ";
        line += std::to_string(GetCurrentThreadId());
        line += "] ";
        line.append(text.data(), text.size());
        // Normalise line endings: a message never contains a raw newline in
        // the file, so grep/tail see one record per line.
        for (char& c : line) {
            if (c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        line += "\r\n";

        LogState& s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.file || (!s.path.empty() && openFileLocked(s))) {
                std::fwrite(line.data(), 1, line.size(), s.file);
                std::fflush(s.file);
                s.bytesWritten += line.size();
                if (s.bytesWritten >= kRotateBytes) {
                    rotateLocked(s);
                }
            }
        }

        // Debugger copy, prefixed so DebugView filters can pick it out.
        std::wstring wide = L"[OpenOSV] ";
        wide += toWide(line);
        OutputDebugStringW(wide.c_str());
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
