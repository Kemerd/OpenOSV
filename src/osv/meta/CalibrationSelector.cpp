// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CalibrationSelector implementation (see the header for the rules).

#include "osv/meta/CalibrationSelector.h"

#include "osv/core/Log.h"
#include "osv/core/Math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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

/// The proto set names, indexed by CalibrationSetId.  Kept next to the enum
/// order so a new set cannot be named without being placed.
constexpr const char* kSetNames[static_cast<std::size_t>(CalibrationSetId::Count)] = {
    "native_refine", "native_refine_far", "lens_guards", "water_above", "water_under", "native",
    "far_07",        "far_09",            "far_11",      "far_12_5",    "far_14",      "far_16",
};

/// The extrinsic rotation a record carries, the same way hasCore() reads it:
/// the dedicated cam_extri_q message first, the 4-float `q` copy second.
/// Identity when neither is usable (callers only pass hasCore() records).
Quatd extrinsicOf(const DewarpParams& d) noexcept {
    if (d.camExtriQ.present) {
        return Quatd::fromWXYZ(d.camExtriQ.w, d.camExtriQ.x, d.camExtriQ.y, d.camExtriQ.z);
    }
    if (d.q.size() == 4) {
        return Quatd::fromWXYZ(d.q[0], d.q[1], d.q[2], d.q[3]);
    }
    return Quatd::identity();
}

/// Angle between two rotations in degrees, sign-agnostic (q and -q are the
/// same rotation).  atan2 of the relative rotation rather than acos of a dot
/// product: acos loses every digit near 1, which is exactly where two
/// calibrations of the same lens live (a 0.02 degree difference would read
/// as 0).
double rotationAngleDeg(const Quatd& a, const Quatd& b) noexcept {
    const Quatd r = a.normalized().conj() * b.normalized();
    const double v = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    const double angle = 2.0 * std::atan2(v, std::fabs(r.w));
    return std::isfinite(angle) ? rad2deg(angle) : 0.0;
}

/// Bit-level float equality (so -0 != +0 and NaN == NaN with the same
/// payload): "identical" must mean the renderer sees the same bytes.
bool sameBits(float a, float b) noexcept { return std::memcmp(&a, &b, sizeof(float)) == 0; }

