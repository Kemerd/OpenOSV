// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_engine_settings.cpp - [WP-SETTINGS] the Source Settings the engine
// renders a clip with, through the BUILT importer module's C ABI.
//
// Field report behind these tests: in Premiere, Exposure -1 in a clip's
// Source Settings darkened the Source monitor (the importer's own equirect)
// but not the Program monitor (the effect's direct render from the
// fisheyes).  The engine renders with its OWN instance of the file and
// learns the user's settings from Premiere's instances, so three things must
// hold, and each is pinned here the way Premiere exercises it:
//
//   * a publication reaches the engine whatever the path spelling on either
//     side - case, separators, a "\\?\" prefix, "." / "..", an 8.3 short
//     name, even a hard link: the file is keyed by its identity on disk;
//   * an OLDER importer instance still holding an old blob cannot undo what
//     a newer one published (Premiere opens a new instance per change and
//     keeps the old one around), and an instance's defaults never override a
//     blob some instance was actually given;
//   * every frame reports the settings generation it was rendered with, and
//     when the sequence's working space is the clip's own colour output the
//     frame's colour block is byte-identical to the importer's own - the
//     exactness the effect's colour rule relies on.

#include "ImporterHarness.h"

#include "OsvEngineAbi.h"

#include "osv/color/ColorParams.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cuda.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using osv::premiere::PrefsBlob;
using osv::premiere::PrefsColorOutput;
using osv::premiere::test::ImporterHarness;
using osv::premiere::test::sampleClipAvailable;
using osv::premiere::test::sampleClipPath;

namespace {

/// Premiere ticks per frame at 59.94 fps (254016000000 * 1001 / 60000).
constexpr std::int64_t kTicksPerFrame5994 = 4237833600LL;

/// The engine exports this file uses, resolved from the loaded module.
struct SettingsApi {
    OsvEngineAcquireFrameFn acquire = nullptr;
    OsvEngineReleaseFrameFn release = nullptr;
    OsvEngineQuerySettingsFn query = nullptr;
    [[nodiscard]] bool ok() const noexcept { return acquire && release && query; }
};

[[nodiscard]] SettingsApi resolveSettingsApi() {
    SettingsApi api;
    const HMODULE module = GetModuleHandleW(OSV_ENGINE_MODULE_NAME);
    if (!module) {
        return api;
    }
    api.acquire = reinterpret_cast<OsvEngineAcquireFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_ACQUIRE_FRAME));
    api.release = reinterpret_cast<OsvEngineReleaseFrameFn>(GetProcAddress(module, OSV_ENGINE_SYM_RELEASE_FRAME));
    api.query = reinterpret_cast<OsvEngineQuerySettingsFn>(GetProcAddress(module, OSV_ENGINE_SYM_QUERY_SETTINGS));
    return api;
}

/// Ask the engine what it would render `path` with; REQUIREs success.
[[nodiscard]] OsvEngineClipSettings query(const SettingsApi& api, const std::wstring& path) {
    OsvEngineClipSettings s{};
    s.structSize = sizeof(s);
    char error[256] = {};
    const std::int32_t rc = api.query(path.c_str(), &s, error, static_cast<std::int32_t>(sizeof(error)));
    INFO("query error: " << error);
    REQUIRE(rc == OSV_ENGINE_OK);
    REQUIRE(s.structSize == sizeof(OsvEngineClipSettings));
    return s;
}

/// Default blob with a given exposure.
[[nodiscard]] PrefsBlob withExposure(float stops) {
    PrefsBlob p = PrefsBlob::defaults();
    p.exposureStops = stops;
    return p;
}

/// A private CUDA context, destroyed LAST (declared before the harness, so
/// it outlives imShutdown, which is when the engine frees its decoders).
struct TestContext {
    CUcontext context = nullptr;
    std::string reason;
    TestContext() {
        if (cuInit(0) != CUDA_SUCCESS) {
            reason = "cuInit failed";
            return;
        }
        CUdevice device = 0;
        if (cuDeviceGet(&device, 0) != CUDA_SUCCESS) {
            reason = "no CUDA device";
            return;
        }
        if (cuCtxCreate(&context, 0, device) != CUDA_SUCCESS) {
            context = nullptr;
            reason = "cuCtxCreate failed";
            return;
        }
        CUcontext popped = nullptr;
        (void)cuCtxPopCurrent(&popped);
    }
    ~TestContext() {
        if (context) {
            (void)cuCtxDestroy(context);
        }
    }
    TestContext(const TestContext&) = delete;
    TestContext& operator=(const TestContext&) = delete;
};

