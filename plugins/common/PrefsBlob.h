// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PrefsBlob: the per-clip importer preferences that Premiere stores in the
// project file and hands back on every call (imGetPrefs8, imGetSourceVideo,
// imGetInfo8, ...).  The layout is the one fixed in docs/PREMIERE.md:
//
//   * flat, byte packed, exactly 128 bytes, versioned, with a magic so a
//     blob written by another importer (or garbage) is recognised;
//   * every field is a plain integer or float so the bytes can be hashed as
//     a PPix cache key and compared with memcmp;
//   * reserved space so future versions can add fields without changing the
//     size (the host treats the blob as an opaque byte array).
//
// The struct is defined inside its own pack(push, 1) region and never
// inside an Adobe header's region.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace osv::premiere {

/// Output colour encoding requested for the stitched frame.
///
/// The numeric values are stored in project files, so a new entry is only
/// ever APPENDED before Count.  Renumbering would silently change the colour
/// of every clip in every saved project.
enum class PrefsColorOutput : std::uint8_t {
    PQ = 0,      ///< BT.2100 PQ, full range RGB 32f.
    HLG = 1,     ///< BT.2100 HLG, full range RGB 32f.
    Rec709 = 2,  ///< BT.709, full range RGB 32f.
    /// D-Log M passthrough: the camera's own log code values, untouched.
    ///
    /// Nothing is applied - not the D-Log M curve and not the primaries
    /// matrix either (color::OutputTransfer::Passthrough bypasses both, see
    /// osvCodeToOutput in include/osv/color/ColorMath.h).  What Premiere
    /// receives is the stitched sphere still in the camera's native D-Log M
    /// encoding and native gamut, as 32-bit float code values.
    ///
    /// It exists for the grade-it-yourself workflow: keep the log signal and
    /// apply a D-Log M LUT or a Lumetri log-to-Rec.709 conversion downstream,
    /// which keeps the whole grade in one place instead of converting twice.
    /// Applying such a LUT on top of a PQ, HLG or Rec.709 output would
    /// DOUBLE-CONVERT and is the mistake this option exists to avoid.
    DLogM = 3,
    Count        ///< Number of valid values (not a value itself).
};

/// Equirectangular output size.
/// Equirect output size.  Every entry is 2:1 because a full 360 x 180 sphere
/// is always 2:1; the 16:9 delivery crop is the reframe effect's job, not the
/// importer's.  The width shown is what a new sequence built from the clip
/// inherits, so the default is chosen for editing comfort rather than for
/// maximum resolution: a 6000 x 3000 sequence is unwieldy and is nobody's
/// delivery format.
enum class PrefsOutputSize : std::uint8_t {
    Native = 0,   ///< 2 x decoded height (6000 x 3000 for 6K).
    UHD4K = 1,    ///< 3840 x 1920.
    QHD2560 = 2,  ///< 2560 x 1280 - the default; pairs with a 2560 x 1440 timeline.
    HD2K = 3,     ///< 1920 x 960.
    Count
};

/// Stabilisation mode applied from the IMU track.
enum class PrefsStabilization : std::uint8_t {
    Off = 0,
    HorizonLock = 1,
    Full = 2,
    Smooth = 3,
    Count
};

/// Lens calibration slot, as stored in the `calibration` byte.
///
/// Native (0) has always meant "whatever the clip recorded" in practice: the
/// importer never forced the native set, it passed no override and let the
/// recorded StreamMeta.extri_lens_mode decide.  That behaviour is now named
/// Auto, and a forced Native is expressed with `calibrationForceNative`
/// (see PrefsCalibrationChoice), so every blob ever written keeps rendering
/// exactly as it did.
enum class PrefsCalibration : std::uint8_t {
    Native = 0,
    LensGuards = 1,
    Underwater = 2,
    Count
};

/// The calibration as the user chooses it: the four entries a Source
/// Settings UI lists, in that order.
///
/// NOT a stored value.  It is derived from the `calibration` byte and the
/// `calibrationForceNative` byte by PrefsBlob::calibrationChoice() and
/// written back by setCalibrationChoice(), so the UI order is free to put
/// Auto first without renumbering anything already in a project file.
enum class PrefsCalibrationChoice : std::uint8_t {
    Auto = 0,        ///< Follow the accessory the camera recorded (the default).
    Native = 1,      ///< Bare lenses, whatever the camera recorded.
    LensGuards = 2,  ///< Force the lens-guard set.
    Underwater = 3,  ///< Force the underwater set.
    Count
};

/// D-Log M decode fit.
///
/// These values are persisted in the blob, so they are append-only: Osmo360 is
/// 2 even though it is the curve new clips default to, so a project saved by
/// an older build still deserialises to the curve that build rendered with.
enum class PrefsDlogmFit : std::uint8_t {
    DjiRefit = 0,
    Pocket3 = 1,
    Osmo360 = 2,
    Count
};

/// Parallax correction: how the overlap band's optical flow is measured.
///
/// The numeric values are stored in the blob and match
/// osv::render::FlowBackendKind, so a change here needs a change there.
enum class PrefsFlowBackend : std::uint8_t {
    /// Neural when it can run on this machine, classical otherwise.  The
    /// default, because it is correct on a machine with the model installed
    /// and on one without.
    Auto = 0,
    /// Dense Inverse Search on the CPU.  Always available and fully
    /// deterministic, so it is also the choice for a reproducible render.
    Classical = 1,
    /// A neural network on the GPU.  Better on large displacements and
    /// repetitive texture; needs the model file and a working GPU runtime.
    Neural = 2,
    Count
};

/// Whether the flow-based parallax correction runs at all.
///
/// Separate from the backend choice because "off" is a legitimate
/// preference, not a third backend: it is the fastest path, it is what a
/// user wants when the scene has no near objects, and it is the A/B against
/// which the corrected result is judged.
enum class PrefsParallax : std::uint8_t {
    Off = 0,
    On = 1,
    Count
};

