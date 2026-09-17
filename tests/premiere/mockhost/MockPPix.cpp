// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PPix Suite v1, PPix 2 Suite v3, PPix Creator v1, PPix Creator 2 v4 and
// PPix Cache v8 / v7 implementations.
//
// Host PPixes created here follow Premiere's uncompressed 4444 convention:
// row bytes are positive (width * bytesPerPixel rounded up to 128) but the
// first row in memory is the BOTTOM scanline of the image.  A plug-in that
// writes its top-down image must therefore address host row
// (height - 1 - y) for image row y.  GPU PPixes (MockGpu.cpp) are the
// opposite: top-left origin.

#include "MockHostImpl.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace osv::premiere::mock {

namespace {

MockHost::Impl* impl() noexcept {
    MockHost* h = MockHost::current();
    return h ? h->implForSuites() : nullptr;
}

/// Row pitch of a host PPix: 128-byte multiples so every row is SIMD
/// aligned, like the real host's 16-byte guarantee but stricter.
std::int32_t alignedRowBytes(std::uint32_t width, std::size_t bpp) noexcept {
    const std::size_t raw = static_cast<std::size_t>(width) * bpp;
    const std::size_t aligned = (raw + 127u) & ~static_cast<std::size_t>(127u);
    return static_cast<std::int32_t>(aligned);
}

CacheKey makeKey(csSDK_uint32 importerId, csSDK_int32 streamIndex, csSDK_int32 frameNumber, csSDK_uint32 quality,
                 const void* prefs, csSDK_int32 prefsLength, const PrSDKColorSpaceID* colorSpace) {
    CacheKey k;
    k.importerId = importerId;
    k.streamIndex = streamIndex;
    k.frameNumber = frameNumber;
    k.quality = quality;
    if (prefs && prefsLength > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(prefs);
        k.prefs.assign(bytes, bytes + prefsLength);
    }
    if (colorSpace) {
        k.colorSpace0 = colorSpace->opaque[0];
        k.colorSpace1 = colorSpace->opaque[1];
    }
    return k;
}

/// True when the cached record satisfies one of the requested formats
/// (0 in any field means "any").
bool formatsMatch(const PPixRecord& rec, csSDK_int32 numFormats, const imFrameFormat* formats) {
    if (numFormats <= 0 || !formats) {
        return true;
    }
    for (csSDK_int32 i = 0; i < numFormats; ++i) {
        const imFrameFormat& f = formats[i];
        const bool fmtOk = f.inPixelFormat == PrPixelFormat_Any || f.inPixelFormat == rec.format;
        const bool wOk = f.inFrameWidth == 0 || static_cast<std::uint32_t>(f.inFrameWidth) == rec.width;
        const bool hOk = f.inFrameHeight == 0 || static_cast<std::uint32_t>(f.inFrameHeight) == rec.height;
        if (fmtOk && wOk && hOk) {
            return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
//  PPix Suite v1
// -----------------------------------------------------------------------------
prSuiteError ppixDispose(PPixHand hand) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    p->releasePPix(rec);
    return suiteError_NoError;
}

prSuiteError ppixGetPixels(PPixHand hand, PrPPixBufferAccess, char** outAddress) {
    MockHost::Impl* p = impl();
    if (!p || !outAddress) {
        return suiteError_InvalidParms;
    }
    *outAddress = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    // GPU frames have no host address (PrSDKGPUDeviceSuite.h says GetPixels
    // is not valid on them).
    if (rec->isGpu) {
        return suiteError_InvalidCall;
    }
    *outAddress = rec->pixels;
    return suiteError_NoError;
}

prSuiteError ppixGetBounds(PPixHand hand, prRect* rect) {
    MockHost::Impl* p = impl();
    if (!p || !rect) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    prSetRect(rect, 0, 0, static_cast<int>(rec->width), static_cast<int>(rec->height));
    return suiteError_NoError;
}

prSuiteError ppixGetRowBytes(PPixHand hand, csSDK_int32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *out = rec->rowBytes;
    return suiteError_NoError;
}

prSuiteError ppixGetPixelAspectRatio(PPixHand hand, csSDK_uint32* num, csSDK_uint32* den) {
    MockHost::Impl* p = impl();
    if (!p || !num || !den) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *num = rec->parNum;
    *den = rec->parDen;
    return suiteError_NoError;
}

prSuiteError ppixGetPixelFormat(PPixHand hand, PrPixelFormat* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *out = rec->format;
    return suiteError_NoError;
}

/// The unique key is the record address plus format: 16 bytes.
constexpr std::size_t kUniqueKeySize = 16;

prSuiteError ppixGetUniqueKey(PPixHand hand, unsigned char* buffer, size_t size) {
    MockHost::Impl* p = impl();
    if (!p || !buffer || size < kUniqueKeySize) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    std::memset(buffer, 0, size);
    const std::uint64_t address = reinterpret_cast<std::uintptr_t>(rec);
    std::memcpy(buffer, &address, sizeof(address));
    const std::uint32_t fmt = static_cast<std::uint32_t>(rec->format);
    std::memcpy(buffer + 8, &fmt, sizeof(fmt));
    return suiteError_NoError;
}

prSuiteError ppixGetUniqueKeySize(size_t* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    *out = kUniqueKeySize;
    return suiteError_NoError;
}

prSuiteError ppixGetRenderTime(PPixHand hand, csSDK_int32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->record(hand)) {
        return suiteError_InvalidParms;
    }
    *out = 0;
    return suiteError_NoError;
}

// -----------------------------------------------------------------------------
//  PPix 2 Suite v3
// -----------------------------------------------------------------------------
prSuiteError ppix2GetSize(PPixHand hand, size_t* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *out = rec->byteSize;
    return suiteError_NoError;
}

prSuiteError ppix2GetYUV420PlanarBuffers(PPixHand, PrPPixBufferAccess, char**, csSDK_uint32*, char**, csSDK_uint32*,
                                         char**, csSDK_uint32*) {
    return suiteError_NotImplemented;
}

prSuiteError ppix2GetOrigin(PPixHand hand, csSDK_int32* x, csSDK_int32* y) {
    MockHost::Impl* p = impl();
    if (!p || !x || !y) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->record(hand)) {
        return suiteError_InvalidParms;
    }
    *x = 0;
    *y = 0;
    return suiteError_NoError;
}

prSuiteError ppix2GetFieldOrder(PPixHand hand, prFieldType* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *out = rec->fieldType;
    return suiteError_NoError;
}

// -----------------------------------------------------------------------------
//  PPix Creator Suite v1
// -----------------------------------------------------------------------------
prSuiteError creatorCreatePPix(PPixHand* out, PrPPixBufferAccess access, PrPixelFormat format, const prRect* bounds) {
    MockHost::Impl* p = impl();
    if (!p || !out || !bounds) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    // The header says ReadOnly is not allowed for a freshly created frame.
    if (access == PrPPixBufferAccess_ReadOnly) {
        return suiteError_InvalidParms;
    }
    const int w = bounds->right - bounds->left;
    const int h = bounds->bottom - bounds->top;
    if (w <= 0 || h <= 0) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->createHostPPix(format, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1, 1,
                                        prFieldsNone, kPrSDKColorSpaceID_Invalid);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *out = &rec->master;
    return suiteError_NoError;
}

prSuiteError creatorClonePPix(PPixHand source, PPixHand* out, PrPPixBufferAccess access) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(source);
    if (!rec || rec->isGpu) {
        return suiteError_InvalidParms;
    }
    // Read-only clones share the buffer (reference counted); writable clones
    // get a copy so the original stays untouched.
    if (access == PrPPixBufferAccess_ReadOnly) {
        p->retainPPix(rec);
        *out = source;
        return suiteError_NoError;
    }
    PPixRecord* copy =
        p->createHostPPix(rec->format, rec->width, rec->height, rec->parNum, rec->parDen, rec->fieldType, rec->colorSpace);
    if (!copy) {
        return suiteError_OutOfMemory;
    }
    std::memcpy(copy->pixels, rec->pixels, std::min(copy->byteSize, rec->byteSize));
    *out = &copy->master;
    return suiteError_NoError;
}

