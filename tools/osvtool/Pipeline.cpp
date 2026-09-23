// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "Pipeline.h"

#include "osv/color/AutoDetect.h"
#include "osv/core/Log.h"
#include "osv/geom/ConventionProbe.h"
#include "osv/geom/LensProtector.h"
#include "osv/geom/StreamScaling.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/FormatDetector.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace osvtool {

using namespace osv;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// "xyzw-w2b-y" -> AttitudeConvention.  Returns false for unknown text.
bool parseAttitudeConvention(const std::string& text, geom::AttitudeConvention& out) {
    const std::string t = lower(text);
    // order-sense-up
    if (t.size() < 8) {
        return false;
    }
    geom::AttitudeConvention c;
    if (t.rfind("wxyz", 0) == 0) {
        c.order = geom::QuatOrder::WXYZ;
    } else if (t.rfind("xyzw", 0) == 0) {
        c.order = geom::QuatOrder::XYZW;
    } else {
        return false;
    }
    if (t.find("-w2b-") != std::string::npos) {
        c.sense = geom::AttitudeSense::WorldToBody;
    } else if (t.find("-b2w-") != std::string::npos) {
        c.sense = geom::AttitudeSense::BodyToWorld;
    } else {
        return false;
    }
    // up axis: "-y", "-z", "-ny", "-nz" (negative axes)
    const std::size_t lastDash = t.rfind('-');
    const std::string upText = lastDash == std::string::npos ? "" : t.substr(lastDash + 1);
    if (upText == "y") {
        c.up = geom::WorldUp::Y;
    } else if (upText == "z") {
        c.up = geom::WorldUp::Z;
    } else if (upText == "ny") {
        c.up = geom::WorldUp::NegY;
    } else if (upText == "nz") {
        c.up = geom::WorldUp::NegZ;
    } else {
        return false;
    }
    out = c;
    return true;
}

}  // namespace

bool parseSize(const std::string& text, int& w, int& h) {
    const std::size_t x = text.find_first_of("xX");
    if (x == std::string::npos || x == 0 || x + 1 >= text.size()) {
        return false;
    }
    try {
        const int pw = std::stoi(text.substr(0, x));
        const int ph = std::stoi(text.substr(x + 1));
        if (pw <= 0 || ph <= 0 || pw > 32768 || ph > 32768) {
            return false;
        }
        w = pw;
        h = ph;
        return true;
    } catch (...) {
        return false;
    }
}