/// Bit-level equality of two float vectors.
bool sameBits(const std::vector<float>& a, const std::vector<float>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    return a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

/// Fold one lens into a running delta.
void accumulate(CalibrationDelta& d, const DewarpParams& a, const DewarpParams& b) noexcept {
    // ---- magnitudes (what the probe prints) --------------------------------
    d.focalPx = std::max({d.focalPx, std::fabs(static_cast<double>(a.fx) - b.fx),
                          std::fabs(static_cast<double>(a.fy) - b.fy)});
    d.centrePx = std::max({d.centrePx, std::fabs(static_cast<double>(a.cx) - b.cx),
                           std::fabs(static_cast<double>(a.cy) - b.cy)});
    for (std::size_t i = 0; i < a.k.size(); ++i) {
        d.distortion = std::max(d.distortion, std::fabs(static_cast<double>(a.k[i]) - b.k[i]));
    }
    d.rotationDeg = std::max(d.rotationDeg, rotationAngleDeg(extrinsicOf(a), extrinsicOf(b)));

    // ---- identity (what the UI needs) --------------------------------------
    // Every field the rig builder consumes: intrinsics, radial terms, frame
    // size, the extrinsic in both of its encodings and the occlusion arc.
    bool same = sameBits(a.fx, b.fx) && sameBits(a.fy, b.fy) && sameBits(a.cx, b.cx) && sameBits(a.cy, b.cy) &&
                a.width == b.width && a.height == b.height && a.camExtriQ.present == b.camExtriQ.present &&
                sameBits(a.camExtriQ.w, b.camExtriQ.w) && sameBits(a.camExtriQ.x, b.camExtriQ.x) &&
                sameBits(a.camExtriQ.y, b.camExtriQ.y) && sameBits(a.camExtriQ.z, b.camExtriQ.z) &&
                sameBits(a.q, b.q) && sameBits(a.occlusionPtX, b.occlusionPtX) &&
                sameBits(a.occlusionPtY, b.occlusionPtY);
    for (std::size_t i = 0; same && i < a.k.size(); ++i) {
        same = sameBits(a.k[i], b.k[i]);
    }
    d.identical = d.identical && same;
}

/// The set id a CalibrationSet came from, recovered from its slave slot
/// name ("lens_guards_slave" -> LensGuards).  nullopt for a name no set
/// produces (a hand-built CalibrationSet in a test, for example).
std::optional<CalibrationSetId> setIdOf(const CalibrationSet& set) noexcept {
    for (std::size_t i = 0; i < static_cast<std::size_t>(CalibrationSetId::Count); ++i) {
        const auto id = static_cast<CalibrationSetId>(i);
        if (set.sourceSlave == PanoDewarpParams::fieldName(calibrationSetSlaveSlot(id))) {
            return id;
        }
    }
    return std::nullopt;
}

/// How the recorded accessory reads in a sentence.
const char* accessoryPhrase(ExtriLensMode mode) noexcept {
    switch (mode) {
    case ExtriLensMode::Native: return "bare lenses";
    case ExtriLensMode::LensGuards: return "lens guards";
    case ExtriLensMode::Underwater: return "the underwater housing";
    }
    return "an unknown accessory";
}

/// The calibration an accessory needs, as an adjective ("no usable
/// lens-guard calibration").
const char* calibrationAdjective(ExtriLensMode mode) noexcept {
    switch (mode) {
    case ExtriLensMode::Native: return "native";
    case ExtriLensMode::LensGuards: return "lens-guard";
    case ExtriLensMode::Underwater: return "underwater";
    }
    return "accessory";
}

/// The slots a forced accessory needs, for the "not in this clip" sentence.
const char* accessorySlots(ExtriLensMode mode) noexcept {
    switch (mode) {
    case ExtriLensMode::LensGuards: return "slots 5/6";
    case ExtriLensMode::Underwater: return "slots 9/10 and 7/8";
    case ExtriLensMode::Native: return "slots 1/2 and 11/12";
    }
    return "its slots";
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

// -----------------------------------------------------------------------------
//  Names and parsing
// -----------------------------------------------------------------------------

const char* calibrationChoiceName(CalibrationChoice choice) noexcept {
    switch (choice) {
    case CalibrationChoice::Auto: return "auto";
    case CalibrationChoice::Native: return "native";
    case CalibrationChoice::LensGuards: return "lens-guards";
    case CalibrationChoice::Underwater: return "underwater";
    case CalibrationChoice::Count: break;
    }
    return "unknown";
}

std::optional<CalibrationChoice> parseCalibrationChoice(std::string_view text) noexcept {
    // Lower-case and drop the separators people type differently
    // ("lens-guards", "lens_guards", "Lens Guards" all mean the same set).
    std::string key;
    key.reserve(text.size());
    for (const char c : text) {
        if (c == '-' || c == '_' || c == ' ') {
            continue;
        }
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (key == "auto") {
        return CalibrationChoice::Auto;
    }
    if (key == "native") {
        return CalibrationChoice::Native;
    }
    if (key == "lensguards" || key == "lensguard" || key == "guards" || key == "lensprotectors") {
        return CalibrationChoice::LensGuards;
    }
    if (key == "underwater" || key == "water") {
        return CalibrationChoice::Underwater;
    }
    return std::nullopt;
}

const char* calibrationSetName(CalibrationSetId id) noexcept {
    return id < CalibrationSetId::Count ? kSetNames[static_cast<std::size_t>(id)] : "unknown";
}

const char* calibrationSetStateName(CalibrationSetState state) noexcept {
    switch (state) {
    case CalibrationSetState::Absent: return "absent";
    case CalibrationSetState::Empty: return "empty";
    case CalibrationSetState::Partial: return "partial";
    case CalibrationSetState::Usable: return "usable";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
//  CalibrationInventory
// -----------------------------------------------------------------------------

const CalibrationSetInfo& CalibrationInventory::at(CalibrationSetId id) const noexcept {
    // An out-of-range id is a programming error; answering with the first
    // entry keeps the call total instead of reading past the array.
    const std::size_t i = id < CalibrationSetId::Count ? static_cast<std::size_t>(id) : 0u;
    return sets[i];
}

bool CalibrationInventory::usable(CalibrationSetId id) const noexcept {
    return id < CalibrationSetId::Count && at(id).state == CalibrationSetState::Usable;
}

CalibrationSetState CalibrationInventory::choiceSetState(CalibrationChoice choice) const noexcept {
    switch (choice) {
    case CalibrationChoice::LensGuards: return at(CalibrationSetId::LensGuards).state;
    case CalibrationChoice::Underwater: {
        // The selector tries under-water first and above-water second, so the
        // choice is as good as the better of the two.
        const CalibrationSetState under = at(CalibrationSetId::WaterUnder).state;
        const CalibrationSetState above = at(CalibrationSetId::WaterAbove).state;
        return static_cast<std::uint8_t>(under) >= static_cast<std::uint8_t>(above) ? under : above;
    }
    case CalibrationChoice::Auto:
    case CalibrationChoice::Native:
    case CalibrationChoice::Count:
    default: break;
    }
    return nativeReference ? at(*nativeReference).state : CalibrationSetState::Absent;
}

bool CalibrationInventory::choiceChangesStitch(CalibrationChoice choice) const noexcept {
    // The accessory set the forced choice would use, in the selector's order.
    std::optional<CalibrationSetId> target;
    switch (choice) {
    case CalibrationChoice::LensGuards:
        if (usable(CalibrationSetId::LensGuards)) {
            target = CalibrationSetId::LensGuards;
        }
        break;
    case CalibrationChoice::Underwater:
        if (usable(CalibrationSetId::WaterUnder)) {
            target = CalibrationSetId::WaterUnder;
        } else if (usable(CalibrationSetId::WaterAbove)) {
            target = CalibrationSetId::WaterAbove;
        }
        break;
    case CalibrationChoice::Auto:
    case CalibrationChoice::Native:
        return true;  // The reference itself.
    case CalibrationChoice::Count:
    default:
        return false;
    }
    if (!target) {
        return false;  // Falls back to native: same picture.
    }
    // Usable but a byte-for-byte copy of native: still the same picture.
    const std::optional<CalibrationDelta>& d = at(*target).vsNative;
    return !(d && d->identical);
}

int CalibrationInventory::usableFarPresets() const noexcept {
    int n = 0;
    for (std::size_t i = static_cast<std::size_t>(CalibrationSetId::Far07);
         i < static_cast<std::size_t>(CalibrationSetId::Count); ++i) {
        n += sets[i].state == CalibrationSetState::Usable ? 1 : 0;
    }
    return n;
}

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

// -----------------------------------------------------------------------------
//  compare / inventory
// -----------------------------------------------------------------------------

CalibrationDelta CalibrationSelector::compare(const CalibrationSet& a, const CalibrationSet& b) noexcept {
    // Start from "identical" and let either lens break it; magnitudes are the
    // worst of the two lenses.
    CalibrationDelta d;
    d.identical = true;
    accumulate(d, a.slave, b.slave);
    accumulate(d, a.master, b.master);
    return d;
}

CalibrationInventory CalibrationSelector::inventory(const StreamMeta& stream) {
    CalibrationInventory inv;
    // extri_lens_mode is proto3: the camera writes the message even for the
    // default value (an empty field 7), so "present" is a real statement.
    inv.recordedMode = stream.extriLensMode;
    inv.recordedModePresent = stream.present.test(7);

    // ---- classify every set ------------------------------------------------
    for (std::size_t i = 0; i < inv.sets.size(); ++i) {
        const auto id = static_cast<CalibrationSetId>(i);
        CalibrationSetInfo& info = inv.sets[i];
        info.id = id;
        const std::uint32_t s = calibrationSetSlaveSlot(id);
        const std::uint32_t m = calibrationSetMasterSlot(id);
        // byField keeps what the decoder saw, placeholders included; get()
        // would hide the difference between "absent" and "zero-filled".
        const std::optional<DewarpParams>& rs = stream.dewarp.byField[s];
        const std::optional<DewarpParams>& rm = stream.dewarp.byField[m];
        const bool anyRecord = rs.has_value() || rm.has_value();
        const bool anyData = (rs && !rs->isEmpty()) || (rm && !rm->isEmpty());
        if (!anyRecord) {
            info.state = CalibrationSetState::Absent;
        } else if (!anyData) {
            info.state = CalibrationSetState::Empty;
        } else if (pairFromSlots(stream, s, m)) {
            info.state = CalibrationSetState::Usable;
        } else {
            info.state = CalibrationSetState::Partial;
        }
    }

    // ---- the native reference (what a Native choice stitches with) --------
    if (inv.usable(CalibrationSetId::NativeRefine)) {
        inv.nativeReference = CalibrationSetId::NativeRefine;
    } else if (inv.usable(CalibrationSetId::Native)) {
        inv.nativeReference = CalibrationSetId::Native;
    }
    if (!inv.nativeReference) {
        return inv;  // Nothing to compare against.
    }
    const std::optional<CalibrationSet> reference = pairFromSlots(
        stream, calibrationSetSlaveSlot(*inv.nativeReference), calibrationSetMasterSlot(*inv.nativeReference));
    if (!reference) {
        return inv;  // Unreachable: usable() just said the pair is there.
    }

    // ---- compare every usable set with it ---------------------------------
    for (CalibrationSetInfo& info : inv.sets) {
        if (info.state != CalibrationSetState::Usable) {
            continue;
        }
        const std::optional<CalibrationSet> pair =
            pairFromSlots(stream, calibrationSetSlaveSlot(info.id), calibrationSetMasterSlot(info.id));
        if (pair) {
            info.vsNative = compare(*pair, *reference);
        }
    }
    return inv;
}

// -----------------------------------------------------------------------------
//  choose
// -----------------------------------------------------------------------------

Result<CalibrationSelection> CalibrationSelector::choose(const StreamMeta& stream, CalibrationChoice choice,
                                                        const Options& options,
                                                        std::vector<std::string>* warnings) {
    // ---- the choice becomes a lens-mode override (or none, for Auto) ------
    Options opt = options;
    switch (choice) {
    case CalibrationChoice::Auto: opt.lensModeOverride.reset(); break;
    case CalibrationChoice::Native: opt.lensModeOverride = ExtriLensMode::Native; break;
    case CalibrationChoice::LensGuards: opt.lensModeOverride = ExtriLensMode::LensGuards; break;
    case CalibrationChoice::Underwater: opt.lensModeOverride = ExtriLensMode::Underwater; break;
    case CalibrationChoice::Count:
    default:
        return Error{ErrorCode::InvalidArgument,
                     std::format("calibration choice {} is not a valid choice", static_cast<int>(choice))};
    }

    // ---- the pair, by the one rule set select() owns ----------------------
    std::vector<std::string> local;
    Result<CalibrationSet> picked = select(stream, opt, &local);
    if (!picked.ok()) {
        return picked.error();
    }
    if (warnings) {
        warnings->insert(warnings->end(), local.begin(), local.end());
    }

    CalibrationSelection sel;
    sel.set = std::move(picked).value();
    sel.choice = choice;
    sel.wanted = opt.lensModeOverride.value_or(stream.extriLensMode);
    sel.used = setIdOf(sel.set).value_or(CalibrationSetId::NativeRefine);

    // ---- did the wanted accessory set actually get used? ------------------
    const bool farPreset = sel.used >= CalibrationSetId::Far07;
    const bool nativeUsed = sel.used == CalibrationSetId::NativeRefine || sel.used == CalibrationSetId::Native;
    sel.fellBack = !farPreset && nativeUsed &&
                   (sel.wanted == ExtriLensMode::LensGuards || sel.wanted == ExtriLensMode::Underwater);

    // ---- an accessory set that is only a copy of native changes nothing ----
    const CalibrationInventory inv = inventory(stream);
    const std::string usedName = calibrationSetName(sel.used);
    const char* refName = inv.nativeReference ? calibrationSetName(*inv.nativeReference) : "native";
    if (!nativeUsed && !farPreset) {
        const std::optional<CalibrationDelta>& d = inv.at(sel.used).vsNative;
        sel.identicalToNative = d.has_value() && d->identical;
        if (sel.identicalToNative) {
            note(warnings, std::format("the {} pair is numerically identical to {}; this choice cannot change the stitch",
                                       usedName, refName));
        }
    }

    // ---- the one sentence ---------------------------------------------------
    // Written for the person looking at the log or the Properties panel: what
    // was asked, what the clip said, which set stitches and - when the answer
    // is "the same as Native" - that it is, so an unchanged picture is
    // explained rather than mysterious.
    std::string asked;
    if (choice == CalibrationChoice::Auto) {
        if (!inv.recordedModePresent) {
            asked = "auto: the clip does not record a lens accessory, assuming bare lenses";
        } else {
            asked = std::format("auto: the camera recorded {} (extri_lens_mode {})", accessoryPhrase(stream.extriLensMode),
                                extriLensModeName(stream.extriLensMode));
        }
    } else {
        asked = std::format("{} (forced)", calibrationChoiceName(choice));
        if (inv.recordedModePresent && sel.wanted != stream.extriLensMode) {
            asked += std::format(", overriding the recorded {}", accessoryPhrase(stream.extriLensMode));
        }
    }

    if (farPreset) {
        sel.reason = std::format("{}; stitch distance {:.2f} m selects the {} preset", asked,
                                 options.stitchDistanceM.value_or(0.0), usedName);
    } else if (sel.fellBack) {
        const CalibrationChoice needed =
            sel.wanted == ExtriLensMode::LensGuards ? CalibrationChoice::LensGuards : CalibrationChoice::Underwater;
        sel.reason = std::format("{}; this clip holds no usable {} calibration ({} {}), so it stitches with {} "
                                 "- identical to Native",
                                 asked, calibrationAdjective(sel.wanted), accessorySlots(sel.wanted),
                                 calibrationSetStateName(inv.choiceSetState(needed)), usedName);
    } else if (sel.identicalToNative) {
        sel.reason = std::format("{}; stitching with {}, which carries exactly the {} numbers - identical to Native",
                                 asked, usedName, refName);
    } else {
        sel.reason = std::format("{}; stitching with {}", asked, usedName);
    }
    return sel;
}

}  // namespace osv::meta
