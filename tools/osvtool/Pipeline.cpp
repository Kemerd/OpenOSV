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
#include "osv/render/ClipSteady.h"
#include "osv/render/MountMask.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <string_view>

namespace osvtool {

using namespace osv;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/**
 * @brief Parse an explicit `--attitude-convention` reading.
 *
 * Grammar: `<order>-<sense>-<up>[-rig]`
 *   - order: wxyz | xyzw
 *   - sense: w2b (stored world -> body) | b2w (stored body -> world)
 *   - up:    y | z | ny | nz (ny / nz = the negative axes)
 *   - -rig:  relabel the IMU's axes into the rig's (AttitudeConvention::rigAxes)
 *
 * `xyzw-w2b-z-rig` is the reading `auto` resolves to (0.5.1 onwards);
 * `xyzw-b2w-ny` is the plain reading 0.4.x / 0.5.0 levelled with, kept for
 * comparisons.  Without the suffix every token is a plain reading, never
 * the rig relabelling, so the old tokens still mean exactly what they meant.
 *
 * @param text The option value (case-insensitive).
 * @param out  Receives the reading; untouched on failure.
 * @return false for unknown or malformed text.
 */
bool parseAttitudeConvention(const std::string& text, geom::AttitudeConvention& out) {
    std::string t = lower(text);
    // An optional "-rig" suffix switches the axis relabelling on; it is cut
    // off first so the up-axis parse below sees the plain grammar.
    constexpr std::string_view kRigSuffix = "-rig";
    bool rigAxes = false;
    if (t.size() > kRigSuffix.size() && t.compare(t.size() - kRigSuffix.size(), kRigSuffix.size(), kRigSuffix) == 0) {
        rigAxes = true;
        t.resize(t.size() - kRigSuffix.size());
    }
    // order-sense-up
    if (t.size() < 8) {
        return false;
    }
    geom::AttitudeConvention c;
    c.rigAxes = rigAxes;
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
    geomGroup->add_option("--hide-mount", opt.hideMount,
                          "Source Settings Hide Mount: on (the calibration occlusion polygons) | off (none) | auto "
                          "(kept only where the lenses disagree on the clip's nine sample frames); wins over "
                          "--occlusion");
    geomGroup->add_flag("--blend,!--no-blend", opt.blend, "Feather-blend the two lenses (off = nearest lens)");

    auto* stabGroup = sub->add_option_group("Stabilisation");
    stabGroup->add_option("--stab", opt.stab, "off|horizon|full|smooth|smooth-horizon")->default_str("off");
    stabGroup->add_option("--attitude-convention", opt.attitudeConvention,
                          "auto (= xyzw-w2b-z-rig) | <order>-<sense>-<up>[-rig] e.g. xyzw-b2w-ny (the 0.5.0 "
                          "reading), wxyz-w2b-z; up = y|z|ny|nz; -rig relabels the IMU axes into the rig's")
        ->default_str("auto");
    stabGroup->add_option("--smooth-sigma", opt.smoothSigmaFrames,
                          "Gaussian sigma in frames for --stab smooth and smooth-horizon")->default_val(15.0);

    auto* colorGroup = sub->add_option_group("Colour");
    colorGroup->add_option("--color", opt.color, "pq|hlg|709|linear|dlogm")->default_str("pq");
    // osmo360 matches DJI's own D-Log M LUT, which DJI ships unchanged for the
    // Pocket 3 as well; pocket3 is a legacy community fit (see docs/COLOR.md).
    colorGroup->add_option("--fit", opt.fit,
                           "D-Log M curve: osmo360 (DJI-matched, also for Pocket 3)|avata360|dji|pocket3 (legacy)")
        ->default_str("osmo360");
    colorGroup->add_option("--input-encoding", opt.inputEncoding, "auto|dlogm|hlg|normal")->default_str("auto");
    colorGroup->add_option("--exposure", opt.exposureStops, "Exposure offset in stops")->default_val(0.0);
    colorGroup->add_option("--look", opt.look, "Rec.709 look: dji (DJI Studio, default) | standard")
        ->default_str("dji");
    // [WP-HDRPEAK] The Source Settings "HDR Peak" choices, nothing in between.
    colorGroup->add_option("--hdr-peak", opt.hdrPeak,
                           "PQ output's peak in nits: 1000 (default, no roll-off) | 600 | 400 | 203 (SDR-safe)")
        ->default_str("1000");
    // [WP-HDRTONE] The Source Settings "Transfer Function (HDR)" styles.
    colorGroup->add_option("--tone", opt.tone,
                           "HDR transfer function (D-Log M to pq|hlg): aces-bright (default, outdoor) | aces-detailed "
                           "(indoor) | bt2408-natural | bt2408-punchy | bt2408-neutral")
        ->default_str("aces-bright");

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
    // ---- Hide Mount (Source Settings), parsed before any decoding --------------
    // On / Off are the occlusion switch; Auto keeps the mask on and rebuilds
    // the polygons once the reader is open (the end of this function).
    const std::string hideMount = lower(options.hideMount);
    if (hideMount == "on" || hideMount == "auto") {
        p->blendParams.useOcclusionMask = true;
    } else if (hideMount == "off") {
        p->blendParams.useOcclusionMask = false;
    } else if (!hideMount.empty()) {
        return Error{ErrorCode::InvalidArgument,
                     "unknown --hide-mount '" + options.hideMount + "' (expected on, off or auto)"};
    }

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
    // Hide Mount Auto shades its bands from host planes as well.
    decOpt.keepOnDevice = (decOpt.hw == video::HwAccel::Cuda) && lower(options.device) != "cpu" &&
                          !options.hostFramesRequired && hideMount != "auto";
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
    // [WP-HDRTONE] How D-Log M scene light becomes PQ / HLG display light
    // (ignored by every other input and output): ACES 2 Bright unless another
    // Source Settings style is asked for.
    color::HdrTone tone = color::kDefaultHdrTone;
    if (!color::parseHdrTone(options.tone, tone)) {
        return Error{ErrorCode::InvalidArgument,
                     "unknown --tone '" + options.tone +
                         "' (expected aces-bright, aces-detailed, bt2408-natural, bt2408-punchy or bt2408-neutral)"};
    }
    // The expansion is built for the decoded sample scale (the LRF proxy's
    // 8-bit samples arrive widened to 10 bits), never the stream's coded depth.
    p->color = color::makeColorParams(fit, p->outputTransfer, static_cast<float>(options.exposureStops),
                                      p->inputEncoding, true, video::kDecodedSampleBits, nullptr,
                                      color::kBt2408SceneScale, look, hdrPeakNits, tone);

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
            // The verified reading, levelled on its own +Z, with the
            // accelerometer canary in the note (the same rule the importer's
            // horizon lock uses).
            const geom::AutoConvention detected = geom::ConventionProbe::autoDetect(p->track);
            detected.applyTo(attOpt);
            p->notes.push_back("attitude convention (auto): " + geom::attitudeConventionName(detected.conv) + ", " +
                               detected.reason);
        } else if (!parseAttitudeConvention(options.attitudeConvention, attOpt.conv)) {
            return Error{ErrorCode::InvalidArgument,
                         "unknown --attitude-convention '" + options.attitudeConvention + "'"};
        }
        // The mount's vibration stays out of the counter-rotation, exactly
        // as the plug-ins' engine builds its track (Stabilization.h).
        attOpt.vibrationCutoffHz = geom::kStabilisationVibrationCutoffHz;
        OSV_TRY_ASSIGN(geom::AttitudeTrack att, geom::AttitudeTrack::build(p->track, attOpt));
        p->attitude = std::move(att);
        if (p->attitude->sampleCount() == 0) {
            return Error{ErrorCode::Malformed, "clip has no attitude samples; cannot stabilise"};
        }
        p->referenceAttitude = p->attitude->worldFromBody(p->attitude->beginUs());
        if (p->attitude->vibrationCutoffHz() > 0.0) {
            p->notes.push_back(std::format("stabilisation: {} IMU samples; rotation above {:.0f} Hz (the mount's "
                                           "vibration) is left in place",
                                           p->attitude->sampleCount(), p->attitude->vibrationCutoffHz()));
        }
        // One orientation per video frame for the smoothing window and the
        // mount measurement, whatever the track's own sample density.
        std::vector<Quatd> perFrame = p->attitude->perFrame(p->track);
        // The same mount the importer measures: identity for a lenses-level
        // camera, a quarter turn for a lens-up / lens-down one (Avata 360).
        p->stabParams.mount = geom::levellingMount(perFrame, p->attitude->worldUp());
        if (p->stabParams.mount.distance(Mat3d::identity()) > 0.0) {
            p->notes.push_back("stabilisation: the lens axes are vertical over the clip; levelling takes its heading "
                               "from the body's horizontal axis");
        }
        // Both smoothing modes read the per-frame smoothed orientation.
        if (geom::stabilizationUsesSmoothing(p->stabParams.mode)) {
            p->smoothedAttitude = geom::Smoother(p->stabParams.smoothSigmaFrames).smooth(perFrame);
        }
    }

    // ---- renderer ---------------------------------------------------------------------
    p->pool = std::make_unique<ThreadPool>(options.threads > 0 ? static_cast<unsigned>(options.threads) : 0u);
    if (needRenderer) {
        OSV_TRY_ASSIGN(p->renderer, render::makeRenderer(options.device, *p->pool, &p->rendererName));
        p->notes.push_back("renderer: " + p->rendererName);
    }

    // ---- Hide Mount Auto: the mount mask, exactly as the plug-ins measure it ------
    // The clip correction's nine sample frames, both lenses without the mask,
    // through this calibration rig; then the rig's polygons are rebuilt from
    // the verdict.  A measurement that fails keeps the full polygons (the
    // plug-ins' fallback) and says so.
    if (hideMount == "auto") {
        const std::vector<std::uint32_t> frames =
            render::clipSampleFrames(p->frameCount(), p->syncFrames(), render::kClipSteadySamples);
        video::DualStreamReader* sampleReader = p->reader.get();
        const render::ClipFrameSource source = [sampleReader](std::uint32_t f) -> Result<video::FramePair> {
            if (!sampleReader) {
                return Error{ErrorCode::Internal, "no reader"};
            }
            return sampleReader->read(f);
        };
        const render::MountMaskParams mountParams;
        auto mask = render::measureMountMask(p->rig, p->blendParams, frames, source, mountParams, *p->pool);
        if (!mask.ok()) {
            p->notes.push_back("hide mount: auto could not be measured (" + mask.error().message +
                               "); keeping the full mask");
        } else {
            auto applied = render::applyMountMask(p->rig, mask.value(), p->blendParams, mountParams);
            if (applied.ok()) {
                const render::MountMaskApplied& a = applied.value();
                char buf[160] = {};
                std::snprintf(buf, sizeof(buf),
                              "; lens 0 %s, lens 1 %s; %u / %u clamped columns; %.0f ms (decode %.0f)",
                              a.changed[0] ? "rebuilt" : "unchanged", a.changed[1] ? "rebuilt" : "unchanged",
                              a.clampedColumns[0], a.clampedColumns[1], mask.value().measureMs + mask.value().decodeMs,
                              mask.value().decodeMs);
                p->notes.push_back("hide mount: auto - " + render::describeMountMask(mask.value()) + buf);
            } else {
                p->notes.push_back("hide mount: auto - the verdict could not be applied (" +
                                   applied.error().message + "); keeping the full mask");
            }
            // The per-window numbers behind the verdict, for the record.  At
            // info level: the sub-command runs while the options are still
            // being parsed, before a global --verbose takes effect.
            for (const render::MountWindow& w : mask.value().windows) {
                // The smoothed far-side difference and the kept columns'
                // verdict say which lens, if any, may be clamped there.
                char line[256] = {};
                std::snprintf(line, sizeof(line),
                              "hide mount: window %4u-%4u agreement %+.3f upper %+.3f lower %+.3f diff %+.3f (%s) "
                              "sigma %.4f %.4f frames %u%s%s",
                              w.col0, w.col0 + mountParams.windowCols - 1, w.agreement, w.upperNcc, w.lowerNcc,
                              w.cleanDiff, render::describeMountKeep(w.keep), w.sigma[0], w.sigma[1], w.frames,
                              w.flat ? " flat" : "", w.released ? " released" : "");
                log::info("{}", line);
            }
        }
        // Every command shows it, `seam` included, which prints no notes.
        log::info("{}", p->notes.back());
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
