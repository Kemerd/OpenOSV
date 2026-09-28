// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for the MP4 / MOV spherical (360) metadata injector.
//
// The checks are made independently of the injector: the tagged file is
// walked with the container module's BoxWalker and parsed with OsvFile, and
// every sample of every track is read through its (possibly moved) chunk
// offsets and compared with the same sample of the untouched source.  The
// layouts covered:
//
//   * fx_largesize.mp4   mdat, then a largesize moov, co64 (offsets stay)
//   * fx_moov_size0.mp4  a size-0 moov as the last box, stco (offsets stay)
//   * synthetic          ftyp | moov | mdat ("faststart"), a video track on
//                        stco and an audio track on co64: every offset moves
//   * ffmpeg             a real faststart HEVC + AAC .mp4 and a .mov
//                        (SKIPs without ffmpeg; ffprobe confirms the side
//                        data when it is there)
//
// Malformed, truncated, fragmented and video-less files must be refused
// with nothing written.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/container/Box.h"
#include "osv/container/BoxWalker.h"
#include "osv/container/MovieInfo.h"
#include "osv/container/OsvFile.h"
#include "osv/container/TrackInfo.h"
#include "osv/core/ByteReader.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/MappedFile.h"
#include "osv/core/Result.h"
#include "osv/io/FfmpegPipe.h"
#include "osv/io/SphericalMetadata.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#include <sys/wait.h>
#endif

using namespace osv;

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

// =============================================================================
//  Files
// =============================================================================

Bytes readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

/// True when the file on disk is exactly `expected`.  A plain bool, so a
/// failing CHECK names the file instead of printing every byte of it.
bool fileHolds(const fs::path& path, const Bytes& expected) {
    return readFile(path) == expected;
}

/// A fresh scratch path for one test (any leftover from an earlier run and
/// its temporary file are removed first).
fs::path scratch(const std::string& name) {
    const fs::path dir = osvtest::tempDir() / "spherical";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path p = dir / name;
    fs::remove(p, ec);
    fs::path tmp = p;
    tmp += ".osvtmp";
    fs::remove(tmp, ec);
    return p;
}

/// Copy a committed fixture to a scratch path.
fs::path fixtureCopy(const char* fixture, const std::string& name) {
    const fs::path src = osvtest::fixtureDir() / fixture;
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        FAIL("fixture missing (run scripts/make_fixtures.py): " << src.string());
    }
    const fs::path dst = scratch(name);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    return dst;
}

/// True when the injector left its temporary file behind.
bool tempLeftBehind(const fs::path& target) {
    fs::path tmp = target;
    tmp += ".osvtmp";
    std::error_code ec;
    return fs::exists(tmp, ec);
}

// =============================================================================
//  Sample data, read through the chunk offset tables
// =============================================================================

/// Every sample of every track, in track order, read through stco / co64 +
/// stsc + stsz.  The OsvFile (and its mapping) is gone when this returns,
/// so the file can be rewritten in place afterwards.
struct TrackSamples {
    std::uint32_t trackId = 0;
    std::vector<Bytes> samples;
    std::vector<std::uint64_t> offsets;
};

std::vector<TrackSamples> readAllSamples(const fs::path& path) {
    std::vector<TrackSamples> out;
    const Result<OsvFile> opened = OsvFile::open(path);
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    for (const TrackInfo& track : file.movie().tracks) {
        TrackSamples t;
        t.trackId = track.trackId;
        for (std::uint32_t i = 0; i < track.samples.count(); ++i) {
            const Result<ByteSpan> bytes = file.sample(track.trackId, i);
            REQUIRE(bytes.ok());
            t.samples.push_back(bytes.value().toVector());
            t.offsets.push_back(track.samples.sampleOffset(i));
        }
        out.push_back(std::move(t));
    }
    return out;
}

/// The tagged file's samples are the source's, byte for byte, and each
/// offset moved by exactly `shift`.
void requireSameSamples(const std::vector<TrackSamples>& before, const std::vector<TrackSamples>& after,
                        std::int64_t shift) {
    REQUIRE(before.size() == after.size());
    for (std::size_t t = 0; t < before.size(); ++t) {
        INFO("track " << before[t].trackId);
        REQUIRE(before[t].trackId == after[t].trackId);
        REQUIRE(before[t].samples.size() == after[t].samples.size());
        for (std::size_t i = 0; i < before[t].samples.size(); ++i) {
            INFO("sample " << i << " (" << before[t].samples[i].size() << " bytes)");
            // Compared into a bool: Catch would otherwise print (and with -s
            // choke on) every byte of a video frame.
            const bool sameBytes = after[t].samples[i] == before[t].samples[i];
            CHECK(sameBytes);
            CHECK(static_cast<std::int64_t>(after[t].offsets[i]) ==
                  static_cast<std::int64_t>(before[t].offsets[i]) + shift);
        }
    }
}

// =============================================================================
//  Structural check of the tags, independent of the injector
// =============================================================================

