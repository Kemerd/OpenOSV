// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectPathSettings.cpp - the direct path's Source Settings rule.
// See DirectPathSettings.h for what and why.

#include "DirectPathSettings.h"

#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace osv::reframe::direct {

namespace {

// ===========================================================================
//  Small helpers
// ===========================================================================

/// True when the block is large enough to hold every field this build reads.
/// A caller that zero-initialised the block and got nothing back (an older
/// engine) sees structSize 0 and therefore "nothing known".
[[nodiscard]] bool blockUsable(const OsvEngineClipSettings& s) noexcept {
    return s.structSize >= sizeof(OsvEngineClipSettings);
}

/// UTF-16 path to UTF-8 for a log line; never throws, "?" on failure.
[[nodiscard]] std::string utf8(const std::wstring& wide) noexcept {
    try {
        if (wide.empty()) {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                          nullptr, nullptr);
        if (n <= 0) {
            return "?";
        }
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    } catch (...) {
        return "?";
    }
}

/// The file name part of a path (after the last separator), for log lines.
[[nodiscard]] std::wstring fileNameOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

/// What the rule last decided for one file.
struct Remembered {
    std::uint32_t generation = 0;
    int workingTransfer = -1;
    bool direct = false;
};

/// Decisions per file, keyed by the path exactly as the media node spells
/// it (every instance of one clip reads the same property, so the spelling
/// is stable within a session).  Bounded: a session with thousands of clips
/// simply starts logging afresh once the table is full.
struct DecisionMemory {
    std::mutex mutex;
    std::map<std::wstring, Remembered> byPath;
};

/// Created on first use and never destroyed: a static destructor running
/// after Premiere unloaded the module's dependencies is not worth the risk
/// for a table of a few entries.
[[nodiscard]] DecisionMemory& memory() {
    static DecisionMemory* instance = new DecisionMemory();
    return *instance;
}

/// Upper bound on remembered files.
constexpr std::size_t kMaxRememberedFiles = 1024;

}  // namespace

// ===========================================================================
//  Public entry points
// ===========================================================================

bool colourModeOverride(ColourMode& out) noexcept {
    // GetEnvironmentVariableW rather than getenv: the effect and the host
    // may not share a CRT, but they always share the process environment.
    wchar_t value[16] = {};
    const DWORD n = GetEnvironmentVariableW(L"OSV_DIRECT_COLOR", value, static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value)) {
        return false;  // unset, or too long to be either word
    }
    if (_wcsicmp(value, L"working") == 0) {
        out = ColourMode::FollowWorkingSpace;
        return true;
    }
    if (_wcsicmp(value, L"match") == 0) {
        out = ColourMode::MatchClip;
        return true;
    }
    return false;
}

ColourMode colourModeFor(const OsvEngineClipSettings& settings) noexcept {
    ColourMode mode = ColourMode::MatchClip;
    if (colourModeOverride(mode)) {
        return mode;
    }
    // PrefsDirectColour::WorkingSpace is 1; anything else - including a
    // block from an engine that does not fill the field - is the default.
    return (blockUsable(settings) && settings.directColour == 1u) ? ColourMode::FollowWorkingSpace
                                                                   : ColourMode::MatchClip;
}

const char* transferLabel(int transfer) noexcept {
    switch (transfer) {
    case OSV_TRANSFER_PQ:          return "PQ";
    case OSV_TRANSFER_HLG:         return "HLG";
    case OSV_TRANSFER_REC709:      return "Rec.709";
    case OSV_TRANSFER_LINEAR:      return "linear";
    case OSV_TRANSFER_PASSTHROUGH: return "D-Log M passthrough";
    default:                       return "unknown";
    }
}

