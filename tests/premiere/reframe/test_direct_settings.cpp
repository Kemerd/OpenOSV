// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_direct_settings.cpp - [WP-SETTINGS] the rules that decide which clips
// the direct path renders (plugins/reframe/DirectPathSettings.h), compiled
// from the same source the .aex contains.
//
// The direct path renders from the fisheyes straight into the sequence's
// working space.  By default ("Program Monitor Colour: Sequence space
// (fast)") it does so for every graded colour output - identical to the
// Source monitor route where the colour output IS the working space, and
// with OpenOSV's own tone mapping where it is not.  "Match Source monitor"
// hands the second group to Premiere's own conversion instead.  Unknown
// settings and the D-Log M passthrough always take the equirect route.
// These tests pin every combination under both modes, the rule each verdict
// names, the environment override, the "not a failure" contract with
// GpuFilter and the once-per-change log.

#include "DirectPathSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using osv::reframe::direct::ColourMode;
using osv::reframe::direct::colourModeFor;
using osv::reframe::direct::colourModeLabel;
using osv::reframe::direct::colourModeOverride;
using osv::reframe::direct::decideSettings;
using osv::reframe::direct::describeDecision;
using osv::reframe::direct::isPolicyFallback;
using osv::reframe::direct::kPolicyReasonPrefix;
using osv::reframe::direct::noteDecision;
using osv::reframe::direct::resetDecisionMemory;
using osv::reframe::direct::SettingsDecision;
using osv::reframe::direct::transferForSeiCodes;

namespace {

/// A settings block as the engine fills it.
[[nodiscard]] OsvEngineClipSettings settingsFor(int clipTransfer, std::uint32_t generation = 3,
                                                float exposure = -1.0f) {
    OsvEngineClipSettings s{};
    s.structSize = sizeof(s);
    s.generation = generation;
    s.clipTransfer = clipTransfer;
    s.exposureStops = exposure;
    s.fileVolume = 0x1234u;
    s.fileIndex = 0x56789u;
    return s;
}

/// Set (or with nullptr clear) OSV_DIRECT_COLOR for the rest of the scope,
/// restoring what was there before.
class ScopedColourEnv {
public:
    explicit ScopedColourEnv(const wchar_t* value) {
        wchar_t old[64] = {};
        const DWORD n = GetEnvironmentVariableW(L"OSV_DIRECT_COLOR", old, static_cast<DWORD>(std::size(old)));
        m_had = n > 0 && n < std::size(old);
        if (m_had) {
            m_old = old;
        }
        SetEnvironmentVariableW(L"OSV_DIRECT_COLOR", value);
    }
    ~ScopedColourEnv() { SetEnvironmentVariableW(L"OSV_DIRECT_COLOR", m_had ? m_old.c_str() : nullptr); }
    ScopedColourEnv(const ScopedColourEnv&) = delete;
    ScopedColourEnv& operator=(const ScopedColourEnv&) = delete;

private:
    bool m_had = false;
    std::wstring m_old;
};

/// The graded colour outputs and the working spaces the path produces.
constexpr int kGraded[] = {OSV_TRANSFER_PQ, OSV_TRANSFER_HLG, OSV_TRANSFER_REC709};

}  // namespace

TEST_CASE("by default every graded colour output is rendered straight into the working space",
          "[reframe][direct][settings]") {
    // "Sequence space (fast)": the user's everyday setup - PQ clips in a
    // Rec.709 sequence - stays on the direct path.
    for (const int clip : kGraded) {
        for (const int space : kGraded) {
            INFO("clip " << clip << ", working " << space);
            const SettingsDecision d = decideSettings(settingsFor(clip), space, ColourMode::SequenceSpace);
            CHECK(d.direct);
            CHECK_FALSE(d.why.empty());
            if (clip == space) {
                // Identical to the Source monitor route, and it says so.
                CHECK(std::string(d.rule) == "same space");
                CHECK(d.why.find("identical to the Source monitor route") != std::string::npos);
            } else {
                // OpenOSV's conversion, and the line says the monitors differ.
                CHECK(std::string(d.rule) == "Sequence space (fast)");
                CHECK(d.why.find("so the two differ") != std::string::npos);
            }
        }
    }
}

