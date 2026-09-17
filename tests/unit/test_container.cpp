// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for osv_container: box walking, sample tables, codec configuration,
// the DJI index table, cover art, the nested 'camd' movie and OsvFile.
//
// Three groups:
//   [container]          synthetic fixtures from scripts/make_fixtures.py and
//                        hand-built byte buffers; always run.
//   [container][sample]  the real 6K clip and its LRF proxy; SKIP when absent.
//   [container][fuzz]    truncation / garbage sweeps that must never crash.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/container/Box.h"
#include "osv/container/BoxWalker.h"
#include "osv/container/HevcConfig.h"
#include "osv/container/IndexTable.h"
#include "osv/container/MovieInfo.h"
#include "osv/container/OsvFile.h"
#include "osv/container/SampleTable.h"
#include "osv/container/TrackInfo.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace osv;
using Catch::Matchers::WithinRel;

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// Read a whole file into memory (fixtures are tiny; the sample clip is 32 MB
/// and only read for the truncation sweeps).
std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Path of a synthetic fixture; the test fails loudly when the generator
/// has not been run.
std::filesystem::path fixture(const char* name) {
    const std::filesystem::path p = osvtest::fixtureDir() / name;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        FAIL("fixture missing (run scripts/make_fixtures.py): " << p.string());
    }
    return p;
}

/// True when the LRF proxy is present next to the sample.
bool haveLrf() {
    std::error_code ec;
    return std::filesystem::exists(osvtest::sampleLrf(), ec) && !ec;
}

