// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/io/ImageWriter.h"
#include "osv/core/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

#include <tinyexr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

namespace osv::io {

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// Quantise a float to an unsigned integer code of `maxCode`.
inline std::uint32_t quantise(float v, std::uint32_t maxCode) noexcept {
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return maxCode;
    }
    return static_cast<std::uint32_t>(v * static_cast<float>(maxCode) + 0.5f);
}

std::string lowerExt(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// RAII wrappers for the few FFmpeg objects we touch.
struct CodecContextDeleter {
    void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};

/// Encode one frame with a still-image codec and write the packet to disk.
Status encodeStill(const std::filesystem::path& path, const char* codecName, AVPixelFormat pixFmt,
                   const render::ImageRGBAf& image, const std::function<void(AVFrame&)>& fill) {
    const AVCodec* codec = avcodec_find_encoder_by_name(codecName);
    if (!codec) {
        return failStatus(ErrorCode::Unsupported, std::string("libavcodec has no encoder named ") + codecName);
    }
    std::unique_ptr<AVCodecContext, CodecContextDeleter> ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        return failStatus(ErrorCode::Internal, "avcodec_alloc_context3 failed");
    }
    ctx->width = static_cast<int>(image.w);
    ctx->height = static_cast<int>(image.h);
    ctx->pix_fmt = pixFmt;
    ctx->time_base = AVRational{1, 25};
    // PNG: default compression is fine; TIFF: uncompressed keeps it simple and fast.
    if (std::strcmp(codecName, "png") == 0) {
        ctx->compression_level = 6;
    }
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        return failStatus(ErrorCode::Internal, std::string("avcodec_open2 failed for ") + codecName);
    }

    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!frame) {
        return failStatus(ErrorCode::Internal, "av_frame_alloc failed");
    }
    frame->format = pixFmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        return failStatus(ErrorCode::Internal, "av_frame_get_buffer failed");
    }
    if (av_frame_make_writable(frame.get()) < 0) {
        return failStatus(ErrorCode::Internal, "av_frame_make_writable failed");
    }
    fill(*frame);

    if (avcodec_send_frame(ctx.get(), frame.get()) < 0) {
        return failStatus(ErrorCode::Internal, "avcodec_send_frame failed");
    }
    // Flush so the encoder emits the (single) packet.
    avcodec_send_frame(ctx.get(), nullptr);

    std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
    if (!pkt) {
        return failStatus(ErrorCode::Internal, "av_packet_alloc failed");
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return failStatus(ErrorCode::Io, "cannot create " + path.string());
    }
    bool wrote = false;
    for (;;) {
        const int r = avcodec_receive_packet(ctx.get(), pkt.get());
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            break;
        }
        if (r < 0) {
            return failStatus(ErrorCode::Internal, "avcodec_receive_packet failed");
        }
        out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
        av_packet_unref(pkt.get());
        wrote = true;
    }
    if (!wrote || !out) {
        return failStatus(ErrorCode::Io, "encoder produced no data for " + path.string());
    }
    return okStatus();
}

/// Fill a 16-bit RGB(A) frame.  `bigEndian` selects the byte order libavcodec
/// expects for the chosen pixel format.
void fill16(AVFrame& frame, const render::ImageRGBAf& image, int channels, bool bigEndian) {
    for (std::uint32_t y = 0; y < image.h; ++y) {
        const float* src = image.row(y);
        std::uint8_t* dst = frame.data[0] + static_cast<std::size_t>(y) * frame.linesize[0];
        for (std::uint32_t x = 0; x < image.w; ++x) {
            for (int c = 0; c < channels; ++c) {
                const std::uint32_t q = quantise(src[x * 4 + c], 65535u);
                const std::size_t o = (static_cast<std::size_t>(x) * channels + c) * 2;
                if (bigEndian) {
                    dst[o] = static_cast<std::uint8_t>(q >> 8);
                    dst[o + 1] = static_cast<std::uint8_t>(q & 0xFF);
                } else {
                    dst[o] = static_cast<std::uint8_t>(q & 0xFF);
                    dst[o + 1] = static_cast<std::uint8_t>(q >> 8);
                }
            }
        }
    }
}

