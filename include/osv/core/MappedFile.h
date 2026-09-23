// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MappedFile: read-only memory mapping of a whole file.
//
// OSV files are tens of gigabytes for long recordings; the container parser
// only touches the box headers and the small metadata tracks, so mapping the
// file and aliasing ByteSpans into it avoids copying anything.  When mapping
// fails (network shares, exotic file systems) the class falls back to reading
// the file into memory so callers never need a second code path.
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace osv {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    /// Map `path` read-only.  Returns Io on any failure (missing file, no
    /// permission, zero length).
    static Result<MappedFile> open(const std::filesystem::path& path);

    /// Wrap an in-memory buffer (used by tests and by the nested `camd` movie).
    static MappedFile fromBuffer(std::vector<std::uint8_t> bytes);

    /// The mapped bytes (empty when not open).
    [[nodiscard]] ByteSpan span() const noexcept { return m_span; }
    [[nodiscard]] std::uint64_t size() const noexcept { return m_span.size(); }
    [[nodiscard]] bool isOpen() const noexcept { return !m_span.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

    /// True when the data came from a real mapping rather than a heap copy.
    [[nodiscard]] bool isMapped() const noexcept { return m_view != nullptr; }

private:
    void release() noexcept;

    std::filesystem::path m_path;
    ByteSpan m_span;
    // Windows mapping handles (opaque here to keep <windows.h> out of headers).
    // POSIX keeps no handles: only m_view is set, and munmap takes its length
    // from m_span.
    void* m_file = nullptr;
    void* m_mapping = nullptr;
    const void* m_view = nullptr;
    // Fallback storage when mapping is not possible.
    std::shared_ptr<std::vector<std::uint8_t>> m_buffer;
};

}  // namespace osv
