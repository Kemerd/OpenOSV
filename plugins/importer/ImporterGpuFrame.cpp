// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterGpuFrame.cpp - pinned banded readback, the renderer-output lock and
// the context scope used by the importer's GPU frame path (see the header).

#include "ImporterGpuFrame.h"

#if defined(OSV_IMPORTER_HAVE_PACK_KERNEL)
#include "ImporterPackKernel.h"
#endif
#include "PluginLog.h"

#include <cuda.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>

namespace osv::premiere {

namespace {

using Clock = std::chrono::steady_clock;

/// Milliseconds since `t0`.
[[nodiscard]] double msSince(Clock::time_point t0) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// A CUresult as "what: CUDA_ERROR_NAME".
[[nodiscard]] std::string cudaText(const char* what, CUresult r) {
    const char* name = nullptr;
    (void)cuGetErrorName(r, &name);
    return std::string(what) + ": " + (name ? name : "CUDA_ERROR_UNKNOWN");
}

/// The live readback per device ordinal.  WEAK references: the instances
/// that use a readback own it, so it goes away with the last clip that used
/// it instead of lingering (with 64 MiB of pinned memory) until process exit.
/// The map itself holds no CUDA state, so its static destructor is harmless.
struct ReadbackRegistry {
    std::mutex mutex;
    std::map<int, std::weak_ptr<GpuReadback>> live;
};

[[nodiscard]] ReadbackRegistry& readbackRegistry() noexcept {
    static ReadbackRegistry registry;
    return registry;
}

}  // namespace

// ===========================================================================
//  The renderer-output lock
// ===========================================================================

std::mutex& cudaRendererOutputMutex() noexcept {
    // A function-local static: constructed on first use (thread-safe), and a
    // std::mutex needs no CUDA to be destroyed.
    static std::mutex mutex;
    return mutex;
}

// ===========================================================================
//  CudaContextScope
// ===========================================================================

CudaContextScope::CudaContextScope(void* context) noexcept {
    if (!context) {
        return;
    }
    m_pushed = cuCtxPushCurrent(static_cast<CUcontext>(context)) == CUDA_SUCCESS;
}

CudaContextScope::~CudaContextScope() {
    if (m_pushed) {
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }
}

// ===========================================================================
//  GpuReadback
// ===========================================================================

/// The driver objects.  Everything lives in the retained primary context.
struct GpuReadback::Impl {
    CUdevice device = 0;
    CUcontext context = nullptr;         ///< Retained primary context (released last).
    CUstream stream = nullptr;           ///< Non-blocking: never waits behind the host's own work.
    CUevent ready[kBands] = {};          ///< "band k's DMA has landed".
    void* staging[kBands] = {};          ///< Pinned, portable host bands of kBandBytes.
    /// Device buffer the pack kernel writes 16u / 8u frames into (tight
    /// rows), grown on demand and kept: 144 MB for a 6000 x 3000 16u frame.
    CUdeviceptr packed = 0;
    std::size_t packedBytes = 0;

    /// Make `packed` at least `bytes` large (the context is current).
    [[nodiscard]] CUresult ensurePacked(std::size_t bytes) noexcept {
        if (packed && packedBytes >= bytes) {
            return CUDA_SUCCESS;
        }
        if (packed) {
            // The stream may still be reading the old buffer (a previous
            // failure path synchronised already; this is belt and braces).
            (void)cuStreamSynchronize(stream);
            (void)cuMemFree(packed);
            packed = 0;
            packedBytes = 0;
        }
        const CUresult r = cuMemAlloc(&packed, bytes);
        if (r != CUDA_SUCCESS) {
            packed = 0;
            return r;
        }
        packedBytes = bytes;
        return CUDA_SUCCESS;
    }

