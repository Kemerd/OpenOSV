// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_direct_e2e.cpp - the effect's DIRECT path (docs/DIRECT_GPU.md) end to
// end, the way Premiere runs it.
//
// Both BUILT plug-ins are loaded into this process, as they are into
// Premiere: Open360Reframe.aex (LoadedPlugin) and OpenOSVImporter.prm (the
// importer tests' own ImporterHarness, which also owns the mock host both
// modules talk to).  Nothing of the direct path is linked into the test or
// stubbed:
//
//   * the mock host models the segment graph Premiere hands the effect -
//     effect node -> owning clip node -> media node whose
//     "MediaNode::MediaInstanceString" is the sample clip - plus the clip
//     node's TransformNodeTime (in point, speed, reverse) and the sequence's
//     working colour space;
//   * the CUDA context is a PRIVATE one, as Premiere's is (not the device's
//     primary context), so every device pointer the importer's engine hands
//     the effect has to live in the effect's context for the kernel to run;
//   * frames are GPU PPixes from the GPU Device Suite, rendered through the
//     module's PrGPUFilter table.
//
// What is proven:
//
//   1. The direct path is TAKEN.  Its instance is handed a solid marker
//      colour as the equirect input, so a frame of the real clip in the
//      output can only have come from the fisheyes; the equirect path would
//      paint the marker.
//   2. It FRAMES like the equirect path.  The same views are rendered by a
//      second instance through the equirect path from the importer's own
//      6000 x 3000 equirect of the same frame, and the two outputs are
//      compared by geometry: both lumas low-passed (the direct render is
//      sharper - one resampling instead of two - and that must not read as a
//      shift), then a 6 x 4 grid of tiles each aligned by Lucas-Kanade to
//      sub-pixel precision.  Tolerance and a negative control are below.
//   3. Clip time reaches the right MEDIA frame on trimmed, sped-up, slowed
//      and reversed clips and across frame rates: each render is compared
//      bit for bit with the identity-mapped render of the expected frame, and
//      the plug-in's own log line (clip time -> media time -> frame) is
//      checked against the same numbers.
//   4. Every FALLBACK renders the equirect path and leaves nothing acquired:
//      importer not loaded, non-OSV media, nested sequences, a multicam or
//      missing input, no owning clip, a file the engine cannot open, working
//      spaces the path cannot produce, hosts that cannot report the working
//      space, and TransformNodeTime failing or pointing before the media.
//
// The engine is resolved ONCE per process (DirectPath.cpp).  ctest runs every
// case in its own process; when the whole executable runs in one process and
// an earlier case resolved the engine before the importer was loaded, the
// direct-path cases here say so and skip rather than fail.

#include "GpuTestSupport.h"
#include "ImporterHarness.h"

#include "OsvEngineAbi.h"
#include "PrefsBlob.h"

#include "PrSDKColorManagementSuite.h"
#include "PrSDKColorSpaces.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKSequenceInfoSuite.h"
#include "PrSDKVideoSegmentProperties.h"
#include "PrSDKVideoSegmentSuite.h"

#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;
using osv::premiere::PrefsBlob;
using osv::premiere::mock::GpuContextKind;
using osv::premiere::mock::MockHost;
using osv::premiere::mock::SequenceConfig;
using osv::premiere::test::ImporterHarness;

namespace {

// ===========================================================================
//  Fixture pieces
// ===========================================================================

/// The timeline every case renders on.
constexpr PrTimelineID kTimeline = 0x0E2E;

/// The marker the equirect path is fed.  Exactly representable in binary16
/// and binary32, and not a colour a PQ-coded frame of the sample produces, so
/// "every pixel is the marker" means "the equirect path drew this" and "no
/// pixel is" means "the fisheyes did".
constexpr float kMarker[4] = {0.125f, 0.875f, 0.25f, 1.0f};

/// Largest tile displacement (output pixels) two renders of the same view may
/// show and still be called the same framing.  WP-C measured the paths'
/// GEOMETRIC difference at <= 0.0016 px; an image-based measurement adds its
/// own noise (different resampling, the seam blend, noise in the clip), so
/// the bar is set well above what it measures and far below the negative
/// control's half pixel.  The measured values are printed with every run.
constexpr double kFramingTolerancePx = 0.15;

/// Which path drew a frame, judged by the marker (see kMarker).
enum class DrawnBy { Direct, Equirect, Unclear };

[[nodiscard]] const char* nameOf(DrawnBy d) {
    switch (d) {
        case DrawnBy::Direct: return "direct (fisheyes)";
        case DrawnBy::Equirect: return "equirect";
        default: return "unclear";
    }
}

[[nodiscard]] DrawnBy drawnBy(const std::vector<float>& image) {
    if (image.empty()) {
        return DrawnBy::Unclear;
    }
    const double marker = fractionOfColour(image, kMarker, 1e-3f);
    if (marker >= 0.999) {
        return DrawnBy::Equirect;
    }
    // Real picture everywhere it is covered, and most of the frame covered.
    if (marker <= 0.001 && opaqueFraction(image) > 0.5) {
        return DrawnBy::Direct;
    }
    return DrawnBy::Unclear;
}

/// Why the direct path cannot run in this process, or "" when it can.
[[nodiscard]] std::string directUnavailableReason() {
    if (const char* kill = std::getenv("OSV_DISABLE_DIRECT")) {
        if (kill[0] == '1') {
            return "OSV_DISABLE_DIRECT=1 is set";
        }
    }
    // DirectPath.cpp resolves the engine once per process and logs this when
    // the importer was absent at that moment - which an earlier case of a
    // single-process run can cause.  ctest runs every case alone.
    if (reframeLogContains("reframe/direct: the OpenOSV importer is not loaded")) {
        return "this process resolved the engine before the importer was loaded; run this case on its own (ctest does)";
    }
    return {};
}

/// The three node ids of one clip in the graph.
struct ClipNodes {
    csSDK_int32 effect = 0;
    csSDK_int32 clip = 0;
    csSDK_int32 media = 0;
};

/// Model "effect on a clip of this media": effect node -> owner clip node ->
/// input 0, a media node naming `instanceUtf8`.
void bindMedia(MockHost& host, const ClipNodes& n, const std::string& instanceUtf8) {
    host.setNodeType(n.effect, kVideoSegment_NodeType_Effect);
    host.setNodeType(n.clip, kVideoSegment_NodeType_Clip);
    host.setNodeType(n.media, kVideoSegment_NodeType_Media);
    host.setNodeOwner(n.effect, n.clip);
    host.addNodeInput(n.clip, n.media, 0);
    host.setNodeProperty(n.media, kVideoSegmentProperty_Media_InstanceString, instanceUtf8);
}

/// UTF-8 spelling of a path, as the media node's instance string carries it.
[[nodiscard]] std::string utf8Of(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

/// The sequence every case uses: `w` x `h`, `ticksPerFrame`, and the given
/// predefined colour space as the working space.
void configureSequence(MockHost& host, int w, int h, PrTime ticksPerFrame, const char* workingSpace) {
    SequenceConfig seq = host.sequence(kTimeline);
    prSetRect(&seq.frameRect, 0, 0, w, h);
    seq.ticksPerFrame = ticksPerFrame;
    seq.workingColorSpace = host.colorSpaceId(workingSpace);
    host.setSequence(kTimeline, seq);
}

/// Publish `prefs` as the sample clip's Source Settings the way Premiere does:
/// open an importer instance of the file and describe it (imGetInfo8 carries
/// the clip's blob, and the instance publishes it to the engine).  Premiere
/// always does this before it can hand the effect a frame of the clip, and
/// the direct path refuses to render settings nobody published [WP-SETTINGS],
/// so every direct-path case starts here.  The handle must outlive the
/// renders only as far as the test wants the instance alive; what it
/// published stays in force either way.
[[nodiscard]] ImporterHarness::ClipHandle publishSourceSettings(ImporterHarness& harness,
                                                                const std::filesystem::path& clip,
                                                                const PrefsBlob& prefs, csSDK_int32 importerId = 7) {
    ImporterHarness::ClipHandle handle = harness.openClip(clip, importerId);
    if (handle.open()) {
        imFileInfoRec8 info{};
        if (harness.getInfo8(handle, info, &prefs) != imNoErr) {
            handle.close();
        }
    }
    return handle;
}

/// Mean of the RGB channels over the covered (alpha > 0) pixels of an RGBA
/// image: a brightness measure for "did the exposure change reach it".
[[nodiscard]] double meanRgb(const std::vector<float>& rgba) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i + 3] > 0.0f) {
            sum += static_cast<double>(rgba[i]) + static_cast<double>(rgba[i + 1]) + static_cast<double>(rgba[i + 2]);
            n += 3;
        }
    }
    return n ? sum / static_cast<double>(n) : 0.0;
}

