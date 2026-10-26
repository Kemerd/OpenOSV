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
//
// --json - writes the document to STDOUT instead of a file, and then stdout
// carries nothing else: the console summary, the "wrote" line and the logs
// all go to stderr.  The bytes are UTF-8 with no byte-order mark and plain
// "\n" line endings on every platform (stdout is switched to binary mode for
// the write, so the Windows C runtime does not turn them into "\r\n").  Exit
// codes as everywhere: 0 ok, 1 usage, 2 unreadable input, 3 runtime failure;
// on any non-zero exit stdout is empty.
//
// STABLE SUBSET (schema "openosv.probe/1").  These top-level keys are a
// contract with the VEGAS extension and are never renamed or retyped; every
// other key of the document may grow.  A value that cannot be determined is
// null (numbers) rather than a missing key.
//
//   schema           "openosv.probe/1" (the djmd field numbering that used to
//                    sit under this key is now "djmdSchema"; it is only
//                    present when a metadata track loaded)
//   path             the path as given on the command line
//   frameCount       video frames (the metadata track's count when it loads,
//                    else the video track's sample count); for a clip that
//                    dropped frames (variable frame rate) the frames of its
//                    constant-rate timeline, gaps held, as the plug-ins
//                    present it (see "timeline" and osv::video::ClipTimeline)
//   fps              { "num": int, "den": int, "value": double } - exact
//                    rational from the video track's timescale and summed
//                    sample durations, reduced (60000/1001, never 59.94); for
//                    a variable-frame-rate clip its NOMINAL rate (50/1)
//   durationSeconds  the video track's duration in seconds; for a
//                    variable-frame-rate clip its timeline's (frameCount / fps)
//   streamW/streamH  size of one lens stream (the whole frame for an LRF)
//   mode             recording mode name ("K6", ...) or null
//   colorModeName    colour mode name ("DLogM", ...) or null
//   hasAudio         true when the container has an audio track
//   audio            { "sampleRate", "channels", "sampleCount" } or null
//   isLrf            true for the side-by-side LRF proxy layout

#include "Commands.h"

#include "osv/container/OsvFile.h"
#include "osv/core/ByteReader.h"
#include "osv/core/Log.h"
#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/ConventionProbe.h"
#include "osv/geom/Stabilization.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/DjmdDecoder.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/MetaJson.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/meta/ProtoTree.h"
#include "osv/video/ClipTimeline.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace osvtool {

namespace {

using nlohmann::json;
using osv::log::safe;

/// Schema marker of the stable top-level subset (see the file comment).
constexpr const char* kProbeSchema = "openosv.probe/1";

/// Path from a UTF-8 command line argument.  main() decodes the Windows
/// command line to UTF-8; a plain std::filesystem::path(std::string) would
/// read those bytes in the ANSI code page and miss any non-ASCII file name.
std::filesystem::path pathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

/// Encode a `std::filesystem::path` back to UTF-8 without touching `CP_ACP`.
std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

/// Keeps stdout clean for `--json -`.
///
/// The console summary is printed with printf all over this file, so instead
/// of touching every call the C-level stdout descriptor is pointed at stderr
/// for the lifetime of the object, and the JSON is written straight to the
/// saved original descriptor.  Destruction restores stdout on every return
/// path.  When the object is not active (a normal run) it does nothing.
class StdoutJson {
public:
    explicit StdoutJson(bool active) {
        if (!active) {
            return;
        }
        std::fflush(stdout);
        std::fflush(stderr);
#if defined(_WIN32)
        m_saved = _dup(_fileno(stdout));
        if (m_saved >= 0) {
            _setmode(m_saved, _O_BINARY);  // no "\n" -> "\r\n" translation for the JSON
            _dup2(_fileno(stderr), _fileno(stdout));
        }
#else
        m_saved = ::dup(STDOUT_FILENO);
        if (m_saved >= 0) {
            ::dup2(STDERR_FILENO, STDOUT_FILENO);
        }
#endif
    }

    ~StdoutJson() {
        if (m_saved < 0) {
            return;
        }
        std::fflush(stdout);
#if defined(_WIN32)
        _dup2(m_saved, _fileno(stdout));
        _close(m_saved);
#else
        ::dup2(m_saved, STDOUT_FILENO);
        ::close(m_saved);
#endif
    }

    StdoutJson(const StdoutJson&) = delete;
    StdoutJson& operator=(const StdoutJson&) = delete;

    /// True while stdout is redirected (false for an inactive object or when dup failed).
    [[nodiscard]] bool ready() const noexcept { return m_saved >= 0; }

