// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxRender.cpp - camera frames and the CPU pixel loops (OfxRender.h).
//
// Each loop has two bodies: the "native" one for a float RGBA target at full
// levels - Resolve's only format, written with exactly the stores it always
// was - and the general one for everything VEGAS can hand out, which packs
// every pixel through storeHostPixel() (OfxHostImage.h).  The format is
// decided once per call, never per pixel.

#include "OfxRender.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osv::ofx {

namespace {

/// True when a target takes the effects' own pixels unchanged: 32-bit float
/// R, G, B, A at full levels.  Such a target is written with plain float
/// stores (or one memcpy per row), bit for bit what every build before VEGAS
/// support wrote.
[[nodiscard]] bool nativeTarget(const HostImageView& dst, OutputLevels levels) noexcept {
    return dst.depth == HostDepth::Float && dst.order == HostOrder::Rgba && levels == OutputLevels::Full;
}

/// Run `body(row)` for `rows` rows on `pool`, or on this thread when there
/// is no pool.  False when the pool reports a failure.
template <class Body>
bool forEachRow(std::size_t rows, std::size_t grain, ThreadPool* pool, const Body& body) noexcept {
    if (pool) {
        return pool->parallelRows(rows, grain, body).ok();
    }
    for (std::size_t r = 0; r < rows; ++r) {
        body(r);
    }
    return true;
}

}  // namespace

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

reframe::ConstFrameView sourceView(const HostImageView& image) noexcept {
    reframe::ConstFrameView view;
    if (!image.usable()) {
        return view;
    }
    // The data pointer is the BOTTOM row (y up), so the frame is described
    // as bottom-up; buildParams() then points the sampler at it with flipY,
    // and a negative pitch (top-down memory) cancels out the same way.
    view.base = image.data;
    view.rowBytes = image.rowBytes;
    view.width = image.width();
    view.height = image.height();
    // Four samples per pixel either way; the layout carries only the SAMPLE
    // TYPE.  The channel ORDER is fixed up by the caller (isBgra) once the
    // setup is built, and an 8-bit source is promoted to float first.
    view.layout = image.depth == HostDepth::Byte ? reframe::PixelLayout::Bgra8u : reframe::PixelLayout::Bgra32f;
    view.topDown = false;
    return view;
}

bool renderReframeCpu(const reframe::KernelSetup& setup, const HostImageView& dst, const OfxRectI& window,
                      const OfxRectI& frame, OutputLevels levels, ThreadPool* pool) noexcept {
    if (!setup.valid || !setup.sourceRow0 || !dst.usable() || empty(frame)) {
        return false;
    }
    const OfxRectI area = intersect(window, dst.bounds);
    if (empty(area)) {
        return true;  // nothing of the window lies inside the image
    }
    const bool native = nativeTarget(dst, levels);
    const int pixelBytes = dst.pixelBytes();

    // One row per task: a 4K row is thousands of kernel evaluations, far
    // more than the cost of taking a chunk from the pool's queue.
    const auto renderRow = [&](std::size_t index) noexcept {
        const int y = area.y1 + static_cast<int>(index);
        const int camY = frame.y2 - 1 - y;
        char* pixel = dst.pixel(area.x1, y);
        if (native) {
            // Straight RGBA from the shared per-pixel function, straight into
            // the host's floats; pixels off the camera frame come back
            // transparent black.
            float* texel = reinterpret_cast<float*>(pixel);
            for (int x = area.x1; x < area.x2; ++x, texel += 4) {
                osvReframeEquirectPixel(&setup.params, &setup.source, setup.sourceRow0, x - frame.x1, camY, texel);
            }
            return;
        }
        // Any other format: the same function into a register-sized
        // quadruple, then the one packing rule (levels, depth, order).
        float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int x = area.x1; x < area.x2; ++x, pixel += pixelBytes) {
            osvReframeEquirectPixel(&setup.params, &setup.source, setup.sourceRow0, x - frame.x1, camY, rgba);
            storeHostPixel(pixel, dst.depth, dst.order, levels, rgba);
        }
    };

    const std::size_t rows = static_cast<std::size_t>(area.y2 - area.y1);
    return forEachRow(rows, 1, pool, renderRow);
}

