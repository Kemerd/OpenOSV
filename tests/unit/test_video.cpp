// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests for osv_video: HevcStreamDecoder (software, container-sample feed,
// frame-accurate seeking, hardware paths), DualStreamReader (native pair and
// the side-by-side LRF proxy), the Annex-B / ADTS extractors and the IMU CSV
// export.  Everything that touches pixels needs the sample clip and SKIPs
// without it; the hardware tests additionally SKIP when no device opens.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestSample.h"

#include "osv/container/OsvFile.h"
#include "osv/core/Crc64.h"
#include "osv/core/Log.h"
#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/video/Decoder.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"
#include "osv/video/ImuCsv.h"
#include "osv/video/PlanarFrame.h"
#include "osv/video/StreamExtract.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace osv;
using namespace osv::video;
using Catch::Matchers::WithinAbs;

namespace {

// -----------------------------------------------------------------------------
//  Helpers
// -----------------------------------------------------------------------------

/// The layout of the native sample clip as FormatDetector would report it,
/// spelled out so these tests do not depend on the meta module's detector.
meta::FormatInfo nativeFormat() {
    meta::FormatInfo f;
    f.mode = meta::Mode::K6;
    f.streamW = 3000;
    f.streamH = 3000;
    f.fps = 59.94;
    f.bitDepth = 10;
    f.dualFisheye = true;
    f.videoTrackIds = {1u, 2u};
    f.sideBySideProxy = false;
    return f;
}

/// The LRF proxy layout (one 2048x1024 side-by-side track).
meta::FormatInfo proxyFormat() {
    meta::FormatInfo f;
    f.mode = meta::Mode::Lrf;
    f.streamW = 2048;
    f.streamH = 1024;
    f.fps = 29.97;
    f.bitDepth = 8;
    f.dualFisheye = true;
    f.videoTrackIds = {1u, 1u};
    f.sideBySideProxy = true;
    return f;
}

/// True when the .LRF proxy sits next to the sample clip.
bool haveLrf() {
    std::error_code ec;
    return std::filesystem::exists(osvtest::sampleLrf(), ec) && !ec;
}

/// CRC-64 (ECMA-182 polynomial, as used by xz) over a byte stream.
class Crc64 {
public:
    Crc64() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint64_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xC96C5795D7870F42ull ^ (c >> 1)) : (c >> 1);
            }
            m_table[i] = c;
        }
    }
    void update(const void* data, std::size_t size) {
        const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            m_crc = m_table[(m_crc ^ p[i]) & 0xFFu] ^ (m_crc >> 8);
        }
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return ~m_crc; }

private:
    std::array<std::uint64_t, 256> m_table{};
    std::uint64_t m_crc = ~0ull;
};

/// Content hash of a frame: every visible sample of every plane, brought to
/// the 10-bit scale, so planar and P010 layouts of the same picture hash
/// identically and stride padding never leaks in.
std::uint64_t frameCrc(const PlanarFrame16& f) {
    Crc64 crc;
    std::vector<std::uint16_t> row;
    row.resize(f.width);
    for (std::uint32_t y = 0; y < f.height; ++y) {
        for (std::uint32_t x = 0; x < f.width; ++x) {
            row[x] = f.luma(x, y);
        }
        crc.update(row.data(), row.size() * sizeof(std::uint16_t));
    }
    row.resize(f.chromaW);
    for (int c = 1; c <= 2; ++c) {
        for (std::uint32_t y = 0; y < f.chromaH; ++y) {
            for (std::uint32_t x = 0; x < f.chromaW; ++x) {
                row[x] = f.chroma(c, x, y);
            }
            crc.update(row.data(), row.size() * sizeof(std::uint16_t));
        }
    }
    return crc.value();
}

/// Luma statistics of one frame on the 10-bit scale.
struct LumaStats {
    std::uint64_t samples = 0;
    std::uint64_t inNarrowRange = 0;  ///< 64..940 inclusive.
    std::uint16_t min = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t max = 0;
};

LumaStats lumaStats(const PlanarFrame16& f) {
    LumaStats s;
    for (std::uint32_t y = 0; y < f.height; ++y) {
        const std::uint16_t* p = f.plane[0] + static_cast<std::size_t>(y) * f.strideElems[0];
        for (std::uint32_t x = 0; x < f.width; ++x) {
            const std::uint16_t v = static_cast<std::uint16_t>(p[x] >> f.bitShift);
            ++s.samples;
            if (v >= 64 && v <= 940) {
                ++s.inNarrowRange;
            }
            s.min = v < s.min ? v : s.min;
            s.max = v > s.max ? v : s.max;
        }
    }
    return s;
}

/// Luma PSNR between two frames of the same size (dB; infinity when equal).
double lumaPsnr(const PlanarFrame16& a, const PlanarFrame16& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double sse = 0.0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const double d = static_cast<double>(a.luma(x, y)) - static_cast<double>(b.luma(x, y));
            sse += d * d;
        }
    }
    const double mse = sse / (static_cast<double>(a.width) * static_cast<double>(a.height));
    if (mse <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }
    return 10.0 * std::log10(1023.0 * 1023.0 / mse);
}

/// Everything the sequential software pass over track 1 produces, computed
/// once per test binary because a 6K HEVC decode of 65 frames is the most
/// expensive thing in this file.
struct SequentialPass {
    std::vector<std::uint64_t> crcs;      ///< Per-frame content hashes.
    std::vector<std::int64_t> ptsUs;      ///< Per-frame presentation times.
    LumaStats luma;                       ///< Accumulated over every frame.
    std::uint32_t allZeroFrames = 0;
    std::string error;                    ///< Non-empty when the pass failed.
};

const SequentialPass& sequentialPass() {
    static const SequentialPass pass = [] {
        SequentialPass p;
        auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, DecoderOptions{});
        if (!opened.ok()) {
            p.error = opened.error().toString();
            return p;
        }
        HevcStreamDecoder& dec = opened.value();
        for (std::uint32_t i = 0; i < dec.frameCount(); ++i) {
            auto frame = dec.next();
            if (!frame.ok()) {
                p.error = "frame " + std::to_string(i) + ": " + frame.error().toString();
                return p;
            }
            const PlanarFrame16& f = frame.value();
            if (f.frameIndex != i) {
                p.error = "frame index mismatch at " + std::to_string(i) + " (got " + std::to_string(f.frameIndex) +
                          ", pts " + std::to_string(f.ptsUs) + " us)";
                return p;
            }
            const LumaStats s = lumaStats(f);
            p.luma.samples += s.samples;
            p.luma.inNarrowRange += s.inNarrowRange;
            p.luma.min = s.min < p.luma.min ? s.min : p.luma.min;
            p.luma.max = s.max > p.luma.max ? s.max : p.luma.max;
            if (s.max == 0) {
                ++p.allZeroFrames;
            }
            p.crcs.push_back(frameCrc(f));
            p.ptsUs.push_back(f.ptsUs);
        }
        return p;
    }();
    return pass;
}

/// Count 00 00 00 01 start codes in a byte buffer.
std::uint64_t countStartCodes(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t n = 0;
    for (std::size_t i = 0; i + 4 <= bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
            ++n;
            i += 3;
        }
    }
    return n;
}

std::vector<std::uint8_t> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Open a decoder on the sample clip with a hardware path, or SKIP.
HevcStreamDecoder openHwOrSkip(HwAccel hw) {
    DecoderOptions options;
    options.hw = hw;
    auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, options);
    if (!opened.ok()) {
        SKIP(std::string(hwAccelName(hw)) << " decoder unavailable: " << opened.error().toString());
    }
    if (opened.value().activeHw() != hw) {
        SKIP(std::string(hwAccelName(hw)) << " fell back to " << hwAccelName(opened.value().activeHw()));
    }
    return std::move(opened).value();
}

}  // namespace

// =============================================================================
//  Static diagnostics (no sample needed)
// =============================================================================
TEST_CASE("FFmpeg build is LGPL and reports its version", "[video]") {
    const std::string version = HevcStreamDecoder::ffmpegVersion();
    const std::string config = HevcStreamDecoder::ffmpegConfiguration();
    INFO("ffmpeg " << version);
    INFO("configuration: " << config);
    WARN("ffmpeg " << version << " | " << config);
    REQUIRE(version.find("avcodec") != std::string::npos);
    REQUIRE_FALSE(HevcStreamDecoder::ffmpegIsGpl());
    // Only 7-bit ASCII must ever reach the console.
    for (const char c : config) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);
    }
    const std::vector<std::string> hw = HevcStreamDecoder::availableHwAccels();
    REQUIRE_FALSE(hw.empty());
    REQUIRE(hw.front() == "none");
}

TEST_CASE("HwAccel names round-trip", "[video]") {
    REQUIRE(parseHwAccel("none") == HwAccel::None);
    REQUIRE(parseHwAccel("D3D11VA") == HwAccel::D3D11VA);
    REQUIRE(parseHwAccel("cuda") == HwAccel::Cuda);
    REQUIRE(parseHwAccel("Auto") == HwAccel::Auto);
    REQUIRE_FALSE(parseHwAccel("vulkan").has_value());
    REQUIRE(std::string(hwAccelName(HwAccel::D3D11VA)) == "d3d11va");
    for (const HwAccel hw : {HwAccel::None, HwAccel::D3D11VA, HwAccel::Cuda, HwAccel::Auto}) {
        REQUIRE(parseHwAccel(hwAccelName(hw)) == hw);
    }
}

TEST_CASE("Opening a missing file fails with Io", "[video]") {
    auto r = HevcStreamDecoder::open(osvtest::tempDir() / "does_not_exist.OSV", 1, DecoderOptions{});
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.error().code == ErrorCode::Io);
    // An unopened decoder is inert rather than dangerous.
    HevcStreamDecoder empty;
    REQUIRE_FALSE(empty.isOpen());
    REQUIRE(empty.frameCount() == 0);
    REQUIRE_FALSE(empty.decodeFrame(0).ok());
    REQUIRE_FALSE(empty.next().ok());
    REQUIRE_FALSE(empty.seek(0).ok());
    REQUIRE_FALSE(empty.lastDeviceFrame().has_value());
}

TEST_CASE("Track id 0 and a missing track are rejected", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    REQUIRE(HevcStreamDecoder::open(osvtest::sampleOsv(), 0, DecoderOptions{}).error().code == ErrorCode::InvalidArgument);
    // Track 3 is the AAC track, 99 does not exist: neither is a video stream
    // by id, and there is no 3rd / 99th video stream to fall back to.
    REQUIRE(HevcStreamDecoder::open(osvtest::sampleOsv(), 99, DecoderOptions{}).error().code == ErrorCode::NotFound);
}

// =============================================================================
//  Stream properties
// =============================================================================
TEST_CASE("Both lens tracks report 65 frames of 3000x3000 at 59.94 fps", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    for (const std::uint32_t track : {1u, 2u}) {
        auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), track, DecoderOptions{});
        REQUIRE(opened.ok());
        const HevcStreamDecoder& d = opened.value();
        INFO("track " << track);
        REQUIRE(d.isOpen());
        REQUIRE(d.trackId() == track);
        REQUIRE(d.frameCount() == 65);
        REQUIRE(d.width() == 3000);
        REQUIRE(d.height() == 3000);
        REQUIRE_THAT(d.fps(), WithinAbs(59.94, 0.01));
        REQUIRE(d.sourceBitDepth() == 10);
        REQUIRE(d.codecName() == "hevc");
        REQUIRE(d.activeHw() == HwAccel::None);
        REQUIRE_FALSE(d.usesContainerSamples());
        REQUIRE(d.timeBase().den > 0);
    }
}

