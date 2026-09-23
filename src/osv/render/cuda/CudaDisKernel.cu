// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaDisKernel.cu - Dense Inverse Search on the GPU.
//
// WHAT THIS IS
// ------------
// A port of src/osv/render/DisFlow.cpp (Kroeger et al., "Fast Optical Flow
// using Dense Inverse Search", ECCV 2016, with the constants DJI's stitcher uses) to
// CUDA.  Each kernel below is one stage of the CPU solver:
//
//   disScaleKernel        intensityScale applied at the pyramid base
//   disBlurRows/ColsKernel  separable Gaussian (pyramid pre-blur, smoothFlow)
//   disDecimateKernel     2x2 box half-scale              (halfScale)
//   disGradientKernel     central differences             (gradients)
//   disTensorKernel       per-patch inverse tensor        (precomputeTensors)
//   disSolveGroupKernel   seeded inverse search, a lane group per patch
//                         (solvePatch; disSolveKernel for other patch sides)
//   disDensifyKernel      photometric-weight gather       (densify)
//   disConsistencyKernel  forward-backward check          (disFlowBidirectional)
//
// WHY IT MIRRORS THE CPU ARITHMETIC SO CLOSELY
// --------------------------------------------
// DIS is an iterative solve with hard thresholds - a tensor determinant
// floor, a displacement cap, an SSD rejection, a convergence break - so a
// last-bit difference in one sum can flip a patch from accepted to rejected
// and move the field by whole pixels there.  Rather than chase a tolerance,
// every per-element computation here is written in the CPU's order and at
// the CPU's precision: the structure tensor and the patch sums accumulate in
// DOUBLE exactly as DisFlow.cpp does, each patch's sums run in the CPU's
// order (one thread, or a lane group relaying the running sum in row order),
// densify gathers overlapping patches in the CPU's order, and the Gaussian
// taps are built on the host with the CPU's own code.  With -fmad=false and
// IEEE division on both sides, the GPU field equals the CPU field bit for
// bit - on the synthetic pairs of the tests and on the sample clip's real
// bands (tests/unit/test_disflow_cuda.cpp compares them with ==).
//
// Double precision is slow on consumer GPUs (1/64 rate on the RTX 5090), and
// it is still the right call here.  The solve is a few thousand patches of 64
// pixels, and the double work is kept to exactly the CPU's additions (see
// "Exact double arithmetic" below): a bidirectional solve of a 2048 x 68 band
// measures ~0.3 ms of GPU time.  Float sums in a different order would buy
// little of that back, at the price of an unexplainable difference from the
// reference - and of the importer's guarantee that an export does not depend
// on which path measured a bucket.
//
// Deliberately free of host library code - see CudaLaunch.h.

#include "CudaAnalysisLaunch.h"

#include <math.h>

namespace osv::render::gpu {

namespace {

// ---------------------------------------------------------------------------
//  Launch geometry
// ---------------------------------------------------------------------------

/// Pixel kernels: one warp per row segment for coalesced access.
constexpr unsigned kPixBlockX = 32;
constexpr unsigned kPixBlockY = 8;

/// Patch kernels: ONE warp per block.  The patch count is small (about five
/// thousand at the finest level of a 2048 x 68 band) and each patch is a
/// long sequential FP64 computation, so the priority is spreading warps over
/// every SM rather than packing them - a 128-thread block would leave most
/// of a 170-SM part idle.
constexpr unsigned kPatchBlock = 32;

/// Resident warps per SM the lane-group solve aims for when it picks how
/// many lanes to give each patch (see disLaunchSolve).
constexpr unsigned kSolveWarpsPerSm = 4;

/// Grid for a w x h pixel pass with `z` planes.
dim3 pixelGrid(int w, int h, int z) {
    return dim3((static_cast<unsigned>(w) + kPixBlockX - 1u) / kPixBlockX,
                (static_cast<unsigned>(h) + kPixBlockY - 1u) / kPixBlockY, static_cast<unsigned>(z));
}

// ---------------------------------------------------------------------------
//  Device helpers - each the exact counterpart of a DisFlow.cpp helper
// ---------------------------------------------------------------------------

/// std::clamp for int.
__device__ __forceinline__ int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------------------
//  Exact double arithmetic on a GPU with slow FP64
//
//  The CPU solver accumulates in double, and matching it bit for bit means
//  doing the same double operations in the same order.  On a consumer GPU
//  the FP64 pipe runs at 1/64 of the FP32 rate - measured, one warp's FP64
//  instruction occupies it for tens of cycles, and a dependent one waits for
//  that - and a first port that simply wrote the CPU expressions spent
//  ~220 us per pyramid level there.  Two identities remove most of that
//  work WITHOUT changing a single result:
//
//  1. Widening a float to double is exact and is only a re-packing of bits,
//     so it is done on the integer pipes (widen) instead of with an
//     F2F.F64.F32 on the FP64 pipe.
//
//  2. The product of two widened floats is EXACT in double: 24 + 24
//     significand bits fit in 53, and the exponent range fits with room to
//     spare.  So the CPU's  acc += double(a) * double(b)  - a multiply that
//     never rounds followed by a rounding add - equals  fma(a, b, acc), one
//     instruction that rounds once.  (An explicit fma() is honoured under
//     -fmad=false; that flag only stops the compiler contracting a*b + c on
//     its own, which here would not even change the value.)
// ---------------------------------------------------------------------------

/// Exact float -> double, by bit manipulation on the integer pipes.
///
/// A normal float's sign and significand carry over unchanged and its
/// exponent is rebiased by 1023 - 127 = 896: the double's high word is the
/// sign, then the float's exponent-and-significand shifted down by 3 with 896
/// added to the exponent field, and its low word holds the significand's last
/// 3 bits.  Zero keeps its sign.  Subnormals, infinities and NaNs - which
/// neither a band nor a gradient produces in practice - take the hardware
/// conversion.  The result is exactly static_cast<double>(f) for every input.
///
/// Why bother: on this class of GPU an F2F.F64.F32 goes down the FP64 pipe,
/// the same narrow pipe the double sums need, and in the patch solve there
/// were more conversions than additions.
__device__ __forceinline__ double widen(float f) {
    const unsigned int bits = __float_as_uint(f);
    const unsigned int magnitude = bits & 0x7FFFFFFFu;
    const unsigned int exponent = magnitude >> 23;
    if (exponent == 0xFFu || (exponent == 0u && magnitude != 0u)) {
        return static_cast<double>(f);  // infinity, NaN or subnormal
    }
    const unsigned int sign = bits & 0x80000000u;
    const unsigned int hi = magnitude == 0u ? sign : (sign | ((magnitude >> 3) + (896u << 20)));
    const unsigned int lo = magnitude << 29;
    return __hiloint2double(static_cast<int>(hi), static_cast<int>(lo));
}

/// acc + double(a) * double(b), rounded exactly as the CPU's separate
/// multiply and add round it (see identity 2 above).
__device__ __forceinline__ double addProduct(double acc, float a, float b) { return fma(widen(a), widen(b), acc); }

/// GrayImage::at: nearest pixel with clamp-to-edge.
__device__ __forceinline__ float at(const float* __restrict__ img, int w, int h, int x, int y) {
    const int cx = clampi(x, 0, w - 1);
    const int cy = clampi(y, 0, h - 1);
    return img[static_cast<size_t>(cy) * static_cast<size_t>(w) + static_cast<size_t>(cx)];
}

/// GrayImage::sample: bilinear with clamp-to-edge; 0 for a non-finite
/// coordinate (which only a diverged solve can produce).  Same expression
/// order as the CPU so the float result is identical.
__device__ __forceinline__ float sampleBilinear(const float* __restrict__ img, int w, int h, float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        return 0.0f;
    }
    const float fx = floorf(x);
    const float fy = floorf(y);
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const float tx = x - fx;
    const float ty = y - fy;
    const float a = at(img, w, h, x0, y0);
    const float b = at(img, w, h, x0 + 1, y0);
    const float c = at(img, w, h, x0, y0 + 1);
    const float d = at(img, w, h, x0 + 1, y0 + 1);
    const float top = a + (b - a) * tx;
    const float bot = c + (d - c) * tx;
    return top + (bot - top) * ty;
}

// ---------------------------------------------------------------------------
//  Pyramid, blur, gradients
// ---------------------------------------------------------------------------

/// Intensity scaling at the pyramid base: DisFlowParams::intensityScale puts
/// the images on the 8-bit range DJI's absolute thresholds assume.
__global__ void disScaleKernel(const float* src0, size_t srcPitch0, const float* src1, size_t srcPitch1, float* dst0,
                               float* dst1, int w, int h, float scale, int applyScale) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= w || y >= h) {
        return;
    }
    // blockIdx.z picks the image.  src may alias dst (the host path uploads
    // straight into the pyramid base and scales in place), which is safe
    // because every thread reads and writes only its own element - and is
    // why none of these pointers is __restrict__.
    const bool second = blockIdx.z != 0;
    const float* src = second ? src1 : src0;
    const size_t pitch = second ? srcPitch1 : srcPitch0;
    float* dst = second ? dst1 : dst0;
    const float value = src[static_cast<size_t>(y) * pitch + static_cast<size_t>(x)];
    dst[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
        applyScale != 0 ? value * scale : value;
}

