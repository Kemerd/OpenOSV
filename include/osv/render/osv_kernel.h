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

/* ========================================================================= */
/*  [WP-FLARE] lens-flare removal: parameter types                           */
/* ========================================================================= */
/* The sun inside a lens's field of view leaves two kinds of stray light on
 * that lens's frame (docs/research/FLARE.md):
 *
 *   * GHOSTS - internal reflections between the lens surfaces, the sensor
 *     stack and the (ND) filter glass.  Each is a defocused image of the
 *     aperture, clipped by a rectangular stop, i.e. a soft-edged rounded
 *     rectangle of nearly flat brightness, displaced from the sun along the
 *     line through the optical centre.
 *   * VEIL - light scattered everywhere in the barrel: a near-uniform
 *     additive floor over the whole frame.
 *
 * Both are ADDITIVE in linear light, so the removal is a subtraction in the
 * lens's native scene-linear RGB, before the lens gain and the blend.  The
 * host (src/osv/render/Flare.cpp) measures them; the kernel only evaluates
 * the fitted shapes.  Everything here is in STREAM pixels of the lens it
 * belongs to, the same frame osvProjectLens returns. */

/* Ghosts the kernel carries per lens.  The sample clip shows four in the
 * sun's lens; more would cost parameter-block space for rarely-seen faint
 * reflections (see the 4 KB budget note on OSV_MAX_OCCLUSION_POINTS). */
#define OSV_FLARE_MAX_GHOSTS 4

/* One ghost: a rotated rounded rectangle with a smoothstep edge.  Its light
 * at a pixel is, per channel,
 *     amp * S + rim * E + gradX * S * u + gradY * S * v   (clamped >= 0)
 * with S the plateau weight (1 inside, smoothstep across +/- soft of the
 * edge), E a compact bump of half width 3 * soft centred ON the edge (the
 * caustic rim a defocused ghost carries), and (u, v) the local coordinates
 * scaled to +/-1 at the half extents (a brightness tilt across the ghost).
 * Measured on the sample's brightest ghost the rim and tilt terms raise the
 * explained local variance from 0.78 to 0.86 (0.61 to 0.83 on a fainter
 * one), and without them its bright rim survives the removal as an outline. */
typedef struct OsvFlareGhost {
    float cx, cy;     /* centre (stream px, continuous coordinates)            */
    float hx, hy;     /* half extents along the rotated local axes (px)        */
    float radius;     /* corner radius (px), 0 .. min(hx, hy)                  */
    float cosA, sinA; /* rotation of the local x axis in the image             */
    float soft;       /* half width of the edge ramp (px), > 0                 */
    float amp[3];     /* additive native-linear RGB on the plateau             */
    float rim[3];     /* additive RGB of the edge bump at its crest            */
    float gradX[3];   /* plateau tilt along local x (RGB at u = +1)            */
    float gradY[3];   /* plateau tilt along local y (RGB at v = +1)            */
    float reach2;     /* squared radius beyond which the ghost is exactly 0    */
} OsvFlareGhost;

/* Everything removed from one lens. */
typedef struct OsvFlareLens {
    OsvFlareGhost ghost[OSV_FLARE_MAX_GHOSTS];
    float veil[3];  /* uniform additive veil, native-linear RGB             */
    int ghostCount; /* ghosts in use, 0 .. OSV_FLARE_MAX_GHOSTS             */
} OsvFlareLens;

/* ======================= [/WP-FLARE] types ============================== */

/* [WP-SEAMTOOLS] Largest Gaussian radius (taps each side of the centre) of
 * the seam low band's blur, and so the size of OsvRenderParams::seamLowTaps.
 * 15 taps of an 8x decimated lens reach 120 lens pixels, ~8 degrees at the
 * rim of the Osmo 360's fisheye - the widest Seam Smoothing on offer. */
#define OSV_SEAM_LOW_MAX_RADIUS 15

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

    /* ---- 2-D parallax warp grid (optional) --------------------------------
     * The seam table above corrects a shift ALONG each meridian and nothing
     * else.  Real parallax is two-dimensional: a wing tip or a propeller near
     * the camera displaces content across meridians too, and a 1-D table can
     * only smear that.  The warp grid carries the missing second component.
     *
     * It is a (warpW x warpH) lattice over the polar-axis overlap band, in
     * the band's own body-frame coordinates - longitude atan2(x, z) across,
     * latitude asin(y) down - holding (dLon, dLat) in RADIANS: the
     * displacement applied to the MASTER lens (lens[1]) sampling direction.
     * The slave lens gets the negation, so the pair meets half way, the same
     * way the seam table splits its disparity between the two lenses.
     *
     * It is expressed in (lon, lat) rather than as rotations about each lens
     * axis because that is the frame the flow is MEASURED in: one band pixel
     * is one fixed (lon, lat) step, so no assumption about lens orientation
     * sits between the measurement and its application.
     *
     * Zero outside the grid's latitude span, and the host decays the field to
     * zero at both latitude edges, so a ray leaving the overlap returns
     * continuously to the uncorrected geometry rather than stepping to it. */
    int warpEnabled;         /* 1 = apply the 2-D warp grid                    */
    int warpW, warpH;        /* grid size (columns = longitude, rows = lat)    */
    float warpLatMinRad;     /* latitude of grid row 0                          */
    float warpLatMaxRad;     /* latitude of grid row warpH - 1                  */
    /* sin() of the lower and upper latitude limits of the grid, precomputed
     * on the host so the kernel can reject a ray OUTSIDE the grid with one
     * comparison on dBody[1] instead of an atan2f + asinf + two lookups.  Most
     * of a frame lies outside the grid, and on the CPU backend - where this
     * arithmetic is the whole cost - the warp as a whole measured +30-50 ms
     * on a ~300 ms 6K equirect render; there is no reason to spend any of
     * that on rays it cannot affect.  lo >= hi (e.g. a zero-initialised
     * block) disables the early-out rather than the warp. */
    float warpSinLatLo;
    float warpSinLatHi;
    OsvColorParams color;    /* colour pipeline (see ColorMath.h)              */

    /* ---- [WP-SEAM] carved blend seam (optional) ---------------------------
     * The FOV feather above cross-fades the two lenses over the whole overlap
     * band - with the default 4 degree feather both lenses sit at 50 % over
     * a ~7 degree strip.  Wherever the lenses disagree (a wing fin a few
     * centimetres from the camera, which no warp can align because each lens
     * sees a different side of it) that strip shows BOTH copies.
     *
     * The blend-seam table replaces the wide cross-fade with a seam carved by
     * dynamic programming through the places the lenses agree (SeamCarve.h):
     * per longitude column of the polar-axis layout it holds the seam
     * latitude and a feather half width, both in radians, interleaved
     * (lat, halfWidth).  Inside the overlap each lens is used on its own side
     * of that seam and the two are mixed only within the (narrow) feather.
     * Coverage still rules: where the chosen lens cannot see a direction
     * (stick occlusion, past its field of view) the other lens fills in.
     *
     * All zero (the builder's default) = the table is ignored and every
     * render is exactly what it was without it. */
    int blendSeamEnabled;      /* 1 = lens weights follow the blend-seam table */
    int blendSeamColumns;      /* longitude columns in the table (2 floats each) */
    float blendSeamEdgeRad;    /* validity ramp below thetaMax for the seam weights */
    /* ---- [/WP-SEAM] ------------------------------------------------------ */

    /* ---- [WP-FLARE] lens-flare removal (0 = off: render unchanged) ------- */
    int flareEnabled;        /* 1 = subtract flare[i] from lens i              */
    OsvFlareLens flare[2];   /* per lens, indexed like lens[]                  */
    /* ---- [/WP-FLARE] ---------------------------------------------------- */

    /* ---- [WP-PHOTO] photometric seam field (PhotoSeam.h) -------------------
     * The sky band along the seam is photometric: a lens's usable rim ends
     * before its calibrated thetaMax (and where depends on longitude), and
     * the two lenses disagree by a gain that changes with direction.  The
     * photo table fixes both, sampled at the ray's polar-axis (lon, lat):
     *
     *   gain  photoW x photoH x 3 floats, log2(L_master / L_slave) per
     *         channel over the overlap rows [photoLatMinRad, photoLatMaxRad];
     *         the slave is scaled by +half of it, the master by -half, in
     *         linear light, decayed to zero beyond the rows;
     *   rim   photoW x 2 floats after the gain, the usable rim angle per
     *         longitude and lens (radians, never above thetaMax): the FOV
     *         feather ends there instead of at thetaMax.
     *
     * All zero (the builder's default) = the table is ignored and every
     * render is exactly what it was without it. */
    int photoEnabled;           /* 1 = apply the photo table                   */
    int photoW, photoH;         /* grid (columns = longitude, rows = lat)      */
    float photoLatMinRad;       /* latitude of gain row 0                      */
    float photoLatMaxRad;       /* latitude of gain row photoH - 1             */
    float photoDecayRad;        /* luma gain: raised cosine to 0 beyond rows   */
    float photoChromaDecayRad;  /* chroma ratios decay over this instead       */
    float photoRimFeatherRad;   /* feather below the per-column rim; 0 = no rim*/
    float photoStrength;        /* gain strength 0..1; 0 = no gain             */
    float photoSinLatLo;        /* early-out: sin(lat) span the table touches  */
    float photoSinLatHi;        /*   (lo >= hi disables the early-out)         */
    float photoCodePerStop;     /* passthrough: log code units per stop        */
    /* ---- [/WP-PHOTO] ------------------------------------------------------ */

    /* ---- [WP-SEAMTOOLS] two-band seam smoothing (SeamTools.h) --------------
     * The carved seam ([WP-SEAM]) mixes the lenses inside a narrow feather,
     * so a near object is never shown twice - but where the two lenses see
     * it from different angles its copies do not line up and the cut shows
     * as a step.  Seam smoothing softens that step the way DJI's multiband
     * blend does (Burt & Adelson 1983, two bands):
     *
     *     out = blend_wide(lowA, lowB) + blend_seam(A - lowA, B - lowB)
     *
     * The LOW frequencies of both lenses mix across a wide band around the
     * seam (half width seamSmoothHalfRad), the HIGH frequencies still switch
     * inside the carved seam's own feather: colour and shading glide across,
     * fine detail stays single.
     *
     * The low band of each lens is a pre-filtered copy of its fisheye frame
     * (the "seamLow" table the renderer builds per frame, SeamTools.h):
     * seamLowFactor x seamLowFactor blocks box-averaged, decoded to linear
     * light (code values for passthrough), weighted by how much of the block
     * the lens really sees, then blurred by a separable Gaussian whose taps
     * are seamLowTaps.  RGB is stored premultiplied by that coverage (A), so
     * the lookup's division is a normalised convolution (Knutsson & Westin
     * 1993): the black outside the image circle and the selfie stick never
     * bleed into the low band.
     *
     * All zero (the builder's default) = no low band: the shader never reads
     * the table and every render is exactly what it was without it. */
    int seamSmoothEnabled;      /* 1 = two-band blend along the carved seam    */
    int seamLowFactor;          /* lens pixels per low-band pixel (even, >= 2) */
    int seamLowW, seamLowH;     /* low-band size of each lens                  */
    int seamLowRadius;          /* Gaussian taps each side, <= MAX_RADIUS      */
    float seamSmoothHalfRad;    /* low-frequency blend half width (radians)    */
    /* Gaussian tap k (|offset| = k low-band pixels), unnormalised; the blur
     * divides by their sum.  Precomputed on the host so every backend runs
     * the same numbers instead of its own expf. */
    float seamLowTaps[OSV_SEAM_LOW_MAX_RADIUS + 1];
    /* ---- [/WP-SEAMTOOLS] ----------------------------------------------------- */
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
    int pitchBytes;      /* row pitch in bytes (positive, see flipY)           */
    int isHalf;          /* 1 = 16-bit float channels, 0 = 32-bit float        */
    int isBgra;          /* 1 = channel order B,G,R,A (Premiere), 0 = R,G,B,A  */
    /* 1 = the pixel pointer addresses the LAST image row and rows ascend in
     * memory as the image descends, i.e. image row y lives at
     * pixels + (h - 1 - y) * pitchBytes.
     *
     * Why this exists: the sampler indexes rows with an UNSIGNED multiply, so
     * a negative pitch is not expressible - it would become an enormous
     * positive offset and walk off the allocation.  Premiere hands out worlds
     * whose rows genuinely descend in memory (a session log shows topDown
     * with rowBytes = -40960), and those used to be REFUSED outright, which
     * made the whole effect render nothing at all.  Pointing at the far end
     * and flipping the row index keeps every offset non-negative and costs
     * one subtraction per fetch. */
    int flipY;
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

