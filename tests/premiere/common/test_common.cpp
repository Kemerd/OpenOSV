// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests of plugins/common, the layer both plug-ins are built on:
//
//   PrefsBlob   layout, defaults, sanitising of a hostile blob, cache key;
//   PixelCopy   RGBA float <-> BGRA 32f / 16f / 8u with positive AND negative
//               row bytes, bottom-left and top-left origins;
//   PluginLog   file creation, level filtering, once() and rotation;
//   DelayLoad   the hook resolves a DLL sitting beside the test executable;
//   HostSuites  RAII acquire / release against the mock SPBasicSuite;
//   HostContext the process-wide renderer pool.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "MockHost.h"

#include "DelayLoad.h"
#include "HostContext.h"
#include "HostSuites.h"
#include "PixelCopy.h"
#include "PluginLog.h"
#include "PrefsBlob.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/ImageRGBAf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace osv::premiere;
using osv::premiere::mock::MockHost;
namespace pc = osv::premiere::pixelcopy;

namespace {

/// A deterministic test image: every channel is a different function of the
/// pixel coordinates so a channel swap or a row flip cannot go unnoticed.
osv::render::ImageRGBAf makeTestImage(std::uint32_t w, std::uint32_t h) {
    auto created = osv::render::ImageRGBAf::create(w, h);
    REQUIRE(created.ok());
    osv::render::ImageRGBAf img = std::move(created).value();
    for (std::uint32_t y = 0; y < h; ++y) {
        float* row = img.row(y);
        REQUIRE(row != nullptr);
        for (std::uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = static_cast<float>(x) / static_cast<float>(w);            // R rises with x
            row[x * 4 + 1] = static_cast<float>(y) / static_cast<float>(h);            // G rises with y
            row[x * 4 + 2] = static_cast<float>((x + y) % 7) / 7.0f;                   // B is a fine pattern
            row[x * 4 + 3] = 1.0f - static_cast<float>(y) / (2.0f * static_cast<float>(h));  // A falls with y
        }
    }
    return img;
}

/// A host buffer with a chosen sign of row bytes.  When `negative` is true
/// the returned ConstHostFrame/HostFrame base points at the LAST row so
/// stepping by the (negative) pitch walks backwards through memory - the
/// layout Premiere uses for a bottom-up frame delivered top-down.
struct Buffer {
    std::vector<std::uint8_t> storage;
    char* base = nullptr;
    std::int32_t rowBytes = 0;

    Buffer(std::uint32_t width, std::uint32_t height, std::size_t bpp, bool negative, std::size_t align = 64) {
        const std::size_t pitch = ((static_cast<std::size_t>(width) * bpp) + align - 1u) & ~(align - 1u);
        storage.assign(pitch * height + align, 0xCD);  // poison so untouched bytes show up
        const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(storage.data());
        const std::uintptr_t aligned = (raw + align - 1u) & ~static_cast<std::uintptr_t>(align - 1u);
        char* first = reinterpret_cast<char*>(aligned);
        if (negative) {
            rowBytes = -static_cast<std::int32_t>(pitch);
            base = first + pitch * (height - 1u);
        } else {
            rowBytes = static_cast<std::int32_t>(pitch);
            base = first;
        }
    }

    [[nodiscard]] pc::HostFrame frame(std::uint32_t w, std::uint32_t h) { return pc::HostFrame{base, rowBytes, w, h}; }
    [[nodiscard]] pc::ConstHostFrame constFrame(std::uint32_t w, std::uint32_t h) const {
        return pc::ConstHostFrame{base, rowBytes, w, h};
    }
};

/// Largest absolute per-channel difference between two images.
float maxAbsDiff(const osv::render::ImageRGBAf& a, const osv::render::ImageRGBAf& b) {
    REQUIRE(a.w == b.w);
    REQUIRE(a.h == b.h);
    float worst = 0.0f;
    for (std::size_t i = 0; i < a.data.size(); ++i) {
        worst = std::max(worst, std::fabs(a.data[i] - b.data[i]));
    }
    return worst;
}

/// Directory of the running test executable, with a trailing backslash.
std::wstring executableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    REQUIRE(n > 0);
    path.resize(n);
    const std::size_t slash = path.find_last_of(L"\\/");
    REQUIRE(slash != std::wstring::npos);
    return path.substr(0, slash + 1);
}

}  // namespace

// =============================================================================
//  PrefsBlob
// =============================================================================
TEST_CASE("PrefsBlob layout is fixed at 128 bytes", "[common][prefs]") {
    // These are compile-time guarantees; asserting them again here documents
    // them for a reader and makes a layout change a test failure too.
    static_assert(sizeof(PrefsBlob) == 128, "PrefsBlob must be exactly 128 bytes");
    static_assert(PrefsBlob::kSize == 128, "PrefsBlob::kSize must match sizeof");
    static_assert(offsetof(PrefsBlob, magic) == 0, "magic must be first");
    static_assert(offsetof(PrefsBlob, version) == 4, "version follows magic");
    static_assert(offsetof(PrefsBlob, colorOutput) == 8, "the byte fields start at 8");
    static_assert(offsetof(PrefsBlob, exposureStops) == 16, "exposureStops sits at 16");
    static_assert(offsetof(PrefsBlob, reserved) == 20, "reserved fills the rest");
    static_assert(std::is_trivially_copyable_v<PrefsBlob>, "the blob is memcpy'd to and from the host");

    REQUIRE(sizeof(PrefsBlob) == PrefsBlob::kSize);
    REQUIRE(PrefsBlob::cacheKeySize() == 128);
}

