// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaWorkspace.cpp - the per-context workspace pool behind the GPU analyses.
// See CudaWorkspace.h for the context rule and the lifetime policy.

#include "CudaWorkspace.h"

#include "osv/core/Log.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace osv::render::gpu {

namespace {

/// Allocation granularity for growth.  Rounding every request up to a whole
/// MiB means a band that is a few rows taller next time reuses the block
/// instead of paying another cudaFree + cudaMalloc (and the device-wide
/// synchronisation cudaFree implies).
constexpr std::size_t kGrowQuantum = std::size_t{1} << 20;

/// `bytes` rounded up to the growth quantum, saturating instead of wrapping.
[[nodiscard]] std::size_t roundUp(std::size_t bytes) noexcept {
    const std::size_t rem = bytes % kGrowQuantum;
    if (rem == 0) {
        return bytes;
    }
    const std::size_t pad = kGrowQuantum - rem;
    return bytes > static_cast<std::size_t>(-1) - pad ? bytes : bytes + pad;
}

/// Driver error text for a Result message.
[[nodiscard]] std::string driverMessage(const char* what, CUresult res) {
    const char* name = nullptr;
    const char* text = nullptr;
    cuGetErrorName(res, &name);
    cuGetErrorString(res, &text);
    return std::string(what) + ": " + (name ? name : "CUDA_ERROR") + " - " + (text ? text : "unknown") + " (" +
           std::to_string(static_cast<int>(res)) + ")";
}

/// The context current on this thread, binding the runtime's primary
/// context when there is none - exactly what the first runtime call on the
/// thread would do anyway, done explicitly so the context can be named.
Result<CUcontext> currentContext() {
    // cuInit is idempotent and cheap after the first call; without it every
    // other driver query fails with CUDA_ERROR_NOT_INITIALIZED.
    CUresult res = cuInit(0);
    if (res != CUDA_SUCCESS) {
        return Error{ErrorCode::Gpu, driverMessage("cuInit", res)};
    }
    CUcontext ctx = nullptr;
    res = cuCtxGetCurrent(&ctx);
    if (res != CUDA_SUCCESS) {
        return Error{ErrorCode::Gpu, driverMessage("cuCtxGetCurrent", res)};
    }
    if (ctx == nullptr) {
        // cudaFree(nullptr) is documented as a no-op; its only effect is the
        // runtime's lazy initialisation, which makes the primary context of
        // the runtime's current device current on this thread.  No device is
        // selected here - that stays the caller's business.
        const cudaError_t err = cudaFree(nullptr);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("CUDA runtime initialisation", err)};
        }
        res = cuCtxGetCurrent(&ctx);
        if (res != CUDA_SUCCESS || ctx == nullptr) {
            return Error{ErrorCode::Gpu, "no CUDA context is current and the runtime could not bind one"};
        }
    }
    return ctx;
}

/// The process-wide pool.  See CudaWorkspace.h, LIFETIME.
class WorkspacePool {
public:
    Result<WorkspaceLease> acquire(unsigned long long contextId, int device, void* externalStream) {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;) {
            // An idle workspace of this context, if there is one.
            int owned = 0;
            for (const std::unique_ptr<Workspace>& ws : m_all) {
                if (ws->contextId != contextId) {
                    continue;
                }
                ++owned;
                if (!ws->busy) {
                    ws->busy = true;
                    return leaseOf(ws.get(), externalStream);
                }
            }
            // Room for another: create it (and its stream) in the current
            // context, which is the one being asked for.
            if (owned < kMaxWorkspacesPerContext) {
                auto ws = std::make_unique<Workspace>();
                ws->contextId = contextId;
                ws->device = device;
                // A performance hint only; a failed query leaves 0, which the
                // solve treats as "size for a small GPU".
                if (cudaDeviceGetAttribute(&ws->multiprocessors, cudaDevAttrMultiProcessorCount, device) !=
                    cudaSuccess) {
                    ws->multiprocessors = 0;
                    (void)cudaGetLastError();
                }
                const cudaError_t err = cudaStreamCreateWithFlags(&ws->ownStream, cudaStreamNonBlocking);
                if (err != cudaSuccess) {
                    return Error{ErrorCode::Gpu, cudaMessage("cudaStreamCreateWithFlags", err)};
                }
                ws->busy = true;
                Workspace* raw = ws.get();
                m_all.push_back(std::move(ws));
                log::debug("gpu analyses: workspace {} created for CUDA context {} (device {})", owned + 1,
                           contextId, device);
                return leaseOf(raw, externalStream);
            }
            // All of this context's workspaces are in use: wait for one.
            // Leases are held only for the duration of one analysis call
            // (about a millisecond), so this is a short queue, not a stall.
            m_returned.wait(lock);
        }
    }

    void release(Workspace* ws) noexcept {
        if (ws == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ws->busy = false;
        }
        m_returned.notify_all();
    }

private:
    static WorkspaceLease leaseOf(Workspace* ws, void* externalStream) {
        cudaStream_t stream = externalStream != nullptr ? static_cast<cudaStream_t>(externalStream) : ws->ownStream;
        return WorkspaceLease(ws, stream);
    }

    std::mutex m_mutex;
    std::condition_variable m_returned;
    std::vector<std::unique_ptr<Workspace>> m_all;
};

