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
    std::vector<float> seamShiftDeg;             ///< Per-column seam table (empty when disabled).

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
        return true;
    }
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

}  // namespace osv::render
