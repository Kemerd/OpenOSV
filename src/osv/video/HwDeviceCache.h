// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Internal: process-wide sharing of FFmpeg hardware device contexts.
//
// Creating a device is the single most expensive step of opening a hardware
// decoder on the sample clip: ~120 ms for a D3D11 device and ~50 ms for a
// CUDA context, every time, while the decode of the first picture costs less.
// The importer opened two of them per clip and threw both away on every
// imQuietFile, so each quiet / unquiet and each Source Settings change paid
// the creation again.
//
// The cache hands out one device per key (back-end type, adapter, slot) to
// every decoder that asks while it is alive.  It holds the devices WEAKLY: a
// device lives exactly as long as some decoder (or a frame a caller still
// holds) references it, and the next decoder after that creates a new one.
// That keeps the cache free of any teardown duty - nothing is left for a
// static destructor to release while the loader lock is held - and a process
// that stops decoding gets its GPU memory back without being told to.
//
// Sharing is safe because FFmpeg serialises every use of a D3D11 device
// (submission and surface download) behind the device context's lock, and a
// CUDA context is used through push / pop.  The price of that lock is that
// two decoders on one D3D11 device download their surfaces one after the
// other; the slot lets a caller that decodes two streams in parallel (the
// dual-lens reader) keep one device per stream.

#pragma once

#include "FfmpegCommon.h"

#include "osv/core/Result.h"

#include <memory>
#include <string>

namespace osv::video::detail {

/// One shared device.  Destroying the last shared_ptr drops the cache's
/// reference; FFmpeg frees the device once the codec contexts and frames
/// that still reference it are gone too.
class SharedHwDevice {
public:
    explicit SharedHwDevice(AVBufferRef* ref) noexcept : m_ref(ref) {}
    ~SharedHwDevice() {
        if (m_ref) {
            av_buffer_unref(&m_ref);
        }
    }
    SharedHwDevice(const SharedHwDevice&) = delete;
    SharedHwDevice& operator=(const SharedHwDevice&) = delete;

    /// The device context (never null for an object the cache handed out).
    [[nodiscard]] AVBufferRef* ref() const noexcept { return m_ref; }

private:
    AVBufferRef* m_ref = nullptr;
};

/// Result of HwDeviceCache::acquire.
struct AcquiredHwDevice {
    std::shared_ptr<SharedHwDevice> device;  ///< Keep alive for as long as the decoder uses it.
    bool reused = false;                     ///< True when a live device served the request.
};

/// Return the live device for (type, deviceArg, slot) or create one with
/// av_hwdevice_ctx_create.  `deviceArg` is FFmpeg's device string (CUDA
/// ordinal, D3D11 adapter index; empty = default).  `slot` selects one of
/// several independent devices of the same type and adapter (0 = the common
/// one); it exists so parallel decoders can avoid serialising on one device.
/// `share` false bypasses the cache and always creates a private device.
/// Thread-safe; creation happens under the cache lock so two decoders that
/// ask at the same moment end up on ONE device instead of racing to two.
/// Errors: Unsupported (the device cannot be created on this machine).
[[nodiscard]] Result<AcquiredHwDevice> acquireHwDevice(AVHWDeviceType type, const std::string& deviceArg, int slot,
                                                       bool share);

/// Number of shared devices currently alive (diagnostics and tests).
[[nodiscard]] std::size_t liveSharedHwDevices() noexcept;

}  // namespace osv::video::detail