void fill8(AVFrame& frame, const render::ImageRGBAf& image, int channels) {
    for (std::uint32_t y = 0; y < image.h; ++y) {
        const float* src = image.row(y);
        std::uint8_t* dst = frame.data[0] + static_cast<std::size_t>(y) * frame.linesize[0];
        for (std::uint32_t x = 0; x < image.w; ++x) {
            for (int c = 0; c < channels; ++c) {
                dst[static_cast<std::size_t>(x) * channels + c] = static_cast<std::uint8_t>(quantise(src[x * 4 + c], 255u));
            }
        }
    }
}

Status writeSidecar(const std::filesystem::path& imagePath, const ImageTag& tag, ImageFormat format) {
    std::filesystem::path side = imagePath;
    side += ".json";
    std::ofstream out(side, std::ios::trunc);
    if (!out) {
        return failStatus(ErrorCode::Io, "cannot create " + side.string());
    }
    const char* fmt = format == ImageFormat::Exr ? "exr" : (format == ImageFormat::Tiff16 ? "tiff16" : (format == ImageFormat::Png8 ? "png8" : "png16"));
    out << "{\n"
        << "  \"generator\": \"OpenOSV\",\n"
        << "  \"format\": \"" << fmt << "\",\n"
        << "  \"transfer\": \"" << imageTransferName(tag.transfer) << "\",\n"
        << "  \"primaries\": \"" << (tag.rec2020 ? "bt2020" : "bt709") << "\",\n"
        << "  \"peak_nits\": " << tag.peakNits << ",\n"
        << "  \"alpha\": " << (tag.includeAlpha ? "true" : "false") << "\n"
        << "}\n";
    return out ? okStatus() : failStatus(ErrorCode::Io, "short write " + side.string());
}

Status writeExrImpl(const std::filesystem::path& path, const render::ImageRGBAf& image, const ImageTag& tag) {
    const int channels = tag.includeAlpha ? 4 : 3;
    // tinyexr wants planar channels; it writes them in alphabetical order so
    // we hand them over as B, G, R (, A) which is the conventional layout.
    std::vector<float> planes[4];
    const std::size_t n = static_cast<std::size_t>(image.w) * image.h;
    for (int c = 0; c < channels; ++c) {
        planes[c].resize(n);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float* px = image.data.data() + i * 4;
        planes[0][i] = px[2];  // B
        planes[1][i] = px[1];  // G
        planes[2][i] = px[0];  // R
        if (channels == 4) {
            planes[3][i] = px[3];
        }
    }
    float* imagePtrs[4] = {planes[0].data(), planes[1].data(), planes[2].data(), channels == 4 ? planes[3].data() : nullptr};
    // Channel order must match the names below (alphabetical: A, B, G, R).
    std::vector<float*> ordered;
    std::vector<std::string> names;
    if (channels == 4) {
        ordered = {imagePtrs[3], imagePtrs[0], imagePtrs[1], imagePtrs[2]};
        names = {"A", "B", "G", "R"};
    } else {
        ordered = {imagePtrs[0], imagePtrs[1], imagePtrs[2]};
        names = {"B", "G", "R"};
    }

    EXRHeader header;
    InitEXRHeader(&header);
    EXRImage exr;
    InitEXRImage(&exr);
    exr.num_channels = channels;
    exr.images = reinterpret_cast<unsigned char**>(ordered.data());
    exr.width = static_cast<int>(image.w);
    exr.height = static_cast<int>(image.h);

    std::vector<EXRChannelInfo> channelInfo(static_cast<std::size_t>(channels));
    std::vector<int> pixelTypes(static_cast<std::size_t>(channels), TINYEXR_PIXELTYPE_FLOAT);
    std::vector<int> requestedTypes(static_cast<std::size_t>(channels), TINYEXR_PIXELTYPE_FLOAT);
    for (int c = 0; c < channels; ++c) {
        std::memset(channelInfo[static_cast<std::size_t>(c)].name, 0, sizeof(channelInfo[0].name));
        std::strncpy(channelInfo[static_cast<std::size_t>(c)].name, names[static_cast<std::size_t>(c)].c_str(), 254);
    }
    header.num_channels = channels;
    header.channels = channelInfo.data();
    header.pixel_types = pixelTypes.data();
    header.requested_pixel_types = requestedTypes.data();
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

    // Chromaticities attribute (8 floats: rx ry gx gy bx by wx wy).
    float chroma[8];
    if (tag.rec2020) {
        const float v[8] = {0.708f, 0.292f, 0.170f, 0.797f, 0.131f, 0.046f, 0.3127f, 0.3290f};
        std::memcpy(chroma, v, sizeof(chroma));
    } else {
        const float v[8] = {0.640f, 0.330f, 0.300f, 0.600f, 0.150f, 0.060f, 0.3127f, 0.3290f};
        std::memcpy(chroma, v, sizeof(chroma));
    }
    EXRAttribute attr;
    std::memset(&attr, 0, sizeof(attr));
    std::strncpy(attr.name, "chromaticities", sizeof(attr.name) - 1);
    std::strncpy(attr.type, "chromaticities", sizeof(attr.type) - 1);
    attr.value = reinterpret_cast<unsigned char*>(chroma);
    attr.size = static_cast<int>(sizeof(chroma));
    header.num_custom_attributes = 1;
    header.custom_attributes = &attr;

    const char* err = nullptr;
    const int ret = SaveEXRImageToFile(&exr, &header, path.string().c_str(), &err);
    if (ret != TINYEXR_SUCCESS) {
        std::string message = err ? err : "unknown tinyexr error";
        if (err) {
            FreeEXRErrorMessage(err);
        }
        return failStatus(ErrorCode::Io, "EXR write failed: " + message);
    }
    return okStatus();
}

}  // namespace

