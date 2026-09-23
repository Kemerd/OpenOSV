// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests of the calibration choice: CalibrationSelector::inventory() (which
// sets a clip holds, which are zero-filled placeholders, how they differ from
// native), CalibrationSelector::choose() (Auto follows the recorded
// accessory, forced choices fall back with a reason, a copy of native is
// called out) and FormatDetector's recorded-accessory flag.
//
// The synthetic cases build StreamMeta by hand, so they run everywhere; the
// [sample] cases pin what the real clip holds and SKIP without it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetadataTrack.h"

#include <cmath>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::meta;
using Catch::Matchers::WithinAbs;

namespace {

/// A complete, plausible lens record (hasCore() is true).  `yawDeg`
/// rotates the extrinsic about the optical axis so two records can differ
/// by a known angle.
DewarpParams lens(float fx, float cx = 1917.0f, double yawDeg = 0.0) {
    DewarpParams d;
    d.fx = fx;
    d.fy = fx;
    d.cx = cx;
    d.cy = 1919.0f;
    d.k = {0.066f, -0.0128f, 0.0103f, -0.0067f, 0.00098f, 0.0f, 0.0f, 0.0f, 0.0f};
    d.width = 3840;
    d.height = 3840;
    d.lensModel = 8.0f;
    d.temperature = -1000.0f;
    const double h = 0.5 * yawDeg * 3.14159265358979323846 / 180.0;
    d.camExtriQ.w = static_cast<float>(std::cos(h));
    d.camExtriQ.z = static_cast<float>(std::sin(h));
    d.camExtriQ.present = true;
    d.occlusionPtX = {1920.0f, 318.0f, 3522.0f};
    d.occlusionPtY = {3735.0f, 2845.0f, 2845.0f};
    return d;
}

/// A zero-filled placeholder record, exactly what the Osmo 360 writes for a
/// set it has not got (repeated fields present but all zero).
DewarpParams placeholder() {
    DewarpParams d;
    d.p = {0.0f, 0.0f};
    d.q = {0.0f, 0.0f, 0.0f, 0.0f};
    d.occlusionPtX.assign(14, 0.0f);
    d.occlusionPtY.assign(14, 0.0f);
    d.tangentCoeff = {0.0f, 0.0f};
    return d;
}

/// A stream shaped like the sample clip: refined + raw native pairs, the
/// four accessory pairs as placeholders, lens mode recorded as native.
StreamMeta sampleLikeStream() {
    StreamMeta s;
    using P = PanoDewarpParams;
    s.dewarp.byField[P::NativeRefineSlave] = lens(1043.88f);
    s.dewarp.byField[P::NativeRefineMaster] = lens(1043.01f, 1908.8f);
    s.dewarp.byField[P::NativeSlave] = lens(1042.22f, 1915.42f);
    s.dewarp.byField[P::NativeMaster] = lens(1037.85f, 1909.46f);
    for (std::uint32_t slot = P::NativeRefineFarSlave; slot <= P::WaterUnderMaster; ++slot) {
        s.dewarp.byField[slot] = placeholder();
    }
    s.extriLensMode = ExtriLensMode::Native;
    s.present.set(6);
    s.present.set(7);  // recorded (an empty field 7 on the wire = native)
    return s;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Names and parsing
// -----------------------------------------------------------------------------

TEST_CASE("calibration choice tokens round trip and set names follow the proto", "[meta][calibration]") {
    for (std::uint8_t c = 0; c < static_cast<std::uint8_t>(CalibrationChoice::Count); ++c) {
        const auto choice = static_cast<CalibrationChoice>(c);
        const auto parsed = parseCalibrationChoice(calibrationChoiceName(choice));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == choice);
    }
    CHECK(parseCalibrationChoice("Lens Guards") == CalibrationChoice::LensGuards);
    CHECK(parseCalibrationChoice("lens_guards") == CalibrationChoice::LensGuards);
    CHECK(parseCalibrationChoice("AUTO") == CalibrationChoice::Auto);
    CHECK_FALSE(parseCalibrationChoice("").has_value());
    CHECK_FALSE(parseCalibrationChoice("nd").has_value());
    CHECK(std::string(calibrationChoiceName(CalibrationChoice::Count)) == "unknown");

    // Set i lives in slots 2i+1 / 2i+2 and is named after the proto field.
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(CalibrationSetId::Count); ++i) {
        const auto id = static_cast<CalibrationSetId>(i);
        const std::string slave = PanoDewarpParams::fieldName(calibrationSetSlaveSlot(id));
        const std::string master = PanoDewarpParams::fieldName(calibrationSetMasterSlot(id));
        INFO("set " << static_cast<int>(i));
        CHECK(slave == std::string(calibrationSetName(id)) + "_slave");
        CHECK(master == std::string(calibrationSetName(id)) + "_master");
    }
    CHECK(calibrationSetSlaveSlot(CalibrationSetId::Count) == 0);
    CHECK(std::string(calibrationSetName(CalibrationSetId::Count)) == "unknown");
}

