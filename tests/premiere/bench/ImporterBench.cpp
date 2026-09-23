// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterBench.cpp - where does a frame's time actually go?
//
// A measurement tool, not a test: it asserts nothing and is deliberately NOT
// registered with ctest, because its numbers depend on the machine and would
// only make the suite slow and flaky.  Run it by hand:
//
//     build\<dir>\bin\osv_importer_bench.exe [clip.OSV] [--frames N] [--part A|B|C|D|P|F|Q|all]
//
// WHY IT EXISTS
// -------------
// Earlier timings came from `osvtool render --out x.png`, and PNG encoding
// alone costs ~970 ms per 3.7 MP frame - more than everything it was meant to
// measure.  This harness never touches an image encoder.  It times four
// things, each in its own part so a part can be run in a fresh process when a
// COLD number (first CUDA use, first decoder open) is what matters:
//
//   A  the real importer, end to end: OpenOSVImporter.prm loaded through the
//      same mock host the importer tests use, imGetSourceVideo called frame by
//      frame exactly as Premiere does during playback.  This is the ground
//      truth - whatever the stage numbers say, this is what a frame costs.
//
//   B  the same work split into stages.  The importer's renderFrame() cannot
//      be instrumented from outside (and its sources are not ours to edit),
//      so its pipeline is REPLAYED here with the same library calls in the
//      same order - decode, seam search, gain estimate, job build, CUDA
//      upload + kernel, readback, PPix copy - each timed on its own.  The sum
//      of the stages is printed next to part A's total so the replay can be
//      checked against the real thing rather than trusted.
//
//   C  the reframe EFFECT's GPU kernel alone, fed from device memory, which
//      is how Premiere's GPU path runs it: the input frame is already a GPU
//      PPix, so upload and readback are not the effect's cost.
//
//   D  the reframe effect's CPU path (renderCpu), for the software renderer.
//
//   P  parking the playhead: the Stopped / 8u / full-size requests Premiere
//      sends when the user clicks somewhere new on the timeline, at scattered
//      frames so no decode-ahead can hide the cost - plus the decoder's
//      random-access cost alone, for comparison.
//
//   F  the importer's own frame by pixel format (32f / 16u / 8u) and by path
//      (the GPU frame path: NVDEC into VRAM, stitch in place, pack, pinned
//      banded readback - versus the host path it replaced, forced with
//      OPENOSV_IMPORTER_NO_GPU_DECODE), parking and playing, in ONE process
//      back to back so the ratios survive a noisy machine.  The mock host's
//      own PPix allocation is measured per format and reported beside them.
//
//   Q  quiet / unquiet and a new instance of the clip (what Premiere does on
//      every Source Settings change): the first frame after each, per frame
//      path, against the same frame on a clip that stayed open.
//
// Output is plain ASCII so it survives any Windows console code page.

#include "ImporterHarness.h"
#include "TestLogIsolation.h"

#include "PixelCopy.h"
#include "PrefsBlob.h"
#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "Pipeline.h"

#include "CudaLaunch.h"
#include "osv/core/ThreadPool.h"
#include "osv/geom/EquirectMap.h"     // [WP-STEADY]
#include "osv/render/ClipSteady.h"    // [WP-STEADY]
#include "osv/render/CudaRenderer.h"
#include "osv/render/FlowBackend.h"   // [WP-STEADY]
#include "osv/render/ImageRGBAf.h"
#include "osv/render/LensAlign.h"     // [WP-STEADY]
#include "osv/render/ParallaxWarp.h"  // [WP-STEADY]
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"
#include "osv/render/SeamCarve.h"     // [WP-STEADY]

#include "osv/geom/AttitudeTrack.h"
#include "osv/geom/ConventionProbe.h"
#include "osv/video/HwAccel.h"

#include "PrSDKImport.h"
#include "PrSDKPPixCreator2Suite.h"
#include "PrSDKPPixSuite.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using osv::premiere::PrefsBlob;
using osv::premiere::PrefsOutputSize;
using osv::premiere::PrefsStabilization;
using osv::premiere::test::ImporterHarness;

/// Premiere's tick rate (PrSDKTimeSuite / PrSDKTypes.h).
constexpr PrTime kTicksPerSecond = 254016000000LL;

// ===========================================================================
//  Timing helpers
// ===========================================================================

/// Milliseconds elapsed since `t0`.
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// Run `fn` once and return how long it took, in milliseconds.
double timeMs(const std::function<void()>& fn) {
    const Clock::time_point t0 = Clock::now();
    fn();
    return msSince(t0);
}

/// Median of a set of samples; 0 for an empty set.  The median rather than
/// the mean because one frame that hits a keyframe, a page-fault storm or a
/// Windows scheduler hiccup should not move the headline number.
double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// Smallest sample; 0 for an empty set.
double minimum(const std::vector<double>& v) {
    return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end());
}

/// Samples from index `from` onward - the "warm" frames, once the decoder,
/// the CUDA buffers and the pinned staging area have all been allocated.
std::vector<double> tail(const std::vector<double>& v, std::size_t from) {
    if (v.size() <= from) {
        return {};
    }
    return std::vector<double>(v.begin() + static_cast<std::ptrdiff_t>(from), v.end());
}

/// Minimum number of leading frames excluded from the warm statistics.
/// Frame 0 is the cold frame; frame 1 still pays one-off costs on some paths.
constexpr std::size_t kWarmFrom = 2;

/// The WARM samples of a run: its second half, and never fewer than
/// kWarmFrom frames in.
///
/// Not simply "everything after frame 1", because of how the decoder works.
/// libavcodec's frame threading keeps about one frame in flight per thread,
/// so the cold frame 0 primes dozens of frames at once and the following
/// dozens are handed back already decoded.  Over a short run every "warm"
/// frame is served from that primed buffer and the decode cost silently
/// vanishes from the numbers.  Taking the second half of a run that is
/// longer than the buffer measures the steady state playback actually sees.
std::vector<double> warm(const std::vector<double>& v) {
    return tail(v, std::max(kWarmFrom, v.size() / 2));
}

// ===========================================================================
//  Command line
// ===========================================================================

struct Options {
    std::filesystem::path clip;
    /// Long enough that the second half lies beyond the decoder's primed
    /// frames (see warm()); shorter runs report decode as nearly free.
    int frames = 64;
    std::string part = "all";
};

Options parseArgs(int argc, char** argv) {
    Options o;
#ifdef OSV_BENCH_DEFAULT_CLIP
    o.clip = OSV_BENCH_DEFAULT_CLIP;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--frames" && i + 1 < argc) {
            // Clamp rather than trust: fewer than kWarmFrom + 1 frames leaves
            // no warm sample, and thousands would only make the run slow.
            o.frames = std::clamp(std::atoi(argv[++i]), static_cast<int>(kWarmFrom) + 1, 240);
        } else if (a == "--part" && i + 1 < argc) {
            o.part = argv[++i];
        } else if (!a.empty() && a[0] != '-') {
            o.clip = a;
        }
    }
    return o;
}

bool wantPart(const Options& o, char part) {
    return o.part == "all" || (o.part.size() == 1 && (o.part[0] == part || o.part[0] == part + ('a' - 'A')));
}

// ===========================================================================
//  The configurations every part is run over
// ===========================================================================

/// One importer configuration: the prefs that drive the stitch and the size
/// the host asks for.
struct ImporterConfig {
    const char* name;
    PrefsOutputSize outputSize;
    int requestW;
    int requestH;
    bool analyses;  ///< Seam search + gain match + horizon lock (the shipping defaults).
};

/// The shipping defaults turn on seam search, gain matching and horizon
/// lock; the importer test pins all three OFF to measure the bare frame path
/// (and it is that test's "~800 ms native frame 0" that prompted this tool).
/// Both are measured so the cost of the analyses is visible on its own.
const ImporterConfig kImporterConfigs[] = {
    {"native 6000x3000, analyses off", PrefsOutputSize::Native, 6000, 3000, false},
    {"native 6000x3000, defaults    ", PrefsOutputSize::Native, 6000, 3000, true},
    {"native, 3000x1500 request, def", PrefsOutputSize::Native, 3000, 1500, true},
    {"2560x1280, analyses off       ", PrefsOutputSize::QHD2560, 2560, 1280, false},
    {"2560x1280, defaults           ", PrefsOutputSize::QHD2560, 2560, 1280, true},
};

PrefsBlob prefsFor(const ImporterConfig& c) {
    PrefsBlob p = PrefsBlob::defaults();
    p.outputSize = static_cast<std::uint8_t>(c.outputSize);
    p.seamSearch = c.analyses ? 1 : 0;
    p.gainMatch = c.analyses ? 1 : 0;
    p.stabilization =
        static_cast<std::uint8_t>(c.analyses ? PrefsStabilization::HorizonLock : PrefsStabilization::Off);
    return p;
}

// ===========================================================================
//  Part A - the real importer, end to end
// ===========================================================================

