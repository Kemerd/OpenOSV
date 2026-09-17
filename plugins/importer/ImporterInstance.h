// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterInstance: everything one opened .OSV / .LRF clip owns inside the
// host process (docs/PREMIERE.md, "Importer design" > "Registration and
// lifetime").
//
// Premiere hands the importer a `void* privatedata` on every selector.  That
// pointer is a host handle (piSuites->memFuncs->newHandle) whose payload is a
// single ImporterInstance* - the C++ object lives on our own heap because the
// host handle may be relocated and because an ImporterInstance holds
// non-trivially-destructible members (decoders, renderer leases, vectors).
//
// Lifetime, exactly as the design doc requires:
//
//   imOpenFile8  -> create (or reuse) the instance, open the OS handle and
//                   parse the container header.  A file that is not a
//                   dual-fisheye OSV fails here so a lower-priority importer
//                   can still claim it.
//   imGetInfo8   -> fill the host's info record from the parsed metadata.
//   imGetSourceVideo -> ensureReader() + ensureRenderer() on first use, then
//                   decode + stitch under m_mutex.
//   imQuietFile  -> releaseHeavy(): drop the decoders, the renderer lease and
//                   the OS handle; keep the parsed metadata so the next
//                   unquiet is cheap.
//   imCloseFile  -> destroy.
//
// Concurrency: one std::mutex per instance serialises decode + render, so
// frames for two different clips still run in parallel (the renderers
// themselves come from the process-wide HostContext and are internally
// serialised).  Nothing here is global.
//
// The prefs blob is a *snapshot*: the host owns the bytes and may hand back a
// different blob on any call, so every entry point that receives prefs calls
// applyPrefs() first and the instance rebuilds whatever the change
// invalidated (colour parameters, output size, seam cache).
#pragma once

#include "PrefsBlob.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Result.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/Stabilization.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/video/DualStreamReader.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace osv::premiere {

// Forward declaration: the audio side lives in its own translation unit and
// owns a second AVFormatContext on the same file.
class AudioDecoder;

/// Output geometry derived from the prefs and the clip's native size.
struct OutputGeometry {
    std::int32_t width = 0;   ///< Equirect width (2 x height).
    std::int32_t height = 0;  ///< Equirect height.

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
    [[nodiscard]] bool operator==(const OutputGeometry& o) const noexcept {
        return width == o.width && height == o.height;
    }
};

/// One rendered frame plus what it was rendered with, kept so a repeat
/// request for the same frame at the same settings does not decode again.
/// (The host's PPix cache is the primary cache; this one only survives a
/// host cache miss caused by a different pixel format request for the same
/// frame, which happens constantly while scrubbing.)
struct RenderedFrame {
    std::uint32_t frameIndex = 0xFFFFFFFFu;
    OutputGeometry geometry;
    PrefsBlob prefs = PrefsBlob::defaults();
    bool seamApplied = false;   ///< Whether the seam search ran for this frame.
    render::ImageRGBAf image;

    [[nodiscard]] bool matches(std::uint32_t index, const OutputGeometry& geom, const PrefsBlob& blob,
                               bool wantSeam) const noexcept {
        return image.valid() && frameIndex == index && geometry == geom && prefs == blob && seamApplied == wantSeam;
    }
};

class ImporterInstance {
public:
    /// Construct around an already-validated path.  open() does the work.
    explicit ImporterInstance(std::filesystem::path path);
    ~ImporterInstance();

    ImporterInstance(const ImporterInstance&) = delete;
    ImporterInstance& operator=(const ImporterInstance&) = delete;

    /// Open the OS handle, map the container, parse the metadata track, pick
    /// the calibration set and build the lens rig.  Fails with Io for an
    /// unreadable file and Malformed for anything that is not a dual-fisheye
    /// OSV (the caller then returns imBadFile after closing the handle).
    /// Safe to call again after releaseHeavy(); the parsed state is reused.
    [[nodiscard]] Status open();

    /// Drop the decoders, the renderer lease, the audio decoder and the OS
    /// handle but keep the parsed metadata (imQuietFile).  Idempotent.
    void releaseHeavy() noexcept;

