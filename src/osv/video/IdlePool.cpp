// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// IdlePool implementation: a mutex-guarded list of parked items, oldest
// first, and a reaper thread that releases them when their idle time runs
// out, memory runs short, or an item asks to go (see IdlePool.h).
//
// Every item released here is destroyed OUTSIDE the pool lock: tearing a
// decoder down joins libavcodec's worker threads and frees GPU surfaces,
// which can take tens of milliseconds, and nobody else should wait on the
// pool for that.

#include "IdlePool.h"

#include "osv/core/Log.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace osv::video::detail {

namespace {

using Clock = std::chrono::steady_clock;

/// How often the reaper looks at the memory situation (and asks every item
/// whether it must go) while anything is parked.  Each look is one
/// GlobalMemoryStatusEx call plus whatever the items check - for a GPU
/// decoder one cuMemGetInfo, a few microseconds.
constexpr std::chrono::milliseconds kPressurePoll{1000};

/// How long clear() waits for the reaper thread to finish exiting.  It only
/// has to notice the empty pool and return; the bound is there so a reaper
/// that a concurrent park() kept busy can never hang the caller.
constexpr unsigned long kReaperExitWaitMs = 2000;

/// A batch of items released by the pool, destroyed after the lock is gone.
using DeadItems = std::vector<std::unique_ptr<IdleItem>>;

}  // namespace

// -----------------------------------------------------------------------------
//  Memory pressure
// -----------------------------------------------------------------------------
bool systemMemoryUnderPressure(std::uint64_t minAvailableMiB) noexcept {
#if defined(_WIN32)
    // The OS's own verdict first.  The notification object is created once
    // and deliberately never closed: it lives as long as the process.
    static const HANDLE lowMemory = ::CreateMemoryResourceNotification(LowMemoryResourceNotification);
    if (lowMemory != nullptr) {
        BOOL low = FALSE;
        if (::QueryMemoryResourceNotification(lowMemory, &low) && low) {
            return true;
        }
    }
    if (minAvailableMiB > 0) {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        // Compare in MiB so a huge floor cannot overflow the byte count.
        if (::GlobalMemoryStatusEx(&status) && (status.ullAvailPhys >> 20) < minAvailableMiB) {
            return true;
        }
    }
    return false;
#elif defined(__APPLE__)
    // The kernel's own verdict first: the memorystatus pressure level is
    // 1 (normal), 2 (warning) or 4 (critical).  Warning is where a Mac lives
    // whenever it swaps a little, so only critical counts - the same "the OS
    // is about to start hurting" point Windows' low-memory notification marks.
    int level = 0;
    std::size_t levelSize = sizeof(level);
    if (::sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &levelSize, nullptr, 0) == 0 && level >= 4) {
        return true;
    }
    if (minAvailableMiB > 0) {
        // Available = free + inactive (file cache the kernel reclaims first)
        // + purgeable pages.  The host port is looked up once: every
        // mach_host_self() call adds a send right to it.
        static const mach_port_t host = ::mach_host_self();
        vm_statistics64_data_t vm{};
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        vm_size_t pageSize = 0;
        if (::host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS &&
            ::host_page_size(host, &pageSize) == KERN_SUCCESS && pageSize > 0) {
            const std::uint64_t pages = static_cast<std::uint64_t>(vm.free_count) +
                                        static_cast<std::uint64_t>(vm.inactive_count) +
                                        static_cast<std::uint64_t>(vm.purgeable_count);
            // Compare in MiB so a huge floor cannot overflow the byte count.
            if (((pages * static_cast<std::uint64_t>(pageSize)) >> 20) < minAvailableMiB) {
                return true;
            }
        }
    }
    return false;
#else
    (void)minAvailableMiB;
    return false;
#endif
}

// =============================================================================
//  State (shared with the reaper, which may outlive the pool object)
// =============================================================================
struct IdlePool::State {
    struct Entry {
        std::wstring key;
        std::unique_ptr<IdleItem> item;
        Clock::time_point parkedAt;
    };

