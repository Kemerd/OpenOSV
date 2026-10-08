// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_clip_prefs.cpp - the clip's stored Source Settings block, as the BUILT
// OpenOSVImporter.prm hands it to the host and reads it back.
//
// Why this file exists
// --------------------
// Premiere keeps one opaque settings block per clip ("prefs") and is the only
// party that stores it in the project.  It hands that block to the importer
// in imGetInfo8 and to the Source Settings effect in
// PF_Cmd_TRANSLATE_PARAMS_TO_PREFS, where the effect writes the controls into
// it.  The block only exists once something has allocated it:
//
//   * the modal dialog route (imGetPrefs8) makes the HOST allocate it, after
//     the size handshake - but Premiere never sends imGetPrefs8 to a clip
//     whose settings live in a master-clip Source Settings effect;
//   * so with the effect, the IMPORTER must allocate it in imGetInfo8 when
//     the host has none (newPtr through piSuites->memFuncs; the host then
//     owns and frees it).
//
// Until this was done the block never existed: Premiere's own logs show
// "TRANSLATE_PARAMS_TO_PREFS with no prefs buffer" in every session, the
// importer was told "the host gave it no settings" even for a clip reopened
// from a saved project, and no project file holds any importer settings.
//
// The tests below pin the whole contract, plus the answers the importer gives
// to every selector Premiere 26.2.2 sends while importing a clip (the
// sequence is the one recorded in the importer log of a real session).

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"
#include "ImporterPlugin.h"
#include "MockHost.h"

#include "PrefsBlob.h"
#include "SourceSettingsIdentity.h"
#include "TestLogIsolation.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::test;

namespace {

#define CLIP_PREFS_REQUIRE_SAMPLE_CLIP()                                                                 \
    do {                                                                                                \
        if (!sampleClipAvailable()) {                                                                   \
            SKIP("the sample clip is not present at " << sampleClipPath().string());                    \
        }                                                                                               \
    } while (false)

/// Raise the plug-in log to DEBUG for the lifetime of the object, so the
/// prefs selectors' lines are written.  Must exist BEFORE the harness: the
/// module reads the level once, when imInit initialises its log.
class DebugLogLevel {
public:
    DebugLogLevel() {
        char* old = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&old, &length, "OSV_PLUGIN_LOG_LEVEL") == 0 && old) {
            m_previous = old;
            m_hadPrevious = true;
        }
        std::free(old);
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "debug");
    }
    ~DebugLogLevel() { ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", m_hadPrevious ? m_previous.c_str() : ""); }
    DebugLogLevel(const DebugLogLevel&) = delete;
    DebugLogLevel& operator=(const DebugLogLevel&) = delete;

private:
    std::string m_previous;
    bool m_hadPrevious = false;
};

/// The importer's log for this process (LOCALAPPDATA is the isolated one the
/// test main set up, never the user's real profile).
[[nodiscard]] std::string importerLog() {
    wchar_t buffer[32768] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return {};
    }
    std::ifstream in(std::filesystem::path(std::wstring(buffer, n)) / L"OpenOSV" / L"OpenOSVImporter.log",
                     std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string();
}

/// Occurrences of `needle` in `hay`.
[[nodiscard]] std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); !needle.empty() && pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

/// imPerformSourceSettingsCommand on a live clip: what the importer says the
/// clip is decoded with (the Source Settings effect's view of it).
[[nodiscard]] PrefsBlob askImporter(ImporterHarness& harness, ImporterHarness::ClipHandle& clip) {
    PrefsBlob request = PrefsBlob::defaults();
    std::vector<char> buffer(PrefsBlob::kSize, 0);
    std::memcpy(buffer.data(), &request, PrefsBlob::kSize);
    imSourceSettingsCommandRec rec{};
    rec.ioData = buffer.data();
    rec.inDataSize = static_cast<csSDK_int32>(PrefsBlob::kSize);
    rec.inPrivateData = clip.privateData();
    imFileAccessRec8 access{};
    REQUIRE(harness.send(imPerformSourceSettingsCommand, &access, &rec) == imNoErr);
    return PrefsBlob::fromBytes(buffer.data(), buffer.size());
}

