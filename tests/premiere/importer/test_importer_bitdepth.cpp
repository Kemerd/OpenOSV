// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_importer_bitdepth.cpp - the importer's own frame: which pixel format
// it is negotiated and delivered in, and the GPU path that produces it.
//
// Two defects motivated everything here, both seen in a real Premiere 26.2.2
// session:
//
//   1. Every frame was requested as BGRA_4444_8u, for a clip that declares
//      "BT.2100 PQ RGB Full".  8-bit PQ bands, and the 10-bit source is
//      thrown away.  The importer's own imSelectClipFrameDescriptor2 answered
//      8u whenever the sequence's Maximum Bit Depth was off (the default), and
//      its format list offered 8u to every clip.
//   2. Every frame still decoded both lenses to host memory, uploaded them,
//      stitched, read 288 MB back and copied it twice, even when the reframe
//      effect renders its view from the fisheyes and ignores that frame.
//
// So these tests pin: the negotiation per signal class (never 8-bit for HDR
// or log unless the host explicitly asks, which the SDK obliges us to
// honour); the delivered formats and their precision; that the GPU frame
// path is really used and is byte-identical to the host path it replaces;
// that concurrent clips sharing the one CUDA renderer never tear each
// other's frames; and that the direct path being active never changes the
// importer's frame.
//
// [sample] tests SKIP without the 6K clip; [cuda] tests SKIP without a CUDA
// device.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "ImporterHarness.h"

#include "MockHost.h"
#include "OsvEngineAbi.h"
#include "PixelCopy.h"
#include "PrefsBlob.h"

#include "PrSDKColorSpaces.h"
#include "PrSDKMALErrors.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "osv/render/ImageRGBAf.h"

#include <cuda.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace osv::premiere;
using namespace osv::premiere::test;
using osv::premiere::mock::MockHost;
namespace pc = osv::premiere::pixelcopy;

namespace {

/// One 59.94 fps frame in Premiere ticks (254016000000 * 1001 / 60000).
constexpr PrTime kTicksPerFrame5994 = 4237833600LL;

/// SKIP the test when the sample clip is not present.
#define REQUIRE_SAMPLE_CLIP()                                                                  \
    do {                                                                                        \
        if (!sampleClipAvailable()) {                                                           \
            SKIP("the sample clip is not present at " << sampleClipPath().string());            \
        }                                                                                       \
    } while (false)

/// True when the CUDA driver loads and reports a device (the GPU frame path
/// and the engine need one).
[[nodiscard]] bool cudaDeviceAvailable() {
    if (cuInit(0) != CUDA_SUCCESS) {
        return false;
    }
    int count = 0;
    return cuDeviceGetCount(&count) == CUDA_SUCCESS && count > 0;
}

/// Prefs for the sample clip with the given output signal and every
/// analysis OFF: the frame path is what is under test, and with the
/// analyses off both paths build the identical parameter block.
[[nodiscard]] PrefsBlob plainPrefs(PrefsColorOutput color) {
    PrefsBlob p = PrefsBlob::defaults();
    p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    p.colorOutput = static_cast<std::uint8_t>(color);
    p.seamSearch = 0;
    p.gainMatch = 0;
    p.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    // [WP-FLARE] Sun ghost removal is on in the defaults now, and it is a
    // measured analysis like the three above: each path fits the ghosts from
    // the frames it has (device frames through the CUDA sampler, host frames
    // through the CPU one), so its fits differ by float noise.  Off here so a
    // "plain" frame is analysis-free; the analysis test below turns it on.
    p.flareRemoval = 0;
    // [WP-PHOTO] the photometric seam field is an analysis too (measured
    // from each path's own frames); the float-noise case below turns it on.
    p.photoSeam = static_cast<std::uint8_t>(PrefsPhotoSeam::Off);
    // Horizon lock stays ON: it only rotates the equirect, identically in
    // both paths, and a stabilised frame is the realistic one.
    return p;
}

/// A delivered frame: its format, its bytes in host row order (tight rows,
/// so two frames compare with ==), and the decoded top-down float picture.
struct Delivered {
    PrPixelFormat format = PrPixelFormat_Invalid;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;
    osv::render::ImageRGBAf rgba;
};

/// Bytes per pixel of the three formats the importer delivers.
[[nodiscard]] std::size_t bppOf(PrPixelFormat format) {
    switch (format) {
    case PrPixelFormat_BGRA_4444_32f: return pc::kBytesPerPixel32f;
    case PrPixelFormat_BGRA_4444_16u: return pc::kBytesPerPixel16u;
    case PrPixelFormat_BGRA_4444_8u:  return pc::kBytesPerPixel8u;
    default:                          return 0;
    }
}

/// Read a PPix the importer returned (and dispose it).
[[nodiscard]] Delivered grab(MockHost& host, const PrSDKPPixSuite* ppix, PPixHand hand) {
    Delivered d;
    REQUIRE(ppix != nullptr);
    REQUIRE(hand != nullptr);
    const auto info = host.inspect(hand);
    REQUIRE(info.has_value());
    d.format = info->format;
    d.width = info->width;
    d.height = info->height;
    const std::size_t bpp = bppOf(d.format);
    REQUIRE(bpp != 0);

    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) == suiteError_NoError);
    REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
    REQUIRE(pixels != nullptr);

    // Tight copy, host row order, so byte equality ignores row padding.
    const std::size_t tight = static_cast<std::size_t>(d.width) * bpp;
    d.bytes.resize(tight * d.height);
    for (std::uint32_t r = 0; r < d.height; ++r) {
        std::memcpy(d.bytes.data() + tight * r, pc::rowAddress(pixels, rowBytes, r), tight);
    }

    // Decode through PixelCopy's readers (bottom-left -> top-down, BGRA -> RGBA).
    const pc::ConstHostFrame frame(pixels, rowBytes, d.width, d.height);
    auto decoded = d.format == PrPixelFormat_BGRA_4444_32f   ? pc::hostBgra32fToRgba(frame, nullptr)
                   : d.format == PrPixelFormat_BGRA_4444_16u ? pc::hostBgra16uToRgba(frame, nullptr)
                                                             : pc::hostBgra8uToRgba(frame, nullptr);
    REQUIRE(decoded.ok());
    d.rgba = std::move(decoded).value();
    ppix->Dispose(hand);
    return d;
}

