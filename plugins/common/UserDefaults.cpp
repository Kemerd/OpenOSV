// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// UserDefaults: the per-user Source Settings defaults file (see UserDefaults.h
// for what it is for and who uses it).
//
// The file is organised in three layers, bottom up:
//
//   1. the FIELD TABLE - one row per JSON key, naming the PrefsBlob bytes it
//      stores and how its value is spelled.  Everything else walks this table,
//      so a setting is added to the file by adding one row;
//   2. the PURE LAYER - text <-> settings and path-explicit file access, with
//      no global state, which is what the unit tests drive;
//   3. the PROCESS-WIDE LAYER - the user's own file, resolved from the
//      environment, cached per module and guarded by one mutex.
//
// Nothing here throws across the public API: every entry point is noexcept
// and turns an exception (allocation failure, a filesystem error) into a
// Status or into the built-in defaults.  The plug-ins call this code from
// inside Premiere's C call stack, where an escaping exception is fatal.

#include "UserDefaults.h"

#include "osv/core/Log.h"
#include "osv/core/Version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fstream>
#include <unistd.h>
#endif

namespace osv::premiere {

namespace {

/// ordered_json keeps the keys in insertion order, so the file lists them in
/// the table's order - the order of the Source Settings panel - rather than
/// alphabetically.  A person editing the file by hand reads it top to bottom.
using Json = nlohmann::ordered_json;

// ===========================================================================
//  Logging
// ===========================================================================

/// The installed sink; null means "use osv::log".  An atomic function
/// pointer: installed once per module at start-up, read on every message.
std::atomic<UserDefaultsLogSink> g_sink{nullptr};

/// One message queued while a lock was held, emitted after it is released.
struct PendingLog {
    UserDefaultsLogLevel level = UserDefaultsLogLevel::Debug;
    std::string text;
};

/// Deliver one message to the installed sink, or to osv::log without one.
/// Never throws: a logging failure must not become a settings failure.
void emit(UserDefaultsLogLevel level, std::string_view text) noexcept {
    const UserDefaultsLogSink sink = g_sink.load(std::memory_order_acquire);
    if (sink) {
        sink(level, text);
        return;
    }
    try {
        // Map onto the library facade's levels one to one.
        log::Level mapped = log::Level::Debug;
        switch (level) {
        case UserDefaultsLogLevel::Info:  mapped = log::Level::Info; break;
        case UserDefaultsLogLevel::Warn:  mapped = log::Level::Warn; break;
        case UserDefaultsLogLevel::Error: mapped = log::Level::Error; break;
        case UserDefaultsLogLevel::Debug:
        default:                          mapped = log::Level::Debug; break;
        }
        if (log::enabled(mapped)) {
            log::message(mapped, text);
        }
    } catch (...) {
        // Dropped: the facade's own sink failed, and there is nowhere else.
    }
}

/// Emit a batch collected under a lock.
void emitAll(const std::vector<PendingLog>& pending) noexcept {
    for (const PendingLog& p : pending) {
        emit(p.level, p.text);
    }
}

/// Append a message to a batch; an allocation failure only loses the line.
void queue(std::vector<PendingLog>& pending, UserDefaultsLogLevel level, std::string text) noexcept {
    try {
        pending.push_back(PendingLog{level, std::move(text)});
    } catch (...) {
        // Out of memory: the message is the least of the problems.
    }
}

// ===========================================================================
//  Token lists
//
//  Each list is indexed by the PrefsBlob enum value it spells, so the
//  static_asserts below pin every list's length to its enum's Count: an enum
//  that gains a value breaks the BUILD here instead of writing a number the
//  reader cannot map back.  The spellings are lower-case, hyphenated and
//  final - they are what users type into the file by hand.
// ===========================================================================

constexpr const char* kColourTokens[] = {"pq", "hlg", "rec709", "dlogm"};
constexpr const char* kLookTokens[] = {"dji", "standard"};
constexpr const char* kSizeTokens[] = {"native", "3840x1920", "2560x1280", "1920x960"};
constexpr const char* kStabTokens[] = {"off", "horizon-lock", "full", "smooth"};
// Indexed by PrefsCalibrationChoice (the user's choice), NOT by the stored
// PrefsCalibration byte: "native" means the FORCED bare-lens set, and "auto"
// follows the accessory the camera recorded, exactly as the UIs say.
constexpr const char* kCalibTokens[] = {"auto", "native", "lens-protectors", "underwater"};
constexpr const char* kFitTokens[] = {"dji-refit", "pocket3", "osmo360"};
constexpr const char* kDeviceTokens[] = {"auto", "cpu", "cuda", "opencl"};
constexpr const char* kFlowTokens[] = {"auto", "classical", "neural"};
constexpr const char* kPhotoTokens[] = {"off", "rim-only", "rim-and-colour"};
constexpr const char* kDirectTokens[] = {"sequence-space", "match-source"};
constexpr const char* kShadingTokens[] = {"off", "auto"};  // [WP-VIGNETTE]

static_assert(std::size(kColourTokens) == static_cast<std::size_t>(PrefsColorOutput::Count),
              "colourOutput does not spell every PrefsColorOutput value");
static_assert(std::size(kLookTokens) == static_cast<std::size_t>(PrefsLook::Count),
              "rec709Look does not spell every PrefsLook value");
static_assert(std::size(kSizeTokens) == static_cast<std::size_t>(PrefsOutputSize::Count),
              "outputSize does not spell every PrefsOutputSize value");
static_assert(std::size(kStabTokens) == static_cast<std::size_t>(PrefsStabilization::Count),
              "stabilisation does not spell every PrefsStabilization value");
static_assert(std::size(kCalibTokens) == static_cast<std::size_t>(PrefsCalibrationChoice::Count),
              "calibration does not spell every PrefsCalibrationChoice value");
static_assert(std::size(kFitTokens) == static_cast<std::size_t>(PrefsDlogmFit::Count),
              "dlogmCurve does not spell every PrefsDlogmFit value");
static_assert(std::size(kDeviceTokens) == static_cast<std::size_t>(PrefsRenderDevice::Count),
              "renderDevice does not spell every PrefsRenderDevice value");
static_assert(std::size(kFlowTokens) == static_cast<std::size_t>(PrefsFlowBackend::Count),
              "flowBackend does not spell every PrefsFlowBackend value");
static_assert(std::size(kPhotoTokens) == static_cast<std::size_t>(PrefsPhotoSeam::Count),
              "skySeamFix does not spell every PrefsPhotoSeam value");
static_assert(std::size(kDirectTokens) == static_cast<std::size_t>(PrefsDirectColour::Count),
              "programMonitorColour does not spell every PrefsDirectColour value");
static_assert(std::size(kShadingTokens) == static_cast<std::size_t>(PrefsLensShading::Count),
              "lensShading does not spell every PrefsLensShading value");

/// ASCII case-insensitive equality, so "HLG" typed by hand still reads.
[[nodiscard]] bool sameToken(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// The token of an enum value.  A value past the list (an unsanitised blob)
/// is spelled with the list's first token rather than indexing out of range;
/// every caller sanitises first, so this is a guard, not a code path.
[[nodiscard]] Json tokenJson(std::uint8_t value, std::span<const char* const> tokens) {
    const std::size_t index = value < tokens.size() ? value : 0u;
    return Json(tokens[index]);
}

/// Read a token back into an enum byte.  False (with the reason) when the
/// value is not a string or names no token; `out` is then left alone, so the
/// built-in value stays.
[[nodiscard]] bool tokenFrom(const Json& value, std::span<const char* const> tokens, std::uint8_t& out,
                             std::string& why) {
    if (!value.is_string()) {
        why = "expected one of the documented words, got a " + std::string(value.type_name());
        return false;
    }
    const std::string& text = value.get_ref<const std::string&>();
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (sameToken(text, tokens[i])) {
            out = static_cast<std::uint8_t>(i);
            return true;
        }
    }
    why = "'" + log::safe(text) + "' is not one of:";
    for (const char* t : tokens) {
        why += ' ';
        why += t;
    }
    return false;
}

/// Read a JSON boolean.  Strict on purpose: "yes", 1 or "on" are refused
/// (and noted) rather than guessed at.
[[nodiscard]] bool boolFrom(const Json& value, bool& out, std::string& why) {
    if (!value.is_boolean()) {
        why = "expected true or false, got a " + std::string(value.type_name());
        return false;
    }
    out = value.get<bool>();
    return true;
}

/// Read a JSON number into a double.  Integers and floats are both numbers
/// to a person editing the file, so both are accepted.
[[nodiscard]] bool numberFrom(const Json& value, double& out, std::string& why) {
    if (!value.is_number()) {
        why = "expected a number, got a " + std::string(value.type_name());
        return false;
    }
    const double v = value.get<double>();
    if (!std::isfinite(v)) {
        why = "the number is not finite";
        return false;
    }
    out = v;
    return true;
}

/// The shortest decimal that reads back as exactly `value` (0.1f -> 0.1, not
/// 0.100000001490116), as a double JSON number.  A float's shortest decimal
/// spelling, re-read as a double and narrowed again, is the original float,
/// so the exposure survives a save / load cycle bit for bit.
[[nodiscard]] Json floatJson(float value) {
    char buffer[64] = {};
    const std::to_chars_result printed = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value);
    if (printed.ec != std::errc()) {
        return Json(static_cast<double>(value));
    }
    double asDouble = 0.0;
    const std::from_chars_result parsed = std::from_chars(buffer, printed.ptr, asDouble);
    if (parsed.ec != std::errc()) {
        return Json(static_cast<double>(value));
    }
    return Json(asDouble);
}

// [WP-SEAMTOOLS] The seam tools' ranges, in degrees: exactly what the blob's
// codes store (PrefsBlob.h), and so what the Source Settings sliders offer.
constexpr double kSeamBlendMinDeg =
    static_cast<double>(PrefsBlob::kMinSeamBlendCode - 1) / PrefsBlob::kSeamToolStepsPerDeg;
constexpr double kSeamBlendMaxDeg =
    static_cast<double>(PrefsBlob::kMaxSeamBlendCode - 1) / PrefsBlob::kSeamToolStepsPerDeg;
constexpr double kParallaxBlendMaxDeg =
    static_cast<double>(PrefsBlob::kMaxParallaxBlendCode - 1) / PrefsBlob::kSeamToolStepsPerDeg;
constexpr double kSeamSmoothingMaxDeg =
    static_cast<double>(PrefsBlob::kMaxSeamSmoothingCode - 1) / PrefsBlob::kSeamToolStepsPerDeg;
constexpr double kSeamOffsetMaxDeg = static_cast<double>(PrefsBlob::kMaxSeamOffsetHundredths) / 100.0;

/// [WP-SEAMTOOLS] Read a seam tool's degrees, brought into [lo, hi] (and
/// `clamped` set when that moved it); false with the reason for anything
/// that is not a finite number.
[[nodiscard]] bool seamToolDegrees(const Json& value, double lo, double hi, double& out, std::string& why,
                                   bool& clamped) {
    double deg = 0.0;
    if (!numberFrom(value, deg, why)) {
        return false;
    }
    clamped = deg < lo || deg > hi;
    out = std::clamp(deg, lo, hi);
    return true;
}

// ===========================================================================
//  The field table
// ===========================================================================

/// One row: a key, the bytes it stores, and how its value is spelled.
struct FieldSpec {
    UserDefaultsField layout;
    /// The value of this setting in `prefs` (already sanitised).
    Json (*write)(const PrefsBlob& prefs);
    /// Store a value read from the file into `prefs`.  False, with the
    /// reason in `why`, when the value cannot be used; `prefs` keeps its
    /// built-in value for this setting then.  `clamped` is set when a number
    /// was brought into range rather than refused.
    bool (*read)(const Json& value, PrefsBlob& prefs, std::string& why, bool& clamped);
};

/// A PrefsBlob byte range, for the table.
#define OSV_UD_FIELD(member) offsetof(PrefsBlob, member), sizeof(PrefsBlob::member)

/// Every setting the file stores, in Source Settings panel order.
///
/// Adding a PrefsBlob field means adding a row here - and the unit test
/// "the defaults file covers every byte of the blob" fails until someone
/// does, because a field without a row would silently never become a
/// default.  Keys are camelCase, British spelling like the UI.
const FieldSpec kFields[] = {
    // ---- the top of the panel ------------------------------------------------
    {{"colourOutput", OSV_UD_FIELD(colorOutput), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.colorOutput, kColourTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) { return tokenFrom(v, kColourTokens, p.colorOutput, why); }},
    {{"rec709Look", OSV_UD_FIELD(look), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.look, kLookTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) { return tokenFrom(v, kLookTokens, p.look, why); }},
    {{"outputSize", OSV_UD_FIELD(outputSize), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.outputSize, kSizeTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) { return tokenFrom(v, kSizeTokens, p.outputSize, why); }},
    {{"stabilisation", OSV_UD_FIELD(stabilization), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.stabilization, kStabTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         return tokenFrom(v, kStabTokens, p.stabilization, why);
     }},

    // ---- Stitching -------------------------------------------------------------
    {{"seamSearch", OSV_UD_FIELD(seamSearch), 0, 0},
     [](const PrefsBlob& p) { return Json(p.seamSearch != 0); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         bool on = false;
         if (!boolFrom(v, on, why)) {
             return false;
         }
         p.seamSearch = on ? 1u : 0u;
         return true;
     }},
    {{"exposureMatch", OSV_UD_FIELD(gainMatch), 0, 0},
     [](const PrefsBlob& p) { return Json(p.gainMatch != 0); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         bool on = false;
         if (!boolFrom(v, on, why)) {
             return false;
         }
         p.gainMatch = on ? 1u : 0u;
         return true;
     }},
    // Calibration is ONE choice stored in two bytes (calibration plus
    // calibrationForceNative); the blob's own setter writes the canonical
    // pair, so the file never has to know the encoding.
    {{"calibration", OSV_UD_FIELD(calibration), OSV_UD_FIELD(calibrationForceNative)},
     [](const PrefsBlob& p) { return tokenJson(static_cast<std::uint8_t>(p.calibrationChoice()), kCalibTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         std::uint8_t choice = 0;
         if (!tokenFrom(v, kCalibTokens, choice, why)) {
             return false;
         }
         p.setCalibrationChoice(static_cast<PrefsCalibrationChoice>(choice));
         return true;
     }},
    {{"sunGhostRemoval", OSV_UD_FIELD(flareRemoval), 0, 0},
     [](const PrefsBlob& p) { return Json(p.flareRemoval != 0); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         bool on = false;
         if (!boolFrom(v, on, why)) {
             return false;
         }
         p.flareRemoval = on ? 1u : 0u;
         return true;
     }},
    {{"skySeamFix", OSV_UD_FIELD(photoSeam), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.photoSeam, kPhotoTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) { return tokenFrom(v, kPhotoTokens, p.photoSeam, why); }},
    // Stored in whole percent, written as an integer; the blob's setter
    // rounds and clamps to 0..100, exactly as the panel's slider does.
    {{"skySeamStrengthPercent", OSV_UD_FIELD(photoStrength), 0, 0},
     [](const PrefsBlob& p) { return Json(static_cast<int>(std::lround(p.photoStrengthPercent()))); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double percent = 0.0;
         if (!numberFrom(v, percent, why)) {
             return false;
         }
         clamped = percent < 0.0 || percent > 100.0;
         p.setPhotoStrengthPercent(std::clamp(percent, 0.0, 100.0));
         return true;
     }},
    // Tenths of a degree, 0..6, through the blob's own setter.
    {{"seamEdgeInsetDeg", OSV_UD_FIELD(seamInset), 0, 0},
     [](const PrefsBlob& p) { return Json(p.seamInsetDeg()); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double deg = 0.0;
         if (!numberFrom(v, deg, why)) {
             return false;
         }
         const double maxDeg = static_cast<double>(PrefsBlob::kMaxSeamInsetCode - 1) / 10.0;
         clamped = deg < 0.0 || deg > maxDeg;
         p.setSeamInsetDeg(std::clamp(deg, 0.0, maxDeg));
         return true;
     }},
    // [WP-SEAMTOOLS] The carved seam's tweaks, degrees, each through the
    // blob's own setter (which rounds to its stored step) after the range
    // the panel's slider offers.
    {{"seamBlendDeg", OSV_UD_FIELD(seamBlend), 0, 0},
     [](const PrefsBlob& p) { return Json(p.seamBlendDeg()); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double deg = 0.0;
         if (!seamToolDegrees(v, kSeamBlendMinDeg, kSeamBlendMaxDeg, deg, why, clamped)) {
             return false;
         }
         p.setSeamBlendDeg(deg);
         return true;
     }},
    {{"parallaxBlendDeg", OSV_UD_FIELD(parallaxBlend), 0, 0},
     [](const PrefsBlob& p) { return Json(p.parallaxBlendDeg()); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double deg = 0.0;
         if (!seamToolDegrees(v, 0.0, kParallaxBlendMaxDeg, deg, why, clamped)) {
             return false;
         }
         p.setParallaxBlendDeg(deg);
         return true;
     }},
    {{"seamSmoothingDeg", OSV_UD_FIELD(seamSmoothing), 0, 0},
     [](const PrefsBlob& p) { return Json(p.seamSmoothingDeg()); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double deg = 0.0;
         if (!seamToolDegrees(v, 0.0, kSeamSmoothingMaxDeg, deg, why, clamped)) {
             return false;
         }
         p.setSeamSmoothingDeg(deg);
         return true;
     }},
    {{"nearOffsetDeg", OSV_UD_FIELD(nearOffset), 0, 0},
     [](const PrefsBlob& p) { return Json(p.nearOffsetDeg()); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double deg = 0.0;
         if (!seamToolDegrees(v, -kSeamOffsetMaxDeg, kSeamOffsetMaxDeg, deg, why, clamped)) {
             return false;
         }
         p.setNearOffsetDeg(deg);
         return true;
     }},
    {{"farOffsetDeg", OSV_UD_FIELD(farOffset), 0, 0},
     [](const PrefsBlob& p) { return Json(p.farOffsetDeg()); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double deg = 0.0;
         if (!seamToolDegrees(v, -kSeamOffsetMaxDeg, kSeamOffsetMaxDeg, deg, why, clamped)) {
             return false;
         }
         p.setFarOffsetDeg(deg);
         return true;
     }},
    // [WP-VIGNETTE] The lens shading correction: the mode as a token, the
    // strength in whole percent through the blob's own setter (which rounds
    // and clamps to 0..100, exactly as the panel's slider does).
    {{"lensShading", OSV_UD_FIELD(lensShading), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.lensShading, kShadingTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         return tokenFrom(v, kShadingTokens, p.lensShading, why);
     }},
    {{"shadingStrengthPercent", OSV_UD_FIELD(shadingStrength), 0, 0},
     [](const PrefsBlob& p) { return Json(static_cast<int>(std::lround(p.shadingStrengthPercent()))); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double percent = 0.0;
         if (!numberFrom(v, percent, why)) {
             return false;
         }
         clamped = percent < 0.0 || percent > 100.0;
         p.setShadingStrengthPercent(std::clamp(percent, 0.0, 100.0));
         return true;
     }},
    // Not in the Source Settings effect (only the modal dialog's hidden
    // fields and osvtool reach them), but they are settings, so they are
    // defaults too: a field missing here would reset silently on every save.
    {{"parallaxCorrection", OSV_UD_FIELD(parallax), 0, 0},
     [](const PrefsBlob& p) { return Json(p.parallaxEnabled()); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         bool on = false;
         if (!boolFrom(v, on, why)) {
             return false;
         }
         p.parallax = static_cast<std::uint8_t>(on ? PrefsParallax::On : PrefsParallax::Off);
         return true;
     }},
    {{"flowBackend", OSV_UD_FIELD(flowBackend), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.flowBackend, kFlowTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) { return tokenFrom(v, kFlowTokens, p.flowBackend, why); }},

    // ---- Advanced ----------------------------------------------------------------
    {{"dlogmCurve", OSV_UD_FIELD(dlogmFit), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.dlogmFit, kFitTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) { return tokenFrom(v, kFitTokens, p.dlogmFit, why); }},
    // Stops, -6..+6 (the blob's clamp range), written as the float's shortest
    // exact decimal so a save / load cycle is bit-exact.
    {{"exposureStops", OSV_UD_FIELD(exposureStops), 0, 0},
     [](const PrefsBlob& p) { return floatJson(p.exposureStops); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool& clamped) {
         double stops = 0.0;
         if (!numberFrom(v, stops, why)) {
             return false;
         }
         const double lo = static_cast<double>(PrefsBlob::kMinExposureStops);
         const double hi = static_cast<double>(PrefsBlob::kMaxExposureStops);
         clamped = stops < lo || stops > hi;
         p.exposureStops = static_cast<float>(std::clamp(stops, lo, hi));
         return true;
     }},
    {{"renderDevice", OSV_UD_FIELD(renderDevice), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.renderDevice, kDeviceTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         return tokenFrom(v, kDeviceTokens, p.renderDevice, why);
     }},
    {{"programMonitorColour", OSV_UD_FIELD(directColour), 0, 0},
     [](const PrefsBlob& p) { return tokenJson(p.directColour, kDirectTokens); },
     [](const Json& v, PrefsBlob& p, std::string& why, bool&) {
         return tokenFrom(v, kDirectTokens, p.directColour, why);
     }},
};

#undef OSV_UD_FIELD

/// The public view of the table, built once (the layouts are plain data).
const std::array<UserDefaultsField, std::size(kFields)> kFieldLayouts = [] {
    std::array<UserDefaultsField, std::size(kFields)> out{};
    for (std::size_t i = 0; i < std::size(kFields); ++i) {
        out[i] = kFields[i].layout;
    }
    return out;
}();

/// The row for `key`, or null.
[[nodiscard]] const FieldSpec* findField(std::string_view key) noexcept {
    for (const FieldSpec& f : kFields) {
        if (key == f.layout.key) {
            return &f;
        }
    }
    return nullptr;
}

// ===========================================================================
//  Environment and paths
// ===========================================================================

#ifdef _WIN32
/// Read an environment variable from the OS block (the one the tests and a
/// render farm set; the CRT copy can be stale).  Empty when unset.
[[nodiscard]] std::wstring environmentW(const wchar_t* name) {
    std::wstring value(1024, L'\0');
    DWORD n = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (n >= value.size()) {
        // Too small: n is the size needed including the terminator.
        value.assign(static_cast<std::size_t>(n), L'\0');
        n = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (n >= value.size()) {
            return {};  // changed under us; treat as unset rather than truncate
        }
    }
    value.resize(static_cast<std::size_t>(n));
    return value;
}
#else
/// Read an environment variable.  Empty when unset.
[[nodiscard]] std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}
#endif

/// A per-process counter for temporary file names, so two saves in one
/// process never share a temporary file.
std::atomic<std::uint32_t> g_tempCounter{0};

/// The temporary sibling a write goes through: same directory (a rename
/// across volumes is not atomic), unique per process and per call.
[[nodiscard]] std::filesystem::path temporarySibling(const std::filesystem::path& target) {
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const std::uint32_t n = g_tempCounter.fetch_add(1u, std::memory_order_relaxed);
    std::filesystem::path temp = target;
    temp += ".tmp-" + std::to_string(pid) + "-" + std::to_string(n);
    return temp;
}

// ===========================================================================
//  Raw file I/O
// ===========================================================================

#ifdef _WIN32
/// Human-readable Win32 error for a Status message.
[[nodiscard]] std::string win32Message(DWORD code) {
    return "Windows error " + std::to_string(static_cast<unsigned long>(code));
}

/// Read the whole file.  Shares read, write AND delete, so a concurrent save
/// (which renames over this file) is never blocked by a reader.
[[nodiscard]] Result<std::string> readWholeFile(const std::filesystem::path& path) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return Error{ErrorCode::NotFound, "no defaults file"};
        }
        return Error{ErrorCode::Io, "cannot open the defaults file (" + win32Message(err) + ")"};
    }
    // RAII close, whatever happens below.
    struct Closer {
        HANDLE h;
        ~Closer() { ::CloseHandle(h); }
    } closer{file};

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        return Error{ErrorCode::Io, "cannot size the defaults file (" + win32Message(::GetLastError()) + ")"};
    }
    if (static_cast<unsigned long long>(size.QuadPart) > kMaxUserDefaultsFileBytes) {
        return Error{ErrorCode::Malformed, "the defaults file is " + std::to_string(size.QuadPart) +
                                               " bytes; a real one is under 1 KB, so this is not it"};
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t done = 0;
    while (done < text.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(text.size() - done);
        if (!::ReadFile(file, text.data() + done, want, &got, nullptr)) {
            return Error{ErrorCode::Io, "cannot read the defaults file (" + win32Message(::GetLastError()) + ")"};
        }
        if (got == 0) {
            break;  // shorter than GetFileSizeEx said (truncated under us)
        }
        done += got;
    }
    text.resize(done);
    return text;
}

