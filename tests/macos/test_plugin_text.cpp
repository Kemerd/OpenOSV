// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The plug-ins' text plumbing on macOS: the host's UTF-16 strings against
// UTF-8 paths and UTF-32 wide strings (HostUtf16.h), and the locale-free
// number parsing of the importer's disk caches (NumberParse.h).

#include <catch2/catch_test_macros.hpp>

#include "HostUtf16.h"
#include "NumberParse.h"

#include <charconv>
#include <clocale>
#include <cstring>
#include <string>

using namespace osv::premiere;

TEST_CASE("a host UTF-16 path becomes the native path, surrogate pairs included", "[macos][plugins][text]") {
    // "/Users/me/Ünïcödé 🎥/clip.OSV": Latin-1 letters and an emoji (a pair).
    const char16_t text[] = u"/Users/me/Ünïcödé \U0001F3A5/clip.OSV";
    const std::filesystem::path path = pathFromHostUtf16(text);
    CHECK(path.string() == "/Users/me/\xC3\x9Cn\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 \xF0\x9F\x8E\xA5/clip.OSV");
    CHECK(path.filename().string() == "clip.OSV");

    // The wide form is UTF-32 here: the emoji is ONE wchar_t.
    const std::wstring wide = wideFromHostUtf16(text);
    CHECK(wide == L"/Users/me/Ünïcödé \U0001F3A5/clip.OSV");

    // And back to exactly the host's units.
    CHECK(hostUtf16FromWide(wide) == std::u16string(text));

    // Null and empty are empty, never a crash.
    CHECK(pathFromHostUtf16(static_cast<const char16_t*>(nullptr)).empty());
    CHECK(wideFromHostUtf16(u"").empty());
}

TEST_CASE("malformed host UTF-16 becomes U+FFFD, never a truncated path", "[macos][plugins][text]") {
    const char16_t loneHigh[] = {u'a', 0xD83C, u'b', 0};
    const char16_t loneLow[] = {u'a', 0xDFA5, u'b', 0};
    CHECK(wideFromHostUtf16(loneHigh) == L"a�b");
    CHECK(wideFromHostUtf16(loneLow) == L"a�b");
    CHECK(pathFromHostUtf16(loneHigh).string() == "a\xEF\xBF\xBD" "b");
}

TEST_CASE("copying into a host field truncates cleanly", "[macos][plugins][text]") {
    // Room for 3 units + NUL: "ab" then the emoji's pair would not fit, and
    // half a pair must not be written.
    char16_t field[4];
    copyWideToHostUtf16(field, 4, std::wstring(L"ab\U0001F3A5"));
    CHECK(field[0] == u'a');
    CHECK(field[1] == u'b');
    CHECK(field[2] == 0);
    CHECK(field[3] == 0);

    // A field that fits is exact and terminated.
    char16_t roomy[16];
    copyWideToHostUtf16(roomy, 16, std::wstring(L"x\U0001F3A5y"));
    CHECK(std::u16string(roomy) == u"x\U0001F3A5y");

    // Degenerate fields are left alone.
    copyWideToHostUtf16(static_cast<char16_t*>(nullptr), 4, std::wstring(L"x"));
    char16_t one[1] = {u'z'};
    copyWideToHostUtf16(one, 1, std::wstring(L"x"));
    CHECK(one[0] == 0);
}

TEST_CASE("cache numbers read back exactly and only in to_chars' own spelling", "[macos][plugins][text]") {
    // Round trip: whatever to_chars wrote, parseWholeNumber reads bit for bit.
    for (const double v : {0.0, 0.5, -1.25, 3.141592653589793, 1e-12, 12345.678901, 2.2250738585072014e-308}) {
        char buf[64] = {};
        const auto printed = std::to_chars(buf, buf + sizeof(buf), v);
        REQUIRE(printed.ec == std::errc{});
        double back = -99.0;
        INFO(buf);
        REQUIRE(parseWholeNumber(buf, printed.ptr, back));
        CHECK(back == v);
    }
    for (const float v : {0.1f, 0.2f, 1.0f / 3.0f, 16777217.0f, -0.0f}) {
        char buf[64] = {};
        const auto printed = std::to_chars(buf, buf + sizeof(buf), v);
        REQUIRE(printed.ec == std::errc{});
        float back = -99.0f;
        INFO(buf);
        REQUIRE(parseWholeNumber(buf, printed.ptr, back));
        CHECK(std::memcmp(&back, &v, sizeof(v)) == 0);
    }

    // What from_chars refuses is refused here too.
    const auto refuse = [](const char* s) {
        double d = 0.0;
        return !parseWholeNumber(s, s + std::strlen(s), d);
    };
    CHECK(refuse(""));
    CHECK(refuse(" 1"));
    CHECK(refuse("+1"));
    CHECK(refuse("0x10"));
    CHECK(refuse("-0x10"));
    CHECK(refuse("1.5x"));
    CHECK(refuse("1e999"));

    // Integers, and a float spelling refused as one.
    int i = 0;
    const char* forty = "-42";
    CHECK(parseWholeNumber(forty, forty + 3, i));
    CHECK(i == -42);
    const char* frac = "3.5";
    CHECK_FALSE(parseWholeNumber(frac, frac + 3, i));
}

TEST_CASE("cache numbers do not depend on the process locale", "[macos][plugins][text]") {
    // A host may switch the C locale to one with a decimal comma; strtod
    // would then stop at the '.', parseWholeNumber must not.
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous ? previous : "C";
    const bool switched = std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr ||
                          std::setlocale(LC_NUMERIC, "fr_FR.UTF-8") != nullptr;
    const char* text = "0.625";
    double v = 0.0;
    const bool ok = parseWholeNumber(text, text + std::strlen(text), v);
    std::setlocale(LC_NUMERIC, saved.c_str());
    INFO("comma locale available: " << switched);
    CHECK(ok);
    CHECK(v == 0.625);
}
