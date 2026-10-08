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
//   * Frame accuracy.  decodeFrame(i) only returns sample i: the frame
//     whose presentation time is sample i's in our SampleTable whenever the
//     container parsed (every camera file, in both demuxing modes), so the
//     index is the sample (and djmd) index even on a variable-frame-rate clip
//     whose table records a dropped frame as one longer sample.  Only a file
//     our parser declines falls back to a time index at the average frame
//     rate.  A request outside the currently decoded GOP seeks to the
//     previous sync sample (from our SampleTable when the container parsed,
//     otherwise through FFmpeg's index) and decodes forward, discarding
//     frames until the timestamp matches.
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

/// @brief What libavcodec said about one decoded picture.
///
/// Recorded for the picture decodeFrame() handed out last (or refused last:
/// a hardware picture libavcodec flagged as damaged is refused, see
/// decodeFrame()), so a caller that suspects the picture - the LRF shadow
/// verifier in DualStreamReader - can name exactly what the decoder knew.
struct DecodedFrameInfo {
    std::uint32_t index = 0;        ///< Frame (sample) index of the picture.
    HwAccel hw = HwAccel::None;     ///< The path that decoded it (activeHw() at that moment).
    bool keyFrame = false;          ///< AV_FRAME_FLAG_KEY: an intra random access picture.
    bool corrupt = false;           ///< AV_FRAME_FLAG_CORRUPT: libavcodec marked the picture as possibly damaged.
    int decodeErrorFlags = 0;       ///< AVFrame::decode_error_flags (FF_DECODE_ERROR_* bits; 0 = clean).
    /// D3D11VA only: the texture-array slice the picture was decoded into
    /// (AVFrame::data[1] of an AV_PIX_FMT_D3D11 frame), so two reports that
    /// name the same surface can be told apart from two that do not.  -1 on
    /// every other path.
    std::int64_t surface = -1;
    /// The first picture since the last key frame - this one included - that
    /// libavcodec flagged as damaged, catch-up pictures counted; -1 when none.
    /// A picture predicted from a damaged one carries its damage WITHOUT being
    /// flagged itself, so this, not the picture's own flags, says whether the
    /// recording is damaged up to here.
    std::int64_t gopDamagedAt = -1;
};

/// @brief Content fingerprint of a decoded picture, for per-frame log lines.
///
/// CRC-64/XZ (osv/core/Crc64.h) over every 4th row of luma, then every 4th
/// row of Cb, then of Cr, each sample brought to the frame's bitDepth scale
/// (sample >> bitShift) and hashed as 16-bit little-endian.  Brought to one
/// layout like that, a D3D11VA picture (NV12, interleaved chroma) and a
/// software one (yuv420p, planar) of the same content fingerprint
/// identically, so a host session's "decode:" lines can be compared with an
/// offline software decode of the same frames.  Every 4th row still catches
/// any block-sized damage (16-pixel macroblocks span 16 rows) at a quarter of
/// the cost of hashing the whole picture.
///
/// @param frame  A host-memory picture (PlanarFrame16::valid()).
/// @return The fingerprint, or 0 for an invalid frame.
[[nodiscard]] std::uint64_t frameFingerprint(const PlanarFrame16& frame) noexcept;

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

    /// AVERAGE frames per second over the sample durations (59.94 for the
    /// sample clip).  Exact on a constant-rate track; on a variable-rate one
    /// informational only - presentation times map to frame indices through
    /// the sample table, and this rate is that mapping only for a file the
    /// container parser declined.
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
    /// Decoder (libavcodec failure, or a HARDWARE picture libavcodec flagged
    /// as damaged - decode_error_flags or AV_FRAME_FLAG_CORRUPT - which a
    /// caller with a software fallback should decode again there), Timing
    /// (the stream produced a presentation time past the request - a
    /// property of the file that every decoder reproduces, never a reason to
    /// leave hardware decoding).  A flagged SOFTWARE picture is still
    /// returned, with a warning: there is no better decoder to ask.
    Result<PlanarFrame16> decodeFrame(std::uint32_t index);

    /// What libavcodec reported about the last picture decodeFrame() / next()
    /// returned or refused; std::nullopt before the first one.
    [[nodiscard]] std::optional<DecodedFrameInfo> lastFrameInfo() const noexcept;

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
