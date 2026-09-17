// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MetadataTrack implementation: primary-track selection, eager sample-0
// decode, the lazily filled frame cache and the lens <-> video track mapping.

#include "osv/meta/MetadataTrack.h"

#include "osv/core/Log.h"
#include "osv/meta/DjmdDecoder.h"

#include <algorithm>
#include <atomic>
#include <format>

namespace osv::meta {

// -----------------------------------------------------------------------------
//  Construction / special members (out of line because of unique_ptr<Cache>)
// -----------------------------------------------------------------------------

MetadataTrack::MetadataTrack() = default;
MetadataTrack::~MetadataTrack() = default;
MetadataTrack::MetadataTrack(MetadataTrack&&) noexcept = default;
MetadataTrack& MetadataTrack::operator=(MetadataTrack&&) noexcept = default;

// -----------------------------------------------------------------------------
//  Accessors with safe defaults
// -----------------------------------------------------------------------------

const ClipMeta& MetadataTrack::clip() const noexcept {
    if (m_clip.has_value()) {
        return *m_clip;
    }
    // A shared default keeps callers that skipped hasClip() out of trouble.
    static const ClipMeta kEmpty{};
    return kEmpty;
}

const StreamMeta& MetadataTrack::stream() const noexcept {
    if (m_stream.has_value()) {
        return *m_stream;
    }
    static const StreamMeta kEmpty{};
    return kEmpty;
}

bool MetadataTrack::hasCalibration() const noexcept {
    if (!m_stream.has_value()) {
        return false;
    }
    // Any slot with a usable record counts.
    for (std::uint32_t slot = 1; slot < PanoDewarpParams::SlotCount; ++slot) {
        const DewarpParams* rec = m_stream->dewarp.get(slot);
        if (rec && rec->hasCore()) {
            return true;
        }
    }
    return false;
}

bool MetadataTrack::isPrimaryCandidate(const ProductMeta& product) noexcept {
    if (!product.clip.has_value() || !product.stream.has_value()) {
        return false;
    }
    for (std::uint32_t slot = 1; slot < PanoDewarpParams::SlotCount; ++slot) {
        const DewarpParams* rec = product.stream->dewarp.get(slot);
        if (rec && rec->hasCore()) {
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
//  Loading
// -----------------------------------------------------------------------------

Result<MetadataTrack> MetadataTrack::load(const OsvFile& file, std::optional<std::uint32_t> trackId) {
    const std::vector<const TrackInfo*> djmd = file.tracksOfKind(TrackKind::Djmd);
    if (djmd.empty()) {
        return Error{ErrorCode::NotFound, "file has no djmd metadata track"};
    }

    // ---- choose the track --------------------------------------------------
    const TrackInfo* chosen = nullptr;
    ProductMeta first;
    std::vector<std::string> selectionWarnings;

    if (trackId.has_value()) {
        // Explicit request: it must exist and be a djmd track.
        const TrackInfo* t = file.track(*trackId);
        if (!t) {
            return Error{ErrorCode::NotFound, std::format("track {} does not exist", *trackId)};
        }
        if (t->kind != TrackKind::Djmd) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("track {} is not a djmd metadata track ({})", *trackId, trackKindName(t->kind))};
        }
        if (t->samples.count() == 0) {
            return Error{ErrorCode::NotFound, std::format("djmd track {} has no samples", *trackId)};
        }
        OSV_TRY_ASSIGN(ByteSpan sample0, file.sample(t->trackId, 0));
        OSV_TRY_ASSIGN(first, DjmdDecoder::decode(sample0));
        chosen = t;
    } else {
        // Default: the first djmd track whose sample 0 carries clip + stream
        // + calibration.  Tracks that fail to decode are skipped with a note.
        for (const TrackInfo* t : djmd) {
            if (!t || t->samples.count() == 0) {
                continue;
            }
            auto sample0 = file.sample(t->trackId, 0);
            if (!sample0.ok()) {
                selectionWarnings.push_back(
                    std::format("djmd track {}: sample 0 unreadable: {}", t->trackId, sample0.error().toString()));
                continue;
            }
            auto product = DjmdDecoder::decode(sample0.value());
            if (!product.ok()) {
                selectionWarnings.push_back(
                    std::format("djmd track {}: sample 0 undecodable: {}", t->trackId, product.error().toString()));
                continue;
            }
            if (isPrimaryCandidate(product.value())) {
                chosen = t;
                first = std::move(product).value();
                break;
            }
        }
        if (!chosen) {
            // Nothing carries calibration: fall back to the first readable
            // djmd track so frame metadata (timestamps, attitude) still works.
            for (const TrackInfo* t : djmd) {
                if (!t || t->samples.count() == 0) {
                    continue;
                }
                auto sample0 = file.sample(t->trackId, 0);
                if (!sample0.ok()) {
                    continue;
                }
                auto product = DjmdDecoder::decode(sample0.value());
                if (!product.ok()) {
                    continue;
                }
                chosen = t;
                first = std::move(product).value();
                selectionWarnings.push_back(std::format(
                    "no djmd track carries calibration; using track {} (frame metadata only)", t->trackId));
                break;
            }
        }
        if (!chosen) {
            return Error{ErrorCode::Malformed, "no djmd track has a decodable sample 0"};
        }
    }

    // ---- build the object ----------------------------------------------------
    MetadataTrack track;
    track.m_file = &file;
    track.m_trackId = chosen->trackId;
    track.m_frameCount = chosen->samples.count();
    track.m_clip = std::move(first.clip);
    track.m_stream = std::move(first.stream);
    track.m_warnings = std::move(selectionWarnings);
    for (std::string& w : first.warnings) {
        track.m_warnings.push_back(std::move(w));
    }
    track.m_cache = std::make_unique<Cache>();
    track.m_cache->frames.resize(track.m_frameCount);
    // Sample 0 is already decoded: seed the cache with its FrameMeta.
    if (first.frame.has_value() && track.m_frameCount > 0) {
        track.m_cache->frames[0] = std::move(first.frame);
    }

    log::debug("meta: loaded djmd track {} ({} samples, clip={}, stream={}, calibration={})", track.m_trackId,
               track.m_frameCount, track.hasClip(), track.hasStream(), track.hasCalibration());
    return track;
}

// -----------------------------------------------------------------------------
//  Frames
// -----------------------------------------------------------------------------

Result<FrameMeta> MetadataTrack::decodeFrame(std::uint32_t index) const {
    if (!m_file) {
        return Error{ErrorCode::Internal, "MetadataTrack is not loaded"};
    }
    if (index >= m_frameCount) {
        return Error{ErrorCode::NotFound, std::format("frame {} out of range (track has {} samples)", index, m_frameCount)};
    }
    OSV_TRY_ASSIGN(ByteSpan sample, m_file->sample(m_trackId, index));
    OSV_TRY_ASSIGN(ProductMeta product, DjmdDecoder::decode(sample));
    if (!product.frame.has_value()) {
        return Error{ErrorCode::Malformed, std::format("djmd sample {} holds no FrameMeta", index)};
    }
    return std::move(*product.frame);
}

Result<FrameMeta> MetadataTrack::frame(std::uint32_t index) const {
    if (!m_cache) {
        return Error{ErrorCode::Internal, "MetadataTrack is not loaded"};
    }
    if (index >= m_frameCount) {
        return Error{ErrorCode::NotFound, std::format("frame {} out of range (track has {} samples)", index, m_frameCount)};
    }
    // Fast path: already cached.
    {
        std::lock_guard<std::mutex> lock(m_cache->mutex);
        if (index < m_cache->frames.size() && m_cache->frames[index].has_value()) {
            return *m_cache->frames[index];
        }
    }
    // Decode outside the lock so concurrent callers do not serialise on the
    // (cheap but non-trivial) protobuf walk.
    OSV_TRY_ASSIGN(FrameMeta decoded, decodeFrame(index));
    {
        std::lock_guard<std::mutex> lock(m_cache->mutex);
        if (index < m_cache->frames.size()) {
            // Another thread may have filled the slot meanwhile: keep theirs,
            // the content is identical.
            if (!m_cache->frames[index].has_value()) {
                m_cache->frames[index] = decoded;
            }
        }
    }
    return decoded;
}

Status MetadataTrack::prefetchAll(ThreadPool* pool) const {
    if (!m_cache || !m_file) {
        return failStatus(ErrorCode::Internal, "MetadataTrack is not loaded");
    }
    if (m_frameCount == 0) {
        return okStatus();
    }
    std::atomic<std::uint32_t> failures{0};
    // Each worker decodes a run of samples and stores them under the lock.
    const auto body = [this, &failures](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            const std::uint32_t index = static_cast<std::uint32_t>(i);
            {
                std::lock_guard<std::mutex> lock(m_cache->mutex);
                if (m_cache->frames[index].has_value()) {
                    continue;
                }
            }
            auto decoded = decodeFrame(index);
            if (!decoded.ok()) {
                failures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::lock_guard<std::mutex> lock(m_cache->mutex);
            if (!m_cache->frames[index].has_value()) {
                m_cache->frames[index] = std::move(decoded).value();
            }
        }
    };
    if (pool) {
        // Grain of 32 samples keeps the per-chunk overhead negligible while
        // still spreading a long clip over every worker.
        OSV_TRY(pool->parallelFor(0, m_frameCount, 32, body));
    } else {
        body(0, m_frameCount);
    }
    const std::uint32_t failed = failures.load();
    if (failed != 0) {
        return failStatus(ErrorCode::Malformed, std::format("{} of {} djmd samples failed to decode", failed, m_frameCount));
    }
    return okStatus();
}

// -----------------------------------------------------------------------------
//  Lens <-> video track mapping
// -----------------------------------------------------------------------------

Result<MetadataTrack::LensStreamOrder> MetadataTrack::lensStreamOrderFor(const OsvFile& file, const StreamMeta* stream) {
    std::vector<const TrackInfo*> video = file.tracksOfKind(TrackKind::Video);
    // Drop null entries defensively and order by track_ID.
    video.erase(std::remove(video.begin(), video.end(), nullptr), video.end());
    std::sort(video.begin(), video.end(),
              [](const TrackInfo* a, const TrackInfo* b) { return a->trackId < b->trackId; });
    if (video.empty()) {
        return Error{ErrorCode::NotFound, "file has no video track"};
    }

    LensStreamOrder order;
    std::string metaNote;
    if (stream) {
        metaNote = std::format("; StreamMeta reports stream id {} (\"{}\")", stream->id, log::safe(stream->name));
    }

    // Rule 1: a single side-by-side track.
    if (video.size() == 1) {
        const TrackInfo* t = video.front();
        const std::uint32_t w = t->codedWidth() ? t->codedWidth() : static_cast<std::uint32_t>(t->width);
        const std::uint32_t h = t->codedHeight() ? t->codedHeight() : static_cast<std::uint32_t>(t->height);
        if (w != 0 && h != 0 && w == 2 * h) {
            order.slaveTrackId = t->trackId;
            order.masterTrackId = t->trackId;
            order.sideBySide = true;
            order.source = std::format("single {}x{} side-by-side track {}: left half = slave (stream 0), right half = master (stream 1){}",
                                       w, h, t->trackId, metaNote);
            return order;
        }
        // One lens only: nothing to pair.  Report it as slave == master with
        // the fallback flag so callers can still address the track.
        order.slaveTrackId = t->trackId;
        order.masterTrackId = t->trackId;
        order.fromFallback = true;
        order.source = std::format("single video track {} ({}x{}) is not side-by-side; treating it as both lenses{}",
                                   t->trackId, w, h, metaNote);
        return order;
    }

    // Rules 2/3: the two lowest track_IDs are the lens streams.
    const TrackInfo* a = video[0];
    const TrackInfo* b = video[1];
    if (a->enabled() != b->enabled()) {
        // Exactly one carries the tkhd enabled flag: that is stream 0.
        const TrackInfo* slave = a->enabled() ? a : b;
        const TrackInfo* master = a->enabled() ? b : a;
        order.slaveTrackId = slave->trackId;
        order.masterTrackId = master->trackId;
        order.source = std::format("tkhd enabled flag: track {} (flags 0x{:x}) = slave, track {} (flags 0x{:x}) = master{}",
                                   slave->trackId, slave->tkhdFlags, master->trackId, master->tkhdFlags, metaNote);
    } else {
        order.slaveTrackId = a->trackId;
        order.masterTrackId = b->trackId;
        order.fromFallback = true;
        order.source = std::format("fallback (both tracks have tkhd flags 0x{:x}): lowest track {} = slave, track {} = master{}",
                                   a->tkhdFlags, a->trackId, b->trackId, metaNote);
    }
    if (video.size() > 2) {
        order.source += std::format("; {} additional video track(s) ignored", video.size() - 2);
    }
    return order;
}

Result<MetadataTrack::LensStreamOrder> MetadataTrack::lensStreamOrder() const {
    if (!m_file) {
        return Error{ErrorCode::Internal, "MetadataTrack is not loaded"};
    }
    return lensStreamOrderFor(*m_file, m_stream.has_value() ? &*m_stream : nullptr);
}

}  // namespace osv::meta
