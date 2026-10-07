// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// test_frame_row_check.cpp - the importer's self-check of a delivered frame
// (plugins/common/FrameRowCheck.h).
//
// The check exists to name, in a user's log, a band of rows that came out
// transparent or black - the shape of a field report whose bottom three
// eighths were black.  These tests build such frames by hand in every host
// layout the importer delivers (BGRA 32f / 16u / 8u, bottom-left origin,
// either sign of row pitch), through PixelCopy's own conversion so the bytes
// are exactly what the importer would write, and prove that:
//
//   * a clean frame passes with no bad row at all;
//   * a black band, a transparent band and an all-zero band (an unwritten,
//     zero-filled PPix) are each found, with the right PICTURE rows (row 0 =
//     top), the right kind and the right share of the height;
//   * frames that cannot be read safely are refused, never read.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "FrameRowCheck.h"
#include "PixelCopy.h"

#include "osv/render/ImageRGBAf.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace pc = osv::premiere::pixelcopy;
namespace rc = osv::premiere::rowcheck;

namespace {

/// A band of picture rows [first, last] and what is written into it.
struct Band {
    enum class Kind { Black, Transparent, Zero };
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    Kind kind = Kind::Black;
};

/// A textured, fully opaque picture (no channel anywhere near zero, so no
/// row can pass for black by accident in any layout, 8-bit included) with
/// the given bands painted in.
///
///   Black        B, G, R = 0, alpha 1 - opaque black pixels;
///   Transparent  colour kept, alpha 0 - what a straight-alpha host shows
///                as black over a black background;
///   Zero         every channel 0 - a PPix the renderer never wrote.
osv::render::ImageRGBAf makePicture(std::uint32_t w, std::uint32_t h, const std::vector<Band>& bands) {
    auto created = osv::render::ImageRGBAf::create(w, h);
    REQUIRE(created.ok());
    osv::render::ImageRGBAf img = std::move(created).value();
    for (std::uint32_t y = 0; y < h; ++y) {
        float* row = img.row(y);
        REQUIRE(row != nullptr);
        for (std::uint32_t x = 0; x < w; ++x) {
            // Every channel within [0.1, 0.9]: varied, never black, never clipped.
            row[x * 4 + 0] = 0.1f + 0.8f * static_cast<float>(x % 97) / 97.0f;
            row[x * 4 + 1] = 0.1f + 0.8f * static_cast<float>(y % 89) / 89.0f;
            row[x * 4 + 2] = 0.1f + 0.8f * static_cast<float>((x + y) % 83) / 83.0f;
            row[x * 4 + 3] = 1.0f;
        }
    }
    // ---- paint the bands ---------------------------------------------------
    for (const Band& band : bands) {
        for (std::uint32_t y = band.first; y <= band.last && y < h; ++y) {
            float* row = img.row(y);
            for (std::uint32_t x = 0; x < w; ++x) {
                float* p = row + static_cast<std::size_t>(x) * 4u;
                if (band.kind == Band::Kind::Transparent) {
                    p[3] = 0.0f;
                } else {
                    p[0] = 0.0f;
                    p[1] = 0.0f;
                    p[2] = 0.0f;
                    p[3] = band.kind == Band::Kind::Zero ? 0.0f : 1.0f;
                }
            }
        }
    }
    return img;
}

/// A host buffer with a chosen pitch sign (negative: base points at the
/// last row in memory) and some row padding, filled with a pattern that is
/// neither black nor transparent so a row the copy missed cannot pass.
struct HostBuffer {
    std::vector<std::uint8_t> storage;
    char* base = nullptr;
    std::int32_t rowBytes = 0;