void partA(const Options& o) {
    std::printf("\n=== A. OpenOSVImporter.prm end to end (imGetSourceVideo, BGRA_4444_32f) ===\n");
    std::printf("    frame 0 of the FIRST row is process-cold (CUDA context + module load);\n");
    std::printf("    frame 0 of later rows is clip-cold (decoder open, analyses) only.\n\n");

    ImporterHarness harness;
    if (!harness.loaded()) {
        std::printf("    cannot load the importer: %s\n", harness.loadError().c_str());
        return;
    }

    // The PPix suite, to dispose each frame: a 6000x3000 32f frame is 288 MB
    // and the harness hands ownership to the caller.
    const void* rawSuite = nullptr;
    if (harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &rawSuite) != kSPNoError ||
        !rawSuite) {
        std::printf("    cannot acquire the PPix suite from the mock host\n");
        return;
    }
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(rawSuite);

    std::printf("    %-32s %10s %10s %10s\n", "config", "frame0 ms", "warm med", "warm min");
    csSDK_int32 importerId = 100;
    for (const ImporterConfig& c : kImporterConfigs) {
        // A fresh clip per configuration, with its own importer id, so no
        // frame cache (the host's or the instance's) can serve a hit.
        harness.host().clearCache();
        auto clip = harness.openClip(o.clip, importerId++);
        if (!clip.open()) {
            std::printf("    %-32s  open failed (%d)\n", c.name, static_cast<int>(clip.openResult()));
            continue;
        }
        const PrefsBlob prefs = prefsFor(c);
        imFileInfoRec8 info{};
        if (harness.getInfo8(clip, info, &prefs) != imNoErr || info.vidScale <= 0 || info.vidSampleSize <= 0) {
            std::printf("    %-32s  imGetInfo8 failed\n", c.name);
            continue;
        }
        const PrTime ticksPerFrame = kTicksPerSecond * info.vidSampleSize / info.vidScale;

        std::vector<double> ms;
        bool failed = false;
        for (int i = 0; i < o.frames; ++i) {
            ImporterHarness::SourceVideoRequest request;
            request.frameTime = ticksPerFrame * i;
            request.format = PrPixelFormat_BGRA_4444_32f;
            request.width = c.requestW;
            request.height = c.requestH;
            // Playback at full rate (ratio 1.0).  isDraftRequest() treats
            // that as NOT a draft, so the seam search and gain estimate run
            // exactly as during real playback that is keeping up.  (Once the
            // host drops the ratio below 1.0 they are skipped - which is why
            // a struggling timeline gets cheaper frames, not dropped ones.)
            request.intent = imRenderIntent_Playing;
            request.playbackRatio = 1.0;
            PPixHand hand = nullptr;
            const Clock::time_point t0 = Clock::now();
            const csSDK_int32 err = harness.getSourceVideo(clip, request, prefs, hand);
            ms.push_back(msSince(t0));
            if (err != imNoErr || !hand) {
                std::printf("    %-32s  frame %d failed (%d)\n", c.name, i, static_cast<int>(err));
                failed = true;
                break;
            }
            ppix->Dispose(hand);
        }
        if (!failed) {
            std::printf("    %-32s %10.1f %10.1f %10.1f\n", c.name, ms.front(), median(warm(ms)),
                        minimum(warm(ms)));
        }
        clip.close();
    }

    // ---- what the MOCK host adds ---------------------------------------------
    // Part A's totals include the mock's own PPix allocation, which zero-fills
    // a fresh buffer on every CreatePPix (MockPPix.cpp: storage.assign(..., 0))
    // and frees it again on Dispose.  That is a property of the test double,
    // not of the importer - Premiere recycles PPix memory from its own pool -
    // so it is measured here on its own and must be subtracted before part A
    // is compared with part B or with a real host.
    const void* rawCreator = nullptr;
    if (harness.host().basicSuite()->AcquireSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion,
                                                  &rawCreator) == kSPNoError &&
        rawCreator) {
        const auto* creator = static_cast<const PrSDKPPixCreator2Suite*>(rawCreator);
        std::printf("\n    mock-host overhead included above (CreatePPix + Dispose, 32f):\n");
        for (const ImporterConfig* c : {&kImporterConfigs[0], &kImporterConfigs[2], &kImporterConfigs[3]}) {
            std::vector<double> ms;
            for (int i = 0; i < 6; ++i) {
                PPixHand hand = nullptr;
                const Clock::time_point t0 = Clock::now();
                if (creator->CreatePPix &&
                    creator->CreatePPix(&hand, PrPPixBufferAccess_ReadWrite, PrPixelFormat_BGRA_4444_32f, c->requestW,
                                        c->requestH, false, 0, 1, 1) == suiteError_NoError &&
                    hand) {
                    ppix->Dispose(hand);
                }
                ms.push_back(msSince(t0));
            }
            std::printf("      %dx%d: %.1f ms per frame\n", c->requestW, c->requestH, median(tail(ms, 1)));
        }
        harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion);
    }
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// ===========================================================================
//  Part B - the importer's pipeline, stage by stage
// ===========================================================================

/// Per-frame stage samples for one configuration.
struct StageSamples {
    std::vector<double> decode, seam, gain, build, upload, gpu, render, alloc, copy32f, copy8u;
};

void printStageRow(const char* stage, const std::vector<double>& v, const char* note) {
    std::printf("      %-26s %9.1f %9.1f   %s\n", stage, v.empty() ? 0.0 : v.front(), median(warm(v)),
                note);
}

/// Bytes the CUDA renderer uploads for one lens plane pair (luma + chroma),
/// and a pageable host-to-device copy of exactly that, so the "upload" share
/// of the stitch stage is measured rather than guessed.
double timeUpload(const osv::render::RenderJob& job, void* devScratch, std::size_t devBytes) {
    const Clock::time_point t0 = Clock::now();
    for (const OsvPlane& p : job.planes) {
        if (!p.y || p.strideY <= 0 || p.h <= 0) {
            continue;
        }
        const std::size_t yBytes = static_cast<std::size_t>(p.strideY) * sizeof(std::uint16_t) * p.h;
        const std::size_t cRows = static_cast<std::size_t>(p.ch);
        const std::size_t cBytes = static_cast<std::size_t>(p.strideC) * sizeof(std::uint16_t) * cRows;
        // Interleaved CbCr is ONE plane of strideC * ch; planar Cb / Cr are
        // two.  Mirror CudaRenderer::uploadPlane's three-or-two copies.
        const std::size_t total = yBytes + (p.chromaInterleaved ? cBytes : 2 * cBytes);
        if (total > devBytes) {
            continue;
        }
        cudaMemcpy(devScratch, p.y, yBytes, cudaMemcpyHostToDevice);
        cudaMemcpy(static_cast<char*>(devScratch) + yBytes, p.u, cBytes, cudaMemcpyHostToDevice);
        if (!p.chromaInterleaved) {
            cudaMemcpy(static_cast<char*>(devScratch) + yBytes + cBytes, p.v, cBytes, cudaMemcpyHostToDevice);
        }
    }
    cudaDeviceSynchronize();
    return msSince(t0);
}

