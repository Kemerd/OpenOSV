// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CudaAnalysis.cpp - GPU band shading, availability, and the installation of
// both GPU analyses into osv_render_cpu's hooks.
//
// The band shading is the GPU twin of SeamAnalysis.cpp's shadeRows(): the
// same RenderJob, the same shared osvShadePixelW, the same absolute rows.
// The difference is where the frames are - device memory, straight from the
// decoder - and what comes back: either the full RGBA rows (gain estimation
// needs colour) or, for the hot path, just the luma and coverage planes the
// flow and the seam search read, reduced on the device so a quarter of the
// data crosses the bus.

#include "osv/render/CudaAnalysis.h"

#include "CudaAnalysisInternal.h"
#include "CudaAnalysisLaunch.h"
#include "CudaWorkspace.h"

#include "osv/core/Log.h"
#include "osv/render/DeviceBandShader.h"
#include "osv/render/FlowBackend.h"

#include <cuda_runtime.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace osv::render {

namespace {

using gpu::Workspace;
using gpu::WorkspaceLease;
using gpu::cudaMessage;

// ---------------------------------------------------------------------------
//  Availability
// ---------------------------------------------------------------------------

/// Devices whose kernel-image probe has been answered: 0 unknown, 1 the
/// kernels run there, 2 they do not.  The probe loads the module, which is
/// worth doing once per device rather than once per flow computation.
constexpr int kProbeSlots = 64;
std::array<std::atomic<int>, kProbeSlots> g_kernelProbe{};

/// True when this build's kernels can run on `device` (cached).
bool kernelsRunOn(int device, std::string* reason) {
    const bool cacheable = device >= 0 && device < kProbeSlots;
    if (cacheable) {
        const int known = g_kernelProbe[static_cast<std::size_t>(device)].load(std::memory_order_acquire);
        if (known == 1) {
            return true;
        }
        if (known == 2) {
            if (reason != nullptr) {
                *reason = "this build has no GPU analysis kernels for CUDA device " + std::to_string(device);
            }
            return false;
        }
    }
    cudaError_t why = cudaSuccess;
    const bool ok = gpu::analysisKernelsLoadable(&why);
    if (!ok) {
        // The probe's failure is recorded as the thread's last error; clear
        // it so it cannot be mistaken for the failure of a later launch.
        gpu::clearStaleCudaError();
        if (reason != nullptr) {
            *reason = cudaMessage(("GPU analysis kernels cannot run on CUDA device " + std::to_string(device)).c_str(),
                                  why);
        }
    }
    if (cacheable) {
        g_kernelProbe[static_cast<std::size_t>(device)].store(ok ? 1 : 2, std::memory_order_release);
    }
    return ok;
}

// ---------------------------------------------------------------------------
//  Band shading
// ---------------------------------------------------------------------------

/// Everything cudaShadeBand* refuses before touching the device.
Status checkBandJob(const RenderJob& job, std::uint32_t row0, std::uint32_t row1) {
    if (!job.valid()) {
        return failStatus(ErrorCode::InvalidArgument, "cuda band shading: invalid render job");
    }
    // Both planes must be device pointers: a host pointer handed to the
    // kernel would fault the context, and a job mixing the two is not
    // something the builder produces for one decoder.
    if (!job.planesOnDevice[0] || !job.planesOnDevice[1]) {
        return failStatus(ErrorCode::InvalidArgument,
                          "cuda band shading: both lens frames must be device-resident (planesOnDevice)");
    }
    if (row1 <= row0 || row1 > static_cast<std::uint32_t>(job.params.outH)) {
        return failStatus(ErrorCode::InvalidArgument, "cuda band shading: row range outside the map");
    }
    return okStatus();
}

/// The frames must live on the device behind the current context: a frame
/// decoded on another GPU cannot be sampled here.  (The same refusal the
/// CUDA renderer makes, for the same reason.)
Status checkFramesOnDevice(const RenderJob& job, int device) {
    for (std::size_t i = 0; i < 2; ++i) {
        const video::DeviceFrameRef& ref = job.deviceFrames[i];
        if (ref.valid() && ref.deviceIndex != device) {
            return failStatus(ErrorCode::InvalidArgument,
                              "cuda band shading: lens frame " + std::to_string(i) + " lives on CUDA device " +
                                  std::to_string(ref.deviceIndex) + ", the current context is on device " +
                                  std::to_string(device));
        }
    }
    return okStatus();
}

/// Device copies of the job's seam table and warp grid (either may be null),
/// uploaded through the pinned staging block on the lease's stream.
struct BandTables {
    const float* seam = nullptr;
    const float* warp = nullptr;
};

Result<BandTables> uploadTables(const WorkspaceLease& lease, const RenderJob& job) {
    Workspace& ws = lease.ws();
    const cudaStream_t s = lease.stream();
    // Exactly the CPU band path's rule for when each table is in force.
    const std::size_t seamCount =
        (job.params.seamShiftEnabled && !job.seamShiftDeg.empty()) ? job.seamShiftDeg.size() : 0u;
    const std::size_t warpCount = (job.params.warpEnabled && !job.warpGrid.empty()) ? job.warpGrid.size() : 0u;
    BandTables out;
    if (seamCount + warpCount == 0) {
        return out;
    }
    OSV_TRY(ws.up.ensure((seamCount + warpCount) * sizeof(float), "table staging"));
    float* up = ws.up.as<float>();
    if (seamCount > 0) {
        std::memcpy(up, job.seamShiftDeg.data(), seamCount * sizeof(float));
        OSV_TRY(ws.seamTable.ensure(seamCount * sizeof(float), "seam table"));
        const cudaError_t err =
            cudaMemcpyAsync(ws.seamTable.get(), up, seamCount * sizeof(float), cudaMemcpyHostToDevice, s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cuda band shading: upload seam table", err)};
        }
        out.seam = ws.seamTable.as<float>();
    }
    if (warpCount > 0) {
        std::memcpy(up + seamCount, job.warpGrid.data(), warpCount * sizeof(float));
        OSV_TRY(ws.warpGrid.ensure(warpCount * sizeof(float), "warp grid"));
        const cudaError_t err =
            cudaMemcpyAsync(ws.warpGrid.get(), up + seamCount, warpCount * sizeof(float), cudaMemcpyHostToDevice, s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cuda band shading: upload warp grid", err)};
        }
        out.warp = ws.warpGrid.as<float>();
    }
    return out;
}

/// Validate, lease, upload the tables: the common prologue of both shaders.
struct BandCall {
    WorkspaceLease lease;
    BandTables tables;
    OsvPlanePair planes{};
    int rows = 0;
    std::size_t pixels = 0;
};

Result<BandCall> beginBandCall(const RenderJob& job, std::uint32_t row0, std::uint32_t row1, void* stream) {
    OSV_TRY(checkBandJob(job, row0, row1));
    gpu::clearStaleCudaError();
    BandCall call;
    OSV_TRY_ASSIGN(call.lease, gpu::acquireWorkspace(stream));
    OSV_TRY(checkFramesOnDevice(job, call.lease.ws().device));
    OSV_TRY_ASSIGN(call.tables, uploadTables(call.lease, job));
    // The job's plane descriptors already hold the device addresses
    // (fillDevicePlane); the kernel receives them by value.
    call.planes.p[0] = job.planes[0];
    call.planes.p[1] = job.planes[1];
    call.rows = static_cast<int>(row1 - row0);
    call.pixels = static_cast<std::size_t>(call.rows) * static_cast<std::size_t>(job.params.outW);
    return call;
}

/// The GPU band shader installed into osv_render_cpu.
class CudaBandShader final : public DeviceBandShader {
public:
    CudaBandShader() = default;

