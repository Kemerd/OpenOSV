// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxHost.h - the one place the OpenFX plug-ins talk to the host's suites.
//
// ===========================================================================
//  Why a hand-written layer and not the OFX C++ "Support" library
// ===========================================================================
// The Support library wraps every action in C++ classes that throw, hides
// the raw property sets (so the CUDA render properties of OFX 1.5 cannot be
// read through it), and brings ~10k lines of code we did not write between
// DaVinci Resolve and our render.  The C API needs only a handful of calls,
// and every one of them can fail in a host we cannot test against.  So each
// call goes through one of the small helpers below, each of which:
//
//   * checks every handle and pointer before it is used;
//   * turns a failed host call into a documented fallback value;
//   * never throws (the dispatcher in OfxMain.cpp still catches everything,
//     because an exception unwinding into the host's C stack would take the
//     host down with it).
//
// Nothing here knows about 360 video; OfxCamera.h / OfxReframe.cpp /
// OfxSource.cpp build the two effects on top of it.
#pragma once

#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace osv::ofx {

// ===========================================================================
//  The host and its suites
// ===========================================================================

/// Every suite the plug-ins use, fetched once per module load.
///
/// The property, parameter and image effect suites are REQUIRED: without any
/// of them no action can be answered, so kOfxActionLoad fails and the host
/// drops the plug-in cleanly.  The message suite is optional - it only
/// carries the "choose an .OSV file" style hints to the user.
struct Suites {
    OfxHost* host = nullptr;                          ///< From OfxSetHost; owned by the host.
    const OfxPropertySuiteV1* prop = nullptr;         ///< Required.
    const OfxParameterSuiteV1* param = nullptr;       ///< Required.
    const OfxImageEffectSuiteV1* effect = nullptr;    ///< Required.
    const OfxMessageSuiteV1* message = nullptr;       ///< Optional (user-facing messages).

    /// True when the three required suites are present.
    [[nodiscard]] bool ready() const noexcept { return host && prop && param && effect; }
};

/// Remember the host handed to OfxPlugin::setHost.  Both plug-ins in the
/// module receive the same pointer; a second call simply overwrites it.
void setHost(OfxHost* host) noexcept;

/// Fetch the suites from the remembered host.  Returns false (and leaves the
/// required suites null) when the host is unset or lacks a required suite.
/// Idempotent: a second call re-fetches, which is harmless.
[[nodiscard]] bool fetchSuites() noexcept;

/// Forget every suite pointer (called on the last kOfxActionUnload).
void clearSuites() noexcept;

/// The suites fetched by fetchSuites().  Every pointer may be null before
/// kOfxActionLoad; callers go through the helpers below, which check.
[[nodiscard]] const Suites& suites() noexcept;

/// Name of the host application (kOfxPropName on the host descriptor, e.g.
/// "DaVinciResolveLite"), or an empty string when it cannot be read.
[[nodiscard]] std::string hostName() noexcept;

// ===========================================================================
//  Property sets
// ===========================================================================
//
// Every getter returns `fallback` when the set is null, the property is
// missing, the index is out of range or the host call fails.  Every setter
// returns false on the same conditions and changes nothing.

[[nodiscard]] std::string getString(OfxPropertySetHandle set, const char* name, int index = 0,
                                    std::string_view fallback = {}) noexcept;
[[nodiscard]] int getInt(OfxPropertySetHandle set, const char* name, int index = 0, int fallback = 0) noexcept;
[[nodiscard]] double getDouble(OfxPropertySetHandle set, const char* name, int index = 0,
                               double fallback = 0.0) noexcept;
[[nodiscard]] void* getPointer(OfxPropertySetHandle set, const char* name, int index = 0,
                               void* fallback = nullptr) noexcept;

/// Read `count` doubles / ints in one call.  False (and `out` untouched) when
/// any of them cannot be read.
[[nodiscard]] bool getDoubles(OfxPropertySetHandle set, const char* name, double* out, int count) noexcept;
[[nodiscard]] bool getInts(OfxPropertySetHandle set, const char* name, int* out, int count) noexcept;

/// Number of values a property holds, or -1 when it does not exist.
[[nodiscard]] int dimension(OfxPropertySetHandle set, const char* name) noexcept;

