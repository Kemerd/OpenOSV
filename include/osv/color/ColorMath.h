// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Per-pixel colour math shared by the CPU reference renderer and the GPU
// kernels (CUDA / OpenCL).  Everything in this header is deliberately written
// in a plain C-style subset:
//
//   * no STL, no std:: names, no templates, no exceptions, no references,
//   * only float arithmetic and the math intrinsics
//       sqrtf atan2f sinf cosf exp2f log2f logf powf fminf fmaxf floorf fabsf,
//   * small POD structs with float / int members and free functions only,
//   * every function is marked OSV_HD, which expands to `inline` on the host
//     and to `__host__ __device__ inline` when the render module compiles it
//     with nvcc (it defines OSV_HD before including this file).
//
// The pipeline implemented here (see docs/COLOR.md):
//
//   YCbCr (narrow, 10-bit) --osvYuvToCode--> R'G'B' code   [0,1]
//   R'G'B' code            --osvCodeToLinear--> scene-linear native (0.18 = grey)
//   scene-linear native    --osvLinearToOutput--> encoded output (HLG/PQ/709/...)
//
// osvCodeToOutput() chains the last two steps and also honours the
// Passthrough transfer, which returns the code unchanged.
#ifndef OSV_COLOR_COLORMATH_H
#define OSV_COLOR_COLORMATH_H

// Host build: pull the C math intrinsics.  nvcc provides them as builtins and
// the OpenCL wrapper maps them onto the OpenCL C equivalents with macros.
#if !defined(__CUDACC__) && !defined(__OPENCL_VERSION__)
#include <math.h>
#endif

// The render module defines OSV_HD as `__host__ __device__ inline` for CUDA.
#ifndef OSV_HD
#define OSV_HD inline
#endif

/* ---------------------------------------------------------------------------
 *  Constants
 * ------------------------------------------------------------------------- */

/** BT.2100 HLG OETF constants. */
#define OSV_HLG_A 0.17883277f
#define OSV_HLG_B 0.28466892f
#define OSV_HLG_C 0.55991073f

/** SMPTE ST 2084 (PQ) constants. */
#define OSV_PQ_M1 0.1593017578125f
#define OSV_PQ_M2 78.84375f
#define OSV_PQ_C1 0.8359375f
#define OSV_PQ_C2 18.8515625f
#define OSV_PQ_C3 18.6875f

/** PQ reference peak in nits (code 1.0). */
#define OSV_PQ_PEAK_NITS 10000.0f

/** BT.2020 luminance weights used by the HLG OOTF. */
#define OSV_BT2020_LUMA_R 0.2627f
#define OSV_BT2020_LUMA_G 0.6780f
#define OSV_BT2020_LUMA_B 0.0593f

/** log2(e): exp(x) == exp2f(x * OSV_LOG2E). */
#define OSV_LOG2E 1.4426950408889634f

/** Input encodings understood by osvCodeToLinear. */
#define OSV_INPUT_DLOGM 0
#define OSV_INPUT_HLG 1
#define OSV_INPUT_REC709_NORMAL 2

/** Output transfer functions understood by osvLinearToOutput. */
#define OSV_TRANSFER_HLG 0
#define OSV_TRANSFER_PQ 1
#define OSV_TRANSFER_REC709 2
#define OSV_TRANSFER_LINEAR 3
#define OSV_TRANSFER_PASSTHROUGH 4

/** Cut modes for OsvDlogMCurve::cutMode. */
#define OSV_DLOGM_CUT_GIVEN 0
#define OSV_DLOGM_CUT_INTERSECTION 1

/* ---------------------------------------------------------------------------
 *  POD types
 * ------------------------------------------------------------------------- */

/**
 * @brief D-Log M -> scene-linear curve parameters (seven-parameter form).
 *
 * lin(code) = piecewise(tmp) * midGrayScaling with
 *   tmp = 2^(scale * code + yShift) + xShift
 *   piecewise(tmp) = tmp < cut ? tmp * slope + intercept : tmp * slope2
 *
 * With cutMode == OSV_DLOGM_CUT_INTERSECTION the stored `cut` is ignored and
 * the branch intersection intercept / (slope2 - slope) is used, which makes
 * the curve C0 continuous by construction.
 */
