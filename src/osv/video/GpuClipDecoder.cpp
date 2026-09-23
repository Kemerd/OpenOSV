// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GpuClipDecoder implementation (see GpuClipDecoder.h for the contract).
//
// Moving parts
//   * GpuFrameStore  - the VRAM side: the CUDA context (retained when it is
//                      the primary one), the pooled frame-pair slots, the
//                      frame -> slot map, the LRU clock and the event pool.
//                      Shared ownership: the decoder holds it, and so does
//                      every pin, so a lease released after the decoder was
//                      destroyed still finds valid memory to release.
//   * GpuSlotPin     - one pinned slot.  The lease and every copy of its
//                      FramePair (through the owner fields) share one pin.
//   * The engine     - the two lens HevcStreamDecoders (NVDEC, container
//                      sample feed, decoding into the store's context) plus
//                      the copy into a slot.  Exactly one thread owns the
//                      engine at a time (`engineBusy` under the store mutex):
//                      a foreground acquire() or the decode-ahead worker.
//                      Lens 0 always decodes on the persistent courier thread
//                      while the owner decodes lens 1, so the two NVDEC
//                      streams run in parallel.
//   * The worker     - decodes ahead after sequential access is seen, ONE
//                      frame pair per engine ownership, and steps aside
//                      whenever a foreground acquire is waiting; that is what
//                      bounds a foreground preemption to one frame pair.
//
// Ordering on the GPU
//   FFmpeg's NVDEC post-processing, its copy into a pool surface and our copy
//   into the slot are all enqueued on the lens stream, so they are ordered
//   without any host wait.  A ready event is recorded after our copy; the
//   first acquire() of a slot waits for it on the host once, which is what
//   lets the lease promise complete data on any stream of the context.
//   Consumers that release on a stream leave an event in the slot; before the
//   slot is overwritten the lens streams wait for those events on the GPU.

#include "osv/video/GpuClipDecoder.h"

#include "osv/core/Log.h"
#include "osv/video/Decoder.h"
#include "osv/video/HwAccel.h"

#if defined(OSV_VIDEO_HAVE_CUDA)
#include <cuda.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace osv::video {

// =============================================================================
//  LeaseSource names (independent of the CUDA build)
// =============================================================================
const char* leaseSourceName(LeaseSource source) noexcept {
    switch (source) {
    case LeaseSource::CacheHit: return "hit";
    case LeaseSource::WaitedForDecode: return "waited";
    case LeaseSource::Decoded: return "decoded";
    }
    return "unknown";
}

#if defined(OSV_VIDEO_HAVE_CUDA)

// =============================================================================
//  CUDA driver helpers
// =============================================================================
namespace {

/// "CUDA_ERROR_OUT_OF_MEMORY (out of memory)" for a driver result code.
std::string cuText(CUresult result) {
    const char* name = nullptr;
    const char* text = nullptr;
    // Both lookups fail for codes the driver does not know; keep going.
    cuGetErrorName(result, &name);
    cuGetErrorString(result, &text);
    std::string out = name ? name : ("CUresult " + std::to_string(static_cast<int>(result)));
    if (text) {
        out += " (";
        out += text;
        out += ")";
    }
    return out;
}

/// A Gpu error naming the driver call that failed.
Error cuError(const char* call, CUresult result) {
    return Error{ErrorCode::Gpu, std::string(call) + " failed: " + cuText(result)};
}

/// cuInit(0) exactly once per process; every later call sees the same result.
CUresult ensureDriver() noexcept {
    static const CUresult result = cuInit(0);
    return result;
}

/// Pushes a context for the lifetime of the object and pops it again.  Every
/// driver call in this file runs inside one of these, on whatever thread it
/// happens to run, so the host's own current context is never disturbed.
class ScopedContext {
public:
    explicit ScopedContext(CUcontext context) noexcept
        : m_result(context ? cuCtxPushCurrent(context) : CUDA_ERROR_INVALID_CONTEXT) {}

    ~ScopedContext() {
        if (m_result == CUDA_SUCCESS) {
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
        }
    }

    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;

    /// True when the push succeeded (and the destructor will pop).
    [[nodiscard]] bool ok() const noexcept { return m_result == CUDA_SUCCESS; }

    /// The push's result, for error messages.
    [[nodiscard]] CUresult result() const noexcept { return m_result; }

private:
    CUresult m_result;
};

/// CUdeviceptr <-> void* without tripping over the integer/pointer casts.
void* toPointer(CUdeviceptr ptr) noexcept { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr)); }
CUdeviceptr toDevicePtr(const void* ptr) noexcept {
    return static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(ptr));
}

// =============================================================================
//  Courier: one persistent helper thread that runs one job at a time
// =============================================================================
/// Lens 0 is decoded here while the engine owner decodes lens 1.  A
/// persistent thread instead of a std::thread per frame avoids paying thread
/// creation (~50-100 us on Windows) hundreds of times per second.
class Courier {
public:
    Courier() = default;
    ~Courier() { stop(); }

    Courier(const Courier&) = delete;
    Courier& operator=(const Courier&) = delete;

    /// Launch the thread.  Internal error when the OS refuses.
    Status start() noexcept {
        try {
            m_thread = std::thread([this] { loop(); });
            return okStatus();
        } catch (const std::exception& e) {
            return failStatus(ErrorCode::Internal, std::string("cannot start the lens thread: ") + e.what());
        } catch (...) {
            return failStatus(ErrorCode::Internal, "cannot start the lens thread");
        }
    }

    /// Hand `job` to the thread.  Precondition: no job is pending (the engine
    /// is exclusive, so there is never more than one).  When the thread is not
    /// running the job runs inline, so wait() can never block forever.
    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_thread.joinable() && !m_stop) {
                m_job = std::move(job);
                m_pending = true;
                job = nullptr;
            }
        }
        if (job) {
            job();  // inline fallback; the jobs catch their own exceptions
            return;
        }
        m_cv.notify_all();
    }

    /// Block until the posted job has finished (returns at once when idle).
    void wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_pending; });
    }

    /// Finish a pending job, then end the thread.  Idempotent.
    void stop() noexcept {
        try {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_cv.notify_all();
            if (m_thread.joinable()) {
                m_thread.join();
            }
        } catch (...) {
            // join() only throws for a thread that cannot be joined; there is
            // nothing left to clean up in that case.
        }
    }

private:
    void loop() noexcept {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || m_pending; });
                // A pending job is always run, even when stopping, so a
                // waiter can never be left hanging.
                if (!m_pending) {
                    return;
                }
                job = std::move(m_job);
                m_job = nullptr;
            }
            try {
                if (job) {
                    job();
                }
            } catch (...) {
                // The jobs convert their own failures into Results; this is
                // only the last line of defence for the thread itself.
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pending = false;
            }
            m_cv.notify_all();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::function<void()> m_job;
    bool m_pending = false;
    bool m_stop = false;
    std::thread m_thread;
};

/// What one lens decode reports besides the pixels.
struct LensOutcome {
    std::int64_t ptsUs = 0;   ///< Presentation time in microseconds.
    bool narrowRange = true;  ///< Limited-range coding.
};

}  // namespace

// =============================================================================
//  The VRAM store and the pin
// =============================================================================
namespace detail {

/// One cache slot: VRAM for one frame pair plus its bookkeeping.  Every field
/// is guarded by GpuFrameStore::mutex, except that a slot's memory, events
/// and frame metadata are immutable while it is pinned or published and may
/// then be read without the lock by whoever holds a pin.
struct GpuSlot {
    CUdeviceptr base = 0;                    ///< Pitched allocation for both lenses (0 = not allocated).
    std::int64_t frame = -1;                 ///< Cached frame index (-1 = empty).
    std::uint32_t pins = 0;                  ///< Live GpuSlotPin objects.
    bool writing = false;                    ///< Reserved by the engine and being filled.
    bool readyConfirmed = false;             ///< The host has seen both ready events complete.
    std::uint64_t lastUse = 0;               ///< LRU clock value of the last access.
    std::array<CUevent, 2> ready{};          ///< Recorded on each lens stream after its copy.
    std::vector<CUevent> releases;           ///< Consumer events the next overwrite must wait for.
    std::array<std::int64_t, 2> ptsUs{};     ///< Per-lens presentation time of the cached frame.
    std::array<bool, 2> narrowRange{{true, true}};
};

/// The shared VRAM side of a decoder (see the file header).
struct GpuFrameStore {
    // ---- context ------------------------------------------------------------
    CUcontext context = nullptr;   ///< Caller's context or the retained primary one.
    CUdevice device = 0;           ///< Device of `context`.
    int deviceOrdinal = 0;         ///< Ordinal reported in DeviceFrameRef::deviceIndex.
    bool retainedPrimary = false;  ///< Release the primary context on destruction.

