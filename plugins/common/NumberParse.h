// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// NumberParse.h - locale-independent parsing of one whole number.
//
// The importer's disk caches (the steady seam's rotations, the lens protector
// check) write their numbers with std::to_chars and read them back with
// std::from_chars: neither depends on the host process's locale, which a
// host is free to change (a German locale turns "0.5" into an error for
// strtod).  Apple's libc++ has floating-point to_chars but no floating-point
// from_chars, so on a standard library without it the floating-point case
// goes through strtod_l in the "C" locale instead - the same answer for
// every string to_chars writes - with the extra spellings strtod accepts
// and from_chars does not (leading blanks, a '+', hexadecimal) refused, so
// both paths accept exactly the same inputs.  Integers always use
// from_chars, which every library has.
#pragma once

#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>
#include <type_traits>

#if !defined(__cpp_lib_to_chars) && !defined(_WIN32)
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#endif

namespace osv::premiere {

/// Parse all of [first, last) as a T; false on anything but a complete,
/// in-range parse (and on null / empty input).
template <class T>
[[nodiscard]] bool parseWholeNumber(const char* first, const char* last, T& out) noexcept {
    if (!first || !last || first >= last) {
        return false;
    }
#if !defined(__cpp_lib_to_chars) && !defined(_WIN32)
    if constexpr (std::is_floating_point_v<T>) {
        // from_chars refuses these; strtod would take them.
        const char c0 = *first;
        if (c0 == ' ' || c0 == '\t' || c0 == '\n' || c0 == '\r' || c0 == '\f' || c0 == '\v' || c0 == '+') {
            return false;
        }
        const char* digits = (c0 == '-') ? first + 1 : first;
        if (last - digits >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
            return false;
        }
        try {
            static const locale_t cLocale = ::newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
            if (!cLocale) {
                return false;
            }
            const std::string text(first, last);  // strtod needs a terminator
            char* end = nullptr;
            errno = 0;
            // One rounding, straight to T, as from_chars does: a float read
            // through a double could land one ulp off.
            T value{};
            if constexpr (std::is_same_v<T, float>) {
                value = ::strtof_l(text.c_str(), &end, cLocale);
            } else if constexpr (std::is_same_v<T, double>) {
                value = ::strtod_l(text.c_str(), &end, cLocale);
            } else {
                value = static_cast<T>(::strtold_l(text.c_str(), &end, cLocale));
            }
            if (end != text.c_str() + text.size() || errno == ERANGE) {
                return false;
            }
            out = value;
            return true;
        } catch (...) {
            return false;
        }
    } else {
        const auto [ptr, ec] = std::from_chars(first, last, out);
        return ec == std::errc{} && ptr == last;
    }
#else
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
#endif
}

}  // namespace osv::premiere
