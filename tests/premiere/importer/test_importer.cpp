// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests of the built OpenOSVImporter.prm, driven through the mock host.
//
// Everything here loads the real module with LoadLibraryW and calls
// xImportEntry, so what is tested is the binary that goes into
// MediaCore\OpenOSV - its exports, its resources, its delay-load table and
// its behaviour.
//
// Tests tagged [sample] need the 6K clip named by OSV_SAMPLE_CLIP_PATH and
// SKIP when it is absent, so a clone without the (large, unredistributable)
// clip still runs the rest of the suite.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ImporterHarness.h"

#include "MockHost.h"

#include "PrSDKColorSpaces.h"
#include "PrSDKImmersiveVideoTypes.h"
#include "PrSDKMALErrors.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKPixelFormat.h"

#include "PrefsBlob.h"
// kSourceSettingsMatchNameW: the one spelling of the Source Settings effect's
// match name.  The same header the effect's PiPL is generated from, which is
// what makes the importer/effect binding provably one string.
#include "SourceSettingsIdentity.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::test;
using osv::premiere::mock::MockHost;

namespace {

/// The exact Premiere tick base.
constexpr PrTime kTicksPerSecond = 254016000000LL;

/// One 59.94 fps (60000/1001) frame in ticks.  254016000000 * 1001 / 60000 =
/// 4237833600, exactly - which is the whole point of computing the frame
/// period from the container rational instead of from a rounded double.
constexpr PrTime kTicksPerFrame5994 = 4237833600LL;

/// Facts about the sample clip, verified with `osvtool probe`:
///   6K mode, 3000 x 3000 per lens -> 6000 x 3000 stitched,
///   65 frames at 59.94 fps, one stereo AAC track at 48 kHz.
constexpr csSDK_int32 kSampleWidth = 6000;
constexpr csSDK_int32 kSampleHeight = 3000;
constexpr csSDK_int64 kSampleFrames = 65;
constexpr csSDK_int32 kSampleAudioChannels = 2;
constexpr float kSampleSampleRate = 48000.0f;

/// SKIP the test when the sample clip is not present.
#define REQUIRE_SAMPLE_CLIP()                                                                  \
    do {                                                                                        \
        if (!sampleClipAvailable()) {                                                           \
            SKIP("the sample clip is not present at " << sampleClipPath().string());            \
        }                                                                                       \
    } while (false)

/// Read a whole PPix back into a float RGBA image (top-down, r,g,b,a), doing
/// the bottom-left -> top-down flip and the BGRA -> RGBA swap that the host
/// convention requires.  Works for both formats the importer produces.
struct DecodedFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;  ///< 4 * w * h, row 0 = TOP of the picture.

    [[nodiscard]] const float* pixel(std::uint32_t x, std::uint32_t y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4u;
    }
};

[[nodiscard]] DecodedFrame readPPix(MockHost& host, const PrSDKPPixSuite* ppix, PPixHand hand) {
    DecodedFrame frame;
    REQUIRE(ppix != nullptr);
    REQUIRE(hand != nullptr);

    const auto info = host.inspect(hand);
    REQUIRE(info.has_value());
    frame.width = info->width;
    frame.height = info->height;
    REQUIRE(frame.width > 0);
    REQUIRE(frame.height > 0);

    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) == suiteError_NoError);
    REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
    REQUIRE(pixels != nullptr);
    REQUIRE(rowBytes != 0);

    frame.rgba.assign(static_cast<std::size_t>(frame.width) * frame.height * 4u, 0.0f);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        // Host row 0 is the BOTTOM scanline, so picture row y lives at host
        // row (height - 1 - y).
        const std::uint32_t hostRow = frame.height - 1u - y;
        const char* src = pixels + static_cast<std::ptrdiff_t>(rowBytes) * static_cast<std::ptrdiff_t>(hostRow);
        float* dst = frame.rgba.data() + static_cast<std::size_t>(y) * frame.width * 4u;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            if (info->format == PrPixelFormat_BGRA_4444_32f) {
                const auto* p = reinterpret_cast<const float*>(src) + static_cast<std::size_t>(x) * 4u;
                dst[x * 4 + 0] = p[2];  // R  <- B,G,R,A
                dst[x * 4 + 1] = p[1];
                dst[x * 4 + 2] = p[0];
                dst[x * 4 + 3] = p[3];
            } else {
                const auto* p = reinterpret_cast<const std::uint8_t*>(src) + static_cast<std::size_t>(x) * 4u;
                dst[x * 4 + 0] = static_cast<float>(p[2]) / 255.0f;
                dst[x * 4 + 1] = static_cast<float>(p[1]) / 255.0f;
                dst[x * 4 + 2] = static_cast<float>(p[0]) / 255.0f;
                dst[x * 4 + 3] = static_cast<float>(p[3]) / 255.0f;
            }
        }
    }
    return frame;
}

/// Mean luma-ish brightness of a band of rows (used for the sky-vs-ground
/// orientation check).
[[nodiscard]] double meanBrightness(const DecodedFrame& frame, std::uint32_t firstRow, std::uint32_t rowCount) {
    double sum = 0.0;
    std::uint64_t n = 0;
    const std::uint32_t last = std::min(frame.height, firstRow + rowCount);
    for (std::uint32_t y = firstRow; y < last; ++y) {
        for (std::uint32_t x = 0; x < frame.width; x += 7) {  // stride: 6000 columns is plenty
            const float* p = frame.pixel(x, y);
            sum += (static_cast<double>(p[0]) + p[1] + p[2]) / 3.0;
            ++n;
        }
    }
    return n ? sum / static_cast<double>(n) : 0.0;
}

/// Largest per-channel difference between two decoded frames of equal size.
[[nodiscard]] float maxChannelDiff(const DecodedFrame& a, const DecodedFrame& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        worst = std::max(worst, std::fabs(a.rgba[i] - b.rgba[i]));
    }
    return worst;
}

}  // namespace

// =============================================================================
//  Loading and registration
// =============================================================================

TEST_CASE("the built .prm loads, exports xImportEntry and answers imInit", "[importer][init]") {
    ImporterHarness harness;
    INFO(harness.loadError());
    REQUIRE(harness.loaded());

    SECTION("imInit returns imIsCacheable") {
        // imIsCacheable is only honest because imInit probes no hardware; the
        // GPU is found lazily on the first frame.
        REQUIRE(harness.initResult() == imIsCacheable);
    }

    SECTION("the capability flags are the ones the design fixes") {
        const imImportInfoRec& info = harness.importInfo();
        // The modal dialog AND the master clip effect, deliberately both:
        // the dialog is muscle memory for existing users and is the fallback
        // on a machine where OpenOSVSourceSettings.aex failed to install.
        REQUIRE(info.hasSetup == kPrTrue);            // the Source Settings dialog
        REQUIRE(info.canProvidePeakAudio == kPrFalse);
        REQUIRE(info.avoidAudioConform == kPrFalse);  // AAC: let the host conform
        REQUIRE(info.priority == 0);
        REQUIRE(info.keepLoaded == kPrFalse);
        REQUIRE(info.addToMenu == imMenuNone);
        // This flag is what makes Premiere look at
        // imFileInfoRec8::sourceSettingsMatchName at all; without it the
        // field is ignored and OpenOSVSourceSettings.aex is never attached to
        // a master clip, so the Effect Controls panel stays empty.
        REQUIRE(info.hasSourceSettingsEffect == kPrTrue);
        REQUIRE(info.canSave == kPrFalse);
        REQUIRE(info.canTrim == kPrFalse);
    }
}

TEST_CASE("imGetIndFormat describes exactly one format", "[importer][format]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    imIndFormatRec rec{};
    REQUIRE(harness.sendIndexed(imGetIndFormat, 0, &rec) == imNoErr);

    SECTION("the file type is 'OSV_' and matches the IMPT resource") {
        REQUIRE(rec.filetype == 'OSV_');
        REQUIRE(rec.filetype == 0x4F53565F);
    }

    SECTION("names and extensions") {
        REQUIRE(std::string(rec.FormatName) == "DJI Osmo 360 (OpenOSV)");
        REQUIRE(std::string(rec.FormatShortName) == "OSV");
        // The extensions are NUL separated and the list is NUL terminated.
        REQUIRE(std::string(rec.PlatformExtension) == "osv");
        REQUIRE(std::string(rec.PlatformExtension + 4) == "lrf");
        REQUIRE(rec.PlatformExtension[8] == '\0');
    }

    SECTION("xfIsMovie is set so the type appears in Attach Proxies") {
        REQUIRE((rec.flags & xfIsMovie) != 0);
        REQUIRE((rec.flags & xfCanOpen) != 0);
        REQUIRE((rec.flags & xfCanImport) != 0);
    }

    SECTION("index 1 ends the enumeration") {
        imIndFormatRec second{};
        REQUIRE(harness.sendIndexed(imGetIndFormat, 1, &second) == imBadFormatIndex);
    }
}

TEST_CASE("the imGetSupports selectors answer with their mal codes", "[importer][format]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    REQUIRE(harness.send(imGetSupports7, nullptr, nullptr) == malSupports7);
    REQUIRE(harness.send(imGetSupports8, nullptr, nullptr) == malSupports8);
    REQUIRE(harness.send(imGetSupportsPerInstancePrefs, nullptr, nullptr) == malSupportsPerInstancePrefs);
}

TEST_CASE("an unknown selector returns imUnsupported without crashing", "[importer][dispatch]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    // A selector that does not exist at all, one that exists but is not
    // implemented, and the 23.3 selector whose record is not public.
    REQUIRE(harness.send(9999, nullptr, nullptr) == imUnsupported);
    REQUIRE(harness.send(imGetMetaData, nullptr, nullptr) == imUnsupported);
    REQUIRE(harness.send(imGetColorSpaceFromOpaqueData, nullptr, nullptr) == imUnsupported);
    REQUIRE(harness.send(imCreateAsyncImporter, nullptr, nullptr) == imUnsupported);

    // imUnsupported is a NON-error return; the host keeps going.
    REQUIRE(PrImporterReturnValueIsError(static_cast<PrImporterReturnValue>(imUnsupported)) == false);
}

