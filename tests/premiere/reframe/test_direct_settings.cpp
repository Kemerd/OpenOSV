// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_direct_settings.cpp - [WP-SETTINGS] the rule that decides which clips
// the direct path renders (plugins/reframe/DirectPathSettings.h), compiled
// from the same source the .aex contains.
//
// The rule is what keeps the Program monitor honest: the direct path renders
// from the fisheyes straight into the sequence's working space, so it may
// only do so where the result is exactly what the importer's equirect route
// would show - the clip's Source Settings known, and its colour output equal
// to the working space.  Every other combination is Premiere's own
// conversion's job.  These tests pin every combination, the escape hatch,
// the "not a failure" contract with GpuFilter and the once-per-change log.

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
using osv::reframe::direct::colourModeOverride;
using osv::reframe::direct::decideSettings;
using osv::reframe::direct::describeDecision;
using osv::reframe::direct::isPolicyFallback;
using osv::reframe::direct::kPolicyReasonPrefix;
using osv::reframe::direct::noteDecision;
using osv::reframe::direct::resetDecisionMemory;
using osv::reframe::direct::SettingsDecision;

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

}  // namespace

TEST_CASE("the direct path renders a clip exactly when its colour output is the working space",
          "[reframe][direct][settings]") {
    const int outputs[] = {OSV_TRANSFER_PQ, OSV_TRANSFER_HLG, OSV_TRANSFER_REC709, OSV_TRANSFER_PASSTHROUGH};
    const int working[] = {OSV_TRANSFER_PQ, OSV_TRANSFER_HLG, OSV_TRANSFER_REC709};
    for (const int clip : outputs) {
        for (const int space : working) {
            INFO("clip " << clip << ", working " << space);
            const SettingsDecision d = decideSettings(settingsFor(clip), space, ColourMode::MatchClip);
            const bool expected = (clip == space);
            CHECK(d.direct == expected);
            CHECK_FALSE(d.why.empty());
            if (!expected) {
                // A hand-over tells the user how to get the direct path back
                // (or why there is none for the passthrough).
                if (clip == OSV_TRANSFER_PASSTHROUGH) {
                    CHECK(d.why.find("D-Log M passthrough") != std::string::npos);
                } else {
                    CHECK(d.why.find("set Source Settings > colour output to") != std::string::npos);
                }
            }
        }
    }
}

TEST_CASE("unknown Source Settings are never rendered by the direct path", "[reframe][direct][settings]") {
    // Generation 0: no Premiere instance of the file published anything the
    // engine could apply - it would render its defaults, not the clip.
    for (const ColourMode mode : {ColourMode::MatchClip, ColourMode::FollowWorkingSpace}) {
        const SettingsDecision d = decideSettings(settingsFor(OSV_TRANSFER_PQ, 0u), OSV_TRANSFER_PQ, mode);
        CHECK_FALSE(d.direct);
        CHECK(d.why.find("has not been told") != std::string::npos);
    }
    // A block an older engine did not fill (structSize 0) is the same thing.
    OsvEngineClipSettings unfilled{};
    unfilled.generation = 9;  // garbage past a zero structSize must not count
    CHECK_FALSE(decideSettings(unfilled, OSV_TRANSFER_PQ, ColourMode::MatchClip).direct);
    // A working space the direct path cannot produce is refused too.
    CHECK_FALSE(decideSettings(settingsFor(OSV_TRANSFER_PQ), -1, ColourMode::MatchClip).direct);
}

TEST_CASE("Direct Path Colour = working space renders every known, graded clip in the working space",
          "[reframe][direct][settings]") {
    // The per-clip opt-in: the behaviour before the rule, for users who
    // prefer the direct path's sharpness over matching the Source monitor.
    CHECK(decideSettings(settingsFor(OSV_TRANSFER_PQ), OSV_TRANSFER_REC709, ColourMode::FollowWorkingSpace).direct);
    CHECK(decideSettings(settingsFor(OSV_TRANSFER_REC709), OSV_TRANSFER_PQ, ColourMode::FollowWorkingSpace).direct);
    CHECK(decideSettings(settingsFor(OSV_TRANSFER_HLG), OSV_TRANSFER_PQ, ColourMode::FollowWorkingSpace).direct);
    // ...but never the log passthrough, whose whole point is to NOT be graded.
    CHECK_FALSE(
        decideSettings(settingsFor(OSV_TRANSFER_PASSTHROUGH), OSV_TRANSFER_PQ, ColourMode::FollowWorkingSpace).direct);
}

