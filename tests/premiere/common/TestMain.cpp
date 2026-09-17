// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_premiere_common_tests.  Own main() so the Windows
// console is switched to UTF-8 (the mock host and the plug-in log both emit
// UTF-8) and so the plug-in logger writes somewhere harmless by default.

#include <catch2/catch_session.hpp>

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
    SetConsoleOutputCP(CP_UTF8);

    // The tests exercise PluginLog explicitly; until they do, keep it quiet
    // so a failing assertion is not buried in log noise.
    osv::premiere::PluginLog::setLevel(std::getenv("OSV_TEST_VERBOSE") ? osv::premiere::PluginLog::Level::Debug
                                                                      : osv::premiere::PluginLog::Level::Error);

    const int result = Catch::Session().run(argc, argv);

    // Close the log file explicitly: the process may still hold it open when
    // Catch2 returns, and a following run would then append to a locked file.
    osv::premiere::PluginLog::shutdown();
    return result;
}
