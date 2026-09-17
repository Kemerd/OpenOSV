// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeKernel.cu - the GPU wrapper around osvReframeEquirectPixel().
//
// This translation unit is NOT linked into the .aex.  nvcc compiles it to a
// fatbin (PTX for compute_50 plus real cubins for every architecture the
// installed toolkit supports), the build embeds those bytes into a C++ array
// and GpuFilter.cpp hands them to cuModuleLoadFatBinary at run time.  That is
// the "Utilize CUDA Driver API only" route the Premiere SDK guide recommends
// (12.1.2) and it means the .aex imports nothing from the CUDA runtime -
// only nvcuda.dll, which every machine with an NVIDIA driver already has.
//
// The kernel itself does as little as possible.  All the projection maths,
// the equirect sampler, the longitude wrap and the half-float decode live in
// osv_kernel.h and run identically on the CPU; the wrapper only
//
//   * maps a thread to an output pixel,
//   * calls the shared function,
//   * stores the float RGBA result as BGRA 16f or 32f.
//
// Reads of the source are handled by the shared function through
// OsvRgbaSource (isHalf / isBgra), so a 16f input needs no converted copy on
// the device.
//
// Compiled with -fmad=false and without fast math, exactly like the library's
// own CUDA renderer, so the GPU result stays inside the parity budget the
// tests assert (PSNR >= 60 dB for 32f, >= 45 dB for 16f).

#include "osv/render/osv_kernel.h"

// ---------------------------------------------------------------------------
//  Output store
// ---------------------------------------------------------------------------

/// IEEE 754 binary32 -> binary16, round to nearest even.  __float2half_rn()
/// would do this in one instruction, but it lives in <cuda_fp16.h> and would
/// drag a header dependency into a file that must stay compilable by the
/// same rules as the shared kernel.  Integer arithmetic keeps the result bit
/// identical to the CPU path's floatToHalf(), which is what makes the 16f
/// parity test meaningful.
__device__ __forceinline__ unsigned short osvFloatToHalfDevice(float value) {
    OsvFloatBits pun;
    pun.f = value;
    const unsigned int bits = pun.u;
    const unsigned int sign = (bits >> 16) & 0x8000u;
    const unsigned int rawExp = (bits >> 23) & 0xFFu;
    const unsigned int mantissa = bits & 0x7FFFFFu;

    /* Infinity / NaN keep their class; a NaN keeps a non-zero payload. */
    if (rawExp == 0xFFu) {
        return (unsigned short)(sign | (mantissa != 0u ? 0x7E00u : 0x7C00u));
    }

    int exponent = (int)rawExp - 127 + 15;
    if (exponent >= 0x1F) {
        return (unsigned short)(sign | 0x7C00u); /* overflow -> infinity */
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return (unsigned short)sign; /* underflow -> signed zero */
        }
        /* Subnormal: restore the implicit one, then round. */
        const unsigned int m = mantissa | 0x800000u;
        const int shift = 14 - exponent;
        const unsigned int half = m >> shift;
        const unsigned int rem = m & ((1u << shift) - 1u);
        const unsigned int halfway = 1u << (shift - 1);
        unsigned int rounded = half;
        if (rem > halfway || (rem == halfway && (half & 1u) != 0u)) {
            rounded += 1u;
        }
        return (unsigned short)(sign | rounded);
    }

    /* Normal: keep 10 mantissa bits, round to nearest even (a carry into the
     * exponent is exactly the right behaviour). */
    unsigned int half = ((unsigned int)exponent << 10) | (mantissa >> 13);
    const unsigned int rem = mantissa & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u) != 0u)) {
        half += 1u;
    }
    return (unsigned short)(sign | half);
}

// ---------------------------------------------------------------------------
//  The kernel
//
//  extern "C" so the name in the fatbin is exactly "osvReframeEquirectKernel"
//  and cuModuleGetFunction can find it without demangling.
//
//  Parameters are passed by value because a CUcontext-scoped constant buffer
//  would have to be uploaded per launch anyway, and the two structs together
//  are well under the 4 KB parameter limit (OsvReframeParams is 80 bytes,
//  OsvRgbaSource 20).
// ---------------------------------------------------------------------------
extern "C" __global__ void osvReframeEquirectKernel(OsvReframeParams params, OsvRgbaSource source,
                                                    const unsigned char* __restrict__ sourcePixels,
                                                    unsigned char* __restrict__ dstBase, int dstRowBytes,
                                                    int dstIsHalf) {
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    /* Grids are rounded up to whole blocks; the tail threads do nothing. */
    if (x >= params.outW || y >= params.outH) {
        return;
    }

    /* The one shared per-pixel function.  It returns transparent black for
     * every pixel outside the viewport, so the letterbox needs no separate
     * clear pass. */
    float rgba[4];
    osvReframeEquirectPixel(&params, &source, sourcePixels, x, y, rgba);

    /* GPU frames are top-left with a positive pitch (PrSDKGPUDeviceSuite.h:
     * "GPU Frames always have origin top left"), so row y is simply
     * y * rowBytes from the base. */
    unsigned char* row = dstBase + (size_t)y * (size_t)dstRowBytes;
    if (dstIsHalf) {
        unsigned short* texel = (unsigned short*)row + (size_t)x * 4u;
        texel[0] = osvFloatToHalfDevice(rgba[2]); /* B */
        texel[1] = osvFloatToHalfDevice(rgba[1]); /* G */
        texel[2] = osvFloatToHalfDevice(rgba[0]); /* R */
        texel[3] = osvFloatToHalfDevice(rgba[3]); /* A */
    } else {
        float* texel = (float*)row + (size_t)x * 4u;
        texel[0] = rgba[2];
        texel[1] = rgba[1];
        texel[2] = rgba[0];
        texel[3] = rgba[3];
    }
}