// -----------------------------------------------------------------------------
//  Inventory
// -----------------------------------------------------------------------------

TEST_CASE("inventory tells absent, placeholder, partial and usable sets apart", "[meta][calibration]") {
    StreamMeta s = sampleLikeStream();
    using P = PanoDewarpParams;
    // Far presets: 0.7 m usable, 0.9 m slave only with a zero focal (partial),
    // the rest absent.
    s.dewarp.byField[P::Far07Slave] = lens(1042.24f);
    s.dewarp.byField[P::Far07Master] = lens(1041.42f, 1908.8f);
    DewarpParams broken = lens(1041.78f);
    broken.fx = 0.0f;
    s.dewarp.byField[P::Far09Slave] = broken;

    const CalibrationInventory inv = CalibrationSelector::inventory(s);
    CHECK(inv.recordedModePresent);
    CHECK(inv.recordedMode == ExtriLensMode::Native);
    REQUIRE(inv.nativeReference.has_value());
    CHECK(*inv.nativeReference == CalibrationSetId::NativeRefine);

    CHECK(inv.at(CalibrationSetId::NativeRefine).state == CalibrationSetState::Usable);
    CHECK(inv.at(CalibrationSetId::Native).state == CalibrationSetState::Usable);
    CHECK(inv.at(CalibrationSetId::NativeRefineFar).state == CalibrationSetState::Empty);
    CHECK(inv.at(CalibrationSetId::LensGuards).state == CalibrationSetState::Empty);
    CHECK(inv.at(CalibrationSetId::WaterAbove).state == CalibrationSetState::Empty);
    CHECK(inv.at(CalibrationSetId::WaterUnder).state == CalibrationSetState::Empty);
    CHECK(inv.at(CalibrationSetId::Far07).state == CalibrationSetState::Usable);
    CHECK(inv.at(CalibrationSetId::Far09).state == CalibrationSetState::Partial);
    CHECK(inv.at(CalibrationSetId::Far11).state == CalibrationSetState::Absent);
    CHECK(inv.usableFarPresets() == 1);

    // The reference compares identical to itself; another set does not.
    REQUIRE(inv.at(CalibrationSetId::NativeRefine).vsNative.has_value());
    CHECK(inv.at(CalibrationSetId::NativeRefine).vsNative->identical);
    REQUIRE(inv.at(CalibrationSetId::Native).vsNative.has_value());
    const CalibrationDelta& raw = *inv.at(CalibrationSetId::Native).vsNative;
    CHECK_FALSE(raw.identical);
    CHECK_THAT(raw.focalPx, WithinAbs(1043.01 - 1037.85, 1e-3));  // master lens is the worse one
    CHECK_THAT(raw.centrePx, WithinAbs(1.58, 1e-3));              // slave |1917 - 1915.42| beats master 0.66
    // Placeholders carry no comparison.
    CHECK_FALSE(inv.at(CalibrationSetId::LensGuards).vsNative.has_value());

    // Forced accessory choices on this clip cannot change the stitch.
    CHECK(inv.choiceSetState(CalibrationChoice::LensGuards) == CalibrationSetState::Empty);
    CHECK(inv.choiceSetState(CalibrationChoice::Underwater) == CalibrationSetState::Empty);
    // Lens guards still change it: without a set they apply the protector
    // field-angle correction to native.  Underwater has no such fallback.
    CHECK(inv.choiceChangesStitch(CalibrationChoice::LensGuards));
    CHECK_FALSE(inv.choiceChangesStitch(CalibrationChoice::Underwater));
    CHECK(inv.choiceChangesStitch(CalibrationChoice::Auto));
    CHECK(inv.choiceChangesStitch(CalibrationChoice::Native));

    SECTION("an absent record is not a placeholder") {
        s.dewarp.byField[P::LensGuardsSlave].reset();
        s.dewarp.byField[P::LensGuardsMaster].reset();
        CHECK(CalibrationSelector::inventory(s).at(CalibrationSetId::LensGuards).state == CalibrationSetState::Absent);
    }
    SECTION("a stream without calibration yields an all-absent inventory") {
        const CalibrationInventory none = CalibrationSelector::inventory(StreamMeta{});
        CHECK_FALSE(none.nativeReference.has_value());
        CHECK_FALSE(none.recordedModePresent);
        for (const CalibrationSetInfo& info : none.sets) {
            CHECK(info.state == CalibrationSetState::Absent);
        }
        CHECK(none.choiceSetState(CalibrationChoice::Native) == CalibrationSetState::Absent);
    }
}

