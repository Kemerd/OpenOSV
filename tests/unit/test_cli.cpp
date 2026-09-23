// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// End-to-end tests that run the built osvtool executable.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/io/ImageWriter.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#if defined(OSV_TOOL_PATH)
const char* const kToolPath = OSV_TOOL_PATH;
#else
const char* const kToolPath = "";
#endif

struct RunResult {
    int exitCode = -1;
    std::string output;  // stdout + stderr
};

/// Run osvtool with `args` and capture its output (Windows CreateProcess).
RunResult runTool(const std::string& args) {
    RunResult r;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) {
        return r;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = std::string("\"") + kToolPath + "\" " + args;
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) {
        CloseHandle(readEnd);
        return r;
    }
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        r.output.append(buffer, got);
    }
    CloseHandle(readEnd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    (void)args;
#endif
    return r;
}

bool asciiOnly(const std::string& s) {
    for (const unsigned char c : s) {
        if (c >= 0x80) {
            return false;
        }
    }
    return true;
}

std::string quoted(const std::filesystem::path& p) { return "\"" + p.string() + "\""; }

}  // namespace

TEST_CASE("osvtool usage errors and version", "[cli]") {
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const RunResult none = runTool("");
    REQUIRE(none.exitCode == 1);
    const RunResult bad = runTool("render --bogus");
    REQUIRE(bad.exitCode == 1);
    const RunResult ver = runTool("--version");
    REQUIRE(ver.exitCode == 0);
    REQUIRE(ver.output.find("OpenOSV") != std::string::npos);
    REQUIRE(asciiOnly(ver.output));
    const RunResult missing = runTool("probe " + quoted(osvtest::tempDir() / "does-not-exist.OSV"));
    REQUIRE(missing.exitCode == 2);
}

TEST_CASE("osvtool lut writes a cube", "[cli]") {
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const auto cube = osvtest::tempDir() / "cli.cube";
    const RunResult r = runTool("lut --out-transfer hlg --size 33 " + quoted(cube));
    REQUIRE(r.exitCode == 0);
    std::ifstream in(cube);
    REQUIRE(in.good());
    std::string line;
    std::size_t dataLines = 0;
    bool sawSize = false;
    while (std::getline(in, line)) {
        if (line.rfind("LUT_3D_SIZE 33", 0) == 0) {
            sawSize = true;
        } else if (!line.empty() && (std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '-')) {
            ++dataLines;
        }
    }
    REQUIRE(sawSize);
    REQUIRE(dataLines == 33u * 33u * 33u);
}

TEST_CASE("osvtool probe/render/seam/selfcheck on the sample clip", "[cli][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const std::string clip = quoted(osvtest::sampleOsv());

    SECTION("probe --json") {
        const auto json = osvtest::tempDir() / "cli_probe.json";
        const RunResult r = runTool("probe " + clip + " --json " + quoted(json));
        REQUIRE(r.exitCode == 0);
        REQUIRE(asciiOnly(r.output));
        std::ifstream in(json);
        REQUIRE(in.good());
        nlohmann::json j;
        in >> j;
        REQUIRE(j["format"]["mode"].get<std::string>() == "K6");
        REQUIRE(j["stream"]["colorModeName"].get<std::string>() == "DLogM");
    }

    SECTION("render one frame to PNG") {
        const auto png = osvtest::tempDir() / "cli_frame.png";
        const RunResult r = runTool("render " + clip + " --frame 0 --size 640x360 --device cpu --out " + quoted(png));
        INFO(r.output);
        REQUIRE(r.exitCode == 0);
        auto img = osv::io::readImage(png);
        REQUIRE(img.ok());
        REQUIRE(img.value().w == 640);
        REQUIRE(img.value().h == 360);
    }

    SECTION("render with the eye-offset projection and a distortion value") {
        const auto png = osvtest::tempDir() / "cli_eye_offset.png";
        const RunResult r = runTool("render " + clip + " --frame 0 --size 320x180 --proj eye-offset --distortion 0.5 --fov 150 --device cpu --out " + quoted(png));
        INFO(r.output);
        REQUIRE(r.exitCode == 0);
        auto img = osv::io::readImage(png);
        REQUIRE(img.ok());
        REQUIRE(img.value().w == 320);
        // Out-of-range distortion is a usage error.
        const RunResult bad = runTool("render " + clip + " --frame 0 --size 64x36 --proj eye-offset --distortion 1.5 --device cpu --out " + quoted(png));
        REQUIRE(bad.exitCode != 0);
        REQUIRE(bad.output.find("--distortion") != std::string::npos);
    }

    SECTION("render polar equirect to linear EXR") {
        const auto exr = osvtest::tempDir() / "cli_equirect.exr";
        const RunResult r = runTool("render " + clip + " --frame 0 --mode equirect-polar --size 1024x512 --color linear --device cpu --out " + quoted(exr));
        INFO(r.output);
        REQUIRE(r.exitCode == 0);
        auto img = osv::io::readExr(exr);
        REQUIRE(img.ok());
        REQUIRE(img.value().w == 1024);
    }

    SECTION("seam --json reports alignment") {
        const RunResult r = runTool("seam " + clip + " --frame 0 --json");
        INFO(r.output);
        REQUIRE(r.exitCode == 0);
        const std::size_t brace = r.output.find('{');
        REQUIRE(brace != std::string::npos);
        nlohmann::json j = nlohmann::json::parse(r.output.substr(brace));
        REQUIRE(j["ncc"].get<double>() >= 0.8);
    }

    SECTION("selfcheck passes") {
        const RunResult r = runTool("selfcheck " + clip);
        INFO(r.output);
        REQUIRE(r.exitCode == 0);
        REQUIRE(r.output.find("ALL CHECKS PASSED") != std::string::npos);
    }
}

