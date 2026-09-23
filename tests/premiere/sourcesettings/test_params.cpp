// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_params.cpp - the module, its global setup and its parameter list.
//
// Everything here is driven through the BUILT .aex, so what is asserted is
// what Premiere will see: the exported symbol, the flag words the host
// compares against the PiPL, the SetIsSourceSettingsEffect call that is the
// difference between a master-clip settings panel and an ordinary video
// filter, and the nine controls with their permanent ids, item lists,
// ranges, flags and defaults.

#include "SourceSettingsTestSupport.h"

#include "SourceSettingsParams.h"

#include "PrefsBlob.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::sourcesettings;
using namespace osv::premiere::sourcesettings::test;

namespace {

/// The parameter list the built module produced, as the host stored it.
[[nodiscard]] std::vector<PF_ParamDef> addedParams(EffectFixture& fixture) {
    return fixture.host().addedParams(fixture.ref());
}

/// The popup item string of a parameter def, as one '|' separated string.
/// AE stores it in u.pd.u.namesptr; a null pointer yields "" rather than a
/// crash, which is what makes a missing item list an assertion failure
/// instead of a fault.
[[nodiscard]] std::string popupItems(const PF_ParamDef& def) {
    const char* names = def.u.pd.u.namesptr;
    return names ? std::string(names) : std::string();
}

}  // namespace

// ===========================================================================
//  The module
// ===========================================================================

TEST_CASE("the module loads and exports EffectMain", "[sourcesettings][module]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    INFO("module: " << toUtf8(plugin.path()));
    INFO("error: " << plugin.error());
    REQUIRE(plugin.ok());
}

TEST_CASE("EffectMain answers an unknown selector without failing", "[sourcesettings][module]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());
    mock::MockHost host;
    PF_ProgPtr ref = host.createEffectRef(1, 1);
    PF_InData in = host.makeInData(ref, mock::InDataSpec{});
    PF_OutData out = host.makeOutData();

    // PF_Err_NONE is the documented "ignored" answer.  Returning an error
    // would make the host report a broken effect over a selector we simply
    // do not implement.
    CHECK(plugin.effectMain()(PF_Cmd_AUDIO_RENDER, &in, &out, nullptr, nullptr, nullptr) == PF_Err_NONE);
    CHECK(plugin.effectMain()(PF_Cmd_ARBITRARY_CALLBACK, &in, &out, nullptr, nullptr, nullptr) == PF_Err_NONE);

    // PF_Cmd_RENDER in particular: a source settings effect is never sent it,
    // and the handler must not be the one thing that crashes if a host ever
    // does.  A null output world and a null params array are the worst case.
    CHECK(plugin.effectMain()(PF_Cmd_RENDER, &in, &out, nullptr, nullptr, nullptr) == PF_Err_NONE);

    host.destroyEffectRef(ref);
}

