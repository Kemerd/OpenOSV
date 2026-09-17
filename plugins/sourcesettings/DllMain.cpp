// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DllMain.cpp - the module entry point of OpenOSVSourceSettings.aex.
//
// It does exactly one thing: arm the delay-load hook from
// plugins/common/DelayLoad.cpp so that any DLL the module delay-loads is
// resolved from the plug-in's OWN folder rather than from Premiere's
// application directory (which ships its own, older, copies of several
// common libraries - notably FFmpeg and fmt).
//
// This module's own needs are small: it links osv_premiere_common for
// PluginLog, which reaches spdlog and fmt.  The hook still matters, because
// loading Adobe's fmt.dll against our spdlog build is exactly the kind of
// silent ABI mismatch that shows up as a crash inside the logger on the very
// first message.
//
// Everything else that could plausibly go here - opening the log file,
// acquiring a suite - is deliberately NOT done.  DllMain runs under the
// loader lock, where file I/O, LoadLibrary and anything that might block are
// all documented ways to deadlock the host.  The log is opened at
// PF_Cmd_GLOBAL_SETUP.
//
// Referencing installHook() from here is also what pulls DelayLoad.obj into
// the link, and with it the __pfnDliNotifyHook2 definition that has to win
// over the default one in delayimp.lib.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "DelayLoad.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // Thread attach/detach notifications are useless to us and cost a
            // callback per thread in a host that creates many.
            DisableThreadLibraryCalls(module);
            // Only stores a pointer; no allocation, no file system, no
            // LoadLibrary - safe under the loader lock.
            osv::premiere::delayload::installHook();
            break;

        case DLL_PROCESS_DETACH:
            // Nothing on purpose.  PF_Cmd_GLOBAL_SETDOWN has already closed
            // the log; if it did not, the process is exiting and flushing a
            // file from here risks deadlocking against the loader lock.
            break;

        default:
            break;
    }
    return TRUE;
}
