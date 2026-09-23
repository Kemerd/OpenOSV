// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectLaunch.cpp - host-side validation and the cuLaunchKernel call for the
// fused fisheye -> view kernel.  See DirectLaunch.h for the contract and for
// why a refusal here is always better than a launch on doubtful inputs.

#include "DirectLaunch.h"

#include <cstddef>
#include <cstdint>

namespace osv::reframe {

namespace {

/// The kernel argument must be exactly the device struct: two plane
/// descriptors, nothing more, nothing less.
static_assert(sizeof(OsvDirectPlanes) == 2 * sizeof(OsvPlane),
              "OsvDirectPlanes must be exactly the two plane descriptors the kernel indexes");

/// Bytes one output pixel occupies: four halves or four floats.
[[nodiscard]] constexpr std::uint64_t outputBytesPerPixel(bool isHalf) noexcept { return isHalf ? 8u : 16u; }

/// True when the `bytes`-byte range starting at `p` is device memory of
/// `device`, lying entirely inside one allocation.
///
/// Three questions to the driver, each of which rules out a real way a
/// kernel could fault or scribble in the host's context:
///   * MEMORY_TYPE        a host pointer (the CPU twin's planes, a stale
///                        mapping) is not device memory;
///   * DEVICE_ORDINAL     a frame decoded on another GPU is not readable here;
///   * RANGE_START/SIZE   a frame that claims more rows than its allocation
///                        holds would read or write past the end.
/// Any failed query is a refusal: an address the driver cannot describe is
/// not one to launch on.
[[nodiscard]] bool deviceRangeOk(const void* p, std::uint64_t bytes, CUdevice device) noexcept {
    if (!p || bytes == 0) {
        return false;
    }
    const CUdeviceptr addr = static_cast<CUdeviceptr>(reinterpret_cast<std::uintptr_t>(p));

    // Device memory at all?
    unsigned int memoryType = 0;
    if (cuPointerGetAttribute(&memoryType, CU_POINTER_ATTRIBUTE_MEMORY_TYPE, addr) != CUDA_SUCCESS ||
        memoryType != CU_MEMORYTYPE_DEVICE) {
        return false;
    }

    // On the device the current context drives?
    int ordinal = -1;
    if (cuPointerGetAttribute(&ordinal, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL, addr) != CUDA_SUCCESS ||
        ordinal != static_cast<int>(device)) {
        return false;
    }

    // Inside its allocation, end to end?
    CUdeviceptr start = 0;
    std::size_t size = 0;
    if (cuPointerGetAttribute(&start, CU_POINTER_ATTRIBUTE_RANGE_START_ADDR, addr) != CUDA_SUCCESS ||
        cuPointerGetAttribute(&size, CU_POINTER_ATTRIBUTE_RANGE_SIZE, addr) != CUDA_SUCCESS) {
        return false;
    }
    if (addr < start) {
        return false;  // the driver contradicted itself; refuse rather than guess
    }
    const std::uint64_t offset = static_cast<std::uint64_t>(addr - start);
    return offset <= size && bytes <= static_cast<std::uint64_t>(size) - offset;
}

/// Bytes spanned by `rows` rows of `rowElems` uint16 elements each at a
/// pitch of `strideElems` elements: every row but the last contributes a
/// whole pitch, the last only the elements actually read.
[[nodiscard]] std::uint64_t planeBytes(int rows, int strideElems, int rowElems) noexcept {
    if (rows <= 0 || strideElems <= 0 || rowElems <= 0) {
        return 0;
    }
    const std::uint64_t elems = static_cast<std::uint64_t>(rows - 1) * static_cast<std::uint64_t>(strideElems) +
                                static_cast<std::uint64_t>(rowElems);
    return elems * sizeof(osv_u16);
}

/// True when every sample the shader can read from lens `i`'s frame lies in
/// device memory of `device`.  planesMatch() has already vetted the
/// descriptor's arithmetic; this vets the addresses behind it.
[[nodiscard]] bool planeMemoryOk(const OsvPlane& P, CUdevice device) noexcept {
    // Luma: h rows of w samples.
    if (!deviceRangeOk(P.y, planeBytes(P.h, P.strideY, P.w), device)) {
        return false;
    }
    if (P.chromaInterleaved) {
        // CbCr interleaved (P010 / NV12): ch rows of 2 * cw samples; the Cr
        // pointer is u + 1 and lies inside the same range.
        return deviceRangeOk(P.u, planeBytes(P.ch, P.strideC, 2 * P.cw), device);
    }
    // Planar chroma: two separate planes of ch rows of cw samples.
    return deviceRangeOk(P.u, planeBytes(P.ch, P.strideC, P.cw), device) &&
           deviceRangeOk(P.v, planeBytes(P.ch, P.strideC, P.cw), device);
}

/// A refusal with its reason and no driver involvement.
[[nodiscard]] DirectLaunchResult refused(DirectLaunchReject reason) noexcept {
    DirectLaunchResult r;
    r.reject = reason;
    r.result = CUDA_ERROR_INVALID_VALUE;
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Names
// ---------------------------------------------------------------------------
const char* directLaunchRejectName(DirectLaunchReject reason) noexcept {
    switch (reason) {
    case DirectLaunchReject::None:      return "launched";
    case DirectLaunchReject::Kernel:    return "no kernel handle";
    case DirectLaunchReject::Setup:     return "the direct setup is not valid";
    case DirectLaunchReject::Planes:    return "the lens plane descriptors do not match the setup";
    case DirectLaunchReject::Output:    return "the output frame is missing, mis-sized or its pitch cannot hold a row";
    case DirectLaunchReject::Alignment: return "the output address or pitch is misaligned for its sample type";
    case DirectLaunchReject::Context:   return "no current CUDA context";
    case DirectLaunchReject::Memory:    return "a plane, table or output address is not device memory of this device";
    case DirectLaunchReject::Driver:    return "cuLaunchKernel failed";
    }
    // Unreachable for any enumerator above.
    return "unknown";
}

// ---------------------------------------------------------------------------
//  The launch
// ---------------------------------------------------------------------------
DirectLaunchResult launchDirect(CUfunction kernel, CUstream stream, const DirectSetup& setup,
                                const OsvPlane* devicePlanes, const DirectOutput& out) noexcept {
    // ---- the cheap host-only checks first ---------------------------------
    if (!kernel) {
        return refused(DirectLaunchReject::Kernel);
    }
    if (!setup.valid) {
        return refused(DirectLaunchReject::Setup);
    }
    if (!devicePlanes || !planesMatch(setup, devicePlanes)) {
        return refused(DirectLaunchReject::Planes);
    }

    // ---- the output frame ---------------------------------------------------
    // It must be exactly the frame the setup was built for: the kernel's
    // bounds come from the parameter block, so a smaller frame would be
    // written past its end and a larger one left partly stale.
    if (!out.data || out.width != setup.params.outW || out.height != setup.params.outH || out.width <= 0 ||
        out.height <= 0) {
        return refused(DirectLaunchReject::Output);
    }
    const std::uint64_t bpp = outputBytesPerPixel(out.isHalf);
    if (out.rowBytes <= 0 || static_cast<std::uint64_t>(out.rowBytes) < static_cast<std::uint64_t>(out.width) * bpp) {
        return refused(DirectLaunchReject::Output);
    }
    // Texels are stored channel by channel, so the address and the pitch must
    // be aligned to one channel: 4 bytes for float, 2 for half.
    const std::uintptr_t align = out.isHalf ? 2u : 4u;
    if ((reinterpret_cast<std::uintptr_t>(out.data) % align) != 0u ||
        (static_cast<std::uintptr_t>(out.rowBytes) % align) != 0u) {
        return refused(DirectLaunchReject::Alignment);
    }

    // ---- the context and the memory behind every address -------------------
    CUdevice device = 0;
    if (cuCtxGetDevice(&device) != CUDA_SUCCESS) {
        return refused(DirectLaunchReject::Context);
    }
    for (int i = 0; i < 2; ++i) {
        // A disabled lens is never read, so its (possibly empty) descriptor
        // is not asked about.
        if (!setup.params.lens[i].enabled) {
            continue;
        }
        if (!planeMemoryOk(devicePlanes[i], device)) {
            return refused(DirectLaunchReject::Memory);
        }
    }
    const std::uint64_t outBytes =
        static_cast<std::uint64_t>(out.height - 1) * static_cast<std::uint64_t>(out.rowBytes) +
        static_cast<std::uint64_t>(out.width) * bpp;
    if (!deviceRangeOk(out.data, outBytes, device)) {
        return refused(DirectLaunchReject::Memory);
    }
    if (setup.seamTable) {
        const std::uint64_t seamBytes = static_cast<std::uint64_t>(setup.params.seamColumns) * sizeof(float);
        if (!deviceRangeOk(setup.seamTable, seamBytes, device)) {
            return refused(DirectLaunchReject::Memory);
        }
    }
    if (setup.warpGrid) {
        const std::uint64_t warpBytes = static_cast<std::uint64_t>(setup.params.warpW) *
                                        static_cast<std::uint64_t>(setup.params.warpH) * 2u * sizeof(float);
        if (!deviceRangeOk(setup.warpGrid, warpBytes, device)) {
            return refused(DirectLaunchReject::Memory);
        }
    }

    // ---- the arguments, in the order DirectKernelAbi.h pins ----------------
    // cuLaunchKernel copies every argument before it returns, so locals are
    // exactly right here.
    OsvRenderParams params = setup.params;
    OsvDirectPlanes planes{};
    planes.lens[0] = devicePlanes[0];
    planes.lens[1] = devicePlanes[1];
    const float* seam = setup.seamTable;
    const float* warp = setup.warpGrid;
    unsigned char* dst = static_cast<unsigned char*>(out.data);
    int dstRowBytes = static_cast<int>(out.rowBytes);
    int dstIsHalf = out.isHalf ? 1 : 0;
    void* args[] = {&params, &planes, &seam, &warp, &dst, &dstRowBytes, &dstIsHalf};

    // One thread per output pixel, the grid rounded up to whole blocks (the
    // kernel guards the tail).  Heights up to 65536 need at most 4096 blocks
    // in y, far inside the 65535 limit.
    const unsigned gridX = (static_cast<unsigned>(out.width) + OSV_DIRECT_BLOCK_X - 1u) / OSV_DIRECT_BLOCK_X;
    const unsigned gridY = (static_cast<unsigned>(out.height) + OSV_DIRECT_BLOCK_Y - 1u) / OSV_DIRECT_BLOCK_Y;

    DirectLaunchResult r;
    r.result = cuLaunchKernel(kernel, gridX, gridY, 1u, OSV_DIRECT_BLOCK_X, OSV_DIRECT_BLOCK_Y, 1u, 0u, stream, args,
                              nullptr);
    r.reject = (r.result == CUDA_SUCCESS) ? DirectLaunchReject::None : DirectLaunchReject::Driver;
    return r;
}

}  // namespace osv::reframe