/// Write `text` to `temp`, flush it to the disk, and rename it over `target`.
[[nodiscard]] Status writeAtomically(const std::filesystem::path& target, const std::string& text) {
    const std::filesystem::path temp = temporarySibling(target);
    // CREATE_NEW: a leftover with this exact name is someone else's; refuse
    // rather than overwrite (the name is unique per process and call).
    HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return failStatus(ErrorCode::Io, "cannot create a temporary file beside the defaults file (" +
                                             win32Message(::GetLastError()) + ")");
    }
    bool ok = true;
    DWORD err = 0;
    std::size_t done = 0;
    while (ok && done < text.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(text.size() - done);
        if (!::WriteFile(file, text.data() + done, want, &wrote, nullptr) || wrote == 0) {
            ok = false;
            err = ::GetLastError();
            break;
        }
        done += wrote;
    }
    // Flush BEFORE the rename: after a power cut the file must hold either
    // the old document or the whole new one, never a renamed empty file.
    if (ok && !::FlushFileBuffers(file)) {
        ok = false;
        err = ::GetLastError();
    }
    ::CloseHandle(file);
    if (!ok) {
        ::DeleteFileW(temp.c_str());
        return failStatus(ErrorCode::Io, "cannot write the defaults file (" + win32Message(err) + ")");
    }

    // The rename.  A reader in another process that opened the old file
    // without FILE_SHARE_DELETE (an editor, a virus scanner) makes it fail
    // for a moment; a few short retries ride that out.
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (::MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return okStatus();
        }
        err = ::GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::DeleteFileW(temp.c_str());
    return failStatus(ErrorCode::Io, "cannot replace the defaults file (" + win32Message(err) + ")");
}