TEST_CASE("compare measures rotation precisely and identity bit for bit", "[meta][calibration]") {
    CalibrationSet a;
    a.slave = lens(1043.0f);
    a.master = lens(1043.0f);
    CalibrationSet b = a;
    CHECK(CalibrationSelector::compare(a, b).identical);

    // A 0.02 degree extrinsic difference - the size of the real far-preset
    // deltas - must read as 0.02, not as 0 (acos of a dot product would).
    b.master = lens(1043.0f, 1917.0f, 0.02);
    const CalibrationDelta d = CalibrationSelector::compare(a, b);
    CHECK_FALSE(d.identical);
    CHECK_THAT(d.rotationDeg, WithinAbs(0.02, 1e-4));
    CHECK(d.focalPx == 0.0);

    // An occlusion arc difference alone breaks identity (it changes the mask).
    CalibrationSet c = a;
    c.slave.occlusionPtX[1] += 1.0f;
    CHECK_FALSE(CalibrationSelector::compare(a, c).identical);
}

// -----------------------------------------------------------------------------
//  choose
// -----------------------------------------------------------------------------

TEST_CASE("Auto follows the recorded accessory and forced choices explain themselves", "[meta][calibration]") {
    StreamMeta s = sampleLikeStream();
    using P = PanoDewarpParams;

    SECTION("a native-only clip: every choice stitches native_refine, and says why") {
        std::vector<std::string> warnings;
        Result<CalibrationSelection> autoSel = CalibrationSelector::choose(s, CalibrationChoice::Auto, {}, &warnings);
        REQUIRE(autoSel.ok());
        CHECK(autoSel.value().used == CalibrationSetId::NativeRefine);
        CHECK_FALSE(autoSel.value().fellBack);
        CHECK(autoSel.value().reason.find("recorded bare lenses") != std::string::npos);
        CHECK(warnings.empty());

        Result<CalibrationSelection> native = CalibrationSelector::choose(s, CalibrationChoice::Native);
        REQUIRE(native.ok());
        CHECK(native.value().used == CalibrationSetId::NativeRefine);
        CHECK(native.value().reason.find("native (forced)") != std::string::npos);

        warnings.clear();
        Result<CalibrationSelection> guards =
            CalibrationSelector::choose(s, CalibrationChoice::LensGuards, {}, &warnings);
        REQUIRE(guards.ok());
        CHECK(guards.value().used == CalibrationSetId::NativeRefine);
        CHECK(guards.value().fellBack);
        CHECK(guards.value().wanted == ExtriLensMode::LensGuards);
        // No dedicated set: native plus the protector correction, not "native".
        CHECK(guards.value().protectorCorrection);
        CHECK(guards.value().reason.find("no dedicated lens-guard calibration in this clip (slots 5/6 empty)") !=
              std::string::npos);
        CHECK(guards.value().reason.find("plus the lens-protector field-angle correction") != std::string::npos);
        CHECK(warnings.size() == 2);  // override note + fallback note, as select() always gave
        CHECK_FALSE(autoSel.value().protectorCorrection);
        CHECK_FALSE(native.value().protectorCorrection);

        Result<CalibrationSelection> water = CalibrationSelector::choose(s, CalibrationChoice::Underwater);
        REQUIRE(water.ok());
        CHECK(water.value().fellBack);
        CHECK_FALSE(water.value().protectorCorrection);
        CHECK(water.value().reason.find("no usable underwater calibration") != std::string::npos);
    }

    SECTION("a clip recorded with lens protectors: Auto uses them, Native overrides them") {
        s.extriLensMode = ExtriLensMode::LensGuards;
        s.dewarp.byField[P::LensGuardsSlave] = lens(1046.0f, 1918.0f);
        s.dewarp.byField[P::LensGuardsMaster] = lens(1045.2f, 1909.5f, 0.05);

        Result<CalibrationSelection> autoSel = CalibrationSelector::choose(s, CalibrationChoice::Auto);
        REQUIRE(autoSel.ok());
        CHECK(autoSel.value().used == CalibrationSetId::LensGuards);
        CHECK(autoSel.value().set.sourceSlave == "lens_guards_slave");
        CHECK(autoSel.value().set.sourceMaster == "lens_guards_master");
        CHECK_FALSE(autoSel.value().fellBack);
        CHECK_FALSE(autoSel.value().identicalToNative);
        // A genuine dedicated set is trusted as is: no correction on top.
        CHECK_FALSE(autoSel.value().protectorCorrection);
        CHECK(autoSel.value().reason.find("recorded lens guards") != std::string::npos);

        Result<CalibrationSelection> native = CalibrationSelector::choose(s, CalibrationChoice::Native);
        REQUIRE(native.ok());
        CHECK(native.value().used == CalibrationSetId::NativeRefine);
        CHECK(native.value().reason.find("overriding the recorded lens guards") != std::string::npos);

        const CalibrationInventory inv = CalibrationSelector::inventory(s);
        CHECK(inv.choiceChangesStitch(CalibrationChoice::LensGuards));
        CHECK(inv.at(CalibrationSetId::LensGuards).vsNative->rotationDeg > 0.04);
    }

    SECTION("a lens-guard set that is a copy of native is called out") {
        s.dewarp.byField[P::LensGuardsSlave] = s.dewarp.byField[P::NativeRefineSlave];
        s.dewarp.byField[P::LensGuardsMaster] = s.dewarp.byField[P::NativeRefineMaster];
        std::vector<std::string> warnings;
        Result<CalibrationSelection> guards =
            CalibrationSelector::choose(s, CalibrationChoice::LensGuards, {}, &warnings);
        REQUIRE(guards.ok());
        CHECK(guards.value().used == CalibrationSetId::LensGuards);
        CHECK_FALSE(guards.value().fellBack);
        CHECK(guards.value().identicalToNative);
        // A copy of native carries no protector information: the correction
        // is applied on top of it, exactly as with no set at all.
        CHECK(guards.value().protectorCorrection);
        CHECK(guards.value().reason.find("copy of native") != std::string::npos);
        CHECK(guards.value().reason.find("plus the lens-protector field-angle correction") != std::string::npos);
        CHECK(CalibrationSelector::inventory(s).choiceChangesStitch(CalibrationChoice::LensGuards));

        // A copy for UNDERWATER has no correction to fall back on and is
        // called out as unable to change anything.
        s.dewarp.byField[P::WaterUnderSlave] = s.dewarp.byField[P::NativeRefineSlave];
        s.dewarp.byField[P::WaterUnderMaster] = s.dewarp.byField[P::NativeRefineMaster];
        warnings.clear();
        Result<CalibrationSelection> water = CalibrationSelector::choose(s, CalibrationChoice::Underwater, {}, &warnings);
        REQUIRE(water.ok());
        CHECK(water.value().identicalToNative);
        CHECK(water.value().reason.find("identical to Native") != std::string::npos);
        REQUIRE_FALSE(warnings.empty());
        CHECK(warnings.back().find("numerically identical") != std::string::npos);
        CHECK_FALSE(CalibrationSelector::inventory(s).choiceChangesStitch(CalibrationChoice::Underwater));
    }

    SECTION("a clip that records no accessory is treated as bare lenses") {
        s.present.reset(7);
        Result<CalibrationSelection> autoSel = CalibrationSelector::choose(s, CalibrationChoice::Auto);
        REQUIRE(autoSel.ok());
        CHECK(autoSel.value().used == CalibrationSetId::NativeRefine);
        CHECK(autoSel.value().reason.find("does not record a lens accessory") != std::string::npos);
    }

    SECTION("underwater falls back to the above-water pair before native") {
        s.dewarp.byField[P::WaterAboveSlave] = lens(1050.0f);
        s.dewarp.byField[P::WaterAboveMaster] = lens(1049.0f, 1908.0f);
        Result<CalibrationSelection> water = CalibrationSelector::choose(s, CalibrationChoice::Underwater);
        REQUIRE(water.ok());
        CHECK(water.value().used == CalibrationSetId::WaterAbove);
        CHECK_FALSE(water.value().fellBack);
        CHECK(CalibrationSelector::inventory(s).choiceSetState(CalibrationChoice::Underwater) ==
              CalibrationSetState::Usable);
    }

    SECTION("a stitch distance still selects a far preset and says so") {
        s.dewarp.byField[P::Far11Slave] = lens(1041.31f);
        s.dewarp.byField[P::Far11Master] = lens(1040.51f, 1908.8f);
        CalibrationSelector::Options opt;
        opt.stitchDistanceM = 1.1;
        Result<CalibrationSelection> far = CalibrationSelector::choose(s, CalibrationChoice::Auto, opt);
        REQUIRE(far.ok());
        CHECK(far.value().used == CalibrationSetId::Far11);
        CHECK(far.value().reason.find("far_11") != std::string::npos);
    }

    SECTION("invalid input is an error, not a guess") {
        CHECK(CalibrationSelector::choose(s, CalibrationChoice::Count).code() == ErrorCode::InvalidArgument);
        CHECK(CalibrationSelector::choose(StreamMeta{}, CalibrationChoice::Auto).code() == ErrorCode::NotFound);
    }
}

