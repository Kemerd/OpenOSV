// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HevcStreamDecoder implementation on top of libavcodec / libavformat.
//
// Two ways of getting packets into the decoder (see Decoder.h):
//   * avformat mode  - libavformat demuxes the MP4 through an AVIOContext
//                      whose callbacks read from our MappedFile, so files and
//                      in-memory buffers behave identically.  Seeking uses
//                      av_seek_frame(AVSEEK_FLAG_BACKWARD); the OpenOSV
//                      SampleTable, when the container parsed, only tells us
//                      whether a forward request is still inside the current
//                      GOP (so we can skip the seek).
//   * samples mode   - DecoderOptions::useContainerSamples.  The OpenOSV
//                      container parser supplies every sample; each one is
//                      rewritten from 4-byte NAL length prefixes to Annex-B
//                      start codes and pushed with avcodec_send_packet.  The
//                      extradata is the hvcC parameter sets in Annex-B form.
// Both modes share the same receive loop, the same pts -> frame index mapping
// and the same AVFrame -> PlanarFrame16 wrapping.

#include "osv/video/Decoder.h"

#include "ContainerSource.h"
#include "FfmpegCommon.h"
#include "HwDeviceCache.h"
#include "osv/core/Log.h"
#include "osv/core/MappedFile.h"

#if defined(OSV_VIDEO_HAVE_CUDA)
#include <cuda_runtime.h>
// AVCUDADeviceContext, for decoding into a caller-supplied CUcontext.  The
// header declares types only (it pulls in cuda.h for CUcontext / CUstream),
// so nothing in this file links against the driver API because of it.
extern "C" {
#include <libavutil/hwcontext_cuda.h>
}
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace osv::video {

namespace {

/// Milliseconds elapsed since `start` on the steady clock (never negative).
[[nodiscard]] double msSince(std::chrono::steady_clock::time_point start) noexcept {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double ms = std::chrono::duration<double, std::milli>(elapsed).count();
    return ms > 0.0 ? ms : 0.0;
}

}  // namespace

// =============================================================================
//  FFmpeg log routing (process wide, installed once)
// =============================================================================
namespace {

/// Forward libav* log lines into osv::log so host applications see them in
/// their own sink.  Messages are truncated to a sane length and scrubbed to
/// 7-bit ASCII.
void ffmpegLogCallback(void* /*avcl*/, int level, const char* fmt, va_list args) {
    if (!fmt) {
        return;
    }
    // Map FFmpeg's levels onto ours before formatting so we do not pay for
    // strings nobody wants to see (AV_LOG_DEBUG and below are dropped).
    log::Level target = log::Level::Trace;
    if (level <= AV_LOG_ERROR) {
        target = log::Level::Error;
    } else if (level <= AV_LOG_WARNING) {
        target = log::Level::Warn;
    } else if (level <= AV_LOG_INFO) {
        target = log::Level::Debug;
    } else if (level <= AV_LOG_VERBOSE) {
        target = log::Level::Trace;
    } else {
        return;
    }
    if (!log::enabled(target)) {
        return;
    }
    char buffer[1024] = {};
    va_list copy;
    va_copy(copy, args);
    const int written = std::vsnprintf(buffer, sizeof(buffer), fmt, copy);
    va_end(copy);
    if (written <= 0) {
        return;
    }
    std::string text(buffer, static_cast<std::size_t>(std::min<int>(written, static_cast<int>(sizeof(buffer) - 1))));
    // FFmpeg terminates lines itself; our logger adds its own newline.
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.empty()) {
        return;
    }
    log::message(target, "ffmpeg: " + log::safe(text));
}

/// Install the callback exactly once per process.
void ensureFfmpegLogging() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_WARNING);
        av_log_set_callback(&ffmpegLogCallback);
    });
}

/// Bits per luma sample of a software pixel format (0 when unknown).
std::uint8_t formatBitDepth(AVPixelFormat format) noexcept {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    if (!desc || desc->nb_components < 1) {
        return 0;
    }
    return static_cast<std::uint8_t>(desc->comp[0].depth);
}

/// True for formats that live in GPU memory (AV_PIX_FMT_CUDA, D3D11, ...).
bool isHwFormat(AVPixelFormat format) noexcept {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    return desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

/// Printable pixel format name (never null).
const char* formatName(AVPixelFormat format) noexcept {
    const char* name = av_get_pix_fmt_name(format);
    return name ? name : "unknown";
}

/// AVHWDeviceType for one of our back-ends (NONE for None / Auto).
AVHWDeviceType hwDeviceType(HwAccel hw) noexcept {
    switch (hw) {
    case HwAccel::D3D11VA: return AV_HWDEVICE_TYPE_D3D11VA;
    case HwAccel::Cuda: return AV_HWDEVICE_TYPE_CUDA;
    case HwAccel::None:
    case HwAccel::Auto:
    default: return AV_HWDEVICE_TYPE_NONE;
    }
}

}  // namespace

// =============================================================================
//  HwAccel helpers (declared in HwAccel.h)
// =============================================================================
const char* hwAccelName(HwAccel hw) noexcept {
    switch (hw) {
    case HwAccel::None: return "none";
    case HwAccel::D3D11VA: return "d3d11va";
    case HwAccel::Cuda: return "cuda";
    case HwAccel::Auto: return "auto";
    }
    return "none";
}

std::optional<HwAccel> parseHwAccel(std::string_view text) noexcept {
    // Lower-case copy so "D3D11VA" and "d3d11va" both work on the CLI.
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text) {
        lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (lower == "none" || lower == "sw" || lower == "software") {
        return HwAccel::None;
    }
    if (lower == "d3d11va" || lower == "d3d11") {
        return HwAccel::D3D11VA;
    }
    if (lower == "cuda" || lower == "nvdec") {
        return HwAccel::Cuda;
    }
    if (lower == "auto") {
        return HwAccel::Auto;
    }
    return std::nullopt;
}

// =============================================================================
//  Impl
// =============================================================================
struct HevcStreamDecoder::Impl {
    // ---- source -------------------------------------------------------------
    std::filesystem::path path;                          ///< File being decoded (for messages).
    MappedFile mapping;                                  ///< Backing bytes for the AVIO callbacks.
    std::uint64_t avioPos = 0;                           ///< Read cursor of the AVIO callbacks.
    AVIOContext* avio = nullptr;                         ///< Custom IO (freed manually, see close()).
    std::unique_ptr<detail::ContainerSource> container;  ///< Our parser (sync info / sample feed).
    bool samplesMode = false;                            ///< True when packets come from `container`.

    // ---- libavformat (avformat mode only) ----------------------------------
    ff::FormatContextPtr fmt;
    int streamIndex = -1;
    AVStream* stream = nullptr;                          ///< Owned by `fmt`.
    std::int64_t startPts = 0;                           ///< pts of frame 0 (edit-list offset).

    // ---- libavcodec ---------------------------------------------------------
    const AVCodec* codec = nullptr;
    ff::CodecContextPtr codecCtx;
    ff::BufferRefPtr hwDevice;
    /// The process-wide device `hwDevice` references (see HwDeviceCache.h);
    /// holding it is what keeps the device alive for the next decoder.
    std::shared_ptr<detail::SharedHwDevice> sharedDevice;
    bool shareHwDevice = true;                           ///< DecoderOptions::shareHwDevice.
    int hwDeviceSlot = 0;                                ///< DecoderOptions::hwDeviceSlot.
    bool deferFirstFrame = false;                        ///< DecoderOptions::deferFirstFrame.
    ff::PacketPtr packet;                                ///< Reused for every send.
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;            ///< Format get_format() must pick.
    HwAccel requestedHw = HwAccel::None;
    HwAccel activeHw = HwAccel::None;
    bool keepOnDevice = false;
    int cudaDeviceIndex = 0;
    void* cudaContext = nullptr;                         ///< Caller's CUcontext (nullptr = FFmpeg creates one).
    void* cudaStream = nullptr;                          ///< Caller's CUstream for FFmpeg's copies (with cudaContext).

