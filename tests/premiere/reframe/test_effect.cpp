// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_effect.cpp - the After Effects side of Open360Reframe.aex, driven
// through the mock host.
//
// These tests exercise the BUILT module: the exported entry points, the
// PiPL resource compiled into it, and the answers PF_Cmd_GLOBAL_SETUP,
// PF_Cmd_PARAMS_SETUP and PF_Cmd_USER_CHANGED_PARAM give when Premiere asks
// the questions it asks at load time and while the user edits.

#include "ReframeTestSupport.h"

#include "ReframeParams.h"

#include "MockHost.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "AE_EffectVers.h"
#include "PrSDKPixelFormat.h"

#include <cstring>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using osv::premiere::mock::MockHost;

namespace {

/// A mock host plus one effect reference, the shape every test here needs.
struct EffectFixture {
    MockHost host;
    PF_ProgPtr ref = nullptr;

    explicit EffectFixture(PrTimelineID timeline = 0x1000, A_long instanceId = 7) {
        ref = host.createEffectRef(timeline, instanceId);
    }
    ~EffectFixture() {
        if (ref) {
            host.destroyEffectRef(ref);
        }
    }
    EffectFixture(const EffectFixture&) = delete;
    EffectFixture& operator=(const EffectFixture&) = delete;
};

/// Run one selector with no params / output, which is all the setup
/// selectors need.
PF_Err call(PF_Cmd cmd, PF_InData& in, PF_OutData& out, void* extra = nullptr) {
    return LoadedPlugin::instance().effectMain()(cmd, &in, &out, nullptr, nullptr, extra);
}

/// Drive GLOBAL_SETUP then PARAMS_SETUP and return the registered params.
std::vector<PF_ParamDef> setupParams(EffectFixture& f) {
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    REQUIRE(call(PF_Cmd_GLOBAL_SETUP, in, out) == PF_Err_NONE);
    REQUIRE(call(PF_Cmd_PARAMS_SETUP, in, out) == PF_Err_NONE);
    return f.host.addedParams(f.ref);
}

/// The items of a '|' separated popup string, as the effect stores it.
std::vector<std::string> splitItems(const char* items) {
    std::vector<std::string> out;
    if (!items) {
        return out;
    }
    std::string current;
    for (const char* p = items; *p; ++p) {
        if (*p == '|') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(*p);
        }
    }
    out.push_back(current);
    return out;
}

}  // namespace

// ===========================================================================
//  The module
// ===========================================================================
TEST_CASE("the built .aex loads and exports both entry points", "[reframe][module]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    INFO("module path: " << toUtf8(plugin.path()));
    INFO("error: " << plugin.error());
    REQUIRE(plugin.ok());

    // Both names are what the outside world binds to: "EffectMain" is what
    // the PiPL's CodeWin64X86 property declares and "xGPUFilterEntry" is the
    // symbol Premiere looks up to discover the GPU renderer.
    CHECK(plugin.effectMain() != nullptr);
    CHECK(plugin.gpuEntry() != nullptr);
    CHECK(plugin.effectMain() != reinterpret_cast<EffectMainFn>(plugin.gpuEntry()));
}

TEST_CASE("an unknown selector is ignored rather than reported as an error", "[reframe][module]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    // Premiere sends selectors an effect may not know (audio, smart render,
    // GPU device setup).  Returning an error for those makes the host log a
    // broken effect; PF_Err_NONE means "ignored".
    CHECK(call(PF_Cmd_AUDIO_RENDER, in, out) == PF_Err_NONE);
    CHECK(call(PF_Cmd_QUERY_DYNAMIC_FLAGS, in, out) == PF_Err_NONE);
    CHECK(call(PF_Cmd_SMART_PRE_RENDER, in, out) == PF_Err_NONE);
}