typedef struct OsvDlogMCurve {
    float xShift;          /**< Added after the power of two. */
    float yShift;          /**< Added to the exponent. */
    float scale;           /**< Exponent slope (stops per unit code). */
    float slope;           /**< Toe branch slope. */
    float slope2;          /**< Main branch slope. */
    float intercept;       /**< Toe branch intercept. */
    float midGrayScaling;  /**< Final multiplier so that grey maps to 0.18. */
    float cut;             /**< Branch switch point (used when cutMode == 0). */
    int cutMode;           /**< OSV_DLOGM_CUT_GIVEN or OSV_DLOGM_CUT_INTERSECTION. */
} OsvDlogMCurve;

/** @brief Row-major 3x3 float matrix (m[row * 3 + col]). */
typedef struct OsvMat3f {
    float m[9];
} OsvMat3f;

/**
 * @brief Everything the per-pixel colour shader needs, packed by value.
 *
 * Built on the host by osv::color::makeColorParams(); the kernels receive it
 * as part of their by-value parameter block and never mutate it.
 */
typedef struct OsvColorParams {
    int enabled;                  /**< 0: every function copies input to output. */
    int inputEncoding;            /**< OSV_INPUT_DLOGM / OSV_INPUT_HLG / OSV_INPUT_REC709_NORMAL. */
    OsvDlogMCurve curve;          /**< Log decode curve (used when inputEncoding == DLogM). */
    OsvMat3f nativeToWorking;     /**< Camera native primaries -> working (Rec.2020) primaries. */
    OsvMat3f workingToOutput;     /**< Working -> output primaries (identity for HLG/PQ). */
    float sceneScale;             /**< Scene-linear -> HLG/PQ scene scale (BT.2408: 0.2674). */
    float exposureGain;           /**< 2^stops applied in linear light. */
    int transfer;                 /**< OSV_TRANSFER_* output encoding. */
    float peakNits;               /**< Display peak for the PQ/Rec.709 OOTF (1000). */
    float ootfGamma;              /**< HLG OOTF system gamma (1.2 at 1000 nit). */
    float sdrPeakNits;            /**< Rec.709 target peak for the BT.2390 EETF (100). */
    float yuvBlack;               /**< Luma black code (64 for narrow 10-bit). */
    float yuvScaleY;              /**< 1 / luma range (1/876 for narrow 10-bit). */
    float yuvScaleC;              /**< 1 / chroma range (1/896 for narrow 10-bit). */
    float yuvToRgb[9];            /**< Row-major Y'CbCr -> R'G'B' matrix on normalised values. */
    int bitDepth;                 /**< Sample bit depth of the YCbCr input (chroma centre = 2^(bitDepth-1)). */
} OsvColorParams;

/* ---------------------------------------------------------------------------
 *  Small helpers
 * ------------------------------------------------------------------------- */

/** @brief Clamp v to [lo, hi]. */
OSV_HD float osvClampf(float v, float lo, float hi) {
    return fminf(fmaxf(v, lo), hi);
}

/** @brief Clamp v to [0, 1]. */
OSV_HD float osvSaturatef(float v) {
    return fminf(fmaxf(v, 0.0f), 1.0f);
}

/** @brief Natural exponential expressed through exp2f (the allowed intrinsic). */
OSV_HD float osvExpf(float x) {
    return exp2f(x * OSV_LOG2E);
}

/** @brief Apply a row-major 3x3 matrix to (r, g, b); out may alias nothing. */
OSV_HD void osvMat3Apply(const OsvMat3f* mat, float r, float g, float b, float out[3]) {
    /* Defensive: a null matrix is treated as identity so callers never crash. */
    if (mat == 0) {
        out[0] = r;
        out[1] = g;
        out[2] = b;
        return;
    }
    out[0] = mat->m[0] * r + mat->m[1] * g + mat->m[2] * b;
    out[1] = mat->m[3] * r + mat->m[4] * g + mat->m[5] * b;
    out[2] = mat->m[6] * r + mat->m[7] * g + mat->m[8] * b;
}