TEST_CASE("every selector tolerates null records", "[importer][dispatch][defensive]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    // A host that hands a null record is a host bug, but the importer must
    // return an error rather than dereference it.  Each of these would be a
    // crash without the null checks in the handlers.
    const csSDK_int32 selectors[] = {imInit,
                                     imGetIndFormat,
                                     imGetInfo8,
                                     imGetInfo9,
                                     imGetIndPixelFormat,
                                     imGetPreferredFrameSize,
                                     imSelectClipFrameDescriptor,
                                     imGetIndColorSpace,
                                     imGetSourceVideo,
                                     imImportAudio7,
                                     imGetSequentialAudio,
                                     imResetSequentialAudio,
                                     imGetAudioChannelLayout,
                                     imGetPrefs8,
                                     imGetInstancePrefs,
                                     imAnalysis,
                                     imGetTimeInfo8,
                                     imGetFileAttributes,
                                     imOpenFile8,
                                     imQuietFile,
                                     imCloseFile};
    for (const csSDK_int32 selector : selectors) {
        INFO("selector " << selector);
        const csSDK_int32 result = harness.send(selector, nullptr, nullptr);
        // Any integer is acceptable as long as we got one back at all.
        REQUIRE(result != 0x7FFFFFFF);
    }
}

// =============================================================================
//  Opening
// =============================================================================

TEST_CASE("a non-OSV file is rejected with the handle closed", "[importer][open]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    // Write a file that is not an ISO container at all.
    const std::filesystem::path junk =
        std::filesystem::path(OSV_TEST_OUTPUT_DIR) / "not-an-osv.osv";
    {
        std::error_code ec;
        std::filesystem::create_directories(junk.parent_path(), ec);
        FILE* f = nullptr;
        REQUIRE(::_wfopen_s(&f, junk.wstring().c_str(), L"wb") == 0);
        REQUIRE(f != nullptr);
        const char payload[] = "this is not an ISO base media file, not even close";
        std::fwrite(payload, 1, sizeof(payload), f);
        std::fclose(f);
    }

    auto clip = harness.openClip(junk);
    REQUIRE(clip.openResult() == imBadFile);
    REQUIRE_FALSE(clip.open());

    // The importer must have closed its handle, or a lower-priority importer
    // could not open the file.  Proving it: a delete succeeds only when no
    // handle without FILE_SHARE_DELETE is open.
    std::error_code ec;
    REQUIRE(std::filesystem::remove(junk, ec));
    REQUIRE_FALSE(ec);
}

TEST_CASE("a missing file fails without leaking privateData", "[importer][open]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const std::size_t before = harness.host().liveMemoryBlocks();

    auto clip = harness.openClip(std::filesystem::path(OSV_TEST_OUTPUT_DIR) / "does-not-exist-at-all.osv");
    REQUIRE_FALSE(clip.open());
    REQUIRE(clip.openResult() != imNoErr);

    // Nothing was allocated for a clip that never opened.
    REQUIRE(harness.host().liveMemoryBlocks() == before);
}

TEST_CASE("the sample clip opens and closes without leaking", "[importer][open][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    const std::size_t before = harness.host().liveMemoryBlocks();
    {
        auto clip = harness.openClip(sampleClipPath());
        INFO("open result " << clip.openResult());
        REQUIRE(clip.open());
        REQUIRE(clip.fileRef() != imInvalidHandleValue);
        // Exactly one handle: the privateData block.
        REQUIRE(harness.host().liveMemoryBlocks() == before + 1);
        REQUIRE(clip.close() == imNoErr);
    }
    // imCloseFile disposed the handle.
    REQUIRE(harness.host().liveMemoryBlocks() == before);
}

TEST_CASE("imQuietFile releases the handle and a later frame request still works",
          "[importer][open][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);

    const auto* ppix = [&] {
        const void* p = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &p) == kSPNoError);
        return static_cast<const PrSDKPPixSuite*>(p);
    }();

    // One frame, then quiet, then another frame.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);  // small = fast
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.width = 1920;
    request.height = 960;

    PPixHand frame = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, frame) == imNoErr);
    REQUIRE(frame != nullptr);
    ppix->Dispose(frame);

    REQUIRE(clip.quiet() == imNoErr);
    // The privateData survives a quiet; only the OS handle and the decoders go.
    REQUIRE(clip.privateData() != nullptr);

    // A frame request after a quiet re-opens the decoders transparently.
    request.frameTime = kTicksPerFrame5994 * 3;
    frame = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, frame) == imNoErr);
    REQUIRE(frame != nullptr);
    ppix->Dispose(frame);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// =============================================================================
//  imGetInfo8
// =============================================================================

TEST_CASE("imGetInfo8 describes the sample clip exactly", "[importer][info][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    // Native explicitly: this case asserts the NATIVE stitched geometry, so
    // it must not follow whatever the default output size happens to be
    // (the default is a smaller editing-friendly size, see PrefsBlob).
    PrefsBlob nativePrefs = PrefsBlob::defaults();
    nativePrefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &nativePrefs) == imNoErr);

    SECTION("video geometry is the native stitched equirect") {
        REQUIRE(info.hasVideo == 1);
        REQUIRE(info.vidInfo.imageWidth == kSampleWidth);
        REQUIRE(info.vidInfo.imageHeight == kSampleHeight);
        // An equirect frame is always 2:1.
        REQUIRE(info.vidInfo.imageWidth == 2 * info.vidInfo.imageHeight);
        REQUIRE(info.vidInfo.pixelAspectNum == 1);
        REQUIRE(info.vidInfo.pixelAspectDen == 1);
        REQUIRE(info.vidInfo.fieldType == prFieldsNone);
        // Straight coverage alpha, not opaque: a dual-fisheye stitch leaves
        // the directions hidden by the camera body (the calibration's
        // occlusion polygon) fully transparent, and declaring alphaOpaque
        // would let the host ignore the alpha channel and composite those
        // pixels as black.
        REQUIRE(info.vidInfo.alphaType == alphaStraight);
        REQUIRE(info.vidInfo.depth == 32);
        REQUIRE(info.vidInfo.bitDepth == 10);
        REQUIRE(info.vidInfo.subType == 'hvc1');
        REQUIRE(info.vidInfo.isStill == 0);
    }

    SECTION("the frame period is the exact 60000/1001 tick count") {
        // This is the assertion that catches a frame rate computed from a
        // rounded double: 254016000000 / 59.94 is 4237837837.8, not
        // 4237833600, and the error accumulates over a timeline.
        REQUIRE(info.vidInfo.frameRate == kTicksPerFrame5994);
        REQUIRE(kTicksPerSecond * 1001 / 60000 == kTicksPerFrame5994);
        // vidScale / vidSampleSize carry the same rational for old hosts.
        REQUIRE(info.vidScale == 60000);
        REQUIRE(info.vidSampleSize == 1001);
    }

    SECTION("duration is reported in frames") {
        REQUIRE(info.vidDurationInFrames == kSampleFrames);
    }

    SECTION("the VR fields declare a monoscopic 360 x 180 equirect") {
        REQUIRE(info.ivProjectionType == kPrIVProjectionType_Equirectangular);
        REQUIRE(info.ivFrameLayout == kPrIVFrameLayout_Monoscopic);
        REQUIRE(info.ivHorizontalCapturedView == 360);
        REQUIRE(info.ivVerticalCapturedView == 180);
    }

    SECTION("audio is declared with the right shape") {
        REQUIRE(info.hasAudio == 1);
        REQUIRE(info.audInfo.numChannels == kSampleAudioChannels);
        REQUIRE(info.audInfo.sampleRate == kSampleSampleRate);
        REQUIRE(info.audInfo.sampleType == kPrAudioSampleType_Compressed);
        REQUIRE(info.audDuration > 0);
        // The audio is about as long as the video (65 frames at 59.94 fps is
        // 1.085 s); allow generous slack for AAC framing.
        REQUIRE(info.audDuration > static_cast<PrAudioSample>(0.5 * kSampleSampleRate));
        REQUIRE(info.audDuration < static_cast<PrAudioSample>(3.0 * kSampleSampleRate));
    }

    SECTION("the access mode enables both random and sequential audio") {
        REQUIRE(info.accessModes == kSeparateSequentialAudio);
    }

    SECTION("frames come through imGetSourceVideo, synchronously") {
        REQUIRE(info.vidInfo.supportsGetSourceVideo == kPrTrue);
        REQUIRE(info.vidInfo.supportsAsyncIO == kPrFalse);
    }

    SECTION("the colour space is declared as fixed and the file path is filled") {
        REQUIRE(info.vidInfo.colorSpaceSupport == imColorSpaceSupport_Fixed);
        REQUIRE(info.filePath[0] != 0);
        const std::wstring path(reinterpret_cast<const wchar_t*>(info.filePath));
        REQUIRE(path.find(L".OSV") != std::wstring::npos);
    }

    SECTION("stream index 1 ends the enumeration") {
        imFileInfoRec8 second{};
        second.privatedata = clip.privateData();
        second.streamIdx = 1;
        imFileAccessRec8 access{};
        REQUIRE(harness.send(imGetInfo8, &access, &second) == imBadStreamIndex);
    }
}

TEST_CASE("the prefs change the advertised output size", "[importer][info][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    struct Case {
        PrefsOutputSize size;
        csSDK_int32 width;
        csSDK_int32 height;
    };
    const Case cases[] = {{PrefsOutputSize::Native, kSampleWidth, kSampleHeight},
                          {PrefsOutputSize::UHD4K, 3840, 1920},
                          // The default: a new sequence from an .OSV opens at
                          // 2560 x 1280 rather than inheriting the full sphere.
                          {PrefsOutputSize::QHD2560, 2560, 1280},
                          {PrefsOutputSize::HD2K, 1920, 960}};
    for (const Case& c : cases) {
        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.outputSize = static_cast<std::uint8_t>(c.size);
        imFileInfoRec8 info{};
        INFO("output size " << static_cast<int>(c.size));
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
        REQUIRE(info.vidInfo.imageWidth == c.width);
        REQUIRE(info.vidInfo.imageHeight == c.height);
    }
}

