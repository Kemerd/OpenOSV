// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxGpuPipeline.cpp - device pools, upload, framing / packing kernels and
// the banded readback of the own-GPU path (see OfxGpuPipeline.h).
//
// CUDA DRIVER API only (cuda.h, nvcuda.dll delay-loaded): like the rest of
// the module, nothing here imports a CUDA runtime DLL.  The kernels are the
// embedded fatbin's (OfxReframeKernel.cu), loaded per context by the cache
// in OfxCuda.cpp.

#include "OfxGpuPipeline.h"

#include "OfxCuda.h"
#include "OfxKernelAbi.h"

#include "PluginLog.h"

#if defined(OSV_OFX_HAVE_CUDA)
#include <cuda.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

namespace osv::ofx::gpu {

#if defined(OSV_OFX_HAVE_CUDA)

using osv::premiere::PluginLog;

namespace {

// ===========================================================================
//  Geometry and planning
// ===========================================================================

/// Largest edge of any image, window or frame this path accepts - the bound
/// PixelCopy and the importer use, and far beyond any timeline.
constexpr int kMaxEdge = 32768;

/// Copies smaller than this run on the calling thread: handing them to the
/// shared pool costs more than the memcpy.
constexpr std::size_t kPoolCopyBytes = std::size_t{1} << 20;

/// Rows of one pool task: at least this many bytes, so a task is worth its
/// queue round trip even for narrow rows.
constexpr std::size_t kPoolChunkBytes = std::size_t{256} << 10;

/// The intersection of two rectangles; empty (x2 <= x1 or y2 <= y1) when
/// they do not overlap.  (OfxRender.h has the same; this layer is kept free
/// of the effects' render code so the GPU tests can build it alone.)
[[nodiscard]] OfxRectI intersectRect(const OfxRectI& a, const OfxRectI& b) noexcept {
    OfxRectI r;
    r.x1 = std::max(a.x1, b.x1);
    r.y1 = std::max(a.y1, b.y1);
    r.x2 = std::min(a.x2, b.x2);
    r.y2 = std::min(a.y2, b.y2);
    if (r.x2 < r.x1) {
        r.x2 = r.x1;
    }
    if (r.y2 < r.y1) {
        r.y2 = r.y1;
    }
    return r;
}

/// True when a rectangle holds no pixel.
[[nodiscard]] bool emptyRect(const OfxRectI& r) noexcept { return r.x2 <= r.x1 || r.y2 <= r.y1; }

/// True when a rectangle's edges fit the kMaxEdge bound (and are not
/// inverted, which emptyRect() catches separately).
[[nodiscard]] bool rectFits(const OfxRectI& r) noexcept {
    // 64-bit differences: a garbage rectangle from a host must not overflow.
    const long long w = static_cast<long long>(r.x2) - static_cast<long long>(r.x1);
    const long long h = static_cast<long long>(r.y2) - static_cast<long long>(r.y1);
    return w > 0 && h > 0 && w <= kMaxEdge && h <= kMaxEdge;
}

/// What phase 1 decided about the target: the rectangle to fill, its packed
/// layout, how it is streamed back, and the kernels' pack block.
struct Plan {
    bool empty = true;               ///< The window misses the image: nothing to write.
    OfxRectI area{0, 0, 0, 0};       ///< window intersected with the image bounds.
    int bpp = 0;                     ///< Bytes per packed pixel (4 or 16).
    std::size_t packedPitch = 0;     ///< Bytes of one packed row (tight).
    std::uint32_t rowsPerBand = 0;   ///< Rows one staging band carries.
    std::uint32_t bands = 0;         ///< Bands the rectangle is streamed in.
    OsvOfxPack pack{};               ///< The kernels' view of the above.
};

/// Check `target` and plan its rectangle.  False with `error` for a target
/// no pixel may be written to; true with plan.empty for a window that misses
/// the image (a success that writes nothing).
[[nodiscard]] bool planTarget(const HostTarget& target, Plan& plan, std::string& error) noexcept {
    plan = Plan{};
    const HostImageView& image = target.image;
    if (!image.usable()) {
        error = "the host image is not usable (no data, empty bounds or a pitch shorter than a row)";
        return false;
    }
    if (image.depth != HostDepth::Byte && image.depth != HostDepth::Float) {
        error = "the host image has an unknown pixel depth";
        return false;
    }
    if (image.order != HostOrder::Rgba && image.order != HostOrder::Bgra) {
        error = "the host image has an unknown channel order";
        return false;
    }
    if (!rectFits(image.bounds)) {
        error = "the host image is larger than 32768 pixels on a side";
        return false;
    }
    if (target.levels != OutputLevels::Full && target.levels != OutputLevels::Studio) {
        error = "unknown output levels";
        return false;
    }

    // ---- the rectangle ------------------------------------------------------
    plan.area = intersectRect(target.window, image.bounds);
    if (emptyRect(plan.area)) {
        plan.empty = true;  // nothing of the window lies inside the image
        return true;
    }
    if (!rectFits(target.frame)) {
        error = "the camera frame is empty or larger than 32768 pixels on a side";
        return false;
    }
    plan.empty = false;

    // ---- packed layout and bands ----------------------------------------------
    const int areaW = plan.area.x2 - plan.area.x1;
    const int areaH = plan.area.y2 - plan.area.y1;
    plan.bpp = image.pixelBytes();
    plan.packedPitch = static_cast<std::size_t>(areaW) * static_cast<std::size_t>(plan.bpp);
    // A row is at most 32768 x 16 bytes = 512 KiB, so a band holds >= 16.
    plan.rowsPerBand = static_cast<std::uint32_t>(
        std::min<std::size_t>(kStageBandBytes / plan.packedPitch, static_cast<std::size_t>(areaH)));
    if (plan.rowsPerBand == 0) {
        error = "a packed row does not fit a staging band";
        return false;
    }
    plan.bands = (static_cast<std::uint32_t>(areaH) + plan.rowsPerBand - 1u) / plan.rowsPerBand;

    // ---- the kernels' pack block ------------------------------------------------
    OsvOfxPack& pk = plan.pack;
    pk.windowX1 = plan.area.x1;
    pk.windowY1 = plan.area.y1;
    pk.windowW = areaW;
    pk.windowH = areaH;
    pk.frameX1 = target.frame.x1;
    pk.frameY1 = target.frame.y1;
    pk.frameX2 = target.frame.x2;
    pk.frameY2 = target.frame.y2;
    pk.dstPitchBytes = static_cast<int>(plan.packedPitch);
    pk.isByte = image.depth == HostDepth::Byte ? 1 : 0;
    pk.isBgra = image.order == HostOrder::Bgra ? 1 : 0;
    pk.studio = target.levels == OutputLevels::Studio ? 1 : 0;
    // The constants cross as the HOST computed them, so the kernel's levels
    // and the CPU's studioFromFull() multiply and add the very same bits.
    pk.studioBlack = kStudioBlack;
    pk.studioSpan = kStudioSpan;
    // promoteIntegerToFloat()'s scale for 8-bit codes, spelled the same way.
    pk.byteScale = 1.0f / 255.0f;
    return true;
}

/// Run `copyRow(i)` for i in [0, rows), on `pool` when the total is worth it
/// (and falling back to this thread if the pool refuses the job - a row copy
/// is idempotent, so repeating rows a failed job already did is harmless).
void copyRows(std::uint32_t rows, std::size_t rowBytes, ThreadPool* pool,
              const std::function<void(std::size_t)>& copyRow) noexcept {
    if (rows == 0 || rowBytes == 0) {
        return;
    }
    const std::size_t total = static_cast<std::size_t>(rows) * rowBytes;
    if (pool && total >= kPoolCopyBytes) {
        const std::size_t grain = std::max<std::size_t>(1, (kPoolChunkBytes + rowBytes - 1) / rowBytes);
        try {
            if (pool->parallelRows(rows, grain, copyRow).ok()) {
                return;
            }
        } catch (...) {
            // std::function / queue allocation: copy on this thread instead.
        }
    }
    for (std::uint32_t r = 0; r < rows; ++r) {
        copyRow(r);
    }
}

// ===========================================================================
//  Pools, slots, jobs
// ===========================================================================

/// "what: CUDA_ERROR_NAME".
[[nodiscard]] std::string cudaText(const char* what, CUresult r) {
    const char* name = nullptr;
    (void)cuGetErrorName(r, &name);
    return std::string(what ? what : "cuda") + ": " + (name ? name : "CUDA_ERROR_UNKNOWN");
}

/// Push a context for a scope and pop it again: the caller's context stack
/// is left exactly as it was, whatever it held.
class ContextPush {
public:
    explicit ContextPush(CUcontext context) noexcept
        : m_pushed(context != nullptr && cuCtxPushCurrent(context) == CUDA_SUCCESS) {}
    ~ContextPush() {
        if (m_pushed) {
            CUcontext popped = nullptr;
            (void)cuCtxPopCurrent(&popped);
        }
    }
    ContextPush(const ContextPush&) = delete;
    ContextPush& operator=(const ContextPush&) = delete;
    [[nodiscard]] bool ok() const noexcept { return m_pushed; }

private:
    bool m_pushed = false;
};

/// Buffer requests are rounded up to this step, so a render window that
/// changes by a few rows does not reallocate on every frame.
constexpr std::size_t kAllocationStep = std::size_t{2} << 20;

/// One render's GPU resources.  Owned by its pool; used by one job at a time.
struct Slot {
    CUstream stream = nullptr;              ///< Non-blocking: never waits behind anyone else's work.
    CUevent band[kStageBands] = {};         ///< "the DMA through pinned band k has completed".
    CUevent kernelDone = nullptr;           ///< "the frame / pack kernel has finished".
    void* stage[kStageBands] = {};          ///< Pinned, portable host bands of kStageBandBytes.
    CUdeviceptr source = 0;                 ///< The filter's uploaded host image.
    std::size_t sourceBytes = 0;
    CUdeviceptr output = 0;                 ///< The packed rectangle.
    std::size_t outputBytes = 0;

