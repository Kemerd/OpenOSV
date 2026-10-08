// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterInstance implementation.  The call sequence mirrors
// tools/osvtool/Pipeline.cpp exactly, so a Premiere frame and an
// `osvtool render --mode equirect` frame of the same clip at the same
// settings are the same pixels.

#include "ImporterInstance.h"

#include "ImporterAudio.h"

#include "Engine.h"

#include "HostContext.h"
// The importer's own GPU frame path: pinned banded readback, the lock around
// the shared renderer's output, the context scope.
#include "ImporterGpuFrame.h"
// colorSpaceTokenFor(): the per-clip colour log line names the exact token the
// importer will hand Premiere, so the log and imGetIndColorSpace can never
// disagree about what the host was told.  A build of this clip engine for a
// host that is not Premiere (the OpenFX plug-in, plugins/ofx) has no token to
// hand anyone and no Adobe SDK to spell one with: it defines
// OSV_CLIP_ENGINE_WITHOUT_PREMIERE, and the log line says so instead.
#if !defined(OSV_CLIP_ENGINE_WITHOUT_PREMIERE)
#include "ImporterPlugin.h"
#endif
#include "PluginLog.h"
#include "ProtectorGuard.h"

#include "osv/color/AutoDetect.h"
#include "osv/geom/ConventionProbe.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/LensProtector.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/render/PhotoSeam.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/GpuDecoderPool.h"
#include "osv/video/ReaderPool.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaAnalysis.h"
#include "osv/render/FlareCuda.h"  // [WP-FLARE]
#include "osv/render/CudaRenderer.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace osv::premiere {

namespace {

/// Close a clip's OS handle and mark it closed; a closed handle is left
/// alone.  CloseHandle on Windows, close() of the descriptor elsewhere.
void closeClipFile(ClipFileHandle& handle) noexcept {
    if (handle == invalidClipFileHandle()) {
        return;
    }
#if defined(_WIN32)
    ::CloseHandle(handle);
#else
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle)));
#endif
    handle = invalidClipFileHandle();
}

/// Keep a bounded analysis cache: once it grows past `limit` the lowest keys
/// (bucket indices - every analysis cache is keyed by bucket) are dropped.  Scrubbing walks forward and backward, so
/// dropping the numerically smallest keys is as good a policy as any and
/// costs nothing to implement on a std::map.
///
/// `keep` is the key the caller has just inserted and is about to read
/// through the iterator emplace() returned.  It is NEVER evicted: when it is
/// itself the smallest key - scrubbing backwards past the limit - the
/// LARGEST key goes instead.  The first version had no such guard, so that
/// case erased the fresh entry and left the caller dereferencing a dangling
/// iterator; the 65-frame sample clip never reaches the limit, which is why
/// no test saw it.
template <class MapT>
void trimAnalysisCache(MapT& cache, std::size_t limit, const typename MapT::key_type& keep) {
    while (cache.size() > limit && !cache.empty()) {
        auto victim = cache.begin();
        if (victim->first == keep) {
            victim = std::prev(cache.end());
            if (victim->first == keep) {
                break;  // the only entry left is the one being kept
            }
        }
        cache.erase(victim);
    }
}

// ---------------------------------------------------------------------------
//  [WP-TEMPORAL] helpers for the anchored per-bucket analyses
// ---------------------------------------------------------------------------
// (The glide's own pieces - the zero grid a refused bucket stands for and the
// seam tables' agreement-weighted glide - live in render/ParallaxWarp.h,
// where they are unit tested.)

/// Clears a pointer member when a scope ends (the analysis GPU decoder is
/// only valid while its caller's frame is being analysed).
template <class T>
class ScopedPointer {
public:
    ScopedPointer(T*& slot, T* value) noexcept : m_slot(slot) { m_slot = value; }
    ~ScopedPointer() { m_slot = nullptr; }
    ScopedPointer(const ScopedPointer&) = delete;
    ScopedPointer& operator=(const ScopedPointer&) = delete;

private:
    T*& m_slot;
};

/// Map the prefs enum onto the library's flow backend.  The numeric values
/// match by design (PrefsBlob.h says so), but an explicit switch means a
/// renumbering on either side fails loudly here instead of silently selecting
/// the wrong solver.
[[nodiscard]] render::FlowBackendKind toFlowBackendKind(PrefsFlowBackend kind) noexcept {
    switch (kind) {
    case PrefsFlowBackend::Classical: return render::FlowBackendKind::Classical;
    case PrefsFlowBackend::Neural:    return render::FlowBackendKind::Neural;
    case PrefsFlowBackend::Auto:
    case PrefsFlowBackend::Count:
    default:                          return render::FlowBackendKind::Auto;
    }
}

/// Map the prefs enum onto the library's stabilisation mode.
[[nodiscard]] geom::StabilizationMode toStabMode(PrefsStabilization mode) noexcept {
    switch (mode) {
    case PrefsStabilization::HorizonLock: return geom::StabilizationMode::HorizonLock;
    case PrefsStabilization::Full:        return geom::StabilizationMode::Full;
    case PrefsStabilization::Smooth:      return geom::StabilizationMode::Smooth;
    case PrefsStabilization::SmoothLevel: return geom::StabilizationMode::SmoothLevel;
    case PrefsStabilization::Off:
    case PrefsStabilization::Count:
    default:                              return geom::StabilizationMode::Off;
    }
}

/// Map the prefs enum onto the library's output transfer.
[[nodiscard]] color::OutputTransfer toOutputTransfer(PrefsColorOutput out) noexcept {
    switch (out) {
    case PrefsColorOutput::HLG:    return color::OutputTransfer::HLG;
    case PrefsColorOutput::Rec709: return color::OutputTransfer::Rec709;
    // Passthrough bypasses the transfer AND the primaries matrix (see
    // osvCodeToOutput in ColorMath.h), so the frame stays in the camera's
    // own D-Log M encoding and its own gamut - which is exactly what a
    // downstream D-Log M LUT expects to be fed.
    case PrefsColorOutput::DLogM: return color::OutputTransfer::Passthrough;
    case PrefsColorOutput::PQ:
    case PrefsColorOutput::Count:
    default:                       return color::OutputTransfer::PQ;
    }
}

/// Map the prefs enum onto the library's D-Log M curve fit.
[[nodiscard]] color::DlogMFit toDlogMFit(PrefsDlogmFit fit) noexcept {
    switch (fit) {
    case PrefsDlogmFit::Pocket3:  return color::DlogMFit::Pocket3;
    case PrefsDlogmFit::DjiRefit: return color::DlogMFit::DjiRefit;
    case PrefsDlogmFit::Avata360: return color::DlogMFit::Avata360;
    case PrefsDlogmFit::Osmo360:
    case PrefsDlogmFit::Count:
    default:                      break;
    }
    // A blob byte outside the enum lands on the project default rather than on
    // an arbitrary curve.
    return color::kDefaultDlogMFit;
}

/// [WP-LOOK] Map the prefs look onto the library's display look.  Zero - every
/// blob written before the byte existed, and every fresh one - is the DJI
/// Studio look, the library default; a byte outside the enum lands there too.
/// Only the Rec.709 output has a look; makeColorParams ignores it otherwise.
[[nodiscard]] color::Look toLook(PrefsLook look) noexcept {
    switch (look) {
    case PrefsLook::Standard: return color::Look::Standard;
    case PrefsLook::DjiStudio:
    case PrefsLook::Count:
    default:                  break;
    }
    return color::kDefaultLook;
}

/// [WP-HDRTONE] Map the prefs transfer function onto the library's HDR tone
/// style (the same values).  Zero - every blob written before the byte
/// existed, and every fresh one - is ACES 2 Bright, the library default; a
/// byte outside the enum lands there too.  Only D-Log M to PQ / HLG has a
/// style; makeColorParams ignores it otherwise.
[[nodiscard]] color::HdrTone toHdrTone(PrefsHdrTone tone) noexcept {
    switch (tone) {
    case PrefsHdrTone::Aces2Detailed: return color::HdrTone::Aces2Detailed;
    case PrefsHdrTone::Bt2408Natural: return color::HdrTone::Bt2408Natural;
    case PrefsHdrTone::Bt2408Punchy:  return color::HdrTone::Bt2408Punchy;
    case PrefsHdrTone::Bt2408Neutral: return color::HdrTone::Bt2408Neutral;
    case PrefsHdrTone::Aces2Bright:
    case PrefsHdrTone::Count:
    default:                          break;
    }
    return color::kDefaultHdrTone;
}

/// Map the prefs choice onto the calibration selector's choice.  Auto - the
/// default, and what every blob with calibration 0 written before the
/// choice existed means - follows the accessory the camera recorded; the
/// other three force their set.
[[nodiscard]] meta::CalibrationChoice toCalibrationChoice(PrefsCalibrationChoice choice) noexcept {
    switch (choice) {
    case PrefsCalibrationChoice::Native:     return meta::CalibrationChoice::Native;
    case PrefsCalibrationChoice::LensGuards: return meta::CalibrationChoice::LensGuards;
    case PrefsCalibrationChoice::Underwater: return meta::CalibrationChoice::Underwater;
    case PrefsCalibrationChoice::Auto:
    case PrefsCalibrationChoice::Count:
    default:                                 return meta::CalibrationChoice::Auto;
    }
}

/// Map the Lens Focal choice onto the rig builder's focal source.  Auto is
/// the rule every clip is stitched with by default (digital_focal_length only
/// where it matches each lens's calibration); Camera is the rule before the
/// 8K-mode measurements; Calibration ignores the recorded value.
[[nodiscard]] geom::FocalSource toFocalSource(PrefsLensFocal choice) noexcept {
    switch (choice) {
    case PrefsLensFocal::Camera:      return geom::FocalSource::DigitalFocalLengthSameStream;
    case PrefsLensFocal::Calibration: return geom::FocalSource::ScaledCalibration;
    case PrefsLensFocal::Auto:
    case PrefsLensFocal::Count:
    default:                          return geom::FocalSource::DigitalFocalLength;
    }
}

/// Map the prefs enum onto HostContext's device preference.
[[nodiscard]] RenderDevicePreference toDevicePreference(PrefsRenderDevice dev) noexcept {
    switch (dev) {
    case PrefsRenderDevice::Cpu:    return RenderDevicePreference::Cpu;
    case PrefsRenderDevice::Cuda:   return RenderDevicePreference::Cuda;
    case PrefsRenderDevice::OpenCl: return RenderDevicePreference::OpenCl;
    case PrefsRenderDevice::Auto:
    case PrefsRenderDevice::Count:
    default:                        return RenderDevicePreference::Auto;
    }
}

/// The clip's own encoding.
///
/// Delegates to the library rule (color::inputEncodingForColorMode) rather
/// than repeating it: osvtool's `--input-encoding auto` and this importer must
/// agree about what a given clip IS, or the same footage would render
/// differently through the two front ends.  The statistical fallback that
/// Pipeline.cpp also has is deliberately not used here - it needs a decoded
/// frame, and this runs before the reader exists.
[[nodiscard]] color::InputEncoding inputEncodingFor(meta::ColorMode mode) noexcept {
    return color::inputEncodingForColorMode(mode);
}

/// Switch the GPU analyses on for this process, once.
///
/// installCudaAnalyses() plugs two things into the library: GPU band shading
/// for frames that live on the device (the direct path's frames - without it
/// the seam search, gain match and parallax measurement refuse them), and the
/// CUDA port of the classical flow solver behind "Auto".  The port produces
/// the SAME field as the CPU solver bit for bit (tests/unit/
/// test_disflow_cuda.cpp compares them with ==) in ~0.8 ms instead of ~16 ms,
/// so the equirect path gets faster and not different.  A machine without a
/// usable CUDA device simply stays on the CPU, which is logged once.
void ensureGpuAnalyses() noexcept {
#if defined(OSV_HAVE_CUDA)
    try {
        static std::once_flag once;
        std::call_once(once, [] {
            const Status installed = render::installCudaAnalyses();
            if (installed.ok()) {
                PluginLog::info("analyses: GPU band shading and GPU flow installed (bit-identical to the CPU solver)");
            } else {
                PluginLog::info("analyses: staying on the CPU ({})", installed.error().message);
            }
            // [WP-FLARE] the ghost analysis's working images from frames in VRAM
            const Status flare = render::installCudaFlareSampler();
            if (!flare.ok()) {
                PluginLog::info("flare: no GPU sampler ({}); direct-path frames render without ghost removal",
                                flare.error().message);
            }
        });
    } catch (...) {
        // std::call_once only rethrows what the callable throws; this keeps
        // a noexcept function honest on a path reached from a C boundary.
    }
#endif
}

// ---------------------------------------------------------------------------
//  Reader helpers (ensureReader / releaseHeavy)
// ---------------------------------------------------------------------------

/// True when every track the reader will decode carries the configuration
/// record (hvcC for the native lenses, avcC for the LRF proxy) that the
/// container-sample feed primes libavcodec with.  Every camera-written clip
/// does; a remuxed file might not, and then libavformat demuxes it instead.
[[nodiscard]] bool containerSamplesUsable(const OsvFile* file, const meta::FormatInfo& format) noexcept {
    if (!file) {
        return false;
    }
    // The same track selection DualStreamReader::open makes.
    std::array<std::uint32_t, 2> tracks{format.videoTrackIds[0], format.videoTrackIds[1]};
    if (format.sideBySideProxy) {
        const std::uint32_t t = format.videoTrackIds[0] != 0 ? format.videoTrackIds[0] : 1u;
        tracks = {t, t};
    }
    for (const std::uint32_t id : tracks) {
        const TrackInfo* track = id != 0 ? file->track(id) : nullptr;
        if (!track || !track->isVideo() || (!track->hevc() && !track->avc())) {
            return false;
        }
    }
    return true;
}

/// The options the importer opens its reader with on one back-end.
///
/// Measured on the sample clip (osv_decoder_open_bench, D3D11VA, both lenses;
/// the machine was shared with parallel builds, so ratios matter more than
/// absolute numbers):
///
///   * useContainerSamples - frame index == sample index == djmd index by
///     construction, and no libavformat probe of all seven streams (only
///     ~3 ms here, but it grows with the metadata tracks).
///   * shareHwDevice - creating a D3D11 device cost ~120 ms per lens, more
///     than everything else in open() together.  Shared, a re-open with any
///     reader of any clip alive costs ~2 ms instead of ~160-280 ms.
///   * deferFirstFrame - open() used to decode frame 0 (~100 ms) only to
///     learn a size the hvcC parameter sets already state; the first real
///     request paid for its own decode on top.
///   * 4 threads on hardware - libavcodec's automatic count (16 on this
///     32-core machine) gives every hardware decoder one surface per thread
///     (36 per lens, ~1 GB of VRAM each at 3072x3072 P010) and a 16-deep
///     pipeline to fill before the first picture.  Twelve scattered landings,
///     both lenses, in the calmer runs:
///
///         threads   surfaces/lens   first landing   landings   sequential
///         auto (16)      36            ~162 ms        ~52-57      ~8.0 ms/pair
///         4              24            ~130-137 ms    ~45         ~8.2-8.6
///         1              20            ~112-117 ms    ~45         ~10.8-10.9
///
///     Four keeps sequential decode where it was, makes random access and
///     the first landing faster, and a parked reader holds a third less VRAM.
///     Software keeps libavcodec's own count: there, frame threads are the
///     whole speed.
[[nodiscard]] video::DecoderOptions importerReaderOptions(video::HwAccel hw, bool containerSamples) noexcept {
    // Hardware decoder threads (see the table above).
    constexpr int kHardwareDecoderThreads = 4;
    video::DecoderOptions opt;
    opt.hw = hw;
    opt.threads = (hw == video::HwAccel::None) ? 0 : kHardwareDecoderThreads;
    opt.keepOnDevice = false;
    opt.useContainerSamples = containerSamples;
    opt.shareHwDevice = true;
    opt.deferFirstFrame = true;
    return opt;
}

/// @brief True when the environment variable `name` is an explicit yes.
///
/// "1", "true", "yes" or "on" (any case), exactly as
/// importerGpuDecodeDisabledByEnvironment() reads OPENOSV_IMPORTER_NO_GPU_DECODE:
/// anything else - unset, "0", garbage - is no, so a stray value can never
/// slow a user down.
[[nodiscard]] bool importerSwitchOn(const char* name) noexcept {
    if (name == nullptr) {
        return false;
    }
#if defined(_WIN32)
    // getenv_s: the importer is /MD and shares the CRT's environment with the
    // host (and with a test that sets the variable).
    char value[8] = {};
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), name) != 0 || length == 0) {
        return false;
    }
#else
    const char* env = std::getenv(name);
    if (env == nullptr || env[0] == '\0') {
        return false;
    }
    char value[8] = {};
    std::strncpy(value, env, sizeof(value) - 1);
#endif
    const char c = value[0];
    const char d = value[1];
    return c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y' ||
           ((c == 'o' || c == 'O') && (d == 'n' || d == 'N'));
}

/// @brief The clip's file name for a log line, UTF-8; "?" when it cannot be
/// converted.  Never throws: path::string() converts through the ANSI code
/// page and throws on a name it cannot map, which must not happen on a
/// noexcept path such as the parallax worker's.
[[nodiscard]] std::string clipLogName(const std::filesystem::path& path) noexcept {
    try {
        const std::u8string name = path.filename().u8string();
        return std::string(name.begin(), name.end());
    } catch (...) {
        return "?";
    }
}

/// @brief "consistent 18% (needs 25%)" - the share of a parallax measurement
/// that passed its consistency check, for a per-bucket refusal line.
///
/// A refusal hands back an Error, not the grid that held the share, so it is
/// rebuilt from what parallaxFromBands() fills in on the side: the consistent
/// pixels are the sum of the per-cell counts (every consistent pixel lands in
/// exactly one cell), the examined ones are the band pixels both lenses cover
/// (alpha > 0.5 in both - gridFromFlow's own test).  The result is exactly
/// ParallaxWarpGrid::consistentFraction() of the refused grid.
///
/// @param cells        What gridFromFlow() saw per cell (empty when it never ran).
/// @param bands        The bands the measurement was made on.
/// @param minFraction  The threshold the measurement had to meet.
/// @return The text, or "consistent n/a" when the flow never got that far.
[[nodiscard]] std::string consistentShareText(const render::ParallaxCellStats& cells, const render::LensBands& bands,
                                              double minFraction) {
    // ---- the examined pixels: co-visible band pixels -------------------------
    const std::size_t n = static_cast<std::size_t>(bands.w) * static_cast<std::size_t>(bands.h);
    if (!cells.valid() || n == 0 || bands.alpha[0].size() < n || bands.alpha[1].size() < n) {
        return "consistent n/a";
    }
    std::uint64_t covisible = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (bands.alpha[0][i] > 0.5f && bands.alpha[1][i] > 0.5f) {
            ++covisible;
        }
    }
    // ---- the consistent ones: the cells' pixel counts -------------------------
    std::uint64_t consistent = 0;
    for (const std::uint32_t p : cells.pixels) {
        consistent += p;
    }
    if (covisible == 0) {
        return "consistent n/a (no co-visible pixel)";
    }
    const double share = static_cast<double>(consistent) / static_cast<double>(covisible);
    return std::format("consistent {:.1f}% (needs {:.0f}%)", 100.0 * share, 100.0 * minFraction);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Construction / destruction
// ---------------------------------------------------------------------------

ImporterInstance::ImporterInstance(std::filesystem::path path) : m_path(std::move(path)) {}

ImporterInstance::~ImporterInstance() {
    // releaseHeavy() closes the OS handle, tears the decoders down in the
    // right order (audio before video, reader before file mapping) and joins
    // the parallax worker - which must be gone before the members it reads
    // are destroyed.
    releaseHeavy();
}

// ---------------------------------------------------------------------------
//  Background parallax analysis
// ---------------------------------------------------------------------------
void ImporterInstance::parallaxWorkerLoop() noexcept {
    for (;;) {
        ParallaxJob job;
        {
            std::unique_lock<std::mutex> lock(m_parallaxMutex);
            m_parallaxCv.wait(lock, [this] { return m_parallaxStop || m_parallaxPending.has_value(); });
            if (m_parallaxStop) {
                return;
            }
            job = std::move(*m_parallaxPending);
            m_parallaxPending.reset();
            m_parallaxBusyBucket = job.bucket;
        }

        // The expensive half, with NO lock held and no pool: the render pool
        // is shared with the frame renders this worker exists to stay out of
        // the way of, and ThreadPool serialises whole jobs, so borrowing it
        // would make a frame render queue behind a flow solve.
        std::shared_ptr<const render::ParallaxWarpGrid> result;
        std::string refusal;
        double ms = 0.0;
        try {
            const auto t0 = std::chrono::steady_clock::now();
            // The per-cell counts ride along so a refusal can say how much of
            // the flow was consistent (consistentShareText); output only.
            render::ParallaxCellStats cells;
            auto grid = render::parallaxFromBands(job.bands, job.params, nullptr, job.bandMs, &cells);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (grid.ok()) {
                result = std::make_shared<const render::ParallaxWarpGrid>(std::move(grid).value());
            } else {
                refusal = grid.error().message;
                // The consistent share walks every band pixel, and only the
                // Debug line below prints it: below Debug it is never built.
                if (PluginLog::enabled(PluginLog::Level::Debug)) {
                    refusal += ", " + consistentShareText(cells, job.bands, job.params.minConsistentFraction);
                }
            }
        } catch (const std::exception& e) {
            // Allocation failure is the realistic case.  Record it as a
            // refusal so the bucket is not retried in a tight loop.
            refusal = std::string("exception: ") + e.what();
        } catch (...) {
            refusal = "unknown exception";
        }

        bool stored = false;
        {
            std::lock_guard<std::mutex> lock(m_parallaxMutex);
            m_parallaxBusyBucket.reset();
            // A result measured under settings that have since changed (or a
            // clip that has since been quieted) describes nothing current.
            if (job.generation == m_parallaxGeneration && !m_parallaxStop) {
                // [WP-TEMPORAL] Bands cut from a frame other than the bucket's
                // anchor make a stand-in: only Interactive requests read it.
                if (job.standIn) {
                    m_standInGrids[job.bucket] = result;  // nullptr records a refusal
                    trimAnalysisCache(m_standInGrids, kMaxStandInCache, job.bucket);
                } else {
                    m_parallaxGrids[job.bucket] = result;  // nullptr records a refusal
                    trimAnalysisCache(m_parallaxGrids, kMaxParallaxCache, job.bucket);
                }
                stored = true;
            }
        }
        // The clip on every line: two clips' buckets interleave in one log.
        // (m_path is set at construction and never changes, so this thread
        // may read it without m_mutex.)
        const char* lane = job.standIn ? " (stand-in)" : "";
        if (result) {
            PluginLog::debug("parallax bucket {}{} of '{}' measured in the background in {:.0f} ms (flow {:.0f}), "
                             "consistent {:.1f}%{}",
                             job.bucket, lane, clipLogName(m_path), ms, result->flowMs,
                             100.0 * result->consistentFraction(), stored ? "" : " - discarded, settings changed");
        } else {
            PluginLog::debug("parallax bucket {}{} of '{}' refused in the background after {:.0f} ms ({})", job.bucket,
                             lane, clipLogName(m_path), ms, refusal);
        }
    }
}

void ImporterInstance::stopParallaxWorker() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_parallaxMutex);
        m_parallaxStop = true;
        m_parallaxPending.reset();
    }
    m_parallaxCv.notify_all();
    // Joining is safe with m_mutex held because the worker never takes it.
    // It may wait for one in-flight measurement to finish: the flow solver
    // has no cancellation point, and abandoning the thread instead would
    // leave it touching this object after the destructor ran.
    if (m_parallaxWorker.joinable()) {
        try {
            m_parallaxWorker.join();
        } catch (...) {
            // join() only throws for a thread that is not joinable or is the
            // calling thread; neither can happen here, and a noexcept
            // function must not let it escape if the library disagrees.
        }
    }
    // Leave the instance ready to start a fresh worker after a quiet.
    std::lock_guard<std::mutex> lock(m_parallaxMutex);
    m_parallaxStop = false;
}

void ImporterInstance::resetParallaxLocked() noexcept {
    // [WP-TEMPORAL] The stand-in lane goes with the anchored caches it
    // stands in for (all callers hold m_mutex, which guards most of it).
    m_standInSeams.clear();
    m_standInTables.clear();
    m_standInGains.clear();
    m_photoStandIns.clear();
    m_shadingStandIns.clear();
    std::lock_guard<std::mutex> lock(m_parallaxMutex);
    m_parallaxGrids.clear();
    m_standInGrids.clear();
    m_blendSeams.clear();  // [WP-SEAM] carved through the corrections just dropped
    m_parallaxPending.reset();
    ++m_parallaxGeneration;
    m_flare.reset();  // [WP-FLARE] its own lock; nothing here is held by it
}

// ---------------------------------------------------------------------------
//  Opening
// ---------------------------------------------------------------------------

Status ImporterInstance::open() {
    std::lock_guard<std::mutex> guard(m_mutex);

    // The OS handle is what Premiere stores in imFileAccessRec8::fileref and
    // what keeps a second application from deleting the clip under us.  Share
    // read so Media Encoder can open the same file at the same time.
#if defined(_WIN32)
    if (m_fileHandle == INVALID_HANDLE_VALUE) {
        const std::wstring wide = m_path.wstring();
        m_fileHandle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_fileHandle == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            return Error{ErrorCode::Io, "CreateFileW failed with " + std::to_string(err)};
        }
    }
#else
    // POSIX has no share modes (nothing stops a delete anyway); the open
    // descriptor is what the host is handed and what proves the file is
    // readable before anything is parsed.
    if (m_fileHandle == invalidClipFileHandle()) {
        int fd = -1;
        do {
            fd = ::open(m_path.c_str(), O_RDONLY | O_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            const int err = errno;
            return Error{ErrorCode::Io, std::string("open failed: ") + std::strerror(err)};
        }
        m_fileHandle = reinterpret_cast<ClipFileHandle>(static_cast<std::intptr_t>(fd));
    }
#endif

    // The container work only happens once; after a quiet/unquiet cycle the
    // parsed state is still there and only the decoders have to come back.
    Status st = parseOnce();
    if (!st.ok()) {
        // Close the handle here: the dispatcher returns imBadFile and a
        // lower-priority importer must be able to open the file.
        closeClipFile(m_fileHandle);
        return st;
    }
    return okStatus();
}