    // ---- geometry (immutable after open) ----------------------------------
    std::uint32_t width = 0;             ///< Luma width of one lens.
    std::uint32_t height = 0;            ///< Luma height of one lens.
    std::uint32_t chromaW = 0;           ///< Chroma width ((width + 1) / 2).
    std::uint32_t chromaH = 0;           ///< Chroma height ((height + 1) / 2).
    std::size_t rowBytes = 0;            ///< Bytes per row requested from cuMemAllocPitch.
    std::size_t rows = 0;                ///< Rows per slot: 2 lenses x (luma + chroma rows).
    std::size_t pitch = 0;               ///< Row pitch in bytes (identical for every slot).
    std::size_t chromaOffsetBytes = 0;   ///< Start of the CbCr plane within a lens.
    std::size_t lensOffsetBytes = 0;     ///< Start of lens 1 within a slot.
    std::size_t slotBytes = 0;           ///< pitch * rows.
    std::size_t budgetBytes = 0;         ///< The budget the capacity came from.

    // ---- mutable state (under `mutex`) ------------------------------------
    mutable std::mutex mutex;
    std::condition_variable cv;              ///< Frame published, engine released, pin dropped, request seen.
    std::vector<GpuSlot> slots;              ///< size() == capacity.
    std::uint32_t usableSlots = 0;           ///< Slots that may still be allocated (shrinks on OOM).
    std::uint32_t allocatedSlots = 0;        ///< Slots [0, allocatedSlots) have VRAM.
    std::vector<std::int32_t> frameToSlot;   ///< One entry per frame (-1 = not cached).
    std::vector<CUevent> eventPool;          ///< Recycled release events.
    std::uint64_t clock = 0;                 ///< LRU clock.
    GpuDecoderStats counters;                ///< The counter fields of stats().

    GpuFrameStore() = default;
    GpuFrameStore(const GpuFrameStore&) = delete;
    GpuFrameStore& operator=(const GpuFrameStore&) = delete;

    /// Frees every slot and event (after waiting for any consumer that
    /// released on a stream) and releases the primary context if retained.
    ~GpuFrameStore() {
        if (context) {
            ScopedContext ctx(context);
            if (ctx.ok()) {
                for (GpuSlot& slot : slots) {
                    // A consumer's kernel may still be reading: wait for the
                    // events it left before the memory goes back.
                    for (CUevent ev : slot.releases) {
                        if (ev) {
                            cuEventSynchronize(ev);
                            cuEventDestroy(ev);
                        }
                    }
                    slot.releases.clear();
                    for (CUevent& ev : slot.ready) {
                        if (ev) {
                            cuEventSynchronize(ev);
                            cuEventDestroy(ev);
                            ev = nullptr;
                        }
                    }
                    if (slot.base) {
                        cuMemFree(slot.base);
                        slot.base = 0;
                    }
                }
                for (CUevent ev : eventPool) {
                    if (ev) {
                        cuEventDestroy(ev);
                    }
                }
                eventPool.clear();
            } else {
                // The caller destroyed its context before the last lease:
                // the memory went with it and there is nothing left to free.
                // (This destructor can run inside a lease's; formatting
                // must not be allowed to throw out of it.)
                try {
                    log::warn("video/gpu: context gone before the frame store ({}); VRAM was released with it",
                              cuText(ctx.result()));
                } catch (...) {
                }
            }
        }
        if (retainedPrimary) {
            cuDevicePrimaryCtxRelease(device);
        }
    }

    /// Drop one pin (called by ~GpuSlotPin on any thread).
    void unpin(std::uint32_t slotIndex) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex);
            if (slotIndex < slots.size() && slots[slotIndex].pins > 0) {
                --slots[slotIndex].pins;
                // A frame the host just finished with is a good candidate
                // to keep: refresh its LRU position.
                slots[slotIndex].lastUse = ++clock;
            }
        } catch (...) {
            // std::mutex::lock only throws on a broken mutex; nothing to do.
        }
        // The worker may have been idle because every slot was pinned.
        cv.notify_all();
    }

    /// An event from the pool, or a new one (context must be current).
    CUevent takeEvent() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!eventPool.empty()) {
                CUevent ev = eventPool.back();
                eventPool.pop_back();
                return ev;
            }
        }
        CUevent ev = nullptr;
        if (cuEventCreate(&ev, CU_EVENT_DISABLE_TIMING) != CUDA_SUCCESS) {
            return nullptr;
        }
        return ev;
    }

    /// Record a release event for `slotIndex` on `stream` (see
    /// GpuFrameLease::releaseAfter).  On any failure the whole context is
    /// synchronised instead, which is slower but just as safe.
    Status recordRelease(std::uint32_t slotIndex, CUstream stream) noexcept {
        try {
            ScopedContext ctx(context);
            if (!ctx.ok()) {
                return Status(cuError("cuCtxPushCurrent", ctx.result()));
            }
            CUevent ev = takeEvent();
            if (!ev) {
                cuCtxSynchronize();
                return failStatus(ErrorCode::Gpu, "cannot create a release event; synchronised the context instead");
            }
            const CUresult recorded = cuEventRecord(ev, stream);
            if (recorded != CUDA_SUCCESS) {
                // Probably a stream of another context.  Waiting for the whole
                // context keeps the slot safe to overwrite.
                cuCtxSynchronize();
                std::lock_guard<std::mutex> lock(mutex);
                eventPool.push_back(ev);
                return Status(cuError("cuEventRecord (release stream)", recorded));
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (slotIndex >= slots.size()) {
                eventPool.push_back(ev);
                return failStatus(ErrorCode::Internal, "release of an unknown slot");
            }
            // Recycle the events of earlier releases that have already
            // passed, so a frame that is leased thousands of times without
            // being evicted does not grow an unbounded list.
            std::vector<CUevent>& list = slots[slotIndex].releases;
            for (std::size_t i = 0; i < list.size();) {
                if (cuEventQuery(list[i]) == CUDA_SUCCESS) {
                    eventPool.push_back(list[i]);
                    list[i] = list.back();
                    list.pop_back();
                } else {
                    ++i;
                }
            }
            list.push_back(ev);
            return okStatus();
        } catch (const std::exception& e) {
            return failStatus(ErrorCode::Internal, std::string("releaseAfter: ") + e.what());
        } catch (...) {
            return failStatus(ErrorCode::Internal, "releaseAfter: unknown failure");
        }
    }
};

/// One pinned slot.  Constructed unarmed (store == nullptr) so that the
/// allocation can happen before the pin count is taken; armed under the
/// store mutex together with the pin increment, which cannot throw.
struct GpuSlotPin {
    std::shared_ptr<GpuFrameStore> store;  ///< Keeps the VRAM alive; nullptr = unarmed.
    std::uint32_t slot = 0;                ///< Pinned slot index.

    GpuSlotPin() = default;
    GpuSlotPin(const GpuSlotPin&) = delete;
    GpuSlotPin& operator=(const GpuSlotPin&) = delete;

    ~GpuSlotPin() {
        if (store) {
            store->unpin(slot);
        }
    }
};

}  // namespace detail

// =============================================================================
//  GpuFrameLease
// =============================================================================
GpuFrameLease::GpuFrameLease() noexcept = default;

GpuFrameLease::~GpuFrameLease() { release(); }

GpuFrameLease::GpuFrameLease(GpuFrameLease&& other) noexcept
    : m_pin(std::move(other.m_pin)), m_pair(std::move(other.m_pair)), m_source(other.m_source) {
    // A moved-from FramePair still holds its shared_ptr owners in some
    // implementations; clear it so the source cannot keep the slot pinned.
    other.m_pair = FramePair{};
    other.m_pin.reset();
}

GpuFrameLease& GpuFrameLease::operator=(GpuFrameLease&& other) noexcept {
    if (this != &other) {
        release();
        m_pin = std::move(other.m_pin);
        m_pair = std::move(other.m_pair);
        m_source = other.m_source;
        other.m_pair = FramePair{};
        other.m_pin.reset();
    }
    return *this;
}

bool GpuFrameLease::valid() const noexcept { return m_pin != nullptr; }

std::uint32_t GpuFrameLease::frameIndex() const noexcept { return m_pin ? m_pair.index : 0; }

const FramePair& GpuFrameLease::pair() const noexcept {
    static const FramePair kEmpty{};
    return m_pin ? m_pair : kEmpty;
}

LeaseSource GpuFrameLease::source() const noexcept { return m_source; }

Status GpuFrameLease::releaseAfter(void* cuStream) noexcept {
    if (!m_pin) {
        return failStatus(ErrorCode::InvalidArgument, "releaseAfter on an empty lease");
    }
    // Record first, then drop the pin: the slot must never become reusable
    // before its release event is in the list.
    Status recorded = okStatus();
    if (m_pin->store) {
        recorded = m_pin->store->recordRelease(m_pin->slot, static_cast<CUstream>(cuStream));
    }
    release();
    return recorded;
}

void GpuFrameLease::release() noexcept {
    // The pair's owner fields share the pin: clear them first so that the
    // pin really goes when the lease lets go (unless the caller kept a copy
    // of the pair, which then keeps the slot pinned on purpose).
    m_pair = FramePair{};
    m_pin.reset();
}

