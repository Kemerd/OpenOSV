// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for the hand-rolled protobuf layer of osv_meta: ProtoScanner wire
// decoding, the packed/unpacked repeated helpers, the schema-less tree and
// the typed DjmdDecoder on synthetic messages.  None of these need the sample
// clip; every message is built here with a tiny encoder.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/core/ByteSpan.h"
#include "osv/meta/CalibrationSelector.h"
#include "osv/meta/DjmdDecoder.h"
#include "osv/meta/ProtoTree.h"
#include "osv/meta/ProtoWire.h"
#include "osv/meta/Types.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace osv;
using namespace osv::meta;
using Catch::Matchers::WithinRel;

namespace {

// -----------------------------------------------------------------------------
//  Minimal protobuf encoder (independent of the library's decoder)
// -----------------------------------------------------------------------------

/// Builds a serialised message byte by byte.  Every method appends and
/// returns *this so messages can be written as one expression.
struct Pb {
    std::vector<std::uint8_t> b;

    /// Raw base-128 varint.
    Pb& varint(std::uint64_t v) {
        do {
            std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7Fu);
            v >>= 7;
            if (v != 0) {
                byte |= 0x80u;
            }
            b.push_back(byte);
        } while (v != 0);
        return *this;
    }
    /// Field tag (number << 3 | wire).
    Pb& tag(std::uint32_t number, unsigned wire) { return varint((static_cast<std::uint64_t>(number) << 3) | wire); }
    /// Varint field.
    Pb& vint(std::uint32_t number, std::uint64_t v) { return tag(number, 0).varint(v); }
    /// Fixed32 field from raw bits.
    Pb& fixed32(std::uint32_t number, std::uint32_t bits) {
        tag(number, 5);
        for (int i = 0; i < 4; ++i) {
            b.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
        }
        return *this;
    }
    /// Fixed32 float field.
    Pb& f32(std::uint32_t number, float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        return fixed32(number, bits);
    }
    /// Fixed64 field from raw bits.
    Pb& fixed64(std::uint32_t number, std::uint64_t bits) {
        tag(number, 1);
        for (int i = 0; i < 8; ++i) {
            b.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
        }
        return *this;
    }
    /// Fixed64 double field.
    Pb& f64(std::uint32_t number, double d) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        return fixed64(number, bits);
    }
    /// Length-delimited field from raw bytes.
    Pb& bytes(std::uint32_t number, const std::vector<std::uint8_t>& payload) {
        tag(number, 2).varint(payload.size());
        b.insert(b.end(), payload.begin(), payload.end());
        return *this;
    }
    /// String field.
    Pb& str(std::uint32_t number, const std::string& s) {
        return bytes(number, std::vector<std::uint8_t>(s.begin(), s.end()));
    }
    /// Embedded message field.
    Pb& msg(std::uint32_t number, const Pb& inner) { return bytes(number, inner.b); }
    /// Deprecated group: start tag, inner fields, end tag.
    Pb& group(std::uint32_t number, const Pb& inner) {
        tag(number, 3);
        b.insert(b.end(), inner.b.begin(), inner.b.end());
        return tag(number, 4);
    }
    /// Packed floats (one length-delimited element).
    Pb& packedFloats(std::uint32_t number, const std::vector<float>& values) {
        std::vector<std::uint8_t> payload;
        for (const float f : values) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            for (int i = 0; i < 4; ++i) {
                payload.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
            }
        }
        return bytes(number, payload);
    }
    /// Packed varints (one length-delimited element).
    Pb& packedVarints(std::uint32_t number, const std::vector<std::uint64_t>& values) {
        Pb inner;
        for (const std::uint64_t v : values) {
            inner.varint(v);
        }
        return bytes(number, inner.b);
    }
    /// Append raw bytes verbatim (for deliberately malformed input).
    Pb& raw(std::initializer_list<std::uint8_t> bytesIn) {
        b.insert(b.end(), bytesIn.begin(), bytesIn.end());
        return *this;
    }

    [[nodiscard]] ByteSpan span() const { return ByteSpan(b); }
};

/// Scan every field of `span` into a vector (helper for the tests).
std::vector<ProtoField> scanAll(ByteSpan span, bool* failed = nullptr) {
    ProtoScanner scanner(span);
    std::vector<ProtoField> out;
    ProtoField f;
    while (scanner.next(f)) {
        out.push_back(f);
    }
    if (failed) {
        *failed = scanner.failed();
    }
    return out;
}

/// int64 -> the 64-bit two's complement varint value protobuf uses.
std::uint64_t asVarint(std::int64_t v) { return static_cast<std::uint64_t>(v); }

}  // namespace

// -----------------------------------------------------------------------------
//  ProtoScanner
// -----------------------------------------------------------------------------

TEST_CASE("ProtoScanner decodes every wire type in order", "[meta][proto]") {
    Pb m;
    m.vint(1, 300);
    m.f64(2, 2.5);
    m.str(3, "abc");
    m.group(4, Pb().vint(1, 99).str(2, "in group"));
    m.f32(5, 1.5f);
    m.vint(6, 7);

    bool failed = true;
    const std::vector<ProtoField> fields = scanAll(m.span(), &failed);
    REQUIRE_FALSE(failed);
    // The group (field 4) is consumed transparently and never returned.
    REQUIRE(fields.size() == 5);

    REQUIRE(fields[0].number == 1);
    REQUIRE(fields[0].wire == WireType::Varint);
    REQUIRE(fields[0].varint == 300);
    REQUIRE(fields[0].asU32() == 300u);

    REQUIRE(fields[1].number == 2);
    REQUIRE(fields[1].wire == WireType::Fixed64);
    REQUIRE(fields[1].asDouble() == 2.5);

    REQUIRE(fields[2].number == 3);
    REQUIRE(fields[2].wire == WireType::LengthDelimited);
    REQUIRE(fields[2].asString() == "abc");
    REQUIRE(fields[2].bytes.size() == 3);

    REQUIRE(fields[3].number == 5);
    REQUIRE(fields[3].wire == WireType::Fixed32);
    REQUIRE(fields[3].asFloat() == 1.5f);

    REQUIRE(fields[4].number == 6);
    REQUIRE(fields[4].varint == 7);

    // A second scanner over the same bytes reports atEnd() once drained.
    ProtoScanner scanner(m.span());
    ProtoField f;
    while (scanner.next(f)) {
    }
    REQUIRE(scanner.atEnd());
    REQUIRE(scanner.pos() == scanner.size());
}

