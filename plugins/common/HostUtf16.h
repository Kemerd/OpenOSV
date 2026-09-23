// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HostUtf16.h - the host's UTF-16 strings (prUTF16Char) and ours.
//
// Premiere hands importers file paths, and takes stream names and match
// names back, as NUL-terminated UTF-16 (prUTF16Char).  The two platforms
// differ in exactly one way that matters:
//
//   * Windows: prUTF16Char IS wchar_t, std::wstring is UTF-16 and a path
//     is natively wide, so a reinterpret_cast is the whole conversion -
//     which is what the plug-ins always did there, and what these helpers
//     still do there, byte for byte;
//   * macOS:   prUTF16Char is a 16-bit integer while wchar_t is 32 bits
//     (UTF-32), and paths are natively UTF-8.  Casting would read every
//     other character as garbage, so the helpers transcode: UTF-16 to
//     UTF-8 for a path, UTF-16 to UTF-32 for a wide string, and back,
//     including surrogate pairs (an emoji in a folder name is a pair).
//
// SDK-free on purpose: templated on the code unit type, so any 16-bit type
// the SDK chooses works and the tests can use char16_t.  Every function is
// noexcept-safe in spirit - malformed input (a lone surrogate) becomes
// U+FFFD instead of an exception or a truncated path - but the allocating
// ones may throw std::bad_alloc like any string construction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

namespace osv::premiere {

namespace hostutf16 {

/// Longest string the helpers will walk before giving up on finding the
/// terminator: the SDK's largest path field is 2048 units, and nothing the
/// host sends is legitimately longer than a few times that.
inline constexpr std::size_t kMaxUnits = 32768;

/// Number of code units before the NUL (at most kMaxUnits).
template <class CharT>
[[nodiscard]] std::size_t length(const CharT* text) noexcept {
    static_assert(sizeof(CharT) == 2, "the host's strings are UTF-16");
    if (!text) {
        return 0;
    }
    std::size_t n = 0;
    while (n < kMaxUnits && text[n] != 0) {
        ++n;
    }
    return n;
}

/// Decode the code point starting at text[i] (advancing i).  A lone or
/// reversed surrogate decodes as U+FFFD.
template <class CharT>
[[nodiscard]] char32_t decodeAt(const CharT* text, std::size_t n, std::size_t& i) noexcept {
    const auto unit = static_cast<std::uint16_t>(text[i++]);
    if (unit >= 0xD800u && unit <= 0xDBFFu) {
        if (i < n) {
            const auto low = static_cast<std::uint16_t>(text[i]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                ++i;
                return static_cast<char32_t>(0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
            }
        }
        return U'�';
    }
    if (unit >= 0xDC00u && unit <= 0xDFFFu) {
        return U'�';
    }
    return static_cast<char32_t>(unit);
}

/// Append one code point as UTF-8.
inline void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

/// Append one code point as UTF-16 (a surrogate pair above the BMP).
inline void appendUtf16(std::u16string& out, char32_t cp) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        cp = U'�';
    }
    if (cp >= 0x10000u) {
        const char32_t v = cp - 0x10000u;
        out.push_back(static_cast<char16_t>(0xD800u + (v >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00u + (v & 0x3FFu)));
    } else {
        out.push_back(static_cast<char16_t>(cp));
    }
}

}  // namespace hostutf16

/// A host UTF-16 string as our wide string (UTF-16 on Windows, UTF-32
/// elsewhere).  Null or empty input gives an empty string.
template <class CharT>
[[nodiscard]] std::wstring wideFromHostUtf16(const CharT* text) {
    static_assert(sizeof(CharT) == 2, "the host's strings are UTF-16");
    if (!text) {
        return {};
    }
#if defined(_WIN32)
    return std::wstring(reinterpret_cast<const wchar_t*>(text));
#else
    const std::size_t n = hostutf16::length(text);
    std::wstring out;
    out.reserve(n);
    for (std::size_t i = 0; i < n;) {
        out.push_back(static_cast<wchar_t>(hostutf16::decodeAt(text, n, i)));
    }
    return out;
#endif
}

/// A host UTF-16 path as a filesystem path.  On Windows exactly the old
/// std::filesystem::path(const wchar_t*); elsewhere through UTF-8, the
/// native path encoding of macOS.
template <class CharT>
[[nodiscard]] std::filesystem::path pathFromHostUtf16(const CharT* text) {
    static_assert(sizeof(CharT) == 2, "the host's strings are UTF-16");
    if (!text) {
        return {};
    }
#if defined(_WIN32)
    return std::filesystem::path(reinterpret_cast<const wchar_t*>(text));
#else
    const std::size_t n = hostutf16::length(text);
    std::string utf8;
    utf8.reserve(n + n / 2);
    for (std::size_t i = 0; i < n;) {
        hostutf16::appendUtf8(utf8, hostutf16::decodeAt(text, n, i));
    }
    return std::filesystem::path(utf8);
#endif
}

/// Our wide string as the host's UTF-16 (NUL not included).
[[nodiscard]] inline std::u16string hostUtf16FromWide(const std::wstring& text) {
    std::u16string out;
    out.reserve(text.size());
#if defined(_WIN32)
    // Same code units: wchar_t is UTF-16 here.
    for (const wchar_t c : text) {
        out.push_back(static_cast<char16_t>(c));
    }
#else
    for (const wchar_t c : text) {
        hostutf16::appendUtf16(out, static_cast<char32_t>(c));
    }
#endif
    return out;
}

/// Copy our wide string into a host UTF-16 field of `capacity` units:
/// always NUL terminated and never overrun.  The whole field is zeroed
/// first.  Off Windows a truncation never splits a surrogate pair either;
/// on Windows the units are copied exactly as the importer always copied
/// them.
template <class CharT>
void copyWideToHostUtf16(CharT* dst, std::size_t capacity, const std::wstring& src) noexcept {
    static_assert(sizeof(CharT) == 2, "the host's strings are UTF-16");
    if (!dst || capacity == 0) {
        return;
    }
    std::memset(dst, 0, capacity * sizeof(CharT));
    try {
        const std::u16string units = hostUtf16FromWide(src);
        std::size_t n = units.size() < capacity - 1u ? units.size() : capacity - 1u;
#if !defined(_WIN32)
        // Do not end on the high half of a pair the field cannot finish.
        if (n > 0 && n < units.size()) {
            const auto last = static_cast<std::uint16_t>(units[n - 1]);
            if (last >= 0xD800u && last <= 0xDBFFu) {
                --n;
            }
        }
#endif
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = static_cast<CharT>(units[i]);
        }
    } catch (...) {
        // Allocation failure: the field stays an empty, terminated string.
        dst[0] = 0;
    }
}

}  // namespace osv::premiere
