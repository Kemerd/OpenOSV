// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MockHost core: construction, the suite registry behind SPBasicSuite, the
// legacy piSuites memory callbacks and the inspection API.

#include "MockHostImpl.h"

#include "SPErrorCodes.h"

// kPrRec709 and the rest of the predefined colour-space tokens.
#include "PrSDKColorSpaces.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace osv::premiere::mock {

namespace {

/// The single live instance (see the header).
MockHost* g_current = nullptr;

/// Shorthand used by every suite function: the current implementation or
/// nullptr when no host exists (calls then fail gracefully).
MockHost::Impl* impl() noexcept { return g_current ? g_current->implForSuites() : nullptr; }

// -----------------------------------------------------------------------------
//  SPBasicSuite
// -----------------------------------------------------------------------------
SPErr SPAPI basicAcquireSuite(const char* name, int version, const void** suite) {
    if (!suite) {
        return kSPBadParameterError;
    }
    *suite = nullptr;
    MockHost::Impl* p = impl();
    if (!p || !name) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    SuiteEntry* e = p->findSuite(name, version);
    if (!e || !e->available) {
        return kSPSuiteNotFoundError;
    }
    ++e->refs;
    *suite = e->suite;
    return kSPNoError;
}

SPErr SPAPI basicReleaseSuite(const char* name, int version) {
    MockHost::Impl* p = impl();
    if (!p || !name) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    SuiteEntry* e = p->findSuite(name, version);
    if (!e) {
        return kSPSuiteNotFoundError;
    }
    // Releasing more than acquired is a plug-in bug; report it instead of
    // letting the count go negative.
    if (e->refs <= 0) {
        return kSPBadParameterError;
    }
    --e->refs;
    return kSPNoError;
}

SPBoolean SPAPI basicIsEqual(const char* a, const char* b) {
    if (!a || !b) {
        return a == b ? 1 : 0;
    }
    return std::strcmp(a, b) == 0 ? 1 : 0;
}

SPErr SPAPI basicAllocateBlock(size_t size, void** block) {
    if (!block) {
        return kSPBadParameterError;
    }
    *block = std::malloc(size == 0 ? 1 : size);
    return *block ? kSPNoError : kSPOutOfMemoryError;
}

SPErr SPAPI basicFreeBlock(void* block) {
    std::free(block);
    return kSPNoError;
}

SPErr SPAPI basicReallocateBlock(void* block, size_t newSize, void** newBlock) {
    if (!newBlock) {
        return kSPBadParameterError;
    }
    void* p = std::realloc(block, newSize == 0 ? 1 : newSize);
    if (!p) {
        return kSPOutOfMemoryError;
    }
    *newBlock = p;
    return kSPNoError;
}

SPErr SPAPI basicUndefined() { return kSPUnimplementedError; }

// -----------------------------------------------------------------------------
//  Legacy memory callbacks (piSuites->memFuncs)
// -----------------------------------------------------------------------------
char* memNewPtr(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->allocPtr(size, false);
}

char* memNewPtrClear(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->allocPtr(size, true);
}

void memSetPtrSize(PrMemoryPtr* ptr, csSDK_uint32 newSize) {
    MockHost::Impl* p = impl();
    if (!p || !ptr) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    char* grown = p->resizePtr(*ptr, newSize);
    if (grown) {
        *ptr = grown;
    }
}

csSDK_int32 memGetPtrSize(char* ptr) {
    MockHost::Impl* p = impl();
    if (!p) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return static_cast<csSDK_int32>(p->ptrSize(ptr));
}

void memDisposePtr(char* ptr) {
    MockHost::Impl* p = impl();
    if (!p) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->freePtr(ptr);
}

char** memNewHandle(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    HandleBlock* b = p->allocHandle(size, false);
    return b ? &b->master : nullptr;
}

char** memNewHandleClear(csSDK_uint32 size) {
    MockHost::Impl* p = impl();
    if (!p) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    HandleBlock* b = p->allocHandle(size, true);
    return b ? &b->master : nullptr;
}

csSDK_int16 memSetHandleSize(PrMemoryHandle h, csSDK_uint32 newSize) {
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

csSDK_int32 memGetHandleSize(PrMemoryHandle h) {
    MockHost::Impl* p = impl();
    if (!p) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    HandleBlock* b = p->handleBlock(h);
    return b ? static_cast<csSDK_int32>(b->size) : 0;
}

void memDisposeHandle(PrMemoryHandle h) {
    MockHost::Impl* p = impl();
    if (!p) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->freeHandle(h);
}

void memLockHandle(PrMemoryHandle) {}
void memUnlockHandle(PrMemoryHandle) {}

// -----------------------------------------------------------------------------
//  piSuites->utilFuncs
// -----------------------------------------------------------------------------
SPBasicSuite* utilGetSPBasicSuite() {
    MockHost::Impl* p = impl();
    return p ? &p->basic : nullptr;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Impl helpers
// -----------------------------------------------------------------------------
void MockHost::Impl::registerSuite(const char* name, int version, const void* suite) {
    SuiteEntry e;
    e.name = name ? name : "";
    e.version = version;
    e.suite = suite;
    registry.push_back(std::move(e));
}

SuiteEntry* MockHost::Impl::findSuite(const char* name, int version) {
    if (!name) {
        return nullptr;
    }
    for (SuiteEntry& e : registry) {
        if (e.version == version && e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

char* MockHost::Impl::allocPtr(std::uint32_t size, bool clear) {
    const std::size_t total = sizeof(PtrHeader) + static_cast<std::size_t>(size) + 16u;
    void* raw = clear ? std::calloc(1, total) : std::malloc(total);
    if (!raw) {
        return nullptr;
    }
    auto* header = static_cast<PtrHeader*>(raw);
    header->magic = PtrHeader::kMagic;
    header->size = size;
    header->reserved = 0;
    char* payload = reinterpret_cast<char*>(header + 1);
    livePtrs.insert(payload);
    return payload;
}

std::uint32_t MockHost::Impl::ptrSize(char* p) const {
    if (!p || !livePtrs.count(p)) {
        return 0;
    }
    const auto* header = reinterpret_cast<const PtrHeader*>(p) - 1;
    return header->magic == PtrHeader::kMagic ? header->size : 0;
}

void MockHost::Impl::freePtr(char* p) {
    if (!p) {
        return;
    }
    auto it = livePtrs.find(p);
    if (it == livePtrs.end()) {
        return;  // not ours (or already freed); never free foreign memory
    }
    livePtrs.erase(it);
    auto* header = reinterpret_cast<PtrHeader*>(p) - 1;
    header->magic = 0;
    std::free(header);
}

char* MockHost::Impl::resizePtr(char* p, std::uint32_t newSize) {
    if (!p) {
        return allocPtr(newSize, true);
    }
    const std::uint32_t oldSize = ptrSize(p);
    if (oldSize == 0 && !livePtrs.count(p)) {
        return nullptr;
    }
    char* fresh = allocPtr(newSize, true);
    if (!fresh) {
        return nullptr;
    }
    std::memcpy(fresh, p, std::min(oldSize, newSize));
    freePtr(p);
    return fresh;
}

HandleBlock* MockHost::Impl::allocHandle(std::uint32_t size, bool clear) {
    char* payload = allocPtr(size, clear);
    if (!payload) {
        return nullptr;
    }
    auto* block = new (std::nothrow) HandleBlock();
    if (!block) {
        freePtr(payload);
        return nullptr;
    }
    block->master = payload;
    block->size = size;
    liveHandles.insert(block);
    return block;
}

HandleBlock* MockHost::Impl::handleBlock(PrMemoryHandle h) const {
    if (!h) {
        return nullptr;
    }
    auto* block = reinterpret_cast<HandleBlock*>(h);
    return liveHandles.count(block) ? block : nullptr;
}

void MockHost::Impl::freeHandle(PrMemoryHandle h) {
    HandleBlock* block = handleBlock(h);
    if (!block) {
        return;
    }
    liveHandles.erase(block);
    freePtr(block->master);
    delete block;
}

EffectRef* MockHost::Impl::effectRef(PF_ProgPtr ref) const {
    if (!ref) {
        return nullptr;
    }
    auto* r = reinterpret_cast<EffectRef*>(ref);
    if (!effectRefs.count(r) || r->magic != EffectRef::kMagic) {
        return nullptr;
    }
    return r;
}

// -----------------------------------------------------------------------------
//  MockHost
// -----------------------------------------------------------------------------
MockHost* MockHost::current() noexcept { return g_current; }

MockHost::MockHost() : m_impl(std::make_unique<Impl>()) {
    if (g_current) {
        throw std::logic_error("MockHost: only one instance may exist at a time");
    }
    Impl& p = *m_impl;

    // Legacy callback tables.
    p.memFuncs.newPtr = &memNewPtr;
    p.memFuncs.setPtrSize = &memSetPtrSize;
    p.memFuncs.getPtrSize = &memGetPtrSize;
    p.memFuncs.disposePtr = &memDisposePtr;
    p.memFuncs.newHandle = &memNewHandle;
    p.memFuncs.setHandleSize = &memSetHandleSize;
    p.memFuncs.getHandleSize = &memGetHandleSize;
    p.memFuncs.disposeHandle = &memDisposeHandle;
    p.memFuncs.newPtrClear = &memNewPtrClear;
    p.memFuncs.newHandleClear = &memNewHandleClear;
    p.memFuncs.lockHandle = &memLockHandle;
    p.memFuncs.unlockHandle = &memUnlockHandle;

    p.utilFuncs.getSPBasicSuite = &utilGetSPBasicSuite;

    p.suites.piInterfaceVer = PR_PISUITES_VERSION;
    p.suites.memFuncs = &p.memFuncs;
    p.suites.windFuncs = nullptr;
    p.suites.ppixFuncs = nullptr;
    p.suites.utilFuncs = &p.utilFuncs;
    p.suites.timelineFuncs = nullptr;

    p.basic.AcquireSuite = &basicAcquireSuite;
    p.basic.ReleaseSuite = &basicReleaseSuite;
    p.basic.IsEqual = &basicIsEqual;
    p.basic.AllocateBlock = &basicAllocateBlock;
    p.basic.FreeBlock = &basicFreeBlock;
    p.basic.ReallocateBlock = &basicReallocateBlock;
    p.basic.Undefined = &basicUndefined;

    // Suite implementations register themselves.
    installPPixSuites(p);
    installMiscSuites(p);
    installGpuSuite(p);
    installAeSuites(p);
    // The recording custom-UI / DrawBot surface (MockDrawbot.cpp).
    installDrawbotSuites(p);

    g_current = this;
}

MockHost::~MockHost() {
    Impl& p = *m_impl;
    {
        std::lock_guard<std::recursive_mutex> lock(p.mutex);
        // Cache first (it holds PPix references), then whatever a sloppy
        // plug-in left behind.
        p.clearCacheLocked();
        for (auto it = p.namedCache.begin(); it != p.namedCache.end();) {
            PPixRecord* rec = it->second;
            it = p.namedCache.erase(it);
            p.releasePPix(rec);
        }
        for (auto it = p.rawCache.begin(); it != p.rawCache.end();) {
            PPixRecord* rec = it->second;
            it = p.rawCache.erase(it);
            p.releasePPix(rec);
        }
        std::vector<PPixRecord*> leftovers(p.livePPix.begin(), p.livePPix.end());
        for (PPixRecord* rec : leftovers) {
            rec->refCount = 1;
            p.releasePPix(rec);
        }
        p.shutdownGpu();
        for (EffectRef* r : p.effectRefs) {
            delete r;
        }
        p.effectRefs.clear();
        p.hostWorlds.clear();
        std::vector<HandleBlock*> handles(p.liveHandles.begin(), p.liveHandles.end());
        for (HandleBlock* b : handles) {
            p.freeHandle(&b->master);
        }
        std::vector<void*> ptrs(p.livePtrs.begin(), p.livePtrs.end());
        for (void* ptr : ptrs) {
            p.freePtr(static_cast<char*>(ptr));
        }
    }
    g_current = nullptr;
}

piSuitesPtr MockHost::piSuites() noexcept { return &m_impl->suites; }

SPBasicSuite* MockHost::basicSuite() noexcept { return &m_impl->basic; }

void MockHost::setSuiteAvailable(const char* name, int version, bool available) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (SuiteEntry* e = m_impl->findSuite(name, version)) {
        e->available = available;
    }
}

int MockHost::suiteRefCount(const char* name, int version) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const SuiteEntry* e = m_impl->findSuite(name, version);
    return e ? e->refs : -1;
}

int MockHost::totalSuiteRefs() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    int total = 0;
    for (const SuiteEntry& e : m_impl->registry) {
        total += e.refs;
    }
    return total;
}

std::size_t MockHost::liveMemoryBlocks() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    // Handles own a pointer each; count each allocation once.
    return m_impl->livePtrs.size();
}

std::optional<PPixInfo> MockHost::inspect(PPixHand hand) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const PPixRecord* rec = m_impl->record(hand);
    if (!rec) {
        return std::nullopt;
    }
    PPixInfo info;
    info.pixels = rec->isGpu ? rec->devicePtr : static_cast<void*>(rec->pixels);
    info.width = rec->width;
    info.height = rec->height;
    info.rowBytes = rec->rowBytes;
    info.format = rec->format;
    info.colorSpace = rec->colorSpace;
    info.isGpu = rec->isGpu;
    info.deviceIndex = rec->deviceIndex;
    info.byteSize = rec->byteSize;
    info.refCount = rec->refCount;
    info.parNum = rec->parNum;
    info.parDen = rec->parDen;
    info.fieldType = rec->fieldType;
    return info;
}

std::size_t MockHost::livePPixCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->livePPix.size();
}