/// A rectilinear view that stays well inside the master lens: a comfortable
/// default for the cases that only care WHICH path drew the frame.
[[nodiscard]] Controls plainView() {
    Controls c;
    c.pan = 20.0;
    c.tilt = 5.0;
    c.fov = 100.0;
    return c;
}

/// Parsed "reframe/direct: mapping ..." line.
struct MappingLine {
    int ordinal = 0;
    long long clipTicks = 0;
    long long mediaTicks = 0;
    long long frame = 0;
};

/// This process's mapping lines for one clip node, in order, written after
/// byte `offset` of the log.
[[nodiscard]] std::vector<MappingLine> mappingLinesFor(csSDK_int32 clipNode, std::uintmax_t offset) {
    static const std::regex pattern(
        R"(reframe/direct: mapping (\d+)/\d+ of this instance \(clip node (-?\d+)\) - clip time (-?\d+) ticks .* -> media time (-?\d+) ticks .* -> frame (\d+) of)");
    std::vector<MappingLine> out;
    for (const std::string& line : reframeLogLinesSince(offset)) {
        std::smatch m;
        if (!std::regex_search(line, m, pattern) || std::stoll(m[2].str()) != clipNode) {
            continue;
        }
        MappingLine l;
        l.ordinal = std::stoi(m[1].str());
        l.clipTicks = std::stoll(m[3].str());
        l.mediaTicks = std::stoll(m[4].str());
        l.frame = std::stoll(m[5].str());
        out.push_back(l);
    }
    return out;
}

/// Copy a host PPix (Premiere's bottom-up row order) into a top-down buffer
/// laid out for `frame`, 32f BGRA both sides.
[[nodiscard]] std::vector<std::uint8_t> packForGpu(MockHost& host, PPixHand hand, const GpuFrame& frame) {
    std::vector<std::uint8_t> bytes;
    const auto info = host.inspect(hand);
    if (!info || info->isGpu || info->format != PrPixelFormat_BGRA_4444_32f ||
        static_cast<int>(info->width) != frame.width() || static_cast<int>(info->height) != frame.height() ||
        frame.isHalf()) {
        return bytes;
    }
    bytes.assign(frame.byteSize(), 0u);
    const std::size_t rowPayload = static_cast<std::size_t>(frame.width()) * 16u;
    const auto* base = static_cast<const std::uint8_t*>(info->pixels);
    for (int y = 0; y < frame.height(); ++y) {
        // Host row 0 is the BOTTOM scanline.
        const std::uint8_t* src =
            base + static_cast<std::ptrdiff_t>(info->rowBytes) * static_cast<std::ptrdiff_t>(frame.height() - 1 - y);
        std::memcpy(bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.rowBytes()), src,
                    rowPayload);
    }
    return bytes;
}

/// Dispose a host PPix through the PPix Suite.
void disposePPix(MockHost& host, PPixHand hand) {
    const void* raw = nullptr;
    if (!hand || host.basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &raw) != kSPNoError ||
        !raw) {
        return;
    }
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(raw);
    if (ppix->Dispose) {
        ppix->Dispose(hand);
    }
    host.basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

/// Render one frame through `instance` and download it (empty on failure).
[[nodiscard]] std::vector<float> renderAndRead(FilterInstance& instance, const GpuFrame& in, GpuFrame& out,
                                               PrTime clipTime, PrTime ticksPerFrame) {
    if (instance.render(in, out, clipTime, ticksPerFrame) != suiteError_NoError) {
        return {};
    }
    return out.downloadRgba();
}

// ---------------------------------------------------------------------------
//  Skips shared by the direct-path cases
// ---------------------------------------------------------------------------

#define E2E_REQUIRE_SAMPLE()                                                                                   \
    do {                                                                                                       \
        if (!e2eSampleClipAvailable()) {                                                                       \
            SKIP("sample clip not available (set OSV_SAMPLE_FILE): " << e2eSampleClipPath().string());         \
        }                                                                                                      \
    } while (0)

#define E2E_REQUIRE_GPU_AND_ENGINE(host)                                                                       \
    do {                                                                                                       \
        REQUIRE((host).setGpuContextKind(GpuContextKind::Private));                                            \
        if (!(host).gpuAvailable()) {                                                                          \
            SKIP("no CUDA device: " << (host).gpuFailureReason());                                             \
        }                                                                                                      \
        const std::string e2eWhyNot = directUnavailableReason();                                               \
        if (!e2eWhyNot.empty()) {                                                                              \
            SKIP(e2eWhyNot);                                                                                   \
        }                                                                                                      \
    } while (0)

}  // namespace

// ===========================================================================
//  0. The mock's segment graph itself
// ===========================================================================