    // ---- stream properties --------------------------------------------------
    std::uint32_t trackId = 0;
    std::uint32_t frameCount = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t sourceBitDepth = 0;
    double fps = 0.0;
    TimeBase timeBase{1, 60000};

    // ---- decode state -------------------------------------------------------
    std::uint32_t nextIndex = 0;          ///< What next() returns.
    bool positionValid = false;           ///< True once a decode succeeded and nothing failed since.
    std::int64_t lastDecodedIndex = -1;   ///< Index of the last frame libavcodec produced (-1 = none).
    bool eofSent = false;                 ///< avcodec_send_packet(nullptr) already issued.
    std::uint32_t sampleCursor = 0;       ///< Samples mode: next sample to push.
    std::optional<std::int64_t> lastPts;  ///< pts of the last frame handed out.
    std::optional<DeviceFrameRef> lastDevice;

    // ---- diagnostics --------------------------------------------------------
    DecoderOpenTimings timings;           ///< Filled phase by phase while open() runs.

    // ---- lifetime -----------------------------------------------------------
    ~Impl() { close(); }

    /// Release everything in the right order.  libavformat does not touch a
    /// custom AVIOContext, so its buffer and the context are ours to free.
    void close() noexcept {
        lastDevice.reset();
        packet.reset();
        codecCtx.reset();
        hwDevice.reset();
        // Last reference to the device record goes after the codec context
        // and our own ref, so a shared device outlives every user it had.
        sharedDevice.reset();
        fmt.reset();
        stream = nullptr;
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
            avio = nullptr;
        }
        container.reset();
    }

    // -------------------------------------------------------------------------
    //  AVIO callbacks over the mapping
    // -------------------------------------------------------------------------
    static int avioRead(void* opaque, std::uint8_t* buf, int bufSize) noexcept {
        Impl* self = static_cast<Impl*>(opaque);
        if (!self || !buf || bufSize <= 0) {
            return AVERROR(EINVAL);
        }
        const ByteSpan bytes = self->mapping.span();
        if (self->avioPos >= bytes.size()) {
            return AVERROR_EOF;
        }
        const std::uint64_t remaining = bytes.size() - self->avioPos;
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(bufSize)));
        std::memcpy(buf, bytes.data() + self->avioPos, take);
        self->avioPos += take;
        return static_cast<int>(take);
    }

    static std::int64_t avioSeek(void* opaque, std::int64_t offset, int whence) noexcept {
        Impl* self = static_cast<Impl*>(opaque);
        if (!self) {
            return AVERROR(EINVAL);
        }
        const std::int64_t size = static_cast<std::int64_t>(self->mapping.size());
        // AVSEEK_SIZE asks for the stream length without moving the cursor.
        if (whence & AVSEEK_SIZE) {
            return size;
        }
        std::int64_t base = 0;
        switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = static_cast<std::int64_t>(self->avioPos); break;
        case SEEK_END: base = size; break;
        default: return AVERROR(EINVAL);
        }
        const std::int64_t target = base + offset;
        if (target < 0 || target > size) {
            return AVERROR(EINVAL);
        }
        self->avioPos = static_cast<std::uint64_t>(target);
        return target;
    }

    // -------------------------------------------------------------------------
    //  get_format: pick the hardware surface format when one was set up
    // -------------------------------------------------------------------------
    static AVPixelFormat getFormat(AVCodecContext* ctx, const AVPixelFormat* formats) noexcept {
        Impl* self = ctx ? static_cast<Impl*>(ctx->opaque) : nullptr;
        if (!formats) {
            return AV_PIX_FMT_NONE;
        }
        if (self && self->hwPixFmt != AV_PIX_FMT_NONE) {
            for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
                if (*p == self->hwPixFmt) {
                    return *p;
                }
            }
            // The decoder did not offer our surface type (e.g. the GPU lacks
            // Main10 support): drop to software and say so once.
            log::warn("video: {} hardware decoding unavailable for this stream, falling back to software",
                      hwAccelName(self->activeHw));
            self->activeHw = HwAccel::None;
            self->hwPixFmt = AV_PIX_FMT_NONE;
        }
        // First software format offered.
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
            if (!isHwFormat(*p)) {
                return *p;
            }
        }
        return formats[0];
    }

    // -------------------------------------------------------------------------
    //  Hardware device creation
    // -------------------------------------------------------------------------
    Status setupHardware(HwAccel want) {
        const AVHWDeviceType type = hwDeviceType(want);
        if (type == AV_HWDEVICE_TYPE_NONE || !codec) {
            return failStatus(ErrorCode::InvalidArgument, "no hardware type requested");
        }
        // The decoder must advertise a hw config that works through a
        // device context for this type (hevc/h264 do for both back-ends).
        AVPixelFormat surface = AV_PIX_FMT_NONE;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) {
                break;
            }
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 && cfg->device_type == type) {
                surface = cfg->pix_fmt;
                break;
            }
        }
        if (surface == AV_PIX_FMT_NONE) {
            return failStatus(ErrorCode::Unsupported, std::string("decoder ") + codec->name +
                                                          " has no " + hwAccelName(want) + " hardware path");
        }
        // A caller-supplied CUDA context takes its own route: FFmpeg must
        // wrap that context rather than create one, and the runtime-API
        // device check below would bind this thread to the primary context,
        // which is exactly what a host with its own context must not see.
        if (want == HwAccel::Cuda && cudaContext != nullptr) {
            return setupExternalCuda(surface);
        }
#if defined(OSV_VIDEO_HAVE_CUDA)
        // With the CUDA runtime linked we can validate the device ordinal up
        // front and give a clearer message than FFmpeg's generic failure.
        if (want == HwAccel::Cuda) {
            int count = 0;
            const cudaError_t err = cudaGetDeviceCount(&count);
            if (err != cudaSuccess || count <= 0) {
                return failStatus(ErrorCode::Unsupported, std::string("no CUDA device available (") +
                                                              cudaGetErrorString(err) + ")");
            }
            if (cudaDeviceIndex < 0 || cudaDeviceIndex >= count) {
                return failStatus(ErrorCode::InvalidArgument, "CUDA device index " + std::to_string(cudaDeviceIndex) +
                                                                  " out of range (" + std::to_string(count) + " devices)");
            }
        }
