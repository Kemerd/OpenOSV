// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SampleTable implementation.  Every table is validated against the size of
// the box that carries it before anything is allocated, so a hostile entry
// count cannot request gigabytes of memory.

#include "osv/container/SampleTable.h"

#include "osv/container/BoxWalker.h"
#include "osv/core/ByteReader.h"

#include <algorithm>
#include <limits>

namespace osv {

namespace {

/// Read the 32-bit entry count that starts every sample table and check that
/// `count * entrySize` bytes actually follow it.  Returns Truncated otherwise.
Status readEntryCount(ByteReader& reader, const BoxHeader& box, std::uint64_t entrySize, std::uint32_t& count) {
    if (!reader.u32be(count)) {
        return failStatus(ErrorCode::Truncated, box.describe() + " has no entry count");
    }
    const std::uint64_t needed = static_cast<std::uint64_t>(count) * entrySize;
    if (needed > reader.remaining()) {
        return failStatus(ErrorCode::Truncated, box.describe() + " declares " + std::to_string(count) +
                                                    " entries (" + std::to_string(needed) + " bytes) but only " +
                                                    std::to_string(reader.remaining()) + " bytes follow");
    }
    return okStatus();
}

/// Largest index in a sorted vector that is <= value, or nullopt.
template <class T>
std::size_t upperIndex(const std::vector<T>& sorted, T value) noexcept {
    // std::upper_bound gives the first element > value; the one before it is
    // the last element <= value.
    const auto it = std::upper_bound(sorted.begin(), sorted.end(), value);
    return static_cast<std::size_t>(it - sorted.begin());
}

}  // namespace

// -----------------------------------------------------------------------------
//  Parsing
// -----------------------------------------------------------------------------

Result<SampleTable> SampleTable::parse(ByteSpan root, const BoxHeader& stbl, WarningList* warnings) {
    SampleTable table;
    std::vector<ChunkRun> chunkRuns;
    bool haveSizes = false;
    bool haveStsc = false;
    bool haveChunks = false;
    bool haveStts = false;
    Status firstError = okStatus();

    // Visit each child once; the first hard error is remembered and returned
    // after the walk so the warnings list still describes everything.
    BoxWalker::forEachChild(root, stbl, warnings, [&](const BoxHeader& box) {
        Status status = okStatus();
        if (box.type == Fourcc{"stsz"}) {
            status = table.parseStsz(box, warnings);
            haveSizes = haveSizes || status.ok();
        } else if (box.type == Fourcc{"stz2"}) {
            status = table.parseStz2(box, warnings);
            haveSizes = haveSizes || status.ok();
        } else if (box.type == Fourcc{"stsc"}) {
            status = table.parseStsc(box, chunkRuns, warnings);
            haveStsc = haveStsc || status.ok();
        } else if (box.type == Fourcc{"stco"}) {
            status = table.parseChunkOffsets(box, false, warnings);
            haveChunks = haveChunks || status.ok();
        } else if (box.type == Fourcc{"co64"}) {
            status = table.parseChunkOffsets(box, true, warnings);
            haveChunks = haveChunks || status.ok();
        } else if (box.type == Fourcc{"stts"}) {
            status = table.parseStts(box, warnings);
            haveStts = haveStts || status.ok();
        } else if (box.type == Fourcc{"stss"}) {
            status = table.parseStss(box, warnings);
        } else if (box.type == Fourcc{"ctts"}) {
            status = table.parseCtts(box, warnings);
        }
        // stsd / sdtp / sgpd etc. are handled elsewhere or ignored.
        if (!status.ok() && firstError.ok()) {
            firstError = status;
        }
        return true;
    });

    if (!firstError.ok()) {
        return Error(firstError.error());
    }
    // Mandatory boxes.
    if (!haveSizes) {
        return Error{ErrorCode::Malformed, "stbl " + stbl.describe() + " has no stsz/stz2 box"};
    }
    if (!haveStsc) {
        return Error{ErrorCode::Malformed, "stbl " + stbl.describe() + " has no stsc box"};
    }
    if (!haveChunks) {
        return Error{ErrorCode::Malformed, "stbl " + stbl.describe() + " has no stco/co64 box"};
    }
    if (!haveStts) {
        return Error{ErrorCode::Malformed, "stbl " + stbl.describe() + " has no stts box"};
    }

    // Expand chunk runs into per-sample offsets.
    OSV_TRY(table.buildSampleOffsets(chunkRuns, warnings));

    // Drop sync entries that point past the end (they can only come from a
    // corrupt file) and keep the list sorted and unique.
    if (table.m_hasStss) {
        std::sort(table.m_sync.begin(), table.m_sync.end());
        table.m_sync.erase(std::unique(table.m_sync.begin(), table.m_sync.end()), table.m_sync.end());
        const auto firstBad = std::lower_bound(table.m_sync.begin(), table.m_sync.end(), table.m_count);
        if (firstBad != table.m_sync.end()) {
            addWarning(warnings, "stss lists " + std::to_string(table.m_sync.end() - firstBad) +
                                     " sync sample(s) beyond the last sample, ignored");
            table.m_sync.erase(firstBad, table.m_sync.end());
        }
    }
    return table;
}

Status SampleTable::parseStsz(const BoxHeader& box, WarningList* warnings) {
    ByteReader reader(box.payload);
    std::uint32_t constantSize = 0;
    if (!reader.u32be(constantSize)) {
        return failStatus(ErrorCode::Truncated, box.describe() + " has no sample_size field");
    }
    std::uint32_t count = 0;
    if (!reader.u32be(count)) {
        return failStatus(ErrorCode::Truncated, box.describe() + " has no sample_count field");
    }
    m_stszCount = count;
    m_sizes.clear();
    m_constantSize = constantSize;
    m_totalSize = 0;
    m_maxSize = 0;

    if (constantSize != 0) {
        // Every sample has the same size; no table follows.
        m_totalSize = static_cast<std::uint64_t>(constantSize) * count;
        m_maxSize = count ? constantSize : 0;
        return okStatus();
    }
    // Variable sizes: count u32 entries must fit in the box.
    const std::uint64_t needed = static_cast<std::uint64_t>(count) * 4;
    if (needed > reader.remaining()) {
        return failStatus(ErrorCode::Truncated, box.describe() + " declares " + std::to_string(count) +
                                                    " sizes but only " + std::to_string(reader.remaining()) +
                                                    " bytes follow");
    }
    m_sizes.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t size = 0;
        if (!reader.u32be(size)) {
            return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the size table");
        }
        m_sizes[i] = size;
        m_totalSize += size;
        m_maxSize = std::max(m_maxSize, size);
    }
    (void)warnings;
    return okStatus();
}

