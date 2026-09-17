// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// `osvtool extract`: pull raw material out of an .OSV / .LRF without
// re-encoding anything.
//
//   osvtool extract <file> --hevc out.hevc [--stream 0|1]
//   osvtool extract <file> --frame N --lens 0|1 --out raw.pgm|raw.ppm
//                          [--hw none|d3d11va|cuda|auto] [--container-samples]
//   osvtool extract <file> --audio out.aac
//   osvtool extract <file> --imu out.csv [--dense]
//
// Exactly one of --hevc / --frame / --audio / --imu is accepted per run.
//
// Frame output format.  The io module (PNG / TIFF / EXR writers) is a later
// layer than this tool, so --frame writes the decoded fisheye as a 16-bit
// binary PNM with maxval 1023 (the samples are 10-bit, or 8-bit widened by
// two bits for the LRF proxy):
//   * .pgm  P5, the luma plane only (what the seam / calibration tooling
//           usually wants)
//   * .ppm  P6, three 16-bit channels holding Y, Cb, Cr with the chroma
//           planes replicated to full resolution.  This is NOT RGB - it is
//           the raw narrow-range YCbCr, kept exactly as decoded so the file
//           can be compared against FFmpeg's own output bit for bit.
// Both formats open in ImageMagick, IrfanView, GIMP and numpy (imageio).

#include "Commands.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/video/Decoder.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"
#include "osv/video/ImuCsv.h"
#include "osv/video/PlanarFrame.h"
#include "osv/video/StreamExtract.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace osvtool {

