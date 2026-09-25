// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_defaults.cpp - [WP-DEFAULTS] the Source Settings effect's Defaults
// group and the seeding of a new clip from the user's saved defaults, driven
// through the BUILT OpenOSVSourceSettings.aex.
//
// The user's story, step by step, is what these tests walk:
//
//   1. the Effect Controls panel has a collapsed "Defaults" group at the end
//      with two buttons;
//   2. "Save as Default for New Clips" writes THIS clip's settings - exactly
//      what the clip is decoded with - to the defaults file, and changes
//      nothing about the clip itself;
//   3. a newly imported clip (PF_Cmd_SEQUENCE_SETUP on untouched controls)
//      starts from those settings;
//   4. a clip that already has settings keeps them - whether its controls
//      say so, or the importer answers with its stored blob;
//   5. "Restore Built-in Defaults" removes the file, and new clips start
//      from the built-in settings again.
//
// Every test runs against a private defaults file (ScopedUserDefaultsFile),
// never the user's real %APPDATA%\OpenOSV\defaults.json.

#include "SourceSettingsTestSupport.h"

#include "SourceSettingsMapping.h"
#include "SourceSettingsParams.h"

#include "PrefsBlob.h"
#include "TestLogIsolation.h"
#include "UserDefaults.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::sourcesettings;
using namespace osv::premiere::sourcesettings::test;
using osv::premiere::testsupport::ScopedUserDefaultsFile;

namespace {

/// Click one of the Defaults buttons: PF_Cmd_USER_CHANGED_PARAM with the
/// button's index, exactly as the host sends it for a supervised parameter.
PF_Err click(EffectFixture& fixture, int aeIndex, PF_OutData* out = nullptr) {
    PF_UserChangedParamExtra extra{};
    extra.param_index = aeIndex;
    return fixture.send(PF_Cmd_USER_CHANGED_PARAM, &extra, out);
}

/// The blob the effect's controls translate to right now.
[[nodiscard]] PrefsBlob translated(EffectFixture& fixture) {
    std::vector<char> bytes(PrefsBlob::kSize, 0);
    PF_TranslateParamsToPrefsExtra extra{};
    extra.prefsPC = reinterpret_cast<PF_ImporterPrefsDataPtr>(bytes.data());
    extra.prefs_sizeLu = static_cast<A_u_long>(PrefsBlob::kSize);
    REQUIRE(fixture.send(PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, &extra) == PF_Err_NONE);
    PrefsBlob blob{};
    std::memcpy(&blob, bytes.data(), PrefsBlob::kSize);
    return blob;
}

/// A set of user defaults nothing like the built-in ones, so a control that
/// did not follow them is unmistakable.
[[nodiscard]] PrefsBlob someUserDefaults() {
    PrefsBlob p = PrefsBlob::defaults();
    p.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    p.look = static_cast<std::uint8_t>(PrefsLook::Standard);
    p.hdrPeak = static_cast<std::uint8_t>(PrefsHdrPeak::Nits600);  // [WP-HDRPEAK]
    p.hdrTone = static_cast<std::uint8_t>(PrefsHdrTone::Aces2Detailed);  // [WP-HDRTONE]
    p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::QHD2560);
    p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Full);
    p.gainMatch = 0;
    p.setCalibrationChoice(PrefsCalibrationChoice::LensGuards);
    p.photoSeam = static_cast<std::uint8_t>(PrefsPhotoSeam::Off);
    p.setSeamInsetDeg(1.5);
    p.exposureStops = 0.7f;
    p.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Cpu);
    REQUIRE(p.sanitise());
    return p;
}

/// The blob the controls express: what a user sees in the panel.
[[nodiscard]] PrefsBlob shown(EffectFixture& fixture) {
    ControlValues c;
    c.colorOutput = fixture.popup(kIndexColorOutput);
    c.rec709Look = fixture.popup(kIndexRec709Look);
    c.hdrPeak = fixture.popup(kIndexHdrPeak);  // [WP-HDRPEAK]
    c.hdrTone = fixture.popup(kIndexHdrTone);  // [WP-HDRTONE]
    c.outputSize = fixture.popup(kIndexOutputSize);
    c.stabilization = fixture.popup(kIndexStabilization);
    c.seamSearch = fixture.checkbox(kIndexSeamSearch);
    c.gainMatch = fixture.checkbox(kIndexGainMatch);
    c.calibration = fixture.popup(kIndexCalibration);
    c.flareRemoval = fixture.checkbox(kIndexFlareRemoval);
    c.photoSeam = fixture.popup(kIndexPhotoSeam);
    c.photoStrengthPercent = fixture.slider(kIndexPhotoStrength);
    c.seamInsetDeg = fixture.slider(kIndexSeamInset);
    c.dlogmFit = fixture.popup(kIndexDlogmFit);
    c.exposureStops = fixture.slider(kIndexExposure);
    c.renderDevice = fixture.popup(kIndexRenderDevice);
    c.directColour = fixture.popup(kIndexDirectColour);
    return prefsFromControls(c);
}

}  // namespace