    HostBuffer(std::uint32_t width, std::uint32_t height, std::size_t bpp, bool negative) {
        const std::size_t pitch = (static_cast<std::size_t>(width) * bpp + 63u) & ~static_cast<std::size_t>(63u);
        storage.assign(pitch * height + 64u, 0x7F);
        char* first = reinterpret_cast<char*>(storage.data());
        rowBytes = negative ? -static_cast<std::int32_t>(pitch) : static_cast<std::int32_t>(pitch);
        base = negative ? first + pitch * (height - 1u) : first;
    }
};

/// The picture written into a new host buffer in `format` (bottom-left, as
/// the importer writes a PPix) and scanned.
rc::RowScan scanPicture(const osv::render::ImageRGBAf& img, pc::HostPixelFormat format, bool negativePitch) {
    HostBuffer buffer(img.w, img.h, pc::bytesPerPixel(format), negativePitch);
    const pc::HostFrame frame{buffer.base, buffer.rowBytes, img.w, img.h};
    REQUIRE(pc::rgbaToHost(img, frame, format, nullptr).ok());
    return rc::scanHostFrame(pc::ConstHostFrame(frame), format);
}

/// Name of a layout for INFO().
const char* layoutName(pc::HostPixelFormat format) { return pc::hostPixelFormatName(format); }

}  // namespace

TEST_CASE("the row self-check passes a clean frame in every host layout", "[common][rowcheck]") {
    const pc::HostPixelFormat format =
        GENERATE(pc::HostPixelFormat::Bgra32f, pc::HostPixelFormat::Bgra16u, pc::HostPixelFormat::Bgra8u);
    const bool negative = GENERATE(false, true);
    INFO(layoutName(format) << (negative ? ", negative pitch" : ", positive pitch"));

    const rc::RowScan scan = scanPicture(makePicture(512, 256, {}), format, negative);
    REQUIRE(scan.scanned);
    CHECK(scan.width == 512u);
    CHECK(scan.height == 256u);
    CHECK(scan.columns == rc::kSampleColumns);
    CHECK_FALSE(scan.defective());
    CHECK(scan.badRows == 0u);
    CHECK(scan.transparentRows == 0u);
    CHECK(scan.blackRows == 0u);
    CHECK(scan.runLength == 0u);
    CHECK(scan.worstRowOpaque == rc::kSampleColumns);
    CHECK(scan.worstRowOpaqueFraction() == 1.0);
    CHECK(scan.nonFiniteSamples == 0u);
    CHECK(std::string(rc::runKindName(scan)) == "clean");
    CHECK(rc::describeRun(scan).empty());
}

TEST_CASE("the row self-check names a black bottom band in picture rows (the field report's 37.5 %)",
          "[common][rowcheck]") {
    const pc::HostPixelFormat format =
        GENERATE(pc::HostPixelFormat::Bgra32f, pc::HostPixelFormat::Bgra16u, pc::HostPixelFormat::Bgra8u);
    const bool negative = GENERATE(false, true);
    INFO(layoutName(format) << (negative ? ", negative pitch" : ", positive pitch"));

    // The reported shape: 2000 x 1000 with rows 625..999 black, opaque.
    // The host frame stores the bottom scanline first, so these are host
    // rows 0..374 - the scan must still report them as 625..999.
    const rc::RowScan scan =
        scanPicture(makePicture(2000, 1000, {{625u, 999u, Band::Kind::Black}}), format, negative);
    REQUIRE(scan.scanned);
    REQUIRE(scan.defective());
    CHECK(scan.runFirst == 625u);
    CHECK(scan.runLast == 999u);
    CHECK(scan.runLength == 375u);
    CHECK(scan.blackRows == 375u);
    CHECK(scan.transparentRows == 0u);
    CHECK(scan.badRows == 375u);
    CHECK(scan.runBlackRows == 375u);
    CHECK(scan.runTransparentRows == 0u);
    // Black but opaque: the alpha side of the check sees nothing wrong.
    CHECK(scan.worstRowOpaque == rc::kSampleColumns);
    CHECK(std::string(rc::runKindName(scan)) == "black");
    CHECK(rc::describeRun(scan) == "rows 625..999 (37.5% of 1000) came out black");
}

