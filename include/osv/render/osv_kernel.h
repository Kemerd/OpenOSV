/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * osv_kernel.h - the ONE per-pixel shader shared by every renderer backend.
 *
 * This header is written in the common subset of C99, C++20, CUDA C++ and
 * OpenCL C so that exactly the same arithmetic runs on the CPU reference
 * renderer, inside a CUDA kernel and inside an OpenCL kernel.  The parity tests
 * (tests/unit/test_render_*.cpp) demand PSNR >= 60 dB between backends after
 * 16-bit quantisation, which is only achievable when the code is literally the
 * same.  Therefore:
 *
 *   * no STL, no references, no templates, no exceptions, no constructors;
 *   * only float arithmetic and the C99 float math functions (sqrtf, atan2f,
 *     sinf, cosf, tanf, expf, exp2f, logf, log2f, powf, fminf, fmaxf, floorf,
 *     fabsf) - the OpenCL preamble maps them onto the overloaded OpenCL
 *     functions;
 *   * every function is marked OSV_HD (host+device inline);
 *   * pointers into image memory are qualified with OSV_GLOBAL (expands to
 *     __global under OpenCL, to nothing elsewhere).
 *
 * Coordinate conventions (see docs/GEOMETRY.md):
 *   view frame   : X right, Y forward, Z up (right-handed)
 *   body frame   : X right, Y forward (master lens axis), Z up
 *   lens frame   : +z optical axis, +x image right, +y image down
 *   pixel centres: sample position (px + 0.5, py + 0.5)
 *
 * The colour maths lives in osv/color/ColorMath.h (same dialect); under
 * OpenCL that header is passed to the compiler as a preceding source string
 * instead of being #included.
 */
#ifndef OSV_KERNEL_H
#define OSV_KERNEL_H

/* ------------------------------------------------------------------------- */
/*  Dialect detection                                                         */
/* ------------------------------------------------------------------------- */
#if defined(__OPENCL_C_VERSION__) || defined(__OPENCL_VERSION__)
#define OSV_KERNEL_OPENCL 1
#ifndef OSV_HD
#define OSV_HD static inline
#endif
#ifndef OSV_GLOBAL
#define OSV_GLOBAL __global
#endif
#elif defined(__CUDACC__)
#define OSV_KERNEL_CUDA 1
#ifndef OSV_HD
#define OSV_HD __host__ __device__ __forceinline__
#endif
#ifndef OSV_GLOBAL
#define OSV_GLOBAL
#endif
#else
#define OSV_KERNEL_HOST 1
#ifndef OSV_HD
#define OSV_HD static inline
#endif
#ifndef OSV_GLOBAL
#define OSV_GLOBAL
#endif
#include <math.h>
#include <stddef.h>
#endif

#if !defined(OSV_KERNEL_OPENCL)
#include "osv/color/ColorMath.h"
#endif

/* Transfer identifiers mirror OsvColorParams.transfer (ColorMath.h). */
#ifndef OSV_TRANSFER_PASSTHROUGH
#define OSV_TRANSFER_PASSTHROUGH 4
#endif

/* Sample type of the decoded planes: 16-bit words holding 10-bit values
 * (or P010 words holding the value in the top bits, see bitShift). */
#if defined(OSV_KERNEL_OPENCL)
typedef ushort osv_u16;
#else
typedef unsigned short osv_u16;
#endif

/* ------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* ------------------------------------------------------------------------- */
#define OSV_KERNEL_PI 3.14159265358979323846f
#define OSV_KERNEL_TWO_PI 6.28318530717958647692f
#define OSV_KERNEL_HALF_PI 1.57079632679489661923f

/* Output mode */
#define OSV_MODE_REFRAME 0   /* virtual camera (rectilinear / fisheye / stereographic) */
#define OSV_MODE_EQUIRECT 1  /* full 360 x 180 equirectangular map */

/* Virtual camera projection (OSV_MODE_REFRAME) */
#define OSV_PROJ_RECTILINEAR 0
#define OSV_PROJ_FISHEYE 1
#define OSV_PROJ_STEREOGRAPHIC 2
/* Eye-offset ("generalised stereographic") projection: the sphere is viewed
 * from a point d radii behind its centre, r/f = (1 + d) sin(theta) /
 * (d + cos(theta)).  d = 0 is rectilinear, d = 1 is stereographic. */
#define OSV_PROJ_EYE_OFFSET 3

/* Entry points usable from plain C/C++, CUDA and OpenCL alike. */
#ifndef OSV_FN
#define OSV_FN OSV_HD
#endif

/* Equirect layout (OSV_MODE_EQUIRECT) */
#define OSV_LAYOUT_STANDARD 0   /* Z up, +Y (master lens) at the image centre */
#define OSV_LAYOUT_POLAR_AXIS 1 /* lens axes at the poles, seam on the equator */

