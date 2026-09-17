// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_pipl.cpp - reading the PiPL back out of the built .aex, and proving
// the match name is the one the importer advertises.
//
// This is the only test that looks at what PREMIERE looks at.  Everything
// else drives the code; the host, before it calls a single function, reads
// the 'PiPL' resource with id 16000 out of the module and decides from it
// whether to load the plug-in at all, what to call it, which bin to put it
// in and which symbol to call.
//
// For THIS module there is a second, sharper reason.  The match name is the
// entire binding between the importer and the effect: Premiere compares
// imFileInfoRec8::sourceSettingsMatchName to the PiPL's
// AE_Effect_Match_Name, with no handshake and no diagnostic on a mismatch.
// One mistyped character and the Effect Controls panel simply never shows the
// stitch options, with nothing in any log to say why.  So the resource is
// parsed here and compared to plugins/common/SourceSettingsIdentity.h - the
// same header the importer reads - which is what makes the two provably one
// string rather than two spellings that happen to agree today.

#include "SourceSettingsTestSupport.h"

#include "SourceSettingsIdentity.h"
#include "SourceSettingsParams.h"

// The spec-version check compares against the AE SDK the module was built
// with, which is where PF_PLUG_IN_VERSION / PF_PLUG_IN_SUBVERS live.
#include "AE_EffectVers.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>

using namespace osv::premiere;
using namespace osv::premiere::sourcesettings;
using namespace osv::premiere::sourcesettings::test;

namespace {

/// The parsed PiPL of the loaded module, or an empty vector with the reason
/// in `error`.
[[nodiscard]] std::vector<PiplProperty> pipl(std::string* error) {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    if (!plugin.ok()) {
        if (error) {
            *error = plugin.error();
        }
        return {};
    }
    return readPipl(plugin.module(), error);
}

}  // namespace

TEST_CASE("the built module carries a parseable PiPL resource", "[sourcesettings][pipl]") {
    std::string error;
    const std::vector<PiplProperty> properties = pipl(&error);
    INFO("parse error: " << error);
    REQUIRE_FALSE(properties.empty());

    // Every property an AE-kind PiPL must have for Premiere to load the
    // effect.  A missing one is the classic "the plug-in just does not
    // appear" failure, with nothing logged anywhere.
    for (const char* key :
         {"kind", "name", "catg", "8664", "ePVR", "eSVR", "eVER", "eINF", "eGLO", "eGL2", "eMNA", "aeFL"}) {
        INFO("required PiPL property '" << key << "'");
        CHECK(findProperty(properties, key) != nullptr);
    }

    // Exactly ONE PiPL in this module.  The AE SDK lists "multiple PiPLs in a
    // single plug-in" among the features Premiere does not support, which is
    // the reason this effect is a separate .aex from Open360Reframe rather
    // than a second resource in it.
    CHECK(FindResourceW(LoadedPlugin::instance().module(), MAKEINTRESOURCEW(16001), L"PiPL") == nullptr);
}

TEST_CASE("the PiPL match name is exactly the constant the importer uses",
          "[sourcesettings][pipl][identity]") {
    std::string error;
    const std::vector<PiplProperty> properties = pipl(&error);
    INFO("parse error: " << error);
    REQUIRE_FALSE(properties.empty());

    const PiplProperty* matchName = findProperty(properties, "eMNA");
    REQUIRE(matchName != nullptr);

    // THE assertion this file exists for.  Both sides of the comparison come
    // from one header: the resource was generated from
    // OSV_SOURCE_SETTINGS_MATCH_NAME by the PiPL pipeline, and
    // kSourceSettingsMatchName is defined from the same macro and is what
    // ImporterVideo.cpp copies into sourceSettingsMatchName.  If the .r were
    // ever edited by hand to a different literal, this is where it would be
    // caught.
    CHECK(matchName->asPascalString() == std::string(kSourceSettingsMatchName));
    CHECK(matchName->asPascalString() == std::string(OSV_SOURCE_SETTINGS_MATCH_NAME));

    // And the two C++ spellings of the same string agree, narrow and wide -
    // the wide one is what goes into the prUTF16Char[256] field.
    const std::wstring wide(kSourceSettingsMatchNameW);
    std::string narrowed;
    narrowed.reserve(wide.size());
    for (const wchar_t c : wide) {
        // The match name is ASCII by construction; a non-ASCII character
        // would make the two spellings incomparable, which is itself the bug.
        REQUIRE(c > 0);
        REQUIRE(c < 128);
        narrowed.push_back(static_cast<char>(c));
    }
    CHECK(narrowed == std::string(kSourceSettingsMatchName));
}