// -----------------------------------------------------------------------------
//  PPix Creator 2 Suite v4
// -----------------------------------------------------------------------------
prSuiteError creator2CreatePPix(PPixHand* out, PrPPixBufferAccess access, PrPixelFormat format, int width, int height,
                                bool useFields, int, int parNum, int parDen) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    if (access == PrPPixBufferAccess_ReadOnly || width <= 0 || height <= 0 || parNum <= 0 || parDen <= 0) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->createHostPPix(format, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                        static_cast<csSDK_uint32>(parNum), static_cast<csSDK_uint32>(parDen),
                                        useFields ? prFieldsUpperFirst : prFieldsNone, kPrSDKColorSpaceID_Invalid);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *out = &rec->master;
    return suiteError_NoError;
}

prSuiteError creator2CreateRawPPix(PPixHand*, PrPPixBufferAccess, int, int) { return suiteError_NotImplemented; }

prSuiteError creator2CreateCustomPPix(PPixHand*, PrPPixBufferAccess, PrPixelFormat, int, int, int, int, int) {
    return suiteError_NotImplemented;
}

prSuiteError creator2CreateDiskAlignedPPix(PPixHand*, PrPixelFormat, int, int, int, int, int, int, int) {
    return suiteError_NotImplemented;
}

prSuiteError creator2CreateColorManagedPPix(PPixHand* out, PrPPixBufferAccess access, PrPixelFormat format, int width,
                                            int height, bool useFields, int, int parNum, int parDen,
                                            PrSDKColorSpaceID colorSpace) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    if (access == PrPPixBufferAccess_ReadOnly || width <= 0 || height <= 0 || parNum <= 0 || parDen <= 0) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->createHostPPix(format, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                        static_cast<csSDK_uint32>(parNum), static_cast<csSDK_uint32>(parDen),
                                        useFields ? prFieldsUpperFirst : prFieldsNone, colorSpace);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    *out = &rec->master;
    return suiteError_NoError;
}