TEST_CASE("every selector survives null records", "[sourcesettings][module]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());

    // A plug-in that faults on a null in_data or out_data takes the host down
    // during project load, which is the worst possible time.  None of these
    // may crash; the return code only has to be a code.
    for (const PF_Cmd cmd : {PF_Cmd_ABOUT, PF_Cmd_GLOBAL_SETUP, PF_Cmd_GLOBAL_SETDOWN, PF_Cmd_PARAMS_SETUP,
                             PF_Cmd_SEQUENCE_SETUP, PF_Cmd_SEQUENCE_RESETUP, PF_Cmd_SEQUENCE_FLATTEN,
                             PF_Cmd_SEQUENCE_SETDOWN, PF_Cmd_UPDATE_PARAMS_UI,
                             PF_Cmd_TRANSLATE_PARAMS_TO_PREFS}) {
        INFO("selector " << static_cast<int>(cmd));
        (void)plugin.effectMain()(cmd, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    SUCCEED("no selector faulted on null records");
}

TEST_CASE("PF_Cmd_ABOUT fills a message", "[sourcesettings][module]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());
    mock::MockHost host;
    PF_ProgPtr ref = host.createEffectRef(1, 1);
    PF_InData in = host.makeInData(ref, mock::InDataSpec{});
    PF_OutData out = host.makeOutData();

    REQUIRE(plugin.effectMain()(PF_Cmd_ABOUT, &in, &out, nullptr, nullptr, nullptr) == PF_Err_NONE);
    const std::string message(out.return_msg);
    CHECK_FALSE(message.empty());
    CHECK(message.find(OSV_SOURCE_SETTINGS_DISPLAY_NAME) != std::string::npos);
    // The About text is where the constraint is explained to a user who
    // wonders why there are no stopwatches, so it must actually say so.
    CHECK(message.find("keyframe") != std::string::npos);

    host.destroyEffectRef(ref);
}

// ===========================================================================
//  GLOBAL_SETUP
// ===========================================================================

TEST_CASE("GLOBAL_SETUP reports the PiPL flag words and the version",
          "[sourcesettings][globalsetup]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    REQUIRE(fixture.globalSetupErr() == PF_Err_NONE);

    // A mismatch between the PiPL and GLOBAL_SETUP is a documented way for an
    // AE effect to be rejected at load time, so these are compared bit for
    // bit against the header the PiPL is generated from.
    CHECK(fixture.globalSetupOut().out_flags == OSV_SOURCE_SETTINGS_OUT_FLAGS);
    CHECK(fixture.globalSetupOut().out_flags2 == OSV_SOURCE_SETTINGS_OUT_FLAGS_2);
    CHECK(fixture.globalSetupOut().my_version == OSV_SOURCE_SETTINGS_PIPL_VERSION);
}

TEST_CASE("GLOBAL_SETUP declares the effect as a source settings effect",
          "[sourcesettings][globalsetup]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    REQUIRE(fixture.globalSetupErr() == PF_Err_NONE);

    // THE assertion of this whole module.  Without this call Premiere treats
    // the .aex as an ordinary video filter: it is offered in the Effects
    // panel to drag onto clips, it is never attached to a master clip, and
    // PF_Cmd_TRANSLATE_PARAMS_TO_PREFS never arrives - so the panel would
    // show nine controls that do nothing at all.  There is no other way to
    // observe the flag, because in a real host it lives entirely inside the
    // host.
    const std::optional<bool> declared = fixture.host().isSourceSettingsEffect(fixture.ref());
    REQUIRE(declared.has_value());
    CHECK(*declared == true);
}

TEST_CASE("GLOBAL_SETUP declares nothing outside Premiere", "[sourcesettings][globalsetup]") {
    // 'FXTC' is After Effects.  A source settings effect is a Premiere
    // concept; AE does not publish the suite and has no master clips, so the
    // call must be skipped rather than made against a host that cannot serve
    // it.  The flags must still be reported, because the PiPL comparison
    // happens in every host.
    EffectFixture fixture('FXTC');
    REQUIRE(LoadedPlugin::instance().ok());
    REQUIRE(fixture.globalSetupErr() == PF_Err_NONE);

    CHECK_FALSE(fixture.host().isSourceSettingsEffect(fixture.ref()).has_value());
    CHECK(fixture.globalSetupOut().out_flags == OSV_SOURCE_SETTINGS_OUT_FLAGS);
    CHECK(fixture.globalSetupOut().out_flags2 == OSV_SOURCE_SETTINGS_OUT_FLAGS_2);
}

TEST_CASE("GLOBAL_SETUP survives a host with no Source Settings Suite",
          "[sourcesettings][globalsetup]") {
    // A plug-in that fails GLOBAL_SETUP is dropped entirely.  A panel that
    // works minus the master-clip attachment is strictly better than no
    // panel, so a missing suite must be a logged degradation and nothing
    // more.
    mock::MockHost host;
    host.setSuiteAvailable(kPFSourceSettingsSuite, kPFSourceSettingsSuiteVersion1, false);
    host.setSuiteAvailable(kPFSourceSettingsSuite, kPFSourceSettingsSuiteVersion2, false);

    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());
    PF_ProgPtr ref = host.createEffectRef(1, 1);
    PF_InData in = host.makeInData(ref, mock::InDataSpec{});
    in.appl_id = 'PrMr';
    PF_OutData out = host.makeOutData();

    CHECK(plugin.effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) == PF_Err_NONE);
    CHECK(out.out_flags == OSV_SOURCE_SETTINGS_OUT_FLAGS);
    CHECK_FALSE(host.isSourceSettingsEffect(ref).has_value());

    host.destroyEffectRef(ref);
}