void partB(const Options& o) {
    std::printf("\n=== B. The importer pipeline replayed stage by stage (osvtool Pipeline, CUDA) ===\n");

    // ---- one-time setup, timed ------------------------------------------------
    // The importer does the equivalent of this in imOpenFile8 (parse, rig) and
    // on its first imGetSourceVideo (decoder open, CUDA renderer lease).
    osvtool::PipelineOptions po;
    po.input = o.clip;
    po.stab = "horizon";  // loads the attitude track, as the default prefs do
    po.hw = "none";       // the importer decodes in software (ensureReader)
    po.device = "cuda";
    std::unique_ptr<osvtool::Pipeline> pipe;
    const double openMs = timeMs([&] {
        auto opened = osvtool::Pipeline::open(po, /*needRenderer=*/false);
        if (opened.ok()) {
            pipe = std::move(opened).value();
        } else {
            std::printf("    Pipeline::open failed: %s\n", opened.error().message.c_str());
        }
    });
    if (!pipe) {
        return;
    }
    std::unique_ptr<osv::render::IRenderer> renderer;
    std::string rendererName;
    const double rendererMs = timeMs([&] {
        auto r = osv::render::makeRenderer("cuda", *pipe->pool, &rendererName);
        if (r.ok()) {
            renderer = std::move(r).value();
        }
    });
    auto* cuda = dynamic_cast<osv::render::CudaRenderer*>(renderer.get());
    if (!cuda) {
        std::printf("    no CUDA renderer on this machine; part B needs one\n");
        return;
    }
    std::printf("    one-time: open clip + metadata + rig + attitude + decoders %.1f ms, CUDA renderer %.1f ms\n",
                openMs, rendererMs);

    // ---- anatomy of the importer's FIRST frame ---------------------------------
    // The importer parses the file and builds the rig in imOpenFile8, but
    // defers three things to the first imGetSourceVideo: the decoders
    // (ensureReader), the attitude track (rebuildStabilization) and, per
    // process, the renderer.  Each is timed here on a FRESH object, so the
    // numbers are cold exactly as the importer's frame 0 is.
    {
        osv::video::DecoderOptions decOpt;  // the importer's: software, all threads, host frames
        decOpt.hw = osv::video::HwAccel::None;
        decOpt.threads = 0;
        decOpt.keepOnDevice = false;
        std::unique_ptr<osv::video::DualStreamReader> fresh;
        const double readerOpenMs = timeMs([&] {
            auto r = osv::video::DualStreamReader::open(o.clip, pipe->format, decOpt);
            if (r.ok()) {
                fresh = std::make_unique<osv::video::DualStreamReader>(std::move(r).value());
            }
        });
        if (!fresh) {
            std::printf("    could not open a second reader\n");
            return;
        }
        osv::Result<osv::video::FramePair> first = osv::Error{osv::ErrorCode::Internal, "not read"};
        const double firstReadMs = timeMs([&] { first = fresh->read(0); });

        // The attitude build rebuildStabilization() performs for any mode
        // other than Off (convention probe + full-clip AttitudeTrack).
        const double attitudeMs = timeMs([&] {
            osv::geom::AttitudeTrack::Options attOpt;
            const osv::geom::ConventionScore best = osv::geom::ConventionProbe::best(pipe->track);
            if (best.framesUsed > 0 && best.meanGravityAngleDeg < 15.0) {
                attOpt.conv = best.conv;
            }
            auto built = osv::geom::AttitudeTrack::build(pipe->track, attOpt);
            (void)built;
        });

        // The first render() on a fresh renderer allocates the device output,
        // the plane buffers and a pinned staging area the size of the frame.
        double coldRenderMs = 0.0;
        double warmRenderMs = 0.0;
        if (first.ok()) {
            auto fresh2 = osv::render::makeRenderer("cuda", *pipe->pool, nullptr);
            osv::render::RenderParamsBuilder builder;
            builder.rig(pipe->rig).color(pipe->color).blend(pipe->blendParams, true).alphaCoverage(true);
            osv::geom::EquirectMap map;
            map.layout = osv::geom::EquirectLayout::Standard;
            map.w = 6000;
            map.h = 3000;
            builder.equirect(map);
            auto job = builder.build(first.value());
            if (fresh2.ok() && job.ok()) {
                coldRenderMs = timeMs([&] { (void)fresh2.value()->render(job.value()); });
                warmRenderMs = timeMs([&] { (void)fresh2.value()->render(job.value()); });
            }
        }
        std::printf("    first-frame anatomy (cold, each on a fresh object):\n");
        std::printf("      decoder open (2x HEVC)          %8.1f ms\n", readerOpenMs);
        std::printf("      first decode, frame 0           %8.1f ms\n", firstReadMs);
        std::printf("      attitude build (horizon/full)   %8.1f ms\n", attitudeMs);
        std::printf("      first render() 6000x3000        %8.1f ms   (second call: %.1f ms)\n", coldRenderMs,
                    warmRenderMs);

        // ---- sustained decode cost, nothing else running ------------------------
        // Inside the pipeline a warm read() returns in well under a millisecond,
        // because libavcodec's frame threads decode AHEAD while the caller is
        // busy with the analyses and the render.  That hides the decoder's real
        // cost rather than removing it.  Reading back to back with no other
        // work shows the rate the decoder itself can sustain, which is the
        // floor under every other number here.
        //
        // It has to run long.  Frame threading keeps roughly one frame in
        // flight per decoder thread, so the first read() primes dozens of
        // frames at once and the next dozens come back already decoded; a
        // short loop only drains that buffer and reports a meaningless
        // couple of milliseconds.  The steady-state rate is taken from the
        // SECOND half of a long run, after the primed frames are used up,
        // as total time over frame count (a per-read median would still be
        // skewed by the bursty way frame threads hand frames back).
        constexpr int kDecodeRun = 160;
        const std::uint32_t available = fresh->frameCount();
        const int run = std::min<int>(kDecodeRun, static_cast<int>(available) - 1);
        std::vector<double> decodeOnly;
        for (int i = 1; i <= run; ++i) {
            decodeOnly.push_back(timeMs([&] { (void)fresh->read(static_cast<std::uint32_t>(i)); }));
        }
        if (run >= 4) {
            const std::size_t half = decodeOnly.size() / 2;
            double secondHalf = 0.0;
            for (std::size_t k = half; k < decodeOnly.size(); ++k) {
                secondHalf += decodeOnly[k];
            }
            const double perFrame = secondHalf / static_cast<double>(decodeOnly.size() - half);
            std::printf("      decode alone, steady state      %8.1f ms per frame (%.1f fps; frames %zu-%d)\n",
                        perFrame, perFrame > 0.0 ? 1000.0 / perFrame : 0.0, half + 1, run);
        }
    }

    // Device scratch for the upload micro-measurement: two 3000x3000 P010
    // lenses are ~54 MB; 128 MB covers every stream this camera writes.
    constexpr std::size_t kScratchBytes = 128u << 20;
    void* devScratch = nullptr;
    if (cudaMalloc(&devScratch, kScratchBytes) != cudaSuccess) {
        devScratch = nullptr;
    }

    // The configurations of part A that are genuinely different pipelines
    // (the 3000x1500 request is the native pipeline at another output size,
    // which part A already shows; it is repeated here for the breakdown).
    for (const ImporterConfig& c : kImporterConfigs) {
        const int outW = c.requestW;
        const int outH = c.requestH;
        StageSamples s;

        // The PPix the importer copies into is host memory Premiere owns and
        // recycles; one reused buffer models that (no first-touch faults per
        // frame).  Sized for 32f, which also holds the 8u variant.
        std::vector<char> ppix(static_cast<std::size_t>(outW) * static_cast<std::size_t>(outH) * 16u);
        std::memset(ppix.data(), 0, ppix.size());

        for (int i = 0; i < o.frames; ++i) {
            const auto index = static_cast<std::uint32_t>(i);

            // -- decode ----------------------------------------------------------
            osv::Result<osv::video::FramePair> pair = osv::Error{osv::ErrorCode::Internal, "not read"};
            s.decode.push_back(timeMs([&] { pair = pipe->reader->read(index); }));
            if (!pair.ok()) {
                std::printf("    decode of frame %d failed: %s\n", i, pair.error().message.c_str());
                break;
            }

            osv::render::RenderParamsBuilder builder;
            builder.rig(pipe->rig).color(pipe->color).blend(pipe->blendParams, true).alphaCoverage(true);

            // -- per-frame analyses (renderFrame runs both when not a draft) ----
            if (c.analyses) {
                osv::render::SeamSearchParams sp;
                std::vector<float> shift;
                s.seam.push_back(timeMs([&] {
                    auto profile = osv::render::searchSeam(pipe->rig, pair.value(), pipe->blendParams, sp, *pipe->pool);
                    if (profile.ok()) {
                        shift = profile.value().shiftDeg;
                    }
                }));
                if (!shift.empty()) {
                    builder.seam(shift);
                }
                osv::render::BandParams band;
                s.gain.push_back(timeMs([&] {
                    auto g = osv::render::estimateGain(pipe->rig, pair.value(), pipe->blendParams, band, *pipe->pool);
                    if (g.ok()) {
                        builder.gain(g.value().gain[0], g.value().gain[1]);
                    }
                }));
                builder.stabilization(pipe->stabilizationFor(index));
            }

            // -- job build ---------------------------------------------------------
            osv::geom::EquirectMap map;
            map.layout = osv::geom::EquirectLayout::Standard;
            map.w = outW;
            map.h = outH;
            builder.equirect(map);
            osv::Result<osv::render::RenderJob> job = osv::Error{osv::ErrorCode::Internal, "not built"};
            s.build.push_back(timeMs([&] { job = builder.build(pair.value()); }));
            if (!job.ok()) {
                std::printf("    job build failed: %s\n", job.error().message.c_str());
                break;
            }

            // -- the stitch, split three ways --------------------------------------
            // render() is what the importer calls: upload + kernel + readback
            // into pinned staging + memcpy into a freshly allocated, zeroed
            // ImageRGBAf.  renderToDevice() is upload + kernel only.  The
            // difference is the readback-and-allocate tail, and a separate
            // ImageRGBAf::create shows how much of that tail is just zeroing
            // a vector that is about to be overwritten.
            //
            // render() runs FIRST so frame 0 reports what the importer's frame
            // 0 pays (device buffers and the pinned staging area allocated on
            // first use).
            osv::Result<osv::render::ImageRGBAf> image = osv::Error{osv::ErrorCode::Internal, "not rendered"};
            s.render.push_back(timeMs([&] { image = renderer->render(job.value()); }));
            if (!image.ok()) {
                std::printf("    render failed: %s\n", image.error().message.c_str());
                break;
            }
            s.gpu.push_back(timeMs([&] { (void)cuda->renderToDevice(job.value(), nullptr); }));
            if (devScratch) {
                s.upload.push_back(timeUpload(job.value(), devScratch, kScratchBytes));
            }
            s.alloc.push_back(timeMs([&] {
                auto scratch = osv::render::ImageRGBAf::create(static_cast<std::uint32_t>(outW),
                                                                static_cast<std::uint32_t>(outH));
                (void)scratch;
            }));

            // -- the PPix copy (RGBA float -> host BGRA, bottom-up rows) -----------
            osv::premiere::pixelcopy::HostFrame dst;
            dst.base = ppix.data();
            dst.rowBytes = outW * 16;
            dst.width = static_cast<std::uint32_t>(outW);
            dst.height = static_cast<std::uint32_t>(outH);
            s.copy32f.push_back(timeMs([&] {
                (void)osv::premiere::pixelcopy::rgbaToHostBgra32f(image.value(), dst, pipe->pool.get());
            }));
            dst.rowBytes = outW * 4;
            s.copy8u.push_back(timeMs([&] {
                (void)osv::premiere::pixelcopy::rgbaToHostBgra8u(image.value(), dst, pipe->pool.get());
            }));
        }

        // ---- report ----------------------------------------------------------------
        std::printf("\n    %s  (%dx%d out)\n", c.name, outW, outH);
        std::printf("      %-26s %9s %9s\n", "stage", "frame0", "warm med");
        printStageRow("decode (2x HEVC, sw)", s.decode, "");
        if (c.analyses) {
            printStageRow("seam search (CPU)", s.seam, "");
            printStageRow("gain estimate (CPU)", s.gain, "");
        }
        printStageRow("job build", s.build, "");
        printStageRow("render() total", s.render, "= upload + kernel + readback + alloc/memcpy");
        printStageRow("  upload+kernel", s.gpu, "renderToDevice()");
        printStageRow("    of which H2D upload", s.upload, "pageable cudaMemcpy of the planes");
        std::vector<double> tailMs;
        for (std::size_t k = 0; k < s.render.size() && k < s.gpu.size(); ++k) {
            tailMs.push_back(s.render[k] - s.gpu[k]);
        }
        printStageRow("  readback+alloc+memcpy", tailMs, "render() - renderToDevice()");
        printStageRow("    of which alloc+zero", s.alloc, "ImageRGBAf::create alone");
        printStageRow("PPix copy 32f", s.copy32f, "pixelcopy::rgbaToHostBgra32f");
        printStageRow("PPix copy 8u (alt.)", s.copy8u, "not in the sum");
        // The sum the importer pays per frame, to hold against part A.
        const double sum = median(warm(s.decode)) + median(warm(s.seam)) +
                           median(warm(s.gain)) + median(warm(s.build)) +
                           median(warm(s.render)) + median(warm(s.copy32f));
        std::printf("      %-26s %9s %9.1f   compare with part A's warm median\n", "SUM (warm)", "", sum);
    }
    if (devScratch) {
        cudaFree(devScratch);
    }
}

// ===========================================================================
//  Part C - the effect's GPU kernel, device to device
// ===========================================================================

