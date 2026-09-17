// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Internal state of MockHost shared by the suite implementation files.
// Not part of the test API; include MockHost.h instead.
#pragma once

#include "MockHost.h"

#include "PrSDKAppInfoSuite.h"
#include "PrSDKColorManagementSuite.h"
#include "PrSDKErrorSuite.h"
#include "PrSDKGPUDeviceSuite.h"
#include "PrSDKImporterFileManagerSuite.h"
#include "PrSDKMemoryManagerSuite.h"
#include "PrSDKPPix2Suite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixCreator2Suite.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKSequenceInfoSuite.h"
#include "PrSDKStringSuite.h"
#include "PrSDKVideoSegmentSuite.h"

#include "AE_EffectSuites.h"

#include <list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osv::premiere::mock {

// -----------------------------------------------------------------------------
//  Records behind the opaque handles
// -----------------------------------------------------------------------------

/// Storage behind a PPixHand.  `master` must stay the first member: the
/// handle handed to plug-ins is &master (a PPix**), and the record is found
/// again by casting the handle back.
struct PPixRecord {
    static constexpr std::uint32_t kMagic = 0x50504958u;  // "PPIX"

    PPix* master = nullptr;
    std::uint32_t magic = kMagic;
    PPix header{};
    std::vector<std::uint8_t> storage;  ///< Host pixels (over-allocated for alignment).
    char* pixels = nullptr;             ///< 128-byte aligned start of row 0.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t rowBytes = 0;
    PrPixelFormat format = PrPixelFormat_Invalid;
    PrSDKColorSpaceID colorSpace{};
    int refCount = 1;
    bool isGpu = false;
    csSDK_uint32 deviceIndex = 0;
    void* devicePtr = nullptr;
    std::size_t byteSize = 0;
    csSDK_uint32 parNum = 1;
    csSDK_uint32 parDen = 1;
    prFieldType fieldType = prFieldsNone;
};

/// Header placed in front of every pointer allocated by the memory
/// functions so getPtrSize / setPtrSize work.
struct PtrHeader {
    static constexpr std::uint32_t kMagic = 0x4D505452u;  // "MPTR"
    std::uint32_t magic = kMagic;
    std::uint32_t size = 0;
    std::uint64_t reserved = 0;  ///< Keeps the payload 16-byte aligned.
};

/// A handle (pointer to master pointer) allocated by the memory functions.
struct HandleBlock {
    char* master = nullptr;  ///< First member: PrMemoryHandle == &master.
    std::uint32_t magic = 0x4D48444Cu;  // "MHDL"
    std::uint32_t size = 0;
};

/// Key of one cached frame.
struct CacheKey {
    csSDK_uint32 importerId = 0;
    csSDK_int32 streamIndex = 0;
    csSDK_int32 frameNumber = 0;
    csSDK_uint32 quality = 0xFFFFFFFFu;
    std::vector<std::uint8_t> prefs;
    csSDK_int64 colorSpace0 = 0;
    csSDK_int64 colorSpace1 = 0;

    bool operator==(const CacheKey& o) const noexcept {
        return importerId == o.importerId && streamIndex == o.streamIndex && frameNumber == o.frameNumber &&
               quality == o.quality && prefs == o.prefs && colorSpace0 == o.colorSpace0 && colorSpace1 == o.colorSpace1;
    }
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
        std::size_t h = static_cast<std::size_t>(k.importerId) * 1000003u;
        h ^= static_cast<std::size_t>(k.streamIndex) + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.frameNumber) + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.quality) + 0x9E3779B9u + (h << 6) + (h >> 2);
        for (const std::uint8_t b : k.prefs) {
            h ^= static_cast<std::size_t>(b) + 0x9E3779B9u + (h << 6) + (h >> 2);
        }
        h ^= static_cast<std::size_t>(k.colorSpace0) + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.colorSpace1) + 0x9E3779B9u + (h << 6) + (h >> 2);
        return h;
    }
};

/// One registry entry: a suite pointer under a (name, version) pair.
struct SuiteEntry {
    std::string name;
    int version = 0;
    const void* suite = nullptr;
    int refs = 0;
    bool available = true;
};