#endif
        // Device string: CUDA takes the ordinal, D3D11VA the adapter index
        // (empty = default adapter).
        std::string deviceName;
        if (want == HwAccel::Cuda) {
            deviceName = std::to_string(cudaDeviceIndex);
        }
        // The device itself comes from the process-wide cache (or is created
        // privately when the caller opted out of sharing).
        auto acquired = detail::acquireHwDevice(type, deviceName, hwDeviceSlot, shareHwDevice);
        if (!acquired.ok()) {
            return failStatus(acquired.error().code, acquired.error().message);
        }
        if (!acquired.value().device || !acquired.value().device->ref()) {
            return failStatus(ErrorCode::Internal, "hardware device cache returned no device");
        }
        // Our own reference for the codec context to share; the record keeps
        // the device registered for the next decoder.
        AVBufferRef* own = av_buffer_ref(acquired.value().device->ref());
        if (!own) {
            return failStatus(ErrorCode::Decoder, "cannot reference the hardware device");
        }
        hwDevice.reset(own);
        sharedDevice = std::move(acquired.value().device);
        timings.hwDeviceReused = acquired.value().reused;
        hwPixFmt = surface;
        return okStatus();
    }

    /// Build the CUDA device context around the caller's CUcontext (and
    /// stream).  av_hwdevice_ctx_alloc leaves AVCUDADeviceContext zeroed;
    /// filling cuda_ctx before av_hwdevice_ctx_init makes FFmpeg adopt the
    /// context without creating, retaining or destroying anything (its
    /// uninit only tears down contexts it allocated itself).
    Status setupExternalCuda(AVPixelFormat surface) {
#if defined(OSV_VIDEO_HAVE_CUDA)
        if (cudaContext == nullptr) {
            return failStatus(ErrorCode::InvalidArgument, "no external CUDA context supplied");
        }
        AVBufferRef* device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
        if (!device || !device->data) {
            av_buffer_unref(&device);
            return failStatus(ErrorCode::Unsupported, "cannot allocate a CUDA device context");
        }
        // The two fields FFmpeg reads from a user-built CUDA device context.
        auto* deviceCtx = reinterpret_cast<AVHWDeviceContext*>(device->data);
        auto* cudaCtx = static_cast<AVCUDADeviceContext*>(deviceCtx->hwctx);
        if (!cudaCtx) {
            av_buffer_unref(&device);
            return failStatus(ErrorCode::Internal, "CUDA device context has no hwctx");
        }
        cudaCtx->cuda_ctx = static_cast<CUcontext>(cudaContext);
        cudaCtx->stream = static_cast<CUstream>(cudaStream);
        // init loads the driver entry points; it fails cleanly (and we free
        // the half-built context) on a machine without nvcuda.dll.
        const int ret = av_hwdevice_ctx_init(device);
        if (ret < 0) {
            av_buffer_unref(&device);
            return failStatus(ErrorCode::Unsupported, "cannot adopt the external CUDA context: " + ff::errorString(ret));
        }
        hwDevice.reset(device);
        hwPixFmt = surface;
        log::debug("video: track {} decodes into an external CUDA context{}", trackId,
                   cudaStream ? " on a caller stream" : "");
        return okStatus();
#else
        (void)surface;
        return failStatus(ErrorCode::Unsupported, "an external CUDA context needs a build with the CUDA toolkit");
#endif
    }

    // -------------------------------------------------------------------------
    //  Packet feeding
    // -------------------------------------------------------------------------

    /// Rewrite one length-prefixed sample into an Annex-B AVPacket.
    Status buildAnnexBPacket(const detail::SampleView& view, std::uint32_t lengthSize) {
        if (!packet) {
            return failStatus(ErrorCode::Internal, "packet not allocated");
        }
        if (lengthSize < 1 || lengthSize > 4) {
            return failStatus(ErrorCode::Malformed, "hvcC NAL length size " + std::to_string(lengthSize));
        }
        const ByteSpan in = view.bytes;
        // Pass 1: validate the NAL lengths and compute the output size.  The
        // output grows by (4 - lengthSize) bytes per NAL, zero on the Osmo.
        std::uint64_t pos = 0;
        std::uint64_t outSize = 0;
        std::uint32_t nalCount = 0;
        while (pos + lengthSize <= in.size()) {
            std::uint32_t len = 0;
            for (std::uint32_t i = 0; i < lengthSize; ++i) {
                len = (len << 8) | in[static_cast<std::size_t>(pos + i)];
            }
            pos += lengthSize;
            if (len == 0 || len > in.size() - pos) {
                return failStatus(ErrorCode::Malformed, "NAL length " + std::to_string(len) + " runs past sample of " +
                                                            std::to_string(in.size()) + " bytes");
            }
            pos += len;
            outSize += 4u + len;
            ++nalCount;
        }
        if (nalCount == 0) {
            return failStatus(ErrorCode::Malformed, "sample contains no NAL units");
        }
        if (outSize > static_cast<std::uint64_t>(INT32_MAX)) {
            return failStatus(ErrorCode::Malformed, "sample too large");
        }
        // Pass 2: copy with start codes.
        av_packet_unref(packet.get());
        const int ret = av_new_packet(packet.get(), static_cast<int>(outSize));
        if (ret < 0) {
            return failStatus(ErrorCode::Decoder, "av_new_packet: " + ff::errorString(ret));
        }
        std::uint8_t* out = packet->data;
        pos = 0;
        while (pos + lengthSize <= in.size()) {
            std::uint32_t len = 0;
            for (std::uint32_t i = 0; i < lengthSize; ++i) {
                len = (len << 8) | in[static_cast<std::size_t>(pos + i)];
            }
            pos += lengthSize;
            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            out[3] = 1;
            std::memcpy(out + 4, in.data() + pos, len);
            out += 4u + len;
            pos += len;
        }
        return okStatus();
    }

    /// Push exactly one packet (or the end-of-stream marker) into libavcodec.
    Status feedOnePacket() {
        if (!codecCtx || !packet) {
            return failStatus(ErrorCode::InvalidArgument, "decoder not open");
        }
        if (samplesMode) {
            // ---- samples mode: next container sample -------------------------
            if (!container || sampleCursor >= container->sampleCount()) {
                const int ret = avcodec_send_packet(codecCtx.get(), nullptr);
                eofSent = true;
                if (ret < 0 && ret != AVERROR_EOF) {
                    return failStatus(ErrorCode::Decoder, "flush: " + ff::errorString(ret));
                }
                return okStatus();
            }
            OSV_TRY_ASSIGN(const detail::SampleView view, container->sample(sampleCursor));
            OSV_TRY(buildAnnexBPacket(view, container->nalLengthSize()));
            packet->pts = view.dts + view.ctsOffset;
            packet->dts = view.dts;
            packet->flags = view.sync ? AV_PKT_FLAG_KEY : 0;
            packet->stream_index = 0;
            packet->time_base = AVRational{timeBase.num, timeBase.den};
            ++sampleCursor;
            const int ret = avcodec_send_packet(codecCtx.get(), packet.get());
            av_packet_unref(packet.get());
            if (ret < 0) {
                return failStatus(ErrorCode::Decoder, "avcodec_send_packet (sample " +
                                                          std::to_string(sampleCursor - 1) + "): " + ff::errorString(ret));
            }
            return okStatus();
        }
        // ---- avformat mode: next packet of our stream -----------------------
        if (!fmt) {
            return failStatus(ErrorCode::InvalidArgument, "decoder not open");
        }
        for (;;) {
            const int rr = av_read_frame(fmt.get(), packet.get());
            if (rr == AVERROR_EOF) {
                const int ret = avcodec_send_packet(codecCtx.get(), nullptr);
                eofSent = true;
                if (ret < 0 && ret != AVERROR_EOF) {
                    return failStatus(ErrorCode::Decoder, "flush: " + ff::errorString(ret));
                }
                return okStatus();
            }
            if (rr < 0) {
                return failStatus(ErrorCode::Decoder, "av_read_frame: " + ff::errorString(rr));
            }
            // Other streams are AVDISCARD_ALL, but be defensive anyway.
            if (packet->stream_index != streamIndex) {
                av_packet_unref(packet.get());
                continue;
            }
            const int ret = avcodec_send_packet(codecCtx.get(), packet.get());
            av_packet_unref(packet.get());
            if (ret < 0) {
                return failStatus(ErrorCode::Decoder, "avcodec_send_packet: " + ff::errorString(ret));
            }
            return okStatus();
        }
    }

    /// Next decoded frame in output order.  NotFound at the end of stream.
    Result<ff::FramePtr> receiveNext() {
        if (!codecCtx) {
            return Error{ErrorCode::InvalidArgument, "decoder not open"};
        }
        // Bound the loop: every iteration either yields a frame, pushes one
        // packet, or ends the stream, so frameCount + a margin is plenty.
        const std::uint64_t maxIterations = static_cast<std::uint64_t>(frameCount) * 2u + 64u;
        for (std::uint64_t iteration = 0; iteration < maxIterations; ++iteration) {
            ff::FramePtr out(av_frame_alloc());
            if (!out) {
                return Error{ErrorCode::Decoder, "av_frame_alloc failed"};
            }
            const int ret = avcodec_receive_frame(codecCtx.get(), out.get());
            if (ret == 0) {
                return out;
            }
            if (ret == AVERROR_EOF) {
                return Error{ErrorCode::NotFound, "end of stream"};
            }
            if (ret != AVERROR(EAGAIN)) {
                return Error{ErrorCode::Decoder, "avcodec_receive_frame: " + ff::errorString(ret)};
            }
            if (eofSent) {
                // EAGAIN after the flush marker cannot happen per the API
                // contract; treat it as the end rather than spinning.
                return Error{ErrorCode::NotFound, "end of stream"};
            }
            OSV_TRY(feedOnePacket());
        }
        return Error{ErrorCode::Decoder, "decoder produced no frame after feeding the whole stream"};
    }

    // -------------------------------------------------------------------------
    //  Timing helpers
    // -------------------------------------------------------------------------

    /// Presentation timestamp of a frame in stream ticks (falls back to the
    /// best-effort estimate, then to the last pts + one frame).
    std::int64_t framePts(const AVFrame& f) const noexcept {
        if (f.pts != AV_NOPTS_VALUE) {
            return f.pts;
        }
        if (f.best_effort_timestamp != AV_NOPTS_VALUE) {
            return f.best_effort_timestamp;
        }
        const double ticksPerFrame = ticksPerSecond() / (fps > 0.0 ? fps : 1.0);
        return lastPts.value_or(startPts - static_cast<std::int64_t>(std::llround(ticksPerFrame))) +
               static_cast<std::int64_t>(std::llround(ticksPerFrame));
    }

    [[nodiscard]] double ticksPerSecond() const noexcept {
        if (timeBase.num <= 0 || timeBase.den <= 0) {
            return 60000.0;
        }
        return static_cast<double>(timeBase.den) / static_cast<double>(timeBase.num);
    }

    /// Frame index = round((pts - startPts) * fps / ticksPerSecond).
    [[nodiscard]] std::int64_t indexFromPts(std::int64_t pts) const noexcept {
        const double seconds = static_cast<double>(pts - startPts) / ticksPerSecond();
        return static_cast<std::int64_t>(std::llround(seconds * fps));
    }

    /// Stream ticks for the start of frame `index` (plus a 0.4-frame margin
    /// so floating point rounding can never land just before the keyframe).
    [[nodiscard]] std::int64_t ptsForSeek(std::uint32_t index) const noexcept {
        const double seconds = (static_cast<double>(index) + 0.4) / (fps > 0.0 ? fps : 1.0);
        return startPts + static_cast<std::int64_t>(std::llround(seconds * ticksPerSecond()));
    }

    [[nodiscard]] std::int64_t ptsToMicroseconds(std::int64_t pts) const noexcept {
        return static_cast<std::int64_t>(std::llround(static_cast<double>(pts) * 1e6 / ticksPerSecond()));
    }

    // -------------------------------------------------------------------------
    //  Seeking
    // -------------------------------------------------------------------------
    Status seekTo(std::uint32_t index) {
        if (!codecCtx) {
            return failStatus(ErrorCode::InvalidArgument, "decoder not open");
        }
        avcodec_flush_buffers(codecCtx.get());
        eofSent = false;
        lastDecodedIndex = -1;
        if (samplesMode) {
            sampleCursor = container ? container->previousSync(index) : 0;
            return okStatus();
        }
        if (!fmt) {
            return failStatus(ErrorCode::InvalidArgument, "decoder not open");
        }
        const int ret = av_seek_frame(fmt.get(), streamIndex, ptsForSeek(index), AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            return failStatus(ErrorCode::Decoder, "av_seek_frame(" + std::to_string(index) + "): " + ff::errorString(ret));
        }
        return okStatus();
    }

    /// True when `index` can be reached by decoding forward from the current
    /// position without a seek.
    [[nodiscard]] bool canDecodeForwardTo(std::uint32_t index) const noexcept {
        // After open(), a failed decode or a moved-from state the decoder's
        // position is unknown: always seek.
        if (!positionValid) {
            return false;
        }
        if (index < nextIndex) {
            return false;
        }
        if (index == nextIndex) {
            return true;
        }
        // Jumping ahead inside the current GOP is cheaper than a seek plus a
        // full GOP re-decode, but only our sample table knows the GOP layout.
        if (container && lastDecodedIndex >= 0) {
            return static_cast<std::int64_t>(container->previousSync(index)) <= lastDecodedIndex;
        }
        return false;
    }

    // -------------------------------------------------------------------------
    //  AVFrame -> PlanarFrame16
    // -------------------------------------------------------------------------

    /// Fill the dimension fields shared by every layout.
    static void fillGeometry(PlanarFrame16& out, const AVFrame& f) noexcept {
        out.width = static_cast<std::uint32_t>(std::max(0, f.width));
        out.height = static_cast<std::uint32_t>(std::max(0, f.height));
        out.chromaW = (out.width + 1) / 2;
        out.chromaH = (out.height + 1) / 2;
        out.narrowRange = f.color_range != AVCOL_RANGE_JPEG;
    }

    /// Widen 8-bit planes into an owned uint16 buffer (value << 2 so the
    /// samples sit on the 10-bit scale the rest of the pipeline expects).
    static Result<PlanarFrame16> widen8(const AVFrame& f, bool interleavedChroma) {
        PlanarFrame16 out;
        fillGeometry(out, f);
        if (out.width == 0 || out.height == 0 || !f.data[0] || !f.data[1] || (!interleavedChroma && !f.data[2])) {
            return Error{ErrorCode::Decoder, "8-bit frame has no plane data"};
        }
        const std::size_t lumaElems = static_cast<std::size_t>(out.width) * out.height;
        const std::size_t chromaRow = interleavedChroma ? static_cast<std::size_t>(out.chromaW) * 2 : out.chromaW;
        const std::size_t chromaElems = chromaRow * out.chromaH;
        auto buffer = std::make_shared<std::vector<std::uint16_t>>();
        buffer->resize(lumaElems + (interleavedChroma ? chromaElems : 2 * chromaElems));
        std::uint16_t* y = buffer->data();
        std::uint16_t* c1 = y + lumaElems;
        std::uint16_t* c2 = interleavedChroma ? c1 + 1 : c1 + chromaElems;
        // Luma rows.
        for (std::uint32_t row = 0; row < out.height; ++row) {
            const std::uint8_t* src = f.data[0] + static_cast<std::ptrdiff_t>(row) * f.linesize[0];
            std::uint16_t* dst = y + static_cast<std::size_t>(row) * out.width;
            for (std::uint32_t x = 0; x < out.width; ++x) {
                dst[x] = static_cast<std::uint16_t>(static_cast<std::uint16_t>(src[x]) << 2);
            }
        }
        // Chroma rows: NV12 has one interleaved plane, yuv420p two.
        const int planes = interleavedChroma ? 1 : 2;
        for (int p = 0; p < planes; ++p) {
            std::uint16_t* base = (p == 0) ? c1 : c1 + chromaElems;
            for (std::uint32_t row = 0; row < out.chromaH; ++row) {
                const std::uint8_t* src = f.data[1 + p] + static_cast<std::ptrdiff_t>(row) * f.linesize[1 + p];
                std::uint16_t* dst = base + static_cast<std::size_t>(row) * chromaRow;
                for (std::size_t x = 0; x < chromaRow; ++x) {
                    dst[x] = static_cast<std::uint16_t>(static_cast<std::uint16_t>(src[x]) << 2);
                }
            }
        }
        out.plane = {y, c1, c2};
        out.strideElems = {out.width, chromaRow, chromaRow};
        out.bitDepth = 10;
        out.bitShift = 0;
        out.chromaInterleaved = interleavedChroma;
        out.owner = std::static_pointer_cast<void>(buffer);
        return out;
    }

    /// Wrap a decoded frame (transferring it from the GPU when necessary).
    Result<PlanarFrame16> wrapFrame(ff::FramePtr decoded, std::int64_t pts, std::uint32_t index) {
        if (!decoded) {
            return Error{ErrorCode::Internal, "null frame"};
        }
        lastDevice.reset();
        AVFrame* f = decoded.get();
        const AVPixelFormat format = static_cast<AVPixelFormat>(f->format);

        // ---- hardware surface --------------------------------------------------
        if (isHwFormat(format)) {
            // Software format the surface would map to (P010 / NV12).
            AVPixelFormat swFormat = AV_PIX_FMT_NONE;
            if (f->hw_frames_ctx && f->hw_frames_ctx->data) {
                swFormat = reinterpret_cast<const AVHWFramesContext*>(f->hw_frames_ctx->data)->sw_format;
            }
            if (format == AV_PIX_FMT_CUDA && keepOnDevice) {
                // Expose the CUDA device pointers; the AVFrame keeps the
                // surface alive for as long as the caller holds `owner`.
                std::shared_ptr<AVFrame> shared = ff::shareFrame(std::move(decoded));
                DeviceFrameRef ref;
                ref.yDevice = shared->data[0];
                ref.uvDevice = shared->data[1];
                ref.pitchBytes = static_cast<std::size_t>(std::max(0, shared->linesize[0]));
                ref.width = static_cast<std::uint32_t>(std::max(0, shared->width));
                ref.height = static_cast<std::uint32_t>(std::max(0, shared->height));
                const bool tenBit = (swFormat == AV_PIX_FMT_P010LE || swFormat == AV_PIX_FMT_P010BE);
                ref.bitDepth = tenBit ? 10 : 8;
                ref.bitShift = tenBit ? 6 : 0;
                ref.deviceIndex = cudaDeviceIndex;
                ref.owner = std::static_pointer_cast<void>(shared);
                if (!ref.valid()) {
                    return Error{ErrorCode::Decoder, "CUDA frame has no device planes"};
                }
                lastDevice = ref;
                PlanarFrame16 out;
                fillGeometry(out, *shared);
                out.bitDepth = ref.bitDepth;
                out.bitShift = ref.bitShift;
                out.chromaInterleaved = true;
                out.ptsUs = ptsToMicroseconds(pts);
                out.frameIndex = index;
                out.owner = ref.owner;
                return out;
            }
            // Copy to host memory in whatever format the device offers.
            ff::FramePtr host(av_frame_alloc());
            if (!host) {
                return Error{ErrorCode::Decoder, "av_frame_alloc failed"};
            }
            const int ret = av_hwframe_transfer_data(host.get(), f, 0);
            if (ret < 0) {
                return Error{ErrorCode::Decoder, "av_hwframe_transfer_data: " + ff::errorString(ret)};
            }
            // Copy the metadata (pts, colour tags) the transfer does not.
            av_frame_copy_props(host.get(), f);
            decoded = std::move(host);
            f = decoded.get();
        }

        // ---- host formats --------------------------------------------------
        const AVPixelFormat hostFormat = static_cast<AVPixelFormat>(f->format);
        if (f->width <= 0 || f->height <= 0) {
            return Error{ErrorCode::Decoder, "decoded frame has no dimensions"};
        }
        PlanarFrame16 out;
        switch (hostFormat) {
        case AV_PIX_FMT_YUV420P10LE: {
            // Planar 10-bit: alias the three planes.
            if (!f->data[0] || !f->data[1] || !f->data[2]) {
                return Error{ErrorCode::Decoder, "yuv420p10 frame has no plane data"};
            }
            fillGeometry(out, *f);
            std::shared_ptr<AVFrame> shared = ff::shareFrame(std::move(decoded));
            for (std::size_t p = 0; p < 3; ++p) {
                out.plane[p] = reinterpret_cast<const std::uint16_t*>(shared->data[p]);
                out.strideElems[p] = static_cast<std::size_t>(std::max(0, shared->linesize[static_cast<int>(p)])) / 2;
            }
            out.bitDepth = 10;
            out.bitShift = 0;
            out.chromaInterleaved = false;
            out.owner = std::static_pointer_cast<void>(shared);
            break;
        }
        case AV_PIX_FMT_P010LE: {
            // Biplanar 10-bit in the top bits: alias with bitShift 6.
            if (!f->data[0] || !f->data[1]) {
                return Error{ErrorCode::Decoder, "P010 frame has no plane data"};
            }
            fillGeometry(out, *f);
            std::shared_ptr<AVFrame> shared = ff::shareFrame(std::move(decoded));
            const std::uint16_t* uv = reinterpret_cast<const std::uint16_t*>(shared->data[1]);
            const std::size_t uvStride = static_cast<std::size_t>(std::max(0, shared->linesize[1])) / 2;
            out.plane = {reinterpret_cast<const std::uint16_t*>(shared->data[0]), uv, uv + 1};
            out.strideElems = {static_cast<std::size_t>(std::max(0, shared->linesize[0])) / 2, uvStride, uvStride};
            out.bitDepth = 10;
            out.bitShift = 6;
            out.chromaInterleaved = true;
            out.owner = std::static_pointer_cast<void>(shared);
            break;
        }
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P: {
            OSV_TRY_ASSIGN(out, widen8(*f, false));
            break;
        }
        case AV_PIX_FMT_NV12: {
            OSV_TRY_ASSIGN(out, widen8(*f, true));
            break;
        }
        default:
            return Error{ErrorCode::Unsupported, std::string("pixel format ") + formatName(hostFormat) +
                                                     " is not supported (expected yuv420p10le, p010le, yuv420p or nv12)"};
        }
        out.ptsUs = ptsToMicroseconds(pts);
        out.frameIndex = index;
        if (!out.valid()) {
            return Error{ErrorCode::Decoder, "decoded frame failed validation"};
        }
        return out;
    }

    // -------------------------------------------------------------------------
    //  The frame-accurate decode
    // -------------------------------------------------------------------------
    Result<PlanarFrame16> decode(std::uint32_t index) {
        if (!codecCtx) {
            return Error{ErrorCode::InvalidArgument, "decoder not open"};
        }
        if (index >= frameCount) {
            return Error{ErrorCode::InvalidArgument, "frame " + std::to_string(index) + " out of range (" +
                                                         std::to_string(frameCount) + " frames)"};
        }
        if (!canDecodeForwardTo(index)) {
            const Status seeked = seekTo(index);
            if (!seeked.ok()) {
                invalidatePosition();
                return Error(seeked.error());
            }
        }
        // Decode forward, discarding everything before the target.  Every
        // failure path invalidates the position so the next request seeks
        // afresh instead of trusting a decoder that is somewhere unknown.
        for (;;) {
            auto got = receiveNext();
            if (!got.ok()) {
                invalidatePosition();
                if (got.error().code == ErrorCode::NotFound) {
                    return Error{ErrorCode::Decoder, "stream ended before frame " + std::to_string(index) +
                                                         " (last decoded " + std::to_string(lastDecodedIndex) + ")"};
                }
                return Error(got.error());
            }
            ff::FramePtr frame = std::move(got).value();
            const std::int64_t pts = framePts(*frame);
            const std::int64_t fi = indexFromPts(pts);
            lastDecodedIndex = fi;
            if (fi < static_cast<std::int64_t>(index)) {
                continue;  // still catching up from the sync sample
            }
            if (fi > static_cast<std::int64_t>(index)) {
                invalidatePosition();
                return Error{ErrorCode::Decoder, "presentation time mismatch: wanted frame " + std::to_string(index) +
                                                     " but decoder produced frame " + std::to_string(fi) + " (pts " +
                                                     std::to_string(pts) + ")"};
            }
            auto wrapped = wrapFrame(std::move(frame), pts, index);
            if (!wrapped.ok()) {
                invalidatePosition();
                return Error(wrapped.error());
            }
            lastPts = pts;
            nextIndex = index + 1;
            positionValid = true;
            return std::move(wrapped).value();
        }
    }

    /// Forget where the decoder is.  With positionValid cleared,
    /// canDecodeForwardTo() can never answer "yes" until a decode succeeded
    /// again (which always starts with a seek); nextIndex is left alone so
    /// next() retries the same frame instead of reporting end of stream.
    void invalidatePosition() noexcept {
        positionValid = false;
        lastDecodedIndex = -1;
    }

    // -------------------------------------------------------------------------
    //  Open helpers
    // -------------------------------------------------------------------------

    /// Create the codec context for `codecId` with `extradata` (may be
    /// empty) and the requested threading / hardware configuration.
    Status openCodec(AVCodecID codecId, const AVCodecParameters* params, const std::vector<std::uint8_t>& extradata,
                     int threads) {
        // The whole function counts as codec open, minus the time spent
        // creating the hardware device (booked separately below).
        const auto codecStart = std::chrono::steady_clock::now();
        double deviceMs = 0.0;
        codec = avcodec_find_decoder(codecId);
        if (!codec) {
            return failStatus(ErrorCode::Unsupported, std::string("no decoder for codec ") +
                                                          (avcodec_get_name(codecId) ? avcodec_get_name(codecId) : "?"));
        }
        codecCtx.reset(avcodec_alloc_context3(codec));
        if (!codecCtx) {
            return failStatus(ErrorCode::Decoder, "avcodec_alloc_context3 failed");
        }
        if (params) {
            const int ret = avcodec_parameters_to_context(codecCtx.get(), params);
            if (ret < 0) {
                return failStatus(ErrorCode::Decoder, "avcodec_parameters_to_context: " + ff::errorString(ret));
            }
        }
        if (!extradata.empty()) {
            // Replace whatever the parameters carried with our Annex-B header.
            av_freep(&codecCtx->extradata);
            codecCtx->extradata = static_cast<std::uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!codecCtx->extradata) {
                return failStatus(ErrorCode::Decoder, "cannot allocate extradata");
            }
            std::memcpy(codecCtx->extradata, extradata.data(), extradata.size());
            codecCtx->extradata_size = static_cast<int>(extradata.size());
        }
        codecCtx->thread_count = std::max(0, threads);
        codecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        codecCtx->pkt_timebase = AVRational{timeBase.num, timeBase.den};
        codecCtx->opaque = this;
        codecCtx->get_format = &Impl::getFormat;

        // Hardware: honour the request, or walk the Auto preference list.
        std::vector<HwAccel> attempts;
        if (requestedHw == HwAccel::Auto) {
            attempts = {HwAccel::Cuda, HwAccel::D3D11VA};
        } else if (requestedHw != HwAccel::None) {
            attempts = {requestedHw};
        }
        activeHw = HwAccel::None;
        for (const HwAccel attempt : attempts) {
            const auto deviceStart = std::chrono::steady_clock::now();
            Status st = setupHardware(attempt);
            deviceMs += msSince(deviceStart);
            if (st.ok()) {
                activeHw = attempt;
                codecCtx->hw_device_ctx = av_buffer_ref(hwDevice.get());
                if (!codecCtx->hw_device_ctx) {
                    return failStatus(ErrorCode::Decoder, "cannot reference hardware device");
                }
                log::debug("video: using {} hardware decoding for track {}", hwAccelName(attempt), trackId);
                break;
            }
            if (requestedHw != HwAccel::Auto) {
                return st;  // an explicit request that cannot be met is an error
            }
            log::debug("video: {} unavailable ({}), trying next", hwAccelName(attempt), st.error().message);
            hwDevice.reset();
            sharedDevice.reset();
            timings.hwDeviceReused = false;
            hwPixFmt = AV_PIX_FMT_NONE;
        }
        if (keepOnDevice && activeHw != HwAccel::Cuda) {
            log::warn("video: keepOnDevice requested but the active path is {}; frames will be host copies",
                      hwAccelName(activeHw));
            keepOnDevice = false;
        }
        const int ret = avcodec_open2(codecCtx.get(), codec, nullptr);
        if (ret < 0) {
            return failStatus(ErrorCode::Decoder, std::string("avcodec_open2(") + codec->name + "): " + ff::errorString(ret));
        }
        packet.reset(av_packet_alloc());
        if (!packet) {
            return failStatus(ErrorCode::Decoder, "av_packet_alloc failed");
        }
        // Book the phases: device creation on its own, the rest as codec open.
        timings.hwDeviceMs = deviceMs;
        timings.codecOpenMs = std::max(0.0, msSince(codecStart) - deviceMs);
        return okStatus();
    }

    /// avformat mode: demux through the AVIO callbacks and pick the stream.
    Status openWithAvformat(std::uint32_t wantedTrack) {
        constexpr int kAvioBufferSize = 1 << 16;
        std::uint8_t* buffer = static_cast<std::uint8_t*>(av_malloc(kAvioBufferSize));
        if (!buffer) {
            return failStatus(ErrorCode::Decoder, "cannot allocate AVIO buffer");
        }
        avio = avio_alloc_context(buffer, kAvioBufferSize, 0, this, &Impl::avioRead, nullptr, &Impl::avioSeek);
        if (!avio) {
            av_free(buffer);
            return failStatus(ErrorCode::Decoder, "avio_alloc_context failed");
        }
        avioPos = 0;

        AVFormatContext* raw = avformat_alloc_context();
        if (!raw) {
            return failStatus(ErrorCode::Decoder, "avformat_alloc_context failed");
        }
        raw->pb = avio;
        raw->flags |= AVFMT_FLAG_CUSTOM_IO;
        // avformat_open_input frees `raw` on failure, so it is only adopted
        // by the unique_ptr once it succeeded.
        const std::string name = path.filename().string();
        const auto demuxStart = std::chrono::steady_clock::now();
        int ret = avformat_open_input(&raw, name.c_str(), nullptr, nullptr);
        timings.demuxOpenMs = msSince(demuxStart);
        if (ret < 0) {
            return failStatus(ErrorCode::Malformed, "avformat_open_input: " + ff::errorString(ret));
        }
        fmt.reset(raw);
        // The probe: libavformat reads packets of every stream (all seven on
        // an OSV) until it can describe each one.  It is what prints "not
        // enough frames to estimate rate" for the metadata streams, but it
        // measured only ~3 ms on the sample clip - the hardware device and
        // the first-frame decode were the expensive parts of an open.
        const auto infoStart = std::chrono::steady_clock::now();
        ret = avformat_find_stream_info(fmt.get(), nullptr);
        timings.streamInfoMs = msSince(infoStart);
        if (ret < 0) {
            return failStatus(ErrorCode::Malformed, "avformat_find_stream_info: " + ff::errorString(ret));
        }

        // Select by container track id first, then by ordinal among the
        // video streams; cover art (attached pictures) never counts.
        int byId = -1;
        int byOrdinal = -1;
        unsigned videoOrdinal = 0;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVStream* st = fmt->streams[i];
            if (!st || !st->codecpar || st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
                continue;
            }
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                continue;
            }
            ++videoOrdinal;
            if (st->id == static_cast<int>(wantedTrack) && byId < 0) {
                byId = static_cast<int>(i);
            }
            if (videoOrdinal == wantedTrack && byOrdinal < 0) {
                byOrdinal = static_cast<int>(i);
            }
        }
        streamIndex = byId >= 0 ? byId : byOrdinal;
        if (streamIndex < 0) {
            return failStatus(ErrorCode::NotFound, "no video stream with track id " + std::to_string(wantedTrack) +
                                                       " (" + std::to_string(videoOrdinal) + " video streams)");
        }
        if (byId < 0) {
            log::debug("video: no stream carries track id {}, using video stream #{}", wantedTrack, wantedTrack);
        }
        stream = fmt->streams[streamIndex];
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            if (static_cast<int>(i) != streamIndex && fmt->streams[i]) {
                fmt->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        timeBase = TimeBase{stream->time_base.num, stream->time_base.den};
        if (timeBase.num <= 0 || timeBase.den <= 0) {
            return failStatus(ErrorCode::Malformed, "stream has an invalid time base");
        }
        startPts = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;

        // Frame rate: our sample table when it parsed, else libavformat's.
        if (container && container->fps() > 0.0) {
            fps = container->fps();
        } else if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
            fps = av_q2d(stream->avg_frame_rate);
        } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
            fps = av_q2d(stream->r_frame_rate);
        }
        if (fps <= 0.0) {
            return failStatus(ErrorCode::Malformed, "cannot determine the frame rate");
        }
        // Frame count: sample table, then nb_frames, then duration * fps.
        if (container && container->sampleCount() > 0) {
            frameCount = container->sampleCount();
        } else if (stream->nb_frames > 0) {
            frameCount = static_cast<std::uint32_t>(stream->nb_frames);
        } else if (stream->duration > 0) {
            frameCount = static_cast<std::uint32_t>(std::llround(static_cast<double>(stream->duration) / ticksPerSecond() * fps));
        }
        if (frameCount == 0) {
            return failStatus(ErrorCode::Malformed, "cannot determine the frame count");
        }
        width = static_cast<std::uint32_t>(std::max(0, stream->codecpar->width));
        height = static_cast<std::uint32_t>(std::max(0, stream->codecpar->height));
        sourceBitDepth = formatBitDepth(static_cast<AVPixelFormat>(stream->codecpar->format));
        return openCodec(stream->codecpar->codec_id, stream->codecpar, {}, /*threads*/ threadsRequested);
    }

    /// samples mode: everything comes from the OpenOSV container parser.
    Status openWithSamples() {
        if (!container) {
            return failStatus(ErrorCode::Internal, "container source missing");
        }
        // The codec comes from the sample entry's configuration record: hvcC
        // for the native streams, avcC for the LRF proxy.  Without either we
        // have no parameter sets to prime the decoder with.
        AVCodecID codecId = AV_CODEC_ID_NONE;
        switch (container->codec()) {
        case detail::TrackCodec::Hevc: codecId = AV_CODEC_ID_HEVC; break;
        case detail::TrackCodec::Avc: codecId = AV_CODEC_ID_H264; break;
        case detail::TrackCodec::Unknown:
        default:
            return failStatus(ErrorCode::Unsupported, "useContainerSamples needs an hvcC or avcC track; track " +
                                                          std::to_string(trackId) + " has neither");
        }
        const std::vector<std::uint8_t> header = container->annexBHeader();
        if (header.empty() || container->nalLengthSize() == 0) {
            return failStatus(ErrorCode::Malformed, "track " + std::to_string(trackId) +
                                                        " carries no parameter sets in its sample entry");
        }
        if (container->timescale() == 0 || container->fps() <= 0.0) {
            return failStatus(ErrorCode::Malformed, "track " + std::to_string(trackId) + " has no usable timing");
        }
        if (container->timescale() > static_cast<std::uint32_t>(INT32_MAX)) {
            return failStatus(ErrorCode::Malformed, "track " + std::to_string(trackId) + " timescale is out of range");
        }
        timeBase = TimeBase{1, static_cast<std::int32_t>(container->timescale())};
        fps = container->fps();
        frameCount = container->sampleCount();
        // Coded size / depth from the sample entry; probeFirstFrame() replaces
        // them with the cropped values the decoder actually produces.
        width = container->codedWidth();
        height = container->codedHeight();
        sourceBitDepth = container->bitDepth();
        // Presentation times start at the first sample's pts.
        OSV_TRY_ASSIGN(const detail::SampleView first, container->sample(0));
        startPts = first.dts + first.ctsOffset;
        sampleCursor = 0;
        return openCodec(codecId, nullptr, header, threadsRequested);
    }

    /// DecoderOptions::deferFirstFrame: take the geometry and bit depth from
    /// the codec context instead of decoding frame 0.  libavcodec's HEVC
    /// decoder parses the parameter sets in the extradata during
    /// avcodec_open2 and exports the CROPPED size and the software pixel
    /// format from the first SPS - exactly what the probe would have read
    /// off the decoded picture.  Returns false (and changes nothing) when
    /// any of it is missing, so the caller falls back to the real probe.
    [[nodiscard]] bool adoptCodecParameters() noexcept {
        if (!codecCtx) {
            return false;
        }
        const int w = codecCtx->width;
        const int h = codecCtx->height;
        // The software format is what the frames carry before any hardware
        // mapping; with a hardware decoder it is still the stream's own
        // format at this point (get_format has not run yet).
        const AVPixelFormat sw = (codecCtx->sw_pix_fmt != AV_PIX_FMT_NONE) ? codecCtx->sw_pix_fmt : codecCtx->pix_fmt;
        if (w <= 0 || h <= 0 || sw == AV_PIX_FMT_NONE || isHwFormat(sw)) {
            return false;
        }
        const std::uint8_t depth = formatBitDepth(sw);
        if (depth == 0) {
            return false;
        }
        width = static_cast<std::uint32_t>(w);
        height = static_cast<std::uint32_t>(h);
        sourceBitDepth = depth;
        // Nothing was decoded: the first request seeks to its sync sample
        // exactly as it would after a probe.
        nextIndex = 0;
        positionValid = false;
        lastDecodedIndex = -1;
        return true;
    }

    /// Decode frame 0 once so dimensions / bit depth / the real hardware
    /// path are known before open() returns (and a broken stream fails
    /// early instead of on the first render).
    Status probeFirstFrame() {
        auto probe = decode(0);
        if (!probe.ok()) {
            return Error(probe.error());
        }
        const PlanarFrame16& f = probe.value();
        if (f.width == 0 || f.height == 0) {
            return failStatus(ErrorCode::Decoder, "first frame has no dimensions");
        }
        width = f.width;
        height = f.height;
        // Source depth: what the codec produces before we widen it.
        if (codecCtx) {
            const AVPixelFormat sw = (codecCtx->sw_pix_fmt != AV_PIX_FMT_NONE) ? codecCtx->sw_pix_fmt : codecCtx->pix_fmt;
            const std::uint8_t depth = formatBitDepth(sw);
            if (depth > 0) {
                sourceBitDepth = depth;
            }
        }
        if (sourceBitDepth == 0) {
            sourceBitDepth = f.bitDepth;
        }
        // Do not keep a device frame from the probe alive.
        lastDevice.reset();
        // The probe consumed frame 0; make next() start from the beginning
        // again (the position stays valid so no seek is needed if the caller
        // asks for frame 1 first).
        nextIndex = 0;
        positionValid = false;
        return okStatus();
    }

    int threadsRequested = 0;
};

