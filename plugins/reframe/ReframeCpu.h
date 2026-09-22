// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeCpu: the geometry builder and the CPU renderer of the reframe
// effect, shared by PF_Cmd_RENDER and by the tests.
//
// The maths itself is NOT here.  osvReframeEquirectPixel() in
// include/osv/render/osv_kernel.h is the one per-pixel function, written in
// the dialect every backend can compile, and the GPU kernel calls exactly
// the same function.  What this header owns is everything around it:
//
//   * turning a resolved Settings block plus the frame geometry into the
//     OsvReframeParams / OsvRgbaSource pair the kernel reads (buildParams);
//   * walking the output frame on the library ThreadPool and storing the
//     result in whichever Premiere layout the host handed us (renderCpu).
//
// Keeping both in one place is what makes the CPU / GPU parity test
// meaningful: the GPU path reuses buildParams() verbatim and only replaces
// the pixel loop.
#pragma once

#include "ReframeParams.h"

#include "osv/core/ThreadPool.h"
#include "osv/render/osv_kernel.h"

#include <cstdint>
#include <vector>

namespace osv::reframe {

/// Pixel layouts the CPU renderer can read and write.  All of them are BGRA
/// (Premiere channel order) and differ only in sample type.
///
/// The enumerator VALUES are load-bearing for nothing outside this header -
/// no project file and no host structure stores them - but `Bgra16u` is
/// appended at the end rather than inserted next to the other integer layout
/// so that a stale object file compiled against the old header cannot
/// silently reinterpret `Bgra8u` as something else.
enum class PixelLayout : int {
    Bgra32f = 0,  ///< PrPixelFormat_BGRA_4444_32f / _32f_Linear / GPU_BGRA_4444_32f: four floats.
    Bgra16f = 1,  ///< PrPixelFormat_GPU_BGRA_4444_16f: four IEEE binary16.
    Bgra8u = 2,   ///< PrPixelFormat_BGRA_4444_8u: four bytes, 0..255.
    Bgra16u = 3,  ///< PrPixelFormat_BGRA_4444_16u: four uint16, 0..32768 (see kBgra16uWhite).
};

/// The code Premiere's 16-bit-integer formats use for white.
///
/// This is 32768, NOT 65535, and getting it wrong is a 100%-scale error on
/// every pixel rather than a subtle one.  The Premiere SDK guide, section
/// 5.4.2 "Byte Order", states it in as many words:
///
///     "8-bit and 16-bit BGRA formats do not contain super whites or super
///      blacks.  The 16-bit formats use channels that go from black at 0 to
///      white at 32768, like After Effects and Photoshop 16-bit formats."
///
/// So the representable range is 0..32768 inclusive - 32769 distinct codes
/// in a container that can hold 65536 - and codes above 32768 are simply
/// out-of-gamut rather than illegal.  We clamp on the way out (a 16-bit
/// host world is a display-referred integer buffer, and writing 40000 there
/// would read as a wildly over-bright pixel on a host that assumes the
/// documented scale) and do NOT clamp on the way in, so an input that does
/// carry an over-range code is resampled faithfully.
constexpr float kBgra16uWhite = 32768.0f;

/// Bytes one pixel of a layout occupies.
[[nodiscard]] constexpr std::size_t bytesPerPixel(PixelLayout layout) noexcept {
    switch (layout) {
        case PixelLayout::Bgra32f:
            return 16;
        case PixelLayout::Bgra16f:
            return 8;
        case PixelLayout::Bgra8u:
            return 4;
        case PixelLayout::Bgra16u:
            return 8;
    }
    return 0;
}

/// A frame as the host describes it: a base address, a signed row pitch and
/// the sample layout.  `topDown` says whether `base` points at the TOP row
/// (GPU frames and AE effect worlds) or at the BOTTOM row (Premiere's
/// uncompressed host formats).  A negative `rowBytes` is legal and means the
/// rows march backwards in memory from `base`, so the two flags are
/// independent and both must be honoured.
struct FrameView {
    void* base = nullptr;
    std::int32_t rowBytes = 0;
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::Bgra32f;
    bool topDown = true;

    /// Cheap sanity check: a usable buffer of a plausible size whose pitch
    /// can hold one row.
    [[nodiscard]] bool valid() const noexcept;

