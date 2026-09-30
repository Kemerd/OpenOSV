// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxRender.h - the CPU half of both effects' rendering: the camera frame of
// a render, and the pixel loops that fill an OpenFX image from a reframe
// setup or from a stitched frame.
//
// ===========================================================================
//  Coordinates, once
// ===========================================================================
// OpenFX pixel coordinates put y UP, and an image may cover any rectangle of
// them (its bounds).  The virtual camera (reframe::buildView) counts rows
// DOWN from the top of the frame it fills.  The frame a render fills is the
// output clip's region of definition at the render scale - the whole
// timeline frame in DaVinci Resolve and in VEGAS Pro - so:
//
//     camera column = x - frame.x1
//     camera row    = frame.y2 - 1 - y
//
// and the output pixel (x, y) lives at
//
//     data + (y - bounds.y1) * rowBytes + (x - bounds.x1) * bytesPerPixel.
//
// OfxReframeKernel.cu does exactly the same arithmetic on the GPU.
//
// ===========================================================================
//  Formats, once
// ===========================================================================
// Every loop here reads and writes a HostImageView (OfxHostImage.h), so the
// same code serves Resolve's float RGBA images and VEGAS's 8-bit / float,
// RGBA / BGRA ones.  Writing always goes through storeHostPixel()'s rule:
// levels on RGB (never alpha), then the depth, then the order.  A float RGBA
// target at full levels - the only kind Resolve hands out - is written with
// the very stores it always was, so its output is bit for bit unchanged.
#pragma once

#include "OfxHost.h"
#include "OfxHostImage.h"

#include "ReframeCpu.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/ImageRGBAf.h"

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

/// Describe an OpenFX host image as the source of a reframe: the
/// ConstFrameView reframe::buildParams() takes (bottom-up rows, handled with
/// the source descriptor's flipY).  A float image is Bgra32f, an 8-bit one
/// Bgra8u - which buildParams() refuses until reframe::promoteIntegerToFloat()
/// has made a float copy of it.  The layout names only the SAMPLE TYPE; the
/// caller sets the channel order on the built setup (source.isBgra) from
/// `image.order`.  An unusable view gives an invalid ConstFrameView.
[[nodiscard]] reframe::ConstFrameView sourceView(const HostImageView& image) noexcept;

/// Render `setup` into the host image `dst`, over `window` (clipped to the
/// image bounds), for a camera filling `frame`, packing every pixel in
/// `levels` (storeHostPixel()).  Rows run in parallel on `pool` (null = this
/// thread).  False - with nothing written - when the setup or the image is
/// unusable.  Pixels the camera does not cover come out transparent black
/// (in `levels`), so nothing needs clearing first.
bool renderReframeCpu(const reframe::KernelSetup& setup, const HostImageView& dst, const OfxRectI& window,
                      const OfxRectI& frame, OutputLevels levels, ThreadPool* pool) noexcept;

/// Write the stitched frame `image` - top-down straight RGBA floats, exactly
/// `frame`'s size - into `dst` over `window` (clipped to the image bounds),
/// packing every pixel in `levels`.  Output pixels of the window outside
/// `frame` (a window wider than the region of definition) come out
/// transparent black.  Rows run in parallel on `pool` (null = this thread).
/// False - with nothing written - when the image or the view is unusable,
/// or `image` is not `frame`'s size.
bool copyStitchedFrame(const render::ImageRGBAf& image, const HostImageView& dst, const OfxRectI& window,
                       const OfxRectI& frame, OutputLevels levels, ThreadPool* pool) noexcept;

/// Write transparent black - (0, 0, 0, 0) before `levels` - over `window` of
/// a CPU host image (clipped to its bounds).  Nothing is written to an
/// unusable view.
void clearCpu(const HostImageView& dst, const OfxRectI& window, OutputLevels levels = OutputLevels::Full) noexcept;

}  // namespace osv::ofx
