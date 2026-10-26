/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * preamble.cl - first source string of the OpenCL program.
 *
 * osv_kernel.h and ColorMath.h are written against the C99 float math names
 * (sqrtf, atan2f, ...).  OpenCL C exposes overloaded functions without the
 * f suffix, so this preamble maps one onto the other before the shared headers
 * are compiled.  It also pins the inline/address-space macros.
 */
/*
 * Disable floating-point multiply-add contraction so the OpenCL backend
 * matches the CPU (/fp:precise) and CUDA (-fmad=false) reference pipelines.
 * In OpenCL C (6.10.2), FP_CONTRACT defaults to ON unless explicitly disabled
 * by pragma at the top of the translation unit.
 */
#pragma OPENCL FP_CONTRACT OFF

/*
 * Pin the function linkage and address-space qualifiers before any shared
 * header is parsed.  In OpenCL C (which follows C99 6.7.4 inline semantics),
 * a bare `inline` without `static` produces an inline-only definition with no
 * external body emitted; if the device compiler declines to inline a large
 * helper such as osvShadePixelWSPL, the linker (e.g. AMD comgr / ld.lld)
 * fails with an undefined symbol error.  Marking every helper `static inline`
 * gives each translation unit internal linkage and guarantees a local body
 * whenever a call is not inlined.
 */
#define OSV_HD static inline
#define OSV_GLOBAL __global

#define sqrtf(x) sqrt((float)(x))
#define sinf(x) sin((float)(x))
#define cosf(x) cos((float)(x))
#define tanf(x) tan((float)(x))
#define asinf(x) asin((float)(x))
#define acosf(x) acos((float)(x))
#define atanf(x) atan((float)(x))
#define atan2f(y, x) atan2((float)(y), (float)(x))
#define expf(x) exp((float)(x))
#define exp2f(x) exp2((float)(x))
#define logf(x) log((float)(x))
#define log2f(x) log2((float)(x))
#define log10f(x) log10((float)(x))
#define powf(x, y) pow((float)(x), (float)(y))
#define fminf(a, b) fmin((float)(a), (float)(b))
#define fmaxf(a, b) fmax((float)(a), (float)(b))
#define floorf(x) floor((float)(x))
#define ceilf(x) ceil((float)(x))
#define fabsf(x) fabs((float)(x))
#define fmodf(x, y) fmod((float)(x), (float)(y))
#define roundf(x) round((float)(x))

/* The shared headers are guarded against multiple inclusion with these
 * macros; nothing else to do here. */
