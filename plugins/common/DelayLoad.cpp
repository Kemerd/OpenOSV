// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "DelayLoad.h"

#include <delayimp.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace osv::premiere::delayload {

namespace {

/// Signature of a delay-load hook, as delayimp.h declares it.
using HookFn = FARPROC(WINAPI*)(unsigned dliNotify, PDelayLoadInfo pdli);

/// The hook the trampoline forwards to; nullptr = inactive.
std::atomic<HookFn> g_activeHook{nullptr};

/// Everything mutable the hook path touches, in ONE object with a
/// deliberately never-run destructor.
///
/// This is the same discipline PluginLog uses, and for the same reason: the
/// delay-load hook can fire at ANY time a delay-loaded import is first
/// touched, which includes teardown.  Namespace-scope objects with
/// non-trivial destructors (a std::mutex, a std::wstring) are destroyed by
/// the CRT at DLL_PROCESS_DETACH or at exit(); a thunk that fires after that
/// would lock a destroyed mutex and read a destroyed string.  A function-local
/// static that is never destroyed cannot be in that state.
///
/// The directory is a FIXED wchar_t buffer rather than a std::wstring, and
/// `dirState` is an atomic rather than a std::once_flag, because the hook
/// runs under the LOADER LOCK.  Allocating there is a documented deadlock
/// risk (another thread can hold the CRT heap lock while waiting on the
/// loader lock), and std::call_once can block.  Filling a fixed buffer once
/// and publishing it with a release store removes both hazards: the hook's
/// hot path is a plain acquire load and a memcpy-free read.
struct HookState {
    std::mutex statsMutex;  ///< Guards `stats` only; never taken on the fast path.
    Stats stats;

    /// The module's directory, including the trailing separator.  Written
    /// once, before `dirState` reaches 2; read-only afterwards.
    wchar_t dir[MAX_PATH * 4] = {};
    /// 0 = not computed, 1 = computing, 2 = ready (possibly empty on failure).
    std::atomic<int> dirState{0};
};

/// The one HookState, intentionally leaked (see above).
HookState& hookState() noexcept {
    static HookState* const state = new HookState();
    return *state;
}

/// Anchor whose address identifies this module for GetModuleHandleExW.
void anchorFunction() noexcept {}

/// Compute the directory of the module that contains anchorFunction().
void computeModuleDirectory() noexcept {
    HMODULE self = nullptr;
    // FROM_ADDRESS resolves the module owning the given code address;
    // UNCHANGED_REFCOUNT avoids pinning ourselves.
    const BOOL ok = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&anchorFunction), &self);
    if (!ok || !self) {
        return;
    }

    // Grow the buffer until the full path fits (long paths are possible when
    // the plug-in folder is deep).
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) {
            return;
        }
        if (n < path.size() - 1) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2, L'\0');
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return;
    }
    // Copy into the fixed buffer.  A path that does not fit leaves the buffer
    // empty, which makes loadBesideModule() fall back to the default search -
    // the safe answer, and one the caller already handles.
    const std::size_t dirLen = slash + 1u;
    HookState& s = hookState();
    constexpr std::size_t kCapacity = sizeof(s.dir) / sizeof(s.dir[0]);
    if (dirLen == 0 || dirLen >= kCapacity) {
        return;
    }
    std::memcpy(s.dir, path.data(), dirLen * sizeof(wchar_t));
    s.dir[dirLen] = L'\0';
}

/// Compute the module directory at most once, then return a pointer to it.
///
/// Never allocates and never blocks, so it is safe under the loader lock.
/// The state machine is 0 -> 1 (this thread computes) -> 2 (published).  A
/// second thread that arrives while the first is computing spins on the
/// atomic; the work is a couple of GetModuleFileNameW calls, so the spin is
/// bounded and short - and a mutex here is exactly what must be avoided.
const wchar_t* moduleDirectoryCached() noexcept {
    HookState& s = hookState();
    int expected = 0;
    if (s.dirState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        computeModuleDirectory();
        // RELEASE: publishes every byte written into s.dir above.
        s.dirState.store(2, std::memory_order_release);
        return s.dir;
    }
    // ACQUIRE: pairs with the release store, so the buffer is fully visible.
    while (s.dirState.load(std::memory_order_acquire) != 2) {
        // A pause hint keeps the spin cheap; the window is microseconds.
        YieldProcessor();
    }
    return s.dir;
}

/// The real hook.  Only dliNotePreLoadLibrary is answered; every other
/// notification returns nullptr, which tells the delay-load helper to
/// proceed normally.
FARPROC WINAPI resolveHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify != dliNotePreLoadLibrary || !pdli || !pdli->szDll) {
        return nullptr;
    }
    // The counters are best effort and their mutex is never on the path that
    // matters, but a throw must still never escape into the loader.
    try {
        HookState& s = hookState();
        std::lock_guard<std::mutex> lock(s.statsMutex);
        ++s.stats.notifications;
    } catch (...) {
    }
    const HMODULE h = loadBesideModule(pdli->szDll);
    if (!h) {
        try {
            HookState& s = hookState();
            std::lock_guard<std::mutex> lock(s.statsMutex);
            ++s.stats.fallbacks;
        } catch (...) {
        }
        return nullptr;
    }
    // Returning an HMODULE from dliNotePreLoadLibrary makes the helper use
    // it instead of calling LoadLibrary itself.
    return reinterpret_cast<FARPROC>(h);
}

