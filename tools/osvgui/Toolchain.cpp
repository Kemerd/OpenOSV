// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Toolchain.cpp - locating and probing osvtool and ffmpeg (see Toolchain.h).

#include "Toolchain.h"

#include "Platform.h"
#include "Queue.h"  // pathToUtf8 / pathFromUtf8
#include "Runner.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace osvgui {

namespace fs = std::filesystem;

namespace {

/// Probe time limits.  Generous: a cold start of a GPU encoder (driver
/// load, first CUDA context) takes a few seconds on a busy machine.
constexpr int kHelpTimeoutMs = 15000;
constexpr int kEncodersTimeoutMs = 15000;
constexpr int kTestEncodeTimeoutMs = 25000;

/// OSV_FFMPEG_EXE, which osvtool honours before PATH.
[[nodiscard]] fs::path ffmpegFromEnvironment() {
#if defined(_WIN32)
    const DWORD needed = ::GetEnvironmentVariableW(L"OSV_FFMPEG_EXE", nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(L"OSV_FFMPEG_EXE", value.data(), needed);
    if (got == 0 || got >= needed) {
        return {};
    }
    value.resize(got);
    return fs::path(value);
#else
    const char* value = std::getenv("OSV_FFMPEG_EXE");
    return (value && *value) ? fs::path(value) : fs::path();
#endif
}

/// Size and last-write time of a file, for the encoder cache key.
void fileStamp(const fs::path& path, std::uint64_t& size, std::int64_t& time) {
    std::error_code ec;
    const std::uintmax_t bytes = fs::file_size(path, ec);
    size = ec ? 0 : static_cast<std::uint64_t>(bytes);
    const fs::file_time_type stamp = fs::last_write_time(path, ec);
    time = ec ? 0 : static_cast<std::int64_t>(stamp.time_since_epoch().count());
}

/// The last non-empty line of some output, for a log note.
[[nodiscard]] std::string lastLine(const std::string& text) {
    std::size_t end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) {
        return {};
    }
    const std::size_t start = text.find_last_of("\r\n", end);
    return text.substr(start == std::string::npos ? 0 : start + 1,
                       end - (start == std::string::npos ? 0 : start + 1) + 1);
}

}  // namespace

// ===========================================================================
//  The probe
// ===========================================================================

