// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImporterGpuFrame.h - the device side of the importer's OWN frame.
//
// The importer hands Premiere a stitched equirect for every clip, even when
// the reframe effect renders its view straight from the fisheyes (the
// equirect is then the effect's fallback, and it is what every view WITHOUT
// the effect shows - see docs/PREMIERE.md, "The importer's own frame").  Its
// cost used to be: decode both lenses to host memory, upload them, stitch on
// the GPU, read 288 MB back into pinned memory, memcpy that single-threaded
// into a float image, then convert that image into the PPix.  The GPU path
// this file supports instead
//
//   1. decodes on NVDEC into VRAM (video::GpuClipDecoder, GOP-aware cache,
//      in the primary context of the CUDA renderer's device - the context
//      the shared CudaRenderer runs in, so its kernel reads the decoded
//      planes in place: zero upload),
//   2. stitches with the SAME CudaRenderer and the same parameter block as
//      before, into the renderer's device buffer (renderToDevice),
//   3. packs it on the GPU into the host's integer layout when the host
//      asked for 16u or 8u (half / a quarter of the bytes), then streams it
//      back in pinned bands (GpuReadback, below) and copies each band
//      straight into the PPix while the next band is still crossing the bus
//      - one DMA plus ONE host pass, no float image in host memory.
//
// Everything here is CUDA DRIVER API (cuda.h): the plug-in rule is that
// nothing the host loads depends on a cudart DLL.  The CudaRenderer itself
// uses the statically linked runtime, bound to the same primary context.
#pragma once

#include "PixelCopy.h"

#include "osv/core/Result.h"
#include "osv/core/ThreadPool.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace osv::premiere {

// ---------------------------------------------------------------------------
//  The shared renderer's device output
// ---------------------------------------------------------------------------

/// The one lock around every use of the process-wide CUDA renderer's OUTPUT.
///
/// CudaRenderer::renderToDevice() returns the renderer's own device buffer,
/// valid only "until the next render call".  HostContext hands that renderer
/// to every clip in the process, so without this lock a second clip's render
/// could overwrite the buffer while the first clip is still reading it back.
/// Every importer call into the shared CUDA renderer - the GPU path's
/// renderToDevice + readback AND the host path's renderInto - holds it.
///
/// Lock order: an ImporterInstance's own mutex first, then this one; nothing
/// that holds this lock takes an instance lock.
[[nodiscard]] std::mutex& cudaRendererOutputMutex() noexcept;

// ---------------------------------------------------------------------------
//  Context scope
// ---------------------------------------------------------------------------

/// RAII push / pop of a CUDA driver context on the calling thread.
///
/// Pushing (rather than setting) leaves whatever the host had current - a
/// Premiere render thread may have its own context bound - exactly as it
/// was once the scope ends.
class CudaContextScope {
public:
    /// Push `context` (a CUcontext, as void* so this header needs no cuda.h).
    /// A null context pushes nothing and ok() is false.
    explicit CudaContextScope(void* context) noexcept;
    /// Pops what the constructor pushed, if anything.
    ~CudaContextScope();

    CudaContextScope(const CudaContextScope&) = delete;
    CudaContextScope& operator=(const CudaContextScope&) = delete;

    /// True when the context was pushed and is current now.
    [[nodiscard]] bool ok() const noexcept { return m_pushed; }

private:
    bool m_pushed = false;
};

// ---------------------------------------------------------------------------
//  Pinned, banded readback
// ---------------------------------------------------------------------------

/// Timing of one GpuReadback::copyToHost() call, for the debug log and the
/// benchmark.  All in milliseconds of wall time on the calling thread.
struct ReadbackTiming {
    double waitMs = 0.0;     ///< Blocked on a band's DMA (the part the pipeline could not hide).
    double convertMs = 0.0;  ///< Converting bands into the host frame.
    double totalMs = 0.0;    ///< The whole call.
    std::uint32_t bands = 0; ///< Bands the frame was streamed in.
};

