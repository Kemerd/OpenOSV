// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DirectRender.cpp - the parameter builder and the CPU twin of the direct
// (fisheye -> view) reframe.
//
// Nothing in this file reimplements the camera, the stitch or the colour
// pipeline:
//
//   * the camera comes from buildView() (ReframeCpu.cpp), the function the
//     equirect path uses, so both paths frame the picture identically;
//   * the stitch comes from the importer's own equirect parameter block,
//     copied field for field;
//   * every pixel comes from osvShadePixelWS() in osv_kernel.h, the function
//     the GPU kernel runs too.
//
// What IS here: composing the stabilisation with the camera rotation, and
// refusing - with a named reason - every input a kernel could misbehave on.
// See DirectRender.h for the derivation of Rout_direct = R_stab * Rout_view.

#include "DirectRender.h"

#include "osv/color/ColorParams.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace osv::reframe {

namespace {

/// Largest frame edge the direct path accepts, for the output and for a lens
/// frame alike.  The same bound ReframeCpu.cpp applies (kMaxEdge there): a
/// corrupt size must be refused before it is multiplied into an offset.
constexpr int kMaxEdge = 65536;

/// Largest seam table we believe.  The importer's tables are a few hundred
/// columns; anything near this is a corrupt block, not a finer measurement.
constexpr int kMaxSeamColumns = 65536;

/// Largest warp-grid edge we believe, for the same reason.
constexpr int kMaxWarpEdge = 65536;

/// How far a matrix may stray from orthonormal and still be accepted as a
/// rotation.  A double rotation rounded to float is off by ~1e-7, so 1e-3 is
/// four orders of magnitude of headroom - while a genuinely wrong block
/// (a scale, a shear, a transposed garbage read) misses it by far more.
constexpr double kRotationTolerance = 1e-3;

/// The shader indexes planes with 32-bit int arithmetic (osvFetchPlane:
/// `plane[y * stride + x * step]`), so the largest index a plane can produce
/// must stay below INT_MAX.  Checked on the host so no kernel ever wraps.
constexpr std::int64_t kMaxPlaneElements = 0x7FFFFFFFll;

/// True when every one of `count` floats is finite.
[[nodiscard]] bool allFinite(const float* values, int count) noexcept {
    if (!values || count < 0) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

/// True when the row-major float 3x3 `m` is a proper rotation within
/// kRotationTolerance: finite, orthonormal rows, determinant +1.
[[nodiscard]] bool isRotation(const float* m) noexcept {
    if (!allFinite(m, 9)) {
        return false;
    }
    // R * R^T must be the identity, entry by entry.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double dot = 0.0;
            for (int k = 0; k < 3; ++k) {
                dot += static_cast<double>(m[r * 3 + k]) * static_cast<double>(m[c * 3 + k]);
            }
            const double expected = (r == c) ? 1.0 : 0.0;
            if (std::fabs(dot - expected) > kRotationTolerance) {
                return false;
            }
        }
    }
    // Orthonormal is not enough: a reflection (det -1) would mirror the
    // picture, so the determinant must be +1.
    const double det = static_cast<double>(m[0]) * (static_cast<double>(m[4]) * m[8] - static_cast<double>(m[5]) * m[7]) -
                       static_cast<double>(m[1]) * (static_cast<double>(m[3]) * m[8] - static_cast<double>(m[5]) * m[6]) +
                       static_cast<double>(m[2]) * (static_cast<double>(m[3]) * m[7] - static_cast<double>(m[4]) * m[6]);
    return std::fabs(det - 1.0) <= kRotationTolerance;
}