/// A hard link to a file, removed on destruction.  Declared BEFORE the
/// harness in a test so it is removed after imShutdown closed every handle
/// the importer holds on the clip (a name cannot be deleted under a handle
/// opened without FILE_SHARE_DELETE).
struct HardLink {
    std::wstring path;
    bool made = false;
    DWORD error = 0;
    HardLink(const std::filesystem::path& link, const std::filesystem::path& target) : path(link.wstring()) {
        (void)DeleteFileW(path.c_str());  // a leftover from an aborted run
        made = CreateHardLinkW(path.c_str(), target.wstring().c_str(), nullptr) != FALSE;
        error = made ? 0 : GetLastError();
    }
    ~HardLink() {
        if (made) {
            (void)DeleteFileW(path.c_str());
        }
    }
    HardLink(const HardLink&) = delete;
    HardLink& operator=(const HardLink&) = delete;
};

/// Upper-case copy of a path.
[[nodiscard]] std::wstring upper(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(c)));
    }
    return s;
}

}  // namespace

TEST_CASE("the engine exports the Source Settings query and refuses bad arguments", "[importer][engine][settings]") {
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const SettingsApi api = resolveSettingsApi();
    REQUIRE(api.ok());
    char error[256] = {};

    OsvEngineClipSettings s{};
    s.structSize = sizeof(s);
    CHECK(api.query(nullptr, &s, error, sizeof(error)) == OSV_ENGINE_ERR_ARGUMENT);
    CHECK(api.query(L"C:\\x.OSV", nullptr, error, sizeof(error)) == OSV_ENGINE_ERR_ARGUMENT);
    CHECK(api.query(L"", &s, error, sizeof(error)) == OSV_ENGINE_ERR_ARGUMENT);
    s.structSize = 4;  // an older caller's smaller block
    CHECK(api.query(L"C:\\x.OSV", &s, error, sizeof(error)) == OSV_ENGINE_ERR_VERSION);

    // A file nobody published anything for: generation 0, and the engine's
    // defaults (PQ) as what it WOULD render.  Not an error.
    const OsvEngineClipSettings none = query(api, L"C:\\definitely\\not\\a\\clip.OSV");
    CHECK(none.generation == 0u);
    CHECK(none.clipTransfer == OSV_TRANSFER_PQ);
    CHECK(none.exposureStops == 0.0f);
    CHECK(none.fileVolume == 0u);
    CHECK(none.fileIndex == 0u);
}