TEST_CASE("PF_Cmd_ABOUT fills a non-empty message", "[reframe][module]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    REQUIRE(call(PF_Cmd_ABOUT, in, out) == PF_Err_NONE);

    const std::string message(out.return_msg);
    CHECK_FALSE(message.empty());
    // The display name and the licence are the two things a user looks for.
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring(OSV_REFRAME_DISPLAY_NAME));
    CHECK_THAT(message, Catch::Matchers::ContainsSubstring("Apache-2.0"));
    // It must fit the host's buffer with room for the terminator.
    CHECK(message.size() < PF_MAX_EFFECT_MSG_LEN);
}

// ===========================================================================
//  GLOBAL_SETUP
// ===========================================================================
TEST_CASE("GLOBAL_SETUP reports exactly the PiPL flag words", "[reframe][setup]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    REQUIRE(call(PF_Cmd_GLOBAL_SETUP, in, out) == PF_Err_NONE);

    // This is the assertion that matters: a mismatch between the resource
    // Premiere reads at scan time and what the code reports at run time is a
    // documented reason for a host to reject an effect.
    CHECK(static_cast<unsigned>(out.out_flags) == static_cast<unsigned>(OSV_REFRAME_OUT_FLAGS));
    CHECK(static_cast<unsigned>(out.out_flags2) == static_cast<unsigned>(OSV_REFRAME_OUT_FLAGS_2));
    CHECK(static_cast<unsigned>(out.my_version) == static_cast<unsigned>(OSV_REFRAME_PIPL_VERSION));

    // And the individual bits, so a failure says WHICH flag moved.
    CHECK((out.out_flags & PF_OutFlag_DEEP_COLOR_AWARE) != 0);
    CHECK((out.out_flags & PF_OutFlag_SEND_UPDATE_PARAMS_UI) != 0);
    CHECK((out.out_flags & PF_OutFlag_PIX_INDEPENDENT) == 0);
    CHECK((out.out_flags & PF_OutFlag_I_USE_AUDIO) == 0);
    CHECK((out.out_flags2 & PF_OutFlag2_FLOAT_COLOR_AWARE) != 0);
    CHECK((out.out_flags2 & PF_OutFlag2_SUPPORTS_THREADED_RENDERING) != 0);
    CHECK((out.out_flags2 & PF_OutFlag2_REVEALS_ZERO_ALPHA) != 0);
    CHECK((out.out_flags2 & PF_OutFlag2_PARAM_GROUP_START_COLLAPSED_FLAG) != 0);
}

TEST_CASE("GLOBAL_SETUP registers the Premiere pixel formats in order", "[reframe][setup]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    // makeInData reports appl_id 'PrMr', so the Premiere branch runs.
    REQUIRE(in.appl_id == kAppID_Premiere);
    REQUIRE(call(PF_Cmd_GLOBAL_SETUP, in, out) == PF_Err_NONE);

    const std::vector<PrPixelFormat> formats = f.host.supportedPixelFormats(f.ref);
    REQUIRE(formats.size() == 2);
    // Order is preference order: float first, so an HDR panorama is not
    // quantised to 8 bits before being resampled.
    CHECK(formats[0] == PrPixelFormat_BGRA_4444_32f);
    CHECK(formats[1] == PrPixelFormat_BGRA_4444_8u);
}

TEST_CASE("GLOBAL_SETUP registers nothing outside Premiere", "[reframe][setup]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    // After Effects identifies itself differently and has no PF Pixel Format
    // Suite; the effect must not try to register anything there.
    in.appl_id = 'FXTC';
    REQUIRE(call(PF_Cmd_GLOBAL_SETUP, in, out) == PF_Err_NONE);
    CHECK(f.host.supportedPixelFormats(f.ref).empty());
    // The flags are host independent, though.
    CHECK(static_cast<unsigned>(out.out_flags) == static_cast<unsigned>(OSV_REFRAME_OUT_FLAGS));
}

