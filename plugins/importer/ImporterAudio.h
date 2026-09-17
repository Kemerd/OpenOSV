// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// AudioDecoder: the importer's AAC path (docs/PREMIERE.md, "Audio").
//
// Premiere always wants 32-bit float, uninterleaved, one buffer per channel,
// addressed in sample frames at the media's own sample rate.  The .OSV
// carries AAC-LC, so the importer decodes it itself; the host never sees the
// bitstream.
//
// Two hard requirements from the design shape this class:
//
//   1. Audio and video must never share a demuxer.  Conforming (which walks
//      the whole audio track sequentially) and scrubbing (which seeks the
//      video all over the timeline) happen at the same time; one
//      AVFormatContext serving both would have the two seek each other into
//      the ground.  AudioDecoder therefore opens its own AVFormatContext on
//      the same path, which means its own OS handle.
//
//   2. Both access patterns must work:
//        * imImportAudio7 with position >= 0  -> random access: seek to the
//          packet containing that sample, decode from the preceding sync
//          point and DISCARD the priming samples (AAC encoders prepend
//          ~1024-2048 samples of encoder delay, and a seek lands on a packet
//          boundary that is almost never the requested sample);
//        * imResetSequentialAudio / imGetSequentialAudio -> a cursor that
//          continues where the last call stopped, with no seek at all.
//
// Output past the end of stream is zero-filled rather than failing: Premiere
// routinely asks for a few samples beyond the duration when conforming the
// tail of a clip.
//
// All methods are serialised by the caller (ImporterInstance's mutex); the
// class itself keeps no global state.
#pragma once

#include "osv/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace osv::premiere {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    /// Open the first audio stream of `path` and set up the AAC decoder plus
    /// a swresample converter to planar float at the native rate.  Fails with
    /// Io when the file cannot be demuxed and NotFound when it carries no
    /// audio stream.
    [[nodiscard]] static Result<AudioDecoder> open(const std::filesystem::path& path);

    /// True after a successful open() and before a move-from.
    [[nodiscard]] bool isOpen() const noexcept;

    [[nodiscard]] std::int32_t channels() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept;
    /// Total sample frames (0 when the container does not say).
    [[nodiscard]] std::int64_t durationSamples() const noexcept;

    /// Read `count` sample frames starting at absolute frame `position` into
    /// `buffers[ch][0 .. count)`.  Seeks when the position is not where the
    /// cursor already is.  Frames past the end of stream are zero-filled and
    /// still reported as success, because that is what the host expects.
    ///
    /// `buffers` must hold at least channels() pointers, each to at least
    /// `count` floats.  A null pointer for a channel makes that channel be
    /// discarded (the host never does this, but a defensive check costs
    /// nothing).
    [[nodiscard]] Status read(std::int64_t position, std::uint32_t count, float* const* buffers);

    /// Continue from the sequential cursor (imGetSequentialAudio).  Same
    /// zero-fill rule at the end of stream.
    [[nodiscard]] Status readSequential(std::uint32_t count, float* const* buffers);

    /// Rewind the sequential cursor to sample 0 (imResetSequentialAudio).
    void resetSequential();

    /// Current sequential cursor in sample frames.
    [[nodiscard]] std::int64_t sequentialPosition() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::premiere
