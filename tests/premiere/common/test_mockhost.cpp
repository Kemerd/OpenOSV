// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tests of the mock Premiere Pro host (tests/premiere/mockhost).  The mock is
// what every later plug-in test runs against, so its own behaviour has to be
// pinned down first: if the fake host lies, a passing importer test proves
// nothing.
//
// Covered here (docs/PREMIERE.md, "Verification"):
//   * SPBasicSuite acquire / release reference counting and hidden suites;
//   * PPix creation and inspection, row bytes and the bottom-left origin;
//   * the PPix cache as a real LRU keyed on the prefs bytes;
//   * String Suite UTF-8 / UTF-16 round trips and buffer sizing;
//   * Time Suite tick arithmetic (254016000000 per second, exact 60000/1001);
//   * Video Segment GetParam keyframe interpolation and hold semantics;
//   * Sequence Info VR configuration;
//   * Error Suite event capture;
//   * GPU Device Suite allocation / free / CreateGPUPPix ([cuda]).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "MockHost.h"

#include "PrSDKAppInfoSuite.h"
#include "PrSDKColorSpaces.h"
#include "PrSDKErrorSuite.h"
#include "PrSDKGPUDeviceSuite.h"
#include "PrSDKMemoryManagerSuite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixCreator2Suite.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKSequenceInfoSuite.h"
#include "PrSDKStringSuite.h"
#include "PrSDKTimeSuite.h"
#include "PrSDKVideoSegmentSuite.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace osv::premiere;
using osv::premiere::mock::kTicksPerSecond;
using osv::premiere::mock::MockHost;

namespace {

/// Acquire a suite by (name, version) or fail the test with a clear message.
template <class SuiteT>
const SuiteT* require(SPBasicSuite* basic, const char* name, int version) {
    const void* raw = nullptr;
    const SPErr err = basic->AcquireSuite(name, version, &raw);
    INFO("acquiring suite " << name << " v" << version);
    REQUIRE(err == kSPNoError);
    REQUIRE(raw != nullptr);
    return static_cast<const SuiteT*>(raw);
}

/// A prefs-like byte blob for cache keys; the contents are opaque to the
/// cache, only the bytes matter.
std::vector<std::uint8_t> blob(std::uint8_t fill, std::size_t size = 128) {
    return std::vector<std::uint8_t>(size, fill);
}

}  // namespace

// =============================================================================
//  SPBasicSuite
// =============================================================================
TEST_CASE("MockHost SPBasicSuite reference counts every acquire", "[mockhost][suites]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    REQUIRE(basic != nullptr);
    REQUIRE(host.totalSuiteRefs() == 0);

    SECTION("acquire and release balance out") {
        const void* a = nullptr;
        REQUIRE(basic->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &a) == kSPNoError);
        REQUIRE(a != nullptr);
        REQUIRE(host.suiteRefCount(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == 1);

        // A second acquire of the same pair hands back the same pointer and
        // bumps the count: this is what makes a leak visible.
        const void* b = nullptr;
        REQUIRE(basic->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &b) == kSPNoError);
        REQUIRE(b == a);
        REQUIRE(host.suiteRefCount(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == 2);

        REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == kSPNoError);
        REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == kSPNoError);
        REQUIRE(host.suiteRefCount(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == 0);
        REQUIRE(host.totalSuiteRefs() == 0);
    }

    SECTION("releasing more than acquired is reported, never negative") {
        REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) != kSPNoError);
        REQUIRE(host.suiteRefCount(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == 0);
    }

    SECTION("an unknown suite or version fails with a null pointer") {
        const void* p = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xDEADBEEFull));
        REQUIRE(basic->AcquireSuite("No Such Suite", 1, &p) != kSPNoError);
        REQUIRE(p == nullptr);

        p = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xDEADBEEFull));
        REQUIRE(basic->AcquireSuite(kPrSDKPPixSuite, 99, &p) != kSPNoError);
        REQUIRE(p == nullptr);

        REQUIRE(host.suiteRefCount("No Such Suite", 1) == -1);
    }

    SECTION("setSuiteAvailable simulates an older host") {
        host.setSuiteAvailable(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4, false);
        const void* p = nullptr;
        REQUIRE(basic->AcquireSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4, &p) != kSPNoError);
        REQUIRE(p == nullptr);

        host.setSuiteAvailable(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4, true);
        REQUIRE(basic->AcquireSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4, &p) == kSPNoError);
        REQUIRE(p != nullptr);
        REQUIRE(basic->ReleaseSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4) == kSPNoError);
    }

    SECTION("the PPix Cache suite answers at both v8 and v7") {
        const void* v8 = nullptr;
        const void* v7 = nullptr;
        REQUIRE(basic->AcquireSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion8, &v8) == kSPNoError);
        REQUIRE(basic->AcquireSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion7, &v7) == kSPNoError);
        REQUIRE(v8 == v7);  // one table serves both, see MockPPix.cpp
        REQUIRE(basic->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion8) == kSPNoError);
        REQUIRE(basic->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion7) == kSPNoError);
    }
}

TEST_CASE("MockHost hands out the legacy piSuites table", "[mockhost][suites]") {
    MockHost host;
    piSuitesPtr suites = host.piSuites();
    REQUIRE(suites != nullptr);
    REQUIRE(suites->memFuncs != nullptr);
    REQUIRE(suites->utilFuncs != nullptr);
    REQUIRE(suites->utilFuncs->getSPBasicSuite != nullptr);
    REQUIRE(suites->utilFuncs->getSPBasicSuite() == host.basicSuite());

    SECTION("newHandle / disposeHandle track allocations") {
        const std::size_t before = host.liveMemoryBlocks();
        char** handle = suites->memFuncs->newHandleClear(256);
        REQUIRE(handle != nullptr);
        REQUIRE(*handle != nullptr);
        REQUIRE(suites->memFuncs->getHandleSize(handle) == 256);
        REQUIRE(host.liveMemoryBlocks() == before + 1);

        // A cleared handle really is zero filled.
        for (int i = 0; i < 256; ++i) {
            REQUIRE((*handle)[i] == 0);
        }
        suites->memFuncs->disposeHandle(handle);
        REQUIRE(host.liveMemoryBlocks() == before);
    }

    SECTION("newPtr / disposePtr track allocations and sizes") {
        char* p = suites->memFuncs->newPtr(64);
        REQUIRE(p != nullptr);
        REQUIRE(suites->memFuncs->getPtrSize(p) == 64);
        suites->memFuncs->disposePtr(p);
        // A pointer the host never handed out is ignored, not freed.
        char foreign = 0;
        suites->memFuncs->disposePtr(&foreign);
    }
}

