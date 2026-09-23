// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_pixelcopy_bands.cpp - PixelCopy's 16-bit integer layout and its
// banded entry points, the pieces the importer's high-bit-depth GPU frame
// path is built from:
//
//   * PrPixelFormat_BGRA_4444_16u runs from 0 to 32768 (NOT 65535) - a frame
//     scaled to the wrong white would read as nearly two stops too bright;
//   * a frame streamed band by band (rgbaRowsToHost, packedRowsToHost) must
//     be byte-identical to the same frame written in one go, whatever the
//     band boundaries and the sign of the host's row pitch.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "PixelCopy.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/ImageRGBAf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace pc = osv::premiere::pixelcopy;

namespace {

/// A deterministic image whose channels are different functions of the
/// coordinates (a swap or a flip cannot hide), with values outside [0, 1]
/// and a NaN so the clamping paths run too.
osv::render::ImageRGBAf makeImage(std::uint32_t w, std::uint32_t h) {
    auto created = osv::render::ImageRGBAf::create(w, h);
    REQUIRE(created.ok());
    osv::render::ImageRGBAf img = std::move(created).value();
    for (std::uint32_t y = 0; y < h; ++y) {
        float* row = img.row(y);
        REQUIRE(row != nullptr);
        for (std::uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = static_cast<float>(x) / static_cast<float>(w);
            row[x * 4 + 1] = static_cast<float>(y) / static_cast<float>(h);
            row[x * 4 + 2] = static_cast<float>((x * 7 + y * 3) % 23) / 11.0f - 0.5f;  // spans -0.5 .. 1.5
            row[x * 4 + 3] = 1.0f - static_cast<float>(y) / (2.0f * static_cast<float>(h));
        }
    }
    img.row(0)[2] = std::numeric_limits<float>::quiet_NaN();
    return img;
}

/// A host buffer with a chosen row pitch sign (negative: base points at the
/// last row in memory, as Premiere may hand a bottom-up frame).
struct Buffer {
    std::vector<std::uint8_t> storage;
    char* base = nullptr;
    std::int32_t rowBytes = 0;

