// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// GPU Device Suite v2 backed by a real CUDA driver-API context.
//
// Premiere gives a GPU effect its device through this suite: a CUdevice, a
// CUcontext (the host's own) and a CUstream, plus device / pinned memory and
// GPU PPixes.  The mock recreates that with cuInit / cuDevicePrimaryCtxRetain
// on device 0 so the effect's xGPUFilterEntry path can be tested end to end
// against a real kernel launch.  GPU PPixes use the private formats
// PrPixelFormat_GPU_BGRA_4444_16f / _32f, have a TOP-LEFT origin and a pitch
// rounded up to 256 bytes.
//
// When the build has no CUDA (OSV_MOCK_HAVE_CUDA undefined) or no device /
// driver is present, GetDeviceCount reports 0 and every other call answers
// suiteError_IDNotValid so a plug-in sees exactly what a GPU-less host would
// show it.

#include "MockHostImpl.h"

#include <cstdlib>
#include <cstring>
#include <new>

#if defined(OSV_MOCK_HAVE_CUDA)
#include <cuda.h>
#endif

namespace osv::premiere::mock {

namespace {

MockHost::Impl* impl() noexcept {
    MockHost* h = MockHost::current();
    return h ? h->implForSuites() : nullptr;
}

#if defined(OSV_MOCK_HAVE_CUDA)

/// Human readable driver error.
std::string cuMessage(const char* what, CUresult r) {
    const char* name = nullptr;
    const char* text = nullptr;
    cuGetErrorName(r, &name);
    cuGetErrorString(r, &text);
    std::string s(what);
    s += ": ";
    s += name ? name : "CUDA_ERROR_?";
    if (text) {
        s += " (";
        s += text;
        s += ")";
    }
    return s;
}

/// RAII push/pop of the mock's context around driver calls.
struct ContextScope {
    bool pushed = false;
    explicit ContextScope(GpuState& g) {
        if (g.context && cuCtxPushCurrent(static_cast<CUcontext>(g.context)) == CUDA_SUCCESS) {
            pushed = true;
        }
    }
    ~ContextScope() {
        if (pushed) {
            CUcontext ignored = nullptr;
            cuCtxPopCurrent(&ignored);
        }
    }
};

/// Initialise once: driver, device 0, primary context, a stream.
void ensureGpu(GpuState& g) {
    if (g.initialised) {
        return;
    }
    g.initialised = true;
    CUresult r = cuInit(0);
    if (r != CUDA_SUCCESS) {
        g.failure = cuMessage("cuInit", r);
        return;
    }
    int count = 0;
    r = cuDeviceGetCount(&count);
    if (r != CUDA_SUCCESS || count <= 0) {
        g.failure = r == CUDA_SUCCESS ? "no CUDA device" : cuMessage("cuDeviceGetCount", r);
        return;
    }
    CUdevice dev = 0;
    r = cuDeviceGet(&dev, 0);
    if (r != CUDA_SUCCESS) {
        g.failure = cuMessage("cuDeviceGet", r);
        return;
    }
    CUcontext ctx = nullptr;
    r = cuDevicePrimaryCtxRetain(&ctx, dev);
    if (r != CUDA_SUCCESS) {
        g.failure = cuMessage("cuDevicePrimaryCtxRetain", r);
        return;
    }
    g.device = reinterpret_cast<void*>(static_cast<std::intptr_t>(dev));
    g.context = ctx;
    // Stream creation needs the context current.
    r = cuCtxPushCurrent(ctx);
    if (r != CUDA_SUCCESS) {
        g.failure = cuMessage("cuCtxPushCurrent", r);
        cuDevicePrimaryCtxRelease(dev);
        g.context = nullptr;
        return;
    }
    CUstream stream = nullptr;
    r = cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING);
    CUcontext ignored = nullptr;
    cuCtxPopCurrent(&ignored);
    if (r != CUDA_SUCCESS) {
        g.failure = cuMessage("cuStreamCreate", r);
        cuDevicePrimaryCtxRelease(dev);
        g.context = nullptr;
        return;
    }
    g.stream = stream;
    g.deviceOrdinal = 0;
    g.available = true;
}

#else

void ensureGpu(GpuState& g) {
    if (!g.initialised) {
        g.initialised = true;
        g.available = false;
        g.failure = "mock host built without CUDA (OSV_ENABLE_CUDA=OFF)";
    }
}

#endif  // OSV_MOCK_HAVE_CUDA

/// Pitch of a GPU PPix row.
std::int32_t gpuRowBytes(std::uint32_t width, std::size_t bpp) noexcept {
    const std::size_t raw = static_cast<std::size_t>(width) * bpp;
    return static_cast<std::int32_t>((raw + 255u) & ~static_cast<std::size_t>(255u));
}

// -----------------------------------------------------------------------------
//  Suite functions
// -----------------------------------------------------------------------------
prSuiteError gpuGetDeviceCount(csSDK_uint32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    ensureGpu(p->gpuState);
    *out = p->gpuState.available ? 1u : 0u;
    return suiteError_NoError;
}

prSuiteError gpuGetDeviceInfo(csSDK_uint32 suiteVersion, csSDK_uint32 deviceIndex, PrGPUDeviceInfo* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    if (suiteVersion == 0 || suiteVersion > kPrSDKGPUDeviceSuiteVersion) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    ensureGpu(p->gpuState);
    std::memset(out, 0, sizeof(*out));
    if (!p->gpuState.available || deviceIndex != 0) {
        return suiteError_IDNotValid;
    }
    out->outDeviceFramework = PrGPUDeviceFramework_CUDA;
    out->outMeetsMinimumRequirementsForAcceleration = kPrTrue;
    out->outPlatformHandle = nullptr;
    out->outDeviceHandle = p->gpuState.device;
    out->outContextHandle = p->gpuState.context;
    out->outCommandQueueHandle = p->gpuState.stream;
    return suiteError_NoError;
}

prSuiteError gpuAcquireExclusive(csSDK_uint32 deviceIndex) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    ensureGpu(p->gpuState);
    return (p->gpuState.available && deviceIndex == 0) ? suiteError_NoError : suiteError_IDNotValid;
}