/* ---- [WP-CAMERA] DJI sphere camera: id and forward declaration ------------
 * The projection DJI's reframe tools render with (DJI Studio and DJI's own
 * Premiere plug-in): a pinhole camera with a VERTICAL field of view, placed
 * `eyeZ` sphere radii behind the centre of the unit panorama sphere and
 * looking through it.  The sphere point it sees is the FAR intersection of
 * the pinhole ray with the sphere.  In OsvReframeParams / OsvRenderParams the
 * existing fields carry it: focalPx = the PINHOLE focal length in pixels
 * ((H / 2) / tan(vfov / 2)), eyeOffset = eyeZ (any value >= 0; above 1 the
 * eye is outside the sphere and rays that miss it are uncovered).  The
 * function itself lives in the [WP-CAMERA] region at the end of this file.
 * See docs/research/DJI_CAMERA.md for the recovery of the model. */
#define OSV_PROJ_DJI_SPHERE 4
OSV_HD int osvDjiSphereRay(float focalPx, float eyeZ, float nx, float ny, float* d);
/* ---- end [WP-CAMERA] ------------------------------------------------------ */

/* Unit view ray for a centred pixel offset (nx right, ny up, in pixels) of a
 * W x H viewport under one of the OSV_PROJ_* camera models.  Shared by the
 * fisheye stitching shader and the equirect reframe entry point so both
 * produce the same framing for the same parameters.  Returns 0 when the
 * pixel maps to no direction (outside the image circle / valid radius). */