TEST_CASE("the PiPL identity matches SourceSettingsParams.h", "[sourcesettings][pipl]") {
    std::string error;
    const std::vector<PiplProperty> properties = pipl(&error);
    REQUIRE_FALSE(properties.empty());

    SECTION("kind is an AE effect") {
        const PiplProperty* kind = findProperty(properties, "kind");
        REQUIRE(kind != nullptr);
        REQUIRE(kind->data.size() == 4u);
        // 'eFKT', stored byte-reversed by PiPLtool.
        std::string code;
        for (int i = 3; i >= 0; --i) {
            code.push_back(static_cast<char>(kind->data[static_cast<std::size_t>(i)]));
        }
        CHECK(code == "eFKT");
    }

    SECTION("the display name and category") {
        const PiplProperty* name = findProperty(properties, "name");
        const PiplProperty* category = findProperty(properties, "catg");
        REQUIRE(name != nullptr);
        REQUIRE(category != nullptr);
        CHECK(name->asPascalString() == std::string(OSV_SOURCE_SETTINGS_DISPLAY_NAME));
        // The same bin as the reframe effect, so the two halves of the
        // workflow sit together in the Effects panel.
        CHECK(category->asPascalString() == std::string(OSV_SOURCE_SETTINGS_CATEGORY));
        CHECK(category->asPascalString() == "OpenOSV");
    }

    SECTION("the entry point name is the exported symbol") {
        const PiplProperty* code = findProperty(properties, "8664");
        REQUIRE(code != nullptr);
        // A mismatch here is the single most common way a plug-in silently
        // does nothing in a real host: the module loads and the host then
        // fails to find the function it was told to call.
        CHECK(code->asCString() == "EffectMain");
        CHECK(GetProcAddress(LoadedPlugin::instance().module(), "EffectMain") != nullptr);
    }

    SECTION("the version words") {
        const PiplProperty* version = findProperty(properties, "eVER");
        REQUIRE(version != nullptr);
        CHECK(version->asUint32() == OSV_SOURCE_SETTINGS_PIPL_VERSION);

        // The PiPL structure version is 2.0 for every AE effect, stored as
        // two 16-bit words in source order.
        const PiplProperty* piplVersion = findProperty(properties, "ePVR");
        REQUIRE(piplVersion != nullptr);
        CHECK(piplVersion->asUint16(0) == 2u);
        CHECK(piplVersion->asUint16(2) == 0u);

        // The spec version describes the SDK the module was built against,
        // so it must be the SDK this test compiled with.
        const PiplProperty* spec = findProperty(properties, "eSVR");
        REQUIRE(spec != nullptr);
        CHECK(spec->asUint16(0) == static_cast<std::uint16_t>(PF_PLUG_IN_VERSION));
        CHECK(spec->asUint16(2) == static_cast<std::uint16_t>(PF_PLUG_IN_SUBVERS));
    }

    SECTION("the out-flag words are the ones GLOBAL_SETUP reports") {
        const PiplProperty* flags = findProperty(properties, "eGLO");
        const PiplProperty* flags2 = findProperty(properties, "eGL2");
        REQUIRE(flags != nullptr);
        REQUIRE(flags2 != nullptr);
        CHECK(flags->asUint32() == OSV_SOURCE_SETTINGS_OUT_FLAGS);
        CHECK(flags2->asUint32() == OSV_SOURCE_SETTINGS_OUT_FLAGS_2);
    }

    SECTION("the info flags and the reserved word") {
        const PiplProperty* info = findProperty(properties, "eINF");
        const PiplProperty* reserved = findProperty(properties, "aeFL");
        REQUIRE(info != nullptr);
        REQUIRE(reserved != nullptr);
        CHECK(info->asUint16() == OSV_SOURCE_SETTINGS_INFO_FLAGS);
        CHECK(reserved->asUint32() == OSV_SOURCE_SETTINGS_RESERVED_INFO);
    }
}

TEST_CASE("the module carries a real VERSIONINFO block", "[sourcesettings][pipl]") {
    const LoadedPlugin& plugin = LoadedPlugin::instance();
    REQUIRE(plugin.ok());

    // Without this block the file properties dialog, the installer and
    // Premiere's Plugin Loading.log all show "0.0.0.0" and a blank publisher,
    // which makes a support log useless for telling builds apart.
    const std::string path = toUtf8(plugin.path());
    const DWORD size = GetFileVersionInfoSizeW(plugin.path().c_str(), nullptr);
    INFO("module: " << path);
    REQUIRE(size > 0u);

    std::vector<std::uint8_t> block(size);
    REQUIRE(GetFileVersionInfoW(plugin.path().c_str(), 0, size, block.data()) != 0);

    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedLen = 0;
    REQUIRE(VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixedLen) != 0);
    REQUIRE(fixed != nullptr);
    CHECK(HIWORD(fixed->dwFileVersionMS) == OSV_SOURCE_SETTINGS_VERSION_MAJOR);
    CHECK(LOWORD(fixed->dwFileVersionMS) == OSV_SOURCE_SETTINGS_VERSION_MINOR);
    CHECK(HIWORD(fixed->dwFileVersionLS) == OSV_SOURCE_SETTINGS_VERSION_BUG);

    // 040904b0 = US English, Unicode, which is what the .rc declares.
    wchar_t* value = nullptr;
    UINT valueLen = 0;
    REQUIRE(VerQueryValueW(block.data(), L"\\StringFileInfo\\040904b0\\OriginalFilename",
                           reinterpret_cast<void**>(&value), &valueLen) != 0);
    REQUIRE(value != nullptr);
    CHECK(std::wstring(value) == L"OpenOSVSourceSettings.aex");
}
