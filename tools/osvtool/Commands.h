// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Sub-command registry for osvtool.  Each Cmd*.cpp implements one
// `registerXxxCommand(CLI::App&, CommandContext&)` function that adds its
// CLI11 sub-command and installs a callback which stores the exit code in the
// shared CommandContext.  Keeping the registry in one header means adding a
// command is a two-line change here plus one new source file.
#pragma once

#include <CLI/CLI.hpp>

#include <string>

namespace osvtool {

/// Process exit codes (documented in README).
inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 1;
inline constexpr int kExitInput = 2;
inline constexpr int kExitRuntime = 3;

/// State shared between main() and the sub-commands.
struct CommandContext {
    int exitCode = kExitOk;  ///< Set by the command callback that ran.
};

/// Register every available sub-command.  Commands whose module is not
/// compiled in (see OSV_HAVE_* definitions) are simply not registered.
void registerAllCommands(CLI::App& app, CommandContext& ctx);

/// `osvtool --version`: library version plus FFmpeg / CUDA / OpenCL details.
int printVersion(CommandContext& ctx);

/// Print FFmpeg build, GPL flag and GPU device information for the modules
/// that are compiled in (implemented in Common.cpp).
void printBackendDiagnostics();

// Individual commands (each in its own translation unit).
void registerProbeCommand(CLI::App& app, CommandContext& ctx);
void registerExtractCommand(CLI::App& app, CommandContext& ctx);
void registerRenderCommand(CLI::App& app, CommandContext& ctx);
void registerLutCommand(CLI::App& app, CommandContext& ctx);
void registerSeamCommand(CLI::App& app, CommandContext& ctx);
void registerSelfcheckCommand(CLI::App& app, CommandContext& ctx);

}  // namespace osvtool