namespace {

/// Options collected by CLI11 for the extract command.
struct ExtractOptions {
    std::string inputPath;
    std::string hevcPath;      ///< --hevc
    int stream = 0;            ///< --stream (lens / stream id for --hevc)
    int frame = -1;            ///< --frame (-1 = not requested)
    int lens = 0;              ///< --lens
    std::string framePath;     ///< --out
    std::string audioPath;     ///< --audio
    std::string imuPath;       ///< --imu
    bool dense = false;        ///< --dense
    std::string hw = "none";   ///< --hw
    bool containerSamples = false;  ///< --container-samples
};

/// Map a library error onto the documented process exit codes.
int exitCodeFor(const osv::Error& error) {
    switch (error.code) {
    case osv::ErrorCode::InvalidArgument: return kExitUsage;
    case osv::ErrorCode::Io:
    case osv::ErrorCode::NotFound:
    case osv::ErrorCode::Malformed:
    case osv::ErrorCode::Truncated: return kExitInput;
    default: return kExitRuntime;
    }
}

/// Print an error line (7-bit ASCII) and return its exit code.
int fail(const osv::Error& error) {
    std::fprintf(stderr, "error: %s\n", osv::log::safe(error.toString()).c_str());
    return exitCodeFor(error);
}

/// Lower-case file extension including the dot ("" when there is none).
std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// -----------------------------------------------------------------------------
//  Format resolution (which track is which lens)
// -----------------------------------------------------------------------------

/// Detect the clip layout.  The metadata track is used when it loads; a file
/// without a usable djmd track still resolves through the container alone.
osv::Result<osv::meta::FormatInfo> resolveFormat(const std::filesystem::path& path) {
    OSV_TRY_ASSIGN(const osv::OsvFile file, osv::OsvFile::open(path));
    auto meta = osv::meta::MetadataTrack::load(file);
    if (!meta.ok()) {
        osv::log::debug("extract: metadata track unavailable ({}); detecting from the container only",
                        osv::log::safe(meta.error().message));
        return osv::meta::FormatDetector::detect(file, nullptr);
    }
    return osv::meta::FormatDetector::detect(file, &meta.value());
}

/// Container track id of lens / stream `stream` (0 slave, 1 master).
osv::Result<std::uint32_t> trackForStream(const osv::meta::FormatInfo& format, int stream) {
    if (stream < 0 || stream > 1) {
        return osv::Error{osv::ErrorCode::InvalidArgument, "--stream / --lens must be 0 or 1"};
    }
    const std::uint32_t id = format.videoTrackIds[static_cast<std::size_t>(stream)];
    if (id == 0) {
        return osv::Error{osv::ErrorCode::NotFound, "the file has no video track for stream " + std::to_string(stream)};
    }
    return id;
}

// -----------------------------------------------------------------------------
//  --hevc
// -----------------------------------------------------------------------------
int runHevc(const ExtractOptions& opt) {
    auto format = resolveFormat(opt.inputPath);
    if (!format.ok()) {
        return fail(format.error());
    }
    auto trackId = trackForStream(format.value(), opt.stream);
    if (!trackId.ok()) {
        return fail(trackId.error());
    }
    if (format.value().sideBySideProxy && opt.stream != 0) {
        std::printf("note: side-by-side proxy has a single track; --stream %d maps to the same data\n", opt.stream);
    }
    auto stats = osv::video::writeAnnexBStream(opt.inputPath, trackId.value(), opt.hevcPath);
    if (!stats.ok()) {
        return fail(stats.error());
    }
    const osv::video::AnnexBStats& s = stats.value();
    std::printf("wrote %s\n", osv::log::safe(opt.hevcPath).c_str());
    std::printf("  track           : %u (stream %d)\n", s.trackId, opt.stream);
    std::printf("  samples         : %u\n", s.samples);
    std::printf("  parameter sets  : %u NAL units\n", s.parameterSetNals);
    std::printf("  sample NALs     : %llu\n", static_cast<unsigned long long>(s.sampleNals));
    std::printf("  bytes           : %llu\n", static_cast<unsigned long long>(s.bytesWritten));
    return kExitOk;
}

// -----------------------------------------------------------------------------
//  --audio
// -----------------------------------------------------------------------------
int runAudio(const ExtractOptions& opt) {
    auto stats = osv::video::writeAdtsAudio(opt.inputPath, 0, opt.audioPath);
    if (!stats.ok()) {
        return fail(stats.error());
    }
    const osv::video::AdtsStats& s = stats.value();
    std::printf("wrote %s\n", osv::log::safe(opt.audioPath).c_str());
    std::printf("  track           : %u\n", s.trackId);
    std::printf("  frames          : %u\n", s.samples);
    std::printf("  object type     : %u (%s)\n", s.audioObjectType, s.audioObjectType == 2 ? "AAC-LC" : "AAC");
    std::printf("  sample rate     : %u Hz (index %u)\n", s.sampleRate, s.samplingIndex);
    std::printf("  channels        : %u\n", s.channelConfig);
    std::printf("  bytes           : %llu\n", static_cast<unsigned long long>(s.bytesWritten));
    return kExitOk;
}

// -----------------------------------------------------------------------------
//  --imu
// -----------------------------------------------------------------------------
int runImu(const ExtractOptions& opt) {
    const osv::Status st = osv::video::writeImuCsv(std::filesystem::path(opt.inputPath),
                                                   std::filesystem::path(opt.imuPath), opt.dense);
    if (!st.ok()) {
        return fail(st.error());
    }
    std::printf("wrote %s (%s)\n", osv::log::safe(opt.imuPath).c_str(), opt.dense ? "dense IMU rows" : "one row per frame");
    return kExitOk;
}

// -----------------------------------------------------------------------------
//  --frame
// -----------------------------------------------------------------------------

/// Write `frame` as a 16-bit PGM (luma) or PPM (Y, Cb, Cr) with maxval 1023.
osv::Status writePnm(const std::filesystem::path& path, const osv::video::PlanarFrame16& frame) {
    if (!frame.valid()) {
        return osv::failStatus(osv::ErrorCode::InvalidArgument, "frame has no host pixel data");
    }
    const std::string ext = lowerExtension(path);
    const bool colour = (ext == ".ppm");
    if (!colour && ext != ".pgm") {
        return osv::failStatus(osv::ErrorCode::InvalidArgument,
                               "--out must end in .pgm (16-bit luma) or .ppm (16-bit Y/Cb/Cr); PNG/TIFF/EXR "
                               "output is provided by `osvtool render`");
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return osv::failStatus(osv::ErrorCode::Io, "cannot create " + osv::log::safe(path.string()));
    }
    // Header.  maxval 1023 tells readers the samples are 10-bit and, being
    // > 255, that each sample is two bytes big-endian (PNM rule).
    const std::uint32_t maxval = static_cast<std::uint32_t>((1u << frame.bitDepth) - 1u);
    out << (colour ? "P6" : "P5") << "\n" << frame.width << " " << frame.height << "\n" << maxval << "\n";
    if (!out.good()) {
        return osv::failStatus(osv::ErrorCode::Io, "write failed: " + osv::log::safe(path.string()));
    }
    // One row at a time, big-endian.  The accessors already apply bitShift
    // (P010 frames from the hardware paths) and clamp coordinates.
    const std::size_t channels = colour ? 3u : 1u;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(frame.width) * channels * 2u);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        std::uint8_t* dst = row.data();
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::uint16_t luma = frame.luma(x, y);
            *dst++ = static_cast<std::uint8_t>(luma >> 8);
            *dst++ = static_cast<std::uint8_t>(luma & 0xFFu);
            if (colour) {
                // Nearest-neighbour chroma replication (4:2:0 -> 4:4:4).
                const std::uint16_t cb = frame.chroma(1, x / 2, y / 2);
                const std::uint16_t cr = frame.chroma(2, x / 2, y / 2);
                *dst++ = static_cast<std::uint8_t>(cb >> 8);
                *dst++ = static_cast<std::uint8_t>(cb & 0xFFu);
                *dst++ = static_cast<std::uint8_t>(cr >> 8);
                *dst++ = static_cast<std::uint8_t>(cr & 0xFFu);
            }
        }
        out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
        if (!out.good()) {
            return osv::failStatus(osv::ErrorCode::Io, "write failed at row " + std::to_string(y));
        }
    }
    out.flush();
    if (!out.good()) {
        return osv::failStatus(osv::ErrorCode::Io, "flush failed: " + osv::log::safe(path.string()));
    }
    return osv::okStatus();
}

