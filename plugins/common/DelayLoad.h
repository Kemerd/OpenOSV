// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DelayLoad: resolve delay-loaded DLLs from the plug-in's own directory.
//
// Why this exists
// ---------------
// Premiere Pro's application directory ships its own FFmpeg (avcodec-59.dll,
// avformat-59.dll, avutil-57.dll, ...) and CUDA runtime DLLs, and that
// directory is the first place the Windows loader looks when a plug-in has
// an implicit import.  OpenOSV links a newer FFmpeg from vcpkg
// (avcodec-63.dll, avformat-63.dll, avutil-61.dll, swresample-7.dll,
// swscale-10.dll) plus OpenCL.dll, fmt.dll, spdlog.dll, z.dll and miniz.dll
// (see vcpkg_installed/x64-windows/bin).  Today the major-version suffixes
// differ so there is no clash, but the moment they coincide the loader
// would silently bind us to Adobe's copy and every FFmpeg call would land in
// the wrong ABI.  A machine without an OpenCL ICD would also refuse to load
// the importer outright if OpenCL.dll were an implicit import.
//
// The fix has two halves:
//   1. cmake: osv_add_delayload() marks those DLLs /DELAYLOAD so nothing is
//      resolved until first use (and a missing OpenCL.dll only fails the
//      OpenCL code path, not the plug-in load);
//   2. this file: a delay-load notification hook (__pfnDliNotifyHook2)
//      that, for dliNotePreLoadLibrary, loads the requested DLL from the
//      directory that contains the plug-in module itself with
//      LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH), so
//      the DLL's own dependencies are also taken from that directory.  When
//      the file is not there the hook returns nullptr and the default search
//      order applies.
//
// Since Visual Studio 2015 the hook pointer __pfnDliNotifyHook2 is a const
// initialised at link time, so it cannot be assigned from DllMain.  The
// definition in DelayLoad.cpp is therefore a trampoline that forwards to an
// atomic "active hook" pointer; installHook() only stores that pointer
// (no loading, no allocation, safe under the loader lock in
// DllMain(DLL_PROCESS_ATTACH)).  The module directory is computed lazily on
// the first notification with GetModuleHandleExW(..._FROM_ADDRESS) on a
// function inside this translation unit, so it is always the directory of
// the module that contains this code (the .prm / .aex), never the host exe.
//
// Contract for plug-in authors: call installHook() from
// DllMain(DLL_PROCESS_ATTACH) and link delayimp through osv_add_delayload().
// Referencing installHook() from DllMain is also what pulls this object
// file (and its __pfnDliNotifyHook2 definition) into the link ahead of the
// default definition in delayimp.lib.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace osv::premiere::delayload {

/// Activate the hook.  Only stores a pointer; safe from DllMain.  Calling
/// it more than once is harmless.
void installHook() noexcept;

/// Deactivate the hook (delay-loads fall back to the default search).
/// Mostly for tests; plug-ins keep the hook active for their lifetime.
void uninstallHook() noexcept;

/// True while the hook is active.
[[nodiscard]] bool hookInstalled() noexcept;

/// Directory of the module containing this code, with a trailing
/// backslash; empty when it could not be determined.  Computed once.
[[nodiscard]] std::wstring moduleDirectory() noexcept;

/// The resolution step the hook performs, exposed for tests and for code
/// that wants to pre-load a DLL explicitly: try
/// "<moduleDirectory()>\<dllName>" with LOAD_WITH_ALTERED_SEARCH_PATH and
/// return the module handle, or nullptr when the file is absent or cannot
/// be loaded.  `dllName` is the bare file name ("avcodec-63.dll"); names
/// containing a path separator are rejected (nullptr).
[[nodiscard]] HMODULE loadBesideModule(const char* dllName) noexcept;

/// Counters for diagnostics and tests.
struct Stats {
    std::uint64_t notifications = 0;   ///< dliNotePreLoadLibrary callbacks seen while active.
    std::uint64_t resolvedBeside = 0;  ///< Loads satisfied from the module directory.
    std::uint64_t fallbacks = 0;       ///< Notifications answered with nullptr (default search).
    std::wstring lastResolvedPath;     ///< Full path of the most recent resolvedBeside hit.
};

/// Snapshot of the counters.
[[nodiscard]] Stats stats() noexcept;

/// Reset the counters (tests).
void resetStats() noexcept;

}  // namespace osv::premiere::delayload
