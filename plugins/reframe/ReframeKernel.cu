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
//
// The fatbin carries TWO kernels:
//
//   osvReframeEquirectKernel  the classic path: sample the view out of the
//                             importer's stitched equirect (above);
//   osvReframeDirectKernel    the direct path: run the stitch shader
//                             osvShadePixelW() in OSV_MODE_REFRAME straight
//                             from the two fisheye frames (P010 in VRAM),
//                             writing Premiere's output frame in one pass.
//                             Parameters come from buildDirectParams()
//                             (DirectRender.h) and the launch from
//                             launchDirect() (DirectLaunch.h).

#include "osv/render/osv_kernel.h"

#include "DirectKernelAbi.h"

// ---------------------------------------------------------------------------
//  Parameter-space access
//
//  osvShadePixelW() takes its parameter block by POINTER and indexes into it
//  with run-time indices (the occlusion polygon loop, the lens loop).  Taking
//  the address of an ordinary by-value kernel parameter makes the compiler
//  copy the whole ~1 KB block into per-thread local memory first - millions of
//  threads times a kilobyte of traffic, several times the cost of the shading
//  itself.  __grid_constant__ (compute capability 7.0+) lets the address point
//  straight into the read-only parameter space instead, which is what the
//  library's own CUDA renderer does.
//
//  The fatbin also carries PTX for compute_50 and cubins for sm_60/61, where
//  the qualifier does not exist; there the parameter keeps the (correct,
//  slower) local copy.  The qualifier changes code generation only, never the
//  parameter's size or layout, so the host launch is identical either way.
// ---------------------------------------------------------------------------
#if defined(__CUDA_ARCH__) && (__CUDA_ARCH__ >= 700)
#define OSV_GRID_CONST __grid_constant__
#else
#define OSV_GRID_CONST
#endif

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

/// Store one straight-RGBA float quadruple as the BGRA texel at column `x`
/// of the output row `row`, in 16f or 32f.  Shared by both kernels so the
/// two paths quantise a 16f frame identically - and identically to the CPU
/// path's storePixel(), whose float -> half conversion is the same algorithm.
__device__ __forceinline__ void osvStoreBgraDevice(unsigned char* row, int x, int isHalf, const float* rgba) {
    if (isHalf) {
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
    osvStoreBgraDevice(row, x, dstIsHalf, rgba);
}

// ---------------------------------------------------------------------------
//  The direct kernel: fisheyes -> view, in one pass
//
//  extern "C" for the same unmangled-name reason as the kernel above; the
//  name and the parameter list are pinned in DirectKernelAbi.h, which the
//  host launcher compiles too.
//
//  Everything that can be validated has been, on the host, before the launch
//  (launchDirect): the parameter block is finite and composed, the planes
//  match the lens blocks, the output is device memory of the right size and
//  alignment.  The kernel therefore only guards the grid tail and runs the
//  shared shader - the same osvShadePixelW() the CPU twin, the library's CPU
//  renderer and its CUDA renderer all run, which is what makes the GPU/CPU
//  parity test meaningful.
//
//  The parameter block is ~1 KB, well inside the 4 KB kernel-parameter limit
//  every architecture in the fatbin supports (OsvRenderParams is asserted
//  <= 4 KB by the library tests; the planes add 112 bytes).
// ---------------------------------------------------------------------------
extern "C" __global__ void osvReframeDirectKernel(OSV_GRID_CONST const OsvRenderParams params,
                                                  OSV_GRID_CONST const OsvDirectPlanes planes,
                                                  const float* __restrict__ seam, const float* __restrict__ warp,
                                                  const float* __restrict__ blendSeam,
                                                  const float* __restrict__ photo,
                                                  const float* __restrict__ seamLow,
                                                  unsigned char* __restrict__ dstBase, int dstRowBytes,
                                                  int dstIsHalf) {
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    /* Grids are rounded up to whole blocks; the tail threads do nothing. */
    if (x >= params.outW || y >= params.outH) {
        return;
    }

    /* The stitch shader in reframe mode: view ray -> body ray (Rout already
     * holds stabilisation * camera) -> both lenses, feather / occlusion
     * blend, seam shift, warp grid, colour pipeline.  Pixels no lens sees
     * come back transparent black, so no clear pass is needed. */
    float rgba[4];
    /* [WP-SEAM] blendSeam is the carved seam table, or NULL without one;
     * [WP-PHOTO] photo is the photometric seam table, or NULL without one;
     * [WP-SEAMTOOLS] seamLow is the engine's seam low band, or NULL. */
    osvShadePixelWSPL(&params, planes.lens, seam, warp, blendSeam, photo, seamLow, x, y, rgba);

    /* Premiere GPU frame: top-left origin, positive pitch. */
    unsigned char* row = dstBase + (size_t)y * (size_t)dstRowBytes;
    osvStoreBgraDevice(row, x, dstIsHalf, rgba);
}
