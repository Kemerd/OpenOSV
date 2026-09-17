// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Elementary-stream extraction (see StreamExtract.h).  Everything here works
// on the container parser's sample table and sample entries; libavcodec is
// never touched, which keeps `osvtool extract --hevc/--audio` usable as an
// independent cross-check of the FFmpeg path.

#include "osv/video/StreamExtract.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace osv::video {

namespace {

/// Buffered binary writer over std::ofstream with a running byte count and
/// an error flag, so the extraction loops stay readable.
class OutputFile {
public:
    explicit OutputFile(const std::filesystem::path& path) : m_stream(path, std::ios::binary | std::ios::trunc) {}

    [[nodiscard]] bool isOpen() const { return m_stream.is_open() && m_stream.good(); }

    /// Append `size` bytes; returns false once the stream went bad.
    bool write(const std::uint8_t* data, std::size_t size) {
        if (!data && size > 0) {
            return false;
        }
        if (size == 0) {
            return m_stream.good();
        }
        m_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        m_written += size;
        return m_stream.good();
    }

    bool write(const std::vector<std::uint8_t>& bytes) { return write(bytes.data(), bytes.size()); }

    bool write(ByteSpan span) { return write(span.data(), span.size()); }

    /// Flush and report whether every byte made it to disk.
    bool finish() {
        m_stream.flush();
        return m_stream.good();
    }

    [[nodiscard]] std::uint64_t written() const noexcept { return m_written; }

private:
    std::ofstream m_stream;
    std::uint64_t m_written = 0;
};

/// Big-endian NAL length prefix (1..4 bytes) at `pos` of `sample`.
[[nodiscard]] std::uint32_t readNalLength(ByteSpan sample, std::uint64_t pos, std::uint32_t lengthSize) noexcept {
    std::uint32_t len = 0;
    for (std::uint32_t i = 0; i < lengthSize; ++i) {
        len = (len << 8) | sample[static_cast<std::size_t>(pos + i)];
    }
    return len;
}

/// The Annex-B start code every NAL unit is prefixed with.
constexpr std::array<std::uint8_t, 4> kStartCode{0x00, 0x00, 0x00, 0x01};

/// ISO/IEC 14496-3 sampling frequency index table (index 13/14 reserved,
/// 15 = explicit frequency which ADTS cannot express).
constexpr std::array<std::uint32_t, 16> kSamplingFrequencies{
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};

}  // namespace

