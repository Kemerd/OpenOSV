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
    double eyeOffset;        ///< Eye offset d in [0, 1] (Projection::EyeOffset).
};

/// The preset table.  Order is the order shown to users.  Numbers are
/// starting points, not conventions - edit freely.
///
/// Every look uses the eye-offset projection so a single "Distortion" control
/// moves between them: d = 1 is exactly the stereographic Crystal Ball /
/// Asteroid rendering, d = 0 is the rectilinear Dewarping, and the wide looks
/// sit in between with a little barrel curvature that keeps corners from
/// stretching.  Users are expected to edit these numbers.
inline constexpr std::array<Preset, 5> kPresets = {{
    {"crystal-ball", "Crystal Ball", Projection::EyeOffset, 240.0, 0.0, 1.0},
    {"asteroid", "Asteroid", Projection::EyeOffset, 300.0, -90.0, 1.0},
    {"wide", "Wide", Projection::EyeOffset, 120.0, 0.0, 0.15},
    {"ultra-wide", "Ultra Wide", Projection::EyeOffset, 150.0, 0.0, 0.4},
    {"dewarping", "Dewarping", Projection::EyeOffset, 95.0, 0.0, 0.0},
}};

/// One of DJI's reframe presets, in DJI's own camera terms (see
/// DjiSphereCamera in VirtualCamera.h): a vertical pinhole field of view and
/// an eye distance ("Correction Angle").
///
/// DJI stores the field of view PER OUTPUT SHAPE, because a vertical field of
/// view means something different on a portrait frame.  The numbers match
/// the presets of DJI's Premiere reframe plug-in ({pan, tilt, roll, fov,
/// eye distance} per preset and output shape); the landscape column equals
/// DJI Studio's own preset list (fov 138 / 78 / 60 / 80 with distortion
/// 1.0 / 0.5 / 0.6 / 0.2).  docs/research/DJI_CAMERA.md describes the model.
struct DjiPreset {
    const char* id;            ///< Stable id, identical to the classic table's ("crystal-ball").
    const char* name;          ///< Display name ("Crystal Ball").
    double vfovLandscapeDeg;   ///< FOV for landscape and square frames (16:9, 1:1, 2.35:1).
    double vfovPortrait916Deg; ///< FOV for a 9:16 frame.
    double vfovPortrait34Deg;  ///< FOV for a 3:4 frame.
    double eyeDistance;        ///< Correction Angle (sphere radii).
    double pitchDeg;           ///< Tilt the preset sets (-90: look straight down).
};

/// DJI's five presets, in the order DJI lists them.  Crystal Ball is the one
/// look with the eye OUTSIDE the sphere (1.8 radii).
inline constexpr std::array<DjiPreset, 5> kDjiPresets = {{
    {"crystal-ball", "Crystal Ball", 75.0, 110.0, 87.0, 1.8, 0.0},
    {"asteroid", "Asteroid", 138.0, 147.0, 147.0, 1.0, -90.0},
    {"wide", "Wide", 60.0, 90.0, 72.0, 0.6, 0.0},
    {"ultra-wide", "Ultra Wide", 78.0, 110.0, 95.0, 0.5, 0.0},
    {"dewarping", "Dewarping", 80.0, 112.0, 97.0, 0.2, 0.0},
}};

/// The field of view a DJI preset uses on a frame of shape `aspect`
/// (width / height).  DJI only knows three columns, so an arbitrary shape is
/// mapped to the nearest: anything at least as wide as it is tall uses the
/// landscape column (DJI files 1:1 and 2.35:1 there too), a portrait frame
/// uses the 9:16 or the 3:4 column, whichever shape it is closer to (split
/// at their geometric mean, 0.6495).  A non-finite or non-positive aspect is
/// treated as landscape, the shape nearly every sequence has.
[[nodiscard]] double djiPresetVfovDeg(const DjiPreset& preset, double aspect) noexcept;

/// Find a DJI preset by id or display name, with the same folding rules as
/// findPreset().  Returns nullptr when unknown.
[[nodiscard]] const DjiPreset* findDjiPreset(std::string_view id) noexcept;

/// Find a preset by id or display name (case-insensitive; '-', '_' and ' '
/// are treated alike).  Returns nullptr when unknown.
[[nodiscard]] const Preset* findPreset(std::string_view id) noexcept;

/// Apply a preset to a camera, keeping its size, yaw, roll and correction.
void applyPreset(const Preset& preset, VirtualCamera& camera) noexcept;

}  // namespace osv::geom
