/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * kernel.metal - last part of the Metal library source: the kernel functions
 * around the shared shader, the Metal twin of opencl/kernel.cl.
 *
 * Buffer layout (MetalRenderer.mm binds exactly this, see kBuf* there):
 *
 *   0         OsvRenderParams              (constant, copied per thread)
 *   1  2  3   lens 0 planes y, u, v         (device ushort; v may alias u)
 *   4         lens 0 descriptor: 8 ints     (w h cw ch strideY strideC bitShift interleaved)
 *   5  6  7   lens 1 planes y, u, v
 *   8         lens 1 descriptor
 *   9         seam table          (always bound; params decide whether it is read)
 *   10        2-D warp grid       (idem)
 *   11        [WP-SEAM] blend seam table
 *   12        [WP-PHOTO] photometric table
 *   13        [WP-SEAMTOOLS] seam low band
 *   14        output RGBA float rows
 *   15        output pitch in floats (int)
 *
 * The shared shader takes its parameter block through a thread pointer
 * (OSV_PRIVATE), the way OpenCL receives a by-value kernel argument, so each
 * kernel copies the block out of constant memory first.
 */

/* Rebuild one lens's OsvPlane from its buffers and descriptor integers.  For
 * interleaved chroma the Cr pointer is Cb + 1, as in the OpenCL wrapper (a
 * buffer cannot be offset by one element when it is bound). */
static inline OsvPlane osvMetalPlane(device const ushort* y, device const ushort* u, device const ushort* v,
                                     constant int* d) {
    OsvPlane p;
    p.y = y;
    p.u = u;
    p.v = d[7] ? (u + 1) : v;
    p.w = d[0];
    p.h = d[1];
    p.cw = d[2];
    p.ch = d[3];
    p.strideY = d[4];
    p.strideC = d[5];
    p.bitShift = d[6];
    p.chromaInterleaved = d[7];
    return p;
}

/* The stitch: one thread per output pixel. */
kernel void osvReframe(constant OsvRenderParams& params [[buffer(0)]],
                       device const ushort* y0 [[buffer(1)]], device const ushort* u0 [[buffer(2)]],
                       device const ushort* v0 [[buffer(3)]], constant int* d0 [[buffer(4)]],
                       device const ushort* y1 [[buffer(5)]], device const ushort* u1 [[buffer(6)]],
                       device const ushort* v1 [[buffer(7)]], constant int* d1 [[buffer(8)]],
                       device const float* seam [[buffer(9)]], device const float* warp [[buffer(10)]],
                       device const float* blendSeam [[buffer(11)]], device const float* photo [[buffer(12)]],
                       device const float* seamLow [[buffer(13)]], device float* out [[buffer(14)]],
                       constant int& outPitchFloats [[buffer(15)]], uint2 gid [[thread_position_in_grid]]) {
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= params.outW || y >= params.outH) {
        return;
    }
    const OsvRenderParams p = params;
    OsvPlane planes[2];
    planes[0] = osvMetalPlane(y0, u0, v0, d0);
    planes[1] = osvMetalPlane(y1, u1, v1, d1);

    float rgba[4];
    /* [WP-SEAM] / [WP-PHOTO] / [WP-SEAMTOOLS]: the tables are always valid
     * buffers; the parameter block decides whether each one is read. */
    osvShadePixelWSPL(&p, planes, seam, warp, blendSeam, photo, seamLow, x, y, rgba);
    device float* dst = out + (size_t)y * (size_t)outPitchFloats + (size_t)x * 4;
    dst[0] = rgba[0];
    dst[1] = rgba[1];
    dst[2] = rgba[2];
    dst[3] = rgba[3];
}

/* [WP-SEAMTOOLS] Seam low band, stage 1: one thread per low-band texel,
 * grid z = the lens.  Same plane buffers as osvReframe; output in buffer 9. */
kernel void osvSeamLowDecimate(constant OsvRenderParams& params [[buffer(0)]],
                               device const ushort* y0 [[buffer(1)]], device const ushort* u0 [[buffer(2)]],
                               device const ushort* v0 [[buffer(3)]], constant int* d0 [[buffer(4)]],
                               device const ushort* y1 [[buffer(5)]], device const ushort* u1 [[buffer(6)]],
                               device const ushort* v1 [[buffer(7)]], constant int* d1 [[buffer(8)]],
                               device float* dst [[buffer(9)]], uint3 gid [[thread_position_in_grid]]) {
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    const int lens = (int)gid.z;
    if (x >= params.seamLowW || y >= params.seamLowH || lens > 1) {
        return;
    }
    const OsvRenderParams p = params;
    const OsvPlane plane = lens == 0 ? osvMetalPlane(y0, u0, v0, d0) : osvMetalPlane(y1, u1, v1, d1);

    float texel[4];
    osvSeamLowDecimatePixel(&p, &plane, lens, x, y, texel);
    device float* d = dst + (((size_t)lens * (size_t)params.seamLowH + (size_t)y) * (size_t)params.seamLowW +
                             (size_t)x) * 4;
    d[0] = texel[0];
    d[1] = texel[1];
    d[2] = texel[2];
    d[3] = texel[3];
}

/* [WP-SEAMTOOLS] Seam low band, stages 2 and 3: one separable blur pass. */
kernel void osvSeamLowBlur(constant OsvRenderParams& params [[buffer(0)]], device const float* src [[buffer(1)]],
                           device float* dst [[buffer(2)]], constant int& horizontal [[buffer(3)]],
                           uint3 gid [[thread_position_in_grid]]) {
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    const int lens = (int)gid.z;
    if (x >= params.seamLowW || y >= params.seamLowH || lens > 1) {
        return;
    }
    const OsvRenderParams p = params;
    float texel[4];
    osvSeamLowBlurPixel(&p, src, lens, x, y, horizontal, texel);
    device float* d = dst + (((size_t)lens * (size_t)params.seamLowH + (size_t)y) * (size_t)params.seamLowW +
                             (size_t)x) * 4;
    d[0] = texel[0];
    d[1] = texel[1];
    d[2] = texel[2];
    d[3] = texel[3];
}

/* The reframe of an already stitched equirect frame (the Premiere effect's
 * shader): one thread per output pixel, straight RGBA floats out with a
 * pitch in floats.  Buffers: 0 OsvReframeParams, 1 OsvRgbaSource, 2 source
 * pixels, 3 output, 4 output pitch in floats. */
kernel void osvReframeEquirect(constant OsvReframeParams& params [[buffer(0)]],
                               constant OsvRgbaSource& source [[buffer(1)]],
                               device const uchar* pixels [[buffer(2)]], device float* out [[buffer(3)]],
                               constant int& outPitchFloats [[buffer(4)]], uint2 gid [[thread_position_in_grid]]) {
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= params.outW || y >= params.outH) {
        return;
    }
    const OsvReframeParams p = params;
    const OsvRgbaSource s = source;
    float rgba[4];
    osvReframeEquirectPixel(&p, &s, pixels, x, y, rgba);
    device float* dst = out + (size_t)y * (size_t)outPitchFloats + (size_t)x * 4;
    dst[0] = rgba[0];
    dst[1] = rgba[1];
    dst[2] = rgba[2];
    dst[3] = rgba[3];
}