bool copyStitchedFrame(const render::ImageRGBAf& image, const HostImageView& dst, const OfxRectI& window,
                       const OfxRectI& frame, OutputLevels levels, ThreadPool* pool) noexcept {
    if (!dst.usable() || empty(frame)) {
        return false;
    }
    // The stitch was asked for at exactly the frame's size; anything else
    // would make the row arithmetic below read outside the image.
    const long long frameW = static_cast<long long>(frame.x2) - frame.x1;
    const long long frameH = static_cast<long long>(frame.y2) - frame.y1;
    if (static_cast<long long>(image.w) != frameW || static_cast<long long>(image.h) != frameH ||
        image.data.size() < static_cast<std::size_t>(frameW) * static_cast<std::size_t>(frameH) * 4u) {
        return false;
    }

    // Output pixels outside the frame (a window wider than the RoD) are
    // transparent black; the frame's own pixels are overwritten below.
    clearCpu(dst, window, levels);
    const OfxRectI area = intersect(intersect(window, dst.bounds), frame);
    if (empty(area)) {
        return true;
    }
    const bool native = nativeTarget(dst, levels);
    const int pixelBytes = dst.pixelBytes();
    const std::size_t nativeBytes = static_cast<std::size_t>(area.x2 - area.x1) * 16u;

    const auto copyRow = [&](std::size_t index) noexcept {
        const int y = area.y1 + static_cast<int>(index);
        // The stitched image is top-down; OpenFX rows count up.
        const std::uint32_t imageRow = static_cast<std::uint32_t>(frame.y2 - 1 - y);
        const float* src = image.row(imageRow);
        if (!src) {
            return;
        }
        src += static_cast<std::ptrdiff_t>(area.x1 - frame.x1) * 4;
        char* out = dst.pixel(area.x1, y);
        if (native) {
            std::memcpy(out, src, nativeBytes);  // the stitch IS the host format
            return;
        }
        for (int x = area.x1; x < area.x2; ++x, src += 4, out += pixelBytes) {
            storeHostPixel(out, dst.depth, dst.order, levels, src);
        }
    };
    const std::size_t rows = static_cast<std::size_t>(area.y2 - area.y1);
    (void)forEachRow(rows, 8, pool, copyRow);
    return true;
}

void clearCpu(const HostImageView& dst, const OfxRectI& window, OutputLevels levels) noexcept {
    if (!dst.usable()) {
        return;
    }
    const OfxRectI area = intersect(window, dst.bounds);
    if (empty(area)) {
        return;
    }
    const int pixelBytes = dst.pixelBytes();
    const std::size_t rowBytes = static_cast<std::size_t>(area.x2 - area.x1) * static_cast<std::size_t>(pixelBytes);

    // Full levels: transparent black is all-zero bytes in every depth and
    // order, so each row is one memset.
    if (levels == OutputLevels::Full) {
        for (int y = area.y1; y < area.y2; ++y) {
            std::memset(dst.pixel(area.x1, y), 0, rowBytes);
        }
        return;
    }
    // Studio levels: black sits at 16/255, so one packed pixel is built and
    // repeated along the row.
    alignas(16) unsigned char packed[16] = {};
    const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    storeHostPixel(packed, dst.depth, dst.order, levels, transparent);
    for (int y = area.y1; y < area.y2; ++y) {
        char* out = dst.pixel(area.x1, y);
        for (int x = area.x1; x < area.x2; ++x, out += pixelBytes) {
            std::memcpy(out, packed, static_cast<std::size_t>(pixelBytes));
        }
    }
}

}  // namespace osv::ofx