TEST_CASE("PrefsBlob defaults match the documented table", "[common][prefs]") {
    const PrefsBlob p = PrefsBlob::defaults();
    REQUIRE(p.isValid());
    REQUIRE(p.magic == PrefsBlob::kMagic);
    REQUIRE(p.version == PrefsBlob::kVersion);
    REQUIRE(p.color() == PrefsColorOutput::PQ);
    REQUIRE(p.size() == PrefsOutputSize::QHD2560);
    REQUIRE(p.stab() == PrefsStabilization::HorizonLock);
    REQUIRE(p.seamSearch == 1);
    REQUIRE(p.gainMatch == 1);
    REQUIRE(p.calib() == PrefsCalibration::Native);
    REQUIRE(p.fit() == PrefsDlogmFit::Osmo360);
    REQUIRE(p.exposureStops == 0.0f);
    REQUIRE(p.device() == PrefsRenderDevice::Auto);
    for (const std::uint8_t b : p.reserved) {
        REQUIRE(b == 0);
    }

    // Defaults are already clean, so sanitise() changes nothing.
    PrefsBlob copy = p;
    REQUIRE(copy.sanitise());
    REQUIRE(copy == p);
}

TEST_CASE("PrefsBlob sanitise clamps every out-of-range field", "[common][prefs]") {
    SECTION("garbage enums fall back to their defaults") {
        PrefsBlob p = PrefsBlob::defaults();
        p.colorOutput = 200;
        p.outputSize = 9;
        p.stabilization = 42;
        p.seamSearch = 7;
        p.gainMatch = 3;
        p.calibration = 250;
        p.dlogmFit = 88;
        p.renderDevice = 100;
        REQUIRE_FALSE(p.sanitise());
        REQUIRE(p.color() == PrefsColorOutput::PQ);
        REQUIRE(p.size() == PrefsOutputSize::QHD2560);
        REQUIRE(p.stab() == PrefsStabilization::HorizonLock);
        REQUIRE(p.seamSearch == 1);
        REQUIRE(p.gainMatch == 1);
        REQUIRE(p.calib() == PrefsCalibration::Native);
        REQUIRE(p.fit() == PrefsDlogmFit::DjiRefit);
        REQUIRE(p.device() == PrefsRenderDevice::Auto);
        // A second pass has nothing left to do.
        REQUIRE(p.sanitise());
    }

    SECTION("every colour output 0..3 is accepted and 4 upward is not") {
        // The enum gained D-Log M passthrough as value 3, APPENDED so saved
        // projects keep their values.  The boundary is worth pinning from
        // both sides: a sanitise() that still stopped at 2 would silently
        // reset every clip a user had set to passthrough.
        for (int value = 0; value < static_cast<int>(PrefsColorOutput::Count); ++value) {
            PrefsBlob p = PrefsBlob::defaults();
            p.colorOutput = static_cast<std::uint8_t>(value);
            INFO("colour output " << value);
            REQUIRE(p.sanitise());   // nothing to change: it is in range
            REQUIRE(p.colorOutput == static_cast<std::uint8_t>(value));
        }
        REQUIRE(static_cast<int>(PrefsColorOutput::DLogM) == 3);
        REQUIRE(static_cast<int>(PrefsColorOutput::Count) == 4);

        for (const int value : {4, 5, 99, 255}) {
            PrefsBlob p = PrefsBlob::defaults();
            p.colorOutput = static_cast<std::uint8_t>(value);
            INFO("out-of-range colour output " << value);
            REQUIRE_FALSE(p.sanitise());
            REQUIRE(p.color() == PrefsColorOutput::PQ);
        }
    }

    SECTION("valid values are preserved") {
        PrefsBlob p = PrefsBlob::defaults();
        p.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::HLG);
        p.outputSize = static_cast<std::uint8_t>(PrefsOutputSize::UHD4K);
        p.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Smooth);
        p.seamSearch = 0;
        p.gainMatch = 0;
        p.calibration = static_cast<std::uint8_t>(PrefsCalibration::Underwater);
        p.dlogmFit = static_cast<std::uint8_t>(PrefsDlogmFit::Pocket3);
        p.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Cuda);
        p.exposureStops = -1.5f;
        REQUIRE(p.sanitise());
        REQUIRE(p.color() == PrefsColorOutput::HLG);
        REQUIRE(p.size() == PrefsOutputSize::UHD4K);
        REQUIRE(p.stab() == PrefsStabilization::Smooth);
        REQUIRE(p.calib() == PrefsCalibration::Underwater);
        REQUIRE(p.fit() == PrefsDlogmFit::Pocket3);
        REQUIRE(p.device() == PrefsRenderDevice::Cuda);
        REQUIRE(p.exposureStops == -1.5f);
    }

    SECTION("exposure is clamped and NaN / infinity are reset") {
        PrefsBlob p = PrefsBlob::defaults();
        p.exposureStops = 99.0f;
        REQUIRE_FALSE(p.sanitise());
        REQUIRE(p.exposureStops == PrefsBlob::kMaxExposureStops);

        p.exposureStops = -99.0f;
        REQUIRE_FALSE(p.sanitise());
        REQUIRE(p.exposureStops == PrefsBlob::kMinExposureStops);

        // NaN compares false with everything, so it must not survive.
        p.exposureStops = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_FALSE(p.sanitise());
        REQUIRE(p.exposureStops == 0.0f);

        p.exposureStops = std::numeric_limits<float>::infinity();
        REQUIRE_FALSE(p.sanitise());
        REQUIRE(p.exposureStops == PrefsBlob::kMaxExposureStops);
    }

    SECTION("dirty reserved bytes are zeroed so the cache key stays stable") {
        PrefsBlob p = PrefsBlob::defaults();
        p.reserved[0] = 0xFF;
        p.reserved[107] = 0x01;
        REQUIRE_FALSE(p.sanitise());
        for (const std::uint8_t b : p.reserved) {
            REQUIRE(b == 0);
        }
    }
}