// ===========================================================================
//  The Defaults group
// ===========================================================================

TEST_CASE("the Defaults group closes the parameter list with two buttons", "[sourcesettings][defaults]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = fixture.host().addedParams(fixture.ref());
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    // A collapsed group, after Advanced, holding the two buttons.
    const PF_ParamDef& topic = params[kIndexDefaultsTopic - 1];
    CHECK(topic.param_type == PF_Param_GROUP_START);
    CHECK(std::string(topic.PF_DEF_NAME) == "Defaults");
    CHECK((topic.flags & PF_ParamFlag_START_COLLAPSED) != 0);
    CHECK(topic.uu.id == OSV_SS_ID_DEFAULTS_TOPIC);
    CHECK(params[kIndexDefaultsTopicEnd - 1].param_type == PF_Param_GROUP_END);
    CHECK(kIndexDefaultsTopicEnd == OSV_SOURCE_SETTINGS_PARAM_COUNT);

    // Momentary buttons with plain labels.  PF_ParamFlag_SUPERVISE is what
    // makes the host send PF_Cmd_USER_CHANGED_PARAM on a click - without it
    // the buttons would be dead.
    const std::pair<int, const char*> buttons[] = {
        {kIndexSaveDefaults, "Save as Default for New Clips"},
        {kIndexRestoreDefaults, "Restore Built-in Defaults"},
    };
    for (const auto& [index, label] : buttons) {
        INFO("index " << index);
        const PF_ParamDef& def = params[static_cast<std::size_t>(index) - 1u];
        CHECK(def.param_type == PF_Param_BUTTON);
        CHECK((def.flags & PF_ParamFlag_SUPERVISE) != 0);
        REQUIRE(def.u.button_d.u.namesptr != nullptr);
        CHECK(std::string(def.u.button_d.u.namesptr) == label);
    }
    CHECK(params[kIndexSaveDefaults - 1].uu.id == OSV_SS_ID_SAVE_DEFAULTS);
    CHECK(params[kIndexRestoreDefaults - 1].uu.id == OSV_SS_ID_RESTORE_DEFAULTS);
}

TEST_CASE("Save as Default for New Clips stores exactly what this clip is decoded with",
          "[sourcesettings][defaults]") {
    ScopedUserDefaultsFile scoped;
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // A clip set up the way a user likes it.
    fixture.setPopup(kIndexColorOutput, static_cast<int>(PrefsColorOutput::HLG) + 1);
    fixture.setPopup(kIndexOutputSize, static_cast<int>(PrefsOutputSize::HD2K) + 1);
    fixture.setPopup(kIndexStabilization, static_cast<int>(PrefsStabilization::Off) + 1);
    fixture.setCheckbox(kIndexSeamSearch, false);
    fixture.setPopup(kIndexCalibration, OSV_SS_CALIB_COUNT);  // Native (bare lenses)
    fixture.setSlider(kIndexPhotoStrength, 40.0);
    fixture.setSlider(kIndexExposure, -1.2);
    fixture.setPopup(kIndexDirectColour, 2);
    const PrefsBlob clip = translated(fixture);

    PF_OutData out = fixture.host().makeOutData();
    REQUIRE(click(fixture, kIndexSaveDefaults, &out) == PF_Err_NONE);

    // The file holds the clip's translated blob, byte for byte.
    const auto saved = readUserDefaultsFile(scoped.path());
    REQUIRE(saved.ok());
    CHECK(saved.value().prefs == clip);
    CHECK(saved.value().prefs.calibrationChoice() == PrefsCalibrationChoice::Native);

    // A default is about the NEXT clip: this one is unchanged.
    CHECK(translated(fixture) == clip);

    // Premiere gets its confirmation in the log, not a modal alert (Adobe's
    // Paramarama sample raises PF_OutFlag_DISPLAY_ERROR_MESSAGE outside
    // Premiere only); the sentence is still there for a host that shows it.
    CHECK((out.out_flags & PF_OutFlag_DISPLAY_ERROR_MESSAGE) == 0);
    CHECK(std::string(out.return_msg).find("Saved") != std::string::npos);
}

