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

/// Launch the reframe kernel asynchronously on `stream`.  `blendSeam` is the
/// device copy of the carved blend-seam table ([WP-SEAM]) or null; `photo`
/// the device copy of the photometric seam table ([WP-PHOTO]) or null.
cudaError_t osvCudaLaunchReframe(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                                 const float* warp, const float* blendSeam, const float* photo, float* out,
                                 int outPitchFloats, cudaStream_t stream);

/// Launch the equirect reframe kernel (osvReframeEquirectPixel) asynchronously
/// on `stream`.  `pixels` is the device copy of the source equirect described
/// by `src`, `out` a device float RGBA buffer of params.outW x params.outH
/// with a row pitch of `outPitchFloats` floats.
cudaError_t osvCudaLaunchReframeEquirect(const OsvReframeParams& params, const OsvRgbaSource& src, const void* pixels,
                                         float* out, int outPitchFloats, cudaStream_t stream);

}  // namespace osv::render
