// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_lens_ui.cpp - [WP-LENSUI] the Effect Controls panel shows one lens at
// a time.
//
// The user's question was "why is there separate control for fov and 'DJI
// FoV' and zoom - why would DJI FOV and FOV appear differently?", and the
// answer asked for was a "DJI | Classic" dropdown that changes what the
// panel shows, DJI by default.  What is pinned here, through the LOADED
// module and the mock host's recording PF Param Utils Suite:
//
//   1. the visibility table itself (pure);
//   2. PF_Cmd_UPDATE_PARAMS_UI hides the other lens's controls and shows the
//      selected lens's, with PF_PUI_INVISIBLE through PF_UpdateParamUI, and
//      names DJI FOV plain "FOV" while it is the one FOV on screen;
//   3. the update never damages the controls: the slider ranges and names
//      come from the effect's own constants even when the host's copy of a
//      def is empty, no value is written and no change flag set;
//   4. a lens switch in USER_CHANGED_PARAM asks for a panel refresh and makes
//      no PF_UpdateParamUI call of its own (Premiere ignores visibility
//      changes made there on the first instance of an effect);
//   5. every failure is non-fatal: no suite, a refused control.

#include "ReframeTestSupport.h"

#include "ReframeParams.h"

#include "MockHost.h"

// kPFParamUtilsSuite / kPFParamUtilsSuiteVersion3, to hide the suite.
#include "AE_EffectSuites.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using osv::premiere::mock::MockHost;
using osv::premiere::mock::ParamUiUpdate;

namespace {

/// The controls UPDATE_PARAMS_UI manages: the five lens-specific controls and
/// the hidden Camera Model mirror.
constexpr int kManaged[] = {kIndexCameraModel, kIndexFov, kIndexDistortion, kIndexZoom, kIndexDjiFov, kIndexCorrection};

/// A mock host with one effect instance whose parameters exist.
struct LensUiFixture {
    MockHost host;
    PF_ProgPtr ref = nullptr;
    PF_InData in{};
    PF_OutData out{};
    std::vector<PF_ParamDef> registered;

    LensUiFixture() {
        ref = host.createEffectRef(0x4000, 31);
        REQUIRE(ref != nullptr);
        in = host.makeInData(ref, {});
        out = host.makeOutData();
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_GLOBAL_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        REQUIRE(LoadedPlugin::instance().effectMain()(PF_Cmd_PARAMS_SETUP, &in, &out, nullptr, nullptr, nullptr) ==
                PF_Err_NONE);
        registered = host.addedParams(ref);
        REQUIRE(registered.size() == static_cast<std::size_t>(kParamCount));
        in = host.makeInData(ref, {});
        out = host.makeOutData();
    }
    ~LensUiFixture() {
        if (ref) {
            host.destroyEffectRef(ref);
        }
    }
    LensUiFixture(const LensUiFixture&) = delete;
    LensUiFixture& operator=(const LensUiFixture&) = delete;

    /// The registered def of a control (AE index, 1-based).
    [[nodiscard]] const PF_ParamDef& def(int aeIndex) const {
        return registered[static_cast<std::size_t>(aeIndex) - 1u];
    }
};

/// A params array the way the host hands one to a supervised selector: index
/// 0 is the input layer, 1..n copies of the registered defs, change flags
/// cleared.  Owned by the test so it can see what the effect wrote.
struct ParamArray {
    std::vector<PF_ParamDef> storage;
    std::vector<PF_ParamDef*> pointers;

    explicit ParamArray(const std::vector<PF_ParamDef>& registered) {
        storage.resize(registered.size() + 1u);
        std::memset(&storage[0], 0, sizeof(PF_ParamDef));
        storage[0].param_type = PF_Param_LAYER;
        for (std::size_t i = 0; i < registered.size(); ++i) {
            storage[i + 1u] = registered[i];
            storage[i + 1u].uu.change_flags = PF_ChangeFlag_NONE;
        }
        pointers.resize(storage.size());
        for (std::size_t i = 0; i < storage.size(); ++i) {
            pointers[i] = &storage[i];
        }
    }

