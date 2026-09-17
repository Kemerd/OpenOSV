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
    /// Errors: Io (cannot map), Malformed / Truncated (parser rejected the
    /// file), NotFound (no such track, not a video track, or no samples).
    static Result<std::unique_ptr<ContainerSource>> open(const std::filesystem::path& path, std::uint32_t trackId);

    /// ISO BMFF track id that was selected.
    [[nodiscard]] std::uint32_t trackId() const noexcept;

    /// Number of samples in the track.
    [[nodiscard]] std::uint32_t sampleCount() const noexcept;

    /// Index of the nearest sync sample at or before `index` (0 when the
    /// table has no stss, which means every sample is a sync sample).
    [[nodiscard]] std::uint32_t previousSync(std::uint32_t index) const noexcept;

    /// Bytes and timing of sample `index`.  Errors: InvalidArgument (out of
    /// range), Truncated (sample lies outside the file).
    [[nodiscard]] Result<SampleView> sample(std::uint32_t index) const;

    /// Media timescale of the track (60000 on the sample clip).
    [[nodiscard]] std::uint32_t timescale() const noexcept;

    /// Frames per second from the sample table (stts), 0.0 when unknown.
    [[nodiscard]] double fps() const noexcept;

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