    /// Free the two device buffers.  The slot's context is current and its
    /// stream idle.
    void freeBuffers() noexcept {
        if (source) {
            (void)cuMemFree(source);
            source = 0;
            sourceBytes = 0;
        }
        if (output) {
            (void)cuMemFree(output);
            output = 0;
            outputBytes = 0;
        }
    }

    /// Free everything, in reverse order of creation.  The slot's context is
    /// current.  Waits for the stream first: nothing may still be reading or
    /// writing what is freed.
    void destroy() noexcept {
        if (stream) {
            (void)cuStreamSynchronize(stream);
        }
        freeBuffers();
        for (int i = 0; i < kStageBands; ++i) {
            if (band[i]) {
                (void)cuEventDestroy(band[i]);
                band[i] = nullptr;
            }
            if (stage[i]) {
                (void)cuMemFreeHost(stage[i]);
                stage[i] = nullptr;
            }
        }
        if (kernelDone) {
            (void)cuEventDestroy(kernelDone);
            kernelDone = nullptr;
        }
        if (stream) {
            (void)cuStreamDestroy(stream);
            stream = nullptr;
        }
    }
};

/// The slots of one CUDA device, all in its retained primary context.
class DevicePool {
public:
    /// A pool for `ordinal`, its primary context retained.  Null with
    /// `error` set when the device cannot be used.
    [[nodiscard]] static std::shared_ptr<DevicePool> create(int ordinal, std::string& error) noexcept {
        try {
            if (ordinal < 0) {
                error = "negative CUDA device ordinal";
                return nullptr;
            }
            CUresult r = cuInit(0);
            if (r != CUDA_SUCCESS) {
                error = cudaText("cuInit", r);
                return nullptr;
            }
            int count = 0;
            r = cuDeviceGetCount(&count);
            if (r != CUDA_SUCCESS || ordinal >= count) {
                error = r != CUDA_SUCCESS ? cudaText("cuDeviceGetCount", r)
                                          : "CUDA device " + std::to_string(ordinal) + " does not exist";
                return nullptr;
            }
            CUdevice device = 0;
            r = cuDeviceGet(&device, ordinal);
            if (r != CUDA_SUCCESS) {
                error = cudaText("cuDeviceGet", r);
                return nullptr;
            }
            // The PRIMARY context: the one the CUDA runtime - and with it the
            // clip engine's CudaRenderer - uses on this device, so the
            // stitched sphere is readable here in place.  Retaining leaves its
            // flags as whoever activated it first set them.
            CUcontext context = nullptr;
            r = cuDevicePrimaryCtxRetain(&context, device);
            if (r != CUDA_SUCCESS || !context) {
                error = cudaText("cuDevicePrimaryCtxRetain", r);
                return nullptr;
            }
            std::shared_ptr<DevicePool> pool(new DevicePool(ordinal, device, context));
            PluginLog::info("ofx gpu: device {} ready for the own-GPU path (primary context {}, up to {} slots of "
                            "{} x {} MiB pinned bands)",
                            ordinal, static_cast<const void*>(context), kMaxSlotsPerDevice, kStageBands,
                            kStageBandBytes >> 20);
            return pool;
        } catch (...) {
            error = "exception while creating a GPU device pool";
            return nullptr;
        }
    }

