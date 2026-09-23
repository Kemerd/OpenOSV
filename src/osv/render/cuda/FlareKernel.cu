// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlareKernel.cu - the GPU half of the flare analysis: reduce one lens frame
// in VRAM to the small native-linear RGB working image the host fits the
// ghosts on (osv/render/Flare.h).
//
// A 6K lens frame is 9 MP of 4:2:0 10-bit samples; the working image at the
// default factor 4 is 0.56 MP of RGB floats.  Doing the reduction here means
// only 6.75 MB cross the bus instead of the whole frame, and a decoded frame
// that never left the GPU (the direct path) can be analysed at all.
//
// One thread per output pixel, each running the shared
// osvFlareDownsamplePixel from osv_kernel.h - the same function, the same
// order of operations and (with -fmad=false, no fast-math, as the renderer
// is built) the same rounding as the CPU reference in Flare.cpp.
//
// Deliberately free of host library code - see CudaLaunch.h.

#include "FlareLaunch.h"

#include "osv/render/osv_kernel.h"

namespace osv::render::gpu {

namespace {

/// 32 x 8 threads: a warp spans one output row (coalesced stores), and the
/// factor x factor source blocks of neighbouring threads are adjacent in
/// memory, so the luma loads coalesce too.
constexpr unsigned kFlareBlockX = 32;
constexpr unsigned kFlareBlockY = 8;

/// One thread per working-image pixel.
__global__ void osvFlareDownsampleKernel(const __grid_constant__ OsvPlane plane,
                                         const __grid_constant__ OsvColorParams color, int factor, int outW,
                                         int outH, float* __restrict__ out) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= outW || y >= outH) {
        return;
    }
    float rgb[3];
    osvFlareDownsamplePixel(&plane, &color, factor, x, y, rgb);
    float* dst = out + (static_cast<size_t>(y) * static_cast<size_t>(outW) + static_cast<size_t>(x)) * 3u;
    dst[0] = rgb[0];
    dst[1] = rgb[1];
    dst[2] = rgb[2];
}

}  // namespace

cudaError_t launchFlareDownsample(const OsvPlane& plane, const OsvColorParams& color, int factor, int outW,
                                  int outH, float* out, cudaStream_t stream) {
    // A zero-sized grid is itself a launch error and a null output would
    // fault on the device; refuse both with a clean status.
    if (out == nullptr || outW <= 0 || outH <= 0 || factor < 1 || plane.y == nullptr || plane.u == nullptr ||
        plane.v == nullptr) {
        return cudaErrorInvalidValue;
    }
    const dim3 block(kFlareBlockX, kFlareBlockY);
    const dim3 grid((static_cast<unsigned>(outW) + kFlareBlockX - 1u) / kFlareBlockX,
                    (static_cast<unsigned>(outH) + kFlareBlockY - 1u) / kFlareBlockY);
    osvFlareDownsampleKernel<<<grid, block, 0, stream>>>(plane, color, factor, outW, outH, out);
    return cudaGetLastError();
}

bool flareKernelLoadable(cudaError_t* why) {
    cudaFuncAttributes attr{};
    const cudaError_t err = cudaFuncGetAttributes(&attr, osvFlareDownsampleKernel);
    if (why != nullptr) {
        *why = err;
    }
    return err == cudaSuccess;
}

}  // namespace osv::render::gpu
