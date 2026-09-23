// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectRender: the reframe effect's view, rendered STRAIGHT FROM THE TWO
// FISHEYE FRAMES instead of out of a stitched equirect.
//
// ---------------------------------------------------------------------------
//  Why this exists
// ---------------------------------------------------------------------------
// The two-step path the effect has always used is
//
//     importer:  fisheyes --(osvShadePixelW, OSV_MODE_EQUIRECT)--> 6000x3000 equirect
//     effect:    equirect --(osvReframeEquirectPixel)-------------> the user's view
//
// It resamples twice, and it routes a whole 18 MP sphere through host memory
// so that a 2560x1440 window can be cut out of it (docs/DIRECT_GPU.md).  The
// direct path traces each output pixel's ray through the SAME camera and the
// SAME stitch in one step:
//
//     fisheyes --(osvShadePixelW, OSV_MODE_REFRAME)--> the user's view
//
// ---------------------------------------------------------------------------
//  Why the framing is identical, by construction
// ---------------------------------------------------------------------------
// Follow one output pixel through the two-step path:
//
//   1. the effect turns it into a view ray and rotates it with its own
//      Rout_view = R_source * R_camera (buildView());
//   2. the equirect sampler reads the panorama at that direction's
//      (longitude, latitude);
//   3. the importer rendered that panorama pixel by rotating the same
//      direction with ITS Rout, which for an equirect with no camera is the
//      frame's stabilisation rotation R_stab = body <- world
//      (RenderParamsBuilder: Rout = bodyFromWorld * I), and projecting the
//      result into the lenses.
//
// Steps 2 and 3 are inverse mappings of the same (lon, lat), so the lenses
// see R_stab * Rout_view * d_view.  The direct path therefore uses
//
//     Rout_direct = R_stab * Rout_view
//
// with the view ray, focal length, eye offset and cover-fit taken verbatim
// from buildView() - the very function the equirect path uses - and every
// stitch field (lens blocks, blend, seam, warp, colour, alpha) taken
// verbatim from the importer's own equirect parameter block.  Nothing about
// the camera is recomputed here, so the two paths cannot drift apart.
//
// ---------------------------------------------------------------------------
//  What is NOT here
// ---------------------------------------------------------------------------
// How the fisheye frames and the stitch state are obtained (the GPU clip
// decoder, the engine ABI, the node walk to the source clip) belongs to other
// work packages; this header only needs their RESULT: two OsvPlane
// descriptors and a StitchState.  The CUDA launch lives in DirectLaunch.h so
// this header stays free of the driver API and the CPU twin can be used by a
// build (or a test) that has no GPU at all.
#pragma once

#include "ReframeCpu.h"
#include "ReframeParams.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/osv_kernel.h"

namespace osv::reframe {

// ===========================================================================
//  Inputs
// ===========================================================================

/// The stitch state of ONE source frame: everything the importer would use to
/// render that frame's equirect, except the pixels.
///
/// `equirect` is the parameter block the importer renders the frame's
/// equirect with - exactly what `render::RenderParamsBuilder` returns for
/// `.rig(rig).color(cp).blend(blend, on).alphaCoverage(on).gain(..)
/// .seam(..)/.warp(..).stabilization(stabilizationFor(index))
/// .equirect(EquirectMap Standard)`.  Carrying the whole block rather than a
/// hand-picked subset is deliberate: every stitch field the importer honours
/// (and any field added to OsvRenderParams later) reaches the direct kernel
/// untouched, so the stitch cannot quietly differ between the two paths.
///
/// The fields the direct path READS from it:
///   * `lens[0..1]`          rig intrinsics/extrinsics, FOV feather, per-lens
///                           exposure gain, occlusion polygon, frame size;
///   * `color`               the whole colour pipeline (input decode, gamut,
///                           exposure, output transfer);
///   * `blendEnabled`, `outputAlphaCoverage` - blend flags;
///   * `seamShiftEnabled`, `seamColumns` - the 1-D seam table's shape;
///   * `warpEnabled`, `warpW`, `warpH`, `warpLatMinRad`, `warpLatMaxRad`,
///     `warpSinLatLo`, `warpSinLatHi` - the 2-D parallax grid's shape;
///   * `Rout`                the frame's STABILISATION rotation, body <-
///                           world.  An equirect block has no camera, so its
///                           Rout is exactly `bodyFromWorld` (identity with
///                           stabilisation off).
///
/// The fields it OVERWRITES (they describe the importer's panorama, not the
/// user's view): outW, outH, mode, projection, layout, focalPx, tanHalfH,
/// tanHalfV, eyeOffset, Rout.
///
/// `mode` must be OSV_MODE_EQUIRECT and `layout` OSV_LAYOUT_STANDARD: that is
/// the only panorama the effect's equirect path samples, and it is what
/// makes `Rout` mean "stabilisation only".  Any other block is refused.
struct StitchState {
    OsvRenderParams equirect{};  ///< The importer's equirect block for this frame (see above).

