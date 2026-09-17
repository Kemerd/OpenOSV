// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The pure parts of the importer's preference handling: the PrefsBlob <->
// dialog-control mapping and the colour-space declaration derived from a
// blob.
//
// These live in their own translation unit on purpose.  Nothing here touches
// Win32, the host suites, the log or an ImporterInstance, so the file can be
// compiled straight into the unit tests and the mapping the shipping dialog
// uses is the mapping the tests exercise - not a copy that could drift.
//
// (SourceSettingsDialog.cpp, which does all of the above, calls into here.)

#include "ImporterPlugin.h"

#include "PrefsBlob.h"

#include <cmath>

namespace osv::premiere {

// ---------------------------------------------------------------------------
//  PrefsBlob -> controls
// ---------------------------------------------------------------------------

DialogControls controlsFromPrefs(const PrefsBlob& prefs) noexcept {
    // Every field of a blob that reached us through PrefsBlob::fromBytes is
    // already in range, so this direction is a straight widening.
    DialogControls c;
    c.colorOutput = static_cast<int>(prefs.colorOutput);
    c.outputSize = static_cast<int>(prefs.outputSize);
    c.stabilization = static_cast<int>(prefs.stabilization);
    c.seamSearch = prefs.seamSearch != 0;
    c.gainMatch = prefs.gainMatch != 0;
    c.calibration = static_cast<int>(prefs.calibration);
    c.dlogmFit = static_cast<int>(prefs.dlogmFit);
    c.exposureStops = static_cast<double>(prefs.exposureStops);
    c.renderDevice = static_cast<int>(prefs.renderDevice);
    return c;
}

// ---------------------------------------------------------------------------
//  controls -> PrefsBlob
// ---------------------------------------------------------------------------

PrefsBlob prefsFromControls(const DialogControls& controls) noexcept {
    PrefsBlob blob = PrefsBlob::defaults();

    // A control index outside its range must never produce an invalid blob.
    // A combo box with no selection reports CB_ERR (-1) and a corrupted
    // dialog state could report anything, so each index is validated against
    // the enum's Count before it is stored.
    auto pick = [](int value, int count, std::uint8_t fallback) -> std::uint8_t {
        if (value < 0 || value >= count) {
            return fallback;
        }
        return static_cast<std::uint8_t>(value);
    };

    blob.colorOutput = pick(controls.colorOutput, static_cast<int>(PrefsColorOutput::Count), 0);
    blob.outputSize = pick(controls.outputSize, static_cast<int>(PrefsOutputSize::Count), 0);
    blob.stabilization = pick(controls.stabilization, static_cast<int>(PrefsStabilization::Count), 1);
    blob.seamSearch = controls.seamSearch ? 1u : 0u;
    blob.gainMatch = controls.gainMatch ? 1u : 0u;
    blob.calibration = pick(controls.calibration, static_cast<int>(PrefsCalibration::Count), 0);
    blob.dlogmFit = pick(controls.dlogmFit, static_cast<int>(PrefsDlogmFit::Count), 0);
    blob.renderDevice = pick(controls.renderDevice, static_cast<int>(PrefsRenderDevice::Count), 0);

    // The exposure edit box is free text: "1e999" parses to infinity and a
    // cleared field can yield NaN.  Non-finite values become 0 here (NaN
    // would survive sanitise()'s range test as "not in range" and land on 0
    // anyway, but being explicit is cheaper to read); finite ones are clamped
    // by sanitise() to the documented +/- 6 stops.
    blob.exposureStops = std::isfinite(controls.exposureStops) ? static_cast<float>(controls.exposureStops) : 0.0f;

    blob.sanitise();
    return blob;
}

// ---------------------------------------------------------------------------
//  Colour declaration
// ---------------------------------------------------------------------------

const char* colorSpaceTokenFor(const PrefsBlob& prefs) noexcept {
    // The tokens (PrSDKColorSpaces.h) describe exactly what the importer
    // emits: full-range 32-bit float RGB signal codes with the chosen
    // transfer.  Using a narrow-range or YCC token here would tell the host
    // to re-interpret perfectly good float RGB.
    switch (prefs.color()) {
    case PrefsColorOutput::HLG:    return kPrOverranged2100HLG;
    case PrefsColorOutput::Rec709: return kPrOverranged709;
    case PrefsColorOutput::PQ:
    case PrefsColorOutput::Count:
    default:                       return kPrOverranged2100PQ;
    }
}

SeiCodes seiCodesFor(const PrefsBlob& prefs) noexcept {
    SeiCodes codes;
    // The matrix is always identity because the frames are RGB, never YCbCr.
    codes.matrix = 0;
    switch (prefs.color()) {
    case PrefsColorOutput::HLG:
        codes.primaries = 9;   // BT.2020
        codes.transfer = 18;   // BT.2100 HLG
        break;
    case PrefsColorOutput::Rec709:
        codes.primaries = 1;   // BT.709
        codes.transfer = 1;    // BT.709
        break;
    case PrefsColorOutput::PQ:
    case PrefsColorOutput::Count:
    default:
        codes.primaries = 9;   // BT.2020
        codes.transfer = 16;   // BT.2100 PQ
        break;
    }
    return codes;
}

}  // namespace osv::premiere
