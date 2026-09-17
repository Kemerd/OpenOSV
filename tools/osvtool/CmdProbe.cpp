// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// `osvtool probe`: inspect an .OSV / .LRF file - container tracks, detected
// format, clip and stream metadata, the calibration pair that would be used
// for stitching and (optionally) per-frame metadata, the schema-less raw
// protobuf tree and the embedded cover images.
//
//   osvtool probe <file> [--json out.json] [--raw] [--frames all|N|a-b]
//                        [--imu] [--covers dir]
//
// Console output is a compact 7-bit ASCII summary; the JSON document is the
// complete picture and is what the golden tests compare against.

#include "Commands.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/DjmdDecoder.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetaJson.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/ProtoTree.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace osvtool {

namespace {

using nlohmann::json;
using osv::log::safe;

/// Options collected by CLI11 for the probe command.
struct ProbeOptions {
    std::string inputPath;      ///< The .OSV / .LRF to inspect.
    std::string jsonPath;       ///< --json: write the full document here.
    std::string frames = "0";   ///< --frames: "all", "N" or "a-b".
    std::string coversDir;      ///< --covers: directory for the JPEG cover images.
    bool raw = false;           ///< --raw: include schema-less protobuf trees.
    bool imu = false;           ///< --imu: include the full IMU quaternion lists.
};

/// Parse the --frames selector into a sorted list of indices bounded by
/// `frameCount`.  Accepts "all", "N" and "a-b" (inclusive).  Returns false
/// on a malformed selector; an empty string selects nothing.
bool parseFrameSelector(const std::string& text, std::uint32_t frameCount, std::vector<std::uint32_t>& out) {
    out.clear();
    if (text.empty()) {
        return true;
    }
    if (text == "all") {
        out.reserve(frameCount);
        for (std::uint32_t i = 0; i < frameCount; ++i) {
            out.push_back(i);
        }
        return true;
    }
    // Digits and at most one '-' are the only legal characters.
    const std::size_t dash = text.find('-');
    auto parseNumber = [](const std::string& s, std::uint32_t& value) -> bool {
        if (s.empty() || s.size() > 9) {
            return false;
        }
        std::uint64_t v = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') {
                return false;
            }
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        value = static_cast<std::uint32_t>(v);
        return true;
    };
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    if (dash == std::string::npos) {
        if (!parseNumber(text, first)) {
            return false;
        }
        last = first;
    } else {
        if (!parseNumber(text.substr(0, dash), first) || !parseNumber(text.substr(dash + 1), last)) {
            return false;
        }
    }
    if (last < first) {
        std::swap(first, last);
    }
    // Clamp to the track so a generous range never produces errors.
    for (std::uint32_t i = first; i <= last && i < frameCount; ++i) {
        out.push_back(i);
        if (i == UINT32_MAX) {
            break;
        }
    }
    return true;
}

/// Strip the bulky per-sample quaternion arrays from a FrameMeta JSON when
/// the user did not ask for them (count/first/last/anchor4 stay).
void stripImuLists(json& frame) {
    if (!frame.is_object() || !frame.contains("imu") || !frame["imu"].is_object()) {
        return;
    }
    for (const char* key : {"current", "prev", "next"}) {
        json& batch = frame["imu"][key];
        if (batch.is_object()) {
            batch.erase("q");
        }
    }
}

/// Human readable track summary line for the console.
std::string describeTrack(const osv::TrackInfo& t) {
    std::string line = "  track " + std::to_string(t.trackId) + ": " + osv::trackKindName(t.kind) + " '" +
                       t.sampleEntry.str() + "'";
    if (t.kind == osv::TrackKind::Video) {
        line += " " + std::to_string(t.codedWidth()) + "x" + std::to_string(t.codedHeight());
    } else if (t.kind == osv::TrackKind::Audio && t.audio) {
        line += " " + std::to_string(t.audio->channelCount) + "ch " + std::to_string(static_cast<long long>(t.audio->sampleRate)) + "Hz";
    }
    line += ", " + std::to_string(t.samples.count()) + " samples";
    const double fps = t.frameRate();
    if (t.kind == osv::TrackKind::Video && fps > 0.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), ", %.3f fps", fps);
        line += buf;
    }
    char flags[32];
    std::snprintf(flags, sizeof(flags), ", tkhd flags 0x%x", static_cast<unsigned>(t.tkhdFlags));
    line += flags;
    return line;
}

