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
// the host - DaVinci Resolve or VEGAS Pro - and our render.  The C API needs
// only a handful of calls, and every one of them can fail in a host we
// cannot test against.  So each call goes through one of the small helpers
// below, each of which:
//
//   * checks every handle and pointer before it is used;
//   * turns a failed host call into a documented fallback value;
//   * never throws (the dispatcher in OfxMain.cpp still catches everything,
//     because an exception unwinding into the host's C stack would take the
//     host down with it).
//
// Nothing here knows about 360 video; OfxCamera.h / OfxReframe.cpp /
// OfxSource.cpp build the two effects on top of it.
//
// It is also the one place that knows WHICH host loaded the module
// (hostProfile() below) and the one place that turns a host image's labels
// into a pixel format the render loops understand (ClipImage::view()):
// Resolve hands float RGBA only, VEGAS hands 8-bit or float in RGBA or BGRA.
#pragma once

#include "OfxHostImage.h"

#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <cstdint>
#include <optional>
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

// ---------------------------------------------------------------------------
//  Host profiles
// ---------------------------------------------------------------------------
// OpenFX leaves a lot to the host, and the two hosts the effects ship for
// fill the gaps differently (docs/RESOLVE.md, docs/VEGAS.md).  Every
// host-specific decision - which pixel depths to declare, which contexts,
// how thread-safe the generator claims to be, which images are accepted
// (ClipImage::view()), whether the generator has an Output Levels control
// and a Choose button - keys on ONE classification of the host's name, made
// here, so no other file ever compares host strings.
//
// The Generic profile is today's behaviour, unchanged: an unknown host is
// treated exactly like DaVinci Resolve was before VEGAS support existed.

/// The host families whose behaviour the effects adapt to.
enum class HostProfile : std::uint8_t {
    Generic = 0,  ///< Any other host (and the test harness): Resolve's behaviour.
    Resolve = 1,  ///< DaVinci Resolve / Resolve Studio ("DaVinciResolve", "DaVinciResolveLite").
    Vegas = 2,    ///< VEGAS Pro / Movie Studio (com.vegascreativesoftware.vegas, com.sonycreativesoftware.vegas...).
};

/// Classify a host name (kOfxPropName) - a pure function, so the tests can
/// feed it every name seen in the field.  Case-insensitive substring match:
/// "vegas" or "sonycreativesoftware" -> Vegas (the VEGAS Pro 2026 name is not
/// documented, and every known one contains "vegas"); "davinci" or
/// "resolve" -> Resolve; anything else, including "", -> Generic.
[[nodiscard]] HostProfile classifyHost(std::string_view name) noexcept;

/// The profile of the host that loaded the module: classifyHost(hostName()),
/// computed on first use after the suites are fetched and cached for the
/// rest of the module's life (a host never changes its name).  Generic while
/// the host cannot be asked yet.
[[nodiscard]] HostProfile hostProfile() noexcept;

/// A short name for logs ("generic", "resolve", "vegas").
[[nodiscard]] const char* hostProfileName(HostProfile profile) noexcept;

// ---------------------------------------------------------------------------
//  Change brackets: an edit, or the host taking attendance
// ---------------------------------------------------------------------------
// OpenFX hosts wrap each batch of kOfxActionInstanceChanged calls in
// kOfxActionBeginInstanceChanged / kOfxActionEndInstanceChanged.  An edit is
// one change per bracket.  VEGAS also sends a roll-call: right after it
// creates an instance (twice, observed live in VEGAS 17), one bracket
// announces EVERY parameter in definition order, each labelled
// kOfxChangeUserEdited although nobody touched anything.  Taken as edits,
// that roll-call re-applies the preset, switches the lens to Classic
// (because "fov" was "edited") and re-derives the DJI lens from Zoom.  So
// under VEGAS only the first change of a bracket counts as the user's; the
// rest of the bracket is the roll-call.  Other hosts are unaffected.

/// kOfxActionBeginInstanceChanged for `instance`: a bracket opens.
void changeBracketBegin(const void* instance) noexcept;

/// kOfxActionEndInstanceChanged (or kOfxActionDestroyInstance) for
/// `instance`: its bracket closes.
void changeBracketEnd(const void* instance) noexcept;

/// Count one kOfxActionInstanceChanged for `instance` (call it exactly once
/// per action) and answer whether it belongs to a host roll-call: true only
/// under VEGAS, for the second and later change of an open bracket.
[[nodiscard]] bool isHostRollCall(const void* instance) noexcept;

// ---- VEGAS's own OpenFX properties ------------------------------------------
// VEGAS's OpenFX extension header (ofxSonyVegas.h) defines the first and the
// last; the window handle is observed in VEGAS.  Every one of them may be
// absent - an older VEGAS, or any other host - and every reader copes.

/// Host property, string: the folder VEGAS keeps its logs and caches in.
inline constexpr const char* kPropVegasHostAppDataDirectory = "OfxPropVegasHostAppDataDirectory";
/// Host property, pointer: VEGAS's main window (an HWND on Windows).
inline constexpr const char* kPropVegasHostHWnd = "OfxPropVegasHostHWnd";
/// Effect instance property, string: where in VEGAS the instance lives
/// ("OfxImageEffectPropVegasContextGenerator", "...Event", "...Track"...).
inline constexpr const char* kPropVegasContext = "OfxImageEffectPropVegasContext";