// =============================================================================
//  writeAnnexBStream
// =============================================================================
Result<AnnexBStats> writeAnnexBStream(const std::filesystem::path& osvPath, std::uint32_t trackId,
                                      const std::filesystem::path& outPath) {
    if (osvPath.empty() || outPath.empty()) {
        return Error{ErrorCode::InvalidArgument, "input and output paths are required"};
    }
    OSV_TRY_ASSIGN(const OsvFile file, OsvFile::open(osvPath));

    // ---- select and validate the track -------------------------------------
    const TrackInfo* track = file.track(trackId);
    if (!track) {
        return Error{ErrorCode::NotFound, "no track with id " + std::to_string(trackId)};
    }
    if (!track->isVideo()) {
        return Error{ErrorCode::NotFound, "track " + std::to_string(trackId) + " is not a video track"};
    }
    // The parameter sets and the NAL length prefix size both come from the
    // configuration record; hvcC for HEVC, avcC for the H.264 LRF proxy.
    std::vector<std::uint8_t> header;
    std::uint32_t lengthSize = 0;
    std::size_t headerNals = 0;
    if (const HevcConfig* hevc = track->hevc()) {
        header = hevc->toAnnexBHeader();
        lengthSize = hevc->nalLengthSize();
        headerNals = hevc->nalUnitCount();
    } else if (const AvcConfig* avc = track->avc()) {
        header = avc->toAnnexBHeader();
        lengthSize = avc->nalLengthSize();
        headerNals = avc->sps.size() + avc->pps.size();
    } else {
        return Error{ErrorCode::Unsupported, "track " + std::to_string(trackId) +
                                                 " has no hvcC/avcC configuration record (sample entry '" +
                                                 track->sampleEntry.str() + "')"};
    }
    if (lengthSize < 1 || lengthSize > 4) {
        return Error{ErrorCode::Malformed, "NAL length size " + std::to_string(lengthSize) + " is out of range"};
    }
    if (header.empty()) {
        return Error{ErrorCode::Malformed, "track " + std::to_string(trackId) + " carries no parameter sets"};
    }
    const std::uint32_t sampleCount = track->samples.count();
    if (sampleCount == 0) {
        return Error{ErrorCode::NotFound, "track " + std::to_string(trackId) + " has no samples"};
    }

    // ---- write ----------------------------------------------------------------
    OutputFile out(outPath);
    if (!out.isOpen()) {
        return Error{ErrorCode::Io, "cannot create " + log::safe(outPath.string())};
    }
    AnnexBStats stats;
    stats.trackId = trackId;
    stats.parameterSetNals = static_cast<std::uint32_t>(headerNals);
    if (!out.write(header)) {
        return Error{ErrorCode::Io, "write failed: " + log::safe(outPath.string())};
    }
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        OSV_TRY_ASSIGN(const ByteSpan sample, readSample(file.movie(), trackId, i));
        // Walk the length-prefixed NAL units, replacing every prefix by a
        // start code.  A length that runs past the sample is a corrupt file.
        std::uint64_t pos = 0;
        std::uint32_t nalsInSample = 0;
        while (pos + lengthSize <= sample.size()) {
            const std::uint32_t len = readNalLength(sample, pos, lengthSize);
            pos += lengthSize;
            if (len == 0 || len > sample.size() - pos) {
                return Error{ErrorCode::Malformed, "sample " + std::to_string(i) + ": NAL length " +
                                                       std::to_string(len) + " runs past the sample (" +
                                                       std::to_string(sample.size()) + " bytes)"};
            }
            if (!out.write(kStartCode.data(), kStartCode.size()) || !out.write(sample.sub(pos, len))) {
                return Error{ErrorCode::Io, "write failed: " + log::safe(outPath.string())};
            }
            pos += len;
            ++nalsInSample;
        }
        if (nalsInSample == 0) {
            return Error{ErrorCode::Malformed, "sample " + std::to_string(i) + " contains no NAL units"};
        }
        if (pos != sample.size()) {
            // Trailing bytes that do not form a NAL: tolerated, but worth a note.
            log::warn("extract: sample {} of track {} has {} trailing bytes after the last NAL", i, trackId,
                      sample.size() - pos);
        }
        stats.sampleNals += nalsInSample;
        ++stats.samples;
    }
    if (!out.finish()) {
        return Error{ErrorCode::Io, "write failed: " + log::safe(outPath.string())};
    }
    stats.bytesWritten = out.written();
    return stats;
}