void partC(const Options&) {
    std::printf("\n=== C. Reframe effect kernel on the GPU (device-resident input, as in Premiere) ===\n");
    std::printf("    Settings: the default 'Wide' look (FOV 120, distortion 15%%), RGBA 32f.\n\n");
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices <= 0) {
        std::printf("    no CUDA device\n");
        return;
    }

    struct Size {
        int w, h;
    };
    const Size sources[] = {{6000, 3000}, {3000, 1500}, {2560, 1280}};
    const Size outputs[] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};

    cudaStream_t stream = nullptr;
    cudaStreamCreate(&stream);
    cudaEvent_t start = nullptr;
    cudaEvent_t stop = nullptr;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    std::printf("    %-12s", "src \\ out");
    for (const Size& out : outputs) {
        std::printf("  %5dx%-5d", out.w, out.h);
    }
    std::printf("   (ms per frame, mean of 50 launches)\n");

    for (const Size& src : sources) {
        void* devSrc = nullptr;
        std::size_t srcPitch = 0;
        if (cudaMallocPitch(&devSrc, &srcPitch, static_cast<std::size_t>(src.w) * 16u,
                            static_cast<std::size_t>(src.h)) != cudaSuccess) {
            std::printf("    cannot allocate a %dx%d source\n", src.w, src.h);
            continue;
        }
        // Content does not change the cost (the fetch pattern is geometry
        // only), so a constant fill is enough and keeps the run fast.
        cudaMemset2D(devSrc, srcPitch, 0x3F, static_cast<std::size_t>(src.w) * 16u, static_cast<std::size_t>(src.h));

        std::printf("    %5dx%-6d", src.w, src.h);
        for (const Size& out : outputs) {
            void* devOut = nullptr;
            std::size_t outPitch = 0;
            if (cudaMallocPitch(&devOut, &outPitch, static_cast<std::size_t>(out.w) * 16u,
                                static_cast<std::size_t>(out.h)) != cudaSuccess) {
                std::printf("  %11s", "alloc fail");
                continue;
            }
            // buildParams() only reads the view's size, pitch and layout and
            // computes the row-0 address from `base`; with a top-down,
            // positive-pitch view that address IS `base`, so describing the
            // device allocation directly yields a device row-0 pointer.
            osv::reframe::ConstFrameView view;
            view.base = devSrc;
            view.rowBytes = static_cast<std::int32_t>(srcPitch);
            view.width = src.w;
            view.height = src.h;
            view.layout = osv::reframe::PixelLayout::Bgra32f;
            view.topDown = true;
            const osv::reframe::Settings settings;  // defaults = the Wide look
            const osv::reframe::KernelSetup setup =
                osv::reframe::buildParams(settings, view, out.w, out.h, osv::reframe::SizePx{});
            if (!setup.valid) {
                std::printf("  %11s", "bad setup");
                cudaFree(devOut);
                continue;
            }
            const int outPitchFloats = static_cast<int>(outPitch / sizeof(float));
            // Warm up (module load, clocks), then time 50 back-to-back launches.
            //
            // The first launch is CHECKED, and its output read back: a launch
            // that fails returns immediately, and a sub-millisecond timing
            // is only believable if the kernel demonstrably wrote the frame.
            // The source is filled with bytes 0x3F, i.e. every float channel
            // is 0x3F3F3F3F = 0.7470..., and a reframe of a constant sphere
            // must reproduce that constant at the centre of the output.
            const cudaError_t launchErr = osv::render::osvCudaLaunchReframeEquirect(
                setup.params, setup.source, setup.sourceRow0, static_cast<float*>(devOut), outPitchFloats, stream);
            float centre[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const std::size_t centreOffset =
                static_cast<std::size_t>(out.h / 2) * outPitch + static_cast<std::size_t>(out.w / 2) * 16u;
            cudaStreamSynchronize(stream);
            cudaMemcpy(centre, static_cast<const char*>(devOut) + centreOffset, sizeof(centre),
                       cudaMemcpyDeviceToHost);
            if (launchErr != cudaSuccess || !(centre[1] > 0.74f && centre[1] < 0.75f)) {
                std::printf("  %11s", launchErr != cudaSuccess ? "launch err" : "bad pixel");
                cudaFree(devOut);
                continue;
            }
            for (int k = 0; k < 4; ++k) {
                osv::render::osvCudaLaunchReframeEquirect(setup.params, setup.source, setup.sourceRow0,
                                                          static_cast<float*>(devOut), outPitchFloats, stream);
            }
            cudaEventRecord(start, stream);
            constexpr int kLaunches = 50;
            for (int k = 0; k < kLaunches; ++k) {
                osv::render::osvCudaLaunchReframeEquirect(setup.params, setup.source, setup.sourceRow0,
                                                          static_cast<float*>(devOut), outPitchFloats, stream);
            }
            cudaEventRecord(stop, stream);
            cudaEventSynchronize(stop);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, start, stop);
            std::printf("  %11.3f", static_cast<double>(ms) / kLaunches);
            cudaFree(devOut);
        }
        std::printf("\n");
        cudaFree(devSrc);
    }
    cudaEventDestroy(start);
    cudaEventDestroy(stop);
    cudaStreamDestroy(stream);
}

// ===========================================================================
//  Part D - the effect's CPU path
// ===========================================================================

void partD(const Options&) {
    std::printf("\n=== D. Reframe effect on the CPU (renderCpu, all threads, 32f) ===\n\n");
    osv::ThreadPool pool(0);
    struct Size {
        int w, h;
    };
    const Size sources[] = {{6000, 3000}, {2560, 1280}};
    const Size out{2560, 1440};
    for (const Size& src : sources) {
        // A constant source: the CPU cost, like the GPU one, is the geometry
        // and the fetch pattern, not the values.
        std::vector<float> srcPixels(static_cast<std::size_t>(src.w) * src.h * 4u, 0.5f);
        std::vector<float> dstPixels(static_cast<std::size_t>(out.w) * out.h * 4u, 0.0f);
        osv::reframe::ConstFrameView view;
        view.base = srcPixels.data();
        view.rowBytes = src.w * 16;
        view.width = src.w;
        view.height = src.h;
        view.layout = osv::reframe::PixelLayout::Bgra32f;
        view.topDown = true;
        osv::reframe::FrameView dst;
        dst.base = dstPixels.data();
        dst.rowBytes = out.w * 16;
        dst.width = out.w;
        dst.height = out.h;
        dst.layout = osv::reframe::PixelLayout::Bgra32f;
        dst.topDown = true;
        const osv::reframe::Settings settings;
        const osv::reframe::KernelSetup setup =
            osv::reframe::buildParams(settings, view, out.w, out.h, osv::reframe::SizePx{});
        if (!setup.valid) {
            std::printf("    bad setup for %dx%d\n", src.w, src.h);
            continue;
        }
        std::vector<double> ms;
        for (int k = 0; k < 6; ++k) {
            ms.push_back(timeMs([&] { (void)osv::reframe::renderCpu(setup, view, dst, &pool); }));
        }
        std::printf("    %dx%d -> %dx%d: median %.1f ms (min %.1f)\n", src.w, src.h, out.w, out.h,
                    median(tail(ms, 1)), minimum(tail(ms, 1)));
    }
}

// ===========================================================================
//  Part P - parking the playhead on a frame (the Stopped path)
// ===========================================================================

/// Where the playhead lands, in the order it lands there.  Deliberately
/// scattered across the clip's GOPs (the sample has sync samples at 1 and 61)
/// and never sequential, so the decoder cannot coast on frames it already
/// decoded ahead - this is clicking around a timeline, not playing it.
constexpr std::uint32_t kParkFrames[] = {27, 45, 10, 63, 32, 5, 50, 18, 40, 2, 58, 23};

/// One parking configuration: the prefs, and what the host asks for.
struct ParkConfig {
    const char* name;
    bool parallax;
    bool analyses;
};

void partP(const Options& o) {
    std::printf("\n=== P. Parking on a frame: imGetSourceVideo, intent Stopped, BGRA_4444_8u, 6000x3000 ===\n");
    std::printf("    Exactly what Premiere logged when the playhead is dropped somewhere new:\n");
    std::printf("    a full-resolution, non-draft, EXACT request for a frame nowhere near the last one.\n\n");

    ImporterHarness harness;
    if (!harness.loaded()) {
        std::printf("    cannot load the importer: %s\n", harness.loadError().c_str());
        return;
    }
    const void* rawSuite = nullptr;
    if (harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &rawSuite) != kSPNoError ||
        !rawSuite) {
        std::printf("    cannot acquire the PPix suite from the mock host\n");
        return;
    }
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(rawSuite);

    // Classical flow, as inside Premiere today (the neural runtime is not
    // staged there), so the numbers describe what the user is waiting for.
    const ParkConfig configs[] = {
        {"defaults (parallax on)  ", true, true},
        {"parallax off            ", false, true},
        {"all analyses off        ", false, false},
    };

    csSDK_int32 importerId = 500;
    for (const ParkConfig& c : configs) {
        harness.host().clearCache();
        auto clip = harness.openClip(o.clip, importerId++);
        if (!clip.open()) {
            std::printf("    %s open failed (%d)\n", c.name, static_cast<int>(clip.openResult()));
            continue;
        }
        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
        prefs.parallax = static_cast<std::uint8_t>(c.parallax ? osv::premiere::PrefsParallax::On
                                                              : osv::premiere::PrefsParallax::Off);
        prefs.flowBackend = static_cast<std::uint8_t>(osv::premiere::PrefsFlowBackend::Classical);
        prefs.seamSearch = c.analyses ? 1 : 0;
        prefs.gainMatch = c.analyses ? 1 : 0;
        prefs.stabilization =
            static_cast<std::uint8_t>(c.analyses ? PrefsStabilization::HorizonLock : PrefsStabilization::Off);

        imFileInfoRec8 info{};
        if (harness.getInfo8(clip, info, &prefs) != imNoErr || info.vidScale <= 0 || info.vidSampleSize <= 0) {
            std::printf("    %s imGetInfo8 failed\n", c.name);
            continue;
        }
        const PrTime ticksPerFrame = kTicksPerSecond * info.vidSampleSize / info.vidScale;

        std::printf("    %s", c.name);
        std::vector<double> ms;
        for (const std::uint32_t frame : kParkFrames) {
            ImporterHarness::SourceVideoRequest request;
            request.frameTime = ticksPerFrame * static_cast<PrTime>(frame);
            request.format = PrPixelFormat_BGRA_4444_8u;
            request.width = 6000;
            request.height = 3000;
            request.intent = imRenderIntent_Stopped;
            request.playbackRatio = 1.0;
            // Each park is a NEW frame; the host's cache must not serve it.
            harness.host().clearCache();
            PPixHand hand = nullptr;
            const Clock::time_point t0 = Clock::now();
            const csSDK_int32 err = harness.getSourceVideo(clip, request, prefs, hand);
            ms.push_back(msSince(t0));
            if (err != imNoErr || !hand) {
                std::printf("  frame %u failed (%d)", frame, static_cast<int>(err));
                break;
            }
            ppix->Dispose(hand);
            std::printf(" %u:%.0f", frame, ms.back());
        }
        // The first park also opens the decoders; the rest are the steady
        // cost of dropping the playhead somewhere new.
        std::printf("\n      -> first %.0f ms, then median %.0f ms, worst %.0f ms\n", ms.empty() ? 0.0 : ms.front(),
                    median(tail(ms, 1)), ms.size() > 1 ? *std::max_element(ms.begin() + 1, ms.end()) : 0.0);
        clip.close();
    }

    // ---- the decoder alone ------------------------------------------------------
    // A random access decodes forward from the previous sync sample, so its
    // cost grows with the distance into the GOP.  Timed on its own reader so
    // nothing else competes for the cores, once per decode back-end: software
    // (what the importer uses today) and the two hardware paths, each copying
    // its frames back to host memory exactly as the importer would need.
    auto parsed = osvtool::Pipeline::open(
        [&] {
            osvtool::PipelineOptions po;
            po.input = o.clip;
            po.hw = "none";
            po.device = "cpu";
            return po;
        }(),
        /*needRenderer=*/false);
    if (!parsed.ok()) {
        std::printf("    Pipeline::open failed: %s\n", parsed.error().message.c_str());
    } else {
        std::printf("\n    decode alone (both lenses, host frames), same landing order:\n");
        for (const osv::video::HwAccel hw :
             {osv::video::HwAccel::None, osv::video::HwAccel::Cuda, osv::video::HwAccel::D3D11VA}) {
            osv::video::DecoderOptions decOpt;
            decOpt.hw = hw;
            decOpt.threads = 0;
            decOpt.keepOnDevice = false;
            std::printf("      %-8s", osv::video::hwAccelName(hw));
            osv::Result<osv::video::DualStreamReader> reader = osv::Error{osv::ErrorCode::Internal, "not opened"};
            const double openMs =
                timeMs([&] { reader = osv::video::DualStreamReader::open(o.clip, parsed.value()->format, decOpt); });
            if (!reader.ok()) {
                std::printf(" open failed: %s\n", reader.error().message.c_str());
                continue;
            }
            std::printf(" open %.0f ms |", openMs);
            std::vector<double> ms;
            for (const std::uint32_t frame : kParkFrames) {
                osv::Result<osv::video::FramePair> pair = osv::Error{osv::ErrorCode::Internal, "not read"};
                ms.push_back(timeMs([&] { pair = reader.value().read(frame); }));
                if (!pair.ok()) {
                    std::printf(" %u:FAILED (%s)", frame, pair.error().message.c_str());
                    break;
                }
                std::printf(" %u:%.0f", frame, ms.back());
            }
            std::printf("\n               -> median %.0f ms, worst %.0f ms\n", median(ms),
                        ms.empty() ? 0.0 : *std::max_element(ms.begin(), ms.end()));
        }
    }
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}


