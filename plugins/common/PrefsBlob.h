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

/// Lens calibration slot.
enum class PrefsCalibration : std::uint8_t {
    Native = 0,
    LensGuards = 1,
    Underwater = 2,
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

/// Renderer selection; the numeric values are the ones stored in the blob
/// and match HostContext's RenderDevicePreference.
enum class PrefsRenderDevice : std::uint8_t {
    Auto = 0,
    Cpu = 1,
    Cuda = 2,
    OpenCl = 3,
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
    std::uint8_t reserved[108] = {};   ///< Zero; future fields.

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
        p.calibration = static_cast<std::uint8_t>(PrefsCalibration::Native);
        p.dlogmFit = static_cast<std::uint8_t>(PrefsDlogmFit::Osmo360);
        p.exposureStops = 0.0f;
        p.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Auto);
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
        clampEnum(calibration, static_cast<std::uint8_t>(PrefsCalibration::Count), 0);
        clampEnum(dlogmFit, static_cast<std::uint8_t>(PrefsDlogmFit::Count), 0);
        clampEnum(renderDevice, static_cast<std::uint8_t>(PrefsRenderDevice::Count), 0);

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
};

#pragma pack(pop)

static_assert(sizeof(PrefsBlob) == PrefsBlob::kSize, "PrefsBlob must be exactly 128 bytes");
static_assert(offsetof(PrefsBlob, exposureStops) == 16, "PrefsBlob layout drifted");
static_assert(offsetof(PrefsBlob, reserved) == 20, "PrefsBlob layout drifted");

}  // namespace osv::premiere
