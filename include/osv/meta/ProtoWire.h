// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProtoWire: a hand-rolled protobuf (proto3) wire-format scanner.
//
// The djmd metadata samples are raw protobuf messages whose schema we only
// know from observation, so instead of depending on libprotobuf and a
// generated class we walk the wire format directly.  The scanner yields one
// ProtoField per top-level element and never throws: a malformed element
// stops the scan and sets failed(), and everything decoded before that point
// stays valid so callers can keep whatever they already have.
//
// Wire format recap (https://protobuf.dev/programming-guides/encoding/):
//   tag   = varint, field number = tag >> 3, wire type = tag & 7
//   0     varint          (int32/int64/uint32/uint64/bool/enum, zigzag sint*)
//   1     64-bit          (fixed64/sfixed64/double), little endian
//   2     length-delimited (string/bytes/embedded message/packed repeated)
//   3/4   start/end group (deprecated; skipped here)
//   5     32-bit          (fixed32/sfixed32/float), little endian
#pragma once

#include "osv/core/ByteReader.h"
#include "osv/core/ByteSpan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace osv::meta {

/// Protobuf wire types (the low three bits of a tag).
enum class WireType : std::uint8_t {
    Varint = 0,           ///< Base-128 varint.
    Fixed64 = 1,          ///< 8 bytes little endian.
    LengthDelimited = 2,  ///< varint length followed by that many bytes.
    StartGroup = 3,       ///< Deprecated group start (skipped).
    EndGroup = 4,         ///< Deprecated group end (skipped).
    Fixed32 = 5           ///< 4 bytes little endian.
};

/// Stable, printable name of a wire type ("varint", "fixed64", ...).
[[nodiscard]] const char* wireTypeName(WireType wire) noexcept;

/// Largest legal protobuf field number (2^29 - 1).
inline constexpr std::uint32_t kMaxProtoFieldNumber = (1u << 29) - 1u;

/// One decoded wire element.  Only the member matching `wire` is meaningful;
/// the others keep their zero defaults.  The `bytes` span aliases the input
/// buffer of the scanner, so it is valid as long as that buffer is.
struct ProtoField {
    std::uint32_t number = 0;          ///< Field number (1 .. kMaxProtoFieldNumber).
    WireType wire = WireType::Varint;  ///< Wire type of this element.
    std::uint64_t varint = 0;          ///< Payload for WireType::Varint.
    std::uint32_t fixed32 = 0;         ///< Raw bits for WireType::Fixed32.
    std::uint64_t fixed64 = 0;         ///< Raw bits for WireType::Fixed64.
    ByteSpan bytes;                    ///< Payload for WireType::LengthDelimited.

    // ---------------------------------------------------------------------
    //  Typed accessors.  Each one is total: a wire type that does not match
    //  the requested interpretation yields a best-effort conversion of the
    //  payload (e.g. asFloat() on a varint returns the varint as a float)
    //  rather than undefined behaviour.  Callers that care check `wire`.
    // ---------------------------------------------------------------------

    /// IEEE-754 float: bit cast of fixed32, narrowed double of fixed64,
    /// or the varint value converted.
    [[nodiscard]] float asFloat() const noexcept;
    /// IEEE-754 double: bit cast of fixed64, widened float of fixed32,
    /// or the varint value converted.
    [[nodiscard]] double asDouble() const noexcept;
    /// int32 (varint truncated to 32 bits, two's complement; sfixed32 for
    /// fixed32; low 32 bits of fixed64).
    [[nodiscard]] std::int32_t asI32() const noexcept;
    /// uint32 (varint truncated to 32 bits; fixed32 bits; low bits of fixed64).
    [[nodiscard]] std::uint32_t asU32() const noexcept;
    /// int64 (varint reinterpreted; sfixed64; sign-extended sfixed32).
    [[nodiscard]] std::int64_t asI64() const noexcept;
    /// uint64 (varint; fixed64 bits; zero-extended fixed32).
    [[nodiscard]] std::uint64_t asU64() const noexcept;
    /// proto3 bool: any non-zero varint (or non-zero fixed payload) is true.
    [[nodiscard]] bool asBool() const noexcept;
    /// UTF-8 string copy of a length-delimited payload; empty for other wire
    /// types.  Embedded NULs are preserved.
    [[nodiscard]] std::string asString() const;
    /// Zigzag decoded sint32/sint64 (only meaningful for varints).
    [[nodiscard]] std::int64_t asZigzag() const noexcept;
};

