// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GpuDecoderPool: the typed front of detail::IdlePool for GpuClipDecoders.
// This file decides what makes two decoders interchangeable (the match key),
// which decoders may be parked at all (those that own a retain on their
// context), and when a parked decoder must go early (its device is short of
// VRAM, or its context is dead).  The parking itself - bounds, idle expiry,
// system memory pressure, the reaper thread - is IdlePool's.

#include "osv/video/GpuDecoderPool.h"

#include "FileIdentity.h"
#include "IdlePool.h"
#include "osv/core/Log.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace osv::video {

namespace {

// -----------------------------------------------------------------------------
//  Matching
// -----------------------------------------------------------------------------

/// Pointer as hex, for the key.
std::wstring pointerText(const void* p) {
    wchar_t buffer[32] = {};
    std::swprintf(buffer, std::size(buffer), L"%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buffer;
}

/// Everything a parked decoder must share with a request to be handed out
/// for it, as one string ('|' cannot occur in a Windows path).  The context
/// is the REQUESTED one: nullptr + a device ordinal names that device's
/// primary context, which is the only kind the pool ever holds.
std::wstring makeKey(const std::wstring& file, const std::array<std::uint32_t, 2>& tracks,
                     const GpuDecoderOptions& o) {
    std::wstring k = file;
    k += L"|tracks=" + std::to_wstring(tracks[0]) + L"," + std::to_wstring(tracks[1]);
    k += L"|context=" + pointerText(o.cuContext);
    k += L"|stream=" + pointerText(o.cuStream);
    k += L"|device=" + std::to_wstring(o.cudaDevice);
    k += L"|budget=" + std::to_wstring(static_cast<unsigned long long>(o.vramBudgetBytes));
    k += L"|ahead=" + std::to_wstring(o.decodeAhead);
    k += L"|threads=" + std::to_wstring(o.decoderThreads);
    return k;
}

// -----------------------------------------------------------------------------
//  The parked item
// -----------------------------------------------------------------------------

/// One parked decoder.  It asks to be released when its context is no longer
/// usable, or when its device has less free VRAM than the pool's floor.
class DecoderItem final : public detail::IdleItem {
public:
    explicit DecoderItem(std::unique_ptr<GpuClipDecoder> decoder) noexcept : m_decoder(std::move(decoder)) {}

    [[nodiscard]] bool mustRelease(std::uint64_t minFreeVramMiB) noexcept override {
        if (!m_decoder || !m_decoder->contextAlive()) {
            return true;
        }
        if (minFreeVramMiB == 0) {
            return false;
        }
        std::size_t freeBytes = 0;
        std::size_t totalBytes = 0;
        if (!m_decoder->deviceMemory(freeBytes, totalBytes).ok()) {
            // The device cannot even say how much memory it has: something is
            // wrong with the context, and a decoder in it is no asset.
            return true;
        }
        return (static_cast<std::uint64_t>(freeBytes) >> 20) < minFreeVramMiB;
    }

    /// Hand the decoder back (the item is empty afterwards).
    [[nodiscard]] std::unique_ptr<GpuClipDecoder> release() noexcept { return std::move(m_decoder); }

private:
    std::unique_ptr<GpuClipDecoder> m_decoder;
};

/// Core limits from the pool's.
detail::IdlePoolLimits toCore(const GpuDecoderPool::Limits& limits) noexcept {
    detail::IdlePoolLimits core;
    core.maxIdle = limits.maxIdleDecoders;
    core.idleTtl = limits.idleTtl;
    core.minAvailableMemoryMiB = limits.minAvailableMemoryMiB;
    core.minFreeDeviceMemoryMiB = limits.minFreeVramMiB;
    return core;
}

}  // namespace

// =============================================================================
//  GpuDecoderPool
// =============================================================================
GpuDecoderPool::GpuDecoderPool() : GpuDecoderPool(Limits{}) {}

GpuDecoderPool::GpuDecoderPool(const Limits& limits)
    : m_core(std::make_unique<detail::IdlePool>("GPU decoder pool", toCore(limits))),
      m_cachedFramesKept(limits.cachedFramesKept) {}

GpuDecoderPool::~GpuDecoderPool() = default;

GpuDecoderPool& GpuDecoderPool::instance() {
    // Never destroyed on purpose: releasing NVDEC decoders from a static
    // destructor would run CUDA and join threads under the loader lock.
    static GpuDecoderPool* const pool = new GpuDecoderPool();
    return *pool;
}

bool GpuDecoderPool::poolable(const GpuClipDecoder& decoder) noexcept {
    // Open (a decoder object exists only after a successful open, but a
    // moved-out shell would report no frames), a known file version, a
    // primary context it retains itself, and no caller stream whose lifetime
    // belongs to someone else.
    return decoder.frameCount() > 0 && !decoder.fileIdentity().empty() && decoder.usesRetainedPrimaryContext() &&
           decoder.options().cuContext == nullptr && decoder.options().cuStream == nullptr;
}

std::unique_ptr<GpuClipDecoder> GpuDecoderPool::take(const std::filesystem::path& path, const meta::FormatInfo& format,
                                                     const GpuDecoderOptions& options) noexcept {
    if (!m_core) {
        return nullptr;
    }
    try {
        // Requests for a caller's context or stream can never match (those
        // decoders are never parked), but still count as a miss.
        std::wstring key;
        if (options.cuContext == nullptr && options.cuStream == nullptr) {
            auto identity = detail::fileIdentity(path);
            if (identity.ok()) {
                key = makeKey(identity.value(), {format.videoTrackIds[0], format.videoTrackIds[1]}, options);
            }
        }
        std::unique_ptr<detail::IdleItem> item = m_core->take(key);
        if (!item) {
            return nullptr;
        }
        // Only DecoderItems are ever parked in this pool's core.
        return static_cast<DecoderItem*>(item.get())->release();
    } catch (...) {
        return nullptr;
    }
}

bool GpuDecoderPool::park(std::unique_ptr<GpuClipDecoder> decoder) noexcept {
    if (!m_core) {
        return false;
    }
    try {
        // A decoder that may not be parked goes to the core with an empty
        // key, which refuses (and releases) it with the right counter.
        std::wstring key;
        if (decoder && poolable(*decoder)) {
            key = makeKey(decoder->fileIdentity(), decoder->trackIds(), decoder->options());
            // Small while idle: no decode-ahead, only the most recent frames.
            // Outside the pool lock - it may wait for a frame pair in flight.
            const std::size_t trimmed = decoder->trimForIdle(m_cachedFramesKept.load(std::memory_order_relaxed));
            m_trimmedBytes.fetch_add(trimmed, std::memory_order_relaxed);
        }
        std::unique_ptr<detail::IdleItem> item;
        if (decoder) {
            item = std::make_unique<DecoderItem>(std::move(decoder));
        }
        return m_core->park(std::move(key), std::move(item));
    } catch (...) {
        // Allocation failure: the decoder is released on the way out.
        return false;
    }
}

void GpuDecoderPool::trim() noexcept {
    if (m_core) {
        m_core->trim();
    }
}

void GpuDecoderPool::clear() noexcept {
    if (m_core) {
        m_core->clear();
    }
}

void GpuDecoderPool::setLimits(const Limits& limits) noexcept {
    m_cachedFramesKept.store(limits.cachedFramesKept, std::memory_order_relaxed);
    if (m_core) {
        m_core->setLimits(toCore(limits));
    }
}

GpuDecoderPool::Limits GpuDecoderPool::limits() const noexcept {
    Limits out;
    out.cachedFramesKept = m_cachedFramesKept.load(std::memory_order_relaxed);
    if (!m_core) {
        return out;
    }
    const detail::IdlePoolLimits core = m_core->limits();
    out.maxIdleDecoders = core.maxIdle;
    out.idleTtl = core.idleTtl;
    out.minAvailableMemoryMiB = core.minAvailableMemoryMiB;
    out.minFreeVramMiB = core.minFreeDeviceMemoryMiB;
    return out;
}

GpuDecoderPool::Stats GpuDecoderPool::stats() const noexcept {
    Stats out;
    out.trimmedBytes = m_trimmedBytes.load(std::memory_order_relaxed);
    if (!m_core) {
        return out;
    }
    const detail::IdlePoolStats core = m_core->stats();
    out.parked = core.parked;
    out.rejected = core.rejected;
    out.hits = core.hits;
    out.misses = core.misses;
    out.expired = core.expired;
    out.evictedForRoom = core.evictedForRoom;
    out.releasedForMemory = core.releasedForMemory;
    out.releasedForVram = core.releasedByItem;
    out.cleared = core.cleared;
    return out;
}

std::size_t GpuDecoderPool::idleCount() const noexcept { return m_core ? m_core->idleCount() : 0; }

bool GpuDecoderPool::reaperRunning() const noexcept { return m_core && m_core->reaperRunning(); }

}  // namespace osv::video
