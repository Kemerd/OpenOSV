// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// TestLogIsolation.h - keep test runs out of the user's real plug-in logs.
//
// WHY THIS EXISTS
// ---------------
// Every plug-in logs to %LOCALAPPDATA%\OpenOSV\<plugin>.log through its own
// copy of PluginLog.  The test executables load those very plug-ins (.prm,
// .aex) into their own process, and until this header existed nothing stopped
// the loaded modules from writing into the SAME files a real Premiere session
// writes to.
//
// That was not cosmetic.  The negative tests deliberately provoke warnings -
// a missing DrawBot suite, a host that refuses the custom UI suite, a
// zero-sized frame - and those warnings landed in the user's log interleaved
// with real Premiere output.  One ctest run wrote exactly 285 lines, several
// runs a few minutes apart produced bursts that looked like Premiere
// misbehaving, and that misled a performance diagnosis twice: first as a
// supposed 46 Hz teardown loop, then as sixteen Premiere helper processes.
// Both were test runs.
//
// HOW
// ---
// PluginLog resolves its directory from the LOCALAPPDATA environment
// variable, and the plug-ins run inside the test process, so pointing that
// variable at a private temporary directory before anything logs redirects
// every module at once - the test executable's own PluginLog and every
// plug-in it loads.  Nothing in the plug-ins changes and nothing is special-
// cased for tests.
//
// Each process gets its own directory (keyed by pid) so that parallel ctest
// workers never contend for one file: PluginLog opens with a deny-write share
// mode, and a second process would otherwise silently lose file logging.
//
// Directories cannot be removed on the way out, because a loaded plug-in
// keeps its log open until the module unloads, which is after main()
// returns.  Instead, stale directories from earlier runs are swept at start.

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace osv::premiere::testsupport {

namespace detail {

/// Storage for the LOCALAPPDATA value this process started with.
///
/// A function-local static rather than a namespace-scope one so there is no
/// static-initialisation-order question: the first call creates it.
inline std::wstring& originalLocalAppDataStorage() {
    static std::wstring value;
    return value;
}

/// Read an environment variable from the OS block.  Empty when unset.
inline std::wstring readEnvironment(const wchar_t* name) {
    wchar_t buffer[32768] = {};
    const DWORD n = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    // 0 means unset (or empty); a return >= the buffer size means it did not
    // fit, which a real LOCALAPPDATA never does - treat both as "no value"
    // rather than using a truncated path.
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return std::wstring(buffer, n);
}

/// Set a variable in BOTH environments a module might read.
///
/// PluginLog reads the OS block with GetEnvironmentVariableW; code that uses
/// getenv reads the C runtime's own copy, which is initialised from the OS
/// block at start-up and not refreshed afterwards.  The plug-ins share this
/// process's CRT (everything is built /MD), so setting both reaches every
/// reader regardless of which API it uses.
inline void writeEnvironment(const wchar_t* name, const std::wstring& value) {
    SetEnvironmentVariableW(name, value.c_str());
    _wputenv_s(name, value.c_str());
}

/// Remove sibling directories left by earlier runs.
///
/// Only directories untouched for a day are removed, so a concurrently
/// running test process - whose directory is being written right now - is
/// never swept from under it.  Every failure is ignored: a directory still
/// held open by a crashed process is simply retried next time.
inline void sweepStaleDirectories(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return;
    }
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    // An explicit loop with increment(ec), not a range-for: the range-for's
    // operator++ THROWS on an I/O error mid-iteration, and test scaffolding
    // must never be the thing that aborts a run.
    std::filesystem::directory_iterator it(root, ec);
    const std::filesystem::directory_iterator end;
    while (!ec && it != end) {
        std::error_code entryEc;
        if (it->is_directory(entryEc)) {
            const auto written = it->last_write_time(entryEc);
            if (!entryEc && written <= cutoff) {
                std::filesystem::remove_all(it->path(), entryEc);
            }
        }
        it.increment(ec);
    }
}

}  // namespace detail

/// The LOCALAPPDATA this process had before isolatePluginLogs() replaced it.
///
/// Exposed so a test can assert the invariant this header exists for: no log
/// file of the run lives under the user's real profile.  Empty when
/// isolatePluginLogs() has not run, or when the variable was unset.
[[nodiscard]] inline const std::wstring& originalLocalAppData() {
    return detail::originalLocalAppDataStorage();
}

/// Point every plug-in log of this process at a private temporary directory.
///
/// Must be the first thing main() does, before any code path that can log:
/// PluginLog caches its path on the first init(), so a redirect after that
/// would miss the file that is already open.
///
/// Returns the directory now standing in for %LOCALAPPDATA%.  If it cannot be
/// created the variable is STILL redirected - tests that need a log file then
/// fail loudly, which is the right outcome on a broken machine, whereas
/// falling back to the real profile would quietly reintroduce the problem.
inline std::filesystem::path isolatePluginLogs() {
    // Record the original exactly once, even if called twice.
    std::wstring& original = detail::originalLocalAppDataStorage();
    if (original.empty()) {
        original = detail::readEnvironment(L"LOCALAPPDATA");
    }

    // <temp>\OpenOSV-tests\pid-<n>.  GetTempPathW returns a trailing
    // backslash; std::filesystem handles either form.
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    std::filesystem::path root = (n > 0 && n <= MAX_PATH) ? std::filesystem::path(temp)
                                                          : std::filesystem::current_path();
    root /= L"OpenOSV-tests";

    detail::sweepStaleDirectories(root);

    const std::filesystem::path mine = root / (L"pid-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(mine, ec);
    if (ec) {
        std::fprintf(stderr, "[test] could not create %ls (%s); plug-in file logging will fail\n",
                     mine.c_str(), ec.message().c_str());
    }

    detail::writeEnvironment(L"LOCALAPPDATA", mine.wstring());

    // Say where the logs went when a developer asked for detail, so a failing
    // test's plug-in log is one copy-paste away rather than a hunt.
    if (std::getenv("OSV_TEST_VERBOSE")) {
        std::fprintf(stderr, "[test] plug-in logs for this run: %ls\\OpenOSV\n", mine.c_str());
    }
    return mine;
}

}  // namespace osv::premiere::testsupport
