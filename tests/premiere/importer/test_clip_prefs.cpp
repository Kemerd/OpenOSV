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
//   * the SDK guide describes the modal dialog route: the HOST allocates the
//     block from the size the imGetPrefs8 handshake answers.  No logged
//     session since 0.2.2 accepted that dialog for a clip with the Source
//     Settings effect, and no clip had a settings block;
//   * so the importer now offers the block itself in imGetInfo8 when the
//     host has none (newPtrClear through piSuites->memFuncs; the host then
//     owns and frees it) - the route an Adobe developer-forum thread gives
//     for an importer with a Source Settings effect.  That Premiere keeps
//     the block still needs confirming in a live session.
//
// What OpenOSV's plug-in logs of the 0.5.0 and 0.5.1 Premiere sessions show
// is that the block never existed: "TRANSLATE_PARAMS_TO_PREFS with no prefs
// buffer" in both sessions, the importer told "the host gave it no settings"
// even for a clip reopened from a saved project, and no project file holding
// any importer settings for an OpenOSV clip.
//
// The tests below pin the importer's side of that contract against the mock
// host, plus the answers the importer gives to every selector Premiere 26.2.2
// sends while importing a clip (the sequence is the one recorded in the
// importer log of a real session).

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
//  imGetInfo8 and the clip's settings block
// ===========================================================================

TEST_CASE("imGetInfo8 gives a clip with no stored settings a block of its own",
          "[importer][prefs][sourcesettings][sample]") {
    CLIP_PREFS_REQUIRE_SAMPLE_CLIP();
    DebugLogLevel debug;  // before the harness: the module reads it at imInit
    // Counted from here: every test case of this process shares one log.
    const std::string clipName = sampleClipPath().filename().string();
    const std::string announced = "imGetInfo8: '" + clipName + "' had no stored Source Settings";
    // The per-call debug lines: one for each call that found no block (and
    // handed one over), one for each call that found ours.
    const std::string handedEach = "imGetInfo8: '" + clipName +
                                   "': the host holds no settings block for the clip; handed it a new 128-byte one";
    const std::string heldOurs =
        "imGetInfo8: '" + clipName + "': the host holds a settings block for the clip (ours, applied)";
    const std::string beforeLog = importerLog();
    const std::size_t announcedBefore = countOf(beforeLog, announced);
    const std::size_t handedBefore = countOf(beforeLog, handedEach);
    const std::size_t heldBefore = countOf(beforeLog, heldOurs);
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

    // One info line says the block was handed over, naming the clip; the
    // debug lines record each call: one block handed over, then one found.
    const std::string log = importerLog();
    INFO(log);
    CHECK(countOf(log, announced) == announcedBefore + 1u);
    CHECK(countOf(log, handedEach) == handedBefore + 1u);
    CHECK(countOf(log, heldOurs) == heldBefore + 1u);
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

TEST_CASE("a stored block is never resized or rewritten by imGetInfo8, ours or not",
          "[importer][prefs][sourcesettings][hidemount][sample]") {
    CLIP_PREFS_REQUIRE_SAMPLE_CLIP();
    DebugLogLevel debug;  // before the harness: the module reads it at imInit
    // Counted from here: every test case of this process shares one log.
    const std::string clipName = sampleClipPath().filename().string();
    const std::string heldOurs =
        "imGetInfo8: '" + clipName + "': the host holds a settings block for the clip (ours, applied)";
    const std::string heldForeign =
        "imGetInfo8: '" + clipName + "': the host holds a settings block for the clip (not ours";
    const std::string beforeLog = importerLog();
    const std::size_t oursBefore = countOf(beforeLog, heldOurs);
    const std::size_t foreignBefore = countOf(beforeLog, heldForeign);

    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());

    PlugMemoryFuncsPtr mem = hostMemory(harness);
    REQUIRE(mem);
    REQUIRE(mem->newPtr);
    REQUIRE(mem->getPtrSize);
    REQUIRE(mem->disposePtr);

    SECTION("a blob stored before Hide Mount existed is adopted as it is, Hide Mount On") {
        // Every release stored 128 bytes; an older one simply left Hide
        // Mount's byte (offset 58) at the zero of the reserved tail, which
        // is On - the mask every older clip was stitched with.
        PrefsBlob stored = someStoredSettings();
        stored.hideMount = 0u;
        char* block = mem->newPtr(static_cast<csSDK_uint32>(PrefsBlob::kSize));
        REQUIRE(block);
        std::memcpy(block, &stored, PrefsBlob::kSize);

        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8WithHostPrefs(clip, info, block) == imNoErr);
        // The same allocation, the same size, the same bytes.
        CHECK(info.prefs == block);
        CHECK(mem->getPtrSize(block) == static_cast<csSDK_int32>(PrefsBlob::kSize));
        CHECK(std::memcmp(block, &stored, PrefsBlob::kSize) == 0);
        // The clip adopted it: 2K output, Hide Mount On.
        CHECK(info.vidInfo.imageWidth == 1920);
        const PrefsBlob reported = askImporter(harness, clip);
        CHECK(reported.outputSize == static_cast<std::uint8_t>(PrefsOutputSize::HD2K));
        CHECK(reported.hideMountChoice() == PrefsHideMount::On);
        // One debug line for the call, saying the block was ours.
        const std::string log = importerLog();
        INFO(log);
        CHECK(countOf(log, heldOurs) == oursBefore + 1u);
        mem->disposePtr(block);
    }

    SECTION("a full-size block that is not ours is left exactly as it is") {
        char* block = mem->newPtr(static_cast<csSDK_uint32>(PrefsBlob::kSize));
        REQUIRE(block);
        std::memset(block, 0x5A, PrefsBlob::kSize);

        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8WithHostPrefs(clip, info, block) == imNoErr);
        // Not ours: the host's memory is untouched ...
        CHECK(info.prefs == block);
        CHECK(mem->getPtrSize(block) == static_cast<csSDK_int32>(PrefsBlob::kSize));
        for (std::size_t i = 0; i < PrefsBlob::kSize; ++i) {
            CHECK(static_cast<unsigned char>(block[i]) == 0x5Au);
        }
        // ... the clip stays on the settings it started from ...
        CHECK(askImporter(harness, clip) == PrefsBlob::defaults());
        // ... and the call's debug line says the block was not ours.
        const std::string log = importerLog();
        INFO(log);
        CHECK(countOf(log, heldForeign) == foreignBefore + 1u);
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