prSuiteError gpuReleaseExclusive(csSDK_uint32 deviceIndex) { return gpuAcquireExclusive(deviceIndex); }

prSuiteError gpuAllocateDeviceMemory(csSDK_uint32 deviceIndex, size_t bytes, void** out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    ensureGpu(p->gpuState);
    if (!p->gpuState.available || deviceIndex != 0) {
        return suiteError_IDNotValid;
    }
    if (bytes == 0) {
        return suiteError_InvalidParms;
    }
#if defined(OSV_MOCK_HAVE_CUDA)
    ContextScope scope(p->gpuState);
    CUdeviceptr ptr = 0;
    const CUresult r = cuMemAlloc(&ptr, bytes);
    if (r != CUDA_SUCCESS) {
        return suiteError_OutOfMemory;
    }
    *out = reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr));
    p->gpuState.deviceAllocations.insert(*out);
    return suiteError_NoError;
#else
    return suiteError_NotImplemented;
#endif
}

prSuiteError gpuFreeDeviceMemory(csSDK_uint32 deviceIndex, void* memory) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->gpuState.available || deviceIndex != 0) {
        return suiteError_IDNotValid;
    }
    if (!memory || !p->gpuState.deviceAllocations.erase(memory)) {
        return suiteError_InvalidParms;
    }
#if defined(OSV_MOCK_HAVE_CUDA)
    ContextScope scope(p->gpuState);
    cuMemFree(static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(memory)));
    return suiteError_NoError;
#else
    return suiteError_NotImplemented;
#endif
}