/// The host's own property set (OfxHost::host: its name, version and
/// capabilities), or null before setHost().
[[nodiscard]] OfxPropertySetHandle hostProperties() noexcept;

/// How a property is typed in the OpenFX specification, for
/// describeProperty().  Only the documented type is read through a pointer;
/// the fallbacks are value reads, which a host that mistypes a property
/// cannot turn into a wild pointer.
enum class PropKind : std::uint8_t {
    String = 0,   ///< Read as a string; an int fallback for hosts that store "true"-style flags as ints.
    Int = 1,      ///< Read as an int; a double fallback.
    Pointer = 2,  ///< Read as a pointer and shown as an address, never dereferenced.
};

/// One property as text for a log line: "absent" when the set lacks it,
/// otherwise its values ("\"com.vegascreativesoftware.vegas\"", "1, 5",
/// "0x0001a2b4"...), at most 16 of them.  Never fails, never throws.
[[nodiscard]] std::string describeProperty(OfxPropertySetHandle set, const char* name, PropKind kind) noexcept;

/// Log, at INFO level, what the host says about itself: its name, label,
/// versions, supported contexts, pixel depths and components, the
/// multiple-depth / tiles / multi-resolution / temporal / overlay flags, its
/// CUDA / OpenCL / OpenGL render support, VEGAS's window and app data
/// properties, and the profile hostProfile() chose.  OpenFX has no way to
/// enumerate a property set, so it is a fixed list, each entry read
/// defensively ("absent" when missing).  Called once per module load.
void logHostDescription() noexcept;

/// State the pixel depths an effect descriptor accepts (kOfxActionDescribe).
///
///   * Generic / Resolve: 32-bit float only - exactly what the effects have
///     always declared (the panorama is HDR more often than not);
///   * VEGAS: 8-bit and float, each in R G B A and, through VEGAS's extension
///     tokens OfxBitDepthByteBGR / OfxBitDepthFloatBGR, in B G R A as well.
///     VEGAS converts every image to a depth the plug-in lists, so listing
///     8-bit keeps an 8-bit project from being converted twice.
void declarePixelDepths(OfxPropertySetHandle effectDescriptor, HostProfile profile) noexcept;

/// The image formats the effects accept under `profile`, in words, for the
/// "unsupported format" log lines ("32-bit float RGBA" outside VEGAS).
[[nodiscard]] const char* acceptedFormats(HostProfile profile) noexcept;

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
///
/// Under VEGAS the group is never opened: VEGAS Pro 17 crashes inside its
/// own paramEditBegin when an effect calls it from the InstanceChanged
/// actions VEGAS sends right after creating an instance (observed live: the
/// fault is inside VEGAS, called from here, before any user edit).  The
/// writes still happen; VEGAS just records them as separate undo steps.
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
///
/// The labels are copied out of the host's storage the moment the image is
/// fetched (VEGAS Pro 2026 fixed a use-after-free in the string buffers it
/// hands out; a copy is immune to either behaviour).
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
    bool rowBytesFromHost = false;  ///< True when the host reported the pitch (not the tightly-packed fallback).
    std::string depth;              ///< kOfxImageEffectPropPixelDepth.
    std::string components;         ///< kOfxImageEffectPropComponents.
    /// "OfxImageEffectPropPixelOrder" (VEGAS's OpenFX extension header,
    /// ofxSonyVegas.h): the image's own, else its clip's; empty when neither
    /// says, and never read outside VEGAS.
    std::string order;

    [[nodiscard]] int width() const noexcept { return bounds.x2 - bounds.x1; }
    [[nodiscard]] int height() const noexcept { return bounds.y2 - bounds.y1; }

    /// True for a 32-bit float RGBA image, the only kind the effects accept
    /// outside VEGAS.  `lenient` (a generator's output) also accepts
    /// UNLABELLED images as long as the pitch holds four floats per pixel:
    ///   - an empty depth or components label;
    ///   - components kOfxImageComponentNone, but only with a pitch the host
    ///     reported itself, so the four floats per pixel are proven by the
    ///     host's own allocation rather than assumed.
    [[nodiscard]] bool isFloatRgba(bool lenient = false) const noexcept;

    /// The image as the render loops see it, or std::nullopt when its format
    /// is not one the effects accept from a host of `profile`:
    ///
    ///   * Generic / Resolve: exactly isFloatRgba(lenient) - float RGBA,
    ///     nothing else, as it always was;
    ///   * VEGAS: 8-bit or 32-bit float, RGBA components, in R G B A or
    ///     B G R A.  The order is the "OfxImageEffectPropPixelOrder" label
    ///     (image, then clip); with no label an image whose depth is one of
    ///     the BGR extension tokens is B G R A and any other R G B A.  An
    ///     order label that is neither value refuses the image.  `lenient`
    ///     keeps the unlabelled-float rule above.
    ///
    /// Either way the pitch must hold a whole row of the chosen depth.
    [[nodiscard]] std::optional<HostImageView> view(HostProfile profile, bool lenient = false) const noexcept;

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
