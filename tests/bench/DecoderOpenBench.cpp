// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osv_decoder_open_bench: where the time of opening a video decoder goes, and
// what a re-open of an already-seen clip costs.  Not a ctest test - timings on
// a shared machine are not pass/fail material - but the numbers it prints are
// the ones quoted in the commit messages.
//
//   osv_decoder_open_bench [clip.OSV] [--reps N] [--part A|B|C|D|E|all]
//
// The clip defaults to OSV_SAMPLE_FILE (environment), then the CMake default.
//
// What it measures, each figure the median of --reps runs:
//   A  container   OsvFile::open (map + moov parse) on its own
//   B  single      HevcStreamDecoder::open of lens track 1, per back-end
//                  (software / D3D11VA / CUDA) and per demux mode
//                  (libavformat vs container samples), with the phase
//                  breakdown the decoder records (DecoderOpenTimings); the
//                  historical options (private device, probe decode) and the
//                  re-open options (shared device kept alive by another
//                  decoder, deferred first frame) back to back
//   C  dual        DualStreamReader::open, the importer's reader, with the
//                  historical options and with the importer's options, cold
//                  (no live device) and warm (another reader alive)
//   D  landings    twelve scattered random accesses on a D3D11VA reader:
//                  one device per lens vs one device for both lenses, and
//                  libavcodec frame threads vs a single thread - the per-frame
//                  price of the open-time choices
//   E  reopen      what the user waits for after an unquiet: reader ready,
//                  and reader ready + the playhead's frame decoded, for the
//                  historical options, the importer's options cold, the same
//                  with another reader alive, and a warm reader from the pool

#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"
#include "osv/meta/FormatDetector.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/video/Decoder.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"
#include "osv/video/ReaderPool.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace osv;
using namespace osv::video;

/// Milliseconds since `start`.
double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// Median of a copy of `values` (0 when empty).
double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return (n % 2 == 1) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

/// Per-phase medians over several opens.
struct PhaseMedians {
    std::vector<double> total, map, container, demux, probe, device, codec, first;

    void add(const DecoderOpenTimings& t) {
        total.push_back(t.totalMs);
        map.push_back(t.mapMs);
        container.push_back(t.containerMs);
        demux.push_back(t.demuxOpenMs);
        probe.push_back(t.streamInfoMs);
        device.push_back(t.hwDeviceMs);
        codec.push_back(t.codecOpenMs);
        first.push_back(t.firstFrameMs);
    }

    void print(const std::string& label) const {
        std::printf("  %-44s total %7.1f | map %4.1f container %4.1f demux %4.1f probe %5.1f device %6.1f codec %5.1f "
                    "first %6.1f  (median of %zu)\n",
                    label.c_str(), median(total), median(map), median(container), median(demux), median(probe),
                    median(device), median(codec), median(first), total.size());
    }
};

/// Command line.
struct Args {
    std::filesystem::path clip;
    int reps = 5;
    std::string part = "all";
};

Args parseArgs(int argc, char** argv) {
    Args a;
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        a.clip = env;
    } else {
        a.clip = OSV_SAMPLE_FILE;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--reps" && i + 1 < argc) {
            a.reps = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--part" && i + 1 < argc) {
            a.part = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            a.clip = arg;
        }
    }
    return a;
}

/// True when part `p` was selected.
bool wants(const Args& a, const char* p) { return a.part == "all" || a.part == p; }

/// The three back-ends the importer can use, in its preference order.
const HwAccel kBackends[] = {HwAccel::D3D11VA, HwAccel::Cuda, HwAccel::None};

/// Options as the importer used them before this bench existed: libavformat,
/// a private device per decoder, frame 0 decoded inside open().
DecoderOptions historicalOptions(HwAccel hw) {
    DecoderOptions o;
    o.hw = hw;
    o.useContainerSamples = false;
    o.shareHwDevice = false;
    o.deferFirstFrame = false;
    return o;
}

/// Options for a re-open: container samples, the shared device, no probe.
DecoderOptions reopenOptions(HwAccel hw) {
    DecoderOptions o;
    o.hw = hw;
    o.useContainerSamples = true;
    o.shareHwDevice = true;
    o.deferFirstFrame = true;
    return o;
}

/// Where the playhead lands (the importer bench's Part P order): scattered
/// over both GOPs, never sequential.
constexpr std::uint32_t kLandings[] = {27, 45, 10, 63, 32, 5, 50, 18, 40, 2, 58, 23};

