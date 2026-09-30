// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/io/WavWriter.h"

#include "osv/core/Log.h"

#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

namespace osv::io {

namespace {

// The sample bytes are written straight from memory, so the host must store
// floats little-endian like the format does (every supported target does).
static_assert(std::endian::native == std::endian::little, "WavWriter writes float samples as they lie in memory");
static_assert(sizeof(float) == 4, "WAVE_FORMAT_IEEE_FLOAT 32-bit needs a 4-byte float");

/// Bytes of the RIFF header that come after the 8-byte "RIFF <size>" prefix.
constexpr std::uint64_t kRiffOverhead = WavWriter::kHeaderBytes - 8;

/// Offsets of the size fields the finalisation step patches.
constexpr std::streamoff kRiffSizeOffset = 4;
constexpr std::streamoff kFactFramesOffset = 46;
constexpr std::streamoff kDataSizeOffset = 54;

/// Append `v` to `out` as a little-endian 16-bit value.
void put16(std::array<std::uint8_t, WavWriter::kHeaderBytes>& out, std::size_t& at, std::uint16_t v) {
    out[at++] = static_cast<std::uint8_t>(v & 0xFFu);
    out[at++] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

/// Append `v` to `out` as a little-endian 32-bit value.
void put32(std::array<std::uint8_t, WavWriter::kHeaderBytes>& out, std::size_t& at, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) {
        out[at++] = static_cast<std::uint8_t>((v >> shift) & 0xFFu);
    }
}

/// Append the four ASCII characters of `tag`.
void putTag(std::array<std::uint8_t, WavWriter::kHeaderBytes>& out, std::size_t& at, const char (&tag)[5]) {
    for (int i = 0; i < 4; ++i) {
        out[at++] = static_cast<std::uint8_t>(tag[i]);
    }
}

/// Little-endian bytes of `v`, for patching a size field in place.
std::array<char, 4> le32(std::uint32_t v) {
    return {static_cast<char>(v & 0xFFu), static_cast<char>((v >> 8) & 0xFFu), static_cast<char>((v >> 16) & 0xFFu),
            static_cast<char>((v >> 24) & 0xFFu)};
}

}  // namespace

struct WavWriter::Impl {
    std::filesystem::path finalPath;
    std::filesystem::path partialPath;
    std::ofstream out;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint64_t frames = 0;
    bool open = false;

    /// Close the stream and delete the partial file (an abandoned run).
    void abandon() noexcept {
        open = false;
        if (out.is_open()) {
            out.close();
        }
        std::error_code ec;
        std::filesystem::remove(partialPath, ec);
    }
};

WavWriter::WavWriter() = default;

WavWriter::~WavWriter() {
    // A writer that was never finalised leaves nothing behind.
    if (m_impl && m_impl->open) {
        m_impl->abandon();
    }
}

WavWriter::WavWriter(WavWriter&&) noexcept = default;

WavWriter& WavWriter::operator=(WavWriter&& other) noexcept {
    if (this != &other) {
        if (m_impl && m_impl->open) {
            m_impl->abandon();
        }
        m_impl = std::move(other.m_impl);
    }
    return *this;
}

std::uint64_t WavWriter::maxFrames(std::uint32_t channels) noexcept {
    if (channels == 0) {
        return 0;
    }
    // Whole sample frames only, so the data chunk stays a multiple of the
    // block align.
    return kMaxDataBytes / (static_cast<std::uint64_t>(channels) * sizeof(float));
}

Result<WavWriter> WavWriter::open(const std::filesystem::path& path, std::uint32_t sampleRate, std::uint32_t channels,
                                  std::uint64_t expectedFrames) {
    // ---- validate everything before touching the disk ----------------------------
    if (path.empty()) {
        return Error{ErrorCode::InvalidArgument, "WAV output path is empty"};
    }
    if (sampleRate == 0 || channels == 0) {
        return Error{ErrorCode::InvalidArgument, "WAV needs a non-zero sample rate and channel count"};
    }
    const std::uint64_t blockAlign = static_cast<std::uint64_t>(channels) * sizeof(float);
    if (blockAlign > std::numeric_limits<std::uint16_t>::max()) {
        return Error{ErrorCode::InvalidArgument, "too many channels for a WAV file: " + std::to_string(channels)};
    }
    if (static_cast<std::uint64_t>(sampleRate) * blockAlign > std::numeric_limits<std::uint32_t>::max()) {
        return Error{ErrorCode::InvalidArgument, "WAV byte rate does not fit 32 bits"};
    }
    // The 4 GiB guard: refuse a known-too-long clip up front, with the numbers.
    const std::uint64_t limit = maxFrames(channels);
    if (expectedFrames > limit) {
        return Error{ErrorCode::InvalidArgument,
                     "audio is too long for a WAV file: " + std::to_string(expectedFrames) + " sample frames of " +
                         std::to_string(channels) + " channels of 32-bit float exceed the 4 GiB limit of " +
                         std::to_string(limit) + " frames (" + std::to_string(kMaxDataBytes) + " bytes of data)"};
    }

    // ---- create the partial file ------------------------------------------------------
    auto impl = std::make_unique<Impl>();
    impl->finalPath = path;
    impl->partialPath = path;
    impl->partialPath += ".partial";
    impl->sampleRate = sampleRate;
    impl->channels = channels;
    impl->out.open(impl->partialPath, std::ios::binary | std::ios::trunc);
    if (!impl->out.is_open()) {
        return Error{ErrorCode::Io, "cannot create " + log::safe(impl->partialPath.string())};
    }
    impl->open = true;

    // ---- header, sizes zero until finalize() ------------------------------------------
    std::array<std::uint8_t, kHeaderBytes> header{};
    std::size_t at = 0;
    putTag(header, at, "RIFF");
    put32(header, at, 0);  // RIFF size, patched
    putTag(header, at, "WAVE");
    putTag(header, at, "fmt ");
    put32(header, at, 18);                                          // fmt chunk size with cbSize
    put16(header, at, 3);                                           // WAVE_FORMAT_IEEE_FLOAT
    put16(header, at, static_cast<std::uint16_t>(channels));        // channels
    put32(header, at, sampleRate);                                  // sample rate
    put32(header, at, static_cast<std::uint32_t>(sampleRate * blockAlign));  // byte rate
    put16(header, at, static_cast<std::uint16_t>(blockAlign));      // block align
    put16(header, at, 32);                                          // bits per sample
    put16(header, at, 0);                                           // cbSize
    putTag(header, at, "fact");
    put32(header, at, 4);
    put32(header, at, 0);  // sample frames per channel, patched
    putTag(header, at, "data");
    put32(header, at, 0);  // data size, patched
    if (at != kHeaderBytes) {
        impl->abandon();
        return Error{ErrorCode::Internal, "WAV header layout is inconsistent"};
    }
    impl->out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!impl->out.good()) {
        const std::string what = log::safe(impl->partialPath.string());
        impl->abandon();
        return Error{ErrorCode::Io, "cannot write the WAV header to " + what};
    }