/// Bytes of the V1 'uuid' box and the V2 pair for one sample entry.
std::uint64_t v1BoxBytes() {
    return 8 + 16 + io::sphericalV1Xml().size();
}
constexpr std::uint64_t kV2BoxBytes = 13 + 88;

/// The first 'vide' trak of a moov.
std::optional<BoxHeader> firstVideoTrak(ByteSpan span, const BoxHeader& moov, WarningList* warnings) {
    for (const BoxHeader& trak : BoxWalker::findChildren(span, moov, Fourcc{"trak"}, warnings)) {
        const std::optional<BoxHeader> hdlr =
            BoxWalker::findPath(span, trak, {Fourcc{"mdia"}, Fourcc{"hdlr"}}, warnings);
        if (!hdlr) {
            continue;
        }
        ByteReader r(hdlr->payload);
        std::uint32_t preDefined = 0;
        Fourcc handler;
        if (r.u32be(preDefined) && r.fourcc(handler) && handler == Fourcc{"vide"}) {
            return trak;
        }
    }
    return std::nullopt;
}

/// REQUIRE that `path` carries exactly one V1 box at the end of its first
/// video trak and exactly one well-formed st3d + sv3d pair in every sample
/// entry of that track, placed before any pasp, and that the whole file
/// walks without a single BoxWalker / parser diagnostic.
void requireWellFormedTags(const fs::path& path) {
    const Result<MappedFile> mapped = MappedFile::open(path);
    REQUIRE(mapped.ok());
    const ByteSpan span = mapped.value().span();
    WarningList warnings;

    // ---- the repository's own parser finds nothing to complain about -------
    const Result<MovieInfo> movie = OsvFile::parseMovie(span, &warnings);
    REQUIRE(movie.ok());
    REQUIRE(movie.value().moovBox.has_value());
    const BoxHeader moov = *movie.value().moovBox;
    CHECK(moov.complete());

    // ---- V1: one spherical uuid, the trak's last child, our XML -------------
    const std::optional<BoxHeader> trak = firstVideoTrak(span, moov, &warnings);
    REQUIRE(trak.has_value());
    const std::vector<BoxHeader> trakChildren = BoxWalker::children(span, *trak, &warnings);
    REQUIRE_FALSE(trakChildren.empty());
    int v1Count = 0;
    for (const BoxHeader& child : trakChildren) {
        if (child.type == Fourcc{"uuid"} && child.hasUuid && child.uuid == io::kSphericalV1Uuid) {
            ++v1Count;
            CHECK(child.size == v1BoxBytes());
            CHECK(child.body.toString() == io::sphericalV1Xml());
        }
    }
    CHECK(v1Count == 1);
    const BoxHeader& last = trakChildren.back();
    CHECK((last.type == Fourcc{"uuid"} && last.uuid == io::kSphericalV1Uuid));
    CHECK(last.end() == trak->end());

    // ---- V2: every sample entry of that track ---------------------------------
    const std::optional<BoxHeader> stsd =
        BoxWalker::findPath(span, *trak, {Fourcc{"mdia"}, Fourcc{"minf"}, Fourcc{"stbl"}, Fourcc{"stsd"}}, &warnings);
    REQUIRE(stsd.has_value());
    ByteReader countReader(stsd->payload);
    std::uint32_t entryCount = 0;
    REQUIRE(countReader.u32be(entryCount));
    std::vector<BoxHeader> entries;
    BoxWalker::forEachChild(span, stsd->payloadOffset() + 4, stsd->end(), stsd->depth + 1, &warnings,
                            [&](const BoxHeader& e) {
                                entries.push_back(e);
                                return true;
                            });
    REQUIRE(entryCount >= 1);
    REQUIRE(entries.size() >= entryCount);
    for (std::uint32_t e = 0; e < entryCount; ++e) {
        const BoxHeader& entry = entries[e];
        INFO("sample entry " << entry.type.str());
        std::vector<BoxHeader> kids;
        BoxWalker::forEachChild(span, entry.bodyOffset() + 78, entry.end(), entry.depth + 1, &warnings,
                                [&](const BoxHeader& k) {
                                    kids.push_back(k);
                                    return true;
                                });
        std::optional<std::size_t> st3dAt, sv3dAt, paspAt;
        int st3dCount = 0, sv3dCount = 0;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            if (kids[i].type == Fourcc{"st3d"}) {
                ++st3dCount;
                st3dAt = i;
            } else if (kids[i].type == Fourcc{"sv3d"}) {
                ++sv3dCount;
                sv3dAt = i;
            } else if (kids[i].type == Fourcc{"pasp"} && !paspAt) {
                paspAt = i;
            }
        }
        REQUIRE(st3dCount == 1);
        REQUIRE(sv3dCount == 1);
        CHECK(*sv3dAt == *st3dAt + 1);  // st3d then sv3d, as the RFC orders them
        if (paspAt) {
            CHECK(*sv3dAt < *paspAt);  // before the optional tail boxes
        }

        // st3d: FullBox v0, stereo_mode 0 (monoscopic).
        const BoxHeader& st3d = kids[*st3dAt];
        CHECK(st3d.size == 13);
        REQUIRE(st3d.body.size() == 5);
        CHECK(st3d.body[0] == 0);  // version
        CHECK(st3d.body[4] == 0);  // stereo_mode

        // sv3d: exactly svhd then proj { prhd, equi }.
        const BoxHeader& sv3d = kids[*sv3dAt];
        CHECK(sv3d.size == 88);
        const std::vector<BoxHeader> sv = BoxWalker::children(span, sv3d, &warnings);
        REQUIRE(sv.size() == 2);
        REQUIRE(sv[0].type == Fourcc{"svhd"});
        REQUIRE(sv[1].type == Fourcc{"proj"});
        const std::string source = std::string(io::kSphericalMetadataSource) + '\0';
        CHECK(sv[0].body.sub(4).toString() == source);  // after version + flags
        const std::vector<BoxHeader> proj = BoxWalker::children(span, sv[1], &warnings);
        REQUIRE(proj.size() == 2);
        REQUIRE(proj[0].type == Fourcc{"prhd"});
        REQUIRE(proj[1].type == Fourcc{"equi"});
        CHECK(proj[0].size == 24);
        CHECK(proj[1].size == 28);
        CHECK(std::all_of(proj[0].body.begin(), proj[0].body.end(), [](std::uint8_t b) { return b == 0; }));
        CHECK(std::all_of(proj[1].body.begin(), proj[1].body.end(), [](std::uint8_t b) { return b == 0; }));
    }

    // Any size inconsistency anywhere above shows up here.
    INFO("first warning: " << (warnings.empty() ? std::string() : warnings.front()));
    CHECK(warnings.empty());
}

