// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The video half of OpenOSVImporter.prm: what the clip is (imGetInfo8 /
// imGetInfo9), what formats and sizes it can be delivered in
// (imGetIndPixelFormat, imGetPreferredFrameSize, imSelectClipFrameDescriptor),
// what colour space it is in (imGetIndColorSpace) and the frames themselves
// (imGetSourceVideo).  Every answer here follows docs/PREMIERE.md,
// "Importer design".

#include "ImporterPlugin.h"

#include "ImporterInstance.h"

#include "HostContext.h"
#include "PixelCopy.h"
#include "PluginLog.h"

#include "PrSDKImmersiveVideoTypes.h"

#include "osv/meta/Types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace osv::premiere {

namespace {

/// The two pixel formats the importer produces, in preference order
/// (decision D3: 32f first because the render is float and the clip is HDR;
/// 8u second because the host may want a cheap draft).
constexpr std::array<PrPixelFormat, 2> kPixelFormats = {PrPixelFormat_BGRA_4444_32f, PrPixelFormat_BGRA_4444_8u};

/// Bytes per pixel of a format we support (0 for anything else).
[[nodiscard]] std::size_t bytesPerPixelFor(PrPixelFormat format) noexcept {
    switch (format) {
    case PrPixelFormat_BGRA_4444_32f: return pixelcopy::kBytesPerPixel32f;
    case PrPixelFormat_BGRA_4444_8u:  return pixelcopy::kBytesPerPixel8u;
    default:                          return 0;
    }
}

/// True when the importer can deliver `format`.
[[nodiscard]] bool isSupportedFormat(PrPixelFormat format) noexcept { return bytesPerPixelFor(format) != 0; }

/// Copy a UTF-8 string into a prUTF16Char array (the host's path / stream
/// name fields).  Always NUL terminated, never overruns.
void copyUtf16(prUTF16Char* dst, std::size_t capacity, const std::wstring& src) noexcept {
    if (!dst || capacity == 0) {
        return;
    }
    std::memset(dst, 0, capacity * sizeof(prUTF16Char));
    const std::size_t n = std::min(src.size(), capacity - 1u);
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] = static_cast<prUTF16Char>(src[i]);
    }
}

/// Ticks per second the host's Time Suite reports, with Premiere's documented
/// value as the fallback when the suite is unavailable.
[[nodiscard]] PrTime ticksPerSecond() noexcept {
    ImporterGlobals& g = globals();
    if (g.suites.time && g.suites.time->GetTicksPerSecond) {
        PrTime ticks = 0;
        if (g.suites.time->GetTicksPerSecond(&ticks) == suiteError_NoError && ticks > 0) {
            return ticks;
        }
    }
    return 254016000000LL;
}

/// Ticks per video frame from the clip's exact container rational.
/// ticksPerSecond * denominator / numerator is exact for 60000/1001 because
/// 254016000000 is divisible by 60000.
[[nodiscard]] PrTime ticksPerFrameFor(const ImporterInstance& instance) noexcept {
    const std::uint32_t num = instance.rateNumerator();
    const std::uint32_t den = instance.rateDenominator();
    if (num == 0 || den == 0) {
        return 0;
    }
    const PrTime tps = ticksPerSecond();
    // Do the multiply first so the division is exact whenever the rate
    // divides the tick base (which it does for every broadcast rate).
    return (tps * static_cast<PrTime>(den)) / static_cast<PrTime>(num);
}

/// Frame index for a host time, rounded to nearest and clamped into the clip
/// (docs: "Frame index = inFrameTime / ticksPerFrame with rounding,
/// clamped").
[[nodiscard]] std::uint32_t frameIndexFor(const ImporterInstance& instance, PrTime frameTime) noexcept {
    const PrTime perFrame = ticksPerFrameFor(instance);
    if (perFrame <= 0) {
        return 0;
    }
    if (frameTime <= 0) {
        return 0;
    }
    // Round to nearest: a host that computes a frame time by multiplying a
    // frame number by a rounded tick count lands a tick or two short, and
    // truncation would then serve the previous frame.
    const PrTime index = (frameTime + perFrame / 2) / perFrame;
    const std::uint32_t total = instance.frameCount();
    if (total == 0) {
        return 0;
    }
    if (index < 0) {
        return 0;
    }
    if (static_cast<std::uint64_t>(index) >= total) {
        return total - 1u;
    }
    return static_cast<std::uint32_t>(index);
}

/// Pick the first requested format the importer supports, and the first
/// requested size.  A null / empty format array means "anything", which per
/// the design doc is 32f at native size.
struct FormatChoice {
    PrPixelFormat format = PrPixelFormat_BGRA_4444_32f;
    std::int32_t width = 0;   ///< 0 = caller has no preference.
    std::int32_t height = 0;
};

