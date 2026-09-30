// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// WavWriter: a streaming writer for 32-bit float PCM WAV files.
//
// The layout written is the one every DAW and editor reads for float audio
// (Microsoft's WAVEFORMATEX rules for WAVE_FORMAT_IEEE_FLOAT):
//
//   offset  size  chunk
//   0       12    "RIFF" <u32 riffSize> "WAVE"
//   12      26    "fmt " <u32 18> format 3 (IEEE float), channels, sample rate,
//                 byte rate, block align, 32 bits, cbSize 0
//   38      12    "fact" <u32 4> <u32 sample frames per channel>
//   50      8     "data" <u32 dataBytes>
//   58      ...   interleaved little-endian float32 samples
//
// The header is 58 bytes.  Both sizes are 32-bit, so a file can carry at most
// kMaxDataBytes of samples (just under 4 GiB); a caller that knows its length
// up front (open() takes it) is refused before a byte is written, and a
// caller that does not is refused by write() when the limit is reached.
//
// Crash safety: samples go to "<path>.partial" and only finalize() renames
// that file onto `path`, after the RIFF / fact / data sizes are patched.  An
// interrupted run therefore never leaves a half-written file under the real
// name; destroying the writer without finalize() deletes the partial file.
#pragma once

#include "osv/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace osv::io {

class WavWriter {
public:
    /// Size of everything before the first sample byte.
    static constexpr std::uint64_t kHeaderBytes = 58;

    /// Largest data chunk a 32-bit RIFF can describe: the RIFF size field
    /// counts the 50 header bytes that follow it as well.
    static constexpr std::uint64_t kMaxDataBytes = 0xFFFFFFFFull - (kHeaderBytes - 8);

    WavWriter();
    ~WavWriter();
    WavWriter(WavWriter&&) noexcept;
    WavWriter& operator=(WavWriter&&) noexcept;
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /// Most sample frames (one sample per channel) a WAV of `channels`
    /// channels can hold; 0 for a channel count of 0.
    [[nodiscard]] static std::uint64_t maxFrames(std::uint32_t channels) noexcept;

    /// Create "<path>.partial" and write the header with zero sizes.
    ///
    /// `sampleRate` and `channels` must be non-zero, the block align (4 * channels)
    /// must fit 16 bits and the byte rate (sampleRate * block align) 32 bits.
    /// `expectedFrames` is the length the caller intends to write (0 =
    /// unknown); when it does not fit maxFrames() the call fails with
    /// InvalidArgument and names the limit, before any file is created.
    [[nodiscard]] static Result<WavWriter> open(const std::filesystem::path& path, std::uint32_t sampleRate,
                                                std::uint32_t channels, std::uint64_t expectedFrames = 0);

    /// True after a successful open() and until finalize() or a move-from.
    [[nodiscard]] bool isOpen() const noexcept;

    [[nodiscard]] std::uint32_t sampleRate() const noexcept;
    [[nodiscard]] std::uint32_t channels() const noexcept;

    /// Sample frames written so far.
    [[nodiscard]] std::uint64_t framesWritten() const noexcept;

    /// Append `frames` interleaved sample frames (`frames * channels()`
    /// floats at `interleaved`).  Fails with InvalidArgument for a null
    /// pointer, and when the block would push the file past kMaxDataBytes.
    /// A failed write leaves the writer open but the file must be abandoned.
    [[nodiscard]] Status write(const float* interleaved, std::size_t frames);

    /// Patch the RIFF, fact and data sizes, close the file and rename it onto
    /// the final path (replacing an existing file).  The writer is closed
    /// afterwards whether or not it succeeded; on failure the partial file is
    /// removed.
    [[nodiscard]] Status finalize();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::io