CacheStats MockHost::cacheStats() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    CacheStats s;
    s.hits = m_impl->cacheHits;
    s.misses = m_impl->cacheMisses;
    s.entries = m_impl->cacheMap.size();
    s.evictions = m_impl->cacheEvictions;
    s.capacity = m_impl->cacheCapacity;
    return s;
}

void MockHost::setCacheCapacity(std::size_t capacity) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->cacheCapacity = capacity;
    // Evict the least recently used until the new limit holds.
    while (m_impl->cacheMap.size() > m_impl->cacheCapacity && !m_impl->cacheOrder.empty()) {
        const CacheKey victim = m_impl->cacheOrder.back();
        auto it = m_impl->cacheMap.find(victim);
        if (it != m_impl->cacheMap.end()) {
            PPixRecord* rec = it->second.first;
            m_impl->cacheMap.erase(it);
            m_impl->releasePPix(rec);
        }
        m_impl->cacheOrder.pop_back();
        ++m_impl->cacheEvictions;
    }
}

void MockHost::clearCache() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->clearCacheLocked();
}

std::string MockHost::utf8(const PrSDKString& s) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    auto it = m_impl->strings.find(s.opaque[0]);
    if (it == m_impl->strings.end()) {
        return {};
    }
    return it->second.utf8;
}

