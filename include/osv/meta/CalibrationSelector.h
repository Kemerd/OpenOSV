// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CalibrationSelector: pick the slave/master DewarpParams pair to stitch with
// out of the 24 PanoDewarpParams slots.
//
// The camera stores several calibration sets: the refined native pair
// (slots 1/2), the raw native pair (11/12), lens-guard and underwater pairs
// (5/6, 9/10 with 7/8 for above water) and six "far" pairs tuned for a fixed
// stitching distance (13..24).  Which pair applies depends on the lens
// accessory recorded in StreamMeta.extri_lens_mode, on the user's override
// and on the requested stitching distance.  Every fallback is reported in
// `warnings` so the user can see when the file did not contain what was
// asked for.
//
// A slot that exists in the file is not necessarily a calibration: the
// Osmo 360 writes EVERY slot, and the ones it has no data for are zero-filled
// placeholders (measured on the sample clip: slots 3..10 are present, 159
// bytes each, all zeros).  inventory() tells the three cases apart - absent,
// empty placeholder, usable - so a user interface can say "this clip carries
// no lens-guard calibration" instead of silently rendering native twice.
#pragma once

#include "osv/core/Result.h"
#include "osv/meta/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osv::meta {

// -----------------------------------------------------------------------------
//  User-facing choice
// -----------------------------------------------------------------------------

/// What the user asked for (Source Settings, `osvtool --calib`).
///
/// Auto is the only choice that reads the clip: it follows the accessory the
/// camera recorded in StreamMeta.extri_lens_mode.  The other three force a
/// set regardless of what was recorded, and fall back to native (with a
/// warning and a reason) when the clip does not carry the forced set.
enum class CalibrationChoice : std::uint8_t {
    Auto = 0,        ///< Follow the recorded accessory (extri_lens_mode).
    Native = 1,      ///< Bare lenses, whatever the camera recorded.
    LensGuards = 2,  ///< Force the lens-guard pair (slots 5/6).
    Underwater = 3,  ///< Force the underwater pair (9/10, then 7/8).
    Count            ///< Number of valid values (not a value itself).
};

/// Stable token of a choice: "auto", "native", "lens-guards", "underwater"
/// (the `osvtool --calib` spelling).  "unknown" for out-of-range values.
[[nodiscard]] const char* calibrationChoiceName(CalibrationChoice choice) noexcept;

/// Parse a `--calib` token (case-insensitive; "lens-guards", "lensguards",
/// "lens_guards" and "guards" are all accepted).  nullopt when unrecognised.
[[nodiscard]] std::optional<CalibrationChoice> parseCalibrationChoice(std::string_view text) noexcept;

// -----------------------------------------------------------------------------
//  Named calibration sets and what the file holds of them
// -----------------------------------------------------------------------------

/// The twelve slave/master pairs of PanoDewarpParams, in field order.
///
/// Set `i` occupies slots 2i+1 (slave) and 2i+2 (master), which is how the
/// proto numbers them; calibrationSetSlaveSlot() / ...MasterSlot() encode
/// that rule once.
enum class CalibrationSetId : std::uint8_t {
    NativeRefine = 0,     ///< 1/2   the camera's refined factory calibration (default).
    NativeRefineFar = 1,  ///< 3/4   (empty on every clip seen).
    LensGuards = 2,       ///< 5/6   lens guards fitted.
    WaterAbove = 3,       ///< 7/8   waterproof housing, above water.
    WaterUnder = 4,       ///< 9/10  waterproof housing, under water.
    Native = 5,           ///< 11/12 the raw factory calibration.
    Far07 = 6,            ///< 13/14 stitching distance 0.7 m.
    Far09 = 7,            ///< 15/16 0.9 m.
    Far11 = 8,            ///< 17/18 1.1 m.
    Far12_5 = 9,          ///< 19/20 1.25 m.
    Far14 = 10,           ///< 21/22 1.4 m.
    Far16 = 11,           ///< 23/24 1.6 m.
    Count
};

/// Stable name of a set, the proto field name without "_slave"/"_master"
/// ("native_refine", "lens_guards", "far_12_5", ...); "unknown" out of range.
[[nodiscard]] const char* calibrationSetName(CalibrationSetId id) noexcept;

/// PanoDewarpParams field of the slave record of `id` (0 when out of range).
[[nodiscard]] constexpr std::uint32_t calibrationSetSlaveSlot(CalibrationSetId id) noexcept {
    return id < CalibrationSetId::Count ? 2u * static_cast<std::uint32_t>(id) + 1u : 0u;
}