// ===========================================================================
//  B. one lens
// ===========================================================================
void partSingle(const Args& args, const meta::FormatInfo& format) {
    std::printf("\nB. single lens (track %u):\n", format.videoTrackIds[0]);
    for (const HwAccel hw : kBackends) {
        // Historical, per demux mode.
        for (const bool samples : {false, true}) {
            DecoderOptions options = historicalOptions(hw);
            options.useContainerSamples = samples;
            PhaseMedians phases;
            std::string failure;
            for (int r = 0; r < args.reps; ++r) {
                auto opened = HevcStreamDecoder::open(args.clip, format.videoTrackIds[0], options);
                if (!opened.ok()) {
                    failure = opened.error().toString();
                    break;
                }
                phases.add(opened.value().openTimings());
            }
            const std::string label = std::string(hwAccelName(hw)) + (samples ? " samples" : " avformat") +
                                      ", private device, probe";
            if (!failure.empty()) {
                std::printf("  %-44s unavailable: %s\n", label.c_str(), failure.c_str());
                continue;
            }
            phases.print(label);
        }
        // Re-open: another decoder of the same slot keeps the device alive.
        {
            const DecoderOptions options = reopenOptions(hw);
            auto keeper = HevcStreamDecoder::open(args.clip, format.videoTrackIds[1], options);
            PhaseMedians phases;
            std::string failure = keeper.ok() ? "" : keeper.error().toString();
            for (int r = 0; r < args.reps && failure.empty(); ++r) {
                auto opened = HevcStreamDecoder::open(args.clip, format.videoTrackIds[0], options);
                if (!opened.ok()) {
                    failure = opened.error().toString();
                    break;
                }
                phases.add(opened.value().openTimings());
            }
            const std::string label = std::string(hwAccelName(hw)) + " samples, shared device, deferred";
            if (!failure.empty()) {
                std::printf("  %-44s unavailable: %s\n", label.c_str(), failure.c_str());
            } else {
                phases.print(label);
            }
        }
    }
}

// ===========================================================================
//  C. the importer's reader
// ===========================================================================
void partDual(const Args& args, const meta::FormatInfo& format) {
    std::printf("\nC. dual reader (both lenses, opened in parallel):\n");
    for (const HwAccel hw : kBackends) {
        // Historical options, every open cold by construction.
        {
            std::vector<double> totals;
            std::string failure;
            for (int r = 0; r < args.reps; ++r) {
                const auto t0 = Clock::now();
                auto reader = DualStreamReader::open(args.clip, format, historicalOptions(hw));
                totals.push_back(msSince(t0));
                if (!reader.ok()) {
                    failure = reader.error().toString();
                    break;
                }
            }
            const std::string label = std::string(hwAccelName(hw)) + " historical options";
            if (!failure.empty()) {
                std::printf("  %-44s unavailable: %s\n", label.c_str(), failure.c_str());
                continue;
            }
            std::printf("  %-44s open %7.1f ms (median of %zu)\n", label.c_str(), median(totals), totals.size());
        }
        // Re-open options: cold (nothing alive) and warm (another reader of
        // the clip alive, as when Premiere opens a second instance for a
        // Source Settings change while the first one still holds its reader).
        for (const bool warm : {false, true}) {
            std::optional<DualStreamReader> keeper;
            if (warm) {
                auto k = DualStreamReader::open(args.clip, format, reopenOptions(hw));
                if (k.ok()) {
                    keeper.emplace(std::move(k).value());
                }
            }
            std::vector<double> totals;
            std::string failure;
            for (int r = 0; r < args.reps; ++r) {
                const auto t0 = Clock::now();
                auto reader = DualStreamReader::open(args.clip, format, reopenOptions(hw));
                totals.push_back(msSince(t0));
                if (!reader.ok()) {
                    failure = reader.error().toString();
                    break;
                }
            }
            const std::string label = std::string(hwAccelName(hw)) + " importer options, " +
                                      (warm ? "another reader alive" : "cold");
            if (!failure.empty()) {
                std::printf("  %-44s unavailable: %s\n", label.c_str(), failure.c_str());
                continue;
            }
            std::printf("  %-44s open %7.1f ms (median of %zu)\n", label.c_str(), median(totals), totals.size());
        }
    }
}

// ===========================================================================
//  D. the per-frame price of the device and threading choices
// ===========================================================================