TEST_CASE("GLOBAL_SETUP releases every suite it acquired", "[sourcesettings][globalsetup]") {
    mock::MockHost host;
    const int before = host.totalSuiteRefs();

    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());
    PF_ProgPtr ref = host.createEffectRef(1, 1);
    PF_InData in = host.makeInData(ref, mock::InDataSpec{});
    in.appl_id = 'PrMr';
    PF_OutData out = host.makeOutData();
    REQUIRE(plugin.effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) == PF_Err_NONE);

    // A leaked suite reference keeps a host object alive past the plug-in's
    // own lifetime, which in Premiere surfaces as a crash on project close
    // rather than anywhere near the leak.
    CHECK(host.totalSuiteRefs() == before);

    host.destroyEffectRef(ref);
}

// ===========================================================================
//  PARAMS_SETUP
// ===========================================================================

TEST_CASE("PARAMS_SETUP registers exactly the documented parameter list",
          "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    REQUIRE(fixture.paramsSetupErr() == PF_Err_NONE);

    const std::vector<PF_ParamDef> params = addedParams(fixture);

    // Twenty-eight: twenty value controls, the two Defaults buttons and the
    // six group markers.  PF_ADD_TOPIC and
    // PF_END_TOPIC each issue their own PF_ADD_PARAM, so a group occupies two
    // real slots - counting only the controls is the mistake that shifts
    // every index after the first group.
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));
    CHECK(fixture.paramsSetupOut().num_params == OSV_SOURCE_SETTINGS_PARAM_COUNT + 1);

    SECTION("the permanent ids are in add order") {
        for (int index = 1; index <= OSV_SOURCE_SETTINGS_PARAM_COUNT; ++index) {
            INFO("index " << index);
            CHECK(params[static_cast<std::size_t>(index) - 1u].uu.id == kParamIdByIndex[index - 1]);
        }
    }

    SECTION("the names are the documented ones") {
        for (int index = 1; index <= OSV_SOURCE_SETTINGS_PARAM_COUNT; ++index) {
            INFO("index " << index);
            // PF_DEF_NAME is the SDK's own macro for the name field; using it
            // rather than the literal member name is what keeps this
            // compiling across the AE header's renames of it.
            CHECK(std::string(params[static_cast<std::size_t>(index) - 1u].PF_DEF_NAME) ==
                  std::string(kParamNameByIndex[index - 1]));
        }
    }

    SECTION("the types are the documented ones") {
        const std::pair<int, PF_ParamType> expected[] = {
            {kIndexColorOutput, PF_Param_POPUP},     {kIndexRec709Look, PF_Param_POPUP},
            {kIndexOutputSize, PF_Param_POPUP},
            {kIndexStabilization, PF_Param_POPUP},   {kIndexStitchTopic, PF_Param_GROUP_START},
            {kIndexSeamSearch, PF_Param_CHECKBOX},   {kIndexGainMatch, PF_Param_CHECKBOX},
            {kIndexCalibration, PF_Param_POPUP},     {kIndexFlareRemoval, PF_Param_CHECKBOX},
            {kIndexPhotoSeam, PF_Param_POPUP},       {kIndexPhotoStrength, PF_Param_FLOAT_SLIDER},  // [WP-PHOTO]
            {kIndexSeamInset, PF_Param_FLOAT_SLIDER},
            {kIndexSeamBlend, PF_Param_FLOAT_SLIDER},  {kIndexParallaxBlend, PF_Param_FLOAT_SLIDER},  // [WP-SEAMTOOLS]
            {kIndexSeamSmoothing, PF_Param_FLOAT_SLIDER}, {kIndexNearOffset, PF_Param_FLOAT_SLIDER},
            {kIndexFarOffset, PF_Param_FLOAT_SLIDER},
            {kIndexStitchTopicEnd, PF_Param_GROUP_END},
            {kIndexAdvancedTopic, PF_Param_GROUP_START}, {kIndexDlogmFit, PF_Param_POPUP},
            {kIndexExposure, PF_Param_FLOAT_SLIDER}, {kIndexRenderDevice, PF_Param_POPUP},
            {kIndexDirectColour, PF_Param_POPUP},    {kIndexAdvancedTopicEnd, PF_Param_GROUP_END},
        };
        for (const auto& [index, type] : expected) {
            INFO("index " << index);
            CHECK(params[static_cast<std::size_t>(index) - 1u].param_type == type);
        }
    }
}

