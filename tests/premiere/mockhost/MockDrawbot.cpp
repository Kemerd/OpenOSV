// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MockDrawbot.cpp - the PF custom-UI and DrawBot surface of the mock host.
//
// These suites RECORD rather than render.  A test does not want a bitmap; it
// wants to assert "a crosshair and a roll ring were drawn", and the cheapest
// honest way to get that is to write down every call in order and let the
// test read the list back.
//
// What is recorded:
//
//   * every path vertex, tagged with the path it belongs to, so a test can
//     count the segments of the crosshair and find the arcs of the ring;
//   * every stroke and fill, with the pen or brush colour and width, so a
//     test can tell the dark outline pass from the light ink pass;
//   * every string drawn, in UTF-8, so the Pan/Tilt/Roll/FOV readout can be
//     checked without decoding UTF-16 in the test;
//   * object creation and release, so a leaked pen or path is a test failure
//     rather than something only a profiler would ever notice.
//
// The suites are deliberately permissive about what they accept and strict
// about what they count: a plug-in that calls StrokePath with a released
// path gets kSPNoError and a recorded entry flagged `staleObject`, because
// the point of the mock is to let a test SEE the mistake, not to make the
// plug-in crash in a way the real host might not.
#include "MockHostImpl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace osv::premiere::mock {

