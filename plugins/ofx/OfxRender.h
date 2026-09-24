// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxRender.h - the CPU half of both effects' rendering: the camera frame of
// a render, and the pixel loop that fills an OpenFX image from a reframe
// setup.
//
// ===========================================================================
//  Coordinates, once
// ===========================================================================
// OpenFX pixel coordinates put y UP, and an image may cover any rectangle of
// them (its bounds).  The virtual camera (reframe::buildView) counts rows
// DOWN from the top of the frame it fills.  The frame a render fills is the
// output clip's region of definition at the render scale - the whole
// timeline frame in DaVinci Resolve - so:
//
//     camera column = x - frame.x1
//     camera row    = frame.y2 - 1 - y
//
// and the output pixel (x, y) lives at
//
//     data + (y - bounds.y1) * rowBytes + (x - bounds.x1) * 16 bytes.
//
// OfxReframeKernel.cu does exactly the same arithmetic on the GPU.
#pragma once

#include "OfxHost.h"

#include "ReframeCpu.h"

#include "osv/core/ThreadPool.h"

namespace osv::ofx {

/// The intersection of two rectangles (empty - x2 <= x1 or y2 <= y1 - when
/// they do not overlap).
[[nodiscard]] OfxRectI intersect(const OfxRectI& a, const OfxRectI& b) noexcept;

/// True when a rectangle has no pixels.
[[nodiscard]] inline bool empty(const OfxRectI& r) noexcept { return r.x2 <= r.x1 || r.y2 <= r.y1; }

/// The camera frame of a render: the region of definition of `clip` at
/// `time`, converted from canonical coordinates to pixels at render scale
/// (`sx`, `sy`) and pixel aspect `par`.  Falls back to `fallback` (the
/// output image's bounds) when the host cannot say.
[[nodiscard]] OfxRectI cameraFrame(OfxImageClipHandle clip, OfxTime time, double sx, double sy, double par,
                                   const OfxRectI& fallback) noexcept;

/// Describe an OpenFX float RGBA image as the source of a reframe: the
/// ConstFrameView reframe::buildParams() takes (bottom-up rows, handled with
/// the source descriptor's flipY), before the caller switches the channel
/// order to RGBA on the built setup.
[[nodiscard]] reframe::ConstFrameView sourceView(const ClipImage& image) noexcept;

/// Render `setup` into the float RGBA image `dst`, over `window` (clipped to
/// the image bounds), for a camera filling `frame`.  Rows run in parallel on
/// `pool` (null = this thread).  False - with nothing written - when the
/// setup or the image is unusable.  Pixels the camera does not cover come
/// out transparent black, so nothing needs clearing first.
bool renderReframeCpu(const reframe::KernelSetup& setup, const ClipImage& dst, const OfxRectI& window,
                      const OfxRectI& frame, ThreadPool* pool) noexcept;

/// Write transparent black over `window` of a float RGBA CPU image.
void clearCpu(const ClipImage& dst, const OfxRectI& window) noexcept;

}  // namespace osv::ofx
