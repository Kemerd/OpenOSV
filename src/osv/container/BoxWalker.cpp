// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// BoxWalker implementation.  See BoxWalker.h for the safety rules; the code
// below is deliberately paranoid because it is the first thing that touches
// bytes from disk.

#include "osv/container/BoxWalker.h"

#include "osv/core/ByteReader.h"

#include <algorithm>
#include <cstring>

namespace osv {

namespace {

// Box types whose contents start with version(1) + flags(3).  Anything not
// in this list is treated as a plain box.  'meta' is handled separately.
constexpr Fourcc kFullBoxTypes[] = {
    Fourcc{"mvhd"}, Fourcc{"tkhd"}, Fourcc{"mdhd"}, Fourcc{"hdlr"}, Fourcc{"vmhd"}, Fourcc{"smhd"}, Fourcc{"hmhd"},
    Fourcc{"nmhd"}, Fourcc{"gmin"}, Fourcc{"dref"}, Fourcc{"url "}, Fourcc{"urn "}, Fourcc{"stsd"}, Fourcc{"stts"},
    Fourcc{"ctts"}, Fourcc{"cslg"}, Fourcc{"stss"}, Fourcc{"stsc"}, Fourcc{"stsz"}, Fourcc{"stz2"}, Fourcc{"stco"},
    Fourcc{"co64"}, Fourcc{"stsh"}, Fourcc{"padb"}, Fourcc{"stdp"}, Fourcc{"sdtp"}, Fourcc{"sbgp"}, Fourcc{"sgpd"},
    Fourcc{"subs"}, Fourcc{"saiz"}, Fourcc{"saio"}, Fourcc{"elst"}, Fourcc{"mehd"}, Fourcc{"trex"}, Fourcc{"mfhd"},
    Fourcc{"tfhd"}, Fourcc{"tfdt"}, Fourcc{"trun"}, Fourcc{"tfra"}, Fourcc{"mfro"}, Fourcc{"sidx"}, Fourcc{"ssix"},
    Fourcc{"prft"}, Fourcc{"pdin"}, Fourcc{"iods"}, Fourcc{"xml "}, Fourcc{"bxml"}, Fourcc{"pitm"}, Fourcc{"iloc"},
    Fourcc{"iinf"}, Fourcc{"infe"}, Fourcc{"ipro"}, Fourcc{"iref"}, Fourcc{"ipma"}, Fourcc{"chpl"}, Fourcc{"esds"},
    Fourcc{"schm"}, Fourcc{"tsel"}, Fourcc{"kind"}, Fourcc{"cprt"}, Fourcc{"trep"}, Fourcc{"leva"}, Fourcc{"stri"},
    Fourcc{"stsg"}, Fourcc{"srpp"}, Fourcc{"assp"}, Fourcc{"tenc"}, Fourcc{"senc"}, Fourcc{"pssh"}, Fourcc{"keys"},
};

// Box types that may legitimately start an ISO BMFF file or appear at its
// top level.  Random data will practically never hit one of these.
constexpr Fourcc kTopLevelTypes[] = {
    Fourcc{"ftyp"}, Fourcc{"styp"}, Fourcc{"free"}, Fourcc{"skip"}, Fourcc{"wide"}, Fourcc{"mdat"},
    Fourcc{"moov"}, Fourcc{"moof"}, Fourcc{"mfra"}, Fourcc{"sidx"}, Fourcc{"ssix"}, Fourcc{"prft"},
    Fourcc{"meta"}, Fourcc{"uuid"}, Fourcc{"pdin"}, Fourcc{"camd"}, Fourcc{"emsg"}, Fourcc{"jP  "},
};

}  // namespace

bool BoxWalker::isFullBoxType(Fourcc type) noexcept {
    // Linear scan: the list is tiny and this runs once per box.
    for (const Fourcc& candidate : kFullBoxTypes) {
        if (candidate == type) {
            return true;
        }
    }
    return false;
}

bool BoxWalker::isKnownTopLevelType(Fourcc type) noexcept {
    for (const Fourcc& candidate : kTopLevelTypes) {
        if (candidate == type) {
            return true;
        }
    }
    return false;
}

Result<BoxHeader> BoxWalker::readHeader(ByteSpan root, std::uint64_t offset, std::uint64_t parentEnd, int depth) {
    // Clamp the parent end to the data we actually have; a caller passing a
    // bogus end must not let us read past the span.
    if (parentEnd > root.size()) {
        parentEnd = root.size();
    }
    // The box must start inside the parent.
    if (offset >= parentEnd) {
        return Error{ErrorCode::Truncated, "box offset " + std::to_string(offset) + " is at or past the parent end " +
                                               std::to_string(parentEnd)};
    }
    const std::uint64_t available = parentEnd - offset;

    // ---- basic header: size(4) + type(4) --------------------------------
    if (available < 8) {
        return Error{ErrorCode::Truncated, "only " + std::to_string(available) + " byte(s) left at offset " +
                                               std::to_string(offset) + ", not enough for a box header"};
    }
    ByteReader reader(root.sub(offset, available));
    std::uint32_t size32 = 0;
    Fourcc type;
    if (!reader.u32be(size32) || !reader.fourcc(type)) {
        // Cannot happen after the check above, but never trust arithmetic.
        return Error{ErrorCode::Internal, "box header read failed"};
    }

    BoxHeader header;
    header.type = type;
    header.offset = offset;
    header.depth = depth;
    header.declaredSize = size32;
    header.headerSize = 8;

    // ---- size resolution ------------------------------------------------
    std::uint64_t size = size32;
    if (size32 == 1) {
        // 64-bit largesize follows the type.
        std::uint64_t large = 0;
        if (!reader.u64be(large)) {
            return Error{ErrorCode::Truncated,
                         "box '" + type.str() + "' at " + std::to_string(offset) + " declares largesize but ends early"};
        }
        header.largeSize = true;
        header.headerSize = 16;
        header.declaredSize = large;
        size = large;
    } else if (size32 == 0) {
        // Box extends to the end of the enclosing container.
        header.toEnd = true;
        size = available;
    }

    // A 'uuid' box carries its 16-byte extended type right after the header.
    if (type == Fourcc{"uuid"}) {
        if (!reader.copyTo(header.uuid.data(), header.uuid.size())) {
            return Error{ErrorCode::Truncated,
                         "'uuid' box at " + std::to_string(offset) + " ends before its extended type"};
        }
        header.hasUuid = true;
        header.headerSize += 16;
    }

    // The size can never be smaller than the header we just consumed.
    if (size < header.headerSize) {
        return Error{ErrorCode::Malformed, "box '" + type.str() + "' at " + std::to_string(offset) + " has size " +
                                               std::to_string(size) + " smaller than its " +
                                               std::to_string(header.headerSize) + "-byte header"};
    }

    // Clamp to the parent; remember that we did so.
    if (size > available) {
        header.truncated = true;
        size = available;
    }
    header.size = size;

    // ---- spans ----------------------------------------------------------
    header.whole = root.sub(offset, size);
    header.body = root.sub(offset + header.headerSize, size - header.headerSize);
    header.payload = header.body;

    // ---- full box detection --------------------------------------------
    bool full = isFullBoxType(type);
    if (type == Fourcc{"meta"}) {
        // ISO writes 'meta' as a FullBox, QuickTime (and DJI's moov-level
        // 'meta') as a plain box whose first child is 'hdlr'.  If the bytes
        // right after the header already form an 'hdlr' box header, there
        // are no version/flags.
        const ByteSpan peek = header.body;
        const bool plainQuickTime = peek.size() >= 8 && peek[4] == 'h' && peek[5] == 'd' && peek[6] == 'l' && peek[7] == 'r';
        full = !plainQuickTime;
    }
    if (full) {
        header.isFull = true;
        if (header.body.size() >= 4) {
            header.version = header.body[0];
            header.flags = (static_cast<std::uint32_t>(header.body[1]) << 16) |
                           (static_cast<std::uint32_t>(header.body[2]) << 8) | header.body[3];
            header.payload = header.body.sub(4);
        } else {
            // A full box that cannot even hold version/flags is unusable.
            header.truncated = true;
            header.payload = ByteSpan{};
        }
    }
    return header;
}

void BoxWalker::forEachChild(ByteSpan root, std::uint64_t start, std::uint64_t end, int depth, WarningList* warnings,
                             const Visitor& visitor) {
    // Depth cap: a hostile file could nest boxes forever within the size limit.
    if (depth > kMaxDepth) {
        addWarning(warnings, "box nesting deeper than " + std::to_string(kMaxDepth) + " at offset " +
                                 std::to_string(start) + ", children ignored");
        return;
    }
    if (!visitor) {
        return;
    }
    if (end > root.size()) {
        end = root.size();
    }

    std::uint64_t pos = start;
    while (pos < end) {
        // Parse the next header; a failure ends this parent.
        Result<BoxHeader> header = readHeader(root, pos, end, depth);
        if (!header.ok()) {
            // A few trailing zero bytes at the end of a container are common
            // padding; anything else is worth reporting.
            const std::uint64_t remaining = end - pos;
            if (remaining < 8) {
                bool allZero = true;
                for (std::uint64_t i = 0; i < remaining; ++i) {
                    if (root[pos + i] != 0) {
                        allZero = false;
                        break;
                    }
                }
                if (!allZero) {
                    addWarning(warnings, header.error().message);
                }
            } else {
                addWarning(warnings, header.error().message);
            }
            return;
        }

        const BoxHeader& box = header.value();
        if (box.truncated) {
            addWarning(warnings, "box " + box.describe() + " declares " + std::to_string(box.declaredSize) +
                                     " bytes but only " + std::to_string(box.size) +
                                     " remain in its parent (truncated); siblings after it are unreachable");
        }

        // Deliver the box (truncated ones too - the caller may still salvage
        // a partial moov) and honour the visitor's stop request.
        if (!visitor(box)) {
            return;
        }
        if (box.truncated) {
            return;
        }

        // Advance.  size >= headerSize >= 8 is guaranteed by readHeader, so
        // the loop always makes progress.
        pos = box.end();
    }
}

void BoxWalker::forEachChild(ByteSpan root, const BoxHeader& parent, WarningList* warnings, const Visitor& visitor) {
    // Children live in the payload (after version/flags for full boxes).
    forEachChild(root, parent.payloadOffset(), parent.end(), parent.depth + 1, warnings, visitor);
}

void BoxWalker::forEachTopLevel(ByteSpan root, WarningList* warnings, const Visitor& visitor) {
    forEachChild(root, 0, root.size(), 0, warnings, visitor);
}

std::vector<BoxHeader> BoxWalker::children(ByteSpan root, const BoxHeader& parent, WarningList* warnings) {
    std::vector<BoxHeader> out;
    forEachChild(root, parent, warnings, [&out](const BoxHeader& box) {
        out.push_back(box);
        return true;
    });
    return out;
}

std::optional<BoxHeader> BoxWalker::findChild(ByteSpan root, const BoxHeader& parent, Fourcc type,
                                              WarningList* warnings) {
    std::optional<BoxHeader> found;
    forEachChild(root, parent, warnings, [&found, type](const BoxHeader& box) {
        if (box.type == type) {
            found = box;
            return false;
        }
        return true;
    });
    return found;
}

std::vector<BoxHeader> BoxWalker::findChildren(ByteSpan root, const BoxHeader& parent, Fourcc type,
                                               WarningList* warnings) {
    std::vector<BoxHeader> out;
    forEachChild(root, parent, warnings, [&out, type](const BoxHeader& box) {
        if (box.type == type) {
            out.push_back(box);
        }
        return true;
    });
    return out;
}

std::optional<BoxHeader> BoxWalker::findPath(ByteSpan root, const BoxHeader& parent, const std::vector<Fourcc>& path,
                                             WarningList* warnings) {
    // Walk down one level per path element.
    std::optional<BoxHeader> current = parent;
    for (const Fourcc& step : path) {
        if (!current) {
            return std::nullopt;
        }
        current = findChild(root, *current, step, warnings);
    }
    return current;
}

}  // namespace osv