// ===========================================================================
//  Part F - the importer's own frame by format and by path
// ===========================================================================

/// Median of `v` without its first sample (the first request of a clip pays
/// the decoder open and the pinned-memory setup).
double warmMedian(const std::vector<double>& v) { return median(tail(v, 1)); }

/// One "frame-cost" line of the importer's debug log (ImporterInstance.cpp,
/// renderFrameToHost / renderFrameOnGpu): key=value fields.
struct FrameCost {
    std::string path;                  ///< "gpu" or "host".
    std::map<std::string, double> ms;  ///< total, decode, job, render, readback, copy, ...
};

/// The importer's log in this process's isolated LOCALAPPDATA.
std::filesystem::path importerLogPath() {
    wchar_t buffer[32768] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path(buffer) / L"OpenOSV" / L"OpenOSVImporter.log";
}

/// Every "frame-cost" line appended to the log since byte `from`; `from`
/// advances to the end.
std::vector<FrameCost> readFrameCosts(const std::filesystem::path& log, std::uintmax_t& from) {
    std::vector<FrameCost> out;
    std::ifstream in(log, std::ios::binary);
    if (!in) {
        return out;
    }
    in.seekg(0, std::ios::end);
    const std::uintmax_t size = static_cast<std::uintmax_t>(in.tellg());
    if (size < from) {
        from = 0;  // rotated
    }
    in.seekg(static_cast<std::streamoff>(from));
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t at = line.find("frame-cost ");
        if (at == std::string::npos) {
            continue;
        }
        FrameCost cost;
        std::size_t pos = at + 11;
        while (pos < line.size()) {
            const std::size_t end = line.find(' ', pos);
            const std::string field = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            const std::size_t eq = field.find('=');
            if (eq != std::string::npos) {
                const std::string key = field.substr(0, eq);
                const std::string value = field.substr(eq + 1);
                if (key == "path") {
                    cost.path = value;
                } else {
                    char* stop = nullptr;
                    const double v = std::strtod(value.c_str(), &stop);
                    if (stop && stop != value.c_str()) {
                        cost.ms[key] = v;
                    }
                }
            }
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
        out.push_back(std::move(cost));
    }
    from = size;
    return out;
}

/// Median of one stage over a set of frame costs (first one skipped: cold).
double stageMedian(const std::vector<FrameCost>& costs, const char* stage) {
    std::vector<double> v;
    for (const FrameCost& c : costs) {
        const auto it = c.ms.find(stage);
        if (it != c.ms.end()) {
            v.push_back(it->second);
        }
    }
    return warmMedian(v);
}

void partF(const Options& o) {
    std::printf("\n=== F. The importer's own frame: pixel format x frame path (native 6000x3000, analyses off) ===\n");
    std::printf("    GPU  = NVDEC into VRAM, stitch in place, 16u/8u packed on the GPU, pinned banded readback\n");
    std::printf("    host = D3D11VA decode to host, upload, stitch, readback, float image, CPU convert\n");
    std::printf("    park = %zu scattered Stopped requests (host cache cleared), play = sequential Playing\n",
                std::size(kParkFrames));
    std::printf("    importer ms = the importer's own time per frame from its frame-cost log lines, i.e. WITHOUT the\n");
    std::printf("    mock host's PPix allocation (it zero-fills every fresh 288 MB frame; Premiere recycles them)\n\n");

    // The importer reads its log level when it initialises, i.e. when the
    // harness below loads it: debug is what writes the frame-cost lines.
    ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "debug");
    const std::filesystem::path log = importerLogPath();

    ImporterHarness harness;
    if (!harness.loaded()) {
        std::printf("    cannot load the importer: %s\n", harness.loadError().c_str());
        return;
    }
    const void* rawSuite = nullptr;
    if (harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &rawSuite) != kSPNoError ||
        !rawSuite) {
        std::printf("    cannot acquire the PPix suite from the mock host\n");
        return;
    }
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(rawSuite);

    // Analyses off: they are per-bucket caches shared by both paths, and
    // what is compared here is the frame path itself.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;
    prefs.parallax = static_cast<std::uint8_t>(osv::premiere::PrefsParallax::Off);

    struct Format {
        const char* name;
        PrPixelFormat format;
    };
    const Format formats[] = {{"32f", PrPixelFormat_BGRA_4444_32f},
                              {"16u", PrPixelFormat_BGRA_4444_16u},
                              {"8u", PrPixelFormat_BGRA_4444_8u}};
    const int playFrames = std::min(o.frames, 60);

    std::printf("    %-4s %-3s | %9s %9s | %9s %9s | %s\n", "path", "fmt", "park wall", "importer", "play wall",
                "importer", "importer stages while playing (median ms)");
    std::uintmax_t logOffset = 0;
    (void)readFrameCosts(log, logOffset);  // skip whatever is there already
    csSDK_int32 importerId = 900;
    for (const bool gpu : {true, false}) {
        // Chosen by each clip at its first frame, so it is set before the
        // clip below renders anything.
        ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", gpu ? "" : "1");
        for (const Format& f : formats) {
            auto clip = harness.openClip(o.clip, importerId++);
            if (!clip.open()) {
                std::printf("    open failed (%d)\n", static_cast<int>(clip.openResult()));
                continue;
            }
            imFileInfoRec8 info{};
            if (harness.getInfo8(clip, info, &prefs) != imNoErr || info.vidScale <= 0 || info.vidSampleSize <= 0) {
                std::printf("    imGetInfo8 failed\n");
                continue;
            }
            const PrTime ticksPerFrame = kTicksPerSecond * info.vidSampleSize / info.vidScale;
            auto request = [&](std::uint32_t frame, imRenderIntent intent) {
                ImporterHarness::SourceVideoRequest r;
                r.frameTime = ticksPerFrame * static_cast<PrTime>(frame);
                r.format = f.format;
                r.width = 6000;
                r.height = 3000;
                r.intent = intent;
                r.playbackRatio = 1.0;
                PPixHand hand = nullptr;
                const Clock::time_point t0 = Clock::now();
                const csSDK_int32 err = harness.getSourceVideo(clip, r, prefs, hand);
                const double ms = msSince(t0);
                if (err != imNoErr || !hand) {
                    return -1.0;
                }
                ppix->Dispose(hand);
                return ms;
            };

            // Parking: scattered, cache cleared, the first landing opens everything.
            std::vector<double> park;
            for (const std::uint32_t frame : kParkFrames) {
                harness.host().clearCache();
                park.push_back(request(frame, imRenderIntent_Stopped));
            }
            const std::vector<FrameCost> parkCosts = readFrameCosts(log, logOffset);
            // Playing: sequential from frame 0 (decode-ahead engages).
            std::vector<double> play;
            for (int i = 0; i < playFrames; ++i) {
                harness.host().clearCache();
                play.push_back(request(static_cast<std::uint32_t>(i), imRenderIntent_Playing));
            }
            const std::vector<FrameCost> playCosts = readFrameCosts(log, logOffset);
            clip.close();

            const bool failed = std::any_of(park.begin(), park.end(), [](double v) { return v < 0.0; }) ||
                                std::any_of(play.begin(), play.end(), [](double v) { return v < 0.0; });
            if (failed) {
                std::printf("    %-4s %-3s | a request failed\n", gpu ? "GPU" : "host", f.name);
                continue;
            }
            // The stage split while playing, in the path's own terms.
            char stages[256] = {};
            if (playCosts.empty()) {
                std::snprintf(stages, sizeof(stages), "(no frame-cost lines: run --part F on its own)");
            } else if (gpu) {
                std::snprintf(stages, sizeof(stages), "decode %.1f  job %.1f  render %.1f  readback %.1f",
                              stageMedian(playCosts, "decode"), stageMedian(playCosts, "job"),
                              stageMedian(playCosts, "render"), stageMedian(playCosts, "readback"));
            } else {
                std::snprintf(stages, sizeof(stages), "decode+upload+stitch+readback %.1f  convert %.1f",
                              stageMedian(playCosts, "render"), stageMedian(playCosts, "copy"));
            }
            // Warm play samples only (the second half), like the other parts.
            std::vector<FrameCost> warmPlay(
                playCosts.begin() + static_cast<std::ptrdiff_t>(std::min(playCosts.size(), playCosts.size() / 2)),
                playCosts.end());
            std::printf("    %-4s %-3s | %9.1f %9.1f | %9.1f %9.1f | %s\n", gpu ? "GPU" : "host", f.name,
                        warmMedian(park), stageMedian(parkCosts, "total"), median(warm(play)),
                        stageMedian(warmPlay, "total"), stages);
        }
    }
    ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", "");
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// ===========================================================================
//  Part Q - quiet / unquiet and the Source Settings reopen
// ===========================================================================