TEST_CASE("ProtoField typed accessors follow protobuf conventions", "[meta][proto]") {
    // int32 -1 is transmitted as the 64-bit two's complement varint.
    Pb m;
    m.vint(1, asVarint(-1));
    m.vint(2, 1);  // zigzag 1 == -1
    m.vint(3, 0);
    m.fixed32(4, 0xFFFFFFFFu);
    m.f32(5, -3.25f);
    const std::vector<ProtoField> fields = scanAll(m.span());
    REQUIRE(fields.size() == 5);
    REQUIRE(fields[0].asI32() == -1);
    REQUIRE(fields[0].asI64() == -1);
    REQUIRE(fields[0].asU32() == 0xFFFFFFFFu);
    REQUIRE(fields[0].asBool());
    REQUIRE(fields[1].asZigzag() == -1);
    REQUIRE_FALSE(fields[2].asBool());
    // sfixed32 reading of all-ones is -1; unsigned reading is the raw bits.
    REQUIRE(fields[3].asI32() == -1);
    REQUIRE(fields[3].asU32() == 0xFFFFFFFFu);
    REQUIRE(fields[3].asI64() == -1);
    REQUIRE(fields[4].asFloat() == -3.25f);
    REQUIRE(fields[4].asDouble() == -3.25);
    // asString on a non length-delimited field is empty, never garbage.
    REQUIRE(fields[4].asString().empty());
    REQUIRE(wireTypeName(WireType::Fixed32) == std::string("fixed32"));
}

TEST_CASE("ProtoScanner rejects a field number of zero", "[meta][proto]") {
    // Tag byte 0x00 -> number 0, wire 0.
    Pb m;
    m.raw({0x00, 0x01});
    bool failed = false;
    const std::vector<ProtoField> fields = scanAll(m.span(), &failed);
    REQUIRE(fields.empty());
    REQUIRE(failed);

    // The largest legal number is accepted, one more is rejected.
    Pb ok;
    ok.vint(kMaxProtoFieldNumber, 1);
    REQUIRE(scanAll(ok.span(), &failed).size() == 1);
    REQUIRE_FALSE(failed);

    Pb tooBig;
    tooBig.varint((static_cast<std::uint64_t>(kMaxProtoFieldNumber) + 1) << 3).varint(1);
    REQUIRE(scanAll(tooBig.span(), &failed).empty());
    REQUIRE(failed);
}

TEST_CASE("ProtoScanner rejects reserved wire types 6 and 7", "[meta][proto]") {
    for (const std::uint8_t wire : {std::uint8_t{6}, std::uint8_t{7}}) {
        Pb m;
        m.raw({static_cast<std::uint8_t>((1u << 3) | wire), 0x00, 0x00});
        bool failed = false;
        REQUIRE(scanAll(m.span(), &failed).empty());
        REQUIRE(failed);
    }
}

TEST_CASE("ProtoScanner rejects varints longer than ten bytes", "[meta][proto]") {
    // Eleven continuation bytes as a tag.
    Pb tag;
    for (int i = 0; i < 11; ++i) {
        tag.raw({0x80});
    }
    tag.raw({0x01});
    bool failed = false;
    REQUIRE(scanAll(tag.span(), &failed).empty());
    REQUIRE(failed);

    // Same as a varint payload after a valid tag.
    Pb payload;
    payload.tag(1, 0);
    for (int i = 0; i < 11; ++i) {
        payload.raw({0x80});
    }
    payload.raw({0x01});
    REQUIRE(scanAll(payload.span(), &failed).empty());
    REQUIRE(failed);

    // A ten byte varint is the legal maximum and decodes.
    Pb legal;
    legal.vint(1, UINT64_MAX);
    const std::vector<ProtoField> fields = scanAll(legal.span(), &failed);
    REQUIRE_FALSE(failed);
    REQUIRE(fields.size() == 1);
    REQUIRE(fields[0].varint == UINT64_MAX);
}

TEST_CASE("ProtoScanner stops on a truncated length-delimited field but keeps earlier fields", "[meta][proto]") {
    Pb m;
    m.vint(1, 1);
    m.tag(2, 2).varint(10).raw({0xAA, 0xBB, 0xCC});  // claims 10 bytes, has 3
    ProtoScanner scanner(m.span());
    ProtoField f;
    REQUIRE(scanner.next(f));
    REQUIRE(f.number == 1);
    REQUIRE_FALSE(scanner.next(f));
    REQUIRE(scanner.failed());
    REQUIRE_FALSE(scanner.failure().empty());
    REQUIRE_FALSE(scanner.atEnd());
    // Once failed the scanner stays failed.
    REQUIRE_FALSE(scanner.next(f));

    // Truncated fixed payloads fail the same way.
    Pb fx;
    fx.vint(1, 5).tag(2, 5).raw({0x01, 0x02});
    bool failed = false;
    REQUIRE(scanAll(fx.span(), &failed).size() == 1);
    REQUIRE(failed);
    Pb fx64;
    fx64.tag(2, 1).raw({0x01, 0x02, 0x03});
    REQUIRE(scanAll(fx64.span(), &failed).empty());
    REQUIRE(failed);
}

