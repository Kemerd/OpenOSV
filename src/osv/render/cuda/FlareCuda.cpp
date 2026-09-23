// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlareCuda.cpp - the CUDA FlareDeviceSampler: the flare analysis's working
// image computed from a lens frame in VRAM (see osv/render/FlareCuda.h).
//
// Everything here follows the rules of the other GPU analyses
// (CudaWorkspace.h): the context current on the calling thread is used as
// is, work is ordered on the lease's stream and only that stream is waited
// on, and every failure comes back as a Status with the runtime's own words.

#include "osv/render/FlareCuda.h"

#include "CudaAnalysisInternal.h"
#include "CudaWorkspace.h"
#include "FlareLaunch.h"

#include "osv/core/Log.h"

#include <cuda_runtime.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace osv::render {

namespace {

using gpu::cudaMessage;

// ---------------------------------------------------------------------------
//  Availability (probed once per device)
// ---------------------------------------------------------------------------

/// 0 unknown, 1 the flare kernel runs on the device, 2 it does not.
constexpr int kProbeSlots = 64;
std::array<std::atomic<int>, kProbeSlots> g_flareProbe{};

/// True when the flare kernel can run on `device` (cached per device).
bool flareRunsOn(int device, std::string* reason) {
    const bool cacheable = device >= 0 && device < kProbeSlots;
    if (cacheable) {
        const int known = g_flareProbe[static_cast<std::size_t>(device)].load(std::memory_order_acquire);
        if (known == 1) {
            return true;
        }
        if (known == 2) {
            if (reason != nullptr) {
                *reason = "this build has no flare kernel for CUDA device " + std::to_string(device);
            }
            return false;
        }
    }
    cudaError_t why = cudaSuccess;
    const bool ok = gpu::flareKernelLoadable(&why);
    if (!ok) {
        // The probe's failure sits in the thread's last-error slot; clear it
        // so a later, unrelated launch check does not report it.
        gpu::clearStaleCudaError();
        if (reason != nullptr) {
            *reason = cudaMessage(("flare kernel cannot run on CUDA device " + std::to_string(device)).c_str(), why);
        }
    }
    if (cacheable) {
        g_flareProbe[static_cast<std::size_t>(device)].store(ok ? 1 : 2, std::memory_order_release);
    }
    return ok;
}

/// A device frame must live on the device behind the current context: a
/// pointer from another GPU (or a host pointer mislabelled as device) would
/// fault the context when the kernel reads it.
Status checkDevicePointer(const void* p, int device, const char* what) {
    cudaPointerAttributes attr{};
    const cudaError_t err = cudaPointerGetAttributes(&attr, p);
    if (err != cudaSuccess) {
        gpu::clearStaleCudaError();
        return failStatus(ErrorCode::InvalidArgument, cudaMessage(what, err));
    }
    if (attr.type != cudaMemoryTypeDevice && attr.type != cudaMemoryTypeManaged) {
        return failStatus(ErrorCode::InvalidArgument,
                          std::string(what) + ": not device memory (the frame is not in VRAM)");
    }
    if (attr.device != device) {
        return failStatus(ErrorCode::InvalidArgument, std::string(what) + ": lives on CUDA device " +
                                                          std::to_string(attr.device) +
                                                          ", the current context is on device " +
                                                          std::to_string(device));
    }
    return okStatus();
}

/// The sampler installed into osv_render_cpu.
class CudaFlareSampler final : public FlareDeviceSampler {
public:
    [[nodiscard]] Result<FlareImage> downsample(const OsvPlane& devicePlane, const OsvColorParams& color,
                                                std::uint32_t factor) override {
        return cudaFlareDownsample(devicePlane, true, color, factor, nullptr);
    }

    [[nodiscard]] const char* name() const noexcept override { return "cuda"; }
};

}  // namespace

// ---------------------------------------------------------------------------
//  Availability and installation
// ---------------------------------------------------------------------------

bool cudaFlareAvailable(std::string* reason) {
    int count = 0;
    const cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess) {
        gpu::clearStaleCudaError();
        if (reason != nullptr) {
            *reason = cudaMessage("cudaGetDeviceCount", err);
        }
        return false;
    }
    if (count <= 0) {
        if (reason != nullptr) {
            *reason = "no CUDA devices";
        }
        return false;
    }
    const Result<int> device = gpu::currentDevice();
    if (!device.ok()) {
        if (reason != nullptr) {
            *reason = device.error().message;
        }
        return false;
    }
    return flareRunsOn(device.value(), reason);
}

Status installCudaFlareSampler() {
    std::string reason;
    if (!cudaFlareAvailable(&reason)) {
        return failStatus(ErrorCode::Unsupported, "GPU flare sampler unavailable: " + reason);
    }
    try {
        setFlareDeviceSampler(std::make_shared<CudaFlareSampler>());
    } catch (const std::bad_alloc&) {
        return failStatus(ErrorCode::Internal, "GPU flare sampler: out of memory");
    }
    log::debug("GPU flare sampler installed");
    return okStatus();
}

void uninstallCudaFlareSampler() noexcept {
    // Only remove our own: a different sampler installed since stays.
    const std::shared_ptr<FlareDeviceSampler> sampler = flareDeviceSampler();
    if (sampler && dynamic_cast<CudaFlareSampler*>(sampler.get()) != nullptr) {
        setFlareDeviceSampler(nullptr);
    }
}