/// Delete `path`; a missing file is success.
[[nodiscard]] Status deleteFile(const std::filesystem::path& path) {
    DWORD err = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (::DeleteFileW(path.c_str())) {
            return okStatus();
        }
        err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return okStatus();
        }
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return failStatus(ErrorCode::Io, "cannot delete the defaults file (" + win32Message(err) + ")");
}
#else
/// Portable read for non-Windows builds of osvtool.
[[nodiscard]] Result<std::string> readWholeFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return Error{ErrorCode::NotFound, "no defaults file"};
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Error{ErrorCode::Io, "cannot size the defaults file: " + ec.message()};
    }
    if (size > kMaxUserDefaultsFileBytes) {
        return Error{ErrorCode::Malformed, "the defaults file is too large to be one"};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Error{ErrorCode::Io, "cannot open the defaults file"};
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

/// Portable atomic write: write + flush a sibling, then rename over.
[[nodiscard]] Status writeAtomically(const std::filesystem::path& target, const std::string& text) {
    const std::filesystem::path temp = temporarySibling(target);
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return failStatus(ErrorCode::Io, "cannot write the defaults file");
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        return failStatus(ErrorCode::Io, "cannot replace the defaults file: " + ec.message());
    }
    return okStatus();
}

/// Portable delete; a missing file is success.
[[nodiscard]] Status deleteFile(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return failStatus(ErrorCode::Io, "cannot delete the defaults file: " + ec.message());
    }
    return okStatus();
}
#endif