/// The host's memory functions (the mock's), the ones Premiere would use.
[[nodiscard]] PlugMemoryFuncsPtr hostMemory(ImporterHarness& harness) {
    imStdParms& std = harness.stdParms();
    return std.piSuites ? std.piSuites->memFuncs : nullptr;
}

/// A clip's stored settings that differ from the defaults in fields the
/// advertised geometry shows (output size) and in fields it does not.
[[nodiscard]] PrefsBlob someStoredSettings() {
    PrefsBlob p = PrefsBlob::defaults();
    p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);
    p.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
    p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Full);
    p.exposureStops = -0.5f;
    p.lensFocal = static_cast<std::uint8_t>(PrefsLensFocal::Camera);
    REQUIRE(p.sanitise());
    return p;
}

}  // namespace

// ===========================================================================
//  PrefsBlob::fromStoredBytes - the pure rule under the upgrade
// ===========================================================================

TEST_CASE("PrefsBlob fromStoredBytes accepts a shorter blob of ours and reads the missing tail as zero",
          "[importer][prefs][hidemount]") {
    PrefsBlob stored = PrefsBlob::defaults();
    stored.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    stored.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);
    stored.lensFocal = static_cast<std::uint8_t>(PrefsLensFocal::Calibration);
    stored.hideMount = static_cast<std::uint8_t>(PrefsHideMount::Off);
    REQUIRE(stored.sanitise());

    SECTION("a full blob reads exactly like fromBytes") {
        bool ours = false;
        bool upgraded = true;
        const PrefsBlob read = PrefsBlob::fromStoredBytes(&stored, sizeof(stored), &ours, &upgraded);
        CHECK(ours);
        CHECK_FALSE(upgraded);
        CHECK(read == stored);
        CHECK(read == PrefsBlob::fromBytes(&stored, sizeof(stored)));
    }

    SECTION("a blob that ends before Hide Mount keeps its bytes and reads Hide Mount as On") {
        // Even though the full blob says Off: the shorter block simply does
        // not reach byte 58, and a missing byte is the old behaviour.
        bool ours = false;
        bool upgraded = false;
        const std::size_t length = offsetof(PrefsBlob, hideMount);
        const PrefsBlob read = PrefsBlob::fromStoredBytes(&stored, length, &ours, &upgraded);
        CHECK(ours);
        CHECK(upgraded);
        CHECK(std::memcmp(&read, &stored, length) == 0);
        CHECK(read.hideMountChoice() == PrefsHideMount::On);
        CHECK(read.lensFocalChoice() == PrefsLensFocal::Calibration);
        // Already clean: nothing for the importer to repair.
        PrefsBlob copy = read;
        CHECK(copy.sanitise());
    }

    SECTION("the first release's 20-byte blob is the shortest one accepted") {
        bool ours = false;
        const PrefsBlob read = PrefsBlob::fromStoredBytes(&stored, PrefsBlob::kOriginalFieldsSize, &ours);
        CHECK(ours);
        CHECK(read.colorOutput == stored.colorOutput);
        CHECK(read.outputSize == stored.outputSize);
        // Every later field at its zero meaning, e.g. Scene Light Auto.
        CHECK(read.sceneLightChoice() == PrefsSceneLight::Auto);
        CHECK(read.hideMountChoice() == PrefsHideMount::On);

        bool shortOurs = true;
        CHECK(PrefsBlob::fromStoredBytes(&stored, PrefsBlob::kOriginalFieldsSize - 1u, &shortOurs) ==
              PrefsBlob::defaults());
        CHECK_FALSE(shortOurs);
    }

    SECTION("null, foreign or garbage blocks give the defaults and say so") {
        bool ours = true;
        CHECK(PrefsBlob::fromStoredBytes(nullptr, 64u, &ours) == PrefsBlob::defaults());
        CHECK_FALSE(ours);
        std::vector<char> garbage(64u, 0x5A);
        ours = true;
        CHECK(PrefsBlob::fromStoredBytes(garbage.data(), garbage.size(), &ours) == PrefsBlob::defaults());
        CHECK_FALSE(ours);
        // A longer block contributes only its first kSize bytes.
        std::vector<char> longer(PrefsBlob::kSize + 32u, 0x77);
        std::memcpy(longer.data(), &stored, PrefsBlob::kSize);
        ours = false;
        CHECK(PrefsBlob::fromStoredBytes(longer.data(), longer.size(), &ours) == stored);
        CHECK(ours);
    }
}

