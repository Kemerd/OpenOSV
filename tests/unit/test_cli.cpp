// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// End-to-end tests that run the built osvtool executable.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/io/ImageWriter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#include <sys/wait.h>
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
    // POSIX: the same command line through /bin/sh, stderr folded into
    // stdout as the Windows branch does with one pipe for both.  The paths
    // the tests pass are double-quoted, which sh reads the same way.
    const std::string cmd = std::string("\"") + kToolPath + "\" " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) {
        return r;
    }
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        r.output.append(buffer, got);
    }
    const int status = ::pclose(pipe);
    if (status != -1 && WIFEXITED(status)) {
        r.exitCode = WEXITSTATUS(status);
    }
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

TEST_CASE("osvtool spherical tags an MP4 in place or into --out, and refuses a broken one", "[cli][spherical]") {
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path fixture = osvtest::fixtureDir() / "fx_largesize.mp4";
    const fs::path truncatedFixture = osvtest::fixtureDir() / "fx_truncated_moov.mp4";
    REQUIRE(fs::exists(fixture, ec));
    REQUIRE(fs::exists(truncatedFixture, ec));

    // Work on copies only: nothing here may ever write into the source tree.
    const fs::path source = osvtest::tempDir() / "cli_spherical_source.mp4";
    const fs::path file = osvtest::tempDir() / "cli_spherical.mp4";
    const fs::path out = osvtest::tempDir() / "cli_spherical_out.mp4";
    const fs::path broken = osvtest::tempDir() / "cli_spherical_truncated.mp4";
    fs::copy_file(fixture, source, fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    fs::copy_file(fixture, file, fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    fs::copy_file(truncatedFixture, broken, fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);
    fs::remove(out, ec);
    const auto sourceSize = fs::file_size(source);

    // ---- in place: tagged, then already tagged -------------------------------
    const RunResult first = runTool("spherical " + quoted(file));
    INFO(first.output);
    REQUIRE(first.exitCode == 0);
    CHECK(first.output.find("tagged") != std::string::npos);
    CHECK(asciiOnly(first.output));
    const auto taggedSize = fs::file_size(file);
    CHECK(taggedSize > sourceSize);
    const RunResult second = runTool("spherical " + quoted(file));
    REQUIRE(second.exitCode == 0);
    CHECK(second.output.find("already tagged") != std::string::npos);
    CHECK(fs::file_size(file) == taggedSize);

    // The V1 document and the V2 boxes are in the file.
    std::ifstream in(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes.find("<GSpherical:ProjectionType>equirectangular</GSpherical:ProjectionType>") != std::string::npos);
    CHECK(bytes.find("st3d") != std::string::npos);
    CHECK(bytes.find("sv3d") != std::string::npos);
    CHECK(bytes.find("equi") != std::string::npos);

    // ---- --out: a tagged copy, the input untouched ------------------------------
    const RunResult copy = runTool("spherical " + quoted(source) + " --out " + quoted(out));
    REQUIRE(copy.exitCode == 0);
    CHECK(fs::file_size(out) == taggedSize);
    CHECK(fs::file_size(source) == sourceSize);

    // ---- input errors (exit 2) leave the file as it was; no file is a usage error
    const auto brokenSize = fs::file_size(broken);
    const RunResult refused = runTool("spherical " + quoted(broken));
    CHECK(refused.exitCode == 2);
    CHECK(refused.output.find("error:") != std::string::npos);
    CHECK(fs::file_size(broken) == brokenSize);
    CHECK(runTool("spherical " + quoted(osvtest::tempDir() / "no-such-file.mp4")).exitCode == 2);
    CHECK(runTool("spherical").exitCode == 1);

    // ---- render takes the flag and its negation (--no-spherical-metadata) ------
    const RunResult help = runTool("render --help");
    CHECK(help.output.find("--spherical-metadata") != std::string::npos);
    CHECK(help.output.find("--no-spherical-metadata") != std::string::npos);
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

    SECTION("render with every --stab mode, smooth + horizon lock included; an unknown one is refused") {
        const auto png = osvtest::tempDir() / "cli_stab.png";
        for (const char* mode : {"off", "horizon", "full", "smooth", "smooth-horizon"}) {
            const RunResult r = runTool("render " + clip + " --frame 3 --size 160x90 --device cpu --stab " + mode +
                                        " --out " + quoted(png));
            INFO(mode << ": " << r.output);
            REQUIRE(r.exitCode == 0);
        }
        const RunResult bad = runTool("render " + clip + " --frame 0 --size 64x36 --device cpu --stab rocksteady --out " +
                                      quoted(png));
        REQUIRE(bad.exitCode != 0);
        REQUIRE(bad.output.find("unknown --stab") != std::string::npos);
    }

    SECTION("render polar equirect to linear EXR") {
        // Both are research outputs of the classic pipeline; the plug-ins'
        // engine (the default) renders the standard equirect and the
        // Source Settings outputs only, and says so.
        const auto exr = osvtest::tempDir() / "cli_equirect.exr";
        const RunResult refused = runTool("render " + clip + " --frame 0 --mode equirect-polar --size 1024x512 --color linear --device cpu --out " + quoted(exr));
        REQUIRE(refused.exitCode != 0);
        REQUIRE(refused.output.find("--engine classic") != std::string::npos);
        const RunResult r = runTool("render " + clip + " --engine classic --frame 0 --mode equirect-polar --size 1024x512 --color linear --device cpu --out " + quoted(exr));
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
    // Lens guards without a dedicated set: native plus the protector correction.
    REQUIRE(r.output.find("lens-guards  -> native_refine  + protector") != std::string::npos);
    REQUIRE(r.output.find("underwater   -> native_refine  (= native)") != std::string::npos);

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
    REQUIRE(cal["choices"]["lens-guards"]["protectorCorrection"] == true);
    REQUIRE(cal["choices"]["auto"]["protectorCorrection"] == false);
    REQUIRE(cal["choices"]["underwater"]["protectorCorrection"] == false);
}

// [WP-LOOK] The Rec.709 look is selectable on both colour commands.
TEST_CASE("osvtool --look selects the Rec.709 look and refuses unknown names", "[cli][look]") {
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    // Every data line of a .cube as text: two LUTs of the same size are the
    // same transform exactly when these match.
    const auto dataOf = [](const std::filesystem::path& path) {
        std::ifstream in(path);
        std::string line;
        std::string data;
        while (std::getline(in, line)) {
            if (!line.empty() && (std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '-')) {
                data += line;
                data += '\n';
            }
        }
        return data;
    };
    const auto dji = osvtest::tempDir() / "cli_look_dji.cube";
    const auto standard = osvtest::tempDir() / "cli_look_standard.cube";
    const auto implicit = osvtest::tempDir() / "cli_look_default.cube";
    REQUIRE(runTool("lut --out-transfer 709 --size 9 --look dji " + quoted(dji)).exitCode == 0);
    REQUIRE(runTool("lut --out-transfer 709 --size 9 --look standard " + quoted(standard)).exitCode == 0);
    REQUIRE(runTool("lut --out-transfer 709 --size 9 " + quoted(implicit)).exitCode == 0);
    const std::string a = dataOf(dji);
    REQUIRE(a.size() > 9u * 9u * 9u * 10u);
    // Two different Rec.709 renderings; the default is the DJI look.
    CHECK(a != dataOf(standard));
    CHECK(a == dataOf(implicit));
    // HDR outputs have no look: the flag changes nothing there.
    const auto hlgDji = osvtest::tempDir() / "cli_look_hlg_dji.cube";
    const auto hlgStd = osvtest::tempDir() / "cli_look_hlg_std.cube";
    REQUIRE(runTool("lut --out-transfer hlg --size 9 --look dji " + quoted(hlgDji)).exitCode == 0);
    REQUIRE(runTool("lut --out-transfer hlg --size 9 --look standard " + quoted(hlgStd)).exitCode == 0);
    CHECK(dataOf(hlgDji) == dataOf(hlgStd));
    // An unknown look is a usage error, not a silent default.
    const RunResult badLut = runTool("lut --out-transfer 709 --look vivid " + quoted(osvtest::tempDir() / "x.cube"));
    CHECK(badLut.exitCode == 1);
    CHECK(badLut.output.find("--look") != std::string::npos);
}

// [WP-LOOK] osvtool render --look on the sample clip: the two Rec.709 looks
// render different pictures, and a bad name is refused.
TEST_CASE("osvtool render --look switches the Rec.709 look", "[cli][look][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const std::string clip = quoted(osvtest::sampleOsv());
    const auto djiTif = osvtest::tempDir() / "cli_look_dji.tif";
    const auto stdTif = osvtest::tempDir() / "cli_look_standard.tif";
    const std::string common = " --frame 0 --size 320x180 --device cpu --color 709 --out ";
    const RunResult a = runTool("render " + clip + common + quoted(djiTif) + " --look dji");
    INFO(a.output);
    REQUIRE(a.exitCode == 0);
    const RunResult b = runTool("render " + clip + common + quoted(stdTif) + " --look standard");
    INFO(b.output);
    REQUIRE(b.exitCode == 0);
    auto imgA = osv::io::readImage(djiTif);
    auto imgB = osv::io::readImage(stdTif);
    REQUIRE(imgA.ok());
    REQUIRE(imgB.ok());
    REQUIRE(imgA.value().data.size() == imgB.value().data.size());
    float worst = 0.0f;
    for (std::size_t i = 0; i < imgA.value().data.size(); ++i) {
        worst = std::max(worst, std::fabs(imgA.value().data[i] - imgB.value().data[i]));
    }
    CHECK(worst > 0.02f);
    // Refused with the reason named, like an unknown --color or --fit (the
    // render command reports every pipeline set-up error with the same code).
    const RunResult bad = runTool("render " + clip + common + quoted(djiTif) + " --look vivid");
    CHECK(bad.exitCode != 0);
    CHECK(bad.output.find("--look") != std::string::npos);
}

// [WP-HDRPEAK] osvtool lut --hdr-peak: a PQ table rolls off into the chosen
// peak and says so in its title; 1000 is the default table; HLG ignores it
// and says so; anything but the four Source Settings choices is refused.
TEST_CASE("osvtool lut --hdr-peak rolls a PQ table off and leaves HLG alone", "[cli][hdrpeak]") {
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    // The data lines of a .cube, and its largest value and its TITLE line.
    struct Cube {
        std::string data;
        double maxValue = 0.0;
        std::string title;
    };
    const auto read = [](const std::filesystem::path& path) {
        Cube c;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("TITLE", 0) == 0) {
                c.title = line;
            } else if (!line.empty() && (std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '-')) {
                c.data += line;
                c.data += '\n';
                // Three numbers per data line; keep the largest.
                double v[3] = {0.0, 0.0, 0.0};
                if (std::sscanf(line.c_str(), "%lf %lf %lf", &v[0], &v[1], &v[2]) == 3) {
                    c.maxValue = std::max({c.maxValue, v[0], v[1], v[2]});
                }
            }
        }
        return c;
    };
    const auto pqDefault = osvtest::tempDir() / "cli_peak_pq_default.cube";
    const auto pq1000 = osvtest::tempDir() / "cli_peak_pq_1000.cube";
    const auto pq400 = osvtest::tempDir() / "cli_peak_pq_400.cube";
    REQUIRE(runTool("lut --out-transfer pq --size 17 " + quoted(pqDefault)).exitCode == 0);
    REQUIRE(runTool("lut --out-transfer pq --size 17 --hdr-peak 1000 " + quoted(pq1000)).exitCode == 0);
    const RunResult r400 = runTool("lut --out-transfer pq --size 17 --hdr-peak 400 " + quoted(pq400));
    INFO(r400.output);
    REQUIRE(r400.exitCode == 0);
    CHECK(r400.output.find("roll-off above 251 nits") != std::string::npos);
    const Cube d = read(pqDefault);
    const Cube a = read(pq1000);
    const Cube b = read(pq400);
    // 1000 is the default table, title and all.
    CHECK(d.data == a.data);
    CHECK(d.title == a.title);
    CHECK(d.title.find("peak") == std::string::npos);
    // 400 is a different table, never above PQ(400 nits) = 0.6526, and
    // named as such; the default reaches past it ([WP-HDRTONE] the default
    // ACES 2 Bright style's own ceiling is 600 nits, PQ 0.6963).
    CHECK(d.data != b.data);
    CHECK(b.maxValue <= 0.65262);
    CHECK(d.maxValue > 0.66);
    CHECK(b.title.find("400-nit peak") != std::string::npos);

    // HLG is display-relative: the table is the default one, and the report
    // says the option was ignored rather than staying silent.
    const auto hlgDefault = osvtest::tempDir() / "cli_peak_hlg_default.cube";
    const auto hlg400 = osvtest::tempDir() / "cli_peak_hlg_400.cube";
    REQUIRE(runTool("lut --out-transfer hlg --size 17 " + quoted(hlgDefault)).exitCode == 0);
    const RunResult h400 = runTool("lut --out-transfer hlg --size 17 --hdr-peak 400 " + quoted(hlg400));
    REQUIRE(h400.exitCode == 0);
    CHECK(h400.output.find("ignored") != std::string::npos);
    CHECK(read(hlgDefault).data == read(hlg400).data);

    // Only the Source Settings choices: 800 is a usage error.
    const RunResult bad = runTool("lut --out-transfer pq --hdr-peak 800 " + quoted(osvtest::tempDir() / "x.cube"));
    CHECK(bad.exitCode == 1);
    CHECK(bad.output.find("--hdr-peak") != std::string::npos);
}

// [WP-HDRTONE] osvtool lut --tone: each style is its own PQ / HLG table and
// names itself in the title, aces-bright is the default, the short names
// are the long ones, Rec.709 ignores the option and says so, and anything
// else is refused.
TEST_CASE("osvtool lut --tone selects the HDR transfer function", "[cli][hdrtone]") {
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    // The data lines and the TITLE line of a .cube.
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream in(path);
        std::string line;
        std::pair<std::string, std::string> dataAndTitle;
        while (std::getline(in, line)) {
            if (line.rfind("TITLE", 0) == 0) {
                dataAndTitle.second = line;
            } else if (!line.empty() && (std::isdigit(static_cast<unsigned char>(line[0])) || line[0] == '-')) {
                dataAndTitle.first += line;
                dataAndTitle.first += '\n';
            }
        }
        return dataAndTitle;
    };
    const char* const kTones[] = {"aces-bright", "aces-detailed", "bt2408-natural", "bt2408-punchy", "bt2408-neutral"};
    for (const char* transfer : {"pq", "hlg"}) {
        std::vector<std::string> tables;
        for (const char* tone : kTones) {
            const auto path = osvtest::tempDir() / (std::string("cli_tone_") + transfer + "_" + tone + ".cube");
            const RunResult r = runTool(std::string("lut --out-transfer ") + transfer + " --size 9 --tone " + tone +
                                        " " + quoted(path));
            INFO(r.output);
            REQUIRE(r.exitCode == 0);
            CHECK(r.output.find("tone      :") != std::string::npos);
            const auto [data, title] = read(path);
            CHECK(title.find(std::string(tone) + " tone") != std::string::npos);
            tables.push_back(data);
        }
        // Five styles, five different tables.
        for (std::size_t i = 0; i < tables.size(); ++i) {
            for (std::size_t j = i + 1; j < tables.size(); ++j) {
                INFO(transfer << ": " << kTones[i] << " vs " << kTones[j]);
                CHECK(tables[i] != tables[j]);
            }
        }
        // The default is ACES 2 Bright, and "neutral" is bt2408-neutral.
        const auto implicit = osvtest::tempDir() / (std::string("cli_tone_") + transfer + "_default.cube");
        REQUIRE(runTool(std::string("lut --out-transfer ") + transfer + " --size 9 " + quoted(implicit)).exitCode == 0);
        CHECK(read(implicit).first == tables[0]);
        const auto shortName = osvtest::tempDir() / (std::string("cli_tone_") + transfer + "_short.cube");
        REQUIRE(runTool(std::string("lut --out-transfer ") + transfer + " --size 9 --tone neutral " +
                        quoted(shortName))
                    .exitCode == 0);
        CHECK(read(shortName).first == tables[4]);
    }
    // Rec.709 has no transfer function style: the table is the default one
    // and the report says the option was ignored.
    const auto sdrDefault = osvtest::tempDir() / "cli_tone_709_default.cube";
    const auto sdrNeutral = osvtest::tempDir() / "cli_tone_709_neutral.cube";
    REQUIRE(runTool("lut --out-transfer 709 --size 9 " + quoted(sdrDefault)).exitCode == 0);
    const RunResult r709 = runTool("lut --out-transfer 709 --size 9 --tone bt2408-neutral " + quoted(sdrNeutral));
    REQUIRE(r709.exitCode == 0);
    CHECK(r709.output.find("ignored") != std::string::npos);
    CHECK(read(sdrDefault).first == read(sdrNeutral).first);
    CHECK(read(sdrDefault).second.find("tone") == std::string::npos);
    // An unknown style is a usage error, not a silent default.
    const RunResult bad = runTool("lut --out-transfer pq --tone aces " + quoted(osvtest::tempDir() / "x.cube"));
    CHECK(bad.exitCode == 1);
    CHECK(bad.output.find("--tone") != std::string::npos);
}

