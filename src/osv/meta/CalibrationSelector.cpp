// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CalibrationSelector implementation (see the header for the rules).

#include "osv/meta/CalibrationSelector.h"

#include "osv/core/Log.h"

#include <cmath>
#include <format>

namespace osv::meta {

namespace {

/// Append to the optional warning list and echo at debug level.
void note(std::vector<std::string>* warnings, std::string text) {
    log::debug("calibration: {}", text);
    if (warnings) {
        warnings->push_back(std::move(text));
    }
}

/// The native pair according to `preferRefined` (1/2 then 11/12, or the
/// reverse), or nullopt when neither pair is usable.
std::optional<CalibrationSet> nativePair(const StreamMeta& stream, bool preferRefined, std::vector<std::string>* warnings) {
    using P = PanoDewarpParams;
    const std::uint32_t firstS = preferRefined ? P::NativeRefineSlave : P::NativeSlave;
    const std::uint32_t firstM = preferRefined ? P::NativeRefineMaster : P::NativeMaster;
    const std::uint32_t secondS = preferRefined ? P::NativeSlave : P::NativeRefineSlave;
    const std::uint32_t secondM = preferRefined ? P::NativeMaster : P::NativeRefineMaster;
    if (auto pair = CalibrationSelector::pairFromSlots(stream, firstS, firstM)) {
        return pair;
    }
    if (auto pair = CalibrationSelector::pairFromSlots(stream, secondS, secondM)) {
        note(warnings, std::format("native pair {}/{} unusable; using {}/{}", P::fieldName(firstS), P::fieldName(firstM),
                                   P::fieldName(secondS), P::fieldName(secondM)));
        return pair;
    }
    return std::nullopt;
}

}  // namespace

const std::array<CalibrationSelector::FarPreset, 6>& CalibrationSelector::farPresets() noexcept {
    using P = PanoDewarpParams;
    static const std::array<FarPreset, 6> kPresets = {{
        {0.7, P::Far07Slave, P::Far07Master},
        {0.9, P::Far09Slave, P::Far09Master},
        {1.1, P::Far11Slave, P::Far11Master},
        {1.25, P::Far12_5Slave, P::Far12_5Master},
        {1.4, P::Far14Slave, P::Far14Master},
        {1.6, P::Far16Slave, P::Far16Master},
    }};
    return kPresets;
}

std::optional<CalibrationSet> CalibrationSelector::pairFromSlots(const StreamMeta& stream, std::uint32_t slaveSlot,
                                                                 std::uint32_t masterSlot) {
    // get() already hides empty records; hasCore() rejects partial ones.
    const DewarpParams* slave = stream.dewarp.get(slaveSlot);
    const DewarpParams* master = stream.dewarp.get(masterSlot);
    if (!slave || !master || !slave->hasCore() || !master->hasCore()) {
        return std::nullopt;
    }
    CalibrationSet set;
    set.slave = *slave;
    set.master = *master;
    set.sourceSlave = PanoDewarpParams::fieldName(slaveSlot);
    set.sourceMaster = PanoDewarpParams::fieldName(masterSlot);
    return set;
}

std::optional<CalibrationSelector::FarPreset> CalibrationSelector::nearestFarPreset(const StreamMeta& stream,
                                                                                    double distanceM) {
    if (!std::isfinite(distanceM)) {
        return std::nullopt;
    }
    std::optional<FarPreset> best;
    double bestDiff = 0.0;
    // Presets are in ascending distance, so on an exact tie the first (the
    // smaller distance) wins because we only replace on a strictly smaller
    // difference.
    for (const FarPreset& preset : farPresets()) {
        if (!pairFromSlots(stream, preset.slaveSlot, preset.masterSlot)) {
            continue;
        }
        const double diff = std::fabs(preset.distanceM - distanceM);
        if (!best || diff < bestDiff) {
            best = preset;
            bestDiff = diff;
        }
    }
    return best;
}

Result<CalibrationSet> CalibrationSelector::select(const StreamMeta& stream, const Options& options,
                                                  std::vector<std::string>* warnings) {
    using P = PanoDewarpParams;

    // ---- explicit stitching distance: far presets first ----------------------
    if (options.stitchDistanceM.has_value()) {
        const double wanted = *options.stitchDistanceM;
        if (!std::isfinite(wanted) || wanted <= 0.0) {
            return Error{ErrorCode::InvalidArgument, std::format("stitch distance {} is not a positive number", wanted)};
        }
        if (auto preset = nearestFarPreset(stream, wanted)) {
            auto pair = pairFromSlots(stream, preset->slaveSlot, preset->masterSlot);
            if (pair) {
                if (std::fabs(preset->distanceM - wanted) > 1e-9) {
                    note(warnings, std::format("stitch distance {:.3f} m mapped to the nearest preset {:.2f} m ({}/{})", wanted,
                                               preset->distanceM, pair->sourceSlave, pair->sourceMaster));
                }
                return *pair;
            }
        }
        note(warnings, std::format("no far preset available for stitch distance {:.3f} m; using the lens-mode rule", wanted));
    }

    // ---- lens mode rule ----------------------------------------------------------
    const ExtriLensMode mode = options.lensModeOverride.value_or(stream.extriLensMode);
    if (options.lensModeOverride.has_value() && *options.lensModeOverride != stream.extriLensMode) {
        note(warnings, std::format("lens mode override {} replaces the recorded {}", extriLensModeName(*options.lensModeOverride),
                                   extriLensModeName(stream.extriLensMode)));
    }

    switch (mode) {
    case ExtriLensMode::LensGuards:
        if (auto pair = pairFromSlots(stream, P::LensGuardsSlave, P::LensGuardsMaster)) {
            return *pair;
        }
        note(warnings, "lens-guard calibration (slots 5/6) not present; falling back to native");
        break;
    case ExtriLensMode::Underwater:
        if (auto pair = pairFromSlots(stream, P::WaterUnderSlave, P::WaterUnderMaster)) {
            return *pair;
        }
        note(warnings, "underwater calibration (slots 9/10) not present; trying the above-water pair (7/8)");
        if (auto pair = pairFromSlots(stream, P::WaterAboveSlave, P::WaterAboveMaster)) {
            return *pair;
        }
        note(warnings, "above-water calibration (slots 7/8) not present; falling back to native");
        break;
    case ExtriLensMode::Native:
        break;
    }
    // Unknown enum values also land here (treated as native with a note).
    if (mode != ExtriLensMode::Native && mode != ExtriLensMode::LensGuards && mode != ExtriLensMode::Underwater) {
        note(warnings, std::format("unknown lens mode {}; treating as native", static_cast<std::int32_t>(mode)));
    }

    if (auto pair = nativePair(stream, options.preferRefined, warnings)) {
        return *pair;
    }
    return Error{ErrorCode::NotFound, "stream metadata holds no usable calibration pair"};
}

}  // namespace osv::meta
