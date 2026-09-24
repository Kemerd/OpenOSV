/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * preamble.metal - first part of the Metal library source.
 *
 * The build concatenates, in this order (cmake/OsvMetal.cmake):
 *
 *     preamble.metal -> ColorMath.h -> osv_kernel.h -> kernel.metal
 *
 * exactly like the OpenCL program is assembled from preamble.cl, so the Metal
 * kernels run the very same shared shader as the CPU, CUDA and OpenCL
 * backends.  osv_kernel.h and ColorMath.h are written against the C99 float
 * math names (sqrtf, atan2f, ...); this preamble maps them onto the Metal
 * standard library and pins the address-space macros:
 *
 *   OSV_GLOBAL   device   - frame planes, tables, output
 *   OSV_PRIVATE  thread   - parameter blocks and small per-pixel arrays
 *
 * Numerical policy: the transcendental functions come from metal::precise
 * (the IEEE-conforming versions, whatever the library's fast-math setting),
 * and the library itself is compiled with fast math off, so division and
 * sqrt round correctly and nothing is reassociated.  That is what lets the
 * parity tests hold Metal to the same >= 60 dB against the CPU reference as
 * CUDA and OpenCL.
 */
#include <metal_stdlib>
using namespace metal;

#define OSV_HD static inline
#define OSV_GLOBAL device
#define OSV_PRIVATE thread

/* ---- C99 float math -> Metal ------------------------------------------- */
#define sqrtf(x) metal::precise::sqrt((float)(x))
#define sinf(x) metal::precise::sin((float)(x))
#define cosf(x) metal::precise::cos((float)(x))
#define tanf(x) metal::precise::tan((float)(x))
#define asinf(x) metal::precise::asin((float)(x))
#define acosf(x) metal::precise::acos((float)(x))
#define atanf(x) metal::precise::atan((float)(x))
#define atan2f(y, x) metal::precise::atan2((float)(y), (float)(x))
#define expf(x) metal::precise::exp((float)(x))
#define exp2f(x) metal::precise::exp2((float)(x))
#define logf(x) metal::precise::log((float)(x))
#define log2f(x) metal::precise::log2((float)(x))
#define log10f(x) metal::precise::log10((float)(x))
#define powf(x, y) metal::precise::pow((float)(x), (float)(y))
/* Exact operations: no precise variant needed. */
#define fminf(a, b) metal::fmin((float)(a), (float)(b))
#define fmaxf(a, b) metal::fmax((float)(a), (float)(b))
#define floorf(x) metal::floor((float)(x))
#define ceilf(x) metal::ceil((float)(x))
#define fabsf(x) metal::fabs((float)(x))
#define fmodf(x, y) metal::fmod((float)(x), (float)(y))
#define roundf(x) metal::round((float)(x))

/* The shared headers follow; they are guarded against multiple inclusion
 * and skip every host-only #include under __METAL_VERSION__. */