// =============================================================================
//  writeAdtsAudio
// =============================================================================
Result<AdtsStats> writeAdtsAudio(const std::filesystem::path& osvPath, std::uint32_t trackId,
                                 const std::filesystem::path& outPath) {
    if (osvPath.empty() || outPath.empty()) {
        return Error{ErrorCode::InvalidArgument, "input and output paths are required"};
    }
    OSV_TRY_ASSIGN(const OsvFile file, OsvFile::open(osvPath));

    // ---- select the track -----------------------------------------------------
    const TrackInfo* track = nullptr;
    if (trackId == 0) {
        // First mp4a track in file order.
        for (const TrackInfo* candidate : file.tracksOfKind(TrackKind::Audio)) {
            if (candidate && candidate->sampleEntry == Fourcc{"mp4a"}) {
                track = candidate;
                break;
            }
        }
        if (!track) {
            return Error{ErrorCode::NotFound, "the file has no mp4a audio track"};
        }
        trackId = track->trackId;
    } else {
        track = file.track(trackId);
        if (!track) {
            return Error{ErrorCode::NotFound, "no track with id " + std::to_string(trackId)};
        }
        if (!track->isAudio()) {
            return Error{ErrorCode::NotFound, "track " + std::to_string(trackId) + " is not an audio track"};
        }
    }

    // ---- AudioSpecificConfig ------------------------------------------------
    // ADTS needs three things from it: the audio object type (profile), the
    // sampling frequency index and the channel configuration.
    if (!track->audio || !track->audio->esds) {
        return Error{ErrorCode::Unsupported, "track " + std::to_string(trackId) + " has no esds descriptor"};
    }
    const EsdsInfo& esds = *track->audio->esds;
    const ByteSpan asc = esds.decoderSpecificInfo;
    if (asc.size() < 2) {
        return Error{ErrorCode::Unsupported, "track " + std::to_string(trackId) + " has no AudioSpecificConfig"};
    }
    if (esds.objectTypeIndication != 0 && esds.objectTypeIndication != 0x40 && esds.objectTypeIndication != 0x66 &&
        esds.objectTypeIndication != 0x67 && esds.objectTypeIndication != 0x68) {
        return Error{ErrorCode::Unsupported, "track " + std::to_string(trackId) + " is not AAC (objectTypeIndication 0x" +
                                                 std::to_string(esds.objectTypeIndication) + ")"};
    }
    const std::uint8_t aot = esds.audioObjectType();
    // ADTS carries a 2-bit profile = audioObjectType - 1, so only Main / LC /
    // SSR / LTP fit.  The Osmo 360 writes AAC-LC (2).
    if (aot < 1 || aot > 4) {
        return Error{ErrorCode::Unsupported, "audio object type " + std::to_string(aot) +
                                                 " cannot be carried in ADTS (only AAC Main/LC/SSR/LTP)"};
    }
    // Bits: aot(5) | samplingFrequencyIndex(4) | channelConfiguration(4).
    const std::uint8_t samplingIndex = static_cast<std::uint8_t>(((asc[0] & 0x07u) << 1) | (asc[1] >> 7));
    if (samplingIndex >= 13) {
        return Error{ErrorCode::Unsupported, "sampling frequency index " + std::to_string(samplingIndex) +
                                                 " (reserved / explicit) cannot be carried in ADTS"};
    }
    const std::uint8_t channelConfig = static_cast<std::uint8_t>((asc[1] >> 3) & 0x0Fu);
    if (channelConfig > 7) {
        return Error{ErrorCode::Unsupported, "channel configuration " + std::to_string(channelConfig) +
                                                 " does not fit the 3-bit ADTS field"};
    }
    const std::uint32_t sampleCount = track->samples.count();
    if (sampleCount == 0) {
        return Error{ErrorCode::NotFound, "track " + std::to_string(trackId) + " has no samples"};
    }

    // ---- write ----------------------------------------------------------------
    OutputFile out(outPath);
    if (!out.isOpen()) {
        return Error{ErrorCode::Io, "cannot create " + log::safe(outPath.string())};
    }
    AdtsStats stats;
    stats.trackId = trackId;
    stats.audioObjectType = aot;
    stats.samplingIndex = samplingIndex;
    stats.sampleRate = kSamplingFrequencies[samplingIndex];
    stats.channelConfig = channelConfig;
    const std::uint8_t profile = static_cast<std::uint8_t>(aot - 1);
    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        OSV_TRY_ASSIGN(const ByteSpan sample, readSample(file.movie(), trackId, i));
        // frame_length is 13 bits and counts the 7-byte header.
        const std::uint64_t frameLength = 7u + sample.size();
        if (sample.empty() || frameLength >= (1u << 13)) {
            return Error{ErrorCode::Malformed, "AAC frame " + std::to_string(i) + " has an impossible size (" +
                                                   std::to_string(sample.size()) + " bytes)"};
        }
        // ADTS fixed + variable header, no CRC (protection_absent = 1):
        //   syncword(12) id(1)=0 layer(2)=0 protection_absent(1)=1
        //   profile(2) sampling_frequency_index(4) private(1)=0
        //   channel_configuration(3) original(1) home(1)
        //   copyright_id_bit(1) copyright_id_start(1) frame_length(13)
        //   adts_buffer_fullness(11)=0x7FF (VBR) number_of_raw_data_blocks(2)=0
        std::array<std::uint8_t, 7> hdr{};
        hdr[0] = 0xFF;
        hdr[1] = 0xF1;
        hdr[2] = static_cast<std::uint8_t>((profile << 6) | (samplingIndex << 2) | ((channelConfig >> 2) & 0x01u));
        hdr[3] = static_cast<std::uint8_t>(((channelConfig & 0x03u) << 6) | ((frameLength >> 11) & 0x03u));
        hdr[4] = static_cast<std::uint8_t>((frameLength >> 3) & 0xFFu);
        hdr[5] = static_cast<std::uint8_t>(((frameLength & 0x07u) << 5) | 0x1Fu);
        hdr[6] = 0xFC;
        if (!out.write(hdr.data(), hdr.size()) || !out.write(sample)) {
            return Error{ErrorCode::Io, "write failed: " + log::safe(outPath.string())};
        }
        ++stats.samples;
    }
    if (!out.finish()) {
        return Error{ErrorCode::Io, "write failed: " + log::safe(outPath.string())};
    }
    stats.bytesWritten = out.written();
    return stats;
}

}  // namespace osv::video