/// Append a big-endian u32.
void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/// Append a plain box header + payload.
void putBox(std::vector<std::uint8_t>& out, const char (&type)[5], const std::vector<std::uint8_t>& payload) {
    putU32(out, static_cast<std::uint32_t>(8 + payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
}

/// True when every byte of `span` equals `value`.
bool allBytesEqual(ByteSpan span, std::uint8_t value) {
    for (std::size_t i = 0; i < span.size(); ++i) {
        if (span[i] != value) {
            return false;
        }
    }
    return !span.empty();
}

/// Values baked into scripts/make_fixtures.py (keep in sync).
constexpr std::uint32_t kFixtureSampleSizes[5] = {10, 20, 30, 40, 50};
constexpr std::size_t kFixtureCoverBytes = 22;
constexpr std::uint32_t kFixtureDjmdSampleSize = 8;
constexpr std::uint32_t kFixtureDjmdSampleCount = 4;
constexpr std::uint32_t kFixtureCamdSampleSizes[3] = {5, 6, 7};

}  // namespace

// =============================================================================
//  BoxWalker on hand-built buffers
// =============================================================================

TEST_CASE("BoxWalker reads plain, largesize, size-0 and uuid headers", "[container]") {
    std::vector<std::uint8_t> buf;
    // Plain 12-byte box.
    putBox(buf, "free", {1, 2, 3, 4});
    // Largesize box: size field 1, then u64 size 24, then 8 payload bytes.
    putU32(buf, 1);
    buf.insert(buf.end(), {'s', 'k', 'i', 'p'});
    for (int i = 0; i < 7; ++i) {
        buf.push_back(0);
    }
    buf.push_back(24);
    for (int i = 0; i < 8; ++i) {
        buf.push_back(0xAA);
    }
    // uuid box: 8 + 16 header, 4 payload bytes.
    putU32(buf, 28);
    buf.insert(buf.end(), {'u', 'u', 'i', 'd'});
    for (int i = 0; i < 16; ++i) {
        buf.push_back(static_cast<std::uint8_t>(i));
    }
    buf.insert(buf.end(), {9, 9, 9, 9});
    // Size-0 box extends to the end: 8 header + 5 payload.
    putU32(buf, 0);
    buf.insert(buf.end(), {'m', 'd', 'a', 't'});
    buf.insert(buf.end(), {7, 7, 7, 7, 7});

    const ByteSpan root(buf);
    std::vector<BoxHeader> boxes;
    WarningList warnings;
    BoxWalker::forEachTopLevel(root, &warnings, [&](const BoxHeader& b) {
        boxes.push_back(b);
        return true;
    });
    REQUIRE(warnings.empty());
    REQUIRE(boxes.size() == 4);

    CHECK(boxes[0].type == Fourcc{"free"});
    CHECK(boxes[0].size == 12);
    CHECK(boxes[0].headerSize == 8);
    CHECK(boxes[0].payload.size() == 4);
    CHECK_FALSE(boxes[0].isFull);

    CHECK(boxes[1].type == Fourcc{"skip"});
    CHECK(boxes[1].largeSize);
    CHECK(boxes[1].size == 24);
    CHECK(boxes[1].headerSize == 16);
    CHECK(boxes[1].offset == 12);
    CHECK(boxes[1].payload.size() == 8);

    CHECK(boxes[2].type == Fourcc{"uuid"});
    CHECK(boxes[2].hasUuid);
    CHECK(boxes[2].headerSize == 24);
    CHECK(boxes[2].uuid[0] == 0);
    CHECK(boxes[2].uuid[15] == 15);
    CHECK(boxes[2].payload.size() == 4);

    CHECK(boxes[3].type == Fourcc{"mdat"});
    CHECK(boxes[3].toEnd);
    CHECK(boxes[3].size == 13);
    CHECK(boxes[3].end() == buf.size());
    CHECK(boxes[3].payload.size() == 5);
}

TEST_CASE("BoxWalker parses full boxes and sniffs QuickTime 'meta'", "[container]") {
    std::vector<std::uint8_t> buf;
    // A full box: version 1, flags 0x000003, then 4 payload bytes.
    putBox(buf, "tkhd", {1, 0, 0, 3, 0xDE, 0xAD, 0xBE, 0xEF});
    // A QuickTime-style 'meta' whose first child is 'hdlr' (no version/flags).
    std::vector<std::uint8_t> metaPayload;
    putBox(metaPayload, "hdlr", {0, 0, 0, 0});
    putBox(buf, "meta", metaPayload);
    // An ISO-style 'meta' with version/flags before the 'hdlr'.
    std::vector<std::uint8_t> isoMeta = {0, 0, 0, 0};
    putBox(isoMeta, "hdlr", {0, 0, 0, 0});
    putBox(buf, "meta", isoMeta);

    const ByteSpan root(buf);
    std::vector<BoxHeader> boxes;
    BoxWalker::forEachTopLevel(root, nullptr, [&](const BoxHeader& b) {
        boxes.push_back(b);
        return true;
    });
    REQUIRE(boxes.size() == 3);
    CHECK(boxes[0].isFull);
    CHECK(boxes[0].version == 1);
    CHECK(boxes[0].flags == 3);
    CHECK(boxes[0].payload.size() == 4);
    CHECK(boxes[0].payload[0] == 0xDE);
    CHECK(boxes[0].payloadOffset() == 12);

    CHECK_FALSE(boxes[1].isFull);
    CHECK(BoxWalker::findChild(root, boxes[1], Fourcc{"hdlr"}, nullptr).has_value());
    CHECK(boxes[2].isFull);
    CHECK(BoxWalker::findChild(root, boxes[2], Fourcc{"hdlr"}, nullptr).has_value());
}

TEST_CASE("BoxWalker clamps a child past its parent and keeps siblings", "[container]") {
    // moov { good(12) ; bad claims 100 bytes but only 16 remain ; unreachable }
    std::vector<std::uint8_t> moovPayload;
    putBox(moovPayload, "mvhd", {0, 0, 0, 0});
    putU32(moovPayload, 100);
    moovPayload.insert(moovPayload.end(), {'t', 'r', 'a', 'k'});
    moovPayload.insert(moovPayload.end(), {1, 2, 3, 4, 5, 6, 7, 8});
    putBox(moovPayload, "udta", {});
    std::vector<std::uint8_t> buf;
    putBox(buf, "moov", moovPayload);
    // A sibling of moov at the top level must still be visited.
    putBox(buf, "free", {});

    const ByteSpan root(buf);
    WarningList warnings;
    std::vector<Fourcc> top;
    std::vector<BoxHeader> children;
    BoxWalker::forEachTopLevel(root, &warnings, [&](const BoxHeader& b) {
        top.push_back(b.type);
        if (b.type == Fourcc{"moov"}) {
            children = BoxWalker::children(root, b, &warnings);
        }
        return true;
    });
    REQUIRE(top.size() == 2);
    CHECK(top[1] == Fourcc{"free"});
    REQUIRE(children.size() == 2);
    CHECK(children[0].type == Fourcc{"mvhd"});
    CHECK(children[1].type == Fourcc{"trak"});
    CHECK(children[1].truncated);
    CHECK(children[1].declaredSize == 100);
    // Clamped to the parent end: the 16 bytes of 'trak' plus the 8-byte
    // 'udta' it swallowed (which is why that sibling is unreachable).
    CHECK(children[1].size == 24);
    CHECK(children[1].end() <= root.size());
    REQUIRE_FALSE(warnings.empty());
    CHECK(warnings[0].find("truncated") != std::string::npos);
}

TEST_CASE("BoxWalker rejects sizes smaller than the header and caps depth", "[container]") {
    std::vector<std::uint8_t> bad;
    putU32(bad, 4);
    bad.insert(bad.end(), {'f', 'r', 'e', 'e'});
    bad.insert(bad.end(), {0, 0, 0, 0});
    const Result<BoxHeader> h = BoxWalker::readHeader(ByteSpan(bad), 0, bad.size(), 0);
    REQUIRE_FALSE(h.ok());
    CHECK(h.code() == ErrorCode::Malformed);

    // Too short for a header.
    const std::vector<std::uint8_t> tiny = {0, 0, 0, 8, 'f', 'r'};
    const Result<BoxHeader> t = BoxWalker::readHeader(ByteSpan(tiny), 0, tiny.size(), 0);
    REQUIRE_FALSE(t.ok());
    CHECK(t.code() == ErrorCode::Truncated);

    // 20 nested 'moov' boxes: recursion must stop at kMaxDepth with a warning.
    std::vector<std::uint8_t> nested = {0xFF};
    for (int level = 0; level < 20; ++level) {
        std::vector<std::uint8_t> wrapped;
        putBox(wrapped, "moov", nested);
        nested = std::move(wrapped);
    }
    const ByteSpan root(nested);
    WarningList warnings;
    int deepest = -1;
    std::function<void(const BoxHeader&)> descend = [&](const BoxHeader& box) {
        deepest = box.depth > deepest ? box.depth : deepest;
        BoxWalker::forEachChild(root, box, &warnings, [&](const BoxHeader& child) {
            descend(child);
            return true;
        });
    };
    BoxWalker::forEachTopLevel(root, &warnings, [&](const BoxHeader& b) {
        descend(b);
        return true;
    });
    CHECK(deepest == BoxWalker::kMaxDepth);
    REQUIRE_FALSE(warnings.empty());
    CHECK(warnings.back().find("nesting") != std::string::npos);
}

// =============================================================================
//  HevcConfig / AvcConfig / Annex-B
// =============================================================================

TEST_CASE("HevcConfig parses a synthetic hvcC record", "[container]") {
    std::vector<std::uint8_t> rec = {
        1,                       // configurationVersion
        0x02,                    // profile_space 0, tier 0, profile_idc 2 (Main 10)
        0x20, 0x00, 0x00, 0x00,  // compatibility flags
        0x90, 0, 0, 0, 0, 0,     // constraint flags
        153,                     // level 5.1
        0xF0, 0x00,              // min_spatial_segmentation_idc
        0xFC,                    // parallelismType 0
        0xFD,                    // chromaFormat 1
        0xFA,                    // bitDepthLumaMinus8 = 2
        0xFA,                    // bitDepthChromaMinus8 = 2
        0x00, 0x00,              // avgFrameRate
        0x0F,                    // constantFrameRate 0, numTemporalLayers 1, nested 1, lengthSizeMinusOne 3
        2,                       // numOfArrays
        // array 0: VPS
        0xA0, 0x00, 0x01, 0x00, 0x03, 0x40, 0x01, 0x0C,
        // array 1: SPS
        0xA1, 0x00, 0x01, 0x00, 0x02, 0x42, 0x01,
    };
    const Result<HevcConfig> r = HevcConfig::parse(ByteSpan(rec));
    REQUIRE(r.ok());
    const HevcConfig& cfg = r.value();
    CHECK(cfg.generalProfileIdc == 2);
    CHECK(cfg.generalLevelIdc == 153);
    CHECK(cfg.chromaFormatIdc == 1);
    CHECK(cfg.bitDepthLuma() == 10);
    CHECK(cfg.bitDepthChroma() == 10);
    CHECK(cfg.nalLengthSize() == 4);
    CHECK(cfg.numTemporalLayers == 1);
    CHECK(cfg.temporalIdNested);
    REQUIRE(cfg.arrays.size() == 2);
    CHECK(cfg.arrays[0].nalType == 32);
    CHECK(cfg.arrays[0].arrayCompleteness);
    CHECK(cfg.vps().size() == 3);
    CHECK(cfg.sps().size() == 2);
    CHECK(cfg.pps().empty());
    CHECK(cfg.nalUnitCount() == 2);
    const std::vector<std::uint8_t> annexB = cfg.toAnnexBHeader();
    REQUIRE(annexB.size() == 4 + 3 + 4 + 2);
    CHECK(annexB[0] == 0);
    CHECK(annexB[3] == 1);
    CHECK(annexB[4] == 0x40);
    CHECK(annexB[11] == 0x42);

    // Truncated inside a NAL unit -> Truncated, not a crash.
    const Result<HevcConfig> cut = HevcConfig::parse(ByteSpan(rec.data(), rec.size() - 2));
    REQUIRE_FALSE(cut.ok());
    CHECK(cut.code() == ErrorCode::Truncated);
}

TEST_CASE("convertSampleToAnnexB re-frames length-prefixed NAL units", "[container]") {
    const std::vector<std::uint8_t> sample = {0, 0, 0, 2, 0x26, 0x01, 0, 0, 0, 3, 0x02, 0x01, 0xFF};
    const Result<std::vector<std::uint8_t>> ok = convertSampleToAnnexB(ByteSpan(sample), 4);
    REQUIRE(ok.ok());
    const std::vector<std::uint8_t> expected = {0, 0, 0, 1, 0x26, 0x01, 0, 0, 0, 1, 0x02, 0x01, 0xFF};
    CHECK(ok.value() == expected);

    // A length running past the end is Malformed.
    const std::vector<std::uint8_t> bad = {0, 0, 0, 9, 0x26};
    const Result<std::vector<std::uint8_t>> fail = convertSampleToAnnexB(ByteSpan(bad), 4);
    REQUIRE_FALSE(fail.ok());
    CHECK(fail.code() == ErrorCode::Malformed);
    CHECK_FALSE(convertSampleToAnnexB(ByteSpan(sample), 0).ok());
}

TEST_CASE("ColrNclx parses nclx and nclc", "[container]") {
    const std::vector<std::uint8_t> nclx = {'n', 'c', 'l', 'x', 0, 9, 0, 18, 0, 9, 0x80};
    const Result<ColrNclx> a = ColrNclx::parse(ByteSpan(nclx));
    REQUIRE(a.ok());
    CHECK(a.value().hasCodePoints());
    CHECK(a.value().primaries == 9);
    CHECK(a.value().transfer == 18);
    CHECK(a.value().matrix == 9);
    CHECK(a.value().fullRange);

    const std::vector<std::uint8_t> nclc = {'n', 'c', 'l', 'c', 0, 1, 0, 1, 0, 1};
    const Result<ColrNclx> b = ColrNclx::parse(ByteSpan(nclc));
    REQUIRE(b.ok());
    CHECK(b.value().primaries == 1);
    CHECK_FALSE(b.value().fullRange);

    const std::vector<std::uint8_t> icc = {'r', 'I', 'C', 'C', 1, 2, 3};
    const Result<ColrNclx> c = ColrNclx::parse(ByteSpan(icc));
    REQUIRE(c.ok());
    CHECK_FALSE(c.value().hasCodePoints());
    CHECK(c.value().raw.size() == 7);

    CHECK_FALSE(ColrNclx::parse(ByteSpan(nclx.data(), 6)).ok());
}

// =============================================================================
//  Synthetic fixtures
// =============================================================================

TEST_CASE("Fixture: largesize boxes, co64, ctts, edit list and udta", "[container]") {
    const Result<OsvFile> opened = OsvFile::open(fixture("fx_largesize.mp4"));
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();

    // A clean file must produce no diagnostics at all.
    CHECK(movie.warnings.empty());

    // Top level: ftyp, mdat(largesize), moov(largesize), free(size 0).
    REQUIRE(movie.topLevel.size() == 4);
    CHECK(movie.topLevel[0].type == Fourcc{"ftyp"});
    CHECK(movie.topLevel[1].type == Fourcc{"mdat"});
    CHECK(movie.topLevel[1].largeSize);
    CHECK(movie.topLevel[1].headerSize == 16);
    CHECK(movie.topLevel[2].type == Fourcc{"moov"});
    CHECK(movie.topLevel[2].largeSize);
    CHECK(movie.topLevel[3].type == Fourcc{"free"});
    CHECK(movie.topLevel[3].toEnd);
    CHECK(movie.topLevel[3].end() == file.size());
    REQUIRE(movie.moovBox.has_value());
    CHECK(movie.moovBox->largeSize);
    REQUIRE(movie.mdatBox.has_value());
    CHECK_FALSE(movie.camdBox.has_value());
    CHECK_FALSE(movie.indexTable.present);

    // ftyp / mvhd.
    REQUIRE(movie.hasFileType);
    CHECK(movie.fileType.majorBrand == Fourcc{"isom"});
    CHECK(movie.fileType.compatibleBrands.size() == 3);
    REQUIRE(movie.hasMovieHeader);
    CHECK(movie.timescale() == 1000);
    CHECK(movie.duration() == 700);
    CHECK_THAT(movie.durationSeconds(), WithinRel(0.7, 1e-9));
    CHECK(movie.header.nextTrackId == 2);

    // The one video track.
    REQUIRE(movie.tracks.size() == 1);
    const TrackInfo* track = file.track(1);
    REQUIRE(track != nullptr);
    CHECK(track->kind == TrackKind::Video);
    CHECK(track->tkhdFlags == 3);
    CHECK(track->enabled());
    CHECK(track->inMovie());
    CHECK_THAT(track->width, WithinRel(64.0, 1e-9));
    CHECK_THAT(track->height, WithinRel(32.0, 1e-9));
    CHECK(track->timescale == 1000);
    CHECK(track->duration == 700);
    CHECK(track->language == "und");
    CHECK(track->handler == Fourcc{"vide"});
    CHECK(track->handlerName == "FixtureVideo");
    CHECK(track->sampleEntry == Fourcc{"avc1"});
    CHECK(track->sampleEntryCount == 1);
    CHECK(track->dataReferenceIndex == 1);
    CHECK(track->codedWidth() == 64);
    CHECK(track->codedHeight() == 32);
    REQUIRE(track->video.has_value());
    CHECK(track->video->compressorName == "OpenOSV");
    CHECK(track->video->depth == 0x18);
    CHECK(track->video->frameCount == 1);
    REQUIRE(track->avc() != nullptr);
    CHECK(track->avc()->avcProfileIndication == 66);
    CHECK(track->avc()->nalLengthSize() == 4);
    REQUIRE(track->avc()->sps.size() == 1);
    REQUIRE(track->avc()->pps.size() == 1);
    CHECK(track->avc()->sps[0].size() == 8);
    CHECK(track->avc()->pps[0].size() == 4);
    CHECK(track->avc()->toAnnexBHeader().size() == 4 + 8 + 4 + 4);
    CHECK(track->hevc() == nullptr);
    REQUIRE(track->colr() != nullptr);
    CHECK(track->colr()->primaries == 9);
    CHECK(track->colr()->transfer == 18);
    CHECK(track->colr()->matrix == 9);
    REQUIRE(track->video->pasp.has_value());
    CHECK(track->video->pasp->hSpacing == 1);
    CHECK(track->video->pasp->vSpacing == 1);
    REQUIRE(track->editList.size() == 1);
    CHECK(track->editList[0].segmentDuration == 700);
    CHECK(track->editList[0].mediaTime == 100);
    CHECK(track->editMediaTime() == 100);
    CHECK_THAT(track->frameRate(), WithinRel(5.0 * 1000.0 / 700.0, 1e-9));
    CHECK(file.tracksOfKind(TrackKind::Video).size() == 1);
    CHECK(file.tracksOfKind(TrackKind::Audio).empty());

    // Sample table: sizes, chunk layout (co64), timing, composition, sync.
    const SampleTable& st = track->samples;
    REQUIRE(st.count() == 5);
    CHECK(st.chunkCount() == 2);
    CHECK(st.hasSyncTable());
    CHECK(st.hasCompositionOffsets());
    CHECK(st.constantSampleSize() == 0);
    CHECK(st.totalDuration() == 700);
    CHECK(st.totalSize() == 150);
    CHECK(st.maxSampleSize() == 50);
    const std::uint64_t dataStart = movie.mdatBox->bodyOffset();
    const std::uint64_t expectedOffsets[5] = {dataStart, dataStart + 10, dataStart + 30 + 7, dataStart + 30 + 7 + 30,
                                              dataStart + 30 + 7 + 30 + 40};
    const std::uint64_t expectedDts[5] = {0, 100, 200, 300, 500};
    const std::uint64_t expectedDur[5] = {100, 100, 100, 200, 200};
    const std::int64_t expectedCts[5] = {0, 0, 0, 100, 100};
    const bool expectedSync[5] = {true, false, false, true, false};
    const std::uint32_t expectedChunk[5] = {0, 0, 1, 1, 1};
    for (std::uint32_t i = 0; i < 5; ++i) {
        INFO("sample " << i);
        const Result<SampleLocation> loc = st.locate(i);
        REQUIRE(loc.ok());
        CHECK(loc.value().size == kFixtureSampleSizes[i]);
        CHECK(loc.value().offset == expectedOffsets[i]);
        CHECK(loc.value().dts == expectedDts[i]);
        CHECK(loc.value().duration == expectedDur[i]);
        CHECK(loc.value().ctsOffset == expectedCts[i]);
        CHECK(loc.value().pts() == expectedDts[i] + static_cast<std::uint64_t>(expectedCts[i]));
        CHECK(loc.value().sync == expectedSync[i]);
        CHECK(loc.value().chunkIndex == expectedChunk[i]);
        CHECK(st.isSync(i) == expectedSync[i]);
        // The bytes come straight out of the mapping and carry the pattern.
        const Result<ByteSpan> bytes = file.sample(1, i);
        REQUIRE(bytes.ok());
        CHECK(bytes.value().size() == kFixtureSampleSizes[i]);
        CHECK(allBytesEqual(bytes.value(), static_cast<std::uint8_t>(i + 1)));
        CHECK(bytes.value().data() == file.span().data() + expectedOffsets[i]);
    }
    CHECK(st.previousSync(0) == 0);
    CHECK(st.previousSync(2) == 0);
    CHECK(st.previousSync(3) == 3);
    CHECK(st.previousSync(4) == 3);
    CHECK(st.previousSync(99) == 3);
    CHECK(st.nextSync(0) == 3);
    CHECK(st.nextSync(3) == 5);
    CHECK(st.syncSamples() == std::vector<std::uint32_t>{0, 3});
    CHECK_FALSE(st.locate(5).ok());
    CHECK(st.locate(5).code() == ErrorCode::NotFound);
    CHECK(file.sample(1, 5).code() == ErrorCode::NotFound);
    CHECK(file.sample(2, 0).code() == ErrorCode::NotFound);

    // User data.
    const UserData& u = movie.udta;
    REQUIRE(u.dbpm.has_value());
    CHECK(*u.dbpm == 21);
    REQUIRE(u.dbcm.has_value());
    CHECK(*u.dbcm == 3);
    CHECK(u.btec == "beauty_enable=0;smoother=0");
    CHECK(u.fsid == "/fixtures/CAM_0001.OSV");
    CHECK(u.tool == "OpenOSV fixture");
    CHECK(u.uid.size() == 4);
    REQUIRE(u.ilst.size() == 2);
    const IlstItem* covr = u.ilstItem(Fourcc{"covr"});
    REQUIRE(covr != nullptr);
    CHECK(covr->dataType == 13);
    CHECK(covr->data.size() == kFixtureCoverBytes);
    CHECK(covr->dataOffset == covr->itemOffset + 24);
    CHECK(movie.covers.covr.size() == kFixtureCoverBytes);
    CHECK(movie.covers.covr[0] == 0xFF);
    CHECK(movie.covers.covr[1] == 0xD8);
    CHECK(movie.covers.snal.empty());
    CHECK(movie.covers.tnal.empty());
    const XtraEntry* xtra = u.xtraEntry("WM/Category");
    REQUIRE(xtra != nullptr);
    CHECK(xtra->valueType == 8);
    CHECK(xtra->text == "pb_file:fixture.proto;model_name:FX001");
    CHECK_FALSE(u.xtraRaw.empty());
    CHECK(u.unknownBoxes.empty());

    // camdMovie on a file without camd is NotFound, not a crash.
    CHECK(file.camdMovie().code() == ErrorCode::NotFound);
    CHECK_FALSE(file.hasCamd());
}

TEST_CASE("Fixture: size-0 moov as the last box with an stco table", "[container]") {
    const Result<OsvFile> opened = OsvFile::open(fixture("fx_moov_size0.mp4"));
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();
    CHECK(movie.warnings.empty());
    REQUIRE(movie.moovBox.has_value());
    CHECK(movie.moovBox->toEnd);
    CHECK(movie.moovBox->end() == file.size());
    REQUIRE(movie.tracks.size() == 1);
    const TrackInfo& track = movie.tracks[0];
    CHECK(track.samples.count() == 5);
    CHECK(track.samples.chunkCount() == 2);
    CHECK(track.samples.sampleOffset(0) == movie.mdatBox->bodyOffset());
    for (std::uint32_t i = 0; i < 5; ++i) {
        const Result<ByteSpan> bytes = file.sample(1, i);
        REQUIRE(bytes.ok());
        CHECK(allBytesEqual(bytes.value(), static_cast<std::uint8_t>(i + 1)));
    }
    // No ilst in this fixture: covers stay empty, the rest of udta is there.
    CHECK(movie.covers.covr.empty());
    CHECK(movie.udta.ilst.empty());
    CHECK(movie.udta.dbpm.has_value());
}

TEST_CASE("Fixture: index table, djmd track and nested camd movie", "[container]") {
    const Result<OsvFile> opened = OsvFile::open(fixture("fx_camd.mp4"));
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();

    // Top level: ftyp, free(index), mdat, moov, camd.
    REQUIRE(movie.topLevel.size() == 5);
    CHECK(movie.topLevel[1].type == Fourcc{"free"});
    CHECK(movie.topLevel[4].type == Fourcc{"camd"});
    REQUIRE(movie.camdBox.has_value());
    CHECK(file.hasCamd());
    CHECK(movie.topLevelBox(Fourcc{"camd"}) != nullptr);
    CHECK(movie.topLevelBox(Fourcc{"moof"}) == nullptr);

    // Index table: covr and camd verified, the bogus entry rejected.
    const IndexTable& index = movie.indexTable;
    REQUIRE(index.present);
    CHECK(index.boxOffset == 28);
    REQUIRE(index.entries.size() == 3);
    CHECK(index.verifiedCount() == 2);
    const IndexEntry* covr = index.find(Fourcc{"covr"});
    REQUIRE(covr != nullptr);
    CHECK(covr->verified);
    CHECK(covr->size == kFixtureCoverBytes);
    CHECK(covr->dataOffset == covr->offset + 24);
    CHECK(covr->payload(file.span()).size() == kFixtureCoverBytes);
    CHECK(covr->payload(file.span()).data() == movie.covers.covr.data());
    const IndexEntry* camd = index.find(Fourcc{"camd"});
    REQUIRE(camd != nullptr);
    CHECK(camd->verified);
    CHECK(camd->dataOffset == movie.camdBox->bodyOffset());
    CHECK(camd->size == movie.camdBox->body.size());
    const IndexEntry* bogus = index.find(Fourcc{"bogx"});
    REQUIRE(bogus != nullptr);
    CHECK_FALSE(bogus->verified);
    CHECK_FALSE(bogus->note.empty());
    CHECK(bogus->payload(file.span()).empty());
    bool warned = false;
    for (const std::string& w : movie.warnings) {
        warned = warned || w.find("bogx") != std::string::npos;
    }
    CHECK(warned);

    // The djmd track with a constant sample size.
    REQUIRE(movie.tracks.size() == 1);
    const TrackInfo& track = movie.tracks[0];
    CHECK(track.kind == TrackKind::Djmd);
    CHECK(track.isDjmd());
    CHECK(track.handler == Fourcc{"meta"});
    CHECK(track.handlerName == "CAM meta");
    CHECK(track.sampleEntry == Fourcc{"djmd"});
    CHECK(track.tkhdFlags == 2);
    CHECK_FALSE(track.enabled());
    CHECK_FALSE(track.video.has_value());
    CHECK_FALSE(track.audio.has_value());
    CHECK(track.samples.constantSampleSize() == kFixtureDjmdSampleSize);
    REQUIRE(track.samples.count() == kFixtureDjmdSampleCount);
    CHECK_FALSE(track.samples.hasSyncTable());
    CHECK(track.samples.isSync(3));
    CHECK(track.samples.previousSync(3) == 3);
    CHECK_THAT(track.frameRate(), WithinRel(60000.0 / 1001.0, 1e-9));
    for (std::uint32_t i = 0; i < kFixtureDjmdSampleCount; ++i) {
        const Result<ByteSpan> bytes = file.sample(1, i);
        REQUIRE(bytes.ok());
        CHECK(bytes.value().size() == kFixtureDjmdSampleSize);
        CHECK(allBytesEqual(bytes.value(), static_cast<std::uint8_t>(i + 1)));
    }
    CHECK(file.tracksOfKind(TrackKind::Djmd).size() == 1);

    // The nested movie: offsets are relative to the camd payload.
    const Result<MovieInfo> nestedResult = file.camdMovie();
    REQUIRE(nestedResult.ok());
    const MovieInfo& nested = nestedResult.value();
    CHECK(nested.warnings.empty());
    CHECK(nested.source.data() == movie.camdBox->body.data());
    CHECK(nested.source.size() == movie.camdBox->body.size());
    CHECK(nested.hasFileType);
    CHECK(nested.timescale() == 60000);
    REQUIRE(nested.tracks.size() == 1);
    CHECK(nested.tracks[0].kind == TrackKind::Djmd);
    REQUIRE(nested.tracks[0].samples.count() == 3);
    for (std::uint32_t i = 0; i < 3; ++i) {
        const Result<ByteSpan> bytes = readSample(nested, 1, i);
        REQUIRE(bytes.ok());
        CHECK(bytes.value().size() == kFixtureCamdSampleSizes[i]);
        CHECK(allBytesEqual(bytes.value(), static_cast<std::uint8_t>(i + 1)));
        // Still inside the outer file's camd payload.
        CHECK(bytes.value().data() >= movie.camdBox->body.data());
        CHECK(bytes.value().data() + bytes.value().size() <= movie.camdBox->body.end());
    }
    CHECK(readSample(nested, 1, 3).code() == ErrorCode::NotFound);
    CHECK(readSample(nested, 7, 0).code() == ErrorCode::NotFound);
}

TEST_CASE("Fixture: truncated moov fails with Truncated and warnings", "[container]") {
    const Result<OsvFile> opened = OsvFile::open(fixture("fx_truncated_moov.mp4"));
    REQUIRE_FALSE(opened.ok());
    CHECK(opened.code() == ErrorCode::Truncated);

    WarningList warnings;
    const std::vector<std::uint8_t> bytes = readFile(fixture("fx_truncated_moov.mp4"));
    const Result<MovieInfo> movie = OsvFile::parseMovie(ByteSpan(bytes), &warnings);
    CHECK_FALSE(movie.ok());
    CHECK_FALSE(warnings.empty());
}

TEST_CASE("OsvFile::fromBuffer parses in-memory bytes", "[container]") {
    std::vector<std::uint8_t> bytes = readFile(fixture("fx_largesize.mp4"));
    Result<OsvFile> opened = OsvFile::fromBuffer(std::move(bytes), "largesize-in-memory");
    REQUIRE(opened.ok());
    CHECK(opened.value().path().string() == "largesize-in-memory");
    CHECK(opened.value().movie().tracks.size() == 1);
    CHECK(opened.value().sample(1, 4).ok());

    // Moving the file keeps the spans valid (storage moves with it).
    OsvFile moved = std::move(opened).value();
    const Result<ByteSpan> s = moved.sample(1, 4);
    REQUIRE(s.ok());
    CHECK(allBytesEqual(s.value(), 5));

    // A default-constructed file answers cleanly.
    const OsvFile empty;
    CHECK(empty.sample(1, 0).code() == ErrorCode::InvalidArgument);
    CHECK(empty.camdMovie().code() == ErrorCode::InvalidArgument);
    CHECK(OsvFile::open(std::filesystem::path{}).code() == ErrorCode::InvalidArgument);
    CHECK(OsvFile::open(osvtest::fixtureDir() / "does_not_exist.mp4").code() == ErrorCode::Io);
}

// =============================================================================
//  Robustness (always run)
// =============================================================================

TEST_CASE("Garbage input is rejected as Malformed", "[container][fuzz]") {
    std::mt19937 rng(0xC0FFEEu);
    std::vector<std::uint8_t> garbage(64 * 1024);
    for (std::uint8_t& b : garbage) {
        b = static_cast<std::uint8_t>(rng());
    }
    WarningList warnings;
    const Result<MovieInfo> r = OsvFile::parseMovie(ByteSpan(garbage), &warnings);
    REQUIRE_FALSE(r.ok());
    CHECK(r.code() == ErrorCode::Malformed);

    // Garbage behind a valid ftyp header must not crash either.
    std::vector<std::uint8_t> withFtyp;
    putBox(withFtyp, "ftyp", {'i', 's', 'o', 'm', 0, 0, 0, 0});
    withFtyp.insert(withFtyp.end(), garbage.begin(), garbage.end());
    const Result<MovieInfo> r2 = OsvFile::parseMovie(ByteSpan(withFtyp), nullptr);
    CHECK_FALSE(r2.ok());

    // Empty and tiny inputs.
    CHECK(OsvFile::parseMovie(ByteSpan{}, nullptr).code() == ErrorCode::Truncated);
    const std::vector<std::uint8_t> tiny = {0, 0, 0, 8, 'f', 't', 'y', 'p'};
    CHECK_FALSE(OsvFile::parseMovie(ByteSpan(tiny), nullptr).ok());
}

TEST_CASE("Every truncation of a fixture parses without crashing", "[container][fuzz]") {
    const std::vector<std::uint8_t> bytes = readFile(fixture("fx_largesize.mp4"));
    const Result<MovieInfo> whole = OsvFile::parseMovie(ByteSpan(bytes), nullptr);
    REQUIRE(whole.ok());
    const std::uint64_t moovEnd = whole.value().moovBox->end();

    for (std::size_t len = 0; len <= bytes.size(); ++len) {
        WarningList warnings;
        const Result<MovieInfo> r = OsvFile::parseMovie(ByteSpan(bytes.data(), len), &warnings);
        if (len >= moovEnd) {
            // Everything up to and including moov is intact: must parse.
            INFO("length " << len);
            REQUIRE(r.ok());
            REQUIRE(r.value().tracks.size() == 1);
        } else if (r.ok()) {
            // Cut inside moov but salvaged: there must be a diagnostic.
            INFO("length " << len);
            CHECK_FALSE(warnings.empty());
        }
    }
}

TEST_CASE("Bit flips in a fixture never crash the parser", "[container][fuzz]") {
    const std::vector<std::uint8_t> original = readFile(fixture("fx_camd.mp4"));
    std::mt19937 rng(20260916u);
    for (int iteration = 0; iteration < 300; ++iteration) {
        std::vector<std::uint8_t> bytes = original;
        // Flip 1..4 random bytes.
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) {
            bytes[rng() % bytes.size()] = static_cast<std::uint8_t>(rng());
        }
        WarningList warnings;
        const Result<MovieInfo> r = OsvFile::parseMovie(ByteSpan(bytes), &warnings);
        if (r.ok()) {
            // Whatever survived must answer sample queries without faults.
            for (const TrackInfo& t : r.value().tracks) {
                for (std::uint32_t i = 0; i < t.samples.count() && i < 8; ++i) {
                    (void)readSample(r.value(), t.trackId, i);
                }
            }
            if (r.value().camdBox) {
                (void)OsvFile::parseMovie(r.value().camdBox->body, nullptr);
            }
        }
    }
    SUCCEED("300 mutated parses completed");
}

