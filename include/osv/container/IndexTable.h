// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// IndexTable: DJI's private quick-access table stored in a top-level 'free'
// box right after 'ftyp' (offset 36 in .OSV files, 40 in .LRF files whose
// ftyp is four bytes longer).  Each 16-byte entry is
//     tag (4 ASCII) | u64 be offset | u32 be size
// and points at the cover art items ('covr', 'snal') inside moov/udta/ilst
// and at the payload of the trailing 'camd' box.  The camera writes it so
// its own player can find these without parsing moov; we only use it as a
// cross-check, so every entry is verified against the actual box structure
// and flagged rather than trusted.
#pragma once

#include "osv/container/Box.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace osv {

/// One entry of the index table.
struct IndexEntry {
    Fourcc tag;                     ///< 'covr', 'snal', 'camd', ...
    std::uint64_t offset = 0;       ///< Offset as written in the table.
    std::uint32_t size = 0;         ///< Size as written in the table.
    bool verified = false;          ///< True when the entry matches the file structure.
    std::uint64_t dataOffset = 0;   ///< Offset of the actual payload once verified (see below).
    std::string note;               ///< Why verification failed (empty when verified).

    /// The bytes the entry points at (empty when not verified).  For 'camd'
    /// this is the nested movie; for ilst items it is the item's data value
    /// (the JPEG for covr/snal), i.e. `offset + 24`.
    [[nodiscard]] ByteSpan payload(ByteSpan file) const noexcept {
        if (!verified) {
            return ByteSpan{};
        }
        return file.sub(dataOffset, size);
    }
};

/// The whole table.
struct IndexTable {
    bool present = false;             ///< True when a table was found.
    std::uint64_t boxOffset = 0;      ///< Offset of the 'free' box that holds it.
    std::uint64_t boxSize = 0;        ///< Size of that box.
    std::vector<IndexEntry> entries;  ///< In file order.

    /// Parse the payload of a candidate 'free' box and verify every entry
    /// against `file` (the whole file span).  Returns a table with
    /// `present == false` when the payload does not look like an index table
    /// at all (that is not an error: most 'free' boxes are just padding).
    [[nodiscard]] static IndexTable parse(ByteSpan file, const BoxHeader& freeBox, WarningList* warnings);

    /// Cheap sniff: does the payload start with a plausible entry?
    [[nodiscard]] static bool looksLikeIndexTable(ByteSpan payload) noexcept;

    /// Entry with the given tag, or null.
    [[nodiscard]] const IndexEntry* find(Fourcc tag) const noexcept;

    /// Number of entries that passed verification.
    [[nodiscard]] std::size_t verifiedCount() const noexcept;
};

}  // namespace osv
