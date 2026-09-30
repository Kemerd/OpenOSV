// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxReframeKernel.cu - OpenOSV 360 Reframe on DaVinci Resolve's CUDA images.
//
// Compiled to a fatbin and embedded in OpenOSV.ofx (plugins/ofx/CMakeLists.txt),
// exactly like the Premiere effect's ReframeKernel.cu: the module imports
// nothing from the CUDA runtime, loads the kernel into whatever context the
// host has current, and launches it with the driver API (OfxCuda.cpp).
//
// The per-pixel work is osvReframeEquirectPixel() from osv_kernel.h - the one
// function the CPU path, the Premiere GPU path and this kernel all call - so
// the only things this file decides are WHERE a pixel is read and written:
//
//   * OpenFX images put y UP: an image's data pointer addresses its
//     bottom-left pixel and row y (host pixel coordinates) lives at
//     data + (y - bounds.y1) * rowBytes.  That is the same for CPU and CUDA
//     images (ofxImageEffect.h, kOfxImagePropData).
//   * The camera counts rows DOWN from the top of the frame it fills.  So a
//     host pixel (x, y) is camera pixel (x - frameX1, frameY2 - 1 - y).
//   * The source is described by its OsvRgbaSource, built on the host by
//     reframe::buildParams(): flipY = 1 and a pointer at the bottom row for an
//     OpenFX image, isBgra = 0 for OpenFX's R, G, B, A order.
//
// Channels are written in the order the sampler returns them - straight RGBA
// - so no swizzle happens anywhere on this path.
//
// [WP-V-GPU] The same fatbin carries the own-GPU path's kernels (the end of
// this file): the camera and the sphere copy for hosts that hand CPU images,
// levelled and packed into the host's pixel format before the readback.

#include "osv/render/osv_kernel.h"

// The argument block, shared with the launcher so both sides agree on it.
#include "OfxKernelAbi.h"

extern "C" __global__ void osvOfxReframeKernel(OsvReframeParams params, OsvRgbaSource source,
                                               const unsigned char* __restrict__ sourceRow0,
                                               unsigned char* __restrict__ dstData, OsvOfxTarget target) {
    // One thread per pixel of the render window; the grid is rounded up to
    // whole blocks, so the tail threads have nothing to do.
    const int wx = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int wy = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (wx >= target.windowW || wy >= target.windowH) {
        return;
    }

    // Host pixel coordinates (y up), then the camera's (y down).
    const int x = target.windowX1 + wx;
    const int y = target.windowY1 + wy;
    const int camX = x - target.frameX1;
    const int camY = target.frameY2 - 1 - y;

    // Pixels outside the camera frame (a render window wider than the RoD)
    // come back transparent black from the kernel itself.
    float rgba[4];
    osvReframeEquirectPixel(&params, &source, sourceRow0, camX, camY, rgba);

    // Signed 64-bit offsets: a negative pitch is legal in OpenFX, and an
    // unsigned multiply would turn it into a wild address.
    const long long rowOffset = (long long)(y - target.boundsY1) * (long long)target.rowBytes;
    float* texel = (float*)(dstData + rowOffset) + (long long)(x - target.boundsX1) * 4;
    texel[0] = rgba[0];
    texel[1] = rgba[1];
    texel[2] = rgba[2];
    texel[3] = rgba[3];
}

// ===========================================================================
//  [WP-V-GPU] The own-GPU path: frame, level and pack for a CPU-image host
// ===========================================================================
//
// VEGAS Pro hands every image in host memory, as 8-bit or float, R,G,B,A or
// B,G,R,A, in full or studio levels.  The kernels below produce exactly the
// bytes the host image must end up holding - framing, levels and the pack
// into the host's format all happen here - so the plug-in only streams the
// finished rectangle back (OfxGpuPipeline.cpp).  OfxKernelAbi.h describes
// the arguments; the rectangle and frame arithmetic is the reframe kernel's
// above, only the output side differs.
//
// Numerics: the fatbin is built with -fmad=false (cmake/OsvCudaFatbin.cmake),
// and the levels and quantisation below spell their roundings out with
// __fmul_rn / __fadd_rn on top of that, so they reproduce OfxHostImage.h's
// studioFromFull() and toByte() - two separately rounded operations each -
// whatever the compiler would otherwise contract.