TEST_CASE("PrefsBlob fromBytes rejects anything that is not ours", "[common][prefs]") {
    const PrefsBlob good = PrefsBlob::defaults();

    SECTION("a null or short buffer gives defaults") {
        REQUIRE(PrefsBlob::fromBytes(nullptr, 128) == good);
        REQUIRE(PrefsBlob::fromBytes(&good, 0) == good);
        REQUIRE(PrefsBlob::fromBytes(&good, PrefsBlob::kSize - 1) == good);
    }

    SECTION("a foreign magic or version gives defaults") {
        PrefsBlob foreign = good;
        foreign.magic = 0x12345678u;
        REQUIRE(PrefsBlob::fromBytes(&foreign, sizeof(foreign)) == good);

        PrefsBlob future = good;
        future.version = 99;
        REQUIRE(PrefsBlob::fromBytes(&future, sizeof(future)) == good);
    }

    SECTION("our own bytes come back sanitised") {
        PrefsBlob stored = good;
        stored.colorOutput = static_cast<std::uint8_t>(PrefsColorOutput::Rec709);
        stored.exposureStops = 42.0f;  // out of range on purpose
        const PrefsBlob loaded = PrefsBlob::fromBytes(&stored, sizeof(stored));
        REQUIRE(loaded.color() == PrefsColorOutput::Rec709);
        REQUIRE(loaded.exposureStops == PrefsBlob::kMaxExposureStops);
    }

    SECTION("a longer buffer only contributes its first 128 bytes") {
        std::vector<std::uint8_t> big(256, 0x77);
        std::memcpy(big.data(), &good, sizeof(good));
        REQUIRE(PrefsBlob::fromBytes(big.data(), big.size()) == good);
    }
}

TEST_CASE("PrefsBlob comparison and cache key cover every field", "[common][prefs]") {
    PrefsBlob a = PrefsBlob::defaults();
    PrefsBlob b = PrefsBlob::defaults();
    REQUIRE(a == b);
    REQUIRE_FALSE(a != b);
    REQUIRE(std::memcmp(a.cacheKey(), b.cacheKey(), static_cast<std::size_t>(PrefsBlob::cacheKeySize())) == 0);

    // Any single setting change must change the key: that is the property the
    // PPix cache relies on.
    b.stabilization = static_cast<std::uint8_t>(PrefsStabilization::Off);
    REQUIRE(a != b);
    REQUIRE(std::memcmp(a.cacheKey(), b.cacheKey(), static_cast<std::size_t>(PrefsBlob::cacheKeySize())) != 0);

    b = a;
    b.exposureStops = 0.5f;
    REQUIRE(a != b);

    b = a;
    b.renderDevice = static_cast<std::uint8_t>(PrefsRenderDevice::Cpu);
    REQUIRE(a != b);
    REQUIRE(a.cacheKey() == &a);
}

// =============================================================================
//  PixelCopy - scalar helpers
// =============================================================================
TEST_CASE("PixelCopy half conversion is IEEE 754 binary16", "[common][pixelcopy]") {
    SECTION("exact values round trip bit for bit") {
        const float values[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 65504.0f, -65504.0f, 0.00006103515625f};
        for (const float v : values) {
            const std::uint16_t h = pc::floatToHalf(v);
            const float back = pc::halfToFloat(h);
            INFO("value " << v << " half 0x" << std::hex << h);
            REQUIRE(back == v);
        }
        // Signed zero survives.
        REQUIRE(pc::floatToHalf(0.0f) == 0x0000);
        REQUIRE(pc::floatToHalf(-0.0f) == 0x8000);
        REQUIRE(pc::floatToHalf(1.0f) == 0x3C00);
        REQUIRE(pc::floatToHalf(-2.0f) == 0xC000);
    }

    SECTION("values with more precision than a half round to nearest even") {
        // 1 + 2^-11 is exactly halfway between 1.0 and the next half; ties go
        // to even, i.e. back to 1.0.
        REQUIRE(pc::floatToHalf(1.0f + 1.0f / 2048.0f) == 0x3C00);
        // Just above the tie rounds up.
        REQUIRE(pc::floatToHalf(1.0f + 1.0f / 2048.0f + 1e-5f) == 0x3C01);
        // A general value: the round trip error is at most half an ulp.
        for (float v = -4.0f; v <= 4.0f; v += 0.013f) {
            const float back = pc::halfToFloat(pc::floatToHalf(v));
            REQUIRE(std::fabs(back - v) <= 0.002f);
        }
    }

    SECTION("subnormals, overflow and NaN behave") {
        // Smallest positive subnormal half = 2^-24.
        const float tiny = std::ldexp(1.0f, -24);
        REQUIRE(pc::floatToHalf(tiny) == 0x0001);
        REQUIRE(pc::halfToFloat(0x0001) == tiny);
        // Half of the smallest subnormal is a tie to even -> zero.
        REQUIRE(pc::floatToHalf(std::ldexp(1.0f, -25)) == 0x0000);
        // Anything smaller underflows to zero.
        REQUIRE(pc::floatToHalf(std::ldexp(1.0f, -40)) == 0x0000);

        // Beyond the half range becomes infinity, keeping the sign.
        REQUIRE(pc::floatToHalf(1.0e30f) == 0x7C00);
        REQUIRE(pc::floatToHalf(-1.0e30f) == 0xFC00);
        REQUIRE(std::isinf(pc::halfToFloat(0x7C00)));
        REQUIRE(pc::halfToFloat(0xFC00) < 0.0f);

        // NaN stays NaN (and quiet).
        REQUIRE(std::isnan(pc::halfToFloat(pc::floatToHalf(std::numeric_limits<float>::quiet_NaN()))));
    }
}

