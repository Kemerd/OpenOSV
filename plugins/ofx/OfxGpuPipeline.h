// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxGpuPipeline.h - the machinery under the effects' own GPU path
// (OfxGpuView.h): per-device pools of streams, pinned staging bands and
// device buffers; the upload of a host image; the framing, levels and pack
// kernels; and the banded readback of the finished rectangle into the host's
// CPU image.
//
// ===========================================================================
//  Why it is its own layer
// ===========================================================================
// OfxGpuView.cpp decides WHETHER the own-GPU path takes a frame (the host
// profile, the switch, the clip's renderer) and gets the pixels to the GPU
// (the engine's stitched sphere, or the filter's CPU source).  Everything
// from there on is the same for all three hooks, and none of it knows about
// clips: it is this file.  Keeping it free of the clip engine also lets the
// GPU tests (tests/ofx_gpu) drive it directly, with every pixel format,
// window and pitch the hooks can be handed.
//
// ===========================================================================
//  Data flow of one frame
// ===========================================================================
//
//   phase 1 (FrameJob::frameDevice / packDevice / frameHost)
//     [frameHost only] host image --memcpy, in bands--> pinned band k
//                      pinned band k --async DMA--> device source buffer
//     kernel: frame (or copy) + levels + pack --> device OUTPUT buffer, which
//             holds exactly the host's bytes for the window, tight rows
//     the first two readback DMAs are queued behind the kernel
//   phase 2 (FrameJob::readBack)
//     wait band k --> copy its rows into the host image (y up, any pitch)
//     --> queue band k + 2 into the freed pinned band
//
// Phase 1 of the generator runs inside the clip engine's renderer lock (the
// stitched sphere is the renderer's own buffer); it returns as soon as the
// kernel has read the sphere, so the lock is released while the view is
// still crossing the bus.
//
// ===========================================================================
//  Threads, streams, contexts
// ===========================================================================
// VEGAS clones a Fully-safe filter once per render thread and runs the
// clones at once.  Nothing here is global per frame: each frame checks a
// SLOT out of its device's pool - its own non-blocking stream, events,
// pinned bands and device buffers - and returns it when done.  A pool holds
// at most kMaxSlotsPerDevice slots; a caller beyond that waits for one.
// Every slot of a device lives in that device's PRIMARY context, retained by
// the pool: the context the clip engine's CUDA renderer (CUDA runtime)
// renders in, so the generator's sphere is readable in place.  Every entry
// point pushes that context and pops it again: the caller's thread is left
// with exactly the context it came with.
//
// Builds without the CUDA kernel (the CPU-only preset, macOS) compile the
// same API; every call then fails with a message the hooks turn into "not
// mine".
#pragma once

#include "OfxHostImage.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/osv_kernel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace osv::ofx::gpu {

/// Where a GPU render lands: the host's CPU output image, the part of it to
/// fill, the camera frame the view is framed for, and the levels to pack in.
/// (Declared here, below the clip engine, so the pipeline and its tests need
/// no ImporterInstance; OfxGpuView.h - the hooks' contract - includes it.)
struct HostTarget {
    HostImageView image;                       ///< The host's output image (CPU memory).
    OfxRectI window{0, 0, 0, 0};               ///< The render window, pixels (clipped to image.bounds by the callee).
    OfxRectI frame{0, 0, 0, 0};                ///< The camera frame (cameraFrame() in OfxRender.h).
    OutputLevels levels = OutputLevels::Full;  ///< RGB levels of the packed output; alpha is never touched.
};

/// A float R,G,B,A image in VRAM, top row first: the engine's stitched
/// sphere, or any device image in the primary context of `device`.
struct DeviceRgba {
    const void* data = nullptr;   ///< Device address of the TOP-left pixel.
    std::size_t pitchBytes = 0;   ///< Byte distance between two rows (>= width * 16).
    std::uint32_t width = 0;      ///< Pixels per row.
    std::uint32_t height = 0;     ///< Rows.
    int device = -1;              ///< CUDA ordinal; the memory belongs to that device's primary context.
};

/// Bytes of one pinned staging band.  Large enough that each DMA runs at
/// full bus speed, small enough that a pool of slots pins little host
/// memory: kStageBands x kMaxSlotsPerDevice x 8 MiB = 64 MiB per device at
/// most, and only once that many renders really ran at the same time.
inline constexpr std::size_t kStageBandBytes = std::size_t{8} << 20;

/// Pinned bands per slot: copy one while the DMA of the other is in flight.
inline constexpr int kStageBands = 2;

/// Most slots a device's pool creates.  More concurrent renders than this
/// queue for a slot (they would only fight over the same copy engines).
inline constexpr int kMaxSlotsPerDevice = 4;

