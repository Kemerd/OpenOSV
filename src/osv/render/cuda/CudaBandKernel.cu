// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaBandKernel.cu - shade the rows of a polar-axis band on the GPU.
//
// The band analyses (seam search, gain estimate, parallax measurement) need
// a thin strip of rows around the equator of a polar-axis equirect, one lens
// at a time.  SeamAnalysis.cpp shades those rows on the CPU with
// osvShadePixelW; these kernels run the SAME shared function on the GPU, with
// the same absolute row index, for frames that live in VRAM.  Everything the
// kernel computes per pixel is therefore exactly what the CPU computes, to
// the few-ulp agreement the renderer parity tests already demand
// (-fmad=false, no fast-math).
//
// Deliberately free of host library code - see CudaLaunch.h.

#include "CudaAnalysisLaunch.h"

#include "osv/render/osv_kernel.h"

namespace osv::render::gpu {

namespace {

/// Threads per block for the band kernels: 64 columns x 4 rows keeps a warp
/// on one row (coalesced stores) and still fills the SMs for a band only a
/// few dozen rows tall.
constexpr unsigned kBandBlockX = 64;
constexpr unsigned kBandBlockY = 4;

/// One thread per band pixel, full RGBA out.
__global__ void osvBandRgbaKernel(const __grid_constant__ OsvRenderParams params,
                                  const __grid_constant__ OsvPlanePair planes, const float* __restrict__ seam,
                                  const float* __restrict__ warp, int row0, int rows, float* __restrict__ out) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int r = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= params.outW || r >= rows) {
        return;
    }
    // Shade at the ABSOLUTE map row, exactly as the CPU band path does, so
    // the band pixel equals the matching pixel of a full-map render.
    float rgba[4];
    osvShadePixelW(&params, planes.p, seam, warp, x, row0 + r, rgba);
    float* dst = out + (static_cast<size_t>(r) * static_cast<size_t>(params.outW) + static_cast<size_t>(x)) * 4u;
    dst[0] = rgba[0];
    dst[1] = rgba[1];
    dst[2] = rgba[2];
    dst[3] = rgba[3];
}

/// One thread per band pixel, reduced to luma + coverage on the spot.
__global__ void osvBandLumaAlphaKernel(const __grid_constant__ OsvRenderParams params,
                                       const __grid_constant__ OsvPlanePair planes, const float* __restrict__ seam,
                                       const float* __restrict__ warp, int row0, int rows, float* __restrict__ luma,
                                       float* __restrict__ alpha) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int r = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= params.outW || r >= rows) {
        return;
    }
    float rgba[4];
    osvShadePixelW(&params, planes.p, seam, warp, x, row0 + r, rgba);
    const size_t i = static_cast<size_t>(r) * static_cast<size_t>(params.outW) + static_cast<size_t>(x);
    // The same expression, in the same order, as SeamAnalysis.cpp's lumaOf():
    // (0.2627 R + 0.6780 G) + 0.0593 B, no contraction (-fmad=false), so a
    // given RGBA gives the same luma on both backends.
    luma[i] = 0.2627f * rgba[0] + 0.6780f * rgba[1] + 0.0593f * rgba[2];
    alpha[i] = rgba[3];
}

/// Grid covering outW columns x `rows` rows.
dim3 bandGrid(int outW, int rows) {
    return dim3((static_cast<unsigned>(outW) + kBandBlockX - 1u) / kBandBlockX,
                (static_cast<unsigned>(rows) + kBandBlockY - 1u) / kBandBlockY);
}

}  // namespace

cudaError_t launchBandRgba(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                           const float* warp, int row0, int rows, float* out, cudaStream_t stream) {
    // A zero-sized grid is itself a launch error and a null output would
    // fault on the device; refuse both here with a clean status.
    if (out == nullptr || params.outW <= 0 || rows <= 0 || row0 < 0 || row0 + rows > params.outH) {
        return cudaErrorInvalidValue;
    }
    osvBandRgbaKernel<<<bandGrid(params.outW, rows), dim3(kBandBlockX, kBandBlockY), 0, stream>>>(
        params, planes, seam, warp, row0, rows, out);
    return cudaGetLastError();
}

cudaError_t launchBandLumaAlpha(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                                const float* warp, int row0, int rows, float* luma, float* alpha,
                                cudaStream_t stream) {
    if (luma == nullptr || alpha == nullptr || params.outW <= 0 || rows <= 0 || row0 < 0 ||
        row0 + rows > params.outH) {
        return cudaErrorInvalidValue;
    }
    osvBandLumaAlphaKernel<<<bandGrid(params.outW, rows), dim3(kBandBlockX, kBandBlockY), 0, stream>>>(
        params, planes, seam, warp, row0, rows, luma, alpha);
    return cudaGetLastError();
}

}  // namespace osv::render::gpu
