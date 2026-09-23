// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlareLaunch.h - interface between the MSVC-compiled host code of the GPU
// flare sampler (FlareCuda.cpp) and its nvcc-compiled kernel
// (FlareKernel.cu).
//
// The same split as CudaAnalysisLaunch.h and for the same reason: the kernel
// side sees only plain structs, raw pointers and cudaError_t, and everything
// that carries an error message lives on the MSVC side.  The launcher is
// asynchronous on the stream it is given, allocates nothing, selects no
// device and synchronises nothing.

#pragma once

#include "osv/render/osv_kernel.h"

#include <cuda_runtime.h>

namespace osv::render::gpu {

/// Downsample one lens plane (DEVICE pointers) into `out`, a tightly packed
/// outW x outH interleaved RGB float image in device memory, running the
/// shared osvFlareDownsamplePixel for every output pixel - the exact
/// function the CPU reference runs, so both backends produce the same
/// working image.
cudaError_t launchFlareDownsample(const OsvPlane& plane, const OsvColorParams& color, int factor, int outW,
                                  int outH, float* out, cudaStream_t stream);

/// True when this build's fatbin holds flare code the CURRENT device can run
/// (probed with cudaFuncGetAttributes, which loads the module without a
/// launch).  On failure `why` receives the runtime's error.
bool flareKernelLoadable(cudaError_t* why);

}  // namespace osv::render::gpu
