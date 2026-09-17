// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Interface between the MSVC-compiled CUDA host code and the nvcc-compiled
// kernel translation unit.
#pragma once

#include "osv/render/osv_kernel.h"

#include <cuda_runtime.h>

namespace osv::render {

/// Both plane descriptors in one by-value kernel argument.
struct OsvPlanePair {
    OsvPlane p[2];
};

/// Launch the reframe kernel asynchronously on `stream`.
cudaError_t osvCudaLaunchReframe(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                                 float* out, int outPitchFloats, cudaStream_t stream);

}  // namespace osv::render
