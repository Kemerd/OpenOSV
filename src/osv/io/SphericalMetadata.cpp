// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Spherical Video V1 + V2 injection.  See SphericalMetadata.h for what is
// written and why; the code below is organised as the rewrite runs:
//
//   1. scanTopLevel      walk the top-level box headers straight from disk
//                        (a few reads, no mapping) and find the one 'moov';
//   2. MoovRebuilder     load 'moov' and rebuild it with the tags, re-sizing
//                        every box on the path to the video sample entries;
//   3. shiftChunkOffsets move every stco / co64 entry that points past the
//                        old 'moov' by the number of bytes it grew;
//   4. writeRewritten    stream [start, moov) + new moov + [moov end, EOF)
//                        into a temporary file beside the target;
//   5. replaceFile       move the temporary file over the target atomically.
//
// Every box header goes through BoxWalker::readHeader, and every list of
// children is taken strictly: a child that does not fit its parent, or stray
// bytes that are not zero padding, refuse the file instead of being skipped,
// because a rewrite must never silently drop part of somebody's video.

#include "osv/io/SphericalMetadata.h"

#include "osv/container/Box.h"
#include "osv/container/BoxWalker.h"
#include "osv/core/ByteReader.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Fourcc.h"
#include "osv/core/Log.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace osv::io {

