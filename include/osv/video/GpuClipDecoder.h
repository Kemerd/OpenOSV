// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GpuClipDecoder: both lenses of a dual-fisheye .OSV clip decoded by NVDEC
// straight into the VRAM of a caller-supplied CUDA context, with a GOP-aware
// VRAM frame cache and a decode-ahead worker.  Work package WP-A of the
// direct GPU pipeline (docs/DIRECT_GPU.md).
//
// Why it exists
//   Parking on a frame in Premiere used to cost 50-60 ms with NVDEC (decode,
//   then a host copy) or ~950 ms in software, because every request decoded
//   from the previous sync sample and threw away everything it decoded on the
//   way.  The hardware floor is ~2.1 ms per frame pair when the frames stay on
//   the GPU.  This decoder keeps them there, keeps every frame it decodes, and
//   decodes ahead while the host plays forward, so a park inside a GOP that
//   was already walked is a cache hit (well under a millisecond) and playback
//   runs at NVDEC speed.
//
// Design notes
//   * One CUDA context, supplied by the caller (Premiere's own), or the
//     device's primary context when none is given.  A private context is
//     never created: each would cost hundreds of MB, and memory allocated in
//     one context is not usable by kernels running in another.
//   * CUDA driver API only (cuda.h / nvcuda.dll); no cudart dependency in
//     anything a plug-in loads.  The context is pushed and popped around every
//     driver call, on every thread, and nothing ever rebinds a thread to the
//     primary context behind the host's back.
//   * Frames live in pooled VRAM slots this class owns (one pitched allocation
//     per frame pair, P010 per lens: luma plane plus interleaved CbCr plane,
//     one pitch).  Each decoded NVDEC surface is copied device-to-device into
//     a slot and handed straight back to FFmpeg, so the decoder's small
//     surface pool is never pinned by the cache.
//   * Frame index == container sample index, by construction (the lens
//     decoders are fed by the OpenOSV container parser).
//   * No exceptions escape; every fallible call returns Result / Status.
#pragma once

#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/video/PlanarFrame.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace osv::video {

namespace detail {
/// Internal: keeps one cache slot pinned (and the VRAM store alive) for as
/// long as any lease or FramePair copy refers to it.  Defined in
/// GpuClipDecoder.cpp.
struct GpuSlotPin;
}  // namespace detail

/// Everything that influences how a GpuClipDecoder is opened.
struct GpuDecoderOptions {
    /// The CUcontext to decode into, passed as void* so this header never
    /// needs cuda.h.  Every NVDEC surface, every cache slot and every event
    /// the decoder creates belongs to this context, so kernels running in it
    /// can read the frames directly.  nullptr = the primary context of
    /// `cudaDevice`, retained (cuDevicePrimaryCtxRetain) for the decoder's
    /// lifetime; its flags are left exactly as the process set them.  The
    /// caller keeps a supplied context alive until the decoder AND every
    /// lease it handed out are gone.
    void* cuContext = nullptr;

    /// The CUstream NVDEC post-processing, FFmpeg's surface copies and the
    /// copies into the cache are ordered on (it must belong to `cuContext`).
    /// nullptr = two non-blocking streams owned by the decoder, one per lens,
    /// so decoding never serialises against the host's own work.  A supplied
    /// stream is shared by both lenses and must stay valid for the decoder's
    /// lifetime; the decode-ahead worker enqueues onto it from its own thread.
    void* cuStream = nullptr;

    /// CUDA device ordinal whose primary context is used when `cuContext` is
    /// nullptr.  Ignored when a context is supplied (its device is queried).
    int cudaDevice = 0;

    /// VRAM the frame cache may occupy, in bytes.  0 = automatic:
    /// min(1.5 GiB, 20 % of the device's free VRAM measured after the NVDEC
    /// decoders were created).  The cache holds floor(budget / slotBytes)
    /// frame pairs (55 MB each for the 3000 x 3000 streams) and never
    /// allocates past that; a budget smaller than one frame pair fails open()
    /// with InvalidArgument.  NVDEC's own decode surfaces are not part of
    /// this budget (see GpuDecoderStats).
    std::size_t vramBudgetBytes = 0;

