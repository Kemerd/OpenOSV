// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxTestSupport.cpp - see OfxTestSupport.h.

#include "OfxTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef OSV_OFX_MODULE_PATH
#error "OSV_OFX_MODULE_PATH must name the built OpenOSV.ofx (tests/ofx/CMakeLists.txt)"
#endif

namespace osv::ofxtest {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

// ===========================================================================
//  Fixture
// ===========================================================================

Fixture::Fixture()
    : module(OSV_OFX_MODULE_PATH),
      reframe(module.plugin("org.openosv.Open360Reframe")),
      source(module.plugin("org.openosv.OSVSource")) {
    if (!module.ok()) {
        return;
    }
    ready = reframe.load() == kOfxStatOK && reframe.describe() == kOfxStatOK && source.load() == kOfxStatOK &&
            source.describe() == kOfxStatOK;
}

Fixture& Fixture::get() {
    static Fixture fixture;
    return fixture;
}

// ===========================================================================
//  HostImage
// ===========================================================================

float* HostImage::pixel(int x, int y) noexcept {
    if (x < bounds.x1 || x >= bounds.x2 || y < bounds.y1 || y >= bounds.y2) {
        return nullptr;
    }
    char* row = reinterpret_cast<char*>(data()) + static_cast<std::ptrdiff_t>(y - bounds.y1) * rowBytes;
    return reinterpret_cast<float*>(row) + static_cast<std::ptrdiff_t>(x - bounds.x1) * 4;
}

const float* HostImage::pixel(int x, int y) const noexcept {
    return const_cast<HostImage*>(this)->pixel(x, y);
}

void HostImage::describe(PropertySet& image) noexcept {
    image.setPointer(kOfxImagePropData, data());
    image.setInts(kOfxImagePropBounds, {bounds.x1, bounds.y1, bounds.x2, bounds.y2});
    image.setInts(kOfxImagePropRegionOfDefinition, {bounds.x1, bounds.y1, bounds.x2, bounds.y2});
    image.setInt(kOfxImagePropRowBytes, rowBytes);
    image.setString(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
    image.setString(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
    image.setString(kOfxImageEffectPropPreMultiplication, kOfxImageUnPreMultiplied);
    image.setDouble(kOfxImagePropPixelAspectRatio, 1.0);
    image.setString(kOfxImagePropField, kOfxImageFieldNone);
    image.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
}

void HostImage::fill(float value) noexcept { std::fill(storage.begin(), storage.end(), value); }

HostImage makeImage(const OfxRectI& bounds, bool negativePitch, int padFloats) {
    HostImage img;
    img.bounds = bounds;
    const int w = std::max(0, bounds.x2 - bounds.x1);
    const int h = std::max(0, bounds.y2 - bounds.y1);
    const int rowFloats = w * 4 + std::max(0, padFloats);
    img.storage.assign(static_cast<std::size_t>(rowFloats) * static_cast<std::size_t>(h), 0.0f);
    if (negativePitch) {
        // Top row first in memory: the bottom row is the LAST one, and the
        // pitch walks backwards from it.
        img.rowBytes = -rowFloats * 4;
        img.bottomOffsetFloats = static_cast<std::size_t>(rowFloats) * static_cast<std::size_t>(h > 0 ? h - 1 : 0);
    } else {
        img.rowBytes = rowFloats * 4;
        img.bottomOffsetFloats = 0;
    }
    return img;
}

void paintPanorama(HostImage& image) {
    const int w = image.width();
    const int h = image.height();
    for (int y = image.bounds.y1; y < image.bounds.y2; ++y) {
        // Image row counted from the TOP (the equirect's own convention):
        // row 0 is latitude +90.
        const int rowFromTop = image.bounds.y2 - 1 - y;
        const double lat = kPi / 2.0 - (rowFromTop + 0.5) / h * kPi;
        for (int x = image.bounds.x1; x < image.bounds.x2; ++x) {
            const double lon = (x - image.bounds.x1 + 0.5) / w * 2.0 * kPi - kPi;
            float* p = image.pixel(x, y);
            // Three independent smooth fields: no mirror or rotation of the
            // sphere maps the picture onto itself.
            p[0] = static_cast<float>(0.5 + 0.45 * std::sin(lon + 0.3) * std::cos(lat));
            p[1] = static_cast<float>(0.5 + 0.45 * std::sin(lat * 1.3 + 0.2));
            p[2] = static_cast<float>(0.5 + 0.35 * std::cos(2.0 * lon - 0.7) * std::cos(lat) + 0.1 * std::sin(lat));
            p[3] = 1.0f;
        }
    }
}

// ===========================================================================
//  The Premiere effect's render, as the reference
// ===========================================================================

HostImage referenceRender(const reframe::Settings& settings, const HostImage& source, const OfxRectI& frame,
                          reframe::SizePx project) {
    // The source as Premiere hands it to Open 360 Reframe: top-left origin,
    // positive pitch, B G R A.
    const int sw = source.width();
    const int sh = source.height();
    std::vector<float> bgra(static_cast<std::size_t>(sw) * sh * 4);
    for (int r = 0; r < sh; ++r) {
        const int y = source.bounds.y2 - 1 - r;  // row r from the top
        for (int x = 0; x < sw; ++x) {
            const float* p = source.pixel(source.bounds.x1 + x, y);
            float* q = bgra.data() + (static_cast<std::size_t>(r) * sw + x) * 4;
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
            q[3] = p[3];
        }
    }
    reframe::ConstFrameView src;
    src.base = bgra.data();
    src.rowBytes = sw * 16;
    src.width = sw;
    src.height = sh;
    src.layout = reframe::PixelLayout::Bgra32f;
    src.topDown = true;

    const int fw = frame.x2 - frame.x1;
    const int fh = frame.y2 - frame.y1;
    std::vector<float> out(static_cast<std::size_t>(fw) * fh * 4, -1.0f);
    reframe::FrameView dst;
    dst.base = out.data();
    dst.rowBytes = fw * 16;
    dst.width = fw;
    dst.height = fh;
    dst.layout = reframe::PixelLayout::Bgra32f;
    dst.topDown = true;

    const reframe::KernelSetup setup = reframe::buildParams(settings, src, fw, fh, project);
    HostImage result = makeImage(frame);
    result.fill(std::numeric_limits<float>::quiet_NaN());
    if (!setup.valid || !reframe::renderCpu(setup, src, dst, nullptr)) {
        return result;  // all NaN: every comparison fails loudly
    }
    for (int r = 0; r < fh; ++r) {
        const int y = frame.y2 - 1 - r;
        for (int x = 0; x < fw; ++x) {
            const float* q = out.data() + (static_cast<std::size_t>(r) * fw + x) * 4;
            float* p = result.pixel(frame.x1 + x, y);
            p[0] = q[2];
            p[1] = q[1];
            p[2] = q[0];
            p[3] = q[3];
        }
    }
    return result;
}

double maxDifference(const HostImage& a, const HostImage& b, const OfxRectI& window) {
    double worst = 0.0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* p = a.pixel(x, y);
            const float* q = b.pixel(x, y);
            if (!p || !q) {
                return std::numeric_limits<double>::infinity();
            }
            for (int c = 0; c < 4; ++c) {
                const double d = std::fabs(static_cast<double>(p[c]) - static_cast<double>(q[c]));
                if (!(d == d)) {
                    return std::numeric_limits<double>::infinity();
                }
                worst = std::max(worst, d);
            }
        }
    }
    return worst;
}

double psnr(const HostImage& a, const HostImage& b, const OfxRectI& window) {
    double sum = 0.0;
    std::size_t n = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* p = a.pixel(x, y);
            const float* q = b.pixel(x, y);
            if (!p || !q) {
                return -1.0;
            }
            for (int c = 0; c < 4; ++c) {
                const double d = (std::isfinite(p[c]) && std::isfinite(q[c])) ? static_cast<double>(p[c]) - q[c] : 1.0;
                sum += d * d;
                ++n;
            }
        }
    }
    if (n == 0) {
        return -1.0;
    }
    const double mse = sum / static_cast<double>(n);
    return mse <= 0.0 ? std::numeric_limits<double>::infinity() : 10.0 * std::log10(1.0 / mse);
}