TEST_CASE("GLOBAL_SETUP survives a host with no PF Pixel Format Suite", "[reframe][setup]") {
    EffectFixture f;
    f.host.setSuiteAvailable(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, false);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    // A missing suite is a graceful downgrade, never a failure: the host then
    // picks the format itself.
    CHECK(call(PF_Cmd_GLOBAL_SETUP, in, out) == PF_Err_NONE);
    CHECK(f.host.supportedPixelFormats(f.ref).empty());
    f.host.setSuiteAvailable(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, true);
}

TEST_CASE("GLOBAL_SETUP and GLOBAL_SETDOWN leave no suite acquired", "[reframe][setup]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    const int before = f.host.totalSuiteRefs();

    REQUIRE(call(PF_Cmd_GLOBAL_SETUP, in, out) == PF_Err_NONE);
    REQUIRE(call(PF_Cmd_GLOBAL_SETDOWN, in, out) == PF_Err_NONE);

    // Every AcquireSuite must be balanced; an unbalanced one keeps the host's
    // suite alive for the life of the process.
    CHECK(f.host.totalSuiteRefs() == before);
}

// ===========================================================================
//  PARAMS_SETUP
// ===========================================================================
TEST_CASE("PARAMS_SETUP registers the 13 documented parameters", "[reframe][params]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> params = setupParams(f);

    REQUIRE(params.size() == static_cast<std::size_t>(kParamCount));

    // Ids are permanent: they are written into every project file that uses
    // the effect, so a change here silently breaks saved projects.
    // The list includes the two PF_Param_GROUP_END terminators: PF_END_TOPIC
    // issues its own PF_ADD_PARAM, so a group terminator is a real parameter
    // occupying a real index in the MIDDLE of the list.
    const int expectedIds[kParamCount] = {
        OSV_REFRAME_ID_OUTPUT_ASPECT,    OSV_REFRAME_ID_CAMERA_TOPIC, OSV_REFRAME_ID_PRESET,
        OSV_REFRAME_ID_PAN,              OSV_REFRAME_ID_TILT,         OSV_REFRAME_ID_ROLL,
        OSV_REFRAME_ID_FOV,              OSV_REFRAME_ID_DISTORTION,   OSV_REFRAME_ID_CAMERA_TOPIC_END,
        OSV_REFRAME_ID_SOURCE_TOPIC,     OSV_REFRAME_ID_SOURCE_PAN,   OSV_REFRAME_ID_SOURCE_TILT,
        OSV_REFRAME_ID_SOURCE_ROLL,      OSV_REFRAME_ID_SOURCE_TOPIC_END, OSV_REFRAME_ID_SMOOTH,
    };
    const PF_ParamType expectedTypes[kParamCount] = {
        PF_Param_POPUP,       PF_Param_GROUP_START, PF_Param_POPUP,        PF_Param_ANGLE,
        PF_Param_ANGLE,       PF_Param_ANGLE,       PF_Param_FLOAT_SLIDER, PF_Param_FLOAT_SLIDER,
        PF_Param_GROUP_END,   PF_Param_GROUP_START, PF_Param_ANGLE,        PF_Param_ANGLE,
        PF_Param_ANGLE,       PF_Param_GROUP_END,   PF_Param_CHECKBOX,
    };
    // PF_END_TOPIC sets no name, and AEFX_CLR_STRUCT zeroes the def before
    // it, so a terminator's name is the empty string.
    const char* expectedNames[kParamCount] = {
        "Output Aspect", "Camera",      "Preset",           "Pan",  "Tilt",
        "Roll",          "FOV",         "Distortion",       "",     "Source",
        "Source Pan",    "Source Tilt", "Source Roll",      "",     "Smooth Keyframes",
    };

    for (int i = 0; i < kParamCount; ++i) {
        INFO("parameter " << i << " (" << expectedNames[i] << ")");
        CHECK(params[static_cast<std::size_t>(i)].uu.id == expectedIds[i]);
        CHECK(params[static_cast<std::size_t>(i)].param_type == expectedTypes[i]);
        CHECK(std::string(params[static_cast<std::size_t>(i)].PF_DEF_NAME) == expectedNames[i]);
        // The AE index of a control is its position + 1 (index 0 is the
        // input layer).  It is NOT the same number as the id: the two
        // PF_Param_GROUP_END terminators sit in the middle of the list, so
        // every control after a closed group has an index one higher than
        // its id per group already closed.  What must hold instead is that
        // the shipping index table agrees with the list the module actually
        // produced - which is the invariant the GPU path's
        // GetParam(index - 1) depends on.
        CHECK(params[static_cast<std::size_t>(i)].uu.id == kParamIdByIndex[static_cast<std::size_t>(i)]);
    }

    // And the named constants really do point at the parameters they name.
    CHECK(params[kIndexOutputAspect - 1].uu.id == OSV_REFRAME_ID_OUTPUT_ASPECT);
    CHECK(params[kIndexPreset - 1].uu.id == OSV_REFRAME_ID_PRESET);
    CHECK(params[kIndexFov - 1].uu.id == OSV_REFRAME_ID_FOV);
    CHECK(params[kIndexDistortion - 1].uu.id == OSV_REFRAME_ID_DISTORTION);
    CHECK(params[kIndexSourcePan - 1].uu.id == OSV_REFRAME_ID_SOURCE_PAN);
    CHECK(params[kIndexSmooth - 1].uu.id == OSV_REFRAME_ID_SMOOTH);
}

