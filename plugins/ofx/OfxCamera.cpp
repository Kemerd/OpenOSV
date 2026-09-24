// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxCamera.cpp - the camera controls of both OpenFX effects (OfxCamera.h).
//
// Every number and every rule comes from plugins/reframe: this file only
// moves values between OpenFX parameters and the shared `reframe::Settings`
// block.  Where it mirrors a function of the Premiere effect
// (plugins/reframe/EffectMain.cpp) the comment names it, so a change to one
// host's behaviour has an obvious twin to update.

#include "OfxCamera.h"

#include "ReframeCpu.h"
#include "ReframeEasing.h"

#include "PluginLog.h"

#include <cmath>
#include <optional>
#include <string>

namespace osv::ofx::camera {

using osv::premiere::PluginLog;
using namespace osv::reframe;

namespace {

// ===========================================================================
//  Describe helpers
// ===========================================================================

/// The Output Resolution items.  Premiere's first entry is "Match Sequence";
/// the same entry is "Match Timeline" in the words of an OpenFX host.  The
/// rest of the list, and so every value, is the shared one.
[[nodiscard]] std::string resolutionItems() {
    std::vector<std::string> items = splitItems(OSV_REFRAME_RESOLUTION_ITEMS);
    if (!items.empty()) {
        items[0] = "Match Timeline";
    }
    std::string joined;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) {
            joined += '|';
        }
        joined += items[i];
    }
    return joined;
}

/// An unbounded-feeling angle control: the geometry wraps and clamps, so the
/// host only needs a range wide enough to keyframe several full turns.
[[nodiscard]] DoubleRange angleRange(double value, double displayLimit) noexcept {
    DoubleRange r;
    r.angle = true;
    r.min = -36000.0;
    r.max = 36000.0;
    r.displayMin = -displayLimit;
    r.displayMax = displayLimit;
    r.value = value;
    r.digits = 2;
    r.increment = 0.1;
    return r;
}

// ===========================================================================
//  Keyframes through the OpenFX parameter suite
// ===========================================================================

/// One control's keyframes, for the shared easing core (ReframeEasing.h) -
/// the OpenFX twin of the Premiere effect's AeKeyframeTrack.  OpenFX times
/// are frames as doubles, which the core takes as they are: one unit for
/// every call on the track, exactly what KeyframeTrack asks for.  Every host
/// refusal is std::nullopt, which makes the core keep the host's own value.
class OfxKeyframeTrack final : public KeyframeTrack {
public:
    explicit OfxKeyframeTrack(OfxParamHandle handle) noexcept : m_handle(handle) {}

    std::optional<double> keyAtOrBefore(double t) override {
        // A key AT t first (direction 0), else the nearest one before it.
        if (auto exact = find(t, 0)) {
            return exact;
        }
        return find(t, -1);
    }
    std::optional<double> keyBefore(double t) override { return find(t, -1); }
    std::optional<double> keyAfter(double t) override { return find(t, +1); }

    std::optional<double> valueAt(double t) override {
        const OfxParameterSuiteV1* ps = suites().param;
        if (!ps || !m_handle || !ps->paramGetValueAtTime || !std::isfinite(t)) {
            return std::nullopt;
        }
        double value = 0.0;
        if (ps->paramGetValueAtTime(m_handle, t, &value) != kOfxStatOK || !std::isfinite(value)) {
            return std::nullopt;
        }
        return value;
    }

private:
    /// paramGetKeyIndex + paramGetKeyTime: the key in `direction` from `t`.
    [[nodiscard]] std::optional<double> find(double t, int direction) const {
        const OfxParameterSuiteV1* ps = suites().param;
        if (!ps || !m_handle || !ps->paramGetKeyIndex || !ps->paramGetKeyTime || !std::isfinite(t)) {
            return std::nullopt;
        }
        int index = -1;
        if (ps->paramGetKeyIndex(m_handle, t, direction, &index) != kOfxStatOK || index < 0) {
            return std::nullopt;
        }
        OfxTime keyTime = 0.0;
        if (ps->paramGetKeyTime(m_handle, static_cast<unsigned int>(index), &keyTime) != kOfxStatOK ||
            !std::isfinite(keyTime)) {
            return std::nullopt;
        }
        return keyTime;
    }