TEST_CASE("the mock host walks and times a segment graph the way the SDK describes", "[reframe][e2e][mockhost]") {
    // A guard on the fixture: if the mock stopped modelling any of this, the
    // end-to-end cases below would be testing nothing and could still pass.
    MockHost host;
    const void* raw = nullptr;
    REQUIRE(host.basicSuite()->AcquireSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion9, &raw) ==
            kSPNoError);
    const auto* s = static_cast<const PrSDKVideoSegmentSuite*>(raw);
    REQUIRE(s != nullptr);

    const ClipNodes n{11, 22, 33};
    bindMedia(host, n, "C:/footage/clip.OSV");

    // ---- the walk -------------------------------------------------------------
    csSDK_int32 owner = 0;
    REQUIRE(s->AcquireOperatorOwnerNodeID(n.effect, &owner) == suiteError_NoError);
    CHECK(owner == n.clip);
    CHECK(host.nodeRefCount(n.clip) == 1);
    csSDK_int32 inputs = 0;
    REQUIRE(s->GetNodeInputCount(owner, &inputs) == suiteError_NoError);
    CHECK(inputs == 1);
    PrTime offset = -1;
    csSDK_int32 media = 0;
    REQUIRE(s->AcquireInputNodeID(owner, 0, &offset, &media) == suiteError_NoError);
    CHECK(media == n.media);
    CHECK(offset == 0);
    csSDK_int32 second = -1;
    CHECK(s->AcquireInputNodeID(owner, 1, &offset, &second) != suiteError_NoError);  // no second input
    CHECK(second == 0);

    char type[kMaxNodeTypeStringSize] = {};
    prPluginID hash{};
    csSDK_int32 flags = 0;
    REQUIRE(s->GetNodeInfo(n.media, type, &hash, &flags) == suiteError_NoError);
    CHECK(std::string(type) == kVideoSegment_NodeType_Media);
    REQUIRE(s->GetNodeInfo(n.clip, type, &hash, &flags) == suiteError_NoError);
    CHECK(std::string(type) == kVideoSegment_NodeType_Clip);

    // ---- properties through the iterator ------------------------------------------
    static thread_local std::map<std::string, std::string>* sink = nullptr;
    std::map<std::string, std::string> seen;
    sink = &seen;
    REQUIRE(s->IterateNodeProperties(
                n.media,
                [](csSDK_int32, const char* key, const prUTF8Char* value) -> prSuiteError {
                    (*sink)[key] = reinterpret_cast<const char*>(value);
                    return suiteError_NoError;
                },
                0) == suiteError_NoError);
    sink = nullptr;
    CHECK(seen[kVideoSegmentProperty_Media_InstanceString] == "C:/footage/clip.OSV");

    // ---- releases balance, and an extra one is caught ---------------------------
    CHECK(s->ReleaseVideoNodeID(media) == suiteError_NoError);
    CHECK(s->ReleaseVideoNodeID(owner) == suiteError_NoError);
    CHECK(host.totalNodeRefs() == 0);
    CHECK(s->ReleaseVideoNodeID(owner) != suiteError_NoError);
    CHECK(host.invalidNodeReleases() == 1);

    // ---- time transforms --------------------------------------------------------------
    const PrTime f = kTicksPerFrame5994;
    PrTime out = 0;
    REQUIRE(s->TransformNodeTime(n.clip, 5 * f, &out) == suiteError_NoError);
    CHECK(out == 5 * f);  // identity by default
    host.setNodeTimeTransform(n.clip, 20 * f, 1, 1);  // trimmed
    REQUIRE(s->TransformNodeTime(n.clip, 5 * f, &out) == suiteError_NoError);
    CHECK(out == 25 * f);
    host.setNodeTimeTransform(n.clip, 4 * f, 2, 1);  // double speed
    REQUIRE(s->TransformNodeTime(n.clip, 3 * f, &out) == suiteError_NoError);
    CHECK(out == 10 * f);
    host.setNodeTimeTransform(n.clip, 60 * f, -1, 1);  // reversed
    REQUIRE(s->TransformNodeTime(n.clip, 5 * f, &out) == suiteError_NoError);
    CHECK(out == 55 * f);
    double rate = 0.0;
    REQUIRE(s->GetNodeTimeScale(n.clip, 0, &rate) == suiteError_NoError);
    CHECK(rate == -1.0);
    host.setNodeTimeTransformError(n.clip, suiteError_Fail);
    CHECK(s->TransformNodeTime(n.clip, 0, &out) == suiteError_Fail);

    // ---- parameter reads: count override, injected failures, the call log -----
    PrParam p{};
    p.mType = kPrParamType_Float64;
    p.mFloat64 = 120.0;
    host.setParam(n.effect, 6, 0, p);
    csSDK_int32 count = 0;
    REQUIRE(s->GetParamCount(n.effect, &count) == suiteError_NoError);
    CHECK(count == 7);
    host.setParamCount(n.effect, 15);
    REQUIRE(s->GetParamCount(n.effect, &count) == suiteError_NoError);
    CHECK(count == 15);
    host.setParamReadError(n.effect, 6, suiteError_Fail, PrTime{0});
    host.clearParamReads();
    PrParam got{};
    CHECK(s->GetParam(n.effect, 6, 0, &got) == suiteError_Fail);
    CHECK(s->GetParam(n.effect, 6, f, &got) == suiteError_NoError);  // only t = 0 fails
    CHECK(got.mFloat64 == 120.0);
    const auto reads = host.paramReads();
    REQUIRE(reads.size() == 2u);
    CHECK(reads[0].time == 0);
    CHECK(reads[0].result == suiteError_Fail);
    CHECK(reads[1].result == suiteError_NoError);

    host.basicSuite()->ReleaseSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion9);
}

TEST_CASE("the mock host can hand out a private CUDA context like Premiere's", "[reframe][e2e][mockhost][cuda]") {
    MockHost host;
    REQUIRE(host.setGpuContextKind(GpuContextKind::Private));
    if (!host.gpuAvailable()) {
        SKIP("no CUDA device: " << host.gpuFailureReason());
    }
    // Once the device exists the kind is fixed.
    CHECK_FALSE(host.setGpuContextKind(GpuContextKind::Primary));
    CHECK(host.setGpuContextKind(GpuContextKind::Private));
    CUcontext context = nullptr;
    {
        HostContextScope scope(host);
        REQUIRE(scope.ok());
        context = scope.context();
    }
    CHECK_FALSE(isPrimaryContext(context));
    // And the thread was left as found: nothing of ours is current.
    CUcontext current = nullptr;
    CHECK(cuCtxGetCurrent(&current) == CUDA_SUCCESS);
    CHECK(current != context);
}

// ===========================================================================
//  1 + 2. The direct path is taken, and frames like the equirect path
// ===========================================================================