[[nodiscard]] FormatChoice chooseFormat(const imSourceVideoRec& rec) noexcept {
    FormatChoice choice;
    if (!rec.inFrameFormats || rec.inNumFrameFormats <= 0) {
        return choice;
    }
    for (csSDK_int32 i = 0; i < rec.inNumFrameFormats; ++i) {
        const imFrameFormat& f = rec.inFrameFormats[i];
        // inPixelFormat 0 (PrPixelFormat_Any) means any format is fine.
        const bool formatOk = f.inPixelFormat == PrPixelFormat_Any || isSupportedFormat(f.inPixelFormat);
        if (!formatOk) {
            continue;
        }
        choice.format = f.inPixelFormat == PrPixelFormat_Any ? PrPixelFormat_BGRA_4444_32f : f.inPixelFormat;
        // A 0 dimension means "any", exactly as imFrameFormat documents.
        choice.width = f.inFrameWidth;
        choice.height = f.inFrameHeight;
        return choice;
    }
    // Nothing matched: fall back to our preferred format at native size
    // rather than failing.  The host converts if it has to.
    return choice;
}

/// The advertised frame sizes for a clip: native, half, quarter.  Sizes are
/// forced even so the equirect stays 2:1 and the renderer never sees an odd
/// dimension.
[[nodiscard]] OutputGeometry scaledGeometry(const OutputGeometry& base, int index) noexcept {
    OutputGeometry g = base;
    for (int i = 0; i < index; ++i) {
        g.width /= 2;
        g.height /= 2;
    }
    // Keep the 2:1 ratio and stay above a sane floor.
    if (g.height < 16 || g.width < 32) {
        return OutputGeometry{};
    }
    g.width = g.height * 2;
    return g;
}

/// Snap a requested size to the nearest advertised one for this clip.
[[nodiscard]] OutputGeometry nearestAdvertisedSize(const ImporterInstance& instance, const PrefsBlob& prefs,
                                                   std::int32_t wantW, std::int32_t wantH) noexcept {
    const OutputGeometry base = instance.geometryFor(prefs);
    if (!base.valid()) {
        return base;
    }
    if (wantW <= 0 && wantH <= 0) {
        return base;
    }
    // Compare on height: the output is always 2:1 so height decides.
    const std::int32_t target = wantH > 0 ? wantH : wantW / 2;
    OutputGeometry best = base;
    std::int64_t bestDistance = std::abs(static_cast<std::int64_t>(base.height) - target);
    for (int i = 1; i <= 2; ++i) {
        const OutputGeometry candidate = scaledGeometry(base, i);
        if (!candidate.valid()) {
            break;
        }
        const std::int64_t d = std::abs(static_cast<std::int64_t>(candidate.height) - target);
        if (d < bestDistance) {
            bestDistance = d;
            best = candidate;
        }
    }
    return best;
}

/// True when this request should skip the seam search: draft quality, or a
/// scrubbing / playing intent whose playback ratio already says the host is
/// struggling.
[[nodiscard]] bool isDraftRequest(const imSourceVideoRec& rec) noexcept {
    if (rec.inQuality != kPrRenderQuality_Invalid && rec.inQuality <= kPrRenderQuality_Low) {
        return true;
    }
    switch (rec.inRenderContext.inIntent) {
    case imRenderIntent_Scrubbing:
    case imRenderIntent_Playing:
    case imRenderIntent_Preroll:
        return rec.inRenderContext.inPlaybackRatio < 1.0;
    case imRenderIntent_Thumbnail:
    case imRenderIntent_SpeculativePrefetch:
    case imRenderIntent_DistantPrefetch:
        return true;
    default:
        return false;
    }
}

/// Name of an intent for the log.
[[nodiscard]] const char* intentName(imRenderIntent intent) noexcept {
    switch (intent) {
    case imRenderIntent_Export:             return "Export";
    case imRenderIntent_Stopped:            return "Stopped";
    case imRenderIntent_Scrubbing:          return "Scrubbing";
    case imRenderIntent_Preroll:            return "Preroll";
    case imRenderIntent_Playing:            return "Playing";
    case imRenderIntent_SpeculativePrefetch:return "SpeculativePrefetch";
    case imRenderIntent_Thumbnail:          return "Thumbnail";
    case imRenderIntent_Analysis:           return "Analysis";
    case imRenderIntent_ExportPreview:      return "ExportPreview";
    case imRenderIntent_ExportProxies:      return "ExportProxies";
    case imRenderIntent_DistantPrefetch:    return "DistantPrefetch";
    case imRenderIntent_Unknown:
    default:                                return "Unknown";
    }
}