/* ---------------------------------------------------------------------------
 *  D-Log M curve
 * ------------------------------------------------------------------------- */

/**
 * @brief Effective branch switch point of a curve.
 *
 * For OSV_DLOGM_CUT_INTERSECTION this is intercept / (slope2 - slope), the
 * value of tmp where both branches agree.  When the slopes are equal (no
 * intersection) the stored cut is returned instead.
 */
OSV_HD float osvDlogmCut(const OsvDlogMCurve* curve) {
    if (curve == 0) {
        return 0.0f;
    }
    if (curve->cutMode == OSV_DLOGM_CUT_INTERSECTION) {
        const float denom = curve->slope2 - curve->slope;
        /* Parallel branches never intersect: fall back to the stored value. */
        if (fabsf(denom) > 1e-12f) {
            return curve->intercept / denom;
        }
    }
    return curve->cut;
}

/**
 * @brief D-Log M code (0..1, may exceed slightly) -> scene-linear (grey 0.18).
 *
 * The result is not clamped: inputs above 1.0 keep extrapolating and a
 * mis-configured curve can go negative.  Callers that need positive light
 * clamp afterwards.
 */
OSV_HD float osvDlogmToLinear(const OsvDlogMCurve* curve, float code) {
    if (curve == 0) {
        return code;
    }
    /* Exponential part of the log curve. */
    const float tmp = exp2f(curve->scale * code + curve->yShift) + curve->xShift;
    /* Piecewise-linear post section, split at the cut. */
    const float cut = osvDlogmCut(curve);
    const float pw = (tmp < cut) ? (tmp * curve->slope + curve->intercept) : (tmp * curve->slope2);
    return pw * curve->midGrayScaling;
}

/* ---------------------------------------------------------------------------
 *  BT.2100 HLG
 * ------------------------------------------------------------------------- */

/**
 * @brief HLG OETF: scene-linear E in [0,1] (1.0 = HLG reference white) -> signal.
 *
 * Negative inputs are clamped to zero; inputs above 1.0 extrapolate along the
 * log branch (the caller clamps the signal if it must stay in [0,1]).
 */
OSV_HD float osvHlgOetf(float e) {
    e = fmaxf(e, 0.0f);
    if (e <= 1.0f / 12.0f) {
        return sqrtf(3.0f * e);
    }
    /* Guard the logarithm: 12e - b is positive for e > 1/12 by construction. */
    return OSV_HLG_A * logf(fmaxf(12.0f * e - OSV_HLG_B, 1e-12f)) + OSV_HLG_C;
}

/** @brief HLG inverse OETF: signal -> scene-linear E (0.75 -> 0.26496). */
OSV_HD float osvHlgInverseOetf(float ep) {
    ep = fmaxf(ep, 0.0f);
    if (ep <= 0.5f) {
        return ep * ep / 3.0f;
    }
    return (osvExpf((ep - OSV_HLG_C) / OSV_HLG_A) + OSV_HLG_B) / 12.0f;
}

/**
 * @brief HLG OOTF luminance multiplier.
 *
 * BT.2100: Yd = peakNits * Ys^(gamma - 1) * E for each channel, where Ys is
 * the scene luminance of the pixel.  This returns the shared multiplier so
 * callers compute `nits = mult * E` per channel.  Ys <= 0 yields 0.
 */
OSV_HD float osvHlgOotfScale(float ys, float peakNits, float gamma) {
    if (ys <= 0.0f) {
        return 0.0f;
    }
    return peakNits * powf(ys, gamma - 1.0f);
}

/* ---------------------------------------------------------------------------
 *  SMPTE ST 2084 PQ
 * ------------------------------------------------------------------------- */