    const char* name = "pool";    ///< For log lines (a string literal).
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<Entry> entries;   ///< Oldest first.
    IdlePoolLimits limits;
    IdlePoolStats stats;
    bool reaperRunning = false;
    bool stopping = false;        ///< The owning pool is gone: park nothing, reaper exits.
    /// Windows: handle of the most recent reaper thread (HANDLE), kept so
    /// clear() can wait for it to be gone.  Closed when the next reaper starts
    /// (the previous one has exited by then) and with the state.
    void* reaperThread = nullptr;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
#if defined(_WIN32)
        if (reaperThread) {
            ::CloseHandle(static_cast<HANDLE>(reaperThread));
        }
#endif
    }

    // ---- helpers; every one expects `mutex` to be held ------------------------

    /// Move items whose idle time has run out into `dead`.
    void collectExpired(Clock::time_point now, DeadItems& dead) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (now - it->parkedAt >= limits.idleTtl) {
                dead.push_back(std::move(it->item));
                it = entries.erase(it);
                ++stats.expired;
            } else {
                ++it;
            }
        }
    }

    /// Move items that ask to be released (or are empty) into `dead`.
    void collectByItem(DeadItems& dead) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (!it->item || it->item->mustRelease(limits.minFreeDeviceMemoryMiB)) {
                dead.push_back(std::move(it->item));
                it = entries.erase(it);
                ++stats.releasedByItem;
            } else {
                ++it;
            }
        }
    }

    /// Move every item into `dead`, counting them in `counter`.
    void collectAll(DeadItems& dead, std::uint64_t& counter) {
        for (Entry& e : entries) {
            dead.push_back(std::move(e.item));
            ++counter;
        }
        entries.clear();
    }

    /// Move the oldest items into `dead` until the count fits the limit.
    void collectOverCapacity(DeadItems& dead) {
        while (entries.size() > limits.maxIdle && !entries.empty()) {
            dead.push_back(std::move(entries.front().item));
            entries.erase(entries.begin());
            ++stats.evictedForRoom;
        }
    }

    /// Everything the reaper and trim() release: expired items, everything
    /// under system memory pressure, and items that ask to go.
    void collectDue(Clock::time_point now, DeadItems& dead) {
        collectExpired(now, dead);
        if (!entries.empty() && systemMemoryUnderPressure(limits.minAvailableMemoryMiB)) {
            collectAll(dead, stats.releasedForMemory);
        }
        collectByItem(dead);
    }

    /// When the oldest parked item expires (time_point::max when none).
    [[nodiscard]] Clock::time_point nextExpiry() const {
        Clock::time_point next = Clock::time_point::max();
        for (const Entry& e : entries) {
            next = std::min(next, e.parkedAt + limits.idleTtl);
        }
        return next;
    }
};

namespace {

// =============================================================================
//  The reaper
// =============================================================================

/// The reaper's loop.  Runs until the pool is empty (or its owner is gone).
void reaperLoop(const std::shared_ptr<IdlePool::State>& s) noexcept {
    if (!s) {
        return;
    }
    try {
        std::unique_lock<std::mutex> lock(s->mutex);
        for (;;) {
            if (s->stopping || s->entries.empty()) {
                s->reaperRunning = false;
                return;
            }
            const Clock::time_point now = Clock::now();
            DeadItems dead;
            s->collectDue(now, dead);
            if (!dead.empty()) {
                // Tear down without the lock, then look again.
                const std::size_t released = dead.size();
                const char* name = s->name;
                lock.unlock();
                dead.clear();
                log::debug("video: {} released {} idle item(s)", name, released);
                lock.lock();
                continue;
            }
            // Sleep until the next expiry, but look at memory at least once a
            // second while anything is parked.
            const Clock::time_point wake = std::min(s->nextExpiry(), now + kPressurePoll);
            s->cv.wait_until(lock, wake);
        }
    } catch (...) {
        // A mutex or allocation failure: stop reaping.  The pool still
        // trims lazily on every park() / take(), so nothing is lost for good.
        try {
            std::lock_guard<std::mutex> guard(s->mutex);
            s->reaperRunning = false;
        } catch (...) {
        }
    }
}

#if defined(_WIN32)
/// What the reaper thread is started with.
struct ReaperContext {
    std::shared_ptr<IdlePool::State> state;
    HMODULE module = nullptr;  ///< Reference on the module holding this code (released on exit).
};

/// Thread entry.  The module reference taken when the thread was started
/// keeps this code mapped while it runs; FreeLibraryAndExitThread drops it
/// and ends the thread in one call, so the module may unload at that moment
/// without the thread ever executing another instruction of it.
DWORD WINAPI reaperEntry(LPVOID param) {
    HMODULE module = nullptr;
    {
        std::unique_ptr<ReaperContext> context(static_cast<ReaperContext*>(param));
        if (context) {
            module = context->module;
            std::shared_ptr<IdlePool::State> state = std::move(context->state);
            reaperLoop(state);
        }
        // `state` and `context` are released here, while the module is
        // still pinned.
    }
    if (module) {
        ::FreeLibraryAndExitThread(module, 0);
    }
    return 0;
}
#endif

/// Start the reaper.  `s->mutex` must be held and `s->reaperRunning` false.
void startReaperLocked(const std::shared_ptr<IdlePool::State>& s) noexcept {
    if (!s) {
        return;
    }
    s->reaperRunning = true;
#if defined(_WIN32)
    // Pin the module that holds reaperEntry for as long as the thread lives.
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                              reinterpret_cast<LPCWSTR>(reinterpret_cast<const void*>(&reaperEntry)), &module) ||
        !module) {
        s->reaperRunning = false;
        log::warn("video: {} cannot pin its module ({}); idle items expire only when the pool is used", s->name,
                  static_cast<unsigned long>(::GetLastError()));
        return;
    }
    ReaperContext* context = nullptr;
    try {
        context = new ReaperContext{s, module};
    } catch (...) {
        ::FreeLibrary(module);
        s->reaperRunning = false;
        return;
    }
    HANDLE thread = ::CreateThread(nullptr, 0, &reaperEntry, context, 0, nullptr);
    if (!thread) {
        delete context;
        ::FreeLibrary(module);
        s->reaperRunning = false;
        log::warn("video: {} cannot start its reaper; idle items expire only when the pool is used", s->name);
        return;
    }
    // The thread runs on its own; nobody joins it.  Its handle is kept only
    // so clear() can wait for it; the previous reaper's handle is done with
    // (that thread cleared reaperRunning before it exited).
    if (s->reaperThread) {
        ::CloseHandle(static_cast<HANDLE>(s->reaperThread));
    }
    s->reaperThread = thread;