TEST_CASE("imGetInfo9 mirrors imGetInfo8 and claims no system-state dependency",
          "[importer][info][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info8{};
    REQUIRE(harness.getInfo8(clip, info8) == imNoErr);

    imFileInfoRec9 info9{};
    info9.info.privatedata = clip.privateData();
    info9.info.streamIdx = 0;
    info9.info.vidInfo.importerID = clip.importerId();
    imFileAccessRec8 access{};
    REQUIRE(harness.send(imGetInfo9, &access, &info9) == imNoErr);

    REQUIRE(info9.info.vidInfo.imageWidth == info8.vidInfo.imageWidth);
    REQUIRE(info9.info.vidInfo.imageHeight == info8.vidInfo.imageHeight);
    REQUIRE(info9.info.vidInfo.frameRate == info8.vidInfo.frameRate);
    REQUIRE(info9.info.vidDurationInFrames == info8.vidDurationInFrames);
    REQUIRE(info9.info.ivProjectionType == kPrIVProjectionType_Equirectangular);

    // Nothing we advertise changes with the hardware, so the host never has to
    // invalidate its cached answers.
    REQUIRE(info9.systemStateFlagMask == 0);
    REQUIRE(info9.systemStateFlagsCurrent == 0);
    REQUIRE(info9.systemStateSubtypeVersion == 0);
}

// =============================================================================
//  Pixel formats, sizes and descriptors
// =============================================================================

TEST_CASE("imGetIndPixelFormat lists 32f then 8u", "[importer][format]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    imIndPixelFormatRec rec{};
    REQUIRE(harness.sendIndexed(imGetIndPixelFormat, 0, &rec) == imNoErr);
    REQUIRE(rec.outPixelFormat == PrPixelFormat_BGRA_4444_32f);

    std::memset(&rec, 0, sizeof(rec));
    REQUIRE(harness.sendIndexed(imGetIndPixelFormat, 1, &rec) == imNoErr);
    REQUIRE(rec.outPixelFormat == PrPixelFormat_BGRA_4444_8u);

    std::memset(&rec, 0, sizeof(rec));
    REQUIRE(harness.sendIndexed(imGetIndPixelFormat, 2, &rec) == imBadFormatIndex);
    REQUIRE(harness.sendIndexed(imGetIndPixelFormat, -1, &rec) == imBadFormatIndex);
}

TEST_CASE("imGetPreferredFrameSize enumerates native, half and quarter",
          "[importer][format][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    // Pin Native: this case asserts the NATIVE enumeration, so it must not
    // silently follow whatever the default output size happens to be.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);

    struct Expected {
        csSDK_int32 index;
        csSDK_int32 width;
        csSDK_int32 height;
        csSDK_int32 result;
    };
    const Expected expected[] = {{0, 6000, 3000, imIterateFrameSizes},
                                 {1, 3000, 1500, imIterateFrameSizes},
                                 {2, 1500, 750, imNoErr}};
    for (const Expected& e : expected) {
        imPreferredFrameSizeRec rec{};
        rec.inPrivateData = clip.privateData();
        rec.inPrefs = &prefs;
        rec.inPixelFormat = PrPixelFormat_BGRA_4444_32f;
        rec.inIndex = e.index;
        INFO("index " << e.index);
        REQUIRE(harness.send(imGetPreferredFrameSize, &rec, nullptr) == e.result);
        REQUIRE(rec.outWidth == e.width);
        REQUIRE(rec.outHeight == e.height);
        // Every advertised size keeps the 2:1 equirect ratio.
        REQUIRE(rec.outWidth == 2 * rec.outHeight);
    }

    // Past the last size the enumeration ends.
    imPreferredFrameSizeRec past{};
    past.inPrivateData = clip.privateData();
    past.inPrefs = &prefs;
    past.inIndex = 3;
    REQUIRE(harness.send(imGetPreferredFrameSize, &past, nullptr) == imOtherErr);
}

TEST_CASE("imSelectClipFrameDescriptor coerces the format and snaps the size",
          "[importer][format][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    // Native: these sections assert the NATIVE size ladder (6000 / 3000 /
    // 1500), which is what the descriptor snaps to.  The default output size
    // is a smaller editing-friendly one, so it is pinned here rather than
    // letting the expectations silently follow it.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);

    SECTION("a supported format is kept") {
        imClipFrameDescriptorRec rec{};
        rec.inPrivateData = clip.privateData();
        rec.inPrefs = &prefs;
        rec.inDesiredClipFrameDescriptor.inPixelFormat = PrPixelFormat_BGRA_4444_8u;
        rec.inDesiredClipFrameDescriptor.inWidth = 6000;
        rec.inDesiredClipFrameDescriptor.inHeight = 3000;
        REQUIRE(harness.send(imSelectClipFrameDescriptor, nullptr, &rec) == imNoErr);
        REQUIRE(rec.outBestFrameDescriptor.inPixelFormat == PrPixelFormat_BGRA_4444_8u);
        REQUIRE(rec.outBestFrameDescriptor.inWidth == 6000);
        REQUIRE(rec.outBestFrameDescriptor.inHeight == 3000);
        REQUIRE(rec.outBestFrameDescriptor.inFieldType == prFieldsNone);
        REQUIRE(rec.outBestFrameDescriptor.inPixelAspectRatioNumerator == 1);
    }

    SECTION("an unsupported format becomes 32f") {
        imClipFrameDescriptorRec rec{};
        rec.inPrivateData = clip.privateData();
        rec.inPrefs = &prefs;
        rec.inDesiredClipFrameDescriptor.inPixelFormat = PrPixelFormat_VUYA_4444_32f;
        rec.inDesiredClipFrameDescriptor.inWidth = 6000;
        rec.inDesiredClipFrameDescriptor.inHeight = 3000;
        REQUIRE(harness.send(imSelectClipFrameDescriptor, nullptr, &rec) == imNoErr);
        REQUIRE(rec.outBestFrameDescriptor.inPixelFormat == PrPixelFormat_BGRA_4444_32f);
    }

    SECTION("an odd size snaps to the nearest advertised one") {
        imClipFrameDescriptorRec rec{};
        rec.inPrivateData = clip.privateData();
        rec.inPrefs = &prefs;
        rec.inDesiredClipFrameDescriptor.inPixelFormat = PrPixelFormat_BGRA_4444_32f;
        rec.inDesiredClipFrameDescriptor.inWidth = 3100;
        rec.inDesiredClipFrameDescriptor.inHeight = 1550;  // nearest is 3000 x 1500
        REQUIRE(harness.send(imSelectClipFrameDescriptor, nullptr, &rec) == imNoErr);
        REQUIRE(rec.outBestFrameDescriptor.inWidth == 3000);
        REQUIRE(rec.outBestFrameDescriptor.inHeight == 1500);
    }

    SECTION("version 2 with Maximum Bit Depth off chooses 8u") {
        imClipFrameDescriptorRec2 rec{};
        rec.inPrivateData = clip.privateData();
        rec.inPrefs = &prefs;
        rec.inDesiredClipFrameDescriptor.inPixelFormat = PrPixelFormat_BGRA_4444_32f;
        rec.inDesiredClipFrameDescriptor.inWidth = 6000;
        rec.inDesiredClipFrameDescriptor.inHeight = 3000;
        rec.inDesiredMaxBitDepth = kMaxBitDepth_Off;
        REQUIRE(harness.send(imSelectClipFrameDescriptor2, nullptr, &rec) == imNoErr);
        REQUIRE(rec.outBestFrameDescriptor.inPixelFormat == PrPixelFormat_BGRA_4444_8u);
    }

    SECTION("version 2 is refused on a host that predates 23.2") {
        // The gate exists so the importer never reads imClipFrameDescriptorRec2's
        // tail on a host that only allocated the v1 record.
        const csSDK_int32 saved = harness.stdParms().imInterfaceVer;
        harness.stdParms().imInterfaceVer = IMPORTMOD_VERSION_23;
        imClipFrameDescriptorRec2 rec{};
        rec.inPrivateData = clip.privateData();
        rec.inPrefs = &prefs;
        REQUIRE(harness.send(imSelectClipFrameDescriptor2, nullptr, &rec) == imUnsupported);
        harness.stdParms().imInterfaceVer = saved;
    }
}

// =============================================================================
//  Colour space
// =============================================================================

TEST_CASE("imGetIndColorSpace declares the token the prefs ask for",
          "[importer][color][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    struct Case {
        PrefsColorOutput output;
        const char* token;
    };
    const Case cases[] = {{PrefsColorOutput::PQ, kPrOverranged2100PQ},
                          {PrefsColorOutput::HLG, kPrOverranged2100HLG},
                          {PrefsColorOutput::Rec709, kPrOverranged709},
                          // The SDK has no DJI D-Log M token, so passthrough
                          // is declared as the closest honest space: full
                          // range, RGB, 32f, SCENE-referred (a log signal is
                          // scene light) with BT.2020 primaries as the widest
                          // standard gamut.  See colorSpaceTokenFor().
                          {PrefsColorOutput::DLogM, kPrOverranged2020Scene}};
    // Every value of the enum is covered, so a newly appended output cannot
    // be added without either extending this table or failing here.
    static_assert(std::size(cases) == static_cast<std::size_t>(PrefsColorOutput::Count),
                  "the colour-space table does not cover every PrefsColorOutput value");

    for (const Case& c : cases) {
        // The colour space follows the prefs, so the instance has to see them
        // first; imGetInfo8 is the call that carries them.
        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.colorOutput = static_cast<std::uint8_t>(c.output);
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

        imIndColorSpaceRec rec{};
        rec.inPrivateData = clip.privateData();
        INFO("colour output " << static_cast<int>(c.output) << " expecting " << c.token);
        REQUIRE(harness.sendIndexed(imGetIndColorSpace, 0, &rec) == imNoErr);
        REQUIRE(rec.outColorSpaceType == kPrSDKColorSpaceType_Predefined);
        REQUIRE(harness.host().utf8(rec.ioProfileRec.outName) == std::string(c.token));
    }

    SECTION("index 1 ends the enumeration") {
        imIndColorSpaceRec rec{};
        rec.inPrivateData = clip.privateData();
        REQUIRE(harness.sendIndexed(imGetIndColorSpace, 1, &rec) == imBadFormatIndex);
    }
}