// =============================================================================
//  A synthetic movie: two tracks, faststart or not
// =============================================================================

void u8(Bytes& b, std::uint8_t v) {
    b.push_back(v);
}
void u16(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void u32(Bytes& b, std::uint32_t v) {
    u16(b, static_cast<std::uint16_t>(v >> 16));
    u16(b, static_cast<std::uint16_t>(v));
}
void u64(Bytes& b, std::uint64_t v) {
    u32(b, static_cast<std::uint32_t>(v >> 32));
    u32(b, static_cast<std::uint32_t>(v));
}
void tag(Bytes& b, const char* four) {
    b.insert(b.end(), four, four + 4);
}
void zeros(Bytes& b, std::size_t n) {
    b.insert(b.end(), n, 0);
}
void append(Bytes& b, const Bytes& more) {
    b.insert(b.end(), more.begin(), more.end());
}

Bytes box(const char* type, const Bytes& payload) {
    Bytes b;
    u32(b, static_cast<std::uint32_t>(8 + payload.size()));
    tag(b, type);
    append(b, payload);
    return b;
}

Bytes fullBox(const char* type, std::uint8_t version, std::uint32_t flags, const Bytes& payload) {
    Bytes p;
    u8(p, version);
    u8(p, static_cast<std::uint8_t>(flags >> 16));
    u8(p, static_cast<std::uint8_t>(flags >> 8));
    u8(p, static_cast<std::uint8_t>(flags));
    append(p, payload);
    return box(type, p);
}

void unityMatrix(Bytes& p) {
    const std::uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (const std::uint32_t v : m) {
        u32(p, v);
    }
}

Bytes mvhd() {
    Bytes p;
    u32(p, 0);           // creation
    u32(p, 0);           // modification
    u32(p, 1000);        // timescale
    u32(p, 500);         // duration
    u32(p, 0x00010000);  // rate 1.0
    u16(p, 0x0100);      // volume 1.0
    zeros(p, 10);
    unityMatrix(p);
    zeros(p, 24);  // pre_defined
    u32(p, 3);     // next_track_ID
    return fullBox("mvhd", 0, 0, p);
}

Bytes tkhd(std::uint32_t id, std::uint16_t w, std::uint16_t h, bool audio) {
    Bytes p;
    u32(p, 0);
    u32(p, 0);
    u32(p, id);
    u32(p, 0);
    u32(p, 500);
    zeros(p, 8);
    u16(p, 0);                   // layer
    u16(p, 0);                   // alternate_group
    u16(p, audio ? 0x0100 : 0);  // volume
    u16(p, 0);
    unityMatrix(p);
    u32(p, static_cast<std::uint32_t>(w) << 16);
    u32(p, static_cast<std::uint32_t>(h) << 16);
    return fullBox("tkhd", 0, 3, p);
}

Bytes mdhd(std::uint32_t timescale, std::uint32_t duration) {
    Bytes p;
    u32(p, 0);
    u32(p, 0);
    u32(p, timescale);
    u32(p, duration);
    u16(p, 0x55C4);  // 'und'
    u16(p, 0);
    return fullBox("mdhd", 0, 0, p);
}

Bytes hdlr(const char* handler, const char* name) {
    Bytes p;
    u32(p, 0);
    tag(p, handler);
    zeros(p, 12);
    p.insert(p.end(), name, name + std::strlen(name) + 1);
    return fullBox("hdlr", 0, 0, p);
}

Bytes dinf() {
    Bytes dref;
    u32(dref, 1);
    append(dref, fullBox("url ", 0, 1, {}));  // self-contained
    return box("dinf", fullBox("dref", 0, 0, dref));
}

Bytes stts(std::uint32_t count, std::uint32_t delta) {
    Bytes p;
    u32(p, 1);
    u32(p, count);
    u32(p, delta);
    return fullBox("stts", 0, 0, p);
}

Bytes stsc(const std::vector<std::array<std::uint32_t, 3>>& runs) {
    Bytes p;
    u32(p, static_cast<std::uint32_t>(runs.size()));
    for (const auto& r : runs) {
        u32(p, r[0]);
        u32(p, r[1]);
        u32(p, r[2]);
    }
    return fullBox("stsc", 0, 0, p);
}

Bytes stsz(const std::vector<std::uint32_t>& sizes) {
    Bytes p;
    u32(p, 0);
    u32(p, static_cast<std::uint32_t>(sizes.size()));
    for (const std::uint32_t s : sizes) {
        u32(p, s);
    }
    return fullBox("stsz", 0, 0, p);
}

Bytes chunkTable(const std::vector<std::uint64_t>& offsets, bool wide) {
    Bytes p;
    u32(p, static_cast<std::uint32_t>(offsets.size()));
    for (const std::uint64_t o : offsets) {
        if (wide) {
            u64(p, o);
        } else {
            u32(p, static_cast<std::uint32_t>(o));
        }
    }
    return fullBox(wide ? "co64" : "stco", 0, 0, p);
}

/// 'hvc1' visual sample entry with colr and pasp children (no hvcC: the
/// injector does not care, and the parser only warns about a bad one).
Bytes hvc1Entry(std::uint16_t w, std::uint16_t h) {
    Bytes p;
    zeros(p, 6);
    u16(p, 1);     // data_reference_index
    zeros(p, 16);  // pre_defined / reserved
    u16(p, w);
    u16(p, h);
    u32(p, 0x00480000);  // 72 dpi
    u32(p, 0x00480000);
    u32(p, 0);
    u16(p, 1);       // frame_count
    zeros(p, 32);    // compressorname
    u16(p, 0x0018);  // depth
    u16(p, 0xFFFF);  // pre_defined -1
    Bytes colr;
    tag(colr, "nclx");
    u16(colr, 1);
    u16(colr, 1);
    u16(colr, 1);
    u8(colr, 0);
    append(p, box("colr", colr));
    Bytes pasp;
    u32(pasp, 1);
    u32(pasp, 1);
    append(p, box("pasp", pasp));
    return box("hvc1", p);
}

Bytes mp4aEntry() {
    Bytes p;
    zeros(p, 6);
    u16(p, 1);
    zeros(p, 8);
    u16(p, 2);   // channelcount
    u16(p, 16);  // samplesize
    u16(p, 0);
    u16(p, 0);
    u32(p, 48000u << 16);  // samplerate 16.16
    return box("mp4a", p);
}

/// Layout and corruption switches of the synthetic movie.
struct SynthOptions {
    bool moovFirst = true;        ///< ftyp | moov | mdat (faststart) instead of ftyp | mdat | moov.
    bool withVideo = true;        ///< false: an audio-only movie.
    bool offsetIntoMoov = false;  ///< Corrupt: the first video chunk offset points inside moov.
    bool fragmented = false;      ///< Append a top-level 'moof'.
};

// Video: 5 samples of 10..14 bytes in two chunks (3 + 2); audio: 3 samples
// of 6 bytes in one chunk between them.  Sample i of the video is filled
// with (i + 1), the audio's with 0xA0 + i.
constexpr std::uint32_t kVideoSizes[5] = {10, 11, 12, 13, 14};
constexpr std::uint32_t kAudioSize = 6;

Bytes buildMoov(std::uint64_t mdatBody, const SynthOptions& o, std::uint64_t moovStart) {
    const std::uint64_t v1 = o.offsetIntoMoov ? moovStart + 8 : mdatBody;
    const std::uint64_t a1 = mdatBody + 10 + 11 + 12;
    const std::uint64_t v2 = a1 + 3 * kAudioSize;
    Bytes moov = mvhd();
    if (o.withVideo) {
        Bytes stbl = fullBox("stsd", 0, 0, [] {
            Bytes p;
            u32(p, 1);
            append(p, hvc1Entry(64, 32));
            return p;
        }());
        append(stbl, stts(5, 100));
        append(stbl, stsc({{1, 3, 1}, {2, 2, 1}}));
        append(stbl, stsz({kVideoSizes[0], kVideoSizes[1], kVideoSizes[2], kVideoSizes[3], kVideoSizes[4]}));
        append(stbl, chunkTable({v1, v2}, false));  // the video on 32-bit stco
        Bytes vmhd;
        zeros(vmhd, 8);
        Bytes minf = fullBox("vmhd", 0, 1, vmhd);
        append(minf, dinf());
        append(minf, box("stbl", stbl));
        Bytes mdia = mdhd(1000, 500);
        append(mdia, hdlr("vide", "Video"));
        append(mdia, box("minf", minf));
        Bytes trak = tkhd(1, 64, 32, false);
        append(trak, box("mdia", mdia));
        append(moov, box("trak", trak));
    }
    {
        Bytes stbl = fullBox("stsd", 0, 0, [] {
            Bytes p;
            u32(p, 1);
            append(p, mp4aEntry());
            return p;
        }());
        append(stbl, stts(3, 1024));
        append(stbl, stsc({{1, 3, 1}}));
        append(stbl, stsz({kAudioSize, kAudioSize, kAudioSize}));
        append(stbl, chunkTable({a1}, true));  // the audio on 64-bit co64
        Bytes smhd;
        zeros(smhd, 4);
        Bytes minf = fullBox("smhd", 0, 0, smhd);
        append(minf, dinf());
        append(minf, box("stbl", stbl));
        Bytes mdia = mdhd(48000, 3072);
        append(mdia, hdlr("soun", "Sound"));
        append(mdia, box("minf", minf));
        Bytes trak = tkhd(2, 0, 0, true);
        append(trak, box("mdia", mdia));
        append(moov, box("trak", trak));
    }
    return box("moov", moov);
}

Bytes buildSynthetic(const SynthOptions& o) {
    Bytes ftyp;
    tag(ftyp, "isom");
    u32(ftyp, 0x200);
    tag(ftyp, "isom");
    tag(ftyp, "iso2");
    tag(ftyp, "mp41");
    const Bytes ftypBox = box("ftyp", ftyp);

    // ---- media data: video chunk 1 | audio chunk | video chunk 2 --------------
    Bytes media;
    for (int i = 0; i < 3; ++i) {
        media.insert(media.end(), kVideoSizes[i], static_cast<std::uint8_t>(i + 1));
    }
    for (int i = 0; i < 3; ++i) {
        media.insert(media.end(), kAudioSize, static_cast<std::uint8_t>(0xA0 + i));
    }
    for (int i = 3; i < 5; ++i) {
        media.insert(media.end(), kVideoSizes[i], static_cast<std::uint8_t>(i + 1));
    }
    const Bytes mdat = box("mdat", media);

    // ---- the moov's size does not depend on the offsets it holds, so one
    //      dry run tells where the media data will start ----------------------
    const std::uint64_t moovSize = buildMoov(0, o, 0).size();
    Bytes file = ftypBox;
    if (o.moovFirst) {
        const std::uint64_t moovStart = ftypBox.size();
        append(file, buildMoov(moovStart + moovSize + 8, o, moovStart));
        append(file, mdat);
    } else {
        const std::uint64_t moovStart = ftypBox.size() + mdat.size();
        append(file, mdat);
        append(file, buildMoov(ftypBox.size() + 8, o, moovStart));
    }
    if (o.fragmented) {
        Bytes mfhd;
        u32(mfhd, 1);
        append(file, box("moof", fullBox("mfhd", 0, 0, mfhd)));
    }
    return file;
}

/// Offset of the first `type` box header in `bytes` (the 4 bytes before the
/// type are its size).
std::size_t findBox(const Bytes& bytes, const char* type) {
    for (std::size_t i = 4; i + 4 <= bytes.size(); ++i) {
        if (std::memcmp(bytes.data() + i, type, 4) == 0) {
            return i - 4;
        }
    }
    FAIL("no '" << type << "' in the buffer");
    return 0;
}

// =============================================================================
//  External programs (ffmpeg / ffprobe) for the guarded end-to-end case
// =============================================================================

struct RunResult {
    int exitCode = -1;
    std::string output;  // stdout + stderr
};

/// Run a command line and capture its output (CreateProcess / popen).
RunResult runProcess(const std::string& commandLine) {
    RunResult r;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) {
        return r;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        return r;
    }
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        r.output.append(buffer, got);
    }
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    FILE* pipe = ::popen((commandLine + " 2>&1").c_str(), "r");
    if (!pipe) {
        return r;
    }
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        r.output.append(buffer, got);
    }
    const int status = ::pclose(pipe);
    if (status != -1 && WIFEXITED(status)) {
        r.exitCode = WEXITSTATUS(status);
    }