OSV_HD int osvViewRay(int projection, float focalPx, float eyeOffset, float tanHalfH, float tanHalfV, float W,
                      float H, float nx, float ny, float* d) {
    /* [WP-CAMERA] DJI's pinhole-behind-the-sphere camera has its own ray
     * construction (a ray / sphere intersection, not a radial angle map). */
    if (projection == OSV_PROJ_DJI_SPHERE) {
        return osvDjiSphereRay(focalPx, eyeOffset, nx, ny, d);
    }
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
/*  2-D parallax warp grid                                                    */
/* ------------------------------------------------------------------------- */

/* Bilinear fetch of one component of the warp grid.
 *
 * `grid` is interleaved (dLon, dLat) pairs, warpW * warpH of them, row-major with
 * rows running from warpLatMinRad to warpLatMaxRad.  `comp` selects
 * 0 = dLon (across meridians) or 1 = dLat (along the meridian).
 *
 * Longitude WRAPS: the band is a closed ring around the sphere, and column
 * warpW - 1 is adjacent to column 0.  Clamping there instead would freeze the
 * correction across the wrap meridian and leave a visible vertical step at
 * longitude +/-180 - the one place the grid is guaranteed to be continuous in
 * the real world.  Latitude CLAMPS, because the band genuinely ends. */
OSV_HD float osvWarpSample(const OsvRenderParams* p, OSV_GLOBAL const float* grid, float lon, float lat, int comp) {
    if (p->warpW <= 0 || p->warpH <= 0 || grid == 0) {
        return 0.0f;
    }
    /* Longitude -> continuous column.  The grid spans the full 2pi, so the
     * sample position is a plain fraction of the ring. */
    float fx = ((lon + OSV_KERNEL_PI) / OSV_KERNEL_TWO_PI) * (float)p->warpW;
    /* Latitude -> continuous row across the band's own extent. */
    const float span = p->warpLatMaxRad - p->warpLatMinRad;
    if (!(span > 1e-9f) && !(span < -1e-9f)) {
        return 0.0f; /* degenerate band: no correction rather than a divide */
    }
    float fy = ((lat - p->warpLatMinRad) / span) * (float)(p->warpH - 1);

    /* Outside the band there is nothing measured.  Returning zero here is
     * what makes the decay ring the host built actually reach zero. */
    if (fy < 0.0f || fy > (float)(p->warpH - 1)) {
        return 0.0f;
    }

    const float flx = floorf(fx);
    const float fly = floorf(fy);
    int x0 = (int)flx;
    int y0 = (int)fly;
    const float tx = fx - flx;
    const float ty = fy - fly;

    /* Wrap the two longitude taps into [0, warpW). */
    int x1 = x0 + 1;
    x0 = ((x0 % p->warpW) + p->warpW) % p->warpW;
    x1 = ((x1 % p->warpW) + p->warpW) % p->warpW;
    int y1 = y0 + 1;
    if (y0 < 0) { y0 = 0; }
    if (y0 > p->warpH - 1) { y0 = p->warpH - 1; }
    if (y1 < 0) { y1 = 0; }
    if (y1 > p->warpH - 1) { y1 = p->warpH - 1; }

    const float a = grid[((y0 * p->warpW) + x0) * 2 + comp];
    const float b = grid[((y0 * p->warpW) + x1) * 2 + comp];
    const float c = grid[((y1 * p->warpW) + x0) * 2 + comp];
    const float d = grid[((y1 * p->warpW) + x1) * 2 + comp];
    const float top = a + (b - a) * tx;
    const float bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

/* Displace a body-frame direction by (dLon, dLat) in the polar-axis frame
 * (lon = atan2(x, z), lat = asin(y)) and write the unit result to `out`.
 *
 * This is how the warp grid is applied.  dLat moves along the meridian - the
 * one direction the 1-D seam table can already correct - and dLon moves
 * across it, which no 1-D table can express and which is what leaves a wing
 * tip smeared when only the meridian is corrected.
 *
 * The band sits on the equator, a few degrees either side of it, so this is
 * nowhere near the polar singularity of atan2 wherever it is actually used. */
OSV_HD void osvPolarDisplace(const float* d, float dLon, float dLat, float* out) {
    const float lon = atan2f(d[0], d[2]) + dLon;
    const float lat = asinf(osvClampf(d[1], -1.0f, 1.0f)) + dLat;
    const float cl = cosf(lat);
    out[0] = cl * sinf(lon);
    out[1] = sinf(lat);
    out[2] = cl * cosf(lon);
}

/* ------------------------------------------------------------------------- */
/*  [WP-SEAM] Carved blend seam                                               */
/* ------------------------------------------------------------------------- */
/* These helpers sit directly in front of the shader that calls them rather
 * than in a region at the end of the file: every dialect needs a function
 * defined before its first use, and a forward declaration would have to be
 * spelled three different ways (static inline, __forceinline__, OpenCL's C99
 * inline) to stay legal in all of them.  Nothing here runs unless
 * OsvRenderParams::blendSeamEnabled is set. */

/* How much lens L can be TRUSTED for a ray, independent of the blend.
 *
 * The lens weight the shader computes (osvLensWeight) is the FOV feather
 * times the occlusion factor.  The feather is a blending device - with a
 * carved seam it is exactly what must NOT decide the mix - but the occlusion
 * factor (the selfie stick) and the hard end of the field of view are facts
 * about the lens.  So the feather is divided back out, and replaced by a
 * much shorter ramp (blendSeamEdgeRad) that only keeps the very rim, where
 * the image is soft and vignetted, from switching on as a hard edge.
 *
 * `theta` is the ray's angle from the optical axis and `w` the shader's
 * lens weight for it (0 when the lens does not see the ray at all). */
OSV_HD float osvSeamVisibility(const OsvRenderParams* p, const OsvLens* L, float theta, float w) {
    if (!(w > 0.0f)) {
        return 0.0f;
    }
    /* The FOV feather osvLensWeight applied, recomputed with its own
     * expression so the division below recovers the occlusion factor. */
    float fov = 1.0f;
    if (L->featherRad > 0.0f) {
        fov = osvSmoothstep((L->thetaMax - theta) / L->featherRad);
    }
    /* At the very rim the feather is ~0 and the quotient meaningless; the
     * edge ramp below is ~0 there as well, so 1 is a safe stand-in. */
    const float occl = (fov > 1e-3f) ? osvClampf(w / fov, 0.0f, 1.0f) : 1.0f;
    float edge = 1.0f;
    if (p->blendSeamEdgeRad > 0.0f) {
        edge = osvSmoothstep((L->thetaMax - theta) / p->blendSeamEdgeRad);
    }
    return occl * edge;
}

/* Blend weights for a seam that asks for master weight `t1` (slave 1 - t1),
 * given what each lens can actually see (`vis`, from osvSeamVisibility).
 *
 *     a_i = vis_i * t_i                  what the seam asks for and gets
 *     w_i = a_i + (1 - sum a) * vis_i / sum vis
 *
 * The second term hands whatever the chosen lens cannot see to the lens that
 * can, in proportion to what it sees - so an occluded or out-of-field side
 * never leaves a hole or a dark smear, and the weights always sum to 1 and
 * vary continuously with every input.  Where both lenses see the ray fully
 * (the normal case) it is simply (1 - t1, t1). */
OSV_HD void osvSeamMix(const float* vis, float t1, float* w) {
    const float a0 = vis[0] * (1.0f - t1);
    const float a1 = vis[1] * t1;
    const float deficit = fmaxf(1.0f - (a0 + a1), 0.0f);
    const float visSum = vis[0] + vis[1];
    if (!(visSum > 0.0f)) {
        /* Defensive: the caller only gets here with both lenses visible. */
        w[0] = 1.0f - t1;
        w[1] = t1;
        return;
    }
    w[0] = a0 + deficit * (vis[0] / visSum);
    w[1] = a1 + deficit * (vis[1] / visSum);
}

/* Seam latitude and feather half width (radians) at longitude `lon`.
 *
 * The table holds one (lat, halfWidth) pair per longitude column, sampled at
 * the column CENTRES, and is interpolated linearly between them with the
 * longitude wrapping - a nearest-column lookup would draw the seam as a
 * staircase once a view is zoomed in far enough for one column to span a
 * few dozen output pixels. */
OSV_HD void osvBlendSeamLookup(const OsvRenderParams* p, OSV_GLOBAL const float* table, float lon, float* lat,
                               float* halfWidth) {
    const int n = p->blendSeamColumns;
    /* Continuous column position with centres at integer + 0.5. */
    const float fx = ((lon + OSV_KERNEL_PI) / OSV_KERNEL_TWO_PI) * (float)n - 0.5f;
    const float flx = floorf(fx);
    const float t = fx - flx;
    int c0 = (int)flx;
    int c1 = c0 + 1;
    /* Wrap both taps into [0, n): column n - 1 is adjacent to column 0. */
    c0 = ((c0 % n) + n) % n;
    c1 = ((c1 % n) + n) % n;
    const float s0 = table[c0 * 2];
    const float s1 = table[c1 * 2];
    const float h0 = table[c0 * 2 + 1];
    const float h1 = table[c1 * 2 + 1];
    *lat = s0 + (s1 - s0) * t;
    *halfWidth = h0 + (h1 - h0) * t;
}

/* Weight of the master lens for a ray at latitude `lat` of a seam at `s`
 * with feather half width `hw`: 0 at s - hw, 1 at s + hw, smoothstep in
 * between (the master lens, lens[1], owns the +Y pole, i.e. the north side
 * of the polar-axis layout).  A zero width is a hard cut. */
OSV_HD float osvSeamSide(float lat, float s, float hw) {
    if (!(hw > 0.0f)) {
        return (lat >= s) ? 1.0f : 0.0f;
    }
    return osvSmoothstep(0.5f + 0.5f * (lat - s) / hw);
}

/* Re-weight the blend of one ray by the carved seam; the shader's single
 * [WP-SEAM] call.
 *
 * `dBody` is the output ray in the body frame (where the table is defined,
 * before any seam shift or warp moves the sampling direction), `theta` each
 * lens's angle from its axis for that ray and `w` the shader's lens weights,
 * which are REPLACED by the seam's weights.  Returns their sum - the value
 * the shader normalises the colour by - or `wsum` unchanged (and `w`
 * untouched) for a ray the seam does not concern: one only a single lens
 * sees, a nearest-lens render, or no table.
 *
 * Why only rays BOTH lenses see: everywhere else one lens is all there is,
 * and the coverage weights already say so - the carved seam changes nothing
 * outside the overlap, by construction. */
OSV_HD float osvBlendSeamApply(const OsvRenderParams* p, OSV_GLOBAL const float* table, const float* dBody,
                               const float* theta, float* w, float wsum) {
    if (!p->blendSeamEnabled || table == 0 || p->blendSeamColumns <= 0 || !p->blendEnabled || !(w[0] > 0.0f) ||
        !(w[1] > 0.0f)) {
        return wsum;
    }
    /* Polar-axis longitude / latitude of the ray (see osvSeamColumn). */
    const float lon = atan2f(dBody[0], dBody[2]);
    const float lat = asinf(osvClampf(dBody[1], -1.0f, 1.0f));
    float s = 0.0f;
    float hw = 0.0f;
    osvBlendSeamLookup(p, table, lon, &s, &hw);

    /* What each lens can be trusted for, without the FOV feather, then the
     * seam's choice filled in by coverage (osvSeamMix sums to 1). */
    float vis[2];
    vis[0] = osvSeamVisibility(p, &p->lens[0], theta[0], w[0]);
    vis[1] = osvSeamVisibility(p, &p->lens[1], theta[1], w[1]);
    float seamW[2];
    osvSeamMix(vis, osvSeamSide(lat, s, hw), seamW);
    const float sum = seamW[0] + seamW[1];
    if (!(sum > 1e-6f)) {
        return wsum; /* defensive: cannot happen with both lenses visible */
    }
    w[0] = seamW[0];
    w[1] = seamW[1];
    return sum;
}
/* ---- [/WP-SEAM] ---------------------------------------------------------- */

/* ========================================================================= */
/*  [WP-FLARE] lens-flare removal: functions                                  */
/* ========================================================================= */

/* Signed distance from stream pixel (px, py) to the rounded rectangle of
 * ghost g (negative inside; Inigo Quilez's box SDF with rounded corners),
 * plus the local coordinates scaled to +/-1 at the half extents. */
OSV_HD float osvFlareGhostDistance(const OsvFlareGhost* g, float px, float py, float* u, float* v) {
    const float dx = px - g->cx;
    const float dy = py - g->cy;
    /* Into the ghost's own axes. */
    const float sx = dx * g->cosA + dy * g->sinA;
    const float sy = -dx * g->sinA + dy * g->cosA;
    *u = sx / fmaxf(g->hx, 1e-3f);
    *v = sy / fmaxf(g->hy, 1e-3f);
    const float qx = fabsf(sx) - (g->hx - g->radius);
    const float qy = fabsf(sy) - (g->hy - g->radius);
    const float ox = fmaxf(qx, 0.0f);
    const float oy = fmaxf(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - g->radius;
}

/* Plateau weight from a signed distance: 1 inside, 0 outside, a smoothstep
 * across [-soft, +soft].  soft > 0 is guaranteed by the host; the guard
 * keeps a zeroed block from dividing by zero. */
OSV_HD float osvFlarePlateau(float d, float soft) {
    const float s = fmaxf(soft, 1e-3f);
    return osvSmoothstep((s - d) / (2.0f * s));
}

/* Rim bump from a signed distance: (1 - (d / 3 soft)^2)^2 inside |d| < 3 soft
 * and exactly 0 beyond, so the ghost's footprint ends at a hard radius. */
OSV_HD float osvFlareRim(float d, float soft) {
    const float t = d / (3.0f * fmaxf(soft, 1e-3f));
    if (!(t * t < 1.0f)) {
        return 0.0f;
    }
    const float b = 1.0f - t * t;
    return b * b;
}

/* Plateau weight of one ghost at stream pixel (px, py) - the shape alone,
 * without its amplitudes.  Zero past the bounding circle. */
OSV_HD float osvFlareGhostShape(const OsvFlareGhost* g, float px, float py) {
    const float dx = px - g->cx;
    const float dy = py - g->cy;
    if (dx * dx + dy * dy > g->reach2) {
        return 0.0f;
    }
    float u, v;
    const float d = osvFlareGhostDistance(g, px, py, &u, &v);
    return osvFlarePlateau(d, g->soft);
}

/* Additive light of ghost g at stream pixel (px, py), native-linear RGB,
 * never negative.  Returns 0 (rgb untouched) past the bounding circle -
 * one compare for the overwhelming majority of pixels. */
OSV_HD int osvFlareGhostLight(const OsvFlareGhost* g, float px, float py, float* rgb) {
    const float dx = px - g->cx;
    const float dy = py - g->cy;
    if (dx * dx + dy * dy > g->reach2) {
        return 0;
    }
    float u, v;
    const float d = osvFlareGhostDistance(g, px, py, &u, &v);
    const float s = osvFlarePlateau(d, g->soft);
    const float e = osvFlareRim(d, g->soft);
    for (int c = 0; c < 3; ++c) {
        /* Light only adds: a tilt or rim that would dip below zero at one
         * end of the ghost contributes nothing there instead. */
        rgb[c] = fmaxf(g->amp[c] * s + g->rim[c] * e + (g->gradX[c] * u + g->gradY[c] * v) * s, 0.0f);
    }
    return 1;
}

/* Remove `g` units of additive light from a value `x` without ever producing
 * negative light or reversing tones.
 *
 *     f(x) = x - g * x^3 / (x^3 + g^3)
 *
 * For x >> g it is x - g (the full subtraction).  Where the estimate reaches
 * the signal itself it backs off smoothly: f(x) >= 0.47 x everywhere and
 * f'(x) >= 0.16, so an over-estimate can dim a dark pixel but never clip it
 * to black nor invert a gradient.  Non-positive inputs and estimates pass
 * through unchanged. */
OSV_HD float osvFlareSoftSubtract(float x, float g) {
    if (!(g > 1e-7f) || !(x > 0.0f)) {
        return x;
    }
    const float x3 = x * x * x;
    const float g3 = g * g * g;
    return x - g * (x3 / (x3 + g3));
}

/* Subtract the veil and every ghost of lens flare block F at stream pixel
 * (px, py) from the native-linear RGB triple `rgb` (in place). */
OSV_HD void osvFlareRemove(const OsvFlareLens* F, float px, float py, float* rgb) {
    if (F == 0 || rgb == 0) {
        return;
    }
    /* Total additive estimate per channel: the uniform veil plus every
     * ghost whose footprint covers this pixel. */
    float add[3];
    add[0] = F->veil[0];
    add[1] = F->veil[1];
    add[2] = F->veil[2];
    const int n = F->ghostCount < OSV_FLARE_MAX_GHOSTS ? F->ghostCount : OSV_FLARE_MAX_GHOSTS;
    for (int k = 0; k < n; ++k) {
        float light[3];
        if (osvFlareGhostLight(&F->ghost[k], px, py, light)) {
            add[0] += light[0];
            add[1] += light[1];
            add[2] += light[2];
        }
    }
    rgb[0] = osvFlareSoftSubtract(rgb[0], add[0]);
    rgb[1] = osvFlareSoftSubtract(rgb[1], add[1]);
    rgb[2] = osvFlareSoftSubtract(rgb[2], add[2]);
}

/* Analysis sampler: mean native-linear RGB of a 2 x 2 grid of lens pixels
 * inside the factor x factor block behind analysis pixel (ox, oy) - the
 * working image the host detects the sun and fits the ghosts on.  Each lens
 * pixel is decoded on its own (luma at the pixel, the co-sited 4:2:0 chroma
 * sample) and the linear values are averaged, because light adds in linear
 * space, not in code.
 *
 * The samples sit at offsets a = factor / 4 and b = factor - 1 - a in both
 * directions, so their mean centre is exactly the block centre (the central
 * 2 x 2 at factor 4, the whole block at factor 2, one pixel at factor 1).
 * Four decodes instead of factor^2: the per-pixel log decode is the whole
 * cost of the analysis on a CPU, and the features it looks for - a sun disc
 * and ghosts tens of pixels across on smooth sky - lose nothing to the
 * sparser sampling. */
OSV_HD void osvFlareDownsamplePixel(const OsvPlane* P, const OsvColorParams* color, int factor, int ox, int oy,
                                    float* rgb) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (P == 0 || color == 0 || factor < 1 || P->w <= 0 || P->h <= 0) {
        return;
    }
    const int step = P->chromaInterleaved ? 2 : 1;
    const int x0 = ox * factor;
    const int y0 = oy * factor;
    /* The two offsets per axis; one when they coincide (factor 1). */
    const int offA = factor / 4;
    const int offB = factor - 1 - offA;
    const int taps = (offB > offA) ? 2 : 1;
    float acc[3] = {0.0f, 0.0f, 0.0f};
    int count = 0;
    for (int j = 0; j < taps; ++j) {
        /* A partial block at the bottom / right edge: clamp the sample into
         * the frame (its in-frame part always holds at least one row and
         * column), so an edge pixel is a real mean and never an empty one. */
        int y = y0 + (j == 0 ? offA : offB);
        y = y >= P->h ? P->h - 1 : y;
        if (y < 0) {
            continue;
        }
        for (int i = 0; i < taps; ++i) {
            int x = x0 + (i == 0 ? offA : offB);
            x = x >= P->w ? P->w - 1 : x;
            if (x < 0) {
                continue;
            }
            /* 4:2:0: chroma sample (x/2, y/2) covers this luma pixel. */
            const float yv = osvFetchPlane(P->y, P->strideY, 1, P->w, P->h, x, y, P->bitShift);
            const float cb = osvFetchPlane(P->u, P->strideC, step, P->cw, P->ch, x >> 1, y >> 1, P->bitShift);
            const float cr = osvFetchPlane(P->v, P->strideC, step, P->cw, P->ch, x >> 1, y >> 1, P->bitShift);
            float code[3];
            osvYuvToCode(color, yv, cb, cr, code);
            float lin[3];
            osvCodeToLinear(color, code, lin);
            acc[0] += lin[0];
            acc[1] += lin[1];
            acc[2] += lin[2];
            ++count;
        }
    }
    if (count > 0) {
        const float inv = 1.0f / (float)count;
        rgb[0] = acc[0] * inv;
        rgb[1] = acc[1] * inv;
        rgb[2] = acc[2] * inv;
    }
}
/* ======================= [/WP-FLARE] functions ========================== */

/* ------------------------------------------------------------------------- */
/*  [WP-PHOTO] Photometric seam field                                         */
/* ------------------------------------------------------------------------- */
/* The four hooks the shader calls, in front of it for the same reason as the
 * [WP-SEAM] helpers (every dialect needs a definition before its first use).
 * The table layout and the reasoning are in include/osv/render/PhotoSeam.h
 * and docs/research/NEURAL_STITCHING.md, section 8.  In short:
 *
 *   * the WEIGHT of lens i ends at its measured usable rim for the ray's
 *     longitude instead of at thetaMax: fov_i = smoothstep((rim_i - theta_i)
 *     / photoRimFeatherRad), times the same occlusion factor as before;
 *   * the GAIN log2(master / slave) at (lon, lat) is split half and half:
 *     the slave is scaled by 2^(+half), the master by 2^(-half), in linear
 *     light (in log code units for passthrough output), and decays to zero
 *     beyond the overlap rows - chroma twice as fast as luma.
 *
 * With a carved blend seam ([WP-SEAM]) the rim-limited weights are what
 * osvSeamVisibility sees, so the seam never shows a lens past its usable rim.
 * Nothing here runs unless OsvRenderParams::photoEnabled is set. */

/* What the hooks share for one output pixel. */
typedef struct OsvPhotoPixel {
    int rimActive;   /* 1 = rim-limited weights may replace the production ones */
    int gainActive;  /* 1 = the gain applies at this pixel                      */
    float rim[2];    /* usable rim angle per lens (radians)                     */
    float halfLog2[3]; /* per channel: 0.5 x strength x decayed log2(master/slave) */
    float w[2];      /* rim-limited weight per lens                             */
} OsvPhotoPixel;

/* Raised-cosine decay by distance `dist` beyond the table rows over `range`:
 * 1 inside (dist <= 0), 0 at and beyond `range`. */
OSV_HD float osvPhotoDecay(float dist, float range) {
    if (!(dist > 0.0f)) {
        return 1.0f;
    }
    if (!(range > 0.0f)) {
        return 0.0f;
    }
    return osvSmoothstep(1.0f - dist / range);
}

/* Fill `ph` for the body ray `dBody`.  Everything position dependent is done
 * once per pixel here; the per-lens hooks below only read it. */
OSV_HD void osvPhotoBegin(const OsvRenderParams* p, OSV_GLOBAL const float* photo, const float* dBody,
                          OsvPhotoPixel* ph) {
    ph->rimActive = 0;
    ph->gainActive = 0;
    ph->rim[0] = 0.0f;
    ph->rim[1] = 0.0f;
    ph->halfLog2[0] = 0.0f;
    ph->halfLog2[1] = 0.0f;
    ph->halfLog2[2] = 0.0f;
    ph->w[0] = 0.0f;
    ph->w[1] = 0.0f;
    if (!p->photoEnabled || photo == 0 || p->photoW <= 0 || p->photoH <= 1) {
        return;
    }
    /* Most of a frame lies outside every latitude the table can touch; one
     * comparison on sin(lat) rejects it before any transcendental. */
    if (p->photoSinLatLo < p->photoSinLatHi && (dBody[1] < p->photoSinLatLo || dBody[1] > p->photoSinLatHi)) {
        return;
    }
    const int W = p->photoW;
    const int H = p->photoH;
    const float lon = atan2f(dBody[0], dBody[2]);
    const float lat = asinf(osvClampf(dBody[1], -1.0f, 1.0f));

    /* Longitude -> continuous column.  Column j sits at lon = -pi + j 2pi/W
     * and the ring WRAPS (the warp grid's convention). */
    const float fx = ((lon + OSV_KERNEL_PI) / OSV_KERNEL_TWO_PI) * (float)W;
    const float flx = floorf(fx);
    const float tx = fx - flx;
    int x0 = (int)flx;
    int x1 = x0 + 1;
    x0 = ((x0 % W) + W) % W;
    x1 = ((x1 % W) + W) % W;

    /* ---- usable rim per lens, linear in longitude ---------------------- */
    if (p->photoRimFeatherRad > 0.0f) {
        OSV_GLOBAL const float* rimT = photo + W * H * 3;
        for (int i = 0; i < 2; ++i) {
            const float a = rimT[x0 * 2 + i];
            const float b = rimT[x1 * 2 + i];
            ph->rim[i] = a + (b - a) * tx;
        }
        ph->rimActive = 1;
    }

    /* ---- gain: bilinear inside the rows, clamped and decayed outside ---- */
    const float span = p->photoLatMaxRad - p->photoLatMinRad;
    if (p->photoStrength > 0.0f && span > 1e-6f) {
        float latC = lat;
        float dist = 0.0f;
        if (lat < p->photoLatMinRad) {
            dist = p->photoLatMinRad - lat;
            latC = p->photoLatMinRad;
        } else if (lat > p->photoLatMaxRad) {
            dist = lat - p->photoLatMaxRad;
            latC = p->photoLatMaxRad;
        }
        const float kL = osvPhotoDecay(dist, p->photoDecayRad);
        const float kC = osvPhotoDecay(dist, p->photoChromaDecayRad);
        if (kL > 0.0f || kC > 0.0f) {
            const float fy = ((latC - p->photoLatMinRad) / span) * (float)(H - 1);
            const float fly = floorf(fy);
            const float ty = fy - fly;
            int y0 = (int)fly;
            if (y0 < 0) {
                y0 = 0;
            }
            if (y0 > H - 1) {
                y0 = H - 1;
            }
            const int y1 = (y0 + 1 > H - 1) ? H - 1 : y0 + 1;
            float g[3];
            for (int c = 0; c < 3; ++c) {
                const float a = photo[(y0 * W + x0) * 3 + c];
                const float b = photo[(y0 * W + x1) * 3 + c];
                const float d0 = photo[(y1 * W + x0) * 3 + c];
                const float d1 = photo[(y1 * W + x1) * 3 + c];
                const float top = a + (b - a) * tx;
                const float bot = d0 + (d1 - d0) * tx;
                g[c] = top + (bot - top) * ty;
            }
            /* G (the luma anchor) decays over photoDecayRad; the R-G and B-G
             * ratios over photoChromaDecayRad (DJI decays chroma 2x faster). */
            const float s = 0.5f * p->photoStrength;
            ph->halfLog2[1] = s * (kL * g[1]);
            ph->halfLog2[0] = s * (kL * g[1] + kC * (g[0] - g[1]));
            ph->halfLog2[2] = s * (kL * g[1] + kC * (g[2] - g[1]));
            ph->gainActive = 1;
        }
    }
}

/* Rim-limited weight of lens i: the production weight `wProd` (FOV feather
 * x occlusion, osvLensWeight) with the FOV part moved from thetaMax to the
 * lens's usable rim.  The occlusion factor is recovered as wProd / fovProd
 * rather than recomputed - the polygon walk is the costliest part of a
 * weight.  fovProd == 0 implies fovPhoto == 0 (rim <= thetaMax, host clamp),
 * so the division is never needed there. */
OSV_HD void osvPhotoLensWeight(const OsvRenderParams* p, OsvPhotoPixel* ph, int i, float theta, float wProd) {
    if (!ph->rimActive) {
        return;
    }
    const OsvLens* L = &p->lens[i];
    const float rim = ph->rim[i];
    float fovPhoto = 0.0f;
    if (!(theta > rim)) {
        fovPhoto = osvSmoothstep((rim - theta) / p->photoRimFeatherRad);
    }
    /* The production FOV factor, the exact expression osvLensWeight uses. */
    float fovProd = 1.0f;
    if (L->featherRad > 0.0f) {
        fovProd = osvSmoothstep((L->thetaMax - theta) / L->featherRad);
    }
    if (fovPhoto == fovProd) {
        ph->w[i] = wProd; /* rim == thetaMax and the same feather: production, bit for bit */
    } else if (fovProd > 1e-6f) {
        ph->w[i] = fovPhoto * (wProd / fovProd);
    } else {
        ph->w[i] = 0.0f;
    }
}

/* Replace the production weights with the rim-limited ones - unless both of
 * those vanish (a direction only a lens past its usable rim sees, e.g. next
 * to the other lens's occlusion): then the production weights stay, and the
 * shader's occlusion rescue still applies after this. */
OSV_HD void osvPhotoPickWeights(const OsvPhotoPixel* ph, float* w) {
    if (!ph->rimActive) {
        return;
    }
    if (ph->w[0] + ph->w[1] > 1e-4f) {
        w[0] = ph->w[0];
        w[1] = ph->w[1];
    }
}

/* Scale lens i's decoded sample by half the measured lens ratio: the slave
 * (i == 0) up by 2^(+half), the master (i == 1) down by 2^(-half).  In
 * passthrough the blend runs on log code values, where a gain of `half`
 * stops is an offset of half x photoCodePerStop code units. */
OSV_HD void osvPhotoApplyGain(const OsvRenderParams* p, const OsvPhotoPixel* ph, int i, int passthrough, float* val) {
    if (!ph->gainActive) {
        return;
    }
    const float sgn = (i == 1) ? -1.0f : 1.0f;
    if (passthrough) {
        for (int c = 0; c < 3; ++c) {
            val[c] += sgn * ph->halfLog2[c] * p->photoCodePerStop;
        }
        return;
    }
    for (int c = 0; c < 3; ++c) {
        val[c] *= exp2f(sgn * ph->halfLog2[c]);
    }
}
/* ---- [/WP-PHOTO] --------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/*  [WP-SEAMTOOLS] Two-band seam smoothing                                    */
/* ------------------------------------------------------------------------- */
/* The low band's build stages and its lookup, in front of the shader for the
 * same reason as the [WP-SEAM] helpers.  The table layout (one RGBA texel per
 * low-band pixel, lens 0 then lens 1, row-major, RGB premultiplied by the
 * coverage in A) and the reasoning are in OsvRenderParams above and in
 * include/osv/render/SeamTools.h.  Nothing here runs unless
 * OsvRenderParams::seamSmoothEnabled is set and a table is passed. */

/* Build stage 1: low-band texel (lx, ly) of lens `lens` from its decoded
 * planes `P`, written to `out` as (R * a, G * a, B * a, a).
 *
 * The block's samples are AVERAGED IN CODE SPACE and the mean is decoded
 * once: 64 cheap loads instead of 64 log decodes per texel, which is what
 * keeps the build a fraction of a millisecond.  That low band is not the
 * exact linear-light low-pass of the frame, but it does not have to be: the
 * shader only ever uses the DIFFERENCE of the two lenses' low bands, both
 * built by this same operator, and it is zero wherever the lenses agree.
 *
 * Coverage a = the share of the block inside the lens's usable circle
 * (normalised radius <= thetaD(thetaMax)) times the selfie-stick factor at
 * the block centre.  Only covered pixels are averaged, so a texel on the rim
 * holds the colour of the image, never of the black beyond it. */
OSV_HD void osvSeamLowDecimatePixel(const OsvRenderParams* p, const OsvPlane* P, int lens, int lx, int ly,
                                    float* out) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (lens < 0 || lens > 1 || P == 0) {
        return;
    }
    const OsvLens* L = &p->lens[lens];
    const int F = p->seamLowFactor;
    /* Defensive: an odd or tiny factor would break the 2 x 2 chroma walk,
     * a disabled lens or an empty plane has nothing to average. */
    if (!L->enabled || F < 2 || (F & 1) != 0 || P->w <= 0 || P->h <= 0 || !(L->fx > 0.0f) || !(L->fy > 0.0f)) {
        return;
    }
    /* The usable circle in normalised image coordinates. */
    const float rMax = osvThetaD(L, L->thetaMax);
    const float rMax2 = rMax * rMax;
    const float invFx = 1.0f / L->fx;
    const float invFy = 1.0f / L->fy;
    const int step = P->chromaInterleaved ? 2 : 1;
    const int x0 = lx * F;
    const int y0 = ly * F;
    float sumY = 0.0f;
    float sumCb = 0.0f;
    float sumCr = 0.0f;
    int count = 0;
    /* 2 x 2 luma quads: one chroma sample (4:2:0) per quad, weighted by how
     * many of its four luma pixels are inside the circle. */
    for (int qy = 0; qy < F; qy += 2) {
        for (int qx = 0; qx < F; qx += 2) {
            float quadY = 0.0f;
            int n = 0;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const int x = x0 + qx + i;
                    const int y = y0 + qy + j;
                    if (x >= P->w || y >= P->h) {
                        continue; /* a partial block at the right / bottom edge */
                    }
                    const float nx = ((float)x + 0.5f - L->cx) * invFx;
                    const float ny = ((float)y + 0.5f - L->cy) * invFy;
                    if (nx * nx + ny * ny > rMax2) {
                        continue; /* outside the usable circle: not image data */
                    }
                    quadY += osvFetchPlane(P->y, P->strideY, 1, P->w, P->h, x, y, P->bitShift);
                    ++n;
                }
            }
            if (n == 0) {
                continue;
            }
            const int cx = (x0 + qx) >> 1;
            const int cy = (y0 + qy) >> 1;
            const float cb = osvFetchPlane(P->u, P->strideC, step, P->cw, P->ch, cx, cy, P->bitShift);
            const float cr = osvFetchPlane(P->v, P->strideC, step, P->cw, P->ch, cx, cy, P->bitShift);
            sumY += quadY;
            sumCb += cb * (float)n;
            sumCr += cr * (float)n;
            count += n;
        }
    }
    if (count == 0) {
        return; /* nothing of the lens here: coverage 0, colour 0 */
    }
    /* Decode the block mean once, into the space the shader blends in. */
    const float inv = 1.0f / (float)count;
    float code[3];
    osvYuvToCode(&p->color, sumY * inv, sumCb * inv, sumCr * inv, code);
    float val[3];
    if (p->color.transfer == OSV_TRANSFER_PASSTHROUGH) {
        val[0] = code[0];
        val[1] = code[1];
        val[2] = code[2];
    } else {
        osvCodeToLinear(&p->color, code, val);
    }
    /* Coverage: circle share times the stick at the block centre. */
    const float share = (float)count / (float)(F * F);
    const float stick = osvOcclusionFactor(L, (float)x0 + 0.5f * (float)F, (float)y0 + 0.5f * (float)F);
    const float a = share * stick;
    out[0] = val[0] * a;
    out[1] = val[1] * a;
    out[2] = val[2] * a;
    out[3] = a;
}