int runFrame(const ExtractOptions& opt) {
    if (opt.frame < 0) {
        std::fprintf(stderr, "error: --frame must be >= 0\n");
        return kExitUsage;
    }
    if (opt.lens < 0 || opt.lens > 1) {
        std::fprintf(stderr, "error: --lens must be 0 or 1\n");
        return kExitUsage;
    }
    if (opt.framePath.empty()) {
        std::fprintf(stderr, "error: --frame needs --out <raw.pgm|raw.ppm>\n");
        return kExitUsage;
    }
    const auto hw = osv::video::parseHwAccel(opt.hw);
    if (!hw) {
        std::fprintf(stderr, "error: unknown --hw '%s' (expected none, d3d11va, cuda or auto)\n",
                     osv::log::safe(opt.hw).c_str());
        return kExitUsage;
    }
    osv::video::DecoderOptions options;
    options.hw = *hw;
    options.useContainerSamples = opt.containerSamples;

    auto format = resolveFormat(opt.inputPath);
    if (!format.ok()) {
        return fail(format.error());
    }
    const std::uint32_t index = static_cast<std::uint32_t>(opt.frame);
    osv::video::PlanarFrame16 frame;
    std::string decoderNote;
    if (format.value().sideBySideProxy) {
        // One track holds both lenses: decode it once and keep the half we
        // were asked for.  The pair's owner keeps the whole frame alive.
        auto reader = osv::video::DualStreamReader::open(opt.inputPath, format.value(), options);
        if (!reader.ok()) {
            return fail(reader.error());
        }
        auto pair = reader.value().read(index);
        if (!pair.ok()) {
            return fail(pair.error());
        }
        frame = pair.value().lens[static_cast<std::size_t>(opt.lens)];
        if (const osv::video::HevcStreamDecoder* d = reader.value().decoder(0)) {
            decoderNote = d->codecName() + " / " + osv::video::hwAccelName(d->activeHw());
        }
    } else {
        // Native layout: only the requested lens track needs decoding.
        auto trackId = trackForStream(format.value(), opt.lens);
        if (!trackId.ok()) {
            return fail(trackId.error());
        }
        auto decoder = osv::video::HevcStreamDecoder::open(opt.inputPath, trackId.value(), options);
        if (!decoder.ok()) {
            return fail(decoder.error());
        }
        auto decoded = decoder.value().decodeFrame(index);
        if (!decoded.ok()) {
            return fail(decoded.error());
        }
        frame = std::move(decoded).value();
        decoderNote = decoder.value().codecName() + " / " + osv::video::hwAccelName(decoder.value().activeHw());
    }
    const osv::Status written = writePnm(std::filesystem::path(opt.framePath), frame);
    if (!written.ok()) {
        return fail(written.error());
    }
    std::printf("wrote %s\n", osv::log::safe(opt.framePath).c_str());
    std::printf("  frame           : %u (lens %d, pts %lld us)\n", frame.frameIndex, opt.lens,
                static_cast<long long>(frame.ptsUs));
    std::printf("  size            : %ux%u, %u-bit samples (maxval %u)\n", frame.width, frame.height, frame.bitDepth,
                (1u << frame.bitDepth) - 1u);
    std::printf("  decoder         : %s\n", decoderNote.c_str());
    std::printf("  layout          : %s\n", lowerExtension(opt.framePath) == ".ppm" ? "Y/Cb/Cr triplets (not RGB)" : "luma only");
    return kExitOk;
}