// =============================================================================
//  The sample clip
// =============================================================================

TEST_CASE("Sample: tracks, codec configuration and timing", "[container][sample]") {
    OSV_REQUIRE_SAMPLE();
    const Result<OsvFile> opened = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();

    // ftyp / top level.
    REQUIRE(movie.hasFileType);
    CHECK(movie.fileType.majorBrand == Fourcc{"isom"});
    REQUIRE(movie.topLevel.size() == 6);
    CHECK(movie.topLevel[0].type == Fourcc{"ftyp"});
    CHECK(movie.topLevel[0].size == 28);
    CHECK(movie.topLevel[1].type == Fourcc{"free"});
    CHECK(movie.topLevel[2].type == Fourcc{"free"});
    CHECK(movie.topLevel[2].offset == 36);
    CHECK(movie.topLevel[3].type == Fourcc{"mdat"});
    CHECK(movie.topLevel[4].type == Fourcc{"moov"});
    CHECK(movie.topLevel[5].type == Fourcc{"camd"});
    REQUIRE(movie.mdatBox.has_value());
    CHECK(movie.mdatBox->offset == 4088);
    CHECK(movie.timescale() == 60000);
    // mvhd duration is the longest track: the audio (51 AAC frames of 1024
    // samples at 48 kHz = 1.088 s) outlasts the 65 video frames (1.0844 s).
    CHECK_THAT(movie.durationSeconds(), WithinRel(51.0 * 1024.0 / 48000.0, 1e-6));
    CHECK(movie.durationSeconds() >= 65.0 * 1001.0 / 60000.0);

    // Seven tracks with the expected kinds and ids.
    REQUIRE(movie.tracks.size() == 7);
    const TrackKind expectedKinds[7] = {TrackKind::Video, TrackKind::Video, TrackKind::Audio, TrackKind::Djmd,
                                        TrackKind::Djmd,  TrackKind::Dbgi,  TrackKind::Dbgi};
    for (std::size_t i = 0; i < 7; ++i) {
        INFO("track index " << i);
        CHECK(movie.tracks[i].trackId == i + 1);
        CHECK(movie.tracks[i].kind == expectedKinds[i]);
    }
    CHECK(file.tracksOfKind(TrackKind::Video).size() == 2);
    CHECK(file.tracksOfKind(TrackKind::Audio).size() == 1);
    CHECK(file.tracksOfKind(TrackKind::Djmd).size() == 2);
    CHECK(file.tracksOfKind(TrackKind::Dbgi).size() == 2);
    CHECK(file.tracksOfKind(TrackKind::Other).empty());
    CHECK(file.track(8) == nullptr);

    // Video tracks: hvc1 3000x3000, Main 10, 4:2:0, 4-byte NAL lengths, bt709 tags.
    for (std::uint32_t id = 1; id <= 2; ++id) {
        INFO("video track " << id);
        const TrackInfo* t = file.track(id);
        REQUIRE(t != nullptr);
        CHECK(t->handler == Fourcc{"vide"});
        CHECK(t->sampleEntry == Fourcc{"hvc1"});
        CHECK(t->codedWidth() == 3000);
        CHECK(t->codedHeight() == 3000);
        CHECK_THAT(t->width, WithinRel(3000.0, 1e-9));
        CHECK_THAT(t->height, WithinRel(3000.0, 1e-9));
        CHECK(t->timescale == 60000);
        CHECK(t->tkhdFlags == (id == 1 ? 3u : 2u));
        REQUIRE(t->hevc() != nullptr);
        const HevcConfig& hevc = *t->hevc();
        CHECK(hevc.generalProfileIdc == 2);
        CHECK(hevc.bitDepthLuma() == 10);
        CHECK(hevc.bitDepthChroma() == 10);
        CHECK(hevc.chromaFormatIdc == 1);
        CHECK(hevc.nalLengthSize() == 4);
        CHECK_FALSE(hevc.vps().empty());
        CHECK_FALSE(hevc.sps().empty());
        CHECK_FALSE(hevc.pps().empty());
        const std::vector<std::uint8_t> header = hevc.toAnnexBHeader();
        REQUIRE(header.size() > 4);
        CHECK(header[0] == 0);
        CHECK(header[1] == 0);
        CHECK(header[2] == 0);
        CHECK(header[3] == 1);
        REQUIRE(t->colr() != nullptr);
        CHECK(t->colr()->colourType == Fourcc{"nclx"});
        CHECK(t->colr()->primaries == 1);
        CHECK(t->colr()->transfer == 1);
        CHECK(t->colr()->matrix == 1);
        CHECK_FALSE(t->colr()->fullRange);

        // 65 samples, keyframes at 0 and 60, 59.94 fps.
        const SampleTable& st = t->samples;
        REQUIRE(st.count() == 65);
        CHECK(st.hasSyncTable());
        CHECK(st.syncSamples() == std::vector<std::uint32_t>{0, 60});
        CHECK(st.isSync(0));
        CHECK(st.isSync(60));
        CHECK_FALSE(st.isSync(1));
        CHECK_FALSE(st.isSync(59));
        CHECK(st.previousSync(0) == 0);
        CHECK(st.previousSync(59) == 0);
        CHECK(st.previousSync(60) == 60);
        CHECK(st.previousSync(64) == 60);
        CHECK(st.nextSync(0) == 60);
        CHECK(st.nextSync(60) == 65);
        CHECK(st.sampleDuration(0) == 1001);
        CHECK(st.sampleDts(64) == 64 * 1001);
        CHECK_FALSE(st.hasCompositionOffsets());
        CHECK(st.totalDuration() == 65 * 1001);
        CHECK_THAT(t->frameRate(), WithinRel(60000.0 / 1001.0, 1e-9));
        CHECK_THAT(t->frameRate(), WithinRel(59.94, 1e-4));
    }

    // Audio: AAC-LC 48 kHz stereo.
    const TrackInfo* audio = file.track(3);
    REQUIRE(audio != nullptr);
    CHECK(audio->sampleEntry == Fourcc{"mp4a"});
    CHECK(audio->timescale == 48000);
    REQUIRE(audio->audio.has_value());
    CHECK(audio->audio->channelCount == 2);
    CHECK_THAT(audio->audio->sampleRate, WithinRel(48000.0, 1e-9));
    REQUIRE(audio->audio->esds.has_value());
    CHECK(audio->audio->esds->objectTypeIndication == 0x40);
    CHECK(audio->audio->esds->streamType == 5);
    CHECK(audio->audio->esds->audioObjectType() == 2);
    CHECK(audio->samples.count() == 51);

    // Metadata tracks.
    for (std::uint32_t id = 4; id <= 5; ++id) {
        const TrackInfo* t = file.track(id);
        REQUIRE(t != nullptr);
        CHECK(t->handler == Fourcc{"meta"});
        CHECK(t->handlerName == "CAM meta");
        CHECK(t->sampleEntry == Fourcc{"djmd"});
        CHECK(t->samples.count() == 65);
        CHECK(t->timescale == 60000);
        CHECK(t->samples.sampleDuration(0) == 1001);
    }
    CHECK(file.track(6)->sampleEntry == Fourcc{"dbgi"});
    CHECK(file.track(7)->sampleEntry == Fourcc{"dbgi"});
}

