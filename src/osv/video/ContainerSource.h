// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Internal: the video module's only doorway into osv_container.
//
// Decoder.cpp needs three things from the container parser - the sample
// count, the previous sync sample and the bytes of sample i - plus the codec
// parameter sets for the direct-feed path.  Wrapping them here keeps the
// container headers out of the FFmpeg translation unit and gives the
// decoder a tiny, testable surface.
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace osv::video::detail {

/// One sample of the selected track, aliasing the file mapping.
struct SampleView {
    ByteSpan bytes;               ///< Raw sample bytes (length-prefixed NAL units for hvc1/avc1).
    std::int64_t dts = 0;         ///< Decode timestamp in track timescale ticks.
    std::int64_t ctsOffset = 0;   ///< Composition offset (ctts) in ticks; 0 without B-frames.
    bool sync = false;            ///< True for a random access point (stss).
};

/// Video codec of the selected track as far as the sample entry tells us.
enum class TrackCodec : std::uint8_t {
    Unknown = 0,  ///< Neither hvcC nor avcC present.
    Hevc = 1,     ///< 'hvc1' / 'hev1' with an hvcC record (native OSV streams).
    Avc = 2       ///< 'avc1' / 'avc3' with an avcC record (LRF proxy).
};

class ContainerSource {
public:
    ~ContainerSource();
    ContainerSource(const ContainerSource&) = delete;
    ContainerSource& operator=(const ContainerSource&) = delete;

    /// Open `path` with osv::OsvFile and select track `trackId`.
    ///
    /// The parsed movie is shared: every ContainerSource of the same file
    /// (same path, size and modification time) that is alive at the same time
    /// uses ONE OsvFile - one mapping, one moov parse - so the two lenses of a
    /// reader, and a second reader of a clip that is already open, skip the
    /// parse.  The table is weak; the movie is freed with its last user.
    /// Errors: Io (cannot map), Malformed / Truncated (parser rejected the
    /// file), NotFound (no such track, not a video track, or no samples).
    static Result<std::unique_ptr<ContainerSource>> open(const std::filesystem::path& path, std::uint32_t trackId);

    /// True when open() found the movie already parsed by a live user.
    [[nodiscard]] bool reusedParse() const noexcept;

    /// ISO BMFF track id that was selected.
    [[nodiscard]] std::uint32_t trackId() const noexcept;

    /// Number of samples in the track.
    [[nodiscard]] std::uint32_t sampleCount() const noexcept;

    /// Index of the nearest sample at or before `index` a decode can start
    /// from.  That is the listed sync sample (listedSync()) whenever its own
    /// first picture is a random access point - every camera file, except one
    /// lens of a recording that dropped frames, whose IDRs can sit a sample
    /// after the shared sync table's entries.  Then the nearest sample at or
    /// before `index` whose first picture IS one is returned instead (found
    /// from the NAL headers, at most kMaxDecodeStartScan samples back), so a
    /// decode never starts on a picture whose references precede it.  0 when
    /// the table has no stss (every sample is a sync sample) or nothing else
    /// is known.
    [[nodiscard]] std::uint32_t previousSync(std::uint32_t index) const noexcept;

    /// The sync table's own answer: the nearest LISTED sync sample at or
    /// before `index` (0 without an stss).  What libavformat's index holds.
    [[nodiscard]] std::uint32_t listedSync(std::uint32_t index) const noexcept;

    /// Bytes and timing of sample `index`.  Errors: InvalidArgument (out of
    /// range), Truncated (sample lies outside the file).
    [[nodiscard]] Result<SampleView> sample(std::uint32_t index) const;

    /// Media timescale of the track (60000 on the sample clip).
    [[nodiscard]] std::uint32_t timescale() const noexcept;

    /// AVERAGE frames per second over the sample table (stts): samples - 1
    /// over the span from the first to the last decode time; 0.0 when
    /// unknown.  Exact for a constant-rate track and informational on a
    /// variable-rate one - frame indices come from the sample table
    /// (sampleAt / presentationTicks), never from this.
    [[nodiscard]] double fps() const noexcept;

    // ---- the sample table as a clock -----------------------------------------
    //
    // On a variable-frame-rate recording (the camera drops frames when it
    // cannot keep up and writes the gap into the stts as a longer sample) a
    // time-based index at the average rate names a different sample than
    // the one asked for.  These turn the table itself into the index: sample
    // i is presented at presentationTicks(i), and a decoded frame's pts maps
    // back to exactly one sample.  For a constant-rate track they return
    // exactly what the average-rate formulas did (i * delta), so behaviour
    // there is unchanged.

    /// True when frames can be indexed by the sample table: the track has no
    /// composition offsets (no B-frame reordering, which the camera never
    /// writes; reordered streams keep the average-rate fallback until they
    /// are tested) and every sample is presented strictly after the previous
    /// one.  False for an unopened source.
    [[nodiscard]] bool indexedByTable() const noexcept;

    /// Presentation time of sample `index` in timescale ticks, relative to
    /// sample 0's: dts(i) + cts(i) - (dts(0) + cts(0)).  Samples past the end
    /// are extrapolated with the last duration (as the table does); 0 for an
    /// unopened source.
    [[nodiscard]] std::int64_t presentationTicks(std::uint32_t index) const noexcept;

    /// Duration of sample `index` in timescale ticks (its stts delta; the
    /// last run's for an index past the end, 0 when unknown).
    [[nodiscard]] std::uint64_t sampleDuration(std::uint32_t index) const noexcept;

    /// The sample presented NEAREST to `relTicks` (ticks relative to sample
    /// 0's presentation time), a tie going to the later sample - exactly
    /// llround(relTicks / delta) on a constant-rate track.  Times before the
    /// first or after the last sample are extrapolated with the edge
    /// sample's duration, so the answer can be negative or >= sampleCount(),
    /// as the average-rate formula's could; callers treat those as "not a
    /// sample of this track".  Meaningful only when indexedByTable().
    [[nodiscard]] std::int64_t sampleAt(std::int64_t relTicks) const noexcept;

    /// Coded width / height from the sample entry (0 when absent).
    [[nodiscard]] std::uint32_t codedWidth() const noexcept;
    [[nodiscard]] std::uint32_t codedHeight() const noexcept;

    /// Codec according to the sample entry's configuration record.
    [[nodiscard]] TrackCodec codec() const noexcept;

    /// Luma bit depth from hvcC (8 for avcC / unknown).
    [[nodiscard]] std::uint8_t bitDepth() const noexcept;

    /// Parameter sets as an Annex-B byte string (each NAL preceded by
    /// 00 00 00 01).  Empty when codec() is Unknown.
    [[nodiscard]] std::vector<std::uint8_t> annexBHeader() const;

    /// Size in bytes of the NAL length prefixes in the samples (4 on the
    /// Osmo 360, i.e. lengthSizeMinusOne + 1).  0 when codec() is Unknown.
    [[nodiscard]] std::uint32_t nalLengthSize() const noexcept;

private:
    ContainerSource();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::video::detail