TEST_CASE("ProtoScanner requires balanced groups", "[meta][proto]") {
    // End-group without a start.
    Pb end;
    end.tag(4, 4);
    bool failed = false;
    REQUIRE(scanAll(end.span(), &failed).empty());
    REQUIRE(failed);

    // Start-group never closed: the fields inside are consumed, then failure.
    Pb open;
    open.vint(1, 1).tag(4, 3).vint(2, 2);
    const std::vector<ProtoField> fields = scanAll(open.span(), &failed);
    REQUIRE(fields.size() == 1);
    REQUIRE(failed);

    // Nested groups are skipped as a unit.
    Pb nested;
    nested.group(4, Pb().vint(1, 1).group(5, Pb().vint(2, 2))).vint(9, 9);
    const std::vector<ProtoField> after = scanAll(nested.span(), &failed);
    REQUIRE_FALSE(failed);
    REQUIRE(after.size() == 1);
    REQUIRE(after[0].number == 9);
}

TEST_CASE("Empty and single-byte inputs are handled", "[meta][proto]") {
    bool failed = false;
    REQUIRE(scanAll(ByteSpan{}, &failed).empty());
    REQUIRE_FALSE(failed);
    ProtoScanner empty(ByteSpan{});
    REQUIRE(empty.atEnd());

    // A lone tag with no payload is truncated.
    Pb lone;
    lone.tag(1, 0);
    REQUIRE(scanAll(lone.span(), &failed).empty());
    REQUIRE(failed);
}

// -----------------------------------------------------------------------------
//  Repeated scalar helpers
// -----------------------------------------------------------------------------

TEST_CASE("Packed and unpacked repeated scalars decode identically", "[meta][proto]") {
    const std::vector<float> floats = {1.5f, -2.25f, 3.0f, 1e-7f};
    const std::vector<std::int32_t> i32s = {-1, 2, 2147483647, -2147483647 - 1};
    const std::vector<std::uint32_t> u32s = {0u, 1u, 4294967295u};
    const std::vector<std::int64_t> i64s = {-5, 0, 1LL << 40, -(1LL << 40)};
    const std::vector<std::uint64_t> u64s = {1ull << 40, UINT64_MAX, 7ull};

    // ---- packed: one element per field ----
    Pb packed;
    packed.packedFloats(1, floats);
    {
        std::vector<std::uint64_t> raw;
        for (const std::int32_t v : i32s) {
            raw.push_back(asVarint(v));
        }
        packed.packedVarints(2, raw);
    }
    packed.packedVarints(3, {0u, 1u, 4294967295u});
    {
        std::vector<std::uint64_t> raw;
        for (const std::int64_t v : i64s) {
            raw.push_back(asVarint(v));
        }
        packed.packedVarints(4, raw);
    }
    packed.packedVarints(5, u64s);

    // ---- unpacked: one element per value, interleaved with other fields ----
    Pb unpacked;
    for (std::size_t i = 0; i < floats.size(); ++i) {
        unpacked.f32(1, floats[i]);
        unpacked.vint(9, i);  // an unrelated field between the values
    }
    for (const std::int32_t v : i32s) {
        unpacked.vint(2, asVarint(v));
    }
    for (const std::uint32_t v : u32s) {
        unpacked.vint(3, v);
    }
    for (const std::int64_t v : i64s) {
        unpacked.vint(4, asVarint(v));
    }
    for (const std::uint64_t v : u64s) {
        unpacked.vint(5, v);
    }

    for (const Pb* m : {&packed, &unpacked}) {
        std::vector<float> outF;
        std::vector<std::int32_t> outI32;
        std::vector<std::uint32_t> outU32;
        std::vector<std::int64_t> outI64;
        std::vector<std::uint64_t> outU64;
        bool failed = false;
        for (const ProtoField& f : scanAll(m->span(), &failed)) {
            switch (f.number) {
            case 1: REQUIRE(appendRepeatedFloat(f, outF)); break;
            case 2: REQUIRE(appendRepeatedI32(f, outI32)); break;
            case 3: REQUIRE(appendRepeatedU32(f, outU32)); break;
            case 4: REQUIRE(appendRepeatedI64(f, outI64)); break;
            case 5: REQUIRE(appendRepeatedU64(f, outU64)); break;
            default: break;
            }
        }
        REQUIRE_FALSE(failed);
        REQUIRE(outF == floats);
        REQUIRE(outI32 == i32s);
        REQUIRE(outU32 == u32s);
        REQUIRE(outI64 == i64s);
        REQUIRE(outU64 == u64s);
    }
}

TEST_CASE("Repeated helpers reject payloads of the wrong shape", "[meta][proto]") {
    // A packed float run must be a whole number of 4-byte values.
    Pb bad;
    bad.bytes(1, {0x00, 0x00, 0x00, 0x00, 0x00});
    bad.f64(2, 1.0);
    bad.tag(3, 2).varint(2).raw({0x80, 0x80});  // packed varints, last one truncated
    const std::vector<ProtoField> fields = scanAll(bad.span());
    REQUIRE(fields.size() == 3);
    std::vector<float> floats;
    REQUIRE_FALSE(appendRepeatedFloat(fields[0], floats));
    REQUIRE(floats.empty());
    REQUIRE_FALSE(appendRepeatedFloat(fields[1], floats));  // fixed64 is not a float
    std::vector<double> doubles;
    REQUIRE(appendRepeatedDouble(fields[1], doubles));
    REQUIRE(doubles.size() == 1);
    std::vector<std::uint32_t> u32s;
    REQUIRE_FALSE(appendRepeatedU32(fields[2], u32s));
}

// -----------------------------------------------------------------------------
//  ProtoTree
// -----------------------------------------------------------------------------

