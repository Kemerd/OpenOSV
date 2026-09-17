// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MovieInfo accessors and sample fetching.  The parser itself lives in
// MovieParser.cpp.

#include "osv/container/MovieInfo.h"

namespace osv {

// -----------------------------------------------------------------------------
//  IlstItem / UserData
// -----------------------------------------------------------------------------

std::string IlstItem::asString() const {
    // Type 1 is UTF-8 text; type 0 ("implicit") is text for the well-known
    // string items as well.  Binary types are not converted.
    if (dataType != 0 && dataType != 1) {
        return std::string{};
    }
    std::string text = data.toString();
    // Strip trailing NULs some writers pad with.
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

const IlstItem* UserData::ilstItem(Fourcc name) const noexcept {
    for (const IlstItem& item : ilst) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

const XtraEntry* UserData::xtraEntry(const std::string& name) const noexcept {
    for (const XtraEntry& entry : xtra) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
//  MovieInfo
// -----------------------------------------------------------------------------

double MovieInfo::durationSeconds() const noexcept {
    if (header.timescale == 0) {
        return 0.0;
    }
    return static_cast<double>(header.duration) / static_cast<double>(header.timescale);
}

const TrackInfo* MovieInfo::track(std::uint32_t trackId) const noexcept {
    for (const TrackInfo& t : tracks) {
        if (t.trackId == trackId) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<const TrackInfo*> MovieInfo::tracksOfKind(TrackKind kind) const {
    std::vector<const TrackInfo*> out;
    for (const TrackInfo& t : tracks) {
        if (t.kind == kind) {
            out.push_back(&t);
        }
    }
    return out;
}

ByteSpan MovieInfo::camdSpan() const noexcept {
    if (!camdBox) {
        return ByteSpan{};
    }
    return camdBox->body;
}

const BoxHeader* MovieInfo::topLevelBox(Fourcc type) const noexcept {
    for (const BoxHeader& box : topLevel) {
        if (box.type == type) {
            return &box;
        }
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
//  Sample access
// -----------------------------------------------------------------------------

Result<ByteSpan> readSample(ByteSpan source, const TrackInfo& track, std::uint32_t index) {
    OSV_TRY_ASSIGN(const SampleLocation loc, track.samples.locate(index));
    // The sample must lie entirely inside the source; a truncated file will
    // have offsets past the end for its last samples.
    if (!source.contains(loc.offset, loc.size)) {
        return Error{ErrorCode::Truncated, "sample " + std::to_string(index) + " of track " +
                                               std::to_string(track.trackId) + " at " + std::to_string(loc.offset) +
                                               "+" + std::to_string(loc.size) + " lies outside the " +
                                               std::to_string(source.size()) + "-byte source"};
    }
    return source.sub(loc.offset, loc.size);
}

Result<ByteSpan> readSample(const MovieInfo& movie, std::uint32_t trackId, std::uint32_t index) {
    const TrackInfo* track = movie.track(trackId);
    if (!track) {
        return Error{ErrorCode::NotFound, "no track with id " + std::to_string(trackId)};
    }
    return readSample(movie.source, *track, index);
}

}  // namespace osv