TEST_CASE("the direct path renders the sample from the fisheyes and frames it exactly like the equirect path",
          "[reframe][direct][e2e][cuda][sample]") {
    E2E_REQUIRE_SAMPLE();
    ImporterHarness harness;  // owns the host; imShutdown runs before the host's context goes
    INFO("importer: " << harness.loadError());
    REQUIRE(harness.loaded());
    MockHost& host = harness.host();
    E2E_REQUIRE_GPU_AND_ENGINE(host);

    // ---- Premiere's setting: a private context --------------------------------
    {
        HostContextScope scope(host);
        REQUIRE(scope.ok());
        INFO("the mock must hand out a private context, as Premiere does");
        // isPrimaryContext pushes the context itself; nesting is harmless.
        CHECK_FALSE(isPrimaryContext(scope.context()));
    }

    const std::filesystem::path sample = e2eSampleClipPath();
    constexpr std::uint32_t kFrame = 32;
    const PrTime clipTime = static_cast<PrTime>(kFrame) * kTicksPerFrame5994;

    // ---- the equirect path's input: the importer's own 6000 x 3000 frame ---------
    // Opening the clip in the importer also publishes its Source Settings to
    // the engine, exactly as Premiere's instance of the clip does.
    PrefsBlob prefs = PrefsBlob::defaults();
    auto clip = harness.openClip(sample);
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
    ImporterHarness::SourceVideoRequest request;
    request.frameTime = clipTime;
    request.format = PrPixelFormat_BGRA_4444_32f;
    request.width = 6000;
    request.height = 3000;
    PPixHand equirectHand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, equirectHand) == imNoErr);
    REQUIRE(equirectHand != nullptr);
    GpuFrame equirect(host, 6000, 3000, false);
    REQUIRE(equirect.valid());
    {
        const std::vector<std::uint8_t> packed = packForGpu(host, equirectHand, equirect);
        REQUIRE(!packed.empty());
        REQUIRE(equirect.upload(packed));
    }
    disposePPix(host, equirectHand);

    // ---- the sequence and the two clips ------------------------------------------
    constexpr int kW = 1280;
    constexpr int kH = 720;
    configureSequence(host, kW, kH, kTicksPerFrame5994, kPrRec2100PQ);
    const ClipNodes directNodes{100, 200, 300};
    bindMedia(host, directNodes, utf8Of(sample));
    // The comparison instance renders the SAME views through the equirect
    // path; its media is not an OSV file, so it can take no other route.
    const ClipNodes twoStepNodes{101, 201, 301};
    bindMedia(host, twoStepNodes, "D:/footage/interview.mov");

    const std::uintmax_t logStart = reframeLogSize();
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame marker(host, 512, 256, false);
    REQUIRE(marker.fill(kMarker));
    GpuFrame outDirect(host, kW, kH, false);
    GpuFrame outTwoStep(host, kW, kH, false);
    REQUIRE(outDirect.valid());
    REQUIRE(outTwoStep.valid());

    // Controls exist before the instances, as they do in Premiere.
    writeVerbatimControls(host, directNodes.effect, plainView());
    writeVerbatimControls(host, twoStepNodes.effect, plainView());
    FilterInstance direct(scope, host, directNodes.effect, kTimeline);
    FilterInstance twoStep(scope, host, twoStepNodes.effect, kTimeline);
    REQUIRE(direct.created() == suiteError_NoError);
    REQUIRE(twoStep.created() == suiteError_NoError);

    struct View {
        const char* name;
        Controls c;
    };
    auto view = [](double pan, double tilt, double roll, double fov, double distortion, double sp = 0.0,
                   double st = 0.0, double sr = 0.0) {
        Controls c;
        c.pan = pan;
        c.tilt = tilt;
        c.roll = roll;
        c.fov = fov;
        c.distortion = distortion;
        c.sourcePan = sp;
        c.sourceTilt = st;
        c.sourceRoll = sr;
        return c;
    };
    const View views[] = {
        {"rectilinear 90", view(30.0, 0.0, 0.0, 90.0, 0.0)},
        {"narrow 40, master lens centre", view(0.0, 0.0, 0.0, 40.0, 0.0)},
        {"narrow 40, slave lens centre", view(180.0, -10.0, 0.0, 40.0, 0.0)},
        {"wide eye-offset 150 + distortion 40", view(-60.0, -5.0, 0.0, 150.0, 40.0)},
        {"across the lens seam (pan 90)", view(90.0, 0.0, 0.0, 100.0, 0.0)},
        {"rolled + source-rotated", view(-40.0, 15.0, 25.0, 110.0, 20.0, 30.0, -12.0, 8.0)},
        {"tiny planet (Asteroid 300)", view(0.0, -90.0, 0.0, 300.0, 100.0)},
    };

    for (const View& v : views) {
        INFO(v.name);
        writeVerbatimControls(host, directNodes.effect, v.c);
        writeVerbatimControls(host, twoStepNodes.effect, v.c);
        const std::vector<float> a = renderAndRead(direct, marker, outDirect, clipTime, kTicksPerFrame5994);
        const std::vector<float> b = renderAndRead(twoStep, equirect, outTwoStep, clipTime, kTicksPerFrame5994);
        REQUIRE(!a.empty());
        REQUIRE(!b.empty());

        // 1. drawn from the fisheyes: not one pixel of the marker it was fed.
        CHECK(drawnBy(a) == DrawnBy::Direct);
        CHECK(drawnBy(b) != DrawnBy::Equirect);  // b's input is the real equirect, never the marker

        // 2. the same framing, by geometry.
        const Alignment al = measureAlignment(b, a, kW, kH);
        WARN("[" << v.name << "] low-passed NCC " << al.ncc << ", " << al.tilesUsed << " tiles, max tile shift "
                 << al.maxShiftPx << " px, median (" << al.medianDx << ", " << al.medianDy << ") px");
        CHECK(al.tilesUsed >= 6);
        CHECK(al.ncc > 0.98);
        CHECK(al.maxShiftPx < kFramingTolerancePx);
    }

    // ---- negative control: half a pixel of framing error is seen ------------------
    // Pan the DIRECT render by the angle that moves the frame centre by 0.5 px
    // at this focal length (rectilinear 90 over 1280 px: f = 640 px).  The
    // measurement must report it - and at about that size, which is what
    // makes the tolerance above a statement about sub-pixel framing.
    {
        const double focal = (kW / 2.0) / std::tan(45.0 * 3.14159265358979323846 / 180.0);
        const double halfPixelDeg = std::atan(0.5 / focal) * 180.0 / 3.14159265358979323846;
        Controls shifted = views[0].c;
        shifted.pan += halfPixelDeg;
        writeVerbatimControls(host, directNodes.effect, shifted);
        writeVerbatimControls(host, twoStepNodes.effect, views[0].c);
        const std::vector<float> a = renderAndRead(direct, marker, outDirect, clipTime, kTicksPerFrame5994);
        const std::vector<float> b = renderAndRead(twoStep, equirect, outTwoStep, clipTime, kTicksPerFrame5994);
        REQUIRE(!a.empty());
        REQUIRE(!b.empty());
        const Alignment al = measureAlignment(b, a, kW, kH);
        WARN("[negative control, pan + " << halfPixelDeg << " deg] max tile shift " << al.maxShiftPx
                                           << " px, median (" << al.medianDx << ", " << al.medianDy << ") px");
        // Rectilinear: 0.5 px at the centre growing to 1 px at the edges.
        CHECK(std::fabs(al.medianDx) > 0.4);
        CHECK(std::fabs(al.medianDx) < 1.1);
        CHECK(std::fabs(al.medianDy) < 0.1);
        CHECK(al.maxShiftPx > 3.0 * kFramingTolerancePx);
    }

    // ---- 16f frames: the same picture at half precision ---------------------------
    {
        writeVerbatimControls(host, directNodes.effect, views[0].c);
        const std::vector<float> full = renderAndRead(direct, marker, outDirect, clipTime, kTicksPerFrame5994);
        GpuFrame marker16(host, 512, 256, true);
        GpuFrame out16(host, kW, kH, true);
        REQUIRE(marker16.fill(kMarker));
        const std::vector<float> half = renderAndRead(direct, marker16, out16, clipTime, kTicksPerFrame5994);
        REQUIRE(!full.empty());
        REQUIRE(!half.empty());
        CHECK(drawnBy(half) == DrawnBy::Direct);
        // binary16 keeps 11 significant bits: <= 2^-11 relative, and every
        // output code is <= 1.
        CHECK(maxAbsDifference(full, half) < 1.0e-3);
    }

    // ---- the log says what happened -------------------------------------------------
    const std::vector<MappingLine> lines = mappingLinesFor(directNodes.clip, logStart);
    REQUIRE(!lines.empty());
    CHECK(lines.front().ordinal == 1);
    CHECK(lines.front().clipTicks == clipTime);
    CHECK(lines.front().mediaTicks == clipTime);
    CHECK(lines.front().frame == kFrame);
    CHECK(mappingLinesFor(twoStepNodes.clip, logStart).empty());  // never on the direct path

    // ---- nothing left acquired -------------------------------------------------------
    CHECK(direct.dispose() == suiteError_NoError);
    CHECK(twoStep.dispose() == suiteError_NoError);
    CHECK(host.totalNodeRefs() == 0);
    CHECK(host.invalidNodeReleases() == 0);
}

