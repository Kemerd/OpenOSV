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
#include "osv/core/Result.h"
#include "osv/meta/FormatInfo.h"
#include "osv/meta/MetadataTrack.h"
#include "osv/video/Decoder.h"
#include "osv/video/DualStreamReader.h"
#include "osv/video/HwAccel.h"
#include "osv/video/ImuCsv.h"
#include "osv/video/PlanarFrame.h"
#include "osv/video/StreamExtract.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
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
