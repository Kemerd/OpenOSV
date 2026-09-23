// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReaderPool implementation: a mutex-guarded list of parked readers, oldest
// first, and a reaper thread that releases them when their idle time runs out
// or memory runs short (see ReaderPool.h for the rules and why they are what
// they are).
//
// Every reader released by the pool is destroyed OUTSIDE the pool lock:
// tearing a decoder down joins libavcodec's worker threads and frees GPU
// surfaces, which can take tens of milliseconds, and nobody else should wait
// on the pool for that.

#include "osv/video/ReaderPool.h"

#include "FileIdentity.h"
#include "osv/core/Log.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <mutex>
#include <string>
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
#endif

namespace osv::video {

namespace {

using Clock = std::chrono::steady_clock;

/// How often the reaper looks at the memory situation while it holds
/// readers.  The check itself is a single GlobalMemoryStatusEx call.
constexpr std::chrono::milliseconds kPressurePoll{1000};

/// How long clear() waits for the reaper thread to finish exiting.  It only
/// has to notice the empty pool and return; the bound is there so a reaper
/// that a concurrent park() kept busy can never hang the caller.
constexpr unsigned long kReaperExitWaitMs = 2000;

// -----------------------------------------------------------------------------
//  Matching
// -----------------------------------------------------------------------------

/// Everything a parked reader must share with a request to be handed out for
/// it.  deferFirstFrame is deliberately absent: it only changes what open()
/// does, and an open reader is the same reader either way.
struct PoolKey {
    std::wstring file;                          ///< detail::fileIdentity (path|size|mtime).
    std::array<std::uint32_t, 2> tracks{};      ///< Lens tracks (the proxy track twice).
    bool sideBySide = false;
    HwAccel hw = HwAccel::None;
    int threads = 0;
    bool keepOnDevice = false;
    int cudaDeviceIndex = 0;
    void* cudaContext = nullptr;
    void* cudaStream = nullptr;
    bool containerSamples = false;
    bool shareHwDevice = true;
    int hwDeviceSlot = 0;

    bool operator==(const PoolKey&) const = default;
};

/// The key of a request / of a reader.
PoolKey makeKey(std::wstring file, const std::array<std::uint32_t, 2>& tracks, bool sideBySide,
                const DecoderOptions& o) {
    PoolKey k;
    k.file = std::move(file);
    k.tracks = tracks;
    k.sideBySide = sideBySide;
    k.hw = o.hw;
    k.threads = o.threads;
    k.keepOnDevice = o.keepOnDevice;
    k.cudaDeviceIndex = o.cudaDeviceIndex;
    k.cudaContext = o.cudaContext;
    k.cudaStream = o.cudaStream;
    k.containerSamples = o.useContainerSamples;
    k.shareHwDevice = o.shareHwDevice;
    k.hwDeviceSlot = o.hwDeviceSlot;
    return k;
}

/// The tracks DualStreamReader::open would decode for `format` (kept in step
/// with it: the proxy decodes its one track for both lenses).
std::array<std::uint32_t, 2> tracksFor(const meta::FormatInfo& format) noexcept {
    if (format.sideBySideProxy) {
        const std::uint32_t t = format.videoTrackIds[0] != 0 ? format.videoTrackIds[0] : 1u;
        return {t, t};
    }
    return {format.videoTrackIds[0], format.videoTrackIds[1]};
}

// -----------------------------------------------------------------------------
//  Memory pressure
// -----------------------------------------------------------------------------

/// True when the system is short of memory: the OS says so, or less than
/// `minAvailableMiB` of physical memory is available (0 = no floor).
bool memoryUnderPressure(std::uint64_t minAvailableMiB) noexcept {
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
#else
    (void)minAvailableMiB;
    return false;
#endif
}

}  // namespace

// =============================================================================
//  State (shared with the reaper, which may outlive a private pool)
// =============================================================================
struct ReaderPool::State {
    struct Entry {
        PoolKey key;
        std::unique_ptr<DualStreamReader> reader;
        Clock::time_point parkedAt;
    };

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<Entry> entries;   ///< Oldest first.
    Limits limits;
    Stats stats;
    bool reaperRunning = false;
    bool stopping = false;        ///< The owning pool is gone: park nothing, reaper exits.
    /// Windows: handle of the most recent reaper thread (HANDLE), kept so
    /// clear() can wait for it to be gone.  Closed when the next reaper starts
    /// (the previous one has exited by then); never closed for the process-
    /// wide pool's last reaper, which costs one handle for the process.
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

