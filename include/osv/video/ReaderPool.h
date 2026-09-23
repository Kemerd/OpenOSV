// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReaderPool: a small process-wide pool of warm DualStreamReaders.
//
// Why it exists
// -------------
// Premiere quiets a clip it is not reading from (imQuietFile) and wakes it
// again seconds later, and every Source Settings change makes it open the
// clip a second time with a fresh importer instance.  Each of those used to
// build a new reader from nothing: on the sample clip ~200 ms of D3D11
// device creation, hardware surface allocation and a probe decode, five to
// ten times in a short session.  A reader that was just put down is exactly
// what the next open of the same file needs, so instead of destroying it the
// importer parks it here and the next open takes it back - already
// initialised, its devices alive, its decode position intact.
//
// Rules
// -----
//   * Exclusive: take() hands a reader to ONE caller and removes it from the
//     pool.  A reader (and its two per-lens decoders) is never shared by two
//     users at once - the pool only holds readers nobody is using.
//   * Exact match: a parked reader serves only a request for the same file
//     VERSION (absolute path, size and modification time), the same tracks and
//     the same DecoderOptions (deferFirstFrame aside, which only affects
//     open() itself).  A rewritten clip never gets its predecessor's decoder.
//   * Bounded: at most Limits::maxIdleReaders readers are parked (oldest out
//     first), each for at most Limits::idleTtl.  An idle hardware reader
//     holds GPU surfaces - ~1.4 GB of VRAM for a 6K dual-HEVC clip with the
//     importer's four decoder threads per lens - and an idle software one
//     holds its frame pools in RAM, so the bound is deliberately small.
//   * Memory pressure: while the system is short of memory (the OS low-memory
//     notification, or less physical memory available than
//     Limits::minAvailableMemoryMiB) nothing is parked and everything parked
//     is released.  GPU memory is not measured: the count and time bounds
//     are what limit the VRAM an idle hardware reader holds.
//   * Self-cleaning: a reaper thread releases expired readers even if nobody
//     touches the pool again, and exits when the pool is empty.  While it
//     runs it holds a reference on the module that contains this code, so a
//     host that unloads the module cannot pull the code out from under it;
//     it drops that reference as it exits.  Nothing is ever released from a
//     static destructor, where FFmpeg's thread joins could deadlock on the
//     loader lock.
//   * clear() releases everything at once.  A host calls it when it shuts
//     the decoding side down (the importer's imShutdown); it is not required
//     for correctness, only for giving the memory back immediately.
//
// Thread-safe: every member function may be called from any thread.

#pragma once

#include "osv/meta/FormatInfo.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace osv::video {

class ReaderPool {
public:
    /// How much the pool may hold, and for how long.
    struct Limits {
        /// Readers kept at most (0 disables parking altogether).
        std::size_t maxIdleReaders = 2;
        /// How long a parked reader waits for a taker before it is released.
        /// Premiere's quiet / unquiet cycles in a real session were 10-75 s
        /// apart; a minute covers most of them without holding GPU memory
        /// for long after a clip has really been put away.
        std::chrono::milliseconds idleTtl{60000};
        /// Release everything and park nothing while less physical memory
        /// than this is available (GlobalMemoryStatusEx::ullAvailPhys), in
        /// MiB.  An absolute floor rather than a load percentage: Premiere
        /// routinely fills most of RAM with its own caches, so "90 % in use"
        /// is normal on a 32 GB machine and says nothing about pressure,
        /// while under 2 GiB available a parked software reader (~1.7 GB of
        /// frame pools) really would push the system towards paging.  0
        /// disables the check; the OS low-memory notification is honoured
        /// either way.
        std::uint64_t minAvailableMemoryMiB = 2048;
    };

    /// Counters since the pool was created (for logs and tests).
    struct Stats {
        std::uint64_t parked = 0;           ///< Readers accepted by park().
        std::uint64_t rejected = 0;         ///< Readers park() refused (not open, unknown identity, pressure, disabled).
        std::uint64_t hits = 0;             ///< take() calls that returned a reader.
        std::uint64_t misses = 0;           ///< take() calls that found nothing.
        std::uint64_t expired = 0;          ///< Readers released because their idle time ran out.
        std::uint64_t evictedForRoom = 0;   ///< Readers released to stay within maxIdleReaders.
        std::uint64_t releasedForMemory = 0;///< Readers released under memory pressure.
        std::uint64_t cleared = 0;          ///< Readers released by clear().
    };

    /// A private pool (tests, tools).  The process-wide one is instance().
    ReaderPool();
    explicit ReaderPool(const Limits& limits);

    /// Releases every parked reader and tells the reaper to stop.
    ~ReaderPool();

    ReaderPool(const ReaderPool&) = delete;
    ReaderPool& operator=(const ReaderPool&) = delete;

    /// The process-wide pool.  Created on first use and deliberately never
    /// destroyed (see the rules above); clear() empties it.
    [[nodiscard]] static ReaderPool& instance();

    /// Take a parked reader that was opened on the current version of `path`
    /// with `format`'s tracks and `options`, or nullptr when there is none
    /// (including when the file cannot be stat'ed).  The most recently parked
    /// match is returned: it is the warmest.  Never throws.
    [[nodiscard]] std::unique_ptr<DualStreamReader> take(const std::filesystem::path& path,
                                                         const meta::FormatInfo& format,
                                                         const DecoderOptions& options) noexcept;

    /// Park `reader` for a later take().  Returns true when the pool kept it.
    /// On false the reader has already been released - a caller never gets
    /// it back either way.  A reader that is not open or whose file identity
    /// is unknown is refused.  Never throws.
    bool park(std::unique_ptr<DualStreamReader> reader) noexcept;

    /// Release expired readers now, and everything if memory is short.
    void trim() noexcept;

    /// Release every parked reader now, then wait (at most two seconds) for
    /// the reaper thread to finish exiting, so a host that unloads the module
    /// right afterwards really unloads it.  The pool stays usable.  This is
    /// what a host's shutdown path calls (the importer's imShutdown); never
    /// call it from DllMain, where waiting for a thread deadlocks.
    void clear() noexcept;

    /// Change the limits; anything over the new bounds is released at once.
    void setLimits(const Limits& limits) noexcept;
    [[nodiscard]] Limits limits() const noexcept;

    [[nodiscard]] Stats stats() const noexcept;

    /// Readers parked right now.
    [[nodiscard]] std::size_t idleCount() const noexcept;

    /// True when the reaper thread is running (tests).
    [[nodiscard]] bool reaperRunning() const noexcept;

    /// The pool's shared state.  Opaque: it is named here only so the
    /// implementation's reaper thread, which may outlive a private pool, can
    /// hold a reference to it.
    struct State;

private:
    std::shared_ptr<State> m_state;
};

}  // namespace osv::video