/// Largest per-channel difference between two decoded frames.
[[nodiscard]] float maxDiff(const Delivered& a, const Delivered& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.rgba.data.size(); ++i) {
        worst = std::max(worst, std::fabs(a.rgba.data[i] - b.rgba.data[i]));
    }
    return worst;
}

/// Byte equality of two delivered frames.  A function rather than a vector
/// ==, so a failing CHECK prints a bool instead of a 288 MB array.
[[nodiscard]] bool sameBytes(const Delivered& a, const Delivered& b) {
    return a.format == b.format && a.width == b.width && a.height == b.height && a.bytes == b.bytes;
}

/// The PPix suite of the mock host, released on scope exit.
struct PPixSuite {
    ImporterHarness& harness;
    const PrSDKPPixSuite* suite = nullptr;
    explicit PPixSuite(ImporterHarness& h) : harness(h) {
        const void* raw = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &raw) ==
                kSPNoError);
        suite = static_cast<const PrSDKPPixSuite*>(raw);
    }
    ~PPixSuite() { harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion); }
    PPixSuite(const PPixSuite&) = delete;
    PPixSuite& operator=(const PPixSuite&) = delete;
};

/// imGetSourceVideo for one frame in one format; the PPix is read and disposed.
[[nodiscard]] Delivered renderOne(ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                  const PrSDKPPixSuite* ppix, std::uint32_t frame, PrPixelFormat format,
                                  csSDK_int32 width, csSDK_int32 height, const PrefsBlob& prefs,
                                  const std::string& selectedProfile = {}) {
    ImporterHarness::SourceVideoRequest request;
    request.frameTime = kTicksPerFrame5994 * static_cast<PrTime>(frame);
    request.format = format;
    request.width = width;
    request.height = height;
    request.intent = imRenderIntent_Export;
    request.selectedColorProfileName = selectedProfile;
    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
    REQUIRE(hand != nullptr);
    return grab(harness.host(), ppix, hand);
}

/// imAnalysis text of an open clip (two-step protocol).
[[nodiscard]] std::string analysisOf(ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                     const PrefsBlob& prefs) {
    PrefsBlob blob = prefs;
    imAnalysisRec rec{};
    rec.privatedata = clip.privateData();
    rec.prefs = &blob;
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    REQUIRE(rec.buffersize > 0);
    // Headroom: a frame rendered between the two steps may add a line.
    std::vector<char> buffer(static_cast<std::size_t>(rec.buffersize) + 1024u, '\0');
    rec.buffer = buffer.data();
    rec.buffersize = static_cast<csSDK_int32>(buffer.size());
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    return std::string(buffer.data());
}

/// OPENOSV_IMPORTER_NO_GPU_DECODE for the lifetime of the object.  The .prm
/// shares this process's CRT (both /MD), so _putenv_s reaches its getenv_s.
struct NoGpuDecode {
    NoGpuDecode() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", "1"); }
    ~NoGpuDecode() { ::_putenv_s("OPENOSV_IMPORTER_NO_GPU_DECODE", ""); }
    NoGpuDecode(const NoGpuDecode&) = delete;
    NoGpuDecode& operator=(const NoGpuDecode&) = delete;
};

constexpr const char* kGpuPathLine = "Frame path: NVDEC decode into VRAM";
constexpr const char* kHostPathLine = "Frame path: decoded to host memory";

}  // namespace

// =============================================================================
//  Negotiation
// =============================================================================