/* Maximum vertices of the occlusion polygon carried per lens.
 *
 * The metadata stores an OPEN arc (14 entries, 13 distinct, on the Osmo 360),
 * but the polygon the kernel tests is that arc closed outward along the image
 * rim, so it carries roughly TWICE the stored vertex count - 26 on this
 * camera.  A limit of 16 silently truncated it to a shape that bore no
 * relation to the stick, so this is sized with headroom for a firmware that
 * samples the arc more finely.  Cost: 4 floats per point per lens, i.e. 512
 * bytes total at 32, well inside the 4 KB parameter-block budget that
 * tests/unit/test_render.cpp asserts. */
#define OSV_MAX_OCCLUSION_POINTS 32

/* ------------------------------------------------------------------------- */
/*  Plain-old-data parameter blocks (built on the host, copied by value)      */
/* ------------------------------------------------------------------------- */

/* One fisheye lens in STREAM pixel units. */
typedef struct OsvLens {
    float fx, fy, cx, cy;   /* pinhole part of the Kannala-Brandt model      */
    float k[5];             /* radial polynomial k1..k5                        */
    float R[9];             /* body -> lens rotation, row-major               */
    float thetaMax;         /* largest usable angle from the optical axis (rad)*/
    float featherRad;       /* width of the FOV feather below thetaMax (rad)  */
    float gain[3];          /* per-channel linear-light gain (exposure match) */
    float occlX[OSV_MAX_OCCLUSION_POINTS]; /* occlusion polygon, stream px    */
    float occlY[OSV_MAX_OCCLUSION_POINTS];
    float occlFeatherPx;    /* feather distance outside the polygon (px)      */
    int occlN;              /* number of polygon vertices (0 = none)          */
    int width, height;      /* decoded frame size in pixels                    */
    int enabled;            /* 0 = ignore this lens entirely                  */
} OsvLens;

/* One decoded 4:2:0 frame (three planes or luma + interleaved chroma). */
typedef struct OsvPlane {
    OSV_GLOBAL const osv_u16* y;   /* luma plane                              */
    OSV_GLOBAL const osv_u16* u;   /* Cb plane (or interleaved CbCr)          */
    OSV_GLOBAL const osv_u16* v;   /* Cr plane (== u + 1 when interleaved)    */
    int w, h;                      /* luma size                               */
    int cw, ch;                    /* chroma size                             */
    int strideY;                   /* luma row pitch in elements              */
    int strideC;                   /* chroma row pitch in elements            */
    int bitShift;                  /* right shift to 10-bit scale (P010: 6)   */
    int chromaInterleaved;         /* 1 = CbCr interleaved (step 2)           */
} OsvPlane;

/* Everything the shader needs besides the planes and the seam table. */
typedef struct OsvRenderParams {
    int outW, outH;          /* output size in pixels                          */
    int mode;                /* OSV_MODE_*                                     */
    int projection;          /* OSV_PROJ_* (reframe)                           */
    int layout;              /* OSV_LAYOUT_* (equirect)                        */
    float focalPx;           /* virtual camera focal length in pixels          */
    float tanHalfH;          /* tan(hfov/2) (rectilinear)                      */
    float tanHalfV;          /* tan(hfov/2) * H / W (rectilinear)              */
    float eyeOffset;         /* d in [0, 1] (OSV_PROJ_EYE_OFFSET only)         */
    float Rout[9];           /* body <- view rotation, row-major               */
    OsvLens lens[2];         /* [0] slave (stream 0), [1] master (stream 1)    */
    int blendEnabled;        /* 0 = nearest-lens only (no feather blend)       */
    int seamShiftEnabled;    /* 1 = apply the per-column seam table            */
    int seamColumns;         /* number of entries in the seam table            */
    int outputAlphaCoverage; /* 1 = alpha = coverage, 0 = alpha always 1       */
    OsvColorParams color;    /* colour pipeline (see ColorMath.h)              */
} OsvRenderParams;

/* ------------------------------------------------------------------------- */
/*  Second entry point: reframing an already stitched equirect RGBA frame     */
/* ------------------------------------------------------------------------- */

/* Description of an equirectangular RGBA frame in the Standard layout
 * (lon = (x/W - 0.5) 2pi, lat = (0.5 - y/H) pi, centre column = +Y, row 0 =
 * top).  The pixel memory itself is passed separately so the same descriptor
 * serves host and device buffers. */
typedef struct OsvRgbaSource {
    int w, h;            /* equirect size in pixels                            */
    int pitchBytes;      /* row pitch in bytes (positive, row 0 at the top)    */
    int isHalf;          /* 1 = 16-bit float channels, 0 = 32-bit float        */
    int isBgra;          /* 1 = channel order B,G,R,A (Premiere), 0 = R,G,B,A  */
} OsvRgbaSource;

/* Everything osvReframeEquirectPixel needs besides the source frame.  The
 * picture is drawn into the viewport rectangle; pixels of the output frame
 * outside it are transparent black (letterbox / pillarbox). */