/** @brief PQ inverse EOTF: absolute luminance in nits -> PQ code (10000 -> 1). */
OSV_HD float osvPqInverseEotf(float nits) {
    const float y = osvSaturatef(nits / OSV_PQ_PEAK_NITS);
    const float ym1 = powf(y, OSV_PQ_M1);
    return powf((OSV_PQ_C1 + OSV_PQ_C2 * ym1) / (1.0f + OSV_PQ_C3 * ym1), OSV_PQ_M2);
}

/** @brief PQ EOTF: PQ code in [0,1] -> absolute luminance in nits. */
OSV_HD float osvPqEotf(float code) {
    const float ep = powf(osvSaturatef(code), 1.0f / OSV_PQ_M2);
    const float num = fmaxf(ep - OSV_PQ_C1, 0.0f);
    const float den = OSV_PQ_C2 - OSV_PQ_C3 * ep;
    /* den is positive for every code in [0,1] (c2 > c3); guard anyway. */
    if (den <= 1e-12f) {
        return OSV_PQ_PEAK_NITS;
    }
    return powf(num / den, 1.0f / OSV_PQ_M1) * OSV_PQ_PEAK_NITS;
}

/* ---------------------------------------------------------------------------
 *  BT.709 / BT.1886
 * ------------------------------------------------------------------------- */

/** @brief BT.709 OETF: display-linear [0,1] -> signal (0.018 -> 0.081). */
OSV_HD float osvRec709Oetf(float e) {
    e = osvSaturatef(e);
    if (e < 0.018f) {
        return 4.5f * e;
    }
    return 1.099f * powf(e, 0.45f) - 0.099f;
}

/** @brief Inverse of osvRec709Oetf: signal [0,1] -> linear. */
OSV_HD float osvRec709InverseOetf(float v) {
    v = osvSaturatef(v);
    if (v < 0.081f) {
        return v / 4.5f;
    }
    return powf((v + 0.099f) / 1.099f, 1.0f / 0.45f);
}

/* ---------------------------------------------------------------------------
 *  BT.2390 EETF (tone mapping between PQ peaks)
 * ------------------------------------------------------------------------- */

/**
 * @brief BT.2390-8 EETF applied to a PQ-encoded value.
 *
 * Maps content mastered for srcPeakNits onto a display with dstPeakNits using
 * the Hermite spline knee (knee start KS = 1.5 * maxLum - 0.5) from the
 * report.  Source and target black are assumed to be 0 nit, so the black
 * lift term (b * (1 - E2)^4 with b = 0) vanishes.  When the target peak is
 * not below the source peak the value passes through unchanged.
 */
OSV_HD float osvBt2390Eetf(float pqCode, float srcPeakNits, float dstPeakNits) {
    /* Nothing to compress when the target can show the full source range. */
    if (dstPeakNits >= srcPeakNits || srcPeakNits <= 0.0f || dstPeakNits <= 0.0f) {
        return pqCode;
    }
    /* Source range in PQ space: [minLumSrc = PQ(0) = 0, maxLumSrc = PQ(src)]. */
    const float srcMax = osvPqInverseEotf(srcPeakNits);
    if (srcMax <= 1e-6f) {
        return pqCode;
    }
    /* Normalise the input and the target peak to the source range. */
    const float e1 = osvSaturatef(pqCode / srcMax);
    const float maxLum = osvPqInverseEotf(dstPeakNits) / srcMax;
    const float ks = 1.5f * maxLum - 0.5f;
    float e2 = e1;
    /* Hermite spline above the knee. */
    if (e1 > ks && ks < 1.0f) {
        const float t = (e1 - ks) / (1.0f - ks);
        const float t2 = t * t;
        const float t3 = t2 * t;
        e2 = (2.0f * t3 - 3.0f * t2 + 1.0f) * ks + (t3 - 2.0f * t2 + t) * (1.0f - ks) + (-2.0f * t3 + 3.0f * t2) * maxLum;
    }
    /* Black lift with b = 0 is a no-op; rescale back to absolute PQ. */
    return osvClampf(e2, 0.0f, maxLum) * srcMax;
}

