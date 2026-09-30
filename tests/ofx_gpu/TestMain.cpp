// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_ofx_gpu_tests (tests/ofx_gpu/CMakeLists.txt).
//
// Like every test executable here that runs plug-in code, the FIRST thing it
// does is keep the plug-in log out of the user's real log folder
// (tests/premiere/mockhost/TestLogIsolation.h says what that cost when it
// was missing), then quiet the log unless the runner asks for it.

#include "TestLogIsolation.h"

#include <catch2/catch_session.hpp>

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
    // PluginLog reads the level when it first logs; errors only, unless the
    // runner wants the whole story.
    ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", std::getenv("OSV_TEST_VERBOSE") ? "debug" : "error");
    return Catch::Session().run(argc, argv);
}