TEST_CASE("Match Source monitor hands every other colour output to Premiere's conversion",
          "[reframe][direct][settings]") {
    for (const int clip : kGraded) {
        for (const int space : kGraded) {
            INFO("clip " << clip << ", working " << space);
            const SettingsDecision d = decideSettings(settingsFor(clip), space, ColourMode::MatchSource);
            CHECK(d.direct == (clip == space));
            CHECK(std::string(d.rule) == (clip == space ? "same space" : "Match Source monitor"));
            if (clip != space) {
                // The hand-over tells the user both ways back to the fast route.
                CHECK(d.why.find("Program Monitor Colour = Sequence space (fast)") != std::string::npos);
            }
        }
    }
}

TEST_CASE("the D-Log M passthrough takes the equirect route in every space the path produces",
          "[reframe][direct][settings]") {
    // The importer declares passthrough as "BT.2020 RGB Full (Scene)" (SEI 9 /
    // 2, PrefsMapping.cpp).  No working space the path produces is that, so
    // the look is Premiere's interpretation of the log signal - whatever the
    // Program Monitor Colour choice.
    for (const ColourMode mode : {ColourMode::SequenceSpace, ColourMode::MatchSource}) {
        for (const int space : kGraded) {
            INFO("mode " << static_cast<int>(mode) << ", working " << space);
            const SettingsDecision d = decideSettings(settingsFor(OSV_TRANSFER_PASSTHROUGH), space, mode);
            CHECK_FALSE(d.direct);
            CHECK(std::string(d.rule) == "D-Log M passthrough");
            CHECK(d.why.find("BT.2020 RGB Full (Scene)") != std::string::npos);
        }
    }
    // The passthrough declaration's SEI codes are no working space the path
    // produces - which is what keeps rule 3 ("same space") from ever meeting
    // a passthrough clip.
    CHECK(transferForSeiCodes(9, 2) == -1);
    // ...and IF a working space ever matched the clip's own encoding, the
    // generic rule would render it exactly (the engine test pins that the
    // passthrough colour block then equals the importer's byte for byte).
    const SettingsDecision exact = decideSettings(settingsFor(OSV_TRANSFER_PASSTHROUGH), OSV_TRANSFER_PASSTHROUGH,
                                                  ColourMode::MatchSource);
    CHECK(exact.direct);
    CHECK(std::string(exact.rule) == "same space");
}

TEST_CASE("the working spaces the path produces are exactly PQ, HLG and Rec.709", "[reframe][direct][settings]") {
    CHECK(transferForSeiCodes(9, 16) == OSV_TRANSFER_PQ);
    CHECK(transferForSeiCodes(9, 18) == OSV_TRANSFER_HLG);
    for (const int t : {1, 6, 14, 15}) {
        INFO("transfer " << t);
        CHECK(transferForSeiCodes(1, t) == OSV_TRANSFER_REC709);
    }
    // Anything else: not ours to produce (the equirect route keeps it).
    CHECK(transferForSeiCodes(9, 1) == -1);    // BT.2020 SDR
    CHECK(transferForSeiCodes(1, 16) == -1);   // PQ on 709 primaries
    CHECK(transferForSeiCodes(9, 8) == -1);    // linear
    CHECK(transferForSeiCodes(0, 0) == -1);
    CHECK(transferForSeiCodes(-1, -1) == -1);
}

TEST_CASE("unknown Source Settings are never rendered by the direct path", "[reframe][direct][settings]") {
    // Generation 0: no Premiere instance of the file published anything the
    // engine could apply - it would render its defaults, not the clip.
    for (const ColourMode mode : {ColourMode::SequenceSpace, ColourMode::MatchSource}) {
        const SettingsDecision d = decideSettings(settingsFor(OSV_TRANSFER_PQ, 0u), OSV_TRANSFER_PQ, mode);
        CHECK_FALSE(d.direct);
        CHECK(std::string(d.rule) == "settings unknown");
        CHECK(d.why.find("has not been told") != std::string::npos);
    }
    // A block an older engine did not fill (structSize 0) is the same thing.
    OsvEngineClipSettings unfilled{};
    unfilled.generation = 9;  // garbage past a zero structSize must not count
    CHECK_FALSE(decideSettings(unfilled, OSV_TRANSFER_PQ, ColourMode::SequenceSpace).direct);
    // A working space the direct path cannot produce is refused too.
    const SettingsDecision noSpace = decideSettings(settingsFor(OSV_TRANSFER_PQ), -1, ColourMode::SequenceSpace);
    CHECK_FALSE(noSpace.direct);
    CHECK(std::string(noSpace.rule) == "working space");
}