TEST_CASE("PARAMS_SETUP reports the parameter count the host will allocate", "[reframe][params]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    REQUIRE(call(PF_Cmd_GLOBAL_SETUP, in, out) == PF_Err_NONE);
    REQUIRE(call(PF_Cmd_PARAMS_SETUP, in, out) == PF_Err_NONE);

    // num_params INCLUDES the input layer the host inserts at index 0.
    CHECK(out.num_params == kParamCount + 1);
}

TEST_CASE("the popup items are exactly the documented lists", "[reframe][params]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> params = setupParams(f);
    REQUIRE(params.size() == static_cast<std::size_t>(kParamCount));

    SECTION("Output Aspect") {
        const PF_ParamDef& def = params[kIndexOutputAspect - 1];
        REQUIRE(def.param_type == PF_Param_POPUP);
        CHECK(def.u.pd.num_choices == OSV_REFRAME_ASPECT_COUNT);
        CHECK(def.u.pd.dephault == OSV_REFRAME_ASPECT_DEFAULT);
        CHECK(def.u.pd.value == OSV_REFRAME_ASPECT_DEFAULT);

        const std::vector<std::string> items = splitItems(def.u.pd.u.namesptr);
        REQUIRE(items.size() == OSV_REFRAME_ASPECT_COUNT);
        for (int i = 0; i < OSV_REFRAME_ASPECT_COUNT; ++i) {
            INFO("aspect item " << i);
            // The label in the table and the label in the popup string must
            // be the same text, or the table lookup a test does is fiction.
            CHECK(items[static_cast<std::size_t>(i)] == kAspects[i].label);
            CHECK(static_cast<int>(kAspects[i].value) == i + 1);
        }
    }

    SECTION("Preset") {
        const PF_ParamDef& def = params[kIndexPreset - 1];
        REQUIRE(def.param_type == PF_Param_POPUP);
        CHECK(def.u.pd.num_choices == OSV_REFRAME_PRESET_COUNT);
        CHECK(def.u.pd.dephault == OSV_REFRAME_PRESET_DEFAULT);
        // "Wide" is the default look.
        CHECK(static_cast<Preset>(def.u.pd.dephault) == Preset::Wide);

        const std::vector<std::string> items = splitItems(def.u.pd.u.namesptr);
        REQUIRE(items.size() == OSV_REFRAME_PRESET_COUNT);
        for (int i = 0; i < OSV_REFRAME_PRESET_COUNT; ++i) {
            INFO("preset item " << i);
            CHECK(items[static_cast<std::size_t>(i)] == kPresetTable[i].label);
            CHECK(static_cast<int>(kPresetTable[i].value) == i + 1);
        }
    }
}