// ===========================================================================
//  The per-module cache
// ===========================================================================

/// What identifies one state of the file on disk: its modification time and
/// size, plus - on Windows - the file's own identity (volume serial and file
/// index).  Every save renames a NEW file over the old one, so the identity
/// changes on every save even when two saves land in the same timestamp tick
/// with the same length (the file system's clock is coarser than a click).
struct FileStamp {
    bool exists = false;
    std::uint64_t mtime = 0;       ///< Modification time, file-system units.
    std::uint64_t size = 0;        ///< Bytes.
    std::uint64_t identity = 0;    ///< File index (Windows); 0 elsewhere.
    std::uint32_t volume = 0;      ///< Volume serial (Windows); 0 elsewhere.

    [[nodiscard]] bool operator==(const FileStamp& o) const noexcept {
        return exists == o.exists &&
               (!exists || (mtime == o.mtime && size == o.size && identity == o.identity && volume == o.volume));
    }
};

/// The file's state now.  Any error reads as "no file", which is what the
/// subsequent read would conclude as well.
[[nodiscard]] FileStamp stampOf(const std::filesystem::path& path) noexcept {
    FileStamp s;
    if (path.empty()) {
        return s;
    }
#ifdef _WIN32
    // One handle, no access rights, every share mode: it neither blocks a
    // writer nor waits for one, and one call returns all four facts.
    HANDLE file = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return s;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = ::GetFileInformationByHandle(file, &info);
    ::CloseHandle(file);
    if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return s;  // unreadable, or a directory where the file should be
    }
    s.exists = true;
    s.mtime = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
              info.ftLastWriteTime.dwLowDateTime;
    s.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    s.identity = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    s.volume = info.dwVolumeSerialNumber;
    return s;
