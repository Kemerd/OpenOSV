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
//
// ===========================================================================
//  Who takes the frame (the policy)
// ===========================================================================
// By default the own-GPU path serves ONLY hostProfile() == HostProfile::Vegas;
// under DaVinci Resolve and every other host the effects behave exactly as
// before it existed (Resolve keeps its CUDA-image filter path and the CPU
// framing).  The environment variable OPENOSV_OFX_GPU overrides that for
// every host: "0" / "off" / "false" / "no" switch the path off everywhere,
// "1" / "on" / "true" / "yes" switch it on everywhere (how the tests drive it
// through the mock host).  It is read on every call, so a test - or a user
// chasing a problem - can flip it without reloading the module.
//
// Every pixel written - the view, and the transparent black outside the
// camera frame alike - is packed by OfxHostImage.h's storeHostPixel() rule:
// levels on R, G and B (never alpha), then the depth, then the order.  So
// transparent black under studio levels is RGB 16/255 with alpha 0, exactly
// what the CPU paths write.
#pragma once

#include "OfxGpuPipeline.h"  // HostTarget, the pipeline under the three hooks
#include "OfxHost.h"
#include "OfxHostImage.h"

#include "ImporterInstance.h"
#include "ReframeCpu.h"

#include <cstdint>
#include <string>

namespace osv::ofx::gpu {

// HostTarget - where a GPU render lands - is declared in OfxGpuPipeline.h,
// below the clip engine, so the pipeline and its tests can use it alone.

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

// ===========================================================================
//  [WP-V-GPU] Which path served an instance's first frame (the log)
// ===========================================================================

/// The three hooks above, for notePath().
enum class Hook : std::uint8_t {
    SourceView = 0,      ///< renderSourceViewGpu().
    SourceEquirect = 1,  ///< renderSourceEquirectGpu().
    ReframeFilter = 2,   ///< renderReframeFromHostGpu().
};

/// True when the own-GPU path may serve this host at all (the policy above),
/// with the reason in `why` (may be null) when not.  What every hook asks
/// first; exposed so a caller can say why nothing ran.
[[nodiscard]] bool ownGpuWanted(std::string* why = nullptr) noexcept;

/// Log - once per (instance, hook), at INFO - which path rendered the first
/// frame an effect instance sent through `hook`: the own GPU path
/// (`servedByGpu`), or the CPU path and why: `gpuError` when the GPU path was
/// tried and failed, otherwise the reason the most recent hook call on THIS
/// thread gave for "not mine".  `instance` is any pointer unique to the
/// instance - the effect handle.  Never throws.
void notePath(const void* instance, Hook hook, bool servedByGpu, const std::string& gpuError) noexcept;

}  // namespace osv::ofx::gpu