TEST_CASE("the sliders carry the documented ranges and defaults", "[reframe][params]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> params = setupParams(f);

    SECTION("FOV") {
        const PF_ParamDef& def = params[kIndexFov - 1];
        REQUIRE(def.param_type == PF_Param_FLOAT_SLIDER);
        CHECK(def.u.fs_d.valid_min == static_cast<PF_FpShort>(OSV_REFRAME_FOV_VALID_MIN));
        CHECK(def.u.fs_d.valid_max == static_cast<PF_FpShort>(OSV_REFRAME_FOV_VALID_MAX));
        CHECK(def.u.fs_d.slider_min == static_cast<PF_FpShort>(OSV_REFRAME_FOV_SLIDER_MIN));
        CHECK(def.u.fs_d.slider_max == static_cast<PF_FpShort>(OSV_REFRAME_FOV_SLIDER_MAX));
        CHECK(def.u.fs_d.dephault == static_cast<PF_FpShort>(OSV_REFRAME_FOV_DEFAULT));
        CHECK(def.u.fs_d.precision == PF_Precision_TENTHS);
    }

    SECTION("Distortion") {
        const PF_ParamDef& def = params[kIndexDistortion - 1];
        REQUIRE(def.param_type == PF_Param_FLOAT_SLIDER);
        CHECK(def.u.fs_d.valid_min == static_cast<PF_FpShort>(OSV_REFRAME_DISTORTION_VALID_MIN));
        CHECK(def.u.fs_d.valid_max == static_cast<PF_FpShort>(OSV_REFRAME_DISTORTION_VALID_MAX));
        CHECK(def.u.fs_d.dephault == static_cast<PF_FpShort>(OSV_REFRAME_DISTORTION_DEFAULT));
        // Shown as a percentage because it IS one: 100 * the eye offset.
        CHECK((def.u.fs_d.display_flags & PF_ValueDisplayFlag_PERCENT) != 0);
    }

    SECTION("the angles default to zero and are unbounded") {
        for (const int index : {kIndexPan, kIndexTilt, kIndexRoll, kIndexSourcePan, kIndexSourceTilt,
                                kIndexSourceRoll}) {
            INFO("angle parameter index " << index);
            const PF_ParamDef& def = params[static_cast<std::size_t>(index) - 1u];
            REQUIRE(def.param_type == PF_Param_ANGLE);
            CHECK(def.u.ad.dephault == 0);
            CHECK(def.u.ad.value == 0);
        }
    }

    SECTION("Smooth Keyframes is an unchecked checkbox") {
        const PF_ParamDef& def = params[kIndexSmooth - 1];
        REQUIRE(def.param_type == PF_Param_CHECKBOX);
        CHECK(def.u.bd.dephault == OSV_REFRAME_SMOOTH_DEFAULT);
        CHECK(def.u.bd.value == OSV_REFRAME_SMOOTH_DEFAULT);
    }
}

TEST_CASE("exactly the supervised parameters carry PF_ParamFlag_SUPERVISE", "[reframe][params]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> params = setupParams(f);

    // Supervision is what makes PF_Cmd_USER_CHANGED_PARAM arrive at all.
    // Preset needs it to write the other three; FOV, Distortion and Tilt
    // need it so editing one flips Preset to Custom.
    const bool expected[kParamCount] = {
        true,   // Output Aspect (reserved for future UI enabling)
        false,  // Camera topic
        true,   // Preset
        false,  // Pan
        true,   // Tilt
        false,  // Roll
        true,   // FOV
        true,   // Distortion
        false,  // Source topic
        false,  // Source Pan
        false,  // Source Tilt
        false,  // Source Roll
        false,  // Smooth Keyframes
    };
    for (int i = 0; i < kParamCount; ++i) {
        INFO("parameter " << (i + 1) << " (" << params[static_cast<std::size_t>(i)].PF_DEF_NAME << ")");
        const bool supervised = (params[static_cast<std::size_t>(i)].flags & PF_ParamFlag_SUPERVISE) != 0;
        CHECK(supervised == expected[i]);
    }
}