// =============================================================================
//  HevcStreamDecoder
// =============================================================================
HevcStreamDecoder::HevcStreamDecoder() = default;
HevcStreamDecoder::~HevcStreamDecoder() = default;
HevcStreamDecoder::HevcStreamDecoder(HevcStreamDecoder&& other) noexcept = default;
HevcStreamDecoder& HevcStreamDecoder::operator=(HevcStreamDecoder&& other) noexcept = default;

Result<HevcStreamDecoder> HevcStreamDecoder::open(const std::filesystem::path& path, std::uint32_t trackId,
                                                  const DecoderOptions& options) {
    ensureFfmpegLogging();
    if (path.empty()) {
        return Error{ErrorCode::InvalidArgument, "empty path"};
    }
    if (trackId == 0) {
        return Error{ErrorCode::InvalidArgument, "track ids are 1-based"};
    }
    HevcStreamDecoder decoder;
    decoder.m_impl = std::make_unique<Impl>();
    Impl& impl = *decoder.m_impl;
    impl.path = path;
    impl.trackId = trackId;
    impl.requestedHw = options.hw;
    impl.keepOnDevice = options.keepOnDevice;
    impl.cudaDeviceIndex = options.cudaDeviceIndex;
    impl.cudaContext = options.cudaContext;
    impl.cudaStream = options.cudaStream;
    impl.threadsRequested = options.threads;
    impl.samplesMode = options.useContainerSamples;
    impl.shareHwDevice = options.shareHwDevice;
    impl.hwDeviceSlot = std::max(0, options.hwDeviceSlot);
    impl.deferFirstFrame = options.deferFirstFrame;
    const auto openStart = std::chrono::steady_clock::now();

    // The mapping is what libavformat reads from; a missing file fails here
    // with Io before FFmpeg is involved at all.  Container-sample mode reads
    // every byte through the container parser's own mapping, so a second one
    // would only cost a handle - but the missing-file check must still give
    // Io, which the parser alone would not guarantee.
    const auto mapStart = std::chrono::steady_clock::now();
    if (impl.samplesMode) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            return Error{ErrorCode::Io, "file not found: " + path.string()};
        }
    } else {
        OSV_TRY_ASSIGN(impl.mapping, MappedFile::open(path));
    }
    impl.timings.mapMs = msSince(mapStart);

    // Our container parser: mandatory in samples mode, best effort otherwise
    // (it provides the sync-sample table and the exact stts frame rate).
    const auto containerStart = std::chrono::steady_clock::now();
    auto source = detail::ContainerSource::open(path, trackId);
    impl.timings.containerMs = msSince(containerStart);
    if (source.ok()) {
        impl.container = std::move(source).value();
        impl.timings.containerReused = impl.container && impl.container->reusedParse();
    } else if (options.useContainerSamples) {
        return Error(source.error());
    } else {
        log::debug("video: container parser declined {} ({}); relying on libavformat only",
                   log::safe(path.filename().string()), source.error().message);
    }

    if (impl.samplesMode) {
        OSV_TRY(impl.openWithSamples());
    } else {
        OSV_TRY(impl.openWithAvformat(trackId));
    }
    // Frame 0 is decoded here only when the parameter sets did not already
    // tell us what it will look like (or the caller wants the early check).
    const auto probeStart = std::chrono::steady_clock::now();
    if (impl.deferFirstFrame && impl.adoptCodecParameters()) {
        impl.timings.firstFrameDeferred = true;
    } else {
        OSV_TRY(impl.probeFirstFrame());
    }
    impl.timings.firstFrameMs = msSince(probeStart);
    impl.timings.totalMs = msSince(openStart);
    const DecoderOpenTimings& t = impl.timings;
    log::debug("video: opened track {} of {}: {}x{} {} fps, {} frames, {}-bit source, codec {}, hw {}", trackId,
               log::safe(path.filename().string()), impl.width, impl.height, impl.fps, impl.frameCount,
               impl.sourceBitDepth, impl.codec ? impl.codec->name : "?", hwAccelName(impl.activeHw));
    log::debug("video: track {} open took {:.1f} ms (map {:.1f}, container {:.1f}{}, demux {:.1f}, probe {:.1f}, "
               "device {:.1f}{}, codec {:.1f}, first frame {})",
               trackId, t.totalMs, t.mapMs, t.containerMs, t.containerReused ? " reused" : "", t.demuxOpenMs,
               t.streamInfoMs, t.hwDeviceMs, t.hwDeviceReused ? " shared" : "", t.codecOpenMs,
               t.firstFrameDeferred ? std::string("deferred") : std::to_string(t.firstFrameMs));
    return decoder;
}

