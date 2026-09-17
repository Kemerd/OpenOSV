// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_reframe_tests.
//
// Own main() for the same two reasons the common-layer tests have one: the
// Windows console has to be switched to UTF-8 before anything prints, and
// the plug-in logger has to be quietened so a failing assertion is not
// buried under the effect's own log lines (the tests load the real .aex, so
// the real logger runs).

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

    // OSV_TEST_VERBOSE=1 turns the effect's own logging back on, which is
    // what you want when a RENDER test fails for an unobvious reason.
    osv::premiere::PluginLog::setLevel(std::getenv("OSV_TEST_VERBOSE") ? osv::premiere::PluginLog::Level::Debug
                                                                      : osv::premiere::PluginLog::Level::Error);

    const int result = Catch::Session().run(argc, argv);

    // The loaded .aex may still hold the log file open; close it explicitly
    // so a following run does not append to a locked file.
    osv::premiere::PluginLog::shutdown();
    return result;
}