/// Sequential scanner over the fields of one message.
///
/// Usage:
///   ProtoScanner scanner(sample);
///   ProtoField field;
///   while (scanner.next(field)) { switch (field.number) { ... } }
///   if (scanner.failed()) { warn(scanner.failure()); }
///
/// Groups (wire types 3/4) are consumed transparently: the fields inside a
/// group are skipped and never returned.  Wire types 6 and 7, a field number
/// of 0 or above 2^29-1, a varint longer than 10 bytes, a length-delimited
/// element whose length runs past the end of the buffer and an unbalanced
/// group all stop the scan with failed() == true.
class ProtoScanner {
public:
    /// Scan `span` from its first byte.
    explicit ProtoScanner(ByteSpan span) noexcept;

    /// Decode the next top-level field into `out`.  Returns false at the end
    /// of the buffer or when a malformed element was hit (see failed()).
    [[nodiscard]] bool next(ProtoField& out) noexcept;

    /// True when scanning stopped because of malformed data.
    [[nodiscard]] bool failed() const noexcept { return m_failed; }

    /// Human readable reason for failed(), empty otherwise.
    [[nodiscard]] const std::string& failure() const noexcept { return m_failure; }

    /// True when every byte has been consumed (and nothing failed).
    [[nodiscard]] bool atEnd() const noexcept { return !m_failed && m_reader.atEnd(); }

    /// Current byte offset inside the scanned span.
    [[nodiscard]] std::uint64_t pos() const noexcept { return m_reader.pos(); }

    /// Size of the scanned span in bytes.
    [[nodiscard]] std::uint64_t size() const noexcept { return m_reader.size(); }

private:
    /// Record a failure (first one wins) and return false for convenience.
    bool fail(const char* reason) noexcept;

    /// Read the payload of a single element at the cursor into `out` (after
    /// its tag has been decoded).  Returns false on malformed data.
    bool readPayload(ProtoField& out) noexcept;

    ByteReader m_reader;
    bool m_failed = false;
    std::string m_failure;
};

// -----------------------------------------------------------------------------
//  Repeated scalar helpers
//
//  proto3 encodes repeated numeric fields packed by default (one length-
//  delimited element holding all values) but parsers must also accept the
//  unpacked form (one element per value, possibly interleaved with other
//  fields).  Each helper appends whatever `field` carries to `out` and returns
//  false when the element cannot be interpreted as the requested type (wrong
//  wire type, packed payload with a partial value, truncated varint).
// -----------------------------------------------------------------------------

/// Repeated float: packed fixed32 run (len % 4 == 0) or a single fixed32.
bool appendRepeatedFloat(const ProtoField& field, std::vector<float>& out);
/// Repeated double: packed fixed64 run (len % 8 == 0) or a single fixed64.
bool appendRepeatedDouble(const ProtoField& field, std::vector<double>& out);
/// Repeated int32: packed varints or a single varint (two's complement wrap).
bool appendRepeatedI32(const ProtoField& field, std::vector<std::int32_t>& out);
/// Repeated uint32: packed varints or a single varint (truncated to 32 bits).
bool appendRepeatedU32(const ProtoField& field, std::vector<std::uint32_t>& out);
/// Repeated int64: packed varints or a single varint.
bool appendRepeatedI64(const ProtoField& field, std::vector<std::int64_t>& out);
/// Repeated uint64: packed varints or a single varint.
bool appendRepeatedU64(const ProtoField& field, std::vector<std::uint64_t>& out);

}  // namespace osv::meta