bool HevcStreamDecoder::isOpen() const noexcept { return m_impl != nullptr && m_impl->codecCtx != nullptr; }

std::uint32_t HevcStreamDecoder::frameCount() const noexcept { return m_impl ? m_impl->frameCount : 0; }

double HevcStreamDecoder::fps() const noexcept { return m_impl ? m_impl->fps : 0.0; }

std::uint32_t HevcStreamDecoder::width() const noexcept { return m_impl ? m_impl->width : 0; }

std::uint32_t HevcStreamDecoder::height() const noexcept { return m_impl ? m_impl->height : 0; }

std::uint8_t HevcStreamDecoder::sourceBitDepth() const noexcept { return m_impl ? m_impl->sourceBitDepth : 0; }

HwAccel HevcStreamDecoder::activeHw() const noexcept { return m_impl ? m_impl->activeHw : HwAccel::None; }

std::string HevcStreamDecoder::codecName() const {
    if (!m_impl || !m_impl->codec || !m_impl->codec->name) {
        return {};
    }
    return m_impl->codec->name;
}

std::uint32_t HevcStreamDecoder::trackId() const noexcept { return m_impl ? m_impl->trackId : 0; }

bool HevcStreamDecoder::usesContainerSamples() const noexcept { return m_impl != nullptr && m_impl->samplesMode; }