#else
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return s;
    }
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return FileStamp{};
    }
    const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
    if (ec) {
        return FileStamp{};
    }
    s.exists = true;
    s.mtime = static_cast<std::uint64_t>(written.time_since_epoch().count());
    s.size = static_cast<std::uint64_t>(bytes);
    return s;
#endif
}

/// The one cache of this module.  A function-local static so its
/// construction is ordered (first use) and never races DllMain.
struct Cache {
    std::mutex mutex;
    bool valid = false;               ///< False until the first load.
    std::filesystem::path path;       ///< The path `value` was read from.
    FileStamp stamp;                  ///< The file state `value` describes.
    UserDefaults value;               ///< What that state means.
};

[[nodiscard]] Cache& cache() noexcept {
    static Cache instance;
    return instance;
}

/// Load `path` in state `stamp` into a UserDefaults, queueing the log lines
/// that state deserves.  Called only when the state CHANGED, which is what
/// makes "a corrupt file is logged once" true without any extra bookkeeping.
[[nodiscard]] UserDefaults loadState(const std::filesystem::path& path, const FileStamp& stamp,
                                     std::vector<PendingLog>& pending) noexcept {
    UserDefaults out;
    out.path = path;
    if (path.empty()) {
        queue(pending, UserDefaultsLogLevel::Debug,
              "user defaults: no location (neither OPENOSV_DEFAULTS_FILE nor APPDATA is set); new clips use the "
              "built-in defaults");
        return out;
    }
    if (!stamp.exists) {
        queue(pending, UserDefaultsLogLevel::Debug,
              "user defaults: none saved (" + userDefaultsPathForLog(path) + "); new clips use the built-in defaults");
        return out;
    }
    Result<UserDefaultsDocument> doc = readUserDefaultsFile(path);
    if (!doc.ok()) {
        // The whole file is ignored: half-trusting a file we cannot parse is
        // how a clip ends up with settings nobody chose.
        queue(pending, UserDefaultsLogLevel::Warn,
              "user defaults: ignoring " + userDefaultsPathForLog(path) + " (" + doc.error().message +
                  "); new clips use the built-in defaults until it is fixed or saved again");
        return out;
    }
    out.prefs = doc.value().prefs;
    out.fromFile = true;
    for (const std::string& note : doc.value().notes) {
        queue(pending, UserDefaultsLogLevel::Info, "user defaults: " + userDefaultsPathForLog(path) + ": " + note);
    }
    queue(pending, UserDefaultsLogLevel::Info,
          "user defaults: read " + userDefaultsPathForLog(path) + " - " + userDefaultsSummary(out.prefs));
    return out;
}

}  // namespace

