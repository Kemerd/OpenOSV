// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Extrinsic quaternion interpretation.

#include "osv/geom/Extrinsics.h"

#include <cmath>

namespace osv::geom {

const char* quatOrderName(QuatOrder order) noexcept {
    switch (order) {
    case QuatOrder::WXYZ: return "WXYZ";
    case QuatOrder::XYZW: return "XYZW";
    }
    return "Unknown";
}

const char* rotationSenseName(RotationSense sense) noexcept {
    switch (sense) {
    case RotationSense::BodyToLens: return "BodyToLens";
    case RotationSense::LensToBody: return "LensToBody";
    }
    return "Unknown";
}

Quatd extrinsicQuat(const meta::Quaternion& q, QuatOrder order) noexcept {
    // An absent record has nothing to say: identity keeps the pipeline alive.
    if (!q.present) {
        return Quatd::identity();
    }
    // Reinterpret the four stored floats according to the requested order.
    const Quatd raw = (order == QuatOrder::XYZW) ? q.toQuatdXYZW() : q.toQuatdWXYZ();
    if (!raw.isFinite()) {
        return Quatd::identity();
    }
    // normalized() already returns identity for a zero quaternion.
    return raw.normalized();
}

Mat3d bodyToLensMatrix(const meta::Quaternion& q, const ExtrinsicConvention& convention) noexcept {
    const Mat3d r = extrinsicQuat(q, convention.order).toMatrix();
    // The verified reading is d_lens = R(q) * d_body; the alternative stores
    // the inverse rotation, so transpose to keep the output meaning fixed.
    return (convention.sense == RotationSense::LensToBody) ? r.transposed() : r;
}

}  // namespace osv::geom
