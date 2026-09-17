// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OsvFile: an opened .OSV (or .LRF, or any ISO BMFF) file - the memory
// mapping plus the parsed MovieInfo.  Sample access returns spans that alias
// the mapping, so reading a 6K HEVC frame or a 6 KB metadata sample costs no
// copy at all.  The nested metadata movie inside the trailing 'camd' box is
// parsed on demand with the same parser.
#pragma once

#include "osv/container/MovieInfo.h"
#include "osv/container/TrackInfo.h"
#include "osv/core/ByteSpan.h"
#include "osv/core/MappedFile.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace osv {

class OsvFile {
public:
    OsvFile() = default;
    OsvFile(OsvFile&&) noexcept = default;
    OsvFile& operator=(OsvFile&&) noexcept = default;
    OsvFile(const OsvFile&) = delete;
    OsvFile& operator=(const OsvFile&) = delete;

    /// Map `path` and parse its movie.  Fails with Io when the file cannot be
    /// opened, Malformed when it is not an ISO BMFF file, Truncated when the
    /// 'moov' box is missing because the file ends early.  Non-fatal problems
    /// are collected in movie().warnings.
    [[nodiscard]] static Result<OsvFile> open(const std::filesystem::path& path);

    /// Parse an in-memory buffer (tests, fixtures).  `name` is reported by path().
    [[nodiscard]] static Result<OsvFile> fromBuffer(std::vector<std::uint8_t> bytes, const std::string& name = "<memory>");

    /// Parse the top-level structure and 'moov' of any ISO BMFF span without
    /// opening a file.  Never throws; hostile or truncated input yields an
    /// error and/or warnings.  When `warnings` is given the messages are
    /// appended there as well as to the returned MovieInfo.
    [[nodiscard]] static Result<MovieInfo> parseMovie(ByteSpan span, WarningList* warnings = nullptr);

    /// The parsed movie.
    [[nodiscard]] const MovieInfo& movie() const noexcept { return m_movie; }

    /// Track by track_ID, or null.
    [[nodiscard]] const TrackInfo* track(std::uint32_t trackId) const noexcept { return m_movie.track(trackId); }

    /// Tracks of one kind in file order.
    [[nodiscard]] std::vector<const TrackInfo*> tracksOfKind(TrackKind kind) const { return m_movie.tracksOfKind(kind); }

    /// Bytes of sample `index` (0-based) of track `trackId`, aliasing the mapping.
    [[nodiscard]] Result<ByteSpan> sample(std::uint32_t trackId, std::uint32_t index) const;

    /// Parse the nested movie stored in the trailing 'camd' box.  Its
    /// `source` is the camd payload, so use readSample(camd, ...) to fetch
    /// its samples.  NotFound when the file has no 'camd' box.
    [[nodiscard]] Result<MovieInfo> camdMovie() const;

    /// True when a 'camd' box exists.
    [[nodiscard]] bool hasCamd() const noexcept { return m_movie.camdBox.has_value(); }

    /// The whole file.
    [[nodiscard]] ByteSpan span() const noexcept { return m_file.span(); }

    /// Path the file was opened from.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    /// File size in bytes.
    [[nodiscard]] std::uint64_t size() const noexcept { return m_file.size(); }

private:
    MappedFile m_file;
    std::filesystem::path m_path;
    MovieInfo m_movie;
};

}  // namespace osv