// =============================================================================
//  Frames
// =============================================================================

TEST_CASE("imGetSourceVideo renders frame 0 at native size", "[importer][video][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    // Pin Native: this case measures and inspects the FULL-resolution frame.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    // Seam search and gain matching are correctness features, not needed to
    // prove the frame path, and each costs a full band render per frame.
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;
    // Stabilisation off so the sky really is at the top of the frame; see the
    // orientation section below.
    prefs.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.format = PrPixelFormat_BGRA_4444_32f;
    request.width = kSampleWidth;
    request.height = kSampleHeight;

    PPixHand hand = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(hand != nullptr);
    WARN("native 6000 x 3000 frame 0: " << ms << " ms");

    const DecodedFrame frame = readPPix(harness.host(), ppix, hand);
    REQUIRE(frame.width == static_cast<std::uint32_t>(kSampleWidth));
    REQUIRE(frame.height == static_cast<std::uint32_t>(kSampleHeight));

    SECTION("the frame is not black and every value is a sane PQ code") {
        double sum = 0.0;
        std::uint64_t n = 0;
        std::uint64_t nonBlack = 0;
        std::uint64_t alphaSamples = 0;
        std::uint64_t alphaOpaqueSamples = 0;
        for (std::uint32_t y = 0; y < frame.height; y += 13) {
            for (std::uint32_t x = 0; x < frame.width; x += 13) {
                const float* p = frame.pixel(x, y);
                for (int c = 0; c < 3; ++c) {
                    REQUIRE(std::isfinite(p[c]));
                    // PQ / HLG / 709 output codes are all in [0, 1].
                    REQUIRE(p[c] >= 0.0f);
                    REQUIRE(p[c] <= 1.0f);
                    sum += p[c];
                    ++n;
                    if (p[c] > 0.01f) {
                        ++nonBlack;
                    }
                }
                // Alpha is straight lens coverage, which imGetInfo8 declares
                // as alphaStraight.  It is 1 over the vast majority of the
                // sphere and drops only where the calibration's occlusion
                // polygon says neither lens sees the direction (the camera
                // body), so the contract to check is "a valid coverage
                // value", not "always 1".
                REQUIRE(p[3] >= 0.0f);
                REQUIRE(p[3] <= 1.0f);
                ++alphaSamples;
                if (p[3] > 0.999f) {
                    ++alphaOpaqueSamples;
                }
            }
        }
        REQUIRE(n > 0);
        // Most of a real outdoor frame is well above black.
        REQUIRE(static_cast<double>(nonBlack) / static_cast<double>(n) > 0.5);
        REQUIRE(sum / static_cast<double>(n) > 0.02);

        // Almost the whole sphere is covered by one lens or the other; only
        // the occlusion polygons are transparent.  A frame that came out
        // mostly transparent would mean the rig or the blend collapsed.
        REQUIRE(alphaSamples > 0);
        const double opaqueFraction = static_cast<double>(alphaOpaqueSamples) / static_cast<double>(alphaSamples);
        INFO("fully opaque fraction " << opaqueFraction);
        REQUIRE(opaqueFraction > 0.9);
    }

    SECTION("the picture is the right way up") {
        // Ground truth, measured on this clip with
        //   osvtool render --mode equirect --size 1920x960 --frame 0         //                  --color pq --stab off
        // the top eighth of the frame averages ~124/255 and the bottom
        // eighth ~97/255: the sky is at the TOP.  readPPix already undid the
        // host's bottom-left origin, so a failure here means the importer
        // wrote its rows the wrong way round.
        //
        // NOTE the prefs above set stabilisation OFF.  With the default
        // horizon lock the same osvtool command reports top ~66 and bottom
        // ~78, because this clip's camera attitude rotates the sky away from
        // the top row - a correct render that simply cannot be checked this
        // way.  Orientation is tested with stabilisation off for exactly
        // that reason.
        const double top = meanBrightness(frame, 0, frame.height / 8);
        const double bottom = meanBrightness(frame, frame.height - frame.height / 8, frame.height / 8);
        INFO("top band " << top << ", bottom band " << bottom);
        REQUIRE(top > bottom);

        // Ordering alone is a weak proxy: it holds for any transform that
        // preserves vertical brightness order.  Pin the measured MAGNITUDES
        // too, against the osvtool numbers above (124/255 = 0.486 and
        // 97/255 = 0.380 in the 0..1 values readPPix produces).  A generous
        // band absorbs the render-size difference between the 1920x960
        // osvtool reference and this frame while still failing a render that
        // is, say, half as bright or twice as contrasty.
        //
        // The bottom band gets more headroom than the top on purpose.  It is
        // the band that contains the selfie-stick occlusion arc, and it used
        // to be measurably DARKER than the scene because a bug in the
        // occlusion polygon punched a black region into it (see
        // buildOcclusion in src/osv/geom/LensRig.cpp).  With that fixed the
        // hole is filled from the other lens, which legitimately raises this
        // band - measured 0.4762 against the 0.4755 that +-25% allowed, i.e.
        // the old ceiling was pinning the BUG.  +-30 % keeps a real
        // regression detectable without re-encoding that hole as the
        // expectation.
        constexpr double kOsvtoolTop = 124.0 / 255.0;
        constexpr double kOsvtoolBottom = 97.0 / 255.0;
        CHECK(top > kOsvtoolTop * 0.75);
        CHECK(top < kOsvtoolTop * 1.25);
        CHECK(bottom > kOsvtoolBottom * 0.75);
        CHECK(bottom < kOsvtoolBottom * 1.30);
    }

    SECTION("longitude is not mirrored and the seam is where it belongs") {
        // The vertical brightness proxy above cannot see a HORIZONTAL flip or
        // a 180-degree pan offset: mirroring an equirect frame left-to-right
        // leaves every row's mean untouched.  These two checks do see them.
        //
        // 1. Column asymmetry.  A real outdoor scene is not left-right
        //    symmetric, so the mean of the left half and the mean of the
        //    right half differ.  Record BOTH, and assert the difference is
        //    real rather than that either one is a particular number: that
        //    makes the next check meaningful without inventing ground truth
        //    this suite cannot measure.
        auto columnBandMean = [&frame](std::uint32_t firstCol, std::uint32_t colCount) {
            double sum = 0.0;
            std::uint64_t n = 0;
            const std::uint32_t last = std::min(frame.width, firstCol + colCount);
            for (std::uint32_t y = 0; y < frame.height; y += 7) {
                for (std::uint32_t x = firstCol; x < last; x += 7) {
                    const float* p = frame.pixel(x, y);
                    sum += (static_cast<double>(p[0]) + p[1] + p[2]) / 3.0;
                    ++n;
                }
            }
            return n ? sum / static_cast<double>(n) : 0.0;
        };
        const double left = columnBandMean(0, frame.width / 2);
        const double right = columnBandMean(frame.width / 2, frame.width / 2);
        INFO("left half " << left << ", right half " << right);
        // The two halves are genuinely different pictures, so a mirror or a
        // 180-degree rotation is detectable at all.
        REQUIRE(std::abs(left - right) > 1e-3);

        // 2. The +-180 seam wraps continuously.  Column 0 and column w-1 are
        //    adjacent on the sphere (they differ by one pixel of longitude),
        //    so their difference must be far smaller than the difference
        //    between column 0 and a column a quarter of the way round.  A
        //    misplaced seam, a half-frame pan offset or a mirrored longitude
        //    all break that relationship, while a correct render satisfies it
        //    whatever the scene happens to contain.
        auto columnMean = [&frame](std::uint32_t x) {
            double sum = 0.0;
            std::uint64_t n = 0;
            for (std::uint32_t y = 0; y < frame.height; y += 3) {
                const float* p = frame.pixel(x, y);
                sum += (static_cast<double>(p[0]) + p[1] + p[2]) / 3.0;
                ++n;
            }
            return n ? sum / static_cast<double>(n) : 0.0;
        };
        const double first = columnMean(0);
        const double last = columnMean(frame.width - 1);
        const double quarter = columnMean(frame.width / 4);
        const double seamJump = std::abs(first - last);
        const double quarterJump = std::abs(first - quarter);
        INFO("seam |col0 - colW-1| = " << seamJump << ", |col0 - colW/4| = " << quarterJump);
        REQUIRE(quarterJump > 1e-4);
        CHECK(seamJump < quarterJump);
    }

    ppix->Dispose(hand);
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("a half-size request renders directly at that size", "[importer][video][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    // Native: the request below asks for 3000 x 1500, which is the HALF step
    // of the native ladder.  imGetSourceVideo snaps a request to an advertised
    // size, and the ladder is derived from the chosen output size, so pinning
    // Native is what makes "half of native" mean 3000 x 1500 here.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;
    // Off, so the sky-at-the-top assertion below is meaningful (the default
    // horizon lock rotates this clip's sky away from the top row).
    prefs.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.width = 3000;
    request.height = 1500;

    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
    REQUIRE(hand != nullptr);

    const DecodedFrame frame = readPPix(harness.host(), ppix, hand);
    REQUIRE(frame.width == 3000);
    REQUIRE(frame.height == 1500);

    // It is a real render, not a downscale of a black frame.
    const double top = meanBrightness(frame, 0, frame.height / 8);
    const double bottom = meanBrightness(frame, frame.height - frame.height / 8, frame.height / 8);
    REQUIRE(top > bottom);

    ppix->Dispose(hand);
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("8u and 32f frames agree within one 8-bit code", "[importer][video][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.width = 1920;
    request.height = 960;

    request.format = PrPixelFormat_BGRA_4444_32f;
    PPixHand hand32 = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand32) == imNoErr);
    const DecodedFrame frame32 = readPPix(harness.host(), ppix, hand32);

    request.format = PrPixelFormat_BGRA_4444_8u;
    PPixHand hand8 = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand8) == imNoErr);
    const DecodedFrame frame8 = readPPix(harness.host(), ppix, hand8);

    // The 8u path quantises the same float render, so the difference is
    // exactly the rounding error of one 8-bit code.
    const float worst = maxChannelDiff(frame32, frame8);
    INFO("largest difference " << worst << " (1/255 = " << 1.0f / 255.0f << ")");
    REQUIRE(worst <= 1.0f / 255.0f + 1e-6f);

    ppix->Dispose(hand32);
    ppix->Dispose(hand8);
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("PQ, HLG, Rec.709 and D-Log M passthrough produce different pixels",
          "[importer][video][color][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    auto render = [&](PrefsColorOutput output) {
        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.colorOutput = static_cast<std::uint8_t>(output);
        prefs.seamSearch = 0;
        prefs.gainMatch = 0;

        ImporterHarness::SourceVideoRequest request;
        request.frameTime = 0;
        request.width = 1920;
        request.height = 960;

        PPixHand hand = nullptr;
        REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
        REQUIRE(hand != nullptr);
        const DecodedFrame frame = readPPix(harness.host(), ppix, hand);
        ppix->Dispose(hand);
        return frame;
    };

    const DecodedFrame pq = render(PrefsColorOutput::PQ);
    const DecodedFrame hlg = render(PrefsColorOutput::HLG);
    const DecodedFrame rec709 = render(PrefsColorOutput::Rec709);
    const DecodedFrame dlogm = render(PrefsColorOutput::DLogM);

    // Four different treatments of the same linear light: every pair must
    // differ substantially, not by a rounding error.  The passthrough pairs
    // are the new ones, and they are the assertion that catches the failure
    // mode that matters - a toOutputTransfer() switch that fell through to
    // its PQ default would make `dlogm` byte-identical to `pq` while every
    // other test still passed.
    REQUIRE(maxChannelDiff(pq, hlg) > 0.02f);
    REQUIRE(maxChannelDiff(pq, rec709) > 0.02f);
    REQUIRE(maxChannelDiff(hlg, rec709) > 0.02f);
    REQUIRE(maxChannelDiff(pq, dlogm) > 0.02f);
    REQUIRE(maxChannelDiff(hlg, dlogm) > 0.02f);
    REQUIRE(maxChannelDiff(rec709, dlogm) > 0.02f);

    // And the passthrough frame is a real picture, not a black or blown-out
    // one: "nothing was applied" must not have become "nothing came out".
    // Passthrough emits the camera's own log code values, so every sample is
    // finite and in [0, 1] and the frame has genuine variation in it.
    {
        float lo = 1.0f;
        float hi = 0.0f;
        double sum = 0.0;
        std::size_t counted = 0;
        for (std::size_t i = 0; i < dlogm.rgba.size(); i += 4) {
            for (int c = 0; c < 3; ++c) {  // RGB only; alpha is lens coverage
                const float v = dlogm.rgba[i + static_cast<std::size_t>(c)];
                REQUIRE(std::isfinite(v));
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += static_cast<double>(v);
                ++counted;
            }
        }
        REQUIRE(counted > 0);
        const double mean = sum / static_cast<double>(counted);
        INFO("passthrough range [" << lo << ", " << hi << "], mean " << mean);
        REQUIRE(lo >= 0.0f);
        REQUIRE(hi <= 1.0f);
        // Not black, not clipped white, and with real contrast in between.
        REQUIRE(hi > 0.1f);
        REQUIRE(mean > 0.01f);
        REQUIRE(hi - lo > 0.05f);
    }

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("the host cache serves the second request for the same frame",
          "[importer][video][cache][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;

    // The importer id has to reach the instance, which happens in
    // imOpenFile8 / imGetInfo8; without it the cache is deliberately bypassed.
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

    harness.host().clearCache();
    const auto before = harness.host().cacheStats();

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = kTicksPerFrame5994 * 2;
    request.width = 1920;
    request.height = 960;

    PPixHand first = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, first) == imNoErr);
    REQUIRE(first != nullptr);
    const auto afterFirst = harness.host().cacheStats();
    REQUIRE(afterFirst.misses > before.misses);
    REQUIRE(afterFirst.entries > before.entries);

    PPixHand second = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, second) == imNoErr);
    REQUIRE(second != nullptr);
    const auto afterSecond = harness.host().cacheStats();
    REQUIRE(afterSecond.hits > afterFirst.hits);
    // A cache hit hands back the very frame that was stored.
    REQUIRE(second == first);

    SECTION("a changed prefs byte misses") {
        PrefsBlob other = prefs;
        other.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
        const auto beforeThird = harness.host().cacheStats();
        PPixHand third = nullptr;
        REQUIRE(harness.getSourceVideo(clip, request, other, third) == imNoErr);
        REQUIRE(third != nullptr);
        REQUIRE(third != first);
        REQUIRE(harness.host().cacheStats().misses > beforeThird.misses);
        ppix->Dispose(third);
    }

    ppix->Dispose(first);
    ppix->Dispose(second);
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("a frame time maps to the frame index by rounding and clamping",
          "[importer][video][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);
    harness.host().clearCache();

    auto renderAt = [&](PrTime time) {
        ImporterHarness::SourceVideoRequest request;
        request.frameTime = time;
        request.width = 1920;
        request.height = 960;
        PPixHand hand = nullptr;
        REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
        REQUIRE(hand != nullptr);
        const DecodedFrame frame = readPPix(harness.host(), ppix, hand);
        ppix->Dispose(hand);
        return frame;
    };

    const DecodedFrame frame5 = renderAt(kTicksPerFrame5994 * 5);

    SECTION("a time a few ticks short of a frame boundary still gives that frame") {
        // A host that multiplies a frame number by a rounded tick count lands
        // just below the boundary; truncation would serve frame 4.
        const DecodedFrame nearly = renderAt(kTicksPerFrame5994 * 5 - 3);
        REQUIRE(maxChannelDiff(frame5, nearly) == 0.0f);
    }

    SECTION("a time past the end clamps to the last frame") {
        const DecodedFrame last = renderAt(kTicksPerFrame5994 * (kSampleFrames - 1));
        const DecodedFrame beyond = renderAt(kTicksPerFrame5994 * 10000);
        REQUIRE(maxChannelDiff(last, beyond) == 0.0f);
    }

    SECTION("a negative time clamps to frame 0") {
        const DecodedFrame zero = renderAt(0);
        const DecodedFrame negative = renderAt(-kTicksPerFrame5994 * 4);
        REQUIRE(maxChannelDiff(zero, negative) == 0.0f);
    }

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("a Rec.709 connection-space request overrides the declared space",
          "[importer][video][color][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::PQ);
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.width = 1920;
    request.height = 960;

    // Normal PQ render.
    PPixHand pqHand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, pqHand) == imNoErr);
    const DecodedFrame pq = readPPix(harness.host(), ppix, pqHand);
    ppix->Dispose(pqHand);

    // The same request, but the host says it fell back to the connection
    // space.  The importer must then emit Rec.709 regardless of the prefs.
    harness.host().clearCache();
    request.selectedColorProfileName = kPrOverranged709;
    PPixHand fallbackHand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, fallbackHand) == imNoErr);
    const DecodedFrame fallback = readPPix(harness.host(), ppix, fallbackHand);
    ppix->Dispose(fallbackHand);

    // Compare against an explicit Rec.709 render: the fallback must match it,
    // not the PQ one.
    harness.host().clearCache();
    PrefsBlob rec709Prefs = prefs;
    rec709Prefs.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    ImporterHarness::SourceVideoRequest plain = request;
    plain.selectedColorProfileName.clear();
    PPixHand rec709Hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, plain, rec709Prefs, rec709Hand) == imNoErr);
    const DecodedFrame rec709 = readPPix(harness.host(), ppix, rec709Hand);
    ppix->Dispose(rec709Hand);

    REQUIRE(maxChannelDiff(fallback, rec709) == 0.0f);
    REQUIRE(maxChannelDiff(fallback, pq) > 0.02f);

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("the colour-managed PPix carries the host's colour space id",
          "[importer][video][color][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.width = 1920;
    request.height = 960;
    request.colorSpace = harness.host().colorSpaceId(kPrOverranged2100PQ);
    REQUIRE((request.colorSpace.opaque[0] != 0 || request.colorSpace.opaque[1] != 0));

    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
    REQUIRE(hand != nullptr);

    const auto info = harness.host().inspect(hand);
    REQUIRE(info.has_value());
    // The frame was created with CreateColorManagedPPix and is tagged.
    REQUIRE(info->colorSpace.opaque[0] == request.colorSpace.opaque[0]);
    REQUIRE(info->colorSpace.opaque[1] == request.colorSpace.opaque[1]);
    REQUIRE(harness.host().predefinedName(info->colorSpace) == std::string(kPrOverranged2100PQ));

    ppix->Dispose(hand);
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// =============================================================================
//  Audio
// =============================================================================

TEST_CASE("imImportAudio7 returns real samples", "[importer][audio][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    REQUIRE(info.hasAudio == 1);
    const std::int32_t channels = info.audInfo.numChannels;
    REQUIRE(channels == kSampleAudioChannels);

    constexpr std::uint32_t kFrames = 4096;
    std::vector<std::vector<float>> buffers;
    REQUIRE(harness.importAudio(clip, 0, kFrames, channels, buffers) == imNoErr);
    REQUIRE(buffers.size() == static_cast<std::size_t>(channels));

    SECTION("every channel was written and holds finite audio") {
        for (const auto& channel : buffers) {
            REQUIRE(channel.size() == kFrames);
            for (const float s : channel) {
                // -12345 is the poison the harness pre-fills; seeing it means
                // the importer did not write that sample at all.
                REQUIRE(s != -12345.0f);
                REQUIRE(std::isfinite(s));
                REQUIRE(std::fabs(s) <= 4.0f);  // float PCM is nominally +/-1
            }
        }
    }

    SECTION("the audio is not silence") {
        double energy = 0.0;
        for (const auto& channel : buffers) {
            for (const float s : channel) {
                energy += static_cast<double>(s) * s;
            }
        }
        INFO("total energy " << energy);
        REQUIRE(energy > 1e-6);
    }
}

TEST_CASE("random and sequential audio agree over the same range",
          "[importer][audio][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    const std::int32_t channels = info.audInfo.numChannels;

    constexpr std::uint32_t kBlock = 2048;
    constexpr int kBlocks = 4;

    // Sequential: reset, then walk forward.
    REQUIRE(harness.resetSequentialAudio(clip) == imNoErr);
    std::vector<std::vector<std::vector<float>>> sequential;
    for (int i = 0; i < kBlocks; ++i) {
        std::vector<std::vector<float>> block;
        REQUIRE(harness.sequentialAudio(clip, kBlock, channels, block) == imNoErr);
        sequential.push_back(std::move(block));
    }

    // Random: ask for the same ranges out of order, so a seek really happens.
    for (int i = kBlocks - 1; i >= 0; --i) {
        std::vector<std::vector<float>> block;
        REQUIRE(harness.importAudio(clip, static_cast<std::int64_t>(i) * kBlock, kBlock, channels, block) == imNoErr);
        for (std::int32_t ch = 0; ch < channels; ++ch) {
            INFO("block " << i << " channel " << ch);
            REQUIRE(block[static_cast<std::size_t>(ch)].size() == kBlock);
            const auto& got = block[static_cast<std::size_t>(ch)];
            const auto& want = sequential[static_cast<std::size_t>(i)][static_cast<std::size_t>(ch)];
            std::size_t firstDiff = kBlock;
            for (std::uint32_t s = 0; s < kBlock; ++s) {
                if (got[s] != want[s]) {
                    firstDiff = s;
                    break;
                }
            }
            if (firstDiff != kBlock) {
                // Report where and by how much, plus the lag that would align
                // the two, so a failure names the cause instead of one number.
                int lag = 9999;
                for (int k = 1; k < 4096 && lag == 9999; ++k) {
                    if (static_cast<std::size_t>(k) + 8 < kBlock && got[0] == want[k]) {
                        bool ok = true;
                        for (int j = 0; j < 8; ++j) {
                            if (got[static_cast<std::size_t>(j)] != want[static_cast<std::size_t>(k + j)]) {
                                ok = false;
                                break;
                            }
                        }
                        if (ok) {
                            lag = k;
                        }
                    }
                }
                INFO("first difference at sample " << firstDiff << ": random " << got[firstDiff] << " vs sequential "
                                                   << want[firstDiff] << "; random[0] aligns with sequential["
                                                   << lag << "]");
                REQUIRE(got[firstDiff] == want[firstDiff]);
            }
        }
    }
}

TEST_CASE("audio past the end of stream is zero filled", "[importer][audio][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    const std::int32_t channels = info.audInfo.numChannels;
    const std::int64_t duration = info.audDuration;
    REQUIRE(duration > 0);

    constexpr std::uint32_t kFrames = 1024;
    std::vector<std::vector<float>> buffers;
    // Well past the end: the host does ask for this while conforming a tail.
    REQUIRE(harness.importAudio(clip, duration + 10 * kFrames, kFrames, channels, buffers) == imNoErr);
    for (const auto& channel : buffers) {
        REQUIRE(channel.size() == kFrames);
        for (const float s : channel) {
            REQUIRE(s == 0.0f);
        }
    }
}

TEST_CASE("imGetAudioChannelLayout is only answered for unusual channel counts",
          "[importer][audio][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    REQUIRE(info.audInfo.numChannels == 2);

    // 1, 2 and 6 channels are fully described by numChannels, so the selector
    // is deliberately unsupported for them.
    imGetAudioChannelLayoutRec rec{};
    rec.inPrivateData = clip.privateData();
    REQUIRE(harness.send(imGetAudioChannelLayout, nullptr, &rec) == imUnsupported);
}

// =============================================================================
//  Prefs
// =============================================================================

TEST_CASE("imGetPrefs8 follows the two-step protocol", "[importer][prefs]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    // Step 1: no buffer, only a size.
    imGetPrefsRec rec{};
    rec.prefs = nullptr;
    rec.prefsLength = 0;
    rec.firstTime = 1;
    imFileAccessRec8 access{};
    REQUIRE(harness.send(imGetPrefs8, &access, &rec) == imNoErr);
    REQUIRE(rec.prefsLength == static_cast<csSDK_int32>(PrefsBlob::kSize));

    // Step 2: with a buffer.  The dialog is suppressed by the environment
    // variable the test main sets, so this returns the defaults immediately
    // instead of blocking on a modal window.
    std::vector<char> buffer(static_cast<std::size_t>(rec.prefsLength), 0x5A);
    rec.prefs = buffer.data();
    REQUIRE(harness.send(imGetPrefs8, &access, &rec) == imNoErr);
    REQUIRE(rec.prefsLength == static_cast<csSDK_int32>(PrefsBlob::kSize));

    const PrefsBlob written = PrefsBlob::fromBytes(buffer.data(), buffer.size());
    REQUIRE(written.isValid());
    // Garbage in the buffer becomes the documented defaults.
    REQUIRE(written == PrefsBlob::defaults());

    SECTION("a buffer smaller than the blob is refused, not overrun") {
        imGetPrefsRec small{};
        std::vector<char> tiny(16, 0);
        small.prefs = tiny.data();
        small.prefsLength = 16;
        REQUIRE(harness.send(imGetPrefs8, &access, &small) == imNoErr);
        REQUIRE(small.prefsLength == static_cast<csSDK_int32>(PrefsBlob::kSize));
        // Nothing was written into the too-small buffer.
        for (const char c : tiny) {
            REQUIRE(c == 0);
        }
    }

    SECTION("an existing valid blob survives a round trip") {
        PrefsBlob custom = PrefsBlob::defaults();
        custom.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
        custom.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::UHD4K);
        custom.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Full);
        custom.seamSearch = 0;
        custom.exposureStops = 1.5f;
        custom.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Cpu);

        std::vector<char> storage(PrefsBlob::kSize, 0);
        std::memcpy(storage.data(), &custom, PrefsBlob::kSize);

        imGetPrefsRec round{};
        round.prefs = storage.data();
        round.prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);
        REQUIRE(harness.send(imGetPrefs8, &access, &round) == imNoErr);

        const PrefsBlob after = PrefsBlob::fromBytes(storage.data(), storage.size());
        REQUIRE(after == custom);
    }
}