#else
    // The reaper is detached, so the image holding its code must never be
    // unmapped under it.  POSIX has no FreeLibraryAndExitThread to hand a
    // reference back from the last instruction of a thread, so the image
    // (a plug-in bundle, or the executable itself) is marked never-unload
    // once instead: RTLD_NOLOAD finds it without loading anything and
    // RTLD_NODELETE makes a later dlclose() leave it mapped.  Best effort:
    // a main executable (osvtool, the tests) is never unloaded anyway, and
    // some loaders refuse to hand out a handle to it by path.
    static const bool pinned = []() noexcept {
        Dl_info info{};
        if (::dladdr(reinterpret_cast<const void*>(&reaperLoop), &info) == 0 || !info.dli_fname) {
            return false;
        }
        return ::dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD | RTLD_NODELETE) != nullptr;
    }();
    if (!pinned) {
        log::debug("video: {} could not mark its module never-unload", s->name);
    }
    try {
        std::thread([state = s]() noexcept { reaperLoop(state); }).detach();
    } catch (...) {
        s->reaperRunning = false;
    }
#endif
}

}  // namespace

// =============================================================================
//  IdlePool
// =============================================================================
IdlePool::IdlePool(const char* name, const IdlePoolLimits& limits) : m_state(std::make_shared<State>()) {
    m_state->name = name ? name : "pool";
    m_state->limits = limits;
}

IdlePool::~IdlePool() {
    if (!m_state) {
        return;
    }
    DeadItems dead;
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->stopping = true;
        m_state->collectAll(dead, m_state->stats.cleared);
    } catch (...) {
        // Locking failed: nothing can be done safely; the items are released
        // with the state when the reaper lets go of it.
    }
    m_state->cv.notify_all();
    dead.clear();
    // The reaper, if running, holds its own reference to the state and exits
    // at its next wake-up because `stopping` is set.
}

std::unique_ptr<IdleItem> IdlePool::take(const std::wstring& key) noexcept {
    if (!m_state) {
        return nullptr;
    }
    DeadItems dead;
    std::unique_ptr<IdleItem> found;
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->collectExpired(Clock::now(), dead);
        if (!key.empty()) {
            // Newest first: the item parked last is the warmest.  A match
            // that asks to be released (its context died, its device is out
            // of memory) is released instead and the search goes on.
            for (auto it = m_state->entries.end(); it != m_state->entries.begin();) {
                --it;
                if (it->key != key) {
                    continue;
                }
                if (!it->item || it->item->mustRelease(m_state->limits.minFreeDeviceMemoryMiB)) {
                    dead.push_back(std::move(it->item));
                    it = m_state->entries.erase(it);
                    ++m_state->stats.releasedByItem;
                    continue;
                }
                found = std::move(it->item);
                m_state->entries.erase(it);
                break;
            }
        }
        if (found) {
            ++m_state->stats.hits;
        } else {
            ++m_state->stats.misses;
        }
    } catch (...) {
        // Out of memory or a mutex failure: behave as a miss; the caller opens.
        found.reset();
    }
    dead.clear();
    return found;
}

