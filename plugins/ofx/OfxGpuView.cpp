// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxGpuView.cpp - the effects' own GPU path (OfxGpuView.h).
//
// This translation unit decides WHETHER the own-GPU path takes a frame - the
// host profile and OPENOSV_OFX_GPU, the NVIDIA driver, the clip's renderer -
// and gets the pixels onto the GPU: the clip engine's stitched sphere, left
// in VRAM by ImporterInstance::renderFrameToDevice(), or the filter's CPU
// source.  Framing, levels, the pack into the host's format and the readback
// of the finished rectangle are OfxGpuPipeline's.
//
// ===========================================================================
//  The generator's frame, end to end
// ===========================================================================
//
//   NVDEC -> VRAM (both lenses; host decode + upload when NVDEC cannot)
//     -> CudaRenderer stitch -> the sphere, float RGBA in VRAM   [renderer lock]
//     -> view kernel: camera + levels + pack -> the view in VRAM [renderer lock]
//     -> pinned bands -> the host's CPU image                   [no lock]
//
// Only the finished view crosses the bus: at 1920 x 1080 8-bit that is 8 MB
// a frame instead of the 288 MB 6K sphere the CPU framing reads back.

#include "OfxGpuView.h"

#include "OfxCuda.h"

#include "HostContext.h"
#include "PluginLog.h"

#if defined(OSV_HAVE_CUDA)
#include "osv/render/CudaRenderer.h"
#endif

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

namespace osv::ofx::gpu {

using osv::premiere::HostContext;
using osv::premiere::ImporterInstance;
using osv::premiere::PluginLog;

namespace {

// ===========================================================================
//  Policy
// ===========================================================================

/// What OPENOSV_OFX_GPU says.
enum class Switch : std::uint8_t {
    Auto,  ///< Unset or unrecognised: the host profile decides.
    Off,   ///< Every own-GPU path off.
    On,    ///< Every own-GPU path on, whatever the host.
};

/// OPENOSV_OFX_GPU, read on every call (a test flips it between renders).
[[nodiscard]] Switch readSwitch() noexcept {
    char value[16] = {};
#if defined(_WIN32)
    // getenv_s: the module is /MD and shares the CRT's environment with the
    // host process (and with a test's _putenv_s).
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), "OPENOSV_OFX_GPU") != 0 || length == 0) {
        return Switch::Auto;
    }
#else
    const char* env = std::getenv("OPENOSV_OFX_GPU");
    if (!env || env[0] == '\0') {
        return Switch::Auto;
    }
    std::strncpy(value, env, sizeof(value) - 1);
#endif
    // Case-insensitive first letters: 1 / 0, on / off, true / false, yes / no.
    const char c = value[0];
    const char d = value[1];
    const bool on = c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y' ||
                    ((c == 'o' || c == 'O') && (d == 'n' || d == 'N'));
    const bool off = c == '0' || c == 'f' || c == 'F' || c == 'n' || c == 'N' ||
                     ((c == 'o' || c == 'O') && (d == 'f' || d == 'F'));
    return on ? Switch::On : (off ? Switch::Off : Switch::Auto);
}

/// Everything below the policy that must hold before a frame is tried: a
/// build with the kernels and a working NVIDIA driver.
[[nodiscard]] bool gpuReady(std::string& why) noexcept {
    if (!pipelineBuilt()) {
        why = "this build has no CUDA kernels";
        return false;
    }
    return cuda::driverInitialised(&why);
}

/// Why the most recent hook on this thread answered "not mine" (notePath()
/// reads it; one per thread, so concurrent renders never mix reasons).
thread_local std::string t_declined;

/// Answer "not mine": nothing written, empty error, the reason kept for the
/// log.
[[nodiscard]] bool decline(std::string why, std::string& error) noexcept {
    error.clear();
    try {
        t_declined = std::move(why);
    } catch (...) {
        // The reason is only for the log.
    }
    return false;
}

/// The CUDA device the clip engine renders on: its CudaRenderer's, when one
/// exists already, else device 0 - the device the renderer factory creates
/// it on (src/osv/render/factory/RendererFactory.cpp).  The filter's own
/// work goes there too, so its primary context and kernels are the ones the
/// generator already warmed up.
[[nodiscard]] int engineDevice() noexcept {
#if defined(OSV_HAVE_CUDA)
    try {
        if (HostContext::exists()) {
            HostContext& host = HostContext::instance();
            bool probed = false;
            // Only an EXISTING renderer is asked: creating one here would
            // initialise the CUDA runtime for a filter that never needs it.
            if (host.backendAvailable("cuda", &probed) && probed) {
                auto lease = host.acquireRenderer(std::string_view("cuda"));
                if (lease.ok() && lease.value().renderer) {
                    if (const auto* cuda = dynamic_cast<const render::CudaRenderer*>(lease.value().renderer.get())) {
                        const int device = cuda->deviceIndex();
                        if (device >= 0) {
                            return device;
                        }
                    }
                }
            }
        }
    } catch (...) {
        // Fall through to the factory's device.
    }
#endif
    return 0;
}

