// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_prefs.cpp - the only path by which anything set in the Effect Controls
// panel reaches the decoder.
//
// Two halves:
//
//   1. PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, driven through the BUILT .aex, so
//      what is asserted is the blob Premiere will actually hand the importer.
//      Every field is round-tripped: each control is set to each of its
//      values in turn and the resulting blob is decoded and compared.
//   2. PF_Cmd_SEQUENCE_SETUP, which asks the importer (through the mock's
//      PerformSourceSettingsCommand) what the media is really being decoded
//      with and seeds the controls from the answer.
//
// The pure arithmetic underneath both - SourceSettingsMapping.cpp - is
// compiled into this executable as well, which is what lets the hostile-input
// cases below be written without a host.

#include "SourceSettingsTestSupport.h"

#include "SourceSettingsMapping.h"
#include "SourceSettingsParams.h"

#include "PrefsBlob.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <limits>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::sourcesettings;
using namespace osv::premiere::sourcesettings::test;

namespace {

/// The buffer the host owns for a TRANSLATE_PARAMS_TO_PREFS call.
///
/// Deliberately LARGER than a PrefsBlob and pre-filled with a recognisable
/// byte, so a test can prove two things at once: that exactly kSize bytes
/// were written, and that the tail was left alone (it is the host's memory,
/// not ours to define).
struct PrefsBuffer {
    static constexpr std::size_t kTail = 64;
    std::vector<char> bytes;

    PrefsBuffer() : bytes(PrefsBlob::kSize + kTail, '\xAB') {}

    [[nodiscard]] PF_TranslateParamsToPrefsExtra extra(A_u_long declaredSize) {
        PF_TranslateParamsToPrefsExtra e{};
        e.prefsPC = reinterpret_cast<PF_ImporterPrefsDataPtr>(bytes.data());
        e.prefs_sizeLu = declaredSize;
        return e;
    }

    [[nodiscard]] PF_TranslateParamsToPrefsExtra extra() {
        return extra(static_cast<A_u_long>(PrefsBlob::kSize));
    }

    /// The blob the effect wrote, read back byte-wise.
    [[nodiscard]] PrefsBlob blob() const {
        PrefsBlob b{};
        std::memcpy(&b, bytes.data(), PrefsBlob::kSize);
        return b;
    }

    /// True when every byte past the blob is still the fill pattern.
    [[nodiscard]] bool tailUntouched() const {
        for (std::size_t i = PrefsBlob::kSize; i < bytes.size(); ++i) {
            if (bytes[i] != '\xAB') {
                return false;
            }
        }
        return true;
    }

    /// True when nothing at all was written.
    [[nodiscard]] bool untouched() const {
        for (const char c : bytes) {
            if (c != '\xAB') {
                return false;
            }
        }
        return true;
    }
};

/// Translate the fixture's current control values into a blob.
[[nodiscard]] PrefsBlob translate(EffectFixture& fixture, PrefsBuffer& buffer) {
    PF_TranslateParamsToPrefsExtra extra = buffer.extra();
    const PF_Err err = fixture.send(PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, &extra);
    CHECK(err == PF_Err_NONE);
    return buffer.blob();
}

}  // namespace

// ===========================================================================
//  TRANSLATE_PARAMS_TO_PREFS - the happy path
// ===========================================================================

TEST_CASE("TRANSLATE_PARAMS_TO_PREFS writes a valid, already sanitised blob",
          "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    PrefsBuffer buffer;

    PrefsBlob blob = translate(fixture, buffer);

    // The importer treats a translated blob exactly like one written by the
    // modal dialog, so it has to satisfy the same two properties.
    CHECK(blob.isValid());
    CHECK(blob.magic == PrefsBlob::kMagic);
    CHECK(blob.version == PrefsBlob::kVersion);

    // sanitise() returning true means nothing had to change - which is the
    // stronger statement: not merely "the importer can repair this", but
    // "there is nothing to repair".
    PrefsBlob copy = blob;
    CHECK(copy.sanitise() == true);
    CHECK(copy == blob);

    // Exactly kSize bytes, and not one more.
    CHECK(buffer.tailUntouched());
}

TEST_CASE("the untouched controls translate to PrefsBlob::defaults()", "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    PrefsBuffer buffer;

    // The controls are at their registered defaults, so the blob must be the
    // documented default blob - byte for byte, because the whole blob is part
    // of the PPix cache key and a single differing byte means every frame is
    // re-rendered.
    const PrefsBlob blob = translate(fixture, buffer);
    CHECK(blob == PrefsBlob::defaults());
}