    /// Write all of `text` to the real stdout.  False on a short or failed write.
    [[nodiscard]] bool emit(const std::string& text) const {
        if (m_saved < 0) {
            return false;
        }
        std::size_t done = 0;
        while (done < text.size()) {
            // Chunked so the int-sized count of _write can never overflow.
            const std::size_t chunk = std::min<std::size_t>(text.size() - done, 1u << 20);
#if defined(_WIN32)
            const int n = _write(m_saved, text.data() + done, static_cast<unsigned>(chunk));
#else
            const ssize_t n = ::write(m_saved, text.data() + done, chunk);
#endif
            if (n <= 0) {
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

private:
    int m_saved = -1;  ///< The original stdout descriptor while redirected, else -1.
};

/// Reduced rational frame rate of `t`: samples * timescale / total duration,
/// which for constant-rate video is exactly timescale / sample delta
/// (60000 / 1001).  Returns false when the track has no usable timing.
bool exactFrameRate(const osv::TrackInfo& t, std::uint64_t& num, std::uint64_t& den) {
    const std::uint64_t total = t.samples.totalDuration();
    if (total == 0 || t.timescale == 0 || t.samples.count() == 0) {
        return false;
    }
    num = static_cast<std::uint64_t>(t.samples.count()) * t.timescale;
    den = total;
    const std::uint64_t g = std::gcd(num, den);
    if (g > 1) {
        num /= g;
        den /= g;
    }
    // The fields are emitted as 32-bit-safe integers.
    return num <= 0x7FFFFFFFull && den <= 0x7FFFFFFFull;
}

/// The "timeline" object: how the clip is presented on a constant-rate
/// timeline (see osv::video::ClipTimeline).  Not part of the stable subset;
/// the stable keys already carry its frame count, rate and length.
json timelineJson(const osv::video::ClipTimeline& timeline) {
    json t;
    t["variable"] = !timeline.identity();
    t["clock"] = osv::video::timelineClockName(timeline.clock);
    t["samples"] = timeline.sampleCount;
    t["frames"] = timeline.frameCount();
    t["heldFrames"] = timeline.heldFrames;
    t["skippedSamples"] = timeline.skippedSamples;
    t["nominalTicks"] = timeline.nominalTicks;
    t["timescale"] = timeline.timescale;
    t["note"] = safe(timeline.note);
    return t;
}

/// Add the stable top-level subset (see the file comment) to `doc`.
///
/// `format` may be null (detection failed); `metaFrames` is the metadata
/// track's frame count (0 without one) and `meta` that track (null without
/// one; its capture timestamps time a variable-frame-rate clip).
void addStableKeys(json& doc, const osv::MovieInfo& movie, const osv::meta::FormatInfo* format,
                   std::uint32_t metaFrames, const osv::meta::MetadataTrack* meta,
                   const std::filesystem::path& input) {
    doc["schema"] = kProbeSchema;

    // ---- the video track the numbers describe -------------------------------------------
    const osv::TrackInfo* video = nullptr;
    if (format) {
        for (const std::uint32_t id : format->videoTrackIds) {
            if (!video && id != 0) {
                video = movie.track(id);
            }
        }
    }
    if (!video) {
        const auto videos = movie.tracksOfKind(osv::TrackKind::Video);
        video = videos.empty() ? nullptr : videos.front();
    }

    // ---- frame count, exact fps, duration -----------------------------------------------
    doc["frameCount"] = metaFrames > 0 ? metaFrames : (video ? video->samples.count() : 0u);
    std::uint64_t num = 0;
    std::uint64_t den = 0;
    if (video && exactFrameRate(*video, num, den)) {
        doc["fps"] = json{{"num", num}, {"den", den}, {"value", static_cast<double>(num) / static_cast<double>(den)}};
    } else {
        doc["fps"] = nullptr;
    }
    doc["durationSeconds"] = video && video->durationSeconds() > 0.0 ? video->durationSeconds() : movie.durationSeconds();

    // ---- [VFR] a clip that dropped frames: its constant-rate timeline ---------------------
    // The plug-ins present such a clip at its nominal rate with the previous
    // picture held over each gap (osv::video::ClipTimeline), and so must the
    // VEGAS import that sizes its event from these three keys - or the picture
    // ends before the sound.  A constant-rate clip keeps the values above.
    if (video) {
        const osv::video::ClipTimeline timeline = osv::video::clipTimelineFor(*video, meta);
        doc["timeline"] = timelineJson(timeline);
        if (!timeline.identity() && timeline.nominalTicks > 0 && timeline.timescale > 0) {
            std::uint64_t tNum = timeline.timescale;
            std::uint64_t tDen = timeline.nominalTicks;
            const std::uint64_t g = std::gcd(tNum, tDen);
            if (g > 1) {
                tNum /= g;
                tDen /= g;
            }
            doc["frameCount"] = timeline.frameCount();
            doc["fps"] = json{{"num", tNum}, {"den", tDen},
                              {"value", static_cast<double>(tNum) / static_cast<double>(tDen)}};
            doc["durationSeconds"] = timeline.durationSeconds();
        }
    } else {
        doc["timeline"] = nullptr;
    }

    // ---- detected format --------------------------------------------------------------------
    if (format) {
        doc["streamW"] = format->streamW;
        doc["streamH"] = format->streamH;
        doc["mode"] = osv::meta::modeName(format->mode);
        doc["colorModeName"] = osv::meta::colorModeName(format->colorMode);
    } else {
        doc["streamW"] = nullptr;
        doc["streamH"] = nullptr;
        doc["mode"] = nullptr;
        doc["colorModeName"] = nullptr;
    }

    // ---- audio -------------------------------------------------------------------------------
    const auto audios = movie.tracksOfKind(osv::TrackKind::Audio);
    const osv::TrackInfo* audio = audios.empty() ? nullptr : audios.front();
    doc["hasAudio"] = audio != nullptr;
    doc["audio"] = nullptr;
    if (audio && audio->audio && audio->timescale > 0) {
        // PCM sample frames: the media duration (mdhd) rescaled to the sample
        // rate, which is what the decoder reports as the track length.
        const std::uint64_t units = audio->duration > 0 ? audio->duration : audio->samples.totalDuration();
        const double rate = audio->audio->sampleRate;
        const auto sampleCount =
            static_cast<std::uint64_t>(std::llround(static_cast<double>(units) * rate / audio->timescale));
        doc["audio"] = json{{"sampleRate", static_cast<std::uint64_t>(std::llround(rate))},
                            {"channels", audio->audio->channelCount},
                            {"sampleCount", sampleCount}};
    }

    // ---- LRF proxy -------------------------------------------------------------------------------
    std::string ext = input.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    doc["isLrf"] = (format && format->sideBySideProxy) || ext == ".lrf";
}

/// One line on what a populated lens record holds and what, if anything,
/// keeps it from being a calibration: which of fx / fy / cx / cy are missing
/// or not positive, whether the frame size is there, where the extrinsic
/// came from (written, the 4-float copy, built from the Euler angles) or
/// that there is none, and the Euler angles themselves.  "partial" on its
/// own told nobody what to look at.
std::string describeRecord(const osv::meta::DewarpParams& d) {
    std::string out;
    // ---- intrinsics --------------------------------------------------------------
    std::string missing;
    const struct { const char* name; float v; } core[] = {{"fx", d.fx}, {"fy", d.fy}, {"cx", d.cx}, {"cy", d.cy}};
    for (const auto& c : core) {
        if (!std::isfinite(c.v) || c.v <= 0.0f) {
            missing += missing.empty() ? "" : "/";
            missing += c.name;
        }
    }
    if (missing.empty()) {
        char buf[96] = {};
        std::snprintf(buf, sizeof(buf), "f=(%.3f, %.3f) c=(%.3f, %.3f)", static_cast<double>(d.fx),
                      static_cast<double>(d.fy), static_cast<double>(d.cx), static_cast<double>(d.cy));
        out += buf;
    } else {
        out += "NO INTRINSICS (" + missing + " missing or zero)";
    }
    // ---- frame size --------------------------------------------------------------
    if (d.width != 0 && d.height != 0) {
        out += " " + std::to_string(d.width) + "x" + std::to_string(d.height);
    } else {
        out += " NO SIZE (width/height 0)";
    }
    // ---- the extrinsic and where it came from --------------------------------------
    bool copyUsable = d.q.size() == 4;
    if (copyUsable) {
        float norm2 = 0.0f;
        for (const float v : d.q) {
            copyUsable = copyUsable && std::isfinite(v);
            norm2 += v * v;
        }
        copyUsable = copyUsable && norm2 >= 1e-6f;
    }
    if (d.camExtriQ.present) {
        char buf[128] = {};
        std::snprintf(buf, sizeof(buf), " q=(%.5f, %.5f, %.5f, %.5f) %s", static_cast<double>(d.camExtriQ.w),
                      static_cast<double>(d.camExtriQ.x), static_cast<double>(d.camExtriQ.y),
                      static_cast<double>(d.camExtriQ.z),
                      d.camExtriQFromEuler ? "BUILT FROM EULER (no cam_extri_q written)" : "written (cam_extri_q)");
        out += buf;
    } else if (copyUsable) {
        char buf[96] = {};
        std::snprintf(buf, sizeof(buf), " q=(%.5f, %.5f, %.5f, %.5f) from the 4-float copy (field 21)",
                      static_cast<double>(d.q[0]), static_cast<double>(d.q[1]), static_cast<double>(d.q[2]),
                      static_cast<double>(d.q[3]));
        out += buf;
    } else {
        out += " NO EXTRINSIC (no cam_extri_q, field 21 ";
        out += d.q.empty() ? "absent" : (d.q.size() == 4 ? "all zero" : std::to_string(d.q.size()) + " floats");
        out += ")";
    }
    if (d.present.test(12) || d.present.test(13) || d.present.test(14)) {
        char buf[96] = {};
        std::snprintf(buf, sizeof(buf), " euler y/p/r=(%.3f, %.3f, %.3f)", static_cast<double>(d.yaw),
                      static_cast<double>(d.pitch), static_cast<double>(d.roll));
        out += buf;
    } else {
        out += " no euler";
    }
    // ---- the fields the camera wrote, by number ---------------------------------------
    std::string fields;
    for (std::size_t i = 1; i < d.present.size(); ++i) {
        if (d.present.test(i)) {
            fields += fields.empty() ? "" : ",";
            fields += std::to_string(i);
        }
    }
    out += " fields[" + fields + "]";
    out += d.hasCore() ? " -> usable" : " -> NOT USABLE";
    return out;
}

/// The attitude the clip carries and what OpenOSV does with it: which
/// sources every frame has (per-frame camera_attitude / camera_acc, the IMU
/// batch), the first frame's values from each, the reading `auto` would
/// build the track with and whether that reading has been verified on this
/// product, and the frame the view is placed in meanwhile.  The next report
/// of a wrong horizon starts from these lines.
void describeAttitude(const osv::meta::MetadataTrack& track, json& doc) {
    using namespace osv;
    using namespace osv::meta;
    using namespace osv::geom;
    const std::uint32_t frameCount = track.frameCount();
    std::uint32_t withAttitude = 0;
    std::uint32_t withAcc = 0;
    std::uint32_t withBatch = 0;
    std::uint64_t batchSamples = 0;
    std::uint32_t decoded = 0;
    std::optional<FrameMeta> first;
    for (std::uint32_t i = 0; i < frameCount; ++i) {
        const Result<FrameMeta> f = track.frame(i);
        if (!f.ok()) {
            continue;
        }
        ++decoded;
        const FrameMeta& m = f.value();
        withAttitude += m.camera.attitude.present ? 1 : 0;
        withAcc += m.camera.accPresent ? 1 : 0;
        if (m.imu.has_value() && m.imu->current.present && !m.imu->current.q.empty()) {
            ++withBatch;
            batchSamples += m.imu->current.q.size();
        }
        if (!first) {
            first = m;
        }
    }
    json a;
    a["framesDecoded"] = decoded;
    a["framesWithCameraAttitude"] = withAttitude;
    a["framesWithCameraAcc"] = withAcc;
    a["framesWithImuBatch"] = withBatch;
    a["imuBatchSamples"] = batchSamples;
    a["imuSamplingRate"] = track.clip().imuSamplingRate;
    std::printf("attitude: %u frames decoded: camera_attitude on %u, camera_acc on %u, IMU batch on %u (%llu samples, "
                "%u Hz)\n",
                decoded, withAttitude, withAcc, withBatch, static_cast<unsigned long long>(batchSamples),
                track.clip().imuSamplingRate);
    if (first) {
        const CameraFrame& c = first->camera;
        if (c.attitude.present) {
            std::printf("  frame 0 camera_attitude (w,x,y,z) = (%.6f, %.6f, %.6f, %.6f)\n", static_cast<double>(c.attitude.w),
                        static_cast<double>(c.attitude.x), static_cast<double>(c.attitude.y),
                        static_cast<double>(c.attitude.z));
        }
        if (c.accPresent) {
            std::printf("  frame 0 camera_acc = (%.4f, %.4f, %.4f)\n", static_cast<double>(c.acc.x),
                        static_cast<double>(c.acc.y), static_cast<double>(c.acc.z));
        }
        if (first->imu.has_value() && first->imu->current.present && !first->imu->current.q.empty()) {
            const ImuBatch& b = first->imu->current;
            const Quaternion& q0 = b.q.front();
            const Quaternion& qn = b.q.back();
            std::printf("  frame 0 IMU batch: ts %u, vsync %u, %u samples, first (w,x,y,z) = (%.6f, %.6f, %.6f, %.6f), "
                        "last = (%.6f, %.6f, %.6f, %.6f)\n",
                        b.ts, b.vsync, static_cast<unsigned>(b.q.size()), static_cast<double>(q0.w),
                        static_cast<double>(q0.x), static_cast<double>(q0.y), static_cast<double>(q0.z),
                        static_cast<double>(qn.w), static_cast<double>(qn.x), static_cast<double>(qn.y),
                        static_cast<double>(qn.z));
        }
    }
    // ---- the reading and what is done with it -----------------------------------------
    const AutoConvention detected = ConventionProbe::autoDetect(track);
    const bool verified = attitudeReadingVerified(track.schema());
    const Mat3d mount = productMount(track.schema());
    a["autoReading"] = attitudeConventionName(detected.conv);
    a["autoReason"] = safe(detected.reason);
    a["readingVerified"] = verified;
    a["productMount"] = json::array({mount.m[0], mount.m[1], mount.m[2], mount.m[3], mount.m[4], mount.m[5],
                                     mount.m[6], mount.m[7], mount.m[8]});
    std::printf("  auto reading: %s; %s\n", attitudeConventionName(detected.conv).c_str(), safe(detected.reason).c_str());
    const Vec3d right = mount.col(0);
    const Vec3d forward = mount.col(1);
    const Vec3d up = mount.col(2);
    std::printf("  product frame (body axes right, forward, up): (%+.0f,%+.0f,%+.0f) (%+.0f,%+.0f,%+.0f) "
                "(%+.0f,%+.0f,%+.0f)%s\n",
                right.x, right.y, right.z, forward.x, forward.y, forward.z, up.x, up.y, up.z,
                mount.distance(Mat3d::identity()) == 0.0 ? " = the body itself" : "");
    if (verified) {
        std::printf("  the %s reading is verified on footage: the plug-ins stabilise and level with it\n",
                    djmdSchemaName(track.schema()));
    } else {
        std::printf("  the %s reading is NOT verified on footage: the plug-ins leave stabilisation off and place the "
                    "view in the product frame; `osvtool render --attitude-convention <reading>` tries one\n",
                    djmdSchemaName(track.schema()));
    }
    doc["attitude"] = a;
}

/// Print one schema-less tree node and its children, one line each, as
/// `field: value` under two spaces of indent per level.  A fixed32 shows
/// both readings (float and unsigned), a varint its unsigned value and, when
/// the top bit of 32 is set, the int32 it would be; bytes show as a string
/// when printable ASCII, else as hex (capped) and, when a whole number of
/// floats, as those floats (capped), since packed float arrays are the
/// schema's habit.  Everything is 7-bit ASCII so the output pastes anywhere.
void printTreeNode(const osv::meta::ProtoNode& n, int depth) {
    using namespace osv::meta;
    const int indent = 2 + 2 * depth;
    std::printf("%*s%u: ", indent, "", n.number);
    switch (n.wire) {
    case WireType::Varint:
        std::printf("varint %llu", static_cast<unsigned long long>(n.varint));
        if (n.varint >= 0x80000000ull && n.varint <= 0xFFFFFFFFull) {
            std::printf(" (int32 %d)", static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(n.varint))));
        }
        std::printf("\n");
        break;
    case WireType::Fixed32: {
        ProtoField tmp;
        tmp.wire = WireType::Fixed32;
        tmp.fixed32 = n.fixed32;
        std::printf("fixed32 %.9g (u32 %u)\n", static_cast<double>(tmp.asFloat()), n.fixed32);
        break;
    }
    case WireType::Fixed64: {
        ProtoField tmp;
        tmp.wire = WireType::Fixed64;
        tmp.fixed64 = n.fixed64;
        std::printf("fixed64 %.17g (u64 %llu)\n", tmp.asDouble(), static_cast<unsigned long long>(n.fixed64));
        break;
    }
    case WireType::LengthDelimited:
        if (n.isMessage) {
            std::printf("message (%llu bytes, %llu fields)%s\n", static_cast<unsigned long long>(n.bytes.size()),
                        static_cast<unsigned long long>(n.children.size()), n.truncated ? " TRUNCATED" : "");
            if (n.truncated) {
                std::printf("%*s  (%s)\n", indent, "", safe(n.failure).c_str());
            }
            for (const ProtoNode& child : n.children) {
                printTreeNode(child, depth + 1);
            }
        } else if (n.isPrintableAscii()) {
            std::printf("\"%s\"\n", safe(std::string(reinterpret_cast<const char*>(n.bytes.data()), n.bytes.size())).c_str());
        } else {
            static constexpr std::size_t kMaxHex = 64;
            static constexpr std::size_t kMaxFloats = 32;
            static constexpr char kDigits[] = "0123456789abcdef";
            std::printf("%llu bytes", static_cast<unsigned long long>(n.bytes.size()));
            const std::size_t count = n.bytes.size() < kMaxHex ? n.bytes.size() : kMaxHex;
            if (count > 0) {
                std::printf(" hex ");
                for (std::size_t i = 0; i < count; ++i) {
                    std::printf("%c%c", kDigits[n.bytes[i] >> 4], kDigits[n.bytes[i] & 0xF]);
                }
                if (n.bytes.size() > kMaxHex) {
                    std::printf("...");
                }
            }
            if (!n.bytes.empty() && n.bytes.size() % 4 == 0 && n.bytes.size() <= 4 * kMaxFloats) {
                std::printf(" floats");
                osv::ByteReader reader(osv::ByteSpan(n.bytes));
                float f = 0.0f;
                while (reader.f32le(f)) {
                    std::printf(" %.9g", static_cast<double>(f));
                }
            }
            std::printf("\n");
        }
        break;
    case WireType::StartGroup:
    case WireType::EndGroup:
        std::printf("group\n");
        break;
    }
}

/// Options collected by CLI11 for the probe command.
struct ProbeOptions {
    std::string inputPath;      ///< The .OSV / .LRF to inspect.
    std::string jsonPath;       ///< --json: write the full document here.
    std::string frames = "0";   ///< --frames: "all", "N" or "a-b".
    std::string coversDir;      ///< --covers: directory for the JPEG cover images.
    bool raw = false;           ///< --raw: include schema-less protobuf trees.
    bool imu = false;           ///< --imu: include the full IMU quaternion lists.
    bool report = false;        ///< --report: everything, plus the JSON beside the clip.
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

/// Print and record everything about the clip's calibration sets: which of
/// the twelve exist (and which are zero-filled placeholders), how each usable
/// one differs from the native reference, what lens accessory the camera
/// recorded, and what every Source Settings choice would actually stitch
/// with.  This is the answer to "I switched Calibration and nothing changed".
///
/// Console lines are 7-bit ASCII like the rest of the probe; `calibration`
/// gains "sets", "accessory" and "choices".
void describeCalibrationSets(const osv::meta::StreamMeta& s, json& calibration) {
    using namespace osv::meta;
    const CalibrationInventory inv = CalibrationSelector::inventory(s);
    const char* refName = inv.nativeReference ? calibrationSetName(*inv.nativeReference) : "none";

    // ---- the twelve sets -----------------------------------------------------
    std::printf("calibration sets (differences vs %s, calibration px):\n", refName);
    json sets = json::array();
    for (const CalibrationSetInfo& info : inv.sets) {
        json j;
        j["name"] = calibrationSetName(info.id);
        j["slots"] = json::array({calibrationSetSlaveSlot(info.id), calibrationSetMasterSlot(info.id)});
        j["state"] = calibrationSetStateName(info.state);
        j["vsNative"] = nullptr;
        std::string detail;
        if (info.vsNative) {
            const CalibrationDelta& d = *info.vsNative;
            j["vsNative"] = json{{"focalPx", d.focalPx},         {"centrePx", d.centrePx},
                                 {"distortion", d.distortion},   {"rotationDeg", d.rotationDeg},
                                 {"identical", d.identical}};
            if (inv.nativeReference && info.id == *inv.nativeReference) {
                detail = "reference";
            } else if (d.identical) {
                detail = "IDENTICAL to the reference (choosing it changes nothing)";
            } else {
                char buf[160] = {};
                std::snprintf(buf, sizeof(buf), "focal %.3f  centre %.3f  k %.7f  rotation %.4f deg", d.focalPx,
                              d.centrePx, d.distortion, d.rotationDeg);
                detail = buf;
            }
        } else if (info.state == CalibrationSetState::Empty) {
            detail = "zero-filled placeholder (the camera had no such calibration)";
        } else if (info.state == CalibrationSetState::Partial) {
            detail = "incomplete (a lens lacks fx/fy/cx/cy/size/extrinsic)";
        }
        std::printf("  %2u/%-2u %-18s %-8s %s\n", calibrationSetSlaveSlot(info.id), calibrationSetMasterSlot(info.id),
                    calibrationSetName(info.id), calibrationSetStateName(info.state), detail.c_str());
        sets.push_back(std::move(j));
    }
    calibration["sets"] = sets;

    // ---- the recorded accessory ------------------------------------------------
    // extri_lens_mode is the only accessory field the format has: it mirrors
    // the camera's "Lens Protection Mode" switch.  There is no ND filter
    // field anywhere in ClipMeta / StreamMeta / FrameMeta, so an ND filter is
    // on record only if the user declared it as a lens protector.
    calibration["accessory"] = json{{"recordedMode", static_cast<int>(inv.recordedMode)},
                                    {"recordedModeName", extriLensModeName(inv.recordedMode)},
                                    {"recordedModePresent", inv.recordedModePresent},
                                    {"ndFilterField", false}};
    std::printf("lens accessory: %s (StreamMeta.extri_lens_mode%s); ND filters have no field of their own\n",
                extriLensModeName(inv.recordedMode), inv.recordedModePresent ? "" : " not recorded, assumed");

    // ---- what each Source Settings choice stitches with ----------------------
    std::printf("calibration choices:\n");
    json choices = json::object();
    for (std::uint8_t c = 0; c < static_cast<std::uint8_t>(CalibrationChoice::Count); ++c) {
        const auto choice = static_cast<CalibrationChoice>(c);
        osv::Result<CalibrationSelection> picked = CalibrationSelector::choose(s, choice);
        if (!picked.ok()) {
            choices[calibrationChoiceName(choice)] = json{{"error", safe(picked.error().toString())}};
            std::printf("  %-12s -> none (%s)\n", calibrationChoiceName(choice), safe(picked.error().toString()).c_str());
            continue;
        }
        const CalibrationSelection& sel = picked.value();
        // "= native" only when nothing at all changes the geometry: a
        // lens-guard choice without its own set is native PLUS the
        // protector field-angle correction, which is not native.
        const bool sameAsNative = (sel.fellBack || sel.identicalToNative) && !sel.protectorCorrection;
        choices[calibrationChoiceName(choice)] = json{{"slave", sel.set.sourceSlave},
                                                      {"master", sel.set.sourceMaster},
                                                      {"set", calibrationSetName(sel.used)},
                                                      {"fellBack", sel.fellBack},
                                                      {"identicalToNative", sel.identicalToNative},
                                                      {"protectorCorrection", sel.protectorCorrection},
                                                      {"reason", safe(sel.reason)}};
        std::printf("  %-12s -> %-14s%s  %s\n", calibrationChoiceName(choice), calibrationSetName(sel.used),
                    sel.protectorCorrection ? " + protector" : (sameAsNative ? " (= native)" : ""),
                    safe(sel.reason).c_str());
    }
    calibration["choices"] = choices;
}

/// Run the command; returns one of the kExit* codes.
int runProbe(ProbeOptions opt) {
    // --report is the one-flag form for a bug report: the tree, every IMU
    // sample of the selected frames, and the JSON document written beside
    // the clip (or into the working directory when that fails) so it can be
    // attached as a file.  An explicit --json still wins.
    if (opt.report) {
        opt.raw = true;
        opt.imu = true;
        if (opt.jsonPath.empty()) {
            const std::filesystem::path clip = pathFromUtf8(opt.inputPath);
            std::filesystem::path beside = clip;
            beside += ".probe.json";
            std::error_code ec;
            const bool writable = !clip.has_parent_path() ||
                                  (std::filesystem::is_directory(clip.parent_path(), ec) && !ec);
            opt.jsonPath = pathToUtf8(writable ? beside : (std::filesystem::path(clip.filename()) += ".probe.json"));
        }
    }
    using namespace osv;
    using namespace osv::meta;

    if (opt.inputPath.empty()) {
        std::fprintf(stderr, "error: input file is required\n");
        return kExitUsage;
    }

    // `--json -`: from here on stdout belongs to the JSON document alone, so
    // the console summary below is diverted to stderr for this run.
    const bool jsonToStdout = (opt.jsonPath == "-");
    StdoutJson stdoutJson(jsonToStdout);
    if (jsonToStdout && !stdoutJson.ready()) {
        std::fprintf(stderr, "error: cannot take over stdout for --json -\n");
        return kExitRuntime;
    }

    // ---- open ------------------------------------------------------------------
    const std::filesystem::path inputPath = pathFromUtf8(opt.inputPath);
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
    doc["path"] = opt.inputPath;  // exact UTF-8, as given
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
        // The schema says whose field numbering the file was read with; the
        // wrong one reads the wrong fields, so it is always printed.
        doc["djmdSchema"] = djmdSchemaName(meta->schema());
        std::printf("metadata: djmd track %u, %u frames, %s schema\n", meta->trackId(), meta->frameCount(),
                    djmdSchemaName(meta->schema()));
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
            // What each populated record holds, so "partial" names the field.
            for (std::uint32_t slot = 1; slot < PanoDewarpParams::SlotCount; ++slot) {
                if (const DewarpParams* rec = s.dewarp.get(slot)) {
                    std::printf("    %2u %-24s %s\n", slot, PanoDewarpParams::fieldName(slot), describeRecord(*rec).c_str());
                }
            }

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
            describeCalibrationSets(s, calibration);
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
        // The attitude: sources, first values, the reading and its standing.
        describeAttitude(*meta, doc);
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
            if (tree.failed) {
                std::printf("  (%s)\n", safe(tree.failure).c_str());
            }
            // The tree itself, on the console: a report of a clip nobody
            // here has needs the field numbers as the camera wrote them,
            // and --json is one more step people skip.
            for (const ProtoNode& n : tree.fields) {
                printTreeNode(n, 0);
            }
        }
    }