Status SampleTable::parseStz2(const BoxHeader& box, WarningList* warnings) {
    // Compact sizes: 24 bits reserved, 8 bits field_size (4, 8 or 16), count.
    ByteReader reader(box.payload);
    std::uint32_t reservedAndFieldSize = 0;
    if (!reader.u32be(reservedAndFieldSize)) {
        return failStatus(ErrorCode::Truncated, box.describe() + " has no field_size");
    }
    const std::uint32_t fieldSize = reservedAndFieldSize & 0xFFu;
    if (fieldSize != 4 && fieldSize != 8 && fieldSize != 16) {
        return failStatus(ErrorCode::Malformed, box.describe() + " has field_size " + std::to_string(fieldSize));
    }
    std::uint32_t count = 0;
    if (!reader.u32be(count)) {
        return failStatus(ErrorCode::Truncated, box.describe() + " has no sample_count");
    }
    const std::uint64_t neededBits = static_cast<std::uint64_t>(count) * fieldSize;
    const std::uint64_t neededBytes = (neededBits + 7) / 8;
    if (neededBytes > reader.remaining()) {
        return failStatus(ErrorCode::Truncated, box.describe() + " declares " + std::to_string(count) +
                                                    " compact sizes but only " + std::to_string(reader.remaining()) +
                                                    " bytes follow");
    }
    m_stszCount = count;
    m_constantSize = 0;
    m_sizes.assign(count, 0);
    m_totalSize = 0;
    m_maxSize = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t size = 0;
        if (fieldSize == 16) {
            std::uint16_t v = 0;
            if (!reader.u16be(v)) {
                return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the size table");
            }
            size = v;
        } else if (fieldSize == 8) {
            std::uint8_t v = 0;
            if (!reader.u8(v)) {
                return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the size table");
            }
            size = v;
        } else {
            // Two 4-bit sizes per byte, high nibble first.
            const std::uint64_t byteIndex = i / 2;
            const std::uint8_t packed = box.payload.at(8 + byteIndex);
            size = (i % 2 == 0) ? (packed >> 4) : (packed & 0x0Fu);
        }
        m_sizes[i] = size;
        m_totalSize += size;
        m_maxSize = std::max(m_maxSize, size);
    }
    (void)warnings;
    return okStatus();
}