TEST_CASE("every field round-trips through the translated blob", "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // Each popup is walked over EVERY item, not just a couple: an off-by-one
    // between AE's 1-based popup values and the blob's 0-based enums would
    // pass a two-value spot check while silently shifting everything.
    SECTION("Colour Output") {
        for (int item = 1; item <= OSV_SS_COLOR_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexColorOutput, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).colorOutput == static_cast<std::uint8_t>(item - 1));
        }
    }
    SECTION("Look (Rec. 709 only)") {
        // [WP-LOOK] "DJI (default)" -> 0 (DjiStudio, what an older blob's
        // zero byte means), "OpenOSV standard" -> 1 (Standard).
        for (int item = 1; item <= OSV_SS_LOOK_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexRec709Look, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).look == static_cast<std::uint8_t>(item - 1));
        }
        PrefsBuffer first;
        fixture.setPopup(kIndexRec709Look, 1);
        CHECK(translate(fixture, first).lookChoice() == PrefsLook::DjiStudio);
        PrefsBuffer second;
        fixture.setPopup(kIndexRec709Look, 2);
        CHECK(translate(fixture, second).lookChoice() == PrefsLook::Standard);
    }
    SECTION("Output Size") {
        for (int item = 1; item <= OSV_SS_SIZE_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexOutputSize, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).outputSize == static_cast<std::uint8_t>(item - 1));
        }
    }
    SECTION("Stabilisation") {
        for (int item = 1; item <= OSV_SS_STAB_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexStabilization, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).stabilization == static_cast<std::uint8_t>(item - 1));
        }
    }
    SECTION("Calibration") {
        for (int item = 1; item <= OSV_SS_CALIB_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexCalibration, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).calibration == static_cast<std::uint8_t>(item - 1));
        }
    }
    SECTION("D-Log M Curve") {
        for (int item = 1; item <= OSV_SS_FIT_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexDlogmFit, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).dlogmFit == static_cast<std::uint8_t>(item - 1));
        }
    }
    SECTION("Render Device") {
        for (int item = 1; item <= OSV_SS_DEVICE_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexRenderDevice, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).renderDevice == static_cast<std::uint8_t>(item - 1));
        }
    }
    SECTION("Program Monitor Colour") {
        // [WP-SETTINGS] "Sequence space (fast)" -> 0 (SequenceSpace, the
        // default), "Match Source monitor" -> 1 (MatchSource).
        for (int item = 1; item <= OSV_SS_DIRECT_COLOUR_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexDirectColour, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).directColour == static_cast<std::uint8_t>(item - 1));
        }
        PrefsBuffer first;
        fixture.setPopup(kIndexDirectColour, 1);
        CHECK(translate(fixture, first).directColourMode() == PrefsDirectColour::SequenceSpace);
        PrefsBuffer second;
        fixture.setPopup(kIndexDirectColour, 2);
        CHECK(translate(fixture, second).directColourMode() == PrefsDirectColour::MatchSource);
    }
    SECTION("Seam Search") {
        for (const bool on : {false, true}) {
            PrefsBuffer buffer;
            fixture.setCheckbox(kIndexSeamSearch, on);
            INFO("checkbox " << on);
            CHECK(translate(fixture, buffer).seamSearch == (on ? 1u : 0u));
        }
    }
    SECTION("Exposure Match") {
        for (const bool on : {false, true}) {
            PrefsBuffer buffer;
            fixture.setCheckbox(kIndexGainMatch, on);
            INFO("checkbox " << on);
            CHECK(translate(fixture, buffer).gainMatch == (on ? 1u : 0u));
        }
    }
    SECTION("Sun Ghost Removal") {  // [WP-FLARE]
        for (const bool on : {false, true}) {
            PrefsBuffer buffer;
            fixture.setCheckbox(kIndexFlareRemoval, on);
            INFO("checkbox " << on);
            CHECK(translate(fixture, buffer).flareRemoval == (on ? 1u : 0u));
        }
    }
    SECTION("Sky Seam Fix") {  // [WP-PHOTO]
        // "Off" -> 0, "Rim only" -> 1, "Rim and colour" -> 2 (RimAndGain).
        for (int item = 1; item <= OSV_SS_PHOTO_SEAM_COUNT; ++item) {
            PrefsBuffer buffer;
            fixture.setPopup(kIndexPhotoSeam, item);
            INFO("popup value " << item);
            CHECK(translate(fixture, buffer).photoSeam == static_cast<std::uint8_t>(item - 1));
        }
        PrefsBuffer off;
        fixture.setPopup(kIndexPhotoSeam, 1);
        CHECK(translate(fixture, off).photoSeamMode() == PrefsPhotoSeam::Off);
        PrefsBuffer full;
        fixture.setPopup(kIndexPhotoSeam, OSV_SS_PHOTO_SEAM_COUNT);
        CHECK(translate(fixture, full).photoSeamMode() == PrefsPhotoSeam::RimAndGain);
    }
    SECTION("Sky Seam Strength") {  // [WP-PHOTO]
        for (const double percent : {0.0, 1.0, 37.0, 99.0, 100.0}) {
            PrefsBuffer buffer;
            fixture.setSlider(kIndexPhotoStrength, percent);
            INFO("percent " << percent);
            CHECK(translate(fixture, buffer).photoStrengthPercent() == Catch::Approx(percent));
        }
    }
    SECTION("Seam Edge Inset") {  // [WP-PHOTO]
        for (const double deg : {0.0, 0.3, 1.5, 2.6, 4.4, 6.0}) {
            PrefsBuffer buffer;
            fixture.setSlider(kIndexSeamInset, deg);
            INFO("degrees " << deg);
            // PF_FpShort -> tenths: 0.3 arrives as 0.30000001 and rounds back.
            CHECK(translate(fixture, buffer).seamInsetDeg() == Catch::Approx(deg));
        }
    }
    SECTION("Exposure") {
        for (const double stops : {-6.0, -3.0, -0.5, 0.0, 0.5, 2.25, 6.0}) {
            PrefsBuffer buffer;
            fixture.setSlider(kIndexExposure, stops);
            INFO("stops " << stops);
            // The blob stores a float, the control a PF_FpShort; Approx
            // absorbs the one rounding step between them.
            CHECK(static_cast<double>(translate(fixture, buffer).exposureStops) == Catch::Approx(stops));
        }
    }
}