    ~DevicePool() {
        {
            ContextPush push(m_context);
            if (push.ok()) {
                for (auto& slot : m_slots) {
                    if (slot) {
                        slot->destroy();
                    }
                }
                // The kernels this path loaded into the primary context: it is
                // certainly alive (this pool still holds it).
                cuda::unloadModulesIn(m_context);
            }
        }
        m_slots.clear();
        (void)cuDevicePrimaryCtxRelease(m_device);
    }

    DevicePool(const DevicePool&) = delete;
    DevicePool& operator=(const DevicePool&) = delete;

    [[nodiscard]] CUcontext context() const noexcept { return m_context; }
    [[nodiscard]] int ordinal() const noexcept { return m_ordinal; }

    [[nodiscard]] int slotCount() noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_slots.size());
    }

    /// A free slot, created when fewer than kMaxSlotsPerDevice exist, else
    /// the next one returned - waiting at most kSlotWaitSeconds.  Null with
    /// `error` set when none could be had.
    [[nodiscard]] Slot* checkout(std::string& error) noexcept {
        try {
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kSlotWaitSeconds);
            for (;;) {
                // A free slot first, then room for a new one - re-checked after
                // every wake, since a failed creation frees room too.
                if (!m_idle.empty()) {
                    Slot* slot = m_idle.back();
                    m_idle.pop_back();
                    return slot;
                }
                if (static_cast<int>(m_slots.size()) + m_creating < kMaxSlotsPerDevice) {
                    ++m_creating;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    error = "all " + std::to_string(kMaxSlotsPerDevice) + " GPU slots of device " +
                            std::to_string(m_ordinal) + " stayed busy for " + std::to_string(kSlotWaitSeconds) + " s";
                    return nullptr;
                }
                (void)m_cv.wait_until(lock, deadline);
            }
            // Created OUTSIDE the lock: pinning 16 MiB takes a few ms, and the
            // other renders' checkouts and checkins must not wait for it.
            lock.unlock();
            std::unique_ptr<Slot> made = makeSlot(error);
            lock.lock();
            --m_creating;
            if (!made) {
                m_cv.notify_one();  // the capacity this call reserved is free again
                return nullptr;
            }
            Slot* raw = made.get();
            m_slots.push_back(std::move(made));
            return raw;
        } catch (...) {
            error = "exception while checking out a GPU slot";
            return nullptr;
        }
    }

    /// Return a slot.  Its stream must be idle (FrameJob waits before this).
    void checkin(Slot* slot) noexcept {
        if (!slot) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_idle.push_back(slot);
        } catch (...) {
            // push_back into a vector reserved to kMaxSlotsPerDevice below
            // cannot allocate; nothing else here throws.
        }
        m_cv.notify_one();
    }

    /// Free the device buffers of every idle slot (the pool's context is
    /// current): the answer to an out-of-memory allocation, before it is
    /// retried once.
    void trimIdle() noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Slot* slot : m_idle) {
            if (slot) {
                (void)cuStreamSynchronize(slot->stream);
                slot->freeBuffers();
            }
        }
    }

