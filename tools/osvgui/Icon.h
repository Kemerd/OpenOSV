// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Icon.h - OpenOSV Studio's icon, drawn in code: a rounded square in a
// blue-to-indigo gradient with a white globe (a sphere, its equator and a
// meridian - 360 degrees at a glance).
//
// No image file is committed: the build renders the Windows .ico from this
// (IconGen.cpp) and the app renders its window icon and header logo from
// it at run time, so every size is drawn for its own pixel grid.
#pragma once

#include <cstdint>
#include <vector>

namespace osvgui {

/// The icon at `size` x `size` pixels as RGBA8 with straight (not
/// premultiplied) alpha, rows top to bottom.  `size` is clamped to 8..1024.
[[nodiscard]] std::vector<std::uint8_t> renderAppIcon(int size);

}  // namespace osvgui