TEST_CASE("Source Settings reach the engine whatever the path spelling, keyed by file identity",
          "[importer][engine][settings][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    const std::filesystem::path clipPath = std::filesystem::absolute(sampleClipPath()).lexically_normal();
    // A hard link lives on the same volume as its target; the build tree is
    // next to the sample on the reference machine.  Declared before the
    // harness (removed after imShutdown).
    const std::filesystem::path linkPath =
        std::filesystem::path(OSV_TEST_OUTPUT_DIR) / L"wp_settings_hardlink_to_sample.OSV";
    HardLink link(linkPath, clipPath);

    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const SettingsApi api = resolveSettingsApi();
    REQUIRE(api.ok());

    // Premiere's instance of the clip publishes exposure -1 at imGetInfo8.
    const OsvEngineClipSettings before = query(api, clipPath.wstring());
    auto clip = harness.openClip(clipPath);
    REQUIRE(clip.open());
    imFileInfoRec8 info{};
    PrefsBlob prefs = withExposure(-1.0f);
    prefs.directColour = static_cast<std::uint8_t>(osv::premiere::PrefsDirectColour::WorkingSpace);
    REQUIRE(harness.getInfo8(clip, info, &prefs) == imNoErr);

    const OsvEngineClipSettings canonical = query(api, clipPath.wstring());
    CHECK(canonical.generation > before.generation);
    CHECK(canonical.exposureStops == -1.0f);
    // The per-clip direct-path colour choice travels with the settings.
    CHECK(canonical.directColour == static_cast<std::uint8_t>(osv::premiere::PrefsDirectColour::WorkingSpace));
    // The identity was readable, and it is what the block reports.
    CHECK((canonical.fileVolume != 0u || canonical.fileIndex != 0u));

    // Every spelling the effect's media node could plausibly use.
    std::vector<std::wstring> spellings;
    spellings.push_back(upper(clipPath.wstring()));
    {
        std::wstring slashes = clipPath.wstring();
        std::replace(slashes.begin(), slashes.end(), L'\\', L'/');
        spellings.push_back(slashes);
    }
    spellings.push_back(L"\\\\?\\" + clipPath.wstring());
    spellings.push_back((clipPath.parent_path() / L"." / clipPath.filename()).wstring());
    spellings.push_back(
        (clipPath.parent_path() / L".." / clipPath.parent_path().filename() / clipPath.filename()).wstring());
    {
        // The 8.3 name, when the volume keeps them.
        wchar_t shortName[MAX_PATH * 2] = {};
        const DWORD n = GetShortPathNameW(clipPath.wstring().c_str(), shortName, static_cast<DWORD>(std::size(shortName)));
        if (n > 0 && n < std::size(shortName) && _wcsicmp(shortName, clipPath.wstring().c_str()) != 0) {
            spellings.emplace_back(shortName);
        } else {
            WARN("the volume keeps no 8.3 names; that spelling is not exercised");
        }
    }
    if (link.made) {
        // A different NAME for the same file: only an identity match finds it.
        spellings.push_back(link.path);
    } else {
        WARN("no hard link could be made next to the build (error " << link.error
                                                                   << "); that spelling is not exercised");
    }

    for (const std::wstring& spelling : spellings) {
        const OsvEngineClipSettings s = query(api, spelling);
        INFO("spelling: " << std::filesystem::path(spelling).string());
        CHECK(s.generation == canonical.generation);
        CHECK(s.exposureStops == -1.0f);
        CHECK(s.fileVolume == canonical.fileVolume);
        CHECK(s.fileIndex == canonical.fileIndex);
    }

    // And a DIFFERENT file (the importer module itself: it exists, so it has
    // an identity) sees nothing of it.
    const OsvEngineClipSettings other =
        query(api, osv::premiere::test::importerModulePath().wstring());
    CHECK(other.generation == 0u);
    CHECK((other.fileVolume != canonical.fileVolume || other.fileIndex != canonical.fileIndex));

    // Published through one spelling, read through another: an instance
    // opened on the hard link publishes, the canonical spelling sees it.
    if (link.made) {
        auto viaLink = harness.openClip(std::filesystem::path(link.path), 21);
        REQUIRE(viaLink.open());
        const PrefsBlob linkPrefs = withExposure(-2.5f);
        REQUIRE(harness.getInfo8(viaLink, info, &linkPrefs) == imNoErr);
        const OsvEngineClipSettings after = query(api, clipPath.wstring());
        CHECK(after.exposureStops == -2.5f);
        CHECK(after.generation == canonical.generation + 1u);
        viaLink.close();
    }
    clip.close();
}