// ===========================================================================
//  Logging
// ===========================================================================

void setUserDefaultsLogSink(UserDefaultsLogSink sink) noexcept { g_sink.store(sink, std::memory_order_release); }

// ===========================================================================
//  The pure layer
// ===========================================================================

std::span<const UserDefaultsField> userDefaultsFields() noexcept { return kFieldLayouts; }

std::string userDefaultsToJson(const PrefsBlob& prefs) noexcept {
    try {
        // Sanitise a copy: every token below must be in range, and the file
        // must never carry a value the reader would refuse.
        PrefsBlob clean = prefs;
        if (!clean.isValid()) {
            clean = PrefsBlob::defaults();
        }
        clean.sanitise();

        Json settings = Json::object();
        for (const FieldSpec& f : kFields) {
            settings[f.layout.key] = f.write(clean);
        }
        Json doc = Json::object();
        doc["format"] = kUserDefaultsFormatName;
        doc["version"] = kUserDefaultsFormatVersion;
        // Informational only (never read back): which build wrote the file,
        // for a support conversation about an odd-looking default.
        doc["savedBy"] = std::string(Version::productName()) + " " + Version::string();
        doc["settings"] = std::move(settings);
        // Pretty-printed, two-space indent, ASCII only (ensure_ascii), and a
        // trailing newline so the file is a well-formed text file.
        std::string text = doc.dump(2, ' ', true);
        text += '\n';
        return text;
    } catch (...) {
        return {};
    }
}

