// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The pure control <-> PrefsBlob mapping of the Source Settings effect.
// See SourceSettingsMapping.h for why it is its own translation unit.

#include "SourceSettingsMapping.h"

#include <cmath>

namespace osv::premiere::sourcesettings {

namespace {

/// Turn a 1-based AE popup value into a 0-based PrefsBlob enum value,
/// falling back to `fallbackZeroBased` when the host handed us anything
/// outside 1..count.
///
/// The bounds test is on the RAW value, before the -1, so a popup value of 0
/// (which a freshly deserialised project with a missing parameter can produce)
/// is rejected here rather than becoming 255 after an unsigned decrement.
[[nodiscard]] std::uint8_t fromPopup(int popupValue, int count, std::uint8_t fallbackZeroBased) noexcept {
    if (popupValue < 1 || popupValue > count) {
        return fallbackZeroBased;
    }
    return static_cast<std::uint8_t>(popupValue - 1);
}

/// The inverse: a 0-based blob value as a 1-based popup value.  The blob has
/// already been sanitised by the caller, so the range test here only guards
/// against a future field whose Count grew past the popup's item list - in
/// which case showing item 1 is better than an out-of-range selection.
[[nodiscard]] int toPopup(std::uint8_t zeroBased, int count) noexcept {
    const int value = static_cast<int>(zeroBased) + 1;
    if (value < 1 || value > count) {
        return 1;
    }
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Controls -> PrefsBlob
// ---------------------------------------------------------------------------

PrefsBlob prefsFromControls(const ControlValues& controls) noexcept {
    // Start from the documented defaults so magic, version and the reserved
    // bytes are correct before any field is touched, and so a field this
    // function forgets would read as its default rather than as zero.
    PrefsBlob blob = PrefsBlob::defaults();

    // Each fallback is the value defaults() just wrote, read back out of the
    // blob rather than repeated as a literal here: two spellings of a default
    // is how the popup and the decoder drift apart.
    blob.colorOutput = fromPopup(controls.colorOutput, OSV_SS_COLOR_COUNT, blob.colorOutput);
    blob.outputSize = fromPopup(controls.outputSize, OSV_SS_SIZE_COUNT, blob.outputSize);
    blob.stabilization = fromPopup(controls.stabilization, OSV_SS_STAB_COUNT, blob.stabilization);
    // Calibration goes through its own table (kCalibrationChoiceByPopup):
    // the popup is not in enum order, and Native is two bytes (calibration
    // plus calibrationForceNative), which setCalibrationChoice() writes as a
    // pair.  An out-of-range value keeps the default choice from defaults().
    if (controls.calibration >= 1 && controls.calibration <= OSV_SS_CALIB_COUNT) {
        blob.setCalibrationChoice(kCalibrationChoiceByPopup[controls.calibration - 1]);
    }
    blob.dlogmFit = fromPopup(controls.dlogmFit, OSV_SS_FIT_COUNT, blob.dlogmFit);
    blob.renderDevice = fromPopup(controls.renderDevice, OSV_SS_DEVICE_COUNT, blob.renderDevice);
    // [WP-SETTINGS] The direct path's colour rule for this clip.
    blob.directColour = fromPopup(controls.directColour, OSV_SS_DIRECT_COLOUR_COUNT, blob.directColour);
    // [WP-LOOK] The Rec.709 output's display look (ignored by the other outputs).
    blob.look = fromPopup(controls.rec709Look, OSV_SS_LOOK_COUNT, blob.look);

    // Checkboxes are already booleans; the blob stores them as 0 / 1 so the
    // bytes can be memcmp'd as part of the PPix cache key.
    blob.seamSearch = controls.seamSearch ? std::uint8_t{1} : std::uint8_t{0};
    blob.gainMatch = controls.gainMatch ? std::uint8_t{1} : std::uint8_t{0};
    blob.flareRemoval = controls.flareRemoval ? std::uint8_t{1} : std::uint8_t{0};  // [WP-FLARE]

    // A float slider cannot normally produce NaN, but an expression or a
    // corrupt project can, and NaN compares false with every bound - so
    // sanitise() would see "not in range" and land on 0 anyway.  Rejecting it
    // explicitly here keeps the reason local and readable.
    blob.exposureStops =
        std::isfinite(controls.exposureStops) ? static_cast<float>(controls.exposureStops) : 0.0f;

    // [WP-PHOTO] The sky seam fix.  The mode is a popup like any other; the
    // two sliders go through the blob's own setters, which round to the
    // stored step (whole percent, tenths of a degree), clamp to the range,
    // turn NaN / infinities into the default and store the default value as
    // code 0 - so an untouched slider keeps tracking the default and the
    // blob stays byte-identical to PrefsBlob::defaults().
    blob.photoSeam = fromPopup(controls.photoSeam, OSV_SS_PHOTO_SEAM_COUNT, blob.photoSeam);
    blob.setPhotoStrengthPercent(controls.photoStrengthPercent);
    blob.setSeamInsetDeg(controls.seamInsetDeg);

    // [WP-SEAMTOOLS] The five seam tools, through the blob's setters in the
    // same way: rounded to the stored step (a twentieth of a degree for the
    // widths, a hundredth for the offsets), clamped, NaN / infinities to the
    // default, the default stored as zero - an untouched control leaves the
    // blob byte-identical to PrefsBlob::defaults().
    blob.setSeamBlendDeg(controls.seamBlendDeg);
    blob.setParallaxBlendDeg(controls.parallaxBlendDeg);
    blob.setSeamSmoothingDeg(controls.seamSmoothingDeg);
    blob.setNearOffsetDeg(controls.nearOffsetDeg);
    blob.setFarOffsetDeg(controls.farOffsetDeg);

    // [WP-VIGNETTE] The lens shading correction: a popup and a percent
    // slider through the blob's setter, exactly like the sky seam fix.
    blob.lensShading = fromPopup(controls.lensShading, OSV_SS_LENS_SHADING_COUNT, blob.lensShading);
    blob.setShadingStrengthPercent(controls.shadingStrengthPercent);
    // [WP-STEADY] Both popups go through their tables (the popup lists the
    // default first; the enums keep the older behaviour at 0).  An
    // out-of-range value keeps the choice defaults() wrote.
    if (controls.parallaxGrid >= 1 && controls.parallaxGrid <= OSV_SS_PARALLAX_GRID_COUNT) {
        blob.parallaxGrid = static_cast<std::uint8_t>(kParallaxGridByPopup[controls.parallaxGrid - 1]);
    }
    if (controls.lensAlign >= 1 && controls.lensAlign <= OSV_SS_LENS_ALIGN_COUNT) {
        blob.lensAlign = static_cast<std::uint8_t>(kLensAlignByPopup[controls.lensAlign - 1]);
    }

    // Clamp everything into range and zero the reserved bytes.  After this
    // the blob is byte-for-byte what the importer expects, which is the
    // property the round-trip test pins.
    blob.sanitise();
    return blob;
}

// ---------------------------------------------------------------------------
//  PrefsBlob -> controls
// ---------------------------------------------------------------------------

ControlValues controlsFromPrefs(const PrefsBlob& prefs) noexcept {
    // Work on a copy: the caller's blob may have come straight off disk or
    // out of a project file, and sanitising in place would mutate something
    // we were only asked to read.
    PrefsBlob clean = prefs;
    if (!clean.isValid()) {
        clean = PrefsBlob::defaults();
    }
    clean.sanitise();

    ControlValues c;
    c.colorOutput = toPopup(clean.colorOutput, OSV_SS_COLOR_COUNT);
    c.outputSize = toPopup(clean.outputSize, OSV_SS_SIZE_COUNT);
    c.stabilization = toPopup(clean.stabilization, OSV_SS_STAB_COUNT);
    // The popup item whose choice is the blob's (see kCalibrationChoiceByPopup);
    // a choice missing from the table cannot happen after sanitise(), and would
    // show item 1 (Auto) rather than an out-of-range selection.
    c.calibration = 1;
    for (int item = 1; item <= OSV_SS_CALIB_COUNT; ++item) {
        if (kCalibrationChoiceByPopup[item - 1] == clean.calibrationChoice()) {
            c.calibration = item;
            break;
        }
    }
    c.dlogmFit = toPopup(clean.dlogmFit, OSV_SS_FIT_COUNT);
    c.renderDevice = toPopup(clean.renderDevice, OSV_SS_DEVICE_COUNT);
    c.directColour = toPopup(clean.directColour, OSV_SS_DIRECT_COLOUR_COUNT);  // [WP-SETTINGS]
    c.rec709Look = toPopup(clean.look, OSV_SS_LOOK_COUNT);                      // [WP-LOOK]
    c.seamSearch = clean.seamSearch != 0;
    c.gainMatch = clean.gainMatch != 0;
    c.flareRemoval = clean.flareRemoval != 0;  // [WP-FLARE]
    c.exposureStops = static_cast<double>(clean.exposureStops);
    // [WP-PHOTO] The blob's decoders turn code 0 into the default values.
    c.photoSeam = toPopup(clean.photoSeam, OSV_SS_PHOTO_SEAM_COUNT);
    c.photoStrengthPercent = clean.photoStrengthPercent();
    c.seamInsetDeg = clean.seamInsetDeg();
    // [WP-SEAMTOOLS] Likewise: code 0 decodes to each tool's default.
    c.seamBlendDeg = clean.seamBlendDeg();
    c.parallaxBlendDeg = clean.parallaxBlendDeg();
    c.seamSmoothingDeg = clean.seamSmoothingDeg();
    c.nearOffsetDeg = clean.nearOffsetDeg();
    c.farOffsetDeg = clean.farOffsetDeg();
    // [WP-VIGNETTE] Code 0 decodes to the default strength.
    c.lensShading = toPopup(clean.lensShading, OSV_SS_LENS_SHADING_COUNT);
    c.shadingStrengthPercent = clean.shadingStrengthPercent();
    // [WP-STEADY] The popup items whose choices are the blob's; item 1 (Auto)
    // for a choice missing from a table, which sanitise() rules out.
    c.parallaxGrid = 1;
    for (int item = 1; item <= OSV_SS_PARALLAX_GRID_COUNT; ++item) {
        if (kParallaxGridByPopup[item - 1] == clean.parallaxGridChoice()) {
            c.parallaxGrid = item;
            break;
        }
    }
    c.lensAlign = 1;
    for (int item = 1; item <= OSV_SS_LENS_ALIGN_COUNT; ++item) {
        if (kLensAlignByPopup[item - 1] == clean.lensAlignChoice()) {
            c.lensAlign = item;
            break;
        }
    }
    return c;
}

}  // namespace osv::premiere::sourcesettings
