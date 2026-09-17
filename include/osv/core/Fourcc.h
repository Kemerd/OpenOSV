// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Fourcc: a four-character code as used by ISO BMFF box types ('moov'),
// sample entries ('hvc1') and DJI's private boxes ('camd', 'djmd').
// Stored big-endian-as-integer so comparisons are a single compare.
#pragma once

#include <cstdint>
#include <string>

namespace osv {

struct Fourcc {
    std::uint32_t v = 0;

    constexpr Fourcc() noexcept = default;
    constexpr explicit Fourcc(std::uint32_t value) noexcept : v(value) {}

    /// Build from a 4-character literal: Fourcc{"moov"}.
    constexpr Fourcc(const char (&s)[5]) noexcept  // NOLINT(google-explicit-constructor)
        : v((static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[0])) << 24) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[1])) << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[2])) << 8) |
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[3]))) {}

    /// Build from four individual bytes.
    static constexpr Fourcc fromBytes(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept {
        return Fourcc{(static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16) |
                      (static_cast<std::uint32_t>(c) << 8) | d};
    }

    constexpr bool operator==(Fourcc other) const noexcept { return v == other.v; }
    constexpr bool operator!=(Fourcc other) const noexcept { return v != other.v; }
    constexpr bool operator<(Fourcc other) const noexcept { return v < other.v; }

    /// Printable form.  Non-ASCII bytes (e.g. the (c) in '©too') are rendered
    /// as '?' so console output stays 7-bit clean.
    [[nodiscard]] std::string str() const {
        std::string s(4, '?');
        const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(v >> 24), static_cast<std::uint8_t>(v >> 16),
                                       static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v)};
        for (int i = 0; i < 4; ++i) {
            if (bytes[i] >= 0x20 && bytes[i] < 0x7F) {
                s[static_cast<std::size_t>(i)] = static_cast<char>(bytes[i]);
            }
        }
        return s;
    }

    /// Hex form for exact identification of non-printable codes.
    [[nodiscard]] std::string hex() const {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string s = "0x";
        for (int shift = 28; shift >= 0; shift -= 4) {
            s.push_back(kDigits[(v >> shift) & 0xF]);
        }
        return s;
    }
};

}  // namespace osv