Result<UserDefaultsDocument> userDefaultsFromJson(std::string_view text) noexcept {
    try {
        // allow_exceptions = false: a parse error yields a discarded value
        // instead of throwing; ignore_comments = true: // and /* */ allowed.
        const Json doc = Json::parse(text.begin(), text.end(), nullptr, false, true);
        if (doc.is_discarded()) {
            return Error{ErrorCode::Malformed, "not valid JSON"};
        }
        if (!doc.is_object()) {
            return Error{ErrorCode::Malformed, "the top level is not a JSON object"};
        }

        UserDefaultsDocument out;
        PrefsBlob prefs = PrefsBlob::defaults();

        // ---- whose file is this ----------------------------------------------
        // A "format" naming something else means the path points at some
        // other program's JSON; its keys must not be read as ours.  A missing
        // "format" is a hand-written file and is accepted with a note.
        const auto format = doc.find("format");
        if (format == doc.end()) {
            out.notes.emplace_back("no \"format\" key; reading it as an OpenOSV defaults file anyway");
        } else if (!format->is_string() || format->get_ref<const std::string&>() != kUserDefaultsFormatName) {
            return Error{ErrorCode::Malformed, "its \"format\" is not \"" + std::string(kUserDefaultsFormatName) + "\""};
        }

        // ---- which layout -----------------------------------------------------
        // Named keys make every version readable; a newer one only earns a
        // note, because its unknown keys will be skipped below.
        const auto version = doc.find("version");
        if (version != doc.end() && version->is_number_integer() &&
            version->get<long long>() > kUserDefaultsFormatVersion) {
            out.notes.emplace_back("written by a newer OpenOSV (layout version " +
                                   std::to_string(version->get<long long>()) +
                                   "); reading the settings this build knows");
        }

        // ---- the settings -----------------------------------------------------
        const auto settings = doc.find("settings");
        if (settings == doc.end()) {
            out.notes.emplace_back("no \"settings\"; every setting keeps its built-in value");
        } else if (!settings->is_object()) {
            return Error{ErrorCode::Malformed, "its \"settings\" is not a JSON object"};
        } else {
            // Known keys, in table order.  A key that is absent keeps its
            // built-in value silently: an older file simply predates it.
            for (const FieldSpec& f : kFields) {
                const auto value = settings->find(f.layout.key);
                if (value == settings->end()) {
                    continue;
                }
                // Read into a scratch copy so a refused value cannot leave a
                // half-written field (calibration writes two bytes).
                PrefsBlob scratch = prefs;
                std::string why;
                bool clamped = false;
                if (f.read(*value, scratch, why, clamped)) {
                    prefs = scratch;
                    if (clamped) {
                        out.notes.emplace_back(std::string("\"") + f.layout.key +
                                               "\" was out of range and has been clamped into it");
                    }
                } else {
                    out.notes.emplace_back(std::string("\"") + f.layout.key + "\": " + why +
                                           "; keeping the built-in value");
                }
            }
            // Unknown keys: a newer file or a typo.  Skipped, and said so, so
            // a hand edit that does nothing is at least explained in the log.
            for (auto it = settings->begin(); it != settings->end(); ++it) {
                if (!findField(it.key())) {
                    out.notes.emplace_back("unknown setting \"" + log::safe(it.key()) + "\" ignored");
                }
            }
        }

        prefs.sanitise();
        out.prefs = prefs;
        return out;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "out of memory while reading the defaults"};
    } catch (const std::exception& e) {
        return Error{ErrorCode::Malformed, std::string("unreadable: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Malformed, "unreadable"};
    }
}

Result<UserDefaultsDocument> readUserDefaultsFile(const std::filesystem::path& path) noexcept {
    try {
        if (path.empty()) {
            return Error{ErrorCode::InvalidArgument, "no defaults file path"};
        }
        Result<std::string> text = readWholeFile(path);
        if (!text.ok()) {
            return text.error();
        }
        return userDefaultsFromJson(text.value());
    } catch (...) {
        return Error{ErrorCode::Internal, "out of memory while reading the defaults file"};
    }
}

Status writeUserDefaultsFile(const std::filesystem::path& path, const PrefsBlob& prefs) noexcept {
    try {
        if (path.empty()) {
            return failStatus(ErrorCode::InvalidArgument, "no defaults file path");
        }
        const std::string text = userDefaultsToJson(prefs);
        if (text.empty()) {
            return failStatus(ErrorCode::Internal, "out of memory while writing the defaults");
        }
        // The directory may not exist yet (first save on this account).
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return failStatus(ErrorCode::Io, "cannot create " + userDefaultsPathForLog(parent) + ": " + ec.message());
            }
        }
        return writeAtomically(path, text);
    } catch (...) {
        return failStatus(ErrorCode::Internal, "unexpected failure while writing the defaults file");
    }
}

Status removeUserDefaultsFile(const std::filesystem::path& path) noexcept {
    try {
        if (path.empty()) {
            return failStatus(ErrorCode::InvalidArgument, "no defaults file path");
        }
        return deleteFile(path);
    } catch (...) {
        return failStatus(ErrorCode::Internal, "unexpected failure while deleting the defaults file");
    }
}

PrefsBlob storedPrefsOrUserDefaults(const void* bytes, std::size_t length, UserDefaults* usedDefaults,
                                    bool* fromDefaults) noexcept {
    // A blob of ours: the clip already has settings, and they always win.
    if (bytes && length >= PrefsBlob::kSize) {
        PrefsBlob stored;
        std::memcpy(&stored, bytes, PrefsBlob::kSize);
        if (stored.isValid()) {
            stored.sanitise();
            if (fromDefaults) {
                *fromDefaults = false;
            }
            return stored;
        }
    }
    // Anything else - no buffer, a short one, zeros, another importer's
    // bytes - is a clip with no settings yet: it starts from the user's.
    const UserDefaults defaults = currentUserDefaults();
    if (usedDefaults) {
        *usedDefaults = defaults;
    }
    if (fromDefaults) {
        *fromDefaults = true;
    }
    return defaults.prefs;
}

