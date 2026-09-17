// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProtoScanner implementation plus the repeated-scalar helpers.  See
// ProtoWire.h for the wire format recap and the failure rules.

#include "osv/meta/ProtoWire.h"

#include <cstring>
#include <limits>

namespace osv::meta {

// -----------------------------------------------------------------------------
//  Names
// -----------------------------------------------------------------------------

const char* wireTypeName(WireType wire) noexcept {
    switch (wire) {
    case WireType::Varint: return "varint";
    case WireType::Fixed64: return "fixed64";
    case WireType::LengthDelimited: return "bytes";
    case WireType::StartGroup: return "start_group";
    case WireType::EndGroup: return "end_group";
    case WireType::Fixed32: return "fixed32";
    }
    return "invalid";
}

// -----------------------------------------------------------------------------
//  ProtoField accessors
// -----------------------------------------------------------------------------

float ProtoField::asFloat() const noexcept {
    switch (wire) {
    case WireType::Fixed32: {
        // Bit cast: the payload is the IEEE-754 pattern, little endian.
        float f = 0.0f;
        std::memcpy(&f, &fixed32, sizeof(f));
        return f;
    }
    case WireType::Fixed64: return static_cast<float>(asDouble());
    case WireType::Varint: return static_cast<float>(varint);
    default: return 0.0f;
    }
}

double ProtoField::asDouble() const noexcept {
    switch (wire) {
    case WireType::Fixed64: {
        double d = 0.0;
        std::memcpy(&d, &fixed64, sizeof(d));
        return d;
    }
    case WireType::Fixed32: return static_cast<double>(asFloat());
    case WireType::Varint: return static_cast<double>(varint);
    default: return 0.0;
    }
}

std::int32_t ProtoField::asI32() const noexcept {
    // Protobuf int32 is transmitted as a 64-bit two's complement varint; the
    // low 32 bits carry the value, so a plain truncation is the right decode.
    return static_cast<std::int32_t>(asU32());
}

std::uint32_t ProtoField::asU32() const noexcept {
    switch (wire) {
    case WireType::Varint: return static_cast<std::uint32_t>(varint & 0xFFFFFFFFu);
    case WireType::Fixed32: return fixed32;
    case WireType::Fixed64: return static_cast<std::uint32_t>(fixed64 & 0xFFFFFFFFu);
    default: return 0;
    }
}

std::int64_t ProtoField::asI64() const noexcept {
    switch (wire) {
    case WireType::Varint: return static_cast<std::int64_t>(varint);
    case WireType::Fixed64: return static_cast<std::int64_t>(fixed64);
    case WireType::Fixed32: return static_cast<std::int64_t>(static_cast<std::int32_t>(fixed32));
    default: return 0;
    }
}

std::uint64_t ProtoField::asU64() const noexcept {
    switch (wire) {
    case WireType::Varint: return varint;
    case WireType::Fixed64: return fixed64;
    case WireType::Fixed32: return fixed32;
    default: return 0;
    }
}

bool ProtoField::asBool() const noexcept {
    // proto3 serialises bool as a varint 0/1; be lenient about the wire type.
    return asU64() != 0;
}

std::string ProtoField::asString() const {
    if (wire != WireType::LengthDelimited) {
        return {};
    }
    return bytes.toString();
}

std::int64_t ProtoField::asZigzag() const noexcept {
    // Zigzag: 0 -> 0, 1 -> -1, 2 -> 1, 3 -> -2, ...
    const std::uint64_t n = asU64();
    return static_cast<std::int64_t>(n >> 1) ^ -static_cast<std::int64_t>(n & 1u);
}

// -----------------------------------------------------------------------------
//  ProtoScanner
// -----------------------------------------------------------------------------

ProtoScanner::ProtoScanner(ByteSpan span) noexcept : m_reader(span) {}

bool ProtoScanner::fail(const char* reason) noexcept {
    // Keep the first failure; later ones would only describe fallout.
    if (!m_failed) {
        m_failed = true;
        m_failure = reason ? reason : "unknown";
    }
    return false;
}

bool ProtoScanner::readPayload(ProtoField& out) noexcept {
    switch (out.wire) {
    case WireType::Varint:
        // ByteReader::varint already rejects more than 10 bytes.
        if (!m_reader.varint(out.varint)) {
            return fail("truncated or over-long varint payload");
        }
        return true;
    case WireType::Fixed64:
        if (!m_reader.u64le(out.fixed64)) {
            return fail("truncated fixed64 payload");
        }
        return true;
    case WireType::Fixed32:
        if (!m_reader.u32le(out.fixed32)) {
            return fail("truncated fixed32 payload");
        }
        return true;
    case WireType::LengthDelimited: {
        std::uint64_t length = 0;
        if (!m_reader.varint(length)) {
            return fail("truncated length prefix");
        }
        // The length must fit inside what is left of the buffer; a hostile
        // file can claim gigabytes here.
        if (length > m_reader.remaining()) {
            return fail("length-delimited field runs past the end of the message");
        }
        if (!m_reader.bytes(length, out.bytes)) {
            return fail("length-delimited field could not be read");
        }
        return true;
    }
    case WireType::StartGroup:
    case WireType::EndGroup:
        // Groups carry no payload of their own; handled by next().
        return true;
    }
    return fail("invalid wire type");
}

bool ProtoScanner::next(ProtoField& out) noexcept {
    if (m_failed) {
        return false;
    }
    // Nesting depth of deprecated groups we are currently skipping.  Fields
    // inside a group are decoded (so the cursor advances correctly) but not
    // returned to the caller.
    unsigned groupDepth = 0;

    while (!m_reader.atEnd()) {
        // ---- tag -------------------------------------------------------
        std::uint64_t tag = 0;
        if (!m_reader.varint(tag)) {
            return fail("truncated or over-long tag varint");
        }
        const std::uint64_t number = tag >> 3;
        const unsigned wireBits = static_cast<unsigned>(tag & 7u);
        if (number == 0 || number > kMaxProtoFieldNumber) {
            return fail("field number out of range");
        }
        if (wireBits == 6 || wireBits == 7) {
            return fail("reserved wire type 6/7");
        }

        // ---- element -----------------------------------------------------
        ProtoField field;
        field.number = static_cast<std::uint32_t>(number);
        field.wire = static_cast<WireType>(wireBits);

        if (field.wire == WireType::StartGroup) {
            ++groupDepth;
            continue;
        }
        if (field.wire == WireType::EndGroup) {
            if (groupDepth == 0) {
                return fail("end-group without a matching start-group");
            }
            --groupDepth;
            continue;
        }
        if (!readPayload(field)) {
            return false;
        }
        // Inside a group: consumed and dropped.
        if (groupDepth > 0) {
            continue;
        }
        out = field;
        return true;
    }

    // Ran out of bytes; that is only an error if a group is still open.
    if (groupDepth > 0) {
        return fail("unterminated group at end of message");
    }
    return false;
}

// -----------------------------------------------------------------------------
//  Repeated scalar helpers
// -----------------------------------------------------------------------------

namespace {

/// Append every varint of a packed payload through `push`.  Returns false on
/// a truncated trailing varint (values before it are still appended).
template <class Push>
bool forEachPackedVarint(ByteSpan payload, Push push) {
    ByteReader reader(payload);
    while (!reader.atEnd()) {
        std::uint64_t v = 0;
        if (!reader.varint(v)) {
            return false;
        }
        push(v);
    }
    return true;
}

}  // namespace

bool appendRepeatedFloat(const ProtoField& field, std::vector<float>& out) {
    if (field.wire == WireType::Fixed32) {
        out.push_back(field.asFloat());
        return true;
    }
    if (field.wire != WireType::LengthDelimited) {
        return false;
    }
    // Packed: a whole number of 4-byte little endian floats.
    if (field.bytes.size() % 4 != 0) {
        return false;
    }
    ByteReader reader(field.bytes);
    out.reserve(out.size() + field.bytes.size() / 4);
    float f = 0.0f;
    while (reader.f32le(f)) {
        out.push_back(f);
    }
    return true;
}

bool appendRepeatedDouble(const ProtoField& field, std::vector<double>& out) {
    if (field.wire == WireType::Fixed64) {
        out.push_back(field.asDouble());
        return true;
    }
    if (field.wire != WireType::LengthDelimited) {
        return false;
    }
    if (field.bytes.size() % 8 != 0) {
        return false;
    }
    ByteReader reader(field.bytes);
    out.reserve(out.size() + field.bytes.size() / 8);
    double d = 0.0;
    while (reader.f64le(d)) {
        out.push_back(d);
    }
    return true;
}

bool appendRepeatedI32(const ProtoField& field, std::vector<std::int32_t>& out) {
    if (field.wire == WireType::Varint) {
        out.push_back(field.asI32());
        return true;
    }
    if (field.wire != WireType::LengthDelimited) {
        return false;
    }
    return forEachPackedVarint(field.bytes, [&out](std::uint64_t v) {
        out.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(v & 0xFFFFFFFFu)));
    });
}

bool appendRepeatedU32(const ProtoField& field, std::vector<std::uint32_t>& out) {
    if (field.wire == WireType::Varint) {
        out.push_back(field.asU32());
        return true;
    }
    if (field.wire != WireType::LengthDelimited) {
        return false;
    }
    return forEachPackedVarint(field.bytes, [&out](std::uint64_t v) {
        out.push_back(static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    });
}

bool appendRepeatedI64(const ProtoField& field, std::vector<std::int64_t>& out) {
    if (field.wire == WireType::Varint) {
        out.push_back(field.asI64());
        return true;
    }
    if (field.wire != WireType::LengthDelimited) {
        return false;
    }
    return forEachPackedVarint(field.bytes, [&out](std::uint64_t v) { out.push_back(static_cast<std::int64_t>(v)); });
}

bool appendRepeatedU64(const ProtoField& field, std::vector<std::uint64_t>& out) {
    if (field.wire == WireType::Varint) {
        out.push_back(field.asU64());
        return true;
    }
    if (field.wire != WireType::LengthDelimited) {
        return false;
    }
    return forEachPackedVarint(field.bytes, [&out](std::uint64_t v) { out.push_back(v); });
}

}  // namespace osv::meta
