// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Codec configuration records found inside video sample entries:
//   * HevcConfig - 'hvcC' (ISO/IEC 14496-15 8.3.3.1), used by the OSV streams.
//   * AvcConfig  - 'avcC' (ISO/IEC 14496-15 5.3.3.1), used by the LRF proxy.
// Both expose the parameter sets so the decoder can be primed with an
// Annex-B header and the NAL length prefix size so samples can be re-framed.
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <vector>

namespace osv {

/// One array of NAL units of a single type from hvcC.
struct HevcNalArray {
    std::uint8_t nalType = 0;         ///< NAL unit type (32 VPS, 33 SPS, 34 PPS, 39 SEI ...).
    bool arrayCompleteness = false;   ///< True when the stream carries no other NALs of this type.
    std::vector<ByteSpan> nalUnits;   ///< Raw NAL units (no length prefix, no start code).
};

/// Parsed 'hvcC' HEVCDecoderConfigurationRecord.  Spans alias the file.
struct HevcConfig {
    std::uint8_t configurationVersion = 0;
    std::uint8_t generalProfileSpace = 0;
    bool generalTierFlag = false;
    std::uint8_t generalProfileIdc = 0;                 ///< 1 Main, 2 Main 10.
    std::uint32_t generalProfileCompatibilityFlags = 0;
    std::uint64_t generalConstraintIndicatorFlags = 0;  ///< 48 significant bits.
    std::uint8_t generalLevelIdc = 0;                   ///< level * 30 (156 = 5.2).
    std::uint16_t minSpatialSegmentationIdc = 0;
    std::uint8_t parallelismType = 0;
    std::uint8_t chromaFormatIdc = 0;                   ///< 0 mono, 1 4:2:0, 2 4:2:2, 3 4:4:4.
    std::uint8_t bitDepthLumaMinus8 = 0;
    std::uint8_t bitDepthChromaMinus8 = 0;
    std::uint16_t avgFrameRate = 0;                     ///< Units of 1/256 fps, 0 = unspecified.
    std::uint8_t constantFrameRate = 0;
    std::uint8_t numTemporalLayers = 0;
    bool temporalIdNested = false;
    std::uint8_t lengthSizeMinusOne = 0;                ///< NAL length prefix size - 1 (usually 3).
    std::vector<HevcNalArray> arrays;
    ByteSpan raw;                                       ///< The whole record.

    /// Parse the payload of an 'hvcC' box.
    [[nodiscard]] static Result<HevcConfig> parse(ByteSpan payload);

    /// Size in bytes of the length prefix in front of every NAL unit in a sample.
    [[nodiscard]] std::uint32_t nalLengthSize() const noexcept { return static_cast<std::uint32_t>(lengthSizeMinusOne) + 1; }

    /// Luma bit depth (8 or 10 for DJI clips).
    [[nodiscard]] std::uint32_t bitDepthLuma() const noexcept { return static_cast<std::uint32_t>(bitDepthLumaMinus8) + 8; }

    /// Chroma bit depth.
    [[nodiscard]] std::uint32_t bitDepthChroma() const noexcept { return static_cast<std::uint32_t>(bitDepthChromaMinus8) + 8; }

    /// Every NAL unit of the given type, in file order.
    [[nodiscard]] std::vector<ByteSpan> nalUnitsOfType(std::uint8_t nalType) const;

    /// First VPS / SPS / PPS (empty span when absent).
    [[nodiscard]] ByteSpan vps() const;
    [[nodiscard]] ByteSpan sps() const;
    [[nodiscard]] ByteSpan pps() const;

    /// All parameter set NAL units concatenated with 4-byte start codes, in
    /// the order they appear in the record: ready to feed to a decoder before
    /// the first sample (the sample itself must be converted with
    /// convertSampleToAnnexB or fed through an "hvc1"-aware demuxer).
    [[nodiscard]] std::vector<std::uint8_t> toAnnexBHeader() const;

    /// Count of NAL units across all arrays.
    [[nodiscard]] std::size_t nalUnitCount() const noexcept;
};

/// Parsed 'avcC' AVCDecoderConfigurationRecord (H.264, used by the LRF proxy).
struct AvcConfig {
    std::uint8_t configurationVersion = 0;
    std::uint8_t avcProfileIndication = 0;   ///< 66 Baseline, 77 Main, 100 High.
    std::uint8_t profileCompatibility = 0;
    std::uint8_t avcLevelIndication = 0;     ///< level * 10 (52 = 5.2).
    std::uint8_t lengthSizeMinusOne = 0;
    std::vector<ByteSpan> sps;               ///< Raw SPS NAL units.
    std::vector<ByteSpan> pps;               ///< Raw PPS NAL units.
    ByteSpan raw;

    /// Parse the payload of an 'avcC' box.
    [[nodiscard]] static Result<AvcConfig> parse(ByteSpan payload);

    /// NAL length prefix size in samples.
    [[nodiscard]] std::uint32_t nalLengthSize() const noexcept { return static_cast<std::uint32_t>(lengthSizeMinusOne) + 1; }

    /// SPS + PPS with 4-byte start codes.
    [[nodiscard]] std::vector<std::uint8_t> toAnnexBHeader() const;
};

/// Re-frame one length-prefixed sample (as stored in hvc1/avc1 tracks) into
/// Annex-B byte-stream format: every `lengthSize`-byte big-endian NAL length
/// becomes a 00 00 00 01 start code.  A length that runs past the sample ends
/// the conversion with Malformed; what was converted so far is discarded.
[[nodiscard]] Result<std::vector<std::uint8_t>> convertSampleToAnnexB(ByteSpan sample, std::uint32_t lengthSize);

}  // namespace osv