// [WP-HDRPEAK] osvtool render --hdr-peak on the sample clip: the PQ still
// never passes the chosen peak, its sidecar records it, and a bad value is
// refused.
TEST_CASE("osvtool render --hdr-peak caps the PQ still at the chosen peak", "[cli][hdrpeak][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const std::string clip = quoted(osvtest::sampleOsv());
    // Frame 30: the sunlit white aircraft beside the camera.  [WP-HDRTONE]
    // On the BT.2408 Neutral style, whose 1000-nit master has highlights up
    // to the full 1000 nits; the default ACES 2 Bright style is checked
    // after it.
    const std::string common =
        " --frame 30 --mode equirect --size 512x256 --device cpu --color pq --tone bt2408-neutral --out ";
    const auto fullTif = osvtest::tempDir() / "cli_peak_1000.tif";
    const auto peakTif = osvtest::tempDir() / "cli_peak_203.tif";
    const RunResult a = runTool("render " + clip + common + quoted(fullTif));
    INFO(a.output);
    REQUIRE(a.exitCode == 0);
    const RunResult b = runTool("render " + clip + common + quoted(peakTif) + " --hdr-peak 203");
    INFO(b.output);
    REQUIRE(b.exitCode == 0);
    auto full = osv::io::readImage(fullTif);
    auto peak = osv::io::readImage(peakTif);
    REQUIRE(full.ok());
    REQUIRE(peak.ok());
    // The largest colour sample of each (every fourth value is the alpha).
    const auto maxColour = [](const std::vector<float>& rgba) {
        float m = 0.0f;
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            m = std::max({m, rgba[i], rgba[i + 1], rgba[i + 2]});
        }
        return m;
    };
    // PQ(203 nits) = 0.5807; one 16-bit step of slack for the TIFF.
    CHECK(maxColour(peak.value().data) <= 0.58075f + 1.0f / 65535.0f);
    CHECK(maxColour(full.value().data) > 0.70f);
    // The sidecars record each still's peak.
    const auto sidecar = [](const std::filesystem::path& image) {
        std::ifstream in(image.string() + ".json");
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    CHECK(sidecar(fullTif).find("\"peak_nits\": 1000") != std::string::npos);
    CHECK(sidecar(peakTif).find("\"peak_nits\": 203") != std::string::npos);
    // [WP-HDRTONE] The default style's own ceiling is 600 nits (PQ 0.6963):
    // the still never passes it, and its sidecar says so.
    const auto brightTif = osvtest::tempDir() / "cli_peak_bright.tif";
    const RunResult c = runTool("render " + clip +
                                " --frame 30 --mode equirect --size 512x256 --device cpu --color pq --out " +
                                quoted(brightTif));
    INFO(c.output);
    REQUIRE(c.exitCode == 0);
    auto bright = osv::io::readImage(brightTif);
    REQUIRE(bright.ok());
    CHECK(maxColour(bright.value().data) <= 0.69630f + 1.0f / 65535.0f);
    CHECK(sidecar(brightTif).find("\"peak_nits\": 600") != std::string::npos);
    // Refused with the reason named.
    const RunResult bad = runTool("render " + clip + common + quoted(peakTif) + " --hdr-peak 500");
    CHECK(bad.exitCode != 0);
    CHECK(bad.output.find("--hdr-peak") != std::string::npos);
}

