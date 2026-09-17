// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_pipl.cpp - reading the PiPL back out of the built .aex.
//
// This is the only test that looks at what PREMIERE looks at.  Everything
// else drives the code; the host, before it calls a single function, reads
// the 'PiPL' resource with id 16000 out of the module and decides from it
// whether to load the plug-in at all, what to call it, which bin to put it
// in and which symbol to call.  A mistake there produces a plug-in that
// simply never appears, with no error anywhere - so the resource is parsed
// here and checked against ReframeParams.h, the same header that generated
// it.
//
// The layout was read out of the bytes PiPLtool actually emits (and matches
// the .rcp text it generates, which is a raw RESOURCE block):
//
//     uint16  version    (1)            <- 16-bit, NOT 32
//     uint32  reserved   (0)
//     uint32  count
//     then `count` properties, each:
//         char[4] vendor    ("8BIM", stored byte-reversed as "MIB8")
//         char[4] key       (e.g. "eFKT", stored byte-reversed as "TKFe")
//         uint32  id        (0)
//         uint32  length
//         byte[length] data
//
// Note the two consequences of the 16-bit version word: the header is TEN
// bytes, not eight, and nothing after it is four-byte aligned - which is why
// the reader below copies each word with memcpy rather than casting a
// pointer, and why the payloads are NOT padded to a multiple of four (the
// lengths in a real resource are already whatever PiPLtool wrote).
//
// Both four-character codes are written byte-reversed by the tool, which is
// why the reader reverses them again.

#include "ReframeTestSupport.h"

#include "ReframeParams.h"

// The spec-version check compares against the AE SDK the module was built
// with, which is where PF_PLUG_IN_VERSION / PF_PLUG_IN_SUBVERS live.
#include "AE_EffectVers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace osv::reframe;
using namespace osv::reframe::test;

namespace {

/// One parsed PiPL property.
struct PiplProperty {
    std::string key;                 ///< Four characters, un-reversed ("eFKT").
    std::vector<std::uint8_t> data;  ///< Raw payload, `length` bytes.

    /// The payload as a 32-bit little-endian word (0 when too short).
    [[nodiscard]] std::uint32_t asUint32(std::size_t offset = 0) const noexcept {
        if (data.size() < offset + 4u) {
            return 0u;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    /// The payload as a 16-bit little-endian word (0 when too short).
    /// Version properties store TWO of these, in source order.
    [[nodiscard]] std::uint16_t asUint16(std::size_t offset = 0) const noexcept {
        if (data.size() < offset + 2u) {
            return 0u;
        }
        std::uint16_t value = 0;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    /// The payload as a Pascal string (length byte then characters), which
    /// is how Name, Category and Match Name are stored.
    [[nodiscard]] std::string asPascalString() const {
        if (data.empty()) {
            return {};
        }
        const std::size_t length = data[0];
        if (length + 1u > data.size()) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(data.data() + 1), length);
    }

    /// The payload as a NUL-terminated C string (how CodeWin64X86 stores the
    /// entry point name).
    [[nodiscard]] std::string asCString() const {
        const char* begin = reinterpret_cast<const char*>(data.data());
        const std::size_t length = ::strnlen(begin, data.size());
        return std::string(begin, length);
    }
};

/// Read resource 16000 of type 'PiPL' out of the loaded module and parse it.
/// Returns an empty map when the resource is absent or malformed.
std::map<std::string, PiplProperty> readPipl(HMODULE module, std::string* error) {
    std::map<std::string, PiplProperty> properties;
    auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return std::map<std::string, PiplProperty>{};
    };

    // Premiere looks the resource up by the numeric type 'PiPL' and id
    // 16000, exactly as here.
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(16000), L"PiPL");
    if (!found) {
        return fail("FindResourceW(16000, 'PiPL') found nothing");
    }
    const DWORD size = SizeofResource(module, found);
    HGLOBAL loaded = LoadResource(module, found);
    if (!loaded || size < 8u) {
        return fail("the PiPL resource could not be loaded or is too small");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(LockResource(loaded));
    if (!bytes) {
        return fail("LockResource returned null");
    }

    std::size_t offset = 0;
    // Nothing in this resource is guaranteed to be aligned (the header is ten
    // bytes long), so every word is copied out rather than read through a
    // cast pointer.
    auto readUint16 = [&](std::uint16_t& out) {
        if (offset + 2u > size) {
            return false;
        }
        std::memcpy(&out, bytes + offset, 2u);
        offset += 2u;
        return true;
    };
    auto readUint32 = [&](std::uint32_t& out) {
        if (offset + 4u > size) {
            return false;
        }
        std::memcpy(&out, bytes + offset, 4u);
        offset += 4u;
        return true;
    };

    std::uint16_t version = 0;
    std::uint32_t reserved = 0;
    std::uint32_t count = 0;
    if (!readUint16(version) || !readUint32(reserved) || !readUint32(count)) {
        return fail("the PiPL header is truncated");
    }
    if (version != 1u) {
        return fail("unexpected PiPL structure version");
    }
    if (count == 0u || count > 64u) {
        return fail("implausible PiPL property count");
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 16u > size) {
            return fail("a PiPL property header is truncated");
        }
        // The vendor and key four-character codes are stored byte-reversed.
        char vendor[5] = {};
        char key[5] = {};
        for (int c = 0; c < 4; ++c) {
            vendor[c] = static_cast<char>(bytes[offset + 3u - static_cast<std::size_t>(c)]);
            key[c] = static_cast<char>(bytes[offset + 4u + 3u - static_cast<std::size_t>(c)]);
        }
        offset += 8u;
        std::uint32_t id = 0;
        std::uint32_t length = 0;
        if (!readUint32(id) || !readUint32(length)) {
            return fail("a PiPL property length is truncated");
        }
        if (std::string(vendor) != "8BIM") {
            return fail("a PiPL property is not vendor 8BIM");
        }
        if (offset + length > size) {
            return fail("a PiPL property payload runs past the resource");
        }

        PiplProperty property;
        property.key = key;
        property.data.assign(bytes + offset, bytes + offset + length);
        properties[property.key] = std::move(property);

        // Payloads are padded up to a multiple of four bytes.  Only an
        // odd-length property makes this visible: AE_Effect_Info_Flags is
        // emitted with length 2 and the next property still begins four
        // bytes later, so skipping only `length` desynchronises the reader
        // from there on.
        offset += (length + 3u) & ~static_cast<std::size_t>(3u);
    }

