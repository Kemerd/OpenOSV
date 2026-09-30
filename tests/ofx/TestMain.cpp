// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_ofx_tests.
//
// Like every test executable that loads a plug-in, the FIRST thing it does is
// keep the module's log out of the user's real log folder
// (tests/premiere/mockhost/TestLogIsolation.h has the diagnosis that cost
// when it was missing), then quiet the log unless the runner asks for it.
//
//   Windows  isolatePluginLogs() points LOCALAPPDATA at a private folder.
//   macOS    the module logs to $HOME/Library/Logs/OpenOSV
//            (plugins/common/PluginLogPosix.cpp), so HOME is pointed at a
//            private folder instead - per process, like the Windows one, so
//            parallel ctest workers never share a file.
//
// The host the mock plays is the process's (OSV_MOCK_OFX_PROFILE, see
// MockOfxHost.h).  A VEGAS process started without a test spec runs the
// [vegas] tests - the only ones written for that host - instead of the
// Resolve-shaped rest of the suite.

#include "MockOfxHost.h"

#include <catch2/catch_session.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include "TestLogIsolation.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

/// Set an environment variable in THIS process's CRT - the one the module
/// reads too (both /MD on Windows; one libc on macOS).
void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    osv::premiere::testsupport::isolatePluginLogs();
    SetConsoleOutputCP(CP_UTF8);
#else
    {
        std::error_code ec;
        const std::filesystem::path home = std::filesystem::temp_directory_path(ec) / "OpenOSV-tests" /
                                           ("pid-" + std::to_string(static_cast<long long>(::getpid())));
        std::filesystem::create_directories(home, ec);
        if (!ec) {
            setEnv("HOME", home.string().c_str());
        }
    }
#endif

    // The module reads the level when kOfxActionLoad initialises its log.
    setEnv("OSV_PLUGIN_LOG_LEVEL", std::getenv("OSV_TEST_VERBOSE") ? "debug" : "error");

    Catch::Session session;
    const int parsed = session.applyCommandLine(argc, argv);
    if (parsed != 0) {
        return parsed;
    }
    // A VEGAS process with no spec of its own runs the VEGAS tests (hidden
    // from every other run), not the tests that expect Resolve's descriptors.
    if (osv::ofxtest::MockHost::instance().isVegas() && session.configData().testsOrTags.empty()) {
        session.configData().testsOrTags.push_back("[vegas]");
        session.useConfigData(session.configData());
    }
    return session.run();
}
