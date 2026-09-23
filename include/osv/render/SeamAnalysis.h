// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Seam analysis on the polar-axis equirect band where both lenses overlap:
//   * overlapNcc   - the regression metric (how well the two lenses agree)
//   * searchSeam   - per-column 1-D disparity search that feeds the kernel's
//                    seam table (parallax correction for near objects)
//   * estimateGain - exposure / white balance matching between the lenses
//
// All three render each lens alone into a small polar-axis equirect band with
// the CPU reference renderer, so they are backend independent and exercise
// exactly the mapping the final render uses.
//
// Frames that live in VRAM (a keepOnDevice decode, RenderJob::planesOnDevice)
// are shaded by the installed DeviceBandShader instead - the same shared
// osvShadePixelW on the GPU (see DeviceBandShader.h, installCudaAnalyses()).
// Without one installed, such frames are refused with InvalidArgument rather
// than read as host memory.
#pragma once

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <vector>

namespace osv::render {

/// Band geometry shared by the three analyses.
struct BandParams {
    std::uint32_t equirectW = 2048;  ///< Width of the polar-axis map (columns = longitude).
    double bandHalfDeg = 6.0;        ///< Half height of the analysed band around the seam (deg).
};

/// Per-lens luma band images (code space) plus coverage.
struct LensBands {
    std::uint32_t w = 0;         ///< Band width (columns).
    std::uint32_t h = 0;         ///< Band height (rows).
    std::uint32_t rowOffset = 0; ///< First equirect row of the band.
    std::uint32_t mapH = 0;      ///< Full equirect height (for row -> degree).
    std::vector<float> luma[2];  ///< Luma per lens, row-major w*h.
    std::vector<float> alpha[2]; ///< Coverage per lens.
};

/// A 2-D parallax warp grid as the band renderer needs to see it.
///
/// Deliberately a plain view rather than a ParallaxWarpGrid: SeamAnalysis is
/// the LOWER layer (ParallaxWarp.h includes it, not the other way round), so
/// taking the full type here would be a circular dependency.  Pointing at the
/// caller's data also keeps the measurement path free of a copy of a grid it
/// only reads.
struct WarpGridView {
    const float* uv = nullptr;  ///< Interleaved (u, v) radians, w * h pairs.
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    float latMinRad = 0.0f;     ///< Latitude of row 0.
    float latMaxRad = 0.0f;     ///< Latitude of row h - 1.

    [[nodiscard]] bool valid() const noexcept { return uv != nullptr && w > 0 && h > 1; }
};

/// Render the two per-lens luma bands.  `linear` selects scene-linear values
/// (gain estimation) instead of D-Log M codes (matching).
///
/// `warp` (optional) applies a 2-D parallax grid while rendering, which is
/// what makes an after-correction NCC measure the same geometry the real
/// render path produces rather than an approximation of it.
Result<LensBands> renderLensBands(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, bool linear,
                                  const std::vector<float>* seamTable, ThreadPool& pool,
                                  const WarpGridView* warp = nullptr);

/// Normalised cross-correlation of the two lenses over the co-visible band.
/// Returns a value in [-1, 1]; 0 when nothing is co-visible.
Result<double> overlapNcc(const geom::LensRig& rig, const video::FramePair& frames, const geom::BlendParams& blend,
                          const BandParams& band, ThreadPool& pool, const std::vector<float>* seamTable = nullptr,
                          const WarpGridView* warp = nullptr);

struct SeamSearchParams {
    BandParams band;
    int maxShiftPx = 24;          ///< Search range in band rows (+/-).
    int windowHalfCols = 4;       ///< Half width of the matching window (columns).
    double smoothSigmaCols = 8.0; ///< Gaussian smoothing of the profile along longitude.
    double minNcc = 0.5;          ///< Columns below this correlation inherit their neighbours.
};

struct SeamProfile {
    std::uint32_t columns = 0;
    std::vector<float> shiftDeg;  ///< Disparity per column in degrees (kernel seam table).
    std::vector<float> ncc;       ///< Best correlation per column (before smoothing).
    double meanNcc = 0.0;         ///< Mean of the accepted columns.
    std::uint32_t acceptedColumns = 0;
};

/// Per-column disparity search.  Positive values mean the same feature sits
/// farther from both lens axes than the calibration predicts (near object).
Result<SeamProfile> searchSeam(const geom::LensRig& rig, const video::FramePair& frames,
                               const geom::BlendParams& blend, const SeamSearchParams& params, ThreadPool& pool);

struct GainEstimate {
    Vec3d gain[2] = {Vec3d{1, 1, 1}, Vec3d{1, 1, 1}};  ///< Per-lens linear gains (slave, master).
    Vec3d overlapMean[2] = {};                          ///< Mean linear RGB per lens in the band.
    std::uint64_t samples = 0;                          ///< Co-visible pixels used.
};

/// Symmetric per-channel gains g0 = sqrt(m1/m0), g1 = 1/g0 (clamped to
/// [0.5, 2]) that make the two lenses agree in the overlap band.
Result<GainEstimate> estimateGain(const geom::LensRig& rig, const video::FramePair& frames,
                                  const geom::BlendParams& blend, const BandParams& band, ThreadPool& pool);

}  // namespace osv::render
