// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Time, String, App Info, Error, Color Management, Memory Manager, Importer
// File Manager, Sequence Info and Video Segment suite implementations.

#include "MockHostImpl.h"

#include "PrSDKColorProfile.h"
#include "PrSDKColorSpaces.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace osv::premiere::mock {

namespace {

MockHost::Impl* impl() noexcept {
    MockHost* h = MockHost::current();
    return h ? h->implForSuites() : nullptr;
}

/// Tag placed in opaque[1] of every string / colour-space id the mock mints
/// so a foreign or zeroed value is recognised.
constexpr csSDK_int64 kStringTag = 0x4F53565F53545247ll;  // "OSV_STRG"
constexpr csSDK_int64 kColorTag = 0x4F53565F434F4C52ll;   // "OSV_COLR"

std::wstring utf8ToUtf16(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string utf16ToUtf8(const wchar_t* s) {
    if (!s || !*s) {
        return {};
    }
    const int len = static_cast<int>(std::wcslen(s));
    const int needed = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), needed, nullptr, nullptr);
    return out;
}

// -----------------------------------------------------------------------------
//  Time Suite v1
// -----------------------------------------------------------------------------
prSuiteError timeGetTicksPerSecond(PrTime* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    *out = kTicksPerSecond;
    return suiteError_NoError;
}

prSuiteError timeGetTicksPerVideoFrame(PrVideoFrameRates rate, PrTime* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    // ticks * num / den with the rate as frames per second = den / num.
    switch (rate) {
    case kVideoFrameRate_24Drop: *out = kTicksPerSecond * 1001 / 24000; return suiteError_NoError;
    case kVideoFrameRate_24: *out = kTicksPerSecond / 24; return suiteError_NoError;
    case kVideoFrameRate_PAL: *out = kTicksPerSecond / 25; return suiteError_NoError;
    case kVideoFrameRate_NTSC: *out = kTicksPerSecond * 1001 / 30000; return suiteError_NoError;
    case kVideoFrameRate_30: *out = kTicksPerSecond / 30; return suiteError_NoError;
    case kVideoFrameRate_PAL_HD: *out = kTicksPerSecond / 50; return suiteError_NoError;
    case kVideoFrameRate_NTSC_HD: *out = kTicksPerSecond * 1001 / 60000; return suiteError_NoError;
    case kVideoFrameRate_60: *out = kTicksPerSecond / 60; return suiteError_NoError;
    default: break;
    }
    *out = 0;
    return suiteError_InvalidParms;
}

prSuiteError timeGetTicksPerAudioSample(float sampleRate, PrTime* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    if (!(sampleRate > 0.0f)) {
        *out = 0;
        return suiteError_InvalidParms;
    }
    const double exact = static_cast<double>(kTicksPerSecond) / static_cast<double>(sampleRate);
    const PrTime rounded = static_cast<PrTime>(std::llround(exact));
    *out = rounded;
    // The real host reports when the rate does not divide the tick rate.
    return std::fabs(exact - static_cast<double>(rounded)) < 1e-6 ? suiteError_NoError : suiteError_TimeRoundedAudioRate;
}

// -----------------------------------------------------------------------------
//  MediaCore StringSuite v1
// -----------------------------------------------------------------------------
PrSDKString allocString(MockHost::Impl& p, std::string utf8, std::wstring utf16) {
    PrSDKString s{};
    const csSDK_int64 id = p.nextStringId++;
    StringRecord rec;
    rec.utf8 = std::move(utf8);
    rec.utf16 = std::move(utf16);
    p.strings.emplace(id, std::move(rec));
    s.opaque[0] = id;
    s.opaque[1] = kStringTag;
    return s;
}

const StringRecord* findString(MockHost::Impl& p, const PrSDKString* s) {
    if (!s || s->opaque[1] != kStringTag) {
        return nullptr;
    }
    auto it = p.strings.find(s->opaque[0]);
    return it == p.strings.end() ? nullptr : &it->second;
}

prSuiteError stringDispose(const PrSDKString* s) {
    MockHost::Impl* p = impl();
    if (!p || !s) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    // A zero string (never allocated) is a legal no-op, like the host.
    if (s->opaque[0] == 0 && s->opaque[1] == 0) {
        return suiteError_NoError;
    }
    if (s->opaque[1] != kStringTag || p->strings.erase(s->opaque[0]) == 0) {
        return suiteError_InvalidParms;
    }
    return suiteError_NoError;
}

