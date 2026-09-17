// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// LensRig construction: calibration records -> stream-space lenses.

#include "osv/geom/LensRig.h"

#include "osv/core/Log.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace osv::geom {

const char* focalSourceName(FocalSource source) noexcept {
    switch (source) {
    case FocalSource::DigitalFocalLength: return "DigitalFocalLength";
    case FocalSource::ScaledCalibration: return "ScaledCalibration";
    }
    return "Unknown";
}

namespace {

/// Build one stream-space lens from a calibration record.
Result<KannalaBrandt5> buildLens(const meta::DewarpParams& params, const char* name, const StreamScaling& scaling,
                                 FocalSource focalSource, double digitalFocalLength, double thetaMaxRad,
                                 std::vector<std::string>& notes) {
    // The record must carry the projection core (fx, fy, cx, cy, size, q).
    if (!params.hasCore()) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("LensRig: {} calibration lacks fx/fy/cx/cy/size/extrinsic", name)};
    }
    // Start from the calibration-space model, then rescale.
    KannalaBrandt5 lens = KannalaBrandt5::fromDewarp(params);
    if (!(lens.fx > 0.0) || !(lens.fy > 0.0)) {
        return Error{ErrorCode::InvalidArgument, std::format("LensRig: {} calibration has non-positive focal", name)};
    }
    if (!(scaling.scale > 0.0) || !std::isfinite(scaling.scale)) {
        return Error{ErrorCode::InvalidArgument, std::format("LensRig: bad stream scale {}", scaling.scale)};
    }

    // Principal point: same similarity transform as every other sensor point.
    const Vec2d c = scaling.apply(Vec2d{lens.cx, lens.cy});
    lens.cx = c.x;
    lens.cy = c.y;

    // Focal length: either the camera's digital_focal_length (verified equal
    // to fx_cal * scale at 6K) or the scaled calibration values.
    //
    // digital_focal_length is only usable when it actually describes THIS
    // stream.  The verified 6K invariant is digital_focal_length ==
    // fx_cal * scale, and that is what makes the field trustworthy; when a
    // clip carries a value that contradicts its own calibration by a wide
    // margin the field is stale, not authoritative.
    //
    // The LRF proxy is exactly such a clip.  Its ClipMeta repeats the 6K
    // camera's digital_focal_length (829.3612 px, correct for a 3000 px
    // stream) verbatim, while its lens images are only 1024 px across and
    // want roughly 283 px.  Taking the field at face value there builds a
    // lens nearly three times too long: the fisheye circle collapses to a
    // small disc in the middle of the equirect and the panorama is mostly
    // empty.  Falling back to the calibration - which IS expressed in sensor
    // pixels and is mapped through `scaling` to this exact stream - restores
    // a correct rig for the proxy and cannot change the verified modes,
    // where the two agree to a fraction of a percent.
    //
    // The tolerance is deliberately loose (a factor of 1.2 either way).  It
    // is a staleness detector, not a precision check: real per-lens
    // manufacturing spread against the shared digital_focal_length is well
    // under a percent, while a wrong-resolution value is off by the ratio of
    // the two stream sizes.
    const double calFocalScaled = 0.5 * (params.fx + params.fy) * scaling.scale;
    constexpr double kFocalAgreementTolerance = 1.2;
    const bool dflUsable = std::isfinite(digitalFocalLength) && digitalFocalLength > 0.0;
    const bool dflAgreesWithCalibration =
        dflUsable && std::isfinite(calFocalScaled) && calFocalScaled > 0.0 &&
        digitalFocalLength <= calFocalScaled * kFocalAgreementTolerance &&
        digitalFocalLength >= calFocalScaled / kFocalAgreementTolerance;

    if (focalSource == FocalSource::DigitalFocalLength && dflUsable && dflAgreesWithCalibration) {
        lens.fx = digitalFocalLength;
        lens.fy = digitalFocalLength;
        notes.push_back(std::format("{}: focal {:.4f} px from digital_focal_length (calibration * scale = {:.4f})",
                                    name, digitalFocalLength, calFocalScaled));
    } else if (focalSource == FocalSource::DigitalFocalLength && dflUsable && !dflAgreesWithCalibration) {
        // Stale or mis-scaled metadata: say so loudly (once per lens) and use
        // the calibration, which is tied to this stream through `scaling`.
        lens.fx *= scaling.scale;
        lens.fy *= scaling.scale;
        notes.push_back(std::format(
            "{}: digital_focal_length {:.4f} px disagrees with calibration * scale {:.4f} px by {:.2f}x; "
            "it does not describe this stream, using the scaled calibration focal {:.4f}/{:.4f} px",
            name, digitalFocalLength, calFocalScaled, digitalFocalLength / calFocalScaled, lens.fx, lens.fy));
        log::warn("LensRig: {} digital_focal_length {:.4f} does not match this stream (calibration * scale {:.4f}); "
                  "using the scaled calibration focal",
                  name, digitalFocalLength, calFocalScaled);
    } else {
        if (focalSource == FocalSource::DigitalFocalLength) {
            notes.push_back(std::format("{}: digital_focal_length unusable ({}), using scaled calibration focal",
                                        name, digitalFocalLength));
        }
        lens.fx *= scaling.scale;
        lens.fy *= scaling.scale;
        notes.push_back(std::format("{}: focal {:.4f}/{:.4f} px from calibration * scale", name, lens.fx, lens.fy));
    }

    lens.thetaMaxRad = thetaMaxRad;
    lens.updateRMax();
    if (!lens.isValid()) {
        return Error{ErrorCode::Internal, std::format("LensRig: {} lens is not finite after scaling", name)};
    }
    // Warn (do not fail) when the model folds inside the usable FOV; the
    // renderer still works but rays near the rim will be rejected.
    if (!lens.isMonotonic(thetaMaxRad)) {
        notes.push_back(std::format("{}: WARNING lens polynomial is non-monotonic inside {:.2f} deg", name,
                                    rad2deg(thetaMaxRad)));
        log::warn("LensRig: {} lens polynomial is non-monotonic inside the usable FOV", name);
    }
    return lens;
}