TEST_CASE("an older importer instance cannot undo the Source Settings a newer one published",
          "[importer][engine][settings][sample]") {
    // Premiere opens a NEW importer instance with the new blob on every
    // Source Settings change and keeps older ones alive; an older one handed
    // its old blob afterwards must not flip the direct path back.
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const SettingsApi api = resolveSettingsApi();
    REQUIRE(api.ok());
    const std::wstring path = sampleClipPath().wstring();
    imFileInfoRec8 info{};

    // The first instance publishes exposure -1.
    auto older = harness.openClip(sampleClipPath(), 11);
    REQUIRE(older.open());
    const PrefsBlob a = withExposure(-1.0f);
    REQUIRE(harness.getInfo8(older, info, &a) == imNoErr);
    const OsvEngineClipSettings s1 = query(api, path);
    CHECK(s1.exposureStops == -1.0f);

    // The user changes it: Premiere opens a newer instance with -2.
    auto newer = harness.openClip(sampleClipPath(), 12);
    REQUIRE(newer.open());
    const PrefsBlob b = withExposure(-2.0f);
    REQUIRE(harness.getInfo8(newer, info, &b) == imNoErr);
    const OsvEngineClipSettings s2 = query(api, path);
    CHECK(s2.exposureStops == -2.0f);
    CHECK(s2.generation == s1.generation + 1u);

    // The OLDER instance is handed a different blob: refused.
    const PrefsBlob stale = withExposure(1.0f);
    REQUIRE(harness.getInfo8(older, info, &stale) == imNoErr);
    const OsvEngineClipSettings s3 = query(api, path);
    CHECK(s3.exposureStops == -2.0f);
    CHECK(s3.generation == s2.generation);

    // The newer instance republishing the same blob changes nothing.
    REQUIRE(harness.getInfo8(newer, info, &b) == imNoErr);
    CHECK(query(api, path).generation == s2.generation);

    // Closing the newest instance keeps what it published: the older one's
    // blob is exactly what must not come back.
    newer.close();
    const OsvEngineClipSettings s4 = query(api, path);
    CHECK(s4.exposureStops == -2.0f);
    CHECK(s4.generation == s2.generation);

    // The next change opens a yet newer instance, which wins...
    auto newest = harness.openClip(sampleClipPath(), 13);
    REQUIRE(newest.open());
    const PrefsBlob d = withExposure(0.5f);
    REQUIRE(harness.getInfo8(newest, info, &d) == imNoErr);
    const OsvEngineClipSettings s5 = query(api, path);
    CHECK(s5.exposureStops == 0.5f);
    CHECK(s5.generation == s2.generation + 1u);

    // ...and a later change on that same instance wins too.
    const PrefsBlob e = withExposure(0.25f);
    REQUIRE(harness.getInfo8(newest, info, &e) == imNoErr);
    const OsvEngineClipSettings s6 = query(api, path);
    CHECK(s6.exposureStops == 0.25f);
    CHECK(s6.generation == s5.generation + 1u);

    // An instance the host gives NO blob publishes only its defaults, which
    // never override a blob another instance was given...
    auto blobless = harness.openClip(sampleClipPath(), 14);
    REQUIRE(blobless.open());
    REQUIRE(harness.getInfo8(blobless, info, nullptr) == imNoErr);
    CHECK(query(api, path).exposureStops == 0.25f);

    // ...but once the host DOES hand it a blob - even one equal to the
    // defaults it already runs on - that is published and, being the newest,
    // wins.  (The reverted-to-defaults case: Premiere's new instance for
    // "exposure back to 0" must not be mistaken for "nothing changed".)
    const PrefsBlob defaults = PrefsBlob::defaults();
    REQUIRE(harness.getInfo8(blobless, info, &defaults) == imNoErr);
    const OsvEngineClipSettings s7 = query(api, path);
    CHECK(s7.exposureStops == 0.0f);
    CHECK(s7.generation == s6.generation + 1u);
}