/// Premiere quiets a clip it is not reading (imQuietFile) and wakes it
/// seconds later, and opens a NEW importer instance of the clip on every
/// Source Settings change.  Both used to rebuild the decoders from nothing.
/// This part times the first frame after each, per frame path, against the
/// same frame on an already-open clip (the floor: decode + stitch + readback
/// with everything warm).
void partQ(const Options& o) {
    std::printf("\n=== Q. Quiet / unquiet and a new instance of the clip: the first frame after each ===\n");
    std::printf("    frame 17, Stopped, BGRA_4444_8u, 6000x3000, analyses off, host cache cleared before every request\n");
    std::printf("    steady   = the same request on a clip that stayed open (the floor)\n");
    std::printf("    unquiet  = imQuietFile, then the request (the importer reopens its decoders)\n");
    std::printf("    new inst = imCloseFile, imOpenFile8 + imGetInfo8 on a new instance, then the request\n\n");

    ImporterHarness harness;
    if (!harness.loaded()) {
        std::printf("    cannot load the importer: %s\n", harness.loadError().c_str());
        return;
    }
    const void* rawSuite = nullptr;
    if (harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &rawSuite) != kSPNoError ||
        !rawSuite) {
        std::printf("    cannot acquire the PPix suite from the mock host\n");
        return;
    }
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(rawSuite);

    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;
    prefs.parallax = static_cast<std::uint8_t>(osv::premiere::PrefsParallax::Off);
    constexpr std::uint32_t kFrame = 17;
    constexpr int kRounds = 6;

    csSDK_int32 importerId = 1200;
    for (const bool gpu : {true, false}) {
        ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", gpu ? "" : "1");
        // One request of frame kFrame on `clip`; -1 on failure.
        auto request = [&](ImporterHarness::ClipHandle& clip, PrTime ticksPerFrame) {
            ImporterHarness::SourceVideoRequest r;
            r.frameTime = ticksPerFrame * static_cast<PrTime>(kFrame);
            r.format = PrPixelFormat_BGRA_4444_8u;
            r.width = 6000;
            r.height = 3000;
            r.intent = imRenderIntent_Stopped;
            r.playbackRatio = 1.0;
            harness.host().clearCache();
            PPixHand hand = nullptr;
            const Clock::time_point t0 = Clock::now();
            const csSDK_int32 err = harness.getSourceVideo(clip, r, prefs, hand);
            const double ms = msSince(t0);
            if (err != imNoErr || !hand) {
                return -1.0;
            }
            ppix->Dispose(hand);
            return ms;
        };
        // Open + imGetInfo8; returns the frame period, 0 on failure.
        auto openInstance = [&](ImporterHarness::ClipHandle& clip) -> PrTime {
            clip = harness.openClip(o.clip, importerId++);
            if (!clip.open()) {
                return 0;
            }
            imFileInfoRec8 info{};
            if (harness.getInfo8(clip, info, &prefs) != imNoErr || info.vidScale <= 0 || info.vidSampleSize <= 0) {
                return 0;
            }
            return kTicksPerSecond * info.vidSampleSize / info.vidScale;
        };

        ImporterHarness::ClipHandle clip;
        const PrTime period = openInstance(clip);
        if (period <= 0) {
            std::printf("    open failed\n");
            continue;
        }
        const double first = request(clip, period);
        std::vector<double> steady, unquiet, fresh;
        for (int r = 0; r < kRounds; ++r) {
            steady.push_back(request(clip, period));
            if (clip.quiet() != imNoErr) {
                std::printf("    imQuietFile failed\n");
                break;
            }
            unquiet.push_back(request(clip, period));
        }
        for (int r = 0; r < kRounds; ++r) {
            clip.close();
            const PrTime p = openInstance(clip);
            if (p <= 0) {
                std::printf("    reopen failed\n");
                break;
            }
            fresh.push_back(request(clip, p));
        }
        clip.close();
        const auto anyFailed = [](const std::vector<double>& v) {
            return std::any_of(v.begin(), v.end(), [](double x) { return x < 0.0; });
        };
        if (first < 0.0 || anyFailed(steady) || anyFailed(unquiet) || anyFailed(fresh)) {
            std::printf("    %-4s a request failed\n", gpu ? "GPU" : "host");
            continue;
        }
        std::printf("    %-4s first open %7.1f | steady %6.1f | unquiet median %6.1f worst %6.1f | new inst median %6.1f "
                    "worst %6.1f  (ms)\n",
                    gpu ? "GPU" : "host", first, median(steady), median(unquiet),
                    *std::max_element(unquiet.begin(), unquiet.end()), median(fresh),
                    *std::max_element(fresh.begin(), fresh.end()));
    }
    ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", "");
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// ===========================================================================
//  Part S - steadiness at the seam [WP-STEADY]
// ===========================================================================
//
// Two measurements of the "slight movement at the seam":
//
//   1. What each per-bucket analysis moves, replayed with the library on the
//      importer's schedule (a measurement at every 8th frame, glided): the
//      parallax grid's displacement change per frame around the nacelle, the
//      carved seam line's, and the seam-shift table's - with the calibration
//      rig and with the lens rotation folded in.  The clip correction
//      (Parallax Grid Steady) is one grid, one seam and one table for every
//      frame, so its row is measured the same way and is zero by
//      construction.
//   2. What the viewer sees: the real importer renders frames 0..64 at
//      6000 x 3000 (Exact, stabilisation off, so the wing is static in the
//      frame), a patch around the nacelle crossing is resampled at the 6K
//      pixel pitch, and DIS measures the motion between consecutive frames.
//      The nacelle is specular and see-through, so its reflections move with
//      the ground whatever the stitch does: the "no seam corrections" row is
//      the scene's own motion, and every other row is compared with it.

namespace steadybench {

/// The nacelle crossing in the polar-axis layout (the research's wing
/// columns, 1880-2040 of 2048, widened by a few degrees), and the rows judged.
constexpr double kWingLon0Deg = 150.5;
constexpr double kWingLon1Deg = 178.7;
constexpr double kPatchLon0Deg = 145.0;
constexpr double kPatchLon1Deg = 180.0;
constexpr double kPatchLatDeg = 6.0;
/// 6K: 6000 px over 360 degrees.
constexpr double kPxPerDeg = 6000.0 / 360.0;

/// Longitude (polar-axis layout, degrees) of grid / seam column `c` of `w`.
double columnLonDeg(std::uint32_t c, std::uint32_t w) {
    return static_cast<double>(c) * 360.0 / static_cast<double>(w) - 180.0;
}

/// Percentile of a copy of `v` (0..100); 0 for an empty set.
double percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const double pos = std::clamp(p, 0.0, 100.0) / 100.0 * static_cast<double>(v.size() - 1);
    const std::size_t i = static_cast<std::size_t>(pos);
    const std::size_t j = std::min(i + 1, v.size() - 1);
    return v[i] + (v[j] - v[i]) * (pos - static_cast<double>(i));
}

/// Per-frame motion samples of one analysis: `perFrame[k]` holds every
/// judged cell's motion between frame k and k + 1, 6K pixels.
struct MotionSeries {
    std::vector<std::vector<double>> perFrame;

    void print(const char* label) const {
        std::vector<double> all;
        std::vector<double> p99s;
        std::size_t over1 = 0;
        double worst = 0.0;
        for (const auto& f : perFrame) {
            all.insert(all.end(), f.begin(), f.end());
            const double fmax = f.empty() ? 0.0 : *std::max_element(f.begin(), f.end());
            worst = std::max(worst, fmax);
            over1 += fmax > 1.0 ? 1u : 0u;
            p99s.push_back(percentile(f, 99.0));
        }
        double mean = 0.0;
        for (const double x : all) {
            mean += x;
        }
        mean = all.empty() ? 0.0 : mean / static_cast<double>(all.size());
        std::printf("      %-46s mean %6.3f  p99 %6.3f  max %6.3f px/frame; frames with a cell > 1 px: %zu/%zu\n",
                    label, mean, percentile(all, 99.0), worst, over1, perFrame.size());
    }
};

/// The grid's per-cell displacement change between two grids of one layout,
/// over the measured rows of the wing columns, in 6K pixels (the angle each
/// lens's picture moves by: the grid is a half correction, and each lens is
/// displaced by exactly it).
std::vector<double> gridMotion(const osv::render::ParallaxWarpGrid& a, const osv::render::ParallaxWarpGrid& b,
                               std::uint32_t decayRows) {
    std::vector<double> out;
    if (!a.valid() || !b.valid() || a.w != b.w || a.h != b.h) {
        return out;
    }
    for (std::uint32_t r = decayRows; r + decayRows < a.h; ++r) {
        for (std::uint32_t c = 0; c < a.w; ++c) {
            const double lon = columnLonDeg(c, a.w);
            if (lon < kWingLon0Deg || lon > kWingLon1Deg) {
                continue;
            }
            const std::size_t i = (static_cast<std::size_t>(r) * a.w + c) * 2u;
            const double du = static_cast<double>(b.uv[i]) - static_cast<double>(a.uv[i]);
            const double dv = static_cast<double>(b.uv[i + 1]) - static_cast<double>(a.uv[i + 1]);
            out.push_back(osv::rad2deg(std::hypot(du, dv)) * kPxPerDeg);
        }
    }
    return out;
}