    OfxParamHandle m_handle = nullptr;
};

/// The eased value of one control at `t`, or `hostValue` when easing is off
/// or the host cannot answer (the Premiere effect's EasingScope::valueOr).
[[nodiscard]] double eased(OfxParamSetHandle set, const char* name, KeyframeEasing easing, double t,
                           double hostValue) noexcept {
    if (easing == KeyframeEasing::None) {
        return hostValue;
    }
    OfxParamHandle handle = paramHandle(set, name);
    if (!handle) {
        return hostValue;
    }
    OfxKeyframeTrack track(handle);
    const std::optional<double> value = easedValue(easing, track, t);
    return value ? *value : hostValue;
}

// ===========================================================================
//  Supervision helpers (EffectMain.cpp's writeSlider / writeLens / ...)
// ===========================================================================

/// The lens the Lens choice selects.
[[nodiscard]] CameraModel selectedLens(OfxParamSetHandle set, OfxTime t) noexcept {
    return cameraModelFromLensPopup(intAt(set, kLens, t, OSV_REFRAME_LENS_DEFAULT - 1) + 1);
}

/// The lens the hidden mirror remembers (ticked = DJI).
[[nodiscard]] CameraModel mirroredLens(OfxParamSetHandle set, OfxTime t) noexcept {
    return cameraModelFromCheckbox(intAt(set, kLensMirror, t, OSV_REFRAME_CAMERA_MODEL_DEFAULT));
}

/// Select a lens: the choice and its mirror, each written only when it
/// actually changes so an unchanged control adds nothing to the undo step.
void writeLens(OfxParamSetHandle set, CameraModel model, OfxTime t) noexcept {
    const int wanted0 = lensPopupValue(model) - 1;
    if (intAt(set, kLens, t, -1) != wanted0) {
        writeInt(set, kLens, wanted0, t);
    }
    if (mirroredLens(set, t) != model) {
        writeInt(set, kLensMirror, model == CameraModel::Dji ? 1 : 0, t);
    }
}

/// Flip Preset to Custom after a manual edit: the look is no longer the
/// preset, and a popup that still named it would be lying.
void presetToCustom(OfxParamSetHandle set, OfxTime t) noexcept {
    const Preset current = sanitisePreset(intAt(set, kPreset, t, OSV_REFRAME_PRESET_DEFAULT - 1) + 1);
    if (current != Preset::Custom) {
        writeInt(set, kPreset, static_cast<int>(Preset::Custom) - 1, t);
    }
}

/// The frame shape the DJI numbers are computed for (framingAspectFor()).
[[nodiscard]] double aspectFor(OfxParamSetHandle set, OfxTime t, SizePx project) noexcept {
    const Resolution resolution =
        sanitiseResolution(intAt(set, kOutputResolution, t, OSV_REFRAME_RESOLUTION_DEFAULT - 1) + 1);
    return framingAspect(resolution, project);
}

[[nodiscard]] ClassicLens classicLensOf(OfxParamSetHandle set, OfxTime t) noexcept {
    ClassicLens lens;
    lens.fovDeg = doubleAt(set, kFov, t, OSV_REFRAME_FOV_DEFAULT);
    lens.distortion = doubleAt(set, kDistortion, t, OSV_REFRAME_DISTORTION_DEFAULT);
    return lens;
}

[[nodiscard]] DjiLens djiLensOf(OfxParamSetHandle set, OfxTime t) noexcept {
    DjiLens lens;
    lens.fovDeg = doubleAt(set, kDjiFov, t, OSV_REFRAME_DJI_FOV_DEFAULT);
    lens.correction = doubleAt(set, kCorrection, t, OSV_REFRAME_CORRECTION_DEFAULT);
    return sanitiseDjiLens(lens);
}

