// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// BoxHeader: the parsed header of one ISO BMFF box plus a bounds-safe view of
// its contents.  Every offset is absolute within the "root" span the walker
// was started on (the whole file for a normal movie, the `camd` payload for
// the nested metadata movie), which is exactly what the chunk offset tables
// (stco/co64) are relative to.
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace osv {

/// Non-fatal parser diagnostics.  Every parse entry point accepts an optional
/// pointer to one of these; a null pointer silently drops the messages.
using WarningList = std::vector<std::string>;

/// Append `message` to `warnings` when the caller supplied a list.
inline void addWarning(WarningList* warnings, std::string message) {
    // A null list means the caller does not care about diagnostics.
    if (!warnings) {
        return;
    }
    warnings->push_back(std::move(message));
}

/// One parsed box header.  Sizes are already resolved (`size == 0` -> to the
/// end of the parent, `size == 1` -> 64-bit largesize) and clamped to the
/// parent so that `whole`, `body` and `payload` never reach outside the data.
struct BoxHeader {
    /// Box type, e.g. 'moov'.  For 'uuid' boxes the type stays 'uuid' and the
    /// 16-byte identifier is in `uuid`.
    Fourcc type;

    /// Absolute offset of the first header byte within the root span.
    std::uint64_t offset = 0;

    /// Total box size in bytes including the header, after resolving the
    /// special values and clamping to the parent (see `truncated`).
    std::uint64_t size = 0;

    /// Size as it appeared on disk before resolution/clamping (0 or 1 for the
    /// special encodings, otherwise the same as `size` unless truncated).
    std::uint64_t declaredSize = 0;

    /// Bytes of the basic header: 8, or 16 with largesize, plus 16 for 'uuid'.
    std::uint32_t headerSize = 0;

    /// Nesting depth: 0 for top-level boxes, 1 for children of moov, ...
    int depth = 0;

    /// True when the 64-bit largesize encoding was used.
    bool largeSize = false;

    /// True when the size field was 0 ("extends to the end of the parent").
    bool toEnd = false;

    /// True when the declared size ran past the parent's end and `size` had to
    /// be clamped.  Contents may be incomplete.
    bool truncated = false;

    /// True for FullBox types (the 4 bytes after the basic header are
    /// version + 24-bit flags and `payload` starts after them).
    bool isFull = false;

    /// FullBox version (0 when not a full box).
    std::uint8_t version = 0;

    /// FullBox 24-bit flags (0 when not a full box).
    std::uint32_t flags = 0;

    /// True when the box is a 'uuid' box and `uuid` is valid.
    bool hasUuid = false;

    /// Extended type of a 'uuid' box.
    std::array<std::uint8_t, 16> uuid{};

    /// The entire box: header and contents (clamped).
    ByteSpan whole;

    /// Contents after the basic header (includes version/flags for full boxes).
    ByteSpan body;

    /// Contents after version/flags for full boxes; identical to `body` for
    /// plain boxes.  This is what field parsers read from.
    ByteSpan payload;

    /// Absolute offset one past the last byte of the box.
    [[nodiscard]] std::uint64_t end() const noexcept { return offset + size; }

    /// Absolute offset of `body`.
    [[nodiscard]] std::uint64_t bodyOffset() const noexcept { return offset + headerSize; }

    /// Absolute offset of `payload`.
    [[nodiscard]] std::uint64_t payloadOffset() const noexcept {
        return offset + headerSize + (isFull ? std::uint64_t{4} : std::uint64_t{0});
    }

    /// True when the box is complete (declared size fully present).
    [[nodiscard]] bool complete() const noexcept { return !truncated; }

    /// Short description for diagnostics, e.g. "'moov' @31875072 (89776 bytes)".
    [[nodiscard]] std::string describe() const {
        return "'" + type.str() + "' @" + std::to_string(offset) + " (" + std::to_string(size) + " bytes)";
    }
};

}  // namespace osv
