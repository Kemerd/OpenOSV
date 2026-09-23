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

#include "CalibrationUi.h"
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
    // The combo lists the CHOICE (Auto, Native, Lens protectors, Underwater),
    // which two stored bytes encode - see PrefsBlob::calibrationChoice().
    c.calibration = static_cast<int>(prefs.calibrationChoice());
    c.dlogmFit = static_cast<int>(prefs.dlogmFit);
    c.exposureStops = static_cast<double>(prefs.exposureStops);
    c.renderDevice = static_cast<int>(prefs.renderDevice);
    return c;
}

// ---------------------------------------------------------------------------
//  controls -> PrefsBlob
// ---------------------------------------------------------------------------

PrefsBlob prefsFromControls(const DialogControls& controls) noexcept {
    return prefsFromControls(controls, PrefsBlob::defaults());
}

PrefsBlob prefsFromControls(const DialogControls& controls, const PrefsBlob& base) noexcept {
    // Start from the blob the dialog was opened with, NOT from defaults():
    // the dialog shows only some of the fields, and every field it does not
    // show (parallax, flow backend, and whatever later packages add) must
    // come back exactly as it went in.  Starting from defaults() silently
    // reset parallax to On and the flow backend to Auto on every OK.
    //
    // The base is sanitised first so a damaged incoming blob cannot smuggle
    // an out-of-range hidden field through.
    PrefsBlob blob = base;
    blob.sanitise();

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
    // An out-of-range calibration index lands on Auto, the default; the
    // setter writes the canonical byte pattern for the choice.
    blob.setCalibrationChoice(static_cast<PrefsCalibrationChoice>(
        pick(controls.calibration, static_cast<int>(PrefsCalibrationChoice::Count),
             static_cast<std::uint8_t>(PrefsCalibrationChoice::Auto))));
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
//  Which file to refresh after OK
// ---------------------------------------------------------------------------

std::wstring prefsRefreshTarget(const PrefsBlob& before, const PrefsBlob& after, const wchar_t* instancePath,
                                const wchar_t* accessPath) noexcept {
    // Unchanged settings: the frames already on screen are the right ones,
    // and every PPix is keyed on the whole blob anyway.
    if (before == after) {
        return {};
    }
    try {
        // The live instance knows its own file best; imGetPrefs8 has no
        // instance, but the host names the clip's file in imFileAccessRec8.
        if (instancePath && instancePath[0] != L'\0') {
            return std::wstring(instancePath);
        }
        if (accessPath && accessPath[0] != L'\0') {
            return std::wstring(accessPath);
        }
    } catch (...) {
        // Allocation failure: no refresh is the safe answer (the user can
        // still force one by reopening the project).
    }
    return {};
}

// ---------------------------------------------------------------------------
//  Calibration combo labels
// ---------------------------------------------------------------------------

std::array<std::wstring, static_cast<std::size_t>(PrefsCalibrationChoice::Count)>
calibrationChoiceLabels(const CalibrationUiFacts& facts) {
    // Index = PrefsCalibrationChoice.  DJI's own words where they exist: the
    // camera's control centre calls the setting "Lens Protection Mode" and
    // the accessory "Transparent Lens Protectors", and ND filters that sit on
    // the lenses the same way are declared through that same mode (Freewell's
    // instructions say exactly that), so the entry names both.
    std::array<std::wstring, static_cast<std::size_t>(PrefsCalibrationChoice::Count)> labels = {
        L"Auto (follow the camera)",
        L"Native (bare lenses)",
        L"Lens protectors / ND filters",
        L"Underwater",
    };
    if (!facts.known) {
        return labels;  // Nothing to say about a clip we could not read.
    }

    // ---- Auto: name what it follows, and whether that set is really there --
    auto& autoLabel = labels[static_cast<std::size_t>(PrefsCalibrationChoice::Auto)];
    switch (facts.recordedAccessory) {
    case -1:
        autoLabel = L"Auto (nothing recorded: Native)";
        break;
    case 0:
        autoLabel = L"Auto (camera: no lens protectors)";
        break;
    case 1:
        // Always backed: without a dedicated set the protector field-angle
        // correction is applied to native.
        autoLabel = L"Auto (camera: lens protectors)";
        break;
    case 2:
        autoLabel = facts.underwater == CalibrationAvailability::Missing ? L"Auto (underwater recorded, no data)"
                                                                          : L"Auto (camera: underwater)";
        break;
    default:
        autoLabel = L"Auto (unknown accessory: Native)";
        break;
    }

    // ---- the forced underwater set: say when it cannot change anything -----
    // (Lens protectors are never marked: they always apply either their own
    // set or the field-angle correction, so they always change the stitch.)
    auto mark = [](std::wstring& label, CalibrationAvailability a, const wchar_t* name) {
        switch (a) {
        case CalibrationAvailability::Missing:
            label = std::wstring(name) + L" (not in clip: Native)";
            break;
        case CalibrationAvailability::SameAsNative:
            label = std::wstring(name) + L" (same as Native here)";
            break;
        case CalibrationAvailability::Unknown:
        case CalibrationAvailability::Usable:
        default:
            break;  // The plain name is the truth.
        }
    };
    mark(labels[static_cast<std::size_t>(PrefsCalibrationChoice::Underwater)], facts.underwater, L"Underwater");
    return labels;
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

    // ---- D-Log M passthrough ------------------------------------------------
    //
    // There is NO predefined token for DJI D-Log M.  PrSDKColorSpaces.h has
    // camera log spaces (kPrSonySGamutSLog2, kPrSony2020SLog3,
    // kPrSonySGamut3CineSLog3, kPrSonySGamut3SLog3) but nothing for DJI, and
    // grepping the whole SDK header set for "dlog" / "dji" finds nothing.  So
    // whatever this returns is an APPROXIMATION and the only question is which
    // approximation misleads the host least.
    //
    // kPrOverranged2020Scene - "BT.2020 RGB Full (Scene)", full range, RGB,
    // 32f, scene-referred - is chosen because three of its four properties are
    // exactly right and the fourth is the closest available:
    //
    //   * FULL RANGE and RGB and 32f: correct, and they are the properties
    //     that decide whether the host rescales or reinterprets our bytes.
    //     Getting these wrong corrupts the pixels; getting the transfer wrong
    //     only mis-previews them.
    //   * SCENE-REFERRED: correct and important.  A log signal is scene
    //     light, not display light.  The Display variant would invite the
    //     host to tone-map it for the monitor, which is precisely what a user
    //     who picked passthrough is asking us not to do.
    //   * BT.2020 primaries: the approximation.  The real gamut is DJI's
    //     native camera gamut, which is wide and has no SDK token; BT.2020 is
    //     the widest standard gamut on offer, so it neither clips the data nor
    //     claims a small gamut for wide-gamut values.
    //
    // What is deliberately NOT returned, and why:
    //
    //   * kPrOverranged709 - claiming BT.709 for log data is the one genuinely
    //     harmful answer.  The host would treat the flat log curve as a
    //     finished Rec.709 image, so the Program Monitor would show washed-out
    //     mush AND a "Match Source" export would bake that interpretation in.
    //   * kPrOverranged2100PQ / ...HLG - both assert a specific HDR display
    //     transfer we did not apply, so the host's tone mapping would fight
    //     the log curve.
    //   * kPrWorkingColorSpace - explicitly forbidden here:
    //     PrSDKColorSpaces.h:104 says "you can't use this token in the
    //     importer. the importer needs to explicitly identify media color
    //     space to the host."
    //
    // handleGetIndColorSpace logs once when this branch is taken, so a
    // support log records that Premiere was told something approximate.  The
    // reasoning is in docs/PREMIERE.md under "D-Log M passthrough".
    case PrefsColorOutput::DLogM: return kPrOverranged2020Scene;

    case PrefsColorOutput::PQ:
    case PrefsColorOutput::Count:
    default:                       return kPrOverranged2100PQ;
    }
}

bool colorSpaceIsApproximate(const PrefsBlob& prefs) noexcept {
    // Only the passthrough output is declared with a token that does not
    // describe it exactly (see colorSpaceTokenFor).  Keeping this as its own
    // predicate rather than a comparison at the call site means the "is this
    // honest?" question has one answer, in the same file as the decision.
    return prefs.color() == PrefsColorOutput::DLogM;
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
    case PrefsColorOutput::DLogM:
        // H.273 has no code point for DJI D-Log M, so the honest answer is
        // the "unspecified" transfer (2) rather than a wrong specific one:
        // 2 tells the host "this is not a transfer you know", which is true,
        // whereas naming BT.709 or PQ would assert a curve we did not apply.
        // The primaries are BT.2020 (9) for the same reason as the predefined
        // token above - the widest standard gamut, so nothing is clipped.
        codes.primaries = 9;   // BT.2020
        codes.transfer = 2;    // unspecified
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
