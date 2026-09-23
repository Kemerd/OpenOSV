// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaWorkspace.h - scratch memory and streams for the GPU analyses, bound to
// whatever CUDA context the caller has current.
//
// THE CONTEXT RULE
// ----------------
// Inside Premiere these analyses run on the host's own CUDA context, with the
// frames decoded into that context (docs/DIRECT_GPU.md, WP-A).  Device
// allocations and streams belong to the context they were created in, so this
// code must never pick a device or a context of its own: no cudaSetDevice, no
// cudaDeviceReset, no cudaDeviceSynchronize.  It asks the driver which context
// is current on the calling thread and uses exactly that one.  When nothing is
// current (a test, osvtool) the runtime binds its primary context to the
// thread, as any runtime call would.
//
// WHY A POOL OF WORKSPACES
// ------------------------
// A parallax measurement needs ~20 buffers (pyramids, gradients, patches, two
// flow fields, staging) and allocating them per call would cost more than the
// arithmetic - cudaMalloc/cudaFree are slow and cudaFree synchronises the
// whole device, Premiere's queued work included.  So each workspace keeps its
// buffers and only ever grows them.  Several threads may analyse at once (the
// importer's render threads and its background worker), so there is a small
// pool per context: a caller leases one, uses it on its own stream, and gives
// it back.  At most kMaxWorkspacesPerContext exist per context; a caller past
// that waits for one to come back rather than growing VRAM without bound.
//
// LIFETIME
// --------
// The pool is created on first use and intentionally never destroyed, like
// FlowBackendOnnx.cpp's engine cache: tearing CUDA objects down from static
// destructors - after the runtime may have unloaded, or inside a host's
// DllMain - is a well-known way to hang or crash a process on its way out,
// and the OS reclaims the memory at exit anyway.

#pragma once

#include "osv/core/Result.h"