void addPipelineOptions(CLI::App* sub, PipelineOptions& opt) {
    if (!sub) {
        return;
    }
    sub->add_option("file", opt.input, "Input .OSV (or .LRF) clip")->required();
    auto* geomGroup = sub->add_option_group("Geometry conventions");
    geomGroup->add_option("--calib", opt.calib, "Calibration set: auto|native|lens-guards|underwater")
        ->default_str("auto");
    geomGroup
        ->add_option("--protector", opt.protector,
                     "Lens-protector field-angle correction: auto|none|forward|inverse (auto = when --calib resolves "
                     "to lens protectors)")
        ->default_str("auto");
    geomGroup->add_option("--stitch-distance", opt.stitchDistanceM, "Use the far_XX preset nearest this distance (m)");
    geomGroup->add_option("--crop-scale", opt.cropScale, "Override the sensor->stream scale (6K verified: 0.794492)");
    geomGroup->add_option("--focal-source", opt.focalSource, "dfl (digital_focal_length) | scaled")->default_str("dfl");
    geomGroup->add_option("--extrinsic-order", opt.extrinsicOrder, "wxyz|xyzw")->default_str("wxyz");
    geomGroup->add_option("--extrinsic-sense", opt.extrinsicSense, "body2lens|lens2body")->default_str("body2lens");
    geomGroup->add_option("--lens-fov", opt.lensFovDeg, "Usable lens FOV in degrees")->default_val(195.18);
    geomGroup->add_option("--feather", opt.featherDeg, "Seam feather width in degrees")->default_val(4.0);
    geomGroup->add_flag("--occlusion,!--no-occlusion", opt.occlusionMask, "Apply the calibration occlusion polygon");
    geomGroup->add_flag("--blend,!--no-blend", opt.blend, "Feather-blend the two lenses (off = nearest lens)");

    auto* stabGroup = sub->add_option_group("Stabilisation");
    stabGroup->add_option("--stab", opt.stab, "off|horizon|full|smooth|smooth-horizon")->default_str("off");
    stabGroup->add_option("--attitude-convention", opt.attitudeConvention,
                          "auto | <order>-<sense>-<up> e.g. xyzw-b2w-ny (default), wxyz-w2b-z; up = y|z|ny|nz")->default_str("auto");
    stabGroup->add_option("--smooth-sigma", opt.smoothSigmaFrames,
                          "Gaussian sigma in frames for --stab smooth and smooth-horizon")->default_val(15.0);

    auto* colorGroup = sub->add_option_group("Colour");
    colorGroup->add_option("--color", opt.color, "pq|hlg|709|linear|dlogm")->default_str("pq");
    colorGroup->add_option("--fit", opt.fit, "D-Log M curve: osmo360|dji|pocket3")->default_str("osmo360");
    colorGroup->add_option("--input-encoding", opt.inputEncoding, "auto|dlogm|hlg|normal")->default_str("auto");
    colorGroup->add_option("--exposure", opt.exposureStops, "Exposure offset in stops")->default_val(0.0);
    colorGroup->add_option("--look", opt.look, "Rec.709 look: dji (DJI Studio, default) | standard")
        ->default_str("dji");
    // [WP-HDRPEAK] The Source Settings "HDR Peak" choices, nothing in between.
    colorGroup->add_option("--hdr-peak", opt.hdrPeak,
                           "PQ output's peak in nits: 1000 (default, no roll-off) | 600 | 400 | 203 (SDR-safe)")
        ->default_str("1000");

    auto* backendGroup = sub->add_option_group("Backends");
#if defined(__APPLE__)
    backendGroup->add_option("--hw", opt.hw, "Decoder acceleration: none|videotoolbox|auto")->default_str("none");
#else
    backendGroup->add_option("--hw", opt.hw, "Decoder acceleration: none|d3d11va|cuda|auto")->default_str("none");
#endif
#if defined(__APPLE__)
    backendGroup->add_option("--device", opt.device, "Renderer: cpu|metal|opencl|auto")->default_str("auto");
#else
    backendGroup->add_option("--device", opt.device, "Renderer: cpu|cuda|opencl|auto")->default_str("auto");
#endif
    backendGroup->add_option("--threads", opt.threads, "CPU threads (0 = all)")->default_val(0);
}

