// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectPathSettings.h - which clips the direct path may render, given the
// clip's Source Settings and the sequence's working colour space.
//
// THE PROBLEM THIS SOLVES
// -----------------------
// On the equirect route the importer encodes its frame in the clip's own
// "colour output" (PQ, HLG, Rec.709 or the D-Log M passthrough) and Premiere
// converts that into the sequence's working colour space before the effect
// sees it.  The direct path renders from the FISHEYES instead, straight into
// the working space, so Premiere's conversion never runs.  Whenever the two
// spaces are the same that conversion is the identity and the two routes
// agree exactly (the engine builds the very colour block the importer uses).
// Whenever they differ, the equirect route's picture is whatever Premiere's
// own conversion makes of the importer's frame - HDR -> SDR tone mapping with
// the clip's gamut-mapping controls, SDR placed in an HDR container at the
// sequence's graphics white, a log signal interpreted as scene light - none
// of which the SDK exposes or documents precisely enough to reproduce.
//
// THE RULE (docs/DIRECT_GPU.md, "WP-SETTINGS")
// --------------------------------------------
//   * the engine has not been told the clip's Source Settings   -> equirect
//   * the clip's colour output is the D-Log M passthrough        -> equirect
//   * the clip's colour output IS the working space              -> direct
//   * anything else                                              -> equirect,
//     unless the clip's Source Settings "Direct Path Colour" asks for the
//     direct path's own conversion into the working space (the behaviour
//     before this rule).  OSV_DIRECT_COLOR=working / =match overrides that
//     per-clip choice for the whole process, for A/B comparisons.
//
// So the Program monitor always shows what the Source Settings say, and the
// direct path is an invisible speed-up wherever it can be one.  The pieces
// here are pure (no host, no GPU) so the tests can pin every combination.
#pragma once

#include "OsvEngineAbi.h"

#include <cstdint>
#include <string>

namespace osv::reframe::direct {

/// How the direct path treats a clip whose colour output is not the
/// sequence's working space (the clip's PrefsDirectColour).
enum class ColourMode : int {
    /// Hand the clip to the equirect route, so Premiere's own conversion
    /// produces exactly what the Source monitor route produces.  The default.
    MatchClip = 0,
    /// Render straight into the working space with OpenOSV's own conversion
    /// (sharper and faster, but the clip's colour output choice no longer
    /// changes the picture).
    FollowWorkingSpace = 1,
};

/// The process-wide override, if any: OSV_DIRECT_COLOR=working selects
/// FollowWorkingSpace and OSV_DIRECT_COLOR=match selects MatchClip (either
/// case); unset or anything else returns false and leaves `out` alone.  Read
/// on every call (a few microseconds), so tests and A/B sessions can switch.
[[nodiscard]] bool colourModeOverride(ColourMode& out) noexcept;

/// The mode for one clip: the override when set, otherwise the clip's own
/// Source Settings choice (settings.directColour; a block too small to hold
/// it, or an unknown value, means MatchClip).
[[nodiscard]] ColourMode colourModeFor(const OsvEngineClipSettings& settings) noexcept;

/// Short name of an OSV_TRANSFER_* id for log lines ("PQ", "HLG",
/// "Rec.709", "linear", "D-Log M passthrough", "unknown").
[[nodiscard]] const char* transferLabel(int transfer) noexcept;

/// The verdict for one clip in one sequence.
struct SettingsDecision {
    bool direct = false;  ///< True: render straight from the fisheyes.
    std::string why;      ///< One line for the log, either way.
};

/// Decide whether the direct path may render a clip.
///
/// @param settings          what the engine reports for the clip (a
///                          structSize too small for the block is treated
///                          as "nothing known").
/// @param workingTransfer   OSV_TRANSFER_* of the sequence's working space
///                          (DirectPath's workingTransfer(); negative when
///                          the direct path cannot produce the space).
/// @param mode              MatchClip or FollowWorkingSpace (normally
///                          colourModeFor(settings)).
[[nodiscard]] SettingsDecision decideSettings(const OsvEngineClipSettings& settings, int workingTransfer,
                                              ColourMode mode) noexcept;

/// Prefix of every renderDirect() reason that is a deliberate hand-over to
/// the equirect route rather than a failure.  The caller must neither warn
/// about it nor stop trying the direct path: a later Source Settings change
/// can make the clip eligible.
inline constexpr const char* kPolicyReasonPrefix = "settings: ";

/// True when `reason` (from renderDirect) is such a deliberate hand-over.
[[nodiscard]] bool isPolicyFallback(const std::string& reason) noexcept;

/// Remember the decision taken for a file and report whether it is NEW -
/// the first for the file, or the Source Settings generation, the working
/// space or the verdict changed since the last one.  Thread-safe; the caller
/// logs exactly when this returns true, which puts one line in the log per
/// change instead of one per frame (Premiere renders ~60 frames and creates
/// ~100 GPU instances a second while a control is dragged).
[[nodiscard]] bool noteDecision(const std::wstring& path, const OsvEngineClipSettings& settings, int workingTransfer,
                                const SettingsDecision& decision) noexcept;

/// Forget every remembered decision (tests only).
void resetDecisionMemory() noexcept;

/// One log line describing a decision: the file, the settings generation and
/// what it holds, the working space and the verdict with its reason.
[[nodiscard]] std::string describeDecision(const std::wstring& path, const OsvEngineClipSettings& settings,
                                           int workingTransfer, const SettingsDecision& decision) noexcept;

}  // namespace osv::reframe::direct
