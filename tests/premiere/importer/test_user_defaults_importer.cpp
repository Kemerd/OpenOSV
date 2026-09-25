// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_user_defaults_importer.cpp - [WP-DEFAULTS] the user's saved Source
// Settings defaults, as the BUILT OpenOSVImporter.prm applies them.
//
// The rule under test is short and absolute: a NEW clip - one the host holds
// no settings for - starts from the user defaults; a clip with a stored blob
// of ours keeps it, whatever the defaults file says.  It is checked at every
// place a clip gets its first settings:
//
//   * imOpenFile8 + imGetInfo8 with no blob, or with the zero-filled buffer a
//     host allocates before anything was stored (the advertised size shows
//     which settings are in force);
//   * imPerformSourceSettingsCommand, which seeds the Source Settings
//     effect's controls for a new clip;
//   * imGetPrefs8's first-time call, which opens the modal dialog;
//   * the dialog's own "Save as Default" (its mapping and its resource).
//
// Every test points OPENOSV_DEFAULTS_FILE at a private file
// (ScopedUserDefaultsFile), never the user's real %APPDATA% one.

#include <catch2/catch_test_macros.hpp>

#include "ImporterHarness.h"
#include "ImporterPlugin.h"
#include "MockHost.h"

#include "PrefsBlob.h"
#include "TestLogIsolation.h"
#include "UserDefaults.h"
#include "resource.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace osv::premiere;
using namespace osv::premiere::test;
using osv::premiere::testsupport::ScopedUserDefaultsFile;

namespace {

#define DEFAULTS_REQUIRE_SAMPLE_CLIP()                                                                  \
    do {                                                                                                \
        if (!sampleClipAvailable()) {                                                                   \
            SKIP("the sample clip is not present at " << sampleClipPath().string());                    \
        }                                                                                               \
    } while (false)

/// Raise the plug-in log to INFO for the lifetime of the object, so the
/// importer's "new clip" line is written.  Must exist BEFORE the harness:
/// the module reads the level once, when it initialises its log.
class InfoLogLevel {
public:
    InfoLogLevel() {
        char* old = nullptr;
        std::size_t length = 0;
        if (::_dupenv_s(&old, &length, "OSV_PLUGIN_LOG_LEVEL") == 0 && old) {
            m_previous = old;
            m_hadPrevious = true;
        }
        std::free(old);
        ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", "info");
    }
    ~InfoLogLevel() { ::_putenv_s("OSV_PLUGIN_LOG_LEVEL", m_hadPrevious ? m_previous.c_str() : ""); }
    InfoLogLevel(const InfoLogLevel&) = delete;
    InfoLogLevel& operator=(const InfoLogLevel&) = delete;

private:
    std::string m_previous;
    bool m_hadPrevious = false;
};

/// The importer's log for this process (LOCALAPPDATA is the isolated one).
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

/// User defaults that change the advertised size (1920 x 960 instead of the
/// sample's native 6000 x 3000), so which settings are in force is visible
/// through imGetInfo8 alone.
[[nodiscard]] PrefsBlob someUserDefaults() {
    PrefsBlob p = PrefsBlob::defaults();
    p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);
    p.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
    p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Full);
    p.setCalibrationChoice(PrefsCalibrationChoice::Native);
    p.exposureStops = -0.4f;
    REQUIRE(p.sanitise());
    return p;
}

/// A clip's stored settings, deliberately different from the defaults above.
[[nodiscard]] PrefsBlob someStoredSettings() {
    PrefsBlob p = PrefsBlob::defaults();
    p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::UHD4K);
    p.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    REQUIRE(p.sanitise());
    return p;
}

/// imPerformSourceSettingsCommand on a live clip; returns what it answered.
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

/// A no-op dialog procedure: lets the dialog manager build the template's
/// controls without running the importer's own procedure.
INT_PTR CALLBACK inertDialogProc(HWND, UINT message, WPARAM, LPARAM) { return message == WM_INITDIALOG ? TRUE : FALSE; }

}  // namespace