    /// Frames decoded ahead of the most recent request once the host is seen
    /// playing forward (a request for k followed by one for k + 1 .. k + 4).
    /// 0 disables the worker thread entirely.  Clamped so the window always
    /// leaves the cache room for the frames the host is holding; values above
    /// 256 are rejected as nonsensical.
    std::uint32_t decodeAhead = 8;

    /// libavcodec frame threads per lens decoder.  With NVDEC the threads
    /// parse and submit pictures while the hardware is still decoding the
    /// previous ones, which is what keeps both decode engines busy.  Every
    /// thread also adds one NVDEC decode surface (~26 MB at 3000 x 3000), so
    /// more is not free.  Measured on the RTX 5090 with the sample clip
    /// (sequential pairs/s with decode-ahead, cold park median, NVDEC VRAM
    /// of both lenses after open): 1 thread 457/s 66 ms 634 MiB;
    /// 2 470/s 59 ms 738 MiB; 3 524/s 46 ms 790 MiB; 4 520/s 49 ms 842 MiB;
    /// 6 519/s 47 ms 946 MiB.  3 is where the curve flattens.  0 =
    /// libavcodec's automatic choice (one per core, capped at 16 - far too
    /// many surfaces).  Values outside 0..16 are rejected.
    int decoderThreads = 3;
};

/// How acquire() obtained a frame.
enum class LeaseSource : std::uint8_t {
    CacheHit = 0,         ///< The frame was already in the VRAM cache when acquire() was called.
    WaitedForDecode = 1,  ///< Another thread (a concurrent acquire or the decode-ahead worker) was producing it; this call waited.
    Decoded = 2           ///< This call ran the decoder, from the previous sync sample or the decoder's current position.
};

/// Stable lower-case name of a LeaseSource ("hit", "waited", "decoded").
[[nodiscard]] const char* leaseSourceName(LeaseSource source) noexcept;

/// Counters and sizes of one GpuClipDecoder, as a consistent snapshot.
struct GpuDecoderStats {
    std::uint64_t acquires = 0;            ///< acquire() calls with a valid index, successful or not.
    std::uint64_t cacheHits = 0;           ///< Served from the cache without waiting.
    std::uint64_t waitedHits = 0;          ///< Served after waiting for another thread's decode.
    std::uint64_t foregroundDecodes = 0;   ///< acquire() calls that had to run the decoder.
    std::uint64_t framesDecoded = 0;       ///< Frame pairs decoded in total (foreground + ahead).
    std::uint64_t framesDecodedAhead = 0;  ///< Frame pairs decoded speculatively by the worker.
    std::uint64_t restarts = 0;            ///< Decodes that had to (re)start at a sync sample.
    std::uint64_t evictions = 0;           ///< Cached frames dropped to make room (LRU).
    std::uint32_t cachedFrames = 0;        ///< Frame pairs currently in the cache.
    std::uint32_t leasedSlots = 0;         ///< Slots pinned by live leases / FramePair copies.
    std::uint32_t allocatedSlots = 0;      ///< Slots with VRAM behind them.
    std::uint32_t capacitySlots = 0;       ///< Maximum number of slots (budget / slotBytes).
    std::uint32_t decodeAheadWindow = 0;   ///< Effective decode-ahead window after clamping.
    std::size_t slotBytes = 0;             ///< VRAM of one frame pair (both lenses, pitched).
    std::size_t vramBytes = 0;             ///< allocatedSlots * slotBytes: what the cache occupies now.
    std::size_t budgetBytes = 0;           ///< The budget the capacity was derived from.
};

/// A pinned, fully decoded frame pair in the VRAM cache.
///
/// Lifetime rule (read this before using the pointers):
///   The device pointers in pair() stay valid, and the slot is never
///   overwritten, while the lease OR any copy of pair() (its owner fields
///   share the pin) is alive.  The decoder cannot see GPU work, so releasing
///   is a statement about the GPU:
///     * release() / destruction: the caller asserts that NO GPU work that
///       reads this frame is still pending (it completed, or was never
///       enqueued).  The slot may be refilled immediately.
///     * releaseAfter(stream): the caller has ENQUEUED its reads on `stream`
///       and releases right away.  An event is recorded on `stream` and the
///       decoder makes the slot's next overwrite wait for it on the GPU
///       (cuStreamWaitEvent), so the host never synchronises.  This is the
///       call a render thread wants: acquire, launch the kernel, releaseAfter.
///   Releasing before the reads are ordered in some stream is a data race
///   the decoder cannot detect: the next decode may overwrite the pixels
///   while a kernel reads them.
///
/// Data completeness: acquire() only returns once the copies into the slot
/// have completed, so pair() may be read from any stream of the decoder's
/// context without further synchronisation.
///
/// A lease is move-only.  Moving from it leaves an empty (invalid) lease.
class GpuFrameLease {
public:
    /// An empty lease: valid() is false and pair() is an empty FramePair.
    GpuFrameLease() noexcept;