// =============================================================================
//  Sequential software decode
// =============================================================================
TEST_CASE("Sequential decode of track 1 stays in narrow range", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    const SequentialPass& pass = sequentialPass();
    INFO("pass error: " << pass.error);
    REQUIRE(pass.error.empty());
    REQUIRE(pass.crcs.size() == 65);
    REQUIRE(pass.allZeroFrames == 0);
    REQUIRE(pass.luma.samples == 65ull * 3000ull * 3000ull);
    const double occupancy = static_cast<double>(pass.luma.inNarrowRange) / static_cast<double>(pass.luma.samples);
    INFO("luma min " << pass.luma.min << " max " << pass.luma.max << " narrow-range occupancy " << occupancy);
    REQUIRE(occupancy >= 0.99);
    REQUIRE(pass.luma.max <= 1023);
    // Presentation times advance by one frame period (16683.35 us) each.
    for (std::size_t i = 1; i < pass.ptsUs.size(); ++i) {
        const std::int64_t delta = pass.ptsUs[i] - pass.ptsUs[i - 1];
        REQUIRE(delta >= 16600);
        REQUIRE(delta <= 16800);
    }
    // 65 distinct pictures (a stuck decoder would repeat a hash).
    for (std::size_t i = 1; i < pass.crcs.size(); ++i) {
        REQUIRE(pass.crcs[i] != pass.crcs[i - 1]);
    }
}

TEST_CASE("Frame planes alias the decoder and expose 10-bit samples", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, DecoderOptions{});
    REQUIRE(opened.ok());
    auto frame = opened.value().decodeFrame(0);
    REQUIRE(frame.ok());
    const PlanarFrame16& f = frame.value();
    REQUIRE(f.valid());
    REQUIRE(f.bitDepth == 10);
    REQUIRE(f.bitShift == 0);
    REQUIRE_FALSE(f.chromaInterleaved);
    REQUIRE(f.narrowRange);
    REQUIRE(f.chromaW == 1500);
    REQUIRE(f.chromaH == 1500);
    REQUIRE(f.strideElems[0] >= 3000);
    REQUIRE(f.owner != nullptr);
    REQUIRE(f.frameIndex == 0);
    REQUIRE(opened.value().lastPts().has_value());
    REQUIRE_FALSE(opened.value().lastDeviceFrame().has_value());
    // The frame outlives the decoder (shared ownership of the AVFrame).
    const std::uint16_t centre = f.luma(1500, 1500);
    opened = HevcStreamDecoder::open(osvtest::tempDir() / "nope.OSV", 1, DecoderOptions{});
    REQUIRE(f.luma(1500, 1500) == centre);
}

// =============================================================================
//  Frame accuracy
// =============================================================================
TEST_CASE("decodeFrame(37) after a fresh open is bit-exact with the sequential pass", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    const SequentialPass& pass = sequentialPass();
    INFO("pass error: " << pass.error);
    REQUIRE(pass.error.empty());
    auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, DecoderOptions{});
    REQUIRE(opened.ok());
    auto frame = opened.value().decodeFrame(37);
    REQUIRE(frame.ok());
    REQUIRE(frame.value().frameIndex == 37);
    REQUIRE(frame.value().ptsUs == pass.ptsUs[37]);
    REQUIRE(frameCrc(frame.value()) == pass.crcs[37]);
    REQUIRE(opened.value().nextIndex() == 38);
    // Forward inside the same GOP without a seek, then across a GOP.
    auto f40 = opened.value().decodeFrame(40);
    REQUIRE(f40.ok());
    REQUIRE(frameCrc(f40.value()) == pass.crcs[40]);
    auto f62 = opened.value().decodeFrame(62);
    REQUIRE(f62.ok());
    REQUIRE(frameCrc(f62.value()) == pass.crcs[62]);
}

TEST_CASE("decodeFrame(0) after decodeFrame(64) re-seeks", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    const SequentialPass& pass = sequentialPass();
    INFO("pass error: " << pass.error);
    REQUIRE(pass.error.empty());
    auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, DecoderOptions{});
    REQUIRE(opened.ok());
    HevcStreamDecoder& d = opened.value();
    auto last = d.decodeFrame(64);
    REQUIRE(last.ok());
    REQUIRE(frameCrc(last.value()) == pass.crcs[64]);
    // Past the end: an error, not a crash, and the decoder stays usable.
    REQUIRE(d.decodeFrame(65).error().code == ErrorCode::InvalidArgument);
    REQUIRE(d.next().error().code == ErrorCode::NotFound);
    auto first = d.decodeFrame(0);
    REQUIRE(first.ok());
    REQUIRE(frameCrc(first.value()) == pass.crcs[0]);
    // seek() positions lazily; next() then delivers the target.
    REQUIRE(d.seek(61).ok());
    REQUIRE(d.nextIndex() == 61);
    auto f61 = d.next();
    REQUIRE(f61.ok());
    REQUIRE(f61.value().frameIndex == 61);
    REQUIRE(frameCrc(f61.value()) == pass.crcs[61]);
    REQUIRE_FALSE(d.seek(65).ok());
}

// =============================================================================
//  Container-sample feed
// =============================================================================
TEST_CASE("useContainerSamples decodes the same pictures as libavformat", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    const SequentialPass& pass = sequentialPass();
    INFO("pass error: " << pass.error);
    REQUIRE(pass.error.empty());
    DecoderOptions options;
    options.useContainerSamples = true;
    auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, options);
    REQUIRE(opened.ok());
    HevcStreamDecoder& d = opened.value();
    REQUIRE(d.usesContainerSamples());
    REQUIRE(d.frameCount() == 65);
    REQUIRE(d.width() == 3000);
    REQUIRE(d.height() == 3000);
    REQUIRE_THAT(d.fps(), WithinAbs(59.94, 0.01));
    REQUIRE(d.timeBase().den == 60000);
    for (std::uint32_t i = 0; i < 65; ++i) {
        auto frame = d.next();
        REQUIRE(frame.ok());
        INFO("frame " << i);
        REQUIRE(frame.value().frameIndex == i);
        REQUIRE(frameCrc(frame.value()) == pass.crcs[i]);
    }
    REQUIRE(d.next().error().code == ErrorCode::NotFound);
    // Random access through our own sync table.
    auto f30 = d.decodeFrame(30);
    REQUIRE(f30.ok());
    REQUIRE(frameCrc(f30.value()) == pass.crcs[30]);
    auto f63 = d.decodeFrame(63);
    REQUIRE(f63.ok());
    REQUIRE(frameCrc(f63.value()) == pass.crcs[63]);
    auto f2 = d.decodeFrame(2);
    REQUIRE(f2.ok());
    REQUIRE(frameCrc(f2.value()) == pass.crcs[2]);
}

