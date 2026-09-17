// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MetadataTrack: the decoded view of one 'djmd' track of an OsvFile.
//
// Sample 0 (ClipMeta + StreamMeta + first FrameMeta) is decoded eagerly when
// the track is loaded because everything else (format detection, calibration
// selection) depends on it.  The remaining per-frame samples are decoded on
// first access and cached; prefetchAll() fills the cache in parallel for
// callers that will iterate the whole clip (IMU export, stabilisation).
//
// Lifetime: the track keeps a pointer to the OsvFile it was loaded from
// (samples alias the file mapping), so the OsvFile must outlive it.
#pragma once

#include "osv/container/OsvFile.h"
#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"
#include "osv/meta/Types.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace osv::meta {

class MetadataTrack {
public:
    /// Which video track carries which lens.
    ///
    /// The camera does not label the video tracks; the mapping is inferred
    /// (see lensStreamOrder()) and the inference is recorded in `source` so
    /// probe output can show how the decision was made.
    struct LensStreamOrder {
        std::uint32_t slaveTrackId = 0;   ///< Stream 0, lens looking along -Y (body).
        std::uint32_t masterTrackId = 0;  ///< Stream 1, lens looking along +Y (body).
        bool sideBySide = false;          ///< One track holds both lenses (LRF proxy: left = slave, right = master).
        bool fromFallback = false;        ///< True when the documented default order had to be assumed.
        std::string source;               ///< Human readable explanation of the decision.
    };

    MetadataTrack();
    ~MetadataTrack();
    MetadataTrack(MetadataTrack&&) noexcept;
    MetadataTrack& operator=(MetadataTrack&&) noexcept;
    MetadataTrack(const MetadataTrack&) = delete;
    MetadataTrack& operator=(const MetadataTrack&) = delete;

    /// Load a djmd track.  Without `trackId` the primary metadata track is
    /// chosen: the first 'djmd' track whose sample 0 decodes with a ClipMeta,
    /// a StreamMeta and at least one non-empty calibration slot (track 4 on
    /// the Osmo 360); when none qualifies the first 'djmd' track is used and
    /// a warning is recorded.  With `trackId` that track is loaded as-is
    /// (NotFound when it does not exist or is not a djmd track).
    [[nodiscard]] static Result<MetadataTrack> load(const OsvFile& file, std::optional<std::uint32_t> trackId = {});

    /// True when sample 0 carried a ClipMeta.
    [[nodiscard]] bool hasClip() const noexcept { return m_clip.has_value(); }
    /// The clip metadata (a default-constructed ClipMeta when !hasClip()).
    [[nodiscard]] const ClipMeta& clip() const noexcept;

    /// True when sample 0 carried a StreamMeta.
    [[nodiscard]] bool hasStream() const noexcept { return m_stream.has_value(); }
    /// The stream metadata (a default-constructed StreamMeta when !hasStream()).
    [[nodiscard]] const StreamMeta& stream() const noexcept;

    /// True when stream() holds at least one usable calibration record.
    [[nodiscard]] bool hasCalibration() const noexcept;

    /// The djmd track this object was loaded from.
    [[nodiscard]] std::uint32_t trackId() const noexcept { return m_trackId; }

    /// Number of metadata samples (== number of video frames on the OSV).
    [[nodiscard]] std::uint32_t frameCount() const noexcept { return m_frameCount; }

    /// Decoded FrameMeta of sample `index`, decoded on first use and cached.
    /// NotFound for an out-of-range index, Malformed when the sample holds no
    /// FrameMeta.
    [[nodiscard]] Result<FrameMeta> frame(std::uint32_t index) const;

    /// Decode every frame into the cache, on `pool` when given (each worker
    /// decodes a run of samples) or sequentially otherwise.  Returns
    /// Malformed with a count when some samples failed; the cache still holds
    /// the ones that succeeded.
    Status prefetchAll(ThreadPool* pool = nullptr) const;

    /// Non-fatal problems found while decoding sample 0.
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return m_warnings; }

    /// The file this track was loaded from (never null after a successful load).
    [[nodiscard]] const OsvFile* file() const noexcept { return m_file; }

    /// Map lenses to video tracks.
    ///
    /// Rules, in order:
    ///  1. A single video track twice as wide as it is high is a side-by-side
    ///     proxy (LRF): left half = slave (stream 0), right half = master.
    ///  2. With two or more video tracks the two lowest track_IDs are the
    ///     lenses.  If exactly one of them has the tkhd `enabled` flag (bit 0)
    ///     it is the slave (the camera writes flags 0x3 on the stream-0 track
    ///     and 0x2 on the other); the other is the master.
    ///  3. Otherwise the documented fallback applies: the lowest track_ID
    ///     (track 1) is the slave, the next (track 2) the master, and
    ///     `fromFallback` is set.
    /// The StreamMeta / FrameMeta stream ids are cross-checked and mentioned
    /// in `source` (both djmd tracks of the Osmo 360 report stream 0, so they
    /// cannot by themselves separate the lenses).
    [[nodiscard]] Result<LensStreamOrder> lensStreamOrder() const;

    /// Same rules applied to an arbitrary file, with an optional StreamMeta
    /// for the cross-check (used by FormatDetector when no track is loaded).
    [[nodiscard]] static Result<LensStreamOrder> lensStreamOrderFor(const OsvFile& file, const StreamMeta* stream);

    /// True when `product` qualifies as the primary metadata sample: ClipMeta
    /// and StreamMeta present and at least one non-empty calibration slot.
    [[nodiscard]] static bool isPrimaryCandidate(const ProductMeta& product) noexcept;

private:
    /// Decode sample `index` without touching the cache.
    [[nodiscard]] Result<FrameMeta> decodeFrame(std::uint32_t index) const;

    /// Lazily filled per-frame cache guarded by its own mutex so a const
    /// MetadataTrack can be shared between decoder threads.
    struct Cache {
        std::mutex mutex;
        std::vector<std::optional<FrameMeta>> frames;
    };

    const OsvFile* m_file = nullptr;
    std::uint32_t m_trackId = 0;
    std::uint32_t m_frameCount = 0;
    std::optional<ClipMeta> m_clip;
    std::optional<StreamMeta> m_stream;
    std::vector<std::string> m_warnings;
    std::unique_ptr<Cache> m_cache;
};

}  // namespace osv::meta
