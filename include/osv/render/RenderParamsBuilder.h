// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// RenderParamsBuilder: converts the double-precision geometry objects
// (LensRig, VirtualCamera / EquirectMap, stabilisation rotation, blend and
// gain settings) plus the colour parameter block into the float POD
// OsvRenderParams consumed by every renderer backend.
//
// Usage:
//   RenderJob job = RenderParamsBuilder()
//       .rig(rig).camera(cam).stabilization(R).blend(blend).color(params)
//       .build(framePair).value();
#pragma once

#include "osv/core/Math.h"
#include "osv/core/Result.h"
#include "osv/geom/Blend.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/VirtualCamera.h"
#include "osv/render/RenderJob.h"

#include <array>
#include <optional>
#include <vector>

namespace osv::render {

// [WP-PHOTO] Defined in PhotoSeam.h, which includes this header.
struct PhotoSeamField;
struct PhotoSeamParams;

class RenderParamsBuilder {
public:
    RenderParamsBuilder();

    /// Lens intrinsics/extrinsics in stream pixels (required).
    RenderParamsBuilder& rig(const geom::LensRig& rig);

    /// Virtual camera for reframe mode.  A camera with Projection::Equirect
    /// switches to equirect mode with the Standard layout.
    RenderParamsBuilder& camera(const geom::VirtualCamera& camera);

    /// Full equirect output (Standard or PolarAxis layout).
    RenderParamsBuilder& equirect(const geom::EquirectMap& map);

    /// Body-from-world stabilisation rotation (identity when omitted).
    RenderParamsBuilder& stabilization(const Mat3d& bodyFromWorld);

    /// Feather / occlusion settings; `enabled = false` selects nearest-lens.
    RenderParamsBuilder& blend(const geom::BlendParams& params, bool enabled = true);

    /// Per-lens linear-light gains (exposure matching); default 1.
    RenderParamsBuilder& gain(const Vec3d& slaveGain, const Vec3d& masterGain);

    /// Per-column seam shift table in degrees (polar-axis longitude columns).
    RenderParamsBuilder& seam(const std::vector<float>& shiftDeg);

    /// 2-D parallax warp grid over the overlap band (see ParallaxWarp.h).
    ///
    /// `uv` is interleaved (u, v) half-corrections in radians, w * h pairs;
    /// `latMinRad` and `latMaxRad` are the latitudes of grid rows 0 and
    /// h - 1.  An empty grid, or one whose size does not match w * h, leaves
    /// the warp disabled rather than half-configured.
    RenderParamsBuilder& warp(const std::vector<float>& uv, std::uint32_t w, std::uint32_t h, float latMinRad,
                              float latMaxRad);

    /// Remove any previously set warp grid (A/B comparison).
    RenderParamsBuilder& clearWarp();

    /// [WP-SEAM] Carved blend-seam table (see SeamCarve.h and osv_kernel.h).
    ///
    /// `table` is interleaved (latitude, feather half width) radian pairs,
    /// `columns` of them, over the polar-axis longitude ring; `edgeRad` is
    /// the validity ramp below each lens's thetaMax.  A table whose size does
    /// not match, a non-finite or out-of-range entry, or a non-finite /
    /// negative ramp leaves the seam disabled rather than half-configured.
    RenderParamsBuilder& blendSeam(const std::vector<float>& table, std::uint32_t columns, float edgeRad);

    /// [WP-SEAM] Remove any previously set blend-seam table (A/B comparison).
    RenderParamsBuilder& clearBlendSeam();

    /// [WP-PHOTO] Photometric seam field (PhotoSeam.h): the per-longitude
    /// usable rim and the 2-D log gain, applied as `params.mode` says (Off
    /// clears it, RimOnly drops the gain, RimAndGain applies both at
    /// `params.strength`).  A field that is not valid() leaves the photo
    /// table disabled rather than half-configured.
    RenderParamsBuilder& photo(const PhotoSeamField& field, const PhotoSeamParams& params);

    /// [WP-PHOTO] Remove any previously set photometric seam field.
    RenderParamsBuilder& clearPhoto();

    /// Colour pipeline block from osv::color::makeColorParams (required).
    RenderParamsBuilder& color(const OsvColorParams& params);

    /// Output alpha = lens coverage (default) or constant 1.
    RenderParamsBuilder& alphaCoverage(bool enabled);

    /// Disable one lens (debug / per-lens NCC measurements).
    RenderParamsBuilder& lensEnabled(int lens, bool enabled);

    /// Assemble the job.  Fails with InvalidArgument when a required piece is
    /// missing or the frames do not match the rig size.
    [[nodiscard]] Result<RenderJob> build(const video::FramePair& frames) const;

    /// The parameter block alone (no frames), for callers that manage planes
    /// themselves such as the Premiere effect.
    [[nodiscard]] Result<OsvRenderParams> buildParams() const;

private:
    std::optional<geom::LensRig> m_rig;
    std::optional<geom::VirtualCamera> m_camera;
    std::optional<geom::EquirectMap> m_equirect;
    Mat3d m_bodyFromWorld = Mat3d::identity();
    geom::BlendParams m_blend;
    bool m_blendEnabled = true;
    std::array<Vec3d, 2> m_gain{Vec3d{1, 1, 1}, Vec3d{1, 1, 1}};
    std::vector<float> m_seam;
    std::vector<float> m_warp;
    std::uint32_t m_warpW = 0;
    std::uint32_t m_warpH = 0;
    float m_warpLatMin = 0.0f;
    float m_warpLatMax = 0.0f;
    // [WP-SEAM] carved blend seam (empty table = disabled)
    std::vector<float> m_blendSeam;
    std::uint32_t m_blendSeamColumns = 0;
    float m_blendSeamEdgeRad = 0.0f;
    // [WP-PHOTO] photometric seam field (empty table = disabled)
    std::vector<float> m_photo;        ///< Kernel table (gain grid, then rim).
    std::uint32_t m_photoW = 0;
    std::uint32_t m_photoH = 0;
    float m_photoLatMin = 0.0f;        ///< Radians.
    float m_photoLatMax = 0.0f;
    float m_photoDecay = 0.0f;         ///< Luma decay distance, radians.
    float m_photoChromaDecay = 0.0f;   ///< Chroma decay distance, radians.
    float m_photoRimFeather = 0.0f;    ///< Feather below the rim, radians; 0 = no rim.
    float m_photoStrength = 0.0f;      ///< Gain strength 0..1; 0 = no gain.
    std::optional<OsvColorParams> m_color;
    bool m_alphaCoverage = true;
    std::array<bool, 2> m_lensEnabled{true, true};
};

}  // namespace osv::render