prSuiteError creator2CreateColorManagedCustomPPix(PPixHand*, PrPPixBufferAccess, PrPixelFormat, int, int, int, int, int,
                                                  PrSDKColorSpaceID) {
    return suiteError_NotImplemented;
}

prSuiteError creator2CreateColorManagedDiskAlignedPPix(PPixHand*, PrPixelFormat, int, int, int, int, int, int, int,
                                                       PrSDKColorSpaceID) {
    return suiteError_NotImplemented;
}

// -----------------------------------------------------------------------------
//  PPix Cache Suite v8 (v7 shares the table; see MockHost.h)
// -----------------------------------------------------------------------------

/// Insert (or refresh) a frame under `key`; the cache takes a reference.
prSuiteError cacheInsert(MockHost::Impl& p, CacheKey key, PPixHand hand) {
    PPixRecord* rec = p.record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    if (p.cacheCapacity == 0) {
        return suiteError_NoError;  // caching disabled: accept and forget
    }
    auto existing = p.cacheMap.find(key);
    if (existing != p.cacheMap.end()) {
        // Replace: release the old frame, move the key to the front.
        PPixRecord* old = existing->second.first;
        p.cacheOrder.erase(existing->second.second);
        p.cacheMap.erase(existing);
        p.releasePPix(old);
    }
    while (p.cacheMap.size() >= p.cacheCapacity && !p.cacheOrder.empty()) {
        const CacheKey victim = p.cacheOrder.back();
        auto it = p.cacheMap.find(victim);
        if (it != p.cacheMap.end()) {
            PPixRecord* victimRec = it->second.first;
            p.cacheMap.erase(it);
            p.releasePPix(victimRec);
        }
        p.cacheOrder.pop_back();
        ++p.cacheEvictions;
    }
    p.retainPPix(rec);
    p.cacheOrder.push_front(key);
    p.cacheMap.emplace(std::move(key), std::make_pair(rec, p.cacheOrder.begin()));
    return suiteError_NoError;
}

/// Look a frame up; on a hit the caller receives a new reference.
prSuiteError cacheLookup(MockHost::Impl& p, const CacheKey& key, csSDK_int32 numFormats, const imFrameFormat* formats,
                         PPixHand* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    auto it = p.cacheMap.find(key);
    if (it == p.cacheMap.end() || !formatsMatch(*it->second.first, numFormats, formats)) {
        ++p.cacheMisses;
        return suiteError_RenderedFrameNotFound;
    }
    // Move to the front (most recently used).
    p.cacheOrder.erase(it->second.second);
    p.cacheOrder.push_front(key);
    it->second.second = p.cacheOrder.begin();
    ++p.cacheHits;
    p.retainPPix(it->second.first);
    *out = &it->second.first->master;
    return suiteError_NoError;
}