/// Horizontal pass of DisFlow.cpp blurPlane: tmp(y) from plane(y).
__global__ void disBlurRowsKernel(const __grid_constant__ DisPlaneBatch batch, int w, int h,
                                  const __grid_constant__ DisBlurTaps taps) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    const int z = static_cast<int>(blockIdx.z);
    if (x >= w || y >= h || z >= batch.count) {
        return;
    }
    const float* __restrict__ src = batch.src[z];
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    // Accumulated from -radius to +radius in float, the CPU's exact order.
    float acc = 0.0f;
    for (int i = -taps.radius; i <= taps.radius; ++i) {
        const int sx = clampi(x + i, 0, w - 1);
        acc += src[row + static_cast<size_t>(sx)] * taps.k[i + taps.radius];
    }
    batch.dst[z][row + static_cast<size_t>(x)] = acc;
}

/// Vertical pass of DisFlow.cpp blurPlane: plane(y) from tmp(y +/- r).
__global__ void disBlurColsKernel(const __grid_constant__ DisPlaneBatch batch, int w, int h,
                                  const __grid_constant__ DisBlurTaps taps) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    const int z = static_cast<int>(blockIdx.z);
    if (x >= w || y >= h || z >= batch.count) {
        return;
    }
    const float* __restrict__ src = batch.src[z];
    float acc = 0.0f;
    for (int i = -taps.radius; i <= taps.radius; ++i) {
        const int sy = clampi(y + i, 0, h - 1);
        acc += src[static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(x)] *
               taps.k[i + taps.radius];
    }
    batch.dst[z][static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = acc;
}

/// DisFlow.cpp halfScale's decimation: 0.25 * (a + b + c + d), summed left
/// to right like the CPU, from the pre-blurred level.
__global__ void disDecimateKernel(const __grid_constant__ DisPlaneBatch batch, int srcW, int srcH, int dstW,
                                  int dstH) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    const int z = static_cast<int>(blockIdx.z);
    if (x >= dstW || y >= dstH || z >= batch.count) {
        return;
    }
    const float* __restrict__ src = batch.src[z];
    const int sx = x * 2;
    const int sy = y * 2;
    const float a = at(src, srcW, srcH, sx, sy);
    const float b = at(src, srcW, srcH, sx + 1, sy);
    const float c = at(src, srcW, srcH, sx, sy + 1);
    const float d = at(src, srcW, srcH, sx + 1, sy + 1);
    batch.dst[z][static_cast<size_t>(y) * static_cast<size_t>(dstW) + static_cast<size_t>(x)] =
        0.25f * (a + b + c + d);
}

/// DisFlow.cpp gradients: 0.5 * (I(x+1) - I(x-1)) and the same along y.
__global__ void disGradientKernel(DisPair img, DisPairW gx, DisPairW gy, int w, int h) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    const int z = static_cast<int>(blockIdx.z);
    if (x >= w || y >= h || z > 1) {
        return;
    }
    const float* __restrict__ src = img.p[z];
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
    gx.p[z][i] = 0.5f * (at(src, w, h, x + 1, y) - at(src, w, h, x - 1, y));
    gy.p[z][i] = 0.5f * (at(src, w, h, x, y + 1) - at(src, w, h, x, y - 1));
}

// ---------------------------------------------------------------------------
//  Patches
// ---------------------------------------------------------------------------