/// Decode `index` on both decoders in parallel, as DualStreamReader::read.
bool readBoth(HevcStreamDecoder& a, HevcStreamDecoder& b, std::uint32_t index) {
    bool okA = false;
    std::thread worker([&] { okA = a.decodeFrame(index).ok(); });
    const bool okB = b.decodeFrame(index).ok();
    worker.join();
    return okA && okB;
}

void partLandings(const Args& args, const meta::FormatInfo& format) {
    std::printf("\nD. twelve scattered landings, D3D11VA, both lenses in parallel, host frames:\n");
    struct Variant {
        const char* name;
        int slotA;
        int slotB;
        int threads;
    };
    const Variant variants[] = {
        {"one device per lens, auto threads", 0, 1, 0},
        {"one device for both, auto threads", 0, 0, 0},
        {"one device per lens, 1 thread", 0, 1, 1},
        {"one device for both, 1 thread", 0, 0, 1},
        {"one device per lens, 2 threads", 0, 1, 2},
        {"one device per lens, 4 threads", 0, 1, 4},
    };
    for (const Variant& v : variants) {
        DecoderOptions oa = reopenOptions(HwAccel::D3D11VA);
        DecoderOptions ob = oa;
        oa.threads = v.threads;
        ob.threads = v.threads;
        oa.hwDeviceSlot = v.slotA + 10;  // slots nobody else uses: fresh devices per variant
        ob.hwDeviceSlot = v.slotB + 10;
        auto a = HevcStreamDecoder::open(args.clip, format.videoTrackIds[0], oa);
        auto b = HevcStreamDecoder::open(args.clip, format.videoTrackIds[1], ob);
        if (!a.ok() || !b.ok()) {
            std::printf("  %-40s unavailable: %s\n", v.name,
                        (!a.ok() ? a.error() : b.error()).toString().c_str());
            continue;
        }
        std::vector<double> ms;
        // The first landing includes the decoder's own first-use setup; it
        // is reported separately so the steady cost is not skewed by it.
        double firstMs = 0.0;
        for (const std::uint32_t frame : kLandings) {
            const auto t0 = Clock::now();
            if (!readBoth(a.value(), b.value(), frame)) {
                std::printf("  %-40s frame %u failed\n", v.name, frame);
                break;
            }
            const double t = msSince(t0);
            if (firstMs == 0.0) {
                firstMs = t;
            } else {
                ms.push_back(t);
            }
        }
        // Sequential throughput: 30 frames in order from a sync sample.
        const auto s0 = Clock::now();
        bool seqOk = true;
        for (std::uint32_t f = 1; f < 31 && seqOk; ++f) {
            seqOk = readBoth(a.value(), b.value(), f);
        }
        const double seqMs = msSince(s0) / 30.0;
        std::printf("  %-40s first %6.1f | landings median %6.1f worst %6.1f | sequential %5.1f ms/pair%s\n", v.name,
                    firstMs, median(ms), ms.empty() ? 0.0 : *std::max_element(ms.begin(), ms.end()), seqMs,
                    seqOk ? "" : " (FAILED)");
    }
}

// ===========================================================================
//  E. what the user waits for: reopen -> first frame on screen
// ===========================================================================

/// One reopen of the clip followed by the frame the playhead is on, the way
/// the importer does it after an unquiet: returns {reader ready ms, reader
/// ready + pair decoded ms}.
struct ReopenTiming {
    double readyMs = 0.0;
    double firstPairMs = 0.0;
    bool ok = false;
};

