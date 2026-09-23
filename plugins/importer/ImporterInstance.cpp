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
// colorSpaceTokenFor(): the per-clip colour log line names the exact token the
// importer will hand Premiere, so the log and imGetIndColorSpace can never
// disagree about what the host was told.
#include "ImporterPlugin.h"
#include "PluginLog.h"

#include "osv/color/AutoDetect.h"
#include "osv/geom/ConventionProbe.h"
#include "osv/geom/EquirectMap.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/video/ReaderPool.h"
#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaAnalysis.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iterator>
#include <mutex>
#include <vector>

namespace osv::premiere {

namespace {

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
    case PrefsDlogmFit::Osmo360:
    case PrefsDlogmFit::Count:
    default:                      break;
    }
    // A blob byte outside the enum lands on the project default rather than on
    // an arbitrary curve.
    return color::kDefaultDlogMFit;
}

/// Map the prefs enum onto the calibration selector's lens-mode override.
/// Native means "use whatever the clip says", so it stays nullopt.
[[nodiscard]] std::optional<meta::ExtriLensMode> toLensModeOverride(PrefsCalibration calib) noexcept {
    switch (calib) {
    case PrefsCalibration::LensGuards: return meta::ExtriLensMode::LensGuards;
    case PrefsCalibration::Underwater: return meta::ExtriLensMode::Underwater;
    case PrefsCalibration::Native:
    case PrefsCalibration::Count:
    default:                           return std::nullopt;
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
            auto grid = render::parallaxFromBands(job.bands, job.params, nullptr, job.bandMs);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (grid.ok()) {
                result = std::make_shared<const render::ParallaxWarpGrid>(std::move(grid).value());
            } else {
                refusal = grid.error().message;
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
                m_parallaxGrids[job.bucket] = result;  // nullptr records a refusal
                trimAnalysisCache(m_parallaxGrids, kMaxParallaxCache, job.bucket);
                stored = true;
            }
        }
        if (result) {
            PluginLog::debug("parallax bucket {} measured in the background in {:.0f} ms (flow {:.0f}){}", job.bucket,
                             ms, result->flowMs, stored ? "" : " - discarded, settings changed");
        } else {
            PluginLog::debug("parallax bucket {} refused in the background after {:.0f} ms ({})", job.bucket, ms,
                             refusal);
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
    std::lock_guard<std::mutex> lock(m_parallaxMutex);
    m_parallaxGrids.clear();
    m_parallaxPending.reset();
    ++m_parallaxGeneration;
}

// ---------------------------------------------------------------------------
//  Opening
// ---------------------------------------------------------------------------

Status ImporterInstance::open() {
    std::lock_guard<std::mutex> guard(m_mutex);

    // The OS handle is what Premiere stores in imFileAccessRec8::fileref and
    // what keeps a second application from deleting the clip under us.  Share
    // read so Media Encoder can open the same file at the same time.
    if (m_fileHandle == INVALID_HANDLE_VALUE) {
        const std::wstring wide = m_path.wstring();
        m_fileHandle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_fileHandle == INVALID_HANDLE_VALUE) {
            const DWORD err = ::GetLastError();
            return Error{ErrorCode::Io, "CreateFileW failed with " + std::to_string(err)};
        }
    }

    // The container work only happens once; after a quiet/unquiet cycle the
    // parsed state is still there and only the decoders have to come back.
    Status st = parseOnce();
    if (!st.ok()) {
        // Close the handle here: the dispatcher returns imBadFile and a
        // lower-priority importer must be able to open the file.
        if (m_fileHandle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(m_fileHandle);
            m_fileHandle = INVALID_HANDLE_VALUE;
        }
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
    // Every DJI clip is constant frame rate; the per-sample delta is the
    // denominator.  Fall back to the whole duration over the sample count so
    // an unusual sample table still yields a sane rational.
    std::uint64_t delta = 0;
    if (m_frameCount > 0 && videoTrack->timescale > 0) {
        delta = videoTrack->samples.sampleDuration(0);
        if (delta == 0 && videoTrack->duration > 0) {
            delta = videoTrack->duration / m_frameCount;
        }
    }
    m_rateDen = static_cast<std::uint32_t>(delta);
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
        PluginLog::info("colour: '{}': source {} ({}) -> input encoding {}, output {} ({})",
                        m_path.filename().string(), meta::colorModeName(m_format.colorMode),
                        m_format.colorModeFromMetadata ? "from metadata" : "inferred from luma statistics",
                        color::inputEncodingName(in), color::outputTransferName(toOutputTransfer(m_prefs.color())),
                        colorSpaceTokenFor(m_prefs));
    }
    return okStatus();
}