/* Build stages 2 and 3: one texel of the separable Gaussian blur of lens
 * `lens`'s low band `src` (the whole two-lens table), along x when
 * `horizontal` is non-zero, along y otherwise.  Clamp-to-edge addressing;
 * the taps are normalised by their own sum, so the blur never changes the
 * mean (and the premultiplied RGB / A ratio survives it exactly). */
OSV_HD void osvSeamLowBlurPixel(const OsvRenderParams* p, OSV_GLOBAL const float* src, int lens, int x, int y,
                                int horizontal, float* out) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    const int W = p->seamLowW;
    const int H = p->seamLowH;
    if (src == 0 || W <= 0 || H <= 0 || lens < 0 || lens > 1 || x < 0 || y < 0 || x >= W || y >= H) {
        return;
    }
    int R = p->seamLowRadius;
    R = R < 0 ? 0 : (R > OSV_SEAM_LOW_MAX_RADIUS ? OSV_SEAM_LOW_MAX_RADIUS : R);
    OSV_GLOBAL const float* base = src + (size_t)lens * (size_t)W * (size_t)H * 4u;
    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wsum = 0.0f;
    for (int k = -R; k <= R; ++k) {
        int xx = x;
        int yy = y;
        if (horizontal) {
            xx = x + k;
            xx = xx < 0 ? 0 : (xx >= W ? W - 1 : xx);
        } else {
            yy = y + k;
            yy = yy < 0 ? 0 : (yy >= H ? H - 1 : yy);
        }
        const float t = p->seamLowTaps[k < 0 ? -k : k];
        OSV_GLOBAL const float* s = base + ((size_t)yy * (size_t)W + (size_t)xx) * 4u;
        acc[0] += t * s[0];
        acc[1] += t * s[1];
        acc[2] += t * s[2];
        acc[3] += t * s[3];
        wsum += t;
    }
    if (!(wsum > 0.0f)) {
        return; /* all-zero taps: a malformed block, never a division by zero */
    }
    const float inv = 1.0f / wsum;
    out[0] = acc[0] * inv;
    out[1] = acc[1] * inv;
    out[2] = acc[2] * inv;
    out[3] = acc[3] * inv;
}