/// Read a PrSDKString back as UTF-8 through the String Suite (empty when the
/// suite is missing or the string is not one of the host's).
[[nodiscard]] std::string readString(const PrSDKString& s) noexcept {
    ImporterGlobals& g = globals();
    if (!g.suites.string || !g.suites.string->CopyToUTF8String) {
        return {};
    }
    csSDK_uint32 size = 0;
    // First call sizes the buffer; the suite reports StringBufferTooSmall.
    g.suites.string->CopyToUTF8String(&s, nullptr, &size);
    if (size == 0 || size > 4096u) {
        return {};
    }
    std::vector<prUTF8Char> buffer(size, 0);
    if (g.suites.string->CopyToUTF8String(&s, buffer.data(), &size) != suiteError_NoError) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(buffer.data()));
}

}  // namespace

// colorSpaceTokenFor() and seiCodesFor() live in PrefsMapping.cpp (no Win32,
// no suites, no instance) so the unit tests can link them directly.

// ---------------------------------------------------------------------------
//  imGetInfo8 / imGetInfo9
// ---------------------------------------------------------------------------

csSDK_int32 handleGetInfo8(imStdParms* stdParms, imFileAccessRec8* fileAccess, imFileInfoRec8* info) {
    (void)fileAccess;  // imGetInfo8 identifies the clip through privatedata.
    if (!stdParms || !info) {
        return imOtherErr;
    }
    // The host asks about one stream at a time; this importer presents the
    // clip as a single stream, so anything past 0 ends the enumeration.
    if (info->streamIdx > 0) {
        return imBadStreamIndex;
    }

    ImporterInstance* instance =
        instanceFromHandle(info->privatedata, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance) {
        // imGetInfo8 always follows imOpenFile8, which allocates privateData.
        // Reaching this means the host handed back something we did not make.
        PluginLog::error("imGetInfo8 with no instance in privateData");
        return imBadFile;
    }
    if (!instance->parsed()) {
        return imBadFile;
    }

    instance->applyPrefs(info->prefs, PrefsBlob::kSize);
    const PrefsBlob prefs = instance->prefs();
    const OutputGeometry geometry = instance->geometryFor(prefs);
    if (!geometry.valid()) {
        return imBadFile;
    }

    // importerID is an [in] field: the host stamps it before the call and it
    // is what keys the PPix cache for this clip instance.
    instance->setImporterId(static_cast<std::uint32_t>(info->vidInfo.importerID));

    info->hasVideo = 1;
    info->hasAudio = instance->hasAudio() ? 1 : 0;

    // ---- video ------------------------------------------------------------
    imImageInfoRec& vid = info->vidInfo;
    vid.imageWidth = geometry.width;
    vid.imageHeight = geometry.height;
    vid.depth = 32;
    vid.subType = kOsvSubType;
    vid.fieldType = prFieldsNone;
    // Straight (unpremultiplied) alpha carrying the lens coverage.
    //
    // A dual-fisheye stitch does not cover the whole sphere: the calibration
    // carries an occlusion polygon per lens (the camera body, the mount),
    // and the kernel writes fully transparent black for any direction that
    // neither lens sees.  Declaring alphaOpaque would tell Premiere it may
    // ignore the alpha channel, and those pixels would composite as opaque
    // black over whatever is underneath in the timeline.
    vid.alphaType = alphaStraight;
    vid.pixelAspectNum = 1;
    vid.pixelAspectDen = 1;
    vid.isStill = 0;
    vid.noDuration = imNoDurationFalse;
    vid.supportsGetSourceVideo = kPrTrue;
    // Synchronous delivery: Premiere already prefetches frames on its own
    // threads, and the async importer is explicitly phase 2 (decision D12).
    vid.supportsAsyncIO = kPrFalse;
    vid.colorSpaceSupport = imColorSpaceSupport_Fixed;
    vid.bitDepth = static_cast<csSDK_int32>(instance->format().bitDepth ? instance->format().bitDepth : 10u);
    vid.hasEmbeddedLUT = kPrFalse;

    // The exact frame period in ticks; this supersedes vidScale/vidSampleSize.
    vid.frameRate = ticksPerFrameFor(*instance);

    // codecDescription is a PrSDKString the host disposes.
    if (globals().suites.string && globals().suites.string->AllocateFromUTF8) {
        globals().suites.string->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(kCodecDescription),
                                                  &vid.codecDescription);
    }

    // vidScale / vidSampleSize are filled from the same rational for hosts
    // that predate vid.frameRate.
    info->vidScale = static_cast<csSDK_int32>(instance->rateNumerator());
    info->vidSampleSize = static_cast<csSDK_int32>(instance->rateDenominator());
    info->vidDurationInFrames = static_cast<csSDK_int64>(instance->frameCount());

    // vidDuration is the duration in the video timebase, i.e. frames *
    // sampleSize (PrSDKImport.h:399-401).  Compute it in 64 bits and SATURATE:
    // both operands are 32-bit unsigned, so `frames * den` wraps in 32-bit
    // arithmetic long before the cast can notice, and a wrapped value reads
    // as a negative duration to any host that looks at it.  vidDurationInFrames
    // is set above and supersedes this field (PrSDKImport.h:426), but a field
    // we fill must not be able to hold a lie.
    const std::int64_t durationTimebase =
        static_cast<std::int64_t>(instance->frameCount()) * static_cast<std::int64_t>(instance->rateDenominator());
    constexpr std::int64_t kMaxInt32 = 0x7FFFFFFF;
    if (durationTimebase > kMaxInt32) {
        PluginLog::oncef("importer/viddur", PluginLog::Level::Warn,
                         "imGetInfo8: the timebase duration {} exceeds csSDK_int32; clamping vidDuration "
                         "(vidDurationInFrames is exact and supersedes it)",
                         durationTimebase);
        info->vidDuration = static_cast<csSDK_int32>(kMaxInt32);
    } else {
        info->vidDuration = static_cast<csSDK_int32>(durationTimebase);
    }

    // ---- VR ---------------------------------------------------------------
    info->ivProjectionType = kPrIVProjectionType_Equirectangular;
    info->ivFrameLayout = kPrIVFrameLayout_Monoscopic;
    info->ivHorizontalCapturedView = 360;
    info->ivVerticalCapturedView = 180;

    // ---- audio ------------------------------------------------------------
    if (instance->hasAudio()) {
        info->audInfo.numChannels = instance->audioChannels();
        info->audInfo.sampleRate = static_cast<float>(instance->audioSampleRate());
        // Informational only; the host still receives float buffers from us.
        info->audInfo.sampleType = kPrAudioSampleType_Compressed;
        info->audDuration = instance->audioDurationSamples();
    } else {
        info->audInfo.numChannels = 0;
        info->audInfo.sampleRate = 0.0f;
        info->audDuration = 0;
    }

    // Random access for scrubbing through imImportAudio7 plus the sequential
    // selectors for conforming (decision D11).
    info->accessModes = kSeparateSequentialAudio;

    info->streamsAsComp = 0;
    info->hasDataRate = 0;
    info->hasDataStreams = 0;
    info->mayBeGrowing = 0;
    info->ignoreGrowing = 1;
    info->alwaysUnquiet = 0;
    info->mayHaveCaptions = 0;
    info->canProvidePeakData = 0;

    copyUtf16(info->filePath, 2048, instance->path().wstring());
    copyUtf16(info->streamName, 256, L"");

    // ---- the Source Settings effect ----------------------------------------
    // This string is the ENTIRE binding between the importer and
    // OpenOSVSourceSettings.aex: Premiere looks for an installed effect whose
    // PiPL match name is byte-identical to it and attaches that effect to the
    // master clip, which is what puts the stitch options in the Effect
    // Controls panel instead of behind the modal dialog.  There is no
    // handshake and no diagnostic on a mismatch, so it comes from the same
    // header the effect's PiPL is generated from (plugins/common/
    // SourceSettingsIdentity.h) and a test compares the two.
    //
    // The modal dialog (imGetPrefs8) is deliberately left working alongside
    // it: right-click > Source Settings is muscle memory for a lot of users,
    // and a machine where the .aex failed to install still needs a way to
    // reach the options.  Both paths write the same PrefsBlob.
    copyUtf16(info->sourceSettingsMatchName, 256, kSourceSettingsMatchNameW);

    PluginLog::info("imGetInfo8: {} x {} equirect, {} frames, {} ticks/frame, audio {} ch", geometry.width,
                    geometry.height, instance->frameCount(), static_cast<long long>(vid.frameRate),
                    instance->audioChannels());
    return imNoErr;
}

