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
    // named as such; the default reaches past it.
    CHECK(d.data != b.data);
    CHECK(b.maxValue <= 0.65262);
    CHECK(d.maxValue > 0.70);
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

// [WP-HDRPEAK] osvtool render --hdr-peak on the sample clip: the PQ still
// never passes the chosen peak, its sidecar records it, and a bad value is
// refused.
TEST_CASE("osvtool render --hdr-peak caps the PQ still at the chosen peak", "[cli][hdrpeak][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {
        SKIP("osvtool not built");
    }
    const std::string clip = quoted(osvtest::sampleOsv());
    // Frame 30: the sunlit white aircraft beside the camera.
    const std::string common = " --frame 30 --mode equirect --size 512x256 --device cpu --color pq --out ";
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
    // ...is exactly the render those options spell out by hand.
    const Rendered byHand =
        render("cli_ud_byhand.tif", " --device cpu --color 709 --look standard --exposure 0.5", false);
    CHECK(fromDefaults.pixels == byHand.pixels);

    // An option given on the command line wins over the saved default.
    const Rendered overridden = render("cli_ud_override.tif", " --use-user-defaults --look dji", true);
    const Rendered overriddenByHand =
        render("cli_ud_override_byhand.tif", " --device cpu --color 709 --look dji --exposure 0.5", false);
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