/* The low band of lens `lens` at lens pixel (px, py) (the coordinates
 * osvProjectLens returned), bilinear, un-premultiplied into `rgb`.  Returns
 * the coverage there; 0 (and rgb 0) when the lens has no image data nearby,
 * which the shader answers by leaving that pixel single-band. */
OSV_HD float osvSeamLowSample(const OsvRenderParams* p, OSV_GLOBAL const float* low, int lens, float px, float py,
                              float* rgb) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    const int W = p->seamLowW;
    const int H = p->seamLowH;
    if (low == 0 || W <= 0 || H <= 0 || p->seamLowFactor < 2 || lens < 0 || lens > 1) {
        return 0.0f;
    }
    /* Low-band pixel j covers lens pixels [jF, jF + F) and is centred on
     * lens coordinate (j + 0.5) F, so the continuous low-band coordinate is
     * px / F with centres at integer + 0.5 - the osvBilinear convention. */
    const float F = (float)p->seamLowFactor;
    const float fx = px / F - 0.5f;
    const float fy = py / F - 0.5f;
    const float flx = floorf(fx);
    const float fly = floorf(fy);
    const float tx = fx - flx;
    const float ty = fy - fly;
    int x0 = (int)flx;
    int y0 = (int)fly;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : (x0 >= W ? W - 1 : x0);
    x1 = x1 < 0 ? 0 : (x1 >= W ? W - 1 : x1);
    y0 = y0 < 0 ? 0 : (y0 >= H ? H - 1 : y0);
    y1 = y1 < 0 ? 0 : (y1 >= H ? H - 1 : y1);
    OSV_GLOBAL const float* base = low + (size_t)lens * (size_t)W * (size_t)H * 4u;
    float v[4];
    for (int c = 0; c < 4; ++c) {
        const float a = base[((size_t)y0 * (size_t)W + (size_t)x0) * 4u + (size_t)c];
        const float b = base[((size_t)y0 * (size_t)W + (size_t)x1) * 4u + (size_t)c];
        const float d0 = base[((size_t)y1 * (size_t)W + (size_t)x0) * 4u + (size_t)c];
        const float d1 = base[((size_t)y1 * (size_t)W + (size_t)x1) * 4u + (size_t)c];
        const float top = a + (b - a) * tx;
        const float bot = d0 + (d1 - d0) * tx;
        v[c] = top + (bot - top) * ty;
    }
    /* Below a thousandth of a covered texel the ratio is noise, not colour. */
    if (!(v[3] > 1e-3f)) {
        return 0.0f;
    }
    const float inv = 1.0f / v[3];
    rgb[0] = v[0] * inv;
    rgb[1] = v[1] * inv;
    rgb[2] = v[2] * inv;
    return v[3];
}

