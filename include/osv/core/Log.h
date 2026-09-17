// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Logging facade.  The library never prints directly; everything goes through
// these functions so a host application (or the CLI) can route messages
// wherever it wants and silence them entirely.  The public header depends only
// on the standard library (std::format); the spdlog sink lives in Log.cpp.
//
// Output is kept 7-bit clean by convention: file names are the only user data
// that may contain non-ASCII and callers pass them through `log::safe`.
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace osv::log {

/// Severity levels (ascending).
enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 6 };

/// Set the global minimum level.
void setLevel(Level level);

/// Current global minimum level.
[[nodiscard]] Level level();

/// True when a message at `level` would be emitted (cheap pre-check for
/// callers that build expensive strings).
[[nodiscard]] bool enabled(Level level);

/// Emit an already formatted message.
void message(Level level, std::string_view text);

/// Replace every byte >= 0x80 (and control characters) with '?' so console
/// output never emits raw UTF-8 on a code page that cannot show it.
[[nodiscard]] std::string safe(std::string_view text);

template <class... Args>
void trace(std::format_string<Args...> fmtStr, Args&&... args) {
    if (enabled(Level::Trace)) {
        message(Level::Trace, std::format(fmtStr, std::forward<Args>(args)...));
    }
}
template <class... Args>
void debug(std::format_string<Args...> fmtStr, Args&&... args) {
    if (enabled(Level::Debug)) {
        message(Level::Debug, std::format(fmtStr, std::forward<Args>(args)...));
    }
}
template <class... Args>
void info(std::format_string<Args...> fmtStr, Args&&... args) {
    if (enabled(Level::Info)) {
        message(Level::Info, std::format(fmtStr, std::forward<Args>(args)...));
    }
}
template <class... Args>
void warn(std::format_string<Args...> fmtStr, Args&&... args) {
    if (enabled(Level::Warn)) {
        message(Level::Warn, std::format(fmtStr, std::forward<Args>(args)...));
    }
}
template <class... Args>
void error(std::format_string<Args...> fmtStr, Args&&... args) {
    if (enabled(Level::Error)) {
        message(Level::Error, std::format(fmtStr, std::forward<Args>(args)...));
    }
}

}  // namespace osv::log
