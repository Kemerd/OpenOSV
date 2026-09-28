// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CommandBuilder.cpp - settings -> `osvtool render` arguments, quoting, and
// the probe parsers (see CommandBuilder.h for the rules).

#include "CommandBuilder.h"

#include "Queue.h"  // pathToUtf8 / pathFromUtf8

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>

namespace osvgui {

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
//  Sentinels the batch form substitutes after quoting.  Only characters
//  every quoting style leaves alone, so they survive quoting unchanged.
// ---------------------------------------------------------------------------
constexpr std::string_view kClipSentinel = "@@OSVCLIP@@";
constexpr std::string_view kOutSentinel = "@@OSVOUT@@";
constexpr std::string_view kNameSentinel = "@@OSVNAME@@";

/// Replace every occurrence of `from` in `text` by `to`.
void replaceAll(std::string& text, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

/// A number as osvtool reads it: no exponent, at most two decimals, no
/// trailing zeros ("90", "12.5", "-3.25"), and always a '.' decimal point.
[[nodiscard]] std::string formatNumber(double value) {
    if (!std::isfinite(value)) {
        return "0";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", value);
    std::string text(buf);
    std::replace(text.begin(), text.end(), ',', '.');  // a locale with a decimal comma
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    if (text == "-0") {
        text = "0";
    }
    return text;
}

/// The option name a token stands for, for override detection: "--x=1"
/// -> "--x", and a negated spelling "--no-x" -> "--x", so "--no-flare" in
/// the extra arguments overrides the Sun ghost toggle's "--flare".  Empty
/// for anything that is not a long option.
[[nodiscard]] std::string optionKey(std::string_view token) {
    if (token.size() < 3 || token[0] != '-' || token[1] != '-') {
        return {};
    }
    std::string name(token.substr(0, token.find('=')));
    if (name.rfind("--no-", 0) == 0 && name.size() > 5) {
        name = "--" + name.substr(5);
    }
    return name;
}

/// Characters no Windows file name may hold (the strictest of the file
/// systems a clip lands on), plus the path separators of every platform.
[[nodiscard]] bool isBadFileNameChar(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
        return true;
    }
    switch (c) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
        return true;
    default:
        return false;
    }
}

/// The output file name with {name} replaced by `nameValue` (already
/// file-name safe, or a sentinel), every other token expanded, bad
/// characters replaced and ".mp4" added when there is no extension.
[[nodiscard]] std::string expandName(const GuiSettings& s, std::string_view nameValue) {
    const bool reframe = s.mode == "reframe";
    std::string text = activePattern(s);

    // The tokens other than {name} are expanded first, then sanitised with
    // the literal text, so a token value can never smuggle in a separator.
    replaceAll(text, "{mode}", reframe ? "reframe" : "360");
    replaceAll(text, "{preset}", reframe ? s.view : std::string("equirect"));
    replaceAll(text, "{size}", reframe ? s.reframeSize : s.equirectSize);
    replaceAll(text, "{name}", kNameSentinel);
    for (char& c : text) {
        if (isBadFileNameChar(c)) {
            c = '_';
        }
    }
    // Windows drops trailing dots and spaces from a name silently; strip
    // them so the file ends up with exactly the name shown.
    while (!text.empty() && (text.back() == '.' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        text = std::string(kNameSentinel) + (reframe ? "_reframe" : "_360");
    }
    // No extension: a video, the common case.
    const std::size_t dot = text.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        text += ".mp4";
    }
    replaceAll(text, kNameSentinel, nameValue);
    return text;
}

/// A clip's name made file-name safe (it came from a file name, so this
/// only matters for a name with characters another system allows).
[[nodiscard]] std::string safeClipName(const fs::path& clip) {
    std::string name = pathToUtf8(clip.stem());
    for (char& c : name) {
        if (isBadFileNameChar(c)) {
            c = '_';
        }
    }
    return name.empty() ? std::string("clip") : name;
}

/// Lower-case ASCII copy.
[[nodiscard]] std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/// Escape text for the inside of a POSIX double-quoted string.
[[nodiscard]] std::string escapeDoubleQuoted(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '\\' || c == '"' || c == '$' || c == '`') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

ShellStyle nativeShellStyle() noexcept {
#if defined(_WIN32)
    return ShellStyle::Windows;
#else
    return ShellStyle::Posix;
#endif
}

// ===========================================================================
//  Output names
// ===========================================================================

std::string outputFileName(const GuiSettings& settings, const fs::path& clip) {
    return expandName(settings, safeClipName(clip));
}

fs::path outputPathFor(const GuiSettings& settings, const fs::path& clip) {
    const fs::path name = pathFromUtf8(outputFileName(settings, clip));
    if (!settings.outputFolder.empty()) {
        return pathFromUtf8(settings.outputFolder) / name;
    }
    return clip.parent_path() / name;
}

bool outputIsVideo(const GuiSettings& settings) {
    const std::string name = expandName(settings, "clip");
    const std::size_t dot = name.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string() : lowerAscii(name.substr(dot));
    return ext == ".mp4" || ext == ".mov";
}

// ===========================================================================
//  The command
// ===========================================================================

std::string effectiveCodec(const GuiSettings& settings, const ToolCaps& caps) {
    return settings.codec == "auto" ? caps.autoCodec : settings.codec;
}

std::vector<std::string> splitArguments(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    bool inQuotes = false;
    bool have = false;  // an argument is open (even an empty "" one)
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const char c = text[i];
        // ---- whitespace ends an argument outside quotes ---------------------------
        if (!inQuotes && (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v')) {
            if (have) {
                out.push_back(std::move(current));
                current.clear();
                have = false;
            }
            ++i;
            continue;
        }
        // ---- backslashes: special only in front of a quote -------------------------
        if (c == '\\') {
            std::size_t count = 0;
            while (i < n && text[i] == '\\') {
                ++count;
                ++i;
            }
            if (i < n && text[i] == '"') {
                current.append(count / 2, '\\');
                if (count % 2 == 1) {
                    current.push_back('"');  // an escaped, literal quote
                    ++i;
                }
                // Even count: the quote is left for the branch below.
            } else {
                current.append(count, '\\');
            }
            have = true;
            continue;
        }
        // ---- quotes toggle grouping; "" inside quotes is a literal quote ------------
        if (c == '"') {
            if (inQuotes && i + 1 < n && text[i + 1] == '"') {
                current.push_back('"');
                i += 2;
            } else {
                inQuotes = !inQuotes;
                ++i;
            }
            have = true;
            continue;
        }
        current.push_back(c);
        have = true;
        ++i;
    }
    if (have) {
        out.push_back(std::move(current));
    }
    return out;
}

std::vector<std::string> buildRenderArgs(const GuiSettings& settings, const ToolCaps& caps, const std::string& clipArg,
                                         const std::string& outArg) {
    const GuiSettings& s = settings;
    const std::vector<std::string> extras = splitArguments(s.extraArgs);

    // ---- what the extra arguments set themselves --------------------------------------
    std::set<std::string> given;
    for (const std::string& token : extras) {
        const std::string key = optionKey(token);
        if (!key.empty()) {
            given.insert(key);
        }
    }
    const auto overridden = [&given](std::string_view name) { return given.count(optionKey(name)) > 0; };

    std::vector<std::string> args;
    args.reserve(32 + extras.size());
    const auto option = [&](std::string_view name, const std::string& value) {
        if (!overridden(name)) {
            args.emplace_back(name);
            args.push_back(value);
        }
    };
    const auto flag = [&](std::string_view name) {
        if (!overridden(name)) {
            args.emplace_back(name);
        }
    };

    args.emplace_back("render");
    args.push_back(clipArg);

    // ---- geometry ---------------------------------------------------------------------------
    const bool reframe = s.mode == "reframe";
    option("--mode", reframe ? "reframe" : "equirect");
    if (reframe) {
        option("--size", s.reframeSize);
        if (s.view == "custom") {
            option("--fov", formatNumber(s.fov));
        } else {
            option("--preset", s.view);
        }
        // Pan / tilt / roll: only when set, so a plain preset reads plainly.
        if (formatNumber(s.yaw) != "0") {
            option("--yaw", formatNumber(s.yaw));
        }
        if (formatNumber(s.pitch) != "0") {
            option("--pitch", formatNumber(s.pitch));
        }
        if (formatNumber(s.roll) != "0") {
            option("--roll", formatNumber(s.roll));
        }
    } else if (s.equirectSize != "native") {
        option("--size", s.equirectSize);
    }

    // ---- colour, stabilisation, sun ghosts - or the saved Premiere defaults ----------------
    if (s.useUserDefaults) {
        flag("--use-user-defaults");
    } else {
        if (s.color != "auto") {
            option("--color", s.color);
        }
        if (s.color == "auto" || s.color == "pq" || s.color == "hlg") {
            option("--tone", s.tone);
        }
        if (s.color == "auto" || s.color == "709") {
            option("--look", s.look);
        }
        option("--stab", s.stab);
        flag(s.flare ? "--flare" : "--no-flare");
    }

    // ---- encoding (video outputs only) ------------------------------------------------------
    if (outputIsVideo(s)) {
        const std::string codec = effectiveCodec(s, caps);
        if (!codec.empty()) {
            option("--codec", codec);
        }
        option("--crf", std::to_string(s.crf));
        if (!s.audio) {
            flag("--no-audio");
        }
        if (!reframe && caps.sphericalMetadata && !s.sphericalMetadata) {
            flag("--no-spherical-metadata");
        }
        const std::string& ffmpeg = !s.ffmpegPath.empty() ? s.ffmpegPath : caps.ffmpegOverride;
        if (!ffmpeg.empty()) {
            option("--ffmpeg", ffmpeg);
        }
    }

    // ---- frames and output --------------------------------------------------------------------
    if (!overridden("--all") && !overridden("--frame") && !overridden("--range")) {
        args.emplace_back("--all");
    }
    option("--out", outArg);

    // ---- the user's own, last ---------------------------------------------------------------------
    args.insert(args.end(), extras.begin(), extras.end());
    return args;
}

std::vector<std::string> buildRenderArgs(const GuiSettings& settings, const ToolCaps& caps, const fs::path& clip) {
    return buildRenderArgs(settings, caps, pathToUtf8(clip), pathToUtf8(outputPathFor(settings, clip)));
}

// ===========================================================================
//  Quoting and display
// ===========================================================================

std::string quoteWindowsArg(std::string_view arg) {
    // Nothing to protect: as is.  cmd.exe's own operators are quoted too, so
    // the preview pastes into a prompt intact; to CreateProcess the quotes
    // cost nothing.
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"&|<>^()") == std::string_view::npos) {
        return std::string(arg);
    }
    std::string out;
    out.reserve(arg.size() + 8);
    out.push_back('"');
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            // Before the closing quote every backslash is doubled, or the
            // last one would escape it.
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            // Double the backslashes and escape the quote itself.
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
}