TEST_CASE("imSelectClipFrameDescriptor never negotiates 8-bit for an HDR or log clip",
          "[importer][format][bitdepth][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const PrPixelFormat desires[] = {PrPixelFormat_BGRA_4444_32f, PrPixelFormat_BGRA_4444_16u,
                                     PrPixelFormat_BGRA_4444_8u, PrPixelFormat_VUYA_4444_32f};
    // -1 = the version 1 record (no Maximum Bit Depth at all).
    const int depths[] = {-1, kMaxBitDepth_Off, kMaxBitDepth_On, kMaxBitDepth_Unknown};
    const PrefsColorOutput colors[] = {PrefsColorOutput::PQ, PrefsColorOutput::HLG, PrefsColorOutput::Rec709,
                                       PrefsColorOutput::DLogM};

    for (const PrefsColorOutput color : colors) {
        PrefsBlob prefs = plainPrefs(color);
        const bool sdr = color == PrefsColorOutput::Rec709;
        const bool log = color == PrefsColorOutput::DLogM;
        for (const PrPixelFormat desired : desires) {
            for (const int depth : depths) {
                imClipFrameDescriptorRec2 rec{};
                rec.inPrivateData = clip.privateData();
                rec.inPrefs = &prefs;
                rec.inDesiredClipFrameDescriptor.inPixelFormat = desired;
                rec.inDesiredClipFrameDescriptor.inWidth = 6000;
                rec.inDesiredClipFrameDescriptor.inHeight = 3000;
                csSDK_int32 result = imOtherErr;
                if (depth < 0) {
                    result = harness.send(imSelectClipFrameDescriptor, nullptr, static_cast<imClipFrameDescriptorRec*>(&rec));
                } else {
                    rec.inDesiredMaxBitDepth = static_cast<csSDK_uint32>(depth);
                    result = harness.send(imSelectClipFrameDescriptor2, nullptr, &rec);
                }
                INFO("colour " << static_cast<int>(color) << ", desired 0x" << std::hex
                               << static_cast<unsigned>(desired) << std::dec << ", max bit depth " << depth);
                REQUIRE(result == imNoErr);
                const PrPixelFormat answer = rec.outBestFrameDescriptor.inPixelFormat;
                const bool off = depth == kMaxBitDepth_Off;
                const bool produced = desired == PrPixelFormat_BGRA_4444_32f ||
                                      desired == PrPixelFormat_BGRA_4444_16u || desired == PrPixelFormat_BGRA_4444_8u;
                if (log) {
                    // The unbounded log code fits no integer format.
                    CHECK(answer == PrPixelFormat_BGRA_4444_32f);
                } else if (sdr) {
                    // SDR is unchanged: Off means the cheap 8-bit path.
                    CHECK(answer == (off ? PrPixelFormat_BGRA_4444_8u
                                         : (produced ? desired : PrPixelFormat_BGRA_4444_32f)));
                } else {
                    // HDR: never 8-bit.  Off -> 16u, else a high-bit wish is kept.
                    CHECK(answer != PrPixelFormat_BGRA_4444_8u);
                    if (off) {
                        CHECK(answer == PrPixelFormat_BGRA_4444_16u);
                    } else if (desired == PrPixelFormat_BGRA_4444_32f || desired == PrPixelFormat_BGRA_4444_16u) {
                        CHECK(answer == desired);
                    } else {
                        CHECK(answer == PrPixelFormat_BGRA_4444_32f);
                    }
                }
                // The size snapping and the fixed fields are unaffected.
                CHECK(rec.outBestFrameDescriptor.inWidth == 6000);
                CHECK(rec.outBestFrameDescriptor.inHeight == 3000);
            }
        }
    }
}

TEST_CASE("imGetIndPixelFormat follows the clip's Source Settings, from the record or the live instance",
          "[importer][format][bitdepth][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    // Collect the whole list for a record.
    auto listFor = [&](const void* prefs) {
        std::vector<PrPixelFormat> out;
        for (csSDK_int32 i = 0; i < 8; ++i) {
            imIndPixelFormatRec rec{};
            rec.privatedata = clip.privateData();
            rec.prefs = prefs;
            const csSDK_int32 r = harness.sendIndexed(imGetIndPixelFormat, i, &rec);
            if (r == imBadFormatIndex) {
                break;
            }
            REQUIRE(r == imNoErr);
            out.push_back(rec.outPixelFormat);
        }
        return out;
    };

    SECTION("from the record's prefs") {
        const PrefsBlob hlg = plainPrefs(PrefsColorOutput::HLG);
        const PrefsBlob sdr = plainPrefs(PrefsColorOutput::Rec709);
        const PrefsBlob log = plainPrefs(PrefsColorOutput::DLogM);
        CHECK(listFor(&hlg) == std::vector<PrPixelFormat>{PrPixelFormat_BGRA_4444_32f, PrPixelFormat_BGRA_4444_16u});
        CHECK(listFor(&sdr) == std::vector<PrPixelFormat>{PrPixelFormat_BGRA_4444_32f, PrPixelFormat_BGRA_4444_8u});
        CHECK(listFor(&log) == std::vector<PrPixelFormat>{PrPixelFormat_BGRA_4444_32f});
    }

    SECTION("from the live instance when the record carries none") {
        // imGetInfo8 hands the instance Rec.709; an enumeration without a blob
        // must describe THAT clip, not the defaults.
        const PrefsBlob sdr = plainPrefs(PrefsColorOutput::Rec709);
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &sdr) == imNoErr);
        CHECK(listFor(nullptr) == std::vector<PrPixelFormat>{PrPixelFormat_BGRA_4444_32f, PrPixelFormat_BGRA_4444_8u});
    }
}

// =============================================================================
//  Delivery
// =============================================================================