TEST_CASE("decodeTree retains unknown fields and nests messages", "[meta][proto]") {
    Pb m;
    m.vint(1, 42);
    m.str(99, "hello");
    m.msg(7, Pb().vint(1, 5).f32(2, 0.5f));
    m.bytes(8, {0x01, 0x02, 0xFF});  // not a message: tag 0x01 has number 0

    const ProtoTree tree = decodeTree(m.span());
    REQUIRE_FALSE(tree.failed);
    REQUIRE(tree.fields.size() == 4);
    REQUIRE(tree.nodeCount == 6);

    REQUIRE(tree.fields[0].number == 1);
    REQUIRE(tree.fields[0].varint == 42);

    // Unknown field 99 is kept verbatim and recognised as a string.
    const ProtoNode* unknown = findChild(tree.fields, 99);
    REQUIRE(unknown != nullptr);
    REQUIRE_FALSE(unknown->isMessage);
    REQUIRE(unknown->isPrintableAscii());
    REQUIRE(std::string(unknown->bytes.begin(), unknown->bytes.end()) == "hello");

    const ProtoNode* nested = findChild(tree.fields, 7);
    REQUIRE(nested != nullptr);
    REQUIRE(nested->isMessage);
    REQUIRE(nested->children.size() == 2);
    REQUIRE(nested->children[0].varint == 5);
    REQUIRE(nested->children[1].wire == WireType::Fixed32);
    // The raw bytes stay available even for nested nodes.
    REQUIRE_FALSE(nested->bytes.empty());

    const ProtoNode* blob = findChild(tree.fields, 8);
    REQUIRE(blob != nullptr);
    REQUIRE_FALSE(blob->isMessage);
    REQUIRE(blob->bytes.size() == 3);
    REQUIRE(findChild(tree.fields, 1234) == nullptr);
}

TEST_CASE("looksLikeMessage heuristic", "[meta][proto]") {
    REQUIRE_FALSE(looksLikeMessage(ByteSpan{}));
    const std::string video = "video";
    REQUIRE_FALSE(looksLikeMessage(ByteSpan(reinterpret_cast<const std::uint8_t*>(video.data()), video.size())));
    Pb ok;
    ok.vint(1, 1);
    REQUIRE(looksLikeMessage(ok.span()));
    Pb truncated;
    truncated.tag(1, 0);
    REQUIRE_FALSE(looksLikeMessage(truncated.span()));
    // A group makes the heuristic say no even though the scanner accepts it.
    Pb grouped;
    grouped.group(1, Pb().vint(1, 1));
    REQUIRE_FALSE(looksLikeMessage(grouped.span()));
}

TEST_CASE("decodeTree honours the nesting limit", "[meta][proto]") {
    // 20 wrapper messages around {1: 1}.
    Pb inner;
    inner.vint(1, 1);
    Pb current = inner;
    for (int i = 0; i < 20; ++i) {
        Pb wrapper;
        wrapper.msg(2, current);
        current = wrapper;
    }
    auto chainLength = [](const ProtoTree& tree) {
        REQUIRE(tree.fields.size() == 1);
        const ProtoNode* node = &tree.fields.front();
        int levels = 0;
        while (node->isMessage) {
            REQUIRE(node->children.size() == 1);
            ++levels;
            node = &node->children.front();
        }
        return levels;
    };
    // Default limit (12): nodes at depth 12 stay opaque bytes.
    REQUIRE(chainLength(decodeTree(current.span())) == 12);
    // A generous limit expands all 20 wrappers; the innermost field is a varint.
    REQUIRE(chainLength(decodeTree(current.span(), 0, 64)) == 20);
    // A zero limit expands nothing.
    REQUIRE(chainLength(decodeTree(current.span(), 0, 0)) == 0);
    // Starting at depth 11 leaves one level.
    REQUIRE(chainLength(decodeTree(current.span(), 11, 12)) == 1);

    // The node budget caps the total.
    const ProtoTree capped = decodeTree(current.span(), 0, 64, 5);
    REQUIRE(capped.nodeCount == 5);
}

TEST_CASE("decodeTree reports a malformed nested payload as truncated", "[meta][proto]") {
    // Field 3 holds bytes that begin like a message but end in a truncated
    // varint; the heuristic keeps it opaque and the top level stays clean.
    Pb m;
    m.vint(1, 1).bytes(3, {0x08, 0x80});
    ProtoTree tree = decodeTree(m.span());
    REQUIRE_FALSE(tree.failed);
    REQUIRE(tree.fields.size() == 2);
    REQUIRE_FALSE(tree.fields[1].isMessage);

    // A top-level truncation is reported with a reason.
    Pb bad;
    bad.vint(1, 1).tag(2, 2).varint(50);
    tree = decodeTree(bad.span());
    REQUIRE(tree.failed);
    REQUIRE_FALSE(tree.failure.empty());
    REQUIRE(tree.fields.size() == 1);
}

// -----------------------------------------------------------------------------
//  DjmdDecoder on synthetic ProductMeta messages
// -----------------------------------------------------------------------------