Status ImporterInstance::parseOnce() {
    if (m_parsed) {
        return okStatus();
    }

    // ---- container ---------------------------------------------------------
    auto opened = OsvFile::open(m_path);
    if (!opened.ok()) {
        return opened.error();
    }
    m_file = std::make_unique<OsvFile>(std::move(opened).value());

    // ---- metadata track ----------------------------------------------------
    auto track = meta::MetadataTrack::load(*m_file);
    if (!track.ok()) {
        return track.error();
    }
    m_track = std::move(track).value();

    // ---- format ------------------------------------------------------------
    auto format = meta::FormatDetector::detect(*m_file, &m_track);
    if (!format.ok()) {
        return format.error();
    }
    m_format = std::move(format).value();
    for (const std::string& n : m_format.notes) {
        m_notes.push_back("format: " + n);
    }

    // Without calibration there is nothing to stitch: this is not a clip this
    // importer can claim, so the dispatcher must hand it back with imBadFile.
    if (!m_track.hasCalibration()) {
        return Error{ErrorCode::Malformed, "clip carries no lens calibration (not a dual-fisheye OSV)"};
    }

    // ---- timing ------------------------------------------------------------
    // The frame rate must be the EXACT container rational, not a rounded
    // double: Premiere's timebase is 254016000000 ticks/second and 59.94 fps
    // has to land on 4237833600 ticks exactly or every frame after the first
    // drifts.  The video track's media timescale over its per-sample duration
    // is that rational.
    const TrackInfo* videoTrack = m_file->track(m_format.videoTrackIds[0]);
    if (!videoTrack) {
        return Error{ErrorCode::Malformed, "video track missing from the container"};
    }
    m_frameCount = videoTrack->samples.count();
    m_rateNum = videoTrack->timescale;
    // [VFR] The camera records at a constant rate, but drops frames when it
    // cannot keep up and writes each gap into the table as one longer
    // sample.  The clip's own timeline (video::ClipTimeline) is the sample
    // list for a constant-rate clip and a held-frame conform at the nominal
    // rate otherwise; its nominal period - the most common sample duration,
    // every sample's on a constant-rate clip, never just sample 0's - is the
    // denominator.  Fall back to the whole duration over the sample count so
    // an unusual sample table still yields a sane rational.
    m_timeline = video::clipTimelineFor(*videoTrack, &m_track);
    std::uint64_t delta = 0;
    if (m_frameCount > 0 && videoTrack->timescale > 0) {
        delta = m_timeline.nominalTicks;
        if (delta == 0 && videoTrack->duration > 0) {
            delta = videoTrack->duration / m_frameCount;
        }
    }
    m_rateDen = static_cast<std::uint32_t>(delta);
    if (!m_timeline.identity()) {
        // What the host will be told, and why: a user comparing the clip's
        // length with another tool finds the answer in the log.
        PluginLog::info("timing: '{}' dropped frames while recording: {} samples presented as {} frames at {:.3f} "
                        "fps ({:.3f} s), the previous picture held over {} gap frames, timed by the {} clock{}{}",
                        m_path.filename().string(), m_frameCount, m_timeline.frameCount(), m_timeline.fps(),
                        m_timeline.durationSeconds(), m_timeline.heldFrames,
                        video::timelineClockName(m_timeline.clock), m_timeline.note.empty() ? "" : " - ",
                        m_timeline.note);
    } else if (!m_timeline.note.empty() && m_frameCount > 1) {
        // A table that varies but could not be placed: presented sample for
        // sample, as every version before the timeline did.
        PluginLog::info("timing: '{}' is presented sample for sample ({})", m_path.filename().string(),
                        m_timeline.note);
    }
    if (m_rateNum == 0 || m_rateDen == 0) {
        // Last resort: the detected fps as a 1000-times rational.  Better a
        // slightly wrong timebase than a division by zero.
        const double fpsGuess = m_format.fps > 0.0 ? m_format.fps : 30.0;
        m_rateNum = static_cast<std::uint32_t>(std::lround(fpsGuess * 1000.0));
        m_rateDen = 1000u;
        m_notes.push_back("timing: container rational unavailable; using the detected frame rate");
    }
    if (m_frameCount == 0) {
        return Error{ErrorCode::Malformed, "video track has no samples"};
    }

    if (m_file->movie().hasMovieHeader) {
        m_creationTime1904 = m_file->movie().header.creationTime;
    }

    // ---- [PROXY] an .LRF beside its .OSV is presented on the .OSV's timeline
    if (m_format.sideBySideProxy) {
        adoptProxyTimelineLocked();
    }

    // ---- audio track (described now, decoded lazily) -----------------------
    for (const TrackInfo* t : m_file->tracksOfKind(TrackKind::Audio)) {
        if (!t || !t->audio) {
            continue;
        }
        m_audioTrackId = t->trackId;
        m_audioChannels = static_cast<std::int32_t>(t->audio->channelCount);
        m_audioSampleRate = t->audio->sampleRate;
        // Duration in sample frames: the media duration is already in the
        // audio timescale, which for AAC is the sample rate.
        if (t->timescale > 0 && m_audioSampleRate > 0.0) {
            const double seconds = static_cast<double>(t->duration) / static_cast<double>(t->timescale);
            m_audioDuration = static_cast<std::int64_t>(std::llround(seconds * m_audioSampleRate));
        }
        break;  // One audio track per clip; ignore any extras.
    }

    // ---- calibration + rig -------------------------------------------------
    Status rigStatus = rebuildRig();
    if (!rigStatus.ok()) {
        return rigStatus;
    }

    m_parsed = true;

    // ---- an SDR recording starts SDR ------------------------------------------
    // The starting settings (the user's saved defaults, else the built-in
    // ones) say PQ, which is right for D-Log M.  A Normal-mode recording is an
    // SDR picture already: made PQ it would only be re-encoded as HDR, and on
    // a Rec.709 timeline Premiere would then tone map it back down.  So while
    // the instance still runs on those starting settings, an SDR clip gets
    // Rec.709 output instead.  A clip with stored settings is handed them at
    // imGetInfo8, next, and they replace this exactly as they replace the
    // seed; a NEW clip keeps it, and its Source Settings show Rec.709.  PQ
    // and HLG stay one click away for anybody who wants SDR in an HDR master.
    if (!m_settingsPublishedFromHost && !m_colorBuilt &&
        inputEncodingFor(m_format.colorMode) == color::InputEncoding::Rec709Normal &&
        (m_prefs.color() == PrefsColorOutput::PQ || m_prefs.color() == PrefsColorOutput::HLG)) {
        PluginLog::info("colour: '{}' was recorded in SDR (Normal colour mode): unless it has stored settings, it "
                        "starts with Rec.709 output instead of {}, so its picture is not re-encoded as HDR",
                        m_path.filename().string(), color::outputTransferName(toOutputTransfer(m_prefs.color())));
        m_prefs.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    }

    // ---- one line per clip, so colour decisions are answerable from a log --
    //
    // This is the line to look for when footage previews wrong.  It states the
    // three facts that decide the whole colour path and cannot be recovered
    // afterwards: what the camera said the clip is, whether that came from
    // metadata or from the luma-histogram fallback, and which input/output
    // encodings the pipeline therefore chose.
    //
    // The input encoding follows the CLIP's metadata (inputEncodingFor), never
    // the output preference.  That separation is the whole point: an HLG or
    // Normal clip must not be decoded with the D-Log M curve just because the
    // output is set to PQ, or the source would be double-converted - a log
    // de-log applied to an already display-referred signal, which crushes the
    // shadows and blows the highlights.  A D-Log M clip with the default PQ
    // output is the "auto PQ for log footage" case the default already gives.
    {
        const color::InputEncoding in = inputEncodingFor(m_format.colorMode);
#if defined(OSV_CLIP_ENGINE_WITHOUT_PREMIERE)
        const char* declared = "none (not Premiere)";
#else
        const char* declared = colorSpaceTokenFor(m_prefs);
#endif
        PluginLog::info("colour: '{}': source {} ({}) -> input encoding {}, output {} ({}), HDR tone {}, "
                        "Rec.709 look {}, HDR peak {:.0f} nits",
                        m_path.filename().string(), meta::colorModeName(m_format.colorMode),
                        m_format.colorModeFromMetadata ? "from metadata" : "inferred from luma statistics",
                        color::inputEncodingName(in), color::outputTransferName(toOutputTransfer(m_prefs.color())),
                        declared, color::hdrToneName(toHdrTone(m_prefs.hdrToneChoice())),  // [WP-HDRTONE]
                        color::lookName(toLook(m_prefs.lookChoice())), static_cast<double>(m_prefs.hdrPeakNits()));
    }
    return okStatus();
}

Status ImporterInstance::rebuildRig() {
    // The lens-guard / underwater slots change the intrinsics AND the
    // extrinsics, so the whole rig is rebuilt; the selector is cheap (it only
    // walks the protobuf records already parsed into StreamMeta).
    //
    // Everything is built into locals and committed together at the end, so
    // a failure half way leaves the previous calibration, rig and notes
    // intact and consistent with each other.
    const PrefsCalibrationChoice choice = m_prefs.calibrationChoice();
    std::vector<std::string> selWarnings;
    auto selected = meta::CalibrationSelector::choose(m_track.stream(), toCalibrationChoice(choice), {}, &selWarnings);
    if (!selected.ok()) {
        return selected.error();
    }
    const meta::CalibrationSelection selection = std::move(selected).value();
    const meta::CalibrationSet& calibration = selection.set;

    // Sensor -> stream scaling: the calibration is expressed in 3840x3840
    // sensor pixels, the decoded frames are 3000x3000 (6K mode).  This is the
    // same derivation Pipeline.cpp performs, with the verified defaults.
    //
    // lensW()/lensH() rather than streamW/streamH, because the LRF proxy is a
    // SINGLE 2048x1024 side-by-side track whose two 1024x1024 halves the
    // reader splits apart before we ever see a frame.  The rig describes one
    // lens, so it must be derived from the half, not from the track.
    std::vector<std::string> scaleNotes;
    const double calFxMean = 0.5 * (calibration.slave.fx + calibration.master.fx);
    auto scaling = geom::StreamScaling::derive(
        static_cast<int>(m_format.lensW()), static_cast<int>(m_format.lensH()), static_cast<int>(m_format.sensorW),
        static_cast<int>(m_format.sensorH), m_format.digitalFocalLength, calFxMean, std::nullopt, &scaleNotes);
    if (!scaling.ok()) {
        return scaling.error();
    }

    // The verified conventions (docs, memory: wxyz order, body->lens sense,
    // focal from digital_focal_length, 195.18 deg usable FOV).
    //
    // Note what the focal source means for a calibration switch: with
    // DigitalFocalLength both lenses take the clip's one digital focal length,
    // so a set contributes its principal point, radial terms, extrinsic
    // rotation and occlusion arc - but not its own fx/fy, unless those
    // disagree with the digital focal length by more than 0.5 % (see
    // LensRig.cpp).  On the 6K sample that is the verified-best choice: the
    // per-lens calibration focal measured a lower overlap NCC (0.807 vs
    // 0.824).  8K-mode clips record a value 1.3-2.7 % off their lenses and
    // take the calibrated focal instead.
    //
    // Source Settings "Lens Focal" overrides the rule for a clip whose mode
    // it gets wrong: Camera trusts the recorded value whenever it describes
    // this stream size (the rule before the 8K measurements), Calibration
    // always takes each lens's own.  Auto is the rule above, bit for bit.
    const PrefsLensFocal focalChoice = m_prefs.lensFocalChoice();
    const geom::ExtrinsicConvention conv;  // defaults are the verified values
    auto rig = geom::LensRig::build(calibration, scaling.value(), toFocalSource(focalChoice),
                                    m_format.digitalFocalLength, conv, 195.18);
    if (!rig.ok()) {
        return rig.error();
    }
    geom::LensRig builtRig = std::move(rig).value();

    // Blend defaults match osvtool's (4 degree feather, occlusion polygon on).
    //
    // The mask is ON here whatever Hide Mount says: the lens-protector check
    // below scores the overlap through this blend and caches its verdict per
    // FILE, so it must not depend on a per-clip display choice.  The stitch's
    // own mask is set from Hide Mount just before the commit.
    geom::BlendParams blend = m_blend;
    blend.lensFovDeg = 195.18;
    blend.featherDeg = 4.0;
    blend.useOcclusionMask = true;

    // ---- lens protectors: the field-angle correction ---------------------------
    //
    // When the choice resolved to lens guards and the clip has no dedicated
    // lens-guard calibration (every clip seen so far), the protector's
    // field-angle curve is folded into both lens models - see
    // geom/LensProtector.h.  The direction is DJI's (forward), checked once
    // per clip on frame 0 by the guard (ProtectorGuard.h), which switches the
    // correction off if the footage plainly was not shot through a
    // protector.  Native never gets here, and neither does Auto on a clip
    // recorded without protectors - those rigs are exactly what they were.
    std::string reason = selection.reason;
    std::string protectorNote;
    if (selection.protectorCorrection) {
        const ProtectorGuardResult guard = resolveProtectorGuard(m_path, m_format, builtRig, blend);
        auto fold = geom::applyLensProtector(builtRig, guard.direction, blend.lensFovDeg);
        if (fold.ok()) {
            blend.lensFovDeg = fold.value().lensFovDeg;
            protectorNote = std::format("lens-protector correction {}: usable FOV {:.2f} deg, refit residual "
                                        "{:.3f} px; check: {}",
                                        geom::protectorDirectionName(guard.direction), blend.lensFovDeg,
                                        fold.value().maxResidualPx, guard.summary);
            if (guard.direction == geom::ProtectorDirection::None) {
                // The selector's sentence promised the correction; say that
                // the pixels overruled it, so the line is true as a whole.
                reason += " - switched off: frame 0 does not look shot through a protector";
            }
        } else {
            // The fold only fails on a lens it cannot refit; the bare lens is
            // still a correct stitch of bare-lens geometry, so render that
            // rather than nothing, and say so.
            protectorNote = std::format("lens-protector correction could not be applied ({}); stitching without it",
                                        fold.error().message);
            reason += " - correction could not be applied, stitching without it";
        }
    }

    // ---- [WP-STEADY] the per-clip lens rotation ------------------------------------
    //
    // AFTER the protector fold: the fold reshapes each lens's field-angle
    // curve, the rotation is a rigid turn of the lens pair measured THROUGH
    // that curve, so it is fitted to (and cached for) the rig the fold made.
    // Here only a remembered verdict is used (memory, then the disk cache -
    // microseconds); an unknown clip keeps the calibration until the steady
    // stage has measured it on its first non-draft frame (prepareSteadyLocked).
    const PrefsLensAlign alignChoice = m_prefs.lensAlignChoice();
    geom::LensRig baseRig = builtRig;
    LensAlignState alignState = LensAlignState::Off;
    std::string alignNote;
    if (alignChoice == PrefsLensAlign::Auto) {
        const std::optional<LensAlignVerdict> known = cachedLensAlign(m_path, baseRig);
        if (!known) {
            alignState = LensAlignState::Pending;
            alignNote = "lens alignment: to be fitted on the clip's first frames";
        } else if (known->accepted && render::applyLensRotation(builtRig, known->wRad).ok()) {
            alignState = LensAlignState::Applied;
            alignNote = "lens alignment: " + known->summary;
        } else {
            alignState = LensAlignState::Refused;
            alignNote = "lens alignment: keeping the calibration - " + known->summary;
        }
    }

    // ---- Hide Mount: the stitch's occlusion mask ------------------------------------
    //
    // On (the default, and every older project's zero byte) applies the
    // calibration's occlusion polygons exactly as the importer always did.
    // Off drops them from the ANALYSIS blend - so the parallax grid, the
    // seam table, the carved seam and every photometric measurement see the
    // full lens overlap - and from the render blend derived from it.  On a
    // car, helmet or suction mount that is what lets the seam align the near
    // field the polygons would otherwise cut off; the mount itself can show.
    const PrefsHideMount hideMount = m_prefs.hideMountChoice();
    blend.useOcclusionMask = hideMount != PrefsHideMount::Off;

    // ---- Hide Mount Auto: the per-clip mount mask --------------------------------------
    //
    // Auto keeps the mask ON and rebuilds the polygons themselves: released
    // where both lenses see the same scene, kept around the mount
    // (render/MountMask.h).  As with the rotation, only a remembered verdict
    // is used here (memory, then hide-mount.tsv - microseconds); an unknown
    // clip stitches with the full polygons until the steady stage has
    // measured it on its sample frames (prepareSteadyLocked).  The verdict
    // is defined against the CALIBRATION rig (baseRig): its band columns and
    // its polygons.  builtRig may carry a cached rotation, which would move
    // the columns (not the polygons, they live in fisheye pixels), so the
    // polygons are rebuilt against baseRig and copied in.
    MountState mountState = MountState::Off;
    std::shared_ptr<const render::MountMask> mountMask;
    std::string mountNote;
    if (hideMount == PrefsHideMount::Auto) {
        mountMask = cachedMountMask(m_path, baseRig, blend, render::MountMaskParams{});
        if (!mountMask) {
            mountState = MountState::Pending;
            mountNote = "hide mount: Auto - to be measured on the clip's sample frames";
        } else {
            mountState = MountState::Settled;
            auto applied =
                render::applyMountMaskFrom(baseRig, builtRig, *mountMask, blend, render::MountMaskParams{});
            if (applied.ok()) {
                mountNote = "hide mount: Auto - " + render::describeMountMask(*mountMask) + " (cached)";
            } else {
                // The calibration's polygons are still a correct (if
                // overlap-starved) stitch: keep them and say why.
                mountNote = "hide mount: Auto - keeping the full mask; the cached verdict could not be applied (" +
                            applied.error().message + ")";
                mountMask.reset();
            }
        }
    }

    // ---- commit ----------------------------------------------------------------
    m_calibration = calibration;
    m_baseRig = std::move(baseRig);  // [WP-STEADY]
    m_rig = std::move(builtRig);
    m_blend = blend;
    m_lensAlignState = alignState;   // [WP-STEADY]
    m_rigLensAlign = alignChoice;    // [WP-STEADY]
    m_rigLensFocal = focalChoice;    // Lens Focal
    m_rigHideMount = hideMount;      // Hide Mount
    m_mountState = mountState;       // Hide Mount Auto
    m_mountMask = std::move(mountMask);

    // The notes feed the Properties panel.  A rebuild REPLACES the previous
    // calibration / scaling / rig notes instead of piling another copy on
    // top of them each time the user flips the setting.
    std::erase_if(m_notes, [](const std::string& n) {
        return n.starts_with("calibration: ") || n.starts_with("scaling: ") || n.starts_with("rig: ") ||
               n.starts_with("lens alignment: ") ||  // [WP-STEADY]
               n.starts_with("hide mount: ");        // Hide Mount Auto
    });
    m_notes.push_back("calibration: " + reason);
    if (!alignNote.empty()) {
        m_notes.push_back(alignNote);  // [WP-STEADY]
    }
    if (!mountNote.empty()) {
        m_notes.push_back(mountNote);  // Hide Mount Auto
    }
    if (!protectorNote.empty()) {
        m_notes.push_back("calibration: " + protectorNote);
    }
    for (const std::string& w : selWarnings) {
        m_notes.push_back("calibration: " + w);
    }
    for (const std::string& n : scaleNotes) {
        m_notes.push_back("scaling: " + n);
    }
    for (const std::string& n : m_rig.notes) {
        m_notes.push_back("rig: " + n);
    }

    // ---- one line per (re)build: which set stitches this clip, and why ------
    //
    // The line to look for when "switching Calibration changes nothing": it
    // names the choice, what the camera recorded, the set actually used and,
    // when that set is native after all (the clip holds no lens-guard /
    // underwater calibration, or holds a copy of native), says so in words.
    // Info level, because it runs once on open and once per calibration
    // change - never per frame.
    PluginLog::info("calibration: '{}': {}/{} - {}", m_path.filename().string(), m_calibration.sourceSlave,
                    m_calibration.sourceMaster, reason);
    // How calibration pixels become stream pixels for this recording mode:
    // the line that tells a misaligned seam of an unverified mode apart from
    // parallax.
    for (const std::string& n : scaleNotes) {
        PluginLog::info("scaling: '{}': {}", m_path.filename().string(), n);
    }
    if (!protectorNote.empty()) {
        // The three scores and the pick, so a protector clip's stitch can be
        // explained from the log alone.
        PluginLog::info("calibration: '{}': {}", m_path.filename().string(), protectorNote);
    }
    if (!alignNote.empty()) {
        // [WP-STEADY] The rotation in force for this rig, from the cache (a
        // measurement logs its own line when it lands).
        PluginLog::info("{} ('{}')", alignNote, m_path.filename().string());
    }
    // Lens Focal: which rule set each lens's focal (the rig notes carry the
    // numbers), so a user's override is visible in the log.
    PluginLog::info("lens focal: '{}': {} ({})", m_path.filename().string(),
                    focalChoice == PrefsLensFocal::Camera        ? "Camera"
                    : focalChoice == PrefsLensFocal::Calibration ? "Calibration"
                                                                 : "Auto",
                    geom::focalSourceName(m_rig.focalSource));
    // Hide Mount: only an explicit Off is worth a line - it is the one
    // setting that can put the camera's mount into the picture, so a report
    // of "the mount shows" is answerable from the log.
    if (hideMount == PrefsHideMount::Off) {
        PluginLog::info("hide mount: '{}': Off - the calibration's occlusion polygons are not applied; the seam "
                        "uses the full lens overlap and the mount can show",
                        m_path.filename().string());
    } else if (!mountNote.empty()) {
        // Auto: the verdict in force (cached), or that it is still to come
        // (the measurement logs its own line when it lands).
        PluginLog::info("{} ('{}')", mountNote, m_path.filename().string());
    }

    // The CHOICE, not the calibration byte: Auto and a forced Native share
    // calibration 0 but can stitch with different sets (on a clip recorded
    // with lens protectors), so the rebuild trigger has to tell them apart.
    m_rigCalibration = choice;
    m_rigBuilt = true;
    return okStatus();
}

Status ImporterInstance::ensureReader() {
    if (m_reader && m_reader->isOpen()) {
        return okStatus();
    }

    // ---- which decoder --------------------------------------------------------
    // RANDOM ACCESS decides this, not playback.  Every time the user drops
    // the playhead somewhere new, Premiere asks for that one frame (intent
    // Stopped) and waits for it.  Reaching it means decoding forward from
    // the previous sync sample - up to a full GOP, 60 frames on camera
    // files - for BOTH 3000x3000 10-bit lenses.  Measured on the sample clip
    // (osv_importer_bench --part P, twelve scattered landings):
    //
    //     software (libavcodec, frame threads)   median  949 ms, worst 1925 ms
    //     D3D11VA, frames copied back to host    median   52 ms, worst  109 ms
    //     NVDEC (CUDA), copied back to host      median   59 ms, worst   92 ms
    //
    // Software is slow here twice over: it pays the GOP, and after every
    // seek its frame threads must refill a pipeline of a dozen or more
    // in-flight frames before the first one comes out - which is why even a
    // frame three past a keyframe cost ~680 ms.
    //
    // D3D11VA goes first: it is as fast as NVDEC, it works on every GPU
    // vendor Premiere supports, and a D3D11 device is far lighter than the
    // private CUDA context FFmpeg would create per decoder.  The frames are
    // copied back to host memory (keepOnDevice stays false) because the
    // renderer comes from a shared pool whose device is not known here.
    // Software is the last resort, and the only choice once hardware has
    // failed on this clip (m_hwDecodeFailed, see readPair()).
    //
    // macOS: VideoToolbox takes D3D11VA's place - Apple's hardware HEVC
    // decoder, on every Apple Silicon Mac - with the same host copy.
    //
    // OPENOSV_IMPORTER_LRF_SOFTWARE=1 (a diagnostic switch, off by default)
    // keeps the side-by-side PROXY on software from the start: it separates a
    // fault in the hardware decode from one later in the chain within a
    // single Premiere session.  Measured on a 2048x1024 proxy it costs
    // 1.3 ms a frame with four threads (3.5 ms single-threaded), so a
    // worst-case landing at the end of a 20-frame GOP is ~25-70 ms instead of
    // ~16 ms.  The native streams are never affected: software is ~1 s per
    // landing there.  The pool key includes the back-end, so a parked
    // hardware reader of the clip is never taken back while the switch is on.
    const bool proxySoftware = m_format.sideBySideProxy && importerSwitchOn("OPENOSV_IMPORTER_LRF_SOFTWARE");
    if (proxySoftware) {
        const std::string clip = clipLogName(m_path);
        PluginLog::oncef("lrf-software/" + clip, PluginLog::Level::Info,
                         "video: '{}' decodes in software (OPENOSV_IMPORTER_LRF_SOFTWARE is set)", clip);
    }
    std::vector<video::HwAccel> order;
    if (!m_hwDecodeFailed && !proxySoftware) {
#if defined(__APPLE__)
        order.push_back(video::HwAccel::VideoToolbox);
#else
        order.push_back(video::HwAccel::D3D11VA);
        order.push_back(video::HwAccel::Cuda);
#endif
    }
    order.push_back(video::HwAccel::None);

    // ---- how it is opened -------------------------------------------------
    // The container-sample feed when the tracks allow it (every camera file
    // does), libavformat otherwise; the rest of the choices and the numbers
    // behind them are in importerReaderOptions().
    const bool samples = containerSamplesUsable(m_file.get(), m_format);

    // ---- where it comes from ------------------------------------------------
    // Premiere quiets a clip it is not reading and wakes it seconds later, and
    // opens a second instance of the clip on every Source Settings change.
    // releaseHeavy() parks the reader in the process-wide pool instead of
    // destroying it, so each back-end first asks the pool for a warm reader of
    // this exact file version and options: no device, no surfaces, no codec
    // set-up, and the decode position it had is kept.  Only a miss opens one.
    // The pool is asked per back-end IN preference order, interleaved with
    // the opens, so a parked software reader never displaces the hardware
    // reader a fresh open would have produced.
    video::ReaderPool& pool = video::ReaderPool::instance();
    Status lastError = okStatus();
    for (const video::HwAccel hw : order) {
        const video::DecoderOptions opt = importerReaderOptions(hw, samples);

        // ---- 1. a warm reader ------------------------------------------------
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<video::DualStreamReader> warm = pool.take(m_path, m_format, opt);
        if (warm && warm->isOpen()) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            m_reader = std::move(warm);
            const video::HevcStreamDecoder* d = m_reader->decoder(0);
            const video::HwAccel active = d ? d->activeHw() : video::HwAccel::None;
            PluginLog::info("video: '{}' decoding with {} (warm reader from the pool in {:.1f} ms)",
                            m_path.filename().string(), video::hwAccelName(active), ms);
            return okStatus();
        }

        // ---- 2. a new reader -------------------------------------------------
        const auto t1 = std::chrono::steady_clock::now();
        auto reader = video::DualStreamReader::open(m_path, m_format, opt);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        if (!reader.ok()) {
            // Expected on a machine without that back-end; the next one is
            // tried, so this is informational rather than a warning.
            PluginLog::info("video: {} decoding unavailable for '{}' ({}); trying the next decoder",
                            video::hwAccelName(hw), m_path.filename().string(), reader.error().message);
            lastError = reader.error();
            continue;
        }
        m_reader = std::make_unique<video::DualStreamReader>(std::move(reader).value());

        // The decoder may still drop to software on its first decode (its
        // get_format callback does when the GPU cannot decode this profile);
        // activeHw() reports that from then on.  Here it names what opened.
        const video::HevcStreamDecoder* d = m_reader->decoder(0);
        const video::HwAccel active = d ? d->activeHw() : video::HwAccel::None;
        const video::DecoderOpenTimings t = d ? d->openTimings() : video::DecoderOpenTimings{};
        PluginLog::info("video: '{}' decoding with {} (opened in {:.0f} ms: device {:.0f} ms{}, codec {:.0f} ms, "
                        "{}, first frame {})",
                        m_path.filename().string(), video::hwAccelName(active), ms, t.hwDeviceMs,
                        t.hwDeviceReused ? " shared" : "", t.codecOpenMs,
                        opt.useContainerSamples ? "container samples" : "libavformat",
                        t.firstFrameDeferred ? "deferred" : "probed");
        return okStatus();
    }
    // Software is always in the list, so reaching here means even it failed
    // and lastError holds its reason; the fallback message covers an empty
    // list, which the code above cannot build but a later edit might.
    if (!lastError.ok()) {
        return lastError;
    }
    return Error{ErrorCode::Decoder, "no video decoder could be opened"};
}