SettingsDecision decideSettings(const OsvEngineClipSettings& settings, int workingTransfer, ColourMode mode) noexcept {
    SettingsDecision d;
    try {
        // ---- nothing known ---------------------------------------------------
        // Premiere opens an importer instance of a clip - and that instance
        // publishes its Source Settings at imGetInfo8 - before it can hand the
        // effect a single frame of it, so "nothing published" means the
        // publication did not reach the engine (a file-identity mismatch, a
        // proxy-only session, an old importer).  Rendering the engine's
        // defaults then would show settings the user never chose, which is
        // exactly the field report this rule exists for.
        if (!blockUsable(settings) || settings.generation == 0) {
            d.direct = false;
            d.why = "the engine has not been told this clip's Source Settings (no Premiere importer instance of the "
                    "file published them in this process), so it cannot render them";
            return d;
        }

        // ---- the working space ---------------------------------------------
        // The caller only asks for spaces the direct path can produce; a
        // negative id here means it asked anyway, and the answer is no.
        if (workingTransfer < 0) {
            d.direct = false;
            d.why = "the sequence's working colour space is not one the direct path produces";
            return d;
        }

        const int clip = settings.clipTransfer;

        // ---- D-Log M passthrough --------------------------------------------
        // The user asked for the camera's log signal so a LUT downstream can
        // grade it.  What that signal looks like in the working space is
        // Premiere's interpretation of the importer's (approximate) colour
        // space declaration; rendering a finished picture instead would make
        // the LUT double-convert.  Only the equirect route is faithful.
        if (clip == OSV_TRANSFER_PASSTHROUGH) {
            d.direct = false;
            d.why = "the clip's colour output is the D-Log M passthrough, whose look in the working space is "
                    "Premiere's own interpretation of the log signal";
            return d;
        }

        // ---- the exact case -------------------------------------------------
        // The engine builds the colour block with the same function, curve,
        // exposure and bit depth as the importer's equirect; with the clip's
        // own transfer requested the two blocks are byte-identical (pinned by
        // the engine tests), and Premiere's source -> working conversion is
        // the identity.  Same pixels, one resampling fewer.
        if (clip == workingTransfer) {
            d.direct = true;
            d.why = std::string("the clip's colour output (") + transferLabel(clip) +
                    ") is the working space, so the direct path renders exactly what the equirect route would";
            return d;
        }

        // ---- a conversion only Premiere knows ---------------------------------
        if (mode == ColourMode::FollowWorkingSpace) {
            d.direct = true;
            d.why = std::string("Direct Path Colour is set to the working space: rendering straight into the ") +
                    transferLabel(workingTransfer) + " working space instead of converting the clip's " +
                    transferLabel(clip) + " output as Premiere would";
            return d;
        }
        d.direct = false;
        d.why = std::string("the clip's colour output is ") + transferLabel(clip) + " and the sequence works in " +
                transferLabel(workingTransfer) + "; Premiere's own " + transferLabel(clip) + " -> " +
                transferLabel(workingTransfer) +
                " conversion cannot be reproduced outside Premiere, so the equirect route keeps it (set Source "
                "Settings > colour output to " +
                transferLabel(workingTransfer) +
                ", or Direct Path Colour to the working space, to render straight from the fisheyes)";
        return d;
    } catch (...) {
        // std::string can only fail on allocation; the safe verdict is the
        // route that existed before the direct path.
        d.direct = false;
        d.why.clear();
        return d;
    }
}

bool isPolicyFallback(const std::string& reason) noexcept {
    return reason.rfind(kPolicyReasonPrefix, 0) == 0;
}

bool noteDecision(const std::wstring& path, const OsvEngineClipSettings& settings, int workingTransfer,
                  const SettingsDecision& decision) noexcept {
    try {
        DecisionMemory& m = memory();
        std::lock_guard<std::mutex> lock(m.mutex);
        const std::uint32_t generation = blockUsable(settings) ? settings.generation : 0u;
        auto it = m.byPath.find(path);
        if (it != m.byPath.end() && it->second.generation == generation &&
            it->second.workingTransfer == workingTransfer && it->second.direct == decision.direct) {
            return false;  // Same generation, same sequence space, same verdict: already logged.
        }
        if (it == m.byPath.end() && m.byPath.size() >= kMaxRememberedFiles) {
            m.byPath.clear();
        }
        Remembered& r = m.byPath[path];
        r.generation = generation;
        r.workingTransfer = workingTransfer;
        r.direct = decision.direct;
        return true;
    } catch (...) {
        // Logging bookkeeping must never cost a frame; report "not new".
        return false;
    }
}

void resetDecisionMemory() noexcept {
    try {
        DecisionMemory& m = memory();
        std::lock_guard<std::mutex> lock(m.mutex);
        m.byPath.clear();
    } catch (...) {
    }
}

std::string describeDecision(const std::wstring& path, const OsvEngineClipSettings& settings, int workingTransfer,
                             const SettingsDecision& decision) noexcept {
    try {
        const std::string name = utf8(fileNameOf(path));
        if (!blockUsable(settings) || settings.generation == 0) {
            return "reframe/direct: '" + name + "' Source Settings unknown -> equirect route: " + decision.why;
        }
        char head[320] = {};
        std::snprintf(head, sizeof(head),
                      "Source Settings generation %u (colour %s, exposure %+.2f stops, fit %u, calibration %u, "
                      "stabilisation %u, direct-path colour %s; file %08llx:%016llx)",
                      static_cast<unsigned>(settings.generation), transferLabel(settings.clipTransfer),
                      static_cast<double>(settings.exposureStops), static_cast<unsigned>(settings.dlogmFit),
                      static_cast<unsigned>(settings.calibration), static_cast<unsigned>(settings.stabilization),
                      colourModeFor(settings) == ColourMode::FollowWorkingSpace ? "working space" : "match clip",
                      static_cast<unsigned long long>(settings.fileVolume),
                      static_cast<unsigned long long>(settings.fileIndex));
        return "reframe/direct: '" + name + "' " + head + " in a " + transferLabel(workingTransfer) + " sequence -> " +
               (decision.direct ? "straight from the fisheyes: " : "equirect route: ") + decision.why;
    } catch (...) {
        return "reframe/direct: (decision description unavailable)";
    }
}

}  // namespace osv::reframe::direct