TEST_CASE("all ten controls together round-trip as one blob", "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    PrefsBuffer buffer;

    // Every control moved away from its default at once, so a field that is
    // read from the wrong index cannot be masked by happening to hold its
    // neighbour's default.  Each value is the LAST item of its list where
    // possible, which is the one an off-by-one is most likely to overflow.
    fixture.setPopup(kIndexColorOutput, OSV_SS_COLOR_COUNT);
    fixture.setPopup(kIndexOutputSize, OSV_SS_SIZE_COUNT);
    fixture.setPopup(kIndexStabilization, OSV_SS_STAB_COUNT);
    fixture.setCheckbox(kIndexSeamSearch, false);
    fixture.setCheckbox(kIndexGainMatch, false);
    fixture.setPopup(kIndexCalibration, OSV_SS_CALIB_COUNT);
    fixture.setPopup(kIndexDlogmFit, OSV_SS_FIT_COUNT);
    fixture.setSlider(kIndexExposure, -2.5);
    fixture.setPopup(kIndexRenderDevice, OSV_SS_DEVICE_COUNT);
    fixture.setPopup(kIndexDirectColour, OSV_SS_DIRECT_COLOUR_COUNT);
    // [WP-PHOTO] The sky seam fix's default IS its last item, so it moves to
    // the first (Off), and both sliders away from their defaults.
    fixture.setPopup(kIndexPhotoSeam, 1);
    fixture.setSlider(kIndexPhotoStrength, 40.0);
    fixture.setSlider(kIndexSeamInset, 1.2);

    // The LAST item of each list, so these track the enums rather than being
    // re-typed every time one grows - Colour Output has already gained
    // D-Log M passthrough as a fourth entry since this test was written.
    const PrefsBlob blob = translate(fixture, buffer);

    // Each popup was set to the LAST item of its list, so each blob field must
    // be the LAST value of its enum - expressed as "Count - 1" rather than by
    // naming the enumerator.  Naming it is how this test kept breaking for the
    // wrong reason: Colour Output gained D-Log M passthrough and the D-Log M
    // Curve list gained Osmo 360, and each time a hard-coded enumerator turned
    // a healthy list into a red test.  Count - 1 tracks both automatically and
    // still catches the thing that matters, an off-by-one at the top of the
    // range.
    CHECK(static_cast<int>(blob.colorOutput) == static_cast<int>(PrefsColorOutput::Count) - 1);
    CHECK(static_cast<int>(blob.colorOutput) == OSV_SS_COLOR_COUNT - 1);
    CHECK(static_cast<int>(blob.outputSize) == static_cast<int>(PrefsOutputSize::Count) - 1);
    CHECK(static_cast<int>(blob.outputSize) == OSV_SS_SIZE_COUNT - 1);
    CHECK(static_cast<int>(blob.stabilization) == static_cast<int>(PrefsStabilization::Count) - 1);
    CHECK(static_cast<int>(blob.stabilization) == OSV_SS_STAB_COUNT - 1);
    CHECK(static_cast<int>(blob.calibration) == static_cast<int>(PrefsCalibration::Count) - 1);
    CHECK(static_cast<int>(blob.calibration) == OSV_SS_CALIB_COUNT - 1);
    CHECK(static_cast<int>(blob.dlogmFit) == static_cast<int>(PrefsDlogmFit::Count) - 1);
    CHECK(static_cast<int>(blob.dlogmFit) == OSV_SS_FIT_COUNT - 1);
    CHECK(static_cast<int>(blob.renderDevice) == static_cast<int>(PrefsRenderDevice::Count) - 1);
    CHECK(static_cast<int>(blob.renderDevice) == OSV_SS_DEVICE_COUNT - 1);
    CHECK(static_cast<int>(blob.directColour) == static_cast<int>(PrefsDirectColour::Count) - 1);
    CHECK(static_cast<int>(blob.directColour) == OSV_SS_DIRECT_COLOUR_COUNT - 1);

    // The two checkboxes and the slider were moved away from their defaults
    // too, so no field is left able to hide behind one.
    CHECK(blob.seamSearch == 0u);
    CHECK(blob.gainMatch == 0u);
    CHECK(static_cast<double>(blob.exposureStops) == Catch::Approx(-2.5));
    CHECK(blob.photoSeamMode() == PrefsPhotoSeam::Off);          // [WP-PHOTO]
    CHECK(blob.photoStrengthPercent() == Catch::Approx(40.0));
    CHECK(blob.seamInsetDeg() == Catch::Approx(1.2));

    // And it is still a clean blob.
    CHECK(blob.isValid());
    PrefsBlob copy = blob;
    CHECK(copy.sanitise() == true);
}