/// DisFlow.cpp precomputeTensors, one thread per patch.  The tensor is
/// accumulated in double, row by row, exactly as the CPU does it, and the
/// singularity test is the same RAW determinant against DJI's floor.
__global__ void disTensorKernel(DisPair gx, DisPair gy, int w, int h, DisGrid grid, double minTensorDet,
                                DisPatchPair patches) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int z = static_cast<int>(blockIdx.y);
    const int count = grid.cols * grid.rows;
    if (i >= count || z > 1) {
        return;
    }
    const int row = i / grid.cols;
    const int col = i - row * grid.cols;
    const int cx = grid.half + col * grid.stride;
    const int cy = grid.half + row * grid.stride;

    DisPatch p;
    p.x = static_cast<float>(cx);
    p.y = static_cast<float>(cy);
    p.u = 0.0f;
    p.v = 0.0f;
    p.quality = 0.0f;
    p.iHxx = 0.0f;
    p.iHxy = 0.0f;
    p.iHyy = 0.0f;
    p.usable = 0;

    const float* __restrict__ gxp = gx.p[z];
    const float* __restrict__ gyp = gy.p[z];
    double hxx = 0.0;
    double hxy = 0.0;
    double hyy = 0.0;
    for (int dy = -grid.half; dy < grid.ps - grid.half; ++dy) {
        for (int dx = -grid.half; dx < grid.ps - grid.half; ++dx) {
            const int sx = clampi(cx + dx, 0, w - 1);
            const int sy = clampi(cy + dy, 0, h - 1);
            const size_t idx = static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx);
            const float a = gxp[idx];
            const float b = gyp[idx];
            // hxx += double(a) * double(a), and so on - exact products, so
            // one fused operation rounds exactly as the CPU's two do.
            hxx = addProduct(hxx, a, a);
            hxy = addProduct(hxy, a, b);
            hyy = addProduct(hyy, b, b);
        }
    }
    // DJI's raw-determinant invertibility test (see DisFlowParams).
    const double det = hxx * hyy - hxy * hxy;
    if (det > minTensorDet) {
        const double invDet = 1.0 / det;
        p.iHxx = static_cast<float>(hyy * invDet);
        p.iHxy = static_cast<float>(-hxy * invDet);
        p.iHyy = static_cast<float>(hxx * invDet);
        p.usable = 1;
    }
    patches.p[z][i] = p;
}

/// DisFlow.cpp solvePatch for one patch, run by one thread.
///
/// The general path, for any patch side up to kDisMaxPatchSize other than
/// DJI's 8 (which runs the faster lane-group solve further down, with
/// identical arithmetic).  One thread per patch keeps every double sum in
/// the CPU's order.  kPs > 0 would fix the side at compile time; the
/// launcher instantiates only kPs == 0, the runtime side.
template <int kPs>
__device__ void solvePatch(DisPatch& patch, const float* __restrict__ from, const float* __restrict__ to,
                           const float* __restrict__ gx, const float* __restrict__ gy, int w, int h, int psRuntime,
                           const DisSolveConsts& c) {
    if (patch.usable == 0) {
        patch.quality = 0.0f;
        return;
    }
    const int ps = kPs > 0 ? kPs : psRuntime;
    const int half = ps / 2;
    const int cx = static_cast<int>(patch.x);
    const int cy = static_cast<int>(patch.y);
    constexpr int kCap = kPs > 0 ? kPs * kPs : kDisMaxPatchSize * kDisMaxPatchSize;
    float tmpl[kCap];
    float target[kCap];
    const double windowPixels = static_cast<double>(ps) * ps;

    // The template and its mean, computed once - the template never moves.
    double tmplSum = 0.0;
#pragma unroll 1
    for (int dy = -half; dy < ps - half; ++dy) {
#pragma unroll
        for (int dx = -half; dx < ps - half; ++dx) {
            const float value = at(from, w, h, cx + dx, cy + dy);
            tmpl[(dy + half) * ps + (dx + half)] = value;
            tmplSum += widen(value);
        }
    }
    const float tmplMean = static_cast<float>(tmplSum / windowPixels);

    float u = patch.u;
    float v = patch.v;
    double lastResidual = 0.0;

    for (int iter = 0; iter < c.iterations; ++iter) {
        // Is the whole displaced window safely inside the image?  Then every
        // clamp in the bilinear fetch is a no-op and a clamp-free fetch gives
        // exactly the same samples at a fraction of the instructions (the
        // clamps and their address arithmetic were most of this kernel's
        // time: with one warp per scheduler it is issue-bound).  The sample
        // coordinate  float(c + d) + u  is monotonic in d, so testing the
        // window's first and last row and column covers every sample.
        const float xFirst = static_cast<float>(cx - half) + u;
        const float xLast = static_cast<float>(cx + ps - half - 1) + u;
        const float yFirst = static_cast<float>(cy - half) + v;
        const float yLast = static_cast<float>(cy + ps - half - 1) + v;
        const bool inside = isfinite(u) && isfinite(v) && floorf(xFirst) >= 0.0f &&
                            floorf(xLast) <= static_cast<float>(w - 2) && floorf(yFirst) >= 0.0f &&
                            floorf(yLast) <= static_cast<float>(h - 2);

        // Target window at the current displacement, and its own mean.
        double targetSum = 0.0;
#pragma unroll
        for (int dy = -half; dy < ps - half; ++dy) {
            // One window row: the samples are independent, so unrolling lets
            // their loads overlap; the double sum then takes them in order.
            float row[kPs > 0 ? kPs : kDisMaxPatchSize];
            if (inside) {
                // The row's vertical position and weight are shared by all
                // its samples: the same expression gives the same floats.
                const float ys = static_cast<float>(cy + dy) + v;
                const float fy = floorf(ys);
                const int y0 = static_cast<int>(fy);
                const float ty = ys - fy;
                const float* __restrict__ line = to + static_cast<size_t>(y0) * static_cast<size_t>(w);
#pragma unroll
                for (int dx = -half; dx < ps - half; ++dx) {
                    const float xs = static_cast<float>(cx + dx) + u;
                    const float fx = floorf(xs);
                    const int x0 = static_cast<int>(fx);
                    const float tx = xs - fx;
                    const float* __restrict__ p = line + x0;
                    const float a = p[0];
                    const float b = p[1];
                    const float cc = p[w];
                    const float d = p[w + 1];
                    const float top = a + (b - a) * tx;
                    const float bot = cc + (d - cc) * tx;
                    row[dx + half] = top + (bot - top) * ty;
                }
            } else {
                // Near an edge (or diverged): the full clamped fetch.
#pragma unroll
                for (int dx = -half; dx < ps - half; ++dx) {
                    row[dx + half] =
                        sampleBilinear(to, w, h, static_cast<float>(cx + dx) + u, static_cast<float>(cy + dy) + v);
                }
            }
#pragma unroll
            for (int dx = -half; dx < ps - half; ++dx) {
                target[(dy + half) * ps + (dx + half)] = row[dx + half];
                targetSum += widen(row[dx + half]);
            }
        }
        const float targetMean = static_cast<float>(targetSum / windowPixels);

        // Project the mean-normalised residual onto the template gradients.
        // bx / by take the exact products fused (see addProduct).
        double bx = 0.0;
        double by = 0.0;
#pragma unroll
        for (int dy = -half; dy < ps - half; ++dy) {
            const int sy = clampi(cy + dy, 0, h - 1);
            const size_t rowBase = static_cast<size_t>(sy) * static_cast<size_t>(w);
#pragma unroll
            for (int dx = -half; dx < ps - half; ++dx) {
                const int sx = clampi(cx + dx, 0, w - 1);
                const size_t gidx = rowBase + static_cast<size_t>(sx);
                const int k = (dy + half) * ps + (dx + half);
                const float t = tmpl[k] - tmplMean;
                const float s = target[k] - targetMean;
                const float residual = s - t;
                bx = addProduct(bx, residual, gx[gidx]);
                by = addProduct(by, residual, gy[gidx]);
            }
        }

        // The mean |residual| of THIS iteration's window.  DisFlow.cpp forms
        // it every iteration, but only the value from the iteration that ends
        // the loop normally (convergence or the iteration cap) is ever read -
        // a rejected patch never reads it at all.  So it is formed here only
        // for that iteration, from the same stored samples, with the same
        // float residuals summed in the same order: the identical double,
        // for a quarter of the FP64 work of every other iteration.
        const auto meanAbsResidual = [&]() {
            double absResidual = 0.0;
            for (int k = 0; k < ps * ps; ++k) {
                const float t = tmpl[k] - tmplMean;
                const float s = target[k] - targetMean;
                const float residual = s - t;
                absResidual += fabs(widen(residual));
            }
            return absResidual / windowPixels;
        };

        // The damped inverse-tensor step (DJI's stepScale 0.4).
        const float du = -c.stepScale * (patch.iHxx * static_cast<float>(bx) + patch.iHxy * static_cast<float>(by));
        const float dv = -c.stepScale * (patch.iHxy * static_cast<float>(bx) + patch.iHyy * static_cast<float>(by));
        if (!isfinite(du) || !isfinite(dv)) {
            patch.usable = 0;
            patch.quality = 0.0f;
            return;
        }
        u += du;
        v += dv;

        // Wandered past any real disparity: disown the patch.
        if (fabs(static_cast<double>(u)) > c.maxDisplacementPx || fabs(static_cast<double>(v)) > c.maxDisplacementPx) {
            patch.usable = 0;
            patch.quality = 0.0f;
            return;
        }
        if (fabs(static_cast<double>(du)) < c.minStepPx && fabs(static_cast<double>(dv)) < c.minStepPx) {
            lastResidual = meanAbsResidual();
            break;  // converged
        }
        if (iter + 1 == c.iterations) {
            lastResidual = meanAbsResidual();  // the cap ends the loop here
        }
    }

    // DJI's SSD rejection, on the mean residual scaled back to a patch SSD.
    const double ssd = lastResidual * lastResidual * static_cast<double>(ps) * ps;
    if (ssd > c.maxPatchSsd) {
        patch.usable = 0;
        patch.quality = 0.0f;
        return;
    }

    patch.u = u;
    patch.v = v;
    // DJI's densification weight 1 / max(1, |residual|), as std::max(1.0, r)
    // would give it (r is finite here: a non-finite step returned above).
    patch.quality = static_cast<float>(1.0 / (lastResidual > 1.0 ? lastResidual : 1.0));
}