prSuiteError cacheAddFrame(csSDK_uint32 importerId, csSDK_int32 streamIndex, PPixHand hand, csSDK_int32 frameNumber,
                           void* prefs, csSDK_int32 prefsLength) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return cacheInsert(*p, makeKey(importerId, streamIndex, frameNumber, 0xFFFFFFFFu, prefs, prefsLength, nullptr), hand);
}

prSuiteError cacheGetFrame(csSDK_uint32 importerId, csSDK_int32 streamIndex, csSDK_int32 frameNumber,
                           csSDK_int32 numFormats, imFrameFormat* formats, PPixHand* out, void* prefs,
                           csSDK_int32 prefsLength) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return cacheLookup(*p, makeKey(importerId, streamIndex, frameNumber, 0xFFFFFFFFu, prefs, prefsLength, nullptr),
                       numFormats, formats, out);
}

prSuiteError cacheAddRaw(csSDK_uint32 importerId, PPixHand hand, csSDK_int32 key) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    const auto mapKey = std::make_pair(importerId, key);
    auto it = p->rawCache.find(mapKey);
    if (it != p->rawCache.end()) {
        PPixRecord* old = it->second;
        p->rawCache.erase(it);
        p->releasePPix(old);
    }
    p->retainPPix(rec);
    p->rawCache.emplace(mapKey, rec);
    return suiteError_NoError;
}

prSuiteError cacheGetRaw(csSDK_uint32 importerId, csSDK_int32 key, PPixHand* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    auto it = p->rawCache.find(std::make_pair(importerId, key));
    if (it == p->rawCache.end()) {
        return suiteError_RenderedFrameNotFound;
    }
    p->retainPPix(it->second);
    *out = &it->second->master;
    return suiteError_NoError;
}

std::string pluginIdKey(const prPluginID* id) {
    if (!id) {
        return {};
    }
    // mGUID is NUL terminated per PrSDKTypes.h; guard anyway.
    const char* begin = id->mGUID;
    const char* end = static_cast<const char*>(std::memchr(begin, '\0', sizeof(id->mGUID)));
    return std::string(begin, end ? end : begin + sizeof(id->mGUID));
}

prSuiteError cacheAddNamed(const prPluginID* id, PPixHand hand) {
    MockHost::Impl* p = impl();
    if (!p || !id) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    const std::string key = pluginIdKey(id);
    auto it = p->namedCache.find(key);
    if (it != p->namedCache.end()) {
        PPixRecord* old = it->second;
        p->namedCache.erase(it);
        p->releasePPix(old);
    }
    p->retainPPix(rec);
    p->namedCache.emplace(key, rec);
    return suiteError_NoError;
}