/// Streams a device float RGBA frame into a host frame through a small ring
/// of pinned staging bands.
///
/// One instance per CUDA device, shared by every clip (the frames go through
/// it one at a time anyway, under cudaRendererOutputMutex()).  Created on
/// first use and destroyed when the last ImporterInstance holding it is
/// destroyed - which Premiere does in imCloseFile, while the driver is
/// certainly alive - so no CUDA call ever runs from a static destructor.
///
/// Why bands instead of one frame-sized pinned buffer: a 6000 x 3000 float
/// frame is 288 MB, and pinning that much host memory per process (or worse,
/// per clip) takes it away from Premiere's own caches.  Two 32 MiB bands give
/// the same DMA bandwidth, and converting band k while band k + 1 is in flight
/// hides most of the transfer behind the conversion the PPix needs anyway.
class GpuReadback {
public:
    /// Bytes of one staging band.  Large enough that the DMA runs at full
    /// bus speed and a band holds a few hundred rows of a 6K frame; two of
    /// them are the whole pinned footprint (64 MiB per process).
    static constexpr std::size_t kBandBytes = std::size_t{32} << 20;

    /// Number of staging bands (double buffering: convert one, fill the other).
    static constexpr int kBands = 2;

    /// The shared instance for CUDA device `deviceOrdinal`, created on first
    /// use: retains the device's primary context (the one the CUDA runtime,
    /// and so the shared CudaRenderer, uses), allocates the pinned bands and
    /// a non-blocking stream.  Errors: InvalidArgument (negative ordinal),
    /// Gpu (driver unavailable, device missing, allocation failed).
    [[nodiscard]] static Result<std::shared_ptr<GpuReadback>> acquire(int deviceOrdinal) noexcept;

    /// Waits for any transfer still in flight, then frees the bands, the
    /// stream and the events and releases the primary context.
    ~GpuReadback();

    GpuReadback(const GpuReadback&) = delete;
    GpuReadback& operator=(const GpuReadback&) = delete;

    /// The retained primary context (CUcontext as void*).  Push it with a
    /// CudaContextScope before using device memory of the shared renderer.
    [[nodiscard]] void* context() const noexcept;

    /// CUDA device ordinal.
    [[nodiscard]] int device() const noexcept { return m_ordinal; }

    /// Copy the device frame at `deviceRgba` - `height` rows of `width`
    /// float RGBA pixels, top-down, `devicePitchBytes` apart, in context() -
    /// into the bottom-left host frame `dst` in `format`, band by band on
    /// `pool` (nullptr = calling thread).
    ///
    /// 16u and 8u are packed on the GPU first (ImporterPackKernel.h, into a
    /// device buffer this object keeps), so only 8 or 4 bytes a pixel cross
    /// the bus and the host merely copies rows; 32f travels as the float
    /// rows it is and the host swizzles while copying.  Either way the bytes
    /// in `dst` are exactly what PixelCopy's whole-frame functions write.
    ///
    /// The caller has context() current, holds cudaRendererOutputMutex(), and
    /// guarantees the device frame is complete (renderToDevice synchronises
    /// its stream before returning).  Returns only after every byte has
    /// landed in `dst` and no transfer into the staging bands is in flight,
    /// on success and on failure alike.  `timing` (may be null) receives the
    /// split.  Errors: InvalidArgument (null / mismatched arguments), Gpu (a
    /// driver call failed), and whatever PixelCopy refuses.
    [[nodiscard]] Status copyToHost(const void* deviceRgba, std::size_t devicePitchBytes, std::uint32_t width,
                                    std::uint32_t height, const pixelcopy::HostFrame& dst,
                                    pixelcopy::HostPixelFormat format, ThreadPool* pool,
                                    ReadbackTiming* timing = nullptr) noexcept;

private:
    GpuReadback() = default;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_ordinal = -1;
    /// Serialises copyToHost() on this instance.  The caller's
    /// cudaRendererOutputMutex() already does, but the staging bands are this
    /// object's own state and must not depend on a caller's discipline.
    std::mutex m_mutex;
};

// ---------------------------------------------------------------------------
//  Switches
// ---------------------------------------------------------------------------

/// True when OPENOSV_IMPORTER_NO_GPU_DECODE=1 is set in the environment: the
/// importer's own frame then takes the host path (D3D11VA / software decode,
/// upload, renderInto) even when the GPU path is available.  An escape hatch
/// for a machine where the GPU path misbehaves, and what lets the tests
/// render the same frame through both paths and compare them.  Read at the
/// moment a clip first chooses its path, not cached per process.
[[nodiscard]] bool importerGpuDecodeDisabledByEnvironment() noexcept;

}  // namespace osv::premiere
