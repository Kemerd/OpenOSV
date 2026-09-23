// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HevcStreamDecoder: frame-accurate decoding of one video track of an .OSV
// (or .LRF / plain MP4) file through libavcodec.
//
// Design notes
//   * FFmpeg is linked dynamically (LGPL) and never leaks into this header;
//     the implementation is behind a pimpl so host applications compile
//     without the FFmpeg SDK.
//   * Two demuxing modes.  By default libavformat parses the container
//     through an AVIOContext that reads from our memory mapping (so the same
//     code path serves files and in-memory buffers).  With
//     DecoderOptions::useContainerSamples the OpenOSV container parser
//     supplies the samples directly and libavformat is not involved at all,
//     which makes "frame index == sample index == djmd index" hold by
//     construction.
//   * Frame accuracy.  decodeFrame(i) only returns the frame whose
//     presentation time rounds to index i.  A request outside the currently
//     decoded GOP seeks to the previous sync sample (from our SampleTable when
//     the container parsed, otherwise through FFmpeg's index) and decodes
//     forward, discarding frames until the timestamp matches.
//   * Output.  PlanarFrame16 aliases the decoder's planes whenever the pixel
//     format allows it (yuv420p10le, P010) and widens 8-bit sources
//     (yuv420p / NV12 from the .LRF proxy) into an owned uint16 buffer.  The
//     widened samples are stored as value << 2 so they sit on the same
//     10-bit scale (narrow range 64..940) as the native streams; the frame
//     reports bitDepth 10 and the decoder reports sourceBitDepth() == 8.
//   * Open cost.  On the sample clip an open was a hardware device (~120 ms
//     D3D11, ~50-85 ms CUDA) plus a probe decode of frame 0 (~100-140 ms);
//     the libavformat probe was ~3 ms.  Hardware devices are therefore
//     shared process-wide (DecoderOptions::shareHwDevice) and the probe can
//     be skipped when the parameter sets describe the stream
//     (DecoderOptions::deferFirstFrame).  openTimings() records the phases.
#pragma once

#include "osv/core/Result.h"
#include "osv/video/HwAccel.h"
#include "osv/video/PlanarFrame.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osv::video {

/// Rational time base (seconds per tick = num / den).
struct TimeBase {
    std::int32_t num = 1;
    std::int32_t den = 60000;
};

/// Where the wall-clock time of one HevcStreamDecoder::open() went.
///
/// Every phase is measured with a steady clock around the call that does the
/// work, so the phases add up to (almost) totalMs; the remainder is option
/// plumbing and allocation.  A phase that did not run in the chosen mode stays
/// at 0 (demuxOpenMs / streamInfoMs in container-sample mode, hwDeviceMs on
/// the software path or when an existing device was reused).
///
/// Exists because "the decoder takes 200 ms to open" is not actionable: the
/// cure for a slow libavformat probe (feed container samples) is different
/// from the cure for a slow device creation (share the device) or a slow
/// first picture (nothing - that is the decode itself).
struct DecoderOpenTimings {
    double mapMs = 0.0;          ///< Memory-mapping the file for the libavformat AVIO callbacks.
    double containerMs = 0.0;    ///< OpenOSV container parse (or parsed-index cache lookup) and track selection.
    double demuxOpenMs = 0.0;    ///< avformat_open_input (libavformat mode only).
    double streamInfoMs = 0.0;   ///< avformat_find_stream_info (libavformat mode only).
    double hwDeviceMs = 0.0;     ///< Hardware device creation or lookup (0 on the software path).
    double codecOpenMs = 0.0;    ///< Codec context setup + avcodec_open2 (hardware device excluded).
    double firstFrameMs = 0.0;   ///< Decoding (and on hardware, reading back) frame 0 to learn the real geometry.
    double totalMs = 0.0;        ///< The whole open() call.
    bool hwDeviceReused = false; ///< True when a live shared hardware device served this decoder.
    bool containerReused = false;///< True when an already parsed container index served this decoder.
    bool firstFrameDeferred = false; ///< True when open() skipped the frame-0 decode (DecoderOptions::deferFirstFrame).
};

class HevcStreamDecoder {
public:
    /// An unopened decoder; every accessor reports zero / empty and every
    /// decode call fails with InvalidArgument.  Exists so the type can live
    /// inside Result<> and std::optional<>.
    HevcStreamDecoder();
    ~HevcStreamDecoder();

    HevcStreamDecoder(HevcStreamDecoder&& other) noexcept;
    HevcStreamDecoder& operator=(HevcStreamDecoder&& other) noexcept;
    HevcStreamDecoder(const HevcStreamDecoder&) = delete;
    HevcStreamDecoder& operator=(const HevcStreamDecoder&) = delete;

