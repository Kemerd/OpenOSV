// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Progress.cpp - osvtool output parsing (see Progress.h).

#include "Progress.h"

#include <cmath>

namespace osvgui {

namespace {

/// `text` without leading and trailing blanks (space, tab, CR, LF).
[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Skip spaces and tabs.
void skipBlanks(std::string_view& text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
}

/// Consume `word` when `text` starts with it.
[[nodiscard]] bool eat(std::string_view& text, std::string_view word) noexcept {
    if (text.substr(0, word.size()) != word) {
        return false;
    }
    text.remove_prefix(word.size());
    return true;
}

/// Consume an unsigned decimal integer (at most nine digits, so it always
/// fits a uint32 without an overflow check per digit).
[[nodiscard]] bool eatUint(std::string_view& text, std::uint32_t& value) noexcept {
    std::size_t digits = 0;
    std::uint32_t v = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        if (digits == 9) {
            return false;  // a ten-digit frame count is not a progress line
        }
        v = v * 10u + static_cast<std::uint32_t>(text[digits] - '0');
        ++digits;
    }
    if (digits == 0) {
        return false;
    }
    text.remove_prefix(digits);
    value = v;
    return true;
}

/// Consume a non-negative decimal number with an optional fraction
/// ("14", "14.2").  Parsed by hand, so no locale can turn '.' into ','.
[[nodiscard]] bool eatDecimal(std::string_view& text, double& value) noexcept {
    std::uint32_t whole = 0;
    if (!eatUint(text, whole)) {
        return false;
    }
    double v = static_cast<double>(whole);
    if (!text.empty() && text.front() == '.') {
        text.remove_prefix(1);
        double scale = 0.1;
        std::size_t digits = 0;
        while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
            v += scale * static_cast<double>(text.front() - '0');
            scale *= 0.1;
            text.remove_prefix(1);
            ++digits;
        }
        if (digits == 0) {
            return false;  // "14." is not what osvtool prints
        }
    }
    value = v;
    return true;
}

}  // namespace

std::optional<ProgressLine> parseProgressLine(std::string_view line) noexcept {
    try {
        std::string_view text = trim(line);
        ProgressLine p;

        // "12/65"
        if (!eatUint(text, p.done) || !eat(text, "/") || !eatUint(text, p.total) || p.total == 0) {
            return std::nullopt;
        }
        // "frames"
        skipBlanks(text);
        if (!eat(text, "frames")) {
            return std::nullopt;
        }
        // "14.2 fps"
        skipBlanks(text);
        if (!eatDecimal(text, p.fps) || !std::isfinite(p.fps)) {
            return std::nullopt;
        }
        skipBlanks(text);
        if (!eat(text, "fps")) {
            return std::nullopt;
        }
        // "(cuda)" - optional; the name itself may hold parentheses, so it
        // runs to the LAST closing one.
        skipBlanks(text);
        if (!text.empty() && text.front() == '(') {
            const std::size_t close = text.rfind(')');
            if (close == std::string_view::npos || close == 0 || !trim(text.substr(close + 1)).empty()) {
                return std::nullopt;  // unclosed, or something after it: not ours
            }
            p.device = std::string(trim(text.substr(1, close - 1)));
        } else if (!text.empty()) {
            return std::nullopt;  // something else follows: not ours
        }
        return p;
    } catch (...) {
        return std::nullopt;
    }
}

double etaSeconds(const ProgressLine& progress) noexcept {
    if (!(progress.fps > 0.0) || !std::isfinite(progress.fps) || progress.total == 0 ||
        progress.done > progress.total) {
        return -1.0;
    }
    return static_cast<double>(progress.total - progress.done) / progress.fps;
}

std::optional<std::string> errorMessage(std::string_view line) {
    const std::string clean = stripAnsi(line);
    const std::string_view text = trim(clean);
    if (text.empty()) {
        return std::nullopt;
    }
    // osvtool's own "error: <message>".
    if (text.substr(0, 6) == "error:") {
        const std::string_view message = trim(text.substr(6));
        return std::string(message.empty() ? text : message);
    }
    // A log line at error or critical level: "[time] [osv] [error] <message>".
    for (const std::string_view marker : {std::string_view("[error] "), std::string_view("[critical] ")}) {
        const std::size_t at = text.find(marker);
        if (at != std::string_view::npos) {
            const std::string_view message = trim(text.substr(at + marker.size()));
            return std::string(message.empty() ? text : message);
        }
    }
    return std::nullopt;
}

std::string stripAnsi(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c != '\x1b') {
            out.push_back(c);
            continue;
        }
        // ESC [ parameters... final byte (0x40..0x7E): a CSI sequence.
        if (i + 1 < line.size() && line[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < line.size() && !(line[j] >= 0x40 && line[j] <= 0x7E)) {
                ++j;
            }
            i = j;  // the loop's ++i steps past the final byte
            continue;
        }
        // ESC and one more character (a two-byte sequence).
        ++i;
    }
    return out;
}

void LineSplitter::feed(const char* data, std::size_t size, std::vector<std::string>& out) {
    if (!data) {
        return;
    }
    const auto emit = [&]() {
        std::string line = stripAnsi(m_pending);
        m_pending.clear();
        if (!trim(line).empty()) {
            out.push_back(std::move(line));
        }
    };
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        // The '\n' of a "\r\n" pair: the line ended at the '\r' already.
        if (m_lastWasCr && c == '\n') {
            m_lastWasCr = false;
            continue;
        }
        m_lastWasCr = false;
        if (c == '\n' || c == '\r') {
            emit();
            m_lastWasCr = c == '\r';
            continue;
        }
        m_pending.push_back(c);
        if (m_pending.size() >= m_maxLine) {
            emit();
        }
    }
}

void LineSplitter::finish(std::vector<std::string>& out) {
    if (!m_pending.empty()) {
        std::string line = stripAnsi(m_pending);
        m_pending.clear();
        if (!trim(line).empty()) {
            out.push_back(std::move(line));
        }
    }
    m_lastWasCr = false;
}

}  // namespace osvgui