namespace {

using Bytes = std::vector<std::uint8_t>;

/// Bytes of fixed fields in a VisualSampleEntry body before its child boxes
/// (ISO/IEC 14496-12 12.1.3; a QuickTime video sample description has the
/// same 78: version, revision, vendor and qualities where ISO has reserved
/// and pre_defined fields).
constexpr std::uint64_t kVisualEntryFixedBytes = 78;

/// Most children one container may have before the file is treated as
/// hostile.  Real containers have a dozen at most.
constexpr std::size_t kMaxChildren = std::size_t{1} << 16;

/// Most top-level boxes a file may have.  Every box is at least 8 bytes, so
/// the scan terminates anyway; the cap keeps a crafted file of millions of
/// empty boxes from growing the list without bound.
constexpr std::size_t kMaxTopLevelBoxes = std::size_t{1} << 20;

/// Block size of the streamed copy: large enough that the per-call overhead
/// vanishes against the disk, small enough to be a trivial allocation.
constexpr std::size_t kCopyChunkBytes = std::size_t{8} << 20;

// ---------------------------------------------------------------------------
//  Big-endian writers
// ---------------------------------------------------------------------------

void putU8(Bytes& out, std::uint8_t value) {
    out.push_back(value);
}

void putU32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putU64(Bytes& out, std::uint64_t value) {
    putU32(out, static_cast<std::uint32_t>(value >> 32));
    putU32(out, static_cast<std::uint32_t>(value));
}

void putFourcc(Bytes& out, Fourcc type) {
    putU32(out, type.v);
}

void putSpan(Bytes& out, ByteSpan bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Overwrite 4 bytes at `pos` (the caller has checked the range).
void patchU32(Bytes& out, std::size_t pos, std::uint32_t value) {
    if (pos > out.size() || out.size() - pos < 4) {
        return;
    }
    out[pos] = static_cast<std::uint8_t>(value >> 24);
    out[pos + 1] = static_cast<std::uint8_t>(value >> 16);
    out[pos + 2] = static_cast<std::uint8_t>(value >> 8);
    out[pos + 3] = static_cast<std::uint8_t>(value);
}

/// Overwrite 8 bytes at `pos` (the caller has checked the range).
void patchU64(Bytes& out, std::size_t pos, std::uint64_t value) {
    patchU32(out, pos, static_cast<std::uint32_t>(value >> 32));
    patchU32(out, pos + 4, static_cast<std::uint32_t>(value));
}

/// Read 4 / 8 big-endian bytes at `pos` of a buffer (range checked by caller).
std::uint32_t loadU32(const Bytes& in, std::size_t pos) {
    return (static_cast<std::uint32_t>(in[pos]) << 24) | (static_cast<std::uint32_t>(in[pos + 1]) << 16) |
           (static_cast<std::uint32_t>(in[pos + 2]) << 8) | in[pos + 3];
}
std::uint64_t loadU64(const Bytes& in, std::size_t pos) {
    return (static_cast<std::uint64_t>(loadU32(in, pos)) << 32) | loadU32(in, pos + 4);
}

// ---------------------------------------------------------------------------
//  Box emission: a header with a placeholder size, patched once the body is
//  complete.  `large` keeps the 64-bit largesize form of the original box.
// ---------------------------------------------------------------------------

struct BoxMark {
    std::size_t start = 0;
    bool large = false;
};

BoxMark beginBox(Bytes& out, Fourcc type, bool large) {
    BoxMark mark{out.size(), large};
    if (large) {
        putU32(out, 1);  // size == 1: the 64-bit largesize follows the type
        putFourcc(out, type);
        putU64(out, 0);
    } else {
        putU32(out, 0);
        putFourcc(out, type);
    }
    return mark;
}

Status endBox(Bytes& out, const BoxMark& mark) {
    if (mark.start > out.size()) {
        return failStatus(ErrorCode::Internal, "spherical metadata: box mark past the end of the buffer");
    }
    const std::uint64_t size = out.size() - mark.start;
    if (mark.large) {
        patchU64(out, mark.start + 8, size);
        return okStatus();
    }
    // A 32-bit box cannot describe more than 4 GB; the moov cap keeps every
    // box far below that, so reaching this is a bug, not bad input.
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return failStatus(ErrorCode::Internal, "spherical metadata: rebuilt box exceeds 4 GB");
    }
    patchU32(out, mark.start, static_cast<std::uint32_t>(size));
    return okStatus();
}

// ---------------------------------------------------------------------------
//  The boxes this file writes
// ---------------------------------------------------------------------------

/// Spherical Video V2: 'st3d' then 'sv3d', 13 + 88 bytes.
///
///   st3d  FullBox(0, 0) { u8 stereo_mode = 0 (monoscopic) }
///   sv3d  Box {
///     svhd  FullBox(0, 0) { string metadata_source (NUL terminated) }
///     proj  Box {
///       prhd  FullBox(0, 0) { i32 yaw, pitch, roll, 16.16 degrees = 0 }
///       equi  FullBox(0, 0) { u32 bounds top, bottom, left, right, 0.32 = 0 }
///     }
///   }
Status appendV2Boxes(Bytes& out) {
    // ---- st3d: monoscopic ---------------------------------------------------
    const BoxMark st3d = beginBox(out, Fourcc{"st3d"}, false);
    putU32(out, 0);  // version 0, flags 0
    putU8(out, 0);   // stereo_mode 0: the frame holds one monoscopic view
    OSV_TRY(endBox(out, st3d));

    // ---- sv3d ------------------------------------------------------------------
    const BoxMark sv3d = beginBox(out, Fourcc{"sv3d"}, false);
    {
        // svhd: who wrote the metadata.
        const BoxMark svhd = beginBox(out, Fourcc{"svhd"}, false);
        putU32(out, 0);  // version 0, flags 0
        for (const char* c = kSphericalMetadataSource; *c != '\0'; ++c) {
            putU8(out, static_cast<std::uint8_t>(*c));
        }
        putU8(out, 0);  // metadata_source is NUL terminated
        OSV_TRY(endBox(out, svhd));

        // proj: the projection header (no pose) and the projection itself.
        const BoxMark proj = beginBox(out, Fourcc{"proj"}, false);
        const BoxMark prhd = beginBox(out, Fourcc{"prhd"}, false);
        putU32(out, 0);  // version 0, flags 0
        putU32(out, 0);  // pose_yaw_degrees   (16.16)
        putU32(out, 0);  // pose_pitch_degrees (16.16)
        putU32(out, 0);  // pose_roll_degrees  (16.16)
        OSV_TRY(endBox(out, prhd));
        const BoxMark equi = beginBox(out, Fourcc{"equi"}, false);
        putU32(out, 0);  // version 0, flags 0
        putU32(out, 0);  // projection_bounds_top    (0.32): uncropped
        putU32(out, 0);  // projection_bounds_bottom (0.32)
        putU32(out, 0);  // projection_bounds_left   (0.32)
        putU32(out, 0);  // projection_bounds_right  (0.32)
        OSV_TRY(endBox(out, equi));
        OSV_TRY(endBox(out, proj));
    }
    return endBox(out, sv3d);
}

/// Spherical Video V1: uuid ffcc8263-... { UTF-8 RDF/XML }, no terminator,
/// exactly as Google's injector lays it out.
Status appendV1Uuid(Bytes& out) {
    const BoxMark mark = beginBox(out, Fourcc{"uuid"}, false);
    putSpan(out, ByteSpan(kSphericalV1Uuid.data(), kSphericalV1Uuid.size()));
    const std::string xml = sphericalV1Xml();
    out.insert(out.end(), xml.begin(), xml.end());
    return endBox(out, mark);
}

/// True for the V1 'uuid' box this file writes (and Google's injector too).
bool isSphericalUuid(const BoxHeader& box) {
    return box.type == Fourcc{"uuid"} && box.hasUuid && box.uuid == kSphericalV1Uuid;
}

// ---------------------------------------------------------------------------
//  Strict child lists
// ---------------------------------------------------------------------------

/// Children of one container plus where its trailing zero padding starts
/// (== the container's end when it has none).
struct ChildList {
    std::vector<BoxHeader> boxes;
    std::uint64_t paddingStart = 0;
};

/// Every box in [start, end) of `root`.  Unlike BoxWalker::forEachChild,
/// which salvages what it can, this refuses: a header that does not parse,
/// a child running past its parent, or trailing bytes other than fewer than
/// eight zeros (the terminator some writers leave) make the file Malformed.
Result<ChildList> strictChildren(ByteSpan root, std::uint64_t start, std::uint64_t end, int depth,
                                 const std::string& where) {
    if (start > end || end > root.size()) {
        return Error{ErrorCode::Malformed, where + ": child range [" + std::to_string(start) + ", " +
                                               std::to_string(end) + ") lies outside the box"};
    }
    ChildList list;
    list.paddingStart = end;
    std::uint64_t pos = start;
    while (pos < end) {
        const std::uint64_t remaining = end - pos;
        // ---- a short tail: only zero padding is acceptable ------------------
        if (remaining < 8) {
            for (std::uint64_t i = 0; i < remaining; ++i) {
                if (root[pos + i] != 0) {
                    return Error{ErrorCode::Malformed, where + ": " + std::to_string(remaining) +
                                                           " stray byte(s) at offset " + std::to_string(pos)};
                }
            }
            list.paddingStart = pos;
            break;
        }
        // ---- one child header ---------------------------------------------------
        Result<BoxHeader> header = BoxWalker::readHeader(root, pos, end, depth);
        if (!header.ok()) {
            return Error{ErrorCode::Malformed, where + ": " + header.error().message};
        }
        const BoxHeader& box = header.value();
        if (box.truncated) {
            return Error{ErrorCode::Truncated, where + ": box " + box.describe() + " declares " +
                                                   std::to_string(box.declaredSize) + " bytes, past its parent"};
        }
        if (list.boxes.size() >= kMaxChildren) {
            return Error{ErrorCode::Malformed, where + ": more than " + std::to_string(kMaxChildren) + " children"};
        }
        list.boxes.push_back(box);
        // readHeader guarantees size >= headerSize >= 8: always progress.
        pos = box.end();
    }
    return list;
}

/// First child of `parent` (children in [parent.bodyOffset(), end)) with the
/// given type, taken strictly; nullopt when there is none.
Result<std::optional<BoxHeader>> strictChild(ByteSpan root, const BoxHeader& parent, Fourcc type) {
    OSV_TRY_ASSIGN(ChildList children, strictChildren(root, parent.bodyOffset(), parent.end(), parent.depth + 1,
                                                      "'" + parent.type.str() + "'"));
    for (const BoxHeader& child : children.boxes) {
        if (child.type == type) {
            return std::optional<BoxHeader>(child);
        }
    }
    return std::optional<BoxHeader>();
}

/// Follow `path` down from `parent` strictly; nullopt when a step is missing.
Result<std::optional<BoxHeader>> strictPath(ByteSpan root, const BoxHeader& parent,
                                            std::initializer_list<Fourcc> path) {
    std::optional<BoxHeader> current = parent;
    for (const Fourcc step : path) {
        OSV_TRY_ASSIGN(std::optional<BoxHeader> next, strictChild(root, *current, step));
        if (!next) {
            return std::optional<BoxHeader>();
        }
        current = std::move(next);
    }
    return current;
}

// ---------------------------------------------------------------------------
//  Rebuilding one container
// ---------------------------------------------------------------------------

/// What a rebuild callback did with a child.
enum class ChildAction {
    Copy,     ///< Nothing: copy the child byte for byte.
    Replaced  ///< The callback wrote its replacement (possibly nothing).
};

using ChildFn = std::function<Result<ChildAction>(const BoxHeader& child, Bytes& out)>;
using TailFn = std::function<Status(Bytes& out)>;

/// Copy one box unchanged.  A size-0 box ("to the end of the parent") gets
/// its real size written instead, because its parent may gain boxes after
/// it and a size-0 box would then swallow them.
void copyBox(const BoxHeader& box, Bytes& out) {
    if (!box.toEnd) {
        putSpan(out, box.whole);
        return;
    }
    // size 0 is only ever in the 32-bit field, so the header is 8 bytes and
    // everything after it (a uuid, the body) follows unchanged.
    const ByteSpan rest = box.whole.sub(8);
    if (box.size <= std::numeric_limits<std::uint32_t>::max()) {
        putU32(out, static_cast<std::uint32_t>(box.size));
        putFourcc(out, box.type);
    } else {
        putU32(out, 1);
        putFourcc(out, box.type);
        putU64(out, box.size + 8);  // the largesize field adds 8 header bytes
    }
    putSpan(out, rest);
}

/// Re-emit `box` into `out`: its header in the same form (largesize stays
/// largesize, a uuid keeps its extended type), the fixed bytes between the
/// header and `childStart` unchanged, then each child through `onChild`
/// (copied unless the callback replaced it), then whatever `onTail` appends,
/// then the original trailing padding.  The new size is written at the end.
Status rebuildContainer(ByteSpan root, const BoxHeader& box, std::uint64_t childStart, Bytes& out,
                        const ChildFn& onChild, const TailFn& onTail) {
    const std::string where = "'" + box.type.str() + "' @" + std::to_string(box.offset);
    if (childStart < box.bodyOffset() || childStart > box.end()) {
        return failStatus(ErrorCode::Malformed, where + " is too short for its fixed fields");
    }
    OSV_TRY_ASSIGN(ChildList children, strictChildren(root, childStart, box.end(), box.depth + 1, where));

    // ---- header and fixed fields -------------------------------------------
    const BoxMark mark = beginBox(out, box.type, box.largeSize);
    if (box.hasUuid) {
        putSpan(out, ByteSpan(box.uuid.data(), box.uuid.size()));
    }
    putSpan(out, root.sub(box.bodyOffset(), childStart - box.bodyOffset()));

    // ---- children ---------------------------------------------------------------
    for (const BoxHeader& child : children.boxes) {
        ChildAction action = ChildAction::Copy;
        if (onChild) {
            OSV_TRY_ASSIGN(action, onChild(child, out));
        }
        if (action == ChildAction::Copy) {
            copyBox(child, out);
        }
    }

    // ---- new boxes go before any trailing zero padding, which a reader
    //      would otherwise take for a size-0 box swallowing them ----------------
    if (onTail) {
        OSV_TRY(onTail(out));
    }
    putSpan(out, root.sub(children.paddingStart, box.end() - children.paddingStart));
    return endBox(out, mark);
}

// ---------------------------------------------------------------------------
//  The moov rebuild
// ---------------------------------------------------------------------------

/// The rebuilt 'moov' and what went into it.
struct RebuiltMoov {
    Bytes bytes;
    bool replaced = false;
    std::uint32_t trackId = 0;
    std::uint32_t entriesTagged = 0;
    std::string entryType;
};

/// Rebuilds one 'moov' box (the whole box, header included, in `moov`) with
/// the spherical tags on its first video track.
class MoovRebuilder {
public:
    explicit MoovRebuilder(ByteSpan moov)
        : m_moov(moov) {}

