// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProtoTree: schema-less decoding of a protobuf message into a tree.
//
// `osvtool probe --raw` needs to show *everything* a djmd sample contains,
// including fields the typed decoder does not know about.  decodeTree()
// walks a message recursively: every length-delimited payload that
// looksLikeMessage() is decoded as a nested message, everything else is kept
// as raw bytes.  Because the wire format cannot distinguish a string from a
// message, the nesting decision is a heuristic and is documented as such.
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/meta/ProtoWire.h"

#include <cstdint>
#include <string>
#include <vector>

namespace osv::meta {

/// One element of a decoded tree.  Unknown fields are retained verbatim so
/// nothing in the file is ever hidden from the user.
struct ProtoNode {
    std::uint32_t number = 0;               ///< Field number.
    WireType wire = WireType::Varint;       ///< Wire type of the element.
    std::uint64_t varint = 0;               ///< Payload for Varint.
    std::uint32_t fixed32 = 0;              ///< Raw bits for Fixed32.
    std::uint64_t fixed64 = 0;              ///< Raw bits for Fixed64.
    std::vector<std::uint8_t> bytes;        ///< Copy of a LengthDelimited payload (also kept when nested).
    bool isMessage = false;                 ///< True when `children` holds the decoded payload.
    std::vector<ProtoNode> children;        ///< Nested fields (only when isMessage).
    bool truncated = false;                 ///< Nested scan hit malformed data (children are partial).
    std::string failure;                    ///< Reason for `truncated`.

    /// True when every byte of the payload is printable 7-bit ASCII (a hint
    /// that the field is a string).
    [[nodiscard]] bool isPrintableAscii() const noexcept;
};

/// Result of decodeTree(): the top-level fields plus the scan status.
struct ProtoTree {
    std::vector<ProtoNode> fields;  ///< Top-level fields in file order.
    bool failed = false;            ///< Top-level scan stopped on malformed data.
    std::string failure;            ///< Reason for `failed`.
    std::size_t nodeCount = 0;      ///< Total nodes decoded (all levels).
};

/// Heuristic: could `span` be a serialised protobuf message?
///
/// Returns true when the span is non-empty, scans cleanly to its exact end
/// with in-range field numbers and known wire types, and is not a run of
/// four or more printable ASCII characters (which is far more likely to be a
/// string such as "video" or "02.01.15").  Groups and reserved wire types
/// make the answer false.
[[nodiscard]] bool looksLikeMessage(ByteSpan span) noexcept;

/// Decode `span` recursively.  `depth` is the current nesting level and
/// `maxDepth` the level at which length-delimited payloads are no longer
/// expanded (kept as bytes) so a hostile file cannot recurse without bound.
/// `maxNodes` bounds the total node count for the same reason.
[[nodiscard]] ProtoTree decodeTree(ByteSpan span, int depth = 0, int maxDepth = 12, std::size_t maxNodes = 200000);

/// Find the first child with the given field number, or nullptr.
[[nodiscard]] const ProtoNode* findChild(const std::vector<ProtoNode>& nodes, std::uint32_t number) noexcept;

}  // namespace osv::meta