/// PanoDewarpParams field of the master record of `id` (0 when out of range).
[[nodiscard]] constexpr std::uint32_t calibrationSetMasterSlot(CalibrationSetId id) noexcept {
    return id < CalibrationSetId::Count ? 2u * static_cast<std::uint32_t>(id) + 2u : 0u;
}

/// What the file holds for one set.
enum class CalibrationSetState : std::uint8_t {
    Absent,   ///< Neither record is in the file.
    Empty,    ///< The records exist but are zero-filled placeholders (the camera writes these for sets it has not got).
    Partial,  ///< Something is there, but at least one lens lacks the projection core (fx/fy/cx/cy/size/extrinsic).
    Usable    ///< Both lenses carry a complete calibration.
};

/// Stable name of a state ("absent", "empty", "partial", "usable").
[[nodiscard]] const char* calibrationSetStateName(CalibrationSetState state) noexcept;

/// How far one pair is from another, taken as the worst of the two lenses.
/// Distances are in calibration-frame pixels (3840 x 3840 on the Osmo 360).
struct CalibrationDelta {
    double focalPx = 0.0;      ///< max |d fx|, |d fy|.
    double centrePx = 0.0;     ///< max |d cx|, |d cy|.
    double distortion = 0.0;   ///< max |d k_i| over the radial terms.
    double rotationDeg = 0.0;  ///< Largest angle between the two extrinsic rotations (degrees).
    /// True when every field the renderer consumes - intrinsics, radial
    /// terms, calibration size, extrinsic quaternion and occlusion polygon -
    /// is bit-for-bit equal: choosing one pair over the other cannot change
    /// a single rendered pixel.
    bool identical = false;
};

/// One set as found in a clip.
struct CalibrationSetInfo {
    CalibrationSetId id = CalibrationSetId::NativeRefine;
    CalibrationSetState state = CalibrationSetState::Absent;
    /// Comparison with the native reference pair (the pair a Native choice
    /// stitches with).  Present for every Usable set when a reference exists;
    /// the reference itself compares identical to itself.
    std::optional<CalibrationDelta> vsNative;
};

/// Everything a user interface needs to tell the truth about a clip's
/// calibration: which sets exist, which are placeholders, how they differ
/// and what accessory the camera recorded.
struct CalibrationInventory {
    std::array<CalibrationSetInfo, static_cast<std::size_t>(CalibrationSetId::Count)> sets{};
    ExtriLensMode recordedMode = ExtriLensMode::Native;  ///< StreamMeta.extri_lens_mode (Native when absent).
    bool recordedModePresent = false;  ///< True when the file carries StreamMeta.extri_lens_mode.
    /// The set a Native choice stitches with (NativeRefine, else Native);
    /// nullopt when the clip has neither.
    std::optional<CalibrationSetId> nativeReference;

    /// The entry for `id` (the NativeRefine entry for an out-of-range id).
    [[nodiscard]] const CalibrationSetInfo& at(CalibrationSetId id) const noexcept;

    /// True when `id` is Usable.
    [[nodiscard]] bool usable(CalibrationSetId id) const noexcept;

    /// True when forcing `choice` would stitch with a DIFFERENT set than
    /// Native does and that set is not numerically identical to native -
    /// i.e. the choice can change the picture.  Auto and Native are always
    /// "effective" (they are the reference), and so is LensGuards: without
    /// a dedicated set it applies the lens-protector field-angle correction
    /// to native, which changes the geometry on any clip.
    [[nodiscard]] bool choiceChangesStitch(CalibrationChoice choice) const noexcept;

    /// The state of the set a forced `choice` needs: lens_guards for
    /// LensGuards, water_under (else water_above) for Underwater, the native
    /// reference for Auto / Native.  Used to say WHY a choice is inert.
    [[nodiscard]] CalibrationSetState choiceSetState(CalibrationChoice choice) const noexcept;

    /// Number of far presets (13..24) that are Usable.
    [[nodiscard]] int usableFarPresets() const noexcept;
};