csSDK_int32 handleGetInfo9(imStdParms* stdParms, imFileAccessRec8* fileAccess, imFileInfoRec9* info) {
    if (!info) {
        return imOtherErr;
    }
    // imGetInfo9 is imGetInfo8 plus the system-state fields.  Nothing this
    // importer advertises depends on system state (the pixel formats and the
    // colour space are the same with or without a GPU: the GPU only changes
    // how fast a frame is produced, never what it looks like), so the masks
    // stay zero and the host never has to invalidate its cache.
    const csSDK_int32 result = handleGetInfo8(stdParms, fileAccess, &info->info);
    if (result != imNoErr) {
        return result;
    }
    info->systemStateFlagMask = 0;
    info->systemStateFlagsCurrent = 0;
    info->systemStateSubtypeVersion = 0;
    return imNoErr;
}

// ---------------------------------------------------------------------------
//  Pixel formats and sizes
// ---------------------------------------------------------------------------

csSDK_int32 handleGetIndPixelFormat(imStdParms* stdParms, csSDK_int32 index, imIndPixelFormatRec* rec) {
    (void)stdParms;  // The format list is the same for every clip instance.
    if (!rec) {
        return imOtherErr;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= kPixelFormats.size()) {
        return imBadFormatIndex;
    }
    rec->outPixelFormat = kPixelFormats[static_cast<std::size_t>(index)];
    return imNoErr;
}