    // ---- covers ---------------------------------------------------------------------------
    if (!opt.coversDir.empty()) {
        const std::filesystem::path dir = pathFromUtf8(opt.coversDir);
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
    addStableKeys(doc, movie, format.ok() ? &format.value() : nullptr, meta ? meta->frameCount() : 0u, meta.get(),
                  inputPath);
    // A clip that dropped frames says so on the console too.
    if (doc.contains("timeline") && doc["timeline"].is_object() && doc["timeline"].value("variable", false)) {
        const json& t = doc["timeline"];
        const double rate = doc["fps"].is_object() ? doc["fps"].value("value", 0.0) : 0.0;
        std::printf("timing: variable frame rate - %u samples presented as %u frames at %.3f fps (%.3f s), %u held "
                    "over dropped frames, %s clock\n",
                    t.value("samples", 0u), t.value("frames", 0u), rate, doc.value("durationSeconds", 0.0),
                    t.value("heldFrames", 0u), t.value("clock", std::string("container")).c_str());
    }
    if (!warnings.empty()) {
        std::printf("warnings (%u):\n", static_cast<unsigned>(warnings.size()));
        for (const json& w : warnings) {
            std::printf("  %s\n", w.get<std::string>().c_str());
        }
    }
    if (jsonToStdout) {
        // UTF-8, unescaped; bytes that are not valid UTF-8 (they can only come
        // from a damaged file name) are replaced rather than aborting the dump.
        const std::string text = doc.dump(2, ' ', false, json::error_handler_t::replace) + '\n';
        if (!stdoutJson.emit(text)) {
            std::fprintf(stderr, "error: write to stdout failed\n");
            return kExitRuntime;
        }
    } else if (!opt.jsonPath.empty()) {
        std::ofstream out(pathFromUtf8(opt.jsonPath), std::ios::binary);
        if (!out.good()) {
            std::fprintf(stderr, "error: cannot create %s\n", safe(opt.jsonPath).c_str());
            return kExitRuntime;
        }
        // NaN / infinity cannot be represented in JSON; nlohmann writes null.
        out << doc.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        if (!out.good()) {
            std::fprintf(stderr, "error: write failed for %s\n", safe(opt.jsonPath).c_str());
            return kExitRuntime;
        }
        std::printf("wrote %s\n", safe(opt.jsonPath).c_str());
    }
    if (opt.report) {
        std::printf("report: paste everything above and attach %s; the plug-in log is in %%LOCALAPPDATA%%\\OpenOSV\\ "
                    "(Premiere, Resolve and VEGAS each write their own)\n",
                    safe(opt.jsonPath).c_str());
    }
    return kExitOk;
}

}  // namespace

void registerProbeCommand(CLI::App& app, CommandContext& ctx) {
    auto opt = std::make_shared<ProbeOptions>();
    CLI::App* sub = app.add_subcommand("probe", "Inspect an .OSV/.LRF file: tracks, format, metadata, calibration");
    sub->add_option("file", opt->inputPath, "Input .OSV or .LRF file")->required();
    sub->add_option("--json", opt->jsonPath,
                    "Write the full JSON document to this path, or to stdout (and nothing else) for '-'");
    sub->add_option("--frames", opt->frames, "Frames to include: all, N or a-b (default 0)")->capture_default_str();
    sub->add_option("--covers", opt->coversDir, "Extract the embedded JPEG cover images into this directory");
    sub->add_flag("--raw", opt->raw,
                  "Print the schema-less protobuf tree of sample 0 (every field as the camera wrote it) and "
                  "include the selected samples' trees in --json");
    sub->add_flag("--imu", opt->imu, "Include every IMU quaternion of the selected frames");
    sub->add_flag("--report", opt->report,
                  "Everything a bug report needs: --raw, --imu and the JSON written beside the clip "
                  "(<clip>.probe.json); paste the console output and attach the file");
    // The callback runs during parse; the exit code lands in the context.
    sub->callback([opt, &ctx]() { ctx.exitCode = runProbe(*opt); });
}

}  // namespace osvtool
