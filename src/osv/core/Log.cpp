// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// spdlog-backed implementation of the logging facade.  spdlog is a private
// dependency of osv_core; nothing in the public headers mentions it.

#include "osv/core/Log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

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

Level fromSpdlog(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace: return Level::Trace;
    case spdlog::level::debug: return Level::Debug;
    case spdlog::level::info: return Level::Info;
    case spdlog::level::warn: return Level::Warn;
    case spdlog::level::err:
    case spdlog::level::critical: return Level::Error;
    default: return Level::Off;
    }
}

/// The logger used by the whole library.  Created lazily so static
/// initialisation order never matters; writes to stderr so stdout stays free
/// for machine readable output (JSON, raw frames over a pipe).
spdlog::logger& logger() {
    static std::shared_ptr<spdlog::logger> instance = [] {
        auto l = spdlog::stderr_color_mt("osv");
        l->set_pattern("[%H:%M:%S.%e] [osv] [%^%l%$] %v");
        l->set_level(spdlog::level::info);
        return l;
    }();
    return *instance;
}

}  // namespace

void setLevel(Level level) { logger().set_level(toSpdlog(level)); }

Level level() { return fromSpdlog(logger().level()); }

bool enabled(Level level) { return logger().should_log(toSpdlog(level)); }

void message(Level level, std::string_view text) { logger().log(toSpdlog(level), "{}", text); }

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
