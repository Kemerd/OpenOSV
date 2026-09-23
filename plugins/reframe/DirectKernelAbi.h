/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * DirectKernelAbi.h - the calling convention of osvReframeDirectKernel, the
 * fused fisheye -> view kernel in ReframeKernel.cu.
 *
 * Included by BOTH sides of the launch: nvcc compiles it into the fatbin and
 * MSVC compiles it into DirectLaunch.cpp, which builds the cuLaunchKernel
 * argument array.  cuLaunchKernel copies each argument by the SIZE the
 * kernel's metadata declares, so a host struct that disagreed with the device
 * struct by a single field would be read as garbage without any error.
 * Declaring the one aggregate argument here, once, is what rules that out.
 *
 * Written in the common subset of C and CUDA C++ (like osv_kernel.h): no
 * namespaces, no constructors, plain typedef'd structs.
 *
 * The kernel's parameter list, in order:
 *
 *   OsvRenderParams      params        the complete REFRAME block (DirectSetup::params)
 *   OsvDirectPlanes      planes        the two lens frames, DEVICE addresses
 *   const float*         seam          seam table (device) or NULL
 *   const float*         warp          warp grid (device) or NULL
 *   unsigned char*       dst           top-left pixel of the output frame (device)
 *   int                  dstRowBytes   positive row pitch in bytes
 *   int                  dstIsHalf     1 = BGRA 16f, 0 = BGRA 32f
 *
 * One thread per output pixel, OSV_DIRECT_BLOCK_X x OSV_DIRECT_BLOCK_Y
 * threads per block, a grid rounded up to whole blocks.
 */
#ifndef OSV_DIRECT_KERNEL_ABI_H
#define OSV_DIRECT_KERNEL_ABI_H

#include "osv/render/osv_kernel.h"

/* Exported (extern "C", so unmangled) name of the kernel in the fatbin. */
#define OSV_DIRECT_KERNEL_NAME "osvReframeDirectKernel"

/* Thread-block shape.  16 x 16 keeps a warp on two adjacent output rows,
 * which keeps neighbouring threads on neighbouring fisheye texels. */
#define OSV_DIRECT_BLOCK_X 16
#define OSV_DIRECT_BLOCK_Y 16

/* The two lens frames, passed by value as ONE kernel argument so the shader
 * can index them as an array without a copy.  [0] = slave (stream 0),
 * [1] = master (stream 1), the same order as OsvRenderParams::lens. */
typedef struct OsvDirectPlanes {
    OsvPlane lens[2];
} OsvDirectPlanes;

#endif /* OSV_DIRECT_KERNEL_ABI_H */