prSuiteError cacheGetNamed(const prPluginID* id, PPixHand* out) {
    MockHost::Impl* p = impl();
    if (!p || !id || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    auto it = p->namedCache.find(pluginIdKey(id));
    if (it == p->namedCache.end()) {
        return suiteError_RenderedFrameNotFound;
    }
    p->retainPPix(it->second);
    *out = &it->second->master;
    return suiteError_NoError;
}

prSuiteError cacheRegisterNamed(const prPluginID* id) { return id ? suiteError_NoError : suiteError_InvalidParms; }
prSuiteError cacheUnregisterNamed(const prPluginID* id) { return id ? suiteError_NoError : suiteError_InvalidParms; }

prSuiteError cacheExpireNamed(const prPluginID* id) {
    MockHost::Impl* p = impl();
    if (!p || !id) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    auto it = p->namedCache.find(pluginIdKey(id));
    if (it == p->namedCache.end()) {
        return suiteError_RenderedFrameNotFound;
    }
    PPixRecord* rec = it->second;
    p->namedCache.erase(it);
    p->releasePPix(rec);
    return suiteError_NoError;
}

prSuiteError cacheExpireAll() {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->clearCacheLocked();
    return suiteError_NoError;
}

prSuiteError cacheAddWithProfile(csSDK_uint32, csSDK_int32, PPixHand, csSDK_int32, void*, csSDK_int32, PrSDKString*, void*,
                                 csSDK_int32) {
    return suiteError_NotImplemented;
}

prSuiteError cacheGetWithProfile(csSDK_uint32, csSDK_int32, csSDK_int32, csSDK_int32, imFrameFormat*, PPixHand* out, void*,
                                 csSDK_int32, PrSDKString*, void*, csSDK_int32) {
    if (out) {
        *out = nullptr;
    }
    return suiteError_NotImplemented;
}

prSuiteError cacheAddWithProfile2(csSDK_uint32, csSDK_int32, PPixHand, csSDK_int32, PrRenderQuality, void*, csSDK_int32,
                                  PrSDKString*, void*, csSDK_int32) {
    return suiteError_NotImplemented;
}

prSuiteError cacheGetWithProfile2(csSDK_uint32, csSDK_int32, csSDK_int32, csSDK_int32, imFrameFormat*, PPixHand* out,
                                  PrRenderQuality, void*, csSDK_int32, PrSDKString*, void*, csSDK_int32) {
    if (out) {
        *out = nullptr;
    }
    return suiteError_NotImplemented;
}

prSuiteError cacheAddWithColorSpace(csSDK_uint32 importerId, csSDK_int32 streamIndex, PPixHand hand,
                                    csSDK_int32 frameNumber, PrRenderQuality quality, void* prefs,
                                    csSDK_int32 prefsLength, PrSDKColorSpaceID* colorSpace) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return cacheInsert(
        *p, makeKey(importerId, streamIndex, frameNumber, static_cast<csSDK_uint32>(quality), prefs, prefsLength, colorSpace),
        hand);
}

prSuiteError cacheGetWithColorSpace(csSDK_uint32 importerId, csSDK_int32 streamIndex, csSDK_int32 frameNumber,
                                    csSDK_int32 numFormats, imFrameFormat* formats, PPixHand* out,
                                    PrRenderQuality quality, void* prefs, csSDK_int32 prefsLength,
                                    PrSDKColorSpaceID* colorSpace) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return cacheLookup(
        *p, makeKey(importerId, streamIndex, frameNumber, static_cast<csSDK_uint32>(quality), prefs, prefsLength, colorSpace),
        numFormats, formats, out);
}

prSuiteError cacheRegisterDependency(csSDK_uint32, csSDK_int32, csSDK_int32, csSDK_int32, imFrameFormat*, prPluginID*,
                                     void*, csSDK_int32) {
    return suiteError_NotImplemented;
}

prSuiteError cacheRegisterDependencyProfile(csSDK_uint32, csSDK_int32, csSDK_int32, csSDK_int32, imFrameFormat*,
                                            prPluginID*, void*, csSDK_int32, PrSDKString*, void*, csSDK_int32) {
    return suiteError_NotImplemented;
}

prSuiteError cacheRegisterDependencyProfile2(csSDK_uint32, csSDK_int32, csSDK_int32, csSDK_int32, imFrameFormat*,
                                             PrRenderQuality, prPluginID*, void*, csSDK_int32, PrSDKString*, void*,
                                             csSDK_int32) {
    return suiteError_NotImplemented;
}