    Result<RebuiltMoov> run() {
        // ---- the moov header itself -----------------------------------------
        Result<BoxHeader> header = BoxWalker::readHeader(m_moov, 0, m_moov.size(), 0);
        if (!header.ok()) {
            return Error{ErrorCode::Malformed, "'moov': " + header.error().message};
        }
        const BoxHeader moov = header.value();
        if (moov.type != Fourcc{"moov"} || moov.truncated || moov.size != m_moov.size()) {
            return Error{ErrorCode::Malformed, "'moov' header does not describe the loaded box"};
        }
        OSV_TRY_ASSIGN(ChildList children,
                       strictChildren(m_moov, moov.bodyOffset(), moov.end(), moov.depth + 1, "'moov'"));

        // ---- the first video track --------------------------------------------
        std::optional<BoxHeader> target;
        for (const BoxHeader& child : children.boxes) {
            // Fragments carry their own sample offsets (tfhd / trun) that
            // this rewrite does not move.
            if (child.type == Fourcc{"mvex"}) {
                return Error{ErrorCode::Unsupported, "fragmented MP4 ('mvex') is not supported"};
            }
            if (!target && child.type == Fourcc{"trak"}) {
                OSV_TRY_ASSIGN(const Fourcc handler, handlerOf(child));
                if (handler == Fourcc{"vide"}) {
                    target = child;
                }
            }
        }
        if (!target) {
            return Error{ErrorCode::NotFound, "no video track (no 'trak' with a 'vide' handler)"};
        }
        OSV_TRY_ASSIGN(const std::uint32_t trackId, trackIdOf(*target));
        m_result.trackId = trackId;

        // ---- rebuild moov, descending into the target track only -------------
        m_result.bytes.reserve(m_moov.size() + 1024);
        const std::uint64_t targetOffset = target->offset;
        OSV_TRY(rebuildContainer(
            m_moov, moov, moov.bodyOffset(), m_result.bytes,
            [&](const BoxHeader& child, Bytes& out) -> Result<ChildAction> {
                if (child.offset != targetOffset) {
                    return ChildAction::Copy;
                }
                OSV_TRY(rebuildTrak(child, out));
                return ChildAction::Replaced;
            },
            nullptr));
        return std::move(m_result);
    }

private:
    /// handler_type of a 'trak' (mdia/hdlr), or a zero code when absent.
    Result<Fourcc> handlerOf(const BoxHeader& trak) const {
        OSV_TRY_ASSIGN(const std::optional<BoxHeader> hdlr, strictPath(m_moov, trak, {Fourcc{"mdia"}, Fourcc{"hdlr"}}));
        if (!hdlr) {
            return Fourcc{};
        }
        // hdlr: FullBox { u32 pre_defined; u32 handler_type; ... }
        ByteReader reader(hdlr->payload);
        std::uint32_t preDefined = 0;
        Fourcc handler;
        if (!reader.u32be(preDefined) || !reader.fourcc(handler)) {
            return Error{ErrorCode::Truncated, "'hdlr' " + hdlr->describe() + " is too short"};
        }
        return handler;
    }