// =============================================================================
//  PPix
// =============================================================================
TEST_CASE("MockHost PPix creation, row bytes and bottom-left origin", "[mockhost][ppix]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* creator = require<PrSDKPPixCreatorSuite>(basic, kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
    const auto* ppix = require<PrSDKPPixSuite>(basic, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);

    prRect bounds{};
    prSetRect(&bounds, 0, 0, 13, 7);  // deliberately not a multiple of anything

    PPixHand hand = nullptr;
    REQUIRE(creator->CreatePPix(&hand, PrPPixBufferAccess_ReadWrite, PrPixelFormat_BGRA_4444_32f, &bounds) ==
            suiteError_NoError);
    REQUIRE(hand != nullptr);
    REQUIRE(host.livePPixCount() == 1);

    const auto info = host.inspect(hand);
    REQUIRE(info.has_value());
    REQUIRE(info->width == 13);
    REQUIRE(info->height == 7);
    REQUIRE(info->format == PrPixelFormat_BGRA_4444_32f);
    REQUIRE(info->isGpu == false);
    REQUIRE(info->refCount == 1);

    SECTION("row bytes are positive, 128-aligned and hold a whole row") {
        csSDK_int32 rowBytes = 0;
        REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
        REQUIRE(rowBytes > 0);
        REQUIRE(rowBytes % 128 == 0);
        REQUIRE(rowBytes >= 13 * 16);
        REQUIRE(rowBytes == info->rowBytes);
    }

    SECTION("bounds report the full frame at the origin") {
        prRect got{};
        REQUIRE(ppix->GetBounds(hand, &got) == suiteError_NoError);
        REQUIRE(got.left == 0);
        REQUIRE(got.top == 0);
        REQUIRE(got.right == 13);
        REQUIRE(got.bottom == 7);
    }

    SECTION("host row 0 is the bottom scanline") {
        char* pixels = nullptr;
        csSDK_int32 rowBytes = 0;
        REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadWrite, &pixels) == suiteError_NoError);
        REQUIRE(ppix->GetRowBytes(hand, &rowBytes) == suiteError_NoError);
        REQUIRE(pixels != nullptr);

        // Write a marker into the pixel the host calls row 0, column 0.  By
        // Premiere's convention for uncompressed 4444 formats that is the
        // BOTTOM-LEFT pixel of the picture, i.e. image row (height - 1).
        auto* row0 = reinterpret_cast<float*>(pixels);
        row0[0] = 0.25f;  // B
        row0[1] = 0.50f;  // G
        row0[2] = 0.75f;  // R
        row0[3] = 1.00f;  // A

        // The last row in memory is the TOP scanline.
        auto* rowTop = reinterpret_cast<float*>(pixels + static_cast<std::ptrdiff_t>(rowBytes) * 6);
        rowTop[0] = 1.0f;

        // Nothing in between was touched.
        auto* rowMid = reinterpret_cast<const float*>(pixels + static_cast<std::ptrdiff_t>(rowBytes) * 3);
        REQUIRE(rowMid[0] == 0.0f);

        // Read back through the same addressing to prove the buffer is the
        // one the suite reports.
        char* again = nullptr;
        REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &again) == suiteError_NoError);
        REQUIRE(again == pixels);
        REQUIRE(reinterpret_cast<const float*>(again)[2] == 0.75f);
    }

    SECTION("pixel format and aspect ratio come back unchanged") {
        PrPixelFormat format = PrPixelFormat_Invalid;
        REQUIRE(ppix->GetPixelFormat(hand, &format) == suiteError_NoError);
        REQUIRE(format == PrPixelFormat_BGRA_4444_32f);

        csSDK_uint32 num = 0;
        csSDK_uint32 den = 0;
        REQUIRE(ppix->GetPixelAspectRatio(hand, &num, &den) == suiteError_NoError);
        REQUIRE(num == 1);
        REQUIRE(den == 1);
    }

    SECTION("disposing releases the record") {
        REQUIRE(ppix->Dispose(hand) == suiteError_NoError);
        REQUIRE(host.livePPixCount() == 0);
        // The handle is dead: the suite must refuse it, not crash.
        REQUIRE(!host.inspect(hand).has_value());
        csSDK_int32 rowBytes = 0;
        REQUIRE(ppix->GetRowBytes(hand, &rowBytes) != suiteError_NoError);
        hand = nullptr;
    }

    if (hand) {
        REQUIRE(ppix->Dispose(hand) == suiteError_NoError);
    }
    REQUIRE(host.livePPixCount() == 0);
    REQUIRE(basic->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion) == kSPNoError);
    REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == kSPNoError);
}

TEST_CASE("MockHost colour managed PPix creation keeps the colour space", "[mockhost][ppix]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* creator2 =
        require<PrSDKPPixCreator2Suite>(basic, kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4);
    const auto* ppix = require<PrSDKPPixSuite>(basic, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);

    const PrSDKColorSpaceID pq = host.colorSpaceId(kPrOverranged2100PQ);
    REQUIRE(host.predefinedName(pq) == std::string(kPrOverranged2100PQ));

    PPixHand hand = nullptr;
    REQUIRE(creator2->CreateColorManagedPPix(&hand, PrPPixBufferAccess_ReadWrite, PrPixelFormat_BGRA_4444_32f, 64, 32,
                                             false, 0, 1, 1, pq) == suiteError_NoError);
    REQUIRE(hand != nullptr);

    const auto info = host.inspect(hand);
    REQUIRE(info.has_value());
    REQUIRE(info->width == 64);
    REQUIRE(info->height == 32);
    REQUIRE(info->colorSpace.opaque[0] == pq.opaque[0]);
    REQUIRE(info->colorSpace.opaque[1] == pq.opaque[1]);
    REQUIRE(info->byteSize == static_cast<std::size_t>(info->rowBytes) * 32u);

    // A GPU format is never a host frame.
    PPixHand bad = nullptr;
    REQUIRE(creator2->CreateColorManagedPPix(&bad, PrPPixBufferAccess_ReadWrite, PrPixelFormat_GPU_BGRA_4444_32f, 8, 8,
                                             false, 0, 1, 1, pq) != suiteError_NoError);
    REQUIRE(bad == nullptr);

    REQUIRE(ppix->Dispose(hand) == suiteError_NoError);
    REQUIRE(host.livePPixCount() == 0);
    REQUIRE(basic->ReleaseSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4) == kSPNoError);
    REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == kSPNoError);
}

