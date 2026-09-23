// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterPackKernel.cu - the device half of ImporterPackKernel.h.
//
// One thread per pixel: read the renderer's float4 (R,G,B,A), quantise each
// channel exactly as PixelCopy's host functions do, store B,G,R,A as one
// 8-byte (16u) or 4-byte (8u) vector.  Rows keep their top-down order; the
// host flips them into Premiere's bottom-left origin while it copies each
// pinned band into the PPix, which costs nothing extra there.
//
// Compiled with the project's CUDA flags (-fmad=false, no fast math): the
// multiply and the add below must round separately, as they do on the host.

#include "ImporterPackKernel.h"

#include <cuda_runtime.h>

namespace osv::premiere {

namespace {

/// White of Premiere's 16-bit integer formats (PixelCopy.h, kWhite16u).
constexpr float kWhite16uF = 32768.0f;

/// PixelCopy::floatTo16u, on the device.  NaN fails the first comparison
/// and lands on black; the format has no super-white, so >= 1 is white.
__device__ __forceinline__ unsigned short to16u(float v) {
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return 32768;
    }
    // __fmul_rn / __fadd_rn pin round-to-nearest multiply and add as two
    // separate operations regardless of any contraction setting.
    return static_cast<unsigned short>(__fadd_rn(__fmul_rn(v, kWhite16uF), 0.5f));
}

/// PixelCopy::floatTo8u, on the device.
__device__ __forceinline__ unsigned char to8u(float v) {
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    return static_cast<unsigned char>(__fadd_rn(__fmul_rn(v, 255.0f), 0.5f));
}

/// RGBA float4 -> BGRA ushort4 (0..32768).
__global__ void packBgra16uKernel(const unsigned char* __restrict__ src, size_t srcPitch,
                                  unsigned char* __restrict__ dst, size_t dstPitch, unsigned width,
                                  unsigned height) {
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    const float4 p = reinterpret_cast<const float4*>(src + static_cast<size_t>(y) * srcPitch)[x];
    ushort4 o;
    o.x = to16u(p.z);  // B
    o.y = to16u(p.y);  // G
    o.z = to16u(p.x);  // R
    o.w = to16u(p.w);  // A
    reinterpret_cast<ushort4*>(dst + static_cast<size_t>(y) * dstPitch)[x] = o;
}

/// RGBA float4 -> BGRA uchar4 (0..255).
__global__ void packBgra8uKernel(const unsigned char* __restrict__ src, size_t srcPitch,
                                 unsigned char* __restrict__ dst, size_t dstPitch, unsigned width, unsigned height) {
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    const float4 p = reinterpret_cast<const float4*>(src + static_cast<size_t>(y) * srcPitch)[x];
    uchar4 o;
    o.x = to8u(p.z);  // B
    o.y = to8u(p.y);  // G
    o.z = to8u(p.x);  // R
    o.w = to8u(p.w);  // A
    reinterpret_cast<uchar4*>(dst + static_cast<size_t>(y) * dstPitch)[x] = o;
}

}  // namespace

int launchPackRgbaToBgra(const void* srcDevice, std::size_t srcPitchBytes, void* dstDevice, std::size_t dstPitchBytes,
                         std::uint32_t width, std::uint32_t height, PackLayout layout, void* stream) noexcept {
    // ---- refuse anything the kernel would fault on -------------------------
    const std::size_t outBpp = packedBytesPerPixel(layout);
    if (!srcDevice || !dstDevice || outBpp == 0 || width == 0 || height == 0 || width > 32768u || height > 32768u) {
        return static_cast<int>(cudaErrorInvalidValue);
    }
    // The float4 loads need 16-byte aligned rows, the stores 8 / 4 bytes.
    if (srcPitchBytes < static_cast<std::size_t>(width) * 16u || srcPitchBytes % 16u != 0 ||
        reinterpret_cast<std::uintptr_t>(srcDevice) % 16u != 0) {
        return static_cast<int>(cudaErrorInvalidValue);
    }
    if (dstPitchBytes < static_cast<std::size_t>(width) * outBpp || dstPitchBytes % outBpp != 0 ||
        reinterpret_cast<std::uintptr_t>(dstDevice) % outBpp != 0) {
        return static_cast<int>(cudaErrorInvalidValue);
    }

    // ---- launch ----------------------------------------------------------------
    // 32 x 8: a warp reads 32 consecutive float4 (512 contiguous bytes) and
    // writes 32 consecutive packed pixels, so both sides coalesce.
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1u) / block.x, (height + block.y - 1u) / block.y);
    const auto s = static_cast<cudaStream_t>(stream);
    const auto* src = static_cast<const unsigned char*>(srcDevice);
    auto* dst = static_cast<unsigned char*>(dstDevice);
    if (layout == PackLayout::Bgra16u) {
        packBgra16uKernel<<<grid, block, 0, s>>>(src, srcPitchBytes, dst, dstPitchBytes, width, height);
    } else {
        packBgra8uKernel<<<grid, block, 0, s>>>(src, srcPitchBytes, dst, dstPitchBytes, width, height);
    }
    return static_cast<int>(cudaGetLastError());
}

const char* packErrorText(int code) noexcept {
    const char* text = cudaGetErrorString(static_cast<cudaError_t>(code));
    return text ? text : "unknown CUDA error";
}

}  // namespace osv::premiere
