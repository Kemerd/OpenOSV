/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * ReframeKernel.metal - the Metal twin of ReframeKernel.cu: Premiere on a Mac
 * hands GPU filters Metal buffers, so this is the effect's GPU path there.
 *
 * Assembled like every Metal library of the project (cmake/OsvMetal.cmake):
 *
 *     preamble.metal -> ColorMath.h -> osv_kernel.h -> ReframeKernel.metal
 *
 * so the kernel runs the one shared function osvReframeEquirectPixel(), the
 * same arithmetic as the CPU path and the CUDA kernel.  The wrapper only maps
 * a thread to an output pixel, calls it, and stores the float RGBA result as
 * BGRA 16f or 32f - with the float -> half conversion written out in integer
 * arithmetic, exactly as ReframeKernel.cu does, so a 16f frame is quantised
 * bit for bit like the CPU path's floatToHalf().
 *
 * Buffers (ReframeMetal.mm binds exactly this):
 *   0  OsvReframeParams    1  OsvRgbaSource
 *   2  source pixels       3  output pixels
 *   4  output row bytes    5  output is 16f (0 / 1)
 */

/* IEEE 754 binary32 -> binary16, round to nearest even (ReframeKernel.cu's
 * osvFloatToHalfDevice, line for line). */
static inline ushort osvFloatToHalfMetal(float value) {
    const uint bits = as_type<uint>(value);
    const uint sign = (bits >> 16) & 0x8000u;
    const uint rawExp = (bits >> 23) & 0xFFu;
    const uint mantissa = bits & 0x7FFFFFu;

    /* Infinity / NaN keep their class; a NaN keeps a non-zero payload. */
    if (rawExp == 0xFFu) {
        return (ushort)(sign | (mantissa != 0u ? 0x7E00u : 0x7C00u));
    }

    const int exponent = (int)rawExp - 127 + 15;
    if (exponent >= 0x1F) {
        return (ushort)(sign | 0x7C00u); /* overflow -> infinity */
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return (ushort)sign; /* underflow -> signed zero */
        }
        /* Subnormal: restore the implicit one, then round.  (`half` is a
         * type in this language, hence `truncated` for the CUDA `half`.) */
        const uint m = mantissa | 0x800000u;
        const int shift = 14 - exponent;
        const uint truncated = m >> shift;
        const uint rem = m & ((1u << shift) - 1u);
        const uint halfway = 1u << (shift - 1);
        uint rounded = truncated;
        if (rem > halfway || (rem == halfway && (truncated & 1u) != 0u)) {
            rounded += 1u;
        }
        return (ushort)(sign | rounded);
    }

    /* Normal: keep 10 mantissa bits, round to nearest even (a carry into the
     * exponent is exactly the right behaviour). */
    uint packed = ((uint)exponent << 10) | (mantissa >> 13);
    const uint rem = mantissa & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (packed & 1u) != 0u)) {
        packed += 1u;
    }
    return (ushort)(sign | packed);
}

/* Store one straight-RGBA float quadruple as the BGRA texel at column `x` of
 * the output row `row`, in 16f or 32f. */
static inline void osvStoreBgraMetal(device uchar* row, int x, int isHalf, thread const float* rgba) {
    if (isHalf) {
        device ushort* texel = (device ushort*)row + (size_t)x * 4u;
        texel[0] = osvFloatToHalfMetal(rgba[2]); /* B */
        texel[1] = osvFloatToHalfMetal(rgba[1]); /* G */
        texel[2] = osvFloatToHalfMetal(rgba[0]); /* R */
        texel[3] = osvFloatToHalfMetal(rgba[3]); /* A */
    } else {
        device float* texel = (device float*)row + (size_t)x * 4u;
        texel[0] = rgba[2];
        texel[1] = rgba[1];
        texel[2] = rgba[0];
        texel[3] = rgba[3];
    }
}

/* The kernel: one thread per output pixel.  Pixels outside the viewport come
 * back transparent black from the shared function, so no clear pass is
 * needed; the grid is rounded up to whole threadgroups and the tail threads
 * return at once. */
kernel void osvReframeEquirectKernel(constant OsvReframeParams& params [[buffer(0)]],
                                     constant OsvRgbaSource& source [[buffer(1)]],
                                     device const uchar* sourcePixels [[buffer(2)]],
                                     device uchar* dstBase [[buffer(3)]], constant int& dstRowBytes [[buffer(4)]],
                                     constant int& dstIsHalf [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= params.outW || y >= params.outH) {
        return;
    }
    /* The shared function takes its blocks through thread pointers. */
    const OsvReframeParams p = params;
    const OsvRgbaSource s = source;
    float rgba[4];
    osvReframeEquirectPixel(&p, &s, sourcePixels, x, y, rgba);

    /* Premiere GPU frames: top-left origin, positive pitch. */
    device uchar* row = dstBase + (size_t)y * (size_t)dstRowBytes;
    osvStoreBgraMetal(row, x, dstIsHalf, rgba);
}
