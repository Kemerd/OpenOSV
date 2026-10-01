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
//
// Two controls exist in VEGAS Pro only, and neither is a stitch setting -
// neither reaches the PrefsBlob:
//
//   * Output Levels (kOutputLevels): the levels the finished pixels are
//     packed in, because VEGAS never level-converts a generator's output
//     and its "video levels" projects expect studio RGB;
//   * Playback Proxy (kPlaybackProxy): whether VEGAS's Draft and Preview
//     playback may stitch the camera's .LRF proxy instead of the .OSV.
//
// Under Resolve and every other host neither is defined at all, so their
// parameter list is exactly what it always was.
#pragma once

#include "OfxHost.h"
#include "OfxHostImage.h"

#include "SourceSettingsMapping.h"

namespace osv::ofx::source_params {

// ---------------------------------------------------------------------------
//  Parameter names (permanent: Resolve and VEGAS store them in their projects,
//  and the VEGAS extension sets them by name)
// ---------------------------------------------------------------------------
inline constexpr const char* kColourGroup = "colourGroup";
inline constexpr const char* kColorOutput = "colorOutput";
/// [WP-HDRTONE] "Transfer Function (HDR)": the PQ / HLG outputs' style.
inline constexpr const char* kHdrTone = "hdrTone";
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

/// VEGAS only: "Output Levels", the levels the generator packs its RGB in
/// (OutputLevels in OfxHostImage.h).  Defined right after Colour Output,
/// in the Colour group, and ONLY under a VEGAS host - which is why it is not
/// in kAllParams, the list every host shares.
inline constexpr const char* kOutputLevels = "outputLevels";
/// Output Levels items, 0-based in OutputLevels order (Full = 0, Studio = 1).
inline constexpr const char* kOutputLevelsItems = "Full range (0-255)|Studio RGB (16-235)";
/// Output Levels default, 0-based: Studio RGB, what VEGAS's 8-bit (and
/// 32-bit video levels) projects - its defaults - work in.
inline constexpr int kOutputLevelsDefault0 = static_cast<int>(OutputLevels::Studio);
static_assert(static_cast<int>(OutputLevels::Full) == 0 && static_cast<int>(OutputLevels::Studio) == 1,
              "Output Levels' items are numbered in OutputLevels order");

/// VEGAS only: "Playback Proxy", a checkbox.  On (the default), a frame
/// VEGAS plays at Draft or Preview quality is stitched from the .LRF proxy
/// the camera recorded beside the .OSV - a fraction of the decode and stitch
/// - while Good and Best, the qualities a file render uses, always stitch
/// the .OSV (OfxSource.cpp).  Defined right after Sphere Size, in the
/// Advanced group, and ONLY under a VEGAS host: like Output Levels it is not
/// a stitch setting and never reaches the PrefsBlob.
inline constexpr const char* kPlaybackProxy = "playbackProxy";
/// Playback Proxy's default: on.
inline constexpr bool kPlaybackProxyDefault = true;

/// Every parameter describe() defines under EVERY host, in definition order
/// (for the tests).  kOutputLevels and kPlaybackProxy, VEGAS only, are not
/// among them.
inline constexpr const char* kAllParams[] = {
    kColourGroup,   kColorOutput,   kHdrTone /* [WP-HDRTONE] */, kLook, kHdrPeak, kStabilization, kStitchGroup,
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

/// Define every control on `set` for a host of `profile`: the shared list,
/// plus, under VEGAS, Output Levels right after Colour Output and Playback
/// Proxy right after Sphere Size.
void describe(OfxParamSetHandle set, HostProfile profile) noexcept;

/// The controls at `time`, as the Source Settings effect's ControlValues
/// (1-BASED popup values, which is what prefsFromControls() expects).
[[nodiscard]] premiere::sourcesettings::ControlValues read(OfxParamSetHandle set, OfxTime time) noexcept;

/// The levels the generator packs its output in at `time`.  Full outside
/// VEGAS, always (no control exists there, and Resolve's output never
/// changes); under VEGAS the Output Levels control, with its default
/// (Studio) when the host cannot answer or answers with an unknown item.
[[nodiscard]] OutputLevels outputLevelsAt(OfxParamSetHandle set, OfxTime time, HostProfile profile) noexcept;

/// Whether VEGAS playback frames may come from the .LRF proxy at `time`.
/// False outside VEGAS, always (no control exists there); under VEGAS the
/// Playback Proxy control, with its default (on) when the host cannot answer.
[[nodiscard]] bool playbackProxyAt(OfxParamSetHandle set, OfxTime time, HostProfile profile) noexcept;

/// True when `name` is one of these controls (the VEGAS-only ones included).
[[nodiscard]] bool owns(const char* name) noexcept;

}  // namespace osv::ofx::source_params