/// studioFromFull() / applyLevels() of OfxHostImage.h: black + value * span,
/// multiply and add rounded separately, exactly like the host.  Linear, so
/// float values outside [0, 1] keep their meaning.
__device__ __forceinline__ float osvOfxLevel(float value, const OsvOfxPack& pack) {
    return pack.studio ? __fadd_rn(pack.studioBlack, __fmul_rn(value, pack.studioSpan)) : value;
}

/// toByte() of OfxHostImage.h: clamp to [0, 1], then value * 255 + 0.5
/// truncated.  NaN fails the first comparison and becomes 0, as on the host.
__device__ __forceinline__ unsigned char osvOfxToByte(float value) {
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    return (unsigned char)__fadd_rn(__fmul_rn(value, 255.0f), 0.5f);
}

/// Store one straight R,G,B,A quadruple as packed pixel `wx` of `row`: RGB
/// levelled, alpha untouched, channels in the host's order, then 8-bit codes
/// or floats - OfxHostImage.h's storeHostPixel(), step for step.
__device__ __forceinline__ void osvOfxStorePixel(unsigned char* row, int wx, float r, float g, float b, float a,
                                                 const OsvOfxPack& pack) {
    r = osvOfxLevel(r, pack);
    g = osvOfxLevel(g, pack);
    b = osvOfxLevel(b, pack);
    // The first and third stored channels swap for B,G,R,A.
    const float c0 = pack.isBgra ? b : r;
    const float c2 = pack.isBgra ? r : b;
    if (pack.isByte) {
        uchar4 o;
        o.x = osvOfxToByte(c0);
        o.y = osvOfxToByte(g);
        o.z = osvOfxToByte(c2);
        o.w = osvOfxToByte(a);
        reinterpret_cast<uchar4*>(row)[wx] = o;
    } else {
        reinterpret_cast<float4*>(row)[wx] = make_float4(c0, g, c2, a);
    }
}

/// Transparent black for a rectangle pixel that lies outside the camera
/// frame (a render window wider than the frame).  Like every other pixel it
/// is (0, 0, 0, 0) BEFORE levels - OfxHostImage.h's storeHostPixel() rule,
/// which the CPU paths' clear follows too - so studio levels store RGB at
/// studio black (code 16) with alpha 0.
__device__ __forceinline__ void osvOfxStoreClear(unsigned char* row, int wx, const OsvOfxPack& pack) {
    osvOfxStorePixel(row, wx, 0.0f, 0.0f, 0.0f, 0.0f, pack);
}

/// One thread's pixel of the rectangle: its packed coordinates, its host
/// coordinates, and whether the camera frame covers it.  False for the tail
/// threads of a grid rounded up to whole blocks.
struct OsvOfxSite {
    int wx;          ///< Packed column.
    int wy;          ///< Packed row (0 = the rectangle's bottom row).
    int camX;        ///< Camera column (x - frameX1).
    int camY;        ///< Camera row, counted DOWN from the frame's top.
    bool inFrame;    ///< The camera frame covers this host pixel.
    unsigned char* row;  ///< Start of packed row wy.
};

__device__ __forceinline__ bool osvOfxSite(const OsvOfxPack& pack, unsigned char* dst, OsvOfxSite& site) {
    site.wx = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    site.wy = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (site.wx >= pack.windowW || site.wy >= pack.windowH) {
        return false;
    }
    // Host pixel (y up); packed row 0 is the rectangle's bottom row.
    const int x = pack.windowX1 + site.wx;
    const int y = pack.windowY1 + site.wy;
    site.inFrame = x >= pack.frameX1 && x < pack.frameX2 && y >= pack.frameY1 && y < pack.frameY2;
    // The camera counts rows DOWN from the top of the frame it fills.
    site.camX = x - pack.frameX1;
    site.camY = pack.frameY2 - 1 - y;
    // 64-bit row offset: a tall 4K float rectangle is > 2 GB of addressing.
    site.row = dst + (long long)site.wy * (long long)pack.dstPitchBytes;
    return true;
}

