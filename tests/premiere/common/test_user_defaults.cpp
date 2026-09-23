// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_user_defaults.cpp - plugins/common/UserDefaults: the per-user Source
// Settings defaults file new clips start from.
//
// What is pinned here, and why each matters to a user:
//
//   * every setting survives a save / load cycle BIT FOR BIT - a default that
//     came back 0.1000000015 stops or one notch off would be a default the
//     user did not choose;
//   * the file covers every byte of PrefsBlob that holds a setting - a field
//     appended to the blob without a key would silently never become a
//     default (this is the test that fails when that happens);
//   * missing, unknown and unreadable keys, an old file, a newer file, a
//     corrupt file, comments and a BOM - the ways a real file differs from
//     the one this build writes;
//   * writes are atomic: concurrent savers and readers never see a torn file;
//   * the process-wide layer follows the file (the per-module cache), logs a
//     corrupt file once, and a stored blob always wins over the defaults.
//
// Every test that touches "the user's file" uses ScopedUserDefaultsFile, so
// the real %APPDATA%\OpenOSV\defaults.json is never read or written.

#include <catch2/catch_test_macros.hpp>

#include "PrefsBlob.h"
#include "TestLogIsolation.h"
#include "UserDefaults.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace osv::premiere;
using osv::premiere::testsupport::ScopedUserDefaultsFile;

namespace {

/// Write raw text to `path` (creating its directory), for hand-made files.
void writeText(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/// Byte-wise equality with a readable failure: the first differing offset.
[[nodiscard]] std::string firstDifference(const PrefsBlob& a, const PrefsBlob& b) {
    const auto* pa = reinterpret_cast<const unsigned char*>(&a);
    const auto* pb = reinterpret_cast<const unsigned char*>(&b);
    for (std::size_t i = 0; i < PrefsBlob::kSize; ++i) {
        if (pa[i] != pb[i]) {
            return "offset " + std::to_string(i) + ": " + std::to_string(pa[i]) + " vs " + std::to_string(pb[i]);
        }
    }
    return "identical";
}

/// Save / load through the pure layer, returning the loaded blob.
[[nodiscard]] PrefsBlob roundTrip(const PrefsBlob& blob) {
    const std::string text = userDefaultsToJson(blob);
    REQUIRE_FALSE(text.empty());
    const auto parsed = userDefaultsFromJson(text);
    REQUIRE(parsed.ok());
    return parsed.value().prefs;
}

/// A blob with every setting away from its built-in value.
[[nodiscard]] PrefsBlob everythingChanged() {
    PrefsBlob p = PrefsBlob::defaults();
    p.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
    p.look = static_cast<std::uint8_t>(PrefsLook::Standard);
    p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::HD2K);
    p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Smooth);
    p.seamSearch = 0;
    p.gainMatch = 0;
    p.setCalibrationChoice(PrefsCalibrationChoice::Underwater);
    p.flareRemoval = 0;
    p.photoSeam = static_cast<std::uint8_t>(PrefsPhotoSeam::RimOnly);
    p.setPhotoStrengthPercent(37.0);
    p.setSeamInsetDeg(4.3);
    // [WP-SEAMTOOLS] the carved seam's tweaks
    p.setSeamBlendDeg(3.25);
    p.setParallaxBlendDeg(0.8);
    p.setSeamSmoothingDeg(2.5);
    p.setNearOffsetDeg(1.37);
    p.setFarOffsetDeg(-0.42);
    p.parallax = static_cast<std::uint8_t>(PrefsParallax::Off);
    p.flowBackend = static_cast<std::uint8_t>(PrefsFlowBackend::Classical);
    p.dlogmFit = static_cast<std::uint8_t>(PrefsDlogmFit::Pocket3);
    p.exposureStops = -1.7f;
    p.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::OpenCl);
    p.directColour = static_cast<std::uint8_t>(PrefsDirectColour::MatchSource);
    p.hdrPeak = static_cast<std::uint8_t>(PrefsHdrPeak::Nits400);  // [WP-HDRPEAK]
    REQUIRE(p.sanitise());  // already clean: every value above is in range
    return p;
}