TEST_CASE("PixelCopy 8-bit quantisation rounds to nearest and clamps", "[common][pixelcopy]") {
    REQUIRE(pc::floatTo8u(0.0f) == 0);
    REQUIRE(pc::floatTo8u(1.0f) == 255);
    REQUIRE(pc::floatTo8u(-0.5f) == 0);
    REQUIRE(pc::floatTo8u(2.0f) == 255);
    REQUIRE(pc::floatTo8u(std::numeric_limits<float>::quiet_NaN()) == 0);

    // Round to nearest: 0.5/255 is the boundary between code 0 and 1.
    REQUIRE(pc::floatTo8u(0.4f / 255.0f) == 0);
    REQUIRE(pc::floatTo8u(0.6f / 255.0f) == 1);
    REQUIRE(pc::floatTo8u(128.0f / 255.0f) == 128);

    // Every code survives a round trip through u8ToFloat.
    for (int code = 0; code <= 255; ++code) {
        const float f = pc::u8ToFloat(static_cast<std::uint8_t>(code));
        REQUIRE(pc::floatTo8u(f) == static_cast<std::uint8_t>(code));
    }
}

// =============================================================================
//  PixelCopy - frames
// =============================================================================
TEST_CASE("PixelCopy round trips BGRA 32f through a bottom-left host frame", "[common][pixelcopy]") {
    osv::ThreadPool pool(2);
    const std::uint32_t w = 37;
    const std::uint32_t h = 11;
    const osv::render::ImageRGBAf source = makeTestImage(w, h);

    const bool negative = GENERATE(false, true);
    INFO("row bytes are " << (negative ? "negative" : "positive"));

    Buffer buffer(w, h, pc::kBytesPerPixel32f, negative);
    pc::HostFrame dst = buffer.frame(w, h);
    REQUIRE(dst.valid(pc::kBytesPerPixel32f));
    REQUIRE(pc::rgbaToHostBgra32f(source, dst, &pool).ok());

    SECTION("the channel order really is B, G, R, A") {
        // Host row 0 is the BOTTOM scanline, i.e. image row h - 1.
        const auto* row0 = reinterpret_cast<const float*>(pc::rowAddress(buffer.base, buffer.rowBytes, 0));
        const float* srcBottom = source.row(h - 1);
        REQUIRE(row0[0] == srcBottom[2]);  // B
        REQUIRE(row0[1] == srcBottom[1]);  // G
        REQUIRE(row0[2] == srcBottom[0]);  // R
        REQUIRE(row0[3] == srcBottom[3]);  // A
    }

    SECTION("the last host row is the top scanline") {
        const auto* rowLast = reinterpret_cast<const float*>(pc::rowAddress(buffer.base, buffer.rowBytes, h - 1));
        const float* srcTop = source.row(0);
        REQUIRE(rowLast[2] == srcTop[0]);
        REQUIRE(rowLast[1] == srcTop[1]);
        REQUIRE(rowLast[0] == srcTop[2]);
    }

    SECTION("reading it back gives the original image exactly") {
        auto readBack = pc::hostBgra32fToRgba(buffer.constFrame(w, h), &pool);
        REQUIRE(readBack.ok());
        const osv::render::ImageRGBAf& got = readBack.value();
        REQUIRE(got.w == w);
        REQUIRE(got.h == h);
        REQUIRE(maxAbsDiff(got, source) == 0.0f);
    }

    SECTION("without a thread pool the result is identical") {
        Buffer serial(w, h, pc::kBytesPerPixel32f, negative);
        pc::HostFrame serialDst = serial.frame(w, h);
        REQUIRE(pc::rgbaToHostBgra32f(source, serialDst, nullptr).ok());
        for (std::uint32_t row = 0; row < h; ++row) {
            const char* a = pc::rowAddress(buffer.base, buffer.rowBytes, row);
            const char* b = pc::rowAddress(serial.base, serial.rowBytes, row);
            REQUIRE(std::memcmp(a, b, static_cast<std::size_t>(w) * pc::kBytesPerPixel32f) == 0);
        }
    }
}

TEST_CASE("PixelCopy round trips BGRA 8u within one code", "[common][pixelcopy]") {
    osv::ThreadPool pool(2);
    const std::uint32_t w = 64;
    const std::uint32_t h = 9;
    const osv::render::ImageRGBAf source = makeTestImage(w, h);

    const bool negative = GENERATE(false, true);
    Buffer buffer(w, h, pc::kBytesPerPixel8u, negative);
    pc::HostFrame dst = buffer.frame(w, h);
    REQUIRE(pc::rgbaToHostBgra8u(source, dst, &pool).ok());

    auto readBack = pc::hostBgra8uToRgba(buffer.constFrame(w, h), &pool);
    REQUIRE(readBack.ok());
    // 8-bit quantisation costs at most half a code, so the round trip error
    // is bounded by 1/255 - the tolerance docs/PREMIERE.md states.
    REQUIRE(maxAbsDiff(readBack.value(), source) <= 1.0f / 255.0f);

    SECTION("the bottom-left origin holds for 8-bit too") {
        const auto* row0 = reinterpret_cast<const std::uint8_t*>(pc::rowAddress(buffer.base, buffer.rowBytes, 0));
        const float* srcBottom = source.row(h - 1);
        REQUIRE(row0[0] == pc::floatTo8u(srcBottom[2]));
        REQUIRE(row0[1] == pc::floatTo8u(srcBottom[1]));
        REQUIRE(row0[2] == pc::floatTo8u(srcBottom[0]));
        REQUIRE(row0[3] == pc::floatTo8u(srcBottom[3]));
    }
}

