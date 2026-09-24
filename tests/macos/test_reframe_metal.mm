// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The effect's Metal GPU path (plugins/reframe/ReframeMetal.mm and
// ReframeKernel.metal) against the CPU path it falls back to
// (ReframeCpu.cpp), with this test standing in for Premiere: it makes the
// device, the queue and the frame buffers, builds the parameters with the
// effect's own buildParams(), and renders the same view both ways.
//
// SKIPs on a Mac without a Metal device.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <catch2/catch_test_macros.hpp>

#include "ReframeCpu.h"
#include "ReframeMetal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace osv::reframe;

namespace {

/// IEEE binary16 -> float, for comparing 16f frames as numbers.
float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    float value = 0.0f;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? NAN : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

/// A smooth, coloured equirect in `layout`, top-down, rows `rowBytes` apart.
std::vector<unsigned char> makeEquirect(int w, int h, PixelLayout layout, std::int32_t rowBytes) {
    std::vector<unsigned char> bytes(static_cast<std::size_t>(rowBytes) * static_cast<std::size_t>(h), 0);
    const std::size_t bpp = bytesPerPixel(layout);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
            const float rgba[4] = {0.5f + 0.4f * std::sin(6.2831853f * u * 3.0f),
                                   0.5f + 0.4f * std::cos(3.1415926f * v * 2.0f),
                                   0.3f + 0.2f * std::sin(6.2831853f * (u + v)), 1.0f};
            storePixel(bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(rowBytes) +
                           static_cast<std::size_t>(x) * bpp,
                       layout, rgba);
        }
    }
    return bytes;
}

/// Render one view on the CPU and through the Metal path; compare.
void compareOnce(id<MTLDevice> device, id<MTLCommandQueue> queue, PixelLayout layout, const Settings& settings) {
    constexpr int kSrcW = 1024;
    constexpr int kSrcH = 512;
    constexpr int kOutW = 640;
    constexpr int kOutH = 360;
    const std::size_t bpp = bytesPerPixel(layout);
    // Pitches with padding, as a host's GPU frames have.
    const std::int32_t srcRowBytes = static_cast<std::int32_t>(kSrcW * bpp + 64);
    const std::int32_t dstRowBytes = static_cast<std::int32_t>(kOutW * bpp + 32);

    std::vector<unsigned char> source = makeEquirect(kSrcW, kSrcH, layout, srcRowBytes);
    ConstFrameView src;
    src.base = source.data();
    src.rowBytes = srcRowBytes;
    src.width = kSrcW;
    src.height = kSrcH;
    src.layout = layout;
    src.topDown = true;

    const KernelSetup setup = buildParams(settings, src, kOutW, kOutH, SizePx{});
    INFO("setup: " << setupRejectName(setup.reject));
    REQUIRE(setup.valid);

    // ---- CPU --------------------------------------------------------------
    std::vector<unsigned char> cpu(static_cast<std::size_t>(dstRowBytes) * kOutH, 0);
    FrameView cpuDst;
    cpuDst.base = cpu.data();
    cpuDst.rowBytes = dstRowBytes;
    cpuDst.width = kOutW;
    cpuDst.height = kOutH;
    cpuDst.layout = layout;
    cpuDst.topDown = true;
    REQUIRE(renderCpu(setup, src, cpuDst, nullptr));

    // ---- Metal ------------------------------------------------------------
    id<MTLBuffer> srcBuffer = [device newBufferWithBytes:source.data()
                                                  length:source.size()
                                                 options:MTLResourceStorageModeShared];
    id<MTLBuffer> dstBuffer = [device newBufferWithLength:cpu.size() options:MTLResourceStorageModeShared];
    REQUIRE(srcBuffer != nil);
    REQUIRE(dstBuffer != nil);

    metal::FrameRequest request;
    request.device = (__bridge void*)device;
    request.queue = (__bridge void*)queue;
    request.params = setup.params;
    request.source = setup.source;
    request.sourceBuffer = (__bridge void*)srcBuffer;
    request.sourceOffset = static_cast<std::size_t>(static_cast<const unsigned char*>(setup.sourceRow0) -
                                                    static_cast<const unsigned char*>(src.base));
    request.outputBuffer = (__bridge void*)dstBuffer;
    request.outputRowBytes = dstRowBytes;
    request.outputIsHalf = layout == PixelLayout::Bgra16f ? 1 : 0;
    request.outputWidth = kOutW;
    request.outputHeight = kOutH;
    std::string error;
    const bool rendered = metal::renderFrame(0, request, error);
    INFO("renderFrame: " << error);
    REQUIRE(rendered);

    // ---- compare ----------------------------------------------------------
    const auto* gpu = static_cast<const unsigned char*>([dstBuffer contents]);
    REQUIRE(gpu != nullptr);
    double worst = 0.0;
    std::size_t over = 0;
    const std::size_t samples = static_cast<std::size_t>(kOutW) * kOutH * 4;
    for (int y = 0; y < kOutH; ++y) {
        for (int x = 0; x < kOutW * 4; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(dstRowBytes);
            float a = 0.0f;
            float b = 0.0f;
            if (layout == PixelLayout::Bgra16f) {
                std::uint16_t ha = 0;
                std::uint16_t hb = 0;
                std::memcpy(&ha, cpu.data() + at + static_cast<std::size_t>(x) * 2, 2);
                std::memcpy(&hb, gpu + at + static_cast<std::size_t>(x) * 2, 2);
                a = halfToFloat(ha);
                b = halfToFloat(hb);
            } else {
                std::memcpy(&a, cpu.data() + at + static_cast<std::size_t>(x) * 4, 4);
                std::memcpy(&b, gpu + at + static_cast<std::size_t>(x) * 4, 4);
            }
            const double d = std::fabs(static_cast<double>(a) - static_cast<double>(b));
            worst = std::max(worst, d);
            // Beyond two steps of a half float around 1.0 (2 * 2^-10).
            if (d > 2.0 / 1024.0) {
                ++over;
            }
        }
    }
    INFO((layout == PixelLayout::Bgra16f ? "16f" : "32f") << ": worst |cpu - metal| " << worst << ", " << over
                                                            << " of " << samples << " samples beyond 2^-9");
    // The same shared function on both sides: sampling positions differ by
    // the last ulp of a transcendental at most, which moves a bilinear
    // sample of this smooth frame by far less than one 16f step.
    CHECK(worst < 4.0 / 1024.0);
    CHECK(over <= samples / 10000);
}

}  // namespace