    /// The per-column seam table, `equirect.seamColumns` floats in degrees,
    /// or nullptr when `equirect.seamShiftEnabled` is 0.  A DEVICE pointer
    /// when the setup is launched on the GPU, a HOST pointer for the CPU
    /// twin; the builder never dereferences it.
    const float* seamTable = nullptr;

    /// The 2-D parallax warp grid, `warpW * warpH` interleaved (dLon, dLat)
    /// radian pairs, or nullptr when `equirect.warpEnabled` is 0.  Device or
    /// host pointer by the same rule as `seamTable`; never dereferenced here.
    const float* warpGrid = nullptr;

    /// [WP-SEAM] The carved blend-seam table, `equirect.blendSeamColumns`
    /// interleaved (latitude, feather half width) radian pairs, or nullptr
    /// when `equirect.blendSeamEnabled` is 0.  Device or host pointer by the
    /// same rule as `seamTable`; never dereferenced here.
    const float* blendSeam = nullptr;

    /// [WP-PHOTO] The photometric seam table: `equirect.photoW *
    /// equirect.photoH * 3` gain floats then `equirect.photoW * 2` rim floats,
    /// or nullptr when `equirect.photoEnabled` is 0.  Device or host pointer
    /// by the same rule as `seamTable`; never dereferenced here.
    const float* photoField = nullptr;