private:
    DevicePool(int ordinal, CUdevice device, CUcontext context) noexcept
        : m_ordinal(ordinal), m_device(device), m_context(context) {
        try {
            m_idle.reserve(kMaxSlotsPerDevice);
            m_slots.reserve(kMaxSlotsPerDevice);
        } catch (...) {
            // Reservation is only an optimisation.
        }
    }

    /// A new slot: stream, events and pinned bands in the pool's context.
    [[nodiscard]] std::unique_ptr<Slot> makeSlot(std::string& error) noexcept {
        try {
            auto slot = std::make_unique<Slot>();
            ContextPush push(m_context);
            if (!push.ok()) {
                error = "cannot make device " + std::to_string(m_ordinal) + "'s primary context current";
                return nullptr;
            }
            CUresult r = cuStreamCreate(&slot->stream, CU_STREAM_NON_BLOCKING);
            if (r != CUDA_SUCCESS) {
                slot->stream = nullptr;
                error = cudaText("cuStreamCreate", r);
                slot->destroy();
                return nullptr;
            }
            // Blocking sync: a render thread waiting for a DMA sleeps instead
            // of spinning a core the host (and the pool's copies) could use.
            constexpr unsigned kEventFlags = CU_EVENT_DISABLE_TIMING | CU_EVENT_BLOCKING_SYNC;
            r = cuEventCreate(&slot->kernelDone, kEventFlags);
            if (r != CUDA_SUCCESS) {
                slot->kernelDone = nullptr;
                error = cudaText("cuEventCreate", r);
                slot->destroy();
                return nullptr;
            }
            for (int i = 0; i < kStageBands; ++i) {
                r = cuEventCreate(&slot->band[i], kEventFlags);
                if (r != CUDA_SUCCESS) {
                    slot->band[i] = nullptr;
                    error = cudaText("cuEventCreate", r);
                    slot->destroy();
                    return nullptr;
                }
                // PORTABLE: pinned for every context, whichever is current.
                r = cuMemHostAlloc(&slot->stage[i], kStageBandBytes, CU_MEMHOSTALLOC_PORTABLE);
                if (r != CUDA_SUCCESS || !slot->stage[i]) {
                    slot->stage[i] = nullptr;
                    error = cudaText("cuMemHostAlloc", r);
                    slot->destroy();
                    return nullptr;
                }
            }
            PluginLog::debug("ofx gpu: device {} slot created ({} in all)", m_ordinal, m_slots.size() + 1);
            return slot;
        } catch (...) {
            error = "exception while creating a GPU slot";
            return nullptr;
        }
    }

    int m_ordinal = -1;
    CUdevice m_device = 0;
    CUcontext m_context = nullptr;  ///< Retained primary context; released last.
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<std::unique_ptr<Slot>> m_slots;  ///< Every slot ever created (owned).
    std::vector<Slot*> m_idle;                   ///< The ones free right now.
    int m_creating = 0;                          ///< Slots being created outside the lock.
};

/// The live pools, per device ordinal.  Allocated once and never destroyed
/// by a static destructor: at process exit the driver may already be gone,
/// so only releaseDevicePools() (kOfxActionUnload) frees the pools.
struct Registry {
    std::mutex mutex;
    std::map<int, std::shared_ptr<DevicePool>> pools;
};

[[nodiscard]] Registry* registry() noexcept {
    static Registry* const instance = []() noexcept -> Registry* {
        try {
            return new Registry();
        } catch (...) {
            return nullptr;
        }
    }();
    return instance;
}