/// Seed from the coarser level and solve, one thread per patch (the general
/// patch-side path); blockIdx.y selects the direction.
template <int kPs>
__global__ void disSolveKernel(DisPair from, DisPair to, DisPair gx, DisPair gy, int w, int h, DisGrid grid,
                               DisPatchPair patches, DisPair coarseU, DisPair coarseV, int coarseW, int coarseH,
                               int seeded, DisSolveConsts consts) {
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int z = static_cast<int>(blockIdx.y);
    const int count = grid.cols * grid.rows;
    if (i >= count || z > 1) {
        return;
    }
    DisPatch p = patches.p[z][i];
    // Seed from the field carried up from the coarser level, doubling the
    // displacement because this level is twice the size - FlowField::atU
    // with its clamp-to-edge.
    if (seeded != 0) {
        const int sx = clampi(static_cast<int>(p.x * 0.5f), 0, coarseW - 1);
        const int sy = clampi(static_cast<int>(p.y * 0.5f), 0, coarseH - 1);
        const size_t ci = static_cast<size_t>(sy) * static_cast<size_t>(coarseW) + static_cast<size_t>(sx);
        p.u = coarseU.p[z][ci] * 2.0f;
        p.v = coarseV.p[z][ci] * 2.0f;
    }
    solvePatch<kPs>(p, from.p[z], to.p[z], gx.p[z], gy.p[z], w, h, grid.ps, consts);
    patches.p[z][i] = p;
}

// ---------------------------------------------------------------------------
//  The lane-group solve
//
//  Profiling the one-thread-per-patch kernel above (Nsight Compute, RTX 5090)
//  showed why it cost ~220 us at EVERY pyramid level, the coarsest included:
//  a level holds a few thousand patches at most, i.e. at most one warp per
//  scheduler, so nothing hides latency and a level takes as long as one
//  patch's serial instruction stream - ~6,000 instructions per iteration at
//  ~7.8 cycles each.  Most of that stream is not the double sums; it is the
//  64 bilinear samples and 64 residuals of each iteration, work with no
//  ordering constraint at all.
//
//  So a patch is solved by a GROUP of kLanes lanes: lane g owns window rows
//  [g * kPs / kLanes, (g + 1) * kPs / kLanes) and does their sampling and
//  residuals.  The double sums still run strictly in the CPU's row-major
//  order: each is RELAYED through the group - lane 0 adds its rows' terms,
//  hands the running double to lane 1 with a shuffle, which adds its rows'
//  terms, and so on - so every addition, and every rounding, happens in the
//  sequence DisFlow.cpp performs it.  All lanes then hold the final sum, and
//  every decision taken from it (convergence, rejection) is uniform across
//  the group.  Everything that is not an ordered addition - sampling, the
//  residuals, widening to double (on the integer pipes, see widen) - is done
//  by all lanes in parallel BEFORE the relay, so the serial path is the
//  additions alone.
//
//  Measured on the RTX 5090, 2048 x 68 band, three pyramid levels, both
//  directions per launch: 220 / 222 / 215 us per level one-thread-per-patch,
//  138 / 86 / 83 us with the lane counts disLaunchSolve picks.
// ---------------------------------------------------------------------------

