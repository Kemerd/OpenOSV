// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Schema-less protobuf tree decoding (see ProtoTree.h).

#include "osv/meta/ProtoTree.h"

namespace osv::meta {

namespace {

/// Printable 7-bit ASCII test shared by the node helper and the heuristic.
bool allPrintableAscii(const std::uint8_t* data, std::size_t size) noexcept {
    if (!data) {
        return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] < 0x20 || data[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

/// Recursive worker.  `budget` is decremented for every node produced so the
/// whole tree stays below the caller's limit.
void decodeInto(ByteSpan span, int depth, int maxDepth, std::size_t& budget, std::vector<ProtoNode>& out,
                bool& failed, std::string& failure) {
    ProtoScanner scanner(span);
    ProtoField field;
    while (scanner.next(field)) {
        // Node budget exhausted: stop expanding but do not report an error,
        // the caller can see nodeCount hit the ceiling.
        if (budget == 0) {
            return;
        }
        --budget;

        ProtoNode node;
        node.number = field.number;
        node.wire = field.wire;
        node.varint = field.varint;
        node.fixed32 = field.fixed32;
        node.fixed64 = field.fixed64;
        if (field.wire == WireType::LengthDelimited) {
            // Keep a private copy so the tree outlives the sample buffer.
            node.bytes = field.bytes.toVector();
            // Nest only below the depth limit and only when the payload has
            // the shape of a message.
            if (depth < maxDepth && looksLikeMessage(field.bytes)) {
                node.isMessage = true;
                decodeInto(field.bytes, depth + 1, maxDepth, budget, node.children, node.truncated, node.failure);
            }
        }
        out.push_back(std::move(node));
    }
    if (scanner.failed()) {
        failed = true;
        failure = scanner.failure();
    }
}

}  // namespace

bool ProtoNode::isPrintableAscii() const noexcept {
    return !bytes.empty() && allPrintableAscii(bytes.data(), bytes.size());
}

bool looksLikeMessage(ByteSpan span) noexcept {
    if (span.empty()) {
        return false;
    }
    // Four or more printable characters: almost certainly a string.
    if (span.size() >= 4 && allPrintableAscii(span.data(), span.size())) {
        return false;
    }
    // Must scan cleanly to the exact end using only the common wire types.
    ProtoScanner scanner(span);
    ProtoField field;
    std::size_t count = 0;
    while (scanner.next(field)) {
        if (field.wire != WireType::Varint && field.wire != WireType::Fixed32 && field.wire != WireType::Fixed64 &&
            field.wire != WireType::LengthDelimited) {
            return false;
        }
        ++count;
    }
    if (scanner.failed() || !scanner.atEnd()) {
        return false;
    }
    return count > 0;
}

ProtoTree decodeTree(ByteSpan span, int depth, int maxDepth, std::size_t maxNodes) {
    ProtoTree tree;
    // Defensive clamps: a negative depth or a zero budget is a caller bug,
    // not a reason to misbehave.
    if (depth < 0) {
        depth = 0;
    }
    if (maxDepth < 0) {
        maxDepth = 0;
    }
    std::size_t budget = maxNodes == 0 ? 1 : maxNodes;
    const std::size_t initialBudget = budget;
    decodeInto(span, depth, maxDepth, budget, tree.fields, tree.failed, tree.failure);
    tree.nodeCount = initialBudget - budget;
    return tree;
}

const ProtoNode* findChild(const std::vector<ProtoNode>& nodes, std::uint32_t number) noexcept {
    for (const ProtoNode& n : nodes) {
        if (n.number == number) {
            return &n;
        }
    }
    return nullptr;
}

}  // namespace osv::meta