// ---------------------------------------------------------------------------
//  An 8-bit source, sampled as its float promotion would be
// ---------------------------------------------------------------------------
// The shared sampler (osvFetchRgba) reads float or half only.  The CPU paths
// promote an 8-bit frame to float first (reframe::promoteIntegerToFloat:
// code * (1/255), per texel, before any interpolation) and then call
// osvReframeEquirectPixel().  Promoting on the GPU would cost a float copy of
// the whole source in VRAM - 472 MB for an 8K panorama - and four times the
// memory traffic of every fetch, so the three functions below are
// osvFetchRgba / osvSampleEquirectRgba / osvReframeEquirectPixel of
// osv_kernel.h with the texel read swapped for a byte read scaled by the SAME
// factor.  Everything after the fetch - the bilinear weights, the order of
// every operation - is theirs line for line, so the result is bit for bit
// the promoted frame's.  Keep them in step with osv_kernel.h.

/// osvFetchRgba() for four 8-bit codes per texel.
__device__ __forceinline__ void osvOfxFetchByte(const OsvRgbaSource* src, const unsigned char* pixels, int x, int y,
                                                float byteScale, float* rgba) {
    // flipY: the pointer is at the last image row and rows ascend in memory
    // as the image descends (an OpenFX image uploaded bottom row first).
    const int memoryRow = src->flipY ? (src->h - 1 - y) : y;
    const unsigned char* texel = pixels + (size_t)memoryRow * (size_t)src->pitchBytes + (size_t)x * 4u;
    // code * (1/255): promoteIntegerToFloat()'s exact operation.
    const float c0 = (float)texel[0] * byteScale;
    const float c1 = (float)texel[1] * byteScale;
    const float c2 = (float)texel[2] * byteScale;
    const float c3 = (float)texel[3] * byteScale;
    if (src->isBgra) {
        rgba[0] = c2;
        rgba[1] = c1;
        rgba[2] = c0;
    } else {
        rgba[0] = c0;
        rgba[1] = c1;
        rgba[2] = c2;
    }
    rgba[3] = c3;
}

/// osvSampleEquirectRgba() over an 8-bit source.
__device__ __forceinline__ void osvOfxSampleEquirectByte(const OsvRgbaSource* src, const unsigned char* pixels,
                                                         const float* dBody, float byteScale, float* rgba) {
    const float W = (float)src->w;
    const float H = (float)src->h;
    // Standard layout: d = (sin lon cos lat, cos lon cos lat, sin lat).
    const float lon = atan2f(dBody[0], dBody[1]);
    const float lat = asinf(osvClampf(dBody[2], -1.0f, 1.0f));
    // Continuous sample coordinates, then the 0.5 centre offset.
    const float fx = (lon / OSV_KERNEL_TWO_PI + 0.5f) * W - 0.5f;
    const float fy = (0.5f - lat / OSV_KERNEL_PI) * H - 0.5f;
    const float flx = floorf(fx);
    const float fly = floorf(fy);
    const float tx = fx - flx;
    const float ty = fy - fly;
    const int x0 = (int)flx;
    const int y0 = (int)fly;
    // Longitude wraps, latitude clamps.
    const int xa = osvWrapIndex(x0, src->w);
    const int xb = osvWrapIndex(x0 + 1, src->w);
    const int ya = y0 < 0 ? 0 : (y0 >= src->h ? src->h - 1 : y0);
    const int yb = (y0 + 1) < 0 ? 0 : ((y0 + 1) >= src->h ? src->h - 1 : y0 + 1);
    float a[4], b[4], c[4], d[4];
    osvOfxFetchByte(src, pixels, xa, ya, byteScale, a);
    osvOfxFetchByte(src, pixels, xb, ya, byteScale, b);
    osvOfxFetchByte(src, pixels, xa, yb, byteScale, c);
    osvOfxFetchByte(src, pixels, xb, yb, byteScale, d);
    for (int i = 0; i < 4; ++i) {
        const float top = a[i] + (b[i] - a[i]) * tx;
        const float bot = c[i] + (d[i] - c[i]) * tx;
        rgba[i] = top + (bot - top) * ty;
    }
}

