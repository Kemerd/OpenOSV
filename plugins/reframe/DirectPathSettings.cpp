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

/// What the rules last decided for one file.
struct Remembered {
    std::uint32_t generation = 0;
    int workingTransfer = -1;
    bool direct = false;
    const char* rule = "";  ///< Static string: compared by address, never freed.
};

// ---------------------------------------------------------------------------
//  Rule names - the words the per-render route line prints.  Static storage,
//  so a SettingsDecision can point at them and noteDecision() can compare the
//  pointers.
// ---------------------------------------------------------------------------
constexpr const char* kRuleUnknown = "settings unknown";
constexpr const char* kRuleWorkingSpace = "working space";
constexpr const char* kRuleSameSpace = "same space";
constexpr const char* kRulePassthrough = "D-Log M passthrough";
constexpr const char* kRuleSequenceSpace = "Sequence space (fast)";
constexpr const char* kRuleMatchSource = "Match Source monitor";

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
        out = ColourMode::SequenceSpace;
        return true;
    }
    if (_wcsicmp(value, L"match") == 0) {
        out = ColourMode::MatchSource;
        return true;
    }
    return false;
}

ColourMode colourModeFor(const OsvEngineClipSettings& settings) noexcept {
    ColourMode mode = ColourMode::SequenceSpace;
    if (colourModeOverride(mode)) {
        return mode;
    }
    // PrefsDirectColour::MatchSource is 1; anything else - 0, a corrupt
    // value, or a block from an engine that does not fill the field - is the
    // default, Sequence space.
    return (blockUsable(settings) && settings.directColour == 1u) ? ColourMode::MatchSource
                                                                   : ColourMode::SequenceSpace;
}

const char* colourModeLabel(ColourMode mode) noexcept {
    return mode == ColourMode::MatchSource ? kRuleMatchSource : kRuleSequenceSpace;
}