// =============================================================================
//  DualStreamReader
// =============================================================================
TEST_CASE("DualStreamReader delivers pts-matched lens pairs", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    const SequentialPass& pass = sequentialPass();
    INFO("pass error: " << pass.error);
    REQUIRE(pass.error.empty());
    auto opened = DualStreamReader::open(osvtest::sampleOsv(), nativeFormat(), DecoderOptions{});
    REQUIRE(opened.ok());
    DualStreamReader& reader = opened.value();
    REQUIRE(reader.isOpen());
    REQUIRE_FALSE(reader.isSideBySide());
    REQUIRE(reader.frameCount() == 65);
    REQUIRE(reader.lensWidth() == 3000);
    REQUIRE(reader.lensHeight() == 3000);
    REQUIRE_THAT(reader.fps(), WithinAbs(59.94, 0.01));
    REQUIRE(reader.decoder(0) != nullptr);
    REQUIRE(reader.decoder(1) != nullptr);
    REQUIRE(reader.decoder(0)->trackId() == 1);
    REQUIRE(reader.decoder(1)->trackId() == 2);
    REQUIRE(reader.decoder(2) == nullptr);

    auto pair = reader.read(10);
    REQUIRE(pair.ok());
    const FramePair& p = pair.value();
    REQUIRE(p.valid());
    REQUIRE_FALSE(p.onDevice());
    REQUIRE(p.index == 10);
    REQUIRE(p.lens[0].ptsUs == p.lens[1].ptsUs);
    REQUIRE(p.ptsUs == p.lens[0].ptsUs);
    REQUIRE(p.ptsUs == pass.ptsUs[10]);
    REQUIRE(p.lens[0].frameIndex == 10);
    REQUIRE(p.lens[1].frameIndex == 10);
    // Lens 0 is track 1: identical to the sequential pass; lens 1 is a
    // different picture.
    REQUIRE(frameCrc(p.lens[0]) == pass.crcs[10]);
    REQUIRE(frameCrc(p.lens[1]) != pass.crcs[10]);
    // Backwards and out of range.
    auto p3 = reader.read(3);
    REQUIRE(p3.ok());
    REQUIRE(frameCrc(p3.value().lens[0]) == pass.crcs[3]);
    REQUIRE(reader.read(65).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("DualStreamReader rejects a format without track ids", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    meta::FormatInfo bad = nativeFormat();
    bad.videoTrackIds = {1u, 0u};
    REQUIRE(DualStreamReader::open(osvtest::sampleOsv(), bad, DecoderOptions{}).error().code ==
            ErrorCode::InvalidArgument);
    DualStreamReader empty;
    REQUIRE_FALSE(empty.isOpen());
    REQUIRE(empty.frameCount() == 0);
    REQUIRE_FALSE(empty.read(0).ok());
    REQUIRE(empty.decoder(0) == nullptr);
}

// =============================================================================
//  LRF proxy (8-bit H.264, side by side)
// =============================================================================
TEST_CASE("The LRF proxy opens as 2048x1024 8-bit widened to 10-bit", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (!haveLrf()) {
        SKIP("LRF proxy not available: " << osvtest::sampleLrf().string());
    }
    auto opened = HevcStreamDecoder::open(osvtest::sampleLrf(), 1, DecoderOptions{});
    REQUIRE(opened.ok());
    HevcStreamDecoder& d = opened.value();
    REQUIRE(d.width() == 2048);
    REQUIRE(d.height() == 1024);
    REQUIRE(d.frameCount() == 124);
    REQUIRE(d.sourceBitDepth() == 8);
    REQUIRE(d.codecName() == "h264");
    REQUIRE_THAT(d.fps(), WithinAbs(29.97, 0.01));
    auto frame = d.decodeFrame(5);
    REQUIRE(frame.ok());
    const PlanarFrame16& f = frame.value();
    REQUIRE(f.valid());
    REQUIRE(f.bitDepth == 10);
    REQUIRE(f.bitShift == 0);
    REQUIRE(f.frameIndex == 5);
    // Widened by two bits: every sample is a multiple of 4 and <= 1020.
    const LumaStats s = lumaStats(f);
    REQUIRE(s.max <= 1020);
    REQUIRE(s.max > 0);
    bool multiplesOfFour = true;
    for (std::uint32_t y = 0; y < f.height && multiplesOfFour; y += 7) {
        for (std::uint32_t x = 0; x < f.width; x += 5) {
            if ((f.luma(x, y) & 0x3u) != 0) {
                multiplesOfFour = false;
                break;
            }
        }
    }
    REQUIRE(multiplesOfFour);
    // The last frame is reachable and the first again after it.
    REQUIRE(d.decodeFrame(123).ok());
    REQUIRE(d.decodeFrame(0).ok());
}

TEST_CASE("The LRF proxy through the container-sample feed matches libavformat", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (!haveLrf()) {
        SKIP("LRF proxy not available: " << osvtest::sampleLrf().string());
    }
    auto viaFormat = HevcStreamDecoder::open(osvtest::sampleLrf(), 1, DecoderOptions{});
    REQUIRE(viaFormat.ok());
    DecoderOptions options;
    options.useContainerSamples = true;
    auto viaSamples = HevcStreamDecoder::open(osvtest::sampleLrf(), 1, options);
    REQUIRE(viaSamples.ok());
    REQUIRE(viaSamples.value().frameCount() == viaFormat.value().frameCount());
    for (const std::uint32_t i : {0u, 17u, 100u}) {
        auto a = viaFormat.value().decodeFrame(i);
        auto b = viaSamples.value().decodeFrame(i);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        INFO("frame " << i);
        REQUIRE(frameCrc(a.value()) == frameCrc(b.value()));
    }
}

TEST_CASE("DualStreamReader splits the LRF into two 1024x1024 halves", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    if (!haveLrf()) {
        SKIP("LRF proxy not available: " << osvtest::sampleLrf().string());
    }
    auto opened = DualStreamReader::open(osvtest::sampleLrf(), proxyFormat(), DecoderOptions{});
    REQUIRE(opened.ok());
    DualStreamReader& reader = opened.value();
    REQUIRE(reader.isSideBySide());
    REQUIRE(reader.frameCount() == 124);
    REQUIRE(reader.lensWidth() == 1024);
    REQUIRE(reader.lensHeight() == 1024);
    REQUIRE(reader.decoder(0) == reader.decoder(1));

    auto pair = reader.read(20);
    REQUIRE(pair.ok());
    const FramePair& p = pair.value();
    REQUIRE(p.valid());
    for (const PlanarFrame16& lens : p.lens) {
        REQUIRE(lens.width == 1024);
        REQUIRE(lens.height == 1024);
        REQUIRE(lens.chromaW == 512);
        REQUIRE(lens.chromaH == 512);
        REQUIRE(lens.frameIndex == 20);
    }
    REQUIRE(p.lens[0].ptsUs == p.lens[1].ptsUs);
    // Both halves share the backing memory and sit one half-row apart.
    REQUIRE(p.lens[0].owner == p.lens[1].owner);
    REQUIRE(p.lens[1].plane[0] == p.lens[0].plane[0] + 1024);
    REQUIRE(p.lens[1].plane[1] == p.lens[0].plane[1] + 512);
    // Cross-check against the whole frame from a plain decoder.
    auto whole = HevcStreamDecoder::open(osvtest::sampleLrf(), 1, DecoderOptions{});
    REQUIRE(whole.ok());
    auto full = whole.value().decodeFrame(20);
    REQUIRE(full.ok());
    const PlanarFrame16& w = full.value();
    for (std::uint32_t y = 0; y < 1024; y += 13) {
        for (std::uint32_t x = 0; x < 1024; x += 11) {
            REQUIRE(p.lens[0].luma(x, y) == w.luma(x, y));
            REQUIRE(p.lens[1].luma(x, y) == w.luma(x + 1024, y));
            REQUIRE(p.lens[0].chroma(1, x / 2, y / 2) == w.chroma(1, x / 2, y / 2));
            REQUIRE(p.lens[1].chroma(2, x / 2, y / 2) == w.chroma(2, x / 2 + 512, y / 2));
        }
    }
}

// =============================================================================
//  Hardware paths
// =============================================================================
TEST_CASE("D3D11VA decode matches software within 50 dB", "[video][sample][hwaccel]") {
    OSV_REQUIRE_SAMPLE();
    const SequentialPass& pass = sequentialPass();
    INFO("pass error: " << pass.error);
    REQUIRE(pass.error.empty());
    HevcStreamDecoder hw = openHwOrSkip(HwAccel::D3D11VA);
    auto sw = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, DecoderOptions{});
    REQUIRE(sw.ok());
    for (const std::uint32_t i : {0u, 37u}) {
        auto a = hw.decodeFrame(i);
        auto b = sw.value().decodeFrame(i);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        const PlanarFrame16& f = a.value();
        INFO("frame " << i << " format bitShift " << int(f.bitShift) << " interleaved " << f.chromaInterleaved);
        REQUIRE(f.valid());
        REQUIRE(f.bitDepth == 10);
        REQUIRE(f.frameIndex == i);
        REQUIRE(f.ptsUs == b.value().ptsUs);
        const double psnr = lumaPsnr(f, b.value());
        WARN("d3d11va frame " << i << " luma PSNR vs software " << psnr << " dB");
        REQUIRE(psnr >= 50.0);
    }
    REQUIRE_FALSE(hw.lastDeviceFrame().has_value());
}

// -----------------------------------------------------------------------------
//  Random access on the proxy, the way Premiere reaches it
// -----------------------------------------------------------------------------
// Premiere reads an .LRF through the importer's reader: D3D11VA, four frame
// threads, the container-sample feed and the process-wide shared device
// (importerReaderOptions).  It lands anywhere, steps forward a few frames and
// jumps again.  H.264 decoding is bit-exact by specification, so every frame
// that path returns must equal the frame a software decoder returns for the
// same index.  The clip is the sample's proxy, or OSV_SEEK_LRF when set: a
// long recording reaches GOP layouts and seek distances the 124-frame sample
// cannot.  OSV_SEEK_LANDINGS overrides the number of landings.
TEST_CASE("D3D11VA random access on the LRF matches software frame for frame", "[video][hwaccel][.lrfstress][lrfseek]") {
    // ---- the clip ---------------------------------------------------------------
    std::filesystem::path path = osvtest::sampleLrf();
    if (const char* env = std::getenv("OSV_SEEK_LRF"); env != nullptr && *env != '\0') {
        path = env;
    }
    if (!std::filesystem::exists(path)) {
        SKIP("LRF not available: " << path.string());
    }
    int landings = 60;
    if (const char* env = std::getenv("OSV_SEEK_LANDINGS"); env != nullptr && *env != '\0') {
        landings = std::max(1, std::atoi(env));
    }

    // ---- the importer's hardware reader, and a software reference ----------------
    DecoderOptions hwOptions;
    hwOptions.hw = HwAccel::D3D11VA;
    hwOptions.threads = 4;
    hwOptions.useContainerSamples = true;
    hwOptions.shareHwDevice = true;
    hwOptions.deferFirstFrame = true;
    auto hwOpened = HevcStreamDecoder::open(path, 1, hwOptions);
    if (!hwOpened.ok()) {
        SKIP("d3d11va decoder unavailable: " << hwOpened.error().toString());
    }
    DecoderOptions swOptions;
    swOptions.useContainerSamples = true;
    auto swOpened = HevcStreamDecoder::open(path, 1, swOptions);
    REQUIRE(swOpened.ok());
    HevcStreamDecoder& hw = hwOpened.value();
    HevcStreamDecoder& sw = swOpened.value();
    const std::uint32_t frames = hw.frameCount();
    REQUIRE(frames > 0);
    REQUIRE(frames == sw.frameCount());

    // ---- landings: a fixed pseudo-random walk, so a failure reproduces -----------
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto draw = [&state](std::uint32_t n) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>((state >> 33) % std::max<std::uint32_t>(1u, n));
    };
    int compared = 0;
    int mismatched = 0;
    double worstPsnr = std::numeric_limits<double>::infinity();
    std::uint32_t worstIndex = 0;
    for (int landing = 0; landing < landings; ++landing) {
        const std::uint32_t start = draw(frames);
        const std::uint32_t run = 1u + draw(8u);
        for (std::uint32_t step = 0; step < run && start + step < frames; ++step) {
            const std::uint32_t index = start + step;
            auto a = hw.decodeFrame(index);
            auto b = sw.decodeFrame(index);
            INFO("landing " << landing << " frame " << index);
            REQUIRE(a.ok());
            REQUIRE(b.ok());
            REQUIRE(a.value().frameIndex == index);
            ++compared;
            const double psnr = lumaPsnr(a.value(), b.value());
            if (psnr < worstPsnr) {
                worstPsnr = psnr;
                worstIndex = index;
            }
            if (psnr < 60.0) {
                ++mismatched;
                WARN("d3d11va frame " << index << " (landing " << landing << ", step " << step << ") differs from "
                                      << "software: luma PSNR " << psnr << " dB");
            }
        }
    }
    WARN("d3d11va random access: " << compared << " frames over " << landings << " landings, " << mismatched
                                   << " mismatched, worst PSNR " << worstPsnr << " dB at frame " << worstIndex);
    // ---- was it hardware at all? -----------------------------------------------------
    // get_format drops a decoder to software, silently but for one log line,
    // when the stream offers no D3D11 surface; every comparison above would
    // then be software against software and prove nothing about D3D11VA.
    // The verdict counts only when the decoder is still on D3D11VA.
    if (hw.activeHw() != HwAccel::D3D11VA) {
        SKIP("the d3d11va decoder fell back to " << hwAccelName(hw.activeHw())
                                                 << " (get_format was offered no D3D11 surface for this stream), so "
                                                 << compared << " comparisons were software against software");
    }
    WARN("d3d11va random access: decoded on " << hwAccelName(hw.activeHw()));
    REQUIRE(hw.activeHw() == HwAccel::D3D11VA);
    REQUIRE(mismatched == 0);
}

// -----------------------------------------------------------------------------
//  The LRF under Premiere's threading: helpers
// -----------------------------------------------------------------------------
// The single-decoder walk above is clean, yet Premiere showed the proxy with
// its lower half in macroblock garbage.  What Premiere adds is threads: its
// render threads take turns on the importer's reader, the background stages
// (steady, scene light, lens alignment) open short-lived D3D11VA readers of
// their own on the SAME shared device, and a quiet parks the reader in the
// pool for another thread to take back.  The tests below rebuild each of
// those on the LRF named by OSV_SEEK_LRF and compare every picture with a
// software decode of the same index.  Nothing here may use a Catch2 macro
// off the main thread, so the workers only collect and the main thread judges.
namespace {

/// The proxy the threaded tests read: OSV_SEEK_LRF, else the sample's.
std::filesystem::path stressLrfPath() {
    std::filesystem::path path = osvtest::sampleLrf();
    if (const char* env = std::getenv("OSV_SEEK_LRF"); env != nullptr && *env != '\0') {
        path = env;
    }
    return path;
}

/// A positive integer from the environment, or `fallback`.
int envCount(const char* name, int fallback) {
    if (const char* env = std::getenv(name); env != nullptr && *env != '\0') {
        return std::max(1, std::atoi(env));
    }
    return fallback;
}

/// The importer's reader options on D3D11VA (importerReaderOptions).
DecoderOptions importerD3d11Options() {
    DecoderOptions o;
    o.hw = HwAccel::D3D11VA;
    o.threads = 4;
    o.keepOnDevice = false;
    o.useContainerSamples = true;
    o.shareHwDevice = true;
    o.deferFirstFrame = true;
    return o;
}

/// The background stages' reader options (SteadyStage / SceneLightStage
/// JobReader): D3D11VA with two frame threads on the same shared device.
DecoderOptions stageD3d11Options() {
    DecoderOptions o = importerD3d11Options();
    o.threads = 2;
    return o;
}

/// The software reference: container samples, two frame threads (bounded so
/// several references can run at once without taking the whole machine).
DecoderOptions softwareReferenceOptions() {
    DecoderOptions o;
    o.hw = HwAccel::None;
    o.threads = 2;
    o.useContainerSamples = true;
    return o;
}

/// How one hardware picture differs from the software one, row by row.
struct LumaDiff {
    double psnr = std::numeric_limits<double>::infinity();  ///< Whole-picture luma PSNR (dB).
    int firstBadRow = -1;   ///< First luma row below 60 dB on its own (-1 = none).
    int badRows = 0;        ///< Rows below 60 dB.
    int badRowsBottom = 0;  ///< Of those, rows in the lower half of the picture.
    bool sizeMismatch = false;
};

/// Compare two decoded pictures without any Catch2 macro (worker-thread safe).
LumaDiff compareLuma(const PlanarFrame16& a, const PlanarFrame16& b) noexcept {
    LumaDiff d;
    if (a.width != b.width || a.height != b.height || a.width == 0 || a.height == 0) {
        d.sizeMismatch = true;
        d.psnr = 0.0;
        return d;
    }
    // A row is "bad" when its own mean squared error is above the 60 dB
    // threshold the whole-picture check uses.
    const double rowLimit = 1023.0 * 1023.0 / std::pow(10.0, 6.0);
    double sse = 0.0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        double rowSse = 0.0;
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const double diff = static_cast<double>(a.luma(x, y)) - static_cast<double>(b.luma(x, y));
            rowSse += diff * diff;
        }
        sse += rowSse;
        if (rowSse / static_cast<double>(a.width) > rowLimit) {
            if (d.firstBadRow < 0) {
                d.firstBadRow = static_cast<int>(y);
            }
            ++d.badRows;
            if (y >= a.height / 2) {
                ++d.badRowsBottom;
            }
        }
    }
    const double mse = sse / (static_cast<double>(a.width) * static_cast<double>(a.height));
    d.psnr = mse <= 0.0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(1023.0 * 1023.0 / mse);
    return d;
}

/// One picture that did not match, kept for the main thread to report.
struct LrfMismatch {
    int worker = 0;
    int request = 0;
    std::uint32_t index = 0;
    LumaDiff diff;
    std::string error;  ///< Non-empty when a decode failed instead.
};

/// What one worker saw.
struct LrfWorkerReport {
    int compared = 0;
    double worstPsnr = std::numeric_limits<double>::infinity();
    std::uint32_t worstIndex = 0;
    std::vector<LrfMismatch> mismatches;
    std::string openError;  ///< Non-empty when the worker could not start.
    HwAccel activeHw = HwAccel::None;  ///< What the hardware decoder really ran on (getFormat may drop to software).
    bool hardwareWorker = false;       ///< True when the worker's decoder was meant to be D3D11VA.
};