bool setString(OfxPropertySetHandle set, const char* name, const char* value, int index = 0) noexcept;
bool setInt(OfxPropertySetHandle set, const char* name, int value, int index = 0) noexcept;
bool setDouble(OfxPropertySetHandle set, const char* name, double value, int index = 0) noexcept;
bool setPointer(OfxPropertySetHandle set, const char* name, void* value, int index = 0) noexcept;
bool setDoubles(OfxPropertySetHandle set, const char* name, const double* values, int count) noexcept;
bool setInts(OfxPropertySetHandle set, const char* name, const int* values, int count) noexcept;

/// The property set of an effect handle (descriptor or instance), or null.
[[nodiscard]] OfxPropertySetHandle effectProps(OfxImageEffectHandle effect) noexcept;

/// The parameter set of an effect handle, or null.
[[nodiscard]] OfxParamSetHandle effectParams(OfxImageEffectHandle effect) noexcept;

// ===========================================================================
//  Parameter description (kOfxActionDescribe / DescribeInContext)
// ===========================================================================

/// Split an After-Effects style popup string ("A|B|C") into its items.  The
/// popup strings live in ReframeParams.h / SourceSettingsParams.h, shared with
/// the Premiere effects, so the two hosts can never show different lists.
[[nodiscard]] std::vector<std::string> splitItems(std::string_view pipeSeparated);

/// Common presentation of one parameter.  `parent` is the name of the group
/// it sits in (null or empty for the top level); `hint` is the tooltip.
struct ParamLook {
    const char* label = nullptr;   ///< Shown to the user; the parameter NAME is never shown.
    const char* hint = nullptr;    ///< Tooltip, or null.
    const char* parent = nullptr;  ///< Group name, or null for the top level.
    bool animates = true;          ///< False for controls that must not keyframe.
};

/// Define a choice (popup).  `default0` is the 0-BASED default item: OFX
/// numbers choice items from 0, the Premiere popups these mirror from 1.
OfxPropertySetHandle defineChoice(OfxParamSetHandle set, const char* name, const ParamLook& look,
                                  std::string_view pipeItems, int default0) noexcept;

/// How a double parameter is presented.
struct DoubleRange {
    double min = 0.0;         ///< Hard minimum (kOfxParamPropMin).
    double max = 1.0;         ///< Hard maximum (kOfxParamPropMax).
    double displayMin = 0.0;  ///< Slider start (kOfxParamPropDisplayMin).
    double displayMax = 1.0;  ///< Slider end (kOfxParamPropDisplayMax).
    double value = 0.0;       ///< Default.
    int digits = 1;           ///< Decimal places shown.
    double increment = 0.1;   ///< Arrow-key / drag step.
    bool angle = false;       ///< kOfxParamDoubleTypeAngle (degrees).
};

OfxPropertySetHandle defineDouble(OfxParamSetHandle set, const char* name, const ParamLook& look,
                                  const DoubleRange& range) noexcept;
OfxPropertySetHandle defineInt(OfxParamSetHandle set, const char* name, const ParamLook& look, int value,
                               int min, int max) noexcept;
OfxPropertySetHandle defineBool(OfxParamSetHandle set, const char* name, const ParamLook& look,
                                bool value) noexcept;
/// A group.  `open` false starts it collapsed.
OfxPropertySetHandle defineGroup(OfxParamSetHandle set, const char* name, const ParamLook& look,
                                 bool open) noexcept;
/// A string.  `mode` is kOfxParamStringIsFilePath, kOfxParamStringIsLabel, ...
OfxPropertySetHandle defineString(OfxParamSetHandle set, const char* name, const ParamLook& look,
                                  const char* mode, const char* value) noexcept;

// ===========================================================================
//  Parameter values at render / change time
// ===========================================================================

/// Handle of a parameter by name, or null.
[[nodiscard]] OfxParamHandle paramHandle(OfxParamSetHandle set, const char* name) noexcept;

/// A double (or angle) at `time`; `fallback` when it cannot be read or is
/// not finite.
[[nodiscard]] double doubleAt(OfxParamSetHandle set, const char* name, OfxTime time, double fallback) noexcept;
/// An integer, boolean or choice at `time` (all three read as int).
[[nodiscard]] int intAt(OfxParamSetHandle set, const char* name, OfxTime time, int fallback) noexcept;
/// A string at `time` (copied out of the host's storage immediately).
[[nodiscard]] std::string stringAt(OfxParamSetHandle set, const char* name, OfxTime time) noexcept;

