// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Progress.h - reading osvtool's output: splitting the byte stream into
// lines, recognising its progress lines and its errors.
//
// osvtool render prints, on stderr, every ten frames and at the last one:
//
//     "  12/65 frames  14.2 fps  (cuda)"
//
// (tools/osvtool/CmdRender.cpp), errors as "error: <message>", and its log
// through spdlog as "[12:34:56.789] [osv] [error] <message>".  Pure code,
// unit-tested; Runner feeds it from the child's pipe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osvgui {

/// One parsed progress line.
struct ProgressLine {
    std::uint32_t done = 0;   ///< Frames finished.
    std::uint32_t total = 0;  ///< Frames in the render.
    double fps = 0.0;         ///< Average speed since the render started.
    std::string device;       ///< The renderer osvtool named ("cuda", "opencl", "cpu", ...).
};

/// The progress line in `line`, or nullopt when it is not one.  Tolerates
/// leading / trailing whitespace and a trailing '\r'.
[[nodiscard]] std::optional<ProgressLine> parseProgressLine(std::string_view line) noexcept;

/// Seconds left at the line's own average speed; negative when unknown
/// (no speed yet, or nothing left to measure).
[[nodiscard]] double etaSeconds(const ProgressLine& progress) noexcept;

/// The message of an error line - "error: <msg>" or a log line at error /
/// critical level - or nullopt for any other line.
[[nodiscard]] std::optional<std::string> errorMessage(std::string_view line);

/// `line` without ANSI escape sequences (a coloured log line from a tool
/// that thinks it writes to a terminal).
[[nodiscard]] std::string stripAnsi(std::string_view line);

/// Cuts a byte stream into lines at '\n', '\r\n' or a lone '\r' (a tool
/// redrawing a status line), with overlong lines split at `maxLine` bytes
/// so one runaway line cannot grow without bound.
class LineSplitter {
public:
    explicit LineSplitter(std::size_t maxLine = 16 * 1024)
        : m_maxLine(maxLine == 0 ? 1 : maxLine) {}

    /// Feed bytes; every completed line is appended to `out` (without its
    /// terminator, ANSI escapes removed).
    void feed(const char* data, std::size_t size, std::vector<std::string>& out);

    /// End of stream: the unterminated rest, if any, becomes a line.
    void finish(std::vector<std::string>& out);

private:
    std::string m_pending;
    std::size_t m_maxLine;
    bool m_lastWasCr = false;  ///< The previous byte ended a line with '\r' (so a '\n' next is part of it).
};

}  // namespace osvgui