std::size_t MockHost::liveStringCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->strings.size();
}

void MockHost::setAppIdentity(const AppIdentity& identity) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->identity = identity;
}

AppIdentity MockHost::appIdentity() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->identity;
}

std::vector<ErrorEvent> MockHost::errorEvents() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->events;
}

void MockHost::clearErrorEvents() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->events.clear();
}

SequenceConfig MockHost::sequence(PrTimelineID timeline) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    auto it = m_impl->sequences.find(timeline);
    if (it != m_impl->sequences.end()) {
        return it->second;
    }
    // First access creates the entry with defaults (documented in MockHost.h)
    // so a test can read a timeline it never configured.  The token comes
    // from the SDK macro, never a hand-typed string: PrSDKColorSpaces.h is
    // the only place that decides what "BT.709" is spelled like.
    SequenceConfig cfg;
    cfg.workingColorSpace = colorSpaceId(kPrRec709);
    m_impl->sequences.emplace(timeline, cfg);
    return cfg;
}

void MockHost::setSequence(PrTimelineID timeline, const SequenceConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->sequences[timeline] = config;
}

void MockHost::removeSequence(PrTimelineID timeline) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->sequences.erase(timeline);
}

void MockHost::setParam(csSDK_int32 nodeId, csSDK_int32 index, PrTime time, const PrParam& value) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->nodes[nodeId].params[index].keys[time] = value;
}