    /// Free everything that exists, in reverse order of creation.  The
    /// context is pushed for the frees and released at the very end.
    void destroy() noexcept {
        if (!context) {
            return;
        }
        {
            CudaContextScope scope(context);
            if (stream) {
                // Never free a band a DMA may still be writing into.
                (void)cuStreamSynchronize(stream);
            }
            if (packed) {
                (void)cuMemFree(packed);
                packed = 0;
                packedBytes = 0;
            }
            for (int i = 0; i < kBands; ++i) {
                if (ready[i]) {
                    (void)cuEventDestroy(ready[i]);
                    ready[i] = nullptr;
                }
                if (staging[i]) {
                    (void)cuMemFreeHost(staging[i]);
                    staging[i] = nullptr;
                }
            }
            if (stream) {
                (void)cuStreamDestroy(stream);
                stream = nullptr;
            }
        }
        (void)cuDevicePrimaryCtxRelease(device);
        context = nullptr;
    }
};

Result<std::shared_ptr<GpuReadback>> GpuReadback::acquire(int deviceOrdinal) noexcept {
    if (deviceOrdinal < 0) {
        return Error{ErrorCode::InvalidArgument, "GpuReadback: negative device ordinal"};
    }
    try {
        ReadbackRegistry& registry = readbackRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);

        // ---- an instance that is already alive ---------------------------
        if (auto it = registry.live.find(deviceOrdinal); it != registry.live.end()) {
            if (std::shared_ptr<GpuReadback> existing = it->second.lock()) {
                return existing;
            }
        }

        // ---- a new one ------------------------------------------------------
        // Created under the registry lock: two clips asking at once must not
        // both pin 64 MiB.  The work is a few driver calls (~10-20 ms, most of
        // it pinning), paid once per process (or once per burst of clips).
        CUresult r = cuInit(0);
        if (r != CUDA_SUCCESS) {
            return Error{ErrorCode::Gpu, cudaText("cuInit", r)};
        }
        std::shared_ptr<GpuReadback> made(new GpuReadback());
        made->m_ordinal = deviceOrdinal;
        made->m_impl = std::make_unique<Impl>();
        Impl& impl = *made->m_impl;

        r = cuDeviceGet(&impl.device, deviceOrdinal);
        if (r != CUDA_SUCCESS) {
            return Error{ErrorCode::Gpu, cudaText("cuDeviceGet", r)};
        }
        // The PRIMARY context: the CUDA runtime - and with it the shared
        // CudaRenderer and the analyses - binds to exactly this one, and the
        // renderer's output buffer lives in it.  Retaining leaves its flags
        // as whoever activated it first set them.
        r = cuDevicePrimaryCtxRetain(&impl.context, impl.device);
        if (r != CUDA_SUCCESS) {
            impl.context = nullptr;
            return Error{ErrorCode::Gpu, cudaText("cuDevicePrimaryCtxRetain", r)};
        }

        // Everything else is created with the context pushed.  A failure only
        // records its reason here; the teardown runs after the scope has
        // popped, because it ends by releasing the very context the scope
        // would otherwise still hold on this thread's stack.
        std::string failure;
        {
            CudaContextScope scope(impl.context);
            if (!scope.ok()) {
                failure = "GpuReadback: cannot make the primary context current";
            } else if ((r = cuStreamCreate(&impl.stream, CU_STREAM_NON_BLOCKING)) != CUDA_SUCCESS) {
                impl.stream = nullptr;
                failure = cudaText("cuStreamCreate", r);
            } else {
                for (int i = 0; i < kBands && failure.empty(); ++i) {
                    // Timing disabled: these events only order work, and a
                    // timing event costs more to record.
                    r = cuEventCreate(&impl.ready[i], CU_EVENT_DISABLE_TIMING);
                    if (r != CUDA_SUCCESS) {
                        impl.ready[i] = nullptr;
                        failure = cudaText("cuEventCreate", r);
                        break;
                    }
                    // PORTABLE: the pinning is valid in every context, so the
                    // band stays usable whichever context a caller has current.
                    r = cuMemHostAlloc(&impl.staging[i], kBandBytes, CU_MEMHOSTALLOC_PORTABLE);
                    if (r != CUDA_SUCCESS) {
                        impl.staging[i] = nullptr;
                        failure = cudaText("cuMemHostAlloc", r);
                    }
                }
            }
        }
        if (!failure.empty()) {
            impl.destroy();
            return Error{ErrorCode::Gpu, failure};
        }

        registry.live[deviceOrdinal] = made;
        PluginLog::info("gpu readback: device {} ready ({} pinned bands of {} MiB)", deviceOrdinal, kBands,
                        kBandBytes >> 20);
        return made;
    } catch (const std::exception& e) {
        // Allocation failure of the registry node or the Impl.
        return Error{ErrorCode::Internal, std::string("GpuReadback: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Internal, "GpuReadback: unknown exception"};
    }
}

GpuReadback::~GpuReadback() {
    if (m_impl) {
        m_impl->destroy();
    }
}

void* GpuReadback::context() const noexcept { return m_impl ? static_cast<void*>(m_impl->context) : nullptr; }

Status GpuReadback::copyToHost(const void* deviceRgba, std::size_t devicePitchBytes, std::uint32_t width,
                               std::uint32_t height, const pixelcopy::HostFrame& dst,
                               pixelcopy::HostPixelFormat format, ThreadPool* pool, ReadbackTiming* timing) noexcept {
    const Clock::time_point tStart = Clock::now();
    ReadbackTiming local;

    // ---- arguments ---------------------------------------------------------
    if (!m_impl || !m_impl->context || !m_impl->stream) {
        return failStatus(ErrorCode::Internal, "GpuReadback: used after a failed creation");
    }
    if (!deviceRgba) {
        return failStatus(ErrorCode::InvalidArgument, "GpuReadback: null device frame");
    }
    const std::size_t floatRowBytes = static_cast<std::size_t>(width) * pixelcopy::kBytesPerPixel32f;
    if (width == 0 || height == 0 || width > 32768u || height > 32768u || devicePitchBytes < floatRowBytes) {
        return failStatus(ErrorCode::InvalidArgument, "GpuReadback: bad device frame geometry");
    }
    const std::size_t hostBpp = pixelcopy::bytesPerPixel(format);
    if (dst.width != width || dst.height != height || !dst.valid(hostBpp)) {
        return failStatus(ErrorCode::InvalidArgument, "GpuReadback: host frame does not match the device frame");
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    Impl& impl = *m_impl;

    // ---- what crosses the bus ------------------------------------------------
    // 32f: the renderer's float rows as they are; the host swizzles RGBA ->
    // BGRA while it copies (same bytes either way, so packing on the GPU
    // would only cost a 288 MB device buffer).  16u / 8u: packed on the GPU
    // first, so half or a quarter of the bytes travel and the host only
    // copies rows.
    CUdeviceptr source = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(deviceRgba));
    std::size_t sourcePitch = devicePitchBytes;
    std::size_t rowBytes = floatRowBytes;
    bool packedOnDevice = false;
#if defined(OSV_IMPORTER_HAVE_PACK_KERNEL)
    if (format == pixelcopy::HostPixelFormat::Bgra16u || format == pixelcopy::HostPixelFormat::Bgra8u) {
        const PackLayout layout =
            format == pixelcopy::HostPixelFormat::Bgra16u ? PackLayout::Bgra16u : PackLayout::Bgra8u;
        const std::size_t packedRow = static_cast<std::size_t>(width) * packedBytesPerPixel(layout);
        CUresult r = impl.ensurePacked(packedRow * height);
        if (r != CUDA_SUCCESS) {
            return failStatus(ErrorCode::Gpu, cudaText("pack buffer", r));
        }
        const int launched = launchPackRgbaToBgra(deviceRgba, devicePitchBytes,
                                                  reinterpret_cast<void*>(static_cast<std::uintptr_t>(impl.packed)),
                                                  packedRow, width, height, layout, impl.stream);
        if (launched != 0) {
            (void)cuStreamSynchronize(impl.stream);
            return failStatus(ErrorCode::Gpu, std::string("pack kernel: ") + packErrorText(launched));
        }
        // The DMAs below are on the same stream, so they start only once the
        // pack has written every row.
        source = impl.packed;
        sourcePitch = packedRow;
        rowBytes = packedRow;
        packedOnDevice = true;
    }
#endif

    // A single row that does not fit a band cannot be streamed at all (a
    // 32768-wide float row is 512 KiB, so this never trips in practice).
    const std::uint32_t rowsPerBand =
        static_cast<std::uint32_t>(std::min<std::size_t>(kBandBytes / rowBytes, height));
    if (rowsPerBand == 0) {
        (void)cuStreamSynchronize(impl.stream);
        return failStatus(ErrorCode::InvalidArgument, "GpuReadback: a row is larger than a staging band");
    }
    const std::uint32_t bandCount = (height + rowsPerBand - 1u) / rowsPerBand;

    // ---- one band: enqueue its DMA and the event that says it landed -------
    auto issue = [&](std::uint32_t band) -> CUresult {
        const std::uint32_t row0 = band * rowsPerBand;
        const std::uint32_t rows = std::min(rowsPerBand, height - row0);
        const int slot = static_cast<int>(band % static_cast<std::uint32_t>(kBands));
        CUDA_MEMCPY2D copy{};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = source + static_cast<CUdeviceptr>(row0) * static_cast<CUdeviceptr>(sourcePitch);
        copy.srcPitch = sourcePitch;
        copy.dstMemoryType = CU_MEMORYTYPE_HOST;
        copy.dstHost = impl.staging[slot];
        copy.dstPitch = rowBytes;  // tight rows in the band
        copy.WidthInBytes = rowBytes;
        copy.Height = rows;
        const CUresult r = cuMemcpy2DAsync(&copy, impl.stream);
        if (r != CUDA_SUCCESS) {
            return r;
        }
        return cuEventRecord(impl.ready[slot], impl.stream);
    };

    // ---- the pipeline --------------------------------------------------------
    // Fill both bands, then: wait for band k, convert it, refill its slot
    // with band k + kBands.  The DMA of the band after k runs while k is
    // being converted, so the transfer mostly hides behind work the PPix
    // needs anyway.
    Status result = okStatus();
    for (std::uint32_t b = 0; b < std::min<std::uint32_t>(bandCount, kBands) && result.ok(); ++b) {
        const CUresult r = issue(b);
        if (r != CUDA_SUCCESS) {
            result = failStatus(ErrorCode::Gpu, cudaText("readback enqueue", r));
        }
    }
    for (std::uint32_t band = 0; band < bandCount && result.ok(); ++band) {
        const int slot = static_cast<int>(band % static_cast<std::uint32_t>(kBands));
        const std::uint32_t row0 = band * rowsPerBand;
        const std::uint32_t rows = std::min(rowsPerBand, height - row0);

        // Wait for this band's bytes.
        const Clock::time_point tWait = Clock::now();
        CUresult r = cuEventSynchronize(impl.ready[slot]);
        local.waitMs += msSince(tWait);
        if (r != CUDA_SUCCESS) {
            result = failStatus(ErrorCode::Gpu, cudaText("readback wait", r));
            break;
        }

        // Into the host frame: a flipped row copy of packed rows, or the
        // flip + swizzle (+ quantisation) of float rows.
        const Clock::time_point tConvert = Clock::now();
        if (packedOnDevice) {
            pixelcopy::PackedRows view;
            view.base = impl.staging[slot];
            view.pitchBytes = rowBytes;
            view.width = width;
            view.rows = rows;
            result = pixelcopy::packedRowsToHost(view, row0, dst, format, pool);
        } else {
            pixelcopy::RgbaRows view;
            view.base = static_cast<const float*>(impl.staging[slot]);
            view.pitchBytes = rowBytes;
            view.width = width;
            view.rows = rows;
            result = pixelcopy::rgbaRowsToHost(view, row0, dst, format, pool);
        }
        local.convertMs += msSince(tConvert);
        ++local.bands;
        if (!result.ok()) {
            break;
        }

        // The slot is free again: start the band that will use it next.
        const std::uint32_t next = band + static_cast<std::uint32_t>(kBands);
        if (next < bandCount) {
            r = issue(next);
            if (r != CUDA_SUCCESS) {
                result = failStatus(ErrorCode::Gpu, cudaText("readback enqueue", r));
                break;
            }
        }
    }

    // ---- never return with a DMA still writing into a band ------------------
    // On success everything issued has been waited for already; on failure a
    // band may still be in flight, and the next call would convert (or
    // refill) it underneath the transfer.
    if (!result.ok()) {
        (void)cuStreamSynchronize(impl.stream);
    }
    local.totalMs = msSince(tStart);
    if (timing) {
        *timing = local;
    }
    return result;
}

// ===========================================================================
//  Switches
// ===========================================================================

bool importerGpuDecodeDisabledByEnvironment() noexcept {
    // getenv_s rather than getenv: the importer is /MD and shares the CRT's
    // environment with the host (and with a test that sets the variable).
    char value[8] = {};
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), "OPENOSV_IMPORTER_NO_GPU_DECODE") != 0 || length == 0) {
        return false;
    }
    // Only an explicit yes switches the GPU path off: "1", "true", "yes",
    // "on" (any case).  Anything else - "0", "false", garbage - leaves the
    // fast path on, so a stray value can never silently slow a user down.
    const char c = value[0];
    const char d = value[1];
    return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y' ||
           ((c == 'o' || c == 'O') && (d == 'n' || d == 'N'));
}

}  // namespace osv::premiere