// ===========================================================================
//  A new clip vs a clip with stored settings
// ===========================================================================

TEST_CASE("a new clip is decoded with the user defaults and a stored blob is untouched",
          "[importer][defaults][sample]") {
    DEFAULTS_REQUIRE_SAMPLE_CLIP();
    ScopedUserDefaultsFile scoped;
    const PrefsBlob mine = someUserDefaults();
    REQUIRE(writeUserDefaultsFile(scoped.path(), mine).ok());
    InfoLogLevel info;  // before the harness: the module reads it at imInit

    ImporterHarness harness;
    REQUIRE(harness.loaded());

    SECTION("no blob at all: a new clip on the user defaults, announced once") {
        const std::string name = sampleClipPath().filename().string();
        const std::size_t announcedBefore = countOf(importerLog(), "new clip '" + name + "'");
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
        CHECK(fileInfo.vidInfo.imageWidth == 1920);
        CHECK(fileInfo.vidInfo.imageHeight == 960);
        // What the Source Settings effect's controls will be seeded with.
        CHECK(askImporter(harness, clip) == mine);

        // One log line for the clip, naming it and the file - however many
        // selectors followed.
        REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
        const std::string log = importerLog();
        INFO(log);
        CHECK(countOf(log, "new clip '" + name + "'") == announcedBefore + 1u);
        CHECK(log.find(userDefaultsPathForLog(scoped.path())) != std::string::npos);
    }

    SECTION("the zero-filled buffer of a clip with nothing stored yet is not a blob") {
        // Before this was handled, zeros were turned into the BUILT-IN
        // defaults and adopted as if the host had chosen them - the new
        // clip lost its user defaults on its very first selector.
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());
        PrefsBlob zeros;
        std::memset(&zeros, 0, sizeof(zeros));
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo, &zeros) == imNoErr);
        CHECK(fileInfo.vidInfo.imageWidth == 1920);
        CHECK(askImporter(harness, clip) == mine);
    }

    SECTION("a stored blob wins, and the clip is not announced as new") {
        // Counted from here: the sections of this test share one process and
        // therefore one log file.
        const std::size_t announcedBefore = countOf(importerLog(), "new clip '");
        const PrefsBlob stored = someStoredSettings();
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo, &stored) == imNoErr);
        CHECK(fileInfo.vidInfo.imageWidth == 3840);
        CHECK(fileInfo.vidInfo.imageHeight == 1920);
        CHECK(askImporter(harness, clip) == stored);
        CHECK(countOf(importerLog(), "new clip '") == announcedBefore);

        // Changing the defaults later changes nothing for this clip.
        PrefsBlob other = mine;
        other.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::QHD2560);
        REQUIRE(writeUserDefaultsFile(scoped.path(), other).ok());
        REQUIRE(harness.getInfo8(clip, fileInfo, &stored) == imNoErr);
        CHECK(fileInfo.vidInfo.imageWidth == 3840);
        CHECK(askImporter(harness, clip) == stored);
    }

    SECTION("a clip that gets its blob after starting on the defaults follows the blob") {
        // The new-clip path in Premiere: imGetInfo8 without a blob, then the
        // Source Settings effect translates its controls into one.
        auto clip = harness.openClip(sampleClipPath());
        REQUIRE(clip.open());
        imFileInfoRec8 fileInfo{};
        REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
        REQUIRE(fileInfo.vidInfo.imageWidth == 1920);
        const PrefsBlob stored = someStoredSettings();
        REQUIRE(harness.getInfo8(clip, fileInfo, &stored) == imNoErr);
        CHECK(fileInfo.vidInfo.imageWidth == 3840);
        CHECK(askImporter(harness, clip) == stored);
    }
}

