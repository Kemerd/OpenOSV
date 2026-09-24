// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxHost.cpp - the defensive wrappers declared in OfxHost.h.

#include "OfxHost.h"

#include "PluginLog.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace osv::ofx {

using osv::premiere::PluginLog;

namespace {

// ---------------------------------------------------------------------------
//  Module-wide suite table
//
//  Written by setHost / fetchSuites / clearSuites, which the host calls from
//  kOfxActionLoad / kOfxActionUnload - never concurrently with a render - and
//  read by everything else.  The mutex only guards the writers against each
//  other (both plug-ins in the module load separately); readers take a copy
//  of a pointer that cannot change while an instance exists.
// ---------------------------------------------------------------------------
std::mutex g_suiteMutex;
Suites g_suites;

/// True for a finite double; the only kind a parameter may hand onwards.
[[nodiscard]] bool finite(double v) noexcept { return std::isfinite(v); }

/// The property suite, or null.  Every helper starts here.
[[nodiscard]] const OfxPropertySuiteV1* propSuite() noexcept { return g_suites.prop; }

/// The parameter suite, or null.
[[nodiscard]] const OfxParameterSuiteV1* paramSuite() noexcept { return g_suites.param; }

/// Keyframe count of a parameter; 0 when it has none or the host cannot say.
[[nodiscard]] unsigned keyCount(OfxParamHandle handle) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    if (!ps || !handle || !ps->paramGetNumKeys) {
        return 0;
    }
    unsigned int n = 0;
    if (ps->paramGetNumKeys(handle, &n) != kOfxStatOK) {
        return 0;
    }
    return n;
}

/// Common part of every define*: create the parameter and apply the look.
[[nodiscard]] OfxPropertySetHandle defineCommon(OfxParamSetHandle set, const char* type, const char* name,
                                                const ParamLook& look) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    if (!ps || !set || !type || !name || !ps->paramDefine) {
        return nullptr;
    }
    OfxPropertySetHandle props = nullptr;
    if (ps->paramDefine(set, type, name, &props) != kOfxStatOK || !props) {
        PluginLog::warn("ofx: the host refused to define parameter '{}' ({})", name, type);
        return nullptr;
    }
    // The label is what the user reads; the name is the permanent id that
    // projects store and must never change.
    setString(props, kOfxPropLabel, look.label ? look.label : name);
    if (look.hint && *look.hint) {
        setString(props, kOfxParamPropHint, look.hint);
    }
    if (look.parent && *look.parent) {
        setString(props, kOfxParamPropParent, look.parent);
    }
    // A script name equal to the parameter name is what every Resolve sample
    // sets, and some hosts refuse parameters without one.
    setString(props, kOfxParamPropScriptName, name);
    return props;
}

}  // namespace

// ===========================================================================
//  Host and suites
// ===========================================================================

void setHost(OfxHost* host) noexcept {
    std::lock_guard<std::mutex> lock(g_suiteMutex);
    g_suites.host = host;
}

bool fetchSuites() noexcept {
    std::lock_guard<std::mutex> lock(g_suiteMutex);
    OfxHost* host = g_suites.host;
    if (!host || !host->fetchSuite) {
        PluginLog::error("ofx: kOfxActionLoad without a host (setHost was never called)");
        return false;
    }
    // fetchSuite returns a const void*; the version numbers are the ones the
    // plug-in was written against, and a host that cannot serve them returns
    // null rather than an incompatible table.
    const auto fetch = [host](const char* name, int version) noexcept -> const void* {
        return host->fetchSuite(host->host, name, version);
    };
    g_suites.prop = static_cast<const OfxPropertySuiteV1*>(fetch(kOfxPropertySuite, 1));
    g_suites.param = static_cast<const OfxParameterSuiteV1*>(fetch(kOfxParameterSuite, 1));
    g_suites.effect = static_cast<const OfxImageEffectSuiteV1*>(fetch(kOfxImageEffectSuite, 1));
    g_suites.message = static_cast<const OfxMessageSuiteV1*>(fetch(kOfxMessageSuite, 1));
    if (!g_suites.prop || !g_suites.param || !g_suites.effect) {
        PluginLog::error("ofx: the host is missing a required suite (property {}, parameter {}, image effect {})",
                         g_suites.prop ? "ok" : "MISSING", g_suites.param ? "ok" : "MISSING",
                         g_suites.effect ? "ok" : "MISSING");
        g_suites.prop = nullptr;
        g_suites.param = nullptr;
        g_suites.effect = nullptr;
        return false;
    }
    return true;
}

