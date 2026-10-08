// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_compat.cpp - a project saved with an older OpenOSV Source Settings
// effect still opens with every setting it had.
//
// How Premiere restores a saved effect instance
// ---------------------------------------------
// A project stores each parameter of the master-clip effect with its
// PERMANENT id (the PF_ParamDef::uu.id given to PF_ADD_PARAM), its type and
// its value - not by its index.  A 0.5.0 project, for example, carries 36
// parameters whose ids run 1, 50, 15, 46, 2, 3, 4, 5, ... 30, 31, 32, 33.
// When the effect has since gained a control (0.5.1 added Hide Mount, id 54,
// at index 26 inside the Stitching group), Premiere matches every stored id
// to the effect's current list, so each stored value lands on the control
// that carries the same id - wherever its index now is - and the new control
// starts at its default.  Measured on the maintainer's own project: the
// 0.5.0 instance re-opened under 0.5.1 kept all 36 values and gained Hide
// Mount = 0 (the first item, On).
//
// That only works while ids are never reused, removed or re-typed.  This
// file plays that host: it takes a stored 0.5.0 instance with a non-default
// value in EVERY control, restores it by id onto the current parameter list
// and checks that the translated blob is exactly what those values mean,
// with Hide Mount On.

#include "SourceSettingsTestSupport.h"

#include "SourceSettingsMapping.h"
#include "SourceSettingsParams.h"

#include "PrefsBlob.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <iterator>
#include <set>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::sourcesettings;
using namespace osv::premiere::sourcesettings::test;