Result<video::FramePair> ImporterInstance::readPair(std::uint32_t index) {
    // The caller holds m_mutex (renderFrame's contract).
    if (!m_reader || !m_reader->isOpen()) {
        return Error{ErrorCode::InvalidArgument, "readPair without an open reader"};
    }
    auto pair = m_reader->read(index);
    if (pair.ok()) {
        return pair;
    }

    // A software failure is a real decode error (a damaged file); retrying
    // it would only fail again.  A HARDWARE failure may be the driver, a
    // lost device or a surface the GPU cannot handle - software can still
    // decode the frame, so fall back for the rest of this clip's life.
    const video::HevcStreamDecoder* d = m_reader->decoder(0);
    const video::HwAccel active = d ? d->activeHw() : video::HwAccel::None;
    if (active == video::HwAccel::None) {
        return pair;
    }
    // A Timing error is the FILE's (a picture that is not the sample asked
    // for, lenses that disagree about a moment): software decodes the same
    // stream to the same failure, so it says nothing against the hardware.
    // The frame fails; the clip keeps its hardware decoder.
    if (pair.error().code == ErrorCode::Timing) {
        PluginLog::warn("video: frame {} of '{}' cannot be decoded ({}); the stream's timing is at fault, so {} "
                        "decoding stays on",
                        index, m_path.filename().string(), pair.error().message, video::hwAccelName(active));
        return pair;
    }
    PluginLog::warn("video: {} decoding failed on frame {} of '{}' ({}); switching this clip to software decoding",
                    video::hwAccelName(active), index, m_path.filename().string(), pair.error().message);
    m_hwDecodeFailed = true;
    // Destroyed, NOT parked: a reader whose hardware just failed must never
    // be handed to the next instance of this clip as a warm one.
    m_reader.reset();
    const Status reopened = ensureReader();
    if (!reopened.ok()) {
        return reopened.error();
    }
    return m_reader->read(index);
}

void ImporterInstance::releaseHeavy() noexcept {
    std::lock_guard<std::mutex> guard(m_mutex);

    // The parallax worker first: it holds bands and may be mid-measurement,
    // and a quiet must not leave a thread running against a clip that is
    // about to lose its decoders.  (Joining with m_mutex held is safe - the
    // worker never takes m_mutex; see the LOCK ORDER note in the header.)
    stopParallaxWorker();
    m_flare.stop();   // [WP-FLARE] the same rule: its worker never takes m_mutex
    m_steady.stop();  // [WP-STEADY] likewise; what it measured stays in the process-wide caches
    // Scene Light: the sky measurement holds a decoder of its own on the clip
    // file; its worker never takes m_mutex either.  A verdict already taken
    // stays; an unfinished one is asked for again by the next non-draft frame
    // (the process-wide answer cache usually has it by then).
    m_sceneStage.stop();

    // Order matters: the audio decoder owns its own AVFormatContext and OS
    // handle, the reader owns two decoders; both must go before the mapping
    // they may reference.
    m_audio.reset();
    m_audioProbed = false;
    // The device path's last host-decoded pair goes with the reader it came
    // from: a quiet is when its memory should go back.
    m_deviceHostPair = video::FramePair{};
    m_deviceHostPairIndex = kNoHostPair;
    // The reader is parked rather than destroyed: the next unquiet of this
    // clip, or the next instance Premiere opens for it (every Source Settings
    // change does), takes it back warm from the process-wide pool instead of
    // paying for a new one.  The pool owns it from here - it decodes from its
    // own shared mapping of the file, not from this instance's - and releases
    // it after an idle minute, under memory pressure, or when a newer reader
    // needs the room (video/ReaderPool.h).  A reader that is not open is
    // simply destroyed by park().
    if (m_reader) {
        const bool parked = video::ReaderPool::instance().park(std::move(m_reader));
        PluginLog::debug("video: '{}' reader {} on release", m_path.filename().string(),
                         parked ? "parked in the pool" : "released");
    }
    m_reader.reset();
    // The NVDEC decoders.  The one behind the importer's own GPU frame runs
    // in the renderer device's PRIMARY context and holds its own retain on
    // it, so it can outlive this instance safely: it is parked in the
    // process-wide GpuDecoderPool (its frame cache trimmed to the last few
    // frames first), and the next unquiet or the next instance of this clip
    // takes it back with its NVDEC decoders and decode position intact.
    // The direct path's decoders run in a CALLER's context (Premiere's), which
    // nothing here can keep alive: the pool refuses them, and they are
    // released now, giving their VRAM back exactly as a quiet always did.
    video::GpuDecoderPool& gpuPool = video::GpuDecoderPool::instance();
    for (auto& entry : m_gpuDecoders) {
        if (!entry.second) {
            continue;
        }
        const bool poolable = video::GpuDecoderPool::poolable(*entry.second);
        const bool parked = gpuPool.park(std::move(entry.second));
        if (poolable) {
            PluginLog::debug("video: '{}' NVDEC decoder {} on release", m_path.filename().string(),
                             parked ? "parked in the pool" : "released (the pool refused it)");
        }
    }
    m_gpuDecoders.clear();

    // Drop the frame and analysis caches: they are pure caches, and holding
    // a 6000x3000 float image (288 MB) across a quiet would defeat the point
    // of quieting.
    m_lastFrame = RenderedFrame{};
    m_seamTables.clear();
    m_gains.clear();
    resetParallaxLocked();

    closeClipFile(m_fileHandle);
}

Result<std::unique_ptr<video::GpuClipDecoder>> ImporterInstance::takeOrOpenGpuDecoder(
    const video::GpuDecoderOptions& options, void* expectedContext, bool& warm) {
    // The caller holds m_mutex.
    warm = false;

    // ---- 1. a parked decoder of this exact file version and options ----------
    // releaseHeavy() parks the importer frame's decoder on every quiet and
    // close, so an unquiet - or the new instance Premiere opens for a Source
    // Settings change - finds its NVDEC decoders, decode position and last
    // frames here.  Only decoders in the primary context are ever parked.
    std::unique_ptr<video::GpuClipDecoder> parked = video::GpuDecoderPool::instance().take(m_path, m_format, options);
    if (parked) {
        // It must decode into the very context the caller stitches in (the
        // renderer's primary context).  The parked decoder's own retain keeps
        // that context from being destroyed, so a mismatch means a different
        // device was asked for under the same ordinal - never trust it.
        if (expectedContext && parked->cuContext() == expectedContext && parked->contextAlive()) {
            warm = true;
            return parked;
        }
        PluginLog::info("video: '{}' parked NVDEC decoder lives in another context; opening a new one",
                        m_path.filename().string());
        parked.reset();
    }

    // ---- 2. a new one --------------------------------------------------------
    return video::GpuClipDecoder::open(m_path, m_format, options);
}

// ---------------------------------------------------------------------------
//  Derived facts
// ---------------------------------------------------------------------------

double ImporterInstance::fps() const noexcept {
    if (m_rateDen == 0) {
        return 0.0;
    }
    return static_cast<double>(m_rateNum) / static_cast<double>(m_rateDen);
}

// ---------------------------------------------------------------------------
//  [PROXY] An .LRF presented as the proxy of its .OSV
// ---------------------------------------------------------------------------

std::filesystem::path ImporterInstance::proxyOriginalFor(const std::filesystem::path& path) {
    try {
        // Only an .LRF has an original; the extension in any case.
        std::wstring ext = path.extension().wstring();
        for (wchar_t& c : ext) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        if (ext != L".lrf") {
            return {};
        }
        // The camera names the pair alike: CAM_..._D.LRF beside CAM_..._D.OSV.
        // Both spellings are tried, for a case-sensitive volume.
        for (const wchar_t* candidateExt : {L".OSV", L".osv"}) {
            std::filesystem::path candidate = path;
            candidate.replace_extension(candidateExt);
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                return candidate;
            }
        }
    } catch (...) {
        // A path the filesystem library cannot handle has no original.
    }
    return {};
}

std::filesystem::path ImporterInstance::proxyFileFor(const std::filesystem::path& path) {
    try {
        // Only an .OSV has a proxy; the extension in any case.
        std::wstring ext = path.extension().wstring();
        for (wchar_t& c : ext) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        if (ext != L".osv") {
            return {};
        }
        // proxyOriginalFor()'s rule the other way round: the camera names the
        // pair alike, and both spellings are tried for a case-sensitive volume.
        for (const wchar_t* candidateExt : {L".LRF", L".lrf"}) {
            std::filesystem::path candidate = path;
            candidate.replace_extension(candidateExt);
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                return candidate;
            }
        }
    } catch (...) {
        // A path the filesystem library cannot handle has no proxy.
    }
    return {};
}

void ImporterInstance::adoptProxyTimelineLocked() {
    const std::filesystem::path original = proxyOriginalFor(m_path);
    if (original.empty()) {
        return;
    }
    const std::string name = m_path.filename().string();
    const std::string originalName = original.filename().string();

    // ---- the original's timeline --------------------------------------------
    // Opened for its container and metadata only: no decoder, no rig.
    auto file = OsvFile::open(original);
    if (!file.ok()) {
        PluginLog::info("proxy: '{}': '{}' is beside it but cannot be read ({}); presented on its own timeline",
                        name, originalName, file.error().message);
        return;
    }
    auto track = meta::MetadataTrack::load(file.value());
    if (!track.ok()) {
        PluginLog::info("proxy: '{}': '{}' has no readable metadata ({}); presented on its own timeline", name,
                        originalName, track.error().message);
        return;
    }
    auto format = meta::FormatDetector::detect(file.value(), &track.value());
    if (!format.ok() || format.value().sideBySideProxy || format.value().streamH == 0) {
        PluginLog::info("proxy: '{}': '{}' is not a dual-lens original; presented on its own timeline", name,
                        originalName);
        return;
    }
    const TrackInfo* video = file.value().track(format.value().videoTrackIds[0]);
    if (!video || video->timescale == 0 || video->samples.count() == 0) {
        PluginLog::info("proxy: '{}': '{}' has no usable video track; presented on its own timeline", name,
                        originalName);
        return;
    }
    // [VFR] The original's own timeline, exactly as its own instance presents
    // it: its sample list at a constant rate, its held-frame conform at the
    // nominal rate when it dropped frames (the metadata is read only then).
    const ::osv::video::ClipTimeline originalTimeline = ::osv::video::clipTimelineFor(*video, &track.value());
    const std::uint32_t frames = originalTimeline.frameCount();
    std::uint64_t delta = originalTimeline.nominalTicks;
    if (delta == 0 && video->duration > 0) {
        delta = video->duration / video->samples.count();
    }
    if (delta == 0 || delta > 0xFFFFFFFFull) {
        PluginLog::info("proxy: '{}': '{}' has no usable frame rate; presented on its own timeline", name,
                        originalName);
        return;
    }

    // ---- the two first frames on the camera's clock ------------------------------
    // Both files carry the camera's own microsecond timestamp per frame, on
    // one clock, so the original's first frame has an exact place on the
    // proxy's timeline.  Without them the two are taken to start together,
    // which is how the camera writes them.
    double offsetSeconds = 0.0;
    auto proxyFirst = m_track.frame(0);
    auto originalFirst = track.value().frame(0);
    if (proxyFirst.ok() && originalFirst.ok() && proxyFirst.value().timestampUs != 0 &&
        originalFirst.value().timestampUs != 0) {
        offsetSeconds = (static_cast<double>(originalFirst.value().timestampUs) -
                         static_cast<double>(proxyFirst.value().timestampUs)) /
                        1e6;
    }

    // ---- they must cover the same moments ----------------------------------------
    // Both on their own timelines: a dropped-frame .LRF lasts its timeline,
    // not its sample count at the nominal rate.
    const double proxySeconds = fps() > 0.0 ? static_cast<double>(ownTimelineFrameCount()) / fps() : 0.0;
    const double originalSeconds =
        static_cast<double>(frames) * static_cast<double>(delta) / static_cast<double>(video->timescale);
    if (!std::isfinite(offsetSeconds) || offsetSeconds >= proxySeconds || offsetSeconds + originalSeconds <= 0.0) {
        PluginLog::info("proxy: '{}': '{}' was recorded at another time (offset {:.3f} s); presented on its own "
                        "timeline",
                        name, originalName, offsetSeconds);
        return;
    }

    // ---- [VFR] which .LRF sample each timeline frame shows ---------------------------
    // Two constant-rate files keep the rate formula in sourceFrameFor().  If
    // either dropped frames, a rate no longer finds the right picture: each
    // timeline frame gets the .LRF sample captured nearest the moment the
    // original shows there - both carry one camera clock, so a frame the
    // original holds over a gap is held in the proxy too.
    std::vector<std::uint32_t> toSample;
    if (!originalTimeline.identity() || !m_timeline.identity()) {
        const TrackInfo* own = m_file ? m_file->track(m_format.videoTrackIds[0]) : nullptr;
        if (own) {
            const ::osv::video::SampleClock proxyClock = ::osv::video::sampleClockFor(*own, &m_track);
            toSample = ::osv::video::mapTimelineToClock(originalTimeline, proxyClock, offsetSeconds * 1e6);
            PluginLog::info("proxy: '{}' dropped frames (it or '{}'): its {} samples are matched to the original's "
                            "{} timeline frames by the {} clock{}{}",
                            name, originalName, m_frameCount, frames, ::osv::video::timelineClockName(proxyClock.clock),
                            proxyClock.note.empty() ? "" : " - ", proxyClock.note);
        }
        if (toSample.empty()) {
            PluginLog::warn("proxy: '{}': the moments of '{}' could not be matched sample by sample; timeline "
                            "frames are found by frame rate",
                            name, originalName);
        }
    }

    m_proxy.active = true;
    m_proxy.original = original;
    m_proxy.rateNum = video->timescale;
    m_proxy.rateDen = static_cast<std::uint32_t>(delta);
    m_proxy.frameCount = frames;
    m_proxy.originalLensH = format.value().streamH;
    m_proxy.offsetSeconds = offsetSeconds;
    m_proxy.toSample = std::move(toSample);
    PluginLog::info("proxy: '{}' is presented as the proxy of '{}': {} frames at {:.3f} fps (its own: {} at "
                    "{:.3f}), the original's first frame {:+.3f} s into it",
                    name, originalName, frames,
                    static_cast<double>(m_proxy.rateNum) / static_cast<double>(m_proxy.rateDen),
                    ownTimelineFrameCount(), fps(), offsetSeconds);
}

std::uint32_t ImporterInstance::ownSourceFrameFor(std::uint32_t timelineIndex) const noexcept {
    if (m_frameCount == 0) {
        return 0;
    }
    // A constant-rate clip: timeline frame k is sample k.
    if (m_timeline.identity()) {
        return std::min(timelineIndex, m_frameCount - 1u);
    }
    // [VFR] A clip that dropped frames: the held-frame table built at open.
    return std::min(m_timeline.sampleFor(timelineIndex), m_frameCount - 1u);
}

std::uint32_t ImporterInstance::sourceFrameFor(std::uint32_t timelineIndex) const noexcept {
    if (m_frameCount == 0) {
        return 0;
    }
    // A clip on its own timeline.
    if (!m_proxy.active) {
        return ownSourceFrameFor(timelineIndex);
    }
    // [VFR] A proxy pair with dropped frames: the table built at open.
    if (!m_proxy.toSample.empty()) {
        const std::size_t k = std::min<std::size_t>(timelineIndex, m_proxy.toSample.size() - 1u);
        return std::min(m_proxy.toSample[k], m_frameCount - 1u);
    }
    if (m_proxy.rateNum == 0 || m_proxy.rateDen == 0 || m_rateDen == 0) {
        return std::min(timelineIndex, m_frameCount - 1u);
    }
    // The moment of the original's frame on the proxy's clock, then the
    // proxy frame nearest it.
    const double seconds = m_proxy.offsetSeconds + static_cast<double>(timelineIndex) *
                                                       static_cast<double>(m_proxy.rateDen) /
                                                       static_cast<double>(m_proxy.rateNum);
    const double index = std::floor(seconds * static_cast<double>(m_rateNum) / static_cast<double>(m_rateDen) + 0.5);
    if (!(index > 0.0)) {
        return 0;
    }
    if (index >= static_cast<double>(m_frameCount - 1u)) {
        return m_frameCount - 1u;
    }
    return static_cast<std::uint32_t>(index);
}

OutputGeometry ImporterInstance::nativeGeometryLocked() const noexcept {
    OutputGeometry g;
    // An equirect frame is 2:1.  The natural height is the per-lens stream
    // height (3000 for 6K), giving 6000 x 3000.  For the LRF proxy the lens
    // halves are 1024 x 1024, so the native output is 2048 x 1024.
    //
    // m_reader is read here under the caller's lock.  releaseHeavy() resets
    // it under the same lock, so without one this would be a plain data race
    // on a unique_ptr and, worse, an isOpen() call on an object mid-destruction.
    const std::uint32_t h = m_reader && m_reader->isOpen() ? m_reader->lensHeight() : m_format.streamH;
    if (h == 0) {
        return g;
    }
    g.height = static_cast<std::int32_t>(h);
    g.width = g.height * 2;
    return g;
}

OutputGeometry ImporterInstance::nativeGeometry() const noexcept {
    std::lock_guard<std::mutex> guard(m_mutex);
    return nativeGeometryLocked();
}

OutputGeometry ImporterInstance::geometryForLocked(const PrefsBlob& prefs) const noexcept {
    OutputGeometry size;
    switch (prefs.size()) {
    case PrefsOutputSize::UHD4K:   size = OutputGeometry{3840, 1920}; break;
    case PrefsOutputSize::QHD2560: size = OutputGeometry{2560, 1280}; break;
    case PrefsOutputSize::HD2K:    size = OutputGeometry{1920, 960}; break;
    case PrefsOutputSize::Native:
    case PrefsOutputSize::Count:
    default:
        // [PROXY] Native is the ORIGINAL's native size for a proxy.
        if (m_proxy.active && m_proxy.originalLensH > 0) {
            size = OutputGeometry{static_cast<std::int32_t>(2u * m_proxy.originalLensH),
                                  static_cast<std::int32_t>(m_proxy.originalLensH)};
        } else {
            return nativeGeometryLocked();
        }
        break;
    }
    if (!m_proxy.active) {
        return size;
    }
    // [PROXY] The original's size divided by the whole number that brings it
    // closest to the proxy's own detail (2048 x 1024 for the camera's .LRF):
    // 6000 x 3000 -> 2000 x 1000, 3840 x 1920 -> 1920 x 960.  Adobe supports
    // a proxy whose size divides the original's; any other ratio is accepted
    // without a warning and misbehaves.
    const OutputGeometry own = nativeGeometryLocked();
    if (!own.valid() || !size.valid()) {
        return size;
    }
    int bestDivisor = 1;
    std::int64_t bestDistance = std::abs(static_cast<std::int64_t>(size.width) - own.width);
    for (int divisor = 2; divisor <= 16; ++divisor) {
        if (size.width % divisor != 0 || size.height % divisor != 0) {
            continue;
        }
        const std::int64_t distance = std::abs(static_cast<std::int64_t>(size.width / divisor) - own.width);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestDivisor = divisor;
        }
    }
    return OutputGeometry{size.width / bestDivisor, size.height / bestDivisor};
}

OutputGeometry ImporterInstance::geometryFor(const PrefsBlob& prefs) const noexcept {
    // The two fixed sizes need no state at all, but taking the lock
    // unconditionally keeps the contract simple: "geometryFor takes the
    // mutex", with no per-prefs exception for a caller to get wrong.
    std::lock_guard<std::mutex> guard(m_mutex);
    return geometryForLocked(prefs);
}

std::size_t ImporterInstance::extraMemoryUsage() const noexcept {
    // The two HEVC decoders hold a handful of 3000x3000 10-bit reference
    // frames each; the frame cache holds one float RGBA image.  A rough but
    // honest figure is better than zero, which would tell the host we cost
    // nothing while open.
    //
    // Under the lock, because the size it reports depends on m_reader via
    // nativeGeometryLocked() and imQuietFile can be resetting that reader on
    // another thread while the host asks us what we cost.
    std::lock_guard<std::mutex> guard(m_mutex);
    std::size_t bytes = 0;
    // Per LENS, so the LRF proxy's side-by-side track is halved (lensW()).
    const std::size_t lensPixels = static_cast<std::size_t>(m_format.lensW()) * m_format.lensH();
    bytes += lensPixels * 2u /*10-bit in 16-bit words*/ * 3u /*Y + interleaved UV*/ / 2u * 8u /*DPB*/ * 2u /*lenses*/;
    const OutputGeometry g = nativeGeometryLocked();
    if (g.valid()) {
        bytes += static_cast<std::size_t>(g.width) * static_cast<std::size_t>(g.height) * 4u * sizeof(float);
    }
    return bytes;
}

AudioDecoder* ImporterInstance::audio() {
    std::lock_guard<std::mutex> guard(m_mutex);
    return audioImpl();
}

AudioDecoder* ImporterInstance::audioImpl() {
    if (m_audioChannels <= 0 || m_audioTrackId == 0) {
        return nullptr;
    }
    if (m_audio) {
        return m_audio.get();
    }
    if (m_audioProbed) {
        // A previous attempt failed; do not retry on every audio request.
        return nullptr;
    }
    m_audioProbed = true;

    auto opened = AudioDecoder::open(m_path);
    if (!opened.ok()) {
        PluginLog::warn("audio: decoder could not be opened for '{}': {}", m_path.filename().string(),
                        opened.error().message);
        return nullptr;
    }
    m_audio = std::make_unique<AudioDecoder>(std::move(opened).value());
    return m_audio.get();
}

// ---------------------------------------------------------------------------
//  Prefs
// ---------------------------------------------------------------------------

void ImporterInstance::applyPrefs(const void* bytes, std::size_t length) {
    std::lock_guard<std::mutex> guard(m_mutex);
    applyPrefsLocked(bytes, length);
}

void ImporterInstance::applyPrefsLocked(const void* bytes, std::size_t length) {
    // A selector that carries no prefs (or a blob written by something else)
    // leaves the current settings alone; fromBytes() already sanitises.
    //
    // [WP-DEFAULTS] "A blob written by something else" includes the
    // zero-filled buffer a host holds for a clip that has no settings yet.
    // It used to fall through to fromBytes(), which turns it into
    // PrefsBlob::defaults() - adopted, and published to the engine, as if
    // the host had chosen them - so a new clip lost the user defaults it was
    // seeded with (seedStartingPrefs) on its very first selector.  A buffer
    // that is not ours is now exactly what it means: no settings.
    auto holdsOurBlob = [](const void* candidate) noexcept {
        PrefsBlob probe;
        std::memcpy(&probe, candidate, PrefsBlob::kSize);
        return probe.isValid();
    };
    if (!bytes || length < PrefsBlob::kSize || !holdsOurBlob(bytes)) {
        if (!m_colorBuilt) {
            rebuildColor();
        }
        // [WP-SETTINGS] Whatever is in force here (the defaults, when the
        // host never gave this clip a blob) is what the equirect route
        // renders, so the engine is told - as a statement that fills a blank
        // but never overrides a blob another instance was actually given.
        publishSettingsLocked(false);
        return;
    }
    const PrefsBlob incoming = PrefsBlob::fromBytes(bytes, length);
    if (m_colorBuilt && incoming == m_prefs) {
        // Nothing changed: the common case, keep every cache.  [WP-SETTINGS]
        // Unless the host's blob was never published from this instance -
        // it first ran on its defaults and the blob equals them - in which
        // case the engine must hear it now, or an older instance's different
        // settings would stay in force.
        if (!m_settingsPublishedFromHost) {
            publishSettingsLocked(true);
        }
        return;
    }

    const PrefsBlob previous = m_prefs;
    m_prefs = incoming;

    // The engine renders this file for the direct GPU path with its OWN
    // instance; tell it what the user chose, so both paths agree.  The
    // engine's instance does not publish back (it would only echo).
    publishSettingsLocked(true);

    // Colour depends on colorOutput, dlogmFit, exposureStops, [WP-LOOK] the
    // Rec.709 look, [WP-HDRPEAK] the PQ output's peak and [WP-HDRTONE] the
    // HDR outputs' transfer function.
    if (!m_colorBuilt || previous.colorOutput != incoming.colorOutput || previous.dlogmFit != incoming.dlogmFit ||
        previous.exposureStops != incoming.exposureStops || previous.look != incoming.look ||
        previous.hdrPeak != incoming.hdrPeak || previous.hdrTone != incoming.hdrTone) {
        rebuildColor();
    }

    // The rig only depends on the calibration choice and [WP-STEADY] on
    // whether the lens rotation is folded into it.  The analysis blend is
    // committed with it, so Hide Mount (its occlusion switch) rebuilds too.
    if (m_parsed && (!m_rigBuilt || m_rigCalibration != incoming.calibrationChoice() ||
                     m_rigLensAlign != incoming.lensAlignChoice() || m_rigLensFocal != incoming.lensFocalChoice() ||
                     m_rigHideMount != incoming.hideMountChoice())) {
        const Status st = rebuildRig();
        if (!st.ok()) {
            PluginLog::warn("prefs: calibration slot {} could not be applied: {}",
                            static_cast<int>(incoming.calibration), st.error().message);
        }
    }

    // Stabilisation depends on the mode only; the attitude track itself is
    // mode independent but the smoothing is not.
    if (m_parsed && (!m_stabBuilt || m_stabBuiltFor != incoming.stab())) {
        rebuildStabilization();
    }

    // Any change at all invalidates the rendered frame and the analyses,
    // because both are keyed on the whole blob.
    m_lastFrame = RenderedFrame{};
    m_seamTables.clear();
    m_gains.clear();
    resetParallaxLocked();
}

PrefsBlob ImporterInstance::prefs() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_prefs;
}

PrefsBlob ImporterInstance::prefsLocked() const noexcept { return m_prefs; }

OsvColorParams ImporterInstance::colorParams() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_color;
}

// [WP-SETTINGS]
void ImporterInstance::publishSettingsLocked(bool fromHost) noexcept {
    // The engine's own instance only ever APPLIES published settings;
    // publishing them back would be an echo.
    if (m_engineOwned) {
        return;
    }
    // The token is taken at the first publication, which for a Premiere
    // instance is imGetInfo8 right after imOpenFile8 - so token order is the
    // order Premiere opened its instances in.
    if (m_settingsPublisher == 0) {
        m_settingsPublisher = engineNewPublisherToken();
    }
    SettingsPublisher who;
    who.token = m_settingsPublisher;
    who.fromHost = fromHost;
    who.importerId = m_importerId.load(std::memory_order_relaxed);
    enginePublishPrefs(m_path, m_prefs, who);
    if (fromHost) {
        m_settingsPublishedFromHost = true;
    }
}

// [WP-DEFAULTS]
bool ImporterInstance::seedStartingPrefs(const PrefsBlob& prefs, std::string source) {
    std::lock_guard<std::mutex> guard(m_mutex);
    // Too late once the clip is parsed (the rig was built from the prefs in
    // force), once anything else was built from them, or once the host has
    // handed over a blob: re-seeding then would change a clip nobody touched.
    if (m_parsed || m_colorBuilt || m_rigBuilt || m_stabBuilt || m_settingsPublishedFromHost) {
        return false;
    }
    // A blob that did not come from us is never trusted, even from this
    // module's own defaults file: sanitise, or fall back to the built-in.
    PrefsBlob clean = prefs;
    if (!clean.isValid()) {
        clean = PrefsBlob::defaults();
    }
    clean.sanitise();
    m_prefs = clean;
    m_defaultsSource = std::move(source);
    return true;
}

// [WP-DEFAULTS]
bool ImporterInstance::takeUserDefaultsNotice(std::string& source) {
    std::lock_guard<std::mutex> guard(m_mutex);
    // Nothing to announce for the built-in defaults, for a clip the host has
    // given stored settings (those replaced the seed), or a second time.
    if (m_defaultsSource.empty() || m_defaultsNoticeTaken || m_settingsPublishedFromHost) {
        return false;
    }
    m_defaultsNoticeTaken = true;
    source = m_defaultsSource;
    return true;
}

AudioDecoder* ImporterInstance::audioLocked() { return audioImpl(); }