TEST_CASE("the colour mode comes from the clip's Source Settings unless OSV_DIRECT_COLOR overrides it",
          "[reframe][direct][settings]") {
    OsvEngineClipSettings sequence = settingsFor(OSV_TRANSFER_PQ);
    sequence.directColour = 0;  // PrefsDirectColour::SequenceSpace, the default
    OsvEngineClipSettings match = settingsFor(OSV_TRANSFER_PQ);
    match.directColour = 1;  // PrefsDirectColour::MatchSource
    OsvEngineClipSettings corrupt = settingsFor(OSV_TRANSFER_PQ);
    corrupt.directColour = 0x7F;
    OsvEngineClipSettings unfilled{};  // an engine that filled nothing
    unfilled.directColour = 1;

    {
        ScopedColourEnv env(nullptr);  // no override: the clip decides
        ColourMode ignored = ColourMode::SequenceSpace;
        CHECK_FALSE(colourModeOverride(ignored));
        CHECK(colourModeFor(sequence) == ColourMode::SequenceSpace);
        CHECK(colourModeFor(match) == ColourMode::MatchSource);
        // Anything that is not an explicit "match" is the default.
        CHECK(colourModeFor(corrupt) == ColourMode::SequenceSpace);
        CHECK(colourModeFor(unfilled) == ColourMode::SequenceSpace);
    }
    {
        ScopedColourEnv env(L"working");  // A/B: force the sequence space
        ColourMode forced = ColourMode::MatchSource;
        CHECK(colourModeOverride(forced));
        CHECK(forced == ColourMode::SequenceSpace);
        CHECK(colourModeFor(match) == ColourMode::SequenceSpace);
    }
    {
        ScopedColourEnv env(L"MATCH");  // A/B: force matching, any case
        ColourMode forced = ColourMode::SequenceSpace;
        CHECK(colourModeOverride(forced));
        CHECK(forced == ColourMode::MatchSource);
        CHECK(colourModeFor(sequence) == ColourMode::MatchSource);
    }
    {
        ScopedColourEnv env(L"a value far too long to be any mode at all");
        ColourMode untouched = ColourMode::MatchSource;
        CHECK_FALSE(colourModeOverride(untouched));
        CHECK(untouched == ColourMode::MatchSource);
        CHECK(colourModeFor(sequence) == ColourMode::SequenceSpace);
    }
    {
        ScopedColourEnv env(L"sometimes");  // not a mode: ignored
        ColourMode untouched = ColourMode::SequenceSpace;
        CHECK_FALSE(colourModeOverride(untouched));
        CHECK(colourModeFor(match) == ColourMode::MatchSource);
    }
    // The labels are the Source Settings popup's own words.
    CHECK(std::string(colourModeLabel(ColourMode::SequenceSpace)) == "Sequence space (fast)");
    CHECK(std::string(colourModeLabel(ColourMode::MatchSource)) == "Match Source monitor");
}

TEST_CASE("a hand-over by the rules is recognisable and is not a failure", "[reframe][direct][settings]") {
    const SettingsDecision d =
        decideSettings(settingsFor(OSV_TRANSFER_PQ), OSV_TRANSFER_REC709, ColourMode::MatchSource);
    REQUIRE_FALSE(d.direct);
    CHECK(isPolicyFallback(std::string(kPolicyReasonPrefix) + d.why));
    // Real failures stay failures (GpuFilter warns and may disable the
    // instance for them).
    CHECK_FALSE(isPolicyFallback("engine: cannot open 'x.OSV'"));
    CHECK_FALSE(isPolicyFallback("setup: the view"));
    CHECK_FALSE(isPolicyFallback(""));
    CHECK_FALSE(isPolicyFallback(d.why));  // without the prefix it is just text
}