// -----------------------------------------------------------------------------
//  Dispatch
// -----------------------------------------------------------------------------
int runExtract(const ExtractOptions& opt) {
    if (opt.inputPath.empty()) {
        std::fprintf(stderr, "error: input file is required\n");
        return kExitUsage;
    }
    // Exactly one action per invocation keeps the output unambiguous.
    int actions = 0;
    actions += opt.hevcPath.empty() ? 0 : 1;
    actions += (opt.frame >= 0 || !opt.framePath.empty()) ? 1 : 0;
    actions += opt.audioPath.empty() ? 0 : 1;
    actions += opt.imuPath.empty() ? 0 : 1;
    if (actions != 1) {
        std::fprintf(stderr, "error: choose exactly one of --hevc, --frame, --audio, --imu\n");
        return kExitUsage;
    }
    if (!opt.hevcPath.empty()) {
        return runHevc(opt);
    }
    if (!opt.audioPath.empty()) {
        return runAudio(opt);
    }
    if (!opt.imuPath.empty()) {
        return runImu(opt);
    }
    return runFrame(opt);
}

}  // namespace

void registerExtractCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<ExtractOptions>();
    CLI::App* sub = app.add_subcommand("extract", "Extract raw streams, frames or metadata without re-encoding");
    sub->add_option("file", opt->inputPath, "Input .OSV or .LRF")->required();
    sub->add_option("--hevc", opt->hevcPath, "Write one video track as an Annex-B elementary stream");
    sub->add_option("--stream", opt->stream, "Stream / lens for --hevc: 0 (slave) or 1 (master)")->capture_default_str();
    sub->add_option("--frame", opt->frame, "Decode this frame index (with --lens and --out)");
    sub->add_option("--lens", opt->lens, "Lens for --frame: 0 (slave) or 1 (master)")->capture_default_str();
    sub->add_option("--out", opt->framePath, "Output for --frame: raw.pgm (16-bit luma) or raw.ppm (16-bit Y/Cb/Cr)");
    sub->add_option("--hw", opt->hw, "Decoder for --frame: none, d3d11va, cuda, auto")->capture_default_str();
    sub->add_flag("--container-samples", opt->containerSamples,
                  "Feed samples from the OpenOSV container parser instead of libavformat (--frame)");
    sub->add_option("--audio", opt->audioPath, "Write the AAC track as an ADTS .aac file");
    sub->add_option("--imu", opt->imuPath, "Write per-frame camera / IMU metadata as CSV");
    sub->add_flag("--dense", opt->dense, "With --imu: one row per fused IMU sample instead of per frame");
    // The callback runs during parse; the exit code lands in the context.
    sub->callback([opt, &ctx]() { ctx.exitCode = runExtract(*opt); });
}

}  // namespace osvtool