Result<std::unique_ptr<Pipeline>> Pipeline::open(const PipelineOptions& options, bool needRenderer) {
    auto p = std::make_unique<Pipeline>();
    p->options = options;

    // ---- container + metadata -------------------------------------------------
    OSV_TRY_ASSIGN(OsvFile file, OsvFile::open(options.input));
    p->file = std::make_unique<OsvFile>(std::move(file));
    OSV_TRY_ASSIGN(p->track, meta::MetadataTrack::load(*p->file));
    OSV_TRY_ASSIGN(p->format, meta::FormatDetector::detect(*p->file, &p->track));
    for (const std::string& n : p->format.notes) {
        p->notes.push_back("format: " + n);
    }
    if (!p->track.hasCalibration()) {
        return Error{ErrorCode::Malformed, "clip carries no lens calibration (not a dual-fisheye OSV?)"};
    }

    // ---- calibration set --------------------------------------------------------
    // The same selector call the importer's rebuildRig makes, so osvtool and
    // Premiere stitch a clip with the same set for the same choice.
    meta::CalibrationChoice choice = meta::CalibrationChoice::Auto;
    const std::string calib = lower(options.calib);
    if (calib == "native") {
        choice = meta::CalibrationChoice::Native;
    } else if (calib == "lens-guards") {
        choice = meta::CalibrationChoice::LensGuards;
    } else if (calib == "underwater") {
        choice = meta::CalibrationChoice::Underwater;
    } else if (calib != "auto") {
        return Error{ErrorCode::InvalidArgument, "unknown --calib value '" + options.calib + "'"};
    }
    // The protector direction is parsed up front so a typo fails before any
    // decoding starts.
    const std::string protector = lower(options.protector);
    std::optional<geom::ProtectorDirection> forcedProtector;
    if (protector == "none") {
        forcedProtector = geom::ProtectorDirection::None;
    } else if (protector == "forward") {
        forcedProtector = geom::ProtectorDirection::Forward;
    } else if (protector == "inverse") {
        forcedProtector = geom::ProtectorDirection::Inverse;
    } else if (protector != "auto") {
        return Error{ErrorCode::InvalidArgument, "unknown --protector value '" + options.protector + "'"};
    }
    meta::CalibrationSelector::Options selOpt;
    selOpt.stitchDistanceM = options.stitchDistanceM;
    std::vector<std::string> selWarnings;
    OSV_TRY_ASSIGN(meta::CalibrationSelection selection,
                   meta::CalibrationSelector::choose(p->track.stream(), choice, selOpt, &selWarnings));
    p->calibration = selection.set;
    p->notes.push_back("calibration: " + selection.reason);
    for (const std::string& w : selWarnings) {
        p->notes.push_back("calibration: " + w);
    }

    // ---- lens rig ------------------------------------------------------------------
    std::vector<std::string> scaleNotes;
    const double calFxMean = 0.5 * (p->calibration.slave.fx + p->calibration.master.fx);
    OSV_TRY_ASSIGN(geom::StreamScaling scaling,
                   // lensW()/lensH(), not streamW/streamH: the LRF proxy's single
                   // side-by-side track is twice as wide as the lens image the
                   // reader actually delivers, and the rig describes ONE lens.
                   geom::StreamScaling::derive(static_cast<int>(p->format.lensW()), static_cast<int>(p->format.lensH()),
                                               static_cast<int>(p->format.sensorW), static_cast<int>(p->format.sensorH),
                                               p->format.digitalFocalLength, calFxMean, options.cropScale, &scaleNotes));
    for (const std::string& n : scaleNotes) {
        p->notes.push_back("scaling: " + n);
    }
    geom::ExtrinsicConvention conv;
    const std::string order = lower(options.extrinsicOrder);
    if (order == "xyzw") {
        conv.order = geom::QuatOrder::XYZW;
    } else if (order != "wxyz") {
        return Error{ErrorCode::InvalidArgument, "unknown --extrinsic-order '" + options.extrinsicOrder + "'"};
    }
    const std::string sense = lower(options.extrinsicSense);
    if (sense == "lens2body") {
        conv.sense = geom::RotationSense::LensToBody;
    } else if (sense != "body2lens") {
        return Error{ErrorCode::InvalidArgument, "unknown --extrinsic-sense '" + options.extrinsicSense + "'"};
    }
    const std::string focal = lower(options.focalSource);
    geom::FocalSource focalSource = geom::FocalSource::DigitalFocalLength;
    if (focal == "scaled") {
        focalSource = geom::FocalSource::ScaledCalibration;
    } else if (focal != "dfl") {
        return Error{ErrorCode::InvalidArgument, "unknown --focal-source '" + options.focalSource + "'"};
    }
    OSV_TRY_ASSIGN(p->rig, geom::LensRig::build(p->calibration, scaling, focalSource, p->format.digitalFocalLength, conv,
                                                options.lensFovDeg));
    for (const std::string& n : p->rig.notes) {
        p->notes.push_back("rig: " + n);
    }

    // ---- lens protectors: the field-angle correction ------------------------
    // Folded into both lens models exactly as rebuildRig does.  The usable
    // FOV shrinks with it, and the kernel takes its angle limit from the
    // blend parameters, so the blend must follow the fold.
    double lensFovDeg = options.lensFovDeg;
    const geom::ProtectorDirection protectorDirection =
        forcedProtector.value_or(selection.protectorCorrection ? geom::ProtectorDirection::Forward
                                                               : geom::ProtectorDirection::None);
    if (protectorDirection != geom::ProtectorDirection::None) {
        OSV_TRY_ASSIGN(geom::ProtectorRigFold fold,
                       geom::applyLensProtector(p->rig, protectorDirection, options.lensFovDeg));
        lensFovDeg = fold.lensFovDeg;
        char buf[160] = {};
        std::snprintf(buf, sizeof(buf), "lens-protector correction %s: usable FOV %.2f deg, refit residual %.3f px%s",
                      geom::protectorDirectionName(protectorDirection), fold.lensFovDeg, fold.maxResidualPx,
                      forcedProtector ? " (forced by --protector)" : " (unverified: the importer checks frame 0)");
        p->notes.push_back(std::string("calibration: ") + buf);
    }
    p->blendParams.lensFovDeg = lensFovDeg;
    p->blendParams.featherDeg = options.featherDeg;
    p->blendParams.useOcclusionMask = options.occlusionMask;

    // ---- decoder -------------------------------------------------------------------
    video::DecoderOptions decOpt;
    if (const auto hw = video::parseHwAccel(options.hw)) {
        decOpt.hw = *hw;
    } else {
        return Error{ErrorCode::InvalidArgument, "unknown --hw '" + options.hw + "'"};
    }
    decOpt.threads = options.threads;
    // Zero-copy (frames stay on the GPU) only when nothing needs them on the
    // host: the CPU renderer and the band analyses both read host planes.
    decOpt.keepOnDevice =
        (decOpt.hw == video::HwAccel::Cuda) && lower(options.device) != "cpu" && !options.hostFramesRequired;
    OSV_TRY_ASSIGN(video::DualStreamReader reader, video::DualStreamReader::open(options.input, p->format, decOpt));
    p->reader = std::make_unique<video::DualStreamReader>(std::move(reader));

    // ---- colour -----------------------------------------------------------------------
    const std::string enc = lower(options.inputEncoding);
    if (enc == "auto") {
        // The metadata is authoritative for the three modes the pipeline has
        // curves for; anything else needs the statistical second opinion.
        // Both branches go through the same library rule
        // (color::inputEncodingForColorMode) so the CLI and the importer can
        // never disagree about what a clip is.
        switch (p->format.colorMode) {
        case meta::ColorMode::HLG:
        case meta::ColorMode::Normal:
        case meta::ColorMode::DLogM:
            p->inputEncoding = color::inputEncodingForColorMode(p->format.colorMode);
            break;
        default: {
            // No usable metadata: look at the first frame's luma statistics.
            // On a read failure the encoding keeps its default, which the
            // library rule also resolves to D-Log M.
            auto pair = p->reader->read(0);
            if (pair.ok()) {
                const color::AutoDetectResult det = color::detectColorMode(pair.value().lens[0]);
                p->notes.push_back("colour mode auto-detected from frame statistics: " +
                                   std::string(meta::colorModeName(det.guess)) + " (confidence " +
                                   std::to_string(det.confidence) + ")");
                p->inputEncoding = color::inputEncodingForColorMode(det.guess);
            }
            break;
        }
        }
    } else if (!color::parseInputEncoding(enc, p->inputEncoding)) {
        return Error{ErrorCode::InvalidArgument, "unknown --input-encoding '" + options.inputEncoding + "'"};
    }
    if (!color::parseOutputTransfer(lower(options.color), p->outputTransfer)) {
        return Error{ErrorCode::InvalidArgument, "unknown --color '" + options.color + "'"};
    }
    color::DlogMFit fit = color::DlogMFit::DjiRefit;
    if (!color::parseDlogMFit(lower(options.fit), fit)) {
        return Error{ErrorCode::InvalidArgument, "unknown --fit '" + options.fit + "'"};
    }
    // The Rec.709 display look (ignored by every other output): DJI Studio's
    // rendering unless the standard one is asked for.
    color::Look look = color::kDefaultLook;
    if (!color::parseLook(lower(options.look), look)) {
        return Error{ErrorCode::InvalidArgument, "unknown --look '" + options.look + "' (expected dji or standard)"};
    }
    // [WP-HDRPEAK] The PQ output's peak (ignored by every other output): the
    // same four choices as Source Settings, 1000 = no roll-off.
    float hdrPeakNits = color::kDefaultHdrPeakNits;
    if (!color::parseHdrPeak(options.hdrPeak, hdrPeakNits)) {
        return Error{ErrorCode::InvalidArgument,
                     "unknown --hdr-peak '" + options.hdrPeak + "' (expected 1000, 600, 400 or 203)"};
    }
    p->color = color::makeColorParams(fit, p->outputTransfer, static_cast<float>(options.exposureStops),
                                      p->inputEncoding, true, p->format.bitDepth ? p->format.bitDepth : 10, nullptr,
                                      color::kBt2408SceneScale, look, hdrPeakNits);

    // ---- stabilisation ----------------------------------------------------------------
    const std::string stab = lower(options.stab);
    if (stab == "horizon") {
        p->stabParams.mode = geom::StabilizationMode::HorizonLock;
    } else if (stab == "full") {
        p->stabParams.mode = geom::StabilizationMode::Full;
    } else if (stab == "smooth") {
        p->stabParams.mode = geom::StabilizationMode::Smooth;
        p->stabParams.smoothSigmaFrames = options.smoothSigmaFrames;
    } else if (stab == "smooth-horizon") {
        // The smoothed heading with a level horizon (Source Settings'
        // "Smooth + Horizon Lock"), with the same sigma as smooth.
        p->stabParams.mode = geom::StabilizationMode::SmoothLevel;
        p->stabParams.smoothSigmaFrames = options.smoothSigmaFrames;
    } else if (stab != "off") {
        return Error{ErrorCode::InvalidArgument, "unknown --stab '" + options.stab + "'"};
    }
    if (p->stabParams.mode != geom::StabilizationMode::Off) {
        geom::AttitudeTrack::Options attOpt;
        if (lower(options.attitudeConvention) == "auto") {
            // The accelerometer probe only decides when the clip actually
            // carries a gravity-like vector (mean angle well below 15 deg);
            // otherwise the best-supported documented reading is used.
            const geom::ConventionScore best = geom::ConventionProbe::best(p->track);
            if (best.framesUsed > 0 && best.meanGravityAngleDeg < 15.0) {
                attOpt.conv = best.conv;
                p->notes.push_back("attitude convention (auto, probe): " + geom::attitudeConventionName(best.conv) +
                                   ", mean gravity angle " + std::to_string(best.meanGravityAngleDeg) + " deg");
            } else {
                attOpt.conv = geom::AttitudeConvention{};
                p->notes.push_back("attitude convention (auto, default): " +
                                   geom::attitudeConventionName(attOpt.conv) +
                                   " (probe inconclusive, mean gravity angle " +
                                   std::to_string(best.meanGravityAngleDeg) + " deg)");
            }
        } else if (!parseAttitudeConvention(options.attitudeConvention, attOpt.conv)) {
            return Error{ErrorCode::InvalidArgument,
                         "unknown --attitude-convention '" + options.attitudeConvention + "'"};
        }
        OSV_TRY_ASSIGN(geom::AttitudeTrack att, geom::AttitudeTrack::build(p->track, attOpt));
        p->attitude = std::move(att);
        if (p->attitude->sampleCount() == 0) {
            return Error{ErrorCode::Malformed, "clip has no attitude samples; cannot stabilise"};
        }
        p->referenceAttitude = p->attitude->worldFromBody(p->attitude->beginUs());
        // Both smoothing modes read the per-frame smoothed orientation.
        if (geom::stabilizationUsesSmoothing(p->stabParams.mode)) {
            std::vector<Quatd> perFrame;
            perFrame.reserve(p->attitude->samples().size());
            for (const auto& s : p->attitude->samples()) {
                perFrame.push_back(s.worldFromBody);
            }
            p->smoothedAttitude = geom::Smoother(p->stabParams.smoothSigmaFrames).smooth(perFrame);
        }
    }

    // ---- renderer ---------------------------------------------------------------------
    p->pool = std::make_unique<ThreadPool>(options.threads > 0 ? static_cast<unsigned>(options.threads) : 0u);
    if (needRenderer) {
        OSV_TRY_ASSIGN(p->renderer, render::makeRenderer(options.device, *p->pool, &p->rendererName));
        p->notes.push_back("renderer: " + p->rendererName);
    }
    return p;
}

