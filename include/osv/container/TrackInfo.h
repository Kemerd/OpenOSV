// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// TrackInfo: everything the rest of the library needs to know about one
// 'trak' box - identity, handler, sample entry (codec configuration, colour
// tagging, audio format), timing and the sample table.
//
// Track kinds map DJI's private tracks onto an enum so callers do not have to
// compare four-character codes: the two 'djmd' tracks carry the protobuf
// metadata (one sample per video frame), the 'dbgi' tracks are debug data.
#pragma once

#include "osv/container/Box.h"
#include "osv/container/HevcConfig.h"
#include "osv/container/SampleTable.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/Result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace osv {

/// Coarse classification of a track.
enum class TrackKind : std::uint8_t {
    Video, ///< 'vide' handler (hvc1/hev1/avc1 ...).
    Audio, ///< 'soun' handler (mp4a ...).
    Djmd,  ///< DJI metadata track: 'meta' handler with a 'djmd' sample entry.
    Dbgi,  ///< DJI debug track: 'meta' handler with a 'dbgi' sample entry.
    Other  ///< Anything else (timed text, hint, unknown private data).
};

/// Stable name of a TrackKind for logs and JSON.
[[nodiscard]] constexpr const char* trackKindName(TrackKind kind) noexcept {
    switch (kind) {
    case TrackKind::Video: return "Video";
    case TrackKind::Audio: return "Audio";
    case TrackKind::Djmd: return "Djmd";
    case TrackKind::Dbgi: return "Dbgi";
    case TrackKind::Other: return "Other";
    }
    return "Unknown";
}

/// Parsed 'colr' box.  Only the 'nclx' (ISO) and 'nclc' (QuickTime) forms
/// carry the three code points; ICC forms keep the raw profile bytes.
struct ColrNclx {
    Fourcc colourType;              ///< 'nclx', 'nclc', 'rICC' or 'prof'.
    std::uint16_t primaries = 0;    ///< ITU-T H.273 ColourPrimaries (1 = BT.709, 9 = BT.2020).
    std::uint16_t transfer = 0;     ///< TransferCharacteristics (1 = BT.709, 16 = PQ, 18 = HLG).
    std::uint16_t matrix = 0;       ///< MatrixCoefficients (1 = BT.709, 9 = BT.2020 NCL).
    bool fullRange = false;         ///< nclx full_range_flag (nclc has none -> false).
    ByteSpan raw;                   ///< The whole box payload (ICC profile for rICC/prof).

    /// Parse the payload of a 'colr' box.
    [[nodiscard]] static Result<ColrNclx> parse(ByteSpan payload);

    /// True when the box carries code points (nclx or nclc).
    [[nodiscard]] bool hasCodePoints() const noexcept {
        return colourType == Fourcc{"nclx"} || colourType == Fourcc{"nclc"};
    }
};

/// Parsed 'pasp' pixel aspect ratio box.
struct PixelAspect {
    std::uint32_t hSpacing = 1;
    std::uint32_t vSpacing = 1;
};

/// Fields of interest from an 'esds' box (MPEG-4 elementary stream
/// descriptor) inside an 'mp4a' sample entry.
struct EsdsInfo {
    std::uint16_t esId = 0;
    std::uint8_t objectTypeIndication = 0; ///< 0x40 = MPEG-4 audio (AAC), 0x6B = MP3.
    std::uint8_t streamType = 0;           ///< 5 = audio stream.
    std::uint32_t bufferSizeDb = 0;
    std::uint32_t maxBitrate = 0;
    std::uint32_t avgBitrate = 0;
    ByteSpan decoderSpecificInfo;          ///< AudioSpecificConfig for AAC (empty when absent).
    ByteSpan raw;                          ///< The whole esds payload after version/flags.

    /// AAC audioObjectType from the first five bits of the AudioSpecificConfig
    /// (2 = AAC-LC), or 0 when there is no decoder specific info.
    [[nodiscard]] std::uint8_t audioObjectType() const noexcept;
};

/// Visual sample entry fields (hvc1 / hev1 / avc1 / ...).
struct VideoSampleEntry {
    std::uint16_t width = 0;               ///< Coded width from the sample entry.
    std::uint16_t height = 0;              ///< Coded height from the sample entry.
    std::uint32_t horizResolution = 0;     ///< 16.16 dpi (usually 72.0).
    std::uint32_t vertResolution = 0;
    std::uint16_t frameCount = 0;          ///< Frames per sample (usually 1).
    std::string compressorName;            ///< Pascal string, may be empty.
    std::uint16_t depth = 0;               ///< Usually 0x18.
    std::optional<HevcConfig> hevc;        ///< From 'hvcC'.
    std::optional<AvcConfig> avc;          ///< From 'avcC'.
    std::optional<ColrNclx> colr;          ///< From 'colr'.
    std::optional<PixelAspect> pasp;       ///< From 'pasp'.
};