TEST_CASE("imGetSourceVideo delivers 16u within half a code, and honours an explicit 8u",
          "[importer][video][bitdepth][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const PrefsBlob pq = plainPrefs(PrefsColorOutput::PQ);
    constexpr std::uint32_t kFrame = 12;
    const Delivered f32 = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 3000, 1500, pq);
    REQUIRE(f32.format == PrPixelFormat_BGRA_4444_32f);

    SECTION("16u is the float frame quantised to 0..32768, nothing else") {
        const Delivered f16 = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_16u, 3000, 1500, pq);
        REQUIRE(f16.format == PrPixelFormat_BGRA_4444_16u);
        const float worst = maxDiff(f32, f16);
        INFO("largest difference " << worst << " (half a 16u code = " << 0.5f / 32768.0f << ")");
        REQUIRE(worst <= 0.5f / 32768.0f + 1e-7f);
        // The 16u codes really use the 0..32768 scale, not 0..65535: nothing
        // above white.
        for (std::size_t i = 0; i + 1 < f16.bytes.size(); i += 2) {
            std::uint16_t code = 0;
            std::memcpy(&code, f16.bytes.data() + i, sizeof(code));
            REQUIRE(code <= pc::kWhite16u);
        }
    }

    SECTION("an explicit 8u request for an HDR clip is honoured (the SDK obliges it)") {
        const Delivered f8 = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_8u, 3000, 1500, pq);
        REQUIRE(f8.format == PrPixelFormat_BGRA_4444_8u);
        REQUIRE(maxDiff(f32, f8) <= 0.5f / 255.0f + 1e-6f);
    }

    SECTION("PrPixelFormat_Any and a format the importer does not make get the clip's preferred 32f") {
        const Delivered any = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_Any, 3000, 1500, pq);
        CHECK(any.format == PrPixelFormat_BGRA_4444_32f);
        CHECK(sameBytes(any, f32));
        // Nothing requested is usable: the preferred format at NATIVE size,
        // as imGetSourceVideo has always answered that case.
        const Delivered vuya = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_VUYA_4444_32f, 3000, 1500, pq);
        CHECK(vuya.format == PrPixelFormat_BGRA_4444_32f);
        CHECK(vuya.width == 6000);
        CHECK(vuya.height == 3000);
    }

    SECTION("the unbounded log output answers a 16u request with lossless 32f") {
        const PrefsBlob log = plainPrefs(PrefsColorOutput::DLogM);
        const Delivered log32 = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 3000, 1500, log);
        const Delivered log16 = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_16u, 3000, 1500, log);
        CHECK(log16.format == PrPixelFormat_BGRA_4444_32f);
        CHECK(sameBytes(log16, log32));
    }
}

// =============================================================================
//  The GPU frame path
// =============================================================================

TEST_CASE("the GPU frame path is used and is byte-identical to the host path",
          "[importer][video][gpupath][sample][cuda]") {
    REQUIRE_SAMPLE_CLIP();
    if (!cudaDeviceAvailable()) {
        SKIP("no CUDA device: the GPU frame path cannot run here");
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);

    // Frames: the first keyframe, one inside the first GOP, one across the
    // sync sample at 61 and the last.  Native size, so the readback streams
    // several pinned bands.
    const std::uint32_t frames[] = {0, 7, 62, 64};
    const PrefsBlob pq = plainPrefs(PrefsColorOutput::PQ);

    // ---- the GPU path --------------------------------------------------------
    std::vector<Delivered> gpu;
    std::string gpuAnalysis;
    {
        auto clip = harness.openClip(sampleClipPath(), 301);
        REQUIRE(clip.open());
        for (const std::uint32_t f : frames) {
            gpu.push_back(renderOne(harness, clip, ppix.suite, f, PrPixelFormat_BGRA_4444_32f, 6000, 3000, pq));
        }
        gpu.push_back(renderOne(harness, clip, ppix.suite, 7, PrPixelFormat_BGRA_4444_16u, 6000, 3000, pq));
        gpu.push_back(renderOne(harness, clip, ppix.suite, 7, PrPixelFormat_BGRA_4444_8u, 6000, 3000, pq));
        gpuAnalysis = analysisOf(harness, clip, pq);
    }
    INFO(gpuAnalysis);
    REQUIRE(gpuAnalysis.find(kGpuPathLine) != std::string::npos);

    // ---- the host path, forced by the documented switch --------------------------
    std::vector<Delivered> host;
    std::string hostAnalysis;
    {
        NoGpuDecode off;
        auto clip = harness.openClip(sampleClipPath(), 302);
        REQUIRE(clip.open());
        for (const std::uint32_t f : frames) {
            host.push_back(renderOne(harness, clip, ppix.suite, f, PrPixelFormat_BGRA_4444_32f, 6000, 3000, pq));
        }
        host.push_back(renderOne(harness, clip, ppix.suite, 7, PrPixelFormat_BGRA_4444_16u, 6000, 3000, pq));
        host.push_back(renderOne(harness, clip, ppix.suite, 7, PrPixelFormat_BGRA_4444_8u, 6000, 3000, pq));
        hostAnalysis = analysisOf(harness, clip, pq);
    }
    INFO(hostAnalysis);
    REQUIRE(hostAnalysis.find(kHostPathLine) != std::string::npos);

    // ---- byte for byte ------------------------------------------------------------
    // Same decoded pixels (both hardware decoders are bit-exact HEVC), same
    // parameter block (buildEquirectJob), same CUDA kernel, same conversion
    // kernels (a band streamed through PixelCopy writes exactly what the
    // whole-frame copy writes).
    REQUIRE(gpu.size() == host.size());
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        INFO("delivery " << i);
        REQUIRE(gpu[i].format == host[i].format);
        CHECK(sameBytes(gpu[i], host[i]));
    }
}