// ===========================================================================
//  3. Clip time -> media time
// ===========================================================================

TEST_CASE("clip time reaches the right media frame on trimmed, sped-up, slowed and reversed clips",
          "[reframe][direct][e2e][cuda][sample]") {
    E2E_REQUIRE_SAMPLE();
    ImporterHarness harness;
    INFO("importer: " << harness.loadError());
    REQUIRE(harness.loaded());
    MockHost& host = harness.host();
    E2E_REQUIRE_GPU_AND_ENGINE(host);

    const std::filesystem::path sample = e2eSampleClipPath();
    const std::string sampleUtf8 = utf8Of(sample);
    constexpr int kW = 480;
    constexpr int kH = 270;
    configureSequence(host, kW, kH, kTicksPerFrame5994, kPrRec2100PQ);
    // [WP-SETTINGS] The clip's Source Settings, published as Premiere's
    // importer instance of it does before any render.
    auto published = publishSourceSettings(harness, sample, PrefsBlob::defaults());
    REQUIRE(published.open());

    const std::uintmax_t logStart = reframeLogSize();
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame marker(host, 512, 256, false);
    REQUIRE(marker.fill(kMarker));
    GpuFrame out(host, kW, kH, false);
    REQUIRE(out.valid());

    const PrTime f = kTicksPerFrame5994;
    const PrTime f25 = osv::premiere::mock::kTicksPerSecond / 25;  // a 25 fps sequence's frame

    // One clip per case; the clip node's transform is the case.  Frame
    // numbers are media frames of the 65-frame, 59.94 fps sample.
    struct Case {
        const char* name;
        PrTime origin;
        std::int64_t num;
        std::int64_t den;
        PrTime sequenceFrame;                 ///< The clip-time unit (the sequence's frame).
        std::vector<int> clipFrames;          ///< Clip times rendered, in sequence frames.
        std::vector<std::uint32_t> expected;  ///< The media frame each must show.
    };
    const std::vector<Case> cases = {
        {"trimmed: in point at frame 20", 20 * f, 1, 1, f, {0, 5}, {20, 25}},
        {"double speed from frame 4", 4 * f, 2, 1, f, {0, 3, 10}, {4, 10, 24}},
        // 11.5 and 12.5 frames: the engine rounds to the nearest frame,
        // halves up - the rule the importer's own imGetSourceVideo applies.
        {"half speed from frame 10", 10 * f, 1, 2, f, {3, 5}, {12, 13}},
        {"reversed from frame 60", 60 * f, -1, 1, f, {0, 5}, {60, 55}},
        // 3 x 1/25 s = 7.19 media frames, 10 x 1/25 s = 23.98.
        {"25 fps sequence over 59.94 fps media", 0, 1, 1, f25, {3, 10}, {7, 24}},
        // Media time past the end: the last frame, never an error.
        {"past the end of the media", 60 * f, 1, 1, f, {10}, {64}},
    };

    // ---- render every case -------------------------------------------------------
    struct Rendered {
        std::uint32_t expected = 0;
        std::vector<float> image;
    };
    std::vector<std::vector<Rendered>> results(cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        INFO(c.name);
        const ClipNodes n{static_cast<csSDK_int32>(110 + i), static_cast<csSDK_int32>(210 + i),
                          static_cast<csSDK_int32>(310 + i)};
        bindMedia(host, n, sampleUtf8);
        host.setNodeTimeTransform(n.clip, c.origin, c.num, c.den);
        writeVerbatimControls(host, n.effect, plainView());
        FilterInstance instance(scope, host, n.effect, kTimeline);
        REQUIRE(instance.created() == suiteError_NoError);
        for (std::size_t k = 0; k < c.clipFrames.size(); ++k) {
            const PrTime clipTime = static_cast<PrTime>(c.clipFrames[k]) * c.sequenceFrame;
            std::vector<float> image = renderAndRead(instance, marker, out, clipTime, c.sequenceFrame);
            REQUIRE(!image.empty());
            CHECK(drawnBy(image) == DrawnBy::Direct);
            results[i].push_back(Rendered{c.expected[k], std::move(image)});
        }

        // The instance's own log lines: clip time -> media time -> frame.
        const std::vector<MappingLine> lines = mappingLinesFor(n.clip, logStart);
        REQUIRE(lines.size() == c.clipFrames.size());
        for (std::size_t k = 0; k < lines.size(); ++k) {
            const PrTime clipTime = static_cast<PrTime>(c.clipFrames[k]) * c.sequenceFrame;
            const PrTime media = c.origin + (clipTime * c.num) / c.den;
            CHECK(lines[k].ordinal == static_cast<int>(k + 1));
            CHECK(lines[k].clipTicks == clipTime);
            CHECK(lines[k].mediaTicks == media);
            CHECK(lines[k].frame == static_cast<long long>(c.expected[k]));
        }
        CHECK(instance.dispose() == suiteError_NoError);
    }

    // ---- the reference: the same view of each expected frame, identity-mapped --------
    const ClipNodes ref{190, 290, 390};
    bindMedia(host, ref, sampleUtf8);
    writeVerbatimControls(host, ref.effect, plainView());
    FilterInstance reference(scope, host, ref.effect, kTimeline);
    REQUIRE(reference.created() == suiteError_NoError);
    std::map<std::uint32_t, std::vector<float>> references;
    auto referenceFor = [&](std::uint32_t frame) -> const std::vector<float>& {
        auto it = references.find(frame);
        if (it == references.end()) {
            std::vector<float> image = renderAndRead(reference, marker, out, static_cast<PrTime>(frame) * f, f);
            REQUIRE(!image.empty());
            REQUIRE(drawnBy(image) == DrawnBy::Direct);
            it = references.emplace(frame, std::move(image)).first;
        }
        return it->second;
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        for (const Rendered& r : results[i]) {
            INFO(cases[i].name << ", media frame " << r.expected);
            // Bit for bit: the same frame through the same engine and kernel.
            CHECK(maxAbsDifference(r.image, referenceFor(r.expected)) == 0.0);
        }
    }
    // Negative control: neighbouring frames really differ, so a mapping one
    // frame off could not pass the equality above.
    CHECK(maxAbsDifference(referenceFor(24), referenceFor(25)) > 1.0e-3);
    CHECK(maxAbsDifference(referenceFor(12), referenceFor(13)) > 1.0e-3);

    CHECK(reference.dispose() == suiteError_NoError);
    CHECK(host.totalNodeRefs() == 0);
    CHECK(host.invalidNodeReleases() == 0);
}

