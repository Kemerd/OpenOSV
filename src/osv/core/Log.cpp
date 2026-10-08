// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// spdlog-backed implementation of the logging facade.  spdlog is a private
// dependency of osv_core; nothing in the public headers mentions it.
//
// Two destinations: a host's Sink (setSink), or the private stderr logger
// below.  The level lives in one atomic so enabled() - asked before every
// message is formatted - costs a single load either way.

#include "osv/core/Log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <mutex>

namespace osv::log {

namespace {

spdlog::level::level_enum toSpdlog(Level level) {
    switch (level) {
    case Level::Trace: return spdlog::level::trace;
    case Level::Debug: return spdlog::level::debug;
    case Level::Info: return spdlog::level::info;
    case Level::Warn: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
    case Level::Off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

/// The logger used by the whole library.  Created lazily so static
/// initialisation order never matters; writes to stderr so stdout stays free
/// for machine readable output (JSON, raw frames over a pipe).
///
/// The logger is deliberately NOT put into spdlog's global registry.
/// spdlog lives in its own DLL, so that registry is shared by every module in
/// the process, and `spdlog::stderr_color_mt("osv")` THROWS
/// spdlog_ex("logger with name 'osv' already exists") when a second module
/// that statically links this library initialises its own copy.  That is not
/// hypothetical: Premiere Pro loads the OpenOSV importer and the Open 360
/// Reframe effect into one process, and a host can load and unload a plug-in
/// repeatedly (spdlog.dll stays resident across the unload, so the stale
/// registry entry outlives the module that made it).  Each module owning a
/// private, unregistered logger is correct for every caller: the name is only
/// used for spdlog's own lookup API, which nothing in this project uses.
spdlog::logger& logger() {
    static std::shared_ptr<spdlog::logger> instance = [] {
        auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto l = std::make_shared<spdlog::logger>("osv", std::move(sink));
        l->set_pattern("[%H:%M:%S.%e] [osv] [%^%l%$] %v");
        l->set_level(spdlog::level::info);
        return l;
    }();
    return *instance;
}

// -----------------------------------------------------------------------------
//  The level and the host's sink
// -----------------------------------------------------------------------------

/// The minimum level, read on every enabled() without a lock.  Info is the
/// stderr logger's own start level, so nothing changes for a process that
/// never calls setLevel().
std::atomic<int> g_level{static_cast<int>(Level::Info)};

/// The installed (sink, user) pair.  Both halves change together under the
/// mutex and are copied out together under it, so a message can never pair
/// one host's sink with another's user pointer.  A mutex rather than a
/// 16-byte atomic: it is portable to every toolchain the plug-ins build with,
/// and message() only runs once enabled() has already said yes - it is never
/// on a per-pixel path.
struct SinkSlot {
    Sink fn = nullptr;
    void* user = nullptr;
};

/// One mutex and slot per module (osv_core is linked statically into each).
struct SinkState {
    std::mutex mutex;
    SinkSlot slot;
};

SinkState& sinkState() {
    static SinkState s;
    return s;
}

/// Fast "is any sink installed" flag, so the common no-sink case (osvtool,
/// the tests) never touches the mutex.
std::atomic<bool> g_haveSink{false};

/// Set while this thread is inside the sink: a message the sink itself logs
/// through osv::log (osvtool mirrors the plug-ins' lines into osv::log) goes
/// to stderr instead of re-entering the sink and recursing without end.
thread_local bool t_inSink = false;

/// The stderr path, also the fallback for anything a sink cannot take.
void toStderr(Level level, std::string_view text) {
    logger().log(toSpdlog(level), "{}", text);
}

}  // namespace

void setSink(Sink sink, void* user) noexcept {
    try {
        SinkState& s = sinkState();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.slot.fn = sink;
        s.slot.user = sink ? user : nullptr;
        g_haveSink.store(sink != nullptr, std::memory_order_release);
    } catch (...) {
        // std::mutex::lock only throws on a broken system; the previous sink
        // then simply stays in place.
    }
}

void setLevel(Level level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
    // The stderr logger keeps its own copy so its should_log agrees.
    logger().set_level(toSpdlog(level));
}

Level level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

bool enabled(Level level) {
    if (level == Level::Off) {
        return false;
    }
    return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed);
}

void message(Level level, std::string_view text) {
    // ---- a host's sink, when one is installed and this is not a re-entry ---
    if (g_haveSink.load(std::memory_order_acquire) && !t_inSink) {
        SinkSlot slot;
        {
            SinkState& s = sinkState();
            std::lock_guard<std::mutex> lock(s.mutex);
            slot = s.slot;
        }
        if (slot.fn != nullptr) {
            // The guard is cleared on every way out, so one throwing sink (the
            // contract says it must not) cannot silence the next message.
            struct InSinkGuard {
                InSinkGuard() noexcept { t_inSink = true; }
                ~InSinkGuard() { t_inSink = false; }
                InSinkGuard(const InSinkGuard&) = delete;
                InSinkGuard& operator=(const InSinkGuard&) = delete;
            } guard;
            slot.fn(level, text, slot.user);
            return;
        }
    }
    // ---- no sink (or a message from inside one): stderr ----------------------
    toStderr(level, text);
}

std::string safe(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back((u >= 0x80 || (u < 0x20 && u != '\t')) ? '?' : c);
    }
    return out;
}

}  // namespace osv::log