TEST_CASE("every value-carrying control refuses to vary over time", "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    // This is not a style preference: the values travel to the importer as
    // ONE FLAT BLOB through PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, and a blob has
    // no time axis, so a keyframe could never be read back.  Without
    // PF_ParamFlag_CANNOT_TIME_VARY the panel would show a stopwatch on every
    // control and every keyframe a user set would silently do nothing.
    const int valueIndices[kValueParamCount] = {
        kIndexColorOutput, kIndexOutputSize, kIndexStabilization, kIndexSeamSearch, kIndexGainMatch,
        kIndexCalibration, kIndexDlogmFit,   kIndexExposure,      kIndexRenderDevice, kIndexDirectColour,
        kIndexFlareRemoval,
        kIndexRec709Look,
        kIndexPhotoSeam, kIndexPhotoStrength, kIndexSeamInset,  // [WP-PHOTO]
        kIndexSeamBlend, kIndexParallaxBlend, kIndexSeamSmoothing, kIndexNearOffset, kIndexFarOffset,  // [WP-SEAMTOOLS]
    };
    for (const int index : valueIndices) {
        REQUIRE(index >= 1);  // a short initialiser list would leave zeros behind
        INFO("index " << index << " (" << kParamNameByIndex[index - 1] << ")");
        CHECK((params[static_cast<std::size_t>(index) - 1u].flags & PF_ParamFlag_CANNOT_TIME_VARY) != 0);
    }
}

TEST_CASE("the Advanced group starts collapsed and the Stitching group does not",
          "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    // Stitching holds the three settings a user changes most, so it is open;
    // Advanced holds the three they rarely touch, so it is folded away.  The
    // collapsed flag only works because
    // PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG is set, which the
    // GLOBAL_SETUP test pins.
    CHECK((params[kIndexStitchTopic - 1].flags & PF_ParamFlag_START_COLLAPSED) == 0);
    CHECK((params[kIndexAdvancedTopic - 1].flags & PF_ParamFlag_START_COLLAPSED) != 0);
}

TEST_CASE("the two groups are balanced and every control is inside the intended one",
          "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    // Walk the list keeping a nesting depth.  An unclosed group is not a
    // cosmetic problem: everything after it nests inside it, so collapsing
    // one group would hide controls meant to be siblings.
    int depth = 0;
    int maxDepth = 0;
    std::vector<int> depthAt(params.size() + 1u, 0);
    for (std::size_t i = 0; i < params.size(); ++i) {
        const PF_ParamDef& def = params[i];
        if (def.param_type == PF_Param_GROUP_END) {
            --depth;
        }
        depthAt[i + 1u] = depth;
        if (def.param_type == PF_Param_GROUP_START) {
            ++depth;
            maxDepth = depth > maxDepth ? depth : maxDepth;
        }
        INFO("index " << (i + 1u));
        CHECK(depth >= 0);
    }
    CHECK(depth == 0);        // every group closed
    CHECK(maxDepth == 1);     // no nesting: the two groups are siblings

    // The four top-level controls really are top level, and the seven grouped
    // ones really are one level in.
    for (const int index : {kIndexColorOutput, kIndexRec709Look, kIndexOutputSize, kIndexStabilization}) {
        INFO("top-level index " << index);
        CHECK(depthAt[static_cast<std::size_t>(index)] == 0);
    }
    for (const int index : {kIndexSeamSearch, kIndexGainMatch, kIndexCalibration, kIndexFlareRemoval, kIndexPhotoSeam,
                            kIndexPhotoStrength, kIndexSeamInset, kIndexSeamBlend, kIndexParallaxBlend,
                            kIndexSeamSmoothing, kIndexNearOffset, kIndexFarOffset, kIndexDlogmFit, kIndexExposure,
                            kIndexRenderDevice, kIndexDirectColour}) {
        INFO("grouped index " << index);
        CHECK(depthAt[static_cast<std::size_t>(index)] == 1);
    }
}

