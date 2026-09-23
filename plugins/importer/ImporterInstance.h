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

#include "PixelCopy.h"
#include "PrefsBlob.h"

#include "osv/color/ColorParams.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/Blend.h"
#include "osv/geom/LensRig.h"
#include "osv/geom/Stabilization.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/Types.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/ParallaxWarp.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/SeamCarve.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/GpuClipDecoder.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

// Forward declaration: the pinned banded readback of the importer's GPU
// frame path (ImporterGpuFrame.h), shared by every clip on one device.
class GpuReadback;

/// Output geometry derived from the prefs and the clip's native size.
struct OutputGeometry {
    std::int32_t width = 0;   ///< Equirect width (2 x height).
    std::int32_t height = 0;  ///< Equirect height.

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
    [[nodiscard]] bool operator==(const OutputGeometry& o) const noexcept {
        return width == o.width && height == o.height;
    }
};

/// Why a frame is being rendered, which decides whether it may wait for the
/// parallax analysis.
///
/// The analysis costs ~220 ms of CPU per measurement.  A frame that will be
/// looked at closely or written to a file waits for it; a frame the user is
/// sweeping past must not.
enum class RenderPurpose {
    /// Export, a paused frame, anything whose pixels must be final: the
    /// analysis for this frame's bucket runs synchronously if it has not
    /// been measured yet, so the result does not depend on timing.
    Exact,
    /// Playback and scrubbing: never block on the analysis.  A missing
    /// measurement is queued for a background worker and the frame renders
    /// with the nearest one already available - or without, if none is.
    Interactive,
};

/// One rendered frame plus what it was rendered with, kept so a repeat
/// request for the same frame at the same settings does not decode again.
/// (The host's PPix cache is the primary cache; this one only survives a
/// host cache miss caused by a different pixel format request for the same
/// frame, which happens constantly while scrubbing.)
struct RenderedFrame {
    /// frameIndex of a cache holding no usable frame.  Never a real index -
    /// clips are far shorter than 2^32 frames - so it can never match.
    static constexpr std::uint32_t kNoFrame = 0xFFFFFFFFu;
    std::uint32_t frameIndex = kNoFrame;
    OutputGeometry geometry;
    PrefsBlob prefs = PrefsBlob::defaults();
    bool seamApplied = false;   ///< Whether the seam search ran for this frame.
    /// Whether the parallax correction was WANTED for this frame (prefs on and
    /// not a draft request) - part of the cache key for the same reason
    /// seamApplied is: the prefs blob alone does not distinguish a draft
    /// render from a full one, and a draft must never be served in its place.
    bool parallaxWanted = false;
    /// False when an Interactive render made do with a stand-in analysis -
    /// a neighbouring bucket's grid, or none while its own was still being
    /// measured.  An Exact request must never be served such a frame, or a
    /// paused frame or an export would inherit whatever happened to be ready
    /// while the user was scrubbing.
    bool exact = true;
    /// The OSV_TRANSFER_* the frame was rendered with when a single request
    /// overrode the clip's own (the host's "BT.709 RGB Full" connection
    /// space), -1 for the clip's own.  Part of the key: the override never
    /// touches `prefs`, so without this a Rec.709 fallback frame would be
    /// served to the next ordinary request of the same frame.
    int outputTransfer = -1;
    render::ImageRGBAf image;