/// Collects this module's UserDefaults log lines while alive.
class CapturedLog {
public:
    CapturedLog() {
        std::lock_guard<std::mutex> guard(mutex());
        lines().clear();
        setUserDefaultsLogSink(&CapturedLog::sink);
    }
    ~CapturedLog() { setUserDefaultsLogSink(nullptr); }
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    /// Lines at `level` containing `needle`.
    [[nodiscard]] std::size_t count(UserDefaultsLogLevel level, const std::string& needle) const {
        std::lock_guard<std::mutex> guard(mutex());
        return static_cast<std::size_t>(std::count_if(lines().begin(), lines().end(), [&](const auto& l) {
            return l.first == level && l.second.find(needle) != std::string::npos;
        }));
    }

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    static std::vector<std::pair<UserDefaultsLogLevel, std::string>>& lines() {
        static std::vector<std::pair<UserDefaultsLogLevel, std::string>> v;
        return v;
    }
    static void sink(UserDefaultsLogLevel level, std::string_view message) noexcept {
        try {
            std::lock_guard<std::mutex> guard(mutex());
            lines().emplace_back(level, std::string(message));
        } catch (...) {
        }
    }
};

}  // namespace

// ===========================================================================
//  The pure layer: text <-> settings
// ===========================================================================

TEST_CASE("the defaults file round-trips every value of every setting bit for bit", "[userdefaults]") {
    SECTION("the built-in defaults") {
        const PrefsBlob d = PrefsBlob::defaults();
        const PrefsBlob back = roundTrip(d);
        INFO(firstDifference(back, d));
        CHECK(back == d);
    }
    SECTION("every setting changed at once") {
        const PrefsBlob p = everythingChanged();
        const PrefsBlob back = roundTrip(p);
        INFO(firstDifference(back, p));
        CHECK(back == p);
    }
    SECTION("every value of every enum, one field at a time") {
        const auto each = [](auto setter, int count) {
            for (int v = 0; v < count; ++v) {
                PrefsBlob p = PrefsBlob::defaults();
                setter(p, static_cast<std::uint8_t>(v));
                p.sanitise();
                const PrefsBlob back = roundTrip(p);
                INFO("value " << v << ": " << firstDifference(back, p));
                CHECK(back == p);
            }
        };
        each([](PrefsBlob& p, std::uint8_t v) { p.colorOutput = v; }, static_cast<int>(PrefsColorOutput::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.look = v; }, static_cast<int>(PrefsLook::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.outputSize = v; }, static_cast<int>(PrefsOutputSize::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.stabilization = v; }, static_cast<int>(PrefsStabilization::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.setCalibrationChoice(static_cast<PrefsCalibrationChoice>(v)); },
             static_cast<int>(PrefsCalibrationChoice::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.dlogmFit = v; }, static_cast<int>(PrefsDlogmFit::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.renderDevice = v; }, static_cast<int>(PrefsRenderDevice::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.flowBackend = v; }, static_cast<int>(PrefsFlowBackend::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.parallax = v; }, static_cast<int>(PrefsParallax::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.photoSeam = v; }, static_cast<int>(PrefsPhotoSeam::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.directColour = v; }, static_cast<int>(PrefsDirectColour::Count));
        each([](PrefsBlob& p, std::uint8_t v) { p.hdrPeak = v; }, static_cast<int>(PrefsHdrPeak::Count));  // [WP-HDRPEAK]
        each([](PrefsBlob& p, std::uint8_t v) { p.seamSearch = v; }, 2);
        each([](PrefsBlob& p, std::uint8_t v) { p.gainMatch = v; }, 2);
        each([](PrefsBlob& p, std::uint8_t v) { p.flareRemoval = v; }, 2);
    }
    SECTION("every stored strength and inset code, and awkward exposures") {
        for (int percent = 0; percent <= 100; ++percent) {
            PrefsBlob p = PrefsBlob::defaults();
            p.setPhotoStrengthPercent(percent);
            const PrefsBlob back = roundTrip(p);
            INFO("strength " << percent << ": " << firstDifference(back, p));
            CHECK(back == p);
        }
        for (int tenths = 0; tenths <= 60; ++tenths) {
            PrefsBlob p = PrefsBlob::defaults();
            p.setSeamInsetDeg(tenths / 10.0);
            const PrefsBlob back = roundTrip(p);
            INFO("inset " << tenths << " tenths: " << firstDifference(back, p));
            CHECK(back == p);
        }
        // Floats with no short decimal: the shortest round-tripping spelling
        // must bring back the very same bits.
        for (const float stops : {0.1f, -0.1f, 1.0f / 3.0f, -2.35f, 5.999f, -6.0f, 6.0f, 1e-7f, 0.0f}) {
            PrefsBlob p = PrefsBlob::defaults();
            p.exposureStops = stops;
            const PrefsBlob back = roundTrip(p);
            INFO("exposure " << stops << ": " << firstDifference(back, p));
            CHECK(back == p);
        }
    }
}

