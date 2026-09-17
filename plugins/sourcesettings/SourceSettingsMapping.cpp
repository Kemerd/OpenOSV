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
    blob.calibration = fromPopup(controls.calibration, OSV_SS_CALIB_COUNT, blob.calibration);
    blob.dlogmFit = fromPopup(controls.dlogmFit, OSV_SS_FIT_COUNT, blob.dlogmFit);
    blob.renderDevice = fromPopup(controls.renderDevice, OSV_SS_DEVICE_COUNT, blob.renderDevice);

    // Checkboxes are already booleans; the blob stores them as 0 / 1 so the
    // bytes can be memcmp'd as part of the PPix cache key.
    blob.seamSearch = controls.seamSearch ? std::uint8_t{1} : std::uint8_t{0};
    blob.gainMatch = controls.gainMatch ? std::uint8_t{1} : std::uint8_t{0};

    // A float slider cannot normally produce NaN, but an expression or a
    // corrupt project can, and NaN compares false with every bound - so
    // sanitise() would see "not in range" and land on 0 anyway.  Rejecting it
    // explicitly here keeps the reason local and readable.
    blob.exposureStops =
        std::isfinite(controls.exposureStops) ? static_cast<float>(controls.exposureStops) : 0.0f;

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
    c.calibration = toPopup(clean.calibration, OSV_SS_CALIB_COUNT);
    c.dlogmFit = toPopup(clean.dlogmFit, OSV_SS_FIT_COUNT);
    c.renderDevice = toPopup(clean.renderDevice, OSV_SS_DEVICE_COUNT);
    c.seamSearch = clean.seamSearch != 0;
    c.gainMatch = clean.gainMatch != 0;
    c.exposureStops = static_cast<double>(clean.exposureStops);
    return c;
}

}  // namespace osv::premiere::sourcesettings