TEST_CASE("the effect's Metal kernel renders a view like the CPU path", "[macos][plugins][metal]") {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            SKIP("no Metal device");
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        REQUIRE(queue != nil);

        std::string error;
        const bool prepared = metal::prepareDevice(0, (__bridge void*)device, error);
        INFO("prepareDevice: " << error);
        REQUIRE(prepared);
        INFO("library: " << (metal::usesPrecompiledLibrary() ? "precompiled" : "run-time compiled"));

        Settings settings;  // the effect's defaults
        compareOnce(device, queue, PixelLayout::Bgra32f, settings);
        compareOnce(device, queue, PixelLayout::Bgra16f, settings);

        // A turned, tilted, rolled view with DJI's lens.
        settings.panDeg = 75.0;
        settings.tiltDeg = -20.0;
        settings.rollDeg = 8.0;
        settings.cameraModel = CameraModel::Dji;
        compareOnce(device, queue, PixelLayout::Bgra32f, settings);
        compareOnce(device, queue, PixelLayout::Bgra16f, settings);
        metal::releaseDevices();
    }
}

TEST_CASE("the effect's Metal path refuses requests it cannot honour", "[macos][plugins][metal]") {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            SKIP("no Metal device");
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> small = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];

        metal::FrameRequest request;
        request.device = (__bridge void*)device;
        request.queue = (__bridge void*)queue;
        request.sourceBuffer = (__bridge void*)small;
        request.outputBuffer = (__bridge void*)small;
        request.params.outW = 64;
        request.params.outH = 64;
        request.outputWidth = 64;
        request.outputHeight = 64;
        request.outputRowBytes = 64 * 16;
        request.source.w = 64;
        request.source.h = 32;
        request.source.pitchBytes = 64 * 16;

        std::string error;
        // Buffers far smaller than their layouts.
        CHECK_FALSE(metal::renderFrame(0, request, error));
        CHECK_FALSE(error.empty());

        // Output size disagreeing with the parameters.
        request.outputWidth = 32;
        error.clear();
        CHECK_FALSE(metal::renderFrame(0, request, error));

        // Missing objects and slots out of range.
        metal::FrameRequest empty;
        CHECK_FALSE(metal::renderFrame(0, empty, error));
        CHECK_FALSE(metal::renderFrame(1000, request, error));
        CHECK_FALSE(metal::prepareDevice(1000, (__bridge void*)device, error));
        CHECK_FALSE(metal::prepareDevice(0, nullptr, error));
    }
}
