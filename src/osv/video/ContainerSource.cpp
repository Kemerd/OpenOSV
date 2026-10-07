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

#include <algorithm>
#include <cmath>
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

// -----------------------------------------------------------------------------
//  Random access points, read from the samples themselves
// -----------------------------------------------------------------------------
//
// The sync table (stss) says where a decode may start, and on every camera
// file it is right - except on one lens of a recording that dropped frames:
// the camera writes ONE sync table for both lens tracks, and after a two-frame
// gap one lens's encoder can place its IDR pictures a sample later than the
// other's (seen on a night drive: from the gap on, every listed sync sample of
// the second lens is a trailing picture whose references precede it).  A
// decode started there drops that picture, and the request fails.  So a listed
// sync sample is checked against its own first picture NAL before it is used.

/// How far back (in samples) a decode start is searched for when the listed
/// sync sample does not start one: several GOPs of any camera mode.
constexpr std::uint32_t kMaxDecodeStartScan = 1024;

/// Whether a sample can start a decode, judged from its first picture NAL.
enum class DecodeStart : std::uint8_t {
    Yes,     ///< An IRAP picture (HEVC) / IDR or I slice (AVC).
    No,      ///< A picture that needs earlier ones.
    Unknown  ///< Nothing to judge by (no picture NAL found, unknown codec).
};

/// Read an unsigned Exp-Golomb value at bit `bit` of `data` (MSB first),
/// advancing `bit`; false when the code runs past `size` bytes or is longer
/// than 31 bits.  Emulation-prevention bytes are not removed: the two AVC
/// fields read with it sit in the first bytes of the slice header, before
/// any 00 00 03 a real encoder could have written.
[[nodiscard]] bool readUe(const std::uint8_t* data, std::size_t size, std::size_t& bit, std::uint32_t& value) noexcept {
    const auto readBit = [&](std::uint32_t& out) {
        if (bit >= size * 8u) {
            return false;
        }
        out = (data[bit / 8u] >> (7u - bit % 8u)) & 1u;
        ++bit;
        return true;
    };
    // Leading zeros, then as many value bits.
    std::uint32_t zeros = 0;
    std::uint32_t b = 0;
    for (;;) {
        if (!readBit(b)) {
            return false;
        }
        if (b == 1u) {
            break;
        }
        if (++zeros > 31u) {
            return false;
        }
    }
    std::uint32_t suffix = 0;
    for (std::uint32_t i = 0; i < zeros; ++i) {
        if (!readBit(b)) {
            return false;
        }
        suffix = (suffix << 1) | b;
    }
    value = ((1u << zeros) - 1u) + suffix;
    return true;
}