// =============================================================================
//  GpuClipDecoder::Impl
// =============================================================================
struct GpuClipDecoder::Impl {
    // ---- shared VRAM side -----------------------------------------------------
    std::shared_ptr<detail::GpuFrameStore> store;

    // ---- the engine ------------------------------------------------------------
    std::array<HevcStreamDecoder, 2> lens;   ///< [0] slave, [1] master (NVDEC into store->context).
    std::array<CUstream, 2> streams{};       ///< Per-lens copy streams (both = caller's when supplied).
    bool ownsStreams = false;                ///< True when we created `streams`.
    Courier courier;                         ///< Runs lens 0 while the owner runs lens 1.
    std::uint32_t engineNext = 0;            ///< Frame the lens decoders produce next without a seek.
    bool engineValid = false;                ///< False until a decode succeeded / after any failure.

    // ---- clip properties -------------------------------------------------------
    std::uint32_t frameCount = 0;
    double fps = 0.0;
    std::int64_t ptsToleranceUs = 1;         ///< One tick of the coarser time base, in microseconds.
    std::uint32_t aheadWindow = 0;           ///< Effective decode-ahead window.

    // ---- scheduling (under store->mutex) ------------------------------------
    bool engineBusy = false;                 ///< Someone owns the engine.
    std::uint32_t foregroundWaiters = 0;     ///< acquire() calls waiting for the engine.
    std::atomic<bool> stopping{false};       ///< Destruction in progress.
    std::int64_t lastRequest = -1;           ///< Most recent acquire() index.
    bool aheadActive = false;                ///< Sequential access detected.
    std::uint32_t aheadBase = 0;             ///< Newest frame the host asked for in the run.
    std::uint32_t aheadEnd = 0;              ///< Last frame of the decode-ahead window.
    std::thread worker;                      ///< Decode-ahead thread (absent when the window is 0).

    ~Impl() { shutdown(); }

    // -------------------------------------------------------------------------
    //  Engine ownership
    // -------------------------------------------------------------------------

    /// Releases the engine (and wakes everyone) on every exit path.
    class EngineGuard {
    public:
        explicit EngineGuard(Impl& impl) noexcept : m_impl(&impl) {}
        ~EngineGuard() { release(); }
        EngineGuard(const EngineGuard&) = delete;
        EngineGuard& operator=(const EngineGuard&) = delete;

        void release() noexcept {
            if (!m_impl) {
                return;
            }
            detail::GpuFrameStore& s = *m_impl->store;
            try {
                std::lock_guard<std::mutex> lock(s.mutex);
                m_impl->engineBusy = false;
            } catch (...) {
                m_impl->engineBusy = false;
            }
            s.cv.notify_all();
            m_impl = nullptr;
        }

    private:
        Impl* m_impl;
    };

    // -------------------------------------------------------------------------
    //  Lifetime
    // -------------------------------------------------------------------------

    /// Stop the worker, drain the copies, close the decoders and streams.
    /// Safe on a partially opened object; idempotent.
    void shutdown() noexcept {
        try {
            if (store) {
                std::lock_guard<std::mutex> lock(store->mutex);
                stopping = true;
                aheadActive = false;
            }
            stopping = true;
            if (store) {
                store->cv.notify_all();
            }
            // The worker finishes at most the frame pair it is decoding.
            if (worker.joinable()) {
                worker.join();
            }
        } catch (...) {
            // join() on a thread that refuses: nothing sensible left to do.
        }
        courier.stop();
        // Every copy we enqueued must land before the source surfaces (owned
        // by FFmpeg) and the streams disappear.
        if (store && store->context) {
            ScopedContext ctx(store->context);
            if (ctx.ok()) {
                for (std::size_t l = 0; l < streams.size(); ++l) {
                    if (streams[l] && (l == 0 || streams[l] != streams[0])) {
                        cuStreamSynchronize(streams[l]);
                    }
                }
            }
        }
        // FFmpeg tears the NVDEC decoders down with the context pushed itself.
        lens[0] = HevcStreamDecoder();
        lens[1] = HevcStreamDecoder();
        if (ownsStreams && store && store->context) {
            ScopedContext ctx(store->context);
            if (ctx.ok()) {
                for (CUstream& s : streams) {
                    if (s) {
                        cuStreamDestroy(s);
                    }
                    s = nullptr;
                }
            }
        }
        streams = {};
        ownsStreams = false;
        // Outstanding pins keep the store (and its VRAM) alive on their own.
        store.reset();
    }

    // -------------------------------------------------------------------------
    //  GOP helpers
    // -------------------------------------------------------------------------

    /// Sync sample the decode of `index` has to start from.  Both tracks are
    /// written with the same GOP layout by the camera; taking the earlier of
    /// the two keeps us correct even if they ever disagree.
    [[nodiscard]] std::uint32_t syncFor(std::uint32_t index) const noexcept {
        const std::uint32_t a = lens[0].previousSyncIndex(index).value_or(0);
        const std::uint32_t b = lens[1].previousSyncIndex(index).value_or(0);
        return std::min({a, b, index});
    }

    /// First frame a decode towards `target` produces: the engine's current
    /// position when it lies inside target's GOP and not past target
    /// (continuing is never more work than restarting), else the sync sample.
    /// Engine owner only (or under the store mutex while the engine is idle).
    [[nodiscard]] std::uint32_t planStart(std::uint32_t target) const noexcept {
        const std::uint32_t sync = syncFor(target);
        if (engineValid && engineNext <= target && sync <= engineNext) {
            return engineNext;
        }
        return sync;
    }

    // -------------------------------------------------------------------------
    //  Slots (store mutex held)
    // -------------------------------------------------------------------------

    /// True when `frame` lies in the protected decode-ahead window.
    [[nodiscard]] bool inAheadWindowLocked(std::int64_t frame) const noexcept {
        return aheadActive && frame >= static_cast<std::int64_t>(aheadBase) &&
               frame <= static_cast<std::int64_t>(aheadEnd);
    }

    /// Whether a slot is a legal home for a new frame right now.
    [[nodiscard]] bool evictableLocked(const detail::GpuSlot& slot, bool speculative) const noexcept {
        if (slot.base == 0 || slot.pins > 0 || slot.writing) {
            return false;
        }
        // The worker must never evict the frames it is trying to get ahead
        // with (it would chase its own tail); the foreground may.
        return !(speculative && slot.frame >= 0 && inAheadWindowLocked(slot.frame));
    }

    /// Reserve a slot for a new frame: an allocated slot that holds nothing,
    /// else a never-used slot while the capacity allows, else the least
    /// recently used legal one.  Marks it `writing`, unmaps whatever it held
    /// and hands back the consumer events the overwrite must wait for.  -1
    /// when nothing is available.
    std::int32_t reserveSlotLocked(bool speculative, bool& needsAllocation, std::vector<CUevent>& waits) {
        detail::GpuFrameStore& s = *store;
        needsAllocation = false;
        // 1. Reuse an empty slot, or remember the least recently used one.
        std::int32_t best = -1;
        std::uint64_t bestUse = std::numeric_limits<std::uint64_t>::max();
        bool bestIsEmpty = false;
        for (std::size_t i = 0; i < s.slots.size(); ++i) {
            const detail::GpuSlot& slot = s.slots[i];
            if (!evictableLocked(slot, speculative)) {
                continue;
            }
            if (slot.frame < 0) {
                best = static_cast<std::int32_t>(i);
                bestIsEmpty = true;
                break;
            }
            if (slot.lastUse < bestUse) {
                bestUse = slot.lastUse;
                best = static_cast<std::int32_t>(i);
            }
        }
        // 2. Grow while the budget allows (slots are allocated in order)
        //    rather than evict a frame somebody may still want.
        if (!bestIsEmpty && s.allocatedSlots < s.usableSlots && s.allocatedSlots < s.slots.size()) {
            detail::GpuSlot& fresh = s.slots[s.allocatedSlots];
            if (fresh.base == 0 && !fresh.writing) {
                fresh.writing = true;
                needsAllocation = true;
                return static_cast<std::int32_t>(s.allocatedSlots);
            }
        }
        if (best < 0) {
            return -1;
        }
        // 3. Evict and claim.
        detail::GpuSlot& victim = s.slots[static_cast<std::size_t>(best)];
        if (victim.frame >= 0) {
            if (static_cast<std::size_t>(victim.frame) < s.frameToSlot.size()) {
                s.frameToSlot[static_cast<std::size_t>(victim.frame)] = -1;
            }
            ++s.counters.evictions;
        }
        victim.frame = -1;
        victim.writing = true;
        victim.readyConfirmed = false;
        waits = std::move(victim.releases);
        victim.releases.clear();
        return best;
    }

    /// True when the worker could store a speculative frame somewhere.
    [[nodiscard]] bool speculativeSlotAvailableLocked() const noexcept {
        const detail::GpuFrameStore& s = *store;
        if (s.allocatedSlots < s.usableSlots && s.allocatedSlots < s.slots.size()) {
            return true;
        }
        return std::any_of(s.slots.begin(), s.slots.end(),
                           [this](const detail::GpuSlot& slot) { return evictableLocked(slot, true); });
    }

