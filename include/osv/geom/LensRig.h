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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace osv::geom {

/// Where the stream-space focal length comes from.
enum class FocalSource {
    /// fx = fy = ClipMeta.digital_focal_length when it agrees with THIS lens's
    /// calibration * scale within 0.5 % (verified at 6K); otherwise the
    /// lens's own scaled calibration (8K-mode clips record a value 1.3-2.7 %
    /// off their lenses).
    DigitalFocalLength,
    ScaledCalibration,  ///< fx, fy = calibration focal lengths * StreamScaling.scale, always.
    /// fx = fy = ClipMeta.digital_focal_length whenever it describes this
    /// stream SIZE (within 1.2x of calibration * scale), the rule before
    /// 8K-mode clips were measured; the scaled calibration otherwise (the LRF
    /// proxy repeats its full-size clip's value).  Source Settings "Lens
    /// Focal: Camera".
    DigitalFocalLengthSameStream,
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

// =============================================================================
//  Hide Mount Auto: clipping an occlusion polygon to azimuth stretches
// =============================================================================
//
// The calibration's occlusion polygon (buildOcclusion in LensRig.cpp) is the
// sliver between the mount's arc and the rim of the image circle.  Seen from
// the lens centre it covers one stretch of POLAR ANGLE (azimuth about the
// optical axis, atan2(y - cy, x - cx) in fisheye pixels), and at every
// azimuth inside that stretch it starts at an inner radius (the arc) and ends
// at an outer one (the rim).
//
// Hide Mount Auto measures, per stretch of azimuth, whether the polygon hides
// the mount or only scene both lenses see, and rebuilds the polygon so that
//
//   * Keep    stretches are exactly the calibration's sliver (the same edges);
//   * Release stretches start beyond the usable image circle, so no pixel the
//               stitch ever weighs is inside or near the polygon there;
//   * Clamp   stretches are kept but start no closer to the centre than a
//               given radius (the lens that does not image the mount must be
//               fully trusted on the seam plane, or the coverage dips there).
//
// The result is still ONE simple polygon (an inner run forward in azimuth,
// an outer run back), so the render kernels' even-odd test and distance
// feather (osvOcclusionFactor) take it unchanged, and it is built once per
// clip - nothing per pixel or per frame changes.

/// How Hide Mount Auto treats one stretch of a lens's occlusion polygon.
enum class OcclusionSpanState : std::uint8_t {
    Keep = 0,     ///< The calibration's polygon, unchanged.
    Release = 1,  ///< Moved beyond the usable image circle: the lens is used there.
    Clamp = 2,    ///< Kept, but starting no closer to the lens centre than the clamp radius.
};

/// One stretch of polar angle about the lens centre, counter-clockwise from
/// `fromRad` to `toRad` (radians, the atan2(y - cy, x - cx) convention of the
/// fisheye image; any multiple of 2 pi is accepted).
struct OcclusionSpan {
    double fromRad = 0.0;  ///< Start of the stretch.
    double toRad = 0.0;    ///< End of the stretch (counter-clockwise from the start).
    OcclusionSpanState state = OcclusionSpanState::Keep;  ///< What happens to the polygon inside it.
};

/// Parameters of clipOcclusionPolygon.
struct OcclusionClipParams {
    /// Radius (px) beyond which no pixel of this lens is ever given weight:
    /// the usable image circle plus the occlusion feather.  Released stretches
    /// are moved beyond it, with margin for the chords between vertices.
    double usableRadiusPx = 0.0;
    /// Smallest inner radius (px) of a Clamp stretch.  Ignored by the others.
    double clampRadiusPx = 0.0;
    /// Vertex budget of the render kernels (OSV_MAX_OCCLUSION_POINTS): a
    /// polygon that would need more is refused (OutOfRange-like Unsupported),
    /// so the caller can merge stretches and try again.
    std::size_t maxVertices = 32;
};

/// Distances from `centre` along the ray at polar angle `angleRad` to the
/// nearest and the farthest crossing of `polygon`'s edges.  nullopt when the
/// ray misses the polygon, the polygon has fewer than three vertices, or any
/// input is not finite.
[[nodiscard]] std::optional<std::pair<double, double>> occlusionRayRadii(const std::vector<Vec2d>& polygon,
                                                                         const Vec2d& centre,
                                                                         double angleRad) noexcept;

/// Rebuild an occlusion polygon for Hide Mount Auto (see the block comment
/// above).  `spans` that do not cover part of the polygon leave it Keep there.
/// Returns the polygon UNCHANGED (the same vertices, bit for bit) when every
/// stretch it covers is Keep, or Clamp with a clamp radius inside which the
/// polygon never reaches: a clip whose verdict keeps everything renders
/// exactly as with the calibration's mask.
///
/// Fails with InvalidArgument for malformed input (non-finite values, fewer
/// than three vertices, a non-positive usable radius) and Unsupported for a
/// polygon this cannot express as one inner and one outer run (it surrounds
/// the lens centre or spans more than 300 deg) or one that would need more
/// than `params.maxVertices` vertices.
[[nodiscard]] Result<std::vector<Vec2d>> clipOcclusionPolygon(const std::vector<Vec2d>& polygon, const Vec2d& centre,
                                                              const std::vector<OcclusionSpan>& spans,
                                                              const OcclusionClipParams& params);

}  // namespace osv::geom