/// Convert the occlusion polygon of a record to stream pixels.
std::vector<Vec2d> buildOcclusion(const meta::DewarpParams& params, const StreamScaling& scaling) {
    std::vector<Vec2d> poly;
    // Both coordinate lists must agree; a mismatch is treated as no polygon.
    const std::size_t n = std::min(params.occlusionPtX.size(), params.occlusionPtY.size());
    if (n < 3 || params.occlusionPtX.size() != params.occlusionPtY.size()) {
        return poly;
    }
    poly.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2d sensorPx{static_cast<double>(params.occlusionPtX[i]), static_cast<double>(params.occlusionPtY[i])};
        // Skip garbage vertices instead of poisoning the polygon.
        if (!std::isfinite(sensorPx.x) || !std::isfinite(sensorPx.y)) {
            continue;
        }
        poly.push_back(scaling.apply(sensorPx));
    }
    if (poly.size() < 3) {
        poly.clear();
    }
    return poly;
}

}  // namespace

// -----------------------------------------------------------------------------
//  build
// -----------------------------------------------------------------------------
Result<LensRig> LensRig::build(const meta::CalibrationSet& calibration, const StreamScaling& scaling,
                               FocalSource focalSource, double digitalFocalLength,
                               const ExtrinsicConvention& convention, double lensFovDeg) {
    // The usable FOV must be a sensible fisheye range.
    if (!std::isfinite(lensFovDeg) || lensFovDeg <= 0.0 || lensFovDeg > 360.0) {
        return Error{ErrorCode::InvalidArgument, std::format("LensRig: bad lens FOV {} deg", lensFovDeg)};
    }
    // Stream size comes from the scaling's destination centre.
    const double w = 2.0 * scaling.dstCx;
    const double h = 2.0 * scaling.dstCy;
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0.0 || h <= 0.0) {
        return Error{ErrorCode::InvalidArgument, "LensRig: stream scaling has no valid destination size"};
    }

    LensRig rig;
    rig.streamW = static_cast<int>(std::lround(w));
    rig.streamH = static_cast<int>(std::lround(h));
    rig.scaling = scaling;
    rig.convention = convention;
    rig.focalSource = focalSource;
    rig.lensFovDeg = lensFovDeg;
    const double thetaMaxRad = deg2rad(0.5 * lensFovDeg);

    // Lens 0 = slave (-Y), lens 1 = master (+Y).
    const meta::DewarpParams* records[kLensCount] = {&calibration.slave, &calibration.master};
    const char* names[kLensCount] = {"slave", "master"};
    for (int i = 0; i < kLensCount; ++i) {
        OSV_TRY_ASSIGN(KannalaBrandt5 lens, buildLens(*records[i], names[i], scaling, focalSource, digitalFocalLength,
                                                     thetaMaxRad, rig.notes));
        rig.lens[static_cast<std::size_t>(i)] = lens;
        rig.bodyToLens[static_cast<std::size_t>(i)] = bodyToLensMatrix(records[i]->camExtriQ, convention);
        rig.occlusionPolyStream[static_cast<std::size_t>(i)] = buildOcclusion(*records[i], scaling);
        rig.notes.push_back(std::format("{}: occlusion polygon with {} vertices", names[i],
                                        rig.occlusionPolyStream[static_cast<std::size_t>(i)].size()));
    }

    // Record where the records came from and how they were read.
    rig.notes.push_back(std::format("calibration slots: slave={} master={}", calibration.sourceSlave,
                                    calibration.sourceMaster));
    rig.notes.push_back(std::format("extrinsics read as {} / {}", quatOrderName(convention.order),
                                    rotationSenseName(convention.sense)));
    if (!scaling.verified) {
        rig.notes.push_back(std::format("stream scale {:.6f} is UNVERIFIED for this mode", scaling.scale));
    }
    return rig;
}

// -----------------------------------------------------------------------------
//  Axis helpers (lens frame axes expressed in the body frame)
// -----------------------------------------------------------------------------
Vec3d LensRig::opticalAxisBody(int i) const noexcept {
    if (!validIndex(i)) {
        return Vec3d{};
    }
    // d_lens = R d_body  =>  d_body = R^T d_lens; the axis is lens +z.
    return bodyToLens[static_cast<std::size_t>(i)].transposed() * Vec3d{0.0, 0.0, 1.0};
}

Vec3d LensRig::imageRightBody(int i) const noexcept {
    if (!validIndex(i)) {
        return Vec3d{};
    }
    return bodyToLens[static_cast<std::size_t>(i)].transposed() * Vec3d{1.0, 0.0, 0.0};
}

Vec3d LensRig::imageDownBody(int i) const noexcept {
    if (!validIndex(i)) {
        return Vec3d{};
    }
    return bodyToLens[static_cast<std::size_t>(i)].transposed() * Vec3d{0.0, 1.0, 0.0};
}

bool LensRig::projectBody(int i, const Vec3d& dBody, Vec2d& px, double& theta) const noexcept {
    if (!validIndex(i) || !dBody.isFinite()) {
        return false;
    }
    // Rotate into the lens frame and hand over to the intrinsic model.
    const Vec3d dLens = bodyToLens[static_cast<std::size_t>(i)] * dBody;
    return lens[static_cast<std::size_t>(i)].project(dLens, px, theta);
}

}  // namespace osv::geom
