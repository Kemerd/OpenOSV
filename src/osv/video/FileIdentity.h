// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Internal: a string that names one version of one file on disk.
//
// The process-wide caches of the video module (parsed containers, warm
// readers) must never hand out state built from a DIFFERENT file than the one
// asked for.  A path alone is not enough - the user can overwrite a clip with
// a new recording of the same name - so the identity also carries the size
// and the last-write time.  Windows paths are case-insensitive, so the path
// part is folded to lower case after it is made absolute and normalised; two
// spellings of one path meet, two different files never do.
//
// POSIX names the file by what the file system itself uses: the device and
// inode numbers from stat().  Every spelling of one file - symbolic links,
// ".." segments, a different case on macOS's case-insensitive volumes -
// meets, and a case-sensitive volume can never confuse two files whose names
// differ only in case.

#pragma once

#include "osv/core/Result.h"

#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#endif

namespace osv::video::detail {

/// Identity key of `path` as it is on disk right now.
/// Errors: Io when the file does not exist or cannot be stat'ed.
[[nodiscard]] inline Result<std::wstring> fileIdentity(const std::filesystem::path& path) {
    if (path.empty()) {
        return Error{ErrorCode::InvalidArgument, "empty path"};
    }
#if !defined(_WIN32)
    // "dev:ino|size|mtime-ns".  stat() follows symbolic links, so a link and
    // its target share one key, as two spellings of one path do on Windows.
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        const int err = errno;
        return Error{ErrorCode::Io, "cannot stat " + path.string() + ": " + std::strerror(err)};
    }
    // A directory has no size to key on (file_size fails for one on Windows).
    if (!S_ISREG(st.st_mode)) {
        return Error{ErrorCode::Io, "not a regular file: " + path.string()};
    }
#if defined(__APPLE__)
    const long long seconds = static_cast<long long>(st.st_mtimespec.tv_sec);
    const long long nanos = static_cast<long long>(st.st_mtimespec.tv_nsec);
#else
    const long long seconds = static_cast<long long>(st.st_mtim.tv_sec);
    const long long nanos = static_cast<long long>(st.st_mtim.tv_nsec);
#endif
    std::wstring key = std::to_wstring(static_cast<unsigned long long>(st.st_dev));
    key += L':';
    key += std::to_wstring(static_cast<unsigned long long>(st.st_ino));
    key += L'|';
    key += std::to_wstring(static_cast<unsigned long long>(st.st_size));
    key += L'|';
    key += std::to_wstring(seconds * 1000000000LL + nanos);
    return key;
#else
    std::error_code ec;
    // absolute() only fails for a path the OS cannot express at all; fall
    // back to the path as given rather than failing the lookup.
    std::filesystem::path full = std::filesystem::absolute(path, ec);
    if (ec) {
        full = path;
        ec.clear();
    }
    full = full.lexically_normal();
    const std::uintmax_t size = std::filesystem::file_size(full, ec);
    if (ec) {
        return Error{ErrorCode::Io, "cannot stat " + path.string() + ": " + ec.message()};
    }
    const auto written = std::filesystem::last_write_time(full, ec);
    if (ec) {
        return Error{ErrorCode::Io, "cannot read the modification time of " + path.string() + ": " + ec.message()};
    }
    // Lower-case path, then '|' size '|' ticks: '|' cannot occur in a Windows
    // path, so no two different triples can produce the same string.
    std::wstring key = full.wstring();
    for (wchar_t& c : key) {
        c = static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c)));
    }
    key += L'|';
    key += std::to_wstring(static_cast<unsigned long long>(size));
    key += L'|';
    key += std::to_wstring(static_cast<long long>(written.time_since_epoch().count()));
    return key;
#endif
}

}  // namespace osv::video::detail