/// The fixed pseudo-random walk of the single-decoder test, seedable.
class LandingWalk {
public:
    explicit LandingWalk(std::uint64_t seed) noexcept : m_state(seed) {}
    std::uint32_t draw(std::uint32_t n) noexcept {
        m_state = m_state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>((m_state >> 33) % std::max<std::uint32_t>(1u, n));
    }

private:
    std::uint64_t m_state;
};

/// Decode `index` on both decoders and record the comparison in `report`.
void compareOne(HevcStreamDecoder& hw, HevcStreamDecoder& sw, std::uint32_t index, int worker, int request,
                LrfWorkerReport& report) {
    auto a = hw.decodeFrame(index);
    auto b = sw.decodeFrame(index);
    if (!a.ok() || !b.ok()) {
        LrfMismatch m;
        m.worker = worker;
        m.request = request;
        m.index = index;
        m.error = !a.ok() ? "hw: " + a.error().toString() : "sw: " + b.error().toString();
        report.mismatches.push_back(std::move(m));
        return;
    }
    ++report.compared;
    const LumaDiff diff = compareLuma(a.value(), b.value());
    if (diff.psnr < report.worstPsnr) {
        report.worstPsnr = diff.psnr;
        report.worstIndex = index;
    }
    if (diff.psnr < 60.0 || a.value().frameIndex != index) {
        LrfMismatch m;
        m.worker = worker;
        m.request = request;
        m.index = index;
        m.diff = diff;
        report.mismatches.push_back(std::move(m));
    }
}

/// Report every worker on the main thread and fail on any mismatch.
void judgeWorkers(const std::vector<LrfWorkerReport>& reports, const char* what) {
    int compared = 0;
    int mismatched = 0;
    double worst = std::numeric_limits<double>::infinity();
    for (std::size_t w = 0; w < reports.size(); ++w) {
        const LrfWorkerReport& r = reports[w];
        if (!r.openError.empty()) {
            WARN(what << ": worker " << w << " could not start: " << r.openError);
        }
        // A decoder whose get_format dropped to software would compare
        // software with software and prove nothing: it must still be D3D11VA.
        if (r.hardwareWorker) {
            INFO(what << ": worker " << w << " decoded on " << hwAccelName(r.activeHw));
            CHECK(r.activeHw == HwAccel::D3D11VA);
        }
        compared += r.compared;
        worst = std::min(worst, r.worstPsnr);
        for (const LrfMismatch& m : r.mismatches) {
            ++mismatched;
            if (!m.error.empty()) {
                WARN(what << ": worker " << m.worker << " request " << m.request << " frame " << m.index
                          << " failed: " << m.error);
            } else {
                WARN(what << ": worker " << m.worker << " request " << m.request << " frame " << m.index
                          << " differs from software: luma PSNR " << m.diff.psnr << " dB, first bad row "
                          << m.diff.firstBadRow << ", " << m.diff.badRows << " bad rows (" << m.diff.badRowsBottom
                          << " in the lower half)");
            }
        }
    }
    WARN(what << ": " << compared << " frames compared on " << reports.size() << " workers, " << mismatched
              << " mismatched, worst PSNR " << worst << " dB");
    REQUIRE(compared > 0);
    REQUIRE(mismatched == 0);
}

}  // namespace

