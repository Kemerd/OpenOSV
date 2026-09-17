// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/render/CpuRenderer.h"
#include "osv/core/Log.h"

namespace osv::render {

Result<ImageRGBAf> CpuRenderer::render(const RenderJob& job) {
    if (!job.valid()) {
        return Error{ErrorCode::InvalidArgument, "CpuRenderer: invalid render job"};
    }
    OSV_TRY_ASSIGN(ImageRGBAf image, ImageRGBAf::create(static_cast<std::uint32_t>(job.params.outW),
                                                        static_cast<std::uint32_t>(job.params.outH)));

    // Copies of the POD blocks so the kernel reads exactly what a GPU would
    // receive by value (and so a caller mutating the job mid-render cannot
    // tear our reads).
    const OsvRenderParams params = job.params;
    const OsvPlane planes[2] = {job.planes[0], job.planes[1]};
    const float* seam = (params.seamShiftEnabled && !job.seamShiftDeg.empty()) ? job.seamShiftDeg.data() : nullptr;
    const int width = params.outW;
    float* pixels = image.data.data();

    // Row bands are independent; the pool hands out `m_rowGrain` rows per task.
    Status st = m_pool.parallelFor(0, static_cast<std::size_t>(params.outH), m_rowGrain,
                                   [&](std::size_t rowBegin, std::size_t rowEnd) {
                                       for (std::size_t y = rowBegin; y < rowEnd; ++y) {
                                           float* row = pixels + y * static_cast<std::size_t>(width) * 4u;
                                           for (int x = 0; x < width; ++x) {
                                               osvShadePixel(&params, planes, seam, x, static_cast<int>(y), row + x * 4);
                                           }
                                       }
                                   });
    OSV_TRY(st);
    return image;
}

}  // namespace osv::render