/// Write a DJI lens back with the Zoom read-out that belongs to it.
void writeDjiLens(OfxParamSetHandle set, const DjiLens& lens, double aspect, bool writeFov, bool writeCorrection,
                  OfxTime t) noexcept {
    if (writeFov) {
        writeDouble(set, kDjiFov, lens.fovDeg, t);
    }
    if (writeCorrection) {
        writeDouble(set, kCorrection, lens.correction, t);
    }
    writeDouble(set, kZoom, djiZoomDeg(lens, aspect), t);
}

/// Carry the picture from one lens's controls to the other's, so switching
/// lenses does not make the framing jump (EffectMain.cpp carryLookTo()).
void carryLookTo(OfxParamSetHandle set, CameraModel to, double aspect, OfxTime t) noexcept {
    if (to == CameraModel::Dji) {
        writeDjiLens(set, djiFromClassic(classicLensOf(set, t), aspect), aspect, true, true, t);
    } else {
        const ClassicLens classic = classicFromDji(djiLensOf(set, t), aspect);
        writeDouble(set, kFov, classic.fovDeg, t);
        writeDouble(set, kDistortion, classic.distortion, t);
    }
}

/// True when `a` and `b` name the same parameter.
[[nodiscard]] bool is(const char* a, const char* b) noexcept {
    return a && b && std::string_view(a) == std::string_view(b);
}

}  // namespace

// ===========================================================================
//  describe
// ===========================================================================