    [[nodiscard]] PF_ParamDef** data() noexcept { return pointers.data(); }
    [[nodiscard]] PF_ParamDef& at(int aeIndex) noexcept { return storage[static_cast<std::size_t>(aeIndex)]; }

    /// Select a lens the way the host holds it after the effect supervised
    /// the switch: the popup and its mirror.
    void setLens(CameraModel model) {
        at(kIndexLens).u.pd.value = static_cast<A_long>(lensPopupValue(model));
        at(kIndexCameraModel).u.bd.value = (model == CameraModel::Dji) ? 1 : 0;
    }
};

/// Run PF_Cmd_UPDATE_PARAMS_UI on an array.
PF_Err updateUi(LensUiFixture& f, ParamArray& p) {
    return LoadedPlugin::instance().effectMain()(PF_Cmd_UPDATE_PARAMS_UI, &f.in, &f.out, p.data(), nullptr, nullptr);
}

/// Run PF_Cmd_USER_CHANGED_PARAM for one control on an array.
PF_Err userChanged(LensUiFixture& f, ParamArray& p, int aeIndex) {
    PF_UserChangedParamExtra extra{};
    extra.param_index = aeIndex;
    return LoadedPlugin::instance().effectMain()(PF_Cmd_USER_CHANGED_PARAM, &f.in, &f.out, p.data(), nullptr, &extra);
}

/// True when `aeIndex` is one of the managed controls.
[[nodiscard]] bool isManaged(int aeIndex) {
    for (const int m : kManaged) {
        if (m == aeIndex) {
            return true;
        }
    }
    return false;
}

/// Check the panel state the mock recorded against the visibility table for
/// `lens`: every managed control updated exactly once, hidden or shown as
/// the table says, under the right name, as the right type, with its
/// registered slider display - and no other control touched.
void checkPanelShows(LensUiFixture& f, CameraModel lens) {
    const std::vector<ParamUiUpdate> updates = f.host.paramUiUpdates(f.ref);
    for (const ParamUiUpdate& u : updates) {
        INFO("update of parameter " << u.index << " (" << u.name << ")");
        CHECK(isManaged(u.index));
    }
    for (const int index : kManaged) {
        INFO("managed parameter " << index);
        int count = 0;
        for (const ParamUiUpdate& u : updates) {
            count += (u.index == index) ? 1 : 0;
        }
        CHECK(count == 1);
        const std::optional<ParamUiUpdate> state = f.host.paramUiState(f.ref, index);
        REQUIRE(state.has_value());
        CHECK(state->invisible() == !controlVisible(index, lens));
        const PF_ParamDef& reg = f.def(index);
        CHECK(state->type == reg.param_type);
        if (index == kIndexCameraModel) {
            CHECK(state->name == "Camera Model");
            continue;
        }
        const LensControl* c = lensControl(index);
        REQUIRE(c != nullptr);
        CHECK(state->name == c->shown);
        // The slider display the update carries is exactly the registered
        // one: PF_UpdateParamUI applies these fields, so a drift would
        // silently re-range a slider.
        CHECK(state->sliderMin == static_cast<float>(reg.u.fs_d.slider_min));
        CHECK(state->sliderMax == static_cast<float>(reg.u.fs_d.slider_max));
        CHECK(state->precision == reg.u.fs_d.precision);
        CHECK(state->displayFlags == reg.u.fs_d.display_flags);
    }
}

}  // namespace

// ===========================================================================
//  1. The table
// ===========================================================================

