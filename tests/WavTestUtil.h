// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// A tiny, strict reader for the float WAV files WavWriter produces, so the
// tests check the bytes on disk and not the writer's own idea of them.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace osvtest {

/// Everything a WavWriter file says about itself.
struct WavFile {
    bool ok = false;                 ///< Every structural check passed.
    std::string why;                 ///< The first check that failed.
    std::vector<std::uint8_t> bytes; ///< The whole file.
    std::uint32_t riffSize = 0;
    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bits = 0;
    std::uint32_t factFrames = 0;
    std::uint32_t dataBytes = 0;
    std::size_t dataOffset = 0;      ///< Offset of the first sample byte.
};

inline std::uint32_t wavLe32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

inline std::uint16_t wavLe16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

/// Read and validate the fixed 58-byte layout.
inline WavFile readWav(const std::filesystem::path& path) {
    WavFile w;
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        w.why = "cannot open";
        return w;
    }
    w.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const auto& b = w.bytes;
    if (b.size() < 58) {
        w.why = "shorter than the 58-byte header";
        return w;
    }
    auto tagIs = [&b](std::size_t at, const char* tag) { return std::memcmp(&b[at], tag, 4) == 0; };
    if (!tagIs(0, "RIFF") || !tagIs(8, "WAVE") || !tagIs(12, "fmt ") || !tagIs(38, "fact") || !tagIs(50, "data")) {
        w.why = "chunk tags are not RIFF/WAVE/fmt /fact/data at their fixed offsets";
        return w;
    }
    if (wavLe32(b, 16) != 18 || wavLe32(b, 42) != 4) {
        w.why = "fmt or fact chunk size";
        return w;
    }
    w.riffSize = wavLe32(b, 4);
    w.format = wavLe16(b, 20);
    w.channels = wavLe16(b, 22);
    w.sampleRate = wavLe32(b, 24);
    w.byteRate = wavLe32(b, 28);
    w.blockAlign = wavLe16(b, 32);
    w.bits = wavLe16(b, 34);
    if (wavLe16(b, 36) != 0) {
        w.why = "cbSize is not 0";
        return w;
    }
    w.factFrames = wavLe32(b, 46);
    w.dataBytes = wavLe32(b, 54);
    w.dataOffset = 58;
    if (b.size() != w.dataOffset + w.dataBytes) {
        w.why = "file size is not header + data size";
        return w;
    }
    if (w.riffSize != b.size() - 8) {
        w.why = "RIFF size is not file size - 8";
        return w;
    }
    w.ok = true;
    return w;
}

/// Sample `index` (interleaved order) of a validated file.
inline float wavSample(const WavFile& w, std::size_t index) {
    float v = 0.0f;
    std::memcpy(&v, &w.bytes[w.dataOffset + index * sizeof(float)], sizeof(float));
    return v;
}

}  // namespace osvtest