csSDK_int32 handleGetPreferredFrameSize(imStdParms* stdParms, imPreferredFrameSizeRec* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    ImporterInstance* instance =
        instanceFromHandle(rec->inPrivateData, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed()) {
        return imOtherErr;
    }
    instance->applyPrefs(rec->inPrefs, PrefsBlob::kSize);

    const OutputGeometry base = instance->geometryFor(instance->prefs());
    if (!base.valid()) {
        return imOtherErr;
    }
    // Index 0 native, 1 half, 2 quarter.  imIterateFrameSizes asks us back
    // for the next one; imOtherErr ends the enumeration.
    if (rec->inIndex < 0 || rec->inIndex > 2) {
        return imOtherErr;
    }
    const OutputGeometry g = scaledGeometry(base, rec->inIndex);
    if (!g.valid()) {
        return imOtherErr;
    }
    rec->outWidth = g.width;
    rec->outHeight = g.height;
    // Ask to be called again while there is a smaller size left to report.
    return rec->inIndex < 2 && scaledGeometry(base, rec->inIndex + 1).valid() ? imIterateFrameSizes : imNoErr;
}

csSDK_int32 handleSelectClipFrameDescriptor(imStdParms* stdParms, imClipFrameDescriptorRec* rec, bool isVersion2) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    ImporterInstance* instance =
        instanceFromHandle(rec->inPrivateData, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed()) {
        return imOtherErr;
    }
    instance->applyPrefs(rec->inPrefs, PrefsBlob::kSize);
    const PrefsBlob prefs = instance->prefs();

    // Start from what the host wants and coerce only what we cannot serve.
    rec->outBestFrameDescriptor = rec->inDesiredClipFrameDescriptor;

    PrPixelFormat format = rec->inDesiredClipFrameDescriptor.inPixelFormat;
    if (!isSupportedFormat(format)) {
        format = PrPixelFormat_BGRA_4444_32f;
    }
    if (isVersion2) {
        // "Maximum Bit Depth" off is an explicit request for the cheap path.
        const auto* rec2 = static_cast<const imClipFrameDescriptorRec2*>(rec);
        if (rec2->inDesiredMaxBitDepth == kMaxBitDepth_Off) {
            format = PrPixelFormat_BGRA_4444_8u;
        }
    }
    rec->outBestFrameDescriptor.inPixelFormat = format;

    const OutputGeometry g = nearestAdvertisedSize(*instance, prefs, rec->inDesiredClipFrameDescriptor.inWidth,
                                                   rec->inDesiredClipFrameDescriptor.inHeight);
    if (g.valid()) {
        rec->outBestFrameDescriptor.inWidth = g.width;
        rec->outBestFrameDescriptor.inHeight = g.height;
    }
    rec->outBestFrameDescriptor.inPixelAspectRatioNumerator = 1;
    rec->outBestFrameDescriptor.inPixelAspectRatioDenominator = 1;
    rec->outBestFrameDescriptor.inFieldType = prFieldsNone;
    return imNoErr;
}

// ---------------------------------------------------------------------------
//  imGetIndColorSpace
// ---------------------------------------------------------------------------

csSDK_int32 handleGetIndColorSpace(imStdParms* stdParms, csSDK_int32 index, imIndColorSpaceRec* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    // "At present, importer is expected to report a single media color space"
    // (PrSDKImport.h): index 0 is the answer, anything else ends it.
    if (index != 0) {
        return imBadFormatIndex;
    }

    ImporterInstance* instance =
        instanceFromHandle(rec->inPrivateData, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    // The colour space depends only on the prefs, so an instance-less call
    // (which the host makes before opening in some paths) still gets the
    // default answer rather than an error.
    const PrefsBlob prefs = instance ? instance->prefs() : PrefsBlob::defaults();

#if defined(OSV_IMPORTER_COLOR_SEI)
    // The SEI-tag variant, kept behind a compile-time switch so the runtime
    // comparison on a real host is one rebuild away (decision D4).
    const SeiCodes codes = seiCodesFor(prefs);
    rec->outColorSpaceType = kPrSDKColorSpaceType_SEITags;
    rec->outSEICodesRec.colorPrimariesCode = codes.primaries;
    rec->outSEICodesRec.transferCharacteristicCode = codes.transfer;
    rec->outSEICodesRec.matrixEquationsCode = codes.matrix;
    rec->outSEICodesRec.bitDepth = 32;
    rec->outSEICodesRec.isFullRange = kPrTrue;
    rec->outSEICodesRec.isRGB = kPrTrue;
    rec->outSEICodesRec.isSceneReferred = kPrFalse;
    return imNoErr;
#else
    const char* token = colorSpaceTokenFor(prefs);
    ImporterGlobals& g = globals();
    if (!g.suites.string || !g.suites.string->AllocateFromUTF8) {
        // Without the String Suite the name cannot be handed over at all;
        // saying "undefined" is better than handing back a garbage string.
        PluginLog::oncef("colorspace-no-string-suite", PluginLog::Level::Warn,
                         "imGetIndColorSpace: the String Suite is unavailable; the colour space cannot be declared");
        return imUnsupported;
    }
    rec->outColorSpaceType = kPrSDKColorSpaceType_Predefined;
    const prSuiteError err =
        g.suites.string->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(token), &rec->ioProfileRec.outName);
    if (err != suiteError_NoError) {
        PluginLog::warn("imGetIndColorSpace: AllocateFromUTF8('{}') failed with {}", token, static_cast<int>(err));
        return imOtherErr;
    }
    PluginLog::oncef(std::string("colorspace-") + token, PluginLog::Level::Info,
                     "imGetIndColorSpace: declaring '{}'", token);
    return imNoErr;
#endif
}