typedef struct OsvReframeParams {
    int outW, outH;                 /* full output frame                        */
    int viewX, viewY, viewW, viewH; /* rectangle that receives the picture      */
    int projection;                 /* OSV_PROJ_* (EYE_OFFSET recommended)      */
    float eyeOffset;                /* d in [0, 1]                              */
    float focalPx;                  /* focal length for the viewport width      */
    float tanHalfH, tanHalfV;       /* rectilinear helpers (viewport aspect)    */
    float Rout[9];                  /* body <- view rotation, row-major         */
    int fillAlphaOne;               /* 1 = opaque output inside the viewport    */
} OsvReframeParams;

/* Reinterpretation helper for the half-float decoder below.  A union is the
 * one type pun every dialect (C99, C++, CUDA C++, OpenCL C) accepts. */
typedef union OsvFloatBits {
    unsigned int u;
    float f;
} OsvFloatBits;

/* ------------------------------------------------------------------------- */
/*  Small vector helpers                                                      */
/* ------------------------------------------------------------------------- */
/* osvClampf comes from ColorMath.h (compiled before this header). */

OSV_HD float osvSmoothstep(float t) {
    t = osvClampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

OSV_HD void osvMat3MulVec(const float* m, const float* v, float* out) {
    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    out[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    out[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

OSV_HD void osvMat3TMulVec(const float* m, const float* v, float* out) {
    out[0] = m[0] * v[0] + m[3] * v[1] + m[6] * v[2];
    out[1] = m[1] * v[0] + m[4] * v[1] + m[7] * v[2];
    out[2] = m[2] * v[0] + m[5] * v[1] + m[8] * v[2];
}

OSV_HD void osvNormalize3(float* v) {
    const float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0.0f) {
        v[0] /= n;
        v[1] /= n;
        v[2] /= n;
    }
}

OSV_HD void osvCross3(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/* Rotate v about the unit axis n by angle a (Rodrigues). */
OSV_HD void osvRotateAboutAxis(const float* v, const float* n, float a, float* out) {
    const float c = cosf(a);
    const float s = sinf(a);
    float nxv[3];
    osvCross3(n, v, nxv);
    const float ndv = n[0] * v[0] + n[1] * v[1] + n[2] * v[2];
    out[0] = v[0] * c + nxv[0] * s + n[0] * ndv * (1.0f - c);
    out[1] = v[1] * c + nxv[1] * s + n[1] * ndv * (1.0f - c);
    out[2] = v[2] * c + nxv[2] * s + n[2] * ndv * (1.0f - c);
}

/* ------------------------------------------------------------------------- */
/*  Output pixel -> view ray                                                  */
/* ------------------------------------------------------------------------- */

/* Eye-offset projection, inverse direction: angle from the view axis for a
 * radius r (pixels from the image centre).  Forward model
 *     r / f = (1 + d) sin(theta) / (d + cos(theta)),
 * inverted through k = r / (f (1 + d)):
 *     theta = atan(k) + asin(k d / sqrt(1 + k^2)).
 * The projection has an asymptote at theta = acos(-d); anything at or beyond
 * it (and every non-finite input) is reported as uncovered by returning a
 * negative angle. */
OSV_HD float osvEyeOffsetTheta(float r, float focalPx, float d) {
    if (!(focalPx > 0.0f) || !(r >= 0.0f)) {
        return -1.0f;
    }
    const float k = r / (focalPx * (1.0f + d));
    /* k d / sqrt(1 + k^2), written so that k^2 overflowing to infinity for
     * absurd radii still gives the correct limit d instead of 0. */
    const float s = (k > 1.0f) ? osvClampf(d / sqrtf(1.0f + 1.0f / (k * k)), -1.0f, 1.0f)
                               : osvClampf(k * d / sqrtf(1.0f + k * k), -1.0f, 1.0f);
    const float theta = atanf(k) + asinf(s);
    const float thetaMax = acosf(osvClampf(-d, -1.0f, 1.0f));
    if (!(theta < thetaMax)) {
        return -1.0f;
    }
    return theta;
}

/* Unit view ray for a centred pixel offset (nx right, ny up, in pixels) of a
 * W x H viewport under one of the OSV_PROJ_* camera models.  Shared by the
 * fisheye stitching shader and the equirect reframe entry point so both
 * produce the same framing for the same parameters.  Returns 0 when the
 * pixel maps to no direction (outside the image circle / valid radius). */
OSV_HD int osvViewRay(int projection, float focalPx, float eyeOffset, float tanHalfH, float tanHalfV, float W,
                      float H, float nx, float ny, float* d) {
    if (projection == OSV_PROJ_RECTILINEAR) {
        const float u = (nx / (0.5f * W)) * tanHalfH;
        const float v = (ny / (0.5f * H)) * tanHalfV;
        d[0] = u;
        d[1] = 1.0f;
        d[2] = v;
        osvNormalize3(d);
        return 1;
    }

    /* Radial models: the angle from the view axis grows with the radius. */
    const float r = sqrtf(nx * nx + ny * ny);
    float theta;
    if (projection == OSV_PROJ_STEREOGRAPHIC) {
        theta = 2.0f * atanf(r / (2.0f * focalPx));
    } else if (projection == OSV_PROJ_EYE_OFFSET) {
        theta = osvEyeOffsetTheta(r, focalPx, eyeOffset);
        if (theta < 0.0f) {
            return 0; /* beyond the valid radius of the eye-offset model */
        }
    } else {
        theta = r / focalPx;
    }
    if (theta >= OSV_KERNEL_PI) {
        return 0; /* beyond the back of the sphere */
    }
    const float st = sinf(theta);
    const float ct = cosf(theta);
    if (r > 0.0f) {
        d[0] = st * (nx / r);
        d[2] = st * (ny / r);
    } else {
        d[0] = 0.0f;
        d[2] = 0.0f;
    }
    d[1] = ct;
    return 1;
}

/* Direction (unit vector, view frame) seen through output pixel (px, py).
 * Returns 0 when the pixel maps to no direction (outside a fisheye circle). */
OSV_HD int osvRayForPixel(const OsvRenderParams* p, float px, float py, float* d) {
    const float W = (float)p->outW;
    const float H = (float)p->outH;
    const float sx = px + 0.5f;
    const float sy = py + 0.5f;

    if (p->mode == OSV_MODE_EQUIRECT) {
        /* Full sphere.  lon runs left to right, lat top (+90) to bottom (-90). */
        const float lon = (sx / W) * OSV_KERNEL_TWO_PI - OSV_KERNEL_PI;
        const float lat = OSV_KERNEL_HALF_PI - (sy / H) * OSV_KERNEL_PI;
        const float cl = cosf(lat);
        if (p->layout == OSV_LAYOUT_POLAR_AXIS) {
            /* Poles = the two lens axes (+/-Y), seam = equator row. */
            d[0] = cl * sinf(lon);
            d[1] = sinf(lat);
            d[2] = cl * cosf(lon);
        } else {
            /* Standard: Z up, image centre looks along +Y (master lens). */
            d[0] = sinf(lon) * cl;
            d[1] = cosf(lon) * cl;
            d[2] = sinf(lat);
        }
        return 1;
    }

    /* Virtual camera.  nx/ny are centred pixel offsets with +ny = up. */
    const float nx = sx - 0.5f * W;
    const float ny = 0.5f * H - sy;
    return osvViewRay(p->projection, p->focalPx, p->eyeOffset, p->tanHalfH, p->tanHalfV, W, H, nx, ny, d);
}

/* ------------------------------------------------------------------------- */
/*  Body ray -> lens pixel                                                    */
/* ------------------------------------------------------------------------- */

/* Kannala-Brandt radial polynomial: r/f as a function of theta. */
OSV_HD float osvThetaD(const OsvLens* L, float theta) {
    const float t2 = theta * theta;
    const float t4 = t2 * t2;
    const float t6 = t4 * t2;
    const float t8 = t4 * t4;
    const float t10 = t8 * t2;
    return theta * (1.0f + L->k[0] * t2 + L->k[1] * t4 + L->k[2] * t6 + L->k[3] * t8 + L->k[4] * t10);
}

/* Project a body-frame direction into lens pixel coordinates.
 * Returns 1 when the ray is inside the usable field of view and the pixel
 * lies inside the frame; theta receives the angle from the optical axis. */
OSV_HD int osvProjectLens(const OsvLens* L, const float* dBody, float* px, float* py, float* theta) {
    float dl[3];
    osvMat3MulVec(L->R, dBody, dl);
    const float rho = sqrtf(dl[0] * dl[0] + dl[1] * dl[1]);
    const float th = atan2f(rho, dl[2]);
    *theta = th;
    if (th > L->thetaMax) {
        return 0;
    }
    const float rd = osvThetaD(L, th);
    float u, v;
    if (rho > 0.0f) {
        u = L->cx + L->fx * rd * (dl[0] / rho);
        v = L->cy + L->fy * rd * (dl[1] / rho);
    } else {
        u = L->cx;
        v = L->cy;
    }
    *px = u;
    *py = v;
    /* Reject samples outside the decoded frame (rim corners are black). */
    if (u < 0.0f || v < 0.0f || u >= (float)L->width || v >= (float)L->height) {
        return 0;
    }
    return 1;
}

/* Signed occlusion factor: 0 inside the occlusion polygon, ramping to 1 at
 * occlFeatherPx outside it.  Even-odd point-in-polygon plus distance to the
 * nearest edge.  With occlN == 0 the factor is always 1. */
OSV_HD float osvOcclusionFactor(const OsvLens* L, float px, float py) {
    const int n = L->occlN;
    if (n < 3) {
        return 1.0f;
    }
    int inside = 0;
    float bestDist2 = 3.4e38f;
    int j = n - 1;
    for (int i = 0; i < n; ++i) {
        const float xi = L->occlX[i], yi = L->occlY[i];
        const float xj = L->occlX[j], yj = L->occlY[j];
        /* even-odd crossing test */
        if (((yi > py) != (yj > py))) {
            const float xcross = xj + (py - yj) * (xi - xj) / (yi - yj);
            if (px < xcross) {
                inside = !inside;
            }
        }
        /* distance from (px,py) to segment (xj,yj)-(xi,yi) */
        const float ex = xi - xj, ey = yi - yj;
        const float len2 = ex * ex + ey * ey;
        float t = 0.0f;
        if (len2 > 0.0f) {
            t = osvClampf(((px - xj) * ex + (py - yj) * ey) / len2, 0.0f, 1.0f);
        }
        const float qx = xj + t * ex - px;
        const float qy = yj + t * ey - py;
        const float d2 = qx * qx + qy * qy;
        if (d2 < bestDist2) {
            bestDist2 = d2;
        }
        j = i;
    }
    if (inside) {
        return 0.0f;
    }
    if (L->occlFeatherPx <= 0.0f) {
        return 1.0f;
    }
    return osvClampf(sqrtf(bestDist2) / L->occlFeatherPx, 0.0f, 1.0f);
}

/* Blend weight of a lens for a ray at angle theta landing on (px, py). */
OSV_HD float osvLensWeight(const OsvLens* L, float theta, float px, float py) {
    if (theta > L->thetaMax) {
        return 0.0f;
    }
    float w = 1.0f;
    if (L->featherRad > 0.0f) {
        w = osvSmoothstep((L->thetaMax - theta) / L->featherRad);
    }
    return w * osvOcclusionFactor(L, px, py);
}

/* ------------------------------------------------------------------------- */
/*  Bilinear YCbCr fetch                                                      */
/* ------------------------------------------------------------------------- */

OSV_HD float osvFetchPlane(OSV_GLOBAL const osv_u16* plane, int stride, int step, int w, int h, int x, int y,
                           int bitShift) {
    x = x < 0 ? 0 : (x >= w ? w - 1 : x);
    y = y < 0 ? 0 : (y >= h ? h - 1 : y);
    return (float)(plane[y * stride + x * step] >> bitShift);
}

/* Bilinear sample of one plane at continuous coordinates (sample centres at
 * integer + 0.5).  Clamp-to-edge addressing.  Returns the value on the
 * 10-bit code scale. */
OSV_HD float osvBilinear(OSV_GLOBAL const osv_u16* plane, int stride, int step, int w, int h, float sx, float sy,
                         int bitShift) {
    const float fx = sx - 0.5f;
    const float fy = sy - 0.5f;
    const float flx = floorf(fx);
    const float fly = floorf(fy);
    const int x0 = (int)flx;
    const int y0 = (int)fly;
    const float tx = fx - flx;
    const float ty = fy - fly;
    const float a = osvFetchPlane(plane, stride, step, w, h, x0, y0, bitShift);
    const float b = osvFetchPlane(plane, stride, step, w, h, x0 + 1, y0, bitShift);
    const float c = osvFetchPlane(plane, stride, step, w, h, x0, y0 + 1, bitShift);
    const float d = osvFetchPlane(plane, stride, step, w, h, x0 + 1, y0 + 1, bitShift);
    const float top = a + (b - a) * tx;
    const float bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

/* Y, Cb, Cr at luma pixel position (px, py).  Chroma is sampled at half
 * resolution with MPEG-2 (left-aligned) siting: chroma sample column j covers
 * luma columns 2j and 2j+1 and is centred on luma x = 2j, so the chroma
 * coordinate is px / 2 horizontally and (py + 0.5) / 2 - 0.25 + 0.25 vertically
 * (vertically centred between the two rows -> py / 2). */
OSV_HD void osvSampleYuv(const OsvPlane* P, float px, float py, float* yuv) {
    yuv[0] = osvBilinear(P->y, P->strideY, 1, P->w, P->h, px, py, P->bitShift);
    const int step = P->chromaInterleaved ? 2 : 1;
    const float cxp = px * 0.5f + 0.25f; /* left-sited chroma: centre at 2j -> (2j+0.5)/2 = j+0.25 */
    const float cyp = py * 0.5f;
    yuv[1] = osvBilinear(P->u, P->strideC, step, P->cw, P->ch, cxp, cyp, P->bitShift);
    yuv[2] = osvBilinear(P->v, P->strideC, step, P->cw, P->ch, cxp, cyp, P->bitShift);
}

/* ------------------------------------------------------------------------- */
/*  Seam shift                                                                */
/* ------------------------------------------------------------------------- */

/* Longitude column index of a body direction in the polar-axis layout. */
OSV_HD int osvSeamColumn(const OsvRenderParams* p, const float* dBody) {
    const float lon = atan2f(dBody[0], dBody[2]); /* -pi..pi */
    float f = (lon + OSV_KERNEL_PI) / OSV_KERNEL_TWO_PI;
    f = osvClampf(f, 0.0f, 0.999999f);
    int c = (int)(f * (float)p->seamColumns);
    if (c < 0) {
        c = 0;
    }
    if (c >= p->seamColumns) {
        c = p->seamColumns - 1;
    }
    return c;
}

/* Rotate the body ray toward (delta > 0) or away from the optical axis of
 * lens i along the meridian through the axis.  Used to apply half of the
 * measured seam disparity to each lens. */
OSV_HD void osvShiftTowardAxis(const OsvLens* L, const float* dBody, float deltaRad, float* out) {
    /* lens optical axis expressed in the body frame = R^T * (0,0,1) */
    const float z[3] = {0.0f, 0.0f, 1.0f};
    float axis[3];
    osvMat3TMulVec(L->R, z, axis);
    float n[3];
    osvCross3(dBody, axis, n);
    const float nn = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nn < 1e-6f || deltaRad == 0.0f) {
        out[0] = dBody[0];
        out[1] = dBody[1];
        out[2] = dBody[2];
        return;
    }
    n[0] /= nn;
    n[1] /= nn;
    n[2] /= nn;
    /* rotating about d x axis by +delta moves d toward the axis */
    osvRotateAboutAxis(dBody, n, deltaRad, out);
}

/* ------------------------------------------------------------------------- */
/*  The shader                                                                */
/* ------------------------------------------------------------------------- */

/* Compute output pixel (x, y).  `planes` holds the two lens frames, `seam`
 * the optional per-column seam shift table in degrees (may be 0 when
 * seamShiftEnabled == 0).  `out` receives R, G, B in the output encoding and
 * A = coverage (or 1).  Pixels seen by neither lens are (0,0,0,0). */
OSV_HD void osvShadePixel(const OsvRenderParams* p, const OsvPlane* planes, OSV_GLOBAL const float* seam, int x,
                          int y, float* out) {
    float dView[3];
    if (!osvRayForPixel(p, (float)x, (float)y, dView)) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    float dBody[3];
    osvMat3MulVec(p->Rout, dView, dBody);

    /* Optional seam correction.  The table holds the measured disparity in
     * degrees: positive means a near object appears farther from BOTH lens
     * axes than the calibration predicts, so each lens samples half that
     * angle farther from its own axis (a negative "toward axis" rotation). */
    float seamDelta = 0.0f;
    if (p->seamShiftEnabled && p->seamColumns > 0 && seam != 0) {
        seamDelta = -seam[osvSeamColumn(p, dBody)] * (OSV_KERNEL_PI / 180.0f) * 0.5f;
    }

    /* Project into both lenses and compute their weights. */
    float w[2] = {0.0f, 0.0f};
    float px[2] = {0.0f, 0.0f};
    float py[2] = {0.0f, 0.0f};
    /* Whether osvProjectLens actually wrote px/py for this lens.  It returns
     * early - leaving them untouched - for a ray past thetaMax, so a rescue
     * that read px/py without checking this would sample pixel (0, 0). */
    int projected[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        const OsvLens* L = &p->lens[i];
        if (!L->enabled) {
            continue;
        }
        float dl[3];
        if (seamDelta != 0.0f) {
            /* both lenses move symmetrically relative to their own axis */
            osvShiftTowardAxis(L, dBody, seamDelta, dl);
        } else {
            dl[0] = dBody[0];
            dl[1] = dBody[1];
            dl[2] = dBody[2];
        }
        float theta;
        if (osvProjectLens(L, dl, &px[i], &py[i], &theta)) {
            w[i] = osvLensWeight(L, theta, px[i], py[i]);
            projected[i] = 1;
        }
    }

    if (!p->blendEnabled) {
        /* Nearest-lens selection: keep only the heavier weight. */
        if (w[0] >= w[1]) {
            w[1] = 0.0f;
        } else {
            w[0] = 0.0f;
        }
    }

    /* Coverage below 1e-4 is treated as none: at the very edge of the feather
     * the GPU and CPU transcendental functions can disagree by an ulp on
     * whether a ray is inside the FOV, and normalising an almost-zero weight
     * would turn that ulp into a full-brightness pixel. */
    float wsum = w[0] + w[1];

    /* ---- occlusion rescue -------------------------------------------------
     * A direction can be occluded in one lens and merely PAST THE FOV EDGE in
     * the other, and then both weights are zero and the pixel comes out black.
     * On a back-to-back 360 rig that is never the honest answer: the selfie
     * stick arc sits at theta 92.6..97.6 deg, where the opposite lens looks
     * along its own theta ~= 180 - 95 = 85 deg, comfortably inside its 97.59
     * deg limit.  The black only appeared because the OCCLUSION factor, not
     * the field of view, had zeroed the near lens while the far lens happened
     * to be just outside its feather.
     *
     * So before giving up, take whichever lens actually landed on valid
     * pixels and is NOT occluded there, ignoring the FOV feather.  The
     * feather exists to cross-fade the seam, not to declare a direction
     * unseeable, and a slightly soft pixel is enormously better than a hole.
     * A direction genuinely outside both lenses still falls through to
     * transparent black below. */
    if (wsum <= 1e-4f) {
        for (int i = 0; i < 2; ++i) {
            const OsvLens* L = &p->lens[i];
            /* projected[i] is the guard that makes px/py meaningful here. */
            if (!L->enabled || !projected[i]) {
                continue;
            }
            /* Inside the stick arc this lens is genuinely blind, so it must
             * stay at zero - rescuing it would paint the stick back in. */
            if (osvOcclusionFactor(L, px[i], py[i]) <= 0.0f) {
                continue;
            }
            /* The lens landed on a real, unoccluded pixel and was zeroed only
             * by the FOV feather.  Give it a tiny weight: enough to fill the
             * hole, small enough that any lens with real coverage still
             * dominates the blend. */
            w[i] = 1e-3f;
        }
        wsum = w[0] + w[1];
    }

    if (wsum <= 1e-4f) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }

    /* Fetch, decode and accumulate.  Passthrough blends in code space
     * because the log curve has no device-side inverse; every other transfer
     * blends in scene-linear light. */
    const int passthrough = (p->color.transfer == OSV_TRANSFER_PASSTHROUGH);
    float acc[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 2; ++i) {
        if (w[i] <= 0.0f) {
            continue;
        }
        float yuv[3];
        osvSampleYuv(&planes[i], px[i], py[i], yuv);
        float code[3];
        osvYuvToCode(&p->color, yuv[0], yuv[1], yuv[2], code);
        float val[3];
        if (passthrough) {
            val[0] = code[0];
            val[1] = code[1];
            val[2] = code[2];
        } else {
            osvCodeToLinear(&p->color, code, val);
            val[0] *= p->lens[i].gain[0];
            val[1] *= p->lens[i].gain[1];
            val[2] *= p->lens[i].gain[2];
        }
        acc[0] += val[0] * w[i];
        acc[1] += val[1] * w[i];
        acc[2] += val[2] * w[i];
    }
    acc[0] /= wsum;
    acc[1] /= wsum;
    acc[2] /= wsum;

    if (passthrough) {
        out[0] = acc[0];
        out[1] = acc[1];
        out[2] = acc[2];
    } else {
        osvLinearToOutput(&p->color, acc, out);
    }
    out[3] = p->outputAlphaCoverage ? fminf(wsum, 1.0f) : 1.0f;
}

/* ------------------------------------------------------------------------- */
/*  Equirect RGBA reframe (Premiere effect path)                              */
/* ------------------------------------------------------------------------- */

/* IEEE 754 binary16 -> binary32 by field manipulation only (no intrinsics,
 * so MSVC, nvcc device code and OpenCL C all run the same instructions).
 * Handles signed zero, subnormals, infinities and NaN payloads. */
OSV_HD float osvHalfToFloat(unsigned int h) {
    const unsigned int sign = (h >> 15) & 1u;
    const unsigned int exponent = (h >> 10) & 0x1Fu;
    const unsigned int mantissa = h & 0x3FFu;
    unsigned int bits;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            /* signed zero */
            bits = sign << 31;
        } else {
            /* subnormal: value = mantissa * 2^-24, renormalise by shifting the
             * leading one into the implicit position (bit 10) */
            unsigned int m = mantissa;
            unsigned int e = 113u; /* 127 - 14: exponent of the first normal */
            while ((m & 0x400u) == 0u) {
                m <<= 1;
                e -= 1u;
            }
            bits = (sign << 31) | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exponent == 0x1Fu) {
        /* infinity or NaN (payload kept) */
        bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
    } else {
        /* normal: rebias 15 -> 127 */
        bits = (sign << 31) | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    OsvFloatBits pun;
    pun.u = bits;
    return pun.f;
}

/* Wrap an integer index into [0, n) (n > 0), used for the longitude seam. */
OSV_HD int osvWrapIndex(int i, int n) {
    i %= n;
    if (i < 0) {
        i += n;
    }
    return i;
}

/* Read one texel of the source as straight RGBA floats.  x and y must
 * already be inside the frame; the channel order and sample type of the
 * source are resolved here so the sampler above stays generic. */
OSV_HD void osvFetchRgba(const OsvRgbaSource* src, OSV_GLOBAL const void* pixels, int x, int y, float* rgba) {
    OSV_GLOBAL const unsigned char* row =
        (OSV_GLOBAL const unsigned char*)pixels + (size_t)y * (size_t)src->pitchBytes;
    float c[4];
    if (src->isHalf) {
        OSV_GLOBAL const osv_u16* texel = (OSV_GLOBAL const osv_u16*)row + (size_t)x * 4u;
        c[0] = osvHalfToFloat((unsigned int)texel[0]);
        c[1] = osvHalfToFloat((unsigned int)texel[1]);
        c[2] = osvHalfToFloat((unsigned int)texel[2]);
        c[3] = osvHalfToFloat((unsigned int)texel[3]);
    } else {
        OSV_GLOBAL const float* texel = (OSV_GLOBAL const float*)row + (size_t)x * 4u;
        c[0] = texel[0];
        c[1] = texel[1];
        c[2] = texel[2];
        c[3] = texel[3];
    }
    if (src->isBgra) {
        rgba[0] = c[2];
        rgba[1] = c[1];
        rgba[2] = c[0];
    } else {
        rgba[0] = c[0];
        rgba[1] = c[1];
        rgba[2] = c[2];
    }
    rgba[3] = c[3];
}

/* Bilinear sample of the Standard-layout equirect at a unit body direction.
 * Longitude wraps around the +/-180 degree seam, latitude clamps at the
 * poles.  Sample centres sit at integer + 0.5 like everywhere else. */
OSV_HD void osvSampleEquirectRgba(const OsvRgbaSource* src, OSV_GLOBAL const void* pixels, const float* dBody,
                                  float* rgba) {
    const float W = (float)src->w;
    const float H = (float)src->h;
    /* Standard layout: d = (sin lon cos lat, cos lon cos lat, sin lat). */
    const float lon = atan2f(dBody[0], dBody[1]);
    const float lat = asinf(osvClampf(dBody[2], -1.0f, 1.0f));
    /* Continuous sample coordinates, then the 0.5 centre offset. */
    const float fx = (lon / OSV_KERNEL_TWO_PI + 0.5f) * W - 0.5f;
    const float fy = (0.5f - lat / OSV_KERNEL_PI) * H - 0.5f;
    const float flx = floorf(fx);
    const float fly = floorf(fy);
    const float tx = fx - flx;
    const float ty = fy - fly;
    const int x0 = (int)flx;
    const int y0 = (int)fly;
    /* Longitude wraps, latitude clamps. */
    const int xa = osvWrapIndex(x0, src->w);
    const int xb = osvWrapIndex(x0 + 1, src->w);
    const int ya = y0 < 0 ? 0 : (y0 >= src->h ? src->h - 1 : y0);
    const int yb = (y0 + 1) < 0 ? 0 : ((y0 + 1) >= src->h ? src->h - 1 : y0 + 1);
    float a[4], b[4], c[4], d[4];
    osvFetchRgba(src, pixels, xa, ya, a);
    osvFetchRgba(src, pixels, xb, ya, b);
    osvFetchRgba(src, pixels, xa, yb, c);
    osvFetchRgba(src, pixels, xb, yb, d);
    for (int i = 0; i < 4; ++i) {
        const float top = a[i] + (b[i] - a[i]) * tx;
        const float bot = c[i] + (d[i] - c[i]) * tx;
        rgba[i] = top + (bot - top) * ty;
    }
}

/* Compute output pixel (px, py) of a reframed equirect frame.  Pixels outside
 * the viewport rectangle, pixels with no ray under the projection and every
 * invalid input yield transparent black (0, 0, 0, 0).  Inside the viewport
 * `out` receives straight RGBA in whatever encoding the source carries (the
 * function is colour agnostic); alpha is the sampled alpha unless
 * fillAlphaOne is set. */
OSV_FN void osvReframeEquirectPixel(const OsvReframeParams* p, const OsvRgbaSource* src, OSV_GLOBAL const void* pixels,
                                    int px, int py, float out[4]) {
    if (!out) {
        return;
    }
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
    /* Letterbox: only the viewport receives the picture. */
    const int lx = px - p->viewX;
    const int ly = py - p->viewY;
    if (lx < 0 || ly < 0 || lx >= p->viewW || ly >= p->viewH) {
        return;
    }
    /* Centred pixel offsets inside the viewport, +ny = up. */
    const float W = (float)p->viewW;
    const float H = (float)p->viewH;
    const float nx = ((float)lx + 0.5f) - 0.5f * W;
    const float ny = 0.5f * H - ((float)ly + 0.5f);
    float dView[3];
    if (!osvViewRay(p->projection, p->focalPx, p->eyeOffset, p->tanHalfH, p->tanHalfV, W, H, nx, ny, dView)) {
        return;
    }
    /* View -> body, then look the body direction up in the panorama. */
    float dBody[3];
    osvMat3MulVec(p->Rout, dView, dBody);
    osvSampleEquirectRgba(src, pixels, dBody, out);
    if (p->fillAlphaOne) {
        out[3] = 1.0f;
    }
}

#endif /* OSV_KERNEL_H */