TEST_CASE("each lens shows its own controls and every shared one", "[reframe][params][lens]") {
    for (int index = 1; index <= kParamCount; ++index) {
        INFO("parameter index " << index);
        const bool classic = controlVisible(index, CameraModel::Classic);
        const bool dji = controlVisible(index, CameraModel::Dji);
        if (index == kIndexFov || index == kIndexDistortion) {
            CHECK(classic);
            CHECK_FALSE(dji);
        } else if (index == kIndexZoom || index == kIndexDjiFov || index == kIndexCorrection) {
            CHECK_FALSE(classic);
            CHECK(dji);
        } else if (index == kIndexCameraModel) {
            // The retired checkbox is the popup's mirror, hidden under both.
            CHECK_FALSE(classic);
            CHECK_FALSE(dji);
        } else {
            // Output Resolution, Preset, Pan / Tilt / Roll, the Source group,
            // Smooth Keyframes, Drag Sensitivity and the Lens popup itself.
            CHECK(classic);
            CHECK(dji);
        }
    }
    // Out of range: never a lens-specific control, and never a crash.
    CHECK(lensControl(0) == nullptr);
    CHECK(lensControl(kParamCount + 1) == nullptr);
    CHECK(lensControl(-7) == nullptr);
}

TEST_CASE("exactly one control named FOV is on screen under either lens", "[reframe][params][lens]") {
    // The question this package answers: two sliders both about "FOV" that
    // meant different angles.  Now each lens shows one, and it is "FOV".
    for (const CameraModel lens : {CameraModel::Classic, CameraModel::Dji}) {
        INFO((lens == CameraModel::Dji ? "DJI" : "Classic"));
        int fovs = 0;
        for (const LensControl& c : kLensControls) {
            if (c.lens == lens && std::string(c.shown) == "FOV") {
                ++fovs;
            }
        }
        CHECK(fovs == 1);
    }
    // DJI FOV keeps its distinct REGISTERED name for a host that shows every
    // control at once, where the two FOVs must stay distinguishable.
    LensUiFixture f;
    CHECK(std::string(f.def(kIndexDjiFov).PF_DEF_NAME) == "DJI FOV");
    CHECK(std::string(lensControl(kIndexDjiFov)->shown) == "FOV");
    // Every other lens control is shown under the name it is registered with.
    for (const LensControl& c : kLensControls) {
        if (c.aeIndex == kIndexDjiFov) {
            continue;
        }
        INFO("parameter " << c.aeIndex);
        CHECK(std::string(f.def(c.aeIndex).PF_DEF_NAME) == c.shown);
    }
}

TEST_CASE("the Lens popup decodes DJI and Classic and treats garbage as the default", "[reframe][params][lens]") {
    CHECK(cameraModelFromLensPopup(1) == CameraModel::Dji);
    CHECK(cameraModelFromLensPopup(2) == CameraModel::Classic);
    // A corrupt project, an expression: anything else is the default, DJI.
    for (const long garbage : {0L, -1L, 3L, 99L, 0x7FFFFFFFL}) {
        INFO("popup value " << garbage);
        CHECK(cameraModelFromLensPopup(garbage) == CameraModel::Dji);
    }
    CHECK(lensPopupValue(CameraModel::Dji) == 1);
    CHECK(lensPopupValue(CameraModel::Classic) == 2);
    CHECK(kDefaultCameraModel == CameraModel::Dji);
    // The hidden mirror's default agrees with the popup's.
    CHECK(cameraModelFromCheckbox(OSV_REFRAME_CAMERA_MODEL_DEFAULT) == kDefaultCameraModel);
}

// ===========================================================================
//  2. UPDATE_PARAMS_UI
// ===========================================================================

TEST_CASE("a new instance shows DJI's controls: Zoom, FOV and Correction Angle", "[reframe][ui][lens]") {
    LensUiFixture f;
    // The array exactly as registered: the Lens popup at its default, DJI.
    ParamArray p(f.registered);
    REQUIRE(cameraModelFromLensPopup(p.at(kIndexLens).u.pd.value) == CameraModel::Dji);
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Dji);

    // Spelled out, since this is what the user sees.
    CHECK(f.host.paramUiState(f.ref, kIndexFov)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexDistortion)->invisible());
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexZoom)->invisible());
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexDjiFov)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexDjiFov)->name == "FOV");
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexCorrection)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexCameraModel)->invisible());
    // The always-visible controls are left alone entirely.
    for (const int index : {kIndexOutputResolution, kIndexPreset, kIndexPan, kIndexTilt, kIndexRoll, kIndexSmooth,
                            kIndexDragSensitivity, kIndexLens}) {
        INFO("parameter " << index);
        CHECK_FALSE(f.host.paramUiState(f.ref, index).has_value());
    }
}

