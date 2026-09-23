// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ContainerSource: adapts osv::OsvFile / TrackInfo / SampleTable to the small
// interface Decoder.cpp needs.  Sample bytes are sliced from the file mapping
// the OsvFile already owns, using the offsets and sizes the sample table
// reports, so nothing is copied until a packet is built.

#include "ContainerSource.h"

#include "FileIdentity.h"
#include "osv/container/OsvFile.h"
#include "osv/core/Log.h"

#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace osv::video::detail {

namespace {

// -----------------------------------------------------------------------------
//  The shared-parse table
// -----------------------------------------------------------------------------
//
// Weak references only: a parsed movie lives exactly as long as some
// ContainerSource uses it, so the table never pins a mapping of a file the
// application has finished with, and its own destruction releases nothing.
struct ParseTable {
    std::mutex mutex;
    std::map<std::wstring, std::weak_ptr<const OsvFile>> movies;
};

ParseTable& parseTable() {
    static ParseTable instance;
    return instance;
}

/// The parsed movie of `path`: the live one when another source holds it,
/// otherwise a fresh parse that is published for the next caller.  The parse
/// runs under the table lock on purpose - the two lens decoders of a reader
/// open at the same moment, and without the lock both would parse.
Result<std::shared_ptr<const OsvFile>> sharedParse(const std::filesystem::path& path, bool* reused) {
    if (reused) {
        *reused = false;
    }
    // A file that cannot even be stat'ed is not cached; OsvFile::open then
    // reports the real reason (Io) exactly as it always did.
    auto identity = fileIdentity(path);
    if (!identity.ok()) {
        OSV_TRY_ASSIGN(OsvFile parsed, OsvFile::open(path));
        return std::make_shared<const OsvFile>(std::move(parsed));
    }
    ParseTable& table = parseTable();
    std::lock_guard<std::mutex> guard(table.mutex);
    const auto it = table.movies.find(identity.value());
    if (it != table.movies.end()) {
        if (std::shared_ptr<const OsvFile> live = it->second.lock()) {
            if (reused) {
                *reused = true;
            }
            return live;
        }
    }
    OSV_TRY_ASSIGN(OsvFile parsed, OsvFile::open(path));
    auto movie = std::make_shared<const OsvFile>(std::move(parsed));
    table.movies[identity.value()] = movie;
    // Sweep dead entries so a long session that touches many clips does not
    // keep one key per clip it ever opened.
    for (auto e = table.movies.begin(); e != table.movies.end();) {
        e = e->second.expired() ? table.movies.erase(e) : std::next(e);
    }
    return movie;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Impl
// -----------------------------------------------------------------------------
struct ContainerSource::Impl {
    std::shared_ptr<const OsvFile> file;   ///< Parsed movie (owns the mapping every span aliases).
    const TrackInfo* track = nullptr;      ///< Selected track (points into `*file`).
    std::uint32_t trackId = 0;
    std::uint32_t sampleCount = 0;
    std::uint32_t timescale = 0;
    double fps = 0.0;
    TrackCodec codec = TrackCodec::Unknown;
    bool reusedParse = false;              ///< The movie came from a live user.
};

ContainerSource::ContainerSource() = default;
ContainerSource::~ContainerSource() = default;

// -----------------------------------------------------------------------------
//  open
// -----------------------------------------------------------------------------
Result<std::unique_ptr<ContainerSource>> ContainerSource::open(const std::filesystem::path& path,
                                                               std::uint32_t trackId) {
    // Parse the container first (or share a live parse); a file that is not
    // an ISO BMFF movie fails here with the parser's own error code
    // (Malformed / Truncated / Io).
    bool reused = false;
    OSV_TRY_ASSIGN(std::shared_ptr<const OsvFile> parsed, sharedParse(path, &reused));
    if (!parsed) {
        return Error{ErrorCode::Internal, "container: parse produced no movie"};
    }

    std::unique_ptr<ContainerSource> self(new ContainerSource());
    self->m_impl = std::make_unique<Impl>();
    Impl& impl = *self->m_impl;
    impl.file = std::move(parsed);
    impl.trackId = trackId;
    impl.reusedParse = reused;

    // Select the track.  The pointer is owned by `*impl.file`, which lives as
    // long as this object, so it is safe to keep.
    impl.track = impl.file->track(trackId);
    if (!impl.track) {
        return Error{ErrorCode::NotFound, "container: no track with id " + std::to_string(trackId)};
    }
    if (!impl.track->isVideo()) {
        return Error{ErrorCode::NotFound, "container: track " + std::to_string(trackId) + " is not a video track (" +
                                              std::string(trackKindName(impl.track->kind)) + ")"};
    }
    impl.sampleCount = impl.track->samples.count();
    if (impl.sampleCount == 0) {
        return Error{ErrorCode::NotFound, "container: track " + std::to_string(trackId) + " has no samples"};
    }
    impl.timescale = impl.track->timescale;

    // Codec according to the configuration record in the sample entry.  The
    // Osmo 360 writes hvc1 + hvcC for the native streams and avc1 + avcC for
    // the LRF proxy; anything else is decodable only through libavformat.
    if (impl.track->hevc()) {
        impl.codec = TrackCodec::Hevc;
    } else if (impl.track->avc()) {
        impl.codec = TrackCodec::Avc;
    } else {
        impl.codec = TrackCodec::Unknown;
    }

    // Derive the frame rate from the decode timestamps of the first and last
    // sample: fps = timescale * (count - 1) / (dts_last - dts_first).  This is
    // exactly what stts encodes and is immune to the edit-list / average
    // rounding that libavformat applies.  A single-sample track falls back to
    // the stts total-duration estimate the container module provides.
    if (impl.sampleCount >= 2 && impl.timescale > 0) {
        auto first = impl.track->samples.locate(0);
        auto last = impl.track->samples.locate(impl.sampleCount - 1);
        if (first.ok() && last.ok()) {
            const std::int64_t span = static_cast<std::int64_t>(last.value().dts) -
                                      static_cast<std::int64_t>(first.value().dts);
            if (span > 0) {
                impl.fps = static_cast<double>(impl.timescale) * static_cast<double>(impl.sampleCount - 1) /
                           static_cast<double>(span);
            }
        }
    }
    if (impl.fps <= 0.0) {
        impl.fps = impl.track->frameRate();
    }
    if (impl.fps <= 0.0) {
        log::warn("container: could not derive fps for track {} (single sample or flat timing)", trackId);
    }
    return self;
}

// -----------------------------------------------------------------------------
//  Accessors
// -----------------------------------------------------------------------------
bool ContainerSource::reusedParse() const noexcept { return m_impl != nullptr && m_impl->reusedParse; }

std::uint32_t ContainerSource::trackId() const noexcept { return m_impl ? m_impl->trackId : 0; }

std::uint32_t ContainerSource::sampleCount() const noexcept { return m_impl ? m_impl->sampleCount : 0; }

std::uint32_t ContainerSource::previousSync(std::uint32_t index) const noexcept {
    if (!m_impl || !m_impl->track || m_impl->sampleCount == 0) {
        return 0;
    }
    // Clamp so a request past the end resolves to the last GOP.
    if (index >= m_impl->sampleCount) {
        index = m_impl->sampleCount - 1;
    }
    const std::uint32_t sync = m_impl->track->samples.previousSync(index);
    // Defensive: a sync index after the request would make the decoder loop
    // forever waiting for a frame it never sees.
    return sync <= index ? sync : 0;
}

Result<SampleView> ContainerSource::sample(std::uint32_t index) const {
    if (!m_impl || !m_impl->track) {
        return Error{ErrorCode::InvalidArgument, "container source not open"};
    }
    if (index >= m_impl->sampleCount) {
        return Error{ErrorCode::InvalidArgument, "sample index " + std::to_string(index) + " out of range (" +
                                                     std::to_string(m_impl->sampleCount) + ")"};
    }
    OSV_TRY_ASSIGN(const SampleLocation loc, m_impl->track->samples.locate(index));

    // The sample must lie entirely inside the file; a truncated recording
    // (camera lost power) is reported instead of read past the mapping.
    if (!m_impl->file) {
        return Error{ErrorCode::InvalidArgument, "container source has no movie"};
    }
    const ByteSpan file = m_impl->file->span();
    if (!file.contains(loc.offset, loc.size)) {
        return Error{ErrorCode::Truncated, "sample " + std::to_string(index) + " of track " +
                                               std::to_string(m_impl->trackId) + " lies outside the file"};
    }
    SampleView view;
    view.bytes = file.sub(loc.offset, loc.size);
    view.dts = static_cast<std::int64_t>(loc.dts);
    view.ctsOffset = loc.ctsOffset;
    view.sync = loc.sync;
    return view;
}

std::uint32_t ContainerSource::timescale() const noexcept { return m_impl ? m_impl->timescale : 0; }

double ContainerSource::fps() const noexcept { return m_impl ? m_impl->fps : 0.0; }

std::uint32_t ContainerSource::codedWidth() const noexcept {
    return (m_impl && m_impl->track) ? m_impl->track->codedWidth() : 0;
}

std::uint32_t ContainerSource::codedHeight() const noexcept {
    return (m_impl && m_impl->track) ? m_impl->track->codedHeight() : 0;
}

TrackCodec ContainerSource::codec() const noexcept { return m_impl ? m_impl->codec : TrackCodec::Unknown; }

std::uint8_t ContainerSource::bitDepth() const noexcept {
    if (!m_impl || !m_impl->track) {
        return 8;
    }
    // Only hvcC records the bit depth; avcC (LRF) streams are always 8-bit.
    const HevcConfig* hevc = m_impl->track->hevc();
    if (!hevc) {
        return 8;
    }
    const std::uint32_t depth = hevc->bitDepthLuma();
    // 8..16 is the legal range of bit_depth_luma_minus8 + 8; clamp garbage.
    return static_cast<std::uint8_t>(depth >= 8 && depth <= 16 ? depth : 8);
}

std::vector<std::uint8_t> ContainerSource::annexBHeader() const {
    if (!m_impl || !m_impl->track) {
        return {};
    }
    // Both records know how to emit their parameter sets with start codes.
    if (const HevcConfig* hevc = m_impl->track->hevc()) {
        return hevc->toAnnexBHeader();
    }
    if (const AvcConfig* avc = m_impl->track->avc()) {
        return avc->toAnnexBHeader();
    }
    return {};
}

std::uint32_t ContainerSource::nalLengthSize() const noexcept {
    if (!m_impl || !m_impl->track) {
        return 0;
    }
    std::uint32_t size = 0;
    if (const HevcConfig* hevc = m_impl->track->hevc()) {
        size = hevc->nalLengthSize();
    } else if (const AvcConfig* avc = m_impl->track->avc()) {
        size = avc->nalLengthSize();
    } else {
        return 0;
    }
    // lengthSizeMinusOne is a 2-bit field; anything else means a corrupt
    // record, and 4 is the only value the Osmo 360 writes.
    return (size >= 1 && size <= 4) ? size : 4;
}

}  // namespace osv::video::detail