    /// track_ID from a 'trak''s 'tkhd' (0 when it has none).
    Result<std::uint32_t> trackIdOf(const BoxHeader& trak) const {
        OSV_TRY_ASSIGN(const std::optional<BoxHeader> tkhd, strictChild(m_moov, trak, Fourcc{"tkhd"}));
        if (!tkhd) {
            return std::uint32_t{0};
        }
        // tkhd v0: creation(4) modification(4) track_ID(4); v1: 8, 8, 4.
        ByteReader reader(tkhd->payload);
        std::uint32_t id = 0;
        if (!reader.skip(tkhd->version == 1 ? 16 : 8) || !reader.u32be(id)) {
            return Error{ErrorCode::Truncated, "'tkhd' " + tkhd->describe() + " is too short"};
        }
        return id;
    }

    /// trak: drop an old V1 box, descend into mdia, append the new V1 box.
    Status rebuildTrak(const BoxHeader& trak, Bytes& out) {
        bool sawMdia = false;
        OSV_TRY(rebuildContainer(
            m_moov, trak, trak.bodyOffset(), out,
            [&](const BoxHeader& child, Bytes& o) -> Result<ChildAction> {
                if (isSphericalUuid(child)) {
                    m_result.replaced = true;  // an earlier V1 tag: replaced below
                    return ChildAction::Replaced;
                }
                if (!sawMdia && child.type == Fourcc{"mdia"}) {
                    sawMdia = true;
                    // mdia -> minf -> stbl -> stsd: each level is rebuilt,
                    // so each gets its new size on the way back up.
                    OSV_TRY(rebuildThrough(child, Fourcc{"minf"}, o, [&](const BoxHeader& minf, Bytes& o2) {
                        return rebuildThrough(minf, Fourcc{"stbl"}, o2, [&](const BoxHeader& stbl, Bytes& o3) {
                            return rebuildThrough(stbl, Fourcc{"stsd"}, o3, [&](const BoxHeader& stsd, Bytes& o4) {
                                return rebuildStsd(stsd, o4);
                            });
                        });
                    }));
                    return ChildAction::Replaced;
                }
                return ChildAction::Copy;
            },
            // The V1 box closes the track, where Google's injector puts it.
            [](Bytes& o) { return appendV1Uuid(o); }));
        if (!sawMdia) {
            return failStatus(ErrorCode::Malformed, "the video 'trak' has no 'mdia'");
        }
        return okStatus();
    }