WorkspacePool& pool() {
    static WorkspacePool* instance = new WorkspacePool();  // intentionally leaked, see LIFETIME
    return *instance;
}

}  // namespace

std::string cudaMessage(const char* what, cudaError_t err) {
    return std::string(what) + ": " + cudaGetErrorString(err) + " (" + std::to_string(static_cast<int>(err)) + ")";
}

// ---------------------------------------------------------------------------
//  DeviceBlock / PinnedBlock
// ---------------------------------------------------------------------------
DeviceBlock::~DeviceBlock() {
    if (m_ptr != nullptr) {
        cudaFree(m_ptr);
    }
}

Status DeviceBlock::ensure(std::size_t bytes, const char* what) {
    if (bytes == 0) {
        return failStatus(ErrorCode::InvalidArgument, std::string("gpu workspace: zero-sized ") + what);
    }
    if (m_ptr != nullptr && bytes <= m_bytes) {
        return okStatus();
    }
    // Growth: release first so peak usage never holds both blocks.
    if (m_ptr != nullptr) {
        cudaFree(m_ptr);
        m_ptr = nullptr;
        m_bytes = 0;
    }
    const std::size_t want = roundUp(bytes);
    void* ptr = nullptr;
    const cudaError_t err = cudaMalloc(&ptr, want);
    if (err != cudaSuccess || ptr == nullptr) {
        return failStatus(ErrorCode::Gpu, cudaMessage((std::string("cudaMalloc ") + what).c_str(), err));
    }
    m_ptr = ptr;
    m_bytes = want;
    return okStatus();
}

PinnedBlock::~PinnedBlock() {
    if (m_ptr != nullptr) {
        cudaFreeHost(m_ptr);
    }
}

Status PinnedBlock::ensure(std::size_t bytes, const char* what) {
    if (bytes == 0) {
        return failStatus(ErrorCode::InvalidArgument, std::string("gpu workspace: zero-sized ") + what);
    }
    if (m_ptr != nullptr && bytes <= m_bytes) {
        return okStatus();
    }
    if (m_ptr != nullptr) {
        cudaFreeHost(m_ptr);
        m_ptr = nullptr;
        m_bytes = 0;
    }
    const std::size_t want = roundUp(bytes);
    void* ptr = nullptr;
    // Portable: valid in every context of the process, so a workspace's
    // staging never depends on which context happens to be current.
    const cudaError_t err = cudaHostAlloc(&ptr, want, cudaHostAllocPortable);
    if (err != cudaSuccess || ptr == nullptr) {
        return failStatus(ErrorCode::Gpu, cudaMessage((std::string("cudaHostAlloc ") + what).c_str(), err));
    }
    m_ptr = ptr;
    m_bytes = want;
    return okStatus();
}

// ---------------------------------------------------------------------------
//  WorkspaceLease
// ---------------------------------------------------------------------------
WorkspaceLease::~WorkspaceLease() { release(); }

WorkspaceLease::WorkspaceLease(WorkspaceLease&& other) noexcept
    : m_ws(std::exchange(other.m_ws, nullptr)), m_stream(std::exchange(other.m_stream, nullptr)) {}

WorkspaceLease& WorkspaceLease::operator=(WorkspaceLease&& other) noexcept {
    if (this != &other) {
        release();
        m_ws = std::exchange(other.m_ws, nullptr);
        m_stream = std::exchange(other.m_stream, nullptr);
    }
    return *this;
}

void WorkspaceLease::release() noexcept {
    if (m_ws != nullptr) {
        // Drain the stream this lease ordered its work on before anyone else
        // can lease the buffers.  Every entry point already synchronises on
        // its success path, where this costs a few microseconds; it matters
        // on an early error return, which could otherwise leave kernels
        // still writing into a workspace that the next caller - possibly on
        // a different stream - is about to reuse.  Only this stream is
        // waited on, never the device.
        if (m_stream != nullptr) {
            cudaStreamSynchronize(m_stream);
        }
        pool().release(m_ws);
        m_ws = nullptr;
        m_stream = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  Entry points
// ---------------------------------------------------------------------------
Result<int> currentDevice() {
    OSV_TRY_ASSIGN(CUcontext ctx, currentContext());
    (void)ctx;
    // With a context current, the runtime reports that context's device.
    int device = -1;
    const cudaError_t err = cudaGetDevice(&device);
    if (err != cudaSuccess || device < 0) {
        return Error{ErrorCode::Gpu, cudaMessage("cudaGetDevice", err)};
    }
    return device;
}

Result<WorkspaceLease> acquireWorkspace(void* externalStream) {
    OSV_TRY_ASSIGN(CUcontext ctx, currentContext());
    // Key by the context's process-unique ID rather than its handle: a
    // destroyed context's handle value can be reused by a new one, and a
    // workspace must never be handed to a context it was not created in.
    unsigned long long id = 0;
    const CUresult res = cuCtxGetId(ctx, &id);
    if (res != CUDA_SUCCESS) {
        return Error{ErrorCode::Gpu, driverMessage("cuCtxGetId", res)};
    }
    int device = -1;
    const cudaError_t err = cudaGetDevice(&device);
    if (err != cudaSuccess || device < 0) {
        return Error{ErrorCode::Gpu, cudaMessage("cudaGetDevice", err)};
    }
    return pool().acquire(id, device, externalStream);
}

}  // namespace osv::render::gpu
