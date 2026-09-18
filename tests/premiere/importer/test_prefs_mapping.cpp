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

#include "ImporterPlugin.h"
#include "PrefsBlob.h"

#include <algorithm>
#include <cmath>
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
        REQUIRE(controls.calibration == 0);     // Native
        REQUIRE(controls.dlogmFit == static_cast<int>(PrefsDlogmFit::Osmo360));
        REQUIRE(controls.exposureStops == 0.0);
        REQUIRE(controls.renderDevice == 0);    // Auto
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

                                        const DialogControls controls = controlsFromPrefs(original);
                                        const PrefsBlob back = prefsFromControls(controls);

                                        INFO("colour " << color << " size " << size << " stab " << stab << " calib "
                                                       << calib << " fit " << fit << " device " << device << " seam "
                                                       << seam << " gain " << gain << " exposure " << exposure);
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