prSuiteError cacheRegisterDependencyColorSpace(csSDK_uint32, csSDK_int32, csSDK_int32, csSDK_int32, imFrameFormat*,
                                               PrRenderQuality, prPluginID*, void*, csSDK_int32, PrSDKColorSpaceID*) {
    return suiteError_NotImplemented;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Shared helpers (Impl)
// -----------------------------------------------------------------------------
std::size_t bytesPerPixel(PrPixelFormat format) noexcept {
    switch (format) {
    case PrPixelFormat_BGRA_4444_8u:
    case PrPixelFormat_BGRX_4444_8u:
    case PrPixelFormat_ARGB_4444_8u:
    case PrPixelFormat_VUYA_4444_8u:
    case PrPixelFormat_VUYA_4444_8u_709:
    case PrPixelFormat_BGRP_4444_8u:
        return 4;
    case PrPixelFormat_BGRA_4444_16u:
    case PrPixelFormat_BGRX_4444_16u:
    case PrPixelFormat_ARGB_4444_16u:
    case PrPixelFormat_VUYA_4444_16u:
    case PrPixelFormat_BGRP_4444_16u:
        return 8;
    case PrPixelFormat_BGRA_4444_32f:
    case PrPixelFormat_BGRX_4444_32f:
    case PrPixelFormat_ARGB_4444_32f:
    case PrPixelFormat_VUYA_4444_32f:
    case PrPixelFormat_VUYA_4444_32f_709:
    case PrPixelFormat_BGRP_4444_32f:
    case PrPixelFormat_BGRA_4444_32f_Linear:
        return 16;
    default:
        break;
    }
    // GPU formats are macros, not enumerators.
    if (format == PrPixelFormat_GPU_BGRA_4444_16f) {
        return 8;
    }
    if (format == PrPixelFormat_GPU_BGRA_4444_32f) {
        return 16;
    }
    return 0;
}

PPixRecord* MockHost::Impl::record(PPixHand hand) const {
    if (!hand) {
        return nullptr;
    }
    // The handle is the address of PPixRecord::master (first member).
    auto* rec = reinterpret_cast<PPixRecord*>(hand);
    if (!livePPix.count(rec) || rec->magic != PPixRecord::kMagic) {
        return nullptr;
    }
    return rec;
}

PPixRecord* MockHost::Impl::createHostPPix(PrPixelFormat format, std::uint32_t width, std::uint32_t height,
                                           csSDK_uint32 parNum, csSDK_uint32 parDen, prFieldType fieldType,
                                           const PrSDKColorSpaceID& colorSpace) {
    const std::size_t bpp = bytesPerPixel(format);
    // GPU formats are never host frames.
    if (bpp == 0 || format == PrPixelFormat_GPU_BGRA_4444_16f || format == PrPixelFormat_GPU_BGRA_4444_32f) {
        return nullptr;
    }
    if (width == 0 || height == 0 || width > 32768u || height > 32768u) {
        return nullptr;
    }
    auto* rec = new (std::nothrow) PPixRecord();
    if (!rec) {
        return nullptr;
    }
    rec->width = width;
    rec->height = height;
    rec->rowBytes = alignedRowBytes(width, bpp);
    rec->format = format;
    rec->colorSpace = colorSpace;
    rec->parNum = parNum;
    rec->parDen = parDen;
    rec->fieldType = fieldType;
    rec->byteSize = static_cast<std::size_t>(rec->rowBytes) * height;
    try {
        // Over-allocate so the start can be aligned to 128 bytes.
        rec->storage.assign(rec->byteSize + 128u, 0);
    } catch (...) {
        delete rec;
        return nullptr;
    }
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(rec->storage.data());
    const std::uintptr_t aligned = (raw + 127u) & ~static_cast<std::uintptr_t>(127u);
    rec->pixels = reinterpret_cast<char*>(aligned);

    // Fill the public PPix header the way the host would (the plug-in is
    // told never to read it, but keep it truthful).
    prSetRect(&rec->header.bounds, 0, 0, static_cast<int>(width), static_cast<int>(height));
    rec->header.rowbytes = rec->rowBytes;
    rec->header.bitsperpixel = static_cast<csSDK_int32>(bpp * 8);
    rec->header.pix = rec->pixels;
    rec->master = &rec->header;
    rec->refCount = 1;
    livePPix.insert(rec);
    return rec;
}

void MockHost::Impl::retainPPix(PPixRecord* rec) {
    if (rec) {
        ++rec->refCount;
    }
}

void MockHost::Impl::releasePPix(PPixRecord* rec) {
    if (!rec || !livePPix.count(rec)) {
        return;
    }
    if (--rec->refCount > 0) {
        return;
    }
    livePPix.erase(rec);
    if (rec->isGpu && rec->devicePtr) {
        // Device memory belongs to the GPU suite; it frees through the
        // same path AllocateDeviceMemory uses.
        if (gpu.FreeDeviceMemory) {
            gpu.FreeDeviceMemory(rec->deviceIndex, rec->devicePtr);
        }
        rec->devicePtr = nullptr;
    }
    rec->magic = 0;
    delete rec;
}

void MockHost::Impl::clearCacheLocked() {
    for (auto& entry : cacheMap) {
        releasePPix(entry.second.first);
    }
    cacheMap.clear();
    cacheOrder.clear();
}

// -----------------------------------------------------------------------------
//  Registration
// -----------------------------------------------------------------------------
void installPPixSuites(MockHost::Impl& p) {
    p.ppix.Dispose = &ppixDispose;
    p.ppix.GetPixels = &ppixGetPixels;
    p.ppix.GetBounds = &ppixGetBounds;
    p.ppix.GetRowBytes = &ppixGetRowBytes;
    p.ppix.GetPixelAspectRatio = &ppixGetPixelAspectRatio;
    p.ppix.GetPixelFormat = &ppixGetPixelFormat;
    p.ppix.GetUniqueKey = &ppixGetUniqueKey;
    p.ppix.GetUniqueKeySize = &ppixGetUniqueKeySize;
    p.ppix.GetRenderTime = &ppixGetRenderTime;
    p.registerSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, &p.ppix);

    p.ppix2.GetSize = &ppix2GetSize;
    p.ppix2.GetYUV420PlanarBuffers = &ppix2GetYUV420PlanarBuffers;
    p.ppix2.GetOrigin = &ppix2GetOrigin;
    p.ppix2.GetFieldOrder = &ppix2GetFieldOrder;
    p.registerSuite(kPrSDKPPix2Suite, kPrSDKPPix2SuiteVersion3, &p.ppix2);

    p.creator.CreatePPix = &creatorCreatePPix;
    p.creator.ClonePPix = &creatorClonePPix;
    p.registerSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion, &p.creator);

    p.creator2.CreatePPix = &creator2CreatePPix;
    p.creator2.CreateRawPPix = &creator2CreateRawPPix;
    p.creator2.CreateCustomPPix = &creator2CreateCustomPPix;
    p.creator2.CreateDiskAlignedPPix = &creator2CreateDiskAlignedPPix;
    p.creator2.CreateColorManagedPPix = &creator2CreateColorManagedPPix;
    p.creator2.CreateColorManagedCustomPPix = &creator2CreateColorManagedCustomPPix;
    p.creator2.CreateColorManagedDiskAlignedPPix = &creator2CreateColorManagedDiskAlignedPPix;
    p.registerSuite(kPrSDKPPixCreator2Suite, kPrSDKPPixCreator2SuiteVersion4, &p.creator2);

    p.cache.AddFrameToCache = &cacheAddFrame;
    p.cache.GetFrameFromCache = &cacheGetFrame;
    p.cache.AddRawPPixToCache = &cacheAddRaw;
    p.cache.GetRawPPixFromCache = &cacheGetRaw;
    p.cache.AddNamedPPixToCache = &cacheAddNamed;
    p.cache.GetNamedPPixFromCache = &cacheGetNamed;
    p.cache.RegisterDependencyOnNamedPPix = &cacheRegisterNamed;
    p.cache.UnregisterDependencyOnNamedPPix = &cacheUnregisterNamed;
    p.cache.ExpireNamedPPixFromCache = &cacheExpireNamed;
    p.cache.ExpireAllPPixesFromCache = &cacheExpireAll;
    p.cache.AddFrameToCacheWithColorProfile = &cacheAddWithProfile;
    p.cache.GetFrameFromCacheWithColorProfile = &cacheGetWithProfile;
    p.cache.AddFrameToCacheWithColorProfile2 = &cacheAddWithProfile2;
    p.cache.GetFrameFromCacheWithColorProfile2 = &cacheGetWithProfile2;
    p.cache.AddFrameToCacheWithColorSpace = &cacheAddWithColorSpace;
    p.cache.GetFrameFromCacheWithColorSpace = &cacheGetWithColorSpace;
    p.cache.RegisterDependencyOnFrame = &cacheRegisterDependency;
    p.cache.RegisterDependencyOnFrameWithColorProfile = &cacheRegisterDependencyProfile;
    p.cache.RegisterDependencyOnFrameWithColorProfile2 = &cacheRegisterDependencyProfile2;
    p.cache.RegisterDependencyOnFrameWithColorSpace = &cacheRegisterDependencyColorSpace;
    // v7 (Premiere 13.0) introduced the colour-space calls; the entries a
    // plug-in written against v7 uses sit at the same offsets in the v8
    // table, so both versions are served by one struct.
    p.registerSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion8, &p.cache);
    p.registerSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion7, &p.cache);
}

}  // namespace osv::premiere::mock