    /// [WP-SEAMTOOLS] The seam smoothing's two-lens low band (2 x
    /// `equirect.seamLowW` x `equirect.seamLowH` RGBA texels, see
    /// osv_kernel.h), or nullptr when `equirect.seamSmoothEnabled` is 0.
    /// Device or host pointer by the same rule as `seamTable`; never
    /// dereferenced here.
    const float* seamLow = nullptr;
};

// ===========================================================================
//  The built setup
// ===========================================================================

/// Why buildDirectParams() refused to build a setup.  Every refusal names
/// one, so a log line says WHICH check fired instead of only that one did.
enum class DirectReject {
    None = 0,      ///< The setup is valid.
    View,          ///< buildView() refused the camera; see DirectSetup::viewReject.
    Viewport,      ///< The camera paints a sub-rectangle, which OsvRenderParams cannot express.
    StitchLayout,  ///< The stitch block is not a Standard-layout equirect block.
    Rotation,      ///< The stabilisation Rout is non-finite or not a proper rotation.
    Lens,          ///< A lens block is disabled on both sides, non-finite or out of range.
    Color,         ///< The colour block carries a non-finite value or an out-of-range enum.
    SeamTable,     ///< Seam shift is on but the table is missing or its size is implausible.
    WarpGrid,      ///< The warp grid is on but missing, degenerate or non-finite in its span.
    Composed,      ///< The composed view block came out non-finite (defensive backstop).
    BlendSeam,     ///< [WP-SEAM] The blend seam is on but its table is missing or its shape implausible.
    PhotoField,    ///< [WP-PHOTO] The photo table is on but missing, degenerate or non-finite in its span.
    SeamLow,       ///< [WP-SEAMTOOLS] Seam smoothing is on but its low band is missing or its fields malformed.
    Shading,       ///< [WP-VIGNETTE] The lens shading block is on but carries a non-finite or out-of-range value.
};

/// Human-readable name of a refusal reason, for the log line.
[[nodiscard]] const char* directRejectName(DirectReject reason) noexcept;

/// Everything the direct kernel (and its CPU twin) needs besides the pixels.
///
/// `params` is a complete OsvRenderParams in OSV_MODE_REFRAME: the camera from
/// buildView(), the stitch from StitchState::equirect, and Rout composed as
/// R_stab * Rout_view.  `seamTable` / `warpGrid` / `blendSeam` are the StitchState pointers,
/// forwarded untouched when their feature is enabled and nulled when it is
/// not, so the kernel never receives a pointer it will not use.
struct DirectSetup {
    OsvRenderParams params{};           ///< The block the shader reads.
    const float* seamTable = nullptr;   ///< Seam table (host or device), or nullptr.
    const float* warpGrid = nullptr;    ///< Warp grid (host or device), or nullptr.
    const float* blendSeam = nullptr;   ///< [WP-SEAM] Blend-seam table (host or device), or nullptr.
    const float* photoField = nullptr;  ///< [WP-PHOTO] Photometric seam table (host or device), or nullptr.
    const float* seamLow = nullptr;     ///< [WP-SEAMTOOLS] Seam smoothing low band (host or device), or nullptr.
    bool valid = false;                 ///< False when any input was unusable.
    DirectReject reject = DirectReject::View;  ///< `None` exactly when `valid`.
    /// The camera's own refusal reason when `reject == DirectReject::View`;
    /// `SetupReject::None` otherwise.
    SetupReject viewReject = SetupReject::None;
};

// ===========================================================================
//  The builder
// ===========================================================================

/// Map the effect's controls plus one frame's stitch state onto the direct
/// kernel's parameter block.
///
/// `settings`      the resolved controls (already smoothed, if smoothing is on)
///                 - exactly what the equirect path passes to buildParams();
/// `stitch`        the source frame's stitch state (see StitchState);
/// `outW`, `outH`  the output frame size (Premiere's render size, which may
///                 be a preview fraction of the sequence);
/// `sequenceSize`  the sequence frame size, invalid when unknown.
///
/// The framing is the equirect path's framing composed with the importer's
/// stabilisation (see the file comment), measured by the framing-parity test
/// in tests/premiere/reframe/test_direct.cpp.
///
/// Refuses - `valid == false`, with a named reason - rather than build a block
/// a kernel could misbehave on: a non-finite or non-rotation stabilisation,
/// a lens block with a NaN, an out-of-range polygon count or no enabled lens,
/// a colour block with a NaN, a seam/warp feature switched on with no data,
/// a sub-rectangle viewport, or anything buildView() refuses.  Non-finite
/// CONTROL values are not refusals: buildView() replaces them with their
/// documented defaults, exactly as the equirect path does.
[[nodiscard]] DirectSetup buildDirectParams(const Settings& settings, const StitchState& stitch, int outW, int outH,
                                            SizePx sequenceSize) noexcept;

/// True when `planes` (two descriptors, [0] = slave, [1] = master) are
/// usable with `setup`: every ENABLED lens has non-null plane pointers, the
/// luma size the lens block expects, chroma planes that fit inside a 4:2:0
/// frame of that size, strides that cover a row, a bit shift in [0, 15] and,
/// for interleaved chroma, v == u + 1.  A disabled lens is never sampled, so
/// its descriptor is not inspected.
///
/// Only the DESCRIPTORS are checked: the pointers may be device addresses
/// (the GPU launch) and are never dereferenced.
[[nodiscard]] bool planesMatch(const DirectSetup& setup, const OsvPlane* planes) noexcept;

// ===========================================================================
//  The CPU twin
// ===========================================================================

/// One pixel of the direct render on the CPU: osvShadePixelWSPL() in
/// OSV_MODE_REFRAME, with no layout conversion.  `planes` must be HOST
/// descriptors (and the setup's seam/warp pointers host pointers).  Writes
/// transparent black and returns false for an invalid setup, unusable planes
/// or a pixel outside the frame.
bool renderDirectPixel(const DirectSetup& setup, const OsvPlane* planes, int x, int y, float out[4]) noexcept;

/// Render the whole view on the CPU into `dst` - the reference the GPU
/// kernel is compared against, and the software fallback for a host frame.
///
/// `planes` are two HOST plane descriptors ([0] slave, [1] master); the
/// setup's seam/warp pointers must be host pointers as well.  `dst` may be
/// any PixelLayout and either row order; it must be exactly outW x outH.
/// Rows run in parallel on `pool` (nullptr = the calling thread).
///
/// Validates everything before writing a single byte and returns false - with
/// `dst` untouched - for an invalid setup, planes that fail planesMatch(), an
/// unusable destination or a size mismatch.  Every output pixel is written,
/// so the caller never has to clear first.
bool renderDirectCpu(const DirectSetup& setup, const OsvPlane* planes, const FrameView& dst,
                     ThreadPool* pool) noexcept;

}  // namespace osv::reframe