TEST_CASE("the defaults file covers every byte of the blob that holds a setting", "[userdefaults]") {
    // Offsets that hold no setting: magic / version (the file does not store
    // them, every blob gets this build's), the harness's padding bytes and
    // the reserved block.  EVERY other byte must belong to a key - so a field
    // appended to PrefsBlob without a row in UserDefaults.cpp's table fails
    // here, instead of silently never becoming a default.
    std::set<std::size_t> padding = {offsetof(PrefsBlob, padAfterCalibration), offsetof(PrefsBlob, padAfterLook),
                                     offsetof(PrefsBlob, padAfterFlare),
                                     offsetof(PrefsBlob, seamToolsPad)};  // [WP-SEAMTOOLS]
    for (std::size_t i = 0; i < sizeof(PrefsBlob::padBeforeFlare); ++i) {
        padding.insert(offsetof(PrefsBlob, padBeforeFlare) + i);
    }
    for (std::size_t i = 0; i < sizeof(PrefsBlob::photoReserved); ++i) {
        padding.insert(offsetof(PrefsBlob, photoReserved) + i);
    }
    // [WP-HDRPEAK] the other packages' bytes before hdrPeak, and its own spare.
    for (std::size_t i = 0; i < sizeof(PrefsBlob::padBeforeHdrPeak); ++i) {
        padding.insert(offsetof(PrefsBlob, padBeforeHdrPeak) + i);
    }
    padding.insert(offsetof(PrefsBlob, padAfterHdrPeak));

    std::vector<int> owners(PrefsBlob::kSize, 0);
    std::set<std::string> keys;
    for (const UserDefaultsField& f : userDefaultsFields()) {
        REQUIRE(f.key != nullptr);
        INFO("key " << f.key);
        CHECK(keys.insert(f.key).second);  // no key twice
        REQUIRE(f.size > 0);
        REQUIRE(f.offset + f.size <= PrefsBlob::kSize);
        for (std::size_t i = 0; i < f.size; ++i) {
            ++owners[f.offset + i];
        }
        for (std::size_t i = 0; i < f.extraSize; ++i) {
            ++owners[f.extraOffset + i];
        }
    }
    const std::size_t firstSetting = offsetof(PrefsBlob, colorOutput);
    const std::size_t reservedStart = offsetof(PrefsBlob, reserved);
    for (std::size_t offset = 0; offset < PrefsBlob::kSize; ++offset) {
        INFO("PrefsBlob byte " << offset);
        if (offset < firstSetting || offset >= reservedStart || padding.count(offset) != 0) {
            CHECK(owners[offset] == 0);  // no key writes magic, padding or reserved bytes
        } else {
            // Exactly one key per setting byte; zero means a field nobody saves.
            CHECK(owners[offset] == 1);
        }
    }
}

