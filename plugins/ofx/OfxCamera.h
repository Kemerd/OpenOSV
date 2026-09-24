// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxCamera.h - the virtual camera controls, shared by both OpenFX effects.
//
// ===========================================================================
//  One camera, three hosts
// ===========================================================================
// Open 360 Reframe in Premiere Pro (plugins/reframe) defines the camera: the
// Output Resolution popup, the six DJI Studio presets, the DJI and Classic
// lenses, Pan / Tilt / Roll, the Source orientation, Keyframe Easing and
// Smooth Keyframes.  Everything that DECIDES something - ranges, defaults,
// popup items, the preset table, the lens conversions, the easing curves,
// the camera maths itself - lives in plugins/reframe/ReframeParams.h,
// ReframeCpu.cpp and ReframeEasing.cpp, and this module compiles those very
// files.  What is here is only the OpenFX spelling of the same controls:
//
//   * describe()          - the parameters, built from the shared constants;
//   * read()              - the host's values at one time, resolved into the
//                           same `reframe::Settings` block the Premiere
//                           effect renders from (easing and smoothing
//                           included), so a frame framed in Premiere and in
//                           Resolve with the same numbers is the same frame;
//   * instanceChanged()   - the supervised behaviour of the Premiere effect's
//                           PF_Cmd_USER_CHANGED_PARAM: presets write the
//                           look, lens switches carry the look across, Zoom
//                           moves along DJI Studio's zoom path;
//   * applyVisibility()   - one lens's controls on screen at a time.
//
// ===========================================================================
//  Numbering
// ===========================================================================
// OpenFX numbers choice items from 0; the shared constants (and the
// sanitisers that read them) number popups from 1, like After Effects.
// Every conversion is therefore a visible "+ 1" or "- 1" at the one place a
// choice is read or written, and nowhere else.
//
// The parameter NAMES below are the permanent identity of each control:
// DaVinci Resolve stores them in project files.  Never rename one; append
// new controls instead.
#pragma once

#include "OfxHost.h"

#include "ReframeParams.h"

namespace osv::ofx::camera {

// ---------------------------------------------------------------------------
//  Parameter names (permanent)
// ---------------------------------------------------------------------------
inline constexpr const char* kOutputResolution = "outputResolution";
inline constexpr const char* kCameraGroup = "cameraGroup";
inline constexpr const char* kPreset = "preset";
inline constexpr const char* kLens = "lens";
inline constexpr const char* kPan = "pan";
inline constexpr const char* kTilt = "tilt";
inline constexpr const char* kRoll = "roll";
inline constexpr const char* kDjiFov = "djiFov";
inline constexpr const char* kCorrection = "correction";
inline constexpr const char* kZoom = "zoom";
inline constexpr const char* kFov = "fov";
inline constexpr const char* kDistortion = "distortion";
inline constexpr const char* kKeyframeEasing = "keyframeEasing";
inline constexpr const char* kSmoothKeyframes = "smoothKeyframes";
inline constexpr const char* kSourceGroup = "sourceGroup";
inline constexpr const char* kSourcePan = "sourcePan";
inline constexpr const char* kSourceTilt = "sourceTilt";
inline constexpr const char* kSourceRoll = "sourceRoll";
/// Hidden mirror of the Lens choice: the lens that was on screen before an
/// edit, which is how a real lens switch is told from a re-pick of the lens
/// already selected (the host reports only WHICH control changed, never its
/// old value).  The Premiere effect keeps the same mirror for the same
/// reason (ReframeParams.h, OSV_REFRAME_ID_LENS).
inline constexpr const char* kLensMirror = "lensMirror";

/// Every parameter describe() defines, in definition order - for the tests,
/// which walk the list the loaded module produced.
inline constexpr const char* kAllParams[] = {
    kOutputResolution, kCameraGroup, kPreset, kLens,          kPan,           kTilt,        kRoll,
    kDjiFov,           kCorrection,  kZoom,   kFov,           kDistortion,    kKeyframeEasing,
    kSmoothKeyframes,  kSourceGroup, kSourcePan, kSourceTilt, kSourceRoll,    kLensMirror,
};

// ---------------------------------------------------------------------------
//  The four entry points
// ---------------------------------------------------------------------------

/// Define every camera control on `set` (a descriptor's parameter set).
void describe(OfxParamSetHandle set) noexcept;

/// The pixel size of the project (kOfxImageEffectPropProjectSize divided by
/// the project's pixel aspect), invalid when the host does not say.  This is
/// the "sequence size" of the Premiere effect: what "Match Timeline" frames
/// for and the shape the DJI conversions are computed on.
[[nodiscard]] reframe::SizePx projectSize(OfxImageEffectHandle effect) noexcept;

/// Every camera control at `time`, resolved exactly as the Premiere effect's
/// CPU path resolves them (ReframeParams.h `Settings`): popups sanitised,
/// Keyframe Easing applied to Pan / Tilt / Roll and the selected lens's two
/// controls, and Smooth Keyframes averaging the six angles over t-1, t, t+1.
[[nodiscard]] reframe::Settings read(OfxParamSetHandle set, OfxTime time) noexcept;

/// Show the selected lens's controls and hide the other lens's (the Premiere
/// effect's controlVisible() table), and keep the hidden mirror hidden.
void applyVisibility(OfxParamSetHandle set, OfxTime time) noexcept;

/// kOfxActionInstanceChanged for a USER edit of `name` at `time`.  Returns
/// true when `name` is a camera control (handled or deliberately ignored),
/// false when it belongs to someone else.  `project` is projectSize().
bool instanceChanged(OfxParamSetHandle set, const char* name, OfxTime time, reframe::SizePx project) noexcept;

}  // namespace osv::ofx::camera