// ===========================================================================
//  TRANSLATE_PARAMS_TO_PREFS - the defensive paths
// ===========================================================================

TEST_CASE("TRANSLATE_PARAMS_TO_PREFS refuses a buffer that is too small",
          "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    PrefsBuffer buffer;

    // A project saved by an older build can make the host's idea of the prefs
    // size disagree with ours.  Writing 128 bytes into a smaller buffer is a
    // heap overflow in the HOST's allocator, which is both a crash and a
    // security problem, so the write must be refused outright.
    PF_TranslateParamsToPrefsExtra extra = buffer.extra(static_cast<A_u_long>(PrefsBlob::kSize) - 1u);
    CHECK(fixture.send(PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, &extra) == PF_Err_NONE);
    CHECK(buffer.untouched());
}

TEST_CASE("TRANSLATE_PARAMS_TO_PREFS accepts a buffer that is larger", "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    PrefsBuffer buffer;

    // A larger buffer is not an error: only the first kSize bytes are ours,
    // and PrefsBlob::fromBytes on the importer side reads exactly that many.
    PF_TranslateParamsToPrefsExtra extra = buffer.extra(static_cast<A_u_long>(buffer.bytes.size()));
    CHECK(fixture.send(PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, &extra) == PF_Err_NONE);
    CHECK(buffer.blob().isValid());
    CHECK(buffer.tailUntouched());
}

TEST_CASE("TRANSLATE_PARAMS_TO_PREFS survives a null extra and a null buffer",
          "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // Leaving the importer on its existing prefs is correct; writing through
    // the null would take the host down.
    CHECK(fixture.send(PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, nullptr) == PF_Err_NONE);

    PF_TranslateParamsToPrefsExtra extra{};
    extra.prefsPC = nullptr;
    extra.prefs_sizeLu = static_cast<A_u_long>(PrefsBlob::kSize);
    CHECK(fixture.send(PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, &extra) == PF_Err_NONE);
}

TEST_CASE("a hostile popup value still produces a valid blob", "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // 0 is what a freshly deserialised project with a missing parameter can
    // produce, and it is the value an unguarded -1 would turn into 255 (or
    // into an unsigned underflow).  A large value and a negative one cover
    // the other directions.
    for (const int hostile : {0, -1, -12345, 99, 1000000}) {
        PrefsBuffer buffer;
        fixture.setPopup(kIndexStabilization, hostile);
        INFO("hostile popup value " << hostile);
        const PrefsBlob blob = translate(fixture, buffer);
        CHECK(blob.isValid());
        // It falls back to the DEFAULT rather than to enum value 0: a corrupt
        // control should land where a fresh one would, not on "Off".
        CHECK(blob.stabilization == PrefsBlob::defaults().stabilization);
        PrefsBlob copy = blob;
        CHECK(copy.sanitise() == true);
    }
    // [WP-PHOTO] The same for the Sky Seam Fix popup: the default mode.
    for (const int hostile : {0, -1, 4, 99}) {
        PrefsBuffer buffer;
        fixture.setPopup(kIndexPhotoSeam, hostile);
        INFO("hostile Sky Seam Fix value " << hostile);
        const PrefsBlob blob = translate(fixture, buffer);
        CHECK(blob.photoSeam == PrefsBlob::defaults().photoSeam);
        PrefsBlob copy = blob;
        CHECK(copy.sanitise() == true);
    }
}

TEST_CASE("out-of-range or non-finite sky seam sliders land on a stored value", "[sourcesettings][prefs][photoseam]") {
    // [WP-PHOTO] An expression or a corrupt project can put anything in a
    // float slider.  Beyond the range clamps to it; NaN, the infinities and
    // negatives fall back to the default (the blob setters' rule).
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());
    const PrefsBlob defaults = PrefsBlob::defaults();
    const struct {
        double strength;
        double inset;
        double wantStrength;
        double wantInset;
    } cases[] = {
        {250.0, 40.0, 100.0, 6.0},
        {-5.0, -1.0, defaults.photoStrengthPercent(), defaults.seamInsetDeg()},
        {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN(),
         defaults.photoStrengthPercent(), defaults.seamInsetDeg()},
        {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
         defaults.photoStrengthPercent(), defaults.seamInsetDeg()},
    };
    for (const auto& c : cases) {
        PrefsBuffer buffer;
        fixture.setSlider(kIndexPhotoStrength, c.strength);
        fixture.setSlider(kIndexSeamInset, c.inset);
        INFO("strength " << c.strength << ", inset " << c.inset);
        PrefsBlob blob = translate(fixture, buffer);
        CHECK(blob.isValid());
        CHECK(blob.photoStrengthPercent() == Catch::Approx(c.wantStrength));
        CHECK(blob.seamInsetDeg() == Catch::Approx(c.wantInset));
        CHECK(blob.sanitise() == true);
    }
}

