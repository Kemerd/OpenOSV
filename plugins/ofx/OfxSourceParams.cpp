// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxSourceParams.cpp - the OpenOSV Source generator's stitching and colour
// controls (OfxSourceParams.h).

#include "OfxSourceParams.h"

#include "PluginLog.h"

#include <string_view>

namespace osv::ofx::source_params {

using osv::premiere::PluginLog;
using osv::premiere::sourcesettings::ControlValues;

namespace {

/// A stitching setting is a property of the recording, not of a moment in
/// it: none of these controls keyframes (the Premiere effect marks every one
/// PF_ParamFlag_CANNOT_TIME_VARY for the same reason).
constexpr bool kStatic = false;

/// A slider in the Source Settings effect's own units.
[[nodiscard]] DoubleRange slider(double min, double max, double value, int digits, double increment) noexcept {
    DoubleRange r;
    r.min = min;
    r.max = max;
    r.displayMin = min;
    r.displayMax = max;
    r.value = value;
    r.digits = digits;
    r.increment = increment;
    return r;
}

}  // namespace

// ===========================================================================
//  describe
// ===========================================================================

void describe(OfxParamSetHandle set) noexcept {
    if (!set) {
        return;
    }

    // ---- Colour ---------------------------------------------------------------
    defineGroup(set, kColourGroup, {"Colour", nullptr, nullptr, true}, true);
    defineChoice(set, kColorOutput,
                 {"Colour Output",
                  "What the stitched picture is encoded as. Rec. 709 suits a default Resolve timeline; pick PQ or HLG "
                  "for HDR, or D-Log M to grade from the camera's log with your own LUT.",
                  kColourGroup, kStatic},
                 OSV_SS_COLOR_ITEMS, kColorOutputDefault0);
    defineChoice(set, kLook,
                 {"Look (Rec. 709 only)",
                  "DJI renders D-Log M the way DJI Studio does. OpenOSV standard is the neutral HLG-in-709 rendering.",
                  kColourGroup, kStatic},
                 OSV_SS_LOOK_ITEMS, OSV_SS_LOOK_DEFAULT - 1);
    defineChoice(set, kHdrPeak,
                 {"HDR Peak (PQ only)",
                  "The display peak PQ highlights roll off into. Mid-tones and faces stay put.", kColourGroup,
                  kStatic},
                 OSV_SS_HDR_PEAK_ITEMS, OSV_SS_HDR_PEAK_DEFAULT - 1);
    defineChoice(set, kStabilization,
                 {"Stabilisation",
                  "From the camera's own gyro. Smooth + Horizon Lock is DJI's RockSteady with Horizon Leveling.",
                  kColourGroup, kStatic},
                 OSV_SS_STAB_ITEMS, OSV_SS_STAB_DEFAULT - 1);

    // ---- Stitching (collapsed: the defaults are the recommended stitch) -----
    defineGroup(set, kStitchGroup, {"Stitching", nullptr, nullptr, true}, false);
    defineBool(set, kSeamSearch,
               {"Seam Search", "Finds the stitch distance that lines the two lenses up, per stretch of frames.",
                kStitchGroup, kStatic},
               OSV_SS_SEAM_SEARCH_DEFAULT != 0);
    defineBool(set, kGainMatch,
               {"Exposure Match", "Evens the two lenses' brightness across the seam.", kStitchGroup, kStatic},
               OSV_SS_GAIN_MATCH_DEFAULT != 0);
    defineChoice(set, kCalibration,
                 {"Calibration",
                  "Which lens calibration to stitch with. Auto follows what the camera recorded.", kStitchGroup,
                  kStatic},
                 OSV_SS_CALIB_ITEMS, OSV_SS_CALIB_DEFAULT - 1);
    defineBool(set, kFlareRemoval,
               {"Sun Ghost Removal", "Removes the lens ghosts a low sun leaves near the seam.", kStitchGroup, kStatic},
               OSV_SS_FLARE_REMOVAL_DEFAULT != 0);
    defineChoice(set, kSkySeamFix,
                 {"Sky Seam Fix",
                  "Rim only ends each lens where it stops being trustworthy. Rim and colour also evens colour "
                  "across the overlap.",
                  kStitchGroup, kStatic},
                 OSV_SS_PHOTO_SEAM_ITEMS, OSV_SS_PHOTO_SEAM_DEFAULT - 1);
    defineDouble(set, kSkySeamStrength,
                 {"Sky Seam Strength", "How much of the colour correction is applied, in percent.", kStitchGroup,
                  kStatic},
                 slider(OSV_SS_PHOTO_STRENGTH_MIN, OSV_SS_PHOTO_STRENGTH_MAX, OSV_SS_PHOTO_STRENGTH_DEFAULT, 0, 1.0));
    defineDouble(set, kSeamEdgeInset,
                 {"Seam Edge Inset", "How far inside each lens's edge the blend ends, in degrees.", kStitchGroup,
                  kStatic},
                 slider(OSV_SS_SEAM_INSET_MIN, OSV_SS_SEAM_INSET_MAX, OSV_SS_SEAM_INSET_DEFAULT, 1, 0.1));
    defineDouble(set, kSeamBlend,
                 {"Seam Blend", "The feather where the two lenses agree, in degrees.", kStitchGroup, kStatic},
                 slider(OSV_SS_SEAM_BLEND_MIN, OSV_SS_SEAM_BLEND_MAX, OSV_SS_SEAM_BLEND_DEFAULT, 2, 0.05));
    defineDouble(set, kParallaxBlend,
                 {"Parallax Blend", "The feather where they disagree, in degrees. 0 is a hard cut.", kStitchGroup,
                  kStatic},
                 slider(OSV_SS_PARALLAX_BLEND_MIN, OSV_SS_PARALLAX_BLEND_MAX, OSV_SS_PARALLAX_BLEND_DEFAULT, 2, 0.05));
    defineDouble(set, kSeamSmoothing,
                 {"Seam Smoothing", "Blends colour across this half width while detail still switches at the seam.",
                  kStitchGroup, kStatic},
                 slider(OSV_SS_SEAM_SMOOTHING_MIN, OSV_SS_SEAM_SMOOTHING_MAX, OSV_SS_SEAM_SMOOTHING_DEFAULT, 2, 0.05));
    defineDouble(set, kNearOffset,
                 {"Near Offset", "Nudges near content along the seam, in degrees.", kStitchGroup, kStatic},
                 slider(OSV_SS_SEAM_OFFSET_MIN, OSV_SS_SEAM_OFFSET_MAX, OSV_SS_SEAM_OFFSET_DEFAULT, 2, 0.01));
    defineDouble(set, kFarOffset,
                 {"Far Offset", "Nudges far content along the seam, in degrees.", kStitchGroup, kStatic},
                 slider(OSV_SS_SEAM_OFFSET_MIN, OSV_SS_SEAM_OFFSET_MAX, OSV_SS_SEAM_OFFSET_DEFAULT, 2, 0.01));
    defineChoice(set, kLensShading,
                 {"Lens Shading", "Takes out the dark ring a lens facing the sun leaves on the sky seam.", kStitchGroup,
                  kStatic},
                 OSV_SS_LENS_SHADING_ITEMS, OSV_SS_LENS_SHADING_DEFAULT - 1);
    defineDouble(set, kShadingStrength,
                 {"Shading Strength", "How much of the measured shading correction is applied, in percent.",
                  kStitchGroup, kStatic},
                 slider(OSV_SS_SHADING_STRENGTH_MIN, OSV_SS_SHADING_STRENGTH_MAX, OSV_SS_SHADING_STRENGTH_DEFAULT, 0,
                        1.0));
    defineChoice(set, kParallaxGrid,
                 {"Parallax Grid",
                  "Steady holds the seam still for a rigid mount. Follows scene re-measures it for handheld shots.",
                  kStitchGroup, kStatic},
                 OSV_SS_PARALLAX_GRID_ITEMS, OSV_SS_PARALLAX_GRID_DEFAULT - 1);
    defineChoice(set, kLensAlignment,
                 {"Lens Alignment", "Fits the small rotation between the two lenses once per clip.", kStitchGroup,
                  kStatic},
                 OSV_SS_LENS_ALIGN_ITEMS, OSV_SS_LENS_ALIGN_DEFAULT - 1);

    // ---- Advanced (collapsed) ---------------------------------------------------
    defineGroup(set, kAdvancedGroup, {"Advanced", nullptr, nullptr, true}, false);
    defineChoice(set, kDlogmCurve,
                 {"D-Log M Curve", "The D-Log M decoding curve. Osmo 360 is fitted to this camera.", kAdvancedGroup,
                  kStatic},
                 OSV_SS_FIT_ITEMS, OSV_SS_FIT_DEFAULT - 1);
    DoubleRange exposure;
    exposure.min = OSV_SS_EXPOSURE_VALID_MIN;
    exposure.max = OSV_SS_EXPOSURE_VALID_MAX;
    exposure.displayMin = OSV_SS_EXPOSURE_SLIDER_MIN;
    exposure.displayMax = OSV_SS_EXPOSURE_SLIDER_MAX;
    exposure.value = OSV_SS_EXPOSURE_DEFAULT;
    exposure.digits = 2;
    exposure.increment = 0.05;
    defineDouble(set, kExposure,
                 {"Exposure", "Shifts exposure before the output transform, in stops.", kAdvancedGroup, kStatic},
                 exposure);
    defineChoice(set, kRenderDevice,
                 {"Render Device", "Where the stitch runs. Auto picks CUDA, then OpenCL, then the CPU.", kAdvancedGroup,
                  kStatic},
                 OSV_SS_DEVICE_ITEMS, OSV_SS_DEVICE_DEFAULT - 1);
    // Premiere's "Output Size": the stitched sphere's size.  In Reframed
    // view mode it is the sphere the camera samples; a 360 equirect output
    // is always rendered straight at the timeline's size instead.
    defineChoice(set, kSphereSize,
                 {"Sphere Size",
                  "The stitched sphere the camera looks into. Native keeps every pixel the camera recorded; smaller "
                  "is faster.",
                  kAdvancedGroup, kStatic},
                 OSV_SS_SIZE_ITEMS, OSV_SS_SIZE_DEFAULT - 1);
}

// ===========================================================================
//  read
// ===========================================================================

ControlValues read(OfxParamSetHandle set, OfxTime time) noexcept {
    ControlValues c;
    // Every popup is read 0-based and stored 1-based, the numbering
    // prefsFromControls() validates against; a missing control keeps the
    // ControlValues default (the Source Settings effect's), except Colour
    // Output, whose OpenFX default differs on purpose (see the header).
    const auto popup = [&](const char* name, int default1) noexcept { return intAt(set, name, time, default1 - 1) + 1; };
    const auto boolean = [&](const char* name, bool fallback) noexcept {
        return intAt(set, name, time, fallback ? 1 : 0) != 0;
    };

    c.colorOutput = popup(kColorOutput, kColorOutputDefault0 + 1);
    c.rec709Look = popup(kLook, c.rec709Look);
    c.hdrPeak = popup(kHdrPeak, c.hdrPeak);
    c.stabilization = popup(kStabilization, c.stabilization);

    c.seamSearch = boolean(kSeamSearch, c.seamSearch);
    c.gainMatch = boolean(kGainMatch, c.gainMatch);
    c.calibration = popup(kCalibration, c.calibration);
    c.flareRemoval = boolean(kFlareRemoval, c.flareRemoval);
    c.photoSeam = popup(kSkySeamFix, c.photoSeam);
    c.photoStrengthPercent = doubleAt(set, kSkySeamStrength, time, c.photoStrengthPercent);
    c.seamInsetDeg = doubleAt(set, kSeamEdgeInset, time, c.seamInsetDeg);
    c.seamBlendDeg = doubleAt(set, kSeamBlend, time, c.seamBlendDeg);
    c.parallaxBlendDeg = doubleAt(set, kParallaxBlend, time, c.parallaxBlendDeg);
    c.seamSmoothingDeg = doubleAt(set, kSeamSmoothing, time, c.seamSmoothingDeg);
    c.nearOffsetDeg = doubleAt(set, kNearOffset, time, c.nearOffsetDeg);
    c.farOffsetDeg = doubleAt(set, kFarOffset, time, c.farOffsetDeg);
    c.lensShading = popup(kLensShading, c.lensShading);
    c.shadingStrengthPercent = doubleAt(set, kShadingStrength, time, c.shadingStrengthPercent);
    c.parallaxGrid = popup(kParallaxGrid, c.parallaxGrid);
    c.lensAlign = popup(kLensAlignment, c.lensAlign);

    c.dlogmFit = popup(kDlogmCurve, c.dlogmFit);
    c.exposureStops = doubleAt(set, kExposure, time, c.exposureStops);
    c.renderDevice = popup(kRenderDevice, c.renderDevice);
    c.outputSize = popup(kSphereSize, c.outputSize);
    // "Program Monitor Colour" steers Premiere's direct GPU path, which has
    // no OpenFX counterpart; it keeps its default.
    return c;
}

bool owns(const char* name) noexcept {
    if (!name) {
        return false;
    }
    const std::string_view n(name);
    for (const char* p : kAllParams) {
        if (n == p) {
            return true;
        }
    }
    return false;
}

}  // namespace osv::ofx::source_params
