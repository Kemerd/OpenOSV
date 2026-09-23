// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterPackKernel.h - pack the stitched float frame into Premiere's
// integer BGRA layouts ON THE GPU, before it crosses the bus.
//
// The CUDA renderer produces float RGBA (16 bytes a pixel).  A frame the host
// asked for in BGRA_4444_16u needs 8 bytes a pixel and one in BGRA_4444_8u
// needs 4, so converting on the device first halves (16u) or quarters (8u)
// what the PCIe transfer and the host copy into the PPix have to move - at
// 6000 x 3000, 144 MB or 216 MB less per frame.
//
// The codes are EXACTLY PixelCopy's floatTo16u / floatTo8u (same clamp,
// same scale, same +0.5 truncation; the device build uses -fmad=false and
// the host build /fp:precise without FMA, so both round the multiply and the
// add separately): a frame packed here is byte-identical to one the host path
// converts on the CPU, which the importer tests compare.
//
// Deliberately free of any CUDA or SDK header: the importer includes this
// from MSVC-compiled code, and only ImporterPackKernel.cu goes through nvcc.
#pragma once

#include <cstddef>
#include <cstdint>

namespace osv::premiere {

/// Target layouts of the pack kernel (the values are an ABI between the two
/// translation units, not PrPixelFormat constants).
enum class PackLayout : int {
    Bgra16u = 1,  ///< 4 x uint16, 0..32768 (PrPixelFormat_BGRA_4444_16u).
    Bgra8u = 2,   ///< 4 x uint8, 0..255 (PrPixelFormat_BGRA_4444_8u).
};

/// Bytes per packed pixel (8 or 4; 0 for a value outside the enum).
[[nodiscard]] constexpr std::size_t packedBytesPerPixel(PackLayout layout) noexcept {
    return layout == PackLayout::Bgra16u ? 8u : layout == PackLayout::Bgra8u ? 4u : 0u;
}

/// Enqueue the pack of a device float RGBA frame (`height` rows of `width`
/// pixels, top-down, `srcPitchBytes` apart) into `dstDevice` (same row order,
/// `dstPitchBytes` apart, B,G,R,A in `layout`) on `stream` - a CUstream /
/// cudaStream_t of the context current on the calling thread.  Asynchronous.
///
/// Returns 0 on success, otherwise the cudaError_t value of the failure (a
/// launch error, or 1 = cudaErrorInvalidValue for arguments it refuses
/// itself: null pointers, empty or oversized frames, pitches too small or
/// misaligned for the vector loads and stores).
[[nodiscard]] int launchPackRgbaToBgra(const void* srcDevice, std::size_t srcPitchBytes, void* dstDevice,
                                       std::size_t dstPitchBytes, std::uint32_t width, std::uint32_t height,
                                       PackLayout layout, void* stream) noexcept;

/// Human readable name of a launchPackRgbaToBgra() result ("no error",
/// "invalid argument", ...), never null.
[[nodiscard]] const char* packErrorText(int code) noexcept;

}  // namespace osv::premiere
