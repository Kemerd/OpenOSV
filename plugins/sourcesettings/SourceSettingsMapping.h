// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The PURE mapping between the Source Settings effect's control values and
// the importer's 128-byte PrefsBlob.
//
// It is a separate, Adobe-free translation unit for the same reason
// plugins/importer/PrefsMapping.cpp is: the effect itself can only be driven
// through the loaded module, but the arithmetic that turns ten control
// values into a blob (and back) is where a mistake is both easy to make and
// invisible - an off-by-one between AE's 1-based popup values and PrefsBlob's
// 0-based enums would silently give every user the wrong stabilisation mode.
// Keeping it here lets tests/premiere/sourcesettings compile the very source
// the .aex contains and walk every value of every enum through it.
//
// Nothing in this header or its .cpp names an Adobe type, includes an Adobe
// header, allocates, logs or touches a global.
#pragma once

// The item counts and defaults the validation below is written against.
#include "SourceSettingsParams.h"

#include "PrefsBlob.h"

#include <cstdint>

namespace osv::premiere::sourcesettings {

/// The value of every control of the Source Settings effect, read out of the
/// host's PF_ParamDef array and not yet validated.
///
/// The five popup members are AE popup values, which are **1-based**
/// (PF_ADD_POPUP's `dflt` and `u.pd.value` both are).  The PrefsBlob enums
/// they map onto are 0-based, so every conversion below is an explicit -1 or
/// +1.  Storing the raw host value rather than a pre-adjusted one is
/// deliberate: it means a hostile or corrupt value (0, -1, 99) arrives here
/// intact and is handled by one validation rule instead of underflowing an
/// unsigned somewhere upstream.
struct ControlValues {
    int colorOutput = OSV_SS_COLOR_DEFAULT;      ///< 1-based popup value.
    int outputSize = OSV_SS_SIZE_DEFAULT;        ///< 1-based popup value.
    int stabilization = OSV_SS_STAB_DEFAULT;     ///< 1-based popup value.
    bool seamSearch = OSV_SS_SEAM_SEARCH_DEFAULT != 0;
    bool gainMatch = OSV_SS_GAIN_MATCH_DEFAULT != 0;
    int calibration = OSV_SS_CALIB_DEFAULT;      ///< 1-based popup value.
    int dlogmFit = OSV_SS_FIT_DEFAULT;           ///< 1-based popup value.
    double exposureStops = OSV_SS_EXPOSURE_DEFAULT;
    int renderDevice = OSV_SS_DEVICE_DEFAULT;    ///< 1-based popup value.
    int directColour = OSV_SS_DIRECT_COLOUR_DEFAULT;  ///< [WP-SETTINGS] 1-based popup value.
    int rec709Look = OSV_SS_LOOK_DEFAULT;             ///< [WP-LOOK] 1-based popup value.
    bool flareRemoval = OSV_SS_FLARE_REMOVAL_DEFAULT != 0;  ///< [WP-FLARE] Sun Ghost Removal.
    // ---- [WP-PHOTO] the sky seam fix ----------------------------------------
    int photoSeam = OSV_SS_PHOTO_SEAM_DEFAULT;                     ///< 1-based popup value.
    double photoStrengthPercent = OSV_SS_PHOTO_STRENGTH_DEFAULT;   ///< Sky Seam Strength, percent.
    double seamInsetDeg = OSV_SS_SEAM_INSET_DEFAULT;               ///< Seam Edge Inset, degrees.
};

/// Control values -> PrefsBlob.
///
/// Every popup value is validated against its item count before it is stored;
/// anything out of range falls back to the field's documented default rather
/// than producing a blob the importer would have to repair.  A non-finite
/// exposure (a host that handed us NaN, or an expression that divided by
/// zero) becomes 0 rather than surviving as NaN.
///
/// The result is always sanitised, so it satisfies isValid() and a further
/// sanitise() call changes nothing.  That property is asserted by a test,
/// because it is what lets the importer treat a translated blob exactly like
/// one written by the modal dialog.
[[nodiscard]] PrefsBlob prefsFromControls(const ControlValues& controls) noexcept;

/// PrefsBlob -> control values, the inverse of the above.
///
/// Used by PF_Cmd_SEQUENCE_SETUP to seed the controls from the prefs the
/// importer already recorded for the media ("as shot"), so opening Source
/// Settings on a clip shows what that clip is actually being decoded with
/// instead of snapping every control back to the global default.
///
/// `prefs` is sanitised on the way in, so an invalid blob yields the
/// defaults rather than out-of-range popup values.
[[nodiscard]] ControlValues controlsFromPrefs(const PrefsBlob& prefs) noexcept;

}  // namespace osv::premiere::sourcesettings