    /// Allocate the VRAM and ready events of one slot (engine owner, no lock).
    Status allocateSlotMemory(CUdeviceptr& base, std::array<CUevent, 2>& ready) const {
        detail::GpuFrameStore& s = *store;
        ScopedContext ctx(s.context);
        if (!ctx.ok()) {
            return Status(cuError("cuCtxPushCurrent", ctx.result()));
        }
        std::size_t pitch = 0;
        CUresult r = cuMemAllocPitch(&base, &pitch, s.rowBytes, s.rows, 16);
        if (r != CUDA_SUCCESS) {
            base = 0;
            return Status(cuError("cuMemAllocPitch (frame cache slot)", r));
        }
        // Every slot must share one pitch: the DeviceFrameRefs and the copy
        // descriptors are built from the store-wide value.
        if (pitch != s.pitch) {
            cuMemFree(base);
            base = 0;
            return failStatus(ErrorCode::Internal, "cuMemAllocPitch returned pitch " + std::to_string(pitch) +
                                                       " instead of " + std::to_string(s.pitch));
        }
        for (CUevent& ev : ready) {
            r = cuEventCreate(&ev, CU_EVENT_DISABLE_TIMING);
            if (r != CUDA_SUCCESS) {
                for (CUevent& made : ready) {
                    if (made) {
                        cuEventDestroy(made);
                        made = nullptr;
                    }
                }
                cuMemFree(base);
                base = 0;
                return Status(cuError("cuEventCreate (slot ready)", r));
            }
        }
        return okStatus();
    }

    // -------------------------------------------------------------------------
    //  One lens, one frame
    // -------------------------------------------------------------------------

    /// Decode frame `index` of lens `l` and, when `dst` is non-zero, copy the
    /// NVDEC surface into it (luma at dst, CbCr at dst + chromaOffsetBytes)
    /// and record `ready` after the copy.  Runs on the courier (lens 0) or the
    /// engine owner (lens 1).
    Result<LensOutcome> decodeLens(std::size_t l, std::uint32_t index, CUdeviceptr dst, CUevent ready) {
        const detail::GpuFrameStore& s = *store;
        const std::string who = "lens " + std::to_string(l) + " frame " + std::to_string(index);
        auto frame = lens[l].decodeFrame(index);
        if (!frame.ok()) {
            return Error{frame.error().code, who + ": " + frame.error().message};
        }
        // With keepOnDevice + CUDA the pixels are only reachable through the
        // decoder's last device frame; its absence means a software fallback.
        const std::optional<DeviceFrameRef> dev = lens[l].lastDeviceFrame();
        if (!dev || !dev->valid()) {
            return Error{ErrorCode::Decoder, who + ": NVDEC delivered no device surface"};
        }
        if (dev->width != s.width || dev->height != s.height) {
            return Error{ErrorCode::Decoder, who + ": surface is " + std::to_string(dev->width) + "x" +
                                                 std::to_string(dev->height) + ", stream was opened as " +
                                                 std::to_string(s.width) + "x" + std::to_string(s.height)};
        }
        if (dev->bitDepth != 10 || dev->bitShift != 6) {
            return Error{ErrorCode::Unsupported, who + ": surface is not P010 (bit depth " +
                                                     std::to_string(dev->bitDepth) + ")"};
        }
        const std::size_t lumaBytes = static_cast<std::size_t>(s.width) * 2u;
        const std::size_t chromaBytes = static_cast<std::size_t>(s.chromaW) * 4u;  // Cb + Cr, 2 bytes each
        if (dev->pitchBytes < std::max(lumaBytes, chromaBytes)) {
            return Error{ErrorCode::Decoder, who + ": surface pitch " + std::to_string(dev->pitchBytes) +
                                                 " is narrower than a row"};
        }
        LensOutcome out;
        out.ptsUs = frame.value().ptsUs;
        out.narrowRange = frame.value().narrowRange;
        if (dst == 0) {
            return out;  // decode only (frame already cached or nowhere to put it)
        }
        ScopedContext ctx(s.context);
        if (!ctx.ok()) {
            return cuError("cuCtxPushCurrent", ctx.result());
        }
        // Luma plane, then the interleaved CbCr plane, both on the lens
        // stream right behind FFmpeg's own copy into the source surface.
        CUDA_MEMCPY2D luma{};
        luma.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        luma.srcDevice = toDevicePtr(dev->yDevice);
        luma.srcPitch = dev->pitchBytes;
        luma.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        luma.dstDevice = dst;
        luma.dstPitch = s.pitch;
        luma.WidthInBytes = lumaBytes;
        luma.Height = s.height;
        CUresult r = cuMemcpy2DAsync(&luma, streams[l]);
        if (r != CUDA_SUCCESS) {
            return cuError("cuMemcpy2DAsync (luma)", r);
        }
        CUDA_MEMCPY2D chroma = luma;
        chroma.srcDevice = toDevicePtr(dev->uvDevice);
        chroma.dstDevice = dst + s.chromaOffsetBytes;
        chroma.WidthInBytes = chromaBytes;
        chroma.Height = s.chromaH;
        r = cuMemcpy2DAsync(&chroma, streams[l]);
        if (r != CUDA_SUCCESS) {
            return cuError("cuMemcpy2DAsync (chroma)", r);
        }
        r = cuEventRecord(ready, streams[l]);
        if (r != CUDA_SUCCESS) {
            return cuError("cuEventRecord (slot ready)", r);
        }
        return out;
    }

    // -------------------------------------------------------------------------
    //  One frame pair (engine owner only)
    // -------------------------------------------------------------------------

    /// Decode pair `index`, storing it in the cache when it is not cached and
    /// a slot is available.  With `pinForCaller` the stored slot comes back
    /// pinned (an Unsupported error when every slot is pinned).  Returns the
    /// slot index, or -1 when the frame was decoded but not stored.
    Result<std::int32_t> decodePair(std::uint32_t index, bool pinForCaller, bool speculative) {
        detail::GpuFrameStore& s = *store;
        if (index >= frameCount) {
            return Error{ErrorCode::InvalidArgument, "frame " + std::to_string(index) + " out of range"};
        }

        // ---- 1. a home for the frame -------------------------------------------
        std::int32_t slot = -1;
        bool needsAllocation = false;
        std::vector<CUevent> waits;
        for (int attempt = 0; attempt < 2; ++attempt) {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                const std::int32_t existing = s.frameToSlot[index];
                if (existing >= 0) {
                    // Only the engine inserts, so this is a frame decoded in
                    // an earlier run: decode it again only to advance the
                    // decoder, and never copy over a slot someone may read.
                    if (pinForCaller) {
                        ++s.slots[static_cast<std::size_t>(existing)].pins;
                        s.slots[static_cast<std::size_t>(existing)].lastUse = ++s.clock;
                        return existing;
                    }
                    slot = -1;
                    break;
                }
                slot = reserveSlotLocked(speculative, needsAllocation, waits);
            }
            if (slot < 0 || !needsAllocation) {
                break;
            }
            // First use of this slot: give it VRAM.
            CUdeviceptr base = 0;
            std::array<CUevent, 2> ready{};
            const Status allocated = allocateSlotMemory(base, ready);
            std::lock_guard<std::mutex> lock(s.mutex);
            detail::GpuSlot& fresh = s.slots[static_cast<std::size_t>(slot)];
            if (allocated.ok()) {
                fresh.base = base;
                fresh.ready = ready;
                ++s.allocatedSlots;
                break;
            }
            // Out of VRAM below the budget: stop growing and reuse instead.
            fresh.writing = false;
            s.usableSlots = s.allocatedSlots;
            log::warn("video/gpu: frame cache capped at {} slots ({})", s.allocatedSlots, allocated.error().message);
            slot = -1;
        }
        if (slot < 0 && pinForCaller) {
            return Error{ErrorCode::Unsupported, "frame " + std::to_string(index) +
                                                     " has nowhere to go: every cache slot is pinned by a lease"};
        }

        // ---- 2. order the overwrite after every consumer of the old frame ----
        if (!waits.empty()) {
            ScopedContext ctx(s.context);
            bool ordered = ctx.ok();
            for (std::size_t l = 0; ordered && l < streams.size(); ++l) {
                if (l > 0 && streams[l] == streams[0]) {
                    break;  // one shared stream: waiting once is enough
                }
                for (CUevent ev : waits) {
                    if (cuStreamWaitEvent(streams[l], ev, 0) != CUDA_SUCCESS) {
                        ordered = false;
                        break;
                    }
                }
            }
            if (!ordered && ctx.ok()) {
                // Could not express the dependency on the GPU: wait on the
                // host instead, which is always correct.
                for (CUevent ev : waits) {
                    cuEventSynchronize(ev);
                }
            }
            // The waits captured the events' state; they can be reused now.
            std::lock_guard<std::mutex> lock(s.mutex);
            s.eventPool.insert(s.eventPool.end(), waits.begin(), waits.end());
            waits.clear();
        }

