// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CUDA backend host side: device memory, uploads, launch (via CudaKernel.cu)
// and readback.  Compiled by MSVC; only the kernel goes through nvcc.

#include "osv/render/CudaRenderer.h"
#include "CudaLaunch.h"
#include "osv/core/Log.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <mutex>

namespace osv::render {

namespace {

/// Format a CUDA error for a Result message.
std::string cudaMessage(const char* what, cudaError_t err) {
    return std::string(what) + ": " + cudaGetErrorString(err) + " (" + std::to_string(static_cast<int>(err)) + ")";
}

/// A pitched device buffer that is re-allocated only when it grows.
struct DeviceBuffer {
    void* ptr = nullptr;
    std::size_t pitch = 0;
    std::size_t widthBytes = 0;
    std::size_t height = 0;

    Status ensure(std::size_t wBytes, std::size_t h) {
        if (ptr && wBytes <= widthBytes && h <= height) {
            return okStatus();
        }
        release();
        void* p = nullptr;
        std::size_t pitchOut = 0;
        const cudaError_t err = cudaMallocPitch(&p, &pitchOut, wBytes, h);
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("cudaMallocPitch", err));
        }
        ptr = p;
        pitch = pitchOut;
        widthBytes = wBytes;
        height = h;
        return okStatus();
    }

    void release() noexcept {
        if (ptr) {
            cudaFree(ptr);
            ptr = nullptr;
        }
        pitch = widthBytes = height = 0;
    }
};

/// Pinned host staging buffer for the readback.
struct PinnedBuffer {
    void* ptr = nullptr;
    std::size_t bytes = 0;

    Status ensure(std::size_t needed) {
        if (ptr && needed <= bytes) {
            return okStatus();
        }
        release();
        void* p = nullptr;
        const cudaError_t err = cudaHostAlloc(&p, needed, cudaHostAllocDefault);
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("cudaHostAlloc", err));
        }
        ptr = p;
        bytes = needed;
        return okStatus();
    }

    void release() noexcept {
        if (ptr) {
            cudaFreeHost(ptr);
            ptr = nullptr;
        }
        bytes = 0;
    }
};

}  // namespace

// -----------------------------------------------------------------------------
//  Implementation state
// -----------------------------------------------------------------------------
struct CudaRenderer::Impl {
    int device = 0;
    cudaStream_t stream = nullptr;
    // Per lens: Y plane, chroma plane(s).  Planar sources need two chroma
    // buffers; interleaved sources use only `uv`.
    DeviceBuffer y[2];
    DeviceBuffer u[2];
    DeviceBuffer v[2];
    DeviceBuffer seam;
    DeviceBuffer out;
    PinnedBuffer staging;
    std::mutex mutex;  // one render at a time per renderer

    ~Impl() {
        if (stream) {
            cudaStreamSynchronize(stream);
            cudaStreamDestroy(stream);
        }
        for (int i = 0; i < 2; ++i) {
            y[i].release();
            u[i].release();
            v[i].release();
        }
        seam.release();
        out.release();
        staging.release();
    }

