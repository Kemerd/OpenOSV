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
//
// Where the messages go
// ---------------------
// By default to stderr (spdlog), which is right for osvtool and the tests and
// useless inside a host: Premiere has no console, so before setSink() existed
// every library message - FFmpeg's included - vanished there.  A host that
// has its own log (the plug-ins' PluginLog) installs a Sink and from then on
// receives every message that passes the level instead of stderr.
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace osv::log {

/// Severity levels (ascending).
enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 6 };

/// @brief A host's destination for every library message.
///
/// Called with the message's level, the already formatted text (no newline,
/// no timestamp) and the `user` pointer given to setSink(), from whichever
/// thread logged - FFmpeg's frame threads included - so it must be
/// thread-safe.  It must not throw.  It may itself log through osv::log: a
/// message logged from inside the sink, on the sink's own thread, goes to
/// stderr instead of re-entering the sink, so a sink can never recurse.
using Sink = void (*)(Level level, std::string_view text, void* user);

/// @brief Route every message that passes the level to `sink` instead of stderr.
///
/// @param sink  The destination; nullptr restores the built-in stderr logger.
/// @param user  Handed back to `sink` on every call (may be nullptr).
///
/// The (sink, user) pair is swapped as one: a concurrent message sees the old
/// pair or the new one, never a mix.  A message already in flight when the
/// pair is replaced may still reach the old sink, so a sink must stay callable
/// for as long as the module that installed it is loaded.  The minimum level
/// is unchanged; a host mirrors its own level with setLevel().
void setSink(Sink sink, void* user) noexcept;

/// Set the global minimum level.  With a sink installed this is the level
/// the sink receives from; without one, the stderr logger's.
void setLevel(Level level);

/// Current global minimum level.
[[nodiscard]] Level level();

/// True when a message at `level` would be emitted (cheap pre-check for
/// callers that build expensive strings).  One relaxed atomic load: it
/// follows setLevel(), whether the message would go to a sink or to stderr.
[[nodiscard]] bool enabled(Level level);

/// Emit an already formatted message (to the sink when one is installed).
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