void ImporterInstance::rebuildColor() {
    const color::InputEncoding input = inputEncodingFor(m_format.colorMode);
    // The camera always writes narrow-range YCbCr.  The expansion is built
    // for the DECODED sample scale, not the stream's coded depth: the decoder
    // widens the LRF proxy's 8-bit samples to 10 bits, and an expansion built
    // for 8 turned the whole proxy magenta (video::kDecodedSampleBits).
    // The look is passed for every output; makeColorParams applies it only to
    // Rec.709 (the one output with a fitted look) and ignores it otherwise.
    // [WP-HDRPEAK] Likewise the HDR peak, which only the PQ output uses, and
    // [WP-HDRTONE] the transfer function, which only D-Log M to PQ / HLG uses.
    m_color = color::makeColorParams(toDlogMFit(m_prefs.fit()), toOutputTransfer(m_prefs.color()),
                                     m_prefs.exposureStops, input, true,
                                     video::kDecodedSampleBits, nullptr, color::kBt2408SceneScale,
                                     toLook(m_prefs.lookChoice()), m_prefs.hdrPeakNits(),
                                     toHdrTone(m_prefs.hdrToneChoice()));
    m_colorBuilt = true;
}

void ImporterInstance::rebuildStabilization() {
    m_stabBuiltFor = m_prefs.stab();
    m_stabBuilt = true;
    m_stabParams = geom::StabilizationParams{};
    m_stabParams.mode = toStabMode(m_prefs.stab());
    m_attitude.reset();
    m_smoothedAttitude.clear();

    if (m_stabParams.mode == geom::StabilizationMode::Off) {
        return;
    }

    // Convention detection is the rule Pipeline.cpp uses for
    // `--attitude-convention auto`: the verified reading of the stored
    // attitude, levelled on that attitude alone.  The accelerometer is only
    // a canary now: its gravity reaction is compared with the reading's up
    // and the angle lands in the reason string that is logged below.
    geom::AttitudeTrack::Options attOpt;
    const geom::AutoConvention detected = geom::ConventionProbe::autoDetect(m_track);
    detected.applyTo(attOpt);
    PluginLog::info("stabilisation: '{}': attitude reading {}: {}", m_path.filename().string(),
                    geom::attitudeConventionName(detected.conv), detected.reason);

    auto built = geom::AttitudeTrack::build(m_track, attOpt);
    if (!built.ok() || built.value().sampleCount() == 0) {
        // No IMU: degrade to no stabilisation rather than failing the clip.
        PluginLog::oncef("stab-no-imu", PluginLog::Level::Warn,
                         "stabilisation requested but the clip carries no usable attitude samples; disabled");
        m_stabParams.mode = geom::StabilizationMode::Off;
        return;
    }
    m_attitude = std::move(built).value();
    m_referenceAttitude = m_attitude->worldFromBody(m_attitude->beginUs());

    std::vector<Quatd> perFrame;
    perFrame.reserve(m_attitude->samples().size());
    for (const auto& s : m_attitude->samples()) {
        perFrame.push_back(s.worldFromBody);
    }
    // How the rig is mounted, from the whole track: identity for a camera
    // held lenses-level (every Osmo 360 clip), a quarter turn for one flown
    // lens-up / lens-down (the Avata 360), whose heading would otherwise sit
    // in gimbal lock.
    m_stabParams.mount = geom::levellingMount(perFrame, m_attitude->worldUp());
    if (m_stabParams.mount.distance(Mat3d::identity()) > 0.0) {
        PluginLog::info("stabilisation: '{}': the lens axes are vertical over the clip; levelling takes its heading "
                        "from the body's horizontal axis",
                        m_path.filename().string());
    }

    // Smooth and Smooth + horizon lock both read the smoothed orientation;
    // the smoothing is one pass over the track, done once per mode change.
    if (geom::stabilizationUsesSmoothing(m_stabParams.mode)) {
        // [PROXY] The smoothing window is counted in this clip's own samples
        // - one attitude sample per frame - but it stands for a span of TIME.
        // An .LRF presented as its .OSV's proxy (VEGAS Draft / Preview
        // playback, a Premiere proxy) runs at half the original's rate: 15 of
        // its frames were 0.6 s where the original smooths over 0.3 s, so the
        // preview and the render framed differently (up to 1.2 deg of heading
        // on a day car clip, 1.9 deg at night).  The proxy therefore smooths
        // over the same SECONDS as its original: sigma x own fps / original
        // fps (7.5 frames at 25 fps for a 50 fps .OSV).  A clip on its own
        // timeline - every .OSV, a lone .LRF - keeps its sigma untouched, so
        // its render is exactly what it was.
        if (m_proxy.active && m_proxy.rateNum > 0 && m_proxy.rateDen > 0) {
            const double originalFps =
                static_cast<double>(m_proxy.rateNum) / static_cast<double>(m_proxy.rateDen);
            const double ownFps = fps();
            const double scaled = m_stabParams.smoothSigmaFrames * ownFps / originalFps;
            // Only a finite, positive window is taken; anything else keeps
            // the default rather than switch the smoothing off (Smoother
            // reads a sigma <= 0 as "no smoothing").
            if (std::isfinite(scaled) && scaled > 0.0 && ownFps > 0.0) {
                PluginLog::info("stabilisation: '{}': smoothing over {:.3f} of its frames ({:.3f} s at {:.3f} "
                                "fps), the window of its original '{}' at {:.3f} fps",
                                m_path.filename().string(), scaled, scaled / ownFps, ownFps,
                                m_proxy.original.filename().string(), originalFps);
                m_stabParams.smoothSigmaFrames = scaled;
            } else {
                PluginLog::warn("stabilisation: '{}': no usable proxy smoothing window ({} fps against {} fps); "
                                "smoothing over {} frames",
                                m_path.filename().string(), ownFps, originalFps, m_stabParams.smoothSigmaFrames);
            }
        }
        m_smoothedAttitude = geom::Smoother(m_stabParams.smoothSigmaFrames).smooth(perFrame);
    }
}

Mat3d ImporterInstance::stabilizationFor(std::uint32_t frameIndex) const {
    if (!m_attitude || m_stabParams.mode == geom::StabilizationMode::Off) {
        return Mat3d::identity();
    }
    // Prefer the metadata track's own timestamp for the frame; fall back to
    // nominal spacing from the start of the attitude track.
    double tUs = m_attitude->beginUs();
    auto fm = m_track.frame(frameIndex);
    if (fm.ok()) {
        tUs = static_cast<double>(fm.value().timestampUs);
    } else {
        const double f = fps();
        if (f > 0.0) {
            tUs += static_cast<double>(frameIndex) * 1e6 / f;
        }
    }
    const Quatd wfb = m_attitude->worldFromBody(tUs);
    // The smoothed pose for the modes that read it; a frame past the end of
    // the smoothed track passes none (Smooth: no correction, Smooth +
    // horizon lock: the horizon lock of the raw pose).
    std::optional<Quatd> smoothed;
    if (geom::stabilizationUsesSmoothing(m_stabParams.mode) && frameIndex < m_smoothedAttitude.size()) {
        smoothed = m_smoothedAttitude[frameIndex];
    }
    return geom::stabilizationBodyFromWorld(wfb, m_stabParams, m_referenceAttitude, m_attitude->worldUp(), smoothed);
}

// ---------------------------------------------------------------------------
//  [WP-PHOTO] The render-only blend (photometric seam fix, stage 1)
// ---------------------------------------------------------------------------

// The blob stores the default inset as tenths of a degree; the library owns
// the value.  Two statements of one default must not drift apart.
static_assert(PrefsBlob::kDefaultSeamInsetTenths == 26 && render::kDefaultSeamInsetDeg == 2.6,
              "PrefsBlob's default seam inset must match render::kDefaultSeamInsetDeg");

void ImporterInstance::refreshRenderBlend() noexcept {
    // The analysis blend with the Source Settings inset applied.  An inset of
    // 0 returns m_blend unchanged, bit for bit, so "no inset" renders exactly
    // as the importer did before the render blend existed.
    m_renderBlend = render::insetRenderBlend(m_blend, m_prefs.seamInsetDeg());
}

std::string ImporterInstance::rendererName() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_rendererName;
}

// ---------------------------------------------------------------------------
//  Rendering
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  [WP-TEMPORAL] anchored per-bucket analyses
// ---------------------------------------------------------------------------
const video::FramePair* ImporterInstance::anchorPairLocked(std::uint32_t bucket, std::uint32_t index,
                                                           const video::FramePair& pair, bool decode) {
    // The caller holds m_mutex (applyAnalyses' contract).
    // ---- the anchor: the bucket's first frame -------------------------------------
    const std::uint64_t anchor64 = static_cast<std::uint64_t>(bucket) * render::kParallaxBucketFrames;
    if (anchor64 >= m_frameCount) {
        return nullptr;  // defensive: a bucket past the clip has no anchor
    }
    const auto anchor = static_cast<std::uint32_t>(anchor64);
    if (index == anchor) {
        return &pair;  // the frame itself: free, and the common case in playback and export
    }
    if (const auto it = m_analysisFrames.find(anchor); it != m_analysisFrames.end()) {
        return &it->second.pair;  // already decoded for another analysis of this frame
    }

    // ---- decoded the way `pair` was: same decoder, same planes, same analyses -------
    // A device frame's anchor must be a device frame too: the analyses shade
    // their bands on the GPU from one and on the CPU from the other, so mixing
    // the two would make a bucket depend on which path measured it.
    AnalysisFrame frame;
    const auto failed = [&](const std::string& why) -> const video::FramePair* {
        if (m_anchorFailures.size() > 4096) {
            m_anchorFailures.clear();  // bounded: it only throttles a log line
        }
        if (m_anchorFailures.insert(bucket).second) {
            PluginLog::debug("frame {} (bucket {}): anchor frame {} could not be decoded ({})", index, bucket, anchor,
                             why);
        }
        return nullptr;
    };
    if (pair.onDevice()) {
        if (m_analysisGpuDecoder == nullptr) {
            return failed("no GPU decoder for a device frame");
        }
        if (!decode && !m_analysisGpuDecoder->isCached(anchor)) {
            return nullptr;  // not free: an Interactive frame does not wait for it
        }
        auto lease = m_analysisGpuDecoder->acquire(anchor);
        if (!lease.ok()) {
            return failed(lease.error().message);
        }
        if (!lease.value().valid() || !lease.value().pair().onDevice()) {
            return failed("the decoder returned no device frame");
        }
        frame.pair = lease.value().pair();
        frame.lease = std::move(lease).value();
    } else {
        if (!decode) {
            return nullptr;  // a host decode is never free
        }
        const Status reader = ensureReader();
        if (!reader.ok()) {
            return failed(reader.error().message);
        }
        auto read = readPair(anchor);
        if (!read.ok()) {
            return failed(read.error().message);
        }
        frame.pair = std::move(read).value();
    }
    if (!frame.pair.valid() && !frame.pair.onDevice()) {
        return failed("an empty frame");
    }
    const auto inserted = m_analysisFrames.emplace(anchor, std::move(frame)).first;
    return &inserted->second.pair;
}

ImporterInstance::BucketCorrection ImporterInstance::bucketCorrectionLocked(std::uint32_t bucket, std::uint32_t index,
                                                                            const video::FramePair& pair,
                                                                            bool wantParallax, bool wantSeam,
                                                                            AnchorMeasure how, ThreadPool& pool) {
    // The caller holds m_mutex; m_parallaxMutex is taken here for the grids.
    BucketCorrection out;
    const bool now = how == AnchorMeasure::Now;
    // The bucket holding the frame itself never loses its correction to an
    // anchor that cannot be decoded: it is then measured on the frame.
    const bool ownBucket = render::parallaxBucket(index) == bucket;
    const auto measuredOn = [&](bool decode) -> const video::FramePair* {
        const video::FramePair* anchor = anchorPairLocked(bucket, index, pair, decode);
        return (anchor == nullptr && now && ownBucket) ? &pair : anchor;
    };
    // Interactive requests read the stand-in lane after the anchored caches;
    // the own bucket of one without a free anchor is measured into it, on
    // the frame itself.  An Exact request (Now, Cached) never touches it.
    const bool readStandIns = how == AnchorMeasure::IfFree || how == AnchorMeasure::LookUp;
    const bool measureStandIn = how == AnchorMeasure::IfFree && ownBucket;

    // ---- the grid ------------------------------------------------------------------
    bool gridKnown = !wantParallax;
    if (wantParallax) {
        bool queued = false;
        bool standInKnown = false;
        std::shared_ptr<const render::ParallaxWarpGrid> standInGrid;
        {
            std::lock_guard<std::mutex> lock(m_parallaxMutex);
            if (const auto it = m_parallaxGrids.find(bucket); it != m_parallaxGrids.end()) {
                gridKnown = true;
                out.grid = it->second;  // nullptr: refused
            } else if (readStandIns) {
                if (const auto si = m_standInGrids.find(bucket); si != m_standInGrids.end()) {
                    standInKnown = true;
                    standInGrid = si->second;  // nullptr: refused
                }
            }
            queued = (m_parallaxBusyBucket && *m_parallaxBusyBucket == bucket) ||
                     (m_parallaxPending && m_parallaxPending->bucket == bucket);
        }
        // Cutting the bands is the only step that needs the decoded frame,
        // and it is cheap (68 rows), so it always happens here.  The flow
        // solve - ~95 % of the cost - runs here only for an Exact request.
        if (!gridKnown && (now || (how == AnchorMeasure::IfFree && !queued))) {
            const video::FramePair* anchor = measuredOn(now);
            // [WP-TEMPORAL] No free anchor: the frame's own bands, for the
            // stand-in lane - unless that already holds the bucket.
            const bool standInJob = anchor == nullptr && measureStandIn && !standInKnown;
            const video::FramePair* source = standInJob ? &pair : anchor;
            if (source != nullptr) {
                const render::ParallaxWarpParams pw = parallaxParamsLocked();
                const auto tBand = std::chrono::steady_clock::now();
                auto bands = render::measureParallaxBands(m_rig, *source, m_blend, pw, nullptr, pool);
                const double bandMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBand).count();
                if (!bands.ok()) {
                    PluginLog::debug("frame {} (bucket {}) of '{}': parallax bands failed ({}); rendering without",
                                     index, bucket, clipLogName(m_path), bands.error().message);
                } else if (now) {
                    const auto t0 = std::chrono::steady_clock::now();
                    // Per-cell counts on the side, for the refusal line's
                    // consistent share (output only: the grid is the same).
                    render::ParallaxCellStats cells;
                    auto grid = render::parallaxFromBands(bands.value(), pw, &pool, bandMs, &cells);
                    const double ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() +
                        bandMs;
                    // The clip on every line: two clips' buckets interleave in one log.
                    if (grid.ok()) {
                        const render::ParallaxWarpGrid& g = grid.value();
                        PluginLog::debug("frame {} (bucket {}, anchor {}) of '{}': parallax {} in {:.0f} ms (flow "
                                         "{:.0f}), consistent {:.0f}%, gated {}/{}, disparity mean {:.2f} / max {:.2f} "
                                         "deg",
                                         index, bucket, source->index, clipLogName(m_path),
                                         render::flowBackendName(g.usedBackend), ms, g.flowMs,
                                         100.0 * g.consistentFraction(), g.gatedCells, g.measuredCells,
                                         g.meanAbsCorrectionDeg, g.maxAbsCorrectionDeg);
                        out.grid = std::make_shared<const render::ParallaxWarpGrid>(std::move(grid).value());
                    } else {
                        // Built only at Debug: the consistent share walks every
                        // band pixel, and the line is the only thing that uses it.
                        if (PluginLog::enabled(PluginLog::Level::Debug)) {
                            PluginLog::debug("frame {} (bucket {}, anchor {}) of '{}': parallax refused after {:.0f} "
                                             "ms ({}, {}); {}",
                                             index, bucket, source->index, clipLogName(m_path), ms,
                                             grid.error().message,
                                             consistentShareText(cells, bands.value(), pw.minConsistentFraction),
                                             wantSeam ? "using the seam table instead" : "rendering without it");
                        }
                        out.grid = nullptr;  // a stored nullptr records the refusal
                    }
                    gridKnown = true;
                    std::lock_guard<std::mutex> lock(m_parallaxMutex);
                    m_parallaxGrids[bucket] = out.grid;
                    trimAnalysisCache(m_parallaxGrids, kMaxParallaxCache, bucket);
                } else {
                    // Interactive: hand the bands to the worker and move on.  A
                    // single slot, latest wins - while scrubbing only the frame
                    // the user stops on matters.
                    {
                        std::lock_guard<std::mutex> lock(m_parallaxMutex);
                        ParallaxJob job;
                        job.bucket = bucket;
                        job.generation = m_parallaxGeneration;
                        job.bands = std::move(bands).value();
                        job.params = pw;
                        job.bandMs = bandMs;
                        job.standIn = standInJob;  // [WP-TEMPORAL] the lane the result goes to
                        m_parallaxPending = std::move(job);
                    }
                    // Started lazily and under m_mutex, which is also what
                    // stopParallaxWorker() runs under, so start and stop can
                    // never race on the std::thread object.
                    if (!m_parallaxWorker.joinable()) {
                        try {
                            m_parallaxWorker = std::thread(&ImporterInstance::parallaxWorkerLoop, this);
                        } catch (const std::exception& e) {
                            PluginLog::warn("parallax: could not start the background worker ({}); interactive "
                                            "frames will render without the correction",
                                            e.what());
                            std::lock_guard<std::mutex> lock(m_parallaxMutex);
                            m_parallaxPending.reset();
                        }
                    }
                    m_parallaxCv.notify_one();
                }
            }
        }
        // [WP-TEMPORAL] Not anchored (yet): the stand-in lane's grid, if any.
        if (!gridKnown && standInKnown) {
            gridKnown = true;
            out.grid = std::move(standInGrid);
            out.standIn = true;
        }
    }
    if (!gridKnown) {
        return out;  // not measured yet: unknown
    }

    // ---- the seam table where there is no grid ------------------------------------------
    // The refused bucket's fallback, and the whole correction with parallax off.
    if (out.grid == nullptr && wantSeam) {
        // The anchored table first: cached, or measured now (Exact) or from a
        // free anchor (Interactive).
        std::map<std::uint32_t, SeamTableEntry>* lane = &m_seamTables;
        auto cached = m_seamTables.find(bucket);
        const video::FramePair* source = nullptr;
        if (cached == m_seamTables.end()) {
            const bool measure = now || how == AnchorMeasure::IfFree;
            source = measure ? measuredOn(now) : nullptr;
            if (source == nullptr && readStandIns) {
                // [WP-TEMPORAL] Interactive without a free anchor: the stand-in
                // lane's table, searched on the frame itself when it has none.
                lane = &m_standInTables;
                cached = m_standInTables.find(bucket);
                source = (cached == m_standInTables.end() && measureStandIn) ? &pair : nullptr;
            }
        }
        if (cached == lane->end() && source != nullptr) {
            render::SeamSearchParams sp;
            auto profile = render::searchSeam(m_rig, *source, m_blend, sp, pool);
            if (profile.ok()) {
                // The table and the confidence of each of its columns: the
                // glide to the next bucket steps only where both are sure.
                render::SeamProfile& p = profile.value();
                SeamTableEntry entry;
                entry.shiftDeg = std::move(p.shiftDeg);
                entry.confidence = std::move(p.confidence);
                cached = lane->emplace(bucket, std::move(entry)).first;
                trimAnalysisCache(*lane, lane == &m_seamTables ? kMaxAnalysisCache : kMaxStandInCache, bucket);
            } else {
                // Searched and failed: the bucket renders without a table.
                PluginLog::debug("frame {} (bucket {}): seam search failed ({}); rendering without a seam table",
                                 index, bucket, profile.error().message);
            }
        }
        if (cached == lane->end() && source == nullptr) {
            return out;  // the table is not known yet: neither is the correction
        }
        if (cached != lane->end()) {
            out.table = cached->second.shiftDeg;
            // Only a confidence that matches the table column for column is
            // carried; otherwise the glide treats the table as ungated.
            if (cached->second.confidence.size() == cached->second.shiftDeg.size()) {
                out.tableConfidence = cached->second.confidence;
            }
        }
        out.standIn = out.standIn || lane == &m_standInTables;
    }
    out.known = true;
    return out;
}

