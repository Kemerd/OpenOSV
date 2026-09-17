// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MovieInfo: the parsed 'moov' plus the top-level structure of one ISO BMFF
// span - tracks, movie header, DJI's user data and index table, the cover
// images and the trailing 'camd' box.  Every ByteSpan aliases the span the
// movie was parsed from (`source`), nothing is copied.
#pragma once

#include "osv/container/Box.h"
#include "osv/container/IndexTable.h"
#include "osv/container/TrackInfo.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace osv {

/// Parsed 'ftyp'.
struct FileType {
    Fourcc majorBrand;                    ///< 'isom' for DJI files.
    std::uint32_t minorVersion = 0;
    std::vector<Fourcc> compatibleBrands; ///< 'isom', 'iso2', 'mp41' (+ 'avc1' for LRF).
};

/// Parsed 'mvhd'.
struct MovieHeader {
    std::uint8_t version = 0;
    std::uint64_t creationTime = 0;      ///< Seconds since 1904-01-01 UTC.
    std::uint64_t modificationTime = 0;
    std::uint32_t timescale = 0;         ///< Movie timescale (60000 for DJI).
    std::uint64_t duration = 0;          ///< Movie duration in timescale units.
    double rate = 1.0;                   ///< 16.16 preferred rate.
    double volume = 1.0;                 ///< 8.8 preferred volume.
    std::uint32_t nextTrackId = 0;
};

/// One item of a 'meta'/'ilst' metadata list (iTunes style).
struct IlstItem {
    Fourcc name;                     ///< Item name, e.g. 'covr', 'snal', 'tnal', (c)'too'.
    std::uint32_t dataType = 0;      ///< 'data' type indicator (1 UTF-8, 13 JPEG, 14 PNG, 21 int).
    std::uint32_t locale = 0;        ///< 'data' locale indicator.
    std::uint64_t itemOffset = 0;    ///< Absolute offset of the item box.
    std::uint64_t dataOffset = 0;    ///< Absolute offset of the value bytes.
    ByteSpan data;                   ///< The value bytes.

    /// Value as text when dataType is UTF-8 (1) or implicit (0); empty otherwise.
    [[nodiscard]] std::string asString() const;
};

/// One entry of a Windows Media 'Xtra' box.
struct XtraEntry {
    std::string name;                ///< e.g. "WM/Category".
    std::uint16_t valueType = 0;     ///< 8 = UTF-16 string.
    ByteSpan value;                  ///< Raw value bytes.
    std::string text;                ///< UTF-16 strings converted to ASCII-safe text.
};

/// DJI 'udta' contents.
struct UserData {
    ByteSpan uid;                          ///< (c)uid raw payload.
    std::optional<std::uint32_t> dbpm;     ///< 'dbpm' value.
    std::optional<std::uint32_t> dbcm;     ///< 'dbcm' value.
    std::string btec;                      ///< 'btec' ASCII ("beauty_enable=0;...").
    std::string fsid;                      ///< 'fsid' ASCII (original camera path).
    std::string tool;                      ///< ilst (c)too value ("Osmo 360").
    std::vector<IlstItem> ilst;            ///< Every ilst item.
    std::vector<XtraEntry> xtra;           ///< Parsed 'Xtra' entries.
    ByteSpan xtraRaw;                      ///< The whole 'Xtra' payload.
    std::vector<BoxHeader> unknownBoxes;   ///< udta children we do not interpret.

    /// The ilst item with the given name, or null.
    [[nodiscard]] const IlstItem* ilstItem(Fourcc name) const noexcept;

    /// The Xtra entry with the given name, or null.
    [[nodiscard]] const XtraEntry* xtraEntry(const std::string& name) const noexcept;
};

/// Cover art carried in udta/meta/ilst.  All spans point at raw JPEG bytes.
struct CoverImages {
    ByteSpan covr;   ///< 688x344 JPEG (both lenses side by side).
    ByteSpan snal;   ///< Identical to covr in the sample clip.
    ByteSpan tnal;   ///< 160x80 thumbnail.
};

/// The parsed movie.
struct MovieInfo {
    ByteSpan source;                       ///< The span the movie was parsed from.
    FileType fileType;
    bool hasFileType = false;
    MovieHeader header;
    bool hasMovieHeader = false;
    std::vector<TrackInfo> tracks;         ///< In 'trak' order.
    UserData udta;
    CoverImages covers;
    IndexTable indexTable;
    std::optional<BoxHeader> moovBox;
    std::optional<BoxHeader> mdatBox;      ///< First 'mdat'.
    std::optional<BoxHeader> camdBox;      ///< Trailing DJI 'camd' box (nested MP4).
    std::vector<BoxHeader> topLevel;       ///< Every top-level box in order.
    WarningList warnings;                  ///< Diagnostics collected while parsing.

    /// Movie timescale (0 without mvhd).
    [[nodiscard]] std::uint32_t timescale() const noexcept { return header.timescale; }

    /// Movie duration in timescale units.
    [[nodiscard]] std::uint64_t duration() const noexcept { return header.duration; }

    /// Movie duration in seconds (0 when timescale is 0).
    [[nodiscard]] double durationSeconds() const noexcept;

    /// Track with the given track_ID, or null.
    [[nodiscard]] const TrackInfo* track(std::uint32_t trackId) const noexcept;

    /// Tracks of one kind in file order.
    [[nodiscard]] std::vector<const TrackInfo*> tracksOfKind(TrackKind kind) const;

    /// The nested movie bytes inside 'camd' (empty when absent).
    [[nodiscard]] ByteSpan camdSpan() const noexcept;

    /// Top-level box of the given type, or null.
    [[nodiscard]] const BoxHeader* topLevelBox(Fourcc type) const noexcept;
};

/// Fetch the bytes of sample `index` of track `trackId` from `movie.source`.
/// The span aliases the source; nothing is copied.  NotFound for an unknown
/// track or index, Truncated when the sample lies outside the source.
[[nodiscard]] Result<ByteSpan> readSample(const MovieInfo& movie, std::uint32_t trackId, std::uint32_t index);

/// Same, given the track directly.
[[nodiscard]] Result<ByteSpan> readSample(ByteSpan source, const TrackInfo& track, std::uint32_t index);

}  // namespace osv
