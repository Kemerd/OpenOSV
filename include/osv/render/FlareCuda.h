// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlareCuda.h - the GPU sampler of the flare analysis (osv_render_cuda).
//
// analyseFlare() (Flare.h) reduces each lens frame to a small native-linear
// RGB working image before it looks for the sun and fits the ghosts.  For a
// frame that lives in VRAM - a keepOnDevice NVDEC decode, the direct GPU
// path - that reduction has to happen on the GPU, and only the working
// image (6.75 MB at the default factor for a 6K lens) comes back.
//
// HOW A CALLER TURNS IT ON
// ------------------------
//   osv::render::installCudaFlareSampler();   // once, at start-up
//
// after which analyseFlare() accepts device-only frame pairs.  Like the other
// GPU analyses (CudaAnalysis.h) it runs in whatever CUDA context is current
// on the calling thread and never selects, resets or synchronises a device;
// only its own stream is waited on.

#pragma once

#include "osv/core/Result.h"
#include "osv/render/Flare.h"

#include <cstdint>
#include <string>

namespace osv::render {

/// True when the flare sampler can run on the calling thread's device.
/// `reason` (optional) receives why not.
[[nodiscard]] bool cudaFlareAvailable(std::string* reason = nullptr);

/// Install the CUDA FlareDeviceSampler into osv_render_cpu (idempotent).
/// Returns Unsupported, installing nothing, when cudaFlareAvailable() is
/// false.
[[nodiscard]] Status installCudaFlareSampler();

/// Remove the sampler installCudaFlareSampler() installed (a different one
/// installed since is left alone).
void uninstallCudaFlareSampler() noexcept;

/// The GPU downsample itself.  `plane` holds DEVICE pointers when
/// `planeOnDevice` is true (the production case); with false, its host
/// planes are uploaded first (tests and tools only - it allocates and frees
/// device memory per call).  `stream` is a cudaStream_t of the current
/// context or nullptr for a pooled one.  Same output as flareDownsample().
[[nodiscard]] Result<FlareImage> cudaFlareDownsample(const OsvPlane& plane, bool planeOnDevice,
                                                     const OsvColorParams& color, std::uint32_t factor,
                                                     void* stream = nullptr);

}  // namespace osv::render