    /// Address of image row `y` counted from the TOP, honouring topDown and
    /// the sign of rowBytes.  The caller must have checked valid() and
    /// 0 <= y < height.
    [[nodiscard]] void* rowTopDown(int y) const noexcept;
    [[nodiscard]] const void* constRowTopDown(int y) const noexcept;
};

/// Read-only variant, so a const source cannot be handed to a writer by
/// accident.  Implicitly convertible from FrameView.
struct ConstFrameView {
    const void* base = nullptr;
    std::int32_t rowBytes = 0;
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::Bgra32f;
    bool topDown = true;

    ConstFrameView() = default;
    ConstFrameView(const FrameView& v) noexcept  // NOLINT(google-explicit-constructor)
        : base(v.base), rowBytes(v.rowBytes), width(v.width), height(v.height), layout(v.layout), topDown(v.topDown) {}

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const void* constRowTopDown(int y) const noexcept;
};

/// Everything the kernel needs, built once per frame.
/// Why buildParams() refused to build a setup.
///
/// Every `return setup;` before the success path sets one of these.  Without
/// it a rejection logs only the frame sizes, which never say which of the
/// several independent checks fired - and the checks fail for completely
/// different reasons (a bad control value, an unpromoted integer world, a
/// bottom-up frame), so guessing between them costs a debugging session.
enum class SetupReject {
    None = 0,          ///< The setup is valid.
    SourceInvalid,     ///< The input frame view itself was not usable.
    OutputSize,        ///< outW/outH non-positive or beyond kMaxEdge.
    Viewport,          ///< The render rectangle came out empty (a degenerate frame).
    DegenerateCamera,  ///< No usable focal length, even at the default FOV.
    NeedsPromotion,    ///< An 8u/16u source reached us without being promoted.
    RowsBackwards,     ///< Image rows do not run forward in memory.
    SourcePointer,     ///< Null row-0 pointer or a non-positive pitch.
};

/// Human-readable name of a rejection reason, for the log line.
[[nodiscard]] const char* setupRejectName(SetupReject reason) noexcept;

struct KernelSetup {
    OsvReframeParams params{};  ///< Projection, viewport, rotation.
    OsvRgbaSource source{};     ///< Size and sample type of the equirect input.
    const void* sourceRow0 = nullptr;  ///< Address the source descriptor is relative to (its TOP row).
    bool valid = false;         ///< False when the inputs were unusable.
    /// Set on every failure path; `None` exactly when `valid` is true.
    SetupReject reject = SetupReject::SourceInvalid;
};

/// True when a source frame's IMAGE rows run forward in memory, which is the
/// only arrangement the shared sampler can describe.
///
/// `OsvRgbaSource` documents `pitchBytes` as positive with row 0 at the top,
/// and `osvFetchRgba` indexes rows with an UNSIGNED multiply, so a negative
/// stride is not expressible - a negative value would become an enormous
/// positive offset and walk off the allocation.
///
/// Two of the four combinations are fine, because the inversions cancel:
///
///     topDown, rowBytes > 0   image row y at base + y * rowBytes    OK
///     !topDown, rowBytes < 0  image row y at base + y * |rowBytes|  OK
///     topDown, rowBytes < 0   rows run backwards                    NO
///     !topDown, rowBytes > 0  the image is stored upside down       NO
///
/// The two rejected cases need the pixels themselves mirrored vertically,
/// which no pointer arithmetic can do; buildParams() refuses them rather
/// than render an upside-down frame or read out of bounds.  Neither arises
/// in practice: GPU frames are documented top-left with a positive pitch,
/// and the AE effect worlds Premiere hands PF_Cmd_RENDER are top-left too.
[[nodiscard]] bool sourceRowsRunForward(const ConstFrameView& src) noexcept;

/// Build the kernel parameters.
///
/// `settings`      the resolved controls (already smoothed, if smoothing is on);
/// `src`           the equirectangular input frame;
/// `outW`, `outH`  the output frame size;
/// `sequenceSize`  pixel size of the sequence frame, invalid when unknown.
///
/// The camera is always built to cover the WHOLE output frame - there is no
/// letterbox any more - and the eye offset it uses is effectiveEyeOffset(),
/// i.e. the Distortion control raised to the automatic ramp's floor for the
/// chosen field of view.
///
/// Returns an invalid setup (`valid == false`) for any input the kernel
/// cannot be pointed at, including a source that fails
/// sourceRowsRunForward().  On success `sourceRow0` is what must be passed
/// to the kernel as `pixels` and `source.pitchBytes` is positive.
[[nodiscard]] KernelSetup buildParams(const Settings& settings, const ConstFrameView& src, int outW, int outH,
                                      SizePx sequenceSize) noexcept;

/// Render one frame on the CPU.
///
/// Reads `src`, writes `dst` (which may be any of the three layouts and may
/// be the same size or a different one), running rows in parallel on `pool`
/// (nullptr = the calling thread).  Returns false when the setup is invalid;
/// in that case `dst` is left untouched, because a half-written frame is
/// worse than an unmodified one.
///
/// 8-bit output is clamped to [0, 1] and rounded to nearest; 16f output is
/// converted with round-to-nearest-even.  Output pixels outside the viewport
/// are written as transparent black, so the caller never has to clear first.
bool renderCpu(const KernelSetup& setup, const ConstFrameView& src, const FrameView& dst, ThreadPool* pool) noexcept;

/// One pixel, for the tests: the reference answer at (px, py) with no
/// layout conversion at all.  Returns false when the setup is invalid.
bool renderPixel(const KernelSetup& setup, const ConstFrameView& src, int px, int py, float out[4]) noexcept;

// ---------------------------------------------------------------------------
//  Integer input promotion
// ---------------------------------------------------------------------------

/// True when a layout stores integer codes the shared sampler cannot read,
/// so a frame in it has to be promoted to float before it can be sampled.
///
/// This is the ONE predicate both buildParams() and the render path consult,
/// so "which layouts need promoting" is stated once.  Adding a new integer
/// layout and forgetting one of the two call sites is exactly how the
/// original 8-bit path came to reject a format the effect advertised.
[[nodiscard]] constexpr bool layoutNeedsPromotion(PixelLayout layout) noexcept {
    return layout == PixelLayout::Bgra8u || layout == PixelLayout::Bgra16u;
}

/// Promote an integer-coded source frame (Bgra8u or Bgra16u) to a packed
/// Bgra32f one.
///
/// The shared sampler (`osvFetchRgba`) reads float or half only, so an
/// integer-coded source cannot be described by an OsvRgbaSource at all.
/// Rather than DECLINE a pixel format we advertise in GLOBAL_SETUP - which
/// would leave the effect contradicting its own registration and depending
/// on an undocumented host retry - the effect promotes the frame once, here,
/// and renders from the promoted copy.
///
/// Promoting rather than teaching the sampler to read integers is the
/// deliberate choice, and the reason is the sampler's inner loop.  A
/// bilinear fetch touches four pixels and sixteen components; decoding each
/// one would put a divide (or a multiply plus a format branch) inside the
/// hottest loop in the effect, on the GPU as well as the CPU, because the
/// kernel is shared source.  Promotion pays the conversion exactly ONCE per
/// source pixel, up front, in a perfectly parallel row-wise pass, and leaves
/// the kernel byte-for-byte the one that renders a float source.  That also
/// means a 16u render and a 32f render of the same picture go through
/// identical code after the promotion, which is what makes them comparable
/// in a test instead of merely similar.
///
/// `scratch` is resized to width * height * 4 floats and becomes the backing
/// store of the returned view, so it must outlive every use of that view.
/// Codes are normalised by the layout's own white point - 255 for Bgra8u,
/// kBgra16uWhite for Bgra16u - row order is normalised to top-down with a
/// positive pitch, and the promoted view is therefore always one the sampler
/// can describe.
///
/// Returns a view whose `valid()` is false when `src` is unusable or is not
/// an integer-coded frame (see layoutNeedsPromotion); the caller checks that
/// rather than getting a half-filled buffer.  Rows run in parallel on `pool`
/// (nullptr = the calling thread).
[[nodiscard]] ConstFrameView promoteIntegerToFloat(const ConstFrameView& src, std::vector<float>& scratch,
                                                   ThreadPool* pool) noexcept;

}  // namespace osv::reframe