/// [WP-SETTINGS] "Program Monitor Colour": how the reframe effect's direct
/// path (docs/DIRECT_GPU.md) treats a clip whose colour output is not the
/// sequence's working space.
///
/// The direct path renders straight from the fisheyes into the working
/// space, so Premiere's conversion of the importer's frame never runs.  When
/// the colour output IS the working space the two routes agree exactly.
/// When it is not, PQ, HLG and Rec.709 are still three encodings of the same
/// scene, and rendering that scene straight into the working space with
/// OpenOSV's own tone mapping is the colour-managed answer - it just is not
/// Premiere's generic conversion, which is what the Source monitor shows.
/// Persisted, so append-only.
enum class PrefsDirectColour : std::uint8_t {
    /// Render straight into the sequence's working space with OpenOSV's own
    /// conversion: the direct path's speed and sharpness for every graded
    /// colour output.  The default - and what the zero byte of an older
    /// project reads as.
    SequenceSpace = 0,
    /// Hand a clip whose colour output is not the working space to the
    /// equirect route, so the Program monitor shows exactly what Premiere's
    /// own conversion makes of it (the Source monitor route).
    MatchSource = 1,
    Count
};

/// [WP-LOOK] The display look of the Rec.709 colour output
/// (osv::color::Look, include/osv/color/Look.h).  Only Rec.709 has a look;
/// PQ, HLG and the passthrough ignore this byte.  Persisted, so append-only.
enum class PrefsLook : std::uint8_t {
    /// DJI Studio's own D-Log M -> Rec.709 rendering, fitted to DJI's Osmo
    /// 360 LUT: the default, and what the zero byte of an older project reads
    /// as (it is also what makeColorParams builds when no look is named).
    DjiStudio = 0,
    /// The HLG signal in Rec.709 primaries: OpenOSV's Rec.709 rendering
    /// before the look existed, kept selectable for projects graded on it.
    Standard = 1,
    Count
};

/// [WP-HDRPEAK] "HDR Peak Brightness": the display peak the PQ colour output's
/// highlights are rolled off into (the BT.2408 Annex 5 EETF, see
/// osv::color::setHdrPeak).  Only PQ has an absolute peak; HLG, Rec.709 and
/// the passthrough ignore this byte.  Persisted, so append-only.
enum class PrefsHdrPeak : std::uint8_t {
    /// 1000 nits: the master as rendered, no roll-off.  The default, and what
    /// the zero byte of every older project reads as - bit for bit the PQ
    /// output of every build before the setting existed.
    Nits1000 = 0,
    Nits600 = 1,  ///< Highlights above 464 nits roll off into 600.
    Nits400 = 2,  ///< Highlights above 251 nits roll off into 400.
    /// 203 nits, "SDR-safe": nothing above BT.2408's HDR reference white.
    /// The knee drops to 88 nits, so diffuse white itself lands at 159.
    Nits203 = 3,
    Count
};

/// [WP-HDRPEAK] The target of each PrefsHdrPeak value in nits, indexed by the
/// value (the same list as osv::color::kHdrPeakChoicesNits).
inline constexpr float kPrefsHdrPeakNits[] = {1000.0f, 600.0f, 400.0f, 203.0f};
static_assert(sizeof(kPrefsHdrPeakNits) / sizeof(kPrefsHdrPeakNits[0]) ==
                  static_cast<std::size_t>(PrefsHdrPeak::Count),
              "kPrefsHdrPeakNits does not list every PrefsHdrPeak value");

/// Renderer selection; the numeric values are the ones stored in the blob
/// and match HostContext's RenderDevicePreference.
enum class PrefsRenderDevice : std::uint8_t {
    Auto = 0,
    Cpu = 1,
    Cuda = 2,
    OpenCl = 3,
    Count
};

/// [WP-PHOTO] The photometric seam fix ("Sky seam fix" in Source Settings):
/// what the per-clip field corrects (osv::render::PhotoSeamMode, same values).
///
/// Off is 0 so an older blob - whose byte is zero - keeps rendering exactly
/// as it did, the way `parallax` was introduced; a fresh blob gets RimAndGain
/// from defaults().
enum class PrefsPhotoSeam : std::uint8_t {
    Off = 0,         ///< No field (the seam edge inset and the global gain still apply).
    RimOnly = 1,     ///< The per-longitude usable rim only.
    RimAndGain = 2,  ///< Rim plus the 2-D gain field (replaces the global gain).
    Count
};

/// [WP-VIGNETTE] The per-lens shading correction ("Lens Shading" in Source
/// Settings): osv::render::LensShadingMode, same values.
///
/// Off is 0 so an older blob - whose byte is zero - keeps rendering exactly
/// as it did, the `photoSeam` rule; a fresh blob gets Auto from defaults().
enum class PrefsLensShading : std::uint8_t {
    Off = 0,   ///< Nothing is measured or added.
    Auto = 1,  ///< Each lens's rim structure measured from its own sky and added back.
    Count
};

#pragma pack(push, 1)

/// The 128-byte preferences record.  Use defaults() to construct one,
/// isValid() to check a blob received from the host and sanitise() to clamp
/// every field into range before using it.
struct PrefsBlob {
    /// 'OOSV' as little-endian bytes: O, O, S, V.
    static constexpr std::uint32_t kMagic = 0x56534F4Fu;
    /// Layout version.  Bump when a field is added or re-interpreted.
    static constexpr std::uint32_t kVersion = 1u;
    /// Fixed size of the blob in bytes (what imGetPrefs8 reports).
    static constexpr std::size_t kSize = 128u;
    /// Exposure range accepted by sanitise(), in stops.
    static constexpr float kMinExposureStops = -6.0f;
    static constexpr float kMaxExposureStops = 6.0f;