    Buffer(std::uint32_t width, std::uint32_t height, std::size_t bpp, bool negative) {
        const std::size_t pitch = (static_cast<std::size_t>(width) * bpp + 63u) & ~static_cast<std::size_t>(63u);
        storage.assign(pitch * height + 64u, 0xCD);
        char* first = reinterpret_cast<char*>(storage.data());
        rowBytes = negative ? -static_cast<std::int32_t>(pitch) : static_cast<std::int32_t>(pitch);
        base = negative ? first + pitch * (height - 1u) : first;
    }
    [[nodiscard]] pc::HostFrame frame(std::uint32_t w, std::uint32_t h) const {
        return pc::HostFrame{base, rowBytes, w, h};
    }
};

/// The visible bytes of two frames are equal (padding ignored).
[[nodiscard]] bool sameRows(const Buffer& a, const Buffer& b, std::uint32_t w, std::uint32_t h, std::size_t bpp) {
    for (std::uint32_t r = 0; r < h; ++r) {
        if (std::memcmp(pc::rowAddress(a.base, a.rowBytes, r), pc::rowAddress(b.base, b.rowBytes, r),
                        static_cast<std::size_t>(w) * bpp) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("PixelCopy 16u uses Premiere's 0..32768 scale, rounds to nearest and clamps", "[common][pixelcopy][16u]") {
    REQUIRE(pc::kWhite16u == 32768);
    REQUIRE(pc::floatTo16u(0.0f) == 0);
    REQUIRE(pc::floatTo16u(1.0f) == 32768);
    REQUIRE(pc::floatTo16u(0.5f) == 16384);
    REQUIRE(pc::floatTo16u(-0.25f) == 0);
    REQUIRE(pc::floatTo16u(7.0f) == 32768);  // no super-white in this format
    REQUIRE(pc::floatTo16u(std::numeric_limits<float>::quiet_NaN()) == 0);
    REQUIRE(pc::floatTo16u(std::numeric_limits<float>::infinity()) == 32768);

    // Round to nearest around the first code boundary.
    REQUIRE(pc::floatTo16u(0.4f / 32768.0f) == 0);
    REQUIRE(pc::floatTo16u(0.6f / 32768.0f) == 1);

    // Every code survives a round trip, and never lands past white.
    for (int code = 0; code <= 32768; ++code) {
        const float f = pc::u16ToFloat(static_cast<std::uint16_t>(code));
        REQUIRE(pc::floatTo16u(f) == static_cast<std::uint16_t>(code));
    }
    REQUIRE(pc::u16ToFloat(32768) == 1.0f);
}

TEST_CASE("PixelCopy round trips BGRA 16u within half a code", "[common][pixelcopy][16u]") {
    osv::ThreadPool pool(3);
    const std::uint32_t w = 53;
    const std::uint32_t h = 17;
    const osv::render::ImageRGBAf source = makeImage(w, h);
    const bool negative = GENERATE(false, true);

    Buffer buffer(w, h, pc::kBytesPerPixel16u, negative);
    REQUIRE(pc::rgbaToHostBgra16u(source, buffer.frame(w, h), &pool).ok());

    SECTION("host row 0 is the bottom scanline, in B,G,R,A order") {
        const auto* row0 = reinterpret_cast<const std::uint16_t*>(pc::rowAddress(buffer.base, buffer.rowBytes, 0));
        const float* bottom = source.row(h - 1);
        REQUIRE(row0[0] == pc::floatTo16u(bottom[2]));
        REQUIRE(row0[1] == pc::floatTo16u(bottom[1]));
        REQUIRE(row0[2] == pc::floatTo16u(bottom[0]));
        REQUIRE(row0[3] == pc::floatTo16u(bottom[3]));
    }

    SECTION("reading it back is the clamped source within half a code") {
        auto readBack = pc::hostBgra16uToRgba(pc::ConstHostFrame(buffer.base, buffer.rowBytes, w, h), &pool);
        REQUIRE(readBack.ok());
        float worst = 0.0f;
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t i = 0; i < w * 4u; ++i) {
                float expected = source.row(y)[i];
                expected = std::isnan(expected) ? 0.0f : std::fmin(std::fmax(expected, 0.0f), 1.0f);
                worst = std::fmax(worst, std::fabs(readBack.value().row(y)[i] - expected));
            }
        }
        REQUIRE(worst <= 0.5f / 32768.0f);
    }

    SECTION("the generic entry point writes the same bytes") {
        Buffer generic(w, h, pc::kBytesPerPixel16u, negative);
        REQUIRE(pc::rgbaToHost(source, generic.frame(w, h), pc::HostPixelFormat::Bgra16u, nullptr).ok());
        REQUIRE(sameRows(buffer, generic, w, h, pc::kBytesPerPixel16u));
    }
}

TEST_CASE("PixelCopy band by band equals the whole frame in one go", "[common][pixelcopy][bands]") {
    osv::ThreadPool pool(4);
    const std::uint32_t w = 61;
    const std::uint32_t h = 29;
    const osv::render::ImageRGBAf source = makeImage(w, h);
    const bool negative = GENERATE(false, true);
    const pc::HostPixelFormat format =
        GENERATE(pc::HostPixelFormat::Bgra32f, pc::HostPixelFormat::Bgra16u, pc::HostPixelFormat::Bgra8u);
    // Uneven bands: 1 row, 7 rows, the rest - boundaries the importer's
    // pinned readback never aligns with anything in particular.
    const std::uint32_t bandRows = GENERATE(1u, 7u, 29u);
    const std::size_t bpp = pc::bytesPerPixel(format);
    INFO("layout " << pc::hostPixelFormatName(format) << ", bands of " << bandRows << " rows, "
                   << (negative ? "negative" : "positive") << " row bytes");

    Buffer whole(w, h, bpp, negative);
    REQUIRE(pc::rgbaToHost(source, whole.frame(w, h), format, &pool).ok());

    Buffer banded(w, h, bpp, negative);
    for (std::uint32_t row0 = 0; row0 < h; row0 += bandRows) {
        pc::RgbaRows band;
        band.base = source.row(row0);
        band.pitchBytes = source.pitchBytes();
        band.width = w;
        band.rows = std::min(bandRows, h - row0);
        REQUIRE(pc::rgbaRowsToHost(band, row0, banded.frame(w, h), format, &pool).ok());
    }
    REQUIRE(sameRows(whole, banded, w, h, bpp));

    SECTION("rows already packed in the layout copy to the same place") {
        // Pack the picture top-down, tight rows, as the GPU does before the
        // readback: take the whole-frame result's rows back in picture order.
        const std::size_t tight = static_cast<std::size_t>(w) * bpp;
        std::vector<std::uint8_t> packed(tight * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            std::memcpy(packed.data() + tight * y, pc::rowAddress(whole.base, whole.rowBytes, h - 1u - y), tight);
        }
        Buffer copied(w, h, bpp, negative);
        for (std::uint32_t row0 = 0; row0 < h; row0 += bandRows) {
            pc::PackedRows band;
            band.base = packed.data() + tight * row0;
            band.pitchBytes = tight;
            band.width = w;
            band.rows = std::min(bandRows, h - row0);
            REQUIRE(pc::packedRowsToHost(band, row0, copied.frame(w, h), format, &pool).ok());
        }
        REQUIRE(sameRows(whole, copied, w, h, bpp));
    }
}

TEST_CASE("PixelCopy's band functions refuse what would write out of bounds", "[common][pixelcopy][bands]") {
    const std::uint32_t w = 16;
    const std::uint32_t h = 8;
    const osv::render::ImageRGBAf source = makeImage(w, h);
    Buffer buffer(w, h, pc::kBytesPerPixel32f, false);
    const pc::HostFrame dst = buffer.frame(w, h);

    pc::RgbaRows band;
    band.base = source.row(0);
    band.pitchBytes = source.pitchBytes();
    band.width = w;
    band.rows = 4;

    // A band that runs past the last row.
    REQUIRE_FALSE(pc::rgbaRowsToHost(band, 5, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());
    // A band of another width.
    pc::RgbaRows narrow = band;
    narrow.width = w - 1;
    REQUIRE_FALSE(pc::rgbaRowsToHost(narrow, 0, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());
    // A pitch smaller than a row.
    pc::RgbaRows tight = band;
    tight.pitchBytes = static_cast<std::size_t>(w) * 16u - 4u;
    REQUIRE_FALSE(pc::rgbaRowsToHost(tight, 0, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());
    // Null source, and a layout outside the enum.
    pc::RgbaRows null = band;
    null.base = nullptr;
    REQUIRE_FALSE(pc::rgbaRowsToHost(null, 0, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());
    REQUIRE_FALSE(pc::rgbaRowsToHost(band, 0, dst, static_cast<pc::HostPixelFormat>(9), nullptr).ok());
    // A firstRow so large that firstRow + rows would wrap in 32 bits.
    REQUIRE_FALSE(pc::rgbaRowsToHost(band, 0xFFFFFFFEu, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());

    // The packed variant, the same way.
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(w) * 16u * 4u);
    pc::PackedRows rows;
    rows.base = packed.data();
    rows.pitchBytes = static_cast<std::size_t>(w) * 16u;
    rows.width = w;
    rows.rows = 4;
    REQUIRE(pc::packedRowsToHost(rows, 4, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());
    REQUIRE_FALSE(pc::packedRowsToHost(rows, 5, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());
    REQUIRE_FALSE(pc::packedRowsToHost(rows, 0xFFFFFFFEu, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());
    pc::PackedRows nullRows = rows;
    nullRows.base = nullptr;
    REQUIRE_FALSE(pc::packedRowsToHost(nullRows, 0, dst, pc::HostPixelFormat::Bgra32f, nullptr).ok());

    // Nothing above wrote a byte past the frame: the poison after the last
    // row is intact.
    const std::size_t pitch = static_cast<std::size_t>(buffer.rowBytes);
    for (std::size_t i = pitch * h; i < buffer.storage.size(); ++i) {
        REQUIRE(buffer.storage[i] == 0xCD);
    }
}
