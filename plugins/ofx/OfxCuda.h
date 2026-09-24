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
void releaseModules() noexcept;

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