    std::uint32_t magic = kMagic;      ///< kMagic.
    std::uint32_t version = kVersion;  ///< kVersion.
    std::uint8_t colorOutput = 0;      ///< PrefsColorOutput.
    /// PrefsOutputSize.  The member initialiser is deliberately left at 0:
    /// defaults() memsets the whole struct and then fills every field, so it
    /// is the single source of truth for what a fresh blob contains.  Two
    /// places stating a default is how they drift apart.
    std::uint8_t outputSize = 0;
    std::uint8_t stabilization = 1;    ///< PrefsStabilization (default horizon lock).
    std::uint8_t seamSearch = 1;       ///< 0 / 1.
    std::uint8_t gainMatch = 1;        ///< 0 / 1.
    std::uint8_t calibration = 0;      ///< PrefsCalibration.
    std::uint8_t dlogmFit = 0;         ///< PrefsDlogmFit.
    std::uint8_t renderDevice = 0;     ///< PrefsRenderDevice.
    float exposureStops = 0.0f;        ///< Exposure offset in stops.
    std::uint8_t parallax = 0;         ///< PrefsParallax; flow-based seam correction.
    std::uint8_t flowBackend = 0;      ///< PrefsFlowBackend.
    /// 1 when the user forced the Native set; only meaningful while
    /// `calibration` is Native (sanitise() clears it otherwise).  0 - every
    /// blob written before this byte existed, and every fresh one - keeps
    /// calibration 0 meaning Auto.  Read it through calibrationChoice().
    std::uint8_t calibrationForceNative = 0;
    /// Offset 23: the rest of WP-CALIB's byte range, unused.  Zero, and
    /// zeroed by sanitise(), so it is free for a future calibration field.
    std::uint8_t padAfterCalibration = 0;
    /// PrefsDirectColour: how the effect's direct path treats this clip's
    /// colour output (0 = SequenceSpace, the default; see DIRECT_GPU.md).
    std::uint8_t directColour = 0;
    /// Offsets 25-27: the byte ranges of WP-SETTINGS and WP-SEAM
    /// (docs/PARALLEL_WORK.md), unused.  Zero, and zeroed by sanitise(); the
    /// lead folds them into those fields at merge.
    std::uint8_t padBeforeFlare[3] = {};
    /// [WP-LOOK] PrefsLook: the Rec.709 output's display look (0 = the DJI
    /// Studio look, the default; 1 = the standard rendering).
    std::uint8_t look = 0;
    /// Offset 29: the rest of WP-LOOK's range, unused.  Zero, and zeroed by
    /// sanitise().
    std::uint8_t padAfterLook = 0;
    /// [WP-FLARE] 1 = remove the sun's internal-reflection ghosts from the
    /// lens that sees the sun (osv/render/Flare.h).  defaults() sets 1; every
    /// blob written before this byte existed holds 0 = off, so a saved
    /// project renders exactly as before (the parallax rule).
    std::uint8_t flareRemoval = 0;
    /// Offset 31: the rest of WP-FLARE's range, kept for the overlap veil
    /// estimate, which must stay off until it is cleared legally
    /// (docs/research/FLARE.md).  Zero, and zeroed by sanitise().
    std::uint8_t padAfterFlare = 0;
    // ---- [WP-PHOTO] ---------------------------------------------------------
    /// Seam edge inset of the RENDER blend (the analyses keep the calibrated
    /// FOV): 0 = the default kDefaultSeamInsetTenths, otherwise (value - 1)
    /// tenths of a degree, 1 = no inset (the render blend before this field
    /// existed) up to kMaxSeamInsetCode = 6.0 deg.  See seamInsetDeg().
    std::uint8_t seamInset = 0;
    /// PrefsPhotoSeam; 0 = Off, so an older blob renders as it did.
    std::uint8_t photoSeam = 0;
    /// Strength of the gain field: 0 = the default (100 %), otherwise
    /// (value - 1) percent, so 1 is 0 % and 101 is 100 %.  See
    /// photoStrengthPercent().
    std::uint8_t photoStrength = 0;
    /// Offsets 35-37: the rest of WP-PHOTO's range.  Zero, and zeroed by
    /// sanitise().
    std::uint8_t photoReserved[3] = {};
    // ---- [/WP-PHOTO] --------------------------------------------------------
    // ---- [WP-SEAMTOOLS] the carved seam's tweaks (osv/render/SeamTools.h) ----
    /// Seam Blend, the feather where the lenses agree: 0 = the default
    /// (kDefaultSeamBlendDeg), otherwise (value - 1) / 20 degrees, valid codes
    /// kMinSeamBlendCode..kMaxSeamBlendCode (0.2..8.0 deg).  See seamBlendDeg().
    std::uint8_t seamBlend = 0;
    /// Parallax Blend, the feather where they disagree: 0 = the default
    /// (kDefaultParallaxBlendDeg), otherwise (value - 1) / 20 degrees, codes
    /// 1..kMaxParallaxBlendCode (0.0..4.0 deg).  See parallaxBlendDeg().
    std::uint8_t parallaxBlend = 0;
    /// Seam Smoothing, the two-band blend's half width: 0 = off (the
    /// default), otherwise (value - 1) / 20 degrees, codes
    /// 1..kMaxSeamSmoothingCode (0.0..8.0 deg).  See seamSmoothingDeg().
    std::uint8_t seamSmoothing = 0;
    /// Offset 41: zero, and zeroed by sanitise(); keeps the two offsets below
    /// on even offsets.
    std::uint8_t seamToolsPad = 0;
    /// Near Offset, hundredths of a degree along the seam where the lenses
    /// disagree, -kMaxSeamOffsetHundredths..+kMaxSeamOffsetHundredths; 0 = none
    /// (the default).  Little-endian, like every multi-byte field here.
    std::int16_t nearOffset = 0;
    /// Far Offset, the same where they agree.
    std::int16_t farOffset = 0;
    // ---- [/WP-SEAMTOOLS] ------------------------------------------------------
    // ---- [WP-VIGNETTE] the lens shading correction (osv/render/LensShading.h) --
    /// PrefsLensShading; 0 = Off, so an older blob renders as it did.
    std::uint8_t lensShading = 0;
    /// Strength of the correction: 0 = the default (100 %), otherwise
    /// (value - 1) percent, so 1 is 0 % and 101 is 100 %.  See
    /// shadingStrengthPercent().
    std::uint8_t shadingStrength = 0;
    // ---- [/WP-VIGNETTE] -------------------------------------------------------
    // ---- [WP-HDRPEAK] the PQ output's peak brightness -------------------------
    /// Offsets 48-53: the byte ranges of the packages still working alongside
    /// this one (docs/PARALLEL_WORK.md).  Zero, and zeroed by sanitise(); the
    /// lead folds them into those packages' fields at merge.
    std::uint8_t padBeforeHdrPeak[6] = {};
    /// PrefsHdrPeak: the display peak the PQ output's highlights roll off
    /// into.  0 = 1000 nits (no roll-off), the default and every older blob.
    std::uint8_t hdrPeak = 0;
    /// Offset 55: the rest of this package's range.  Zero, and zeroed by
    /// sanitise().
    std::uint8_t padAfterHdrPeak = 0;
    // ---- [/WP-HDRPEAK] --------------------------------------------------------
    std::uint8_t reserved[72] = {};    ///< Zero; future fields.