ImporterInstance::AnalysisOutcome ImporterInstance::applyAnalyses(std::uint32_t index, const video::FramePair& pair,
                                                                 bool draft, RenderPurpose purpose, ThreadPool& pool,
                                                                 render::RenderParamsBuilder& builder) {
    // The caller holds m_mutex (renderFrame's contract), which is also what
    // guards the seam and gain caches below; the parallax state has its own
    // m_parallaxMutex because the background worker touches it.
    //
    // The same three conditions renderFrame uses for its cache key, derived
    // from the same inputs so the two can never disagree.
    const bool wantSeam = m_prefs.seamSearch != 0 && !draft;
    const bool wantParallax = m_prefs.parallaxEnabled() && !draft;
    const bool exactWanted = purpose == RenderPurpose::Exact;

    // ---- 2-D parallax correction (ParallaxWarp.h) --------------------------
    // When it yields a grid, the grid REPLACES the 1-D seam table instead of
    // composing with it.  The grid already contains the along-meridian
    // correction the table makes - spatially resolved rather than one number
    // per column - plus the cross-meridian one the table cannot express, and
    // composing the two measured WORSE than the grid alone.  On the sample
    // clip, whole-band overlap NCC on frames 0 / 32 / 64:
    //     seam table alone   0.897 / 0.902 / 0.900
    //     grid alone         0.916 / 0.918 / 0.921
    //     table + grid       0.906 / 0.902 / 0.909
    // (after the table the residual is smaller, so the benefit gate keeps
    // fewer cells, and the table's heavily smoothed per-column shift stays
    // where the grid would have done better).  Replacing it also saves the
    // table's cost.  When the grid is refused - featureless content with too
    // little consistent flow - the seam table below is the fallback, so
    // turning parallax on never leaves a frame with LESS correction.
    // Every analysis is keyed by BUCKET, not frame: see the temporal
    // schedule in ParallaxWarp.h for why one measurement per
    // kParallaxBucketFrames frames loses nothing a viewer can see.
    const std::uint32_t bucket = render::parallaxBucket(index);
    bool parallaxApplied = false;
    bool frameExact = true;
    // [WP-SEAMTOOLS] The parallax grid this frame renders with (glided or
    // borrowed), for the Near / Far Offset to add to after the carve.
    std::shared_ptr<const render::ParallaxWarpGrid> appliedGrid;
    // [WP-TEMPORAL] Anchors decoded for this frame's analyses live until it is
    // built, then go (an 8K pair is ~180 MB on the host).
    struct AnchorRelease {
        std::map<std::uint32_t, AnalysisFrame>& frames;
        ~AnchorRelease() { frames.clear(); }
    } anchorRelease{m_analysisFrames};
    // [WP-VIGNETTE] lens shading first: the photometric field and the exposure match are measured on corrected lenses
    prepareLensShading(index, pair, draft, exactWanted, pool);
    // [WP-PHOTO] photometric seam field: measured first, so its usable rim is the carved seam's Rim cost below
    const render::PhotoRimPenaltyScope photoRimScope = preparePhotoSeam(index, pair, draft, exactWanted, pool);

    // ---- [WP-STEADY] which schedule serves this frame's seam corrections ----
    // The clip correction (Parallax Grid Steady, or Auto's verdict) replaces
    // all three per-bucket analyses - grid, seam table, carve - with one
    // measured on the clip's fixed sample frames, so nothing at the seam
    // moves from frame to frame.  Follows scene keeps the per-bucket code
    // below exactly as it was.
    const SteadyUse steadyUse = (wantParallax || wantSeam) ? steadyUseLocked(purpose) : SteadyUse::PerBucket;
    const std::shared_ptr<const render::ClipSteady> clipSteady =
        steadyUse == SteadyUse::Clip ? m_steadyFrame.clip : nullptr;
    if (steadyUse == SteadyUse::Clip && clipSteady) {
        // The clip grid for every frame alike (none when the clip's flow was
        // refused - the clip seam table below is then the correction).
        if (wantParallax && clipSteady->grid && clipSteady->grid->valid()) {
            const render::ParallaxWarpGrid& g = *clipSteady->grid;
            builder.warp(g.uv, g.w, g.h, g.latMinRad, g.latMaxRad);
            parallaxApplied = true;
            appliedGrid = clipSteady->grid;  // [WP-SEAMTOOLS]
        }
    } else if (steadyUse == SteadyUse::StandIn) {
        // Interactive while the clip correction is being measured: the sample
        // grid nearest this frame, if one exists yet.  Not final either way.
        frameExact = false;
        if (wantParallax) {
            std::shared_ptr<const render::ParallaxWarpGrid> nearest;
            std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
            for (const auto& [frame, grid] : m_steadyFrame.samples) {
                const std::uint32_t d = frame > index ? frame - index : index - frame;
                if (grid && grid->valid() && d < best) {
                    best = d;
                    nearest = grid;
                }
            }
            if (nearest) {
                builder.warp(nearest->uv, nearest->w, nearest->h, nearest->latMinRad, nearest->latMaxRad);
                parallaxApplied = true;
                appliedGrid = nearest;  // [WP-SEAMTOOLS]
            }
        }
    }

    if ((wantParallax || wantSeam) && steadyUse == SteadyUse::PerBucket) {
        // ---- [WP-TEMPORAL] this bucket and its glide partner ------------------
        // Each bucket's correction is its grid when the flow was accepted, else
        // its seam table (or nothing), measured on the bucket's ANCHOR.  An
        // Exact frame measures both buckets now; an Interactive one measures
        // its own from a free anchor (the flow in the background), else on
        // itself into the stand-in lane, and takes its partner from the caches.
        const AnchorMeasure ownHow = exactWanted ? AnchorMeasure::Now : AnchorMeasure::IfFree;
        const BucketCorrection own =
            bucketCorrectionLocked(bucket, index, pair, wantParallax, wantSeam, ownHow, pool);
        BucketCorrection partner;
        const bool hasPartner = bucket > 0;
        if (hasPartner) {
            partner = bucketCorrectionLocked(bucket - 1, index, pair, wantParallax, wantSeam,
                                             exactWanted ? AnchorMeasure::Now : AnchorMeasure::LookUp, pool);
        }
        // A stand-in on either side (Interactive only) makes the frame non-final.
        if ((own.known && own.standIn) || (partner.known && partner.standIn)) {
            frameExact = false;
        }

        // ---- choose what to glide between ----------------------------------------
        // `from` null: no glide, `to` alone.  A known partner is glided FROM
        // whatever it holds - a grid, a table, or nothing - so the correction
        // never steps at a bucket edge: a refused bucket glides through the
        // uncorrected geometry (a zero warp) into its table and back, instead
        // of switching the grid off and the table on in one frame.
        const BucketCorrection* from = nullptr;
        const BucketCorrection* to = nullptr;
        double w = 1.0;
        if (own.known) {
            to = &own;
            if (hasPartner && partner.known) {
                from = &partner;
                w = render::parallaxCrossfadeWeight(index);
            } else if (hasPartner && !exactWanted) {
                // Interactive without its partner yet: its own correction alone,
                // which an Exact request would glide - not final.
                frameExact = false;
            }
        } else {
            // Interactive, own bucket not measured yet: the bucket right before
            // it (what the previous frames ended on) at full weight, or the
            // uncorrected geometry - never a correction from further away.
            static_assert(kParallaxBorrowBuckets == 1, "an Interactive stand-in borrows bucket - 1 only");
            frameExact = false;
            if (hasPartner && partner.known) {
                to = &partner;
            }
        }

        // ---- the warp: grid to grid, or a grid to / from a zero warp --------------
        if (to != nullptr) {
            const render::ParallaxWarpGrid* gFrom = from != nullptr ? from->grid.get() : nullptr;
            const render::ParallaxWarpGrid* gTo = to->grid.get();
            std::shared_ptr<const render::ParallaxWarpGrid> apply;
            if (from == nullptr) {
                apply = to->grid;  // no glide: the bucket's own grid exactly (or none)
            } else if (gFrom != nullptr && gTo != nullptr) {
                auto blended = render::blendParallaxGrids(*gFrom, *gTo, w);
                apply = blended.ok() ? std::make_shared<const render::ParallaxWarpGrid>(std::move(blended).value())
                                     : to->grid;
            } else if (gFrom != nullptr || gTo != nullptr) {
                // One side refused (or measured nothing): it stands for a zero
                // warp of the other's layout.
                const render::ParallaxWarpGrid zero = render::zeroParallaxGridLike(gFrom != nullptr ? *gFrom : *gTo);
                auto blended = render::blendParallaxGrids(gFrom != nullptr ? *gFrom : zero,
                                                          gTo != nullptr ? *gTo : zero, w);
                if (blended.ok()) {
                    apply = std::make_shared<const render::ParallaxWarpGrid>(std::move(blended).value());
                } else {
                    PluginLog::debug("frame {} (bucket {}): parallax glide failed ({}); rendering the bucket's own "
                                     "correction",
                                     index, bucket, blended.error().message);
                    apply = to->grid;
                }
            }
            if (apply && apply->valid()) {
                builder.warp(apply->uv, apply->w, apply->h, apply->latMinRad, apply->latMaxRad);
                parallaxApplied = true;
                appliedGrid = apply;  // [WP-SEAMTOOLS]
            }

            // ---- the seam table: only on the side(s) without a grid ---------------
            // Faded by the same weight, so it comes in as the grid goes out -
            // column by column, only while the two sides agree: where they
            // differ by more than measurement noise (a near object came or
            // went between the anchors) the column steps to the newer side at
            // the anchor instead of keeping the stale table on screen for most
            // of the bucket (render::blendSeamTables).
            const std::vector<float>* tFrom =
                (from != nullptr && from->grid == nullptr && !from->table.empty()) ? &from->table : nullptr;
            const std::vector<float>* tTo = (to->grid == nullptr && !to->table.empty()) ? &to->table : nullptr;
            // Each table's per-column confidence gates the step: a large
            // change steps at the anchor only where both measurements are
            // sure of it (a near object arrived); matching noise on
            // featureless columns glides instead of jumping every 8 frames.
            const std::vector<float>* cFrom =
                (tFrom != nullptr && !from->tableConfidence.empty()) ? &from->tableConfidence : nullptr;
            const std::vector<float>* cTo = (tTo != nullptr && !to->tableConfidence.empty()) ? &to->tableConfidence
                                                                                             : nullptr;
            if (wantSeam && (tFrom != nullptr || tTo != nullptr)) {
                if (from == nullptr) {
                    if (tTo != nullptr) {
                        builder.seam(*tTo);  // no glide: the table exactly
                    }
                } else {
                    render::blendSeamTables(tFrom, tTo, w, m_seamTableFrame, render::kSeamTableGlideNoiseDeg,
                                            render::kSeamTableStepDeg, cFrom, cTo);
                    if (!m_seamTableFrame.empty()) {
                        builder.seam(m_seamTableFrame);
                    }
                }
            }
        }
    }

    if (wantSeam && !parallaxApplied && steadyUse == SteadyUse::Clip) {
        // [WP-STEADY] the clip seam table, where the clip has no grid.
        if (clipSteady && clipSteady->seamTable && !clipSteady->seamTable->empty()) {
            builder.seam(*clipSteady->seamTable);
        }
    }

    // ---- [WP-FLARE] sun ghost removal (FlareStage.h) --------------------------
    // Before the carve, which reads this frame's model through the penalty.
    // The frame's metered scene brightness (ISO, shutter, aperture) tells the
    // stage when the sun cannot be in view; read only when the stage will
    // look (NaN, "not recorded", otherwise).
    const bool flareWanted = m_prefs.flareRemoval != 0 && !draft;
    const double sceneEv100 =
        flareWanted ? FlareStage::sceneEv100(m_track, index) : std::numeric_limits<double>::quiet_NaN();
    const FlareStage::Outcome flare = m_flare.apply(index, pair, m_rig, m_color, sceneEv100,
                                                    m_prefs.flareRemoval != 0, draft, exactWanted, pool, builder,
                                                    m_path.filename().string());
    frameExact = frameExact && flare.exact;

    // ---- [WP-SEAM] carved blend seam ---------------------------------------
    // Under the seam preference: "seam search" now means both halves of the
    // seam - the disparity correction above and WHERE the two lenses meet.
    std::shared_ptr<const render::BlendSeam> carvedSeam;
    if (wantSeam && steadyUse == SteadyUse::Clip) {
        // [WP-STEADY] the clip's carved seam: one line for every frame.
        if (clipSteady && clipSteady->seam && clipSteady->seam->valid()) {
            render::applyBlendSeam(builder, *clipSteady->seam);
            carvedSeam = clipSteady->seam;
        }
    } else if (wantSeam && steadyUse == SteadyUse::PerBucket) {
        carvedSeam = applyCarvedSeam(index, pair, wantParallax, purpose, pool, builder, frameExact);
    }
    // (StandIn: the feather blend until the clip's seam is carved.)
    // ---- [WP-SEAMTOOLS] Near / Far Offset and Seam Smoothing, on that seam ---
    applySeamTools(index, carvedSeam.get(), appliedGrid.get(), builder);

    // Scene Light: at night the global exposure match is a ratio of means the
    // street lights dominate - it flipped sign within a second and made sky
    // seam steps of up to 10.9 codes on the night driving clip (4.1 with no
    // correction) - so the night profile renders without it on EVERY path,
    // the field-refused fallback included.  The builder's gain stays identity.
    if (m_prefs.gainMatch != 0 && !nightProfileLocked()) {
        // [WP-TEMPORAL] The bucket's gains are measured on its anchor, through
        // the anchor's shading model, into m_gains - by a non-draft request
        // only: a draft never decodes an anchor (a thumbnail or a prefetch
        // must stay cheap) and never measures the lens shading, so what it
        // measured could differ from what an Exact frame of the bucket
        // measures.  A draft, or an Interactive frame without a free anchor,
        // measures on itself through the shading it renders with, into the
        // stand-in lane, and is not final.
        const std::array<Vec3d, 2>* gains = nullptr;
        if (const auto cached = m_gains.find(bucket); cached != m_gains.end()) {
            gains = &cached->second;
        } else {
            const video::FramePair* gainPair = draft ? nullptr : anchorPairLocked(bucket, index, pair, exactWanted);
            if (gainPair == nullptr && exactWanted && !draft) {
                gainPair = &pair;  // Exact: an anchor that cannot be decoded - the frame itself
            }
            render::BandParams band;
            if (gainPair != nullptr) {
                // [WP-VIGNETTE] on the lenses as the anchor's frames render them
                const std::shared_ptr<const render::LensShadingModel> gainShading =
                    shadingModelForLocked(bucket * render::kParallaxBucketFrames);
                auto g = render::estimateGain(m_rig, *gainPair, m_blend, band, pool, gainShading.get());
                if (g.ok()) {
                    std::array<Vec3d, 2> measured{g.value().gain[0], g.value().gain[1]};
                    const auto stored = m_gains.emplace(bucket, measured).first;
                    trimAnalysisCache(m_gains, kMaxAnalysisCache, bucket);
                    gains = &stored->second;
                } else {
                    PluginLog::debug("frame {} (bucket {}): gain estimation failed ({}); rendering without exposure "
                                     "matching",
                                     index, bucket, g.error().message);
                }
            } else {
                // The stand-in: measured once per bucket, on the first frame
                // that needs it, with the shading this frame renders with.
                frameExact = false;
                auto standIn = m_standInGains.find(bucket);
                if (standIn == m_standInGains.end()) {
                    auto g = render::estimateGain(m_rig, pair, m_blend, band, pool, m_shadingFrame.get());
                    if (g.ok()) {
                        std::array<Vec3d, 2> measured{g.value().gain[0], g.value().gain[1]};
                        standIn = m_standInGains.emplace(bucket, measured).first;
                        trimAnalysisCache(m_standInGains, kMaxStandInCache, bucket);
                    } else {
                        PluginLog::debug("frame {} (bucket {}): stand-in gain estimation failed ({}); rendering "
                                         "without exposure matching",
                                         index, bucket, g.error().message);
                    }
                }
                if (standIn != m_standInGains.end()) {
                    gains = &standIn->second;
                }
            }
        }
        if (gains != nullptr) {
            builder.gain((*gains)[0], (*gains)[1]);
        }
    }

    frameExact = applyPhotoSeam(builder) && frameExact;  // [WP-PHOTO] rim + gain field (after the global gain)
    frameExact = applyLensShading(builder) && frameExact;  // [WP-VIGNETTE] the lens shading correction
    frameExact = frameExact && m_sceneFrameExact;  // Scene Light: a provisional day profile is not final
    // Hide Mount Auto: the full mask stands in until the verdict lands.
    frameExact = frameExact && m_mountState != MountState::Pending;
    return AnalysisOutcome{parallaxApplied, frameExact};
}

// ---------------------------------------------------------------------------
//  [WP-PHOTO] photometric seam field
// ---------------------------------------------------------------------------
namespace {

/// The rig flattened into the numbers the photometric field depends on: both
/// lenses' intrinsics and body-to-lens rotations.  Two rigs with the same key
/// see every band pixel identically.
[[nodiscard]] std::vector<double> photoRigKey(const geom::LensRig& rig) {
    std::vector<double> key;
    key.reserve(2u * (4u + 5u + 1u + 9u) + 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        const geom::KannalaBrandt5& L = rig.lens[i];
        key.insert(key.end(), {L.fx, L.fy, L.cx, L.cy, L.thetaMaxRad});
        key.insert(key.end(), L.k.begin(), L.k.end());
        key.insert(key.end(), std::begin(rig.bodyToLens[i].m), std::end(rig.bodyToLens[i].m));
    }
    key.push_back(static_cast<double>(rig.streamW));
    key.push_back(static_cast<double>(rig.streamH));
    return key;
}

/// Append both lenses' occlusion polygons to a measurement's cache key.
/// Hide Mount Auto rebuilds the polygons without touching anything else of
/// the rig, so a field measured through the old ones must not be reused.
/// For a clip whose polygons never change this only adds constant entries.
void appendOcclusionKey(std::vector<double>& key, const geom::LensRig& rig) {
    for (std::size_t i = 0; i < 2; ++i) {
        key.push_back(static_cast<double>(rig.occlusionPolyStream[i].size()));
        for (const Vec2d& v : rig.occlusionPolyStream[i]) {
            key.push_back(v.x);
            key.push_back(v.y);
        }
    }
}

}  // namespace

render::PhotoSeamParams ImporterInstance::photoParamsLocked() const noexcept {
    render::PhotoSeamParams params;
    // The prefs enum and the library enum share their values (PrefsBlob.h).
    switch (m_prefs.photoSeamMode()) {
    case PrefsPhotoSeam::RimOnly: params.mode = render::PhotoSeamMode::RimOnly; break;
    case PrefsPhotoSeam::RimAndGain: params.mode = render::PhotoSeamMode::RimAndGain; break;
    case PrefsPhotoSeam::Off:
    case PrefsPhotoSeam::Count:
    default: params.mode = render::PhotoSeamMode::Off; break;
    }
    params.strength = m_prefs.photoStrengthPercent() / 100.0;
    // Scene Light: at night a short, gently clamped field - the 20 deg decay
    // painted a halo band into a crushed black sky.  Luma decays over 6 deg
    // beyond the overlap, chroma (full inside it) over 3.  Day keeps every
    // default, so a day clip renders exactly as before.
    if (nightProfileLocked()) {
        render::applyNightPhotoProfile(params);
    }
    return params;
}

render::PhotoRimPenaltyScope ImporterInstance::preparePhotoSeam(std::uint32_t index, const video::FramePair& pair,
                                                               bool draft, bool exact, ThreadPool& pool) {
    // The caller (applyAnalyses) holds m_mutex, which guards all of this.
    m_photoFrame.reset();
    m_photoFrameExact = true;
    const render::PhotoSeamParams params = photoParamsLocked();
    if (params.mode == render::PhotoSeamMode::Off) {
        return render::PhotoRimPenaltyScope(nullptr, nullptr);
    }
    try {
        // A different rig (calibration slot, lens-protector correction)
        // invalidates every rim and gain measured with the old one.
        std::vector<double> key = photoRigKey(m_rig);
        // Hide Mount: the field is measured through the analysis blend's
        // occlusion mask, which Source Settings can switch without touching
        // the rig - fields measured through the other mask are stale.
        key.push_back(m_blend.useOcclusionMask ? 1.0 : 0.0);
        appendOcclusionKey(key, m_rig);  // Hide Mount Auto rebuilds the polygons themselves
        // Scene Light: each cell is clamped when it is MEASURED, so fields
        // stored under the other profile's clamp are stale too.
        key.push_back(params.maxAbsLog2Gain);
        if (key != m_photoRigKey) {
            m_photo.clear();
            m_photoStandIns.clear();  // [WP-TEMPORAL] measured with the old rig too
            m_photoLast.reset();
            m_photoRigKey = std::move(key);
        }

        // ---- measure this bucket (never for a draft) ------------------------
        // Synchronous for Exact and Interactive alike: it is a band shade
        // (GPU from device frames, CPU from host frames) plus a few ms of
        // statistics per bucket of eight frames.
        const std::uint32_t bucket = render::parallaxBucket(index);
        // One measurement of bucket `b` on `on`, through `shading`, stored in
        // `history` (the anchored one, or the stand-in lane).
        const auto measureInto = [&](render::PhotoSeamHistory& history, std::uint32_t b, const video::FramePair& on,
                                     const render::LensShadingModel* shading, const char* lane) {
            auto field = render::measurePhotoSeam(m_rig, on, m_blend, params, pool, shading);
            if (field.ok()) {
                const render::PhotoSeamField& f = field.value();
                PluginLog::debug("frame {} (bucket {}{}): photometric seam field in {:.1f} ms (bands {:.1f}, stats "
                                 "{:.1f}), trusted {:.1f}%, usable rim {:.2f} / {:.2f} deg, median gain "
                                 "{:+.3f} / {:+.3f} / {:+.3f} stops",
                                 on.index, b, lane, f.bandMs + f.statsMs, f.bandMs, f.statsMs,
                                 f.bandPixels ? 100.0 * static_cast<double>(f.trustedPixels) /
                                                    static_cast<double>(f.bandPixels)
                                              : 0.0,
                                 f.rimMedianDeg[0], f.rimMedianDeg[1], f.medianLog2Gain[0], f.medianLog2Gain[1],
                                 f.medianLog2Gain[2]);
                history.store(b, std::make_shared<const render::PhotoSeamField>(std::move(field).value()), params);
            } else {
                PluginLog::debug("frame {} (bucket {}{}): photometric seam field refused ({}); keeping the seam edge "
                                 "inset and the global gain",
                                 on.index, b, lane, field.error().message);
                history.store(b, nullptr, params);  // not measured again
            }
            history.trim(&history == &m_photo ? kMaxAnalysisCache : kMaxStandInCache, b);
        };
        // [WP-TEMPORAL] One bucket's field, measured on its anchor through the
        // anchor's shading model - exactly what a sequential render measures,
        // whichever frame asks.  False when the anchor is not available (an
        // Interactive frame whose anchor is not free).
        const auto measureBucket = [&](std::uint32_t b, bool decode) -> bool {
            const video::FramePair* anchor = anchorPairLocked(b, index, pair, decode);
            if (anchor == nullptr) {
                if (!(decode && b == bucket)) {
                    return false;
                }
                anchor = &pair;  // an Exact frame never loses its own bucket's field
            }
            // [WP-VIGNETTE] on the lenses as the kernel will blend them
            const std::shared_ptr<const render::LensShadingModel> shading =
                shadingModelForLocked(b * render::kParallaxBucketFrames);
            measureInto(m_photo, b, *anchor, shading.get(), "");
            return true;
        };
        bool standIn = false;
        if (!draft && !m_photo.measured(bucket)) {
            // The glide and EMA partner first, so this bucket is stored after it
            // (an Exact frame is then the same as in a sequential render).
            if (exact && bucket > 0 && !m_photo.measured(bucket - 1)) {
                (void)measureBucket(bucket - 1, true);
            }
            standIn = !measureBucket(bucket, exact);
            // [WP-TEMPORAL] No free anchor (Interactive): measured on the frame
            // itself, through the shading it renders with, into the stand-in
            // lane - once per bucket, with that lane's own EMA and glide - as
            // every frame was measured before the anchoring.
            if (standIn && !m_photoStandIns.measured(bucket)) {
                measureInto(m_photoStandIns, bucket, pair, m_shadingFrame.get(), ", stand-in");
            }
        }

        // ---- the field this frame renders with --------------------------------
        std::shared_ptr<const render::PhotoSeamField> field =
            standIn ? m_photoStandIns.fieldFor(index, params) : m_photo.fieldFor(index, params);
        if (standIn) {
            m_photoFrameExact = false;  // a stand-in, whatever it renders with
        }
        if (field) {
            if (!draft) {
                m_photoLast = field;
            }
        } else if ((draft || standIn) && m_photoLast) {
            // A draft never measures; the last accepted field stands in (and
            // where the stand-in lane refused the bucket), and the frame is
            // not final.
            field = m_photoLast;
            m_photoFrameExact = false;
        }
        m_photoFrame = field;
        // The usable rim as the carved seam's Rim cost, for this thread, for
        // the rest of applyAnalyses (render::PhotoRimPenaltyScope).
        render::installPhotoRimPenaltyHook();
        return render::PhotoRimPenaltyScope(&m_rig, std::move(field));
    } catch (const std::exception& e) {
        // Allocation failure is the realistic case: render without the field.
        PluginLog::warn("photometric seam field: {}; rendering without it", e.what());
        m_photoFrame.reset();
        return render::PhotoRimPenaltyScope(nullptr, nullptr);
    }
}

bool ImporterInstance::applyPhotoSeam(render::RenderParamsBuilder& builder) {
    const render::PhotoSeamParams params = photoParamsLocked();
    std::shared_ptr<const render::PhotoSeamField> field = std::move(m_photoFrame);
    m_photoFrame.reset();
    if (params.mode == render::PhotoSeamMode::Off || !field) {
        return true;  // stage 1 (inset + global gain) stays in force
    }
    builder.photo(*field, params);
    // The per-longitude rim replaces the fixed stage-1 inset: back to the
    // calibrated FOV and its feather, which the kernel then ends at the rim.
    builder.blend(m_blend, true);
    // The field carries the whole lens ratio, so the global gain would count
    // it twice.
    if (params.mode == render::PhotoSeamMode::RimAndGain) {
        builder.gain(Vec3d{1.0, 1.0, 1.0}, Vec3d{1.0, 1.0, 1.0});
    }
    return m_photoFrameExact;
}

// ---------------------------------------------------------------------------
//  Scene Light: the photometric profile (render/SceneLight.h)
// ---------------------------------------------------------------------------
void ImporterInstance::prepareSceneLightLocked(RenderPurpose purpose, bool draft) {
    // The caller holds m_mutex.  Every path below leaves m_sceneLight set.
    m_sceneFrameExact = true;
    const std::string name = m_path.filename().string();

    // ---- a forced choice is taken as it is ------------------------------------------
    switch (m_prefs.sceneLightChoice()) {
    case PrefsSceneLight::Day: m_sceneLight = render::SceneLight::Day; return;
    case PrefsSceneLight::Night: m_sceneLight = render::SceneLight::Night; return;
    case PrefsSceneLight::Auto:
    case PrefsSceneLight::Count:
    default: break;
    }

    // ---- Auto, already decided: the verdict depends only on the clip ----------------
    if (m_sceneVerdict) {
        m_sceneLight = m_sceneVerdict->light;
        return;
    }

    // ---- the metered light, once (metadata only) ---------------------------------------
    if (!m_metered) {
        m_metered = render::meteredLightOf(m_track, m_frameCount);
    }
    if (!render::meteredLightSaysDark(*m_metered)) {
        // Daylight, the ambiguous middle or no metadata: today's profile, decided
        // without a single decoded pixel.
        m_sceneVerdict = render::classifySceneLight(*m_metered, std::nullopt);
        m_sceneLight = m_sceneVerdict->light;
        PluginLog::info("scene light: '{}': {} ({})", name, render::sceneLightName(m_sceneLight),
                        m_sceneVerdict->reason);
        return;
    }

    // ---- dark metered light: the sky decides --------------------------------------------
    // Drafts (thumbnails, prefetch) never start a measurement: importing a
    // folder of night clips must not decode every one of them.
    if (!draft) {
        std::optional<SceneLightRequest> request = sceneRequestLocked();
        if (!request) {
            // No attitude (or no frame): the cap cannot be levelled, so the
            // verdict is the day profile, and final.
            m_sceneVerdict = render::classifySceneLight(*m_metered, std::nullopt);
            m_sceneLight = m_sceneVerdict->light;
            PluginLog::info("scene light: '{}': {} ({}; the clip has no attitude to level the sky by)", name,
                            render::sceneLightName(m_sceneLight), m_sceneVerdict->reason);
            return;
        }
        m_sceneStage.request(*request, name);
        if (purpose == RenderPurpose::Exact) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool settled = m_sceneStage.waitSettled(kSceneExactWait);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!settled && !m_sceneWaitWarned) {
                m_sceneWaitWarned = true;
                PluginLog::warn("scene light: '{}': the sky measurement was not ready after {:.0f} ms; this frame "
                                "renders with the day profile",
                                name, ms);
            } else if (settled && ms > 1.0) {
                PluginLog::debug("scene light: '{}': an exact frame waited {:.0f} ms for the sky measurement", name,
                                 ms);
            }
        }
    }

    // ---- adopt the answer once it has landed ----------------------------------------------
    const SceneLightStage::Snapshot snap = m_sceneStage.snapshot();
    if (!snap.active || !snap.settled) {
        // Still measuring (or a draft before any request): the day profile,
        // provisionally - the frame is not final and stays out of the caches
        // that serve Exact requests.
        m_sceneLight = render::SceneLight::Day;
        m_sceneFrameExact = false;
        return;
    }
    m_sceneVerdict = render::classifySceneLight(*m_metered, snap.cap);
    m_sceneLight = m_sceneVerdict->light;
    PluginLog::info("scene light: '{}': {} ({})", name, render::sceneLightName(m_sceneLight), m_sceneVerdict->reason);
    if (m_sceneLight != render::SceneLight::Day) {
        // Frames rendered while the sky was measured used the day profile:
        // drop the rendered frame and the exposure gains (the photometric
        // fields and shading models go by themselves, keyed by their
        // parameters).
        m_lastFrame = RenderedFrame{};
        m_gains.clear();
    }
}

std::optional<SceneLightRequest> ImporterInstance::sceneRequestLocked() {
    try {
        SceneLightRequest r;
        r.path = m_path;
        r.format = m_format;
        r.containerSamples = containerSamplesUsable(m_file.get(), m_format);

        // ---- the fixed sample frames: the lens rotation fit's three --------------------
        // 10 / 50 / 90 % of the clip, snapped to sync frames on a long clip,
        // so each is one intra decode and the answer depends on the clip alone.
        std::vector<std::uint32_t> syncFrames;
        if (m_file) {
            const std::uint32_t id = m_format.videoTrackIds[0] != 0 ? m_format.videoTrackIds[0] : 1u;
            if (const TrackInfo* video = m_file->track(id); video && video->samples.hasSyncTable()) {
                syncFrames = video->samples.syncSamples();
            }
        }
        const std::vector<std::uint32_t> frames =
            render::clipSampleFrames(m_frameCount, syncFrames, render::kLensRotationSamples, 0.1, 0.9);

        // ---- gravity-up per frame: the stabilisation's attitude, or one of our own --------
        const geom::AttitudeTrack* attitude = m_attitude ? &*m_attitude : nullptr;
        if (!attitude) {
            if (!m_sceneAttitude) {
                // The same reading the stabilisation would use (the stored
                // attitude), built once for a clip rendered with it off.
                geom::AttitudeTrack::Options options;
                geom::ConventionProbe::autoDetect(m_track).applyTo(options);
                auto built = geom::AttitudeTrack::build(m_track, options);
                if (!built.ok() || built.value().sampleCount() == 0) {
                    return std::nullopt;
                }
                m_sceneAttitude = std::move(built).value();
            }
            attitude = &*m_sceneAttitude;
        }
        for (const std::uint32_t f : frames) {
            if (const std::optional<Vec3d> up = render::bodyUpAt(*attitude, m_track, f, fps())) {
                r.frames.push_back(f);
                r.upBody.push_back(*up);
            }
        }
        if (r.frames.empty()) {
            return std::nullopt;
        }

        // ---- the geometry and the decode to scene-linear ------------------------------------
        // The calibration rig and the analysis blend (a rotation of a third of
        // a degree is nothing to a 45 deg cap); the clip's own D-Log M curve,
        // no exposure offset - the cap is compared against METERED grey.
        r.rig = m_baseRig;
        r.blend = m_blend;
        r.linearColor = color::makeColorParams(toDlogMFit(m_prefs.fit()), color::OutputTransfer::Linear, 0.0f,
                                               inputEncodingFor(m_format.colorMode), true, video::kDecodedSampleBits);
        return r;
    } catch (const std::exception& e) {
        PluginLog::warn("scene light: '{}': no sky request ({}); day profile", m_path.filename().string(), e.what());
        return std::nullopt;
    }
}

std::string ImporterInstance::sceneLightTextLocked() const {
    // A forced choice says so; Auto says what it decided and from what.
    switch (m_prefs.sceneLightChoice()) {
    case PrefsSceneLight::Day: return "Day (set in Source Settings)";
    case PrefsSceneLight::Night: return "Night (set in Source Settings)";
    case PrefsSceneLight::Auto:
    case PrefsSceneLight::Count:
    default: break;
    }
    if (m_sceneVerdict) {
        return std::format("{} (auto: {})", render::sceneLightName(m_sceneVerdict->light),
                           render::sceneLightEvidence(*m_sceneVerdict));
    }
    if (m_metered) {
        return std::format("Auto, measuring the sky ({} {:.1f})", m_metered->fromAecLv ? "LV" : "EV100",
                           m_metered->median);
    }
    return "Auto (decided on the first frame)";
}

// ---------------------------------------------------------------------------
//  [WP-VIGNETTE] lens shading correction
// ---------------------------------------------------------------------------
render::LensShadingParams ImporterInstance::shadingParamsLocked() const noexcept {
    render::LensShadingParams params;
    // The prefs enum and the library enum share their values (PrefsBlob.h).
    params.mode = m_prefs.lensShadingMode() == PrefsLensShading::Auto ? render::LensShadingMode::Auto
                                                                       : render::LensShadingMode::Off;
    params.strength = m_prefs.shadingStrengthPercent() / 100.0;
    // Scene Light: its sky gate is a smoothness test with no elevation, which
    // a dark sky, a lit door and a blurred road all pass at night.
    if (nightProfileLocked()) {
        render::applyNightShadingProfile(params);
    }
    return params;
}

std::shared_ptr<const render::LensShadingModel> ImporterInstance::shadingModelForLocked(std::uint32_t frame) const {
    // The caller holds m_mutex.  prepareLensShading's choice for a non-draft
    // frame, without its stand-in: the bucket's stored model glided from its
    // partner, active only, scaled by the strength exactly as there.
    const render::LensShadingParams params = shadingParamsLocked();
    if (params.mode == render::LensShadingMode::Off) {
        return nullptr;
    }
    try {
        std::shared_ptr<const render::LensShadingModel> model = m_shading.modelFor(frame, params);
        if (!model || !model->active()) {
            return nullptr;
        }
        return params.strength >= 1.0 ? model
                                      : std::make_shared<const render::LensShadingModel>(
                                            render::scaledLensShadingModel(*model, params.strength));
    } catch (const std::exception& e) {
        // Allocation failure is the realistic case: measure without it.
        PluginLog::warn("lens shading: {}; measuring without it", e.what());
        return nullptr;
    }
}

