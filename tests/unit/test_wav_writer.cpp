// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// WavWriter: exact header bytes, round trip, crash safety and the 4 GiB guard.
// CPU-only and needs no sample clip.

#include <catch2/catch_test_macros.hpp>

#include "TestSample.h"
#include "WavTestUtil.h"

#include "osv/io/WavWriter.h"

#include <cmath>
#include <vector>

namespace {

/// A deterministic two-channel test signal with distinctive bit patterns.
std::vector<float> syntheticSignal(std::size_t frames, std::size_t channels) {
    std::vector<float> s(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            s[i * channels + c] = std::sin(0.37f * static_cast<float>(i) + static_cast<float>(c)) * 0.5f +
                                  static_cast<float>(c) * 0.001f;
        }
    }
    return s;
}

}  // namespace

TEST_CASE("WavWriter writes the exact float WAV header and round-trips the samples", "[io][wav]") {
    const auto path = osvtest::tempDir() / "wav_roundtrip.wav";
    std::filesystem::remove(path);

    constexpr std::size_t kFrames = 1000;
    constexpr std::uint32_t kChannels = 2;
    const std::vector<float> signal = syntheticSignal(kFrames, kChannels);

    auto opened = osv::io::WavWriter::open(path, 48000, kChannels, kFrames);
    REQUIRE(opened.ok());
    osv::io::WavWriter writer = std::move(opened).value();
    REQUIRE(writer.isOpen());
    REQUIRE(writer.channels() == kChannels);

    // Blocks of uneven size, as a streaming decoder would deliver them.
    std::size_t done = 0;
    for (const std::size_t block : {std::size_t{1}, std::size_t{333}, std::size_t{0}, std::size_t{666}}) {
        REQUIRE(writer.write(signal.data() + done * kChannels, block).ok());
        done += block;
    }
    REQUIRE(done == kFrames);
    REQUIRE(writer.framesWritten() == kFrames);

    // Until finalize() only the partial file exists.
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE(writer.finalize().ok());
    REQUIRE_FALSE(writer.isOpen());
    REQUIRE(std::filesystem::exists(path));
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(path.string() + ".partial")));

    const osvtest::WavFile w = osvtest::readWav(path);
    INFO(w.why);
    REQUIRE(w.ok);
    CHECK(w.format == 3);  // WAVE_FORMAT_IEEE_FLOAT
    CHECK(w.channels == 2);
    CHECK(w.sampleRate == 48000);
    CHECK(w.byteRate == 48000 * 8);
    CHECK(w.blockAlign == 8);
    CHECK(w.bits == 32);
    CHECK(w.factFrames == kFrames);
    CHECK(w.dataBytes == kFrames * 8);
    CHECK(w.riffSize == 50 + kFrames * 8);
    REQUIRE(w.bytes.size() == 58 + kFrames * 8);

    // Bit-exact samples, not approximately equal ones.
    bool identical = true;
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const float got = osvtest::wavSample(w, i);
        identical = identical && std::memcmp(&got, &signal[i], sizeof(float)) == 0;
    }
    CHECK(identical);
}

TEST_CASE("WavWriter never leaves a half file and replaces an existing one", "[io][wav]") {
    const auto path = osvtest::tempDir() / "wav_atomic.wav";
    const auto partial = std::filesystem::path(path.string() + ".partial");
    std::filesystem::remove(path);
    const std::vector<float> one = syntheticSignal(4, 1);

    {
        // An abandoned writer (no finalize) removes its partial file and makes no final one.
        auto opened = osv::io::WavWriter::open(path, 44100, 1);
        REQUIRE(opened.ok());
        REQUIRE(opened.value().write(one.data(), 4).ok());
        REQUIRE(std::filesystem::exists(partial));
    }
    CHECK_FALSE(std::filesystem::exists(partial));
    CHECK_FALSE(std::filesystem::exists(path));

    // Finalising over an existing file replaces it.
    for (int pass = 0; pass < 2; ++pass) {
        auto opened = osv::io::WavWriter::open(path, 44100, 1);
        REQUIRE(opened.ok());
        REQUIRE(opened.value().write(one.data(), 4).ok());
        REQUIRE(opened.value().finalize().ok());
    }
    const osvtest::WavFile w = osvtest::readWav(path);
    REQUIRE(w.ok);
    CHECK(w.factFrames == 4);
}