TEST_CASE("imGetInstancePrefs reaches the live instance", "[importer][prefs][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    PrefsBlob custom = PrefsBlob::defaults();
    custom.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);

    std::vector<char> storage(PrefsBlob::kSize, 0);
    std::memcpy(storage.data(), &custom, PrefsBlob::kSize);

    imGetInstancePrefsRec rec{};
    rec.privateData = clip.privateData();
    rec.prefsRec.prefs = storage.data();
    rec.prefsRec.prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);
    imFileAccessRec8 access{};
    REQUIRE(harness.send(imGetInstancePrefs, &access, &rec) == imNoErr);

    // The instance adopted the blob, so imGetInfo8 with NO prefs now reports
    // the 2K size the instance is holding.
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    REQUIRE(info.vidInfo.imageWidth == 1920);
    REQUIRE(info.vidInfo.imageHeight == 960);
}

// =============================================================================
//  The Source Settings effect binding
// =============================================================================

TEST_CASE("imGetInfo8 advertises the Source Settings effect's match name",
          "[importer][sourcesettings][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);

    // This string is the ENTIRE binding between the importer and
    // OpenOSVSourceSettings.aex: Premiere compares it to the effect's PiPL
    // match name, with no handshake and no diagnostic on a mismatch.  One
    // mistyped character and the Effect Controls panel never shows the stitch
    // options, with nothing in any log to say why - which is exactly why both
    // sides read plugins/common/SourceSettingsIdentity.h and why this test
    // and the effect's own PiPL test compare against the same constant.
    //
    // prUTF16Char is a 16-bit code unit, which on Windows is wchar_t.
    const std::wstring advertised(reinterpret_cast<const wchar_t*>(info.sourceSettingsMatchName));
    REQUIRE(advertised == std::wstring(kSourceSettingsMatchNameW));
    REQUIRE_FALSE(advertised.empty());
    // It must fit the field WITH its terminator, or the host reads past it.
    REQUIRE(advertised.size() < 256u);
}