// =============================================================================
//  PPix cache
// =============================================================================
TEST_CASE("MockHost PPix cache hits, misses and prefs-keyed lookups", "[mockhost][cache]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* creator2 =
        require<PrSDKPPixCreator2Suite>(basic, kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4);
    const auto* ppix = require<PrSDKPPixSuite>(basic, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
    const auto* cache = require<PrSDKPPixCacheSuite>(basic, kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion8);

    const csSDK_uint32 importerId = 7;
    const csSDK_int32 stream = 0;
    std::vector<std::uint8_t> prefsA = blob(0xA1);
    std::vector<std::uint8_t> prefsB = blob(0xB2);
    PrSDKColorSpaceID space = host.colorSpaceId(kPrOverranged2100PQ);

    const auto makeFrame = [&]() {
        PPixHand h = nullptr;
        REQUIRE(creator2->CreatePPix(&h, PrPPixBufferAccess_ReadWrite, PrPixelFormat_BGRA_4444_32f, 32, 16, false, 0, 1,
                                     1) == suiteError_NoError);
        REQUIRE(h != nullptr);
        return h;
    };

    SECTION("a stored frame is found again and the caller gets a reference") {
        PPixHand stored = makeFrame();
        REQUIRE(cache->AddFrameToCacheWithColorSpace(importerId, stream, stored, 0, kPrRenderQuality_Max, prefsA.data(),
                                                     static_cast<csSDK_int32>(prefsA.size()),
                                                     &space) == suiteError_NoError);
        // The cache holds its own reference on top of ours.
        REQUIRE(host.inspect(stored)->refCount == 2);

        PPixHand found = nullptr;
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 0, 0, nullptr, &found, kPrRenderQuality_Max,
                                                       prefsA.data(), static_cast<csSDK_int32>(prefsA.size()),
                                                       &space) == suiteError_NoError);
        REQUIRE(found == stored);
        REQUIRE(host.cacheStats().hits == 1);
        REQUIRE(host.cacheStats().misses == 0);
        REQUIRE(host.inspect(stored)->refCount == 3);

        // Give back both of our references; the cache keeps the frame alive.
        REQUIRE(ppix->Dispose(found) == suiteError_NoError);
        REQUIRE(ppix->Dispose(stored) == suiteError_NoError);
        REQUIRE(host.livePPixCount() == 1);
        host.clearCache();
        REQUIRE(host.livePPixCount() == 0);
    }

    SECTION("a different prefs blob is a different frame") {
        PPixHand stored = makeFrame();
        REQUIRE(cache->AddFrameToCacheWithColorSpace(importerId, stream, stored, 0, kPrRenderQuality_Max, prefsA.data(),
                                                     static_cast<csSDK_int32>(prefsA.size()),
                                                     &space) == suiteError_NoError);
        REQUIRE(ppix->Dispose(stored) == suiteError_NoError);

        PPixHand found = nullptr;
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 0, 0, nullptr, &found, kPrRenderQuality_Max,
                                                       prefsB.data(), static_cast<csSDK_int32>(prefsB.size()),
                                                       &space) == suiteError_RenderedFrameNotFound);
        REQUIRE(found == nullptr);
        REQUIRE(host.cacheStats().misses == 1);

        // A single changed byte is enough: this is what stops a settings
        // change from hitting a stale frame.
        std::vector<std::uint8_t> prefsA2 = prefsA;
        prefsA2[64] = 0x00;
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 0, 0, nullptr, &found, kPrRenderQuality_Max,
                                                       prefsA2.data(), static_cast<csSDK_int32>(prefsA2.size()),
                                                       &space) == suiteError_RenderedFrameNotFound);
        REQUIRE(found == nullptr);

        // The original key still hits.
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 0, 0, nullptr, &found, kPrRenderQuality_Max,
                                                       prefsA.data(), static_cast<csSDK_int32>(prefsA.size()),
                                                       &space) == suiteError_NoError);
        REQUIRE(found != nullptr);
        REQUIRE(ppix->Dispose(found) == suiteError_NoError);
    }

    SECTION("a different frame number, quality or colour space misses") {
        PPixHand stored = makeFrame();
        REQUIRE(cache->AddFrameToCacheWithColorSpace(importerId, stream, stored, 5, kPrRenderQuality_Max, prefsA.data(),
                                                     static_cast<csSDK_int32>(prefsA.size()),
                                                     &space) == suiteError_NoError);
        REQUIRE(ppix->Dispose(stored) == suiteError_NoError);

        PPixHand found = nullptr;
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 6, 0, nullptr, &found, kPrRenderQuality_Max,
                                                       prefsA.data(), static_cast<csSDK_int32>(prefsA.size()),
                                                       &space) == suiteError_RenderedFrameNotFound);
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 5, 0, nullptr, &found, kPrRenderQuality_Draft,
                                                       prefsA.data(), static_cast<csSDK_int32>(prefsA.size()),
                                                       &space) == suiteError_RenderedFrameNotFound);
        PrSDKColorSpaceID other = host.colorSpaceId(kPrOverranged709);
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 5, 0, nullptr, &found, kPrRenderQuality_Max,
                                                       prefsA.data(), static_cast<csSDK_int32>(prefsA.size()),
                                                       &other) == suiteError_RenderedFrameNotFound);
        REQUIRE(host.cacheStats().misses == 3);
    }

    SECTION("the cache is a real LRU: the least recently used frame is evicted") {
        host.setCacheCapacity(2);
        REQUIRE(host.cacheStats().capacity == 2);

        // Three frames into a cache of two.
        for (csSDK_int32 frame = 0; frame < 2; ++frame) {
            PPixHand h = makeFrame();
            REQUIRE(cache->AddFrameToCacheWithColorSpace(importerId, stream, h, frame, kPrRenderQuality_Max,
                                                         prefsA.data(), static_cast<csSDK_int32>(prefsA.size()),
                                                         &space) == suiteError_NoError);
            REQUIRE(ppix->Dispose(h) == suiteError_NoError);
        }
        REQUIRE(host.cacheStats().entries == 2);

        // Touch frame 0 so frame 1 becomes the least recently used one.
        PPixHand touched = nullptr;
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 0, 0, nullptr, &touched,
                                                       kPrRenderQuality_Max, prefsA.data(),
                                                       static_cast<csSDK_int32>(prefsA.size()),
                                                       &space) == suiteError_NoError);
        REQUIRE(ppix->Dispose(touched) == suiteError_NoError);

        PPixHand third = makeFrame();
        REQUIRE(cache->AddFrameToCacheWithColorSpace(importerId, stream, third, 2, kPrRenderQuality_Max, prefsA.data(),
                                                     static_cast<csSDK_int32>(prefsA.size()),
                                                     &space) == suiteError_NoError);
        REQUIRE(ppix->Dispose(third) == suiteError_NoError);

        REQUIRE(host.cacheStats().evictions == 1);
        REQUIRE(host.cacheStats().entries == 2);

        PPixHand found = nullptr;
        // Frame 1 was the victim.
        REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, 1, 0, nullptr, &found, kPrRenderQuality_Max,
                                                       prefsA.data(), static_cast<csSDK_int32>(prefsA.size()),
                                                       &space) == suiteError_RenderedFrameNotFound);
        // Frames 0 and 2 survived.
        for (const csSDK_int32 frame : {0, 2}) {
            REQUIRE(cache->GetFrameFromCacheWithColorSpace(importerId, stream, frame, 0, nullptr, &found,
                                                           kPrRenderQuality_Max, prefsA.data(),
                                                           static_cast<csSDK_int32>(prefsA.size()),
                                                           &space) == suiteError_NoError);
            REQUIRE(found != nullptr);
            REQUIRE(ppix->Dispose(found) == suiteError_NoError);
        }
        // Evicted frames really were freed, not leaked.
        REQUIRE(host.livePPixCount() == 2);
    }

    SECTION("an evicted frame stays alive while the caller still holds it") {
        host.setCacheCapacity(1);
        PPixHand first = makeFrame();
        REQUIRE(cache->AddFrameToCacheWithColorSpace(importerId, stream, first, 0, kPrRenderQuality_Max, prefsA.data(),
                                                     static_cast<csSDK_int32>(prefsA.size()),
                                                     &space) == suiteError_NoError);
        PPixHand second = makeFrame();
        REQUIRE(cache->AddFrameToCacheWithColorSpace(importerId, stream, second, 1, kPrRenderQuality_Max, prefsA.data(),
                                                     static_cast<csSDK_int32>(prefsA.size()),
                                                     &space) == suiteError_NoError);
        // `first` was evicted but we still hold our own reference.
        REQUIRE(host.inspect(first).has_value());
        REQUIRE(host.inspect(first)->refCount == 1);
        REQUIRE(ppix->Dispose(first) == suiteError_NoError);
        REQUIRE(ppix->Dispose(second) == suiteError_NoError);
    }

    host.clearCache();
    REQUIRE(basic->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion8) == kSPNoError);
    REQUIRE(basic->ReleaseSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4) == kSPNoError);
    REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == kSPNoError);
}