TEST_CASE("every parameter group is opened and closed exactly once", "[reframe][params]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> params = setupParams(f);

    // Walk the list keeping a nesting depth.  Two properties matter and
    // neither is implied by the other:
    //
    //   * depth never goes negative (no terminator without an opener), and
    //     it is back to 0 at the end (every group closed);
    //   * the controls that are meant to be TOP LEVEL really are at depth 0.
    //
    // Without the second check a list that opened "Camera", never closed it
    // and appended a stray terminator at the very end would still balance -
    // and would still nest "Source" and "Smooth Keyframes" inside Camera,
    // which is precisely the bug this test exists to catch.
    int depth = 0;
    int maxDepth = 0;
    std::vector<int> depthOfIndex(params.size() + 1u, -1);

    for (std::size_t i = 0; i < params.size(); ++i) {
        const PF_ParamType type = params[i].param_type;
        if (type == PF_Param_GROUP_END) {
            --depth;
            INFO("parameter index " << (i + 1u) << " closes a group that was never opened");
            REQUIRE(depth >= 0);
        }
        // The depth RECORDED for a control is the depth it is displayed at:
        // an opener belongs to the level it sits in, its members to the next.
        depthOfIndex[i + 1u] = depth;
        if (type == PF_Param_GROUP_START) {
            ++depth;
            maxDepth = std::max(maxDepth, depth);
        }
    }

    INFO("final nesting depth " << depth);
    CHECK(depth == 0);
    // Two groups, neither inside the other.
    CHECK(maxDepth == 1);

    // The three top-level controls.
    CHECK(depthOfIndex[kIndexOutputAspect] == 0);
    CHECK(depthOfIndex[kIndexCameraTopic] == 0);
    CHECK(depthOfIndex[kIndexSourceTopic] == 0);
    CHECK(depthOfIndex[kIndexSmooth] == 0);

    // ... and a representative member of each group, which must NOT be.
    CHECK(depthOfIndex[kIndexFov] == 1);
    CHECK(depthOfIndex[kIndexSourcePan] == 1);
}

TEST_CASE("the Source topic starts collapsed", "[reframe][params]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> params = setupParams(f);
    // The out-flag that makes this honoured is asserted separately; here we
    // check the parameter actually carries the request.
    CHECK((params[kIndexSourceTopic - 1].flags & PF_ParamFlag_START_COLLAPSED) != 0);
    // The Camera topic is the one users always need, so it stays open.
    CHECK((params[kIndexCameraTopic - 1].flags & PF_ParamFlag_START_COLLAPSED) == 0);
}

// ===========================================================================
//  USER_CHANGED_PARAM (the supervised behaviour)
// ===========================================================================
namespace {

/// A params array the way PF_Cmd_USER_CHANGED_PARAM receives it: index 0 is
/// the input layer, 1..13 the controls.  The PF_ParamDefs are owned by the
/// caller so the test can inspect what the effect wrote.
struct ParamArray {
    std::vector<PF_ParamDef> storage;
    std::vector<PF_ParamDef*> pointers;

    explicit ParamArray(const std::vector<PF_ParamDef>& registered) {
        storage.resize(registered.size() + 1u);
        std::memset(&storage[0], 0, sizeof(PF_ParamDef));
        storage[0].param_type = PF_Param_LAYER;
        for (std::size_t i = 0; i < registered.size(); ++i) {
            storage[i + 1u] = registered[i];
            // The host clears change_flags before every call; the effect
            // sets them on the parameters it modified.
            storage[i + 1u].uu.change_flags = PF_ChangeFlag_NONE;
        }
        pointers.resize(storage.size());
        for (std::size_t i = 0; i < storage.size(); ++i) {
            pointers[i] = &storage[i];
        }
    }

