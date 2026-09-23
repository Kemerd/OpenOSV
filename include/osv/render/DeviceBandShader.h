// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DeviceBandShader.h - the seam through which the band analyses reach a GPU.
//
// WHY THIS EXISTS
// ---------------
// Seam search, gain estimation and the parallax measurement all start the
// same way: shade a thin band of rows around the equator of a polar-axis
// equirect, one lens at a time (SeamAnalysis.cpp, shadeRows).  That shading
// runs osvShadePixelW on the CPU, which is fine for frames in host memory and
// impossible for frames the decoder left in VRAM (RenderJob::planesOnDevice):
// their plane pointers are device addresses, and reading them from the CPU is
// an access violation.
//
// The direct GPU pipeline (docs/DIRECT_GPU.md, WP-B) keeps every frame in
// VRAM, so the band analyses need a GPU shading path.  That path lives in
// osv_render_cuda, and the library dependency runs osv_render_cpu ->
// osv_render_cuda: the CPU library cannot call CUDA, and making it do so
// would drag the CUDA runtime into every CPU-only build.  So the CPU library
// defines this interface, the CUDA library implements it and installs it
// (installCudaAnalyses() in CudaAnalysis.h), and shadeRows() hands every
// device-resident job to whatever is installed.
//
// HOST FRAMES ARE UNTOUCHED
// -------------------------
// A job whose planes are in host memory never reaches this interface: the
// CPU path is bit-identical to what it was, whether or not a shader is
// installed.  Only jobs that the CPU could not read at all take the GPU path,
// so installing a shader can only turn a refusal into a result.

#pragma once

#include "osv/core/Result.h"
#include "osv/render/RenderJob.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace osv::render {

/// Shades rows of a polar-axis band from device-resident frames.
///
/// Implementations must be thread-safe: the importer shades bands on its
/// render threads and a background worker at the same time.  They must run
/// in whatever GPU context is current on the calling thread (inside Premiere
/// that is the host's own CUDA context, the one the frames were decoded in),
/// and they return only once the results are in the caller's host buffers.
class DeviceBandShader {
public:
    virtual ~DeviceBandShader() = default;

    DeviceBandShader(const DeviceBandShader&) = delete;
    DeviceBandShader& operator=(const DeviceBandShader&) = delete;

    /// Short name for logs ("cuda").  Never null.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Shade rows [row0, row1) of `job` and return them as tightly packed
    /// RGBA floats, (row1 - row0) * outW * 4 values - exactly what the CPU
    /// shadeRows() returns for a host job.
    ///
    /// `job` must have device planes (planesOnDevice) and be valid(); the
    /// seam table and warp grid it carries in host memory are uploaded by
    /// the implementation.  Errors: InvalidArgument for a malformed job or
    /// row range, Gpu for a device failure.
    [[nodiscard]] virtual Result<std::vector<float>> shadeRowsRgba(const RenderJob& job, std::uint32_t row0,
                                                                   std::uint32_t row1) = 0;

    /// Shade rows [row0, row1) of `job` and reduce each pixel on the device
    /// to the two planes renderLensBands() keeps: luma (BT.2020 weights, the
    /// same expression as the CPU path) and coverage (alpha).
    ///
    /// Only the two planes cross the bus - a quarter of the RGBA traffic -
    /// which is what makes a per-frame parallax measurement from VRAM cheap.
    /// `luma` and `alpha` are resized to (row1 - row0) * outW.
    [[nodiscard]] virtual Status shadeRowsLumaAlpha(const RenderJob& job, std::uint32_t row0, std::uint32_t row1,
                                                    std::vector<float>& luma, std::vector<float>& alpha) = 0;

protected:
    DeviceBandShader() = default;
};

/// Install (or, with nullptr, remove) the process-wide device band shader.
///
/// Thread-safe.  Calls already in flight keep the shader they started with
/// (they hold a shared_ptr to it), so replacing or removing it never pulls an
/// object out from under a running analysis.
void setDeviceBandShader(std::shared_ptr<DeviceBandShader> shader) noexcept;

/// The installed device band shader, or nullptr when none is.
[[nodiscard]] std::shared_ptr<DeviceBandShader> deviceBandShader() noexcept;

}  // namespace osv::render