// =============================================================================
//  String Suite
// =============================================================================
TEST_CASE("MockHost String Suite round trips UTF-8 and UTF-16", "[mockhost][string]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* strings = require<PrSDKStringSuite>(basic, kPrSDKStringSuite, kPrSDKStringSuiteVersion);

    SECTION("UTF-8 in, UTF-8 out") {
        const std::string text = "BT.2100 PQ RGB Full";
        PrSDKString s{};
        REQUIRE(strings->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(text.c_str()), &s) == suiteError_NoError);
        REQUIRE(host.liveStringCount() == 1);
        REQUIRE(host.utf8(s) == text);

        // A too-small buffer reports the needed size and writes nothing.
        csSDK_uint32 elements = 1;
        std::array<prUTF8Char, 64> buffer{};
        REQUIRE(strings->CopyToUTF8String(&s, buffer.data(), &elements) == suiteError_StringBufferTooSmall);
        REQUIRE(elements == text.size() + 1);
        REQUIRE(buffer[0] == 0);

        // The reported size is enough.
        REQUIRE(strings->CopyToUTF8String(&s, buffer.data(), &elements) == suiteError_NoError);
        REQUIRE(std::string(reinterpret_cast<const char*>(buffer.data())) == text);

        REQUIRE(strings->DisposeString(&s) == suiteError_NoError);
        REQUIRE(host.liveStringCount() == 0);
        // Disposing twice is an error, not a crash or a double free.
        REQUIRE(strings->DisposeString(&s) != suiteError_NoError);
    }

    SECTION("UTF-16 in, UTF-16 out, non-ASCII survives") {
        const std::wstring text = L"D-Log M → PQ";
        PrSDKString s{};
        REQUIRE(strings->AllocateFromUTF16(reinterpret_cast<const prUTF16Char*>(text.c_str()), &s) ==
                suiteError_NoError);

        csSDK_uint32 elements = 0;
        REQUIRE(strings->CopyToUTF16String(&s, nullptr, &elements) == suiteError_StringBufferTooSmall);
        REQUIRE(elements == text.size() + 1);

        std::vector<prUTF16Char> buffer(elements, 0);
        REQUIRE(strings->CopyToUTF16String(&s, buffer.data(), &elements) == suiteError_NoError);
        REQUIRE(std::wstring(reinterpret_cast<const wchar_t*>(buffer.data())) == text);

        // The mock also keeps a UTF-8 copy, which is what tests read.
        REQUIRE(host.utf8(s).find("D-Log M") == 0);
        REQUIRE(strings->DisposeString(&s) == suiteError_NoError);
    }

    SECTION("a zero string is a legal no-op and an unknown one is refused") {
        PrSDKString zero{};
        REQUIRE(strings->DisposeString(&zero) == suiteError_NoError);

        PrSDKString bogus{};
        bogus.opaque[0] = 4242;
        bogus.opaque[1] = 4242;
        REQUIRE(strings->DisposeString(&bogus) != suiteError_NoError);
        REQUIRE(host.utf8(bogus).empty());

        csSDK_uint32 elements = 16;
        std::array<prUTF8Char, 16> buffer{};
        REQUIRE(strings->CopyToUTF8String(&bogus, buffer.data(), &elements) != suiteError_NoError);
    }

    SECTION("makeString gives a test the same kind of string the host would") {
        PrSDKString s = host.makeString(kPrOverranged709);
        REQUIRE(host.utf8(s) == std::string(kPrOverranged709));
        REQUIRE(strings->DisposeString(&s) == suiteError_NoError);
    }

    REQUIRE(basic->ReleaseSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion) == kSPNoError);
}