    /// Rebuild a plain container, descending into its first child of type
    /// `next` with `inner` and copying everything else.
    Status rebuildThrough(const BoxHeader& box, Fourcc next, Bytes& out,
                          const std::function<Status(const BoxHeader&, Bytes&)>& inner) {
        bool found = false;
        OSV_TRY(rebuildContainer(
            m_moov, box, box.bodyOffset(), out,
            [&](const BoxHeader& child, Bytes& o) -> Result<ChildAction> {
                if (found || child.type != next) {
                    return ChildAction::Copy;
                }
                found = true;
                OSV_TRY(inner(child, o));
                return ChildAction::Replaced;
            },
            nullptr));
        if (!found) {
            return failStatus(ErrorCode::Malformed,
                              "the video track has no '" + next.str() + "' in its '" + box.type.str() + "'");
        }
        return okStatus();
    }

    /// stsd: FullBox { u32 entry_count; SampleEntry entries[] }.  Every
    /// declared entry is a visual sample entry of the video track and gets
    /// the V2 boxes; the entry count does not change.
    Status rebuildStsd(const BoxHeader& stsd, Bytes& out) {
        ByteReader reader(stsd.payload);
        std::uint32_t entryCount = 0;
        if (!stsd.isFull || !reader.u32be(entryCount)) {
            return failStatus(ErrorCode::Truncated, "'stsd' of the video track has no entry count");
        }
        std::uint32_t index = 0;
        OSV_TRY(rebuildContainer(
            m_moov, stsd, stsd.payloadOffset() + 4, out,
            [&](const BoxHeader& entry, Bytes& o) -> Result<ChildAction> {
                // Boxes past the declared count are not sample entries.
                if (index >= entryCount) {
                    return ChildAction::Copy;
                }
                ++index;
                OSV_TRY(rebuildEntry(entry, o));
                return ChildAction::Replaced;
            },
            nullptr));
        if (entryCount == 0 || index < entryCount) {
            return failStatus(ErrorCode::Malformed, "'stsd' of the video track declares " + std::to_string(entryCount) +
                                                        " entries but holds " + std::to_string(index));
        }
        return okStatus();
    }

    /// One visual sample entry: drop old st3d / sv3d, put the new pair
    /// before the optional clap / pasp / btrt boxes (or at the end).
    Status rebuildEntry(const BoxHeader& entry, Bytes& out) {
        if (entry.size < std::uint64_t{entry.headerSize} + kVisualEntryFixedBytes) {
            return failStatus(ErrorCode::Malformed,
                              "sample entry " + entry.describe() + " is shorter than a visual sample entry");
        }
        bool inserted = false;
        OSV_TRY(rebuildContainer(
            m_moov, entry, entry.bodyOffset() + kVisualEntryFixedBytes, out,
            [&](const BoxHeader& child, Bytes& o) -> Result<ChildAction> {
                if (child.type == Fourcc{"st3d"} || child.type == Fourcc{"sv3d"}) {
                    m_result.replaced = true;  // an earlier V2 tag: replaced by ours
                    return ChildAction::Replaced;
                }
                const bool optionalTail =
                    child.type == Fourcc{"clap"} || child.type == Fourcc{"pasp"} || child.type == Fourcc{"btrt"};
                if (!inserted && optionalTail) {
                    OSV_TRY(appendV2Boxes(o));
                    inserted = true;
                }
                return ChildAction::Copy;
            },
            [&](Bytes& o) {
                if (inserted) {
                    return okStatus();
                }
                inserted = true;
                return appendV2Boxes(o);
            }));
        ++m_result.entriesTagged;
        if (m_result.entryType.empty()) {
            m_result.entryType = entry.type.str();
        }
        return okStatus();
    }

    ByteSpan m_moov;
    RebuiltMoov m_result;
};

// ---------------------------------------------------------------------------
//  Chunk offsets
// ---------------------------------------------------------------------------

/// The part of the file the chunk offsets are judged against.
struct OffsetFrame {
    std::uint64_t moovStart = 0;  ///< Old 'moov' offset in the file.
    std::uint64_t moovEnd = 0;    ///< One past the old 'moov'.
    std::int64_t delta = 0;       ///< Bytes the 'moov' grew by.
    std::uint64_t fileSize = 0;   ///< Size of the source file.
};

/// Move the entries of one stco / co64 table that point past the old moov.
Result<std::uint64_t> shiftTable(Bytes& moov, const BoxHeader& table, const OffsetFrame& frame) {
    const bool wide = table.type == Fourcc{"co64"};
    const std::uint64_t entryBytes = wide ? 8 : 4;
    ByteReader reader(table.payload);
    std::uint32_t count = 0;
    if (!table.isFull || !reader.u32be(count)) {
        return Error{ErrorCode::Truncated, "chunk offset table " + table.describe() + " has no entry count"};
    }
    if (static_cast<std::uint64_t>(count) * entryBytes > reader.remaining()) {
        return Error{ErrorCode::Malformed, "chunk offset table " + table.describe() + " declares " +
                                               std::to_string(count) + " entries that do not fit in it"};
    }
    const std::uint64_t first = table.payloadOffset() + 4;
    std::uint64_t shifted = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t pos = static_cast<std::size_t>(first + i * entryBytes);
        const std::uint64_t old = wide ? loadU64(moov, pos) : loadU32(moov, pos);
        // A chunk past the end of the file means the file is cut short.
        if (old > frame.fileSize) {
            return Error{ErrorCode::Truncated, "chunk offset " + std::to_string(old) +
                                                   " lies past the end of the file (" + std::to_string(frame.fileSize) +
                                                   " bytes)"};
        }
        // Media data inside 'moov' is not something this rewrite can place.
        if (old >= frame.moovStart && old < frame.moovEnd) {
            return Error{ErrorCode::Malformed, "chunk offset " + std::to_string(old) + " points inside 'moov'"};
        }
        // Data before 'moov' does not move.
        if (old < frame.moovEnd || frame.delta == 0) {
            continue;
        }
        const std::int64_t moved = static_cast<std::int64_t>(old) + frame.delta;
        if (moved < 0) {
            return Error{ErrorCode::Internal,
                         "chunk offset " + std::to_string(old) + " would move before the file start"};
        }
        if (!wide && static_cast<std::uint64_t>(moved) > std::numeric_limits<std::uint32_t>::max()) {
            return Error{ErrorCode::Unsupported,
                         "chunk offset " + std::to_string(old) + " would pass 4 GB in a 32-bit 'stco' table"};
        }
        if (wide) {
            patchU64(moov, pos, static_cast<std::uint64_t>(moved));
        } else {
            patchU32(moov, pos, static_cast<std::uint32_t>(moved));
        }
        ++shifted;
    }
    return shifted;
}