/// The shared pool for band copies (the CPU paths' pool), or null.
[[nodiscard]] std::shared_ptr<ThreadPool> copyPool() noexcept {
    try {
        return HostContext::instance().threadPoolShared();
    } catch (...) {
        return nullptr;
    }
}

/// Width and height of a camera frame, 0 for an empty one.
[[nodiscard]] int frameWidth(const OfxRectI& r) noexcept { return r.x2 > r.x1 ? r.x2 - r.x1 : 0; }
[[nodiscard]] int frameHeight(const OfxRectI& r) noexcept { return r.y2 > r.y1 ? r.y2 - r.y1 : 0; }

/// The generator hooks' shared body: render `geometry` on the clip's CUDA
/// renderer into VRAM, let `phaseOne` frame or pack it there (under the
/// renderer lock), then read the result back into the target (outside it).
template <class PhaseOne>
[[nodiscard]] bool renderClipOnDevice(ImporterInstance& clip, std::uint32_t index,
                                      const premiere::OutputGeometry& geometry, bool draft,
                                      premiere::RenderPurpose purpose, FrameJob& job, const PhaseOne& phaseOne,
                                      std::string& error) {
    // Check a slot out on the engine's device BEFORE the renderer lock is
    // taken: a wait for a slot must not hold every other clip's stitch up.
    // Best effort - phase 1 reserves on the frame's real device anyway.
    {
        std::string ignored;
        (void)job.reserve(engineDevice(), ignored);
    }
    std::string phaseError;
    std::string whyNot;
    const auto consume = [&](const ImporterInstance::DeviceFrame& frame) -> Status {
        DeviceRgba sphere;
        sphere.data = frame.data;
        sphere.pitchBytes = frame.pitchBytes;
        sphere.width = frame.width;
        sphere.height = frame.height;
        sphere.device = frame.deviceIndex;
        if (!phaseOne(sphere, phaseError)) {
            return failStatus(ErrorCode::Gpu, phaseError.empty() ? std::string("the GPU framing failed") : phaseError);
        }
        return okStatus();
    };
    // The clip's own output transfer (-1), as renderFrame() uses it here.
    auto served = clip.renderFrameToDevice(index, geometry, draft, purpose, -1, consume, &whyNot);
    if (!served.ok()) {
        error = served.error().message.empty() ? std::string("the GPU stitch failed") : served.error().message;
        return false;
    }
    if (!served.value()) {
        return decline(whyNot.empty() ? std::string("the clip has no GPU frame path") : whyNot, error);
    }
    // Phase 2, outside every lock but the caller's clip lock.
    std::shared_ptr<ThreadPool> pool = copyPool();
    if (!job.readBack(pool.get(), error)) {
        if (error.empty()) {
            error = "the GPU readback failed";
        }
        return false;
    }
    return true;
}

// ===========================================================================
//  The per-instance path log
// ===========================================================================

std::mutex g_notedMutex;
std::set<std::pair<const void*, int>> g_noted;

/// Instances remembered before the set is cleared: a long session with
/// thousands of razor cuts must not grow it without bound (an instance that
/// is forgotten logs its path once more, which is harmless).
constexpr std::size_t kMaxNoted = 4096;

[[nodiscard]] const char* hookName(Hook hook) noexcept {
    switch (hook) {
    case Hook::SourceView:     return "OpenOSV Source (reframed view)";
    case Hook::SourceEquirect: return "OpenOSV Source (360 equirect)";
    case Hook::ReframeFilter:  return "OpenOSV 360 Reframe";
    }
    return "?";
}

}  // namespace

// ===========================================================================
//  Policy (public)
// ===========================================================================

bool ownGpuWanted(std::string* why) noexcept {
    try {
        switch (readSwitch()) {
        case Switch::Off:
            if (why) {
                *why = "OPENOSV_OFX_GPU switches the own-GPU path off";
            }
            return false;
        case Switch::On:
            return true;
        case Switch::Auto:
        default:
            break;
        }
        const HostProfile profile = hostProfile();
        if (profile == HostProfile::Vegas) {
            return true;
        }
        if (why) {
            *why = std::string("the host profile is '") + hostProfileName(profile) +
                   "' (the own-GPU path serves VEGAS; OPENOSV_OFX_GPU=1 forces it)";
        }
        return false;
    } catch (...) {
        return false;
    }
}

// ===========================================================================
//  The three hooks
// ===========================================================================

