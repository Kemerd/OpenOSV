/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * OfxKernelAbi.h - the arguments of the OpenFX kernels, shared by the .cu
 * that defines them and the host code that launches them (OfxCuda.cpp,
 * OfxGpuPipeline.cpp).
 *
 * cuLaunchKernel copies each argument byte for byte, so the two sides must
 * agree on the layout exactly; one header included by both is how they do.
 * Plain C: it is compiled by nvcc and by MSVC.
 */
#ifndef OSV_OFX_KERNEL_ABI_H
#define OSV_OFX_KERNEL_ABI_H

/* Name the kernel is exported under (extern "C", so unmangled). */
#define OSV_OFX_REFRAME_KERNEL_NAME "osvOfxReframeKernel"

/* Thread block shape: 16 x 16, two adjacent output rows per warp, the same
 * access pattern the Premiere effect's kernel uses. */
#define OSV_OFX_BLOCK_X 16
#define OSV_OFX_BLOCK_Y 16

/* Where the output image sits and which part of it one launch renders.
 * Host pixel coordinates throughout, y UP (OpenFX's convention). */
typedef struct OsvOfxTarget {
    int boundsX1;  /* Output image bounds: left.                          */
    int boundsY1;  /* Output image bounds: bottom.                        */
    int windowX1;  /* Render window: left.                                */
    int windowY1;  /* Render window: bottom.                              */
    int windowW;   /* Render window width (> 0).                          */
    int windowH;   /* Render window height (> 0).                         */
    int frameX1;   /* Camera frame (the output RoD): left.                */
    int frameY2;   /* Camera frame: one past its top row.                 */
    int rowBytes;  /* Output row pitch in bytes, y up (may be negative).  */
} OsvOfxTarget;

/* ========================================================================= */
/*  [WP-V-GPU] The own-GPU path for hosts that hand CPU images (VEGAS Pro)   */
/* ========================================================================= */
/*
 * Three more kernels live in the same fatbin (OfxReframeKernel.cu).  Each
 * writes one PACKED rectangle - the render window clipped to the image
 * bounds - into a tight device buffer, in the host's pixel format and
 * levels, so that only the finished pixels cross the bus:
 *
 *   osvOfxViewPackKernel      frame a FLOAT R,G,B,A (or B,G,R,A) equirect -
 *                             the engine's stitched sphere in VRAM, or a
 *                             host float image uploaded by the plug-in -
 *                             through osvReframeEquirectPixel();
 *   osvOfxViewPackByteKernel  the same camera over an 8-bit source, sampled
 *                             exactly as if it had been promoted to float
 *                             first (code * byteScale per texel);
 *   osvOfxEquirectPackKernel  copy a float R,G,B,A device image that is
 *                             exactly the camera frame (the sphere rendered
 *                             at the frame's size), packing as above.
 *
 * Packed row 0 is the BOTTOM row of the rectangle (host y = windowY1), so a
 * host image whose rows ascend in memory receives the bands in memory order.
 */
#define OSV_OFX_VIEW_KERNEL_NAME "osvOfxViewPackKernel"
#define OSV_OFX_VIEW_BYTE_KERNEL_NAME "osvOfxViewPackByteKernel"
#define OSV_OFX_EQUIRECT_KERNEL_NAME "osvOfxEquirectPackKernel"

/* How one packed rectangle is laid out and encoded. */
typedef struct OsvOfxPack {
    int windowX1;       /* Rectangle: left column, host pixels.                        */
    int windowY1;       /* Rectangle: bottom row, host pixels (y up).                  */
    int windowW;        /* Rectangle width (> 0).                                      */
    int windowH;        /* Rectangle height (> 0).                                     */
    int frameX1;        /* Camera frame: left.                                         */
    int frameY1;        /* Camera frame: bottom.                                       */
    int frameX2;        /* Camera frame: one past its right column.                    */
    int frameY2;        /* Camera frame: one past its top row.                         */
    int dstPitchBytes;  /* Packed row pitch in bytes (> 0); row 0 = host row windowY1. */
    int isByte;         /* 1 = four 8-bit codes per pixel, 0 = four 32-bit floats.     */
    int isBgra;         /* 1 = B,G,R,A in memory, 0 = R,G,B,A.                         */
    int studio;         /* 1 = studio levels on R, G, B (alpha never), 0 = full range. */
    float studioBlack;  /* kStudioBlack of OfxHostImage.h, as the host computed it.    */
    float studioSpan;   /* kStudioSpan of OfxHostImage.h, as the host computed it.     */
    float byteScale;    /* 1/255 as the host computed it: an 8-bit source's code scale. */
} OsvOfxPack;

#endif /* OSV_OFX_KERNEL_ABI_H */
