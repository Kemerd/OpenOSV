// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests of the pure PrefsBlob <-> dialog-control mapping.
//
// This is the reason controlsFromPrefs / prefsFromControls exist as free
// functions instead of living inside the dialog procedure: a modal
// DialogBoxParamW cannot run inside a test (it pumps messages until a user
// clicks something), so the only way to prove the dialog writes the right
// bytes is to test the mapping it uses.
//
// The functions are linked from the plug-in's own source file, so what is
// tested is the code the .prm executes - not a copy.

#include <catch2/catch_test_macros.hpp>

#include "CalibrationUi.h"
#include "ImporterPlugin.h"
#include "PrefsBlob.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace osv::premiere;

TEST_CASE("the defaults round trip through the control mapping", "[importer][prefs][mapping]") {
    const PrefsBlob defaults = PrefsBlob::defaults();
    const DialogControls controls = controlsFromPrefs(defaults);

    SECTION("the controls show the documented defaults") {
        REQUIRE(controls.colorOutput == 0);     // PQ
        REQUIRE(controls.outputSize == static_cast<int>(PrefsOutputSize::Native));  // full sensor
        REQUIRE(controls.stabilization == 1);   // Horizon lock
        REQUIRE(controls.seamSearch == true);
        REQUIRE(controls.gainMatch == true);
        REQUIRE(controls.calibration == 0);     // Auto (follow the recorded accessory)
        REQUIRE(controls.dlogmFit == static_cast<int>(PrefsDlogmFit::Osmo360));
        REQUIRE(controls.exposureStops == 0.0);
        REQUIRE(controls.renderDevice == 0);    // Auto
        REQUIRE(controls.flareRemoval == true); // [WP-FLARE] on for new clips
    }

    SECTION("mapping back reproduces the blob byte for byte") {
        const PrefsBlob back = prefsFromControls(controls);
        REQUIRE(back == defaults);
        REQUIRE(std::memcmp(&back, &defaults, PrefsBlob::kSize) == 0);
    }
}