/// JSON row for one container track.
json trackJson(const osv::TrackInfo& t) {
    json j;
    j["id"] = t.trackId;
    j["kind"] = osv::trackKindName(t.kind);
    j["handler"] = t.handler.str();
    j["handlerName"] = safe(t.handlerName);
    j["entry"] = t.sampleEntry.str();
    j["tkhdFlags"] = t.tkhdFlags;
    j["enabled"] = t.enabled();
    j["codedWidth"] = t.codedWidth();
    j["codedHeight"] = t.codedHeight();
    j["width"] = t.width;
    j["height"] = t.height;
    j["timescale"] = t.timescale;
    j["duration"] = t.duration;
    j["language"] = safe(t.language);
    j["sampleCount"] = t.samples.count();
    j["fps"] = t.frameRate();
    j["firstSampleSize"] = t.samples.count() > 0 ? t.samples.sampleSize(0) : 0u;
    j["maxSampleSize"] = t.samples.maxSampleSize();
    j["totalSize"] = t.samples.totalSize();
    // Sync samples as 1-based numbers (ISO convention, matches stss).
    json sync = json::array();
    for (const std::uint32_t s : t.samples.syncSamples()) {
        sync.push_back(s + 1);
    }
    j["syncSamples"] = sync;
    if (const osv::HevcConfig* hevc = t.hevc()) {
        j["bitDepthLuma"] = hevc->bitDepthLuma();
        j["bitDepthChroma"] = hevc->bitDepthChroma();
    }
    if (const osv::ColrNclx* colr = t.colr()) {
        j["colr"] = json{{"type", colr->colourType.str()},
                         {"primaries", colr->primaries},
                         {"transfer", colr->transfer},
                         {"matrix", colr->matrix},
                         {"fullRange", colr->fullRange}};
    }
    if (t.audio) {
        j["audio"] = json{{"channels", t.audio->channelCount},
                          {"sampleRate", t.audio->sampleRate},
                          {"sampleSize", t.audio->sampleSize}};
    }
    return j;
}