TEST_CASE("a TransformNodeTime failure or a media time before the clip falls back for that frame only",
          "[reframe][direct][e2e][cuda][sample]") {
    E2E_REQUIRE_SAMPLE();
    ImporterHarness harness;
    INFO("importer: " << harness.loadError());
    REQUIRE(harness.loaded());
    MockHost& host = harness.host();
    E2E_REQUIRE_GPU_AND_ENGINE(host);

    constexpr int kW = 320;
    constexpr int kH = 180;
    configureSequence(host, kW, kH, kTicksPerFrame5994, kPrRec2100PQ);
    // [WP-SETTINGS] Published as Premiere's importer instance does first.
    auto published = publishSourceSettings(harness, e2eSampleClipPath(), PrefsBlob::defaults());
    REQUIRE(published.open());
    const ClipNodes n{120, 220, 320};
    bindMedia(host, n, utf8Of(e2eSampleClipPath()));
    writeVerbatimControls(host, n.effect, plainView());

    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame marker(host, 512, 256, false);
    REQUIRE(marker.fill(kMarker));
    GpuFrame out(host, kW, kH, false);
    FilterInstance instance(scope, host, n.effect, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);
    const PrTime t = 10 * kTicksPerFrame5994;

    auto drawn = [&] { return drawnBy(renderAndRead(instance, marker, out, t, kTicksPerFrame5994)); };
    CHECK(drawn() == DrawnBy::Direct);

    // The host cannot map the time: this frame from the equirect...
    host.setNodeTimeTransformError(n.clip, suiteError_Fail);
    {
        const DrawnBy d = drawn();
        INFO("drawn by " << nameOf(d));
        CHECK(d == DrawnBy::Equirect);
    }
    // ...and the next one straight from the fisheyes again: a transient
    // failure must not switch the instance off.
    host.setNodeTimeTransformError(n.clip, suiteError_NoError);
    CHECK(drawn() == DrawnBy::Direct);

    // A transform that lands before the start of the media is refused too.
    host.setNodeTimeTransform(n.clip, -100 * kTicksPerFrame5994, 1, 1);
    CHECK(drawn() == DrawnBy::Equirect);
    host.setNodeTimeTransform(n.clip, 0, 1, 1);
    CHECK(drawn() == DrawnBy::Direct);

    CHECK(instance.dispose() == suiteError_NoError);
    CHECK(host.totalNodeRefs() == 0);
}

// ===========================================================================
//  4. Fallbacks
// ===========================================================================

TEST_CASE("every source the direct path cannot serve renders through the equirect path",
          "[reframe][direct][e2e][cuda]") {
    ImporterHarness harness;
    INFO("importer: " << harness.loadError());
    REQUIRE(harness.loaded());
    MockHost& host = harness.host();
    E2E_REQUIRE_GPU_AND_ENGINE(host);

    constexpr int kW = 320;
    constexpr int kH = 180;
    configureSequence(host, kW, kH, kTicksPerFrame5994, kPrRec2100PQ);

    // A file with the right extension and the wrong contents, for the engine
    // to refuse.  Written into this process's private temporary directory.
    const std::filesystem::path garbage = reframeLogPath().parent_path().parent_path() / L"not_a_clip.OSV";
    {
        std::ofstream file(garbage, std::ios::binary | std::ios::trunc);
        REQUIRE(file.good());
        for (int i = 0; i < 4096; ++i) {
            file.put(static_cast<char>((i * 131 + 7) & 0xFF));
        }
    }
    const std::filesystem::path missing = garbage.parent_path() / L"missing_clip.OSV";

    struct Variant {
        const char* name;
        ClipNodes nodes;
        std::function<void(MockHost&, const ClipNodes&)> build;
        const char* logReason;  ///< Text the per-instance fallback warning must carry, or null.
    };
    const std::vector<Variant> variants = {
        {"media that is not an OSV file", {130, 230, 330},
         [](MockHost& h, const ClipNodes& n) { bindMedia(h, n, "D:/footage/interview.mov"); }, nullptr},
        {"a nested sequence (the clip's input is a compositor)", {131, 231, 331},
         [](MockHost& h, const ClipNodes& n) {
             bindMedia(h, n, "Nested Sequence 01");
             h.setNodeType(n.media, kVideoSegment_NodeType_Compositor);
         },
         nullptr},
        {"a nested sequence served as a media node", {132, 232, 332},
         [](MockHost& h, const ClipNodes& n) {
             bindMedia(h, n, "Nested Sequence 01");
             h.setNodeProperty(n.media, kVideoSegmentProperty_Media_NestedSequenceTimelineID, "4711");
         },
         nullptr},
        {"a multicam source", {133, 233, 333},
         [](MockHost& h, const ClipNodes& n) {
             bindMedia(h, n, "Multicam 01");
             h.setNodeType(n.media, kVideoSegment_NodeType_Multicam);
         },
         nullptr},
        {"a clip node with no input", {134, 234, 334},
         [](MockHost& h, const ClipNodes& n) {
             h.setNodeType(n.effect, kVideoSegment_NodeType_Effect);
             h.setNodeType(n.clip, kVideoSegment_NodeType_Clip);
             h.setNodeOwner(n.effect, n.clip);
         },
         nullptr},
        {"an effect with no owning clip", {135, 235, 335},
         [](MockHost& h, const ClipNodes& n) { h.setNodeType(n.effect, kVideoSegment_NodeType_Effect); }, nullptr},
        // [WP-SETTINGS] No importer instance can open either file, so no
        // Source Settings are ever published for it, and the direct path's
        // settings rule hands it to the equirect route before the engine is
        // even asked to open it (it used to be the engine's "cannot open").
        {"an OSV file that does not exist", {136, 236, 336},
         [missing](MockHost& h, const ClipNodes& n) { bindMedia(h, n, utf8Of(missing)); },
         "'missing_clip.OSV' Source Settings unknown -> equirect route"},
        {"an OSV file the engine cannot parse", {137, 237, 337},
         [garbage](MockHost& h, const ClipNodes& n) { bindMedia(h, n, utf8Of(garbage)); },
         "'not_a_clip.OSV' Source Settings unknown -> equirect route"},
    };

    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame marker(host, 512, 256, false);
    REQUIRE(marker.fill(kMarker));
    GpuFrame out(host, kW, kH, false);

    for (const Variant& v : variants) {
        INFO(v.name);
        v.build(host, v.nodes);
        writeVerbatimControls(host, v.nodes.effect, plainView());
        const std::uintmax_t logStart = reframeLogSize();
        FilterInstance instance(scope, host, v.nodes.effect, kTimeline);
        REQUIRE(instance.created() == suiteError_NoError);
        // Two frames: an instance that gave up must stay on the equirect
        // path, and one that never bound must not start trying.
        for (int frame = 0; frame < 2; ++frame) {
            const DrawnBy d =
                drawnBy(renderAndRead(instance, marker, out, frame * kTicksPerFrame5994, kTicksPerFrame5994));
            INFO("frame " << frame << " drawn by " << nameOf(d));
            CHECK(d == DrawnBy::Equirect);
        }
        if (v.logReason) {
            bool found = false;
            for (const std::string& line : reframeLogLinesSince(logStart)) {
                found = found || line.find(v.logReason) != std::string::npos;
            }
            CHECK(found);
        }
        CHECK(instance.dispose() == suiteError_NoError);
        CHECK(host.totalNodeRefs() == 0);
    }
    CHECK(host.invalidNodeReleases() == 0);

    std::error_code ec;
    std::filesystem::remove(garbage, ec);
}

