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
    std::vector<std::string> extraArgs;    ///< Appended verbatim before the output path.
};

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
