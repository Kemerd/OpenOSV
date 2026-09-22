// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_source_settings_tests.
//
// Own main() for the same two reasons the other plug-in test executables have
// one: the Windows console has to be switched to UTF-8 before anything
// prints, and the plug-in logger has to be quietened so a failing assertion
// is not buried under the effect's own log lines (the tests load the real
// .aex, so the real logger runs).

#include <catch2/catch_session.hpp>

#include "TestLogIsolation.h"

#include "PluginLog.h"

#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int main(int argc, char* argv[]) {
    // FIRST, before anything can log: keep every plug-in log of this run out
    // of the user's real %LOCALAPPDATA%\OpenOSV (see TestLogIsolation.h for
    // the diagnosis this cost when it was missing).
    osv::premiere::testsupport::isolatePluginLogs();

    SetConsoleOutputCP(CP_UTF8);

    // OSV_TEST_VERBOSE=1 turns the effect's own logging back on, which is
    // what you want when a prefs-translation test fails for an unobvious
    // reason - the effect logs every translated blob at Debug.
    osv::premiere::PluginLog::setLevel(std::getenv("OSV_TEST_VERBOSE") ? osv::premiere::PluginLog::Level::Debug
                                                                      : osv::premiere::PluginLog::Level::Error);

    const int result = Catch::Session().run(argc, argv);

    // The loaded .aex may still hold the log file open; close it explicitly
    // so a following run does not append to a locked file.
    osv::premiere::PluginLog::shutdown();
    return result;
}