    [[nodiscard]] PF_ParamDef** data() noexcept { return pointers.data(); }
    [[nodiscard]] PF_ParamDef& at(int aeIndex) noexcept { return storage[static_cast<std::size_t>(aeIndex)]; }
};

/// Degrees out of an AE angle control (fixed 16.16).
double angleOf(const PF_ParamDef& def) { return static_cast<double>(def.u.ad.value) / 65536.0; }

}  // namespace

TEST_CASE("changing Preset writes FOV, Distortion and Tilt", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    // Every preset that writes controls, checked against the same table the
    // effect uses - so this catches a table the effect fails to consult, not
    // a table whose numbers changed.
    for (const PresetEntry& entry : kPresetTable) {
        if (!entry.writesControls) {
            continue;
        }
        INFO("preset '" << entry.label << "'");
        ParamArray params(registered);
        params.at(kIndexPreset).u.pd.value = static_cast<A_long>(entry.value);

        PF_UserChangedParamExtra extra{};
        extra.param_index = kIndexPreset;
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr,
                                                      &extra) == PF_Err_NONE);

        CHECK(params.at(kIndexFov).u.fs_d.value == static_cast<PF_FpShort>(entry.fovDeg));
        CHECK(params.at(kIndexDistortion).u.fs_d.value == static_cast<PF_FpShort>(entry.distortion));
        CHECK(angleOf(params.at(kIndexTilt)) == entry.tiltDeg);

        // Without CHANGED_VALUE the host would draw the new numbers but not
        // record an undoable edit, and they would be lost on the next render.
        CHECK((params.at(kIndexFov).uu.change_flags & PF_ChangeFlag_CHANGED_VALUE) != 0);
        CHECK((params.at(kIndexDistortion).uu.change_flags & PF_ChangeFlag_CHANGED_VALUE) != 0);
        CHECK((params.at(kIndexTilt).uu.change_flags & PF_ChangeFlag_CHANGED_VALUE) != 0);

        // Nothing else may be touched: Pan and Roll are the user's framing.
        CHECK(angleOf(params.at(kIndexPan)) == 0.0);
        CHECK(angleOf(params.at(kIndexRoll)) == 0.0);
        CHECK(params.at(kIndexPan).uu.change_flags == PF_ChangeFlag_NONE);
        CHECK(params.at(kIndexRoll).uu.change_flags == PF_ChangeFlag_NONE);
        // And the preset itself keeps the value the user chose.
        CHECK(params.at(kIndexPreset).u.pd.value == static_cast<A_long>(entry.value));
    }
}

TEST_CASE("selecting Custom writes nothing", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    ParamArray params(registered);
    // A look the user built by hand.
    params.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Custom);
    params.at(kIndexFov).u.fs_d.value = 77.0f;
    params.at(kIndexDistortion).u.fs_d.value = 33.0f;
    params.at(kIndexTilt).u.ad.value = static_cast<PF_Fixed>(12 * 65536);

    PF_UserChangedParamExtra extra{};
    extra.param_index = kIndexPreset;
    REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr,
                                                  &extra) == PF_Err_NONE);

    // "Custom" means "these are my numbers"; overwriting them would throw
    // the user's work away the moment they reselected it.
    CHECK(params.at(kIndexFov).u.fs_d.value == 77.0f);
    CHECK(params.at(kIndexDistortion).u.fs_d.value == 33.0f);
    CHECK(angleOf(params.at(kIndexTilt)) == 12.0);
    CHECK(params.at(kIndexFov).uu.change_flags == PF_ChangeFlag_NONE);
}

TEST_CASE("editing FOV, Distortion or Tilt flips Preset to Custom", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    for (const int changedIndex : {kIndexFov, kIndexDistortion, kIndexTilt}) {
        INFO("edited parameter index " << changedIndex);
        ParamArray params(registered);
        params.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Asteroid);

        PF_UserChangedParamExtra extra{};
        extra.param_index = changedIndex;
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr,
                                                      &extra) == PF_Err_NONE);

        // The popup must stop claiming a look the numbers no longer match.
        CHECK(static_cast<Preset>(params.at(kIndexPreset).u.pd.value) == Preset::Custom);
        CHECK((params.at(kIndexPreset).uu.change_flags & PF_ChangeFlag_CHANGED_VALUE) != 0);
    }
}