// [WP-DEFAULTS] osvtool render --use-user-defaults: the Source Settings saved
// in Premiere as the default for new clips become the starting values of the
// render options - only when asked, and never over an option given on the
// command line.
TEST_CASE("osvtool render --use-user-defaults follows the saved defaults only when asked",
          "[cli][defaults][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
#if defined(_WIN32)
    // A defaults file as the plug-ins write it.  Keys a file does not carry
    // are the importer's built-in values, so the ones that would make a CPU
    // render slow - parallax, the sky seam fix - are written out as off.
    const auto defaultsFile = osvtest::tempDir() / "cli_user_defaults" / "defaults.json";
    std::filesystem::create_directories(defaultsFile.parent_path());
    {
        std::ofstream out(defaultsFile, std::ios::binary | std::ios::trunc);
        out << R"({
  "format": "openosv-source-settings-defaults",
  "version": 1,
  "settings": {
    "colourOutput": "rec709",
    "rec709Look": "standard",
    "stabilisation": "off",
    "seamSearch": false,
    "exposureMatch": false,
    "calibration": "auto",
    "skySeamFix": "off",
    "parallaxCorrection": false,
    "dlogmCurve": "osmo360",
    "exposureStops": 0.5,
    "renderDevice": "cpu"
  }
})";
    }

    // The child inherits this process's environment, so the variable is set
    // for the renders that should see it and removed for the references.
    struct ScopedVariable {
        explicit ScopedVariable(const std::string& value) {
            ::SetEnvironmentVariableA("OPENOSV_DEFAULTS_FILE", value.empty() ? nullptr : value.c_str());
        }
        ~ScopedVariable() { ::SetEnvironmentVariableA("OPENOSV_DEFAULTS_FILE", nullptr); }
        ScopedVariable(const ScopedVariable&) = delete;
        ScopedVariable& operator=(const ScopedVariable&) = delete;
    };

    const std::string clip = quoted(osvtest::sampleOsv());
    const std::string frame = " --frame 0 --size 320x180 --out ";
    struct Rendered {
        std::vector<float> pixels;
        std::string output;
    };
    const auto render = [&](const std::string& name, const std::string& flags, bool withFile) {
        const ScopedVariable variable(withFile ? defaultsFile.string() : std::string());
        const auto path = osvtest::tempDir() / name;
        const RunResult r = runTool("render " + clip + frame + quoted(path) + flags);
        INFO(r.output);
        REQUIRE(r.exitCode == 0);
        auto img = osv::io::readImage(path);
        REQUIRE(img.ok());
        return Rendered{img.value().data, r.output};
    };

    // Asked: the saved Rec.709 / standard look / +0.5 stops on the CPU...
    const Rendered fromDefaults = render("cli_ud_defaults.tif", " --use-user-defaults", true);
    CHECK(fromDefaults.output.find("--use-user-defaults:") != std::string::npos);
    // ...is exactly the render those options spell out by hand.  The
    // plug-ins' engine starts from the Source Settings defaults, so "by hand"
    // also switches off what the file switches off; the keys the file does
    // not carry are the built-in values on both sides.
    const std::string savedOff = " --stab off --no-seam-search --no-gain --photo off --no-parallax";
    const Rendered byHand =
        render("cli_ud_byhand.tif", " --device cpu --color 709 --look standard --exposure 0.5" + savedOff, false);
    CHECK(fromDefaults.pixels == byHand.pixels);

    // An option given on the command line wins over the saved default.
    const Rendered overridden = render("cli_ud_override.tif", " --use-user-defaults --look dji", true);
    const Rendered overriddenByHand =
        render("cli_ud_override_byhand.tif", " --device cpu --color 709 --look dji --exposure 0.5" + savedOff, false);
    CHECK(overridden.pixels == overriddenByHand.pixels);
    CHECK(overridden.pixels != fromDefaults.pixels);

    // Not asked: the file is ignored even though the variable names it, so a
    // plain render is the same on every machine.
    const Rendered notAsked = render("cli_ud_not_asked.tif", " --device cpu", true);
    const Rendered reference = render("cli_ud_reference.tif", " --device cpu", false);
    CHECK(notAsked.pixels == reference.pixels);
    CHECK(notAsked.pixels != fromDefaults.pixels);
    CHECK(notAsked.output.find("--use-user-defaults") == std::string::npos);
