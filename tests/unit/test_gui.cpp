// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for OpenOSV Studio's UI-free half (tools/osvgui): the exact
// `osvtool render` arguments every setting produces, quoting and splitting
// of command lines, output names, osvtool's progress and error lines, the
// folder scan and the queue, and the settings file.  No window, no GPU, no
// osvtool run: these hold on any machine.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#if defined(OSV_HAVE_GUI_CORE)

#include "TestSample.h"

#include "CommandBuilder.h"
#include "Progress.h"
#include "Queue.h"
#include "Settings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace osvgui;
namespace fs = std::filesystem;

namespace {

using Args = std::vector<std::string>;

/// The caps of an osvtool that knows the 360 metadata option, with an
/// ffmpeg whose NVENC works.
ToolCaps fullCaps() {
    ToolCaps caps;
    caps.sphericalMetadata = true;
    caps.autoCodec = "hevc_nvenc";
    return caps;
}

/// True when `args` contains `token`.
bool has(const Args& args, const std::string& token) {
    return std::find(args.begin(), args.end(), token) != args.end();
}

/// The value after `option` in `args`, or "" when absent.
std::string valueOf(const Args& args, const std::string& option) {
    const auto it = std::find(args.begin(), args.end(), option);
    return (it != args.end() && it + 1 != args.end()) ? *(it + 1) : std::string();
}

/// A fresh scratch folder for one test.
fs::path scratch(const std::string& name) {
    const fs::path dir = osvtest::tempDir() / ("gui_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

/// Write `bytes` bytes to `path` (0 makes an empty file).
void writeFile(const fs::path& path, std::size_t bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    for (std::size_t i = 0; i < bytes; ++i) {
        out.put(static_cast<char>('a' + (i % 26)));
    }
}

}  // namespace

// ===========================================================================
//  Settings -> osvtool arguments
// ===========================================================================

TEST_CASE("gui: default settings give the exact equirect command", "[gui]") {
    const GuiSettings s;
    const Args args = buildRenderArgs(s, fullCaps(), std::string("clip.OSV"), std::string("clip_360.mp4"));
    // Native size: no --size.  Auto colour: no --color, tone and look both.
    const Args expected = {"render",     "clip.OSV", "--mode", "equirect",       "--tone",  "aces-bright",
                           "--look",     "dji",      "--stab", "smooth-horizon", "--flare", "--codec",
                           "hevc_nvenc", "--crf",    "18",     "--all",          "--out",   "clip_360.mp4"};
    REQUIRE(args == expected);
}

TEST_CASE("gui: reframe with a preset, a size and a pan", "[gui]") {
    GuiSettings s;
    s.mode = "reframe";
    s.reframeSize = "1080x1920";
    s.view = "ultra-wide";
    s.yaw = 45.0;
    s.pitch = -12.5;
    const Args args = buildRenderArgs(s, fullCaps(), std::string("c.OSV"), std::string("c_reframe.mp4"));
    REQUIRE(valueOf(args, "--mode") == "reframe");
    REQUIRE(valueOf(args, "--size") == "1080x1920");
    REQUIRE(valueOf(args, "--preset") == "ultra-wide");
    REQUIRE(valueOf(args, "--yaw") == "45");
    REQUIRE(valueOf(args, "--pitch") == "-12.5");
    REQUIRE_FALSE(has(args, "--roll"));  // zero angles stay out
    REQUIRE_FALSE(has(args, "--fov"));
    // A flat video never gets the 360 tag option, ticked or not.
    s.sphericalMetadata = false;
    REQUIRE_FALSE(
        has(buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4")), "--no-spherical-metadata"));
}

TEST_CASE("gui: a custom view is an explicit field of view", "[gui]") {
    GuiSettings s;
    s.mode = "reframe";
    s.view = "custom";
    s.fov = 100.0;
    const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4"));
    REQUIRE(valueOf(args, "--fov") == "100");
    REQUIRE_FALSE(has(args, "--preset"));
}

TEST_CASE("gui: colour choices emit only the options that apply", "[gui]") {
    GuiSettings s;
    SECTION("PQ: --color and --tone, no --look") {
        s.color = "pq";
        s.tone = "bt2408-punchy";
        const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4"));
        REQUIRE(valueOf(args, "--color") == "pq");
        REQUIRE(valueOf(args, "--tone") == "bt2408-punchy");
        REQUIRE_FALSE(has(args, "--look"));
    }
    SECTION("HLG: --color and --tone") {
        s.color = "hlg";
        const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4"));
        REQUIRE(valueOf(args, "--color") == "hlg");
        REQUIRE(has(args, "--tone"));
        REQUIRE_FALSE(has(args, "--look"));
    }
    SECTION("SDR: --color 709 and --look, no --tone") {
        s.color = "709";
        s.look = "standard";
        const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4"));
        REQUIRE(valueOf(args, "--color") == "709");
        REQUIRE(valueOf(args, "--look") == "standard");
        REQUIRE_FALSE(has(args, "--tone"));
    }
}

TEST_CASE("gui: Premiere defaults leave colour, stabilisation and sun ghosts to them", "[gui]") {
    GuiSettings s;
    s.useUserDefaults = true;
    s.color = "709";
    s.flare = false;
    const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4"));
    REQUIRE(has(args, "--use-user-defaults"));
    for (const char* option : {"--color", "--tone", "--look", "--stab", "--flare", "--no-flare"}) {
        INFO(option);
        REQUIRE_FALSE(has(args, option));
    }
}

TEST_CASE("gui: encoding options", "[gui]") {
    GuiSettings s;
    SECTION("sun ghosts off") {
        s.flare = false;
        const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4"));
        REQUIRE(has(args, "--no-flare"));
        REQUIRE_FALSE(has(args, "--flare"));
    }
    SECTION("an explicit encoder wins over the probed one; crf follows the slider") {
        s.codec = "libx265";
        s.crf = 22;
        const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4"));
        REQUIRE(valueOf(args, "--codec") == "libx265");
        REQUIRE(valueOf(args, "--crf") == "22");
    }
    SECTION("auto with nothing probed leaves --codec to osvtool") {
        ToolCaps caps = fullCaps();
        caps.autoCodec.clear();
        REQUIRE_FALSE(has(buildRenderArgs(s, caps, std::string("c"), std::string("o.mp4")), "--codec"));
    }
    SECTION("audio off") {
        s.audio = false;
        REQUIRE(has(buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4")), "--no-audio"));
    }
    SECTION("the 360 tag switched off, only when osvtool knows the option") {
        s.sphericalMetadata = false;
        REQUIRE(has(buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4")), "--no-spherical-metadata"));
        ToolCaps older = fullCaps();
        older.sphericalMetadata = false;
        REQUIRE_FALSE(
            has(buildRenderArgs(s, older, std::string("c"), std::string("o.mp4")), "--no-spherical-metadata"));
        s.sphericalMetadata = true;  // on is osvtool's own default: nothing to say
        REQUIRE_FALSE(
            has(buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4")), "--spherical-metadata"));
    }
    SECTION("ffmpeg: the chosen one, else one found off PATH") {
        s.ffmpegPath = "D:/tools/ffmpeg.exe";
        REQUIRE(valueOf(buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4")), "--ffmpeg") ==
                "D:/tools/ffmpeg.exe");
        s.ffmpegPath.clear();
        ToolCaps caps = fullCaps();
        caps.ffmpegOverride = "/opt/homebrew/bin/ffmpeg";
        REQUIRE(valueOf(buildRenderArgs(s, caps, std::string("c"), std::string("o.mp4")), "--ffmpeg") ==
                "/opt/homebrew/bin/ffmpeg");
        REQUIRE_FALSE(has(buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4")), "--ffmpeg"));
    }
    SECTION("a fixed equirect size") {
        s.equirectSize = "3840x1920";
        REQUIRE(valueOf(buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o.mp4")), "--size") ==
                "3840x1920");
    }
    SECTION("stills: no video options") {
        s.equirectPattern = "{name}_%05d.png";
        const Args args = buildRenderArgs(s, fullCaps(), std::string("c"), std::string("o_%05d.png"));
        REQUIRE_FALSE(outputIsVideo(s));
        for (const char* option : {"--codec", "--crf", "--no-audio", "--ffmpeg"}) {
            INFO(option);
            REQUIRE_FALSE(has(args, option));
        }
    }
}

TEST_CASE("gui: extra arguments are appended and override the same options", "[gui]") {
    GuiSettings s;
    s.extraArgs = "--range 0-9 --codec libx265 --no-flare";
    const Args args = buildRenderArgs(s, fullCaps(), std::string("c.OSV"), std::string("o.mp4"));
    // Frames picked by the user: no --all.
    REQUIRE_FALSE(has(args, "--all"));
    // Ours are dropped where theirs name the same option (either spelling).
    REQUIRE(std::count(args.begin(), args.end(), "--codec") == 1);
    REQUIRE_FALSE(has(args, "--flare"));
    // Theirs come last, verbatim.
    const Args tail(args.end() - 5, args.end());
    REQUIRE(tail == Args{"--range", "0-9", "--codec", "libx265", "--no-flare"});
    REQUIRE(valueOf(args, "--out") == "o.mp4");
}

// ===========================================================================
//  Quoting and splitting
// ===========================================================================

TEST_CASE("gui: Windows quoting follows the C runtime's rules", "[gui]") {
    REQUIRE(quoteWindowsArg("render") == "render");
    REQUIRE(quoteWindowsArg("") == "\"\"");
    REQUIRE(quoteWindowsArg("C:\\My Clips\\CAM 0001.OSV") == "\"C:\\My Clips\\CAM 0001.OSV\"");
    // A trailing backslash inside quotes is doubled, or it would escape the quote.
    REQUIRE(quoteWindowsArg("D:\\out dir\\") == "\"D:\\out dir\\\\\"");
    // A quote inside is escaped, and the backslashes before it doubled.
    REQUIRE(quoteWindowsArg("say \"hi\"") == "\"say \\\"hi\\\"\"");
    REQUIRE(quoteWindowsArg("a\\\"b") == "\"a\\\\\\\"b\"");
    // cmd.exe's operators are quoted, so a pasted command stays one command.
    REQUIRE(quoteWindowsArg("Tom&Jerry.OSV") == "\"Tom&Jerry.OSV\"");
    // Backslashes not before a quote stay single.
    REQUIRE(quoteWindowsArg("C:\\clips\\a.OSV") == "C:\\clips\\a.OSV");
}

TEST_CASE("gui: POSIX quoting", "[gui]") {
    REQUIRE(quotePosixArg("render") == "render");
    REQUIRE(quotePosixArg("/Volumes/Card/DCIM/CAM_0001.OSV") == "/Volumes/Card/DCIM/CAM_0001.OSV");
    REQUIRE(quotePosixArg("/Volumes/My Card/a.OSV") == "'/Volumes/My Card/a.OSV'");
    REQUIRE(quotePosixArg("it's") == "'it'\\''s'");
    REQUIRE(quotePosixArg("") == "''");
}

TEST_CASE("gui: a command line with spaces in the paths", "[gui]") {
    GuiSettings s;
    Args argv = {"C:\\Program Files\\OpenOSV\\cli\\osvtool.exe"};
    const Args args = buildRenderArgs(s, fullCaps(), std::string("D:\\Footage Intake\\CAM 0001.OSV"),
                                      std::string("D:\\Renders\\CAM 0001_360.mp4"));
    argv.insert(argv.end(), args.begin(), args.end());
    const std::string line = joinCommandLine(argv, ShellStyle::Windows);
    REQUIRE(line.rfind("\"C:\\Program Files\\OpenOSV\\cli\\osvtool.exe\" render \"D:\\Footage Intake\\CAM 0001.OSV\"",
                       0) == 0);
    REQUIRE(line.find("--out \"D:\\Renders\\CAM 0001_360.mp4\"") != std::string::npos);
    // What CreateProcess hands the child is exactly the argument vector.
    REQUIRE(splitArguments(line) == argv);
}

TEST_CASE("gui: extra arguments split like a Windows command line", "[gui]") {
    REQUIRE(splitArguments("") == Args{});
    REQUIRE(splitArguments("   ") == Args{});
    REQUIRE(splitArguments("--range 0-9") == Args{"--range", "0-9"});
    REQUIRE(splitArguments("  --out   \"D:\\my out\\x.mp4\"  ") == Args{"--out", "D:\\my out\\x.mp4"});
    REQUIRE(splitArguments("a \"\" b") == Args{"a", "", "b"});
    REQUIRE(splitArguments("say\\\"hi") == Args{"say\"hi"});
    REQUIRE(splitArguments("C:\\path\\") == Args{"C:\\path\\"});
    REQUIRE(splitArguments("\"in \"\"quotes\"\"\"") == Args{"in \"quotes\""});
    // Every argument survives quoting and splitting unchanged.
    const Args tricky = {"plain", "with space", "trailing\\", "sp ace\\", "q\"uote", "\\\"", "", "tab\there", "&|<>"};
    REQUIRE(splitArguments(joinCommandLine(tricky, ShellStyle::Windows)) == tricky);
}

// ===========================================================================
//  Output names
// ===========================================================================

TEST_CASE("gui: output names from the pattern", "[gui]") {
    GuiSettings s;
    const fs::path clip = fs::path("DCIM") / "CAM_0001.OSV";
    REQUIRE(outputFileName(s, clip) == "CAM_0001_360.mp4");
    s.mode = "reframe";
    REQUIRE(outputFileName(s, clip) == "CAM_0001_reframe.mp4");
    s.reframePattern = "{name}-{mode}-{preset}-{size}.mov";
    REQUIRE(outputFileName(s, clip) == "CAM_0001-reframe-wide-1920x1080.mov");
    // No extension: a video.  Characters no file system takes: '_'.
    s.reframePattern = "{name} a:b*c?";
    REQUIRE(outputFileName(s, clip) == "CAM_0001 a_b_c_.mp4");
    // Emptied: the mode's default name.
    s.reframePattern.clear();
    REQUIRE(outputFileName(s, clip) == "CAM_0001_reframe.mp4");
    // A dotted clip name keeps its dots.
    s.mode = "equirect";
    REQUIRE(outputFileName(s, fs::path("CAM.v2.OSV")) == "CAM.v2_360.mp4");
}

TEST_CASE("gui: output folder, or next to each clip", "[gui]") {
    GuiSettings s;
    const fs::path clip = fs::path("card") / "DCIM" / "CAM_0001.OSV";
    REQUIRE(outputPathFor(s, clip) == fs::path("card") / "DCIM" / "CAM_0001_360.mp4");
    s.outputFolder = "renders";
    REQUIRE(outputPathFor(s, clip) == fs::path("renders") / "CAM_0001_360.mp4");
}

TEST_CASE("gui: the whole-folder form of the command", "[gui]") {
    GuiSettings s;
    const std::string win =
        batchCommandLine(s, fullCaps(), "C:\\OpenOSV\\osvtool.exe", fs::path("D:\\DCIM"), ShellStyle::Windows);
    REQUIRE(win.rfind("for %f in (", 0) == 0);
    REQUIRE(win.find("C:\\OpenOSV\\osvtool.exe render \"%f\"") != std::string::npos);
    REQUIRE(win.find("--out \"%~dpf%~nf_360.mp4\"") != std::string::npos);

    const std::string sh =
        batchCommandLine(s, fullCaps(), "/usr/local/bin/osvtool", fs::path("/Volumes/Card/DCIM"), ShellStyle::Posix);
    REQUIRE(sh.rfind("for f in /Volumes/Card/DCIM/*.[Oo][Ss][Vv]; do ", 0) == 0);
    REQUIRE(sh.find("render \"$f\"") != std::string::npos);
    REQUIRE(sh.find("--out \"$(dirname \"$f\")/$(basename \"${f%.*}\")_360.mp4\"") != std::string::npos);
    REQUIRE(sh.substr(sh.size() - 6) == "; done");
}

// ===========================================================================
//  osvtool's output
// ===========================================================================

TEST_CASE("gui: progress lines", "[gui]") {
    auto p = parseProgressLine("  12/65 frames  14.2 fps  (cuda)");
    REQUIRE(p);
    REQUIRE(p->done == 12);
    REQUIRE(p->total == 65);
    REQUIRE(p->fps == Catch::Approx(14.2));
    REQUIRE(p->device == "cuda");
    REQUIRE(etaSeconds(*p) == Catch::Approx(53.0 / 14.2));

    // A carriage return, no device, a device name with parentheses.
    REQUIRE(parseProgressLine("  65/65 frames  3.0 fps\r"));
    p = parseProgressLine("1/10 frames 0.5 fps (opencl (NVIDIA GeForce RTX 4090))");
    REQUIRE(p);
    REQUIRE(p->device == "opencl (NVIDIA GeForce RTX 4090)");

    // Not progress lines.
    REQUIRE_FALSE(parseProgressLine("error: frame 3: decode failed"));
    REQUIRE_FALSE(parseProgressLine("12/0 frames 1.0 fps"));
    REQUIRE_FALSE(parseProgressLine("12/65 frames"));
    REQUIRE_FALSE(parseProgressLine("12/65 frames 14. fps"));
    REQUIRE_FALSE(parseProgressLine("[12:00:00.000] [osv] [info] 12/65 frames 14.2 fps"));
    REQUIRE_FALSE(parseProgressLine("12/65 frames 14.2 fps (cuda) extra"));

    // No speed yet: no ETA.
    ProgressLine still{0, 10, 0.0, ""};
    REQUIRE(etaSeconds(still) < 0.0);
}

TEST_CASE("gui: error lines", "[gui]") {
    REQUIRE(errorMessage("error: cannot start ffmpeg (ffmpeg): not found") ==
            std::optional<std::string>("cannot start ffmpeg (ffmpeg): not found"));
    REQUIRE(errorMessage("[12:34:56.789] [osv] [error] engine: no device") ==
            std::optional<std::string>("engine: no device"));
    REQUIRE(errorMessage("\x1b[31m[12:34:56.789] [osv] [critical] out of memory\x1b[0m") ==
            std::optional<std::string>("out of memory"));
    REQUIRE_FALSE(errorMessage("[12:34:56.789] [osv] [info] engine: plug-in clip engine, 65 frames"));
    REQUIRE_FALSE(errorMessage("  12/65 frames  14.2 fps  (cuda)"));
}

TEST_CASE("gui: the output stream is cut into lines", "[gui]") {
    LineSplitter splitter;
    std::vector<std::string> lines;
    // Chunks that split a CRLF pair and a line; a lone CR ends a line too;
    // blank lines and colour codes are dropped.
    const std::string chunks[] = {"first li", "ne\r", "\nsecond\r\r\n", "\x1b[32mthird\x1b[0m\nfour", "th"};
    for (const std::string& chunk : chunks) {
        splitter.feed(chunk.data(), chunk.size(), lines);
    }
    splitter.finish(lines);
    REQUIRE(lines == std::vector<std::string>{"first line", "second", "third", "fourth"});

    // An endless line is cut, not grown without bound.
    LineSplitter small(8);
    std::vector<std::string> cut;
    const std::string longLine(20, 'x');
    small.feed(longLine.data(), longLine.size(), cut);
    small.finish(cut);
    REQUIRE(cut == std::vector<std::string>{"xxxxxxxx", "xxxxxxxx", "xxxx"});
}

TEST_CASE("gui: probe helpers", "[gui]") {
    REQUIRE(helpMentionsSphericalMetadata("  --spherical-metadata, --no-spherical-metadata{false}"));
    REQUIRE_FALSE(helpMentionsSphericalMetadata("  --no-audio  Do not copy the source audio"));

    const std::string encoders = "Encoders:\n"
                                 " V..... = Video\n"
                                 " A..... = Audio\n"
                                 " ------\n"
                                 " V....D libx264              libx264 H.264 (codec h264)\n"
                                 " V....D libx265              libx265 H.265 / HEVC (codec hevc)\n"
                                 " V....D hevc_amf             AMD AMF HEVC encoder (codec hevc)\n"
                                 " V....D hevc_nvenc           NVIDIA NVENC hevc encoder (codec hevc)\r\n"
                                 " A....D aac                  AAC (Advanced Audio Coding)\n";
    const std::vector<std::string> video = parseVideoEncoders(encoders);
    REQUIRE(video == std::vector<std::string>{"libx264", "libx265", "hevc_amf", "hevc_nvenc"});
    REQUIRE(pickEncoderOrder(video) == std::vector<std::string>{"hevc_nvenc", "hevc_amf", "libx265"});

    const std::vector<std::string> test = testEncodeArgs("hevc_nvenc", 18);
    REQUIRE(valueOf(test, "-c:v") == "hevc_nvenc");
    REQUIRE(valueOf(test, "-pix_fmt") == "yuv420p10le");
    REQUIRE(valueOf(test, "-cq") == "18");
    REQUIRE(Args(test.end() - 3, test.end()) == Args{"-f", "null", "-"});
    REQUIRE(valueOf(testEncodeArgs("hevc_qsv", 20), "-crf") == "20");
}

// ===========================================================================
//  Folder scan and queue
// ===========================================================================

TEST_CASE("gui: a folder scan finds the clips and skips the rest", "[gui]") {
    const fs::path root = scratch("scan");
    writeFile(root / "A_0001.OSV", 64);
    writeFile(root / "b_0002.osv", 64);                     // any case
    writeFile(root / "sub" / "deeper" / "C_0003.OsV", 64);  // any depth
    writeFile(root / "sub" / "deeper" / "C_0003.LRF", 64);  // proxy
    writeFile(root / "empty.OSV", 0);                       // interrupted copy
    writeFile(root / "._A_0001.OSV", 32);                   // macOS metadata twin
    writeFile(root / "notes.txt", 10);

    const ScanResult result = scanPaths({root});
    std::vector<std::string> names;
    for (const fs::path& clip : result.clips) {
        names.push_back(pathToUtf8(clip.filename()));
    }
    std::sort(names.begin(), names.end());
    REQUIRE(names == std::vector<std::string>{"A_0001.OSV", "C_0003.OsV", "b_0002.osv"});
    REQUIRE(result.skippedProxies == 1);
    REQUIRE(result.skippedEmpty == 1);
    REQUIRE(result.skippedOther == 0);  // stray files inside a folder are not counted
    REQUIRE(result.problems.empty());

    // Dropped files: a clip, a proxy, something else, something missing.
    const ScanResult files = scanPaths(
        {root / "A_0001.OSV", root / "sub" / "deeper" / "C_0003.LRF", root / "notes.txt", root / "missing.OSV"});
    REQUIRE(files.clips.size() == 1);
    REQUIRE(files.skippedProxies == 1);
    REQUIRE(files.skippedOther == 1);
    REQUIRE(files.problems.size() == 1);

    // The limit.
    const ScanResult capped = scanPaths({root}, 2);
    REQUIRE(capped.clips.size() == 2);
    REQUIRE(capped.truncated);
}

TEST_CASE("gui: the queue de-duplicates, moves and removes", "[gui]") {
    const fs::path root = scratch("queue");
    writeFile(root / "one.OSV", 10);
    writeFile(root / "two.OSV", 20);
    writeFile(root / "three.OSV", 30);

    ClipQueue queue;
    REQUIRE(queue.add({root / "one.OSV", root / "two.OSV", root / "one.OSV"}) == 2);
    REQUIRE(queue.add({root / "sub" / ".." / "two.OSV", root / "three.OSV"}) == 1);  // same file, other spelling
#if defined(_WIN32)
    REQUIRE(queue.add({root / "ONE.osv"}) == 0);  // Windows paths ignore case
#endif
    REQUIRE(queue.size() == 3);
    REQUIRE(queue.totalBytes() == 60);
    REQUIRE(queue.items()[0].name == "one.OSV");
    REQUIRE(queue.items()[0].sizeBytes == 10);

    // Drag the last to the front, then back.
    REQUIRE(queue.move(2, 0));
    REQUIRE(queue.items()[0].name == "three.OSV");
    REQUIRE(queue.items()[1].name == "one.OSV");
    REQUIRE(queue.move(0, 2));
    REQUIRE(queue.items()[2].name == "three.OSV");
    REQUIRE_FALSE(queue.move(0, 7));

    // Finished clips can be cleared; the first queued one is next.
    queue.items()[0].status = ClipStatus::Done;
    queue.items()[1].status = ClipStatus::Failed;
    REQUIRE(queue.firstQueued() == &queue.items()[2]);
    REQUIRE(queue.count(ClipStatus::Queued) == 1);
    REQUIRE(queue.removeFinished() == 2);
    REQUIRE(queue.size() == 1);
    const std::uint64_t id = queue.items()[0].id;
    REQUIRE(queue.remove(id));
    REQUIRE_FALSE(queue.remove(id));
    REQUIRE(queue.empty());
}

TEST_CASE("gui: sizes and durations read like the system shows them", "[gui]") {
    REQUIRE(formatBytes(0) == "0 B");
    REQUIRE(formatBytes(812000) == "812 KB");
    REQUIRE(formatBytes(4200000000ull) == "4.2 GB");
    REQUIRE(formatBytes(38000000000ull) == "38 GB");
    REQUIRE(formatDuration(12.4) == "12 s");
    REQUIRE(formatDuration(59.6) == "1:00");
    REQUIRE(formatDuration(245) == "4:05");
    REQUIRE(formatDuration(3729) == "1:02:09");
    REQUIRE(formatDuration(-1) == "--");
}

// ===========================================================================
//  Settings file
// ===========================================================================

TEST_CASE("gui: settings survive a JSON round trip", "[gui]") {
    GuiSettings s;
    s.mode = "reframe";
    s.equirectSize = "5760x2880";
    s.reframeSize = "3840x2160";
    s.view = "custom";
    s.fov = 110.5;
    s.yaw = -30.25;
    s.pitch = 12.0;
    s.roll = 3.0;
    s.color = "hlg";
    s.tone = "bt2408-natural";
    s.look = "standard";
    s.stab = "full";
    s.flare = false;
    s.useUserDefaults = true;
    s.codec = "hevc_qsv";
    s.crf = 23;
    s.audio = false;
    s.sphericalMetadata = false;
    s.outputFolder = "D:\\Renders \xC3\xA9t\xC3\xA9";  // non-ASCII survives
    s.equirectPattern = "{name}_equi.mov";
    s.reframePattern = "{name}_{preset}.mp4";
    s.skipExisting = true;
    s.ffmpegPath = "C:\\ffmpeg\\bin\\ffmpeg.exe";
    s.extraArgs = "--range 0-99 --out \"x y.mp4\"";
    s.theme = "light";
    s.showLog = true;
    s.batchPreview = true;
    s.advancedOpen = true;
    s.window = WindowState{100, -20, 1500, 900, true};
    s.encoderCache = EncoderCache{"C:\\ffmpeg\\bin\\ffmpeg.exe", 123456789ull, -42, "hevc_nvenc"};
    s.lastCommandLine = "osvtool render \"a b.OSV\" --all --out x.mp4";

    const std::string text = settingsToJsonText(s);
    REQUIRE_FALSE(text.empty());
    GuiSettings back;
    std::string error;
    REQUIRE(settingsFromJsonText(text, back, &error));
    REQUIRE(back == s);
}

TEST_CASE("gui: a hand-edited settings file is made safe", "[gui]") {
    GuiSettings s;
    const std::string text = R"({
        // comments are allowed
        "output": { "mode": "sideways", "fov": 500, "yaw": "left", "pitch": -120 },
        "colour": { "color": "pq", "tone": "vivid" },
        "encoding": { "crf": 99, "audio": "yes" },
        "destination": { "equirectPattern": "" },
        "interface": { "theme": "neon", "window": { "width": 10, "height": 100000 } },
        "unknown": 1
    })";
    REQUIRE(settingsFromJsonText(text, s));
    REQUIRE(s.mode == "equirect");  // unknown choice -> default
    REQUIRE(s.fov == 170.0);        // clamped
    REQUIRE(s.yaw == 0.0);          // wrong type -> kept default
    REQUIRE(s.pitch == -90.0);
    REQUIRE(s.color == "pq");  // valid values are taken
    REQUIRE(s.tone == "aces-bright");
    REQUIRE(s.crf == 51);
    REQUIRE(s.audio == true);
    REQUIRE(s.equirectPattern == "{name}_360.mp4");
    REQUIRE(s.theme == "dark");
    REQUIRE(s.window.width == 640);
    REQUIRE(s.window.height == 16384);

    GuiSettings untouched;
    REQUIRE_FALSE(settingsFromJsonText("this is not json", untouched));
    REQUIRE(untouched == GuiSettings{});
    REQUIRE_FALSE(settingsFromJsonText("[1, 2, 3]", untouched));
}

TEST_CASE("gui: the settings file is written and read back", "[gui]") {
    const fs::path dir = scratch("settings");
    const fs::path path = dir / "nested" / "osvgui.json";
    GuiSettings s;
    s.crf = 24;
    s.outputFolder = "E:\\out";
    std::string error;
    REQUIRE(saveSettings(path, s, &error));
    REQUIRE(fs::exists(path));
    // No temporary file is left behind.
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(path.parent_path())) {
        (void)entry;
        ++files;
    }
    REQUIRE(files == 1);

    GuiSettings back;
    REQUIRE(loadSettings(path, back, &error));
    REQUIRE(back == s);

    // A missing file is the first run: defaults, no error.
    GuiSettings fresh;
    REQUIRE(loadSettings(dir / "none.json", fresh, &error));
    REQUIRE(fresh == GuiSettings{});
}

#endif  // OSV_HAVE_GUI_CORE