TEST_CASE("Sample: index table, cover art and user data", "[container][sample]") {
    OSV_REQUIRE_SAMPLE();
    const Result<OsvFile> opened = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();

    // Index table at offset 36 with three verified entries.
    const IndexTable& index = movie.indexTable;
    REQUIRE(index.present);
    CHECK(index.boxOffset == 36);
    CHECK(index.boxSize == 4052);
    REQUIRE(index.entries.size() == 3);
    CHECK(index.verifiedCount() == 3);
    const IndexEntry* covr = index.find(Fourcc{"covr"});
    const IndexEntry* snal = index.find(Fourcc{"snal"});
    const IndexEntry* camd = index.find(Fourcc{"camd"});
    REQUIRE(covr != nullptr);
    REQUIRE(snal != nullptr);
    REQUIRE(camd != nullptr);
    CHECK(covr->verified);
    CHECK(snal->verified);
    CHECK(camd->verified);
    CHECK(covr->size == 39258);
    CHECK(snal->size == 39258);
    CHECK(covr->payload(file.span()).data() == movie.covers.covr.data());
    CHECK(snal->payload(file.span()).data() == movie.covers.snal.data());
    REQUIRE(movie.camdBox.has_value());
    CHECK(camd->dataOffset == movie.camdBox->bodyOffset());
    CHECK(camd->size == movie.camdBox->body.size());
    CHECK(camd->payload(file.span()).data() == movie.camdBox->body.data());

    // Cover JPEGs.
    REQUIRE(movie.covers.covr.size() == 39258);
    CHECK(movie.covers.covr[0] == 0xFF);
    CHECK(movie.covers.covr[1] == 0xD8);
    CHECK(movie.covers.covr[39256] == 0xFF);
    CHECK(movie.covers.covr[39257] == 0xD9);
    CHECK(movie.covers.snal.equals(movie.covers.covr));
    REQUIRE_FALSE(movie.covers.tnal.empty());
    CHECK(movie.covers.tnal[0] == 0xFF);
    CHECK(movie.covers.tnal[1] == 0xD8);
    CHECK(movie.covers.tnal.size() < movie.covers.covr.size());

    // udta.
    const UserData& u = movie.udta;
    CHECK(u.tool == "Osmo 360");
    REQUIRE(u.dbpm.has_value());
    CHECK(*u.dbpm == 0x15);
    REQUIRE(u.dbcm.has_value());
    CHECK(*u.dbcm == 3);
    CHECK(u.btec.rfind("beauty_enable=0", 0) == 0);
    CHECK(u.fsid.rfind("/mnt/media_rw/sd/DCIM/", 0) == 0);
    CHECK(u.uid.size() == 4);
    const XtraEntry* category = u.xtraEntry("WM/Category");
    REQUIRE(category != nullptr);
    CHECK(category->valueType == 8);
    CHECK(category->text.find("pb_file:dvtm_oq101.proto") != std::string::npos);
    CHECK(category->text.find("model_name:OQ001") != std::string::npos);
    // Every string we expose is 7-bit clean.
    for (const std::string& s : {u.tool, u.btec, u.fsid, category->text}) {
        for (const char c : s) {
            CHECK(static_cast<unsigned char>(c) < 0x80);
        }
    }
    CHECK(u.ilstItem(Fourcc{"covr"}) != nullptr);
    CHECK(u.ilstItem(Fourcc{"snal"}) != nullptr);
    CHECK(u.ilstItem(Fourcc{"tnal"}) != nullptr);
    CHECK(u.ilstItem(Fourcc{"covr"})->dataType == 13);
}