    /// Releases the pin as release() does (no stream: see the class notes).
    ~GpuFrameLease();

    GpuFrameLease(GpuFrameLease&& other) noexcept;

    /// Releases the pin currently held (as release()) and takes `other`'s.
    GpuFrameLease& operator=(GpuFrameLease&& other) noexcept;

    GpuFrameLease(const GpuFrameLease&) = delete;
    GpuFrameLease& operator=(const GpuFrameLease&) = delete;

    /// True while the lease pins a frame.
    [[nodiscard]] bool valid() const noexcept;

    /// Frame (container sample) index of the pinned pair; 0 when empty.
    [[nodiscard]] std::uint32_t frameIndex() const noexcept;

    /// The pinned pair.  device[0] / device[1] are valid DeviceFrameRefs
    /// (P010: bitDepth 10, bitShift 6, luma plane plus interleaved CbCr at
    /// the same pitch, deviceIndex = the context's device).  lens[0] / lens[1]
    /// carry width, height, chroma size, bitDepth/bitShift, range, pts and
    /// frame index but NO host planes, which is exactly what
    /// render::RenderParamsBuilder accepts as a zero-copy device frame.
    /// lens[0] is the slave lens (track videoTrackIds[0]), lens[1] the master.
    /// Every owner field shares this lease's pin.  Empty when !valid().
    [[nodiscard]] const FramePair& pair() const noexcept;

    /// How acquire() obtained the frame.
    [[nodiscard]] LeaseSource source() const noexcept;

    /// Release after the GPU work already enqueued on `cuStream` (a CUstream
    /// of the decoder's context; nullptr = that context's NULL stream).  An
    /// event is recorded on the stream now and the slot's next overwrite waits
    /// for it on the GPU.  The lease is empty afterwards, even on failure.
    /// Errors: Gpu (the event could not be recorded, e.g. the stream belongs
    /// to another context: the host then waits for all work queued in the
    /// decoder's context before letting go, which is safe but blocks),
    /// InvalidArgument (empty lease).
    Status releaseAfter(void* cuStream) noexcept;

    /// Release now.  The caller asserts no pending GPU work reads the frame
    /// (see the class notes).  No-op on an empty lease.
    void release() noexcept;

private:
    friend class GpuClipDecoder;

    std::shared_ptr<detail::GpuSlotPin> m_pin;  ///< The pin (shared with m_pair's owner fields).
    FramePair m_pair{};                         ///< Device refs + frame geometry.
    LeaseSource m_source = LeaseSource::CacheHit;
};

/// NVDEC decoder for both lenses of a dual-track .OSV with a VRAM frame cache.
///
/// Thread safety: acquire(), isCached(), stats(), dropCachedFrames() and the
/// accessors may be called concurrently from any number of threads.  Leases
/// may be released on any thread, also after the decoder is destroyed (the
/// VRAM store stays alive until the last pin is gone).  The destructor must
/// not run concurrently with a call on the same object.
class GpuClipDecoder {
public:
    /// The automatic budget's absolute cap (1.5 GiB).
    static constexpr std::size_t kDefaultBudgetCapBytes = std::size_t{1536} << 20;

    /// The automatic budget's share of the free VRAM (20 %).
    static constexpr double kDefaultBudgetFreeShare = 0.20;

    /// Largest forward step between two requests that still counts as
    /// playing forward (a 59.94 clip in a 29.97 sequence steps by 2).
    static constexpr std::uint32_t kMaxSequentialStep = 4;