Mat3d Pipeline::stabilizationFor(std::uint32_t frameIndex) const {
    if (!attitude || stabParams.mode == geom::StabilizationMode::Off) {
        return Mat3d::identity();
    }
    // Frame time from the metadata track (falls back to nominal spacing).
    double tUs = attitude->beginUs();
    auto fm = track.frame(frameIndex);
    if (fm.ok()) {
        tUs = static_cast<double>(fm.value().timestampUs);
    } else if (fps() > 0.0) {
        tUs += static_cast<double>(frameIndex) * 1e6 / fps();
    }
    const Quatd wfb = attitude->worldFromBody(tUs);
    std::optional<Quatd> smoothed;
    if (geom::stabilizationUsesSmoothing(stabParams.mode) && frameIndex < smoothedAttitude.size()) {
        smoothed = smoothedAttitude[frameIndex];
    }
    return geom::stabilizationBodyFromWorld(wfb, stabParams, referenceAttitude, attitude->worldUp(), smoothed);
}

std::uint32_t Pipeline::frameCount() const noexcept { return reader ? reader->frameCount() : 0; }

double Pipeline::fps() const noexcept { return reader ? reader->fps() : format.fps; }

std::vector<std::uint32_t> Pipeline::syncFrames() const {
    // The first lens's track (the LRF proxy's single track); both lenses of
    // a camera-written clip share one GOP structure.
    if (!file) {
        return {};
    }
    const std::uint32_t id = format.videoTrackIds[0] != 0 ? format.videoTrackIds[0] : 1u;
    const osv::TrackInfo* video = file->track(id);
    if (!video || !video->samples.hasSyncTable()) {
        return {};
    }
    return video->samples.syncSamples();
}

}  // namespace osvtool