/// Walk every track of the rebuilt moov and shift its chunk offsets.  Also
/// the one place every track's sample table is looked at, so the tables
/// this rewrite cannot keep consistent are refused here.
Result<std::uint64_t> shiftChunkOffsets(Bytes& moov, const OffsetFrame& frame) {
    const ByteSpan span(moov);
    Result<BoxHeader> header = BoxWalker::readHeader(span, 0, span.size(), 0);
    if (!header.ok() || header.value().truncated) {
        return Error{ErrorCode::Internal, "rebuilt 'moov' does not parse"};
    }
    const BoxHeader& root = header.value();
    OSV_TRY_ASSIGN(ChildList tracks, strictChildren(span, root.bodyOffset(), root.end(), 1, "'moov'"));
    std::uint64_t shifted = 0;
    for (const BoxHeader& trak : tracks.boxes) {
        if (trak.type != Fourcc{"trak"}) {
            continue;
        }
        OSV_TRY_ASSIGN(const std::optional<BoxHeader> stbl,
                       strictPath(span, trak, {Fourcc{"mdia"}, Fourcc{"minf"}, Fourcc{"stbl"}}));
        if (!stbl) {
            continue;  // no sample table, no chunks
        }
        OSV_TRY_ASSIGN(ChildList tables,
                       strictChildren(span, stbl->bodyOffset(), stbl->end(), stbl->depth + 1, "'stbl'"));
        for (const BoxHeader& table : tables.boxes) {
            // saio holds absolute file offsets of auxiliary data (encrypted
            // content); moving them is not implemented, so refuse cleanly.
            if (table.type == Fourcc{"saio"}) {
                return Error{ErrorCode::Unsupported, "auxiliary sample offsets ('saio') are not supported"};
            }
            if (table.type != Fourcc{"stco"} && table.type != Fourcc{"co64"}) {
                continue;
            }
            OSV_TRY_ASSIGN(const std::uint64_t n, shiftTable(moov, table, frame));
            shifted += n;
        }
    }
    return shifted;
}

// ---------------------------------------------------------------------------
//  File access
// ---------------------------------------------------------------------------

/// Read exactly `count` bytes at `offset`.
bool readAt(std::ifstream& in, std::uint64_t offset, std::uint8_t* dst, std::size_t count) {
    if (!dst) {
        return false;
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        return false;
    }
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(count));
    return in.gcount() == static_cast<std::streamsize>(count);
}

/// One top-level box as found on disk.
struct TopBox {
    Fourcc type;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

/// Walk the top-level headers of an open file.  Every size is checked
/// against the bytes that remain; a box running past the end of the file
/// (an unfinished recording, a cut-off copy) is refused.
Result<std::vector<TopBox>> scanTopLevel(std::ifstream& in, std::uint64_t fileSize) {
    std::vector<TopBox> boxes;
    std::uint64_t pos = 0;
    while (pos < fileSize) {
        const std::uint64_t remaining = fileSize - pos;
        // ---- a few stray bytes at the end are copied as they are ------------
        if (remaining < 8) {
            if (boxes.empty()) {
                return Error{ErrorCode::Malformed, "file is too small to be an MP4 / MOV"};
            }
            break;
        }
        // ---- size(4) type(4) [largesize(8)] ----------------------------------------
        std::uint8_t head[16] = {};
        const std::size_t want = remaining >= 16 ? 16 : 8;
        if (!readAt(in, pos, head, want)) {
            return Error{ErrorCode::Io, "read failed at offset " + std::to_string(pos)};
        }
        ByteReader reader(ByteSpan(head, want));
        std::uint32_t size32 = 0;
        Fourcc type;
        reader.u32be(size32);
        reader.fourcc(type);
        std::uint64_t size = size32;
        std::uint64_t headerSize = 8;
        if (size32 == 1) {
            if (!reader.u64be(size)) {
                return Error{ErrorCode::Truncated,
                             "box '" + type.str() + "' at " + std::to_string(pos) + " ends inside its largesize"};
            }
            headerSize = 16;
        } else if (size32 == 0) {
            size = remaining;  // extends to the end of the file
        }
        // ---- the first box decides whether this is an ISO BMFF file at all ---------
        if (boxes.empty() && !BoxWalker::isKnownTopLevelType(type)) {
            return Error{ErrorCode::Malformed, "not an MP4 / MOV file (it starts with '" + type.str() + "')"};
        }
        if (size < headerSize) {
            return Error{ErrorCode::Malformed, "box '" + type.str() + "' at " + std::to_string(pos) + " has size " +
                                                   std::to_string(size) + ", smaller than its header"};
        }
        if (size > remaining) {
            return Error{ErrorCode::Truncated, "box '" + type.str() + "' at " + std::to_string(pos) + " declares " +
                                                   std::to_string(size) + " bytes but the file has only " +
                                                   std::to_string(remaining) + " left (cut short?)"};
        }
        if (boxes.size() >= kMaxTopLevelBoxes) {
            return Error{ErrorCode::Malformed, "more than " + std::to_string(kMaxTopLevelBoxes) + " top-level boxes"};
        }
        boxes.push_back(TopBox{type, pos, size});
        pos += size;
    }
    if (boxes.empty()) {
        return Error{ErrorCode::Malformed, "file is empty"};
    }
    return boxes;
}

/// Stream [begin, end) of `in` to `out` in kCopyChunkBytes blocks.
Status copyRange(std::ifstream& in, std::ofstream& out, std::uint64_t begin, std::uint64_t end, Bytes& buffer) {
    if (begin > end) {
        return failStatus(ErrorCode::Internal, "copy range is reversed");
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(begin), std::ios::beg);
    if (!in) {
        return failStatus(ErrorCode::Io, "seek failed at offset " + std::to_string(begin));
    }
    std::uint64_t remaining = end - begin;
    while (remaining > 0) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(n));
        if (in.gcount() != static_cast<std::streamsize>(n)) {
            return failStatus(ErrorCode::Io, "short read at offset " + std::to_string(end - remaining));
        }
        out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(n));
        if (!out) {
            return failStatus(ErrorCode::Io, "write failed (disk full?)");
        }
        remaining -= n;
    }
    return okStatus();
}