// -----------------------------------------------------------------------------
//  Public API
// -----------------------------------------------------------------------------
ImageFormat formatFromExtension(const std::filesystem::path& path) {
    const std::string ext = lowerExt(path);
    if (ext == ".exr") {
        return ImageFormat::Exr;
    }
    if (ext == ".tif" || ext == ".tiff") {
        return ImageFormat::Tiff16;
    }
    return ImageFormat::Png16;
}

const char* imageTransferName(ImageTransfer t) noexcept {
    switch (t) {
    case ImageTransfer::HLG: return "hlg";
    case ImageTransfer::PQ: return "pq";
    case ImageTransfer::Rec709: return "rec709";
    case ImageTransfer::Linear: return "linear";
    case ImageTransfer::DLogM: return "dlogm";
    }
    return "unknown";
}

Status writeImage(const std::filesystem::path& path, const render::ImageRGBAf& image, ImageFormat format,
                  const ImageTag& tag) {
    if (!image.valid()) {
        return failStatus(ErrorCode::InvalidArgument, "writeImage: invalid image");
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const int channels = tag.includeAlpha ? 4 : 3;
    Status st = okStatus();
    switch (format) {
    case ImageFormat::Png16:
        st = encodeStill(path, "png", channels == 4 ? AV_PIX_FMT_RGBA64BE : AV_PIX_FMT_RGB48BE, image,
                         [&](AVFrame& f) { fill16(f, image, channels, true); });
        break;
    case ImageFormat::Png8:
        st = encodeStill(path, "png", channels == 4 ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24, image,
                         [&](AVFrame& f) { fill8(f, image, channels); });
        break;
    case ImageFormat::Tiff16:
        st = encodeStill(path, "tiff", channels == 4 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB48LE, image,
                         [&](AVFrame& f) { fill16(f, image, channels, false); });
        break;
    case ImageFormat::Exr:
        st = writeExrImpl(path, image, tag);
        break;
    }
    OSV_TRY(st);
    if (tag.writeSidecar && format != ImageFormat::Exr) {
        OSV_TRY(writeSidecar(path, tag, format));
    }
    return okStatus();
}

Result<render::ImageRGBAf> readImage(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return Error{ErrorCode::Io, "cannot open " + path.string()};
    }
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        return Error{ErrorCode::Io, "empty file " + path.string()};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(bytes.data()), size);

    const std::string ext = lowerExt(path);
    const char* codecName = (ext == ".tif" || ext == ".tiff") ? "tiff" : "png";
    const AVCodec* codec = avcodec_find_decoder_by_name(codecName);
    if (!codec) {
        return Error{ErrorCode::Unsupported, std::string("no decoder ") + codecName};
    }
    std::unique_ptr<AVCodecContext, CodecContextDeleter> ctx(avcodec_alloc_context3(codec));
    if (!ctx || avcodec_open2(ctx.get(), codec, nullptr) < 0) {
        return Error{ErrorCode::Internal, "cannot open decoder"};
    }
    std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
    pkt->data = bytes.data();
    pkt->size = static_cast<int>(size);
    if (avcodec_send_packet(ctx.get(), pkt.get()) < 0) {
        return Error{ErrorCode::Malformed, "decoder rejected " + path.string()};
    }
    avcodec_send_packet(ctx.get(), nullptr);
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (avcodec_receive_frame(ctx.get(), frame.get()) < 0) {
        return Error{ErrorCode::Malformed, "no frame decoded from " + path.string()};
    }

    OSV_TRY_ASSIGN(render::ImageRGBAf image, render::ImageRGBAf::create(static_cast<std::uint32_t>(frame->width),
                                                                        static_cast<std::uint32_t>(frame->height)));
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);
    int channels = 0;
    int bytesPer = 0;
    bool bigEndian = false;
    switch (fmt) {
    case AV_PIX_FMT_RGB24: channels = 3; bytesPer = 1; break;
    case AV_PIX_FMT_RGBA: channels = 4; bytesPer = 1; break;
    case AV_PIX_FMT_RGB48BE: channels = 3; bytesPer = 2; bigEndian = true; break;
    case AV_PIX_FMT_RGB48LE: channels = 3; bytesPer = 2; break;
    case AV_PIX_FMT_RGBA64BE: channels = 4; bytesPer = 2; bigEndian = true; break;
    case AV_PIX_FMT_RGBA64LE: channels = 4; bytesPer = 2; break;
    default:
        return Error{ErrorCode::Unsupported, "unsupported pixel format in " + path.string()};
    }
    const float scale = bytesPer == 2 ? 1.0f / 65535.0f : 1.0f / 255.0f;
    for (std::uint32_t y = 0; y < image.h; ++y) {
        const std::uint8_t* src = frame->data[0] + static_cast<std::size_t>(y) * frame->linesize[0];
        float* dst = image.row(y);
        for (std::uint32_t x = 0; x < image.w; ++x) {
            for (int c = 0; c < 4; ++c) {
                float v = 1.0f;
                if (c < channels) {
                    const std::uint8_t* p = src + (static_cast<std::size_t>(x) * channels + c) * bytesPer;
                    std::uint32_t q = bytesPer == 2 ? (bigEndian ? (static_cast<std::uint32_t>(p[0]) << 8) | p[1]
                                                                 : (static_cast<std::uint32_t>(p[1]) << 8) | p[0])
                                                    : p[0];
                    v = static_cast<float>(q) * scale;
                }
                dst[x * 4 + c] = v;
            }
        }
    }
    return image;
}

Result<render::ImageRGBAf> readExr(const std::filesystem::path& path) {
    float* rgba = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    const int ret = LoadEXR(&rgba, &w, &h, path.string().c_str(), &err);
    if (ret != TINYEXR_SUCCESS) {
        std::string message = err ? err : "unknown tinyexr error";
        if (err) {
            FreeEXRErrorMessage(err);
        }
        return Error{ErrorCode::Io, "EXR read failed: " + message};
    }
    auto img = render::ImageRGBAf::create(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    if (img.ok()) {
        std::memcpy(img.value().data.data(), rgba, static_cast<std::size_t>(w) * h * 4 * sizeof(float));
    }
    std::free(rgba);
    return img;
}

}  // namespace osv::io