    [[nodiscard]] bool matches(std::uint32_t index, const OutputGeometry& geom, const PrefsBlob& blob, bool wantSeam,
                               bool wantParallax, bool needExact, int transfer) const noexcept {
        return image.valid() && frameIndex == index && geometry == geom && prefs == blob && seamApplied == wantSeam &&
               parallaxWanted == wantParallax && (exact || !needExact) && outputTransfer == transfer;
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
    /// handle but keep the parsed metadata (imQuietFile).  The video reader
    /// is parked in video::ReaderPool rather than destroyed, so the next open
    /// of the same file - this instance's unquiet or a new instance - takes
    /// it back warm; the pool releases it after an idle minute.  Idempotent.
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
    /// seam search and the parallax correction for this frame regardless of
    /// the prefs (scrubbing and low quality requests).  `purpose` decides
    /// whether a missing parallax measurement is made now (Exact) or queued
    /// for the background worker (Interactive) - see RenderPurpose.
    ///
    /// The returned image is owned by the instance's frame cache and stays
    /// valid until the next renderFrame() call on this instance, so the
    /// caller must copy it out before releasing the lock.  The cache reuses
    /// one allocation across frames, so a stale pointer would read the NEXT
    /// frame's pixels, not freed memory - still wrong, hence the rule.
    ///
    /// `outputTransfer` is an OSV_TRANSFER_* id that overrides the clip's own
    /// output transfer for THIS frame only (the host's Rec.709 connection
    /// space), or a negative value for the clip's Source Settings choice.
    /// It never touches the prefs: an override that went through
    /// applyPrefsLocked() would be published to the direct-path engine as
    /// if the user had changed the clip, and would reset every analysis.
    ///
    /// The caller MUST hold lock() for the whole call and for its use of the
    /// returned reference.
    [[nodiscard]] Result<const render::ImageRGBAf*> renderFrame(std::uint32_t index, const OutputGeometry& geometry,
                                                                bool draft,
                                                                RenderPurpose purpose = RenderPurpose::Exact,
                                                                int outputTransfer = -1);

    /// Backend that served the last renderFrame() ("cpu", "cuda", "opencl";
    /// empty before the first frame).
    [[nodiscard]] std::string rendererName() const;

    // ---- [WP-IMPORTER] the importer's own frame, straight into the PPix ----
    /// How the importer's own frames reach the host (see renderFrameToHost).
    enum class FramePath : int {
        None = 0,  ///< No frame rendered yet.
        Gpu = 1,   ///< NVDEC -> VRAM -> CUDA stitch in place -> pinned banded readback into the PPix.
        Host = 2,  ///< Decoded to host memory (D3D11VA / NVDEC copy-back / software), uploaded, rendered, copied.
    };

    /// Decode + stitch frame `index` at `geometry` and write it into the host
    /// frame `dst` (a PPix's pixels: bottom-left, `format`).
    ///
    /// Prefers the GPU path: both lenses decoded on NVDEC into VRAM
    /// (video::GpuClipDecoder, in the primary context of the shared CUDA
    /// renderer's device), stitched by that renderer from the device planes
    /// with no upload, and streamed back through GpuReadback's pinned bands,
    /// each band converted into `dst` while the next is in flight.  The
    /// parameter block is built exactly as renderFrame() builds it, from the
    /// same analysis caches, so both paths stitch identically.
    ///
    /// Falls back to renderFrame() + PixelCopy (the host path) whenever the
    /// GPU path cannot serve the clip: no CUDA renderer (prefs choose CPU /
    /// OpenCL, or no NVIDIA GPU), a stream NVDEC does not take (the LRF
    /// proxy, 8-bit), OPENOSV_IMPORTER_NO_GPU_DECODE=1, or a GPU failure on
    /// this frame (three in a row switch the clip to the host path for good).
    ///
    /// The caller MUST hold lock() for the whole call.  `draft`, `purpose`
    /// and `outputTransfer` mean exactly what they mean for renderFrame().
    [[nodiscard]] Status renderFrameToHost(std::uint32_t index, const OutputGeometry& geometry, bool draft,
                                           RenderPurpose purpose, const pixelcopy::HostFrame& dst,
                                           pixelcopy::HostPixelFormat format, int outputTransfer = -1);

    /// The path that served the most recent renderFrameToHost() - readable
    /// without the lock (imAnalysis reports it).
    [[nodiscard]] FramePath lastFramePath() const noexcept {
        return static_cast<FramePath>(m_lastFramePath.load(std::memory_order_relaxed));
    }

    // ---- the direct GPU path (docs/DIRECT_GPU.md) --------------------------
    /// One frame for the direct renderer: both lenses decoded on NVDEC into a
    /// caller's CUDA context and the stitch state that goes with them.
    struct DirectFrame {
        /// Pins the decoded pair in the decoder's VRAM cache.  The planes in
        /// `job` point into it; they stay valid until the lease is released.
        video::GpuFrameLease lease;
        /// The stitch block exactly as the equirect path builds it for this
        /// frame (rig, colour, blend, gains, seam table or warp grid,
        /// stabilisation folded into Rout, equirect mode), the lens planes as
        /// DEVICE pointers (planesOnDevice), and the seam table / warp grid
        /// as host vectors for the caller to upload.
        render::RenderJob job;
        /// False when an Interactive request was served a stand-in analysis.
        bool exact = true;
    };

    /// Decode frame `index` into `cuContext` and build its stitch state.
    ///
    /// `cuContext` must be current on the calling thread (the engine pushes
    /// it): the analyses shade their bands from the device frames in that
    /// context.  `outputTransfer` is an OSV_TRANSFER_* id for the colour
    /// block, or a negative value for the clip's own Source Settings choice.
    /// Shares every analysis cache with renderFrame().  The caller MUST hold
    /// lock().
    [[nodiscard]] Result<DirectFrame> directFrame(std::uint32_t index, void* cuContext, RenderPurpose purpose,
                                                  int outputTransfer);

    /// Mark this instance as the engine's own (not one Premiere opened), so
    /// its prefs changes are not published back to the engine registry.
    void setEngineOwned(bool owned) noexcept { m_engineOwned = owned; }

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

    /// Create the DualStreamReader if it does not exist yet, on the fastest
    /// decode back-end this machine offers (see the implementation for the
    /// order and why random access is what decides it).
    [[nodiscard]] Status ensureReader();

    /// Decode frame `index` from both lenses.  If a hardware decoder fails,
    /// the clip is switched to software decoding for good and the frame is
    /// retried once, so a driver hiccup costs one slow frame instead of a
    /// "media offline" in the Program Monitor.
    [[nodiscard]] Result<video::FramePair> readPair(std::uint32_t index);

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

    /// [WP-PHOTO] Recompute m_renderBlend from m_blend and the prefs' seam
    /// edge inset.  Cheap (a few assignments); called by the render builders
    /// so the render blend can never lag a prefs or calibration change.
    void refreshRenderBlend() noexcept;

    /// What applyAnalyses() put into the builder.
    struct AnalysisOutcome {
        bool parallaxApplied = false;  ///< A 2-D warp grid (own, blended or borrowed) was applied.
        /// False when an Interactive request was served a stand-in (a
        /// neighbouring bucket's grid) because its own measurement is still
        /// running in the background; an Exact request must never reuse it.
        bool exact = true;
    };

    /// Run - or fetch from the per-bucket caches - the per-frame stitch
    /// analyses for frame `index` and apply them to `builder`: the 2-D
    /// parallax grid (bucketed, measured synchronously for an Exact request
    /// and in the background for an Interactive one), else the 1-D seam
    /// table, and the exposure gains.  `draft` turns the seam search and the
    /// parallax correction off exactly as renderFrame documents.
    ///
    /// `pair` may hold host frames (the equirect path) or device-resident
    /// frames (the direct GPU path); the analyses shade their bands from
    /// whichever the frames are, and the two paths share every cache.
    ///
    /// The caller MUST hold lock().  Failures of an individual analysis are
    /// logged and leave that correction out - they never fail the frame.
    [[nodiscard]] AnalysisOutcome applyAnalyses(std::uint32_t index, const video::FramePair& pair, bool draft,
                                                RenderPurpose purpose, ThreadPool& pool,
                                                render::RenderParamsBuilder& builder);

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
    /// The ANALYSIS blend: the calibrated FOV (195.18 deg) and its 4 deg
    /// feather.  Every measurement (parallax bands, seam search, gain, the
    /// photometric field) uses this one; narrowing it costs parallax quality.
    geom::BlendParams m_blend;
    /// [WP-PHOTO] The RENDER blend: m_blend with the Source Settings seam
    /// edge inset applied (render::insetRenderBlend), used ONLY by the two
    /// render builders (renderFrame, directFrame).  Refreshed from m_blend and
    /// the prefs by refreshRenderBlend() right before each use.
    geom::BlendParams m_renderBlend;
    std::vector<std::string> m_notes;
    std::uint32_t m_frameCount = 0;
    std::uint32_t m_rateNum = 0;
    std::uint32_t m_rateDen = 0;
    std::uint64_t m_creationTime1904 = 0;
    std::int32_t m_audioChannels = 0;
    double m_audioSampleRate = 0.0;
    std::int64_t m_audioDuration = 0;
    std::uint32_t m_audioTrackId = 0;

    /// The calibration choice the current rig was built for, so rebuildRig()
    /// can skip the work when the prefs did not touch it.  The choice rather
    /// than the stored byte: Auto and a forced Native share calibration 0.
    PrefsCalibrationChoice m_rigCalibration = PrefsCalibrationChoice::Auto;
    bool m_rigBuilt = false;

    /// Set once a hardware decoder has failed on this clip: every reader
    /// opened afterwards is software.  Deliberately NOT reset by
    /// releaseHeavy() - a quiet / unquiet does not make a GPU that could not
    /// decode this stream able to, and retrying would pay the failure again.
    bool m_hwDecodeFailed = false;

    // ---- heavy, dropped by releaseHeavy() ---------------------------------
    std::unique_ptr<video::DualStreamReader> m_reader;
    /// NVDEC decoders for the direct path, one per CUDA context that asked
    /// (Premiere uses one).  Frames decoded in one context are unusable in
    /// another, which is why this is keyed rather than shared.
    std::map<void*, std::unique_ptr<video::GpuClipDecoder>> m_gpuDecoders;
    /// True for the engine registry's own instance (see setEngineOwned).
    bool m_engineOwned = false;

    // ---- [WP-SETTINGS] Source Settings publication (Engine.h) -------------
    /// This instance's publisher token, taken at its first publication
    /// (right after imOpenFile8); 0 until then.  Larger means opened later,
    /// which is how the engine lets the newest instance of a file win.
    std::uint64_t m_settingsPublisher = 0;
    /// True once a blob the HOST handed over has been published from here.
    /// Until then the instance may have run on its defaults (a selector
    /// without prefs) and must publish the host's blob even when it happens
    /// to equal them, or an older instance's settings would stay in force.
    bool m_settingsPublishedFromHost = false;
    /// Publish m_prefs to the engine registry.  `fromHost`: the blob came
    /// from the host (false: the host gave none and the defaults are in
    /// force).  Never publishes from the engine's own instance.  The caller
    /// holds m_mutex.
    void publishSettingsLocked(bool fromHost) noexcept;
    // ---- [/WP-SETTINGS] ----------------------------------------------------
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

    // ---- analysis caches, keyed by BUCKET (render::parallaxBucket) --------
    /// Seam shift table per bucket (only populated when seamSearch is on).
    /// Keyed by bucket rather than frame: the seam search costs ~28 ms and a
    /// shift that changes from one frame to the next is noise, not signal,
    /// so one measurement per kParallaxBucketFrames frames loses nothing and
    /// removes ~7/8 of the cost.  Bounded: entries beyond kMaxAnalysisCache
    /// are dropped (never the one being inserted) so a long timeline cannot
    /// grow the instance without limit.
    std::map<std::uint32_t, std::vector<float>> m_seamTables;
    /// Per-lens linear gains per bucket (gainMatch).  Exposure drifts even
    /// more slowly than the seam, so the same bucketing applies.
    std::map<std::uint32_t, std::array<Vec3d, 2>> m_gains;
    static constexpr std::size_t kMaxAnalysisCache = 256;

    // ---- parallax analysis, shared with the background worker -------------
    //
    // LOCK ORDER: m_mutex, then m_parallaxMutex - never the reverse, and the
    // worker NEVER takes m_mutex.  That last rule is load-bearing:
    // releaseHeavy() holds m_mutex while it joins the worker, so a worker
    // that wanted m_mutex would deadlock the quiet.  Everything below is
    // guarded by m_parallaxMutex alone.

    /// One unit of background work: a frame's bands, OWNED, so the decoded
    /// frame they were cut from can be released as soon as they exist.
    struct ParallaxJob {
        std::uint32_t bucket = 0;
        std::uint64_t generation = 0;  ///< m_parallaxGeneration when queued.
        render::LensBands bands;
        render::ParallaxWarpParams params;
        double bandMs = 0.0;
    };

    /// Measured parallax per bucket.  A PRESENT entry holding nullptr records
    /// a measurement that was REFUSED (too little consistent flow - open sky)
    /// so the bucket is not measured again; an ABSENT entry means not yet
    /// measured.  shared_ptr so a render can keep using a grid while the
    /// worker trims the cache under it.  A grid is ~100 KB, so 64 buckets is
    /// ~6 MB and covers ~8.5 s at 60 fps.
    std::map<std::uint32_t, std::shared_ptr<const render::ParallaxWarpGrid>> m_parallaxGrids;
    static constexpr std::size_t kMaxParallaxCache = 64;
    /// An Interactive frame whose own bucket is not measured yet may borrow
    /// the nearest measured grid up to this many buckets away (32 frames,
    /// ~0.5 s at 60 fps).  Further than that the scene near the seam may
    /// have changed, and the seam table is the safer fallback.
    static constexpr std::uint32_t kParallaxBorrowBuckets = 4;

    mutable std::mutex m_parallaxMutex;
    std::condition_variable m_parallaxCv;
    std::thread m_parallaxWorker;  ///< Started lazily; joined by stopParallaxWorker().
    /// The next job.  A single slot, LATEST WINS: while scrubbing only the
    /// frame the user stopped on matters, so a newer request replaces an
    /// older one that has not started rather than queueing behind it.
    std::optional<ParallaxJob> m_parallaxPending;
    std::optional<std::uint32_t> m_parallaxBusyBucket;  ///< Bucket being measured right now.
    bool m_parallaxStop = false;
    /// Bumped whenever the measurements become invalid (prefs change, quiet),
    /// so a job that was already running for the old settings has its result
    /// discarded instead of polluting the fresh cache.
    std::uint64_t m_parallaxGeneration = 0;

    /// The worker's body.  Takes only m_parallaxMutex (see LOCK ORDER).
    void parallaxWorkerLoop() noexcept;
    /// Stop and join the worker, dropping any job not yet started.  Safe to
    /// call when no worker is running.  May block for one in-flight
    /// measurement (~220 ms).  The caller may hold m_mutex; it must NOT hold
    /// m_parallaxMutex.
    void stopParallaxWorker() noexcept;
    /// Invalidate every measurement: clear the cache, drop the pending job
    /// and bump the generation.  Takes m_parallaxMutex.
    void resetParallaxLocked() noexcept;

    // ---- [WP-SEAM] carved blend seam, keyed by bucket ------------------------
    /// Carved seams per bucket (SeamCarve.h), each measured through its own
    /// bucket's correction (its parallax grid, else its seam table).  Guarded
    /// by m_parallaxMutex and cleared with the parallax state in
    /// resetParallaxLocked(), because a seam is only as current as the
    /// correction it was carved through.  ~8 KB each.
    std::map<std::uint32_t, std::shared_ptr<const render::BlendSeam>> m_blendSeams;

    /// Carve (or fetch) the blend seam for frame `index` and hand it to
    /// `builder`: glided from the previous bucket's seam like the parallax
    /// grid, steered by a neighbouring bucket's seam when one is cached.  An
    /// Interactive request whose bucket cannot be carved yet (its parallax
    /// grid is still being measured) borrows a nearby bucket's seam and
    /// clears `frameExact`.  Called by applyAnalyses with m_mutex held;
    /// takes m_parallaxMutex itself.  Failures are logged and leave the frame
    /// on the ordinary feather.
    void applyCarvedSeam(std::uint32_t index, const video::FramePair& pair, bool wantParallax, RenderPurpose purpose,
                         ThreadPool& pool, render::RenderParamsBuilder& builder, bool& frameExact);
    // ---- [/WP-SEAM] ----------------------------------------------------------

    // ---- [WP-PHOTO] photometric seam field, keyed by bucket ------------------
    // (docs/research/NEURAL_STITCHING.md section 8, render/PhotoSeam.h.)  All
    // guarded by m_mutex: the field is measured synchronously on the render
    // thread (a band shade plus a few ms of statistics per bucket), so no
    // worker ever touches it.

    /// Per-bucket fields (EMA'd) and the clip's accumulated usable rim.
    render::PhotoSeamHistory m_photo;
    /// The rig the fields were measured with (intrinsics and extrinsics,
    /// flattened); a different rig - another calibration set, a protector
    /// correction - clears m_photo, because every rim and gain is a property
    /// of the lenses as calibrated.
    std::vector<double> m_photoRigKey;
    /// The field chosen for the frame being built (preparePhotoSeam ->
    /// applyPhotoSeam), and whether it is the frame's own (false: a draft
    /// borrowed the last accepted one).
    std::shared_ptr<const render::PhotoSeamField> m_photoFrame;
    bool m_photoFrameExact = true;
    /// The last field a non-draft frame used: what a draft renders with.
    std::shared_ptr<const render::PhotoSeamField> m_photoLast;

    /// The analysis parameters for the current prefs (mode, strength).
    [[nodiscard]] render::PhotoSeamParams photoParamsLocked() const noexcept;

    /// First half of the per-frame hook, called at the top of applyAnalyses:
    /// measure this frame's bucket if it has not been (never for a draft),
    /// choose the field the frame renders with, and return the scope that
    /// makes its usable rim the carved seam's Rim cost on this thread for
    /// the rest of applyAnalyses.  Failures are logged and leave the frame
    /// on the stage-1 inset and the global gain.
    [[nodiscard]] render::PhotoRimPenaltyScope preparePhotoSeam(std::uint32_t index, const video::FramePair& pair,
                                                                bool draft, ThreadPool& pool);

    /// Second half, the last line of applyAnalyses: hand the chosen field to
    /// `builder` - its rim replaces the stage-1 inset (the analysis blend is
    /// restored) and, in RimAndGain, its gain replaces the global one.
    /// Returns false when the frame used a stand-in field.
    [[nodiscard]] bool applyPhotoSeam(render::RenderParamsBuilder& builder);
    // ---- [/WP-PHOTO] ----------------------------------------------------------

    RenderedFrame m_lastFrame;
    std::string m_rendererName;
    // Atomic because both are touched by selectors that do NOT hold m_mutex
    // (imGetSourceVideo bumps the counter and reads the id before it takes
    // the lock), and Premiere runs those on several threads for one clip.
    // Neither participates in an invariant with the rest of the instance, so
    // an atomic is sufficient - see the accessors above.
    std::atomic<std::uint64_t> m_videoRequests{0};
    std::atomic<std::uint32_t> m_importerId{0};

    // ---- [WP-IMPORTER] the importer's own frame on the GPU -----------------
    /// Build the equirect stitch job for frame `index` at `geometry` from
    /// `pair` (host or device frames): rig, colour, blend, coverage alpha,
    /// the per-bucket analyses, stabilisation and the equirect map, in
    /// exactly the order renderFrame() has always used.  Shared by both
    /// paths so they can never assemble different parameter blocks.
    /// `outputTransfer` as for renderFrame().  `outcome` receives what
    /// applyAnalyses() did.  Caller holds m_mutex.
    [[nodiscard]] Result<render::RenderJob> buildEquirectJob(std::uint32_t index, const video::FramePair& pair,
                                                             const OutputGeometry& geometry, bool draft,
                                                             RenderPurpose purpose, ThreadPool& pool,
                                                             AnalysisOutcome& outcome, int outputTransfer);

    /// The colour block for one frame: the clip's own (m_color) for a
    /// negative `outputTransfer`, otherwise the same block rebuilt with that
    /// OSV_TRANSFER_* id - built exactly as rebuildColor() builds m_color,
    /// so the two can only ever differ by the transfer.  Caller holds m_mutex.
    [[nodiscard]] OsvColorParams colorForTransfer(int outputTransfer) const;

    /// The GPU path of renderFrameToHost().  Returns true when it served the
    /// frame, false when it does not apply to this clip or request (the caller
    /// then takes the host path).  A failure after the path was chosen is an
    /// Error the caller logs before falling back.  Caller holds m_mutex.
    [[nodiscard]] Result<bool> renderFrameOnGpu(std::uint32_t index, const OutputGeometry& geometry, bool draft,
                                                RenderPurpose purpose, const pixelcopy::HostFrame& dst,
                                                pixelcopy::HostPixelFormat format, int outputTransfer);

    /// Where the GPU path stands for this clip.
    enum class GpuFrameState : std::uint8_t {
        Untried,   ///< Not attempted yet.
        Active,    ///< Serving frames.
        Disabled,  ///< Refused for good (reason logged once): the host path serves every frame.
    };
    GpuFrameState m_gpuFrameState = GpuFrameState::Untried;
    /// Consecutive GPU-path failures; three switch the clip to the host path.
    std::uint32_t m_gpuFrameFailures = 0;
    /// Key of this path's decoder in m_gpuDecoders (the retained primary
    /// context of the renderer's device).  Kept in that map on purpose:
    /// releaseHeavy() already frees every decoder there on imQuietFile, which
    /// is exactly when this one's VRAM should go back too.
    void* m_gpuFrameContext = nullptr;
    /// The shared pinned readback; held so it lives exactly as long as some
    /// clip may still use it (released with the instance).
    std::shared_ptr<GpuReadback> m_gpuReadback;
    /// FramePath of the last renderFrameToHost(), atomic for lastFramePath().
    std::atomic<int> m_lastFramePath{0};
};

}  // namespace osv::premiere
