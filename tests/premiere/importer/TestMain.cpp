// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Entry point of osv_importer_tests.
//
// Two things happen before Catch2 runs, and both are load-bearing:
//
//   1. OPENOSV_IMPORTER_NO_DIALOG=1.  imGetPrefs8 shows a MODAL Win32 dialog
//      that pumps messages until a user clicks something; without this the
//      prefs tests would hang a CI machine forever.  The variable is a
//      documented feature of the importer (docs/BUILDING.md), not a test-only
//      back door, because an unattended render farm needs exactly the same
//      behaviour.
//
//   2. The plug-in's own log is pointed at a harmless level.  The importer
//      logs to %LOCALAPPDATA%\OpenOSV\OpenOSVImporter.log through its own
//      copy of PluginLog inside the .prm, which this process cannot reach
//      (different module, different statics), so the level is controlled the
//      way the plug-in itself reads it: through the environment.

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
    SetConsoleOutputCP(CP_UTF8);

    // Never block on a modal dialog.  _putenv_s rather than SetEnvironmentVariable
    // because the plug-in reads it with getenv_s, which uses the CRT's own copy
    // of the environment - and the .prm shares this process's CRT (both /MD).
    ::_putenv_s("OPENOSV_IMPORTER_NO_DIALOG", "1");

    // Debug-level plug-in logging only when the runner asks for it; the
    // importer reads this variable when PluginLog::init runs inside the .prm.
    if (!std::getenv("OSV_TEST_VERBOSE")) {
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "error");
    } else {
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "debug");
    }

    return Catch::Session().run(argc, argv);
}