TEST_CASE("WavWriter refuses bad arguments", "[io][wav]") {
    const auto path = osvtest::tempDir() / "wav_bad.wav";
    CHECK_FALSE(osv::io::WavWriter::open({}, 48000, 2).ok());
    CHECK_FALSE(osv::io::WavWriter::open(path, 0, 2).ok());
    CHECK_FALSE(osv::io::WavWriter::open(path, 48000, 0).ok());
    CHECK_FALSE(osv::io::WavWriter::open(path, 48000, 20000).ok());       // block align > 16 bits
    CHECK_FALSE(osv::io::WavWriter::open(path, 0xFFFFFFFFu, 2).ok());     // byte rate > 32 bits

    auto opened = osv::io::WavWriter::open(path, 48000, 2);
    REQUIRE(opened.ok());
    CHECK_FALSE(opened.value().write(nullptr, 10).ok());
    CHECK(opened.value().write(nullptr, 0).ok());  // nothing to write is fine
    osv::io::WavWriter closed = std::move(opened).value();
    REQUIRE(closed.finalize().ok());
    CHECK_FALSE(closed.write(nullptr, 0).ok());  // closed
    CHECK_FALSE(closed.finalize().ok());
}

TEST_CASE("WavWriter's 4 GiB guard fires before and during a write", "[io][wav][guard]") {
    const auto path = osvtest::tempDir() / "wav_guard.wav";
    std::filesystem::remove(path);
    const auto partial = std::filesystem::path(path.string() + ".partial");
    std::filesystem::remove(partial);

    // The limit itself: 32-bit RIFF minus the 50 header bytes after its size field.
    CHECK(osv::io::WavWriter::kMaxDataBytes == 0xFFFFFFFFull - 50);
    const std::uint64_t maxStereo = osv::io::WavWriter::maxFrames(2);
    CHECK(maxStereo * 8 <= osv::io::WavWriter::kMaxDataBytes);
    CHECK((maxStereo + 1) * 8 > osv::io::WavWriter::kMaxDataBytes);

    // 48 kHz stereo reaches it after about 3.1 hours, as the contract says.
    CHECK(maxStereo / 48000.0 / 3600.0 > 3.0);
    CHECK(maxStereo / 48000.0 / 3600.0 < 3.2);

    SECTION("a known length that is too long is refused before any file exists") {
        auto refused = osv::io::WavWriter::open(path, 48000, 2, maxStereo + 1);
        REQUIRE_FALSE(refused.ok());
        CHECK(refused.error().code == osv::ErrorCode::InvalidArgument);
        CHECK(refused.error().message.find("4 GiB") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(partial));
        CHECK_FALSE(std::filesystem::exists(path));
        // Exactly the limit is accepted (nothing is written, the writer is dropped).
        CHECK(osv::io::WavWriter::open(path, 48000, 2, maxStereo).ok());
    }

    SECTION("an unknown length is refused by write() when the block would cross the limit") {
        auto opened = osv::io::WavWriter::open(path, 48000, 2);
        REQUIRE(opened.ok());
        const float sample[2] = {0.0f, 0.0f};
        // The guard runs before the sample pointer is read, so a huge count is safe to ask for.
        const osv::Status st = opened.value().write(sample, static_cast<std::size_t>(maxStereo + 1));
        REQUIRE_FALSE(st.ok());
        CHECK(st.error().code == osv::ErrorCode::InvalidArgument);
        CHECK(st.error().message.find("4 GiB") != std::string::npos);
        CHECK(opened.value().framesWritten() == 0);
    }
}
