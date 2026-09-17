// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Small box parsers and accessors that belong to TrackInfo: 'colr', esds
// helpers, track classification and derived timing.

#include "osv/container/TrackInfo.h"

#include "osv/core/ByteReader.h"

namespace osv {

// -----------------------------------------------------------------------------
//  ColrNclx
// -----------------------------------------------------------------------------

Result<ColrNclx> ColrNclx::parse(ByteSpan payload) {
    ColrNclx colr;
    colr.raw = payload;
    ByteReader reader(payload);
    if (!reader.fourcc(colr.colourType)) {
        return Error{ErrorCode::Truncated, "colr box has no colour_type"};
    }
    // 'nclx' (ISO) has three u16 code points and a full-range bit; 'nclc'
    // (QuickTime) has the three code points only.  ICC forms keep `raw`.
    if (colr.colourType == Fourcc{"nclx"} || colr.colourType == Fourcc{"nclc"}) {
        if (!reader.u16be(colr.primaries) || !reader.u16be(colr.transfer) || !reader.u16be(colr.matrix)) {
            return Error{ErrorCode::Truncated, "colr '" + colr.colourType.str() + "' box ends inside its code points"};
        }
        if (colr.colourType == Fourcc{"nclx"}) {
            std::uint8_t rangeByte = 0;
            // The full-range flag is optional in older writers; missing = 0.
            if (reader.u8(rangeByte)) {
                colr.fullRange = (rangeByte & 0x80u) != 0;
            }
        }
    }
    return colr;
}

// -----------------------------------------------------------------------------
//  EsdsInfo
// -----------------------------------------------------------------------------

std::uint8_t EsdsInfo::audioObjectType() const noexcept {
    // AudioSpecificConfig: audioObjectType is the first 5 bits; 31 escapes
    // to a 6-bit extension (32 + next 6 bits).
    if (decoderSpecificInfo.empty()) {
        return 0;
    }
    const std::uint8_t first = decoderSpecificInfo[0];
    const std::uint8_t type = static_cast<std::uint8_t>(first >> 3);
    if (type != 31) {
        return type;
    }
    if (decoderSpecificInfo.size() < 2) {
        return type;
    }
    const std::uint16_t bits = static_cast<std::uint16_t>((static_cast<std::uint16_t>(first) << 8) | decoderSpecificInfo[1]);
    // Skip the 5 escape bits, take the next 6.
    return static_cast<std::uint8_t>(32 + ((bits >> 5) & 0x3Fu));
}

// -----------------------------------------------------------------------------
//  TrackInfo
// -----------------------------------------------------------------------------

std::int64_t TrackInfo::editMediaTime() const noexcept {
    // The first edit that maps to media (mediaTime >= 0) tells where the
    // presentation starts in the media timeline.
    for (const EditListEntry& edit : editList) {
        if (edit.mediaTime >= 0) {
            return edit.mediaTime;
        }
    }
    return 0;
}

double TrackInfo::frameRate() const noexcept {
    const std::uint64_t total = samples.totalDuration();
    if (total == 0 || timescale == 0 || samples.count() == 0) {
        return 0.0;
    }
    return static_cast<double>(samples.count()) * static_cast<double>(timescale) / static_cast<double>(total);
}

double TrackInfo::durationSeconds() const noexcept {
    if (timescale == 0) {
        return 0.0;
    }
    return static_cast<double>(duration) / static_cast<double>(timescale);
}

TrackKind classifyTrack(Fourcc handler, Fourcc sampleEntry) noexcept {
    // DJI's private tracks are identified by their sample entry, whatever
    // handler they carry ('meta' in practice).
    if (sampleEntry == Fourcc{"djmd"}) {
        return TrackKind::Djmd;
    }
    if (sampleEntry == Fourcc{"dbgi"}) {
        return TrackKind::Dbgi;
    }
    if (handler == Fourcc{"vide"}) {
        return TrackKind::Video;
    }
    if (handler == Fourcc{"soun"}) {
        return TrackKind::Audio;
    }
    return TrackKind::Other;
}

}  // namespace osv
