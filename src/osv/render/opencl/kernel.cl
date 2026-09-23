/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * kernel.cl - last source string of the OpenCL program: the __kernel wrapper
 * around osvShadePixelWSPL.  OpenCL cannot pass structs containing pointers as
 * kernel arguments, so the two planes arrive as separate buffers plus their
 * integer descriptors and are re-assembled into OsvPlane here.
 */
__kernel void osvReframe(const OsvRenderParams params,
                         __global const ushort* y0, __global const ushort* u0, __global const ushort* v0,
                         int w0, int h0, int cw0, int ch0, int sy0, int sc0, int bs0, int il0,
                         __global const ushort* y1, __global const ushort* u1, __global const ushort* v1,
                         int w1, int h1, int cw1, int ch1, int sy1, int sc1, int bs1, int il1,
                         __global const float* seam,
                         __global const float* warp,
                         __global const float* blendSeam,
                         __global const float* photo,
                         __global const float* seamLow,
                         __global float* out, int outPitchFloats) {
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    if (x >= params.outW || y >= params.outH) {
        return;
    }
    OsvPlane planes[2];
    planes[0].y = y0; planes[0].u = u0; planes[0].v = il0 ? (u0 + 1) : v0;
    planes[0].w = w0; planes[0].h = h0; planes[0].cw = cw0; planes[0].ch = ch0;
    planes[0].strideY = sy0; planes[0].strideC = sc0; planes[0].bitShift = bs0; planes[0].chromaInterleaved = il0;
    planes[1].y = y1; planes[1].u = u1; planes[1].v = il1 ? (u1 + 1) : v1;
    planes[1].w = w1; planes[1].h = h1; planes[1].cw = cw1; planes[1].ch = ch1;
    planes[1].strideY = sy1; planes[1].strideC = sc1; planes[1].bitShift = bs1; planes[1].chromaInterleaved = il1;

    float rgba[4];
    /* [WP-SEAM] blendSeam is always a valid buffer; params.blendSeamEnabled
     * decides whether it is read (OpenCL rejects a null __global argument). */
    /* [WP-PHOTO] photo is always a valid buffer too; params.photoEnabled
     * decides whether it is read. */
    /* [WP-SEAMTOOLS] seamLow likewise; params.seamSmoothEnabled (with a
     * carved seam) decides whether it is read. */
    osvShadePixelWSPL(&params, planes, seam, warp, blendSeam, photo, seamLow, x, y, rgba);
    __global float* dst = out + (size_t)y * (size_t)outPitchFloats + (size_t)x * 4;
    dst[0] = rgba[0];
    dst[1] = rgba[1];
    dst[2] = rgba[2];
    dst[3] = rgba[3];
}

/* [WP-SEAMTOOLS] Seam low band, stage 1: one work item per low-band texel,
 * global id 2 = the lens.  Same plane arguments as osvReframe. */
__kernel void osvSeamLowDecimate(const OsvRenderParams params,
                                 __global const ushort* y0, __global const ushort* u0, __global const ushort* v0,
                                 int w0, int h0, int cw0, int ch0, int sy0, int sc0, int bs0, int il0,
                                 __global const ushort* y1, __global const ushort* u1, __global const ushort* v1,
                                 int w1, int h1, int cw1, int ch1, int sy1, int sc1, int bs1, int il1,
                                 __global float* dst) {
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    const int lens = (int)get_global_id(2);
    if (x >= params.seamLowW || y >= params.seamLowH || lens > 1) {
        return;
    }
    OsvPlane planes[2];
    planes[0].y = y0; planes[0].u = u0; planes[0].v = il0 ? (u0 + 1) : v0;
    planes[0].w = w0; planes[0].h = h0; planes[0].cw = cw0; planes[0].ch = ch0;
    planes[0].strideY = sy0; planes[0].strideC = sc0; planes[0].bitShift = bs0; planes[0].chromaInterleaved = il0;
    planes[1].y = y1; planes[1].u = u1; planes[1].v = il1 ? (u1 + 1) : v1;
    planes[1].w = w1; planes[1].h = h1; planes[1].cw = cw1; planes[1].ch = ch1;
    planes[1].strideY = sy1; planes[1].strideC = sc1; planes[1].bitShift = bs1; planes[1].chromaInterleaved = il1;

    float texel[4];
    osvSeamLowDecimatePixel(&params, &planes[lens], lens, x, y, texel);
    __global float* d = dst + (((size_t)lens * (size_t)params.seamLowH + (size_t)y) * (size_t)params.seamLowW +
                               (size_t)x) * 4;
    d[0] = texel[0];
    d[1] = texel[1];
    d[2] = texel[2];
    d[3] = texel[3];
}

/* [WP-SEAMTOOLS] Seam low band, stages 2 and 3: one separable blur pass. */
__kernel void osvSeamLowBlur(const OsvRenderParams params, __global const float* src, __global float* dst,
                             int horizontal) {
    const int x = (int)get_global_id(0);
    const int y = (int)get_global_id(1);
    const int lens = (int)get_global_id(2);
    if (x >= params.seamLowW || y >= params.seamLowH || lens > 1) {
        return;
    }
    float texel[4];
    osvSeamLowBlurPixel(&params, src, lens, x, y, horizontal, texel);
    __global float* d = dst + (((size_t)lens * (size_t)params.seamLowH + (size_t)y) * (size_t)params.seamLowW +
                               (size_t)x) * 4;
    d[0] = texel[0];
    d[1] = texel[1];
    d[2] = texel[2];
    d[3] = texel[3];
}