// -----------------------------------------------------------------------------
//  Several readers of one LRF decoding at the same moment
// -----------------------------------------------------------------------------
// Premiere can hold several importer instances of one clip (thumbnail, Source
// Monitor, Program Monitor), each with its own reader, and every reader's
// D3D11VA decoder sits on the one shared device.  OSV_SEEK_THREADS readers
// (default 3) each walk OSV_SEEK_LANDINGS landings (default 60) at the same
// time, every one opened on its own thread after a start barrier.
TEST_CASE("D3D11VA readers of one LRF decoding on three threads at once match software",
          "[video][hwaccel][.lrfstress][lrfseek][lrfconcurrent]") {
    const std::filesystem::path path = stressLrfPath();
    if (!std::filesystem::exists(path)) {
        SKIP("LRF not available: " << path.string());
    }
    const int workers = envCount("OSV_SEEK_THREADS", 3);
    const int landings = envCount("OSV_SEEK_LANDINGS", 60);

    std::vector<LrfWorkerReport> reports(static_cast<std::size_t>(workers));
    std::atomic<int> opened{0};
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(workers));
    for (int w = 0; w < workers; ++w) {
        threads.emplace_back([&, w]() noexcept {
            LrfWorkerReport& report = reports[static_cast<std::size_t>(w)];
            try {
                // ---- each worker opens its own pair ----------------------------------
                auto hw = HevcStreamDecoder::open(path, 1, importerD3d11Options());
                auto sw = HevcStreamDecoder::open(path, 1, softwareReferenceOptions());
                opened.fetch_add(1);
                if (!hw.ok() || !sw.ok()) {
                    report.openError = !hw.ok() ? hw.error().toString() : sw.error().toString();
                    return;
                }
                // ---- start together so the decodes overlap ---------------------------
                while (opened.load() < workers) {
                    std::this_thread::yield();
                }
                const std::uint32_t frames = hw.value().frameCount();
                LandingWalk walk(0x9E3779B97F4A7C15ull + 0x1000193ull * static_cast<std::uint64_t>(w + 1));
                int request = 0;
                for (int landing = 0; landing < landings; ++landing) {
                    const std::uint32_t start = walk.draw(frames);
                    const std::uint32_t run = 1u + walk.draw(8u);
                    for (std::uint32_t step = 0; step < run && start + step < frames; ++step) {
                        compareOne(hw.value(), sw.value(), start + step, w, request++, report);
                    }
                }
                report.hardwareWorker = true;
                report.activeHw = hw.value().activeHw();
            } catch (const std::exception& e) {
                report.openError = std::string("exception: ") + e.what();
            } catch (...) {
                report.openError = "unknown exception";
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    judgeWorkers(reports, "concurrent readers");
}

// -----------------------------------------------------------------------------
//  One reader taken in turns by render threads, parked, and beside stages
// -----------------------------------------------------------------------------
// The importer's own reader is used under the instance mutex by whichever
// render thread Premiere calls on, parked in the pool on a quiet and taken
// back by the next instance; meanwhile the background stages open, use and
// destroy short-lived D3D11VA readers of the same clip on the same shared
// device.  Four "render threads" take turns on one importer-equivalent
// decoder (every OSV_SEEK_PARK_EVERY requests, default 7, the decoder is
// moved out to a parking slot and back); two "stage" threads keep opening a
// two-thread decoder, reading three scattered frames and destroying it.
TEST_CASE("A D3D11VA LRF reader taken in turns, parked and run beside stage readers matches software",
          "[video][hwaccel][.lrfstress][lrfseek][lrfhandoff]") {
    const std::filesystem::path path = stressLrfPath();
    if (!std::filesystem::exists(path)) {
        SKIP("LRF not available: " << path.string());
    }
    const int requests = envCount("OSV_SEEK_LANDINGS", 60) * 4;
    const int parkEvery = envCount("OSV_SEEK_PARK_EVERY", 7);
    const int stageRounds = envCount("OSV_SEEK_STAGE_ROUNDS", 12);

    // ---- the instance's reader, its reference, and the instance mutex ----------
    auto hwOpened = HevcStreamDecoder::open(path, 1, importerD3d11Options());
    if (!hwOpened.ok()) {
        SKIP("d3d11va decoder unavailable: " << hwOpened.error().toString());
    }
    auto swOpened = HevcStreamDecoder::open(path, 1, softwareReferenceOptions());
    REQUIRE(swOpened.ok());
    auto reader = std::make_unique<HevcStreamDecoder>(std::move(hwOpened).value());
    HevcStreamDecoder reference = std::move(swOpened).value();
    const std::uint32_t frames = reader->frameCount();
    REQUIRE(frames > 0);
    std::unique_ptr<HevcStreamDecoder> parked;  // the pool's slot
    std::mutex instanceMutex;
    int nextRequest = 0;
    std::uint32_t runLeft = 0;
    std::uint32_t cursor = 0;
    LandingWalk walk(0xC2B2AE3D27D4EB4Full);

    // ---- render threads -------------------------------------------------------
    constexpr int kRenderThreads = 4;
    constexpr int kStageThreads = 2;
    std::vector<LrfWorkerReport> reports(kRenderThreads + kStageThreads);
    std::vector<std::thread> threads;
    for (int w = 0; w < kRenderThreads; ++w) {
        threads.emplace_back([&, w]() noexcept {
            LrfWorkerReport& report = reports[static_cast<std::size_t>(w)];
            try {
                for (;;) {
                    std::lock_guard<std::mutex> lock(instanceMutex);
                    if (nextRequest >= requests) {
                        return;
                    }
                    const int request = nextRequest++;
                    // A quiet: the reader goes to the pool and the next request
                    // takes it back, on whatever thread that request runs.
                    if (request > 0 && request % parkEvery == 0) {
                        parked = std::move(reader);
                        reader = std::move(parked);
                    }
                    // The next frame: continue the current run or land anew.
                    if (runLeft == 0) {
                        cursor = walk.draw(frames);
                        runLeft = 1u + walk.draw(8u);
                    } else {
                        cursor = std::min(cursor + 1u, frames - 1u);
                    }
                    --runLeft;
                    compareOne(*reader, reference, cursor, w, request, report);
                    report.hardwareWorker = true;
                    report.activeHw = reader->activeHw();
                }
            } catch (...) {
                report.openError = "exception in a render thread";
            }
        });
    }
    // ---- stage threads: short-lived readers on the same shared device ----------
    for (int s = 0; s < kStageThreads; ++s) {
        const int w = kRenderThreads + s;
        threads.emplace_back([&, w, s]() noexcept {
            LrfWorkerReport& report = reports[static_cast<std::size_t>(w)];
            try {
                LandingWalk stageWalk(0x165667B19E3779F9ull + static_cast<std::uint64_t>(s));
                auto swStage = HevcStreamDecoder::open(path, 1, softwareReferenceOptions());
                if (!swStage.ok()) {
                    report.openError = swStage.error().toString();
                    return;
                }
                for (int round = 0; round < stageRounds; ++round) {
                    auto hwStage = HevcStreamDecoder::open(path, 1, stageD3d11Options());
                    if (!hwStage.ok()) {
                        report.openError = hwStage.error().toString();
                        return;
                    }
                    for (int k = 0; k < 3; ++k) {
                        compareOne(hwStage.value(), swStage.value(), stageWalk.draw(frames), w, round * 3 + k,
                                   report);
                    }
                    report.hardwareWorker = true;
                    report.activeHw = hwStage.value().activeHw();
                    // hwStage is destroyed here, its surfaces and decoder with it,
                    // while the render threads keep decoding on the same device.
                }
            } catch (...) {
                report.openError = "exception in a stage thread";
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    judgeWorkers(reports, "handed-off reader");
}

// -----------------------------------------------------------------------------
//  Replay of a recorded request order
// -----------------------------------------------------------------------------
// OSV_SEEK_SEQUENCE names a text file of frame indices, one per line, in the
// order a host session decoded them (taken from the importer log).  One
// importer-equivalent reader decodes them in that order and each picture is
// compared with a software decode.  SKIPs without the variable.
TEST_CASE("D3D11VA replay of a recorded LRF request order matches software", "[video][hwaccel][.lrfstress][lrfseek][lrfreplay]") {
    const std::filesystem::path path = stressLrfPath();
    const char* sequenceEnv = std::getenv("OSV_SEEK_SEQUENCE");
    if (sequenceEnv == nullptr || *sequenceEnv == '\0' || !std::filesystem::exists(path)) {
        SKIP("OSV_SEEK_SEQUENCE or the LRF is not available");
    }
    std::vector<std::uint32_t> order;
    {
        std::ifstream in(sequenceEnv);
        REQUIRE(in.good());
        long long v = 0;
        while (in >> v) {
            if (v >= 0) {
                order.push_back(static_cast<std::uint32_t>(v));
            }
        }
    }
    REQUIRE_FALSE(order.empty());
    auto hwOpened = HevcStreamDecoder::open(path, 1, importerD3d11Options());
    if (!hwOpened.ok()) {
        SKIP("d3d11va decoder unavailable: " << hwOpened.error().toString());
    }
    auto swOpened = HevcStreamDecoder::open(path, 1, softwareReferenceOptions());
    REQUIRE(swOpened.ok());
    std::vector<LrfWorkerReport> reports(1);
    const std::uint32_t frames = hwOpened.value().frameCount();
    int request = 0;
    for (const std::uint32_t index : order) {
        if (index < frames) {
            compareOne(hwOpened.value(), swOpened.value(), index, 0, request, reports[0]);
        }
        ++request;
    }
    reports[0].hardwareWorker = true;
    reports[0].activeHw = hwOpened.value().activeHw();
    judgeWorkers(reports, "replayed order");
}

// =============================================================================
//  Observability: the log sink, fingerprints, damage flags, the shadow check
// =============================================================================
// Everything below runs on the CPU (software decoding only): it is what makes
// a damaged proxy frame in a host session diagnosable - library and FFmpeg
// messages that reach the host's log, a per-picture fingerprint, libavcodec's
// own damage flags, and a software re-decode that names the first bad row.
namespace {

/// @brief Captures every osv::log message for its lifetime.
///
/// Installs itself as the library's sink at `level` and puts stderr and the
/// previous level back on destruction.  The sink is called from FFmpeg's
/// frame threads too, hence the mutex.
class CapturedLog {
public:
    explicit CapturedLog(log::Level level) : m_previous(log::level()) {
        log::setSink(&CapturedLog::sink, this);
        log::setLevel(level);
    }
    ~CapturedLog() {
        log::setSink(nullptr, nullptr);
        log::setLevel(m_previous);
    }
    CapturedLog(const CapturedLog&) = delete;
    CapturedLog& operator=(const CapturedLog&) = delete;

    /// Every line captured so far, in arrival order.
    [[nodiscard]] std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lines;
    }
    /// How many captured lines contain `needle`.
    [[nodiscard]] std::size_t count(const std::string& needle) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<std::size_t>(std::count_if(m_lines.begin(), m_lines.end(), [&](const std::string& l) {
            return l.find(needle) != std::string::npos;
        }));
    }
    /// The first captured line containing `needle` (empty when none).
    [[nodiscard]] std::string first(const std::string& needle) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::string& l : m_lines) {
            if (l.find(needle) != std::string::npos) {
                return l;
            }
        }
        return {};
    }

private:
    static void sink(log::Level /*level*/, std::string_view text, void* user) {
        auto* self = static_cast<CapturedLog*>(user);
        if (self == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_lines.emplace_back(text);
    }

    mutable std::mutex m_mutex;
    std::vector<std::string> m_lines;
    log::Level m_previous;
};

/// Set an environment variable for the lifetime of the object (the C
/// runtime's copy, which std::getenv - and the library - read).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : m_name(name) {
        if (const char* old = std::getenv(name); old != nullptr) {
            m_had = true;
            m_old = old;
        }
        set(value);
    }
    ~ScopedEnv() { set(m_had ? m_old.c_str() : ""); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void set(const char* value) {
#if defined(_WIN32)
        ::_putenv_s(m_name.c_str(), value);  // "" removes the variable
#else
        if (value[0] == '\0') {
            ::unsetenv(m_name.c_str());
        } else {
            ::setenv(m_name.c_str(), value, 1);
        }
#endif
    }
    std::string m_name;
    std::string m_old;
    bool m_had = false;
};

/// @brief A copy of an LRF in which ONE P picture is truncated.
///
/// The second half of the picture's slice NAL is zeroed in the copy, the way
/// a recording damaged mid-picture looks to a decoder: the first rows decode
/// as recorded, the rest come from libavcodec's concealment (pictures of
/// earlier frames), and every later picture of the GOP predicts from that.
/// The next sync sample heals it.
struct DamagedLrf {
    std::filesystem::path dir;      ///< A private temporary folder (removed by the test).
    std::filesystem::path path;     ///< The damaged copy.
    std::uint32_t damaged = 0;      ///< The truncated picture.
    std::uint32_t sync = 0;         ///< Its GOP's sync sample.
    std::uint32_t nextSync = 0;     ///< The next GOP's sync sample.
};

/// Build a DamagedLrf of `clean` in `<temp>/openosv-<tag>` (REQUIREs on the
/// way: a clip this cannot damage is a broken fixture, not a skip).
DamagedLrf makeDamagedLrf(const std::filesystem::path& clean, const std::string& tag) {
    DamagedLrf out;
    // ---- the sample table: a P picture with clean pictures around it ------------
    auto file = OsvFile::open(clean);
    REQUIRE(file.ok());
    const TrackInfo* track = file.value().track(1);
    REQUIRE(track != nullptr);
    const std::vector<std::uint32_t>& syncs = track->samples.syncSamples();
    REQUIRE(syncs.size() >= 2);
    bool found = false;
    for (std::size_t i = 0; i + 1 < syncs.size() && !found; ++i) {
        // stss is 1-based in the file; syncSamples() holds 0-based indices
        // (isSync agrees, checked below).  Six pictures of room: the damaged
        // one at +3 has two clean predecessors and two damaged successors.
        if (syncs[i + 1] >= syncs[i] + 6u) {
            out.sync = syncs[i];
            out.nextSync = syncs[i + 1];
            out.damaged = syncs[i] + 3u;
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(track->samples.isSync(out.sync));
    REQUIRE_FALSE(track->samples.isSync(out.damaged));

    // ---- the bytes, with the damaged picture's last NAL half zeroed -----------
    std::vector<char> bytes;
    {
        std::ifstream in(clean, std::ios::binary);
        REQUIRE(in.good());
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::uint64_t offset = track->samples.sampleOffset(out.damaged);
    const std::uint32_t size = track->samples.sampleSize(out.damaged);
    REQUIRE(size > 16u);
    REQUIRE(offset + size <= bytes.size());
    // Walk the 4-byte NAL length prefixes (the avcC of every camera LRF) to
    // the last NAL, so no length prefix is ever touched.
    std::uint64_t pos = offset;
    std::uint64_t lastPayload = 0;
    std::uint32_t lastLength = 0;
    while (pos + 4u <= offset + size) {
        const auto b = [&](std::uint64_t at) { return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])); };
        const std::uint32_t len = (b(pos) << 24) | (b(pos + 1) << 16) | (b(pos + 2) << 8) | b(pos + 3);
        REQUIRE(len > 0u);
        REQUIRE(pos + 4u + len <= offset + size);
        lastPayload = pos + 4u;
        lastLength = len;
        pos += 4u + len;
    }
    REQUIRE(pos == offset + size);
    REQUIRE(lastLength > 8u);
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(lastPayload + lastLength / 2u),
              bytes.begin() + static_cast<std::ptrdiff_t>(lastPayload + lastLength), '\0');

    // ---- written to a private folder ---------------------------------------------
    std::error_code ec;
    out.dir = std::filesystem::temp_directory_path(ec) / ("openosv-" + tag);
    REQUIRE_FALSE(ec);
    std::filesystem::remove_all(out.dir, ec);
    std::filesystem::create_directories(out.dir, ec);
    REQUIRE_FALSE(ec);
    out.path = out.dir / ("damaged_" + clean.filename().string());
    std::ofstream o(out.path, std::ios::binary | std::ios::trunc);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(o.good());
    return out;
}

/// A software decoder of `path` the way the importer's software fallback
/// opens one, with two frame threads (CPU-friendly) and the sample feed.
Result<HevcStreamDecoder> openSoftware(const std::filesystem::path& path) {
    DecoderOptions o;
    o.hw = HwAccel::None;
    o.threads = 2;
    o.useContainerSamples = true;
    return HevcStreamDecoder::open(path, 1, o);
}

/// A synthetic 4:2:0 frame owning its planes: planar 10-bit (bitShift 0), or
/// P010-like (values << 6, Cb/Cr interleaved) carrying the same values.
PlanarFrame16 syntheticFrame(std::uint32_t w, std::uint32_t h, bool p010,
                             const std::function<std::uint16_t(int plane, std::uint32_t x, std::uint32_t y)>& value) {
    PlanarFrame16 f;
    f.width = w;
    f.height = h;
    f.chromaW = (w + 1) / 2;
    f.chromaH = (h + 1) / 2;
    f.bitDepth = 10;
    f.bitShift = p010 ? 6 : 0;
    f.chromaInterleaved = p010;
    const std::size_t lumaElems = static_cast<std::size_t>(w) * h;
    const std::size_t chromaRow = p010 ? static_cast<std::size_t>(f.chromaW) * 2u : f.chromaW;
    const std::size_t chromaElems = chromaRow * f.chromaH;
    auto buffer = std::make_shared<std::vector<std::uint16_t>>(lumaElems + (p010 ? chromaElems : 2u * chromaElems));
    std::uint16_t* y = buffer->data();
    std::uint16_t* cb = y + lumaElems;
    std::uint16_t* cr = p010 ? cb + 1 : cb + chromaElems;
    const int shift = f.bitShift;
    for (std::uint32_t r = 0; r < h; ++r) {
        for (std::uint32_t c = 0; c < w; ++c) {
            y[static_cast<std::size_t>(r) * w + c] = static_cast<std::uint16_t>(value(0, c, r) << shift);
        }
    }
    const std::size_t step = p010 ? 2u : 1u;
    for (std::uint32_t r = 0; r < f.chromaH; ++r) {
        for (std::uint32_t c = 0; c < f.chromaW; ++c) {
            cb[r * chromaRow + c * step] = static_cast<std::uint16_t>(value(1, c, r) << shift);
            cr[r * chromaRow + c * step] = static_cast<std::uint16_t>(value(2, c, r) << shift);
        }
    }
    f.plane = {y, cb, cr};
    f.strideElems = {w, chromaRow, chromaRow};
    f.owner = std::static_pointer_cast<void>(buffer);
    return f;
}

}  // namespace

TEST_CASE("CRC-64/XZ matches its check value and the byte-wise form", "[video][crc]") {
    // ---- the published check value of CRC-64/XZ ----------------------------------
    const char* check = "123456789";
    CHECK(osv::crc64(check, 9) == 0x995DC9BBDF1939FAull);
    // ---- slicing by 8 against the tests' byte-at-a-time form, every tail length ---
    std::vector<std::uint8_t> data(1031);
    std::uint64_t state = 0x243F6A8885A308D3ull;
    for (std::uint8_t& b : data) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        b = static_cast<std::uint8_t>(state >> 56);
    }
    for (std::size_t n : {0u, 1u, 7u, 8u, 9u, 63u, 64u, 1031u}) {
        Crc64 reference;
        reference.update(data.data(), n);
        INFO("length " << n);
        CHECK(osv::crc64(data.data(), n) == (n == 0 ? 0u : reference.value()));
    }
    // ---- streaming: two blocks give the CRC of their concatenation -------------------
    CHECK(osv::crc64(data.data() + 500, 531, osv::crc64(data.data(), 500)) == osv::crc64(data.data(), 1031));
    // A null pointer with a size is refused without touching memory.
    CHECK(osv::crc64(nullptr, 16, 42u) == 42u);
}

TEST_CASE("frameFingerprint is the same for planar and interleaved layouts of one picture", "[video][crc]") {
    const auto value = [](int plane, std::uint32_t x, std::uint32_t y) {
        return static_cast<std::uint16_t>((plane * 311 + x * 7 + y * 13) % 1024);
    };
    const PlanarFrame16 planar = syntheticFrame(64, 36, false, value);
    const PlanarFrame16 p010 = syntheticFrame(64, 36, true, value);
    REQUIRE(planar.valid());
    REQUIRE(p010.valid());
    const std::uint64_t a = frameFingerprint(planar);
    CHECK(a != 0u);
    // NV12 / P010 from a hardware decoder and yuv420p from software: one value.
    CHECK(frameFingerprint(p010) == a);
    // Row 8 is hashed (every 4th row), row 9 is not.
    const PlanarFrame16 row8 = syntheticFrame(64, 36, false, [&](int plane, std::uint32_t x, std::uint32_t y) {
        return static_cast<std::uint16_t>(value(plane, x, y) ^ ((plane == 0 && y == 8 && x == 5) ? 1u : 0u));
    });
    const PlanarFrame16 row9 = syntheticFrame(64, 36, false, [&](int plane, std::uint32_t x, std::uint32_t y) {
        return static_cast<std::uint16_t>(value(plane, x, y) ^ ((plane == 0 && y == 9 && x == 5) ? 1u : 0u));
    });
    CHECK(frameFingerprint(row8) != a);
    CHECK(frameFingerprint(row9) == a);
    // A chroma change in a hashed chroma row counts too.
    const PlanarFrame16 cr = syntheticFrame(64, 36, false, [&](int plane, std::uint32_t x, std::uint32_t y) {
        return static_cast<std::uint16_t>(value(plane, x, y) ^ ((plane == 2 && y == 4 && x == 3) ? 2u : 0u));
    });
    CHECK(frameFingerprint(cr) != a);
    // An invalid frame has no fingerprint.
    CHECK(frameFingerprint(PlanarFrame16{}) == 0u);
}

TEST_CASE("compareDecodedLuma names the first bad row and the bottom half's share", "[video][verify]") {
    const auto clean = [](int plane, std::uint32_t x, std::uint32_t y) {
        return static_cast<std::uint16_t>(64 + (plane * 97 + x * 5 + y * 3) % 800);
    };
    const PlanarFrame16 a = syntheticFrame(64, 32, false, clean);
    const PlanarFrame16 sameP010 = syntheticFrame(64, 32, true, clean);
    // ---- identical content, different layouts: no difference ---------------------
    const LumaComparison same = compareDecodedLuma(a, sameP010);
    REQUIRE(same.comparable);
    CHECK_FALSE(same.mismatch());
    CHECK(std::isinf(same.psnrDb));
    CHECK(same.badRows == 0u);
    CHECK(same.firstBadRow == 32u);  // == height: none
    // ---- rows 20..31 damaged: all twelve in the bottom half --------------------------
    const PlanarFrame16 damaged = syntheticFrame(64, 32, false, [&](int plane, std::uint32_t x, std::uint32_t y) {
        return plane == 0 && y >= 20 ? static_cast<std::uint16_t>(clean(plane, x, y) / 2) : clean(plane, x, y);
    });
    const LumaComparison bad = compareDecodedLuma(damaged, a);
    REQUIRE(bad.comparable);
    CHECK(bad.mismatch());
    CHECK(bad.psnrDb < kDecodeMismatchPsnrDb);
    CHECK(bad.firstBadRow == 20u);
    CHECK(bad.lastBadRow == 31u);
    CHECK(bad.badRows == 12u);
    CHECK(bad.badRowsBottom == 12u);
    // ---- different sizes, or an invalid frame: not comparable -------------------------
    CHECK_FALSE(compareDecodedLuma(a, syntheticFrame(32, 32, false, clean)).comparable);
    CHECK_FALSE(compareDecodedLuma(a, PlanarFrame16{}).comparable);
    CHECK_FALSE(compareDecodedLuma(a, PlanarFrame16{}).mismatch());
}

TEST_CASE("a log sink receives the library's messages at the level set and never recurses", "[video][log]") {
    // ---- delivered, at the level -------------------------------------------------------
    {
        CapturedLog captured(log::Level::Warn);
        CHECK(log::enabled(log::Level::Warn));
        CHECK_FALSE(log::enabled(log::Level::Info));
        CHECK_FALSE(log::enabled(log::Level::Off));
        log::warn("sink-marker {}", 7);
        log::info("below-the-level {}", 8);
        CHECK(captured.count("sink-marker 7") == 1u);
        CHECK(captured.count("below-the-level") == 0u);
        // The level is the sink's to follow: Debug lets debug lines through.
        log::setLevel(log::Level::Debug);
        CHECK(log::enabled(log::Level::Debug));
        log::debug("debug-marker");
        CHECK(captured.count("debug-marker") == 1u);
    }
    // ---- a sink that logs from inside itself: the nested line goes to stderr ----------
    {
        struct Recursing {
            static void sink(log::Level, std::string_view text, void* user) {
                auto* calls = static_cast<std::atomic<int>*>(user);
                calls->fetch_add(1);
                // Would recurse without end if osv::log re-entered the sink.
                log::warn("nested inside the sink: {}", text);
            }
        };
        std::atomic<int> calls{0};
        const log::Level previous = log::level();
        log::setLevel(log::Level::Warn);
        log::setSink(&Recursing::sink, &calls);
        log::warn("outer");
        log::setSink(nullptr, nullptr);
        log::setLevel(previous);
        CHECK(calls.load() == 1);
    }
    // ---- removed: messages go back to stderr, the old sink sees nothing -------------
    {
        std::vector<std::string> seen;
        {
            CapturedLog captured(log::Level::Warn);
            log::warn("while installed");
            seen = captured.lines();
        }
        log::warn("after removal (stderr)");
        CHECK(seen.size() == 1u);
    }
}

TEST_CASE("a damaged LRF picture: FFmpeg's lines name the clip, the flags are reported, software serves it",
          "[video][sample][verify]") {
    OSV_REQUIRE_SAMPLE_LRF();
    const DamagedLrf damaged = makeDamagedLrf(osvtest::sampleLrf(), "lrfverify-flags");
    {
        CapturedLog captured(log::Level::Warn);
        auto opened = openSoftware(damaged.path);
        REQUIRE(opened.ok());
        HevcStreamDecoder& dec = opened.value();
        // ---- the picture before the damage is clean ---------------------------------
        auto before = dec.decodeFrame(damaged.damaged - 1u);
        REQUIRE(before.ok());
        REQUIRE(dec.lastFrameInfo().has_value());
        CHECK(dec.lastFrameInfo()->decodeErrorFlags == 0);
        CHECK_FALSE(dec.lastFrameInfo()->corrupt);
        CHECK(dec.lastFrameInfo()->hw == HwAccel::None);
        CHECK(dec.lastFrameInfo()->surface == -1);
        CHECK(dec.lastFrameInfo()->gopDamagedAt == -1);
        // ---- the damaged one: still served (software has no fallback), flagged --------
        auto hit = dec.decodeFrame(damaged.damaged);
        REQUIRE(hit.ok());
        const std::optional<DecodedFrameInfo> info = dec.lastFrameInfo();
        REQUIRE(info.has_value());
        CHECK(info->index == damaged.damaged);
        CHECK_FALSE(info->keyFrame);
        WARN("damaged frame " << damaged.damaged << ": decode_error_flags 0x" << std::hex << info->decodeErrorFlags
                              << std::dec << ", corrupt " << info->corrupt);
        CHECK((info->decodeErrorFlags != 0 || info->corrupt));
        CHECK(info->gopDamagedAt == static_cast<std::int64_t>(damaged.damaged));
        CHECK(captured.count("decoded with damage") >= 1u);
        // ---- the next picture predicts from it: damaged without its own flag --------
        auto after = dec.decodeFrame(damaged.damaged + 1u);
        REQUIRE(after.ok());
        REQUIRE(dec.lastFrameInfo().has_value());
        CHECK(dec.lastFrameInfo()->gopDamagedAt == static_cast<std::int64_t>(damaged.damaged));
        // ---- and so does a fresh random access to a later picture of the GOP --------
        // (the damaged picture is decoded on the way, as a catch-up picture).
        auto fresh = openSoftware(damaged.path);
        REQUIRE(fresh.ok());
        auto later = fresh.value().decodeFrame(damaged.damaged + 2u);
        REQUIRE(later.ok());
        REQUIRE(fresh.value().lastFrameInfo().has_value());
        CHECK(fresh.value().lastFrameInfo()->gopDamagedAt == static_cast<std::int64_t>(damaged.damaged));
        // ---- FFmpeg said so too, and every line names the clip -----------------------
        const std::string tag = "ffmpeg: [h264 'damaged_" + osvtest::sampleLrf().filename().string() + "']";
        const std::vector<std::string> lines = captured.lines();
        std::size_t ffmpeg = 0;
        std::size_t tagged = 0;
        for (const std::string& l : lines) {
            if (l.rfind("ffmpeg: ", 0) == 0) {
                ++ffmpeg;
                tagged += l.rfind(tag, 0) == 0 ? 1u : 0u;
            }
        }
        INFO("first ffmpeg line: " << captured.first("ffmpeg: "));
        CHECK(ffmpeg >= 1u);
        CHECK(tagged == ffmpeg);
        // ---- the next GOP is whole again ---------------------------------------------
        auto healed = dec.decodeFrame(damaged.nextSync);
        REQUIRE(healed.ok());
        CHECK(dec.lastFrameInfo()->decodeErrorFlags == 0);
        CHECK(dec.lastFrameInfo()->keyFrame);
        CHECK(dec.lastFrameInfo()->gopDamagedAt == -1);
    }
    std::error_code ec;
    std::filesystem::remove_all(damaged.dir, ec);
}

TEST_CASE("the shadow verifier names the first bad row, dumps the planes and hands back the software picture",
          "[video][sample][verify]") {
    OSV_REQUIRE_SAMPLE_LRF();
    // The primary decodes the DAMAGED copy (standing in for a hardware decoder
    // that returned a wrong picture); the verifier's reference decodes the
    // clean file, as it decodes the reader's own file in production.
    const DamagedLrf damaged = makeDamagedLrf(osvtest::sampleLrf(), "lrfverify-shadow");
    const std::filesystem::path dumps = damaged.dir / "decode-mismatch";
    {
        CapturedLog captured(log::Level::Warn);
        auto primaryOpened = openSoftware(damaged.path);
        REQUIRE(primaryOpened.ok());
        HevcStreamDecoder& primary = primaryOpened.value();
        DecoderOptions primaryOptions;
        primaryOptions.useContainerSamples = true;
        auto verifierOpened = ShadowDecodeVerifier::open(osvtest::sampleLrf(), 1, primaryOptions, dumps);
        REQUIRE(verifierOpened.ok());
        ShadowDecodeVerifier& verifier = verifierOpened.value();
        REQUIRE(verifier.isOpen());
        auto clean = openSoftware(osvtest::sampleLrf());
        REQUIRE(clean.ok());
        const std::string clip = "verify-test.LRF";

        // ---- a clean picture: nothing to replace ---------------------------------------
        {
            auto p = primary.decodeFrame(damaged.damaged - 1u);
            REQUIRE(p.ok());
            auto v = verifier.check(damaged.damaged - 1u, p.value(), primary.lastFrameInfo(),
                                    primary.previousSyncIndex(damaged.damaged - 1u), clip);
            REQUIRE(v.ok());
            CHECK(v.value().comparison.comparable);
            CHECK_FALSE(v.value().comparison.mismatch());
            CHECK_FALSE(v.value().replacement.has_value());
        }
        // ---- the damaged picture: named, dumped, replaced ------------------------------
        {
            auto p = primary.decodeFrame(damaged.damaged);
            REQUIRE(p.ok());
            auto v = verifier.check(damaged.damaged, p.value(), primary.lastFrameInfo(),
                                    primary.previousSyncIndex(damaged.damaged), clip);
            REQUIRE(v.ok());
            const LumaComparison& c = v.value().comparison;
            WARN("damaged frame " << damaged.damaged << ": PSNR " << c.psnrDb << " dB, first bad row "
                                  << c.firstBadRow << ", last " << c.lastBadRow << ", " << c.badRows << " bad rows ("
                                  << c.badRowsBottom << " in the bottom half) of " << c.height);
            CHECK(c.mismatch());
            CHECK(c.badRows > 0u);
            CHECK(c.firstBadRow < c.height);
            CHECK(c.badRowsBottom > 0u);
            // The software picture comes back, and it IS the clean picture.
            REQUIRE(v.value().replacement.has_value());
            auto reference = clean.value().decodeFrame(damaged.damaged);
            REQUIRE(reference.ok());
            CHECK(std::isinf(compareDecodedLuma(*v.value().replacement, reference.value()).psnrDb));
            CHECK(frameFingerprint(*v.value().replacement) == frameFingerprint(reference.value()));
            // Both luma planes on disk, as 16-bit PGMs of the right size.
            REQUIRE_FALSE(v.value().dumpedPrimary.empty());
            REQUIRE_FALSE(v.value().dumpedReference.empty());
            const std::uintmax_t pixels = static_cast<std::uintmax_t>(p.value().width) * p.value().height;
            CHECK(std::filesystem::file_size(v.value().dumpedPrimary) > pixels * 2u);
            CHECK(std::filesystem::file_size(v.value().dumpedReference) > pixels * 2u);
            // The warning says where and how far into the GOP.
            const std::string warning = captured.first("differs from software");
            INFO(warning);
            CHECK(warning.find("frame " + std::to_string(damaged.damaged) + " (sync " +
                               std::to_string(damaged.sync) + " + 3)") != std::string::npos);
            CHECK(warning.find("first bad row " + std::to_string(c.firstBadRow)) != std::string::npos);
            CHECK(warning.find("serving the software picture") != std::string::npos);
            // The reference decoded the CLEAN file: the line blames the primary.
            CHECK(warning.find("the software decode is clean") != std::string::npos);
        }
        // ---- the rest of the GOP predicts from it; only its onset is dumped ----------
        {
            auto p = primary.decodeFrame(damaged.damaged + 1u);
            REQUIRE(p.ok());
            auto v = verifier.check(damaged.damaged + 1u, p.value(), primary.lastFrameInfo(),
                                    primary.previousSyncIndex(damaged.damaged + 1u), clip);
            REQUIRE(v.ok());
            CHECK(v.value().comparison.mismatch());
            CHECK(v.value().replacement.has_value());
            CHECK(v.value().dumpedPrimary.empty());
        }
        // ---- the next GOP: clean again -------------------------------------------------
        {
            auto p = primary.decodeFrame(damaged.nextSync);
            REQUIRE(p.ok());
            auto v = verifier.check(damaged.nextSync, p.value(), primary.lastFrameInfo(),
                                    primary.previousSyncIndex(damaged.nextSync), clip);
            REQUIRE(v.ok());
            CHECK_FALSE(v.value().comparison.mismatch());
        }
        CHECK(verifier.checked() == 4u);
        CHECK(verifier.mismatched() == 2u);
    }
    // ---- a reference that decodes a DAMAGED recording says so -----------------------------
    // In production the reference decodes the reader's own file.  When that
    // file is damaged, a mismatch is two decoders concealing differently, not
    // a hardware fault - also for a picture that is only PREDICTED from the
    // damaged one and carries no flag of its own.  Roles swapped here: the
    // primary is the clean decode, the reference the damaged file.
    {
        CapturedLog captured(log::Level::Warn);
        auto primaryOpened = openSoftware(osvtest::sampleLrf());
        REQUIRE(primaryOpened.ok());
        DecoderOptions primaryOptions;
        primaryOptions.useContainerSamples = true;
        auto verifierOpened = ShadowDecodeVerifier::open(damaged.path, 1, primaryOptions, std::filesystem::path());
        REQUIRE(verifierOpened.ok());
        const std::uint32_t index = damaged.damaged + 1u;
        auto p = primaryOpened.value().decodeFrame(index);
        REQUIRE(p.ok());
        auto v = verifierOpened.value().check(index, p.value(), primaryOpened.value().lastFrameInfo(),
                                             primaryOpened.value().previousSyncIndex(index), "verify-test.LRF");
        REQUIRE(v.ok());
        CHECK(v.value().comparison.mismatch());
        // No dump folder given: nothing written.
        CHECK(v.value().dumpedPrimary.empty());
        const std::string warning = captured.first("differs from software");
        INFO(warning);
        CHECK(warning.find("the software decode saw damage too, at frame " + std::to_string(damaged.damaged)) !=
              std::string::npos);
    }
    std::error_code ec;
    std::filesystem::remove_all(damaged.dir, ec);
}

TEST_CASE("the reader's shadow check never runs on a software picture", "[video][sample][verify]") {
    OSV_REQUIRE_SAMPLE_LRF();
    // OPENOSV_VERIFY_HW_DECODE asks for the check, but a software primary has
    // nothing independent to be compared with: the verifier never opens.
    ScopedEnv verify("OPENOSV_VERIFY_HW_DECODE", "1");
    DecoderOptions o;
    o.hw = HwAccel::None;
    o.threads = 2;
    o.useContainerSamples = true;
    auto reader = DualStreamReader::open(osvtest::sampleLrf(), proxyFormat(), o);
    REQUIRE(reader.ok());
    for (const std::uint32_t i : {0u, 1u, 5u}) {
        auto pair = reader.value().read(i);
        REQUIRE(pair.ok());
    }
    CHECK(reader.value().verifiedFrames() == 0u);
    CHECK(reader.value().verifyMismatches() == 0u);
}

TEST_CASE("the decoder's Debug line carries the picture's fingerprint", "[video][sample][log]") {
    OSV_REQUIRE_SAMPLE_LRF();
    CapturedLog captured(log::Level::Debug);
    auto opened = openSoftware(osvtest::sampleLrf());
    REQUIRE(opened.ok());
    auto frame = opened.value().decodeFrame(3);
    REQUIRE(frame.ok());
    char crc[17] = {};
    std::snprintf(crc, sizeof(crc), "%016llx", static_cast<unsigned long long>(frameFingerprint(frame.value())));
    const std::string line = captured.first("decode: track 1 frame 3 ");
    INFO(line);
    CHECK(line.find(std::string("crc ") + crc + " hw none key 0") != std::string::npos);
    CHECK(line.find("'" + osvtest::sampleLrf().filename().string() + "'") != std::string::npos);
    // The open itself is at Debug too, and now reaches a sink.
    CHECK(captured.count("video: opened track 1 of ") == 1u);
}

// -----------------------------------------------------------------------------
//  Software fingerprints for a host session's log (hidden, CPU only)
// -----------------------------------------------------------------------------
// The importer at Debug writes 'decode: track 1 frame N crc C hw H ...' for
// every picture it decodes.  This prints the SOFTWARE fingerprint of the same
// frames in the same format, so the two can be lined up frame by frame: a
// frame whose hardware CRC differs from this one was decoded wrong.
//   OSV_SEEK_LRF=<clip> OSV_CRC_FRAMES=<first>-<last> osv_tests "[.lrfcrc]"
// (or OSV_SEEK_SEQUENCE=<file of indices>).  Software decoding only.
TEST_CASE("software fingerprints of an LRF, for lining up with a session log", "[video][.lrfcrc]") {
    const std::filesystem::path path = stressLrfPath();
    if (!std::filesystem::exists(path)) {
        SKIP("LRF not available: " << path.string());
    }
    // ---- which frames ---------------------------------------------------------------
    std::vector<std::uint32_t> order;
    if (const char* range = std::getenv("OSV_CRC_FRAMES"); range != nullptr && *range != '\0') {
        unsigned long long a = 0;
        unsigned long long b = 0;
        char dash = 0;
        std::istringstream in(range);
        REQUIRE(static_cast<bool>(in >> a >> dash >> b));
        REQUIRE(dash == '-');
        REQUIRE(a <= b);
        REQUIRE(b - a < 100000u);
        for (unsigned long long i = a; i <= b; ++i) {
            order.push_back(static_cast<std::uint32_t>(i));
        }
    } else if (const char* seq = std::getenv("OSV_SEEK_SEQUENCE"); seq != nullptr && *seq != '\0') {
        std::ifstream in(seq);
        REQUIRE(in.good());
        long long v = 0;
        while (in >> v) {
            if (v >= 0) {
                order.push_back(static_cast<std::uint32_t>(v));
            }
        }
    } else {
        for (std::uint32_t i = 0; i < 30u; ++i) {
            order.push_back(i);
        }
    }
    // ---- decode and print ------------------------------------------------------------
    // The decode and the fingerprint are timed too: what the software path
    // (OPENOSV_IMPORTER_LRF_SOFTWARE, the shadow check's reference) and the
    // Debug 'decode:' line cost per picture on this machine.
    using Clock = std::chrono::steady_clock;
    // OSV_CRC_THREADS: the decoder's frame threads (default 2, the shadow
    // check's reference; 0 = libavcodec's own count, the importer's software
    // reader).  Bit-exact streams decode identically either way; a DAMAGED
    // picture's concealment need not, which is worth being able to see.
    DecoderOptions crcOptions;
    crcOptions.hw = HwAccel::None;
    crcOptions.threads = 2;
    crcOptions.useContainerSamples = true;
    if (const char* t = std::getenv("OSV_CRC_THREADS"); t != nullptr && *t != '\0') {
        crcOptions.threads = std::clamp(std::atoi(t), 0, 64);
    }
    auto opened = HevcStreamDecoder::open(path, 1, crcOptions);
    REQUIRE(opened.ok());
    HevcStreamDecoder& dec = opened.value();
    const std::string clip = path.filename().string();
    int printed = 0;
    double decodeMs = 0.0;
    double fingerprintMs = 0.0;
    for (const std::uint32_t index : order) {
        if (index >= dec.frameCount()) {
            continue;
        }
        const auto t0 = Clock::now();
        auto f = dec.decodeFrame(index);
        const auto t1 = Clock::now();
        REQUIRE(f.ok());
        const std::uint64_t crc = frameFingerprint(f.value());
        const auto t2 = Clock::now();
        decodeMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        fingerprintMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
        const std::optional<DecodedFrameInfo> info = dec.lastFrameInfo();
        std::printf("decode: track 1 frame %u crc %016llx hw none key %d '%s'\n", index,
                    static_cast<unsigned long long>(crc), (info && info->keyFrame) ? 1 : 0, clip.c_str());
        ++printed;
    }
    REQUIRE(printed > 0);
    // ---- the CRC's own throughput (a 32 MiB buffer: one 2048x1024 32f frame) -------
    std::vector<std::uint8_t> block(32u << 20);
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = static_cast<std::uint8_t>(i * 2654435761u >> 24);
    }
    const auto c0 = Clock::now();
    const std::uint64_t blockCrc = osv::crc64(block.data(), block.size());
    const double crcMs = std::chrono::duration<double, std::milli>(Clock::now() - c0).count();
    std::printf("timing: %d pictures %ux%u, software decode %.2f ms/picture (%d threads, this order), fingerprint "
                "%.3f ms/picture, crc64 %.2f GB/s (crc %016llx)\n",
                printed, dec.width(), dec.height(), decodeMs / printed, crcOptions.threads, fingerprintMs / printed,
                crcMs > 0.0 ? static_cast<double>(block.size()) / (crcMs * 1.0e6) : 0.0,
                static_cast<unsigned long long>(blockCrc));
    std::fflush(stdout);
}