#endif
}

// The default engine of osvtool render is the plug-ins' own clip engine: a
// render is the frame Premiere shows for a new clip, with the Source Settings
// defaults for whatever the command line leaves out.  The classic research
// pipeline stays one flag away, and each engine refuses the other's options
// instead of ignoring them.
TEST_CASE("osvtool render uses the plug-ins' engine by default; --engine classic keeps the research options",
          "[cli][engine][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const std::string clip = quoted(osvtest::sampleOsv());
    const std::string frame = " --frame 0 --size 320x160 --device cpu --out ";

    // The default is the plug-ins' engine, and a plain equirect without
    // --size takes the Source Settings Output Size (Native: 2 x lens height).
    const auto native = osvtest::tempDir() / "cli_engine_native.tif";
    const RunResult plain =
        runTool("render " + clip + " --frame 0 --mode equirect --device cpu --no-flare --out " + quoted(native));
    INFO(plain.output);
    REQUIRE(plain.exitCode == 0);
    CHECK(plain.output.find("plug-in clip engine") != std::string::npos);
    auto nativeImage = osv::io::readImage(native);
    REQUIRE(nativeImage.ok());
    CHECK(nativeImage.value().w == 2u * nativeImage.value().h);
    CHECK(nativeImage.value().h == 3000u);

    // A reframe through the engine frames exactly like the classic one: the
    // same virtual camera, only the stitch differs.
    const auto viaEngine = osvtest::tempDir() / "cli_engine_view.tif";
    const RunResult view = runTool("render " + clip + " --preset wide" + frame + quoted(viaEngine));
    INFO(view.output);
    REQUIRE(view.exitCode == 0);
    auto viewImage = osv::io::readImage(viaEngine);
    REQUIRE(viewImage.ok());
    CHECK(viewImage.value().w == 320u);
    CHECK(viewImage.value().h == 160u);

    // A research option on the default engine is refused, with the way out.
    const auto refusedPath = osvtest::tempDir() / "cli_engine_refused.tif";
    const RunResult refused = runTool("render " + clip + " --lens-fov 190" + frame + quoted(refusedPath));
    CHECK(refused.exitCode != 0);
    CHECK(refused.output.find("--lens-fov") != std::string::npos);
    CHECK(refused.output.find("--engine classic") != std::string::npos);
    // ...and accepted by the classic pipeline.
    const RunResult classic =
        runTool("render " + clip + " --engine classic --lens-fov 190" + frame + quoted(refusedPath));
    INFO(classic.output);
    CHECK(classic.exitCode == 0);
    CHECK(classic.output.find("plug-in clip engine") == std::string::npos);

    // An option of the plug-ins' engine is refused by the classic pipeline.
    const RunResult noFlare = runTool("render " + clip + " --engine classic --no-flare" + frame + quoted(refusedPath));
    CHECK(noFlare.exitCode != 0);
    CHECK(noFlare.output.find("--flare") != std::string::npos);

    // Unknown engine names never reach a render.
    const RunResult bogus = runTool("render " + clip + " --engine turbo" + frame + quoted(refusedPath));
    CHECK(bogus.exitCode != 0);
}