namespace {

/// One parameter of a saved instance, as a project stores it: the permanent
/// id, the type, and the value (popups 1-based, as AE hands them over).
struct StoredParam {
    int id;
    PF_ParamType type;
    int value;      ///< Popup item (1-based) or checkbox 0 / 1; unused otherwise.
    double slider;  ///< Float slider value; unused otherwise.
};

/// The 36 parameters of a 0.5.0 instance, in the order that project stores
/// them (ids copied from a 0.5.0 project), each control set to something
/// other than its default so a value landing on the wrong control shows.
constexpr StoredParam kStored050[] = {
    {1, PF_Param_POPUP, 2, 0.0},             // Colour Output: BT.2100 HLG
    {50, PF_Param_POPUP, 3, 0.0},            // Transfer Function (HDR): BT.2408 Natural
    {15, PF_Param_POPUP, 2, 0.0},            // Look: OpenOSV standard
    {46, PF_Param_POPUP, 3, 0.0},            // HDR Peak: 400 nits
    {2, PF_Param_POPUP, 4, 0.0},             // Output Size: 2K
    {3, PF_Param_POPUP, 3, 0.0},             // Stabilisation: Full
    {4, PF_Param_GROUP_START, 0, 0.0},       // Stitching
    {5, PF_Param_CHECKBOX, 0, 0.0},          // Seam Search: off
    {6, PF_Param_CHECKBOX, 0, 0.0},          // Exposure Match: off
    {7, PF_Param_POPUP, 3, 0.0},             // Calibration: Underwater
    {16, PF_Param_CHECKBOX, 0, 0.0},         // Sun Ghost Removal: off
    {17, PF_Param_POPUP, 2, 0.0},            // Sky Seam Fix: Rim only
    {18, PF_Param_FLOAT_SLIDER, 0, 60.0},    // Sky Seam Strength
    {19, PF_Param_FLOAT_SLIDER, 0, 1.5},     // Seam Edge Inset
    {20, PF_Param_FLOAT_SLIDER, 0, 2.0},     // Seam Blend
    {21, PF_Param_FLOAT_SLIDER, 0, 1.0},     // Parallax Blend
    {22, PF_Param_FLOAT_SLIDER, 0, 0.5},     // Seam Smoothing
    {23, PF_Param_FLOAT_SLIDER, 0, 0.5},     // Near Offset
    {24, PF_Param_FLOAT_SLIDER, 0, -0.5},    // Far Offset
    {34, PF_Param_POPUP, 1, 0.0},            // Lens Shading: Off
    {35, PF_Param_FLOAT_SLIDER, 0, 60.0},    // Shading Strength
    {40, PF_Param_POPUP, 2, 0.0},            // Parallax Grid: Steady
    {41, PF_Param_POPUP, 2, 0.0},            // Lens Alignment: Off
    {52, PF_Param_POPUP, 3, 0.0},            // Scene Light: Night
    {53, PF_Param_POPUP, 2, 0.0},            // Lens Focal: Camera
    {8, PF_Param_GROUP_END, 0, 0.0},         // (end of Stitching)
    {9, PF_Param_GROUP_START, 0, 0.0},       // Advanced
    {10, PF_Param_POPUP, 1, 0.0},            // D-Log M Curve: DJI Refit
    {11, PF_Param_FLOAT_SLIDER, 0, -1.0},    // Exposure
    {12, PF_Param_POPUP, 2, 0.0},            // Render Device: CPU
    {14, PF_Param_POPUP, 2, 0.0},            // Program Monitor Colour: Match Source monitor
    {13, PF_Param_GROUP_END, 0, 0.0},        // (end of Advanced)
    {30, PF_Param_GROUP_START, 0, 0.0},      // Defaults
    {31, PF_Param_BUTTON, 0, 0.0},           // Save
    {32, PF_Param_BUTTON, 0, 0.0},           // Restore
    {33, PF_Param_GROUP_END, 0, 0.0},        // (end of Defaults)
};
static_assert(std::size(kStored050) == 36, "a 0.5.0 instance stores exactly 36 parameters");

/// The 1-based AE index of the control carrying `id` in the current list,
/// or 0 when no control carries it.
[[nodiscard]] int indexOfId(const std::vector<PF_ParamDef>& added, int id) {
    for (std::size_t i = 0; i < added.size(); ++i) {
        if (added[i].uu.id == id) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/// What the stored values mean, through the same pure mapping the effect
/// uses - with every control the stored instance does not have (Hide Mount)
/// left at its default.
[[nodiscard]] PrefsBlob expectedFromStored() {
    ControlValues c;  // defaults first: what a control missing from the project gets
    c.colorOutput = 2;
    c.hdrTone = 3;
    c.rec709Look = 2;
    c.hdrPeak = 3;
    c.outputSize = 4;
    c.stabilization = 3;
    c.seamSearch = false;
    c.gainMatch = false;
    c.calibration = 3;
    c.flareRemoval = false;
    c.photoSeam = 2;
    c.photoStrengthPercent = 60.0;
    c.seamInsetDeg = 1.5;
    c.seamBlendDeg = 2.0;
    c.parallaxBlendDeg = 1.0;
    c.seamSmoothingDeg = 0.5;
    c.nearOffsetDeg = 0.5;
    c.farOffsetDeg = -0.5;
    c.lensShading = 1;
    c.shadingStrengthPercent = 60.0;
    c.parallaxGrid = 2;
    c.lensAlign = 2;
    c.sceneLight = 3;
    c.lensFocal = 2;
    c.dlogmFit = 1;
    c.exposureStops = -1.0;
    c.renderDevice = 2;
    c.directColour = 2;
    return prefsFromControls(c);
}

}  // namespace

TEST_CASE("a 0.5.0 project's 36-parameter instance restores by id onto the current list",
          "[sourcesettings][params][hidemount]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    REQUIRE(fixture.paramsSetupErr() == PF_Err_NONE);
    const std::vector<PF_ParamDef> added = fixture.host().addedParams(fixture.ref());
    REQUIRE(added.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    SECTION("every stored id still exists, with the type it was saved with") {
        for (const StoredParam& stored : kStored050) {
            INFO("id " << stored.id);
            const int index = indexOfId(added, stored.id);
            REQUIRE(index > 0);
            CHECK(added[static_cast<std::size_t>(index) - 1u].param_type == stored.type);
            // A stored popup value must still name an item of the list.
            if (stored.type == PF_Param_POPUP) {
                CHECK(stored.value <= added[static_cast<std::size_t>(index) - 1u].u.pd.num_choices);
            }
        }
    }

    SECTION("the only control the old instance lacks is Hide Mount, and it starts On") {
        std::set<int> storedIds;
        for (const StoredParam& stored : kStored050) {
            storedIds.insert(stored.id);
        }
        std::vector<int> missing;
        for (const PF_ParamDef& def : added) {
            if (storedIds.count(static_cast<int>(def.uu.id)) == 0u) {
                missing.push_back(static_cast<int>(def.uu.id));
            }
        }
        REQUIRE(missing.size() == 1u);
        CHECK(missing.front() == OSV_SS_ID_HIDE_MOUNT);
        // The default the host gives a control a project does not mention.
        CHECK(fixture.popup(kIndexHideMount) == OSV_SS_HIDE_MOUNT_DEFAULT);
        CHECK(OSV_SS_HIDE_MOUNT_DEFAULT == static_cast<int>(PrefsHideMount::On) + 1);
    }

    SECTION("restored by id, the old values translate to exactly what they meant") {
        // Play the host: every stored value goes to the control with its id.
        for (const StoredParam& stored : kStored050) {
            const int index = indexOfId(added, stored.id);
            REQUIRE(index > 0);
            switch (stored.type) {
            case PF_Param_POPUP: fixture.setPopup(index, stored.value); break;
            case PF_Param_CHECKBOX: fixture.setCheckbox(index, stored.value != 0); break;
            case PF_Param_FLOAT_SLIDER: fixture.setSlider(index, stored.slider); break;
            default: break;  // group markers and buttons carry no value
            }
        }

        // The host's prefs block for the TRANSLATE call.
        std::vector<char> bytes(PrefsBlob::kSize, '\0');
        PF_TranslateParamsToPrefsExtra extra{};
        extra.prefsPC = reinterpret_cast<PF_ImporterPrefsDataPtr>(bytes.data());
        extra.prefs_sizeLu = static_cast<A_u_long>(PrefsBlob::kSize);
        REQUIRE(fixture.send(PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, &extra) == PF_Err_NONE);
        PrefsBlob translated{};
        std::memcpy(&translated, bytes.data(), PrefsBlob::kSize);

        REQUIRE(translated.isValid());
        CHECK(translated == expectedFromStored());
        // Spot checks in plain words, so a failure names the field.
        CHECK(translated.color() == PrefsColorOutput::HLG);
        CHECK(translated.outputSize == static_cast<std::uint8_t>(PrefsOutputSize::HD2K));
        CHECK(translated.stab() == PrefsStabilization::Full);
        CHECK(translated.seamSearch == 0u);
        CHECK(translated.calibrationChoice() == PrefsCalibrationChoice::Underwater);
        CHECK(translated.lensFocalChoice() == PrefsLensFocal::Camera);
        CHECK(translated.sceneLightChoice() == PrefsSceneLight::Night);
        CHECK(translated.exposureStops == -1.0f);
        // And the control the 0.5.0 instance never had: On.
        CHECK(translated.hideMountChoice() == PrefsHideMount::On);
    }
}

TEST_CASE("the effect's version moved past 0.5.0's when its parameter set changed",
          "[sourcesettings][params][pipl]") {
    // 0.5.0 shipped 1.0.0 (PiPL word 525824) with 36 parameters; 0.5.1 added
    // Hide Mount under the same number.  A changed parameter set takes a new
    // version, so the host's plug-in cache and a project's stored instance
    // can tell the two lists apart.
    constexpr A_u_long kVersion050 = 525824u;
    CHECK(static_cast<A_u_long>(OSV_SOURCE_SETTINGS_PIPL_VERSION) > kVersion050);
    CHECK(OSV_SOURCE_SETTINGS_VERSION_MAJOR == 1);
    CHECK(OSV_SOURCE_SETTINGS_VERSION_MINOR == 1);
    CHECK(OSV_SOURCE_SETTINGS_VERSION_BUG == 0);
    CHECK(OSV_SOURCE_SETTINGS_PIPL_VERSION == PF_VERSION(1, 1, 0, PF_Stage_RELEASE, 0));

    // What GLOBAL_SETUP reports is that same word.
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    CHECK(fixture.globalSetupOut().my_version == static_cast<A_u_long>(OSV_SOURCE_SETTINGS_PIPL_VERSION));
}
