// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors

#include "osv/render/CpuRenderer.h"
#include "osv/core/Log.h"

namespace osv::render {

Result<ImageRGBAf> CpuRenderer::render(const RenderJob& job) {
    // A fresh image grows from empty, so reshape() value-initialises it
    // exactly as create() did - the result is byte-identical to before.
    ImageRGBAf image;
    OSV_TRY(renderInto(job, image));
    return image;
}

Status CpuRenderer::renderInto(const RenderJob& job, ImageRGBAf& image) {
    if (!job.valid()) {
        return failStatus(ErrorCode::InvalidArgument, "CpuRenderer: invalid render job");
    }
    // A job whose planes are GPU addresses (a keepOnDevice decode) cannot be
    // read from the host; dereferencing them would fault.
    if (job.planesOnDevice[0] || job.planesOnDevice[1]) {
        return failStatus(ErrorCode::InvalidArgument,
                          "CpuRenderer: the lens frames are on the GPU (decode with keepOnDevice = false for the CPU)");
    }
    // Reuse the caller's allocation.  No zero-fill is needed: the loop below
    // writes every pixel of every row.
    OSV_TRY(image.reshape(static_cast<std::uint32_t>(job.params.outW), static_cast<std::uint32_t>(job.params.outH)));

    // Copies of the POD blocks so the kernel reads exactly what a GPU would
    // receive by value (and so a caller mutating the job mid-render cannot
    // tear our reads).
    const OsvRenderParams params = job.params;
    const OsvPlane planes[2] = {job.planes[0], job.planes[1]};
    const float* seam = (params.seamShiftEnabled && !job.seamShiftDeg.empty()) ? job.seamShiftDeg.data() : nullptr;
    // The 2-D parallax grid, when one was built.  job.valid() has already
    // checked that its size matches warpW * warpH, so the kernel's indexing
    // is bounded by construction rather than by trust.
    const float* warp = (params.warpEnabled && !job.warpGrid.empty()) ? job.warpGrid.data() : nullptr;
    // [WP-SEAM] The carved blend-seam table, size-checked by job.valid().
    const float* blendSeam = (params.blendSeamEnabled && !job.blendSeam.empty()) ? job.blendSeam.data() : nullptr;
    // [WP-PHOTO] The photometric seam table, size-checked by job.valid().
    const float* photo = (params.photoEnabled && !job.photoField.empty()) ? job.photoField.data() : nullptr;
    const int width = params.outW;
    float* pixels = image.data.data();

    // Row bands are independent; the pool hands out `m_rowGrain` rows per task.
    Status st = m_pool.parallelFor(0, static_cast<std::size_t>(params.outH), m_rowGrain,
                                   [&](std::size_t rowBegin, std::size_t rowEnd) {
                                       for (std::size_t y = rowBegin; y < rowEnd; ++y) {
                                           float* row = pixels + y * static_cast<std::size_t>(width) * 4u;
                                           for (int x = 0; x < width; ++x) {
                                               osvShadePixelWSP(&params, planes, seam, warp, blendSeam, photo,
                                                                x, static_cast<int>(y), row + x * 4);
                                           }
                                       }
                                   });
    OSV_TRY(st);
    return okStatus();
}

}  // namespace osv::render