Status SampleTable::parseStsc(const BoxHeader& box, std::vector<ChunkRun>& runs, WarningList* warnings) {
    ByteReader reader(box.payload);
    std::uint32_t count = 0;
    OSV_TRY(readEntryCount(reader, box, 12, count));
    runs.clear();
    runs.reserve(count);
    std::uint32_t previousFirstChunk = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        ChunkRun run;
        if (!reader.u32be(run.firstChunk) || !reader.u32be(run.samplesPerChunk) ||
            !reader.u32be(run.descriptionIndex)) {
            return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the entry table");
        }
        // Runs must be in strictly increasing chunk order, 1-based.
        if (run.firstChunk == 0 || run.firstChunk <= previousFirstChunk) {
            addWarning(warnings, box.describe() + " entry " + std::to_string(i) + " has first_chunk " +
                                     std::to_string(run.firstChunk) + " out of order; later entries ignored");
            break;
        }
        previousFirstChunk = run.firstChunk;
        runs.push_back(run);
    }
    return okStatus();
}

Status SampleTable::parseChunkOffsets(const BoxHeader& box, bool is64, WarningList* warnings) {
    ByteReader reader(box.payload);
    std::uint32_t count = 0;
    OSV_TRY(readEntryCount(reader, box, is64 ? 8 : 4, count));
    if (!m_chunkOffsets.empty()) {
        addWarning(warnings, box.describe() + " duplicates an earlier chunk offset table; using the later one");
    }
    m_chunkOffsets.assign(count, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint64_t offset = 0;
        if (is64) {
            if (!reader.u64be(offset)) {
                return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the offset table");
            }
        } else {
            std::uint32_t offset32 = 0;
            if (!reader.u32be(offset32)) {
                return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the offset table");
            }
            offset = offset32;
        }
        m_chunkOffsets[i] = offset;
    }
    return okStatus();
}

Status SampleTable::parseStts(const BoxHeader& box, WarningList* warnings) {
    ByteReader reader(box.payload);
    std::uint32_t count = 0;
    OSV_TRY(readEntryCount(reader, box, 8, count));
    m_timeRuns.clear();
    m_timeRuns.reserve(count);
    std::uint64_t sampleIndex = 0;
    std::uint64_t time = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        TimeRun run;
        if (!reader.u32be(run.count) || !reader.u32be(run.delta)) {
            return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the entry table");
        }
        // Skip empty runs; they carry no information.
        if (run.count == 0) {
            continue;
        }
        // Guard the sample index against overflowing 32 bits.
        if (sampleIndex + run.count > std::numeric_limits<std::uint32_t>::max()) {
            addWarning(warnings, box.describe() + " covers more than 2^32 samples; remaining entries ignored");
            break;
        }
        run.firstSample = static_cast<std::uint32_t>(sampleIndex);
        run.startTime = time;
        m_timeRuns.push_back(run);
        sampleIndex += run.count;
        time += static_cast<std::uint64_t>(run.count) * run.delta;
    }
    m_totalDuration = time;
    return okStatus();
}

Status SampleTable::parseStss(const BoxHeader& box, WarningList* warnings) {
    ByteReader reader(box.payload);
    std::uint32_t count = 0;
    OSV_TRY(readEntryCount(reader, box, 4, count));
    m_hasStss = true;
    m_sync.clear();
    m_sync.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t sampleNumber = 0;
        if (!reader.u32be(sampleNumber)) {
            return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the entry table");
        }
        // stss is 1-based; 0 is invalid.
        if (sampleNumber == 0) {
            addWarning(warnings, box.describe() + " contains sample number 0, ignored");
            continue;
        }
        m_sync.push_back(sampleNumber - 1);
    }
    return okStatus();
}