TEST_CASE("Sample: samples alias the mapping and the camd movie parses", "[container][sample]") {
    OSV_REQUIRE_SAMPLE();
    const Result<OsvFile> opened = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();

    // Metadata samples: sample 0 carries the full ClipMeta/StreamMeta.
    const Result<ByteSpan> meta0 = file.sample(4, 0);
    REQUIRE(meta0.ok());
    CHECK(meta0.value().size() == 6381);
    const Result<ByteSpan> meta1 = file.sample(4, 1);
    REQUIRE(meta1.ok());
    CHECK(meta1.value().size() == 553);
    const Result<ByteSpan> stripped0 = file.sample(5, 0);
    REQUIRE(stripped0.ok());
    CHECK(stripped0.value().size() == 312);
    // Protobuf field 1 (ClipMeta), wire type 2 -> first byte 0x0A.
    CHECK(meta0.value()[0] == 0x0A);
    CHECK(stripped0.value()[0] == 0x0A);

    // Video samples: sizes straight from stsz, bytes inside mdat.
    const Result<ByteSpan> v0 = file.sample(1, 0);
    REQUIRE(v0.ok());
    CHECK(v0.value().size() == 525091);
    CHECK(v0.value().size() == movie.tracks[0].samples.sampleSize(0));
    CHECK(v0.value().data() >= movie.mdatBox->body.data());
    CHECK(v0.value().data() + v0.value().size() <= movie.mdatBox->body.end());
    // The first 4-byte NAL length must fit inside the sample.
    const std::uint32_t nalLength = (static_cast<std::uint32_t>(v0.value()[0]) << 24) |
                                    (static_cast<std::uint32_t>(v0.value()[1]) << 16) |
                                    (static_cast<std::uint32_t>(v0.value()[2]) << 8) | v0.value()[3];
    CHECK(nalLength + 4 <= v0.value().size());
    const Result<std::vector<std::uint8_t>> annexB = convertSampleToAnnexB(v0.value(), 4);
    REQUIRE(annexB.ok());
    CHECK(annexB.value().size() == v0.value().size());
    const Result<ByteSpan> v1 = file.sample(2, 0);
    REQUIRE(v1.ok());
    CHECK(v1.value().size() == 478011);
    const Result<ByteSpan> last = file.sample(1, 64);
    REQUIRE(last.ok());
    CHECK(last.value().data() + last.value().size() <= file.span().end());
    CHECK(file.sample(1, 65).code() == ErrorCode::NotFound);
    CHECK(file.sample(99, 0).code() == ErrorCode::NotFound);

    // Nested camd movie: two djmd tracks of 120 samples each.
    const Result<MovieInfo> camdResult = file.camdMovie();
    REQUIRE(camdResult.ok());
    const MovieInfo& camd = camdResult.value();
    CHECK(camd.source.data() == movie.camdBox->body.data());
    CHECK(camd.hasFileType);
    REQUIRE(camd.moovBox.has_value());
    CHECK(camd.tracksOfKind(TrackKind::Djmd).size() == 2);
    CHECK(camd.tracks.size() == 2);
    for (const TrackInfo& t : camd.tracks) {
        INFO("camd track " << t.trackId);
        CHECK(t.kind == TrackKind::Djmd);
        CHECK(t.samples.count() == 120);
        for (std::uint32_t i = 0; i < t.samples.count(); ++i) {
            const Result<ByteSpan> s = readSample(camd, t.trackId, i);
            REQUIRE(s.ok());
            CHECK_FALSE(s.value().empty());
            CHECK(s.value().data() >= movie.camdBox->body.data());
            CHECK(s.value().data() + s.value().size() <= movie.camdBox->body.end());
        }
        // The first nested sample also starts with the ClipMeta field.
        CHECK(readSample(camd, t.trackId, 0).value()[0] == 0x0A);
    }
}