TEST_CASE("the written file is the documented, human-readable format", "[userdefaults]") {
    const std::string text = userDefaultsToJson(everythingChanged());
    // The identity and one named key per setting, in plain words.
    CHECK(text.find("\"format\": \"openosv-source-settings-defaults\"") != std::string::npos);
    CHECK(text.find("\"version\": 1") != std::string::npos);
    CHECK(text.find("\"settings\": {") != std::string::npos);
    CHECK(text.find("\"colourOutput\": \"rec709\"") != std::string::npos);
    CHECK(text.find("\"rec709Look\": \"standard\"") != std::string::npos);
    CHECK(text.find("\"outputSize\": \"1920x960\"") != std::string::npos);
    CHECK(text.find("\"stabilisation\": \"smooth\"") != std::string::npos);
    CHECK(text.find("\"calibration\": \"underwater\"") != std::string::npos);
    CHECK(text.find("\"skySeamFix\": \"rim-only\"") != std::string::npos);
    CHECK(text.find("\"skySeamStrengthPercent\": 37") != std::string::npos);
    CHECK(text.find("\"seamEdgeInsetDeg\": 4.3") != std::string::npos);
    CHECK(text.find("\"seamBlendDeg\": 3.25") != std::string::npos);  // [WP-SEAMTOOLS]
    CHECK(text.find("\"parallaxBlendDeg\": 0.8") != std::string::npos);
    CHECK(text.find("\"seamSmoothingDeg\": 2.5") != std::string::npos);
    CHECK(text.find("\"nearOffsetDeg\": 1.37") != std::string::npos);
    CHECK(text.find("\"farOffsetDeg\": -0.42") != std::string::npos);
    CHECK(text.find("\"exposureStops\": -1.7") != std::string::npos);
    CHECK(text.find("\"programMonitorColour\": \"match-source\"") != std::string::npos);
    CHECK(text.find("\"hdrPeakNits\": 400") != std::string::npos);  // [WP-HDRPEAK]
    // Plain ASCII text ending with a newline.
    CHECK(std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
    REQUIRE_FALSE(text.empty());
    CHECK(text.back() == '\n');
    // Never the raw blob: no key names a byte offset or holds 128 numbers.
    CHECK(text.find("reserved") == std::string::npos);
    CHECK(text.find("magic") == std::string::npos);
}

TEST_CASE("missing keys keep the built-in values", "[userdefaults]") {
    const auto parsed = userDefaultsFromJson(
        R"({"format": "openosv-source-settings-defaults", "version": 1, "settings": {"colourOutput": "hlg"}})");
    REQUIRE(parsed.ok());
    PrefsBlob expected = PrefsBlob::defaults();
    expected.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
    INFO(firstDifference(parsed.value().prefs, expected));
    CHECK(parsed.value().prefs == expected);
    CHECK(parsed.value().notes.empty());  // an absent key is normal, not noteworthy
}

TEST_CASE("an old file without the newer settings reads them as built-in", "[userdefaults]") {
    // What a file saved before the look, sun ghost removal, the sky seam fix
    // and Program Monitor Colour existed would hold: only the original keys.
    const auto parsed = userDefaultsFromJson(R"({
        "format": "openosv-source-settings-defaults",
        "version": 1,
        "settings": {
            "colourOutput": "rec709",
            "outputSize": "3840x1920",
            "stabilisation": "full",
            "seamSearch": false,
            "exposureMatch": true,
            "calibration": "lens-protectors",
            "dlogmCurve": "osmo360",
            "exposureStops": 0.5,
            "renderDevice": "cuda"
        }
    })");
    REQUIRE(parsed.ok());
    const PrefsBlob& p = parsed.value().prefs;
    const PrefsBlob d = PrefsBlob::defaults();
    // The file's own values...
    CHECK(p.color() == PrefsColorOutput::Rec709);
    CHECK(p.size() == PrefsOutputSize::UHD4K);
    CHECK(p.stab() == PrefsStabilization::Full);
    CHECK(p.seamSearch == 0);
    CHECK(p.calibrationChoice() == PrefsCalibrationChoice::LensGuards);
    CHECK(p.exposureStops == 0.5f);
    CHECK(p.device() == PrefsRenderDevice::Cuda);
    // ...and every newer setting at its built-in value, NOT at the zero an
    // old PROJECT blob holds (a new clip gets today's defaults).
    CHECK(p.look == d.look);
    CHECK(p.flareRemoval == d.flareRemoval);
    CHECK(p.photoSeam == d.photoSeam);
    CHECK(p.photoStrength == d.photoStrength);
    CHECK(p.seamInset == d.seamInset);
    CHECK(p.directColour == d.directColour);
    CHECK(p.parallax == d.parallax);
    CHECK(p.flowBackend == d.flowBackend);
}