TEST_CASE("PixelCopy round trips top-left frames in 32f, 16f and 8u", "[common][pixelcopy]") {
    osv::ThreadPool pool(2);
    const std::uint32_t w = 40;
    const std::uint32_t h = 13;
    const osv::render::ImageRGBAf source = makeTestImage(w, h);
    const bool negative = GENERATE(false, true);

    SECTION("32f is lossless") {
        Buffer buffer(w, h, pc::kBytesPerPixel32f, negative);
        pc::HostFrame dst = buffer.frame(w, h);
        REQUIRE(pc::rgbaToTopLeftBgra32f(source, dst, &pool).ok());

        // Top-left: host row 0 IS image row 0.
        const auto* row0 = reinterpret_cast<const float*>(pc::rowAddress(buffer.base, buffer.rowBytes, 0));
        const float* srcTop = source.row(0);
        REQUIRE(row0[0] == srcTop[2]);
        REQUIRE(row0[2] == srcTop[0]);

        auto readBack = pc::topLeftBgra32fToRgba(buffer.constFrame(w, h), &pool);
        REQUIRE(readBack.ok());
        REQUIRE(maxAbsDiff(readBack.value(), source) == 0.0f);
    }

    SECTION("16f keeps half precision") {
        Buffer buffer(w, h, pc::kBytesPerPixel16f, negative);
        pc::HostFrame dst = buffer.frame(w, h);
        REQUIRE(pc::rgbaToTopLeftBgra16f(source, dst, &pool).ok());

        auto readBack = pc::topLeftBgra16fToRgba(buffer.constFrame(w, h), &pool);
        REQUIRE(readBack.ok());
        // Values live in [0, 1]; a half has 11 significant bits there, so
        // 2^-11 is a generous bound on the round trip error.
        REQUIRE(maxAbsDiff(readBack.value(), source) <= 1.0f / 2048.0f);

        // The stored halves are exactly what floatToHalf produces.
        const auto* row0 = reinterpret_cast<const std::uint16_t*>(pc::rowAddress(buffer.base, buffer.rowBytes, 0));
        const float* srcTop = source.row(0);
        REQUIRE(row0[0] == pc::floatToHalf(srcTop[2]));
        REQUIRE(row0[3] == pc::floatToHalf(srcTop[3]));
    }

    SECTION("8u stays within one code") {
        Buffer buffer(w, h, pc::kBytesPerPixel8u, negative);
        pc::HostFrame dst = buffer.frame(w, h);
        REQUIRE(pc::rgbaToTopLeftBgra8u(source, dst, &pool).ok());
        auto readBack = pc::topLeftBgra8uToRgba(buffer.constFrame(w, h), &pool);
        REQUIRE(readBack.ok());
        REQUIRE(maxAbsDiff(readBack.value(), source) <= 1.0f / 255.0f);
    }
}

TEST_CASE("PixelCopy top-left and bottom-left differ by exactly a row flip", "[common][pixelcopy]") {
    osv::ThreadPool pool(2);
    const std::uint32_t w = 16;
    const std::uint32_t h = 8;
    const osv::render::ImageRGBAf source = makeTestImage(w, h);

    Buffer bottomUp(w, h, pc::kBytesPerPixel32f, false);
    Buffer topDown(w, h, pc::kBytesPerPixel32f, false);
    pc::HostFrame a = bottomUp.frame(w, h);
    pc::HostFrame b = topDown.frame(w, h);
    REQUIRE(pc::rgbaToHostBgra32f(source, a, &pool).ok());
    REQUIRE(pc::rgbaToTopLeftBgra32f(source, b, &pool).ok());

    const std::size_t rowSize = static_cast<std::size_t>(w) * pc::kBytesPerPixel32f;
    for (std::uint32_t row = 0; row < h; ++row) {
        const char* fromBottom = pc::rowAddress(bottomUp.base, bottomUp.rowBytes, row);
        const char* fromTop = pc::rowAddress(topDown.base, topDown.rowBytes, h - 1u - row);
        REQUIRE(std::memcmp(fromBottom, fromTop, rowSize) == 0);
    }
}

TEST_CASE("PixelCopy refuses invalid frames instead of writing out of bounds", "[common][pixelcopy]") {
    const osv::render::ImageRGBAf source = makeTestImage(8, 4);
    std::vector<std::uint8_t> storage(8 * 4 * 16, 0);

    SECTION("null base") {
        pc::HostFrame dst{nullptr, 8 * 16, 8, 4};
        REQUIRE_FALSE(dst.valid(pc::kBytesPerPixel32f));
        REQUIRE_FALSE(pc::rgbaToHostBgra32f(source, dst, nullptr).ok());
    }

    SECTION("zero row bytes") {
        pc::HostFrame dst{reinterpret_cast<char*>(storage.data()), 0, 8, 4};
        REQUIRE_FALSE(dst.valid(pc::kBytesPerPixel32f));
        REQUIRE_FALSE(pc::rgbaToHostBgra32f(source, dst, nullptr).ok());
    }

    SECTION("a pitch too small for one row") {
        // 8 pixels of 16 bytes need 128; 64 is a truncated row.
        pc::HostFrame dst{reinterpret_cast<char*>(storage.data()), 64, 8, 4};
        REQUIRE_FALSE(dst.valid(pc::kBytesPerPixel32f));
        REQUIRE_FALSE(pc::rgbaToHostBgra32f(source, dst, nullptr).ok());
        // The same pitch is fine for an 8-bit frame (8 * 4 = 32 bytes).
        REQUIRE(dst.valid(pc::kBytesPerPixel8u));
    }

    SECTION("a size mismatch is refused") {
        pc::HostFrame dst{reinterpret_cast<char*>(storage.data()), 8 * 16, 8, 3};
        REQUIRE(dst.valid(pc::kBytesPerPixel32f));
        REQUIRE_FALSE(pc::rgbaToHostBgra32f(source, dst, nullptr).ok());
    }

    SECTION("reading an invalid frame yields an error, not an image") {
        pc::ConstHostFrame src{nullptr, 128, 8, 4};
        REQUIRE_FALSE(pc::hostBgra32fToRgba(src, nullptr).ok());
        REQUIRE_FALSE(pc::topLeftBgra16fToRgba(src, nullptr).ok());
    }

    SECTION("an invalid source image is refused") {
        const osv::render::ImageRGBAf empty;
        pc::HostFrame dst{reinterpret_cast<char*>(storage.data()), 8 * 16, 8, 4};
        REQUIRE_FALSE(pc::rgbaToHostBgra32f(empty, dst, nullptr).ok());
    }
}