/// True when one lens block is safe to hand to the shader.
///
/// Every value the shader divides by, takes a root of or indexes with is
/// checked: a NaN focal length would poison every ray of that lens, and an
/// occlusion count above OSV_MAX_OCCLUSION_POINTS would walk the polygon
/// arrays off the end of the parameter block.
[[nodiscard]] bool lensValid(const OsvLens& L) noexcept {
    // Pinhole part: positive, finite focal lengths and a finite centre.
    if (!std::isfinite(L.fx) || !std::isfinite(L.fy) || !(L.fx > 0.0f) || !(L.fy > 0.0f)) {
        return false;
    }
    if (!std::isfinite(L.cx) || !std::isfinite(L.cy)) {
        return false;
    }
    // Radial polynomial and the body -> lens rotation.
    if (!allFinite(L.k, 5) || !isRotation(L.R)) {
        return false;
    }
    // Field of view: a usable half angle, at most the whole sphere.
    if (!std::isfinite(L.thetaMax) || !(L.thetaMax > 0.0f) || L.thetaMax > OSV_KERNEL_PI) {
        return false;
    }
    if (!std::isfinite(L.featherRad) || L.featherRad < 0.0f) {
        return false;
    }
    // Exposure gains multiply linear light; negative would invert colour.
    if (!allFinite(L.gain, 3) || L.gain[0] < 0.0f || L.gain[1] < 0.0f || L.gain[2] < 0.0f) {
        return false;
    }
    // Occlusion polygon: a count inside the arrays, finite vertices.
    if (L.occlN < 0 || L.occlN > OSV_MAX_OCCLUSION_POINTS) {
        return false;
    }
    if (!allFinite(L.occlX, L.occlN) || !allFinite(L.occlY, L.occlN) || !std::isfinite(L.occlFeatherPx)) {
        return false;
    }
    // Decoded frame size, which osvProjectLens clips against.
    return L.width > 0 && L.height > 0 && L.width <= kMaxEdge && L.height <= kMaxEdge;
}

/// The row-major product a * b of two float 3x3 matrices, accumulated in
/// double and rounded once, so the composition adds no error beyond the
/// final float rounding the kernel's own Rout carries anyway.
void multiply3(const float* a, const float* b, float* out) noexcept {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += static_cast<double>(a[r * 3 + k]) * static_cast<double>(b[k * 3 + c]);
            }
            out[r * 3 + c] = static_cast<float>(sum);
        }
    }
}