void clearSuites() noexcept {
    std::lock_guard<std::mutex> lock(g_suiteMutex);
    g_suites.prop = nullptr;
    g_suites.param = nullptr;
    g_suites.effect = nullptr;
    g_suites.message = nullptr;
}

const Suites& suites() noexcept { return g_suites; }

std::string hostName() noexcept {
    OfxHost* host = g_suites.host;
    if (!host) {
        return {};
    }
    return getString(host->host, kOfxPropName);
}

// ===========================================================================
//  Property sets
// ===========================================================================

std::string getString(OfxPropertySetHandle set, const char* name, int index, std::string_view fallback) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || index < 0 || !ps->propGetString) {
        return std::string(fallback);
    }
    char* value = nullptr;
    if (ps->propGetString(set, name, index, &value) != kOfxStatOK || !value) {
        return std::string(fallback);
    }
    try {
        return std::string(value);
    } catch (...) {
        return std::string();
    }
}

int getInt(OfxPropertySetHandle set, const char* name, int index, int fallback) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || index < 0 || !ps->propGetInt) {
        return fallback;
    }
    int value = 0;
    if (ps->propGetInt(set, name, index, &value) != kOfxStatOK) {
        return fallback;
    }
    return value;
}

double getDouble(OfxPropertySetHandle set, const char* name, int index, double fallback) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || index < 0 || !ps->propGetDouble) {
        return fallback;
    }
    double value = 0.0;
    if (ps->propGetDouble(set, name, index, &value) != kOfxStatOK || !finite(value)) {
        return fallback;
    }
    return value;
}

void* getPointer(OfxPropertySetHandle set, const char* name, int index, void* fallback) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || index < 0 || !ps->propGetPointer) {
        return fallback;
    }
    void* value = nullptr;
    if (ps->propGetPointer(set, name, index, &value) != kOfxStatOK) {
        return fallback;
    }
    return value;
}

bool getDoubles(OfxPropertySetHandle set, const char* name, double* out, int count) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || !out || count <= 0 || !ps->propGetDoubleN) {
        return false;
    }
    // Read into a scratch array first so a half-successful host call never
    // leaves `out` partly overwritten.
    double scratch[8] = {};
    if (count > 8 || ps->propGetDoubleN(set, name, count, scratch) != kOfxStatOK) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!finite(scratch[i])) {
            return false;
        }
    }
    for (int i = 0; i < count; ++i) {
        out[i] = scratch[i];
    }
    return true;
}

bool getInts(OfxPropertySetHandle set, const char* name, int* out, int count) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || !out || count <= 0 || count > 8 || !ps->propGetIntN) {
        return false;
    }
    int scratch[8] = {};
    if (ps->propGetIntN(set, name, count, scratch) != kOfxStatOK) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        out[i] = scratch[i];
    }
    return true;
}

int dimension(OfxPropertySetHandle set, const char* name) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || !ps->propGetDimension) {
        return -1;
    }
    int count = 0;
    if (ps->propGetDimension(set, name, &count) != kOfxStatOK) {
        return -1;
    }
    return count;
}

bool setString(OfxPropertySetHandle set, const char* name, const char* value, int index) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || !value || index < 0 || !ps->propSetString) {
        return false;
    }
    return ps->propSetString(set, name, index, value) == kOfxStatOK;
}

bool setInt(OfxPropertySetHandle set, const char* name, int value, int index) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || index < 0 || !ps->propSetInt) {
        return false;
    }
    return ps->propSetInt(set, name, index, value) == kOfxStatOK;
}

bool setDouble(OfxPropertySetHandle set, const char* name, double value, int index) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || index < 0 || !finite(value) || !ps->propSetDouble) {
        return false;
    }
    return ps->propSetDouble(set, name, index, value) == kOfxStatOK;
}