/// Whether `bytes` (one length-prefixed sample) starts a decode.
[[nodiscard]] DecodeStart decodeStartOf(ByteSpan bytes, std::uint32_t lengthSize, TrackCodec codec) noexcept {
    if (lengthSize < 1 || lengthSize > 4 || codec == TrackCodec::Unknown) {
        return DecodeStart::Unknown;
    }
    // Walk the NAL units up to the first picture (VCL) one; parameter sets,
    // SEI and delimiters before it say nothing.
    std::size_t pos = 0;
    const std::size_t size = bytes.size();
    while (pos + lengthSize < size) {
        std::uint32_t len = 0;
        for (std::uint32_t i = 0; i < lengthSize; ++i) {
            len = (len << 8) | bytes[pos + i];
        }
        pos += lengthSize;
        if (len == 0 || len > size - pos) {
            return DecodeStart::Unknown;  // a damaged sample: let the decoder report it
        }
        const std::uint8_t* nal = bytes.data() + pos;
        if (codec == TrackCodec::Hevc) {
            // nal_unit_type: 6 bits after the forbidden zero bit; < 32 are
            // pictures, 16..23 the random access (IRAP) ones.
            const std::uint32_t type = (nal[0] >> 1) & 0x3Fu;
            if (type < 32u) {
                return (type >= 16u && type <= 23u) ? DecodeStart::Yes : DecodeStart::No;
            }
        } else {
            // AVC: 5 bits.  An IDR starts a decode; a non-IDR slice does when
            // it is intra (slice_type 2 / 7 = I, 4 / 9 = SI).
            const std::uint32_t type = nal[0] & 0x1Fu;
            if (type == 5u) {
                return DecodeStart::Yes;
            }
            if (type == 1u) {
                std::size_t bit = 0;
                std::uint32_t firstMb = 0;
                std::uint32_t sliceType = 0;
                if (!readUe(nal + 1, len - 1u, bit, firstMb) || !readUe(nal + 1, len - 1u, bit, sliceType)) {
                    return DecodeStart::Unknown;
                }
                const std::uint32_t kind = sliceType % 5u;
                return (kind == 2u || kind == 4u) ? DecodeStart::Yes : DecodeStart::No;
            }
            if (type >= 2u && type <= 4u) {
                return DecodeStart::Unknown;  // data partitions: not judged
            }
        }
        pos += len;
    }
    return DecodeStart::Unknown;
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
    /// The sample table can index frames (see indexedByTable()); decided
    /// once at open, the table is immutable afterwards.
    bool indexedByTable = false;
    /// Presentation time of sample 0 (dts + cts), the origin of every
    /// relative time below.
    std::int64_t firstPresentation = 0;
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

    // The sample table as the frame index (see indexedByTable()): only for a
    // track without composition offsets whose samples are presented in
    // strictly increasing order - every camera file.  The origin is sample
    // 0's presentation time, which is also where both decoder modes put
    // their startPts on such a file.
    const SampleTable& table = impl.track->samples;
    impl.indexedByTable = impl.timescale > 0 && !table.hasCompositionOffsets() && table.dtsStrictlyIncreasing();
    impl.firstPresentation = static_cast<std::int64_t>(table.sampleDts(0)) + table.compositionOffset(0);
    if (!impl.indexedByTable) {
        log::debug("container: track {} is indexed by its average frame rate ({})", trackId,
                   table.hasCompositionOffsets() ? "composition offsets present" : "sample times do not increase");
    }

    // Derive the AVERAGE frame rate from the decode timestamps of the first
    // and last sample: fps = timescale * (count - 1) / (dts_last - dts_first).
    // On a constant-rate track this is exactly the stts rate and immune to
    // the edit-list / average rounding that libavformat applies; on a
    // variable-rate one it is informational only (the decoder indexes by
    // the table above).  A single-sample track falls back to the stts
    // total-duration estimate the container module provides.
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

std::uint32_t ContainerSource::listedSync(std::uint32_t index) const noexcept {
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

std::uint32_t ContainerSource::previousSync(std::uint32_t index) const noexcept {
    const std::uint32_t listed = listedSync(index);
    if (!m_impl || !m_impl->track || m_impl->sampleCount == 0) {
        return listed;
    }
    try {
        // ---- the listed sync sample, when it really starts a decode -------------
        // Every camera file's sync table is right for the first lens, and for
        // both until a recording drops frames: one header check, same answer.
        const TrackCodec codec = m_impl->codec;
        const std::uint32_t lengthSize = nalLengthSize();
        const auto startsAt = [&](std::uint32_t i) {
            auto view = sample(i);
            return view.ok() ? decodeStartOf(view.value().bytes, lengthSize, codec) : DecodeStart::Unknown;
        };
        if (startsAt(listed) != DecodeStart::No) {
            return listed;
        }
        // ---- otherwise the nearest sample at or before `index` that does ---------
        // (often one sample AFTER the listed one, when an encoder put its IDR
        // a sample late; a request before that IDR goes back a GOP).
        const std::uint32_t last = std::min(index, m_impl->sampleCount - 1u);
        const std::uint32_t floor = last > kMaxDecodeStartScan ? last - kMaxDecodeStartScan : 0u;
        for (std::uint32_t i = last + 1u; i-- > floor;) {
            if (startsAt(i) == DecodeStart::Yes) {
                log::debug("container: track {}: listed sync sample {} cannot start a decode; sample {} does "
                           "(request {})",
                           m_impl->trackId, listed, i, index);
                return i;
            }
        }
        log::debug("container: track {}: no decode start found within {} samples before {}; using the listed {}",
                   m_impl->trackId, kMaxDecodeStartScan, index, listed);
    } catch (...) {
        // Allocation failure while reading the headers: the listed answer.
    }
    return listed;
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

bool ContainerSource::indexedByTable() const noexcept {
    return m_impl != nullptr && m_impl->track != nullptr && m_impl->indexedByTable;
}

std::int64_t ContainerSource::presentationTicks(std::uint32_t index) const noexcept {
    if (!m_impl || !m_impl->track) {
        return 0;
    }
    // dts + cts of the sample, relative to sample 0's; sampleDts extrapolates
    // past the table with the last delta, so an index one past the end is
    // where that sample would have been.
    const SampleTable& table = m_impl->track->samples;
    const std::int64_t presentation =
        static_cast<std::int64_t>(table.sampleDts(index)) + table.compositionOffset(index);
    return presentation - m_impl->firstPresentation;
}

std::uint64_t ContainerSource::sampleDuration(std::uint32_t index) const noexcept {
    if (!m_impl || !m_impl->track) {
        return 0;
    }
    return m_impl->track->samples.sampleDuration(index);
}

std::int64_t ContainerSource::sampleAt(std::int64_t relTicks) const noexcept {
    if (!m_impl || !m_impl->track || m_impl->sampleCount == 0) {
        return 0;
    }
    const SampleTable& table = m_impl->track->samples;
    const std::uint32_t last = m_impl->sampleCount - 1u;

    // ---- before sample 0: extrapolate with its duration ------------------------
    // round(relTicks / d0), as the average-rate formula rounded there: a
    // pre-roll frame a whole duration early is index -1 and is skipped, one
    // a fraction of a duration early still rounds to sample 0.
    if (relTicks < 0) {
        const std::uint64_t d0 = table.sampleDuration(0);
        if (d0 == 0) {
            return 0;
        }
        return static_cast<std::int64_t>(std::llround(static_cast<double>(relTicks) / static_cast<double>(d0)));
    }

    // ---- inside the table: the two samples around the time ---------------------
    // `below` is presented at or before the time; the next one after it.  The
    // nearer one wins and a tie goes to the later sample, which is llround's
    // half-away-from-zero on a constant-rate track.
    const std::int64_t absoluteSigned = relTicks + m_impl->firstPresentation;
    const std::uint64_t absolute = absoluteSigned > 0 ? static_cast<std::uint64_t>(absoluteSigned) : 0u;
    const std::uint32_t below = table.sampleAtOrBeforeDts(absolute);
    if (below >= last) {
        // ---- at or after the last sample: extrapolate with its duration --------
        const std::int64_t lastTicks = presentationTicks(last);
        const std::uint64_t dl = table.sampleDuration(last);
        if (relTicks <= lastTicks || dl == 0) {
            return static_cast<std::int64_t>(last);
        }
        const double beyond = static_cast<double>(relTicks - lastTicks) / static_cast<double>(dl);
        return static_cast<std::int64_t>(last) + static_cast<std::int64_t>(std::llround(beyond));
    }
    const std::int64_t belowTicks = presentationTicks(below);
    const std::int64_t aboveTicks = presentationTicks(below + 1u);
    // 2 * (t - below) >= (above - below)  <=>  t is at least half way across.
    if (2 * (relTicks - belowTicks) >= aboveTicks - belowTicks) {
        return static_cast<std::int64_t>(below) + 1;
    }
    return static_cast<std::int64_t>(below);
}

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
