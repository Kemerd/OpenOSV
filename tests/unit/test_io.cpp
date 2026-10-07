// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for osv_io: image round trips and the ffmpeg pipe.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"

#include "osv/io/FfmpegPipe.h"
#include "osv/io/ImageWriter.h"
#include "osv/render/ImageRGBAf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace osv;

namespace {

/// A deterministic gradient with alpha so channel order and endianness
/// mistakes are caught.
render::ImageRGBAf makeGradient(std::uint32_t w, std::uint32_t h) {
    render::ImageRGBAf img = render::ImageRGBAf::create(w, h).value();
    for (std::uint32_t y = 0; y < h; ++y) {
        float* row = img.row(y);
        for (std::uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = static_cast<float>(x) / static_cast<float>(w - 1);
            row[x * 4 + 1] = static_cast<float>(y) / static_cast<float>(h - 1);
            row[x * 4 + 2] = 0.25f + 0.5f * static_cast<float>((x + y) % 7) / 6.0f;
            row[x * 4 + 3] = (x % 2) ? 1.0f : 0.5f;
        }
    }
    return img;
}

/// Max absolute difference over RGB (alpha optionally).
float maxDiff(const render::ImageRGBAf& a, const render::ImageRGBAf& b, bool alpha) {
    float m = 0.0f;
    for (std::size_t i = 0; i < a.data.size(); ++i) {
        if (!alpha && (i & 3u) == 3u) {
            continue;
        }
        m = std::max(m, std::fabs(a.data[i] - b.data[i]));
    }
    return m;
}

}  // namespace

TEST_CASE("PNG16 round trip is exact to 16-bit quantisation", "[io]") {
    const auto img = makeGradient(64, 48);
    const auto path = osvtest::tempDir() / "roundtrip16.png";
    io::ImageTag tag;
    tag.includeAlpha = true;
    REQUIRE(io::writeImage(path, img, io::ImageFormat::Png16, tag).ok());
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::exists(path.string() + ".json"));
    auto back = io::readImage(path);
    REQUIRE(back.ok());
    REQUIRE(back.value().w == 64);
    REQUIRE(back.value().h == 48);
    REQUIRE(maxDiff(img, back.value(), true) <= 1.0f / 65535.0f + 1e-6f);
}

TEST_CASE("PNG8 and TIFF16 round trips", "[io]") {
    const auto img = makeGradient(33, 17);
    io::ImageTag tag;
    tag.writeSidecar = false;
    const auto png8 = osvtest::tempDir() / "roundtrip8.png";
    REQUIRE(io::writeImage(png8, img, io::ImageFormat::Png8, tag).ok());
    auto back8 = io::readImage(png8);
    REQUIRE(back8.ok());
    REQUIRE(maxDiff(img, back8.value(), false) <= 1.0f / 255.0f + 1e-6f);
    REQUIRE_FALSE(std::filesystem::exists(png8.string() + ".json"));

    const auto tif = osvtest::tempDir() / "roundtrip16.tif";
    REQUIRE(io::writeImage(tif, img, io::ImageFormat::Tiff16, tag).ok());
    auto backT = io::readImage(tif);
    REQUIRE(backT.ok());
    REQUIRE(maxDiff(img, backT.value(), false) <= 1.0f / 65535.0f + 1e-6f);
    REQUIRE(io::formatFromExtension(tif) == io::ImageFormat::Tiff16);
    REQUIRE(io::formatFromExtension("x.EXR") == io::ImageFormat::Exr);
    REQUIRE(io::formatFromExtension("x.bmp") == io::ImageFormat::Png16);
}

TEST_CASE("EXR round trip keeps floats exactly", "[io]") {
    auto img = makeGradient(40, 30);
    // Values above 1.0 and negative must survive (scene-linear data).
    img.row(0)[0] = 12.5f;
    img.row(1)[4] = -0.25f;
    const auto path = osvtest::tempDir() / "roundtrip.exr";
    io::ImageTag tag;
    tag.transfer = io::ImageTransfer::Linear;
    tag.includeAlpha = true;
    REQUIRE(io::writeImage(path, img, io::ImageFormat::Exr, tag).ok());
    auto back = io::readExr(path);
    REQUIRE(back.ok());
    REQUIRE(back.value().w == 40);
    REQUIRE(maxDiff(img, back.value(), true) == 0.0f);
}

