// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Crc64.h - CRC-64/XZ over a byte range, for content fingerprints in logs.
//
// WHY A CRC
// ---------
// The decoder (one line per decoded picture at Debug) and the importer (one
// line per delivered frame) print fingerprints so that a host session's log
// can be compared, frame by frame, with an offline decode of the same clip:
// equal fingerprints mean equal pixels.  The CRC is the one xz uses (ECMA-182
// polynomial, reflected, init and final XOR all ones), so a value can be
// re-computed with any xz-compatible tool, and it is the same CRC the unit
// tests have always used to hash frames.
//
// HOW
// ---
// "Slicing by 8": eight 256-entry tables let the loop consume eight bytes per
// step with eight independent table lookups instead of a dependent chain of
// eight, about four to five times faster than the byte-at-a-time form.  The
// tables are built at compile time (16 KiB, read-only data), so there is no
// initialisation order to get wrong and nothing to lock.
//
// Header-only on purpose: it is a pure function used by the library, the
// plug-ins and the tests alike, and needs no build-system entry anywhere.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace osv {

namespace detail {

/// The reflected ECMA-182 polynomial (CRC-64/XZ).
inline constexpr std::uint64_t kCrc64Polynomial = 0xC96C5795D7870F42ull;

/// The eight slicing tables: table[0] is the classic byte table, table[k][b]
/// is table[0] advanced by k further zero bytes.
using Crc64Tables = std::array<std::array<std::uint64_t, 256>, 8>;

/// @brief Build the slicing-by-8 tables (evaluated at compile time).
/// @return The eight 256-entry tables.
constexpr Crc64Tables makeCrc64Tables() noexcept {
    Crc64Tables t{};
    // ---- the byte table: one polynomial division per byte value ------------
    for (std::uint32_t b = 0; b < 256; ++b) {
        std::uint64_t c = b;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (kCrc64Polynomial ^ (c >> 1)) : (c >> 1);
        }
        t[0][b] = c;
    }
    // ---- each further table shifts the previous one by a zero byte --------
    for (std::size_t k = 1; k < 8; ++k) {
        for (std::uint32_t b = 0; b < 256; ++b) {
            const std::uint64_t prev = t[k - 1][b];
            t[k][b] = t[0][prev & 0xFFu] ^ (prev >> 8);
        }
    }
    return t;
}

/// The tables, one copy per program (an inline variable).
inline constexpr Crc64Tables kCrc64Tables = makeCrc64Tables();

}  // namespace detail

/// @brief CRC-64/XZ of `bytes` bytes at `data`, continuing from `crc`.
///
/// Streaming works like xz's lzma_crc64(): pass 0 for the first block and the
/// previous result for each further block, so
/// `crc64(b, nb, crc64(a, na))` equals the CRC of a followed by b.
///
/// @param data   First byte; may be nullptr only when `bytes` is 0.
/// @param bytes  Number of bytes to hash.
/// @param crc    The CRC of everything before `data` (0 to start).
/// @return The CRC of everything up to and including this block.  A null
///         `data` with a non-zero size returns `crc` unchanged.
[[nodiscard]] inline std::uint64_t crc64(const void* data, std::size_t bytes, std::uint64_t crc = 0) noexcept {
    if (data == nullptr || bytes == 0) {
        return crc;
    }
    const auto& t = detail::kCrc64Tables;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    // The register works on the inverted value (init and final XOR ~0).
    std::uint64_t c = ~crc;

    // ---- eight bytes per step --------------------------------------------------
    // memcpy, not a pointer cast: the input has no alignment guarantee, and
    // the compiler turns this into one unaligned 64-bit load.  The byte order
    // is little-endian on every platform the project builds for (x64, arm64),
    // which is what the table indices below assume.
    while (bytes >= 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, p, sizeof(word));
        c ^= word;
        c = t[7][c & 0xFFu] ^ t[6][(c >> 8) & 0xFFu] ^ t[5][(c >> 16) & 0xFFu] ^ t[4][(c >> 24) & 0xFFu] ^
            t[3][(c >> 32) & 0xFFu] ^ t[2][(c >> 40) & 0xFFu] ^ t[1][(c >> 48) & 0xFFu] ^ t[0][c >> 56];
        p += 8;
        bytes -= 8;
    }
    // ---- the tail, one byte at a time ------------------------------------------
    while (bytes > 0) {
        c = t[0][(c ^ *p) & 0xFFu] ^ (c >> 8);
        ++p;
        --bytes;
    }
    return ~c;
}

}  // namespace osv