TEST_CASE("the popup item lists are the documented ones", "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    // The ORDER of every list is the PrefsBlob enum order, so an inserted
    // entry silently re-maps the setting in every saved project.  Comparing
    // the whole string catches an insertion anywhere.
    const std::pair<int, const char*> expected[] = {
        {kIndexColorOutput, OSV_SS_COLOR_ITEMS},   {kIndexRec709Look, OSV_SS_LOOK_ITEMS},
        {kIndexOutputSize, OSV_SS_SIZE_ITEMS},
        {kIndexStabilization, OSV_SS_STAB_ITEMS},  {kIndexCalibration, OSV_SS_CALIB_ITEMS},
        {kIndexDlogmFit, OSV_SS_FIT_ITEMS},        {kIndexRenderDevice, OSV_SS_DEVICE_ITEMS},
        {kIndexDirectColour, OSV_SS_DIRECT_COLOUR_ITEMS},
        {kIndexPhotoSeam, OSV_SS_PHOTO_SEAM_ITEMS},  // [WP-PHOTO]
    };
    for (const auto& [index, items] : expected) {
        INFO("index " << index << " (" << kParamNameByIndex[index - 1] << ")");
        CHECK(popupItems(params[static_cast<std::size_t>(index) - 1u]) == std::string(items));
    }
}

TEST_CASE("the Exposure slider's valid range is the blob's own clamp range",
          "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    const PF_ParamDef& def = params[kIndexExposure - 1];
    // A slider that can reach a value sanitise() clamps is a slider that
    // silently disagrees with the render.
    CHECK(static_cast<double>(def.u.fs_d.valid_min) ==
          Catch::Approx(static_cast<double>(PrefsBlob::kMinExposureStops)));
    CHECK(static_cast<double>(def.u.fs_d.valid_max) ==
          Catch::Approx(static_cast<double>(PrefsBlob::kMaxExposureStops)));
    // The slider range is narrower on purpose, so a drag has useful
    // resolution over the stops people actually use.
    CHECK(static_cast<double>(def.u.fs_d.slider_min) == Catch::Approx(OSV_SS_EXPOSURE_SLIDER_MIN));
    CHECK(static_cast<double>(def.u.fs_d.slider_max) == Catch::Approx(OSV_SS_EXPOSURE_SLIDER_MAX));
    CHECK(static_cast<double>(def.u.fs_d.slider_min) >= static_cast<double>(def.u.fs_d.valid_min));
    CHECK(static_cast<double>(def.u.fs_d.slider_max) <= static_cast<double>(def.u.fs_d.valid_max));
}

TEST_CASE("every control's default is PrefsBlob::defaults()", "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    // The one assertion that cannot be a static_assert: PrefsBlob::defaults()
    // memsets and then assigns, so it is a runtime function.  It is also the
    // assertion that matters most - two independently written defaults (a
    // 1-based popup literal in the PiPL and a 0-based enum in the blob) is
    // exactly how a plug-in ends up showing "2560 x 1280" while decoding at
    // 6000 x 3000.
    const PrefsBlob defaults = PrefsBlob::defaults();

    // AE popup values are 1-based; the blob's enums are 0-based.
    CHECK(params[kIndexColorOutput - 1].u.pd.dephault == static_cast<A_long>(defaults.colorOutput) + 1);
    CHECK(params[kIndexOutputSize - 1].u.pd.dephault == static_cast<A_long>(defaults.outputSize) + 1);
    CHECK(params[kIndexStabilization - 1].u.pd.dephault == static_cast<A_long>(defaults.stabilization) + 1);
    CHECK(params[kIndexCalibration - 1].u.pd.dephault == static_cast<A_long>(defaults.calibration) + 1);
    CHECK(params[kIndexDlogmFit - 1].u.pd.dephault == static_cast<A_long>(defaults.dlogmFit) + 1);
    CHECK(params[kIndexRenderDevice - 1].u.pd.dephault == static_cast<A_long>(defaults.renderDevice) + 1);
    CHECK(params[kIndexDirectColour - 1].u.pd.dephault == static_cast<A_long>(defaults.directColour) + 1);
    CHECK(params[kIndexRec709Look - 1].u.pd.dephault == static_cast<A_long>(defaults.look) + 1);  // [WP-LOOK]

    CHECK(params[kIndexSeamSearch - 1].u.bd.dephault == static_cast<A_long>(defaults.seamSearch));
    CHECK(params[kIndexGainMatch - 1].u.bd.dephault == static_cast<A_long>(defaults.gainMatch));
    CHECK(params[kIndexFlareRemoval - 1].u.bd.dephault == static_cast<A_long>(defaults.flareRemoval));  // [WP-FLARE]
    // [WP-PHOTO] The sky seam fix: Rim and colour at 100 %, the 2.6 deg inset.
    CHECK(params[kIndexPhotoSeam - 1].u.pd.dephault == static_cast<A_long>(defaults.photoSeam) + 1);
    CHECK(static_cast<double>(params[kIndexPhotoStrength - 1].u.fs_d.dephault) ==
          Catch::Approx(defaults.photoStrengthPercent()));
    CHECK(static_cast<double>(params[kIndexSeamInset - 1].u.fs_d.dephault) ==
          Catch::Approx(defaults.seamInsetDeg()));
    CHECK(static_cast<double>(params[kIndexExposure - 1].u.fs_d.dephault) ==
          Catch::Approx(static_cast<double>(defaults.exposureStops)));

    // And the same facts stated from the header's side, so an edit to either
    // spelling breaks this test rather than shipping a mismatch.
    CHECK(OSV_SS_SIZE_DEFAULT == static_cast<int>(defaults.outputSize) + 1);
    CHECK(OSV_SS_STAB_DEFAULT == static_cast<int>(defaults.stabilization) + 1);
    CHECK(OSV_SS_COLOR_DEFAULT == static_cast<int>(defaults.colorOutput) + 1);
}

