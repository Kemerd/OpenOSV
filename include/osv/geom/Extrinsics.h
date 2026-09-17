// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Interpretation of the per-lens extrinsic quaternion (DewarpParams.cam_extri_q).
//
// Verified convention (docs/GEOMETRY.md): the stored floats are (w, x, y, z)
// and the rotation maps body-frame directions into the lens frame,
//
//     d_lens = R(q) * d_body
//
// Body frame: X right, Y forward (= master optical axis; the slave looks
// along -Y), Z up.  Both lens images have +y (image down) pointing to -Z of
// the body.  The alternative readings are kept selectable so a clip from a
// different firmware can be checked with the convention tests.
#pragma once

#include "osv/core/Math.h"
#include "osv/meta/Types.h"

namespace osv::geom {

/// Component order of a four-float quaternion as stored in the metadata.
enum class QuatOrder { WXYZ, XYZW };

/// Direction of the extrinsic rotation.
enum class RotationSense {
    BodyToLens,  ///< d_lens = R(q) * d_body  (verified on the Osmo 360)
    LensToBody   ///< d_body = R(q) * d_lens
};

/// The pair of switches that define how cam_extri_q is read.
struct ExtrinsicConvention {
    QuatOrder order = QuatOrder::WXYZ;
    RotationSense sense = RotationSense::BodyToLens;
};

/// Stable names for logs / JSON.
[[nodiscard]] const char* quatOrderName(QuatOrder order) noexcept;
[[nodiscard]] const char* rotationSenseName(RotationSense sense) noexcept;

/// Convert a stored quaternion to a unit Quatd using the given component
/// order.  A quaternion that is not present, not finite or zero yields
/// identity.
[[nodiscard]] Quatd extrinsicQuat(const meta::Quaternion& q, QuatOrder order) noexcept;

/// Rotation matrix mapping body-frame directions to the lens frame under the
/// given convention (the LensToBody reading is transposed so the output
/// always means body -> lens).  Identity for an absent / degenerate quaternion.
[[nodiscard]] Mat3d bodyToLensMatrix(const meta::Quaternion& q, const ExtrinsicConvention& convention) noexcept;

}  // namespace osv::geom