/// The pool of `ordinal`, created on first use.
[[nodiscard]] std::shared_ptr<DevicePool> poolFor(int ordinal, std::string& error) noexcept {
    Registry* reg = registry();
    if (!reg) {
        error = "out of memory for the GPU pool registry";
        return nullptr;
    }
    try {
        std::lock_guard<std::mutex> lock(reg->mutex);
        if (auto it = reg->pools.find(ordinal); it != reg->pools.end() && it->second) {
            return it->second;
        }
        // Under the registry lock: two first renders must not both retain
        // and log.  Creating a pool is a few driver calls (the primary
        // context may be created here, once per process).
        std::shared_ptr<DevicePool> made = DevicePool::create(ordinal, error);
        if (!made) {
            return nullptr;
        }
        reg->pools[ordinal] = made;
        return made;
    } catch (...) {
        error = "exception while looking up a GPU device pool";
        return nullptr;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  FrameJob
// ---------------------------------------------------------------------------

struct FrameJob::Impl {
    std::shared_ptr<DevicePool> pool;  ///< Keeps the slot's pool (and context) alive.
    Slot* slot = nullptr;              ///< Checked out of `pool`, or null.
    bool inFlight = false;             ///< Work may still be queued on the slot's stream.
    bool planned = false;              ///< Phase 1 succeeded; readBack() may run.
    HostTarget target;                 ///< Where readBack() writes.
    Plan plan;                         ///< What phase 1 decided.
    std::uint32_t issued = 0;          ///< Readback bands queued so far.

    ~Impl() { release(); }

    /// Return the slot (after its stream went idle) and drop the pool.
    void release() noexcept {
        if (slot && pool) {
            if (inFlight) {
                // An abandoned job: never let a DMA still write into a pinned
                // band, or a kernel still write the output, after the slot is
                // someone else's.
                ContextPush push(pool->context());
                (void)cuStreamSynchronize(slot->stream);
            }
            pool->checkin(slot);
        }
        slot = nullptr;
        pool.reset();
        inFlight = false;
        planned = false;
    }

    /// Hold a slot on device `ordinal`.
    [[nodiscard]] bool reserve(int ordinal, std::string& error) noexcept {
        if (slot && pool && pool->ordinal() == ordinal) {
            return true;
        }
        release();
        if (ordinal < 0) {
            error = "no CUDA device to render on";
            return false;
        }
        // The driver must be there before the first driver call, or the
        // delay-load stub raises instead of returning an error.
        if (!cuda::driverPresent()) {
            error = "the NVIDIA driver (nvcuda.dll) is not available";
            return false;
        }
        std::shared_ptr<DevicePool> p = poolFor(ordinal, error);
        if (!p) {
            return false;
        }
        Slot* s = p->checkout(error);
        if (!s) {
            return false;
        }
        pool = std::move(p);
        slot = s;
        return true;
    }

    /// Make `buffer` at least `bytes` large.  The pool's context is current;
    /// the slot's stream is idle or only holds work that no longer needs
    /// the old buffer's contents (it is synchronised before a free anyway).
    [[nodiscard]] bool ensure(CUdeviceptr& buffer, std::size_t& capacity, std::size_t bytes,
                              std::string& error) noexcept {
        if (buffer && capacity >= bytes) {
            return true;
        }
        const std::size_t want = (bytes + kAllocationStep - 1) / kAllocationStep * kAllocationStep;
        if (buffer) {
            (void)cuStreamSynchronize(slot->stream);
            (void)cuMemFree(buffer);
            buffer = 0;
            capacity = 0;
        }
        CUresult r = cuMemAlloc(&buffer, want);
        if (r == CUDA_ERROR_OUT_OF_MEMORY) {
            // Other idle slots may be sitting on big buffers: give them back
            // and try once more before the frame goes to the CPU.
            pool->trimIdle();
            r = cuMemAlloc(&buffer, want);
        }
        if (r != CUDA_SUCCESS || !buffer) {
            buffer = 0;
            error = cudaText("cuMemAlloc", r) + " (" + std::to_string(want >> 20) + " MiB)";
            return false;
        }
        capacity = want;
        return true;
    }

    /// Queue the DMA of readback band `index` into its pinned band and the
    /// event that says it landed.  The pool's context is current.
    [[nodiscard]] bool issueBand(std::uint32_t index, std::string& error) noexcept {
        const std::uint32_t areaH = static_cast<std::uint32_t>(plan.area.y2 - plan.area.y1);
        const std::uint32_t row0 = index * plan.rowsPerBand;
        if (index >= plan.bands || row0 >= areaH) {
            error = "readback band out of range";
            return false;
        }
        const std::uint32_t rows = std::min(plan.rowsPerBand, areaH - row0);
        const int k = static_cast<int>(index % static_cast<std::uint32_t>(kStageBands));
        const CUdeviceptr from = slot->output + static_cast<CUdeviceptr>(row0) * plan.packedPitch;
        CUresult r = cuMemcpyDtoHAsync(slot->stage[k], from, static_cast<std::size_t>(rows) * plan.packedPitch,
                                       slot->stream);
        if (r != CUDA_SUCCESS) {
            error = cudaText("readback DMA", r);
            return false;
        }
        r = cuEventRecord(slot->band[k], slot->stream);
        if (r != CUDA_SUCCESS) {
            error = cudaText("cuEventRecord", r);
            return false;
        }
        ++issued;
        return true;
    }

    /// Launch `kernel` over the planned rectangle, record its completion,
    /// queue the first readback bands behind it and - when `waitForKernel` -
    /// wait until the kernel has finished reading its source.  The pool's
    /// context is current and the output buffer is allocated.
    [[nodiscard]] bool launchAndQueue(CUfunction kernel, void** args, bool waitForKernel,
                                      std::string& error) noexcept {
        const unsigned gridX = static_cast<unsigned>((plan.pack.windowW + OSV_OFX_BLOCK_X - 1) / OSV_OFX_BLOCK_X);
        const unsigned gridY = static_cast<unsigned>((plan.pack.windowH + OSV_OFX_BLOCK_Y - 1) / OSV_OFX_BLOCK_Y);
        CUresult r = cuLaunchKernel(kernel, gridX, gridY, 1u, OSV_OFX_BLOCK_X, OSV_OFX_BLOCK_Y, 1u, 0u, slot->stream,
                                    args, nullptr);
        if (r != CUDA_SUCCESS) {
            error = cudaText("cuLaunchKernel", r);
            return false;
        }
        inFlight = true;
        r = cuEventRecord(slot->kernelDone, slot->stream);
        if (r != CUDA_SUCCESS) {
            error = cudaText("cuEventRecord", r);
            return false;
        }
        // The first bands cross the bus as soon as the kernel is done, while
        // the caller is still releasing whatever it held for phase 1.
        issued = 0;
        const std::uint32_t first = std::min<std::uint32_t>(plan.bands, static_cast<std::uint32_t>(kStageBands));
        for (std::uint32_t b = 0; b < first; ++b) {
            if (!issueBand(b, error)) {
                return false;
            }
        }
        if (waitForKernel) {
            r = cuEventSynchronize(slot->kernelDone);
            if (r != CUDA_SUCCESS) {
                error = cudaText("frame kernel", r);
                return false;
            }
        }
        planned = true;
        return true;
    }

    /// Common start of every phase 1: plan the target, and for a non-empty
    /// rectangle make sure a slot is held and the output buffer is big
    /// enough.  `nothingToDo` is set for an empty rectangle (then the job is
    /// planned and readBack() succeeds without a GPU call).
    [[nodiscard]] bool begin(const HostTarget& t, int ordinal, bool& nothingToDo, std::string& error) noexcept {
        planned = false;
        nothingToDo = false;
        target = t;
        if (!planTarget(target, plan, error)) {
            return false;
        }
        if (plan.empty) {
            nothingToDo = true;
            planned = true;
            return true;
        }
        return reserve(ordinal, error);
    }
};

FrameJob::FrameJob() noexcept {
    try {
        m_impl = std::make_unique<Impl>();
    } catch (...) {
        m_impl.reset();  // every member function reports the missing state
    }
}

FrameJob::~FrameJob() = default;

int FrameJob::device() const noexcept { return (m_impl && m_impl->pool) ? m_impl->pool->ordinal() : -1; }

bool FrameJob::reserve(int ordinal, std::string& error) noexcept {
    if (!m_impl) {
        error = "out of memory for a GPU frame job";
        return false;
    }
    return m_impl->reserve(ordinal, error);
}

bool FrameJob::frameDevice(const OsvReframeParams& params, const DeviceRgba& source, const HostTarget& target,
                           std::string& error) noexcept {
    if (!m_impl) {
        error = "out of memory for a GPU frame job";
        return false;
    }
    Impl& job = *m_impl;
    // ---- the source ---------------------------------------------------------------
    if (!source.data || source.device < 0) {
        error = "no device image to frame";
        return false;
    }
    if (source.width == 0 || source.height == 0 || source.width > static_cast<std::uint32_t>(kMaxEdge) ||
        source.height > static_cast<std::uint32_t>(kMaxEdge)) {
        error = "the device image is empty or larger than 32768 pixels on a side";
        return false;
    }
    if (source.pitchBytes < static_cast<std::size_t>(source.width) * 16u ||
        source.pitchBytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "the device image's pitch does not hold a row of float RGBA (or exceeds 2 GB)";
        return false;
    }
    // ---- the target and the slot -------------------------------------------------
    bool nothing = false;
    if (!job.begin(target, source.device, nothing, error)) {
        return false;
    }
    if (nothing) {
        return true;
    }
    if (params.outW != target.frame.x2 - target.frame.x1 || params.outH != target.frame.y2 - target.frame.y1) {
        error = "the camera was built for another frame size";
        return false;
    }

    ContextPush push(job.pool->context());
    if (!push.ok()) {
        error = "cannot make the GPU device's primary context current";
        return false;
    }
    // The sphere must live in THIS context (the engine's primary context of
    // the same device): ask the pointer itself, as the direct path does.
    CUcontext owner = nullptr;
    const CUresult asked = cuPointerGetAttribute(&owner, CU_POINTER_ATTRIBUTE_CONTEXT,
                                                 reinterpret_cast<CUdeviceptr>(source.data));
    if (asked != CUDA_SUCCESS || owner != job.pool->context()) {
        error = asked != CUDA_SUCCESS ? cudaText("the sphere is not device memory", asked)
                                      : "the sphere lives in another CUDA context than the device's primary one";
        return false;
    }
    cuda::OwnKernels kernels;
    if (!cuda::ownKernels(job.pool->context(), kernels, error)) {
        return false;
    }
    const std::size_t outBytes = job.plan.packedPitch * static_cast<std::size_t>(job.plan.pack.windowH);
    if (!job.ensure(job.slot->output, job.slot->outputBytes, outBytes, error)) {
        return false;
    }

    // ---- the launch -----------------------------------------------------------------
    // The engine's sphere: R,G,B,A floats, top row first, positive pitch.
    OsvReframeParams p = params;
    OsvRgbaSource s{};
    s.w = static_cast<int>(source.width);
    s.h = static_cast<int>(source.height);
    s.pitchBytes = static_cast<int>(source.pitchBytes);
    s.isHalf = 0;
    s.isBgra = 0;
    s.flipY = 0;
    const void* row0 = source.data;
    void* dst = reinterpret_cast<void*>(static_cast<std::uintptr_t>(job.slot->output));
    OsvOfxPack pack = job.plan.pack;
    void* args[] = {&p, &s, &row0, &dst, &pack};
    return job.launchAndQueue(static_cast<CUfunction>(kernels.viewFloat), args, /*waitForKernel=*/true, error);
}

bool FrameJob::packDevice(const DeviceRgba& image, const HostTarget& target, std::string& error) noexcept {
    if (!m_impl) {
        error = "out of memory for a GPU frame job";
        return false;
    }
    Impl& job = *m_impl;
    if (!image.data || image.device < 0 || image.width == 0 || image.height == 0) {
        error = "no device image to pack";
        return false;
    }
    if (image.pitchBytes < static_cast<std::size_t>(image.width) * 16u) {
        error = "the device image's pitch does not hold a row of float RGBA";
        return false;
    }
    // The image IS the camera frame: anything else would stretch or crop.
    const long long frameW = static_cast<long long>(target.frame.x2) - target.frame.x1;
    const long long frameH = static_cast<long long>(target.frame.y2) - target.frame.y1;
    if (static_cast<long long>(image.width) != frameW || static_cast<long long>(image.height) != frameH) {
        error = "the device image is not the camera frame's size";
        return false;
    }
    bool nothing = false;
    if (!job.begin(target, image.device, nothing, error)) {
        return false;
    }
    if (nothing) {
        return true;
    }

    ContextPush push(job.pool->context());
    if (!push.ok()) {
        error = "cannot make the GPU device's primary context current";
        return false;
    }
    CUcontext owner = nullptr;
    const CUresult asked = cuPointerGetAttribute(&owner, CU_POINTER_ATTRIBUTE_CONTEXT,
                                                 reinterpret_cast<CUdeviceptr>(image.data));
    if (asked != CUDA_SUCCESS || owner != job.pool->context()) {
        error = asked != CUDA_SUCCESS ? cudaText("the sphere is not device memory", asked)
                                      : "the sphere lives in another CUDA context than the device's primary one";
        return false;
    }
    cuda::OwnKernels kernels;
    if (!cuda::ownKernels(job.pool->context(), kernels, error)) {
        return false;
    }
    const std::size_t outBytes = job.plan.packedPitch * static_cast<std::size_t>(job.plan.pack.windowH);
    if (!job.ensure(job.slot->output, job.slot->outputBytes, outBytes, error)) {
        return false;
    }

    const void* src = image.data;
    unsigned long long pitch = static_cast<unsigned long long>(image.pitchBytes);
    int w = static_cast<int>(image.width);
    int h = static_cast<int>(image.height);
    void* dst = reinterpret_cast<void*>(static_cast<std::uintptr_t>(job.slot->output));
    OsvOfxPack pack = job.plan.pack;
    void* args[] = {&src, &pitch, &w, &h, &dst, &pack};
    return job.launchAndQueue(static_cast<CUfunction>(kernels.equirect), args, /*waitForKernel=*/true, error);
}

bool FrameJob::frameHost(const OsvReframeParams& params, const HostImageView& source, const HostTarget& target,
                         int ordinal, ThreadPool* pool, std::string& error) noexcept {
    if (!m_impl) {
        error = "out of memory for a GPU frame job";
        return false;
    }
    Impl& job = *m_impl;
    // ---- the source -------------------------------------------------------------------
    if (!source.usable()) {
        error = "the source image is not usable (no data, empty bounds or a pitch shorter than a row)";
        return false;
    }
    if (source.depth != HostDepth::Byte && source.depth != HostDepth::Float) {
        error = "the source image has an unknown pixel depth";
        return false;
    }
    if (source.order != HostOrder::Rgba && source.order != HostOrder::Bgra) {
        error = "the source image has an unknown channel order";
        return false;
    }
    if (!rectFits(source.bounds)) {
        error = "the source image is larger than 32768 pixels on a side";
        return false;
    }
    // ---- the target and the slot ----------------------------------------------------
    bool nothing = false;
    if (!job.begin(target, ordinal, nothing, error)) {
        return false;
    }
    if (nothing) {
        return true;
    }
    if (params.outW != target.frame.x2 - target.frame.x1 || params.outH != target.frame.y2 - target.frame.y1) {
        error = "the camera was built for another frame size";
        return false;
    }

    ContextPush push(job.pool->context());
    if (!push.ok()) {
        error = "cannot make the GPU device's primary context current";
        return false;
    }
    cuda::OwnKernels kernels;
    if (!cuda::ownKernels(job.pool->context(), kernels, error)) {
        return false;
    }
    Slot& slot = *job.slot;

    // ---- device buffers -----------------------------------------------------------------
    // The source travels in its own format - a quarter of the bytes for an
    // 8-bit project - with tight rows, bottom row first (OpenFX's y-up order,
    // whatever the host's pitch sign or padding).
    const int w = source.width();
    const int h = source.height();
    const std::size_t srcTight = static_cast<std::size_t>(w) * static_cast<std::size_t>(source.pixelBytes());
    const std::size_t srcBytes = srcTight * static_cast<std::size_t>(h);
    if (!job.ensure(slot.source, slot.sourceBytes, srcBytes, error)) {
        return false;
    }
    const std::size_t outBytes = job.plan.packedPitch * static_cast<std::size_t>(job.plan.pack.windowH);
    if (!job.ensure(slot.output, slot.outputBytes, outBytes, error)) {
        return false;
    }

    // ---- upload, band by band -------------------------------------------------------------
    // Copy band b into pinned band b % 2 while the DMA of band b - 1 is in
    // flight; before a pinned band is refilled, wait for the DMA that read
    // it.  The kernel is queued behind the last DMA on the same stream.
    const std::uint32_t rowsPerUpload =
        static_cast<std::uint32_t>(std::min<std::size_t>(kStageBandBytes / srcTight, static_cast<std::size_t>(h)));
    if (rowsPerUpload == 0) {
        error = "a source row does not fit a staging band";
        return false;
    }
    const std::uint32_t uploads = (static_cast<std::uint32_t>(h) + rowsPerUpload - 1u) / rowsPerUpload;
    for (std::uint32_t b = 0; b < uploads; ++b) {
        const int k = static_cast<int>(b % static_cast<std::uint32_t>(kStageBands));
        if (b >= static_cast<std::uint32_t>(kStageBands)) {
            const CUresult waited = cuEventSynchronize(slot.band[k]);
            if (waited != CUDA_SUCCESS) {
                error = cudaText("upload wait", waited);
                return false;
            }
        }
        const std::uint32_t row0 = b * rowsPerUpload;
        const std::uint32_t rows = std::min(rowsPerUpload, static_cast<std::uint32_t>(h) - row0);
        char* stage = static_cast<char*>(slot.stage[k]);
        // Host row (bounds.y1 + row0 + i) -> pinned row i; pixel() honours a
        // negative or padded pitch.
        copyRows(rows, srcTight, pool, [&](std::size_t i) {
            const int y = source.bounds.y1 + static_cast<int>(row0) + static_cast<int>(i);
            std::memcpy(stage + i * srcTight, source.pixel(source.bounds.x1, y), srcTight);
        });
        const CUdeviceptr to = slot.source + static_cast<CUdeviceptr>(row0) * srcTight;
        CUresult r = cuMemcpyHtoDAsync(to, stage, static_cast<std::size_t>(rows) * srcTight, slot.stream);
        if (r != CUDA_SUCCESS) {
            error = cudaText("upload DMA", r);
            return false;
        }
        job.inFlight = true;
        r = cuEventRecord(slot.band[k], slot.stream);
        if (r != CUDA_SUCCESS) {
            error = cudaText("cuEventRecord", r);
            return false;
        }
    }

    // ---- the launch -----------------------------------------------------------------
    // Rows ascend in device memory as the image DESCENDS (bottom row first),
    // which the descriptor states with flipY and a pointer at the bottom row -
    // exactly how buildParams() describes the OpenFX image on the CPU.
    OsvReframeParams p = params;
    OsvRgbaSource s{};
    s.w = w;
    s.h = h;
    s.pitchBytes = static_cast<int>(srcTight);
    s.isHalf = 0;
    s.isBgra = source.order == HostOrder::Bgra ? 1 : 0;
    s.flipY = 1;
    const void* row0 = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(slot.source));
    void* dst = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot.output));
    OsvOfxPack pack = job.plan.pack;
    void* args[] = {&p, &s, &row0, &dst, &pack};
    const CUfunction kernel = static_cast<CUfunction>(source.depth == HostDepth::Byte ? kernels.viewByte
                                                                                       : kernels.viewFloat);
    // No need to wait for the kernel here: readBack() waits for the bands,
    // which the stream orders behind it.
    return job.launchAndQueue(kernel, args, /*waitForKernel=*/false, error);
}