/// The outcome of CalibrationSelector::choose(): the pair plus the story of
/// how it was chosen, for the log line and the Properties panel.
struct CalibrationSelection {
    CalibrationSet set;                                   ///< The pair to stitch with.
    CalibrationChoice choice = CalibrationChoice::Auto;   ///< What was asked for.
    ExtriLensMode wanted = ExtriLensMode::Native;         ///< The accessory set the choice resolved to.
    CalibrationSetId used = CalibrationSetId::NativeRefine;  ///< The set actually used.
    /// True when the wanted accessory set was not usable and native was used
    /// instead: the render is exactly what Native would give.
    bool fellBack = false;
    /// True when the used set is an accessory set whose numbers equal the
    /// native reference: the render is again exactly what Native gives.
    bool identicalToNative = false;
    /// True when the lens-protector field-angle correction must be applied
    /// on top of `set` (geom/LensProtector.h): the choice resolved to lens
    /// guards, but the clip holds no dedicated lens-guard calibration (or
    /// only a copy of native).  That is the normal case - DJI's own tools
    /// never read slots 5/6 and correct protector footage exactly this way -
    /// and it replaces the old "falls back to native" outcome for lens guards.
    bool protectorCorrection = false;
    /// One sentence naming the set and why it was chosen, e.g.
    /// "auto: the camera recorded bare lenses (extri_lens_mode native); stitching with native_refine".
    std::string reason;
};

class CalibrationSelector {
public:
    /// Selection knobs (all optional).
    struct Options {
        std::optional<ExtriLensMode> lensModeOverride;  ///< Ignore StreamMeta.extri_lens_mode and use this.
        std::optional<double> stitchDistanceM;          ///< Use the far preset nearest to this distance (metres).
        bool preferRefined = true;                      ///< Native: try the refined pair (1/2) before the raw pair (11/12).
    };

    /// One entry of the far-preset table.
    struct FarPreset {
        double distanceM = 0.0;          ///< Nominal stitching distance in metres.
        std::uint32_t slaveSlot = 0;     ///< PanoDewarpParams field of the slave record.
        std::uint32_t masterSlot = 0;    ///< PanoDewarpParams field of the master record.
    };

    /// The far presets in ascending distance: 0.7 m (13/14), 0.9 m (15/16),
    /// 1.1 m (17/18), 1.25 m (19/20), 1.4 m (21/22), 1.6 m (23/24).
    [[nodiscard]] static const std::array<FarPreset, 6>& farPresets() noexcept;

    /// Select a pair.  Rules:
    ///  * Native (default): slots 1/2 when both have the core fields, else
    ///    11/12 (order swapped when !preferRefined).
    ///  * LensGuards: slots 5/6; falls back to the native rule with a warning.
    ///  * Underwater: slots 9/10; falls back to 7/8, then native, with warnings.
    ///  * stitchDistanceM: the far pair nearest to the requested distance
    ///    replaces the native pair.  Ties (e.g. 1.0 m between 0.9 and 1.1)
    ///    resolve to the smaller distance.  A preset whose records are
    ///    missing is skipped for the next nearest; when no far pair exists at
    ///    all the lens-mode rule applies with a warning.
    /// NotFound when the stream holds no usable pair at all (e.g. the second
    /// djmd track, which carries no calibration).
    [[nodiscard]] static Result<CalibrationSet> select(const StreamMeta& stream, const Options& options = {},
                                                      std::vector<std::string>* warnings = nullptr);

    /// Select a pair for a user-facing choice and explain the result.
    ///
    /// `choice` replaces `options.lensModeOverride` (Auto clears it, so the
    /// recorded extri_lens_mode decides; Native / LensGuards / Underwater
    /// force that mode).  `stitchDistanceM` and `preferRefined` keep their
    /// select() meaning.  The returned reason is one human sentence naming
    /// the set and the rule that picked it; every fallback and every "this
    /// set equals native" finding is also appended to `warnings`.
    [[nodiscard]] static Result<CalibrationSelection> choose(const StreamMeta& stream, CalibrationChoice choice,
                                                            const Options& options = {},
                                                            std::vector<std::string>* warnings = nullptr);

    /// Classify every set of `stream` and compare the usable ones with the
    /// native reference.  Never fails: a stream without calibration yields
    /// an inventory whose sets are all Absent.
    [[nodiscard]] static CalibrationInventory inventory(const StreamMeta& stream);

    /// Compare two calibration pairs (worst lens wins).
    [[nodiscard]] static CalibrationDelta compare(const CalibrationSet& a, const CalibrationSet& b) noexcept;

    /// Fetch a usable pair from two slots (both must satisfy hasCore()).
    [[nodiscard]] static std::optional<CalibrationSet> pairFromSlots(const StreamMeta& stream,
                                                                     std::uint32_t slaveSlot,
                                                                     std::uint32_t masterSlot);

    /// The far preset nearest to `distanceM` among those whose records are
    /// present in `stream` (ties resolve to the smaller distance); nullopt
    /// when none is present.
    [[nodiscard]] static std::optional<FarPreset> nearestFarPreset(const StreamMeta& stream, double distanceM);
};

}  // namespace osv::meta