bool IdlePool::park(std::wstring key, std::unique_ptr<IdleItem> item) noexcept {
    if (!m_state) {
        return false;
    }
    DeadItems dead;
    bool kept = false;
    try {
        bool wake = false;
        {
            std::lock_guard<std::mutex> guard(m_state->mutex);
            const Clock::time_point now = Clock::now();
            m_state->collectExpired(now, dead);
            if (!item || key.empty() || m_state->stopping || m_state->limits.maxIdle == 0) {
                ++m_state->stats.rejected;
            } else if (systemMemoryUnderPressure(m_state->limits.minAvailableMemoryMiB)) {
                // Short of memory: this item goes, and so does everything else.
                m_state->collectAll(dead, m_state->stats.releasedForMemory);
                ++m_state->stats.rejected;
            } else if (item->mustRelease(m_state->limits.minFreeDeviceMemoryMiB)) {
                // The item itself says it must not be kept (e.g. its device
                // is already short of memory); anything else parked on that
                // device is judged by the same test below.
                m_state->collectByItem(dead);
                ++m_state->stats.rejected;
            } else {
                m_state->entries.push_back(State::Entry{std::move(key), std::move(item), now});
                ++m_state->stats.parked;
                kept = true;
                m_state->collectOverCapacity(dead);
                if (!m_state->reaperRunning) {
                    startReaperLocked(m_state);
                }
                wake = true;
            }
        }
        if (wake) {
            // The reaper may be sleeping towards a later expiry than this one.
            m_state->cv.notify_all();
        }
    } catch (...) {
        kept = false;
    }
    // A refused item (still in `item`) and anything released above are
    // destroyed here, outside the lock.
    item.reset();
    dead.clear();
    return kept;
}

void IdlePool::trim() noexcept {
    if (!m_state) {
        return;
    }
    DeadItems dead;
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->collectDue(Clock::now(), dead);
    } catch (...) {
    }
    dead.clear();
}

void IdlePool::clear() noexcept {
    if (!m_state) {
        return;
    }
    DeadItems dead;
#if defined(_WIN32)
    HANDLE reaper = nullptr;
#endif
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->collectAll(dead, m_state->stats.cleared);
#if defined(_WIN32)
        // A private copy of the reaper's handle to wait on: the stored one may
        // be closed by a reaper started later, while this call still waits.
        if (m_state->reaperRunning && m_state->reaperThread) {
            if (!::DuplicateHandle(::GetCurrentProcess(), static_cast<HANDLE>(m_state->reaperThread),
                                   ::GetCurrentProcess(), &reaper, SYNCHRONIZE, FALSE, 0)) {
                reaper = nullptr;
            }
        }
#endif
    } catch (...) {
    }
    // Wake the reaper so it sees the empty pool and exits now, releasing its
    // module reference, instead of at its next scheduled look.
    m_state->cv.notify_all();
    dead.clear();
#if defined(_WIN32)
    // Wait (bounded) until the reaper thread is really gone, so a host that
    // unloads the module right after this call unloads it for real.
    if (reaper) {
        ::WaitForSingleObject(reaper, kReaperExitWaitMs);
        ::CloseHandle(reaper);
    }
#endif
}

void IdlePool::setLimits(const IdlePoolLimits& limits) noexcept {
    if (!m_state) {
        return;
    }
    DeadItems dead;
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->limits = limits;
        m_state->collectExpired(Clock::now(), dead);
        m_state->collectOverCapacity(dead);
    } catch (...) {
    }
    m_state->cv.notify_all();
    dead.clear();
}

IdlePoolLimits IdlePool::limits() const noexcept {
    if (!m_state) {
        return {};
    }
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        return m_state->limits;
    } catch (...) {
        return {};
    }
}

IdlePoolStats IdlePool::stats() const noexcept {
    if (!m_state) {
        return {};
    }
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        return m_state->stats;
    } catch (...) {
        return {};
    }
}

std::size_t IdlePool::idleCount() const noexcept {
    if (!m_state) {
        return 0;
    }
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        return m_state->entries.size();
    } catch (...) {
        return 0;
    }
}

bool IdlePool::reaperRunning() const noexcept {
    if (!m_state) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        return m_state->reaperRunning;
    } catch (...) {
        return false;
    }
}

}  // namespace osv::video::detail
