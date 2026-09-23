// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GpuDecoderPool: a small process-wide pool of warm GpuClipDecoders.
//
// Why it exists
// -------------
// The importer renders its own frame on the GPU: NVDEC into VRAM, stitched in
// place (video::GpuClipDecoder).  Premiere quiets a clip it is not reading
// and wakes it seconds later, and opens a second importer instance of a clip
// on every Source Settings change.  Each of those used to destroy the decoder
// and open a new one on the next frame: two NVDEC decoders, a primed frame 0
// and a decode from the GOP start, ~100 ms before the frame the user is
// looking at comes back.  A decoder that was just put down is exactly what
// the next open of the same file needs, so the importer parks it here and
// takes it back - NVDEC decoders alive, decode position intact, the frames it
// showed last still in VRAM.
//
// Rules (the ReaderPool rules, plus the ones VRAM needs)
// ------------------------------------------------------
//   * Exclusive: take() hands a decoder to ONE caller and removes it from the
//     pool.  A decoder is never used by two callers at once.
//   * Exact match: a parked decoder serves only a request for the same file
//     VERSION (absolute path, size, modification time), the same lens tracks
//     and the same GpuDecoderOptions (context, device, stream, budget,
//     decode-ahead, threads).
//   * Only decoders that own their context: a decoder is parked only when it
//     runs in the device's PRIMARY context and holds its own retain on it
//     (GpuDecoderOptions::cuContext was nullptr) and uses its own streams.
//     That retain is what guarantees a parked decoder never outlives its CUDA
//     context: the context cannot be destroyed while the decoder holds it.  A
//     decoder in a caller's context (the effect's direct path decodes into
//     Premiere's) is refused and released at once, because nothing here can
//     know when its owner tears that context down.  Should the primary
//     context be reset anyway (cudaDeviceReset elsewhere in the process), the
//     decoder reports its context dead and is released instead of handed out.
//   * Small in VRAM while parked: park() first calls
//     GpuClipDecoder::trimForIdle(Limits::cachedFramesKept) - decode-ahead
//     stops and the frame cache gives back all but the most recently used
//     frames.  The NVDEC decoders stay (~800 MiB for a 6K clip).
//   * Bounded: at most Limits::maxIdleDecoders parked (oldest out first),
//     each for at most Limits::idleTtl.
//   * VRAM pressure: whenever the device of a parked decoder has less free
//     VRAM than Limits::minFreeVramMiB (cuMemGetInfo, checked at park, at
//     take and by the reaper once a second), that decoder is released.  A
//     decoder is not parked at all when its device is already below it.
//   * System memory pressure: as ReaderPool (Limits::minAvailableMemoryMiB).
//   * Self-cleaning: a reaper thread releases expired decoders even when
//     nobody touches the pool again (see IdlePool.h).  Nothing is ever
//     released from a static destructor.
//   * clear() releases everything at once.  The importer calls it at
//     imShutdown BEFORE the renderer pool goes (HostContext::shutdown), while
//     the CUDA driver and the primary context are certainly alive.
//
// Thread-safe: every member function may be called from any thread.

#pragma once

#include "osv/meta/FormatInfo.h"
#include "osv/video/GpuClipDecoder.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace osv::video {

namespace detail {
/// Internal: the type-erased parking lot both pools are built on.
class IdlePool;
}  // namespace detail

class GpuDecoderPool {
public:
    /// How much the pool may hold, for how long, and when to let go early.
    struct Limits {
        /// Decoders kept at most (0 disables parking altogether).
        std::size_t maxIdleDecoders = 2;
        /// How long a parked decoder waits for a taker before it is released.
        std::chrono::milliseconds idleTtl{60000};
        /// Release everything while less physical memory than this is
        /// available, in MiB (0 = no floor; see ReaderPool::Limits).
        std::uint64_t minAvailableMemoryMiB = 2048;
        /// Release a parked decoder (and park none) while its device has
        /// less free VRAM than this, in MiB.  A parked 6K decoder holds
        /// ~800 MiB of NVDEC surfaces plus its kept frames (~53 MiB each);
        /// below 2 GiB free that memory is worth more to the host than a
        /// faster reopen.  0 disables the check.
        std::uint64_t minFreeVramMiB = 2048;
        /// Frames of the VRAM cache a parked decoder keeps (the most recently
        /// used ones: the frame the user was looking at and its neighbours).
        std::uint32_t cachedFramesKept = 4;
    };