/// Continue `acc` with lane-owned terms in lane order: lane 0's first, then
/// lane 1's, ...  `add` performs one lane's additions in its own row order.
/// Returns the final sum on every lane of the group.
template <int kLanes, class AddFn>
__device__ __forceinline__ double relaySum(double acc, int lane, unsigned groupMask, AddFn add) {
#pragma unroll
    for (int src = 0; src < kLanes; ++src) {
        if (lane == src) {
            acc = add(acc);
        }
        if (kLanes > 1) {
            acc = __shfl_sync(groupMask, acc, src, kLanes);
        }
    }
    return acc;
}

/// The same relay for the two gradient projections, carried together.
template <int kLanes, class AddFn>
__device__ __forceinline__ void relaySum2(double& a, double& b, int lane, unsigned groupMask, AddFn add) {
#pragma unroll
    for (int src = 0; src < kLanes; ++src) {
        if (lane == src) {
            add(a, b);
        }
        if (kLanes > 1) {
            a = __shfl_sync(groupMask, a, src, kLanes);
            b = __shfl_sync(groupMask, b, src, kLanes);
        }
    }
}

/// DisFlow.cpp solvePatch for a kPs x kPs window, by a group of kLanes lanes
/// (see above).  Arithmetic identical to solvePatch<kPs>.
template <int kPs, int kLanes>
__global__ void disSolveGroupKernel(DisPair from, DisPair to, DisPair gx, DisPair gy, int w, int h, DisGrid grid,
                                    DisPatchPair patches, DisPair coarseU, DisPair coarseV, int coarseW, int coarseH,
                                    int seeded, DisSolveConsts c) {
    static_assert(kPs > 0 && kPs % kLanes == 0, "each lane must own whole window rows");
    static_assert(32 % kLanes == 0, "a group must not straddle warps");
    constexpr int kRows = kPs / kLanes;  // window rows per lane
    constexpr int kHalf = kPs / 2;
    constexpr double kWindowPixels = static_cast<double>(kPs) * kPs;

    const int thread = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int i = thread / kLanes;
    const int lane = thread % kLanes;
    const int z = static_cast<int>(blockIdx.y);
    const int count = grid.cols * grid.rows;
    // Groups are aligned to warps and blocks, so a whole group leaves here
    // together and the shuffles below always find all of their lanes.
    if (i >= count || z > 1) {
        return;
    }
    const unsigned groupMask =
        kLanes >= 32 ? 0xFFFFFFFFu : (((1u << kLanes) - 1u) << ((threadIdx.x & 31u) / kLanes * kLanes));

    const float* __restrict__ src = from.p[z];
    const float* __restrict__ dst = to.p[z];
    const float* __restrict__ gxp = gx.p[z];
    const float* __restrict__ gyp = gy.p[z];

    // Every lane reads the same patch and computes the same seed.
    DisPatch patch = patches.p[z][i];
    if (seeded != 0) {
        const int sx = clampi(static_cast<int>(patch.x * 0.5f), 0, coarseW - 1);
        const int sy = clampi(static_cast<int>(patch.y * 0.5f), 0, coarseH - 1);
        const size_t ci = static_cast<size_t>(sy) * static_cast<size_t>(coarseW) + static_cast<size_t>(sx);
        patch.u = coarseU.p[z][ci] * 2.0f;
        patch.v = coarseV.p[z][ci] * 2.0f;
    }
    const auto finish = [&]() {
        if (lane == 0) {
            patches.p[z][i] = patch;
        }
    };
    if (patch.usable == 0) {
        patch.quality = 0.0f;
        finish();
        return;
    }
    const int cx = static_cast<int>(patch.x);
    const int cy = static_cast<int>(patch.y);
    const int rowBegin = lane * kRows - kHalf;  // this lane's first dy

    // This lane's template rows, and the template mean (relayed sum).
    float tmpl[kRows][kPs];
#pragma unroll
    for (int r = 0; r < kRows; ++r) {
#pragma unroll
        for (int dx = -kHalf; dx < kPs - kHalf; ++dx) {
            tmpl[r][dx + kHalf] = at(src, w, h, cx + dx, cy + rowBegin + r);
        }
    }
    const double tmplSum = relaySum<kLanes>(0.0, lane, groupMask, [&](double acc) {
#pragma unroll
        for (int r = 0; r < kRows; ++r) {
#pragma unroll
            for (int k = 0; k < kPs; ++k) {
                acc += widen(tmpl[r][k]);
            }
        }
        return acc;
    });
    const float tmplMean = static_cast<float>(tmplSum / kWindowPixels);

    // The template's gradients never change during the solve, so they are
    // widened to double once here, in parallel across the lanes, rather than
    // once per iteration inside the serial projection sums.  The template
    // window lies inside the image by construction of the patch grid; the
    // clamps are kept anyway, exactly as the CPU writes them.
    double gxD[kRows][kPs];
    double gyD[kRows][kPs];
#pragma unroll
    for (int r = 0; r < kRows; ++r) {
        const int sy = clampi(cy + rowBegin + r, 0, h - 1);
        const size_t rowBase = static_cast<size_t>(sy) * static_cast<size_t>(w);
#pragma unroll
        for (int dx = -kHalf; dx < kPs - kHalf; ++dx) {
            const size_t gidx = rowBase + static_cast<size_t>(clampi(cx + dx, 0, w - 1));
            gxD[r][dx + kHalf] = widen(gxp[gidx]);
            gyD[r][dx + kHalf] = widen(gyp[gidx]);
        }
    }

    float u = patch.u;
    float v = patch.v;
    double lastResidual = 0.0;
    float target[kRows][kPs];
    double wide[kRows][kPs];  // widened samples, then widened residuals

    for (int iter = 0; iter < c.iterations; ++iter) {
        // The clamp-free fetch applies when the whole displaced window is
        // inside the image (see solvePatch); every lane tests the same
        // window, so the choice is uniform across the group.
        const float xFirst = static_cast<float>(cx - kHalf) + u;
        const float xLast = static_cast<float>(cx + kPs - kHalf - 1) + u;
        const float yFirst = static_cast<float>(cy - kHalf) + v;
        const float yLast = static_cast<float>(cy + kPs - kHalf - 1) + v;
        const bool inside = isfinite(u) && isfinite(v) && floorf(xFirst) >= 0.0f &&
                            floorf(xLast) <= static_cast<float>(w - 2) && floorf(yFirst) >= 0.0f &&
                            floorf(yLast) <= static_cast<float>(h - 2);
#pragma unroll
        for (int r = 0; r < kRows; ++r) {
            const int dy = rowBegin + r;
            if (inside) {
                const float ys = static_cast<float>(cy + dy) + v;
                const float fy = floorf(ys);
                const int y0 = static_cast<int>(fy);
                const float ty = ys - fy;
                const float* __restrict__ line = dst + static_cast<size_t>(y0) * static_cast<size_t>(w);
#pragma unroll
                for (int dx = -kHalf; dx < kPs - kHalf; ++dx) {
                    const float xs = static_cast<float>(cx + dx) + u;
                    const float fx = floorf(xs);
                    const int x0 = static_cast<int>(fx);
                    const float tx = xs - fx;
                    const float* __restrict__ p = line + x0;
                    const float a = p[0];
                    const float b = p[1];
                    const float cc = p[w];
                    const float d = p[w + 1];
                    const float top = a + (b - a) * tx;
                    const float bot = cc + (d - cc) * tx;
                    target[r][dx + kHalf] = top + (bot - top) * ty;
                }
            } else {
#pragma unroll
                for (int dx = -kHalf; dx < kPs - kHalf; ++dx) {
                    target[r][dx + kHalf] =
                        sampleBilinear(dst, w, h, static_cast<float>(cx + dx) + u, static_cast<float>(cy + dy) + v);
                }
            }
        }
        // Widen this lane's samples in parallel with the other lanes, so the
        // relayed sum below is nothing but the ordered additions.
#pragma unroll
        for (int r = 0; r < kRows; ++r) {
#pragma unroll
            for (int k = 0; k < kPs; ++k) {
                wide[r][k] = widen(target[r][k]);
            }
        }
        const double targetSum = relaySum<kLanes>(0.0, lane, groupMask, [&](double acc) {
#pragma unroll
            for (int r = 0; r < kRows; ++r) {
#pragma unroll
                for (int k = 0; k < kPs; ++k) {
                    acc += wide[r][k];
                }
            }
            return acc;
        });
        const float targetMean = static_cast<float>(targetSum / kWindowPixels);

        // The mean-normalised residuals (float, as on the CPU), widened in
        // parallel; then bx / by, relayed together in the CPU's pixel order.
        // fma of two widened floats rounds exactly like the CPU's exact
        // product followed by a rounding add (see addProduct).
#pragma unroll
        for (int r = 0; r < kRows; ++r) {
#pragma unroll
            for (int k = 0; k < kPs; ++k) {
                const float t = tmpl[r][k] - tmplMean;
                const float s = target[r][k] - targetMean;
                const float residual = s - t;
                wide[r][k] = widen(residual);
            }
        }
        double bx = 0.0;
        double by = 0.0;
        relaySum2<kLanes>(bx, by, lane, groupMask, [&](double& ax, double& ay) {
#pragma unroll
            for (int r = 0; r < kRows; ++r) {
#pragma unroll
                for (int k = 0; k < kPs; ++k) {
                    ax = fma(wide[r][k], gxD[r][k], ax);
                    ay = fma(wide[r][k], gyD[r][k], ay);
                }
            }
        });

        // Only the iteration that ends the loop normally needs its mean
        // |residual| (see solvePatch); relayed like the other sums.
        // `wide` still holds this iteration's widened residuals; |x| of a
        // double is exact, so this is the CPU's fabs(double(residual)).
        const auto meanAbsResidual = [&]() {
            const double absSum = relaySum<kLanes>(0.0, lane, groupMask, [&](double acc) {
#pragma unroll
                for (int r = 0; r < kRows; ++r) {
#pragma unroll
                    for (int k = 0; k < kPs; ++k) {
                        acc += fabs(wide[r][k]);
                    }
                }
                return acc;
            });
            return absSum / kWindowPixels;
        };

        // The damped step and the tests - identical on every lane, because
        // every lane now holds the same bx, by, u and v.
        const float du = -c.stepScale * (patch.iHxx * static_cast<float>(bx) + patch.iHxy * static_cast<float>(by));
        const float dv = -c.stepScale * (patch.iHxy * static_cast<float>(bx) + patch.iHyy * static_cast<float>(by));
        if (!isfinite(du) || !isfinite(dv)) {
            patch.usable = 0;
            patch.quality = 0.0f;
            finish();
            return;
        }
        u += du;
        v += dv;
        if (fabs(static_cast<double>(u)) > c.maxDisplacementPx || fabs(static_cast<double>(v)) > c.maxDisplacementPx) {
            patch.usable = 0;
            patch.quality = 0.0f;
            finish();
            return;
        }
        if (fabs(static_cast<double>(du)) < c.minStepPx && fabs(static_cast<double>(dv)) < c.minStepPx) {
            lastResidual = meanAbsResidual();
            break;  // converged
        }
        if (iter + 1 == c.iterations) {
            lastResidual = meanAbsResidual();
        }
    }

    // DJI's SSD rejection and densification weight, as in solvePatch.
    const double ssd = lastResidual * lastResidual * static_cast<double>(kPs) * kPs;
    if (ssd > c.maxPatchSsd) {
        patch.usable = 0;
        patch.quality = 0.0f;
        finish();
        return;
    }
    patch.u = u;
    patch.v = v;
    patch.quality = static_cast<float>(1.0 / (lastResidual > 1.0 ? lastResidual : 1.0));
    finish();
}