TEST_CASE("the colour mode comes from the clip's Source Settings unless OSV_DIRECT_COLOR overrides it",
          "[reframe][direct][settings]") {
    OsvEngineClipSettings match = settingsFor(OSV_TRANSFER_PQ);
    match.directColour = 0;  // PrefsDirectColour::MatchClip
    OsvEngineClipSettings working = settingsFor(OSV_TRANSFER_PQ);
    working.directColour = 1;  // PrefsDirectColour::WorkingSpace
    OsvEngineClipSettings corrupt = settingsFor(OSV_TRANSFER_PQ);
    corrupt.directColour = 0x7F;
    OsvEngineClipSettings unfilled{};  // an engine that filled nothing
    unfilled.directColour = 1;

    {
        ScopedColourEnv env(nullptr);  // no override: the clip decides
        ColourMode ignored = ColourMode::MatchClip;
        CHECK_FALSE(colourModeOverride(ignored));
        CHECK(colourModeFor(match) == ColourMode::MatchClip);
        CHECK(colourModeFor(working) == ColourMode::FollowWorkingSpace);
        CHECK(colourModeFor(corrupt) == ColourMode::MatchClip);
        CHECK(colourModeFor(unfilled) == ColourMode::MatchClip);
    }
    {
        ScopedColourEnv env(L"working");  // A/B: force the working space
        ColourMode forced = ColourMode::MatchClip;
        CHECK(colourModeOverride(forced));
        CHECK(forced == ColourMode::FollowWorkingSpace);
        CHECK(colourModeFor(match) == ColourMode::FollowWorkingSpace);
    }
    {
        ScopedColourEnv env(L"MATCH");  // A/B: force matching, any case
        ColourMode forced = ColourMode::FollowWorkingSpace;
        CHECK(colourModeOverride(forced));
        CHECK(forced == ColourMode::MatchClip);
        CHECK(colourModeFor(working) == ColourMode::MatchClip);
    }
    {
        ScopedColourEnv env(L"a value far too long to be any mode at all");
        ColourMode untouched = ColourMode::FollowWorkingSpace;
        CHECK_FALSE(colourModeOverride(untouched));
        CHECK(untouched == ColourMode::FollowWorkingSpace);
        CHECK(colourModeFor(match) == ColourMode::MatchClip);
    }
    {
        ScopedColourEnv env(L"sometimes");  // not a mode: ignored
        ColourMode untouched = ColourMode::MatchClip;
        CHECK_FALSE(colourModeOverride(untouched));
        CHECK(colourModeFor(working) == ColourMode::FollowWorkingSpace);
    }
}

TEST_CASE("a hand-over by the rule is recognisable and is not a failure", "[reframe][direct][settings]") {
    const SettingsDecision d = decideSettings(settingsFor(OSV_TRANSFER_PQ), OSV_TRANSFER_REC709, ColourMode::MatchClip);
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
    const SettingsDecision direct = decideSettings(g3, OSV_TRANSFER_REC709, ColourMode::MatchClip);
    REQUIRE(direct.direct);

    CHECK(noteDecision(clip, g3, OSV_TRANSFER_REC709, direct));        // first sight
    CHECK_FALSE(noteDecision(clip, g3, OSV_TRANSFER_REC709, direct));  // the next frame
    CHECK_FALSE(noteDecision(clip, g3, OSV_TRANSFER_REC709, direct));  // and the next

    // A Source Settings change (new generation) is reported - this is the
    // log line that proves Premiere re-rendered after the change.
    const OsvEngineClipSettings g4 = settingsFor(OSV_TRANSFER_REC709, 4, -2.0f);
    CHECK(noteDecision(clip, g4, OSV_TRANSFER_REC709, direct));
    CHECK_FALSE(noteDecision(clip, g4, OSV_TRANSFER_REC709, direct));

    // The same clip in a sequence with another working space is reported.
    const SettingsDecision handOver = decideSettings(g4, OSV_TRANSFER_PQ, ColourMode::MatchClip);
    REQUIRE_FALSE(handOver.direct);
    CHECK(noteDecision(clip, g4, OSV_TRANSFER_PQ, handOver));
    CHECK_FALSE(noteDecision(clip, g4, OSV_TRANSFER_PQ, handOver));

    // Another file is its own story.
    CHECK(noteDecision(other, g4, OSV_TRANSFER_PQ, handOver));
    resetDecisionMemory();
    CHECK(noteDecision(clip, g4, OSV_TRANSFER_PQ, handOver));
    resetDecisionMemory();
}

TEST_CASE("the decision log line names the clip, the settings and the verdict", "[reframe][direct][settings]") {
    const OsvEngineClipSettings s = settingsFor(OSV_TRANSFER_PQ, 7, -1.0f);
    const SettingsDecision d = decideSettings(s, OSV_TRANSFER_REC709, ColourMode::MatchClip);
    const std::string line = describeDecision(L"L:\\Dev\\clips\\example_footage_dlogm.OSV", s, OSV_TRANSFER_REC709, d);
    CHECK(line.find("'example_footage_dlogm.OSV'") != std::string::npos);
    CHECK(line.find("generation 7") != std::string::npos);
    CHECK(line.find("colour PQ") != std::string::npos);
    CHECK(line.find("exposure -1.00") != std::string::npos);
    CHECK(line.find("in a Rec.709 sequence") != std::string::npos);
    CHECK(line.find("-> equirect route") != std::string::npos);
    CHECK(line.find("file 00001234:0000000000056789") != std::string::npos);

    const OsvEngineClipSettings same = settingsFor(OSV_TRANSFER_REC709, 8, 0.5f);
    const SettingsDecision direct = decideSettings(same, OSV_TRANSFER_REC709, ColourMode::MatchClip);
    const std::string ok = describeDecision(L"C:/x/y.OSV", same, OSV_TRANSFER_REC709, direct);
    CHECK(ok.find("'y.OSV'") != std::string::npos);
    CHECK(ok.find("-> straight from the fisheyes") != std::string::npos);

    const std::string unknown =
        describeDecision(L"z.OSV", settingsFor(OSV_TRANSFER_PQ, 0u), OSV_TRANSFER_PQ,
                         decideSettings(settingsFor(OSV_TRANSFER_PQ, 0u), OSV_TRANSFER_PQ, ColourMode::MatchClip));
    CHECK(unknown.find("Source Settings unknown -> equirect route") != std::string::npos);
}