// ---------------------------------------------------------------------------
//  imGetSourceVideo
// ---------------------------------------------------------------------------

csSDK_int32 handleGetSourceVideo(imStdParms* stdParms, imSourceVideoRec* rec) {
    if (!stdParms || !rec || !rec->outFrame) {
        return imOtherErr;
    }
    ImporterGlobals& g = globals();
    if (!g.suites.ppix || !g.suites.ppix->GetPixels || !g.suites.ppix->GetRowBytes) {
        PluginLog::error("imGetSourceVideo: the PPix Suite is unavailable");
        return imOtherErr;
    }

    ImporterInstance* instance =
        instanceFromHandle(rec->inPrivateData, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed()) {
        return imBadFile;
    }

    // Work out the blob this frame must be rendered with BEFORE taking the
    // lock, then apply it inside the lock together with the render, so the
    // bytes that key the cache entry are exactly the bytes the renderer used
    // even when another thread changes the settings in between.
    // A request that carries no prefs must NOT reset the clip's settings to
    // the defaults - fromBytes() would do exactly that - so the instance's
    // current blob is used instead.
    const bool haveHostPrefs = rec->prefs != nullptr && rec->prefsSize >= static_cast<csSDK_int32>(PrefsBlob::kSize);
    PrefsBlob prefs = haveHostPrefs ? PrefsBlob::fromBytes(rec->prefs, static_cast<std::size_t>(rec->prefsSize))
                                    : instance->prefs();

    // The connection-space rule: when the host could not use the space we
    // declared it hands back "BT.709 RGB Full" and we must convert ourselves.
    // Rendering with the Rec.709 transfer is exactly that conversion.  This
    // overrides the clip's own setting for this frame only, which is why it
    // is applied to the local blob and then pushed into the instance below -
    // writing it back into the stored prefs would make the override sticky.
    const std::string selected = readString(rec->selectedColorProfileName);
    if (!selected.empty() && selected == kPrOverranged709 && prefs.color() != PrefsColorOutput::Rec709) {
        PluginLog::oncef("colorspace-fallback-709", PluginLog::Level::Info,
                         "the host selected '{}'; rendering with the Rec.709 transfer instead of the declared space",
                         kPrOverranged709);
        prefs.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    }

    const FormatChoice choice = chooseFormat(*rec);
    const std::size_t bpp = bytesPerPixelFor(choice.format);
    if (bpp == 0) {
        return imUnsupported;
    }

    const OutputGeometry geometry = nearestAdvertisedSize(*instance, prefs, choice.width, choice.height);
    if (!geometry.valid()) {
        return imOtherErr;
    }

    const std::uint32_t frameIndex = frameIndexFor(*instance, rec->inFrameTime);
    const bool draft = isDraftRequest(*rec);

    // Log the first few requests with everything that decides what we do, so
    // a support log shows the host's real behaviour without drowning.
    instance->noteVideoRequest();
    if (instance->videoRequestCount() <= 5) {
        PluginLog::info(
            "imGetSourceVideo #{}: t={} -> frame {}, {} formats requested, chose {}x{} fmt 0x{:08X}, quality {}, "
            "intent {} (ratio {:.2f}), draft {}",
            instance->videoRequestCount(), static_cast<long long>(rec->inFrameTime), frameIndex,
            rec->inNumFrameFormats, geometry.width, geometry.height, static_cast<unsigned>(choice.format),
            static_cast<int>(rec->inQuality), intentName(rec->inRenderContext.inIntent),
            rec->inRenderContext.inPlaybackRatio, draft ? 1 : 0);
    }

    // ---- cache lookup ------------------------------------------------------
    // The blob is part of the key, so changed settings never hit a stale
    // frame.  The colour space id is part of it too, which matters when the
    // host switches its working space mid-session.
    imFrameFormat wanted{};
    wanted.inFrameWidth = geometry.width;
    wanted.inFrameHeight = geometry.height;
    wanted.inPixelFormat = choice.format;

    // The importer id the host assigned in imOpenFile8 / imGetInfo8 is what
    // keys every cache entry; without it the cache would be shared between
    // clips, so a zero id disables the cache entirely rather than risking a
    // cross-clip hit.
    const csSDK_uint32 importerId = instance->importerId();

    if (importerId != 0 && g.suites.ppixCache) {
        PPixHand cached = nullptr;
        prSuiteError cacheErr = suiteError_Fail;
        if (g.suites.ppixCache->GetFrameFromCacheWithColorSpace) {
            cacheErr = g.suites.ppixCache->GetFrameFromCacheWithColorSpace(
                importerId, 0, static_cast<csSDK_int32>(frameIndex), 1, &wanted, &cached, rec->inQuality, &prefs,
                PrefsBlob::cacheKeySize(), &rec->opaqueColorSpaceIdentifier);
        } else if (g.suites.ppixCache->GetFrameFromCache) {
            cacheErr = g.suites.ppixCache->GetFrameFromCache(importerId, 0, static_cast<csSDK_int32>(frameIndex), 1,
                                                             &wanted, &cached, &prefs, PrefsBlob::cacheKeySize());
        }
        if (cacheErr == suiteError_NoError && cached) {
            *rec->outFrame = cached;
            return imNoErr;
        }
    }

    // ---- render ------------------------------------------------------------
    // One lock for decode + render + copy: the image the instance returns
    // lives in its frame cache and must not be replaced by another thread
    // while we are reading it.
    std::lock_guard<std::mutex> guard(instance->lock());
    instance->applyPrefsLocked(&prefs, PrefsBlob::kSize);

    auto rendered = instance->renderFrame(frameIndex, geometry, draft);
    if (!rendered.ok()) {
        PluginLog::error("imGetSourceVideo: frame {} failed: {}", frameIndex, rendered.error().message);
        // A decode failure for one frame is not a bad file; the host shows a
        // missing frame and carries on.
        return rendered.error().code == ErrorCode::InvalidArgument ? imFrameNotFound : imDecompressionError;
    }
    const render::ImageRGBAf& image = *rendered.value();
    if (!image.valid() || image.w != static_cast<std::uint32_t>(geometry.width) ||
        image.h != static_cast<std::uint32_t>(geometry.height)) {
        return imOtherErr;
    }

    // ---- create the PPix ---------------------------------------------------
    PPixHand frame = nullptr;
    // CreateColorManagedPPix is a PPixCreator2 v4 addition (the header marks
    // it "Pr 14.0; color managed extensions"), and the suite is now acquired
    // with a 4 -> 1 fallback so an older host still gets a working importer.
    // The VERSION test has to come first: on a v3 struct the member simply
    // does not exist, so reading its pointer would read past the end of what
    // the host allocated and a null test would prove nothing.
    const bool haveColorSpace =
        g.suites.ppixCreator2 && g.suites.ppixCreator2.version() >= kPrSDKPPixCreator2SuiteVersion4 &&
        g.suites.ppixCreator2->CreateColorManagedPPix &&
        (rec->opaqueColorSpaceIdentifier.opaque[0] != 0 || rec->opaqueColorSpaceIdentifier.opaque[1] != 0);
    prSuiteError createErr = suiteError_Fail;
    if (haveColorSpace) {
        createErr = g.suites.ppixCreator2->CreateColorManagedPPix(&frame, PrPPixBufferAccess_ReadWrite, choice.format,
                                                                  geometry.width, geometry.height, false, 0, 1, 1,
                                                                  rec->opaqueColorSpaceIdentifier);
    }
    if ((createErr != suiteError_NoError || !frame) && g.suites.ppixCreator2 && g.suites.ppixCreator2->CreatePPix) {
        createErr = g.suites.ppixCreator2->CreatePPix(&frame, PrPPixBufferAccess_ReadWrite, choice.format,
                                                      geometry.width, geometry.height, false, 0, 1, 1);
    }
    if ((createErr != suiteError_NoError || !frame) && g.suites.ppixCreator && g.suites.ppixCreator->CreatePPix) {
        // The legacy creator takes a rect instead of a size.
        prRect bounds{};
        bounds.left = 0;
        bounds.top = 0;
        bounds.right = static_cast<csSDK_int16>(geometry.width);
        bounds.bottom = static_cast<csSDK_int16>(geometry.height);
        createErr = g.suites.ppixCreator->CreatePPix(&frame, PrPPixBufferAccess_ReadWrite, choice.format, &bounds);
    }
    if (createErr != suiteError_NoError || !frame) {
        PluginLog::error("imGetSourceVideo: PPix creation failed with {}", static_cast<int>(createErr));
        return imMemErr;
    }

    // ---- copy --------------------------------------------------------------
    char* pixels = nullptr;
    csSDK_int32 rowBytes = 0;
    if (g.suites.ppix->GetPixels(frame, PrPPixBufferAccess_ReadWrite, &pixels) != suiteError_NoError || !pixels ||
        g.suites.ppix->GetRowBytes(frame, &rowBytes) != suiteError_NoError || rowBytes == 0) {
        g.suites.ppix->Dispose(frame);
        PluginLog::error("imGetSourceVideo: GetPixels / GetRowBytes failed");
        return imMemErr;
    }

    pixelcopy::HostFrame dst;
    dst.base = pixels;
    dst.rowBytes = rowBytes;
    dst.width = static_cast<std::uint32_t>(geometry.width);
    dst.height = static_cast<std::uint32_t>(geometry.height);

    // Host 4444 frames are BOTTOM-LEFT origin; PixelCopy's host* functions do
    // the row flip.  The thread pool comes from the process-wide context so
    // a 6000x3000 copy is not single threaded.
    //
    // Take a shared_ptr LEASE on the pool, not a raw pointer.  HostContext is
    // process-wide, so the per-clip mutex held here gives no protection at
    // all against imShutdown running on another thread: that calls
    // HostContext::shutdown(), which deletes the context, drops the last
    // reference to the pool and JOINS its workers.  A 6000x3000 32f copy
    // takes tens of milliseconds, which is a wide window.  The lease keeps
    // the pool alive for the duration of the copy; an exists() test would
    // only narrow the race, not close it, because it releases its lock
    // before we ever touch the pool.
    std::shared_ptr<ThreadPool> poolLease;
    if (HostContext::exists()) {
        poolLease = HostContext::instance().threadPoolShared();
    }
    ThreadPool* pool = poolLease.get();
    Status copyStatus = choice.format == PrPixelFormat_BGRA_4444_32f
                            ? pixelcopy::rgbaToHostBgra32f(image, dst, pool)
                            : pixelcopy::rgbaToHostBgra8u(image, dst, pool);
    if (!copyStatus.ok()) {
        g.suites.ppix->Dispose(frame);
        PluginLog::error("imGetSourceVideo: pixel copy failed: {}", copyStatus.error().message);
        return imOtherErr;
    }

    // ---- cache + hand over -------------------------------------------------
    if (importerId != 0 && g.suites.ppixCache) {
        if (g.suites.ppixCache->AddFrameToCacheWithColorSpace) {
            g.suites.ppixCache->AddFrameToCacheWithColorSpace(importerId, 0, frame,
                                                              static_cast<csSDK_int32>(frameIndex), rec->inQuality,
                                                              &prefs, PrefsBlob::cacheKeySize(),
                                                              &rec->opaqueColorSpaceIdentifier);
        } else if (g.suites.ppixCache->AddFrameToCache) {
            g.suites.ppixCache->AddFrameToCache(importerId, 0, frame, static_cast<csSDK_int32>(frameIndex), &prefs,
                                                PrefsBlob::cacheKeySize());
        }
    }

    *rec->outFrame = frame;
    return imNoErr;
}