TEST_CASE("Sample: truncations never crash and are reported", "[container][sample][fuzz]") {
    OSV_REQUIRE_SAMPLE();
    const std::vector<std::uint8_t> bytes = readFile(osvtest::sampleOsv());
    const Result<MovieInfo> whole = OsvFile::parseMovie(ByteSpan(bytes), nullptr);
    REQUIRE(whole.ok());
    const std::uint64_t moovEnd = whole.value().moovBox->end();

    // Every top-level box boundary, plus one byte either side.
    std::vector<std::uint64_t> lengths;
    for (const BoxHeader& box : whole.value().topLevel) {
        lengths.push_back(box.offset);
        lengths.push_back(box.offset + 1);
        lengths.push_back(box.offset + 8);
        lengths.push_back(box.end() - 1);
        lengths.push_back(box.end());
    }
    // 100 pseudo-random lengths (fixed seed so failures reproduce).
    std::mt19937_64 rng(0x05C0DE2026ull);
    for (int i = 0; i < 100; ++i) {
        lengths.push_back(rng() % bytes.size());
    }

    for (const std::uint64_t len : lengths) {
        INFO("truncated to " << len << " bytes");
        WarningList warnings;
        const Result<MovieInfo> r = OsvFile::parseMovie(ByteSpan(bytes.data(), static_cast<std::size_t>(len)), &warnings);
        if (len < moovEnd) {
            // The movie is incomplete: either an error or a warning must say so.
            CHECK((!r.ok() || !warnings.empty()));
            if (!r.ok()) {
                CHECK((r.code() == ErrorCode::Truncated || r.code() == ErrorCode::Malformed));
            }
        } else {
            REQUIRE(r.ok());
            CHECK(r.value().tracks.size() == 7);
            // Samples that fall outside the truncated span are refused, not read.
            for (const TrackInfo& t : r.value().tracks) {
                const Result<ByteSpan> s = readSample(r.value(), t.trackId, t.samples.count() - 1);
                if (s.ok()) {
                    CHECK(s.value().data() + s.value().size() <= bytes.data() + len);
                }
            }
        }
    }
}