/// Audio sample entry fields (mp4a / ...), versions 0, 1 and 2.
struct AudioSampleEntry {
    std::uint16_t version = 0;
    std::uint16_t channelCount = 0;
    std::uint16_t sampleSize = 0;          ///< Bits per sample (16 for AAC entries).
    double sampleRate = 0.0;               ///< Hz.
    std::optional<EsdsInfo> esds;
};

/// One entry of an edit list ('elst').
struct EditListEntry {
    std::uint64_t segmentDuration = 0;     ///< Movie timescale units.
    std::int64_t mediaTime = -1;           ///< Media timescale units; -1 = empty edit.
    std::int16_t mediaRateInteger = 1;
    std::int16_t mediaRateFraction = 0;
};

/// One parsed 'trak'.
struct TrackInfo {
    // ---- identity -------------------------------------------------------
    std::uint32_t trackId = 0;             ///< tkhd track_ID.
    std::uint32_t tkhdFlags = 0;           ///< tkhd flags (bit 0 enabled, bit 1 in movie, bit 2 in preview).
    std::uint8_t tkhdVersion = 0;
    std::uint64_t creationTime = 0;        ///< mdhd creation time (seconds since 1904).
    std::uint64_t modificationTime = 0;
    std::uint64_t movieDuration = 0;       ///< tkhd duration in movie timescale units.
    std::uint16_t layer = 0;
    std::uint16_t alternateGroup = 0;
    double volume = 0.0;                   ///< tkhd 8.8 volume.
    double width = 0.0;                    ///< tkhd 16.16 presentation width.
    double height = 0.0;                   ///< tkhd 16.16 presentation height.
    std::array<double, 9> matrix{};        ///< tkhd transformation matrix (a b u c d v x y w).

    // ---- media ----------------------------------------------------------
    Fourcc handler;                        ///< hdlr handler_type ('vide', 'soun', 'meta').
    std::string handlerName;               ///< hdlr name ("VideoHandler", "CAM meta").
    std::uint32_t timescale = 0;           ///< mdhd timescale.
    std::uint64_t duration = 0;            ///< mdhd duration (media timescale units).
    std::string language;                  ///< mdhd ISO-639-2/T code ("und").
    TrackKind kind = TrackKind::Other;

    // ---- sample description ---------------------------------------------
    std::uint32_t sampleEntryCount = 0;    ///< stsd entry_count.
    Fourcc sampleEntry;                    ///< First stsd entry type ('hvc1', 'mp4a', 'djmd').
    ByteSpan sampleEntryRaw;               ///< Whole first sample entry box.
    std::uint16_t dataReferenceIndex = 0;
    std::optional<VideoSampleEntry> video;
    std::optional<AudioSampleEntry> audio;

    // ---- timing / samples -----------------------------------------------
    std::vector<EditListEntry> editList;   ///< From edts/elst (empty when absent).
    SampleTable samples;
    ByteSpan trakBox;                      ///< The whole 'trak' box (diagnostics).

    /// tkhd flag bit 0.
    [[nodiscard]] bool enabled() const noexcept { return (tkhdFlags & 0x1u) != 0; }

    /// tkhd flag bit 1.
    [[nodiscard]] bool inMovie() const noexcept { return (tkhdFlags & 0x2u) != 0; }

    /// Convenience accessors for the common video queries.
    [[nodiscard]] bool isVideo() const noexcept { return kind == TrackKind::Video; }
    [[nodiscard]] bool isAudio() const noexcept { return kind == TrackKind::Audio; }
    [[nodiscard]] bool isDjmd() const noexcept { return kind == TrackKind::Djmd; }

    /// Coded width/height from the sample entry (0 for non-video tracks).
    [[nodiscard]] std::uint32_t codedWidth() const noexcept { return video ? video->width : 0; }
    [[nodiscard]] std::uint32_t codedHeight() const noexcept { return video ? video->height : 0; }

    /// HEVC configuration when the track is hvc1/hev1.
    [[nodiscard]] const HevcConfig* hevc() const noexcept { return (video && video->hevc) ? &*video->hevc : nullptr; }

    /// AVC configuration when the track is avc1/avc3.
    [[nodiscard]] const AvcConfig* avc() const noexcept { return (video && video->avc) ? &*video->avc : nullptr; }

    /// Colour tagging when a 'colr' box was present.
    [[nodiscard]] const ColrNclx* colr() const noexcept { return (video && video->colr) ? &*video->colr : nullptr; }

    /// Media time of the first non-empty edit (0 when there is no edit list).
    [[nodiscard]] std::int64_t editMediaTime() const noexcept;

    /// Frames per second derived from the sample table (count * timescale /
    /// total duration).  0 when it cannot be computed.
    [[nodiscard]] double frameRate() const noexcept;

    /// Track duration in seconds from mdhd (0 when timescale is 0).
    [[nodiscard]] double durationSeconds() const noexcept;
};

/// Decide the TrackKind from the handler type and the sample entry fourcc.
[[nodiscard]] TrackKind classifyTrack(Fourcc handler, Fourcc sampleEntry) noexcept;

}  // namespace osv