    /// Open `path` and select the video track whose ISO BMFF track id equals
    /// `trackId` (1 or 2 on the Osmo 360).  When no stream carries that id
    /// the `trackId`-th video stream (1-based) is used instead, so plain MP4
    /// files still work.  Errors: Io (file missing / unreadable), NotFound
    /// (no such video track), Unsupported (codec or hardware path not
    /// available), Decoder (libavcodec refused the stream).
    static Result<HevcStreamDecoder> open(const std::filesystem::path& path, std::uint32_t trackId,
                                          const DecoderOptions& options = {});

    /// True when open() succeeded and the object has not been moved from.
    [[nodiscard]] bool isOpen() const noexcept;

    /// Number of frames (samples) in the selected track.
    [[nodiscard]] std::uint32_t frameCount() const noexcept;

    /// Frames per second derived from the sample durations (59.94 for the
    /// sample clip), used to map presentation times to frame indices.
    [[nodiscard]] double fps() const noexcept;

    /// Decoded (cropped) luma width / height in pixels.
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;

    /// Bit depth of the coded stream (10 for the native OSV streams, 8 for
    /// the .LRF proxy).  Frames are always delivered on a 10-bit scale.
    [[nodiscard]] std::uint8_t sourceBitDepth() const noexcept;

    /// The hardware path that is actually in use (None after a fallback).
    [[nodiscard]] HwAccel activeHw() const noexcept;

    /// libavcodec decoder name ("hevc", "h264", ...).
    [[nodiscard]] std::string codecName() const;

    /// ISO BMFF track id of the selected stream.
    [[nodiscard]] std::uint32_t trackId() const noexcept;

    /// True when samples come from the OpenOSV container parser rather than
    /// libavformat (DecoderOptions::useContainerSamples honoured).
    [[nodiscard]] bool usesContainerSamples() const noexcept;

    /// Time base of the presentation timestamps (ticks per second = den/num).
    [[nodiscard]] TimeBase timeBase() const noexcept;

    /// Where the time of the open() that produced this decoder went (all
    /// zero on an unopened decoder).  Cheap to call; the numbers are frozen
    /// when open() returns.
    [[nodiscard]] DecoderOpenTimings openTimings() const noexcept;

    /// Index the next call to next() will return.
    [[nodiscard]] std::uint32_t nextIndex() const noexcept;

    /// Decode frame `index` (frame accurate).  Sequential requests decode
    /// forward without seeking; anything else seeks to the previous sync
    /// sample first.  Errors: InvalidArgument (index out of range),
    /// Decoder (libavcodec failure or a presentation time that does not
    /// match the request).
    Result<PlanarFrame16> decodeFrame(std::uint32_t index);

    /// Decode the next frame in presentation order (after open() or seek()
    /// that is frame 0 / the seek target).  Returns NotFound past the end.
    Result<PlanarFrame16> next();

    /// Position the decoder so the next call to next() returns `index`.
    /// The actual seek happens lazily on the next decode.
    Status seek(std::uint32_t index);

    /// Presentation timestamp (in timeBase() ticks) of the last frame
    /// returned, or std::nullopt when nothing was decoded yet.
    [[nodiscard]] std::optional<std::int64_t> lastPts() const noexcept;

    /// CUDA + keepOnDevice only: device pointers of the last decoded frame.
    /// std::nullopt on the software / D3D11VA paths or before any decode.
    [[nodiscard]] std::optional<DeviceFrameRef> lastDeviceFrame() const;

    /// Index of the nearest sync sample (random access point) at or before
    /// `index`, read from the OpenOSV sample table.  This is where a
    /// frame-accurate decode of `index` starts, so a caller that wants to keep
    /// every frame of the GOP knows which frames the decoder will produce on
    /// the way.  An index past the end resolves to the last GOP.
    /// std::nullopt when the decoder is not open or our container parser
    /// declined the file (avformat mode on a plain MP4 it cannot read).
    [[nodiscard]] std::optional<std::uint32_t> previousSyncIndex(std::uint32_t index) const noexcept;

    /// Runtime FFmpeg library versions, e.g. "avcodec 63.1.100 / avformat 63.0.100 / avutil 61.0.100".
    [[nodiscard]] static std::string ffmpegVersion();

    /// The configure line libavcodec was built with.
    [[nodiscard]] static std::string ffmpegConfiguration();

    /// True when the linked libavcodec was configured with --enable-gpl or
    /// --enable-nonfree, i.e. binaries built against it are NOT
    /// redistributable under Apache-2.0.
    [[nodiscard]] static bool ffmpegIsGpl();

    /// Names of the hardware device types compiled into the linked FFmpeg
    /// that this module knows how to drive ("none" is always listed).
    /// Presence means the code path exists, not that a device is available.
    [[nodiscard]] static std::vector<std::string> availableHwAccels();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::video