    /// True between a successful open() and releaseHeavy().
    [[nodiscard]] bool isOpen() const noexcept { return m_fileHandle != INVALID_HANDLE_VALUE; }

    /// The OS handle Premiere stores in imFileAccessRec8::fileref.  The
    /// instance keeps ownership; releaseHeavy() closes it.
    [[nodiscard]] HANDLE fileHandle() const noexcept { return m_fileHandle; }

    // There is deliberately NO detachFileHandle(): this importer never hands
    // its OS handle to the host.  imQuietFile and imCloseFile both go through
    // releaseHeavy(), which closes it, and a detach method would be a trap -
    // it clears m_fileHandle, so releaseHeavy()'s CloseHandle becomes a no-op
    // and any caller that dropped the returned handle would leak one kernel
    // object per clip open.  If a file-manager path ever needs it, add it
    // together with the code that takes ownership, not before.

    // ---- parsed clip facts -------------------------------------------------
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] const meta::FormatInfo& format() const noexcept { return m_format; }
    [[nodiscard]] const meta::MetadataTrack& metadata() const noexcept { return m_track; }
    [[nodiscard]] const geom::LensRig& rig() const noexcept { return m_rig; }
    [[nodiscard]] bool parsed() const noexcept { return m_parsed; }

    /// Number of video frames (0 before open()).
    [[nodiscard]] std::uint32_t frameCount() const noexcept { return m_frameCount; }

    /// Frame rate as the exact container rational: numerator / denominator
    /// (60000 / 1001 for the sample clip).  Both are > 0 after open().
    [[nodiscard]] std::uint32_t rateNumerator() const noexcept { return m_rateNum; }
    [[nodiscard]] std::uint32_t rateDenominator() const noexcept { return m_rateDen; }

    /// Frames per second as a double (rateNumerator / rateDenominator).
    [[nodiscard]] double fps() const noexcept;

    /// Native equirect output size: 2 x decoded lens height by lens height.
    ///
    /// Takes m_mutex.  It has to: the answer depends on m_reader, which
    /// imQuietFile resets under that same lock, and a reader that takes no
    /// lock gets no mutual exclusion from a writer that does.  The unlocked
    /// version used to be reachable from imGetInfo8 and imGetPreferredFrameSize
    /// while imQuietFile ran on the media-cache thread, which is a read of a
    /// unique_ptr being destroyed.
    [[nodiscard]] OutputGeometry nativeGeometry() const noexcept;

    /// Output size for a prefs blob (Native / 4K / 2K).  Takes m_mutex.
    [[nodiscard]] OutputGeometry geometryFor(const PrefsBlob& prefs) const noexcept;

    /// Same two, for callers that ALREADY hold m_mutex.  The mutex is a plain
    /// std::mutex (see the class comment for why it is not recursive), so a
    /// locked caller must use these or it self-deadlocks.
    [[nodiscard]] OutputGeometry nativeGeometryLocked() const noexcept;
    [[nodiscard]] OutputGeometry geometryForLocked(const PrefsBlob& prefs) const noexcept;

    // ---- audio -------------------------------------------------------------
    [[nodiscard]] bool hasAudio() const noexcept { return m_audioChannels > 0; }
    [[nodiscard]] std::int32_t audioChannels() const noexcept { return m_audioChannels; }
    [[nodiscard]] double audioSampleRate() const noexcept { return m_audioSampleRate; }
    /// Duration in audio sample frames (0 without audio).
    [[nodiscard]] std::int64_t audioDurationSamples() const noexcept { return m_audioDuration; }

    /// The audio decoder, created on first use.  Null when the clip has no
    /// audio track or the decoder could not be opened (the caller then
    /// zero-fills, which is what the host expects for a silent clip).
    /// Takes lock() itself.
    [[nodiscard]] AudioDecoder* audio();

    /// Same, for a caller that already holds lock().  Calling audio() under
    /// the lock would deadlock on the non-recursive mutex.
    [[nodiscard]] AudioDecoder* audioLocked();

    // ---- container timing --------------------------------------------------
    /// Creation time of the movie in seconds since 1904-01-01 UTC (0 when
    /// the container carries none).
    [[nodiscard]] std::uint64_t creationTime1904() const noexcept { return m_creationTime1904; }

    // ---- prefs -------------------------------------------------------------
    /// Adopt a prefs blob received from the host.  Rebuilds the colour
    /// parameters, the stabilisation setup and the calibration selection when
    /// the relevant fields changed, and drops the frame cache when the output
    /// would differ.  Passing null keeps the current blob (some selectors do
    /// not carry prefs).  Takes lock() itself.
    void applyPrefs(const void* bytes, std::size_t length);

    /// The blob currently in force (defaults before the first applyPrefs()).
    /// Takes lock() itself.
    [[nodiscard]] PrefsBlob prefs() const;

    // ---- the same two, for a caller that already holds lock() -------------
    //
    // m_mutex is a plain std::mutex, so a locking method called from inside
    // the lock throws system_error("resource deadlock would occur") on MSVC
    // rather than blocking.  Every entry point therefore uses exactly one of
    // the two forms: the plain one when it holds no lock, the *Locked one
    // when it does.  (A recursive_mutex would hide the mistake instead of
    // making it impossible, and would let a half-updated instance be observed
    // from a nested call.)
    void applyPrefsLocked(const void* bytes, std::size_t length);
    [[nodiscard]] PrefsBlob prefsLocked() const noexcept;

    // ---- rendering ---------------------------------------------------------
    /// Decode + stitch frame `index` at `geometry`.  `draft` disables the
    /// seam search for this frame regardless of the prefs (scrubbing and low
    /// quality requests).  The returned image is owned by the instance's
    /// frame cache and stays valid until the next renderFrame() call on this
    /// instance, so the caller must copy it out before releasing the lock.
    ///
    /// The caller MUST hold lock() for the whole call and for its use of the
    /// returned reference.
    [[nodiscard]] Result<const render::ImageRGBAf*> renderFrame(std::uint32_t index, const OutputGeometry& geometry,
                                                                bool draft);

    /// Backend that served the last renderFrame() ("cpu", "cuda", "opencl";
    /// empty before the first frame).
    [[nodiscard]] std::string rendererName() const;

    /// The per-instance lock.  Every entry point that decodes, renders or
    /// touches the reader takes it.
    [[nodiscard]] std::mutex& lock() noexcept { return m_mutex; }

    /// Human-readable summary for imAnalysis (camera, mode, colour, lens,
    /// frame rate, calibration slot).  CR/LF line endings as the host wants.
    [[nodiscard]] std::string analysisText() const;

    /// Diagnostics gathered while opening (calibration warnings, scaling
    /// notes); written to the log once after open().
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return m_notes; }

    /// Number of imGetSourceVideo calls served so far (drives the "log the
    /// first few requests" rule).
    ///
    /// Atomic rather than lock-guarded: Premiere issues imGetSourceVideo for
    /// one clip from several decode threads at once and the handler bumps
    /// this BEFORE it takes the instance lock, so a plain ++ is a genuine
    /// read-modify-write race.  The counter takes part in no invariant with
    /// the rest of the instance, so an atomic is both sufficient and free -
    /// relaxed ordering, because nothing is published through it.
    [[nodiscard]] std::uint64_t videoRequestCount() const noexcept {
        return m_videoRequests.load(std::memory_order_relaxed);
    }
    void noteVideoRequest() noexcept { m_videoRequests.fetch_add(1, std::memory_order_relaxed); }

    /// The importer id Premiere assigns this instance (imFileOpenRec8 ::
    /// inImporterID, mirrored into imImageInfoRec::importerID).  It keys
    /// every PPix cache entry, so a zero id means "do not use the cache".
    ///
    /// Atomic for the same reason: it is written from imGetInfo8 and
    /// imOpenFile8 and read from imGetSourceVideo, none of which hold the
    /// instance lock at that point.  A torn read here would silently produce
    /// a wrong cache key.
    [[nodiscard]] std::uint32_t importerId() const noexcept { return m_importerId.load(std::memory_order_relaxed); }
    void setImporterId(std::uint32_t id) noexcept {
        if (id != 0) {
            m_importerId.store(id, std::memory_order_relaxed);
        }
    }

    /// Extra memory the instance holds while open, reported through
    /// imFileOpenRec8::outExtraMemoryUsage so the host's cache accounting is
    /// not blind to our decoders and frame cache.
    [[nodiscard]] std::size_t extraMemoryUsage() const noexcept;