        // ---- 3. both lenses in parallel -----------------------------------------
        CUdeviceptr base = 0;
        std::array<CUevent, 2> ready{};
        if (slot >= 0) {
            std::lock_guard<std::mutex> lock(s.mutex);
            base = s.slots[static_cast<std::size_t>(slot)].base;
            ready = s.slots[static_cast<std::size_t>(slot)].ready;
        }
        Result<LensOutcome> first = Error{ErrorCode::Internal, "lens 0 was not decoded"};
        courier.post([&]() {
            try {
                first = decodeLens(0, index, base, ready[0]);
            } catch (const std::exception& e) {
                first = Error{ErrorCode::Internal, std::string("lens 0: ") + e.what()};
            } catch (...) {
                first = Error{ErrorCode::Internal, "lens 0: unknown failure"};
            }
        });
        Result<LensOutcome> second = Error{ErrorCode::Internal, "lens 1 was not decoded"};
        try {
            second = decodeLens(1, index, base ? base + s.lensOffsetBytes : 0, ready[1]);
        } catch (const std::exception& e) {
            second = Error{ErrorCode::Internal, std::string("lens 1: ") + e.what()};
        } catch (...) {
            second = Error{ErrorCode::Internal, "lens 1: unknown failure"};
        }
        courier.wait();

        // ---- 4. verdict ----------------------------------------------------------
        Status verdict = okStatus();
        if (!first.ok()) {
            verdict = Status(first.error());
        } else if (!second.ok()) {
            verdict = Status(second.error());
        } else {
            // Both tracks run on one encoder clock; more than a tick apart
            // means the two surfaces are not the same instant.
            const std::int64_t a = first.value().ptsUs;
            const std::int64_t b = second.value().ptsUs;
            if ((a > b ? a - b : b - a) > ptsToleranceUs) {
                verdict = failStatus(ErrorCode::Decoder, "lens presentation times differ at frame " +
                                                             std::to_string(index) + ": " + std::to_string(a) +
                                                             " us vs " + std::to_string(b) + " us");
            }
        }

        // ---- 5. publish or roll back ------------------------------------------
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!verdict.ok()) {
            engineValid = false;
            if (slot >= 0) {
                detail::GpuSlot& failed = s.slots[static_cast<std::size_t>(slot)];
                failed.writing = false;
                failed.frame = -1;
            }
            return Error(verdict.error());
        }
        engineNext = index + 1;
        engineValid = true;
        ++s.counters.framesDecoded;
        if (speculative) {
            ++s.counters.framesDecodedAhead;
        }
        if (slot >= 0) {
            detail::GpuSlot& done = s.slots[static_cast<std::size_t>(slot)];
            done.frame = index;
            done.writing = false;
            done.readyConfirmed = false;
            done.lastUse = ++s.clock;
            done.ptsUs = {first.value().ptsUs, second.value().ptsUs};
            done.narrowRange = {first.value().narrowRange, second.value().narrowRange};
            if (pinForCaller) {
                ++done.pins;
            }
            s.frameToSlot[index] = slot;
        }
        s.cv.notify_all();
        return slot;
    }

    /// Foreground decode up to `target`, keeping every frame on the way.
    /// Returns target's slot, pinned for the caller.  Engine owner only.
    Result<std::uint32_t> decodeTo(std::uint32_t target) {
        const std::uint32_t start = planStart(target);
        if (!engineValid || start != engineNext) {
            std::lock_guard<std::mutex> lock(store->mutex);
            ++store->counters.restarts;
        }
        for (std::uint32_t i = start; i <= target; ++i) {
            if (stopping) {
                return Error{ErrorCode::Internal, "decoder is shutting down"};
            }
            auto decoded = decodePair(i, i == target, false);
            if (!decoded.ok()) {
                // A failed reference frame makes every later frame of the
                // GOP wrong, so the request fails as a whole.
                return Error(decoded.error());
            }
            if (i == target) {
                if (decoded.value() < 0) {
                    return Error{ErrorCode::Internal, "target frame was decoded but not stored"};
                }
                return static_cast<std::uint32_t>(decoded.value());
            }
        }
        return Error{ErrorCode::Internal, "decode plan ended before frame " + std::to_string(target)};
    }

    // -------------------------------------------------------------------------
    //  Sequential detection (store mutex held)
    // -------------------------------------------------------------------------

    /// Update the decode-ahead window for a request of `k`.
    void noteAccessLocked(std::uint32_t k) {
        if (aheadWindow == 0) {
            lastRequest = k;
            return;
        }
        const std::uint32_t last = frameCount > 0 ? frameCount - 1 : 0;
        // Several render threads may ask for k, k+2, k+1, k+3 out of order:
        // anything inside the live window (or just behind it) keeps the run.
        const bool inWindow = aheadActive && static_cast<std::uint64_t>(k) + kMaxSequentialStep >= aheadBase &&
                              k <= aheadEnd;
        const bool stepForward = lastRequest >= 0 && static_cast<std::int64_t>(k) > lastRequest &&
                                 static_cast<std::int64_t>(k) - lastRequest <= kMaxSequentialStep;
        if (inWindow || stepForward) {
            aheadBase = (aheadActive && inWindow) ? std::max(aheadBase, k) : k;
            aheadEnd = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(aheadBase) + aheadWindow, last));
            aheadActive = true;
        } else if (static_cast<std::int64_t>(k) != lastRequest) {
            // A jump: whatever the worker was preparing is no longer wanted.
            aheadActive = false;
        }
        lastRequest = k;
        store->cv.notify_all();
    }

    /// First frame of the window that is not cached, or -1.
    [[nodiscard]] std::int64_t firstMissingAheadLocked() const noexcept {
        if (!aheadActive) {
            return -1;
        }
        for (std::uint32_t i = aheadBase + 1; i <= aheadEnd && i < frameCount; ++i) {
            if (store->frameToSlot[i] < 0) {
                return i;
            }
        }
        return -1;
    }

    /// The worker's wake-up predicate.
    [[nodiscard]] bool workerHasWorkLocked() const noexcept {
        if (!aheadActive || engineBusy || foregroundWaiters > 0 || stopping) {
            return false;
        }
        const std::int64_t target = firstMissingAheadLocked();
        if (target < 0) {
            return false;
        }
        // The frame the engine would decode next either is cached already
        // (decode only, no slot needed) or needs a slot the worker may use.
        const std::uint32_t next = planStart(static_cast<std::uint32_t>(target));
        if (next < frameCount && store->frameToSlot[next] >= 0) {
            return true;
        }
        return speculativeSlotAvailableLocked();
    }

    // -------------------------------------------------------------------------
    //  The decode-ahead worker
    // -------------------------------------------------------------------------
    void workerLoop() noexcept {
        detail::GpuFrameStore& s = *store;
        while (!stopping) {
            try {
                std::unique_lock<std::mutex> lock(s.mutex);
                s.cv.wait(lock, [this] { return stopping || workerHasWorkLocked(); });
                if (stopping) {
                    return;
                }
                const std::uint32_t target = static_cast<std::uint32_t>(firstMissingAheadLocked());
                engineBusy = true;
                lock.unlock();
                EngineGuard guard(*this);
                // One frame pair per ownership: the step towards `target`.
                const std::uint32_t next = planStart(target);
                if (!engineValid || next != engineNext) {
                    std::lock_guard<std::mutex> countLock(s.mutex);
                    ++s.counters.restarts;
                }
                auto decoded = decodePair(next, false, true);
                if (!decoded.ok()) {
                    log::debug("video/gpu: decode-ahead stopped at frame {}: {}", next, decoded.error().message);
                    std::lock_guard<std::mutex> stopLock(s.mutex);
                    aheadActive = false;
                }
            } catch (const std::exception& e) {
                log::error("video/gpu: decode-ahead worker: {}", e.what());
                try {
                    std::lock_guard<std::mutex> stopLock(s.mutex);
                    aheadActive = false;
                } catch (...) {
                }
            } catch (...) {
                log::error("video/gpu: decode-ahead worker: unknown failure");
            }
        }
    }

    // -------------------------------------------------------------------------
    //  Lease construction (slot already pinned, `pin` armed)
    // -------------------------------------------------------------------------
    Result<GpuFrameLease> makeLease(std::shared_ptr<detail::GpuSlotPin> pin, std::uint32_t frameIndex,
                                    LeaseSource source) {
        detail::GpuFrameStore& s = *store;
        const std::uint32_t slotIndex = pin->slot;
        // The pinned slot cannot be refilled, so its memory, events and
        // metadata are stable from here on.
        bool confirmed = false;
        CUdeviceptr base = 0;
        std::array<CUevent, 2> ready{};
        std::array<std::int64_t, 2> ptsUs{};
        std::array<bool, 2> narrow{{true, true}};
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            const detail::GpuSlot& slot = s.slots[slotIndex];
            confirmed = slot.readyConfirmed;
            base = slot.base;
            ready = slot.ready;
            ptsUs = slot.ptsUs;
            narrow = slot.narrowRange;
        }
        if (base == 0) {
            return Error{ErrorCode::Internal, "pinned slot has no memory"};
        }
        // Complete data on return: wait for the copies once per fill.
        if (!confirmed) {
            ScopedContext ctx(s.context);
            if (!ctx.ok()) {
                return cuError("cuCtxPushCurrent", ctx.result());
            }
            for (CUevent ev : ready) {
                const CUresult r = ev ? cuEventSynchronize(ev) : CUDA_SUCCESS;
                if (r != CUDA_SUCCESS) {
                    return cuError("cuEventSynchronize (slot ready)", r);
                }
            }
            std::lock_guard<std::mutex> lock(s.mutex);
            s.slots[slotIndex].readyConfirmed = true;
        }
        // Describe both lenses exactly the way fillDevicePlane() reads them.
        FramePair pair;
        pair.index = frameIndex;
        pair.ptsUs = ptsUs[0];
        const std::shared_ptr<void> owner = std::static_pointer_cast<void>(pin);
        for (std::size_t l = 0; l < 2; ++l) {
            const CUdeviceptr lensBase = base + l * s.lensOffsetBytes;
            DeviceFrameRef& d = pair.device[l];
            d.yDevice = toPointer(lensBase);
            d.uvDevice = toPointer(lensBase + s.chromaOffsetBytes);
            d.pitchBytes = s.pitch;
            d.width = s.width;
            d.height = s.height;
            d.bitShift = 6;
            d.bitDepth = 10;
            d.deviceIndex = s.deviceOrdinal;
            d.owner = owner;
            PlanarFrame16& f = pair.lens[l];
            f.width = s.width;
            f.height = s.height;
            f.chromaW = s.chromaW;
            f.chromaH = s.chromaH;
            f.bitDepth = 10;
            f.bitShift = 6;
            f.chromaInterleaved = true;
            f.narrowRange = narrow[l];
            f.ptsUs = ptsUs[l];
            f.frameIndex = frameIndex;
            f.owner = owner;
        }
        GpuFrameLease lease;
        lease.m_pin = std::move(pin);
        lease.m_pair = std::move(pair);
        lease.m_source = source;
        return lease;
    }
};