/// Write the new file: everything before the old moov, the new moov,
/// everything after the old moov.
Status writeRewritten(std::ifstream& in, std::uint64_t fileSize, const OffsetFrame& frame, const Bytes& moov,
                      const std::filesystem::path& temp) {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return failStatus(ErrorCode::Io, "cannot create " + temp.string());
    }
    Bytes buffer(kCopyChunkBytes);
    OSV_TRY(copyRange(in, out, 0, frame.moovStart, buffer));
    out.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    if (!out) {
        return failStatus(ErrorCode::Io, "write failed (disk full?)");
    }
    OSV_TRY(copyRange(in, out, frame.moovEnd, fileSize, buffer));
    out.flush();
    out.close();
    if (!out) {
        return failStatus(ErrorCode::Io, "closing " + temp.string() + " failed (disk full?)");
    }
    return okStatus();
}

/// Move `temp` over `target` in one step (the target is replaced whole or
/// not at all).
Status replaceFile(const std::filesystem::path& temp, const std::filesystem::path& target) {
#if defined(_WIN32)
    // MOVEFILE_WRITE_THROUGH: return only once the rename is on disk.
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        return failStatus(ErrorCode::Io, "cannot replace " + target.string() + ": " +
                                             std::system_category().message(static_cast<int>(code)) +
                                             " (is it open in another program?)");
    }
    return okStatus();
#else
    // POSIX rename() replaces the target atomically.
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        return failStatus(ErrorCode::Io, "cannot replace " + target.string() + ": " + ec.message());
    }
    return okStatus();
#endif
}

/// Remove a leftover temporary file; failures are only logged.
void removeQuietly(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
        log::warn("spherical metadata: could not remove {}: {}", log::safe(path.string()), ec.message());
    }
}

