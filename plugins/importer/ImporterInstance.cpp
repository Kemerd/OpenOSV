// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterInstance implementation.  The call sequence mirrors
// tools/osvtool/Pipeline.cpp exactly, so a Premiere frame and an
// `osvtool render --mode equirect` frame of the same clip at the same
// settings are the same pixels.

#include "ImporterInstance.h"

#include "ImporterAudio.h"

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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

namespace osv::premiere {

namespace {

/// Keep a bounded analysis cache: once it grows past `limit` the lowest
/// frame indices are dropped.  Scrubbing walks forward and backward, so
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

}  // namespace

// ---------------------------------------------------------------------------
//  Construction / destruction
// ---------------------------------------------------------------------------

ImporterInstance::ImporterInstance(std::filesystem::path path) : m_path(std::move(path)) {}

ImporterInstance::~ImporterInstance() {
    // releaseHeavy() closes the OS handle and tears the decoders down in the
    // right order (audio before video, reader before file mapping).
    releaseHeavy();
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
    // Software decode: the importer hands host memory back to Premiere, so a
    // GPU-resident decode would only add a download.  (keepOnDevice is a
    // CUDA-renderer optimisation the osvtool path uses when both the decoder
    // and the renderer sit on the same device; the plug-in's renderer comes
    // from a shared pool whose device is not known here.)
    video::DecoderOptions opt;
    opt.hw = video::HwAccel::None;
    opt.threads = 0;
    opt.keepOnDevice = false;

    auto reader = video::DualStreamReader::open(m_path, m_format, opt);
    if (!reader.ok()) {
        return reader.error();
    }
    m_reader = std::make_unique<video::DualStreamReader>(std::move(reader).value());
    return okStatus();
}

void ImporterInstance::releaseHeavy() noexcept {
    std::lock_guard<std::mutex> guard(m_mutex);

    // Order matters: the audio decoder owns its own AVFormatContext and OS
    // handle, the reader owns two decoders; both must go before the mapping
    // they may reference.
    m_audio.reset();
    m_audioProbed = false;
    m_reader.reset();

    // Drop the frame and analysis caches: they are pure caches, and holding
    // a 6000x3000 float image (288 MB) across a quiet would defeat the point
    // of quieting.
    m_lastFrame = RenderedFrame{};
    m_seamTables.clear();
    m_gains.clear();
    m_parallaxGrids.clear();

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
    m_parallaxGrids.clear();
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

Result<const render::ImageRGBAf*> ImporterInstance::renderFrame(std::uint32_t index, const OutputGeometry& geometry,
                                                                bool draft) {
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
    // that is already falling behind) - because it costs a couple of hundred
    // milliseconds of CPU per frame.  Paused frames and export get it.
    const bool wantParallax = m_prefs.parallaxEnabled() && !draft;

    // Cache hit: the host asked for the same frame twice (it does, once per
    // requested pixel format while scrubbing).
    if (m_lastFrame.matches(index, geometry, m_prefs, wantSeam, wantParallax)) {
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

    // ---- decode ------------------------------------------------------------
    auto pair = m_reader->read(index);
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
    bool parallaxApplied = false;
    if (wantParallax) {
        auto cached = m_parallaxGrids.find(index);
        if (cached == m_parallaxGrids.end()) {
            render::ParallaxWarpParams pw;
            pw.backend = toFlowBackendKind(m_prefs.flow());
            const auto t0 = std::chrono::steady_clock::now();
            auto grid = render::buildParallaxWarp(m_rig, pair.value(), m_blend, pw, nullptr, *renderer.pool);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (grid.ok()) {
                const render::ParallaxWarpGrid& g = grid.value();
                PluginLog::debug("frame {}: parallax {} in {:.0f} ms (flow {:.0f}), consistent {:.0f}%, gated {}/{}, "
                                 "disparity mean {:.2f} / max {:.2f} deg",
                                 index, render::flowBackendName(g.usedBackend), ms, g.flowMs,
                                 100.0 * g.consistentFraction(), g.gatedCells, g.measuredCells,
                                 g.meanAbsCorrectionDeg, g.maxAbsCorrectionDeg);
                cached = m_parallaxGrids.emplace(index, std::move(grid).value()).first;
            } else {
                PluginLog::debug("frame {}: parallax refused after {:.0f} ms ({}); {}", index, ms,
                                 grid.error().message,
                                 wantSeam ? "using the seam table instead" : "rendering without it");
                cached = m_parallaxGrids.emplace(index, std::nullopt).first;
            }
            trimAnalysisCache(m_parallaxGrids, kMaxParallaxCache, index);
        }
        if (cached != m_parallaxGrids.end() && cached->second.has_value()) {
            const render::ParallaxWarpGrid& g = *cached->second;
            builder.warp(g.uv, g.w, g.h, g.latMinRad, g.latMaxRad);
            parallaxApplied = true;
        }
    }

    if (wantSeam && !parallaxApplied) {
        auto cached = m_seamTables.find(index);
        if (cached == m_seamTables.end()) {
            render::SeamSearchParams sp;
            auto profile = render::searchSeam(m_rig, pair.value(), m_blend, sp, *renderer.pool);
            if (profile.ok()) {
                cached = m_seamTables.emplace(index, std::move(profile).value().shiftDeg).first;
                trimAnalysisCache(m_seamTables, kMaxAnalysisCache, index);
            } else {
                PluginLog::debug("frame {}: seam search failed ({}); rendering without a seam table", index,
                                 profile.error().message);
            }
        }
        if (cached != m_seamTables.end()) {
            builder.seam(cached->second);
        }
    }

    if (m_prefs.gainMatch != 0) {
        auto cached = m_gains.find(index);
        if (cached == m_gains.end()) {
            render::BandParams band;
            auto g = render::estimateGain(m_rig, pair.value(), m_blend, band, *renderer.pool);
            if (g.ok()) {
                std::array<Vec3d, 2> gains{g.value().gain[0], g.value().gain[1]};
                cached = m_gains.emplace(index, gains).first;
                trimAnalysisCache(m_gains, kMaxAnalysisCache, index);
            } else {
                PluginLog::debug("frame {}: gain estimation failed ({}); rendering without exposure matching", index,
                                 g.error().message);
            }
        }
        if (cached != m_gains.end()) {
            builder.gain(cached->second[0], cached->second[1]);
        }
    }

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
    auto image = renderer.renderer->render(job.value());
    if (!image.ok()) {
        return image.error();
    }

    m_lastFrame.frameIndex = index;
    m_lastFrame.geometry = geometry;
    m_lastFrame.prefs = m_prefs;
    m_lastFrame.seamApplied = wantSeam;
    m_lastFrame.parallaxWanted = wantParallax;
    m_lastFrame.image = std::move(image).value();
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