/* The low band's blend weights for one ray: the carved seam's own mix
 * (osvSeamMix over osvSeamVisibility) with the feather widened to
 * max(seamSmoothHalfRad, the seam's half width), normalised to sum 1.
 * `theta` and `wPre` are each lens's angle and the shader's weights BEFORE
 * the carved seam re-weighted them.  Returns 0 - leave the pixel single-band
 * - exactly where osvBlendSeamApply leaves it alone too, and when smoothing
 * is off.  Beyond the widened feather both mixes saturate to the same
 * values, so the two bands' weights agree and the pixel is unchanged. */
OSV_HD int osvSeamSmoothWeights(const OsvRenderParams* p, OSV_GLOBAL const float* table, const float* dBody,
                                const float* theta, const float* wPre, float* wl) {
    wl[0] = wl[1] = 0.0f;
    if (!p->seamSmoothEnabled || !p->blendSeamEnabled || table == 0 || p->blendSeamColumns <= 0 ||
        !p->blendEnabled || !(wPre[0] > 0.0f) || !(wPre[1] > 0.0f)) {
        return 0;
    }
    /* The same polar-axis position and seam lookup as osvBlendSeamApply. */
    const float lon = atan2f(dBody[0], dBody[2]);
    const float lat = asinf(osvClampf(dBody[1], -1.0f, 1.0f));
    float s = 0.0f;
    float hw = 0.0f;
    osvBlendSeamLookup(p, table, lon, &s, &hw);
    const float hwWide = fmaxf(p->seamSmoothHalfRad, hw);
    float vis[2];
    vis[0] = osvSeamVisibility(p, &p->lens[0], theta[0], wPre[0]);
    vis[1] = osvSeamVisibility(p, &p->lens[1], theta[1], wPre[1]);
    osvSeamMix(vis, osvSeamSide(lat, s, hwWide), wl);
    const float sum = wl[0] + wl[1];
    if (!(sum > 1e-6f)) {
        return 0; /* defensive: cannot happen with both lenses visible */
    }
    wl[0] = wl[0] / sum;
    wl[1] = wl[1] / sum;
    return 1;
}