    /// Counters since the pool was created (for logs and tests).
    struct Stats {
        std::uint64_t parked = 0;             ///< Decoders accepted by park().
        std::uint64_t rejected = 0;           ///< Decoders park() refused (see park()).
        std::uint64_t hits = 0;               ///< take() calls that returned a decoder.
        std::uint64_t misses = 0;             ///< take() calls that found nothing usable.
        std::uint64_t expired = 0;            ///< Decoders released because their idle time ran out.
        std::uint64_t evictedForRoom = 0;     ///< Decoders released to stay within maxIdleDecoders.
        std::uint64_t releasedForMemory = 0;  ///< Decoders released under system memory pressure.
        std::uint64_t releasedForVram = 0;    ///< Decoders released because their device ran short of VRAM (or their context died).
        std::uint64_t cleared = 0;            ///< Decoders released by clear().
        std::uint64_t trimmedBytes = 0;       ///< VRAM given back by trimForIdle() on park.
    };

    /// A private pool (tests, tools).  The process-wide one is instance().
    GpuDecoderPool();
    explicit GpuDecoderPool(const Limits& limits);

    /// Releases every parked decoder and tells the reaper to stop.
    ~GpuDecoderPool();

    GpuDecoderPool(const GpuDecoderPool&) = delete;
    GpuDecoderPool& operator=(const GpuDecoderPool&) = delete;

    /// The process-wide pool.  Created on first use and deliberately never
    /// destroyed; clear() empties it.
    [[nodiscard]] static GpuDecoderPool& instance();

    /// True when `decoder` may be parked at all: open, its file identity
    /// known, running in a primary context it retains itself, on its own
    /// streams.
    [[nodiscard]] static bool poolable(const GpuClipDecoder& decoder) noexcept;

    /// Take a parked decoder that was opened on the current version of `path`
    /// with `format`'s lens tracks and `options`, or nullptr when there is
    /// none (including when the file cannot be stat'ed, and for options that
    /// name a caller's context or stream - such decoders are never parked).
    /// The most recently parked match is returned.  A match whose context is
    /// gone or whose device is short of VRAM is released instead.  Never
    /// throws.
    [[nodiscard]] std::unique_ptr<GpuClipDecoder> take(const std::filesystem::path& path,
                                                       const meta::FormatInfo& format,
                                                       const GpuDecoderOptions& options) noexcept;

    /// Park `decoder` for a later take().  Returns true when the pool kept
    /// it; on false it has already been released - a caller never gets it
    /// back either way.  Refused: null, not poolable(), parking disabled,
    /// system memory or its device's VRAM short.  Trims the decoder's cache
    /// (Limits::cachedFramesKept) before parking it.  Never throws.
    bool park(std::unique_ptr<GpuClipDecoder> decoder) noexcept;

    /// Release expired decoders now, everything if system memory is short,
    /// and every decoder whose device is short of VRAM.
    void trim() noexcept;

    /// Release every parked decoder now, then wait (at most two seconds) for
    /// the reaper thread to finish exiting.  The pool stays usable.  Never
    /// call it from DllMain.
    void clear() noexcept;

    /// Change the limits; anything over the new bounds is released at once
    /// (a raised VRAM floor takes effect at the next trim(), take(), park()
    /// or reaper look, i.e. within a second).
    void setLimits(const Limits& limits) noexcept;
    [[nodiscard]] Limits limits() const noexcept;

    [[nodiscard]] Stats stats() const noexcept;

    /// Decoders parked right now.
    [[nodiscard]] std::size_t idleCount() const noexcept;

    /// True when the reaper thread is running (tests).
    [[nodiscard]] bool reaperRunning() const noexcept;

private:
    /// The parking logic, shared with ReaderPool (src/osv/video/IdlePool.h).
    std::unique_ptr<detail::IdlePool> m_core;
    /// Limits::cachedFramesKept (the core knows nothing about frame caches).
    std::atomic<std::uint32_t> m_cachedFramesKept{4};
    /// Stats::trimmedBytes.
    std::atomic<std::uint64_t> m_trimmedBytes{0};
};

}  // namespace osv::video