TEST_CASE("the row self-check names a transparent band, and an all-zero (unwritten) one", "[common][rowcheck]") {
    const pc::HostPixelFormat format =
        GENERATE(pc::HostPixelFormat::Bgra32f, pc::HostPixelFormat::Bgra16u, pc::HostPixelFormat::Bgra8u);
    const bool negative = GENERATE(false, true);
    INFO(layoutName(format) << (negative ? ", negative pitch" : ", positive pitch"));

    SECTION("transparent: the colour is there, the coverage alpha is not") {
        const rc::RowScan scan =
            scanPicture(makePicture(512, 256, {{100u, 159u, Band::Kind::Transparent}}), format, negative);
        REQUIRE(scan.defective());
        CHECK(scan.runFirst == 100u);
        CHECK(scan.runLast == 159u);
        CHECK(scan.runLength == 60u);
        CHECK(scan.transparentRows == 60u);
        CHECK(scan.blackRows == 0u);
        CHECK(scan.worstRowOpaque == 0u);
        CHECK(scan.worstRow == 100u);
        CHECK(std::string(rc::runKindName(scan)) == "transparent");
        CHECK(rc::describeRun(scan) == "rows 100..159 (23.4% of 256) came out transparent");
    }
    SECTION("all zero: a bottom band of a PPix nothing was written into") {
        const rc::RowScan scan =
            scanPicture(makePicture(512, 256, {{160u, 255u, Band::Kind::Zero}}), format, negative);
        REQUIRE(scan.defective());
        CHECK(scan.runFirst == 160u);
        CHECK(scan.runLast == 255u);
        CHECK(scan.runLength == 96u);
        CHECK(scan.transparentRows == 96u);
        CHECK(scan.blackRows == 96u);
        CHECK(scan.badRows == 96u);
        CHECK(std::string(rc::runKindName(scan)) == "transparent and black");
        CHECK(rc::describeRun(scan) == "rows 160..255 (37.5% of 256) came out transparent and black");
    }
    SECTION("two bands: the longer run is reported, both are counted") {
        const rc::RowScan scan = scanPicture(
            makePicture(512, 256, {{10u, 19u, Band::Kind::Transparent}, {200u, 239u, Band::Kind::Black}}), format,
            negative);
        REQUIRE(scan.defective());
        CHECK(scan.runFirst == 200u);
        CHECK(scan.runLast == 239u);
        CHECK(scan.badRows == 50u);
        CHECK(scan.transparentRows == 10u);
        CHECK(scan.blackRows == 40u);
        CHECK(std::string(rc::runKindName(scan)) == "black");
    }
    SECTION("adjacent bands of both kinds make one run of both kinds") {
        const rc::RowScan scan = scanPicture(
            makePicture(512, 256, {{50u, 69u, Band::Kind::Transparent}, {70u, 99u, Band::Kind::Black}}), format,
            negative);
        REQUIRE(scan.defective());
        CHECK(scan.runFirst == 50u);
        CHECK(scan.runLast == 99u);
        CHECK(scan.runTransparentRows == 20u);
        CHECK(scan.runBlackRows == 30u);
        CHECK(std::string(rc::runKindName(scan)) == "transparent and black");
    }
    SECTION("a single empty top row is found too") {
        const rc::RowScan scan = scanPicture(makePicture(512, 256, {{0u, 0u, Band::Kind::Zero}}), format, negative);
        REQUIRE(scan.defective());
        CHECK(scan.runFirst == 0u);
        CHECK(scan.runLast == 0u);
        CHECK(scan.runLength == 1u);
    }
}

