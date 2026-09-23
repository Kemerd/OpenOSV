// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectLaunch: launching the fused fisheye -> view kernel
// (osvReframeDirectKernel in ReframeKernel.cu) through the CUDA DRIVER API.
//
// The effect module never links the CUDA runtime (see GpuFilter.cpp); the
// kernel lives in the embedded fatbin and is reached with
// cuModuleGetFunction(module, kDirectKernelName).  This header is the one
// place that turns a DirectSetup plus device frames into a cuLaunchKernel
// call, and the one place that refuses to launch when anything about those
// inputs is wrong.
//
// Why the refusals matter more here than anywhere else: a kernel that faults
// in Premiere's CUDA context leaves a STICKY error behind, and every later
// GPU operation in that context - ours and Premiere's own - fails until the
// process restarts.  So a host pointer passed where a device pointer belongs,
// an output frame of the wrong size or a pitch that cannot hold a row is
// caught on the host, named, and never reaches the GPU.
//
// Typical use (the effect's Render callback; the context is the host's and
// must be current on the calling thread):
//
//     CUfunction fn = nullptr;
//     cuModuleGetFunction(&fn, module, kDirectKernelName);        // once per module
//     const DirectSetup setup = buildDirectParams(settings, stitch, dstW, dstH, sequenceSize);
//     OsvPlane planes[2];                                          // from the device frames
//     render::fillDevicePlane(deviceFrame[0], planes[0]);
//     render::fillDevicePlane(deviceFrame[1], planes[1]);
//     DirectOutput out{dstData, dstRowBytes, dstW, dstH, dstIsHalf};
//     const DirectLaunchResult r = launchDirect(fn, hostStream, setup, planes, out);
//     if (!r.ok()) { ...log directLaunchRejectName(r.reject), fall back to the equirect path... }
#pragma once

#include "DirectKernelAbi.h"
#include "DirectRender.h"

#include <cuda.h>

#include <cstdint>

namespace osv::reframe {

/// Name of the direct kernel inside the effect's fatbin, for
/// cuModuleGetFunction.  The same string the kernel is exported under.
inline constexpr const char* kDirectKernelName = OSV_DIRECT_KERNEL_NAME;

/// Premiere's GPU output frame (or any frame in the same layout).
///
/// BGRA, one pixel = four 32-bit floats (PrPixelFormat_GPU_BGRA_4444_32f) or
/// four IEEE binary16 values (PrPixelFormat_GPU_BGRA_4444_16f), top-left
/// origin, positive row pitch - "GPU Frames always have origin top left"
/// (PrSDKGPUDeviceSuite.h).  `data` is a DEVICE address in the context the
/// kernel is launched in.
struct DirectOutput {
    void* data = nullptr;       ///< Device address of pixel (0, 0), the TOP-left corner.
    std::int32_t rowBytes = 0;  ///< Row pitch in bytes; positive, at least width * bytes per pixel.
    int width = 0;              ///< Pixels per row; must equal the setup's outW.
    int height = 0;             ///< Rows; must equal the setup's outH.
    bool isHalf = false;        ///< true = BGRA 16f, false = BGRA 32f.
};

/// Why launchDirect() did not launch (or what the driver said when it did).
enum class DirectLaunchReject {
    None = 0,    ///< Launched; `result` is CUDA_SUCCESS.
    Kernel,      ///< Null CUfunction.
    Setup,       ///< The DirectSetup is not valid.
    Planes,      ///< The plane descriptors fail planesMatch().
    Output,      ///< Null output, wrong size, or a pitch that cannot hold a row.
    Alignment,   ///< The output address or pitch is not aligned for its sample type.
    Context,     ///< No current CUDA context on the calling thread.
    Memory,      ///< A plane, table or output address is not device memory of the current device.
    Driver,      ///< cuLaunchKernel itself failed; see `result`.
};

/// Human-readable name of a launch refusal, for the log line.
[[nodiscard]] const char* directLaunchRejectName(DirectLaunchReject reason) noexcept;

/// The outcome of one launch attempt.
struct DirectLaunchResult {
    DirectLaunchReject reject = DirectLaunchReject::Setup;  ///< `None` exactly when launched.
    CUresult result = CUDA_ERROR_INVALID_VALUE;             ///< The driver's answer (or INVALID_VALUE when refused).

    /// True when the kernel was enqueued.  Asynchronous execution errors
    /// surface later, on the stream, like any other kernel's.
    [[nodiscard]] bool ok() const noexcept { return reject == DirectLaunchReject::None && result == CUDA_SUCCESS; }
};

/// Enqueue the direct kernel on `stream`.
///
/// `kernel`        osvReframeDirectKernel from the effect's fatbin, loaded in
///                 the CURRENT context;
/// `stream`        the stream to order the launch on - Premiere's own, so the
///                 work is sequenced against the host's use of the frames;
///                 nullptr is the context's default stream;
/// `setup`         from buildDirectParams(); its seam/warp/blend-seam
///                 pointers must be DEVICE addresses (or null when the
///                 feature is off);
/// `devicePlanes`  two descriptors ([0] slave, [1] master) whose pointers are
///                 DEVICE addresses - e.g. render::fillDevicePlane() of the
///                 GPU clip decoder's P010 frames;
/// `out`           the output frame (see DirectOutput).
///
/// Validates on the host before anything reaches the GPU: the kernel handle,
/// the setup, the plane descriptors (planesMatch), the output size, pitch and
/// alignment, a current context, and - through cuPointerGetAttribute - that
/// every plane, table and output address really is device memory on the
/// context's device.  Any failure returns a named refusal and launches
/// nothing.  Never throws; never synchronises the stream.
[[nodiscard]] DirectLaunchResult launchDirect(CUfunction kernel, CUstream stream, const DirectSetup& setup,
                                              const OsvPlane* devicePlanes, const DirectOutput& out) noexcept;

}  // namespace osv::reframe