namespace {

/// Body-frame latitude, in degrees from the seam plane, of Standard-layout
/// equirect pixel (x, y) with stabilisation OFF (body == view): the lens
/// axes are +/-Y, and a direction's Y component is cos(lon) * cos(lat).
[[nodiscard]] double seamLatitudeDeg(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    constexpr double kPi = 3.14159265358979323846;
    const double lon = (static_cast<double>(x) + 0.5) / w * 2.0 * kPi - kPi;
    const double lat = 0.5 * kPi - (static_cast<double>(y) + 0.5) / h * kPi;
    return std::asin(std::clamp(std::cos(lon) * std::cos(lat), -1.0, 1.0)) * 180.0 / kPi;
}

/// How two renders of one frame differ, inside and outside the overlap band
/// (|seam latitude| <= 9.5 deg: the parallax grid's span plus margin, and
/// far beyond the seam blend's 4 degree feather).
struct PathDifference {
    std::uint64_t pixelsInside = 0;    ///< Band pixels with any channel different.
    std::uint64_t pixelsOutside = 0;   ///< Same, outside the band.
    std::uint64_t bandPixels = 0;      ///< Pixels in the band.
    float worstInside = 0.0f;          ///< Largest channel difference in the band.
    float worstOutside = 0.0f;         ///< Largest channel difference outside it.
    double meanInside = 0.0;           ///< Mean absolute channel difference over the band.
};

/// `bandDeg` widens the band for an analysis that reaches further ([WP-PHOTO]
/// the photometric gain field decays 20 degrees beyond its 9 degree rows).
[[nodiscard]] PathDifference pathDifference(const Delivered& a, const Delivered& b, double bandDeg = 9.5) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    PathDifference d;
    double sumInside = 0.0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        const float* p = a.rgba.row(y);
        const float* q = b.rgba.row(y);
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const bool inside = std::fabs(seamLatitudeDeg(x, y, a.width, a.height)) <= bandDeg;
            float worst = 0.0f;
            for (int c = 0; c < 4; ++c) {
                const float e = std::fabs(p[x * 4 + c] - q[x * 4 + c]);
                worst = std::max(worst, e);
                if (inside) {
                    sumInside += e;
                }
            }
            if (inside) {
                ++d.bandPixels;
                d.pixelsInside += worst > 0.0f ? 1u : 0u;
                d.worstInside = std::max(d.worstInside, worst);
            } else {
                d.pixelsOutside += worst > 0.0f ? 1u : 0u;
                d.worstOutside = std::max(d.worstOutside, worst);
            }
        }
    }
    d.meanInside = d.bandPixels ? sumInside / (4.0 * static_cast<double>(d.bandPixels)) : 0.0;
    return d;
}

}  // namespace