// ---------------------------------------------------------------------------
//  Small informational selectors
// ---------------------------------------------------------------------------

csSDK_int32 handleAnalysis(imStdParms* stdParms, imAnalysisRec* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    ImporterInstance* instance =
        instanceFromHandle(rec->privatedata, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed()) {
        return imOtherErr;
    }
    instance->applyPrefs(rec->prefs, PrefsBlob::kSize);

    const std::string text = instance->analysisText();
    // Two-step protocol: the first call has no buffer and only wants a size.
    if (!rec->buffer) {
        rec->buffersize = static_cast<csSDK_int32>(text.size() + 1u);
        return imNoErr;
    }
    if (rec->buffersize <= 0) {
        return imNoErr;
    }
    const std::size_t capacity = static_cast<std::size_t>(rec->buffersize);
    const std::size_t n = std::min(text.size(), capacity - 1u);
    std::memcpy(rec->buffer, text.data(), n);
    rec->buffer[n] = '\0';
    return imNoErr;
}

csSDK_int32 handleGetTimeInfo8(imStdParms* stdParms, imTimeInfoRec8* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    ImporterInstance* instance =
        instanceFromHandle(rec->privatedata, stdParms->piSuites ? stdParms->piSuites->memFuncs : nullptr);
    if (!instance || !instance->parsed()) {
        return imNoTimecode;
    }
    // DJI writes no 'tmcd' track, so there is no real start timecode.  The
    // container creation time is a date, not a timecode, and inventing one
    // from it would put a wrong number in the Media Start column.
    // imNoTimecode is a non-error return that tells the host exactly that.
    return imNoTimecode;
}

csSDK_int32 handleGetFileAttributes(imStdParms* stdParms, imFileAttributesRec* rec) {
    if (!stdParms || !rec) {
        return imOtherErr;
    }
    // The record carries no privateData, so the date has to come from the
    // file system rather than from the parsed container.  That is also what
    // the host would show without an importer, so the column stays truthful.
    std::memset(&rec->creationDateStamp, 0, sizeof(rec->creationDateStamp));
    return imUnsupported;
}

}  // namespace osv::premiere