#endif
    return r;
}

std::string quoted(const fs::path& p) {
    return "\"" + p.string() + "\"";
}

/// ffmpeg: OSV_FFMPEG_EXE, then C:\ffmpeg\bin, then PATH; empty when none.
fs::path findFfmpeg() {
    std::error_code ec;
    if (const char* env = std::getenv("OSV_FFMPEG_EXE"); env && *env && fs::exists(env, ec)) {
        return fs::path(env);
    }
    const fs::path fixed("C:/ffmpeg/bin/ffmpeg.exe");
    if (fs::exists(fixed, ec)) {
        return fixed;
    }
    const fs::path resolved = io::FfmpegPipeWriter::resolveExecutable({});
    if (fs::exists(resolved, ec)) {
        return resolved;
    }
    return {};
}

/// Make a one-second faststart clip with ffmpeg (CPU encoders only): the
/// first video encoder of `videoCodecs` that works.  False when none does.
bool makeClip(const fs::path& ffmpeg, const fs::path& out, const std::vector<std::string>& videoCodecs, bool audio) {
    for (const std::string& codec : videoCodecs) {
        std::string cmd = quoted(ffmpeg) + " -nostdin -v error -y -f lavfi -i testsrc2=s=256x128:d=1:r=10";
        if (audio) {
            cmd += " -f lavfi -i sine=d=1 -c:a aac";
        }
        cmd += " -c:v " + codec + " -movflags +faststart " + quoted(out);
        if (runProcess(cmd).exitCode == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

// =============================================================================
//  Tests
// =============================================================================

TEST_CASE("Spherical V1 XML is Google's document with OpenOSV as the stitcher", "[spherical]") {
    const std::string xml = io::sphericalV1Xml();
    CHECK(xml.rfind("<?xml version=\"1.0\"?><rdf:SphericalVideo", 0) == 0);
    CHECK(xml.find("xmlns:GSpherical=\"http://ns.google.com/videos/1.0/spherical/\"") != std::string::npos);
    CHECK(xml.find("<GSpherical:Spherical>true</GSpherical:Spherical>") != std::string::npos);
    CHECK(xml.find("<GSpherical:Stitched>true</GSpherical:Stitched>") != std::string::npos);
    CHECK(xml.find("<GSpherical:StitchingSoftware>OpenOSV</GSpherical:StitchingSoftware>") != std::string::npos);
    CHECK(xml.find("<GSpherical:ProjectionType>equirectangular</GSpherical:ProjectionType>") != std::string::npos);
    const std::string footer = "</rdf:SphericalVideo>";
    REQUIRE(xml.size() > footer.size());
    CHECK(xml.compare(xml.size() - footer.size(), footer.size(), footer) == 0);
}

TEST_CASE("Spherical: moov after mdat, largesize moov and co64 - offsets stay", "[spherical]") {
    const fs::path file = fixtureCopy("fx_largesize.mp4", "largesize.mp4");
    const Bytes before = readFile(file);
    const std::vector<TrackSamples> samplesBefore = readAllSamples(file);

    const Result<io::SphericalInjectReport> r = io::injectSphericalMetadata(file);
    REQUIRE(r.ok());
    CHECK(r.value().changed);
    CHECK_FALSE(r.value().replacedExisting);
    CHECK_FALSE(r.value().moovBeforeMdat);
    CHECK(r.value().chunkOffsetsShifted == 0);
    CHECK(r.value().sampleEntriesTagged == 1);
    CHECK(r.value().sampleEntryType == "avc1");
    CHECK(r.value().videoTrackId == 1);
    CHECK(r.value().moovGrowth == static_cast<std::int64_t>(v1BoxBytes() + kV2BoxBytes));
    CHECK_FALSE(tempLeftBehind(file));

    const Bytes after = readFile(file);
    REQUIRE(after.size() == before.size() + v1BoxBytes() + kV2BoxBytes);
    // Everything before the moov (ftyp and the whole mdat) is untouched.
    const std::size_t moovAt = findBox(before, "moov");
    CHECK(std::equal(before.begin(), before.begin() + static_cast<std::ptrdiff_t>(moovAt), after.begin()));

    requireWellFormedTags(file);
    requireSameSamples(samplesBefore, readAllSamples(file), 0);
}

TEST_CASE("Spherical: a size-0 moov at the end of the file gets its real size", "[spherical]") {
    const fs::path file = fixtureCopy("fx_moov_size0.mp4", "moov_size0.mp4");
    const std::vector<TrackSamples> samplesBefore = readAllSamples(file);
    const Result<io::SphericalInjectReport> r = io::injectSphericalMetadata(file);
    REQUIRE(r.ok());
    CHECK(r.value().changed);
    CHECK(r.value().chunkOffsetsShifted == 0);
    requireWellFormedTags(file);
    requireSameSamples(samplesBefore, readAllSamples(file), 0);

    // The moov now declares its size instead of "to the end of the file".
    const Result<OsvFile> opened = OsvFile::open(file);
    REQUIRE(opened.ok());
    REQUIRE(opened.value().movie().moovBox.has_value());
    CHECK_FALSE(opened.value().movie().moovBox->toEnd);
    CHECK(opened.value().movie().moovBox->end() == opened.value().size());
}

TEST_CASE("Spherical: faststart file - every stco and co64 offset of every track moves", "[spherical]") {
    const fs::path file = scratch("faststart.mp4");
    const Bytes source = buildSynthetic(SynthOptions{});
    writeFile(file, source);
    const std::vector<TrackSamples> samplesBefore = readAllSamples(file);
    REQUIRE(samplesBefore.size() == 2);
    REQUIRE(samplesBefore[0].samples.size() == 5);
    REQUIRE(samplesBefore[1].samples.size() == 3);

    const Result<io::SphericalInjectReport> r = io::injectSphericalMetadata(file);
    REQUIRE(r.ok());
    const std::int64_t growth = static_cast<std::int64_t>(v1BoxBytes() + kV2BoxBytes);
    CHECK(r.value().changed);
    CHECK(r.value().moovBeforeMdat);
    CHECK(r.value().moovGrowth == growth);
    CHECK(r.value().chunkOffsetsShifted == 3);  // two stco entries (video) + one co64 entry (audio)
    CHECK(r.value().sampleEntryType == "hvc1");

    requireWellFormedTags(file);
    // Same bytes, each read through an offset moved by exactly the growth.
    requireSameSamples(samplesBefore, readAllSamples(file), growth);
    // The media data itself is copied unchanged behind the grown moov.
    const Bytes after = readFile(file);
    const std::size_t mdatBefore = findBox(source, "mdat");
    const std::size_t mdatAfter = findBox(after, "mdat");
    CHECK(mdatAfter == mdatBefore + static_cast<std::size_t>(growth));
    CHECK(std::equal(source.begin() + static_cast<std::ptrdiff_t>(mdatBefore), source.end(),
                     after.begin() + static_cast<std::ptrdiff_t>(mdatAfter)));
}

TEST_CASE("Spherical: tagging twice changes nothing; an old tag is replaced, not duplicated", "[spherical]") {
    const fs::path file = scratch("idempotent.mp4");
    writeFile(file, buildSynthetic(SynthOptions{}));
    const std::vector<TrackSamples> samplesBefore = readAllSamples(file);
    REQUIRE(io::injectSphericalMetadata(file).ok());
    const Bytes once = readFile(file);

    SECTION("a second run leaves the file byte for byte as it was") {
        const Result<io::SphericalInjectReport> again = io::injectSphericalMetadata(file);
        REQUIRE(again.ok());
        CHECK_FALSE(again.value().changed);
        CHECK(again.value().replacedExisting);  // it found (and would re-write) its own tags
        CHECK(again.value().moovGrowth == 0);
        CHECK(fileHolds(file, once));
        CHECK_FALSE(tempLeftBehind(file));
        requireWellFormedTags(file);
    }

    SECTION("a different earlier tag is swapped for ours, once") {
        // Another tool's V1 document: same length, different stitcher.
        Bytes edited = once;
        const std::string ours = "<GSpherical:StitchingSoftware>OpenOSV<";
        const auto at = std::search(edited.begin(), edited.end(), ours.begin(), ours.end());
        REQUIRE(at != edited.end());
        *(at + 30) = 'X';  // "OpenOSV" -> "XpenOSV"
        writeFile(file, edited);
        const Result<io::SphericalInjectReport> r = io::injectSphericalMetadata(file);
        REQUIRE(r.ok());
        CHECK(r.value().changed);
        CHECK(r.value().replacedExisting);
        CHECK(r.value().moovGrowth == 0);
        CHECK(fileHolds(file, once));
        requireWellFormedTags(file);
    }

    requireSameSamples(samplesBefore, readAllSamples(file), static_cast<std::int64_t>(v1BoxBytes() + kV2BoxBytes));
}

TEST_CASE("Spherical: --out writes a tagged copy and leaves the input alone", "[spherical]") {
    const fs::path input = scratch("out_source.mp4");
    const fs::path output = scratch("out_tagged.mp4");
    const Bytes source = buildSynthetic(SynthOptions{});
    writeFile(input, source);

    const Result<io::SphericalInjectReport> r = io::injectSphericalMetadata(input, output);
    REQUIRE(r.ok());
    CHECK(r.value().changed);
    CHECK(fileHolds(input, source));
    requireWellFormedTags(output);
    requireSameSamples(readAllSamples(input), readAllSamples(output),
                       static_cast<std::int64_t>(v1BoxBytes() + kV2BoxBytes));

    // An already tagged input still produces the copy that was asked for.
    const fs::path copy = scratch("out_copy.mp4");
    const Result<io::SphericalInjectReport> again = io::injectSphericalMetadata(output, copy);
    REQUIRE(again.ok());
    CHECK_FALSE(again.value().changed);
    CHECK(fileHolds(copy, readFile(output)));
}

TEST_CASE("Spherical: malformed, truncated and unsupported files are refused, nothing written", "[spherical]") {
    // Every case: the call fails with the expected code, the file is byte
    // for byte what it was, and no temporary file is left behind.
    const auto refuse = [](const fs::path& file, ErrorCode expected) {
        const Bytes before = readFile(file);
        const Result<io::SphericalInjectReport> r = io::injectSphericalMetadata(file);
        REQUIRE_FALSE(r.ok());
        INFO(r.error().toString());
        CHECK(r.error().code == expected);
        CHECK_FALSE(r.error().message.empty());
        CHECK(fileHolds(file, before));
        CHECK_FALSE(tempLeftBehind(file));
    };

    SECTION("a moov cut short by the end of the file") {
        refuse(fixtureCopy("fx_truncated_moov.mp4", "bad_truncated.mp4"), ErrorCode::Truncated);
    }
    SECTION("a top-level largesize past the end of the file") {
        const fs::path file = fixtureCopy("fx_largesize.mp4", "bad_largesize.mp4");
        Bytes bytes = readFile(file);
        const std::size_t moov = findBox(bytes, "moov");
        bytes[moov + 8] = 0x7F;  // the high byte of the 64-bit largesize
        writeFile(file, bytes);
        refuse(file, ErrorCode::Truncated);
    }
    SECTION("a trak that runs past its moov") {
        const fs::path file = scratch("bad_trak.mp4");
        Bytes bytes = buildSynthetic(SynthOptions{});
        const std::size_t trak = findBox(bytes, "trak");
        bytes[trak] = 0x01;  // +16 MB: far past the end of its moov

        writeFile(file, bytes);
        refuse(file, ErrorCode::Truncated);
    }
    SECTION("a chunk offset pointing inside moov") {
        const fs::path file = scratch("bad_offset.mp4");
        SynthOptions o;
        o.offsetIntoMoov = true;
        writeFile(file, buildSynthetic(o));
        refuse(file, ErrorCode::Malformed);
    }
    SECTION("fragmented MP4") {
        const fs::path file = scratch("bad_fragmented.mp4");
        SynthOptions o;
        o.fragmented = true;
        writeFile(file, buildSynthetic(o));
        refuse(file, ErrorCode::Unsupported);
    }
    SECTION("no video track") {
        const fs::path file = scratch("bad_audio_only.mp4");
        SynthOptions o;
        o.withVideo = false;
        writeFile(file, buildSynthetic(o));
        refuse(file, ErrorCode::NotFound);
    }
    SECTION("not an MP4 at all") {
        const fs::path file = scratch("bad_text.mp4");
        const std::string text = "this is a text file, not a video, whatever its name says\n";
        writeFile(file, Bytes(text.begin(), text.end()));
        refuse(file, ErrorCode::Malformed);
    }
    SECTION("too small to hold a box") {
        const fs::path file = scratch("bad_tiny.mp4");
        writeFile(file, Bytes{0, 0, 0});
        refuse(file, ErrorCode::Malformed);
    }
    SECTION("missing file and empty path") {
        CHECK(io::injectSphericalMetadata(scratch("does_not_exist.mp4")).code() == ErrorCode::Io);
        CHECK(io::injectSphericalMetadata(fs::path()).code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("Spherical: real ffmpeg faststart MP4 and MOV; ffprobe sees the projection", "[spherical][ffmpeg-exe]") {
    const fs::path ffmpeg = findFfmpeg();
    if (ffmpeg.empty()) {
        SKIP("ffmpeg not found (OSV_FFMPEG_EXE, C:\\ffmpeg\\bin or PATH)");
    }
    fs::path ffprobe = ffmpeg.parent_path() / ("ffprobe" + ffmpeg.extension().string());
    std::error_code ec;
    const bool haveProbe = fs::exists(ffprobe, ec);

    // What osvtool writes (HEVC 'hvc1' with AAC) and what an NLE exports
    // (ProRes in a .mov); mpeg4 is the fallback every ffmpeg build has.
    struct Clip {
        const char* name;
        std::vector<std::string> codecs;
        bool audio;
    };
    const Clip clips[] = {
        {"ffmpeg_hevc.mp4", {"libx265 -tag:v hvc1 -x265-params log-level=error", "mpeg4"}, true},
        {"ffmpeg_prores.mov", {"prores_ks", "mpeg4"}, false},
    };
    for (const Clip& clip : clips) {
        DYNAMIC_SECTION(clip.name) {
            const fs::path file = scratch(clip.name);
            if (!makeClip(ffmpeg, file, clip.codecs, clip.audio)) {
                SKIP("ffmpeg could not encode a test clip");
            }
            const std::vector<TrackSamples> samplesBefore = readAllSamples(file);

            const Result<io::SphericalInjectReport> r = io::injectSphericalMetadata(file);
            REQUIRE(r.ok());
            CHECK(r.value().changed);
            CHECK(r.value().moovBeforeMdat);  // -movflags +faststart
            CHECK(r.value().chunkOffsetsShifted > 0);
            requireWellFormedTags(file);
            requireSameSamples(samplesBefore, readAllSamples(file), r.value().moovGrowth);

            // The file still decodes cleanly end to end.
            const RunResult decode =
                runProcess(quoted(ffmpeg) + " -nostdin -v error -i " + quoted(file) + " -f null -");
            CHECK(decode.exitCode == 0);
            CHECK(decode.output.empty());

            if (haveProbe) {
                const RunResult probe =
                    runProcess(quoted(ffprobe) + " -v error -show_streams -select_streams v:0 " + quoted(file));
                REQUIRE(probe.exitCode == 0);
                INFO(probe.output);
                CHECK(probe.output.find("side_data_type=Spherical Mapping") != std::string::npos);
                CHECK(probe.output.find("projection=equirectangular") != std::string::npos);
                CHECK(probe.output.find("side_data_type=Stereo 3D") != std::string::npos);
            } else {
                WARN("ffprobe not found next to ffmpeg; side data not checked");
            }
        }
    }
}