// osvtool render --occlusion / --no-occlusion on the plug-ins' engine is
// Source Settings "Hide Mount" (On / Off).  The engine used to refuse the pair
// as a classic-only research option, so the Off picture Premiere, Resolve and
// VEGAS render could not be reproduced from the command line at all.  This
// pins the three promises of the mapping:
//   * --no-occlusion is accepted and really renders Off (the engine says so,
//     and the frame changes where the calibration's polygons cut the lenses);
//   * --occlusion is On, which is the built-in default, bit for bit;
//   * a render that names neither stays the default and logs nothing about it.
// CPU renderer and the classical flow (the solver the plug-ins ship), at a
// small size: the mapping is under test here, not the stitch's quality.
TEST_CASE("osvtool render --occlusion / --no-occlusion is Hide Mount On / Off on the plug-ins' engine",
          "[cli][engine][hidemount][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const std::string clip = quoted(osvtest::sampleOsv());
    // One frame, an equirect so the mount-side seam is in the picture
    // whatever the view, and the settings every render below shares.
    const std::string common =
        " --frame 0 --mode equirect --size 512x256 --device cpu --flow-backend classical --no-flare --out ";

    // Render once and read the still back; a failed run or an unreadable
    // still fails the test with osvtool's own output attached.
    const auto renderTo = [&](const std::string& extra, const char* name, RunResult& run) {
        const auto path = osvtest::tempDir() / name;
        std::error_code ec;
        std::filesystem::remove(path, ec);  // never compare against a stale still
        run = runTool("render " + clip + extra + common + quoted(path));
        INFO(run.output);
        REQUIRE(run.exitCode == 0);
        auto image = osv::io::readImage(path);
        REQUIRE(image.ok());
        REQUIRE(image.value().w == 512u);
        REQUIRE(image.value().h == 256u);
        REQUIRE(image.value().data.size() == std::size_t{4} * 512u * 256u);
        return std::move(image).value();
    };

    // ---- the default: Hide Mount On, and nothing logged about it ----------
    RunResult plainRun;
    const auto plain = renderTo("", "cli_hidemount_default.tif", plainRun);
    CHECK(plainRun.output.find("plug-in clip engine") != std::string::npos);
    CHECK(plainRun.output.find("hide mount:") == std::string::npos);

    // ---- --occlusion: On, the default bit for bit --------------------------
    RunResult onRun;
    const auto on = renderTo(" --occlusion", "cli_hidemount_on.tif", onRun);
    CHECK(onRun.output.find("hide mount:") == std::string::npos);
    CHECK(on.data == plain.data);

    // ---- --no-occlusion: Off, accepted and in force ------------------------
    RunResult offRun;
    const auto off = renderTo(" --no-occlusion", "cli_hidemount_off.tif", offRun);
    INFO(offRun.output);
    // Accepted on the default engine, never sent to the classic pipeline.
    CHECK(offRun.output.find("belongs to the classic pipeline") == std::string::npos);
    CHECK(offRun.output.find("plug-in clip engine") != std::string::npos);
    // The engine's own line for an explicit Off (rebuildRig).
    CHECK(offRun.output.find("hide mount:") != std::string::npos);
    CHECK(offRun.output.find("Off - the calibration's occlusion polygons are not applied") != std::string::npos);
    // And the picture follows: pixels inside the polygons now come from the
    // lens the mask used to cut, and the seam gains are measured over the
    // full overlap, so the frame is not the On frame.  A pixel counts once
    // however many of its channels moved.  Measured on the sample at this
    // size: 75,509 of 131,072 pixels change; the bound is the one the
    // importer's [hidemount] test uses, far below that and far above noise.
    std::size_t changed = 0;
    for (std::size_t i = 0; i + 3 < off.data.size(); i += 4) {
        for (std::size_t ch = 0; ch < 4; ++ch) {
            if (off.data[i + ch] != plain.data[i + ch]) {
                ++changed;
                break;
            }
        }
    }
    INFO(changed << " of " << (off.data.size() / 4) << " pixels differ between Off and On");
    CHECK(changed > 1000u);
}

