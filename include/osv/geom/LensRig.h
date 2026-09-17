// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensRig: the two calibrated lenses of a clip expressed in stream pixel
// units together with their body->lens rotations and occlusion polygons.
// Everything a renderer needs to turn a body-frame ray into a pixel of
// either video stream lives here.
#pragma once

#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/geom/Extrinsics.h"
#include "osv/geom/KannalaBrandt5.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/Types.h"

#include <array>
#include <string>
#include <vector>

namespace osv::geom {

/// Where the stream-space focal length comes from.
enum class FocalSource {
    DigitalFocalLength,  ///< fx = fy = ClipMeta.digital_focal_length (verified at 6K).
    ScaledCalibration    ///< fx, fy = calibration focal lengths * StreamScaling.scale.
};

/// Stable name for logs / JSON.
[[nodiscard]] const char* focalSourceName(FocalSource source) noexcept;

/// Index of each lens in the LensRig arrays (== video stream id).
enum LensIndex : int {
    kSlaveLens = 0,   ///< Video track 1 / stream 0, optical axis -Y body.
    kMasterLens = 1,  ///< Video track 2 / stream 1, optical axis +Y body.
    kLensCount = 2
};

/// The stitched pair of lenses in stream pixel units.
struct LensRig {
    std::array<KannalaBrandt5, kLensCount> lens{};                     ///< Intrinsics in stream px.
    std::array<Mat3d, kLensCount> bodyToLens{};                        ///< d_lens = bodyToLens[i] * d_body.
    std::array<std::vector<Vec2d>, kLensCount> occlusionPolyStream{};  ///< Occlusion polygon, stream px.
    int streamW = 0;                                                   ///< Stream width (px).
    int streamH = 0;                                                   ///< Stream height (px).
    StreamScaling scaling;                                             ///< Sensor -> stream mapping used.
    ExtrinsicConvention convention;                                    ///< How cam_extri_q was read.
    FocalSource focalSource = FocalSource::DigitalFocalLength;         ///< Where fx/fy came from.
    double lensFovDeg = 195.18;                                        ///< Usable lens FOV (thetaMax = half).
    std::vector<std::string> notes;                                    ///< Human readable build notes.

    /// Build the rig.  `calibration` is in sensor (calibration) pixels; the
    /// intrinsics and occlusion polygons are mapped through `scaling`.
    /// The stream size is taken from the scaling's destination centre
    /// (2 * dstCx, 2 * dstCy).  Fails when a calibration record lacks its
    /// core fields or the resulting intrinsics are not finite.
    [[nodiscard]] static Result<LensRig> build(const meta::CalibrationSet& calibration, const StreamScaling& scaling,
                                               FocalSource focalSource, double digitalFocalLength,
                                               const ExtrinsicConvention& convention, double lensFovDeg = 195.18);

    /// Optical axis of lens `i` expressed in the body frame (+Y for the master).
    [[nodiscard]] Vec3d opticalAxisBody(int i) const noexcept;
    /// Image-right direction of lens `i` in the body frame.
    [[nodiscard]] Vec3d imageRightBody(int i) const noexcept;
    /// Image-down direction of lens `i` in the body frame (-Z on the Osmo 360).
    [[nodiscard]] Vec3d imageDownBody(int i) const noexcept;

    /// Project a body-frame direction through lens `i`.  Returns false when
    /// the index is bad or the ray is outside the lens's usable FOV.
    [[nodiscard]] bool projectBody(int i, const Vec3d& dBody, Vec2d& px, double& theta) const noexcept;

    /// True when `i` is a valid lens index.
    [[nodiscard]] static constexpr bool validIndex(int i) noexcept { return i >= 0 && i < kLensCount; }
};

}  // namespace osv::geom