/// Write a double from kOfxActionInstanceChanged.  When the control already
/// has keyframes the value is written as a keyframe at `time` (so the edit
/// lands where the user is standing, as every keyframed control behaves);
/// otherwise the static value is set.  Returns false on any failure.
bool writeDouble(OfxParamSetHandle set, const char* name, double value, OfxTime time) noexcept;
/// Same for an integer / choice / boolean.
bool writeInt(OfxParamSetHandle set, const char* name, int value, OfxTime time) noexcept;
/// Set a string (never keyframed).
bool writeString(OfxParamSetHandle set, const char* name, const char* value) noexcept;

/// Show or hide a parameter (kOfxParamPropSecret) and enable or grey it out
/// (kOfxParamPropEnabled).  Hosts that ignore one of the two after creation
/// simply keep the other; neither is ever an error.
void setParamVisible(OfxParamSetHandle set, const char* name, bool visible) noexcept;

/// Group the writes between begin and end into one undo step.
class EditGroup {
public:
    EditGroup(OfxParamSetHandle set, const char* label) noexcept;
    ~EditGroup();
    EditGroup(const EditGroup&) = delete;
    EditGroup& operator=(const EditGroup&) = delete;

private:
    OfxParamSetHandle m_set = nullptr;
    bool m_open = false;
};

// ===========================================================================
//  Images
// ===========================================================================

/// One fetched clip image, released on destruction.
///
/// `data` addresses the image's BOTTOM-LEFT pixel (OFX puts y up), and row
/// `y` (in the image's own pixel coordinates) lives at
/// data + (y - bounds.y1) * rowBytes.  With CUDA render on it is a device
/// pointer; nothing here dereferences it.
class ClipImage {
public:
    ClipImage() = default;
    ClipImage(OfxImageClipHandle clip, OfxTime time) noexcept;
    ~ClipImage();
    ClipImage(const ClipImage&) = delete;
    ClipImage& operator=(const ClipImage&) = delete;

    /// True when the host returned an image with a data pointer.
    [[nodiscard]] bool valid() const noexcept { return m_image && data; }

    void* data = nullptr;           ///< kOfxImagePropData.
    OfxRectI bounds{0, 0, 0, 0};    ///< kOfxImagePropBounds (pixels).
    int rowBytes = 0;               ///< kOfxImagePropRowBytes (may be negative in principle).
    bool rowBytesFromHost = false;  ///< True when the host reported the pitch (not the width * 16 fallback).
    std::string depth;              ///< kOfxImageEffectPropPixelDepth.
    std::string components;         ///< kOfxImageEffectPropComponents.

    [[nodiscard]] int width() const noexcept { return bounds.x2 - bounds.x1; }
    [[nodiscard]] int height() const noexcept { return bounds.y2 - bounds.y1; }

    /// True for a 32-bit float RGBA image, the only kind the effects accept.
    /// `lenient` (a generator's output) also accepts UNLABELLED images as
    /// long as the pitch holds four floats per pixel:
    ///   - an empty depth or components label;
    ///   - components kOfxImageComponentNone, but only with a pitch the host
    ///     reported itself, so the four floats per pixel are proven by the
    ///     host's own allocation rather than assumed.
    [[nodiscard]] bool isFloatRgba(bool lenient = false) const noexcept;

private:
    OfxPropertySetHandle m_image = nullptr;
};

/// A clip handle by name, or null.
[[nodiscard]] OfxImageClipHandle clipHandle(OfxImageEffectHandle effect, const char* name) noexcept;

/// Post a message to the user (error / warning / message) when the host has
/// a message suite.  Never fails; logs instead when there is no suite.
void postMessage(OfxImageEffectHandle effect, const char* type, const std::string& text) noexcept;

/// The render scale (x, y) of an action's inArgs, 1 when unset or unusable.
void renderScale(OfxPropertySetHandle inArgs, double& sx, double& sy) noexcept;

}  // namespace osv::ofx