TEST_CASE("the row self-check: ties, partial rows, narrow frames and non-finite samples", "[common][rowcheck]") {
    SECTION("two equal runs: the first one is reported") {
        const rc::RowScan scan = scanPicture(
            makePicture(256, 128, {{10u, 19u, Band::Kind::Black}, {100u, 109u, Band::Kind::Black}}),
            pc::HostPixelFormat::Bgra8u, false);
        REQUIRE(scan.defective());
        CHECK(scan.runFirst == 10u);
        CHECK(scan.runLast == 19u);
        CHECK(scan.badRows == 20u);
    }
    SECTION("a row transparent over its left half is not a transparent row, but is the worst row") {
        osv::render::ImageRGBAf img = makePicture(512, 256, {});
        float* row = img.row(77);
        for (std::uint32_t x = 0; x < 256; ++x) {
            row[x * 4 + 3] = 0.0f;
        }
        const rc::RowScan scan = scanPicture(img, pc::HostPixelFormat::Bgra32f, false);
        CHECK_FALSE(scan.defective());
        CHECK(scan.worstRow == 77u);
        CHECK(scan.worstRowOpaque == rc::kSampleColumns / 2u);
        CHECK(scan.worstRowOpaqueFraction() == 0.5);
    }
    SECTION("a frame narrower than the sample count samples every column once") {
        const rc::RowScan clean = scanPicture(makePicture(10, 6, {}), pc::HostPixelFormat::Bgra16u, true);
        REQUIRE(clean.scanned);
        CHECK(clean.columns == 10u);
        CHECK_FALSE(clean.defective());
        const rc::RowScan banded =
            scanPicture(makePicture(10, 6, {{4u, 5u, Band::Kind::Zero}}), pc::HostPixelFormat::Bgra16u, true);
        CHECK(banded.runFirst == 4u);
        CHECK(banded.runLast == 5u);
    }
    SECTION("a NaN alpha counts as transparent and is counted as non-finite") {
        // Written straight into a 32f host buffer: rgbaToHost would carry the
        // NaN through as well, but the row has to be exact.
        HostBuffer buffer(64, 8, pc::kBytesPerPixel32f, false);
        for (std::uint32_t r = 0; r < 8; ++r) {
            float* p = reinterpret_cast<float*>(pc::rowAddress(buffer.base, buffer.rowBytes, r));
            for (std::uint32_t x = 0; x < 64; ++x) {
                p[x * 4 + 0] = 0.25f;
                p[x * 4 + 1] = 0.5f;
                p[x * 4 + 2] = 0.75f;
                // Host row 0 is the bottom scanline: picture row 7.
                p[x * 4 + 3] = r == 0 ? std::numeric_limits<float>::quiet_NaN() : 1.0f;
            }
        }
        const rc::RowScan scan =
            rc::scanHostFrame(pc::ConstHostFrame(buffer.base, buffer.rowBytes, 64, 8), pc::HostPixelFormat::Bgra32f);
        REQUIRE(scan.defective());
        CHECK(scan.runFirst == 7u);
        CHECK(scan.runLast == 7u);
        CHECK(scan.transparentRows == 1u);
        CHECK(scan.blackRows == 0u);
        CHECK(scan.nonFiniteSamples == 64u);
    }
}

TEST_CASE("the row self-check refuses a frame it cannot read safely", "[common][rowcheck]") {
    std::vector<std::uint8_t> storage(64u * 16u * 8u, 0);
    char* base = reinterpret_cast<char*>(storage.data());
    const auto refused = [](const rc::RowScan& scan) {
        CHECK_FALSE(scan.scanned);
        CHECK_FALSE(scan.defective());
        CHECK(scan.runLength == 0u);
        CHECK(std::string(rc::runKindName(scan)) == "unscanned");
        CHECK(rc::describeRun(scan).empty());
        CHECK(scan.worstRowOpaqueFraction() == 0.0);
        CHECK(scan.runPercentOfHeight() == 0.0);
    };
    // Null pixels, a zero pitch, a pitch narrower than a row, no size, an
    // absurd size, and a layout outside the enum.
    refused(rc::scanHostFrame(pc::ConstHostFrame(nullptr, 64 * 16, 64, 8), pc::HostPixelFormat::Bgra32f));
    refused(rc::scanHostFrame(pc::ConstHostFrame(base, 0, 64, 8), pc::HostPixelFormat::Bgra32f));
    refused(rc::scanHostFrame(pc::ConstHostFrame(base, 64 * 16 - 1, 64, 8), pc::HostPixelFormat::Bgra32f));
    refused(rc::scanHostFrame(pc::ConstHostFrame(base, 64 * 16, 0, 8), pc::HostPixelFormat::Bgra32f));
    refused(rc::scanHostFrame(pc::ConstHostFrame(base, 64 * 16, 64, 0), pc::HostPixelFormat::Bgra32f));
    refused(rc::scanHostFrame(pc::ConstHostFrame(base, 64 * 16, 64, 40000), pc::HostPixelFormat::Bgra32f));
    refused(rc::scanHostFrame(pc::ConstHostFrame(base, 64 * 16, 64, 8), static_cast<pc::HostPixelFormat>(7)));
    // The same buffer is fine as 8-bit, whose rows are a quarter as wide.
    const rc::RowScan eightBit = rc::scanHostFrame(pc::ConstHostFrame(base, 64 * 4, 64, 8), pc::HostPixelFormat::Bgra8u);
    CHECK(eightBit.scanned);
    // ...and, all zero, every row of it is empty.
    CHECK(eightBit.runLength == 8u);
    CHECK(std::string(rc::runKindName(eightBit)) == "transparent and black");
}