// =============================================================================
//  Time Suite
// =============================================================================
TEST_CASE("MockHost Time Suite uses Premiere's tick rate", "[mockhost][time]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* time = require<PrSDKTimeSuite>(basic, kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);

    PrTime ticksPerSecond = 0;
    REQUIRE(time->GetTicksPerSecond(&ticksPerSecond) == suiteError_NoError);
    REQUIRE(ticksPerSecond == 254016000000ll);
    REQUIRE(ticksPerSecond == kTicksPerSecond);

    SECTION("59.94 fps divides the tick rate exactly") {
        PrTime ticks = 0;
        REQUIRE(time->GetTicksPerVideoFrame(kVideoFrameRate_NTSC_HD, &ticks) == suiteError_NoError);
        // 254016000000 * 1001 / 60000 = 4237833600 with no remainder: this
        // exactness is why Premiere picked that tick rate.
        REQUIRE(ticks == 4237833600ll);
        REQUIRE(ticksPerSecond * 1001ll % 60000ll == 0);
        // 60000/1001 frames really do add up to one second.
        REQUIRE(ticks * 60000ll / 1001ll == ticksPerSecond);
    }

    SECTION("the whole standard rate table is exact") {
        const struct {
            PrVideoFrameRates rate;
            PrTime expected;
        } cases[] = {
            {kVideoFrameRate_24, kTicksPerSecond / 24},
            {kVideoFrameRate_24Drop, kTicksPerSecond * 1001 / 24000},
            {kVideoFrameRate_PAL, kTicksPerSecond / 25},
            {kVideoFrameRate_NTSC, kTicksPerSecond * 1001 / 30000},
            {kVideoFrameRate_30, kTicksPerSecond / 30},
            {kVideoFrameRate_PAL_HD, kTicksPerSecond / 50},
            {kVideoFrameRate_NTSC_HD, kTicksPerSecond * 1001 / 60000},
            {kVideoFrameRate_60, kTicksPerSecond / 60},
        };
        for (const auto& c : cases) {
            PrTime ticks = 0;
            REQUIRE(time->GetTicksPerVideoFrame(c.rate, &ticks) == suiteError_NoError);
            REQUIRE(ticks == c.expected);
            REQUIRE(ticks > 0);
        }
    }

    SECTION("audio sample ticks are exact for 48 kHz and rounded for 44.1 kHz") {
        PrTime ticks = 0;
        REQUIRE(time->GetTicksPerAudioSample(48000.0f, &ticks) == suiteError_NoError);
        REQUIRE(ticks == kTicksPerSecond / 48000);
        REQUIRE(ticks * 48000ll == kTicksPerSecond);

        REQUIRE(time->GetTicksPerAudioSample(44100.0f, &ticks) == suiteError_NoError);
        REQUIRE(ticks == kTicksPerSecond / 44100);

        // Nonsense rates are refused rather than dividing by zero.
        REQUIRE(time->GetTicksPerAudioSample(0.0f, &ticks) != suiteError_NoError);
        REQUIRE(time->GetTicksPerAudioSample(-48000.0f, &ticks) != suiteError_NoError);
    }

    SECTION("null outputs are refused") {
        REQUIRE(time->GetTicksPerSecond(nullptr) != suiteError_NoError);
        REQUIRE(time->GetTicksPerVideoFrame(kVideoFrameRate_30, nullptr) != suiteError_NoError);
    }

    REQUIRE(basic->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion) == kSPNoError);
}

// =============================================================================
//  Video Segment Suite
// =============================================================================
TEST_CASE("MockHost Video Segment GetParam interpolates keyframes", "[mockhost][videosegment]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* vs = require<PrSDKVideoSegmentSuite>(basic, kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion9);

    const csSDK_int32 node = 100;
    const PrTime frame = kTicksPerSecond * 1001 / 60000;  // one 59.94 fps frame

    // Pan: a float32 angle, keyframed 0 deg at frame 0 and 90 deg at frame 10.
    PrParam a{};
    a.mType = kPrParamType_Float32;
    a.mFloat32 = 0.0f;
    PrParam b{};
    b.mType = kPrParamType_Float32;
    b.mFloat32 = 90.0f;
    host.setParam(node, 3, 0, a);
    host.setParam(node, 3, frame * 10, b);

    SECTION("values between keyframes are linear") {
        PrParam got{};
        REQUIRE(vs->GetParam(node, 3, frame * 5, &got) == suiteError_NoError);
        REQUIRE(got.mType == kPrParamType_Float32);
        REQUIRE_THAT(got.mFloat32, Catch::Matchers::WithinAbs(45.0f, 1e-4f));

        REQUIRE(vs->GetParam(node, 3, frame * 2, &got) == suiteError_NoError);
        REQUIRE_THAT(got.mFloat32, Catch::Matchers::WithinAbs(18.0f, 1e-4f));
    }

    SECTION("values on a keyframe are exact") {
        PrParam got{};
        REQUIRE(vs->GetParam(node, 3, 0, &got) == suiteError_NoError);
        REQUIRE(got.mFloat32 == 0.0f);
        REQUIRE(vs->GetParam(node, 3, frame * 10, &got) == suiteError_NoError);
        REQUIRE(got.mFloat32 == 90.0f);
    }

    SECTION("outside the keyframe range the value is clamped, never extrapolated") {
        PrParam got{};
        REQUIRE(vs->GetParam(node, 3, -frame * 100, &got) == suiteError_NoError);
        REQUIRE(got.mFloat32 == 0.0f);
        REQUIRE(vs->GetParam(node, 3, frame * 1000, &got) == suiteError_NoError);
        REQUIRE(got.mFloat32 == 90.0f);
    }

    SECTION("float64 sliders interpolate, popups and checkboxes hold") {
        PrParam fovA{};
        fovA.mType = kPrParamType_Float64;
        fovA.mFloat64 = 60.0;
        PrParam fovB{};
        fovB.mType = kPrParamType_Float64;
        fovB.mFloat64 = 120.0;
        host.setParam(node, 7, 0, fovA);
        host.setParam(node, 7, frame * 4, fovB);

        PrParam got{};
        REQUIRE(vs->GetParam(node, 7, frame * 2, &got) == suiteError_NoError);
        REQUIRE_THAT(got.mFloat64, Catch::Matchers::WithinAbs(90.0, 1e-9));

        // A popup (int32) holds its left keyframe: Premiere does not blend
        // discrete values, and the effect's preset popup relies on that.
        PrParam popA{};
        popA.mType = kPrParamType_Int32;
        popA.mInt32 = 1;
        PrParam popB{};
        popB.mType = kPrParamType_Int32;
        popB.mInt32 = 5;
        host.setParam(node, 1, 0, popA);
        host.setParam(node, 1, frame * 4, popB);
        REQUIRE(vs->GetParam(node, 1, frame * 3, &got) == suiteError_NoError);
        REQUIRE(got.mType == kPrParamType_Int32);
        REQUIRE(got.mInt32 == 1);

        // A checkbox likewise.
        PrParam boolA{};
        boolA.mType = kPrParamType_Bool;
        boolA.mBool = 0;
        PrParam boolB{};
        boolB.mType = kPrParamType_Bool;
        boolB.mBool = 1;
        host.setParam(node, 13, 0, boolA);
        host.setParam(node, 13, frame * 4, boolB);
        REQUIRE(vs->GetParam(node, 13, frame * 3, &got) == suiteError_NoError);
        REQUIRE(got.mBool == 0);
        REQUIRE(vs->GetParam(node, 13, frame * 4, &got) == suiteError_NoError);
        REQUIRE(got.mBool == 1);
    }

    SECTION("a single keyframe holds everywhere") {
        PrParam only{};
        only.mType = kPrParamType_Float32;
        only.mFloat32 = 15.0f;
        host.setParam(node, 8, frame * 3, only);
        PrParam got{};
        for (const PrTime t : {PrTime(0), frame * 3, frame * 99}) {
            REQUIRE(vs->GetParam(node, 8, t, &got) == suiteError_NoError);
            REQUIRE(got.mFloat32 == 15.0f);
        }
    }

    SECTION("unknown nodes and parameters are refused") {
        PrParam got{};
        REQUIRE(vs->GetParam(999, 3, 0, &got) == suiteError_IDNotValid);
        REQUIRE(vs->GetParam(node, 42, 0, &got) != suiteError_NoError);
        REQUIRE(vs->GetParam(node, 3, 0, nullptr) != suiteError_NoError);

        host.clearNode(node);
        REQUIRE(vs->GetParam(node, 3, 0, &got) == suiteError_IDNotValid);
    }

    SECTION("GetParamCount reports the highest index in use") {
        csSDK_int32 count = 0;
        REQUIRE(vs->GetParamCount(node, &count) == suiteError_NoError);
        REQUIRE(count == 4);  // index 3 is the highest set above
    }

    SECTION("node properties are returned in host memory") {
        host.setNodeProperty(node, "MatchName", "OpenOSV.Open360Reframe");
        PrMemoryPtr value = nullptr;
        REQUIRE(vs->GetNodeProperty(node, "MatchName", &value) == suiteError_NoError);
        REQUIRE(value != nullptr);
        REQUIRE(std::string(value) == "OpenOSV.Open360Reframe");
        // The value must be freeable through the Memory Manager, which is
        // what a plug-in would use.
        const auto* memory =
            require<PrSDKMemoryManagerSuite>(basic, kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion4);
        memory->PrDisposePtr(value);
        REQUIRE(basic->ReleaseSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion4) == kSPNoError);

        REQUIRE(vs->GetNodeProperty(node, "NoSuchKey", &value) != suiteError_NoError);
        REQUIRE(value == nullptr);
    }

    SECTION("the suite answers at v6 through v9 with one table") {
        for (int v = kPrSDKVideoSegmentSuiteVersion6; v <= kPrSDKVideoSegmentSuiteVersion9; ++v) {
            const void* raw = nullptr;
            REQUIRE(basic->AcquireSuite(kPrSDKVideoSegmentSuite, v, &raw) == kSPNoError);
            REQUIRE(raw == vs);
            REQUIRE(basic->ReleaseSuite(kPrSDKVideoSegmentSuite, v) == kSPNoError);
        }
    }

    REQUIRE(basic->ReleaseSuite(kPrSDKVideoSegmentSuite, kPrSDKVideoSegmentSuiteVersion9) == kSPNoError);
}