/// DisFlow.cpp densify as a per-pixel gather.  For output pixel (x, y) the
/// covering patches are the rows r and columns c with
///     (y - ps + 1) / stride <= r <= y / stride     (rounded inward)
/// and likewise for x.  They are visited rows ascending, each row's columns
/// ascending - the order the CPU's row gather adds them in - so every pixel
/// sums the same float terms in the same order.
__global__ void disDensifyKernel(DisPatchPair patches, DisGrid grid, int w, int h, DisPairW outU, DisPairW outV) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    const int z = static_cast<int>(blockIdx.z);
    if (x >= w || y >= h || z > 1) {
        return;
    }
    const DisPatch* __restrict__ all = patches.p[z];
    const int ps = grid.ps;
    const int half = grid.half;
    const int stride = grid.stride;

    float accU = 0.0f;
    float accV = 0.0f;
    float weight = 0.0f;
    const int firstRow = max(0, (y - ps + 1 + stride - 1) / stride);
    const int lastRow = min(grid.rows - 1, y / stride);
    const int firstCol = max(0, (x - ps + 1 + stride - 1) / stride);
    const int lastCol = min(grid.cols - 1, x / stride);
    for (int r = firstRow; r <= lastRow; ++r) {
        const int cy = half + r * stride;
        const int dy = y - cy;
        if (dy < -half || dy >= ps - half) {
            continue;  // guards the rounding above, as on the CPU
        }
        const DisPatch* rowPatches = all + static_cast<size_t>(r) * static_cast<size_t>(grid.cols);
        for (int c = firstCol; c <= lastCol; ++c) {
            const DisPatch& p = rowPatches[c];
            if (p.usable == 0 || !(p.quality > 0.0f)) {
                continue;
            }
            const int cx = static_cast<int>(p.x);
            const int x0 = max(0, cx - half);
            const int x1 = min(w - 1, cx + ps - half - 1);
            if (x < x0 || x > x1) {
                continue;  // the same per-patch column span the CPU loops over
            }
            const float wgt = p.quality;
            accU += p.u * wgt;
            accV += p.v * wgt;
            weight += wgt;
        }
    }
    // Normalise; an unreached pixel keeps zero flow, the honest answer.
    if (weight > 0.0f) {
        const float inv = 1.0f / weight;
        accU *= inv;
        accV *= inv;
    }
    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
    outU.p[z][i] = accU;
    outV.p[z][i] = accV;
}

