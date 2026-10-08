// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FfmpegPipeWriter: streams rendered frames as raw rgb48le video into an
// external ffmpeg.exe process which encodes HEVC 10-bit with the correct
// BT.2100 signalling and (optionally) copies the source audio track.
//
// Piping keeps GPL-licensed encoders (libx265) out of our binary: the user's
// ffmpeg.exe is a separate program and its licence is its own business.
#pragma once

#include "osv/core/Result.h"
#include "osv/render/ImageRGBAf.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace osv::io {

enum class PipeTransfer { HLG, PQ, Rec709 };

struct FfmpegPipeOptions {
    std::filesystem::path ffmpegExe;       ///< Empty = "ffmpeg" on PATH (or OSV_FFMPEG_EXE env).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double fps = 59.94;
    std::string codec = "hevc_nvenc";      ///< First choice encoder.
    std::string fallbackCodec = "libx265"; ///< Used when the first one fails to start.
    std::string pixFmt = "yuv420p10le";
    PipeTransfer transfer = PipeTransfer::PQ;
    int crf = 18;                          ///< Quality for software encoders (-crf) / nvenc (-cq).
    std::filesystem::path audioSource;     ///< Optional: copy the first audio stream of this file.
    /// Where in `audioSource` the copied audio starts, in seconds (>= 0).
    ///
    /// A render of frames A..B must play the sound recorded with frame A, not
    /// the sound at 0:00, so the caller sets this to the moment of the first
    /// rendered frame on the clip's timeline (first frame x rate denominator
    /// / rate numerator).  It becomes an input-side `-ss` on the audio input
    /// only, written with microsecond precision.  With `-c:a copy` ffmpeg's
    /// input seek lands on the clip's VIDEO keyframe at or before that
    /// moment (the default stream of the seek) and keeps every audio packet
    /// from there on, timed relative to the moment itself, so the lead-in
    /// carries negative timestamps: up to one keyframe interval (0.8 s on a
    /// 25 fps Osmo 360 clip), not just one AAC packet.  The .mp4's edit list
    /// trims that lead-in, so the sound lines up to the sample wherever edit
    /// lists are honoured; a player that ignores them starts the sound up to
    /// one keyframe interval early.  0 - the default - adds no `-ss`
    /// at all, so a render from the clip's start keeps the exact command
    /// line it always had.  NaN, infinite or negative values are treated as
    /// 0 (FfmpegPipeWriter::open logs a warning).  Ignored without
    /// `audioSource`.
    double audioStartSeconds = 0.0;
    std::vector<std::string> extraArgs;    ///< Appended verbatim before the output path.
};

/// The ffmpeg argument vector (WITHOUT the executable itself) that
/// FfmpegPipeWriter::open() starts the encoder `codec` with, writing `out`.
///
/// Input 0 is the raw rgb48le frames on stdin; input 1, when
/// `options.audioSource` is set, is that file, seeked to
/// `options.audioStartSeconds` and its first audio stream copied.  Exposed
/// so the exact command line can be checked without starting a process;
/// open() uses nothing else.  Pure: no files, no environment, no logging.
///
/// @param options  The pipe's options (size, rate, codec knobs, audio).
/// @param codec    The encoder to name after -c:v (options.codec or its fallback).
/// @param out      The output file, passed through as the last argument.
/// @return         Every argument in order, each one unquoted.
[[nodiscard]] std::vector<std::string> buildFfmpegArgs(const FfmpegPipeOptions& options, const std::string& codec,
                                                       const std::filesystem::path& out);

class FfmpegPipeWriter {
public:
    /// An unopened writer (every call fails with Io until open() succeeds).
    FfmpegPipeWriter();
    ~FfmpegPipeWriter();
    FfmpegPipeWriter(FfmpegPipeWriter&&) noexcept;
    FfmpegPipeWriter& operator=(FfmpegPipeWriter&&) noexcept;
    FfmpegPipeWriter(const FfmpegPipeWriter&) = delete;
    FfmpegPipeWriter& operator=(const FfmpegPipeWriter&) = delete;

    /// Start ffmpeg for `out`.  Fails with Io when the executable cannot be
    /// started or exits immediately (the fallback codec is tried first).
    static Result<FfmpegPipeWriter> open(const FfmpegPipeOptions& options, const std::filesystem::path& out);

    /// Convert one frame to rgb48le and push it down the pipe.
    Status writeFrame(const render::ImageRGBAf& image);

    /// Close stdin, wait for ffmpeg and report a non-zero exit code as Io.
    Status close();

    /// The command line that was (or would be) executed, for logs.
    [[nodiscard]] const std::string& commandLine() const noexcept;

    /// Number of frames written so far.
    [[nodiscard]] std::uint64_t framesWritten() const noexcept;

    /// Resolve the ffmpeg executable (options.ffmpegExe, OSV_FFMPEG_EXE, PATH).
    static std::filesystem::path resolveExecutable(const std::filesystem::path& preferred);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::io