TEST_CASE("an out-of-range or non-finite exposure is clamped or reset", "[sourcesettings][prefs]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    SECTION("beyond the valid range, clamped to it") {
        for (const double stops : {-100.0, 100.0}) {
            PrefsBuffer buffer;
            fixture.setSlider(kIndexExposure, stops);
            INFO("stops " << stops);
            const PrefsBlob blob = translate(fixture, buffer);
            CHECK(blob.exposureStops >= PrefsBlob::kMinExposureStops);
            CHECK(blob.exposureStops <= PrefsBlob::kMaxExposureStops);
        }
    }
    SECTION("NaN and the infinities, reset to zero") {
        for (const double stops : {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity()}) {
            PrefsBuffer buffer;
            fixture.setSlider(kIndexExposure, stops);
            const PrefsBlob blob = translate(fixture, buffer);
            CHECK(blob.isValid());
            // NaN compares false with every bound, so a field that survived
            // as NaN would poison the cache key and every render.
            CHECK(blob.exposureStops == blob.exposureStops);  // i.e. not NaN
            CHECK(blob.exposureStops >= PrefsBlob::kMinExposureStops);
            CHECK(blob.exposureStops <= PrefsBlob::kMaxExposureStops);
        }
    }
}

// ===========================================================================
//  SEQUENCE_SETUP - seeding the controls from the media
// ===========================================================================

TEST_CASE("SEQUENCE_SETUP asks the importer and seeds the controls from the answer",
          "[sourcesettings][sequence]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // Stand in for an importer whose clip is being decoded at 4K HLG with
    // stabilisation off - nothing like the defaults, so a control that failed
    // to update is unmistakable.
    PrefsBlob fromImporter = PrefsBlob::defaults();
    fromImporter.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
    fromImporter.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::UHD4K);
    fromImporter.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
    fromImporter.seamSearch = 0;
    fromImporter.gainMatch = 0;
    fromImporter.calibration = static_cast<std::uint8_t>(PrefsCalibration::Underwater);
    fromImporter.dlogmFit = static_cast<std::uint8_t>(PrefsDlogmFit::Pocket3);
    fromImporter.exposureStops = 1.5f;
    fromImporter.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Cuda);
    fromImporter.directColour = static_cast<std::uint8_t>(PrefsDirectColour::MatchSource);  // not the default
    fromImporter.flareRemoval = 0;  // [WP-FLARE] not the default
    fromImporter.photoSeam = static_cast<std::uint8_t>(PrefsPhotoSeam::RimOnly);  // [WP-PHOTO] none the default
    fromImporter.setPhotoStrengthPercent(65.0);
    fromImporter.setSeamInsetDeg(0.8);
    REQUIRE(fromImporter.sanitise());

    const char* raw = reinterpret_cast<const char*>(&fromImporter);
    fixture.host().setSourceSettingsReply(fixture.ref(), std::vector<char>(raw, raw + PrefsBlob::kSize));

    REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);

    // The effect must have actually made the call - a handler that quietly
    // did nothing would pass every value assertion below if the controls
    // happened to match.
    CHECK(fixture.host().sourceSettingsCallCount(fixture.ref()) == 1u);
    CHECK(fixture.host().sourceSettingsSentSize(fixture.ref()) == PrefsBlob::kSize);

    // And it must have seeded the buffer with a VALID blob on the way out, so
    // an importer that chooses to accept rather than override receives
    // something usable instead of zeros.
    const std::vector<char> sent = fixture.host().sourceSettingsSentData(fixture.ref());
    REQUIRE(sent.size() == PrefsBlob::kSize);
    PrefsBlob outbound{};
    std::memcpy(&outbound, sent.data(), PrefsBlob::kSize);
    CHECK(outbound.isValid());

    // Now the controls: 1-based popup values of the 0-based blob fields.
    CHECK(fixture.popup(kIndexColorOutput) == static_cast<int>(fromImporter.colorOutput) + 1);
    CHECK(fixture.popup(kIndexOutputSize) == static_cast<int>(fromImporter.outputSize) + 1);
    CHECK(fixture.popup(kIndexStabilization) == static_cast<int>(fromImporter.stabilization) + 1);
    CHECK(fixture.checkbox(kIndexSeamSearch) == false);
    CHECK(fixture.checkbox(kIndexGainMatch) == false);
    CHECK(fixture.popup(kIndexCalibration) == static_cast<int>(fromImporter.calibration) + 1);
    CHECK(fixture.popup(kIndexDlogmFit) == static_cast<int>(fromImporter.dlogmFit) + 1);
    CHECK(fixture.slider(kIndexExposure) == Catch::Approx(1.5));
    CHECK(fixture.popup(kIndexRenderDevice) == static_cast<int>(fromImporter.renderDevice) + 1);
    CHECK(fixture.popup(kIndexDirectColour) == static_cast<int>(fromImporter.directColour) + 1);
    CHECK(fixture.checkbox(kIndexFlareRemoval) == false);  // [WP-FLARE]
    CHECK(fixture.popup(kIndexPhotoSeam) == static_cast<int>(PrefsPhotoSeam::RimOnly) + 1);  // [WP-PHOTO]
    CHECK(fixture.slider(kIndexPhotoStrength) == Catch::Approx(65.0));
    CHECK(fixture.slider(kIndexSeamInset) == Catch::Approx(0.8));

    // A round trip proves the seeding and the translation agree: translating
    // the seeded controls must reproduce the importer's blob exactly.
    PrefsBuffer buffer;
    CHECK(translate(fixture, buffer) == fromImporter);
}

