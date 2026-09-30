// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxTestSupport.h - shared pieces of the OpenOSV.ofx tests: the loaded
// module, OpenFX-layout images in every format a host hands out (Resolve's
// float RGBA, VEGAS's 8-bit / float in RGBA / BGRA), a synthetic panorama,
// and the Premiere effect's own CPU render as the reference every OpenFX
// render is held to.
#pragma once

#include "MockOfxHost.h"

#include "OfxHostImage.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"

#include <cstdint>
#include <string>
#include <vector>

namespace osv::ofxtest {

/// The built module (OSV_OFX_MODULE_PATH), loaded once per process, and one
/// harness per plug-in that has had kOfxActionLoad and kOfxActionDescribe.
struct Fixture {
    LoadedModule module;
    PluginHarness reframe;
    PluginHarness source;
    bool ready = false;

    static Fixture& get();

private:
    Fixture();
};

/// An image laid out the OpenFX way: pixel (x, y) of the host's coordinates
/// (y UP) lives at data() + (y - bounds.y1) * rowBytes + (x - bounds.x1) *
/// pixelBytes().  `negativePitch` stores the rows top-down in memory
/// (rowBytes < 0), `padFloats` adds that many floats of padding to every row
/// - both legal in OpenFX, both handled by the plug-in.
///
/// The default is Resolve's only format, 32-bit float R G B A.  VEGAS's
/// formats are the same struct with another `depth` / `order`: an 8-bit
/// image keeps its four bytes per pixel in `storage` too (one float's worth
/// per pixel), so every image - and the CUDA tests' float-sized uploads of
/// float images - shares one storage type.
struct HostImage {
    OfxRectI bounds{0, 0, 0, 0};
    int rowBytes = 0;
    std::vector<float> storage;
    std::size_t bottomOffsetFloats = 0;  ///< Where the bottom row starts in `storage`.
    osv::ofx::HostDepth depth = osv::ofx::HostDepth::Float;  ///< Bits per channel.
    osv::ofx::HostOrder order = osv::ofx::HostOrder::Rgba;   ///< Channel order in memory.
    /// True: describe() states the order on the image itself
    /// ("OfxImageEffectPropPixelOrder").  False: it says nothing, and the
    /// plug-in must find the order on the clip or assume R G B A.
    bool labelOrder = false;
    /// The pixel depth label describe() writes; empty = the depth's own
    /// (kOfxBitDepthFloat / kOfxBitDepthByte).  For label tests only.
    std::string depthLabel;

    [[nodiscard]] int width() const noexcept { return bounds.x2 - bounds.x1; }
    [[nodiscard]] int height() const noexcept { return bounds.y2 - bounds.y1; }
    /// Bytes per pixel: 16 (float) or 4 (8-bit).
    [[nodiscard]] int pixelBytes() const noexcept { return osv::ofx::bytesPerPixel(depth); }
    [[nodiscard]] float* data() noexcept { return storage.data() + bottomOffsetFloats; }
    [[nodiscard]] const float* data() const noexcept { return storage.data() + bottomOffsetFloats; }
    /// The first byte of pixel (x, y) in any format; null outside the bounds.
    [[nodiscard]] unsigned char* rawPixel(int x, int y) noexcept;
    [[nodiscard]] const unsigned char* rawPixel(int x, int y) const noexcept;
    /// The four floats of pixel (x, y) of a FLOAT image; null outside the
    /// bounds and for an 8-bit image.  The order is the image's own.
    [[nodiscard]] float* pixel(int x, int y) noexcept;
    [[nodiscard]] const float* pixel(int x, int y) const noexcept;

    /// Pixel (x, y) as straight R, G, B, A floats whatever the format (8-bit
    /// codes * 1/255, exactly as the plug-in promotes them).  False, with
    /// `rgba` untouched, outside the bounds.
    bool readRgba(int x, int y, float rgba[4]) const noexcept;
    /// Store straight R, G, B, A floats at (x, y) in the image's format, at
    /// full levels (osv::ofx::storeHostPixel).  False outside the bounds.
    bool writeRgba(int x, int y, const float rgba[4]) noexcept;

    /// Fill an image property set as a host would (data, bounds, pitch,
    /// depth, components, pixel aspect, field - and the pixel order when
    /// `labelOrder` is set).
    void describe(PropertySet& image) noexcept;
    /// Every float of a float image set to `value`.  An 8-bit image has no
    /// code for the float sentinels the tests use (-7), so it gets the
    /// sentinel code kByteSentinel in every byte instead.
    void fill(float value) noexcept;
    /// The code fill() writes into an 8-bit image: odd, mid-grey, and never
    /// what a render of transparent black or of the test panorama's alpha
    /// produces.
    static constexpr std::uint8_t kByteSentinel = 0xA5;
    /// Every byte of the image's storage set to `code`.
    void fillBytes(std::uint8_t code) noexcept;
};

[[nodiscard]] HostImage makeImage(const OfxRectI& bounds, bool negativePitch = false, int padFloats = 0,
                                  osv::ofx::HostDepth depth = osv::ofx::HostDepth::Float,
                                  osv::ofx::HostOrder order = osv::ofx::HostOrder::Rgba);

/// `source`'s picture in another format: every pixel read as RGBA floats and
/// stored in `depth` / `order` (8-bit rounded exactly as the plug-in rounds).
[[nodiscard]] HostImage convertImage(const HostImage& source, osv::ofx::HostDepth depth, osv::ofx::HostOrder order,
                                     bool negativePitch = false, int padFloats = 0);

/// A smooth, asymmetric test panorama (no two directions share a colour, no
/// symmetry that could hide a flip) painted into an OpenFX-layout image.
void paintPanorama(HostImage& image);

/// The Premiere effect's own CPU render of `settings` from `source` (read as
/// the host image it is, in any format), as a float RGBA OpenFX-layout image
/// of `frame`: what Open 360 Reframe in Premiere shows for the same picture
/// and controls.
[[nodiscard]] HostImage referenceRender(const reframe::Settings& settings, const HostImage& source,
                                        const OfxRectI& frame, reframe::SizePx project);

/// Largest absolute difference over `window` of two images (all four
/// channels, read as RGBA floats); +infinity when a pixel is missing from
/// either or is NaN.
[[nodiscard]] double maxDifference(const HostImage& a, const HostImage& b, const OfxRectI& window);

/// Largest difference over `window` between `image` and what the packing
/// rule makes of the full-range float `reference` in `levels`: the reference
/// pixel through osv::ofx::storeHostPixel() into `image`'s format, compared
/// channel by channel in the image's own units - CODES for an 8-bit image,
/// float values for a float one.  +infinity when a pixel is missing or NaN.
[[nodiscard]] double maxPackedDifference(const HostImage& image, const HostImage& reference, const OfxRectI& window,
                                         osv::ofx::OutputLevels levels = osv::ofx::OutputLevels::Full);

/// PSNR of `a` against `b` over `window` (peak 1.0).
[[nodiscard]] double psnr(const HostImage& a, const HostImage& b, const OfxRectI& window);

/// The Settings a FRESH instance renders: every control at its default,
/// exactly as the Premiere effect reads a fresh instance (Lens: DJI).
[[nodiscard]] reframe::Settings defaultSettings();

/// Hand `image` to `clip` for every time.
void provideImage(Clip& clip, HostImage& image);

/// True when the NVIDIA driver's nvcuda.dll can be loaded, so a driver-API
/// call cannot raise a delay-load exception (the test executable
/// delay-loads it).  Always false off Windows.
[[nodiscard]] bool cudaDriverLoadable();

}  // namespace osv::ofxtest