void MockHost::clearNode(csSDK_int32 nodeId) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->nodes.erase(nodeId);
}

void MockHost::setNodeProperty(csSDK_int32 nodeId, std::string_view key, std::string_view value) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->nodes[nodeId].properties[std::string(key)] = std::string(value);
}

void MockHost::setParamCount(csSDK_int32 nodeId, csSDK_int32 count) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    // Any negative value means "derive it", so the override can be undone.
    m_impl->nodes[nodeId].paramCountOverride = count < 0 ? -1 : count;
}

void MockHost::setParamReadError(csSDK_int32 nodeId, csSDK_int32 index, prSuiteError error,
                                 std::optional<PrTime> onlyAt) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    NodeRecord& node = m_impl->nodes[nodeId];
    if (error == suiteError_NoError) {
        node.readFailures.erase(index);
        return;
    }
    ParamReadFailure f;
    f.error = error;
    f.onlyAtTime = onlyAt.has_value();
    f.time = onlyAt.value_or(0);
    node.readFailures[index] = f;
}

std::vector<ParamReadRecord> MockHost::paramReads() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->paramReads;
}

void MockHost::clearParamReads() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->paramReads.clear();
    m_impl->paramReadsDropped = 0;
}

// -----------------------------------------------------------------------------
//  Segment graph
// -----------------------------------------------------------------------------