TEST_CASE("CUDA decode matches software within 50 dB", "[video][sample][hwaccel][cuda]") {
    OSV_REQUIRE_SAMPLE();
    const SequentialPass& pass = sequentialPass();
    INFO("pass error: " << pass.error);
    REQUIRE(pass.error.empty());
    HevcStreamDecoder hw = openHwOrSkip(HwAccel::Cuda);
    auto sw = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, DecoderOptions{});
    REQUIRE(sw.ok());
    for (const std::uint32_t i : {0u, 37u}) {
        auto a = hw.decodeFrame(i);
        auto b = sw.value().decodeFrame(i);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        const PlanarFrame16& f = a.value();
        REQUIRE(f.valid());
        REQUIRE(f.frameIndex == i);
        REQUIRE(f.ptsUs == b.value().ptsUs);
        const double psnr = lumaPsnr(f, b.value());
        WARN("cuda frame " << i << " luma PSNR vs software " << psnr << " dB");
        REQUIRE(psnr >= 50.0);
    }
    // Device-resident frames: pointers but no host planes.
    DecoderOptions options;
    options.hw = HwAccel::Cuda;
    options.keepOnDevice = true;
    auto onDevice = HevcStreamDecoder::open(osvtest::sampleOsv(), 1, options);
    REQUIRE(onDevice.ok());
    auto frame = onDevice.value().decodeFrame(3);
    REQUIRE(frame.ok());
    REQUIRE_FALSE(frame.value().valid());
    REQUIRE(frame.value().width == 3000);
    REQUIRE(frame.value().owner != nullptr);
    const auto ref = onDevice.value().lastDeviceFrame();
    REQUIRE(ref.has_value());
    REQUIRE(ref->valid());
    REQUIRE(ref->width == 3000);
    REQUIRE(ref->height == 3000);
    REQUIRE(ref->bitShift == 6);
    REQUIRE(ref->pitchBytes >= 3000 * 2);
}

