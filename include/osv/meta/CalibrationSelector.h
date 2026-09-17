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
#pragma once

#include "osv/core/Result.h"
#include "osv/meta/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace osv::meta {

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