/// osvReframeEquirectPixel() over an 8-bit source: transparent black outside
/// the viewport, for a pixel with no ray and for every invalid input.
__device__ __forceinline__ void osvOfxReframeBytePixel(const OsvReframeParams* p, const OsvRgbaSource* src,
                                                       const unsigned char* pixels, int px, int py, float byteScale,
                                                       float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!p || !src || !pixels) {
        return;
    }
    if (src->w <= 0 || src->h <= 0 || src->pitchBytes <= 0) {
        return;
    }
    if (p->outW <= 0 || p->outH <= 0 || p->viewW <= 0 || p->viewH <= 0) {
        return;
    }
    if (px < 0 || py < 0 || px >= p->outW || py >= p->outH) {
        return;
    }
    // Letterbox: only the viewport receives the picture.
    const int lx = px - p->viewX;
    const int ly = py - p->viewY;
    if (lx < 0 || ly < 0 || lx >= p->viewW || ly >= p->viewH) {
        return;
    }
    // Centred pixel offsets inside the viewport, +ny = up.
    const float W = (float)p->viewW;
    const float H = (float)p->viewH;
    const float nx = ((float)lx + 0.5f) - 0.5f * W;
    const float ny = 0.5f * H - ((float)ly + 0.5f);
    float dView[3];
    if (!osvViewRay(p->projection, p->focalPx, p->eyeOffset, p->tanHalfH, p->tanHalfV, W, H, nx, ny, dView)) {
        return;
    }
    // View -> body, then look the body direction up in the panorama.
    float dBody[3];
    osvMat3MulVec(p->Rout, dView, dBody);
    osvOfxSampleEquirectByte(src, pixels, dBody, byteScale, out);
    if (p->fillAlphaOne) {
        out[3] = 1.0f;
    }
}

// ---------------------------------------------------------------------------
//  The three kernels
// ---------------------------------------------------------------------------

/// Frame a FLOAT source (the engine's sphere in VRAM, or an uploaded host
/// float image) and pack the rectangle.  `source` / `sourceRow0` describe it
/// exactly as for osvOfxReframeKernel.
extern "C" __global__ void osvOfxViewPackKernel(OsvReframeParams params, OsvRgbaSource source,
                                                const unsigned char* __restrict__ sourceRow0,
                                                unsigned char* __restrict__ dst, OsvOfxPack pack) {
    OsvOfxSite site;
    if (!osvOfxSite(pack, dst, site)) {
        return;
    }
    if (!site.inFrame) {
        osvOfxStoreClear(site.row, site.wx, pack);
        return;
    }
    // The one per-pixel function every other path calls.
    float rgba[4];
    osvReframeEquirectPixel(&params, &source, sourceRow0, site.camX, site.camY, rgba);
    osvOfxStorePixel(site.row, site.wx, rgba[0], rgba[1], rgba[2], rgba[3], pack);
}

/// The same over an 8-bit source (four codes per texel, `source.pitchBytes`
/// apart, R,G,B,A or B,G,R,A per source.isBgra).
extern "C" __global__ void osvOfxViewPackByteKernel(OsvReframeParams params, OsvRgbaSource source,
                                                    const unsigned char* __restrict__ sourceRow0,
                                                    unsigned char* __restrict__ dst, OsvOfxPack pack) {
    OsvOfxSite site;
    if (!osvOfxSite(pack, dst, site)) {
        return;
    }
    if (!site.inFrame) {
        osvOfxStoreClear(site.row, site.wx, pack);
        return;
    }
    float rgba[4];
    osvOfxReframeBytePixel(&params, &source, sourceRow0, site.camX, site.camY, pack.byteScale, rgba);
    osvOfxStorePixel(site.row, site.wx, rgba[0], rgba[1], rgba[2], rgba[3], pack);
}

/// Pack a float R,G,B,A device image that IS the camera frame (`imageW` x
/// `imageH`, top row first, `imagePitch` bytes apart): the sphere the engine
/// rendered at the frame's size for the 360 equirect output.
extern "C" __global__ void osvOfxEquirectPackKernel(const unsigned char* __restrict__ image,
                                                    unsigned long long imagePitch, int imageW, int imageH,
                                                    unsigned char* __restrict__ dst, OsvOfxPack pack) {
    OsvOfxSite site;
    if (!osvOfxSite(pack, dst, site)) {
        return;
    }
    // Outside the frame, or outside an image that does not match the frame
    // (the host code refuses that; this is the belt to its braces).
    if (!site.inFrame || site.camX < 0 || site.camX >= imageW || site.camY < 0 || site.camY >= imageH) {
        osvOfxStoreClear(site.row, site.wx, pack);
        return;
    }
    const float* texel =
        reinterpret_cast<const float*>(image + (unsigned long long)site.camY * imagePitch) + (long long)site.camX * 4;
    osvOfxStorePixel(site.row, site.wx, texel[0], texel[1], texel[2], texel[3], pack);
}