    /// seamInset: the default inset in tenths of a degree (2.6 deg - the
    /// render weight then ends at 95.0 deg on the calibrated 97.59 deg lens;
    /// mirrors osv::render::kDefaultSeamInsetDeg).
    static constexpr std::uint8_t kDefaultSeamInsetTenths = 26;
    /// seamInset: the largest stored code (61 = 6.0 deg).
    static constexpr std::uint8_t kMaxSeamInsetCode = 61;
    /// photoStrength: the largest stored code (101 = 100 %).
    static constexpr std::uint8_t kMaxPhotoStrengthCode = 101;
    /// [WP-VIGNETTE] shadingStrength: the largest stored code (101 = 100 %).
    static constexpr std::uint8_t kMaxShadingStrengthCode = 101;
    // [WP-SEAMTOOLS] The seam tools' codes: (code - 1) / kSeamToolStepsPerDeg
    // degrees, 0 = the default.  Twentieths of a degree, because the
    // defaults are 1.5 and 0.35 and both must be exactly representable.
    static constexpr int kSeamToolStepsPerDeg = 20;
    /// Seam Blend default and range (mirror render::kDefaultSeamBlendDeg and
    /// the SeamTools.h range; a static_assert in the importer ties them).
    static constexpr double kDefaultSeamBlendDeg = 1.5;
    static constexpr std::uint8_t kMinSeamBlendCode = 5;    ///< 0.2 deg.
    static constexpr std::uint8_t kMaxSeamBlendCode = 161;  ///< 8.0 deg.
    /// Parallax Blend default and range.
    static constexpr double kDefaultParallaxBlendDeg = 0.35;
    static constexpr std::uint8_t kMaxParallaxBlendCode = 81;  ///< 4.0 deg.
    /// Seam Smoothing range (the default is 0 = off).
    static constexpr std::uint8_t kMaxSeamSmoothingCode = 161;  ///< 8.0 deg.
    /// Near / Far Offset range, hundredths of a degree either way (3.00 deg).
    static constexpr std::int16_t kMaxSeamOffsetHundredths = 300;