TEST_CASE("HwAccel::Auto never fails just because no GPU is present", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    DecoderOptions options;
    options.hw = HwAccel::Auto;
    auto opened = HevcStreamDecoder::open(osvtest::sampleOsv(), 2, options);
    REQUIRE(opened.ok());
    WARN("auto selected " << hwAccelName(opened.value().activeHw()));
    auto frame = opened.value().decodeFrame(1);
    REQUIRE(frame.ok());
    REQUIRE(frame.value().frameIndex == 1);
    REQUIRE(frame.value().valid());
}

// =============================================================================
//  Extraction (container parser only)
// =============================================================================
TEST_CASE("extract --hevc writes an Annex-B stream with one start code per NAL", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::filesystem::path out = osvtest::tempDir() / "track1.hevc";
    auto stats = writeAnnexBStream(osvtest::sampleOsv(), 1, out);
    REQUIRE(stats.ok());
    const AnnexBStats& s = stats.value();
    REQUIRE(s.trackId == 1);
    REQUIRE(s.samples == 65);
    REQUIRE(s.parameterSetNals >= 3);   // VPS + SPS + PPS at least
    REQUIRE(s.sampleNals >= 65ull * 3ull);  // three slices per picture
    const std::vector<std::uint8_t> bytes = readAll(out);
    REQUIRE(bytes.size() == s.bytesWritten);
    REQUIRE(bytes.size() >= 4);
    REQUIRE(bytes[0] == 0x00);
    REQUIRE(bytes[1] == 0x00);
    REQUIRE(bytes[2] == 0x00);
    REQUIRE(bytes[3] == 0x01);
    REQUIRE(countStartCodes(bytes) == s.parameterSetNals + s.sampleNals);
    // The first NAL after the header is a VPS (type 32) - HEVC NAL header
    // byte 0 = forbidden(1) | type(6) | layer_id_high(1).
    REQUIRE(((bytes[4] >> 1) & 0x3Fu) == 32);
    // Errors: a non-video track and a missing one.
    REQUIRE(writeAnnexBStream(osvtest::sampleOsv(), 3, osvtest::tempDir() / "bad.hevc").error().code == ErrorCode::NotFound);
    REQUIRE(writeAnnexBStream(osvtest::sampleOsv(), 42, osvtest::tempDir() / "bad.hevc").error().code == ErrorCode::NotFound);
    // Both lens tracks must have the same sample count.
    auto stats2 = writeAnnexBStream(osvtest::sampleOsv(), 2, osvtest::tempDir() / "track2.hevc");
    REQUIRE(stats2.ok());
    REQUIRE(stats2.value().samples == 65);
}