TEST_CASE("each frame reports the Source Settings it was rendered with, and matches the importer's colour exactly",
          "[importer][engine][settings][cuda][sample]") {
    if (!sampleClipAvailable()) {
        SKIP("the sample clip is not present at " << sampleClipPath().string());
    }
    TestContext cuda;  // before the harness: outlives imShutdown
    if (!cuda.context) {
        SKIP("CUDA unavailable: " << cuda.reason);
    }
    ImporterHarness harness;
    REQUIRE(harness.loaded());
    const SettingsApi api = resolveSettingsApi();
    REQUIRE(api.ok());
    const std::wstring path = sampleClipPath().wstring();
    char error[512] = {};

    const auto acquire = [&](std::uint32_t frameIndex, std::int32_t transfer, OsvEngineFrame& frame) {
        OsvEngineFrameRequest r{};
        r.structSize = sizeof(r);
        r.path = path.c_str();
        r.mediaTicks = kTicksPerFrame5994 * static_cast<std::int64_t>(frameIndex);
        r.purpose = OSV_ENGINE_PURPOSE_EXACT;
        r.outputTransfer = transfer;
        r.cuContext = cuda.context;
        r.cuStream = nullptr;
        frame = OsvEngineFrame{};
        frame.structSize = sizeof(frame);
        const std::int32_t rc = api.acquire(&r, &frame, error, static_cast<std::int32_t>(sizeof(error)));
        INFO("engine error: " << error);
        REQUIRE(rc == OSV_ENGINE_OK);
        REQUIRE(frame.lease != nullptr);
    };

    // Nothing published yet: the frame says so (generation 0), which is
    // what makes the effect refuse to show settings nobody chose.
    {
        OsvEngineFrame frame{};
        acquire(1, OSV_ENGINE_TRANSFER_FROM_CLIP, frame);
        CHECK(frame.settings.structSize == sizeof(OsvEngineClipSettings));
        CHECK(frame.settings.generation == query(api, path).generation);
        api.release(frame.lease, nullptr);
    }

    // Every colour output, each published by a newer Premiere instance (as a
    // Source Settings change does), with exposure -1 throughout.
    struct Case {
        PrefsColorOutput output;
        std::int32_t transfer;          // what the settings block must report
        osv::color::OutputTransfer lib; // what the importer builds its block with
    };
    const Case cases[] = {
        {PrefsColorOutput::PQ, OSV_TRANSFER_PQ, osv::color::OutputTransfer::PQ},
        {PrefsColorOutput::HLG, OSV_TRANSFER_HLG, osv::color::OutputTransfer::HLG},
        {PrefsColorOutput::Rec709, OSV_TRANSFER_REC709, osv::color::OutputTransfer::Rec709},
        {PrefsColorOutput::DLogM, OSV_TRANSFER_PASSTHROUGH, osv::color::OutputTransfer::Passthrough},
    };
    std::vector<ImporterHarness::ClipHandle> instances;
    csSDK_int32 importerId = 30;
    std::uint32_t frameIndex = 2;
    for (const Case& c : cases) {
        INFO("colour output " << static_cast<int>(c.output));
        instances.push_back(harness.openClip(sampleClipPath(), importerId++));
        REQUIRE(instances.back().open());
        PrefsBlob prefs = withExposure(-1.0f);
        prefs.colorOutput = static_cast<std::uint8_t>(c.output);
        imFileInfoRec8 info{};
        REQUIRE(harness.getInfo8(instances.back(), info, &prefs) == imNoErr);
        const OsvEngineClipSettings published = query(api, path);
        CHECK(published.clipTransfer == c.transfer);
        CHECK(published.colorOutput == static_cast<std::uint8_t>(c.output));
        CHECK(published.exposureStops == -1.0f);

        // The clip's own colour block: its transfer is the importer's own
        // mapping of the colour output, which the settings block must agree
        // with (the two mappings live in different files).
        OsvEngineFrame own{};
        acquire(frameIndex++, OSV_ENGINE_TRANSFER_FROM_CLIP, own);
        CHECK(own.settings.generation == published.generation);
        CHECK(own.settings.clipTransfer == c.transfer);
        CHECK(own.settings.exposureStops == -1.0f);
        CHECK(own.stitch.color.transfer == c.transfer);

        // ...and it is exactly the library's block for the settings, i.e.
        // exactly what the importer's equirect route encodes with.
        const OsvColorParams expected = osv::color::makeColorParams(
            osv::color::kDefaultDlogMFit, c.lib, -1.0f, osv::color::InputEncoding::DLogM, true, 10u);
        CHECK(std::memcmp(&own.stitch.color, &expected, sizeof(OsvColorParams)) == 0);
        if (c.transfer != OSV_TRANSFER_PASSTHROUGH) {
            CHECK(own.stitch.color.exposureGain == Catch::Approx(0.5f));
        }

        // The working space equal to the clip's output - the only case the
        // effect's rule lets the direct path render - gives a byte-identical
        // block: no conversion anywhere, same pixels as the equirect route.
        if (c.transfer != OSV_TRANSFER_PASSTHROUGH) {
            OsvEngineFrame working{};
            acquire(frameIndex++, c.transfer, working);
            CHECK(std::memcmp(&working.stitch.color, &own.stitch.color, sizeof(OsvColorParams)) == 0);
            CHECK(working.settings.generation == own.settings.generation);
            api.release(working.lease, nullptr);
        }
        api.release(own.lease, nullptr);
    }
}
