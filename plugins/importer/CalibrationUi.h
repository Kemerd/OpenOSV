// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CalibrationUi: what the Source Settings dialog says about a clip's lens
// calibration.
//
// The dialog used to offer Native / Lens guards / Underwater as if every
// clip carried all three.  Most do not: the Osmo 360 writes a lens-guard
// set only when the in-camera "Lens Protection Mode" was on, and an
// underwater set only for the housing - otherwise those slots are zero-filled
// placeholders, and picking them silently renders native.  The labels built
// here say that up front ("not in clip: Native"), and name what Auto will
// follow ("camera: no lens protectors").
//
// Deliberately free of osv_meta and Win32: the facts are reduced to a small
// POD by the dialog (SourceSettingsDialog.cpp, from the clip's
// CalibrationInventory), and the label logic lives in PrefsMapping.cpp, which
// the unit tests compile directly - so the strings the shipping dialog shows
// are the strings the tests check.
#pragma once

#include "PrefsBlob.h"

#include <array>
#include <cstdint>
#include <string>

namespace osv::premiere {

/// How usable one accessory calibration is in the clip at hand.
enum class CalibrationAvailability : std::uint8_t {
    Unknown = 0,       ///< No clip facts (the dialog was opened without a readable file).
    Usable = 1,        ///< The clip carries the set and it differs from native.
    SameAsNative = 2,  ///< The clip carries the set, but with exactly the native numbers.
    Missing = 3        ///< Absent, zero-filled placeholder or incomplete: the choice renders native.
};

/// The clip facts the calibration combo needs.
struct CalibrationUiFacts {
    /// False when nothing is known about the clip; every other field is then
    /// ignored and the labels are the plain names.
    bool known = false;
    /// The recorded accessory as the protobuf enum value (0 bare lenses,
    /// 1 lens protectors, 2 underwater), or -1 when the clip does not record
    /// one.  Other values are an accessory this build does not know.
    int recordedAccessory = -1;
    CalibrationAvailability lensGuards = CalibrationAvailability::Unknown;  ///< Slots 5/6.
    CalibrationAvailability underwater = CalibrationAvailability::Unknown;  ///< Slots 9/10, else 7/8.
};

/// The calibration combo entries for a clip, indexed by
/// PrefsCalibrationChoice (Auto, Native, Lens protectors, Underwater).  Each
/// label stays within ~40 characters so the closed combo box shows it whole.
[[nodiscard]] std::array<std::wstring, static_cast<std::size_t>(PrefsCalibrationChoice::Count)>
calibrationChoiceLabels(const CalibrationUiFacts& facts);

}  // namespace osv::premiere
