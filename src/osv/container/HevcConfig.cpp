// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// hvcC / avcC parsing and Annex-B conversion helpers.

#include "osv/container/HevcConfig.h"

#include "osv/core/ByteReader.h"

namespace osv {

namespace {

// The 4-byte Annex-B start code.
constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

/// Append start code + NAL bytes to `out`.
void appendAnnexB(std::vector<std::uint8_t>& out, ByteSpan nal) {
    if (nal.empty()) {
        return;
    }
    out.insert(out.end(), kStartCode, kStartCode + 4);
    out.insert(out.end(), nal.begin(), nal.end());
}

}  // namespace

// -----------------------------------------------------------------------------
//  HevcConfig
// -----------------------------------------------------------------------------

Result<HevcConfig> HevcConfig::parse(ByteSpan payload) {
    // The fixed part of the record is 23 bytes (ISO/IEC 14496-15 8.3.3.1.2).
    if (payload.size() < 23) {
        return Error{ErrorCode::Truncated,
                     "hvcC record is " + std::to_string(payload.size()) + " bytes, at least 23 required"};
    }
    ByteReader reader(payload);
    HevcConfig cfg;
    cfg.raw = payload;

    std::uint8_t b = 0;
    // configurationVersion (always 1).
    reader.u8(cfg.configurationVersion);
    // general_profile_space(2) general_tier_flag(1) general_profile_idc(5)
    reader.u8(b);
    cfg.generalProfileSpace = static_cast<std::uint8_t>(b >> 6);
    cfg.generalTierFlag = (b & 0x20u) != 0;
    cfg.generalProfileIdc = static_cast<std::uint8_t>(b & 0x1Fu);
    // general_profile_compatibility_flags (32 bits)
    reader.u32be(cfg.generalProfileCompatibilityFlags);
    // general_constraint_indicator_flags (48 bits)
    std::uint64_t constraint = 0;
    for (int i = 0; i < 6; ++i) {
        reader.u8(b);
        constraint = (constraint << 8) | b;
    }
    cfg.generalConstraintIndicatorFlags = constraint;
    // general_level_idc
    reader.u8(cfg.generalLevelIdc);
    // reserved(4) + min_spatial_segmentation_idc(12)
    std::uint16_t u16 = 0;
    reader.u16be(u16);
    cfg.minSpatialSegmentationIdc = static_cast<std::uint16_t>(u16 & 0x0FFFu);
    // reserved(6) + parallelismType(2)
    reader.u8(b);
    cfg.parallelismType = static_cast<std::uint8_t>(b & 0x03u);
    // reserved(6) + chromaFormat(2)
    reader.u8(b);
    cfg.chromaFormatIdc = static_cast<std::uint8_t>(b & 0x03u);
    // reserved(5) + bitDepthLumaMinus8(3)
    reader.u8(b);
    cfg.bitDepthLumaMinus8 = static_cast<std::uint8_t>(b & 0x07u);
    // reserved(5) + bitDepthChromaMinus8(3)
    reader.u8(b);
    cfg.bitDepthChromaMinus8 = static_cast<std::uint8_t>(b & 0x07u);
    // avgFrameRate
    reader.u16be(cfg.avgFrameRate);
    // constantFrameRate(2) numTemporalLayers(3) temporalIdNested(1) lengthSizeMinusOne(2)
    reader.u8(b);
    cfg.constantFrameRate = static_cast<std::uint8_t>(b >> 6);
    cfg.numTemporalLayers = static_cast<std::uint8_t>((b >> 3) & 0x07u);
    cfg.temporalIdNested = (b & 0x04u) != 0;
    cfg.lengthSizeMinusOne = static_cast<std::uint8_t>(b & 0x03u);
    // numOfArrays
    std::uint8_t numArrays = 0;
    reader.u8(numArrays);

    // NAL unit arrays.  Each array: completeness/type byte, u16 count, then
    // (u16 length + bytes) per NAL.  A short record ends with Truncated so a
    // decoder never sees half a parameter set.
    cfg.arrays.reserve(numArrays);
    for (std::uint8_t a = 0; a < numArrays; ++a) {
        HevcNalArray array;
        std::uint8_t typeByte = 0;
        std::uint16_t numNalus = 0;
        if (!reader.u8(typeByte) || !reader.u16be(numNalus)) {
            return Error{ErrorCode::Truncated, "hvcC ends inside NAL array " + std::to_string(a)};
        }
        array.arrayCompleteness = (typeByte & 0x80u) != 0;
        array.nalType = static_cast<std::uint8_t>(typeByte & 0x3Fu);
        array.nalUnits.reserve(numNalus);
        for (std::uint16_t n = 0; n < numNalus; ++n) {
            std::uint16_t length = 0;
            ByteSpan nal;
            if (!reader.u16be(length) || !reader.bytes(length, nal)) {
                return Error{ErrorCode::Truncated, "hvcC ends inside NAL unit " + std::to_string(n) + " of array " +
                                                       std::to_string(a)};
            }
            array.nalUnits.push_back(nal);
        }
        cfg.arrays.push_back(std::move(array));
    }
    return cfg;
}

std::vector<ByteSpan> HevcConfig::nalUnitsOfType(std::uint8_t nalType) const {
    std::vector<ByteSpan> out;
    for (const HevcNalArray& array : arrays) {
        if (array.nalType != nalType) {
            continue;
        }
        out.insert(out.end(), array.nalUnits.begin(), array.nalUnits.end());
    }
    return out;
}

ByteSpan HevcConfig::vps() const {
    const std::vector<ByteSpan> units = nalUnitsOfType(32);
    return units.empty() ? ByteSpan{} : units.front();
}

ByteSpan HevcConfig::sps() const {
    const std::vector<ByteSpan> units = nalUnitsOfType(33);
    return units.empty() ? ByteSpan{} : units.front();
}

ByteSpan HevcConfig::pps() const {
    const std::vector<ByteSpan> units = nalUnitsOfType(34);
    return units.empty() ? ByteSpan{} : units.front();
}

std::size_t HevcConfig::nalUnitCount() const noexcept {
    std::size_t total = 0;
    for (const HevcNalArray& array : arrays) {
        total += array.nalUnits.size();
    }
    return total;
}

std::vector<std::uint8_t> HevcConfig::toAnnexBHeader() const {
    std::vector<std::uint8_t> out;
    // Pre-size: 4 bytes of start code per NAL plus the NAL bytes.
    std::size_t bytes = 0;
    for (const HevcNalArray& array : arrays) {
        for (const ByteSpan& nal : array.nalUnits) {
            bytes += 4 + nal.size();
        }
    }
    out.reserve(bytes);
    for (const HevcNalArray& array : arrays) {
        for (const ByteSpan& nal : array.nalUnits) {
            appendAnnexB(out, nal);
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
//  AvcConfig
// -----------------------------------------------------------------------------

Result<AvcConfig> AvcConfig::parse(ByteSpan payload) {
    // Fixed part: version, profile, compat, level, lengthSizeMinusOne, numSPS.
    if (payload.size() < 6) {
        return Error{ErrorCode::Truncated,
                     "avcC record is " + std::to_string(payload.size()) + " bytes, at least 6 required"};
    }
    ByteReader reader(payload);
    AvcConfig cfg;
    cfg.raw = payload;
    std::uint8_t b = 0;
    reader.u8(cfg.configurationVersion);
    reader.u8(cfg.avcProfileIndication);
    reader.u8(cfg.profileCompatibility);
    reader.u8(cfg.avcLevelIndication);
    // reserved(6) + lengthSizeMinusOne(2)
    reader.u8(b);
    cfg.lengthSizeMinusOne = static_cast<std::uint8_t>(b & 0x03u);
    // reserved(3) + numOfSequenceParameterSets(5)
    reader.u8(b);
    const std::uint8_t numSps = static_cast<std::uint8_t>(b & 0x1Fu);
    for (std::uint8_t i = 0; i < numSps; ++i) {
        std::uint16_t length = 0;
        ByteSpan nal;
        if (!reader.u16be(length) || !reader.bytes(length, nal)) {
            return Error{ErrorCode::Truncated, "avcC ends inside SPS " + std::to_string(i)};
        }
        cfg.sps.push_back(nal);
    }
    std::uint8_t numPps = 0;
    if (!reader.u8(numPps)) {
        return Error{ErrorCode::Truncated, "avcC ends before the PPS count"};
    }
    for (std::uint8_t i = 0; i < numPps; ++i) {
        std::uint16_t length = 0;
        ByteSpan nal;
        if (!reader.u16be(length) || !reader.bytes(length, nal)) {
            return Error{ErrorCode::Truncated, "avcC ends inside PPS " + std::to_string(i)};
        }
        cfg.pps.push_back(nal);
    }
    // High-profile extensions (chroma format, bit depth, SPS ext) follow for
    // profiles 100/110/122/144; they are not needed to decode and are left in
    // `raw` for anyone who wants them.
    return cfg;
}

std::vector<std::uint8_t> AvcConfig::toAnnexBHeader() const {
    std::vector<std::uint8_t> out;
    for (const ByteSpan& nal : sps) {
        appendAnnexB(out, nal);
    }
    for (const ByteSpan& nal : pps) {
        appendAnnexB(out, nal);
    }
    return out;
}

// -----------------------------------------------------------------------------
//  Sample conversion
// -----------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> convertSampleToAnnexB(ByteSpan sample, std::uint32_t lengthSize) {
    if (lengthSize < 1 || lengthSize > 4) {
        return Error{ErrorCode::InvalidArgument, "NAL length size must be 1..4, got " + std::to_string(lengthSize)};
    }
    std::vector<std::uint8_t> out;
    // Output is at most the input plus (4 - lengthSize) bytes per NAL; the
    // input size is a fine reservation for the common 4-byte case.
    out.reserve(sample.size() + 16);
    ByteReader reader(sample);
    while (!reader.atEnd()) {
        // Read the big-endian length of the next NAL unit.
        std::uint32_t length = 0;
        for (std::uint32_t i = 0; i < lengthSize; ++i) {
            std::uint8_t b = 0;
            if (!reader.u8(b)) {
                return Error{ErrorCode::Malformed, "sample ends inside a NAL length prefix at byte " +
                                                       std::to_string(reader.pos())};
            }
            length = (length << 8) | b;
        }
        ByteSpan nal;
        if (!reader.bytes(length, nal)) {
            return Error{ErrorCode::Malformed, "NAL length " + std::to_string(length) + " at byte " +
                                                   std::to_string(reader.pos()) + " runs past the sample end"};
        }
        // Zero-length NAL units are legal but useless; keep the start code
        // out to avoid confusing decoders.
        appendAnnexB(out, nal);
    }
    return out;
}

}  // namespace osv