    /// A blob with every field at its documented default.
    [[nodiscard]] static PrefsBlob defaults() noexcept {
        PrefsBlob p;
        std::memset(&p, 0, sizeof(p));
        p.magic = kMagic;
        p.version = kVersion;
        p.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::PQ);
        // NATIVE, which for 6K footage is 6000 x 3000.
        //
        // This defaulted to QHD2560 for a while, to stop a new sequence built
        // from an .OSV inheriting a 6000 x 3000 timeline.  That was the wrong
        // lever: it solved a TIMELINE problem by permanently discarding
        // SOURCE resolution, so every reframe - which crops a small window out
        // of the sphere and therefore magnifies it - was upscaling from a
        // quarter-area panorama.  Zooming in looked soft for no reason.
        //
        // The timeline is handled where it belongs, by the sequence presets
        // that scripts/install_plugins.ps1 installs (2560x1440 and the rest).
        // The importer's job is to hand over every pixel the camera recorded
        // and let the sequence decide the delivery size; Source Settings still
        // offers the smaller sizes for a machine that cannot keep up.
        p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
        p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::HorizonLock);
        p.seamSearch = 1;
        p.gainMatch = 1;
        // Auto: follow the lens accessory the camera recorded.  It is the
        // only choice that is right for footage shot with AND without the
        // lens protectors, and it is what calibration 0 always did.
        p.calibration = static_cast<std::uint8_t>(PrefsCalibration::Native);
        p.calibrationForceNative = 0;
        p.dlogmFit = static_cast<std::uint8_t>(PrefsDlogmFit::Osmo360);
        p.exposureStops = 0.0f;
        p.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Auto);
        // Parallax correction ON by default: it is the thing that makes a
        // seam look stitched rather than folded, and a user who does not
        // know the option exists should still get the better picture.
        p.parallax = static_cast<std::uint8_t>(PrefsParallax::On);
        p.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Auto);
        // [WP-FLARE] Sun ghost removal ON for new clips, like parallax: it
        // only ever subtracts fitted reflections of a sun that is in frame
        // (docs/research/FLARE.md).  A blob saved before the byte existed
        // holds 0 there and keeps rendering exactly as it did.
        p.flareRemoval = 1;
        // [WP-PHOTO] The sky seam fix ON by default: rim and gain at full
        // strength; the inset stays at its default (code 0).
        p.photoSeam = static_cast<std::uint8_t>(PrefsPhotoSeam::RimAndGain);
        p.photoStrength = 0;
        // [WP-VIGNETTE] The lens shading correction ON for new clips: on the
        // sample it removes the soft dark band the master lens's rim ring
        // leaves on every sky seam crossing (NEURAL_STITCHING.md, section 9),
        // and a lens whose sky shows no structure is left untouched.
        p.lensShading = static_cast<std::uint8_t>(PrefsLensShading::Auto);
        p.shadingStrength = 0;
        return p;
    }

    /// True when the magic and version identify a blob written by this
    /// importer.  Field ranges are not checked here; call sanitise() for
    /// that.
    [[nodiscard]] bool isValid() const noexcept { return magic == kMagic && version == kVersion; }

    /// Interpret an arbitrary byte buffer as a PrefsBlob.  Returns
    /// defaults() when the buffer is null, too small or fails isValid().
    /// The returned blob is sanitised.
    [[nodiscard]] static PrefsBlob fromBytes(const void* bytes, std::size_t length) noexcept {
        if (!bytes || length < kSize) {
            return defaults();
        }
        PrefsBlob p;
        std::memcpy(&p, bytes, kSize);
        if (!p.isValid()) {
            return defaults();
        }
        p.sanitise();
        return p;
    }

    /// Clamp every enum and float into its documented range, zero the
    /// reserved bytes and restore magic/version.  Returns true when nothing
    /// had to change.
    bool sanitise() noexcept {
        bool clean = true;
        auto clampEnum = [&clean](std::uint8_t& field, std::uint8_t count, std::uint8_t fallback) {
            if (field >= count) {
                field = fallback;
                clean = false;
            }
        };
        clampEnum(colorOutput, static_cast<std::uint8_t>(PrefsColorOutput::Count), 0);
        // The fallback is the DEFAULT size, not enum value 0: a corrupt blob
        // should land on the same setting a fresh one would, otherwise
        // "garbage in Source Settings" silently means "full 6000 x 3000".
        clampEnum(outputSize, static_cast<std::uint8_t>(PrefsOutputSize::Count),
                  static_cast<std::uint8_t>(PrefsOutputSize::Native));
        clampEnum(stabilization, static_cast<std::uint8_t>(PrefsStabilization::Count), 1);
        clampEnum(seamSearch, 2, 1);
        clampEnum(gainMatch, 2, 1);
        // Remembered for calibrationForceNative below: a garbage calibration
        // byte must land on the default CHOICE (Auto), not on a forced Native.
        const bool calibrationWasGarbage = calibration >= static_cast<std::uint8_t>(PrefsCalibration::Count);
        clampEnum(calibration, static_cast<std::uint8_t>(PrefsCalibration::Count), 0);
        clampEnum(dlogmFit, static_cast<std::uint8_t>(PrefsDlogmFit::Count), 0);
        clampEnum(renderDevice, static_cast<std::uint8_t>(PrefsRenderDevice::Count), 0);
        // Both fall back to the DEFAULT, not to enum value 0, wherever those
        // differ: a corrupt blob should land where a fresh one would.  For
        // parallax that means On (1), not Off (0).
        clampEnum(parallax, static_cast<std::uint8_t>(PrefsParallax::Count),
                  static_cast<std::uint8_t>(PrefsParallax::On));
        clampEnum(flowBackend, static_cast<std::uint8_t>(PrefsFlowBackend::Count),
                  static_cast<std::uint8_t>(PrefsFlowBackend::Auto));
        // A boolean byte: anything but 0/1 is corruption and lands on the
        // default (0, Auto).  It only qualifies Native, so it is cleared for
        // the other sets - one meaning, one byte pattern, one cache key.
        clampEnum(calibrationForceNative, 2, 0);
        if (calibrationForceNative != 0 &&
            (calibrationWasGarbage || calibration != static_cast<std::uint8_t>(PrefsCalibration::Native))) {
            calibrationForceNative = 0;
            clean = false;
        }
        // Zero is the default, so a corrupt byte lands there too.
        clampEnum(directColour, static_cast<std::uint8_t>(PrefsDirectColour::Count),
                  static_cast<std::uint8_t>(PrefsDirectColour::SequenceSpace));
        if (padAfterCalibration != 0) {
            padAfterCalibration = 0;
            clean = false;
        }
        // [WP-FLARE] a boolean byte: anything but 0/1 is corruption and
        // lands on the default (off); its padding stays zero.
        clampEnum(flareRemoval, 2, 0);
        for (std::uint8_t& b : padBeforeFlare) {
            if (b != 0) {
                b = 0;
                clean = false;
            }
        }
        if (padAfterFlare != 0) {
            padAfterFlare = 0;
            clean = false;
        }
        // [WP-LOOK] zero is the default look, so a corrupt byte lands there;
        // the rest of the range stays zero.
        clampEnum(look, static_cast<std::uint8_t>(PrefsLook::Count), static_cast<std::uint8_t>(PrefsLook::DjiStudio));
        if (padAfterLook != 0) {
            padAfterLook = 0;
            clean = false;
        }

        // NaN compares false with everything, so test the valid range and
        // reset anything else (NaN, infinities, out of range).
        if (!(exposureStops >= kMinExposureStops && exposureStops <= kMaxExposureStops)) {
            exposureStops = (exposureStops > kMaxExposureStops) ? kMaxExposureStops
                            : (exposureStops < kMinExposureStops) ? kMinExposureStops
                                                                  : 0.0f;
            clean = false;
        }
        for (std::uint8_t& b : reserved) {
            if (b != 0) {
                b = 0;
                clean = false;
            }
        }
        // [WP-PHOTO] A corrupt mode lands on the DEFAULT (RimAndGain), like
        // parallax; a corrupt strength on the default 100 %.
        clampEnum(photoSeam, static_cast<std::uint8_t>(PrefsPhotoSeam::Count),
                  static_cast<std::uint8_t>(PrefsPhotoSeam::RimAndGain));
        if (photoStrength > kMaxPhotoStrengthCode) {
            photoStrength = 0;
            clean = false;
        }
        for (std::uint8_t& b : photoReserved) {
            if (b != 0) {
                b = 0;
                clean = false;
            }
        }
        // An out-of-range inset code falls back to the default (code 0), the
        // setting a fresh blob would have.
        if (seamInset > kMaxSeamInsetCode) {
            seamInset = 0;
            clean = false;
        }
        // [WP-SEAMTOOLS] Out-of-range codes land on the default (0), the
        // setting a fresh blob has; so does a Seam Blend code below 0.2 deg,
        // which no setter writes.  A code that spells the default value
        // itself is rewritten as 0 too - one meaning, one byte pattern, one
        // cache key, exactly what the setters write.  The pad byte stays zero.
        const auto canonical = [&clean](std::uint8_t& code, std::uint8_t minCode, std::uint8_t maxCode,
                                        double defaultDeg) {
            const long defaultCode = static_cast<long>(defaultDeg * kSeamToolStepsPerDeg + 0.5) + 1;
            if (code != 0 && (code < minCode || code > maxCode || static_cast<long>(code) == defaultCode)) {
                code = 0;
                clean = false;
            }
        };
        canonical(seamBlend, kMinSeamBlendCode, kMaxSeamBlendCode, kDefaultSeamBlendDeg);
        canonical(parallaxBlend, 1, kMaxParallaxBlendCode, kDefaultParallaxBlendDeg);
        canonical(seamSmoothing, 1, kMaxSeamSmoothingCode, 0.0);
        if (seamToolsPad != 0) {
            seamToolsPad = 0;
            clean = false;
        }
        if (nearOffset > kMaxSeamOffsetHundredths || nearOffset < -kMaxSeamOffsetHundredths) {
            nearOffset = 0;
            clean = false;
        }
        if (farOffset > kMaxSeamOffsetHundredths || farOffset < -kMaxSeamOffsetHundredths) {
            farOffset = 0;
            clean = false;
        }
        // [WP-VIGNETTE] A corrupt mode lands on the DEFAULT (Auto), like
        // photoSeam; a corrupt strength on the default 100 %.
        clampEnum(lensShading, static_cast<std::uint8_t>(PrefsLensShading::Count),
                  static_cast<std::uint8_t>(PrefsLensShading::Auto));
        if (shadingStrength > kMaxShadingStrengthCode) {
            shadingStrength = 0;
            clean = false;
        }
        // [WP-HDRPEAK] zero is the default (1000 nits, no roll-off), so a
        // corrupt byte lands there; the padding around it stays zero.
        clampEnum(hdrPeak, static_cast<std::uint8_t>(PrefsHdrPeak::Count),
                  static_cast<std::uint8_t>(PrefsHdrPeak::Nits1000));
        for (std::uint8_t& b : padBeforeHdrPeak) {
            if (b != 0) {
                b = 0;
                clean = false;
            }
        }
        if (padAfterHdrPeak != 0) {
            padAfterHdrPeak = 0;
            clean = false;
        }
        if (magic != kMagic || version != kVersion) {
            magic = kMagic;
            version = kVersion;
            clean = false;
        }
        return clean;
    }

    /// Pointer to the bytes that identify a rendering configuration; the
    /// whole blob takes part in every PPix cache key so a changed setting
    /// never hits a stale frame.
    [[nodiscard]] const void* cacheKey() const noexcept { return this; }

    /// Number of bytes cacheKey() points at.
    [[nodiscard]] static constexpr std::int32_t cacheKeySize() noexcept { return static_cast<std::int32_t>(kSize); }

    /// Byte-wise equality (what the host does with prefs blobs too).
    [[nodiscard]] bool operator==(const PrefsBlob& other) const noexcept {
        return std::memcmp(this, &other, kSize) == 0;
    }
    [[nodiscard]] bool operator!=(const PrefsBlob& other) const noexcept { return !(*this == other); }

    // Typed accessors keep call sites readable.
    [[nodiscard]] PrefsColorOutput color() const noexcept { return static_cast<PrefsColorOutput>(colorOutput); }
    [[nodiscard]] PrefsOutputSize size() const noexcept { return static_cast<PrefsOutputSize>(outputSize); }
    [[nodiscard]] PrefsStabilization stab() const noexcept { return static_cast<PrefsStabilization>(stabilization); }
    [[nodiscard]] PrefsCalibration calib() const noexcept { return static_cast<PrefsCalibration>(calibration); }
    [[nodiscard]] PrefsDlogmFit fit() const noexcept { return static_cast<PrefsDlogmFit>(dlogmFit); }
    [[nodiscard]] PrefsRenderDevice device() const noexcept { return static_cast<PrefsRenderDevice>(renderDevice); }
    [[nodiscard]] PrefsParallax parallaxMode() const noexcept { return static_cast<PrefsParallax>(parallax); }
    [[nodiscard]] PrefsFlowBackend flow() const noexcept { return static_cast<PrefsFlowBackend>(flowBackend); }
    /// [WP-SETTINGS]
    [[nodiscard]] PrefsDirectColour directColourMode() const noexcept {
        return static_cast<PrefsDirectColour>(directColour);
    }
    /// [WP-LOOK]
    [[nodiscard]] PrefsLook lookChoice() const noexcept { return static_cast<PrefsLook>(look); }
    /// [WP-HDRPEAK] The HDR peak choice; an out-of-range byte (an unsanitised
    /// blob) reads as the default, 1000 nits.
    [[nodiscard]] PrefsHdrPeak hdrPeakChoice() const noexcept {
        return hdrPeak < static_cast<std::uint8_t>(PrefsHdrPeak::Count) ? static_cast<PrefsHdrPeak>(hdrPeak)
                                                                        : PrefsHdrPeak::Nits1000;
    }
    /// [WP-HDRPEAK] The PQ output's target peak in nits (1000 = no roll-off).
    [[nodiscard]] float hdrPeakNits() const noexcept {
        return kPrefsHdrPeakNits[static_cast<std::size_t>(hdrPeakChoice())];
    }
    /// True when the flow-based parallax correction should run.
    [[nodiscard]] bool parallaxEnabled() const noexcept { return parallaxMode() == PrefsParallax::On; }

    /// The user's calibration choice, decoded from the two stored bytes:
    ///
    ///     calibration   calibrationForceNative   choice
    ///     Native (0)    0                        Auto        (every old blob, every fresh one)
    ///     Native (0)    1                        Native      (forced)
    ///     LensGuards    -                        LensGuards
    ///     Underwater    -                        Underwater
    ///
    /// An out-of-range calibration byte (an unsanitised blob) reads as Auto,
    /// the default, rather than as an arbitrary set.
    [[nodiscard]] PrefsCalibrationChoice calibrationChoice() const noexcept {
        switch (calib()) {
        case PrefsCalibration::LensGuards: return PrefsCalibrationChoice::LensGuards;
        case PrefsCalibration::Underwater: return PrefsCalibrationChoice::Underwater;
        case PrefsCalibration::Native:
            return calibrationForceNative != 0 ? PrefsCalibrationChoice::Native : PrefsCalibrationChoice::Auto;
        case PrefsCalibration::Count:
        default: break;
        }
        return PrefsCalibrationChoice::Auto;
    }

    /// Store a calibration choice in its canonical byte pattern (the table
    /// above), so equal choices always produce equal blobs and cache keys.
    /// An out-of-range choice stores Auto.
    void setCalibrationChoice(PrefsCalibrationChoice choice) noexcept {
        switch (choice) {
        case PrefsCalibrationChoice::Native:
            calibration = static_cast<std::uint8_t>(PrefsCalibration::Native);
            calibrationForceNative = 1;
            return;
        case PrefsCalibrationChoice::LensGuards:
            calibration = static_cast<std::uint8_t>(PrefsCalibration::LensGuards);
            calibrationForceNative = 0;
            return;
        case PrefsCalibrationChoice::Underwater:
            calibration = static_cast<std::uint8_t>(PrefsCalibration::Underwater);
            calibrationForceNative = 0;
            return;
        case PrefsCalibrationChoice::Auto:
        case PrefsCalibrationChoice::Count:
        default:
            calibration = static_cast<std::uint8_t>(PrefsCalibration::Native);
            calibrationForceNative = 0;
            return;
        }
    }

    // [WP-PHOTO]
    /// The render-blend seam edge inset in degrees: code 0 = the default
    /// (2.6), code v >= 1 = (v - 1) / 10, so code 1 is exactly 0 (no inset).
    /// Out-of-range codes read as the default, like sanitise() stores them.
    [[nodiscard]] double seamInsetDeg() const noexcept {
        const std::uint8_t code = seamInset > kMaxSeamInsetCode ? 0 : seamInset;
        const unsigned tenths = code == 0 ? kDefaultSeamInsetTenths : static_cast<unsigned>(code - 1);
        return static_cast<double>(tenths) / 10.0;
    }
    /// Store an inset in degrees (rounded to a tenth, clamped to 0..6); the
    /// default value is stored as code 0 so it keeps tracking the default.
    void setSeamInsetDeg(double deg) noexcept {
        // NaN / infinities: the default, like every other garbage input here.
        if (!(deg >= 0.0) || deg > 1e6) {
            seamInset = 0;
            return;
        }
        long tenths = static_cast<long>(deg * 10.0 + 0.5);
        if (tenths > static_cast<long>(kMaxSeamInsetCode - 1)) {
            tenths = kMaxSeamInsetCode - 1;
        }
        seamInset = tenths == kDefaultSeamInsetTenths ? std::uint8_t{0} : static_cast<std::uint8_t>(tenths + 1);
    }
    /// [WP-PHOTO] The sky seam fix mode.
    [[nodiscard]] PrefsPhotoSeam photoSeamMode() const noexcept { return static_cast<PrefsPhotoSeam>(photoSeam); }
    /// [WP-PHOTO] Gain-field strength in percent (code 0 = the default 100).
    [[nodiscard]] double photoStrengthPercent() const noexcept {
        if (photoStrength == 0 || photoStrength > kMaxPhotoStrengthCode) {
            return 100.0;
        }
        return static_cast<double>(photoStrength - 1);
    }
    /// [WP-PHOTO] Store a strength in percent (rounded, clamped to 0..100);
    /// 100 is stored as code 0 so it keeps tracking the default.
    void setPhotoStrengthPercent(double percent) noexcept {
        if (!(percent >= 0.0) || percent > 1e6) {
            photoStrength = 0;  // NaN, infinities, negatives: the default
            return;
        }
        long p = static_cast<long>(percent + 0.5);
        if (p > 100) {
            p = 100;
        }
        photoStrength = p == 100 ? std::uint8_t{0} : static_cast<std::uint8_t>(p + 1);
    }

    // ---- [WP-SEAMTOOLS] ------------------------------------------------------
    /// Seam Blend in degrees (code 0 or out of range = the default 1.5).
    [[nodiscard]] double seamBlendDeg() const noexcept {
        return decodeSeamTool(seamBlend, kMinSeamBlendCode, kMaxSeamBlendCode, kDefaultSeamBlendDeg);
    }
    /// Store a Seam Blend (rounded to a twentieth, clamped to 0.2..8.0); the
    /// default is stored as code 0, NaN / infinities as the default too.
    void setSeamBlendDeg(double deg) noexcept {
        seamBlend = encodeSeamTool(deg, kMinSeamBlendCode, kMaxSeamBlendCode, kDefaultSeamBlendDeg);
    }
    /// Parallax Blend in degrees (code 0 or out of range = the default 0.35).
    [[nodiscard]] double parallaxBlendDeg() const noexcept {
        return decodeSeamTool(parallaxBlend, 1, kMaxParallaxBlendCode, kDefaultParallaxBlendDeg);
    }
    /// Store a Parallax Blend (rounded, clamped to 0..4.0; 0 is a hard cut).
    void setParallaxBlendDeg(double deg) noexcept {
        parallaxBlend = encodeSeamTool(deg, 1, kMaxParallaxBlendCode, kDefaultParallaxBlendDeg);
    }
    /// Seam Smoothing in degrees (code 0 = off, the default).
    [[nodiscard]] double seamSmoothingDeg() const noexcept {
        return decodeSeamTool(seamSmoothing, 1, kMaxSeamSmoothingCode, 0.0);
    }
    /// Store a Seam Smoothing (rounded, clamped to 0..8.0; 0 = off = code 0).
    void setSeamSmoothingDeg(double deg) noexcept {
        seamSmoothing = encodeSeamTool(deg, 1, kMaxSeamSmoothingCode, 0.0);
    }
    /// Near Offset in degrees (out of range = 0, the default).
    [[nodiscard]] double nearOffsetDeg() const noexcept { return decodeSeamOffset(nearOffset); }
    /// Store a Near Offset (rounded to a hundredth, clamped to +/- 3.00;
    /// NaN / infinities = 0).
    void setNearOffsetDeg(double deg) noexcept { nearOffset = encodeSeamOffset(deg); }
    /// Far Offset in degrees.
    [[nodiscard]] double farOffsetDeg() const noexcept { return decodeSeamOffset(farOffset); }
    /// Store a Far Offset.
    void setFarOffsetDeg(double deg) noexcept { farOffset = encodeSeamOffset(deg); }

    // ---- [WP-VIGNETTE] ---------------------------------------------------------
    /// The lens shading correction mode.
    [[nodiscard]] PrefsLensShading lensShadingMode() const noexcept {
        return static_cast<PrefsLensShading>(lensShading);
    }
    /// Its strength in percent (code 0 or out of range = the default 100).
    [[nodiscard]] double shadingStrengthPercent() const noexcept {
        if (shadingStrength == 0 || shadingStrength > kMaxShadingStrengthCode) {
            return 100.0;
        }
        return static_cast<double>(shadingStrength - 1);
    }
    /// Store a strength in percent (rounded, clamped to 0..100); 100 is
    /// stored as code 0 so it keeps tracking the default.
    void setShadingStrengthPercent(double percent) noexcept {
        if (!(percent >= 0.0) || percent > 1e6) {
            shadingStrength = 0;  // NaN, infinities, negatives: the default
            return;
        }
        long p = static_cast<long>(percent + 0.5);
        if (p > 100) {
            p = 100;
        }
        shadingStrength = p == 100 ? std::uint8_t{0} : static_cast<std::uint8_t>(p + 1);
    }