/// A string allocated by the String Suite.
struct StringRecord {
    std::string utf8;
    std::wstring utf16;
};

/// Keyframes of one (node, index).
struct ParamTrack {
    std::map<PrTime, PrParam> keys;
};

/// Everything about one node.
struct NodeRecord {
    std::map<csSDK_int32, ParamTrack> params;
    std::map<std::string, std::string> properties;
};

/// The opaque effect reference behind PF_ProgPtr.
struct EffectRef {
    static constexpr std::uint32_t kMagic = 0x45464643u;  // "EFFC"
    std::uint32_t magic = kMagic;
    PrTimelineID timeline = 0;
    A_long instanceId = 0;
    std::vector<PrPixelFormat> formats;
    std::vector<PF_ParamDef> params;   ///< Added through add_param (index 1..n).
    PF_ParamDef inputLayer{};          ///< params[0].
    EffectWorld* input = nullptr;
    std::vector<PF_ParamDef*> renderArray;

    /// AE-side keyframes: (parameter index, time) -> value.
    ///
    /// Empty by default, in which case checkout_param returns the stored
    /// value for every time - the original behaviour, and the right one for
    /// a test that does not care about time.  A test that DOES care (the
    /// Smooth Keyframes average, which samples t-1, t and t+1) installs
    /// different values at different times so the arithmetic is observable.
    /// Without this a smoothing test can only prove (v+v+v)/3 == v, which
    /// every plausible implementation satisfies.
    std::map<std::pair<A_long, A_long>, PF_ParamDef> keyframes;

    /// The custom UI the effect registered through
    /// PF_InteractCallbacks::register_ui, and whether it registered one at
    /// all.  A test reads these to prove the reframe overlay asked for
    /// PF_CustomEFlag_COMP (the Program Monitor).
    PF_CustomUIInfo customUi{};
    bool customUiRegistered = false;

    // ---- PF Source Settings Suite -------------------------------------------

    /// Whether the effect called SetIsSourceSettingsEffect, and with what.
    ///
    /// In a real host this is the declaration that makes Premiere attach the
    /// effect to a master clip rather than offering it as a timeline filter,
    /// and there is no other way to observe it - the flag lives entirely in
    /// the host.  So the mock records it and a test reads it back, which is
    /// the only way to prove the effect made the call at all.
    bool isSourceSettingsEffect = false;
    bool sourceSettingsFlagSet = false;

    /// What the mock's PerformSourceSettingsCommand does with the effect's
    /// buffer, standing in for the importer at the other end of the private
    /// channel.
    ///
    /// `sourceSettingsReply` is written INTO the buffer when
    /// `sourceSettingsReplies` is true, which is how a test drives the
    /// "importer reports different settings than the controls hold" path
    /// without loading the .prm as well.  When false the buffer is left
    /// exactly as the effect passed it - the real behaviour when no live clip
    /// instance exists.
    std::vector<char> sourceSettingsReply;
    bool sourceSettingsReplies = false;
    /// Forced return code; PF_Err_NONE means "succeed".
    PF_Err sourceSettingsError = PF_Err_NONE;
    /// A copy of what the effect last sent, and how many bytes it declared.
    std::vector<char> sourceSettingsSent;
    csSDK_uint32 sourceSettingsSentSize = 0;
    std::size_t sourceSettingsCallCount = 0;
};

/// The surface translation the mock tracks.
///
/// Only the translation, because that is all the overlay's shadow pass uses
/// and a full 3x3 concatenation would be state no test could usefully check.
struct DrawbotTransform {
    float dx = 0.0f;
    float dy = 0.0f;
};

/// Everything the recording DrawBot suites accumulate.
///
/// Lives in the Impl (not in MockHost) because the suite functions are plain
/// C function pointers that reach it through MockHost::current().
struct DrawbotState {
    /// The one drawing reference the mock hands out, and the context behind
    /// the PF_ContextH a test builds.
    DRAWBOT_DrawRef drawRef = nullptr;
    PF_Context context{};
    PF_Context* contextPtr = nullptr;