/// The seam line's per-column latitude change in the wing columns, 6K px.
std::vector<double> seamMotion(const osv::render::BlendSeam& a, const osv::render::BlendSeam& b) {
    std::vector<double> out;
    if (!a.valid() || !b.valid() || a.columns != b.columns) {
        return out;
    }
    for (std::uint32_t c = 0; c < a.columns; ++c) {
        const double lon = columnLonDeg(c, a.columns);
        if (lon < kWingLon0Deg || lon > kWingLon1Deg) {
            continue;
        }
        const double d = static_cast<double>(b.table[c * 2u]) - static_cast<double>(a.table[c * 2u]);
        out.push_back(osv::rad2deg(std::fabs(d)) * kPxPerDeg);
    }
    return out;
}

/// The seam-shift table's per-column change in the wing columns, 6K px (each
/// lens moves by half the full disparity the table stores).
std::vector<double> tableMotion(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<double> out;
    if (a.empty() || a.size() != b.size()) {
        return out;
    }
    for (std::size_t c = 0; c < a.size(); ++c) {
        const double lon = columnLonDeg(static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(a.size()));
        if (lon < kWingLon0Deg || lon > kWingLon1Deg) {
            continue;
        }
        out.push_back(0.5 * std::fabs(static_cast<double>(b[c]) - static_cast<double>(a[c])) * kPxPerDeg);
    }
    return out;
}

/// Mean |g| (full disparity, degrees) over the measured rows of the columns
/// in [lon0, lon1]: how much a grid corrects there.
double gridMeanDeg(const osv::render::ParallaxWarpGrid& g, std::uint32_t decayRows, double lon0, double lon1) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::uint32_t r = decayRows; g.valid() && r + decayRows < g.h; ++r) {
        for (std::uint32_t c = 0; c < g.w; ++c) {
            const double lon = columnLonDeg(c, g.w);
            if (lon < lon0 || lon > lon1) {
                continue;
            }
            const std::size_t i = (static_cast<std::size_t>(r) * g.w + c) * 2u;
            sum += 2.0 * osv::rad2deg(std::hypot(static_cast<double>(g.uv[i]), static_cast<double>(g.uv[i + 1])));
            ++n;
        }
    }
    return n ? sum / static_cast<double>(n) : 0.0;
}

/// Replay the importer's per-bucket schedule for one rig and gate.
void replaySchedule(osvtool::Pipeline& P, const osv::geom::LensRig& rig, double gate, const char* title) {
    namespace r = osv::render;
    const std::uint32_t frames = std::min<std::uint32_t>(P.frameCount(), 65u);
    const std::uint32_t buckets = r::parallaxBucket(frames - 1u) + 1u;
    r::ParallaxWarpParams pw;
    pw.backend = r::FlowBackendKind::Classical;
    pw.requiredImprovement = gate;
    const r::SeamCarveParams carveParams;
    r::SeamSearchParams sp;
    std::vector<std::shared_ptr<r::ParallaxWarpGrid>> grids(buckets);
    std::vector<std::shared_ptr<r::BlendSeam>> seams(buckets);
    std::vector<std::vector<float>> tables(buckets);
    double wingMean = 0.0;
    double groundMean = 0.0;
    std::uint32_t measured = 0;
    for (std::uint32_t b = 0; b < buckets; ++b) {
        auto pair = P.reader->read(b * r::kParallaxBucketFrames);
        if (!pair.ok()) {
            std::printf("      decode of frame %u failed\n", b * r::kParallaxBucketFrames);
            return;
        }
        auto grid = r::buildParallaxWarp(rig, pair.value(), P.blendParams, pw, nullptr, *P.pool);
        r::WarpGridView view;
        r::SeamCorrection correction;
        if (grid.ok()) {
            grids[b] = std::make_shared<r::ParallaxWarpGrid>(std::move(grid).value());
            view.uv = grids[b]->uv.data();
            view.w = grids[b]->w;
            view.h = grids[b]->h;
            view.latMinRad = grids[b]->latMinRad;
            view.latMaxRad = grids[b]->latMaxRad;
            correction.warp = &view;
            wingMean += gridMeanDeg(*grids[b], pw.decayRows, kWingLon0Deg, kWingLon1Deg);
            groundMean += gridMeanDeg(*grids[b], pw.decayRows, 15.0, 118.0);  // the ground, 1110-1700 of 2048
            ++measured;
        }
        auto table = r::searchSeam(rig, pair.value(), P.blendParams, sp, *P.pool);
        if (table.ok()) {
            tables[b] = table.value().shiftDeg;
        }
        // Steered by the previous bucket's seam, as the importer's playback is.
        auto carved = r::carveSeam(rig, pair.value(), P.blendParams, pw.band, correction, carveParams,
                                   b > 0 && seams[b - 1] ? seams[b - 1].get() : nullptr, *P.pool);
        if (carved.ok()) {
            seams[b] = std::make_shared<r::BlendSeam>(std::move(carved).value());
        }
    }
    // ---- the glide, frame by frame --------------------------------------------
    MotionSeries gridSeries, seamSeries, tableSeries;
    const auto gridAt = [&](std::uint32_t f) -> std::shared_ptr<r::ParallaxWarpGrid> {
        const std::uint32_t b = r::parallaxBucket(f);
        if (!grids[b]) {
            return nullptr;
        }
        if (b == 0 || !grids[b - 1]) {
            return grids[b];
        }
        auto g = r::blendParallaxGrids(*grids[b - 1], *grids[b], r::parallaxCrossfadeWeight(f));
        return g.ok() ? std::make_shared<r::ParallaxWarpGrid>(std::move(g).value()) : grids[b];
    };
    const auto seamAt = [&](std::uint32_t f) -> std::shared_ptr<r::BlendSeam> {
        const std::uint32_t b = r::parallaxBucket(f);
        if (!seams[b]) {
            return nullptr;
        }
        if (b == 0 || !seams[b - 1]) {
            return seams[b];
        }
        auto s = r::blendSeams(*seams[b - 1], *seams[b], r::parallaxCrossfadeWeight(f));
        return s.ok() ? std::make_shared<r::BlendSeam>(std::move(s).value()) : seams[b];
    };
    for (std::uint32_t f = 0; f + 1 < frames; ++f) {
        const auto g0 = gridAt(f);
        const auto g1 = gridAt(f + 1);
        gridSeries.perFrame.push_back(g0 && g1 ? gridMotion(*g0, *g1, pw.decayRows) : std::vector<double>{});
        const auto s0 = seamAt(f);
        const auto s1 = seamAt(f + 1);
        seamSeries.perFrame.push_back(s0 && s1 ? seamMotion(*s0, *s1) : std::vector<double>{});
        // The table is NOT glided: it steps at every bucket edge.
        tableSeries.perFrame.push_back(
            tableMotion(tables[r::parallaxBucket(f)], tables[r::parallaxBucket(f + 1)]));
    }
    std::printf("    %s (gate %.2f): %u of %u bucket grids accepted; mean correction wing %.3f deg, ground %.3f deg\n",
                title, gate, measured, buckets, measured ? wingMean / measured : 0.0,
                measured ? groundMean / measured : 0.0);
    gridSeries.print("parallax grid (per bucket, glided)");
    seamSeries.print("carved seam line (per bucket, glided)");
    tableSeries.print("seam-shift table (per bucket, stepped; unused here");
}

/// Luma of a BGRA_4444_32f host frame (bottom-left rows) at continuous
/// top-down pixel (x, y), bilinear; longitude wraps, latitude clamps.
float hostLuma(const char* base, csSDK_int32 rowBytes, int w, int h, double x, double y) {
    const auto at = [&](int xx, int yy) {
        xx = ((xx % w) + w) % w;
        yy = std::clamp(yy, 0, h - 1);
        const auto* row = reinterpret_cast<const float*>(base + static_cast<std::ptrdiff_t>(h - 1 - yy) * rowBytes);
        const float* px = row + static_cast<std::ptrdiff_t>(xx) * 4;
        // BGRA: Rec.709 weights on the encoded values - a stable texture
        // signal for the flow, not a colorimetric quantity.
        return 0.0722f * px[0] + 0.7152f * px[1] + 0.2126f * px[2];
    };
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = static_cast<float>(fx - x0);
    const float ty = static_cast<float>(fy - y0);
    const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const float bot = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return top + (bot - top) * ty;
}

/// The nacelle patch of a rendered standard equirect, resampled on a
/// polar-axis grid at the 6K pixel pitch.
osv::render::GrayImage nacellePatch(const char* base, csSDK_int32 rowBytes, int w, int h) {
    osv::render::GrayImage img;
    const double step = 1.0 / kPxPerDeg;
    img.w = static_cast<std::uint32_t>((kPatchLon1Deg - kPatchLon0Deg) / step);
    img.h = static_cast<std::uint32_t>(2.0 * kPatchLatDeg / step);
    img.data.assign(static_cast<std::size_t>(img.w) * img.h, 0.0f);
    osv::geom::EquirectMap map;
    map.layout = osv::geom::EquirectLayout::Standard;
    map.w = w;
    map.h = h;
    for (std::uint32_t j = 0; j < img.h; ++j) {
        const double lat = osv::deg2rad(kPatchLatDeg - (static_cast<double>(j) + 0.5) * step);
        for (std::uint32_t i = 0; i < img.w; ++i) {
            const double lon = osv::deg2rad(kPatchLon0Deg + (static_cast<double>(i) + 0.5) * step);
            const osv::Vec3d d{std::cos(lat) * std::sin(lon), std::sin(lat), std::cos(lat) * std::cos(lon)};
            osv::Vec2d px;
            if (map.dirToPixel(d, px)) {
                img.data[static_cast<std::size_t>(j) * img.w + i] = hostLuma(base, rowBytes, w, h, px.x, px.y);
            }
        }
    }
    return img;
}

/// One configuration of the importer and its prefs.
struct SteadyConfig {
    const char* name;
    bool corrections;  ///< Parallax + seam search on.
    bool seamSearch;
    osv::premiere::PrefsParallaxGrid grid;
    osv::premiere::PrefsLensAlign align;
};

}  // namespace steadybench