void MockHost::setNodeType(csSDK_int32 nodeId, std::string_view type) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->nodes[nodeId].type = std::string(type);
}

void MockHost::setNodeOwner(csSDK_int32 operatorNodeId, csSDK_int32 ownerNodeId) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->nodes[operatorNodeId].owner = ownerNodeId;
    // The owner is a node in its own right: create it so the walk that
    // follows the edge finds something to answer GetNodeInfo with.
    if (ownerNodeId != 0) {
        (void)m_impl->nodes[ownerNodeId];
    }
}

void MockHost::addNodeInput(csSDK_int32 nodeId, csSDK_int32 inputNodeId, PrTime offset) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    NodeInput input;
    input.node = inputNodeId;
    input.offset = offset;
    m_impl->nodes[nodeId].inputs.push_back(input);
    (void)m_impl->nodes[inputNodeId];
}

void MockHost::setNodeTimeTransform(csSDK_int32 nodeId, PrTime origin, std::int64_t rateNum, std::int64_t rateDen) {
    // A zero or negative denominator describes no transform at all; refusing
    // it here keeps TransformNodeTime from ever dividing by it.
    if (rateDen <= 0) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    NodeRecord& node = m_impl->nodes[nodeId];
    node.timeOrigin = origin;
    node.rateNum = rateNum;
    node.rateDen = rateDen;
}

void MockHost::setNodeTimeTransformError(csSDK_int32 nodeId, prSuiteError error) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->nodes[nodeId].transformError = error;
}

int MockHost::nodeRefCount(csSDK_int32 nodeId) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const auto it = m_impl->nodes.find(nodeId);
    return it == m_impl->nodes.end() ? 0 : it->second.refs;
}

int MockHost::totalNodeRefs() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    int total = 0;
    for (const auto& entry : m_impl->nodes) {
        total += entry.second.refs;
    }
    return total;
}

std::size_t MockHost::invalidNodeReleases() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    return m_impl->invalidNodeReleases;
}

}  // namespace osv::premiere::mock