private:
    /// A seam tool code as degrees: 0 or anything outside [minCode, maxCode]
    /// is the default, otherwise (code - 1) / kSeamToolStepsPerDeg - a
    /// division, so a code decodes to exactly the double its decimal literal
    /// is (30 -> 1.5, 7 -> 0.35).
    [[nodiscard]] static double decodeSeamTool(std::uint8_t code, std::uint8_t minCode, std::uint8_t maxCode,
                                               double defaultDeg) noexcept {
        if (code == 0 || code < minCode || code > maxCode) {
            return defaultDeg;
        }
        return static_cast<double>(code - 1) / static_cast<double>(kSeamToolStepsPerDeg);
    }
    /// Degrees as a seam tool code: rounded to a step, clamped to the code
    /// range, the default stored as 0; NaN / infinities are the default.
    [[nodiscard]] static std::uint8_t encodeSeamTool(double deg, std::uint8_t minCode, std::uint8_t maxCode,
                                                     double defaultDeg) noexcept {
        if (!(deg > -1e6 && deg < 1e6)) {
            return 0;  // NaN, infinities, absurd magnitudes: the default
        }
        long steps = static_cast<long>(deg * static_cast<double>(kSeamToolStepsPerDeg) + (deg >= 0.0 ? 0.5 : -0.5));
        steps = steps < static_cast<long>(minCode) - 1 ? static_cast<long>(minCode) - 1 : steps;
        steps = steps > static_cast<long>(maxCode) - 1 ? static_cast<long>(maxCode) - 1 : steps;
        const long defaultSteps =
            static_cast<long>(defaultDeg * static_cast<double>(kSeamToolStepsPerDeg) + 0.5);
        return steps == defaultSteps ? std::uint8_t{0} : static_cast<std::uint8_t>(steps + 1);
    }
    /// An offset in hundredths as degrees (out of range = 0).
    [[nodiscard]] static double decodeSeamOffset(std::int16_t hundredths) noexcept {
        if (hundredths > kMaxSeamOffsetHundredths || hundredths < -kMaxSeamOffsetHundredths) {
            return 0.0;
        }
        return static_cast<double>(hundredths) / 100.0;
    }
    /// Degrees as hundredths, rounded half away from zero and clamped.
    [[nodiscard]] static std::int16_t encodeSeamOffset(double deg) noexcept {
        if (!(deg > -1e6 && deg < 1e6)) {
            return 0;
        }
        long h = static_cast<long>(deg * 100.0 + (deg >= 0.0 ? 0.5 : -0.5));
        h = h > kMaxSeamOffsetHundredths ? kMaxSeamOffsetHundredths : h;
        h = h < -kMaxSeamOffsetHundredths ? -kMaxSeamOffsetHundredths : h;
        return static_cast<std::int16_t>(h);
    }
};

