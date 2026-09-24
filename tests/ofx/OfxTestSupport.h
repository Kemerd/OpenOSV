// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxTestSupport.h - shared pieces of the OpenOSV.ofx tests: the loaded
// module, OpenFX-layout images, a synthetic panorama, and the Premiere
// effect's own CPU render as the reference every OpenFX render is held to.
#pragma once

#include "MockOfxHost.h"

#include "ReframeCpu.h"
#include "ReframeParams.h"

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

/// A 32-bit float RGBA image laid out the OpenFX way: pixel (x, y) of the
/// host's coordinates (y UP) lives at data() + (y - bounds.y1) * rowBytes
/// + (x - bounds.x1) * 16.  `negativePitch` stores the rows top-down in
/// memory (rowBytes < 0), `padFloats` adds that many floats of padding to
/// every row - both legal in OpenFX, both handled by the plug-in.
struct HostImage {
    OfxRectI bounds{0, 0, 0, 0};
    int rowBytes = 0;
    std::vector<float> storage;
    std::size_t bottomOffsetFloats = 0;  ///< Where the bottom row starts in `storage`.

    [[nodiscard]] int width() const noexcept { return bounds.x2 - bounds.x1; }
    [[nodiscard]] int height() const noexcept { return bounds.y2 - bounds.y1; }
    [[nodiscard]] float* data() noexcept { return storage.data() + bottomOffsetFloats; }
    [[nodiscard]] const float* data() const noexcept { return storage.data() + bottomOffsetFloats; }
    [[nodiscard]] float* pixel(int x, int y) noexcept;
    [[nodiscard]] const float* pixel(int x, int y) const noexcept;

    /// Fill an image property set as a host would (data, bounds, pitch,
    /// depth, components, pixel aspect, field).
    void describe(PropertySet& image) noexcept;
    /// Every float of every pixel set to `value`.
    void fill(float value) noexcept;
};

[[nodiscard]] HostImage makeImage(const OfxRectI& bounds, bool negativePitch = false, int padFloats = 0);

/// A smooth, asymmetric test panorama (no two directions share a colour, no
/// symmetry that could hide a flip) painted into an OpenFX-layout image.
void paintPanorama(HostImage& image);

/// The Premiere effect's own CPU render of `settings` from `source` (read as
/// the host image it is), as an OpenFX-layout image of `frame`: what Open 360
/// Reframe in Premiere shows for the same picture and controls.
[[nodiscard]] HostImage referenceRender(const reframe::Settings& settings, const HostImage& source,
                                        const OfxRectI& frame, reframe::SizePx project);

/// Largest absolute difference over `window` of two images (all four
/// channels); +infinity when a pixel is missing from either.
[[nodiscard]] double maxDifference(const HostImage& a, const HostImage& b, const OfxRectI& window);

/// PSNR of `a` against `b` over `window` (peak 1.0).
[[nodiscard]] double psnr(const HostImage& a, const HostImage& b, const OfxRectI& window);

/// The Settings a FRESH instance renders: every control at its default,
/// exactly as the Premiere effect reads a fresh instance (Lens: DJI).
[[nodiscard]] reframe::Settings defaultSettings();

/// Hand `image` to `clip` for every time.
void provideImage(Clip& clip, HostImage& image);

}  // namespace osv::ofxtest