TEST_CASE("Classic shows FOV and Distortion and hides DJI's three", "[reframe][ui][lens]") {
    LensUiFixture f;
    ParamArray p(f.registered);
    p.setLens(CameraModel::Classic);
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Classic);
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexFov)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexFov)->name == "FOV");
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexDistortion)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexZoom)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexDjiFov)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexCorrection)->invisible());
    CHECK(f.host.paramUiState(f.ref, kIndexCameraModel)->invisible());
}

TEST_CASE("the panel follows the popup, never the hidden checkbox", "[reframe][ui][lens]") {
    // A WP-CAMERA project whose checkbox says Classic, opened now: the popup
    // is at its default, DJI, and the panel must show DJI's controls.
    LensUiFixture f;
    ParamArray p(f.registered);
    p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Dji);
    p.at(kIndexCameraModel).u.bd.value = 0;
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Dji);

    // And the other way round.
    f.host.clearParamUiUpdates(f.ref);
    p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Classic);
    p.at(kIndexCameraModel).u.bd.value = 1;
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Classic);
}

TEST_CASE("switching back and forth shows each lens again every time", "[reframe][ui][lens]") {
    // Hide, show, hide: a control hidden once must come back.  The update is
    // applied on every call rather than trusting the host's copy of the flags.
    LensUiFixture f;
    ParamArray p(f.registered);
    for (const CameraModel lens :
         {CameraModel::Dji, CameraModel::Classic, CameraModel::Dji, CameraModel::Classic, CameraModel::Classic}) {
        INFO((lens == CameraModel::Dji ? "DJI" : "Classic"));
        f.host.clearParamUiUpdates(f.ref);
        p.setLens(lens);
        REQUIRE(updateUi(f, p) == PF_Err_NONE);
        checkPanelShows(f, lens);
    }
}

// ===========================================================================
//  3. The update never damages a control
// ===========================================================================

TEST_CASE("a host copy with no names and no slider fields still gets the right labels and ranges",
          "[reframe][ui][lens]") {
    // Nothing documents that the params a host hands UPDATE_PARAMS_UI carry
    // their names and slider fields, and PF_UpdateParamUI APPLIES the name
    // and the slider display of the def it is given - so a blind copy of an
    // empty def would blank a label or collapse a slider to 0..0.
    LensUiFixture f;
    ParamArray p(f.registered);
    for (const int index : kManaged) {
        PF_ParamDef& d = p.at(index);
        std::memset(&d, 0, sizeof(d));
    }
    p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Classic);
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Classic);
}

TEST_CASE("a null entry in the host's array is updated from the effect's own constants", "[reframe][ui][lens]") {
    LensUiFixture f;
    ParamArray p(f.registered);
    p.pointers[static_cast<std::size_t>(kIndexZoom)] = nullptr;
    p.pointers[static_cast<std::size_t>(kIndexCameraModel)] = nullptr;
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Dji);
}

TEST_CASE("UPDATE_PARAMS_UI writes no value and marks nothing changed", "[reframe][ui][lens]") {
    // It may only make cosmetic changes (AE_Effect.h, PF_Cmd_UPDATE_PARAMS_UI).
    LensUiFixture f;
    for (const CameraModel lens : {CameraModel::Dji, CameraModel::Classic}) {
        ParamArray p(f.registered);
        p.setLens(lens);
        const std::vector<PF_ParamDef> before = p.storage;
        REQUIRE(updateUi(f, p) == PF_Err_NONE);
        for (int index = 1; index <= kParamCount; ++index) {
            INFO("parameter " << index);
            CHECK(std::memcmp(&p.storage[static_cast<std::size_t>(index)], &before[static_cast<std::size_t>(index)],
                              sizeof(PF_ParamDef)) == 0);
        }
    }
}

