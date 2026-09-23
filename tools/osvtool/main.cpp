// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// osvtool entry point.
//
// Responsibilities of this file only:
//   * turn the wide-character Windows argv into UTF-8,
//   * put the console into UTF-8 mode,
//   * build the CLI11 application, register every sub-command and run it.
//
// Each sub-command lives in its own Cmd*.cpp and registers itself through the
// functions declared in Commands.h.  Exit codes: 0 ok, 1 usage error, 2 input
// error (file missing / unparsable), 3 runtime error (decoder, GPU, IO).

#include "Commands.h"

#include "osv/core/Log.h"
#include "osv/core/Version.h"

#include <CLI/CLI.hpp>

#include <cstdio>
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
#include <shellapi.h>
#endif

namespace {

/// Convert a wide string to UTF-8 (Windows only; identity elsewhere).
std::string toUtf8(const wchar_t* wide) {
#if defined(_WIN32)
    if (!wide) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
#else
    (void)wide;
    return {};
#endif
}

int runApp(std::vector<std::string> args) {
    CLI::App app{std::string(osv::Version::productName()) + " " + osv::Version::string() +
                 " - independent open-source DJI Osmo 360 (.OSV) toolkit"};
    app.set_help_all_flag("--help-all", "Show help for every sub-command");
    app.require_subcommand(0, 1);

    bool showVersion = false;
    app.add_flag("--version", showVersion, "Print version and backend diagnostics");

    int verbosity = 0;
    app.add_flag("-v,--verbose", verbosity, "Increase log verbosity (repeatable)");
    bool quiet = false;
    app.add_flag("-q,--quiet", quiet, "Only print errors");

    osvtool::CommandContext ctx;
    osvtool::registerAllCommands(app, ctx);

    // CLI11 wants argv in reverse order for parse(std::vector<std::string>).
    std::vector<std::string> reversed(args.rbegin(), args.rend());
    try {
        app.parse(reversed);
    } catch (const CLI::CallForHelp& e) {
        return app.exit(e);
    } catch (const CLI::CallForAllHelp& e) {
        return app.exit(e);
    } catch (const CLI::ParseError& e) {
        app.exit(e);
        return osvtool::kExitUsage;
    }

    if (quiet) {
        osv::log::setLevel(osv::log::Level::Error);
    } else if (verbosity >= 2) {
        osv::log::setLevel(osv::log::Level::Trace);
    } else if (verbosity == 1) {
        osv::log::setLevel(osv::log::Level::Debug);
    } else {
        osv::log::setLevel(osv::log::Level::Info);
    }

    if (showVersion) {
        return osvtool::printVersion(ctx);
    }
    if (app.get_subcommands().empty()) {
        std::fputs(app.help().c_str(), stdout);
        return osvtool::kExitUsage;
    }
    return ctx.exitCode;
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.push_back(toUtf8(argv[i]));
    }
    return runApp(std::move(args));
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    return runApp(std::move(args));
}
#endif