TEST_CASE("with an analysis on, the two frame paths differ only by that analysis' float noise",
          "[importer][video][gpupath][sample][cuda]") {
    REQUIRE_SAMPLE_CLIP();
    if (!cudaDeviceAvailable()) {
        SKIP("no CUDA device: the GPU frame path cannot run here");
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);

    // The measured analyses shade their bands from the frames each path has:
    // on the GPU from device frames, on the CPU from host frames - two
    // shaders 108-111 dB apart (docs/DIRECT_GPU.md, WP-B).  The seam search's
    // dynamic programme and the flow solver's consistency gates can turn
    // those last bits into a different choice, so with an analysis on the
    // paths are NOT byte-identical.  What must hold is that the difference
    // stays where the analysis acts: a seam or parallax change inside the
    // overlap band only, a gain change as a tiny global scale, a sun ghost
    // removal change far below a 10-bit code in the ghosts.  (The GPU
    // path's analyses are the ones the effect's direct path computes, so
    // the importer's equirect and the direct view now agree with each other.)
    // Stabilisation off, so the band can be located in the equirect.
    PrefsBlob base = plainPrefs(PrefsColorOutput::PQ);
    base.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
    base.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Classical);

    struct Case {
        const char* name;
        bool seam;
        bool gain;
        bool parallax;
        bool flare;  ///< [WP-FLARE] sun ghost removal
        bool photo;  ///< [WP-PHOTO] the sky seam fix (rim and gain field)
    };
    const Case cases[] = {
        {"seam search", true, false, false, false},
        {"parallax", false, false, true, false},
        {"gain match", false, true, false, false},
        {"sun ghost removal", false, false, false, true, false},  // [WP-FLARE]
        {"sky seam fix", false, false, false, false, true},       // [WP-PHOTO]
    };
    csSDK_int32 id = 311;
    for (const Case& c : cases) {
        PrefsBlob prefs = base;
        prefs.seamSearch = c.seam ? 1 : 0;
        prefs.gainMatch = c.gain ? 1 : 0;
        prefs.parallax = static_cast<std::uint8_t>(c.parallax ? PrefsParallax::On : PrefsParallax::Off);
        prefs.flareRemoval = c.flare ? 1 : 0;
        prefs.photoSeam = static_cast<std::uint8_t>(c.photo ? PrefsPhotoSeam::RimAndGain : PrefsPhotoSeam::Off);
        constexpr std::uint32_t kFrame = 20;

        Delivered gpu;
        {
            auto clip = harness.openClip(sampleClipPath(), id++);
            REQUIRE(clip.open());
            gpu = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 3000, 1500, prefs);
            REQUIRE(analysisOf(harness, clip, prefs).find(kGpuPathLine) != std::string::npos);
        }
        Delivered host;
        {
            NoGpuDecode off;
            auto clip = harness.openClip(sampleClipPath(), id++);
            REQUIRE(clip.open());
            host = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 3000, 1500, prefs);
            REQUIRE(analysisOf(harness, clip, prefs).find(kHostPathLine) != std::string::npos);
        }

        // [WP-PHOTO] the field's rows span +-9 deg and its gain decays over
        // 20 deg beyond them: nothing past 29 deg may differ.
        const PathDifference d = pathDifference(gpu, host, c.photo ? 30.0 : 9.5);
        INFO(c.name << ": band pixels differing " << d.pixelsInside << " / " << d.bandPixels << " (worst "
                    << d.worstInside << ", mean " << d.meanInside << "), outside the band " << d.pixelsOutside
                    << " (worst " << d.worstOutside << ")");
        if (c.flare) {
            // [WP-FLARE] Each path fits the sun's ghosts from its own frames
            // (CUDA sampler on device frames, CPU sampler on host frames), so
            // the subtracted ghosts differ by the fit's float noise: measured
            // 264 pixels, worst 7.4e-6, none in the band (the ghosts sit near
            // the sun, far from the seam).  A ghost one path fitted and the
            // other rejected would differ by tenths, so half a 10-bit code
            // still catches that with room for the noise.
            CHECK(d.worstOutside <= 0.5f / 1023.0f);
            CHECK(d.worstInside <= 0.5f / 1023.0f);
            // And the comparison is not vacuous: the removal really acted on
            // this frame (the host path with it off is visibly different).
            PrefsBlob plain = prefs;
            plain.flareRemoval = 0;
            Delivered hostOff;
            {
                NoGpuDecode off;
                auto clip = harness.openClip(sampleClipPath(), id++);
                REQUIRE(clip.open());
                hostOff = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 3000, 1500, plain);
            }
            const float removed = maxDiff(host, hostOff);
            INFO("largest change the removal made " << removed);
            CHECK(removed > 0.01f);
        } else if (c.gain) {
            // Per-lens gains from slightly different bands: a global scale a
            // hair apart (measured 8e-6) - below half a 16-bit code anywhere.
            CHECK(d.worstOutside <= 0.5f / 32768.0f);
            CHECK(d.worstInside <= 0.5f / 32768.0f);
        } else if (c.photo) {
            // [WP-PHOTO] The field from device bands on one path and host bands
            // on the other: exactly nothing beyond its reach, and across it
            // the two fields' float noise - a gain a hair apart, like the
            // gain match (measured worst 1.0e-5, mean 3e-7): below half a
            // 16-bit code anywhere.
            CHECK(d.pixelsOutside == 0);
            CHECK(d.worstInside <= 0.5f / 32768.0f);
        } else if (c.seam) {
            // The seam table shifts every pixel of a column along its
            // meridian (osv_kernel.h, osvShiftTowardAxis), not just the band,
            // so a table a hair apart moves the whole picture by a hair:
            // measured 8e-5 at worst, a twelfth of a 10-bit code.
            CHECK(d.worstOutside <= 0.5f / 1023.0f);
            CHECK(d.worstInside <= 0.5f / 1023.0f);
        } else {
            // The parallax grid acts inside the band only - exactly zero
            // outside it (the kernel returns no correction beyond its span).
            CHECK(d.pixelsOutside == 0);
            // Inside, a flow cell gated differently moves the stitch by a
            // pixel in a textured spot (measured worst 0.09 in single pixels)
            // but the band as a whole agrees to a fraction of a 10-bit code
            // (measured mean 1.4e-4).
            CHECK(d.meanInside <= 1.0 / 1023.0);
        }
    }
}

TEST_CASE("clips rendering at once through the shared CUDA renderer never tear each other's frames",
          "[importer][video][gpupath][concurrency][sample][cuda]") {
    REQUIRE_SAMPLE_CLIP();
    if (!cudaDeviceAvailable()) {
        SKIP("no CUDA device: the GPU frame path cannot run here");
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);

    // Two instances with DIFFERENT output signals, so a frame read back from
    // the renderer's buffer after the other clip overwrote it would be
    // visibly wrong - and one of them on the host path, whose renderInto
    // shares the renderer's output buffer too.
    const PrefsBlob pq = plainPrefs(PrefsColorOutput::PQ);
    const PrefsBlob hlg = plainPrefs(PrefsColorOutput::HLG);
    auto clipA = harness.openClip(sampleClipPath(), 321);
    REQUIRE(clipA.open());
    ImporterHarness::ClipHandle clipB;
    {
        NoGpuDecode off;  // chosen at clip B's first frame, below
        clipB = harness.openClip(sampleClipPath(), 322);
        REQUIRE(clipB.open());
        (void)renderOne(harness, clipB, ppix.suite, 3, PrPixelFormat_BGRA_4444_32f, 3000, 1500, hlg);
    }

    // Serial references.
    const Delivered refA = renderOne(harness, clipA, ppix.suite, 5, PrPixelFormat_BGRA_4444_32f, 3000, 1500, pq);
    const Delivered refB = renderOne(harness, clipB, ppix.suite, 5, PrPixelFormat_BGRA_4444_32f, 3000, 1500, hlg);
    REQUIRE(!sameBytes(refA, refB));

    // Hammer both at once.  The host cache would serve repeats without
    // rendering, so frames 5 and 6 alternate and the cache is cleared; the
    // second frame's references are taken serially first as well.
    const Delivered refA6 = renderOne(harness, clipA, ppix.suite, 6, PrPixelFormat_BGRA_4444_32f, 3000, 1500, pq);
    const Delivered refB6 = renderOne(harness, clipB, ppix.suite, 6, PrPixelFormat_BGRA_4444_32f, 3000, 1500, hlg);
    std::atomic<int> mismatches{0};
    std::atomic<int> failures{0};
    auto worker = [&](ImporterHarness::ClipHandle& clip, const PrefsBlob& prefs, const Delivered& ref5,
                      const Delivered& ref6) {
        for (int i = 0; i < 6; ++i) {
            const std::uint32_t frame = (i % 2 == 0) ? 5u : 6u;
            ImporterHarness::SourceVideoRequest request;
            request.frameTime = kTicksPerFrame5994 * static_cast<PrTime>(frame);
            request.format = PrPixelFormat_BGRA_4444_32f;
            request.width = 3000;
            request.height = 1500;
            PPixHand hand = nullptr;
            // No REQUIRE off the main thread (Catch2 assertions are not
            // thread-safe): count, and assert afterwards.
            if (harness.getSourceVideo(clip, request, prefs, hand) != imNoErr || !hand) {
                failures.fetch_add(1);
                continue;
            }
            char* pixels = nullptr;
            csSDK_int32 rowBytes = 0;
            (void)ppix.suite->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels);
            (void)ppix.suite->GetRowBytes(hand, &rowBytes);
            const Delivered& ref = frame == 5u ? ref5 : ref6;
            const std::size_t tight = static_cast<std::size_t>(ref.width) * pc::kBytesPerPixel32f;
            bool same = pixels != nullptr;
            for (std::uint32_t r = 0; same && r < ref.height; ++r) {
                same = std::memcmp(pc::rowAddress(pixels, rowBytes, r), ref.bytes.data() + tight * r, tight) == 0;
            }
            if (!same) {
                mismatches.fetch_add(1);
            }
            ppix.suite->Dispose(hand);
            harness.host().clearCache();
        }
    };
    std::thread a([&] { worker(clipA, pq, refA, refA6); });
    std::thread b([&] { worker(clipB, hlg, refB, refB6); });
    a.join();
    b.join();
    REQUIRE(failures.load() == 0);
    REQUIRE(mismatches.load() == 0);
}

