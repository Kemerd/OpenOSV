// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SampleTable: the per-track sample map built from the 'stbl' children
// (stsz/stz2, stsc, stco/co64, stts, stss, ctts).
//
// The class answers "where is sample i, how big is it, when is it decoded and
// which keyframe precedes it" in O(1) / O(log n).  Sample offsets are expanded
// once at parse time (12 bytes per sample) because the chunk-to-sample walk is
// where every naive MP4 reader goes quadratic on long recordings.  Time and
// composition tables stay run-length encoded and are binary searched.
#pragma once

#include "osv/container/Box.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <vector>

namespace osv {

/// Everything needed to fetch and time one sample.
struct SampleLocation {
    std::uint64_t offset = 0;     ///< Absolute byte offset in the root span.
    std::uint32_t size = 0;       ///< Size in bytes.
    std::uint64_t dts = 0;        ///< Decode timestamp in media timescale units.
    std::uint64_t duration = 0;   ///< Sample duration in media timescale units.
    std::int64_t ctsOffset = 0;   ///< Composition offset (ctts), 0 without a ctts box.
    bool sync = false;            ///< True when the sample is a random access point.
    std::uint32_t chunkIndex = 0; ///< 0-based chunk that holds the sample.

    /// Presentation timestamp (dts + composition offset), never negative.
    [[nodiscard]] std::uint64_t pts() const noexcept {
        const std::int64_t v = static_cast<std::int64_t>(dts) + ctsOffset;
        return v < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(v);
    }
};

class SampleTable {
public:
    SampleTable() = default;

    /// Build the table from the children of an 'stbl' box.  Fails when one of
    /// the mandatory tables (stsz, stsc, stco/co64, stts) is missing or does
    /// not fit in its box.  Inconsistencies that can be tolerated (chunk runs
    /// covering fewer samples than stsz, stts covering more) become warnings.
    [[nodiscard]] static Result<SampleTable> parse(ByteSpan root, const BoxHeader& stbl, WarningList* warnings);

    /// Number of samples that can be located.
    [[nodiscard]] std::uint32_t count() const noexcept { return m_count; }

    /// True when the track has no samples.
    [[nodiscard]] bool empty() const noexcept { return m_count == 0; }

    /// Full location of sample `index` (0-based).  NotFound when out of range.
    [[nodiscard]] Result<SampleLocation> locate(std::uint32_t index) const;

    /// Byte offset of sample `index`, or 0 when out of range.
    [[nodiscard]] std::uint64_t sampleOffset(std::uint32_t index) const noexcept;

    /// Byte size of sample `index`, or 0 when out of range.
    [[nodiscard]] std::uint32_t sampleSize(std::uint32_t index) const noexcept;

    /// Decode timestamp of sample `index` in media timescale units.
    [[nodiscard]] std::uint64_t sampleDts(std::uint32_t index) const noexcept;

    /// Duration of sample `index` in media timescale units.
    [[nodiscard]] std::uint64_t sampleDuration(std::uint32_t index) const noexcept;

    /// Composition offset of sample `index` (0 without ctts).
    [[nodiscard]] std::int64_t compositionOffset(std::uint32_t index) const noexcept;

    /// True when sample `index` is a sync sample.  Without an stss box every
    /// sample is a sync sample (ISO 14496-12 8.6.2).
    [[nodiscard]] bool isSync(std::uint32_t index) const noexcept;

    /// Index of the nearest sync sample at or before `index` (0 when none is
    /// listed before it, which is also the correct answer for a stream that
    /// starts with a keyframe).
    [[nodiscard]] std::uint32_t previousSync(std::uint32_t index) const noexcept;

    /// Index of the first sync sample strictly after `index`, or count()
    /// when there is none (the end of the GOP that contains `index`).
    [[nodiscard]] std::uint32_t nextSync(std::uint32_t index) const noexcept;

    /// Sorted 0-based sync sample indices (empty when no stss box exists).
    [[nodiscard]] const std::vector<std::uint32_t>& syncSamples() const noexcept { return m_sync; }