TEST_CASE("unknown keys and values this build does not understand are noted and skipped", "[userdefaults]") {
    const auto parsed = userDefaultsFromJson(R"({
        "format": "openosv-source-settings-defaults",
        "version": 1,
        "settings": {
            "colourOutput": "ultraviolet",
            "seamSearch": "yes",
            "stabilisation": "FULL",
            "exposureStops": 99,
            "skySeamStrengthPercent": -20,
            "futureKnob": 3
        }
    })");
    REQUIRE(parsed.ok());
    const PrefsBlob& p = parsed.value().prefs;
    const PrefsBlob d = PrefsBlob::defaults();
    CHECK(p.colorOutput == d.colorOutput);  // an unknown word keeps the built-in
    CHECK(p.seamSearch == d.seamSearch);    // "yes" is not a boolean
    CHECK(p.stab() == PrefsStabilization::Full);  // case does not matter to a person
    CHECK(p.exposureStops == PrefsBlob::kMaxExposureStops);  // clamped, not refused
    CHECK(p.photoStrengthPercent() == 0.0);
    // Each problem is explained, so a hand edit that did nothing is visible.
    const auto& notes = parsed.value().notes;
    const auto mentions = [&notes](const std::string& needle) {
        return std::any_of(notes.begin(), notes.end(),
                           [&](const std::string& n) { return n.find(needle) != std::string::npos; });
    };
    CHECK(mentions("\"colourOutput\""));
    CHECK(mentions("\"seamSearch\""));
    CHECK(mentions("\"exposureStops\" was out of range"));
    CHECK(mentions("\"skySeamStrengthPercent\" was out of range"));
    CHECK(mentions("unknown setting \"futureKnob\""));
    CHECK_FALSE(mentions("\"stabilisation\""));
}

TEST_CASE("a newer layout, comments and a byte order mark are read", "[userdefaults]") {
    SECTION("a newer layout version keeps the keys this build knows") {
        const auto parsed = userDefaultsFromJson(
            R"({"format": "openosv-source-settings-defaults", "version": 7, "settings": {"outputSize": "2560x1280", "holography": true}})");
        REQUIRE(parsed.ok());
        CHECK(parsed.value().prefs.size() == PrefsOutputSize::QHD2560);
        CHECK(parsed.value().notes.size() == 2u);  // the version and the unknown key
    }
    SECTION("comments, a BOM and no \"format\" key") {
        const std::string text = "\xEF\xBB\xBF"
                                 "// my defaults\n"
                                 "{ /* laptop */ \"settings\": { \"renderDevice\": \"cpu\" } }";
        const auto parsed = userDefaultsFromJson(text);
        REQUIRE(parsed.ok());
        CHECK(parsed.value().prefs.device() == PrefsRenderDevice::Cpu);
        CHECK(parsed.value().notes.size() == 1u);  // "no format key"
    }
}

