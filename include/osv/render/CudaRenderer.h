// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaRenderer: executes osv_kernel.h on an NVIDIA GPU through the CUDA
// runtime.  Input planes are uploaded into pitched device buffers (or used in
// place when the decoder left them on the device), the parameter block travels
// by value as a __grid_constant__ kernel argument (no mutable global device
// state, so several renderers can coexist in one process), and the float RGBA
// result is read back through pinned host memory.
//
// Numerical policy: compiled with -fmad=false and without fast-math so the
// result matches the CPU reference renderer to a few ulp (see docs).
#pragma once

#include "osv/render/Renderer.h"

#include <cstddef>
#include <memory>
#include <string>

namespace osv::render {

class CudaRenderer final : public IRenderer {
public:
    ~CudaRenderer() override;

    /// Create a renderer on CUDA device `deviceIndex`.  Fails with Gpu when no
    /// usable device exists (the reason is in the error message).
    static Result<std::unique_ptr<CudaRenderer>> create(int deviceIndex = 0);

    /// True when at least one CUDA device with compute capability >= 5.0 is
    /// present and the driver is new enough for the runtime.  `reason` (may
    /// be null) receives a short explanation when false.
    static bool available(std::string* reason);

    /// Number of CUDA devices (0 when the runtime cannot initialise).
    static int deviceCount();

    /// Human readable name of a device ("NVIDIA GeForce RTX 5090 (sm_120)").
    static std::string deviceName(int deviceIndex);

    Result<ImageRGBAf> render(const RenderJob& job) override;
    /// Reuses `out`'s allocation across same-sized frames (IRenderer::renderInto).
    Status renderInto(const RenderJob& job, ImageRGBAf& out) override;
    [[nodiscard]] const char* name() const noexcept override { return "cuda"; }

    /// Render and leave the result on the device.  Returns the float4 buffer
    /// (row pitch in bytes via `pitchBytes`); the memory belongs to the
    /// renderer and stays valid until the next render call or destruction.
    /// This is the entry point the Premiere effect will use.
    Result<void*> renderToDevice(const RenderJob& job, std::size_t* pitchBytes);

    /// CUDA device ordinal this renderer runs on.
    [[nodiscard]] int deviceIndex() const noexcept;

private:
    CudaRenderer();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Run the equirect reframe entry point (osvReframeEquirectPixel) on CUDA
/// device `deviceIndex` for a host-resident source frame: uploads `pixels`
/// (src.h rows of src.pitchBytes bytes), launches the kernel and reads the
/// float RGBA result back.  Synchronous; every device allocation is released
/// before returning.  This is the verification path that proves the device
/// build of the shared function agrees with the CPU one; the Premiere effect
/// runs the same kernel on the host's own buffers.
Result<ImageRGBAf> cudaReframeEquirect(int deviceIndex, const OsvReframeParams& params, const OsvRgbaSource& src,
                                       const void* pixels);

/// [WP-SEAMTOOLS] Build the seam smoothing's two-lens low band (SeamTools.h)
/// from DEVICE planes in the CUDA context current on this thread, on
/// `stream` (a cudaStream_t / CUstream of that context, or null for its
/// default stream), WITHOUT synchronising: whoever reads `dst` must order
/// its work after `stream`.
///
/// `dst` and `scratch` are device buffers of seamLowTableFloats(params)
/// floats each in that context; `planes` the two lens descriptors of
/// `params` (render::fillDevicePlane).  This is the direct path's build: the
/// importer's engine calls it in the effect's context, on the effect's
/// stream.  InvalidArgument for smoothing that is off, malformed fields or a
/// null buffer; Gpu for a launch the driver refused.
[[nodiscard]] Status cudaBuildSeamLowBand(const OsvRenderParams& params, const OsvPlane planes[2], float* dst,
                                          float* scratch, void* stream);

}  // namespace osv::render