/* Put a low-band sample of lens i through exactly what its full sample goes
 * through after the decode - the flare removal, the lens gain, the photo
 * gain - so the two bands of one lens stay in one space and the difference
 * the shader adds is a difference of like with like. */
OSV_HD void osvSeamLowShade(const OsvRenderParams* p, const OsvPhotoPixel* photoPx, int i, int passthrough,
                            float px, float py, float* val) {
    if (!passthrough) {
        if (p->flareEnabled) {
            osvFlareRemove(&p->flare[i], px, py, val);
        }
        val[0] *= p->lens[i].gain[0];
        val[1] *= p->lens[i].gain[1];
        val[2] *= p->lens[i].gain[2];
    }
    osvPhotoApplyGain(p, photoPx, i, passthrough, val);
}
/* ---- [/WP-SEAMTOOLS] ----------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/*  The shader                                                                */
/* ------------------------------------------------------------------------- */

/* Compute output pixel (x, y).  `planes` holds the two lens frames, `seam`
 * the optional per-column seam shift table in degrees (may be 0 when
 * seamShiftEnabled == 0), `warp` the optional 2-D parallax grid (may be 0
 * when warpEnabled == 0), `blendSeam` the optional carved blend-seam table
 * (may be 0 when blendSeamEnabled == 0), `photo` the optional photometric
 * seam table (may be 0 when photoEnabled == 0), `seamLow` the optional
 * two-lens low-band table of the seam smoothing (may be 0 when
 * seamSmoothEnabled == 0).  `out` receives R, G, B in the output encoding and
 * A = coverage (or 1).  Pixels seen by neither lens are (0,0,0,0).
 *
 * The ONE full entry point: every table the kernel knows.  The older names
 * below (osvShadePixelWSP, osvShadePixelWS, osvShadePixelW, osvShadePixel)
 * are thin wrappers passing null for the tables they predate. */
OSV_HD void osvShadePixelWSPL(const OsvRenderParams* p, const OsvPlane* planes, OSV_GLOBAL const float* seam,
                              OSV_GLOBAL const float* warp, OSV_GLOBAL const float* blendSeam,
                              OSV_GLOBAL const float* photo, OSV_GLOBAL const float* seamLow, int x, int y,
                              float* out) {
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

    /* Optional 2-D parallax warp: (dLon, dLat) for the master lens at this
     * ray's polar-axis position; the slave lens takes the negation below.
     * Sampled once per pixel, at the OUTPUT ray, because the grid describes
     * where the two lenses disagree about what belongs at this direction.
     * The grid already holds HALF the measured disparity, so nothing is
     * halved here. */
    float warpLon = 0.0f;
    float warpLat = 0.0f;
    const int warpInSpan = (p->warpSinLatLo >= p->warpSinLatHi) ||
                           (dBody[1] >= p->warpSinLatLo && dBody[1] <= p->warpSinLatHi);
    if (p->warpEnabled && warp != 0 && p->warpW > 0 && p->warpH > 0 && warpInSpan) {
        const float lon = atan2f(dBody[0], dBody[2]);
        const float lat = asinf(osvClampf(dBody[1], -1.0f, 1.0f));
        warpLon = osvWarpSample(p, warp, lon, lat, 0);
        warpLat = osvWarpSample(p, warp, lon, lat, 1);
    }

    /* [WP-PHOTO] per-pixel rim and gain lookup (a no-op when photoEnabled == 0) */
    OsvPhotoPixel photoPx;
    osvPhotoBegin(p, photo, dBody, &photoPx);

    /* Project into both lenses and compute their weights. */
    float w[2] = {0.0f, 0.0f};
    float px[2] = {0.0f, 0.0f};
    float py[2] = {0.0f, 0.0f};
    /* Whether osvProjectLens actually wrote px/py for this lens.  It returns
     * early - leaving them untouched - for a ray past thetaMax, so a rescue
     * that read px/py without checking this would sample pixel (0, 0). */
    int projected[2] = {0, 0};
    /* [WP-SEAM] each lens's angle from its axis, for the carved seam's
     * visibility (osvSeamVisibility); unused without a blend-seam table. */
    float thetaL[2] = {0.0f, 0.0f};
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
        /* 2-D parallax warp, applied on top of the 1-D seam shift.
         *
         * Master (i == 1) moves by +(dLon, dLat), slave (i == 0) by the
         * negation, so the two sampling directions separate by the full
         * measured disparity and the content they fetch meets in the middle.
         *
         * The sign really does flip here, unlike the seam shift above.  The
         * seam shift is expressed relative to EACH lens's own axis, and the
         * two axes point in opposite directions, so one shared "toward the
         * axis" angle is already an opposite move on the sphere.  The warp is
         * expressed in one shared (lon, lat) frame, where opposite moves need
         * opposite signs.  Mixing the two conventions turns the correction
         * into a rigid shift of BOTH lenses the same way, which moves the
         * picture without closing any disparity at all. */
        if (warpLon != 0.0f || warpLat != 0.0f) {
            const float sgn = (i == 1) ? 1.0f : -1.0f;
            float tmp[3];
            osvPolarDisplace(dl, sgn * warpLon, sgn * warpLat, tmp);
            dl[0] = tmp[0];
            dl[1] = tmp[1];
            dl[2] = tmp[2];
        }
        float theta;
        if (osvProjectLens(L, dl, &px[i], &py[i], &theta)) {
            w[i] = osvLensWeight(L, theta, px[i], py[i]);
            projected[i] = 1;
            thetaL[i] = theta;
            osvPhotoLensWeight(p, &photoPx, i, theta, w[i]); /* [WP-PHOTO] rim-limited twin of w[i] */
        }
    }
    osvPhotoPickWeights(&photoPx, w); /* [WP-PHOTO] rim-limited weights unless both vanish */

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

    /* [WP-SEAMTOOLS] the weights BEFORE the carved seam replaces them: the
     * low band's wide mix is computed from the same visibility they carry.
     * A copy only - nothing below reads it unless smoothing is on. */
    const float wPre[2] = {w[0], w[1]};

    /* [WP-SEAM] carved seam: re-weights the blend; alpha keeps the coverage. */
    const float coverage = wsum;
    wsum = osvBlendSeamApply(p, blendSeam, dBody, thetaL, w, wsum);

    /* [WP-SEAMTOOLS] the low band's own (wide) weights, when this ray is one
     * the carved seam re-weighted and a low-band table was built. */
    float wLow[2] = {0.0f, 0.0f};
    const int smooth = (seamLow != 0) ? osvSeamSmoothWeights(p, blendSeam, dBody, thetaL, wPre, wLow) : 0;

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
            /* [WP-FLARE] subtract this lens's measured ghosts and veil in
             * its own native linear light, before gain and blend. */
            if (p->flareEnabled) {
                osvFlareRemove(&p->flare[i], px[i], py[i], val);
            }
            /* [/WP-FLARE] */
            val[0] *= p->lens[i].gain[0];
            val[1] *= p->lens[i].gain[1];
            val[2] *= p->lens[i].gain[2];
        }
        osvPhotoApplyGain(p, &photoPx, i, passthrough, val); /* [WP-PHOTO] half the lens ratio each way */
        acc[0] += val[0] * w[i];
        acc[1] += val[1] * w[i];
        acc[2] += val[2] * w[i];
    }
    acc[0] /= wsum;
    acc[1] /= wsum;
    acc[2] /= wsum;

    /* [WP-SEAMTOOLS] Two-band seam smoothing.  With both mixes normalised,
     *     sum_i wSeam_i (val_i - low_i) + sum_i wLow_i low_i
     *   = acc + d1 (low_1 - low_0),   d1 = wLow_1 - wSeam_1,
     * so the high band stays exactly the carved seam's single-lens blend and
     * only the lenses' low-band DIFFERENCE is glided across the wide band.
     * Beyond the widened feather d1 is exactly 0 and the pixel untouched. */
    if (smooth) {
        const float d1 = wLow[1] - w[1] / wsum;
        if (d1 != 0.0f) {
            float low0[3];
            float low1[3];
            const float cov0 = osvSeamLowSample(p, seamLow, 0, px[0], py[0], low0);
            const float cov1 = osvSeamLowSample(p, seamLow, 1, px[1], py[1], low1);
            /* A lens with no image data nearby contributes no difference:
             * the pixel stays single-band rather than inventing colour. */
            if (cov0 > 0.0f && cov1 > 0.0f) {
                osvSeamLowShade(p, &photoPx, 0, passthrough, px[0], py[0], low0);
                osvSeamLowShade(p, &photoPx, 1, passthrough, px[1], py[1], low1);
                for (int c = 0; c < 3; ++c) {
                    acc[c] += d1 * (low1[c] - low0[c]);
                    /* Linear light cannot go negative; a dark detail under a
                     * bright neighbour's low band could, briefly. */
                    if (!passthrough) {
                        acc[c] = fmaxf(acc[c], 0.0f);
                    }
                }
            }
        }
    }

    if (passthrough) {
        out[0] = acc[0];
        out[1] = acc[1];
        out[2] = acc[2];
    } else {
        osvLinearToOutput(&p->color, acc, out);
    }
    out[3] = p->outputAlphaCoverage ? fminf(coverage, 1.0f) : 1.0f; /* [WP-SEAM] coverage */
}