TEST_CASE("writeImage rejects invalid images", "[io]") {
    render::ImageRGBAf empty;
    REQUIRE(io::writeImage(osvtest::tempDir() / "nope.png", empty, io::ImageFormat::Png16, io::ImageTag{}).error().code ==
            ErrorCode::InvalidArgument);
    REQUIRE_FALSE(io::readImage(osvtest::tempDir() / "missing.png").ok());
}

// ---------------------------------------------------------------------------
//  The ffmpeg command line (no process is started)
// ---------------------------------------------------------------------------

namespace {

/// Options with the audio copy on: a 320 x 180 PQ clip at 30 fps whose
/// audio comes from "clip.LRF", encoded with libx265.
io::FfmpegPipeOptions audioPipeOptions() {
    io::FfmpegPipeOptions opt;
    opt.width = 320;
    opt.height = 180;
    opt.fps = 30.0;
    opt.codec = "libx265";
    opt.transfer = io::PipeTransfer::PQ;
    opt.audioSource = std::filesystem::path("clip.LRF");
    return opt;
}

/// Index of the first `value` in `args`, or args.size() when it is absent.
std::size_t indexOf(const std::vector<std::string>& args, const std::string& value) {
    const auto it = std::find(args.begin(), args.end(), value);
    return static_cast<std::size_t>(it - args.begin());
}

}  // namespace

TEST_CASE("ffmpeg pipe arguments: a render from the clip's start keeps its exact command line",
          "[io][ffmpegpipe]") {
    // The whole vector, pinned: an audio start of 0 must add nothing, so every
    // render from frame 0 (and every --all) runs exactly what it always ran.
    const io::FfmpegPipeOptions opt = audioPipeOptions();
    REQUIRE(opt.audioStartSeconds == 0.0);  // the default
    const std::filesystem::path out("out.mp4");
    const std::vector<std::string> expected = {
        "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb48le", "-s", "320x180", "-r", "30.000000", "-i", "-",
        "-i", std::filesystem::path("clip.LRF").string(), "-map", "0:v:0", "-map", "1:a:0?", "-c:a", "copy",
        "-shortest",
        "-c:v", "libx265", "-pix_fmt", "yuv420p10le",
        "-crf", "18", "-preset", "medium", "-x265-params",
        "profile=main10:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc",
        "-vf", "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc",
        "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
        "-color_range", "tv", "-movflags", "+write_colr+faststart", "-tag:v", "hvc1",
        out.string()};
    const std::vector<std::string> args = io::buildFfmpegArgs(opt, opt.codec, out);
    CHECK(args == expected);
    CHECK(indexOf(args, "-ss") == args.size());
}

