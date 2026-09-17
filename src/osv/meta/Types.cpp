// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Out-of-line helpers for the typed metadata structs declared in Types.h:
// enum names, calibration record predicates and the 24-slot addressing of
// PanoDewarpParams.

#include "osv/meta/Types.h"

#include <cmath>

namespace osv::meta {

// -----------------------------------------------------------------------------
//  Enum names
// -----------------------------------------------------------------------------

const char* colorModeName(ColorMode mode) noexcept {
    switch (mode) {
    case ColorMode::Normal: return "Normal";
    case ColorMode::DCinelike: return "DCinelike";
    case ColorMode::DLog: return "DLog";
    case ColorMode::HLG: return "HLG";
    case ColorMode::Vivid: return "Vivid";
    case ColorMode::DLogM: return "DLogM";
    case ColorMode::DLog2: return "DLog2";
    case ColorMode::Unknown: return "Unknown";
    }
    // Enum values the camera may write that we have not catalogued yet.
    return "Unknown";
}

const char* extriLensModeName(ExtriLensMode mode) noexcept {
    switch (mode) {
    case ExtriLensMode::Native: return "Native";
    case ExtriLensMode::LensGuards: return "LensGuards";
    case ExtriLensMode::Underwater: return "Underwater";
    }
    return "Unknown";
}

const char* eisStatusName(EisStatus status) noexcept {
    switch (status) {
    case EisStatus::Off: return "Off";
    case EisStatus::RockSteady: return "RockSteady";
    case EisStatus::HorizonSteady: return "HorizonSteady";
    case EisStatus::Hyper: return "Hyper";
    case EisStatus::Tradeoff: return "Tradeoff";
    case EisStatus::HorizonBalancing: return "HorizonBalancing";
    case EisStatus::DeepSpace: return "DeepSpace";
    case EisStatus::OffWithCrop: return "OffWithCrop";
    case EisStatus::HorizonCorrection: return "HorizonCorrection";
    case EisStatus::RsAuto: return "RsAuto";
    }
    return "Unknown";
}

// -----------------------------------------------------------------------------
//  DewarpParams predicates
// -----------------------------------------------------------------------------

bool DewarpParams::hasCore() const noexcept {
    // Intrinsics must be finite and strictly positive: a zero focal length
    // or principal point means the slot was never calibrated.
    const float core[4] = {fx, fy, cx, cy};
    for (const float v : core) {
        if (!std::isfinite(v) || v <= 0.0f) {
            return false;
        }
    }
    // The calibration frame size is needed to scale to the stream size.
    if (width == 0 || height == 0) {
        return false;
    }
    // Radial terms must at least be finite (they may legitimately be zero).
    for (const float v : k) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    // The extrinsic quaternion places the lens in the body frame.  Accept
    // either the dedicated message or the 4-float `q` copy, but it must be a
    // usable (non-zero, finite) rotation.
    float qw = 0.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
    if (camExtriQ.present) {
        qw = camExtriQ.w;
        qx = camExtriQ.x;
        qy = camExtriQ.y;
        qz = camExtriQ.z;
    } else if (q.size() == 4) {
        qw = q[0];
        qx = q[1];
        qy = q[2];
        qz = q[3];
    } else {
        return false;
    }
    const float norm2 = qw * qw + qx * qx + qy * qy + qz * qz;
    if (!std::isfinite(norm2) || norm2 < 1e-6f) {
        return false;
    }
    return true;
}

bool DewarpParams::isEmpty() const noexcept {
    // Scalars first: any non-zero value means the slot carries data.
    const float scalars[] = {fx,   fy,        cx,          cy,          xi,          yaw,        pitch,
                             roll, lensModel, temperature, tempCompenK, gimbalYawH1, gimbalYawH2};
    for (const float v : scalars) {
        if (v != 0.0f) {
            return false;
        }
    }
    if (width != 0 || height != 0) {
        return false;
    }
    for (const float v : k) {
        if (v != 0.0f) {
            return false;
        }
    }
    // Vectors: an empty vector and a vector of zeros both count as empty.
    const std::vector<float>* vectors[] = {&p, &q, &occlusionPtX, &occlusionPtY, &tangentCoeff, &tempCompenKOrder};
    for (const std::vector<float>* vec : vectors) {
        for (const float v : *vec) {
            if (v != 0.0f) {
                return false;
            }
        }
    }
    // Quaternions: only their numeric content matters (a present but all-zero
    // quaternion is still "empty" - the camera writes zero records for
    // unused slots).
    const Quaternion* quats[] = {&camImuExtriQ, &camExtriQ};
    for (const Quaternion* qq : quats) {
        if (qq->present && (qq->w != 0.0f || qq->x != 0.0f || qq->y != 0.0f || qq->z != 0.0f)) {
            return false;
        }
    }
    if (camImuCaliEnable || tempCompenEnable) {
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  PanoDewarpParams
// -----------------------------------------------------------------------------

const char* PanoDewarpParams::fieldName(std::uint32_t field) noexcept {
    // Names follow the proto field names in proto/dvtm_osmo360.proto so the
    // JSON output and the .proto file agree.
    static constexpr const char* kNames[SlotCount] = {
        "unknown",
        "native_refine_slave",      // 1
        "native_refine_master",     // 2
        "native_refine_far_slave",  // 3
        "native_refine_far_master", // 4
        "lens_guards_slave",        // 5
        "lens_guards_master",       // 6
        "water_above_slave",        // 7
        "water_above_master",       // 8
        "water_under_slave",        // 9
        "water_under_master",       // 10
        "native_slave",             // 11
        "native_master",            // 12
        "far_07_slave",             // 13
        "far_07_master",            // 14
        "far_09_slave",             // 15
        "far_09_master",            // 16
        "far_11_slave",             // 17
        "far_11_master",            // 18
        "far_12_5_slave",           // 19
        "far_12_5_master",          // 20
        "far_14_slave",             // 21
        "far_14_master",            // 22
        "far_16_slave",             // 23
        "far_16_master",            // 24
    };
    if (field == 0 || field >= SlotCount) {
        return "unknown";
    }
    return kNames[field];
}

const DewarpParams* PanoDewarpParams::get(std::uint32_t field) const noexcept {
    if (field == 0 || field >= SlotCount) {
        return nullptr;
    }
    const std::optional<DewarpParams>& slot = byField[field];
    if (!slot.has_value()) {
        return nullptr;
    }
    // An all-zero record is what the camera writes for an unused slot; treat
    // it exactly like an absent one so callers never project through zeros.
    if (slot->isEmpty()) {
        return nullptr;
    }
    return &slot.value();
}

}  // namespace osv::meta
