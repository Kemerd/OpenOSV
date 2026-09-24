// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxSourceParams.h - the stitching and colour controls of the OpenOSV Source
// generator: Premiere's "OpenOSV Source Settings" effect, as OpenFX
// parameters.
//
// Premiere keeps these settings on the master clip, in the importer's
// 128-byte PrefsBlob.  An OpenFX generator has no master clip, so the same
// controls sit on the generator itself and are turned into the same blob on
// every render:
//
//     OpenFX parameters --read()--> sourcesettings::ControlValues
//                       --sourcesettings::prefsFromControls()--> PrefsBlob
//
// prefsFromControls() is the Source Settings effect's own translation unit
// (plugins/sourcesettings/SourceSettingsMapping.cpp), compiled into
// OpenOSV.ofx unchanged, so an .OSV stitched in Resolve and in Premiere with
// the same settings goes through the same blob and the same importer code.
//
// Item lists, ranges and defaults come from SourceSettingsParams.h.  The one
// default that differs is Colour Output (see kColorOutputDefault0).
#pragma once

#include "OfxHost.h"

#include "SourceSettingsMapping.h"

namespace osv::ofx::source_params {

// ---------------------------------------------------------------------------
//  Parameter names (permanent: Resolve stores them in its projects)
// ---------------------------------------------------------------------------
inline constexpr const char* kColourGroup = "colourGroup";
inline constexpr const char* kColorOutput = "colorOutput";
inline constexpr const char* kLook = "look";
inline constexpr const char* kHdrPeak = "hdrPeak";
inline constexpr const char* kStabilization = "stabilization";
inline constexpr const char* kStitchGroup = "stitchGroup";
inline constexpr const char* kSeamSearch = "seamSearch";
inline constexpr const char* kGainMatch = "gainMatch";
inline constexpr const char* kCalibration = "calibration";
inline constexpr const char* kFlareRemoval = "flareRemoval";
inline constexpr const char* kSkySeamFix = "skySeamFix";
inline constexpr const char* kSkySeamStrength = "skySeamStrength";
inline constexpr const char* kSeamEdgeInset = "seamEdgeInset";
inline constexpr const char* kSeamBlend = "seamBlend";
inline constexpr const char* kParallaxBlend = "parallaxBlend";
inline constexpr const char* kSeamSmoothing = "seamSmoothing";
inline constexpr const char* kNearOffset = "nearOffset";
inline constexpr const char* kFarOffset = "farOffset";
inline constexpr const char* kLensShading = "lensShading";
inline constexpr const char* kShadingStrength = "shadingStrength";
inline constexpr const char* kParallaxGrid = "parallaxGrid";
inline constexpr const char* kLensAlignment = "lensAlignment";
inline constexpr const char* kAdvancedGroup = "advancedGroup";
inline constexpr const char* kDlogmCurve = "dlogmCurve";
inline constexpr const char* kExposure = "exposure";
inline constexpr const char* kRenderDevice = "renderDevice";
inline constexpr const char* kSphereSize = "sphereSize";

/// Every parameter describe() defines, in definition order (for the tests).
inline constexpr const char* kAllParams[] = {
    kColourGroup,   kColorOutput,   kLook,         kHdrPeak,         kStabilization, kStitchGroup,
    kSeamSearch,    kGainMatch,     kCalibration,  kFlareRemoval,    kSkySeamFix,    kSkySeamStrength,
    kSeamEdgeInset, kSeamBlend,     kParallaxBlend, kSeamSmoothing,  kNearOffset,    kFarOffset,
    kLensShading,   kShadingStrength, kParallaxGrid, kLensAlignment, kAdvancedGroup, kDlogmCurve,
    kExposure,      kRenderDevice,  kSphereSize,
};

/// Colour Output's default, 0-based: Rec. 709.
///
/// Premiere defaults to BT.2100 PQ because it TAGS the importer's frames and
/// converts them into the sequence's space.  An OpenFX generator cannot tag
/// anything - its pixels are taken to be in the timeline's space - and a new
/// DaVinci Resolve project's timeline is Rec. 709 Gamma 2.4.  Rec. 709 (with
/// DJI's look) is therefore the output that looks right the moment the
/// generator lands on a default timeline; PQ, HLG and the D-Log M passthrough
/// stay one click away for HDR and colour-managed projects.
inline constexpr int kColorOutputDefault0 = 2;
static_assert(kColorOutputDefault0 >= 0 && kColorOutputDefault0 < OSV_SS_COLOR_COUNT,
              "the Colour Output default must be an item of the list");

/// Define every control on `set`.
void describe(OfxParamSetHandle set) noexcept;

/// The controls at `time`, as the Source Settings effect's ControlValues
/// (1-BASED popup values, which is what prefsFromControls() expects).
[[nodiscard]] premiere::sourcesettings::ControlValues read(OfxParamSetHandle set, OfxTime time) noexcept;

/// True when `name` is one of these controls.
[[nodiscard]] bool owns(const char* name) noexcept;

}  // namespace osv::ofx::source_params