std::string quotePosixArg(std::string_view arg) {
    const auto safe = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '@' ||
               c == '%' || c == '+' || c == '=' || c == ':' || c == ',' || c == '.' || c == '/' || c == '-';
    };
    if (!arg.empty() && std::all_of(arg.begin(), arg.end(), safe)) {
        return std::string(arg);
    }
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";  // close, escaped quote, reopen
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string joinCommandLine(const std::vector<std::string>& argv, ShellStyle style) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) {
            out.push_back(' ');
        }
        out += style == ShellStyle::Windows ? quoteWindowsArg(argv[i]) : quotePosixArg(argv[i]);
    }
    return out;
}

std::string batchCommandLine(const GuiSettings& settings, const ToolCaps& caps, const std::string& exe,
                             const fs::path& folder, ShellStyle style) {
    // The command with sentinels where the loop variable goes, quoted as a
    // whole, then the sentinels swapped for the shell's expressions.
    std::vector<std::string> argv;
    argv.push_back(exe);
    const std::vector<std::string> args =
        buildRenderArgs(settings, caps, std::string(kClipSentinel), std::string(kOutSentinel));
    argv.insert(argv.end(), args.begin(), args.end());
    std::string command = joinCommandLine(argv, style);

    if (style == ShellStyle::Windows) {
        // cmd.exe: %~nf is the clip's name, %~dpf its drive and folder
        // (with the trailing backslash).
        const std::string name = expandName(settings, "%~nf");
        const std::string out = settings.outputFolder.empty()
                                    ? "%~dpf" + name
                                    : pathToUtf8(pathFromUtf8(settings.outputFolder) / pathFromUtf8(name));
        replaceAll(command, kClipSentinel, "\"%f\"");
        replaceAll(command, kOutSentinel, "\"" + out + "\"");
        const std::string pattern = pathToUtf8(folder / "*.OSV");
        return "for %f in (" + quoteWindowsArg(pattern) + ") do " + command;
    }

    // sh: "${f%.*}" is the clip without its extension; basename strips the
    // folder.  Both the literal parts and the folder are escaped for the
    // inside of the double quotes they sit in.
    std::string name = escapeDoubleQuoted(expandName(settings, std::string(kNameSentinel)));
    replaceAll(name, kNameSentinel, "$(basename \"${f%.*}\")");
    const std::string dir =
        settings.outputFolder.empty() ? std::string("$(dirname \"$f\")") : escapeDoubleQuoted(settings.outputFolder);
    replaceAll(command, kClipSentinel, "\"$f\"");
    replaceAll(command, kOutSentinel, "\"" + dir + "/" + name + "\"");
    std::string glob = quotePosixArg(pathToUtf8(folder));
    return "for f in " + glob + "/*.[Oo][Ss][Vv]; do " + command + "; done";
}