// =============================================================================
//  PluginLog
// =============================================================================
TEST_CASE("PluginLog writes, filters by level and rotates", "[common][log]") {
    // A unique name so parallel ctest runs never fight over the same file.
    const std::wstring name = L"osv_test_" + std::to_wstring(GetCurrentProcessId());

    // Read through a Win32 handle with every share flag set: the log file is
    // open for writing in this very process, and this is exactly how a user
    // tailing the log (or a second host process) would see it.  An ifstream
    // is no good here - it cannot express the share mode.
    const auto readFile = [](const std::wstring& logPath) {
        const HANDLE handle = CreateFileW(logPath.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return std::string();
        }
        LARGE_INTEGER size{};
        std::string text;
        if (GetFileSizeEx(handle, &size) && size.QuadPart > 0) {
            text.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
            text.resize(read);
        }
        CloseHandle(handle);
        return text;
    };

    REQUIRE(PluginLog::init(name));
    const std::wstring path = PluginLog::filePath();
    REQUIRE_FALSE(path.empty());
    REQUIRE(path.find(name) != std::wstring::npos);
    REQUIRE(path.find(L"\\OpenOSV\\") != std::wstring::npos);

    // Start from a clean file for deterministic assertions.
    PluginLog::shutdown();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path), ec);
    std::filesystem::remove(std::filesystem::path(path + L".1"), ec);
    REQUIRE(PluginLog::init(name));

    SECTION("a written line carries the level, a timestamp and a thread id") {
        PluginLog::setLevel(PluginLog::Level::Info);
        PluginLog::info("hello {} number {}", "world", 42);
        const std::string text = readFile(path);
        REQUIRE(text.find("hello world number 42") != std::string::npos);
        REQUIRE(text.find("[INFO ]") != std::string::npos);
        REQUIRE(text.find("[tid ") != std::string::npos);
        // One record per line: embedded newlines are flattened.
        PluginLog::info("two\nlines");
        const std::string text2 = readFile(path);
        REQUIRE(text2.find("two lines") != std::string::npos);
    }

    SECTION("messages below the level are dropped") {
        PluginLog::setLevel(PluginLog::Level::Warn);
        REQUIRE_FALSE(PluginLog::enabled(PluginLog::Level::Info));
        REQUIRE(PluginLog::enabled(PluginLog::Level::Warn));
        REQUIRE(PluginLog::enabled(PluginLog::Level::Error));
        PluginLog::debug("debug-marker");
        PluginLog::info("info-marker");
        PluginLog::warn("warn-marker");
        const std::string text = readFile(path);
        REQUIRE(text.find("debug-marker") == std::string::npos);
        REQUIRE(text.find("info-marker") == std::string::npos);
        REQUIRE(text.find("warn-marker") != std::string::npos);

        // Off silences everything.
        PluginLog::setLevel(PluginLog::Level::Off);
        REQUIRE_FALSE(PluginLog::enabled(PluginLog::Level::Error));
        PluginLog::error("must-not-appear");
        REQUIRE(readFile(path).find("must-not-appear") == std::string::npos);
    }

    SECTION("once() writes a key exactly one time") {
        PluginLog::setLevel(PluginLog::Level::Info);
        REQUIRE(PluginLog::once("selector-42", PluginLog::Level::Info, "unsupported selector 42"));
        REQUIRE_FALSE(PluginLog::once("selector-42", PluginLog::Level::Info, "unsupported selector 42"));
        REQUIRE(PluginLog::once("selector-43", PluginLog::Level::Info, "unsupported selector 43"));

        const std::string text = readFile(path);
        std::size_t count = 0;
        for (std::size_t at = text.find("selector 42"); at != std::string::npos; at = text.find("selector 42", at + 1)) {
            ++count;
        }
        REQUIRE(count == 1);
        REQUIRE(text.find("selector 43") != std::string::npos);
    }

    SECTION("the file rotates to .log.1 once it passes the limit") {
        PluginLog::setLevel(PluginLog::Level::Info);
        // One line is about 100 bytes; kRotateBytes / 100 lines with a margin
        // crosses the limit without writing for ever.
        const std::string filler(512, 'x');
        const unsigned long long lines = PluginLog::kRotateBytes / 512ull + 16ull;
        for (unsigned long long i = 0; i < lines; ++i) {
            PluginLog::write(PluginLog::Level::Info, filler);
        }
        const std::filesystem::path backup(path + L".1");
        REQUIRE(std::filesystem::exists(backup));
        // The live file restarted, so it is far below the limit.
        REQUIRE(std::filesystem::file_size(std::filesystem::path(path)) < PluginLog::kRotateBytes);
        REQUIRE(std::filesystem::file_size(backup) >= PluginLog::kRotateBytes);

        // Logging still works after a rotation.
        PluginLog::info("after-rotation");
        REQUIRE(readFile(path).find("after-rotation") != std::string::npos);
    }

    SECTION("a formatting failure never propagates") {
        PluginLog::setLevel(PluginLog::Level::Info);
        // A perfectly valid format with an awkward argument still logs.
        PluginLog::info("{}", std::string(4096, 'z'));
        REQUIRE(readFile(path).size() > 4096);
    }

    PluginLog::setLevel(PluginLog::Level::Error);
    PluginLog::shutdown();
    // Logging after shutdown() must not crash (it only reaches the debugger).
    PluginLog::error("after shutdown");
    REQUIRE(PluginLog::filePath().empty());
    std::filesystem::remove(std::filesystem::path(path), ec);
    std::filesystem::remove(std::filesystem::path(path + L".1"), ec);
}