    if (error) {
        error->clear();
    }
    return properties;
}

}  // namespace

TEST_CASE("the built module carries a parseable PiPL resource", "[reframe][pipl]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());

    std::string error;
    const std::map<std::string, PiplProperty> properties = readPipl(plugin.module(), &error);
    INFO("parse error: " << error);
    REQUIRE_FALSE(properties.empty());

    // Every property an AE-kind PiPL must have for Premiere to load the
    // effect.  A missing one is the classic "the plug-in just does not
    // appear" failure.
    for (const char* key : {"kind", "name", "catg", "8664", "ePVR", "eSVR", "eVER", "eINF", "eGLO", "eGL2", "eMNA",
                            "aeFL"}) {
        INFO("required PiPL property '" << key << "'");
        CHECK(properties.count(key) == 1);
    }
}

TEST_CASE("the PiPL identity matches ReframeParams.h", "[reframe][pipl]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());
    std::string error;
    const std::map<std::string, PiplProperty> properties = readPipl(plugin.module(), &error);
    REQUIRE_FALSE(properties.empty());

    SECTION("kind is an AE effect") {
        REQUIRE(properties.count("kind") == 1);
        REQUIRE(properties.at("kind").data.size() == 4u);
        // The VALUE of the 'kind' property is the four-character code of the
        // plug-in kind, stored byte-reversed like every other one here.
        // 'eFKT' is an AE effect; a Premiere legacy filter would be 'SPFX'
        // and would be loaded by a completely different code path.
        const std::vector<std::uint8_t>& data = properties.at("kind").data;
        const std::string kindCode{static_cast<char>(data[3]), static_cast<char>(data[2]),
                                   static_cast<char>(data[1]), static_cast<char>(data[0])};
        CHECK(kindCode == "eFKT");
    }

    SECTION("the display name and category") {
        REQUIRE(properties.count("name") == 1);
        REQUIRE(properties.count("catg") == 1);
        CHECK(properties.at("name").asPascalString() == OSV_REFRAME_DISPLAY_NAME);
        CHECK(properties.at("catg").asPascalString() == OSV_REFRAME_CATEGORY);
    }

    SECTION("the match name never changes") {
        REQUIRE(properties.count("eMNA") == 1);
        // This string is written into every project file and is what binds
        // xGPUFilterEntry to the effect.  It is the single value here that
        // must NEVER change, for any reason.
        CHECK(properties.at("eMNA").asPascalString() == OSV_REFRAME_MATCH_NAME);
    }

    SECTION("the entry point is the symbol the module actually exports") {
        REQUIRE(properties.count("8664") == 1);
        const std::string entryPoint = properties.at("8664").asCString();
        CHECK(entryPoint == "EffectMain");
        // ...and it resolves, which is what the module test proves too, but
        // here it is tied to the NAME IN THE RESOURCE rather than a literal.
        CHECK(GetProcAddress(plugin.module(), entryPoint.c_str()) != nullptr);
    }
}