// =============================================================================
//  Sequence Info Suite
// =============================================================================
TEST_CASE("MockHost Sequence Info serves VR configuration and geometry", "[mockhost][sequence]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* seq = require<PrSDKSequenceInfoSuite>(basic, kPrSDKSequenceInfoSuite, kPrSDKSequenceInfoSuiteVersion);

    const PrTimelineID timeline = 0x1234;

    // sequence() creates the entry with defaults on first access.
    mock::SequenceConfig cfg = host.sequence(timeline);
    REQUIRE(cfg.frameRect.right == 1920);
    REQUIRE(cfg.frameRect.bottom == 1080);
    REQUIRE(cfg.projection == kPrIVProjectionType_None);

    SECTION("a plain 16:9 timeline reports no VR") {
        prRect rect{};
        REQUIRE(seq->GetFrameRect(timeline, &rect) == suiteError_NoError);
        REQUIRE(rect.right - rect.left == 1920);
        REQUIRE(rect.bottom - rect.top == 1080);

        PrIVProjectionType projection = kPrIVProjectionType_Equirectangular;
        PrIVFrameLayout layout = kPrIVFrameLayout_Monoscopic;
        csSDK_uint32 h = 0;
        csSDK_uint32 v = 0;
        REQUIRE(seq->GetImmersiveVideoVRConfiguration(timeline, &projection, &layout, &h, &v) == suiteError_NoError);
        REQUIRE(projection == kPrIVProjectionType_None);
    }

    SECTION("a configured VR timeline reports equirectangular 360 x 180") {
        cfg.frameRect.right = 3840;
        cfg.frameRect.bottom = 2160;
        cfg.projection = kPrIVProjectionType_Equirectangular;
        cfg.layout = kPrIVFrameLayout_Monoscopic;
        cfg.horizontalView = 360;
        cfg.verticalView = 180;
        cfg.ticksPerFrame = kTicksPerSecond * 1001 / 60000;
        host.setSequence(timeline, cfg);

        PrIVProjectionType projection = kPrIVProjectionType_None;
        PrIVFrameLayout layout = kPrIVFrameLayout_StereoscopicOverUnder;
        csSDK_uint32 h = 0;
        csSDK_uint32 v = 0;
        REQUIRE(seq->GetImmersiveVideoVRConfiguration(timeline, &projection, &layout, &h, &v) == suiteError_NoError);
        REQUIRE(projection == kPrIVProjectionType_Equirectangular);
        REQUIRE(layout == kPrIVFrameLayout_Monoscopic);
        REQUIRE(h == 360);
        REQUIRE(v == 180);

        prRect rect{};
        REQUIRE(seq->GetFrameRect(timeline, &rect) == suiteError_NoError);
        REQUIRE(rect.right == 3840);
        REQUIRE(rect.bottom == 2160);

        PrTime ticks = 0;
        REQUIRE(seq->GetFrameRate(timeline, &ticks) == suiteError_NoError);
        REQUIRE(ticks == 4237833600ll);

        csSDK_uint32 num = 0;
        csSDK_uint32 den = 0;
        REQUIRE(seq->GetPixelAspectRatio(timeline, &num, &den) == suiteError_NoError);
        REQUIRE(num == 1);
        REQUIRE(den == 1);
    }

    SECTION("an unknown timeline is refused, not answered with garbage") {
        const PrTimelineID unknown = 0xBEEF;
        prRect rect{};
        REQUIRE(seq->GetFrameRect(unknown, &rect) == suiteError_IDNotValid);

        // Removing a known timeline has the same effect.
        host.removeSequence(timeline);
        REQUIRE(seq->GetFrameRect(timeline, &rect) == suiteError_IDNotValid);
    }

    SECTION("the working colour space defaults to BT.709") {
        PrSDKColorSpaceID space{};
        REQUIRE(seq->GetWorkingColorSpace(timeline, &space) == suiteError_NoError);
        REQUIRE(host.predefinedName(space) == std::string(kPrRec709));
    }

    REQUIRE(basic->ReleaseSuite(kPrSDKSequenceInfoSuite, kPrSDKSequenceInfoSuiteVersion) == kSPNoError);
}