TimeBase HevcStreamDecoder::timeBase() const noexcept { return m_impl ? m_impl->timeBase : TimeBase{}; }

DecoderOpenTimings HevcStreamDecoder::openTimings() const noexcept {
    return m_impl ? m_impl->timings : DecoderOpenTimings{};
}

std::uint32_t HevcStreamDecoder::nextIndex() const noexcept { return m_impl ? m_impl->nextIndex : 0; }

Result<PlanarFrame16> HevcStreamDecoder::decodeFrame(std::uint32_t index) {
    if (!isOpen()) {
        return Error{ErrorCode::InvalidArgument, "decoder not open"};
    }
    return m_impl->decode(index);
}

Result<PlanarFrame16> HevcStreamDecoder::next() {
    if (!isOpen()) {
        return Error{ErrorCode::InvalidArgument, "decoder not open"};
    }
    if (m_impl->nextIndex >= m_impl->frameCount) {
        return Error{ErrorCode::NotFound, "end of stream"};
    }
    return m_impl->decode(m_impl->nextIndex);
}

Status HevcStreamDecoder::seek(std::uint32_t index) {
    if (!isOpen()) {
        return failStatus(ErrorCode::InvalidArgument, "decoder not open");
    }
    if (index >= m_impl->frameCount) {
        return failStatus(ErrorCode::InvalidArgument, "frame " + std::to_string(index) + " out of range (" +
                                                          std::to_string(m_impl->frameCount) + " frames)");
    }
    // Lazy: decode() decides whether a forward skip suffices or a real seek
    // to the previous sync sample is needed.
    m_impl->nextIndex = index;
    return okStatus();
}