/* [WP-SEAMTOOLS] The entry point from before the seam smoothing's low band:
 * shade with every other table and no low band - bit for bit the shader as
 * it was, since nothing of the two-band path runs without the table. */
OSV_HD void osvShadePixelWSP(const OsvRenderParams* p, const OsvPlane* planes, OSV_GLOBAL const float* seam,
                             OSV_GLOBAL const float* warp, OSV_GLOBAL const float* blendSeam,
                             OSV_GLOBAL const float* photo, int x, int y, float* out) {
    osvShadePixelWSPL(p, planes, seam, warp, blendSeam, photo, (OSV_GLOBAL const float*)0, x, y, out);
}

/* [WP-PHOTO] The entry point from before the photometric seam table: shade
 * with every other table and no photo table. */
OSV_HD void osvShadePixelWS(const OsvRenderParams* p, const OsvPlane* planes, OSV_GLOBAL const float* seam,
                            OSV_GLOBAL const float* warp, OSV_GLOBAL const float* blendSeam, int x, int y,
                            float* out) {
    osvShadePixelWSP(p, planes, seam, warp, blendSeam, (OSV_GLOBAL const float*)0, x, y, out);
}

/* [WP-SEAM] The entry point every caller used before the carved seam
 * existed: shade with no blend-seam table.  Kept under its old name so the
 * band analyses (which render each lens ALONE and must never see a seam) and
 * every existing call site keep their exact behaviour and signature. */
OSV_HD void osvShadePixelW(const OsvRenderParams* p, const OsvPlane* planes, OSV_GLOBAL const float* seam,
                           OSV_GLOBAL const float* warp, int x, int y, float* out) {
    osvShadePixelWS(p, planes, seam, warp, (OSV_GLOBAL const float*)0, x, y, out);
}

/* Back-compatible entry point: shade with no 2-D warp grid.
 *
 * Kept as a distinct symbol rather than folded into osvShadePixelW with a
 * null argument at every call site, because the OpenCL kernel and the parity
 * tests both call this name and a signature change there would ripple into
 * source strings compiled at runtime. */
OSV_HD void osvShadePixel(const OsvRenderParams* p, const OsvPlane* planes, OSV_GLOBAL const float* seam, int x,
                          int y, float* out) {
    osvShadePixelW(p, planes, seam, (OSV_GLOBAL const float*)0, x, y, out);
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
    /* With flipY the pointer is at the last image row, so walking DOWN the
     * image walks UP in memory.  The index stays non-negative either way,
     * which is what the unsigned multiply below requires. */
    const int memoryRow = src->flipY ? (src->h - 1 - y) : y;
    OSV_GLOBAL const unsigned char* row =
        (OSV_GLOBAL const unsigned char*)pixels + (size_t)memoryRow * (size_t)src->pitchBytes;
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

/* ========================================================================= */
/*  [WP-CAMERA] DJI sphere camera                                            */
/* ========================================================================= */

/* View ray of DJI's reframe camera for a centred pixel offset (nx right,
 * ny up, pixels).
 *
 * THE MODEL (matching DJI Studio and DJI's Premiere plug-in, see
 * docs/research/DJI_CAMERA.md): the panorama is a unit sphere at the origin;
 * the camera is an ordinary pinhole with focal length `focalPx` (pixels),
 * looking along +Y (view frame: X right, Y forward, Z up), with its eye at
 * E = (0, -eyeZ, 0), i.e. eyeZ radii BEHIND the centre.  A pixel sees the
 * point where its pinhole ray leaves the sphere - the far root of
 * |E + t u| = 1 - which is also the only face DJI's renderer keeps (it culls
 * the faces seen from outside).  The returned direction is that sphere
 * point, which on a unit sphere is already the direction from the centre.
 *
 *   eyeZ = 0      plain rectilinear (pinhole at the centre);
 *   eyeZ = 1      stereographic (DJI's "Asteroid");
 *   eyeZ > 1      the eye is OUTSIDE the sphere ("Crystal Ball"): rays that
 *                 miss it are uncovered and the function returns 0.
 *
 * For eyeZ <= 1 this is the same map as OSV_PROJ_EYE_OFFSET with
 * f_eye = focalPx / (1 + eyeZ); it is written as an intersection instead of
 * an angle inversion because that form needs no trigonometry, stays exact
 * past eyeZ = 1 and is parameterised the way DJI's controls are (a pinhole
 * field of view, not a visible angle).
 *
 * Numerics: sin^2 of the ray's angle is formed as (nx^2 + ny^2) / |ray|^2
 * rather than 1 - cos^2, so the discriminant keeps full precision near the
 * view axis; the result is renormalised to absorb the last ulp of drift. */
OSV_HD int osvDjiSphereRay(float focalPx, float eyeZ, float nx, float ny, float* d) {
    /* Every comparison is written so a NaN fails it. */
    if (!(focalPx > 0.0f) || !(eyeZ >= 0.0f)) {
        return 0;
    }
    const float r2 = nx * nx + ny * ny;
    const float len2 = r2 + focalPx * focalPx;
    if (!(len2 > 0.0f)) {
        return 0;
    }
    const float invLen = 1.0f / sqrtf(len2);
    /* Unit pinhole ray. */
    const float ux = nx * invLen;
    const float uy = focalPx * invLen;
    const float uz = ny * invLen;
    /* |E + t u|^2 = 1 with E = (0, -e, 0):
     *   t^2 - 2 e uy t + (e^2 - 1) = 0
     *   t = e uy + sqrt(1 - e^2 sin^2(alpha)),  sin^2(alpha) = r2 / len2. */
    const float sin2 = r2 / len2;
    const float disc = 1.0f - eyeZ * eyeZ * sin2;
    if (!(disc >= 0.0f)) {
        return 0; /* eye outside the sphere and the ray passes it by */
    }
    const float t = eyeZ * uy + sqrtf(disc);
    if (!(t > 0.0f)) {
        return 0;
    }
    d[0] = t * ux;
    d[1] = t * uy - eyeZ;
    d[2] = t * uz;
    osvNormalize3(d);
    return 1;
}

/* ---- end [WP-CAMERA] ------------------------------------------------------ */

#endif /* OSV_KERNEL_H */