prSuiteError stringAllocateFromUTF8(const prUTF8Char* text, PrSDKString* out) {
    MockHost::Impl* p = impl();
    if (!p || !text || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const std::string utf8(reinterpret_cast<const char*>(text));
    *out = allocString(*p, utf8, utf8ToUtf16(utf8));
    return suiteError_NoError;
}

prSuiteError stringCopyToUTF8(const PrSDKString* s, prUTF8Char* buffer, csSDK_uint32* ioElements) {
    MockHost::Impl* p = impl();
    if (!p || !ioElements) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const StringRecord* rec = findString(*p, s);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    const csSDK_uint32 needed = static_cast<csSDK_uint32>(rec->utf8.size() + 1);
    if (!buffer || *ioElements < needed) {
        *ioElements = needed;
        return suiteError_StringBufferTooSmall;
    }
    std::memcpy(buffer, rec->utf8.c_str(), needed);
    *ioElements = needed;
    return suiteError_NoError;
}

prSuiteError stringAllocateFromUTF16(const prUTF16Char* text, PrSDKString* out) {
    MockHost::Impl* p = impl();
    if (!p || !text || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    *out = allocString(*p, utf16ToUtf8(text), std::wstring(text));
    return suiteError_NoError;
}

prSuiteError stringCopyToUTF16(const PrSDKString* s, prUTF16Char* buffer, csSDK_uint32* ioElements) {
    MockHost::Impl* p = impl();
    if (!p || !ioElements) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const StringRecord* rec = findString(*p, s);
    if (!rec) {
        return suiteError_InvalidParms;
    }
    const csSDK_uint32 needed = static_cast<csSDK_uint32>(rec->utf16.size() + 1);
    if (!buffer || *ioElements < needed) {
        *ioElements = needed;
        return suiteError_StringBufferTooSmall;
    }
    std::memcpy(buffer, rec->utf16.c_str(), needed * sizeof(wchar_t));
    *ioElements = needed;
    return suiteError_NoError;
}

// -----------------------------------------------------------------------------
//  App Info Suite v3
// -----------------------------------------------------------------------------
prSuiteError appInfoGet(int selector, void* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    switch (selector) {
    case PrSDKAppInfoSuite::kAppInfo_AppFourCC:
        *static_cast<csSDK_uint32*>(out) = p->identity.fourcc;
        return suiteError_NoError;
    case PrSDKAppInfoSuite::kAppInfo_Version: {
        auto* v = static_cast<VersionInfo*>(out);
        v->major = p->identity.major;
        v->minor = p->identity.minor;
        v->patch = p->identity.patch;
        return suiteError_NoError;
    }
    case PrSDKAppInfoSuite::kAppInfo_Build:
        *static_cast<csSDK_uint32*>(out) = p->identity.build;
        return suiteError_NoError;
    case PrSDKAppInfoSuite::kAppInfo_Language: {
        auto* l = static_cast<LanguageInfo*>(out);
        std::memset(l->languageID, 0, sizeof(l->languageID));
        std::memcpy(l->languageID, p->identity.language.c_str(),
                    std::min(sizeof(l->languageID) - 1, p->identity.language.size()));
        return suiteError_NoError;
    }
    default:
        break;
    }
    return suiteError_InvalidParms;
}

// -----------------------------------------------------------------------------
//  Error Suite v3
// -----------------------------------------------------------------------------
prSuiteError errorSetEventStringUnicode(csSDK_uint32 type, prUTF16Char* title, prUTF16Char* description) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    ErrorEvent e;
    e.type = type;
    e.title = title ? std::wstring(title) : std::wstring();
    e.description = description ? std::wstring(description) : std::wstring();
    p->events.push_back(std::move(e));
    return suiteError_NoError;
}

// -----------------------------------------------------------------------------
//  Color Management Suite v1
// -----------------------------------------------------------------------------

/// Predefined colour spaces and the SEI triplet each maps to.  Numbers are
/// the PrColorPrimaries / PrTransferCharacteristic / PrMatrixEquations codes
/// from PrSDKColorSEICodes.h; range / RGB / scene flags follow the comments
/// in PrSDKColorSpaces.h.
struct ColorTableEntry {
    const char* token;
    csSDK_int32 primaries;
    csSDK_int32 transfer;
    csSDK_int32 matrix;
    csSDK_int32 bitDepth;
    bool fullRange;
    bool rgb;
    bool scene;
};

constexpr ColorTableEntry kColorTable[] = {
    {kPrSRGBColorSpace, 1, 13, 0, 8, true, true, false},
    {kPrRec601525ColorSpace, 6, 6, 6, 8, false, false, false},
    {kPrRec601625ColorSpace, 5, 5, 5, 8, false, false, false},
    {kPrRec709, 1, 1, 1, 8, false, false, false},
    {kPrRec709Scene, 1, 1, 1, 8, false, false, true},
    {kPrRec709RGB, 1, 1, 0, 8, true, true, false},
    {kPrRec709RGBScene, 1, 1, 0, 8, true, true, true},
    {kPrOverranged709, 1, 1, 0, 32, true, true, false},
    {kPrOverranged709Scene, 1, 1, 0, 32, true, true, true},
    {kPrOverranged709Display, 1, 1, 0, 32, true, true, false},
    {kPrRec2020, 9, 14, 9, 10, false, false, false},
    {kPrRec2020Scene, 9, 14, 9, 10, false, false, true},
    {kPrRec2020RGB, 9, 14, 0, 10, true, true, false},
    {kPrRec2020RGBScene, 9, 14, 0, 10, true, true, true},
    {kPrOverranged2020, 9, 14, 0, 32, true, true, false},
    {kPrOverranged2020Scene, 9, 14, 0, 32, true, true, true},
    {kPrOverranged2020Display, 9, 14, 0, 32, true, true, false},
    {kPrRec2100HLG, 9, 18, 9, 10, false, false, false},
    {kPrRec2100HLGScene, 9, 18, 9, 10, false, false, true},
    {kPrRec2100HLGRGB, 9, 18, 0, 10, true, true, false},
    {kPrRec2100HLGRGBScene, 9, 18, 0, 10, true, true, true},
    {kPrOverranged2100HLG, 9, 18, 0, 32, true, true, false},
    {kPrOverranged2100HLGScene, 9, 18, 0, 32, true, true, true},
    {kPrOverranged2100HLGDisplay, 9, 18, 0, 32, true, true, false},
    {kPrRec2100PQ, 9, 16, 9, 10, false, false, false},
    {kPrRec2100PQScene, 9, 16, 9, 10, false, false, true},
    {kPrRec2100PQRGB, 9, 16, 0, 10, true, true, false},
    {kPrRec2100PQRGBScene, 9, 16, 0, 10, true, true, true},
    {kPrOverranged2100PQ, 9, 16, 0, 32, true, true, false},
    {kPrOverranged2100PQScene, 9, 16, 0, 32, true, true, true},
    {kPrOverranged2100PQDisplay, 9, 16, 0, 32, true, true, false},
    {kPrDCDMXYZ, 10, 17, 0, 12, true, true, false},
    {kPrSonySGamutSLog2, 1010, 1000, 0, 10, true, true, true},
    {kPrSony2020SLog3, 9, 1001, 0, 10, true, true, true},
    {kPrSonySGamut3CineSLog3, 1012, 1001, 0, 10, true, true, true},
    {kPrSonySGamut3SLog3, 1011, 1001, 0, 10, true, true, true},
    {kPrWorkingColorSpace, 1, 1, 0, 32, true, true, false},
};

prSuiteError colorGetType(const PrSDKColorSpaceID* id, PrSDKColorSpaceType* out) {
    MockHost::Impl* p = impl();
    if (!p || !id || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    std::size_t index = 0;
    if (p->colorIndexForId(*id, &index)) {
        *out = kPrSDKColorSpaceType_Predefined;
        return suiteError_NoError;
    }
    // The invalid id is a legal question with an "undefined" answer; any
    // other unknown guid is a caller error.
    if (id->opaque[0] == 0 && id->opaque[1] == 0) {
        *out = kPrSDKColorSpaceType_Undefined;
        return suiteError_NoError;
    }
    return suiteError_InvalidParms;
}

prSuiteError colorHasIcc(const PrSDKColorSpaceID* id, prBool* out) {
    if (!id || !out) {
        return suiteError_InvalidParms;
    }
    *out = kPrFalse;
    return suiteError_NoError;
}

prSuiteError colorGetIccEquivalent(const PrSDKColorSpaceID*, PrSDKColorSpaceID* out) {
    if (out) {
        *out = kPrSDKColorSpaceID_Invalid;
    }
    return suiteError_NotImplemented;
}

prSuiteError colorGetSei(const PrSDKColorSpaceID* id, prSEIColorCodesRec* out) {
    MockHost::Impl* p = impl();
    if (!p || !id || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    std::size_t index = 0;
    if (!p->colorIndexForId(*id, &index) || index >= p->colorSei.size()) {
        return suiteError_InvalidParms;
    }
    *out = p->colorSei[index];
    return suiteError_NoError;
}

prSuiteError colorGetIccSize(const PrSDKColorSpaceID*, csSDK_int32* out) {
    if (out) {
        *out = 0;
    }
    return suiteError_NotImplemented;
}

prSuiteError colorGetIcc(const PrSDKColorSpaceID*, PrMemoryPtr) { return suiteError_NotImplemented; }

prSuiteError colorGetDisplayName(const char* token, prUTF16Char out[256]) {
    MockHost::Impl* p = impl();
    if (!p || !token || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    for (const std::string& t : p->colorTokens) {
        if (t == token) {
            const std::wstring wide = utf8ToUtf16(t);
            const std::size_t n = std::min<std::size_t>(wide.size(), 255);
            std::memcpy(out, wide.c_str(), n * sizeof(wchar_t));
            out[n] = 0;
            return suiteError_NoError;
        }
    }
    out[0] = 0;
    return suiteError_InvalidParms;
}

// -----------------------------------------------------------------------------
//  Memory Manager Suite v4
// -----------------------------------------------------------------------------
prSuiteError memoryReserve(csSDK_uint32, csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->reservedBytes = size;
    return suiteError_NoError;
}

prSuiteError memoryGetSize(csSDK_uint64* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    *out = 8ull * 1024ull * 1024ull * 1024ull;
    return suiteError_NoError;
}

prSuiteError memoryAddBlock(csSDK_size_t, PrSDKMemoryManagerSuite_PurgeMemoryFunction fn, void* data, csSDK_uint32* outId) {
    MockHost::Impl* p = impl();
    if (!p || !outId) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const csSDK_uint32 id = p->nextBlockId++;
    p->memoryBlocks[id] = std::make_pair(fn, data);
    *outId = id;
    return suiteError_NoError;
}

prSuiteError memoryTouchBlock(csSDK_uint32 id) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->memoryBlocks.count(id) ? suiteError_NoError : suiteError_IDNotValid;
}

prSuiteError memoryRemoveBlock(csSDK_uint32 id) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->memoryBlocks.erase(id) ? suiteError_NoError : suiteError_IDNotValid;
}

PrMemoryPtr memoryNewPtrClear(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->allocPtr(size, true);
}

PrMemoryPtr memoryNewPtr(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->allocPtr(size, false);
}

csSDK_uint32 memoryGetPtrSize(PrMemoryPtr ptr) {
    MockHost::Impl* p = impl();
    if (!p) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->ptrSize(ptr);
}

void memorySetPtrSize(PrMemoryPtr* ptr, csSDK_uint32 newSize) {
    MockHost::Impl* p = impl();
    if (!p || !ptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (char* grown = p->resizePtr(*ptr, newSize)) {
        *ptr = grown;
    }
}

PrMemoryHandle memoryNewHandle(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    HandleBlock* b = p->allocHandle(size, false);
    return b ? &b->master : nullptr;
}

PrMemoryHandle memoryNewHandleClear(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    HandleBlock* b = p->allocHandle(size, true);
    return b ? &b->master : nullptr;
}

void memoryDisposePtr(PrMemoryPtr ptr) {
    MockHost::Impl* p = impl();
    if (!p) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->freePtr(ptr);
}

void memoryDisposeHandle(PrMemoryHandle h) {
    MockHost::Impl* p = impl();
    if (!p) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->freeHandle(h);
}

short memorySetHandleSize(PrMemoryHandle h, csSDK_uint32 newSize) {
    MockHost::Impl* p = impl();
    if (!p) {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    HandleBlock* b = p->handleBlock(h);
    if (!b) {
        return -1;
    }
    char* grown = p->resizePtr(b->master, newSize);
    if (!grown) {
        return -1;
    }
    b->master = grown;
    b->size = newSize;
    return 0;
}

csSDK_uint32 memoryGetHandleSize(PrMemoryHandle h) {
    MockHost::Impl* p = impl();
    if (!p) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    HandleBlock* b = p->handleBlock(h);
    return b ? b->size : 0;
}

prSuiteError memoryAdjustReserved(csSDK_uint32, csSDK_int64 delta) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const csSDK_int64 next = static_cast<csSDK_int64>(p->reservedBytes) + delta;
    p->reservedBytes = next > 0 ? static_cast<csSDK_uint64>(next) : 0;
    return suiteError_NoError;
}

// -----------------------------------------------------------------------------
//  Importer File Manager Suite v4 (bookkeeping only)
// -----------------------------------------------------------------------------
prSuiteError fileManagerSetStreamFileCount(csSDK_uint32, csSDK_int32, csSDK_int32) { return suiteError_NoError; }
prSuiteError fileManagerRefreshFileAsync(const prUTF16Char* path) {
    return path ? suiteError_NoError : suiteError_InvalidParms;
}
prSuiteError fileManagerGetGrowingInterval(csSDK_int32* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    *out = 0;
    return suiteError_NoError;
}
prSuiteError fileManagerSetInstanceStreamFileCount(void*, csSDK_int32) { return suiteError_NoError; }

// -----------------------------------------------------------------------------
//  Sequence Info Suite v9
// -----------------------------------------------------------------------------

/// Fetch the config of a timeline; nullptr when the test never set it up.
const SequenceConfig* sequenceFor(MockHost::Impl& p, PrTimelineID id) {
    auto it = p.sequences.find(id);
    return it == p.sequences.end() ? nullptr : &it->second;
}

#define OSV_MOCK_SEQ_PROLOGUE(...)                                                                                     \
    MockHost::Impl* p = impl();                                                                                        \
    if (!p) {                                                                                                          \
        return suiteError_InvalidCall;                                                                                 \
    }                                                                                                                  \
    std::lock_guard<std::recursive_mutex> lock(p->mutex);                                                              \
    const SequenceConfig* cfg = sequenceFor(*p, timeline);                                                             \
    if (!cfg) {                                                                                                        \
        return suiteError_IDNotValid;                                                                                  \
    }

prSuiteError seqGetFrameRect(PrTimelineID timeline, prRect* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->frameRect;
    return suiteError_NoError;
}

prSuiteError seqGetPixelAspectRatio(PrTimelineID timeline, csSDK_uint32* num, csSDK_uint32* den) {
    if (!num || !den) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *num = cfg->parNum;
    *den = cfg->parDen;
    return suiteError_NoError;
}

prSuiteError seqGetFrameRate(PrTimelineID timeline, PrTime* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->ticksPerFrame;
    return suiteError_NoError;
}

prSuiteError seqGetFieldType(PrTimelineID timeline, prFieldType* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->fieldType;
    return suiteError_NoError;
}

prSuiteError seqGetZeroPoint(PrTimelineID timeline, PrTime* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->zeroPoint;
    return suiteError_NoError;
}

prSuiteError seqGetTimecodeDropFrame(PrTimelineID timeline, prBool* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->dropFrame ? kPrTrue : kPrFalse;
    return suiteError_NoError;
}

prSuiteError seqGetProxyFlag(PrTimelineID timeline, prBool* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->proxy ? kPrTrue : kPrFalse;
    return suiteError_NoError;
}

prSuiteError seqGetVRConfiguration(PrTimelineID timeline, PrIVProjectionType* projection, PrIVFrameLayout* layout,
                                   csSDK_uint32* hView, csSDK_uint32* vView) {
    if (!projection || !layout || !hView || !vView) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *projection = cfg->projection;
    *layout = cfg->layout;
    *hView = cfg->horizontalView;
    *vView = cfg->verticalView;
    return suiteError_NoError;
}

prSuiteError seqGetWorkingColorSpace(PrTimelineID timeline, PrSDKColorSpaceID* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->workingColorSpace;
    return suiteError_NoError;
}

prSuiteError seqGetGraphicsWhite(PrTimelineID timeline, csSDK_uint32* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->graphicsWhiteLuminance;
    return suiteError_NoError;
}

prSuiteError seqGetLutInterpolation(PrTimelineID timeline, csSDK_uint32* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->lutInterpolation;
    return suiteError_NoError;
}

prSuiteError seqGetAutoToneMap(PrTimelineID timeline, prBool* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->autoToneMap ? kPrTrue : kPrFalse;
    return suiteError_NoError;
}

prSuiteError seqGetSdrGamma(PrTimelineID timeline, csSDK_uint32* out) {
    if (!out) {
        return suiteError_InvalidParms;
    }
    OSV_MOCK_SEQ_PROLOGUE()
    *out = cfg->sdrGamma;
    return suiteError_NoError;
}

#undef OSV_MOCK_SEQ_PROLOGUE

// -----------------------------------------------------------------------------
//  Video Segment Suite (v9 table; v6..v8 are prefixes of it)
// -----------------------------------------------------------------------------

/// Linear interpolation of two PrParams at fraction t in [0, 1].  Floats and
/// points blend; integers, bools, guids and memory pointers hold the left
/// keyframe (Premiere holds popups and checkboxes between keyframes).
PrParam interpolate(const PrParam& a, const PrParam& b, double t) {
    PrParam out = a;
    if (a.mType != b.mType) {
        return out;
    }
    switch (a.mType) {
    case kPrParamType_Float32:
        out.mFloat32 = static_cast<float>(a.mFloat32 + (b.mFloat32 - a.mFloat32) * t);
        break;
    case kPrParamType_Float64:
        out.mFloat64 = a.mFloat64 + (b.mFloat64 - a.mFloat64) * t;
        break;
    case kPrParamType_Point:
        out.mPoint.x = a.mPoint.x + (b.mPoint.x - a.mPoint.x) * t;
        out.mPoint.y = a.mPoint.y + (b.mPoint.y - a.mPoint.y) * t;
        break;
    default:
        break;  // hold
    }
    return out;
}

/// Append one GetParam call to the inspection log (bounded, see the Impl).
void recordParamRead(MockHost::Impl& p, csSDK_int32 nodeId, csSDK_int32 index, PrTime time, prSuiteError result) {
    if (p.paramReads.size() >= MockHost::Impl::kMaxParamReads) {
        ++p.paramReadsDropped;
        return;
    }
    ParamReadRecord r;
    r.nodeId = nodeId;
    r.index = index;
    r.time = time;
    r.result = result;
    p.paramReads.push_back(r);
}

/// The value of a (node, index) track at `time`, or the error a real host
/// would give.  Split out of vsGetParam so every exit is recorded once.
prSuiteError readParamLocked(MockHost::Impl& p, csSDK_int32 nodeId, csSDK_int32 index, PrTime time, PrParam* out) {
    auto nodeIt = p.nodes.find(nodeId);
    if (nodeIt == p.nodes.end()) {
        return suiteError_IDNotValid;
    }
    // An injected failure wins over the keyframes, at every time or only at
    // the one time it names.
    const auto failIt = nodeIt->second.readFailures.find(index);
    if (failIt != nodeIt->second.readFailures.end()) {
        const ParamReadFailure& f = failIt->second;
        if (!f.onlyAtTime || f.time == time) {
            return f.error;
        }
    }
    auto trackIt = nodeIt->second.params.find(index);
    if (trackIt == nodeIt->second.params.end() || trackIt->second.keys.empty()) {
        return suiteError_InvalidParms;
    }
    const auto& keys = trackIt->second.keys;
    // Before the first keyframe / after the last: clamp.
    auto upper = keys.upper_bound(time);
    if (upper == keys.begin()) {
        *out = upper->second;
        return suiteError_NoError;
    }
    if (upper == keys.end()) {
        *out = std::prev(upper)->second;
        return suiteError_NoError;
    }
    auto lower = std::prev(upper);
    const double span = static_cast<double>(upper->first - lower->first);
    const double t = span > 0.0 ? static_cast<double>(time - lower->first) / span : 0.0;
    *out = interpolate(lower->second, upper->second, t);
    return suiteError_NoError;
}

prSuiteError vsGetParam(csSDK_int32 nodeId, csSDK_int32 index, PrTime time, PrParam* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const prSuiteError result = readParamLocked(*p, nodeId, index, time, out);
    recordParamRead(*p, nodeId, index, time, result);
    return result;
}

prSuiteError vsGetParamCount(csSDK_int32 nodeId, csSDK_int32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    auto nodeIt = p->nodes.find(nodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    // A test that pinned the count (a host reporting entries it cannot
    // serve) gets exactly that answer.
    if (nodeIt->second.paramCountOverride >= 0) {
        *out = nodeIt->second.paramCountOverride;
        return suiteError_NoError;
    }
    csSDK_int32 count = 0;
    for (const auto& track : nodeIt->second.params) {
        count = std::max(count, track.first + 1);
    }
    *out = count;
    return suiteError_NoError;
}

prSuiteError vsGetNodeProperty(csSDK_int32 nodeId, const char* key, PrMemoryPtr* out) {
    MockHost::Impl* p = impl();
    if (!p || !key || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    auto nodeIt = p->nodes.find(nodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    auto propIt = nodeIt->second.properties.find(key);
    if (propIt == nodeIt->second.properties.end()) {
        return suiteError_InvalidParms;
    }
    // The plug-in frees the value with the Memory Manager's PrDisposePtr,
    // so it must come from the same allocator.
    const std::string& value = propIt->second;
    char* buffer = p->allocPtr(static_cast<std::uint32_t>(value.size() + 1), true);
    if (!buffer) {
        return suiteError_OutOfMemory;
    }
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    *out = buffer;
    return suiteError_NoError;
}

prSuiteError vsGetNodeInfo(csSDK_int32 nodeId, char* outType, prPluginID* outHash, csSDK_int32* outFlags) {
    MockHost::Impl* p = impl();
    if (!p || !outType) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const auto nodeIt = p->nodes.find(nodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    // The node's modelled type, or the effect type every node reported
    // before the segment graph existed.  Truncated to the documented buffer
    // size (kMaxNodeTypeStringSize, terminator included).
    std::memset(outType, 0, kMaxNodeTypeStringSize);
    const std::string& type = nodeIt->second.type;
    if (type.empty()) {
        std::memcpy(outType, kVideoSegment_NodeType_Effect, sizeof(kVideoSegment_NodeType_Effect));
    } else {
        const std::size_t n = std::min<std::size_t>(type.size(), kMaxNodeTypeStringSize - 1);
        std::memcpy(outType, type.data(), n);
    }
    if (outHash) {
        std::memset(outHash->mGUID, 0, sizeof(outHash->mGUID));
        std::snprintf(outHash->mGUID, sizeof(outHash->mGUID), "%08x-0000-0000-0000-000000000000",
                      static_cast<unsigned>(nodeId));
    }
    if (outFlags) {
        *outFlags = 0;
    }
    return suiteError_NoError;
}

prSuiteError vsTransformNodeTime(csSDK_int32 nodeId, PrTime time, PrTime* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const auto nodeIt = p->nodes.find(nodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    const NodeRecord& node = nodeIt->second;
    if (node.transformError != suiteError_NoError) {
        return node.transformError;
    }
    // media = origin + time * num / den, in integer ticks.  The product is
    // checked before it is formed: a wrapped int64 would hand the plug-in a
    // plausible-looking but wrong media time, which is exactly the kind of
    // failure a mapping test must never manufacture by itself.
    if (node.rateDen <= 0) {
        return suiteError_InvalidParms;
    }
    const std::int64_t num = node.rateNum;
    const std::int64_t magnitude = num < 0 ? -num : num;
    const std::int64_t absTime = time < 0 ? -time : time;
    if (magnitude != 0 && absTime > std::numeric_limits<std::int64_t>::max() / magnitude) {
        return suiteError_InvalidParms;
    }
    const std::int64_t scaled = (time * num) / node.rateDen;
    *out = node.timeOrigin + scaled;
    return suiteError_NoError;
}

prSuiteError vsGetNodeTimeScale(csSDK_int32 nodeId, PrTime, double* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const auto nodeIt = p->nodes.find(nodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    // The instantaneous rate of the transform above ("the rate of change of
    // TransformNodeTime"), negative for a reversed clip.
    const NodeRecord& node = nodeIt->second;
    *out = node.rateDen > 0 ? static_cast<double>(node.rateNum) / static_cast<double>(node.rateDen) : 1.0;
    return suiteError_NoError;
}

// -----------------------------------------------------------------------------
//  Segment-graph walk (operator -> owner -> inputs, properties, releases)
// -----------------------------------------------------------------------------

prSuiteError vsAcquireOperatorOwner(csSDK_int32 operatorNodeId, csSDK_int32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = 0;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const auto nodeIt = p->nodes.find(operatorNodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    // No owner modelled: the call fails, as it would for a node that is not
    // an operator of anything.
    const csSDK_int32 owner = nodeIt->second.owner;
    const auto ownerIt = owner != 0 ? p->nodes.find(owner) : p->nodes.end();
    if (ownerIt == p->nodes.end()) {
        return suiteError_InvalidParms;
    }
    ++ownerIt->second.refs;
    *out = owner;
    return suiteError_NoError;
}

prSuiteError vsGetNodeInputCount(csSDK_int32 nodeId, csSDK_int32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = 0;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const auto nodeIt = p->nodes.find(nodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    *out = static_cast<csSDK_int32>(nodeIt->second.inputs.size());
    return suiteError_NoError;
}

prSuiteError vsAcquireInputNodeId(csSDK_int32 nodeId, csSDK_int32 index, PrTime* outOffset, csSDK_int32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = 0;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const auto nodeIt = p->nodes.find(nodeId);
    if (nodeIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    const std::vector<NodeInput>& inputs = nodeIt->second.inputs;
    if (index < 0 || static_cast<std::size_t>(index) >= inputs.size()) {
        return suiteError_InvalidParms;
    }
    const NodeInput& input = inputs[static_cast<std::size_t>(index)];
    const auto inputIt = p->nodes.find(input.node);
    if (inputIt == p->nodes.end()) {
        return suiteError_IDNotValid;
    }
    ++inputIt->second.refs;
    if (outOffset) {
        *outOffset = input.offset;
    }
    *out = input.node;
    return suiteError_NoError;
}

prSuiteError vsReleaseNodeId(csSDK_int32 nodeId) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const auto nodeIt = p->nodes.find(nodeId);
    // Releasing what was never acquired (or twice) is a plug-in bug; it is
    // counted so a test can assert there were none, and refused.
    if (nodeIt == p->nodes.end() || nodeIt->second.refs <= 0) {
        ++p->invalidNodeReleases;
        return suiteError_InvalidParms;
    }
    --nodeIt->second.refs;
    return suiteError_NoError;
}

prSuiteError vsIterateNodeProperties(csSDK_int32 nodeId, SegmentNodePropertyCallback callback,
                                     csSDK_int32 pluginObject) {
    MockHost::Impl* p = impl();
    if (!p || !callback) {
        return suiteError_InvalidParms;
    }
    // Copy the properties out and call back WITHOUT the host lock: the
    // callback is plug-in code and may call into other suites.
    std::vector<std::pair<std::string, std::string>> properties;
    {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        const auto nodeIt = p->nodes.find(nodeId);
        if (nodeIt == p->nodes.end()) {
            return suiteError_IDNotValid;
        }
        properties.assign(nodeIt->second.properties.begin(), nodeIt->second.properties.end());
    }
    for (const auto& [key, value] : properties) {
        const prSuiteError err =
            callback(pluginObject, key.c_str(), reinterpret_cast<const prUTF8Char*>(value.c_str()));
        if (err != suiteError_NoError) {
            return err;  // the plug-in asked to stop
        }
    }
    return suiteError_NoError;
}

// Everything the plug-ins do not use answers NotImplemented (never a null
// function pointer, so a stray call cannot crash the test process).
prSuiteError vsAcquireSegmentsId(PrTimelineID, csSDK_int32* out) {
    if (out) {
        *out = 0;
    }
    return suiteError_NotImplemented;
}
prSuiteError vsReleaseSegmentsId(csSDK_int32) { return suiteError_NotImplemented; }
prSuiteError vsGetHash(csSDK_int32, prPluginID*) { return suiteError_NotImplemented; }
prSuiteError vsGetSegmentCount(csSDK_int32, csSDK_int32* out) {
    if (out) {
        *out = 0;
    }
    return suiteError_NotImplemented;
}
prSuiteError vsGetSegmentInfo(csSDK_int32, csSDK_int32, PrTime*, PrTime*, PrTime*, prPluginID*) {
    return suiteError_NotImplemented;
}
prSuiteError vsAcquireNodeId(csSDK_int32, prPluginID*, csSDK_int32*) { return suiteError_NotImplemented; }
prSuiteError vsGetNodeOperatorCount(csSDK_int32, csSDK_int32* out) {
    if (out) {
        *out = 0;
    }
    return suiteError_NotImplemented;
}
prSuiteError vsAcquireOperatorNodeId(csSDK_int32, csSDK_int32, csSDK_int32*) { return suiteError_NotImplemented; }
prSuiteError vsGetNextKeyframeTime(csSDK_int32, csSDK_int32, PrTime, PrTime*, csSDK_int32*) {
    return suiteError_NotImplemented;
}
prSuiteError vsGetSegmentsProperties(PrTimelineID, prRect*, csSDK_int32*, csSDK_int32*, PrTime*, prFieldType*) {
    return suiteError_NotImplemented;
}
prSuiteError vsAcquireNodeForTime(csSDK_int32, PrTime, csSDK_int32*, PrTime*) { return suiteError_NotImplemented; }
prSuiteError vsAcquireSegmentsIdLabel(PrTimelineID, PrSDKStreamLabel, csSDK_int32*) { return suiteError_NotImplemented; }
prSuiteError vsAcquireFirstNodeInRange(csSDK_int32, PrTime, PrTime, csSDK_int32*, PrTime*) {
    return suiteError_NotImplemented;
}
prSuiteError vsGetGraphicsTransformedParams(csSDK_int32, PrTime, prFPoint64*, prFPoint64*, prFPoint64*, float*) {
    return suiteError_NotImplemented;
}
prSuiteError vsHasGraphicsGroup(csSDK_int32, bool* out) {
    if (out) {
        *out = false;
    }
    return suiteError_NotImplemented;
}
prSuiteError vsGetGraphicsGroupId(csSDK_int32, csSDK_int32*) { return suiteError_NotImplemented; }
prSuiteError vsGetSegmentsPropertiesExt(PrTimelineID, prRect*, csSDK_int32*, csSDK_int32*, PrTime*, prFieldType*,
                                        PrSDKColorSpaceID*) {
    return suiteError_NotImplemented;
}
prSuiteError vsAcquireFirstNodeInRangeExt(csSDK_int32, PrTime, PrTime, csSDK_int32*, PrTime*, PrTime*, PrTime*) {
    return suiteError_NotImplemented;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Impl helpers for colour ids
// -----------------------------------------------------------------------------
PrSDKColorSpaceID MockHost::Impl::colorIdForIndex(std::size_t index) const {
    PrSDKColorSpaceID id{};
    if (index >= colorTokens.size()) {
        return kPrSDKColorSpaceID_Invalid;
    }
    id.opaque[0] = static_cast<csSDK_int64>(index) + 1;
    id.opaque[1] = kColorTag;
    return id;
}

bool MockHost::Impl::colorIndexForId(const PrSDKColorSpaceID& id, std::size_t* index) const {
    if (id.opaque[1] != kColorTag || id.opaque[0] <= 0) {
        return false;
    }
    const std::size_t i = static_cast<std::size_t>(id.opaque[0] - 1);
    if (i >= colorTokens.size()) {
        return false;
    }
    if (index) {
        *index = i;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  MockHost API backed by this file
// -----------------------------------------------------------------------------
PrSDKString MockHost::makeString(std::string_view utf8Text) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const std::string utf8(utf8Text);
    return allocString(*m_impl, utf8, utf8ToUtf16(utf8));
}

PrSDKColorSpaceID MockHost::colorSpaceId(std::string_view predefinedName) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    for (std::size_t i = 0; i < m_impl->colorTokens.size(); ++i) {
        if (m_impl->colorTokens[i] == predefinedName) {
            return m_impl->colorIdForIndex(i);
        }
    }
    return kPrSDKColorSpaceID_Invalid;
}

std::string MockHost::predefinedName(const PrSDKColorSpaceID& id) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    std::size_t index = 0;
    if (!m_impl->colorIndexForId(id, &index)) {
        return {};
    }
    return m_impl->colorTokens[index];
}

prSEIColorCodesRec MockHost::seiCodes(std::string_view predefinedName) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    for (std::size_t i = 0; i < m_impl->colorTokens.size(); ++i) {
        if (m_impl->colorTokens[i] == predefinedName) {
            return m_impl->colorSei[i];
        }
    }
    return prSEIColorCodesRec();
}

// -----------------------------------------------------------------------------
//  Registration
// -----------------------------------------------------------------------------
void installMiscSuites(MockHost::Impl& p) {
    p.time.GetTicksPerSecond = &timeGetTicksPerSecond;
    p.time.GetTicksPerVideoFrame = &timeGetTicksPerVideoFrame;
    p.time.GetTicksPerAudioSample = &timeGetTicksPerAudioSample;
    p.registerSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion, &p.time);

    p.string.DisposeString = &stringDispose;
    p.string.AllocateFromUTF8 = &stringAllocateFromUTF8;
    p.string.CopyToUTF8String = &stringCopyToUTF8;
    p.string.AllocateFromUTF16 = &stringAllocateFromUTF16;
    p.string.CopyToUTF16String = &stringCopyToUTF16;
    p.registerSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion, &p.string);

    p.appInfo.GetAppInfo = &appInfoGet;
    p.registerSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion, &p.appInfo);

    p.error.SetEventStringUnicode = &errorSetEventStringUnicode;
    p.registerSuite(kPrSDKErrorSuite, kPrSDKErrorSuiteVersion3, &p.error);

    // Colour table -> ids.
    for (const ColorTableEntry& e : kColorTable) {
        p.colorTokens.emplace_back(e.token);
        p.colorSei.emplace_back(e.primaries, e.transfer, e.matrix, e.bitDepth, e.fullRange ? kPrTrue : kPrFalse,
                                e.rgb ? kPrTrue : kPrFalse, e.scene ? kPrTrue : kPrFalse);
    }
    p.color.GetColorSpaceTypeForColorSpace = &colorGetType;
    p.color.HasICCEquivalentColorSpaceForColorSpace = &colorHasIcc;
    p.color.GetICCEquivalentColorSpaceForColorSpace = &colorGetIccEquivalent;
    p.color.GetSEIColorCodesForColorSpace = &colorGetSei;
    p.color.GetICCProfileSizeForColorSpace = &colorGetIccSize;
    p.color.GetICCProfileForColorSpace = &colorGetIcc;
    p.color.GetDisplayNameForPredefinedColorSpace = &colorGetDisplayName;
    p.registerSuite(kPrSDKColorManagementSuite, kPrSDKColorManagementSuiteVersion1, &p.color);

    p.memory.ReserveMemory = &memoryReserve;
    p.memory.GetMemoryManagerSize = &memoryGetSize;
    p.memory.AddBlock = &memoryAddBlock;
    p.memory.TouchBlock = &memoryTouchBlock;
    p.memory.RemoveBlock = &memoryRemoveBlock;
    p.memory.NewPtrClear = &memoryNewPtrClear;
    p.memory.NewPtr = &memoryNewPtr;
    p.memory.GetPtrSize = &memoryGetPtrSize;
    p.memory.SetPtrSize = &memorySetPtrSize;
    p.memory.NewHandle = &memoryNewHandle;
    p.memory.NewHandleClear = &memoryNewHandleClear;
    p.memory.PrDisposePtr = &memoryDisposePtr;
    p.memory.DisposeHandle = &memoryDisposeHandle;
    p.memory.SetHandleSize = &memorySetHandleSize;
    p.memory.GetHandleSize = &memoryGetHandleSize;
    p.memory.AdjustReservedMemorySize = &memoryAdjustReserved;
    p.registerSuite(kPrSDKMemoryManagerSuite, kPrSDKMemoryManagerSuiteVersion4, &p.memory);

    p.fileManager.SetImporterStreamFileCount = &fileManagerSetStreamFileCount;
    p.fileManager.RefreshFileAsync = &fileManagerRefreshFileAsync;
    p.fileManager.GetGrowingFileRefreshInterval = &fileManagerGetGrowingInterval;
    p.fileManager.SetImporterInstanceStreamFileCount = &fileManagerSetInstanceStreamFileCount;
    p.registerSuite(kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion, &p.fileManager);

    p.sequenceInfo.GetFrameRect = &seqGetFrameRect;
    p.sequenceInfo.GetPixelAspectRatio = &seqGetPixelAspectRatio;
    p.sequenceInfo.GetFrameRate = &seqGetFrameRate;
    p.sequenceInfo.GetFieldType = &seqGetFieldType;
    p.sequenceInfo.GetZeroPoint = &seqGetZeroPoint;
    p.sequenceInfo.GetTimecodeDropFrame = &seqGetTimecodeDropFrame;
    p.sequenceInfo.GetProxyFlag = &seqGetProxyFlag;
    p.sequenceInfo.GetImmersiveVideoVRConfiguration = &seqGetVRConfiguration;
    p.sequenceInfo.GetWorkingColorSpace = &seqGetWorkingColorSpace;
    p.sequenceInfo.GetGraphicsWhiteLuminance = &seqGetGraphicsWhite;
    p.sequenceInfo.GetLUTInterpolationMethod = &seqGetLutInterpolation;
    p.sequenceInfo.GetAutoToneMapEnabled = &seqGetAutoToneMap;
    p.sequenceInfo.GetSDRGamma = &seqGetSdrGamma;
    // v5 (CC 2015.3) added the VR call; v9 is what the header defines.  The
    // v5..v8 tables are prefixes of v9, so one struct serves them all.
    for (int v = 5; v <= kPrSDKSequenceInfoSuiteVersion; ++v) {
        p.registerSuite(kPrSDKSequenceInfoSuite, v, &p.sequenceInfo);
    }

    p.videoSegment.AcquireVideoSegmentsID = &vsAcquireSegmentsId;
    p.videoSegment.AcquireVideoSegmentsWithPreviewsID = &vsAcquireSegmentsId;
    p.videoSegment.AcquireVideoSegmentsWithOpaquePreviewsID = &vsAcquireSegmentsId;
    p.videoSegment.ReleaseVideoSegmentsID = &vsReleaseSegmentsId;
    p.videoSegment.GetHash = &vsGetHash;
    p.videoSegment.GetSegmentCount = &vsGetSegmentCount;
    p.videoSegment.GetSegmentInfo = &vsGetSegmentInfo;
    p.videoSegment.AcquireNodeID = &vsAcquireNodeId;
    p.videoSegment.ReleaseVideoNodeID = &vsReleaseNodeId;
    p.videoSegment.GetNodeInfo = &vsGetNodeInfo;
    p.videoSegment.GetNodeInputCount = &vsGetNodeInputCount;
    p.videoSegment.AcquireInputNodeID = &vsAcquireInputNodeId;
    p.videoSegment.GetNodeOperatorCount = &vsGetNodeOperatorCount;
    p.videoSegment.AcquireOperatorNodeID = &vsAcquireOperatorNodeId;
    p.videoSegment.IterateNodeProperties = &vsIterateNodeProperties;
    p.videoSegment.GetNodeProperty = &vsGetNodeProperty;
    p.videoSegment.GetParamCount = &vsGetParamCount;
    p.videoSegment.GetParam = &vsGetParam;
    p.videoSegment.GetNextKeyframeTime = &vsGetNextKeyframeTime;
    p.videoSegment.TransformNodeTime = &vsTransformNodeTime;
    p.videoSegment.GetVideoSegmentsProperties = &vsGetSegmentsProperties;
    p.videoSegment.AcquireNodeForTime = &vsAcquireNodeForTime;
    p.videoSegment.AcquireVideoSegmentsIDWithStreamLabel = &vsAcquireSegmentsIdLabel;
    p.videoSegment.AcquireVideoSegmentsWithPreviewsIDWithStreamLabel = &vsAcquireSegmentsIdLabel;
    p.videoSegment.AcquireVideoSegmentsWithOpaquePreviewsIDWithStreamLabel = &vsAcquireSegmentsIdLabel;
    p.videoSegment.AcquireFirstNodeInTimeRange = &vsAcquireFirstNodeInRange;
    p.videoSegment.AcquireOperatorOwnerNodeID = &vsAcquireOperatorOwner;
    p.videoSegment.GetGraphicsTransformedParams = &vsGetGraphicsTransformedParams;
    p.videoSegment.HasGraphicsGroup = &vsHasGraphicsGroup;
    p.videoSegment.GetGraphicsGroupID = &vsGetGraphicsGroupId;
    p.videoSegment.GetVideoSegmentsPropertiesExt = &vsGetSegmentsPropertiesExt;
    p.videoSegment.AcquireFirstNodeInTimeRangeExt = &vsAcquireFirstNodeInRangeExt;
    p.videoSegment.GetNodeTimeScale = &vsGetNodeTimeScale;
    // v6 (CC 2014) .. v9: the header grows the table at the end with each
    // version, so a v6 client reading the v9 table sees exactly the v6
    // layout.  GetParam / GetParamCount / GetNodeProperty are v4 members.
    for (int v = kPrSDKVideoSegmentSuiteVersion6; v <= kPrSDKVideoSegmentSuiteVersion9; ++v) {
        p.registerSuite(kPrSDKVideoSegmentSuite, v, &p.videoSegment);
    }
}

}  // namespace osv::premiere::mock
