// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CommandBuilder.h - the settings, turned into an `osvtool render` command.
//
// Pure functions only (no process, no UI), so the unit tests pin down the
// exact argument vector every setting produces.  Every option emitted here
// is one `osvtool render --help` lists (tools/osvtool/CmdRender.cpp and
// Pipeline.cpp); nothing is invented.
//
// The rules, in one place:
//   * render <clip> first, --out last before the user's extra arguments;
//   * --all, unless the extra arguments pick frames (--frame / --range /
//     --all) themselves;
//   * an option the extra arguments name is NOT emitted, so the extra
//     arguments override the controls instead of colliding with them
//     (CLI11 rejects an option given twice);
//   * "Use my Premiere defaults" (--use-user-defaults) leaves colour,
//     stabilisation and sun ghost removal to the saved defaults: those
//     options are not emitted at all;
//   * colour "auto" leaves --color out (osvtool: D-Log M -> PQ, SDR stays
//     SDR); --tone goes with PQ / HLG / auto, --look with 709 / auto;
//   * equirect size "native" leaves --size out;
//   * --no-spherical-metadata only when the box is unticked, the output is
//     equirect, and this osvtool knows the option (ToolCaps).
#pragma once

#include "Settings.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace osvgui {

/// What the osvtool (and ffmpeg) at hand can do - found by probing them
/// once at start-up (Toolchain in the app).
struct ToolCaps {
    /// `osvtool render --help` mentions spherical-metadata: the 360 tag
    /// option exists (--no-spherical-metadata switches it off).
    bool sphericalMetadata = false;
    /// The encoder the probe found working with this ffmpeg ("" = unknown:
    /// --codec is then left to osvtool's default).
    std::string autoCodec;
    /// An ffmpeg found somewhere osvtool will not look (not on PATH): passed
    /// as --ffmpeg.  Empty when osvtool finds it by itself.
    std::string ffmpegOverride;
};

/// Which shell a displayed command line is written for.
enum class ShellStyle {
    Windows,  ///< cmd.exe / CreateProcess quoting ("...", \" escapes).
    Posix,    ///< sh quoting ('...').
};

/// The style of the platform this was compiled for.
[[nodiscard]] ShellStyle nativeShellStyle() noexcept;

// ===========================================================================
//  Output names
// ===========================================================================

/// The output file NAME for `clip`: the pattern in force with {name} (the
/// clip's name without extension), {mode} ("360" / "reframe"), {preset}
/// (the view, "equirect" for a 360 render) and {size} ("native" or WxH)
/// replaced, characters no file system accepts replaced by '_', and ".mp4"
/// appended when the result has no extension.
[[nodiscard]] std::string outputFileName(const GuiSettings& settings, const std::filesystem::path& clip);

/// The full output path: the output folder (or the clip's own folder) plus
/// outputFileName().
[[nodiscard]] std::filesystem::path outputPathFor(const GuiSettings& settings, const std::filesystem::path& clip);

/// True when the output is a video (.mp4 / .mov): ffmpeg is needed, and the
/// encoder / audio / 360 tag options apply.
[[nodiscard]] bool outputIsVideo(const GuiSettings& settings);

// ===========================================================================
//  The command
// ===========================================================================

/// The --codec value in force: the explicit choice, or for "auto" the
/// probed one ("" = leave --codec out).
[[nodiscard]] std::string effectiveCodec(const GuiSettings& settings, const ToolCaps& caps);

/// Split free text into arguments the way the C runtime splits a Windows
/// command line: whitespace separates, double quotes group, a backslash
/// escapes a quote (2n backslashes + quote = n backslashes and a quote
/// that toggles grouping; 2n+1 = n backslashes and a literal quote).
[[nodiscard]] std::vector<std::string> splitArguments(std::string_view text);

/// The arguments of one render (without the executable): "render", the
/// clip, the options, "--out", the output, then the extra arguments.
/// `clipArg` / `outArg` are passed through as given, so the batch preview
/// can put shell variables there.
[[nodiscard]] std::vector<std::string> buildRenderArgs(const GuiSettings& settings, const ToolCaps& caps,
                                                       const std::string& clipArg, const std::string& outArg);

/// The same for a real clip: its path and outputPathFor() it, as UTF-8.
[[nodiscard]] std::vector<std::string> buildRenderArgs(const GuiSettings& settings, const ToolCaps& caps,
                                                       const std::filesystem::path& clip);

// ===========================================================================
//  Quoting and display
// ===========================================================================

/// One argument quoted for CreateProcess / the C runtime's argv parser
/// (and so for cmd.exe as long as it holds no % or ^): unchanged when it
/// needs nothing, else in double quotes with quotes and the backslashes
/// before them escaped.
[[nodiscard]] std::string quoteWindowsArg(std::string_view arg);

/// One argument quoted for a POSIX shell: unchanged when it holds only
/// safe characters, else in single quotes ('\'' for a quote inside).
[[nodiscard]] std::string quotePosixArg(std::string_view arg);

/// `argv` (executable first) as one command line in `style`.
[[nodiscard]] std::string joinCommandLine(const std::vector<std::string>& argv, ShellStyle style);

/// The whole-folder form of the command: a cmd.exe `for` loop (Windows) or
/// an sh `for` loop (POSIX) over every .OSV in `folder`, each rendered with
/// these settings to the output the pattern gives it.
[[nodiscard]] std::string batchCommandLine(const GuiSettings& settings, const ToolCaps& caps, const std::string& exe,
                                           const std::filesystem::path& folder, ShellStyle style);

// ===========================================================================
//  Probing helpers
// ===========================================================================

/// True when `osvtool render --help` output mentions the 360 metadata
/// option (--spherical-metadata / --no-spherical-metadata).
[[nodiscard]] bool helpMentionsSphericalMetadata(std::string_view helpText) noexcept;

/// The video encoder names in `ffmpeg -hide_banner -encoders` output (the
/// lines whose flags start with 'V').
[[nodiscard]] std::vector<std::string> parseVideoEncoders(std::string_view encodersText);

/// The HEVC encoders worth trying, best first, among `available`:
/// hevc_nvenc, hevc_amf, hevc_qsv, hevc_videotoolbox, then libx265.
[[nodiscard]] std::vector<std::string> pickEncoderOrder(const std::vector<std::string>& available);

/// The ffmpeg arguments of a tiny test encode with `codec`, mirroring the
/// options osvtool's pipe writer uses for it (10-bit 4:2:0, the quality
/// option of its family) so "it works here" means "it works in a render".
[[nodiscard]] std::vector<std::string> testEncodeArgs(const std::string& codec, int crf);

}  // namespace osvgui