Status ImporterInstance::rebuildRig() {
    // The lens-guard / underwater slots change the intrinsics AND the
    // extrinsics, so the whole rig is rebuilt; the selector is cheap (it only
    // walks the protobuf records already parsed into StreamMeta).
    meta::CalibrationSelector::Options selOpt;
    selOpt.lensModeOverride = toLensModeOverride(m_prefs.calib());

    std::vector<std::string> selWarnings;
    auto selected = meta::CalibrationSelector::select(m_track.stream(), selOpt, &selWarnings);
    if (!selected.ok()) {
        return selected.error();
    }
    m_calibration = std::move(selected).value();
    for (const std::string& w : selWarnings) {
        m_notes.push_back("calibration: " + w);
    }

    // Sensor -> stream scaling: the calibration is expressed in 3840x3840
    // sensor pixels, the decoded frames are 3000x3000 (6K mode).  This is the
    // same derivation Pipeline.cpp performs, with the verified defaults.
    //
    // lensW()/lensH() rather than streamW/streamH, because the LRF proxy is a
    // SINGLE 2048x1024 side-by-side track whose two 1024x1024 halves the
    // reader splits apart before we ever see a frame.  The rig describes one
    // lens, so it must be derived from the half, not from the track.
    std::vector<std::string> scaleNotes;
    const double calFxMean = 0.5 * (m_calibration.slave.fx + m_calibration.master.fx);
    auto scaling = geom::StreamScaling::derive(
        static_cast<int>(m_format.lensW()), static_cast<int>(m_format.lensH()), static_cast<int>(m_format.sensorW),
        static_cast<int>(m_format.sensorH), m_format.digitalFocalLength, calFxMean, std::nullopt, &scaleNotes);
    if (!scaling.ok()) {
        return scaling.error();
    }
    for (const std::string& n : scaleNotes) {
        m_notes.push_back("scaling: " + n);
    }

    // The verified conventions (docs, memory: wxyz order, body->lens sense,
    // focal from digital_focal_length, 195.18 deg usable FOV).
    const geom::ExtrinsicConvention conv;  // defaults are the verified values
    auto rig = geom::LensRig::build(m_calibration, scaling.value(), geom::FocalSource::DigitalFocalLength,
                                    m_format.digitalFocalLength, conv, 195.18);
    if (!rig.ok()) {
        return rig.error();
    }
    m_rig = std::move(rig).value();
    for (const std::string& n : m_rig.notes) {
        m_notes.push_back("rig: " + n);
    }

    // Blend defaults match osvtool's (4 degree feather, occlusion polygon on).
    m_blend.lensFovDeg = 195.18;
    m_blend.featherDeg = 4.0;
    m_blend.useOcclusionMask = true;

    m_rigCalibration = m_prefs.calib();
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
    std::vector<video::HwAccel> order;
    if (!m_hwDecodeFailed) {
        order.push_back(video::HwAccel::D3D11VA);
        order.push_back(video::HwAccel::Cuda);
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

    // Order matters: the audio decoder owns its own AVFormatContext and OS
    // handle, the reader owns two decoders; both must go before the mapping
    // they may reference.
    m_audio.reset();
    m_audioProbed = false;
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
    // The direct path's NVDEC decoders and their VRAM frame caches (up to
    // ~1.5 GB each): a quiet is exactly when that memory should go back.
    m_gpuDecoders.clear();

    // Drop the frame and analysis caches: they are pure caches, and holding
    // a 6000x3000 float image (288 MB) across a quiet would defeat the point
    // of quieting.
    m_lastFrame = RenderedFrame{};
    m_seamTables.clear();
    m_gains.clear();
    resetParallaxLocked();

    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_fileHandle);
        m_fileHandle = INVALID_HANDLE_VALUE;
    }
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
    switch (prefs.size()) {
    case PrefsOutputSize::UHD4K:   return OutputGeometry{3840, 1920};
    case PrefsOutputSize::QHD2560: return OutputGeometry{2560, 1280};
    case PrefsOutputSize::HD2K:    return OutputGeometry{1920, 960};
    case PrefsOutputSize::Native:
    case PrefsOutputSize::Count:
    default:                     return nativeGeometryLocked();
    }
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
    if (!bytes || length < PrefsBlob::kSize) {
        if (!m_colorBuilt) {
            rebuildColor();
        }
        return;
    }
    const PrefsBlob incoming = PrefsBlob::fromBytes(bytes, length);
    if (m_colorBuilt && incoming == m_prefs) {
        return;  // Nothing changed: the common case, keep every cache.
    }

    const PrefsBlob previous = m_prefs;
    m_prefs = incoming;

    // The engine renders this file for the direct GPU path with its OWN
    // instance; tell it what the user chose, so both paths agree.  The
    // engine's instance does not publish back (it would only echo).
    if (!m_engineOwned) {
        enginePublishPrefs(m_path, m_prefs);
    }

    // Colour depends on colorOutput, dlogmFit and exposureStops.
    if (!m_colorBuilt || previous.colorOutput != incoming.colorOutput || previous.dlogmFit != incoming.dlogmFit ||
        previous.exposureStops != incoming.exposureStops) {
        rebuildColor();
    }

    // The rig only depends on the calibration slot.
    if (m_parsed && (!m_rigBuilt || m_rigCalibration != incoming.calib())) {
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

AudioDecoder* ImporterInstance::audioLocked() { return audioImpl(); }

void ImporterInstance::rebuildColor() {
    const color::InputEncoding input = inputEncodingFor(m_format.colorMode);
    // The camera always writes narrow-range YCbCr; bit depth comes from the
    // stream (10 for the Osmo 360, 8 for the LRF proxy).
    m_color = color::makeColorParams(toDlogMFit(m_prefs.fit()), toOutputTransfer(m_prefs.color()),
                                     m_prefs.exposureStops, input, true,
                                     m_format.bitDepth ? m_format.bitDepth : 10u);
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

    // Convention detection is the same two-branch rule Pipeline.cpp uses for
    // `--attitude-convention auto`: trust the accelerometer probe only when
    // the clip really carries a gravity-like vector.
    geom::AttitudeTrack::Options attOpt;
    const geom::ConventionScore best = geom::ConventionProbe::best(m_track);
    if (best.framesUsed > 0 && best.meanGravityAngleDeg < 15.0) {
        attOpt.conv = best.conv;
    } else {
        attOpt.conv = geom::AttitudeConvention{};
    }

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

    if (m_stabParams.mode == geom::StabilizationMode::Smooth) {
        std::vector<Quatd> perFrame;
        perFrame.reserve(m_attitude->samples().size());
        for (const auto& s : m_attitude->samples()) {
            perFrame.push_back(s.worldFromBody);
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
    std::optional<Quatd> smoothed;
    if (m_stabParams.mode == geom::StabilizationMode::Smooth && frameIndex < m_smoothedAttitude.size()) {
        smoothed = m_smoothedAttitude[frameIndex];
    }
    return geom::stabilizationBodyFromWorld(wfb, m_stabParams, m_referenceAttitude, m_attitude->worldUp(), smoothed);
}

std::string ImporterInstance::rendererName() const {
    std::lock_guard<std::mutex> guard(m_mutex);
    return m_rendererName;
}

// ---------------------------------------------------------------------------
//  Rendering
// ---------------------------------------------------------------------------

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

    if (wantParallax) {
        render::ParallaxWarpParams pw;
        pw.backend = toFlowBackendKind(m_prefs.flow());

        // What is already known: this bucket, the one before it (for the
        // glide), and whether the worker already has this bucket in hand.
        bool ownMeasured = false;
        std::shared_ptr<const render::ParallaxWarpGrid> own;
        std::shared_ptr<const render::ParallaxWarpGrid> previous;
        bool alreadyQueued = false;
        {
            std::lock_guard<std::mutex> lock(m_parallaxMutex);
            if (const auto it = m_parallaxGrids.find(bucket); it != m_parallaxGrids.end()) {
                ownMeasured = true;
                own = it->second;
            }
            if (bucket > 0) {
                if (const auto it = m_parallaxGrids.find(bucket - 1); it != m_parallaxGrids.end()) {
                    previous = it->second;
                }
            }
            alreadyQueued = (m_parallaxBusyBucket && *m_parallaxBusyBucket == bucket) ||
                            (m_parallaxPending && m_parallaxPending->bucket == bucket);
        }

        // ---- measure this bucket, now or in the background ---------------
        // Cutting the bands is the only step that needs the decoded frame,
        // and it is cheap (68 rows), so it always happens here.  The flow
        // solve - ~95 % of the cost - runs here only for an Exact request.
        if (!ownMeasured && (exactWanted || !alreadyQueued)) {
            const auto tBand = std::chrono::steady_clock::now();
            auto bands = render::measureParallaxBands(m_rig, pair, m_blend, pw, nullptr, pool);
            const double bandMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tBand).count();

            if (!bands.ok()) {
                PluginLog::debug("frame {}: parallax bands failed ({}); rendering without", index,
                                 bands.error().message);
            } else if (exactWanted) {
                const auto t0 = std::chrono::steady_clock::now();
                auto grid = render::parallaxFromBands(bands.value(), pw, &pool, bandMs);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() + bandMs;
                if (grid.ok()) {
                    const render::ParallaxWarpGrid& g = grid.value();
                    PluginLog::debug("frame {} (bucket {}): parallax {} in {:.0f} ms (flow {:.0f}), consistent "
                                     "{:.0f}%, gated {}/{}, disparity mean {:.2f} / max {:.2f} deg",
                                     index, bucket, render::flowBackendName(g.usedBackend), ms, g.flowMs,
                                     100.0 * g.consistentFraction(), g.gatedCells, g.measuredCells,
                                     g.meanAbsCorrectionDeg, g.maxAbsCorrectionDeg);
                    own = std::make_shared<const render::ParallaxWarpGrid>(std::move(grid).value());
                } else {
                    PluginLog::debug("frame {} (bucket {}): parallax refused after {:.0f} ms ({}); {}", index, bucket,
                                     ms, grid.error().message,
                                     wantSeam ? "using the seam table instead" : "rendering without it");
                    own = nullptr;  // a stored nullptr records the refusal
                }
                ownMeasured = true;
                std::lock_guard<std::mutex> lock(m_parallaxMutex);
                m_parallaxGrids[bucket] = own;
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

        // ---- choose what to apply ------------------------------------------
        std::shared_ptr<const render::ParallaxWarpGrid> apply;
        if (ownMeasured) {
            apply = own;
            // Glide from the previous bucket's measurement instead of
            // stepping to this one at the bucket edge.  Only between two
            // ACCEPTED grids: a refusal falls back to the seam table below,
            // and blending a grid toward "nothing" would be neither.
            if (own && previous) {
                auto blended = render::blendParallaxGrids(*previous, *own, render::parallaxCrossfadeWeight(index));
                if (blended.ok()) {
                    apply = std::make_shared<const render::ParallaxWarpGrid>(std::move(blended).value());
                }
            }
        } else {
            // Interactive, own bucket still being measured: borrow the nearest
            // ACCEPTED measurement within kParallaxBorrowBuckets, earlier
            // first (playback runs forward).  This frame is then a stand-in,
            // and an Exact request for it later must re-render.
            frameExact = false;
            std::lock_guard<std::mutex> lock(m_parallaxMutex);
            for (std::uint32_t d = 1; d <= kParallaxBorrowBuckets && !apply; ++d) {
                if (bucket >= d) {
                    if (const auto it = m_parallaxGrids.find(bucket - d); it != m_parallaxGrids.end() && it->second) {
                        apply = it->second;
                        break;
                    }
                }
                if (const auto it = m_parallaxGrids.find(bucket + d); it != m_parallaxGrids.end() && it->second) {
                    apply = it->second;
                }
            }
        }
        if (apply) {
            builder.warp(apply->uv, apply->w, apply->h, apply->latMinRad, apply->latMaxRad);
            parallaxApplied = true;
        }
    }

    if (wantSeam && !parallaxApplied) {
        auto cached = m_seamTables.find(bucket);
        if (cached == m_seamTables.end()) {
            render::SeamSearchParams sp;
            auto profile = render::searchSeam(m_rig, pair, m_blend, sp, pool);
            if (profile.ok()) {
                cached = m_seamTables.emplace(bucket, std::move(profile).value().shiftDeg).first;
                trimAnalysisCache(m_seamTables, kMaxAnalysisCache, bucket);
            } else {
                PluginLog::debug("frame {} (bucket {}): seam search failed ({}); rendering without a seam table", index,
                                 bucket, profile.error().message);
            }
        }
        if (cached != m_seamTables.end()) {
            builder.seam(cached->second);
        }
    }

    if (m_prefs.gainMatch != 0) {
        auto cached = m_gains.find(bucket);
        if (cached == m_gains.end()) {
            render::BandParams band;
            auto g = render::estimateGain(m_rig, pair, m_blend, band, pool);
            if (g.ok()) {
                std::array<Vec3d, 2> gains{g.value().gain[0], g.value().gain[1]};
                cached = m_gains.emplace(bucket, gains).first;
                trimAnalysisCache(m_gains, kMaxAnalysisCache, bucket);
            } else {
                PluginLog::debug("frame {} (bucket {}): gain estimation failed ({}); rendering without exposure "
                                 "matching",
                                 index, bucket, g.error().message);
            }
        }
        if (cached != m_gains.end()) {
            builder.gain(cached->second[0], cached->second[1]);
        }
    }

    return AnalysisOutcome{parallaxApplied, frameExact};
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
        color = color::makeColorParams(toDlogMFit(m_prefs.fit()), static_cast<color::OutputTransfer>(outputTransfer),
                                       m_prefs.exposureStops, inputEncodingFor(m_format.colorMode), true,
                                       m_format.bitDepth ? m_format.bitDepth : 10u);
    }

    // ---- the stitch block ------------------------------------------------------
    // Assembled exactly as renderFrame assembles the equirect's, from the same
    // analysis caches, so a direct view and the importer's equirect of the
    // same frame are stitched identically.
    render::RenderParamsBuilder builder;
    builder.rig(m_rig).color(color).blend(m_blend, true);
    builder.alphaCoverage(true);
    std::shared_ptr<ThreadPool> pool = HostContext::instance().threadPoolShared();
    if (!pool) {
        return Error{ErrorCode::Internal, "directFrame: no thread pool"};
    }
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
                                                                bool draft, RenderPurpose purpose) {
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

    const bool wantSeam = m_prefs.seamSearch != 0 && !draft;
    // The parallax correction runs under exactly the conditions the seam
    // search does - never for a draft request (thumbnails, prefetch, playback
    // that is already falling behind).  Its cost is amortised: one
    // measurement per bucket of frames, off the render thread for an
    // Interactive request (see RenderPurpose and the block below).
    const bool wantParallax = m_prefs.parallaxEnabled() && !draft;
    const bool exactWanted = purpose == RenderPurpose::Exact;

    // Cache hit: the host asked for the same frame twice (it does, once per
    // requested pixel format while scrubbing).  An Exact request is never
    // served a frame an Interactive render built with a stand-in analysis.
    if (m_lastFrame.matches(index, geometry, m_prefs, wantSeam, wantParallax, exactWanted)) {
        return &m_lastFrame.image;
    }

    if (!m_colorBuilt) {
        rebuildColor();
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

    // ---- per-frame analyses (exactly as osvtool's render loop does them) ---
    render::RenderParamsBuilder builder;
    builder.rig(m_rig).color(m_color).blend(m_blend, true);

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
    const AnalysisOutcome analyses = applyAnalyses(index, pair.value(), draft, purpose, *renderer.pool, builder);
    const bool frameExact = analyses.exact;

    builder.stabilization(stabilizationFor(index));

    // ---- equirect output ---------------------------------------------------
    // The renderer takes any output size, so a request for 4K or 2K renders
    // directly at that size rather than rendering native and downscaling.
    geom::EquirectMap map;
    map.layout = geom::EquirectLayout::Standard;
    map.w = geometry.width;
    map.h = geometry.height;
    builder.equirect(map);

    auto job = builder.build(pair.value());
    if (!job.ok()) {
        return job.error();
    }
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
    const Status rendered = renderer.renderer->renderInto(job.value(), m_lastFrame.image);
    if (!rendered.ok()) {
        return rendered.error();
    }

    m_lastFrame.frameIndex = index;
    m_lastFrame.geometry = geometry;
    m_lastFrame.prefs = m_prefs;
    m_lastFrame.seamApplied = wantSeam;
    m_lastFrame.parallaxWanted = wantParallax;
    m_lastFrame.exact = frameExact;
    return &m_lastFrame.image;
}

// ---------------------------------------------------------------------------
//  Analysis text (File > Properties)
// ---------------------------------------------------------------------------

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
    line("Frames: " + std::to_string(m_frameCount));

    line(std::string("Source colour mode: ") + meta::colorModeName(m_format.colorMode) +
         (m_format.colorModeFromMetadata ? " (from metadata)" : " (inferred)"));
    const char* outName = "BT.2100 PQ";
    switch (m_prefs.color()) {
    case PrefsColorOutput::HLG:    outName = "BT.2100 HLG"; break;
    case PrefsColorOutput::Rec709: outName = "BT.709"; break;
    case PrefsColorOutput::DLogM:  outName = "D-Log M passthrough (camera gamut, no transform)"; break;
    default:                       break;
    }
    line(std::string("Output colour: ") + outName + ", full-range RGB 32-bit float");
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
    default:                              break;
    }
    line(std::string("Stabilisation: ") + stabName);
    line(std::string("Seam search: ") + (m_prefs.seamSearch ? "on" : "off") + ", exposure match: " +
         (m_prefs.gainMatch ? "on" : "off"));

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