bool FrameJob::readBack(ThreadPool* pool, std::string& error) noexcept {
    if (!m_impl) {
        error = "out of memory for a GPU frame job";
        return false;
    }
    Impl& job = *m_impl;
    if (!job.planned) {
        error = "nothing was framed to read back";
        return false;
    }
    job.planned = false;
    if (job.plan.empty) {
        return true;  // the window missed the image: nothing to write, nothing written
    }
    if (!job.slot || !job.pool) {
        error = "the GPU slot is gone";
        return false;
    }
    ContextPush push(job.pool->context());
    if (!push.ok()) {
        error = "cannot make the GPU device's primary context current";
        return false;
    }
    Slot& slot = *job.slot;
    const HostImageView& image = job.target.image;
    const Plan& plan = job.plan;
    const std::uint32_t areaH = static_cast<std::uint32_t>(plan.area.y2 - plan.area.y1);

    // ---- the pipeline ---------------------------------------------------------------
    // Wait for band b, copy its rows into the host image, then refill its
    // pinned band with band b + 2: the copy of one band hides the DMA of the
    // next.
    for (std::uint32_t b = 0; b < plan.bands; ++b) {
        const int k = static_cast<int>(b % static_cast<std::uint32_t>(kStageBands));
        if (b >= job.issued) {
            // Never happens (phase 1 queues the first bands, this loop the
            // rest), but a band that was never queued must not be waited for.
            error = "readback band " + std::to_string(b) + " was never queued";
            return false;
        }
        const CUresult waited = cuEventSynchronize(slot.band[k]);
        if (waited != CUDA_SUCCESS) {
            error = cudaText("readback wait", waited);
            return false;
        }
        const std::uint32_t row0 = b * plan.rowsPerBand;
        const std::uint32_t rows = std::min(plan.rowsPerBand, areaH - row0);
        const char* stage = static_cast<const char*>(slot.stage[k]);
        // Packed row r is host row area.y1 + r (packed row 0 is the bottom),
        // written at the image's own pitch - padded or negative alike.
        copyRows(rows, plan.packedPitch, pool, [&](std::size_t i) {
            const int y = plan.area.y1 + static_cast<int>(row0) + static_cast<int>(i);
            std::memcpy(image.pixel(plan.area.x1, y), stage + i * plan.packedPitch, plan.packedPitch);
        });
        const std::uint32_t next = b + static_cast<std::uint32_t>(kStageBands);
        if (next < plan.bands && !job.issueBand(next, error)) {
            return false;
        }
    }
    // Every band was waited for, and each came after the kernel: the stream
    // is idle again.
    job.inFlight = false;
    return true;
}