TEST_CASE("a document that is not ours or not JSON is refused as a whole", "[userdefaults]") {
    for (const char* text : {"", "not json at all", "[1, 2, 3]", "\"a string\"",
                             R"({"format": "something-else", "settings": {"colourOutput": "hlg"}})",
                             R"({"format": "openosv-source-settings-defaults", "settings": 5})",
                             R"({"format": 42})", R"({"settings": {"colourOutput": "hlg")"}) {
        INFO("text: " << text);
        const auto parsed = userDefaultsFromJson(text);
        CHECK_FALSE(parsed.ok());
        CHECK(parsed.code() == osv::ErrorCode::Malformed);
    }
}

// ===========================================================================
//  Files
// ===========================================================================

TEST_CASE("the defaults file is written atomically", "[userdefaults][file]") {
    ScopedUserDefaultsFile scoped;
    const std::filesystem::path path = scoped.path();

    SECTION("a save leaves exactly the file, no temporary behind, and reads back") {
        REQUIRE(writeUserDefaultsFile(path, everythingChanged()).ok());
        std::vector<std::filesystem::path> entries;
        for (const auto& e : std::filesystem::directory_iterator(scoped.directory())) {
            entries.push_back(e.path().filename());
        }
        REQUIRE(entries.size() == 1u);
        CHECK(entries[0] == L"defaults.json");
        const auto read = readUserDefaultsFile(path);
        REQUIRE(read.ok());
        CHECK(read.value().prefs == everythingChanged());
    }

    SECTION("a failed replace keeps the old target and removes its temporary") {
        // The target is a DIRECTORY: nothing can be renamed over it.
        std::filesystem::create_directories(path);
        const auto status = writeUserDefaultsFile(path, everythingChanged());
        CHECK_FALSE(status.ok());
        CHECK(std::filesystem::is_directory(path));
        std::size_t leftovers = 0;
        for (const auto& e : std::filesystem::directory_iterator(scoped.directory())) {
            if (e.path().filename().wstring().find(L".tmp-") != std::wstring::npos) {
                ++leftovers;
            }
        }
        CHECK(leftovers == 0u);
    }

    SECTION("concurrent savers and readers never see a torn file") {
        // Two blobs that serialise to different lengths, so a torn read would
        // be a parse failure or a third, mixed blob.
        const PrefsBlob a = PrefsBlob::defaults();
        const PrefsBlob b = everythingChanged();
        REQUIRE(writeUserDefaultsFile(path, a).ok());

        std::atomic<bool> stop{false};
        std::atomic<int> torn{0};
        std::atomic<int> reads{0};
        std::vector<std::thread> threads;
        for (int w = 0; w < 2; ++w) {
            threads.emplace_back([&, w] {
                for (int i = 0; i < 40; ++i) {
                    // A failed replace (another writer won the race for the
                    // name) is allowed; a torn file is not.
                    (void)writeUserDefaultsFile(path, ((i + w) % 2) ? b : a);
                }
            });
        }
        for (int r = 0; r < 2; ++r) {
            threads.emplace_back([&] {
                while (!stop.load()) {
                    const auto read = readUserDefaultsFile(path);
                    if (read.ok()) {
                        const PrefsBlob& got = read.value().prefs;
                        if (!(got == a) && !(got == b)) {
                            ++torn;
                        }
                        ++reads;
                    } else if (read.code() != osv::ErrorCode::NotFound && read.code() != osv::ErrorCode::Io) {
                        ++torn;  // Malformed: a half-written document
                    }
                }
            });
        }
        threads[0].join();
        threads[1].join();
        stop.store(true);
        threads[2].join();
        threads[3].join();
        CHECK(torn.load() == 0);
        CHECK(reads.load() > 0);
        // And the survivor is one of the two whole documents.
        const auto last = readUserDefaultsFile(path);
        REQUIRE(last.ok());
        CHECK((last.value().prefs == a || last.value().prefs == b));
    }
}