// osvtool render --alpha: the coverage alpha a host composites with, written
// into the stills as a 4th channel - and only when asked, so every render
// without the flag stays byte-for-byte what it was.  Without it, a band of
// transparent rows (which a host shows black over a black background) was
// invisible to every osvtool-based check, because the stills carried RGB only.
TEST_CASE("osvtool render --alpha writes the coverage alpha into .tif / .png / .exr stills, only when asked",
          "[cli][alpha][sample]") {
    OSV_REQUIRE_SAMPLE_LRF();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    namespace fs = std::filesystem;
    const std::string clip = quoted(osvtest::sampleLrf());
    // The research pipeline with a 150-degree lens: the two lenses no longer
    // meet, so the picture has a real, partly transparent coverage - rows
    // near the poles that no lens sees - that the alpha must carry.
    const std::string narrow =
        " --engine classic --lens-fov 150 --mode equirect --size 256x128 --frame 0 --device cpu";
    const auto sidecar = [](const fs::path& image) {
        std::ifstream in(image.string() + ".json");
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    // ---- without the flag: RGB only, as always ----------------------------------------
    const auto plainTif = osvtest::tempDir() / "cli_alpha_off.tif";
    const RunResult plain = runTool("render " + clip + narrow + " --out " + quoted(plainTif));
    INFO(plain.output);
    REQUIRE(plain.exitCode == 0);
    CHECK(sidecar(plainTif).find("\"alpha\": false") != std::string::npos);
    auto plainImage = osv::io::readImage(plainTif);
    REQUIRE(plainImage.ok());

    // ---- with it: the same colour, plus the coverage -------------------------------------
    const auto alphaTif = osvtest::tempDir() / "cli_alpha_on.tif";
    const auto alphaPng = osvtest::tempDir() / "cli_alpha_on.png";
    const auto alphaExr = osvtest::tempDir() / "cli_alpha_on.exr";
    for (const fs::path& out : {alphaTif, alphaPng, alphaExr}) {
        const RunResult r = runTool("render " + clip + narrow + " --alpha --out " + quoted(out));
        INFO(out.filename().string() << ": " << r.output);
        REQUIRE(r.exitCode == 0);
    }
    CHECK(sidecar(alphaTif).find("\"alpha\": true") != std::string::npos);
    CHECK(sidecar(alphaPng).find("\"alpha\": true") != std::string::npos);
    // Four 16-bit channels instead of three.
    std::error_code ec;
    CHECK(fs::file_size(alphaTif, ec) > fs::file_size(plainTif, ec) * 5u / 4u);

    auto tif = osv::io::readImage(alphaTif);
    auto png = osv::io::readImage(alphaPng);
    auto exr = osv::io::readExr(alphaExr);
    REQUIRE(tif.ok());
    REQUIRE(png.ok());
    REQUIRE(exr.ok());
    const auto& a = tif.value();
    REQUIRE(a.w == 256u);
    REQUIRE(a.h == 128u);
    REQUIRE(plainImage.value().w == a.w);
    REQUIRE(plainImage.value().data.size() == a.data.size());
    REQUIRE(png.value().data.size() == a.data.size());
    REQUIRE(exr.value().data.size() == a.data.size());

    std::size_t transparentPixels = 0;
    std::size_t transparentRows = 0;
    float colourDiff = 0.0f;
    float pngAlphaDiff = 0.0f;
    float exrAlphaDiff = 0.0f;
    float plainAlphaMin = 1.0f;
    for (std::uint32_t y = 0; y < a.h; ++y) {
        double rowAlpha = 0.0;
        for (std::uint32_t x = 0; x < a.w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * a.w + x) * 4u;
            // The flag changes nothing about the colour...
            for (std::size_t c = 0; c < 3; ++c) {
                colourDiff = std::max(colourDiff, std::fabs(a.data[i + c] - plainImage.value().data[i + c]));
            }
            // ...the RGB-only still reads back fully opaque...
            plainAlphaMin = std::min(plainAlphaMin, plainImage.value().data[i + 3]);
            // ...and the three formats carry the same alpha (16-bit .tif and
            // .png exactly; the float .exr to within half a 16-bit step).
            pngAlphaDiff = std::max(pngAlphaDiff, std::fabs(png.value().data[i + 3] - a.data[i + 3]));
            exrAlphaDiff = std::max(exrAlphaDiff, std::fabs(exr.value().data[i + 3] - a.data[i + 3]));
            transparentPixels += a.data[i + 3] < 0.5f ? 1u : 0u;
            rowAlpha += a.data[i + 3];
        }
        transparentRows += rowAlpha < 0.5 * a.w ? 1u : 0u;
    }
    INFO("transparent pixels " << transparentPixels << ", transparent rows " << transparentRows);
    CHECK(colourDiff == 0.0f);
    CHECK(plainAlphaMin == 1.0f);
    CHECK(pngAlphaDiff == 0.0f);
    CHECK(exrAlphaDiff <= 0.5f / 65535.0f + 1e-6f);
    // The uncovered rows show up - exactly what the RGB-only still hid.
    CHECK(transparentPixels > 0u);
    CHECK(transparentRows > 0u);

    // ---- the plug-ins' engine: every row of the .LRF covered ---------------------------
    const auto engineTif = osvtest::tempDir() / "cli_alpha_engine.tif";
    const RunResult engine = runTool("render " + clip +
                                     " --mode equirect --size 256x128 --frame 0 --device cpu --alpha --out " +
                                     quoted(engineTif));
    INFO(engine.output);
    REQUIRE(engine.exitCode == 0);
    auto delivered = osv::io::readImage(engineTif);
    REQUIRE(delivered.ok());
    const auto& d = delivered.value();
    std::uint32_t worstRow = 0;
    std::size_t worstOpaque = d.w;
    for (std::uint32_t y = 0; y < d.h; ++y) {
        std::size_t opaque = 0;
        for (std::uint32_t x = 0; x < d.w; ++x) {
            opaque += d.data[(static_cast<std::size_t>(y) * d.w + x) * 4u + 3u] >= 0.5f ? 1u : 0u;
        }
        if (opaque < worstOpaque) {
            worstOpaque = opaque;
            worstRow = y;
        }
    }
    INFO("worst row " << worstRow << ": " << worstOpaque << " of " << d.w << " columns opaque");
    CHECK(worstOpaque * 100u >= static_cast<std::size_t>(d.w) * 95u);

    // ---- the flag is documented -------------------------------------------------------------
    CHECK(runTool("render --help").output.find("--alpha") != std::string::npos);
}