// =============================================================================
//  The direct path being active changes nothing
// =============================================================================

TEST_CASE("the importer's frame is unchanged while the effect's direct path renders the clip",
          "[importer][video][engine][sample][cuda]") {
    REQUIRE_SAMPLE_CLIP();
    if (!cudaDeviceAvailable()) {
        SKIP("no CUDA device: the engine cannot run here");
    }
    // A private context, as Premiere's is, created before the harness so it
    // outlives imShutdown (which releases the engine's decoders in it).
    struct PrivateContext {
        CUcontext context = nullptr;
        PrivateContext() {
            CUdevice device = 0;
            if (cuDeviceGet(&device, 0) == CUDA_SUCCESS && cuCtxCreate(&context, 0, device) == CUDA_SUCCESS) {
                CUcontext popped = nullptr;
                (void)cuCtxPopCurrent(&popped);
            } else {
                context = nullptr;
            }
        }
        ~PrivateContext() {
            if (context) {
                (void)cuCtxDestroy(context);
            }
        }
    } cuda;
    REQUIRE(cuda.context != nullptr);

    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);
    const HMODULE module = GetModuleHandleW(OSV_ENGINE_MODULE_NAME);
    REQUIRE(module != nullptr);
    const auto acquire =
        reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
    const auto release =
        reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
    REQUIRE(acquire != nullptr);
    REQUIRE(release != nullptr);

    const PrefsBlob pq = plainPrefs(PrefsColorOutput::PQ);
    auto clip = harness.openClip(sampleClipPath(), 331);
    REQUIRE(clip.open());
    constexpr std::uint32_t kFrame = 9;
    const Delivered before = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 3000, 1500, pq);
    CHECK(analysisOf(harness, clip, pq).find("Direct path:") == std::string::npos);

    // The effect renders the same frame directly.
    const std::wstring path = sampleClipPath().wstring();
    OsvEngineFrameRequest request{};
    request.structSize = sizeof(OsvEngineFrameRequest);
    request.path = path.c_str();
    request.mediaTicks = kTicksPerFrame5994 * static_cast<std::int64_t>(kFrame);
    request.purpose = OSV_ENGINE_PURPOSE_EXACT;
    request.outputTransfer = OSV_ENGINE_TRANSFER_FROM_CLIP;
    request.cuContext = cuda.context;
    OsvEngineFrame frame{};
    frame.structSize = sizeof(OsvEngineFrame);
    char error[512] = {};
    const std::int32_t rc = acquire(&request, &frame, error, sizeof(error));
    INFO("engine error: " << error);
    REQUIRE(rc == OSV_ENGINE_OK);
    release(frame.lease, nullptr);

    // The importer knows (the Properties panel says so) ...
    const std::string analysis = analysisOf(harness, clip, pq);
    INFO(analysis);
    CHECK(analysis.find("Direct path:") != std::string::npos);
    CHECK(analysis.find("(active now)") != std::string::npos);

    // ... and its frame is exactly what it was: it is the effect's fallback
    // and every other view's picture, and the host cache cannot tell the
    // requests apart, so it must never be degraded.
    harness.host().clearCache();
    const Delivered after = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 3000, 1500, pq);
    CHECK(sameBytes(after, before));
}

// =============================================================================
//  The Rec.709 connection-space override is local to its request
// =============================================================================