TEST_CASE("extract --audio writes ADTS AAC-LC 48 kHz stereo", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    const std::filesystem::path out = osvtest::tempDir() / "audio.aac";
    auto stats = writeAdtsAudio(osvtest::sampleOsv(), 0, out);
    REQUIRE(stats.ok());
    const AdtsStats& s = stats.value();
    REQUIRE(s.trackId == 3);
    REQUIRE(s.audioObjectType == 2);
    REQUIRE(s.sampleRate == 48000);
    REQUIRE(s.samplingIndex == 3);
    REQUIRE(s.channelConfig == 2);
    REQUIRE(s.samples > 0);
    const std::vector<std::uint8_t> bytes = readAll(out);
    REQUIRE(bytes.size() == s.bytesWritten);
    // Walk every ADTS frame: sync word, header fields, frame length chain.
    std::size_t pos = 0;
    std::uint32_t frames = 0;
    while (pos + 7 <= bytes.size()) {
        REQUIRE(bytes[pos] == 0xFF);
        REQUIRE((bytes[pos + 1] & 0xF6u) == 0xF0);
        REQUIRE((bytes[pos + 1] & 0x01u) == 0x01);  // protection_absent
        REQUIRE(((bytes[pos + 2] >> 6) & 0x03u) == 1);  // profile = LC
        REQUIRE(((bytes[pos + 2] >> 2) & 0x0Fu) == 3);  // 48 kHz
        const std::uint32_t channels = ((bytes[pos + 2] & 0x01u) << 2) | (bytes[pos + 3] >> 6);
        REQUIRE(channels == 2);
        const std::size_t length = ((bytes[pos + 3] & 0x03u) << 11) | (bytes[pos + 4] << 3) | (bytes[pos + 5] >> 5);
        REQUIRE(length > 7);
        REQUIRE(pos + length <= bytes.size());
        pos += length;
        ++frames;
    }
    REQUIRE(pos == bytes.size());
    REQUIRE(frames == s.samples);
    // Explicit track selection and errors.
    REQUIRE(writeAdtsAudio(osvtest::sampleOsv(), 3, osvtest::tempDir() / "audio2.aac").ok());
    REQUIRE(writeAdtsAudio(osvtest::sampleOsv(), 1, osvtest::tempDir() / "bad.aac").error().code == ErrorCode::NotFound);
}

// =============================================================================
//  IMU CSV
// =============================================================================
TEST_CASE("extract --imu writes one row per frame with the documented columns", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    auto track = meta::MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    REQUIRE(track.value().frameCount() == 65);

    std::ostringstream out;
    REQUIRE(writeImuCsv(track.value(), out, false).ok());
    const std::string csv = out.str();
    for (const char c : csv) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);
    }
    std::istringstream lines(csv);
    std::string line;
    REQUIRE(std::getline(lines, line));
    REQUIRE(line == "frame,seq,timestamp_us,att_w,att_x,att_y,att_z,acc_x,acc_y,acc_z,iso,exposure_num,exposure_den,wb_cct,sensor_temp");
    std::uint32_t rows = 0;
    std::string firstRow;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        if (rows == 0) {
            firstRow = line;
        }
        // 15 columns = 14 commas.
        std::size_t commas = 0;
        for (const char c : line) {
            commas += (c == ',') ? 1 : 0;
        }
        REQUIRE(commas == 14);
        ++rows;
    }
    REQUIRE(rows == 65);
    // Frame 0 golden values: seq 0, timestamp 30669420766, attitude
    // (0.46131432, 0.54081959, 0.54205739, 0.44819313), iso 142,
    // exposure [1, 208], wb 6545.
    REQUIRE(firstRow.rfind("0,0,30669420766,", 0) == 0);
    REQUIRE(firstRow.find(",0.4613143,") != std::string::npos);
    REQUIRE(firstRow.find(",0.5408196,") != std::string::npos);
    REQUIRE(firstRow.find(",142,1,208,6545,") != std::string::npos);
}

TEST_CASE("extract --imu --dense writes one row per IMU sample", "[video][sample]") {
    OSV_REQUIRE_SAMPLE();
    auto file = OsvFile::open(osvtest::sampleOsv());
    REQUIRE(file.ok());
    auto track = meta::MetadataTrack::load(file.value());
    REQUIRE(track.ok());
    std::ostringstream out;
    REQUIRE(writeImuCsv(track.value(), out, true).ok());
    std::istringstream lines(out.str());
    std::string line;
    REQUIRE(std::getline(lines, line));
    REQUIRE(line == "frame,sample_index,ts_ticks,q_w,q_x,q_y,q_z");
    std::uint64_t rows = 0;
    std::uint64_t previousTicks = 0;
    std::uint32_t previousFrame = 0;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        // frame,sample_index,ts_ticks,...
        const std::size_t c1 = line.find(',');
        const std::size_t c2 = line.find(',', c1 + 1);
        const std::size_t c3 = line.find(',', c2 + 1);
        REQUIRE(c3 != std::string::npos);
        const std::uint32_t frame = static_cast<std::uint32_t>(std::stoul(line.substr(0, c1)));
        const std::uint32_t sample = static_cast<std::uint32_t>(std::stoul(line.substr(c1 + 1, c2 - c1 - 1)));
        const std::uint64_t ticks = std::stoull(line.substr(c2 + 1, c3 - c2 - 1));
        if (rows > 0 && frame == previousFrame) {
            // Inside a batch the clock advances by exactly one sample period.
            REQUIRE(ticks == previousTicks + 1006);
        }
        if (sample == 0 && frame == 0) {
            REQUIRE(ticks == 604649192);
        }
        previousTicks = ticks;
        previousFrame = frame;
        ++rows;
    }
    // 16 or 17 samples per frame over 65 frames.
    REQUIRE(rows >= 65ull * 16ull);
    REQUIRE(rows <= 65ull * 17ull);
    // The stripped djmd track (5) has no IMU batches at all.
    auto stripped = meta::MetadataTrack::load(file.value(), 5u);
    REQUIRE(stripped.ok());
    std::ostringstream none;
    REQUIRE(writeImuCsv(stripped.value(), none, true).error().code == ErrorCode::NotFound);
    // The per-frame layout still works for it (camera data is present).
    std::ostringstream perFrame;
    REQUIRE(writeImuCsv(stripped.value(), perFrame, false).ok());
    // Path overload creates the file.
    const std::filesystem::path csvPath = osvtest::tempDir() / "imu.csv";
    REQUIRE(writeImuCsv(osvtest::sampleOsv(), csvPath, false).ok());
    REQUIRE(std::filesystem::file_size(csvPath) > 65 * 40);
    REQUIRE(writeImuCsv(osvtest::tempDir() / "missing.OSV", csvPath, false).error().code == ErrorCode::Io);
}

// =============================================================================
//  osvtool extract end to end (when the tool was built)
// =============================================================================
#if defined(OSV_TOOL_PATH)
TEST_CASE("osvtool extract --hevc / --audio / --imu / --frame run end to end", "[video][sample][cli]") {
    OSV_REQUIRE_SAMPLE();
    const std::filesystem::path tool(OSV_TOOL_PATH);
    std::error_code ec;
    if (!std::filesystem::exists(tool, ec)) {
        SKIP("osvtool not built at " << tool.string());
    }
    // Windows cmd needs the whole command line wrapped in an extra pair of
    // quotes when the program path itself is quoted.
    auto run = [&](const std::string& args) {
        const std::string cmd = "\"\"" + tool.string() + "\" extract \"" + osvtest::sampleOsv().string() + "\" " + args + "\"";
        return std::system(cmd.c_str());
    };
    const std::filesystem::path hevc = osvtest::tempDir() / "cli_track.hevc";
    const std::filesystem::path aac = osvtest::tempDir() / "cli_audio.aac";
    const std::filesystem::path csv = osvtest::tempDir() / "cli_imu.csv";
    const std::filesystem::path pgm = osvtest::tempDir() / "cli_frame.pgm";
    const std::filesystem::path ppm = osvtest::tempDir() / "cli_frame.ppm";
    REQUIRE(run("--hevc \"" + hevc.string() + "\" --stream 1") == 0);
    REQUIRE(run("--audio \"" + aac.string() + "\"") == 0);
    REQUIRE(run("--imu \"" + csv.string() + "\" --dense") == 0);
    REQUIRE(run("--frame 7 --lens 0 --out \"" + pgm.string() + "\"") == 0);
    REQUIRE(run("--frame 7 --lens 1 --out \"" + ppm.string() + "\" --container-samples") == 0);
    // PGM: header then 3000*3000*2 bytes.
    const std::vector<std::uint8_t> pgmBytes = readAll(pgm);
    const std::string pgmHeader = "P5\n3000 3000\n1023\n";
    REQUIRE(pgmBytes.size() == pgmHeader.size() + 3000ull * 3000ull * 2ull);
    REQUIRE(std::string(pgmBytes.begin(), pgmBytes.begin() + static_cast<std::ptrdiff_t>(pgmHeader.size())) == pgmHeader);
    const std::vector<std::uint8_t> ppmBytes = readAll(ppm);
    const std::string ppmHeader = "P6\n3000 3000\n1023\n";
    REQUIRE(ppmBytes.size() == ppmHeader.size() + 3000ull * 3000ull * 6ull);
    // Usage errors exit 1, missing input exits 2.
    REQUIRE(run("--hevc \"" + hevc.string() + "\" --audio \"" + aac.string() + "\"") == 1);
    REQUIRE(run("--frame 0 --lens 0 --out \"" + (osvtest::tempDir() / "cli_frame.png").string() + "\"") == 1);
    const std::string missing = "\"\"" + tool.string() + "\" extract \"" +
                                (osvtest::tempDir() / "missing.OSV").string() + "\" --audio \"" + aac.string() + "\"\"";
    REQUIRE(std::system(missing.c_str()) == 2);
}
#endif