// =============================================================================
//  Error Suite and App Info Suite
// =============================================================================
TEST_CASE("MockHost Error Suite captures what a plug-in reports", "[mockhost][error]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* error = require<PrSDKErrorSuite3>(basic, kPrSDKErrorSuite, kPrSDKErrorSuiteVersion3);

    REQUIRE(host.errorEvents().empty());

    std::wstring title = L"OpenOSV";
    std::wstring description = L"Could not open the calibration blob";
    REQUIRE(error->SetEventStringUnicode(PrSDKErrorSuite3::kEventTypeError, reinterpret_cast<prUTF16Char*>(title.data()),
                                         reinterpret_cast<prUTF16Char*>(description.data())) == suiteError_NoError);

    std::wstring info = L"Rendering with the CPU backend";
    REQUIRE(error->SetEventStringUnicode(PrSDKErrorSuite3::kEventTypeInformational, reinterpret_cast<prUTF16Char*>(title.data()),
                                         reinterpret_cast<prUTF16Char*>(info.data())) == suiteError_NoError);

    const auto events = host.errorEvents();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == static_cast<csSDK_uint32>(PrSDKErrorSuite3::kEventTypeError));
    REQUIRE(events[0].title == title);
    REQUIRE(events[0].description == description);
    REQUIRE(events[1].type == static_cast<csSDK_uint32>(PrSDKErrorSuite3::kEventTypeInformational));
    REQUIRE(events[1].description == info);

    // Null strings are accepted (a plug-in may omit the title).
    REQUIRE(error->SetEventStringUnicode(PrSDKErrorSuite3::kEventTypeWarning, nullptr, nullptr) == suiteError_NoError);
    REQUIRE(host.errorEvents().size() == 3);
    REQUIRE(host.errorEvents()[2].title.empty());

    host.clearErrorEvents();
    REQUIRE(host.errorEvents().empty());
    REQUIRE(basic->ReleaseSuite(kPrSDKErrorSuite, kPrSDKErrorSuiteVersion3) == kSPNoError);
}

TEST_CASE("MockHost App Info Suite reports a configurable identity", "[mockhost][appinfo]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* appInfo = require<PrSDKAppInfoSuite>(basic, kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion);

    VersionInfo version{};
    REQUIRE(appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_Version, &version) == suiteError_NoError);
    REQUIRE(version.major == 26);

    csSDK_uint32 fourcc = 0;
    REQUIRE(appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_AppFourCC, &fourcc) == suiteError_NoError);
    REQUIRE(fourcc == static_cast<csSDK_uint32>('PPro'));

    // A test can pretend to be Media Encoder or an older Premiere.
    mock::AppIdentity ame;
    ame.fourcc = 'AME_';
    ame.major = 25;
    ame.minor = 6;
    ame.patch = 3;
    host.setAppIdentity(ame);
    REQUIRE(appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_Version, &version) == suiteError_NoError);
    REQUIRE(version.major == 25);
    REQUIRE(version.minor == 6);
    REQUIRE(version.patch == 3);
    REQUIRE(appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_AppFourCC, &fourcc) == suiteError_NoError);
    REQUIRE(fourcc == static_cast<csSDK_uint32>('AME_'));

    // An unknown selector is refused rather than writing into the caller's
    // buffer.
    csSDK_uint32 scratch = 0xFFFFFFFFu;
    REQUIRE(appInfo->GetAppInfo(9999, &scratch) != suiteError_NoError);
    REQUIRE(scratch == 0xFFFFFFFFu);

    REQUIRE(basic->ReleaseSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion) == kSPNoError);
}

