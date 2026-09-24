// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_ofx_tests.
//
// Like every test executable that loads a plug-in, the FIRST thing it does is
// keep the module's log out of the user's real %LOCALAPPDATA%\OpenOSV
// (tests/premiere/mockhost/TestLogIsolation.h has the diagnosis that cost
// when it was missing), then quiet the log unless the runner asks for it.

#include <catch2/catch_session.hpp>

#include "TestLogIsolation.h"

#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int main(int argc, char* argv[]) {
    osv::premiere::testsupport::isolatePluginLogs();

    SetConsoleOutputCP(CP_UTF8);

    // The module reads the level when kOfxActionLoad initialises its log;
    // it shares this process's CRT (both /MD), so _putenv_s reaches it.
    if (!std::getenv("OSV_TEST_VERBOSE")) {
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "error");
    } else {
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "debug");
    }

    return Catch::Session().run(argc, argv);
}
