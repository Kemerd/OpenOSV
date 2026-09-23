// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// RenderJob: everything a renderer backend needs for one output frame, in a
// backend-neutral form.  Built by RenderParamsBuilder from the geometry and
// colour objects, consumed by CpuRenderer / CudaRenderer / OpenClRenderer.
#pragma once

#include "osv/core/Result.h"
#include "osv/render/osv_kernel.h"
#include "osv/video/PlanarFrame.h"

#include <array>
#include <cstdint>
#include <vector>

namespace osv::render {

struct RenderJob {
    OsvRenderParams params{};                    ///< POD parameter block (copied to the device by value).
    std::array<OsvPlane, 2> planes{};            ///< Host plane descriptors (CPU / upload path).
    std::array<video::PlanarFrame16, 2> frames{};///< Keeps the decoded frames alive for the job's lifetime.
    std::array<video::DeviceFrameRef, 2> deviceFrames{}; ///< Optional GPU-resident inputs (CUDA zero-copy).

    /// True for a lens whose `planes` entry holds DEVICE pointers, because the
    /// decoder left that frame on the GPU and never copied it to the host
    /// (DecoderOptions::keepOnDevice).  Only a GPU backend on the same device
    /// can read such a job; the CPU and OpenCL backends refuse it rather than
    /// dereference device addresses.
    std::array<bool, 2> planesOnDevice{false, false};

    std::vector<float> seamShiftDeg;             ///< Per-column seam table (empty when disabled).

    /// 2-D parallax warp grid, interleaved (u, v) radians, warpW * warpH
    /// pairs (empty when disabled).  See osv_kernel.h and ParallaxWarp.h.
    std::vector<float> warpGrid;

    /// [WP-SEAM] Carved blend-seam table: interleaved (latitude, feather half
    /// width) radian pairs, blendSeamColumns of them (empty when disabled).
    /// See osv_kernel.h and SeamCarve.h.
    std::vector<float> blendSeam;

    /// [WP-PHOTO] Photometric seam table: the log2 gain grid (photoW *
    /// photoH * 3 floats) followed by the per-column usable rim (photoW * 2
    /// floats, radians).  Empty when params.photoEnabled is 0.  See
    /// osv_kernel.h and PhotoSeam.h.
    std::vector<float> photoField;

    /// [WP-PHOTO] Number of floats the photo table must hold for the photo
    /// fields of `p` (0 when the table is off or its shape is degenerate).
    [[nodiscard]] static std::size_t photoTableSize(const OsvRenderParams& p) noexcept {
        if (!p.photoEnabled || p.photoW <= 0 || p.photoH <= 1) {
            return 0;
        }
        const std::size_t w = static_cast<std::size_t>(p.photoW);
        return w * static_cast<std::size_t>(p.photoH) * 3u + w * 2u;
    }

    /// True when both planes describe usable memory and the output size is sane.
    [[nodiscard]] bool valid() const noexcept {
        if (params.outW <= 0 || params.outH <= 0 || params.outW > 32768 || params.outH > 32768) {
            return false;
        }
        for (const OsvPlane& p : planes) {
            if (!p.y || !p.u || !p.v || p.w <= 0 || p.h <= 0 || p.cw <= 0 || p.ch <= 0) {
                return false;
            }
        }
        if (params.seamShiftEnabled && seamShiftDeg.size() != static_cast<std::size_t>(params.seamColumns)) {
            return false;
        }
        // The kernel indexes the grid as warpW * warpH interleaved pairs; a
        // table that does not match those dimensions would read past its end.
        if (params.warpEnabled) {
            if (params.warpW <= 0 || params.warpH <= 0) {
                return false;
            }
            const std::size_t need = static_cast<std::size_t>(params.warpW) *
                                     static_cast<std::size_t>(params.warpH) * 2u;
            if (warpGrid.size() != need) {
                return false;
            }
        }
        // [WP-SEAM] The kernel reads two floats per blend-seam column; a
        // table of any other length would be indexed past its end.
        if (params.blendSeamEnabled) {
            if (params.blendSeamColumns <= 0 ||
                blendSeam.size() != static_cast<std::size_t>(params.blendSeamColumns) * 2u) {
                return false;
            }
        }
        // [WP-PHOTO] The kernel indexes the photo table by photoW / photoH;
        // any other size would read past its end.
        if (params.photoEnabled) {
            const std::size_t need = photoTableSize(params);
            if (need == 0 || photoField.size() != need) {
                return false;
            }
        }
        // [WP-SEAMTOOLS] The seam smoothing's low band is built by the
        // renderer from the planes, so the job carries no table - only the
        // fields that size and blur it, which must describe something every
        // backend can build and index safely.
        if (params.seamSmoothEnabled && !seamSmoothFieldsValid(params)) {
            return false;
        }
        return true;
    }

    /// [WP-SEAMTOOLS] True when the seam smoothing fields of `p` are usable
    /// (defined in SeamTools.cpp as seamSmoothParamsValid; declared here so
    /// the job's own check needs no extra include).
    [[nodiscard]] static bool seamSmoothFieldsValid(const OsvRenderParams& p) noexcept;
};

/// Fill an OsvPlane descriptor from a decoded frame.  Returns false when the
/// frame is not valid.
inline bool fillPlane(const video::PlanarFrame16& frame, OsvPlane& out) noexcept {
    if (!frame.valid()) {
        return false;
    }
    out.y = frame.plane[0];
    out.u = frame.plane[1];
    out.v = frame.plane[2];
    out.w = static_cast<int>(frame.width);
    out.h = static_cast<int>(frame.height);
    out.cw = static_cast<int>(frame.chromaW);
    out.ch = static_cast<int>(frame.chromaH);
    out.strideY = static_cast<int>(frame.strideElems[0]);
    out.strideC = static_cast<int>(frame.strideElems[1]);
    out.bitShift = frame.bitShift;
    out.chromaInterleaved = frame.chromaInterleaved ? 1 : 0;
    return true;
}

/// Fill an OsvPlane descriptor from a GPU-resident decoded frame: luma plus
/// interleaved CbCr (NV12 / P010 surfaces), one pitch for both planes.  The
/// pointers are DEVICE addresses - see RenderJob::planesOnDevice.  Returns
/// false when the reference is not valid.
inline bool fillDevicePlane(const video::DeviceFrameRef& ref, OsvPlane& out) noexcept {
    if (!ref.valid() || ref.pitchBytes % sizeof(osv_u16) != 0) {
        return false;
    }
    out.y = static_cast<const osv_u16*>(ref.yDevice);
    out.u = static_cast<const osv_u16*>(ref.uvDevice);
    out.v = out.u + 1;  // interleaved: Cr follows Cb, the sampler steps by 2
    out.w = static_cast<int>(ref.width);
    out.h = static_cast<int>(ref.height);
    out.cw = static_cast<int>((ref.width + 1) / 2);
    out.ch = static_cast<int>((ref.height + 1) / 2);
    out.strideY = static_cast<int>(ref.pitchBytes / sizeof(osv_u16));
    out.strideC = out.strideY;
    out.bitShift = ref.bitShift;
    out.chromaInterleaved = 1;
    return true;
}

}  // namespace osv::render