    /// Upload one plane (planar or interleaved) into device memory and fill
    /// the device-side OsvPlane descriptor.
    Status uploadPlane(int lens, const OsvPlane& host, OsvPlane& dev) {
        const std::size_t lumaBytes = static_cast<std::size_t>(host.w) * sizeof(osv_u16);
        OSV_TRY(y[lens].ensure(lumaBytes, static_cast<std::size_t>(host.h)));
        cudaError_t err = cudaMemcpy2DAsync(y[lens].ptr, y[lens].pitch, host.y,
                                            static_cast<std::size_t>(host.strideY) * sizeof(osv_u16), lumaBytes,
                                            static_cast<std::size_t>(host.h), cudaMemcpyHostToDevice, stream);
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("upload luma", err));
        }
        dev = host;
        dev.y = static_cast<const osv_u16*>(y[lens].ptr);
        dev.strideY = static_cast<int>(y[lens].pitch / sizeof(osv_u16));

        if (host.chromaInterleaved) {
            // One interleaved CbCr plane, two samples per chroma pixel.
            const std::size_t chromaBytes = static_cast<std::size_t>(host.cw) * 2 * sizeof(osv_u16);
            OSV_TRY(u[lens].ensure(chromaBytes, static_cast<std::size_t>(host.ch)));
            err = cudaMemcpy2DAsync(u[lens].ptr, u[lens].pitch, host.u,
                                    static_cast<std::size_t>(host.strideC) * sizeof(osv_u16), chromaBytes,
                                    static_cast<std::size_t>(host.ch), cudaMemcpyHostToDevice, stream);
            if (err != cudaSuccess) {
                return failStatus(ErrorCode::Gpu, cudaMessage("upload chroma", err));
            }
            dev.u = static_cast<const osv_u16*>(u[lens].ptr);
            dev.v = dev.u + 1;
            dev.strideC = static_cast<int>(u[lens].pitch / sizeof(osv_u16));
        } else {
            const std::size_t chromaBytes = static_cast<std::size_t>(host.cw) * sizeof(osv_u16);
            OSV_TRY(u[lens].ensure(chromaBytes, static_cast<std::size_t>(host.ch)));
            OSV_TRY(v[lens].ensure(chromaBytes, static_cast<std::size_t>(host.ch)));
            err = cudaMemcpy2DAsync(u[lens].ptr, u[lens].pitch, host.u,
                                    static_cast<std::size_t>(host.strideC) * sizeof(osv_u16), chromaBytes,
                                    static_cast<std::size_t>(host.ch), cudaMemcpyHostToDevice, stream);
            if (err != cudaSuccess) {
                return failStatus(ErrorCode::Gpu, cudaMessage("upload Cb", err));
            }
            err = cudaMemcpy2DAsync(v[lens].ptr, v[lens].pitch, host.v,
                                    static_cast<std::size_t>(host.strideC) * sizeof(osv_u16), chromaBytes,
                                    static_cast<std::size_t>(host.ch), cudaMemcpyHostToDevice, stream);
            if (err != cudaSuccess) {
                return failStatus(ErrorCode::Gpu, cudaMessage("upload Cr", err));
            }
            // Both chroma buffers were allocated with the same width so their
            // pitches match; the descriptor carries a single chroma stride.
            dev.u = static_cast<const osv_u16*>(u[lens].ptr);
            dev.v = static_cast<const osv_u16*>(v[lens].ptr);
            dev.strideC = static_cast<int>(u[lens].pitch / sizeof(osv_u16));
            if (u[lens].pitch != v[lens].pitch) {
                return failStatus(ErrorCode::Internal, "chroma pitches differ");
            }
        }
        return okStatus();
    }

    /// Describe a decoder-resident device frame without copying.
    static bool describeDeviceFrame(const video::DeviceFrameRef& ref, const OsvPlane& host, OsvPlane& dev) {
        if (!ref.valid()) {
            return false;
        }
        dev = host;
        dev.y = static_cast<const osv_u16*>(ref.yDevice);
        dev.u = static_cast<const osv_u16*>(ref.uvDevice);
        dev.v = dev.u + 1;
        dev.w = static_cast<int>(ref.width);
        dev.h = static_cast<int>(ref.height);
        dev.cw = static_cast<int>((ref.width + 1) / 2);
        dev.ch = static_cast<int>((ref.height + 1) / 2);
        dev.strideY = static_cast<int>(ref.pitchBytes / sizeof(osv_u16));
        dev.strideC = dev.strideY;
        dev.bitShift = ref.bitShift;
        dev.chromaInterleaved = 1;
        return true;
    }

    /// Launch the kernel for `job`; on success `out` holds the frame.
    Status launch(const RenderJob& job) {
        if (!job.valid()) {
            return failStatus(ErrorCode::InvalidArgument, "CudaRenderer: invalid render job");
        }
        cudaError_t err = cudaSetDevice(device);
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("cudaSetDevice", err));
        }

        // Inputs: zero-copy when the decoder left the frame on this device,
        // otherwise upload.
        OsvPlane devPlanes[2];
        for (int i = 0; i < 2; ++i) {
            const video::DeviceFrameRef& ref = job.deviceFrames[static_cast<std::size_t>(i)];
            if (ref.valid() && ref.deviceIndex == device) {
                describeDeviceFrame(ref, job.planes[static_cast<std::size_t>(i)], devPlanes[i]);
            } else {
                OSV_TRY(uploadPlane(i, job.planes[static_cast<std::size_t>(i)], devPlanes[i]));
            }
        }

        // Seam table (optional).
        const float* seamPtr = nullptr;
        if (job.params.seamShiftEnabled && !job.seamShiftDeg.empty()) {
            const std::size_t bytes = job.seamShiftDeg.size() * sizeof(float);
            OSV_TRY(seam.ensure(bytes, 1));
            err = cudaMemcpyAsync(seam.ptr, job.seamShiftDeg.data(), bytes, cudaMemcpyHostToDevice, stream);
            if (err != cudaSuccess) {
                return failStatus(ErrorCode::Gpu, cudaMessage("upload seam table", err));
            }
            seamPtr = static_cast<const float*>(seam.ptr);
        }

        // Output buffer.
        const std::size_t outWidthBytes = static_cast<std::size_t>(job.params.outW) * 4 * sizeof(float);
        OSV_TRY(out.ensure(outWidthBytes, static_cast<std::size_t>(job.params.outH)));
        const int outPitchFloats = static_cast<int>(out.pitch / sizeof(float));

        // Launch through the nvcc-compiled translation unit.
        OsvPlanePair pair;
        pair.p[0] = devPlanes[0];
        pair.p[1] = devPlanes[1];
        err = osvCudaLaunchReframe(job.params, pair, seamPtr, static_cast<float*>(out.ptr), outPitchFloats, stream);
        if (err != cudaSuccess) {
            return failStatus(ErrorCode::Gpu, cudaMessage("kernel launch", err));
        }
        return okStatus();
    }
};