bool setPointer(OfxPropertySetHandle set, const char* name, void* value, int index) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || index < 0 || !ps->propSetPointer) {
        return false;
    }
    return ps->propSetPointer(set, name, index, value) == kOfxStatOK;
}

bool setDoubles(OfxPropertySetHandle set, const char* name, const double* values, int count) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || !values || count <= 0 || !ps->propSetDoubleN) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!finite(values[i])) {
            return false;
        }
    }
    return ps->propSetDoubleN(set, name, count, values) == kOfxStatOK;
}

bool setInts(OfxPropertySetHandle set, const char* name, const int* values, int count) noexcept {
    const OfxPropertySuiteV1* ps = propSuite();
    if (!ps || !set || !name || !values || count <= 0 || !ps->propSetIntN) {
        return false;
    }
    return ps->propSetIntN(set, name, count, values) == kOfxStatOK;
}

OfxPropertySetHandle effectProps(OfxImageEffectHandle effect) noexcept {
    const OfxImageEffectSuiteV1* es = g_suites.effect;
    if (!es || !effect || !es->getPropertySet) {
        return nullptr;
    }
    OfxPropertySetHandle props = nullptr;
    if (es->getPropertySet(effect, &props) != kOfxStatOK) {
        return nullptr;
    }
    return props;
}

OfxParamSetHandle effectParams(OfxImageEffectHandle effect) noexcept {
    const OfxImageEffectSuiteV1* es = g_suites.effect;
    if (!es || !effect || !es->getParamSet) {
        return nullptr;
    }
    OfxParamSetHandle params = nullptr;
    if (es->getParamSet(effect, &params) != kOfxStatOK) {
        return nullptr;
    }
    return params;
}

// ===========================================================================
//  Parameter description
// ===========================================================================

std::vector<std::string> splitItems(std::string_view pipeSeparated) {
    std::vector<std::string> items;
    std::size_t start = 0;
    // An empty string is one empty item in AE's grammar, but no popup of
    // ours is empty, so it yields no items at all rather than a blank entry.
    if (pipeSeparated.empty()) {
        return items;
    }
    while (start <= pipeSeparated.size()) {
        const std::size_t bar = pipeSeparated.find('|', start);
        const std::size_t end = (bar == std::string_view::npos) ? pipeSeparated.size() : bar;
        items.emplace_back(pipeSeparated.substr(start, end - start));
        if (bar == std::string_view::npos) {
            break;
        }
        start = bar + 1;
    }
    return items;
}

OfxPropertySetHandle defineChoice(OfxParamSetHandle set, const char* name, const ParamLook& look,
                                  std::string_view pipeItems, int default0) noexcept {
    OfxPropertySetHandle props = defineCommon(set, kOfxParamTypeChoice, name, look);
    if (!props) {
        return nullptr;
    }
    try {
        const std::vector<std::string> items = splitItems(pipeItems);
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            setString(props, kOfxParamPropChoiceOption, items[static_cast<std::size_t>(i)].c_str(), i);
        }
        // A default outside the list would make the host show nothing
        // selected; clamp it onto the list instead.
        const int last = static_cast<int>(items.size()) - 1;
        const int value = (default0 < 0 || default0 > last) ? 0 : default0;
        setInt(props, kOfxParamPropDefault, value);
    } catch (...) {
        PluginLog::warn("ofx: could not build the items of choice '{}'", name);
    }
    setInt(props, kOfxParamPropAnimates, look.animates ? 1 : 0);
    return props;
}

OfxPropertySetHandle defineDouble(OfxParamSetHandle set, const char* name, const ParamLook& look,
                                  const DoubleRange& range) noexcept {
    OfxPropertySetHandle props = defineCommon(set, kOfxParamTypeDouble, name, look);
    if (!props) {
        return nullptr;
    }
    // An angle is a plain double to the host's storage; the type only
    // changes the widget (a dial in some hosts, a degrees field in Resolve).
    setString(props, kOfxParamPropDoubleType, range.angle ? kOfxParamDoubleTypeAngle : kOfxParamDoubleTypePlain);
    setDouble(props, kOfxParamPropMin, range.min);
    setDouble(props, kOfxParamPropMax, range.max);
    setDouble(props, kOfxParamPropDisplayMin, range.displayMin);
    setDouble(props, kOfxParamPropDisplayMax, range.displayMax);
    setDouble(props, kOfxParamPropDefault, range.value);
    setInt(props, kOfxParamPropDigits, range.digits);
    setDouble(props, kOfxParamPropIncrement, range.increment);
    setInt(props, kOfxParamPropAnimates, look.animates ? 1 : 0);
    return props;
}