void partS(const Options& o) {
    namespace r = osv::render;
    using namespace steadybench;
    std::printf("\n=== S. Steadiness at the seam: what moves around the nacelle, frame to frame ===\n");

    // ---- 1. each analysis on the importer's schedule ------------------------------
    std::printf("\n  1. The per-bucket analyses replayed on the importer's schedule (library, classical flow, 6K px)\n");
    osvtool::PipelineOptions po;
    po.input = o.clip;
    po.stab = "off";
    po.hw = "d3d11va";
    po.device = "cpu";
    po.hostFramesRequired = true;
    auto opened = osvtool::Pipeline::open(po, /*needRenderer=*/false);
    if (!opened.ok()) {
        std::printf("    Pipeline::open failed: %s\n", opened.error().message.c_str());
        return;
    }
    osvtool::Pipeline& P = *opened.value();
    replaySchedule(P, P.rig, r::ParallaxWarpParams{}.requiredImprovement, "calibration rig (today)");
    // The lens rotation, fitted as the importer fits it, then the same replay.
    const std::vector<std::uint32_t> rotFrames =
        r::clipSampleFrames(P.frameCount(), P.syncFrames(), r::kLensRotationSamples, 0.1, 0.9);
    r::ParallaxWarpParams rp;
    rp.backend = r::FlowBackendKind::Classical;
    const auto tRot = Clock::now();
    auto rotation = r::measureLensRotation(
        P.rig, P.blendParams, rotFrames, [&P](std::uint32_t f) { return P.reader->read(f); }, rp,
        r::LensRotationParams{}, *P.pool);
    const double rotMs = msSince(tRot);
    osv::geom::LensRig aligned = P.rig;
    if (rotation.ok() && rotation.value().accepted && r::applyLensRotation(aligned, rotation.value().fit.wRad).ok()) {
        std::printf("    lens rotation: %s (measured in %.0f ms, decode %.0f)\n",
                    r::describeLensRotation(rotation.value().fit).c_str(), rotMs, rotation.value().decodeMs);
        replaySchedule(P, aligned, r::kAlignedRequiredImprovement, "rotation folded in");
    } else {
        std::printf("    lens rotation: none (%s)\n",
                    rotation.ok() ? rotation.value().reason.c_str() : rotation.error().message.c_str());
    }
    std::printf("    clip correction (Parallax Grid Steady): one grid, one table and one seam for every frame -\n"
                "      every row above is 0.000 px/frame by construction.\n");

    // ---- 2. what the viewer sees ---------------------------------------------------
    std::printf("\n  2. Rendered by the importer (Exact, 6000x3000, stabilisation off); DIS between consecutive frames\n"
                "     on a %.0f x %.0f deg patch around the nacelle at the 6K pitch.  The nacelle reflects the moving\n"
                "     ground, so the first row is the scene's own motion.\n\n",
                kPatchLon1Deg - kPatchLon0Deg, 2.0 * kPatchLatDeg);
    ImporterHarness harness;
    if (!harness.loaded()) {
        std::printf("    cannot load the importer: %s\n", harness.loadError().c_str());
        return;
    }
    const void* rawSuite = nullptr;
    if (harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &rawSuite) != kSPNoError ||
        !rawSuite) {
        std::printf("    cannot acquire the PPix suite from the mock host\n");
        return;
    }
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(rawSuite);
    using osv::premiere::PrefsLensAlign;
    using osv::premiere::PrefsParallaxGrid;
    const SteadyConfig configs[] = {
        {"no seam corrections (the scene's own motion)", false, false, PrefsParallaxGrid::FollowsScene,
         PrefsLensAlign::Off},
        {"today: per moment, calibration only", true, true, PrefsParallaxGrid::FollowsScene, PrefsLensAlign::Off},
        {"today with Seam Search off", true, false, PrefsParallaxGrid::FollowsScene, PrefsLensAlign::Off},
        {"per moment + lens alignment", true, true, PrefsParallaxGrid::FollowsScene, PrefsLensAlign::Auto},
        {"steady, calibration only", true, true, PrefsParallaxGrid::Steady, PrefsLensAlign::Off},
        {"new defaults: Auto + lens alignment", true, true, PrefsParallaxGrid::Auto, PrefsLensAlign::Auto},
    };
    std::printf("    %-46s %8s %8s %8s %8s %9s %9s\n", "config", "mean px", "p95 px", "p99 px", "max p99",
                "|dI| x1e3", "1st frame");
    csSDK_int32 importerId = 1500;
    for (const SteadyConfig& c : configs) {
        harness.host().clearCache();
        auto clip = harness.openClip(o.clip, importerId++);
        if (!clip.open()) {
            std::printf("    %-46s open failed\n", c.name);
            continue;
        }
        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
        prefs.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
        prefs.parallax = static_cast<std::uint8_t>(c.corrections ? osv::premiere::PrefsParallax::On
                                                                 : osv::premiere::PrefsParallax::Off);
        prefs.seamSearch = c.seamSearch ? 1u : 0u;
        prefs.parallaxGrid = static_cast<std::uint8_t>(c.grid);
        prefs.lensAlign = static_cast<std::uint8_t>(c.align);
        imFileInfoRec8 info{};
        if (harness.getInfo8(clip, info, &prefs) != imNoErr || info.vidScale <= 0 || info.vidSampleSize <= 0) {
            std::printf("    %-46s imGetInfo8 failed\n", c.name);
            continue;
        }
        const PrTime ticksPerFrame = kTicksPerSecond * info.vidSampleSize / info.vidScale;
        std::vector<r::GrayImage> patches;
        double firstMs = 0.0;
        for (int f = 0; f < 65; ++f) {
            ImporterHarness::SourceVideoRequest request;
            request.frameTime = ticksPerFrame * f;
            request.format = PrPixelFormat_BGRA_4444_32f;
            request.width = 6000;
            request.height = 3000;
            request.intent = imRenderIntent_Export;
            PPixHand hand = nullptr;
            const auto t0 = Clock::now();
            if (harness.getSourceVideo(clip, request, prefs, hand) != imNoErr || !hand) {
                std::printf("    %-46s frame %d failed\n", c.name, f);
                break;
            }
            if (f == 0) {
                firstMs = msSince(t0);
            }
            char* pixels = nullptr;
            csSDK_int32 rowBytes = 0;
            if (ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) == suiteError_NoError && pixels &&
                ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError && rowBytes > 0) {
                patches.push_back(nacellePatch(pixels, rowBytes, 6000, 3000));
            }
            ppix->Dispose(hand);
        }
        clip.close();
        // ---- consecutive-frame motion ----------------------------------------------
        std::vector<double> means, p95s, p99s, diffs;
        for (std::size_t k = 0; k + 1 < patches.size(); ++k) {
            auto flow = r::computeFlow(r::FlowBackendKind::Classical, patches[k], patches[k + 1],
                                       r::FlowBackendParams{}, P.pool.get(), nullptr);
            if (!flow.ok()) {
                continue;
            }
            const r::BidirFlow& bf = flow.value();
            std::vector<double> mags;
            for (std::size_t i = 0; i < bf.ok.size(); ++i) {
                if (bf.ok[i]) {
                    mags.push_back(std::hypot(static_cast<double>(bf.forward.u[i]),
                                              static_cast<double>(bf.forward.v[i])));
                }
            }
            double m = 0.0;
            for (const double x : mags) {
                m += x;
            }
            means.push_back(mags.empty() ? 0.0 : m / static_cast<double>(mags.size()));
            p95s.push_back(percentile(mags, 95.0));
            p99s.push_back(percentile(mags, 99.0));
            double d = 0.0;
            for (std::size_t i = 0; i < patches[k].data.size(); ++i) {
                d += std::fabs(static_cast<double>(patches[k + 1].data[i]) - static_cast<double>(patches[k].data[i]));
            }
            diffs.push_back(1e3 * d / static_cast<double>(std::max<std::size_t>(1, patches[k].data.size())));
        }
        const auto avg = [](const std::vector<double>& v) {
            double s = 0.0;
            for (const double x : v) {
                s += x;
            }
            return v.empty() ? 0.0 : s / static_cast<double>(v.size());
        };
        std::printf("    %-46s %8.3f %8.3f %8.3f %8.3f %9.3f %7.0f ms\n", c.name, avg(means), avg(p95s), avg(p99s),
                    p99s.empty() ? 0.0 : *std::max_element(p99s.begin(), p99s.end()), avg(diffs), firstMs);
    }
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

}  // namespace

int main(int argc, char** argv) {
    // FIRST, before anything can load a plug-in or open a log: redirect the
    // plug-ins' log files away from the user's real %LOCALAPPDATA%\OpenOSV.
    // A benchmark run writes hundreds of imGetSourceVideo lines, and those
    // landing in the real importer log have already misled a performance
    // diagnosis (see TestLogIsolation.h).
    osv::premiere::testsupport::isolatePluginLogs();

    const Options o = parseArgs(argc, argv);
    if (o.clip.empty() || !std::filesystem::exists(o.clip)) {
        std::printf("usage: osv_importer_bench [clip.OSV] [--frames N] [--part A|B|C|D|P|F|Q|all]\n");
        std::printf("clip not found: '%s'\n", o.clip.string().c_str());
        return 2;
    }
    std::printf("OpenOSV frame-cost benchmark\n  clip: %s\n  frames per config: %d (warm = second half of each run)\n",
                o.clip.string().c_str(), o.frames);

    // Each part is independent; running one per process gives honest cold
    // numbers for that part (see the file header).
    if (wantPart(o, 'A')) {
        partA(o);
    }
    if (wantPart(o, 'B')) {
        partB(o);
    }
    if (wantPart(o, 'C')) {
        partC(o);
    }
    if (wantPart(o, 'D')) {
        partD(o);
    }
    if (wantPart(o, 'P')) {
        partP(o);
    }
    if (wantPart(o, 'F')) {
        partF(o);
    }
    if (wantPart(o, 'Q')) {
        partQ(o);
    }
    // [WP-STEADY] Not in "all": it renders 390 native frames and replays the
    // analyses twice; run it on its own (--part S).
    if (o.part == "S" || o.part == "s") {
        partS(o);
    }
    return 0;
}