    // Behaviour switches a test can flip.
    bool supportsText = true;
    bool provideDrawRef = true;
    bool failNewPen = false;
    bool failNewPath = false;
    float defaultFontSize = 11.0f;

    // The record.
    std::vector<DrawbotPenRecord> pens;
    std::vector<DrawbotBrushRecord> brushes;
    std::vector<DrawbotPathRecord> paths;
    std::vector<DrawbotFontRecord> fonts;
    std::vector<DrawbotDrawOp> ops;
    std::vector<DrawbotStringOp> strings;
    std::vector<DrawbotPaintRect> paintRects;

    // Surface state.
    DrawbotTransform transform;
    std::vector<DrawbotTransform> transformStack;
    DRAWBOT_Rect32 clipBounds{};
    DRAWBOT_InterpolationPolicy interpolation = kDRAWBOT_InterpolationPolicy_Default;
    DRAWBOT_AntiAliasPolicy antiAlias = kDRAWBOT_AntiAliasPolicy_Default;

    // Counters.
    std::size_t retainCount = 0;
    std::size_t releaseCount = 0;
    std::size_t pushCount = 0;
    std::size_t popCount = 0;
    std::size_t unbalancedPops = 0;
    std::size_t flushCount = 0;
    std::size_t drawImageCount = 0;
    std::size_t imageCount = 0;
};

/// GPU device state (only meaningful when built with CUDA).
struct GpuState {
    bool initialised = false;
    bool available = false;
    std::string failure;
    int deviceOrdinal = 0;
    void* device = nullptr;     ///< CUdevice stored as pointer-sized value.
    void* context = nullptr;    ///< CUcontext.
    void* stream = nullptr;     ///< CUstream.
    std::unordered_set<void*> deviceAllocations;
    std::unordered_set<void*> hostAllocations;
};

// -----------------------------------------------------------------------------
//  The implementation object
// -----------------------------------------------------------------------------
struct MockHost::Impl {
    mutable std::recursive_mutex mutex;

    // Callback tables handed to the plug-in.
    PlugMemoryFuncs memFuncs{};
    PlugUtilFuncs utilFuncs{};
    // Fully qualified on purpose: Impl is nested inside MockHost, whose
    // member function MockHost::piSuites() would otherwise hide the global
    // Adobe type of the same name (C3646 "unknown override specifier").
    ::piSuites suites{};
    SPBasicSuite basic{};

    // Suite function tables.
    PrSDKPPixSuite ppix{};
    PrSDKPPix2Suite ppix2{};
    PrSDKPPixCreatorSuite creator{};
    PrSDKPPixCreator2Suite creator2{};
    PrSDKPPixCacheSuite cache{};
    PrSDKTimeSuite time{};
    PrSDKStringSuite string{};
    PrSDKAppInfoSuite appInfo{};
    PrSDKErrorSuite3 error{};
    PrSDKColorManagementSuite color{};
    PrSDKMemoryManagerSuite memory{};
    PrSDKImporterFileManagerSuite fileManager{};
    PrSDKSequenceInfoSuite sequenceInfo{};
    PrSDKVideoSegmentSuite videoSegment{};
    PrSDKGPUDeviceSuite gpu{};
    PF_PixelFormatSuite1 pfPixelFormat{};
    PF_UtilitySuite4 pfUtility{};
    PF_SourceSettingsSuite pfSourceSettings{};

    // Custom UI / DrawBot (MockDrawbot.cpp).
    PF_EffectCustomUISuite2 pfCustomUi{};
    DRAWBOT_DrawbotSuiteCurrent drawbotDraw{};
    DRAWBOT_SupplierSuiteCurrent drawbotSupplier{};
    DRAWBOT_SurfaceSuiteCurrent drawbotSurface{};
    DRAWBOT_PathSuiteCurrent drawbotPath{};
    DRAWBOT_PenSuiteCurrent drawbotPen{};
    DRAWBOT_ImageSuiteCurrent drawbotImage{};

    // Registry.
    std::vector<SuiteEntry> registry;