Status SampleTable::parseCtts(const BoxHeader& box, WarningList* warnings) {
    ByteReader reader(box.payload);
    std::uint32_t count = 0;
    OSV_TRY(readEntryCount(reader, box, 8, count));
    m_hasCtts = true;
    m_compositionRuns.clear();
    m_compositionRuns.reserve(count);
    std::uint64_t sampleIndex = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        CompositionRun run;
        std::uint32_t rawOffset = 0;
        if (!reader.u32be(run.count) || !reader.u32be(rawOffset)) {
            return failStatus(ErrorCode::Truncated, box.describe() + " ends inside the entry table");
        }
        // Version 1 offsets are signed (negative composition offsets).
        run.offset = box.version == 1 ? static_cast<std::int64_t>(static_cast<std::int32_t>(rawOffset))
                                      : static_cast<std::int64_t>(rawOffset);
        if (run.count == 0) {
            continue;
        }
        if (sampleIndex + run.count > std::numeric_limits<std::uint32_t>::max()) {
            addWarning(warnings, box.describe() + " covers more than 2^32 samples; remaining entries ignored");
            break;
        }
        run.firstSample = static_cast<std::uint32_t>(sampleIndex);
        m_compositionRuns.push_back(run);
        sampleIndex += run.count;
    }
    return okStatus();
}

Status SampleTable::buildSampleOffsets(const std::vector<ChunkRun>& runs, WarningList* warnings) {
    const std::uint32_t chunkCount = static_cast<std::uint32_t>(m_chunkOffsets.size());
    m_offsets.clear();
    m_sampleChunk.clear();
    m_count = 0;
    if (m_stszCount == 0) {
        return okStatus();
    }
    // Reserve for the common case only.  A hostile stsz with a constant
    // sample size can declare billions of samples without needing a table,
    // so the reservation is capped; the vectors still grow on demand for
    // the (chunk-table bounded) samples that really exist.
    constexpr std::uint32_t kReserveCap = 1u << 20;
    const std::uint32_t reserveCount = std::min(m_stszCount, kReserveCap);
    m_offsets.reserve(reserveCount);
    m_sampleChunk.reserve(reserveCount);

    // Walk the stsc runs; each run applies from its first_chunk up to the
    // next run's first_chunk (exclusive), the last run to the final chunk.
    std::uint32_t sampleIndex = 0;
    bool stopped = false;
    for (std::size_t r = 0; r < runs.size() && !stopped; ++r) {
        const ChunkRun& run = runs[r];
        const std::uint32_t firstChunk = run.firstChunk;  // 1-based
        const std::uint32_t endChunk = (r + 1 < runs.size()) ? runs[r + 1].firstChunk : chunkCount + 1;
        if (firstChunk > chunkCount) {
            addWarning(warnings, "stsc run starts at chunk " + std::to_string(firstChunk) + " but only " +
                                     std::to_string(chunkCount) + " chunk offsets exist");
            break;
        }
        for (std::uint32_t chunk = firstChunk; chunk < endChunk && chunk <= chunkCount; ++chunk) {
            std::uint64_t offset = m_chunkOffsets[chunk - 1];
            for (std::uint32_t s = 0; s < run.samplesPerChunk; ++s) {
                if (sampleIndex >= m_stszCount) {
                    stopped = true;
                    break;
                }
                m_offsets.push_back(offset);
                m_sampleChunk.push_back(chunk - 1);
                const std::uint32_t size = m_constantSize ? m_constantSize : m_sizes[sampleIndex];
                // Guard against absurd sizes pushing the offset past 2^64.
                if (offset > std::numeric_limits<std::uint64_t>::max() - size) {
                    addWarning(warnings, "sample offsets overflow 64 bits at sample " + std::to_string(sampleIndex));
                    stopped = true;
                    break;
                }
                offset += size;
                ++sampleIndex;
            }
            if (stopped) {
                break;
            }
        }
    }

    m_count = sampleIndex;
    if (m_count < m_stszCount) {
        addWarning(warnings, "chunk tables cover " + std::to_string(m_count) + " of " + std::to_string(m_stszCount) +
                                 " samples declared by stsz; the rest cannot be located");
    }
    // Warn when the time table disagrees with the sample count; dts of the
    // uncovered samples is extrapolated with the last delta.
    std::uint64_t timed = 0;
    if (!m_timeRuns.empty()) {
        const TimeRun& last = m_timeRuns.back();
        timed = static_cast<std::uint64_t>(last.firstSample) + last.count;
    }
    if (timed < m_count) {
        addWarning(warnings, "stts covers " + std::to_string(timed) + " of " + std::to_string(m_count) +
                                 " samples; durations of the rest are extrapolated");
    }
    return okStatus();
}

