// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaAnalysisLaunch.h - interface between the MSVC-compiled host code of the
// GPU analyses (CudaAnalysis.cpp) and the nvcc-compiled kernels
// (CudaBandKernel.cu, CudaDisKernel.cu).
//
// Same split as CudaLaunch.h and for the same reason: nvcc's device code
// generator chokes on the Result / std::variant machinery, so the kernels and
// their launchers see only plain structs, raw pointers and cudaError_t, and
// everything with an error message lives on the MSVC side.
//
// Every launcher is asynchronous on the stream it is given and returns the
// launch status (cudaGetLastError) - an execution fault surfaces at the
// caller's stream synchronisation, not here.  None of them allocates, sets a
// device or synchronises anything: they run in whatever context is current,
// which is the whole point when that context is Premiere's.

#pragma once

#include "CudaLaunch.h"

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace osv::render::gpu {

// ===========================================================================
//  Band shading (the rows of a polar-axis band, one lens per job)
// ===========================================================================

/// Shade rows [row0, row0 + rows) of the map `params` describes into tightly
/// packed RGBA floats: out[(r * outW + x) * 4 + c].
cudaError_t launchBandRgba(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                           const float* warp, int row0, int rows, float* out, cudaStream_t stream);

/// Shade the same rows and keep only luma (BT.2020 weights, the exact
/// expression SeamAnalysis.cpp uses) and coverage: luma[r * outW + x],
/// alpha[r * outW + x].
cudaError_t launchBandLumaAlpha(const OsvRenderParams& params, const OsvPlanePair& planes, const float* seam,
                                const float* warp, int row0, int rows, float* luma, float* alpha,
                                cudaStream_t stream);

// ===========================================================================
//  Dense Inverse Search
//
//  Every kernel below processes BOTH images (or both directions) of a
//  bidirectional solve in one launch - blockIdx.y (or .z) selects which -
//  because the problem is small (a 2048 x 68 band) and a launch costs about
//  as much as the arithmetic of a pass.  The two directions share their
//  pyramids and gradients: direction 0 solves A -> B from A's structure
//  tensors, direction 1 solves B -> A from B's.
// ===========================================================================

/// Largest patch side the GPU solver accepts.  The per-patch template and
/// target windows live in per-thread arrays, which must have a fixed size;
/// DJI's 8 fits with room to spare, and anything larger is refused so the
/// caller falls back to the CPU solver rather than overflowing.
inline constexpr int kDisMaxPatchSize = 16;

/// Largest Gaussian radius (taps each side) a blur launch carries.  Radius is
/// ceil(3 sigma): 3 for the pyramid's 0.8, 5 for the default flow smoothing.
inline constexpr int kDisMaxBlurRadius = 64;

/// Planes one batched blur / decimate launch can carry: two images during
/// pyramid construction, (u, v) x two directions during flow smoothing.
inline constexpr int kDisMaxBatch = 4;

/// A normalised 1-D Gaussian, built on the HOST with exactly the arithmetic
/// DisFlow.cpp uses (double exp, float taps, float multiply by a float
/// reciprocal of the double sum) so both backends blur with identical taps.
struct DisBlurTaps {
    int radius = 0;
    float k[2 * kDisMaxBlurRadius + 1] = {};
};

/// Up to kDisMaxBatch (src, dst) plane pairs of one size, row-major, pitch =
/// width.  blockIdx.z selects the pair; entries at or past `count` are
/// ignored.
struct DisPlaneBatch {
    const float* src[kDisMaxBatch] = {};
    float* dst[kDisMaxBatch] = {};
    int count = 0;
};

/// Two read-only planes (one per image / direction).
struct DisPair {
    const float* p[2] = {};
};

/// Two writable planes (one per image / direction).
struct DisPairW {
    float* p[2] = {};
};

/// One patch's state - the same fields, meaning and float widths as
/// DisFlow.cpp's Patch, so each stage can mirror the CPU arithmetic exactly.
struct DisPatch {
    float x;        ///< Patch centre in level pixels.
    float y;
    float u;        ///< Solved displacement.
    float v;
    float quality;  ///< 1 / max(1, mean |residual|); 0 = rejected.
    float iHxx;     ///< Inverse structure tensor, symmetric.
    float iHxy;
    float iHyy;
    int usable;     ///< 0 when the tensor was singular or the solve failed.
};

/// Patches of both directions: p[0] from image A's tensors, p[1] from B's.
struct DisPatchPair {
    DisPatch* p[2] = {};
};

/// The patch grid of one pyramid level (DisFlow.cpp precomputeTensors).
struct DisGrid {
    int ps = 8;      ///< Patch side.
    int half = 4;    ///< ps / 2: centre offset from the patch's first pixel.
    int stride = 5;  ///< Pixels between neighbouring patch centres.
    int cols = 0;    ///< Patches per row.
    int rows = 0;    ///< Patch rows.
};

/// The constants of the inverse-search iteration, pre-converted on the host
/// exactly as DisFlow.cpp converts them at their point of use.
struct DisSolveConsts {
    int iterations = 12;             ///< Already max(1, params.iterations).
    float stepScale = 0.4f;          ///< static_cast<float>(params.stepScale).
    double minStepPx = 0.01;
    double maxDisplacementPx = 24.0;
    double maxPatchSsd = 1.0e7;
};

/// dst = src * scale (or a plain copy when applyScale is 0) for both images,
/// src rows `srcPitch*` floats apart, dst tightly packed.  src may alias dst.
cudaError_t disLaunchScale(const float* src0, std::size_t srcPitch0, const float* src1, std::size_t srcPitch1,
                           float* dst0, float* dst1, int w, int h, float scale, int applyScale,
                           cudaStream_t stream);

/// Horizontal Gaussian pass, clamp-to-edge: dst row y from src row y.
cudaError_t disLaunchBlurRows(const DisPlaneBatch& batch, int w, int h, const DisBlurTaps& taps,
                              cudaStream_t stream);

/// Vertical Gaussian pass, clamp-to-edge: dst row y from src rows y +/- r.
cudaError_t disLaunchBlurCols(const DisPlaneBatch& batch, int w, int h, const DisBlurTaps& taps,
                              cudaStream_t stream);

/// 2x2 box decimation of (already blurred) src planes srcW x srcH into dst
/// planes dstW x dstH, clamp-to-edge.
cudaError_t disLaunchDecimate(const DisPlaneBatch& batch, int srcW, int srcH, int dstW, int dstH,
                              cudaStream_t stream);

/// Central-difference gradients of both images, clamp-to-edge.
cudaError_t disLaunchGradients(DisPair img, DisPairW gx, DisPairW gy, int w, int h, cudaStream_t stream);

/// Lay out both patch grids and precompute every inverse structure tensor
/// (double accumulation, raw-determinant test against minTensorDet).
cudaError_t disLaunchTensors(DisPair gx, DisPair gy, int w, int h, DisGrid grid, double minTensorDet,
                             DisPatchPair patches, cudaStream_t stream);

/// Seed every patch of both directions from the coarser level's field
/// (when `seeded`, coarse planes coarseW x coarseH) and run the damped
/// inverse search.  Direction d solves from[d] -> to[d] with gx[d] / gy[d].
/// `multiprocessors` (the device's SM count) sizes the lane groups of the
/// solve to the level's patch count; it changes speed, never results.
cudaError_t disLaunchSolve(DisPair from, DisPair to, DisPair gx, DisPair gy, int w, int h, DisGrid grid,
                           DisPatchPair patches, DisPair coarseU, DisPair coarseV, int coarseW, int coarseH,
                           int seeded, const DisSolveConsts& consts, int multiprocessors, cudaStream_t stream);

/// Densify both directions: every pixel is the quality-weighted mean of the
/// usable patches covering it, summed in the CPU's order (patch rows top to
/// bottom, each left to right) so the float sums round identically.
cudaError_t disLaunchDensify(DisPatchPair patches, DisGrid grid, int w, int h, DisPairW u, DisPairW v,
                             cudaStream_t stream);

/// Forward-backward consistency: ok[i] = 1 where following the forward field
/// and then the backward field returns within sqrt(tol2) pixels.  `count`
/// (one device word, zeroed by the caller) receives the number of 1s.
cudaError_t disLaunchConsistency(const float* fu, const float* fv, const float* bu, const float* bv, int w, int h,
                                 double tol2, std::uint8_t* ok, unsigned long long* count, cudaStream_t stream);

/// True when this build's fatbin holds code the CURRENT device can run
/// (SASS for its architecture or PTX it can JIT).  Probed with
/// cudaFuncGetAttributes, which loads the module without launching anything.
/// On failure `why` receives the runtime's error.
bool analysisKernelsLoadable(cudaError_t* why);

}  // namespace osv::render::gpu
