// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Internal: the one place that includes the FFmpeg C headers.  Everything
// else in the video module includes this file so the extern "C" wrapping,
// the warning suppression and the small RAII helpers live in a single spot.
// Never include this from a public header.
#pragma once

#if defined(_MSC_VER)
#pragma warning(push)
// C4244: conversion from 'int64_t' to 'int' inside FFmpeg's inline helpers.
// C4819: the FFmpeg headers contain non-ASCII comments (author names).
#pragma warning(disable : 4244 4819)
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/version.h>
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <memory>
#include <string>

namespace osv::video::ff {

/// Human readable text for an FFmpeg error code (never empty).
[[nodiscard]] inline std::string errorString(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, buf, sizeof(buf)) < 0) {
        return "ffmpeg error " + std::to_string(code);
    }
    return std::string(buf);
}

/// Deleters so FFmpeg objects can sit in std::unique_ptr / std::shared_ptr.
struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept {
        if (f) {
            av_frame_free(&f);
        }
    }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept {
        if (p) {
            av_packet_free(&p);
        }
    }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* c) const noexcept {
        if (c) {
            avcodec_free_context(&c);
        }
    }
};
struct FormatContextDeleter {
    void operator()(AVFormatContext* f) const noexcept {
        if (f) {
            avformat_close_input(&f);
        }
    }
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* b) const noexcept {
        if (b) {
            av_buffer_unref(&b);
        }
    }
};

using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

/// Shared ownership of an AVFrame; the frame is freed (and its buffers
/// unreferenced) when the last PlanarFrame16 / DeviceFrameRef lets go.
[[nodiscard]] inline std::shared_ptr<AVFrame> shareFrame(FramePtr frame) {
    return std::shared_ptr<AVFrame>(frame.release(), FrameDeleter{});
}

}  // namespace osv::video::ff
