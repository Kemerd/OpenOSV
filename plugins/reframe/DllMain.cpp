// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DllMain.cpp - the module entry point of Open360Reframe.aex.
//
// It does exactly one thing: arm the delay-load hook from
// plugins/common/DelayLoad.cpp so that any DLL the module delay-loads is
// resolved from the plug-in's OWN folder rather than from Premiere's
// application directory (which ships its own FFmpeg and CUDA runtimes).
//
// Everything else that could plausibly go here - opening the log file,
// creating the shared HostContext, probing for a GPU - is deliberately NOT
// done.  DllMain runs under the loader lock, where file I/O, LoadLibrary and
// anything that might block are all documented ways to deadlock the host.
// The log is opened at PF_Cmd_GLOBAL_SETUP / xGPUFilterEntry startup and the
// context is created on first use.
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
            // Thread attach/detach notifications are useless to us and cost
            // a callback per thread in a host that creates many.
            DisableThreadLibraryCalls(module);
            // Only stores a pointer; no allocation, no file system, no
            // LoadLibrary - safe under the loader lock.
            osv::premiere::delayload::installHook();
            break;

        case DLL_PROCESS_DETACH:
            // Nothing is torn down here on purpose: at process detach the
            // CUDA driver may already be unloaded and freeing device memory
            // would crash the host.  GLOBAL_SETDOWN and the GPU entry's
            // shutdown phase are the documented, ordered places for that.
            break;

        default:
            break;
    }
    return TRUE;
}