    WavWriter writer;
    writer.m_impl = std::move(impl);
    return writer;
}

bool WavWriter::isOpen() const noexcept { return m_impl && m_impl->open; }

std::uint32_t WavWriter::sampleRate() const noexcept { return m_impl ? m_impl->sampleRate : 0u; }

std::uint32_t WavWriter::channels() const noexcept { return m_impl ? m_impl->channels : 0u; }

std::uint64_t WavWriter::framesWritten() const noexcept { return m_impl ? m_impl->frames : 0u; }

Status WavWriter::write(const float* interleaved, std::size_t frames) {
    if (!isOpen()) {
        return failStatus(ErrorCode::InvalidArgument, "write to a closed WAV writer");
    }
    if (frames == 0) {
        return okStatus();
    }
    if (!interleaved) {
        return failStatus(ErrorCode::InvalidArgument, "null sample block");
    }
    // The 4 GiB guard for a writer whose length was not known at open().
    const std::uint64_t limit = maxFrames(m_impl->channels);
    if (frames > limit || m_impl->frames > limit - frames) {
        return failStatus(ErrorCode::InvalidArgument,
                          "audio is too long for a WAV file: more than " + std::to_string(limit) +
                              " sample frames (4 GiB of data)");
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(frames) * m_impl->channels * sizeof(float);
    if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return failStatus(ErrorCode::InvalidArgument, "sample block is too large");
    }
    m_impl->out.write(reinterpret_cast<const char*>(interleaved), static_cast<std::streamsize>(bytes));
    if (!m_impl->out.good()) {
        return failStatus(ErrorCode::Io, "write failed: " + log::safe(m_impl->partialPath.string()));
    }
    m_impl->frames += frames;
    return okStatus();
}

Status WavWriter::finalize() {
    if (!isOpen()) {
        return failStatus(ErrorCode::InvalidArgument, "finalize on a closed WAV writer");
    }
    Impl& s = *m_impl;
    const std::uint64_t dataBytes = s.frames * s.channels * sizeof(float);
    // Belt and braces: write() already enforces this.
    if (dataBytes > kMaxDataBytes) {
        s.abandon();
        return failStatus(ErrorCode::InvalidArgument, "audio is too long for a WAV file (4 GiB limit)");
    }

    // Patch the three size fields, then flush and close before the rename so
    // Windows does not refuse to move an open file.
    auto patch = [&s](std::streamoff offset, std::uint32_t value) {
        s.out.seekp(offset, std::ios::beg);
        const auto bytes = le32(value);
        s.out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    patch(kRiffSizeOffset, static_cast<std::uint32_t>(dataBytes + kRiffOverhead));
    patch(kFactFramesOffset, static_cast<std::uint32_t>(s.frames));
    patch(kDataSizeOffset, static_cast<std::uint32_t>(dataBytes));
    s.out.flush();
    const bool good = s.out.good();
    s.out.close();
    if (!good) {
        const std::string what = log::safe(s.partialPath.string());
        s.abandon();
        return failStatus(ErrorCode::Io, "cannot finish " + what);
    }

    // Publish under the real name; an existing file is replaced.
    std::error_code ec;
    std::filesystem::rename(s.partialPath, s.finalPath, ec);
    s.open = false;
    if (ec) {
        const std::string what = log::safe(s.finalPath.string()) + ": " + log::safe(ec.message());
        std::filesystem::remove(s.partialPath, ec);
        return failStatus(ErrorCode::Io, "cannot move the finished WAV to " + what);
    }
    return okStatus();
}

}  // namespace osv::io