TEST_CASE("reading a missing, oversized or foreign file fails the right way", "[userdefaults][file]") {
    ScopedUserDefaultsFile scoped;
    CHECK(readUserDefaultsFile(scoped.path()).code() == osv::ErrorCode::NotFound);
    CHECK(readUserDefaultsFile({}).code() == osv::ErrorCode::InvalidArgument);
    writeText(scoped.path(), std::string(kMaxUserDefaultsFileBytes + 1u, ' '));
    CHECK(readUserDefaultsFile(scoped.path()).code() == osv::ErrorCode::Malformed);
    writeText(scoped.path(), "{\"format\": \"somebody-elses\"}");
    CHECK(readUserDefaultsFile(scoped.path()).code() == osv::ErrorCode::Malformed);
    // Removing a file that is not there is success: "no defaults" holds.
    std::filesystem::remove(scoped.path());
    CHECK(removeUserDefaultsFile(scoped.path()).ok());
}

// ===========================================================================
//  The process-wide layer
// ===========================================================================

TEST_CASE("tests never see the user's real defaults file", "[userdefaults][isolation]") {
    // isolatePluginLogs() (TestMain) pointed OPENOSV_DEFAULTS_FILE into this
    // process's private directory, which wins over %APPDATA%.
    const std::filesystem::path path = userDefaultsPath();
    REQUIRE_FALSE(path.empty());
    wchar_t appData[32768] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
    if (n > 0 && n < std::size(appData)) {
        const std::wstring real = std::filesystem::path(std::wstring(appData, n)) / L"OpenOSV" / L"defaults.json";
        CHECK(path.wstring() != real);
    }
    CHECK(path.wstring().find(L"OpenOSV-tests") != std::wstring::npos);
}

TEST_CASE("the process-wide defaults follow the file", "[userdefaults][cache]") {
    ScopedUserDefaultsFile scoped;
    CapturedLog log;

    // Nothing saved: the built-in defaults, and they say so.
    UserDefaults now = currentUserDefaults();
    CHECK(now.path == scoped.path());
    CHECK_FALSE(now.fromFile);
    CHECK(now.prefs == PrefsBlob::defaults());

    // Saved through the API: in force at once, from the file.
    const PrefsBlob mine = everythingChanged();
    REQUIRE(saveUserDefaults(mine).ok());
    now = currentUserDefaults();
    CHECK(now.fromFile);
    CHECK(now.prefs == mine);
    CHECK(userDefaults() == mine);
    CHECK(log.count(UserDefaultsLogLevel::Info, "saved to") == 1u);

    // Replaced behind this module's back (another module or process saved):
    // the modification time / size check picks the new file up.
    PrefsBlob other = PrefsBlob::defaults();
    other.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::DLogM);
    REQUIRE(writeUserDefaultsFile(scoped.path(), other).ok());
    CHECK(currentUserDefaults().prefs == other);

    // Corrupt: ignored as a whole, built-in defaults, logged ONCE.
    writeText(scoped.path(), "{ this is not json");
    for (int i = 0; i < 3; ++i) {
        now = currentUserDefaults();
        CHECK_FALSE(now.fromFile);
        CHECK(now.prefs == PrefsBlob::defaults());
    }
    CHECK(log.count(UserDefaultsLogLevel::Warn, "ignoring") == 1u);
    // A different corrupt state is a new problem, reported again.
    writeText(scoped.path(), "[\"still\", \"not\", \"ours\", \"at\", \"all\"]");
    (void)currentUserDefaults();
    (void)currentUserDefaults();
    CHECK(log.count(UserDefaultsLogLevel::Warn, "ignoring") == 2u);

    // Restored: the file is gone and new clips start from the built-in set.
    REQUIRE(saveUserDefaults(mine).ok());
    REQUIRE(resetUserDefaults().ok());
    CHECK_FALSE(std::filesystem::exists(scoped.path()));
    now = currentUserDefaults();
    CHECK_FALSE(now.fromFile);
    CHECK(now.prefs == PrefsBlob::defaults());
    // Restoring again is harmless.
    CHECK(resetUserDefaults().ok());
}