    /// Open the two lens tracks named by `format.videoTrackIds` and set up the
    /// cache.  Both lens decoders are primed (frame 0 is decoded, which
    /// creates the NVDEC decoders), so a broken stream or a GPU without
    /// HEVC Main10 NVDEC fails here instead of on the first render.  Frame 0
    /// is left in the cache and the decoder positioned on frame 1, so the
    /// first landing inside the first GOP continues instead of restarting.
    /// Errors: InvalidArgument (empty path, missing track ids, the LRF
    /// side-by-side proxy, options out of range, a budget smaller than one
    /// frame pair, a context that is not valid), Unsupported (no CUDA driver
    /// or device, NVDEC cannot decode the streams, a build without CUDA, a
    /// stream that is not 10-bit 4:2:0), Io / NotFound / Malformed /
    /// Decoder (from the container parser and libavcodec), Gpu (a driver
    /// call failed).
    [[nodiscard]] static Result<std::unique_ptr<GpuClipDecoder>> open(const std::filesystem::path& path,
                                                                      const meta::FormatInfo& format,
                                                                      const GpuDecoderOptions& options = {});

    /// Cheap check whether the CUDA driver loads and reports at least one
    /// device (it does not probe NVDEC).  `reason` receives a human readable
    /// explanation when the answer is no.
    [[nodiscard]] static bool available(std::string* reason = nullptr) noexcept;

    /// Stops the decode-ahead worker (it finishes at most the one frame pair
    /// it is decoding), waits for this decoder's copies to complete and
    /// closes the NVDEC decoders.  Outstanding leases stay valid.
    ~GpuClipDecoder();

    GpuClipDecoder(const GpuClipDecoder&) = delete;
    GpuClipDecoder& operator=(const GpuClipDecoder&) = delete;
    GpuClipDecoder(GpuClipDecoder&&) = delete;
    GpuClipDecoder& operator=(GpuClipDecoder&&) = delete;

    /// Pin frame pair `frameIndex` (thread-safe).
    ///
    /// A cached frame is pinned and returned at once.  Otherwise the call
    /// waits while another thread's decode is running (and returns as soon as
    /// that decode produced the frame), or runs the decoder itself: from the
    /// decoder's current position when that is inside the frame's GOP and not
    /// past it, else from the previous sync sample.  EVERY frame decoded on
    /// the way is kept in the cache, so stepping around inside that GOP
    /// afterwards is a cache hit.  A foreground request preempts the
    /// decode-ahead worker within one frame pair.
    ///
    /// Errors: InvalidArgument (index out of range), Decoder (libavcodec
    /// failure, the two lenses disagree on the presentation time), Gpu (a
    /// driver call failed), Unsupported (every cache slot is pinned by a
    /// live lease so the frame has nowhere to go - release leases and
    /// retry), Internal (the decoder is shutting down).
    [[nodiscard]] Result<GpuFrameLease> acquire(std::uint32_t frameIndex);

    /// True when `frameIndex` is in the cache right now.  Pure query: it does
    /// not count as an access (no LRU touch, no sequential detection).
    [[nodiscard]] bool isCached(std::uint32_t frameIndex) const noexcept;

    /// Drop every cached frame that is not pinned (the VRAM stays allocated
    /// for reuse).  For memory-pressure handling and cold-cache measurements.
    /// Returns the number of frames dropped.
    std::uint32_t dropCachedFrames() noexcept;

    /// A consistent snapshot of the counters and sizes.
    [[nodiscard]] GpuDecoderStats stats() const noexcept;

    /// Number of frame pairs (the smaller track's count, which on
    /// camera-written files is both).
    [[nodiscard]] std::uint32_t frameCount() const noexcept;

    /// Frames per second of the lens streams (59.94 for the sample clip).
    [[nodiscard]] double fps() const noexcept;

    /// Per-lens luma width / height in pixels (3000 x 3000 in 6K mode).
    [[nodiscard]] std::uint32_t lensWidth() const noexcept;
    [[nodiscard]] std::uint32_t lensHeight() const noexcept;

    /// The CUcontext everything lives in (the caller's, or the retained
    /// primary context), as void*.
    [[nodiscard]] void* cuContext() const noexcept;

    /// CUDA device ordinal of that context.
    [[nodiscard]] int deviceIndex() const noexcept;

private:
    GpuClipDecoder();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::video
