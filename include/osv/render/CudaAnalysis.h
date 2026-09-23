// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaAnalysis.h - the stitching analyses on the GPU (osv_render_cuda).
//
// WHAT THIS PROVIDES
// ------------------
// docs/DIRECT_GPU.md, work package B.  Two things, both built so that a frame
// decoded into VRAM never has to come back to the host to be analysed:
//
//   1. Band shading.  Seam search, gain estimation and the parallax
//      measurement all start by shading a thin band of rows around the
//      equator of a polar-axis equirect (SeamAnalysis.cpp).  For frames in
//      VRAM that now runs on the GPU - the same shared osvShadePixelW - and
//      only the small luma / coverage planes are downloaded.
//
//   2. Dense Inverse Search in CUDA: DisFlow.cpp's solver (Kroeger et al.,
//      ECCV 2016, with the constants DJI's stitcher uses) ported stage by stage, with
//      the same accumulation order and precision, exposed as the flow backend
//      FlowBackendKind::ClassicalCuda so computeFlow() / parallaxFromBands()
//      use it unchanged.
//
// HOW A CALLER TURNS IT ON
// ------------------------
//   osv::render::installCudaAnalyses();              // once, at start-up
//
// After that, any FramePair whose frames are device-only (a keepOnDevice
// NVDEC decode, or WP-A's GpuClipDecoder) works with renderLensBands,
// searchSeam, estimateGain, overlapNcc and measureParallaxBands - the band
// rows are shaded on the GPU automatically - and setting
//
//   ParallaxWarpParams::backend = FlowBackendKind::ClassicalCuda;
//
// runs the flow solve on the GPU too.  Frames in host memory keep the CPU
// band path, bit for bit as before.  Without the install, device frames are
// refused with a message that says why, and ClassicalCuda reports itself
// unavailable so computeFlow() falls back to the CPU solver.
//
// CONTEXTS AND STREAMS
// --------------------
// Everything here runs in the CUDA context current on the calling thread -
// inside Premiere, the host's own - and never selects a device, resets one or
// synchronises one.  Work is ordered on a stream (the caller's when given,
// otherwise one owned by a pooled per-context workspace) and only that stream
// is waited on.  See src/osv/render/cuda/CudaWorkspace.h.
//
// NUMERICS
// --------
// Built with -fmad=false and without fast-math, like the renderer.  The DIS
// port keeps the CPU's double-precision accumulations and summation order,
// so its field matches the CPU solver's to the last bit on the inputs
// measured (the tests assert the tolerance the design doc sets, and report
// the actual difference).  Band shading matches the CPU band path to the
// renderer's own GPU/CPU parity (PSNR >= 60 dB after 16-bit quantisation).

#pragma once

#include "osv/core/Result.h"
#include "osv/render/DisFlow.h"
#include "osv/render/RenderJob.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace osv::render {

/// True when the GPU analyses can run on the calling thread: a CUDA device
/// is visible and this build's kernels can execute on the device behind the
/// current context.  `reason` (optional) receives why not.  Cheap enough to
/// call before every use - FlowBackend::isAvailable() does exactly that,
/// because a GPU can be lost at runtime.
[[nodiscard]] bool cudaAnalysesAvailable(std::string* reason = nullptr);

/// Install the GPU band shader (setDeviceBandShader) and the ClassicalCuda
/// flow backend factory (setCudaFlowBackendFactory) into osv_render_cpu.
///
/// Idempotent and thread-safe.  Returns Unsupported (installing nothing)
/// when cudaAnalysesAvailable() is false, so a machine without a usable GPU
/// keeps its existing behaviour exactly.
[[nodiscard]] Status installCudaAnalyses();

/// Remove what installCudaAnalyses() installed.  Calls already running keep
/// the objects they started with.
void uninstallCudaAnalyses() noexcept;

/// True when installCudaAnalyses() is in effect (both hooks point here).
[[nodiscard]] bool cudaAnalysesInstalled() noexcept;

// ---------------------------------------------------------------------------
//  Dense Inverse Search
// ---------------------------------------------------------------------------

/// Bidirectional DIS on the GPU from two host images - what the
/// ClassicalCuda backend runs.  Same contract as disFlowBidirectional():
/// the same parameters, the same InvalidArgument cases, the same output
/// layout, consistency mask and count.
///
/// Additional refusals, both Unsupported so computeFlow() falls back to the
/// CPU solver: a patch side above 16, and a smoothing sigma whose Gaussian
/// radius exceeds 64 taps.  `stream` is a cudaStream_t / CUstream of the
/// current context, or nullptr for a pooled one; returns once the result is
/// in host memory.
[[nodiscard]] Result<BidirFlow> cudaDisFlowBidirectional(const GrayImage& a, const GrayImage& b,
                                                         const DisFlowParams& params, void* stream = nullptr);

/// The same solve from DEVICE images: two w x h float planes in the current
/// context, rows `pitchBytes` apart (a multiple of 4, at least 4 w).  The
/// images are only read.  This is the entry point for a caller that already
/// has its bands in VRAM and wants to skip the host round trip.
[[nodiscard]] Result<BidirFlow> cudaDisFlowBidirectionalDevice(const float* a, const float* b, std::uint32_t w,
                                                               std::uint32_t h, std::size_t pitchBytes,
                                                               const DisFlowParams& params, void* stream = nullptr);

// ---------------------------------------------------------------------------
//  Band shading
// ---------------------------------------------------------------------------

/// Shade rows [row0, row1) of a device-resident job into tightly packed RGBA
/// floats, (row1 - row0) * outW * 4 values - the GPU twin of the CPU band
/// path's shadeRows().  Both lenses' planes must be device pointers
/// (planesOnDevice) on the device behind the current context.
[[nodiscard]] Result<std::vector<float>> cudaShadeBandRgba(const RenderJob& job, std::uint32_t row0,
                                                           std::uint32_t row1, void* stream = nullptr);

/// Shade the same rows and reduce them on the device to luma (the CPU band
/// path's exact BT.2020 expression) and coverage; only those two planes are
/// downloaded.  `luma` / `alpha` are resized to (row1 - row0) * outW.
[[nodiscard]] Status cudaShadeBandLumaAlpha(const RenderJob& job, std::uint32_t row0, std::uint32_t row1,
                                            std::vector<float>& luma, std::vector<float>& alpha,
                                            void* stream = nullptr);

}  // namespace osv::render