TEST_CASE("imPerformSourceSettingsCommand exchanges a prefs blob",
          "[importer][sourcesettings][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileAccessRec8 access{};

    SECTION("a live instance reports the blob it is actually decoding with") {
        // Put a known, non-default blob into the instance the way the host
        // would, then ask the selector what the clip is doing.  Answering
        // with the instance's own settings is what makes the effect's panel
        // show "as shot" after a project reopen instead of snapping every
        // control back to the global default.
        PrefsBlob adopted = PrefsBlob::defaults();
        adopted.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::UHD4K);
        adopted.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
        adopted.seamSearch = 0;
        REQUIRE(adopted.sanitise());

        std::vector<char> prefsStorage(PrefsBlob::kSize, 0);
        std::memcpy(prefsStorage.data(), &adopted, PrefsBlob::kSize);
        imGetInstancePrefsRec instanceRec{};
        instanceRec.privateData = clip.privateData();
        instanceRec.prefsRec.prefs = prefsStorage.data();
        instanceRec.prefsRec.prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);
        REQUIRE(harness.send(imGetInstancePrefs, &access, &instanceRec) == imNoErr);

        // The effect sends its own (different) idea of the settings.
        PrefsBlob fromEffect = PrefsBlob::defaults();
        fromEffect.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);
        std::vector<char> buffer(PrefsBlob::kSize, 0);
        std::memcpy(buffer.data(), &fromEffect, PrefsBlob::kSize);

        imSourceSettingsCommandRec rec{};
        rec.ioData = buffer.data();
        rec.inDataSize = static_cast<csSDK_int32>(PrefsBlob::kSize);
        rec.inPrivateData = clip.privateData();
        REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imNoErr);

        // The instance's blob wins, because it is the only party that knows
        // what the media is really being decoded with.
        const PrefsBlob returned = PrefsBlob::fromBytes(buffer.data(), buffer.size());
        REQUIRE(returned == adopted);
    }

    SECTION("with no instance the effect's own settings are echoed back") {
        // Normal during project load and before imOpenFile8.  Overwriting
        // with defaults here would reset every control of every clip on every
        // project open, so the effect's values have to survive.
        PrefsBlob fromEffect = PrefsBlob::defaults();
        fromEffect.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Smooth);
        fromEffect.exposureStops = -1.25f;
        REQUIRE(fromEffect.sanitise());

        std::vector<char> buffer(PrefsBlob::kSize, 0);
        std::memcpy(buffer.data(), &fromEffect, PrefsBlob::kSize);

        imSourceSettingsCommandRec rec{};
        rec.ioData = buffer.data();
        rec.inDataSize = static_cast<csSDK_int32>(PrefsBlob::kSize);
        rec.inPrivateData = nullptr;
        REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imNoErr);

        REQUIRE(PrefsBlob::fromBytes(buffer.data(), buffer.size()) == fromEffect);
    }

    SECTION("a payload that is not one of our blobs becomes the defaults") {
        // A stale payload from an older build must not be trusted: its bytes
        // would otherwise be reinterpreted as stitch settings.
        std::vector<char> buffer(PrefsBlob::kSize, '\x5A');

        imSourceSettingsCommandRec rec{};
        rec.ioData = buffer.data();
        rec.inDataSize = static_cast<csSDK_int32>(PrefsBlob::kSize);
        rec.inPrivateData = nullptr;
        REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imNoErr);

        const PrefsBlob returned = PrefsBlob::fromBytes(buffer.data(), buffer.size());
        REQUIRE(returned.isValid());
        REQUIRE(returned == PrefsBlob::defaults());
    }
}