// ===========================================================================
//  imGetInfo8 and the clip's settings block
// ===========================================================================

TEST_CASE("imGetInfo8 gives a clip with no stored settings a block of its own",
          "[importer][prefs][sourcesettings][sample]") {
    CLIP_PREFS_REQUIRE_SAMPLE_CLIP();
    DebugLogLevel debug;  // before the harness: the module reads it at imInit
    // Counted from here: every test case of this process shares one log.
    const std::string announced =
        "imGetInfo8: '" + sampleClipPath().filename().string() + "' had no stored Source Settings";
    const std::size_t announcedBefore = countOf(importerLog(), announced);
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    // The host holds nothing for a newly imported clip.
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);

    // Measured value for the report: how many bytes the host holds for the
    // clip after its first imGetInfo8.
    WARN("host settings block after imGetInfo8 with none: " << harness.lastHostPrefs().size() << " bytes");

    // Exactly one blob, allocated through the host's memory functions (the
    // harness copied it out and freed it the way Premiere frees it).
    REQUIRE(harness.lastHostPrefs().size() == PrefsBlob::kSize);
    PrefsBlob handed{};
    std::memcpy(&handed, harness.lastHostPrefs().data(), PrefsBlob::kSize);
    CHECK(handed.isValid());
    // Already clean: nothing the effect or a later imGetInfo8 has to repair.
    PrefsBlob copy = handed;
    CHECK(copy.sanitise());
    // It holds what the clip is decoded with - here the built-in defaults,
    // because the test process has no user defaults file - which is also
    // what the Source Settings effect's controls are seeded with.
    CHECK(handed == PrefsBlob::defaults());
    CHECK(handed == askImporter(harness, clip));

    // A second imGetInfo8 with that block handed back reads it and keeps it:
    // the clip is then a clip with stored settings, nothing new is allocated.
    PlugMemoryFuncsPtr mem = hostMemory(harness);
    REQUIRE(mem);
    REQUIRE(mem->newPtr);
    char* stored = mem->newPtr(static_cast<csSDK_uint32>(PrefsBlob::kSize));
    REQUIRE(stored);
    std::memcpy(stored, &handed, PrefsBlob::kSize);
    imFileInfoRec8 again{};
    REQUIRE(harness.getInfo8WithHostPrefs(clip, again, stored) == imNoErr);
    CHECK(again.prefs == stored);
    CHECK(std::memcmp(stored, &handed, PrefsBlob::kSize) == 0);
    mem->disposePtr(static_cast<char*>(again.prefs));

    // One line says the block was handed over, naming the clip.
    const std::string log = importerLog();
    INFO(log);
    CHECK(countOf(log, announced) == announcedBefore + 1u);
}

TEST_CASE("a settings block the host hands back is read and left in place",
          "[importer][prefs][sourcesettings][sample]") {
    CLIP_PREFS_REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    // A full block, allocated the way Premiere allocates the clip's prefs.
    PlugMemoryFuncsPtr mem = hostMemory(harness);
    REQUIRE(mem);
    const PrefsBlob stored = someStoredSettings();
    char* block = mem->newPtr(static_cast<csSDK_uint32>(PrefsBlob::kSize));
    REQUIRE(block);
    std::memcpy(block, &stored, PrefsBlob::kSize);

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8WithHostPrefs(clip, info, block) == imNoErr);

    // Same allocation, same bytes: a stored blob is the clip's settings and
    // is never rewritten by imGetInfo8.
    CHECK(info.prefs == block);
    CHECK(mem->getPtrSize(block) == static_cast<csSDK_int32>(PrefsBlob::kSize));
    CHECK(std::memcmp(block, &stored, PrefsBlob::kSize) == 0);
    // And the clip adopted it: 2K output, and the effect is told so.
    CHECK(info.vidInfo.imageWidth == 1920);
    CHECK(info.vidInfo.imageHeight == 960);
    CHECK(askImporter(harness, clip) == stored);
    mem->disposePtr(static_cast<char*>(info.prefs));
}

