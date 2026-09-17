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
    std::optional<OsvColorParams> m_color;
    bool m_alphaCoverage = true;
    std::array<bool, 2> m_lensEnabled{true, true};
};

}  // namespace osv::render