// =============================================================================
//  GPU Device Suite
// =============================================================================
TEST_CASE("MockHost GPU Device Suite allocates, frees and creates GPU PPixes", "[mockhost][gpu][cuda]") {
    MockHost host;
    if (!host.gpuAvailable()) {
        SKIP("no CUDA device: " + host.gpuFailureReason());
    }

    SPBasicSuite* basic = host.basicSuite();
    const auto* gpu = require<PrSDKGPUDeviceSuite>(basic, kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion);

    csSDK_uint32 deviceCount = 0;
    REQUIRE(gpu->GetDeviceCount(&deviceCount) == suiteError_NoError);
    REQUIRE(deviceCount == 1);

    SECTION("GetDeviceInfo hands out a real CUDA context and stream") {
        PrGPUDeviceInfo info{};
        REQUIRE(gpu->GetDeviceInfo(kPrSDKGPUDeviceSuiteVersion, 0, &info) == suiteError_NoError);
        REQUIRE(info.outDeviceFramework == PrGPUDeviceFramework_CUDA);
        REQUIRE(info.outMeetsMinimumRequirementsForAcceleration == kPrTrue);
        REQUIRE(info.outContextHandle != nullptr);
        REQUIRE(info.outCommandQueueHandle != nullptr);

        // A device index the host does not have is refused.
        REQUIRE(gpu->GetDeviceInfo(kPrSDKGPUDeviceSuiteVersion, 1, &info) == suiteError_IDNotValid);
    }

    SECTION("device memory round trips") {
        void* memory = nullptr;
        REQUIRE(gpu->AllocateDeviceMemory(0, 1024 * 1024, &memory) == suiteError_NoError);
        REQUIRE(memory != nullptr);
        REQUIRE(gpu->FreeDeviceMemory(0, memory) == suiteError_NoError);
        // Freeing twice is refused, never a double free on the device.
        REQUIRE(gpu->FreeDeviceMemory(0, memory) != suiteError_NoError);
        REQUIRE(gpu->AllocateDeviceMemory(0, 0, &memory) != suiteError_NoError);
    }

    SECTION("pinned host memory round trips") {
        void* memory = nullptr;
        REQUIRE(gpu->AllocateHostMemory(0, 4096, &memory) == suiteError_NoError);
        REQUIRE(memory != nullptr);
        // Pinned memory is addressable from the CPU.
        std::memset(memory, 0x5A, 4096);
        REQUIRE(static_cast<const std::uint8_t*>(memory)[4095] == 0x5A);
        REQUIRE(gpu->FreeHostMemory(0, memory) == suiteError_NoError);
    }

    SECTION("CreateGPUPPix produces a top-left 256-aligned device frame") {
        PPixHand hand = nullptr;
        REQUIRE(gpu->CreateGPUPPix(0, PrPixelFormat_GPU_BGRA_4444_32f, 1920, 1080, 1, 1, prFieldsNone, &hand) ==
                suiteError_NoError);
        REQUIRE(hand != nullptr);

        const auto info = host.inspect(hand);
        REQUIRE(info.has_value());
        REQUIRE(info->isGpu);
        REQUIRE(info->width == 1920);
        REQUIRE(info->height == 1080);
        REQUIRE(info->rowBytes > 0);
        REQUIRE(info->rowBytes % 256 == 0);
        REQUIRE(info->rowBytes >= 1920 * 16);
        REQUIRE(info->byteSize == static_cast<std::size_t>(info->rowBytes) * 1080u);

        void* data = nullptr;
        REQUIRE(gpu->GetGPUPPixData(hand, &data) == suiteError_NoError);
        REQUIRE(data != nullptr);
        REQUIRE(data == info->pixels);

        csSDK_uint32 index = 0xFFFFFFFFu;
        REQUIRE(gpu->GetGPUPPixDeviceIndex(hand, &index) == suiteError_NoError);
        REQUIRE(index == 0);

        std::size_t size = 0;
        REQUIRE(gpu->GetGPUPPixSize(hand, &size) == suiteError_NoError);
        REQUIRE(size == info->byteSize);

        // A GPU frame has no host address.
        const auto* ppix = require<PrSDKPPixSuite>(basic, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        char* pixels = reinterpret_cast<char*>(static_cast<std::uintptr_t>(1));
        REQUIRE(ppix->GetPixels(hand, PrPPixBufferAccess_ReadOnly, &pixels) != suiteError_NoError);
        REQUIRE(pixels == nullptr);

        // Disposing frees the device allocation with it.
        REQUIRE(ppix->Dispose(hand) == suiteError_NoError);
        REQUIRE(host.livePPixCount() == 0);
        REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == kSPNoError);
    }

    SECTION("only the two GPU formats are accepted") {
        PPixHand hand = nullptr;
        REQUIRE(gpu->CreateGPUPPix(0, PrPixelFormat_BGRA_4444_32f, 64, 64, 1, 1, prFieldsNone, &hand) !=
                suiteError_NoError);
        REQUIRE(hand == nullptr);
        REQUIRE(gpu->CreateGPUPPix(0, PrPixelFormat_GPU_BGRA_4444_16f, 64, 64, 1, 1, prFieldsNone, &hand) ==
                suiteError_NoError);
        REQUIRE(hand != nullptr);
        const auto info = host.inspect(hand);
        REQUIRE(info.has_value());
        REQUIRE(info->rowBytes >= 64 * 8);

        const auto* ppix = require<PrSDKPPixSuite>(basic, kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        REQUIRE(ppix->Dispose(hand) == suiteError_NoError);
        REQUIRE(basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion) == kSPNoError);
    }

    REQUIRE(basic->ReleaseSuite(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion) == kSPNoError);
}

TEST_CASE("MockHost GPU Device Suite degrades cleanly without a device", "[mockhost][gpu]") {
    MockHost host;
    SPBasicSuite* basic = host.basicSuite();
    const auto* gpu = require<PrSDKGPUDeviceSuite>(basic, kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion);

    // Whatever the machine has, the count call itself always succeeds and a
    // GPU-less answer is a count of 0 with every other call refused - which
    // is what the effect's CreateInstance must survive.
    csSDK_uint32 deviceCount = 0xFFFFFFFFu;
    REQUIRE(gpu->GetDeviceCount(&deviceCount) == suiteError_NoError);
    REQUIRE(deviceCount == host.gpuDeviceCount());

    if (!host.gpuAvailable()) {
        REQUIRE(deviceCount == 0);
        REQUIRE_FALSE(host.gpuFailureReason().empty());
        PrGPUDeviceInfo info{};
        REQUIRE(gpu->GetDeviceInfo(kPrSDKGPUDeviceSuiteVersion, 0, &info) == suiteError_IDNotValid);
        REQUIRE(info.outContextHandle == nullptr);
        void* memory = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
        REQUIRE(gpu->AllocateDeviceMemory(0, 1024, &memory) == suiteError_IDNotValid);
        REQUIRE(memory == nullptr);
    } else {
        REQUIRE(deviceCount == 1);
        REQUIRE(host.gpuFailureReason().empty());
    }

    REQUIRE(basic->ReleaseSuite(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion) == kSPNoError);
}

// =============================================================================
//  Lifetime
// =============================================================================
TEST_CASE("MockHost cleans up after a sloppy plug-in", "[mockhost][lifetime]") {
    PPixHand leaked = nullptr;
    {
        MockHost host;
        REQUIRE(MockHost::current() == &host);
        SPBasicSuite* basic = host.basicSuite();
        const auto* creator2 =
            require<PrSDKPPixCreator2Suite>(basic, kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4);

        // Allocate a frame, a string and a handle and never give any back.
        REQUIRE(creator2->CreatePPix(&leaked, PrPPixBufferAccess_ReadWrite, PrPixelFormat_BGRA_4444_8u, 32, 32, false, 0,
                                     1, 1) == suiteError_NoError);
        (void)host.makeString("leaked");
        (void)host.piSuites()->memFuncs->newHandleClear(64);
        REQUIRE(host.livePPixCount() == 1);
        REQUIRE(host.liveStringCount() == 1);
        REQUIRE(host.liveMemoryBlocks() >= 1);
        // The suite is still acquired: the destructor must not care.
    }
    // Destruction released everything; there is no live host any more.
    REQUIRE(MockHost::current() == nullptr);

    // A second host can be created afterwards (the singleton was cleared).
    MockHost again;
    REQUIRE(MockHost::current() == &again);
    REQUIRE(again.livePPixCount() == 0);
    REQUIRE(again.liveStringCount() == 0);
}