reframe::Settings defaultSettings() {
    reframe::Settings s;
    s.resolution = reframe::sanitiseResolution(OSV_REFRAME_RESOLUTION_DEFAULT);
    s.preset = reframe::sanitisePreset(OSV_REFRAME_PRESET_DEFAULT);
    s.cameraModel = reframe::kDefaultCameraModel;
    s.fovDeg = OSV_REFRAME_FOV_DEFAULT;
    s.distortion = OSV_REFRAME_DISTORTION_DEFAULT;
    s.zoomDeg = OSV_REFRAME_ZOOM_DEFAULT;
    s.djiFovDeg = OSV_REFRAME_DJI_FOV_DEFAULT;
    s.correction = OSV_REFRAME_CORRECTION_DEFAULT;
    s.easing = reframe::KeyframeEasing::None;
    s.smoothKeyframes = false;
    return s;
}

bool cudaDriverLoadable() {
#if defined(_WIN32)
    // From System32 only, where the driver installs it.  Kept loaded: the
    // delay-load stubs then bind to this very module.
    return ::LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
#else
    return false;
#endif
}

void provideImage(Clip& clip, HostImage& image) {
    clip.provide = [&image](double, PropertySet& props) {
        image.describe(props);
        return true;
    };
}

}  // namespace osv::ofxtest