    /// True when an stss box was present.
    [[nodiscard]] bool hasSyncTable() const noexcept { return m_hasStss; }

    /// True when a ctts box was present.
    [[nodiscard]] bool hasCompositionOffsets() const noexcept { return m_hasCtts; }

    /// Sum of all sample durations (media timescale units).
    [[nodiscard]] std::uint64_t totalDuration() const noexcept { return m_totalDuration; }

    /// Largest sample size in the table (0 for an empty table).
    [[nodiscard]] std::uint32_t maxSampleSize() const noexcept { return m_maxSize; }

    /// Sum of all sample sizes.
    [[nodiscard]] std::uint64_t totalSize() const noexcept { return m_totalSize; }

    /// Number of chunks (stco/co64 entries).
    [[nodiscard]] std::uint32_t chunkCount() const noexcept { return static_cast<std::uint32_t>(m_chunkOffsets.size()); }

    /// Chunk offsets straight from stco/co64.
    [[nodiscard]] const std::vector<std::uint64_t>& chunkOffsets() const noexcept { return m_chunkOffsets; }

    /// Constant sample size from stsz (0 when sizes vary).
    [[nodiscard]] std::uint32_t constantSampleSize() const noexcept { return m_constantSize; }

private:
    /// One run of the decoding time table (stts) with its prefix sums.
    struct TimeRun {
        std::uint32_t firstSample = 0; ///< Index of the first sample in the run.
        std::uint32_t count = 0;       ///< Samples in the run.
        std::uint32_t delta = 0;       ///< Duration of each sample.
        std::uint64_t startTime = 0;   ///< DTS of the first sample.
    };

    /// One run of the composition offset table (ctts).
    struct CompositionRun {
        std::uint32_t firstSample = 0; ///< Index of the first sample in the run.
        std::uint32_t count = 0;       ///< Samples in the run.
        std::int64_t offset = 0;       ///< Composition offset (signed for v1).
    };

    /// One run of the sample-to-chunk table (stsc).
    struct ChunkRun {
        std::uint32_t firstChunk = 0;      ///< 1-based first chunk of the run.
        std::uint32_t samplesPerChunk = 0; ///< Samples in each chunk of the run.
        std::uint32_t descriptionIndex = 0;
    };

    Status parseStsz(const BoxHeader& box, WarningList* warnings);
    Status parseStz2(const BoxHeader& box, WarningList* warnings);
    Status parseStsc(const BoxHeader& box, std::vector<ChunkRun>& runs, WarningList* warnings);
    Status parseChunkOffsets(const BoxHeader& box, bool is64, WarningList* warnings);
    Status parseStts(const BoxHeader& box, WarningList* warnings);
    Status parseStss(const BoxHeader& box, WarningList* warnings);
    Status parseCtts(const BoxHeader& box, WarningList* warnings);
    Status buildSampleOffsets(const std::vector<ChunkRun>& runs, WarningList* warnings);

    std::uint32_t m_count = 0;          ///< Locatable samples.
    std::uint32_t m_stszCount = 0;      ///< Samples declared by stsz.
    std::uint32_t m_constantSize = 0;   ///< stsz sample_size when non-zero.
    std::vector<std::uint32_t> m_sizes; ///< Per-sample sizes (empty when constant).
    std::vector<std::uint64_t> m_offsets; ///< Per-sample absolute offsets.
    std::vector<std::uint32_t> m_sampleChunk; ///< Per-sample 0-based chunk index.
    std::vector<std::uint64_t> m_chunkOffsets;
    std::vector<TimeRun> m_timeRuns;
    std::vector<CompositionRun> m_compositionRuns;
    std::vector<std::uint32_t> m_sync;
    bool m_hasStss = false;
    bool m_hasCtts = false;
    std::uint64_t m_totalDuration = 0;
    std::uint64_t m_totalSize = 0;
    std::uint32_t m_maxSize = 0;
};

}  // namespace osv
