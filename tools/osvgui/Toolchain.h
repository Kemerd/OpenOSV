// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Toolchain.h - finding osvtool and ffmpeg, and learning what they can do,
// off the UI thread.
//
// At start-up (and whenever the ffmpeg setting changes) a worker thread:
//   1. finds osvtool next to this program, else on PATH;
//   2. asks `osvtool render --help` whether it knows the 360 metadata
//      option (--no-spherical-metadata);
//   3. finds ffmpeg: the path in the settings, else OSV_FFMPEG_EXE, else
//      PATH, else the usual install places (Homebrew, winget, ...);
//   4. picks the encoder: `ffmpeg -encoders`, then a tiny test encode with
//      each HEVC candidate, best first, until one works - skipped when the
//      settings remember the answer for this very ffmpeg.
// The UI polls for the report; nothing here touches ImGui.
#pragma once

#include "CommandBuilder.h"
#include "Settings.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace osvgui {

/// What the probe found.
struct ToolchainReport {
    std::filesystem::path osvtool;          ///< Empty: not found.
    std::filesystem::path ffmpeg;           ///< Empty: not found.
    bool ffmpegFromSettings = false;        ///< It is the path the user chose.
    ToolCaps caps;                          ///< Metadata option, auto encoder, --ffmpeg override.
    std::vector<std::string> hevcEncoders;  ///< HEVC encoders this ffmpeg lists, best first.
    EncoderCache encoderCache;              ///< What to remember for next time.
    bool encoderFromCache = false;          ///< The pick came from the settings, not a test.
    std::vector<std::string> notes;         ///< Log lines about what was found and how.
};

/// Runs the probe on a worker thread; one at a time.
class Toolchain {
public:
    Toolchain() = default;
    /// Joins a probe still in flight.
    ~Toolchain();
    Toolchain(const Toolchain&) = delete;
    Toolchain& operator=(const Toolchain&) = delete;

    /// Start probing with these settings (a copy is taken).  A probe still
    /// running finishes first; its report is dropped in favour of this one.
    void start(const GuiSettings& settings);

    /// The newest report, once; nullopt while probing or after it was taken.
    [[nodiscard]] std::optional<ToolchainReport> take();

    /// A probe is in flight.
    [[nodiscard]] bool probing() const noexcept { return m_probing.load(std::memory_order_acquire); }

private:
    std::thread m_thread;
    std::mutex m_mutex;
    std::optional<ToolchainReport> m_report;
    std::atomic<bool> m_probing{false};
};

/// The probe itself, synchronous (the worker's body; exposed for tools).
[[nodiscard]] ToolchainReport probeToolchain(const GuiSettings& settings);

}  // namespace osvgui