/// One refusal: an invalid setup that names its reason.  Logging is left to
/// the caller, which knows the frame and the context the refusal belongs to
/// (and whether it is already falling back to the equirect path).
DirectSetup refuse(DirectReject reason, SetupReject viewReason = SetupReject::None) noexcept {
    DirectSetup setup;
    setup.valid = false;
    setup.reject = reason;
    setup.viewReject = viewReason;
    return setup;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Names
// ---------------------------------------------------------------------------
const char* directRejectName(DirectReject reason) noexcept {
    switch (reason) {
    case DirectReject::None:         return "none";
    case DirectReject::View:         return "the virtual camera could not be built";
    case DirectReject::Viewport:     return "the camera paints a sub-rectangle the direct kernel cannot express";
    case DirectReject::StitchLayout: return "the stitch block is not a Standard-layout equirect block";
    case DirectReject::Rotation:     return "the stabilisation rotation is not a finite proper rotation";
    case DirectReject::Lens:         return "a lens block is unusable (or no lens is enabled)";
    case DirectReject::Color:        return "the colour block carries a non-finite or out-of-range value";
    case DirectReject::SeamTable:    return "seam shift is enabled without a usable seam table";
    case DirectReject::WarpGrid:     return "the warp grid is enabled without a usable grid";
    case DirectReject::Composed:     return "the composed view block came out non-finite";
    case DirectReject::BlendSeam:    return "the blend seam is enabled without a usable table";
    }
    // Unreachable for any enumerator above; a corrupt value lands here
    // rather than off the end of a table.
    return "unknown";
}

// ---------------------------------------------------------------------------
//  The builder
// ---------------------------------------------------------------------------
DirectSetup buildDirectParams(const Settings& settings, const StitchState& stitch, int outW, int outH,
                              SizePx sequenceSize) noexcept {
    const OsvRenderParams& eq = stitch.equirect;

    // ---- the stitch block must be what the equirect path samples ---------
    // A Standard-layout equirect is the only panorama osvReframeEquirectPixel
    // reads, and it is what makes the block's Rout "stabilisation and nothing
    // else".  A reframe block (or a polar-axis one) carries a different
    // rotation meaning, and composing it would silently mis-frame the view.
    if (eq.mode != OSV_MODE_EQUIRECT || eq.layout != OSV_LAYOUT_STANDARD) {
        return refuse(DirectReject::StitchLayout);
    }

    // ---- the stabilisation rotation ----------------------------------------
    // It is composed into every ray, so it must be a genuine rotation: a
    // scaled or sheared matrix would bend the picture, a reflection would
    // mirror it, and a NaN would blank it.
    if (!isRotation(eq.Rout)) {
        return refuse(DirectReject::Rotation);
    }

    // ---- the lens blocks ---------------------------------------------------
    // A disabled lens is never read by the shader, so only enabled ones are
    // inspected - but at least one must be enabled, or every pixel would be
    // transparent black and the render pointless.
    int enabledLenses = 0;
    for (int i = 0; i < 2; ++i) {
        const OsvLens& L = eq.lens[i];
        if (!L.enabled) {
            continue;
        }
        if (!lensValid(L)) {
            return refuse(DirectReject::Lens);
        }
        ++enabledLenses;
    }
    if (enabledLenses == 0) {
        return refuse(DirectReject::Lens);
    }

    // ---- colour ------------------------------------------------------------
    // The library's own validity rule (every float finite, enums in range),
    // the same one makeColorParams() output satisfies by construction.
    if (!color::colorParamsValid(eq.color)) {
        return refuse(DirectReject::Color);
    }

    // ---- the 1-D seam table ------------------------------------------------
    // Switched on means the importer applied it, so the direct render must
    // too; with no table to read that is not something to paper over by
    // rendering without it - the two paths would disagree at the seam.
    const bool seamOn = eq.seamShiftEnabled != 0;
    if (seamOn && (eq.seamColumns <= 0 || eq.seamColumns > kMaxSeamColumns || stitch.seamTable == nullptr)) {
        return refuse(DirectReject::SeamTable);
    }

    // ---- the 2-D warp grid -------------------------------------------------
    // Same rule as the seam table, plus the grid's latitude span: the shader
    // divides by it, so a degenerate or non-finite span is refused here.
    const bool warpOn = eq.warpEnabled != 0;
    if (warpOn) {
        const bool sizeOk = eq.warpW > 0 && eq.warpW <= kMaxWarpEdge && eq.warpH > 1 && eq.warpH <= kMaxWarpEdge;
        const bool spanOk = std::isfinite(eq.warpLatMinRad) && std::isfinite(eq.warpLatMaxRad) &&
                            std::fabs(eq.warpLatMaxRad - eq.warpLatMinRad) > 1e-6f &&
                            std::isfinite(eq.warpSinLatLo) && std::isfinite(eq.warpSinLatHi);
        if (!sizeOk || !spanOk || stitch.warpGrid == nullptr) {
            return refuse(DirectReject::WarpGrid);
        }
    }

    // ---- [WP-SEAM] the carved blend seam ---------------------------------
    // Same rule as the seam table: on means the importer stitched with it, so
    // the direct render must too.  The kernel reads two floats per column
    // and divides by the edge ramp, so it must be finite and non-negative.
    const bool blendSeamOn = eq.blendSeamEnabled != 0;
    if (blendSeamOn) {
        const bool shapeOk = eq.blendSeamColumns > 0 && eq.blendSeamColumns <= kMaxSeamColumns;
        const bool rampOk = std::isfinite(eq.blendSeamEdgeRad) && eq.blendSeamEdgeRad >= 0.0f;
        if (!shapeOk || !rampOk || stitch.blendSeam == nullptr) {
            return refuse(DirectReject::BlendSeam);
        }
    }

    // ---- the camera --------------------------------------------------------
    // Built by the equirect path's own function.  Non-finite controls are
    // replaced by their defaults inside it, exactly as the equirect path
    // would do for the same frame.
    const ViewSetup view = buildView(settings, outW, outH, sequenceSize);
    if (!view.valid) {
        return refuse(DirectReject::View, view.reject);
    }
    const OsvReframeParams& v = view.params;

    // The equirect kernel confines the picture to its viewport rectangle;
    // OsvRenderParams has no such rectangle and always paints the whole
    // frame.  computeViewport() covers the whole frame by design today, and
    // this refusal is what keeps that assumption from ever failing silently:
    // a future sub-rectangle sends the effect back to the equirect path
    // instead of rendering a mis-framed picture.
    if (v.viewX != 0 || v.viewY != 0 || v.viewW != outW || v.viewH != outH || v.outW != outW || v.outH != outH) {
        return refuse(DirectReject::Viewport);
    }

    // ---- compose -----------------------------------------------------------
    DirectSetup setup;
    // Start from the importer's block so every stitch field - including any
    // added to OsvRenderParams after this was written - reaches the kernel
    // exactly as the importer's equirect render saw it.
    setup.params = eq;

    // Then overwrite the fields that describe the importer's PANORAMA with
    // the ones that describe the user's VIEW.
    OsvRenderParams& p = setup.params;
    p.outW = outW;
    p.outH = outH;
    p.mode = OSV_MODE_REFRAME;
    p.projection = v.projection;
    // Unused in reframe mode; set to the documented default so the block
    // never carries the panorama's value into a context where it means
    // nothing.
    p.layout = OSV_LAYOUT_STANDARD;
    p.focalPx = v.focalPx;
    p.tanHalfH = v.tanHalfH;
    p.tanHalfV = v.tanHalfV;
    p.eyeOffset = v.eyeOffset;

    // body <- view = (body <- world: the importer's stabilisation)
    //              * (world <- view: the effect's camera and source rotation).
    // The order is the whole point: the effect reframes INSIDE the stabilised
    // sphere the importer produced, so its rotation is applied first to the
    // view ray and the stabilisation last.
    multiply3(eq.Rout, v.Rout, p.Rout);

    // Pointers travel only with their feature: a disabled seam or warp never
    // hands the kernel an address it has no business reading.
    setup.seamTable = seamOn ? stitch.seamTable : nullptr;
    setup.warpGrid = warpOn ? stitch.warpGrid : nullptr;
    setup.blendSeam = blendSeamOn ? stitch.blendSeam : nullptr;  // [WP-SEAM]

    // ---- final backstop ----------------------------------------------------
    // buildView() already guarantees a finite camera; this repeats the check
    // on the COMPOSED block, the thing a kernel will actually read.
    // The eye's distance behind the sphere centre is capped per projection:
    // the Classic lens's eye offset lives in [0, 1], while DJI's Correction
    // Angle legitimately goes past the sphere (Crystal Ball is 1.8).  Capping
    // both at 1 would refuse every Crystal Ball frame and quietly hand it to
    // the slower equirect path.
    const float eyeOffsetMax = (p.projection == OSV_PROJ_DJI_SPHERE)
                                   ? static_cast<float>(OSV_REFRAME_CORRECTION_VALID_MAX)
                                   : 1.0f;
    const bool cameraFinite = std::isfinite(p.focalPx) && p.focalPx > 0.0f && std::isfinite(p.tanHalfH) &&
                              std::isfinite(p.tanHalfV) && std::isfinite(p.eyeOffset) && p.eyeOffset >= 0.0f &&
                              p.eyeOffset <= eyeOffsetMax;
    if (!cameraFinite || !isRotation(p.Rout)) {
        return refuse(DirectReject::Composed);
    }

    setup.valid = true;
    setup.reject = DirectReject::None;
    setup.viewReject = SetupReject::None;
    return setup;
}

// ---------------------------------------------------------------------------
//  Plane descriptors
// ---------------------------------------------------------------------------
bool planesMatch(const DirectSetup& setup, const OsvPlane* planes) noexcept {
    if (!planes || !setup.valid) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        const OsvLens& L = setup.params.lens[i];
        // A disabled lens is skipped by the shader before its plane is ever
        // touched, so its descriptor may legitimately be empty.
        if (!L.enabled) {
            continue;
        }
        const OsvPlane& P = planes[i];
        // All three sample pointers must exist; the shader reads each one.
        if (!P.y || !P.u || !P.v) {
            return false;
        }
        // The luma plane must be exactly the frame the lens block describes,
        // or every projected coordinate would land in the wrong place.
        if (P.w != L.width || P.h != L.height) {
            return false;
        }
        // 4:2:0 only: the shader samples chroma at half the luma coordinate.
        if (P.cw != (P.w + 1) / 2 || P.ch != (P.h + 1) / 2) {
            return false;
        }
        // Strides must cover one row (interleaved chroma steps by two).
        const int step = P.chromaInterleaved ? 2 : 1;
        if (P.strideY < P.w || P.strideC < P.cw * step) {
            return false;
        }
        // Interleaved CbCr: Cr is the element right after Cb.
        if (P.chromaInterleaved && P.v != P.u + 1) {
            return false;
        }
        // 10-bit samples live in the low bits (shift 0) or the top bits of a
        // 16-bit word (P010: shift 6); anything outside [0, 15] is garbage.
        if (P.bitShift < 0 || P.bitShift > 15) {
            return false;
        }
        // The shader's plane index is 32-bit int arithmetic; refuse a frame
        // whose last element would overflow it.
        const std::int64_t lumaSpan = static_cast<std::int64_t>(P.h) * P.strideY;
        const std::int64_t chromaSpan = static_cast<std::int64_t>(P.ch) * P.strideC;
        if (lumaSpan >= kMaxPlaneElements || chromaSpan >= kMaxPlaneElements) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
//  The CPU twin
// ---------------------------------------------------------------------------
bool renderDirectPixel(const DirectSetup& setup, const OsvPlane* planes, int x, int y, float out[4]) noexcept {
    if (!out) {
        return false;
    }
    // Transparent black is written first so every failure path leaves a
    // defined value behind.
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (!planesMatch(setup, planes)) {
        return false;
    }
    if (x < 0 || y < 0 || x >= setup.params.outW || y >= setup.params.outH) {
        return false;
    }
    osvShadePixelWS(&setup.params, planes, setup.seamTable, setup.warpGrid, setup.blendSeam, x, y, out);
    return true;
}

bool renderDirectCpu(const DirectSetup& setup, const OsvPlane* planes, const FrameView& dst,
                     ThreadPool* pool) noexcept {
    // Validate everything before a single byte is written: a partially
    // rendered frame reaching the host is worse than an untouched one.
    if (!planesMatch(setup, planes)) {
        return false;
    }
    if (!dst.valid()) {
        return false;
    }
    if (dst.width != setup.params.outW || dst.height != setup.params.outH) {
        return false;
    }
    const std::size_t bpp = bytesPerPixel(dst.layout);
    if (bpp == 0) {
        return false;
    }

    // One row per job.  Each row reads only the (const) planes and tables and
    // writes only its own destination row, so rows are independent.  The
    // lambda captures by reference; parallelRows() returns only after every
    // row has run, so nothing it references goes out of scope early.
    const auto renderRow = [&](std::size_t row) noexcept {
        const int y = static_cast<int>(row);
        char* dstRow = static_cast<char*>(dst.rowTopDown(y));
        for (int x = 0; x < dst.width; ++x) {
            float rgba[4];
            osvShadePixelWS(&setup.params, planes, setup.seamTable, setup.warpGrid, setup.blendSeam, x, y, rgba);
            storePixel(dstRow + static_cast<std::ptrdiff_t>(x) * static_cast<std::ptrdiff_t>(bpp), dst.layout, rgba);
        }
    };

    if (pool) {
        // A grain of one row: a stitched row is thousands of shader
        // evaluations, far more than the cost of taking a chunk from the queue.
        const Status status = pool->parallelRows(static_cast<std::size_t>(dst.height), 1, renderRow);
        return status.ok();
    }
    for (int y = 0; y < dst.height; ++y) {
        renderRow(static_cast<std::size_t>(y));
    }
    return true;
}

}  // namespace osv::reframe
