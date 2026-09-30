// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxGpuView.h - the effects' own GPU path for hosts that hand them CPU
// images.
//
// ===========================================================================
//  Why the effects need a GPU path of their own
// ===========================================================================
// DaVinci Resolve can hand the reframe filter CUDA images (OfxCuda.h).  VEGAS
// Pro never does: every OpenFX image it passes lives in host memory.  Without
// a path of their own the effects would then do their heaviest work on the
// CPU:
//
//   * OpenOSV Source stitches the sphere on the GPU (the importer engine's
//     CUDA / OpenCL renderer), reads the WHOLE sphere back - 6000 x 3000 x 16
//     bytes, 288 MB a frame - and frames the view from it on the CPU;
//   * OpenOSV 360 Reframe frames the view on the CPU.
//
// The functions below keep all of that on the GPU - decode, stitch, frame,
// levels and the pack into the host's pixel format - and read back only the
// finished view, straight into the host's CPU image.
//
// ===========================================================================
//  The contract
// ===========================================================================
// Every function returns true when it wrote EVERY pixel of the target's
// render window (clipped to the image bounds): the view where the camera
// frame covers it, transparent black elsewhere.  It returns false when it
// did not take the frame, and then it has written NOTHING the caller relies
// on, so the caller simply runs its CPU path.  `error` tells the two kinds
// of false apart:
//
//   * empty    - "not mine": no GPU path serves this frame (no CUDA device,
//                the clip renders on OpenCL or the CPU, the path is switched
//                off).  The caller falls back silently;
//   * non-empty - the GPU path was tried and failed.  The caller logs it once
//                and falls back; a render never fails because of this path.
//
// None of the functions throws; none leaves a CUDA context current that was
// not current on entry (the host's own GPU work must not notice us).
#pragma once

#include "OfxHost.h"
#include "OfxHostImage.h"

#include "ImporterInstance.h"
#include "ReframeCpu.h"

#include <cstdint>
#include <string>

namespace osv::ofx::gpu {

/// Where a GPU render lands: the host's CPU output image, the part of it to
/// fill, the camera frame the view is framed for, and the levels to pack in.
struct HostTarget {
    HostImageView image;                       ///< The host's output image (CPU memory).
    OfxRectI window{0, 0, 0, 0};               ///< The render window, in pixels (clipped to image.bounds by the callee).
    OfxRectI frame{0, 0, 0, 0};                ///< The camera frame (cameraFrame() in OfxRender.h).
    OutputLevels levels = OutputLevels::Full;  ///< RGB levels of the packed output; alpha is never touched.
};

/// OpenOSV Source, Reframed view: stitch frame `index` of `clip` into its
/// native sphere (`sphere`, from geometryForLocked) without leaving the GPU,
/// frame the camera `settings` from it on the GPU, pack the view into the
/// target's format and levels, and read back only the view.
///
/// `draft` and `purpose` mean what they mean for
/// ImporterInstance::renderFrame(); `projectSize` is camera::projectSize().
/// The caller MUST hold clip.lock() for the whole call, exactly as for
/// renderFrame().
[[nodiscard]] bool renderSourceViewGpu(premiere::ImporterInstance& clip, std::uint32_t index,
                                       const premiere::OutputGeometry& sphere, bool draft,
                                       premiere::RenderPurpose purpose, const reframe::Settings& settings,
                                       reframe::SizePx projectSize, const HostTarget& target,
                                       std::string& error) noexcept;

/// OpenOSV Source, 360 equirect: stitch frame `index` straight at the camera
/// frame's size on the GPU, pack it into the target's format and levels, and
/// read it back.  Same lock rule as renderSourceViewGpu().
[[nodiscard]] bool renderSourceEquirectGpu(premiere::ImporterInstance& clip, std::uint32_t index, bool draft,
                                           premiere::RenderPurpose purpose, const HostTarget& target,
                                           std::string& error) noexcept;

/// OpenOSV 360 Reframe on a CPU source image: upload `source`, frame
/// `setup` (built by reframe::buildParams() from that source, channel order
/// already set) on the GPU, pack into the target's format and read back.
/// The reframe filter keeps its levels (it moves pixels the host already
/// levelled), so callers pass OutputLevels::Full in the target.
[[nodiscard]] bool renderReframeFromHostGpu(const reframe::KernelSetup& setup, const HostImageView& source,
                                            const HostTarget& target, std::string& error) noexcept;

}  // namespace osv::ofx::gpu