TEST_CASE("imPerformSourceSettingsCommand refuses a bad record", "[importer][sourcesettings]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    imFileAccessRec8 access{};

    SECTION("a null record") {
        REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, nullptr) == imOtherErr);
    }

    SECTION("a null buffer") {
        imSourceSettingsCommandRec rec{};
        rec.ioData = nullptr;
        rec.inDataSize = static_cast<csSDK_int32>(PrefsBlob::kSize);
        REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imOtherErr);
    }

    SECTION("a buffer smaller than a blob is refused and NOT written") {
        // Writing 128 bytes into a smaller buffer is a heap overflow in the
        // host's own allocator - both a crash and a security problem - so the
        // write has to be refused outright rather than truncated.
        std::vector<char> buffer(PrefsBlob::kSize, '\xAB');
        imSourceSettingsCommandRec rec{};
        rec.ioData = buffer.data();
        rec.inDataSize = static_cast<csSDK_int32>(PrefsBlob::kSize) - 1;
        REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imOtherErr);
        for (const char c : buffer) {
            REQUIRE(c == '\xAB');
        }
    }

    SECTION("a negative size is refused") {
        std::vector<char> buffer(PrefsBlob::kSize, '\xAB');
        imSourceSettingsCommandRec rec{};
        rec.ioData = buffer.data();
        rec.inDataSize = -1;
        REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imOtherErr);
    }
}

TEST_CASE("imPerformSourceSettingsCommand leaves a larger buffer's tail alone",
          "[importer][sourcesettings]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    imFileAccessRec8 access{};

    // Only the first kSize bytes are ours.  The tail belongs to the host and
    // is not ours to define, so it must come back untouched.
    constexpr std::size_t kTail = 32;
    std::vector<char> buffer(PrefsBlob::kSize + kTail, '\xAB');
    const PrefsBlob fromEffect = PrefsBlob::defaults();
    std::memcpy(buffer.data(), &fromEffect, PrefsBlob::kSize);

    imSourceSettingsCommandRec rec{};
    rec.ioData = buffer.data();
    rec.inDataSize = static_cast<csSDK_int32>(buffer.size());
    rec.inPrivateData = nullptr;
    REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imNoErr);

    REQUIRE(PrefsBlob::fromBytes(buffer.data(), PrefsBlob::kSize) == fromEffect);
    for (std::size_t i = PrefsBlob::kSize; i < buffer.size(); ++i) {
        REQUIRE(buffer[i] == '\xAB');
    }
}

// =============================================================================
//  Informational selectors
// =============================================================================

TEST_CASE("imAnalysis produces a readable summary", "[importer][info][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    // The summary quotes the clip's NATIVE stitched geometry, so pin Native
    // rather than inheriting the default (editing-friendly) output size.
    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);

    // Two-step: size first.
    imAnalysisRec rec{};
    rec.privatedata = clip.privateData();
    rec.prefs = &prefs;
    rec.buffer = nullptr;
    rec.buffersize = 0;
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);
    REQUIRE(rec.buffersize > 32);

    std::vector<char> buffer(static_cast<std::size_t>(rec.buffersize), '\xCC');
    rec.buffer = buffer.data();
    REQUIRE(harness.send(imAnalysis, nullptr, &rec) == imNoErr);

    const std::string text(buffer.data());
    INFO(text);
    REQUIRE(text.find("OpenOSV") != std::string::npos);
    REQUIRE(text.find("equirectangular") != std::string::npos);
    REQUIRE(text.find("6000 x 3000") != std::string::npos);
    REQUIRE(text.find("59.940") != std::string::npos);
    // CR/LF line endings, as the host's edit control wants.
    REQUIRE(text.find("\r\n") != std::string::npos);
    // The string is NUL terminated inside the buffer.
    REQUIRE(text.size() < buffer.size());
}

TEST_CASE("imGetTimeInfo8 reports no timecode rather than inventing one",
          "[importer][info][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imTimeInfoRec8 rec{};
    rec.privatedata = clip.privateData();
    // DJI writes no tmcd track; imNoTimecode is a non-error return that says
    // exactly that, and is better than a made-up start time in Media Start.
    REQUIRE(harness.send(imGetTimeInfo8, nullptr, &rec) == imNoTimecode);
}

// =============================================================================
//  The LRF proxy
// =============================================================================

TEST_CASE("the .LRF proxy opens and describes itself", "[importer][open][lrf][sample]") {
    const std::filesystem::path proxy = sampleProxyPath();
    std::error_code ec;
    if (proxy.empty() || !std::filesystem::exists(proxy, ec)) {
        SKIP("the .LRF proxy is not present at " << proxy.string());
    }

    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(proxy);
    INFO("open result " << clip.openResult());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);

    // The proxy is a single 2048 x 1024 side-by-side track that FormatDetector
    // reports as Mode::Lrf, so the clip is recognised and described the same
    // way a native .OSV is.
    REQUIRE(info.hasVideo == 1);
    REQUIRE(info.vidInfo.imageWidth == 2 * info.vidInfo.imageHeight);
    REQUIRE(info.ivProjectionType == kPrIVProjectionType_Equirectangular);
    REQUIRE(info.ivFrameLayout == kPrIVFrameLayout_Monoscopic);
    REQUIRE(info.vidDurationInFrames > 0);
    REQUIRE(info.vidInfo.frameRate > 0);

    // The .LRF proxy RENDERS.  It is a single 2048 x 1024 side-by-side track
    // holding two 1024 x 1024 fisheye halves; video::DualStreamReader splits
    // it, and FormatInfo::lensW() / lensH() give the rig the per-lens size so
    // the two agree.
    //
    // This assertion used to be the exact opposite - the importer reported
    // imFrameNotFound and the test asserted that failure - because every rig
    // builder fed StreamScaling the WHOLE TRACK (2048 x 1024) while the
    // reader handed out halves (1024 x 1024), and RenderParamsBuilder
    // rejected the mismatch on every frame:
    //
    //   imGetSourceVideo: frame 0 failed: RenderParamsBuilder: frame size
    //   does not match the rig (1024x1024 vs 2048x1024)
    //
    // The library-level cover for the split lives in tests/unit/test_render.cpp
    // ("the LRF proxy builds a rig that matches its decoded halves"); what is
    // checked here is that the importer delivers the frame to the host.
    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    PrefsBlob prefs = PrefsBlob::defaults();
    prefs.seamSearch = 0;
    prefs.gainMatch = 0;
    prefs.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.width = info.vidInfo.imageWidth;
    request.height = info.vidInfo.imageHeight;

    PPixHand hand = nullptr;
    const csSDK_int32 result = harness.getSourceVideo(clip, request, prefs, hand);
    INFO("imGetSourceVideo returned " << result);
    REQUIRE(result == imNoErr);
    REQUIRE(hand != nullptr);

    const DecodedFrame frame = readPPix(harness.host(), ppix, hand);
    REQUIRE(frame.width == static_cast<std::uint32_t>(info.vidInfo.imageWidth));
    REQUIRE(frame.height == static_cast<std::uint32_t>(info.vidInfo.imageHeight));

    // Finite, in range, and mostly opaque: with a correct rig the two
    // 195-degree lenses cover the sphere, so nearly every direction is filled.
    // A rig built from the whole track would have projected each lens into a
    // small disc and left most of the panorama transparent.
    std::size_t opaque = 0;
    std::size_t total = 0;
    bool varies = false;
    const float* first = frame.pixel(0, 0);
    for (std::uint32_t y = 0; y < frame.height; y += 4u) {
        for (std::uint32_t x = 0; x < frame.width; x += 4u) {
            const float* p = frame.pixel(x, y);
            for (int c = 0; c < 4; ++c) {
                REQUIRE(std::isfinite(p[c]));
                REQUIRE(p[c] >= -0.001f);
            }
            if (p[3] > 0.5f) {
                ++opaque;
            }
            if (!varies && (p[0] != first[0] || p[1] != first[1] || p[2] != first[2])) {
                varies = true;
            }
            ++total;
        }
    }
    REQUIRE(total > 0u);
    const double coverage = static_cast<double>(opaque) / static_cast<double>(total);
    INFO("LRF alpha coverage " << coverage);
    CHECK(coverage > 0.90);
    // A uniform frame would satisfy every check above and still be wrong.
    CHECK(varies);

    if (ppix->Dispose) {
        ppix->Dispose(hand);
    }
    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

// =============================================================================
//  Timing
// =============================================================================