void describe(OfxParamSetHandle set) noexcept {
    if (!set) {
        return;
    }
    try {
        // ---- Output Resolution ---------------------------------------------
        // The shape the field of view is measured across; the host decides
        // how many pixels are rendered (ReframeParams.h, "Output resolution").
        defineChoice(set, kOutputResolution,
                     {"Output Resolution",
                      "The shape the camera frames for. Match Timeline fills the frame you are rendering; a fixed "
                      "size crops it to that shape.",
                      nullptr, false},
                     resolutionItems(), OSV_REFRAME_RESOLUTION_DEFAULT - 1);

        // ---- Camera ----------------------------------------------------------
        defineGroup(set, kCameraGroup, {"Camera", nullptr, nullptr, true}, true);

        defineChoice(set, kPreset,
                     {"Preset", "DJI Studio's looks. Picking one sets the lens, the field of view and the tilt.",
                      kCameraGroup, false},
                     OSV_REFRAME_PRESET_ITEMS, OSV_REFRAME_PRESET_DEFAULT - 1);

        defineChoice(set, kLens,
                     {"Lens",
                      "DJI matches DJI Studio and DJI's own plug-ins number for number. Classic is OpenOSV's "
                      "eye-offset lens.",
                      kCameraGroup, false},
                     OSV_REFRAME_LENS_ITEMS, OSV_REFRAME_LENS_DEFAULT - 1);

        defineDouble(set, kPan, {"Pan", "Turns the camera left and right.", kCameraGroup, true},
                     angleRange(OSV_REFRAME_PAN_DEFAULT, 180.0));
        defineDouble(set, kTilt, {"Tilt", "Tips the camera up and down.", kCameraGroup, true},
                     angleRange(OSV_REFRAME_TILT_DEFAULT, OSV_REFRAME_TILT_LIMIT_DEG));
        defineDouble(set, kRoll, {"Roll", "Rotates the frame around the view.", kCameraGroup, true},
                     angleRange(OSV_REFRAME_ROLL_DEFAULT, 180.0));

        // ---- the DJI lens ----------------------------------------------------
        DoubleRange djiFov;
        djiFov.min = OSV_REFRAME_DJI_FOV_VALID_MIN;
        djiFov.max = OSV_REFRAME_DJI_FOV_VALID_MAX;
        djiFov.displayMin = OSV_REFRAME_DJI_FOV_SLIDER_MIN;
        djiFov.displayMax = OSV_REFRAME_DJI_FOV_SLIDER_MAX;
        djiFov.value = OSV_REFRAME_DJI_FOV_DEFAULT;
        djiFov.digits = 1;
        djiFov.increment = 0.1;
        defineDouble(set, kDjiFov, {"FOV", "DJI's vertical field of view, in degrees.", kCameraGroup, true}, djiFov);

        DoubleRange correction;
        correction.min = OSV_REFRAME_CORRECTION_VALID_MIN;
        correction.max = OSV_REFRAME_CORRECTION_VALID_MAX;
        correction.displayMin = OSV_REFRAME_CORRECTION_SLIDER_MIN;
        correction.displayMax = OSV_REFRAME_CORRECTION_SLIDER_MAX;
        correction.value = OSV_REFRAME_CORRECTION_DEFAULT;
        correction.digits = 2;
        correction.increment = 0.01;
        defineDouble(set, kCorrection,
                     {"Correction Angle",
                      "How far behind the sphere's centre the camera sits. More curves the edges, and past 1.0 you "
                      "get a crystal ball.",
                      kCameraGroup, true},
                     correction);

        // Zoom is a read-out that can be typed into: never rendered from,
        // so never keyframed (DJI keyframes FOV and Correction, not Zoom).
        DoubleRange zoom;
        zoom.min = OSV_REFRAME_ZOOM_VALID_MIN;
        zoom.max = OSV_REFRAME_ZOOM_VALID_MAX;
        zoom.displayMin = OSV_REFRAME_ZOOM_SLIDER_MIN;
        zoom.displayMax = OSV_REFRAME_ZOOM_SLIDER_MAX;
        zoom.value = OSV_REFRAME_ZOOM_DEFAULT;
        zoom.digits = 1;
        zoom.increment = 0.1;
        defineDouble(set, kZoom,
                     {"Zoom",
                      "The visible angle across the frame. Type a value and FOV and Correction Angle move along DJI "
                      "Studio's zoom path to reach it.",
                      kCameraGroup, false},
                     zoom);

        // ---- the Classic lens -------------------------------------------------
        DoubleRange fov;
        fov.min = OSV_REFRAME_FOV_VALID_MIN;
        fov.max = OSV_REFRAME_FOV_VALID_MAX;
        fov.displayMin = OSV_REFRAME_FOV_SLIDER_MIN;
        fov.displayMax = OSV_REFRAME_FOV_SLIDER_MAX;
        fov.value = OSV_REFRAME_FOV_DEFAULT;
        fov.digits = 1;
        fov.increment = 0.1;
        defineDouble(set, kFov,
                     {"Classic FOV",
                      "The visible angle across the frame. Past 120 degrees the lens eases towards stereographic "
                      "on its own.",
                      kCameraGroup, true},
                     fov);

        DoubleRange distortion;
        distortion.min = OSV_REFRAME_DISTORTION_VALID_MIN;
        distortion.max = OSV_REFRAME_DISTORTION_VALID_MAX;
        distortion.displayMin = OSV_REFRAME_DISTORTION_SLIDER_MIN;
        distortion.displayMax = OSV_REFRAME_DISTORTION_SLIDER_MAX;
        distortion.value = OSV_REFRAME_DISTORTION_DEFAULT;
        distortion.digits = 1;
        distortion.increment = 0.5;
        defineDouble(set, kDistortion,
                     {"Classic Distortion", "0 keeps straight lines straight. 100 is fully stereographic.",
                      kCameraGroup, true},
                     distortion);

        // ---- how keyframes are joined -----------------------------------------
        defineChoice(set, kKeyframeEasing,
                     {"Keyframe Easing",
                      "DJI Studio's keyframe curves for Pan, Tilt, Roll and the lens. None keeps the host's own "
                      "interpolation.",
                      kCameraGroup, false},
                     OSV_REFRAME_EASING_ITEMS, OSV_REFRAME_EASING_DEFAULT - 1);
        defineBool(set, kSmoothKeyframes,
                   {"Smooth Keyframes", "Averages each angle over three frames to take the edge off a jerky move.",
                    kCameraGroup, false},
                   OSV_REFRAME_SMOOTH_DEFAULT != 0);

        // ---- Source: the sphere's own orientation (collapsed) ----------------
        defineGroup(set, kSourceGroup, {"Source", nullptr, nullptr, true}, false);
        defineDouble(set, kSourcePan, {"Source Pan", "Turns the whole sphere before the camera looks at it.",
                                       kSourceGroup, true},
                     angleRange(0.0, 180.0));
        defineDouble(set, kSourceTilt, {"Source Tilt", "Levels a sphere shot on a tipped mount.", kSourceGroup, true},
                     angleRange(0.0, 180.0));
        defineDouble(set, kSourceRoll, {"Source Roll", "Levels a sphere shot on a rolled mount.", kSourceGroup, true},
                     angleRange(0.0, 180.0));

        // ---- the hidden lens mirror --------------------------------------------
        OfxPropertySetHandle mirror =
            defineBool(set, kLensMirror, {"Lens (mirror)", nullptr, nullptr, false}, OSV_REFRAME_CAMERA_MODEL_DEFAULT != 0);
        setInt(mirror, kOfxParamPropSecret, 1);
    } catch (...) {
        // defineChoice / resolutionItems allocate; an allocation failure
        // leaves a shorter parameter list, which read() survives (every read
        // has a default) - never an exception into the host.
        PluginLog::error("ofx: describing the camera controls threw; some controls may be missing");
    }
}