TEST_CASE("ffmpeg pipe arguments: the audio input is seeked to the first rendered frame", "[io][ffmpegpipe]") {
    io::FfmpegPipeOptions opt = audioPipeOptions();
    const std::filesystem::path out("out.mp4");
    const std::string audio = std::filesystem::path("clip.LRF").string();

    SECTION("12.5 s: -ss 12.500000 immediately before the audio input, after the stdin one") {
        opt.audioStartSeconds = 12.5;
        const std::vector<std::string> args = io::buildFfmpegArgs(opt, opt.codec, out);
        const std::size_t ss = indexOf(args, "-ss");
        REQUIRE(ss + 3 < args.size());
        CHECK(args[ss + 1] == "12.500000");
        CHECK(args[ss + 2] == "-i");
        CHECK(args[ss + 3] == audio);
        // An input-side seek of the AUDIO input only: the raw frames on stdin
        // ("-i -") come first and are not moved.
        const std::size_t stdinInput = indexOf(args, "-");
        REQUIRE(stdinInput < args.size());
        CHECK(stdinInput < ss);
        CHECK(std::count(args.begin(), args.end(), "-ss") == 1);
        // Everything else is the start-0 command line with the two added.
        io::FfmpegPipeOptions zero = audioPipeOptions();
        std::vector<std::string> withoutSeek = args;
        withoutSeek.erase(withoutSeek.begin() + static_cast<std::ptrdiff_t>(ss),
                          withoutSeek.begin() + static_cast<std::ptrdiff_t>(ss) + 2);
        CHECK(withoutSeek == io::buildFfmpegArgs(zero, zero.codec, out));
    }

    SECTION("a timeline frame's moment is written to the microsecond, as an exact decimal") {
        // Frame 3000 of a 30000/1001 clip: 100.100000 s.
        opt.audioStartSeconds = 3000.0 * 1001.0 / 30000.0;
        auto args = io::buildFfmpegArgs(opt, opt.codec, out);
        std::size_t ss = indexOf(args, "-ss");
        REQUIRE(ss + 1 < args.size());
        CHECK(args[ss + 1] == "100.100000");
        // Frame 1 of a 25 fps clip: 40 ms.
        opt.audioStartSeconds = 1.0 / 25.0;
        args = io::buildFfmpegArgs(opt, opt.codec, out);
        ss = indexOf(args, "-ss");
        REQUIRE(ss + 1 < args.size());
        CHECK(args[ss + 1] == "0.040000");
        // An hour and a bit: no exponent, no truncation.
        opt.audioStartSeconds = 3725.25;
        args = io::buildFfmpegArgs(opt, opt.codec, out);
        ss = indexOf(args, "-ss");
        REQUIRE(ss + 1 < args.size());
        CHECK(args[ss + 1] == "3725.250000");
    }

    SECTION("a start that is not a time copies from the start (no -ss)") {
        for (const double bad : {std::nan(""), -1.0, -0.04, std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(), 1.0e300}) {
            opt.audioStartSeconds = bad;
            const std::vector<std::string> args = io::buildFfmpegArgs(opt, opt.codec, out);
            CHECK(indexOf(args, "-ss") == args.size());
            CHECK(indexOf(args, audio) < args.size());  // the audio is still copied
        }
        // Below half a microsecond rounds to no seek at all.
        opt.audioStartSeconds = 1.0e-7;
        const std::vector<std::string> args = io::buildFfmpegArgs(opt, opt.codec, out);
        CHECK(indexOf(args, "-ss") == args.size());
    }

    SECTION("without an audio source there is no audio input and no seek") {
        opt.audioSource.clear();
        opt.audioStartSeconds = 12.5;
        const std::vector<std::string> args = io::buildFfmpegArgs(opt, opt.codec, out);
        CHECK(indexOf(args, "-ss") == args.size());
        CHECK(std::count(args.begin(), args.end(), "-i") == 1);
        CHECK(indexOf(args, "-shortest") == args.size());
    }

    SECTION("the fallback encoder gets the same seek") {
        opt.audioStartSeconds = 12.5;
        const std::vector<std::string> args = io::buildFfmpegArgs(opt, "hevc_nvenc", out);
        const std::size_t ss = indexOf(args, "-ss");
        REQUIRE(ss + 3 < args.size());
        CHECK(args[ss + 1] == "12.500000");
        CHECK(args[ss + 3] == audio);
        CHECK(indexOf(args, "hevc_nvenc") < args.size());
    }
}

TEST_CASE("ffmpeg pipe produces a tagged HEVC file", "[io][ffmpeg-exe]") {
    const auto exe = io::FfmpegPipeWriter::resolveExecutable({});
    if (!std::filesystem::exists(exe) && exe.string() == "ffmpeg") {
        SKIP("ffmpeg.exe not found on PATH / OSV_FFMPEG_EXE");
    }
    io::FfmpegPipeOptions opt;
    opt.width = 320;
    opt.height = 180;
    opt.fps = 30.0;
    opt.transfer = io::PipeTransfer::PQ;
    opt.codec = "libx265";  // deterministic availability on most builds
    opt.fallbackCodec = "hevc_nvenc";
    const auto out = osvtest::tempDir() / "pipe_test.mp4";
    auto writer = io::FfmpegPipeWriter::open(opt, out);
    if (!writer.ok()) {
        SKIP("ffmpeg could not be started: " << writer.error().message);
    }
    const auto img = makeGradient(320, 180);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(writer.value().writeFrame(img).ok());
    }
    REQUIRE(writer.value().close().ok());
    REQUIRE(writer.value().framesWritten() == 5);
    REQUIRE(std::filesystem::file_size(out) > 1024);
    // The container must carry the BT.2020 / PQ nclx tag: look for the raw
    // 'colr' payload bytes "nclx" 9/16/9.
    std::ifstream in(out, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string needle = std::string("nclx") + '\x00' + '\x09' + '\x00' + '\x10' + '\x00' + '\x09';
    REQUIRE(bytes.find(needle) != std::string::npos);
}
