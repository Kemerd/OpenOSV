// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Preset lookup.

#include "osv/geom/Presets.h"

#include <cctype>
#include <string>

namespace osv::geom {

namespace {

/// Lower-case a name and fold '-', '_' and ' ' to '-' so "Crystal Ball",
/// "crystal_ball" and "crystal-ball" all compare equal.
std::string canonical(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '_' || c == ' ' || c == '-') {
            out.push_back('-');
        } else {
            out.push_back(static_cast<char>(std::tolower(uc)));
        }
    }
    return out;
}

}  // namespace

const Preset* findPreset(std::string_view id) noexcept {
    if (id.empty()) {
        return nullptr;
    }
    const std::string wanted = canonical(id);
    // Match either the CLI id or the display name.
    for (const Preset& preset : kPresets) {
        if (!preset.id || !preset.name) {
            continue;
        }
        if (canonical(preset.id) == wanted || canonical(preset.name) == wanted) {
            return &preset;
        }
    }
    return nullptr;
}

void applyPreset(const Preset& preset, VirtualCamera& camera) noexcept {
    // Only the look-defining fields change; framing set by the user stays.
    camera.projection = preset.projection;
    camera.hfovDeg = preset.hfovDeg;
    camera.pitchDeg = preset.pitchDeg;
    // The offset is meaningful for the eye-offset projection only, but it is
    // copied unconditionally so the camera mirrors the table exactly.
    camera.eyeOffset = preset.eyeOffset;
}

}  // namespace osv::geom