TEST_CASE("SEQUENCE_SETUP leaves the controls alone when the importer says nothing",
          "[sourcesettings][sequence]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // No reply configured: the mock leaves the buffer as the effect passed
    // it, which is what a real host does when there is no live clip instance.
    // The effect must then keep whatever the project stored - overwriting
    // with defaults here would reset every control of every clip on every
    // project open.
    fixture.setPopup(kIndexOutputSize, OSV_SS_SIZE_COUNT);
    fixture.setPopup(kIndexColorOutput, OSV_SS_COLOR_COUNT);
    const int sizeBefore = fixture.popup(kIndexOutputSize);
    const int colorBefore = fixture.popup(kIndexColorOutput);

    REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);

    CHECK(fixture.host().sourceSettingsCallCount(fixture.ref()) == 1u);
    CHECK(fixture.popup(kIndexOutputSize) == sizeBefore);
    CHECK(fixture.popup(kIndexColorOutput) == colorBefore);
}

TEST_CASE("SEQUENCE_SETUP ignores a failed host call", "[sourcesettings][sequence]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // A failing host call leaves the buffer in an undefined state.  An effect
    // that read it anyway would seed its controls from garbage, so the
    // configured reply is deliberately set AS WELL - if the effect reads past
    // the error it will pick the reply up and the assertions below will fail.
    PrefsBlob poison = PrefsBlob::defaults();
    poison.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    const char* raw = reinterpret_cast<const char*>(&poison);
    fixture.host().setSourceSettingsReply(fixture.ref(), std::vector<char>(raw, raw + PrefsBlob::kSize));
    fixture.host().setSourceSettingsError(fixture.ref(), PF_Err_INTERNAL_STRUCT_DAMAGED);

    fixture.setPopup(kIndexOutputSize, OSV_SS_SIZE_COUNT);
    const int before = fixture.popup(kIndexOutputSize);

    // The selector itself must still succeed: a source settings effect that
    // failed SEQUENCE_SETUP would make the whole sequence unusable over a
    // failure to read an optional convenience.
    REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
    CHECK(fixture.popup(kIndexOutputSize) == before);
}

TEST_CASE("SEQUENCE_SETUP rejects a reply that is not one of our blobs",
          "[sourcesettings][sequence]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // A stale payload from another importer, or from an older build with a
    // different version word, must not be trusted: it would silently
    // reinterpret whatever bytes happen to be there as stitch settings.
    std::vector<char> garbage(PrefsBlob::kSize, '\x5A');
    fixture.host().setSourceSettingsReply(fixture.ref(), garbage);

    fixture.setPopup(kIndexOutputSize, OSV_SS_SIZE_COUNT);
    const int before = fixture.popup(kIndexOutputSize);

    REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
    CHECK(fixture.popup(kIndexOutputSize) == before);
}

TEST_CASE("SEQUENCE_SETUP never allocates sequence_data", "[sourcesettings][sequence]") {
    EffectFixture fixture;
    REQUIRE(LoadedPlugin::instance().ok());

    // The effect keeps no per-instance state, so a non-null sequence_data
    // would be a pointer the host later tries to flatten and free without
    // anyone having allocated it.
    for (const PF_Cmd cmd : {PF_Cmd_SEQUENCE_SETUP, PF_Cmd_SEQUENCE_RESETUP, PF_Cmd_SEQUENCE_FLATTEN,
                             PF_Cmd_SEQUENCE_SETDOWN}) {
        PF_OutData out = fixture.host().makeOutData();
        out.sequence_data = reinterpret_cast<PF_Handle>(static_cast<std::uintptr_t>(0xDEADBEEF));
        INFO("selector " << static_cast<int>(cmd));
        CHECK(fixture.send(cmd, nullptr, &out) == PF_Err_NONE);
        CHECK(out.sequence_data == nullptr);
    }
}

TEST_CASE("SEQUENCE_SETUP does not call the importer outside Premiere",
          "[sourcesettings][sequence]") {
    // After Effects has no importers and no master clips, so the call would
    // be against a host that cannot serve it.
    EffectFixture fixture('FXTC');
    REQUIRE(LoadedPlugin::instance().ok());
    REQUIRE(fixture.send(PF_Cmd_SEQUENCE_SETUP, nullptr) == PF_Err_NONE);
    CHECK(fixture.host().sourceSettingsCallCount(fixture.ref()) == 0u);
}

// ===========================================================================
//  The pure mapping (SourceSettingsMapping.cpp, compiled in directly)
// ===========================================================================