// =============================================================================
//  DelayLoad
// =============================================================================
TEST_CASE("DelayLoad resolves a DLL sitting beside the module", "[common][delayload]") {
    delayload::resetStats();

    SECTION("the module directory is the one this code lives in") {
        const std::wstring dir = delayload::moduleDirectory();
        REQUIRE_FALSE(dir.empty());
        REQUIRE(dir.back() == L'\\');
        // The common layer is linked statically into the test executable, so
        // "the module" is the .exe itself.
        REQUIRE(dir == executableDirectory());
    }

    SECTION("a DLL next to the executable is loaded from there") {
        const std::wstring dir = delayload::moduleDirectory();
        const std::filesystem::path probe = std::filesystem::path(dir) / L"osv_delayload_probe.dll";
        INFO("probe DLL at " << probe.string());
        REQUIRE(std::filesystem::exists(probe));

        const HMODULE handle = delayload::loadBesideModule("osv_delayload_probe.dll");
        REQUIRE(handle != nullptr);

        // It really is the DLL we built: its exported function answers.
        using ProbeFn = int(__cdecl*)();
        const auto fn = reinterpret_cast<ProbeFn>(GetProcAddress(handle, "osvDelayLoadProbe"));
        REQUIRE(fn != nullptr);
        REQUIRE(fn() == 424242);

        // The statistics record where it came from.
        const delayload::Stats stats = delayload::stats();
        REQUIRE(stats.resolvedBeside == 1);
        REQUIRE(stats.lastResolvedPath == probe.wstring());

        FreeLibrary(handle);
    }

    SECTION("a DLL that is not there falls back to the default search") {
        REQUIRE(delayload::loadBesideModule("osv_no_such_library.dll") == nullptr);
        // kernel32 exists on every Windows machine but NOT beside the test
        // executable, so the hook must decline it and let the loader win.
        REQUIRE(delayload::loadBesideModule("kernel32.dll") == nullptr);
    }

    SECTION("names with a path are rejected outright") {
        REQUIRE(delayload::loadBesideModule("..\\osv_delayload_probe.dll") == nullptr);
        REQUIRE(delayload::loadBesideModule("sub/dir.dll") == nullptr);
        REQUIRE(delayload::loadBesideModule("C:\\Windows\\System32\\kernel32.dll") == nullptr);
        REQUIRE(delayload::loadBesideModule(nullptr) == nullptr);
        REQUIRE(delayload::loadBesideModule("") == nullptr);
    }

    SECTION("the hook can be installed and removed") {
        const bool wasInstalled = delayload::hookInstalled();
        delayload::installHook();
        REQUIRE(delayload::hookInstalled());
        delayload::installHook();  // idempotent
        REQUIRE(delayload::hookInstalled());
        delayload::uninstallHook();
        REQUIRE_FALSE(delayload::hookInstalled());
        if (wasInstalled) {
            delayload::installHook();
        }
    }

    delayload::resetStats();
    REQUIRE(delayload::stats().resolvedBeside == 0);
    REQUIRE(delayload::stats().lastResolvedPath.empty());
}