// ---------------------------------------------------------------------------
//  Pools
// ---------------------------------------------------------------------------

bool pipelineBuilt() noexcept { return true; }

int devicePoolSlots(int ordinal) noexcept {
    Registry* reg = registry();
    if (!reg) {
        return 0;
    }
    std::shared_ptr<DevicePool> pool;
    {
        std::lock_guard<std::mutex> lock(reg->mutex);
        if (auto it = reg->pools.find(ordinal); it != reg->pools.end()) {
            pool = it->second;
        }
    }
    return pool ? pool->slotCount() : 0;
}

void releaseDevicePools() noexcept {
    Registry* reg = registry();
    if (!reg) {
        return;
    }
    // Moved out under the lock, destroyed outside it: a pool's destructor
    // waits for its streams and takes the module cache's lock.
    std::map<int, std::shared_ptr<DevicePool>> doomed;
    {
        std::lock_guard<std::mutex> lock(reg->mutex);
        doomed.swap(reg->pools);
    }
    if (!doomed.empty()) {
        PluginLog::info("ofx gpu: releasing {} device pool(s)", doomed.size());
    }
    doomed.clear();
}

#else  // !OSV_OFX_HAVE_CUDA
// ===========================================================================
//  No CUDA kernel in this build: the same API, always refusing
// ===========================================================================