TEST_CASE("the pure mapping round-trips every value of every field",
          "[sourcesettings][mapping]") {
    // The module test above proves the BUILT .aex behaves; this proves the
    // arithmetic underneath it exhaustively, including combinations a host
    // test would need one call each for.
    for (int color = 1; color <= OSV_SS_COLOR_COUNT; ++color) {
        for (int size = 1; size <= OSV_SS_SIZE_COUNT; ++size) {
            for (int stab = 1; stab <= OSV_SS_STAB_COUNT; ++stab) {
                for (int device = 1; device <= OSV_SS_DEVICE_COUNT; ++device) {
                    ControlValues c;
                    c.colorOutput = color;
                    c.outputSize = size;
                    c.stabilization = stab;
                    c.renderDevice = device;
                    const PrefsBlob blob = prefsFromControls(c);
                    REQUIRE(blob.isValid());
                    const ControlValues back = controlsFromPrefs(blob);
                    CHECK(back.colorOutput == color);
                    CHECK(back.outputSize == size);
                    CHECK(back.stabilization == stab);
                    CHECK(back.renderDevice == device);
                }
            }
        }
    }
    for (int calib = 1; calib <= OSV_SS_CALIB_COUNT; ++calib) {
        for (int fit = 1; fit <= OSV_SS_FIT_COUNT; ++fit) {
            for (const bool seam : {false, true}) {
                for (const bool gain : {false, true}) {
                    ControlValues c;
                    c.calibration = calib;
                    c.dlogmFit = fit;
                    c.seamSearch = seam;
                    c.gainMatch = gain;
                    const ControlValues back = controlsFromPrefs(prefsFromControls(c));
                    CHECK(back.calibration == calib);
                    CHECK(back.dlogmFit == fit);
                    CHECK(back.seamSearch == seam);
                    CHECK(back.gainMatch == gain);
                }
            }
        }
    }
}

TEST_CASE("the pure mapping round-trips Sun Ghost Removal", "[sourcesettings][mapping]") {
    // [WP-FLARE]
    for (const bool on : {false, true}) {
        ControlValues c;
        c.flareRemoval = on;
        const PrefsBlob blob = prefsFromControls(c);
        REQUIRE(blob.isValid());
        CHECK(blob.flareRemoval == (on ? 1u : 0u));
        CHECK(controlsFromPrefs(blob).flareRemoval == on);
    }
}

TEST_CASE("the pure mapping round-trips the sky seam fix", "[sourcesettings][mapping][photoseam]") {
    // [WP-PHOTO] Every mode x a spread of strengths and insets, both ways.
    for (int item = 1; item <= OSV_SS_PHOTO_SEAM_COUNT; ++item) {
        for (const double percent : {0.0, 1.0, 50.0, 99.0, 100.0}) {
            for (const double deg : {0.0, 0.1, 2.6, 3.7, 6.0}) {
                ControlValues c;
                c.photoSeam = item;
                c.photoStrengthPercent = percent;
                c.seamInsetDeg = deg;
                const PrefsBlob blob = prefsFromControls(c);
                INFO("mode " << item << ", strength " << percent << ", inset " << deg);
                REQUIRE(blob.isValid());
                CHECK(blob.photoSeam == static_cast<std::uint8_t>(item - 1));
                const ControlValues back = controlsFromPrefs(blob);
                CHECK(back.photoSeam == item);
                CHECK(back.photoStrengthPercent == Catch::Approx(percent));
                CHECK(back.seamInsetDeg == Catch::Approx(deg));
            }
        }
    }
    // Hostile popup values: the default mode, never a blob needing repair.
    for (const int hostile : {std::numeric_limits<int>::min(), -1, 0, OSV_SS_PHOTO_SEAM_COUNT + 1, 99}) {
        ControlValues c;
        c.photoSeam = hostile;
        PrefsBlob blob = prefsFromControls(c);
        CHECK(blob.photoSeamMode() == PrefsPhotoSeam::RimAndGain);
        CHECK(blob.sanitise());
    }
    // An older project's blob (zero bytes) shows Off at 100 % and the 2.6 deg
    // inset - what it renders with.
    PrefsBlob old = PrefsBlob::defaults();
    old.photoSeam = 0;
    old.photoStrength = 0;
    old.seamInset = 0;
    const ControlValues shown = controlsFromPrefs(old);
    CHECK(shown.photoSeam == 1);
    CHECK(shown.photoStrengthPercent == Catch::Approx(100.0));
    CHECK(shown.seamInsetDeg == Catch::Approx(2.6));
}

TEST_CASE("the pure mapping round-trips the Program Monitor Colour choice", "[sourcesettings][mapping]") {
    // [WP-SETTINGS]
    for (int item = 1; item <= OSV_SS_DIRECT_COLOUR_COUNT; ++item) {
        ControlValues c;
        c.directColour = item;
        const PrefsBlob blob = prefsFromControls(c);
        REQUIRE(blob.isValid());
        CHECK(blob.directColour == static_cast<std::uint8_t>(item - 1));
        CHECK(controlsFromPrefs(blob).directColour == item);
    }
}