/* ---------------------------------------------------------------------------
 *  Pipeline stages
 * ------------------------------------------------------------------------- */

/**
 * @brief Narrow/full range Y'CbCr samples (bitDepth scale) -> R'G'B' code in [0,1].
 *
 * The luma is expanded with (y - yuvBlack) * yuvScaleY, the chroma with
 * (c - 2^(bitDepth-1)) * yuvScaleC, then the yuvToRgb matrix is applied and
 * the result clamped to [0,1].  For D-Log M input the result is the log code;
 * for HLG / Rec.709 input it is that transfer's signal.
 */
OSV_HD void osvYuvToCode(const OsvColorParams* params, float y, float u, float v, float out[3]) {
    if (params == 0) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    /* Chroma centre for the stored bit depth (512 for 10-bit). */
    int depth = params->bitDepth;
    if (depth < 1) {
        depth = 10;
    }
    if (depth > 16) {
        depth = 16;
    }
    const float chromaMid = (float)(1 << (depth - 1));
    /* Range expansion. */
    const float yn = (y - params->yuvBlack) * params->yuvScaleY;
    const float un = (u - chromaMid) * params->yuvScaleC;
    const float vn = (v - chromaMid) * params->yuvScaleC;
    /* Y'CbCr -> R'G'B'. */
    const float* k = params->yuvToRgb;
    out[0] = osvSaturatef(k[0] * yn + k[1] * un + k[2] * vn);
    out[1] = osvSaturatef(k[3] * yn + k[4] * un + k[5] * vn);
    out[2] = osvSaturatef(k[6] * yn + k[7] * un + k[8] * vn);
}

/**
 * @brief R'G'B' code -> scene-linear native RGB with 18 % grey at 0.18.
 *
 *  - DLogM:         the log curve per channel.
 *  - HLG:           HLG inverse OETF per channel divided by sceneScale, so an
 *                   HLG grey of 0.38 lands on 0.18 like the log path.
 *  - Rec709Normal:  BT.709 inverse OETF per channel (display-referred SDR is
 *                   treated as scene-linear with grey ~0.18).
 */
OSV_HD void osvCodeToLinear(const OsvColorParams* params, const float code[3], float out[3]) {
    int i;
    if (params == 0 || params->enabled == 0) {
        out[0] = code[0];
        out[1] = code[1];
        out[2] = code[2];
        return;
    }
    if (params->inputEncoding == OSV_INPUT_HLG) {
        /* Undo the BT.2408 scene scale so grey normalises to 0.18. */
        const float inv = (params->sceneScale > 1e-12f) ? (1.0f / params->sceneScale) : 1.0f;
        for (i = 0; i < 3; ++i) {
            out[i] = osvHlgInverseOetf(code[i]) * inv;
        }
        return;
    }
    if (params->inputEncoding == OSV_INPUT_REC709_NORMAL) {
        for (i = 0; i < 3; ++i) {
            out[i] = osvRec709InverseOetf(code[i]);
        }
        return;
    }
    /* Default: D-Log M. */
    for (i = 0; i < 3; ++i) {
        out[i] = osvDlogmToLinear(&params->curve, code[i]);
    }
}

/**
 * @brief Scene-linear native RGB -> encoded output signal.
 *
 * Steps: nativeToWorking matrix, exposure gain, then per transfer:
 *  - HLG:     * sceneScale, workingToOutput, HLG OETF per channel.
 *  - PQ:      * sceneScale, workingToOutput, HLG OOTF (peakNits, ootfGamma on
 *             BT.2020 luminance), PQ inverse EOTF per channel.
 *  - Rec709:  * sceneScale, OOTF to peakNits, PQ encode, BT.2390 EETF
 *             peakNits -> sdrPeakNits, PQ decode, / sdrPeakNits,
 *             workingToOutput (2020 -> 709), clamp, BT.709 OETF.
 *  - Linear:  workingToOutput applied, no sceneScale (grey stays 0.18);
 *             this is what the EXR writer stores.
 *  - Passthrough: input copied unchanged (see osvCodeToOutput).
 * Outputs of the encoded transfers are clamped to [0,1].
 */