/// The body of injectSphericalMetadata (which only adds the exception fence).
Result<SphericalInjectReport> inject(const std::filesystem::path& input, const std::filesystem::path& output) {
    // ---- the paths --------------------------------------------------------------
    if (input.empty()) {
        return Error{ErrorCode::InvalidArgument, "spherical metadata: no input file"};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(input, ec) || ec) {
        return Error{ErrorCode::Io, "file not found: " + input.string()};
    }
    // In place when no output is given or it names the input itself.
    bool inPlace = output.empty();
    if (!inPlace) {
        std::error_code eq;
        inPlace = std::filesystem::exists(output, eq) && std::filesystem::equivalent(input, output, eq) && !eq;
    }
    const std::filesystem::path target = inPlace ? input : output;
    const std::uint64_t fileSize = std::filesystem::file_size(input, ec);
    if (ec) {
        return Error{ErrorCode::Io, "cannot read the size of " + input.string() + ": " + ec.message()};
    }

    // ---- 1. top level -------------------------------------------------------------
    std::ifstream in(input, std::ios::binary);
    if (!in) {
        return Error{ErrorCode::Io, "cannot open " + input.string()};
    }
    OSV_TRY_ASSIGN(const std::vector<TopBox> top, scanTopLevel(in, fileSize));
    const TopBox* moovBox = nullptr;
    for (const TopBox& box : top) {
        if (box.type == Fourcc{"moof"}) {
            return Error{ErrorCode::Unsupported, "fragmented MP4 ('moof') is not supported"};
        }
        if (box.type == Fourcc{"moov"}) {
            if (moovBox) {
                return Error{ErrorCode::Malformed, "the file has more than one 'moov' box"};
            }
            moovBox = &box;
        }
    }
    if (!moovBox) {
        return Error{ErrorCode::NotFound, "no 'moov' box (an unfinished recording?)"};
    }
    if (moovBox->size > kMaxSphericalMoovBytes) {
        return Error{ErrorCode::Unsupported, "'moov' is " + std::to_string(moovBox->size) + " bytes, above the " +
                                                 std::to_string(kMaxSphericalMoovBytes) + "-byte limit"};
    }
    SphericalInjectReport report;
    for (const TopBox& box : top) {
        if (box.type == Fourcc{"mdat"} && box.offset > moovBox->offset) {
            report.moovBeforeMdat = true;
        }
    }

    // ---- 2. rebuild moov ------------------------------------------------------------
    Bytes oldMoov(static_cast<std::size_t>(moovBox->size));
    if (!readAt(in, moovBox->offset, oldMoov.data(), oldMoov.size())) {
        return Error{ErrorCode::Io, "cannot read 'moov' at offset " + std::to_string(moovBox->offset)};
    }
    OSV_TRY_ASSIGN(RebuiltMoov rebuilt, MoovRebuilder(ByteSpan(oldMoov)).run());

    // ---- 3. chunk offsets -------------------------------------------------------------
    OffsetFrame frame;
    frame.moovStart = moovBox->offset;
    frame.moovEnd = moovBox->offset + moovBox->size;
    frame.delta = static_cast<std::int64_t>(rebuilt.bytes.size()) - static_cast<std::int64_t>(oldMoov.size());
    frame.fileSize = fileSize;
    OSV_TRY_ASSIGN(const std::uint64_t shifted, shiftChunkOffsets(rebuilt.bytes, frame));

    report.changed = rebuilt.bytes != oldMoov;
    report.replacedExisting = rebuilt.replaced;
    report.moovGrowth = frame.delta;
    report.chunkOffsetsShifted = shifted;
    report.videoTrackId = rebuilt.trackId;
    report.sampleEntriesTagged = rebuilt.entriesTagged;
    report.sampleEntryType = rebuilt.entryType;

    // ---- already tagged, in place: nothing to write ------------------------------------
    if (!report.changed && inPlace) {
        log::debug("spherical metadata: {} already carries the tags", log::safe(input.string()));
        return report;
    }

    // ---- 4. the temporary file beside the target ---------------------------------------
    std::filesystem::path temp = target;
    temp += ".osvtmp";
    const Status written = writeRewritten(in, fileSize, frame, rebuilt.bytes, temp);
    in.close();  // the source must be closed before an in-place replace
    if (!written.ok()) {
        removeQuietly(temp);
        return written.error();
    }
    // Check what landed on disk before it replaces anything: the expected
    // size, and a top level that still walks cleanly to the end.
    const std::uint64_t expected = static_cast<std::uint64_t>(static_cast<std::int64_t>(fileSize) + frame.delta);
    const std::uint64_t got = std::filesystem::file_size(temp, ec);
    if (ec || got != expected) {
        removeQuietly(temp);
        return Error{ErrorCode::Io,
                     "the rewritten file has " + std::to_string(got) + " bytes, expected " + std::to_string(expected)};
    }
    {
        std::ifstream check(temp, std::ios::binary);
        std::string problem;
        if (!check) {
            problem = "it cannot be reopened";
        } else {
            const Result<std::vector<TopBox>> rescanned = scanTopLevel(check, got);
            if (!rescanned.ok()) {
                problem = rescanned.error().message;
            } else if (rescanned.value().size() != top.size()) {
                problem = "its top-level box count differs";
            }
        }
        check.close();  // closed before the file is removed or moved
        if (!problem.empty()) {
            removeQuietly(temp);
            return Error{ErrorCode::Internal, "the rewritten file does not walk like its source: " + problem};
        }
    }

    // ---- 5. replace -----------------------------------------------------------------------
    const Status replaced = replaceFile(temp, target);
    if (!replaced.ok()) {
        removeQuietly(temp);
        return replaced.error();
    }
    log::debug("spherical metadata: {} tagged (track {}, {} entr{}, moov {:+} bytes, {} chunk offsets moved)",
               log::safe(target.string()), report.videoTrackId, report.sampleEntriesTagged,
               report.sampleEntriesTagged == 1 ? "y" : "ies", report.moovGrowth, report.chunkOffsetsShifted);
    return report;
}

}  // namespace

std::string sphericalV1Xml() {
    // Google's SPHERICAL_XML_HEADER + SPHERICAL_XML_CONTENTS + FOOTER, with
    // our name as the stitching software.
    std::string xml;
    xml += "<?xml version=\"1.0\"?>";
    xml += "<rdf:SphericalVideo\n";
    xml += "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"\n";
    xml += "xmlns:GSpherical=\"http://ns.google.com/videos/1.0/spherical/\">";
    xml += "<GSpherical:Spherical>true</GSpherical:Spherical>";
    xml += "<GSpherical:Stitched>true</GSpherical:Stitched>";
    xml += "<GSpherical:StitchingSoftware>";
    xml += kSphericalMetadataSource;
    xml += "</GSpherical:StitchingSoftware>";
    xml += "<GSpherical:ProjectionType>equirectangular</GSpherical:ProjectionType>";
    xml += "</rdf:SphericalVideo>";
    return xml;
}

Result<SphericalInjectReport> injectSphericalMetadata(const std::filesystem::path& input,
                                                      const std::filesystem::path& output) {
    // The library runs inside host applications: an allocation failure or a
    // filesystem exception must come back as an Error, never escape.
    try {
        return inject(input, output);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "spherical metadata: out of memory"};
    } catch (const std::exception& e) {
        return Error{ErrorCode::Internal, std::string("spherical metadata: ") + e.what()};
    }
}

}  // namespace osv::io