TEST_CASE("Sample: LRF proxy opens with one avc1 track and two djmd tracks", "[container][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (!haveLrf()) {
        SKIP("LRF proxy not available: " << osvtest::sampleLrf().string());
    }
    const Result<OsvFile> opened = OsvFile::open(osvtest::sampleLrf());
    REQUIRE(opened.ok());
    const OsvFile& file = opened.value();
    const MovieInfo& movie = file.movie();

    const std::vector<const TrackInfo*> video = file.tracksOfKind(TrackKind::Video);
    REQUIRE(video.size() == 1);
    CHECK(video[0]->sampleEntry == Fourcc{"avc1"});
    CHECK(video[0]->codedWidth() == 2048);
    CHECK(video[0]->codedHeight() == 1024);
    CHECK(video[0]->samples.count() == 124);
    CHECK(video[0]->timescale == 30000);
    CHECK_THAT(video[0]->frameRate(), WithinRel(30000.0 / 1001.0, 1e-9));
    REQUIRE(video[0]->avc() != nullptr);
    CHECK(video[0]->avc()->nalLengthSize() == 4);
    CHECK_FALSE(video[0]->avc()->sps.empty());
    CHECK_FALSE(video[0]->avc()->pps.empty());
    CHECK(video[0]->hevc() == nullptr);
    CHECK(video[0]->samples.isSync(0));
    CHECK(video[0]->samples.previousSync(25) == 20);

    const std::vector<const TrackInfo*> djmd = file.tracksOfKind(TrackKind::Djmd);
    REQUIRE(djmd.size() == 2);
    CHECK(djmd[0]->samples.count() == 124);
    CHECK(djmd[1]->samples.count() == 124);
    CHECK(file.tracksOfKind(TrackKind::Audio).size() == 1);
    CHECK(file.tracksOfKind(TrackKind::Dbgi).empty());

    // The LRF ftyp is 4 bytes longer, so its index table sits at 40.
    REQUIRE(movie.indexTable.present);
    CHECK(movie.indexTable.boxOffset == 40);
    CHECK(movie.indexTable.verifiedCount() == 3);
    CHECK(movie.covers.covr.size() == 39258);
    REQUIRE(file.hasCamd());
    const Result<MovieInfo> camd = file.camdMovie();
    REQUIRE(camd.ok());
    CHECK(camd.value().tracksOfKind(TrackKind::Djmd).size() == 2);

    const Result<ByteSpan> first = file.sample(video[0]->trackId, 0);
    REQUIRE(first.ok());
    CHECK(first.value().size() == 196769);
}
