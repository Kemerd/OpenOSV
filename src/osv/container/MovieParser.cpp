// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OsvFile::parseMovie - turns an ISO BMFF span into a MovieInfo.
//
// Layout of a DJI .OSV (verified against the sample clip):
//   ftyp | free | free (index table) | mdat | moov | camd
// The parser walks the top level once, descends into moov for the tracks and
// user data, and records the other boxes so callers can reason about the file
// without a second pass.  All spans alias the input.

#include "osv/container/OsvFile.h"

#include "osv/container/BoxWalker.h"
#include "osv/container/HevcConfig.h"
#include "osv/container/IndexTable.h"
#include "osv/container/SampleTable.h"
#include "osv/core/ByteReader.h"
#include "osv/core/Log.h"

#include <cstring>
#include <string>
#include <utility>

namespace osv {

namespace {

/// Bytes of fixed fields in a VisualSampleEntry body before its child boxes.
constexpr std::uint64_t kVisualEntryFixedBytes = 78;

/// Bytes of fixed fields in an AudioSampleEntry body for versions 0, 1, 2.
constexpr std::uint64_t kAudioEntryFixedBytesV0 = 28;
constexpr std::uint64_t kAudioEntryFixedBytesV1 = 44;
constexpr std::uint64_t kAudioEntryFixedBytesV2 = 64;

/// Decode the packed ISO-639-2/T language of mdhd (three 5-bit letters).
std::string decodeLanguage(std::uint16_t packed) {
    std::string lang(3, '?');
    for (int i = 0; i < 3; ++i) {
        const std::uint16_t v = static_cast<std::uint16_t>((packed >> (10 - 5 * i)) & 0x1Fu);
        const char c = static_cast<char>(v + 0x60);
        lang[static_cast<std::size_t>(i)] = (c >= 'a' && c <= 'z') ? c : '?';
    }
    return lang;
}

/// Convert UTF-16LE bytes to an ASCII-safe std::string (non-ASCII -> '?'),
/// stopping at the first NUL code unit.
std::string utf16leToAscii(ByteSpan bytes) {
    std::string out;
    out.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const std::uint16_t unit = static_cast<std::uint16_t>(bytes[i] | (static_cast<std::uint16_t>(bytes[i + 1]) << 8));
        if (unit == 0) {
            break;
        }
        out.push_back((unit >= 0x20 && unit < 0x7F) ? static_cast<char>(unit) : '?');
    }
    return out;
}

/// Trim trailing NUL bytes from a string read out of a box.
std::string trimNul(std::string text) {
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

/// Read an MPEG-4 descriptor length (1..4 bytes, 7 bits each, MSB continues).
bool readDescriptorLength(ByteReader& reader, std::uint32_t& length) {
    length = 0;
    for (int i = 0; i < 4; ++i) {
        std::uint8_t b = 0;
        if (!reader.u8(b)) {
            return false;
        }
        length = (length << 7) | (b & 0x7Fu);
        if ((b & 0x80u) == 0) {
            return true;
        }
    }
    // Four continuation bytes without a terminator is not a valid length.
    return false;
}

/// Everything the parser needs while walking one movie.
class MovieParser {
public:
    MovieParser(ByteSpan root, MovieInfo& movie) : m_root(root), m_movie(movie), m_warnings(&movie.warnings) {}

    /// Parse the whole span.  See OsvFile::parseMovie for the contract.
    Status run();

private:
    // ---- top level ------------------------------------------------------
    void parseFtyp(const BoxHeader& box);
    void parseMoov(const BoxHeader& box);
    void parseMvhd(const BoxHeader& box);

    // ---- tracks ---------------------------------------------------------
    Result<TrackInfo> parseTrak(const BoxHeader& box);
    Status parseTkhd(const BoxHeader& box, TrackInfo& track);
    Status parseMdhd(const BoxHeader& box, TrackInfo& track);
    void parseHdlr(const BoxHeader& box, TrackInfo& track);
    void parseElst(const BoxHeader& box, TrackInfo& track);
    Status parseStsd(const BoxHeader& box, TrackInfo& track);
    void parseVisualEntry(const BoxHeader& entry, TrackInfo& track);
    void parseAudioEntry(const BoxHeader& entry, TrackInfo& track);
    std::optional<EsdsInfo> parseEsds(const BoxHeader& box);

    // ---- user data ------------------------------------------------------
    void parseUdta(const BoxHeader& box);
    void parseIlst(const BoxHeader& box);
    void parseXtra(const BoxHeader& box);

    ByteSpan m_root;
    MovieInfo& m_movie;
    WarningList* m_warnings;
};

// -----------------------------------------------------------------------------
//  Top level
// -----------------------------------------------------------------------------

Status MovieParser::run() {
    m_movie.source = m_root;

    // A file that cannot hold one box header is not worth walking.
    if (m_root.size() < 8) {
        return failStatus(ErrorCode::Truncated,
                          "input is " + std::to_string(m_root.size()) + " bytes, smaller than a box header");
    }
    // Reject non-ISO-BMFF input up front: the first box must be one of the
    // types that can legally open a file.  This is what turns random bytes
    // into a clean Malformed instead of a wander through garbage sizes.
    {
        ByteReader peek(m_root.sub(4, 4));
        Fourcc first;
        peek.fourcc(first);
        if (!BoxWalker::isKnownTopLevelType(first)) {
            return failStatus(ErrorCode::Malformed, "not an ISO BMFF file: first box type is '" + first.str() + "' (" +
                                                        first.hex() + ")");
        }
    }

    bool sawTruncation = false;
    std::uint64_t lastEnd = 0;
    bool seenMediaBox = false;  // once mdat/moov appeared, later 'free' boxes are not the index table

    BoxWalker::forEachTopLevel(m_root, m_warnings, [&](const BoxHeader& box) {
        m_movie.topLevel.push_back(box);
        lastEnd = box.end();
        if (box.truncated) {
            sawTruncation = true;
        }

        if (box.type == Fourcc{"ftyp"}) {
            parseFtyp(box);
        } else if (box.type == Fourcc{"moov"}) {
            seenMediaBox = true;
            if (m_movie.moovBox) {
                addWarning(m_warnings, "second 'moov' box at " + std::to_string(box.offset) + " ignored");
            } else {
                m_movie.moovBox = box;
                parseMoov(box);
            }
        } else if (box.type == Fourcc{"mdat"}) {
            seenMediaBox = true;
            if (!m_movie.mdatBox) {
                m_movie.mdatBox = box;
            }
        } else if (box.type == Fourcc{"camd"}) {
            if (m_movie.camdBox) {
                addWarning(m_warnings, "second 'camd' box at " + std::to_string(box.offset) + " ignored");
            } else {
                m_movie.camdBox = box;
            }
        } else if (box.type == Fourcc{"free"} || box.type == Fourcc{"skip"}) {
            // DJI's index table lives in a 'free' box between ftyp and mdat.
            if (!m_movie.indexTable.present && !seenMediaBox && !box.truncated) {
                IndexTable table = IndexTable::parse(m_root, box, m_warnings);
                if (table.present) {
                    m_movie.indexTable = std::move(table);
                }
            }
        }
        return true;
    });

    // Bytes after the last parsed box that could not be read as a box mean
    // the file ends mid-structure.
    const bool tailUnparsed = lastEnd < m_root.size();

    if (!m_movie.moovBox) {
        if (sawTruncation || tailUnparsed || m_movie.mdatBox) {
            return failStatus(ErrorCode::Truncated,
                              "no 'moov' box found; the file appears to end before the movie header was written");
        }
        return failStatus(ErrorCode::Malformed, "no 'moov' box found");
    }
    if (m_movie.moovBox->truncated && m_movie.tracks.empty()) {
        return failStatus(ErrorCode::Truncated, "'moov' box is truncated and no track could be recovered");
    }
    if (m_movie.tracks.empty()) {
        addWarning(m_warnings, "'moov' contains no usable track");
    }
    return okStatus();
}

void MovieParser::parseFtyp(const BoxHeader& box) {
    ByteReader reader(box.payload);
    FileType ft;
    if (!reader.fourcc(ft.majorBrand) || !reader.u32be(ft.minorVersion)) {
        addWarning(m_warnings, "'ftyp' box is too short");
        return;
    }
    // Compatible brands fill the rest of the box, four bytes each.
    Fourcc brand;
    while (reader.fourcc(brand)) {
        ft.compatibleBrands.push_back(brand);
    }
    m_movie.fileType = std::move(ft);
    m_movie.hasFileType = true;
}

void MovieParser::parseMoov(const BoxHeader& box) {
    BoxWalker::forEachChild(m_root, box, m_warnings, [&](const BoxHeader& child) {
        if (child.type == Fourcc{"mvhd"}) {
            parseMvhd(child);
        } else if (child.type == Fourcc{"trak"}) {
            Result<TrackInfo> track = parseTrak(child);
            if (track.ok()) {
                m_movie.tracks.push_back(std::move(track).value());
            } else {
                addWarning(m_warnings, "track " + child.describe() + " dropped: " + track.error().message);
            }
        } else if (child.type == Fourcc{"udta"}) {
            parseUdta(child);
        }
        // iods, meta, mvex etc. carry nothing we need.
        return true;
    });
}

void MovieParser::parseMvhd(const BoxHeader& box) {
    ByteReader reader(box.payload);
    MovieHeader h;
    h.version = box.version;
    bool ok = true;
    if (box.version == 1) {
        ok = reader.u64be(h.creationTime) && reader.u64be(h.modificationTime) && reader.u32be(h.timescale) &&
             reader.u64be(h.duration);
    } else {
        std::uint32_t c = 0, m = 0, d = 0;
        ok = reader.u32be(c) && reader.u32be(m) && reader.u32be(h.timescale) && reader.u32be(d);
        h.creationTime = c;
        h.modificationTime = m;
        h.duration = d;
    }
    if (!ok) {
        addWarning(m_warnings, "'mvhd' box is too short");
        return;
    }
    // rate 16.16, volume 8.8, 10 reserved, 36 matrix, 24 pre_defined, next_track_ID.
    double rate = 1.0;
    std::int16_t volume = 0x0100;
    if (reader.fixed1616be(rate)) {
        h.rate = rate;
    }
    if (reader.i16be(volume)) {
        h.volume = static_cast<double>(volume) / 256.0;
    }
    if (reader.skip(10 + 36 + 24)) {
        reader.u32be(h.nextTrackId);
    }
    m_movie.header = h;
    m_movie.hasMovieHeader = true;
}

// -----------------------------------------------------------------------------
//  Tracks
// -----------------------------------------------------------------------------

Result<TrackInfo> MovieParser::parseTrak(const BoxHeader& box) {
    TrackInfo track;
    track.trakBox = box.whole;

    std::optional<BoxHeader> tkhd = BoxWalker::findChild(m_root, box, Fourcc{"tkhd"}, m_warnings);
    if (!tkhd) {
        return Error{ErrorCode::Malformed, "no 'tkhd' box"};
    }
    OSV_TRY(parseTkhd(*tkhd, track));

    // Edit list is optional.
    if (std::optional<BoxHeader> elst =
            BoxWalker::findPath(m_root, box, {Fourcc{"edts"}, Fourcc{"elst"}}, m_warnings)) {
        parseElst(*elst, track);
    }

    std::optional<BoxHeader> mdia = BoxWalker::findChild(m_root, box, Fourcc{"mdia"}, m_warnings);
    if (!mdia) {
        return Error{ErrorCode::Malformed, "no 'mdia' box"};
    }
    std::optional<BoxHeader> mdhd = BoxWalker::findChild(m_root, *mdia, Fourcc{"mdhd"}, m_warnings);
    if (!mdhd) {
        return Error{ErrorCode::Malformed, "no 'mdhd' box"};
    }
    OSV_TRY(parseMdhd(*mdhd, track));

    if (std::optional<BoxHeader> hdlr = BoxWalker::findChild(m_root, *mdia, Fourcc{"hdlr"}, m_warnings)) {
        parseHdlr(*hdlr, track);
    } else {
        addWarning(m_warnings, "track " + std::to_string(track.trackId) + " has no 'hdlr' box");
    }

    std::optional<BoxHeader> stbl = BoxWalker::findPath(m_root, *mdia, {Fourcc{"minf"}, Fourcc{"stbl"}}, m_warnings);
    if (!stbl) {
        return Error{ErrorCode::Malformed, "no 'minf/stbl' box"};
    }
    std::optional<BoxHeader> stsd = BoxWalker::findChild(m_root, *stbl, Fourcc{"stsd"}, m_warnings);
    if (!stsd) {
        return Error{ErrorCode::Malformed, "no 'stsd' box"};
    }
    OSV_TRY(parseStsd(*stsd, track));

    // The sample table is what makes the track usable; its parser reports
    // the exact reason when it cannot be built.
    OSV_TRY_ASSIGN(track.samples, SampleTable::parse(m_root, *stbl, m_warnings));

    track.kind = classifyTrack(track.handler, track.sampleEntry);
    return track;
}

Status MovieParser::parseTkhd(const BoxHeader& box, TrackInfo& track) {
    ByteReader reader(box.payload);
    track.tkhdVersion = box.version;
    track.tkhdFlags = box.flags;
    bool ok = true;
    std::uint32_t reserved = 0;
    if (box.version == 1) {
        ok = reader.u64be(track.creationTime) && reader.u64be(track.modificationTime) && reader.u32be(track.trackId) &&
             reader.u32be(reserved) && reader.u64be(track.movieDuration);
    } else {
        std::uint32_t c = 0, m = 0, d = 0;
        ok = reader.u32be(c) && reader.u32be(m) && reader.u32be(track.trackId) && reader.u32be(reserved) &&
             reader.u32be(d);
        track.creationTime = c;
        track.modificationTime = m;
        track.movieDuration = d;
    }
    if (!ok) {
        return failStatus(ErrorCode::Truncated, "'tkhd' box is too short");
    }
    // reserved(8) layer(2) alternate_group(2) volume(2) reserved(2) matrix(36) width(4) height(4)
    std::int16_t volume = 0;
    std::uint16_t reserved16 = 0;
    if (!reader.skip(8) || !reader.u16be(track.layer) || !reader.u16be(track.alternateGroup) ||
        !reader.i16be(volume) || !reader.u16be(reserved16)) {
        // Older or exotic writers may stop here; the identity is already known.
        addWarning(m_warnings, "'tkhd' of track " + std::to_string(track.trackId) + " ends before its matrix");
        return okStatus();
    }
    track.volume = static_cast<double>(volume) / 256.0;
    for (std::size_t i = 0; i < 9; ++i) {
        std::int32_t raw = 0;
        if (!reader.i32be(raw)) {
            return okStatus();
        }
        // Entries u, v, w (indices 2, 5, 8) are 2.30 fixed point, the rest 16.16.
        const double scale = (i == 2 || i == 5 || i == 8) ? 1073741824.0 : 65536.0;
        track.matrix[i] = static_cast<double>(raw) / scale;
    }
    double w = 0.0, h = 0.0;
    if (reader.fixed1616be(w) && reader.fixed1616be(h)) {
        track.width = w;
        track.height = h;
    }
    return okStatus();
}

Status MovieParser::parseMdhd(const BoxHeader& box, TrackInfo& track) {
    ByteReader reader(box.payload);
    bool ok = true;
    if (box.version == 1) {
        std::uint64_t c = 0, m = 0;
        ok = reader.u64be(c) && reader.u64be(m) && reader.u32be(track.timescale) && reader.u64be(track.duration);
        // tkhd already filled creation/modification; mdhd values are the
        // media ones and take precedence when present.
        if (ok) {
            track.creationTime = c;
            track.modificationTime = m;
        }
    } else {
        std::uint32_t c = 0, m = 0, d = 0;
        ok = reader.u32be(c) && reader.u32be(m) && reader.u32be(track.timescale) && reader.u32be(d);
        if (ok) {
            track.creationTime = c;
            track.modificationTime = m;
            track.duration = d;
        }
    }
    if (!ok) {
        return failStatus(ErrorCode::Truncated, "'mdhd' box is too short");
    }
    if (track.timescale == 0) {
        addWarning(m_warnings, "track " + std::to_string(track.trackId) + " has a zero media timescale");
    }
    std::uint16_t language = 0;
    if (reader.u16be(language)) {
        track.language = decodeLanguage(language);
    }
    return okStatus();
}

void MovieParser::parseHdlr(const BoxHeader& box, TrackInfo& track) {
    ByteReader reader(box.payload);
    std::uint32_t preDefined = 0;
    if (!reader.u32be(preDefined) || !reader.fourcc(track.handler) || !reader.skip(12)) {
        addWarning(m_warnings, "'hdlr' of track " + std::to_string(track.trackId) + " is too short");
        return;
    }
    // Name: ISO says NUL-terminated UTF-8; old QuickTime writers use a Pascal
    // string (length byte first).  Detect the latter by its length byte.
    ByteSpan nameBytes = box.payload.sub(reader.pos());
    std::string name;
    if (!nameBytes.empty() && nameBytes[0] != 0 && nameBytes[0] == nameBytes.size() - 1) {
        name = nameBytes.sub(1).toString();
    } else {
        name = nameBytes.toString();
    }
    track.handlerName = trimNul(std::move(name));
    // Keep console output 7-bit clean.
    track.handlerName = log::safe(track.handlerName);
}

void MovieParser::parseElst(const BoxHeader& box, TrackInfo& track) {
    ByteReader reader(box.payload);
    std::uint32_t count = 0;
    if (!reader.u32be(count)) {
        addWarning(m_warnings, "'elst' of track " + std::to_string(track.trackId) + " has no entry count");
        return;
    }
    const std::uint64_t entrySize = box.version == 1 ? 20 : 12;
    if (static_cast<std::uint64_t>(count) * entrySize > reader.remaining()) {
        addWarning(m_warnings, "'elst' of track " + std::to_string(track.trackId) + " declares " +
                                   std::to_string(count) + " entries that do not fit in the box");
        return;
    }
    track.editList.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        EditListEntry edit;
        bool ok = true;
        if (box.version == 1) {
            ok = reader.u64be(edit.segmentDuration) && reader.i64be(edit.mediaTime);
        } else {
            std::uint32_t d = 0;
            std::int32_t t = 0;
            ok = reader.u32be(d) && reader.i32be(t);
            edit.segmentDuration = d;
            edit.mediaTime = t;
        }
        ok = ok && reader.i16be(edit.mediaRateInteger) && reader.i16be(edit.mediaRateFraction);
        if (!ok) {
            break;
        }
        track.editList.push_back(edit);
    }
}

Status MovieParser::parseStsd(const BoxHeader& box, TrackInfo& track) {
    ByteReader reader(box.payload);
    if (!reader.u32be(track.sampleEntryCount)) {
        return failStatus(ErrorCode::Truncated, "'stsd' box has no entry count");
    }
    // Sample entries are boxes that follow the count.
    bool first = true;
    BoxWalker::forEachChild(m_root, box.payloadOffset() + 4, box.end(), box.depth + 1, m_warnings,
                            [&](const BoxHeader& entry) {
                                if (!first) {
                                    // Only the first description is used; DJI files have one.
                                    return false;
                                }
                                first = false;
                                track.sampleEntry = entry.type;
                                track.sampleEntryRaw = entry.whole;
                                // Every SampleEntry starts with 6 reserved bytes + data_reference_index.
                                ByteReader entryReader(entry.body);
                                if (!entryReader.skip(6) || !entryReader.u16be(track.dataReferenceIndex)) {
                                    addWarning(m_warnings, "sample entry '" + entry.type.str() + "' of track " +
                                                               std::to_string(track.trackId) + " is too short");
                                    return false;
                                }
                                if (track.handler == Fourcc{"vide"}) {
                                    parseVisualEntry(entry, track);
                                } else if (track.handler == Fourcc{"soun"}) {
                                    parseAudioEntry(entry, track);
                                }
                                return true;
                            });
    if (first) {
        return failStatus(ErrorCode::Malformed, "'stsd' box has no sample entry");
    }
    return okStatus();
}

void MovieParser::parseVisualEntry(const BoxHeader& entry, TrackInfo& track) {
    VideoSampleEntry v;
    ByteReader reader(entry.body);
    // reserved(6) dref(2) pre_defined(2) reserved(2) pre_defined(12)
    if (!reader.skip(24) || !reader.u16be(v.width) || !reader.u16be(v.height) || !reader.u32be(v.horizResolution) ||
        !reader.u32be(v.vertResolution) || !reader.skip(4) || !reader.u16be(v.frameCount)) {
        addWarning(m_warnings, "visual sample entry of track " + std::to_string(track.trackId) + " is too short");
        track.video = std::move(v);
        return;
    }
    // compressorname: Pascal string in a 32-byte field.
    ByteSpan nameField;
    if (reader.bytes(32, nameField)) {
        const std::size_t len = nameField[0] <= 31 ? nameField[0] : 31;
        v.compressorName = log::safe(nameField.sub(1, len).toString());
    }
    std::uint16_t preDefined = 0;
    reader.u16be(v.depth);
    reader.u16be(preDefined);

    // Child boxes: codec configuration and colour information.
    BoxWalker::forEachChild(m_root, entry.bodyOffset() + kVisualEntryFixedBytes, entry.end(), entry.depth + 1,
                            m_warnings, [&](const BoxHeader& child) {
                                if (child.type == Fourcc{"hvcC"}) {
                                    Result<HevcConfig> cfg = HevcConfig::parse(child.payload);
                                    if (cfg.ok()) {
                                        v.hevc = std::move(cfg).value();
                                    } else {
                                        addWarning(m_warnings, "track " + std::to_string(track.trackId) +
                                                                   ": " + cfg.error().message);
                                    }
                                } else if (child.type == Fourcc{"avcC"}) {
                                    Result<AvcConfig> cfg = AvcConfig::parse(child.payload);
                                    if (cfg.ok()) {
                                        v.avc = std::move(cfg).value();
                                    } else {
                                        addWarning(m_warnings, "track " + std::to_string(track.trackId) +
                                                                   ": " + cfg.error().message);
                                    }
                                } else if (child.type == Fourcc{"colr"}) {
                                    Result<ColrNclx> colr = ColrNclx::parse(child.payload);
                                    if (colr.ok()) {
                                        v.colr = std::move(colr).value();
                                    } else {
                                        addWarning(m_warnings, "track " + std::to_string(track.trackId) +
                                                                   ": " + colr.error().message);
                                    }
                                } else if (child.type == Fourcc{"pasp"}) {
                                    ByteReader paspReader(child.payload);
                                    PixelAspect pasp;
                                    if (paspReader.u32be(pasp.hSpacing) && paspReader.u32be(pasp.vSpacing)) {
                                        v.pasp = pasp;
                                    }
                                }
                                return true;
                            });
    track.video = std::move(v);
}

void MovieParser::parseAudioEntry(const BoxHeader& entry, TrackInfo& track) {
    AudioSampleEntry a;
    ByteReader reader(entry.body);
    // reserved(6) dref(2) version(2) revision(2) vendor(4)
    if (!reader.skip(8) || !reader.u16be(a.version) || !reader.skip(6)) {
        addWarning(m_warnings, "audio sample entry of track " + std::to_string(track.trackId) + " is too short");
        track.audio = std::move(a);
        return;
    }
    std::uint64_t fixedBytes = kAudioEntryFixedBytesV0;
    if (a.version == 2) {
        // QuickTime version 2: always3(2) always16(2) alwaysMinus2(2) always0(2)
        // always65536(4) sizeOfStructOnly(4) sampleRate(f64) numChannels(4) ...
        fixedBytes = kAudioEntryFixedBytesV2;
        std::uint64_t rateBits = 0;
        std::uint32_t channels = 0;
        std::uint32_t always7F = 0;
        std::uint32_t bitsPerChannel = 0;
        if (reader.skip(16) && reader.u64be(rateBits) && reader.u32be(channels) && reader.u32be(always7F) &&
            reader.u32be(bitsPerChannel)) {
            double rate = 0.0;
            static_assert(sizeof(rate) == sizeof(rateBits));
            std::memcpy(&rate, &rateBits, sizeof(rate));
            a.sampleRate = rate;
            a.channelCount = static_cast<std::uint16_t>(channels > 0xFFFFu ? 0xFFFFu : channels);
            a.sampleSize = static_cast<std::uint16_t>(bitsPerChannel > 0xFFFFu ? 0xFFFFu : bitsPerChannel);
        }
    } else {
        // Versions 0 and 1: channelcount(2) samplesize(2) compression(2) packet(2) samplerate(16.16)
        std::uint16_t compression = 0, packetSize = 0;
        std::uint32_t rate = 0;
        if (reader.u16be(a.channelCount) && reader.u16be(a.sampleSize) && reader.u16be(compression) &&
            reader.u16be(packetSize) && reader.u32be(rate)) {
            a.sampleRate = static_cast<double>(rate >> 16);
        }
        if (a.version == 1) {
            fixedBytes = kAudioEntryFixedBytesV1;
        }
    }
    BoxWalker::forEachChild(m_root, entry.bodyOffset() + fixedBytes, entry.end(), entry.depth + 1, m_warnings,
                            [&](const BoxHeader& child) {
                                if (child.type == Fourcc{"esds"}) {
                                    a.esds = parseEsds(child);
                                }
                                return true;
                            });
    track.audio = std::move(a);
}

std::optional<EsdsInfo> MovieParser::parseEsds(const BoxHeader& box) {
    EsdsInfo info;
    info.raw = box.payload;
    ByteReader reader(box.payload);
    std::uint8_t tag = 0;
    std::uint32_t length = 0;
    // ES_Descriptor (tag 3).
    if (!reader.u8(tag) || tag != 0x03 || !readDescriptorLength(reader, length)) {
        return info;  // presence is still meaningful; fields stay zero
    }
    std::uint8_t esFlags = 0;
    if (!reader.u16be(info.esId) || !reader.u8(esFlags)) {
        return info;
    }
    // Optional dependsOn_ES_ID, URL string and OCR_ES_Id.
    if ((esFlags & 0x80u) != 0) {
        reader.skip(2);
    }
    if ((esFlags & 0x40u) != 0) {
        std::uint8_t urlLen = 0;
        if (reader.u8(urlLen)) {
            reader.skip(urlLen);
        }
    }
    if ((esFlags & 0x20u) != 0) {
        reader.skip(2);
    }
    // DecoderConfigDescriptor (tag 4).
    if (!reader.u8(tag) || tag != 0x04 || !readDescriptorLength(reader, length)) {
        return info;
    }
    const std::uint64_t decoderConfigEnd = reader.pos() + length;
    std::uint8_t streamTypeByte = 0;
    if (!reader.u8(info.objectTypeIndication) || !reader.u8(streamTypeByte) || !reader.u24be(info.bufferSizeDb) ||
        !reader.u32be(info.maxBitrate) || !reader.u32be(info.avgBitrate)) {
        return info;
    }
    info.streamType = static_cast<std::uint8_t>(streamTypeByte >> 2);
    // DecoderSpecificInfo (tag 5) is optional and sits inside the config.
    if (reader.pos() < decoderConfigEnd && reader.u8(tag) && tag == 0x05 && readDescriptorLength(reader, length)) {
        ByteSpan dsi;
        if (reader.bytes(length, dsi)) {
            info.decoderSpecificInfo = dsi;
        }
    }
    return info;
}

// -----------------------------------------------------------------------------
//  User data
// -----------------------------------------------------------------------------

void MovieParser::parseUdta(const BoxHeader& box) {
    UserData& u = m_movie.udta;
    BoxWalker::forEachChild(m_root, box, m_warnings, [&](const BoxHeader& child) {
        if (child.type == Fourcc::fromBytes(0xA9, 'u', 'i', 'd')) {
            u.uid = child.payload;
        } else if (child.type == Fourcc{"dbpm"} || child.type == Fourcc{"dbcm"}) {
            ByteReader reader(child.payload);
            std::uint32_t value = 0;
            if (reader.u32be(value)) {
                (child.type == Fourcc{"dbpm"} ? u.dbpm : u.dbcm) = value;
            }
        } else if (child.type == Fourcc{"btec"}) {
            u.btec = log::safe(trimNul(child.payload.toString()));
        } else if (child.type == Fourcc{"fsid"}) {
            u.fsid = log::safe(trimNul(child.payload.toString()));
        } else if (child.type == Fourcc{"meta"}) {
            if (std::optional<BoxHeader> ilst = BoxWalker::findChild(m_root, child, Fourcc{"ilst"}, m_warnings)) {
                parseIlst(*ilst);
            }
        } else if (child.type == Fourcc{"Xtra"}) {
            parseXtra(child);
        } else {
            u.unknownBoxes.push_back(child);
        }
        return true;
    });
}

void MovieParser::parseIlst(const BoxHeader& box) {
    UserData& u = m_movie.udta;
    BoxWalker::forEachChild(m_root, box, m_warnings, [&](const BoxHeader& item) {
        IlstItem entry;
        entry.name = item.type;
        entry.itemOffset = item.offset;
        // The value is in the first 'data' child: type indicator (version
        // byte + 24-bit type), locale, bytes.
        std::optional<BoxHeader> data = BoxWalker::findChild(m_root, item, Fourcc{"data"}, m_warnings);
        if (data) {
            ByteReader reader(data->body);
            std::uint32_t typeIndicator = 0;
            if (reader.u32be(typeIndicator) && reader.u32be(entry.locale)) {
                entry.dataType = typeIndicator & 0x00FFFFFFu;
                entry.dataOffset = data->bodyOffset() + 8;
                entry.data = data->body.sub(8);
            }
        }
        // Cover art and the tool tag are the items we care about by name.
        if (entry.name == Fourcc{"covr"}) {
            m_movie.covers.covr = entry.data;
        } else if (entry.name == Fourcc{"snal"}) {
            m_movie.covers.snal = entry.data;
        } else if (entry.name == Fourcc{"tnal"}) {
            m_movie.covers.tnal = entry.data;
        } else if (entry.name == Fourcc::fromBytes(0xA9, 't', 'o', 'o')) {
            u.tool = log::safe(entry.asString());
        }
        u.ilst.push_back(entry);
        return true;
    });
}

void MovieParser::parseXtra(const BoxHeader& box) {
    UserData& u = m_movie.udta;
    u.xtraRaw = box.payload;
    ByteReader reader(box.payload);
    // Each entry: u32 entry size (including itself), u32 name length, name,
    // u32 value count, then values of u32 length (including itself), u16
    // type and the bytes.
    while (reader.remaining() >= 12) {
        const std::uint64_t entryStart = reader.pos();
        std::uint32_t entrySize = 0, nameLen = 0;
        if (!reader.u32be(entrySize) || !reader.u32be(nameLen) || entrySize < 12) {
            break;
        }
        ByteSpan nameBytes;
        std::uint32_t valueCount = 0;
        if (!reader.bytes(nameLen, nameBytes) || !reader.u32be(valueCount)) {
            break;
        }
        const std::string name = log::safe(nameBytes.toString());
        for (std::uint32_t i = 0; i < valueCount; ++i) {
            std::uint32_t valueLen = 0;
            std::uint16_t valueType = 0;
            if (!reader.u32be(valueLen) || !reader.u16be(valueType) || valueLen < 6) {
                break;
            }
            ByteSpan value;
            if (!reader.bytes(valueLen - 6, value)) {
                break;
            }
            XtraEntry x;
            x.name = name;
            x.valueType = valueType;
            x.value = value;
            // Type 8 is a UTF-16LE string; other types are left raw.
            if (valueType == 8) {
                x.text = utf16leToAscii(value);
            }
            u.xtra.push_back(std::move(x));
        }
        // Continue at the declared entry end regardless of how much we read.
        if (!reader.seek(entryStart + entrySize)) {
            break;
        }
    }
}

}  // namespace

// -----------------------------------------------------------------------------
//  OsvFile::parseMovie
// -----------------------------------------------------------------------------

Result<MovieInfo> OsvFile::parseMovie(ByteSpan span, WarningList* warnings) {
    MovieInfo movie;
    MovieParser parser(span, movie);
    const Status status = parser.run();
    // Mirror the diagnostics into the caller's list even on failure so the
    // reason is visible next to the error.
    if (warnings) {
        warnings->insert(warnings->end(), movie.warnings.begin(), movie.warnings.end());
    }
    if (!status.ok()) {
        return Error(status.error());
    }
    return movie;
}

}  // namespace osv