TEST_CASE("every field survives the round trip", "[importer][prefs][mapping]") {
    // Walk every value of every enum and a spread of exposures.  A field the
    // mapping forgot would show up as a value that does not come back.
    //
    // Nested loops rather than a GENERATE cross product on purpose: eight
    // generators would multiply out to ~1700 Catch2 test-case runs for a
    // mapping that has no state between iterations, which buys nothing and
    // makes the test listing unreadable.
    for (int color = 0; color < static_cast<int>(PrefsColorOutput::Count); ++color) {
        for (int size = 0; size < static_cast<int>(PrefsOutputSize::Count); ++size) {
            for (int stab = 0; stab < static_cast<int>(PrefsStabilization::Count); ++stab) {
                for (int calib = 0; calib < static_cast<int>(PrefsCalibration::Count); ++calib) {
                    for (int fit = 0; fit < static_cast<int>(PrefsDlogmFit::Count); ++fit) {
                        for (int device = 0; device < static_cast<int>(PrefsRenderDevice::Count); ++device) {
                            for (int seam = 0; seam < 2; ++seam) {
                                for (int gain = 0; gain < 2; ++gain) {
                                    for (const float exposure : {-6.0f, -1.25f, 0.0f, 0.5f, 3.0f, 6.0f}) {
                                      for (int flare = 0; flare < 2; ++flare) {  // [WP-FLARE]
                                        PrefsBlob original = PrefsBlob::defaults();
                                        original.colorOutput = static_cast<std::uint8_t>(color);
                                        original.outputSize = static_cast<std::uint8_t>(size);
                                        original.stabilization = static_cast<std::uint8_t>(stab);
                                        original.calibration = static_cast<std::uint8_t>(calib);
                                        original.dlogmFit = static_cast<std::uint8_t>(fit);
                                        original.renderDevice = static_cast<std::uint8_t>(device);
                                        original.seamSearch = static_cast<std::uint8_t>(seam);
                                        original.gainMatch = static_cast<std::uint8_t>(gain);
                                        original.exposureStops = exposure;
                                        original.flareRemoval = static_cast<std::uint8_t>(flare);

                                        const DialogControls controls = controlsFromPrefs(original);
                                        const PrefsBlob back = prefsFromControls(controls);

                                        INFO("colour " << color << " size " << size << " stab " << stab << " calib "
                                                       << calib << " fit " << fit << " device " << device << " seam "
                                                       << seam << " gain " << gain << " exposure " << exposure
                                                       << " flare " << flare);
                                        REQUIRE(back == original);
                                      }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("sun ghost removal is on for new clips and off for a blob saved before it existed",
          "[importer][prefs][mapping]") {
    // [WP-FLARE] The byte was carved out of the zero-filled reserved block, so
    // a project saved before it existed carries 0 there - which must read as
    // OFF (the project renders exactly as it did), while a new clip gets the
    // default ON.  The parallax rule, for the same reason.
    const PrefsBlob fresh = PrefsBlob::defaults();
    REQUIRE(fresh.flareRemoval == 1u);

    PrefsBlob modern = fresh;
    std::uint8_t bytes[PrefsBlob::kSize];
    std::memcpy(bytes, &modern, PrefsBlob::kSize);
    bytes[offsetof(PrefsBlob, flareRemoval)] = 0;
    const PrefsBlob old = PrefsBlob::fromBytes(bytes, sizeof(bytes));
    REQUIRE(old.flareRemoval == 0u);
    REQUIRE(controlsFromPrefs(old).flareRemoval == false);

    // The dialog's checkbox round-trips both ways and changes nothing else.
    DialogControls c = controlsFromPrefs(old);
    c.flareRemoval = true;
    PrefsBlob back = prefsFromControls(c, old);
    REQUIRE(back.flareRemoval == 1u);
    back.flareRemoval = 0;
    REQUIRE(back == old);

    // A corrupt byte lands on off, never on an undefined value.
    bytes[offsetof(PrefsBlob, flareRemoval)] = 0xFE;
    REQUIRE(PrefsBlob::fromBytes(bytes, sizeof(bytes)).flareRemoval == 0u);
}

TEST_CASE("out-of-range control values cannot produce an invalid blob",
          "[importer][prefs][mapping][defensive]") {
    // A combo box with no selection reports CB_ERR (-1); a corrupted dialog
    // state could report anything.  Neither may reach the renderer.
    DialogControls controls;
    controls.colorOutput = -1;
    controls.outputSize = 99;
    controls.stabilization = -7;
    controls.calibration = 1000;
    controls.dlogmFit = -1;
    controls.renderDevice = 42;
    controls.exposureStops = 0.0;

    const PrefsBlob blob = prefsFromControls(controls);
    REQUIRE(blob.isValid());

    // Each out-of-range value fell back to its documented default.
    REQUIRE(blob.colorOutput == 0);
    REQUIRE(blob.outputSize == 0);
    REQUIRE(blob.stabilization == 1);
    REQUIRE(blob.calibration == 0);
    REQUIRE(blob.dlogmFit == 0);
    REQUIRE(blob.renderDevice == 0);

    // And the blob is genuinely sane: sanitise() finds nothing to change.
    PrefsBlob copy = blob;
    REQUIRE(copy.sanitise() == true);
}

TEST_CASE("a hostile exposure value is clamped or zeroed", "[importer][prefs][mapping][defensive]") {
    // The edit box is free text: "1e999" parses to infinity, and a user can
    // type any magnitude.
    struct Case {
        double input;
        float expected;
    };
    const Case cases[] = {
        {1000.0, PrefsBlob::kMaxExposureStops},
        {-1000.0, PrefsBlob::kMinExposureStops},
        {std::numeric_limits<double>::infinity(), 0.0f},
        {-std::numeric_limits<double>::infinity(), 0.0f},
        {std::numeric_limits<double>::quiet_NaN(), 0.0f},
        {2.5, 2.5f},
    };
    for (const Case& c : cases) {
        DialogControls controls;
        controls.exposureStops = c.input;
        const PrefsBlob blob = prefsFromControls(controls);
        INFO("input " << c.input);
        REQUIRE(std::isfinite(blob.exposureStops));
        REQUIRE(blob.exposureStops == c.expected);
        REQUIRE(blob.exposureStops >= PrefsBlob::kMinExposureStops);
        REQUIRE(blob.exposureStops <= PrefsBlob::kMaxExposureStops);
    }
}

TEST_CASE("the colour-space token follows the colour output", "[importer][prefs][color]") {
    PrefsBlob blob = PrefsBlob::defaults();

    blob.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::PQ);
    REQUIRE(std::string(colorSpaceTokenFor(blob)) == "BT.2100 PQ RGB Full");
    blob.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
    REQUIRE(std::string(colorSpaceTokenFor(blob)) == "BT.2100 HLG RGB Full");
    blob.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    REQUIRE(std::string(colorSpaceTokenFor(blob)) == "BT.709 RGB Full");

    SECTION("D-Log M passthrough declares a scene-referred wide-gamut space") {
        blob.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::DLogM);

        // The SDK has no DJI D-Log M token, so this one is an approximation -
        // and the test pins WHICH approximation, because the wrong choice is
        // actively harmful rather than merely imprecise.
        REQUIRE(std::string(colorSpaceTokenFor(blob)) == "BT.2020 RGB Full (Scene)");

        // Scene-referred, not Display: a log signal is scene light, and the
        // Display variant would invite the host to tone-map it for the
        // monitor - exactly what a user who picked passthrough asked us not
        // to do.
        REQUIRE(std::string(colorSpaceTokenFor(blob)).find("(Scene)") != std::string::npos);

        // And above all NOT Rec.709.  Claiming BT.709 for log data is the one
        // genuinely damaging answer: the host would treat the flat log curve
        // as a finished Rec.709 image, so the Program Monitor would show
        // washed-out mush and a "Match Source" export would bake that
        // interpretation in.
        REQUIRE(std::string(colorSpaceTokenFor(blob)) != std::string(kPrOverranged709));
        REQUIRE(std::string(colorSpaceTokenFor(blob)) != std::string(kPrOverranged2100PQ));
        REQUIRE(std::string(colorSpaceTokenFor(blob)) != std::string(kPrOverranged2100HLG));

        // It is the ONLY output declared approximately; the other three are
        // exact, and the flag is what makes the log line honest.
        REQUIRE(colorSpaceIsApproximate(blob));
        for (const PrefsColorOutput exact :
             {PrefsColorOutput::PQ, PrefsColorOutput::HLG, PrefsColorOutput::Rec709}) {
            PrefsBlob other = PrefsBlob::defaults();
            other.colorOutput = static_cast<std::uint8_t>(exact);
            REQUIRE_FALSE(colorSpaceIsApproximate(other));
        }
    }

    SECTION("every colour output has a non-empty, distinct token") {
        // A new enum value that nobody wired into the switch would fall
        // through to the PQ default and be indistinguishable from PQ, which
        // is precisely the bug this catches.
        std::vector<std::string> tokens;
        for (int color = 0; color < static_cast<int>(PrefsColorOutput::Count); ++color) {
            PrefsBlob b = PrefsBlob::defaults();
            b.colorOutput = static_cast<std::uint8_t>(color);
            const char* token = colorSpaceTokenFor(b);
            INFO("colour output " << color);
            REQUIRE(token != nullptr);
            REQUIRE(std::string(token).length() > 0);
            tokens.emplace_back(token);
        }
        std::sort(tokens.begin(), tokens.end());
        REQUIRE(std::adjacent_find(tokens.begin(), tokens.end()) == tokens.end());
    }

    SECTION("the SEI codes describe the same three spaces") {
        blob.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::PQ);
        SeiCodes codes = seiCodesFor(blob);
        REQUIRE(codes.primaries == 9);   // BT.2020
        REQUIRE(codes.transfer == 16);   // BT.2100 PQ
        REQUIRE(codes.matrix == 0);      // identity: we emit RGB

        blob.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
        codes = seiCodesFor(blob);
        REQUIRE(codes.primaries == 9);
        REQUIRE(codes.transfer == 18);   // BT.2100 HLG

        blob.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
        codes = seiCodesFor(blob);
        REQUIRE(codes.primaries == 1);   // BT.709
        REQUIRE(codes.transfer == 1);
    }
}

// =============================================================================
//  Calibration: the combo lists the CHOICE, and the labels tell the truth
// =============================================================================

TEST_CASE("the calibration combo maps to the choice and old blobs read as Auto", "[importer][prefs][mapping]") {
    // Combo index == PrefsCalibrationChoice: Auto, Native, Lens protectors,
    // Underwater.  Each index must produce its canonical blob and come back.
    for (int index = 0; index < static_cast<int>(PrefsCalibrationChoice::Count); ++index) {
        DialogControls controls = controlsFromPrefs(PrefsBlob::defaults());
        controls.calibration = index;
        const PrefsBlob blob = prefsFromControls(controls);
        INFO("combo index " << index);
        REQUIRE(static_cast<int>(blob.calibrationChoice()) == index);
        REQUIRE(controlsFromPrefs(blob).calibration == index);
        PrefsBlob copy = blob;
        REQUIRE(copy.sanitise());  // canonical: nothing to fix
    }

    // A blob saved before the choice existed: calibration 0 with a zero
    // (formerly reserved) force byte.  It rendered by the recorded accessory,
    // and it shows - and stays - Auto.
    PrefsBlob old = PrefsBlob::defaults();
    old.calibration = 0;
    old.calibrationForceNative = 0;
    REQUIRE(controlsFromPrefs(old).calibration == static_cast<int>(PrefsCalibrationChoice::Auto));
    REQUIRE(prefsFromControls(controlsFromPrefs(old)) == old);

    // A forced Native and Auto are different blobs (different cache keys).
    DialogControls native = controlsFromPrefs(PrefsBlob::defaults());
    native.calibration = static_cast<int>(PrefsCalibrationChoice::Native);
    REQUIRE(prefsFromControls(native) != PrefsBlob::defaults());
    REQUIRE(prefsFromControls(native).calibration == 0);
    REQUIRE(prefsFromControls(native).calibrationForceNative == 1);
}

TEST_CASE("the calibration labels say which sets the clip holds", "[importer][prefs][mapping]") {
    constexpr auto kAuto = static_cast<std::size_t>(PrefsCalibrationChoice::Auto);
    constexpr auto kNative = static_cast<std::size_t>(PrefsCalibrationChoice::Native);
    constexpr auto kGuards = static_cast<std::size_t>(PrefsCalibrationChoice::LensGuards);
    constexpr auto kWater = static_cast<std::size_t>(PrefsCalibrationChoice::Underwater);

    SECTION("no clip facts: plain names") {
        const auto labels = calibrationChoiceLabels(CalibrationUiFacts{});
        REQUIRE(labels[kAuto] == L"Auto (follow the camera)");
        REQUIRE(labels[kNative] == L"Native (bare lenses)");
        REQUIRE(labels[kGuards] == L"Lens protectors / ND filters");
        REQUIRE(labels[kWater] == L"Underwater");
    }

    SECTION("the sample clip: recorded bare lenses, no accessory sets") {
        CalibrationUiFacts facts;
        facts.known = true;
        facts.recordedAccessory = 0;
        facts.underwater = CalibrationAvailability::Missing;
        const auto labels = calibrationChoiceLabels(facts);
        REQUIRE(labels[kAuto] == L"Auto (camera: no lens protectors)");
        // Lens protectors always do something (their own set or the
        // field-angle correction on native), so they are never marked.
        REQUIRE(labels[kGuards] == L"Lens protectors / ND filters");
        REQUIRE(labels[kWater] == L"Underwater (not in clip: Native)");
    }

    SECTION("a clip shot with lens protectors") {
        CalibrationUiFacts facts;
        facts.known = true;
        facts.recordedAccessory = 1;
        facts.underwater = CalibrationAvailability::Missing;
        const auto labels = calibrationChoiceLabels(facts);
        REQUIRE(labels[kAuto] == L"Auto (camera: lens protectors)");
        REQUIRE(labels[kGuards] == L"Lens protectors / ND filters");
        // A copy of native is marked as such - for underwater, which has no
        // correction to fall back on.
        facts.underwater = CalibrationAvailability::SameAsNative;
        REQUIRE(calibrationChoiceLabels(facts)[kWater] == L"Underwater (same as Native here)");
    }

    SECTION("nothing recorded, underwater recorded, unknown accessory") {
        CalibrationUiFacts facts;
        facts.known = true;
        facts.recordedAccessory = -1;
        REQUIRE(calibrationChoiceLabels(facts)[kAuto] == L"Auto (nothing recorded: Native)");
        facts.recordedAccessory = 2;
        facts.underwater = CalibrationAvailability::Usable;
        REQUIRE(calibrationChoiceLabels(facts)[kAuto] == L"Auto (camera: underwater)");
        facts.recordedAccessory = 9;
        REQUIRE(calibrationChoiceLabels(facts)[kAuto] == L"Auto (unknown accessory: Native)");
    }

    SECTION("every label fits the closed combo box") {
        // The combo is 179 dialog units wide: about 42 characters of the
        // dialog font.  A longer label is cut off exactly where it says
        // "Native", which is the part that matters.
        for (const int recorded : {-1, 0, 1, 2, 7}) {
            for (const CalibrationAvailability a :
                 {CalibrationAvailability::Unknown, CalibrationAvailability::Usable,
                  CalibrationAvailability::SameAsNative, CalibrationAvailability::Missing}) {
                CalibrationUiFacts facts;
                facts.known = true;
                facts.recordedAccessory = recorded;
                facts.underwater = a;
                for (const std::wstring& label : calibrationChoiceLabels(facts)) {
                    REQUIRE(!label.empty());
                    REQUIRE(label.size() <= 40u);
                }
            }
        }
    }
}

// =============================================================================
//  Fields the dialog does not show survive an OK
// =============================================================================

TEST_CASE("OK keeps every field the dialog does not show", "[importer][prefs][mapping][regression]") {
    // Regression: prefsFromControls used to start from defaults(), so
    // clicking OK in the Win32 dialog silently turned parallax back On and
    // the flow backend back to Auto - fields the dialog has no control for.
    PrefsBlob incoming = PrefsBlob::defaults();
    incoming.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    incoming.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Neural);
    incoming.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);

    SECTION("an unchanged OK returns the incoming blob byte for byte") {
        const PrefsBlob back = prefsFromControls(controlsFromPrefs(incoming), incoming);
        REQUIRE(back == incoming);
        REQUIRE(back.parallaxMode() == PrefsParallax::Off);
        REQUIRE(back.flow() == PrefsFlowBackend::Neural);
    }

    SECTION("a changed shown field changes only that field") {
        DialogControls c = controlsFromPrefs(incoming);
        c.colorOutput = static_cast<int>(PrefsColorOutput::Rec709);
        c.calibration = static_cast<int>(PrefsCalibrationChoice::LensGuards);
        const PrefsBlob back = prefsFromControls(c, incoming);
        REQUIRE(back.color() == PrefsColorOutput::Rec709);
        REQUIRE(back.calibrationChoice() == PrefsCalibrationChoice::LensGuards);
        REQUIRE(back.parallaxMode() == PrefsParallax::Off);        // hidden: kept
        REQUIRE(back.flow() == PrefsFlowBackend::Neural);          // hidden: kept
        REQUIRE(back.exposureStops == incoming.exposureStops);
    }

    SECTION("the one-argument form is still defaults-based") {
        // Kept for callers that have no incoming blob; documented behaviour.
        const PrefsBlob fresh = prefsFromControls(controlsFromPrefs(incoming));
        REQUIRE(fresh.parallaxMode() == PrefsParallax::On);
        REQUIRE(fresh.flow() == PrefsFlowBackend::Auto);
    }

    SECTION("every valid hidden byte survives, including offset 24 (WP-SETTINGS' Direct Path Colour)") {
        // Generic over the whole blob, so fields other packages append from
        // the reserved block (WP-SETTINGS at 24, WP-PHOTO at 32..37, ...) are
        // covered without this test knowing their names: for every byte the
        // dialog does not map, a value that the blob accepts as clean must
        // come back from an unchanged OK.  On a branch where an offset is
        // still reserved, sanitise() rejects a non-zero value there and the
        // byte is skipped; once the field exists it is checked.
        const DialogControls shown = controlsFromPrefs(incoming);
        int checked = 0;
        bool offset24Checked = false;
        for (std::size_t off = offsetof(PrefsBlob, parallax); off < PrefsBlob::kSize; ++off) {
            for (const std::uint8_t value : {std::uint8_t{1}, std::uint8_t{2}}) {
                PrefsBlob probe = incoming;
                reinterpret_cast<std::uint8_t*>(&probe)[off] = value;
                PrefsBlob clean = probe;
                if (!clean.sanitise() || controlsFromPrefs(probe).calibration != shown.calibration) {
                    continue;  // Reserved here, out of range, or a shown field.
                }
                const PrefsBlob back = prefsFromControls(controlsFromPrefs(probe), probe);
                INFO("offset " << off << " value " << static_cast<int>(value));
                REQUIRE(reinterpret_cast<const std::uint8_t*>(&back)[off] == value);
                REQUIRE(back == probe);
                ++checked;
                offset24Checked = offset24Checked || off == 24;
            }
        }
        // parallax (20) and flowBackend (21) are hidden and valid for 1 and 2.
        REQUIRE(checked >= 3);
        if (!offset24Checked) {
            // Offset 24 is still `reserved` on this branch (the field arrives
            // with WP-SETTINGS at merge); a reserved byte is zeroed by
            // sanitise() by design, which the loop above already respects.
            WARN("offset 24 is reserved on this branch; it is covered once WP-SETTINGS' field is merged");
        }
    }

    SECTION("a damaged incoming blob cannot smuggle a bad hidden field through") {
        PrefsBlob damaged = incoming;
        damaged.flowBackend = 200;
        damaged.parallax = 9;
        const PrefsBlob back = prefsFromControls(controlsFromPrefs(incoming), damaged);
        PrefsBlob copy = back;
        REQUIRE(copy.sanitise());  // already clean
        REQUIRE(back.flow() == PrefsFlowBackend::Auto);
        REQUIRE(back.parallaxMode() == PrefsParallax::On);
    }
}

// =============================================================================
//  RefreshFileAsync after OK
// =============================================================================

TEST_CASE("a changed OK names the file to refresh, from the instance or the access record",
          "[importer][prefs][mapping][refresh]") {
    // The SDK guide asks the importer to call RefreshFileAsync on the main
    // file whenever the Clip Source Settings changed in a way that needs the
    // frames reimported.  imGetPrefs8 has no instance, so the path must come
    // from the imFileAccessRec8 there - the case the old code skipped.
    const PrefsBlob before = PrefsBlob::defaults();
    PrefsBlob after = before;
    after.setCalibrationChoice(PrefsCalibrationChoice::LensGuards);

    SECTION("unchanged settings refresh nothing") {
        REQUIRE(prefsRefreshTarget(before, before, L"C:\\clip.OSV", L"C:\\clip.OSV").empty());
    }
    SECTION("imGetInstancePrefs: the instance path") {
        REQUIRE(prefsRefreshTarget(before, after, L"C:\\a.OSV", L"C:\\b.OSV") == L"C:\\a.OSV");
    }
    SECTION("imGetPrefs8: no instance, the access record's path") {
        REQUIRE(prefsRefreshTarget(before, after, nullptr, L"C:\\b.OSV") == L"C:\\b.OSV");
        REQUIRE(prefsRefreshTarget(before, after, L"", L"C:\\b.OSV") == L"C:\\b.OSV");
    }
    SECTION("no path at all: nothing to refresh") {
        REQUIRE(prefsRefreshTarget(before, after, nullptr, nullptr).empty());
        REQUIRE(prefsRefreshTarget(before, after, L"", L"").empty());
    }
}

// ---------------------------------------------------------------------------
//  [WP-PHOTO] the sky seam fix rows
// ---------------------------------------------------------------------------

TEST_CASE("the sky seam fix controls round trip and refuse garbage", "[importer][prefs][mapping][photoseam]") {
    // Defaults: rim and colour at 100 %, the 2.6 degree seam edge inset.
    const DialogControls shown = controlsFromPrefs(PrefsBlob::defaults());
    REQUIRE(shown.photoSeam == static_cast<int>(PrefsPhotoSeam::RimAndGain));
    REQUIRE(shown.photoStrengthPercent == 100.0);
    REQUIRE(shown.seamInsetDeg == 2.6);

    // Every mode x a spread of strengths and insets survives the round trip.
    for (int mode = 0; mode < static_cast<int>(PrefsPhotoSeam::Count); ++mode) {
        for (const double strength : {0.0, 1.0, 35.0, 99.0, 100.0}) {
            for (const double inset : {0.0, 0.1, 1.5, 2.6, 6.0}) {
                PrefsBlob original = PrefsBlob::defaults();
                original.photoSeam = static_cast<std::uint8_t>(mode);
                original.setPhotoStrengthPercent(strength);
                original.setSeamInsetDeg(inset);
                const DialogControls controls = controlsFromPrefs(original);
                INFO("mode " << mode << " strength " << strength << " inset " << inset);
                REQUIRE(controls.photoSeam == mode);
                REQUIRE(controls.photoStrengthPercent == strength);
                REQUIRE(std::abs(controls.seamInsetDeg - inset) < 1e-9);
                REQUIRE(prefsFromControls(controls) == original);
            }
        }
    }

    // An old project (both bytes zero): Off, and OK keeps it Off.
    PrefsBlob old = PrefsBlob::defaults();
    old.photoSeam = 0;
    REQUIRE(controlsFromPrefs(old).photoSeam == static_cast<int>(PrefsPhotoSeam::Off));
    REQUIRE(prefsFromControls(controlsFromPrefs(old), old) == old);

    // Garbage from a broken dialog lands on the defaults, never on a bad blob.
    DialogControls bad = controlsFromPrefs(PrefsBlob::defaults());
    bad.photoSeam = -1;
    bad.photoStrengthPercent = std::numeric_limits<double>::quiet_NaN();
    bad.seamInsetDeg = -3.0;
    PrefsBlob blob = prefsFromControls(bad);
    REQUIRE(blob.photoSeamMode() == PrefsPhotoSeam::RimAndGain);
    REQUIRE(blob.photoStrength == 0);
    REQUIRE(blob.seamInset == 0);
    REQUIRE(blob.sanitise());
    bad.photoSeam = 9;
    bad.photoStrengthPercent = 1e9;
    bad.seamInsetDeg = std::numeric_limits<double>::infinity();
    blob = prefsFromControls(bad);
    REQUIRE(blob.photoSeamMode() == PrefsPhotoSeam::RimAndGain);
    REQUIRE(blob.photoStrengthPercent() == 100.0);
    REQUIRE(blob.seamInsetDeg() == 2.6);
    REQUIRE(blob.sanitise());
}
