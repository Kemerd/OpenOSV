// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The osvtool contract the VEGAS extension relies on:
//   * `probe <file> --json -` puts ONLY the JSON on stdout, with a stable
//     top-level subset;
//   * `extract <file> --audio out.wav` writes the importer AudioDecoder's
//     samples, bit for bit, as a 32-bit float WAV.
// Everything here is CPU-only: probing reads the container, and AAC decoding
// is plain FFmpeg.  Nothing opens NVDEC, CUDA or D3D11VA.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"
#include "WavTestUtil.h"

#include "ImporterAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
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
#include <sys/wait.h>
#include <unistd.h>  // getpid
#endif

namespace {

#if defined(OSV_TOOL_PATH)
const char* const kToolPath = OSV_TOOL_PATH;
#else
const char* const kToolPath = "";
#endif

/// What one osvtool run produced, with the two streams kept apart.
struct Run {
    int exitCode = -1;
    std::string out;  ///< stdout, byte for byte
    std::string err;  ///< stderr
};

std::string readFileBytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string quoted(const std::filesystem::path& p) { return "\"" + p.string() + "\""; }

/// A file in the shared test-output folder that only THIS process uses.
/// ctest runs every test case in a process of its own, several at a time
/// (-j), all in one folder: a fixed name there makes two runs delete and
/// rewrite each other's files.  The process id keeps them apart.
std::filesystem::path scratch(const std::string& stem, const std::string& extension) {
#if defined(_WIN32)
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    return osvtest::tempDir() / (stem + "-" + std::to_string(pid) + extension);
}

/// Run osvtool with stdout and stderr captured separately.  Both go to files
/// so a full pipe can never stall the child.
Run runTool(const std::string& args) {
    Run r;
    const auto outFile = scratch("vegas_cli_stdout", ".bin");
    const auto errFile = scratch("vegas_cli_stderr", ".txt");
    std::filesystem::remove(outFile);
    std::filesystem::remove(errFile);
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    auto openForChild = [&sa](const std::filesystem::path& p) {
        return CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    HANDLE hOut = openForChild(outFile);
    HANDLE hErr = openForChild(errFile);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        return r;
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = std::string("\"") + kToolPath + "\" " + args;
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    const BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exitCode = static_cast<int>(code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hOut);
    CloseHandle(hErr);
#else
    const std::string cmd = std::string("\"") + kToolPath + "\" " + args + " >" + quoted(outFile) + " 2>" + quoted(errFile);
    const int status = std::system(cmd.c_str());
    if (status != -1 && WIFEXITED(status)) {
        r.exitCode = WEXITSTATUS(status);
    }
#endif
    r.out = readFileBytes(outFile);
    r.err = readFileBytes(errFile);
    return r;
}

/// SKIP when there is no osvtool to run.
#define REQUIRE_TOOL()                                                                                                 \
    do {                                                                                                               \
        if (std::string(kToolPath).empty() || !std::filesystem::exists(kToolPath)) {                                   \
            SKIP("osvtool not built");                                                                                 \
        }                                                                                                              \
    } while (0)

}  // namespace

TEST_CASE("osvtool probe --json - writes only the JSON to stdout", "[cli][io][sample]") {
    OSV_REQUIRE_SAMPLE();
    REQUIRE_TOOL();

    const Run r = runTool("probe " + quoted(osvtest::sampleOsv()) + " --json -");
    INFO(r.err);
    REQUIRE(r.exitCode == 0);
    REQUIRE_FALSE(r.out.empty());

    // No BOM, no console summary, LF line endings only.
    CHECK(static_cast<unsigned char>(r.out[0]) == '{');
    CHECK(r.out.find('\r') == std::string::npos);
    CHECK(r.out.back() == '\n');
    // The human summary went to stderr instead.
    CHECK(r.err.find("container:") != std::string::npos);
    CHECK(r.out.find("container:") == std::string::npos);

    const nlohmann::json j = nlohmann::json::parse(r.out, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    REQUIRE(j.is_object());

    // ---- the stable subset --------------------------------------------------------
    CHECK(j.at("schema").get<std::string>() == "openosv.probe/1");
    CHECK(j.at("path").get<std::string>() == osvtest::sampleOsv().string());
    CHECK(j.at("frameCount").get<std::uint32_t>() > 0);
    CHECK(j.at("durationSeconds").get<double>() > 0.0);
    CHECK(j.at("streamW").get<std::uint32_t>() > 0);
    CHECK(j.at("streamH").get<std::uint32_t>() > 0);
    CHECK(j.at("mode").get<std::string>() == "K6");
    CHECK(j.at("colorModeName").get<std::string>() == "DLogM");
    CHECK(j.at("isLrf").get<bool>() == false);

    // fps is an exact reduced rational and value agrees with it.
    const auto& fps = j.at("fps");
    REQUIRE(fps.is_object());
    const std::uint64_t num = fps.at("num").get<std::uint64_t>();
    const std::uint64_t den = fps.at("den").get<std::uint64_t>();
    REQUIRE(num > 0);
    REQUIRE(den > 0);
    CHECK(std::gcd(num, den) == 1);
    CHECK(fps.at("value").get<double>() == static_cast<double>(num) / static_cast<double>(den));
    // frames / seconds is the same rate to within one frame of rounding.
    const double implied = j.at("frameCount").get<double>() / j.at("durationSeconds").get<double>();
    CHECK(std::abs(implied - fps.at("value").get<double>()) < 0.05 * fps.at("value").get<double>());

    // Audio: the clip has a track, and the probe agrees with the decoder about it.
    CHECK(j.at("hasAudio").get<bool>() == true);
    const auto& audio = j.at("audio");
    REQUIRE(audio.is_object());
    auto decoder = osv::premiere::AudioDecoder::open(osvtest::sampleOsv());
    REQUIRE(decoder.ok());
    CHECK(audio.at("sampleRate").get<double>() == decoder.value().sampleRate());
    CHECK(audio.at("channels").get<int>() == decoder.value().channels());
    CHECK(audio.at("sampleCount").get<std::int64_t>() == decoder.value().durationSamples());

    // Existing keys are all still there.
    for (const char* key : {"file", "size", "container", "format", "clip", "stream", "calibration", "frames",
                            "warnings", "metaTrackId", "djmdSchema"}) {
        INFO(key);
        CHECK(j.contains(key));
    }
}

TEST_CASE("osvtool probe --json <path> still writes a file and prints the summary", "[cli][io][sample]") {
    OSV_REQUIRE_SAMPLE();
    REQUIRE_TOOL();

    const auto path = scratch("vegas_probe_file", ".json");
    std::filesystem::remove(path);
    const Run r = runTool("probe " + quoted(osvtest::sampleOsv()) + " --json " + quoted(path));
    REQUIRE(r.exitCode == 0);
    CHECK(r.out.find("container:") != std::string::npos);
    CHECK(r.out.find("wrote ") != std::string::npos);
    const nlohmann::json j = nlohmann::json::parse(readFileBytes(path), nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    CHECK(j.at("schema").get<std::string>() == "openosv.probe/1");
}

TEST_CASE("osvtool probe --json - leaves stdout empty on failure", "[cli][io]") {
    REQUIRE_TOOL();
    const Run r = runTool("probe " + quoted(osvtest::tempDir() / "no_such_clip.OSV") + " --json -");
    CHECK(r.exitCode != 0);
    CHECK(r.out.empty());
    CHECK_FALSE(r.err.empty());
}

TEST_CASE("osvtool probe of the LRF proxy reports isLrf", "[cli][io][sample]") {
    OSV_REQUIRE_SAMPLE_LRF();
    REQUIRE_TOOL();
    const Run r = runTool("probe " + quoted(osvtest::sampleLrf()) + " --json -");
    INFO(r.err);
    REQUIRE(r.exitCode == 0);
    const nlohmann::json j = nlohmann::json::parse(r.out, nullptr, false);
    REQUIRE_FALSE(j.is_discarded());
    CHECK(j.at("isLrf").get<bool>() == true);
    CHECK(j.at("frameCount").get<std::uint32_t>() > 0);
    CHECK(j.at("fps").at("num").get<std::uint64_t>() > 0);
}

TEST_CASE("osvtool extract --audio .wav is the AudioDecoder's output, bit for bit", "[cli][io][sample]") {
    OSV_REQUIRE_SAMPLE();
    REQUIRE_TOOL();

    const auto wavPath = scratch("vegas_extract_audio", ".wav");
    std::filesystem::remove(wavPath);
    const Run r = runTool("extract " + quoted(osvtest::sampleOsv()) + " --audio " + quoted(wavPath));
    INFO(r.out);
    INFO(r.err);
    REQUIRE(r.exitCode == 0);
    // No temp file is left behind.
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(wavPath.string() + ".partial")));

    // ---- the reference: the importer's own decoder --------------------------------------
    auto opened = osv::premiere::AudioDecoder::open(osvtest::sampleOsv());
    REQUIRE(opened.ok());
    osv::premiere::AudioDecoder& dec = opened.value();
    const std::int64_t total = dec.durationSamples();
    const std::size_t channels = static_cast<std::size_t>(dec.channels());
    REQUIRE(total > 0);
    REQUIRE(channels > 0);

    // ---- the header -------------------------------------------------------------------------
    const osvtest::WavFile wav = osvtest::readWav(wavPath);
    INFO(wav.why);
    REQUIRE(wav.ok);
    CHECK(wav.format == 3);
    CHECK(wav.bits == 32);
    CHECK(wav.channels == channels);
    CHECK(static_cast<double>(wav.sampleRate) == dec.sampleRate());
    CHECK(static_cast<std::int64_t>(wav.factFrames) == total);
    CHECK(static_cast<std::int64_t>(wav.dataBytes) == total * static_cast<std::int64_t>(channels) * 4);

    // ---- every sample, in blocks, against a fresh decoder ------------------------------------
    // Random access read() for the head (the path Premiere's host takes for
    // the first request), then the sequential cursor for the rest.
    constexpr std::uint32_t kBlock = 4096;
    std::vector<std::vector<float>> planes(channels, std::vector<float>(kBlock));
    std::vector<float*> ptrs(channels);
    for (std::size_t c = 0; c < channels; ++c) {
        ptrs[c] = planes[c].data();
    }
    std::int64_t mismatches = 0;
    for (std::int64_t done = 0; done < total; done += kBlock) {
        const auto want = static_cast<std::uint32_t>(std::min<std::int64_t>(kBlock, total - done));
        REQUIRE(dec.read(done, want, ptrs.data()).ok());
        for (std::uint32_t i = 0; i < want; ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                const float got = osvtest::wavSample(wav, static_cast<std::size_t>(done + i) * channels + c);
                if (std::memcmp(&got, &planes[c][i], sizeof(float)) != 0) {
                    ++mismatches;
                }
            }
        }
    }
    CHECK(mismatches == 0);