namespace {

/// A DewarpParams record with the core fields; `widthAsFloat` picks the
/// encoding of fields 10/11 (the camera writes fixed32 floats).
Pb makeDewarp(float fx, float fy, float cx, float cy, bool widthAsFloat, const Quaternion& q) {
    Pb d;
    d.f32(1, fx).f32(2, fy).f32(3, cx).f32(4, cy);
    d.f32(5, 0.0667397f).f32(6, -0.0128859f).f32(7, 0.0103815f).f32(8, -0.00677581f);
    d.f32(9, 0.0f);
    if (widthAsFloat) {
        d.f32(10, 3840.0f).f32(11, 3840.0f);
    } else {
        d.vint(10, 3840).vint(11, 3840);
    }
    d.f32(12, -179.6f).f32(13, 90.2f).f32(14, 0.06f);
    d.f32(15, 0.00098791f);
    d.packedFloats(20, {-2.25e-4f, -1.34e-4f});
    d.packedFloats(21, {q.w, q.x, q.y, q.z});
    d.packedFloats(22, {1920.0f, 317.85f, 518.14f});
    d.packedFloats(23, {3735.0f, 2845.0f, 3096.3f});
    d.f32(24, 8.0f).f32(25, -1000.0f);
    d.packedFloats(27, {-2.25e-4f, -1.34e-4f});
    d.msg(28, Pb().f32(1, q.w).f32(2, q.x).f32(3, q.y).f32(4, q.z));
    return d;
}

/// An all-zero 159-byte style empty slot (zeros in the repeated fields only).
Pb makeEmptyDewarp() {
    Pb d;
    d.packedFloats(20, {0.0f, 0.0f});
    d.packedFloats(21, {0.0f, 0.0f, 0.0f, 0.0f});
    d.packedFloats(22, std::vector<float>(14, 0.0f));
    d.packedFloats(23, std::vector<float>(14, 0.0f));
    d.packedFloats(27, {0.0f, 0.0f});
    d.msg(28, Pb().f32(1, 0.0f).f32(2, 0.0f).f32(3, 0.0f).f32(4, 0.0f));
    return d;
}

/// A complete synthetic sample 0: ClipMeta + StreamMeta + FrameMeta.
Pb makeProductMeta() {
    const Quaternion qs{0.0026615f, 0.0019091f, -0.7056412f, 0.7085618f, true};
    const Quaternion qm{0.7036960f, 0.7103991f, -0.0046943f, -0.0110939f, true};

    Pb header;
    header.str(1, "dvtm_test.proto").str(2, "02.01.15").str(3, "2.0.8").str(5, "SN123").str(6, "10.00.25.29");
    header.vint(7, 1).vint(8, 2).vint(9, 30669420766ull).str(10, "Osmo 360");

    Pb clip;
    clip.msg(1, header);
    clip.msg(2, Pb().vint(1, 1).vint(2, 1));
    clip.msg(3, Pb().packedFloats(1, {0.1551311f, 0.1371409f, -0.0938614f, 0.0041704f}));
    clip.msg(4, Pb().vint(1, 17282160));
    clip.msg(5, Pb().vint(1, 4));
    clip.msg(8, Pb().f32(1, 829.3612f));
    clip.msg(9, Pb().vint(1, 0));
    clip.msg(10, Pb().vint(1, 1000));
    clip.msg(11, Pb().f32(1, 59.939388f));
    clip.msg(12, Pb().vint(1, asVarint(-136)));
    clip.msg(13, Pb().vint(1, 3429));
    clip.msg(14, Pb().vint(1, 3840).vint(2, 3840));
    clip.msg(15, Pb().packedVarints(1, {19, 10}));
    clip.msg(16, Pb());
    clip.msg(17, Pb());
    clip.vint(99, 5);  // unknown field: dropped without complaint

    Pb pano;
    pano.msg(1, makeDewarp(1043.8802f, 1043.6731f, 1917.0421f, 1919.1294f, true, qs));
    pano.msg(2, makeDewarp(1043.0103f, 1042.9268f, 1908.8036f, 1918.7257f, false, qm));
    pano.msg(3, makeEmptyDewarp());
    pano.msg(4, makeEmptyDewarp());
    pano.msg(15, makeDewarp(1041.78f, 1041.78f, 1917.0f, 1919.0f, true, qs));
    pano.msg(16, makeDewarp(1040.97f, 1040.97f, 1908.0f, 1918.0f, true, qm));

    Pb stream;
    stream.msg(1, Pb().vint(1, 0).vint(2, 0).str(3, "video"));
    stream.msg(3, Pb().vint(1, 3000).vint(2, 3000).f32(3, 59.94f).vint(4, 1).vint(5, 10).vint(6, 4).vint(7, 0).vint(8, 1));
    stream.msg(4, Pb().vint(1, 19));
    stream.msg(5, Pb().vint(1, 3));
    stream.msg(6, pano);
    stream.msg(7, Pb().vint(1, 0));
    stream.msg(8, Pb().vint(1, 7));

    const Quaternion att{0.46131432f, 0.54081959f, 0.54205739f, 0.44819313f, true};
    Pb batch;
    batch.vint(1, 604649192).vint(2, 238617);
    for (int i = 0; i < 17; ++i) {
        const float w = att.w + 0.001f * static_cast<float>(i - 4);
        batch.msg(3, Pb().f32(1, w).f32(2, att.x).f32(3, att.y).f32(4, att.z));
    }
    batch.f32(4, -0.9025f);

    Pb camera;
    camera.msg(1, Pb().vint(1, 1).str(4, "camera"));
    camera.msg(2, Pb().f32(1, 0.0f));
    camera.msg(3, Pb().f32(1, 142.0f));
    camera.msg(4, Pb().packedVarints(1, {1, 208}));
    camera.msg(5, Pb().f32(1, 1.0f));
    camera.msg(6, Pb().vint(1, 6545));
    camera.msg(7, Pb().vint(1, 0));
    camera.msg(8, Pb().vint(1, 0));
    camera.msg(9, Pb().f32(1, att.w).f32(2, att.x).f32(3, att.y).f32(4, att.z));
    camera.msg(10, Pb().f32(2, 0.3f).f32(3, -1.5f).f32(4, 0.25f));
    camera.msg(15, Pb().f32(1, 9.88f).f32(2, 201.98f).f32(6, 1.49f));
    camera.msg(16, Pb().f32(1, 22.0f));
    camera.msg(17, Pb().vint(1, 100).vint(1, 1000));  // unpacked repeated int32

    Pb imu;
    imu.msg(1, Pb().vint(1, 2));
    imu.msg(2, Pb().msg(1, batch));
    imu.msg(3, Pb().vint(1, 5));

    Pb frame;
    frame.msg(1, Pb().vint(1, 0).vint(2, 30669420766ull).vint(3, 0));
    frame.msg(2, camera);
    frame.msg(3, imu);
    frame.msg(4, Pb().msg(1, Pb().vint(1, 3).str(4, "Osmo OQ001").f32(5, 59.94f)));

    Pb product;
    product.msg(1, clip).msg(2, stream).msg(3, frame);
    return product;
}

}  // namespace