std::string userDefaultsSummary(const PrefsBlob& prefs) noexcept {
    try {
        PrefsBlob clean = prefs;
        if (!clean.isValid()) {
            clean = PrefsBlob::defaults();
        }
        clean.sanitise();
        std::string out;
        for (const FieldSpec& f : kFields) {
            if (!out.empty()) {
                out += ", ";
            }
            out += f.layout.key;
            out += ' ';
            const Json value = f.write(clean);
            out += value.is_string() ? value.get<std::string>() : value.dump();
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::string userDefaultsPathForLog(const std::filesystem::path& path) noexcept {
    try {
        const std::u8string utf8 = path.u8string();
        return log::safe(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    } catch (...) {
        return "<unprintable path>";
    }
}

// ===========================================================================
//  The process-wide layer
// ===========================================================================

std::filesystem::path userDefaultsPath() noexcept {
    try {
#ifdef _WIN32
        // The explicit override first: tests and render farms.
        const std::wstring overridePath = environmentW(kUserDefaultsFileEnvVarW);
        if (!overridePath.empty()) {
            return std::filesystem::path(overridePath);
        }
        // %APPDATA% (roaming): a preference that should follow the user.
        const std::wstring appData = environmentW(L"APPDATA");
        if (!appData.empty()) {
            return std::filesystem::path(appData) / L"OpenOSV" / L"defaults.json";
        }
#else
        const std::string overridePath = environment(kUserDefaultsFileEnvVar);
        if (!overridePath.empty()) {
            return std::filesystem::path(overridePath);
        }
        const std::string xdg = environment("XDG_CONFIG_HOME");
        if (!xdg.empty()) {
            return std::filesystem::path(xdg) / "openosv" / "defaults.json";
        }
        const std::string home = environment("HOME");
        if (!home.empty()) {
            return std::filesystem::path(home) / ".config" / "openosv" / "defaults.json";
        }
#endif
    } catch (...) {
        // Allocation failure: no location, which reads as "built-in".
    }
    return {};
}

UserDefaults currentUserDefaults() noexcept {
    std::vector<PendingLog> pending;
    UserDefaults result;
    try {
        // The stat happens outside the lock: it is the only per-call cost and
        // it needs no shared state.
        const std::filesystem::path path = userDefaultsPath();
        const FileStamp stamp = stampOf(path);

        Cache& c = cache();
        {
            std::lock_guard<std::mutex> guard(c.mutex);
            if (!c.valid || c.path != path || !(c.stamp == stamp)) {
                // A new state: read it once and remember what it meant.
                c.value = loadState(path, stamp, pending);
                c.path = path;
                c.stamp = stamp;
                c.valid = true;
            }
            result = c.value;
        }
    } catch (...) {
        // Allocation failure somewhere above: the built-in defaults, which
        // are always a correct (if unpersonalised) answer.
        result = UserDefaults{};
    }
    emitAll(pending);
    return result;
}

PrefsBlob userDefaults() noexcept { return currentUserDefaults().prefs; }

Status saveUserDefaults(const PrefsBlob& prefs) noexcept {
    std::vector<PendingLog> pending;
    Status status = okStatus();
    try {
        PrefsBlob clean = prefs;
        if (!clean.isValid()) {
            clean = PrefsBlob::defaults();
        }
        clean.sanitise();

        const std::filesystem::path path = userDefaultsPath();
        if (path.empty()) {
            status = failStatus(ErrorCode::NotFound,
                                "no place to save the defaults: neither OPENOSV_DEFAULTS_FILE nor APPDATA is set");
        } else {
            Cache& c = cache();
            // Under the cache lock, so two saves from one module land in a
            // defined order and the cache always describes the last one.
            std::lock_guard<std::mutex> guard(c.mutex);
            status = writeUserDefaultsFile(path, clean);
            if (status.ok()) {
                c.value = UserDefaults{clean, path, true};
                c.path = path;
                c.stamp = stampOf(path);
                c.valid = true;
                queue(pending, UserDefaultsLogLevel::Info,
                      "user defaults: saved to " + userDefaultsPathForLog(path) + " - new clips start from " +
                          userDefaultsSummary(clean));
            }
        }
        if (!status.ok()) {
            queue(pending, UserDefaultsLogLevel::Error, "user defaults: NOT saved - " + status.error().message);
        }
    } catch (...) {
        status = failStatus(ErrorCode::Internal, "unexpected failure while saving the defaults");
    }
    emitAll(pending);
    return status;
}

Status resetUserDefaults() noexcept {
    std::vector<PendingLog> pending;
    Status status = okStatus();
    try {
        const std::filesystem::path path = userDefaultsPath();
        if (path.empty()) {
            // Nothing can have been saved without a location, so the outcome
            // the user asked for already holds.
            queue(pending, UserDefaultsLogLevel::Info,
                  "user defaults: no location, so nothing to remove; new clips use the built-in defaults");
        } else {
            Cache& c = cache();
            std::lock_guard<std::mutex> guard(c.mutex);
            status = removeUserDefaultsFile(path);
            if (status.ok()) {
                c.value = UserDefaults{PrefsBlob::defaults(), path, false};
                c.path = path;
                c.stamp = stampOf(path);
                c.valid = true;
                queue(pending, UserDefaultsLogLevel::Info,
                      "user defaults: removed " + userDefaultsPathForLog(path) +
                          " - new clips start from the built-in defaults");
            } else {
                queue(pending, UserDefaultsLogLevel::Error, "user defaults: NOT removed - " + status.error().message);
            }
        }
    } catch (...) {
        status = failStatus(ErrorCode::Internal, "unexpected failure while removing the defaults");
    }
    emitAll(pending);
    return status;
}

}  // namespace osv::premiere