TEST_CASE("the direct path serves exactly the working spaces it can produce", "[reframe][direct][e2e][cuda][sample]") {
    E2E_REQUIRE_SAMPLE();
    ImporterHarness harness;
    INFO("importer: " << harness.loadError());
    REQUIRE(harness.loaded());
    MockHost& host = harness.host();
    E2E_REQUIRE_GPU_AND_ENGINE(host);

    constexpr int kW = 320;
    constexpr int kH = 180;
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame marker(host, 512, 256, false);
    REQUIRE(marker.fill(kMarker));
    GpuFrame out(host, kW, kH, false);
    const std::string sampleUtf8 = utf8Of(e2eSampleClipPath());
    const PrTime t = 8 * kTicksPerFrame5994;

    // The working space is read when the instance is created, so every
    // variant is a fresh clip + instance under a freshly configured sequence.
    csSDK_int32 nextNode = 140;
    auto renderUnder = [&](const char* workingSpace) {
        configureSequence(host, kW, kH, kTicksPerFrame5994, workingSpace);
        const ClipNodes n{nextNode, nextNode + 100, nextNode + 200};
        ++nextNode;
        bindMedia(host, n, sampleUtf8);
        writeVerbatimControls(host, n.effect, plainView());
        FilterInstance instance(scope, host, n.effect, kTimeline);
        REQUIRE(instance.created() == suiteError_NoError);
        std::vector<float> image = renderAndRead(instance, marker, out, t, kTicksPerFrame5994);
        CHECK(instance.dispose() == suiteError_NoError);
        return image;
    };

    // ---- produced: PQ and HLG on BT.2020, BT.709 ------------------------------------
    // [WP-SETTINGS] The clip's Source Settings ask the direct path to render
    // in the working space whatever the clip's own colour output (the
    // per-clip "Direct Path Colour" opt-in), so every space it can produce is
    // served straight from the fisheyes.
    PrefsBlob followWorking = PrefsBlob::defaults();
    followWorking.directColour = static_cast<std::uint8_t>(osv::premiere::PrefsDirectColour::WorkingSpace);
    auto publishedFollow = publishSourceSettings(harness, e2eSampleClipPath(), followWorking, 40);
    REQUIRE(publishedFollow.open());
    const std::vector<float> pq = renderUnder(kPrRec2100PQ);
    const std::vector<float> hlg = renderUnder(kPrRec2100HLG);
    const std::vector<float> rec709 = renderUnder(kPrRec709);
    CHECK(drawnBy(pq) == DrawnBy::Direct);
    CHECK(drawnBy(hlg) == DrawnBy::Direct);
    CHECK(drawnBy(rec709) == DrawnBy::Direct);
    // The working space really reached the engine: the three encodings differ.
    CHECK(maxAbsDifference(pq, hlg) > 0.01);
    CHECK(maxAbsDifference(pq, rec709) > 0.01);

    // ---- [WP-SETTINGS] the default: only the clip's own colour output ----------------
    // With the default "match the colour output", a PQ clip is served
    // directly only in a PQ sequence; in HLG and Rec.709 sequences Premiere's
    // own conversion of the importer's PQ frame is the faithful picture, so
    // the equirect route keeps it.  (A newer importer instance publishes, as
    // Premiere's does after a Source Settings change.)
    auto publishedMatch = publishSourceSettings(harness, e2eSampleClipPath(), PrefsBlob::defaults(), 41);
    REQUIRE(publishedMatch.open());
    const std::vector<float> pqMatch = renderUnder(kPrRec2100PQ);
    CHECK(drawnBy(pqMatch) == DrawnBy::Direct);
    CHECK(maxAbsDifference(pqMatch, pq) == 0.0);  // the same picture either way when the spaces agree
    CHECK(drawnBy(renderUnder(kPrRec2100HLG)) == DrawnBy::Equirect);
    CHECK(drawnBy(renderUnder(kPrRec709)) == DrawnBy::Equirect);

    // ---- not produced: the equirect path, where Premiere's own conversion of
    // the importer's frame stays in charge -------------------------------------
    for (const char* space : {kPrRec601525ColorSpace, kPrSony2020SLog3, kPrSRGBColorSpace}) {
        INFO("working space " << space);
        CHECK(drawnBy(renderUnder(space)) == DrawnBy::Equirect);
    }

    // ---- a host that cannot say: v8 Sequence Info (no GetWorkingColorSpace),
    // or no Color Management Suite ----------------------------------------------
    host.setSuiteAvailable(kPrSDKSequenceInfoSuite, 9, false);
    {
        INFO("Sequence Info Suite v8");
        CHECK(drawnBy(renderUnder(kPrRec2100PQ)) == DrawnBy::Equirect);
    }
    host.setSuiteAvailable(kPrSDKSequenceInfoSuite, 9, true);
    host.setSuiteAvailable(kPrSDKColorManagementSuite, kPrSDKColorManagementSuiteVersion, false);
    {
        INFO("no Color Management Suite");
        CHECK(drawnBy(renderUnder(kPrRec2100PQ)) == DrawnBy::Equirect);
    }
    host.setSuiteAvailable(kPrSDKColorManagementSuite, kPrSDKColorManagementSuiteVersion, true);

    CHECK(host.totalNodeRefs() == 0);
    CHECK(host.invalidNodeReleases() == 0);
}

TEST_CASE("without the importer in the process the effect renders through the equirect path",
          "[reframe][direct][e2e][cuda]") {
    // The engine lives in the importer module; with no importer loaded the
    // effect must not even try.  Only meaningful in a process that never
    // loaded it - the module is pinned once found - so a single-process run
    // after the other cases skips this one (ctest runs it alone).
    if (GetModuleHandleW(OSV_ENGINE_MODULE_NAME) != nullptr) {
        SKIP("the importer is already loaded in this process; run this case on its own (ctest does)");
    }
    MockHost host;
    REQUIRE(host.setGpuContextKind(GpuContextKind::Private));
    if (!host.gpuAvailable()) {
        SKIP("no CUDA device: " << host.gpuFailureReason());
    }
    constexpr int kW = 320;
    constexpr int kH = 180;
    configureSequence(host, kW, kH, kTicksPerFrame5994, kPrRec2100PQ);
    const ClipNodes n{150, 250, 350};
    // The graph is complete and names an OSV file: only the missing engine
    // stands between this instance and the direct path.
    bindMedia(host, n, e2eSampleClipAvailable() ? utf8Of(e2eSampleClipPath()) : std::string("C:/footage/clip.OSV"));
    writeVerbatimControls(host, n.effect, plainView());

    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame marker(host, 512, 256, false);
    REQUIRE(marker.fill(kMarker));
    GpuFrame out(host, kW, kH, false);
    {
        FilterInstance instance(scope, host, n.effect, kTimeline);
        REQUIRE(instance.created() == suiteError_NoError);
        CHECK(drawnBy(renderAndRead(instance, marker, out, 0, kTicksPerFrame5994)) == DrawnBy::Equirect);
        CHECK(instance.dispose() == suiteError_NoError);
    }
    CHECK(reframeLogContains("reframe/direct: the OpenOSV importer is not loaded"));
    // An unbound instance acquires nothing to begin with.
    CHECK(host.totalNodeRefs() == 0);
}

// ===========================================================================
//  5. [WP-SETTINGS] Source Settings changes reach the direct path
// ===========================================================================

