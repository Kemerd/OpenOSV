// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Reframing presets.  These are editable DEFAULTS chosen to look like the
// familiar 360 looks, not format conventions: nothing in the file format
// depends on them and users are expected to tweak the numbers.
#pragma once

#include "osv/geom/VirtualCamera.h"

#include <array>
#include <string_view>

namespace osv::geom {

/// One named starting point for the virtual camera.
struct Preset {
    const char* id;          ///< Stable CLI identifier ("crystal-ball").
    const char* name;        ///< Display name ("Crystal Ball").
    Projection projection;   ///< Virtual camera projection.
    double hfovDeg;          ///< Horizontal FOV (deg).
    double pitchDeg;         ///< Initial pitch (deg).
};

/// The preset table.  Order is the order shown to users.  Numbers are
/// starting points, not conventions - edit freely.
inline constexpr std::array<Preset, 5> kPresets = {{
    {"crystal-ball", "Crystal Ball", Projection::Stereographic, 240.0, 0.0},
    {"asteroid", "Asteroid", Projection::Stereographic, 300.0, -90.0},
    {"wide", "Wide", Projection::Rectilinear, 120.0, 0.0},
    {"ultra-wide", "Ultra Wide", Projection::Rectilinear, 150.0, 0.0},
    {"dewarping", "Dewarping", Projection::Rectilinear, 95.0, 0.0},
}};

/// Find a preset by id or display name (case-insensitive; '-', '_' and ' '
/// are treated alike).  Returns nullptr when unknown.
[[nodiscard]] const Preset* findPreset(std::string_view id) noexcept;

/// Apply a preset to a camera, keeping its size, yaw, roll and correction.
void applyPreset(const Preset& preset, VirtualCamera& camera) noexcept;

}  // namespace osv::geom