#pragma pack(pop)

static_assert(sizeof(PrefsBlob) == PrefsBlob::kSize, "PrefsBlob must be exactly 128 bytes");
static_assert(offsetof(PrefsBlob, exposureStops) == 16, "PrefsBlob layout drifted");
// parallax and flowBackend were taken from the front of the reserved block,
// which is what that block is for: the struct stays 128 bytes, every field
// before them keeps its offset, and a blob written by an older build still
// deserialises - its zero bytes read as Auto / Off there.
//
// Off is NOT the default for parallax (On is), so an old blob deliberately
// reads as "parallax disabled" rather than silently changing how an existing
// project renders.  A new blob gets On from defaults().
static_assert(offsetof(PrefsBlob, parallax) == 20, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, flowBackend) == 21, "PrefsBlob layout drifted");
// calibrationForceNative was taken from the front of the reserved block the
// same way.  Its zero in an old blob means "calibration 0 is Auto", which is
// exactly how calibration 0 always behaved (the importer passed no override
// for it and followed the recorded accessory), so an old project renders as
// before; a user who wants the native set regardless of the recording now
// has a choice that says so.
static_assert(offsetof(PrefsBlob, calibration) == 13, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, calibrationForceNative) == 22, "PrefsBlob layout drifted");
// directColour was taken from the reserved block the same way, at offset 24
// (offset 23 stays unused so each field keeps the offset it was built and
// tested against).  An older blob's zero byte reads as
// PrefsDirectColour::SequenceSpace, the default.
static_assert(offsetof(PrefsBlob, padAfterCalibration) == 23, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, directColour) == 24, "PrefsBlob layout drifted");
// [WP-FLARE] flareRemoval takes offset 30 of the range the harness assigned
// (30-31); 25-29 are padded for the packages whose ranges they are.  An older
// blob's zero byte reads as "off", which is how every project rendered
// before the removal existed; new blobs get "on" from defaults().
static_assert(offsetof(PrefsBlob, padBeforeFlare) == 25, "PrefsBlob layout drifted");
// [WP-LOOK] look takes offset 28 of its assigned range (28-29).  An older
// blob's zero byte reads as PrefsLook::DjiStudio, the default.
static_assert(offsetof(PrefsBlob, look) == 28, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, padAfterLook) == 29, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, flareRemoval) == 30, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, padAfterFlare) == 31, "PrefsBlob layout drifted");
// [WP-PHOTO] owns offsets 32-37 (docs/PARALLEL_WORK.md), taken from the front
// of the reserved block.  seamInset is the photometric seam fix's stage 1;
// zero means the default inset, so an old project gets the thinner seam band
// without a Source Settings visit.
static_assert(offsetof(PrefsBlob, seamInset) == 32, "PrefsBlob layout drifted");
// photoSeam's zero in an old blob reads as Off, so an old project renders as
// before (like parallax); a fresh blob gets RimAndGain.
static_assert(offsetof(PrefsBlob, photoSeam) == 33, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, photoStrength) == 34, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, photoReserved) == 35, "PrefsBlob layout drifted");
// [WP-SEAMTOOLS] The seam tools take offsets 38-45 from the front of the
// reserved block.  Every zero in an older blob reads as the default - the
// seam exactly as it rendered before the tools existed.
static_assert(offsetof(PrefsBlob, seamBlend) == 38, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, parallaxBlend) == 39, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, seamSmoothing) == 40, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, seamToolsPad) == 41, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, nearOffset) == 42, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, farOffset) == 44, "PrefsBlob layout drifted");
// [WP-VIGNETTE] The lens shading correction takes offsets 46-47 from the
// front of the reserved block.  lensShading's zero in an older blob reads as
// Off, so an old project renders as before; a fresh blob gets Auto.
static_assert(offsetof(PrefsBlob, lensShading) == 46, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, shadingStrength) == 47, "PrefsBlob layout drifted");
// [WP-HDRPEAK] hdrPeak takes offset 54 of its assigned range (54-55); 48-53
// are padded for the packages that own them.  An older blob's zero byte reads
// as PrefsHdrPeak::Nits1000 - no roll-off, the PQ output it always had.
static_assert(offsetof(PrefsBlob, padBeforeHdrPeak) == 48, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, hdrPeak) == 54, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, padAfterHdrPeak) == 55, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, reserved) == 56, "PrefsBlob layout drifted");
static_assert(static_cast<int>(PrefsHdrPeak::Nits1000) == 0, "zero must stay the no-roll-off default");
// The codes' upper bounds are the ranges the controls offer.
static_assert(PrefsBlob::kMaxSeamBlendCode == 8 * PrefsBlob::kSeamToolStepsPerDeg + 1, "Seam Blend reaches 8 deg");
static_assert(PrefsBlob::kMinSeamBlendCode == 4 + 1, "Seam Blend starts at 0.2 deg");
static_assert(PrefsBlob::kMaxParallaxBlendCode == 4 * PrefsBlob::kSeamToolStepsPerDeg + 1, "Parallax Blend reaches 4");
static_assert(PrefsBlob::kMaxSeamSmoothingCode == 8 * PrefsBlob::kSeamToolStepsPerDeg + 1, "Smoothing reaches 8 deg");

}  // namespace osv::premiere