private:
    /// Parse the container, metadata, calibration and rig (once).
    [[nodiscard]] Status parseOnce();

    /// Create the DualStreamReader if it does not exist yet.
    [[nodiscard]] Status ensureReader();

    /// The body of audio() / audioLocked(); assumes the lock is held.
    [[nodiscard]] AudioDecoder* audioImpl();

    /// Recompute m_color from the current prefs.
    void rebuildColor();

    /// Re-select the calibration set and rebuild the rig for the current
    /// prefs (calibration slot).  Cheap; only runs when the slot changed.
    [[nodiscard]] Status rebuildRig();

    /// Build (or rebuild) the attitude track for the current stabilisation
    /// mode.  A clip without IMU samples silently degrades to no
    /// stabilisation and records a note.
    void rebuildStabilization();

    /// Body-from-world correction for a frame index (identity when off).
    [[nodiscard]] Mat3d stabilizationFor(std::uint32_t frameIndex) const;

    std::filesystem::path m_path;
    HANDLE m_fileHandle = INVALID_HANDLE_VALUE;

    mutable std::mutex m_mutex;

    // ---- parsed once, kept across quiet/unquiet ---------------------------
    bool m_parsed = false;
    std::unique_ptr<OsvFile> m_file;
    meta::MetadataTrack m_track;
    meta::FormatInfo m_format;
    meta::CalibrationSet m_calibration;
    geom::LensRig m_rig;
    geom::BlendParams m_blend;
    std::vector<std::string> m_notes;
    std::uint32_t m_frameCount = 0;
    std::uint32_t m_rateNum = 0;
    std::uint32_t m_rateDen = 0;
    std::uint64_t m_creationTime1904 = 0;
    std::int32_t m_audioChannels = 0;
    double m_audioSampleRate = 0.0;
    std::int64_t m_audioDuration = 0;
    std::uint32_t m_audioTrackId = 0;

    /// The calibration slot the current rig was built for, so rebuildRig()
    /// can skip the work when the prefs did not touch it.
    PrefsCalibration m_rigCalibration = PrefsCalibration::Native;
    bool m_rigBuilt = false;

    // ---- heavy, dropped by releaseHeavy() ---------------------------------
    std::unique_ptr<video::DualStreamReader> m_reader;
    std::unique_ptr<AudioDecoder> m_audio;
    bool m_audioProbed = false;

    // ---- prefs derived ----------------------------------------------------
    PrefsBlob m_prefs = PrefsBlob::defaults();
    OsvColorParams m_color{};
    bool m_colorBuilt = false;
    geom::StabilizationParams m_stabParams;
    std::optional<geom::AttitudeTrack> m_attitude;
    std::vector<Quatd> m_smoothedAttitude;
    Quatd m_referenceAttitude;
    PrefsStabilization m_stabBuiltFor = PrefsStabilization::Off;
    bool m_stabBuilt = false;

    // ---- per-frame analysis caches ----------------------------------------
    /// Seam shift table per frame index (only populated when seamSearch is
    /// on).  Bounded: entries beyond kMaxAnalysisCache are dropped oldest
    /// first so a long timeline cannot grow the instance without limit.
    std::map<std::uint32_t, std::vector<float>> m_seamTables;
    /// Per-lens linear gains per frame index (gainMatch).
    std::map<std::uint32_t, std::array<Vec3d, 2>> m_gains;
    static constexpr std::size_t kMaxAnalysisCache = 256;

    RenderedFrame m_lastFrame;
    std::string m_rendererName;
    // Atomic because both are touched by selectors that do NOT hold m_mutex
    // (imGetSourceVideo bumps the counter and reads the id before it takes
    // the lock), and Premiere runs those on several threads for one clip.
    // Neither participates in an invariant with the rest of the instance, so
    // an atomic is sufficient - see the accessors above.
    std::atomic<std::uint64_t> m_videoRequests{0};
    std::atomic<std::uint32_t> m_importerId{0};
};

}  // namespace osv::premiere