TEST_CASE("Restore Built-in Defaults removes the file and leaves the clip alone", "[sourcesettings][defaults]") {
    ScopedUserDefaultsFile scoped;
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    fixture.setPopup(kIndexColorOutput, static_cast<int>(PrefsColorOutput::DLogM) + 1);
    REQUIRE(click(fixture, kIndexSaveDefaults) == PF_Err_NONE);
    REQUIRE(std::filesystem::exists(scoped.path()));
    const PrefsBlob clip = translated(fixture);

    PF_OutData out = fixture.host().makeOutData();
    REQUIRE(click(fixture, kIndexRestoreDefaults, &out) == PF_Err_NONE);
    CHECK_FALSE(std::filesystem::exists(scoped.path()));
    CHECK_FALSE(currentUserDefaults().fromFile);
    CHECK(translated(fixture) == clip);
    CHECK(std::string(out.return_msg).find("Restored") != std::string::npos);

    // Twice is harmless: the outcome ("no user defaults") already holds.
    CHECK(click(fixture, kIndexRestoreDefaults) == PF_Err_NONE);
}

TEST_CASE("outside Premiere the confirmation is raised as a message", "[sourcesettings][defaults]") {
    ScopedUserDefaultsFile scoped;
    EffectFixture fixture('FXTC');
    REQUIRE(LoadedPlugin::instance().ok());
    PF_OutData out = fixture.host().makeOutData();
    REQUIRE(click(fixture, kIndexSaveDefaults, &out) == PF_Err_NONE);
    CHECK((out.out_flags & PF_OutFlag_DISPLAY_ERROR_MESSAGE) != 0);
    CHECK(std::filesystem::exists(scoped.path()));
}

TEST_CASE("a button click with nothing to act on changes nothing", "[sourcesettings][defaults]") {
    ScopedUserDefaultsFile scoped;
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const LoadedPlugin& plugin = LoadedPlugin::instance();

    // No extra record, an index that is not a button, a Save with no
    // parameter values (the host sent none): no file, no crash, no error.
    CHECK(fixture.send(PF_Cmd_USER_CHANGED_PARAM, nullptr) == PF_Err_NONE);
    CHECK(click(fixture, kIndexColorOutput) == PF_Err_NONE);
    CHECK(click(fixture, 9999) == PF_Err_NONE);
    PF_UserChangedParamExtra extra{};
    extra.param_index = kIndexSaveDefaults;
    PF_OutData out = fixture.host().makeOutData();
    PF_InData in = fixture.host().makeInData(fixture.ref(), mock::InDataSpec{});
    CHECK(plugin.effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, nullptr, nullptr, &extra) == PF_Err_NONE);
    CHECK(plugin.effectMain()(PF_Cmd_USER_CHANGED_PARAM, nullptr, nullptr, nullptr, nullptr, &extra) ==
          PF_Err_NONE);
    CHECK_FALSE(std::filesystem::exists(scoped.path()));
}

// ===========================================================================
//  A new clip starts from the user defaults; an existing one keeps its own
// ===========================================================================

TEST_CASE("a newly applied effect starts from the user defaults", "[sourcesettings][defaults][sequence]") {
    ScopedUserDefaultsFile scoped;
    const PrefsBlob mine = someUserDefaults();
    REQUIRE(writeUserDefaultsFile(scoped.path(), mine).ok());

    // A brand-new clip: untouched controls and - the mock's default - no
    // importer instance to answer, so the request comes back as sent.
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);

    // The importer was offered the user defaults...
    const std::vector<char> sent = fixture.host().sourceSettingsSentData(fixture.ref());
    REQUIRE(sent.size() == PrefsBlob::kSize);
    PrefsBlob outbound{};
    std::memcpy(&outbound, sent.data(), PrefsBlob::kSize);
    CHECK(outbound == mine);
    // ...and the panel shows them, and the clip is decoded with them.
    CHECK(shown(fixture) == mine);
    CHECK(translated(fixture) == mine);
    CHECK(fixture.popup(kIndexColorOutput) == static_cast<int>(PrefsColorOutput::Rec709) + 1);
    CHECK(fixture.slider(kIndexExposure) == Catch::Approx(0.7));
}

