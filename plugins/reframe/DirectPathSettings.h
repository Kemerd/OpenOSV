// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectPathSettings.h - which clips the direct path renders, given the
// clip's Source Settings and the sequence's working colour space.
//
// THE QUESTION THIS ANSWERS
// -------------------------
// On the equirect route the importer encodes its frame in the clip's own
// "colour output" (PQ, HLG, Rec.709 or the D-Log M passthrough) and Premiere
// converts that into the sequence's working colour space before the effect
// sees it - which is also what the Source monitor shows.  The direct path
// renders from the FISHEYES instead, straight into the working space, so
// Premiere's conversion never runs:
//
//   * colour output == working space: that conversion is the identity and the
//     two routes agree exactly (the engine builds the very colour block the
//     importer uses; the engine tests pin it byte for byte);
//   * a DIFFERENT graded output (PQ / HLG / Rec.709): all three are encodings
//     of the same scene, so rendering the scene straight into the working
//     space with OpenOSV's own tone mapping is the colour-managed answer.  It
//     is not Premiere's generic conversion (HDR -> SDR tone mapping with the
//     clip's gamut controls, SDR at graphics white in an HDR container), so
//     the Program monitor then differs from the Source monitor - by design,
//     and in a Rec.709 sequence it is the DJI-matched look the project aims
//     for rather than Premiere's generic one;
//   * the D-Log M passthrough is a LOG signal for a downstream LUT, declared
//     to Premiere as "BT.2020 RGB Full (Scene)" (PrefsMapping.cpp).  The
//     direct path could only reproduce it exactly in a working space equal to
//     that declaration, and none of the spaces it produces (PQ, HLG, Rec.709)
//     is - so passthrough always takes the equirect route.
//
// THE RULES (docs/DIRECT_GPU.md, "WP-SETTINGS"), in order
// -------------------------------------------------------
//   1. "settings unknown"        nothing published for the file   -> equirect
//   2. "working space"           a space the path cannot produce  -> equirect
//   3. "same space"              colour output == working space   -> direct
//   4. "D-Log M passthrough"     log signal for a LUT             -> equirect
//   5. "Sequence space (fast)"   the default for a different graded
//                                output: OpenOSV's conversion     -> direct
//   6. "Match Source monitor"    the per-clip opt-in: Premiere's
//                                conversion of the importer frame -> equirect
//
// Rules 5 / 6 are the clip's Source Setting "Program Monitor Colour"
// (PrefsBlob::directColour, 0 = Sequence space).  OSV_DIRECT_COLOR=working /
// =match overrides that per-clip choice for the whole process, for A/B
// comparisons.  Everything here is pure (no host, no GPU) so the tests can
// pin every combination.
#pragma once

#include "OsvEngineAbi.h"

#include <cstdint>
#include <string>

namespace osv::reframe::direct {

/// "Program Monitor Colour": how the direct path treats a clip whose colour
/// output is not the sequence's working space (the clip's PrefsDirectColour).
enum class ColourMode : int {
    /// Render straight into the working space with OpenOSV's own conversion:
    /// the direct path's speed and sharpness.  The default
    /// (PrefsDirectColour::SequenceSpace == 0).
    SequenceSpace = 0,
    /// Hand the clip to the equirect route, so the Program monitor shows
    /// exactly what Premiere's own conversion makes of the importer's frame,
    /// as the Source monitor does (PrefsDirectColour::MatchSource == 1).
    MatchSource = 1,
};

/// The process-wide override, if any: OSV_DIRECT_COLOR=working selects
/// SequenceSpace and OSV_DIRECT_COLOR=match selects MatchSource (either
/// case); unset or anything else returns false and leaves `out` alone.  Read
/// on every call (a few microseconds), so tests and A/B sessions can switch.
[[nodiscard]] bool colourModeOverride(ColourMode& out) noexcept;

/// The mode for one clip: the override when set, otherwise the clip's own
/// Source Settings choice (settings.directColour 1 = MatchSource; 0, an
/// unknown value or a block too small to hold it = SequenceSpace).
[[nodiscard]] ColourMode colourModeFor(const OsvEngineClipSettings& settings) noexcept;

/// The Source Settings label of a mode ("Sequence space (fast)" /
/// "Match Source monitor"), for log lines.
[[nodiscard]] const char* colourModeLabel(ColourMode mode) noexcept;

/// Short name of an OSV_TRANSFER_* id for log lines ("PQ", "HLG",
/// "Rec.709", "linear", "D-Log M passthrough", "unknown").
[[nodiscard]] const char* transferLabel(int transfer) noexcept;

/// The OSV_TRANSFER_* id of a working colour space given by its H.273 codes
/// (the SEI description the Colour Management Suite reports), or -1 when the
/// direct path cannot produce it: PQ and HLG on BT.2020 primaries (9/16,
/// 9/18) and BT.709 (primaries 1 with transfer 1, 6, 14 or 15 - one curve).
/// Never OSV_TRANSFER_PASSTHROUGH: no working space is a camera log signal.
[[nodiscard]] int transferForSeiCodes(int primaries, int transfer) noexcept;

/// The verdict for one clip in one sequence.
struct SettingsDecision {
    bool direct = false;          ///< True: render straight from the fisheyes.
    const char* rule = "";        ///< Which rule decided (static string, see the file header).
    std::string why;              ///< One line for the log, either way.
};

/// Decide whether the direct path renders a clip.
///
/// @param settings          what the engine reports for the clip (a
///                          structSize too small for the block is treated
///                          as "nothing known").
/// @param workingTransfer   OSV_TRANSFER_* of the sequence's working space
///                          (DirectPath's workingTransfer(); negative when
///                          the direct path cannot produce the space).
/// @param mode              SequenceSpace or MatchSource (normally
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
/// space, the rule or the verdict changed since the last one.  Thread-safe;
/// the caller logs exactly when this returns true, which puts one line in the
/// log per change instead of one per frame (Premiere renders ~60 frames and
/// creates ~100 GPU instances a second while a control is dragged).
[[nodiscard]] bool noteDecision(const std::wstring& path, const OsvEngineClipSettings& settings, int workingTransfer,
                                const SettingsDecision& decision) noexcept;

/// Forget every remembered decision (tests only).
void resetDecisionMemory() noexcept;

/// One log line describing a decision: the file, the settings generation and
/// what it holds, the working space, the route, the rule and its reason.
[[nodiscard]] std::string describeDecision(const std::wstring& path, const OsvEngineClipSettings& settings,
                                           int workingTransfer, const SettingsDecision& decision) noexcept;

}  // namespace osv::reframe::direct