    // Memory.
    std::unordered_set<void*> livePtrs;      ///< Payload pointers.
    std::unordered_set<HandleBlock*> liveHandles;
    std::unordered_map<csSDK_uint32, std::pair<PrSDKMemoryManagerSuite_PurgeMemoryFunction, void*>> memoryBlocks;
    csSDK_uint32 nextBlockId = 1;
    csSDK_uint64 reservedBytes = 0;

    // PPix.
    std::unordered_set<PPixRecord*> livePPix;

    // Cache (LRU: front = most recent).
    std::list<CacheKey> cacheOrder;
    std::unordered_map<CacheKey, std::pair<PPixRecord*, std::list<CacheKey>::iterator>, CacheKeyHash> cacheMap;
    std::unordered_map<std::string, PPixRecord*> namedCache;
    std::map<std::pair<csSDK_uint32, csSDK_int32>, PPixRecord*> rawCache;
    std::size_t cacheCapacity = 16;
    std::size_t cacheHits = 0;
    std::size_t cacheMisses = 0;
    std::size_t cacheEvictions = 0;

    // Strings.
    std::unordered_map<csSDK_int64, StringRecord> strings;
    csSDK_int64 nextStringId = 1;

    // App info / errors.
    AppIdentity identity;
    std::vector<ErrorEvent> events;

    // Colour: index -> token.
    std::vector<std::string> colorTokens;
    std::vector<prSEIColorCodesRec> colorSei;

    // Sequences.
    std::map<PrTimelineID, SequenceConfig> sequences;

    // Video segment.
    std::map<csSDK_int32, NodeRecord> nodes;

    // GPU.
    GpuState gpuState;

    // Custom UI / DrawBot recording.
    DrawbotState drawbot;

    // AE.
    std::unordered_set<EffectRef*> effectRefs;
    std::unordered_map<const PF_EffectWorld*, EffectWorld*> worlds;  ///< Registered worlds (owned by callers or by the host).
    std::vector<std::unique_ptr<EffectWorld>> hostWorlds;             ///< Worlds created through NewWorldOfPixelFormat.

    // ---- helpers shared by the suite files ---------------------------------
    void registerSuite(const char* name, int version, const void* suite);
    SuiteEntry* findSuite(const char* name, int version);

    /// Validate and fetch the record behind a handle (nullptr when unknown).
    PPixRecord* record(PPixHand hand) const;
    /// Allocate a host PPix; nullptr for an unsupported format / size.
    PPixRecord* createHostPPix(PrPixelFormat format, std::uint32_t width, std::uint32_t height, csSDK_uint32 parNum,
                               csSDK_uint32 parDen, prFieldType fieldType, const PrSDKColorSpaceID& colorSpace);
    /// Drop one reference; frees the record at zero.
    void releasePPix(PPixRecord* rec);
    /// Add a reference (cache / clone).
    void retainPPix(PPixRecord* rec);
    /// Free everything the cache holds.
    void clearCacheLocked();

    /// Memory helpers.
    char* allocPtr(std::uint32_t size, bool clear);
    void freePtr(char* p);
    std::uint32_t ptrSize(char* p) const;
    char* resizePtr(char* p, std::uint32_t newSize);
    HandleBlock* allocHandle(std::uint32_t size, bool clear);
    void freeHandle(PrMemoryHandle h);
    HandleBlock* handleBlock(PrMemoryHandle h) const;

    /// Colour helpers.
    PrSDKColorSpaceID colorIdForIndex(std::size_t index) const;
    bool colorIndexForId(const PrSDKColorSpaceID& id, std::size_t* index) const;

    /// Effect ref validation.
    EffectRef* effectRef(PF_ProgPtr ref) const;

    /// GPU teardown (no-op without CUDA).
    void shutdownGpu();
};

/// Bytes per pixel of a host / GPU pixel format handled by the mock, 0 for
/// unsupported formats.
std::size_t bytesPerPixel(PrPixelFormat format) noexcept;

// Installers implemented in the per-suite translation units.
void installPPixSuites(MockHost::Impl& impl);
void installMiscSuites(MockHost::Impl& impl);
void installGpuSuite(MockHost::Impl& impl);
void installAeSuites(MockHost::Impl& impl);
void installDrawbotSuites(MockHost::Impl& impl);

}  // namespace osv::premiere::mock