OfxPropertySetHandle defineInt(OfxParamSetHandle set, const char* name, const ParamLook& look, int value, int min,
                               int max) noexcept {
    OfxPropertySetHandle props = defineCommon(set, kOfxParamTypeInteger, name, look);
    if (!props) {
        return nullptr;
    }
    setInt(props, kOfxParamPropMin, min);
    setInt(props, kOfxParamPropMax, max);
    setInt(props, kOfxParamPropDisplayMin, min);
    setInt(props, kOfxParamPropDisplayMax, max);
    setInt(props, kOfxParamPropDefault, value);
    setInt(props, kOfxParamPropAnimates, look.animates ? 1 : 0);
    return props;
}

OfxPropertySetHandle defineBool(OfxParamSetHandle set, const char* name, const ParamLook& look, bool value) noexcept {
    OfxPropertySetHandle props = defineCommon(set, kOfxParamTypeBoolean, name, look);
    if (!props) {
        return nullptr;
    }
    setInt(props, kOfxParamPropDefault, value ? 1 : 0);
    setInt(props, kOfxParamPropAnimates, look.animates ? 1 : 0);
    return props;
}

OfxPropertySetHandle defineGroup(OfxParamSetHandle set, const char* name, const ParamLook& look, bool open) noexcept {
    OfxPropertySetHandle props = defineCommon(set, kOfxParamTypeGroup, name, look);
    if (!props) {
        return nullptr;
    }
    setInt(props, kOfxParamPropGroupOpen, open ? 1 : 0);
    return props;
}

OfxPropertySetHandle defineString(OfxParamSetHandle set, const char* name, const ParamLook& look, const char* mode,
                                  const char* value) noexcept {
    OfxPropertySetHandle props = defineCommon(set, kOfxParamTypeString, name, look);
    if (!props) {
        return nullptr;
    }
    setString(props, kOfxParamPropStringMode, mode ? mode : kOfxParamStringIsSingleLine);
    setString(props, kOfxParamPropDefault, value ? value : "");
    // A file path is chosen once; keyframing it would ask the host to
    // interpolate between two file names.
    setInt(props, kOfxParamPropAnimates, 0);
    if (mode && std::string_view(mode) == kOfxParamStringIsFilePath) {
        // The file must already exist: it is an input, never an output.
        setInt(props, kOfxParamPropStringFilePathExists, 1);
    }
    return props;
}

// ===========================================================================
//  Parameter values
// ===========================================================================

OfxParamHandle paramHandle(OfxParamSetHandle set, const char* name) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    if (!ps || !set || !name || !ps->paramGetHandle) {
        return nullptr;
    }
    OfxParamHandle handle = nullptr;
    if (ps->paramGetHandle(set, name, &handle, nullptr) != kOfxStatOK) {
        return nullptr;
    }
    return handle;
}

double doubleAt(OfxParamSetHandle set, const char* name, OfxTime time, double fallback) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    OfxParamHandle handle = paramHandle(set, name);
    if (!ps || !handle || !ps->paramGetValueAtTime || !finite(time)) {
        return fallback;
    }
    double value = 0.0;
    if (ps->paramGetValueAtTime(handle, time, &value) != kOfxStatOK || !finite(value)) {
        return fallback;
    }
    return value;
}

int intAt(OfxParamSetHandle set, const char* name, OfxTime time, int fallback) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    OfxParamHandle handle = paramHandle(set, name);
    if (!ps || !handle || !ps->paramGetValueAtTime || !finite(time)) {
        return fallback;
    }
    int value = 0;
    if (ps->paramGetValueAtTime(handle, time, &value) != kOfxStatOK) {
        return fallback;
    }
    return value;
}

