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
                                 const float* __restrict__ warp, const float* __restrict__ blendSeam,
                                 const float* __restrict__ photo, const float* __restrict__ seamLow,
                                 float* __restrict__ out, int outPitchFloats) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= params.outW || y >= params.outH) {
        return;
    }
    float rgba[4];
    // [WP-SEAM] blendSeam is null unless a carved seam was built;
    // [WP-PHOTO] photo is null unless a photometric seam field was built;
    // [WP-SEAMTOOLS] seamLow is null unless the seam smoothing is on.
    osvShadePixelWSPL(&params, planes.p, seam, warp, blendSeam, photo, seamLow, x, y, rgba);
    float* dst = out + static_cast<size_t>(y) * outPitchFloats + static_cast<size_t>(x) * 4;
    dst[0] = rgba[0];
    dst[1] = rgba[1];
    dst[2] = rgba[2];
    dst[3] = rgba[3];
}

/// One thread per output pixel of the equirect reframe path.  The source is
/// an already stitched RGBA panorama; the shared function does the letterbox,
/// projection, rotation and bilinear lookup.
__global__ void osvReframeEquirectKernel(const __grid_constant__ OsvReframeParams params,
                                         const __grid_constant__ OsvRgbaSource src, const void* __restrict__ pixels,
                                         float* __restrict__ out, int outPitchFloats) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= params.outW || y >= params.outH) {
        return;
    }
    float rgba[4];
    osvReframeEquirectPixel(&params, &src, pixels, x, y, rgba);
    float* dst = out + static_cast<size_t>(y) * outPitchFloats + static_cast<size_t>(x) * 4;
    dst[0] = rgba[0];
    dst[1] = rgba[1];
    dst[2] = rgba[2];
    dst[3] = rgba[3];
}

/// [WP-SEAMTOOLS] Seam low band, stage 1: one thread per low-band texel,
/// blockIdx.z = the lens.  The shared osvSeamLowDecimatePixel does the work.
__global__ void osvSeamLowDecimateKernel(const __grid_constant__ OsvRenderParams params,
                                         const __grid_constant__ OsvPlanePair planes, float* __restrict__ dst) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int lens = static_cast<int>(blockIdx.z);
    if (x >= params.seamLowW || y >= params.seamLowH || lens > 1) {
        return;
    }
    float* texel = dst + ((static_cast<size_t>(lens) * params.seamLowH + y) * params.seamLowW + x) * 4;
    osvSeamLowDecimatePixel(&params, &planes.p[lens], lens, x, y, texel);
}

/// [WP-SEAMTOOLS] Seam low band, stages 2 and 3: one separable blur pass.
__global__ void osvSeamLowBlurKernel(const __grid_constant__ OsvRenderParams params, const float* __restrict__ src,
                                     float* __restrict__ dst, int horizontal) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int lens = static_cast<int>(blockIdx.z);
    if (x >= params.seamLowW || y >= params.seamLowH || lens > 1) {
        return;
    }
    float* texel = dst + ((static_cast<size_t>(lens) * params.seamLowH + y) * params.seamLowW + x) * 4;
    osvSeamLowBlurPixel(&params, src, lens, x, y, horizontal, texel);
}

}  // namespace

cudaError_t osvCudaLaunchReframe(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                                 const float* warp, const float* blendSeam, const float* photo, const float* seamLow,
                                 float* out, int outPitchFloats, cudaStream_t stream) {
    // 16x16 threads per block covers the image with edge guards in the kernel.
    const dim3 block(16, 16);
    const dim3 grid((static_cast<unsigned>(params.outW) + block.x - 1) / block.x,
                    (static_cast<unsigned>(params.outH) + block.y - 1) / block.y);
    osvReframeKernel<<<grid, block, 0, stream>>>(params, planes, seam, warp, blendSeam, photo, seamLow, out,
                                                 outPitchFloats);
    return cudaGetLastError();
}

cudaError_t osvCudaBuildSeamLow(const OsvRenderParams& params, const OsvPlanePair& planes, float* dst,
                                float* scratch, cudaStream_t stream) {
    // Refuse degenerate launches up front: a zero-sized grid is a CUDA error
    // and a null buffer would fault inside the kernel.
    if (!dst || !scratch || !params.seamSmoothEnabled || params.seamLowW <= 0 || params.seamLowH <= 0) {
        return cudaErrorInvalidValue;
    }
    const dim3 block(16, 16, 1);
    const dim3 grid((static_cast<unsigned>(params.seamLowW) + block.x - 1) / block.x,
                    (static_cast<unsigned>(params.seamLowH) + block.y - 1) / block.y, 2u);
    // Decimate into dst, blur along x into scratch, along y back into dst:
    // three launches in stream order, no synchronisation in between.
    osvSeamLowDecimateKernel<<<grid, block, 0, stream>>>(params, planes, dst);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        return err;
    }
    osvSeamLowBlurKernel<<<grid, block, 0, stream>>>(params, dst, scratch, 1);
    err = cudaGetLastError();
    if (err != cudaSuccess) {
        return err;
    }
    osvSeamLowBlurKernel<<<grid, block, 0, stream>>>(params, scratch, dst, 0);
    return cudaGetLastError();
}

cudaError_t osvCudaLaunchReframeEquirect(const OsvReframeParams& params, const OsvRgbaSource& src, const void* pixels,
                                         float* out, int outPitchFloats, cudaStream_t stream) {
    // Refuse degenerate launches up front: a zero-sized grid is a CUDA error
    // and a null buffer would fault inside the kernel.
    if (!pixels || !out || params.outW <= 0 || params.outH <= 0 || outPitchFloats <= 0) {
        return cudaErrorInvalidValue;
    }
    const dim3 block(16, 16);
    const dim3 grid((static_cast<unsigned>(params.outW) + block.x - 1) / block.x,
                    (static_cast<unsigned>(params.outH) + block.y - 1) / block.y);
    osvReframeEquirectKernel<<<grid, block, 0, stream>>>(params, src, pixels, out, outPitchFloats);
    return cudaGetLastError();
}

}  // namespace osv::render
