// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Internal: the parking lot behind ReaderPool and GpuDecoderPool.
//
// Both pools do the same thing with different objects: hold a few expensive,
// fully initialised decoders that nobody is using, hand one back to the next
// caller that asks for exactly what it holds, and let go of them when they
// have waited too long or the machine needs the memory.  This class is that
// logic once, over type-erased items; the public pools are thin typed
// facades that build the match keys and the items.
//
// Rules (the facades document them for their callers):
//   * Exclusive: take() removes the item it returns.  An item is never in
//     the pool and in a caller's hands at the same time.
//   * Exact match: an item serves only a take() with the same key string.
//     Among several matches the most recently parked one is returned.
//   * Bounded: at most IdlePoolLimits::maxIdle items (oldest out first), each
//     for at most IdlePoolLimits::idleTtl.
//   * Memory pressure: while the OS reports low memory or less physical
//     memory than IdlePoolLimits::minAvailableMemoryMiB is available, nothing
//     is parked and everything parked is released.  Each item may also ask to
//     be released for a reason of its own (IdleItem::mustRelease: the GPU
//     decoder's device is short of VRAM, or its context is gone).
//   * Self-cleaning: a reaper thread releases expired items even when nobody
//     touches the pool again and exits when the pool is empty.  It pins the
//     module that holds this code while it runs and unpins it as it exits.
//     Nothing is released from a static destructor.
//   * Every released item is destroyed OUTSIDE the pool lock: tearing a
//     decoder down joins threads and frees GPU memory, and nobody else should
//     wait on the pool for that.
//
// Thread-safe: every member function may be called from any thread.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace osv::video::detail {

/// One parked object, type-erased.  Destroying it releases the object.
class IdleItem {
public:
    IdleItem() = default;
    virtual ~IdleItem() = default;
    IdleItem(const IdleItem&) = delete;
    IdleItem& operator=(const IdleItem&) = delete;

    /// True when the item must be released now for a reason of its own.
    /// `minFreeDeviceMemoryMiB` is IdlePoolLimits::minFreeDeviceMemoryMiB,
    /// for items that live on a device.  Called with the pool lock held, so
    /// it must be cheap, must not throw and must never call into the pool.
    [[nodiscard]] virtual bool mustRelease(std::uint64_t minFreeDeviceMemoryMiB) noexcept {
        (void)minFreeDeviceMemoryMiB;
        return false;
    }
};

/// How much a pool may hold, and for how long.
struct IdlePoolLimits {
    /// Items kept at most (0 disables parking altogether).
    std::size_t maxIdle = 2;
    /// How long a parked item waits for a taker before it is released.
    std::chrono::milliseconds idleTtl{60000};
    /// Release everything and park nothing while less physical memory than
    /// this is available, in MiB (0 = no floor; the OS low-memory
    /// notification is honoured either way).
    std::uint64_t minAvailableMemoryMiB = 2048;
    /// Handed to IdleItem::mustRelease (0 = no device floor).
    std::uint64_t minFreeDeviceMemoryMiB = 0;
};

/// Counters since the pool was created.
struct IdlePoolStats {
    std::uint64_t parked = 0;             ///< Items accepted by park().
    std::uint64_t rejected = 0;           ///< Items park() refused (null, no key, disabled, pressure, the item's own verdict).
    std::uint64_t hits = 0;               ///< take() calls that returned an item.
    std::uint64_t misses = 0;             ///< take() calls that found nothing usable.
    std::uint64_t expired = 0;            ///< Items released because their idle time ran out.
    std::uint64_t evictedForRoom = 0;     ///< Items released to stay within maxIdle.
    std::uint64_t releasedForMemory = 0;  ///< Items released under system memory pressure.
    std::uint64_t releasedByItem = 0;     ///< Items released because IdleItem::mustRelease said so.
    std::uint64_t cleared = 0;            ///< Items released by clear() or the pool's destruction.
};

class IdlePool {
public:
    /// `name` appears in log lines ("reader pool", "GPU decoder pool"); it
    /// must be a string literal (it is kept by pointer).
    IdlePool(const char* name, const IdlePoolLimits& limits);

    /// Releases every parked item and tells the reaper to stop.
    ~IdlePool();

    IdlePool(const IdlePool&) = delete;
    IdlePool& operator=(const IdlePool&) = delete;

    /// The most recently parked item under `key` that does not ask to be
    /// released, or nullptr.  Never throws.
    [[nodiscard]] std::unique_ptr<IdleItem> take(const std::wstring& key) noexcept;

    /// Park `item` under `key`.  True when the pool kept it; on false the
    /// item has already been released.  An empty key or a null item is
    /// refused.  Never throws.
    bool park(std::wstring key, std::unique_ptr<IdleItem> item) noexcept;

    /// Release expired items now, everything under memory pressure, and
    /// every item that asks to be released.
    void trim() noexcept;

    /// Release everything now, then wait (at most two seconds) for the reaper
    /// to finish exiting so the module can unload right afterwards.  Never
    /// call it from DllMain.
    void clear() noexcept;

    void setLimits(const IdlePoolLimits& limits) noexcept;
    [[nodiscard]] IdlePoolLimits limits() const noexcept;
    [[nodiscard]] IdlePoolStats stats() const noexcept;
    [[nodiscard]] std::size_t idleCount() const noexcept;
    [[nodiscard]] bool reaperRunning() const noexcept;

    /// Shared with the reaper thread, which may outlive the pool object.
    struct State;

private:
    std::shared_ptr<State> m_state;
};

/// True when the system is short of memory: the OS says so, or less than
/// `minAvailableMiB` of physical memory is available (0 = no floor).
[[nodiscard]] bool systemMemoryUnderPressure(std::uint64_t minAvailableMiB) noexcept;

}  // namespace osv::video::detail