TEST_CASE("the pure mapping round-trips the Rec.709 look choice", "[sourcesettings][mapping][look]") {
    // [WP-LOOK] Every item, both directions; hostile popup values fall back
    // to the default look rather than producing a blob that needs repair.
    for (int item = 1; item <= OSV_SS_LOOK_COUNT; ++item) {
        ControlValues c;
        c.rec709Look = item;
        const PrefsBlob blob = prefsFromControls(c);
        REQUIRE(blob.isValid());
        CHECK(blob.look == static_cast<std::uint8_t>(item - 1));
        CHECK(controlsFromPrefs(blob).rec709Look == item);
    }
    for (const int hostile : {std::numeric_limits<int>::min(), -1, 0, OSV_SS_LOOK_COUNT + 1, 99}) {
        ControlValues c;
        c.rec709Look = hostile;
        PrefsBlob blob = prefsFromControls(c);
        CHECK(blob.lookChoice() == PrefsLook::DjiStudio);
        CHECK(blob.sanitise());
    }
    // An older project's blob (zero byte) shows as "DJI (default)".
    PrefsBlob old = PrefsBlob::defaults();
    old.look = 0;
    CHECK(controlsFromPrefs(old).rec709Look == OSV_SS_LOOK_DEFAULT);
}

TEST_CASE("the pure mapping's defaults are the blob's defaults", "[sourcesettings][mapping]") {
    // A default-constructed ControlValues is what the header's defaults say;
    // translating it must give exactly PrefsBlob::defaults().
    CHECK(prefsFromControls(ControlValues{}) == PrefsBlob::defaults());

    // And the inverse: the default blob must give back the header's defaults.
    const ControlValues c = controlsFromPrefs(PrefsBlob::defaults());
    CHECK(c.colorOutput == OSV_SS_COLOR_DEFAULT);
    CHECK(c.outputSize == OSV_SS_SIZE_DEFAULT);
    CHECK(c.stabilization == OSV_SS_STAB_DEFAULT);
    CHECK(c.calibration == OSV_SS_CALIB_DEFAULT);
    CHECK(c.dlogmFit == OSV_SS_FIT_DEFAULT);
    CHECK(c.renderDevice == OSV_SS_DEVICE_DEFAULT);
    CHECK(c.directColour == OSV_SS_DIRECT_COLOUR_DEFAULT);
    CHECK(c.rec709Look == OSV_SS_LOOK_DEFAULT);  // [WP-LOOK]
    CHECK(c.seamSearch == (OSV_SS_SEAM_SEARCH_DEFAULT != 0));
    CHECK(c.gainMatch == (OSV_SS_GAIN_MATCH_DEFAULT != 0));
    CHECK(c.flareRemoval == (OSV_SS_FLARE_REMOVAL_DEFAULT != 0));  // [WP-FLARE]
    CHECK(c.photoSeam == OSV_SS_PHOTO_SEAM_DEFAULT);                   // [WP-PHOTO]
    CHECK(c.photoStrengthPercent == Catch::Approx(OSV_SS_PHOTO_STRENGTH_DEFAULT));
    CHECK(c.seamInsetDeg == Catch::Approx(OSV_SS_SEAM_INSET_DEFAULT));
    CHECK(c.exposureStops == Catch::Approx(OSV_SS_EXPOSURE_DEFAULT));
}

TEST_CASE("the pure mapping rejects an invalid blob", "[sourcesettings][mapping]") {
    // A blob with the wrong magic is not ours, and reinterpreting its bytes
    // as settings is how a stale payload silently changes how a clip decodes.
    PrefsBlob bad = PrefsBlob::defaults();
    bad.magic = 0xDEADBEEFu;
    const ControlValues c = controlsFromPrefs(bad);
    const ControlValues expected = controlsFromPrefs(PrefsBlob::defaults());
    CHECK(c.colorOutput == expected.colorOutput);
    CHECK(c.outputSize == expected.outputSize);
    CHECK(c.stabilization == expected.stabilization);

    // Same for a wrong version word.
    PrefsBlob oldVersion = PrefsBlob::defaults();
    oldVersion.version = PrefsBlob::kVersion + 1u;
    CHECK(controlsFromPrefs(oldVersion).outputSize == expected.outputSize);
}

TEST_CASE("the pure mapping never produces a blob that needs repair",
          "[sourcesettings][mapping]") {
    // Whatever goes in, what comes out satisfies isValid() and sanitises to
    // itself.  That property is what lets the importer treat a translated
    // blob exactly like one the modal dialog wrote.
    const int hostile[] = {std::numeric_limits<int>::min(), -1, 0, 1, 7, 99, std::numeric_limits<int>::max()};
    for (const int v : hostile) {
        ControlValues c;
        c.colorOutput = v;
        c.outputSize = v;
        c.stabilization = v;
        c.calibration = v;
        c.dlogmFit = v;
        c.renderDevice = v;
        c.directColour = v;
        c.photoSeam = v;                                // [WP-PHOTO]
        c.photoStrengthPercent = static_cast<double>(v);
        c.seamInsetDeg = static_cast<double>(v);
        c.exposureStops = static_cast<double>(v);
        PrefsBlob blob = prefsFromControls(c);
        INFO("hostile control value " << v);
        CHECK(blob.isValid());
        CHECK(blob.sanitise() == true);
        // And the reserved bytes really are zero: they take part in the PPix
        // cache key, so a stray byte would miss the cache on every frame.
        for (const std::uint8_t b : blob.reserved) {
            CHECK(b == 0u);
        }
    }
}
