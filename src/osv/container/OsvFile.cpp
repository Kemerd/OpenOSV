// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OsvFile: file mapping + parsed movie.  The parser proper is in
// MovieParser.cpp (OsvFile::parseMovie); this file owns the lifetime side of
// things - opening, wrapping buffers, and handing out sample spans that alias
// the mapping.

#include "osv/container/OsvFile.h"

#include "osv/core/Log.h"

#include <utility>

namespace osv {

// -----------------------------------------------------------------------------
//  Opening
// -----------------------------------------------------------------------------

Result<OsvFile> OsvFile::open(const std::filesystem::path& path) {
    // An empty path is a caller bug; report it rather than letting the OS
    // produce a confusing "file not found" for "".
    if (path.empty()) {
        return Error{ErrorCode::InvalidArgument, "OsvFile::open: empty path"};
    }

    // Map the whole file read-only.  MappedFile falls back to reading the
    // bytes into memory when mapping is impossible, so this is the only
    // path we need.
    Result<MappedFile> mapped = MappedFile::open(path);
    if (!mapped.ok()) {
        return Error(mapped.error());
    }

    OsvFile file;
    file.m_file = std::move(mapped).value();
    file.m_path = path;

    // Parse the top level + moov.  Warnings end up inside the MovieInfo.
    Result<MovieInfo> movie = parseMovie(file.m_file.span(), nullptr);
    if (!movie.ok()) {
        log::debug("OsvFile::open('{}') failed: {}", log::safe(path.string()), movie.error().toString());
        return Error(movie.error());
    }
    file.m_movie = std::move(movie).value();

    // A short summary at debug level helps when a host application swallows
    // our return values.
    log::debug("OsvFile::open('{}'): {} bytes, {} track(s), {} warning(s), camd={}", log::safe(path.string()),
               file.m_file.size(), file.m_movie.tracks.size(), file.m_movie.warnings.size(),
               file.m_movie.camdBox.has_value());
    return file;
}

Result<OsvFile> OsvFile::fromBuffer(std::vector<std::uint8_t> bytes, const std::string& name) {
    // Wrap the buffer so the spans in MovieInfo have stable storage that
    // moves with the OsvFile.
    OsvFile file;
    file.m_file = MappedFile::fromBuffer(std::move(bytes));
    file.m_path = std::filesystem::path(name);

    Result<MovieInfo> movie = parseMovie(file.m_file.span(), nullptr);
    if (!movie.ok()) {
        return Error(movie.error());
    }
    file.m_movie = std::move(movie).value();
    return file;
}

// -----------------------------------------------------------------------------
//  Sample access
// -----------------------------------------------------------------------------

Result<ByteSpan> OsvFile::sample(std::uint32_t trackId, std::uint32_t index) const {
    // Guard against use of a default-constructed / moved-from object.
    if (!m_file.isOpen()) {
        return Error{ErrorCode::InvalidArgument, "OsvFile::sample: file is not open"};
    }
    // readSample validates the track, the index and that the bytes are
    // really inside the mapping (a truncated recording has offsets past
    // the end for its last samples).
    return readSample(m_movie, trackId, index);
}

// -----------------------------------------------------------------------------
//  Nested metadata movie
// -----------------------------------------------------------------------------

Result<MovieInfo> OsvFile::camdMovie() const {
    if (!m_file.isOpen()) {
        return Error{ErrorCode::InvalidArgument, "OsvFile::camdMovie: file is not open"};
    }
    if (!m_movie.camdBox) {
        return Error{ErrorCode::NotFound, "file has no top-level 'camd' box"};
    }

    // The payload of 'camd' is a complete ISO BMFF file of its own whose
    // chunk offsets are relative to the payload start, so parsing the body
    // span as a root makes every offset line up without adjustment.
    const ByteSpan payload = m_movie.camdBox->body;
    if (payload.size() < 8) {
        return Error{ErrorCode::Truncated, "'camd' box payload is " + std::to_string(payload.size()) +
                                               " bytes, too small for a nested movie"};
    }

    WarningList warnings;
    Result<MovieInfo> nested = parseMovie(payload, &warnings);
    if (!nested.ok()) {
        // Prefix so the caller can tell the nested failure from the outer one.
        return Error{nested.error().code, "nested 'camd' movie: " + nested.error().message};
    }
    // Make the origin of the warnings obvious when they are merged into a
    // combined diagnostic list by the caller.
    for (std::string& w : nested.value().warnings) {
        w.insert(0, "camd: ");
    }
    return nested;
}

}  // namespace osv