// ---------------------------------------------------------------------------
//  Consistency
// ---------------------------------------------------------------------------

/// disFlowBidirectional's forward-backward test, one thread per pixel; the
/// per-block count of passing pixels is reduced with __syncthreads_count and
/// added once per block, which is exact (integer) and order-independent.
__global__ void disConsistencyKernel(const float* __restrict__ fu, const float* __restrict__ fv,
                                     const float* __restrict__ bu, const float* __restrict__ bv, int w, int h,
                                     double tol2, unsigned char* __restrict__ ok, unsigned long long* count) {
    const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
    const bool inside = x < w && y < h;
    int pass = 0;
    if (inside) {
        const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
        const float u = fu[i];
        const float v = fv[i];
        if (isfinite(u) && isfinite(v)) {
            // Nearest-pixel lookup of the backward field, rounded half away
            // from zero like std::lround, clamped like FlowField::atU.
            const int qx = clampi(static_cast<int>(lround(static_cast<double>(x) + u)), 0, w - 1);
            const int qy = clampi(static_cast<int>(lround(static_cast<double>(y) + v)), 0, h - 1);
            const size_t q = static_cast<size_t>(qy) * static_cast<size_t>(w) + static_cast<size_t>(qx);
            const double ex = static_cast<double>(u) + bu[q];
            const double ey = static_cast<double>(v) + bv[q];
            pass = (ex * ex + ey * ey <= tol2) ? 1 : 0;
        }
        ok[i] = static_cast<unsigned char>(pass);
    }
    // Every thread of the block must reach the barrier, inside or not.
    const int blockPasses = __syncthreads_count(pass);
    if (threadIdx.x == 0 && threadIdx.y == 0 && blockPasses > 0) {
        atomicAdd(count, static_cast<unsigned long long>(blockPasses));
    }
}

}  // namespace

// ===========================================================================
//  Launchers
// ===========================================================================

cudaError_t disLaunchScale(const float* src0, std::size_t srcPitch0, const float* src1, std::size_t srcPitch1,
                           float* dst0, float* dst1, int w, int h, float scale, int applyScale,
                           cudaStream_t stream) {
    if (src0 == nullptr || src1 == nullptr || dst0 == nullptr || dst1 == nullptr || w <= 0 || h <= 0 ||
        srcPitch0 < static_cast<std::size_t>(w) || srcPitch1 < static_cast<std::size_t>(w)) {
        return cudaErrorInvalidValue;
    }
    disScaleKernel<<<pixelGrid(w, h, 2), dim3(kPixBlockX, kPixBlockY), 0, stream>>>(
        src0, srcPitch0, src1, srcPitch1, dst0, dst1, w, h, scale, applyScale);
    return cudaGetLastError();
}

cudaError_t disLaunchBlurRows(const DisPlaneBatch& batch, int w, int h, const DisBlurTaps& taps,
                              cudaStream_t stream) {
    if (batch.count <= 0 || batch.count > kDisMaxBatch || w <= 0 || h <= 0 || taps.radius < 0 ||
        taps.radius > kDisMaxBlurRadius) {
        return cudaErrorInvalidValue;
    }
    disBlurRowsKernel<<<pixelGrid(w, h, batch.count), dim3(kPixBlockX, kPixBlockY), 0, stream>>>(batch, w, h, taps);
    return cudaGetLastError();
}

cudaError_t disLaunchBlurCols(const DisPlaneBatch& batch, int w, int h, const DisBlurTaps& taps,
                              cudaStream_t stream) {
    if (batch.count <= 0 || batch.count > kDisMaxBatch || w <= 0 || h <= 0 || taps.radius < 0 ||
        taps.radius > kDisMaxBlurRadius) {
        return cudaErrorInvalidValue;
    }
    disBlurColsKernel<<<pixelGrid(w, h, batch.count), dim3(kPixBlockX, kPixBlockY), 0, stream>>>(batch, w, h, taps);
    return cudaGetLastError();
}

cudaError_t disLaunchDecimate(const DisPlaneBatch& batch, int srcW, int srcH, int dstW, int dstH,
                              cudaStream_t stream) {
    if (batch.count <= 0 || batch.count > kDisMaxBatch || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        return cudaErrorInvalidValue;
    }
    disDecimateKernel<<<pixelGrid(dstW, dstH, batch.count), dim3(kPixBlockX, kPixBlockY), 0, stream>>>(
        batch, srcW, srcH, dstW, dstH);
    return cudaGetLastError();
}

cudaError_t disLaunchGradients(DisPair img, DisPairW gx, DisPairW gy, int w, int h, cudaStream_t stream) {
    if (w <= 0 || h <= 0 || img.p[0] == nullptr || img.p[1] == nullptr || gx.p[0] == nullptr ||
        gx.p[1] == nullptr || gy.p[0] == nullptr || gy.p[1] == nullptr) {
        return cudaErrorInvalidValue;
    }
    disGradientKernel<<<pixelGrid(w, h, 2), dim3(kPixBlockX, kPixBlockY), 0, stream>>>(img, gx, gy, w, h);
    return cudaGetLastError();
}