TEST_CASE("DjmdDecoder decodes a synthetic ProductMeta", "[meta][proto]") {
    const Pb sample = makeProductMeta();
    const Result<ProductMeta> decoded = DjmdDecoder::decode(sample.span());
    REQUIRE(decoded.ok());
    const ProductMeta& p = decoded.value();
    REQUIRE(p.warnings.empty());

    // ---- ClipMeta ----
    REQUIRE(p.clip.has_value());
    const ClipMeta& c = *p.clip;
    REQUIRE(c.header.protoFileName == "dvtm_test.proto");
    REQUIRE(c.header.libVersion == "02.01.15");
    REQUIRE(c.header.serialNumber == "SN123");
    REQUIRE(c.header.firmware == "10.00.25.29");
    REQUIRE(c.header.clipTimestampUs == 30669420766ull);
    REQUIRE(c.header.productName == "Osmo 360");
    REQUIRE(c.videoStreamCount == 1);
    REQUIRE(c.audioStreamCount == 1);
    REQUIRE(c.distortionCoefficients.size() == 4);
    REQUIRE_THAT(c.distortionCoefficients[0], WithinRel(0.1551311f, 1e-6f));
    REQUIRE(c.sensorReadoutTime == 17282160);
    REQUIRE(c.sensorReadDirection == 4);
    REQUIRE_THAT(c.digitalFocalLength, WithinRel(829.3612f, 1e-6f));
    REQUIRE(c.eisStatus == EisStatus::Off);
    REQUIRE(c.imuSamplingRate == 1000);
    REQUIRE(c.itd == -136);
    REQUIRE(c.lro == 3429);
    REQUIRE(c.sensorW == 3840);
    REQUIRE(c.sensorH == 3840);
    REQUIRE(c.fNumber == std::vector<std::uint32_t>{19, 10});
    for (const std::size_t bit : {1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}) {
        INFO("present bit " << bit);
        REQUIRE(c.present.test(bit));
    }
    REQUIRE_FALSE(c.present.test(6));
    REQUIRE_FALSE(c.present.test(7));

    // ---- StreamMeta ----
    REQUIRE(p.stream.has_value());
    const StreamMeta& s = *p.stream;
    REQUIRE(s.id == 0);
    REQUIRE(s.name == "video");
    REQUIRE(s.video.width == 3000);
    REQUIRE(s.video.height == 3000);
    REQUIRE(s.video.bitDepthValid);
    REQUIRE(s.video.bitDepth == 10);
    REQUIRE(s.video.bitFormat == 4);
    REQUIRE(s.video.codec == 1);
    REQUIRE(s.colorMode == ColorMode::DLogM);
    REQUIRE(s.fovType == 3);
    REQUIRE(s.extriLensMode == ExtriLensMode::Native);
    REQUIRE(s.shadingCalibModeNum == 7);
    REQUIRE(s.present.test(6));

    // Width/height decode to 3840 whether written as float or varint.
    const DewarpParams* slave = s.dewarp.get(PanoDewarpParams::NativeRefineSlave);
    const DewarpParams* master = s.dewarp.get(PanoDewarpParams::NativeRefineMaster);
    REQUIRE(slave != nullptr);
    REQUIRE(master != nullptr);
    REQUIRE(slave->width == 3840);
    REQUIRE(slave->height == 3840);
    REQUIRE(master->width == 3840);
    REQUIRE(master->height == 3840);
    REQUIRE_THAT(slave->fx, WithinRel(1043.8802f, 1e-6f));
    REQUIRE_THAT(slave->k[4], WithinRel(0.00098791f, 1e-6f));
    REQUIRE(slave->k[5] == 0.0f);
    REQUIRE(slave->p.size() == 2);
    REQUIRE(slave->q.size() == 4);
    REQUIRE(slave->occlusionPtX.size() == 3);
    REQUIRE(slave->camExtriQ.present);
    REQUIRE_THAT(slave->camExtriQ.y, WithinRel(-0.7056412f, 1e-6f));
    REQUIRE_FALSE(slave->camImuExtriQ.present);
    REQUIRE(slave->lensModel == 8.0f);
    REQUIRE(slave->temperature == -1000.0f);
    REQUIRE(slave->hasCore());
    REQUIRE_FALSE(slave->isEmpty());
    REQUIRE(slave->present.test(10));
    REQUIRE(slave->present.test(28));
    REQUIRE_FALSE(slave->present.test(26));

    // Empty slots are stored but hidden by get().
    REQUIRE(s.dewarp.byField[3].has_value());
    REQUIRE(s.dewarp.byField[3]->isEmpty());
    REQUIRE_FALSE(s.dewarp.byField[3]->hasCore());
    REQUIRE(s.dewarp.get(3) == nullptr);
    REQUIRE(s.dewarp.get(0) == nullptr);
    REQUIRE(s.dewarp.get(25) == nullptr);
    REQUIRE(s.dewarp.get(11) == nullptr);
    REQUIRE(std::string(PanoDewarpParams::fieldName(15)) == "far_09_slave");
    REQUIRE(std::string(PanoDewarpParams::fieldName(0)) == "unknown");
    REQUIRE(std::string(PanoDewarpParams::fieldName(99)) == "unknown");

    // ---- FrameMeta ----
    REQUIRE(p.frame.has_value());
    const FrameMeta& f = *p.frame;
    REQUIRE(f.seq == 0);
    REQUIRE(f.timestampUs == 30669420766ull);
    REQUIRE(f.streamId == 0);
    REQUIRE(f.camera.iso == 142.0f);
    REQUIRE(f.camera.exposureTime == std::vector<std::int32_t>{1, 208});
    REQUIRE(f.camera.wbCct == 6545);
    REQUIRE(f.camera.attitude.present);
    REQUIRE_THAT(f.camera.attitude.w, WithinRel(0.46131432f, 1e-6f));
    REQUIRE(f.camera.accPresent);
    REQUIRE(f.camera.acc.y == -1.5f);
    REQUIRE_THAT(f.camera.aecLuxIdx, WithinRel(201.98f, 1e-6f));
    REQUIRE(f.camera.sensorTemperature == 22.0f);
    REQUIRE(f.camera.eqFocal == std::vector<std::int32_t>{100, 1000});
    REQUIRE(f.imu.has_value());
    REQUIRE(f.imu->current.present);
    REQUIRE(f.imu->current.q.size() == 17);
    REQUIRE(f.imu->current.ts == 604649192);
    REQUIRE(f.imu->current.vsync == 238617);
    REQUIRE_THAT(f.imu->current.offset, WithinRel(-0.9025f, 1e-6f));
    REQUIRE_FALSE(f.imu->prev.present);
    REQUIRE_FALSE(f.imu->next.present);
    REQUIRE(f.imu->vsyncPos == 5);
    // The anchor sample equals the camera attitude by construction.
    REQUIRE(f.imu->current.q[4].w == f.camera.attitude.w);
    REQUIRE(f.gimbalDeviceName == "Osmo OQ001");
    REQUIRE_THAT(f.gimbalDeviceFrequency, WithinRel(59.94f, 1e-6f));

    // ---- CalibrationSelector on the synthetic stream ----
    std::vector<std::string> warnings;
    const Result<CalibrationSet> native = CalibrationSelector::select(s, {}, &warnings);
    REQUIRE(native.ok());
    REQUIRE(native.value().sourceSlave == "native_refine_slave");
    REQUIRE(native.value().sourceMaster == "native_refine_master");
    REQUIRE(native.value().slave.fx == slave->fx);
    REQUIRE(warnings.empty());

    // Only the 0.9 m preset exists: every distance maps to it with a note,
    // except an exact 0.9 which is silent.
    CalibrationSelector::Options far;
    far.stitchDistanceM = 1.6;
    const Result<CalibrationSet> preset = CalibrationSelector::select(s, far, &warnings);
    REQUIRE(preset.ok());
    REQUIRE(preset.value().sourceSlave == "far_09_slave");
    REQUIRE(warnings.size() == 1);
    warnings.clear();
    far.stitchDistanceM = 0.9;
    REQUIRE(CalibrationSelector::select(s, far, &warnings).ok());
    REQUIRE(warnings.empty());
    far.stitchDistanceM = -1.0;
    REQUIRE(CalibrationSelector::select(s, far, &warnings).code() == ErrorCode::InvalidArgument);

    // The refined pair is the only native pair: !preferRefined still lands on it.
    CalibrationSelector::Options rawFirst;
    rawFirst.preferRefined = false;
    warnings.clear();
    const Result<CalibrationSet> raw = CalibrationSelector::select(s, rawFirst, &warnings);
    REQUIRE(raw.ok());
    REQUIRE(raw.value().sourceSlave == "native_refine_slave");
    REQUIRE(warnings.size() == 1);

    // Lens guards are absent: fallback to native with a warning.
    CalibrationSelector::Options guards;
    guards.lensModeOverride = ExtriLensMode::LensGuards;
    warnings.clear();
    REQUIRE(CalibrationSelector::select(s, guards, &warnings).ok());
    REQUIRE(warnings.size() == 2);  // override note + fallback note
}

