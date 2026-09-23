// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReaderPool: the typed front of detail::IdlePool for DualStreamReaders.
// This file only decides what makes two readers interchangeable (the match
// key) and what a parked reader is; the parking itself - bounds, idle
// expiry, memory pressure, the reaper thread - lives in IdlePool.cpp and is
// shared with GpuDecoderPool.

#include "osv/video/ReaderPool.h"

#include "FileIdentity.h"
#include "IdlePool.h"

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

/// Pointer as fixed-width hex, for the key (two contexts must never collide).
std::wstring pointerText(const void* p) {
    wchar_t buffer[32] = {};
    std::swprintf(buffer, std::size(buffer), L"%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));
    return buffer;
}

/// Everything a parked reader must share with a request to be handed out for
/// it, as one string: the file version, the tracks, the layout and every
/// DecoderOptions field except deferFirstFrame (which only changes what
/// open() does - an open reader is the same reader either way).  '|' cannot
/// occur in a Windows path, so the fields can never run into each other.
std::wstring makeKey(const std::wstring& file, const std::array<std::uint32_t, 2>& tracks, bool sideBySide,
                     const DecoderOptions& o) {
    std::wstring k = file;
    k += L"|tracks=" + std::to_wstring(tracks[0]) + L"," + std::to_wstring(tracks[1]);
    k += L"|sbs=" + std::to_wstring(sideBySide ? 1 : 0);
    k += L"|hw=" + std::to_wstring(static_cast<int>(o.hw));
    k += L"|threads=" + std::to_wstring(o.threads);
    k += L"|device=" + std::to_wstring(o.keepOnDevice ? 1 : 0);
    k += L"|cudaDevice=" + std::to_wstring(o.cudaDeviceIndex);
    k += L"|cudaContext=" + pointerText(o.cudaContext);
    k += L"|cudaStream=" + pointerText(o.cudaStream);
    k += L"|samples=" + std::to_wstring(o.useContainerSamples ? 1 : 0);
    k += L"|shareHw=" + std::to_wstring(o.shareHwDevice ? 1 : 0);
    k += L"|slot=" + std::to_wstring(o.hwDeviceSlot);
    return k;
}

/// The tracks DualStreamReader::open would decode for `format` (kept in step
/// with it: the proxy decodes its one track for both lenses).
std::array<std::uint32_t, 2> tracksFor(const meta::FormatInfo& format) noexcept {
    if (format.sideBySideProxy) {
        const std::uint32_t t = format.videoTrackIds[0] != 0 ? format.videoTrackIds[0] : 1u;
        return {t, t};
    }
    return {format.videoTrackIds[0], format.videoTrackIds[1]};
}

// -----------------------------------------------------------------------------
//  The parked item
// -----------------------------------------------------------------------------

/// One parked reader.  A reader that is no longer open (it never should be)
/// asks to be released rather than handed out.
class ReaderItem final : public detail::IdleItem {
public:
    explicit ReaderItem(std::unique_ptr<DualStreamReader> reader) noexcept : m_reader(std::move(reader)) {}

    [[nodiscard]] bool mustRelease(std::uint64_t /*minFreeDeviceMemoryMiB*/) noexcept override {
        return !m_reader || !m_reader->isOpen();
    }

    /// Hand the reader back (the item is empty afterwards).
    [[nodiscard]] std::unique_ptr<DualStreamReader> release() noexcept { return std::move(m_reader); }

private:
    std::unique_ptr<DualStreamReader> m_reader;
};

/// Core limits from the reader pool's (no device floor: readers copy their
/// frames to host memory, and their hardware surfaces are bounded by count
/// and time).
detail::IdlePoolLimits toCore(const ReaderPool::Limits& limits) noexcept {
    detail::IdlePoolLimits core;
    core.maxIdle = limits.maxIdleReaders;
    core.idleTtl = limits.idleTtl;
    core.minAvailableMemoryMiB = limits.minAvailableMemoryMiB;
    core.minFreeDeviceMemoryMiB = 0;
    return core;
}

}  // namespace

// =============================================================================
//  ReaderPool
// =============================================================================
ReaderPool::ReaderPool() : ReaderPool(Limits{}) {}

ReaderPool::ReaderPool(const Limits& limits)
    : m_core(std::make_unique<detail::IdlePool>("reader pool", toCore(limits))) {}

ReaderPool::~ReaderPool() = default;

ReaderPool& ReaderPool::instance() {
    // Never destroyed on purpose: releasing decoders from a static destructor
    // would join libavcodec's threads under the loader lock (see the header).
    static ReaderPool* const pool = new ReaderPool();
    return *pool;
}

std::unique_ptr<DualStreamReader> ReaderPool::take(const std::filesystem::path& path, const meta::FormatInfo& format,
                                                   const DecoderOptions& options) noexcept {
    if (!m_core) {
        return nullptr;
    }
    try {
        // The identity is read before the pool lock (two file-system calls).
        // A file that cannot be stat'ed still counts as a miss.
        auto identity = detail::fileIdentity(path);
        const std::wstring key =
            identity.ok() ? makeKey(identity.value(), tracksFor(format), format.sideBySideProxy, options) : L"";
        std::unique_ptr<detail::IdleItem> item = m_core->take(key);
        if (!item) {
            return nullptr;
        }
        // Only ReaderItems are ever parked in this pool's core.
        auto* reader = static_cast<ReaderItem*>(item.get());
        return reader->release();
    } catch (...) {
        return nullptr;
    }
}

bool ReaderPool::park(std::unique_ptr<DualStreamReader> reader) noexcept {
    if (!m_core) {
        return false;
    }
    try {
        // A reader that is not open, or whose file identity is unknown, can
        // never be matched; the core refuses the empty key and releases it.
        std::wstring key;
        if (reader && reader->isOpen() && !reader->fileIdentity().empty()) {
            key = makeKey(reader->fileIdentity(), reader->trackIds(), reader->isSideBySide(), reader->options());
        }
        std::unique_ptr<detail::IdleItem> item;
        if (reader) {
            item = std::make_unique<ReaderItem>(std::move(reader));
        }
        return m_core->park(std::move(key), std::move(item));
    } catch (...) {
        // Allocation failure: the reader (wherever it is now) is released on
        // the way out, exactly as a refusal would.
        return false;
    }
}

void ReaderPool::trim() noexcept {
    if (m_core) {
        m_core->trim();
    }
}

void ReaderPool::clear() noexcept {
    if (m_core) {
        m_core->clear();
    }
}

void ReaderPool::setLimits(const Limits& limits) noexcept {
    if (m_core) {
        m_core->setLimits(toCore(limits));
    }
}

ReaderPool::Limits ReaderPool::limits() const noexcept {
    Limits out;
    if (!m_core) {
        return out;
    }
    const detail::IdlePoolLimits core = m_core->limits();
    out.maxIdleReaders = core.maxIdle;
    out.idleTtl = core.idleTtl;
    out.minAvailableMemoryMiB = core.minAvailableMemoryMiB;
    return out;
}

ReaderPool::Stats ReaderPool::stats() const noexcept {
    Stats out;
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
    out.cleared = core.cleared;
    return out;
}

std::size_t ReaderPool::idleCount() const noexcept { return m_core ? m_core->idleCount() : 0; }

bool ReaderPool::reaperRunning() const noexcept { return m_core && m_core->reaperRunning(); }

}  // namespace osv::video