TEST_CASE("without a defaults file a new clip gets the built-in settings", "[importer][defaults][sample]") {
    DEFAULTS_REQUIRE_SAMPLE_CLIP();
    ScopedUserDefaultsFile scoped;  // names a file that does not exist
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    auto clip = harness.openClip(sampleClipPath());
    REQUIRE(clip.open());
    imFileInfoRec8 fileInfo{};
    REQUIRE(harness.getInfo8(clip, fileInfo) == imNoErr);
    // Native, the built-in output size: 2 x the decoded lens height.
    CHECK(fileInfo.vidInfo.imageWidth == 2 * fileInfo.vidInfo.imageHeight);
    CHECK(fileInfo.vidInfo.imageHeight > 960);
    CHECK(askImporter(harness, clip) == PrefsBlob::defaults());
}

// ===========================================================================
//  imGetPrefs8 (the modal dialog's entry)
// ===========================================================================

TEST_CASE("imGetPrefs8 opens a clip with no stored settings on the user defaults", "[importer][defaults][prefs]") {
    ScopedUserDefaultsFile scoped;
    const PrefsBlob mine = someUserDefaults();
    REQUIRE(writeUserDefaultsFile(scoped.path(), mine).ok());
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    imFileAccessRec8 access{};

    SECTION("a first-time buffer (the dialog is suppressed, so OK is immediate)") {
        std::vector<char> buffer(PrefsBlob::kSize, 0);
        imGetPrefsRec rec{};
        rec.prefs = buffer.data();
        rec.prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);
        rec.firstTime = 1;
        REQUIRE(harness.send(imGetPrefs8, &access, &rec) == imNoErr);
        CHECK(PrefsBlob::fromBytes(buffer.data(), buffer.size()) == mine);
    }
    SECTION("a clip's stored blob is kept as it is") {
        const PrefsBlob stored = someStoredSettings();
        std::vector<char> buffer(PrefsBlob::kSize, 0);
        std::memcpy(buffer.data(), &stored, PrefsBlob::kSize);
        imGetPrefsRec rec{};
        rec.prefs = buffer.data();
        rec.prefsLength = static_cast<csSDK_int32>(PrefsBlob::kSize);
        REQUIRE(harness.send(imGetPrefs8, &access, &rec) == imNoErr);
        CHECK(PrefsBlob::fromBytes(buffer.data(), buffer.size()) == stored);
    }
}

// ===========================================================================
//  The dialog's "Save as Default"
// ===========================================================================

TEST_CASE("the dialog's Save as Default stores the shown settings over the clip's hidden ones",
          "[importer][defaults][mapping]") {
    ScopedUserDefaultsFile scoped;
    // The clip the dialog was opened on, including fields it does not show.
    PrefsBlob base = PrefsBlob::defaults();
    base.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    base.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Classical);
    base.directColour = static_cast<std::uint8_t>(PrefsDirectColour::MatchSource);
    REQUIRE(base.sanitise());

    // The user changes a few shown settings, then clicks Save as Default:
    // the same mapping OK uses, on top of the same base.
    DialogControls controls = controlsFromPrefs(base);
    controls.colorOutput = static_cast<int>(PrefsColorOutput::Rec709);
    controls.look = static_cast<int>(PrefsLook::Standard);
    controls.stabilization = static_cast<int>(PrefsStabilization::Off);
    controls.calibration = static_cast<int>(PrefsCalibrationChoice::Underwater);
    controls.photoStrengthPercent = 55.0;
    const PrefsBlob saved = prefsFromControls(controls, base);
    REQUIRE(saveUserDefaults(saved).ok());

    // The file holds the shown fields as set and the hidden ones as the clip
    // had them - and a new clip gets exactly that.
    const auto read = readUserDefaultsFile(scoped.path());
    REQUIRE(read.ok());
    const PrefsBlob& got = read.value().prefs;
    CHECK(got == saved);
    CHECK(got.color() == PrefsColorOutput::Rec709);
    CHECK(got.lookChoice() == PrefsLook::Standard);
    CHECK(got.calibrationChoice() == PrefsCalibrationChoice::Underwater);
    CHECK(got.photoStrengthPercent() == 55.0);
    CHECK(got.parallaxMode() == PrefsParallax::Off);
    CHECK(got.flow() == PrefsFlowBackend::Classical);
    CHECK(got.directColourMode() == PrefsDirectColour::MatchSource);
    bool fromDefaults = false;
    CHECK(storedPrefsOrUserDefaults(nullptr, 0, nullptr, &fromDefaults) == saved);
    CHECK(fromDefaults);
}