    // The first N samples explicitly, as the sync-critical part (priming already discarded).
    REQUIRE(dec.read(0, 1024, ptrs.data()).ok());
    bool headIdentical = true;
    for (std::uint32_t i = 0; i < 1024; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            const float got = osvtest::wavSample(wav, static_cast<std::size_t>(i) * channels + c);
            headIdentical = headIdentical && std::memcmp(&got, &planes[c][i], sizeof(float)) == 0;
        }
    }
    CHECK(headIdentical);
}

TEST_CASE("osvtool extract --audio .aac keeps writing ADTS", "[cli][io][sample]") {
    OSV_REQUIRE_SAMPLE();
    REQUIRE_TOOL();
    const auto aac = scratch("vegas_extract_audio", ".aac");
    std::filesystem::remove(aac);
    const Run r = runTool("extract " + quoted(osvtest::sampleOsv()) + " --audio " + quoted(aac));
    REQUIRE(r.exitCode == 0);
    const std::string bytes = readFileBytes(aac);
    REQUIRE(bytes.size() > 7);
    // ADTS syncword: 12 set bits.
    CHECK(static_cast<unsigned char>(bytes[0]) == 0xFF);
    CHECK((static_cast<unsigned char>(bytes[1]) & 0xF0) == 0xF0);
}
