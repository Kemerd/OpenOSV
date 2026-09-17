// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Shared helpers for the test-suite: locating the sample clip, golden data,
// fixtures and a per-run scratch directory.
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace osvtest {

/// Path of the sample .OSV clip: OSV_SAMPLE_FILE environment variable wins,
/// otherwise the CMake-configured default.
inline std::filesystem::path sampleOsv() {
    if (const char* env = std::getenv("OSV_SAMPLE_FILE")) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path(OSV_SAMPLE_FILE);
}

/// Path of the sample .LRF proxy.
inline std::filesystem::path sampleLrf() {
    if (const char* env = std::getenv("OSV_SAMPLE_LRF_FILE")) {
        return std::filesystem::path(env);
    }
    return std::filesystem::path(OSV_SAMPLE_LRF_FILE);
}

/// True when the sample clip is present on this machine.
inline bool haveSample() {
    std::error_code ec;
    return std::filesystem::exists(sampleOsv(), ec) && !ec;
}

/// True when the sample .LRF proxy is present on this machine.
inline bool haveSampleLrf() {
    std::error_code ec;
    return std::filesystem::exists(sampleLrf(), ec) && !ec;
}

/// SKIP the current test when the sample is missing (CI without footage).
#define OSV_REQUIRE_SAMPLE()                                                                                           \
    do {                                                                                                               \
        if (!::osvtest::haveSample()) {                                                                                \
            SKIP("sample clip not available: " << ::osvtest::sampleOsv().string());                                    \
        }                                                                                                              \
    } while (0)

/// SKIP the current test when the sample .LRF proxy is missing.  It is a
/// separate file from the .OSV and a machine can easily have one and not the
/// other, so it needs its own guard rather than riding on OSV_REQUIRE_SAMPLE.
#define OSV_REQUIRE_SAMPLE_LRF()                                                                                       \
    do {                                                                                                               \
        if (!::osvtest::haveSampleLrf()) {                                                                             \
            SKIP("sample LRF proxy not available: " << ::osvtest::sampleLrf().string());                               \
        }                                                                                                              \
    } while (0)

/// Directory holding committed golden JSON files.
inline std::filesystem::path goldenDir() { return std::filesystem::path(OSV_GOLDEN_DIR); }

/// Directory holding small synthetic fixtures.
inline std::filesystem::path fixtureDir() { return std::filesystem::path(OSV_FIXTURE_DIR); }

/// Load a golden JSON document by file name (e.g. "sample_probe.json").
inline nlohmann::json loadGolden(const std::string& name) {
    std::ifstream in(goldenDir() / name);
    REQUIRE(in.good());
    nlohmann::json j;
    in >> j;
    return j;
}

/// Scratch directory unique to this test binary run.
inline std::filesystem::path tempDir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path p = std::filesystem::path(OSV_TEST_OUTPUT_DIR);
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }();
    return dir;
}

/// Relative comparison helper for floating point goldens.
inline bool approxRel(double a, double b, double rel = 1e-6, double abs = 1e-9) {
    const double diff = a > b ? a - b : b - a;
    const double scale = (a < 0 ? -a : a) > (b < 0 ? -b : b) ? (a < 0 ? -a : a) : (b < 0 ? -b : b);
    return diff <= abs || diff <= rel * scale;
}

}  // namespace osvtest