TEST_CASE("a clip with its own settings is never touched by the user defaults", "[sourcesettings][defaults][sequence]") {
    ScopedUserDefaultsFile scoped;
    const PrefsBlob mine = someUserDefaults();
    REQUIRE(writeUserDefaultsFile(scoped.path(), mine).ok());

    SECTION("controls that already say something are the clip's settings") {
        EffectFixture fixture;
        REQUIRE(LoadedPlugin::instance().ok());
        fixture.setPopup(kIndexStabilization, static_cast<int>(PrefsStabilization::Smooth) + 1);
        const PrefsBlob before = translated(fixture);
        REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
        // Offered as they are, and kept as they are.
        const std::vector<char> sent = fixture.host().sourceSettingsSentData(fixture.ref());
        REQUIRE(sent.size() == PrefsBlob::kSize);
        PrefsBlob outbound{};
        std::memcpy(&outbound, sent.data(), PrefsBlob::kSize);
        CHECK(outbound == before);
        CHECK(translated(fixture) == before);
    }

    SECTION("the importer's stored blob wins over the seed") {
        EffectFixture fixture;
        REQUIRE(LoadedPlugin::instance().ok());
        // The importer holds this clip's stored settings (a live instance).
        PrefsBlob stored = PrefsBlob::defaults();
        stored.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::UHD4K);
        stored.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
        REQUIRE(stored.sanitise());
        const char* raw = reinterpret_cast<const char*>(&stored);
        fixture.host().setSourceSettingsReply(fixture.ref(), std::vector<char>(raw, raw + PrefsBlob::kSize));

        REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
        CHECK(translated(fixture) == stored);
        CHECK(shown(fixture) == stored);
    }

    SECTION("a saved project (SEQUENCE_RESETUP) is not re-seeded") {
        EffectFixture fixture;
        REQUIRE(LoadedPlugin::instance().ok());
        REQUIRE(fixture.send(PF_Cmd_SEQUENCE_RESETUP, nullptr) == PF_Err_NONE);
        CHECK(fixture.host().sourceSettingsCallCount(fixture.ref()) == 0u);
        CHECK(translated(fixture) == PrefsBlob::defaults());
    }
}

TEST_CASE("without a defaults file a new clip starts from the built-in settings", "[sourcesettings][defaults][sequence]") {
    ScopedUserDefaultsFile scoped;  // points at a file that does not exist
    {
        // Scoped: the mock host allows one instance at a time.
        EffectFixture fixture;
        REQUIRE(LoadedPlugin::instance().ok());
        REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
        CHECK(translated(fixture) == PrefsBlob::defaults());
    }

    // A corrupt file is the same as none: ignored, built-in settings.
    {
        std::filesystem::create_directories(scoped.directory());
        std::FILE* f = nullptr;
        REQUIRE(::_wfopen_s(&f, scoped.path().c_str(), L"wb") == 0);
        REQUIRE(f != nullptr);
        std::fputs("{ not json", f);
        std::fclose(f);
    }
    EffectFixture second;
    REQUIRE(second.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
    CHECK(translated(second) == PrefsBlob::defaults());
}

TEST_CASE("saved from one clip, applied to the next", "[sourcesettings][defaults][sequence]") {
    // The whole story in one test: set up clip A, save, import clip B.
    ScopedUserDefaultsFile scoped;
    PrefsBlob fromA{};
    {
        EffectFixture clipA;
        REQUIRE(LoadedPlugin::instance().ok());
        clipA.setPopup(kIndexColorOutput, static_cast<int>(PrefsColorOutput::Rec709) + 1);
        clipA.setPopup(kIndexRec709Look, static_cast<int>(PrefsLook::Standard) + 1);
        clipA.setPopup(kIndexPhotoSeam, static_cast<int>(PrefsPhotoSeam::RimOnly) + 1);
        clipA.setSlider(kIndexSeamInset, 3.4);
        clipA.setCheckbox(kIndexFlareRemoval, false);
        fromA = translated(clipA);
        REQUIRE(click(clipA, kIndexSaveDefaults) == PF_Err_NONE);
    }
    EffectFixture clipB;
    REQUIRE(clipB.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
    CHECK(translated(clipB) == fromA);
    CHECK(clipB.slider(kIndexSeamInset) == Catch::Approx(3.4));
    CHECK(clipB.checkbox(kIndexFlareRemoval) == false);
}