TEST_CASE("each decision is reported once per change, not once per frame", "[reframe][direct][settings]") {
    resetDecisionMemory();
    const std::wstring clip = L"L:\\footage\\a.OSV";
    const std::wstring other = L"L:\\footage\\b.OSV";
    const OsvEngineClipSettings g3 = settingsFor(OSV_TRANSFER_REC709, 3);
    const SettingsDecision direct = decideSettings(g3, OSV_TRANSFER_REC709, ColourMode::SequenceSpace);
    REQUIRE(direct.direct);

    CHECK(noteDecision(clip, g3, OSV_TRANSFER_REC709, direct));        // first sight
    CHECK_FALSE(noteDecision(clip, g3, OSV_TRANSFER_REC709, direct));  // the next frame
    CHECK_FALSE(noteDecision(clip, g3, OSV_TRANSFER_REC709, direct));  // and the next

    // A Source Settings change (new generation) is reported - this is the
    // log line that proves Premiere re-rendered after the change.
    const OsvEngineClipSettings g4 = settingsFor(OSV_TRANSFER_REC709, 4, -2.0f);
    CHECK(noteDecision(clip, g4, OSV_TRANSFER_REC709, direct));
    CHECK_FALSE(noteDecision(clip, g4, OSV_TRANSFER_REC709, direct));

    // The same clip in a sequence with another working space is reported,
    // and so is a change of RULE with the same verdict (an OSV_DIRECT_COLOR
    // override switched within one generation).
    const SettingsDecision viaSequence = decideSettings(g4, OSV_TRANSFER_PQ, ColourMode::SequenceSpace);
    REQUIRE(viaSequence.direct);
    CHECK(noteDecision(clip, g4, OSV_TRANSFER_PQ, viaSequence));
    CHECK_FALSE(noteDecision(clip, g4, OSV_TRANSFER_PQ, viaSequence));
    const SettingsDecision handOver = decideSettings(g4, OSV_TRANSFER_PQ, ColourMode::MatchSource);
    REQUIRE_FALSE(handOver.direct);
    CHECK(noteDecision(clip, g4, OSV_TRANSFER_PQ, handOver));
    CHECK_FALSE(noteDecision(clip, g4, OSV_TRANSFER_PQ, handOver));

    // Another file is its own story.
    CHECK(noteDecision(other, g4, OSV_TRANSFER_PQ, handOver));
    resetDecisionMemory();
    CHECK(noteDecision(clip, g4, OSV_TRANSFER_PQ, handOver));
    resetDecisionMemory();
}

TEST_CASE("the route line names the clip, the settings, the route and the rule", "[reframe][direct][settings]") {
    // The default: a PQ clip in a Rec.709 sequence, straight from the fisheyes.
    const OsvEngineClipSettings s = settingsFor(OSV_TRANSFER_PQ, 7, -1.0f);
    const SettingsDecision d = decideSettings(s, OSV_TRANSFER_REC709, colourModeFor(s));
    const std::string line = describeDecision(L"L:\\Dev\\clips\\example_footage_dlogm.OSV", s, OSV_TRANSFER_REC709, d);
    CHECK(line.find("'example_footage_dlogm.OSV'") != std::string::npos);
    CHECK(line.find("generation 7") != std::string::npos);
    CHECK(line.find("colour PQ") != std::string::npos);
    CHECK(line.find("exposure -1.00") != std::string::npos);
    CHECK(line.find("Program Monitor Colour Sequence space (fast)") != std::string::npos);
    CHECK(line.find("in a Rec.709 sequence") != std::string::npos);
    CHECK(line.find("-> straight from the fisheyes (rule: Sequence space (fast))") != std::string::npos);
    CHECK(line.find("file 00001234:0000000000056789") != std::string::npos);

    // The opt-in hands it over, and says so.
    OsvEngineClipSettings m = s;
    m.directColour = 1;
    const std::string handed =
        describeDecision(L"C:/x/y.OSV", m, OSV_TRANSFER_REC709, decideSettings(m, OSV_TRANSFER_REC709, colourModeFor(m)));
    CHECK(handed.find("'y.OSV'") != std::string::npos);
    CHECK(handed.find("Program Monitor Colour Match Source monitor") != std::string::npos);
    CHECK(handed.find("-> equirect route (rule: Match Source monitor)") != std::string::npos);

    // Same space.
    const OsvEngineClipSettings same = settingsFor(OSV_TRANSFER_REC709, 8, 0.5f);
    const std::string exact = describeDecision(L"C:/x/y.OSV", same, OSV_TRANSFER_REC709,
                                               decideSettings(same, OSV_TRANSFER_REC709, colourModeFor(same)));
    CHECK(exact.find("-> straight from the fisheyes (rule: same space)") != std::string::npos);

    // Unknown.
    const std::string unknown =
        describeDecision(L"z.OSV", settingsFor(OSV_TRANSFER_PQ, 0u), OSV_TRANSFER_PQ,
                         decideSettings(settingsFor(OSV_TRANSFER_PQ, 0u), OSV_TRANSFER_PQ, ColourMode::SequenceSpace));
    CHECK(unknown.find("Source Settings unknown -> equirect route (rule: settings unknown)") != std::string::npos);
}