struct FrameJob::Impl {};

namespace {
constexpr const char* kNoCuda = "this build of OpenOSV.ofx has no CUDA kernel";
}  // namespace

FrameJob::FrameJob() noexcept = default;
FrameJob::~FrameJob() = default;

int FrameJob::device() const noexcept { return -1; }

bool FrameJob::reserve(int, std::string& error) noexcept {
    error = kNoCuda;
    return false;
}

bool FrameJob::frameDevice(const OsvReframeParams&, const DeviceRgba&, const HostTarget&,
                           std::string& error) noexcept {
    error = kNoCuda;
    return false;
}

bool FrameJob::packDevice(const DeviceRgba&, const HostTarget&, std::string& error) noexcept {
    error = kNoCuda;
    return false;
}

bool FrameJob::frameHost(const OsvReframeParams&, const HostImageView&, const HostTarget&, int, ThreadPool*,
                         std::string& error) noexcept {
    error = kNoCuda;
    return false;
}

bool FrameJob::readBack(ThreadPool*, std::string& error) noexcept {
    error = kNoCuda;
    return false;
}

bool pipelineBuilt() noexcept { return false; }

int devicePoolSlots(int) noexcept { return 0; }

void releaseDevicePools() noexcept {}

#endif  // OSV_OFX_HAVE_CUDA

// ---------------------------------------------------------------------------
//  Whole frames
// ---------------------------------------------------------------------------

bool frameHostImage(const OsvReframeParams& params, const HostImageView& source, const HostTarget& target,
                    int ordinal, ThreadPool* pool, std::string& error) noexcept {
    FrameJob job;
    return job.frameHost(params, source, target, ordinal, pool, error) && job.readBack(pool, error);
}

bool frameDeviceImage(const OsvReframeParams& params, const DeviceRgba& source, const HostTarget& target,
                      ThreadPool* pool, std::string& error) noexcept {
    FrameJob job;
    return job.frameDevice(params, source, target, error) && job.readBack(pool, error);
}

bool packDeviceImage(const DeviceRgba& image, const HostTarget& target, ThreadPool* pool,
                     std::string& error) noexcept {
    FrameJob job;
    return job.packDevice(image, target, error) && job.readBack(pool, error);
}

}  // namespace osv::ofx::gpu