// -----------------------------------------------------------------------------
//  Queries
// -----------------------------------------------------------------------------

std::uint64_t SampleTable::sampleOffset(std::uint32_t index) const noexcept {
    if (index >= m_offsets.size()) {
        return 0;
    }
    return m_offsets[index];
}

std::uint32_t SampleTable::sampleSize(std::uint32_t index) const noexcept {
    if (index >= m_count) {
        return 0;
    }
    if (m_constantSize != 0) {
        return m_constantSize;
    }
    return index < m_sizes.size() ? m_sizes[index] : 0;
}

std::uint64_t SampleTable::sampleDts(std::uint32_t index) const noexcept {
    if (m_timeRuns.empty()) {
        return 0;
    }
    // Binary search the run that contains `index` (runs are sorted by
    // firstSample).  Samples past the last run use its delta.
    auto it = std::upper_bound(m_timeRuns.begin(), m_timeRuns.end(), index,
                               [](std::uint32_t value, const TimeRun& run) { return value < run.firstSample; });
    if (it == m_timeRuns.begin()) {
        return 0;
    }
    --it;
    const std::uint64_t within = static_cast<std::uint64_t>(index) - it->firstSample;
    return it->startTime + within * it->delta;
}

std::uint64_t SampleTable::sampleDuration(std::uint32_t index) const noexcept {
    if (m_timeRuns.empty()) {
        return 0;
    }
    auto it = std::upper_bound(m_timeRuns.begin(), m_timeRuns.end(), index,
                               [](std::uint32_t value, const TimeRun& run) { return value < run.firstSample; });
    if (it == m_timeRuns.begin()) {
        return 0;
    }
    --it;
    return it->delta;
}

std::int64_t SampleTable::compositionOffset(std::uint32_t index) const noexcept {
    if (m_compositionRuns.empty()) {
        return 0;
    }
    auto it = std::upper_bound(m_compositionRuns.begin(), m_compositionRuns.end(), index,
                               [](std::uint32_t value, const CompositionRun& run) { return value < run.firstSample; });
    if (it == m_compositionRuns.begin()) {
        return 0;
    }
    --it;
    // Beyond the last run there is no information; report 0.
    if (static_cast<std::uint64_t>(index) >= static_cast<std::uint64_t>(it->firstSample) + it->count) {
        return 0;
    }
    return it->offset;
}

bool SampleTable::isSync(std::uint32_t index) const noexcept {
    if (index >= m_count) {
        return false;
    }
    // No stss box: every sample is a sync sample.
    if (!m_hasStss) {
        return true;
    }
    return std::binary_search(m_sync.begin(), m_sync.end(), index);
}

std::uint32_t SampleTable::previousSync(std::uint32_t index) const noexcept {
    if (m_count == 0) {
        return 0;
    }
    if (index >= m_count) {
        index = m_count - 1;
    }
    if (!m_hasStss) {
        return index;
    }
    const std::size_t pos = upperIndex(m_sync, index);
    if (pos == 0) {
        return 0;
    }
    return m_sync[pos - 1];
}

std::uint32_t SampleTable::nextSync(std::uint32_t index) const noexcept {
    if (m_count == 0) {
        return 0;
    }
    if (!m_hasStss) {
        return index + 1 < m_count ? index + 1 : m_count;
    }
    const std::size_t pos = upperIndex(m_sync, index);
    if (pos >= m_sync.size()) {
        return m_count;
    }
    return m_sync[pos];
}

Result<SampleLocation> SampleTable::locate(std::uint32_t index) const {
    if (index >= m_count) {
        return Error{ErrorCode::NotFound, "sample index " + std::to_string(index) + " out of range (" +
                                              std::to_string(m_count) + " samples)"};
    }
    SampleLocation loc;
    loc.offset = m_offsets[index];
    loc.size = sampleSize(index);
    loc.dts = sampleDts(index);
    loc.duration = sampleDuration(index);
    loc.ctsOffset = compositionOffset(index);
    loc.sync = isSync(index);
    loc.chunkIndex = index < m_sampleChunk.size() ? m_sampleChunk[index] : 0;
    return loc;
}

}  // namespace osv
