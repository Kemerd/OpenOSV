// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxCuda.h - the reframe kernel on the host's CUDA images (OpenFX 1.5 CUDA
// render, which DaVinci Resolve offers on NVIDIA machines).
//
// When the host sets kOfxImageEffectPropCudaEnabled on a render, every image
// data pointer is CUDA device memory in the context the host has current on
// the render thread, and kOfxImageEffectPropCudaStream (when present) is the
// stream to queue the work on.  This module:
//
//   * talks to CUDA only through the driver API (nvcuda.dll, delay-loaded),
//     never through a runtime DLL that could collide with the host's own;
//   * loads the embedded fatbin once per CUDA context and keeps it until the
//     module unloads (a host that recreates its context gets a fresh load);
//   * launches asynchronously on the host's stream and returns without
//     waiting, as the specification asks; with no stream it waits for the
//     default stream instead, which is the specification's other branch.
//
// Nothing here reads a pixel on the CPU, and nothing here decides a pixel:
// OfxReframeKernel.cu calls the shared osvReframeEquirectPixel().
#pragma once

#include "OfxKernelAbi.h"

#include "osv/render/osv_kernel.h"

#include <string>

namespace osv::ofx::cuda {

/// True when the NVIDIA driver (nvcuda.dll) is present in the process or on
/// the system, so a driver-API call cannot raise a delay-load exception.
[[nodiscard]] bool driverPresent() noexcept;

/// [WP-V-GPU] driverPresent() AND cuInit() succeeded, so the driver answers
/// every call - a CurrentContextGuard built afterwards records the real
/// current context even on the very first GPU frame of the process.
/// `why` (may be null) receives the reason when false.
[[nodiscard]] bool driverInitialised(std::string* why = nullptr) noexcept;

/// Launch the reframe kernel on the context current on this thread.
///
/// `sourceRow0` / `dstData` are device pointers (the OsvRgbaSource and the
/// target block say how to walk them), `stream` is the host's CUDA stream or
/// null.  Returns false - with `error` saying why - when no context is
/// current, the module cannot be loaded or the launch fails; the caller then
/// reports a failed render rather than handing back an untouched frame.
[[nodiscard]] bool launchReframe(const OsvReframeParams& params, const OsvRgbaSource& source, const void* sourceRow0,
                                 void* dstData, const OsvOfxTarget& target, void* stream, std::string& error) noexcept;

/// Forget every loaded module (called on the last kOfxActionUnload).  The
/// modules themselves are NOT unloaded: by then the host may already have
/// destroyed the contexts they live in, and unloading into a dead context is
/// undefined behaviour.  Their memory goes with the context.
///
/// [WP-V-GPU] It first releases the own-GPU path's device pools
/// (OfxGpuPipeline.h, releaseDevicePools): their streams, pinned bands and
/// device buffers, and the modules loaded into the primary contexts they
/// retain - contexts that are certainly alive, because the pools hold them.
void releaseModules() noexcept;

// ---------------------------------------------------------------------------
//  [WP-V-GPU] The own-GPU path's kernels (OfxKernelAbi.h, OfxGpuPipeline.h)
// ---------------------------------------------------------------------------

/// The fatbin's own-GPU kernels as loaded into one context (CUfunction
/// handles as void*, so this header needs no cuda.h).  All three are set, or
/// none is.
struct OwnKernels {
    void* viewFloat = nullptr;  ///< OSV_OFX_VIEW_KERNEL_NAME.
    void* viewByte = nullptr;   ///< OSV_OFX_VIEW_BYTE_KERNEL_NAME.
    void* equirect = nullptr;   ///< OSV_OFX_EQUIRECT_KERNEL_NAME.
};

/// The own-GPU kernels for `context` (a CUcontext that is CURRENT on this
/// thread), loading the embedded fatbin into it on first use - the same
/// context-id-keyed module cache the reframe kernel uses.  False with
/// `error` set when the context is null, the fatbin is missing or a kernel
/// cannot be found.  Thread-safe.
[[nodiscard]] bool ownKernels(void* context, OwnKernels& out, std::string& error) noexcept;

/// Unload - and forget - every module this cache loaded into `context`.
/// Only for a context the caller keeps alive (a primary context it retains)
/// and has current; used when an own-GPU device pool is released.
void unloadModulesIn(void* context) noexcept;

/// Puts the host's CUDA context back on this thread after our own GPU work.
///
/// The OpenOSV Source generator stitches through the importer's engine,
/// whose CUDA renderer selects its device with the CUDA runtime - and that
/// makes the device's PRIMARY context current on the calling thread.  The
/// calling thread here is one of the host's render threads, which may have
/// the host's own context current; leaving ours in its place would send the
/// host's next CUDA call to the wrong context.  This guard records what was
/// current when it was built and restores it when it goes out of scope.  It
/// does nothing when the NVIDIA driver is not loaded in the process (then
/// there is no host context to protect, and no driver call may be made).
class CurrentContextGuard {
public:
    CurrentContextGuard() noexcept;
    ~CurrentContextGuard();
    CurrentContextGuard(const CurrentContextGuard&) = delete;
    CurrentContextGuard& operator=(const CurrentContextGuard&) = delete;

private:
    void* m_saved = nullptr;  ///< The CUcontext current at construction (may be null).
    bool m_active = false;    ///< True when the driver was queried successfully.
};

}  // namespace osv::ofx::cuda