TEST_CASE("a smaller stored block of ours is accepted and upgraded",
          "[importer][prefs][sourcesettings][hidemount][sample]") {
    CLIP_PREFS_REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    PlugMemoryFuncsPtr mem = hostMemory(harness);
    REQUIRE(mem);
    REQUIRE(mem->setPtrSize);

    SECTION("a block that ends before Hide Mount reads it as On and grows to the full blob") {
        // The first 58 bytes: everything up to and including Lens Focal - a
        // blob written before Hide Mount existed.  The byte Hide Mount took
        // (offset 58) is simply not there, which must read as On, the mask
        // every older clip was stitched with.
        constexpr std::size_t kOldLength = 58;
        static_assert(kOldLength == offsetof(PrefsBlob, hideMount), "the old block ends right before Hide Mount");
        const PrefsBlob stored = someStoredSettings();
        char* block = mem->newPtr(static_cast<csSDK_uint32>(kOldLength));
        REQUIRE(block);
        std::memcpy(block, &stored, kOldLength);

        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8WithHostPrefs(clip, info, block) == imNoErr);
        REQUIRE(info.prefs != nullptr);

        // Measured value for the report: the block's size after the call.
        const csSDK_int32 grown = mem->getPtrSize(static_cast<char*>(info.prefs));
        WARN("a " << kOldLength << "-byte stored block after imGetInfo8: " << grown << " bytes");
        CHECK(grown == static_cast<csSDK_int32>(PrefsBlob::kSize));

        PrefsBlob upgraded{};
        if (grown >= static_cast<csSDK_int32>(PrefsBlob::kSize)) {
            std::memcpy(&upgraded, info.prefs, PrefsBlob::kSize);
        }
        CHECK(upgraded.isValid());
        // Every byte the old writer wrote is kept ...
        CHECK(std::memcmp(&upgraded, &stored, kOldLength) == 0);
        // ... and the missing tail reads as each later field's old meaning.
        CHECK(upgraded.hideMountChoice() == PrefsHideMount::On);
        CHECK(upgraded.hideMount == 0u);
        // The clip adopted the stored settings (2K output).
        CHECK(info.vidInfo.imageWidth == 1920);
        CHECK(askImporter(harness, clip).outputSize == static_cast<std::uint8_t>(PrefsOutputSize::HD2K));
        CHECK(askImporter(harness, clip).hideMountChoice() == PrefsHideMount::On);
        mem->disposePtr(static_cast<char*>(info.prefs));
    }

    SECTION("a smaller block that is not ours is never read past its end nor rewritten") {
        constexpr std::size_t kLength = 40;
        char* block = mem->newPtr(static_cast<csSDK_uint32>(kLength));
        REQUIRE(block);
        std::memset(block, 0x5A, kLength);

        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8WithHostPrefs(clip, info, block) == imNoErr);
        // Not ours: the host's memory is left exactly as it was ...
        CHECK(info.prefs == block);
        CHECK(mem->getPtrSize(block) == static_cast<csSDK_int32>(kLength));
        for (std::size_t i = 0; i < kLength; ++i) {
            CHECK(static_cast<unsigned char>(block[i]) == 0x5Au);
        }
        // ... and the clip stays on the settings it started from.
        CHECK(askImporter(harness, clip) == PrefsBlob::defaults());
        mem->disposePtr(block);
    }
}

TEST_CASE("an .LRF beside its .OSV keeps following the original instead of storing its own block",
          "[importer][prefs][proxy][sample]") {
    const std::filesystem::path proxy = sampleProxyPath();
    std::error_code ec;
    if (proxy.empty() || !std::filesystem::exists(proxy, ec)) {
        SKIP("the .LRF proxy is not present at " << proxy.string());
    }
    CLIP_PREFS_REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(proxy);
    REQUIRE(clip.open());

    // Its settings come from its original's live instance every time it is
    // opened ([PROXY] in ImporterEntry.cpp).  A stored block would freeze
    // them at the moment of the first imGetInfo8, so none is handed over.
    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    CHECK(harness.lastHostPrefs().empty());
    CHECK(info.prefs == nullptr);
}

// ===========================================================================
//  The prefs size handshake (imGetPrefs8) and its log
// ===========================================================================

