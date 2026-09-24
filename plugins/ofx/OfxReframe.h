// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxReframe.h - "OpenOSV 360 Reframe" as an OpenFX filter.
//
// Point a virtual camera into ANY equirectangular clip - the OpenOSV Source
// generator's 360 output, a DJI Studio export, another camera's sphere - and
// keyframe it.  The controls, the lenses, the presets and the pixels are the
// Premiere effect's (OfxCamera.h); this file is only the OpenFX side of it:
// the descriptor, the clips, and a render that runs on the CPU or, when the
// host hands over CUDA images (DaVinci Resolve on NVIDIA), on the GPU.
#pragma once

#include "OfxHost.h"

namespace osv::ofx::reframe_filter {

/// The plug-in identifier.  DaVinci Resolve stores it in every project that
/// uses the effect: NEVER change it.
inline constexpr const char* kPluginId = "org.openosv.Open360Reframe";

/// Major / minor version reported to the host.  A host treats a new MAJOR
/// version as a different plug-in, so it only moves if a project saved with
/// the old one could no longer load.
inline constexpr unsigned kVersionMajor = 1;
inline constexpr unsigned kVersionMinor = 0;

/// The OfxPlugin::mainEntry of the filter.  Never throws.
OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                    OfxPropertySetHandle outArgs) noexcept;

}  // namespace osv::ofx::reframe_filter
