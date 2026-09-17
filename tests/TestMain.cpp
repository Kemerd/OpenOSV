// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Test entry point.  We provide our own main() so that the Windows console is
// switched to UTF-8 and the logger is quietened before Catch2 runs.

#include <catch2/catch_session.hpp>

#include "osv/core/Log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    // Tests are noisy enough on their own; only warnings and errors from the
    // library make it to the console unless OSV_TEST_VERBOSE is set.
    osv::log::setLevel(std::getenv("OSV_TEST_VERBOSE") ? osv::log::Level::Debug : osv::log::Level::Warn);
    return Catch::Session().run(argc, argv);
}
