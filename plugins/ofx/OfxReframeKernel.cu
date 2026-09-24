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
