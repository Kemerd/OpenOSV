// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterBench.cpp - where does a frame's time actually go?
//
// A measurement tool, not a test: it asserts nothing and is deliberately NOT
// registered with ctest, because its numbers depend on the machine and would
// only make the suite slow and flaky.  Run it by hand:
//
//     build\<dir>\bin\osv_importer_bench.exe [clip.OSV] [--frames N] [--part A|B|C|D|all]
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
#include "osv/render/CudaRenderer.h"
#include "osv/render/ImageRGBAf.h"
#include "osv/render/RenderParamsBuilder.h"
#include "osv/render/SeamAnalysis.h"

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
        std::printf("usage: osv_importer_bench [clip.OSV] [--frames N] [--part A|B|C|D|all]\n");
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
    return 0;
}