void ImporterInstance::prepareLensShading(std::uint32_t index, const video::FramePair& pair, bool draft, bool exact,
                                          ThreadPool& pool) {
    // The caller (applyAnalyses) holds m_mutex, which guards all of this.
    m_shadingFrame.reset();
    m_shadingFrameExact = true;
    const render::LensShadingParams params = shadingParamsLocked();
    try {
        // The photometric fields in m_photo were measured on lenses corrected
        // by THIS mode and strength; any change makes them stale.
        std::vector<double> photoKey{static_cast<double>(params.mode), params.strength};
        if (photoKey != m_shadingPhotoKey) {
            if (!m_shadingPhotoKey.empty()) {
                m_photo.clear();
                m_photoStandIns.clear();  // [WP-TEMPORAL] measured under the old shading too
                m_photoLast.reset();
            }
            m_shadingPhotoKey = std::move(photoKey);
        }
        if (params.mode == render::LensShadingMode::Off) {
            return;
        }
        // A different rig (calibration slot, lens-protector correction) moves
        // every lens angle the models are tabulated in.
        std::vector<double> rigKey = photoRigKey(m_rig);
        // Hide Mount: the models are measured through the analysis blend's
        // occlusion mask too, which can change while the rig does not.
        rigKey.push_back(m_blend.useOcclusionMask ? 1.0 : 0.0);
        appendOcclusionKey(rigKey, m_rig);  // Hide Mount Auto rebuilds the polygons themselves
        if (rigKey != m_shadingRigKey) {
            m_shading.clear();
            m_shadingStandIns.clear();  // [WP-TEMPORAL] measured with the old rig too
            m_shadingLast.reset();
            m_shadingRigKey = std::move(rigKey);
        }

        // ---- measure this bucket (never for a draft) ------------------------
        const std::uint32_t bucket = render::parallaxBucket(index);
        // One measurement of bucket `b` on `on`, stored in `history` (the
        // anchored one, or the stand-in lane).
        const auto measureInto = [&](render::LensShadingHistory& history, std::uint32_t b, const video::FramePair& on,
                                     const char* lane) {
            auto model = render::measureLensShading(m_rig, on, m_blend, params, pool);
            if (model.ok()) {
                const render::LensShadingModel& m = model.value();
                PluginLog::debug("frame {} (bucket {}{}): lens shading in {:.1f} ms (bands {:.1f}); slave {} sectors, "
                                 "peak {:+.4f} ({:+.3f} stop at {:.1f} deg); master {} sectors, peak {:+.4f} "
                                 "({:+.3f} stop at {:.1f} deg)",
                                 on.index, b, lane, m.bandMs + m.statsMs, m.bandMs, m.lens[0].measuredSectors,
                                 m.lens[0].peakAmount, m.lens[0].peakStops, m.lens[0].peakThetaDeg,
                                 m.lens[1].measuredSectors, m.lens[1].peakAmount, m.lens[1].peakStops,
                                 m.lens[1].peakThetaDeg);
                history.store(b, std::make_shared<const render::LensShadingModel>(std::move(model).value()), params);
            } else {
                PluginLog::debug("frame {} (bucket {}{}): lens shading refused ({}); rendering without it", on.index,
                                 b, lane, model.error().message);
                history.store(b, nullptr, params);  // not measured again
            }
            history.trim(&history == &m_shading ? kMaxAnalysisCache : kMaxStandInCache, b);
        };
        // [WP-TEMPORAL] One bucket's model, measured on its anchor; false when
        // the anchor is not available (an Interactive frame, not free).
        const auto measureBucket = [&](std::uint32_t b, bool decode) -> bool {
            const video::FramePair* anchor = anchorPairLocked(b, index, pair, decode);
            if (anchor == nullptr) {
                if (!(decode && b == bucket)) {
                    return false;
                }
                anchor = &pair;  // an Exact frame never loses its own bucket's model
            }
            measureInto(m_shading, b, *anchor, "");
            return true;
        };
        bool standIn = false;
        if (!draft && !m_shading.measured(bucket)) {
            // The glide and EMA partner first (see preparePhotoSeam).
            if (exact && bucket > 0 && !m_shading.measured(bucket - 1)) {
                (void)measureBucket(bucket - 1, true);
            }
            standIn = !measureBucket(bucket, exact);
            // [WP-TEMPORAL] No free anchor: measured on the frame itself into
            // the stand-in lane, once per bucket (see preparePhotoSeam).
            if (standIn && !m_shadingStandIns.measured(bucket)) {
                measureInto(m_shadingStandIns, bucket, pair, ", stand-in");
            }
        }

        // ---- the model this frame renders with ------------------------------
        std::shared_ptr<const render::LensShadingModel> model =
            standIn ? m_shadingStandIns.modelFor(index, params) : m_shading.modelFor(index, params);
        if (standIn) {
            m_shadingFrameExact = false;  // a stand-in, whatever it renders with
        }
        if (model) {
            if (!draft) {
                m_shadingLast = model;
            }
        } else if ((draft || standIn) && m_shadingLast) {
            // A draft never measures; the last accepted model stands in (and
            // where the stand-in lane refused the bucket), and the frame is
            // not final.
            model = m_shadingLast;
            m_shadingFrameExact = false;
        }
        if (model && model->active()) {
            // The strength is applied once, here, so the analyses and the
            // kernel see exactly the same correction.
            m_shadingFrame = params.strength >= 1.0
                                 ? model
                                 : std::make_shared<const render::LensShadingModel>(
                                       render::scaledLensShadingModel(*model, params.strength));
        }
    } catch (const std::exception& e) {
        // Allocation failure is the realistic case: render without it.
        PluginLog::warn("lens shading: {}; rendering without it", e.what());
        m_shadingFrame.reset();
    }
}

bool ImporterInstance::applyLensShading(render::RenderParamsBuilder& builder) {
    std::shared_ptr<const render::LensShadingModel> model = std::move(m_shadingFrame);
    m_shadingFrame.reset();
    if (!model) {
        builder.clearShading();
        return true;
    }
    // Already scaled by the strength (prepareLensShading).
    builder.shading(*model, 1.0);
    return m_shadingFrameExact;
}

// ---------------------------------------------------------------------------
//  [WP-SEAM] carved blend seam
// ---------------------------------------------------------------------------
std::shared_ptr<const render::BlendSeam> ImporterInstance::applyCarvedSeam(std::uint32_t index,
                                                                           const video::FramePair& pair,
                                                                           bool wantParallax, RenderPurpose purpose,
                                                                           ThreadPool& pool,
                                                                           render::RenderParamsBuilder& builder,
                                                                           bool& frameExact) {
    const std::uint32_t bucket = render::parallaxBucket(index);
    const bool exact = purpose == RenderPurpose::Exact;
    // Set when the frame renders a seam from the stand-in lane (Interactive).
    bool usedStandIn = false;

    // ---- [WP-TEMPORAL] one bucket's seam, carved on its anchor -----------------
    // Through the bucket's own correction (its grid, else its table), steered
    // by the bucket's own usable rim, and with NO prior: a prior is a chain
    // back to the first bucket ever carved, so the seam of an exact frame
    // depended on where playback or export started.  The glide below is the
    // temporal smoothing, between two seams that each depend on their own
    // bucket alone.  Without the prior the seam is also free to route around
    // a near object that crosses it faster than the old per-bucket clamp
    // (2 deg per bucket) allowed.  An Interactive frame whose anchor is not
    // free (or whose correction is itself a stand-in) carves on ITSELF,
    // through that correction and with its own rim, into the stand-in lane -
    // on the first frame after its grid lands, as before the anchoring.
    // `how` as for the corrections; null when the bucket cannot be carved
    // (yet).
    const auto seamFor = [&](std::uint32_t b, AnchorMeasure how) -> std::shared_ptr<const render::BlendSeam> {
        {
            std::lock_guard<std::mutex> lock(m_parallaxMutex);
            if (const auto it = m_blendSeams.find(b); it != m_blendSeams.end()) {
                return it->second;
            }
        }
        // Interactive: the stand-in lane's seam after the anchored one.
        const bool interactive = how == AnchorMeasure::IfFree || how == AnchorMeasure::LookUp;
        std::shared_ptr<const render::BlendSeam> standIn;
        if (interactive) {
            if (const auto it = m_standInSeams.find(b); it != m_standInSeams.end()) {
                standIn = it->second;
            }
        }
        const auto useStandIn = [&]() {
            usedStandIn = usedStandIn || standIn != nullptr;
            return standIn;
        };
        if (how == AnchorMeasure::LookUp) {
            return useStandIn();
        }
        // The correction must be settled before the seam can be carved through
        // it; applyAnalyses has just measured it as far as `how` allows.
        const BucketCorrection c = bucketCorrectionLocked(b, index, pair, wantParallax, true,
                                                          interactive ? AnchorMeasure::LookUp : AnchorMeasure::Cached,
                                                          pool);
        if (!c.known) {
            return useStandIn();
        }
        // The anchored seam needs the anchored correction AND the anchor
        // (decoded for an Exact request, free for an Interactive one).
        const video::FramePair* anchor =
            c.standIn ? nullptr : anchorPairLocked(b, index, pair, how == AnchorMeasure::Now);
        bool intoStandIns = false;
        if (anchor == nullptr) {
            if (how == AnchorMeasure::Now && b == bucket) {
                anchor = &pair;  // an Exact frame never loses its own bucket's seam
            } else if (how == AnchorMeasure::IfFree && b == bucket) {
                if (standIn) {
                    return useStandIn();  // carved on an earlier frame of the bucket
                }
                anchor = &pair;  // the stand-in lane: carved on the frame itself
                intoStandIns = true;
            } else {
                return nullptr;
            }
        }
        render::WarpGridView warpView;
        render::SeamCorrection correction;
        if (c.grid && c.grid->valid()) {
            warpView.uv = c.grid->uv.data();
            warpView.w = c.grid->w;
            warpView.h = c.grid->h;
            warpView.latMinRad = c.grid->latMinRad;
            warpView.latMaxRad = c.grid->latMaxRad;
            correction.warp = &warpView;
        } else if (!c.table.empty()) {
            correction.seamShiftDeg = &c.table;
        }
        // [WP-PHOTO] The usable rim of the ANCHOR as the Rim cost - what a
        // sequential render carves with - not the field of whichever frame
        // asked; a scope nests over the frame's own.  A stand-in carve keeps
        // the frame's own (the scope preparePhotoSeam installed).
        std::optional<render::PhotoRimPenaltyScope> rimScope;
        if (!intoStandIns) {
            const render::PhotoSeamParams photoParams = photoParamsLocked();
            std::shared_ptr<const render::PhotoSeamField> rim;
            if (photoParams.mode != render::PhotoSeamMode::Off) {
                rim = m_photo.fieldFor(b * render::kParallaxBucketFrames, photoParams);
            }
            rimScope.emplace(rim ? &m_rig : nullptr, rim);
        }
        render::SeamCarveParams params;
        // [WP-FLARE] Steer away from this frame's ghosts - in its own bucket
        // only: a glide partner carved now is cached for that bucket's frames,
        // and must not keep another bucket's ghosts.
        if (b == bucket) {
            params.penalty = m_flare.seamPenalty();
        }
        // [WP-SEAMTOOLS] Seam Blend / Parallax Blend: the feather widths only
        // (the seam's path is measured over its own window).  Default prefs
        // leave the parameters exactly at their defaults.
        render::applySeamBlendWidths(seamToolsLocked(), params);
        const render::BandParams band = render::ParallaxWarpParams{}.band;
        auto carved = render::carveSeam(m_rig, *anchor, m_blend, band, correction, params, nullptr, pool);
        if (!carved.ok()) {
            PluginLog::debug("frame {} (bucket {}): seam carve failed ({}); keeping the feather blend", index, b,
                             carved.error().message);
            return nullptr;
        }
        const render::BlendSeam& s = carved.value();
        PluginLog::debug("frame {} (bucket {}, {} {}): seam carved in {:.1f} ms through {}, latitude mean {:+.2f} / "
                         "max {:.2f} deg, feather {:.2f} deg mean, {} narrow / {} forced columns",
                         index, b, intoStandIns ? "stand-in on" : "anchor", anchor->index, s.carveMs,
                         correction.warp ? "the parallax grid"
                                         : (correction.seamShiftDeg ? "the seam table" : "no correction"),
                         s.meanLatDeg, s.maxAbsLatDeg, s.meanHalfWidthDeg, s.narrowColumns, s.forcedColumns);
        auto stored = std::make_shared<const render::BlendSeam>(std::move(carved).value());
        if (intoStandIns) {
            m_standInSeams[b] = stored;
            trimAnalysisCache(m_standInSeams, kMaxStandInCache, b);
            usedStandIn = true;
            return stored;
        }
        std::lock_guard<std::mutex> lock(m_parallaxMutex);
        m_blendSeams[b] = stored;
        trimAnalysisCache(m_blendSeams, kMaxAnalysisCache, b);
        return stored;
    };

    // ---- this bucket's seam and its glide partner -----------------------------------
    // Cheap enough for the render thread (two band renders' worth of shading
    // plus a DP over 1024 x ~70 cells), so there is no background path.
    const std::shared_ptr<const render::BlendSeam> own =
        seamFor(bucket, exact ? AnchorMeasure::Now : AnchorMeasure::IfFree);
    const std::shared_ptr<const render::BlendSeam> previous =
        bucket > 0 ? seamFor(bucket - 1, exact ? AnchorMeasure::Now : AnchorMeasure::LookUp) : nullptr;
    if (usedStandIn) {
        frameExact = false;  // a seam measured on another frame than its anchor
    }

    // ---- choose what to apply ------------------------------------------------
    std::shared_ptr<const render::BlendSeam> apply;
    if (own) {
        apply = own;
        // Glide from the previous bucket's seam instead of stepping to this
        // one at the bucket edge - the same schedule as the parallax grid, so
        // the two move together.
        if (previous) {
            auto blended = render::blendSeams(*previous, *own, render::parallaxCrossfadeWeight(index));
            if (blended.ok()) {
                apply = std::make_shared<const render::BlendSeam>(std::move(blended).value());
            }
        } else if (!exact && bucket > 0) {
            frameExact = false;  // an Exact request would glide: not final
        }
    } else if (!exact) {
        // Stand-in: the bucket right before this one (what the previous frames
        // ended on), never one further away.  The frame is then not final.
        frameExact = false;
        apply = previous;
    }
    if (apply) {
        render::applyBlendSeam(builder, *apply);
    }
    return apply;  // [WP-SEAMTOOLS] for the tools that follow the carve
}

// ---------------------------------------------------------------------------
//  [WP-SEAMTOOLS] the carved seam's tweaks
// ---------------------------------------------------------------------------

// The blob stores the seam tools' defaults; the library owns them.  Two
// statements of one default must not drift apart.
static_assert(PrefsBlob::kDefaultSeamBlendDeg == render::kDefaultSeamBlendDeg &&
                  PrefsBlob::kDefaultParallaxBlendDeg == render::kDefaultParallaxBlendDeg,
              "PrefsBlob's seam blend defaults must match SeamTools.h");
static_assert(static_cast<double>(PrefsBlob::kMaxSeamOffsetHundredths) / 100.0 == render::kMaxSeamOffsetDeg,
              "PrefsBlob's offset range must match SeamTools.h");

render::SeamTools ImporterInstance::seamToolsLocked() const noexcept {
    // The blob's decoders turn code 0 into the defaults, so default prefs are
    // default tools field for field.
    render::SeamTools tools;
    tools.seamBlendDeg = m_prefs.seamBlendDeg();
    tools.parallaxBlendDeg = m_prefs.parallaxBlendDeg();
    tools.smoothingDeg = m_prefs.seamSmoothingDeg();
    tools.nearOffsetDeg = m_prefs.nearOffsetDeg();
    tools.farOffsetDeg = m_prefs.farOffsetDeg();
    return tools;
}

void ImporterInstance::applySeamTools(std::uint32_t index, const render::BlendSeam* seam,
                                      const render::ParallaxWarpGrid* grid, render::RenderParamsBuilder& builder) {
    // Both tools act on the carved seam; without one there is nothing to
    // nudge or smooth, and the frame stays exactly as it is.
    if (!seam) {
        return;
    }
    const render::SeamTools tools = seamToolsLocked();
    try {
        // ---- Near / Far Offset: into the warp grid in force -----------------
        if (tools.offsetOn()) {
            auto shifted = render::seamOffsetGrid(grid, *seam, tools.nearOffsetDeg, tools.farOffsetDeg);
            if (shifted.ok()) {
                const render::ParallaxWarpGrid& g = shifted.value();
                builder.warp(g.uv, g.w, g.h, g.latMinRad, g.latMaxRad);
            } else {
                PluginLog::debug("frame {}: seam offset refused ({}); rendering without it", index,
                                 shifted.error().message);
            }
        }
        // ---- Seam Smoothing: the renderer builds the low band --------------
        if (tools.smoothingOn()) {
            builder.seamSmooth(tools.smoothingDeg);
        }
    } catch (const std::exception& e) {
        // Allocation failure is the realistic case: render without the tools.
        PluginLog::warn("seam tools: {}; rendering without them", e.what());
    }
}

// ---------------------------------------------------------------------------
//  [WP-STEADY] per-clip lens alignment and steady seam corrections
// ---------------------------------------------------------------------------

render::ParallaxWarpParams ImporterInstance::parallaxParamsLocked() const noexcept {
    render::ParallaxWarpParams pw;
    pw.backend = toFlowBackendKind(m_prefs.flow());
    // A rig with the lens rotation folded in carries only a small residual
    // per cell, which the default 20 % benefit bar would mostly refuse
    // (render::kAlignedRequiredImprovement has the measurements).  Every
    // other rig keeps the default, so it renders exactly as before.
    if (m_lensAlignState == LensAlignState::Applied) {
        pw.requiredImprovement = render::kAlignedRequiredImprovement;
    }
    return pw;
}

SteadyRequest ImporterInstance::steadyRequestLocked(bool wantRotation, bool wantClip) const {
    SteadyRequest r;
    r.path = m_path;
    r.format = m_format;
    r.frameCount = m_frameCount;
    // The sync frames the sample frames snap to on a long clip - the first
    // lens's track, as the stage's decoder reads it.
    if (m_file) {
        const std::uint32_t id = m_format.videoTrackIds[0] != 0 ? m_format.videoTrackIds[0] : 1u;
        if (const TrackInfo* video = m_file->track(id); video && video->samples.hasSyncTable()) {
            r.syncFrames = video->samples.syncSamples();
        }
    }
    r.baseRig = m_baseRig;
    r.blend = m_blend;  // the ANALYSIS blend, as every per-bucket measurement uses
    r.containerSamples = containerSamplesUsable(m_file.get(), m_format);
    r.wantRotation = wantRotation;
    r.wantClip = wantClip;
    // The clip correction exactly as the per-bucket analyses measure theirs:
    // the flow backend of the prefs (the stage sets the aligned gate itself
    // once it knows the rotation), the seam switches, the Seam Blend /
    // Parallax Blend widths and the photometric rim as the carve's rim cost.
    r.clip.parallax = render::ParallaxWarpParams{};
    r.clip.parallax.backend = toFlowBackendKind(m_prefs.flow());
    r.clip.parallaxOn = m_prefs.parallaxEnabled();
    r.clip.seamOn = m_prefs.seamSearch != 0;
    render::applySeamBlendWidths(seamToolsLocked(), r.clip.carve);
    const render::PhotoSeamParams photo = photoParamsLocked();
    r.clip.rimCost = photo.mode != render::PhotoSeamMode::Off;
    r.clip.photo = photo;
    // [WP-VIGNETTE] that rim on shading-corrected lenses, like the per-bucket field.
    r.clip.shading = shadingParamsLocked();
    r.clip.shadingOn = r.clip.shading.mode == render::LensShadingMode::Auto;
    // Hide Mount Auto: the mount mask, measured (or looked up) by the stage;
    // the clip correction above is then measured through its polygons.
    r.wantMount = m_prefs.hideMountChoice() == PrefsHideMount::Auto;
    r.mount = render::MountMaskParams{};
    return r;
}

bool ImporterInstance::applyMountMaskLocked(geom::LensRig& rig) const {
    if (!m_mountMask) {
        return true;  // the calibration's polygons: nothing to fold in
    }
    // Rebuilt against the calibration rig the verdict was measured through,
    // whatever rotation `rig` carries (applyMountMaskFrom).
    geom::LensRig candidate = rig;
    auto applied =
        render::applyMountMaskFrom(m_baseRig, candidate, *m_mountMask, m_blend, render::MountMaskParams{});
    if (!applied.ok()) {
        PluginLog::warn("hide mount: '{}': the verdict could not be applied ({}); keeping the full mask",
                        m_path.filename().string(), applied.error().message);
        return false;
    }
    rig = std::move(candidate);
    return true;
}

void ImporterInstance::settleMountLocked(const std::shared_ptr<const render::MountMask>& mount,
                                         const std::string& failure) {
    const std::string name = m_path.filename().string();
    m_mountState = MountState::Settled;
    std::string note;
    if (mount) {
        // m_rig carries the calibration's polygons here (no verdict was
        // folded in while it was pending), with or without the rotation; the
        // polygons are rebuilt against m_baseRig, the rig the verdict's
        // columns belong to, and copied in.
        geom::LensRig rig = m_rig;
        auto applied = render::applyMountMaskFrom(m_baseRig, rig, *mount, m_blend, render::MountMaskParams{});
        if (applied.ok()) {
            m_mountMask = mount;
            note = "hide mount: Auto - " + render::describeMountMask(*mount);
            const render::MountMaskApplied& a = applied.value();
            if (a.changed[0] || a.changed[1]) {
                m_rig = std::move(rig);
                // Everything measured so far was measured through the full
                // polygons: the rendered frame, the per-bucket grids, tables,
                // seams and gains (the photometric fields and lens shading
                // models go by themselves, keyed by the polygons).
                m_lastFrame = RenderedFrame{};
                m_seamTables.clear();
                m_gains.clear();
                resetParallaxLocked();
                PluginLog::info("hide mount: '{}': folded into the rig from now on (lens 0 {}, lens 1 {}; {} "
                                "released, {} / {} clamped columns{})",
                                name, a.changed[0] ? "rebuilt" : "unchanged", a.changed[1] ? "rebuilt" : "unchanged",
                                a.releasedColumns, a.clampedColumns[0], a.clampedColumns[1],
                                a.mergedStretches > 0
                                    ? std::format(", {} stretch(es) kept to fit the polygon budget", a.mergedStretches)
                                    : std::string());
            } else {
                PluginLog::info("hide mount: '{}': Auto keeps the calibration's polygons", name);
            }
        } else {
            note = "hide mount: Auto - keeping the full mask; the verdict could not be applied (" +
                   applied.error().message + ")";
            PluginLog::warn("{} ('{}')", note, name);
        }
    } else {
        // The stage abstained (too few frames, a failure): the full mask.
        const std::string why = failure.empty() ? std::string("not measured") : failure;
        note = "hide mount: Auto - keeping the full mask (" + why + ")";
    }
    // The Properties panel's note describes the polygons actually in force.
    std::erase_if(m_notes, [](const std::string& n) { return n.starts_with("hide mount: "); });
    m_notes.push_back(note);
}

void ImporterInstance::settleLensAlignLocked(const std::optional<LensAlignVerdict>& rotation,
                                             const std::string& failure) {
    const std::string name = m_path.filename().string();
    std::string note;
    if (rotation && rotation->accepted) {
        geom::LensRig rig = m_baseRig;
        const Status folded = render::applyLensRotation(rig, rotation->wRad);
        if (folded.ok()) {
            // Hide Mount Auto: m_baseRig carries the calibration's polygons,
            // so a verdict already in force is folded in again.
            (void)applyMountMaskLocked(rig);
            m_rig = std::move(rig);
            m_lensAlignState = LensAlignState::Applied;
            note = "lens alignment: " + rotation->summary;
            // Everything measured so far was measured through the old rig:
            // the rendered frame, the per-bucket grids, tables, seams, gains
            // and ghost models (the photometric fields go by themselves, keyed
            // by the rig).  Stand-ins rendered before this point were
            // non-exact, so no Exact frame ever saw the old rig.
            m_lastFrame = RenderedFrame{};
            m_seamTables.clear();
            m_gains.clear();
            resetParallaxLocked();
            PluginLog::info("lens alignment: '{}': folded into the rig from now on", name);
        } else {
            m_lensAlignState = LensAlignState::Refused;
            note = "lens alignment: keeping the calibration - " + folded.error().message;
        }
    } else {
        m_lensAlignState = LensAlignState::Refused;
        note = "lens alignment: keeping the calibration - " +
               (rotation ? rotation->summary : (failure.empty() ? std::string("not measured") : failure));
    }
    // The Properties panel's note describes the rig actually in force.
    std::erase_if(m_notes, [](const std::string& n) { return n.starts_with("lens alignment: "); });
    m_notes.push_back(note);
}

void ImporterInstance::prepareSteadyLocked(RenderPurpose purpose, bool draft) {
    m_steadyFrame = SteadyStage::Snapshot{};
    m_steadyFrame.serial = m_steady.serial();  // what a frame built without the stage is current against
    // ---- what this clip's prefs ask for ----------------------------------------
    const bool alignAuto = m_prefs.lensAlignChoice() == PrefsLensAlign::Auto;
    const bool corrections = m_prefs.parallaxEnabled() || m_prefs.seamSearch != 0;
    const bool wantClip = m_prefs.parallaxGridChoice() != PrefsParallaxGrid::FollowsScene && corrections;
    const bool mountAuto = m_prefs.hideMountChoice() == PrefsHideMount::Auto;  // Hide Mount Auto
    if (!alignAuto && !wantClip && !mountAuto) {
        return;  // the per-bucket schedule and the calibration, exactly as before
    }

    // ---- the request, and for an Exact frame the wait ----------------------------
    // Drafts (thumbnails, prefetch) never start a measurement: importing a
    // folder of clips must not decode every one of them in the background.
    if (!draft) {
        m_steady.request(steadyRequestLocked(alignAuto, wantClip), m_path.filename().string());
        if (purpose == RenderPurpose::Exact) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool settled = m_steady.waitSettled(wantClip, kSteadyExactWait);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!settled && !m_steadyWaitWarned) {
                m_steadyWaitWarned = true;
                PluginLog::warn("steady: '{}': the per-clip analyses were not ready after {:.0f} ms; this frame "
                                "renders with the per-moment corrections",
                                m_path.filename().string(), ms);
            } else if (settled && ms > 1.0) {
                PluginLog::debug("steady: '{}': an exact frame waited {:.0f} ms for the per-clip analyses",
                                 m_path.filename().string(), ms);
            }
        }
    }

    // ---- the snapshot this frame renders with ------------------------------------
    SteadyStage::Snapshot snap = m_steady.snapshot();
    // A rotation that has just been answered: fold it into the rig (or give
    // up on it) BEFORE anything below reads m_rig.
    if (m_lensAlignState == LensAlignState::Pending && snap.active && snap.rotationSettled) {
        settleLensAlignLocked(snap.rotation, snap.failure);
    }
    // Hide Mount Auto: a mount mask that has just been answered rebuilds the
    // polygons, likewise before anything below reads m_rig.
    if (m_mountState == MountState::Pending && snap.active && snap.mountSettled) {
        settleMountLocked(snap.mount, snap.mountFailure);  // its own reason, not the rotation's
    }
    m_steadyFrame = std::move(snap);
}

ImporterInstance::SteadyUse ImporterInstance::steadyUseLocked(RenderPurpose purpose) const noexcept {
    const PrefsParallaxGrid mode = m_prefs.parallaxGridChoice();
    if (mode == PrefsParallaxGrid::FollowsScene || !m_steadyFrame.active) {
        return SteadyUse::PerBucket;
    }
    if (m_steadyFrame.clipSettled) {
        // Settled without a result: the measurement failed (logged), and the
        // per-moment corrections are the graceful fallback.
        if (!m_steadyFrame.clip) {
            return SteadyUse::PerBucket;
        }
        // Auto follows its verdict; Steady always holds.
        if (mode == PrefsParallaxGrid::Auto && !m_steadyFrame.clip->decision.steady) {
            return SteadyUse::PerBucket;
        }
        return SteadyUse::Clip;
    }
    // Still being measured.  An Exact frame only gets here when its wait timed
    // out: it takes the per-bucket analyses, which it can measure on the spot,
    // rather than a stand-in.
    return purpose == RenderPurpose::Exact ? SteadyUse::PerBucket : SteadyUse::StandIn;
}