// -----------------------------------------------------------------------------
//  Public API
// -----------------------------------------------------------------------------
CudaRenderer::CudaRenderer() : m_impl(std::make_unique<Impl>()) {}
CudaRenderer::~CudaRenderer() = default;

int CudaRenderer::deviceCount() {
    int count = 0;
    const cudaError_t err = cudaGetDeviceCount(&count);
    return err == cudaSuccess ? count : 0;
}

bool CudaRenderer::available(std::string* reason) {
    int count = 0;
    const cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess) {
        if (reason) {
            *reason = cudaMessage("cudaGetDeviceCount", err);
        }
        return false;
    }
    if (count <= 0) {
        if (reason) {
            *reason = "no CUDA devices";
        }
        return false;
    }
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess || prop.major < 5) {
        if (reason) {
            *reason = "device compute capability below 5.0";
        }
        return false;
    }
    return true;
}

std::string CudaRenderer::deviceName(int deviceIndex) {
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, deviceIndex) != cudaSuccess) {
        return "unknown";
    }
    return std::string(prop.name) + " (sm_" + std::to_string(prop.major) + std::to_string(prop.minor) + ")";
}

Result<std::unique_ptr<CudaRenderer>> CudaRenderer::create(int deviceIndex) {
    std::string reason;
    if (!available(&reason)) {
        return Error{ErrorCode::Gpu, "CUDA unavailable: " + reason};
    }
    if (deviceIndex < 0 || deviceIndex >= deviceCount()) {
        return Error{ErrorCode::InvalidArgument, "CUDA device index out of range"};
    }
    std::unique_ptr<CudaRenderer> r(new CudaRenderer());
    r->m_impl->device = deviceIndex;
    cudaError_t err = cudaSetDevice(deviceIndex);
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("cudaSetDevice", err)};
    }
    err = cudaStreamCreateWithFlags(&r->m_impl->stream, cudaStreamNonBlocking);
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("cudaStreamCreate", err)};
    }
    log::debug("CudaRenderer: using {}", deviceName(deviceIndex));
    return std::move(r);
}

int CudaRenderer::deviceIndex() const noexcept { return m_impl ? m_impl->device : -1; }

Result<void*> CudaRenderer::renderToDevice(const RenderJob& job, std::size_t* pitchBytes) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    OSV_TRY(m_impl->launch(job));
    const cudaError_t err = cudaStreamSynchronize(m_impl->stream);
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("kernel execution", err)};
    }
    if (pitchBytes) {
        *pitchBytes = m_impl->out.pitch;
    }
    return m_impl->out.ptr;
}

Result<ImageRGBAf> CudaRenderer::render(const RenderJob& job) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    OSV_TRY(m_impl->launch(job));

    // Readback into pinned memory (fast DMA), then into the result vector.
    OSV_TRY_ASSIGN(ImageRGBAf image, ImageRGBAf::create(static_cast<std::uint32_t>(job.params.outW),
                                                        static_cast<std::uint32_t>(job.params.outH)));
    const std::size_t rowBytes = image.pitchBytes();
    const std::size_t totalBytes = rowBytes * image.h;
    OSV_TRY(m_impl->staging.ensure(totalBytes));
    cudaError_t err = cudaMemcpy2DAsync(m_impl->staging.ptr, rowBytes, m_impl->out.ptr, m_impl->out.pitch, rowBytes,
                                        image.h, cudaMemcpyDeviceToHost, m_impl->stream);
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("readback", err)};
    }
    err = cudaStreamSynchronize(m_impl->stream);
    if (err != cudaSuccess) {
        return Error{ErrorCode::Gpu, cudaMessage("kernel execution", err)};
    }
    std::memcpy(image.data.data(), m_impl->staging.ptr, totalBytes);
    return image;
}

}  // namespace osv::render
