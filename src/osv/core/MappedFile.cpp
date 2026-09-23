// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/core/MappedFile.h"
#include "osv/core/Log.h"

#include <fstream>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// POSIX: open + fstat + mmap, the same read-only whole-file view.
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace osv {

namespace {

/// Read a whole file into a vector.  Used as the fallback when mapping fails
/// and on non-Windows hosts.
Result<std::vector<std::uint8_t>> readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return Error{ErrorCode::Io, "cannot open file: " + path.string()};
    }
    const std::streamoff length = in.tellg();
    if (length <= 0) {
        return Error{ErrorCode::Io, "file is empty: " + path.string()};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), length)) {
        return Error{ErrorCode::Io, "short read: " + path.string()};
    }
    return bytes;
}

}  // namespace

MappedFile::~MappedFile() { release(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : m_path(std::move(other.m_path)), m_span(other.m_span), m_file(other.m_file), m_mapping(other.m_mapping),
      m_view(other.m_view), m_buffer(std::move(other.m_buffer)) {
    other.m_span = ByteSpan{};
    other.m_file = nullptr;
    other.m_mapping = nullptr;
    other.m_view = nullptr;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        release();
        m_path = std::move(other.m_path);
        m_span = other.m_span;
        m_file = other.m_file;
        m_mapping = other.m_mapping;
        m_view = other.m_view;
        m_buffer = std::move(other.m_buffer);
        other.m_span = ByteSpan{};
        other.m_file = nullptr;
        other.m_mapping = nullptr;
        other.m_view = nullptr;
    }
    return *this;
}

void MappedFile::release() noexcept {
#if defined(_WIN32)
    if (m_view) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping) {
        CloseHandle(static_cast<HANDLE>(m_mapping));
        m_mapping = nullptr;
    }
    if (m_file) {
        CloseHandle(static_cast<HANDLE>(m_file));
        m_file = nullptr;
    }
#else
    // The mapping owns no descriptor (it is closed right after mmap), so the
    // view is the only thing to give back; the span still holds its length.
    if (m_view) {
        ::munmap(const_cast<void*>(m_view), static_cast<std::size_t>(m_span.size()));
        m_view = nullptr;
    }
#endif
    m_buffer.reset();
    m_span = ByteSpan{};
}

MappedFile MappedFile::fromBuffer(std::vector<std::uint8_t> bytes) {
    MappedFile f;
    f.m_buffer = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
    f.m_span = ByteSpan{f.m_buffer->data(), f.m_buffer->size()};
    f.m_path = "<memory>";
    return f;
}

Result<MappedFile> MappedFile::open(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return Error{ErrorCode::Io, "file not found: " + path.string()};
    }
    if (std::filesystem::is_directory(path, ec)) {
        return Error{ErrorCode::Io, "path is a directory: " + path.string()};
    }

    MappedFile f;
    f.m_path = path;

#if defined(_WIN32)
    // CreateFileW + CreateFileMappingW + MapViewOfFile: the classic read-only
    // mapping.  Sequential-scan hint helps the OS prefetch when we decode.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
            HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (mapping) {
                const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
                if (view) {
                    f.m_file = file;
                    f.m_mapping = mapping;
                    f.m_view = view;
                    f.m_span = ByteSpan{static_cast<const std::uint8_t*>(view), static_cast<std::size_t>(size.QuadPart)};
                    return f;
                }
                CloseHandle(mapping);
            }
        }
        CloseHandle(file);
        log::debug("MappedFile: mapping failed for {}, falling back to a heap copy", path.string());
    }
#else
    // open + mmap: a MAP_PRIVATE read-only view of the whole file.  The
    // descriptor can be closed straight away - the mapping keeps the file
    // alive - so nothing but the view (and its length, in the span) has to
    // be remembered.
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) {
        struct stat st{};
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            const auto length = static_cast<std::size_t>(st.st_size);
            void* view = ::mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
            if (view != MAP_FAILED) {
                // Sequential-scan hint, as FILE_FLAG_SEQUENTIAL_SCAN above.
                (void)::madvise(view, length, MADV_SEQUENTIAL);
                ::close(fd);
                f.m_view = view;
                f.m_span = ByteSpan{static_cast<const std::uint8_t*>(view), length};
                return f;
            }
        }
        ::close(fd);
        log::debug("MappedFile: mapping failed for {}, falling back to a heap copy", path.string());
    }
#endif

    // Fallback: read the whole file.
    OSV_TRY_ASSIGN(auto bytes, readWholeFile(path));
    f.m_buffer = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
    f.m_span = ByteSpan{f.m_buffer->data(), f.m_buffer->size()};
    return f;
}

}  // namespace osv
