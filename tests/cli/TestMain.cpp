// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_cli_tests: the tests that run the built osvtool and
// compare its output with the plug-in engine's AudioDecoder.  The AudioDecoder
// logs through PluginLog, and so does the osvtool child (which inherits this
// process' environment), so the log folder is redirected FIRST, before either
// can write into the user's real %LOCALAPPDATA%\OpenOSV.

#include <catch2/catch_session.hpp>

#include "osv/core/Log.h"

#if defined(_WIN32)
#include "TestLogIsolation.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdlib>

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    osv::premiere::testsupport::isolatePluginLogs();
    SetConsoleOutputCP(CP_UTF8);
#endif
    osv::log::setLevel(std::getenv("OSV_TEST_VERBOSE") ? osv::log::Level::Debug : osv::log::Level::Warn);
    return Catch::Session().run(argc, argv);
}