#include <cuda.h>
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace osv::render::gpu {

/// Format a runtime error for a Result message: "what: text (code)".
[[nodiscard]] std::string cudaMessage(const char* what, cudaError_t err);

/// Most workspaces one context may own at once.  Four covers the importer
/// (a couple of render threads plus the parallax worker) with room to spare;
/// each is ~20 MB at the default band size.
inline constexpr int kMaxWorkspacesPerContext = 4;

/// A device allocation that only ever grows.
///
/// Growth frees the old block (cudaFree - which synchronises the device, so
/// it is kept to the first calls, when the band size is first seen) and
/// allocates the new one in the CURRENT context, which the pool guarantees is
/// the workspace's own.
class DeviceBlock {
public:
    DeviceBlock() = default;
    ~DeviceBlock();
    DeviceBlock(const DeviceBlock&) = delete;
    DeviceBlock& operator=(const DeviceBlock&) = delete;

    /// Make at least `bytes` available; contents are NOT preserved on growth.
    [[nodiscard]] Status ensure(std::size_t bytes, const char* what);

    [[nodiscard]] void* get() const noexcept { return m_ptr; }
    template <class T>
    [[nodiscard]] T* as() const noexcept {
        return static_cast<T*>(m_ptr);
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_bytes; }

private:
    void* m_ptr = nullptr;
    std::size_t m_bytes = 0;
};

/// A page-locked host allocation that only ever grows, used to stage every
/// upload and download so the copies are true DMA and genuinely asynchronous
/// on the workspace's stream.  Allocated PORTABLE, so it is valid in every
/// context of the process.
class PinnedBlock {
public:
    PinnedBlock() = default;
    ~PinnedBlock();
    PinnedBlock(const PinnedBlock&) = delete;
    PinnedBlock& operator=(const PinnedBlock&) = delete;

    /// Make at least `bytes` available; contents are NOT preserved on growth.
    [[nodiscard]] Status ensure(std::size_t bytes, const char* what);

    [[nodiscard]] void* get() const noexcept { return m_ptr; }
    template <class T>
    [[nodiscard]] T* as() const noexcept {
        return static_cast<T*>(m_ptr);
    }

private:
    void* m_ptr = nullptr;
    std::size_t m_bytes = 0;
};

/// Everything one analysis call needs, owned by one context.
///
/// The members are plain buffers named for their use; which ones a call
/// touches is up to the call (the DIS solve and the band shader share
/// nothing but the stream and the staging blocks).
struct Workspace {
    unsigned long long contextId = 0;  ///< cuCtxGetId of the owning context.
    int device = -1;                   ///< Runtime device ordinal of that context.
    int multiprocessors = 0;           ///< SM count of that device (sizes the solve's lane groups).
    cudaStream_t ownStream = nullptr;  ///< Non-blocking stream created in that context.
    bool busy = false;                 ///< Leased right now (guarded by the pool's mutex).

    // ---- Dense Inverse Search -------------------------------------------
    DeviceBlock pyramid[2];   ///< All levels of image A / B, level 0 first, tightly packed.
    DeviceBlock blurTmp[2];   ///< Horizontal-pass output during pyramid construction.
    DeviceBlock blurred[2];   ///< Pre-blurred level awaiting decimation.
    DeviceBlock gradX[2];     ///< Gradients of the current level of A / B.
    DeviceBlock gradY[2];
    DeviceBlock patches[2];   ///< Patch grid of direction 0 (A -> B) / 1 (B -> A).
    DeviceBlock flowU[2];     ///< Current field of direction 0 / 1.
    DeviceBlock flowV[2];
    DeviceBlock flowTmp[4];   ///< Horizontal-pass output while smoothing (u0, v0, u1, v1).
    DeviceBlock okMask;       ///< Consistency mask, one byte per pixel.
    DeviceBlock okCount;      ///< One 64-bit counter.

    // ---- band shading ----------------------------------------------------
    DeviceBlock bandOut0;     ///< RGBA rows, or luma.
    DeviceBlock bandOut1;     ///< Alpha (luma/alpha mode only).
    DeviceBlock seamTable;    ///< Uploaded copy of RenderJob::seamShiftDeg.
    DeviceBlock warpGrid;     ///< Uploaded copy of RenderJob::warpGrid.

    // ---- staging -----------------------------------------------------------
    PinnedBlock up;           ///< Host -> device staging.
    PinnedBlock down;         ///< Device -> host staging.
};

/// Exclusive use of one workspace for the duration of a call.
///
/// Move-only.  The destructor synchronises the lease's stream (only that
/// stream) and returns the workspace to the pool, so the next lessee never
/// races in-flight work on the same buffers - including after an early error
/// return that skipped the caller's own synchronisation.
class WorkspaceLease {
public:
    WorkspaceLease() = default;
    WorkspaceLease(Workspace* ws, cudaStream_t stream) noexcept : m_ws(ws), m_stream(stream) {}
    ~WorkspaceLease();
    WorkspaceLease(WorkspaceLease&& other) noexcept;
    WorkspaceLease& operator=(WorkspaceLease&& other) noexcept;
    WorkspaceLease(const WorkspaceLease&) = delete;
    WorkspaceLease& operator=(const WorkspaceLease&) = delete;

    [[nodiscard]] Workspace& ws() const noexcept { return *m_ws; }
    /// The stream this call orders its work on: the caller's, when one was
    /// passed to acquireWorkspace(), otherwise the workspace's own.
    [[nodiscard]] cudaStream_t stream() const noexcept { return m_stream; }
    [[nodiscard]] bool valid() const noexcept { return m_ws != nullptr; }

private:
    void release() noexcept;

    Workspace* m_ws = nullptr;
    cudaStream_t m_stream = nullptr;
};

/// Lease a workspace owned by the context current on this thread, creating
/// one (and its stream) if the context has fewer than
/// kMaxWorkspacesPerContext, otherwise waiting for one to be returned.
///
/// `externalStream` (a cudaStream_t / CUstream of the current context, or
/// nullptr) is the stream the lessee must order its work on; nullptr selects
/// the workspace's own stream.  Errors: Gpu when no context can be made
/// current or the driver refuses a query.
[[nodiscard]] Result<WorkspaceLease> acquireWorkspace(void* externalStream);

/// The runtime ordinal of the device behind the current context, binding the
/// runtime's primary context first if nothing is current.  Never selects a
/// device itself.
[[nodiscard]] Result<int> currentDevice();

}  // namespace osv::render::gpu