// ===========================================================================
//  Probing helpers
// ===========================================================================

bool helpMentionsSphericalMetadata(std::string_view helpText) noexcept {
    return helpText.find("spherical-metadata") != std::string_view::npos;
}

std::vector<std::string> parseVideoEncoders(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        start = end + 1;

        // " V....D hevc_nvenc   NVIDIA NVENC hevc encoder (codec hevc)":
        // a six-character flag field, then the name.
        const std::size_t f0 = line.find_first_not_of(" \t\r");
        if (f0 == std::string_view::npos) {
            continue;
        }
        const std::size_t f1 = line.find_first_of(" \t", f0);
        if (f1 == std::string_view::npos || f1 - f0 != 6 || line[f0] != 'V') {
            continue;
        }
        const std::size_t n0 = line.find_first_not_of(" \t", f1);
        if (n0 == std::string_view::npos) {
            continue;
        }
        const std::size_t n1 = line.find_first_of(" \t\r", n0);
        const std::string name(line.substr(n0, n1 == std::string_view::npos ? std::string_view::npos : n1 - n0));
        // The legend (" V..... = Video") has "=" where a name would be.
        if (!name.empty() && name != "=") {
            out.push_back(name);
        }
    }
    return out;
}

std::vector<std::string> pickEncoderOrder(const std::vector<std::string>& available) {
    static constexpr std::array<const char*, 5> kPreference = {"hevc_nvenc", "hevc_amf", "hevc_qsv",
                                                               "hevc_videotoolbox", "libx265"};
    std::vector<std::string> out;
    for (const char* name : kPreference) {
        if (std::find(available.begin(), available.end(), name) != available.end()) {
            out.emplace_back(name);
        }
    }
    return out;
}