/// Write one cover image to `dir/<stem>_<name>.jpg`; returns false on failure.
bool writeCover(const std::filesystem::path& dir, const std::string& stem, const char* name, osv::ByteSpan bytes) {
    if (bytes.empty()) {
        return true;  // Nothing to write is not a failure.
    }
    const std::filesystem::path path = dir / (stem + "_" + name + ".jpg");
    std::ofstream out(path, std::ios::binary);
    if (!out.good()) {
        std::fprintf(stderr, "error: cannot create %s\n", safe(path.string()).c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        std::fprintf(stderr, "error: write failed for %s\n", safe(path.string()).c_str());
        return false;
    }
    std::printf("  wrote %s (%llu bytes)\n", safe(path.string()).c_str(), static_cast<unsigned long long>(bytes.size()));
    return true;
}

/// Run the command; returns one of the kExit* codes.
int runProbe(const ProbeOptions& opt) {
    using namespace osv;
    using namespace osv::meta;

    if (opt.inputPath.empty()) {
        std::fprintf(stderr, "error: input file is required\n");
        return kExitUsage;
    }

    // ---- open ------------------------------------------------------------------
    const std::filesystem::path inputPath(opt.inputPath);
    Result<OsvFile> opened = OsvFile::open(inputPath);
    if (!opened.ok()) {
        std::fprintf(stderr, "error: cannot open %s: %s\n", safe(opt.inputPath).c_str(),
                     safe(opened.error().toString()).c_str());
        return kExitInput;
    }
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();
    if (movie.tracks.empty()) {
        std::fprintf(stderr, "error: %s has no tracks\n", safe(opt.inputPath).c_str());
        return kExitInput;
    }

    json doc;
    doc["file"] = safe(inputPath.filename().string());
    doc["path"] = safe(inputPath.string());
    doc["size"] = file.size();
    json warnings = json::array();
    for (const std::string& w : movie.warnings) {
        warnings.push_back(safe(w));
    }

    // ---- container -----------------------------------------------------------------
    std::printf("file: %s (%llu bytes)\n", safe(opt.inputPath).c_str(), static_cast<unsigned long long>(file.size()));
    std::printf("container: %u tracks, timescale %u, duration %.3f s\n", static_cast<unsigned>(movie.tracks.size()),
                movie.timescale(), movie.durationSeconds());
    json container;
    if (movie.hasFileType) {
        json brands = json::array();
        for (const Fourcc& b : movie.fileType.compatibleBrands) {
            brands.push_back(b.str());
        }
        container["majorBrand"] = movie.fileType.majorBrand.str();
        container["compatibleBrands"] = brands;
    }
    container["timescale"] = movie.timescale();
    container["duration"] = movie.duration();
    container["durationSeconds"] = movie.durationSeconds();
    json topLevel = json::array();
    for (const BoxHeader& b : movie.topLevel) {
        topLevel.push_back(json{{"type", b.type.str()}, {"offset", b.offset}, {"size", b.size}});
    }
    container["boxes"] = topLevel;
    json tracks = json::array();
    for (const TrackInfo& t : movie.tracks) {
        std::printf("%s\n", describeTrack(t).c_str());
        tracks.push_back(trackJson(t));
    }
    container["tracks"] = tracks;

    // Index table entries with their verification status.
    json indexTable = json::array();
    for (const IndexEntry& e : movie.indexTable.entries) {
        json row;
        row["tag"] = e.tag.str();
        row["offset"] = e.offset;
        row["size"] = e.size;
        row["verified"] = e.verified;
        row["dataOffset"] = e.dataOffset;
        if (!e.note.empty()) {
            row["note"] = safe(e.note);
        }
        indexTable.push_back(row);
    }
    container["indexTable"] = indexTable;
    std::printf("index table: %s, %u entries (%u verified)\n", movie.indexTable.present ? "present" : "absent",
                static_cast<unsigned>(movie.indexTable.entries.size()),
                static_cast<unsigned>(movie.indexTable.verifiedCount()));

    // Nested camd movie: track list and sample counts.
    json camd;
    camd["present"] = file.hasCamd();
    if (file.hasCamd()) {
        Result<MovieInfo> nested = file.camdMovie();
        if (nested.ok()) {
            json camdTracks = json::array();
            json samples = json::array();
            for (const TrackInfo& t : nested.value().tracks) {
                camdTracks.push_back(trackJson(t));
                samples.push_back(t.samples.count());
            }
            camd["tracks"] = camdTracks;
            camd["samples"] = samples;
            camd["size"] = movie.camdBox ? movie.camdBox->size : 0;
            for (const std::string& w : nested.value().warnings) {
                warnings.push_back("camd: " + safe(w));
            }
            std::printf("camd: nested movie with %u tracks\n", static_cast<unsigned>(nested.value().tracks.size()));
        } else {
            camd["error"] = safe(nested.error().toString());
            warnings.push_back("camd: " + safe(nested.error().toString()));
            std::printf("camd: present but unparsable (%s)\n", safe(nested.error().toString()).c_str());
        }
    } else {
        std::printf("camd: absent\n");
    }
    container["camd"] = camd;

    // udta essentials.
    json udta;
    udta["tool"] = safe(movie.udta.tool);
    udta["fsid"] = safe(movie.udta.fsid);
    udta["btec"] = safe(movie.udta.btec);
    udta["covr"] = movie.covers.covr.size();
    udta["snal"] = movie.covers.snal.size();
    udta["tnal"] = movie.covers.tnal.size();
    container["udta"] = udta;
    doc["container"] = container;

    // ---- metadata track --------------------------------------------------------------
    std::unique_ptr<MetadataTrack> meta;
    {
        Result<MetadataTrack> loaded = MetadataTrack::load(file);
        if (loaded.ok()) {
            meta = std::make_unique<MetadataTrack>(std::move(loaded).value());
            for (const std::string& w : meta->warnings()) {
                warnings.push_back("meta: " + safe(w));
            }
        } else {
            warnings.push_back("meta: " + safe(loaded.error().toString()));
            std::printf("metadata: none (%s)\n", safe(loaded.error().toString()).c_str());
        }
    }

    // ---- format ------------------------------------------------------------------------
    Result<FormatInfo> format = FormatDetector::detect(file, meta.get());
    if (format.ok()) {
        const FormatInfo& f = format.value();
        doc["format"] = toJson(f);
        std::printf("format: %s, mode %s, %ux%u @ %.3f fps, %u-bit, colour %s%s, lens %s, tracks slave=%u master=%u%s\n",
                    safe(f.cameraModel).c_str(), modeName(f.mode), f.streamW, f.streamH, f.fps, f.bitDepth,
                    colorModeName(f.colorMode), f.colorModeFromMetadata ? " (metadata)" : " (unknown)",
                    extriLensModeName(f.lensMode), f.videoTrackIds[0], f.videoTrackIds[1],
                    f.sideBySideProxy ? " (side-by-side proxy)" : "");
        for (const std::string& n : f.notes) {
            std::printf("  note: %s\n", safe(n).c_str());
        }
    } else {
        doc["format"] = nullptr;
        warnings.push_back("format: " + safe(format.error().toString()));
        std::printf("format: undetermined (%s)\n", safe(format.error().toString()).c_str());
    }

    // ---- clip / stream / calibration -------------------------------------------
    doc["clip"] = nullptr;
    doc["stream"] = nullptr;
    doc["calibration"] = nullptr;
    doc["metaTrackId"] = meta ? meta->trackId() : 0u;
    doc["frameCount"] = meta ? meta->frameCount() : 0u;
    if (meta) {
        std::printf("metadata: djmd track %u, %u frames\n", meta->trackId(), meta->frameCount());
        if (meta->hasClip()) {
            const ClipMeta& c = meta->clip();
            doc["clip"] = toJson(c);
            std::printf("clip: %s sn=%s fw=%s proto=%s/%s lib=%s\n", safe(c.header.productName).c_str(),
                        safe(c.header.serialNumber).c_str(), safe(c.header.firmware).c_str(),
                        safe(c.header.protoFileName).c_str(), safe(c.header.productProtoVersion).c_str(),
                        safe(c.header.libVersion).c_str());
            std::printf("  sensor %ux%u, digital focal length %.4f px, sensor fps %.4f, imu %u Hz, eis %s, clip ts %llu us\n",
                        c.sensorW, c.sensorH, static_cast<double>(c.digitalFocalLength),
                        static_cast<double>(c.sensorFps), c.imuSamplingRate, eisStatusName(c.eisStatus),
                        static_cast<unsigned long long>(c.header.clipTimestampUs));
        }
        if (meta->hasStream()) {
            const StreamMeta& s = meta->stream();
            doc["stream"] = toJson(s);
            std::printf("stream: id %u \"%s\" %ux%u @ %.3f fps, %u-bit%s, colour %s (%d), fov %d, lens mode %s, shading %u\n",
                        s.id, safe(s.name).c_str(), s.video.width, s.video.height, static_cast<double>(s.video.fps),
                        s.video.bitDepth, s.video.bitDepthValid ? "" : " (unverified)", colorModeName(s.colorMode),
                        static_cast<int>(s.colorMode), s.fovType, extriLensModeName(s.extriLensMode),
                        s.shadingCalibModeNum);
            // Populated calibration slots.
            std::string slots;
            for (std::uint32_t slot = 1; slot < PanoDewarpParams::SlotCount; ++slot) {
                if (s.dewarp.get(slot)) {
                    slots += slots.empty() ? "" : ", ";
                    slots += PanoDewarpParams::fieldName(slot);
                }
            }
            std::printf("  calibration slots: %s\n", slots.empty() ? "(none)" : slots.c_str());

            std::vector<std::string> calWarnings;
            Result<CalibrationSet> selected = CalibrationSelector::select(s, {}, &calWarnings);
            json calibration;
            json calWarnJson = json::array();
            for (const std::string& w : calWarnings) {
                calWarnJson.push_back(safe(w));
            }
            calibration["warnings"] = calWarnJson;
            if (selected.ok()) {
                const CalibrationSet& set = selected.value();
                calibration["selected"] = json{{"slave", set.sourceSlave}, {"master", set.sourceMaster}};
                calibration["slave"] = toJson(set.slave);
                calibration["master"] = toJson(set.master);
                std::printf("selected calibration: %s / %s\n", set.sourceSlave.c_str(), set.sourceMaster.c_str());
                for (const DewarpParams* d : {&set.slave, &set.master}) {
                    std::printf("  %s: f=(%.4f, %.4f) c=(%.4f, %.4f) k=[%.7f %.7f %.7f %.7f %.7f] %ux%u q=(%.7f, %.7f, %.7f, %.7f) occl=%u pts\n",
                                d == &set.slave ? "slave " : "master", static_cast<double>(d->fx),
                                static_cast<double>(d->fy), static_cast<double>(d->cx), static_cast<double>(d->cy),
                                static_cast<double>(d->k[0]), static_cast<double>(d->k[1]),
                                static_cast<double>(d->k[2]), static_cast<double>(d->k[3]),
                                static_cast<double>(d->k[4]), d->width, d->height,
                                static_cast<double>(d->camExtriQ.w), static_cast<double>(d->camExtriQ.x),
                                static_cast<double>(d->camExtriQ.y), static_cast<double>(d->camExtriQ.z),
                                static_cast<unsigned>(d->occlusionPtX.size()));
                }
            } else {
                calibration["selected"] = nullptr;
                calibration["error"] = safe(selected.error().toString());
                std::printf("selected calibration: none (%s)\n", safe(selected.error().toString()).c_str());
            }
            for (const std::string& w : calWarnings) {
                std::printf("  note: %s\n", safe(w).c_str());
            }
            doc["calibration"] = calibration;
        }

        // Lens order explanation.
        Result<MetadataTrack::LensStreamOrder> order = meta->lensStreamOrder();
        if (order.ok()) {
            doc["lensOrder"] = json{{"slaveTrackId", order.value().slaveTrackId},
                                    {"masterTrackId", order.value().masterTrackId},
                                    {"sideBySide", order.value().sideBySide},
                                    {"fromFallback", order.value().fromFallback},
                                    {"source", safe(order.value().source)}};
        }
    }

    // ---- frames --------------------------------------------------------------------------
    json frames = json::array();
    if (meta) {
        std::vector<std::uint32_t> indices;
        if (!parseFrameSelector(opt.frames, meta->frameCount(), indices)) {
            std::fprintf(stderr, "error: --frames expects all, N or a-b (got '%s')\n", safe(opt.frames).c_str());
            return kExitUsage;
        }
        std::uint32_t failed = 0;
        for (const std::uint32_t index : indices) {
            Result<FrameMeta> frame = meta->frame(index);
            if (!frame.ok()) {
                ++failed;
                warnings.push_back("frame " + std::to_string(index) + ": " + safe(frame.error().toString()));
                continue;
            }
            json fj = toJson(frame.value());
            fj["index"] = index;
            if (!opt.imu) {
                stripImuLists(fj);
            }
            if (opt.raw) {
                Result<ByteSpan> sample = file.sample(meta->trackId(), index);
                if (sample.ok()) {
                    fj["raw"] = toJson(decodeTree(sample.value()));
                }
            }
            frames.push_back(std::move(fj));
        }
        if (!indices.empty()) {
            std::printf("frames: %u selected (%u decoded, %u failed)\n", static_cast<unsigned>(indices.size()),
                        static_cast<unsigned>(indices.size() - failed), failed);
        }
        // First frame headline so the console shows something useful.
        if (!frames.empty()) {
            const json& f0 = frames.front();
            const json& cam = f0.contains("camera") ? f0["camera"] : json(nullptr);
            std::printf("  frame %u: seq %llu, ts %llu us, iso %.0f, wb %u K",
                        f0.value("index", 0u), static_cast<unsigned long long>(f0.value("seq", 0ull)),
                        static_cast<unsigned long long>(f0.value("timestampUs", 0ull)),
                        cam.is_object() ? cam.value("iso", 0.0) : 0.0, cam.is_object() ? cam.value("wbCct", 0u) : 0u);
            if (f0.contains("imu") && f0["imu"].is_object() && f0["imu"]["current"].is_object()) {
                std::printf(", imu batch %u samples", f0["imu"]["current"].value("count", 0u));
            }
            std::printf("\n");
        }
    }
    doc["frames"] = frames;

    // ---- raw tree of sample 0 ---------------------------------------------------------
    if (opt.raw && meta) {
        Result<ByteSpan> sample0 = file.sample(meta->trackId(), 0);
        if (sample0.ok()) {
            ProtoTree tree = decodeTree(sample0.value());
            doc["rawSample0"] = toJson(tree);
            std::printf("raw: sample 0 decoded into %llu nodes%s\n", static_cast<unsigned long long>(tree.nodeCount),
                        tree.failed ? " (scan failed)" : "");
        }
    }

    // ---- covers ---------------------------------------------------------------------------
    if (!opt.coversDir.empty()) {
        const std::filesystem::path dir(opt.coversDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::fprintf(stderr, "error: cannot create %s: %s\n", safe(dir.string()).c_str(), safe(ec.message()).c_str());
            return kExitRuntime;
        }
        const std::string stem = inputPath.stem().string();
        std::printf("covers:\n");
        if (!writeCover(dir, stem, "covr", movie.covers.covr) || !writeCover(dir, stem, "snal", movie.covers.snal) ||
            !writeCover(dir, stem, "tnal", movie.covers.tnal)) {
            return kExitRuntime;
        }
        if (movie.covers.covr.empty() && movie.covers.snal.empty() && movie.covers.tnal.empty()) {
            std::printf("  (no cover images in this file)\n");
        }
    }

    // ---- warnings + JSON --------------------------------------------------------------
    doc["warnings"] = warnings;
    if (!warnings.empty()) {
        std::printf("warnings (%u):\n", static_cast<unsigned>(warnings.size()));
        for (const json& w : warnings) {
            std::printf("  %s\n", w.get<std::string>().c_str());
        }
    }
    if (!opt.jsonPath.empty()) {
        std::ofstream out(std::filesystem::path(opt.jsonPath), std::ios::binary);
        if (!out.good()) {
            std::fprintf(stderr, "error: cannot create %s\n", safe(opt.jsonPath).c_str());
            return kExitRuntime;
        }
        // NaN / infinity cannot be represented in JSON; nlohmann writes null.
        out << doc.dump(2) << '\n';
        if (!out.good()) {
            std::fprintf(stderr, "error: write failed for %s\n", safe(opt.jsonPath).c_str());
            return kExitRuntime;
        }
        std::printf("wrote %s\n", safe(opt.jsonPath).c_str());
    }
    return kExitOk;
}

}  // namespace

void registerProbeCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<ProbeOptions>();
    CLI::App* sub = app.add_subcommand("probe", "Inspect an .OSV/.LRF file: tracks, format, metadata, calibration");
    sub->add_option("file", opt->inputPath, "Input .OSV or .LRF file")->required();
    sub->add_option("--json", opt->jsonPath, "Write the full JSON document to this path");
    sub->add_option("--frames", opt->frames, "Frames to include: all, N or a-b (default 0)")->capture_default_str();
    sub->add_option("--covers", opt->coversDir, "Extract the embedded JPEG cover images into this directory");
    sub->add_flag("--raw", opt->raw, "Include the schema-less protobuf tree of the selected samples");
    sub->add_flag("--imu", opt->imu, "Include every IMU quaternion of the selected frames");
    // The callback runs during parse; the exit code lands in the context.
    sub->callback([opt, &ctx]() { ctx.exitCode = runProbe(*opt); });
}

}  // namespace osvtool