// ===========================================================================
//  Project size
// ===========================================================================

SizePx projectSize(OfxImageEffectHandle effect) noexcept {
    OfxPropertySetHandle props = effectProps(effect);
    double size[2] = {0.0, 0.0};
    if (!getDoubles(props, kOfxImageEffectPropProjectSize, size, 2)) {
        return SizePx{};
    }
    // Canonical coordinates are pixels times the pixel aspect ratio in x.
    double par = getDouble(props, kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.0);
    if (!(par > 0.0) || !std::isfinite(par)) {
        par = 1.0;
    }
    const double w = size[0] / par;
    const double h = size[1];
    // A corrupt or absurd size is "unknown", never a camera dimension (the
    // Premiere effect's kMaxSequenceEdge rule).
    if (!(w >= 1.0) || !(h >= 1.0) || w > 65536.0 || h > 65536.0) {
        return SizePx{};
    }
    return SizePx{static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))};
}

// ===========================================================================
//  read
// ===========================================================================

Settings read(OfxParamSetHandle set, OfxTime time) noexcept {
    Settings s;
    if (!set || !std::isfinite(time)) {
        // The defaults of every control: the same frame a fresh instance
        // renders, rather than a NaN camera.
        s.cameraModel = kDefaultCameraModel;
        return s;
    }

    // ---- popups (0-based on the host, 1-based in the shared sanitisers) ----
    s.resolution = sanitiseResolution(intAt(set, kOutputResolution, time, OSV_REFRAME_RESOLUTION_DEFAULT - 1) + 1);
    s.preset = sanitisePreset(intAt(set, kPreset, time, OSV_REFRAME_PRESET_DEFAULT - 1) + 1);
    s.cameraModel = selectedLens(set, time);
    s.easing = sanitiseKeyframeEasing(intAt(set, kKeyframeEasing, time, OSV_REFRAME_EASING_DEFAULT - 1) + 1);
    s.smoothKeyframes = intAt(set, kSmoothKeyframes, time, OSV_REFRAME_SMOOTH_DEFAULT) != 0;

    // ---- the two lenses ---------------------------------------------------------
    s.fovDeg = doubleAt(set, kFov, time, OSV_REFRAME_FOV_DEFAULT);
    s.distortion = doubleAt(set, kDistortion, time, OSV_REFRAME_DISTORTION_DEFAULT);
    s.zoomDeg = doubleAt(set, kZoom, time, OSV_REFRAME_ZOOM_DEFAULT);
    s.djiFovDeg = doubleAt(set, kDjiFov, time, OSV_REFRAME_DJI_FOV_DEFAULT);
    s.correction = doubleAt(set, kCorrection, time, OSV_REFRAME_CORRECTION_DEFAULT);

    // [WP-EASING] The selected lens's two controls follow the preset curve;
    // the other lens is never rendered from, so it is not worth a query.
    if (s.easing != KeyframeEasing::None) {
        if (s.cameraModel == CameraModel::Dji) {
            s.djiFovDeg = eased(set, kDjiFov, s.easing, time, s.djiFovDeg);
            s.correction = eased(set, kCorrection, s.easing, time, s.correction);
        } else {
            s.fovDeg = eased(set, kFov, s.easing, time, s.fovDeg);
            s.distortion = eased(set, kDistortion, s.easing, time, s.distortion);
        }
    }

    // ---- the six angles ---------------------------------------------------------
    const char* const angleNames[6] = {kPan, kTilt, kRoll, kSourcePan, kSourceTilt, kSourceRoll};
    // Only the camera's own three are eased: the Source angles are the
    // panorama's orientation, not the camera move (readSettings()).
    constexpr int kEasedAngles = 3;
    double angles[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6; ++i) {
        angles[i] = doubleAt(set, angleNames[i], time, 0.0);
        if (i < kEasedAngles) {
            angles[i] = eased(set, angleNames[i], s.easing, time, angles[i]);
        }
    }

    // Smooth Keyframes: the average at t-1, t and t+1 frames, over eased
    // samples when an easing is chosen - the Premiere effect's arithmetic.
    if (s.smoothKeyframes) {
        for (int i = 0; i < 6; ++i) {
            const double centre = angles[i];
            double prev = doubleAt(set, angleNames[i], time - 1.0, centre);
            double next = doubleAt(set, angleNames[i], time + 1.0, centre);
            if (i < kEasedAngles) {
                prev = eased(set, angleNames[i], s.easing, time - 1.0, prev);
                next = eased(set, angleNames[i], s.easing, time + 1.0, next);
            }
            angles[i] = (prev + centre + next) / 3.0;
        }
    }

    s.panDeg = angles[0];
    s.tiltDeg = angles[1];
    s.rollDeg = angles[2];
    s.sourcePanDeg = angles[3];
    s.sourceTiltDeg = angles[4];
    s.sourceRollDeg = angles[5];
    return s;
}

