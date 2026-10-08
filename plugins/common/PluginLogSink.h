// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PluginLogSink.h - the library's log (osv::log) routed into PluginLog.
//
// PRIVATE to PluginLog.cpp and PluginLogPosix.cpp (exactly one of the two is
// compiled into a module); nothing else includes it.  It lives in a header so
// the two platform files share one implementation of the policy below.
//
// WHY
// ---
// osv::log - every message of the library, FFmpeg's included (Decoder.cpp
// forwards av_log into it) - used to reach only a stderr logger.  Inside
// Premiere there is no stderr anybody reads, so a diagnosis of a damaged
// proxy frame found 0 'ffmpeg:' and 0 'video: opened track' lines in a
// session log that was otherwise at Debug.  PluginLog::init() now installs
// forwardLibraryMessage() as the library's sink, in every module that logs
// through PluginLog, and keeps the library's level equal to PluginLog's.
//
// POLICY
// ------
//   * Library lines keep their own text; PluginLog adds the timestamp, pid
//     and tid like for any other line.
//   * FFmpeg lines ('ffmpeg: [codec 'clip'] ...') reach the file at WARNING
//     and above only: FFmpeg's informational chatter is per stream and
//     would bury everything else at Debug.
//   * FFmpeg warnings and errors repeat per packet ("error while decoding MB
//     12 34", then 13 34, ...): each one is written once per clip and SHAPE -
//     the message with every run of digits replaced by '#' - through
//     PluginLog::once.  The decoder's own per-frame damage lines carry the
//     frame numbers, so nothing is lost but the repeats.
#pragma once

#include "PluginLog.h"

#include "osv/core/Log.h"

#include <atomic>
#include <string>
#include <string_view>

namespace osv::premiere::detail {

/// @brief PluginLog's level for a library level (Off for anything unknown).
[[nodiscard]] inline PluginLog::Level pluginLevelOf(osv::log::Level level) noexcept {
    switch (level) {
    case osv::log::Level::Trace: return PluginLog::Level::Trace;
    case osv::log::Level::Debug: return PluginLog::Level::Debug;
    case osv::log::Level::Info:  return PluginLog::Level::Info;
    case osv::log::Level::Warn:  return PluginLog::Level::Warn;
    case osv::log::Level::Error: return PluginLog::Level::Error;
    case osv::log::Level::Off:
    default:                     return PluginLog::Level::Off;
    }
}

/// @brief The library's level for a PluginLog level.
[[nodiscard]] inline osv::log::Level libraryLevelOf(PluginLog::Level level) noexcept {
    switch (level) {
    case PluginLog::Level::Trace: return osv::log::Level::Trace;
    case PluginLog::Level::Debug: return osv::log::Level::Debug;
    case PluginLog::Level::Info:  return osv::log::Level::Info;
    case PluginLog::Level::Warn:  return osv::log::Level::Warn;
    case PluginLog::Level::Error: return osv::log::Level::Error;
    case PluginLog::Level::Off:
    default:                      return osv::log::Level::Off;
    }
}

/// The prefix Decoder.cpp gives every FFmpeg line.
inline constexpr std::string_view kFfmpegPrefix = "ffmpeg: ";

/// @brief The PluginLog::once key of an FFmpeg line: its level, its tag
/// ("[h264 'clip']") verbatim and its message with every digit run as '#'.
///
/// The tag stays verbatim so two clips never share a key (clip names are
/// full of digits); only the message part is folded, which is where the
/// per-packet numbers - macroblock coordinates, POCs, sizes - live.
///
/// @param level  The line's level (a warning and an error of the same shape
///               are two keys).
/// @param text   The whole line, starting with kFfmpegPrefix.
/// @return The key.
[[nodiscard]] inline std::string ffmpegThrottleKey(PluginLog::Level level, std::string_view text) {
    std::string key = "ffmpeg/";
    key += std::to_string(static_cast<int>(level));
    key += '/';
    // ---- the tag, verbatim -----------------------------------------------------
    std::size_t body = 0;
    if (text.size() > kFfmpegPrefix.size() && text[kFfmpegPrefix.size()] == '[') {
        const std::size_t close = text.find("] ", kFfmpegPrefix.size());
        if (close != std::string_view::npos) {
            body = close + 2;
        }
    }
    key.append(text.data(), body);
    // ---- the message, digit runs folded --------------------------------------
    bool inDigits = false;
    for (std::size_t i = body; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            if (!inDigits) {
                key.push_back('#');
                inDigits = true;
            }
            continue;
        }
        inDigits = false;
        key.push_back(c);
    }
    return key;
}

/// @brief The osv::log::Sink PluginLog installs: one library line into this
/// module's PluginLog, with the FFmpeg policy described at the top.
///
/// Never throws (the sink contract): a line that cannot be built is dropped.
inline void forwardLibraryMessage(osv::log::Level level, std::string_view text, void* /*user*/) {
    try {
        const PluginLog::Level mapped = pluginLevelOf(level);
        if (mapped == PluginLog::Level::Off || !PluginLog::enabled(mapped)) {
            return;
        }
        // ---- FFmpeg: warnings and errors only, each shape once per clip ----------
        if (text.substr(0, kFfmpegPrefix.size()) == kFfmpegPrefix) {
            if (static_cast<int>(mapped) < static_cast<int>(PluginLog::Level::Warn)) {
                return;
            }
            (void)PluginLog::once(ffmpegThrottleKey(mapped, text), mapped, text);
            return;
        }
        // ---- everything else, as it is ----------------------------------------------
        PluginLog::write(mapped, text);
    } catch (...) {
        // Dropping a line is the only acceptable failure of a log sink.
    }
}

/// Whether this module's PluginLog::init() has installed the sink, so
/// PluginLog::setLevel() knows to carry the level over.  One flag per module
/// (an inline function's static), like the sink itself.
[[nodiscard]] inline std::atomic<bool>& librarySinkInstalled() noexcept {
    static std::atomic<bool> installed{false};
    return installed;
}

/// @brief Give the library PluginLog's level (only once the sink is ours).
///
/// Without the sink - osvtool, which runs the clip engine but never calls
/// PluginLog::init() - the library's level belongs to the host's own -v
/// flags and is left alone.
inline void mirrorLibraryLevel(PluginLog::Level level) noexcept {
    if (librarySinkInstalled().load(std::memory_order_acquire)) {
        osv::log::setLevel(libraryLevelOf(level));
    }
}

/// @brief Route the library's log into PluginLog from now on (init()).
///
/// Installs forwardLibraryMessage as osv::log's sink and gives the library
/// PluginLog's current level, so a Debug session also gets the library's
/// Debug lines ('video: opened track ...', the per-picture 'decode:' lines).
/// Idempotent: every init() call may repeat it.
inline void installLibrarySink() noexcept {
    osv::log::setSink(&forwardLibraryMessage, nullptr);
    librarySinkInstalled().store(true, std::memory_order_release);
    mirrorLibraryLevel(PluginLog::level());
}

}  // namespace osv::premiere::detail