int transferForSeiCodes(int primaries, int transfer) noexcept {
    // The spaces the colour pipeline produces exactly, by their H.273 codes.
    // First real sessions: primaries 9 / transfer 16 (Rec.2100 PQ) and
    // primaries 1 / transfer 1 (BT.709 RGB Full).
    if (primaries == 9 && transfer == 16) {
        return OSV_TRANSFER_PQ;
    }
    if (primaries == 9 && transfer == 18) {
        return OSV_TRANSFER_HLG;
    }
    // BT.709, SMPTE 170M and BT.2020 10/12-bit all name the same OETF.
    if (primaries == 1 && (transfer == 1 || transfer == 6 || transfer == 14 || transfer == 15)) {
        return OSV_TRANSFER_REC709;
    }
    return -1;
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
        // ---- 1. nothing known -------------------------------------------------
        // Premiere opens an importer instance of a clip - and that instance
        // publishes its Source Settings at imGetInfo8 - before it can hand the
        // effect a single frame of it, so "nothing published" means the
        // publication did not reach the engine (a file-identity mismatch, a
        // proxy-only session, an old importer).  Rendering the engine's
        // defaults then would show settings the user never chose, which is
        // exactly the field report this rule exists for.
        if (!blockUsable(settings) || settings.generation == 0) {
            d.direct = false;
            d.rule = kRuleUnknown;
            d.why = "the engine has not been told this clip's Source Settings (no Premiere importer instance of the "
                    "file published them in this process), so it cannot render them";
            return d;
        }

        // ---- 2. the working space ---------------------------------------------
        // The caller only asks for spaces the direct path can produce; a
        // negative id here means it asked anyway, and the answer is no.
        if (workingTransfer < 0) {
            d.direct = false;
            d.rule = kRuleWorkingSpace;
            d.why = "the sequence's working colour space is not one the direct path produces";
            return d;
        }

        const int clip = settings.clipTransfer;

        // ---- 3. the exact case ------------------------------------------------
        // The engine builds the colour block with the same function, curve,
        // exposure and bit depth as the importer's equirect; with the clip's
        // own transfer requested the two blocks are byte-identical (pinned by
        // the engine tests, the passthrough block included), and Premiere's
        // source -> working conversion is the identity.  Same pixels as the
        // Source monitor route, one resampling fewer.
        if (clip == workingTransfer) {
            d.direct = true;
            d.rule = kRuleSameSpace;
            d.why = std::string("the clip's colour output (") + transferLabel(clip) +
                    ") is the working space: identical to the Source monitor route";
            return d;
        }

        // ---- 4. D-Log M passthrough ---------------------------------------------
        // The user asked for the camera's log signal so a LUT downstream can
        // grade it.  The importer declares it as "BT.2020 RGB Full (Scene)"
        // (SEI 9 / 2), which is none of the working spaces the direct path
        // produces (transferForSeiCodes never returns passthrough), so rule 3
        // cannot apply; what the signal looks like in this working space is
        // Premiere's interpretation of that declaration, and rendering a
        // finished picture instead would make the LUT double-convert.
        if (clip == OSV_TRANSFER_PASSTHROUGH) {
            d.direct = false;
            d.rule = kRulePassthrough;
            d.why = std::string("the clip's log signal is declared as BT.2020 RGB Full (Scene), not the ") +
                    transferLabel(workingTransfer) +
                    " working space; its look here is Premiere's interpretation, for a LUT downstream";
            return d;
        }

        // ---- 5. a different graded output: OpenOSV's conversion (default) -------
        // PQ, HLG and Rec.709 encode the same scene; rendering it straight
        // into the working space is the colour-managed answer, with OpenOSV's
        // tone mapping instead of Premiere's generic one.
        if (mode == ColourMode::SequenceSpace) {
            d.direct = true;
            d.rule = kRuleSequenceSpace;
            d.why = std::string("rendering the scene straight into the ") + transferLabel(workingTransfer) +
                    " working space with OpenOSV's tone mapping; the Source monitor shows Premiere's own " +
                    transferLabel(clip) + " -> " + transferLabel(workingTransfer) + " conversion, so the two differ";
            return d;
        }

        // ---- 6. ...or Premiere's, on request ----------------------------------------
        d.direct = false;
        d.rule = kRuleMatchSource;
        d.why = std::string("Premiere converts the clip's ") + transferLabel(clip) + " output into the " +
                transferLabel(workingTransfer) +
                " working space as on the Source monitor (Program Monitor Colour = Sequence space (fast), or colour "
                "output " +
                transferLabel(workingTransfer) + ", renders straight from the fisheyes)";
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
            it->second.workingTransfer == workingTransfer && it->second.direct == decision.direct &&
            it->second.rule == decision.rule) {
            return false;  // Same generation, sequence space, rule and verdict: already logged.
        }
        if (it == m.byPath.end() && m.byPath.size() >= kMaxRememberedFiles) {
            m.byPath.clear();
        }
        Remembered& r = m.byPath[path];
        r.generation = generation;
        r.workingTransfer = workingTransfer;
        r.direct = decision.direct;
        r.rule = decision.rule ? decision.rule : "";
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
        // The route, then the rule that chose it - the part a field log is
        // read for.
        const std::string route = std::string(decision.direct ? "straight from the fisheyes" : "equirect route") +
                                  " (rule: " + (decision.rule ? decision.rule : "") + "): ";
        if (!blockUsable(settings) || settings.generation == 0) {
            return "reframe/direct: '" + name + "' Source Settings unknown -> " + route + decision.why;
        }
        char head[320] = {};
        std::snprintf(head, sizeof(head),
                      "Source Settings generation %u (colour %s, exposure %+.2f stops, fit %u, calibration %u, "
                      "stabilisation %u, Program Monitor Colour %s; file %08llx:%016llx)",
                      static_cast<unsigned>(settings.generation), transferLabel(settings.clipTransfer),
                      static_cast<double>(settings.exposureStops), static_cast<unsigned>(settings.dlogmFit),
                      static_cast<unsigned>(settings.calibration), static_cast<unsigned>(settings.stabilization),
                      colourModeLabel(colourModeFor(settings)), static_cast<unsigned long long>(settings.fileVolume),
                      static_cast<unsigned long long>(settings.fileIndex));
        return "reframe/direct: '" + name + "' " + head + " in a " + transferLabel(workingTransfer) + " sequence -> " +
               route + decision.why;
    } catch (...) {
        return "reframe/direct: (decision description unavailable)";
    }
}

}  // namespace osv::reframe::direct
