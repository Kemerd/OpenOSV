// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HwDeviceCache: the weak, process-wide table of shared FFmpeg hardware
// devices (see HwDeviceCache.h for why it exists and why it is weak).

#include "HwDeviceCache.h"

#include "osv/core/Log.h"

#include <iterator>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

namespace osv::video::detail {

namespace {

/// Key of one shared device: back-end type, FFmpeg device string, slot.
using DeviceKey = std::tuple<int, std::string, int>;

/// The table.  Only weak references live here, so the table owns no GPU
/// resource and its destruction at process exit releases nothing.
struct DeviceTable {
    std::mutex mutex;
    std::map<DeviceKey, std::weak_ptr<SharedHwDevice>> devices;
};

/// The one table of the process (created on first use).
DeviceTable& table() {
    static DeviceTable instance;
    return instance;
}

/// Create a device with FFmpeg.  Unsupported when the back-end is missing
/// on this machine (no GPU, no driver, a remote desktop session, ...).
Result<AVBufferRef*> createDevice(AVHWDeviceType type, const std::string& deviceArg) {
    AVBufferRef* device = nullptr;
    const char* arg = deviceArg.empty() ? nullptr : deviceArg.c_str();
    const int ret = av_hwdevice_ctx_create(&device, type, arg, nullptr, 0);
    if (ret < 0 || !device) {
        if (device) {
            av_buffer_unref(&device);
        }
        const char* typeName = av_hwdevice_get_type_name(type);
        return Error{ErrorCode::Unsupported, std::string("cannot create ") + (typeName ? typeName : "hardware") +
                                                 " device: " + ff::errorString(ret)};
    }
    return device;
}

}  // namespace

Result<AcquiredHwDevice> acquireHwDevice(AVHWDeviceType type, const std::string& deviceArg, int slot, bool share) {
    if (type == AV_HWDEVICE_TYPE_NONE) {
        return Error{ErrorCode::InvalidArgument, "no hardware device type"};
    }
    // A negative slot is a caller bug; fold it onto the common device rather
    // than inventing a key nobody else can hit.
    if (slot < 0) {
        slot = 0;
    }

    // ---- private device: the historical one-device-per-decoder behaviour ----
    if (!share) {
        OSV_TRY_ASSIGN(AVBufferRef* raw, createDevice(type, deviceArg));
        AcquiredHwDevice out;
        out.device = std::make_shared<SharedHwDevice>(raw);
        out.reused = false;
        return out;
    }

    // ---- shared device ---------------------------------------------------------
    // Creation stays under the lock on purpose: the dual-lens reader opens both
    // lenses at the same moment, and without the lock both would miss the
    // table and create a device each - the very cost this cache removes.
    DeviceTable& t = table();
    std::lock_guard<std::mutex> guard(t.mutex);
    const DeviceKey key{static_cast<int>(type), deviceArg, slot};
    auto it = t.devices.find(key);
    if (it != t.devices.end()) {
        if (std::shared_ptr<SharedHwDevice> live = it->second.lock()) {
            AcquiredHwDevice out;
            out.device = std::move(live);
            out.reused = true;
            return out;
        }
    }
    OSV_TRY_ASSIGN(AVBufferRef* raw, createDevice(type, deviceArg));
    std::shared_ptr<SharedHwDevice> created;
    try {
        created = std::make_shared<SharedHwDevice>(raw);
    } catch (...) {
        // The allocation failed after FFmpeg succeeded: do not leak the device.
        av_buffer_unref(&raw);
        return Error{ErrorCode::Internal, "cannot allocate the shared device record"};
    }
    t.devices[key] = created;

    // Drop entries whose device has died so the table cannot grow with keys
    // that were used once (a new CUDA ordinal, a new slot) and never again.
    for (auto e = t.devices.begin(); e != t.devices.end();) {
        e = e->second.expired() ? t.devices.erase(e) : std::next(e);
    }
    const char* typeName = av_hwdevice_get_type_name(type);
    log::debug("video: created shared {} device (adapter '{}', slot {})", typeName ? typeName : "hardware",
               deviceArg.empty() ? "default" : deviceArg, slot);

    AcquiredHwDevice out;
    out.device = std::move(created);
    out.reused = false;
    return out;
}

std::size_t liveSharedHwDevices() noexcept {
    try {
        DeviceTable& t = table();
        std::lock_guard<std::mutex> guard(t.mutex);
        std::size_t live = 0;
        for (const auto& entry : t.devices) {
            live += entry.second.expired() ? 0u : 1u;
        }
        return live;
    } catch (...) {
        // std::mutex::lock can throw system_error; a diagnostic must not.
        return 0;
    }
}

}  // namespace osv::video::detail
