// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxRender.cpp - camera frames and the CPU pixel loop (OfxRender.h).

#include "OfxRender.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osv::ofx {

OfxRectI intersect(const OfxRectI& a, const OfxRectI& b) noexcept {
    OfxRectI r;
    r.x1 = std::max(a.x1, b.x1);
    r.y1 = std::max(a.y1, b.y1);
    r.x2 = std::min(a.x2, b.x2);
    r.y2 = std::min(a.y2, b.y2);
    if (r.x2 < r.x1) {
        r.x2 = r.x1;
    }
    if (r.y2 < r.y1) {
        r.y2 = r.y1;
    }
    return r;
}

OfxRectI cameraFrame(OfxImageClipHandle clip, OfxTime time, double sx, double sy, double par,
                     const OfxRectI& fallback) noexcept {
    const OfxImageEffectSuiteV1* es = suites().effect;
    if (!es || !clip || !es->clipGetRegionOfDefinition || !std::isfinite(time)) {
        return fallback;
    }
    OfxRectD rod{0, 0, 0, 0};
    if (es->clipGetRegionOfDefinition(clip, time, &rod) != kOfxStatOK) {
        return fallback;
    }
    if (!(par > 0.0) || !std::isfinite(par)) {
        par = 1.0;
    }
    // Canonical -> pixel: scale by the render scale, and undo the pixel
    // aspect in x.  Rounded, because a RoD is a whole number of pixels at
    // every scale a host renders at, and floor/ceil would grow it by one on
    // the tiniest floating-point error.
    const double x1 = rod.x1 * sx / par;
    const double x2 = rod.x2 * sx / par;
    const double y1 = rod.y1 * sy;
    const double y2 = rod.y2 * sy;
    if (!std::isfinite(x1) || !std::isfinite(x2) || !std::isfinite(y1) || !std::isfinite(y2)) {
        return fallback;
    }
    // A RoD beyond anything renderable is a host that could not answer
    // (an infinite generator RoD, for instance), not a frame.
    constexpr double kLimit = 1.0e6;
    if (std::fabs(x1) > kLimit || std::fabs(x2) > kLimit || std::fabs(y1) > kLimit || std::fabs(y2) > kLimit) {
        return fallback;
    }
    OfxRectI frame;
    frame.x1 = static_cast<int>(std::lround(x1));
    frame.x2 = static_cast<int>(std::lround(x2));
    frame.y1 = static_cast<int>(std::lround(y1));
    frame.y2 = static_cast<int>(std::lround(y2));
    if (empty(frame)) {
        return fallback;
    }
    return frame;
}

reframe::ConstFrameView sourceView(const ClipImage& image) noexcept {
    reframe::ConstFrameView view;
    if (!image.valid()) {
        return view;
    }
    // The data pointer is the BOTTOM row (y up), so the frame is described
    // as bottom-up; buildParams() then points the sampler at it with flipY,
    // and a negative pitch (top-down memory) cancels out the same way.
    view.base = image.data;
    view.rowBytes = image.rowBytes;
    view.width = image.width();
    view.height = image.height();
    // Four 32-bit floats per pixel; the channel ORDER is fixed up by the
    // caller (isBgra = 0) once the setup is built.
    view.layout = reframe::PixelLayout::Bgra32f;
    view.topDown = false;
    return view;
}

bool renderReframeCpu(const reframe::KernelSetup& setup, const ClipImage& dst, const OfxRectI& window,
                      const OfxRectI& frame, ThreadPool* pool) noexcept {
    if (!setup.valid || !setup.sourceRow0 || !dst.isFloatRgba(true) || empty(frame)) {
        return false;
    }
    const OfxRectI area = intersect(window, dst.bounds);
    if (empty(area)) {
        return true;  // nothing of the window lies inside the image
    }

    // One row per task: a 4K row is thousands of kernel evaluations, far
    // more than the cost of taking a chunk from the pool's queue.
    const auto renderRow = [&](std::size_t index) noexcept {
        const int y = area.y1 + static_cast<int>(index);
        const int camY = frame.y2 - 1 - y;
        char* row = static_cast<char*>(dst.data) +
                    static_cast<std::ptrdiff_t>(y - dst.bounds.y1) * static_cast<std::ptrdiff_t>(dst.rowBytes);
        float* texel = reinterpret_cast<float*>(row) + static_cast<std::ptrdiff_t>(area.x1 - dst.bounds.x1) * 4;
        for (int x = area.x1; x < area.x2; ++x, texel += 4) {
            // Straight RGBA from the shared per-pixel function; pixels off
            // the camera frame come back transparent black.
            osvReframeEquirectPixel(&setup.params, &setup.source, setup.sourceRow0, x - frame.x1, camY, texel);
        }
    };

    const std::size_t rows = static_cast<std::size_t>(area.y2 - area.y1);
    if (pool) {
        return pool->parallelRows(rows, 1, renderRow).ok();
    }
    for (std::size_t r = 0; r < rows; ++r) {
        renderRow(r);
    }
    return true;
}

void clearCpu(const ClipImage& dst, const OfxRectI& window) noexcept {
    if (!dst.isFloatRgba(true)) {
        return;
    }
    const OfxRectI area = intersect(window, dst.bounds);
    if (empty(area)) {
        return;
    }
    const std::size_t bytes = static_cast<std::size_t>(area.x2 - area.x1) * 16u;
    for (int y = area.y1; y < area.y2; ++y) {
        char* row = static_cast<char*>(dst.data) +
                    static_cast<std::ptrdiff_t>(y - dst.bounds.y1) * static_cast<std::ptrdiff_t>(dst.rowBytes) +
                    static_cast<std::ptrdiff_t>(area.x1 - dst.bounds.x1) * 16;
        std::memset(row, 0, bytes);
    }
}

}  // namespace osv::ofx
