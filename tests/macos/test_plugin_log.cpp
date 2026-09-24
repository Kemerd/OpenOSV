// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PluginLog on macOS (plugins/common/PluginLogPosix.cpp): where the file
// goes, what a line carries, levels, once(), rotation.  HOME is pointed at a
// scratch folder for the duration, so the test never writes into the real
// ~/Library/Logs/OpenOSV of whoever runs it.

#include <catch2/catch_test_macros.hpp>

#include "PluginLog.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

using osv::premiere::PluginLog;

namespace {

namespace fs = std::filesystem;

/// HOME set to a fresh scratch folder for one scope, then put back.
class ScratchHome {
public:
    ScratchHome() {
        const char* old = std::getenv("HOME");
        m_hadHome = old != nullptr;
        if (old) {
            m_oldHome = old;
        }
        m_dir = fs::temp_directory_path() / ("openosv-log-test-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(m_dir, ec);
        fs::create_directories(m_dir);
        ::setenv("HOME", m_dir.c_str(), 1);
    }
    ~ScratchHome() {
        PluginLog::shutdown();
        if (m_hadHome) {
            ::setenv("HOME", m_oldHome.c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }
    ScratchHome(const ScratchHome&) = delete;
    ScratchHome& operator=(const ScratchHome&) = delete;

    [[nodiscard]] const fs::path& dir() const noexcept { return m_dir; }

private:
    fs::path m_dir;
    std::string m_oldHome;
    bool m_hadHome = false;
};

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("the plug-in log lands in ~/Library/Logs/OpenOSV, one record per line", "[macos][plugins][log]") {
    ScratchHome home;
    PluginLog::setLevel(PluginLog::Level::Info);
    REQUIRE(PluginLog::init(L"OpenOSVLogTest"));

    const fs::path expected = home.dir() / "Library" / "Logs" / "OpenOSV" / "OpenOSVLogTest.log";
    CHECK(fs::path(PluginLog::filePath()) == expected);

    PluginLog::info("hello {} from the test", 42);
    PluginLog::debug("below the level: {}", "dropped");
    PluginLog::warn("two\nlines become one");
    CHECK(PluginLog::once("the-key", PluginLog::Level::Info, "first time only"));
    CHECK_FALSE(PluginLog::once("the-key", PluginLog::Level::Info, "first time only"));
    PluginLog::shutdown();

    REQUIRE(fs::is_regular_file(expected));
    const std::string text = readAll(expected);
    INFO(text);
    CHECK(text.find("hello 42 from the test") != std::string::npos);
    CHECK(text.find("[INFO ] [pid " + std::to_string(::getpid()) + " tid ") != std::string::npos);
    CHECK(text.find("dropped") == std::string::npos);
    CHECK(text.find("two lines become one") != std::string::npos);
    std::size_t onceCount = 0;
    for (std::size_t at = text.find("first time only"); at != std::string::npos;
         at = text.find("first time only", at + 1)) {
        ++onceCount;
    }
    CHECK(onceCount == 1);
}

TEST_CASE("the plug-in log rotates at its size limit and keeps writing", "[macos][plugins][log]") {
    ScratchHome home;
    PluginLog::setLevel(PluginLog::Level::Info);
    REQUIRE(PluginLog::init(L"OpenOSVRotateTest"));
    const fs::path file = home.dir() / "Library" / "Logs" / "OpenOSV" / "OpenOSVRotateTest.log";

    // Just over the limit in ~1 KB lines.
    const std::string filler(1000, 'x');
    const unsigned long long lines = PluginLog::kRotateBytes / 1000ull + 16ull;
    for (unsigned long long i = 0; i < lines; ++i) {
        PluginLog::info("{} {}", i, filler);
    }
    PluginLog::info("after the rotation");
    PluginLog::shutdown();

    fs::path backup = file;
    backup += ".1";
    CHECK(fs::is_regular_file(backup));
    REQUIRE(fs::is_regular_file(file));
    CHECK(fs::file_size(file) < PluginLog::kRotateBytes);
    CHECK(readAll(file).find("after the rotation") != std::string::npos);
}

TEST_CASE("without a home folder the log degrades to the unified log only", "[macos][plugins][log]") {
    ScratchHome home;
    ::unsetenv("HOME");
    PluginLog::shutdown();
    CHECK_FALSE(PluginLog::init(L"OpenOSVNoHome"));
    CHECK(PluginLog::filePath().empty());
    // Writing must still be safe.
    PluginLog::error("nowhere to go: {}", 1);
    SUCCEED();
}