TEST_CASE("the dialog template carries the Save as Default button on the OK row", "[importer][defaults][dialog]") {
    // The template as the built .prm carries it, built into real (hidden)
    // windows by the dialog manager with an inert procedure.
    HMODULE module = ::LoadLibraryExW(importerModulePath().c_str(), nullptr,
                                      LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    REQUIRE(module != nullptr);
    HWND dialog = ::CreateDialogParamW(module, MAKEINTRESOURCEW(IDD_SOURCE_SETTINGS), nullptr, &inertDialogProc, 0);
    REQUIRE(dialog != nullptr);

    HWND button = ::GetDlgItem(dialog, IDC_SAVE_AS_DEFAULT);
    HWND status = ::GetDlgItem(dialog, IDC_STATIC_DEFAULT_SAVED);
    HWND ok = ::GetDlgItem(dialog, IDOK);
    REQUIRE(button != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(ok != nullptr);
    wchar_t text[64] = {};
    ::GetWindowTextW(button, text, static_cast<int>(std::size(text)));
    CHECK(std::wstring(text) == L"Save as De&fault");
    // On OK's row, left of it, and not overlapping it.
    RECT b{};
    RECT o{};
    RECT s{};
    REQUIRE(::GetWindowRect(button, &b));
    REQUIRE(::GetWindowRect(ok, &o));
    REQUIRE(::GetWindowRect(status, &s));
    CHECK(b.top == o.top);
    CHECK(b.right < s.left);
    CHECK(s.right < o.left);

    ::DestroyWindow(dialog);
    ::FreeLibrary(module);
}

TEST_CASE("the dialog template carries the transfer function under Colour output", "[importer][dialog][hdrtone]") {
    // [WP-HDRTONE] The row the dialog code fills, greys and puts its tooltip
    // on, as the built .prm carries it.
    HMODULE module = ::LoadLibraryExW(importerModulePath().c_str(), nullptr,
                                      LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    REQUIRE(module != nullptr);
    HWND dialog = ::CreateDialogParamW(module, MAKEINTRESOURCEW(IDD_SOURCE_SETTINGS), nullptr, &inertDialogProc, 0);
    REQUIRE(dialog != nullptr);

    HWND colour = ::GetDlgItem(dialog, IDC_COLOR_OUTPUT);
    HWND tone = ::GetDlgItem(dialog, IDC_HDR_TONE);
    HWND label = ::GetDlgItem(dialog, IDC_STATIC_HDR_TONE);
    HWND look = ::GetDlgItem(dialog, IDC_REC709_LOOK);
    REQUIRE(colour != nullptr);
    REQUIRE(tone != nullptr);
    REQUIRE(label != nullptr);
    REQUIRE(look != nullptr);
    wchar_t text[64] = {};
    ::GetWindowTextW(label, text, static_cast<int>(std::size(text)));
    CHECK(std::wstring(text) == L"Transfer (HDR):");
    // One row under Colour output, one row above the Rec.709 look, aligned
    // with both, and nothing overlapping.
    RECT c{};
    RECT t{};
    RECT l{};
    REQUIRE(::GetWindowRect(colour, &c));
    REQUIRE(::GetWindowRect(tone, &t));
    REQUIRE(::GetWindowRect(look, &l));
    CHECK(t.left == c.left);
    CHECK(t.left == l.left);
    CHECK(t.top > c.top);
    CHECK(l.top > t.top);

    ::DestroyWindow(dialog);
    ::FreeLibrary(module);
}