TEST_CASE("the popups list every value of their prefs enum", "[sourcesettings][params]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    // A popup with fewer items than its enum has values is a setting the user
    // cannot select.  The module static_asserts the counts against the enums;
    // this checks the built module really registered that many items.
    const std::pair<int, int> expected[] = {
        {kIndexColorOutput, static_cast<int>(PrefsColorOutput::Count)},
        {kIndexOutputSize, static_cast<int>(PrefsOutputSize::Count)},
        {kIndexStabilization, static_cast<int>(PrefsStabilization::Count)},
        // Calibration lists every CHOICE (Auto, Lens Protectors, Underwater,
        // forced Native), which is one more than the stored set enum.
        {kIndexCalibration, static_cast<int>(PrefsCalibrationChoice::Count)},
        {kIndexDlogmFit, static_cast<int>(PrefsDlogmFit::Count)},
        {kIndexRenderDevice, static_cast<int>(PrefsRenderDevice::Count)},
        {kIndexDirectColour, static_cast<int>(PrefsDirectColour::Count)},
        {kIndexRec709Look, static_cast<int>(PrefsLook::Count)},
        {kIndexPhotoSeam, static_cast<int>(PrefsPhotoSeam::Count)},  // [WP-PHOTO]
    };
    for (const auto& [index, count] : expected) {
        INFO("index " << index << " (" << kParamNameByIndex[index - 1] << ")");
        CHECK(params[static_cast<std::size_t>(index) - 1u].u.pd.num_choices == count);
        // The item string must have exactly count-1 separators.
        const std::string items = popupItems(params[static_cast<std::size_t>(index) - 1u]);
        const std::ptrdiff_t separators = std::count(items.begin(), items.end(), '|');
        CHECK(separators == count - 1);
    }
}