/// Longest a render waits for a free slot before it gives up and lets the
/// caller render on the CPU (a GPU that stopped answering must not hang the
/// host's render thread for good).
inline constexpr int kSlotWaitSeconds = 60;

/// One frame's trip through the own-GPU path: a slot checked out of a
/// device's pool, the rectangle planned in phase 1, and its readback.
///
/// Not thread-safe itself (one frame, one thread) and not copyable.  The
/// destructor returns the slot, after waiting for anything still in flight
/// on its stream, so an abandoned job (an error, an early return) can never
/// leave a DMA writing into a band the next frame uses.
///
/// Every member function is noexcept, reports failures through `error`, and
/// leaves the thread's CUDA context stack exactly as it found it.
class FrameJob {
public:
    FrameJob() noexcept;
    ~FrameJob();
    FrameJob(const FrameJob&) = delete;
    FrameJob& operator=(const FrameJob&) = delete;

    /// Check a slot out of device `ordinal`'s pool now (creating the pool,
    /// and retaining the device's primary context, on first use).  Optional:
    /// every phase-1 call reserves what it needs; reserving first keeps the
    /// wait for a slot out of a lock the caller is about to take.  True when
    /// a slot on `ordinal` is held.
    [[nodiscard]] bool reserve(int ordinal, std::string& error) noexcept;

    /// Phase 1, generator view: frame `params` (buildView() for the camera
    /// frame's size) from the float R,G,B,A device image `source`, level and
    /// pack the window into this job's device buffer.  Returns once the
    /// kernel has finished reading `source` - the caller may release it -
    /// with the first readback DMAs already queued.
    [[nodiscard]] bool frameDevice(const OsvReframeParams& params, const DeviceRgba& source, const HostTarget& target,
                                   std::string& error) noexcept;

    /// Phase 1, generator equirect: `image` IS the camera frame (the sphere
    /// rendered at the frame's size); level and pack the window of it.
    /// Returns once the kernel has finished reading `image`.
    [[nodiscard]] bool packDevice(const DeviceRgba& image, const HostTarget& target, std::string& error) noexcept;

    /// Phase 1, filter: upload the host image `source` (Byte or Float, RGBA
    /// or BGRA, any pitch) through the pinned bands on device `ordinal`,
    /// frame `params` from it, level and pack the window.  Band copies run
    /// on `pool` (null = this thread).
    [[nodiscard]] bool frameHost(const OsvReframeParams& params, const HostImageView& source, const HostTarget& target,
                                 int ordinal, ThreadPool* pool, std::string& error) noexcept;

    /// Phase 2: stream the packed window into the target image phase 1 was
    /// given, band by band, each band's rows copied (on `pool`, null = this
    /// thread) while the next band crosses the bus.  True when every pixel
    /// of the window (clipped to the image bounds) holds its final bytes.
    [[nodiscard]] bool readBack(ThreadPool* pool, std::string& error) noexcept;

    /// CUDA ordinal of the slot held, or -1.
    [[nodiscard]] int device() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
//  Whole frames (phase 1 + phase 2), for callers that hold no lock between
// ---------------------------------------------------------------------------

/// The filter on a CPU source: FrameJob::frameHost + readBack.
[[nodiscard]] bool frameHostImage(const OsvReframeParams& params, const HostImageView& source,
                                  const HostTarget& target, int ordinal, ThreadPool* pool,
                                  std::string& error) noexcept;

/// A view of a device sphere: FrameJob::frameDevice + readBack.
[[nodiscard]] bool frameDeviceImage(const OsvReframeParams& params, const DeviceRgba& source,
                                    const HostTarget& target, ThreadPool* pool, std::string& error) noexcept;

/// The window of a device image that is the camera frame: packDevice + readBack.
[[nodiscard]] bool packDeviceImage(const DeviceRgba& image, const HostTarget& target, ThreadPool* pool,
                                   std::string& error) noexcept;

// ---------------------------------------------------------------------------
//  Pools
// ---------------------------------------------------------------------------

/// True when this build carries the CUDA kernels (Windows with CUDA).  False
/// on the CPU-only preset and on macOS, where the hooks answer "not mine"
/// before they get here.
[[nodiscard]] bool pipelineBuilt() noexcept;

/// Slots a device's pool has created so far (0 when it has no pool) - for
/// the tests and the log.
[[nodiscard]] int devicePoolSlots(int ordinal) noexcept;

/// Destroy every device pool: wait for its streams, free its buffers and
/// pinned bands, unload the kernels loaded into its primary context, and
/// release that context.  Called by cuda::releaseModules() on the last
/// kOfxActionUnload - never from a static destructor, when the driver may be
/// gone.  A job still holding a slot keeps its pool alive until it finishes.
void releaseDevicePools() noexcept;

}  // namespace osv::ofx::gpu
