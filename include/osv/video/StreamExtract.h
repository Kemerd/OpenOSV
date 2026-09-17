// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Raw elementary-stream extraction straight from the container parser (no
// FFmpeg involved).  Backs `osvtool extract --hevc` and `--audio` and is
// exposed as a library function so the test-suite can verify the output
// without spawning the tool.
//
//   * HEVC  -> Annex-B byte stream: the hvcC parameter sets (VPS/SPS/PPS)
//              once at the start, then every sample with its 4-byte NAL
//              length prefixes rewritten as 00 00 00 01 start codes.  The
//              result plays in any player and round-trips through ffmpeg
//              with `-c copy`.
//   * AAC   -> ADTS: every mp4a sample gets a 7-byte ADTS header built from
//              the esds AudioSpecificConfig (object type, sampling frequency
//              index, channel configuration).
#pragma once

#include "osv/core/Result.h"

#include <cstdint>
#include <filesystem>

namespace osv::video {

/// What writeAnnexBStream() produced.
struct AnnexBStats {
    std::uint32_t trackId = 0;          ///< Track that was extracted.
    std::uint32_t samples = 0;          ///< Samples (access units) written.
    std::uint32_t parameterSetNals = 0; ///< NAL units taken from hvcC (VPS+SPS+PPS).
    std::uint64_t sampleNals = 0;       ///< NAL units found inside the samples.
    std::uint64_t bytesWritten = 0;     ///< Size of the output file.
};

/// Extract video track `trackId` of `osvPath` as an Annex-B .hevc file.
/// Errors: Io, NotFound (no such track), Unsupported (track is not HEVC /
/// has no hvcC), Malformed (a NAL length runs past its sample).
Result<AnnexBStats> writeAnnexBStream(const std::filesystem::path& osvPath, std::uint32_t trackId,
                                      const std::filesystem::path& outPath);

/// What writeAdtsAudio() produced.
struct AdtsStats {
    std::uint32_t trackId = 0;          ///< Audio track that was extracted.
    std::uint32_t samples = 0;          ///< AAC frames written.
    std::uint8_t audioObjectType = 0;   ///< From AudioSpecificConfig (2 = AAC-LC).
    std::uint8_t samplingIndex = 0;     ///< Sampling frequency index (3 = 48 kHz).
    std::uint32_t sampleRate = 0;       ///< Sampling frequency in Hz.
    std::uint8_t channelConfig = 0;     ///< Channel configuration (2 = stereo).
    std::uint64_t bytesWritten = 0;     ///< Size of the output file.
};

/// Extract the AAC track of `osvPath` as an ADTS .aac file.  `trackId` 0
/// selects the first mp4a track found in the movie.  Errors: Io, NotFound
/// (no audio track), Unsupported (not AAC / no usable AudioSpecificConfig).
Result<AdtsStats> writeAdtsAudio(const std::filesystem::path& osvPath, std::uint32_t trackId,
                                 const std::filesystem::path& outPath);

}  // namespace osv::video