bool renderSourceViewGpu(ImporterInstance& clip, std::uint32_t index, const premiere::OutputGeometry& sphere,
                         bool draft, premiere::RenderPurpose purpose, const reframe::Settings& settings,
                         reframe::SizePx projectSize, const HostTarget& target, std::string& error) noexcept {
    error.clear();
    try {
        // ---- does this path take the frame at all? ------------------------------
        std::string why;
        if (!ownGpuWanted(&why) || !gpuReady(why)) {
            return decline(why, error);
        }
        if (!sphere.valid() || sphere.view) {
            return decline("no equirect sphere geometry for the clip", error);
        }
        // ---- the camera: buildParams()'s own, for the camera frame ---------------
        const int frameW = frameWidth(target.frame);
        const int frameH = frameHeight(target.frame);
        if (frameW <= 0 || frameH <= 0) {
            error = "the camera frame is empty";
            return false;
        }
        const reframe::ViewSetup view = reframe::buildView(settings, frameW, frameH, projectSize);
        if (!view.valid) {
            error = std::string("no camera for a ") + std::to_string(frameW) + "x" + std::to_string(frameH) +
                    " frame (" + reframe::setupRejectName(view.reject) + ")";
            return false;
        }
        // After cuInit (gpuReady), so the guard records the real current
        // context even on the process's first GPU frame.
        cuda::CurrentContextGuard guard;
        FrameJob job;
        return renderClipOnDevice(
            clip, index, sphere, draft, purpose, job,
            [&](const DeviceRgba& image, std::string& phaseError) {
                return job.frameDevice(view.params, image, target, phaseError);
            },
            error);
    } catch (const std::exception& e) {
        error = std::string("exception in the GPU view path: ") + e.what();
        return false;
    } catch (...) {
        error = "exception in the GPU view path";
        return false;
    }
}

bool renderSourceEquirectGpu(ImporterInstance& clip, std::uint32_t index, bool draft,
                             premiere::RenderPurpose purpose, const HostTarget& target, std::string& error) noexcept {
    error.clear();
    try {
        std::string why;
        if (!ownGpuWanted(&why) || !gpuReady(why)) {
            return decline(why, error);
        }
        // ---- the sphere, stitched straight at the camera frame's size ----------
        const int frameW = frameWidth(target.frame);
        const int frameH = frameHeight(target.frame);
        if (frameW <= 0 || frameH <= 0) {
            error = "the camera frame is empty";
            return false;
        }
        const premiere::OutputGeometry geometry{frameW, frameH};
        cuda::CurrentContextGuard guard;
        FrameJob job;
        return renderClipOnDevice(
            clip, index, geometry, draft, purpose, job,
            [&](const DeviceRgba& image, std::string& phaseError) {
                return job.packDevice(image, target, phaseError);
            },
            error);
    } catch (const std::exception& e) {
        error = std::string("exception in the GPU sphere path: ") + e.what();
        return false;
    } catch (...) {
        error = "exception in the GPU sphere path";
        return false;
    }
}

bool renderReframeFromHostGpu(const reframe::KernelSetup& setup, const HostImageView& source,
                              const HostTarget& target, std::string& error) noexcept {
    error.clear();
    try {
        std::string why;
        if (!ownGpuWanted(&why) || !gpuReady(why)) {
            return decline(why, error);
        }
        if (!setup.valid) {
            error = std::string("the camera setup is invalid (") + reframe::setupRejectName(setup.reject) + ")";
            return false;
        }
        // Only the camera half of the setup is used: the source is uploaded
        // from `source` itself, whose depth and order describe its bytes.  A
        // setup built for another picture size is a caller mistake.
        if (setup.source.w != 0 && (setup.source.w != source.width() || setup.source.h != source.height())) {
            error = "the camera setup describes another source size";
            return false;
        }
        cuda::CurrentContextGuard guard;
        std::shared_ptr<ThreadPool> pool = copyPool();
        if (!frameHostImage(setup.params, source, target, engineDevice(), pool.get(), error)) {
            if (error.empty()) {
                error = "the GPU filter path failed";
            }
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string("exception in the GPU filter path: ") + e.what();
        return false;
    } catch (...) {
        error = "exception in the GPU filter path";
        return false;
    }
}

// ===========================================================================
//  The per-instance path log
// ===========================================================================

void notePath(const void* instance, Hook hook, bool servedByGpu, const std::string& gpuError) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(g_notedMutex);
            const std::pair<const void*, int> key{instance, static_cast<int>(hook)};
            if (!g_noted.insert(key).second) {
                return;  // this instance's first frame was noted already
            }
            if (g_noted.size() > kMaxNoted) {
                g_noted.clear();
                g_noted.insert(key);
            }
        }
        const char* name = hookName(hook);
        if (servedByGpu) {
            PluginLog::info("ofx gpu: {} instance {}: first frame on the own GPU path (host profile '{}')", name,
                            instance, hostProfileName(hostProfile()));
        } else if (!gpuError.empty()) {
            PluginLog::info("ofx gpu: {} instance {}: first frame on the CPU path - the GPU path failed: {}", name,
                            instance, gpuError);
        } else {
            PluginLog::info("ofx gpu: {} instance {}: first frame on the CPU path - {}", name, instance,
                            t_declined.empty() ? std::string("no GPU path serves it") : t_declined);
        }
    } catch (...) {
        // Logging only.
    }
}

}  // namespace osv::ofx::gpu