namespace {

/// The live mock host's state, or nullptr when no host exists.
///
/// Every suite function starts with this: the suite tables are plain C
/// function pointers with no user data, so the state has to be reached
/// through the singleton, and a call arriving after the host was destroyed
/// must be answered rather than crash.
[[nodiscard]] MockHost::Impl* impl() noexcept {
    MockHost* host = MockHost::current();
    return host ? host->implForSuites() : nullptr;
}

// ---------------------------------------------------------------------------
//  Object identity
//
//  DrawBot hands out opaque pointers.  The mock never allocates a real
//  object; it hands back a small integer cast to the pointer type, offset by
//  a per-kind base so a pen can never be mistaken for a path even if the
//  plug-in mixes them up.  The integer indexes the record table.
// ---------------------------------------------------------------------------
constexpr std::uintptr_t kPenBase = 0x1000u;
constexpr std::uintptr_t kBrushBase = 0x2000u;
constexpr std::uintptr_t kPathBase = 0x3000u;
constexpr std::uintptr_t kFontBase = 0x4000u;
constexpr std::uintptr_t kImageBase = 0x5000u;

/// The one supplier and the one surface the mock serves.  Their identity
/// never changes, so a test can compare against these directly.
constexpr std::uintptr_t kSupplierToken = 0x9001u;
constexpr std::uintptr_t kSurfaceToken = 0x9002u;

/// Pack an index and a kind base into an opaque DrawBot reference.
template <class RefT>
[[nodiscard]] RefT makeRef(std::uintptr_t base, std::size_t index) noexcept {
    return reinterpret_cast<RefT>(base + index + 1u);  // +1: never hand out null.
}

/// Recover the index from a reference, or SIZE_MAX when the reference does
/// not belong to this kind.
[[nodiscard]] std::size_t refIndex(const void* ref, std::uintptr_t base) noexcept {
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(ref);
    if (raw <= base) {
        return static_cast<std::size_t>(-1);
    }
    const std::uintptr_t offset = raw - base - 1u;
    // Each kind owns a 0x1000-wide window; anything past it belongs to a
    // different kind and must not be resolved here.
    if (offset >= 0x1000u) {
        return static_cast<std::size_t>(-1);
    }
    return static_cast<std::size_t>(offset);
}

/// UTF-16 (as DrawBot passes it) -> UTF-8, for the recorded string.
///
/// The overlay only ever draws ASCII, but a surrogate pair or a Latin-1
/// character must still round-trip into something a test can compare, so the
/// full BMP range is encoded properly rather than truncated.
[[nodiscard]] std::string fromDrawbotString(const DRAWBOT_UTF16Char* text) {
    std::string out;
    if (!text) {
        return out;
    }
    for (int i = 0; i < 4096 && text[i] != 0; ++i) {
        const unsigned int c = static_cast<unsigned int>(text[i]);
        if (c < 0x80u) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (c >> 6)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xE0u | (c >> 12)));
            out.push_back(static_cast<char>(0x80u | ((c >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  DRAWBOT Draw Suite
// ---------------------------------------------------------------------------
SPErr drawGetSupplier(DRAWBOT_DrawRef drawRef, DRAWBOT_SupplierRef* outSupplier) {
    MockHost::Impl* p = impl();
    if (!p || !outSupplier) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    // A drawing reference the host never handed out is a genuine plug-in
    // bug; answering with an error is what a real supplier would do.
    if (drawRef != p->drawbot.drawRef) {
        return kSPBadParameterError;
    }
    *outSupplier = reinterpret_cast<DRAWBOT_SupplierRef>(kSupplierToken);
    return kSPNoError;
}

SPErr drawGetSurface(DRAWBOT_DrawRef drawRef, DRAWBOT_SurfaceRef* outSurface) {
    MockHost::Impl* p = impl();
    if (!p || !outSurface) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (drawRef != p->drawbot.drawRef) {
        return kSPBadParameterError;
    }
    *outSurface = reinterpret_cast<DRAWBOT_SurfaceRef>(kSurfaceToken);
    return kSPNoError;
}

// ---------------------------------------------------------------------------
//  DRAWBOT Supplier Suite
// ---------------------------------------------------------------------------
SPErr supplierNewPen(DRAWBOT_SupplierRef supplier, const DRAWBOT_ColorRGBA* colour, float size,
                     DRAWBOT_PenRef* outPen) {
    MockHost::Impl* p = impl();
    if (!p || !outPen || !colour) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (reinterpret_cast<std::uintptr_t>(supplier) != kSupplierToken) {
        return kSPBadParameterError;
    }
    // The mock can be told to refuse, so a test can prove the plug-in
    // degrades gracefully instead of dereferencing a null pen.
    if (p->drawbot.failNewPen) {
        return kSPOutOfMemoryError;
    }
    DrawbotPenRecord rec;
    rec.colour = *colour;
    rec.width = size;
    rec.live = true;
    p->drawbot.pens.push_back(rec);
    *outPen = makeRef<DRAWBOT_PenRef>(kPenBase, p->drawbot.pens.size() - 1u);
    return kSPNoError;
}

SPErr supplierNewBrush(DRAWBOT_SupplierRef supplier, const DRAWBOT_ColorRGBA* colour, DRAWBOT_BrushRef* outBrush) {
    MockHost::Impl* p = impl();
    if (!p || !outBrush || !colour) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (reinterpret_cast<std::uintptr_t>(supplier) != kSupplierToken) {
        return kSPBadParameterError;
    }
    DrawbotBrushRecord rec;
    rec.colour = *colour;
    rec.live = true;
    p->drawbot.brushes.push_back(rec);
    *outBrush = makeRef<DRAWBOT_BrushRef>(kBrushBase, p->drawbot.brushes.size() - 1u);
    return kSPNoError;
}

SPErr supplierSupportsText(DRAWBOT_SupplierRef supplier, DRAWBOT_Boolean* outSupports) {
    MockHost::Impl* p = impl();
    if (!p || !outSupports) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (reinterpret_cast<std::uintptr_t>(supplier) != kSupplierToken) {
        return kSPBadParameterError;
    }
    // Configurable: a real supplier may genuinely have no text, and the
    // overlay must still draw its graphics in that case.
    *outSupports = p->drawbot.supportsText ? 1 : 0;
    return kSPNoError;
}

SPErr supplierGetDefaultFontSize(DRAWBOT_SupplierRef supplier, float* outSize) {
    MockHost::Impl* p = impl();
    if (!p || !outSize) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (reinterpret_cast<std::uintptr_t>(supplier) != kSupplierToken) {
        return kSPBadParameterError;
    }
    *outSize = p->drawbot.defaultFontSize;
    return kSPNoError;
}

SPErr supplierNewDefaultFont(DRAWBOT_SupplierRef supplier, float size, DRAWBOT_FontRef* outFont) {
    MockHost::Impl* p = impl();
    if (!p || !outFont) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (reinterpret_cast<std::uintptr_t>(supplier) != kSupplierToken) {
        return kSPBadParameterError;
    }
    if (!p->drawbot.supportsText) {
        return kSPUnimplementedError;
    }
    DrawbotFontRecord rec;
    rec.size = size;
    rec.live = true;
    p->drawbot.fonts.push_back(rec);
    *outFont = makeRef<DRAWBOT_FontRef>(kFontBase, p->drawbot.fonts.size() - 1u);
    return kSPNoError;
}

SPErr supplierNewImageFromBuffer(DRAWBOT_SupplierRef, int, int, int, DRAWBOT_PixelLayout, const void*,
                                 DRAWBOT_ImageRef* outImage) {
    // The overlay draws no images.  The entry exists so the suite table is
    // complete (a null member would be called blindly by a future user) and
    // it hands back a token that ReleaseObject accepts.
    MockHost::Impl* p = impl();
    if (!p || !outImage) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.imageCount++;
    *outImage = makeRef<DRAWBOT_ImageRef>(kImageBase, p->drawbot.imageCount - 1u);
    return kSPNoError;
}

SPErr supplierNewPath(DRAWBOT_SupplierRef supplier, DRAWBOT_PathRef* outPath) {
    MockHost::Impl* p = impl();
    if (!p || !outPath) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (reinterpret_cast<std::uintptr_t>(supplier) != kSupplierToken) {
        return kSPBadParameterError;
    }
    if (p->drawbot.failNewPath) {
        return kSPOutOfMemoryError;
    }
    DrawbotPathRecord rec;
    rec.live = true;
    p->drawbot.paths.push_back(rec);
    *outPath = makeRef<DRAWBOT_PathRef>(kPathBase, p->drawbot.paths.size() - 1u);
    return kSPNoError;
}

SPErr supplierSupportsBgra(DRAWBOT_SupplierRef, DRAWBOT_Boolean* out) {
    if (!out) {
        return kSPBadParameterError;
    }
    *out = 1;
    return kSPNoError;
}

SPErr supplierPrefersBgra(DRAWBOT_SupplierRef, DRAWBOT_Boolean* out) {
    if (!out) {
        return kSPBadParameterError;
    }
    *out = 1;
    return kSPNoError;
}

SPErr supplierSupportsArgb(DRAWBOT_SupplierRef, DRAWBOT_Boolean* out) {
    if (!out) {
        return kSPBadParameterError;
    }
    *out = 1;
    return kSPNoError;
}

SPErr supplierPrefersArgb(DRAWBOT_SupplierRef, DRAWBOT_Boolean* out) {
    if (!out) {
        return kSPBadParameterError;
    }
    *out = 0;
    return kSPNoError;
}

SPErr supplierRetainObject(DRAWBOT_ObjectRef obj) {
    MockHost::Impl* p = impl();
    if (!p || !obj) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.retainCount++;
    return kSPNoError;
}

SPErr supplierReleaseObject(DRAWBOT_ObjectRef obj) {
    MockHost::Impl* p = impl();
    if (!p || !obj) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.releaseCount++;

    // Mark the specific object dead so a leak test can compare live counts
    // rather than only totals.  An unknown reference is counted but not
    // resolved, which is exactly how a double release shows up.
    std::size_t index = refIndex(obj, kPenBase);
    if (index < p->drawbot.pens.size()) {
        p->drawbot.pens[index].live = false;
        return kSPNoError;
    }
    index = refIndex(obj, kBrushBase);
    if (index < p->drawbot.brushes.size()) {
        p->drawbot.brushes[index].live = false;
        return kSPNoError;
    }
    index = refIndex(obj, kPathBase);
    if (index < p->drawbot.paths.size()) {
        p->drawbot.paths[index].live = false;
        return kSPNoError;
    }
    index = refIndex(obj, kFontBase);
    if (index < p->drawbot.fonts.size()) {
        p->drawbot.fonts[index].live = false;
        return kSPNoError;
    }
    return kSPNoError;
}

// ---------------------------------------------------------------------------
//  DRAWBOT Path Suite
//
//  Vertices are appended to the path's own list, so a test can ask "how many
//  segments did the crosshair have" without untangling them from the ring.
// ---------------------------------------------------------------------------

/// The record behind a path reference, or nullptr.
[[nodiscard]] DrawbotPathRecord* pathRecord(MockHost::Impl* p, DRAWBOT_PathRef ref) noexcept {
    if (!p) {
        return nullptr;
    }
    const std::size_t index = refIndex(ref, kPathBase);
    if (index >= p->drawbot.paths.size()) {
        return nullptr;
    }
    return &p->drawbot.paths[index];
}

SPErr pathMoveTo(DRAWBOT_PathRef ref, float x, float y) {
    MockHost::Impl* p = impl();
    if (!p) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    DrawbotPathRecord* rec = pathRecord(p, ref);
    if (!rec) {
        return kSPBadParameterError;
    }
    rec->vertices.push_back(DrawbotVertex{DrawbotVertexKind::MoveTo, x, y, 0.0f, 0.0f, 0.0f});
    return kSPNoError;
}

SPErr pathLineTo(DRAWBOT_PathRef ref, float x, float y) {
    MockHost::Impl* p = impl();
    if (!p) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    DrawbotPathRecord* rec = pathRecord(p, ref);
    if (!rec) {
        return kSPBadParameterError;
    }
    rec->vertices.push_back(DrawbotVertex{DrawbotVertexKind::LineTo, x, y, 0.0f, 0.0f, 0.0f});
    return kSPNoError;
}

SPErr pathBezierTo(DRAWBOT_PathRef ref, const DRAWBOT_PointF32*, const DRAWBOT_PointF32*,
                   const DRAWBOT_PointF32* pt3) {
    MockHost::Impl* p = impl();
    if (!p || !pt3) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    DrawbotPathRecord* rec = pathRecord(p, ref);
    if (!rec) {
        return kSPBadParameterError;
    }
    // Only the end point is recorded: the overlay draws no beziers, and a
    // test that starts to care can extend the record then.
    rec->vertices.push_back(DrawbotVertex{DrawbotVertexKind::BezierTo, pt3->x, pt3->y, 0.0f, 0.0f, 0.0f});
    return kSPNoError;
}

SPErr pathAddRect(DRAWBOT_PathRef ref, const DRAWBOT_RectF32* rect) {
    MockHost::Impl* p = impl();
    if (!p || !rect) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    DrawbotPathRecord* rec = pathRecord(p, ref);
    if (!rec) {
        return kSPBadParameterError;
    }
    // x/y carry the origin, radius/start/sweep carry width and height, so
    // one vertex kind describes the whole rectangle.
    rec->vertices.push_back(DrawbotVertex{DrawbotVertexKind::Rect, rect->left, rect->top, rect->width, rect->height, 0.0f});
    return kSPNoError;
}

SPErr pathAddArc(DRAWBOT_PathRef ref, const DRAWBOT_PointF32* centre, float radius, float startAngle, float sweep) {
    MockHost::Impl* p = impl();
    if (!p || !centre) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    DrawbotPathRecord* rec = pathRecord(p, ref);
    if (!rec) {
        return kSPBadParameterError;
    }
    rec->vertices.push_back(DrawbotVertex{DrawbotVertexKind::Arc, centre->x, centre->y, radius, startAngle, sweep});
    return kSPNoError;
}

SPErr pathClose(DRAWBOT_PathRef ref) {
    MockHost::Impl* p = impl();
    if (!p) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    DrawbotPathRecord* rec = pathRecord(p, ref);
    if (!rec) {
        return kSPBadParameterError;
    }
    rec->vertices.push_back(DrawbotVertex{DrawbotVertexKind::Close, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    return kSPNoError;
}

// ---------------------------------------------------------------------------
//  DRAWBOT Surface Suite
// ---------------------------------------------------------------------------
SPErr surfacePushState(DRAWBOT_SurfaceRef surface) {
    MockHost::Impl* p = impl();
    if (!p || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    // The transform stack is modelled for real, because the overlay draws
    // its shadow pass by translating the surface and a test must be able to
    // prove the shadow landed one pixel away from the ink.
    p->drawbot.transformStack.push_back(p->drawbot.transform);
    p->drawbot.pushCount++;
    return kSPNoError;
}

SPErr surfacePopState(DRAWBOT_SurfaceRef surface) {
    MockHost::Impl* p = impl();
    if (!p || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (p->drawbot.transformStack.empty()) {
        // An unbalanced pop is recorded rather than crashed on, so a test
        // can assert it never happens.
        p->drawbot.unbalancedPops++;
        return kSPBadParameterError;
    }
    p->drawbot.transform = p->drawbot.transformStack.back();
    p->drawbot.transformStack.pop_back();
    p->drawbot.popCount++;
    return kSPNoError;
}

SPErr surfaceTransform(DRAWBOT_SurfaceRef surface, const DRAWBOT_MatrixF32* matrix) {
    MockHost::Impl* p = impl();
    if (!p || !matrix || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    // Only the translation is tracked: that is all the overlay uses, and a
    // full 3x3 concatenation would be state a test cannot check anyway.
    p->drawbot.transform.dx += matrix->mat[2][0];
    p->drawbot.transform.dy += matrix->mat[2][1];
    return kSPNoError;
}

SPErr surfacePaintRect(DRAWBOT_SurfaceRef surface, const DRAWBOT_ColorRGBA* colour, const DRAWBOT_RectF32* rect) {
    MockHost::Impl* p = impl();
    if (!p || !colour || !rect || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    // Recorded because the design brief forbids filled overlays: a test can
    // assert this count stays at zero.
    DrawbotPaintRect entry;
    entry.colour = *colour;
    entry.rect = *rect;
    p->drawbot.paintRects.push_back(entry);
    return kSPNoError;
}

SPErr surfaceFillPath(DRAWBOT_SurfaceRef surface, DRAWBOT_BrushRef brush, DRAWBOT_PathRef path, DRAWBOT_FillType) {
    MockHost::Impl* p = impl();
    if (!p || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);

    DrawbotDrawOp op;
    op.kind = DrawbotOpKind::FillPath;
    op.pathIndex = refIndex(path, kPathBase);
    op.dx = p->drawbot.transform.dx;
    op.dy = p->drawbot.transform.dy;

    const std::size_t brushIndex = refIndex(brush, kBrushBase);
    if (brushIndex < p->drawbot.brushes.size()) {
        op.colour = p->drawbot.brushes[brushIndex].colour;
        op.staleObject = !p->drawbot.brushes[brushIndex].live;
    }
    p->drawbot.ops.push_back(op);
    return kSPNoError;
}

SPErr surfaceStrokePath(DRAWBOT_SurfaceRef surface, DRAWBOT_PenRef pen, DRAWBOT_PathRef path) {
    MockHost::Impl* p = impl();
    if (!p || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);

    DrawbotDrawOp op;
    op.kind = DrawbotOpKind::StrokePath;
    op.pathIndex = refIndex(path, kPathBase);
    // The translation in force at the time of the stroke is captured with
    // it, which is how a test distinguishes the shadow pass (offset by one
    // pixel) from the ink pass (no offset).
    op.dx = p->drawbot.transform.dx;
    op.dy = p->drawbot.transform.dy;

    const std::size_t penIndex = refIndex(pen, kPenBase);
    if (penIndex < p->drawbot.pens.size()) {
        op.colour = p->drawbot.pens[penIndex].colour;
        op.width = p->drawbot.pens[penIndex].width;
        op.staleObject = !p->drawbot.pens[penIndex].live;
    }
    p->drawbot.ops.push_back(op);
    return kSPNoError;
}

SPErr surfaceClip(DRAWBOT_SurfaceRef, DRAWBOT_SupplierRef, const DRAWBOT_Rect32* rect) {
    MockHost::Impl* p = impl();
    if (!p || !rect) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.clipBounds = *rect;
    return kSPNoError;
}

SPErr surfaceGetClipBounds(DRAWBOT_SurfaceRef, DRAWBOT_Rect32* outRect) {
    MockHost::Impl* p = impl();
    if (!p || !outRect) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    *outRect = p->drawbot.clipBounds;
    return kSPNoError;
}

SPErr surfaceIsWithinClipBounds(DRAWBOT_SurfaceRef, const DRAWBOT_Rect32* rect, DRAWBOT_Boolean* outWithin) {
    MockHost::Impl* p = impl();
    if (!p || !rect || !outWithin) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const DRAWBOT_Rect32& clip = p->drawbot.clipBounds;
    const bool within = rect->left >= clip.left && rect->top >= clip.top &&
                        rect->left + rect->width <= clip.left + clip.width &&
                        rect->top + rect->height <= clip.top + clip.height;
    *outWithin = within ? 1 : 0;
    return kSPNoError;
}

SPErr surfaceDrawString(DRAWBOT_SurfaceRef surface, DRAWBOT_BrushRef brush, DRAWBOT_FontRef font,
                        const DRAWBOT_UTF16Char* text, const DRAWBOT_PointF32* origin, DRAWBOT_TextAlignment,
                        DRAWBOT_TextTruncation, float) {
    MockHost::Impl* p = impl();
    if (!p || !text || !origin || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);

    DrawbotStringOp op;
    op.text = fromDrawbotString(text);
    op.x = origin->x;
    op.y = origin->y;

    const std::size_t brushIndex = refIndex(brush, kBrushBase);
    if (brushIndex < p->drawbot.brushes.size()) {
        op.colour = p->drawbot.brushes[brushIndex].colour;
    }
    const std::size_t fontIndex = refIndex(font, kFontBase);
    if (fontIndex < p->drawbot.fonts.size()) {
        op.fontSize = p->drawbot.fonts[fontIndex].size;
    }
    p->drawbot.strings.push_back(op);
    return kSPNoError;
}

SPErr surfaceDrawImage(DRAWBOT_SurfaceRef, DRAWBOT_ImageRef, const DRAWBOT_PointF32*, float) {
    MockHost::Impl* p = impl();
    if (!p) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.drawImageCount++;
    return kSPNoError;
}

SPErr surfaceSetInterpolation(DRAWBOT_SurfaceRef, DRAWBOT_InterpolationPolicy policy) {
    MockHost::Impl* p = impl();
    if (!p) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.interpolation = policy;
    return kSPNoError;
}

SPErr surfaceGetInterpolation(DRAWBOT_SurfaceRef, DRAWBOT_InterpolationPolicy* outPolicy) {
    MockHost::Impl* p = impl();
    if (!p || !outPolicy) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    *outPolicy = p->drawbot.interpolation;
    return kSPNoError;
}

SPErr surfaceSetAntiAlias(DRAWBOT_SurfaceRef, DRAWBOT_AntiAliasPolicy policy) {
    MockHost::Impl* p = impl();
    if (!p) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.antiAlias = policy;
    return kSPNoError;
}

SPErr surfaceGetAntiAlias(DRAWBOT_SurfaceRef, DRAWBOT_AntiAliasPolicy* outPolicy) {
    MockHost::Impl* p = impl();
    if (!p || !outPolicy) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    *outPolicy = p->drawbot.antiAlias;
    return kSPNoError;
}

SPErr surfaceFlush(DRAWBOT_SurfaceRef surface) {
    MockHost::Impl* p = impl();
    if (!p || reinterpret_cast<std::uintptr_t>(surface) != kSurfaceToken) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->drawbot.flushCount++;
    return kSPNoError;
}

SPErr surfaceGetScale(DRAWBOT_SurfaceRef, float* outScale) {
    if (!outScale) {
        return kSPBadParameterError;
    }
    *outScale = 1.0f;
    return kSPNoError;
}

// ---------------------------------------------------------------------------
//  DRAWBOT Pen / Image suites (one member each)
// ---------------------------------------------------------------------------
SPErr penSetDashPattern(DRAWBOT_PenRef pen, const float*, int patternSize) {
    MockHost::Impl* p = impl();
    if (!p) {
        return kSPBadParameterError;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const std::size_t index = refIndex(pen, kPenBase);
    if (index >= p->drawbot.pens.size()) {
        return kSPBadParameterError;
    }
    p->drawbot.pens[index].dashSegments = patternSize;
    return kSPNoError;
}

SPErr imageSetScaleFactor(DRAWBOT_ImageRef, float) { return kSPNoError; }

// ---------------------------------------------------------------------------
//  PF Effect Custom UI Suite
// ---------------------------------------------------------------------------
PF_Err customUiGetDrawingReference(const PF_ContextH contextH, DRAWBOT_DrawRef* outRef) {
    MockHost::Impl* p = impl();
    if (!p || !outRef) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!contextH) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // A test can switch this off to prove the plug-in survives a host that
    // has no drawing reference for the context - the "null DrawBot" case.
    if (!p->drawbot.provideDrawRef) {
        *outRef = nullptr;
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    *outRef = p->drawbot.drawRef;
    return PF_Err_NONE;
}

PF_Err customUiGetContextAsyncManager(PF_InData*, PF_EventExtra*, PF_AsyncManagerP* outManager) {
    // The overlay never asks for async rendering; the member exists so the
    // table is complete.
    if (outManager) {
        *outManager = nullptr;
    }
    return PF_Err_NONE;
}

}  // namespace

// -----------------------------------------------------------------------------
//  MockHost API backed by this file
// -----------------------------------------------------------------------------
void MockHost::setDrawbotSupportsText(bool supports) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->drawbot.supportsText = supports;
}

void MockHost::setDrawbotProvidesDrawRef(bool provides) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->drawbot.provideDrawRef = provides;
}

void MockHost::setDrawbotFailures(bool failNewPen, bool failNewPath) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->drawbot.failNewPen = failNewPen;
    m_impl->drawbot.failNewPath = failNewPath;
}

void MockHost::clearDrawbotRecord() {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    DrawbotState& d = m_impl->drawbot;
    d.pens.clear();
    d.brushes.clear();
    d.paths.clear();
    d.fonts.clear();
    d.ops.clear();
    d.strings.clear();
    d.paintRects.clear();
    d.transformStack.clear();
    d.transform = DrawbotTransform{};
    d.retainCount = 0;
    d.releaseCount = 0;
    d.pushCount = 0;
    d.popCount = 0;
    d.unbalancedPops = 0;
    d.flushCount = 0;
    d.drawImageCount = 0;
    d.imageCount = 0;
}

DrawbotRecord MockHost::drawbotRecord() const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const DrawbotState& d = m_impl->drawbot;

    DrawbotRecord out;
    out.paths = d.paths;
    out.ops = d.ops;
    out.strings = d.strings;
    out.paintRects = d.paintRects;
    out.pens = d.pens;
    out.brushes = d.brushes;
    out.retainCount = d.retainCount;
    out.releaseCount = d.releaseCount;
    out.pushCount = d.pushCount;
    out.popCount = d.popCount;
    out.unbalancedPops = d.unbalancedPops;
    out.flushCount = d.flushCount;
    out.drawImageCount = d.drawImageCount;
    out.antiAlias = d.antiAlias;

    // Objects created but never released.  Counted here rather than in the
    // test so every test that looks at the record gets the same answer.
    out.liveObjects = 0;
    for (const DrawbotPenRecord& pen : d.pens) {
        if (pen.live) {
            ++out.liveObjects;
        }
    }
    for (const DrawbotBrushRecord& brush : d.brushes) {
        if (brush.live) {
            ++out.liveObjects;
        }
    }
    for (const DrawbotPathRecord& path : d.paths) {
        if (path.live) {
            ++out.liveObjects;
        }
    }
    for (const DrawbotFontRecord& font : d.fonts) {
        if (font.live) {
            ++out.liveObjects;
        }
    }
    return out;
}

PF_ContextH MockHost::makeCustomUiContext(PF_WindowType windowType) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    // PF_Context is Adobe's struct; the mock fills one and hands back a
    // handle to a pointer to it, which is what PF_ContextH is.
    std::memset(&m_impl->drawbot.context, 0, sizeof(m_impl->drawbot.context));
    m_impl->drawbot.context.magic = PF_CONTEXT_MAGIC;
    m_impl->drawbot.context.w_type = windowType;
    m_impl->drawbot.contextPtr = &m_impl->drawbot.context;
    return reinterpret_cast<PF_ContextH>(&m_impl->drawbot.contextPtr);
}

// -----------------------------------------------------------------------------
//  Registration
// -----------------------------------------------------------------------------
void installDrawbotSuites(MockHost::Impl& p) {
    // A stable, non-null token for the drawing reference.  It is never
    // dereferenced - only compared - so any distinctive value will do.
    p.drawbot.drawRef = reinterpret_cast<DRAWBOT_DrawRef>(static_cast<std::uintptr_t>(0x0DEC0DEBu));

    // Clip bounds default to a generous frame so IsWithinClipBounds answers
    // sensibly before a test sets anything.
    p.drawbot.clipBounds = DRAWBOT_Rect32{0, 0, 1 << 16, 1 << 16};

    p.drawbotDraw.GetSupplier = &drawGetSupplier;
    p.drawbotDraw.GetSurface = &drawGetSurface;
    p.registerSuite(kDRAWBOT_DrawSuite, kDRAWBOT_DrawSuite_VersionCurrent, &p.drawbotDraw);

    p.drawbotSupplier.NewPen = &supplierNewPen;
    p.drawbotSupplier.NewBrush = &supplierNewBrush;
    p.drawbotSupplier.SupportsText = &supplierSupportsText;
    p.drawbotSupplier.GetDefaultFontSize = &supplierGetDefaultFontSize;
    p.drawbotSupplier.NewDefaultFont = &supplierNewDefaultFont;
    p.drawbotSupplier.NewImageFromBuffer = &supplierNewImageFromBuffer;
    p.drawbotSupplier.NewPath = &supplierNewPath;
    p.drawbotSupplier.SupportsPixelLayoutBGRA = &supplierSupportsBgra;
    p.drawbotSupplier.PrefersPixelLayoutBGRA = &supplierPrefersBgra;
    p.drawbotSupplier.SupportsPixelLayoutARGB = &supplierSupportsArgb;
    p.drawbotSupplier.PrefersPixelLayoutARGB = &supplierPrefersArgb;
    p.drawbotSupplier.RetainObject = &supplierRetainObject;
    p.drawbotSupplier.ReleaseObject = &supplierReleaseObject;
    p.registerSuite(kDRAWBOT_SupplierSuite, kDRAWBOT_SupplierSuite_VersionCurrent, &p.drawbotSupplier);

    p.drawbotSurface.PushStateStack = &surfacePushState;
    p.drawbotSurface.PopStateStack = &surfacePopState;
    p.drawbotSurface.PaintRect = &surfacePaintRect;
    p.drawbotSurface.FillPath = &surfaceFillPath;
    p.drawbotSurface.StrokePath = &surfaceStrokePath;
    p.drawbotSurface.Clip = &surfaceClip;
    p.drawbotSurface.GetClipBounds = &surfaceGetClipBounds;
    p.drawbotSurface.IsWithinClipBounds = &surfaceIsWithinClipBounds;
    p.drawbotSurface.Transform = &surfaceTransform;
    p.drawbotSurface.DrawString = &surfaceDrawString;
    p.drawbotSurface.DrawImage = &surfaceDrawImage;
    p.drawbotSurface.SetInterpolationPolicy = &surfaceSetInterpolation;
    p.drawbotSurface.GetInterpolationPolicy = &surfaceGetInterpolation;
    p.drawbotSurface.SetAntiAliasPolicy = &surfaceSetAntiAlias;
    p.drawbotSurface.GetAntiAliasPolicy = &surfaceGetAntiAlias;
    p.drawbotSurface.Flush = &surfaceFlush;
    p.drawbotSurface.GetTransformToScreenScale = &surfaceGetScale;
    // v1 and v2 are the same table (DrawbotSuite.h:271 "typedef
    // DRAWBOT_SurfaceSuite2 DRAWBOT_SurfaceSuite1"), so both answer.
    p.registerSuite(kDRAWBOT_SurfaceSuite, kDRAWBOT_SurfaceSuite_Version1, &p.drawbotSurface);
    p.registerSuite(kDRAWBOT_SurfaceSuite, kDRAWBOT_SurfaceSuite_Version2, &p.drawbotSurface);

    p.drawbotPath.MoveTo = &pathMoveTo;
    p.drawbotPath.LineTo = &pathLineTo;
    p.drawbotPath.BezierTo = &pathBezierTo;
    p.drawbotPath.AddRect = &pathAddRect;
    p.drawbotPath.AddArc = &pathAddArc;
    p.drawbotPath.Close = &pathClose;
    p.registerSuite(kDRAWBOT_PathSuite, kDRAWBOT_PathSuite_VersionCurrent, &p.drawbotPath);

    p.drawbotPen.SetDashPattern = &penSetDashPattern;
    p.registerSuite(kDRAWBOT_PenSuite, kDRAWBOT_PenSuite_VersionCurrent, &p.drawbotPen);

    p.drawbotImage.SetScaleFactor = &imageSetScaleFactor;
    p.registerSuite(kDRAWBOT_ImageSuite, kDRAWBOT_ImageSuite_VersionCurrent, &p.drawbotImage);

    p.pfCustomUi.PF_GetDrawingReference = &customUiGetDrawingReference;
    p.pfCustomUi.PF_GetContextAsyncManager = &customUiGetContextAsyncManager;
    p.registerSuite(kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion2, &p.pfCustomUi);
}

}  // namespace osv::premiere::mock
