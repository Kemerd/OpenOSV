// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeMetal.h - the effect's GPU path on macOS, behind a C++ interface.
//
// Premiere on a Mac renders GPU filters with Metal: PrGPUDeviceInfo carries an
// id<MTLDevice> and an id<MTLCommandQueue>, and a GPU PPix's data is an
// id<MTLBuffer>.  GpuFilter.cpp (plain C++) hands those through here as
// opaque pointers; ReframeMetal.mm (Objective-C++) turns them back into
// Metal objects, keeps one compute pipeline per device, and runs
// osvReframeEquirectKernel (ReframeKernel.metal) - the Metal twin of the
// CUDA kernel, around the same shared osvReframeEquirectPixel().
//
// Nothing here depends on the Adobe SDKs, which is what lets the macOS
// test-suite drive this exact code with Metal objects of its own
// (tests/macos) on machines that have no SDK.
//
// Threading: every function may be called from any thread.  The pipeline
// table is guarded by a mutex; a render encodes into its own command buffer
// on the host's queue.
#pragma once

#include "osv/render/osv_kernel.h"

#include <cstddef>
#include <string>

namespace osv::reframe::metal {

/// One frame to render: the reframe parameters, the source equirect and the
/// output frame, all as Premiere describes them.
struct FrameRequest {
    void* device = nullptr;          ///< id<MTLDevice> (PrGPUDeviceInfo::outDeviceHandle).
    void* queue = nullptr;           ///< id<MTLCommandQueue> (outCommandQueueHandle).
    OsvReframeParams params{};       ///< The view, from buildParams().
    OsvRgbaSource source{};          ///< The source frame's layout, from buildParams().
    void* sourceBuffer = nullptr;    ///< id<MTLBuffer> of the input PPix.
    std::size_t sourceOffset = 0;    ///< Byte offset of source row 0 in it.
    void* outputBuffer = nullptr;    ///< id<MTLBuffer> of the output PPix.
    int outputRowBytes = 0;          ///< Output pitch in bytes (positive, top-down).
    int outputIsHalf = 0;            ///< 1 = BGRA 16f, 0 = BGRA 32f.
    int outputWidth = 0;             ///< Output size in pixels; must equal params.outW / outH.
    int outputHeight = 0;
};

/// Build (or find) the compute pipeline for device slot `deviceIndex` on
/// `device`.  Cheap after the first call for a device; the library comes
/// from the metallib embedded at build time, or from the embedded source
/// compiled with fast math off when there is none.  False with the reason
/// in `error` on any failure.
[[nodiscard]] bool prepareDevice(unsigned deviceIndex, void* device, std::string& error) noexcept;

/// Render one frame and wait for it: the host takes Render's return as the
/// signal that the output is complete (the lesson of the CUDA path, see
/// GpuFilter.cpp).  Validates every field of `request` first.  False with
/// the reason in `error` on any failure; the output is then untrustworthy.
[[nodiscard]] bool renderFrame(unsigned deviceIndex, const FrameRequest& request, std::string& error) noexcept;

/// Drop every cached pipeline (the effect's GPU shutdown).
void releaseDevices() noexcept;

/// True when the library in use came from the build's metallib (diagnostics).
[[nodiscard]] bool usesPrecompiledLibrary() noexcept;

}  // namespace osv::reframe::metal