TEST_CASE("editing a supervised control while Preset is already Custom changes nothing", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    ParamArray params(registered);
    params.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Custom);

    PF_UserChangedParamExtra extra{};
    extra.param_index = kIndexFov;
    REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr,
                                                  &extra) == PF_Err_NONE);

    // Setting CHANGED_VALUE on a value that did not change would put a
    // pointless entry in the undo stack on every slider drag.
    CHECK(static_cast<Preset>(params.at(kIndexPreset).u.pd.value) == Preset::Custom);
    CHECK(params.at(kIndexPreset).uu.change_flags == PF_ChangeFlag_NONE);
}

TEST_CASE("editing an unsupervised control leaves Preset alone", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    ParamArray params(registered);
    params.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Wide);

    // Pan and Roll are framing, not look: they do not invalidate a preset.
    for (const int changedIndex : {kIndexPan, kIndexRoll, kIndexSourcePan, kIndexSmooth}) {
        INFO("edited parameter index " << changedIndex);
        PF_UserChangedParamExtra extra{};
        extra.param_index = changedIndex;
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr,
                                                      &extra) == PF_Err_NONE);
        CHECK(static_cast<Preset>(params.at(kIndexPreset).u.pd.value) == Preset::Wide);
    }
}

TEST_CASE("USER_CHANGED_PARAM with a garbage preset value does not crash", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    // A corrupt project file, or an expression, can put anything here.
    for (const A_long value : {A_long(-5), A_long(0), A_long(99), A_long(0x7FFFFFFF)}) {
        INFO("preset popup value " << value);
        ParamArray params(registered);
        params.at(kIndexPreset).u.pd.value = value;
        PF_UserChangedParamExtra extra{};
        extra.param_index = kIndexPreset;
        CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr,
                                                    &extra) == PF_Err_NONE);
    }
}

TEST_CASE("USER_CHANGED_PARAM tolerates a null extra pointer", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    ParamArray params(registered);

    // Defensive: the selector is documented to carry an extra, but a plug-in
    // that dereferences it blindly takes the host down if one ever does not.
    CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &in, &out, params.data(), nullptr,
                                                nullptr) == PF_Err_NONE);
}

TEST_CASE("UPDATE_PARAMS_UI is accepted", "[reframe][supervise]") {
    EffectFixture f;
    const std::vector<PF_ParamDef> registered = setupParams(f);
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();
    ParamArray params(registered);

    // The out-flag promises we handle it; returning an error would be a lie.
    CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_UPDATE_PARAMS_UI, &in, &out, params.data(), nullptr,
                                                nullptr) == PF_Err_NONE);
}

// ===========================================================================
//  Sequence commands
// ===========================================================================
TEST_CASE("the sequence commands leave sequence_data null", "[reframe][sequence]") {
    EffectFixture f;
    PF_InData in = f.host.makeInData(f.ref, {});
    PF_OutData out = f.host.makeOutData();

    // The effect keeps no per-instance state, which is what makes
    // PF_OutFlag2_SUPPORTS_THREADED_RENDERING safe to declare.  A non-null
    // handle here would be a leak the host never frees.
    for (const PF_Cmd cmd : {PF_Cmd_SEQUENCE_SETUP, PF_Cmd_SEQUENCE_RESETUP, PF_Cmd_SEQUENCE_FLATTEN,
                             PF_Cmd_SEQUENCE_SETDOWN}) {
        INFO("selector " << static_cast<int>(cmd));
        out.sequence_data = reinterpret_cast<PF_Handle>(static_cast<std::uintptr_t>(0xDEADBEEF));
        CHECK(call(cmd, in, out) == PF_Err_NONE);
        CHECK(out.sequence_data == nullptr);
    }
}
