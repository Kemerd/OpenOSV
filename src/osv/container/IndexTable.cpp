// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DJI index table parsing and verification.

#include "osv/container/IndexTable.h"

#include "osv/container/BoxWalker.h"
#include "osv/core/ByteReader.h"

namespace osv {

namespace {

/// Size of one table entry: tag(4) + offset(8) + size(4).
constexpr std::uint64_t kEntrySize = 16;

/// Bytes between the start of an ilst item box and its value: item header
/// (8) + 'data' box header (8) + type indicator (4) + locale (4).
constexpr std::uint64_t kIlstValueOffset = 24;

/// True when all four bytes of a tag are printable ASCII letters/digits.
bool isPrintableTag(Fourcc tag) noexcept {
    for (int shift = 24; shift >= 0; shift -= 8) {
        const std::uint8_t c = static_cast<std::uint8_t>(tag.v >> shift);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// Read the 32-bit size and fourcc of a box header at `offset` (both zero
/// when the bytes are not available).
void peekBoxHeader(ByteSpan file, std::uint64_t offset, std::uint32_t& size, Fourcc& type) noexcept {
    size = 0;
    type = Fourcc{};
    if (!file.contains(offset, 8)) {
        return;
    }
    ByteReader reader(file.sub(offset, 8));
    reader.u32be(size);
    reader.fourcc(type);
}

/// Verify one entry against the file structure and fill dataOffset/note.
void verifyEntry(ByteSpan file, IndexEntry& entry) {
    entry.verified = false;
    entry.dataOffset = 0;
    entry.note.clear();

    // The referenced range must lie inside the file.
    if (!file.contains(entry.offset, entry.size)) {
        entry.note = "range " + std::to_string(entry.offset) + "+" + std::to_string(entry.size) +
                     " lies outside the file (" + std::to_string(file.size()) + " bytes)";
        return;
    }

    std::uint32_t size = 0;
    Fourcc type;
    if (entry.tag == Fourcc{"camd"}) {
        // The entry points at the payload of a top-level 'camd' box: the box
        // header sits 8 bytes earlier and its size is payload + 8.
        if (entry.offset < 8) {
            entry.note = "camd payload offset smaller than a box header";
            return;
        }
        peekBoxHeader(file, entry.offset - 8, size, type);
        if (type != entry.tag) {
            entry.note = "no 'camd' box header at " + std::to_string(entry.offset - 8) + " (found '" + type.str() + "')";
            return;
        }
        if (static_cast<std::uint64_t>(size) != static_cast<std::uint64_t>(entry.size) + 8) {
            entry.note = "camd box size " + std::to_string(size) + " does not match entry size + 8 (" +
                         std::to_string(static_cast<std::uint64_t>(entry.size) + 8) + ")";
            return;
        }
        entry.dataOffset = entry.offset;
        entry.verified = true;
        return;
    }

    // Everything else observed so far (covr, snal) points at the start of an
    // ilst item box whose value begins 24 bytes in and is `size` bytes long.
    peekBoxHeader(file, entry.offset, size, type);
    if (type == entry.tag) {
        if (static_cast<std::uint64_t>(size) < kIlstValueOffset + entry.size) {
            entry.note = "ilst item '" + type.str() + "' is " + std::to_string(size) + " bytes, too small for a " +
                         std::to_string(entry.size) + "-byte value";
            return;
        }
        if (!file.contains(entry.offset + kIlstValueOffset, entry.size)) {
            entry.note = "ilst item value lies outside the file";
            return;
        }
        // The 'data' box must follow the item header.
        std::uint32_t dataSize = 0;
        Fourcc dataType;
        peekBoxHeader(file, entry.offset + 8, dataSize, dataType);
        if (dataType != Fourcc{"data"}) {
            entry.note = "ilst item is not followed by a 'data' box";
            return;
        }
        entry.dataOffset = entry.offset + kIlstValueOffset;
        entry.verified = true;
        return;
    }

    // Unknown layout: accept a plain box whose header matches the tag and
    // whose size equals the entry size (a generic "box at offset" entry).
    if (entry.offset >= 8) {
        peekBoxHeader(file, entry.offset - 8, size, type);
        if (type == entry.tag && static_cast<std::uint64_t>(size) == static_cast<std::uint64_t>(entry.size) + 8) {
            entry.dataOffset = entry.offset;
            entry.verified = true;
            return;
        }
    }
    entry.note = "no box matching '" + entry.tag.str() + "' at the referenced offset";
}

}  // namespace

bool IndexTable::looksLikeIndexTable(ByteSpan payload) noexcept {
    // Needs at least one entry with a printable tag and a non-zero size.
    if (payload.size() < kEntrySize) {
        return false;
    }
    ByteReader reader(payload);
    Fourcc tag;
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
    if (!reader.fourcc(tag) || !reader.u64be(offset) || !reader.u32be(size)) {
        return false;
    }
    return isPrintableTag(tag) && size != 0;
}

IndexTable IndexTable::parse(ByteSpan file, const BoxHeader& freeBox, WarningList* warnings) {
    IndexTable table;
    table.boxOffset = freeBox.offset;
    table.boxSize = freeBox.size;
    if (!looksLikeIndexTable(freeBox.payload)) {
        return table;
    }
    table.present = true;

    ByteReader reader(freeBox.payload);
    while (reader.remaining() >= kEntrySize) {
        IndexEntry entry;
        if (!reader.fourcc(entry.tag) || !reader.u64be(entry.offset) || !reader.u32be(entry.size)) {
            break;
        }
        // The table is zero padded to the box size; the first all-zero tag
        // ends it.  A non-printable tag means we have run into something else.
        if (entry.tag.v == 0) {
            break;
        }
        if (!isPrintableTag(entry.tag)) {
            addWarning(warnings, "index table entry with non-ASCII tag " + entry.tag.hex() + " ends the table");
            break;
        }
        verifyEntry(file, entry);
        if (!entry.verified) {
            addWarning(warnings, "index table entry '" + entry.tag.str() + "' failed verification: " + entry.note);
        }
        table.entries.push_back(entry);
    }
    return table;
}

const IndexEntry* IndexTable::find(Fourcc tag) const noexcept {
    for (const IndexEntry& entry : entries) {
        if (entry.tag == tag) {
            return &entry;
        }
    }
    return nullptr;
}

std::size_t IndexTable::verifiedCount() const noexcept {
    std::size_t n = 0;
    for (const IndexEntry& entry : entries) {
        if (entry.verified) {
            ++n;
        }
    }
    return n;
}

}  // namespace osv