TEST_CASE("the host's Rec.709 connection space overrides one request, never the clip or the engine",
          "[importer][video][color][engine][sample][cuda]") {
    REQUIRE_SAMPLE_CLIP();
    if (!cudaDeviceAvailable()) {
        SKIP("no CUDA device: the engine cannot run here");
    }
    // The effect's context, created before the harness so it outlives
    // imShutdown (which releases the engine's decoders in it).
    struct PrivateContext {
        CUcontext context = nullptr;
        PrivateContext() {
            CUdevice device = 0;
            if (cuDeviceGet(&device, 0) == CUDA_SUCCESS && cuCtxCreate(&context, 0, device) == CUDA_SUCCESS) {
                CUcontext popped = nullptr;
                (void)cuCtxPopCurrent(&popped);
            } else {
                context = nullptr;
            }
        }
        ~PrivateContext() {
            if (context) {
                (void)cuCtxDestroy(context);
            }
        }
    } cuda;
    REQUIRE(cuda.context != nullptr);

    ImporterHarness harness;
    REQUIRE(harness.loaded());
    PPixSuite ppix(harness);
    const HMODULE module = GetModuleHandleW(OSV_ENGINE_MODULE_NAME);
    REQUIRE(module != nullptr);
    const auto acquire =
        reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
    const auto release =
        reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
    REQUIRE(acquire != nullptr);
    REQUIRE(release != nullptr);

    // The transfer the engine renders the clip with when the effect asks for
    // "the clip's own" - i.e. what the importer's instances published.
    const std::wstring path = sampleClipPath().wstring();
    auto engineTransfer = [&]() {
        OsvEngineFrameRequest request{};
        request.structSize = sizeof(OsvEngineFrameRequest);
        request.path = path.c_str();
        request.mediaTicks = 0;
        request.purpose = OSV_ENGINE_PURPOSE_EXACT;
        request.outputTransfer = OSV_ENGINE_TRANSFER_FROM_CLIP;
        request.cuContext = cuda.context;
        OsvEngineFrame frame{};
        frame.structSize = sizeof(OsvEngineFrame);
        char error[512] = {};
        const std::int32_t rc = acquire(&request, &frame, error, sizeof(error));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        const int transfer = frame.stitch.color.transfer;
        release(frame.lease, nullptr);
        return transfer;
    };

    const PrefsBlob pq = plainPrefs(PrefsColorOutput::PQ);
    const PrefsBlob rec709 = plainPrefs(PrefsColorOutput::Rec709);
    const bool gpuPath = GENERATE(true, false);
    INFO((gpuPath ? "GPU frame path" : "host frame path"));
    std::unique_ptr<NoGpuDecode> hostOnly;
    if (!gpuPath) {
        hostOnly = std::make_unique<NoGpuDecode>();
    }

    auto clip = harness.openClip(sampleClipPath(), gpuPath ? 341 : 342);
    REQUIRE(clip.open());
    constexpr std::uint32_t kFrame = 4;

    // The settings the engine holds for the file (no decode, no GPU):
    // their generation moves whenever a publication changes them.
    const auto query =
        reinterpret_cast<OsvEngineQuerySettingsFn>(GetProcAddress(module, OSV_ENGINE_SYM_QUERY_SETTINGS));
    REQUIRE(query != nullptr);
    auto settings = [&]() {
        OsvEngineClipSettings s{};
        s.structSize = sizeof(OsvEngineClipSettings);
        char error[512] = {};
        REQUIRE(query(path.c_str(), &s, error, sizeof(error)) == OSV_ENGINE_OK);
        return s;
    };

    // An ordinary PQ frame, and the clip's PQ reaches the engine.
    const Delivered ordinary = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 1920, 960, pq);
    CHECK(engineTransfer() == OSV_TRANSFER_PQ);
    const OsvEngineClipSettings before = settings();
    CHECK(before.generation != 0);
    CHECK(before.clipTransfer == OSV_TRANSFER_PQ);

    // The host falls back to the connection space for one request: that
    // frame is Rec.709 - exactly the clip rendered with Rec.709 chosen ...
    harness.host().clearCache();
    const Delivered fallback =
        renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 1920, 960, pq, kPrOverranged709);
    CHECK(maxDiff(fallback, ordinary) > 0.02f);

    // ... but the clip's settings did not change: nothing was published (the
    // generation stands still) and the engine still renders PQ.  (The
    // override used to go through applyPrefsLocked and publish "Rec.709"
    // here, so every direct view of the clip turned SDR.)
    const OsvEngineClipSettings after = settings();
    CHECK(after.generation == before.generation);
    CHECK(after.clipTransfer == OSV_TRANSFER_PQ);
    CHECK(after.colorOutput == before.colorOutput);
    CHECK(engineTransfer() == OSV_TRANSFER_PQ);

    // The next ordinary request of the same frame is PQ again, bit for bit -
    // not the Rec.709 frame the host path's one-frame cache still holds.
    harness.host().clearCache();
    const Delivered again = renderOne(harness, clip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 1920, 960, pq);
    CHECK(sameBytes(again, ordinary));

    // And the fallback frame is the same picture a Rec.709 clip produces.
    // Rendered on a second instance: switching this clip's own settings to
    // Rec.709 would publish them - which is fine for a real change, but is
    // not what is under test.
    auto sdrClip = harness.openClip(sampleClipPath(), gpuPath ? 343 : 344);
    REQUIRE(sdrClip.open());
    const Delivered sdr = renderOne(harness, sdrClip, ppix.suite, kFrame, PrPixelFormat_BGRA_4444_32f, 1920, 960, rec709);
    CHECK(sameBytes(fallback, sdr));
}