    /// Move readers whose idle time has run out into `dead`.
    void collectExpired(Clock::time_point now, std::vector<std::unique_ptr<DualStreamReader>>& dead) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (now - it->parkedAt >= limits.idleTtl) {
                dead.push_back(std::move(it->reader));
                it = entries.erase(it);
                ++stats.expired;
            } else {
                ++it;
            }
        }
    }

    /// Move every reader into `dead`, counting them in `counter`.
    void collectAll(std::vector<std::unique_ptr<DualStreamReader>>& dead, std::uint64_t& counter) {
        for (Entry& e : entries) {
            dead.push_back(std::move(e.reader));
            ++counter;
        }
        entries.clear();
    }

    /// Move the oldest readers into `dead` until the count fits the limit.
    void collectOverCapacity(std::vector<std::unique_ptr<DualStreamReader>>& dead) {
        while (entries.size() > limits.maxIdleReaders && !entries.empty()) {
            dead.push_back(std::move(entries.front().reader));
            entries.erase(entries.begin());
            ++stats.evictedForRoom;
        }
    }

    /// When the oldest parked reader expires (time_point::max when none).
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

/// The reaper's loop.  Runs until the pool is empty (or its owner is gone),
/// releasing readers as they expire and everything under memory pressure.
void reaperLoop(const std::shared_ptr<ReaderPool::State>& s) noexcept {
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
            std::vector<std::unique_ptr<DualStreamReader>> dead;
            s->collectExpired(now, dead);
            if (!s->entries.empty() && memoryUnderPressure(s->limits.minAvailableMemoryMiB)) {
                s->collectAll(dead, s->stats.releasedForMemory);
            }
            if (!dead.empty()) {
                // Tear down without the lock, then look again.
                const std::size_t released = dead.size();
                lock.unlock();
                dead.clear();
                log::debug("video: reader pool released {} idle reader(s)", released);
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
    std::shared_ptr<ReaderPool::State> state;
    HMODULE module = nullptr;   ///< Reference on the module holding this code (released on exit).
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
            std::shared_ptr<ReaderPool::State> state = std::move(context->state);
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
void startReaperLocked(const std::shared_ptr<ReaderPool::State>& s) noexcept {
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
        log::warn("video: reader pool cannot pin its module ({}); idle readers expire only when the pool is used",
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
        log::warn("video: reader pool cannot start its reaper; idle readers expire only when the pool is used");
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
    try {
        std::thread([state = s]() noexcept { reaperLoop(state); }).detach();
    } catch (...) {
        s->reaperRunning = false;
    }
#endif
}

}  // namespace

// =============================================================================
//  ReaderPool
// =============================================================================
ReaderPool::ReaderPool() : m_state(std::make_shared<State>()) {}

ReaderPool::ReaderPool(const Limits& limits) : m_state(std::make_shared<State>()) { m_state->limits = limits; }

ReaderPool::~ReaderPool() {
    if (!m_state) {
        return;
    }
    std::vector<std::unique_ptr<DualStreamReader>> dead;
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->stopping = true;
        m_state->collectAll(dead, m_state->stats.cleared);
    } catch (...) {
        // Locking failed: nothing can be done safely; the readers are
        // released with the state when the reaper lets go of it.
    }
    m_state->cv.notify_all();
    dead.clear();
    // The reaper, if running, holds its own reference to the state and exits
    // at its next wake-up because `stopping` is set.
}

ReaderPool& ReaderPool::instance() {
    // Never destroyed on purpose: releasing decoders from a static destructor
    // would join libavcodec's threads under the loader lock (see the header).
    static ReaderPool* const pool = new ReaderPool();
    return *pool;
}

std::unique_ptr<DualStreamReader> ReaderPool::take(const std::filesystem::path& path, const meta::FormatInfo& format,
                                                   const DecoderOptions& options) noexcept {
    if (!m_state) {
        return nullptr;
    }
    std::vector<std::unique_ptr<DualStreamReader>> dead;
    std::unique_ptr<DualStreamReader> found;
    try {
        // The identity is read outside the lock (two file-system calls).
        auto identity = detail::fileIdentity(path);
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->collectExpired(Clock::now(), dead);
        if (identity.ok()) {
            const PoolKey key = makeKey(std::move(identity).value(), tracksFor(format), format.sideBySideProxy, options);
            // Newest first: the reader parked last is the warmest.
            for (auto it = m_state->entries.rbegin(); it != m_state->entries.rend(); ++it) {
                if (it->key == key && it->reader && it->reader->isOpen()) {
                    found = std::move(it->reader);
                    m_state->entries.erase(std::next(it).base());
                    break;
                }
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

bool ReaderPool::park(std::unique_ptr<DualStreamReader> reader) noexcept {
    if (!m_state) {
        return false;
    }
    std::vector<std::unique_ptr<DualStreamReader>> dead;
    bool kept = false;
    try {
        const bool eligible = reader && reader->isOpen() && !reader->fileIdentity().empty();
        // Build the key before taking the lock (it copies strings).
        PoolKey key;
        if (eligible) {
            key = makeKey(reader->fileIdentity(), reader->trackIds(), reader->isSideBySide(), reader->options());
        }
        bool wake = false;
        {
            std::lock_guard<std::mutex> guard(m_state->mutex);
            const Clock::time_point now = Clock::now();
            m_state->collectExpired(now, dead);
            if (!eligible || m_state->stopping || m_state->limits.maxIdleReaders == 0) {
                ++m_state->stats.rejected;
            } else if (memoryUnderPressure(m_state->limits.minAvailableMemoryMiB)) {
                // Short of memory: this reader goes, and so does everything else.
                m_state->collectAll(dead, m_state->stats.releasedForMemory);
                ++m_state->stats.rejected;
            } else {
                m_state->entries.push_back(State::Entry{std::move(key), std::move(reader), now});
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
    // A refused reader (still in `reader`) and anything released above are
    // destroyed here, outside the lock.
    reader.reset();
    dead.clear();
    return kept;
}

void ReaderPool::trim() noexcept {
    if (!m_state) {
        return;
    }
    std::vector<std::unique_ptr<DualStreamReader>> dead;
    try {
        std::lock_guard<std::mutex> guard(m_state->mutex);
        m_state->collectExpired(Clock::now(), dead);
        if (!m_state->entries.empty() && memoryUnderPressure(m_state->limits.minAvailableMemoryMiB)) {
            m_state->collectAll(dead, m_state->stats.releasedForMemory);
        }
    } catch (...) {
    }
    dead.clear();
}

void ReaderPool::clear() noexcept {
    if (!m_state) {
        return;
    }
    std::vector<std::unique_ptr<DualStreamReader>> dead;
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
    // unloads the module right after this call unloads it for real instead of
    // leaving it pinned until the reaper's next wake-up.
    if (reaper) {
        ::WaitForSingleObject(reaper, kReaperExitWaitMs);
        ::CloseHandle(reaper);
    }
#endif
}

void ReaderPool::setLimits(const Limits& limits) noexcept {
    if (!m_state) {
        return;
    }
    std::vector<std::unique_ptr<DualStreamReader>> dead;
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

ReaderPool::Limits ReaderPool::limits() const noexcept {
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

ReaderPool::Stats ReaderPool::stats() const noexcept {
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

std::size_t ReaderPool::idleCount() const noexcept {
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

bool ReaderPool::reaperRunning() const noexcept {
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

}  // namespace osv::video