TEST_CASE("the sky seam sliders offer exactly what the blob stores", "[sourcesettings][params][photoseam]") {
    // [WP-PHOTO] Sky Seam Strength is whole percent 0..100 and Seam Edge
    // Inset tenths of a degree 0.0..6.0 - PrefsBlob's photoStrength and
    // seamInset codes.  A slider that reached further would offer a value the
    // blob silently clamps; one that stopped short would hide a stored one.
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));

    const PF_ParamDef& strength = params[kIndexPhotoStrength - 1];
    CHECK(static_cast<double>(strength.u.fs_d.valid_min) == Catch::Approx(0.0));
    CHECK(static_cast<double>(strength.u.fs_d.valid_max) ==
          Catch::Approx(static_cast<double>(PrefsBlob::kMaxPhotoStrengthCode - 1)));
    CHECK(static_cast<double>(strength.u.fs_d.slider_min) == Catch::Approx(0.0));
    CHECK(static_cast<double>(strength.u.fs_d.slider_max) == Catch::Approx(100.0));
    CHECK(strength.u.fs_d.precision == PF_Precision_INTEGER);
    CHECK(strength.u.fs_d.display_flags == PF_ValueDisplayFlag_PERCENT);

    const PF_ParamDef& inset = params[kIndexSeamInset - 1];
    CHECK(static_cast<double>(inset.u.fs_d.valid_min) == Catch::Approx(0.0));
    CHECK(static_cast<double>(inset.u.fs_d.valid_max) ==
          Catch::Approx(static_cast<double>(PrefsBlob::kMaxSeamInsetCode - 1) / 10.0));
    CHECK(static_cast<double>(inset.u.fs_d.slider_min) == Catch::Approx(0.0));
    CHECK(static_cast<double>(inset.u.fs_d.slider_max) == Catch::Approx(6.0));
    CHECK(inset.u.fs_d.precision == PF_Precision_TENTHS);
}

TEST_CASE("the seam tool sliders offer exactly what the blob stores", "[sourcesettings][params][seamtools]") {
    // [WP-SEAMTOOLS] Each slider's valid and slider range is its blob field's
    // stored range, its default the blob's default, shown in hundredths.
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const std::vector<PF_ParamDef> params = addedParams(fixture);
    REQUIRE(params.size() == static_cast<std::size_t>(OSV_SOURCE_SETTINGS_PARAM_COUNT));
    const PrefsBlob defaults = PrefsBlob::defaults();
    struct Expected {
        int index;
        double lo;
        double hi;
        double dflt;
    };
    const double step = static_cast<double>(PrefsBlob::kSeamToolStepsPerDeg);
    const double offsetMax = static_cast<double>(PrefsBlob::kMaxSeamOffsetHundredths) / 100.0;
    const Expected expected[] = {
        {kIndexSeamBlend, (PrefsBlob::kMinSeamBlendCode - 1) / step, (PrefsBlob::kMaxSeamBlendCode - 1) / step,
         defaults.seamBlendDeg()},
        {kIndexParallaxBlend, 0.0, (PrefsBlob::kMaxParallaxBlendCode - 1) / step, defaults.parallaxBlendDeg()},
        {kIndexSeamSmoothing, 0.0, (PrefsBlob::kMaxSeamSmoothingCode - 1) / step, defaults.seamSmoothingDeg()},
        {kIndexNearOffset, -offsetMax, offsetMax, defaults.nearOffsetDeg()},
        {kIndexFarOffset, -offsetMax, offsetMax, defaults.farOffsetDeg()},
    };
    for (const Expected& e : expected) {
        const PF_ParamDef& def = params[static_cast<std::size_t>(e.index) - 1u];
        INFO("index " << e.index << " (" << kParamNameByIndex[e.index - 1] << ")");
        CHECK(def.param_type == PF_Param_FLOAT_SLIDER);
        CHECK(static_cast<double>(def.u.fs_d.valid_min) == Catch::Approx(e.lo));
        CHECK(static_cast<double>(def.u.fs_d.valid_max) == Catch::Approx(e.hi));
        CHECK(static_cast<double>(def.u.fs_d.slider_min) == Catch::Approx(e.lo));
        CHECK(static_cast<double>(def.u.fs_d.slider_max) == Catch::Approx(e.hi));
        CHECK(static_cast<double>(def.u.fs_d.dephault) == Catch::Approx(e.dflt));
        CHECK(def.u.fs_d.precision == PF_Precision_HUNDREDTHS);
        CHECK((def.flags & PF_ParamFlag_CANNOT_TIME_VARY) != 0);
    }
    // Their permanent ids are this package's range, 20-29.
    for (const int id : {OSV_SS_ID_SEAM_BLEND, OSV_SS_ID_PARALLAX_BLEND, OSV_SS_ID_SEAM_SMOOTHING,
                         OSV_SS_ID_NEAR_OFFSET, OSV_SS_ID_FAR_OFFSET}) {
        CHECK(id >= 20);
        CHECK(id <= 29);
    }
}