ToolchainReport probeToolchain(const GuiSettings& settings) {
    ToolchainReport r;
    try {
        // ---- 1. osvtool ------------------------------------------------------------------
        r.osvtool = platform::findTool("osvtool");
        if (r.osvtool.empty()) {
            r.notes.push_back("osvtool was not found next to OpenOSV Studio or on PATH.");
        } else {
            r.notes.push_back("osvtool: " + pathToUtf8(r.osvtool));
            // ---- 2. what it knows -----------------------------------------------------------
            const CaptureResult help = runCapture(r.osvtool, {"render", "--help"}, kHelpTimeoutMs);
            if (!help.started) {
                r.notes.push_back("osvtool did not start: " + help.error);
            } else if (help.timedOut) {
                r.notes.push_back("osvtool render --help did not answer in time.");
            } else {
                r.caps.sphericalMetadata = helpMentionsSphericalMetadata(help.output);
                r.notes.push_back(std::string("360 metadata option: ") +
                                  (r.caps.sphericalMetadata ? "available" : "not in this osvtool"));
            }
        }

        // ---- 3. ffmpeg --------------------------------------------------------------------------
        if (!settings.ffmpegPath.empty()) {
            const fs::path chosen = pathFromUtf8(settings.ffmpegPath);
            if (platform::isExecutableFile(chosen)) {
                r.ffmpeg = chosen;
                r.ffmpegFromSettings = true;
            } else {
                r.notes.push_back("The FFmpeg set in OpenOSV Studio is gone: " + settings.ffmpegPath);
            }
        }
        if (r.ffmpeg.empty()) {
            // osvtool reads OSV_FFMPEG_EXE and PATH by itself: no --ffmpeg.
            if (const fs::path env = ffmpegFromEnvironment(); platform::isExecutableFile(env)) {
                r.ffmpeg = env;
            } else if (const fs::path onPath = platform::findOnPath("ffmpeg"); !onPath.empty()) {
                r.ffmpeg = onPath;
            }
        }
        if (r.ffmpeg.empty()) {
            // Somewhere osvtool will not look: hand it over with --ffmpeg.
            const std::vector<fs::path> extra = platform::extraFfmpegLocations();
            if (!extra.empty()) {
                r.ffmpeg = extra.front();
                r.caps.ffmpegOverride = pathToUtf8(r.ffmpeg);
            }
        }
        if (r.ffmpeg.empty()) {
            r.notes.push_back("FFmpeg was not found: video output needs it.");
            return r;
        }
        r.notes.push_back("ffmpeg: " + pathToUtf8(r.ffmpeg));

        // ---- 4. the encoder ------------------------------------------------------------------------
        std::uint64_t size = 0;
        std::int64_t time = 0;
        fileStamp(r.ffmpeg, size, time);
        const std::string ffmpegUtf8 = pathToUtf8(r.ffmpeg);

        const CaptureResult list = runCapture(r.ffmpeg, {"-hide_banner", "-encoders"}, kEncodersTimeoutMs);
        if (!list.started || list.timedOut) {
            r.notes.push_back("ffmpeg -encoders failed: " + (list.started ? std::string("no answer") : list.error));
            return r;
        }
        r.hevcEncoders = pickEncoderOrder(parseVideoEncoders(list.output));

        const EncoderCache& cache = settings.encoderCache;
        if (cache.ffmpegPath == ffmpegUtf8 && cache.ffmpegSize == size && cache.ffmpegTime == time) {
            // The same ffmpeg as last time: its answer stands.
            r.caps.autoCodec = cache.codec;
            r.encoderFromCache = true;
        } else {
            // One tiny encode per candidate, best first; the first that
            // works is the one a render will use.
            for (const std::string& codec : r.hevcEncoders) {
                const auto t0 = std::chrono::steady_clock::now();
                const CaptureResult test =
                    runCapture(r.ffmpeg, testEncodeArgs(codec, settings.crf), kTestEncodeTimeoutMs);
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                        .count();
                if (test.started && !test.timedOut && test.exitCode == 0) {
                    r.caps.autoCodec = codec;
                    r.notes.push_back("Encoder test: " + codec + " works (" + std::to_string(ms) + " ms).");
                    break;
                }
                const std::string why = test.timedOut ? std::string("timed out") : lastLine(test.output);
                r.notes.push_back("Encoder test: " + codec + " is not usable here" + (why.empty() ? "." : ": " + why));
            }
        }
        r.encoderCache.ffmpegPath = ffmpegUtf8;
        r.encoderCache.ffmpegSize = size;
        r.encoderCache.ffmpegTime = time;
        r.encoderCache.codec = r.caps.autoCodec;
        r.notes.push_back(r.caps.autoCodec.empty()
                              ? "No HEVC encoder worked; osvtool will pick its own."
                              : "Auto encoder: " + r.caps.autoCodec + (r.encoderFromCache ? " (remembered)" : ""));
    } catch (const std::exception& e) {
        r.notes.push_back(std::string("Probe failed: ") + e.what());
    } catch (...) {
        r.notes.push_back("Probe failed.");
    }
    return r;
}

// ===========================================================================
//  Toolchain
// ===========================================================================

Toolchain::~Toolchain() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Toolchain::start(const GuiSettings& settings) {
    // One probe at a time.  A running one is short (seconds) and only ever
    // replaced after the user picked another ffmpeg, so waiting for it here
    // is rare and brief; its report is overwritten below anyway.
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_report.reset();
    }
    m_probing.store(true, std::memory_order_release);
    try {
        const GuiSettings copy = settings;
        m_thread = std::thread([this, copy] {
            ToolchainReport report = probeToolchain(copy);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_report = std::move(report);
            }
            m_probing.store(false, std::memory_order_release);
        });
    } catch (...) {
        // No thread available: probe inline rather than never.
        ToolchainReport report = probeToolchain(settings);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_report = std::move(report);
        m_probing.store(false, std::memory_order_release);
    }
}

std::optional<ToolchainReport> Toolchain::take() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_report) {
        return std::nullopt;
    }
    std::optional<ToolchainReport> out = std::move(m_report);
    m_report.reset();
    return out;
}

}  // namespace osvgui