// ===========================================================================
//  applyVisibility
// ===========================================================================

void applyVisibility(OfxParamSetHandle set, OfxTime time) noexcept {
    if (!set) {
        return;
    }
    const CameraModel lens = selectedLens(set, std::isfinite(time) ? time : 0.0);
    // The same table the Premiere panel is driven by (kLensControls).
    const bool dji = (lens == CameraModel::Dji);
    setParamVisible(set, kDjiFov, dji);
    setParamVisible(set, kCorrection, dji);
    setParamVisible(set, kZoom, dji);
    setParamVisible(set, kFov, !dji);
    setParamVisible(set, kDistortion, !dji);
    setParamVisible(set, kLensMirror, false);
}

// ===========================================================================
//  instanceChanged (EffectMain.cpp userChangedParam())
// ===========================================================================

bool instanceChanged(OfxParamSetHandle set, const char* name, OfxTime time, SizePx project) noexcept {
    if (!set || !name) {
        return false;
    }
    const OfxTime t = std::isfinite(time) ? time : 0.0;

    // ---- Preset: write the look, select DJI ------------------------------------
    if (is(name, kPreset)) {
        const Preset preset = sanitisePreset(intAt(set, kPreset, t, OSV_REFRAME_PRESET_DEFAULT - 1) + 1);
        const PresetEntry* entry = presetEntry(preset);
        if (!entry || !entry->writesControls) {
            return true;  // "Custom" writes nothing
        }
        EditGroup group(set, "Preset");
        writeDouble(set, kFov, entry->fovDeg, t);
        writeDouble(set, kDistortion, entry->distortion, t);
        writeDouble(set, kTilt, entry->tiltDeg, t);
        // DJI's numbers for the same look, for this frame's shape, and the
        // switch to the DJI lens that makes them the picture.
        const double aspect = aspectFor(set, t, project);
        const DjiLens lens = sanitiseDjiLens(DjiLens{djiPresetFovDeg(*entry, aspect), entry->correction});
        writeDjiLens(set, lens, aspect, true, true, t);
        writeLens(set, CameraModel::Dji, t);
        applyVisibility(set, t);
        PluginLog::debug("ofx camera: preset '{}' -> DJI fov {} correction {} (aspect {:.4f}), tilt {}", entry->label,
                         lens.fovDeg, lens.correction, aspect, entry->tiltDeg);
        return true;
    }

    // ---- a Classic control: Classic is the picture now -------------------------
    if (is(name, kFov) || is(name, kDistortion)) {
        EditGroup group(set, "Classic lens");
        presetToCustom(set, t);
        writeLens(set, CameraModel::Classic, t);
        applyVisibility(set, t);
        return true;
    }

    if (is(name, kTilt)) {
        presetToCustom(set, t);
        return true;
    }

    // ---- a DJI control ------------------------------------------------------------
    if (is(name, kDjiFov) || is(name, kCorrection)) {
        EditGroup group(set, "DJI lens");
        const double aspect = aspectFor(set, t, project);
        DjiLens lens = djiLensOf(set, t);
        const bool fovEdited = is(name, kDjiFov);
        if (selectedLens(set, t) != CameraModel::Dji) {
            // Coming from Classic: the control the user did NOT touch takes
            // its value from the current look, so only their edit shows.
            const DjiLens carried = djiFromClassic(classicLensOf(set, t), aspect);
            if (fovEdited) {
                lens.correction = carried.correction;
            } else {
                lens.fovDeg = carried.fovDeg;
            }
        }
        writeDjiLens(set, lens, aspect, !fovEdited, fovEdited, t);
        writeLens(set, CameraModel::Dji, t);
        presetToCustom(set, t);
        applyVisibility(set, t);
        return true;
    }

    // ---- Zoom: move along DJI Studio's zoom path ------------------------------
    if (is(name, kZoom)) {
        EditGroup group(set, "Zoom");
        const double aspect = aspectFor(set, t, project);
        const double target = doubleAt(set, kZoom, t, OSV_REFRAME_ZOOM_DEFAULT);
        const DjiLens from = (selectedLens(set, t) == CameraModel::Dji) ? djiLensOf(set, t)
                                                                       : djiFromClassic(classicLensOf(set, t), aspect);
        const DjiLens to = djiZoomTo(target, from, aspect);
        writeDjiLens(set, to, aspect, true, true, t);
        writeLens(set, CameraModel::Dji, t);
        presetToCustom(set, t);
        applyVisibility(set, t);
        PluginLog::debug("ofx camera: zoom {} -> DJI fov {} correction {} (aspect {:.4f})", target, to.fovDeg,
                         to.correction, aspect);
        return true;
    }

    // ---- the Lens choice: a real switch carries the look across --------------
    if (is(name, kLens)) {
        EditGroup group(set, "Lens");
        const CameraModel requested = selectedLens(set, t);
        const CameraModel previous = mirroredLens(set, t);
        writeLens(set, requested, t);  // brings the mirror into step
        const bool switched = (requested != previous);
        if (switched) {
            carryLookTo(set, requested, aspectFor(set, t, project), t);
        }
        // Classic always travels with "Custom", exactly as in Premiere.
        if (switched || requested == CameraModel::Classic) {
            presetToCustom(set, t);
        }
        applyVisibility(set, t);
        PluginLog::debug("ofx camera: lens {} -> {}{}", previous == CameraModel::Dji ? "DJI" : "Classic",
                         requested == CameraModel::Dji ? "DJI" : "Classic", switched ? "" : " (nothing to convert)");
        return true;
    }

    // ---- the rest of the camera needs no supervision --------------------------
    for (const char* camera : kAllParams) {
        if (is(name, camera)) {
            return true;
        }
    }
    return false;
}

}  // namespace osv::ofx::camera