std::optional<std::int64_t> HevcStreamDecoder::lastPts() const noexcept {
    return m_impl ? m_impl->lastPts : std::nullopt;
}

std::optional<DeviceFrameRef> HevcStreamDecoder::lastDeviceFrame() const {
    return m_impl ? m_impl->lastDevice : std::nullopt;
}

std::optional<std::uint32_t> HevcStreamDecoder::previousSyncIndex(std::uint32_t index) const noexcept {
    // Only our own sample table knows the GOP layout; libavformat's index
    // is not consulted here because it is not guaranteed to be complete.
    if (!m_impl || !m_impl->container) {
        return std::nullopt;
    }
    return m_impl->container->previousSync(index);
}

// -----------------------------------------------------------------------------
//  Static diagnostics
// -----------------------------------------------------------------------------
std::string HevcStreamDecoder::ffmpegVersion() {
    const unsigned c = avcodec_version();
    const unsigned f = avformat_version();
    const unsigned u = avutil_version();
    auto fmtVersion = [](unsigned v) {
        return std::to_string(AV_VERSION_MAJOR(v)) + "." + std::to_string(AV_VERSION_MINOR(v)) + "." +
               std::to_string(AV_VERSION_MICRO(v));
    };
    return "avcodec " + fmtVersion(c) + " / avformat " + fmtVersion(f) + " / avutil " + fmtVersion(u);
}

std::string HevcStreamDecoder::ffmpegConfiguration() {
    const char* cfg = avcodec_configuration();
    return cfg ? log::safe(cfg) : std::string();
}

bool HevcStreamDecoder::ffmpegIsGpl() {
    const std::string cfg = ffmpegConfiguration();
    return cfg.find("--enable-gpl") != std::string::npos || cfg.find("--enable-nonfree") != std::string::npos;
}

std::vector<std::string> HevcStreamDecoder::availableHwAccels() {
    std::vector<std::string> names{"none"};
    for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); t != AV_HWDEVICE_TYPE_NONE;
         t = av_hwdevice_iterate_types(t)) {
        if (t == AV_HWDEVICE_TYPE_D3D11VA) {
            names.emplace_back(hwAccelName(HwAccel::D3D11VA));
        } else if (t == AV_HWDEVICE_TYPE_CUDA) {
            names.emplace_back(hwAccelName(HwAccel::Cuda));
        }
    }
    return names;
}

}  // namespace osv::video