TEST_CASE("frame timing on each render backend", "[importer][video][timing][sample][!benchmark]") {
    REQUIRE_SAMPLE_CLIP();

    // Reported, not asserted: how long a stitched frame takes is a property
    // of the machine, and a threshold here would fail on a slower one for no
    // good reason.  The numbers still belong in the test output, because a
    // ten-times regression is the kind of thing that otherwise ships.
    struct Backend {
        PrefsRenderDevice device;
        const char* name;
    };
    const Backend backends[] = {{PrefsRenderDevice::Cpu, "CPU"}, {PrefsRenderDevice::Cuda, "CUDA"}};

    for (const Backend& backend : backends) {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());

        const void* suite = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) ==
                kSPNoError);
        const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.renderDevice = static_cast<std::uint8_t>(backend.device);
        prefs.seamSearch = 0;
        prefs.gainMatch = 0;
        prefs.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);

        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

        ImporterHarness::SourceVideoRequest request;
        request.width = kSampleWidth;
        request.height = kSampleHeight;

        // Frame 0 warms the decoder, the renderer and any device context;
        // only the frames after it are timed.
        request.frameTime = 0;
        PPixHand warmup = nullptr;
        const csSDK_int32 first = harness.getSourceVideo(clip, request, prefs, warmup);
        if (first != imNoErr) {
            // An explicit backend that this machine does not have is a
            // legitimate outcome, not a failure.
            WARN(backend.name << ": unavailable on this machine (imGetSourceVideo returned " << first << ")");
            harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
            continue;
        }
        REQUIRE(warmup != nullptr);
        ppix->Dispose(warmup);

        constexpr int kFrames = 4;
        harness.host().clearCache();
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 1; i <= kFrames; ++i) {
            request.frameTime = kTicksPerFrame5994 * i;
            PPixHand hand = nullptr;
            REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
            REQUIRE(hand != nullptr);
            ppix->Dispose(hand);
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kFrames;
        WARN(backend.name << ": " << ms << " ms per 6000 x 3000 frame (decode + stitch + copy)");

        harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
    }
}

// ---------------------------------------------------------------------------
//  Parallax correction (PrefsBlob::parallax / flowBackend)
// ---------------------------------------------------------------------------

namespace {

/// Body-frame latitude, in degrees from the seam plane, of the ray through
/// Standard-layout equirect pixel (x, y) with stabilisation OFF (so body ==
/// view).  The seam plane is the one perpendicular to the lens axes (+/-Y),
/// and in the Standard layout the Y component of a direction is
/// cos(lon) * cos(lat) - exactly the kernel's osvRayForPixel.
[[nodiscard]] double seamLatitudeDeg(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    constexpr double kPi = 3.14159265358979323846;
    const double lon = (static_cast<double>(x) + 0.5) / w * 2.0 * kPi - kPi;
    const double lat = 0.5 * kPi - (static_cast<double>(y) + 0.5) / h * kPi;
    const double dy = std::cos(lon) * std::cos(lat);
    return std::asin(std::clamp(dy, -1.0, 1.0)) * 180.0 / kPi;
}

/// Render one frame and read it back; disposes the PPix.
[[nodiscard]] DecodedFrame renderFrame(ImporterHarness& harness, ImporterHarness::ClipHandle& clip,
                                       const PrSDKPPixSuite* ppix, const ImporterHarness::SourceVideoRequest& request,
                                       const PrefsBlob& prefs) {
    PPixHand hand = nullptr;
    REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
    REQUIRE(hand != nullptr);
    DecodedFrame frame = readPPix(harness.host(), ppix, hand);
    ppix->Dispose(hand);
    return frame;
}

}  // namespace

TEST_CASE("parallax correction changes only the overlap band, and only when asked",
          "[importer][video][parallax][sample]") {
    REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    const void* suite = nullptr;
    REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) == kSPNoError);
    const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

    // Everything else that touches the seam is OFF, so any difference between
    // the two renders is the parallax correction and nothing else.  Classical
    // flow explicitly: the result must not depend on whether a neural model
    // happens to be installed on the test machine.
    PrefsBlob off = PrefsBlob::defaults();
    off.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
    off.seamSearch = 0;
    off.gainMatch = 0;
    off.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
    off.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    off.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Classical);
    PrefsBlob on = off;
    on.parallax = static_cast<std::uint8_t>(PrefsParallax::On);

    ImporterHarness::SourceVideoRequest request;
    request.frameTime = 0;
    request.width = 3000;  // the half step of the native ladder
    request.height = 1500;

    const DecodedFrame frameOff = renderFrame(harness, clip, ppix, request, off);
    const DecodedFrame frameOn = renderFrame(harness, clip, ppix, request, on);
    REQUIRE(frameOff.width == frameOn.width);
    REQUIRE(frameOff.height == frameOn.height);

    // The grid spans +/-9.1 degrees around the seam plane (a 6 degree band
    // plus the decay ring) and the kernel returns exactly zero correction
    // beyond it, so everything outside must be BIT-identical - a tolerance
    // here would hide a correction leaking out of the overlap.  Inside, the
    // correction must actually do something.
    std::uint64_t changedInside = 0;
    std::uint64_t changedOutside = 0;
    for (std::uint32_t y = 0; y < frameOn.height; ++y) {
        for (std::uint32_t x = 0; x < frameOn.width; ++x) {
            const float* a = frameOff.pixel(x, y);
            const float* b = frameOn.pixel(x, y);
            const bool changed = a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3];
            if (!changed) {
                continue;
            }
            if (std::fabs(seamLatitudeDeg(x, y, frameOn.width, frameOn.height)) > 9.5) {
                ++changedOutside;
            } else {
                ++changedInside;
            }
        }
    }
    INFO("changed pixels: " << changedInside << " inside the overlap, " << changedOutside << " outside");
    REQUIRE(changedOutside == 0);
    REQUIRE(changedInside > 10000);

    SECTION("a draft request never pays for it") {
        // Low quality is a draft request (isDraftRequest), the same rule that
        // already skips the seam search during struggling playback.
        harness.host().clearCache();
        ImporterHarness::SourceVideoRequest draft = request;
        draft.quality = kPrRenderQuality_Low;
        const DecodedFrame frameDraft = renderFrame(harness, clip, ppix, draft, on);
        REQUIRE(maxChannelDiff(frameDraft, frameOff) == 0.0f);
    }

    SECTION("a project saved before the option existed renders with it off") {
        // The two fields were carved out of the zero-filled reserved block,
        // so an old project's blob carries zeros there - which must read as
        // Off, not as the new default On, or opening an old project would
        // silently change its pictures.
        PrefsBlob modern = on;
        std::uint8_t bytes[PrefsBlob::kSize];
        std::memcpy(bytes, &modern, PrefsBlob::kSize);
        bytes[offsetof(PrefsBlob, parallax)] = 0;
        bytes[offsetof(PrefsBlob, flowBackend)] = 0;
        const PrefsBlob old = PrefsBlob::fromBytes(bytes, sizeof(bytes));
        REQUIRE(!old.parallaxEnabled());

        harness.host().clearCache();
        const DecodedFrame frameOld = renderFrame(harness, clip, ppix, request, old);
        REQUIRE(maxChannelDiff(frameOld, frameOff) == 0.0f);
    }

    SECTION("when the grid is available it replaces the seam table") {
        // Measured: table + grid is worse than the grid alone (see
        // ImporterInstance::renderFrame), so with both enabled the importer
        // uses the grid only and the seam search setting makes no difference.
        PrefsBlob onWithSeam = on;
        onWithSeam.seamSearch = 1;
        harness.host().clearCache();
        const DecodedFrame frameBoth = renderFrame(harness, clip, ppix, request, onWithSeam);
        REQUIRE(maxChannelDiff(frameBoth, frameOn) == 0.0f);
    }

    harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
}

TEST_CASE("parallax correction cost in the importer",
          "[importer][video][timing][parallax][sample][!benchmark]") {
    REQUIRE_SAMPLE_CLIP();

    // Reported, not asserted - the same policy as the frame timing test
    // above: a threshold would encode one machine's speed.  Three settings,
    // the ones a user actually meets:
    //   * the previous default: seam search + gain, no parallax;
    //   * the new default: seam search + gain + parallax (the grid replaces
    //     the seam table whenever it is accepted);
    //   * the new default revisiting frames it has already measured, which
    //     is what paused scrubbing mostly does.
    struct Setting {
        const char* name;
        bool parallax;
    };
    const Setting settings[] = {{"seam table, parallax off", false}, {"parallax on", true}};

    for (const Setting& setting : settings) {
        ImporterHarness harness;
        REQUIRE(harness.loaded());
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());
        const void* suite = nullptr;
        REQUIRE(harness.host().basicSuite()->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &suite) ==
                kSPNoError);
        const auto* ppix = static_cast<const PrSDKPPixSuite*>(suite);

        PrefsBlob prefs = PrefsBlob::defaults();
        prefs.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::Native);
        prefs.parallax = static_cast<std::uint8_t>(setting.parallax ? PrefsParallax::On : PrefsParallax::Off);
        prefs.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Classical);

        ImporterHarness::SourceVideoRequest request;
        request.width = kSampleWidth;
        request.height = kSampleHeight;

        // Frame 0 warms the decoder and the renderer; only later frames count.
        request.frameTime = 0;
        (void)renderFrame(harness, clip, ppix, request, prefs);

        constexpr int kFrames = 4;
        const auto timeFrames = [&]() {
            harness.host().clearCache();
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 1; i <= kFrames; ++i) {
                request.frameTime = kTicksPerFrame5994 * i;
                PPixHand hand = nullptr;
                REQUIRE(harness.getSourceVideo(clip, request, prefs, hand) == imNoErr);
                REQUIRE(hand != nullptr);
                ppix->Dispose(hand);
            }
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kFrames;
        };
        const double first = timeFrames();
        // Same frames again with the HOST cache cleared: the instance's own
        // analysis caches (seam tables, gains, parallax grids) are still warm.
        const double revisit = timeFrames();
        WARN(setting.name << ": " << first << " ms per 6000 x 3000 frame first time, " << revisit
                          << " ms revisiting (decode + analysis + stitch + copy)");

        harness.host().basicSuite()->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
    }
}