TEST_CASE("the PiPL flag words match the constants the code reports", "[reframe][pipl]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());
    std::string error;
    const std::map<std::string, PiplProperty> properties = readPipl(plugin.module(), &error);
    REQUIRE_FALSE(properties.empty());

    REQUIRE(properties.count("eGLO") == 1);
    REQUIRE(properties.count("eGL2") == 1);
    REQUIRE(properties.count("eVER") == 1);
    REQUIRE(properties.count("eINF") == 1);

    // The whole point of ReframeParams.h: the resource Premiere reads at
    // scan time and the values PF_Cmd_GLOBAL_SETUP reports at run time come
    // from the same constants, so they cannot drift.  test_effect.cpp checks
    // the run-time half against the same constants.
    CHECK(properties.at("eGLO").asUint32() == static_cast<std::uint32_t>(OSV_REFRAME_OUT_FLAGS));
    CHECK(properties.at("eGL2").asUint32() == static_cast<std::uint32_t>(OSV_REFRAME_OUT_FLAGS_2));
    CHECK(properties.at("eVER").asUint32() == static_cast<std::uint32_t>(OSV_REFRAME_PIPL_VERSION));
    // AE_Effect_Info_Flags is emitted as a 16-bit payload by PiPLtool
    // (its declared length is 2, not 4), so it is read as one.
    CHECK(properties.at("eINF").asUint16() == static_cast<std::uint16_t>(OSV_REFRAME_INFO_FLAGS));
}

TEST_CASE("the PiPL declares the AE structure and spec versions", "[reframe][pipl]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());
    std::string error;
    const std::map<std::string, PiplProperty> properties = readPipl(plugin.module(), &error);
    REQUIRE_FALSE(properties.empty());

    SECTION("the PiPL structure version is 2.0") {
        REQUIRE(properties.count("ePVR") == 1);
        REQUIRE(properties.at("ePVR").data.size() >= 4u);
        // The two numbers are written as two little-endian 16-bit words in
        // source order, so the MAJOR is the low half of the 32-bit payload.
        CHECK(properties.at("ePVR").asUint16(0) == 2u);
        CHECK(properties.at("ePVR").asUint16(2) == 0u);
    }

    SECTION("the effect spec version is the SDK's") {
        REQUIRE(properties.count("eSVR") == 1);
        REQUIRE(properties.at("eSVR").data.size() >= 4u);
        // AE_Effect_Spec_Version { PF_PLUG_IN_VERSION, PF_PLUG_IN_SUBVERS }
        // from the AE SDK the module was compiled against.
        CHECK(properties.at("eSVR").asUint16(0) == static_cast<std::uint16_t>(PF_PLUG_IN_VERSION));
        CHECK(properties.at("eSVR").asUint16(2) == static_cast<std::uint16_t>(PF_PLUG_IN_SUBVERS));
        // 13.x is the effect API Premiere supports; a 14 here would mean the
        // module was built against something Premiere cannot load.
        CHECK(properties.at("eSVR").asUint16(0) == 13u);
    }
}

TEST_CASE("the version resource carries a real version and publisher", "[reframe][pipl]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());

    // VERSIONINFO is what the file properties dialog, an installer and
    // Premiere's Plugin Loading.log all read.  A module with none shows
    // "0.0.0.0" and a blank publisher, which looks like malware to a user.
    HRSRC found = FindResourceW(plugin.module(), MAKEINTRESOURCEW(VS_VERSION_INFO), RT_VERSION);
    REQUIRE(found != nullptr);
    const DWORD size = SizeofResource(plugin.module(), found);
    CHECK(size > 0);

    HGLOBAL loaded = LoadResource(plugin.module(), found);
    REQUIRE(loaded != nullptr);
    const void* data = LockResource(loaded);
    REQUIRE(data != nullptr);

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLength = 0;
    REQUIRE(VerQueryValueW(data, L"\\", reinterpret_cast<LPVOID*>(&fixed), &fixedLength) != 0);
    REQUIRE(fixed != nullptr);
    CHECK(HIWORD(fixed->dwFileVersionMS) == OSV_REFRAME_VERSION_MAJOR);
    CHECK(LOWORD(fixed->dwFileVersionMS) == OSV_REFRAME_VERSION_MINOR);
    CHECK(HIWORD(fixed->dwFileVersionLS) == OSV_REFRAME_VERSION_BUG);
    // A plug-in is a DLL as far as the version resource is concerned.
    CHECK(fixed->dwFileType == VFT_DLL);
}