// ---------------------------------------------------------------------------
//  The downsample
// ---------------------------------------------------------------------------

Result<FlareImage> cudaFlareDownsample(const OsvPlane& plane, bool planeOnDevice, const OsvColorParams& color,
                                       std::uint32_t factor, void* stream) {
    // ---- validate everything the kernel will index ---------------------------
    if (factor < 1 || factor > 16) {
        return Error{ErrorCode::InvalidArgument, "cudaFlareDownsample: factor must be 1..16"};
    }
    if (!plane.y || !plane.u || !plane.v || plane.w <= 0 || plane.h <= 0 || plane.cw <= 0 || plane.ch <= 0 ||
        plane.strideY < plane.w || plane.bitShift < 0 || plane.bitShift > 15) {
        return Error{ErrorCode::InvalidArgument, "cudaFlareDownsample: malformed plane"};
    }
    const int chromaStep = plane.chromaInterleaved ? 2 : 1;
    if (plane.strideC < plane.cw * chromaStep) {
        return Error{ErrorCode::InvalidArgument, "cudaFlareDownsample: chroma stride shorter than a row"};
    }
    try {
        gpu::clearStaleCudaError();
        OSV_TRY_ASSIGN(gpu::WorkspaceLease lease, gpu::acquireWorkspace(stream));
        gpu::Workspace& ws = lease.ws();
        const cudaStream_t s = lease.stream();

        FlareImage img;
        img.factor = factor;
        img.w = flareAnalysisSize(static_cast<std::uint32_t>(plane.w), factor);
        img.h = flareAnalysisSize(static_cast<std::uint32_t>(plane.h), factor);
        const std::size_t count = static_cast<std::size_t>(img.w) * img.h * 3u;
        const std::size_t bytes = count * sizeof(float);
        // The workspace's band buffers are grow-only scratch owned by this
        // lease for the duration of the call.
        OSV_TRY(ws.bandOut0.ensure(bytes, "flare working image"));
        OSV_TRY(ws.down.ensure(bytes, "flare download staging"));

        // ---- the planes the kernel reads ------------------------------------
        OsvPlane dev = plane;
        // Host planes (tests and tools): upload into blocks that live for
        // this call only.  Production frames are already in VRAM.
        gpu::DeviceBlock upY;
        gpu::DeviceBlock upU;
        gpu::DeviceBlock upV;
        if (planeOnDevice) {
            OSV_TRY(checkDevicePointer(plane.y, ws.device, "cudaFlareDownsample: luma plane"));
            OSV_TRY(checkDevicePointer(plane.u, ws.device, "cudaFlareDownsample: chroma plane"));
        } else {
            const std::size_t lumaBytes =
                static_cast<std::size_t>(plane.strideY) * static_cast<std::size_t>(plane.h) * sizeof(osv_u16);
            const std::size_t chromaBytes =
                static_cast<std::size_t>(plane.strideC) * static_cast<std::size_t>(plane.ch) * sizeof(osv_u16);
            OSV_TRY(upY.ensure(lumaBytes, "flare luma upload"));
            OSV_TRY(upU.ensure(chromaBytes, "flare chroma upload"));
            cudaError_t err = cudaMemcpyAsync(upY.get(), plane.y, lumaBytes, cudaMemcpyHostToDevice, s);
            if (err == cudaSuccess) {
                err = cudaMemcpyAsync(upU.get(), plane.u, chromaBytes, cudaMemcpyHostToDevice, s);
            }
            dev.y = upY.as<const osv_u16>();
            dev.u = upU.as<const osv_u16>();
            if (plane.chromaInterleaved) {
                // Cr follows Cb in the same buffer.
                dev.v = dev.u + 1;
            } else {
                OSV_TRY(upV.ensure(chromaBytes, "flare chroma upload"));
                if (err == cudaSuccess) {
                    err = cudaMemcpyAsync(upV.get(), plane.v, chromaBytes, cudaMemcpyHostToDevice, s);
                }
                dev.v = upV.as<const osv_u16>();
            }
            if (err != cudaSuccess) {
                return Error{ErrorCode::Gpu, cudaMessage("cudaFlareDownsample: upload", err)};
            }
        }

        // ---- launch, download, wait -------------------------------------------
        cudaError_t err = gpu::launchFlareDownsample(dev, color, static_cast<int>(factor), static_cast<int>(img.w),
                                                     static_cast<int>(img.h), ws.bandOut0.as<float>(), s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cudaFlareDownsample: launch", err)};
        }
        err = cudaMemcpyAsync(ws.down.get(), ws.bandOut0.get(), bytes, cudaMemcpyDeviceToHost, s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cudaFlareDownsample: download", err)};
        }
        err = cudaStreamSynchronize(s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cudaFlareDownsample: execution", err)};
        }
        const float* host = ws.down.as<const float>();
        img.rgb.assign(host, host + count);
        return img;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "cudaFlareDownsample: out of host memory"};
    } catch (const std::exception& e) {
        return Error{ErrorCode::Internal, std::string("cudaFlareDownsample: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Internal, "cudaFlareDownsample: unknown exception"};
    }
}

}  // namespace osv::render