cudaError_t disLaunchTensors(DisPair gx, DisPair gy, int w, int h, DisGrid grid, double minTensorDet,
                             DisPatchPair patches, cudaStream_t stream) {
    const long long count = static_cast<long long>(grid.cols) * grid.rows;
    if (w <= 0 || h <= 0 || grid.cols <= 0 || grid.rows <= 0 || count > 0x7fffffffLL || grid.ps < 2 ||
        grid.ps > kDisMaxPatchSize || grid.stride < 1 || patches.p[0] == nullptr || patches.p[1] == nullptr) {
        return cudaErrorInvalidValue;
    }
    const dim3 blocks((static_cast<unsigned>(count) + kPatchBlock - 1u) / kPatchBlock, 2u);
    disTensorKernel<<<blocks, kPatchBlock, 0, stream>>>(gx, gy, w, h, grid, minTensorDet, patches);
    return cudaGetLastError();
}

cudaError_t disLaunchSolve(DisPair from, DisPair to, DisPair gx, DisPair gy, int w, int h, DisGrid grid,
                           DisPatchPair patches, DisPair coarseU, DisPair coarseV, int coarseW, int coarseH,
                           int seeded, const DisSolveConsts& consts, int multiprocessors, cudaStream_t stream) {
    const long long count = static_cast<long long>(grid.cols) * grid.rows;
    if (w <= 0 || h <= 0 || grid.cols <= 0 || grid.rows <= 0 || count > 0x7fffffffLL || grid.ps < 2 ||
        grid.ps > kDisMaxPatchSize || consts.iterations < 1 || patches.p[0] == nullptr ||
        patches.p[1] == nullptr) {
        return cudaErrorInvalidValue;
    }
    // A seeded solve must have a coarse field to read.
    if (seeded != 0 && (coarseW <= 0 || coarseH <= 0 || coarseU.p[0] == nullptr || coarseU.p[1] == nullptr ||
                        coarseV.p[0] == nullptr || coarseV.p[1] == nullptr)) {
        return cudaErrorInvalidValue;
    }
    const dim3 blocks((static_cast<unsigned>(count) + kPatchBlock - 1u) / kPatchBlock, 2u);
    // DJI's patch side gets the lane-group solve; any other side runs the
    // general one-thread-per-patch kernel.
    if (grid.ps == 8) {
        // Lanes per patch.  A level with few patches is bound by one patch's
        // serial path, so it gets the most lanes (8: one window row each);
        // a level with many is bound by the FP64 pipe, which a partly idle
        // warp occupies as long as a full one, so it gets fewer.  The rule -
        // the most lanes that keep resident warps at or below
        // kSolveWarpsPerSm per SM - picked 2 / 8 / 8 lanes for the three
        // levels of a 2048 x 68 band, measured best at each level on the
        // RTX 5090 (138 / 86 / 83 us against 155-268 / 97-127 / 96-125).
        const unsigned long long budget = static_cast<unsigned long long>(multiprocessors > 0 ? multiprocessors : 1) *
                                          kSolveWarpsPerSm * kPatchBlock;
        const unsigned long long patchesBothWays = static_cast<unsigned long long>(count) * 2ull;
        int lanes = 8;
        while (lanes > 2 && patchesBothWays * static_cast<unsigned long long>(lanes) > budget) {
            lanes /= 2;
        }
        const unsigned threads = static_cast<unsigned>(count) * static_cast<unsigned>(lanes);
        const dim3 groupBlocks((threads + kPatchBlock - 1u) / kPatchBlock, 2u);
        if (lanes == 8) {
            disSolveGroupKernel<8, 8><<<groupBlocks, kPatchBlock, 0, stream>>>(
                from, to, gx, gy, w, h, grid, patches, coarseU, coarseV, coarseW, coarseH, seeded, consts);
        } else if (lanes == 4) {
            disSolveGroupKernel<8, 4><<<groupBlocks, kPatchBlock, 0, stream>>>(
                from, to, gx, gy, w, h, grid, patches, coarseU, coarseV, coarseW, coarseH, seeded, consts);
        } else {
            disSolveGroupKernel<8, 2><<<groupBlocks, kPatchBlock, 0, stream>>>(
                from, to, gx, gy, w, h, grid, patches, coarseU, coarseV, coarseW, coarseH, seeded, consts);
        }
    } else {
        disSolveKernel<0><<<blocks, kPatchBlock, 0, stream>>>(from, to, gx, gy, w, h, grid, patches, coarseU,
                                                              coarseV, coarseW, coarseH, seeded, consts);
    }
    return cudaGetLastError();
}

cudaError_t disLaunchDensify(DisPatchPair patches, DisGrid grid, int w, int h, DisPairW u, DisPairW v,
                             cudaStream_t stream) {
    if (w <= 0 || h <= 0 || grid.cols <= 0 || grid.rows <= 0 || grid.stride < 1 || patches.p[0] == nullptr ||
        patches.p[1] == nullptr || u.p[0] == nullptr || u.p[1] == nullptr || v.p[0] == nullptr ||
        v.p[1] == nullptr) {
        return cudaErrorInvalidValue;
    }
    disDensifyKernel<<<pixelGrid(w, h, 2), dim3(kPixBlockX, kPixBlockY), 0, stream>>>(patches, grid, w, h, u, v);
    return cudaGetLastError();
}

cudaError_t disLaunchConsistency(const float* fu, const float* fv, const float* bu, const float* bv, int w, int h,
                                 double tol2, std::uint8_t* ok, unsigned long long* count, cudaStream_t stream) {
    if (fu == nullptr || fv == nullptr || bu == nullptr || bv == nullptr || ok == nullptr || count == nullptr ||
        w <= 0 || h <= 0) {
        return cudaErrorInvalidValue;
    }
    disConsistencyKernel<<<pixelGrid(w, h, 1), dim3(kPixBlockX, kPixBlockY), 0, stream>>>(fu, fv, bu, bv, w, h, tol2,
                                                                                         ok, count);
    return cudaGetLastError();
}

bool analysisKernelsLoadable(cudaError_t* why) {
    // Probing one kernel is enough: every kernel of the analyses is compiled
    // for the same architecture list.  The solve is the one that matters
    // most, so it is the probe.
    cudaFuncAttributes attr{};
    const cudaError_t err = cudaFuncGetAttributes(&attr, disSolveGroupKernel<8, 8>);
    if (why != nullptr) {
        *why = err;
    }
    return err == cudaSuccess;
}

}  // namespace osv::render::gpu