    [[nodiscard]] const char* name() const noexcept override { return "cuda"; }

    [[nodiscard]] Result<std::vector<float>> shadeRowsRgba(const RenderJob& job, std::uint32_t row0,
                                                           std::uint32_t row1) override {
        return cudaShadeBandRgba(job, row0, row1, nullptr);
    }

    [[nodiscard]] Status shadeRowsLumaAlpha(const RenderJob& job, std::uint32_t row0, std::uint32_t row1,
                                            std::vector<float>& luma, std::vector<float>& alpha) override {
        return cudaShadeBandLumaAlpha(job, row0, row1, luma, alpha, nullptr);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
//  Availability and installation
// ---------------------------------------------------------------------------
bool cudaAnalysesAvailable(std::string* reason) {
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
    return kernelsRunOn(device.value(), reason);
}

Status installCudaAnalyses() {
    std::string reason;
    if (!cudaAnalysesAvailable(&reason)) {
        return failStatus(ErrorCode::Unsupported, "GPU analyses unavailable: " + reason);
    }
    try {
        setDeviceBandShader(std::make_shared<CudaBandShader>());
    } catch (const std::bad_alloc&) {
        return failStatus(ErrorCode::Internal, "GPU analyses: out of memory installing the band shader");
    }
    setCudaFlowBackendFactory(&gpu::makeCudaDisFlowBackend);
    log::debug("GPU analyses installed (band shading + classical-cuda flow)");
    return okStatus();
}

void uninstallCudaAnalyses() noexcept {
    // Only remove what is ours: a different shader installed since must not
    // be pulled out by a stale uninstall.
    const std::shared_ptr<DeviceBandShader> shader = deviceBandShader();
    if (shader && dynamic_cast<CudaBandShader*>(shader.get()) != nullptr) {
        setDeviceBandShader(nullptr);
    }
    if (cudaFlowBackendFactory() == &gpu::makeCudaDisFlowBackend) {
        setCudaFlowBackendFactory(nullptr);
    }
}

bool cudaAnalysesInstalled() noexcept {
    const std::shared_ptr<DeviceBandShader> shader = deviceBandShader();
    return shader && dynamic_cast<CudaBandShader*>(shader.get()) != nullptr &&
           cudaFlowBackendFactory() == &gpu::makeCudaDisFlowBackend;
}

// ---------------------------------------------------------------------------
//  Band shading entry points
// ---------------------------------------------------------------------------
Result<std::vector<float>> cudaShadeBandRgba(const RenderJob& job, std::uint32_t row0, std::uint32_t row1,
                                             void* stream) {
    try {
        OSV_TRY_ASSIGN(BandCall call, beginBandCall(job, row0, row1, stream));
        Workspace& ws = call.lease.ws();
        const cudaStream_t s = call.lease.stream();
        const std::size_t bytes = call.pixels * 4u * sizeof(float);
        OSV_TRY(ws.bandOut0.ensure(bytes, "band rgba"));
        OSV_TRY(ws.down.ensure(bytes, "band download staging"));

        cudaError_t err = gpu::launchBandRgba(job.params, call.planes, call.tables.seam, call.tables.warp,
                                              static_cast<int>(row0), call.rows, ws.bandOut0.as<float>(), s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cuda band shading: launch", err)};
        }
        err = cudaMemcpyAsync(ws.down.get(), ws.bandOut0.get(), bytes, cudaMemcpyDeviceToHost, s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cuda band shading: download", err)};
        }
        // Wait for this stream only - never the device.
        err = cudaStreamSynchronize(s);
        if (err != cudaSuccess) {
            return Error{ErrorCode::Gpu, cudaMessage("cuda band shading: execution", err)};
        }
        const float* src = ws.down.as<float>();
        return std::vector<float>(src, src + call.pixels * 4u);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::Internal, "cuda band shading: out of host memory"};
    } catch (const std::exception& e) {
        return Error{ErrorCode::Internal, std::string("cuda band shading: ") + e.what()};
    } catch (...) {
        return Error{ErrorCode::Internal, "cuda band shading: unknown exception"};
    }
}

Status cudaShadeBandLumaAlpha(const RenderJob& job, std::uint32_t row0, std::uint32_t row1, std::vector<float>& luma,
                              std::vector<float>& alpha, void* stream) {
    try {
        OSV_TRY_ASSIGN(BandCall call, beginBandCall(job, row0, row1, stream));
        Workspace& ws = call.lease.ws();
        const cudaStream_t s = call.lease.stream();
        const std::size_t planeBytes = call.pixels * sizeof(float);
        OSV_TRY(ws.bandOut0.ensure(planeBytes, "band luma"));
        OSV_TRY(ws.bandOut1.ensure(planeBytes, "band alpha"));
        OSV_TRY(ws.down.ensure(2u * planeBytes, "band download staging"));

        cudaError_t err = gpu::launchBandLumaAlpha(job.params, call.planes, call.tables.seam, call.tables.warp,
                                                   static_cast<int>(row0), call.rows, ws.bandOut0.as<float>(),
                                                   ws.bandOut1.as<float>(), s);
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("cuda band shading: launch", err));
        }
        auto* down = ws.down.as<unsigned char>();
        err = cudaMemcpyAsync(down, ws.bandOut0.get(), planeBytes, cudaMemcpyDeviceToHost, s);
        if (err == cudaSuccess) {
            err = cudaMemcpyAsync(down + planeBytes, ws.bandOut1.get(), planeBytes, cudaMemcpyDeviceToHost, s);
        }
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("cuda band shading: download", err));
        }
        err = cudaStreamSynchronize(s);
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("cuda band shading: execution", err));
        }
        const auto* planes = reinterpret_cast<const float*>(down);
        luma.assign(planes, planes + call.pixels);
        alpha.assign(planes + call.pixels, planes + 2u * call.pixels);
        return okStatus();
    } catch (const std::bad_alloc&) {
        return failStatus(ErrorCode::Internal, "cuda band shading: out of host memory");
    } catch (const std::exception& e) {
        return failStatus(ErrorCode::Internal, std::string("cuda band shading: ") + e.what());
    } catch (...) {
        return failStatus(ErrorCode::Internal, "cuda band shading: unknown exception");
    }
}

}  // namespace osv::render