TEST_CASE("DjmdDecoder records malformed sub-messages and keeps going", "[meta][proto]") {
    // ClipMeta whose header payload is garbage but whose focal length is fine.
    Pb clip;
    clip.bytes(1, {0x00, 0xFF});             // tag 0 -> malformed header
    clip.msg(8, Pb().f32(1, 829.3612f));
    clip.vint(2, 7);                         // known field with the wrong wire type
    Pb product;
    product.msg(1, clip);

    const Result<ProductMeta> decoded = DjmdDecoder::decode(product.span());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().clip.has_value());
    REQUIRE_THAT(decoded.value().clip->digitalFocalLength, WithinRel(829.3612f, 1e-6f));
    REQUIRE(decoded.value().clip->present.test(8));
    REQUIRE_FALSE(decoded.value().clip->present.test(2));
    REQUIRE(decoded.value().warnings.size() == 2);
    REQUIRE(decoded.value().warnings[0].find("ClipMeta.header") != std::string::npos);
    REQUIRE(decoded.value().warnings[1].find("wire type") != std::string::npos);
    REQUIRE_FALSE(decoded.value().stream.has_value());
    REQUIRE_FALSE(decoded.value().frame.has_value());
}

TEST_CASE("DjmdDecoder rejects empty and useless samples", "[meta][proto]") {
    REQUIRE(DjmdDecoder::decode(ByteSpan{}).code() == ErrorCode::InvalidArgument);

    // Only unknown fields: nothing usable.
    Pb unknown;
    unknown.vint(9, 1).str(10, "x");
    REQUIRE(DjmdDecoder::decode(unknown.span()).code() == ErrorCode::Malformed);

    // Garbage bytes.
    Pb garbage;
    garbage.raw({0x00, 0x00, 0x00});
    REQUIRE(DjmdDecoder::decode(garbage.span()).code() == ErrorCode::Malformed);

    // A FrameMeta alone is enough.
    Pb frameOnly;
    frameOnly.msg(3, Pb().msg(1, Pb().vint(1, 4).vint(2, 123).vint(3, 0)));
    const Result<ProductMeta> ok = DjmdDecoder::decode(frameOnly.span());
    REQUIRE(ok.ok());
    REQUIRE(ok.value().frame.has_value());
    REQUIRE(ok.value().frame->seq == 4);
    REQUIRE(ok.value().frame->timestampUs == 123);
    REQUIRE_FALSE(ok.value().frame->imu.has_value());
    REQUIRE_FALSE(ok.value().frame->camera.attitude.present);
}

