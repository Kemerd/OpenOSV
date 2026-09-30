// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_ofx_bundle.cpp - the bundle layout VEGAS Pro's plug-in scan needs,
// seen from the running module: OpenOSV.ofx sits alone in Contents\Win64 and
// its delay-loaded dependencies come from Contents\Libraries\Win64.
//
// CPU only: nothing here decodes video, touches NVDEC or CUDA, or renders.
// (The static half - what the staged tree holds - is the ctest
// "ofx_bundle_layout", tests/ofx/CheckBundleLayout.cmake.)

#include "OfxTestSupport.h"

#if defined(_WIN32)

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include "OfxSource.h"
#include "OfxSourceParams.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using namespace osv::ofxtest;

namespace {

/// The full path of the module `name` loaded in this process, or empty when
/// it is not loaded.
std::filesystem::path loadedModulePath(const wchar_t* name) {
    if (!name) {
        return {};
    }
    const HMODULE h = ::GetModuleHandleW(name);
    if (!h) {
        return {};
    }
    // Grow the buffer until the whole path fits.
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD n = ::GetModuleFileNameW(h, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) {
            return {};
        }
        if (n < path.size() - 1) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2, L'\0');
    }
    return {};
}

/// True when `path` lies directly inside `dir` (canonical, case-insensitive).
bool isDirectlyIn(const std::filesystem::path& path, const std::filesystem::path& dir) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::weakly_canonical(path.parent_path(), ec);
    const std::filesystem::path want = std::filesystem::weakly_canonical(dir, ec);
    return !parent.empty() && !want.empty() && ::_wcsicmp(parent.c_str(), want.c_str()) == 0;
}

/// The bundle's dependency folder, derived from the module's own location:
/// <bundle>/Contents/Win64/OpenOSV.ofx -> <bundle>/Contents/Libraries/Win64.
std::filesystem::path librariesFolder() {
    return std::filesystem::path(OSV_OFX_MODULE_PATH).parent_path() / L".." / L"Libraries" / L"Win64";
}

}  // namespace

// ===========================================================================
//  Where the module's DLLs come from
// ===========================================================================

TEST_CASE("the module's delay-loaded DLLs come from Contents/Libraries/Win64", "[ofx][bundle]") {
    Fixture& f = Fixture::get();
    REQUIRE(f.ready);
    const std::filesystem::path libs = librariesFolder();
    REQUIRE(std::filesystem::is_directory(libs));

    // Choosing a file makes the generator open it for the Clip read-out,
    // which runs the library's container and logging code - calls into fmt
    // and spdlog, both /DELAYLOAD'ed - so the module's delay-load hook has
    // resolved them by the time it returns.  The file is not a clip, so
    // nothing is decoded, and no GPU is touched.
    const std::filesystem::path junk = std::filesystem::temp_directory_path() / "openosv-ofx-bundle-probe.OSV";
    {
        std::ofstream out(junk, std::ios::binary);
        out << "this is not an ISO BMFF container";
    }
    OfxStatus st = kOfxStatFailed;
    auto effect = f.source.createInstance(kOfxImageEffectContextGenerator, 320, 180, 29.97, &st);
    REQUIRE(st == kOfxStatOK);
    Param* file = effect->params.find(osv::ofx::source::kFile);
    REQUIRE(file);
    file->s = junk.string();
    CHECK(f.source.instanceChanged(*effect, osv::ofx::source::kFile, kOfxChangeUserEdited, 0.0) == kOfxStatOK);
    (void)f.source.destroyInstance(*effect);
    std::error_code removed;
    std::filesystem::remove(junk, removed);

    // The hook takes them from <bundle>/Contents/Libraries/Win64: the folder
    // beside OpenOSV.ofx holds no DLL to take.
    for (const wchar_t* name : {L"fmt.dll", L"spdlog.dll"}) {
        const std::filesystem::path where = loadedModulePath(name);
        INFO(std::filesystem::path(name).string() << " was loaded from '" << where.string() << "'");
        REQUIRE_FALSE(where.empty());
        CHECK(isDirectlyIn(where, libs));
    }
}

TEST_CASE("FFmpeg and its own dependencies resolve inside Contents/Libraries/Win64", "[ofx][bundle]") {
    REQUIRE(Fixture::get().ready);
    const std::filesystem::path libs = librariesFolder();
    const std::filesystem::path avformat = libs / L"avformat-63.dll";
    REQUIRE(std::filesystem::exists(avformat));

    // The module itself calls into FFmpeg only to decode, and decoding tries
    // the GPU decoders first - which these tests must not touch.  So the
    // library is loaded the way the module's hook loads it (an absolute path
    // with LOAD_WITH_ALTERED_SEARCH_PATH), and what is proved is the half the
    // hook depends on: FFmpeg's inter-dependencies (avcodec, avutil,
    // swresample) resolve INSIDE that folder, whatever PATH holds.
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(avformat, ec);
    REQUIRE_FALSE(canonical.empty());
    const HMODULE h = ::LoadLibraryExW(canonical.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE(h != nullptr);

    for (const wchar_t* name : {L"avformat-63.dll", L"avcodec-63.dll", L"avutil-61.dll", L"swresample-7.dll"}) {
        const std::filesystem::path where = loadedModulePath(name);
        INFO(std::filesystem::path(name).string() << " was loaded from '" << where.string() << "'");
        REQUIRE_FALSE(where.empty());
        CHECK(isDirectlyIn(where, libs));
    }
    ::FreeLibrary(h);
}

#endif  // _WIN32