void partReopen(const Args& args, const meta::FormatInfo& format) {
    std::printf("\nE. reopen -> first frame (frame 17, both lenses, host frames), %d reps each:\n", args.reps);
    for (const HwAccel hw : kBackends) {
        // Is the back-end there at all?
        {
            auto probe = DualStreamReader::open(args.clip, format, reopenOptions(hw));
            if (!probe.ok()) {
                std::printf("  %-8s unavailable: %s\n", hwAccelName(hw), probe.error().toString().c_str());
                continue;
            }
        }
        // The importer's options on hardware use four decoder threads (see
        // ImporterInstance.cpp importerReaderOptions); software keeps auto.
        DecoderOptions importer = reopenOptions(hw);
        importer.threads = (hw == HwAccel::None) ? 0 : 4;

        auto runCold = [&](const DecoderOptions& options) {
            ReopenTiming t;
            const auto t0 = Clock::now();
            auto reader = DualStreamReader::open(args.clip, format, options);
            t.readyMs = msSince(t0);
            if (reader.ok()) {
                t.ok = reader.value().read(17).ok();
            }
            t.firstPairMs = msSince(t0);
            return t;
        };

        std::vector<double> histReady, histFirst, coldReady, coldFirst, aliveReady, aliveFirst, warmReady, warmFirst;
        for (int r = 0; r < args.reps; ++r) {
            // 1. before: the historical options, nothing alive.
            const ReopenTiming a = runCold(historicalOptions(hw));
            histReady.push_back(a.readyMs);
            histFirst.push_back(a.firstPairMs);
            // 2. importer options, nothing alive (first open of the session).
            const ReopenTiming b = runCold(importer);
            coldReady.push_back(b.readyMs);
            coldFirst.push_back(b.firstPairMs);
        }
        {
            // 3. importer options while another reader of the clip is alive
            //    (a Source Settings change with the old instance still open).
            auto keeper = DualStreamReader::open(args.clip, format, importer);
            for (int r = 0; r < args.reps && keeper.ok(); ++r) {
                const ReopenTiming c = runCold(importer);
                aliveReady.push_back(c.readyMs);
                aliveFirst.push_back(c.firstPairMs);
            }
        }
        {
            // 4. the pool: the reader parked by a quiet (it had shown frame 10)
            //    is taken back by the unquiet.
            ReaderPool::Limits benchLimits;
            benchLimits.minAvailableMemoryMiB = 0;  // other processes' memory use is not under test
            ReaderPool pool(benchLimits);
            for (int r = 0; r < args.reps; ++r) {
                auto opened = DualStreamReader::open(args.clip, format, importer);
                if (!opened.ok() || !opened.value().read(10).ok()) {
                    break;
                }
                pool.park(std::make_unique<DualStreamReader>(std::move(opened).value()));
                const auto t0 = Clock::now();
                std::unique_ptr<DualStreamReader> warm = pool.take(args.clip, format, importer);
                warmReady.push_back(msSince(t0));
                const bool ok = warm && warm->read(17).ok();
                warmFirst.push_back(msSince(t0));
                if (!ok) {
                    std::printf("  %-8s warm reader failed to decode\n", hwAccelName(hw));
                    break;
                }
            }
        }
        std::printf("  %-8s historical, cold            ready %7.1f  first frame %7.1f ms\n", hwAccelName(hw),
                    median(histReady), median(histFirst));
        std::printf("  %-8s importer, cold              ready %7.1f  first frame %7.1f ms\n", hwAccelName(hw),
                    median(coldReady), median(coldFirst));
        std::printf("  %-8s importer, reader alive      ready %7.1f  first frame %7.1f ms\n", hwAccelName(hw),
                    median(aliveReady), median(aliveFirst));
        std::printf("  %-8s importer, warm from pool    ready %7.1f  first frame %7.1f ms\n", hwAccelName(hw),
                    median(warmReady), median(warmFirst));
    }
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    std::error_code ec;
    if (!std::filesystem::exists(args.clip, ec)) {
        std::fprintf(stderr, "clip not found: %s\n", args.clip.string().c_str());
        return 2;
    }
    // FFmpeg's probe warnings about the metadata streams are noise here.
    log::setLevel(log::Level::Error);
    std::printf("osv_decoder_open_bench: %s, %d reps\n", args.clip.filename().string().c_str(), args.reps);

    // ---- A. the container on its own ---------------------------------------------
    meta::FormatInfo format;
    {
        std::vector<double> parse;
        for (int r = 0; r < args.reps; ++r) {
            const auto t0 = Clock::now();
            auto file = OsvFile::open(args.clip);
            parse.push_back(msSince(t0));
            if (!file.ok()) {
                std::fprintf(stderr, "OsvFile::open failed: %s\n", file.error().toString().c_str());
                return 1;
            }
            if (r == 0) {
                auto track = meta::MetadataTrack::load(file.value());
                auto detected = meta::FormatDetector::detect(file.value(), track.ok() ? &track.value() : nullptr);
                if (!detected.ok()) {
                    std::fprintf(stderr, "format detection failed: %s\n", detected.error().toString().c_str());
                    return 1;
                }
                format = std::move(detected).value();
            }
        }
        std::printf("\nA. container: OsvFile::open %.2f ms (median)\n", median(parse));
    }

    if (wants(args, "B")) {
        partSingle(args, format);
    }
    if (wants(args, "C")) {
        partDual(args, format);
    }
    if (wants(args, "D")) {
        partLandings(args, format);
    }
    if (wants(args, "E")) {
        partReopen(args, format);
    }
    return 0;
}