prSuiteError gpuPurgeDeviceMemory(csSDK_uint32, size_t, size_t* outPurged) {
    if (outPurged) {
        *outPurged = 0;
    }
    return suiteError_NoError;
}

prSuiteError gpuAllocateHostMemory(csSDK_uint32 deviceIndex, size_t bytes, void** out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    ensureGpu(p->gpuState);
    if (!p->gpuState.available || deviceIndex != 0) {
        return suiteError_IDNotValid;
    }
    if (bytes == 0) {
        return suiteError_InvalidParms;
    }
#if defined(OSV_MOCK_HAVE_CUDA)
    ContextScope scope(p->gpuState);
    void* ptr = nullptr;
    const CUresult r = cuMemHostAlloc(&ptr, bytes, CU_MEMHOSTALLOC_PORTABLE);
    if (r != CUDA_SUCCESS || !ptr) {
        return suiteError_OutOfMemory;
    }
    *out = ptr;
    p->gpuState.hostAllocations.insert(ptr);
    return suiteError_NoError;
#else
    return suiteError_NotImplemented;
#endif
}

prSuiteError gpuFreeHostMemory(csSDK_uint32 deviceIndex, void* memory) {
    MockHost::Impl* p = impl();
    if (!p) {
        return suiteError_InvalidCall;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->gpuState.available || deviceIndex != 0) {
        return suiteError_IDNotValid;
    }
    if (!memory || !p->gpuState.hostAllocations.erase(memory)) {
        return suiteError_InvalidParms;
    }
#if defined(OSV_MOCK_HAVE_CUDA)
    ContextScope scope(p->gpuState);
    cuMemFreeHost(memory);
    return suiteError_NoError;
#else
    return suiteError_NotImplemented;
#endif
}

prSuiteError gpuPurgeHostMemory(csSDK_uint32, size_t, size_t* outPurged) {
    if (outPurged) {
        *outPurged = 0;
    }
    return suiteError_NoError;
}

prSuiteError gpuCreatePPix(csSDK_uint32 deviceIndex, PrPixelFormat format, int width, int height, int parNum,
                           int parDen, prFieldType fieldType, PPixHand* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    if (format != PrPixelFormat_GPU_BGRA_4444_16f && format != PrPixelFormat_GPU_BGRA_4444_32f) {
        return suiteError_InvalidParms;
    }
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || parNum <= 0 || parDen <= 0) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    ensureGpu(p->gpuState);
    if (!p->gpuState.available || deviceIndex != 0) {
        return suiteError_IDNotValid;
    }
    const std::size_t bpp = bytesPerPixel(format);
    const std::int32_t pitch = gpuRowBytes(static_cast<std::uint32_t>(width), bpp);
    const std::size_t bytes = static_cast<std::size_t>(pitch) * static_cast<std::size_t>(height);

    void* device = nullptr;
    const prSuiteError allocErr = gpuAllocateDeviceMemory(deviceIndex, bytes, &device);
    if (allocErr != suiteError_NoError) {
        return allocErr;
    }

    auto* rec = new (std::nothrow) PPixRecord();
    if (!rec) {
        gpuFreeDeviceMemory(deviceIndex, device);
        return suiteError_OutOfMemory;
    }
    rec->width = static_cast<std::uint32_t>(width);
    rec->height = static_cast<std::uint32_t>(height);
    rec->rowBytes = pitch;
    rec->format = format;
    rec->isGpu = true;
    rec->deviceIndex = deviceIndex;
    rec->devicePtr = device;
    rec->byteSize = bytes;
    rec->parNum = static_cast<csSDK_uint32>(parNum);
    rec->parDen = static_cast<csSDK_uint32>(parDen);
    rec->fieldType = fieldType;
    prSetRect(&rec->header.bounds, 0, 0, width, height);
    rec->header.rowbytes = pitch;
    rec->header.bitsperpixel = static_cast<csSDK_int32>(bpp * 8);
    rec->header.pix = nullptr;  // device memory, not addressable
    rec->master = &rec->header;
    rec->refCount = 1;
    p->livePPix.insert(rec);
    *out = &rec->master;
    return suiteError_NoError;
}