// =============================================================================
//  HostSuites
// =============================================================================
TEST_CASE("HostSuites acquires and releases through the mock host", "[common][hostsuites]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();

    SECTION("every reference is given back when the bundle goes away") {
        {
            ImporterSuites suites;
            const int count = suites.acquire(basic);
            REQUIRE(count >= 4);
            REQUIRE(suites.hasEssentials());
            REQUIRE(host.totalSuiteRefs() == count);
            REQUIRE(suites.ppix);
            REQUIRE(suites.time);
            REQUIRE(suites.string);
            REQUIRE(suites.ppixCreator);
            // The cache suite is tried at v8 first.
            REQUIRE(suites.ppixCache.version() == 8);
        }
        REQUIRE(host.totalSuiteRefs() == 0);
    }

    SECTION("release() is idempotent") {
        ImporterSuites suites;
        REQUIRE(suites.acquire(basic) > 0);
        suites.release();
        REQUIRE(host.totalSuiteRefs() == 0);
        suites.release();
        REQUIRE(host.totalSuiteRefs() == 0);
        REQUIRE_FALSE(suites.hasEssentials());
    }

    SECTION("a missing suite is a graceful downgrade, not a crash") {
        host.setSuiteAvailable(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4, false);
        host.setSuiteAvailable(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion, false);

        ImporterSuites suites;
        REQUIRE(suites.acquire(basic) > 0);
        REQUIRE_FALSE(suites.ppixCreator2);
        REQUIRE(suites.ppixCreator2.get() == nullptr);
        REQUIRE_FALSE(suites.appInfo);
        // The essentials are still there, so the importer can carry on.
        REQUIRE(suites.hasEssentials());
        // An unavailable App Info Suite means "host version unknown".
        const HostVersion v = suites.hostVersion();
        REQUIRE_FALSE(v.valid);
        REQUIRE(v.toString() == "unknown");
        REQUIRE(v.appName() == "????");
        REQUIRE(suites.describe().find("PPixCreator2=none") != std::string::npos);
    }

    SECTION("the PPix Cache falls back from v8 to v7") {
        host.setSuiteAvailable(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion8, false);
        ImporterSuites suites;
        REQUIRE(suites.acquire(basic) > 0);
        REQUIRE(suites.ppixCache);
        REQUIRE(suites.ppixCache.version() == 7);
    }

    SECTION("a null basic suite is handled") {
        ImporterSuites suites;
        REQUIRE(suites.acquire(nullptr) == 0);
        REQUIRE_FALSE(suites.hasEssentials());
    }

    SECTION("the host version comes from the App Info Suite") {
        ImporterSuites suites;
        REQUIRE(suites.acquire(basic) > 0);
        const HostVersion v = suites.hostVersion();
        REQUIRE(v.valid);
        REQUIRE(v.major == 26);
        REQUIRE(v.toString() == "26.2.2");
        REQUIRE(v.appName() == "PPro");
        REQUIRE(suites.describe().find("host=PPro 26.2.2") != std::string::npos);
    }

    SECTION("SuiteHandle move keeps exactly one reference") {
        SuiteHandle<PrSDKTimeSuite> a(basic, kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
        REQUIRE(a);
        REQUIRE(host.suiteRefCount(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion) == 1);

        SuiteHandle<PrSDKTimeSuite> b(std::move(a));
        REQUIRE(b);
        REQUIRE_FALSE(a);  // NOLINT(bugprone-use-after-move) - deliberate
        REQUIRE(host.suiteRefCount(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion) == 1);

        SuiteHandle<PrSDKTimeSuite> c;
        c = std::move(b);
        REQUIRE(c);
        REQUIRE(host.suiteRefCount(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion) == 1);
        REQUIRE(c->GetTicksPerSecond != nullptr);
    }

    SECTION("acquireFirst picks the first version the host offers") {
        SuiteHandle<PrSDKPPixCacheSuite> handle;
        REQUIRE(handle.acquireFirst(basic, kPrSDKPPixCacheSuite, {99, 98, 8, 7}));
        REQUIRE(handle.version() == 8);
        handle.release();

        SuiteHandle<PrSDKPPixCacheSuite> none;
        REQUIRE_FALSE(none.acquireFirst(basic, kPrSDKPPixCacheSuite, {99, 98}));
        REQUIRE_FALSE(none);
        REQUIRE(none.version() == 0);
    }

    REQUIRE(host.totalSuiteRefs() == 0);
}

// =============================================================================
//  HostContext
// =============================================================================
TEST_CASE("HostContext hands out renderers and shuts down cleanly", "[common][hostcontext]") {
    // The context is a process-wide singleton; make sure this test starts and
    // finishes with it gone so ordering between test cases cannot matter.
    HostContext::shutdown();
    REQUIRE_FALSE(HostContext::exists());

    HostContext& ctx = HostContext::instance();
    REQUIRE(HostContext::exists());
    REQUIRE(ctx.threadPool().size() >= 1);
    REQUIRE(ctx.threadPoolShared() != nullptr);
    REQUIRE(&HostContext::instance() == &ctx);

    SECTION("the CPU backend is always available") {
        auto lease = ctx.acquireRenderer(RenderDevicePreference::Cpu);
        REQUIRE(lease.ok());
        REQUIRE(lease.value().renderer != nullptr);
        REQUIRE(lease.value().backend == "cpu");
        REQUIRE(lease.value().pool != nullptr);
        REQUIRE(ctx.backendAvailable("cpu"));

        // The same renderer comes back on a second request: it is cached.
        auto again = ctx.acquireRenderer(RenderDevicePreference::Cpu);
        REQUIRE(again.ok());
        REQUIRE(again.value().renderer == lease.value().renderer);
    }

    SECTION("auto returns the best backend the machine has") {
        auto lease = ctx.acquireRenderer(RenderDevicePreference::Auto);
        REQUIRE(lease.ok());
        REQUIRE(lease.value().renderer != nullptr);
        const std::string& backend = lease.value().backend;
        REQUIRE((backend == "cuda" || backend == "opencl" || backend == "cpu"));
    }

    SECTION("an unknown backend name is refused") {
        auto lease = ctx.acquireRenderer(std::string_view("nonsense"));
        REQUIRE_FALSE(lease.ok());
        REQUIRE_FALSE(ctx.backendAvailable("nonsense"));
        REQUIRE(ctx.backendFailure("nonsense") == "unknown backend");
    }

    SECTION("a failed probe is remembered, not repeated") {
        bool probed = false;
        // Ask for a backend that may or may not exist; either way the state
        // afterwards must be recorded.
        auto lease = ctx.acquireRenderer(RenderDevicePreference::Cuda);
        (void)ctx.backendAvailable("cuda", &probed);
        REQUIRE(probed);
        if (!lease.ok()) {
            REQUIRE_FALSE(ctx.backendAvailable("cuda"));
            REQUIRE_FALSE(ctx.backendFailure("cuda").empty());
            // A second attempt fails the same way without probing again.
            auto retry = ctx.acquireRenderer(RenderDevicePreference::Cuda);
            REQUIRE_FALSE(retry.ok());
        } else {
            REQUIRE(ctx.backendAvailable("cuda"));
            REQUIRE(lease.value().backend == "cuda");
        }
    }

    SECTION("preferenceName covers every enumerator") {
        REQUIRE(std::string(HostContext::preferenceName(RenderDevicePreference::Auto)) == "auto");
        REQUIRE(std::string(HostContext::preferenceName(RenderDevicePreference::Cpu)) == "cpu");
        REQUIRE(std::string(HostContext::preferenceName(RenderDevicePreference::Cuda)) == "cuda");
        REQUIRE(std::string(HostContext::preferenceName(RenderDevicePreference::OpenCl)) == "opencl");
    }

    SECTION("a lease outlives the context it came from") {
        HostContext::RendererLease held;
        {
            auto lease = HostContext::instance().acquireRenderer(RenderDevicePreference::Cpu);
            REQUIRE(lease.ok());
            held = std::move(lease).value();
        }
        HostContext::shutdown();
        REQUIRE_FALSE(HostContext::exists());
        // The renderer and its pool are still alive through the lease.
        REQUIRE(held.renderer != nullptr);
        REQUIRE(held.pool != nullptr);
        REQUIRE(held.pool->size() >= 1);
    }

    HostContext::shutdown();
    REQUIRE_FALSE(HostContext::exists());
    // Shutting down twice is a no-op.
    HostContext::shutdown();
    REQUIRE_FALSE(HostContext::exists());
}