// -----------------------------------------------------------------------------
//  The sample clip
// -----------------------------------------------------------------------------

TEST_CASE("the sample clip holds native and far sets only, recorded without lens protectors",
          "[meta][calibration][sample]") {
    OSV_REQUIRE_SAMPLE();
    Result<OsvFile> file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    Result<MetadataTrack> track = MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    const StreamMeta& s = track.value().stream();

    // ---- what the camera recorded -----------------------------------------
    // StreamMeta field 7 is on the wire (empty = EXTRI_LENS_MODE_NATIVE_REFINE),
    // i.e. the in-camera Lens Protection Mode was off.
    Result<FormatInfo> format = FormatDetector::detect(file.value(), &track.value());
    REQUIRE(format.ok());
    CHECK(format.value().lensMode == ExtriLensMode::Native);
    CHECK(format.value().lensModeFromMetadata);

    // ---- which sets exist ---------------------------------------------------
    const CalibrationInventory inv = CalibrationSelector::inventory(s);
    CHECK(inv.recordedModePresent);
    CHECK(inv.recordedMode == ExtriLensMode::Native);
    CHECK(inv.at(CalibrationSetId::NativeRefine).state == CalibrationSetState::Usable);
    CHECK(inv.at(CalibrationSetId::Native).state == CalibrationSetState::Usable);
    // Slots 3..10 are on the wire as 159-byte all-zero records.
    CHECK(inv.at(CalibrationSetId::NativeRefineFar).state == CalibrationSetState::Empty);
    CHECK(inv.at(CalibrationSetId::LensGuards).state == CalibrationSetState::Empty);
    CHECK(inv.at(CalibrationSetId::WaterAbove).state == CalibrationSetState::Empty);
    CHECK(inv.at(CalibrationSetId::WaterUnder).state == CalibrationSetState::Empty);
    CHECK(inv.usableFarPresets() == 6);

    // ---- how they differ (measured; calibration pixels) --------------------
    const CalibrationDelta& raw = *inv.at(CalibrationSetId::Native).vsNative;
    CHECK_THAT(raw.focalPx, WithinAbs(5.1603, 1e-3));   // master fx 1043.0103 vs 1037.85
    CHECK_THAT(raw.centrePx, WithinAbs(1.6221, 1e-3));  // slave cx 1917.0421 vs 1915.42
    CHECK(raw.distortion == 0.0);                       // the radial terms are shared
    CHECK_THAT(raw.rotationDeg, WithinAbs(0.097, 0.005));
    const CalibrationDelta& far16 = *inv.at(CalibrationSetId::Far16).vsNative;
    CHECK_THAT(far16.focalPx, WithinAbs(3.7253, 1e-3));
    CHECK(far16.distortion == 0.0);
    CHECK(far16.rotationDeg < 0.05);

    // ---- every choice: what it stitches with ------------------------------
    for (const CalibrationChoice choice : {CalibrationChoice::Auto, CalibrationChoice::Native,
                                           CalibrationChoice::LensGuards, CalibrationChoice::Underwater}) {
        Result<CalibrationSelection> sel = CalibrationSelector::choose(s, choice);
        REQUIRE(sel.ok());
        INFO(calibrationChoiceName(choice) << ": " << sel.value().reason);
        CHECK(sel.value().set.sourceSlave == "native_refine_slave");
        CHECK(sel.value().set.sourceMaster == "native_refine_master");
        const bool forcedAccessory = choice == CalibrationChoice::LensGuards || choice == CalibrationChoice::Underwater;
        CHECK(sel.value().fellBack == forcedAccessory);
        // Only the forced lens-guard choice brings the protector correction;
        // Auto on this bare-lens recording does not.
        CHECK(sel.value().protectorCorrection == (choice == CalibrationChoice::LensGuards));
    }
    CHECK(inv.choiceChangesStitch(CalibrationChoice::LensGuards));
    CHECK_FALSE(inv.choiceChangesStitch(CalibrationChoice::Underwater));
}