TEST_CASE("osvtool probe lists the calibration sets, the accessory and every choice", "[cli][calibration][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const auto path = osvtest::tempDir() / "cli_probe_calibration.json";
    const RunResult r = runTool("probe " + quoted(osvtest::sampleOsv()) + " --json " + quoted(path));
    INFO(r.output);
    REQUIRE(r.exitCode == 0);
    REQUIRE(asciiOnly(r.output));

    // ---- console: the table and the verdict a user needs ----------------------
    REQUIRE(r.output.find("calibration sets (differences vs native_refine") != std::string::npos);
    REQUIRE(r.output.find("lens_guards        empty    zero-filled placeholder") != std::string::npos);
    REQUIRE(r.output.find("lens accessory: Native (StreamMeta.extri_lens_mode)") != std::string::npos);
    REQUIRE(r.output.find("lens-guards  -> native_refine  (= native)") != std::string::npos);

    // ---- JSON ---------------------------------------------------------------------
    std::ifstream in(path);
    REQUIRE(in.good());
    nlohmann::json j;
    in >> j;
    const nlohmann::json& cal = j["calibration"];
    REQUIRE(cal["sets"].is_array());
    REQUIRE(cal["sets"].size() == 12);
    REQUIRE(cal["sets"][0]["name"] == "native_refine");
    REQUIRE(cal["sets"][0]["state"] == "usable");
    REQUIRE(cal["sets"][0]["vsNative"]["identical"] == true);
    REQUIRE(cal["sets"][2]["name"] == "lens_guards");
    REQUIRE(cal["sets"][2]["state"] == "empty");
    REQUIRE(cal["sets"][2]["vsNative"].is_null());
    REQUIRE(cal["sets"][5]["name"] == "native");
    REQUIRE(cal["sets"][5]["vsNative"]["focalPx"].get<double>() > 5.0);

    REQUIRE(cal["accessory"]["recordedModeName"] == "Native");
    REQUIRE(cal["accessory"]["recordedModePresent"] == true);
    REQUIRE(cal["accessory"]["ndFilterField"] == false);

    for (const char* choice : {"auto", "native", "lens-guards", "underwater"}) {
        INFO(choice);
        REQUIRE(cal["choices"][choice]["set"] == "native_refine");
        REQUIRE(cal["choices"][choice]["reason"].is_string());
    }
    REQUIRE(cal["choices"]["auto"]["fellBack"] == false);
    REQUIRE(cal["choices"]["lens-guards"]["fellBack"] == true);
    REQUIRE(cal["choices"]["underwater"]["fellBack"] == true);
}