// =============================================================================
//  GpuClipDecoder
// =============================================================================
GpuClipDecoder::GpuClipDecoder() = default;

GpuClipDecoder::~GpuClipDecoder() {
    if (m_impl) {
        m_impl->shutdown();
    }
}

bool GpuClipDecoder::available(std::string* reason) noexcept {
    try {
        const CUresult init = ensureDriver();
        if (init != CUDA_SUCCESS) {
            if (reason) {
                *reason = "CUDA driver unavailable: " + cuText(init);
            }
            return false;
        }
        int count = 0;
        const CUresult counted = cuDeviceGetCount(&count);
        if (counted != CUDA_SUCCESS || count <= 0) {
            if (reason) {
                *reason = counted != CUDA_SUCCESS ? "cuDeviceGetCount failed: " + cuText(counted) : "no CUDA device";
            }
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

Result<std::unique_ptr<GpuClipDecoder>> GpuClipDecoder::open(const std::filesystem::path& path,
                                                             const meta::FormatInfo& format,
                                                             const GpuDecoderOptions& options) {
    try {
        // ---- arguments ---------------------------------------------------------
        if (path.empty()) {
            return Error{ErrorCode::InvalidArgument, "empty path"};
        }
        if (format.sideBySideProxy) {
            return Error{ErrorCode::InvalidArgument,
                         "the LRF proxy is one 8-bit side-by-side track; GpuClipDecoder serves the native lens tracks"};
        }
        if (format.videoTrackIds[0] == 0 || format.videoTrackIds[1] == 0) {
            return Error{ErrorCode::InvalidArgument, "FormatInfo names no video track for one of the lenses"};
        }
        if (format.videoTrackIds[0] == format.videoTrackIds[1]) {
            return Error{ErrorCode::InvalidArgument, "both lenses name the same track " +
                                                         std::to_string(format.videoTrackIds[0])};
        }
        if (options.decodeAhead > 256) {
            return Error{ErrorCode::InvalidArgument, "decodeAhead " + std::to_string(options.decodeAhead) +
                                                         " is out of range (0..256)"};
        }
        if (options.decoderThreads < 0 || options.decoderThreads > 16) {
            return Error{ErrorCode::InvalidArgument, "decoderThreads " + std::to_string(options.decoderThreads) +
                                                         " is out of range (0..16)"};
        }
        if (!options.cuContext && options.cudaDevice < 0) {
            return Error{ErrorCode::InvalidArgument, "negative CUDA device ordinal"};
        }

        // ---- driver --------------------------------------------------------------
        const CUresult init = ensureDriver();
        if (init != CUDA_SUCCESS) {
            return Error{ErrorCode::Unsupported, "CUDA driver unavailable: " + cuText(init)};
        }

        std::unique_ptr<GpuClipDecoder> decoder(new GpuClipDecoder());
        decoder->m_impl = std::make_unique<Impl>();
        Impl& impl = *decoder->m_impl;
        impl.store = std::make_shared<detail::GpuFrameStore>();
        detail::GpuFrameStore& s = *impl.store;

        // ---- context: the caller's, or the primary one (never a new one) ------
        if (options.cuContext) {
            s.context = static_cast<CUcontext>(options.cuContext);
            ScopedContext ctx(s.context);
            if (!ctx.ok()) {
                s.context = nullptr;  // not ours to touch again
                return Error{ErrorCode::InvalidArgument, "the supplied CUDA context cannot be made current: " +
                                                             cuText(ctx.result())};
            }
            CUdevice dev = 0;
            const CUresult r = cuCtxGetDevice(&dev);
            if (r != CUDA_SUCCESS) {
                return Error{ErrorCode::InvalidArgument, "cuCtxGetDevice on the supplied context: " + cuText(r)};
            }
            s.device = dev;
        } else {
            int count = 0;
            CUresult r = cuDeviceGetCount(&count);
            if (r != CUDA_SUCCESS || count <= 0) {
                return Error{ErrorCode::Unsupported, r != CUDA_SUCCESS ? "cuDeviceGetCount: " + cuText(r)
                                                                       : std::string("no CUDA device")};
            }
            if (options.cudaDevice >= count) {
                return Error{ErrorCode::InvalidArgument, "CUDA device " + std::to_string(options.cudaDevice) +
                                                             " out of range (" + std::to_string(count) + " devices)"};
            }
            CUdevice dev = 0;
            r = cuDeviceGet(&dev, options.cudaDevice);
            if (r != CUDA_SUCCESS) {
                return cuError("cuDeviceGet", r);
            }
            // Retain leaves the primary context's flags as the process set
            // them.  FFmpeg's AV_CUDA_USE_PRIMARY_CONTEXT would instead
            // insist on CU_CTX_SCHED_BLOCKING_SYNC and fail outright once
            // anything (cudart, a renderer) activated the context with the
            // default flags, so the context is handed to FFmpeg as external.
            CUcontext primary = nullptr;
            r = cuDevicePrimaryCtxRetain(&primary, dev);
            if (r != CUDA_SUCCESS || !primary) {
                return cuError("cuDevicePrimaryCtxRetain", r);
            }
            s.context = primary;
            s.device = dev;
            s.retainedPrimary = true;
        }
        // A CUdevice handle is the device ordinal.
        s.deviceOrdinal = static_cast<int>(s.device);

        // ---- streams -------------------------------------------------------------
        if (options.cuStream) {
            impl.streams = {static_cast<CUstream>(options.cuStream), static_cast<CUstream>(options.cuStream)};
            impl.ownsStreams = false;
        } else {
            ScopedContext ctx(s.context);
            if (!ctx.ok()) {
                return cuError("cuCtxPushCurrent", ctx.result());
            }
            impl.ownsStreams = true;
            for (CUstream& stream : impl.streams) {
                // Non-blocking: never serialise against the host's work on
                // the legacy default stream of a shared context.
                const CUresult r = cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);
                if (r != CUDA_SUCCESS) {
                    stream = nullptr;
                    return cuError("cuStreamCreate", r);
                }
            }
        }

        // ---- the lens decoders, opened in parallel ------------------------------
        OSV_TRY(impl.courier.start());
        std::array<DecoderOptions, 2> lensOptions{};
        for (std::size_t l = 0; l < 2; ++l) {
            DecoderOptions& o = lensOptions[l];
            o.hw = HwAccel::Cuda;
            o.keepOnDevice = true;
            o.cudaDeviceIndex = s.deviceOrdinal;
            o.cudaContext = s.context;
            o.cudaStream = impl.streams[l];
            o.threads = options.decoderThreads;
            o.useContainerSamples = true;  // frame index == container sample index
        }
        Result<HevcStreamDecoder> opened0 = Error{ErrorCode::Internal, "lens 0 was not opened"};
        impl.courier.post([&]() {
            try {
                opened0 = HevcStreamDecoder::open(path, format.videoTrackIds[0], lensOptions[0]);
            } catch (const std::exception& e) {
                opened0 = Error{ErrorCode::Internal, e.what()};
            } catch (...) {
                opened0 = Error{ErrorCode::Internal, "unknown failure"};
            }
        });
        Result<HevcStreamDecoder> opened1 = HevcStreamDecoder::open(path, format.videoTrackIds[1], lensOptions[1]);
        impl.courier.wait();
        if (!opened0.ok()) {
            return Error{opened0.error().code, "lens 0 (track " + std::to_string(format.videoTrackIds[0]) +
                                                   "): " + opened0.error().message};
        }
        if (!opened1.ok()) {
            return Error{opened1.error().code, "lens 1 (track " + std::to_string(format.videoTrackIds[1]) +
                                                   "): " + opened1.error().message};
        }
        impl.lens[0] = std::move(opened0).value();
        impl.lens[1] = std::move(opened1).value();

        // ---- NVDEC must really be in use, with matching 10-bit streams ------------
        for (std::size_t l = 0; l < 2; ++l) {
            if (impl.lens[l].activeHw() != HwAccel::Cuda) {
                return Error{ErrorCode::Unsupported, "NVDEC cannot decode lens " + std::to_string(l) +
                                                         " (libavcodec fell back to " +
                                                         hwAccelName(impl.lens[l].activeHw()) + ")"};
            }
            if (impl.lens[l].sourceBitDepth() != 10) {
                return Error{ErrorCode::Unsupported, "lens " + std::to_string(l) + " is " +
                                                         std::to_string(impl.lens[l].sourceBitDepth()) +
                                                         "-bit; the GPU decoder serves 10-bit streams"};
            }
        }
        const HevcStreamDecoder& a = impl.lens[0];
        const HevcStreamDecoder& b = impl.lens[1];
        if (a.width() == 0 || a.height() == 0 || a.width() != b.width() || a.height() != b.height()) {
            return Error{ErrorCode::Malformed, "lens streams differ in size: " + std::to_string(a.width()) + "x" +
                                                   std::to_string(a.height()) + " vs " + std::to_string(b.width()) +
                                                   "x" + std::to_string(b.height())};
        }
        if (std::fabs(a.fps() - b.fps()) > 1e-3) {
            return Error{ErrorCode::Malformed, "lens streams differ in frame rate"};
        }
        if (a.frameCount() != b.frameCount()) {
            log::warn("video/gpu: lens streams have {} and {} frames; using the smaller count", a.frameCount(),
                      b.frameCount());
        }
        impl.frameCount = std::min(a.frameCount(), b.frameCount());
        impl.fps = a.fps();
        if (impl.frameCount == 0) {
            return Error{ErrorCode::Malformed, "lens streams have no frames"};
        }
        std::int64_t tolerance = 1;
        for (const HevcStreamDecoder* d : {&a, &b}) {
            const TimeBase tb = d->timeBase();
            if (tb.num > 0 && tb.den > 0) {
                tolerance = std::max(tolerance, static_cast<std::int64_t>(std::ceil(
                                                    1e6 * static_cast<double>(tb.num) / static_cast<double>(tb.den))));
            }
        }
        impl.ptsToleranceUs = tolerance;

        // ---- slot geometry: one pitched allocation per frame pair ----------------
        s.width = a.width();
        s.height = a.height();
        s.chromaW = (s.width + 1) / 2;
        s.chromaH = (s.height + 1) / 2;
        s.rowBytes = std::max(static_cast<std::size_t>(s.width) * 2u, static_cast<std::size_t>(s.chromaW) * 4u);
        s.rows = 2u * (static_cast<std::size_t>(s.height) + s.chromaH);
        {
            ScopedContext ctx(s.context);
            if (!ctx.ok()) {
                return cuError("cuCtxPushCurrent", ctx.result());
            }
            // Free VRAM is sampled after the NVDEC decoders exist, so the
            // automatic budget reflects what is really left.
            std::size_t freeBytes = 0;
            std::size_t totalBytes = 0;
            CUresult r = cuMemGetInfo(&freeBytes, &totalBytes);
            if (r != CUDA_SUCCESS) {
                return cuError("cuMemGetInfo", r);
            }
            CUdeviceptr base = 0;
            std::size_t pitch = 0;
            r = cuMemAllocPitch(&base, &pitch, s.rowBytes, s.rows, 16);
            if (r != CUDA_SUCCESS) {
                return cuError("cuMemAllocPitch (first frame cache slot)", r);
            }
            s.pitch = pitch;
            s.chromaOffsetBytes = pitch * s.height;
            s.lensOffsetBytes = pitch * (static_cast<std::size_t>(s.height) + s.chromaH);
            s.slotBytes = pitch * s.rows;
            // Budget and capacity.
            std::size_t budget = options.vramBudgetBytes;
            if (budget == 0) {
                const double share = static_cast<double>(freeBytes) * kDefaultBudgetFreeShare;
                budget = std::min(kDefaultBudgetCapBytes, static_cast<std::size_t>(share));
            }
            s.budgetBytes = budget;
            std::size_t capacity = s.slotBytes > 0 ? budget / s.slotBytes : 0;
            // More slots than frames would never be used.
            capacity = std::min<std::size_t>(capacity, impl.frameCount);
            if (capacity == 0) {
                cuMemFree(base);
                return Error{ErrorCode::InvalidArgument, "VRAM budget of " + std::to_string(budget) +
                                                             " bytes is smaller than one frame pair (" +
                                                             std::to_string(s.slotBytes) + " bytes)"};
            }
            s.slots.resize(capacity);
            s.usableSlots = static_cast<std::uint32_t>(capacity);
            // The first slot's events; the memory is already there.
            s.slots[0].base = base;
            for (CUevent& ev : s.slots[0].ready) {
                r = cuEventCreate(&ev, CU_EVENT_DISABLE_TIMING);
                if (r != CUDA_SUCCESS) {
                    ev = nullptr;
                    return cuError("cuEventCreate (slot ready)", r);  // ~GpuFrameStore frees the slot
                }
            }
            s.allocatedSlots = 1;
        }
        s.frameToSlot.assign(impl.frameCount, -1);

        // ---- prime: frame 0 into the cache ------------------------------------------
        // Each lens decoder already decoded frame 0 once (the probe that proves
        // NVDEC works) and threw it away.  Decoding it once more through the
        // engine costs a few ms here but leaves frame 0 cached and the engine
        // positioned on frame 1, so the first landing anywhere in the first
        // GOP continues from there instead of paying a decoder flush plus a
        // re-decode of frame 0 (~6 ms of every cold park in that GOP).
        {
            auto primed = impl.decodePair(0, false, false);
            if (!primed.ok()) {
                return Error{primed.error().code, "priming frame 0: " + primed.error().message};
            }
        }

        // ---- decode-ahead ----------------------------------------------------------
        // Leave room for the frames the host holds while playing: the window
        // never takes more than capacity - 2 slots.
        const std::size_t capacity = s.slots.size();
        impl.aheadWindow = capacity > 2 ? static_cast<std::uint32_t>(
                                              std::min<std::size_t>(options.decodeAhead, capacity - 2))
                                        : 0u;
        if (impl.aheadWindow > 0) {
            Impl* self = &impl;
            impl.worker = std::thread([self] { self->workerLoop(); });
        }

        log::debug("video/gpu: opened {}: 2 x {}x{} P010, {} frames at {} fps, device {}, {} context, slot {} bytes "
                   "(pitch {}), cache {} slots / {} bytes budget, decode-ahead {}, {} decoder threads",
                   log::safe(path.filename().string()), s.width, s.height, impl.frameCount, impl.fps,
                   s.deviceOrdinal, s.retainedPrimary ? "primary" : "caller", s.slotBytes, s.pitch, capacity,
                   s.budgetBytes, impl.aheadWindow, options.decoderThreads);
        return decoder;
    } catch (const std::exception& e) {
        return Error{ErrorCode::Internal, std::string("GpuClipDecoder::open: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Internal, "GpuClipDecoder::open: unknown failure"};
    }
}

Result<GpuFrameLease> GpuClipDecoder::acquire(std::uint32_t frameIndex) {
    try {
        if (!m_impl || !m_impl->store) {
            return Error{ErrorCode::InvalidArgument, "decoder not open"};
        }
        Impl& impl = *m_impl;
        detail::GpuFrameStore& s = *impl.store;
        if (frameIndex >= impl.frameCount) {
            return Error{ErrorCode::InvalidArgument, "frame " + std::to_string(frameIndex) + " out of range (" +
                                                         std::to_string(impl.frameCount) + " frames)"};
        }
        // The pin object is allocated before any pin is taken, so nothing
        // that can throw sits between taking the pin and owning it.
        auto pin = std::make_shared<detail::GpuSlotPin>();

        std::unique_lock<std::mutex> lock(s.mutex);
        ++s.counters.acquires;
        if (impl.stopping) {
            return Error{ErrorCode::Internal, "decoder is shutting down"};
        }
        impl.noteAccessLocked(frameIndex);
        bool waited = false;
        for (;;) {
            // ---- cached: pin and go ------------------------------------------
            const std::int32_t cached = s.frameToSlot[frameIndex];
            if (cached >= 0) {
                detail::GpuSlot& slot = s.slots[static_cast<std::size_t>(cached)];
                ++slot.pins;
                slot.lastUse = ++s.clock;
                pin->store = impl.store;
                pin->slot = static_cast<std::uint32_t>(cached);
                if (waited) {
                    ++s.counters.waitedHits;
                } else {
                    ++s.counters.cacheHits;
                }
                lock.unlock();
                return impl.makeLease(std::move(pin), frameIndex,
                                      waited ? LeaseSource::WaitedForDecode : LeaseSource::CacheHit);
            }
            // ---- engine free: this call decodes ----------------------------
            if (!impl.engineBusy) {
                impl.engineBusy = true;
                ++s.counters.foregroundDecodes;
                break;
            }
            // ---- engine busy: wait for it to publish our frame or let go -----
            // (the worker yields to waiters after its current frame pair)
            waited = true;
            ++impl.foregroundWaiters;
            s.cv.wait(lock);
            --impl.foregroundWaiters;
            if (impl.foregroundWaiters == 0) {
                s.cv.notify_all();  // the worker may resume once nobody waits
            }
            if (impl.stopping) {
                return Error{ErrorCode::Internal, "decoder is shutting down"};
            }
        }
        lock.unlock();

        // ---- the decode: from the engine position or the sync sample --------------
        Impl::EngineGuard guard(impl);
        auto decoded = impl.decodeTo(frameIndex);
        if (!decoded.ok()) {
            return Error(decoded.error());
        }
        // decodeTo pinned the slot under the lock; adopting it cannot throw.
        pin->store = impl.store;
        pin->slot = decoded.value();
        guard.release();
        return impl.makeLease(std::move(pin), frameIndex, LeaseSource::Decoded);
    } catch (const std::exception& e) {
        return Error{ErrorCode::Internal, std::string("GpuClipDecoder::acquire: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Internal, "GpuClipDecoder::acquire: unknown failure"};
    }
}

bool GpuClipDecoder::isCached(std::uint32_t frameIndex) const noexcept {
    if (!m_impl || !m_impl->store) {
        return false;
    }
    try {
        const detail::GpuFrameStore& s = *m_impl->store;
        std::lock_guard<std::mutex> lock(s.mutex);
        return frameIndex < s.frameToSlot.size() && s.frameToSlot[frameIndex] >= 0;
    } catch (...) {
        return false;
    }
}

std::uint32_t GpuClipDecoder::dropCachedFrames() noexcept {
    if (!m_impl || !m_impl->store) {
        return 0;
    }
    try {
        detail::GpuFrameStore& s = *m_impl->store;
        std::lock_guard<std::mutex> lock(s.mutex);
        std::uint32_t dropped = 0;
        for (detail::GpuSlot& slot : s.slots) {
            // Pinned slots and the one being filled stay; the VRAM stays
            // allocated for reuse either way.
            if (slot.base == 0 || slot.pins > 0 || slot.writing || slot.frame < 0) {
                continue;
            }
            if (static_cast<std::size_t>(slot.frame) < s.frameToSlot.size()) {
                s.frameToSlot[static_cast<std::size_t>(slot.frame)] = -1;
            }
            slot.frame = -1;
            slot.readyConfirmed = false;
            ++dropped;
        }
        return dropped;
    } catch (...) {
        return 0;
    }
}

GpuDecoderStats GpuClipDecoder::stats() const noexcept {
    GpuDecoderStats out;
    if (!m_impl || !m_impl->store) {
        return out;
    }
    try {
        const detail::GpuFrameStore& s = *m_impl->store;
        std::lock_guard<std::mutex> lock(s.mutex);
        out = s.counters;
        out.cachedFrames = 0;
        out.leasedSlots = 0;
        for (const detail::GpuSlot& slot : s.slots) {
            if (slot.frame >= 0) {
                ++out.cachedFrames;
            }
            if (slot.pins > 0) {
                ++out.leasedSlots;
            }
        }
        out.allocatedSlots = s.allocatedSlots;
        out.capacitySlots = static_cast<std::uint32_t>(s.slots.size());
        out.decodeAheadWindow = m_impl->aheadWindow;
        out.slotBytes = s.slotBytes;
        out.vramBytes = static_cast<std::size_t>(s.allocatedSlots) * s.slotBytes;
        out.budgetBytes = s.budgetBytes;
    } catch (...) {
        // A snapshot that could not be taken is reported as empty.
    }
    return out;
}

std::uint32_t GpuClipDecoder::frameCount() const noexcept { return m_impl ? m_impl->frameCount : 0; }

double GpuClipDecoder::fps() const noexcept { return m_impl ? m_impl->fps : 0.0; }

std::uint32_t GpuClipDecoder::lensWidth() const noexcept {
    return (m_impl && m_impl->store) ? m_impl->store->width : 0;
}

std::uint32_t GpuClipDecoder::lensHeight() const noexcept {
    return (m_impl && m_impl->store) ? m_impl->store->height : 0;
}

void* GpuClipDecoder::cuContext() const noexcept {
    return (m_impl && m_impl->store) ? static_cast<void*>(m_impl->store->context) : nullptr;
}

int GpuClipDecoder::deviceIndex() const noexcept {
    return (m_impl && m_impl->store) ? m_impl->store->deviceOrdinal : 0;
}

#else  // !OSV_VIDEO_HAVE_CUDA

// =============================================================================
//  Builds without the CUDA toolkit: the type exists, open() says why not.
// =============================================================================
namespace detail {
/// Nothing can be pinned without CUDA; the type only has to be complete.
struct GpuSlotPin {};
}  // namespace detail

struct GpuClipDecoder::Impl {};

GpuFrameLease::GpuFrameLease() noexcept = default;
GpuFrameLease::~GpuFrameLease() = default;
GpuFrameLease::GpuFrameLease(GpuFrameLease&& other) noexcept
    : m_pin(std::move(other.m_pin)), m_pair(std::move(other.m_pair)), m_source(other.m_source) {}
GpuFrameLease& GpuFrameLease::operator=(GpuFrameLease&& other) noexcept {
    if (this != &other) {
        m_pin = std::move(other.m_pin);
        m_pair = std::move(other.m_pair);
        m_source = other.m_source;
    }
    return *this;
}
bool GpuFrameLease::valid() const noexcept { return false; }
std::uint32_t GpuFrameLease::frameIndex() const noexcept { return 0; }
const FramePair& GpuFrameLease::pair() const noexcept {
    static const FramePair kEmpty{};
    return kEmpty;
}
LeaseSource GpuFrameLease::source() const noexcept { return m_source; }
Status GpuFrameLease::releaseAfter(void* /*cuStream*/) noexcept {
    return failStatus(ErrorCode::InvalidArgument, "releaseAfter on an empty lease");
}
void GpuFrameLease::release() noexcept {
    m_pair = FramePair{};
    m_pin.reset();
}

GpuClipDecoder::GpuClipDecoder() = default;
GpuClipDecoder::~GpuClipDecoder() = default;

bool GpuClipDecoder::available(std::string* reason) noexcept {
    if (reason) {
        *reason = "this build of OpenOSV has no CUDA support";
    }
    return false;
}

Result<std::unique_ptr<GpuClipDecoder>> GpuClipDecoder::open(const std::filesystem::path& /*path*/,
                                                             const meta::FormatInfo& /*format*/,
                                                             const GpuDecoderOptions& /*options*/) {
    return Error{ErrorCode::Unsupported, "this build of OpenOSV has no CUDA support"};
}

Result<GpuFrameLease> GpuClipDecoder::acquire(std::uint32_t /*frameIndex*/) {
    return Error{ErrorCode::Unsupported, "this build of OpenOSV has no CUDA support"};
}

bool GpuClipDecoder::isCached(std::uint32_t /*frameIndex*/) const noexcept { return false; }
std::uint32_t GpuClipDecoder::dropCachedFrames() noexcept { return 0; }
GpuDecoderStats GpuClipDecoder::stats() const noexcept { return {}; }
std::uint32_t GpuClipDecoder::frameCount() const noexcept { return 0; }
double GpuClipDecoder::fps() const noexcept { return 0.0; }
std::uint32_t GpuClipDecoder::lensWidth() const noexcept { return 0; }
std::uint32_t GpuClipDecoder::lensHeight() const noexcept { return 0; }
void* GpuClipDecoder::cuContext() const noexcept { return nullptr; }
int GpuClipDecoder::deviceIndex() const noexcept { return 0; }

#endif  // OSV_VIDEO_HAVE_CUDA

}  // namespace osv::video