/// Link-time trampoline: forwards to whatever installHook() stored.
FARPROC WINAPI trampolineHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    const HookFn active = g_activeHook.load(std::memory_order_acquire);
    return active ? active(dliNotify, pdli) : nullptr;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Public API
// -----------------------------------------------------------------------------
void installHook() noexcept { g_activeHook.store(&resolveHook, std::memory_order_release); }

void uninstallHook() noexcept { g_activeHook.store(nullptr, std::memory_order_release); }

bool hookInstalled() noexcept { return g_activeHook.load(std::memory_order_acquire) != nullptr; }

std::wstring moduleDirectory() noexcept {
    // The public accessor still returns a std::wstring - that is its
    // contract and its callers (tests, logging) are not on the hook path.
    // The hook itself uses moduleDirectoryCached(), which never allocates.
    try {
        const wchar_t* dir = moduleDirectoryCached();
        return dir ? std::wstring(dir) : std::wstring();
    } catch (...) {
        return {};
    }
}

HMODULE loadBesideModule(const char* dllName) noexcept {
    if (!dllName || !*dllName) {
        return nullptr;
    }
    // Only bare file names are honoured; a path would defeat the purpose.
    if (std::strchr(dllName, '\\') || std::strchr(dllName, '/') || std::strchr(dllName, ':')) {
        return nullptr;
    }
    // The cached, non-allocating accessor: this function runs under the
    // loader lock, where a heap allocation is a documented deadlock risk.
    const wchar_t* dir = moduleDirectoryCached();
    if (!dir || !*dir) {
        return nullptr;
    }

    // Build "<dir><name>" in a STACK buffer.  MultiByteToWideChar writes
    // straight into it, so there is no intermediate std::wstring either.
    wchar_t full[MAX_PATH * 4] = {};
    constexpr int kFullCapacity = static_cast<int>(sizeof(full) / sizeof(full[0]));

    const std::size_t dirLen = ::wcsnlen(dir, static_cast<std::size_t>(kFullCapacity));
    const int nameLen = static_cast<int>(std::strlen(dllName));
    // Leave room for the directory, the converted name and the NUL.
    if (dirLen == 0 || dirLen >= static_cast<std::size_t>(kFullCapacity) - 2u) {
        return nullptr;
    }
    std::memcpy(full, dir, dirLen * sizeof(wchar_t));

    const int room = kFullCapacity - static_cast<int>(dirLen) - 1;
    const int written = MultiByteToWideChar(CP_ACP, 0, dllName, nameLen, full + dirLen, room);
    if (written <= 0) {
        return nullptr;
    }
    full[dirLen + static_cast<std::size_t>(written)] = L'\0';

    // Skip the LoadLibrary attempt when the file is not there: cheaper and it
    // keeps the loader from touching the default search path twice.
    const DWORD attrs = GetFileAttributesW(full);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return nullptr;
    }

    // LOAD_WITH_ALTERED_SEARCH_PATH: dependencies of the loaded DLL are
    // searched starting in its own directory, so avcodec finds our avutil.
    const HMODULE h = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        return nullptr;
    }
    try {
        HookState& s = hookState();
        std::lock_guard<std::mutex> lock(s.statsMutex);
        ++s.stats.resolvedBeside;
        // This DOES allocate (assigning a std::wstring), so it is deliberately
        // the very last thing done and only after the library is already
        // loaded: by this point the loader lock is no longer held on our
        // behalf, and a failure here loses a diagnostic, nothing more.
        s.stats.lastResolvedPath = full;
    } catch (...) {
        // Statistics are best effort.
    }
    return h;
}

Stats stats() noexcept {
    try {
        HookState& s = hookState();
        std::lock_guard<std::mutex> lock(s.statsMutex);
        return s.stats;
    } catch (...) {
        return {};
    }
}

void resetStats() noexcept {
    try {
        HookState& s = hookState();
        std::lock_guard<std::mutex> lock(s.statsMutex);
        s.stats = Stats{};
    } catch (...) {
    }
}

}  // namespace osv::premiere::delayload

// -----------------------------------------------------------------------------
//  The linker-level hook pointer.  delayimp.lib references this symbol from
//  the delay-load helper and supplies a null default in a separate object
//  file; because this object is pulled into the link by installHook() (called
//  from the plug-in's DllMain) our definition wins.
// -----------------------------------------------------------------------------
extern "C" const PfnDliHook __pfnDliNotifyHook2 = &osv::premiere::delayload::trampolineHook;