OSV_HD void osvLinearToOutput(const OsvColorParams* params, const float lin[3], float out[3]) {
    float working[3];
    float tmp[3];
    int i;
    if (params == 0 || params->enabled == 0 || params->transfer == OSV_TRANSFER_PASSTHROUGH) {
        out[0] = lin[0];
        out[1] = lin[1];
        out[2] = lin[2];
        return;
    }
    /* Native primaries -> working primaries, then exposure. */
    osvMat3Apply(&params->nativeToWorking, lin[0], lin[1], lin[2], working);
    for (i = 0; i < 3; ++i) {
        working[i] *= params->exposureGain;
    }

    if (params->transfer == OSV_TRANSFER_LINEAR) {
        /* Raw scene-linear in the output primaries; nothing else applied. */
        osvMat3Apply(&params->workingToOutput, working[0], working[1], working[2], out);
        return;
    }

    /* Scene scale (BT.2408: 0.18 grey -> 0.04813 -> HLG 0.38). */
    for (i = 0; i < 3; ++i) {
        working[i] = fmaxf(working[i] * params->sceneScale, 0.0f);
    }

    if (params->transfer == OSV_TRANSFER_HLG) {
        osvMat3Apply(&params->workingToOutput, working[0], working[1], working[2], tmp);
        for (i = 0; i < 3; ++i) {
            out[i] = osvSaturatef(osvHlgOetf(fmaxf(tmp[i], 0.0f)));
        }
        return;
    }

    /* PQ and Rec.709 share the HLG OOTF onto a peakNits display. */
    {
        const float ys = OSV_BT2020_LUMA_R * working[0] + OSV_BT2020_LUMA_G * working[1] +
                         OSV_BT2020_LUMA_B * working[2];
        const float mult = osvHlgOotfScale(ys, params->peakNits, params->ootfGamma);
        float nits[3];
        for (i = 0; i < 3; ++i) {
            nits[i] = fmaxf(mult * working[i], 0.0f);
        }

        if (params->transfer == OSV_TRANSFER_PQ) {
            osvMat3Apply(&params->workingToOutput, nits[0], nits[1], nits[2], tmp);
            for (i = 0; i < 3; ++i) {
                out[i] = osvSaturatef(osvPqInverseEotf(fmaxf(tmp[i], 0.0f)));
            }
            return;
        }

        /* Rec.709 SDR: the HLG signal itself is the SDR rendering (HLG is
         * backward compatible by design, BT.2390 section "display on SDR").
         * Grey lands at 38 %, diffuse white at 75 % and the camera clip near
         * 99 %, which matches DJI's own D-Log M -> Rec.709 rendering far
         * better than a peak-to-peak EETF.  The 2020 -> 709 matrix runs in
         * linear light before the OETF so hues stay put. */
        {
            osvMat3Apply(&params->workingToOutput, working[0], working[1], working[2], tmp);
            for (i = 0; i < 3; ++i) {
                out[i] = osvSaturatef(osvHlgOetf(fmaxf(tmp[i], 0.0f)));
            }
        }
    }
}

/**
 * @brief Full path R'G'B' code -> output signal, honouring Passthrough.
 *
 * This is the entry point the kernels use after osvYuvToCode.  Passthrough
 * (and enabled == 0) copy the code unchanged; every other transfer runs
 * osvCodeToLinear followed by osvLinearToOutput.
 */
OSV_HD void osvCodeToOutput(const OsvColorParams* params, const float code[3], float out[3]) {
    float lin[3];
    if (params == 0 || params->enabled == 0 || params->transfer == OSV_TRANSFER_PASSTHROUGH) {
        out[0] = code[0];
        out[1] = code[1];
        out[2] = code[2];
        return;
    }
    osvCodeToLinear(params, code, lin);
    osvLinearToOutput(params, lin, out);
}

#endif /* OSV_COLOR_COLORMATH_H */