Result<ImporterInstance::DirectFrame> ImporterInstance::directFrame(std::uint32_t index, void* cuContext,
                                                                   RenderPurpose purpose, int outputTransfer) {
    // The caller holds m_mutex (the header contract) and has cuContext
    // current on this thread (the engine export pushes it).
    if (!m_parsed) {
        return Error{ErrorCode::InvalidArgument, "directFrame before the clip was opened"};
    }
    if (index >= m_frameCount) {
        return Error{ErrorCode::InvalidArgument, "frame index " + std::to_string(index) + " beyond the clip"};
    }
    if (!cuContext) {
        return Error{ErrorCode::InvalidArgument, "directFrame without a CUDA context"};
    }
    if (outputTransfer > OSV_TRANSFER_PASSTHROUGH) {
        return Error{ErrorCode::InvalidArgument,
                     "directFrame: unknown output transfer " + std::to_string(outputTransfer)};
    }
    ensureGpuAnalyses();
    if (!m_colorBuilt) {
        rebuildColor();
    }
    if (!m_stabBuilt) {
        rebuildStabilization();
    }

    // ---- the decoder for this context ---------------------------------------
    // Opened on first use and kept: its VRAM cache is what makes stepping
    // around inside a GOP free.  One per context, because a frame decoded in
    // one CUDA context cannot be read by kernels in another.
    auto decoder = m_gpuDecoders.find(cuContext);
    if (decoder == m_gpuDecoders.end()) {
        video::GpuDecoderOptions options;
        options.cuContext = cuContext;
        const auto t0 = std::chrono::steady_clock::now();
        auto opened = video::GpuClipDecoder::open(m_path, m_format, options);
        if (!opened.ok()) {
            return opened.error();
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        PluginLog::info("direct: '{}' NVDEC decoder opened in the host's CUDA context in {:.0f} ms",
                        m_path.filename().string(), ms);
        decoder = m_gpuDecoders.emplace(cuContext, std::move(opened).value()).first;
    }
    if (!decoder->second) {
        m_gpuDecoders.erase(decoder);
        return Error{ErrorCode::Internal, "directFrame: an empty decoder slot"};
    }

    OSV_TRY_ASSIGN(video::GpuFrameLease lease, decoder->second->acquire(index));
    if (!lease.valid() || !lease.pair().onDevice()) {
        return Error{ErrorCode::Decoder, "directFrame: the decoder returned no device frame"};
    }

    // ---- colour ---------------------------------------------------------------
    // The clip's Source Settings choice unless the caller asked for the space
    // its frames must end up in (the sequence's working space).  Built exactly
    // like rebuildColor() so the two can only ever differ by the transfer.
    OsvColorParams color = m_color;
    if (outputTransfer >= 0 && outputTransfer != m_color.transfer) {
        // [WP-LOOK] the clip's look travels with it: a PQ clip rendered into
        // a Rec.709 working space gets the look the user chose for Rec.709.
        // [WP-HDRPEAK] So does its HDR peak: any clip rendered into a PQ
        // working space rolls off into the peak chosen for it.
        // [WP-HDRTONE] And its transfer function: a Rec.709 clip rendered
        // into a PQ or HLG working space gets the style chosen for HDR.
        color = color::makeColorParams(toDlogMFit(m_prefs.fit()), static_cast<color::OutputTransfer>(outputTransfer),
                                       m_prefs.exposureStops, inputEncodingFor(m_format.colorMode), true,
                                       video::kDecodedSampleBits, nullptr,
                                       color::kBt2408SceneScale, toLook(m_prefs.lookChoice()),
                                       m_prefs.hdrPeakNits(), toHdrTone(m_prefs.hdrToneChoice()));
    }

    // ---- the stitch block ------------------------------------------------------
    // Assembled exactly as renderFrame assembles the equirect's, from the same
    // analysis caches, so a direct view and the importer's equirect of the
    // same frame are stitched identically.
    prepareSceneLightLocked(purpose, /*draft=*/false);  // Scene Light: the profile the analyses read
    prepareSteadyLocked(purpose, /*draft=*/false);  // [WP-STEADY] before anything reads m_rig
    render::RenderParamsBuilder builder;
    refreshRenderBlend();  // [WP-PHOTO] the render-only inset blend; analyses keep m_blend
    builder.rig(m_rig).color(color).blend(m_renderBlend, true);
    builder.alphaCoverage(true);
    std::shared_ptr<ThreadPool> pool = HostContext::instance().threadPoolShared();
    if (!pool) {
        return Error{ErrorCode::Internal, "directFrame: no thread pool"};
    }
    // [WP-TEMPORAL] a bucket's anchor is decoded by this same decoder
    const ScopedPointer<video::GpuClipDecoder> analysisDecoder(m_analysisGpuDecoder, decoder->second.get());
    const AnalysisOutcome analyses = applyAnalyses(index, lease.pair(), /*draft=*/false, purpose, *pool, builder);
    builder.stabilization(stabilizationFor(index));

    // The equirect's SIZE is irrelevant to the direct renderer (it replaces
    // every view field); the mode and the stabilised Rout are what it takes.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    const OutputGeometry g = geometryForLocked(m_prefs);
    map.w = g.valid() ? g.width : 2048;
    map.h = g.valid() ? g.height : 1024;
    builder.equirect(map);

    OSV_TRY_ASSIGN(render::RenderJob job, builder.build(lease.pair()));
    if (!job.planesOnDevice[0] || !job.planesOnDevice[1]) {
        return Error{ErrorCode::Internal, "directFrame: the stitch job does not reference the device frames"};
    }

    DirectFrame out;
    out.lease = std::move(lease);
    out.job = std::move(job);
    out.exact = analyses.exact;
    return out;
}

Result<const render::ImageRGBAf*> ImporterInstance::renderFrame(std::uint32_t index, const OutputGeometry& geometry,
                                                                bool draft, RenderPurpose purpose,
                                                                int outputTransfer) {
    // The caller holds m_mutex (see the header contract); nothing here locks
    // again or the instance would deadlock on itself.
    if (!m_parsed) {
        return Error{ErrorCode::InvalidArgument, "renderFrame before the clip was opened"};
    }
    if (!geometry.valid()) {
        return Error{ErrorCode::InvalidArgument, "renderFrame with an empty output geometry"};
    }
    if (index >= m_frameCount) {
        return Error{ErrorCode::InvalidArgument, "frame index " + std::to_string(index) + " beyond the clip"};
    }
    if (outputTransfer > OSV_TRANSFER_PASSTHROUGH) {
        return Error{ErrorCode::InvalidArgument, "renderFrame: unknown output transfer " + std::to_string(outputTransfer)};
    }
    if (!m_colorBuilt) {
        rebuildColor();
    }
    // An override equal to the clip's own transfer is no override: it must
    // share the cache key (and the pixels) of an ordinary request.
    const int transfer = (outputTransfer >= 0 && outputTransfer != m_color.transfer) ? outputTransfer : -1;

    const bool wantSeam = m_prefs.seamSearch != 0 && !draft;
    // The parallax correction runs under exactly the conditions the seam
    // search does - never for a draft request (thumbnails, prefetch, playback
    // that is already falling behind).  Its cost is amortised: one
    // measurement per bucket of frames, off the render thread for an
    // Interactive request (see RenderPurpose and the block below).
    const bool wantParallax = m_prefs.parallaxEnabled() && !draft;
    const bool wantFlare = m_prefs.flareRemoval != 0 && !draft;  // [WP-FLARE]
    const bool exactWanted = purpose == RenderPurpose::Exact;

    // [WP-STEADY] A stand-in frame is stale once the per-clip analyses have
    // published something since it was rendered: even an Interactive request
    // must then get the real correction, not the cached stand-in.
    if (!m_lastFrame.exact && m_lastFrameSteadySerial != m_steady.serial()) {
        m_lastFrame = RenderedFrame{};
    }
    // Cache hit: the host asked for the same frame twice (it does, once per
    // requested pixel format while scrubbing).  An Exact request is never
    // served a frame an Interactive render built with a stand-in analysis.
    if (m_lastFrame.matches(index, geometry, m_prefs, wantSeam, wantParallax, wantFlare, exactWanted, transfer)) {
        m_lastRenderExact = m_lastFrame.exact;  // [WP-STEADY]
        return &m_lastFrame.image;
    }

    if (!m_stabBuilt) {
        rebuildStabilization();
    }

    Status readerStatus = ensureReader();
    if (!readerStatus.ok()) {
        return readerStatus.error();
    }

    // ---- renderer lease ----------------------------------------------------
    auto lease = HostContext::instance().acquireRenderer(toDevicePreference(m_prefs.device()));
    if (!lease.ok()) {
        return lease.error();
    }
    HostContext::RendererLease renderer = std::move(lease).value();
    if (!renderer.renderer || !renderer.pool) {
        return Error{ErrorCode::Internal, "HostContext returned an empty renderer lease"};
    }
    m_rendererName = renderer.backend;
    // A CUDA renderer means a usable device: let the analyses use it too.
    if (renderer.backend == "cuda") {
        ensureGpuAnalyses();
    }

    // ---- decode ------------------------------------------------------------
    auto pair = readPair(index);
    if (!pair.ok()) {
        return pair.error();
    }

    // ---- the stitch job ----------------------------------------------------
    // Rig, colour, blend, coverage alpha, the per-bucket analyses,
    // stabilisation and the equirect map - assembled by the one function the
    // GPU path uses too (buildEquirectJob), so the two paths can never build
    // different parameter blocks for the same frame.
    AnalysisOutcome analyses;
    auto job = buildEquirectJob(index, pair.value(), geometry, draft, purpose, *renderer.pool, analyses, transfer);
    if (!job.ok()) {
        return job.error();
    }
    const bool frameExact = analyses.exact;

    // Render INTO the cached image, reusing its allocation.  At native
    // 6000x3000 a fresh image per frame was 288 MB of allocation and
    // zero-fill that the kernel then overwrote in full - ~50 ms of an
    // importer frame, measured - and it bought nothing.
    //
    // The key is invalidated FIRST: renderInto() may leave a partially
    // written frame behind if it fails, and a key still naming the previous
    // frame must never match that.  The buffer itself survives the failure
    // and is reused by the next attempt.
    m_lastFrame.frameIndex = RenderedFrame::kNoFrame;
    Status rendered = okStatus();
    if (renderer.backend == "cuda") {
        // The CUDA renderer is shared by every clip, and the GPU path of
        // another clip may be streaming the renderer's device buffer back
        // right now; rendering into it underneath that readback would tear
        // the other clip's frame (see cudaRendererOutputMutex()).
        std::lock_guard<std::mutex> outputLock(cudaRendererOutputMutex());
        rendered = renderer.renderer->renderInto(job.value(), m_lastFrame.image);
    } else {
        rendered = renderer.renderer->renderInto(job.value(), m_lastFrame.image);
    }
    if (!rendered.ok()) {
        return rendered.error();
    }

    m_lastFrame.frameIndex = index;
    m_lastFrame.geometry = geometry;
    m_lastFrame.prefs = m_prefs;
    m_lastFrame.seamApplied = wantSeam;
    m_lastFrame.parallaxWanted = wantParallax;
    m_lastFrame.flareWanted = wantFlare;  // [WP-FLARE]
    m_lastFrame.exact = frameExact;
    m_lastFrame.outputTransfer = transfer;
    m_lastFrameSteadySerial = m_steadyFrame.serial;  // [WP-STEADY] what the stand-in test compares
    m_lastRenderExact = frameExact;                  // [WP-STEADY]
    return &m_lastFrame.image;
}

// ---------------------------------------------------------------------------
//  [WP-IMPORTER] The stitch job, shared by the host and the GPU path
// ---------------------------------------------------------------------------

OsvColorParams ImporterInstance::colorForTransfer(int outputTransfer) const {
    // The clip's own block unless a single request asked for another
    // transfer.  Rebuilt exactly as rebuildColor() builds m_color (and as
    // directFrame() builds the direct path's), so an override differs from
    // the clip's own block in the transfer and nothing else.
    if (outputTransfer < 0 || outputTransfer > OSV_TRANSFER_PASSTHROUGH || outputTransfer == m_color.transfer) {
        return m_color;
    }
    // [WP-LOOK] the same look as the clip's own block, so a Rec.709
    // connection-space override renders the look the user chose,
    // [WP-HDRPEAK] a PQ one the HDR peak the user chose, and [WP-HDRTONE] a
    // PQ or HLG one the transfer function the user chose.
    return color::makeColorParams(toDlogMFit(m_prefs.fit()), static_cast<color::OutputTransfer>(outputTransfer),
                                  m_prefs.exposureStops, inputEncodingFor(m_format.colorMode), true,
                                  video::kDecodedSampleBits, nullptr, color::kBt2408SceneScale,
                                  toLook(m_prefs.lookChoice()), m_prefs.hdrPeakNits(),
                                  toHdrTone(m_prefs.hdrToneChoice()));
}

Result<render::RenderJob> ImporterInstance::buildEquirectJob(std::uint32_t index, const video::FramePair& pair,
                                                             const OutputGeometry& geometry, bool draft,
                                                             RenderPurpose purpose, ThreadPool& pool,
                                                             AnalysisOutcome& outcome, int outputTransfer) {
    // The caller holds m_mutex (applyAnalyses and the caches need it).
    if (!geometry.valid()) {
        return Error{ErrorCode::InvalidArgument, "buildEquirectJob with an empty output geometry"};
    }

    // ---- per-frame analyses (exactly as osvtool's render loop does them) ---
    // The colour block is the clip's own unless this one request overrides
    // the transfer (colorForTransfer); the analyses never depend on it.
    const OsvColorParams frameColor = colorForTransfer(outputTransfer);
    prepareSceneLightLocked(purpose, draft);  // Scene Light: the profile the analyses read
    prepareSteadyLocked(purpose, draft);  // [WP-STEADY] before anything reads m_rig
    render::RenderParamsBuilder builder;
    refreshRenderBlend();  // [WP-PHOTO] the render-only inset blend; analyses keep m_blend
    builder.rig(m_rig).color(frameColor).blend(m_renderBlend, true);

    // Alpha = lens coverage (the builder's default, stated here because it
    // has to agree with the alphaType imGetInfo8 declares).
    //
    // A dual-fisheye stitch is NOT opaque everywhere: the kernel writes fully
    // transparent black wherever neither lens sees a direction, which the
    // calibration's occlusion polygon really does produce near the camera
    // body.  Declaring alphaOpaque and emitting coverage would let Premiere
    // skip the alpha channel and composite garbage into those pixels, and
    // forcing alpha to 1 would instead paint black over whatever the user
    // put underneath.  Straight coverage alpha plus alphaStraight is the
    // truthful pair.
    builder.alphaCoverage(true);

    // ---- per-frame stitch analyses ------------------------------------------
    // Seam table, exposure gains and the 2-D parallax grid, bucketed and
    // cached per instance.  Shared with the direct GPU path, which feeds the
    // same routine device-resident frames, so both paths use - and fill -
    // one set of caches (see applyAnalyses).
    outcome = applyAnalyses(index, pair, draft, purpose, pool, builder);

    builder.stabilization(stabilizationFor(index));

    // ---- a view, when one was asked for (osvtool's reframe) ---------------
    // Straight from the fisheyes, with everything above - analyses,
    // stabilisation, colour - exactly as the equirect gets it.
    if (geometry.view) {
        builder.camera(*geometry.view);
        return builder.build(pair);
    }

    // ---- equirect output ---------------------------------------------------
    // The renderer takes any output size, so a request for 4K or 2K renders
    // directly at that size rather than rendering native and downscaling.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = geometry.width;
    map.h = geometry.height;
    builder.equirect(map);

    return builder.build(pair);
}

// ---------------------------------------------------------------------------
//  [WP-IMPORTER] The importer's own frame, straight into the host's PPix
// ---------------------------------------------------------------------------

Status ImporterInstance::renderFrameToHost(std::uint32_t index, const OutputGeometry& geometry, bool draft,
                                           RenderPurpose purpose, const pixelcopy::HostFrame& dst,
                                           pixelcopy::HostPixelFormat format, int outputTransfer) {
    // The caller holds m_mutex (the header contract).
    if (!m_parsed) {
        return failStatus(ErrorCode::InvalidArgument, "renderFrameToHost before the clip was opened");
    }
    if (!geometry.valid() || dst.width != static_cast<std::uint32_t>(geometry.width) ||
        dst.height != static_cast<std::uint32_t>(geometry.height) || !dst.valid(pixelcopy::bytesPerPixel(format))) {
        return failStatus(ErrorCode::InvalidArgument, "renderFrameToHost: the host frame does not match the geometry");
    }
    if (index >= m_frameCount) {
        return failStatus(ErrorCode::InvalidArgument, "frame index " + std::to_string(index) + " beyond the clip");
    }
    if (outputTransfer > OSV_TRANSFER_PASSTHROUGH) {
        return failStatus(ErrorCode::InvalidArgument,
                          "renderFrameToHost: unknown output transfer " + std::to_string(outputTransfer));
    }

    // ---- the GPU path first -------------------------------------------------
    if (m_gpuFrameState != GpuFrameState::Disabled) {
        auto served = renderFrameOnGpu(index, geometry, draft, purpose, dst, format, outputTransfer);
        if (served.ok() && served.value()) {
            m_gpuFrameFailures = 0;
            if (m_gpuFrameState != GpuFrameState::Active) {
                m_gpuFrameState = GpuFrameState::Active;
            }
            m_lastFramePath.store(static_cast<int>(FramePath::Gpu), std::memory_order_relaxed);
            return okStatus();
        }
        if (!served.ok() && served.error().code == ErrorCode::Timing) {
            // The stream's own timing (see ErrorCode::Timing): the host path
            // gets its turn below, but the GPU path takes no strike for it.
            PluginLog::warn("video: GPU frame path failed on frame {} of '{}' ({}); the stream's timing is at fault, "
                            "not the GPU path",
                            index, m_path.filename().string(), served.error().message);
        } else if (!served.ok()) {
            // A real failure on a clip the path had accepted: a driver
            // hiccup, a lost device, VRAM pressure.  This frame takes the
            // host path; three in a row mean it is not a hiccup.
            ++m_gpuFrameFailures;
            PluginLog::warn("video: GPU frame path failed on frame {} of '{}' ({}); this frame takes the host path{}",
                            index, m_path.filename().string(), served.error().message,
                            m_gpuFrameFailures >= 3 ? " - three in a row, so this clip stays on it" : "");
            if (m_gpuFrameFailures >= 3) {
                m_gpuFrameState = GpuFrameState::Disabled;
                // Its VRAM (NVDEC surfaces + frame cache) is better spent elsewhere now.
                if (m_gpuFrameContext) {
                    m_gpuDecoders.erase(m_gpuFrameContext);
                }
            }
        }
        // served == false: the path does not apply; renderFrameOnGpu already
        // set Disabled with its reason when that is permanent.
    }

    // ---- the host path -------------------------------------------------------
    const auto tHost = std::chrono::steady_clock::now();
    auto rendered = renderFrame(index, geometry, draft, purpose, outputTransfer);
    if (!rendered.ok()) {
        return rendered.error();
    }
    const render::ImageRGBAf* image = rendered.value();
    if (!image || !image->valid() || image->w != dst.width || image->h != dst.height) {
        return failStatus(ErrorCode::Internal, "renderFrameToHost: the rendered image does not match the frame");
    }
    const auto tCopy = std::chrono::steady_clock::now();
    // The pool the renderers use (HostContext's), leased so imShutdown on
    // another thread cannot join it under the copy.
    std::shared_ptr<ThreadPool> pool;
    if (HostContext::exists()) {
        pool = HostContext::instance().threadPoolShared();
    }
    OSV_TRY(pixelcopy::rgbaToHost(*image, dst, format, pool.get()));
    const auto tEnd = std::chrono::steady_clock::now();
    m_lastFramePath.store(static_cast<int>(FramePath::Host), std::memory_order_relaxed);
    // The same "frame-cost" line as the GPU path (renderFrame covers decode,
    // upload, stitch and readback there; copy is the conversion into dst).
    PluginLog::debug("frame-cost path=host frame={} size={}x{} fmt={} total={:.2f} render={:.2f} copy={:.2f}", index,
                     geometry.width, geometry.height, pixelcopy::hostPixelFormatName(format),
                     std::chrono::duration<double, std::milli>(tEnd - tHost).count(),
                     std::chrono::duration<double, std::milli>(tCopy - tHost).count(),
                     std::chrono::duration<double, std::milli>(tEnd - tCopy).count());
    return okStatus();
}

Result<bool> ImporterInstance::renderFrameOnGpu(std::uint32_t index, const OutputGeometry& geometry, bool draft,
                                                RenderPurpose purpose, const pixelcopy::HostFrame& dst,
                                                pixelcopy::HostPixelFormat format, int outputTransfer) {
#if defined(OSV_HAVE_CUDA)
    // The caller holds m_mutex.  Every "return false" below means "this path
    // does not apply" and leaves the frame to the host path; a permanent
    // reason also sets Disabled and is logged once, so it is not re-probed
    // on every frame.
    const auto disable = [this](const std::string& reason) {
        m_gpuFrameState = GpuFrameState::Disabled;
        PluginLog::info("video: '{}' keeps the host frame path ({})", m_path.filename().string(), reason);
        return false;
    };

    // ---- first use: the switch and the cheap refusals ----------------------
    if (m_gpuFrameState == GpuFrameState::Untried) {
        if (importerGpuDecodeDisabledByEnvironment()) {
            return disable("OPENOSV_IMPORTER_NO_GPU_DECODE is set");
        }
        if (m_hwDecodeFailed) {
            return disable("hardware decoding already failed on this clip");
        }
    }

    // ---- the renderer: the GPU path exists only for the CUDA backend -------
    // Asked for on every frame, exactly as renderFrame asks, so a prefs change
    // to CPU or OpenCL takes effect at once (the host path then serves it).
    auto lease = HostContext::instance().acquireRenderer(toDevicePreference(m_prefs.device()));
    if (!lease.ok()) {
        return false;  // renderFrame reports the same failure with its own message
    }
    HostContext::RendererLease renderer = std::move(lease).value();
    if (!renderer.renderer || !renderer.pool || renderer.backend != "cuda") {
        return false;  // CPU / OpenCL chosen or the only thing available: not a permanent refusal
    }
    auto* cuda = dynamic_cast<render::CudaRenderer*>(renderer.renderer.get());
    if (!cuda) {
        return disable("the CUDA backend is not a CudaRenderer");
    }
    m_rendererName = renderer.backend;
    // Device frames need the GPU band shading (and get the GPU flow solver
    // for Auto).  The flow solver is bit-identical to the CPU one; the band
    // shader agrees with the CPU's to 108-111 dB, so with an analysis on this
    // path's stitch matches the effect's direct path (which shades the same
    // way) rather than the host path to the last bit - see
    // test_importer_bitdepth.cpp for the measured difference.
    ensureGpuAnalyses();
    // The same lazily built state renderFrame() makes sure of: a clip whose
    // first request carried no prefs has neither yet.
    if (!m_colorBuilt) {
        rebuildColor();
    }
    if (!m_stabBuilt) {
        rebuildStabilization();
    }

    // ---- the readback (and with it the primary context) ----------------------
    if (!m_gpuReadback || m_gpuReadback->device() != cuda->deviceIndex()) {
        auto readback = GpuReadback::acquire(cuda->deviceIndex());
        if (!readback.ok()) {
            return disable("pinned readback unavailable: " + readback.error().message);
        }
        m_gpuReadback = std::move(readback).value();
    }
    CudaContextScope scope(m_gpuReadback->context());
    if (!scope.ok()) {
        return Error{ErrorCode::Gpu, "cannot make the primary context current"};
    }

    // ---- the decoder, in the renderer's own context ---------------------------
    // Kept in m_gpuDecoders under the primary context, so imQuietFile frees
    // it with the direct path's decoders (releaseHeavy) and it reopens on the
    // next frame after an unquiet.
    auto decoder = m_gpuDecoders.find(m_gpuReadback->context());
    if (decoder == m_gpuDecoders.end() || !decoder->second) {
        video::GpuDecoderOptions options;
        // No context supplied: the decoder retains the device's PRIMARY
        // context itself - the very context the CUDA runtime, and so the
        // renderer, runs in.  Its planes are then readable by the stitch
        // kernel in place (CudaRenderer's zero-copy branch).
        options.cuContext = nullptr;
        options.cudaDevice = cuda->deviceIndex();
        const auto t0 = std::chrono::steady_clock::now();
        // A warm decoder parked by this clip's quiet (or by another instance
        // of the same file) when there is one; a new one otherwise.
        bool warm = false;
        auto opened = takeOrOpenGpuDecoder(options, m_gpuReadback->context(), warm);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!opened.ok()) {
            // Unsupported / InvalidArgument: a stream NVDEC does not take (the
            // LRF proxy, 8-bit), no NVIDIA decoder.  Anything else (Io, Gpu,
            // Decoder) may be transient, but reopening per frame would pay
            // the ~50 ms open each time - one attempt per clip is the rule.
            return disable("NVDEC decoder unavailable: " + opened.error().message);
        }
        if (!opened.value() || opened.value()->cuContext() != m_gpuReadback->context()) {
            return disable("the NVDEC decoder did not open in the renderer's context");
        }
        const video::GpuDecoderStats stats = opened.value()->stats();
        if (warm) {
            PluginLog::info("video: '{}' importer frames decode on NVDEC again (warm decoder from the pool in {:.1f} ms, "
                            "{} frame(s) still cached)",
                            m_path.filename().string(), ms, stats.cachedFrames);
        } else {
            PluginLog::info("video: '{}' importer frames now decode on NVDEC into VRAM and stitch in place (decoder "
                            "opened in {:.0f} ms, frame cache {} x {:.1f} MiB)",
                            m_path.filename().string(), ms, stats.capacitySlots,
                            static_cast<double>(stats.slotBytes) / (1024.0 * 1024.0));
        }
        m_gpuFrameContext = m_gpuReadback->context();
        decoder = m_gpuDecoders.insert_or_assign(m_gpuFrameContext, std::move(opened).value()).first;
    }

    // Stage timings for the "frame-cost" debug line below (the benchmark's
    // part F reads it: the only way to time the importer without the host's
    // own PPix allocation in the number).
    using StageClock = std::chrono::steady_clock;
    const auto stageMs = [](StageClock::time_point a, StageClock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const StageClock::time_point tDecode = StageClock::now();

    // ---- decode: a VRAM cache hit, or NVDEC from the right place in the GOP ---
    OSV_TRY_ASSIGN(video::GpuFrameLease frame, decoder->second->acquire(index));
    if (!frame.valid() || !frame.pair().onDevice()) {
        return Error{ErrorCode::Decoder, "the decoder returned no device frame"};
    }
    if (frame.pair().device[0].deviceIndex != cuda->deviceIndex()) {
        return Error{ErrorCode::Internal, "the decoded frame lives on another device than the renderer"};
    }
    const StageClock::time_point tJob = StageClock::now();

    // ---- the job: identical assembly to the host path --------------------------
    AnalysisOutcome analyses;
    // [WP-TEMPORAL] a bucket's anchor is decoded by this same decoder
    const ScopedPointer<video::GpuClipDecoder> analysisDecoder(m_analysisGpuDecoder, decoder->second.get());
    OSV_TRY_ASSIGN(render::RenderJob job, buildEquirectJob(index, frame.pair(), geometry, draft, purpose,
                                                           *renderer.pool, analyses, outputTransfer));
    m_lastRenderExact = analyses.exact;  // [WP-STEADY] a stand-in stays out of the host's frame cache
    if (!job.planesOnDevice[0] || !job.planesOnDevice[1]) {
        return Error{ErrorCode::Internal, "the stitch job does not reference the device frames"};
    }

    // ---- stitch in place, stream straight into the PPix -------------------------
    {
        std::lock_guard<std::mutex> outputLock(cudaRendererOutputMutex());
        const StageClock::time_point tRender = StageClock::now();
        std::size_t pitch = 0;
        auto device = cuda->renderToDevice(job, &pitch);
        // renderToDevice synchronised its stream: the kernel has finished
        // reading the fisheyes, so the slot may be recycled right away.  The
        // job's copy of the pair shares the pin (its owner fields), so the
        // job is emptied too - releasing only the lease would not unpin.
        job = render::RenderJob{};
        frame.release();
        if (!device.ok()) {
            return device.error();
        }
        const StageClock::time_point tReadback = StageClock::now();
        ReadbackTiming timing;
        OSV_TRY(m_gpuReadback->copyToHost(device.value(), pitch, static_cast<std::uint32_t>(geometry.width),
                                          static_cast<std::uint32_t>(geometry.height), dst, format,
                                          renderer.pool.get(), &timing));
        const StageClock::time_point tEnd = StageClock::now();
        // One line per frame, a stable "frame-cost" prefix and key=value
        // fields: the benchmark parses exactly this.
        PluginLog::debug("frame-cost path=gpu frame={} size={}x{} fmt={} total={:.2f} decode={:.2f} job={:.2f} "
                         "render={:.2f} readback={:.2f} waited={:.2f} converted={:.2f} bands={}",
                         index, geometry.width, geometry.height, pixelcopy::hostPixelFormatName(format),
                         stageMs(tDecode, tEnd), stageMs(tDecode, tJob), stageMs(tJob, tRender),
                         stageMs(tRender, tReadback), stageMs(tReadback, tEnd), timing.waitMs, timing.convertMs,
                         timing.bands);
    }
    return true;
#else
    (void)index;
    (void)geometry;
    (void)draft;
    (void)purpose;
    (void)dst;
    (void)format;
    (void)outputTransfer;
    // A build without the CUDA renderer has nothing to render device frames
    // with; the host path serves every frame.
    m_gpuFrameState = GpuFrameState::Disabled;
    return false;
#endif
}

// ---------------------------------------------------------------------------
//  [WP-V-GPU] The stitched frame, left on the GPU for a caller
// ---------------------------------------------------------------------------

Result<bool> ImporterInstance::renderFrameToDevice(std::uint32_t index, const OutputGeometry& geometry, bool draft,
                                                   RenderPurpose purpose, int outputTransfer,
                                                   const DeviceFrameConsumer& consume, std::string* whyNot,
                                                   const JobRetarget& retarget) {
    // "Not mine": the caller renders the frame its own way; the reason goes
    // to its log.
    const auto notMine = [whyNot](std::string reason) {
        if (whyNot) {
            *whyNot = std::move(reason);
        }
        return false;
    };
#if defined(OSV_HAVE_CUDA)
    // The caller holds m_mutex (the header contract).
    if (!m_parsed) {
        return Error{ErrorCode::InvalidArgument, "renderFrameToDevice before the clip was opened"};
    }
    if (!geometry.valid() || geometry.view) {
        return Error{ErrorCode::InvalidArgument, "renderFrameToDevice needs a non-empty equirect geometry"};
    }
    if (index >= m_frameCount) {
        return Error{ErrorCode::InvalidArgument, "frame index " + std::to_string(index) + " beyond the clip"};
    }
    if (outputTransfer > OSV_TRANSFER_PASSTHROUGH) {
        return Error{ErrorCode::InvalidArgument,
                     "renderFrameToDevice: unknown output transfer " + std::to_string(outputTransfer)};
    }
    if (!consume) {
        return Error{ErrorCode::InvalidArgument, "renderFrameToDevice without a consumer"};
    }

    // ---- the renderer: this path exists only for the CUDA backend -------------
    // Asked for on every frame, exactly as renderFrame() asks, so a Source
    // Settings change to CPU or OpenCL takes effect at once.
    auto lease = HostContext::instance().acquireRenderer(toDevicePreference(m_prefs.device()));
    if (!lease.ok()) {
        return notMine("no renderer for the clip (" + lease.error().message + ")");
    }
    HostContext::RendererLease renderer = std::move(lease).value();
    if (!renderer.renderer || !renderer.pool) {
        return Error{ErrorCode::Internal, "HostContext returned an empty renderer lease"};
    }
    if (renderer.backend != "cuda") {
        return notMine("the clip renders on the " + renderer.backend + " renderer");
    }
    auto* cuda = dynamic_cast<render::CudaRenderer*>(renderer.renderer.get());
    if (!cuda) {
        return notMine("the CUDA backend is not a CudaRenderer");
    }
    m_rendererName = renderer.backend;
    // The same lazily built state renderFrame() makes sure of, and the GPU
    // analyses a CUDA renderer switches on there too.
    ensureGpuAnalyses();
    if (!m_colorBuilt) {
        rebuildColor();
    }
    if (!m_stabBuilt) {
        rebuildStabilization();
    }

    // ---- the renderer's context, current for the whole frame ------------------
    // The renderer's output buffer, the NVDEC frames and the consumer's work
    // all live in the device's PRIMARY context.  Retained for the call (a
    // reference count; the renderer's runtime keeps the context alive
    // anyway) and pushed, so the caller's stack is restored on every return.
    auto primary = retainPrimaryContext(cuda->deviceIndex());
    if (!primary.ok()) {
        return primary.error();
    }
    const std::shared_ptr<void> primaryRef = std::move(primary).value();
    void* const context = primaryRef.get();
    CudaContextScope scope(context);
    if (!scope.ok()) {
        return Error{ErrorCode::Gpu, "cannot make the renderer's primary context current"};
    }

    // Stage timings for the per-frame "frame-cost" debug line at the end -
    // the same prefix and key=value style as renderFrameOnGpu()'s, so one
    // grep collects every path's numbers from a user's log.
    using StageClock = std::chrono::steady_clock;
    const auto stageMs = [](StageClock::time_point a, StageClock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const StageClock::time_point tDecode = StageClock::now();

    // ---- decode: NVDEC into VRAM, the importer frame path's decoder ----------
    // One decoder per clip and context, shared with renderFrameToHost() (the
    // same key, the same state machine): a clip NVDEC cannot serve is marked
    // once and decodes on the host from then on.
    const auto keepHostDecode = [this](const std::string& reason) {
        m_gpuFrameState = GpuFrameState::Disabled;
        PluginLog::info("video: '{}' GPU frames decode on the host and upload ({})", m_path.filename().string(),
                        reason);
    };
    if (m_gpuFrameState == GpuFrameState::Untried) {
        if (importerGpuDecodeDisabledByEnvironment()) {
            keepHostDecode("OPENOSV_IMPORTER_NO_GPU_DECODE is set");
        } else if (m_hwDecodeFailed) {
            keepHostDecode("hardware decoding already failed on this clip");
        }
    }
    video::GpuFrameLease frame;
    bool onGpu = false;
    if (m_gpuFrameState != GpuFrameState::Disabled) {
        auto decoder = m_gpuDecoders.find(context);
        if (decoder == m_gpuDecoders.end() || !decoder->second) {
            video::GpuDecoderOptions options;
            // No context supplied: the decoder retains the device's primary
            // context itself - `context` - so its planes are read by the
            // stitch kernel in place.
            options.cuContext = nullptr;
            options.cudaDevice = cuda->deviceIndex();
            bool warm = false;
            const auto t0 = std::chrono::steady_clock::now();
            auto opened = takeOrOpenGpuDecoder(options, context, warm);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!opened.ok()) {
                // A stream NVDEC does not take (the LRF proxy, 8-bit), no
                // NVIDIA decoder: one attempt per clip, as renderFrameToHost.
                keepHostDecode("NVDEC decoder unavailable: " + opened.error().message);
            } else if (!opened.value() || opened.value()->cuContext() != context) {
                keepHostDecode("the NVDEC decoder did not open in the renderer's context");
            } else {
                PluginLog::info("video: '{}' GPU frames decode on NVDEC into VRAM ({} decoder in {:.0f} ms)",
                                m_path.filename().string(), warm ? "warm" : "new", ms);
                m_gpuFrameContext = context;
                decoder = m_gpuDecoders.insert_or_assign(context, std::move(opened).value()).first;
            }
        }
        if (m_gpuFrameState != GpuFrameState::Disabled && decoder != m_gpuDecoders.end() && decoder->second) {
            auto acquired = decoder->second->acquire(index);
            if (!acquired.ok()) {
                return countDeviceFrameFailure(acquired.error());
            }
            frame = std::move(acquired).value();
            if (!frame.valid() || !frame.pair().onDevice()) {
                return countDeviceFrameFailure(Error{ErrorCode::Decoder, "the decoder returned no device frame"});
            }
            if (frame.pair().device[0].deviceIndex != cuda->deviceIndex()) {
                return Error{ErrorCode::Internal, "the decoded frame lives on another device than the renderer"};
            }
            onGpu = true;
        }
    }

    // ---- or on the host, uploaded by the renderer ---------------------------------
    // A repeat of the frame the previous call decoded is served from
    // m_deviceHostPair.  It is the common case for an .LRF proxy, recorded
    // at half the .OSV's rate: each of its frames is asked for twice in a
    // row, and the host decoder can only answer a step backwards with a seek
    // and a GOP re-decode (the NVDEC path has its own VRAM cache for that).
    bool hostRepeat = false;
    if (!onGpu) {
        if (m_deviceHostPairIndex == index && m_deviceHostPair.lens[0].valid() && m_deviceHostPair.lens[1].valid()) {
            hostRepeat = true;
        } else {
            // Dropped FIRST: never two decoded pairs held at once (two 8K
            // pairs on the host fallback are ~180 MB).
            m_deviceHostPair = video::FramePair{};
            m_deviceHostPairIndex = kNoHostPair;
            const Status readerStatus = ensureReader();
            if (!readerStatus.ok()) {
                return readerStatus.error();
            }
            auto read = readPair(index);
            if (!read.ok()) {
                return read.error();
            }
            m_deviceHostPair = std::move(read).value();
            m_deviceHostPairIndex = index;
        }
    }
    const video::FramePair& pair = onGpu ? frame.pair() : m_deviceHostPair;
    const StageClock::time_point tJob = StageClock::now();

    // ---- the job: renderFrame()'s own assembly -----------------------------------
    AnalysisOutcome analyses;
    // [WP-TEMPORAL] a device frame's bucket anchor is decoded by the decoder it came from
    video::GpuClipDecoder* frameDecoder = nullptr;
    if (onGpu) {
        if (const auto it = m_gpuDecoders.find(context); it != m_gpuDecoders.end()) {
            frameDecoder = it->second.get();
        }
    }
    const ScopedPointer<video::GpuClipDecoder> analysisDecoder(m_analysisGpuDecoder, frameDecoder);
    auto built = buildEquirectJob(index, pair, geometry, draft, purpose, *renderer.pool, analyses, outputTransfer);
    if (!built.ok()) {
        return onGpu ? countDeviceFrameFailure(built.error()) : Result<bool>(built.error());
    }
    render::RenderJob job = std::move(built).value();

    // ---- the caller's output, when it wants another one --------------------------
    // The stitch above is untouched; only the output fields change (the
    // OpenFX generator's camera view straight from the fisheyes).  A refusal
    // is the caller's, not the decoder's: no strike against NVDEC.
    if (retarget) {
        const Status retargeted = retarget(job);
        if (!retargeted.ok()) {
            return retargeted.error();
        }
    }
    // The size the consumer receives: the retargeted block's, or - exactly as
    // before retargeting existed - the geometry's.
    const std::int32_t outW = retarget ? job.params.outW : geometry.width;
    const std::int32_t outH = retarget ? job.params.outH : geometry.height;
    if (outW <= 0 || outH <= 0) {
        return Error{ErrorCode::InvalidArgument, "renderFrameToDevice: the retargeted output is empty"};
    }

    // ---- stitch, and hand the frame over while it is in VRAM ---------------------
    StageClock::time_point tRender;
    StageClock::time_point tConsume;
    StageClock::time_point tEnd;
    {
        // The renderer's output buffer is shared by every clip: it stays ours
        // until the consumer has read it.
        std::lock_guard<std::mutex> outputLock(cudaRendererOutputMutex());
        tRender = StageClock::now();
        std::size_t pitch = 0;
        auto device = cuda->renderToDevice(job, &pitch);
        // renderToDevice synchronised its stream: the lenses have been read.
        // The job's copy of the pair shares the NVDEC slot's pin, so it is
        // emptied too - releasing only the lease would not unpin.  (A host
        // pair stays in m_deviceHostPair for a repeat request.)
        job = render::RenderJob{};
        frame.release();
        if (!device.ok()) {
            return onGpu ? countDeviceFrameFailure(device.error()) : Result<bool>(device.error());
        }
        tConsume = StageClock::now();
        DeviceFrame out;
        out.data = device.value();
        out.pitchBytes = pitch;
        out.width = static_cast<std::uint32_t>(outW);
        out.height = static_cast<std::uint32_t>(outH);
        out.deviceIndex = cuda->deviceIndex();
        out.cuContext = context;
        out.exact = analyses.exact;
        out.decodedOnGpu = onGpu;
        const Status consumed = consume(out);
        if (!consumed.ok()) {
            // The caller's own GPU work failed, not the decode or the stitch:
            // no strike against NVDEC.
            return consumed.error();
        }
        tEnd = StageClock::now();
    }
    if (onGpu) {
        m_gpuFrameFailures = 0;
        m_gpuFrameState = GpuFrameState::Active;
    }
    // One line per frame, renderFrameOnGpu()'s "frame-cost" style: where a
    // user's frame time goes - the decode (and from where), the analyses and
    // job, the stitch, the caller's GPU work - straight from their log.
    PluginLog::debug("frame-cost path=device frame={} size={}x{} source={} view={} exact={} total={:.2f} "
                     "decode={:.2f} job={:.2f} render={:.2f} consume={:.2f}",
                     index, outW, outH, onGpu ? "nvdec" : (hostRepeat ? "host-repeat" : "host"),
                     retarget ? "direct" : "equirect", analyses.exact ? 1 : 0, stageMs(tDecode, tEnd),
                     stageMs(tDecode, tJob), stageMs(tJob, tRender), stageMs(tRender, tConsume),
                     stageMs(tConsume, tEnd));
    return true;
#else
    (void)index;
    (void)geometry;
    (void)draft;
    (void)purpose;
    (void)outputTransfer;
    (void)consume;
    (void)retarget;
    return notMine("this build has no CUDA renderer");
#endif
}

Result<bool> ImporterInstance::countDeviceFrameFailure(const Error& error) {
    // A failure of the NVDEC path on a clip it had accepted: a driver hiccup,
    // a lost device, VRAM pressure.  This frame fails (the caller renders it
    // its own way); three in a row move the clip to host decoding for good,
    // and its VRAM goes back - exactly renderFrameToHost()'s rule.
    //
    // A Timing error is not NVDEC's: the host decoder reads the same stream
    // to the same failure.  The frame still fails, but it is no strike.
    if (error.code == ErrorCode::Timing) {
        PluginLog::warn("video: NVDEC frame failed on '{}' ({}); the stream's timing is at fault, not the decoder",
                        m_path.filename().string(), error.message);
        return error;
    }
    ++m_gpuFrameFailures;
    PluginLog::warn("video: NVDEC frame failed on '{}' ({}){}", m_path.filename().string(), error.message,
                    m_gpuFrameFailures >= 3 ? " - three in a row, so this clip decodes on the host from now on" : "");
    if (m_gpuFrameFailures >= 3) {
        m_gpuFrameState = GpuFrameState::Disabled;
        if (m_gpuFrameContext) {
            m_gpuDecoders.erase(m_gpuFrameContext);
        }
    }
    return error;
}

// ---------------------------------------------------------------------------
//  Analysis text (File > Properties)
// ---------------------------------------------------------------------------

void ImporterInstance::appendCameraSettingsLocked(const std::function<void(const std::string&)>& line) const {
    // What the camera recorded about its own exposure, per frame, in the djmd
    // track (FrameMetaOfCamera).  Nothing here drives the render - the
    // camera's auto-exposure already placed mid-grey where D-Log M expects it
    // - but it is exactly what a user wants to see next to the clip: ISO,
    // shutter, white balance, the metered light value.
    //
    // Three samples (first, middle, last frame) instead of a full scan: the
    // Properties panel is asked for synchronously, the metadata of a long
    // clip runs to a hundred thousand frames, and three samples still show
    // an exposure that the auto-exposure changed during the take as a range.
    if (m_frameCount == 0) {
        return;
    }
    const std::uint32_t last = m_frameCount - 1;
    const std::uint32_t samples[3] = {0, last / 2, last};
    std::vector<meta::CameraFrame> cams;
    cams.reserve(3);
    for (const std::uint32_t index : samples) {
        auto frame = m_track.frame(index);
        if (frame.ok()) {
            cams.push_back(frame.value().camera);
        }
    }
    if (cams.empty()) {
        return;  // metadata unreadable: say nothing rather than something wrong
    }

    // "142" or "142 - 400": a range only when the samples disagree.
    auto range = [&cams](auto pick, const char* format) {
        double lo = pick(cams.front());
        double hi = lo;
        for (const meta::CameraFrame& c : cams) {
            lo = std::min(lo, static_cast<double>(pick(c)));
            hi = std::max(hi, static_cast<double>(pick(c)));
        }
        char a[48] = {};
        char b[48] = {};
        std::snprintf(a, sizeof(a), format, lo);
        if (hi - lo < 1e-6) {
            return std::string(a);
        }
        std::snprintf(b, sizeof(b), format, hi);
        return std::string(a) + " - " + b;
    };

    line("Camera settings (as recorded):");
    // ISO: a positive sensor gain, 0 when the field was absent.
    if (cams.front().iso > 0.0f) {
        line("  ISO " + range([](const meta::CameraFrame& c) { return c.iso; }, "%.0f"));
    }
    // Shutter: a [num, den] rational in seconds, shown the way cameras print
    // it, plus the shutter angle at this clip's frame rate - the number that
    // says how much motion blur is baked in (180 degrees is the film rule).
    const std::vector<std::int32_t>& et = cams.front().exposureTime;
    if (et.size() >= 2 && et[0] > 0 && et[1] > 0) {
        const double seconds = static_cast<double>(et[0]) / static_cast<double>(et[1]);
        char buf[96] = {};
        if (et[0] == 1) {
            std::snprintf(buf, sizeof(buf), "  Shutter 1/%d s", et[1]);
        } else {
            std::snprintf(buf, sizeof(buf), "  Shutter %d/%d s", et[0], et[1]);
        }
        std::string text = buf;
        if (m_rateDen > 0 && m_rateNum > 0) {
            const double angle = 360.0 * seconds * fps();
            if (std::isfinite(angle) && angle > 0.0) {
                std::snprintf(buf, sizeof(buf), " (%.0f deg shutter angle at %.2f fps)", angle, fps());
                text += buf;
            }
        }
        line(text);
    }
    // Aperture: fixed on the Osmo 360 and recorded once per clip, as a
    // rational ([19, 10] = f/1.9).
    const std::vector<std::uint32_t>& fn = m_track.clip().fNumber;
    if (fn.size() >= 2 && fn[1] > 0) {
        char buf[48] = {};
        std::snprintf(buf, sizeof(buf), "  Aperture f/%.1f", static_cast<double>(fn[0]) / static_cast<double>(fn[1]));
        line(buf);
    }
    if (cams.front().wbCct > 0) {
        line("  White balance " +
             range([](const meta::CameraFrame& c) { return static_cast<double>(c.wbCct); }, "%.0f") + " K");
    }
    // The auto-exposure's metered light value: what the camera saw through
    // any ND filter in front of the lens, not the scene's absolute brightness.
    if (cams.front().aecLv > 0.0f) {
        line("  Metered light value LV " + range([](const meta::CameraFrame& c) { return c.aecLv; }, "%.1f"));
    }
    // Scene Light: the profile the seam corrections run with, and what Auto
    // decided it from.
    line("  Scene light: " + sceneLightTextLocked());
    if (cams.front().sensorTemperature != 0.0f) {
        line("  Sensor temperature " +
             range([](const meta::CameraFrame& c) { return c.sensorTemperature; }, "%.0f") + " C");
    }
    // Lens Protection Mode is not repeated here: the calibration block
    // already reports it ("Lens accessory: ...").
}

std::string ImporterInstance::analysisText() const {
    std::lock_guard<std::mutex> guard(m_mutex);

    // CR/LF line endings: the host renders the buffer verbatim in a Windows
    // edit control.
    std::string out;
    auto line = [&out](const std::string& text) {
        out += text;
        out += "\r\n";
    };

    line("OpenOSV importer (DJI Osmo 360)");
    if (!m_format.cameraModel.empty()) {
        line("Camera: " + m_format.cameraModel);
    }
    line(std::string("Recording mode: ") + meta::modeName(m_format.mode) + " (" + std::to_string(m_format.lensW()) +
         " x " + std::to_string(m_format.lensH()) + " per lens)");

    // The *Locked variant: this function already holds m_mutex and the mutex
    // is not recursive.
    const OutputGeometry g = geometryForLocked(m_prefs);
    line("Stitched output: " + std::to_string(g.width) + " x " + std::to_string(g.height) +
         " equirectangular, 360 x 180 degrees");

    // Frame rate as the exact rational plus a rounded decimal, which is what
    // a user recognises.
    if (m_rateDen > 0) {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "%.3f", fps());
        line("Frame rate: " + std::string(buf) + " fps (" + std::to_string(m_rateNum) + " / " +
             std::to_string(m_rateDen) + ")");
    }
    // [VFR] A clip that dropped frames: what the timeline holds, and how many
    // pictures it repeats to keep the sound in step.
    if (m_timeline.identity()) {
        line("Frames: " + std::to_string(m_frameCount));
    } else {
        line("Frames: " + std::to_string(ownTimelineFrameCount()) + " on the timeline (" +
             std::to_string(m_frameCount) + " recorded; the camera dropped frames, the previous picture is held over " +
             std::to_string(m_timeline.heldFrames) + " of them)");
    }

    line(std::string("Source colour mode: ") + meta::colorModeName(m_format.colorMode) +
         (m_format.colorModeFromMetadata ? " (from metadata)" : " (inferred)"));
    appendCameraSettingsLocked(line);
    const char* outName = "BT.2100 PQ";
    switch (m_prefs.color()) {
    case PrefsColorOutput::HLG:    outName = "BT.2100 HLG"; break;
    // [WP-LOOK] Rec.709 names its display look: the two render visibly apart.
    case PrefsColorOutput::Rec709:
        outName = (m_prefs.lookChoice() == PrefsLook::Standard) ? "BT.709 (OpenOSV standard look)"
                                                                : "BT.709 (DJI Studio look)";
        break;
    case PrefsColorOutput::DLogM:  outName = "D-Log M passthrough (camera gamut, no transform)"; break;
    default:                       break;
    }
    // The bit depth follows the per-clip format policy in ImporterVideo.cpp
    // (offeredFormatsFor): HDR never goes below 16 bits, Rec.709 may go to 8
    // when the sequence asks, and the unbounded log signal is float only.
    // Which one a given frame uses is the host's pick from that list.
    const char* depth = "32-bit float, or 16-bit when the sequence's Maximum Bit Depth is off";
    switch (m_prefs.color()) {
    case PrefsColorOutput::Rec709: depth = "32-bit float, or 8-bit when the sequence's Maximum Bit Depth is off"; break;
    case PrefsColorOutput::DLogM:  depth = "32-bit float"; break;
    default:                       break;
    }
    line(std::string("Output colour: ") + outName + ", full-range RGB " + depth);
    // [WP-HDRTONE] The HDR outputs name their transfer function: the styles
    // render visibly apart.  Only a D-Log M clip has one; an HLG or Normal
    // clip's own rendering is kept.
    if (m_prefs.color() == PrefsColorOutput::PQ || m_prefs.color() == PrefsColorOutput::HLG) {
        const color::HdrTone tone = toHdrTone(m_prefs.hdrToneChoice());
        if (inputEncodingFor(m_format.colorMode) == color::InputEncoding::DLogM) {
            line(std::string("Transfer function (HDR): ") + color::hdrToneLabel(tone));
        } else {
            line(std::string("Transfer function (HDR): ") + color::hdrToneLabel(tone) +
                 " (D-Log M only; this clip keeps its own rendering)");
        }
    }
    // [WP-HDRPEAK] Said only when the PQ output does not carry the master's
    // full 1000 nits, with where the roll-off starts - and, for any other
    // output, that the setting is there but not used by it.
    if (m_prefs.hdrPeakChoice() != PrefsHdrPeak::Nits1000) {
        char buf[160] = {};
        const double peak = static_cast<double>(m_prefs.hdrPeakNits());
        if (m_prefs.color() == PrefsColorOutput::PQ) {
            std::snprintf(buf, sizeof(buf), "HDR peak: %.0f nits (highlights above %.0f nits roll off, BT.2408 EETF)",
                          peak, static_cast<double>(color::hdrPeakKneeNits(m_prefs.hdrPeakNits())));
        } else {
            std::snprintf(buf, sizeof(buf), "HDR peak: %.0f nits (PQ output only; this output does not use it)",
                          peak);
        }
        line(buf);
    }
    if (m_prefs.color() == PrefsColorOutput::DLogM) {
        // Said out loud in the Properties panel, because it is the one output
        // whose numbers are NOT ready to look at: the frame is log, so it
        // will look flat and washed out until a D-Log M LUT or a Lumetri
        // log conversion is applied downstream.
        line("  Grade downstream: apply a D-Log M LUT or Lumetri log conversion.");
        line("  Do NOT apply one on top of a PQ / HLG / 709 output - that double-converts.");
    }
    if (m_prefs.exposureStops != 0.0f) {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "%+.2f", static_cast<double>(m_prefs.exposureStops));
        line("Exposure offset: " + std::string(buf) + " stops");
    }

    line("Lens accessory: " + std::string(meta::extriLensModeName(m_format.lensMode)));
    line("Calibration slots: " + m_calibration.sourceSlave + " / " + m_calibration.sourceMaster);

    const char* stabName = "off";
    switch (m_prefs.stab()) {
    case PrefsStabilization::HorizonLock: stabName = "horizon lock"; break;
    case PrefsStabilization::Full:        stabName = "full"; break;
    case PrefsStabilization::Smooth:      stabName = "smooth"; break;
    case PrefsStabilization::SmoothLevel: stabName = "smooth + horizon lock"; break;
    default:                              break;
    }
    line(std::string("Stabilisation: ") + stabName);
    line(std::string("Seam search: ") + (m_prefs.seamSearch ? "on" : "off") + ", exposure match: " +
         (m_prefs.gainMatch ? "on" : "off"));
    line(std::string("Sun ghost removal: ") +  // [WP-FLARE] (FlareStage.h: passthrough is not treated)
         (!m_prefs.flareRemoval                          ? "off"
          : m_color.transfer == OSV_TRANSFER_PASSTHROUGH ? "on, not applied (D-Log M passthrough blends in log code)"
                                                         : "on"));

    if (m_audioChannels > 0) {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "%.0f", m_audioSampleRate);
        line("Audio: AAC, " + std::to_string(m_audioChannels) + " channels, " + std::string(buf) + " Hz");
    } else {
        line("Audio: none");
    }
    if (!m_rendererName.empty()) {
        line("Render backend: " + m_rendererName);
    }
    return out;
}

}  // namespace osv::premiere