TEST_CASE("imGetPrefs8 answers the size handshake however the host asks", "[importer][prefs]") {
    DebugLogLevel debug;
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    imFileAccessRec8 access{};

    SECTION("no buffer at all") {
        imGetPrefsRec rec{};
        rec.prefs = nullptr;
        rec.prefsLength = 0;
        rec.firstTime = 1;
        REQUIRE(harness.send(imGetPrefs8, &access, &rec) == imNoErr);
        CHECK(rec.prefsLength == static_cast<csSDK_int32>(PrefsBlob::kSize));
    }

    SECTION("a buffer with a zero length (the SDK sample's way of asking)") {
        // The SDK's own file importer sample tells the two steps apart by
        // prefsLength == 0, not by a null pointer, so a host that asks this
        // way must get the size and an untouched buffer.
        std::vector<char> buffer(PrefsBlob::kSize, 0x11);
        imGetPrefsRec rec{};
        rec.prefs = buffer.data();
        rec.prefsLength = 0;
        REQUIRE(harness.send(imGetPrefs8, &access, &rec) == imNoErr);
        CHECK(rec.prefsLength == static_cast<csSDK_int32>(PrefsBlob::kSize));
        for (const char c : buffer) {
            CHECK(c == 0x11);
        }
    }

    // Either way the selector leaves a line in the log, so a session log
    // answers "did the host ever ask?" - which it could not before.
    const std::string log = importerLog();
    INFO(log);
    CHECK(countOf(log, "imGetPrefs8: the host asked for the settings size") >= 1u);
}

// ===========================================================================
//  The import-time selector sequence of a real Premiere 26.2.2 session
// ===========================================================================

TEST_CASE("the selectors Premiere sends while importing a clip are answered as before",
          "[importer][dispatch][sourcesettings][sample]") {
    CLIP_PREFS_REQUIRE_SAMPLE_CLIP();
    ImporterHarness harness;
    REQUIRE(harness.loaded());

    // ---- imInit: the capability flags that decide the clip's settings UI --
    const imImportInfoRec& init = harness.importInfo();
    CHECK(harness.initResult() == imIsCacheable);
    CHECK(init.hasSetup == kPrTrue);                 // the modal dialog stays
    CHECK(init.setupOnDblClk == kPrFalse);           // a double click opens the clip, not a dialog
    CHECK(init.hasSourceSettingsEffect == kPrTrue);  // the master clip effect
    CHECK(init.canValidatePrefs == kPrFalse);
    CHECK(init.hasPersistentData == kPrFalse);
    CHECK(init.addToMenu == imMenuNone);

    // ---- the sequence, in the order the session log recorded it ------------
    // 54 imGetSubTypeNames and 57 imQueryContentState arrive first; both are
    // answered imUnsupported (a non-error), with no record touched.
    CHECK(harness.send(imGetSubTypeNames, nullptr, nullptr) == imUnsupported);
    CHECK(harness.send(imQueryContentState, nullptr, nullptr) == imUnsupported);

    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    imFileInfoRec8 info{};
    REQUIRE(harness.getInfo8(clip, info) == imNoErr);
    const std::wstring advertised(reinterpret_cast<const wchar_t*>(info.sourceSettingsMatchName));
    CHECK(advertised == std::wstring(kSourceSettingsHostMatchNameW));
    // The clip leaves imGetInfo8 with a settings block the effect can write.
    CHECK(harness.lastHostPrefs().size() == PrefsBlob::kSize);

    // 67 imGetExtendedFormatInfo, 23 imGetMetaData and 85 (a selector newer
    // than the 26.0 SDK headers) follow, all unsupported.
    CHECK(harness.send(imGetExtendedFormatInfo, nullptr, nullptr) == imUnsupported);
    CHECK(harness.send(imGetMetaData, nullptr, nullptr) == imUnsupported);
    CHECK(harness.send(85, nullptr, nullptr) == imUnsupported);

    // imGetIndPixelFormat and imGetIndColorSpace describe the clip.
    imIndPixelFormatRec pixel{};
    pixel.privatedata = clip.privateData();
    CHECK(harness.sendIndexed(imGetIndPixelFormat, 0, &pixel) == imNoErr);

    // The Source Settings effect's SEQUENCE_SETUP round trip.
    CHECK(askImporter(harness, clip).isValid());
}