TEST_CASE("a stored blob always wins over the user defaults", "[userdefaults]") {
    ScopedUserDefaultsFile scoped;
    const PrefsBlob mine = everythingChanged();
    REQUIRE(saveUserDefaults(mine).ok());

    SECTION("a valid stored blob comes back as stored (sanitised)") {
        PrefsBlob stored = PrefsBlob::defaults();
        stored.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::UHD4K);
        bool fromDefaults = true;
        const PrefsBlob got = storedPrefsOrUserDefaults(&stored, sizeof(stored), nullptr, &fromDefaults);
        CHECK_FALSE(fromDefaults);
        CHECK(got == stored);
    }
    SECTION("no buffer, a short one, zeros or garbage mean a new clip: the user defaults") {
        std::vector<unsigned char> zeros(PrefsBlob::kSize, 0u);
        std::vector<unsigned char> garbage(PrefsBlob::kSize, 0x5Au);
        const std::pair<const void*, std::size_t> cases[] = {
            {nullptr, PrefsBlob::kSize},
            {zeros.data(), 16u},
            {zeros.data(), zeros.size()},
            {garbage.data(), garbage.size()},
        };
        for (const auto& [bytes, length] : cases) {
            UserDefaults used;
            bool fromDefaults = false;
            const PrefsBlob got = storedPrefsOrUserDefaults(bytes, length, &used, &fromDefaults);
            CHECK(fromDefaults);
            CHECK(used.fromFile);
            CHECK(used.path == scoped.path());
            CHECK(got == mine);
        }
    }
    SECTION("the null out-parameters are optional") {
        CHECK(storedPrefsOrUserDefaults(nullptr, 0, nullptr, nullptr) == mine);
    }
}

TEST_CASE("the summary names every setting in the file's own words", "[userdefaults]") {
    const std::string summary = userDefaultsSummary(everythingChanged());
    for (const UserDefaultsField& f : userDefaultsFields()) {
        INFO(f.key);
        CHECK(summary.find(f.key) != std::string::npos);
    }
    CHECK(summary.find("colourOutput rec709") != std::string::npos);
    CHECK(summary.find("calibration underwater") != std::string::npos);
}

TEST_CASE("the HDR peak is saved as its nits and only the four choices read back", "[userdefaults][hdrpeak]") {
    // [WP-HDRPEAK] Every choice round-trips as the number the panel shows.
    for (int i = 0; i < static_cast<int>(PrefsHdrPeak::Count); ++i) {
        PrefsBlob p = PrefsBlob::defaults();
        p.hdrPeak = static_cast<std::uint8_t>(i);
        const std::string text = userDefaultsToJson(p);
        const std::string expected =
            "\"hdrPeakNits\": " + std::to_string(static_cast<int>(kPrefsHdrPeakNits[static_cast<std::size_t>(i)]));
        INFO(expected);
        CHECK(text.find(expected) != std::string::npos);
        CHECK(roundTrip(p) == p);
    }
    // A number that is not one of the displays is refused (and noted), never
    // rounded to a neighbour; a string is refused the same way.
    for (const char* bad : {"800", "0", "-600", "\"600\"", "1e9"}) {
        const std::string doc = std::string(R"({"format": "openosv-source-settings-defaults", "version": 1, )") +
                                R"("settings": {"hdrPeakNits": )" + bad + "}}";
        const auto parsed = userDefaultsFromJson(doc);
        INFO(doc);
        REQUIRE(parsed.ok());
        CHECK(parsed.value().prefs.hdrPeakChoice() == PrefsHdrPeak::Nits1000);
        const auto& notes = parsed.value().notes;
        CHECK(std::any_of(notes.begin(), notes.end(),
                          [](const std::string& n) { return n.find("\"hdrPeakNits\"") != std::string::npos; }));
    }
    // 600 written as a float is still 600.
    const auto parsed = userDefaultsFromJson(
        R"({"format": "openosv-source-settings-defaults", "version": 1, "settings": {"hdrPeakNits": 600.0}})");
    REQUIRE(parsed.ok());
    CHECK(parsed.value().prefs.hdrPeakChoice() == PrefsHdrPeak::Nits600);
}