prSuiteError gpuGetPPixData(PPixHand hand, void** out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    *out = nullptr;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec || !rec->isGpu) {
        return suiteError_InvalidParms;
    }
    *out = rec->devicePtr;
    return suiteError_NoError;
}

prSuiteError gpuGetPPixDeviceIndex(PPixHand hand, csSDK_uint32* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec || !rec->isGpu) {
        return suiteError_InvalidParms;
    }
    *out = rec->deviceIndex;
    return suiteError_NoError;
}

prSuiteError gpuGetPPixSize(PPixHand hand, size_t* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return suiteError_InvalidParms;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    PPixRecord* rec = p->record(hand);
    if (!rec || !rec->isGpu) {
        return suiteError_InvalidParms;
    }
    *out = rec->byteSize;
    return suiteError_NoError;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Teardown and MockHost API
// -----------------------------------------------------------------------------
void MockHost::Impl::shutdownGpu() {
#if defined(OSV_MOCK_HAVE_CUDA)
    GpuState& g = gpuState;
    if (!g.available) {
        return;
    }
    {
        ContextScope scope(g);
        // Anything a plug-in leaked is released here so the primary context
        // can go away cleanly.
        for (void* ptr : g.deviceAllocations) {
            cuMemFree(static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(ptr)));
        }
        for (void* ptr : g.hostAllocations) {
            cuMemFreeHost(ptr);
        }
        if (g.stream) {
            cuStreamDestroy(static_cast<CUstream>(g.stream));
        }
    }
    g.deviceAllocations.clear();
    g.hostAllocations.clear();
    g.stream = nullptr;
    if (g.context) {
        cuDevicePrimaryCtxRelease(static_cast<CUdevice>(reinterpret_cast<std::intptr_t>(g.device)));
        g.context = nullptr;
    }
    g.available = false;
#endif
}

bool MockHost::gpuAvailable() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    ensureGpu(m_impl->gpuState);
    return m_impl->gpuState.available;
}

csSDK_uint32 MockHost::gpuDeviceCount() {
    return gpuAvailable() ? 1u : 0u;
}

std::string MockHost::gpuFailureReason() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    ensureGpu(m_impl->gpuState);
    return m_impl->gpuState.available ? std::string() : m_impl->gpuState.failure;
}

void installGpuSuite(MockHost::Impl& p) {
    p.gpu.GetDeviceCount = &gpuGetDeviceCount;
    p.gpu.GetDeviceInfo = &gpuGetDeviceInfo;
    p.gpu.AcquireExclusiveDeviceAccess = &gpuAcquireExclusive;
    p.gpu.ReleaseExclusiveDeviceAccess = &gpuReleaseExclusive;
    p.gpu.AllocateDeviceMemory = &gpuAllocateDeviceMemory;
    p.gpu.FreeDeviceMemory = &gpuFreeDeviceMemory;
    p.gpu.PurgeDeviceMemory = &gpuPurgeDeviceMemory;
    p.gpu.AllocateHostMemory = &gpuAllocateHostMemory;
    p.gpu.FreeHostMemory = &gpuFreeHostMemory;
    p.gpu.PurgeHostMemory = &gpuPurgeHostMemory;
    p.gpu.CreateGPUPPix = &gpuCreatePPix;
    p.gpu.GetGPUPPixData = &gpuGetPPixData;
    p.gpu.GetGPUPPixDeviceIndex = &gpuGetPPixDeviceIndex;
    p.gpu.GetGPUPPixSize = &gpuGetPPixSize;
    p.registerSuite(kPrSDKGPUDeviceSuite, kPrSDKGPUDeviceSuiteVersion, &p.gpu);
}

}  // namespace osv::premiere::mock
