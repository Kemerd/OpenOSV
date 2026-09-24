/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * OfxKernelAbi.h - the arguments of the OpenFX reframe kernel, shared by the
 * .cu that defines it and the host code that launches it (OfxCuda.cpp).
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

#endif /* OSV_OFX_KERNEL_ABI_H */