std::string stringAt(OfxParamSetHandle set, const char* name, OfxTime time) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    OfxParamHandle handle = paramHandle(set, name);
    if (!ps || !handle || !ps->paramGetValueAtTime) {
        return {};
    }
    // The host owns the returned characters and may reuse them on the next
    // call, so they are copied out before anything else touches the suite.
    char* value = nullptr;
    OfxStatus status = ps->paramGetValueAtTime(handle, finite(time) ? time : 0.0, &value);
    if ((status != kOfxStatOK || !value) && ps->paramGetValue) {
        // A string is never animated; a host that only answers the static
        // form is still asked.
        status = ps->paramGetValue(handle, &value);
    }
    if (status != kOfxStatOK || !value) {
        return {};
    }
    try {
        return std::string(value);
    } catch (...) {
        return {};
    }
}

bool writeDouble(OfxParamSetHandle set, const char* name, double value, OfxTime time) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    OfxParamHandle handle = paramHandle(set, name);
    if (!ps || !handle || !finite(value)) {
        return false;
    }
    // A keyframed control takes the edit as a key at the current time, the
    // way a user's own edit of it would; a static one takes a static value.
    if (keyCount(handle) > 0 && ps->paramSetValueAtTime && finite(time)) {
        return ps->paramSetValueAtTime(handle, time, value) == kOfxStatOK;
    }
    if (!ps->paramSetValue) {
        return false;
    }
    return ps->paramSetValue(handle, value) == kOfxStatOK;
}

bool writeInt(OfxParamSetHandle set, const char* name, int value, OfxTime time) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    OfxParamHandle handle = paramHandle(set, name);
    if (!ps || !handle) {
        return false;
    }
    if (keyCount(handle) > 0 && ps->paramSetValueAtTime && finite(time)) {
        return ps->paramSetValueAtTime(handle, time, value) == kOfxStatOK;
    }
    if (!ps->paramSetValue) {
        return false;
    }
    return ps->paramSetValue(handle, value) == kOfxStatOK;
}

bool writeString(OfxParamSetHandle set, const char* name, const char* value) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    OfxParamHandle handle = paramHandle(set, name);
    if (!ps || !handle || !value || !ps->paramSetValue) {
        return false;
    }
    return ps->paramSetValue(handle, value) == kOfxStatOK;
}

void setParamVisible(OfxParamSetHandle set, const char* name, bool visible) noexcept {
    const OfxParameterSuiteV1* ps = paramSuite();
    if (!ps || !set || !name || !ps->paramGetHandle) {
        return;
    }
    OfxParamHandle handle = nullptr;
    OfxPropertySetHandle props = nullptr;
    if (ps->paramGetHandle(set, name, &handle, &props) != kOfxStatOK || !props) {
        return;
    }
    // Both properties are written: a host that honours Secret hides the
    // control, one that only honours Enabled greys it, and one that honours
    // neither still renders from the selected lens only.
    setInt(props, kOfxParamPropSecret, visible ? 0 : 1);
    setInt(props, kOfxParamPropEnabled, visible ? 1 : 0);
}

EditGroup::EditGroup(OfxParamSetHandle set, const char* label) noexcept : m_set(set) {
    const OfxParameterSuiteV1* ps = paramSuite();
    if (!ps || !m_set || !ps->paramEditBegin) {
        return;
    }
    m_open = ps->paramEditBegin(m_set, label ? label : "OpenOSV") == kOfxStatOK;
}

EditGroup::~EditGroup() {
    const OfxParameterSuiteV1* ps = paramSuite();
    if (m_open && ps && ps->paramEditEnd) {
        ps->paramEditEnd(m_set);
    }
}

// ===========================================================================
//  Images
// ===========================================================================