std::vector<std::string> testEncodeArgs(const std::string& codec, int crf) {
    const std::string quality = std::to_string(std::clamp(crf, 0, 51));
    std::vector<std::string> a = {"-hide_banner", "-loglevel",  "error", "-nostdin",
                                  "-f",           "lavfi",      "-i",    "color=c=black:s=320x240:r=30",
                                  "-frames:v",    "5",          "-c:v",  codec,
                                  "-pix_fmt",     "yuv420p10le"};
    // The same quality knobs osvtool's pipe writer gives each family
    // (src/osv/io/FfmpegPipe.cpp), so a failure here is a failure there.
    if (codec.find("nvenc") != std::string::npos) {
        for (const char* v : {"-preset", "p5", "-rc", "vbr", "-cq"}) {
            a.emplace_back(v);
        }
        a.push_back(quality);
        for (const char* v : {"-b:v", "0", "-profile:v", "main10"}) {
            a.emplace_back(v);
        }
    } else if (codec == "libx265") {
        a.emplace_back("-crf");
        a.push_back(quality);
        for (const char* v : {"-preset", "medium", "-x265-params", "profile=main10:log-level=error"}) {
            a.emplace_back(v);
        }
    } else {
        a.emplace_back("-crf");
        a.push_back(quality);
    }
    for (const char* v : {"-f", "null", "-"}) {
        a.emplace_back(v);
    }
    return a;
}

}  // namespace osvgui
