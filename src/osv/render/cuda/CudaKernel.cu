// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CUDA kernel + launcher.  Deliberately free of any host-side library code:
// nvcc's device code generator has an internal error on the Result/variant
// machinery, so everything except the kernel lives in CudaRenderer.cpp
// (compiled by MSVC) and reaches the kernel through osvCudaLaunchReframe().

#include "CudaLaunch.h"

#include "osv/render/osv_kernel.h"

namespace osv::render {

namespace {

/// One thread per output pixel.  The parameter blocks are grid constants so
/// the compiler can address them directly in constant memory.
__global__ void osvReframeKernel(const __grid_constant__ OsvRenderParams params,
                                 const __grid_constant__ OsvPlanePair planes, const float* __restrict__ seam,
                                 float* __restrict__ out, int outPitchFloats) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= params.outW || y >= params.outH) {
        return;
    }
    float rgba[4];
    osvShadePixel(&params, planes.p, seam, x, y, rgba);
    float* dst = out + static_cast<size_t>(y) * outPitchFloats + static_cast<size_t>(x) * 4;
    dst[0] = rgba[0];
    dst[1] = rgba[1];
    dst[2] = rgba[2];
    dst[3] = rgba[3];
}

}  // namespace

cudaError_t osvCudaLaunchReframe(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                                 float* out, int outPitchFloats, cudaStream_t stream) {
    // 16x16 threads per block covers the image with edge guards in the kernel.
    const dim3 block(16, 16);
    const dim3 grid((static_cast<unsigned>(params.outW) + block.x - 1) / block.x,
                    (static_cast<unsigned>(params.outH) + block.y - 1) / block.y);
    osvReframeKernel<<<grid, block, 0, stream>>>(params, planes, seam, out, outPitchFloats);
    return cudaGetLastError();
}

}  // namespace osv::render