ClipImage::ClipImage(OfxImageClipHandle clip, OfxTime time) noexcept {
    const OfxImageEffectSuiteV1* es = g_suites.effect;
    if (!es || !clip || !es->clipGetImage || !finite(time)) {
        return;
    }
    OfxPropertySetHandle image = nullptr;
    // A null region asks for the whole image the host would give by default,
    // which for both effects is the full frame (they never declare tiles).
    if (es->clipGetImage(clip, time, nullptr, &image) != kOfxStatOK || !image) {
        return;
    }
    m_image = image;
    data = getPointer(image, kOfxImagePropData);
    int b[4] = {0, 0, 0, 0};
    if (getInts(image, kOfxImagePropBounds, b, 4)) {
        bounds = OfxRectI{b[0], b[1], b[2], b[3]};
    }
    rowBytes = getInt(image, kOfxImagePropRowBytes);
    rowBytesFromHost = rowBytes != 0;
    depth = getString(image, kOfxImageEffectPropPixelDepth);
    components = getString(image, kOfxImageEffectPropComponents);
    // A host that reports no pitch at all hands out tightly packed rows (the
    // assumption every one of Blackmagic's own sample kernels makes), so an
    // unset pitch means four floats per pixel rather than an unusable image.
    if (rowBytes == 0 && width() > 0) {
        rowBytes = width() * 16;
    }
}

ClipImage::~ClipImage() {
    const OfxImageEffectSuiteV1* es = g_suites.effect;
    if (m_image && es && es->clipReleaseImage) {
        es->clipReleaseImage(m_image);
    }
}

bool ClipImage::isFloatRgba(bool lenient) const noexcept {
    if (!valid() || width() <= 0 || height() <= 0 || rowBytes == 0) {
        return false;
    }
    // The pitch must hold a row of four floats, whatever the labels say.
    if (static_cast<long long>(rowBytes < 0 ? -rowBytes : rowBytes) < static_cast<long long>(width()) * 16) {
        return false;
    }
    // Unlabelled images are accepted only when the caller allows it: DaVinci
    // Resolve labels a GENERATOR's output OfxImageComponentNone unless the
    // generator states its format in kOfxImageEffectActionGetClipPreferences
    // (OfxSource.cpp does), while the image itself is its usual float RGBA.
    // "None" is trusted only with a host-reported pitch, so every byte this
    // plug-in writes is inside the host's own allocation.
    const bool depthOk = depth == kOfxBitDepthFloat || (lenient && depth.empty());
    const bool unlabelled = components.empty() || (components == kOfxImageComponentNone && rowBytesFromHost);
    const bool componentsOk = components == kOfxImageComponentRGBA || (lenient && unlabelled);
    if (depthOk && componentsOk && components != kOfxImageComponentRGBA) {
        PluginLog::oncef("ofx/image/unlabelled", PluginLog::Level::Warn,
                         "ofx: the host labelled an image '{}' '{}' with {} bytes per row for {} pixels; treating it "
                         "as float RGBA",
                         depth, components, rowBytes, width());
    }
    return depthOk && componentsOk;
}

OfxImageClipHandle clipHandle(OfxImageEffectHandle effect, const char* name) noexcept {
    const OfxImageEffectSuiteV1* es = g_suites.effect;
    if (!es || !effect || !name || !es->clipGetHandle) {
        return nullptr;
    }
    OfxImageClipHandle clip = nullptr;
    if (es->clipGetHandle(effect, name, &clip, nullptr) != kOfxStatOK) {
        return nullptr;
    }
    return clip;
}

void postMessage(OfxImageEffectHandle effect, const char* type, const std::string& text) noexcept {
    const OfxMessageSuiteV1* ms = g_suites.message;
    if (!ms || !ms->message || !effect || !type) {
        PluginLog::info("ofx message ({}): {}", type ? type : "?", text);
        return;
    }
    // The text goes through "%s" so a percent sign in a file name can never
    // be read as a format directive.
    ms->message(effect, type, nullptr, "%s", text.c_str());
}

void renderScale(OfxPropertySetHandle inArgs, double& sx, double& sy) noexcept {
    double s[2] = {1.0, 1.0};
    if (!getDoubles(inArgs, kOfxImageEffectPropRenderScale, s, 2) || !(s[0] > 0.0) || !(s[1] > 0.0)) {
        s[0] = 1.0;
        s[1] = 1.0;
    }
    sx = s[0];
    sy = s[1];
}

}  // namespace osv::ofx