// ===========================================================================
//  4. The switch itself
// ===========================================================================

TEST_CASE("picking a lens asks for a panel refresh and leaves visibility to UPDATE_PARAMS_UI",
          "[reframe][supervise][ui][lens]") {
    LensUiFixture f;
    ParamArray p(f.registered);  // a fresh instance: DJI

    // The user picks Classic.
    p.at(kIndexLens).u.pd.value = static_cast<A_long>(LensPopup::Classic);
    f.out.out_flags = 0;
    REQUIRE(userChanged(f, p, kIndexLens) == PF_Err_NONE);
    CHECK((f.out.out_flags & PF_OutFlag_REFRESH_UI) != 0);
    // No PF_UpdateParamUI during USER_CHANGED_PARAM: Premiere was reported
    // to ignore visibility changes made there on an effect's first instance
    // (Adobe DVARC-3737), so the panel is changed only by the refresh.
    CHECK(f.host.paramUiUpdates(f.ref).empty());
    // The mirror followed the popup.
    CHECK(p.at(kIndexCameraModel).u.bd.value == 0);

    // The refresh the host then sends shows Classic's controls.
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Classic);
}

TEST_CASE("a preset shows DJI's controls again", "[reframe][supervise][ui][lens]") {
    LensUiFixture f;
    ParamArray p(f.registered);
    p.setLens(CameraModel::Classic);
    p.at(kIndexPreset).u.pd.value = static_cast<A_long>(Preset::Asteroid);
    REQUIRE(userChanged(f, p, kIndexPreset) == PF_Err_NONE);
    CHECK(cameraModelFromLensPopup(p.at(kIndexLens).u.pd.value) == CameraModel::Dji);
    CHECK((p.at(kIndexLens).uu.change_flags & PF_ChangeFlag_CHANGED_VALUE) != 0);
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    checkPanelShows(f, CameraModel::Dji);
}

// ===========================================================================
//  5. Failures are never fatal
// ===========================================================================

TEST_CASE("without the Param Utils Suite the effect still answers and shows every control",
          "[reframe][ui][lens]") {
    LensUiFixture f;
    ParamArray p(f.registered);
    const int refsBefore = f.host.totalSuiteRefs();
    f.host.setSuiteAvailable(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, false);
    CHECK(updateUi(f, p) == PF_Err_NONE);
    CHECK(f.host.paramUiUpdates(f.ref).empty());
    f.host.setSuiteAvailable(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, true);
    // And with it, every acquire is released.
    REQUIRE(updateUi(f, p) == PF_Err_NONE);
    CHECK(f.host.totalSuiteRefs() == refsBefore);
}

TEST_CASE("a control the host refuses does not stop the others", "[reframe][ui][lens]") {
    LensUiFixture f;
    ParamArray p(f.registered);
    f.host.setParamUiError(f.ref, kIndexFov, PF_Err_BAD_CALLBACK_PARAM);
    CHECK(updateUi(f, p) == PF_Err_NONE);
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexFov).has_value());
    // Everything after the refused one was still applied.
    CHECK(f.host.paramUiState(f.ref, kIndexDistortion)->invisible());
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexZoom)->invisible());
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexDjiFov)->invisible());
    CHECK_FALSE(f.host.paramUiState(f.ref, kIndexCorrection)->invisible());
}

TEST_CASE("UPDATE_PARAMS_UI tolerates null arguments", "[reframe][ui][lens]") {
    LensUiFixture f;
    CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_UPDATE_PARAMS_UI, &f.in, &f.out, nullptr, nullptr, nullptr) ==
          PF_Err_NONE);
    CHECK(LoadedPlugin::instance().effectMain()(PF_Cmd_UPDATE_PARAMS_UI, nullptr, nullptr, nullptr, nullptr,
                                                nullptr) == PF_Err_NONE);
    CHECK(f.host.paramUiUpdates(f.ref).empty());
}