TEST_CASE("Quaternion decoding zero-fills missing components", "[meta][proto]") {
    Pb partial;
    partial.f32(1, 0.5f).f32(4, 0.25f);
    const Quaternion q = DjmdDecoder::decodeQuaternion(partial.span());
    REQUIRE(q.present);
    REQUIRE(q.w == 0.5f);
    REQUIRE(q.x == 0.0f);
    REQUIRE(q.y == 0.0f);
    REQUIRE(q.z == 0.25f);
    // The two Quatd conversions expose both component orders.
    REQUIRE(q.toQuatdWXYZ().w == 0.5);
    REQUIRE(q.toQuatdXYZW().x == 0.5);
    REQUIRE(q.toQuatdXYZW().w == 0.25);
}

TEST_CASE("The Avata 360 colour mode is read from StreamMeta 2.4 and not from 4", "[meta][proto]") {
    // Layout of the three Avata 360 clips this was checked on (two D-Log M,
    // one Normal): StreamMeta field 2 is a camera sub-message whose field 4
    // wraps the colour mode, and StreamMeta field 4 - the Osmo 360's colour
    // mode - is an empty message.  Read the Osmo way, every Avata clip is
    // Normal.  `colorWrapper` is StreamMeta 2.4's payload, or null to leave
    // field 2.4 out.  `streamFirst` puts StreamMeta ahead of ClipMeta.
    const auto avataSample = [](const char* proto, const Pb* colorWrapper, bool streamFirst = false) {
        Pb header;
        header.str(1, proto).str(10, "DJI Avata360");
        Pb clip;
        clip.msg(1, header);
        Pb camera;
        camera.msg(1, Pb().vint(2, 1).str(4, "DJI FCA188"));
        camera.msg(3, Pb().str(1, "SN"));
        if (colorWrapper != nullptr) {
            camera.msg(4, *colorWrapper);
        }
        camera.msg(5, Pb());
        Pb stream;
        stream.msg(1, Pb().str(3, "video"));
        stream.msg(2, camera);
        stream.msg(3, Pb().vint(1, 3840).vint(2, 3840).vint(5, 10));
        stream.msg(4, Pb());  // empty on every Avata 360 sample seen
        Pb product;
        if (streamFirst) {
            product.msg(2, stream).msg(1, clip);
        } else {
            product.msg(1, clip).msg(2, stream);
        }
        return product;
    };
    const auto decodeStream = [](const Pb& sample, std::vector<std::string>* warnings = nullptr) {
        const Result<ProductMeta> decoded = DjmdDecoder::decode(sample.span());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().stream.has_value());
        if (warnings != nullptr) {
            *warnings = decoded.value().warnings;
        }
        return *decoded.value().stream;
    };
    const Pb dlogm = Pb().vint(1, 19);
    const Pb empty;

    SECTION("D-Log M") {
        const StreamMeta s = decodeStream(avataSample("dvtm_AVATA360.proto", &dlogm));
        REQUIRE(s.colorMode == ColorMode::DLogM);
        REQUIRE(s.present.test(4));
        // The rest of StreamMeta still decodes the usual way.
        REQUIRE(s.video.width == 3840);
        REQUIRE(s.name == "video");
    }
    SECTION("D-Log M with StreamMeta ahead of ClipMeta") {
        const StreamMeta s = decodeStream(avataSample("dvtm_AVATA360.proto", &dlogm, true));
        REQUIRE(s.colorMode == ColorMode::DLogM);
        REQUIRE(s.present.test(4));
    }
    SECTION("An empty 2.4 is proto3's unwritten 0, Normal") {
        const StreamMeta s = decodeStream(avataSample("dvtm_AVATA360.proto", &empty));
        REQUIRE(s.colorMode == ColorMode::Normal);
        REQUIRE(s.present.test(4));
    }
    SECTION("No 2.4 is not recorded, never Normal") {
        std::vector<std::string> warnings;
        const StreamMeta s = decodeStream(avataSample("dvtm_AVATA360.proto", nullptr), &warnings);
        REQUIRE(s.colorMode == ColorMode::Unknown);
        REQUIRE_FALSE(s.present.test(4));
        REQUIRE_FALSE(warnings.empty());
    }
    SECTION("A mode no Avata 360 sample has shown is left unknown") {
        const Pb hlg = Pb().vint(1, 9);
        std::vector<std::string> warnings;
        const StreamMeta s = decodeStream(avataSample("dvtm_AVATA360.proto", &hlg), &warnings);
        REQUIRE(s.colorMode == ColorMode::Unknown);
        REQUIRE_FALSE(s.present.test(4));
        REQUIRE_FALSE(warnings.empty());
    }
    SECTION("Any other proto keeps the Osmo 360 reading of field 4") {
        // Same bytes, Osmo proto name: 2.4 is ignored and the empty field 4
        // reads as Normal, exactly as before.
        const StreamMeta s = decodeStream(avataSample("dvtm_oq101.proto", &dlogm));
        REQUIRE(s.colorMode == ColorMode::Normal);
        REQUIRE(s.present.test(4));
    }
}

TEST_CASE("Enum names are stable and total", "[meta]") {
    REQUIRE(std::string(colorModeName(ColorMode::DLogM)) == "DLogM");
    REQUIRE(std::string(colorModeName(static_cast<ColorMode>(1234))) == "Unknown");
    REQUIRE(std::string(extriLensModeName(ExtriLensMode::Underwater)) == "Underwater");
    REQUIRE(std::string(extriLensModeName(static_cast<ExtriLensMode>(77))) == "Unknown");
    REQUIRE(std::string(eisStatusName(EisStatus::RockSteady)) == "RockSteady");
    REQUIRE(std::string(eisStatusName(static_cast<EisStatus>(-3))) == "Unknown");
}