TEST_CASE("a Source Settings change reaches the very next direct frame of the same GPU instance",
          "[reframe][direct][e2e][cuda][sample][settings]") {
    // Field report: Exposure -1 in a clip's Source Settings darkened the
    // Source monitor but not the Program monitor's direct render.  Premiere's
    // answer to a Source Settings change is to open a NEW importer instance
    // with the new blob (and to keep the old one around); the effect renders
    // the same clip time again.  This drives exactly that sequence through
    // both built modules and checks the pixels, not just a parameter block -
    // and deliberately keeps ONE GPU filter instance throughout, so the change
    // is proven to need nothing from Premiere beyond calling Render again.
    E2E_REQUIRE_SAMPLE();
    ImporterHarness harness;
    INFO("importer: " << harness.loadError());
    REQUIRE(harness.loaded());
    MockHost& host = harness.host();
    E2E_REQUIRE_GPU_AND_ENGINE(host);

    const std::filesystem::path sample = e2eSampleClipPath();
    constexpr int kW = 480;
    constexpr int kH = 270;
    configureSequence(host, kW, kH, kTicksPerFrame5994, kPrRec2100PQ);
    const PrTime t = 16 * kTicksPerFrame5994;

    // ---- the clip as imported: default Source Settings ---------------------------
    auto original = publishSourceSettings(harness, sample, PrefsBlob::defaults(), 51);
    REQUIRE(original.open());

    // The effect's media node spells the file differently from the importer's
    // instances - long-path prefix, upper case, backslashes.  The engine keys
    // Source Settings by file identity, so they must still meet; the unique
    // spelling also gives this case its own entries in the effect's
    // once-per-change log memory, whatever ran earlier in the process.
    std::wstring wide = L"\\\\?\\" + std::filesystem::absolute(sample).lexically_normal().wstring();
    for (wchar_t& c : wide) {
        c = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)));
    }
    const std::string spelled = utf8Of(std::filesystem::path(wide));

    const ClipNodes n{160, 260, 360};
    bindMedia(host, n, spelled);
    writeVerbatimControls(host, n.effect, plainView());
    const std::uintmax_t logStart = reframeLogSize();
    GpuEntryScope scope(host);
    REQUIRE(scope.result() == suiteError_NoError);
    GpuFrame marker(host, 512, 256, false);
    REQUIRE(marker.fill(kMarker));
    GpuFrame out(host, kW, kH, false);
    FilterInstance instance(scope, host, n.effect, kTimeline);
    REQUIRE(instance.created() == suiteError_NoError);

    const std::vector<float> before = renderAndRead(instance, marker, out, t, kTicksPerFrame5994);
    REQUIRE(!before.empty());
    REQUIRE(drawnBy(before) == DrawnBy::Direct);

    // ---- the user sets Exposure -1: Premiere opens a newer instance ---------------
    PrefsBlob darker = PrefsBlob::defaults();
    darker.exposureStops = -1.0f;
    auto changed = publishSourceSettings(harness, sample, darker, 52);
    REQUIRE(changed.open());
    const std::vector<float> after = renderAndRead(instance, marker, out, t, kTicksPerFrame5994);
    REQUIRE(!after.empty());
    CHECK(drawnBy(after) == DrawnBy::Direct);
    const double meanBefore = meanRgb(before);
    const double meanAfter = meanRgb(after);
    WARN("mean PQ code before " << meanBefore << ", after Exposure -1 " << meanAfter);
    // One stop down in linear light is a clear drop in PQ code values; the
    // bound is loose on purpose (PQ is not linear) but far above noise - the
    // same render repeated is bit-identical (checked below).
    CHECK(meanAfter < meanBefore - 0.01);

    // A FRESH instance of the same clip and time renders the same picture:
    // the long-lived instance kept nothing stale.
    {
        const ClipNodes fresh{161, 261, 361};
        bindMedia(host, fresh, spelled);
        writeVerbatimControls(host, fresh.effect, plainView());
        FilterInstance second(scope, host, fresh.effect, kTimeline);
        REQUIRE(second.created() == suiteError_NoError);
        GpuFrame out2(host, kW, kH, false);
        const std::vector<float> again = renderAndRead(second, marker, out2, t, kTicksPerFrame5994);
        REQUIRE(!again.empty());
        CHECK(maxAbsDifference(again, after) == 0.0);
        CHECK(second.dispose() == suiteError_NoError);
    }

    // ---- the OLD importer instance is handed its old blob again: ignored -------------
    {
        imFileInfoRec8 info{};
        const PrefsBlob stale = PrefsBlob::defaults();
        REQUIRE(harness.getInfo8(original, info, &stale) == imNoErr);
    }
    const std::vector<float> stillDarker = renderAndRead(instance, marker, out, t, kTicksPerFrame5994);
    CHECK(maxAbsDifference(stillDarker, after) == 0.0);

    // ---- Colour Output Rec.709 in this PQ sequence: Premiere's conversion rules --------
    PrefsBlob rec709 = darker;
    rec709.colorOutput = static_cast<std::uint8_t>(osv::premiere::PrefsColorOutput::Rec709);
    auto colourChanged = publishSourceSettings(harness, sample, rec709, 53);
    REQUIRE(colourChanged.open());
    CHECK(drawnBy(renderAndRead(instance, marker, out, t, kTicksPerFrame5994)) == DrawnBy::Equirect);

    // ---- ...unless the clip opts into the working space --------------------------------
    // Then the direct path renders PQ exactly as it did with the PQ output:
    // the colour output only decides what the IMPORTER's frame is encoded in.
    PrefsBlob optedIn = rec709;
    optedIn.directColour = static_cast<std::uint8_t>(osv::premiere::PrefsDirectColour::WorkingSpace);
    auto optedInInstance = publishSourceSettings(harness, sample, optedIn, 54);
    REQUIRE(optedInInstance.open());
    const std::vector<float> working = renderAndRead(instance, marker, out, t, kTicksPerFrame5994);
    CHECK(drawnBy(working) == DrawnBy::Direct);
    CHECK(maxAbsDifference(working, after) == 0.0);

    // ---- the log tells the story, one line per change -----------------------------------
    // The effect logs one decision line per (file, generation, working space,
    // verdict); the fresh instance above shares the file's line.  Four states
    // were rendered: the defaults, exposure -1, Rec.709 handed over, and the
    // opt-in.
    std::vector<std::string> decisions;
    for (const std::string& line : reframeLogLinesSince(logStart)) {
        if (line.find("reframe/direct: 'EXAMPLE_FOOTAGE_DLOGM.OSV' Source Settings generation") != std::string::npos) {
            decisions.push_back(line);
        }
    }
    for (const std::string& d : decisions) {
        WARN(d);
    }
    REQUIRE(decisions.size() == 4u);
    CHECK(decisions[0].find("exposure +0.00") != std::string::npos);
    CHECK(decisions[0].find("-> straight from the fisheyes") != std::string::npos);
    CHECK(decisions[1].find("exposure -1.00") != std::string::npos);
    CHECK(decisions[1].find("-> straight from the fisheyes") != std::string::npos);
    CHECK(decisions[2].find("colour Rec.709") != std::string::npos);
    CHECK(decisions[2].find("-> equirect route") != std::string::npos);
    CHECK(decisions[3].find("direct-path colour working space") != std::string::npos);
    CHECK(decisions[3].find("-> straight from the fisheyes") != std::string::npos);

    CHECK(instance.dispose() == suiteError_NoError);
    CHECK(host.totalNodeRefs() == 0);
    CHECK(host.invalidNodeReleases() == 0);
}
